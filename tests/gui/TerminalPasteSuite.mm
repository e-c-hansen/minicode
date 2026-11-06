// TerminalPasteSuite.mm — `make guitest SUITES=terminal_paste`: pasting
// into the grid terminal and typing a password there.
//   - A bracketed paste cannot end its bracket early: the nested end mark
//     ("ESC[20" + "ESC[201~" + "1~") pasted at zsh's prompt leaves the
//     command in the line editor instead of running it.
//   - Several lines into a program that did not turn bracketed paste on
//     (cat) ask first, as VS Code does: Cancel sends nothing, Paste as One
//     Line takes the breaks out, Paste sends them; a command with one
//     trailing break goes in without it; terminal.multiline-paste-warning
//     = never and always.
//   - Secure keyboard entry while `read -s` reads with echo off and the grid
//     has the keyboard, off when the keyboard leaves and when echo returns.
// The nonce after `exec bash` is checked in terminal_exec (LooseEndsSuite).
#import "Harness.h"
#import "EditorController.h"
#import "Terminal.h"
#import "TerminalGridView.h"
#import "TerminalPanel.h"
#import <Carbon/Carbon.h>   // IsSecureEventInputEnabled

namespace {

TerminalGridView *Grid(TerminalView *t) { return [t valueForKey:@"grid"]; }

void Clip(NSString *text) {
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:text forType:NSPasteboardTypeString];
}

// Edit > Paste with the grid as its target, as the menu sends it.
void Paste(TerminalView *t) { [NSApp sendAction:@selector(paste:) to:Grid(t) from:nil]; }

NSString *Str(NSData *d) {
    return [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] ?: @"(not UTF-8)";
}

}  // namespace

@interface MCPasteState : NSObject
@property(nonatomic, strong) EditorController *c;
@property(nonatomic, strong) TerminalView *term;
@property(nonatomic, strong) NSMutableData *sent;
@property(nonatomic) NSUInteger alerts;
@end
@implementation MCPasteState
@end

