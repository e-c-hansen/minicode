// AutoCloseSuite.mm — `make guitest SUITES=auto_close`: brackets and quotes
// closing as they are typed in the editor (editor.auto-close, AutoClose),
// through the text view's own typing path: print( gives print() with the
// caret inside, ) types over the closer, Backspace takes the pair, Cmd+Z
// takes the keystroke back in one step; quotes by Python's rules; several
// carets; nothing while an input method composes; and the setting.
#import "Harness.h"
#import "EditingCommands.h"
#import "EditorController.h"
#import "Lsp.h"
#import "MultiCaretView.h"

namespace {

CodeTextView *Editor(EditorController *c) { return [c valueForKey:@"textView"]; }

// One typed character, as a key press delivers it, in its own undo group
// (each key press is one event).
void Type(NSTextView *tv, NSString *keys) {
    for (NSUInteger i = 0; i < keys.length; i++) {
        NSString *ch = [keys substringWithRange:NSMakeRange(i, 1)];
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:ch replacementRange:NSMakeRange(NSNotFound, 0)];
        });
    }
}

void Backspace(NSTextView *tv) {
    MCGuiUndoGroup(tv.undoManager, ^{ [tv doCommandBySelector:@selector(deleteBackward:)]; });
}

// The last line of the text with "|" where the caret is.
NSString *LastLine(NSTextView *tv) {
    NSString *s = tv.string;
    const NSUInteger caret = tv.selectedRange.location;
    NSString *marked = [NSString stringWithFormat:@"%@|%@", [s substringToIndex:caret],
                                                  [s substringFromIndex:caret]];
    return [marked componentsSeparatedByString:@"\n"].lastObject;
}

}  // namespace

