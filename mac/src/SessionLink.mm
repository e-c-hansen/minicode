// SessionLink.mm — see SessionLink.h.
#import "SessionLink.h"

#import "AppSettings.h"
#import "BackupStore.h"
#import "EditorTab.h"
#import "Environment.h"
#import "Lsp.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <os/log.h>
#include <thread>
#include <vector>

#include "ControlPaths.h"
#include "Ot.h"
#include "SessionFiles.h"
#include "SessionHost.h"
#include "SessionProtocol.h"
#include "SessionServer.h"

static os_log_t SessionLog(void) {
    static os_log_t log = os_log_create("ai.ible.mutible", "session");
    return log;
}

@interface MCSessionDoc ()
- (void)hostDirty:(BOOL)dirty;
- (void)hostWrote:(uint64_t)version json:(const Json &)json;
@end

// ---------------------------------------------------------------- documents by id

// Each open document by its client and id, for the host's notices that name
// a document (DocDirty), which the client hands over by id. Main thread
// only: the clients dispatch there.
static NSMapTable<NSString *, MCSessionDoc *> *DocRegistry(void) {
    static NSMapTable *table;
    if (!table) table = [NSMapTable strongToWeakObjectsMapTable];
    return table;
}

static NSString *DocKey(const Shared::Client *c, Shared::DocId doc) {
    return [NSString stringWithFormat:@"%p/%llu", (const void *)c, (unsigned long long)doc];
}

// What every client of the app's says and hears (stage 7): its tabs save
// through a host that saves for apps (Hello flag 16), and the host's
// notices of a document's unsaved state and of its writes go to the tab.
static void HearHostSaves(Shared::Client *c) {
    c->setSavesThroughHost(true);
    c->hearDirty([c](Shared::DocId doc, bool dirty) { [[DocRegistry() objectForKey:DocKey(c, doc)] hostDirty:dirty]; });
    c->hearWrites([c](Shared::DocId doc, std::uint64_t v, const Json &json) {
        [[DocRegistry() objectForKey:DocKey(c, doc)] hostWrote:v json:json];
    });
}

// ---------------------------------------------------------------- the host

