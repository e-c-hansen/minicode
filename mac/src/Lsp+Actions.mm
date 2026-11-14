// Lsp+Actions.mm — Objective-C++. LspSession (Actions): rename (with
// prepareRename and its field), references and the quick fix list (one
// picker by the caret), code actions asked of every server and merged,
// running one (its edit, then its command), and Organize Imports, also
// before a save. Edits go in through the applier (Lsp+WorkspaceEdit.mm).
#import "Lsp+Private.h"
#import "Lsp+ActionsIvars.h"

@implementation LspSession (Actions)

// The lists and the rename field make way for one another.
- (void)closeTransients {
    [self closeCompletion];
    [self closeSignature];
    [self closeHover];
    [self closePicker];
    [_rename close];
}

// The document left the view: answers still on their way were for it, and
// the rename field goes, unless it is the edit going in that left it (an
// edit to another file shows that file).
- (void)forgetActionRequests {
    if (!_applyingEdit) [_rename close];
    _referencesGen++;
    _actionGen++;
    if (!_applyingEdit) _renameGen++;
}

// The quick fix whose command is running, which names the edits the
// server makes meanwhile (workspace/applyEdit), or nil.
- (NSString *)runningCommandTitle { return _commandTitle; }

// ---- rename
- (void)renameSymbol {
    MCLspServer *server = [self serverFor:Lsp::Feature::Rename missing:@"rename"];
    if (!server) return;
    _renameServer = server;
    [self closeTransients];
    [self flushChanges];
    int gen = ++_renameGen;
    _askedSelection = _tv.selectedRange;
    NSUInteger index = _tv.selectedRange.location;
    NSString *uri = _uri;
    if (!server.client.canPrepareRename()) {
        [self openRenameAt:[self nameRangeAt:index] gen:gen placeholder:nil];
        return;
    }
    __weak LspSession *weakSelf = self;
    Lsp::Position pos = Lsp::positionForOffset(U16Of(_tv.string), index);
    server.client.prepareRename(Std(uri), pos, [weakSelf, gen, uri, index](const Json &r,
                                                                        const Json &e) {
        [weakSelf prepareRenameArrived:r error:e gen:gen uri:uri index:index];
    });
}

// The identifier at or just before a character, or an empty range.
- (NSRange)nameRangeAt:(NSUInteger)index {
    NSString *t = _tv.string;
    if (index < t.length && IsIdentChar([t characterAtIndex:index])) return [self wordAt:index];
    if (index > 0 && index <= t.length && IsIdentChar([t characterAtIndex:index - 1]))
        return [self wordAt:index - 1];
    return NSMakeRange(index, 0);
}

- (void)prepareRenameArrived:(const Json &)result error:(const Json &)error gen:(int)gen
                         uri:(NSString *)uri index:(NSUInteger)index {
    if (gen != _renameGen || ![uri isEqualToString:_uri] ||
        !NSEqualRanges(_tv.selectedRange, _askedSelection))
        return;
    Lsp::PrepareRename p = Lsp::parsePrepareRename(result);
    if (!p.ok) {
        // Null for a keyword, a literal, or a name from a library.
        NSBeep();
        [self note:ErrorText(error) ?: @"This can't be renamed"];
        return;
    }
    NSRange range = [self nameRangeAt:index];
    if (p.hasRange) {
        std::u16string text = U16Of(_tv.string);
        size_t a = Lsp::offsetForPosition(text, p.range.start);
        size_t b = Lsp::offsetForPosition(text, p.range.end);
        if (b > a) range = NSMakeRange(a, b - a);
    }
    [self openRenameAt:range gen:gen placeholder:p.placeholder.empty() ? nil : NS(p.placeholder)];
}

