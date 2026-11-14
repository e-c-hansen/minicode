// EditorController+Navigation.mm — EditorController: Go to Line, Go Back and Forward, Go to Symbol.
#import "EditorController+Private.h"

@implementation EditorController (Navigation)

// Moves the caret to a 1-based line and column (the column counts
// characters, and 0 means the start of the line), showing the source first
// if a preview is up.
- (void)goToLine:(int)line column:(int)column {
    [self.navigator jump:^{ [self moveToLine:line column:column]; }];
}

- (void)moveToLine:(int)line column:(int)column {
    if (self.previewMode && self.canTogglePreview) [self togglePreview:nil];
    NSString *text = self.textView.string;
    NSUInteger index = 0;
    for (int l = 1; l < line && index < text.length; l++) {
        NSRange nl = [text rangeOfString:@"\n" options:0
                                   range:NSMakeRange(index, text.length - index)];
        if (nl.location == NSNotFound) { index = text.length; break; }
        index = NSMaxRange(nl);
    }
    NSRange lineRange = [text lineRangeForRange:NSMakeRange(MIN(index, text.length), 0)];
    NSUInteger lineEnd = NSMaxRange(lineRange);
    while (lineEnd > lineRange.location &&
           [text characterAtIndex:lineEnd - 1] == '\n') lineEnd--;
    for (int c = 1; c < column && index < lineEnd; c++)
        index = NSMaxRange([text rangeOfComposedCharacterSequenceAtIndex:index]);
    [self.window makeFirstResponder:self.textView];
    [self.textView setSelectedRange:NSMakeRange(index, 0)];
    [self.textView scrollRangeToVisible:NSMakeRange(index, 0)];
    if (lineEnd > lineRange.location)
        [self.textView showFindIndicatorForRange:
            NSMakeRange(lineRange.location, lineEnd - lineRange.location)];
}

// For the palette's Go to Line (Ctrl+G) and Quick Open (Cmd+P).
- (BOOL)canGoToLine {
    return self.currentPath && !self.showingMessage && !self.isImage && !self.isPDF &&
           !self.isMedia && !self.isDiff;
}

// ---------------------------------------------- Go Back, Go to Symbol
// Ctrl+- and Ctrl+Shift+-, VS Code's Mac keys (Navigation.h). The grid
// terminal keeps Ctrl+- for its program (it claims Ctrl keys first).
- (void)navigateBack:(id)sender {
    (void)sender;
    [self.navigator goBack];
}
- (void)navigateForward:(id)sender {
    (void)sender;
    [self.navigator goForward];
}

// MCNavigatorHost: the file on show, and whether the editor holds its text.
- (NSString *)navigationPath:(BOOL *)hasText {
    if (hasText)
        *hasText = self.canGoToLine && !self.previewMode && self.textView.editable;
    return _activeTab ? self.currentPath : nil;
}

- (BOOL)showNavigationPlace:(MCPlace *)place {
    NSString *path = place.path;
    if (!path.length) return NO;
    if (![path isEqualToString:self.currentPath]) {
        MCTab *tab = [self tabForPath:path];
        if (tab) [self activateTab:tab];
        else if ([[NSFileManager defaultManager] fileExistsAtPath:path]) [self showFile:path];
        if (![self.currentPath isEqualToString:path]) return NO;
    }
    [self revealEditor];
    NSTextView *tv = self.textView;
    if (place.hasText && self.canGoToLine && !self.previewMode && tv.editable) {
        const NSUInteger n = tv.string.length;
        NSRange r = place.selection;
        if (r.location > n) r = NSMakeRange(n, 0);
        if (NSMaxRange(r) > n) r.length = n - r.location;
        [self.window makeFirstResponder:tv];
        [tv setSelectedRange:r];
        [self revealSymbolRange:r];
    } else {
        [self focusActiveDocument];
    }
    return YES;
}

// A symbol's range in view: left where it is when it already shows, else a
// third of the way down the pane, as VS Code centres it.
- (void)revealSymbolRange:(NSRange)r {
    NSTextView *tv = self.textView;
    NSRect rect = [self textRectForCharacter:r.location];
    if (NSIsEmptyRect(rect)) {
        [tv scrollRangeToVisible:r];
        return;
    }
    if (!NSContainsRect(tv.visibleRect, rect))
        [self scrollEditorToY:NSMinY(rect) - NSHeight(tv.visibleRect) / 3];
}

