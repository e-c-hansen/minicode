// SessionLink.cpp — see SessionLink.h.
#include "SessionLink.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>

#include <atomic>
#include <cerrno>
#include <set>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "ControlPaths.h"
#include "EditorTabs.h"
#include "EnvLock.h"
#include "SessionHost.h"
#include "SessionProtocol.h"
#include "SessionServer.h"

extern char** environ;

namespace SessionLink {

// What the client's wake reaches: the link's dispatch on the main loop, once
// per batch. Shared with the idle sources it queues, so a link that went
// meanwhile is simply not dispatched; the host's or reader's thread never
// holds the link itself (its last reference must go on the main thread).
struct Link::Wake {
    std::atomic<bool> queued{false};
    std::weak_ptr<Link> link;   // the main thread's only
};

struct Link::Viewers {
    std::mutex mu;
    std::map<Shared::ClientId, Shared::Client*> clients;
};

namespace {

// Every link, by folder, while something holds it.
std::map<std::string, std::weak_ptr<Link>>& registry() {
    static auto* r = new std::map<std::string, std::weak_ptr<Link>>();
    return *r;
}

std::string envString(const char* name) {
    EnvLock::EnvRead r;
    const char* v = std::getenv(name);
    return v ? v : "";
}

// Where a headless host started from here writes what it says:
// $XDG_STATE_HOME/mutible/host-<hash>.log, made anew by each start.
std::string hostLogPath(const std::string& folder) {
    const std::string dir = std::string(g_get_user_state_dir()) + "/mutible";
    g_mkdir_with_parents(dir.c_str(), 0700);
    std::string name = Shared::hostSocketName(folder);
    const std::string sock = ".sock";
    if (name.size() > sock.size()) name.resize(name.size() - sock.size());
    return dir + "/" + name + ".log";
}

// `mutible host --folder <folder>` started as this program (the GTK binary
// takes the CLI's subcommands), fully detached: its own session, through a
// second fork so that init (or the nearest subreaper) is its parent and
// reaps it, never this app; stdin from /dev/null, stdout and stderr to a log
// under the state folder; nothing of this process's open files. Its pid,
// or -1 with why.
pid_t startHostProcess(const std::string& folder, std::string* why) {
    std::vector<std::string> args = {"mutible", "host", "--folder", folder};
    const std::string idle = envString("MUTIBLE_HOST_IDLE_MINUTES");
    if (!idle.empty()) {
        args.push_back("--idle-minutes");
        args.push_back(idle);
    }
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    // The environment copied now, under the lock (EnvLock.h): after the
    // fork only async-signal-safe calls run.
    std::vector<std::string> env;
    {
        EnvLock::EnvRead r;
        for (char** e = environ; e && *e; e++) env.push_back(*e);
    }
    std::vector<char*> envp;
    for (std::string& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);
    const std::string logPath = hostLogPath(folder);
    const int devNull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int log = open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (log < 0) log = open("/dev/null", O_WRONLY | O_CLOEXEC);
    int pipeFds[2] = {-1, -1};
    if (devNull < 0 || log < 0 || pipe2(pipeFds, O_CLOEXEC) != 0) {
        *why = std::string("could not prepare its start: ") + std::strerror(errno);
        for (int fd : {devNull, log, pipeFds[0], pipeFds[1]})
            if (fd >= 0) close(fd);
        return -1;
    }
    const pid_t child = fork();
    if (child == 0) {
        setsid();
        const pid_t grandchild = fork();
        if (grandchild == 0) {
            // Signals as a fresh program has them: none blocked, the ones
            // the host waits for or reaps with at their defaults.
            sigset_t none;
            sigemptyset(&none);
            sigprocmask(SIG_SETMASK, &none, nullptr);
            struct sigaction dfl;
            std::memset(&dfl, 0, sizeof dfl);
            dfl.sa_handler = SIG_DFL;
            for (int s : {SIGCHLD, SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGQUIT})
                sigaction(s, &dfl, nullptr);
            dup2(devNull, 0);
            dup2(log, 1);
            dup2(log, 2);
            // Nothing else of the app's: its display, its sockets, its pipes.
            if (syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
                for (int fd = 3; fd < 4096; fd++) close(fd);
            execve("/proc/self/exe", argv.data(), envp.data());
            _exit(127);
        }
        const pid_t sent = grandchild;
        ssize_t n;
        do n = write(pipeFds[1], &sent, sizeof sent);
        while (n < 0 && errno == EINTR);
        _exit(grandchild > 0 ? 0 : 1);
    }
    close(pipeFds[1]);
    close(devNull);
    close(log);
    if (child < 0) {
        *why = std::string("could not fork: ") + std::strerror(errno);
        close(pipeFds[0]);
        return -1;
    }
    pid_t host = -1;
    ssize_t n;
    do n = read(pipeFds[0], &host, sizeof host);
    while (n < 0 && errno == EINTR);
    close(pipeFds[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (n != (ssize_t)sizeof host || host <= 0) {
        *why = "could not start `mutible host`";
        return -1;
    }
    return host;
}

}  // namespace

// ---------------------------------------------------------------- a link

int Link::dispatchOnMain(void* p) {
    const std::shared_ptr<Wake>& w = *static_cast<std::shared_ptr<Wake>*>(p);
    w->queued = false;
    if (std::shared_ptr<Link> l = w->link.lock()) l->dispatch();
    return G_SOURCE_REMOVE;
}

void Link::makeClient() {
    client_ = std::make_unique<Shared::Client>(Model::Unit::CodePoint, "gtk");
    // The windows back up their unsaved files themselves (Persistence.h), so
    // a headless host keeps no backup of a document while one has it open.
    client_->setKeepsBackups(true);
    // Its tabs save through a host that saves for apps (stage 7: Hello flag
    // 16, so a page may save a file a window shows), and hear the host's
    // count of each document as unsaved and its writes for others, which go
    // to the document by its id (listen). On the main loop, inside dispatch.
    client_->setSavesThroughHost(true);
    client_->hearDirty([this](Shared::DocId doc, bool dirty) {
        auto it = notices_.find(doc);
        if (it != notices_.end()) it->second->hostDirty(dirty);
    });
    client_->hearWrites([this](Shared::DocId doc, std::uint64_t v, const Json& json) {
        auto it = notices_.find(doc);
        if (it != notices_.end()) it->second->hostWrote(v, json);
    });
    // The host's messages arrive on its thread (or the socket's reader);
    // the main loop dispatches them, once per batch.
    std::shared_ptr<Wake> w = wake_;
    client_->setWake([w] {
        if (!w->queued.exchange(true))
            g_idle_add_full(G_PRIORITY_DEFAULT, dispatchOnMain, new std::shared_ptr<Wake>(w),
                            [](gpointer p) { delete static_cast<std::shared_ptr<Wake>*>(p); });
    });
}

void Link::startInProcess(bool loopback, unsigned delayUs) {
    kind_ = loopback ? Kind::Loopback : Kind::InProcess;
    socketPath_.clear();
    startedHost_ = 0;
    makeClient();
    Shared::Client* c = client_.get();
    auto wire = [loopback](Shared::Message&& m) { return loopback ? Shared::roundTrip(m) : std::move(m); };
    viewers_ = std::make_shared<Viewers>();
    std::shared_ptr<Viewers> v = viewers_;
    host_ = std::make_unique<Shared::SessionHost>(
        [c, wire, v](Shared::ClientId to, Shared::Message&& m) {
            {
                std::lock_guard<std::mutex> lk(v->mu);
                auto it = v->clients.find(to);
                if (it != v->clients.end()) {
                    it->second->deliver(wire(std::move(m)));
                    return;
                }
            }
            c->deliver(wire(std::move(m)));
        },
        delayUs);
    Shared::SessionHost* h = host_.get();
    // The disk, for the browser's page of this window (its tree and Source
    // Control follow it), before any renderer. Only roots a page names in a
    // watch are walked, so a window with no page served starts none; the
    // window keeps its own monitors and follows its tabs itself (this
    // client asks for no Hello flag 32).
    h->watchDisk(true);
    hostId_ = h->connect();
    const Shared::ClientId id = hostId_;
    client_->connect([h, id, wire](Shared::Message&& m) { h->post(id, wire(std::move(m))); });
    // The Welcome, with the base of this renderer's ids (Client::newId).
    if (!client_->settle()) g_printerr("mutible: the session's host did not answer\n");
}

std::function<void()> Link::connectViewer(Shared::Client& client) {
    if (!host_ || !viewers_) return nullptr;
    Shared::SessionHost* h = host_.get();
    const Shared::ClientId id = h->connect();
    std::shared_ptr<Viewers> v = viewers_;
    {
        std::lock_guard<std::mutex> lk(v->mu);
        v->clients[id] = &client;
    }
    const bool loopback = kind_ == Kind::Loopback;
    client.connect([h, id, loopback](Shared::Message&& m) {
        h->post(id, loopback ? Shared::roundTrip(m) : std::move(m));
    });
    return [h, id, v] {
        {
            std::lock_guard<std::mutex> lk(v->mu);
            v->clients.erase(id);
        }
        h->disconnect(id);
    };
}

// The folder's headless host, found or started, attached to. False, with
// why, when there is none to be had; nothing of it is kept then.
bool Link::startSocket(std::string* why) {
    if (!g_file_test(folder_.c_str(), G_FILE_TEST_IS_DIR)) {
        *why = folder_.empty() ? "the window has no folder" : folder_ + " is not a folder";
        return false;
    }
    const std::vector<std::string> dirs = ControlChannel::socketFolders();
    std::string path = Shared::findHostSocket(folder_, dirs), error;
    bool refused = false;
    std::unique_ptr<Shared::HostLink> link;
    if (!path.empty()) link = Shared::HostLink::open(path, &error, &refused);
    pid_t started = 0;
    if (!link) {
        // A host that answered and was refused (another user's) is not
        // replaced; a socket nothing listens on (a host that died) or none
        // at all gets a host of our own, which removes it.
        if (!path.empty() && !refused) {
            *why = error;
            return false;
        }
        started = startHostProcess(folder_, why);
        if (started <= 0) return false;
        // Its socket, within a few seconds: a host starts in milliseconds.
        const gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
        while (!link && g_get_monotonic_time() < deadline) {
            path = Shared::findHostSocket(folder_, dirs);
            refused = false;
            if (!path.empty()) link = Shared::HostLink::open(path, &error, &refused);
            if (!link && !path.empty() && !refused) {
                *why = error;
                return false;
            }
            if (!link) {
                if (kill(started, 0) != 0 && errno == ESRCH) break;   // it ended, reaped already
                g_usleep(20 * 1000);
            }
        }
        if (!link) {
            *why = "the `mutible host` it started (pid " + std::to_string(started) +
                   ") did not answer within 5 seconds; see " + hostLogPath(folder_);
            return false;
        }
    }
    kind_ = Kind::Socket;
    makeClient();
    Shared::Client* c = client_.get();
    Shared::HostLink* raw = link.get();
    // Delivered on the link's reader thread; its end (the host gone, or our
    // own detach) leaves the client waiting for nothing.
    raw->start([c](Shared::Message&& m) { c->deliver(std::move(m)); }, [c] { c->hostLost(); });
    client_->connect([raw](Shared::Message&& m) { raw->send(m); });
    socket_ = std::move(link);
    // The Welcome, with the base of this renderer's ids.
    if (!client_->settle(5000) || !client_->idBase()) {
        *why = "the host at " + path + " did not welcome it";
        socket_->close();
        socket_.reset();
        client_.reset();
        return false;
    }
    socketPath_ = path;
    startedHost_ = started;
    return true;
}

Link::~Link() {
    // The transport first: its thread delivers to the client.
    if (socket_) socket_->close();   // detaches: the host keeps everything else
    socket_.reset();
    host_.reset();                   // the in-process host's thread joined
    client_.reset();
}

void Link::unlisten(Shared::DocId doc, DocNotices* n) {
    auto it = notices_.find(doc);
    if (it != notices_.end() && it->second == n) notices_.erase(it);
}

void Link::detach() {
    if (!socket_ || detached_) return;
    detached_ = true;
    socket_->close();
}

void Link::dispatch() {
    client_->dispatch();
    for (const std::string& e : client_->takeErrors())
        g_printerr("mutible: the session's host for %s: %s\n", folder_.c_str(), e.c_str());
    if (!client_->lost() || detached_ || lostTold_) return;
    lostTold_ = true;
    const std::string message = "The session's host for " + folder_ +
                                " went away: undo is off in its files and its terminals ended";
    g_printerr("mutible: the session's host for %s went away (%s); its files stay open "
               "without undo and its terminals ended\n",
               folder_.c_str(), socketPath_.c_str());
    // A listener may take itself off.
    const auto listeners = lostListeners_;
    for (const auto& [token, f] : listeners)
        if (lostListeners_.count(token)) f(message);
}

int Link::addLostListener(std::function<void(const std::string&)> f) {
    const int token = ++nextListener_;
    lostListeners_[token] = std::move(f);
    return token;
}

void Link::removeLostListener(int token) { lostListeners_.erase(token); }

std::uint64_t Link::hostWindow(std::uint64_t window) {
    auto it = hostWindow_.find(window);
    if (it != hostWindow_.end()) return it->second;
    return hostWindow_[window] = client_->newId();
}

Json Link::tabsJson(std::uint64_t window, const Model::Window& w) const {
    Json tabs = Shared::windowJson(w);
    Json terms = Json::array();
    auto t = windowTerms_.find(window);
    if (t != windowTerms_.end())
        for (std::uint64_t id : t->second) terms.push(Json((int64_t)id));
    tabs.set("terms", std::move(terms));
    auto l = windowLayout_.find(window);
    if (l != windowLayout_.end() && !l->second.isNull()) tabs.set("termLayout", l->second);
    auto p = windowPanes_.find(window);
    if (p != windowPanes_.end()) {
        if (!p->second.browser.isNull()) tabs.set("browser", p->second.browser);
        if (!p->second.panes.isNull()) tabs.set("panes", p->second.panes);
        if (!p->second.reviews.isNull()) tabs.set("reviews", p->second.reviews);
    }
    return tabs;
}

void Link::windowPanes(std::uint64_t window, const Model::Window& w, Json browser, Json panes, Json reviews) {
    windowPanes_[window] = {std::move(browser), std::move(panes), std::move(reviews)};
    windowTabs(window, w);
}

void Link::windowTerminals(std::uint64_t window, const Model::Window& w, std::vector<std::uint64_t> terms,
                           Json layout) {
    windowTerms_[window] = std::move(terms);
    windowLayout_[window] = std::move(layout);
    windowTabs(window, w);
}

void Link::windowDetached(std::uint64_t window, const Model::Window& w) {
    auto it = hostWindow_.find(window);
    const bool kept = w.count() > 0 || (windowTerms_.count(window) && !windowTerms_[window].empty());
    if (it != hostWindow_.end() && kept) {
        // Sent again even when unchanged: its "seen" makes it the latest
        // record a closing window left, and "detached" tells a page it is
        // closed (the Mac marks its own the same way).
        Json tabs = tabsJson(window, w);
        tabs.set("detached", Json(true));
        client_->windowTabs(it->second, std::move(tabs));
        hostWindow_.erase(it);
        sentTabs_.erase(window);
        windowTerms_.erase(window);
        windowLayout_.erase(window);
        windowPanes_.erase(window);
        return;
    }
    windowClosed(window);
}

Link::Held Link::free(bool all) {
    if (client_->lost()) return Held();
    std::set<std::uint64_t> live;
    for (const auto& [w, h] : hostWindow_) live.insert(h);
    Shared::Client& c = *client_;
    return Shared::reattachFrom(
        c.sessionState(), c.clientId(), live, [&c](Shared::DocId d) { return c.isOpen(d); },
        [&c](Shared::TermId t) { return c.showsTerminal(t); }, all);
}

Link::Held Link::takeForWindow(std::uint64_t window) {
    Held h = free(false);
    if (h.window) {
        // What the window reported before it took the record (its tabs as
        // it was made) goes, or a later window could take that empty record
        // for the latest one.
        auto had = hostWindow_.find(window);
        if (had != hostWindow_.end() && had->second != h.window) client_->windowClosed(had->second);
        hostWindow_[window] = h.window;
        sentTabs_.erase(window);
    }
    return h;
}

Link::Held Link::allFree() { return free(true); }

void Link::windowTabs(std::uint64_t window, const Model::Window& w) {
    Json tabs = tabsJson(window, w);
    std::string dump = tabs.dump();
    auto it = sentTabs_.find(window);
    if (it != sentTabs_.end() && it->second == dump) return;
    sentTabs_[window] = std::move(dump);
    client_->windowTabs(hostWindow(window), std::move(tabs));
}

void Link::windowClosed(std::uint64_t window) {
    sentTabs_.erase(window);
    windowTerms_.erase(window);
    windowLayout_.erase(window);
    windowPanes_.erase(window);
    auto it = hostWindow_.find(window);
    if (it == hostWindow_.end()) return;   // never sent
    client_->windowClosed(it->second);
    hostWindow_.erase(it);
}

std::string Link::checkDocuments(const std::vector<std::pair<std::uint64_t, std::string>>& docs) {
    Shared::Client& c = *client_;
    if (c.lost()) return "";   // nothing to compare with
    std::string out;
    if (!c.settle()) return "the session's host did not answer";
    for (const auto& [id, text] : docs) {
        if (!c.isOpen(id)) {
            out += "document " + std::to_string(id) + " is not open on the host; ";
            continue;
        }
        const Shared::Client::HostDoc h = c.hostDocument(id);
        if (!h.open) out += "the host has no document " + std::to_string(id) + "; ";
        else if (Model::utf8FromUtf16(h.text) != text)
            out += "the host's text of document " + std::to_string(id) + " differs from its buffer (" +
                   std::to_string(h.text.size()) + " units against " + std::to_string(text.size()) +
                   " bytes); ";
    }
    for (const std::string& e : c.takeErrors()) out += "the host said: " + e + "; ";
    return out;
}

std::string Link::checkWindow(std::uint64_t window, const Model::Window& w) {
    Shared::Client& c = *client_;
    if (c.lost()) return "";
    std::string out;
    // The tabs as the host has them, once the window's are sent (the strip
    // sends them as it is drawn; a change that drew nothing is sent here).
    windowTabs(window, w);
    const Json want = tabsJson(window, w);
    const Json state = c.sessionState();
    if (!state.isObject()) return "the session's host did not answer";
    const std::uint64_t onHost = hostWindow(window);
    bool found = false;
    for (const Json& win : state["windows"].elements())
        if ((std::uint64_t)win["window"].asInt() == onHost) {
            found = true;
            if (win["tabs"] != want)
                out += "the host's tabs of window " + std::to_string(window) + " are " + win["tabs"].dump() +
                       ", the window's " + want.dump() + "; ";
        }
    if (!found && w.count() > 0) out += "the host has no tabs for window " + std::to_string(window) + "; ";
    for (const std::string& e : c.takeErrors()) out += "the host said: " + e + "; ";
    return out;
}

// ---------------------------------------------------------------- links

namespace {

std::string folderKey(const std::string& root) {
    std::string folder = Shared::resolveFolder(root);
    return folder.empty() ? root : folder;
}

// In-process links kept while they hold what closed windows left
// (keepWhileHolding), by folder.
std::map<std::string, std::shared_ptr<Link>>& kept() {
    static auto* k = new std::map<std::string, std::shared_ptr<Link>>();
    return *k;
}

}  // namespace

bool folderLost(const std::string& root) {
    std::shared_ptr<Link> have = registry()[folderKey(root)].lock();
    return have && have->lost();
}

void keepWhileHolding(const std::shared_ptr<Link>& link) {
    if (!link) return;
    auto& k = kept();
    if (link->kind() == Link::Kind::Socket || link->lost()) {
        if (k.count(link->folder()) && k[link->folder()] == link) k.erase(link->folder());
        return;
    }
    const Json st = link->client().sessionState();
    const bool holding = st.isObject() && (st["documents"].size() > 0 || st["terminals"].size() > 0 ||
                                           st["windows"].size() > 0);
    if (holding) k[link->folder()] = link;
    else if (k.count(link->folder()) && k[link->folder()] == link) k.erase(link->folder());
}

void endDetached() {
    for (const std::shared_ptr<Link>& l : all()) {
        if (l->lost()) continue;
        Shared::Client& c = l->client();
        const Link::Held h = l->allFree();
        for (const Link::HeldDoc& d : h.docs) {
            c.open(d.id, "", "", nullptr);   // a seat, to close it with
            c.close(d.id);
        }
        for (const Link::HeldTerm& t : h.terms) {
            c.attachTerminal(t.id, nullptr);
            c.closeTerminal(t.id);
        }
        for (std::uint64_t w : h.records) c.windowClosed(w);
        c.settle();
    }
    // Kept links holding nothing now go (their hosts' threads end).
    std::vector<std::shared_ptr<Link>> keptNow;
    for (const auto& [folder, l] : kept()) keptNow.push_back(l);
    for (const std::shared_ptr<Link>& l : keptNow) keepWhileHolding(l);
}

std::shared_ptr<Link> forFolder(const std::string& root, bool headless) {
    const std::string folder = folderKey(root);
    auto& reg = registry();
    // A link whose host went is passed over: its holders keep it until they
    // move to the new one.
    if (std::shared_ptr<Link> have = reg[folder].lock(); have && !have->lost()) return have;
    if (kept().count(folder) && kept()[folder]->lost()) kept().erase(folder);
    std::shared_ptr<Link> link(new Link());
    link->folder_ = folder;
    link->wake_ = std::make_shared<Link::Wake>();
    link->wake_->link = link;
    const std::string mode = envString("MUTIBLE_HOST");
    const unsigned delayUs = (unsigned)std::strtoul(envString("MUTIBLE_HOST_DELAY_US").c_str(), nullptr, 10);
    const bool socket = mode == "socket" || (mode.empty() && headless);
    std::string why;
    if (socket && link->startSocket(&why)) {
        if (link->startedHost())
            g_printerr("mutible: started the session's host for %s (pid %d, %s)\n", folder.c_str(),
                       (int)link->startedHost(), link->socketPath().c_str());
    } else {
        if (socket)
            g_printerr("mutible: the session's host for %s stays in this process: %s\n", folder.c_str(),
                       why.c_str());
        const bool loopback = mode == "loopback";
        static bool told = false;
        if (loopback && !told)
            g_printerr("mutible: the session's host: every message through the protocol's encoding\n");
        if (delayUs && !told) g_printerr("mutible: the session's host: each message %u us late\n", delayUs);
        told = told || loopback || delayUs;
        link->startInProcess(loopback, delayUs);
    }
    reg[folder] = link;
    return link;
}

std::vector<std::shared_ptr<Link>> all() {
    std::vector<std::shared_ptr<Link>> out;
    for (auto it = registry().begin(); it != registry().end();) {
        if (std::shared_ptr<Link> l = it->second.lock()) {
            out.push_back(l);
            ++it;
        } else {
            it = registry().erase(it);
        }
    }
    return out;
}

void detachAll() {
    for (const std::shared_ptr<Link>& l : all()) l->detach();
    kept().clear();   // in-process hosts end, their shells hung up
}

}  // namespace SessionLink
