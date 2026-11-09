// Lsp+Navigation.mm — Objective-C++. LspSession (Navigation): going to a
// definition (and opening any location a server names), hover info for
// the pointer or the caret, and Go to Symbol, from a server or the
// built-in outline.
#import "Lsp+Private.h"
#import "Lsp+NavigationIvars.h"

@implementation MCSymbolItem
@end

// Where each line of a text starts (LSP's lines: \n, \r\n and \r end
// one), for turning many positions into offsets in one pass.
static std::vector<NSUInteger> LineStarts(NSString *text) {
    std::vector<NSUInteger> starts{0};
    const NSUInteger n = text.length;
    std::vector<unichar> buf(n);
    if (n) [text getCharacters:buf.data() range:NSMakeRange(0, n)];
    for (NSUInteger i = 0; i < n; i++) {
        if (buf[i] == '\r') {
            if (i + 1 < n && buf[i + 1] == '\n') i++;
            starts.push_back(i + 1);
        } else if (buf[i] == '\n') {
            starts.push_back(i + 1);
        }
    }
    return starts;
}

static NSUInteger OffsetAt(const std::vector<NSUInteger> &starts, NSString *text,
                           Lsp::Position p) {
    if (p.line < 0) return 0;
    if ((size_t)p.line >= starts.size()) return text.length;
    NSUInteger a = starts[(size_t)p.line];
    NSUInteger b = (size_t)p.line + 1 < starts.size() ? starts[(size_t)p.line + 1] : text.length;
    while (b > a && ([text characterAtIndex:b - 1] == '\n' || [text characterAtIndex:b - 1] == '\r'))
        b--;
    return MIN(a + (NSUInteger)MAX(0, p.character), b);
}

static MCSymbolItem *MCSymbolItemFrom(const Lsp::Symbol &s) {
    MCSymbolItem *it = [MCSymbolItem new];
    it.name = NS(s.name);
    it.detail = s.detail.empty() ? nil : NS(s.detail);
    it.container = s.container.empty() ? nil : NS(s.container);
    it.kind = s.kind;
    it.depth = s.depth;
    it.line = s.selectionRange.start.line;
    it.character = s.selectionRange.start.character;
    it.endLine = s.selectionRange.end.line;
    it.endCharacter = s.selectionRange.end.character;
    it.range = NSMakeRange(NSNotFound, 0);
    return it;
}

// Symbols of `text` as items with their ranges in it, and the one at the caret.
static NSArray<MCSymbolItem *> *MCSymbolItemsFrom(const std::vector<Lsp::Symbol> &syms,
                                                  NSString *text, NSUInteger caret,
                                                  NSInteger *caretIndex) {
    const std::vector<NSUInteger> starts = LineStarts(text);
    NSMutableArray<MCSymbolItem *> *items = [NSMutableArray array];
    for (const Lsp::Symbol &s : syms) {
        MCSymbolItem *it = MCSymbolItemFrom(s);
        NSUInteger a = OffsetAt(starts, text, s.selectionRange.start);
        NSUInteger b = OffsetAt(starts, text, s.selectionRange.end);
        it.range = NSMakeRange(a, b >= a ? b - a : 0);
        [items addObject:it];
    }
    if (caretIndex) {
        caret = MIN(caret, text.length);
        auto at = std::upper_bound(starts.begin(), starts.end(), caret);
        const int line = (int)(at - starts.begin()) - 1;
        *caretIndex = Lsp::symbolAt(syms, Lsp::Position{line, (int)(caret - starts[(size_t)line])});
    }
    return items;
}

NSArray<MCSymbolItem *> *MCOutlineSymbols(NSString *path, NSString *text, NSUInteger caret,
                                          NSInteger *caretIndex) {
    if (caretIndex) *caretIndex = -1;
    if (!path.length || !text) return @[];
    std::vector<Lsp::Symbol> syms = Outline::outline(Std(path.lastPathComponent), U16Of(text));
    return MCSymbolItemsFrom(syms, text, caret, caretIndex);
}

@implementation LspSession (Navigation)

// ---- go to definition
- (void)goToDefinition {
    if (![self serverFor:Lsp::Feature::Definition]) {
        NSBeep();
        [self flash:self.statusText.length ? self.statusText : @"No language server for this file"];
        return;
    }
    [self definitionAtIndex:_tv.selectedRange.location];
}