namespace {

// Both live as long as the process: the host's thread waits for messages
// until the app exits, and no destructor runs at exit to join it.
Shared::SessionHost *gHost = nullptr;
Shared::Client *gClient = nullptr;
Shared::ClientId gId = 0;
bool gLoopback = false;
std::atomic<bool> gDispatchQueued{false};
// Renderers other than the app's own client (a browser's view,
// MCSessionConnectViewer), by their id on the host: each message the host
// sends one of them goes to its client, under the lock, so letting one go
// stops its deliveries at once.
std::mutex gViewersMu;
std::map<Shared::ClientId, Shared::Client *> gViewers;

Shared::Message Wire(Shared::Message &&m) { return gLoopback ? Shared::roundTrip(m) : std::move(m); }

void DispatchOnMain() {
    gDispatchQueued = false;
    MCSession::processLink()->dispatch();
}

void Start() {
    const char *mode = std::getenv("MUTIBLE_HOST");
    gLoopback = mode && std::string(mode) == "loopback";
    const char *delay = std::getenv("MUTIBLE_HOST_DELAY_US");
    const unsigned delayUs = delay ? (unsigned)std::strtoul(delay, nullptr, 10) : 0;
    gClient = new Shared::Client(Model::Unit::Utf16, "mac");
    // The windows back up their unsaved files themselves (BackupStore), so a
    // host keeps no backup of a document while one has it open (Hello flag
    // 4). A browser's view (MCSessionConnectViewer) says nothing of the kind.
    gClient->setKeepsBackups(true);
    HearHostSaves(gClient);
    // The host's messages arrive on its thread; the main queue dispatches
    // them, once per batch, at the top of a run-loop turn (never inside a
    // text system callback; a wait that dispatches there defers the edits,
    // MCSessionDoc's hostEdited).
    gClient->setWake([] {
        if (!gDispatchQueued.exchange(true)) dispatch_async(dispatch_get_main_queue(), ^{ DispatchOnMain(); });
    });
    gHost = new Shared::SessionHost(
        [](Shared::ClientId to, Shared::Message &&m) {
            {
                std::lock_guard<std::mutex> lk(gViewersMu);
                auto it = gViewers.find(to);
                if (it != gViewers.end()) {
                    it->second->deliver(Wire(std::move(m)));
                    return;
                }
            }
            gClient->deliver(Wire(std::move(m)));
        },
        delayUs);
    // The disk, for a browser's page of a window (its tree and Source
    // Control follow it). The windows keep their own FSEvents streams and
    // follow their tabs themselves: this client hears none of it (no Hello
    // flag 32), and the host follows no document a window has open.
    gHost->watchDisk(true);
    gId = gHost->connect();
    gClient->connect([](Shared::Message &&m) { gHost->post(gId, Wire(std::move(m))); });
    // The Welcome, with the base of this renderer's ids (Client::newId).
    if (!gClient->settle()) os_log_error(SessionLog(), "the session's host did not answer");
}

std::string EnvString(NSString *name) {
    NSString *v = MCEnv(name);
    return v ? std::string(v.UTF8String ?: "") : std::string();
}

std::string FolderKey(const std::string &root) {
    if (root.empty()) return root;
    std::string f = Shared::resolveFolder(root);
    return f.empty() ? root : f;
}

// Whether `path` is `folder` or inside it.
bool Under(const std::string &path, const std::string &folder) {
    if (folder.empty() || path.size() < folder.size() || path.compare(0, folder.size(), folder) != 0) return false;
    return path.size() == folder.size() || path[folder.size()] == '/' || folder.back() == '/';
}

// The command line's client, beside the app's executable (the bundle's
// Contents/MacOS, or build/ for the GUI checks).
NSString *CliPath(void) {
    NSBundle *b = NSBundle.mainBundle;
    NSMutableArray<NSString *> *tries = [NSMutableArray array];
    if (NSString *aux = [b pathForAuxiliaryExecutable:@"mutible-cli"]) [tries addObject:aux];
    NSString *exeDir = b.executablePath.stringByDeletingLastPathComponent;
    if (exeDir.length) [tries addObject:[exeDir stringByAppendingPathComponent:@"mutible-cli"]];
    for (NSString *p in tries)
        if ([NSFileManager.defaultManager isExecutableFileAtPath:p]) return p;
    return nil;
}

// ~/Library/Logs/Mutible/host-<hash>.log, made anew by each start.
NSString *HostLogPath(const std::string &folder) {
    NSString *dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/Mutible"];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    std::string name = Shared::hostSocketName(folder);
    const std::string sock = ".sock";
    if (name.size() > sock.size()) name.resize(name.size() - sock.size());
    return [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.log", name.c_str()]];
}

}  // namespace

NSDictionary<NSString *, NSString *> *MCSessionHostEnvironment(void) {
    NSMutableDictionary<NSString *, NSString *> *e = [MCEnvironment() mutableCopy];
    // The host's backups go in this app's store: the CLI finds the store by
    // $HOME, which is not where NSHomeDirectory (CFFIXED_USER_HOME, a
    // sandbox) may put the app's.
    if (NSString *dir = [MCBackupStore shared].directory) e[@"MUTIBLE_BACKUP_DIR"] = dir;
    return e;
}

namespace {

// `mutible-cli host --folder <folder>`, in a session of its own, stdin from
// /dev/null, stdout and stderr to its log, nothing else of this process's
// open files, signals at their defaults. It is this process's child until
// the app quits (launchd's then), reaped by a thread of its own when it
// ends first. Its pid, or -1 with why.
pid_t StartHostProcess(const std::string &folder, std::string *why) {
    NSString *cli = CliPath();
    if (!cli) {
        *why = "mutible-cli is not beside the app";
        return -1;
    }
    std::vector<std::string> args = {"mutible-cli", "host", "--folder", folder};
    const std::string idle = EnvString(@"MUTIBLE_HOST_IDLE_MINUTES");
    if (!idle.empty()) {
        args.push_back("--idle-minutes");
        args.push_back(idle);
    }
    std::vector<char *> argv;
    for (std::string &a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<std::string> env;
    NSDictionary<NSString *, NSString *> *e = MCSessionHostEnvironment();
    for (NSString *k in e) env.push_back(std::string(k.UTF8String ?: "") + "=" + (e[k].UTF8String ?: ""));
    std::vector<char *> envp;
    for (std::string &x : env) envp.push_back(x.data());
    envp.push_back(nullptr);
    const std::string logPath = HostLogPath(folder).fileSystemRepresentation;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&at);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK |
                                      POSIX_SPAWN_SETSIGDEF);
    pid_t pid = -1;
    const int r = posix_spawn(&pid, cli.fileSystemRepresentation, &fa, &at, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (r != 0) {
        *why = std::string("could not start `mutible host`: ") + std::strerror(r);
        return -1;
    }
    std::thread([pid] {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }).detach();
    return pid;
}

// Every folder link, by folder, while something holds it.
std::map<std::string, std::weak_ptr<MCSession::Link>> &Registry() {
    static auto *r = new std::map<std::string, std::weak_ptr<MCSession::Link>>();
    return *r;
}

// The documents and terminals a record names (its groups' tabs; "terms").
void NamedIn(const Json &tabs, std::vector<std::uint64_t> *docs, std::vector<std::uint64_t> *terms) {
    for (const Json &g : tabs["groups"].elements())
        for (const Json &t : g["tabs"].elements())
            if (std::uint64_t d = (std::uint64_t)t["doc"].asInt()) docs->push_back(d);
    for (const Json &t : tabs["terms"].elements())
        if (std::uint64_t id = (std::uint64_t)t.asInt()) terms->push_back(id);
}

}  // namespace

BOOL MCSessionHostEnabled(void) {
    static const BOOL forced = [] {
        const char *e = std::getenv("MUTIBLE_SESSION_HOST");
        return (BOOL)(e && (std::string(e) == "on" || std::string(e) == "1"));
    }();
    return forced || [AppSettings shared].settings.sessionHost();
}

Shared::Client &MCSessionClient(void) {
    if (!gClient) Start();
    return *gClient;
}

std::function<void()> MCSessionConnectViewer(Shared::Client &client) {
    MCSessionClient();
    const Shared::ClientId id = gHost->connect();
    {
        std::lock_guard<std::mutex> lk(gViewersMu);
        gViewers[id] = &client;
    }
    client.connect([id](Shared::Message &&m) { gHost->post(id, Wire(std::move(m))); });
    return [id] {
        {
            std::lock_guard<std::mutex> lk(gViewersMu);
            gViewers.erase(id);
        }
        gHost->disconnect(id);
    };
}

BOOL MCSessionLoopback(void) {
    MCSessionClient();
    return gLoopback;
}

// ---------------------------------------------------------------- links

namespace MCSession {

Link::~Link() {
    // The transport first: its thread delivers to the client.
    if (socket_) socket_->close();   // detaches: the host keeps everything else
    socket_.reset();
    owned_.reset();
}

void Link::detach() {
    if (!socket_ || detached_) return;
    detached_ = true;
    socket_->close();
}

void Link::dispatch() {
    client_->dispatch();
    pumpStreams();
    for (const std::string &e : client_->takeErrors())
        os_log_error(SessionLog(), "the session's host%{public}s%{public}s: %{public}s",
                     folder_.empty() ? "" : " for ", folder_.c_str(), e.c_str());
    if (!client_->lost() || detached_ || lostTold_) return;
    lostTold_ = true;
    // ssh's end, said in the window's words (the link reaped it before it
    // told the client).
    if (kind_ == Kind::Remote)
        failure_ = Shared::explainSshFailure(target_, socket_ ? socket_->exitStatus() : -1,
                                             socket_ ? socket_->stderrText() : std::string());
    os_log_error(SessionLog(), "the session's host for %{public}s went away (%{public}s)", folder_.c_str(),
                 socketPath_.c_str());
    const auto listeners = lostListeners_;   // a listener may take itself off
    for (const auto &[token, f] : listeners)
        if (lostListeners_.count(token)) f();
}

int Link::addLostListener(std::function<void()> f) {
    const int token = ++nextListener_;
    lostListeners_[token] = std::move(f);
    return token;
}

void Link::removeLostListener(int token) { lostListeners_.erase(token); }

int Link::addDiskListener(std::function<void(const Json &)> f) {
    const int token = ++nextListener_;
    diskListeners_[token] = std::move(f);
    return token;
}

void Link::removeDiskListener(int token) { diskListeners_.erase(token); }

std::uint64_t Link::stream(Json request, std::function<void(std::vector<Json> &&, bool)> answers) {
    if (!client_ || lost() || !client_->hostSearches()) return 0;
    const std::uint64_t req = client_->requestStream(0, std::move(request));
    if (req) streams_[req] = std::move(answers);
    return req;
}

void Link::cancelStream(std::uint64_t req) {
    if (!streams_.erase(req) || !client_) return;
    client_->cancelStream(req, 0);
}

// Each stream's answers since the last dispatch, to its handler; one that
// is done is forgotten first, so its handler may start another.
void Link::pumpStreams() {
    if (streams_.empty()) return;
    std::vector<std::uint64_t> reqs;
    for (const auto &[req, f] : streams_) reqs.push_back(req);
    for (std::uint64_t req : reqs) {
        auto it = streams_.find(req);
        if (it == streams_.end()) continue;   // cancelled by a handler before it
        std::vector<Json> got;
        const bool done = client_->takeStream(req, &got) || client_->lost();
        if (got.empty() && !done) continue;
        auto f = done ? std::move(it->second) : it->second;
        if (done) streams_.erase(it);
        if (f) f(std::move(got), done);
    }
}

std::uint64_t Link::newWindow() {
    const std::uint64_t id = client_->newId();
    if (id) live_.insert(id);
    return id;
}

void Link::windowTabs(std::uint64_t window, Json tabs) {
    if (!window || lost()) return;
    std::string dump = tabs.dump();
    auto it = sent_.find(window);
    if (it != sent_.end() && it->second == dump) return;
    sent_[window] = std::move(dump);
    client_->windowTabs(window, std::move(tabs));
}

void Link::windowDetached(std::uint64_t window, Json tabs, bool keep) {
    if (!window) return;
    live_.erase(window);
    if (lost()) {
        sent_.erase(window);
        return;
    }
    if (keep) {
        // Sent again even when unchanged: its "seen" makes it the latest
        // record a closing window left, and "detached" says it is closed.
        tabs.set("detached", Json(true));
        sent_.erase(window);
        client_->windowTabs(window, std::move(tabs));
        return;
    }
    sent_.erase(window);
    client_->windowClosed(window);
}

void Link::windowClosed(std::uint64_t window) {
    live_.erase(window);
    if (sent_.erase(window) && !lost()) client_->windowClosed(window);
}

// What a window takes is the core's (Shared::reattachFrom, both apps): the
// latest free record with what it names, then what no record names; never
// two documents of one path; an untitled buffer too. The process's host
// serves every folder, so before the core chooses, a record of another
// folder counts as an open window's (left alone, and what it names with
// it), and of what no record names only documents under the folder are
// kept, and no terminals, which say no folder of their own.
Held Link::free(bool all, const std::string &root) {
    Held out;
    if (client_->lost()) return out;
    Json st = client_->sessionState();
    if (!st.isObject()) return out;
    std::set<std::uint64_t> live = live_;
    if (kind_ == Kind::InProcess && !all) {
        const std::string folder = FolderKey(root);
        std::set<std::uint64_t> named, namedTerms;
        for (const Json &win : st["windows"].elements()) {
            std::vector<std::uint64_t> d, t;
            NamedIn(win["tabs"], &d, &t);
            named.insert(d.begin(), d.end());
            namedTerms.insert(t.begin(), t.end());
            if (FolderKey(win["tabs"]["root"].asString()) != folder) live.insert((std::uint64_t)win["window"].asInt());
        }
        Json docs = Json::array(), terms = Json::array();
        for (const Json &d : st["documents"].elements()) {
            const std::string &path = d["path"].asString();
            if (named.count((std::uint64_t)d["doc"].asInt()) || (!path.empty() && Under(path, folder))) docs.push(d);
        }
        for (const Json &t : st["terminals"].elements())
            if (namedTerms.count((std::uint64_t)t["term"].asInt())) terms.push(t);
        st.set("documents", std::move(docs));
        st.set("terminals", std::move(terms));
    }
    Shared::Client &c = *client_;
    const Shared::Reattach r = Shared::reattachFrom(
        st, c.clientId(), live, [&c](Shared::DocId d) { return c.isOpen(d); },
        [&c](Shared::TermId t) { return c.showsTerminal(t); }, all);
    // The terminals' sizes, which the core's choice does not carry.
    std::map<std::uint64_t, std::pair<int, int>> sizes;
    for (const Json &t : st["terminals"].elements())
        sizes[(std::uint64_t)t["term"].asInt()] = {(int)t["cols"].asInt(80), (int)t["rows"].asInt(24)};
    out.window = r.window;
    out.tabs = r.tabs;
    out.records = r.records;
    for (const Shared::Reattach::Doc &d : r.docs) out.docs.push_back({d.id, d.path, d.dirty});
    for (const Shared::Reattach::Term &t : r.terms) {
        HeldTerm h;
        h.id = t.id;
        h.pid = t.pid;
        h.shell = t.shell;
        auto sz = sizes.find(t.id);
        h.cols = sz == sizes.end() ? 80 : sz->second.first;
        h.rows = sz == sizes.end() ? 24 : sz->second.second;
        out.terms.push_back(h);
    }
    return out;
}

Held Link::takeForWindow(std::uint64_t *window, const std::string &root) {
    Held h = free(false, root);
    if (h.window && window) {
        // What the window reported before it took the record (its tabs as
        // it was made) goes, or a later window could take that record for
        // the latest one.
        if (*window && *window != h.window) {
            live_.erase(*window);
            sent_.erase(*window);
            if (!lost()) client_->windowClosed(*window);
        }
        *window = h.window;
        live_.insert(h.window);
        sent_.erase(h.window);
    }
    return h;
}

Held Link::allFree() { return free(true, ""); }

// The folder's headless host, found or started, attached to. False, with
// why, when there is none to be had; nothing of it is kept then.
bool Link::startSocket(std::string *why) {
    BOOL dir = NO;
    if (folder_.empty() || ![NSFileManager.defaultManager fileExistsAtPath:@(folder_.c_str()) isDirectory:&dir] ||
        !dir) {
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
        // replaced; a socket nothing listens on, or none, gets a host of
        // our own, which removes it.
        if (!path.empty() && !refused) {
            *why = error;
            return false;
        }
        started = StartHostProcess(folder_, why);
        if (started <= 0) return false;
        // Its socket, within a few seconds: a host starts in milliseconds.
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:5];
        while (!link && deadline.timeIntervalSinceNow > 0) {
            path = Shared::findHostSocket(folder_, dirs);
            refused = false;
            if (!path.empty()) link = Shared::HostLink::open(path, &error, &refused);
            if (!link && !path.empty() && !refused) {
                *why = error;
                return false;
            }
            if (!link) {
                if (kill(started, 0) != 0 && errno == ESRCH) break;
                usleep(20 * 1000);
            }
        }
        if (!link) {
            *why = "the `mutible host` it started (pid " + std::to_string(started) +
                   ") did not answer within 5 seconds; see " + std::string(HostLogPath(folder_).UTF8String);
            return false;
        }
    }
    kind_ = Kind::Socket;
    if (!attachClient(std::move(link), Registry()[folder_], 5000)) {
        *why = "the host at " + path + " did not welcome it";
        socket_.reset();
        owned_.reset();
        client_ = nullptr;
        return false;
    }
    socketPath_ = path;
    startedHost_ = started;
    return true;
}

bool Link::attachClient(std::unique_ptr<Shared::HostLink> link, std::weak_ptr<Link> weak, int timeoutMs) {
    owned_ = std::make_unique<Shared::Client>(Model::Unit::Utf16, "mac");
    owned_->setKeepsBackups(true);   // as the process's client: the windows keep their own
    HearHostSaves(owned_.get());
    // A window over SSH hears the remote disk (Hello flag 32): its tree's
    // folders listed again as they change there (FsChanged).
    if (kind_ == Kind::Remote)
        owned_->hearDisk(
            [weak](std::uint64_t, const Json &changed) {
                std::shared_ptr<Link> l = weak.lock();
                if (!l) return;
                const auto listeners = l->diskListeners_;   // a listener may take itself off
                for (const auto &[token, f] : listeners)
                    if (l->diskListeners_.count(token)) f(changed);
            },
            [](Shared::DocId, int) {});
    client_ = owned_.get();
    // The host's messages arrive on the link's reader thread; the main queue
    // dispatches them, once per batch. The block holds the link weakly, so
    // a link let go meanwhile is simply not dispatched.
    queued_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> q = queued_;
    client_->setWake([q, weak] {
        if (q->exchange(true)) return;
        dispatch_async(dispatch_get_main_queue(), ^{
            q->store(false);
            if (std::shared_ptr<Link> l = weak.lock()) l->dispatch();
        });
    });
    link->connect(*client_);
    socket_ = std::move(link);
    if (!client_->settle(timeoutMs) || !client_->idBase()) {
        // Closed, not let go: a command's status and last words stay for
        // the caller to say why, which then lets it go.
        socket_->close();
        return false;
    }
    return true;
}

std::string Link::key() const {
    return kind_ == Kind::Remote ? machine_ + folder_ : folder_;
}

// `mutible host --stdio <path>` on the target, through the user's ssh (or
// $MUTIBLE_SSH, the checks' stand-in), attached to over its pipes. False,
// with why in the window's words, when its Welcome does not come or the
// host there is too old for a window over SSH; nothing of it is kept then.
bool Link::startRemote(std::weak_ptr<Link> self, std::string *why) {
    std::string ssh = EnvString(@"MUTIBLE_SSH");
    if (ssh.empty()) ssh = "/usr/bin/ssh";
    std::vector<std::string> env;
    NSDictionary<NSString *, NSString *> *e = MCEnvironment();
    for (NSString *k in e) env.push_back(std::string(k.UTF8String ?: "") + "=" + (e[k].UTF8String ?: ""));
    std::string error;
    std::unique_ptr<Shared::HostLink> link = Shared::HostLink::spawn(Shared::sshCommand(target_, ssh), env, &error);
    if (!link) {
        *why = error;
        return false;
    }
    Shared::HostLink *transport = link.get();
    kind_ = Kind::Remote;
    if (!attachClient(std::move(link), self, 30000)) {
        // close() waited for ssh's end: its status and what it said.
        *why = Shared::explainSshFailure(target_, transport->exitStatus(), transport->stderrText());
        socket_.reset();
        owned_.reset();
        client_ = nullptr;
        return false;
    }
    if (!client_->hostSavesForApps() || !client_->hostRunsOwnShell() || client_->hostFolder().empty()) {
        *why = "mutible on " + target_.host + " is too old for a window over SSH. Upgrade it there, then open "
               "the address again.";
        socket_->close();
        socket_.reset();
        owned_.reset();
        client_ = nullptr;
        return false;
    }
    folder_ = client_->hostFolder();
    askedFile_ = client_->welcomeJson()["file"].asString();
    askedFileNew_ = !askedFile_.empty() && client_->welcomeJson()["new"].asBool();
    return true;
}

MCDiskText *Link::readText(const std::string &path) {
    MCDiskText *d = [MCDiskText new];
    if (lost() || folder_.empty() || !Under(path, folder_) || path.size() <= folder_.size() + 1) return d;
    const std::string rel = path.substr(folder_.size() + 1);
    // 64 MB at most, as the host follows a file (SessionFiles.h's kMaxFollowed).
    const std::size_t limit = std::size_t(64) << 20;
    std::string bytes;
    for (;;) {
        Json ask = Json::object();
        ask.set("op", Json("read"));
        ask.set("path", Json(rel));
        ask.set("offset", Json((double)bytes.size()));
        ask.set("length", Json((double)Shared::kMaxReadBytes));
        ask.set("hidden", Json(true));
        std::string piece;
        const std::uint64_t req = client_->requestRead(0, std::move(ask));
        Json answer;
        if (!req || !client_->settle(20000) || !client_->readAnswer(req, &answer, &piece)) {
            if (req) client_->forgetRead(req);
            return d;
        }
        if (!answer["error"].isNull()) return d;
        const double size = answer["size"].asNumber(0);
        if (size > (double)limit) {
            d.tooLarge = YES;
            return d;
        }
        bytes += piece;
        if (piece.empty() || (double)bytes.size() >= size) break;
    }
    FolderSearch::TextEncoding encoding = FolderSearch::TextEncoding::Utf8;
    bool bom = false;
    if (!FolderSearch::decodeFileText(&bytes, &encoding, &bom)) return d;
    d.bom = bom;
    d.encoding = (MCTextEncoding)encoding;
    d.text = [[NSString alloc] initWithBytes:bytes.data() length:bytes.size() encoding:NSUTF8StringEncoding];
    return d;
}

std::shared_ptr<Link> processLink() {
    static std::shared_ptr<Link> link;
    if (!link) {
        link.reset(new Link());
        link->kind_ = Link::Kind::InProcess;
        link->client_ = &MCSessionClient();
    }
    return link;
}

static bool Headless(void) {
    const std::string mode = EnvString(@"MUTIBLE_HOST");
    if (!mode.empty()) return mode == "socket";
    return [AppSettings shared].settings.sessionHostHeadless();
}

std::shared_ptr<Link> forFolder(const std::string &root) {
    if (!Headless()) return processLink();
    const std::string folder = FolderKey(root);
    if (folder.empty()) return processLink();
    auto &reg = Registry();
    if (std::shared_ptr<Link> have = reg[folder].lock(); have && !have->lost()) return have;
    std::shared_ptr<Link> link(new Link());
    link->folder_ = folder;
    reg[folder] = link;   // before starting: its wake holds it by this
    std::string why;
    if (link->startSocket(&why)) {
        if (link->startedHost())
            os_log(SessionLog(), "started the session's host for %{public}s (pid %d, %{public}s)", folder.c_str(),
                   (int)link->startedHost(), link->socketPath().c_str());
        return link;
    }
    os_log_error(SessionLog(), "the session's host for %{public}s stays in this process: %{public}s",
                 folder.c_str(), why.c_str());
    reg.erase(folder);
    return processLink();
}

// Every remote link, by its machine and path as the address named them,
// while something holds it.
static std::map<std::string, std::weak_ptr<Link>> &RemoteRegistry() {
    static auto *r = new std::map<std::string, std::weak_ptr<Link>>();
    return *r;
}

std::shared_ptr<Link> forRemote(const std::string &address, std::string *why) {
    Shared::SshTarget t;
    std::string error;
    if (!Shared::parseSshUrl(address, &t, &error)) {
        if (why) *why = error;
        return nullptr;
    }
    const std::string name = Shared::sshMachineKey(t) + "\n" + t.path;
    auto &reg = RemoteRegistry();
    if (std::shared_ptr<Link> have = reg[name].lock(); have && !have->lost()) return have;
    std::shared_ptr<Link> link(new Link());
    link->target_ = t;
    link->address_ = address;
    link->machine_ = Shared::sshMachineKey(t);
    reg[name] = link;   // before starting: its wake holds it by this
    if (!link->startRemote(link, &error)) {
        os_log_error(SessionLog(), "no window over ssh for %{public}s: %{public}s", address.c_str(), error.c_str());
        reg.erase(name);
        if (why) *why = error;
        return nullptr;
    }
    // The title's "host:path": the folder as the address names it, so a
    // file's address shows its folder.
    Shared::SshTarget folder = t;
    if (!link->askedFile_.empty()) {
        const std::size_t slash = t.path.rfind('/');
        folder.path = slash == std::string::npos ? "~" : slash == 0 ? "/" : t.path.substr(0, slash);
    }
    link->label_ = Shared::sshLabel(folder);
    os_log(SessionLog(), "a window over ssh: %{public}s, the host's folder %{public}s", address.c_str(),
           link->folder_.c_str());
    return link;
}

std::shared_ptr<Link> linkForPath(const std::string &path) {
    if (!path.empty())
        for (auto &[folder, weak] : Registry())
            if (std::shared_ptr<Link> l = weak.lock(); l && !l->lost() && Under(path, folder)) return l;
    return processLink();
}

bool folderLost(const std::string &root) {
    auto &reg = Registry();
    auto it = reg.find(FolderKey(root));
    if (it == reg.end()) return false;
    std::shared_ptr<Link> l = it->second.lock();
    return l && l->lost();
}

std::vector<std::shared_ptr<Link>> all() {
    std::vector<std::shared_ptr<Link>> out;
    if (gClient) out.push_back(processLink());
    for (auto *reg : {&Registry(), &RemoteRegistry()})
        for (auto it = reg->begin(); it != reg->end();) {
            if (std::shared_ptr<Link> l = it->second.lock()) {
                out.push_back(l);
                ++it;
            } else {
                it = reg->erase(it);
            }
        }
    return out;
}

void detachAll() {
    for (const std::shared_ptr<Link> &l : all()) l->detach();
}

void endDetached() {
    for (const std::shared_ptr<Link> &l : all()) {
        if (l->lost()) continue;
        Shared::Client &c = l->client();
        const Held h = l->allFree();
        for (const HeldDoc &d : h.docs) {
            c.open(d.id, "", "", nullptr);   // a seat, to close it with
            c.close(d.id);
        }
        for (const HeldTerm &t : h.terms) {
            c.attachTerminal(t.id, nullptr);
            c.closeTerminal(t.id);
        }
        for (std::uint64_t w : h.records) c.windowClosed(w);
        c.settle();
    }
}

}  // namespace MCSession