- (void)openRenameAt:(NSRange)range gen:(int)gen placeholder:(NSString *)placeholder {
    if (range.length == 0 || NSMaxRange(range) > _tv.string.length) {
        NSBeep();
        [self note:@"No name at the cursor to rename"];
        return;
    }
    if (!_tv.window) return;
    _renameRange = range;
    _renameText = [_tv.string copy];
    __block NSRect rect = NSZeroRect;
    [_tv enumerateRectsForRange:range block:^(NSRect r) {
        if (NSIsEmptyRect(rect)) rect = r;
    }];
    if (NSIsEmptyRect(rect)) return;
    if (!_rename) {
        _rename = [MCRenameBox new];
        __weak LspSession *weakSelf = self;
        _rename.onClose = ^{
            LspSession *s = weakSelf;
            CodeTextView *tv = s ? s->_tv : nil;
            if (tv.window && tv.window.firstResponder != tv) [tv.window makeFirstResponder:tv];
        };
    }
    __weak LspSession *weakSelf = self;
    _rename.onCommit = ^(NSString *name) { [weakSelf commitRename:name gen:gen]; };
    [_rename showWithName:placeholder ?: [_tv.string substringWithRange:range]
           relativeToRect:rect ofView:_tv];
}

- (void)commitRename:(NSString *)name gen:(int)gen {
    MCLspServer *server = _renameServer;
    if (gen != _renameGen || ![_servers containsObject:server] || !_uri ||
        ![_tv.string isEqualToString:_renameText])
        return;
    NSString *old = [_tv.string substringWithRange:_renameRange];
    if ([name isEqualToString:old]) return;
    // Every tab's text as it is, so uses in unsaved tabs are renamed too.
    [self flushChanges];
    [self syncOpenDocuments];
    __weak LspSession *weakSelf = self;
    NSString *uri = _uri;
    Lsp::Position pos = Lsp::positionForOffset(U16Of(_tv.string), _renameRange.location);
    // What every open file's text was when asked, to refuse a reply for a
    // file that changed meanwhile (applyWorkspaceEdit:server:asked:).
    auto asked = std::make_shared<const Lsp::DocVersions>(server.client.versions());
    server.client.rename(Std(uri), pos, Std(name), [weakSelf, gen, uri, server,
                                                    asked](const Json &r, const Json &e) {
        Json result = r, error = e;
        // Applied once the reply has been read: applying switches tabs,
        // which talks to the server.
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf renameArrived:result error:error gen:gen uri:uri server:server
                              asked:asked];
        });
    });
}

- (void)renameArrived:(const Json &)result error:(const Json &)error gen:(int)gen
                  uri:(NSString *)uri server:(MCLspServer *)server
                asked:(std::shared_ptr<const Lsp::DocVersions>)asked {
    // Only onto the text the name was read from.
    if (gen != _renameGen || ![uri isEqualToString:_uri] ||
        ![_tv.string isEqualToString:_renameText])
        return;
    if (NSString *why = ErrorText(error)) { NSBeep(); [self note:why]; return; }
    Lsp::WorkspaceEdit we = Lsp::parseWorkspaceEdit(result, CanonicalURI);
    if (we.editCount() == 0 && we.problem.empty()) {
        NSBeep();
        [self note:@"Nothing to rename"];
        return;
    }
    NSString *failure = nil;
    NSInteger files = [self applyWorkspaceEdit:we server:server asked:asked.get()
                                    actionName:@"Rename" failure:&failure];
    if (files < 0) { NSBeep(); [self note:failure]; return; }
    size_t n = we.editCount();
    [self note:[NSString stringWithFormat:@"Renamed %zu occurrence%@ in %ld file%@", n,
                                          n == 1 ? @"" : @"s", (long)files,
                                          files == 1 ? @"" : @"s"]];
}

// ---- references
- (void)findReferences {
    MCLspServer *server = [self serverFor:Lsp::Feature::References
                                  missing:@"find references"];
    if (!server) return;
    [self closeTransients];
    [self flushChanges];
    [self syncOpenDocuments];
    int gen = ++_referencesGen;
    _askedSelection = _tv.selectedRange;
    __weak LspSession *weakSelf = self;
    NSString *uri = _uri;
    Lsp::Position pos = Lsp::positionForOffset(U16Of(_tv.string), _tv.selectedRange.location);
    server.client.references(Std(uri), pos, true, [weakSelf, gen, uri](const Json &r,
                                                                    const Json &e) {
        [weakSelf referencesArrived:r error:e gen:gen uri:uri];
    });
}

static NSFont *PickerMono(void) {
    return MCMonoFont(12, NSFontWeightRegular);
}