- (void)definitionAtIndex:(NSUInteger)index {
    MCLspServer *server = [self serverFor:Lsp::Feature::Definition];
    if (!server || !_uri) return;
    [self flushChanges];
    int gen = ++_definitionGen;
    _definitionSelection = _tv.selectedRange;
    __weak LspSession *weakSelf = self;
    NSString *uri = _uri;
    Lsp::Position pos = Lsp::positionForOffset(U16Of(_tv.string), index);
    server.client.definition(Std(uri), pos, [weakSelf, gen, uri](const Json &r, const Json &e) {
        (void)e;
        [weakSelf definitionArrived:r gen:gen uri:uri];
    });
}

- (void)definitionArrived:(const Json &)result gen:(int)gen uri:(NSString *)uri {
    // A late answer jumps only if the user is still where they asked: the
    // same document, never left since (detachDocument bumps the generation,
    // so A, B and back to A drops it), and the selection unmoved.
    if (gen != _definitionGen || ![uri isEqualToString:_uri] ||
        !NSEqualRanges(_tv.selectedRange, _definitionSelection))
        return;
    std::vector<Lsp::Location> locs = Lsp::parseLocations(result);
    if (locs.empty()) { NSBeep(); [self flash:@"No definition found"]; return; }
    [self revealLocation:locs[0]];
}

// A file the server named, under the tree's own spelling of the path when it
// is inside the folder (the server may say /private/tmp for a /tmp folder),
// so the tree can select it. Nil when the URI is not a file.
- (NSString *)treePathForURI:(const std::string &)uri {
    NSString *target = NS(Lsp::pathFromUri(uri));
    if (!target.length) return nil;
    NSString *real = RealPath(target), *rootReal = RealPath(_root);
    if (rootReal.length && [real hasPrefix:[rootReal stringByAppendingString:@"/"]])
        return [_root stringByAppendingPathComponent:[real substringFromIndex:rootReal.length + 1]];
    return target;
}

// Open a location's file (go to definition, a reference) and select its
// range, inside aroundJump when the window has one (for Go Back).
- (void)revealLocation:(const Lsp::Location &)loc {
    if (self.aroundJump) {
        Lsp::Location copy = loc;
        __weak LspSession *weakSelf = self;
        self.aroundJump(^{ [weakSelf revealLocationNow:copy]; });
        return;
    }
    [self revealLocationNow:loc];
}

- (void)revealLocationNow:(const Lsp::Location &)loc {
    if (![self uri:loc.uri isSameFileAs:_uri]) {
        NSString *path = [self treePathForURI:loc.uri];
        BOOL (^open)(NSString *) = self.openFileForNavigation ?: self.openFile;
        if (!path.length || !open || !open(path)) return;
    }
    std::u16string text = U16Of(_tv.string);
    size_t a = Lsp::offsetForPosition(text, loc.range.start);
    size_t b = Lsp::offsetForPosition(text, loc.range.end);
    NSRange r = NSMakeRange(a, b >= a ? b - a : 0);
    [_tv.window makeFirstResponder:_tv];
    [_tv setSelectedRange:r];
    [_tv scrollRangeToVisible:r];
    [_tv showFindIndicatorForRange:r];
}

// ---- hover
// Cmd+I: about the caret, until the caret moves or Esc.
- (void)showHoverInfo {
    if (!_servers.count || !_uri) {
        NSBeep();
        [self flash:self.statusText.length ? self.statusText : @"No language server for this file"];
        return;
    }
    [self requestHoverAt:_tv.selectedRange.location byMouse:NO];
}

// The pointer rests on a character for a moment: ask about it, as VS Code
// does. Moving within what the box is about keeps it; moving off it closes
// it a moment later unless the pointer went into the box.
static const NSTimeInterval kHoverDelay = 0.4, kHoverGrace = 0.3;

- (void)pointerAt:(NSUInteger)index {
    _pointerIndex = index;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(pointerRested)
                                               object:nil];
    if (_hover.visible && _hoverByMouse && index != NSNotFound &&
        NSLocationInRange(index, _hoverRange))
        return;
    if (_hover.visible && _hoverByMouse) [self closeHoverSoon];
    if (index != NSNotFound && [self hoverWantedAt:index])
        [self performSelector:@selector(pointerRested) withObject:nil afterDelay:kHoverDelay];
}

