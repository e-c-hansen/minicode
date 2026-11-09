// LooseEndsSuite.mm — smaller checks gathered in one file:
//   make guitest SUITES=multicaret_rows   Up and Down with 50,000 carets
//   make guitest SUITES=terminal_zoom     terminal.font-size, the terminals'
//                                         zoom and the editor's, apart
//   make guitest SUITES=terminal_keys     Ctrl+Shift keys of disabled items
//                                         reaching the program
//   make guitest SUITES=terminal_exec     exec bash, sh and zsh
//   make guitest SUITES=agent_autosave    auto-save waiting for Keep
#import "Harness.h"
#import "AgentPanel.h"
#import "EditorController.h"
#import "EditorFont.h"
#import "Lsp.h"
#import "MultiCaretView.h"
#import "Terminal.h"
#import "TerminalGridView.h"
#import "TerminalPanel.h"
#import "AppSettings.h"
#import <objc/message.h>
#import "TreeDetails.h"
#include <libproc.h>
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

// The terminal's shell process is named `name` (an exec keeps its pid).
[[maybe_unused]] inline bool ShellNameIs(TerminalView *t, const char *name) {
    char buf[PROC_PIDPATHINFO_MAXSIZE] = {0};
    return t.shellPid > 0 && proc_name(t.shellPid, buf, sizeof buf) > 0 && strcmp(buf, name) == 0;
}

// A child of the terminal's shell is named `name` (it has started).
[[maybe_unused]] inline bool ShellHasChild(TerminalView *t, const char *name) {
    if (t.shellPid <= 0) return false;
    pid_t kids[256];
    const int n = proc_listchildpids(t.shellPid, kids, sizeof kids);
    for (int i = 0; i < n && i < 256; i++) {
        char buf[PROC_PIDPATHINFO_MAXSIZE] = {0};
        if (proc_name(kids[i], buf, sizeof buf) > 0 && strcmp(buf, name) == 0) return true;
    }
    return false;
}

} // namespace

