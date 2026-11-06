// PaletteSuite.mm — `make guitest SUITES=palette`: the palette over the
// window. Shift+Cmd+P lists the menu's commands with their keys, a fuzzy
// query ranks word starts first, Return runs the command and puts the
// keyboard back, and the command comes first next time; Cmd+P lists the
// folder's files and opens one; Ctrl+G goes to a line and column; Esc
// changes nothing; deleting the ">" turns commands into files.
#import "Harness.h"
#import "EditorController.h"
#import "PalettePanel.h"

namespace {
NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
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
NSUInteger LineOf(NSString *text, NSUInteger at) {
    NSUInteger n = 1;
    for (NSUInteger i = 0; i < at && i < text.length; i++) n += [text characterAtIndex:i] == '\n';
    return n;
}
}  // namespace

MC_GUI_SUITE(palette) {
    __block EditorController *c = nil;
    __block BOOL terminalWas = NO;
    NSString *big = MCGuiProjectFile(@"big.txt");
    NSString *deep = MCGuiProjectFile(@"pal/deeper/quick-open-target.md");

    MCGuiStep(@"Shift+Cmd+P: the menu's commands with \">\"", ^{
        MCGuiWriteFile(deep, @"# Target\n");
        c = MCGuiController();
        [c focusEditor:nil];
        GCHECK([c showFile:MCGuiProjectFile(@"README.md")]);
        [c.window makeFirstResponder:Text(c)];
        terminalWas = [[c valueForKey:@"terminalVisible"] boolValue];
        GCHECK(MCGuiPerformMenuKey(c.window, 35, NSEventModifierFlagCommand | NSEventModifierFlagShift,
                                   nil, nil));
        GCHECK(Palette(c) != nil);
        GCHECK_EQ_STR(MCCommandPaletteField(Palette(c)).stringValue, @">");
        GCHECK(c.window.firstResponder == FieldEditor(c));
        NSArray *l = Labels(c);
        GCHECK(l.count > 50);
        GCHECK([l containsObject:@"View > Toggle Terminal"]);
        // Hidden second keys (Cmd+J) are not listed twice.
        NSUInteger n = 0;
        for (NSString *s in l) n += [s isEqualToString:@"View > Toggle Terminal"];
        GCHECK(n == 1);
        NSInteger i = [l indexOfObject:@"View > Toggle Terminal"];
        if (i != NSNotFound) GCHECK_EQ_STR(MCCommandPaletteKeys(Palette(c))[(NSUInteger)i], @"⇧⌘T");
    });
    MCGuiStep(@"\"tog term\" ranks Toggle Terminal first; Return runs it", ^{
        Type(c, @"tog term");
        GCHECK_EQ_STR(Selected(c), @"View > Toggle Terminal");
        Command(c, @selector(insertNewline:));
        GCHECK(Palette(c) == nil);
    });
    MCGuiWait(@"the terminal toggled", 5, ^BOOL {
        return [[c valueForKey:@"terminalVisible"] boolValue] != terminalWas;
    });
    MCGuiStep(@"the keyboard went back; the command is first next time", ^{
        // Toggle Terminal gives the keyboard to the terminal when it opens.
        GCHECK(Palette(c) == nil);
        [c toggleTerminal:nil];   // as it was
        [c focusEditor:nil];
        GCHECK(MCGuiPerformMenuKey(c.window, 35, NSEventModifierFlagCommand | NSEventModifierFlagShift,
                                   nil, nil));
        GCHECK_EQ_STR(Labels(c).firstObject, @"View > Toggle Terminal");
        GCHECK_EQ_STR(Selected(c), @"View > Toggle Terminal");
    });
    MCGuiStep(@"Esc closes it; nothing ran", ^{
        BOOL now = [[c valueForKey:@"terminalVisible"] boolValue];
        Command(c, @selector(cancelOperation:));
        GCHECK(Palette(c) == nil);
        GCHECK([[c valueForKey:@"terminalVisible"] boolValue] == now);
        GCHECK(c.window.firstResponder == Text(c));
    });
    // ------------------------------------------------------- Quick Open
    MCGuiStep(@"Cmd+P: the folder's files", ^{
        GCHECK(MCGuiPerformMenuKey(c.window, 35, NSEventModifierFlagCommand, nil, nil));
        GCHECK(Palette(c) != nil);
        GCHECK_EQ_STR(MCCommandPaletteField(Palette(c)).stringValue, @"");
    });
    MCGuiWait(@"listed", 10, ^BOOL { return MCCommandPaletteFilesListed(Palette(c)); });
    MCGuiStep(@"\"qot\" finds the file by its name's word starts; Return opens it", ^{
        Type(c, @"qot");
        GCHECK([Selected(c) containsString:@"quick-open-target.md"]);
        Command(c, @selector(insertNewline:));
    });
    MCGuiWait(@"opened", 5, ^BOOL { return [[c valueForKey:@"currentPath"] hasSuffix:@"quick-open-target.md"]; });
    MCGuiStep(@"\">\" deleted in the commands lists files", ^{
        GCHECK(MCGuiPerformMenuKey(c.window, 35, NSEventModifierFlagCommand | NSEventModifierFlagShift,
                                   nil, nil));
        Command(c, @selector(deleteBackward:));
        GCHECK_EQ_STR(MCCommandPaletteField(Palette(c)).stringValue, @"");
    });
    MCGuiWait(@"files again, the one just opened first", 10, ^BOOL {
        return MCCommandPaletteFilesListed(Palette(c)) && Labels(c).count > 0;
    });
    MCGuiStep(@"recently opened first", ^{
        GCHECK([Labels(c).firstObject containsString:@"quick-open-target.md"]);
        Command(c, @selector(cancelOperation:));
    });
    // -------------------------------------------------------- Go to Line
    MCGuiStep(@"Ctrl+G \":\", then 120:6", ^{
        GCHECK([c showFile:big]);
        [c.window makeFirstResponder:Text(c)];
        GCHECK(MCGuiPerformMenuKey(c.window, 5, NSEventModifierFlagControl, nil, nil));
        GCHECK_EQ_STR(MCCommandPaletteField(Palette(c)).stringValue, @":");
        Type(c, @"120:6");
        Command(c, @selector(insertNewline:));
        GCHECK(Palette(c) == nil);
    });
    MCGuiStep(@"the caret on line 120, column 6, the keyboard in the editor", ^{
        NSUInteger at = Text(c).selectedRange.location;
        GCHECK(LineOf(Text(c).string, at) == 120);
        NSRange line = [Text(c).string lineRangeForRange:NSMakeRange(at, 0)];
        GCHECK(at - line.location == 5);
        GCHECK(c.window.firstResponder == Text(c));
    });
}