- (BOOL)hoverWantedAt:(NSUInteger)index {
    if (!_tv.editable || index >= _tv.string.length) return NO;
    if (self.completionVisible) return NO;   // the completion list has the stage
    unichar c = [_tv.string characterAtIndex:index];
    if ([[NSCharacterSet whitespaceAndNewlineCharacterSet] characterIsMember:c]) return NO;
    return (_servers.count && _uri) || [_tv diagnosticMessageAtIndex:index] != nil;
}

- (BOOL)pointerInHover {
    return _hover.visible &&
           NSPointInRect([NSEvent mouseLocation], NSInsetRect(_hover.frame, -4, -4));
}

- (void)pointerRested {
    NSUInteger index = _pointerIndex;
    if (index == NSNotFound || ![self hoverWantedAt:index] || [self pointerInHover]) return;
    if (![self serverFor:Lsp::Feature::Hover]) {   // only a diagnostic to show
        [self showHover:Json() gen:_hoverGen uri:_uri index:index byMouse:YES];
        return;
    }
    [self requestHoverAt:index byMouse:YES];
}

- (void)requestHoverAt:(NSUInteger)index byMouse:(BOOL)byMouse {
    MCLspServer *server = [self serverFor:Lsp::Feature::Hover];
    if (!server) {   // no server answers hover: what the diagnostics say there
        [self showHover:Json() gen:_hoverGen uri:_uri index:index byMouse:byMouse];
        return;
    }
    [self flushChanges];
    int gen = ++_hoverGen;
    __weak LspSession *weakSelf = self;
    NSString *uri = _uri;
    Lsp::Position pos = Lsp::positionForOffset(U16Of(_tv.string), index);
    server.client.hover(Std(uri), pos, [weakSelf, gen, uri, index, byMouse](const Json &r,
                                                                              const Json &e) {
        (void)e;
        [weakSelf showHover:r gen:gen uri:uri index:index byMouse:byMouse];
    });
}

// The word around a character, for a server that names no range.
- (NSRange)wordAt:(NSUInteger)index {
    NSString *t = _tv.string;
    if (index >= t.length) return NSMakeRange(index, 0);
    if (!IsIdentChar([t characterAtIndex:index])) return NSMakeRange(index, 1);
    NSUInteger a = index, b = index + 1;
    while (a > 0 && IsIdentChar([t characterAtIndex:a - 1])) a--;
    while (b < t.length && IsIdentChar([t characterAtIndex:b])) b++;
    return NSMakeRange(a, b - a);
}

- (void)showHover:(const Json &)result gen:(int)gen uri:(NSString *)uri
            index:(NSUInteger)index byMouse:(BOOL)byMouse {
    if (gen != _hoverGen) return;
    if (uri != _uri && ![uri isEqualToString:_uri]) return;
    if (index > _tv.string.length) return;
    NSRange range = [self wordAt:index];
    Lsp::HoverDoc doc = Lsp::parseHoverDoc(result);
    if (doc.hasRange) {
        std::u16string text = U16Of(_tv.string);
        size_t a = Lsp::offsetForPosition(text, doc.range.start);
        size_t b = Lsp::offsetForPosition(text, doc.range.end);
        if (a <= index && index < b) range = NSMakeRange(a, b - a);
    }
    // The pointer moved on while the server thought.
    if (byMouse && (_pointerIndex == NSNotFound ||
                    !(NSLocationInRange(_pointerIndex, range) || _pointerIndex == index)))
        return;

    NSMutableAttributedString *s = [NSMutableAttributedString new];
    NSAttributedString *gap = [[NSAttributedString alloc] initWithString:@"\n\n"
        attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:5]}];
    for (MCDiagnosticMark *m in _tv.diagnostics) {
        if (!(NSLocationInRange(index, m.range) ||
              (m.range.length == 0 && index == m.range.location)))
            continue;
        if (s.length) [s appendAttributedString:gap];
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:m.message ?: @""
                attributes:@{NSFontAttributeName: DocFont(NO, NO, NO, 12),
                             NSForegroundColorAttributeName: SeverityColor(m.severity)}]];
        range = NSUnionRange(range, m.range);
    }
    for (const Lsp::HoverDoc::Part &part : doc.parts) {
        NSAttributedString *t = MCDocText(NS(part.text), part.markdown);
        if (!t.length) continue;
        if (s.length) [s appendAttributedString:gap];
        [s appendAttributedString:t];
    }
    _lastHoverText = s.length ? [s.string copy] : @"";
    if (!s.length) {
        if (!byMouse) [self flash:@"No information here"];
        return;
    }

    // Anchored to the line the text starts on, at the text (or the caret).
    __block NSRect line = NSZeroRect;
    NSRange anchor = byMouse ? NSMakeRange(range.location, MAX(range.length, 1))
                             : NSMakeRange(index, 0);
    if (anchor.length) {
        [_tv enumerateRectsForRange:anchor block:^(NSRect r) {
            if (NSIsEmptyRect(line)) line = r;
        }];
    }
    if (NSIsEmptyRect(line)) {
        NSRect screen = [_tv firstRectForCharacterRange:NSMakeRange(index, 0) actualRange:NULL];
        line = [_tv.window convertRectFromScreen:screen];
    } else {
        line = [_tv convertRect:line toView:nil];
    }
    NSRect screenLine = [_tv.window convertRectToScreen:line];
    screenLine.size.width = MAX(screenLine.size.width, 1);

    if (!_hover) {
        _hover = [MCHoverPopup new];
        __weak LspSession *weakSelf = self;
        _hover.onExit = ^{ [weakSelf closeHoverSoon]; };
    }
    _hoverByMouse = byMouse;
    _hoverRange = range;
    [_hover show:s atScreenRect:screenLine parent:_tv.window];
}

