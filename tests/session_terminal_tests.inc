// session_terminal_tests.inc — the `screen:replay`, `pty:*` and `term:*`
// groups of run_tests.cpp: terminals owned by the session's host
// (SESSION-DESIGN.md, stage 5). TerminalScreen::replay() against the screen
// it rebuilds, under a fuzz; a shell on a pty (HostPty.h) as the Mac's
// terminal_pty suite checks MCPty (the shell reaped, the fd closed, one
// ignoring SIGHUP killed, a megabyte into a program that does not read,
// closing during `yes`); and the host's terminals through a client: output,
// the integration's marks and state, replay to a renderer attaching, exit
// statuses, resizing and the output credit. Real processes (/bin/sh),
// under TSan in CI: the host's thread does the pty I/O. Included in
// run_tests.cpp after session_host_tests.inc.

#include <csignal>
#include <fcntl.h>
#include <limits.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "HostPty.h"

namespace {

using Shared::Message;

// What differs between two screens, "" when nothing: every line of the
// scrollback and the screen (characters, widths, styles, links by address,
// underline colors by color, soft wraps), the cursor, the modes, the title
// and directory, and the marked commands, lines counted from the top of
// the scrollback.
std::string screenDiff(const TerminalScreen& a, const TerminalScreen& b) {
    auto num = [](long long n) { return std::to_string(n); };
    if (a.cols() != b.cols() || a.rows() != b.rows()) return "size";
    if (a.historySize() != b.historySize())
        return "scrollback " + num(a.historySize()) + " against " + num(b.historySize());
    for (int line = -a.historySize(); line < a.rows(); line++) {
        const std::string where = "line " + num(line);
        // A scrollback line's width may differ by blanks at its end (one
        // that wrapped when it went up, unwrapped since, kept them): the
        // cells are compared.
        // (replay() says why a bottom row continuing a wrap may not wrap.)
        const bool bottomOfWrap = line == a.rows() - 1 && (a.lineWraps(line - 1) || a.rows() < 3);
        if (a.lineWraps(line) != b.lineWraps(line) && !bottomOfWrap) return where + " wrap";
        const int w = std::max(a.lineWidth(line), b.lineWidth(line));
        for (int c = 0; c < w; c++) {
            const TermCell x = a.lineCell(line, c), y = b.lineCell(line, c);
            TermStyle sx = x.style, sy = y.style;
            sx.link = sy.link = 0;
            sx.underlineColor = sy.underlineColor = 0;
            const std::string at = where + " col " + num(c);
            if (x.ch != y.ch || x.width != y.width)
                return at + " [" + x.ch + "/" + num(x.width) + "] against [" + y.ch + "/" + num(y.width) +
                       "] in [" + a.lineText(line) + "] and [" + b.lineText(line) + "]";
            if (!(sx == sy)) return at + " style";
            if (a.linkURI(x.style.link) != b.linkURI(y.style.link)) return at + " link";
            if (a.underlineColor(x.style.underlineColor) != b.underlineColor(y.style.underlineColor))
                return at + " underline color";
        }
    }
    if (a.cursorRow() != b.cursorRow() || a.cursorCol() != b.cursorCol())
        return "cursor " + num(a.cursorRow()) + "," + num(a.cursorCol()) + " against " +
               num(b.cursorRow()) + "," + num(b.cursorCol());
    if (a.altScreen() != b.altScreen()) return "alternate screen";
    const struct { const char* name; bool same; } modes[] = {
        {"application cursor keys", a.appCursorKeys() == b.appCursorKeys()},
        {"application keypad", a.appKeypad() == b.appKeypad()},
        {"bracketed paste", a.bracketedPaste() == b.bracketedPaste()},
        {"cursor visible", a.cursorVisible() == b.cursorVisible()},
        {"autowrap", a.autowrap() == b.autowrap()},
        {"mouse tracking", a.mouseTracking() == b.mouseTracking()},
        {"mouse encoding", a.mouseEncoding() == b.mouseEncoding()},
        {"focus events", a.focusEvents() == b.focusEvents()},
        {"synchronized output", a.synchronizedOutput() == b.synchronizedOutput()},
        {"keyboard flags", a.keyboardFlags() == b.keyboardFlags()},
        {"cursor shape", a.cursorShape() == b.cursorShape()},
        {"cursor blink", a.cursorBlinks() == b.cursorBlinks()},
        {"cluster widths", a.clusterWidths() == b.clusterWidths()},
    };
    for (const auto& m : modes)
        if (!m.same) return std::string("mode: ") + m.name;
    if (a.scrollTop() != b.scrollTop() || a.scrollBottom() != b.scrollBottom()) return "scroll region";
    if (a.title() != b.title()) return "title";
    if (a.directory() != b.directory()) return "directory [" + a.directory() + "] [" + b.directory() + "]";
    const std::vector<TermCommand> ca = a.commands(), cb = b.commands();
    if (ca.size() != cb.size()) return "commands " + num(ca.size()) + " against " + num(cb.size());
    for (size_t i = 0; i < ca.size(); i++) {
        const TermCommand &x = ca[i], &y = cb[i];
        if (x.promptLine != y.promptLine || x.promptCol != y.promptCol || x.ran != y.ran ||
            (x.ran && (x.outputLine != y.outputLine || x.outputCol != y.outputCol)) || x.ended != y.ended ||
            (x.ended && (x.endLine != y.endLine || x.endCol != y.endCol || x.status != y.status)))
            return "command " + num((long long)i) + " (prompt " + num(x.promptLine) + "," + num(x.promptCol) +
                   " against " + num(y.promptLine) + "," + num(y.promptCol) + ")";
    }
    return "";
}

// A screen rebuilt from `a`'s replay, as a renderer attaching would have it.
std::string replayDiff(const TerminalScreen& a, const std::string& nonce) {
    TerminalScreen b(a.cols(), a.rows());
    b.setScrollbackLimit(a.scrollbackLimit());
    b.setMarkNonce(nonce);
    b.feed(a.replay(nonce));
    std::string d = screenDiff(a, b);
    if (!d.empty() || !a.altScreen()) return d;
    // The main screen under the alternate one, and the cursor saved with it.
    TerminalScreen a2 = a;
    a2.feed("\x1b[?1049l");
    b.feed("\x1b[?1049l");
    d = screenDiff(a2, b);
    return d.empty() ? d : "under the alternate screen: " + d;
}

// Output much like a shell's and its programs', at random: words, colors
// and the other styles, wide and combining characters, line ends and long
// lines that wrap, cursor movement and erasing, scroll regions, links, the
// integration's marks (some without the nonce), the alternate screen,
// modes, the saved cursor, tab stops and the line-drawing set.
std::string randomTerminalOutput(std::mt19937& rng, int pieces, const std::string& nonce) {
    auto pick = [&](int n) { return (int)(rng() % (unsigned)n); };
    auto mark = [&](const std::string& body) {
        return "\x1b]" + body + (pick(8) ? ";mc=" + nonce : std::string()) + "\a";
    };
    static const char* const words[] = {"ls", "make", "error:", "src/main.cpp:42:7", "ok", "漢字", "😀",
                                        "cafe\xcc\x81", "naïve", "------", "x", "  "};
    std::string out;
    for (int i = 0; i < pieces; i++) {
        switch (pick(30)) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 6:
            out += words[pick(12)];
            out += ' ';
            break;
        case 7: case 8: out += "\r\n"; break;
        case 9: out += std::string((size_t)(20 + pick(150)), (char)('a' + pick(26))); break;
        case 10: {
            static const char* const sgr[] = {"0", "1", "3", "4", "4:3", "7", "9", "53", "31", "92",
                                              "38;5;208", "38;2;10;20;30", "48;5;17", "44",
                                              "58;5;196", "58;2;1;2;3", "22", "24", "39;49", "2;8"};
            out += std::string("\x1b[") + sgr[pick(20)] + "m";
            break;
        }
        case 11: out += "\x1b[" + std::to_string(1 + pick(30)) + ";" + std::to_string(1 + pick(90)) + "H"; break;
        case 12: out += std::string("\x1b[") + "012"[pick(3)] + "K"; break;
        case 13: out += pick(3) ? "\x1b[J" : "\x1b[1J"; break;
        case 14: out += mark("133;A"); out += "$ "; break;
        case 15: out += mark("633;E;make test") + mark("133;C"); break;
        case 16: out += mark("133;D;" + std::to_string(pick(3))); break;
        case 17: out += pick(2) ? "\x1b]8;;https://example.com/" + std::to_string(pick(4)) + "\x1b\\"
                                : "\x1b]8;;\x1b\\";
            break;
        case 18: out += pick(2) ? "\x1b[?1049h" : "\x1b[?1049l"; break;
        case 19: {
            static const char* const modes[] = {"?1h", "?1l", "?2004h", "?1000h", "?1006h", "?25l",
                                                "?25h", "?7l", "?7h", "4h", "4l", "?1004h", "?12h"};
            out += std::string("\x1b[") + modes[pick(13)];
            break;
        }
        case 20: out += pick(2) ? "\x1b" "7" : "\x1b" "8"; break;
        case 21: out += "\x1b[" + std::to_string(1 + pick(5)) + ";" + std::to_string(6 + pick(10)) + "r"; break;
        case 22: out += "\x1b[r"; break;
        case 23: out += pick(2) ? "\x1b" "M" : "\x1b[" + std::to_string(1 + pick(3)) + (pick(2) ? "L" : "M"); break;
        case 24: out += pick(2) ? "\t" : "\x1bH"; break;
        case 25: out += pick(2) ? "\x1b(0lqqk\x1b(B" : "\x0e" "x\x0f"; break;
        case 26: out += "\x1b]2;title " + std::to_string(pick(9)) + "\a"; break;
        case 27: out += mark("7;file://host/tmp/d%3Bir" + std::to_string(pick(3))); break;
        case 28: out += std::string("\x1b[") + "0123456"[pick(7)] + " q"; break;
        case 29: out += pick(2) ? "\x1b[>1u" : "\x1b[<u"; break;
        }
    }
    return out;
}

