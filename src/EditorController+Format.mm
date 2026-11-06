// EditorController+Format.mm — EditorController: Format Document.
#import "EditorController+Private.h"

@implementation EditorController (Format)

// ------------------------------------------------------------ format document
// Shift+Option+F: run the file's formatter (FormatRunner.mm) on the buffer and
// put its output in as one undo step. Only the source view of a file can be
// formatted, never a preview, a message, a picture or a diff.
- (BOOL)canFormatSource {
    return self.currentPath && self.textView.editable && !self.showingMessage;
}

- (void)formatDocument:(id)sender {
    if (![self canFormatSource]) { NSBeep(); return; }
    [self formatSourceQuietly:NO then:nil];
}

// Start formatting. `then` runs once the result is in (applied, or refused)
// if the same file is still open; quietly leaves out the notes that only
// answer a key press (no formatter for this kind of file, already formatted).
- (void)formatSourceQuietly:(BOOL)quiet then:(void (^)(void))then {
    NSTextView *tv = self.textView;
    NSString *original = [tv.string copy];
    NSString *path = self.currentPath;
    MCTab *tab = _activeTab;
    // Format Document while Cmd+S waits for format.on-save: this run takes
    // over the save, since the one it replaces will be dropped.
    if (!then && _formatSaveTab && _formatSaveTab == tab) {
        __weak EditorController *weakSelf = self;
        then = ^{ [weakSelf finishFormatSave]; };
    }
    // A selection formats its lines, where the formatter can do that.
    NSInteger first = 0, last = 0;
    NSRange sel = tv.selectedRange;
    if (sel.length > 0) {
        NSUInteger end = NSMaxRange(sel);
        if ([original characterAtIndex:end - 1] == '\n' && end - 1 > sel.location) end--;
        // 1-based lines as the formatters count them ('\n'), in one pass up
        // to the selection's end through an inline buffer (a message per
        // character from the start cost tens of ms near the end of 5 MB).
        CFStringInlineBuffer buf;
        CFStringInitInlineBuffer((__bridge CFStringRef)original, &buf, CFRangeMake(0, (CFIndex)end));
        NSInteger line = 1;
        for (NSUInteger i = 0; i < end; i++) {
            if (i == sel.location) first = line;
            if (CFStringGetCharacterFromInlineBuffer(&buf, (CFIndex)i) == '\n') line++;
        }
        if (first == 0) first = line;   // the selection starts at its end (never expected)
        last = line;
    }
    NSUInteger generation = ++_formatGeneration;
    __weak EditorController *weakSelf = self;
    __weak MCTab *weakTab = tab;
    MCFormatText(original, path, self.rootPath, first, last, 10, ^(MCFormatResult *r) {
        EditorController *s = weakSelf;
        // Another format started, or the window closed, or another tab is
        // on show (which saved this one as it left, if a save waited).
        if (!s || generation != s->_formatGeneration || !weakTab || weakTab != s->_activeTab)
            return;
        if (r.error) {
            if (!(quiet && r.unavailable))
                [s showFormatNote:r.error error:YES install:r.installPrograms];
        } else if (!s.textView.editable || ![s.textView.string isEqualToString:original]) {
            // Typed into meanwhile, or a preview took over: the output is
            // for text that is no longer there.
        } else if (![s replaceSourceWithText:r.text actionName:@"Format Document"]) {
            if (!quiet)
                [s showFormatNote:[NSString stringWithFormat:@"Already formatted (%@, %@).",
                                                             r.tool, r.style]
                            error:NO];
        }
        if (then) then();
    });
}

// format.on-save: NO when this file has no formatter, so the save goes on.
- (BOOL)formatBeforeSaving {
    if (![self canFormatSource] || Formatter::languageFor(Utf8String(self.currentPath)).empty())
        return NO;
    __weak EditorController *weakSelf = self;
    _formatSaveTab = _activeTab;
    [self formatSourceQuietly:YES then:^{ [weakSelf finishFormatSave]; }];
    return YES;
}

// The formatter is done (or its result was dropped): the save Cmd+S asked
// for happens now, if it still waits.
- (void)finishFormatSave {
    MCTab *t = _formatSaveTab;
    _formatSaveTab = nil;
    if (t && t == _activeTab) [self saveCurrentFile:nil];
}

// The tab whose save waits for the formatter is leaving the screen (another
// tab, or it closes): the formatter's result is dropped and the tab saved
// now, unformatted, while it is still the one on show.
- (void)saveBeforeLeavingFormatSave {
    if (!_formatSaveTab || _formatSaveTab != _activeTab) return;
    _formatSaveTab = nil;
    _formatGeneration++;
    [self saveCurrentFile:nil];
}