@implementation MCSessionWrite
@end

@implementation MCSessionLinkRef {
    std::shared_ptr<MCSession::Link> _link;
}
- (instancetype)initWithLink:(std::shared_ptr<MCSession::Link>)link {
    if ((self = [super init])) _link = std::move(link);
    return self;
}
- (std::shared_ptr<MCSession::Link>)link { return _link; }
@end

@implementation MCSessionTermPlace
@end

// ---------------------------------------------------------------- helpers

static std::u16string U16(NSString *s) {
    std::u16string out(s.length, u'\0');
    if (s.length) [s getCharacters:(unichar *)out.data() range:NSMakeRange(0, s.length)];
    return out;
}

static NSString *NSFromU16(const std::u16string &s) {
    return [NSString stringWithCharacters:(const unichar *)s.data() length:s.size()];
}

static std::string Utf8(NSString *s) {
    return Model::utf8FromUtf16(U16(s ?: @""));
}

static BOOL IsHigh(unichar c) { return c >= 0xD800 && c <= 0xDBFF; }
static BOOL IsLow(unichar c) { return c >= 0xDC00 && c <= 0xDFFF; }

static Shared::Selection SelectionOf(NSArray<NSValue *> *ranges) {
    Shared::Selection out;
    for (NSValue *v in ranges) {
        const NSRange r = v.rangeValue;
        out.push_back({r.location, NSMaxRange(r)});
    }
    return out;
}