// ------------------------------------------------------------ the zoom
MC_GUI_SUITE(terminal_zoom) {
    MCLooseState *s = [MCLooseState new];
    NSString *md = MCGuiProjectFile(@"zoom.md");
    TerminalAtPrompt(s);
    MCGuiStep(@"the terminals' zoom starts once from the editor's", ^{
        [MCAppDefaults() removeObjectForKey:@"MCTerminalZoom"];
        MCEditorZoomBy(2);
        GCHECK(MCTerminalZoom() == 2);   // seeded, as the terminals had it
        MCEditorZoomBy(0);
        GCHECK(MCTerminalZoom() == 2);   // and its own from then on
        MCTerminalZoomBy(0);
        GCHECK(MCTerminalZoom() == 0 && [MCAppDefaults() objectForKey:@"MCTerminalZoom"]);
        // Their menu items hold no key: the editor's do.
        NSMenuItem *mine = nil, *editors = nil;
        for (NSMenuItem *top in NSApp.mainMenu.itemArray)
            for (NSMenuItem *it in top.submenu.itemArray) {
                if (it.action == @selector(terminalZoomIn:)) mine = it;
                if (it.action == @selector(editorZoomIn:) && !it.isHidden) editors = it;
            }
        GCHECK(mine != nil && mine.keyEquivalent.length == 0);
        GCHECK_EQ_STR(editors.keyEquivalent, @"=");
    });
    MCGuiStep(@"at 12 points with no zoom", ^{
        MCEditorZoomBy(0);
        MCTerminalZoomBy(0);
        GCHECK(MCTerminalFontSize() == 12);
        GCHECK([TerminalGridView cellFont].pointSize == 12);
        NSTextField *input = [s.term valueForKey:@"input"];
        GCHECK(input.font.pointSize == 12);
        s.cell = [TerminalGridView cellSize];
        s.cols = Cols(s.term);
        MCGuiNote(@"12 pt: cell %.2f x %.0f, %d columns", s.cell.width, s.cell.height, s.cols);
        GCHECK(s.cols > 20);
    });
    MCGuiStep(@"Cmd+= and Shift+Cmd+= with the grid's keyboard, through the key monitor", ^{
        [s.c.window makeFirstResponder:Grid(s.term)];
        MCGuiSendKeyToApp(s.c.window, 24, NSEventModifierFlagCommand, @"=", @"=");
        MCGuiSendKeyToApp(s.c.window, 24, NSEventModifierFlagCommand | NSEventModifierFlagShift,
                          @"+", @"+");
    });
    MCGuiStep(@"the terminal is at 14 points, with fewer columns; the editor as it was", ^{
        GCHECK(MCTerminalZoom() == 2);
        GCHECK(MCEditorZoom() == 0);
        GCHECK(MCEditorFontSize() == 13);
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
    MCGuiStep(@"Cmd+= twice in the page zooms the editor, not the terminal", ^{
        GCHECK([s.c.window.firstResponder isKindOfClass:CodeTextView.class]);
        MCGuiSendKeyToApp(s.c.window, 24, NSEventModifierFlagCommand, @"=", @"=");
        // The menu key works as well.
        GCHECK(MCGuiPerformMenuKey(s.c.window, 24, NSEventModifierFlagCommand, nil, nil));
        GCHECK(MCEditorZoom() == 2);
        GCHECK(MCTerminalZoom() == 2 && [TerminalGridView cellFont].pointSize == 14);
        // Cmd+- in the terminal leaves the editor at 2.
        [s.c.window makeFirstResponder:Grid(s.term)];
        MCGuiSendKeyToApp(s.c.window, 27, NSEventModifierFlagCommand, @"-", @"-");
        GCHECK(MCTerminalZoom() == 1 && MCEditorZoom() == 2);
        MCGuiSendKeyToApp(s.c.window, 24, NSEventModifierFlagCommand, @"=", @"=");
        GCHECK(MCTerminalZoom() == 2);
        [s.c focusEditor:nil];
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
    MCGuiStep(@"Option+Cmd+0 in the page resets the editor and the page, not the terminal", ^{
        GCHECK([s.c.window.firstResponder isKindOfClass:CodeTextView.class]);
        MCGuiSendKeyToApp(s.c.window, 29, NSEventModifierFlagCommand | NSEventModifierFlagOption,
                          @"0", @"0");
        GCHECK(MCEditorZoom() == 0);
        GCHECK(MCTerminalZoom() == 2 && [TerminalGridView cellFont].pointSize == 14);
        NSTextView *tv = Text(s.c);
        NSRange r = [tv.string rangeOfString:@"Body text"];
        GCHECK(r.location != NSNotFound);
        if (r.location != NSNotFound) {
            NSFont *f = [tv.textStorage attribute:NSFontAttributeName atIndex:r.location
                                   effectiveRange:NULL];
            GCHECK(f.pointSize == 15);
        }
    });
    MCGuiStep(@"Option+Cmd+0 in the terminal resets the terminals", ^{
        [s.c.window makeFirstResponder:Grid(s.term)];
        MCEditorZoomBy(1);
        MCGuiSendKeyToApp(s.c.window, 29, NSEventModifierFlagCommand | NSEventModifierFlagOption,
                          @"0", @"0");
        GCHECK(MCTerminalZoom() == 0);
        GCHECK(MCEditorZoom() == 1);
        MCEditorZoomBy(0);
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
        // Cmd+= three times with the line-mode terminal's input line.
        NSTextField *field = [s.term valueForKey:@"input"];
        GCHECK([s.c.window makeFirstResponder:field]);
        for (int i = 0; i < 3; i++)
            MCGuiSendKeyToApp(s.c.window, 24, NSEventModifierFlagCommand, @"=", @"=");
        GCHECK(MCTerminalZoom() == 3 && MCEditorZoom() == 0);
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
        MCTerminalZoomBy(0);
        [(MCTerminalPanel *)[s.c valueForKey:@"terminalPanel"] closeTerminal:s.term];
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
    MCGuiWait(@"cat runs", 10, ^BOOL { return s.term.busy && ShellHasChild(s.term, "cat"); });
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
        // Ctrl+Shift+Tab: Previous Tab, which cannot act while the grid has
        // the keyboard with one tab, goes to the program as a back-tab.
        cg = CGEventCreateKeyboardEvent(NULL, 48, true);
        CGEventSetFlags(cg, (CGEventFlags)(kCGEventFlagMaskControl | kCGEventFlagMaskShift));
        e = [NSEvent eventWithCGEvent:cg];
        CFRelease(cg);
        NSMenuItem *previous = nil;
        for (NSMenuItem *top in NSApp.mainMenu.itemArray)
            for (NSMenuItem *i in top.submenu.itemArray)
                if ([i.keyEquivalent isEqualToString:@"\t"] &&
                    (i.keyEquivalentModifierMask & NSEventModifierFlagShift))
                    previous = i;
        [previous.menu update];
        s.sent.length = 0;
        const BOOL tabTook = [s.c.window performKeyEquivalent:e];
        NSString *sent = [[NSString alloc] initWithData:s.sent encoding:NSUTF8StringEncoding];
        if (previous && !previous.enabled) GCHECK(tabTook && [sent isEqualToString:@"\x1b[Z"]);
        MCGuiNote(@"Previous Tab %@: Ctrl+Shift+Tab sent %lu bytes",
                  previous.enabled ? @"enabled" : @"disabled", (unsigned long)s.sent.length);
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
    // A terminal of its own, closed at the end: it ends in a shell without
    // the integration, which other suites must not be handed.
    MCGuiStep(@"a new terminal", ^{ [MCGuiController() newTerminal:nil]; });
    TerminalAtPrompt(s);
    // exec zsh keeps the integration (an earlier fix): a prompt, not busy,
    // and the next command runs.
    // Waited for, not paused for (1.5 s was not always enough on a busy
    // machine); at least half a second, so the old prompt cannot count.
    __block NSDate *execAt = nil;
    MCGuiStep(@"exec zsh", ^{
        [s.term runCommand:@"exec zsh"];
        execAt = [NSDate date];
    });
    MCGuiWait(@"zsh again, at a prompt, not busy", 15, ^BOOL {
        return -execAt.timeIntervalSinceNow > 0.5 && s.term.atPrompt && !s.term.busy &&
               s.term.runningCommands.count == 0;
    });
    MCGuiStep(@"a command", ^{ [s.term runCommand:@"echo after-exec-zsh"]; });
    MCGuiWait(@"it runs", 10, ^BOOL {
        return s.term.atPrompt && [[s.term recentText:3] containsString:@"\nafter-exec-zsh"];
    });
    // exec bash: no marks from here on. Busy only while a program holds
    // the foreground.
    MCGuiStep(@"exec bash", ^{ [s.term runCommand:@"exec /bin/bash --norc --noprofile"]; });
    MCGuiWait(@"bash's prompt", 10, ^BOOL { return [[s.term recentText:2] containsString:@"bash-"]; });
    // The tab sees bash by polling the shell's name twice a second.
    MCGuiWait(@"the tab takes bash for a shell", 10, ^BOOL {
        return !s.term.busy && s.term.runningCommands.count == 0 &&
               ![s.term.title containsString:@"exec"];
    });
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
    // The nonce went to the exec'd zsh only: bash, and what bash runs,
    // never see it (any of them could print marks the tab believed).
    MCGuiStep(@"bash has no nonce", ^{
        [s.term runCommand:@"echo nonce-count-$(env | grep -c MINICODE_NONCE)"];
    });
    MCGuiWait(@"none", 10, ^BOOL {
        return [[s.term recentText:4] containsString:@"\nnonce-count-0"];
    });
    // exec zsh from bash: zsh loads the integration but has no nonce, so
    // its marks are not believed; the tab goes on as a shell without them.
    MCGuiStep(@"exec zsh from bash", ^{ [s.term runCommand:@"exec zsh"]; });
    MCGuiWait(@"the shell is zsh, not busy", 10, ^BOOL {
        return ShellNameIs(s.term, "zsh") && !s.term.busy;
    });
    MCGuiStep(@"a command in that zsh", ^{
        GCHECK(!s.term.busy);
        [s.term runCommand:@"echo zsh-from-bash-$ZSH_VERSION"];
    });
    MCGuiWait(@"it runs", 10, ^BOOL {
        return [[s.term recentText:4] containsString:@"\nzsh-from-bash-5"];
    });
    MCGuiStep(@"sleep 1 in zsh", ^{ [s.term runCommand:@"sleep 1"]; });
    MCGuiWait(@"busy by the foreground", 5, ^BOOL { return s.term.busy; });
    MCGuiWait(@"and done", 10, ^BOOL { return !s.term.busy; });
    // exec sh, the same.
    MCGuiStep(@"exec sh", ^{ [s.term runCommand:@"exec /bin/sh"]; });
    MCGuiWait(@"sh's prompt", 10, ^BOOL { return [[s.term recentText:2] containsString:@"sh-"]; });
    MCGuiWait(@"the tab takes sh for a shell", 10, ^BOOL {
        return !s.term.busy && s.term.runningCommands.count == 0;
    });
    MCGuiStep(@"not busy in sh", ^{
        GCHECK(!s.term.busy && s.term.runningCommands.count == 0);
        [s.term runCommand:@"echo sh-at-last"];
    });
    MCGuiWait(@"and a command there", 10, ^BOOL {
        return [[s.term recentText:3] containsString:@"\nsh-at-last"];
    });
    MCGuiStep(@"its terminal closes", ^{
        GCHECK([(MCTerminalPanel *)[s.c valueForKey:@"terminalPanel"] closeTerminal:s.term]);
    });
}

// ------------------------------------------ the inline agent and auto-save
// What the auto-save timer runs, run now (EditorController+Persistence.mm).
static void AutoSaveNow(EditorController *c) {
    SEL save = NSSelectorFromString(@"autoSaveNow");
    GCHECK([c respondsToSelector:save]);
    if ([c respondsToSelector:save]) ((void (*)(id, SEL))objc_msgSend)(c, save);
}

MC_GUI_SUITE(agent_autosave) {
    MCLooseState *s = [MCLooseState new];
    NSString *file = MCGuiProjectFile(@"agent-save.txt");
    NSString *script = [MCGuiScratch() stringByAppendingPathComponent:@"fake-claude.sh"];
    NSString *(^disk)(void) = ^NSString *(void) {
        return [NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:nil];
    };
    MCGuiStep(@"auto-save after 200 ms, and an agent that capitalizes alpha", ^{
        MCGuiWriteFile(script, @"#!/bin/sh\ncat >/dev/null\nprintf '<<<<<<< SEARCH\\nalpha\\n"
                               @"=======\\nALPHA\\n>>>>>>> REPLACE\\n'\n");
        chmod(script.fileSystemRepresentation, 0755);
        SetSettings([NSString stringWithFormat:@"lsp.enabled = false\neditor.auto-save = after-delay\n"
                                               @"editor.auto-save-delay = 200\nagent.command = %@\n",
                                               script]);
        MCGuiWriteFile(file, @"alpha\nbeta\n");
        s.c = MCGuiController();
        [s.c focusEditor:nil];
        GCHECK([s.c showFile:file]);
        Text(s.c).selectedRange = NSMakeRange(0, 0);
        [s.c inlineAgent:nil];
        MCAgentSession *agent = [s.c valueForKey:@"agentSession"];
        GCHECK(agent.shown);
        [agent send:@"capitalize alpha"];
    });
    MCGuiWait(@"the agent's edit", 20, ^BOOL {
        MCAgentSession *agent = [s.c valueForKey:@"agentSession"];
        return agent.editPending;
    });
    // Auto-save (200 ms) must not write the proposed edit. Not a pause:
    // the step runs the auto-save itself (autoSaveNow, what the delay's
    // timer runs), so the check holds whenever that timer would fire.
    MCGuiStep(@"proposed, not in the file, while Keep waits", ^{
        AutoSaveNow(s.c);
        MCAgentSession *agent = [s.c valueForKey:@"agentSession"];
        GCHECK([agent.pendingText hasPrefix:@"ALPHA\n"]);
        GCHECK([Text(s.c).string hasPrefix:@"alpha\n"]);
        GCHECK_EQ_STR(disk(), @"alpha\nbeta\n");
        [agent keep];
        GCHECK([Text(s.c).string hasPrefix:@"ALPHA\n"]);
    });
    MCGuiWait(@"Keep: auto-saved", 3, ^BOOL { return [disk() isEqualToString:@"ALPHA\nbeta\n"]; });
    // Discard: the edit never reaches the disk, and the tab is clean again.
    MCGuiStep(@"again, then Discard", ^{
        MCGuiWriteFile(script, @"#!/bin/sh\ncat >/dev/null\nprintf '<<<<<<< SEARCH\\nbeta\\n"
                               @"=======\\nBETA\\n>>>>>>> REPLACE\\n'\n");
        [s.c focusEditor:nil];
        [s.c inlineAgent:nil];
        [(MCAgentSession *)[s.c valueForKey:@"agentSession"] send:@"capitalize beta"];
    });
    MCGuiWait(@"the second edit", 20, ^BOOL {
        return ((MCAgentSession *)[s.c valueForKey:@"agentSession"]).editPending;
    });
    MCGuiStep(@"proposed; Discard", ^{
        AutoSaveNow(s.c);   // as above
        MCAgentSession *agent = [s.c valueForKey:@"agentSession"];
        GCHECK_EQ_STR(agent.pendingText, @"ALPHA\nBETA\n");
        GCHECK([Text(s.c).string isEqualToString:@"ALPHA\nbeta\n"]);
        GCHECK_EQ_STR(disk(), @"ALPHA\nbeta\n");
        [agent discard];
        GCHECK([Text(s.c).string isEqualToString:@"ALPHA\nbeta\n"]);
    });
    MCGuiStep(@"the disk never had BETA", ^{
        AutoSaveNow(s.c);   // no auto-save of the discarded edit
        GCHECK_EQ_STR(disk(), @"ALPHA\nbeta\n");
        GCHECK(![[s.c valueForKey:@"dirty"] boolValue]);
        SetSettings(@"lsp.enabled = false\n");
    });
}

// ------------------------------------------------- Up and Down, many carets
MC_GUI_SUITE(multicaret_rows) {
    __block EditorController *c = nil;
    __block CodeTextView *tv = nil;
    static const int kLines = 50000;
    NSString *path = MCGuiProjectFile(@"many-lines.txt");
    MCGuiStep(@"a 50,000-line file", ^{
        NSMutableString *s = [NSMutableString string];
        for (int i = 0; i < kLines; i++) [s appendFormat:@"line %06d of the file\n", i];
        MCGuiWriteFile(path, s);
        c = MCGuiController();
        [c focusEditor:nil];
        GCHECK([c showFile:path]);
        tv = (CodeTextView *)Text(c);
        GCHECK([tv isKindOfClass:CodeTextView.class]);
    });
    MCGuiStep(@"a caret at column 5 of every line, then Down and Up", ^{
        NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
        const NSUInteger lineLen = [@"line 000000 of the file\n" length];
        for (int i = 0; i < kLines; i++)
            [ranges addObject:[NSValue valueWithRange:NSMakeRange(i * lineLen + 5, 0)]];
        [tv.multiCarets setCaretRanges:ranges];
        GCHECK(tv.multiCarets.caretRanges.count == (NSUInteger)kLines);
        double t0 = Now();
        GCHECK([tv.multiCarets doCommand:@selector(moveDown:)]);
        double down = Now() - t0;
        NSArray<NSValue *> *after = tv.multiCarets.caretRanges;
        // The last caret has no line below but the empty one at the end.
        MCGuiNote(@"carets after Down: %lu", (unsigned long)after.count);
        GCHECK(after.count == (NSUInteger)kLines);
        GCHECK(after.count > 2 && after[0].rangeValue.location == lineLen + 5 &&
               after[1].rangeValue.location == 2 * lineLen + 5);
        t0 = Now();
        GCHECK([tv.multiCarets doCommand:@selector(moveUp:)]);
        double up = Now() - t0;
        after = tv.multiCarets.caretRanges;
        GCHECK(after.count > 2 && after[0].rangeValue.location == 5 &&
               after[1].rangeValue.location == lineLen + 5);
        t0 = Now();
        GCHECK([tv.multiCarets doCommand:@selector(moveDown:)]);
        double again = Now() - t0;
        MCGuiNote(@"50,000 carets: Down %.0f ms, Up %.0f ms, Down again %.0f ms",
                  down * 1000, up * 1000, again * 1000);
        [tv.multiCarets clear];
        tv.selectedRange = NSMakeRange(0, 0);
    });
    // Lines of different lengths: a short line takes the caret to its end,
    // the next line gives it its column back, the last line keeps it there,
    // and Shift extends.
    NSString *small = MCGuiProjectFile(@"rows.txt");
    MCGuiStep(@"short lines, the last line, the goal column, Shift", ^{
        MCGuiWriteFile(small, @"abcdefgh\nab\nabcdefgh\nabcdefgh\nxy");
        GCHECK([c showFile:small]);
        tv = (CodeTextView *)Text(c);
        NSString *t = tv.string;
        auto at = ^NSUInteger(int line, int col) {
            NSUInteger i = 0;
            for (int l = 0; l < line; l++) i = NSMaxRange([t lineRangeForRange:NSMakeRange(i, 0)]);
            return i + col;
        };
        auto heads = ^NSString *(void) {
            NSMutableArray *out = [NSMutableArray array];
            for (NSValue *v in tv.multiCarets.caretRanges) {
                NSRange r = v.rangeValue;
                [out addObject:r.length ? [NSString stringWithFormat:@"%lu-%lu",
                                           (unsigned long)r.location, (unsigned long)NSMaxRange(r)]
                                        : [NSString stringWithFormat:@"%lu",
                                           (unsigned long)r.location]];
            }
            return [out componentsJoinedByString:@" "];
        };
        [tv.multiCarets setCaretRanges:@[[NSValue valueWithRange:NSMakeRange(at(0, 5), 0)],
                                         [NSValue valueWithRange:NSMakeRange(at(3, 5), 0)]]];
        [tv.multiCarets doCommand:@selector(moveDown:)];
        GCHECK_EQ_STR(heads(), ([NSString stringWithFormat:@"%lu %lu",
                                 (unsigned long)at(1, 2), (unsigned long)at(4, 2)]));
        [tv.multiCarets doCommand:@selector(moveDown:)];
        GCHECK_EQ_STR(heads(), ([NSString stringWithFormat:@"%lu %lu",
                                 (unsigned long)at(2, 5), (unsigned long)t.length]));
        [tv.multiCarets doCommand:@selector(moveUp:)];
        GCHECK_EQ_STR(heads(), ([NSString stringWithFormat:@"%lu %lu",
                                 (unsigned long)at(1, 2), (unsigned long)at(3, 5)]));
        [tv.multiCarets doCommand:@selector(moveUp:)];
        [tv.multiCarets doCommand:@selector(moveUp:)];
        GCHECK_EQ_STR(heads(), ([NSString stringWithFormat:@"0 %lu", (unsigned long)at(1, 2)]));
        // Shift+Down from carets at column 1 of lines 0 and 2.
        [tv.multiCarets setCaretRanges:@[[NSValue valueWithRange:NSMakeRange(at(0, 1), 0)],
                                         [NSValue valueWithRange:NSMakeRange(at(2, 1), 0)]]];
        [tv.multiCarets doCommand:@selector(moveDownAndModifySelection:)];
        GCHECK_EQ_STR(heads(), ([NSString stringWithFormat:@"%lu-%lu %lu-%lu",
                                 (unsigned long)at(0, 1), (unsigned long)at(1, 1),
                                 (unsigned long)at(2, 1), (unsigned long)at(3, 1)]));
        [tv.multiCarets clear];
    });
    // A text ending in a line break: Down from the last line reaches the
    // empty line after it, and Up comes back to the column.
    MCGuiStep(@"the empty line after a final line break", ^{
        NSString *ending = MCGuiProjectFile(@"rows-ending.txt");
        MCGuiWriteFile(ending, @"abcdef\nabcdef\n");
        GCHECK([c showFile:ending]);
        tv = (CodeTextView *)Text(c);
        GCHECK_EQ_STR(tv.string, @"abcdef\nabcdef\n");
        [tv.multiCarets setCaretRanges:@[[NSValue valueWithRange:NSMakeRange(3, 0)],
                                         [NSValue valueWithRange:NSMakeRange(10, 0)]]];
        [tv.multiCarets doCommand:@selector(moveDown:)];
        NSArray<NSValue *> *r = tv.multiCarets.caretRanges;
        GCHECK(r.count == 2 && r[0].rangeValue.location == 10 && r[1].rangeValue.location == 14);
        [tv.multiCarets doCommand:@selector(moveUp:)];
        r = tv.multiCarets.caretRanges;
        GCHECK(r.count == 2 && r[0].rangeValue.location == 3 && r[1].rangeValue.location == 10);
        [tv.multiCarets clear];
    });
}
