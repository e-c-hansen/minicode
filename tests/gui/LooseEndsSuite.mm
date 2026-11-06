// LooseEndsSuite.mm — smaller checks gathered in one file:
//   make guitest SUITES=terminal_zoom     terminal.font-size, and the zoom
//                                         scaling terminals
#import "Harness.h"
#import "AgentPanel.h"
#import "EditorController.h"
#import "EditorFont.h"
#import "Lsp.h"
#import "MultiCaretView.h"
#import "Terminal.h"
#import "TerminalGridView.h"
#import "AppSettings.h"
#include <sys/stat.h>

namespace {

[[maybe_unused]] inline NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }

[[maybe_unused]] inline double Now() { return NSProcessInfo.processInfo.systemUptime; }

[[maybe_unused]] inline TerminalGridView *Grid(TerminalView *t) { return [t valueForKey:@"grid"]; }

[[maybe_unused]] inline NSString *SettingsPath() { return NSProcessInfo.processInfo.environment[@"MINICODE_SETTINGS"]; }

// Writes the settings file and reads it at once.
[[maybe_unused]] inline void SetSettings(NSString *text) {
    MCGuiWriteFile(SettingsPath(), text);
    [[AppSettings shared] reload];
}

} // namespace

// A holder the steps of a suite share (a block's __block variables move
// when the block is copied, so their addresses cannot be handed around).
@interface MCLooseState : NSObject
@property(nonatomic, strong) EditorController *c;
@property(nonatomic, strong) TerminalView *term;
@property(nonatomic, strong) NSMutableData *sent;
@property(nonatomic) NSSize cell;
@property(nonatomic) int cols;
@end
@implementation MCLooseState
@end

namespace {

// The terminal on show, at its prompt, with every byte its view sends to
// the program recorded in s.sent.
[[maybe_unused]] inline void TerminalAtPrompt(MCLooseState *s) {
    MCGuiStep(@"a terminal", ^{
        s.c = MCGuiController();
        [s.c focusTerminal:nil];
        s.term = [s.c valueForKey:@"terminal"];
        GCHECK(s.term != nil);
        s.sent = [NSMutableData data];
        TerminalGridView *g = Grid(s.term);
        void (^forward)(NSData *) = g.onInput;
        g.onInput = ^(NSData *d) {
            [s.sent appendData:d];
            if (forward) forward(d);
        };
    });
    MCGuiWait(@"its prompt", 20, ^BOOL { return s.term.atPrompt && !s.term.busy; });
}

[[maybe_unused]] inline int Cols(TerminalView *t) { return Grid(t).screen ? Grid(t).screen->cols() : 0; }

} // namespace