// The edit `op`, made on `text`, widened to whole code points: an edit
// that starts or ends inside a surrogate pair takes the pair's other half
// with it (and puts it back), so a renderer that counts code points can
// follow it (SESSION-DESIGN.md, "Stage 4a as built").
static Ot::Op WholePoints(NSString *text, Ot::Op op) {
    const NSUInteger n = text.length;
    if (op.pos > 0 && op.pos < n && IsLow([text characterAtIndex:op.pos]) &&
        IsHigh([text characterAtIndex:op.pos - 1])) {
        op.ins.insert(op.ins.begin(), (char16_t)[text characterAtIndex:op.pos - 1]);
        op.pos--;
        op.del++;
    }
    const NSUInteger end = op.pos + op.del;
    if (end > 0 && end < n && IsHigh([text characterAtIndex:end - 1]) && IsLow([text characterAtIndex:end])) {
        op.ins.push_back((char16_t)[text characterAtIndex:end]);
        op.del++;
    }
    return op;
}

// ---------------------------------------------------------------- a document

@class MCSessionDoc;
@interface MCSessionDoc ()
- (void)hostEdited:(const Ot::Ops &)ops flag:(int)flag select:(const Shared::Selection *)select;
@end

namespace {
struct DocListener : Shared::Client::Listener {
    __weak MCSessionDoc *doc = nil;
    void hostEdited(Shared::DocId, const Ot::Ops &ops, int flag, const Shared::Selection *select) override {
        [doc hostEdited:ops flag:flag select:select];
    }
};
}  // namespace

