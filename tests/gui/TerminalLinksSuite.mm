// TerminalLinksSuite.mm — `make guitest SUITES=terminal_links`: web pages
// opened from a terminal land in minicode's browser pane, the user's page.
// A local server (python3 -m http.server on 127.0.0.1) prints its address,
// and a Cmd+click on it loads it there, the keyboard staying in the
// terminal; 0.0.0.0 is read as localhost; an OSC 8 hyperlink printed by
// printf opens its own address on Cmd+click (a file one in the editor),
// shows it as the tooltip, and a javascript: one is no link at all; and a
// program with mouse reporting on, which takes the plain click itself and
// opens the page with Python's webbrowser, goes through BROWSER (the
// helper, then `minicode open-url`) into the pane too, while Cmd still
// reaches minicode's own link. Pages that are not local, and any page when
// minicode does not answer, go to MINICODE_OPEN_FALLBACK (a recorder here,
// /usr/bin/open for real), and node runs the helper as Vite does. Nothing
// off the Mac is ever loaded. Real zsh and python3 (node when there is one).
#import "Harness.h"
#import "Browser.h"
#import "BrowserPages.h"
#import "ControlServer.h"
#import "EditorController.h"
#import "Lsp.h"   // MCFindProgram
#import "Terminal.h"
#import "TerminalGridView.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <objc/runtime.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

NSString *gDir;   // the suite's folder (its window's)
int gPort;

TerminalGridView *Grid(TerminalView *t) { return [t valueForKey:@"grid"]; }
TerminalScreen *Screen(TerminalView *t) { return Grid(t).screen; }

// A port the system gave out and took back, for the server to listen on.
int FreePort() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    int port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) == 0 &&
        getsockname(fd, (struct sockaddr *)&a, &len) == 0)
        port = ntohs(a.sin_port);
    close(fd);
    return port;
}

NSString *Local(NSString *path) {
    return [NSString stringWithFormat:@"http://127.0.0.1:%d%@", gPort, path];
}

NSPoint CellPoint(TerminalGridView *g, int row, int col) {
    NSSize cs = [TerminalGridView cellSize];
    CGFloat pad = [TerminalGridView padding];
    NSPoint p = NSMakePoint(pad + (col + 0.5) * cs.width, pad + (row + 0.5) * cs.height);
    return [g convertPoint:p toView:nil];
}

void Click(TerminalGridView *g, int row, int col, NSEventModifierFlags flags) {
    for (NSEventType type : {NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp}) {
        NSEvent *e = [NSEvent mouseEventWithType:type location:CellPoint(g, row, col)
                                   modifierFlags:flags
                                       timestamp:NSProcessInfo.processInfo.systemUptime
                                    windowNumber:g.window.windowNumber context:nil eventNumber:0
                                      clickCount:1 pressure:1];
        if (type == NSEventTypeLeftMouseDown) [g mouseDown:e];
        else [g mouseUp:e];
    }
}

// The last screen row whose text starts with `prefix` (the output, not the
// command line that printed it), or -1.
int RowStarting(TerminalScreen *s, NSString *prefix) {
    int found = -1;
    for (int r = 0; r < s->rows(); r++)
        if ([@(s->lineText(r).c_str()) hasPrefix:prefix]) found = r;
    return found;
}

// The last screen row holding `text`, or -1 (a background job's line may
// follow the prompt on its row).
int RowHolding(TerminalScreen *s, NSString *text) {
    int found = -1;
    for (int r = 0; r < s->rows(); r++)
        if ([@(s->lineText(r).c_str()) containsString:text]) found = r;
    return found;
}

// Column of `text` in a row (ASCII rows: a byte is a cell), or -1.
int ColumnOf(TerminalScreen *s, int row, NSString *text) {
    if (row < 0) return -1;
    size_t at = s->lineText(row).find(text.UTF8String);
    return at == std::string::npos ? -1 : (int)at;
}

