// FindBarSuite.mm — `make guitest SUITES=find_bar`: the editor's find and
// replace bar (EditorFind.h), with the menu's keys. Cmd+F opens it with the
// keyboard in the query, typing finds as you go and selects the match from
// the caret, Return and Cmd+G step on, the three options narrow the
// matches, Replace changes one and moves to the next, Replace All is one
// undo step, and Esc closes the bar and gives the editor the keyboard.
// Then VS Code's layout, frame by frame against findWidget.css, in its four
// widths; Cmd+F seeding from the selection or the word at the caret; Find
// in Selection and Preserve Case; the red of "No results"; and pictures in
// a dark and a light theme ($MC_GUI_SNAPSHOTS).
#import "Harness.h"
#import "EditorController.h"
#import "EditorFind.h"
#import "FindWidget.h"

namespace {
NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
MCEditorFind *Find(EditorController *c) { return [c valueForKey:@"editorFind"]; }
NSTextView *Editor(NSTextField *field) { return (NSTextView *)field.currentEditor; }
void TypeInto(NSTextField *field, NSString *s) {
    NSTextView *ed = Editor(field);
    [ed insertText:s replacementRange:ed.selectedRange];
}
void SetField(NSTextField *field, NSString *s) {
    NSTextView *ed = Editor(field);
    [ed selectAll:nil];
    [ed insertText:s replacementRange:ed.selectedRange];
}
NSString *Selected(EditorController *c) {
    NSTextView *tv = Text(c);
    return [tv.string substringWithRange:tv.selectedRange];
}
BOOL Settled(EditorController *c, NSUInteger count) {
    return Find(c).resultsCurrent && Find(c).matchRanges.count == count;
}
}  // namespace

