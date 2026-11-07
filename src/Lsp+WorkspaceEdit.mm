// Lsp+WorkspaceEdit.mm — Objective-C++. LspSession (WorkspaceEdit): the
// workspace edit applier, which rename, code actions, a server's own
// workspace/applyEdit and Replace in Folder share. Every file or none:
// each is read and checked (the editor's text, a tab's buffer, or the
// disk's, against what the server was sent), opened, and then edited as
// one multi-range replacement, one undo step per file; a refusal after
// edits puts them back. It keeps no state of its own.
#import "Lsp+Private.h"

@implementation LspSession (WorkspaceEdit)

// The text of a file as the editor has it: the text view's for the file on
// show, a tab's buffer for another open file, else the disk's.
- (NSString *)textOfPath:(NSString *)path uri:(const std::string &)uri {
    if ([self uri:uri isSameFileAs:_uri]) return _tv.string;
    NSString *buffer = self.bufferText ? self.bufferText(path) : nil;
    if (buffer) return buffer;
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}

// The text a server's positions in a file refer to: what it was last sent
// for the file when a server has it open (every tab's file is), else the
// disk's.
- (NSString *)serverTextOfPath:(NSString *)path uri:(const std::string &)uri {
    for (MCLspEntry *e in _entries.allValues) {
        const std::string *t = e.server ? e.server.client.textFor(uri) : nullptr;
        if (t) return NS(*t);
    }
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}

// Another window with a tab on the file whose text the disk does not have
// (unsaved changes): an edit here would be lost there, or lose them on its
// Keep Mine. Replace in Folder asks the same of every window.
- (BOOL)otherWindowHasChangesTo:(NSString *)path {
    NSString *disk = nil;
    BOOL read = NO;
    for (LspSession *other in MCLspAllSessions().allObjects) {
        if (other == self || !other.bufferText) continue;
        NSString *buffer = other.bufferText(path);
        if (!buffer) continue;
        if (!read) {
            disk = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding
                                                error:nil];
            read = YES;
        }
        if (!disk || ![buffer isEqualToString:disk]) return YES;
    }
    return NO;
}

- (BOOL)showsPath:(NSString *)path {
    return _path && [RealPath(path) isEqualToString:RealPath(_path)];
}

// Show a file an edit is about to change in the editor, opening its tab if
// need be (and noting a tab it had to make in `opened`). NO, with *why, when
// it does not show as text to edit, holding `text`.
- (BOOL)showEditFile:(NSString *)path text:(NSString *)text
              opened:(NSMutableArray<NSString *> *)opened failure:(NSString **)why {
    if (![self showsPath:path]) {
        BOOL hadTab = NO;
        for (NSString *p in self.openPaths ? self.openPaths() : @[])
            if ([RealPath(p) isEqualToString:RealPath(path)]) hadTab = YES;
        if (!self.openFile(path)) {
            *why = [NSString stringWithFormat:@"Could not open %@", path.lastPathComponent];
            return NO;
        }
        if (!hadTab && ![opened containsObject:path]) [opened addObject:path];
    }
    if (!_tv.editable || ![_tv.string isEqualToString:text]) {
        *why = [NSString stringWithFormat:@"%@ is not open as text to edit",
                                          path.lastPathComponent];
        return NO;
    }
    return YES;
}

// Apply a WorkspaceEdit (rename, a quick fix, workspace/applyEdit). Each
// file's edits go into its buffer as one undo step, through the text view,
// so highlighting, the dirty mark and didChange follow as for typing. A file
// not open gets a tab; nothing is saved, so the user reviews and saves.
// All or nothing: every file is read and checked first, then opened in its
// tab and checked again, and only then edited; a file that cannot be edited
// after others were puts those back (their undo step undone), and tabs the
// edit opened close again. Returns how many files changed, or -1 with
// *failure saying why.
- (NSInteger)applyWorkspaceEdit:(const Lsp::WorkspaceEdit &)we
                     actionName:(NSString *)name
                        failure:(NSString **)failure {
    return [self applyWorkspaceEdit:we server:nil asked:nullptr actionName:name failure:failure];
}

