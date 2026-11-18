// TerminalHostSuite.mm — `make guitest SUITES=terminal_host`: with
// editor.session-host on, a grid terminal's pty belongs to the session's
// host (SESSION-DESIGN.md stage 5; docs/notes/session-host.md,
// "Terminals"), so a second screen of the same session sees it and types
// into it.
//   - The suite turns the setting on itself, so it runs in both of CI's
//     passes; a terminal opened then is on the host, with the shell the
//     host started (its pid in the host's state).
//   - A command typed in the Mac's terminal shows on the host's own screen
//     and on a second renderer's: a scripted protocol client connected to
//     the process's host (MCSessionConnectViewer), attached to the
//     terminal, its replay and live output fed to a TerminalScreen of its
//     own.
//   - A command the second client types (TermInput) runs in the Mac's
//     terminal: its output on the Mac's screen, the marks seeing it end.
//   - The size follows whoever typed last (the core's Host::sizer): the
//     Mac types, the second client says a smaller size and nothing moves;
//     it types and the pty, the host's screen and the Mac's grid take its
//     size, the grid drawn at the top left of the larger view; the Mac
//     types and its own size is back; the second's answer to a query
//     (TermInput flag 1) moves nothing; a larger size the second types at
//     is drawn clipped, the cursor in sight.
//   - The second client leaving keeps the terminal going.
#import "Harness.h"
#import "EditorController.h"
#import "Terminal.h"
#import "TerminalPanel.h"
#import "TerminalGridView.h"
#import "SessionLink.h"

#include <memory>

#include "SessionClient.h"
#include "TerminalScreen.h"

namespace {

// The second screen: what the host forwards, fed to a screen of its own.
struct SecondScreen : Shared::Client::TermListener {
    Shared::Client *client = nullptr;
    int cols = 80, rows = 24;
    std::unique_ptr<TerminalScreen> screen;
    bool replayed = false, exited = false;
    void termOutput(Shared::TermId t, const std::string &bytes, long long, bool, bool replay) override {
        if (replay || !screen) {
            screen = std::make_unique<TerminalScreen>(cols, rows);
            replayed = replayed || replay;
        }
        screen->feed(bytes);
        screen->takeReplies();   // the Mac's screen answers queries
        client->ackTerminal(t, bytes.size());
    }
    void termExited(Shared::TermId, int, const std::string &) override { exited = true; }
    std::string text() const {
        std::string out;
        if (!screen) return out;
        for (int r = -screen->historySize(); r < screen->rows(); r++) out += screen->lineText(r) + "\n";
        return out;
    }
};

}  // namespace

@interface MCTermHostState : NSObject
@property(nonatomic, strong) EditorController *c;
@property(nonatomic, strong) TerminalView *term;
@end
@implementation MCTermHostState {
@public
    std::unique_ptr<Shared::Client> client;
    std::function<void()> letGo;
    SecondScreen second;
    int macCols, macRows;   // the Mac's view, in cells
    int smallCols, smallRows;   // the second's smaller view
}
@end

// The pty's size as the host's state says it.
static std::pair<int, int> MCHostTermSize(Shared::Client &client, unsigned long long term) {
    const Json st = client.sessionState();
    for (const Json &t : st["terminals"].elements())
        if ((unsigned long long)t["term"].asInt() == term) return {(int)t["cols"].asInt(), (int)t["rows"].asInt()};
    return {0, 0};
}

static TerminalScreen *MCGridScreen(TerminalView *term) {
    return ((TerminalGridView *)[term valueForKey:@"grid"]).screen;
}

