// EditorController+Search.mm — EditorController: find and replace, Find in Folder and TODOs.
#import "EditorController+Private.h"

@implementation EditorController (Search)

// ------------------------------------------------- find and replace
// The find items reach the text view first when it has the keyboard
// (CodeTextView onFindAction); from anywhere else in the window (the tree,
// the source control panel) they open the editor's bar while it shows text.
- (void)performTextFinderAction:(id)sender {
    if (self.editorScroll.hidden) {
        NSBeep();
        return;
    }
    [_editorFind performFindAction:[sender respondsToSelector:@selector(tag)] ? [sender tag] : 0];
}
// Option+Cmd+C, W and R (Edit > Find Options), VS Code's keys.
- (void)toggleFindMatchCase:(id)sender { [_editorFind toggleMatchCase]; }
- (void)toggleFindWholeWord:(id)sender { [_editorFind toggleWholeWord]; }
- (void)toggleFindRegex:(id)sender { [_editorFind toggleRegex]; }
- (void)toggleFindPreserveCase:(id)sender { [_editorFind togglePreserveCase]; }
- (void)toggleFindInSelection:(id)sender {
    if (self.editorScroll.hidden) {
        NSBeep();
        return;
    }
    [_editorFind toggleFindInSelection];
}

// Cmd+Shift+F : project-wide text search. As in VS Code, text selected on
// one line of the editor becomes the query.
- (void)openSearch:(id)sender {
    [self makeSearchPanel];
    [self seedSearchFromSelection];
    [self.searchPanel show];
}

// Option+Shift+Cmd+F: the same window, the keyboard in its replace field.
- (void)openReplaceInFolder:(id)sender {
    [self makeSearchPanel];
    [self seedSearchFromSelection];
    [self.searchPanel showReplace];
}

- (void)seedSearchFromSelection {
    if (self.window.firstResponder != self.textView || self.editorScroll.hidden) return;
    NSRange sel = self.textView.selectedRange;
    NSString *text = self.textView.string;
    if (sel.length == 0 || sel.length > 1000 || NSMaxRange(sel) > text.length) return;
    NSString *s = [text substringWithRange:sel];
    if ([s rangeOfCharacterFromSet:[NSCharacterSet newlineCharacterSet]].location != NSNotFound)
        return;
    if (self.searchPanel.regex) s = [NSRegularExpression escapedPatternForString:s];
    [self.searchPanel seedQuery:s];
}

- (void)makeSearchPanel {
    if (!self.searchPanel) {
        __weak EditorController *weakSelf = self;
        self.searchPanel = [[SearchPanel alloc]
            initWithRoot:_root.path
             openHandler:^(NSString *path, NSInteger line) {
                EditorController *s = weakSelf;
                [s.navigator jump:^{
                    [s openFileAtPath:path from:EditorTabs::OpenFrom::SearchOpen];
                    [s jumpToLine:line];
                }];
                [s.window makeKeyAndOrderFront:nil];
            }];
        // A single click shows the line in the preview tab, the keyboard
        // staying in the results (VS Code's search view).
        self.searchPanel.previewHandler = ^(NSString *path, NSInteger line) {
            EditorController *s = weakSelf;
            [s.navigator jump:^{
                [s openFileAtPath:path from:EditorTabs::OpenFrom::SearchClick];
                if ([s.currentPath isEqualToString:path]) [s jumpToLine:line];
            }];
        };
        // Replace in Folder edits open tabs through their buffers, as
        // rename does, and leaves a tab with unsaved changes alone.
        self.searchPanel.openState = ^MCOpenFileState(NSString *path) {
            return [weakSelf replaceStateOfPath:path];
        };
        self.searchPanel.tabText = ^NSString *(NSString *path) {
            EditorController *s = weakSelf;
            return s ? s.lsp.bufferText(path) : nil;
        };
        self.searchPanel.editTabs = ^NSInteger(NSArray<NSString *> *paths,
                                               NSArray<NSArray<NSValue *> *> *ranges,
                                               NSArray<NSArray<NSString *> *> *strings,
                                               NSString **failure) {
            EditorController *s = weakSelf;
            if (!s) return -1;
            return [s.lsp applyEditsToPaths:paths ranges:ranges strings:strings
                                 actionName:@"Replace in Folder" failure:failure];
        };
    }
    // If a folder is selected in the tree, scope the search to it; otherwise
    // leave the scope as it is (the opened folder by default).
    NSInteger row = self.outline.selectedRow;
    if (row >= 0) {
        FileItem *node = [self.outline itemAtRow:row];
        if (node.isDir) [self.searchPanel setScope:node.path];
    }
}

