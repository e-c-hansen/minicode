// EditorController+EditorView.mm — EditorController: the editor: line numbers, brackets, zoom, comments, tasks and the editing hooks.
#import "EditorController+Private.h"
#import "EditorController+EditorViewIvars.h"

@implementation EditorController (EditorView)

// The buffer changed: it has unsaved changes exactly when it differs from
// the text on disk, so an edit taken back (typed and deleted, undone, a
// preview edit reverted) leaves none. Lengths are compared first, so most
// keystrokes cost nothing more; equal lengths compare the characters, a
// fraction of a millisecond for a large file.
- (void)noteEdited {
    [self scheduleBackups];
    [self scheduleAutoSave];
    NSString *now = self.sourceText ?: @"", *saved = self.savedText ?: @"";
    // A file deleted outside has nothing on disk to match.
    const BOOL dirty = _activeTab.gone || now.length != saved.length ||
                       ![now isEqualToString:saved];
    if (dirty && _activeTab.agentPreview) {
        _activeTab.agentPreview = NO;   // typed in: the user's tab now
        [self reloadTabStrip];
        [self saveSession];
    }
    if (dirty == self.dirty) return;
    self.dirty = dirty;
    [self updateTitle];
}

// Cmd+/ : comment or uncomment the lines the selection touches.
- (void)toggleComment:(id)sender {
    NSTextView *tv = self.textView;
    std::string marker = self.currentPath
        ? LineComments::markerFor(self.currentPath.UTF8String) : "";
    if (self.window.firstResponder != tv || !tv.editable || marker.empty()) {
        NSBeep();
        return;
    }
    NSRange sel = tv.selectedRange;
    LineComments::Result r = LineComments::toggle(U16(tv.string), sel.location,
                                                  NSMaxRange(sel), marker);
    if (!r.changed) return;
    NSRange range = NSMakeRange(r.replaceStart, r.replaceLength);
    NSString *replacement = FromU16(r.replacement);
    // Each toggle is its own undo step, not merged with typing or the last
    // toggle.
    [tv breakUndoCoalescing];
    if (![tv shouldChangeTextInRange:range replacementString:replacement]) return;
    [tv.textStorage replaceCharactersInRange:range withString:replacement];
    [tv didChangeText];
    [tv breakUndoCoalescing];
    [tv.undoManager setActionName:@"Toggle Comment"];
    [tv setSelectedRange:NSMakeRange(r.selStart, r.selEnd - r.selStart)];
}

// A click on a task's box in the preview: tick or clear it in the source,
// which is then dirty until Cmd+S, as after any other preview edit.
- (void)toggleTaskOnSourceLine:(int)line {
    const std::string src = Utf8String(self.sourceText ?: @"");
    const std::string edited = MarkdownTasks::toggleBox(src, line);
    if (edited == src) { NSBeep(); return; }
    [self applyMarkdownSource:[NSString stringWithUTF8String:edited.c_str()]];
    [self.textView.undoManager setActionName:@"Toggle Task"];
}

// Option+Cmd+L in Markdown source: make the lines the selection touches tasks, or
// tick or clear them.
- (void)toggleTask:(id)sender {
    NSTextView *tv = self.textView;
    if (!self.isMarkdown || self.window.firstResponder != tv || !tv.editable) {
        NSBeep();
        return;
    }
    NSRange sel = tv.selectedRange;
    MarkdownTasks::Edit e = MarkdownTasks::toggle(U16(tv.string), sel.location, NSMaxRange(sel));
    if (!e.changed) { NSBeep(); return; }
    NSRange range = NSMakeRange(e.replaceStart, e.replaceLength);
    NSString *replacement = FromU16(e.replacement);
    [tv breakUndoCoalescing];
    if (![tv shouldChangeTextInRange:range replacementString:replacement]) return;
    [tv.textStorage replaceCharactersInRange:range withString:replacement];
    [tv didChangeText];
    [tv breakUndoCoalescing];
    [tv.undoManager setActionName:@"Toggle Task"];
    [tv setSelectedRange:NSMakeRange(e.selStart, e.selEnd - e.selStart)];
}

