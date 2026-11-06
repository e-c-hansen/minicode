// LooseEndsSuite.mm — smaller checks gathered in one file:
//   make guitest SUITES=terminal_zoom     terminal.font-size, and the zoom
//                                         scaling terminals and the preview
//   make guitest SUITES=terminal_keys     Ctrl+Shift keys of disabled items
//                                         reaching the program
//   make guitest SUITES=terminal_exec     exec bash, sh and zsh
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
    NSString *md = MCGuiProjectFile(@"zoom.md");
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
    // The Markdown preview follows the zoom.
    MCGuiStep(@"a Markdown page", ^{
        MCGuiWriteFile(md, @"# Title\n\nBody text here.\n\n- an item\n");
        [s.c focusEditor:nil];
        GCHECK([s.c showFile:md]);
    });
    MCGuiStep(@"its text is 15 points scaled by the zoom", ^{
        NSTextView *tv = Text(s.c);
        NSRange r = [tv.string rangeOfString:@"Body text"];
        GCHECK(r.location != NSNotFound);
        if (r.location == NSNotFound) return;
        NSFont *f = [tv.textStorage attribute:NSFontAttributeName atIndex:r.location
                               effectiveRange:NULL];
        GCHECK(f.pointSize == MCZoomed(15) && f.pointSize > 15);
        NSFont *h = [tv.textStorage attribute:NSFontAttributeName
                                      atIndex:[tv.string rangeOfString:@"Title"].location
                               effectiveRange:NULL];
        GCHECK(h.pointSize == MCZoomed(26));
        MCGuiNote(@"zoom 2: body %.1f pt, h1 %.1f pt", f.pointSize, h.pointSize);
    });
    MCGuiStep(@"Option+Cmd+0 resets the editor, the page and the terminal", ^{
        GCHECK(MCGuiPerformMenuKey(s.c.window, 29, NSEventModifierFlagCommand | NSEventModifierFlagOption,
                                   nil, nil));
        GCHECK(MCEditorZoom() == 0);
        NSTextView *tv = Text(s.c);
        NSRange r = [tv.string rangeOfString:@"Body text"];
        GCHECK(r.location != NSNotFound);
        if (r.location != NSNotFound) {
            NSFont *f = [tv.textStorage attribute:NSFontAttributeName atIndex:r.location
                                   effectiveRange:NULL];
            GCHECK(f.pointSize == 15);
        }
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

// ----------------------------------------------- Ctrl+Shift keys in the grid
MC_GUI_SUITE(terminal_keys) {
    MCLooseState *s = [MCLooseState new];
    TerminalAtPrompt(s);
    MCGuiStep(@"cat -v reads the keys", ^{ [s.term runCommand:@"cat -v"]; });
    MCGuiWait(@"cat runs", 10, ^BOOL { return s.term.busy; });
    MCGuiPause(0.3);
    MCGuiStep(@"Ctrl+Shift+K reaches cat, not Delete Line", ^{
        GCHECK(s.c.window.firstResponder == Grid(s.term));
        s.sent.length = 0;
        // The window's key equivalents first (the grid's), as AppKit offers them.
        CGEventRef cg = CGEventCreateKeyboardEvent(NULL, 40, true);
        CGEventSetFlags(cg, (CGEventFlags)(kCGEventFlagMaskControl | kCGEventFlagMaskShift));
        NSEvent *e = [NSEvent eventWithCGEvent:cg];
        CFRelease(cg);
        GCHECK([s.c.window performKeyEquivalent:e]);
        GCHECK(s.sent.length == 1 && ((const char *)s.sent.bytes)[0] == 0x0B);
        // Ctrl+Shift+- reads "_": matched to Go Forward's "-" all the same,
        // and the program's while that item cannot act.
        cg = CGEventCreateKeyboardEvent(NULL, 27, true);
        CGEventSetFlags(cg, (CGEventFlags)(kCGEventFlagMaskControl | kCGEventFlagMaskShift));
        e = [NSEvent eventWithCGEvent:cg];
        CFRelease(cg);
        NSMenuItem *forward = nil;
        for (NSMenuItem *top in NSApp.mainMenu.itemArray)
            for (NSMenuItem *i in top.submenu.itemArray)
                if (i.action == NSSelectorFromString(@"navigateForward:")) forward = i;
        GCHECK(forward != nil);
        [forward.menu update];
        s.sent.length = 0;
        const BOOL took = [s.c.window performKeyEquivalent:e];
        if (!forward.enabled) GCHECK(took && s.sent.length == 1 && ((const char *)s.sent.bytes)[0] == 0x1F);
        else GCHECK(!took && s.sent.length == 0);
        MCGuiNote(@"Go Forward %@: Ctrl+Shift+- sent %lu bytes", forward.enabled ? @"enabled" : @"disabled",
                  (unsigned long)s.sent.length);
        // Every Ctrl+Shift item of the menu, and whether it acts here.
        NSMutableArray *items = [NSMutableArray array];
        NSMutableArray *stack = [NSMutableArray arrayWithObject:NSApp.mainMenu];
        while (stack.count) {
            NSMenu *m = stack.lastObject;
            [stack removeLastObject];
            [m update];
            for (NSMenuItem *i in m.itemArray) {
                if (i.submenu) { [stack addObject:i.submenu]; continue; }
                NSEventModifierFlags f = i.keyEquivalentModifierMask;
                if ((f & NSEventModifierFlagControl) && !(f & NSEventModifierFlagCommand) &&
                    i.keyEquivalent.length)
                    [items addObject:[NSString stringWithFormat:@"%@ (%@%@%@)%@", i.title,
                        f & NSEventModifierFlagShift ? @"Shift+" : @"",
                        f & NSEventModifierFlagOption ? @"Option+" : @"", i.keyEquivalent,
                        i.enabled ? @"" : @" disabled"]];
            }
        }
        MCGuiNote(@"Ctrl items with the grid's keyboard: %@", [items componentsJoinedByString:@", "]);
    });
    MCGuiWait(@"cat echoes ^K", 5, ^BOOL { return [[s.term recentText:3] containsString:@"^K"]; });
    MCGuiStep(@"Ctrl+Shift+W, an enabled item, is still the app's (no byte sent)", ^{
        // Close Terminal is enabled here, so it acts: checked by the menu's
        // own validation rather than run, which would close the terminal.
        CGEventRef cg = CGEventCreateKeyboardEvent(NULL, 13, true);
        CGEventSetFlags(cg, (CGEventFlags)(kCGEventFlagMaskControl | kCGEventFlagMaskShift));
        NSEvent *e = [NSEvent eventWithCGEvent:cg];
        CFRelease(cg);
        NSMenuItem *close = nil;
        NSMutableArray *stack = [NSMutableArray arrayWithObject:NSApp.mainMenu];
        while (stack.count && !close) {
            NSMenu *m = stack.lastObject;
            [stack removeLastObject];
            for (NSMenuItem *i in m.itemArray) {
                if (i.submenu) [stack addObject:i.submenu];
                else if ([i.keyEquivalent isEqualToString:@"w"] &&
                         i.keyEquivalentModifierMask ==
                             (NSEventModifierFlagControl | NSEventModifierFlagShift))
                    close = i;
            }
        }
        GCHECK(close != nil);
        [close.menu update];
        GCHECK(close.enabled);
        s.sent.length = 0;
        // The grid lets it go to the menu: offered to the grid alone, it is refused.
        GCHECK(![Grid(s.term) performKeyEquivalent:e]);
        GCHECK(s.sent.length == 0);
    });
    MCGuiStep(@"Ctrl+C ends cat", ^{
        const char etx = 0x03;
        Grid(s.term).onInput([NSData dataWithBytes:&etx length:1]);
    });
    MCGuiWait(@"the prompt", 10, ^BOOL { return s.term.atPrompt && !s.term.busy; });
}

// ------------------------------------------------------ exec into a shell
MC_GUI_SUITE(terminal_exec) {
    MCLooseState *s = [MCLooseState new];
    TerminalAtPrompt(s);
    // exec zsh keeps the integration (an earlier fix): a prompt, not busy,
    // and the next command runs.
    MCGuiStep(@"exec zsh", ^{ [s.term runCommand:@"exec zsh"]; });
    MCGuiPause(1.5);
    MCGuiStep(@"zsh again, not busy; a command", ^{
        GCHECK(s.term.atPrompt && !s.term.busy && s.term.runningCommands.count == 0);
        [s.term runCommand:@"echo after-exec-zsh"];
    });
    MCGuiWait(@"it runs", 10, ^BOOL {
        return s.term.atPrompt && [[s.term recentText:3] containsString:@"\nafter-exec-zsh"];
    });
    // exec bash: no marks from here on. Busy only while a program holds
    // the foreground.
    MCGuiStep(@"exec bash", ^{ [s.term runCommand:@"exec /bin/bash --norc --noprofile"]; });
    MCGuiWait(@"bash's prompt", 10, ^BOOL { return [[s.term recentText:2] containsString:@"bash-"]; });
    MCGuiPause(1.0);
    MCGuiStep(@"the tab is not busy, and not titled exec", ^{
        GCHECK(!s.term.busy);
        GCHECK(s.term.runningCommands.count == 0);
        GCHECK(![s.term.title containsString:@"exec"]);
        MCGuiNote(@"title in bash: %@", s.term.title);
        [s.term runCommand:@"echo in-bash-$BASH_VERSINFO"];
    });
    MCGuiWait(@"a command typed in bash runs", 10, ^BOOL {
        return [[s.term recentText:4] containsString:@"\nin-bash-3"];
    });
    MCGuiStep(@"sleep 2 in bash", ^{ [s.term runCommand:@"sleep 2"]; });
    MCGuiWait(@"sleep holds the terminal", 5, ^BOOL {
        return s.term.runningCommands.count > 0 && [s.term.runningCommand containsString:@"sleep"];
    });
    MCGuiWait(@"and lets it go", 10, ^BOOL { return s.term.runningCommands.count == 0; });
    // exec zsh from bash: zsh loads the integration, and its marks count again.
    MCGuiStep(@"exec zsh from bash", ^{ [s.term runCommand:@"exec zsh"]; });
    MCGuiWait(@"zsh's integration again: at a prompt", 15, ^BOOL {
        return s.term.atPrompt && !s.term.busy;
    });
    MCGuiStep(@"sleep 1 in zsh", ^{ [s.term runCommand:@"sleep 1"]; });
    MCGuiWait(@"busy by the marks", 5, ^BOOL { return s.term.busy; });
    MCGuiWait(@"and done", 10, ^BOOL { return s.term.atPrompt && !s.term.busy; });
    // exec sh, the same.
    MCGuiStep(@"exec sh", ^{ [s.term runCommand:@"exec /bin/sh"]; });
    MCGuiWait(@"sh's prompt", 10, ^BOOL { return [[s.term recentText:2] containsString:@"sh-"]; });
    MCGuiPause(1.0);
    MCGuiStep(@"not busy in sh", ^{
        GCHECK(!s.term.busy && s.term.runningCommands.count == 0);
        [s.term runCommand:@"exec zsh"];
    });
    MCGuiWait(@"back at zsh's prompt", 15, ^BOOL { return s.term.atPrompt && !s.term.busy; });
    MCGuiStep(@"and a command there", ^{ [s.term runCommand:@"echo zsh-at-last"]; });
    MCGuiWait(@"it runs", 10, ^BOOL {
        return s.term.atPrompt && [[s.term recentText:3] containsString:@"\nzsh-at-last"];
    });
}