- (void)referencesArrived:(const Json &)result error:(const Json &)error gen:(int)gen
                      uri:(NSString *)uri {
    if (gen != _referencesGen || ![uri isEqualToString:_uri] ||
        !NSEqualRanges(_tv.selectedRange, _askedSelection))
        return;
    if (NSString *why = ErrorText(error)) { NSBeep(); [self note:why]; return; }
    std::vector<Lsp::ReferenceFile> files =
        Lsp::groupReferences(Lsp::parseLocations(result), Std(_uri), CanonicalURI);
    if (files.empty()) { NSBeep(); [self note:@"No references found"]; return; }

    NSMutableArray<NSAttributedString *> *rows = [NSMutableArray array];
    NSMutableIndexSet *headers = [NSMutableIndexSet indexSet];
    std::vector<Lsp::Location> where;
    NSInteger select = -1;
    NSUInteger caret = _tv.selectedRange.location;
    NSDictionary *muted = @{NSFontAttributeName: PickerMono(),
                            NSForegroundColorAttributeName: LHex(0x9CA3AF)};
    NSDictionary *plain = @{NSFontAttributeName: PickerMono(),
                            NSForegroundColorAttributeName: LHex(0xD4D4D4)};
    NSString *rootReal = RealPath(_root);
    size_t total = 0;
    for (const Lsp::ReferenceFile &f : files) {
        NSString *path = [self treePathForURI:f.uri] ?: NS(f.uri);
        NSString *text = [self textOfPath:path uri:f.uri] ?: @"";
        std::u16string u = U16Of(text);
        const Lsp::PositionIndex index(u);   // one pass, then a search per reference
        BOOL current = [self uri:f.uri isSameFileAs:_uri];
        // The heading: the file's name, its folder within the project, and
        // how many references it holds.
        NSString *real = RealPath(path), *folder = real.stringByDeletingLastPathComponent;
        if (rootReal.length && [folder hasPrefix:rootReal])
            folder = [folder substringFromIndex:rootReal.length];
        if ([folder hasPrefix:@"/"] && rootReal.length) folder = [folder substringFromIndex:1];
        NSMutableAttributedString *h = [[NSMutableAttributedString alloc]
            initWithString:[NSString stringWithFormat:@" %@", path.lastPathComponent]
                attributes:@{NSFontAttributeName: [NSFont boldSystemFontOfSize:12],
                             NSForegroundColorAttributeName: LHex(0xE5E5E5)}];
        NSString *tail = [NSString stringWithFormat:@"  %@%@%zu", folder,
                                                    folder.length ? @"  " : @"",
                                                    f.ranges.size()];
        [h appendAttributedString:[[NSAttributedString alloc]
            initWithString:tail attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11],
                                             NSForegroundColorAttributeName: LHex(0x9CA3AF)}]];
        [headers addIndex:rows.count];
        [rows addObject:h];
        where.push_back(Lsp::Location());
        for (const Lsp::Range &r : f.ranges) {
            std::u16string lu = Lsp::lineOf(index, r.start.line);
            if (lu.size() > 400) lu.resize(400);   // a minified line
            NSString *line = [NSString stringWithCharacters:(const unichar *)lu.data()
                                                     length:lu.size()];
            // Leading space goes; the match is highlighted where it is.
            NSUInteger indent = 0;
            while (indent < line.length &&
                   [[NSCharacterSet whitespaceCharacterSet]
                       characterIsMember:[line characterAtIndex:indent]])
                indent++;
            NSUInteger len = line.length;
            NSUInteger a = MIN((NSUInteger)MAX(r.start.character, 0), len);
            NSUInteger b = r.end.line == r.start.line ? MIN((NSUInteger)MAX(r.end.character, 0), len)
                                                      : len;
            NSString *shown = [line substringFromIndex:indent];
            NSMutableAttributedString *row = [[NSMutableAttributedString alloc]
                initWithString:[NSString stringWithFormat:@"  %5d  ", r.start.line + 1]
                    attributes:muted];
            NSUInteger start = row.length;
            [row appendAttributedString:[[NSAttributedString alloc] initWithString:shown
                                                                        attributes:plain]];
            if (b > a && a >= indent) {
                NSRange hl = NSMakeRange(start + a - indent, b - a);
                if (NSMaxRange(hl) <= row.length)
                    [row addAttributes:@{NSBackgroundColorAttributeName:
                                             [LHex(0x4EA1F7) colorWithAlphaComponent:0.35],
                                         NSForegroundColorAttributeName: LHex(0xFFFFFF)}
                                 range:hl];
            }
            // The reference under the caret starts selected.
            if (current && select < 0) {
                size_t r0 = index.offset(r.start), r1 = index.offset(r.end);
                if (caret >= r0 && caret <= r1) select = (NSInteger)rows.count;
            }
            [rows addObject:row];
            where.push_back(Lsp::Location{f.uri, r});
            total++;
        }
    }
    _refRows = where;
    _pickerKind = @"references";
    [self showPickerRows:rows headers:headers select:select width:620 maxRows:14];
    [self flash:[NSString stringWithFormat:@"%zu reference%@ in %zu file%@", total,
                                           total == 1 ? @"" : @"s", files.size(),
                                           files.size() == 1 ? @"" : @"s"]];
}