// ------------------------------------------- line numbers, brackets and zoom
// The gutter's colors and mode, the bracket box's color, and the font (into
// the source attributes; recolorEditor then sets it in the text).
- (void)applyGutterSettings {
    const Settings &st = [AppSettings shared].settings;
    MCLineGutter *g = self.gutter;
    const Settings::LineNumbers mode = st.lineNumbers();
    g.mode = mode == Settings::LineNumbers::Off      ? MCLineNumbersOff
           : mode == Settings::LineNumbers::Relative ? MCLineNumbersRelative
                                                     : MCLineNumbersOn;
    g.backgroundColor = MCColor(st.gutterBackground());
    g.numberColor = MCColor(st.lineNumberColor());
    g.activeNumberColor = MCColor(st.lineNumberActiveColor());
    self.textView.bracketMatchColor = MCColor(st.bracketMatchColor());
    [self setEditorFont:MCEditorFont()];
}

// The font for the source from now on: the attributes the highlighter
// restyles with, what typing gets, and the gutter's numbers. The text on
// show changes when it is next restyled.
- (void)setEditorFont:(NSFont *)font {
    self.gutter.font = font;
    if (_sourceAttributes && ![_sourceAttributes[NSFontAttributeName] isEqual:font]) {
        NSMutableDictionary *base = [_sourceAttributes mutableCopy];
        base[NSFontAttributeName] = font;
        _sourceAttributes = base;
    }
    if (self.textView.editable && _liveHighlight) {
        NSMutableDictionary *typing = [self.textView.typingAttributes mutableCopy];
        typing[NSFontAttributeName] = font;
        self.textView.typingAttributes = typing;
    }
}

// Zoom In, Zoom Out or Reset Zoom, from any window: the source on show
// takes the new size at once, keeping the line at the top of the pane.
- (void)editorZoomChanged:(NSNotification *)note {
    (void)note;
    // The page beside a Markdown source follows the zoom, keeping its place.
    if ([self sidePreviewShowing]) [self renderSidePreviewKeepingPlace];
    NSFont *font = MCEditorFont();
    const BOOL showing = _liveHighlight && self.textView.editable;
    const BOOL changed = showing && ![_sourceAttributes[NSFontAttributeName] isEqual:font];
    if (!changed) {
        [self setEditorFont:font];
        // A rendered Markdown page follows the zoom too, keeping its place.
        if (self.isMarkdown && self.previewMode && !self.textView.editable)
            [self renderMarkdownKeepingPlace];
        return;
    }
    NSTextView *tv = self.textView;
    const NSUInteger n = tv.string.length;
    NSFont *old = _sourceAttributes[NSFontAttributeName];
    const CGFloat scale = old.pointSize > 0 ? font.pointSize / old.pointSize : 1;
    NSUInteger top = 0;
    CGFloat offset = 0;
    if (n > 0) {
        const NSRect visible = tv.visibleRect;
        top = MIN([tv characterIndexForInsertionAtPoint:
                      NSMakePoint(NSMinX(visible) + 20, NSMinY(visible) + 1)], n - 1);
        const NSRect r = [self textRectForCharacter:top];
        offset = NSIsEmptyRect(r) ? 0 : NSMinY(r) - NSMinY(visible);
    }
    [self setEditorFont:font];
    [self applyHighlighting];   // every run in the new font
    if (n > 0) {
        const NSRect r = [self textRectForCharacter:top];
        if (!NSIsEmptyRect(r)) {
            [tv.layoutManager ensureLayoutForBoundingRect:
                NSMakeRect(0, 0, CGFLOAT_MAX,
                           NSMaxY(r) + 2 * NSHeight(self.editorScroll.contentView.bounds))
                                          inTextContainer:tv.textContainer];
            [self scrollEditorToY:NSMinY(r) - offset * scale];
        }
    }
}