MC_GUI_SUITE(auto_close) {
    __block EditorController *c = nil;
    __block CodeTextView *tv = nil;
    NSString *py = MCGuiProjectFile(@"auto_close.py");
    NSString *md = MCGuiProjectFile(@"auto_close.md");

    MCGuiStep(@"a Python file, the caret on a new last line", ^{
        MCGuiWriteFile(py, @"def f():\n    pass\n");
        MCGuiWriteFile(md, @"");   // empty: opens in the source
        c = MCGuiController();
        tv = Editor(c);
        [c focusEditor:nil];
        GCHECK([c showFile:py]);
        tv.selectedRange = NSMakeRange(tv.string.length, 0);
    });

    MCGuiStep(@"print( gives print() with the caret inside", ^{
        Type(tv, @"print(");
        MCGuiNote(@"after print(: %@", LastLine(tv));
        GCHECK([LastLine(tv) isEqualToString:@"print(|)"]);
        GCHECK(tv.multiCarets.autoClose.rememberedActions == 1);
    });

    // (Checked before the type-over: a key press that changes nothing
    // opens no undo group, but the explicit group a simulated one is
    // wrapped in would stay, empty, on top.)
    MCGuiStep(@"Cmd+Z takes the pair back in one step", ^{
        [tv.undoManager undo];
        MCGuiNote(@"after undo: %@", LastLine(tv));
        GCHECK([tv.string hasSuffix:@"\nprint"]);
        [tv.undoManager redo];
        GCHECK([tv.string hasSuffix:@"\nprint()"]);
        tv.selectedRange = NSMakeRange(tv.string.length - 1, 0);   // between the pair again
    });

    MCGuiStep(@") types over the closer", ^{
        // The caret left the pair and came back (undo, redo, a click): the
        // closer is let go, as in VS Code, so close a new one.
        tv.selectedRange = NSMakeRange(tv.string.length, 0);
        Type(tv, @"\nprint(");
        Type(tv, @")");
        GCHECK([LastLine(tv) isEqualToString:@"print()|"]);
        GCHECK([tv.string hasSuffix:@"\nprint()"]);   // no second )
        GCHECK(tv.multiCarets.autoClose.rememberedActions == 0);
    });

    MCGuiStep(@"Backspace between an empty pair it put in deletes both", ^{
        tv.selectedRange = NSMakeRange(tv.string.length, 0);
        Type(tv, @"\nlen(");
        GCHECK([LastLine(tv) isEqualToString:@"len(|)"]);
        Backspace(tv);
        GCHECK([LastLine(tv) isEqualToString:@"len|"]);
        // A pair typed by hand stays: only its opener goes.
        Type(tv, @"()");
        tv.selectedRange = NSMakeRange(tv.string.length - 1, 0);
        // (The caret came back from outside: the pair was let go.)
        Backspace(tv);
        GCHECK([LastLine(tv) isEqualToString:@"len|)"]);
    });

    MCGuiStep(@"quotes by Python's rules", ^{
        tv.selectedRange = NSMakeRange(tv.string.length, 0);
        Type(tv, @"\nx = \"hi\"");
        GCHECK([LastLine(tv) isEqualToString:@"x = \"hi\"|"]);
        Type(tv, @"\n# don't");
        GCHECK([LastLine(tv) isEqualToString:@"# don't|"]);
        Type(tv, @"\ny = f'");
        GCHECK([LastLine(tv) isEqualToString:@"y = f'|'"]);
        // The third quote of a docstring closes it.
        tv.selectedRange = NSMakeRange(tv.string.length, 0);
        Type(tv, @"\n\"\"\"");
        GCHECK([LastLine(tv) isEqualToString:@"\"\"\"|\"\"\""]);
        // Over a selection: wrapped.
        tv.selectedRange = NSMakeRange(tv.string.length, 0);
        Type(tv, @"\nword");
        tv.selectedRange = NSMakeRange(tv.string.length - 4, 4);
        Type(tv, @"[");
        GCHECK([tv.string hasSuffix:@"\n[word]"]);
        GCHECK(tv.selectedRange.length == 4);
    });

    MCGuiStep(@"two carets close together and type over together", ^{
        const NSUInteger end = tv.string.length;
        tv.selectedRange = NSMakeRange(end, 0);
        Type(tv, @"\na\nb");
        const NSUInteger a = end + 2, b = tv.string.length;
        [tv.multiCarets setCaretRanges:@[[NSValue valueWithRange:NSMakeRange(a, 0)],
                                         [NSValue valueWithRange:NSMakeRange(b, 0)]]];
        Type(tv, @"(");
        GCHECK([tv.string hasSuffix:@"\na()\nb()"]);
        Type(tv, @"1)");
        GCHECK([tv.string hasSuffix:@"\na(1)\nb(1)"]);
        NSArray<NSValue *> *carets = tv.multiCarets.caretRanges;
        GCHECK(carets.count == 2 && carets[1].rangeValue.location == tv.string.length);
        [tv.multiCarets clear];
        tv.selectedRange = NSMakeRange(tv.string.length, 0);
    });

    MCGuiStep(@"nothing closes while an input method composes", ^{
        Type(tv, @"\nz = ");
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv setMarkedText:@"(" selectedRange:NSMakeRange(1, 0)
                replacementRange:NSMakeRange(NSNotFound, 0)];
            GCHECK(tv.hasMarkedText);
            [tv insertText:@"(" replacementRange:NSMakeRange(NSNotFound, 0)];
        });
        GCHECK(!tv.hasMarkedText);
        MCGuiNote(@"after composing (: %@", LastLine(tv));
        GCHECK([LastLine(tv) isEqualToString:@"z = (|"]);
    });

    MCGuiStep(@"Markdown closes brackets, not quotes", ^{
        GCHECK([c showFile:md]);
        tv.selectedRange = NSMakeRange(tv.string.length, 0);
        Type(tv, @"see (");
        GCHECK([LastLine(tv) isEqualToString:@"see (|)"]);
        tv.selectedRange = NSMakeRange(tv.string.length, 0);
        Type(tv, @" it's \"");
        GCHECK([LastLine(tv) isEqualToString:@"see () it's \"|"]);
    });

    MCGuiStep(@"editor.auto-close = off types plainly", ^{
        MCGuiSetSettings(@"lsp.enabled = false\neditor.auto-close = off\n");
        GCHECK([c showFile:py]);
        tv.selectedRange = NSMakeRange(tv.string.length, 0);
        Type(tv, @"\nq(");
        GCHECK([LastLine(tv) isEqualToString:@"q(|"]);
        MCGuiSetSettings(@"lsp.enabled = false\n");
    });

    MCGuiStep(@"save, so no question is left open", ^{
        for (NSString *path in @[ py, md ]) {
            GCHECK([c showFile:path]);
            [c saveCurrentFile:nil];
        }
    });
}
