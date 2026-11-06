// TerminalReflowSuite.mm — `make guitest SUITES=terminal_reflow`: the grid
// terminal reflowing on a resize (a scrolled-back view staying on its
// text), rows it wrapped copying, finding and Cmd-clicking as one line,
// mouse reporting to vim (click, wheel, Shift to select instead), focus
// events, a program's title on its tab, and mouse modes a program left on
// going when the prompt comes back. Real zsh and /usr/bin/vim.
#import "Harness.h"
#import "EditorController.h"
#import "Terminal.h"
#import "TerminalGridView.h"

namespace {

TerminalGridView *Grid(TerminalView *t) { return [t valueForKey:@"grid"]; }
TerminalScreen *Screen(TerminalView *t) { return Grid(t).screen; }

// The window point at the middle of a cell of the view.
NSPoint CellPoint(TerminalGridView *g, int row, int col) {
    NSSize cs = [TerminalGridView cellSize];
    CGFloat pad = [TerminalGridView padding];
    NSPoint p = NSMakePoint(pad + (col + 0.5) * cs.width, pad + (row + 0.5) * cs.height);
    return [g convertPoint:p toView:nil];
}

NSEvent *Mouse(TerminalGridView *g, NSEventType type, int row, int col,
               NSEventModifierFlags flags, NSInteger clicks) {
    return [NSEvent mouseEventWithType:type location:CellPoint(g, row, col) modifierFlags:flags
                             timestamp:NSProcessInfo.processInfo.systemUptime
                          windowNumber:g.window.windowNumber context:nil eventNumber:0
                            clickCount:clicks pressure:1];
}

void Click(TerminalGridView *g, int row, int col, NSEventModifierFlags flags, NSInteger clicks) {
    [g mouseDown:Mouse(g, NSEventTypeLeftMouseDown, row, col, flags, clicks)];
    [g mouseUp:Mouse(g, NSEventTypeLeftMouseUp, row, col, flags, clicks)];
}

// A wheel notch: positive scrolls up (back), negative down.
void Wheel(TerminalGridView *g, int lines, NSEventModifierFlags flags) {
    CGEventRef cg = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 1, lines);
    CGEventSetFlags(cg, (CGEventFlags)flags);
    NSEvent *e = [NSEvent eventWithCGEvent:cg];
    CFRelease(cg);
    [g scrollWheel:e];
}

// The first screen row whose text starts with `prefix`, or -1.
int RowStarting(TerminalScreen *s, NSString *prefix) {
    for (int r = 0; r < s->rows(); r++)
        if ([@(s->lineText(r).c_str()) hasPrefix:prefix]) return r;
    return -1;
}

void Send(TerminalGridView *g, NSString *s) {
    NSData *d = [s dataUsingEncoding:NSUTF8StringEncoding];
    g.onInput(d);
}

NSString *Repeat(NSString *s, int n) {
    return [@"" stringByPaddingToLength:s.length * n withString:s startingAtIndex:0];
}

} // namespace