- (void)closeHoverSoon {
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(closeHoverUnlessPointer)
                                               object:nil];
    [self performSelector:@selector(closeHoverUnlessPointer) withObject:nil
               afterDelay:kHoverGrace];
}

// Kept while the pointer is in the box or on what it is about.
- (void)closeHoverUnlessPointer {
    if (!_hover.visible) return;
    if (_hoverByMouse) {
        if ([self pointerInHover]) return;
        NSWindow *w = _tv.window;
        if (w) {
            NSPoint p = [_tv convertPoint:[w convertPointFromScreen:[NSEvent mouseLocation]]
                                 fromView:nil];
            NSUInteger i = [_tv characterIndexUnderPoint:p];
            if (i != NSNotFound && NSLocationInRange(i, _hoverRange)) return;
        }
    }
    [self closeHover];
}

- (void)closeHover {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(pointerRested)
                                               object:nil];
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(closeHoverUnlessPointer)
                                               object:nil];
    _hoverGen++;
    [_hover close];
}

- (BOOL)hoverVisible { return _hover.visible; }

// What the diagnostics say at a character, in the box at the caret (F8's
// step, Lsp+Diagnostics.mm), until the caret moves.
- (void)showHoverAtCaret:(NSUInteger)index {
    [self showHover:Json() gen:_hoverGen uri:_uri index:index byMouse:NO];
}

// Esc closes the box (handleCommand:, when no list took the key).
- (BOOL)hoverTakesCommand:(SEL)sel {
    if (!_hover.visible || sel != @selector(cancelOperation:)) return NO;
    [self closeHover];
    return YES;
}

// The document left the view: answers still on their way were for it.
- (void)forgetNavigationRequests {
    _definitionGen++;
    _hoverGen++;
}

// ---- Go to Symbol
- (void)documentSymbolsWithCompletion:(void (^)(NSArray<MCSymbolItem *> *, NSInteger,
                                                NSString *))done {
    if (!done) return;
    if (!_path || !_tv.editable) {
        done(nil, -1, nil);
        return;
    }
    MCLspServer *server = [self serverFor:Lsp::Feature::DocumentSymbols];
    NSString *path = _path;
    if (!server || !_uri) {
        NSInteger caret = -1;
        NSArray *items = MCOutlineSymbols(path, _tv.string, _tv.selectedRange.location, &caret);
        done(items, caret, @"outline");
        return;
    }
    [self flushChanges];
    int gen = ++_symbolGen;
    NSString *uri = _uri, *name = server.name;
    __weak LspSession *weakSelf = self;
    server.client.documentSymbols(Std(uri), [weakSelf, gen, uri, path, name, done](
                                                const Json &r, const Json &e) {
        LspSession *s = weakSelf;
        if (!s || gen != s->_symbolGen || ![uri isEqualToString:s->_uri]) return;
        NSString *text = s->_tv.string;
        NSUInteger caret = s->_tv.selectedRange.location;
        NSInteger caretIndex = -1;
        if (!e.isNull()) {   // the server refused: the outline stands in
            NSArray *items = MCOutlineSymbols(path, text, caret, &caretIndex);
            done(items, caretIndex, @"outline");
            return;
        }
        std::vector<Lsp::Symbol> syms = Lsp::parseDocumentSymbols(r);
        done(MCSymbolItemsFrom(syms, text, caret, &caretIndex), caretIndex, name);
    });
}

