// SymbolsSuite.mm — `make guitest SUITES=symbols`: Go to Symbol in File
// (Shift+Cmd+O, the palette's "@"), Go to Symbol in Workspace (Cmd+T, "#")
// and Go Back / Go Forward (Ctrl+-, Ctrl+Shift+-). The built-in outline is
// checked everywhere; the language server parts run against basedpyright
// when it is installed and are skipped (with a note) when it is not.
#import "Harness.h"
#import "EditorController.h"
#import "Navigation.h"
#import "PalettePanel.h"
#import "Lsp.h"
#import "Terminal.h"
#import "TerminalGridView.h"
#include <pwd.h>
#include <unistd.h>

namespace {

NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
MCNavigator *Nav(EditorController *c) { return [c valueForKey:@"navigator"]; }
NSView *Palette(EditorController *c) { return MCCommandPaletteIn(c.window); }
NSArray<NSString *> *Labels(EditorController *c) { return MCCommandPaletteLabels(Palette(c)); }
NSString *Selected(EditorController *c) {
    NSInteger i = MCCommandPaletteSelection(Palette(c));
    NSArray *l = Labels(c);
    return i >= 0 && i < (NSInteger)l.count ? l[(NSUInteger)i] : nil;
}
NSTextView *FieldEditor(EditorController *c) {
    return (NSTextView *)MCCommandPaletteField(Palette(c)).currentEditor;
}
void Type(EditorController *c, NSString *s) {
    NSTextView *ed = FieldEditor(c);
    [ed insertText:s replacementRange:ed.selectedRange];
}
void Command(EditorController *c, SEL sel) { [FieldEditor(c) doCommandBySelector:sel]; }

// The range of the first occurrence of `word` on 1-based line `line`.
NSRange WordOnLine(NSString *text, int line, NSString *word) {
    NSUInteger at = 0;
    for (int l = 1; l < line; l++) at = NSMaxRange([text lineRangeForRange:NSMakeRange(at, 0)]);
    NSRange lr = [text lineRangeForRange:NSMakeRange(at, 0)];
    return [text rangeOfString:word options:0 range:lr];
}

NSInteger LineOf(NSString *text, NSUInteger at) {
    NSInteger n = 0;
    for (NSUInteger i = 0; i < at && i < text.length; i++) n += [text characterAtIndex:i] == '\n';
    return n + 1;
}

BOOL ShiftCmdO(EditorController *c) {
    return MCGuiPerformMenuKey(c.window, 31, NSEventModifierFlagCommand | NSEventModifierFlagShift,
                               nil, nil);
}
BOOL CmdT(EditorController *c) {
    return MCGuiPerformMenuKey(c.window, 17, NSEventModifierFlagCommand, nil, nil);
}
BOOL CtrlMinus(EditorController *c) {
    return MCGuiPerformMenuKey(c.window, 27, NSEventModifierFlagControl, nil, nil);
}
BOOL CtrlShiftMinus(EditorController *c) {
    return MCGuiPerformMenuKey(c.window, 27, NSEventModifierFlagControl | NSEventModifierFlagShift,
                               nil, nil);
}

void CollectKeys(NSMenu *menu, NSMutableDictionary<NSString *, NSMutableArray *> *out) {
    for (NSMenuItem *item in menu.itemArray) {
        if (item.submenu) CollectKeys(item.submenu, out);
        if (!item.keyEquivalent.length || item.isAlternate) continue;
        NSEventModifierFlags m = item.keyEquivalentModifierMask &
            (NSEventModifierFlagCommand | NSEventModifierFlagShift | NSEventModifierFlagOption |
             NSEventModifierFlagControl);
        NSString *key = item.keyEquivalent;
        if (![key.lowercaseString isEqualToString:key]) {
            m |= NSEventModifierFlagShift;
            key = key.lowercaseString;
        }
        NSString *k = [NSString stringWithFormat:@"%lu:%@", (unsigned long)m, key];
        if (!out[k]) out[k] = [NSMutableArray array];
        if (![out[k] containsObject:item.title]) [out[k] addObject:item.title];
    }
}

NSString *const kShapes =
    @"\"\"\"Shapes.\"\"\"\n"                        // 1
    @"import math\n"                                // 2
    @"\n"                                           // 3
    @"\n"                                           // 4
    @"class Shape:\n"                               // 5
    @"    def area(self):\n"                        // 6
    @"        return 0\n"                           // 7
    @"\n"                                           // 8
    @"    def describe(self):\n"                    // 9
    @"        return f\"{self.area()}\"\n"          // 10
    @"\n"                                           // 11
    @"\n"                                           // 12
    @"class Circle(Shape):\n"                       // 13
    @"    def __init__(self, r):\n"                 // 14
    @"        self.r = r\n"                         // 15
    @"\n"                                           // 16
    @"    def area(self):\n";                       // 17

NSString *ShapesText() {
    NSMutableString *s = [kShapes mutableCopy];
    [s appendString:@"        return math.pi * self.r ** 2\n"];   // 18
    for (int i = 19; i <= 79; i++) [s appendFormat:@"# filler %d\n", i];
    [s appendString:@"def main():\n"];                            // 80
    [s appendString:@"    c = Circle(2)\n"];
    [s appendString:@"    return c.describe()\n"];
    return s;
}

NSString *BasedPyright() {
    NSMutableArray *dirs = [[NSProcessInfo.processInfo.environment[@"PATH"]
        componentsSeparatedByString:@":"] mutableCopy];
    struct passwd *pw = getpwuid(getuid());   // HOME is the scratch one here
    if (pw) [dirs addObject:[@(pw->pw_dir) stringByAppendingPathComponent:@".local/bin"]];
    for (NSString *d in dirs) {
        NSString *p = [d stringByAppendingPathComponent:@"basedpyright-langserver"];
        if ([[NSFileManager defaultManager] isExecutableFileAtPath:p]) return p;
    }
    return nil;
}

}  // namespace