- (void)showPickerRows:(NSArray<NSAttributedString *> *)rows headers:(NSIndexSet *)headers
                select:(NSInteger)select width:(CGFloat)width maxRows:(NSInteger)maxRows {
    if (!_picker) {
        _picker = [MCPickerPopup new];
        __weak LspSession *weakSelf = self;
        _picker.onAccept = ^(NSInteger row) { [weakSelf acceptPickerRow:row]; };
    }
    NSUInteger caret = _tv.selectedRange.location;
    NSRect at = [_tv firstRectForCharacterRange:NSMakeRange(caret, 0) actualRange:NULL];
    [_picker showRows:rows headers:headers select:select width:width maxRows:maxRows
         atScreenRect:at parent:_tv.window];
}

- (void)closePicker {
    if (!_picker.visible && !_pickerKind) return;
    [_picker close];
    _pickerKind = nil;
    _refRows.clear();
    _actions.clear();
    _actionServers = nil;
}

- (void)acceptPickerRow:(NSInteger)row {
    if (!_picker.visible || row < 0) return;
    if ([_pickerKind isEqualToString:@"references"] && (size_t)row < _refRows.size() &&
        !_refRows[(size_t)row].uri.empty()) {
        Lsp::Location loc = _refRows[(size_t)row];
        [self closePicker];
        [self revealLocation:loc];
    } else if ([_pickerKind isEqualToString:@"fixes"] && (size_t)row < _actions.size()) {
        Lsp::CodeAction action = _actions[(size_t)row];
        MCLspServer *server = action.origin >= 0 && (NSUInteger)action.origin < _actionServers.count
                                  ? _actionServers[(NSUInteger)action.origin] : nil;
        [self closePicker];
        [self runCodeAction:action server:server];
    }
}

// The references or quick fix list's keys (handleCommand:): the arrows
// move through it, Return takes a row, Esc closes it. Any other key goes
// to the editor, and the list closes when the text or the caret changes.
- (BOOL)pickerTakesCommand:(SEL)sel {
    if (!_picker.visible) return NO;
    if (sel == @selector(moveUp:)) { [_picker moveSelection:-1]; return YES; }
    if (sel == @selector(moveDown:)) { [_picker moveSelection:1]; return YES; }
    if (sel == @selector(scrollPageUp:) || sel == @selector(pageUp:)) {
        [_picker moveSelection:-10];
        return YES;
    }
    if (sel == @selector(scrollPageDown:) || sel == @selector(pageDown:)) {
        [_picker moveSelection:10];
        return YES;
    }
    if (sel == @selector(insertNewline:)) {
        [self acceptPickerRow:_picker.selectedRow];
        return YES;
    }
    if (sel == @selector(cancelOperation:)) { [self closePicker]; return YES; }
    return NO;
}

- (NSArray<NSString *> *)pickerRows {
    NSMutableArray *out = [NSMutableArray array];
    if (_picker.visible)
        for (NSAttributedString *r in _picker.rows) [out addObject:r.string];
    return out;
}
- (NSString *)pickerKind { return _picker.visible ? _pickerKind : nil; }
- (NSInteger)pickerSelectedRow { return _picker.visible ? _picker.selectedRow : -1; }
- (NSView *)pickerView { return _picker.visible ? _picker.contentView : nil; }
- (NSTextField *)renameField { return _rename.visible ? _rename.field : nil; }
- (NSView *)renameView { return _rename.visible ? _rename.contentView : nil; }