// /tmp/mc-term-XXXXXX, made for a test.
std::string scratchFolder() {
    char tmpl[] = "/tmp/mc-term-XXXXXX";
    const char* made = mkdtemp(tmpl);
    return made ? made : "/tmp";
}

double nowMs() {
    using namespace std::chrono;
    return (double)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count() / 1000.0;
}

bool gone(int pid) { return pid > 0 && kill(pid, 0) != 0 && errno == ESRCH; }
bool fdClosed(int fd) { return fd >= 0 && fcntl(fd, F_GETFD) == -1 && errno == EBADF; }

// The host on its thread and one renderer's client on this one, with a
// terminal's listener that feeds its own screen (the renderer's emulator)
// and acknowledges what it takes.
struct TermRig : Shared::Client::TermListener {
    std::unique_ptr<Shared::Client> client;
    std::unique_ptr<Shared::SessionHost> host;
    Shared::ClientId id = 0;
    bool loopback = false;
    bool ack = true;
    std::string out;
    long long received = 0;
    int pid = 0;
    bool exited = false;
    int status = -2;
    std::string error;
    Shared::TermShell shell;
    std::vector<Shared::TermMark> marks;
    long long cursorLine = -1;
    bool alt = false;
    int replays = 0;
    TerminalScreen mirror{80, 24};