MC_GUI_SUITE(symbols) {
    __block EditorController *c = nil;
    __block NSTextView *tv = nil;
    __block NSRange start = {0, 0};
    NSString *shapes = MCGuiProjectFile(@"sym/shapes.py");
    NSString *notes = MCGuiProjectFile(@"sym/notes.md");
    NSString *other = MCGuiProjectFile(@"sym/other.txt");
    NSString *rect = MCGuiProjectFile(@"sym/rect.py");

    MCGuiStep(@"files to look through", ^{
        MCGuiWriteFile(shapes, ShapesText());
        NSMutableString *md = [NSMutableString stringWithString:@"# Notes\n\nIntro.\n\n## First\n\n"];
        for (int i = 0; i < 120; i++) [md appendFormat:@"Paragraph %d of the first part.\n\n", i];
        [md appendString:@"## Second\n\nThe end.\n"];
        MCGuiWriteFile(notes, md);
        MCGuiWriteFile(other, @"one\ntwo\n");
        MCGuiWriteFile(rect, @"class Rectangle:\n    def area(self):\n        return 1\n");
        c = MCGuiController();
        tv = Text(c);
        [Nav(c) clear];
        GCHECK([c showFile:shapes]);
        GCHECK_EQ_STR(tv.string, ShapesText());
    });

    MCGuiStep(@"every key is claimed once, and the new ones are free of clashes", ^{
        NSMutableDictionary<NSString *, NSMutableArray *> *keys = [NSMutableDictionary dictionary];
        CollectKeys(NSApp.mainMenu, keys);
        for (NSString *k in keys) {
            NSArray *titles = keys[k];
            if (titles.count > 1) MCGuiNote(@"key %@ is on %@", k, titles);
            GCHECK(titles.count == 1);
        }
        const unsigned long cmd = NSEventModifierFlagCommand, shift = NSEventModifierFlagShift,
                            ctrl = NSEventModifierFlagControl;
        NSArray *o = keys[[NSString stringWithFormat:@"%lu:o", cmd | shift]];
        NSArray *t = keys[[NSString stringWithFormat:@"%lu:t", cmd]];
        NSArray *b = keys[[NSString stringWithFormat:@"%lu:-", ctrl]];
        NSArray *f = keys[[NSString stringWithFormat:@"%lu:-", ctrl | shift]];
        GCHECK([o isEqualToArray:@[@"Go to Symbol in File…"]]);
        GCHECK([t isEqualToArray:@[@"Go to Symbol in Workspace…"]]);
        GCHECK([b isEqualToArray:@[@"Go Back"]]);
        GCHECK([f isEqualToArray:@[@"Go Forward"]]);
    });

    // ------------------------------------------------- the built-in outline
    MCGuiStep(@"Shift+Cmd+O lists the outline, the caret's symbol selected", ^{
        start = NSMakeRange(WordOnLine(tv.string, 15, @"self.r").location, 0);
        [c.window makeFirstResponder:tv];
        tv.selectedRange = start;
        [Nav(c) clear];   // putting the caret there was a far move of its own
        GCHECK(ShiftCmdO(c));
        GCHECK(Palette(c) != nil);
        GCHECK_EQ_STR(MCCommandPaletteField(Palette(c)).stringValue, @"@");
    });
    MCGuiWait(@"the symbols arrive", 5, ^BOOL { return Labels(c).count == 7; });
    MCGuiStep(@"in the file's order, the caret's method selected", ^{
        GCHECK([Labels(c) isEqualToArray:(@[@"Shape", @"area", @"describe", @"Circle",
                                            @"__init__", @"area", @"main"])]);
        GCHECK_EQ_STR(Selected(c), @"__init__");
        GCHECK(NSEqualRanges(tv.selectedRange, start));   // nothing moved yet
        MCGuiSnapshot(Palette(c), @"symbols-outline");
    });
    MCGuiStep(@"Down shows the next symbol in the editor; the keyboard stays", ^{
        Command(c, @selector(moveDown:));
        GCHECK_EQ_STR(Selected(c), @"area");
        GCHECK(NSEqualRanges(tv.selectedRange, WordOnLine(tv.string, 17, @"area")));
        GCHECK(c.window.firstResponder == FieldEditor(c));
        Command(c, @selector(moveDown:));
        GCHECK_EQ_STR(Selected(c), @"main");
        GCHECK(NSEqualRanges(tv.selectedRange, WordOnLine(tv.string, 80, @"main")));
    });
    MCGuiStep(@"Esc puts the editor back and records nothing", ^{
        Command(c, @selector(cancelOperation:));
        GCHECK(Palette(c) == nil);
        GCHECK(NSEqualRanges(tv.selectedRange, start));
        GCHECK(c.window.firstResponder == tv);
        GCHECK(Nav(c).entries.count == 0);
    });

    MCGuiStep(@"typing ranks, Return goes there", ^{
        GCHECK(ShiftCmdO(c));
    });
    MCGuiWait(@"the symbols arrive", 5, ^BOOL { return Labels(c).count == 7; });
    MCGuiStep(@"\"mai\" picks main, previewed; Return", ^{
        Type(c, @"mai");
        GCHECK_EQ_STR(Selected(c), @"main");
        GCHECK(NSEqualRanges(tv.selectedRange, WordOnLine(tv.string, 80, @"main")));
        Command(c, @selector(insertNewline:));
        GCHECK(Palette(c) == nil);
    });
    MCGuiPause(0.2);
    MCGuiStep(@"at main, the keyboard in the editor, the jump recorded", ^{
        GCHECK(NSEqualRanges(tv.selectedRange, WordOnLine(tv.string, 80, @"main")));
        GCHECK(c.window.firstResponder == tv);
        NSArray *e = Nav(c).entries;
        GCHECK(e.count == 2 && [e[0] hasSuffix:@"shapes.py:14"] && [e[1] hasSuffix:@"shapes.py:79"]);
        if (e.count != 2) MCGuiNote(@"entries %@", e);
    });
    MCGuiStep(@"Ctrl+- goes back to where Shift+Cmd+O was pressed, Ctrl+Shift+- forward", ^{
        GCHECK(CtrlMinus(c));
        GCHECK(NSEqualRanges(tv.selectedRange, start));
        GCHECK(CtrlShiftMinus(c));
        GCHECK(NSEqualRanges(tv.selectedRange, WordOnLine(tv.string, 80, @"main")));
        GCHECK(![Nav(c) canGoForward]);
    });

    MCGuiStep(@"\"@:\" groups by kind, headings never selected", ^{
        tv.selectedRange = start;
        GCHECK(ShiftCmdO(c));
        Type(c, @":");
    });
    MCGuiWait(@"grouped", 5, ^BOOL { return Labels(c).count == 10; });
    MCGuiStep(@"classes, methods, functions", ^{
        NSArray *l = Labels(c);
        GCHECK([l[0] isEqualToString:@"classes (2)"] && [l[3] isEqualToString:@"methods (4)"] &&
               [l[8] isEqualToString:@"functions (1)"]);
        if (l.count == 10) MCGuiNote(@"grouped: %@", l);
        GCHECK_EQ_STR(Selected(c), @"__init__");
        Command(c, @selector(moveUp:));   // describe
        Command(c, @selector(moveUp:));   // area, Shape's
        Command(c, @selector(moveUp:));   // over "methods (4)" to Circle
        GCHECK_EQ_STR(Selected(c), @"Circle");
        MCGuiSnapshot(Palette(c), @"symbols-by-kind");
        Command(c, @selector(cancelOperation:));
        GCHECK(NSEqualRanges(tv.selectedRange, start));
    });

    // -------------------------------------------------------- Go Back rules
    MCGuiStep(@"a far caret move is a jump; a near one is not", ^{
        [Nav(c) clear];
        tv.selectedRange = NSMakeRange(WordOnLine(tv.string, 6, @"area").location, 0);
        tv.selectedRange = NSMakeRange(WordOnLine(tv.string, 9, @"describe").location, 0);
        GCHECK(Nav(c).entries.count == 0);
        tv.selectedRange = NSMakeRange(WordOnLine(tv.string, 60, @"filler").location, 0);
        GCHECK(Nav(c).entries.count == 2);
        GCHECK(CtrlMinus(c));
        GCHECK(LineOf(tv.string, tv.selectedRange.location) == 9);
        GCHECK(CtrlShiftMinus(c));
        GCHECK(LineOf(tv.string, tv.selectedRange.location) == 60);
    });
    MCGuiStep(@"typing many lines is no jump", ^{
        tv.selectedRange = NSMakeRange(tv.string.length, 0);
        [Nav(c) clear];
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:@"\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n" replacementRange:tv.selectedRange];
        });
        GCHECK(Nav(c).entries.count == 0);
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, ShapesText());
    });
    MCGuiStep(@"Go to Line and a tab switch are jumps", ^{
        tv.selectedRange = start;
        [Nav(c) clear];
        [c goToLine:70 column:0];
        GCHECK(LineOf(tv.string, tv.selectedRange.location) == 70);
        GCHECK([c showFile:other]);
        GCHECK(Nav(c).entries.count == 3);
        GCHECK(CtrlMinus(c));
        GCHECK([[c valueForKey:@"currentPath"] isEqualToString:shapes]);
        GCHECK(LineOf(tv.string, tv.selectedRange.location) == 70);
        GCHECK(CtrlMinus(c));
        GCHECK(NSEqualRanges(tv.selectedRange, start));
        GCHECK(!CtrlMinus(c) || ![Nav(c) canGoBack]);   // disabled at the start
        GCHECK(CtrlShiftMinus(c) && CtrlShiftMinus(c));
        GCHECK([[c valueForKey:@"currentPath"] isEqualToString:other]);
    });
    MCGuiStep(@"a renamed file is followed, a deleted one skipped", ^{
        NSString *moved = MCGuiProjectFile(@"sym/renamed.txt");
        [[NSFileManager defaultManager] moveItemAtPath:other toPath:moved error:nil];
        [c performSelector:@selector(pathMovedFrom:to:) withObject:other withObject:moved];
        BOOL follows = NO, stale = NO;
        for (NSString *e in Nav(c).entries) {
            follows |= [e containsString:@"renamed.txt"];
            stale |= [e containsString:@"other.txt"];
        }
        GCHECK(follows && !stale);
        // Back to shapes.py, then renamed.txt goes from the disk: Forward
        // has nowhere to go.
        GCHECK(CtrlMinus(c));
        GCHECK([[c valueForKey:@"currentPath"] isEqualToString:shapes]);
        [[NSFileManager defaultManager] removeItemAtPath:moved error:nil];
        [Nav(c) goForward];
        GCHECK([[c valueForKey:@"currentPath"] isEqualToString:shapes]);
        for (NSString *e in Nav(c).entries) GCHECK(![e containsString:@"renamed.txt"]);
    });

    // ------------------------------------------------- a Markdown preview
    MCGuiStep(@"a Markdown preview lists its headings and scrolls to them", ^{
        GCHECK([c showFile:notes]);
        GCHECK([[c valueForKey:@"previewMode"] boolValue]);
        GCHECK(ShiftCmdO(c));
    });
    MCGuiWait(@"the headings", 5, ^BOOL { return Labels(c).count == 3; });
    __block CGFloat top = 0;
    MCGuiStep(@"Notes, First, Second; Down to Second scrolls the page; Esc scrolls back", ^{
        GCHECK([Labels(c) isEqualToArray:(@[@"Notes", @"First", @"Second"])]);
        NSScrollView *sv = tv.enclosingScrollView;
        top = sv.contentView.bounds.origin.y;
        Command(c, @selector(moveDown:));
        Command(c, @selector(moveDown:));
        GCHECK_EQ_STR(Selected(c), @"Second");
        GCHECK(sv.contentView.bounds.origin.y > top + 500);
        Command(c, @selector(cancelOperation:));
        GCHECK(sv.contentView.bounds.origin.y == top);
    });

    // -------------------------------------- the grid terminal keeps Ctrl+-
    __block TerminalView *term = nil;
    MCGuiStep(@"a terminal for cat -v", ^{
        [c showFile:shapes];
        [c focusTerminal:nil];
        term = [c valueForKey:@"terminal"];
        GCHECK(term != nil);
    });
    MCGuiWait(@"its prompt", 15, ^BOOL { return term.atPrompt; });
    MCGuiStep(@"cat -v", ^{ [term runCommand:@"cat -v"]; });
    MCGuiWait(@"cat runs", 10, ^BOOL { return term.busy; });
    MCGuiStep(@"Ctrl+- in the grid reaches the program, not Go Back", ^{
        [term focusInput];
        GCHECK([c.window.firstResponder isKindOfClass:TerminalGridView.class]);
        [Nav(c) clear];
        tv.selectedRange = start;
        NSRange before = tv.selectedRange;
        GCHECK(CtrlMinus(c));   // the grid took it
        GCHECK(NSEqualRanges(tv.selectedRange, before));
        CGEventRef cg = CGEventCreateKeyboardEvent(NULL, 36, true);
        NSEvent *ret = [NSEvent eventWithCGEvent:cg];
        CFRelease(cg);
        [c.window.firstResponder keyDown:ret];
    });
    MCGuiWait(@"cat -v echoes ^_", 5, ^BOOL { return [[term recentText:5] containsString:@"^_"]; });
    MCGuiStep(@"Ctrl+C ends it", ^{
        MCGuiPerformMenuKey(c.window, 8, NSEventModifierFlagControl, nil, nil);
    });
    MCGuiWait(@"the prompt again", 10, ^BOOL { return term.atPrompt && !term.busy; });

    // ------------------------------------------- with a language server
    NSString *server = BasedPyright();
    if (!server) {
        MCGuiStep(@"basedpyright is not installed: the server checks are skipped", ^{
            MCGuiNote(@"basedpyright-langserver not found; skipping the language server checks");
        });
        return;
    }
    __block NSString *settings = NSProcessInfo.processInfo.environment[@"MINICODE_SETTINGS"];
    MCGuiStep(@"basedpyright on", ^{
        MCGuiWriteFile(settings, [NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@ --stdio\nlsp.python.also = off\n", server]);
        [c focusEditor:nil];
    });
    MCGuiPause(1.0);
    MCGuiStep(@"open shapes.py on it", ^{
        [c showFile:rect];
        [c showFile:shapes];
        tv.selectedRange = start;
    });
    MCGuiWait(@"basedpyright runs", 60, ^BOOL {
        LspSession *lsp = [c valueForKey:@"lsp"];
        return lsp.hasServer && [lsp.statusText containsString:@"basedpyright"] &&
               ![lsp.statusText containsString:@"starting"];
    });
    MCGuiStep(@"Shift+Cmd+O asks the server", ^{ GCHECK(ShiftCmdO(c)); });
    MCGuiWait(@"its symbols", 30, ^BOOL { return Labels(c).count >= 7; });
    MCGuiStep(@"the server's symbols, the caret's selected", ^{
        GCHECK_EQ_STR([Palette(c) valueForKey:@"symbolSource"], @"basedpyright");
        NSArray *l = Labels(c);
        MCGuiNote(@"basedpyright's symbols: %@", l);
        GCHECK([l containsObject:@"Circle"] && [l containsObject:@"describe"] &&
               [l containsObject:@"main"]);
        GCHECK_EQ_STR(Selected(c), @"__init__");
        Command(c, @selector(cancelOperation:));
    });
    MCGuiStep(@"Cmd+T for Rect", ^{
        [Nav(c) clear];
        GCHECK(CmdT(c));
        GCHECK_EQ_STR(MCCommandPaletteField(Palette(c)).stringValue, @"#");
        Type(c, @"Rect");
    });
    MCGuiWait(@"Rectangle from the workspace", 30, ^BOOL {
        return [Labels(c) containsObject:@"Rectangle"];
    });
    MCGuiStep(@"Return opens rect.py at it, and Ctrl+- comes back", ^{
        MCGuiSnapshot(Palette(c), @"symbols-workspace");
        NSInteger row = [Labels(c) indexOfObject:@"Rectangle"];
        for (int k = 0; k < 50 && Palette(c) && MCCommandPaletteSelection(Palette(c)) != row; k++)
            Command(c, @selector(moveDown:));
        GCHECK(MCCommandPaletteSelection(Palette(c)) == row);
        Command(c, @selector(insertNewline:));
    });
    MCGuiPause(0.3);
    MCGuiStep(@"at Rectangle; back to shapes.py", ^{
        GCHECK([[c valueForKey:@"currentPath"] isEqualToString:rect]);
        GCHECK([[tv.string substringWithRange:tv.selectedRange] containsString:@"Rectangle"]);
        GCHECK(CtrlMinus(c));
        GCHECK([[c valueForKey:@"currentPath"] isEqualToString:shapes]);
        GCHECK(NSEqualRanges(tv.selectedRange, start));
    });
    MCGuiStep(@"language servers off again", ^{
        MCGuiWriteFile(settings, @"lsp.enabled = false\n");
    });
    MCGuiPause(0.5);
}