MC_GUI_SUITE(find_bar) {
    __block EditorController *c = nil;
    NSString *file = MCGuiProjectFile(@"find/words.txt");
    // Per line: "alpha" twice, "Alpha" once, "alphabet" once.
    NSMutableString *text = [NSMutableString string];
    for (int i = 0; i < 50; i++) [text appendFormat:@"%d alpha Alpha alphabet alpha.\n", i];
    NSString *original = [text copy];

    MCGuiStep(@"a file; Cmd+F", ^{
        MCGuiWriteFile(file, original);
        c = MCGuiController();
        [c focusEditor:nil];
        GCHECK([c showFile:file]);
        Text(c).selectedRange = NSMakeRange([original rangeOfString:@"10 alpha"].location, 0);
        [c.window makeFirstResponder:Text(c)];
        GCHECK(MCGuiPerformMenuKey(c.window, 3, NSEventModifierFlagCommand, nil, nil));
        GCHECK(Find(c).shown);
        GCHECK(c.window.firstResponder == Editor(Find(c).bar.findField));
        TypeInto(Find(c).bar.findField, @"alpha");
    });
    MCGuiWait(@"200 matches, case ignored", 5, ^BOOL { return Settled(c, 200); });
    MCGuiStep(@"the first match after the caret is selected; the count says so", ^{
        NSUInteger at = [original rangeOfString:@"10 alpha"].location + 3;
        GCHECK(Text(c).selectedRange.location == at && Text(c).selectedRange.length == 5);
        GCHECK(Find(c).currentIndex == 40);
        GCHECK_EQ_STR(Find(c).bar.countLabel.stringValue, @"41 of 200");
        // Return in the query: the next match; the keyboard stays there.
        [Editor(Find(c).bar.findField) doCommandBySelector:@selector(insertNewline:)];
    });
    MCGuiWait(@"the next", 5, ^BOOL { return Find(c).currentIndex == 41; });
    MCGuiStep(@"Cmd+G from the bar steps on too", ^{
        GCHECK_EQ_STR(Selected(c), @"Alpha");
        GCHECK(c.window.firstResponder == Editor(Find(c).bar.findField));
        GCHECK(MCGuiPerformMenuKey(c.window, 5, NSEventModifierFlagCommand, nil, nil));
    });
    MCGuiWait(@"the one after", 5, ^BOOL { return Find(c).currentIndex == 42; });
    MCGuiStep(@"Match Case (Option+Cmd+C)", ^{
        GCHECK_EQ_STR(Selected(c), @"alpha");   // the start of alphabet
        GCHECK(MCGuiPerformMenuKey(c.window, 8, NSEventModifierFlagCommand | NSEventModifierFlagOption,
                                   nil, nil));
        GCHECK(Find(c).matchCase);
    });
    MCGuiWait(@"150 matches", 5, ^BOOL { return Settled(c, 150); });
    MCGuiStep(@"Whole Word (Option+Cmd+W)", ^{
        GCHECK(MCGuiPerformMenuKey(c.window, 13, NSEventModifierFlagCommand | NSEventModifierFlagOption,
                                   nil, nil));
        GCHECK(Find(c).wholeWord);
    });
    MCGuiWait(@"100 matches", 5, ^BOOL { return Settled(c, 100); });
    MCGuiStep(@"a regular expression (Option+Cmd+R), Match Case and Whole Word off", ^{
        [Find(c) toggleMatchCase];
        [Find(c) toggleWholeWord];
        GCHECK(MCGuiPerformMenuKey(c.window, 15, NSEventModifierFlagCommand | NSEventModifierFlagOption,
                                   nil, nil));
        GCHECK(Find(c).regex);
        SetField(Find(c).bar.findField, @"alpha(bet)?\\.");
    });
    MCGuiWait(@"50 matches of the regex", 5, ^BOOL { return Settled(c, 50); });
    MCGuiStep(@"a bad regex says so and finds nothing", ^{
        SetField(Find(c).bar.findField, @"alpha(");
    });
    MCGuiWait(@"no matches", 5, ^BOOL { return Find(c).resultsCurrent && Find(c).matchRanges.count == 0; });
    MCGuiStep(@"Replace (Option+Cmd+F opens its row)", ^{
        MCGuiNote(@"a bad regex: \"%@\"", Find(c).bar.countLabel.stringValue);
        [Find(c) toggleRegex];
        SetField(Find(c).bar.findField, @"alphabet");
        GCHECK(MCGuiPerformMenuKey(c.window, 3, NSEventModifierFlagCommand | NSEventModifierFlagOption,
                                   nil, nil));
        GCHECK(Find(c).bar.replaceShown);
        GCHECK(c.window.firstResponder == Editor(Find(c).bar.replaceField));
        TypeInto(Find(c).bar.replaceField, @"omega");
    });
    MCGuiWait(@"50 alphabets", 5, ^BOOL { return Settled(c, 50); });
    MCGuiStep(@"Return in the replace field replaces one and moves on", ^{
        NSUInteger before = Find(c).currentIndex;
        [Editor(Find(c).bar.replaceField) doCommandBySelector:@selector(insertNewline:)];
        (void)before;
    });
    MCGuiWait(@"one replaced", 5, ^BOOL { return Settled(c, 49); });
    MCGuiStep(@"unsaved; the next alphabet selected; Replace All", ^{
        GCHECK([[c valueForKey:@"dirty"] boolValue]);
        GCHECK([Text(c).string componentsSeparatedByString:@"omega"].count == 2);
        GCHECK_EQ_STR(Selected(c), @"alphabet");
        // The harness's trap: edits made from its callbacks share the undo
        // manager's automatic group, which a real event would have closed.
        NSUndoManager *um = Text(c).undoManager;
        if (um.groupsByEvent && um.groupingLevel > 0) [um endUndoGrouping];
        GCHECK([Find(c) performFindAction:NSTextFinderActionReplaceAll]);
    });
    MCGuiWait(@"all replaced", 5, ^BOOL { return Settled(c, 0); });
    MCGuiStep(@"50 omegas; the note; one undo takes back Replace All", ^{
        GCHECK([Text(c).string componentsSeparatedByString:@"omega"].count == 51);
        GCHECK_EQ_STR([[c valueForKey:@"statusLabel"] stringValue], @"Replaced 49 matches");
        [Text(c).undoManager undo];
        NSUInteger left = [Text(c).string componentsSeparatedByString:@"omega"].count - 1;
        if (left != 1) MCGuiNote(@"omegas after one undo: %lu (%@)", (unsigned long)left,
                                 Text(c).undoManager.undoActionName);
        GCHECK(left == 1);
        [Text(c).undoManager undo];
        GCHECK_EQ_STR(Text(c).string, original);
        GCHECK(![[c valueForKey:@"dirty"] boolValue]);
    });
    MCGuiStep(@"Esc closes the bar; the editor has the keyboard", ^{
        [Editor(Find(c).bar.replaceField) doCommandBySelector:@selector(cancelOperation:)];
        GCHECK(!Find(c).shown);
        GCHECK(c.window.firstResponder == Text(c));
    });

    // ---- VS Code's layout (findWidget.css): checked by frames, and pictures.
    MCGuiStep(@"Cmd+F with \"Alpha\" selected on one line: it is the query", ^{
        NSRange alpha = [original rangeOfString:@"Alpha"];
        Text(c).selectedRange = alpha;
        [c.window makeFirstResponder:Text(c)];
        GCHECK(MCGuiPerformMenuKey(c.window, 3, NSEventModifierFlagCommand, nil, nil));
        GCHECK(Find(c).shown);
        GCHECK_EQ_STR(Find(c).bar.findField.stringValue, @"Alpha");
        GCHECK(c.window.firstResponder == Editor(Find(c).bar.findField));
        // The replace row stays as it was left (open), as in VS Code; the
        // chevron closes it.
        GCHECK(Find(c).bar.replaceShown);
        [Find(c).bar.toggleReplaceButton performClick:nil];
        GCHECK(!Find(c).bar.replaceShown);
    });
    MCGuiWait(@"200 matches of Alpha", 5, ^BOOL { return Settled(c, 200); });
    MCGuiStep(@"the bar's frame and its parts, left to right", ^{
        MCEditorFindBar *bar = Find(c).bar;
        NSScrollView *sv = Text(c).enclosingScrollView;
        NSRect editor = [sv convertRect:sv.bounds toView:bar.superview];
        MCGuiNote(@"editor %.0f wide; bar %@", NSWidth(editor), NSStringFromRect(bar.frame));
        GCHECK(NSHeight(bar.frame) == 33);
        GCHECK(fabs(NSMaxY(editor) - 4 - NSMaxY(bar.frame)) < 0.5);   // 4 below the top
        GCHECK(NSMaxX(bar.frame) <= NSMaxX(editor) - 14 && NSMaxX(bar.frame) >= NSMaxX(editor) - 20);
        // The parts, in an editor wide enough for the whole bar (other
        // suites may have left this one narrower).
        [Find(c) layoutInEditorRect:NSMakeRect(0, 0, 900, 600) visible:YES];
        [bar layoutSubtreeIfNeeded];
        GCHECK(bar.widthMode == 0 && NSWidth(bar.frame) == 419);
        // Chevron on the far left, the full height; the input from 27.
        GCHECK(NSMinX(bar.toggleReplaceButton.frame) == 1 &&
               NSHeight(bar.toggleReplaceButton.frame) == 31);
        NSRect in = bar.findInput.frame;
        GCHECK(NSMinX(in) == 27 && NSHeight(in) == 25);
        // The three toggles inside the input's right end, 20 x 20.
        CGFloat x = NSMaxX(bar.findInput.bounds) - 2;
        for (MCFindButton *t in @[bar.regexButton, bar.wordButton, bar.caseButton]) {
            GCHECK(t.superview == bar.findInput && NSWidth(t.frame) == 20 && !t.hidden);
            GCHECK(fabs(NSMaxX(t.frame) - x) < 0.5);
            x = NSMinX(t.frame) - 2;
        }
        GCHECK(NSMaxX(bar.findInput.field.frame) <= NSMinX(bar.caseButton.frame));
        // Then the count, Previous, Next, Find in Selection; Close at the
        // top right.
        NSView *row[] = {bar.findInput, bar.countLabel, bar.previousButton, bar.nextButton,
                         bar.selectionButton};
        for (int i = 1; i < 5; i++) GCHECK(NSMinX(row[i].frame) >= NSMaxX(row[i - 1].frame));
        GCHECK(NSMaxX(bar.selectionButton.frame) == NSWidth(bar.frame) - 30);
        GCHECK(NSEqualRects(bar.closeButton.frame, NSMakeRect(419 - 27, 6, 22, 22)));
        GCHECK_EQ_STR(bar.countLabel.stringValue, @"2 of 200");   // Alpha, the second
        // Tooltips name the keys.
        GCHECK([bar.replaceAllButton.toolTip containsString:@"⌥⌘⏎"]);
        GCHECK([bar.selectionButton.toolTip containsString:@"⌥⌘L"]);
        GCHECK([bar.caseButton.toolTip containsString:@"⌥⌘C"]);
        GCHECK([bar.toggleReplaceButton.toolTip containsString:@"⌥⌘F"]);
        // Find in Selection is offered: there is a selection (the match).
        GCHECK(bar.selectionButton.enabled && !bar.selectionButton.on);
        MCGuiSnapshot(bar, @"find-bar-dark");
        Find(c).onLayoutNeeded();
    });
    MCGuiStep(@"the replace row: its input as wide, Preserve Case inside, two icons", ^{
        GCHECK(MCGuiPerformMenuKey(c.window, 3, NSEventModifierFlagCommand | NSEventModifierFlagOption,
                                   nil, nil));
        MCEditorFindBar *bar = Find(c).bar;
        [bar.superview layoutSubtreeIfNeeded];
        GCHECK(bar.replaceShown && NSHeight(bar.frame) == 62);
        NSRect rin = bar.replaceInput.frame, in = bar.findInput.frame;
        GCHECK(NSMinX(rin) == NSMinX(in) && NSWidth(rin) == NSWidth(in) && NSMinY(rin) == 33);
        GCHECK(bar.preserveCaseButton.superview == bar.replaceInput);
        GCHECK(NSMinX(bar.replaceButton.frame) == NSMaxX(rin) + 3);
        GCHECK(NSMinX(bar.replaceAllButton.frame) == NSMaxX(bar.replaceButton.frame) + 3);
        GCHECK(NSHeight(bar.toggleReplaceButton.frame) == 60);
        GCHECK(bar.toggleReplaceButton.icon == MCFindIconChevronDown);
        TypeInto(bar.replaceField, @"Omega");
        MCGuiSnapshot(bar, @"find-bar-replace-dark");
        MCGuiSnapshot(c.window.contentView, @"find-window-dark");
    });
    MCGuiStep(@"nothing found: \"No results\" in red, the query outlined", ^{
        [c.window makeFirstResponder:Find(c).bar.findField];
        SetField(Find(c).bar.findField, @"zebra");
    });
    MCGuiWait(@"no matches", 5, ^BOOL { return Settled(c, 0); });
    MCGuiStep(@"the red", ^{
        MCEditorFindBar *bar = Find(c).bar;
        GCHECK_EQ_STR(bar.countLabel.stringValue, @"No results");
        GCHECK(bar.findInput.error);
        GCHECK(!bar.previousButton.enabled && !bar.nextButton.enabled);
        MCGuiSnapshot(bar, @"find-bar-no-results-dark");
        SetField(bar.findField, @"alpha");
    });
    MCGuiWait(@"alpha again", 5, ^BOOL { return Settled(c, 200); });
    MCGuiStep(@"found again: no red", ^{ GCHECK(!Find(c).bar.findInput.error); });

    // ---- seeding from the selection, the bar already open
    MCGuiStep(@"a selection over two lines leaves the query alone", ^{
        NSRange two = NSMakeRange([original rangeOfString:@"3 alpha"].location, 40);
        Text(c).selectedRange = two;
        [c.window makeFirstResponder:Text(c)];
        GCHECK(MCGuiPerformMenuKey(c.window, 3, NSEventModifierFlagCommand, nil, nil));
        GCHECK_EQ_STR(Find(c).bar.findField.stringValue, @"alpha");
        GCHECK(c.window.firstResponder == Editor(Find(c).bar.findField));
    });
    MCGuiStep(@"no selection: the word at the caret (inside \"alphabet\")", ^{
        NSUInteger at = [original rangeOfString:@"7 alpha Alpha alphabet"].location + 17;
        Text(c).selectedRange = NSMakeRange(at, 0);
        [c.window makeFirstResponder:Text(c)];
        GCHECK(MCGuiPerformMenuKey(c.window, 3, NSEventModifierFlagCommand, nil, nil));
        GCHECK_EQ_STR(Find(c).bar.findField.stringValue, @"alphabet");
    });
    MCGuiWait(@"50 alphabets, the one at the caret selected", 5, ^BOOL {
        return Settled(c, 50) && Find(c).currentIndex == 7;
    });
    MCGuiStep(@"with Regular Expression on, the selection is escaped", ^{
        [Find(c) toggleRegex];
        NSRange dot = [original rangeOfString:@"alpha.\n"];
        Text(c).selectedRange = NSMakeRange(dot.location, 6);
        [c.window makeFirstResponder:Text(c)];
        GCHECK(MCGuiPerformMenuKey(c.window, 3, NSEventModifierFlagCommand, nil, nil));
        GCHECK_EQ_STR(Find(c).bar.findField.stringValue, @"alpha\\.");
        [Find(c) toggleRegex];
        SetField(Find(c).bar.findField, @"alpha");
    });
    MCGuiWait(@"alpha", 5, ^BOOL { return Settled(c, 200); });

    // ---- Find in Selection (Option+Cmd+L in the bar)
    __block NSRange scope;
    MCGuiStep(@"lines 5 to 9 selected; Option+Cmd+L in the bar", ^{
        NSUInteger a = [original rangeOfString:@"5 alpha"].location;
        NSUInteger b = [original rangeOfString:@"10 alpha"].location;
        scope = NSMakeRange(a, b - a);   // ends at the start of line 10
        Text(c).selectedRange = scope;
        [c.window makeFirstResponder:Editor(Find(c).bar.findField)];
        GCHECK(MCGuiPerformMenuKey(c.window, 37, NSEventModifierFlagCommand | NSEventModifierFlagOption,
                                   nil, nil));
        GCHECK(Find(c).findInSelection);
        GCHECK(Find(c).bar.selectionButton.on);
        // The line break at its end is left out, as VS Code does.
        GCHECK(Find(c).searchScope.location == scope.location &&
               Find(c).searchScope.length == scope.length - 1);
    });
    MCGuiWait(@"20 matches, all in the five lines", 5, ^BOOL { return Settled(c, 20); });
    MCGuiStep(@"inside the scope; then Preserve Case (Option+Cmd+P) and Replace All", ^{
        for (NSValue *v in Find(c).matchRanges)
            GCHECK(v.rangeValue.location >= scope.location && NSMaxRange(v.rangeValue) <= NSMaxRange(scope));
        MCGuiSnapshot(c.window.contentView, @"find-in-selection-dark");
        GCHECK(MCGuiPerformMenuKey(c.window, 35, NSEventModifierFlagCommand | NSEventModifierFlagOption,
                                   nil, nil));
        GCHECK(Find(c).preserveCase && Find(c).bar.preserveCaseButton.on);
        SetField(Find(c).bar.replaceField, @"omega");
        NSUndoManager *um = Text(c).undoManager;
        if (um.groupsByEvent && um.groupingLevel > 0) [um endUndoGrouping];
        GCHECK([Find(c) performFindAction:NSTextFinderActionReplaceAll]);
    });
    MCGuiWait(@"replaced in the scope only", 5, ^BOOL {
        return [Text(c).string containsString:@"5 omega Omega omegabet omega."];
    });
    MCGuiStep(@"five lines changed, case kept; the rest alone; one undo", ^{
        NSString *t = Text(c).string;
        GCHECK([t componentsSeparatedByString:@"Omega"].count == 6);
        GCHECK([t containsString:@"9 omega Omega omegabet omega.\n10 alpha Alpha"]);
        GCHECK([t containsString:@"4 alpha Alpha alphabet alpha.\n5 omega"]);
        [Text(c).undoManager undo];
        GCHECK_EQ_STR(Text(c).string, original);
    });
    MCGuiStep(@"typing before the scope moves it; Esc ends it", ^{
        NSRange before = Find(c).searchScope;
        MCGuiUndoGroup(Text(c).undoManager, ^{
            [Text(c) insertText:@"xx" replacementRange:NSMakeRange(0, 0)];
        });
        GCHECK(Find(c).searchScope.location == before.location + 2 &&
               Find(c).searchScope.length == before.length);
        [Text(c).undoManager undo];
        [Find(c) close];
        GCHECK(!Find(c).findInSelection);
        [Find(c) togglePreserveCase];
        GCHECK(!Find(c).preserveCase);
    });

    // ---- VS Code's narrow editor: reduced, narrow, collapsed
    MCGuiStep(@"the widths VS Code gives a narrow editor", ^{
        [Find(c) performFindAction:NSTextFinderActionShowFindInterface];
        MCEditorFindBar *bar = Find(c).bar;
        struct { CGFloat editor; NSInteger mode; CGFloat width; } cases[] = {
            {800, 0, 419}, {440, 1, 397}, {370, 2, 257}, {320, 3, 170},
        };
        for (auto k : cases) {
            [Find(c) layoutInEditorRect:NSMakeRect(0, 0, k.editor, 500) visible:YES];
            [bar layoutSubtreeIfNeeded];
            MCGuiNote(@"editor %.0f: mode %ld, %.0f wide", k.editor, (long)bar.widthMode,
                      NSWidth(bar.frame));
            GCHECK(bar.widthMode == k.mode && NSWidth(bar.frame) == k.width);
            GCHECK(bar.countLabel.hidden == (k.mode != 0));
            GCHECK(bar.previousButton.hidden == (k.mode == 3));
            GCHECK(bar.caseButton.hidden == (k.mode == 3));
            GCHECK(!bar.selectionButton.hidden && !bar.closeButton.hidden);
            if (k.mode == 3) MCGuiSnapshot(bar, @"find-bar-collapsed-dark");
        }
        Find(c).onLayoutNeeded();
        [bar layoutSubtreeIfNeeded];
        NSScrollView *sv = Text(c).enclosingScrollView;
        GCHECK(bar.widthMode == (NSWidth(sv.frame) >= 419 + 28 ? 0 : bar.widthMode));
    });

    // ---- the light theme's colors
    MCGuiStep(@"Light Modern", ^{
        MCGuiSetSettings(@"theme = light-modern\nlsp.enabled = false\n");
    });
    MCGuiWait(@"the bar recolored", 5, ^BOOL {
        NSColor *bg = [NSColor colorWithCGColor:Find(c).bar.layer.backgroundColor];
        bg = [bg colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
        return bg && fabs(bg.redComponent - 0xF8 / 255.0) < 0.01;
    });
    MCGuiStep(@"pictures in the light theme", ^{
        MCEditorFindBar *bar = Find(c).bar;
        [Find(c) performFindAction:NSTextFinderActionShowReplaceInterface];
        [bar.superview layoutSubtreeIfNeeded];
        GCHECK(bar.replaceShown);
        MCGuiSnapshot(bar, @"find-bar-replace-light");
        MCGuiSnapshot(c.window.contentView, @"find-window-light");
        [Find(c) close];
        MCGuiSetSettings(@"lsp.enabled = false\n");
    });
}
