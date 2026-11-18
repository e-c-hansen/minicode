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
//   - The second client leaving keeps the terminal going.
#import "Harness.h"
#import "EditorController.h"
#import "Terminal.h"
#import "TerminalPanel.h"
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
}
@end

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