// The stretches of [lo, hi) that are strings or comments, from the
// highlighter's tokens of those lines, offset from lo (none without a
// grammar, so every bracket counts).
- (std::vector<Brackets::Span>)nonCodeSpansFrom:(NSUInteger)lo to:(NSUInteger)hi {
    std::vector<Brackets::Span> spans;
    if (!_hl || hi <= lo) return spans;
    NSTextStorage *storage = self.textView.textStorage;
    if (_hl->length() != storage.length) return spans;
    std::vector<Token> tokens;
    _hl->lineTokens(NSStringSource(storage.string), _hl->lineOf(lo), _hl->lineOf(hi - 1) + 1,
                    tokens);
    for (const Token &t : tokens) {
        if (!isStringStyle(t.style) && t.style != TokenStyle::Comment) continue;
        const size_t a = MAX((size_t)lo, t.start), b = MIN((size_t)hi, t.start + t.length);
        if (b <= a) continue;
        if (!spans.empty() && spans.back().end >= a - lo)
            spans.back().end = MAX(spans.back().end, b - lo);
        else
            spans.push_back({a - lo, b - lo});
    }
    return spans;
}

// ------------------------------------------------- auto-closing pairs (host)
// MCAutoClose asks these on each typed character and Backspace.
- (NSString *)autoClosePath {
    return self.currentPath;
}

- (BOOL)autoCloseOn {
    return [AppSettings shared].settings.autoClose() && _liveHighlight && !self.showingMessage;
}

- (std::unique_ptr<AutoClose::Context>)autoCloseContext {
    NSTextStorage *storage = self.textView.textStorage;
    if (!_hl || _hl->length() != storage.length) return std::make_unique<AutoClose::Context>();
    return std::make_unique<AutoClose::HighlighterContext>(
        *_hl, std::make_unique<NSStringSource>(storage.string));
}

// Bracket matching runs on a window of the text around the caret: what the
// search may reach, and no more.
- (NSRange)bracketWindowAround:(NSUInteger)caret {
    const NSUInteger n = self.textView.textStorage.length;
    const NSUInteger reach = Brackets::kDefaultScan + 1;
    const NSUInteger lo = caret > reach ? caret - reach : 0;
    const NSUInteger hi = MIN(n, caret + reach);
    return NSMakeRange(lo, hi - lo);
}

- (BOOL)bracketMatchingApplies {
    CodeTextView *tv = self.textView;
    return _liveHighlight && tv.editable && !tv.multiCaretActive && !self.showingMessage &&
           ![self largeFileFeaturesOff];
}

- (void)textViewDidChangeSelection:(NSNotification *)note {
    if (note.object != self.textView || _bracketMatchQueued) return;
    // Once the edit or the selection settles (typing changes both).
    _bracketMatchQueued = YES;
    dispatch_async(dispatch_get_main_queue(), ^{
        self->_bracketMatchQueued = NO;
        [self updateBracketMatch];
    });
}

// The bracket beside the caret and its partner get a box, as in VS Code.
- (void)updateBracketMatch {
    CodeTextView *tv = self.textView;
    const NSRange sel = tv.selectedRange;
    NSString *s = tv.textStorage.string;
    NSArray<NSValue *> *boxes = @[];
    if ([self bracketMatchingApplies] && sel.length == 0 && sel.location <= s.length) {
        const NSUInteger caret = sel.location;
        const BOOL near = (caret > 0 && Brackets::isBracket([s characterAtIndex:caret - 1])) ||
                          (caret < s.length && Brackets::isBracket([s characterAtIndex:caret]));
        if (near) {
            const NSRange w = [self bracketWindowAround:caret];
            std::vector<unichar> buf(w.length);
            [s getCharacters:buf.data() range:w];
            const Brackets::Pair p = Brackets::match(
                reinterpret_cast<const char16_t *>(buf.data()), buf.size(), caret - w.location,
                [self nonCodeSpansFrom:w.location to:NSMaxRange(w)]);
            if (p.found())
                boxes = @[[NSValue valueWithRange:NSMakeRange(w.location + p.open, 1)],
                          [NSValue valueWithRange:NSMakeRange(w.location + p.close, 1)]];
        }
    }
    tv.bracketRanges = boxes;
}