- (void)addSymbolsToPaletteContext:(MCPaletteContext *)ctx {
    __weak EditorController *weakSelf = self;
    // "#" asks every language server running in the window.
    ctx.workspaceSymbols = ^(NSString *query,
                             void (^done)(NSArray<MCSymbolItem *> *, BOOL, NSString *)) {
        [weakSelf.lsp workspaceSymbols:query completion:done];
    };
    ctx.cancelWorkspaceSymbols = ^{ [weakSelf.lsp cancelWorkspaceSymbols]; };
    ctx.openWorkspaceSymbol = ^(MCSymbolItem *item) { [weakSelf.lsp revealSymbol:item]; };

    if (!self.canGoToLine || self.showingMessage) return;
    NSString *path = self.currentPath;
    NSTextView *tv = self.textView;
    // Where the editor is now, for Esc and for Go Back after a symbol.
    const NSRange selection = tv.selectedRange;
    const NSPoint scroll = self.editorScroll.contentView.bounds.origin;
    MCPlace *from = [self.navigator currentPlace];
    void (^restore)(void) = ^{
        EditorController *s = weakSelf;
        if (!s || ![s.currentPath isEqualToString:path]) return;
        [s.navigator beQuiet:^{
            NSTextView *t = s.textView;
            if (t.editable && NSMaxRange(selection) <= t.string.length)
                t.selectedRange = selection;
            [s scrollEditorToY:scroll.y];
        }];
    };
    if (self.previewMode) {
        // A rendered Markdown page: its headings, from the source, and the
        // page scrolled to each.
        if (!self.isMarkdown) return;
        NSString *source = self.sourceText ?: @"";
        ctx.documentSymbols = ^(void (^done)(NSArray<MCSymbolItem *> *, NSInteger, NSString *)) {
            done(MCOutlineSymbols(path, source, 0, NULL), -1, @"outline");
        };
        void (^show)(MCSymbolItem *) = ^(MCSymbolItem *item) {
            EditorController *s = weakSelf;
            if (!s || ![s.currentPath isEqualToString:path] || !s.previewMode) return;
            NSUInteger at = [s markdownCharacterForSourceLine:item.line];
            NSRect r = [s textRectForCharacter:at];
            if (!NSIsEmptyRect(r)) [s scrollEditorToY:NSMinY(r) - 8];
        };
        ctx.previewSymbol = show;
        ctx.goToSymbol = ^(MCSymbolItem *item) {
            show(item);
            [weakSelf focusActiveDocument];
        };
        ctx.endPreview = restore;
        return;
    }
    ctx.documentSymbols = ^(void (^done)(NSArray<MCSymbolItem *> *, NSInteger, NSString *)) {
        EditorController *s = weakSelf;
        if (!s || ![s.currentPath isEqualToString:path]) { done(nil, -1, nil); return; }
        [s.lsp documentSymbolsWithCompletion:done];
    };
    ctx.previewSymbol = ^(MCSymbolItem *item) {
        EditorController *s = weakSelf;
        if (!s || ![s.currentPath isEqualToString:path] || item.range.location == NSNotFound)
            return;
        NSTextView *t = s.textView;
        if (NSMaxRange(item.range) > t.string.length) return;
        [s.navigator beQuiet:^{
            t.selectedRange = item.range;
            [s revealSymbolRange:item.range];
        }];
    };
    ctx.goToSymbol = ^(MCSymbolItem *item) {
        EditorController *s = weakSelf;
        if (!s || ![s.currentPath isEqualToString:path] || item.range.location == NSNotFound)
            return;
        NSTextView *t = s.textView;
        if (NSMaxRange(item.range) > t.string.length) return;
        [s.navigator jumpFrom:from body:^{
            [s.window makeFirstResponder:t];
            t.selectedRange = item.range;
            [s revealSymbolRange:item.range];
            [t showFindIndicatorForRange:item.range];
        }];
    };
    ctx.endPreview = restore;
}

@end