// `server` is the one that computed the edit, and `asked` the versions of
// its open documents when the request went out: a file whose text changed
// since (reloaded from disk while the server worked, say) is refused, as is
// one whose documentChanges version is not the version open now, since the
// edit's positions are for the older text.
- (NSInteger)applyWorkspaceEdit:(const Lsp::WorkspaceEdit &)we
                         server:(MCLspServer *)server
                          asked:(const Lsp::DocVersions *)asked
                     actionName:(NSString *)name
                        failure:(NSString **)failure {
    __autoreleasing NSString *dummy = nil;
    if (!failure) failure = &dummy;
    *failure = nil;
    if (!we.problem.empty()) { *failure = NS(we.problem); return -1; }
    if (we.files.empty()) return 0;
    if (!self.openFile) { *failure = @"No window to edit in"; return -1; }
    if (server) {
        std::string stale = Lsp::staleEditFile(we, asked ? *asked : Lsp::DocVersions(),
                                               server.client.versions());
        if (!stale.empty()) {
            *failure = [NSString stringWithFormat:
                @"%@ changed while the language server worked on it; try again",
                NS(Lsp::pathFromUri(stale)).lastPathComponent];
            return -1;
        }
    }

    // 1. Read and check every file.
    NSMutableArray<NSString *> *paths = [NSMutableArray array], *texts = [NSMutableArray array];
    for (const Lsp::FileEdit &f : we.files) {
        NSString *path = [self treePathForURI:f.uri];
        if (!path.length) {
            *failure = [NSString stringWithFormat:@"The edit names %s, which is not a file",
                                                  f.uri.c_str()];
            return -1;
        }
        NSString *text = [self textOfPath:path uri:f.uri];
        if (!text) {
            *failure = [NSString stringWithFormat:@"Could not read %@", path.lastPathComponent];
            return -1;
        }
        if ([self otherWindowHasChangesTo:path]) {
            *failure = [NSString stringWithFormat:
                @"%@ has unsaved changes in another window; save them there and try again",
                path.lastPathComponent];
            return -1;
        }
        // The server's positions are in the text it has: a tab's text as
        // last sent, or the disk's for a file no tab has open. Text that
        // differs from that (typed since, or a tab the server was never
        // sent) is edited only when every range holds the same text in
        // both; otherwise the edit would land in the wrong places.
        NSString *base = [self serverTextOfPath:path uri:f.uri];
        if (base && ![base isEqualToString:text]) {
            std::u16string a = U16Of(text), b = U16Of(base);
            const Lsp::PositionIndex ia(a), ib(b);
            for (const Lsp::TextEdit &e : f.edits) {
                size_t a0 = ia.offset(e.range.start);
                size_t a1 = ia.offset(e.range.end);
                size_t b0 = ib.offset(e.range.start);
                size_t b1 = ib.offset(e.range.end);
                if (a.compare(a0, a1 - a0, b, b0, b1 - b0) != 0 ||
                    ia.position(a0).line != e.range.start.line) {
                    *failure = [NSString stringWithFormat:
                        @"%@ has changes the language server has not seen; "
                         "save it and try again", path.lastPathComponent];
                    return -1;
                }
            }
        }
        [paths addObject:path];
        [texts addObject:text];
    }

    // The tab to come back to: the file on show, or what the tab shows
    // when it is no source to edit (a preview, a picture, a PDF).
    NSString *home = _path ?: (self.shownTabPath ? self.shownTabPath() : nil);
    NSMutableArray<NSString *> *opened = [NSMutableArray array];   // tabs this edit made
    NSMutableArray<NSNumber *> *applied = [NSMutableArray array];
    NSInteger changed = 0;
    NSString *why = nil;
    _applyingEdit = YES;

    // 2. Open every file in its tab, before anything is edited.
    BOOL ok = YES;
    for (NSUInteger i = 0; i < paths.count && ok; i++)
        ok = [self showEditFile:paths[i] text:texts[i] opened:opened failure:&why];

    // 3. Edit them, each in one multi-range replacement: one undo step, and
    // the text view takes all of it or none.
    for (NSUInteger i = 0; i < paths.count && ok; i++) {
        ok = [self showEditFile:paths[i] text:texts[i] opened:opened failure:&why];
        if (!ok) break;
        std::vector<Lsp::OffsetEdit> edits = Lsp::offsetEdits(U16Of(_tv.string), we.files[i].edits);
        NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
        NSMutableArray<NSString *> *strings = [NSMutableArray array];
        for (auto e = edits.rbegin(); e != edits.rend(); ++e) {   // ascending
            [ranges addObject:[NSValue valueWithRange:NSMakeRange(e->start, e->length)]];
            [strings addObject:[NSString stringWithCharacters:(const unichar *)e->text.data()
                                                       length:e->text.size()]];
        }
        NSRange sel = _tv.selectedRange;
        NSUndoManager *um = _tv.undoManager;
        [_tv breakUndoCoalescing];
        [um beginUndoGrouping];
        BOOL may = [_tv shouldChangeTextInRanges:ranges replacementStrings:strings];
        if (may) {
            // Each edit its own storage edit, last first, so the
            // highlighter, the squiggles and the server follow each as
            // they follow typing; one beginEditing around them all
            // stretched the squiggles between the first and the last over
            // the whole run. One didChangeText for them all: NSTextView
            // lays out from the edit onwards in every one, as
            // replaceSourceWithText: found (EditorController.mm).
            NSTextStorage *ts = _tv.textStorage;
            for (NSInteger k = (NSInteger)ranges.count - 1; k >= 0; k--)
                [ts replaceCharactersInRange:ranges[(NSUInteger)k].rangeValue
                                  withString:strings[(NSUInteger)k]];
            [_tv didChangeText];
            // The caret keeps its place in the text around it.
            for (const Lsp::OffsetEdit &e : edits) {   // last first
                NSUInteger at = sel.location, end = e.start + e.length;
                if (at >= end) at = at - e.length + e.text.size();
                else if (at > e.start) at = e.start + MIN(at - e.start, e.text.size());
                sel = NSMakeRange(at, 0);
            }
        }
        [um setActionName:name ?: @"Edit"];
        [um endUndoGrouping];
        [_tv breakUndoCoalescing];
        if (!may) {
            why = [NSString stringWithFormat:@"%@ could not be edited",
                                             paths[i].lastPathComponent];
            ok = NO;
            break;
        }
        if (NSMaxRange(sel) <= _tv.string.length) [_tv setSelectedRange:sel];
        [applied addObject:@(i)];
        changed++;
    }

    // 4. A failure part way: the files already edited go back as they were.
    if (!ok) {
        for (NSNumber *n in applied.reverseObjectEnumerator) {
            NSUInteger i = n.unsignedIntegerValue;
            if (![self showsPath:paths[i]] && !self.openFile(paths[i])) continue;
            // Exactly the group this edit made, even inside the event's own
            // group, which is still open while this runs. Showing the tab
            // may have put an edit of its own on top (a keep-sorted sort
            // waiting for it), so undo goes on, a step or two, until the
            // text is what it was.
            NSUndoManager *um = _tv.undoManager;
            for (int k = 0; k < 3 && um.canUndo && ![_tv.string isEqualToString:texts[i]]; k++)
                [um undoNestedGroup];
            if (![_tv.string isEqualToString:texts[i]]) {
                NSLog(@"MiniCode: could not put %@ back after a failed edit", paths[i]);
                why = [NSString stringWithFormat:@"%@; %@ was changed and could not be put "
                                                  "back, so check it before saving",
                                                 why ?: @"The edit could not be made",
                                                 paths[i].lastPathComponent];
            }
        }
        changed = 0;
    }
    // Back to the file the edit was asked from.
    if (home && ![self showsPath:home]) self.openFile(home);
    if (!ok && self.closeFile)
        for (NSString *p in opened)
            if (![self showsPath:p]) self.closeFile(p);
    _applyingEdit = NO;
    [self flushChanges];
    [self syncOpenDocuments];
    if (!ok) *failure = why ?: @"The edit could not be made";
    return ok ? changed : -1;
}