// Shift+Cmd+\ (VS Code's Go to Bracket): to the partner of the bracket by
// the caret, else to the closer of the pair around it, else to the next
// bracket.
- (void)goToBracket:(id)sender {
    (void)sender;
    CodeTextView *tv = self.textView;
    if (![self bracketMatchingApplies]) { NSBeep(); return; }
    const NSUInteger caret = NSMaxRange(tv.selectedRange);
    NSString *s = tv.textStorage.string;
    const NSRange w = [self bracketWindowAround:caret];
    std::vector<unichar> buf(w.length);
    [s getCharacters:buf.data() range:w];
    const size_t to = Brackets::jumpTarget(reinterpret_cast<const char16_t *>(buf.data()),
                                           buf.size(), caret - w.location,
                                           [self nonCodeSpansFrom:w.location to:NSMaxRange(w)]);
    if (to == Brackets::npos) { NSBeep(); return; }
    tv.selectedRange = NSMakeRange(w.location + to, 0);
    [tv scrollRangeToVisible:tv.selectedRange];
}

// -------------------------------------------------------------- editing hooks
// Arrows, Return, Tab and Esc drive the completion list while it is open;
// Option+Esc (complete:) asks the language server.
- (BOOL)textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
    if (tv == self.textView && [_notes handleCommand:sel]) return YES;
    if ([self.lsp handleCommand:sel]) return YES;
    // Esc with nothing else to close closes the find bar, as in VS Code.
    if (tv == self.textView && sel == @selector(cancelOperation:) &&
        !self.textView.multiCaretActive && [_editorFind closeFromEditor])
        return YES;
    // Return, Tab, Shift+Tab and Backspace follow the language's indentation
    // (AutoIndent), and Return in a Markdown list continues the list.
    if (tv == self.textView && MCIndentCommand(tv, sel, self.currentPath, self.isMarkdown))
        return YES;
    return NO;
}

// A typed closer, or Python's ':' after else and the like, may move its line
// back; MCIndentDidChange (from textDidChange:) does it once the text is in.
- (BOOL)textView:(NSTextView *)tv shouldChangeTextInRange:(NSRange)range
    replacementString:(NSString *)replacement {
    // Several carets type the same character everywhere, and a selection
    // wrapped in brackets keeps its place; no line moves.
    if (tv == self.textView && replacement && !self.textView.multiCaretActive &&
        !self.textView.multiCarets.applying)
        MCIndentWillChange(tv, range, replacement);
    return YES;
}

// Several ranges at once (CodeTextView replaceRanges:): answered here, since
// with only the method above NSTextView builds one string from the first
// range to the last to ask it, every time (a sixth of a keystroke at 10,000
// carets). Several carets never move a line.
- (BOOL)textView:(NSTextView *)tv shouldChangeTextInRanges:(NSArray<NSValue *> *)ranges
    replacementStrings:(NSArray<NSString *> *)strings {
    if (ranges.count == 1)
        return [self textView:tv shouldChangeTextInRange:ranges[0].rangeValue
            replacementString:strings.firstObject];
    return YES;
}

// F9: sort the selected lines, or the keep-sorted block around the caret.
- (void)sortLines:(id)sender {
    if (self.window.firstResponder != self.textView) {
        NSBeep();
        return;
    }
    [self flashStatus:MCSortLines(self.textView)];
}

// Shift+F9: sort every keep-sorted block in the file.
- (void)fixKeepSorted:(id)sender {
    if (self.window.firstResponder != self.textView) {
        NSBeep();
        return;
    }
    [self flashStatus:MCFixKeepSorted(self.textView)];
}

- (void)textDidChange:(NSNotification *)note {
    // A copy of the storage's string. With AppKit's storage (an
    // NSBigMutableString) it shared the characters and took about a
    // microsecond; MCSourceStorage keeps a CFString, so it is a copy of the
    // characters, 0.13 ms for a 2.4 MB file (measured), still a small part
    // of a keystroke. backedUpText's pointer compare relies on a new object
    // per change.
    self.sourceText = self.textView.string;
    [self noteEdited];
    // The title's task count follows typing, a moment after it stops.
    if (self.isMarkdown) {
        [NSObject cancelPreviousPerformRequestsWithTarget:self
                                                 selector:@selector(updateTitle)
                                                   object:nil];
        [self performSelector:@selector(updateTitle) withObject:nil afterDelay:0.3];
        [self sidePreviewSourceChanged];   // the page beside it, once typing pauses
    }
    [self flushHighlighting];   // recolor the lines this change touched
    MCIndentDidChange(self.textView, self.currentPath);
}

@end