@implementation MCSessionDoc {
    std::unique_ptr<DocListener> _listener;
    MCSessionLinkRef *_linkRef;
    std::shared_ptr<MCSession::Link> _link;
    std::string _hostPath;   // the path the host was told
    BOOL _capturing;         // a held one's text coming back (initWithTab:held:)
    NSMutableString *_captured;
    BOOL _open;
    BOOL _stepOpen, _named;
    // The step is replaceRanges:'s own (an explicit undo group of its own
    // under NSUndoManager), how deep its calls are, and whether it ended:
    // a later edit of the turn then starts a new step, as a new group did.
    BOOL _explicit, _sealed;
    NSInteger _namedDepth;
    BOOL _forgetting;     // read afresh this turn: a whole set resets
    BOOL _flushQueued;
    // The host's edits not in the mirror yet, in the mirror's text as it
    // is: applied once the storage is done editing.
    Ot::Ops _deferred;
    Shared::Selection _deferredSelect;
    BOOL _deferredHasSelect;
    __weak CodeTextView *_view;   // the view the step's edits were made in
    // The host's latest count of it as unsaved, waiting for the next turn
    // (hostDirty:), with the tab's text generation it was heard at.
    BOOL _dirtyQueued, _hostDirty;
    uint64_t _dirtyGeneration;
}

// The path the host has the tab's document by: none for an untitled buffer
// the host held (MCTab untitled) until it is saved to its file.
static std::string HostPathOf(MCTab *tab) { return tab.untitled ? std::string() : Utf8(tab.currentPath); }

// The tab's window's link (EditorController+Session.mm), else the one
// holding its path.
static MCSessionLinkRef *LinkForTab(MCTab *tab) {
    id owner = tab.owner;
    if ([owner respondsToSelector:@selector(sessionLinkForTab:)])
        if (MCSessionLinkRef *r = [owner sessionLinkForTab:tab]) return r;
    return [[MCSessionLinkRef alloc]
        initWithLink:MCSession::linkForPath(tab.currentPath ? std::string(tab.currentPath.fileSystemRepresentation)
                                                            : std::string())];
}

- (instancetype)initWithTab:(MCTab *)tab text:(NSString *)text {
    if ((self = [super init])) {
        _tab = tab;
        _linkRef = LinkForTab(tab);
        _link = _linkRef.link;
        _docId = [self client].newId();   // from the client's range on the host
        _listener = std::make_unique<DocListener>();
        _listener->doc = self;
        // UTF-8 cannot carry a lone surrogate (no text read from a file has
        // one: such a file goes to Quick Look); one typed in becomes U+FFFD
        // in the host's copy, which the GUI checks would catch.
        _hostPath = HostPathOf(tab);
        [self client].open(_docId, _hostPath, Utf8(text), _listener.get());
        _open = YES;
        [self enrol];
    }
    return self;
}

// Known by its client and id to the host's notices (DocRegistry).
- (void)enrol {
    [DocRegistry() setObject:self forKey:DocKey(&[self client], _docId)];
}

- (void)unenrol {
    NSString *key = DocKey(&[self client], _docId);
    if ([DocRegistry() objectForKey:key] == self) [DocRegistry() removeObjectForKey:key];
}

- (instancetype)initWithTab:(MCTab *)tab held:(Shared::DocId)docId link:(MCSessionLinkRef *)link {
    if ((self = [super init])) {
        _tab = tab;
        _linkRef = link;
        _link = link.link;
        _docId = docId;
        _listener = std::make_unique<DocListener>();
        _listener->doc = self;
        _hostPath = HostPathOf(tab);
        // The host's text comes back as one insertion (Client::open with an
        // empty text), taken here rather than put in a mirror.
        _capturing = YES;
        _captured = [NSMutableString string];
        [self client].open(_docId, _hostPath, "", _listener.get());
        _open = YES;
        [self enrol];
        const BOOL answered = [self client].settle(5000);
        _capturing = NO;
        _heldText = answered ? [_captured copy] : nil;
        _captured = nil;
    }
    return self;
}

- (Shared::Client &)client { return _link->client(); }
- (MCSessionLinkRef *)link { return _linkRef; }
- (BOOL)open { return _open && !_link->lost(); }

- (void)dealloc { [self close]; }

- (void)close {
    if (!_open) return;
    [self unenrol];
    _open = NO;
    _stepOpen = NO;
    if (_link && !_link->lost() && [self client].isOpen(_docId)) [self client].close(_docId);
}

- (void)detach {
    if (!_open) return;
    [self closeStep];   // what this turn recorded goes first
    [self unenrol];
    _open = NO;
    _stepOpen = NO;
    if (!_link->lost() && [self client].isOpen(_docId)) [self client].detach(_docId);
}

- (void)pathChanged {
    const std::string now = HostPathOf(_tab);
    if (!_open || now == _hostPath || _link->lost()) return;
    _hostPath = now;
    [self client].setPath(_docId, now);
}

