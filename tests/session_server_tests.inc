// session_server_tests.inc — the `hostserver:*` groups of run_tests.cpp: a
// headless session host (SessionServer.h, SESSION-DESIGN.md stage 6)
// started in this process on a socket in a scratch folder, and renderers
// that attach to it over the socket, each a Client with a HostLink: one
// host per folder, two renderers editing at once and converging, one
// detaching and attaching again to the same state, terminals running on
// across a detach and replayed on attach, `attach --dump`'s text, and the
// lifetime rules with a short timer. Real sockets and shells, under TSan
// in CI. Included in run_tests.cpp after session_terminal_tests.inc, whose
// helpers (TestMirror, scratchFolder, nowMs, gone) it uses.

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "SessionServer.h"

namespace {

// A host serving a scratch folder, run() on a thread of its own.
struct ServerRig {
    std::string dir = scratchFolder();   // the socket folder: short, for sun_path
    std::string proj;
    std::unique_ptr<Shared::HostServer> server;
    std::thread thread;
    std::atomic<int> result{-1};   // run()'s answer: 1 the lifetime ended, 0 stopped
    double started = 0, ended = 0;
    bool watch = false;   // HostOptions::watchDisk

    ServerRig() {
        proj = Shared::resolveFolder(scratchFolder());
        dir += "/sockets";
    }
    ~ServerRig() { stop(); }
    Shared::HostServer::Start start(long long idleMs, std::string* error = nullptr) {
        server = std::make_unique<Shared::HostServer>();
        Shared::HostOptions o;
        o.folder = proj;
        o.socketDir = dir;
        o.idleMs = idleMs;
        o.watchDisk = watch;
        std::string why;
        const Shared::HostServer::Start s = server->start(o, &why);
        if (error) *error = why;
        if (s != Shared::HostServer::Start::Ok) return s;
        started = nowMs();
        thread = std::thread([this] {
            const bool lifetime = server->run();
            ended = nowMs();
            result = lifetime ? 1 : 0;   // after `ended`, which a check reads once it sees this
        });
        return s;
    }
    void stop() {
        if (thread.joinable()) {
            server->stop();
            thread.join();
        }
        server.reset();
    }
    std::string path() const { return dir + "/" + Shared::hostSocketName(proj); }
    // The host's terminal screen's text, "" when it has no such terminal.
    std::string screenText(Shared::TermId t) {
        std::string s;
        server->host().inspect([&](const Shared::Host& h) {
            if (const TerminalScreen* sc = h.screen(t)) s = sc->text();
        });
        return s;
    }
    std::string busy() {
        std::string s;
        server->host().inspect([&](const Shared::Host& h) { s = h.busyReason(); });
        return s;
    }
};

// A renderer over the host's socket: a client, one document's mirror and
// terminals fed into a screen of its own (its emulator).
struct SocketRenderer : Shared::Client::TermListener {
    std::unique_ptr<Shared::HostLink> link;
    std::unique_ptr<Shared::Client> client;
    TestMirror mirror;
    TerminalScreen screen{80, 24};
    std::string out;   // every byte fed, as it came
    int replays = 0;
    int pid = 0;