// Put `text` in place of the source as one undo step, touching only the
// lines that differ, so the highlighter and the language server see an
// ordinary edit and the caret and the scroll position stay with their text.
// NO when the text is the same.
//
// The hunks go in through CodeTextView's replaceRanges: (one undo step, one
// didChangeText). Each hunk is still its own storage edit, so the
// highlighter, the squiggles and the language server follow each one as
// they follow typing. Each hunk used to get a didChangeText too, and
// NSTextView lays out from the edit onwards in every one: 298 hunks spread
// over a 100,000-line file took 58 s, against 2 s now (most of it the
// layout of the lines between the first hunk and the last, which a
// contiguous layout has to redo).
- (BOOL)replaceSourceWithText:(NSString *)text actionName:(NSString *)name {
    CodeTextView *tv = self.textView;
    const std::u16string a = U16(tv.string), b = U16(text);
    std::vector<LineDiff::Hunk> hunks = LineDiff::diff(a, b);
    if (hunks.empty()) return NO;

    // Where the view is: the character at the top of the pane, and how far
    // above or below the top its line sits.
    NSRect visible = tv.visibleRect;
    NSUInteger top = [tv characterIndexForInsertionAtPoint:
        NSMakePoint(NSMinX(visible) + tv.textContainerOrigin.x + 1, NSMinY(visible) + 1)];
    __block CGFloat topY = NAN;
    NSUInteger length = a.size();
    if (top <= length)
        [tv enumerateRectsForRange:NSMakeRange(top, top < length ? 1 : 0) block:^(NSRect r) {
            if (isnan(topY)) topY = NSMinY(r);
        }];
    NSRange sel = tv.selectedRange;

    NSMutableArray<NSValue *> *ranges = [NSMutableArray arrayWithCapacity:hunks.size()];
    NSMutableArray<NSString *> *strings = [NSMutableArray arrayWithCapacity:hunks.size()];
    for (const LineDiff::Hunk &h : hunks) {
        [ranges addObject:[NSValue valueWithRange:NSMakeRange(h.oldStart, h.oldLength)]];
        [strings addObject:FromU16(b.substr(h.newStart, h.newLength))];
    }
    if (![tv replaceRanges:ranges withStrings:strings actionName:name]) return NO;

    size_t start = LineDiff::mapOffset(a, b, hunks, sel.location);
    size_t end = sel.length ? LineDiff::mapOffset(a, b, hunks, NSMaxRange(sel)) : start;
    if (end < start) end = start;
    [tv setSelectedRange:NSMakeRange(start, end - start)];
    if (!isnan(topY)) {
        NSUInteger newTop = LineDiff::mapOffset(a, b, hunks, top);
        NSUInteger newLength = tv.string.length;
        __block CGFloat y = NAN;
        [tv enumerateRectsForRange:NSMakeRange(newTop, newTop < newLength ? 1 : 0)
                             block:^(NSRect r) { if (isnan(y)) y = NSMinY(r); }];
        if (!isnan(y))
            [tv scrollPoint:NSMakePoint(NSMinX(visible), MAX(0, y - (topY - NSMinY(visible))))];
    }
    return YES;
}

// A note from Format Document by the caret: the formatter's complaint, what
// to install, or that there was nothing to change (which closes itself).
- (void)showFormatNote:(NSString *)message error:(BOOL)error {
    [self showFormatNote:message error:error install:nil];
}

// With `install` (formatters, best first), the note is how to install one:
// the message, then the commands with Copy and Run in Terminal.
- (void)showFormatNote:(NSString *)message error:(BOOL)error
               install:(NSArray<NSString *> *)install {
    self.lastFormatNote = message;
    [_formatNote close];
    _formatNote = nil;
    CodeTextView *tv = self.textView;
    if (!tv.window || tv.isHiddenOrHasHiddenAncestor) { NSBeep(); return; }

    MCInstallView *installView = nil;
    if (install.count) {
        __weak EditorController *weakSelf = self;
        installView = [[MCInstallView alloc]
            initWithFormatters:install message:message width:500
                 runInTerminal:^(NSString *command) { [weakSelf typeCommandInTerminal:command]; }];
        if (!installView.command) installView = nil;
    }

    NSTextField *label = [NSTextField wrappingLabelWithString:message];
    label.font = error ? [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular]
                       : [NSFont systemFontOfSize:12];
    label.selectable = YES;
    const CGFloat maxWidth = 560;
    label.preferredMaxLayoutWidth = maxWidth;
    NSSize fit = [label sizeThatFits:NSMakeSize(maxWidth, CGFLOAT_MAX)];
    fit = NSMakeSize(ceil(MIN(fit.width, maxWidth)), ceil(fit.height));
    NSView *content = [[NSView alloc]
        initWithFrame:NSMakeRect(0, 0, fit.width + 24, fit.height + 20)];
    label.frame = NSMakeRect(12, 10, fit.width, fit.height);
    [content addSubview:label];
    NSViewController *vc = [NSViewController new];
    vc.view = installView ?: content;
    vc.preferredContentSize = vc.view.frame.size;
    NSPopover *note = [NSPopover new];
    note.contentViewController = vc;
    note.behavior = NSPopoverBehaviorTransient;
    if (installView) {
        note.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        __weak NSPopover *weakNote = note;
        installView.onDone = ^{ [weakNote close]; };
    }

    // Under the caret's line when it is on screen, else at the top of the pane.
    NSRect visible = tv.visibleRect;
    NSUInteger caret = tv.selectedRange.location, length = tv.string.length;
    __block NSRect anchor = NSZeroRect;
    [tv enumerateRectsForRange:NSMakeRange(caret, caret < length ? 1 : 0) block:^(NSRect r) {
        if (NSIsEmptyRect(anchor)) anchor = r;
    }];
    if (NSIsEmptyRect(anchor) || !NSIntersectsRect(anchor, visible))
        anchor = NSMakeRect(NSMidX(visible), NSMinY(visible), 1, 1);
    anchor.size.width = MAX(anchor.size.width, 1);
    anchor.size.height = MAX(anchor.size.height, 1);
    [note showRelativeToRect:anchor ofView:tv preferredEdge:NSRectEdgeMaxY];
    _formatNote = note;
    if (!error) {
        __weak NSPopover *weakNote = note;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.5 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{ [weakNote close]; });
    }
}

@end