// ---- quick fixes and Organize Imports
// Ask every one of the file's servers that offers code actions for those at
// `range` (each with its own diagnostics there, which it matches its fixes
// on), and hand on the merged list (Lsp::mergeCodeActions) once all have
// answered, or after a few seconds with what has. `done` is not called
// when the file, the selection or a newer request has overtaken it.
- (void)askCodeActionsInRange:(Lsp::Range)range
                         only:(const std::vector<std::string> &)only
                         done:(void (^)(std::vector<Lsp::CodeAction> actions,
                                        NSArray<MCLspServer *> *servers,
                                        NSString *error))done {
    NSArray<MCLspServer *> *servers = [self serversFor:Lsp::Feature::CodeActions];
    int gen = ++_actionGen;
    NSString *uri = _uri;
    NSRange sel = _tv.selectedRange;
    const NSUInteger edits = _edits;
    _askedSelection = sel;
    // Each server's open documents' versions as asked: the actions' edits
    // are for those texts (performCodeAction refuses newer ones).
    auto versions = std::make_shared<std::vector<Lsp::DocVersions>>();
    for (MCLspServer *server in servers) versions->push_back(server.client.versions());
    struct Pending {
        std::vector<std::vector<Lsp::CodeAction>> lists;
        size_t waiting = 0;
        bool finished = false;
        std::string error;
    };
    auto pending = std::make_shared<Pending>();
    pending->lists.resize(servers.count);
    pending->waiting = servers.count;
    __weak LspSession *weakSelf = self;
    void (^finish)(void) = ^{
        LspSession *me = weakSelf;
        if (!me || pending->finished) return;
        pending->finished = true;
        // The text changed (a reload keeps the caret, so the selection alone
        // did not tell): the actions are for the text before.
        if (gen != me->_actionGen || ![uri isEqualToString:me->_uri] ||
            !NSEqualRanges(me->_tv.selectedRange, sel) || me->_edits != edits)
            return;
        std::vector<Lsp::CodeAction> merged = Lsp::mergeCodeActions(pending->lists);
        for (Lsp::CodeAction &a : merged)
            if (a.origin >= 0 && (size_t)a.origin < versions->size())
                a.asked = std::make_shared<const Lsp::DocVersions>((*versions)[(size_t)a.origin]);
        done(merged, servers, merged.empty() && !pending->error.empty()
                                  ? NS(pending->error) : nil);
    };
    for (NSUInteger i = 0; i < servers.count; i++) {
        MCLspServer *server = servers[i];
        const Lsp::DiagnosticStore::Set *set = _store.find(Std(uri), Std(server.serverId));
        Json diags = set ? Lsp::diagnosticsInRange(set->list, range) : Json::array();
        server.client.codeActions(Std(uri), range, diags,
            [pending, i, server, finish](const Json &r, const Json &e) {
                if (pending->finished) return;
                if (NSString *why = ErrorText(e)) pending->error = Std(why);
                else
                    pending->lists[i] =
                        Lsp::parseCodeActions(r, server.client.canResolveCodeAction());
                if (--pending->waiting == 0) dispatch_async(dispatch_get_main_queue(), finish);
            }, only);
    }
    // A server that never answers holds up the others' for 3 s at most.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC),
                   dispatch_get_main_queue(), finish);
}

- (void)showQuickFixes {
    if (![self serverFor:Lsp::Feature::CodeActions missing:@"offer quick fixes"]) return;
    [self closeTransients];
    [self flushChanges];
    NSRange sel = _tv.selectedRange;
    std::u16string text = U16Of(_tv.string);
    Lsp::Range range{Lsp::positionForOffset(text, sel.location),
                     Lsp::positionForOffset(text, NSMaxRange(sel))};
    // A caret on a line with a problem that is not under it (clang reports
    // a missing ';' at the next token): the line's problems, at the first.
    std::vector<Lsp::Diagnostic> all = _store.merged(Std(_uri));
    if (sel.length == 0 && Lsp::diagnosticsInRange(all, range).size() == 0) {
        Lsp::Range line{{range.start.line, 0}, {range.start.line, INT_MAX}};
        Json onLine = Lsp::diagnosticsInRange(all, line);
        if (onLine.size() > 0) range = Lsp::parseRange(onLine[0]["range"]);
    }
    __weak LspSession *weakSelf = self;
    [self askCodeActionsInRange:range only:{}
                           done:^(std::vector<Lsp::CodeAction> actions,
                                  NSArray<MCLspServer *> *servers, NSString *error) {
        [weakSelf codeActionsArrived:actions servers:servers error:error];
    }];
}