- (NSInteger)applyEditsToPaths:(NSArray<NSString *> *)paths
                        ranges:(NSArray<NSArray<NSValue *> *> *)ranges
                       strings:(NSArray<NSArray<NSString *> *> *)strings
                    actionName:(NSString *)name
                       failure:(NSString **)failure {
    [self flushChanges];
    Lsp::WorkspaceEdit we;
    for (NSUInteger i = 0; i < paths.count; i++) {
        Lsp::FileEdit f;
        f.uri = Lsp::uriFromPath(Std(RealPath(paths[i])));
        NSString *text = [self textOfPath:paths[i] uri:f.uri];
        if (!text) {
            if (failure) *failure = [NSString stringWithFormat:@"Could not read %@",
                                                               paths[i].lastPathComponent];
            return -1;
        }
        const std::u16string u = U16Of(text);
        const Lsp::PositionIndex index(u);
        for (NSUInteger k = 0; k < ranges[i].count; k++) {
            NSRange r = ranges[i][k].rangeValue;
            Lsp::TextEdit e;
            e.range.start = index.position(r.location);
            e.range.end = index.position(NSMaxRange(r));
            e.newText = Std(strings[i][k]);
            f.edits.push_back(e);
        }
        we.files.push_back(f);
    }
    return [self applyWorkspaceEdit:we actionName:name failure:failure];
}

@end