// Cmd held and the pointer resting on a cell, as updateLinkHover reads
// them (the real modifier keys and mouse are the user's): +[NSEvent
// modifierFlags] and -[NSWindow mouseLocationOutsideOfEventStream] answer
// from here during `block`.
void WithCommandHover(TerminalGridView *g, int row, int col, void (^block)(void)) {
    Method flags = class_getClassMethod(NSEvent.class, @selector(modifierFlags));
    Method mouse = class_getInstanceMethod(NSWindow.class, @selector(mouseLocationOutsideOfEventStream));
    IMP oldFlags = method_getImplementation(flags), oldMouse = method_getImplementation(mouse);
    NSPoint at = CellPoint(g, row, col);
    method_setImplementation(flags, imp_implementationWithBlock(^NSEventModifierFlags(id) {
        return NSEventModifierFlagCommand;
    }));
    method_setImplementation(mouse, imp_implementationWithBlock(^NSPoint(id) { return at; }));
    block();
    method_setImplementation(flags, oldFlags);
    method_setImplementation(mouse, oldMouse);
}

BrowserView *Pane(EditorController *c) { return [c browserPane]; }

BOOL PaneShows(EditorController *c, NSString *title) {
    BrowserView *b = Pane(c);
    return b && !b.agentPageShown && [b.userProbe.title isEqualToString:title] && !b.userProbe.loading;
}

NSString *Recorded() {
    return [NSString stringWithContentsOfFile:[gDir stringByAppendingPathComponent:@"opened.txt"]
                                     encoding:NSUTF8StringEncoding error:nil] ?: @"";
}

void Page(NSString *name, NSString *title) {
    MCGuiWriteFile([[gDir stringByAppendingPathComponent:@"site"] stringByAppendingPathComponent:name],
                   [NSString stringWithFormat:@"<html><head><title>%@</title></head>"
                                              @"<body>%@</body></html>\n", title, title]);
}

}  // namespace