- (void)cancelWorkspaceSymbols {
    _workspaceGen++;
    for (NSArray *pair in _workspaceRequests) {
        MCLspServer *server = pair[0];
        if (server.running) server.client.cancel([pair[1] intValue]);
    }
    [_workspaceRequests removeAllObjects];
}

- (void)workspaceSymbols:(NSString *)query
              completion:(void (^)(NSArray<MCSymbolItem *> *, BOOL, NSString *))done {
    if (!done) return;
    [self cancelWorkspaceSymbols];
    NSMutableArray<MCLspServer *> *servers = [NSMutableArray array];
    // Each running server once, in a steady order (the open file's first).
    NSMutableArray<MCLspEntry *> *entries = [NSMutableArray array];
    for (NSString *key in [_entries.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        MCLspEntry *e = _entries[key];
        if ([_servers containsObject:e.server]) [entries insertObject:e atIndex:0];
        else [entries addObject:e];
    }
    for (MCLspEntry *e in entries) {
        MCLspServer *server = e.server;
        if (!server.running || server.client.state() != Lsp::Client::State::Ready) continue;
        if (!server.client.offers(Lsp::Feature::WorkspaceSymbols)) continue;
        if (![servers containsObject:server]) [servers addObject:server];
    }
    if (!servers.count) {
        done(@[], YES, _entries.count ? @"No running language server searches the workspace"
                                      : @"No language server is running in this window");
        return;
    }
    [self flushChanges];
    [self syncOpenDocuments];
    const int gen = ++_workspaceGen;
    auto answers = std::make_shared<std::vector<std::vector<Lsp::Symbol>>>(servers.count);
    auto pending = std::make_shared<size_t>(servers.count);
    __weak LspSession *weakSelf = self;
    for (NSUInteger i = 0; i < servers.count; i++) {
        MCLspServer *server = servers[i];
        auto answered = std::make_shared<bool>(false);
        // One answer per server: its reply, or nothing when it exits first
        // (serverExited:), which used to leave the palette searching.
        void (^answer)(const Json *) = ^(const Json *r) {
            LspSession *s = weakSelf;
            if (!s || gen != s->_workspaceGen || *answered) return;
            *answered = true;
            if (r) (*answers)[i] = Lsp::parseWorkspaceSymbols(*r);
            if (*pending > 0) (*pending)--;
            std::vector<Lsp::Symbol> merged = Lsp::mergeSymbols(*answers, CanonicalURI);
            NSMutableArray<MCSymbolItem *> *items = [NSMutableArray array];
            for (const Lsp::Symbol &sym : merged) {
                MCSymbolItem *it = MCSymbolItemFrom(sym);
                it.path = [s treePathForURI:sym.uri];
                if (it.path.length) [items addObject:it];
            }
            done(items, *pending == 0, nil);
        };
        int rid = server.client.workspaceSymbols(Std(query), [answer](const Json &r,
                                                                      const Json &e) {
            answer(e.isNull() ? &r : nullptr);
        });
        [_workspaceRequests addObject:@[server, @(rid), (id)answer]];
    }
}

// A server that exits takes its replies with it: what it was asked for
// Go to Symbol in Workspace counts as answered with nothing.
- (void)finishWorkspaceSymbolsOf:(MCLspServer *)server {
    for (NSArray *entry in [_workspaceRequests copy]) {
        if (entry[0] != server) continue;
        [_workspaceRequests removeObject:entry];
        void (^answer)(const Json *) = entry[2];
        answer(nullptr);
    }
}

- (void)revealSymbol:(MCSymbolItem *)item {
    if (!item) return;
    Lsp::Location loc;
    if (item.path.length) {
        loc.uri = Lsp::uriFromPath(Std(RealPath(item.path)));
    } else if (_uri) {
        loc.uri = Std(_uri);
    } else {
        return;
    }
    loc.range.start = Lsp::Position{(int)item.line, (int)item.character};
    loc.range.end = Lsp::Position{(int)item.endLine, (int)item.endCharacter};
    [self revealLocation:loc];
}

@end
