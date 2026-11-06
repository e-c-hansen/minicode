// TerminalMarksSuite.mm — `make guitest SUITES=terminal_marks`: moving
// between the commands the shell integration marked, in a real zsh.
//   - Three commands run; Cmd+Up lands on each one's prompt in turn (the
//     prompt at the top of the view as far as the scrollback allows), Cmd+Down
//     comes back and past the newest returns to the bottom.
//   - Shift+Cmd+Up from the bottom selects the last command and its output,
//     a second press takes the one before too.
//   - `false` gets the red bar beside its prompt (pixels), `true` none.
//   - Terminal > Copy Last Command Output copies what the last one printed.
//   - Cmd+Up reaches the grid: no menu item and no key equivalent takes it.
#import "Harness.h"
#import "EditorController.h"
#import "Terminal.h"
#import "TerminalGridView.h"
#import "TerminalPanel.h"
#import <Carbon/Carbon.h>   // kVK_*
#include <climits>

namespace {

TerminalGridView *Grid(TerminalView *t) { return [t valueForKey:@"grid"]; }

// Commands that ran and finished, per the screen's marks.
int Ended(TerminalView *t) {
    int n = 0;
    for (const TermCommand &c : Grid(t).screen->commands()) n += c.ended;
    return n;
}

std::vector<TermCommand> Ran(TerminalView *t) {
    std::vector<TermCommand> out;
    for (const TermCommand &c : Grid(t).screen->commands())
        if (c.ran) out.push_back(c);
    return out;
}

// Cmd+Up or Cmd+Down (with Shift), made as the hardware makes it, to the
// grid's keyDown: (an offscreen window delivers no key events itself).
void Arrow(TerminalView *t, BOOL up, BOOL shift) {
    CGEventRef ce = CGEventCreateKeyboardEvent(NULL, up ? kVK_UpArrow : kVK_DownArrow, true);
    CGEventSetFlags(ce, (CGEventFlags)(kCGEventFlagMaskCommand |
                                       (shift ? kCGEventFlagMaskShift : 0)));
    NSEvent *e = [NSEvent eventWithCGEvent:ce];
    CFRelease(ce);
    [Grid(t) keyDown:e];
}

// The color at a point of the grid as drawn (points from the top left).
NSColor *PixelAt(TerminalGridView *g, CGFloat x, CGFloat y) {
    NSBitmapImageRep *rep = [g bitmapImageRepForCachingDisplayInRect:g.bounds];
    [g cacheDisplayInRect:g.bounds toBitmapImageRep:rep];
    CGFloat scale = rep.pixelsWide / NSWidth(g.bounds);
    return [[rep colorAtX:(NSInteger)(x * scale) y:(NSInteger)(y * scale)]
        colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
}

BOOL Red(NSColor *c) {
    return c && c.redComponent > 0.75 && c.greenComponent < 0.5 && c.blueComponent < 0.5;
}

}  // namespace

@interface MCMarksState : NSObject
@property(nonatomic, strong) EditorController *c;
@property(nonatomic, strong) TerminalView *term;
@end
@implementation MCMarksState
@end

MC_GUI_SUITE(terminal_marks) {
    MCMarksState *s = [MCMarksState new];
    MCGuiStep(@"a terminal of its own, in the project folder", ^{
        MCGuiSetSettings(@"lsp.enabled = false\n");
        s.c = MCGuiController();
        [s.c newTerminal:nil];
        s.term = [s.c valueForKey:@"terminal"];
        GCHECK(s.term != nil && !s.term.lineMode);
    });
    MCGuiWait(@"its prompt", 20, ^BOOL { return s.term.atPrompt && !s.term.busy; });
    MCGuiStep(@"cd", ^{
        [s.term runCommand:[NSString stringWithFormat:@"cd '%@'", MCGuiProject()]];
    });
    MCGuiWait(@"there", 10, ^BOOL { return Ended(s.term) == 1 && s.term.atPrompt; });
    MCGuiStep(@"true", ^{ [s.term runCommand:@"true"]; });
    MCGuiWait(@"true ran", 10, ^BOOL { return Ended(s.term) == 2 && s.term.atPrompt; });
    MCGuiStep(@"false", ^{ [s.term runCommand:@"false"]; });
    MCGuiWait(@"false ran", 10, ^BOOL { return Ended(s.term) == 3 && s.term.atPrompt; });
    MCGuiStep(@"a long output", ^{ [s.term runCommand:@"seq 1 80"]; });
    MCGuiWait(@"seq ran", 10, ^BOOL {
        return Ended(s.term) == 4 && s.term.atPrompt &&
               [[s.term valueForKey:@"zleReady"] boolValue];
    });

    MCGuiStep(@"the marks: four commands ran, the prompt waits", ^{
        std::vector<TermCommand> all = Grid(s.term).screen->commands();
        GCHECK(all.size() >= 5 && !all.back().ran);
        std::vector<TermCommand> ran = Ran(s.term);
        GCHECK(ran.size() == 4);
        if (ran.size() == 4) {
            GCHECK(ran[1].status == 0 && ran[2].status == 1 && ran[3].status == 0);
            GCHECK([[NSString stringWithUTF8String:Grid(s.term).screen->lineText(ran[3].promptLine).c_str()]
                hasSuffix:@"seq 1 80"]);
            // seq's 80 lines put the earlier prompts in the scrollback.
            GCHECK(ran[2].promptLine < 0);
        }
        GCHECK(Grid(s.term).currentCommandLine == INT_MIN && Grid(s.term).scrollOffset == 0);
    });

    MCGuiStep(@"Cmd+Up is no menu key: it reaches the grid", ^{
        [s.c.window makeFirstResponder:Grid(s.term)];
        GCHECK(!MCGuiPerformMenuKey(s.c.window, kVK_UpArrow, NSEventModifierFlagCommand, nil, nil));
        GCHECK(!MCGuiPerformMenuKey(s.c.window, kVK_DownArrow,
                                    NSEventModifierFlagCommand | NSEventModifierFlagShift, nil, nil));
    });

    MCGuiStep(@"Cmd+Up: each command in turn, its prompt at the top", ^{
        std::vector<TermCommand> ran = Ran(s.term);
        if (ran.size() != 4) { GCHECK(NO); return; }
        TerminalGridView *g = Grid(s.term);
        for (int i = 3; i >= 0; i--) {
            Arrow(s.term, YES, NO);
            GCHECK(g.currentCommandLine == ran[i].promptLine);
            GCHECK(g.scrollOffset == MAX(0, MIN(-ran[i].promptLine, g.screen->historySize())));
            GCHECK(g.selectedText == nil);
        }
        // Nothing above the first: it stays.
        Arrow(s.term, YES, NO);
        GCHECK(g.currentCommandLine == ran[0].promptLine);
        // And back down, then past the newest to the bottom.
        Arrow(s.term, NO, NO);
        GCHECK(g.currentCommandLine == ran[1].promptLine);
        Arrow(s.term, NO, NO);
        GCHECK(g.currentCommandLine == ran[2].promptLine);
    });

    MCGuiStep(@"false's prompt at the top has the red bar; true's has none", ^{
        TerminalGridView *g = Grid(s.term);
        NSSize cs = [TerminalGridView cellSize];
        CGFloat pad = [TerminalGridView padding];
        // false's prompt is the top row now; true's is the row above it,
        // out of view, so look at it through one more press.
        NSColor *bar = PixelAt(g, 2.5, pad + cs.height / 2);
        GCHECK(Red(bar));
        if (!Red(bar)) MCGuiNote(@"the margin beside false's prompt is %@", bar);
        // A row of seq's output beside it: no bar.
        NSColor *plain = PixelAt(g, 2.5, pad + cs.height * 2.5);
        GCHECK(!Red(plain));
        MCGuiSnapshot(g, @"terminal-marks-failed");
        Arrow(s.term, YES, NO);
        GCHECK(!Red(PixelAt(g, 2.5, pad + cs.height / 2)));
    });

    MCGuiStep(@"past the newest: the bottom again", ^{
        TerminalGridView *g = Grid(s.term);
        for (int i = 0; i < 3; i++) Arrow(s.term, NO, NO);
        GCHECK(g.currentCommandLine == INT_MIN && g.scrollOffset == 0);
    });

    MCGuiStep(@"Shift+Cmd+Up selects the last command and its output", ^{
        TerminalGridView *g = Grid(s.term);
        Arrow(s.term, YES, YES);
        NSString *sel = g.selectedText;
        GCHECK([sel containsString:@"seq 1 80\n1\n2\n"]);
        GCHECK([sel hasSuffix:@"\n79\n80"]);
        GCHECK(![sel containsString:@"false"]);
        // Again: the command before it joins.
        Arrow(s.term, YES, YES);
        sel = g.selectedText;
        GCHECK([sel containsString:@"false\n"] && [sel hasSuffix:@"\n80"]);
        GCHECK(![sel containsString:@"true"]);
        // Shift+Cmd+Down gives it back.
        Arrow(s.term, NO, YES);
        GCHECK(![g.selectedText containsString:@"false"] && [g.selectedText hasSuffix:@"\n80"]);
        // A plain Cmd+Up ends the selection.
        Arrow(s.term, YES, NO);
        GCHECK(g.selectedText == nil);
    });

    MCGuiStep(@"Copy Last Command Output", ^{
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Copy Last Command Output"
                                                      action:@selector(copyLastCommandOutput:)
                                               keyEquivalent:@""];
        GCHECK([(id<NSMenuItemValidation>)s.term validateMenuItem:item]);
        [[NSPasteboard generalPasteboard] clearContents];
        [NSApp sendAction:@selector(copyLastCommandOutput:) to:s.term from:item];
        NSString *copied = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        NSMutableArray *want = [NSMutableArray array];
        for (int i = 1; i <= 80; i++) [want addObject:[NSString stringWithFormat:@"%d", i]];
        GCHECK_EQ_STR(copied, [want componentsJoinedByString:@"\n"]);
        // The menu has it, in the Terminal menu.
        NSMenuItem *terminalMenu = [NSApp.mainMenu itemWithTitle:@"Terminal"];
        GCHECK([terminalMenu.submenu indexOfItemWithTarget:nil
                                                 andAction:@selector(copyLastCommandOutput:)] >= 0);
    });

    MCGuiStep(@"typing goes back to the bottom; the terminal closes", ^{
        TerminalGridView *g = Grid(s.term);
        Arrow(s.term, YES, NO);
        GCHECK(g.currentCommandLine != INT_MIN);
        [g insertText:@"x" replacementRange:NSMakeRange(NSNotFound, 0)];
        GCHECK(g.currentCommandLine == INT_MIN && g.scrollOffset == 0);
        GCHECK([(MCTerminalPanel *)[s.c valueForKey:@"terminalPanel"] closeTerminal:s.term]);
    });
}