- (void)rehostOn:(MCSessionLinkRef *)link saved:(NSString *)saved {
    if (!link || link.link == _link) return;
    // Nothing reaches a lost host; the client's entry goes.
    if ([self client].isOpen(_docId)) [self client].close(_docId);
    [self unenrol];
    _stepOpen = NO;
    _explicit = _sealed = NO;
    _deferred.clear();
    _deferredHasSelect = NO;
    _linkRef = link;
    _link = link.link;
    _docId = [self client].newId();
    NSString *now = _tab.currentText ?: @"";
    _hostPath = HostPathOf(_tab);
    [self client].open(_docId, _hostPath, Utf8(now), _listener.get());
    _open = YES;
    [self enrol];
    [self hostSaved:saved];
}

- (void)hostSaved:(NSString *)saved {
    if (!saved || !self.open) return;
    NSString *now = _tab.currentText ?: @"";
    if ([saved isEqualToString:now]) return;
    // The file's text as the host's saved text, and the buffer's difference
    // from it an edit outside the history: the host counts it unsaved, and
    // its lifetime keeps an unsaved file.
    [self resetTo:saved];
    const std::u16string x = U16(saved), y = U16(now);
    const size_t a = x.size(), b = y.size();
    const size_t pre = (size_t)(std::mismatch(x.begin(), x.begin() + (long)std::min(a, b), y.begin()).first -
                                x.begin());
    size_t suf = 0;
    while (suf < a - pre && suf < b - pre && x[a - 1 - suf] == y[b - 1 - suf]) suf++;
    [self client].recordOutside(_docId, Ot::Op{pre, a - pre - suf, y.substr(pre, b - pre - suf)});
}

- (NSUInteger)deferredEdits { return _deferred.size(); }

// The view showing the tab's text, or nil when the tab is not in the
// storage (a background tab, a preview, a viewer).
- (CodeTextView *)mirrorView {
    MCTab *t = _tab;
    id owner = t.owner;
    if (!t || ![owner respondsToSelector:@selector(textViewShowingSourceOf:)]) return nil;
    return [owner textViewShowingSourceOf:t];
}

- (Shared::Selection)selectionNow:(CodeTextView *)view {
    if (view) return SelectionOf(view.selectedRanges);
    const NSRange r = _tab.selection;
    return {{r.location, NSMaxRange(r)}};
}

// ---- steps

- (void)openStepWithName:(NSString *)name view:(CodeTextView *)view {
    if (_stepOpen || !_open) return;
    _explicit = _sealed = NO;
    Model::UndoHistory::StepInfo info{"Edit", false};
    if (name) info = {name.UTF8String ?: "Edit", false};
    else if (view.sessionTyping > 0) info = {"Typing", true};
    _view = view;
    [self client].openStep(_docId, info, [self selectionNow:view]);
    _stepOpen = YES;
    _named = name != nil;
    // The step ends with the run-loop turn, as NSUndoManager's group by
    // event did: everything one key press, menu item or check step did is
    // one step.
    __weak MCSessionDoc *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf closeStep]; });
}

- (void)closeStep {
    if (!_stepOpen) return;
    _stepOpen = NO;
    _explicit = _sealed = NO;
    CodeTextView *view = _view;
    if (view && view != [self mirrorView]) view = nil;
    [self client].closeStep(_docId, [self selectionNow:view]);
}

- (void)nameStep:(NSString *)name {
    if (!_open || !name.length) return;
    if (!_stepOpen) return;   // asked after a step of the turn: nothing to name
    if (_named) return;
    [self client].renameStep(_docId, {name.UTF8String ?: "Edit", false});
    _named = YES;
}

// replaceRanges:withStrings:actionName: begins (endNamed when it ends). It
// made an explicit undo group of its own under NSUndoManager, nested in
// whatever group was open: inside a group (the tab's undo manager's, or
// another replaceRanges:'s), or after this turn's typing, it is part of
// that step; otherwise it is a step of its own, which a later edit of the
// turn does not join.
- (void)beginNamed:(NSString *)name view:(CodeTextView *)view {
    if (!_open) return;
    const BOOL nested = _namedDepth > 0 || view.undoManager.groupingLevel > 0;
    _namedDepth++;
    if (_stepOpen && !_sealed && (nested || !_explicit)) {
        [self nameStep:name];
        return;
    }
    [self closeStep];
    [self openStepWithName:name view:view];
    _explicit = !nested;
}

- (void)endNamed {
    if (_namedDepth > 0 && --_namedDepth == 0 && _stepOpen && _explicit) _sealed = YES;
}

- (void)breakCoalescing {
    if (_open) [self client].breakCoalescing(_docId);
}

- (void)noteSelection:(NSArray<NSValue *> *)ranges {
    if (_open && !_stepOpen && !_applying) [self client].noteSelection(_docId, SelectionOf(ranges));
}

- (void)forgetHistory {
    if (!_open) return;
    [self closeStep];
    [self client].forgetHistory(_docId);
}

- (void)saved {
    if (_open) {
        [self closeStep];
        [self client].saved(_docId);
    }
}

- (BOOL)savedAs:(NSString *)text {
    if (!self.open || !text) return NO;
    [self closeStep];
    return [self client].savedAs(_docId, U16(text));
}

- (BOOL)savePending {
    return self.open && [self client].dirtyFenced(_docId);
}

// ---- saving through the host (stage 7)

- (BOOL)hostSavesFile {
    return self.open && [self client].hostSavesForApps();
}

- (MCHostSave)saveOnHostEncoding:(FolderSearch::TextEncoding)encoding bom:(BOOL)bom {
    MCHostSave out;
    if (!self.hostSavesFile) return out;
    Shared::Client &c = [self client];
    // What this turn recorded goes first: the save follows every edit sent.
    [self closeStep];
    const std::uint64_t req = c.requestSave(_docId, 0, true, FolderSearch::encodingName(encoding, bom));
    if (!req) return out;
    Shared::Client::SaveAnswer a;
    // The answer comes after every edit the host had before it, each of
    // which settle() puts in the mirror (or defers, as any wait does).
    if (!c.settle(10000) || !c.saveAnswer(req, &a)) {
        os_log_error(SessionLog(), "the session's host did not answer a save of %{public}@",
                     _tab.currentPath ?: @"a file");
        return out;
    }
    out.answered = true;
    out.ok = a.ok;
    out.reason = a.json["reason"].asString();
    out.why = a.json["why"].asString();
    out.note = a.json["note"].asString();
    out.created = a.json["created"].asBool();
    bool b = false;
    FolderSearch::TextEncoding e = encoding;
    if (FolderSearch::parseEncodingName(a.json["encodingName"].asString(), &e, &b)) {
        out.encoding = e;
        out.bom = b;
    } else {   // a host without the name: as asked
        out.encoding = encoding;
        out.bom = bom;
    }
    out.written = Shared::stampFromJson(a.json["stamp"]);
    out.inStep = a.ok && c.version(_docId) == a.version && [self mirrorInStep];
    return out;
}

// Whether the tab's text is the host's at the version last heard: nothing
// of this tab's waits for the host (another tab's edits do not count),
// and nothing of the host's waits for the storage.
- (BOOL)mirrorInStep {
    return self.open && [self client].unconfirmed(_docId) == 0 && _deferred.empty();
}

- (void)hostDirty:(BOOL)dirty {
    MCTab *tab = _tab;
    if (!tab || ![self mirrorInStep]) return;
    // Taken at the top of a turn, not inside the dispatch (which a wait can
    // run anywhere), and only if the tab's text has not changed since; the
    // latest of several heard before then (a reload's edit, then its save).
    _hostDirty = dirty;
    _dirtyGeneration = tab.textGeneration;
    if (_dirtyQueued) return;
    _dirtyQueued = YES;
    __weak MCSessionDoc *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        MCSessionDoc *d = weakSelf;
        if (!d) return;
        d->_dirtyQueued = NO;
        MCTab *t = d->_tab;
        if (!t || !d.open || t.textGeneration != d->_dirtyGeneration) return;
        id owner = t.owner;
        if ([owner respondsToSelector:@selector(tab:dirtyFromHost:)]) [owner tab:t dirtyFromHost:d->_hostDirty];
    });
}