// How a tab of this window holds a file, for Replace in Folder: 0 none (or
// one that will simply reload from disk: not read yet, or a clean preview),
// 1 a text tab holding the disk's text, 2 a tab whose text the disk does
// not have (unsaved changes, or a change on disk not taken in yet).
- (int)tabHoldOfPath:(NSString *)path disk:(NSString *__strong *)disk {
    MCTab *t = [self tabForPath:path];
    if (!t || t.showingMessage || t.isImage || t.isPDF || t.isMedia || t.isDiff) return 0;
    if (!t.loaded && !t.dirty) return 0;
    NSString *text = self.lsp.bufferText(path);
    if (!text) return t.dirty ? 2 : 0;
    // Read as the tab read it (a UTF-16 file decoded, a byte order mark
    // kept apart), or a clean UTF-16 tab would count as holding changes.
    if (!*disk) *disk = MCReadDiskText(path, ULLONG_MAX).text ?: @"";
    if (![text isEqualToString:*disk]) return 2;
    if (t.previewMode && !t.dirty) return 0;   // shows the disk's text: it reloads
    return t.previewMode ? 2 : 1;
}

// Across every editor window: a tab with text the disk lacks anywhere
// leaves the file alone; a tab here holding the disk's text is edited
// there; otherwise the file is written (other windows' clean tabs reload).
- (MCOpenFileState)replaceStateOfPath:(NSString *)path {
    NSString *disk = nil;
    MCOpenFileState state = MCOpenFileNone;
    for (NSWindow *w in NSApp.windows) {
        id d = w.delegate;
        if (![d isKindOfClass:[EditorController class]]) continue;
        int hold = [(EditorController *)d tabHoldOfPath:path disk:&disk];
        if (hold == 2) return MCOpenFileConflict;
        if (hold == 1 && d == self) state = MCOpenFileHere;
    }
    return state;
}

- (BOOL)ownsWindow:(NSWindow *)w {
    if (!w) return NO;
    if (w == self.window || w == self.searchPanel.window || w == self.todoPanel.window) return YES;
    // A sheet or a child window (a popover's) of one of those.
    NSWindow *up = w.sheetParent ?: w.parentWindow;
    return up && up != w && [self ownsWindow:up];
}

// Shift+Cmd+D: every TODO comment and open task in the folder, scoped as
// Find in Folder is.
- (void)showTodos:(id)sender {
    if (!self.todoPanel) {
        __weak EditorController *weakSelf = self;
        self.todoPanel = [[SearchPanel alloc]
            initTodosWithRoot:_root.path
                  openHandler:^(NSString *path, NSInteger line) {
                EditorController *s = weakSelf;
                [s openFileAtPath:path from:EditorTabs::OpenFrom::SearchOpen];
                if (![s.currentPath isEqualToString:path]) return;   // refused
                [s.window makeKeyAndOrderFront:nil];
                [s goToLine:(int)line column:0];
            }];
        self.todoPanel.previewHandler = ^(NSString *path, NSInteger line) {
            EditorController *s = weakSelf;
            [s openFileAtPath:path from:EditorTabs::OpenFrom::SearchClick];
            if ([s.currentPath isEqualToString:path]) [s goToLine:(int)line column:0];
        };
    }
    NSInteger row = self.outline.selectedRow;
    if (row >= 0) {
        FileItem *node = [self.outline itemAtRow:row];
        if (node.isDir) [self.todoPanel setScope:node.path];
    }
    [self.todoPanel show];
}

// Move the editor selection to a 1-based line and reveal it.
- (void)jumpToLine:(NSInteger)line {
    if (line < 1) return;
    NSString *s = self.textView.string;
    __block NSUInteger loc = NSNotFound;
    __block NSInteger n = 1;
    [s enumerateSubstringsInRange:NSMakeRange(0, s.length)
                          options:NSStringEnumerationByLines
                       usingBlock:^(NSString *sub, NSRange r, NSRange e, BOOL *stop) {
        (void)sub; (void)e;
        if (n == line) { loc = r.location; *stop = YES; }
        n++;
    }];
    if (loc == NSNotFound) return;
    NSRange lineRange = [s lineRangeForRange:NSMakeRange(loc, 0)];
    [self.textView setSelectedRange:lineRange];
    [self.textView scrollRangeToVisible:lineRange];
    [self.window makeFirstResponder:self.textView];
}

@end