MC_GUI_SUITE(terminal_paste) {
    MCPasteState *s = [MCPasteState new];
    MCGuiStep(@"a terminal of its own, in the project folder", ^{
        MCGuiSetSettings(@"lsp.enabled = false\n");
        s.c = MCGuiController();
        [s.c newTerminal:nil];
        s.term = [s.c valueForKey:@"terminal"];
        GCHECK(s.term != nil && !s.term.lineMode);
        s.sent = [NSMutableData data];
        TerminalGridView *g = Grid(s.term);
        void (^forward)(NSData *) = g.onInput;
        g.onInput = ^(NSData *d) {
            [s.sent appendData:d];
            if (forward) forward(d);
        };
    });
    MCGuiWait(@"its prompt", 20, ^BOOL { return s.term.atPrompt && !s.term.busy; });
    MCGuiStep(@"cd", ^{
        [s.term runCommand:[NSString stringWithFormat:@"cd '%@'", MCGuiProject()]];
    });
    MCGuiWait(@"there, with bracketed paste on at the prompt", 10, ^BOOL {
        return s.term.atPrompt && [[s.term valueForKey:@"zleReady"] boolValue] &&
               Grid(s.term).screen->bracketedPaste();
    });

    // ---- the nested end mark
    MCGuiStep(@"paste the nested end mark at the prompt", ^{
        s.alerts = MCGuiAlertsShown().count;
        [s.sent setLength:0];
        Clip(@"\x1b[20\x1b[201~1~echo INJECTED-$((40+2))\r");
        Paste(s.term);
        NSString *sent = Str(s.sent);
        // One end mark, at the very end, and no ESC between the marks.
        GCHECK([sent hasPrefix:@"\x1b[200~"] && [sent hasSuffix:@"\x1b[201~"]);
        NSString *inside = sent.length >= 12 ? [sent substringWithRange:NSMakeRange(6, sent.length - 12)] : @"";
        GCHECK([inside rangeOfString:@"\x1b"].location == NSNotFound);
        GCHECK([inside containsString:@"␛[201~1~echo INJECTED"]);
        // Bracketed paste on: the lines are text, so nothing asked.
        GCHECK(MCGuiAlertsShown().count == s.alerts);
    });
    MCGuiPause(1.0);
    MCGuiStep(@"nothing ran; the paste waits in the line editor", ^{
        NSString *recent = [s.term recentText:6];
        GCHECK(![recent containsString:@"INJECTED-42"]);
        if ([recent containsString:@"INJECTED-42"]) MCGuiNote(@"%@", recent);
        const char etx = 0x03;   // drop the line
        Grid(s.term).onInput([NSData dataWithBytes:&etx length:1]);
    });
    MCGuiWait(@"a fresh prompt", 10, ^BOOL {
        return s.term.atPrompt && [[s.term valueForKey:@"zleReady"] boolValue];
    });

    // ---- several lines into cat, which takes no bracketed paste
    MCGuiStep(@"cat", ^{ [s.term runCommand:@"cat"]; });
    MCGuiWait(@"cat runs, bracketed paste off", 10, ^BOOL {
        return s.term.busy && !Grid(s.term).screen->bracketedPaste();
    });
    MCGuiStep(@"three lines, Cancel: nothing sent", ^{
        s.alerts = MCGuiAlertsShown().count;
        [s.sent setLength:0];
        Clip(@"one\ntwo\nthree");
        MCGuiAnswerAlerts(@[@(NSAlertThirdButtonReturn)]);
        Paste(s.term);
        GCHECK(MCGuiAlertsShown().count == s.alerts + 1);
        GCHECK_EQ_STR(MCGuiAlertsShown().lastObject, @"Paste 3 lines into the terminal?");
        GCHECK(s.sent.length == 0);
    });
    MCGuiStep(@"Paste as One Line", ^{
        [s.sent setLength:0];
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);
        Paste(s.term);
        GCHECK_EQ_STR(Str(s.sent), @"onetwothree");
    });
    MCGuiStep(@"Paste", ^{
        [s.sent setLength:0];
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        Clip(@"alpha\r\nbeta");
        Paste(s.term);
        GCHECK_EQ_STR(Str(s.sent), @"alpha\rbeta");
    });
    MCGuiStep(@"a command with its line break goes in without it, unasked", ^{
        s.alerts = MCGuiAlertsShown().count;
        [s.sent setLength:0];
        Clip(@"rm -rf build\n");
        Paste(s.term);
        GCHECK(MCGuiAlertsShown().count == s.alerts);
        GCHECK_EQ_STR(Str(s.sent), @"rm -rf build");
    });
    MCGuiStep(@"never: no question", ^{
        MCGuiSetSettings(@"lsp.enabled = false\nterminal.multiline-paste-warning = never\n");
        s.alerts = MCGuiAlertsShown().count;
        [s.sent setLength:0];
        Clip(@"x\ny\n");
        Paste(s.term);
        GCHECK(MCGuiAlertsShown().count == s.alerts);
        GCHECK_EQ_STR(Str(s.sent), @"x\ry\r");
    });
    MCGuiStep(@"cat ends", ^{
        const char eot = 0x04;
        Grid(s.term).onInput([NSData dataWithBytes:&eot length:1]);
        const char etx = 0x03;
        Grid(s.term).onInput([NSData dataWithBytes:&etx length:1]);
    });
    MCGuiWait(@"the prompt", 10, ^BOOL {
        return s.term.atPrompt && !s.term.busy && [[s.term valueForKey:@"zleReady"] boolValue];
    });
    MCGuiStep(@"always: asks even at zsh's prompt", ^{
        MCGuiSetSettings(@"lsp.enabled = false\nterminal.multiline-paste-warning = always\n");
        GCHECK(Grid(s.term).screen->bracketedPaste());
        s.alerts = MCGuiAlertsShown().count;
        [s.sent setLength:0];
        Clip(@"echo a\necho b");
        MCGuiAnswerAlerts(@[@(NSAlertThirdButtonReturn)]);
        Paste(s.term);
        GCHECK(MCGuiAlertsShown().count == s.alerts + 1);
        GCHECK(s.sent.length == 0);
        MCGuiSetSettings(@"lsp.enabled = false\n");
    });

    // ---- secure keyboard entry
    MCGuiStep(@"read -s with the keyboard in the grid", ^{
        [s.c.window makeFirstResponder:Grid(s.term)];
        GCHECK(!Grid(s.term).secureInputOn);
        GCHECK([TerminalGridView secureInputViews] == 0);
        [s.term runCommand:@"read -s 'pw?Password: '; echo; echo got-${#pw}"];
    });
    MCGuiWait(@"secure entry on", 10, ^BOOL { return Grid(s.term).secureInputOn; });
    MCGuiStep(@"for the whole process, with the lock", ^{
        GCHECK(IsSecureEventInputEnabled());
        GCHECK([TerminalGridView secureInputViews] == 1);
        MCGuiSnapshot(Grid(s.term), @"terminal-secure-input");
        // The keyboard leaves the grid: off at once (the next turn).
        [s.c.window makeFirstResponder:nil];
    });
    MCGuiWait(@"off while the grid has no keyboard", 5, ^BOOL {
        return !Grid(s.term).secureInputOn && [TerminalGridView secureInputViews] == 0;
    });
    MCGuiStep(@"the keyboard back", ^{ [s.c.window makeFirstResponder:Grid(s.term)]; });
    MCGuiWait(@"on again", 5, ^BOOL { return Grid(s.term).secureInputOn; });
    MCGuiStep(@"the password typed", ^{
        Grid(s.term).onInput([@"secret\r" dataUsingEncoding:NSUTF8StringEncoding]);
    });
    MCGuiWait(@"read ends and secure entry goes off", 10, ^BOOL {
        return [[s.term recentText:4] containsString:@"\ngot-6"] && !Grid(s.term).secureInputOn &&
               [TerminalGridView secureInputViews] == 0;
    });
    MCGuiStep(@"the password was never echoed", ^{
        GCHECK(![[s.term recentText:6] containsString:@"secret"]);
    });
    MCGuiWait(@"the prompt", 10, ^BOOL { return s.term.atPrompt && !s.term.busy; });
    MCGuiStep(@"at the prompt, nothing is hidden; the terminal closes", ^{
        GCHECK(!Grid(s.term).hiddenInput && !Grid(s.term).secureInputOn);
        GCHECK([(MCTerminalPanel *)[s.c valueForKey:@"terminalPanel"] closeTerminal:s.term]);
    });
}