MC_GUI_SUITE(terminal_links) {
    __block EditorController *c = nil;
    __block TerminalView *term = nil;
    __block TerminalGridView *grid = nil;
    NSString *python = MCFindProgram(@"python3");
    NSString *node = MCFindProgram(@"node");
    gDir = [MCGuiScratch() stringByAppendingPathComponent:@"links"];
    gPort = FreePort();
    NSString *recorder = [gDir stringByAppendingPathComponent:@"record.sh"];
    NSString *helper = [MCGuiBuildDir() stringByAppendingPathComponent:@"minicode-browser.js"];
    if (!python) {
        MCGuiStep(@"python3 is needed for the local server", ^{ MCGuiNote(@"no python3: skipped"); });
        return;
    }

    MCGuiStep(@"a window of the suite's own, with a site and a recorder", ^{
        Page(@"index.html", @"Served");
        Page(@"zero.html", @"Zero");
        Page(@"osc8.html", @"Hyperlink");
        Page(@"cmd.html", @"Cmd");
        Page(@"mouse.html", @"Mouse");
        Page(@"vite.html", @"Vite");
        Page(@"dead.html", @"Dead");
        MCGuiWriteFile([gDir stringByAppendingPathComponent:@"notes.txt"], @"the notes\n");
        // What /usr/bin/open would have been asked to open, written down
        // instead: the tests never open a browser.
        MCGuiWriteFile(recorder, [NSString stringWithFormat:
            @"#!/bin/sh\nprintf '%%s\\n' \"$*\" >> '%@/opened.txt'\n", gDir]);
        chmod(recorder.fileSystemRepresentation, 0755);
        setenv("MINICODE_OPEN_FALLBACK", recorder.fileSystemRepresentation, 1);
        // A program that turns on mouse reporting, prints one address, and
        // on a plain click opens another with Python's webbrowser.
        MCGuiWriteFile([gDir stringByAppendingPathComponent:@"mouse_open.py"],
            @"import os, sys, termios, time, tty, webbrowser\n"
            @"shown, opens = sys.argv[1], sys.argv[2]\n"
            @"fd = sys.stdin.fileno()\n"
            @"old = termios.tcgetattr(fd)\n"
            @"tty.setraw(fd)\n"
            @"try:\n"
            @"    os.write(1, b'\\x1b[?1000h\\x1b[?1006h')\n"
            @"    os.write(1, ('click the link: ' + shown + '\\r\\n').encode())\n"
            @"    buf = b''\n"
            @"    while b'M' not in buf.partition(b'\\x1b[<0;')[2]:\n"
            @"        got = os.read(fd, 64)\n"
            @"        if not got or b'q' in got: sys.exit(1)\n"
            @"        buf += got\n"
            @"finally:\n"
            @"    os.write(1, b'\\x1b[?1006l\\x1b[?1000l')\n"
            @"    time.sleep(0.2)\n"
            @"    termios.tcflush(fd, termios.TCIFLUSH)   # the release, unread\n"
            @"    termios.tcsetattr(fd, termios.TCSADRAIN, old)\n"
            @"print('opened', webbrowser.open(opens))\n");
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:gDir];
        c = MCGuiController();
        MCGuiSetKeyWindow(c.window);
        NSRect f = c.window.frame;
        if (f.size.width < 1200) {
            f.size.width = 1200;
            [c.window setFrame:f display:NO];
        }
        [c focusTerminal:nil];
        term = [c valueForKey:@"terminal"];
        grid = Grid(term);
        GCHECK(term && grid && !term.lineMode);
        GCHECK([[NSFileManager defaultManager] isExecutableFileAtPath:helper]);
    });
    MCGuiWait(@"its prompt", 20, ^BOOL { return term.atPrompt; });
    MCGuiStep(@"BROWSER names the helper; the server starts", ^{
        [term runCommand:[NSString stringWithFormat:
            @"cd '%@'; echo \"browser=$BROWSER\"; "
            @"python3 -m http.server --bind 127.0.0.1 --directory site %d 2>/dev/null &", gDir, gPort]];
    });
    MCGuiWait(@"the server says where it is", 20, ^BOOL {
        return [[term recentText:20] containsString:[NSString stringWithFormat:@"(%@)", Local(@"/")]];
    });
    MCGuiStep(@"the shell's BROWSER", ^{
        GCHECK([[term recentText:20] containsString:[@"browser=" stringByAppendingString:helper]]);
    });

    // ------------------------------------------- Cmd+click on the address
    MCGuiStep(@"Cmd+click on the address the server printed", ^{
        TerminalScreen *s = Screen(term);
        int row = RowHolding(s, @"Serving HTTP");
        int col = ColumnOf(s, row, @"http://127.0.0.1");
        GCHECK(row >= 0 && col >= 0);
        if (col < 0) return;
        [c.window makeFirstResponder:grid];
        WithCommandHover(grid, row, col + 5, ^{ [grid updateLinkHover]; });
        GCHECK([[grid valueForKey:@"linkId"] longLongValue] != LLONG_MIN);
        GCHECK([[grid valueForKey:@"linkCol0"] intValue] == col);
        GCHECK(grid.toolTip == nil);   // a link read from the text says where it goes
        Click(grid, row, col + 5, NSEventModifierFlagCommand);
    });
    MCGuiWait(@"the page is in the pane, the user's", 20, ^BOOL { return PaneShows(c, @"Served"); });
    MCGuiStep(@"the keyboard stayed in the terminal", ^{
        GCHECK(c.window.firstResponder == grid);
        GCHECK([[Pane(c).userProbe.url description] isEqualToString:Local(@"/")]);
        GCHECK(!Pane(c).hasAgentPage);
        GCHECK([Recorded() isEqualToString:@""]);
    });
    MCGuiStep(@"an address on 0.0.0.0", ^{
        [term runCommand:[NSString stringWithFormat:@"clear; echo 'at http://0.0.0.0:%d/zero.html'", gPort]];
    });
    MCGuiWait(@"it is printed", 10, ^BOOL { return term.atPrompt && RowStarting(Screen(term), @"at http") >= 0; });
    MCGuiStep(@"Cmd+click on it", ^{
        TerminalScreen *s = Screen(term);
        int row = RowStarting(s, @"at http");
        Click(grid, row, 8, NSEventModifierFlagCommand);
    });
    MCGuiWait(@"localhost's page loads", 20, ^BOOL { return PaneShows(c, @"Zero"); });
    MCGuiStep(@"as localhost", ^{
        GCHECK_EQ_STR(Pane(c).userProbe.url,
                      ([NSString stringWithFormat:@"http://localhost:%d/zero.html", gPort]));
    });

    // ------------------------------------------------------------ OSC 8
    MCGuiStep(@"OSC 8 hyperlinks printed by printf", ^{
        [term runCommand:[NSString stringWithFormat:
            @"clear; printf '\\e]8;;%@\\e\\\\Open the app\\e]8;;\\e\\\\ beside\\n"
            @"\\e]8;;file://%%s%%s\\e\\\\the notes\\e]8;;\\e\\\\\\n"
            @"\\e]8;;javascript:alert(1)\\e\\\\Not a link\\e]8;;\\e\\\\\\n' \"$(hostname)\" \"$PWD/notes.txt\"",
            Local(@"/osc8.html")]];
    });
    MCGuiWait(@"they are printed", 10, ^BOOL {
        return term.atPrompt && RowStarting(Screen(term), @"Not a link") >= 0;
    });
    MCGuiStep(@"Cmd hovering the first underlines its text and shows its address", ^{
        TerminalScreen *s = Screen(term);
        int row = RowStarting(s, @"Open the app");
        GCHECK(row >= 0 && s->lineCell(row, 0).style.link != 0 && s->lineCell(row, 12).style.link == 0);
        GCHECK(s->linkURI(s->lineCell(row, 0).style.link) == std::string(Local(@"/osc8.html").UTF8String));
        WithCommandHover(grid, row, 5, ^{ [grid updateLinkHover]; });
        GCHECK([[grid valueForKey:@"linkCol0"] intValue] == 0);
        GCHECK([[grid valueForKey:@"linkCol1"] intValue] == 12);
        GCHECK_EQ_STR(grid.toolTip, Local(@"/osc8.html"));
        int bad = RowStarting(s, @"Not a link");
        GCHECK(bad >= 0 && s->lineCell(bad, 0).style.link == 0);
        WithCommandHover(grid, bad, 2, ^{ [grid updateLinkHover]; });
        GCHECK([[grid valueForKey:@"linkId"] longLongValue] == LLONG_MIN);
        GCHECK(grid.toolTip == nil);
        Click(grid, row, 5, NSEventModifierFlagCommand);
    });
    MCGuiWait(@"its address loads in the pane", 20, ^BOOL { return PaneShows(c, @"Hyperlink"); });
    MCGuiStep(@"Cmd+click on the file one", ^{
        TerminalScreen *s = Screen(term);
        int row = RowStarting(s, @"the notes");
        GCHECK(row >= 0 && s->lineCell(row, 0).style.link != 0);
        Click(grid, row, 3, NSEventModifierFlagCommand);
    });
    MCGuiWait(@"notes.txt opens in the editor", 10, ^BOOL {
        return [[c valueForKey:@"currentPath"] hasSuffix:@"/notes.txt"];
    });

    // ------------------------------------ a program that takes the mouse
    MCGuiStep(@"a program with mouse reporting on", ^{
        [c.window makeFirstResponder:grid];
        [term runCommand:[NSString stringWithFormat:@"clear; python3 mouse_open.py %@ %@",
                                                    Local(@"/cmd.html"), Local(@"/mouse.html")]];
    });
    MCGuiWait(@"it waits for a click", 15, ^BOOL {
        return Screen(term)->mouseTracking() == 1000 && RowStarting(Screen(term), @"click the link") >= 0;
    });
    MCGuiStep(@"Cmd still finds the address, and a Cmd+click is minicode's", ^{
        TerminalScreen *s = Screen(term);
        int row = RowStarting(s, @"click the link");
        int col = ColumnOf(s, row, @"http://");
        WithCommandHover(grid, row, col + 3, ^{ [grid updateLinkHover]; });
        GCHECK([[grid valueForKey:@"linkCol0"] intValue] == col);
        Click(grid, row, col + 3, NSEventModifierFlagCommand);
    });
    MCGuiWait(@"the address shown loads", 20, ^BOOL { return PaneShows(c, @"Cmd"); });
    MCGuiStep(@"the program saw nothing; now a plain click, which it takes", ^{
        GCHECK(Screen(term)->mouseTracking() == 1000);
        GCHECK(term.busy);
        int row = RowStarting(Screen(term), @"click the link");
        Click(grid, row, 3, 0);
    });
    MCGuiWait(@"its webbrowser.open lands in the pane through BROWSER", 20, ^BOOL {
        return PaneShows(c, @"Mouse");
    });
    MCGuiWait(@"and it was told so", 10, ^BOOL {
        return term.atPrompt && [[term recentText:5] containsString:@"opened True"];
    });
    MCGuiStep(@"the keyboard is still the terminal's, nothing went to the default browser", ^{
        GCHECK(c.window.firstResponder == grid);
        GCHECK([Recorded() isEqualToString:@""]);
        GCHECK(!Pane(c).hasAgentPage);
    });

    // ----------------------------------------- what goes somewhere else
    MCGuiStep(@"a page that is not local, through webbrowser", ^{
        [term runCommand:@"python3 -c 'import webbrowser; print(\"gave\", "
                         @"webbrowser.open(\"https://example.invalid/sign-in\"))'"];
    });
    MCGuiWait(@"it went to the default browser (the recorder)", 15, ^BOOL {
        return term.atPrompt && [Recorded() containsString:@"https://example.invalid/sign-in"];
    });
    MCGuiStep(@"the pane kept its page", ^{
        GCHECK(PaneShows(c, @"Mouse"));
        GCHECK([[term recentText:5] containsString:@"gave True"]);
    });
    MCGuiStep(@"a minicode that does not answer", ^{
        [term runCommand:[NSString stringWithFormat:@"MINICODE_PID=999999 \"$BROWSER\" %@; echo exit=$?",
                                                    Local(@"/dead.html")]];
    });
    MCGuiWait(@"the default browser has it", 15, ^BOOL {
        return term.atPrompt && [Recorded() containsString:Local(@"/dead.html")];
    });
    MCGuiStep(@"and the helper said all was well", ^{
        GCHECK([[term recentText:5] containsString:@"exit=0"]);
        GCHECK(PaneShows(c, @"Mouse"));
    });
    if (node) {
        MCGuiStep(@"node runs the helper as Vite's --open does", ^{
            [term runCommand:[NSString stringWithFormat:@"node \"$BROWSER\" --open --port 5173 %@",
                                                        Local(@"/vite.html")]];
        });
        MCGuiWait(@"the page lands in the pane", 20, ^BOOL { return PaneShows(c, @"Vite"); });
    } else {
        MCGuiStep(@"no node here", ^{ MCGuiNote(@"node not found: the .js path is unchecked"); });
    }

    MCGuiStep(@"the server stops and the suite's window closes", ^{
        unsetenv("MINICODE_OPEN_FALLBACK");
        [term runCommand:@"kill %1"];
    });
    MCGuiWait(@"the server is gone", 10, ^BOOL { return term.atPrompt && !term.busy; });
    MCGuiStep(@"close", ^{
        GCHECK([Recorded() componentsSeparatedByString:@"\n"].count == 3);   // two, and the last newline
        [c.window close];
        c = nil;
        term = nil;
        grid = nil;
        MCGuiSetKeyWindow(nil);
    });
}