// Shift+Option+O: the servers' source.organizeImports action for the whole
// file, applied at once (Ruff's own before a general server's, by
// Lsp::pickOrganizeImports), through the same all-or-nothing edit as a
// quick fix.
- (void)organizeImports {
    if (![self serverFor:Lsp::Feature::CodeActions missing:@"organize imports"]) return;
    [self closeTransients];
    [self flushChanges];
    std::u16string text = U16Of(_tv.string);
    Lsp::Range range{{0, 0}, Lsp::positionForOffset(text, text.size())};
    __weak LspSession *weakSelf = self;
    NSString *uri = _uri;
    [self askCodeActionsInRange:range only:{"source.organizeImports"}
                           done:^(std::vector<Lsp::CodeAction> actions,
                                  NSArray<MCLspServer *> *servers, NSString *error) {
        LspSession *me = weakSelf;
        if (!me) return;
        int i = Lsp::pickOrganizeImports(actions);
        if (i < 0) {
            NSBeep();
            [me note:error ?: @"The language servers have no way to organize these imports"];
            return;
        }
        Lsp::CodeAction action = actions[(size_t)i];
        MCLspServer *server = action.origin >= 0 && (NSUInteger)action.origin < servers.count
                                  ? servers[(NSUInteger)action.origin] : nil;
        NSString *before = [me->_tv.string copy];
        [me runCodeAction:action server:server then:^{
            LspSession *s = weakSelf;
            if (!s || ![uri isEqualToString:s->_uri]) return;
            if ([s->_tv.string isEqualToString:before]) [s note:@"The imports are in order"];
            else [s note:[NSString stringWithFormat:@"Organized imports (%@)", server.name]];
        }];
    }];
}

- (BOOL)organizeImportsBeforeSave:(void (^)(void))done {
    if (!_uri || ![self serversFor:Lsp::Feature::CodeActions].count) return NO;
    [self closeTransients];
    [self flushChanges];
    // Called once, whichever comes first: the edit, nothing to do, or the
    // deadline (askCodeActions gives a silent server 3 s; a resolve or a
    // command may take longer still).
    __block BOOL called = NO;
    void (^once)(void) = ^{
        if (called) return;
        called = YES;
        done();
    };
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC),
                   dispatch_get_main_queue(), once);
    std::u16string text = U16Of(_tv.string);
    Lsp::Range range{{0, 0}, Lsp::positionForOffset(text, text.size())};
    __weak LspSession *weakSelf = self;
    [self askCodeActionsInRange:range only:{"source.organizeImports"}
                           done:^(std::vector<Lsp::CodeAction> actions,
                                  NSArray<MCLspServer *> *servers, NSString *error) {
        LspSession *me = weakSelf;
        int i = Lsp::pickOrganizeImports(actions);
        if (!me || called || i < 0) {
            if (me && error && !called) [me note:error];
            once();
            return;
        }
        Lsp::CodeAction action = actions[(size_t)i];
        MCLspServer *server = action.origin >= 0 && (NSUInteger)action.origin < servers.count
                                  ? servers[(NSUInteger)action.origin] : nil;
        NSString *before = [me->_tv.string copy];
        [me runCodeAction:action server:server then:^{
            LspSession *s = weakSelf;
            if (s && !called && ![s->_tv.string isEqualToString:before])
                [s note:[NSString stringWithFormat:@"Organized imports (%@)", server.name]];
            once();
        }];
    }];
    return YES;
}

static NSString *ActionKindName(const std::string &kind) {
    if (kind.empty()) return @"";
    if (kind.compare(0, 8, "quickfix") == 0) return @"fix";
    if (kind.compare(0, 22, "source.organizeImports") == 0) return @"imports";
    if (kind.compare(0, 6, "source") == 0) return @"source";
    if (kind.compare(0, 8, "refactor") == 0) return @"refactor";
    return NS(kind);
}