- (void)hostWrote:(uint64_t)version json:(const Json &)json {
    MCTab *tab = _tab;
    if (!tab || !self.open) return;
    MCSessionWrite *w = [MCSessionWrite new];
    // The text written, when the tab's is the host's at that version.
    if ([self mirrorInStep] && [self client].version(_docId) == version) w.text = [tab.currentText copy] ?: @"";
    const std::string path = json["path"].asString();
    w.path = path.empty() ? nil : @(path.c_str());
    FolderSearch::TextEncoding e = FolderSearch::TextEncoding::Utf8;
    bool b = false;
    w.knowsEncoding = FolderSearch::parseEncodingName(json["encodingName"].asString(), &e, &b);
    w.encoding = e;
    w.bom = b;
    w.stamp = Shared::stampFromJson(json["stamp"]);
    __weak MCTab *weakTab = tab;
    dispatch_async(dispatch_get_main_queue(), ^{
        MCTab *t = weakTab;
        id owner = t.owner;
        if (t && [owner respondsToSelector:@selector(tab:writtenByHost:)]) [owner tab:t writtenByHost:w];
    });
}

// ---- recording

// `outside`: the edit is in no undo step (recordOutside).
- (void)record:(Ot::Op)op outside:(BOOL)outside {
    Shared::Client &c = [self client];
    Ot::Ops mine{std::move(op)};
    if (!_deferred.empty()) {
        // The host's edits not in the mirror yet came first, as the client
        // sees it: the mirror's edit is moved past them, and they past it.
        Ot::Ops moved, theirs;
        Ot::transform(mine, _deferred, Model::Unit::Utf16, &moved, &theirs);
        _deferred = std::move(theirs);
        mine = std::move(moved);
    }
    for (Ot::Op &o : mine) {
        if (outside) c.recordOutside(_docId, std::move(o));
        else c.record(_docId, std::move(o));
    }
}

- (void)record:(Ot::Op)op {
    [self record:std::move(op) outside:NO];
}

- (void)storageWillReplace:(NSRange)range with:(NSString *)string view:(CodeTextView *)view {
    if (!_open || _applying) return;
    // The storage edited outside a change of the view's (directly, as
    // nothing in the app does but a check may): NSUndoManager had no step
    // for it, and later undos moved past it. The host keeps it so: the
    // turn's step ends before it, and it is in none.
    if (!view.sessionChanging) {
        [self closeStep];
        [self record:WholePoints(view.textStorage.string, Ot::Op{range.location, range.length,
                                                                  U16(string ?: @"")})
             outside:YES];
        return;
    }
    if (_sealed) [self closeStep];
    if (!_stepOpen) [self openStepWithName:nil view:view];
    NSString *text = view.textStorage.string;
    [self record:WholePoints(text, Ot::Op{range.location, range.length, U16(string ?: @"")})];
}

- (void)textWillChangeFrom:(NSString *)old to:(NSString *)now {
    if (!_open || _applying) return;
    old = old ?: @"";
    now = now ?: @"";
    if (old == now || [old isEqualToString:now]) return;
    if (_forgetting) {
        [self resetTo:now];
        return;
    }
    // The one range in which they differ.
    const std::u16string x = U16(old), y = U16(now);
    const size_t a = x.size(), b = y.size();
    const size_t pre = (size_t)(std::mismatch(x.begin(), x.begin() + (long)std::min(a, b), y.begin()).first -
                                x.begin());
    size_t suf = 0;
    while (suf < a - pre && suf < b - pre && x[a - 1 - suf] == y[b - 1 - suf]) suf++;
    NSString *ins = [now substringWithRange:NSMakeRange(pre, b - pre - suf)];
    if (_sealed) [self closeStep];
    if (!_stepOpen) [self openStepWithName:nil view:nil];
    [self record:WholePoints(old, Ot::Op{pre, a - pre - suf, U16(ins)})];
}

- (void)resetTo:(NSString *)text {
    _stepOpen = NO;
    _explicit = _sealed = NO;
    _deferred.clear();
    _deferredHasSelect = NO;
    [self client].reset(_docId, Utf8(text));
}

- (void)forgetHistoryWithText:(NSString *)text {
    if (!_open) return;
    [self resetTo:text ?: @""];
    if (_forgetting) return;
    _forgetting = YES;
    __weak MCSessionDoc *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        MCSessionDoc *d = weakSelf;
        if (d) d->_forgetting = NO;
    });
}

// ---- the host's edits

static BOOL StorageBusy(CodeTextView *view) {
    // Inside an edit of the storage (between edited: and the end of
    // processEditing, its notifications included), nothing else may edit
    // it.
    return view.textStorage.editedMask != 0;
}

- (void)hostEdited:(const Ot::Ops &)ops flag:(int)flag select:(const Shared::Selection *)select {
    if (_capturing) {
        for (const Ot::Op &o : ops) {
            if (o.pos + o.del > _captured.length) break;
            [_captured replaceCharactersInRange:NSMakeRange(o.pos, o.del) withString:NSFromU16(o.ins)];
        }
        return;
    }
    if (!_open || (ops.empty() && !select)) return;
    CodeTextView *view = [self mirrorView];
    if (view && (StorageBusy(view) || !_deferred.empty() || _applying)) {
        _deferred.insert(_deferred.end(), ops.begin(), ops.end());
        if (select) {
            _deferredSelect = *select;
            _deferredHasSelect = YES;
        }
        [self scheduleFlush];
        return;
    }
    [self apply:ops flag:flag select:select view:view];
}

- (void)scheduleFlush {
    if (_flushQueued) return;
    _flushQueued = YES;
    __weak MCSessionDoc *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        MCSessionDoc *d = weakSelf;
        if (!d) return;
        d->_flushQueued = NO;
        [d flushDeferred];
    });
}

- (void)flushDeferred {
    if (_deferred.empty() && !_deferredHasSelect) return;
    CodeTextView *view = [self mirrorView];
    if (view && StorageBusy(view)) {
        [self scheduleFlush];
        return;
    }
    Ot::Ops ops = std::move(_deferred);
    _deferred.clear();
    Shared::Selection sel = _deferredSelect;
    const BOOL hasSelect = _deferredHasSelect;
    _deferredHasSelect = NO;
    os_log(SessionLog(), "%lu of the host's edits put in a turn late: the storage was editing",
           (unsigned long)ops.size());
    [self apply:ops flag:0 select:hasSelect ? &sel : nullptr view:view];
}

