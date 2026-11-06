// FindBarSuite.mm — `make guitest SUITES=find_bar`: the editor's find and
// replace bar (EditorFind.h), with the menu's keys. Cmd+F opens it with the
// keyboard in the query, typing finds as you go and selects the match from
// the caret, Return and Cmd+G step on, the three options narrow the
// matches, Replace changes one and moves to the next, Replace All is one
// undo step, and Esc closes the bar and gives the editor the keyboard.
#import "Harness.h"
#import "EditorController.h"
#import "EditorFind.h"

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
}