- (void)codeActionsArrived:(const std::vector<Lsp::CodeAction> &)actions
                   servers:(NSArray<MCLspServer *> *)servers error:(NSString *)error {
    if (error) { NSBeep(); [self note:error]; return; }
    if (actions.empty()) { [self note:@"No quick fixes"]; return; }
    NSMutableArray<NSAttributedString *> *rows = [NSMutableArray array];
    for (const Lsp::CodeAction &a : actions) {
        NSMutableAttributedString *row = [[NSMutableAttributedString alloc]
            initWithString:[NSString stringWithFormat:@" %@", NS(a.title)]
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:12.5],
                             NSForegroundColorAttributeName: LHex(0xD4D4D4)}];
        NSString *kind = ActionKindName(a.kind);
        if (a.preferred) kind = kind.length ? [kind stringByAppendingString:@", preferred"]
                                            : @"preferred";
        // Which server offers it, when the file has more than one.
        if (servers.count > 1 && a.origin >= 0 && (NSUInteger)a.origin < servers.count)
            kind = kind.length ? [NSString stringWithFormat:@"%@, %@", kind,
                                           servers[(NSUInteger)a.origin].name]
                               : servers[(NSUInteger)a.origin].name;
        if (kind.length)
            [row appendAttributedString:[[NSAttributedString alloc]
                initWithString:[@"   " stringByAppendingString:kind]
                    attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11],
                                 NSForegroundColorAttributeName: LHex(0x9CA3AF)}]];
        [rows addObject:row];
    }
    _actions = actions;
    _actionServers = servers;
    _pickerKind = @"fixes";
    [self showPickerRows:rows headers:[NSIndexSet indexSet] select:0 width:440 maxRows:10];
}

// A chosen action: resolved first when it came without its edit and the
// server can fill it in, then its edit applied and its command run.
- (void)runCodeAction:(Lsp::CodeAction)action server:(MCLspServer *)server {
    [self runCodeAction:action server:server then:nil];
}

// `then` runs once the action's edit has gone in (or failed), when it has
// one; a command's edits come later, through workspace/applyEdit.
- (void)runCodeAction:(Lsp::CodeAction)action server:(MCLspServer *)server
                 then:(void (^)(void))then {
    if (!server || server.stopping) return;
    __weak LspSession *weakSelf = self;
    if (!action.hasEdit() && server.client.canResolveCodeAction() && action.raw) {
        server.client.resolveCodeAction(action, [weakSelf, action, server, then](const Json &r,
                                                                            const Json &e) {
            Lsp::CodeAction done = action;
            if (e.isNull()) {
                Lsp::CodeAction resolved = Lsp::parseCodeAction(r);
                if (resolved.hasEdit()) done.edit = resolved.edit;
                if (!done.hasCommand() && resolved.hasCommand()) done.command = resolved.command;
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                [weakSelf performCodeAction:done server:server then:then];
            });
        });
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        [weakSelf performCodeAction:action server:server then:then];
    });
}

- (void)performCodeAction:(const Lsp::CodeAction &)action server:(MCLspServer *)server
                     then:(void (^)(void))then {
    if (action.hasEdit()) {
        NSString *failure = nil;
        NSInteger n = [self applyWorkspaceEdit:Lsp::parseWorkspaceEdit(action.edit, CanonicalURI)
                                        server:server
                                         asked:action.asked.get()
                                    actionName:NS(action.title)
                                       failure:&failure];
        if (n < 0) { NSBeep(); [self note:failure]; return; }
        if (then && !action.hasCommand()) then();
    }
    if (action.hasCommand() && server && !server.stopping) {
        // The server makes its own edits with workspace/applyEdit while it
        // runs the command (onApplyEdit).
        // Its edits are undone under the fix's title.
        __weak LspSession *weakSelf = self;
        NSString *title = NS(action.title);
        _commandTitle = title;
        server.client.executeCommand(action.command, [weakSelf, title, then](const Json &r,
                                                                             const Json &e) {
            (void)r;
            NSString *why = ErrorText(e);
            dispatch_async(dispatch_get_main_queue(), ^{
                LspSession *me = weakSelf;
                if (!me) return;
                if (me->_commandTitle == title) me->_commandTitle = nil;
                if (why) [me note:why];
                else if (then) then();
            });
        });
    } else if (!action.hasEdit()) {
        [self note:@"The language server gave nothing to apply"];
    }
}
@end