- (void)apply:(const Ot::Ops &)ops
         flag:(int)flag
       select:(const Shared::Selection *)select
         view:(CodeTextView *)view {
    MCTab *tab = _tab;
    if (!tab) return;
    _applying = YES;
    // The host's edit is a step of the host's already: nothing of it goes
    // on NSUndoManager's stacks (NSTextView's or replaceRanges:'s).
    NSUndoManager *um = tab.undoManager;
    const BOOL registering = um.isUndoRegistrationEnabled;
    if (registering) [um disableUndoRegistration];
    if (view) {
        const BOOL editable = view.editable;
        view.editable = YES;   // the mirror follows the host even while read-only
        view.sessionUndoing = flag != 0;
        // An undo's plan comes as replacements apart in the text: one
        // multi-range edit, as replaceRanges: makes them. Any other shape
        // goes in an operation at a time.
        std::vector<Ot::Replacement> reps;
        BOOL done = NO;
        if (!ops.empty() && Ot::asReplacements(ops, Model::Unit::Utf16, &reps)) {
            NSMutableArray<NSValue *> *ranges = [NSMutableArray arrayWithCapacity:reps.size()];
            NSMutableArray<NSString *> *strings = [NSMutableArray arrayWithCapacity:reps.size()];
            for (const Ot::Replacement &r : reps) {
                [ranges addObject:[NSValue valueWithRange:NSMakeRange(r.start, r.length)]];
                [strings addObject:NSFromU16(r.text)];
            }
            done = reps.empty() || [view replaceRanges:ranges withStrings:strings actionName:nil];
        } else if (ops.empty()) {
            done = YES;
        }
        if (!done)
            for (const Ot::Op &o : ops)
                if (![view replaceRanges:@[ [NSValue valueWithRange:NSMakeRange(o.pos, o.del)] ]
                             withStrings:@[ NSFromU16(o.ins) ]
                              actionName:nil])
                    os_log_error(SessionLog(), "the host's edit at %lu did not go into the editor",
                                 (unsigned long)o.pos);
        if (select && !select->empty()) {
            // Every piece the step started with selected again (as
            // replaceRanges:'s own undo did): the selected ones, else the
            // first caret.
            const NSUInteger n = view.textStorage.length;
            NSMutableArray<NSValue *> *sel = [NSMutableArray arrayWithCapacity:select->size()];
            NSRange first = NSMakeRange(NSNotFound, 0);
            for (const Model::UndoHistory::Range &r : *select) {
                const NSUInteger a = MIN(MIN(r.anchor, r.caret), n), b = MIN(MAX(r.anchor, r.caret), n);
                if (first.location == NSNotFound) first = NSMakeRange(a, b - a);
                if (b > a) [sel addObject:[NSValue valueWithRange:NSMakeRange(a, b - a)]];
            }
            if (!sel.count) [sel addObject:[NSValue valueWithRange:first]];
            [view setSelectedRanges:sel affinity:NSSelectionAffinityDownstream stillSelecting:NO];
            [view scrollRangeToVisible:sel.firstObject.rangeValue];
        }
        view.sessionUndoing = NO;
        view.editable = editable;
    } else {
        NSMutableString *text = [(tab.currentText ?: @"") mutableCopy];
        for (const Ot::Op &o : ops) {
            if (o.pos + o.del > text.length) break;
            [text replaceCharactersInRange:NSMakeRange(o.pos, o.del) withString:NSFromU16(o.ins)];
        }
        tab.sourceText = text;
        if (select && !select->empty()) {
            const Model::UndoHistory::Range &r = (*select)[0];
            const NSUInteger a = MIN(MIN(r.anchor, r.caret), text.length),
                             b = MIN(MAX(r.anchor, r.caret), text.length);
            tab.selection = NSMakeRange(a, b - a);
        }
        id owner = tab.owner;
        if ([owner respondsToSelector:@selector(tabTextChangedByHost:)]) [owner tabTextChangedByHost:tab];
    }
    if (registering) [um enableUndoRegistration];
    _applying = NO;
}

// ---- undo

- (BOOL)canUndo:(BOOL)redo name:(NSString **)name {
    if (!_open) return NO;
    [self closeStep];
    const Shared::UndoInfo u = [self client].undoInfo(_docId);
    const BOOL can = redo ? u.canRedo : u.canUndo;
    if (name) *name = can ? @((redo ? u.redoName : u.undoName).c_str()) : nil;
    return can;
}

- (BOOL)undo:(BOOL)redo inView:(CodeTextView *)view {
    (void)view;
    if (!_open || _applying || _undoing) return NO;
    [self closeStep];
    [self flushDeferred];
    _undoing = redo ? 2 : 1;
    const Shared::Outcome o = [self client].undo(_docId, redo);
    _undoing = 0;
    if (o != Shared::Outcome::Nothing) _undoCount++;
    if (o == Shared::Outcome::Refused) {
        os_log_error(SessionLog(),
                     "%{public}s in %{public}@ refused: the text it would change is not what it left there, "
                     "so the history was forgotten",
                     redo ? "redo" : "undo", _tab.currentPath ?: @"a new file");
        NSBeep();
    }
    return o == Shared::Outcome::Done;
}

// ---- the host's text

- (const Model::Text *)hostTextFor:(const void *)owner {
    if (!_open) return nullptr;
    // The step stays open: what it has recorded goes ahead of the settle's
    // ping (the outbox is sent in order), and closing it here, inside a
    // storage notification as completion asks, would split typing.
    Shared::Client &c = [self client];
    const Model::Text *copy = c.watch(_docId, owner, [](Shared::DocId, const Ot::Ops &) {}, 2000);
    if (!copy || !c.settle(2000)) return nullptr;
    return c.hostCopy(_docId);
}

- (void)releaseHostTextFor:(const void *)owner {
    if (_open && !_link->lost()) [self client].unwatch(_docId, owner);
}

@end

// ---------------------------------------------------------------- checks

NSString *MCSessionCheckTabs(NSArray<MCTab *> *tabs) {
    NSMutableString *out = [NSMutableString string];
    // Each tab against its own link's host; a lost one has nothing to
    // compare with.
    std::set<MCSession::Link *> settled;
    for (MCTab *t in tabs) {
        MCSessionDoc *d = t.session;
        if (!d || !d.open) continue;
        std::shared_ptr<MCSession::Link> link = d.link.link;
        Shared::Client &c = link->client();
        if (!settled.count(link.get())) {
            settled.insert(link.get());
            if (!c.settle()) {
                [out appendString:@"the session's host did not answer; "];
                continue;
            }
        }
        if (d.deferredEdits) {
            [out appendFormat:@"%@ has %lu of the host's edits not put in; ", t.currentPath.lastPathComponent,
                              (unsigned long)d.deferredEdits];
            continue;
        }
        const Shared::Client::HostDoc h = c.hostDocument(d.docId);
        NSString *mine = t.currentText ?: @"";
        if (!h.open) {
            [out appendFormat:@"the host has no document for %@; ", t.currentPath.lastPathComponent ?: @"a tab"];
            continue;
        }
        if (h.text != U16(mine)) {
            // Where they part, for the log.
            const std::u16string m = U16(mine);
            size_t i = 0;
            while (i < m.size() && i < h.text.size() && m[i] == h.text[i]) i++;
            [out appendFormat:@"the host's text of %@ differs from the tab's (%lu units against %lu, "
                              @"first at %lu); ",
                              t.currentPath.lastPathComponent ?: @"a tab", (unsigned long)h.text.size(),
                              (unsigned long)m.size(), (unsigned long)i];
        }
    }
    for (const std::shared_ptr<MCSession::Link> &l : MCSession::all())
        if (!l->lost())
            for (const std::string &e : l->client().takeErrors()) [out appendFormat:@"the host said: %s; ", e.c_str()];
    return out;
}

MCSessionTerminalState MCSessionInspectTerminal(unsigned long long term) {
    MCSessionTerminalState out;
    if (!gHost) return out;
    gHost->inspect([&](const Shared::Host &h) {
        out.fd = h.ptyFd(term);
        out.pid = h.shellPid(term);
        std::vector<Shared::Host::PollFd> fds;
        h.pollSet(&fds);
        for (const auto &p : fds)
            if (p.term == term) {
                out.writeWaiting = p.write;
                out.reading = p.read;
            }
        out.dying = h.dyingCount();
        if (const TerminalScreen *s = h.screen(term)) {
            int last = s->rows() - 1;
            while (last >= 0 && s->lineText(last).empty()) last--;
            for (int r = -s->historySize(); r <= last; r++) out.screen += s->lineText(r) + "\n";
        }
    });
    return out;
}