    bool attach(const std::string& path, const char* name) {
        std::string error;
        link = Shared::HostLink::open(path, &error);
        if (!link) {
            std::printf("  attach: %s\n", error.c_str());
            return false;
        }
        client = std::make_unique<Shared::Client>(Model::Unit::Utf16, name);
        link->connect(*client);
        mirror = TestMirror();
        mirror.client = client.get();
        return true;
    }
    void detach() {
        if (link) link->close();   // the reader first: it delivers to the client
        client.reset();
        link.reset();
    }
    ~SocketRenderer() { detach(); }
    void openDoc(Shared::DocId d, const std::u16string& text) {
        mirror.doc = d;
        mirror.text = text;
        client->open(d, "/notes.txt", Model::utf8FromUtf16(text), &mirror);
    }
    void termOpened(Shared::TermId, int p) override { pid = p; }
    void termOutput(Shared::TermId t, const std::string& b, long long, bool, bool replay) override {
        screen.feed(b);
        out += b;
        if (replay) replays++;
        else client->ackTerminal(t, b.size());
    }
    bool wait(const std::function<bool()>& done, int ms = 8000) {
        const double end = nowMs() + ms;
        for (;;) {
            client->dispatch();
            if (done()) return true;
            if (nowMs() > end) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

bool waitUntil(const std::function<bool()>& done, int ms = 8000) {
    const double end = nowMs() + ms;
    while (!done()) {
        if (nowMs() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

Shared::TermSpec shellSpec(const std::string& cwd, std::vector<std::string> argv) {
    Shared::TermSpec s;
    s.program = "/bin/sh";
    s.argv = std::move(argv);
    s.env = {"PATH=/usr/bin:/bin", "HOME=" + cwd, "TERM=xterm-256color", "PS1=$ ", "LANG=C", "ENV="};
    s.cwd = cwd;
    s.scrollback = 1000;
    return s;
}

// A renderer over two pipes, as a window over SSH is to `mutible host
// --stdio` (stage 8): what it sends is written to the host's input, and a
// reader thread of its own delivers what the host writes, then hears the
// host is gone when that ends.
struct PipeRenderer : Shared::Client::TermListener {
    int toHost = -1, fromHost = -1;
    std::thread reader;
    std::unique_ptr<Shared::Client> client;
    TestMirror mirror;
    TerminalScreen screen{80, 24};
    std::atomic<int> pid{0};

    // `before`: what the client says in its Hello beyond an app's (hearDisk).
    void connect(int write, int read, const std::function<void(Shared::Client&)>& before = nullptr) {
        toHost = write;
        fromHost = read;
        client = std::make_unique<Shared::Client>(Model::Unit::Utf16, "mac");
        client->setKeepsBackups(true);
        client->setSavesThroughHost(true);
        if (before) before(*client);
        mirror.client = client.get();
        reader = std::thread([this] {
            std::string in;
            char buf[65536];
            for (;;) {
                const ssize_t n = ::read(fromHost, buf, sizeof buf);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                in.append(buf, (std::size_t)n);
                std::size_t nl;
                while ((nl = in.find('\n')) != std::string::npos) {
                    Shared::Message m;
                    if (Shared::decode(in.substr(0, nl), &m)) client->deliver(std::move(m));
                    in.erase(0, nl + 1);
                }
            }
            client->hostLost();
        });
        client->connect([this](Shared::Message&& m) {
            if (toHost < 0) return;
            const std::string line = Shared::encode(m) + "\n";
            std::size_t at = 0;
            while (at < line.size()) {
                const ssize_t n = ::write(toHost, line.data() + at, line.size() - at);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) return;
                at += (std::size_t)n;
            }
        });
    }
    void closeInput() {
        if (toHost >= 0) ::close(toHost);
        toHost = -1;
    }
    ~PipeRenderer() {
        closeInput();
        if (reader.joinable()) reader.join();
        if (fromHost >= 0) ::close(fromHost);
    }
    void termOpened(Shared::TermId, int p) override { pid = p; }
    void termOutput(Shared::TermId t, const std::string& b, long long, bool, bool) override {
        screen.feed(b);
        client->ackTerminal(t, b.size());
    }
    bool wait(const std::function<bool()>& done, int ms = 8000) {
        const double end = nowMs() + ms;
        for (;;) {
            client->dispatch();
            if (done()) return true;
            if (nowMs() > end) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

}  // namespace

static void testSessionServer() {
    GROUP("hostserver:one-per-folder");
    {
        ServerRig rig;
        std::string error;
        CHECK(rig.start(60000, &error) == Shared::HostServer::Start::Ok);
        CHECK(rig.server->socketPath() == rig.path());
        struct stat st;
        CHECK(lstat(rig.dir.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
        CHECK(lstat(rig.path().c_str(), &st) == 0 && S_ISSOCK(st.st_mode));
        CHECK(Shared::findHostSocket(rig.proj, {rig.dir}) == rig.path());
        // A second host for the folder finds the first and names its socket.
        Shared::HostServer second;
        Shared::HostOptions o;
        o.folder = rig.proj;
        o.socketDir = rig.dir;
        CHECK(second.start(o, &error) == Shared::HostServer::Start::Running);
        CHECK(error == rig.path());
        // Another folder gets a host of its own beside it.
        Shared::HostServer other;
        o.folder = Shared::resolveFolder(scratchFolder());
        CHECK(other.start(o, &error) == Shared::HostServer::Start::Ok);
        CHECK(other.socketPath() != rig.path());
        // The host says which folder it serves.
        SocketRenderer r;
        CHECK(r.attach(rig.path(), "one"));
        if (r.client) CHECK(r.client->sessionState()["folder"].asString() == rig.proj);
        r.detach();
        rig.stop();
        CHECK(lstat(rig.path().c_str(), &st) != 0);   // the socket goes with the host

        // A socket a dead host left (nothing listens) is replaced.
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", rig.path().c_str());
        CHECK(bind(fd, (sockaddr*)&addr, sizeof addr) == 0);
        close(fd);
        bool refused = false;
        CHECK(!Shared::HostLink::open(rig.path(), &error, &refused) && refused);
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        CHECK(r.attach(rig.path(), "again"));
        r.detach();
        rig.stop();

        // A socket folder others can enter is never used.
        chmod(rig.dir.c_str(), 0755);
        CHECK(!Shared::trustedSocketFolder(rig.dir));
        CHECK(rig.start(60000, &error) == Shared::HostServer::Start::Failed);
        CHECK(Shared::findHostSocket(rig.proj, {rig.dir}).empty());
        chmod(rig.dir.c_str(), 0700);
        CHECK(Shared::resolveFolder(rig.proj + "/missing").empty());
        CHECK(Shared::hostSocketName(rig.proj) == Shared::hostSocketName(rig.proj + "/x/..") ||
              Shared::resolveFolder(rig.proj + "/x/..").empty());
        // A host's socket shares the control channel's folder, so the
        // control command line must never take it for a mutible's.
        CHECK(ControlChannel::pidFromSocketName(Shared::hostSocketName(rig.proj)) == 0);
        CHECK(ControlChannel::pidFromSocketName(Shared::hostSocketName("/")) == 0);
    }

    GROUP("hostserver:attach-edit-detach");
    {
        ServerRig rig;
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        SocketRenderer a, b;
        CHECK(a.attach(rig.path(), "a"));
        CHECK(b.attach(rig.path(), "b"));
        CHECK(waitUntil([&] { return rig.server->renderers() == 2; }));
        if (a.client && b.client) {
            const std::u16string start = u"hello\nworld\n";
            a.openDoc(1, start);
            CHECK(a.client->settle());
            // The second renderer finds the document in the session and
            // opens it from the host's text.
            const Json s = b.client->sessionState();
            CHECK(s["documents"].size() == 1 && s["documents"][0]["path"].asString() == "/notes.txt");
            b.mirror.doc = 1;
            b.client->open(1, "/notes.txt", "", &b.mirror);
            CHECK(b.client->settle());
            CHECK(b.mirror.text == start);
            // Both type at once, neither waiting for the other.
            a.mirror.caret = 0;
            b.mirror.caret = b.mirror.text.size();
            for (int i = 0; i < 40; i++) {
                a.mirror.type(u"a");
                b.mirror.type(i % 7 == 0 ? u"\n" : u"b");
                if (i % 5 == 0) a.client->dispatch();
                if (i % 3 == 0) b.client->dispatch();
            }
            for (int i = 0; i < 3; i++) {
                CHECK(a.client->settle());
                CHECK(b.client->settle());
            }
            const std::u16string host = a.client->hostDocument(1).text;
            CHECK(a.mirror.text == b.mirror.text && a.mirror.text == host);
            CHECK(host.size() == start.size() + 80);
            CHECK(a.client->hostDocument(1).dirty);

            // One detaches; the other edits on; it attaches again to the
            // same state, and the host kept its undo history.
            a.detach();
            CHECK(waitUntil([&] { return rig.server->renderers() == 1; }));
            b.client->breakCoalescing(1);
            b.mirror.type(u"!");
            CHECK(b.client->settle());
            CHECK(a.attach(rig.path(), "a again"));
            a.mirror.doc = 1;
            a.client->open(1, "/notes.txt", "", &a.mirror);
            CHECK(a.client->settle());
            CHECK(a.mirror.text == b.mirror.text);
            CHECK(a.client->undoInfo(1).canUndo);
            CHECK(a.client->undo(1, false) == Shared::Outcome::Done);   // b's "!"
            CHECK(b.client->settle());
            CHECK(a.mirror.text == b.mirror.text && a.mirror.text == host);

            // The text dump, from a third renderer.
            SocketRenderer c;
            CHECK(c.attach(rig.path(), "dump"));
            const std::string dump = Shared::dumpSession(*c.client);
            CHECK(dump.find("== host " + rig.proj + "\n") == 0);
            CHECK(dump.find("== document 1 /notes.txt (version ") != std::string::npos);
            CHECK(dump.find(", unsaved)\n" + Model::utf8FromUtf16(host)) != std::string::npos);
        }
    }

    GROUP("hostserver:id-bases");
    {
        // Two renderers over the socket take their ids from the bases their
        // Welcomes gave: their documents and terminals never clash, and a
        // document the host holds keeps its id when the other edits it.
        ServerRig rig;
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        SocketRenderer a, b;
        CHECK(a.attach(rig.path(), "a"));
        CHECK(b.attach(rig.path(), "b"));
        const std::string home = scratchFolder();
        if (a.client && b.client) {
            CHECK(a.client->settle() && b.client->settle());
            const std::uint64_t ba = a.client->idBase(), bb = b.client->idBase();
            CHECK(ba >= Shared::kIdRange && bb >= Shared::kIdRange && ba != bb);
            CHECK(ba % Shared::kIdRange == 0 && bb % Shared::kIdRange == 0);
            const Shared::DocId da = a.client->newId(), db = b.client->newId();
            const Shared::TermId ta = a.client->newId(), tb = b.client->newId();
            CHECK(da == ba + 1 && db == bb + 1 && ta == ba + 2 && tb == bb + 2);
            a.openDoc(da, u"from a\n");
            b.openDoc(db, u"from b\n");
            a.client->openTerminal(ta, shellSpec(home, {"sh"}), &a);
            b.client->openTerminal(tb, shellSpec(home, {"sh"}), &b);
            CHECK(a.wait([&] { return a.pid > 0; }) && b.wait([&] { return b.pid > 0; }));
            CHECK(a.pid != b.pid);
            const Json s = a.client->sessionState();
            std::set<std::uint64_t> docs, terms;
            for (const Json& d : s["documents"].elements()) docs.insert((std::uint64_t)d["doc"].asInt());
            for (const Json& t : s["terminals"].elements()) terms.insert((std::uint64_t)t["term"].asInt());
            CHECK(docs == (std::set<std::uint64_t>{da, db}) && terms == (std::set<std::uint64_t>{ta, tb}));
            // Each terminal's input reaches its own shell.
            a.client->terminalInput(ta, "echo mine-$((1+1))\n");
            b.client->terminalInput(tb, "echo theirs-$((2+2))\n");
            CHECK(a.wait([&] { return a.screen.text().find("mine-2") != std::string::npos; }));
            CHECK(b.wait([&] { return b.screen.text().find("theirs-4") != std::string::npos; }));
            CHECK(a.screen.text().find("theirs-4") == std::string::npos);
            // b edits a's document under its id; a's mirror follows.
            TestMirror other;
            other.client = b.client.get();
            other.doc = da;
            b.client->open(da, "/notes.txt", "", &other);
            CHECK(b.client->settle());
            CHECK(other.text == u"from a\n");
            other.caret = 0;
            other.type(u">");
            CHECK(b.client->settle());
            CHECK(a.wait([&] { return a.mirror.text == u">from a\n"; }));
            CHECK(a.client->hostDocument(da).text == u">from a\n");
            CHECK(a.client->sessionState()["documents"].size() == 2);
            b.client->close(da);   // before `other` goes
            a.client->closeTerminal(ta);
            b.client->closeTerminal(tb);
            CHECK(waitUntil([&] { return gone(a.pid) && gone(b.pid); }, 5000));
        }
    }

    GROUP("hostserver:host-gone");
    {
        // The host ending under a renderer (killed, or its lifetime): the
        // link closes and the client hears it (HostLink::connect), so its
        // waits return at once instead of each waiting out its timeout,
        // nothing more is sent, and ids already given keep coming.
        ServerRig rig;
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        SocketRenderer a;
        CHECK(a.attach(rig.path(), "a"));
        if (a.client) {
            CHECK(a.client->settle());
            const Shared::DocId d = a.client->newId();
            a.openDoc(d, u"kept\n");
            a.mirror.caret = 4;
            a.mirror.type(u"!");
            CHECK(a.client->settle() && a.client->hostDocument(d).text == u"kept!\n");
            CHECK(!a.client->lost());
            rig.stop();   // the host ends: its connections close
            CHECK(waitUntil([&] { return a.client->lost(); }, 5000));
            const double t0 = nowMs();
            CHECK(!a.client->settle(10000));
            CHECK(a.client->undo(d, false) == Shared::Outcome::Nothing);
            CHECK(!a.client->undoInfo(d).canUndo);
            CHECK(a.client->sessionState().isNull());
            a.mirror.type(u"?");   // recorded, and dropped
            CHECK(!a.client->settle(10000));
            CHECK(nowMs() - t0 < 2000);
            CHECK(a.client->newId() == d + 1);
            a.client->dispatch();   // what the wake queued: nothing to do
        }
    }

    GROUP("hostserver:terminals-across-detach");
    {
        ServerRig rig;
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        SocketRenderer a;
        CHECK(a.attach(rig.path(), "a"));
        const std::string home = scratchFolder();
        if (a.client) {
            a.client->openTerminal(7, shellSpec(home, {"sh"}), &a);
            CHECK(a.wait([&] { return a.pid > 0 && a.out.find("$ ") != std::string::npos; }));
            a.client->terminalInput(7, "echo before-$((1+1))\n");
            CHECK(a.wait([&] { return a.screen.text().find("before-2") != std::string::npos; }));
            // Output that comes while nobody is attached goes into the
            // host's screen; the shell runs on.
            a.client->terminalInput(7, "sleep 0.3; echo while-$((2+2))\n");
            a.client->settle();
            a.detach();
            CHECK(waitUntil([&] { return rig.server->renderers() == 0; }));
            CHECK(waitUntil([&] { return rig.screenText(7).find("while-4") != std::string::npos; }));
            CHECK(!gone(a.pid));

            // A renderer attaching gets the screen replayed, then live output.
            SocketRenderer b;
            CHECK(b.attach(rig.path(), "b"));
            const Json s = b.client->sessionState();
            CHECK(s["terminals"].size() == 1 && s["terminals"][0]["term"].asNumber() == 7);
            CHECK(s["terminals"][0]["pid"].asNumber() == a.pid);
            b.client->attachTerminal(7, &b);
            CHECK(b.wait([&] { return b.replays == 1; }));
            CHECK(b.screen.text().find("before-2") != std::string::npos);
            CHECK(b.screen.text().find("while-4") != std::string::npos);
            b.client->terminalInput(7, "echo after-$((3+3))\n");
            CHECK(b.wait([&] { return b.screen.text().find("after-6") != std::string::npos; }));

            // Two renderers show it at once: both hear its output.
            SocketRenderer c;
            CHECK(c.attach(rig.path(), "c"));
            c.client->attachTerminal(7, &c);
            CHECK(c.wait([&] { return c.replays == 1; }));
            c.client->terminalInput(7, "echo both-$((4+4))\n");
            CHECK(c.wait([&] { return c.screen.text().find("both-8") != std::string::npos; }));
            CHECK(b.wait([&] { return b.screen.text().find("both-8") != std::string::npos; }));

            // The dump draws the screen from the replay.
            SocketRenderer d;
            CHECK(d.attach(rig.path(), "dump"));
            const std::string dump = Shared::dumpSession(*d.client);
            CHECK(dump.find("== terminal 7 (80x24, shell " + std::to_string(a.pid)) != std::string::npos);
            CHECK(dump.find("while-4\n") != std::string::npos && dump.find("both-8\n") != std::string::npos);
            d.detach();

            b.client->closeTerminal(7);
            CHECK(c.wait([&] { return c.client->sessionState()["terminals"].size() == 0; }));
            CHECK(waitUntil([&] { return gone(a.pid); }, 5000));
        }
    }

    GROUP("hostserver:lifetime");
    {
        // No renderer from the start: the host ends once the time is up.
        {
            ServerRig rig;
            CHECK(rig.start(150) == Shared::HostServer::Start::Ok);
            CHECK(waitUntil([&] { return rig.result == 1; }, 5000));
            CHECK(rig.ended - rig.started >= 140);
        }
        // Unsaved changes keep it; saving lets it go.
        {
            ServerRig rig;
            CHECK(rig.start(150) == Shared::HostServer::Start::Ok);
            SocketRenderer r;
            CHECK(r.attach(rig.path(), "r"));
            if (r.client) {
                r.openDoc(3, u"text");
                r.mirror.caret = 4;
                r.mirror.type(u"+");
                CHECK(r.client->settle());
                r.detach();
                CHECK(rig.busy().find("/notes.txt has unsaved changes") == 0);
                // Time is the condition: three times the idle time without
                // a renderer, and the host is still there.
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                CHECK(rig.result == -1);
                CHECK(r.attach(rig.path(), "r again"));
                r.mirror.doc = 3;
                r.client->open(3, "/notes.txt", "", &r.mirror);
                CHECK(r.client->settle());
                CHECK(r.mirror.text == u"text+");
                // A renderer attached keeps it whatever the time.
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                CHECK(rig.result == -1);
                r.client->saved(3);
                CHECK(r.client->settle());
                const double left = nowMs();
                r.detach();
                CHECK(waitUntil([&] { return rig.result == 1; }, 5000));
                CHECK(rig.ended - left >= 140);
            }
        }
        // A command running keeps it (the integration's marks say so), and
        // so does a program in the foreground; an idle shell does not.
        {
            ServerRig rig;
            CHECK(rig.start(150) == Shared::HostServer::Start::Ok);
            SocketRenderer r;
            CHECK(r.attach(rig.path(), "r"));
            const std::string home = scratchFolder();
            if (r.client) {
                Shared::TermSpec spec = shellSpec(home, {"sh", "-c", "exec cat"});
                spec.nonce = "N9";
                r.client->openTerminal(2, spec, &r);
                CHECK(r.wait([&] { return r.pid > 0; }));
                r.client->terminalInput(2, "\033]633;E;sleep 9;mc=N9\007\033]133;C;mc=N9\007\n");
                CHECK(waitUntil([&] { return rig.busy() == "terminal 2 is running sleep 9"; }));
                r.detach();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));   // as above
                CHECK(rig.result == -1);
                CHECK(r.attach(rig.path(), "r again"));
                r.client->attachTerminal(2, &r);
                r.client->terminalInput(2, "\033]133;D;0;mc=N9\007\n");
                CHECK(waitUntil([&] { return rig.busy().empty(); }));
                r.detach();
                CHECK(waitUntil([&] { return rig.result == 1; }, 5000));
                CHECK(!gone(r.pid));
                rig.stop();   // the host ends: its shells with it
                CHECK(waitUntil([&] { return gone(r.pid); }, 5000));
            }
        }
        {
            ServerRig rig;
            CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
            SocketRenderer r;
            CHECK(r.attach(rig.path(), "r"));
            const std::string home = scratchFolder();
            if (r.client) {
                r.client->openTerminal(4, shellSpec(home, {"sh", "-i"}), &r);
                CHECK(r.wait([&] { return r.pid > 0 && r.out.find("$ ") != std::string::npos; }));
                CHECK(rig.busy().empty());
                r.client->terminalInput(4, "sleep 30\n");
                CHECK(waitUntil([&] { return rig.busy() == "terminal 4 is running a program"; }));
                r.client->terminalInput(4, "\003");
                CHECK(waitUntil([&] { return rig.busy().empty(); }));
            }
        }
    }

    GROUP("hostserver:command-line");
    {
        using namespace ControlChannel;
        const std::string cwd = "/work", home = "/home/me";
        auto exists = [](const std::string&) { return true; };
        CliCommand c = parseCommandLine({"host", "--folder", "proj", "--idle-minutes", "0.5"}, cwd, home, exists);
        CHECK(c.error.empty() && c.cmd == "host");
        CHECK(c.args["folder"].asString() == "/work/proj" && c.args["idle_minutes"].asNumber() == 0.5);
        CHECK(parseCommandLine({"host"}, cwd, home, exists).error == "host needs --folder or --stdio");
        // Over stdio (stage 8): a folder or a file, ~ this machine's home.
        c = parseCommandLine({"host", "--stdio", "~/proj"}, cwd, home, exists);
        CHECK(c.error.empty() && c.cmd == "host" && c.args["stdio"].asString() == "/home/me/proj");
        CHECK(parseCommandLine({"host", "--stdio", "~"}, cwd, home, exists).args["stdio"].asString() == "/home/me");
        CHECK(parseCommandLine({"host", "--stdio", "/srv/a.c"}, cwd, home, exists).args["stdio"].asString() == "/srv/a.c");
        CHECK(!parseCommandLine({"host", "--stdio", "p", "--folder", "q"}, cwd, home, exists).error.empty());
        CHECK(!parseCommandLine({"host", "--stdio", "p", "--web", "8080"}, cwd, home, exists).error.empty());
        CHECK(!parseCommandLine({"host", "--stdio"}, cwd, home, exists).error.empty());
        CHECK(!parseCommandLine({"attach", "--stdio", "p"}, cwd, home, exists).error.empty());
        CHECK(usageText("host").find("--stdio") != std::string::npos);
        CHECK(!parseCommandLine({"host", "--folder", "x", "--idle-minutes", "soon"}, cwd, home, exists)
                   .error.empty());
        CHECK(!parseCommandLine({"host", "--folder", "x", "--dump"}, cwd, home, exists).error.empty());
        c = parseCommandLine({"attach", "--folder", "~/p", "--dump"}, cwd, home, exists);
        CHECK(c.error.empty() && c.cmd == "attach" && c.args["dump"].asBool());
        CHECK(c.args["folder"].asString() == "/home/me/p");
        CHECK(!parseCommandLine({"attach", "--folder", "p"}, cwd, home, exists).error.empty());
        CHECK(parseCommandLine({"attach", "-h"}, cwd, home, exists).help);
        CHECK(!usageText("host").empty() && !usageText("attach").empty());
        CHECK(isSubcommand("host") && isSubcommand("attach"));
    }

    GROUP("hostserver:stdio");
    {
        // `mutible host --stdio` (stage 8): one renderer on two pipes, no
        // socket. Its Welcome offers what a window on another machine needs
        // (saves for apps, its own shell); a file typed into is written by
        // the host; a terminal asking for the host's own shell gets the
        // host's environment with the renderer's laid over it, and is the
        // host's child; the renderer's input closing ends the host, its
        // shell with it.
        signal(SIGPIPE, SIG_IGN);   // a write to a pipe nobody reads is EPIPE (Linux)
        const std::string proj = Shared::resolveFolder(scratchFolder());
        {
            std::ofstream f(proj + "/notes.txt");
            f << "one\n";
        }
        int toHost[2], fromHost[2];
        CHECK(pipe(toHost) == 0 && pipe(fromHost) == 0);
        auto server = std::make_unique<Shared::HostServer>();
        Shared::HostOptions o;
        o.folder = proj;
        o.shellEnv = {"PATH=/usr/bin:/bin", "HOME=" + proj, "PS1=$ ", "ENV=", "LANG=C",
                      "SHELL=/bin/sh", "MARK=host-env", "EXTRA=host-value"};
        o.askedFile = proj + "/notes.txt";
        std::string error;
        CHECK(server->startStdio(o, toHost[0], fromHost[1], &error) == Shared::HostServer::Start::Ok);
        std::atomic<int> result{-1};
        std::thread run([&] { result = server->run() ? 1 : 0; });
        PipeRenderer r;
        r.connect(toHost[1], fromHost[0]);
        CHECK(r.client->settle(5000));
        CHECK(r.client->idBase() != 0);
        CHECK(r.client->hostSavesForApps() && r.client->hostRunsOwnShell());
        CHECK(r.client->hostFolder() == proj);
        CHECK(r.client->welcomeJson()["file"].asString() == proj + "/notes.txt");
        const Json st = r.client->sessionState();
        CHECK(st["folder"].asString() == proj);

        // A file opened with the text the renderer read through the host,
        // typed into, and saved by the host.
        const Shared::DocId d = r.client->newId();
        r.mirror.doc = d;
        r.mirror.text = u"one\n";
        r.client->open(d, proj + "/notes.txt", "one\n", &r.mirror);
        r.mirror.caret = r.mirror.length();
        r.mirror.type(u"two\n");
        CHECK(r.client->settle());
        const std::uint64_t req = r.client->requestSave(d, 0, true, "utf8");
        Shared::Client::SaveAnswer a;
        CHECK(req != 0 && r.wait([&] { return r.client->saveAnswer(req, &a); }));
        CHECK(a.ok);
        {
            std::ifstream f(proj + "/notes.txt");
            std::stringstream ss;
            ss << f.rdbuf();
            CHECK(ss.str() == "one\ntwo\n");
        }

        // The host's own shell, its environment under the renderer's.
        Shared::TermSpec spec;
        spec.hostShell = true;
        spec.env = {"EXTRA=from-renderer", "TERM=xterm-256color"};
        spec.cwd = proj;
        spec.scrollback = 1000;
        r.client->openTerminal(5, spec, &r);
        CHECK(r.wait([&] { return r.pid.load() > 0; }));
        r.client->terminalInput(5, "echo \"got-$MARK-$EXTRA-$PPID\"\n");
        const std::string want = "got-host-env-from-renderer-" + std::to_string((int)getpid());
        CHECK(r.wait([&] { return r.screen.text().find(want) != std::string::npos; }));
        if (r.screen.text().find(want) == std::string::npos) std::printf("  screen: %s\n", r.screen.text().c_str());
        const int shell = r.pid.load();
        CHECK(!gone(shell));

        // The renderer's input closes (the connection went): the host ends.
        r.closeInput();
        CHECK(waitUntil([&] { return result.load() != -1; }, 5000));
        CHECK(result.load() == 1);
        if (run.joinable()) run.join();
        server.reset();   // the host's end: its shells hung up, its output closed
        CHECK(waitUntil([&] { return gone(shell); }, 5000));
        CHECK(waitUntil([&] { return r.client->lost(); }, 5000));
    }

    GROUP("hostserver:stdio-target");
    {
        // What `mutible host --stdio PATH` serves (stage 8): a folder, a
        // file (its folder, the file named), or a new file's name in a
        // folder that is there (named, and new); nothing else.
        namespace fs = std::filesystem;
        const std::string proj = Shared::resolveFolder(scratchFolder());
        fs::create_directories(proj + "/sub");
        {
            std::ofstream f(proj + "/notes.txt");
            f << "one\n";
        }
        Shared::StdioTarget t;
        std::string why;
        CHECK(Shared::stdioTarget(proj, &t, &why) && t.folder == proj && t.file.empty() && !t.isNew);
        CHECK(Shared::stdioTarget(proj + "/sub/", &t, &why) && t.folder == proj + "/sub" && t.file.empty());
        CHECK(Shared::stdioTarget(proj + "/notes.txt", &t, &why) && t.folder == proj &&
              t.file == proj + "/notes.txt" && !t.isNew);
        CHECK(Shared::stdioTarget(proj + "/sub/new.txt", &t, &why) && t.folder == proj + "/sub" &&
              t.file == proj + "/sub/new.txt" && t.isNew);
        // Spelled through a link: the folder as resolved, the name as given.
        fs::create_directory_symlink(proj + "/sub", proj + "/via");
        CHECK(Shared::stdioTarget(proj + "/via/later.md", &t, &why) && t.folder == proj + "/sub" &&
              t.file == proj + "/sub/later.md" && t.isNew);
        why.clear();
        CHECK(!Shared::stdioTarget(proj + "/missing/new.txt", &t, &why));
        CHECK(why == proj + "/missing/new.txt: no such file or folder");
        CHECK(!Shared::stdioTarget(proj + "/missing/..", &t, &why));
        CHECK(!Shared::stdioTarget(proj + "/notes.txt/x", &t, &why));   // a file is no folder
        CHECK(!Shared::stdioTarget("", &t, &why));
    }

    GROUP("hostserver:spawn");
    {
        // A command's pipes as a link (HostLink::spawn, what carries a host
        // over ssh): a line sent is a line its output brings back, and once
        // it ends its exit status and standard error are there to say why.
        Shared::Message ping;
        ping.kind = Shared::Kind::Ping;
        ping.req = 42;
        std::string error;
        std::unique_ptr<Shared::HostLink> link = Shared::HostLink::spawn(
            {"sh", "-c", "IFS= read -r l; printf '%s\\n' \"$l\"; echo \"it said: $X\" >&2; exit 3"},
            {"PATH=/usr/bin:/bin", "X=hi"}, &error);
        CHECK(link != nullptr);
        if (link) {
            CHECK(link->pid() > 0 && link->exitStatus() == -1);
            std::mutex mu;
            std::vector<Shared::Message> got;
            std::atomic<bool> ended{false};
            link->start(
                [&](Shared::Message&& m) {
                    std::lock_guard<std::mutex> lk(mu);
                    got.push_back(std::move(m));
                },
                [&] { ended = true; });
            CHECK(link->send(ping));
            CHECK(waitUntil([&] { return ended.load(); }, 5000));
            {
                std::lock_guard<std::mutex> lk(mu);
                CHECK(got.size() == 1 && got[0].kind == Shared::Kind::Ping && got[0].req == 42);
            }
            const int st = link->exitStatus();
            CHECK(st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 3);
            CHECK(link->stderrText().find("it said: hi") != std::string::npos);
            CHECK(link->closed() && !link->send(ping));
            link->close();
        }
        // A command that outlives its input closing is hung up by close().
        link = Shared::HostLink::spawn({"sleep", "30"}, {"PATH=/usr/bin:/bin"}, &error);
        CHECK(link != nullptr);
        if (link) {
            link->start([](Shared::Message&&) {});
            const int pid = link->pid();
            link->close();
            CHECK(gone(pid));
            // Hung up (or, when that did not end it in time, killed), never
            // left to end by itself 30 s on: a check of what close() did,
            // not of how long a loaded machine took to do it.
            const int st = link->exitStatus();
            CHECK(st != -1 && WIFSIGNALED(st) && (WTERMSIG(st) == SIGHUP || WTERMSIG(st) == SIGKILL));
            if (!(st != -1 && WIFSIGNALED(st))) std::printf("  sleep's status: %d\n", st);
        }
        // A program that is not there: no link, or one that ends at once
        // with 127 (a libc that reports exec's failure only so).
        link = Shared::HostLink::spawn({"/nonexistent/ssh-stand-in"}, {}, &error);
        if (link) {
            std::atomic<bool> ended{false};
            link->start([](Shared::Message&&) {}, [&] { ended = true; });
            CHECK(waitUntil([&] { return ended.load(); }, 5000));
            const int st = link->exitStatus();
            CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 127);
        } else {
            CHECK(error.find("could not run /nonexistent/ssh-stand-in") != std::string::npos);
        }
    }

    GROUP("hostserver:ssh-url");
    {
        Shared::SshTarget t;
        std::string why;
        CHECK(Shared::parseSshUrl("ssh://thinkpad/~/proj", &t, &why));
        CHECK(t.user.empty() && t.host == "thinkpad" && t.port == 0 && t.path == "~/proj");
        CHECK(Shared::sshLabel(t) == "thinkpad:~/proj" && Shared::sshMachineKey(t) == "ssh://thinkpad");
        CHECK(Shared::parseSshUrl("SSH://eric@pi.local:2222/srv/www/", &t, &why));
        CHECK(t.user == "eric" && t.host == "pi.local" && t.port == 2222 && t.path == "/srv/www");
        CHECK(Shared::sshLabel(t) == "eric@pi.local:/srv/www");
        CHECK(Shared::sshMachineKey(t) == "ssh://eric@pi.local:2222");
        CHECK(Shared::parseSshUrl("ssh://box", &t, &why) && t.path == "~");
        CHECK(Shared::parseSshUrl("ssh://box/", &t, &why) && t.path == "~");
        CHECK(Shared::parseSshUrl("ssh://box/~", &t, &why) && t.path == "~");
        CHECK(Shared::parseSshUrl("ssh://box/my%20notes/a%27b.txt", &t, &why) && t.path == "/my notes/a'b.txt");
        CHECK(Shared::parseSshUrl("ssh://[fe80::1%25en0]:22/x", &t, &why));
        CHECK(t.host == "fe80::1%25en0" && t.port == 22 && t.path == "/x");
        CHECK(Shared::sshMachineKey(t) == "ssh://[fe80::1%25en0]:22");
        // Refused: not ssh, no host, an option in disguise, a bad port, a
        // host name with what no host name holds, a line break in the path.
        for (const char* bad :
             {"http://box/x", "ssh://", "ssh:///x", "ssh://-oProxyCommand=touch%20x/x", "ssh://-x/y",
              "ssh://-l@box/x", "ssh://box:0/x", "ssh://box:65536/x", "ssh://box:22a/x", "ssh://box:/x",
              "ssh://bo x/y", "ssh://box;rm/x", "ssh://box/a%0Ab", "ssh://box/a%2", "ssh://@box/x",
              "ssh://[::1/x"}) {
            why.clear();
            const bool ok = Shared::parseSshUrl(bad, &t, &why);
            if (ok) std::printf("  taken: %s\n", bad);
            CHECK(!ok && !why.empty());
        }
    }

    GROUP("hostserver:ssh-command");
    {
        Shared::SshTarget t;
        CHECK(Shared::parseSshUrl("ssh://eric@pi:2222/~/it's here", &t, nullptr));
        const std::vector<std::string> a = Shared::sshCommand(t, "/usr/bin/ssh");
        const std::vector<std::string> want = {
            "/usr/bin/ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=20", "-o",
            "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3", "-e", "none", "-p", "2222", "-l",
            "eric", "--", "pi", "mutible host --stdio '~/it'\\''s here'"};
        CHECK(a == want);
        CHECK(Shared::shellQuote("") == "''" && Shared::shellQuote("a b") == "'a b'");
        CHECK(Shared::shellQuote("$(x)`y`") == "'$(x)`y`'");
        CHECK(Shared::parseSshUrl("ssh://box/p", &t, nullptr));
        CHECK(Shared::sshCommand(t, "")[0] == "ssh");
        CHECK(Shared::sshCommand(t, "").size() == 15);   // no -p, no -l

        // Why it never answered, in the window's words.
        auto exited = [](int code) { return code << 8; };   // as waitpid gives it
        std::string s = Shared::explainSshFailure(t, exited(255), "eric@box: Permission denied (publickey).\n");
        CHECK(s.find("password or a key's passphrase") != std::string::npos && s.find("`ssh box`") != std::string::npos);
        s = Shared::explainSshFailure(t, exited(255), "Host key verification failed.\n");
        CHECK(s.find("host key") != std::string::npos && s.find("`ssh box` once in a terminal") != std::string::npos);
        s = Shared::explainSshFailure(t, exited(127), "bash: line 1: mutible: command not found\n");
        CHECK(s.find("mutible is not installed on box") != std::string::npos && s.find("PPA") != std::string::npos);
        s = Shared::explainSshFailure(t, exited(2), "mutible: unknown option --stdio for host\n");
        CHECK(s.find("too old") != std::string::npos);
        s = Shared::explainSshFailure(t, exited(255), "ssh: connect to host box port 22: Connection refused\n\n");
        CHECK(s == "ssh to box failed: ssh: connect to host box port 22: Connection refused");
        s = Shared::explainSshFailure(t, exited(1), "mutible: /home/x/p: no such file or folder\n");
        CHECK(s == "The host on box ended: mutible: /home/x/p: no such file or folder");
        CHECK(Shared::explainSshFailure(t, -1, "") == "The connection to box closed.");
        CHECK(Shared::parseSshUrl("ssh://eric@box:2200/p", &t, nullptr));
        s = Shared::explainSshFailure(t, exited(255), "Permission denied\n");
        CHECK(s.find("`ssh -p 2200 eric@box`") != std::string::npos);
    }

    GROUP("hostserver:own-shell");
    {
        // TermSpec's hostShell: the host's login shell as a login shell, in
        // the host's environment with the renderer's laid over it.
        Shared::PtySpec p = Shared::ownShellSpec(
            "", {"SHELL=/usr/bin/zsh", "PATH=/remote/bin", "TERM=dumb", "HOME=/home/e", "junk"},
            {"TERM=xterm-256color", "COLORTERM=truecolor"}, "/home/e/proj", 100, 30);
        CHECK(p.program == "/usr/bin/zsh" && p.argv == std::vector<std::string>{"-zsh"});
        CHECK(p.cwd == "/home/e/proj" && p.cols == 100 && p.rows == 30);
        const std::vector<std::string> env = {"SHELL=/usr/bin/zsh", "PATH=/remote/bin", "HOME=/home/e",
                                              "TERM=xterm-256color", "COLORTERM=truecolor"};
        CHECK(p.env == env);
        p = Shared::ownShellSpec("/bin/bash", {"SHELL=/usr/bin/zsh"}, {}, "/", 80, 24);
        CHECK(p.program == "/bin/bash" && p.argv == std::vector<std::string>{"-bash"});
        p = Shared::ownShellSpec("", {}, {}, "/", 80, 24);   // the password database's, or sh
        CHECK(!p.program.empty() && p.program[0] == '/' && p.argv.size() == 1 && p.argv[0][0] == '-');
        // Through the protocol: the flag is kept, and an older spec has none.
        Shared::TermSpec s;
        s.hostShell = true;
        CHECK(Shared::termSpecFrom(Shared::termSpecJson(s)).hostShell);
        CHECK(!Shared::termSpecFrom(Shared::termSpecJson(Shared::TermSpec())).hostShell);
        CHECK(!Shared::termSpecJson(Shared::TermSpec()).has("hostShell"));
    }
}