// ------------------------------------------------------------ the zoom
MC_GUI_SUITE(terminal_zoom) {
    MCLooseState *s = [MCLooseState new];
    TerminalAtPrompt(s);
    MCGuiStep(@"at 12 points with no zoom", ^{
        MCEditorZoomBy(0);
        GCHECK(MCTerminalFontSize() == 12);
        GCHECK([TerminalGridView cellFont].pointSize == 12);
        NSTextField *input = [s.term valueForKey:@"input"];
        GCHECK(input.font.pointSize == 12);
        s.cell = [TerminalGridView cellSize];
        s.cols = Cols(s.term);
        MCGuiNote(@"12 pt: cell %.2f x %.0f, %d columns", s.cell.width, s.cell.height, s.cols);
        GCHECK(s.cols > 20);
    });
    MCGuiStep(@"Cmd+= twice through the menu", ^{
        GCHECK(MCGuiPerformMenuKey(s.c.window, 24, NSEventModifierFlagCommand, @"=", @"="));
        GCHECK(MCGuiPerformMenuKey(s.c.window, 24, NSEventModifierFlagCommand, nil, nil));
    });
    MCGuiStep(@"the terminal is at 14 points, with fewer columns", ^{
        GCHECK(MCEditorZoom() == 2);
        GCHECK(MCTerminalFontSize() == 14);
        GCHECK([TerminalGridView cellFont].pointSize == 14);
        GCHECK(MCTerminalFont(YES, NO).pointSize == 14);
        NSTextField *input = [s.term valueForKey:@"input"];
        GCHECK(input.font.pointSize == 14);
        NSSize cell = [TerminalGridView cellSize];
        GCHECK(cell.width > s.cell.width && cell.height > s.cell.height);
        MCGuiNote(@"14 pt: cell %.2f x %.0f, %d columns", cell.width, cell.height, Cols(s.term));
        GCHECK(Cols(s.term) < s.cols);
        [s.term runCommand:@"echo cols=$(tput cols)"];
    });
    MCGuiWait(@"the program sees the new width", 10, ^BOOL {
        return s.term.atPrompt && [[s.term recentText:4] containsString:
            [NSString stringWithFormat:@"cols=%d", Cols(s.term)]];
    });
    MCGuiStep(@"Option+Cmd+0 resets the terminal", ^{
        GCHECK(MCGuiPerformMenuKey(s.c.window, 29, NSEventModifierFlagCommand | NSEventModifierFlagOption,
                                   nil, nil));
        GCHECK(MCEditorZoom() == 0);
        GCHECK([TerminalGridView cellFont].pointSize == 12);
        GCHECK(Cols(s.term) == s.cols);
    });
    // terminal.font-size, and a line-mode terminal.
    MCGuiStep(@"terminal.font-size = 16 and terminal.mode = line", ^{
        SetSettings(@"lsp.enabled = false\nterminal.font-size = 16\nterminal.mode = line\n");
        GCHECK(MCTerminalFontSize() == 16);
        GCHECK([TerminalGridView cellFont].pointSize == 16);
        NSTextField *input = [s.term valueForKey:@"input"];
        GCHECK(input.font.pointSize == 16);
        GCHECK(Cols(s.term) < s.cols);
        [s.c newTerminal:nil];
        s.term = [s.c valueForKey:@"terminal"];
        GCHECK(s.term.lineMode);
    });
    MCGuiWait(@"the line-mode terminal's prompt", 20, ^BOOL { return s.term.atPrompt; });
    MCGuiStep(@"its output", ^{ [s.term runCommand:@"echo line-mode-output"]; });
    MCGuiWait(@"printed", 10, ^BOOL {
        return s.term.atPrompt && [[s.term recentText:6] containsString:@"line-mode-output"];
    });
    MCGuiStep(@"zoomed in: the log, the input line and the pty follow", ^{
        NSTextView *out = [s.term valueForKey:@"output"];
        NSRange r = [out.string rangeOfString:@"line-mode-output" options:NSBackwardsSearch];
        GCHECK(r.location != NSNotFound);
        if (r.location == NSNotFound) return;
        NSFont *before = [out.textStorage attribute:NSFontAttributeName atIndex:r.location
                                     effectiveRange:NULL];
        GCHECK(before.pointSize == 16);
        const int cols = Cols(s.term);
        MCEditorZoomBy(3);
        NSFont *after = [out.textStorage attribute:NSFontAttributeName atIndex:r.location
                                    effectiveRange:NULL];
        GCHECK(after.pointSize == 19);
        NSTextField *input = [s.term valueForKey:@"input"];
        GCHECK(input.font.pointSize == 19);
        GCHECK(Cols(s.term) < cols);
        [s.term runCommand:@"echo cols=$(tput cols)"];
    });
    MCGuiWait(@"the shell sees the new width", 10, ^BOOL {
        return s.term.atPrompt && [[s.term recentText:4] containsString:
            [NSString stringWithFormat:@"cols=%d", Cols(s.term)]];
    });
    MCGuiStep(@"back to the defaults", ^{
        MCEditorZoomBy(0);
        [[s.c valueForKey:@"terminalPanel"] closeTerminal:s.term];
        SetSettings(@"lsp.enabled = false\n");
        GCHECK(MCTerminalFontSize() == 12);
        s.term = [s.c valueForKey:@"terminal"];
        GCHECK(!s.term.lineMode);
        GCHECK(Cols(s.term) == s.cols);
    });
}