MC_GUI_SUITE(terminal_reflow) {
    __block EditorController *c = nil;
    __block TerminalView *term = nil;
    __block TerminalGridView *grid = nil;
    __block NSMutableData *sent = [NSMutableData data];
    __block NSRect wide = NSZeroRect;
    __block int wideCols = 0;
    __block NSString *topText = nil;
    NSString *bees = Repeat(@"B", 250);
    // A path longer than the terminal is wide, made first so the file tree
    // has it by the time it is Cmd-clicked.
    NSMutableString *longPath = [NSMutableString stringWithString:@"deep"];
    while (longPath.length < 160) [longPath appendString:@"/folder"];
    [longPath appendString:@"/target.txt"];
    NSString *path = longPath;

    MCGuiStep(@"a grid terminal", ^{
        MCGuiWriteFile(MCGuiProjectFile(path), @"one\ntwo\nthree\n");
        c = MCGuiController();
        wide = c.window.frame;
        if (wide.size.width < 1100) {
            wide.size.width = 1100;
            [c.window setFrame:wide display:NO];
        }
        [c focusTerminal:nil];
        term = [c valueForKey:@"terminal"];
        grid = Grid(term);
        GCHECK(term && grid && !term.lineMode);
        // Every byte the view sends to the program, recorded on its way.
        void (^forward)(NSData *) = grid.onInput;
        grid.onInput = ^(NSData *d) {
            [sent appendData:d];
            forward(d);
        };
    });
    MCGuiWait(@"its prompt", 20, ^BOOL { return term.atPrompt; });
    // Other suites may have left this terminal elsewhere.
    MCGuiStep(@"in the project folder", ^{
        [term runCommand:[NSString stringWithFormat:@"cd '%@'", MCGuiProject()]];
    });
    MCGuiWait(@"there", 10, ^BOOL {
        return term.atPrompt && [term.directory.stringByResolvingSymlinksInPath
                                    isEqualToString:MCGuiProject().stringByResolvingSymlinksInPath];
    });

    // -------------------------------------------------------------- reflow
    MCGuiStep(@"300 short lines and a 250-character one", ^{
        [term runCommand:@"seq -f 'row %g' 1 300; printf 'B%.0s' {1..250}; echo; echo the-end"];
    });
    MCGuiWait(@"they are printed", 20, ^BOOL {
        return term.atPrompt && [[term recentText:5] containsString:@"the-end"];
    });
    MCGuiStep(@"the long line wrapped and reads back whole", ^{
        TerminalScreen *s = Screen(term);
        wideCols = s->cols();
        MCGuiNote(@"terminal %dx%d", s->cols(), s->rows());
        GCHECK(wideCols < 250);
        GCHECK([[term recentText:5] containsString:bees]);
        int r = RowStarting(s, @"BBB");
        GCHECK(r >= 0 && s->lineWraps(r));
    });
    MCGuiStep(@"scroll back with the wheel", ^{
        for (int i = 0; i < 30; i++) Wheel(grid, 1, 0);
        TerminalScreen *s = Screen(term);
        GCHECK(grid.scrollOffset >= 60);
        topText = @(s->lineText(-grid.scrollOffset).c_str());
        MCGuiNote(@"top line scrolled back: %@", topText);
        GCHECK([topText hasPrefix:@"row "]);
    });
    MCGuiStep(@"a narrower window reflows and keeps the view on its text", ^{
        NSRect f = wide;
        f.size.width = 640;
        [c.window setFrame:f display:NO];
        [c.window.contentView layoutSubtreeIfNeeded];
    });
    MCGuiWait(@"the terminal is narrower", 5, ^BOOL { return Screen(term)->cols() < wideCols; });
    MCGuiStep(@"the scrolled-back view shows the same line at its top", ^{
        TerminalScreen *s = Screen(term);
        GCHECK(grid.scrollOffset > 0);
        GCHECK_EQ_STR(@(s->lineText(-grid.scrollOffset).c_str()), topText);
        GCHECK([[term recentText:5] containsString:bees]);   // still one line
        [grid scrollToBottom];
    });
    MCGuiStep(@"the program sees the new width", ^{ [term runCommand:@"echo cols=$(tput cols)"]; });
    MCGuiWait(@"tput answers", 10, ^BOOL {
        NSString *want = [NSString stringWithFormat:@"cols=%d", Screen(term)->cols()];
        return term.atPrompt && [[term recentText:3] containsString:want];
    });

    // ------------------------------------------ one line over its wraps
    MCGuiStep(@"a double click on a wrapped row takes the whole word", ^{
        TerminalScreen *s = Screen(term);
        int r = RowStarting(s, @"BBB");
        if (r < 0) {   // scrolled off: bring it back
            for (int i = 0; i < 4 && RowStarting(s, @"BBB") < 0; i++) Wheel(grid, 1, 0);
            r = -1;
            for (int row = 0; row < s->rows(); row++)
                if (s->lineText(-grid.scrollOffset + row).rfind("BBB", 0) == 0) { r = row; break; }
        }
        GCHECK(r >= 0);
        if (r < 0) return;
        Click(grid, r + 1, 3, 0, 1);       // the second row of it
        Click(grid, r + 1, 3, 0, 2);
        GCHECK_EQ_STR(grid.selectedText, bees);
        Click(grid, r, 0, 0, 3);           // a triple click: the whole line
        GCHECK_EQ_STR(grid.selectedText, bees);
        [grid scrollToBottom];
    });
    MCGuiStep(@"a long path printed so that it wraps", ^{
        [term runCommand:[NSString stringWithFormat:@"clear; echo %@:2", path]];
    });
    MCGuiWait(@"it is printed", 10, ^BOOL {
        return term.atPrompt && [[term recentText:4] containsString:[path stringByAppendingString:@":2"]];
    });
    MCGuiStep(@"find matches it over the wrap", ^{
        [grid findText:path matchCase:YES];
        GCHECK(grid.findCount >= 1);
        [grid endFind];
    });
    MCGuiStep(@"Cmd-click on its second row opens the file", ^{
        TerminalScreen *s = Screen(term);
        int r = RowStarting(s, @"deep/folder");
        int out = -1;   // the echoed line, not the command line
        for (int row = 0; row < s->rows(); row++)
            if (s->lineText(row).rfind("deep/folder", 0) == 0) out = row;
        (void)r;
        GCHECK(out >= 0 && s->lineWraps(out));
        if (out < 0) return;
        [grid mouseDown:Mouse(grid, NSEventTypeLeftMouseDown, out + 1, 2,
                              NSEventModifierFlagCommand, 1)];
        [grid mouseUp:Mouse(grid, NSEventTypeLeftMouseUp, out + 1, 2, NSEventModifierFlagCommand, 1)];
    });
    MCGuiWait(@"target.txt is open", 5, ^BOOL {
        return [[c valueForKey:@"currentPath"] hasSuffix:@"/target.txt"];
    });

    // ------------------------------------------------------ program titles
    MCGuiStep(@"a program's title names the tab while it runs", ^{
        [term focusInput];
        [term runCommand:@"printf '\\e]2;Reflow Title\\a'; sleep 2"];
    });
    MCGuiWait(@"the title shows", 10, ^BOOL { return [term.title isEqualToString:@"Reflow Title"]; });
    MCGuiWait(@"and goes at the prompt", 10, ^BOOL {
        return term.atPrompt && ![term.title isEqualToString:@"Reflow Title"];
    });
    MCGuiStep(@"the shell's own title at its prompt does not count", ^{
        [term runCommand:@"precmd() { printf '\\e]2;shell title\\a' }"];
    });
    MCGuiWait(@"a prompt or two later", 10, ^BOOL { return term.atPrompt; });
    MCGuiStep(@"checked", ^{
        GCHECK(![term.title isEqualToString:@"shell title"]);
        [term runCommand:@"unset -f precmd"];
    });
    MCGuiWait(@"the prompt", 10, ^BOOL { return term.atPrompt; });

    // ------------------------------------------- mouse modes left on
    MCGuiStep(@"a program that leaves mouse reporting on", ^{
        [term runCommand:@"printf '\\e[?1002h\\e[?1006h\\e[?1004h'; sleep 0.3"];
    });
    MCGuiWait(@"it ran", 10, ^BOOL { return term.busy; });
    MCGuiWait(@"the prompt turns it off", 10, ^BOOL {
        TerminalScreen *s = Screen(term);
        return term.atPrompt && s->mouseTracking() == 0 && !s->focusEvents();
    });

    // ------------------------------------------------------- focus events
    MCGuiStep(@"cat -v after asking for focus events", ^{
        [term runCommand:@"printf '\\e[?1004h'; cat -v"];
    });
    MCGuiWait(@"focus events on", 10, ^BOOL { return term.busy && Screen(term)->focusEvents(); });
    MCGuiStep(@"the keyboard leaves the terminal", ^{
        [c.window makeFirstResponder:[c valueForKey:@"textView"]];
    });
    MCGuiWait(@"cat shows the focus-out report", 5, ^BOOL {
        return [[term recentText:3] containsString:@"^[[O"];
    });
    MCGuiStep(@"and comes back", ^{ [term focusInput]; });
    MCGuiWait(@"cat shows the focus-in report", 5, ^BOOL {
        return [[term recentText:3] containsString:@"^[[I"];
    });
    MCGuiStep(@"Ctrl+C", ^{ Send(grid, @"\x03"); });
    MCGuiWait(@"the prompt", 10, ^BOOL { return term.atPrompt && !term.busy; });

    // ------------------------------------------------------- vim, mouse=a
    NSString *file = MCGuiProjectFile(@"mouse.txt");
    MCGuiStep(@"vim with mouse=a on a file of numbered lines", ^{
        NSMutableString *text = [NSMutableString string];
        for (int i = 1; i <= 80; i++) [text appendFormat:@"line %02d abcdef\n", i];
        MCGuiWriteFile(file, text);
        [term runCommand:@"vim -u NONE -N -i NONE -c 'set mouse=a' mouse.txt"];
    });
    MCGuiWait(@"vim asks for the mouse", 15, ^BOOL {
        TerminalScreen *s = Screen(term);
        return s->altScreen() && s->mouseTracking() >= 1000 && RowStarting(s, @"line 01") == 0;
    });
    MCGuiStep(@"Shift-drag selects instead of reaching vim", ^{
        TerminalScreen *s = Screen(term);
        MCGuiNote(@"vim's mouse mode %d, encoding %d", s->mouseTracking(), s->mouseEncoding());
        NSUInteger before = sent.length;
        NSEventModifierFlags shift = NSEventModifierFlagShift;
        [grid mouseDown:Mouse(grid, NSEventTypeLeftMouseDown, 1, 0, shift, 1)];
        [grid mouseDragged:Mouse(grid, NSEventTypeLeftMouseDragged, 1, 6, shift, 1)];
        [grid mouseUp:Mouse(grid, NSEventTypeLeftMouseUp, 1, 6, shift, 1)];
        GCHECK(sent.length == before);
        GCHECK_EQ_STR(grid.selectedText, @"line 02");
    });
    MCGuiStep(@"a click puts vim's cursor there, and x deletes under it", ^{
        NSUInteger before = sent.length;
        Click(grid, 4, 5, 0, 1);           // "line 05": its '0'
        GCHECK(sent.length > before);
        GCHECK(grid.selectedText == nil);
    });
    MCGuiPause(0.3);
    MCGuiStep(@"x", ^{ Send(grid, @"x"); });
    MCGuiWait(@"line 05 lost its 0", 5, ^BOOL {
        TerminalScreen *s = Screen(term);
        return s->lineText(4) == "line 5 abcdef";
    });
    MCGuiStep(@"the wheel scrolls vim", ^{
        NSUInteger before = sent.length;
        Wheel(grid, -1, 0);
        GCHECK(sent.length > before);
        GCHECK(grid.scrollOffset == 0);
    });
    MCGuiWait(@"vim scrolled down", 5, ^BOOL {
        return RowStarting(Screen(term), @"line 01") < 0;
    });
    MCGuiStep(@"save and quit", ^{ Send(grid, @"\x1b:wq\r"); });
    MCGuiWait(@"back at the prompt, mouse off", 10, ^BOOL {
        return term.atPrompt && !Screen(term)->altScreen() && Screen(term)->mouseTracking() == 0;
    });
    MCGuiStep(@"vim wrote what the click and x did", ^{
        NSString *text = [NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:nil];
        NSArray *lines = [text componentsSeparatedByString:@"\n"];
        GCHECK(lines.count > 5);
        if (lines.count > 5) GCHECK_EQ_STR(lines[4], @"line 5 abcdef");
        if (lines.count > 5) GCHECK_EQ_STR(lines[3], @"line 04 abcdef");
    });
    MCGuiStep(@"the window back to its width", ^{ [c.window setFrame:wide display:NO]; });
}