    explicit TermRig(bool wire = false) : loopback(wire) {
        client = std::make_unique<Shared::Client>(Model::Unit::CodePoint, "test");
        host = std::make_unique<Shared::SessionHost>([this](Shared::ClientId, Message&& m) {
            client->deliver(loopback ? Shared::roundTrip(m) : std::move(m));
        });
        id = host->connect();
        client->connect([this](Message&& m) { host->post(id, loopback ? Shared::roundTrip(m) : std::move(m)); });
    }
    ~TermRig() { host.reset(); }   // its thread first: it delivers to the client

    void termOpened(Shared::TermId, int p) override { pid = p; }
    void termOutput(Shared::TermId t, const std::string& b, long long line, bool a, bool replay) override {
        out += b;
        received += (long long)b.size();
        mirror.feed(b);
        cursorLine = line;
        alt = a;
        if (replay) replays++;
        if (ack && !replay) client->ackTerminal(t, b.size());
    }
    void termShell(Shared::TermId, const Shared::TermShell& s) override { shell = s; }
    void termMarks(Shared::TermId, const std::vector<Shared::TermMark>& m) override { marks = m; }
    void termExited(Shared::TermId, int st, const std::string& e) override {
        exited = true;
        status = st;
        error = e;
    }

    Shared::TermSpec spec(const std::string& program, std::vector<std::string> argv) {
        static const std::string home = scratchFolder();
        Shared::TermSpec s;
        s.program = program;
        s.argv = std::move(argv);
        s.env = {"PATH=/usr/bin:/bin", "HOME=" + home, "TERM=xterm-256color", "PS1=$ ", "LANG=C", "ENV="};
        s.cwd = home;
        s.cols = 80;
        s.rows = 24;
        s.scrollback = 1000;
        return s;
    }
    void open(Shared::TermId t, const Shared::TermSpec& s) {
        mirror = TerminalScreen(s.cols, s.rows);
        mirror.setScrollbackLimit(s.scrollback);
        mirror.setMarkNonce(s.nonce);
        client->openTerminal(t, s, this);
    }
    void type(Shared::TermId t, const std::string& s) { client->terminalInput(t, s); }
    // Dispatches the host's messages until `done`, at most `ms`.
    bool wait(const std::function<bool()>& done, int ms = 8000) {
        const double end = nowMs() + ms;
        for (;;) {
            client->dispatch();
            if (done()) return true;
            if (nowMs() > end) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    // The host's counts of terminals and shells waiting to be reaped.
    std::pair<size_t, size_t> counts() {
        std::pair<size_t, size_t> c;
        host->inspect([&](const Shared::Host& h) { c = {h.terminalCount(), h.dyingCount()}; });
        return c;
    }
    bool settled(int ms = 8000) {
        return wait([&] { return counts() == std::pair<size_t, size_t>(0, 0); }, ms);
    }
    int fd(Shared::TermId t) {
        int f = -1;
        host->inspect([&](const Shared::Host& h) { f = h.ptyFd(t); });
        return f;
    }
    bool has(const std::string& s) const { return out.find(s) != std::string::npos; }
};

}  // namespace

static void testSessionTerminals() {
    GROUP("screen:replay");
    {
        // A few screens by hand: plain lines, a long line wrapped, a wide
        // character at the edge, colors and a link, then the scrollback.
        TerminalScreen a(20, 5);
        a.setScrollbackLimit(100);
        a.feed("hello\r\n\x1b[31mred\x1b[0m and \x1b]8;;https://x.org/\x1b\\link\x1b]8;;\x1b\\\r\n");
        a.feed(std::string(45, 'w') + "\r\n0123456789012345678漢x\r\n");
        for (int i = 0; i < 12; i++) a.feed("line " + std::to_string(i) + "\r\n");
        a.feed("\x1b[44m   \x1b[K\x1b[0mtail");
        CHECK(a.historySize() > 0);
        std::string d = replayDiff(a, "");
        if (!d.empty()) std::printf("  replay: %s\n", d.c_str());
        CHECK(d.empty());

        // Marks with the nonce, a failed command, and the alternate screen
        // over them with a program's modes.
        TerminalScreen m(30, 8);
        m.setScrollbackLimit(50);
        m.setMarkNonce("n0");
        m.feed("\x1b]133;A;mc=n0\a$ \x1b]133;B;mc=n0\afalse\r\n\x1b]133;C;mc=n0\a");
        m.feed("\x1b]133;D;1;mc=n0\a\x1b]133;A;mc=n0\a$ ls\r\n\x1b]133;C;mc=n0\aa b c\r\n");
        m.feed("\x1b]133;D;0;mc=n0\a\x1b]133;A;mc=n0\a$ \x1b]7;file://h/tmp/x;mc=n0\a");
        CHECK(m.commands().size() == 3);
        d = replayDiff(m, "n0");
        if (!d.empty()) std::printf("  replay with marks: %s\n", d.c_str());
        CHECK(d.empty());
        m.feed("\x1b[?1049h\x1b[?1h\x1b[?2004h\x1b[?1000h\x1b[?1006h\x1b[2;6r\x1b[3;4H\x1b[1mvim\x1b[5 q");
        d = replayDiff(m, "n0");
        if (!d.empty()) std::printf("  replay over the alternate screen: %s\n", d.c_str());
        CHECK(d.empty());

        // The fuzz: random output into screens of a few sizes, replayed.
        std::mt19937 rng(20261117);
        int failed = 0;
        for (int round = 0; round < 300; round++) {
            const int cols = 6 + (int)(rng() % 90), rows = 2 + (int)(rng() % 30);
            TerminalScreen s(cols, rows);
            s.setScrollbackLimit(round % 3 ? 200 : 20);
            s.setMarkNonce("fz");
            s.feed(randomTerminalOutput(rng, 40 + (int)(rng() % 400), "fz"));
            const std::string why = replayDiff(s, "fz");
            if (!why.empty() && failed++ < 3)
                std::printf("  replay fuzz round %d (%dx%d): %s\n", round, cols, rows, why.c_str());
        }
        CHECK(failed == 0);
    }

    GROUP("pty:spawn");
    {
        // A shell on a pty: what it is given (argv, environment, folder,
        // size), its output read without blocking, and its exit reaped.
        const std::string dir = scratchFolder();
        Shared::PtySpec spec;
        spec.program = "sh";
        spec.argv = {"sh", "-c", "printf '%s|%s|' \"$MCX\" \"$PWD\"; stty size; exit 7"};
        spec.env = {"PATH=/usr/bin:/bin", "MCX=given"};
        spec.cwd = dir;
        spec.cols = 91;
        spec.rows = 17;
        std::string error;
        std::unique_ptr<Shared::Pty> p = Shared::Pty::spawn(spec, &error);
        CHECK(p && error.empty());
        if (p) {
            std::string out;
            const double end = nowMs() + 5000;
            while (nowMs() < end && p->read(out)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            char real[PATH_MAX];
            const std::string resolved = realpath(dir.c_str(), real) ? real : dir;
            CHECK(out.find("given|") != std::string::npos);
            CHECK(out.find(resolved + "|") != std::string::npos || out.find(dir + "|") != std::string::npos);
            CHECK(out.find("17 91") != std::string::npos);
            int status = 0;
            const double end2 = nowMs() + 5000;
            while (!p->reap(&status) && nowMs() < end2) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            CHECK(p->reaped() && WIFEXITED(status) && WEXITSTATUS(status) == 7);
            CHECK(gone(p->pid()));
        }
        // A program that is not there exits 127, as a shell's would.
        spec.program = "/nonexistent/mc-shell";
        spec.argv.clear();
        p = Shared::Pty::spawn(spec, &error);
        CHECK(p != nullptr);
        if (p) {
            int status = 0;
            const double end = nowMs() + 5000;
            std::string sink;
            while (p->read(sink) && nowMs() < end) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            while (!p->reap(&status) && nowMs() < end) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 127);
        }
    }

    GROUP("term:host");
    {
        // A shell in the host: what it prints reaches the renderer, which
        // acknowledges it; what is typed reaches the shell; the host's
        // screen holds what the renderer's does.
        TermRig r;
        r.open(1, r.spec("/bin/sh", {"sh"}));
        CHECK(r.wait([&] { return r.pid > 0; }));
        r.type(1, "echo hi-$((40+2))\n");
        CHECK(r.wait([&] { return r.has("hi-42"); }));
        bool same = false;
        CHECK(r.wait([&] {
            r.host->inspect([&](const Shared::Host& h) {
                const TerminalScreen* s = h.screen(1);
                same = s && s->text() == r.mirror.text() && r.cursorLine == s->historyBase() + s->cursorRow();
            });
            return same;
        }));
        // Resizing: the shell's programs see the new size, and so does the
        // host's screen.
        r.client->resizeTerminal(1, 101, 31);
        r.type(1, "stty size\n");
        CHECK(r.wait([&] { return r.has("31 101"); }));
        int cols = 0;
        r.host->inspect([&](const Shared::Host& h) { cols = h.screen(1) ? h.screen(1)->cols() : 0; });
        CHECK(cols == 101);
        // exit: the shell's status comes back, after its last output.
        r.type(1, "echo last; exit 5\n");
        CHECK(r.wait([&] { return r.exited; }));
        CHECK(r.has("last"));
        CHECK(WIFEXITED(r.status) && WEXITSTATUS(r.status) == 5);
        CHECK(r.settled());
        // A shell that cannot start says why; one with a folder it cannot
        // enter starts in /.
        TermRig r2;
        Shared::TermSpec s = r2.spec("/bin/sh", {"sh", "-c", "pwd; exit 0"});
        s.cwd = "/nonexistent/mc-folder";
        r2.open(2, s);
        CHECK(r2.wait([&] { return r2.exited; }));
        CHECK(r2.out.find("/\r\n") != std::string::npos);

        // The tty's modes, as the Mac's checks read them (TerminalView): a
        // password read (echo off, canonical), a line editor's raw mode that
        // keeps signals (ZLE: zsh's widths), and a raw program's with
        // signals off (fzf: clusters). A script that waits on `read` between
        // them, so each holds until the check has seen it (an interactive
        // shell's own line editor sets modes of its own at its prompt).
        TermRig r3;
        r3.open(4, r3.spec("/bin/sh", {"sh", "-c",
            "stty -echo; echo modes-1; read x; stty echo -icanon; echo modes-2; read x;"
            " stty -isig; echo modes-3; read x; stty sane; echo modes-4; read x"}));
        CHECK(r3.wait([&] { return r3.has("modes-1") && r3.shell.echoOff; }));
        CHECK(!r3.shell.raw && !r3.shell.signalsOff);
        r3.type(4, "\n");
        CHECK(r3.wait([&] { return r3.has("modes-2") && r3.shell.raw; }));
        CHECK(!r3.shell.echoOff && !r3.shell.signalsOff);
        r3.type(4, "\n");
        CHECK(r3.wait([&] { return r3.has("modes-3") && r3.shell.signalsOff; }));
        r3.type(4, "\n");
        CHECK(r3.wait([&] { return r3.has("modes-4") && !r3.shell.raw && !r3.shell.signalsOff; }));
        r3.type(4, "\n");
        CHECK(r3.wait([&] { return r3.exited; }));
    }

    GROUP("term:marks");
    {
        // The shell integration's marks, with the nonce and without, read
        // by the host from the pty's bytes: the shell's state and where the
        // commands are, through the protocol's encoding both ways.
        TermRig r(true);
        Shared::TermSpec s = r.spec("/bin/sh", {"sh", "-c",
            "printf '\\033]133;A;mc=N1\\007$ '; printf '\\033]633;E;ls -l;mc=N1\\007\\033]133;C;mc=N1\\007';"
            " echo listing; printf '\\033]133;D;3;mc=N1\\007'; printf '\\033]133;A;mc=N1\\007$ ';"
            " printf '\\033]133;A\\007\\033]133;A;mc=other\\007'; exec cat"});
        s.nonce = "N1";
        r.open(3, s);
        CHECK(r.wait([&] { return r.shell.prompts == 2 && r.marks.size() == 2; }));
        CHECK(r.shell.known && r.shell.atPrompt && !r.shell.busy);
        CHECK(r.shell.started == 1 && r.shell.done == 1 && r.shell.status == 3);
        if (r.marks.size() == 2) {
            CHECK(r.marks[0].output >= r.marks[0].prompt && r.marks[0].end > r.marks[0].output);
            CHECK(r.marks[0].status == 3);
            CHECK(r.marks[1].output == -1 && r.marks[1].prompt == r.marks[0].end);
        }
        // A command running: its text, then its end.
        r.type(3, "\033]633;E;sleep 9;mc=N1\007\033]133;C;mc=N1\007\n");
        CHECK(r.wait([&] { return r.shell.busy; }));
        CHECK(r.shell.command == "sleep 9" && !r.shell.atPrompt && r.shell.started == 2);
        r.type(3, "\033]133;D;0;mc=N1\007\033]133;A;mc=N1\007\n");
        CHECK(r.wait([&] { return !r.shell.busy && r.shell.prompts == 3; }));
        CHECK(r.shell.done == 2 && r.shell.status == 0 && r.marks.size() == 3);
        // The foreground group is the shell's (cat, exec'd) and the modes are
        // cooked.
        CHECK(r.wait([&] { return r.shell.foreground == r.pid; }));
        CHECK(!r.shell.raw);

        // A renderer attaching (stage 6's way back in): the replay rebuilds
        // the host's screen, marks included, and live output follows.
        TerminalScreen host(80, 24);
        r.host->inspect([&](const Shared::Host& h) {
            if (h.screen(3)) host = *h.screen(3);
        });
        r.mirror = TerminalScreen(80, 24);
        r.mirror.setScrollbackLimit(1000);
        r.mirror.setMarkNonce("N1");
        r.marks.clear();
        r.client->attachTerminal(3, &r);
        CHECK(r.wait([&] { return r.replays == 1 && r.marks.size() == 3; }));
        const std::string d = screenDiff(host, r.mirror);
        if (!d.empty()) std::printf("  attached: %s\n", d.c_str());
        CHECK(d.empty());
        r.type(3, "after attach\n");
        CHECK(r.wait([&] { return r.mirror.text().find("after attach") != std::string::npos; }));
        r.client->closeTerminal(3);
        CHECK(r.settled());
    }

    GROUP("term:teardown");
    {
        // The Mac's terminal_pty cases. Closing a shell: its pty's fd is
        // closed, nothing of it is polled, and it is reaped.
        TermRig r;
        r.open(4, r.spec("/bin/sh", {"sh"}));
        CHECK(r.wait([&] { return r.pid > 0 && r.has("$ "); }));
        int fd = r.fd(4);
        const int pid = r.pid;
        CHECK(fd >= 0);
        r.client->closeTerminal(4);
        CHECK(r.settled(5000));
        CHECK(fdClosed(fd));
        CHECK(gone(pid));
        CHECK(!r.exited);   // a terminal its renderer closed tells it nothing

        // A shell ignoring SIGHUP: killed 2 s after the hang-up.
        r.pid = 0;
        r.out.clear();
        r.open(5, r.spec("/bin/sh", {"sh", "-c", "trap '' HUP; echo ready; while :; do sleep 1; done"}));
        CHECK(r.wait([&] { return r.pid > 0 && r.has("ready"); }));
        const int stubborn = r.pid;
        double t0 = nowMs();
        r.client->closeTerminal(5);
        CHECK(r.settled(6000));
        CHECK(gone(stubborn));
        CHECK(nowMs() - t0 >= 1500);   // it waited for the hang-up first

        // A megabyte into a program that does not read: the host's thread
        // never blocks on it (a round trip still answers at once), and
        // closing it drops the rest.
        r.pid = 0;
        r.out.clear();
        r.open(6, r.spec("/bin/sh", {"sh", "-c", "echo ready; exec sleep 60"}));
        CHECK(r.wait([&] { return r.pid > 0 && r.has("ready"); }));
        const int sleeper = r.pid;
        r.type(6, std::string(1 << 20, 'x'));
        t0 = nowMs();
        CHECK(r.client->settle(5000));
        CHECK_TIME(nowMs() - t0, 1000);
        r.client->closeTerminal(6);
        CHECK(r.settled(5000));
        CHECK(gone(sleeper));

        // Closing during `yes`.
        r.pid = 0;
        r.received = 0;
        r.out.clear();
        r.open(7, r.spec("yes", {"yes", "minicode"}));
        CHECK(r.wait([&] { return r.received > 200000; }));
        const int yes = r.pid;
        fd = r.fd(7);
        r.client->closeTerminal(7);
        CHECK(r.settled(5000));
        CHECK(gone(yes));
        CHECK(fdClosed(fd));
        t0 = nowMs();
        CHECK(r.client->settle(5000));
        CHECK_TIME(nowMs() - t0, 1000);
    }

    GROUP("term:credit");
    {
        // Output nobody takes stops the host reading the pty (the program
        // blocks) at kTermCredit; taking it lets it go on.
        TermRig r;
        r.ack = false;
        r.open(8, r.spec("yes", {"yes", "credit"}));
        CHECK(r.wait([&] { return r.received >= Shared::kTermCredit; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        r.client->dispatch();
        const long long held = r.received;
        CHECK(held <= Shared::kTermCredit + 65536);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        r.client->dispatch();
        CHECK(r.received == held);
        r.ack = true;
        r.client->ackTerminal(8, (size_t)held);
        CHECK(r.wait([&] { return r.received > held + 100000; }));
        r.client->closeTerminal(8);
        CHECK(r.settled(5000));
    }

    GROUP("term:protocol");
    {
        // Terminal messages through the encoding: any bytes, the shell's
        // state, the marks and the spec.
        Message m;
        m.kind = Shared::Kind::TermInput;
        m.term = 12;
        for (int i = 0; i < 256; i++) m.bytes += (char)i;
        m.bytes += "\x1b[1;2H\xff\xfe";
        Message back = Shared::roundTrip(m);
        CHECK(back.kind == Shared::Kind::TermInput && back.term == 12 && back.bytes == m.bytes);
        for (size_t n = 0; n < 5; n++) {
            Message k;
            k.kind = Shared::Kind::TermOutput;
            k.bytes = m.bytes.substr(0, n);
            k.v = 77;
            k.flag = 1;
            Message kb = Shared::roundTrip(k);
            CHECK(kb.bytes == k.bytes && kb.v == 77 && kb.flag == 1);
        }
        m.kind = Shared::Kind::TermResize;
        m.bytes.clear();
        m.cols = 120;
        m.rows = 40;
        back = Shared::roundTrip(m);
        CHECK(back.cols == 120 && back.rows == 40);
        Shared::TermShell s;
        s.known = s.busy = true;
        s.started = 9;
        s.done = 8;
        s.status = 2;
        s.command = "make -j8";
        s.cwd = "/tmp/a b";
        s.foreground = 4321;
        s.signalsOff = true;
        CHECK(Shared::termShellFrom(Shared::termShellJson(s)) == s);
        CHECK(Shared::termShellFrom(Json()).status == -1);
        std::vector<Shared::TermMark> marks(2);
        marks[0].prompt = 5;
        marks[0].output = 6;
        marks[0].end = 9;
        marks[0].status = 1;
        marks[1].prompt = 9;
        const std::vector<Shared::TermMark> mb = Shared::termMarksFrom(Shared::termMarksJson(marks));
        CHECK(mb.size() == 2 && mb[0].end == 9 && mb[0].status == 1 && mb[1].output == -1);
        Shared::TermSpec spec;
        spec.program = "/bin/zsh";
        spec.argv = {"-zsh", "-i"};
        spec.env = {"A=1", "B=two words"};
        spec.cols = 99;
        spec.nonce = "abc";
        const Shared::TermSpec sb = Shared::termSpecFrom(Shared::termSpecJson(spec));
        CHECK(sb.program == spec.program && sb.argv == spec.argv && sb.env == spec.env && sb.cols == 99 &&
              sb.rows == 24 && sb.nonce == "abc");
    }
}