MC_GUI_SUITE(terminal_host) {
    MCTermHostState *s = [MCTermHostState new];
    MCGuiStep(@"the session's host on; a terminal of its own", ^{
        MCGuiSetSettings(@"lsp.enabled = false\neditor.session-host = on\n");
        GCHECK(MCSessionHostEnabled());
        s.c = MCGuiController();
        [s.c newTerminal:nil];
        s.term = [s.c valueForKey:@"terminal"];
        GCHECK(s.term != nil && !s.term.lineMode);
        GCHECK(s.term.onSessionHost && s.term.sessionTerminal != 0);
        GCHECK(s.term.shellPid > 0);
        const MCSessionTerminalState h = MCSessionInspectTerminal(s.term.sessionTerminal);
        GCHECK(h.fd >= 0 && h.pid == s.term.shellPid);
    });
    MCGuiWait(@"its prompt", 20, ^BOOL { return s.term.atPrompt && !s.term.busy; });
    MCGuiStep(@"a command typed on the Mac", ^{
        [s.term runCommand:[NSString stringWithFormat:@"cd '%@' && echo mac-side-$((6*7))", MCGuiProject()]];
    });
    MCGuiWait(@"it ran", 10, ^BOOL {
        return [[s.term recentText:20] containsString:@"\nmac-side-42"] && s.term.atPrompt && !s.term.busy;
    });
    MCGuiWait(@"the host's own screen has it", 5, ^BOOL {
        return MCSessionInspectTerminal(s.term.sessionTerminal).screen.find("\nmac-side-42") != std::string::npos;
    });

    MCGuiStep(@"a second renderer connects and attaches", ^{
        s->client = std::make_unique<Shared::Client>(Model::Unit::Utf16, "second screen");
        s->letGo = MCSessionConnectViewer(*s->client);
        s->second.client = s->client.get();
        const Json st = s->client->sessionState();
        bool found = false;
        for (const Json &t : st["terminals"].elements()) {
            if ((unsigned long long)t["term"].asInt() != s.term.sessionTerminal) continue;
            found = true;
            GCHECK((pid_t)t["pid"].asInt() == s.term.shellPid);
            GCHECK(t["renderers"].asInt() == 1);   // the Mac's
            s->second.cols = (int)t["cols"].asInt(80);
            s->second.rows = (int)t["rows"].asInt(24);
            const Shared::TermShell sh = Shared::termShellFrom(t["shell"]);
            GCHECK(sh.known && sh.atPrompt && !sh.busy);
            GCHECK(sh.cwd.find(MCGuiProject().lastPathComponent.UTF8String) != std::string::npos);
        }
        GCHECK(found);
        s->client->attachTerminal(s.term.sessionTerminal, &s->second);
    });
    MCGuiWait(@"its screen shows the Mac's command", 10, ^BOOL {
        s->client->dispatch();
        return s->second.replayed && s->second.text().find("\nmac-side-42") != std::string::npos;
    });

    MCGuiStep(@"the second renderer types a command", ^{
        GCHECK(s.term.atPrompt && !s.term.busy);
        s->client->terminalInput(s.term.sessionTerminal, "echo from-second-$((6*8))\r");
    });
    MCGuiWait(@"it runs in the Mac's terminal and both screens show it", 10, ^BOOL {
        s->client->dispatch();
        return [[s.term recentText:20] containsString:@"\nfrom-second-48"] && s.term.atPrompt &&
               !s.term.busy && s->second.text().find("\nfrom-second-48") != std::string::npos;
    });
    MCGuiStep(@"the Mac's marks saw it end", ^{
        GCHECK([s.term.lastCommandOutput hasPrefix:@"from-second-48"]);
        if (![s.term.lastCommandOutput hasPrefix:@"from-second-48"])
            MCGuiNote(@"last command output: %@", s.term.lastCommandOutput);
    });

    // ---- the size follows the typist
    // The second typed last (above), with no size said: the Mac types now,
    // so its size is the one a later size from the second must not move.
    MCGuiStep(@"the Mac types again", ^{
        [s.term runCommand:@"echo mac-typed"];
    });
    MCGuiWait(@"it ran", 10, ^BOOL {
        return [[s.term recentText:20] containsString:@"\nmac-typed"] && s.term.atPrompt && !s.term.busy;
    });
    MCGuiStep(@"the second says a smaller size: nothing moves until it types", ^{
        TerminalScreen *g = MCGridScreen(s.term);
        s->macCols = g->cols();
        s->macRows = g->rows();
        s->smallCols = MAX(20, s->macCols / 2);
        s->smallRows = MAX(4, s->macRows / 2);
        GCHECK(s->smallCols < s->macCols && s->smallRows < s->macRows);
        GCHECK(MCHostTermSize(*s->client, s.term.sessionTerminal) == std::make_pair(s->macCols, s->macRows));
        s->client->resizeTerminal(s.term.sessionTerminal, s->smallCols, s->smallRows);
        GCHECK(s->client->settle());
        const auto z = MCHostTermSize(*s->client, s.term.sessionTerminal);
        GCHECK(z == std::make_pair(s->macCols, s->macRows));
        if (z != std::make_pair(s->macCols, s->macRows)) MCGuiNote(@"pty %dx%d", z.first, z.second);
        s->client->terminalInput(s.term.sessionTerminal, "stty size\r");
    });
    MCGuiWait(@"the pty, the host and the Mac's grid take the second's size", 10, ^BOOL {
        s->client->dispatch();
        TerminalScreen *g = MCGridScreen(s.term);
        NSString *line = [NSString stringWithFormat:@"\n%d %d", s->smallRows, s->smallCols];
        return g->cols() == s->smallCols && g->rows() == s->smallRows &&
               [[s.term recentText:20] containsString:line] && s.term.atPrompt && !s.term.busy;
    });
    MCGuiStep(@"the smaller grid is drawn at the top left of the Mac's view", ^{
        TerminalGridView *grid = [s.term valueForKey:@"grid"];
        GCHECK(NSEqualRects(grid.frame, s.term.bounds));
        GCHECK(MCHostTermSize(*s->client, s.term.sessionTerminal) == std::make_pair(s->smallCols, s->smallRows));
        [s.term runCommand:@"stty size"];   // the Mac types
    });
    MCGuiWait(@"the Mac's size is back", 10, ^BOOL {
        TerminalScreen *g = MCGridScreen(s.term);
        NSString *line = [NSString stringWithFormat:@"\n%d %d", s->macRows, s->macCols];
        return g->cols() == s->macCols && g->rows() == s->macRows &&
               [[s.term recentText:20] containsString:line] && s.term.atPrompt && !s.term.busy;
    });
    MCGuiStep(@"an answer to a query is not typing", ^{
        // Control U at zsh's empty prompt: the line goes, nothing else.
        s->client->terminalInput(s.term.sessionTerminal, "\x15", true);
        GCHECK(s->client->settle());
        GCHECK(MCHostTermSize(*s->client, s.term.sessionTerminal) == std::make_pair(s->macCols, s->macRows));
        // A size larger than the Mac's view, and the second types there.
        s->client->resizeTerminal(s.term.sessionTerminal, s->macCols + 40, s->macRows + 20);
        s->client->terminalInput(s.term.sessionTerminal, "stty size\r");
    });
    MCGuiWait(@"the Mac's grid takes the larger size", 10, ^BOOL {
        s->client->dispatch();
        TerminalScreen *g = MCGridScreen(s.term);
        NSString *line = [NSString stringWithFormat:@"\n%d %d", s->macRows + 20, s->macCols + 40];
        return g->cols() == s->macCols + 40 && g->rows() == s->macRows + 20 &&
               [[s.term recentText:20] containsString:line] && s.term.atPrompt && !s.term.busy;
    });
    MCGuiStep(@"it is clipped to the view, the cursor in sight", ^{
        TerminalGridView *grid = [s.term valueForKey:@"grid"];
        TerminalScreen *g = grid.screen;
        const NSRect b = s.term.bounds, f = grid.frame;
        GCHECK(NSWidth(f) > NSWidth(b) && NSHeight(f) > NSHeight(b));
        // The cursor's cell, in the terminal view's coordinates.
        const NSSize cs = [TerminalGridView cellSize];
        const CGFloat pad = [TerminalGridView padding];
        const NSRect cell = [s.term convertRect:NSMakeRect(pad + g->cursorCol() * cs.width,
                                                           pad + g->cursorRow() * cs.height,
                                                           cs.width, cs.height)
                                       fromView:grid];
        GCHECK(NSContainsRect(b, cell));
        if (!NSContainsRect(b, cell))
            MCGuiNote(@"cursor %@ outside %@ (grid %@)", NSStringFromRect(cell), NSStringFromRect(b),
                      NSStringFromRect(f));
        [s.term runCommand:@"echo mac-again"];   // the Mac types: its size
    });
    MCGuiWait(@"the Mac's size again", 10, ^BOOL {
        TerminalScreen *g = MCGridScreen(s.term);
        return g->cols() == s->macCols && g->rows() == s->macRows &&
               [[s.term recentText:20] containsString:@"\nmac-again"] && s.term.atPrompt && !s.term.busy;
    });
    MCGuiStep(@"the grid fills the view again", ^{
        TerminalGridView *grid = [s.term valueForKey:@"grid"];
        GCHECK(NSEqualRects(grid.frame, s.term.bounds));
    });

    MCGuiStep(@"the second renderer leaves; the terminal goes on", ^{
        s->letGo();
        s->letGo = nullptr;
        s->client.reset();
        GCHECK(!s->second.exited);
        [s.term runCommand:@"echo still-here"];
    });
    MCGuiWait(@"the terminal still runs commands", 10, ^BOOL {
        return [[s.term recentText:20] containsString:@"\nstill-here"] && s.term.atPrompt && !s.term.busy;
    });
    MCGuiStep(@"close it", ^{
        const unsigned long long id = s.term.sessionTerminal;
        GCHECK([(MCTerminalPanel *)[s.c valueForKey:@"terminalPanel"] closeTerminal:s.term]);
        s.term = nil;
        GCHECK(MCSessionInspectTerminal(id).fd == -1);
    });
}
