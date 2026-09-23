// Lsp.cpp — see Lsp.h. Server processes (lspgtk::Server), and LspSession: the
// diagnostics tags, the completion popover, hover and go to definition. The
// macOS counterpart is src/Lsp.mm, and the behaviour follows it closely.
#include "Lsp.h"
#include "Palette.h"
#include "Utf8Offsets.h"

#include <glib-unix.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

// ------------------------------------------------------------------ helpers
namespace {

std::string realPath(const std::string& p) {
    if (p.empty()) return p;
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}

bool isIdent(gunichar c) {
    return c == '_' || c == '$' || g_unichar_isalnum(c);
}

std::string lowerExt(const std::string& path) {
    auto slash = path.find_last_of('/');
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string e = path.substr(dot + 1);
    for (char& c : e) c = (char)g_ascii_tolower(c);
    return e;
}

// The places language servers get installed, after PATH. A launcher started
// from the dock may have a short PATH, and cargo, go and pip put servers in
// the home folder.
std::vector<std::string> searchDirs() {
    std::vector<std::string> dirs;
    auto add = [&](const std::string& d) {
        if (!d.empty() && std::find(dirs.begin(), dirs.end(), d) == dirs.end())
            dirs.push_back(d);
    };
    const char* path = g_getenv("PATH");
    std::stringstream ss(path ? path : "");
    std::string d;
    while (std::getline(ss, d, ':')) add(d);
    const std::string home = g_get_home_dir();
    add(home + "/.cargo/bin");
    add(home + "/go/bin");
    add(home + "/.local/bin");
    add("/usr/local/bin");
    add("/usr/bin");
    return dirs;
}

std::string findProgram(const std::string& name) {
    auto runnable = [](const std::string& p) {
        return g_file_test(p.c_str(), G_FILE_TEST_IS_EXECUTABLE) &&
               !g_file_test(p.c_str(), G_FILE_TEST_IS_DIR);
    };
    if (name.find('/') != std::string::npos) {
        std::string p = name;
        if (p.rfind("~/", 0) == 0) p = std::string(g_get_home_dir()) + p.substr(1);
        return runnable(p) ? p : "";
    }
    for (const std::string& dir : searchDirs()) {
        std::string p = dir + "/" + name;
        if (runnable(p)) return p;
    }
    return "";
}

// A command line from the settings (or the defaults) -> the program's full
// path plus its arguments, or nothing when the program is not installed.
std::vector<std::string> resolveCommand(const std::string& command) {
    std::vector<std::string> words = Lsp::splitCommand(command);
    if (words.empty()) return {};
    std::string prog = findProgram(words[0]);
    if (prog.empty()) return {};
    words[0] = prog;
    return words;
}

// A server that dies mid-write must not take the editor with it. A handler
// that does nothing (rather than SIG_IGN) makes write() fail with EPIPE, and
// unlike SIG_IGN it is reset to the default in every program the app starts,
// so the terminal's shell still gets ordinary SIGPIPE behaviour.
void ignoreSigpipeHere() {
    static bool done = false;
    if (done) return;
    done = true;
    struct sigaction old {};
    sigaction(SIGPIPE, nullptr, &old);
    if (old.sa_handler != SIG_DFL) return;   // someone already decided
    struct sigaction sa {};
    sa.sa_handler = [](int) {};
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGPIPE, &sa, nullptr);
}

// In the server, between fork and exec: if MiniCode dies without shutting
// it down (a crash, SIGKILL), the server gets SIGTERM. EOF on its stdin is
// the other backstop.
void childSetup(gpointer) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
}

// The UTF-8 text of line `line` of `text`, lines ending at \n, \r\n or \r
// (both LSP's rule and GtkTextBuffer's). False past the last line.
bool lineOf(const std::string& text, int line, std::string& out) {
    size_t pos = 0;
    for (int l = 0; l < line; ++l) {
        size_t nl = text.find_first_of("\r\n", pos);
        if (nl == std::string::npos) return false;
        pos = nl + ((text[nl] == '\r' && nl + 1 < text.size() && text[nl + 1] == '\n') ? 2 : 1);
    }
    size_t end = text.find_first_of("\r\n", pos);
    out = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    return true;
}

// A UTF-16 column in a line -> its byte offset in the line's UTF-8.
size_t byteColumn(const std::string& lineText, int utf16Col) {
    std::u16string u = utf16::fromUtf8(lineText);
    long chars = utf16::toCharOffset(u, (size_t)std::max(0, utf16Col));
    const char* p = g_utf8_offset_to_pointer(lineText.c_str(), chars);
    return (size_t)(p - lineText.c_str());
}

void installCss() {
    static bool done = false;
    if (done || !gdk_display_get_default()) return;
    done = true;
    std::string css =
        "popover.minicode-completion > contents, popover.minicode-hover > contents {"
        "  background-color: " + std::string(pal::SidebarBg) + ";"
        "  color: " + pal::EditorText + ";"
        "  border: 1px solid #454545; border-radius: 4px; padding: 0; }"
        "popover.minicode-completion list { background: transparent; }"
        "popover.minicode-completion row { padding: 1px 8px; min-height: 0;"
        "  font-family: monospace; }"
        "popover.minicode-completion row:selected { background-color: #04395E; }"
        "popover.minicode-hover label { font-family: monospace; padding: 8px 10px; }";
    GtkCssProvider* p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, css.c_str());
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(p),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

}  // namespace

// ------------------------------------------------------------- the server
namespace lspgtk {

class Server;
// Every server the app started and has not seen exit. Never freed: it must
// outlive anything that runs at exit.
static std::vector<std::shared_ptr<Server>>* gServers =
    new std::vector<std::shared_ptr<Server>>();

// One language server process. Everything runs on the main thread: output is
// read from a non-blocking pipe when the main loop says it is readable, and
// writes that do not fit in the pipe wait for it to drain, so a server that
// is slow to read never blocks typing.
class Server : public std::enable_shared_from_this<Server> {
public:
    static std::shared_ptr<Server> start(const std::string& key,
                                         const std::vector<std::string>& argv,
                                         const std::string& root);
    ~Server();

    Lsp::Client& client() { return *client_; }
    const std::string& key() const { return key_; }
    const std::string& name() const { return name_; }
    bool running() const { return running_; }
    bool stopping() const { return stopping_; }
    int pid() const { return pid_; }
    std::function<void(Server*)> onExit;

    void stop();       // shutdown, exit, then signals if it lingers
    void stopNow();    // for quitting: exit at once, written before returning
    void signal(int sig) { if (running_ && proc_) g_subprocess_send_signal(proc_, sig); }

private:
    Server() = default;
    void send(const std::string& bytes);
    void flushWrites();
    void closeInput();   // EOF on the server's stdin, after what is queued
    void doClose();
    bool readAvailable();   // true at EOF
    void ended();
    void log(const char* prefix, const char* data, size_t n);
    static gboolean onReadable(int fd, GIOCondition cond, gpointer self);
    static gboolean onWritable(int fd, GIOCondition cond, gpointer self);
    static void onWaited(GObject* src, GAsyncResult* res, gpointer data);

    std::string key_, name_;
    GSubprocess* proc_ = nullptr;
    int pid_ = 0;
    int inFd_ = -1, outFd_ = -1;   // our ends of the server's stdin and stdout
    guint readId_ = 0, writeId_ = 0;
    std::string out_;              // bytes waiting for room in the pipe
    bool closeWanted_ = false, inputClosed_ = false;
    bool running_ = false, stopping_ = false;
    int logFd_ = -1;
    std::unique_ptr<Lsp::Client> client_;
};

std::shared_ptr<Server> Server::start(const std::string& key,
                                      const std::vector<std::string>& argv,
                                      const std::string& root) {
    std::shared_ptr<Server> s(new Server());
    s->key_ = key;
    char* base = g_path_get_basename(argv[0].c_str());
    s->name_ = base;
    g_free(base);

    int toServer[2], fromServer[2];
    if (pipe2(toServer, O_CLOEXEC) != 0) return nullptr;
    if (pipe2(fromServer, O_CLOEXEC) != 0) {
        close(toServer[0]);
        close(toServer[1]);
        return nullptr;
    }

    // stderr must go somewhere that never fills up: clangd logs every request
    // there, and a full pipe would stall it mid-reply. MINICODE_LSP_LOG gets
    // it instead, along with all the traffic, opened O_APPEND so several
    // servers can share the file.
    int errFd = -1;
    const char* logPath = g_getenv("MINICODE_LSP_LOG");
    if (logPath && *logPath) {
        errFd = open(logPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        s->logFd_ = open(logPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }
    GSubprocessLauncher* l = g_subprocess_launcher_new(
        errFd >= 0 ? G_SUBPROCESS_FLAGS_NONE : G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    g_subprocess_launcher_take_stdin_fd(l, toServer[0]);
    g_subprocess_launcher_take_stdout_fd(l, fromServer[1]);
    if (errFd >= 0) g_subprocess_launcher_take_stderr_fd(l, errFd);
    if (!root.empty() && g_file_test(root.c_str(), G_FILE_TEST_IS_DIR))
        g_subprocess_launcher_set_cwd(l, root.c_str());
    // Servers start helpers of their own (node, go, cargo), which need the
    // same wider PATH the server was found on.
    std::string path;
    for (const std::string& d : searchDirs()) path += (path.empty() ? "" : ":") + d;
    g_subprocess_launcher_setenv(l, "PATH", path.c_str(), TRUE);
    g_subprocess_launcher_set_child_setup(l, childSetup, nullptr, nullptr);

    std::vector<const char*> av;
    for (const std::string& a : argv) av.push_back(a.c_str());
    av.push_back(nullptr);
    GError* err = nullptr;
    s->proc_ = g_subprocess_launcher_spawnv(l, av.data(), &err);
    g_object_unref(l);   // closes the child's ends of the pipes
    if (!s->proc_) {
        g_printerr("MiniCode: could not start %s: %s\n", argv[0].c_str(),
                   err ? err->message : "?");
        g_clear_error(&err);
        close(toServer[1]);
        close(fromServer[0]);
        return nullptr;
    }
    ignoreSigpipeHere();
    const char* id = g_subprocess_get_identifier(s->proc_);
    s->pid_ = id ? atoi(id) : 0;
    s->inFd_ = toServer[1];
    s->outFd_ = fromServer[0];
    g_unix_set_fd_nonblocking(s->inFd_, TRUE, nullptr);
    g_unix_set_fd_nonblocking(s->outFd_, TRUE, nullptr);
    s->running_ = true;
    s->readId_ = g_unix_fd_add(s->outFd_, (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR),
                               onReadable, s.get());
    g_subprocess_wait_async(s->proc_, nullptr, onWaited, new std::shared_ptr<Server>(s));
    Server* raw = s.get();
    s->client_ = std::make_unique<Lsp::Client>([raw](const std::string& bytes) {
        raw->send(bytes);
    });
    gServers->push_back(s);
    return s;
}

Server::~Server() {
    if (readId_) g_source_remove(readId_);
    if (writeId_) g_source_remove(writeId_);
    if (inFd_ >= 0) close(inFd_);
    if (outFd_ >= 0) close(outFd_);
    if (logFd_ >= 0) close(logFd_);
    if (proc_) g_object_unref(proc_);
}

void Server::log(const char* prefix, const char* data, size_t n) {
    if (logFd_ < 0) return;
    std::string line = std::string(prefix) + std::string(data, n) + "\n";
    ssize_t w = write(logFd_, line.data(), line.size());
    (void)w;
}

void Server::send(const std::string& bytes) {
    if (closeWanted_ || inputClosed_) return;
    log("--> ", bytes.data(), bytes.size());
    out_ += bytes;
    flushWrites();
}

void Server::flushWrites() {
    while (!out_.empty() && !inputClosed_) {
        ssize_t n = write(inFd_, out_.data(), out_.size());
        if (n > 0) {
            out_.erase(0, (size_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!writeId_) writeId_ = g_unix_fd_add(inFd_, G_IO_OUT, onWritable, this);
            return;
        }
        out_.clear();   // EPIPE: the server is gone; its exit is handled elsewhere
    }
    if (writeId_) {
        g_source_remove(writeId_);
        writeId_ = 0;
    }
    if (closeWanted_ && !inputClosed_) doClose();
}

gboolean Server::onWritable(int, GIOCondition, gpointer selfp) {
    Server* s = static_cast<Server*>(selfp);
    s->writeId_ = 0;   // flushWrites adds a new watch if it is still needed
    s->flushWrites();
    return G_SOURCE_REMOVE;
}

void Server::closeInput() {
    closeWanted_ = true;
    if (out_.empty() && !inputClosed_) doClose();
}

void Server::doClose() {
    inputClosed_ = true;
    out_.clear();
    if (writeId_) {
        g_source_remove(writeId_);
        writeId_ = 0;
    }
    if (inFd_ >= 0) close(inFd_);
    inFd_ = -1;
}

bool Server::readAvailable() {
    if (outFd_ < 0) return true;
    char buf[65536];
    for (;;) {
        ssize_t n = read(outFd_, buf, sizeof buf);
        if (n > 0) {
            log("<-- ", buf, (size_t)n);
            client_->receive(buf, (size_t)n);
            continue;
        }
        if (n == 0) return true;
        if (errno == EINTR) continue;
        return !(errno == EAGAIN || errno == EWOULDBLOCK);
    }
}

gboolean Server::onReadable(int, GIOCondition, gpointer selfp) {
    Server* s = static_cast<Server*>(selfp);
    std::shared_ptr<Server> keep = s->shared_from_this();
    if (s->readAvailable()) {
        s->readId_ = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void Server::onWaited(GObject* src, GAsyncResult* res, gpointer data) {
    auto* holder = static_cast<std::shared_ptr<Server>*>(data);
    std::shared_ptr<Server> s = *holder;
    delete holder;
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, nullptr);
    s->ended();
}

void Server::ended() {
    if (!running_) return;
    std::shared_ptr<Server> keep = shared_from_this();
    if (readId_) readAvailable();   // whatever it wrote before going
    running_ = false;
    if (readId_) {
        g_source_remove(readId_);
        readId_ = 0;
    }
    doClose();
    auto& all = *gServers;
    all.erase(std::remove(all.begin(), all.end(), keep), all.end());
    if (onExit) {
        auto cb = onExit;
        cb(this);
    }
}

void Server::stop() {
    if (!running_ || stopping_) return;
    stopping_ = true;
    std::weak_ptr<Server> weak = shared_from_this();
    client_->shutdown([weak] {
        if (auto s = weak.lock()) s->closeInput();
    });
    // A server that does not answer shutdown within two seconds is told to
    // go, then made to.
    auto release = [](gpointer d) { delete static_cast<std::shared_ptr<Server>*>(d); };
    g_timeout_add_full(G_PRIORITY_DEFAULT, 2000, [](gpointer d) -> gboolean {
        std::shared_ptr<Server> s = *static_cast<std::shared_ptr<Server>*>(d);
        if (!s->running_) return G_SOURCE_REMOVE;
        s->closeInput();
        g_subprocess_send_signal(s->proc_, SIGTERM);
        g_timeout_add_full(G_PRIORITY_DEFAULT, 1000, [](gpointer d2) -> gboolean {
            auto& s2 = *static_cast<std::shared_ptr<Server>*>(d2);
            if (s2->running_) g_subprocess_force_exit(s2->proc_);
            return G_SOURCE_REMOVE;
        }, new std::shared_ptr<Server>(s),
           [](gpointer d2) { delete static_cast<std::shared_ptr<Server>*>(d2); });
        return G_SOURCE_REMOVE;
    }, new std::shared_ptr<Server>(shared_from_this()), release);
}

void Server::stopNow() {
    if (!running_) return;
    stopping_ = true;
    client_->exitNow();
    closeWanted_ = true;
    // Written out before returning, since the main loop will not run again.
    const gint64 deadline = g_get_monotonic_time() + 200000;
    while (!out_.empty() && !inputClosed_ && g_get_monotonic_time() < deadline) {
        struct pollfd p = {inFd_, POLLOUT, 0};
        poll(&p, 1, 20);
        flushWrites();
    }
    if (!inputClosed_) doClose();
}

}  // namespace lspgtk

using lspgtk::Server;

// Wait up to `ms` for every listed server to exit, without the main loop
// (which has stopped by the time the app quits). A pidfd turns readable when
// its process exits, and waiting on one does not reap it, so GLib's own child
// watch is left alone. Returns true when all have exited.
static bool waitForExit(const std::vector<std::shared_ptr<Server>>& list, int ms) {
#ifdef SYS_pidfd_open
    std::vector<struct pollfd> fds;
    for (const auto& s : list) {
        if (!s->running() || s->pid() <= 0) continue;
        int fd = (int)syscall(SYS_pidfd_open, s->pid(), 0);
        if (fd >= 0) fds.push_back({fd, POLLIN, 0});
    }
    const gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;
    size_t pending = fds.size();
    while (pending > 0) {
        const gint64 left = (deadline - g_get_monotonic_time()) / 1000;
        if (left <= 0) break;
        if (poll(fds.data(), fds.size(), (int)left) < 0 && errno != EINTR) break;
        for (auto& p : fds)
            if (p.fd >= 0 && p.revents) {
                close(p.fd);
                p.fd = -1;   // poll ignores negative descriptors
                --pending;
            }
    }
    for (auto& p : fds) if (p.fd >= 0) close(p.fd);
    return pending == 0;
#else
    g_usleep((gulong)ms * 1000);
    return false;
#endif
}

void LspTerminateAllServers() {
    std::vector<std::shared_ptr<Server>> list = *lspgtk::gServers;
    if (list.empty()) return;
    for (auto& s : list) s->stopNow();
    // Half a second for a clean exit, then signals.
    if (waitForExit(list, 500)) return;
    for (auto& s : list) s->signal(SIGTERM);
    if (waitForExit(list, 300)) return;
    for (auto& s : list) s->signal(SIGKILL);
    waitForExit(list, 300);
}

std::size_t LspRunningServerCount() {
    std::size_t n = 0;
    for (auto& s : *lspgtk::gServers) if (s->running()) ++n;
    return n;
}

std::vector<int> LspServerPids() {
    std::vector<int> out;
    for (auto& s : *lspgtk::gServers) if (s->running()) out.push_back(s->pid());
    return out;
}

// ------------------------------------------------------------ LspSession

LspSession::LspSession(Editor* editor, const std::string& root, const Settings& settings)
    : editor_(editor), view_(editor->textView()), buffer_(editor->buffer()),
      root_(root), settings_(settings), signature_(settingsSignature(settings)) {
    installCss();

    // Squiggles: tags of their own, never among the highlighter's (it removes
    // only its own tags on a retag), so re-highlighting leaves them alone.
    // Created from hint up to error, so an error's underline wins where they
    // overlap (a later tag has the higher priority).
    static const struct { const char* name; const char* color; } kinds[4] = {
        {"lsp-error", "#F14C4C"}, {"lsp-warning", "#CCA700"},
        {"lsp-information", "#3794FF"}, {"lsp-hint", "#9CA3AF"}};
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buffer_);
    for (int i = 3; i >= 0; --i) {
        GtkTextTag* t = gtk_text_tag_table_lookup(table, kinds[i].name);
        if (!t) {
            GdkRGBA c;
            gdk_rgba_parse(&c, kinds[i].color);
            t = gtk_text_buffer_create_tag(buffer_, kinds[i].name,
                                           "underline", PANGO_UNDERLINE_ERROR,
                                           "underline-rgba", &c, NULL);
        }
        tags_[i] = t;
    }

    // Keys for the completion list, ahead of the text view's own handling.
    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(onKey), this);
    gtk_widget_add_controller(view_, keys);

    // Ctrl+click: go to definition.
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(onCtrlClick), this);
    gtk_widget_add_controller(view_, GTK_EVENT_CONTROLLER(click));

    GtkEventController* focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(onFocusLeave), this);
    gtk_widget_add_controller(view_, focus);

    gtk_widget_set_has_tooltip(view_, TRUE);
    g_signal_connect(view_, "query-tooltip", G_CALLBACK(onQueryTooltip), this);
    // The popovers are the text view's children only by gtk_widget_set_parent,
    // which GtkTextView's own dispose does not know about: it tries to remove
    // them as its own children, fails, and tries again forever (the window
    // never closed). So they go when the view is unrealized, which comes
    // before dispose, and are built again on next use.
    g_signal_connect(view_, "unrealize", G_CALLBACK(onUnrealize), this);
    g_signal_connect(buffer_, "mark-set", G_CALLBACK(onMarkSet), this);
}

LspSession::~LspSession() {
    shutdown();
    g_signal_handlers_disconnect_by_data(view_, this);
    g_signal_handlers_disconnect_by_data(buffer_, this);
    if (refilterId_) g_source_remove(refilterId_);
    if (triggerId_) g_source_remove(triggerId_);
    if (flashId_) g_source_remove(flashId_);
    dropPopovers();
}

void LspSession::dropPopovers() {
    closeCompletion();
    if (popover_) gtk_widget_unparent(popover_);
    if (hoverPopover_) gtk_widget_unparent(hoverPopover_);
    popover_ = list_ = listScroll_ = nullptr;
    hoverPopover_ = hoverLabel_ = nullptr;
}

void LspSession::onUnrealize(GtkWidget*, gpointer selfp) {
    static_cast<LspSession*>(selfp)->dropPopovers();
}

// ---- status

void LspSession::updateStatus() {
    std::string s;
    if (!transient_.empty()) {
        s = transient_;
    } else if (server_) {
        if (server_->client().state() != Lsp::Client::State::Ready) {
            s = server_->name() + " starting…";
        } else {
            int errors = 0, warnings = 0;
            auto it = diags_.find(uri_);
            if (it != diags_.end())
                for (const Lsp::Diagnostic& d : it->second) {
                    if (d.severity == Lsp::Severity::Error) ++errors;
                    else if (d.severity == Lsp::Severity::Warning) ++warnings;
                }
            std::string parts;
            if (errors)
                parts = std::to_string(errors) + (errors == 1 ? " error" : " errors");
            if (warnings)
                parts += (parts.empty() ? "" : ", ") + std::to_string(warnings) +
                         (warnings == 1 ? " warning" : " warnings");
            s = server_->name() + ": " + (parts.empty() ? "no problems" : parts);
        }
    } else {
        s = note_;
    }
    status_ = s;
    if (onStatus) onStatus(s);
}

void LspSession::flash(const std::string& message) {
    transient_ = message;
    updateStatus();
    if (flashId_) g_source_remove(flashId_);
    flashId_ = g_timeout_add_seconds(3, [](gpointer p) -> gboolean {
        LspSession* self = static_cast<LspSession*>(p);
        self->flashId_ = 0;
        self->transient_.clear();
        self->updateStatus();
        return G_SOURCE_REMOVE;
    }, this);
}

bool LspSession::noServer() {
    if (server_ && !uri_.empty()) return false;
    gtk_widget_error_bell(view_);
    flash(note_.empty() ? "No language server for this file" : note_);
    return true;
}

// ---- servers

std::string LspSession::settingsSignature(const Settings& s) const {
    std::string sig = s.lspEnabled() ? "1" : "0";
    for (const std::string& k : Settings::lspServers()) sig += "|" + s.lspCommand(k);
    return sig;
}

std::shared_ptr<Server> LspSession::serverForKey(const std::string& key) {
    auto found = servers_.find(key);
    if (found != servers_.end()) return found->second;
    auto noted = notes_.find(key);
    if (noted != notes_.end()) {
        note_ = noted->second;
        return nullptr;
    }
    if (settings_.lspOff(key)) {
        notes_[key] = "";   // switched off on purpose: say nothing
        return nullptr;
    }
    std::vector<std::string> commands;
    if (!settings_.lspCommand(key).empty()) commands.push_back(settings_.lspCommand(key));
    else commands = Lsp::defaultCommands(key);
    std::shared_ptr<Server> server;
    for (const std::string& cmd : commands) {
        std::vector<std::string> argv = resolveCommand(cmd);
        if (argv.empty()) continue;
        server = Server::start(key, argv, root_);
        if (server) break;
    }
    if (!server) {
        note_ = "No " + Lsp::serverDisplayName(key) + " language server found";
        notes_[key] = note_;
        return nullptr;
    }
    std::weak_ptr<int> alive = life_;
    Lsp::Client& c = server->client();
    c.onReady = [alive, this] {
        if (!alive.expired()) updateStatus();
    };
    c.onDiagnostics = [alive, this](const std::string& uri,
                                    const std::vector<Lsp::Diagnostic>& list) {
        if (!alive.expired()) diagnosticsArrived(uri, list);
    };
    std::string name = server->name();
    c.onProtocolError = [name](const std::string& problem) {
        g_printerr("MiniCode: %s: %s\n", name.c_str(), problem.c_str());
    };
    server->onExit = [alive, this](Server* s) {
        if (!alive.expired()) serverExited(s);
    };
    c.initialize(root_, (int)getpid());
    servers_[key] = server;
    return server;
}

void LspSession::serverExited(Server* s) {
    std::shared_ptr<Server> keep;
    auto it = servers_.find(s->key());
    if (it != servers_.end() && it->second.get() == s) {
        keep = it->second;
        servers_.erase(it);
        if (!s->stopping()) notes_[s->key()] = s->name() + " stopped";
    }
    if (server_.get() == s) {
        server_.reset();
        uri_.clear();
        note_ = s->stopping() ? "" : notes_[s->key()];
        clearMarks();
        closeCompletion();
        updateStatus();
    }
}

void LspSession::stopAllServers() {
    for (auto& kv : servers_) kv.second->stop();
    servers_.clear();
    notes_.clear();
    diags_.clear();
}

void LspSession::applySettings(const Settings& settings) {
    settings_ = settings;
    std::string sig = settingsSignature(settings);
    if (sig == signature_) return;
    signature_ = sig;
    std::string path = path_;
    closeDocument();
    stopAllServers();
    documentChanged(path);
}

void LspSession::setRoot(const std::string& root) {
    if (root == root_) return;
    std::string path = path_;
    closeDocument();
    stopAllServers();
    root_ = root;
    documentChanged(path);   // the open file gets a server rooted in the new folder
}

void LspSession::shutdown() {
    closeDocument();
    stopAllServers();
}

// ---- documents

void LspSession::closeDocument() {
    if (changeTimer_) g_source_remove(changeTimer_);
    changeTimer_ = 0;
    if (triggerId_) g_source_remove(triggerId_);
    triggerId_ = 0;
    closeCompletion();
    if (hoverPopover_) gtk_popover_popdown(GTK_POPOVER(hoverPopover_));
    if (server_ && !uri_.empty()) {
        server_->client().didClose(uri_);
        diags_.erase(uri_);
    }
    server_.reset();
    uri_.clear();
    path_.clear();
    note_.clear();
    clearMarks();
    ++docGen_;
    hoverKey_ = hoverAsked_ = -1;
    hoverText_.clear();
}

void LspSession::documentChanged(const std::string& path) {
    closeDocument();
    path_ = path;
    Lsp::Language lang;
    if (path.empty() || !settings_.lspEnabled() ||
        !Lsp::languageForExtension(lowerExt(path), lang)) {
        updateStatus();
        return;
    }
    std::shared_ptr<Server> server = serverForKey(lang.server);
    if (!server) {
        updateStatus();
        return;
    }
    server_ = server;
    uri_ = Lsp::uriFromPath(realPath(path));
    server->client().didOpen(uri_, lang.languageId, editor_->text());
    updateStatus();
}

void LspSession::documentSaved() {
    if (!server_ || uri_.empty()) return;
    if (changeTimer_) g_source_remove(changeTimer_);
    changeTimer_ = 0;
    server_->client().didSave(uri_, editor_->text());
}

// Full-document sync, a moment after typing pauses. Anything that asks the
// server about the text flushes first. Client::didChange drops a text that
// has not changed, so flushing twice costs nothing.
void LspSession::flushChanges() {
    if (changeTimer_) g_source_remove(changeTimer_);
    changeTimer_ = 0;
    if (server_ && !uri_.empty()) server_->client().didChange(uri_, editor_->text());
}

void LspSession::scheduleChange() {
    if (changeTimer_) g_source_remove(changeTimer_);
    changeTimer_ = g_timeout_add(300, [](gpointer p) -> gboolean {
        LspSession* self = static_cast<LspSession*>(p);
        self->changeTimer_ = 0;
        self->flushChanges();
        return G_SOURCE_REMOVE;
    }, this);
}

void LspSession::textEdited(const char* inserted, int len) {
    ++docGen_;
    hoverKey_ = hoverAsked_ = -1;
    hoverText_.clear();
    if (!server_ || uri_.empty()) return;
    scheduleChange();

    // One character typed: is it the end of ".", "->" or "::"? The caret is
    // already past it (the insert mark moves with text typed at it).
    if (inserted && len == 1) {
        GtkTextIter cur, prev;
        cursorIter(&cur);
        prev = cur;
        const char c = inserted[0];
        const bool atCaret = gtk_text_iter_backward_char(&prev) &&
                             gtk_text_iter_get_char(&prev) == (gunichar)(unsigned char)c;
        gunichar before = 0;
        if (atCaret && gtk_text_iter_backward_char(&prev)) before = gtk_text_iter_get_char(&prev);
        std::string trigger;
        if (atCaret) {
            if (c == '.') trigger = ".";
            else if (c == '>' && before == '-') trigger = ">";
            else if (c == ':' && before == ':') trigger = ":";
        }
        if (!trigger.empty()) {
            std::vector<std::string> triggers = server_->client().completionTriggers();
            if (triggers.empty() ||
                std::find(triggers.begin(), triggers.end(), trigger) != triggers.end()) {
                // Once the text view has finished with the key press.
                pendingTrigger_ = trigger;
                if (!triggerId_)
                    triggerId_ = g_idle_add([](gpointer p) -> gboolean {
                        LspSession* self = static_cast<LspSession*>(p);
                        self->triggerId_ = 0;
                        self->requestCompletion(self->pendingTrigger_);
                        return G_SOURCE_REMOVE;
                    }, this);
                return;
            }
        }
    }
    if (completing_) scheduleRefilter();
}

// ---- positions
// LSP counts UTF-16 units within a line, GTK counts characters. Lines are
// the same in both (\n, \r\n and \r end one), except for U+2029, which GTK
// also breaks a line at and LSP does not; source files do not use it.

Lsp::Position LspSession::positionOf(const GtkTextIter* it) const {
    GtkTextIter ls = *it;
    gtk_text_iter_set_line_offset(&ls, 0);
    char* s = gtk_text_buffer_get_slice(buffer_, &ls, it, TRUE);
    Lsp::Position p;
    p.line = gtk_text_iter_get_line(it);
    p.character = (int)utf16::fromUtf8(s ? s : "").size();
    g_free(s);
    return p;
}

void LspSession::iterAt(Lsp::Position p, GtkTextIter* it) const {
    if (p.line >= gtk_text_buffer_get_line_count(buffer_)) {
        gtk_text_buffer_get_end_iter(buffer_, it);
        return;
    }
    gtk_text_buffer_get_iter_at_line(buffer_, it, std::max(0, p.line));
    GtkTextIter le = *it;
    if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
    char* s = gtk_text_buffer_get_slice(buffer_, it, &le, TRUE);
    std::u16string u = utf16::fromUtf8(s ? s : "");
    g_free(s);
    gtk_text_iter_forward_chars(
        it, (int)utf16::toCharOffset(u, (size_t)std::max(0, p.character)));
}

void LspSession::cursorIter(GtkTextIter* it) const {
    gtk_text_buffer_get_iter_at_mark(buffer_, it, gtk_text_buffer_get_insert(buffer_));
}

// ---- diagnostics

bool LspSession::sameFile(const std::string& a, const std::string& b) const {
    if (b.empty()) return false;
    if (a == b) return true;
    std::string pa = Lsp::pathFromUri(a), pb = Lsp::pathFromUri(b);
    return !pa.empty() && realPath(pa) == realPath(pb);
}

void LspSession::clearMarks() {
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    for (GtkTextTag* t : tags_) gtk_text_buffer_remove_tag(buffer_, t, &a, &b);
    for (Mark& m : marks_) {
        gtk_text_buffer_delete_mark(buffer_, m.start);
        gtk_text_buffer_delete_mark(buffer_, m.end);
    }
    marks_.clear();
}

void LspSession::diagnosticsArrived(const std::string& uri,
                                    const std::vector<Lsp::Diagnostic>& list) {
    if (!sameFile(uri, uri_)) {
        diags_[uri] = list;   // another file's
        return;
    }
    diags_[uri_] = list;
    clearMarks();
    for (const Lsp::Diagnostic& d : list) {
        GtkTextIter a, b;
        iterAt(d.range.start, &a);
        iterAt(d.range.end, &b);
        if (gtk_text_iter_compare(&b, &a) <= 0) {
            // An empty range: mark the word there, or at least one character.
            b = a;
            while (!gtk_text_iter_is_end(&b) && isIdent(gtk_text_iter_get_char(&b)))
                gtk_text_iter_forward_char(&b);
            if (gtk_text_iter_equal(&a, &b)) {
                if (!gtk_text_iter_ends_line(&a)) gtk_text_iter_forward_char(&b);
                else if (!gtk_text_iter_starts_line(&a)) gtk_text_iter_backward_char(&a);
            }
        }
        const int sev = std::min(std::max((int)d.severity, 1), 4);
        gtk_text_buffer_apply_tag(buffer_, tags_[sev - 1], &a, &b);
        Mark m;
        // The range stretches when text is typed at either end, the way the
        // Mac's marks do, until the server sends fresh ones.
        m.start = gtk_text_buffer_create_mark(buffer_, nullptr, &a, TRUE);
        m.end = gtk_text_buffer_create_mark(buffer_, nullptr, &b, FALSE);
        m.severity = sev;
        const char* kind = sev == 1 ? "Error" : sev == 2 ? "Warning"
                         : sev == 3 ? "Info" : "Hint";
        m.message = std::string(kind) + ": " + d.message;
        marks_.push_back(m);
    }
    updateStatus();
}

std::string LspSession::diagnosticAt(const GtkTextIter* it) const {
    std::string out;
    for (const Mark& m : marks_) {
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_mark(buffer_, &a, m.start);
        gtk_text_buffer_get_iter_at_mark(buffer_, &b, m.end);
        const bool inside = gtk_text_iter_equal(&a, &b)
            ? gtk_text_iter_equal(it, &a)
            : gtk_text_iter_compare(it, &a) >= 0 && gtk_text_iter_compare(it, &b) < 0;
        if (inside) out += (out.empty() ? "" : "\n") + m.message;
    }
    return out;
}

// ---- hover

std::string LspSession::tooltipAt(const GtkTextIter* itp, bool* requested) {
    if (requested) *requested = false;
    std::string text = diagnosticAt(itp);
    if (server_ && !uri_.empty() && isIdent(gtk_text_iter_get_char(itp))) {
        GtkTextIter ws = *itp;
        for (GtkTextIter p = ws; gtk_text_iter_backward_char(&p) && isIdent(gtk_text_iter_get_char(&p));)
            ws = p;
        const long key = gtk_text_iter_get_offset(&ws);
        if (key == hoverKey_) {
            if (!hoverText_.empty()) text += (text.empty() ? "" : "\n\n") + hoverText_;
        } else if (key != hoverAsked_) {
            // Asked once per word; the tooltip is queried again on arrival.
            hoverAsked_ = key;
            flushChanges();
            std::weak_ptr<int> alive = life_;
            const int gen = docGen_;
            const std::string uri = uri_;
            server_->client().hover(uri, positionOf(itp),
                [alive, this, gen, uri, key](const Json& r, const Json&) {
                    if (alive.expired() || gen != docGen_ || uri != uri_ ||
                        key != hoverAsked_)
                        return;
                    hoverKey_ = key;
                    hoverText_ = Lsp::parseHover(r);
                    hoverAsked_ = -1;
                    gtk_widget_trigger_tooltip_query(view_);
                });
            if (requested) *requested = true;
        }
    }
    return text;
}

gboolean LspSession::onQueryTooltip(GtkWidget*, int x, int y, gboolean keyboard,
                                    GtkTooltip* tip, gpointer selfp) {
    LspSession* self = static_cast<LspSession*>(selfp);
    if (!self->editor_->isSource()) return FALSE;
    GtkTextIter it;
    if (keyboard) {
        self->cursorIter(&it);
    } else {
        int bx, by;
        gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(self->view_),
                                              GTK_TEXT_WINDOW_WIDGET, x, y, &bx, &by);
        if (!gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(self->view_), &it, bx, by))
            return FALSE;   // past the end of a line, or below the text
    }
    std::string text = self->tooltipAt(&it);
    if (text.empty()) return FALSE;
    char* esc = g_markup_escape_text(text.c_str(), -1);
    std::string markup = std::string("<span font_family=\"monospace\">") + esc + "</span>";
    g_free(esc);
    gtk_tooltip_set_markup(tip, markup.c_str());
    return TRUE;
}

void LspSession::showHoverInfo() {
    if (noServer()) return;
    flushChanges();
    GtkTextIter cur;
    cursorIter(&cur);
    const int gen = ++hoverGen_;
    const std::string uri = uri_;
    const long key = gtk_text_iter_get_offset(&cur);
    std::weak_ptr<int> alive = life_;
    server_->client().hover(uri, positionOf(&cur),
        [alive, this, gen, uri, key](const Json& r, const Json&) {
            if (!alive.expired()) hoverArrived(r, gen, uri, key, true);
        });
}

void LspSession::hoverArrived(const Json& result, int gen, const std::string& uri,
                              long key, bool popup) {
    if (gen != hoverGen_ || uri != uri_ || !popup) return;
    std::string text = Lsp::parseHover(result);
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_offset(buffer_, &at, (int)key);
    std::string diag = diagnosticAt(&at);
    if (!diag.empty()) text = text.empty() ? diag : diag + "\n\n" + text;
    lastHover_ = text;
    if (text.empty()) {
        flash("No information here");
        return;
    }
    showHoverPopover(text);
}

void LspSession::showHoverPopover(const std::string& text) {
    if (!hoverPopover_) {
        hoverPopover_ = gtk_popover_new();
        gtk_widget_add_css_class(hoverPopover_, "minicode-hover");
        gtk_popover_set_position(GTK_POPOVER(hoverPopover_), GTK_POS_BOTTOM);
        GtkWidget* scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
        gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll), TRUE);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 360);
        hoverLabel_ = gtk_label_new("");
        gtk_label_set_wrap(GTK_LABEL(hoverLabel_), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(hoverLabel_), 72);
        gtk_label_set_xalign(GTK_LABEL(hoverLabel_), 0.0);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), hoverLabel_);
        gtk_popover_set_child(GTK_POPOVER(hoverPopover_), scroll);
        gtk_widget_set_parent(hoverPopover_, view_);
    }
    gtk_label_set_text(GTK_LABEL(hoverLabel_), text.c_str());
    GtkTextIter cur;
    cursorIter(&cur);
    GdkRectangle r;
    caretRect(&cur, &r);
    gtk_popover_set_pointing_to(GTK_POPOVER(hoverPopover_), &r);
    gtk_popover_popup(GTK_POPOVER(hoverPopover_));
}

// ---- go to definition

void LspSession::goToDefinition() {
    if (noServer()) return;
    GtkTextIter cur;
    cursorIter(&cur);
    definitionAt(&cur);
}

void LspSession::definitionAt(const GtkTextIter* it) {
    if (!server_ || uri_.empty()) return;
    flushChanges();
    const int gen = ++definitionGen_;
    const std::string uri = uri_;
    std::weak_ptr<int> alive = life_;
    server_->client().definition(uri, positionOf(it),
        [alive, this, gen, uri](const Json& r, const Json&) {
            if (!alive.expired()) definitionArrived(r, gen, uri);
        });
}

void LspSession::definitionArrived(const Json& result, int gen, const std::string& uri) {
    if (gen != definitionGen_ || uri != uri_) return;
    std::vector<Lsp::Location> locs = Lsp::parseLocations(result);
    if (locs.empty()) {
        gtk_widget_error_bell(view_);
        flash("No definition found");
        return;
    }
    const Lsp::Location& loc = locs[0];
    const std::string target = Lsp::pathFromUri(loc.uri);
    if (target.empty()) return;
    const Lsp::Position s = loc.range.start, e = loc.range.end;

    if (sameFile(loc.uri, uri_)) {
        // The buffer, not the disk: it may hold unsaved edits.
        GtkTextIter ls, le;
        gtk_text_buffer_get_iter_at_line(buffer_, &ls, std::max(0, s.line));
        le = ls;
        if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
        char* raw = gtk_text_buffer_get_slice(buffer_, &ls, &le, TRUE);
        const std::string line = raw ? raw : "";
        g_free(raw);
        const size_t a = byteColumn(line, s.character);
        const size_t b = e.line == s.line ? byteColumn(line, e.character) : a;
        editor_->revealLine(s.line + 1, a, b > a ? b - a : 0);
        return;
    }

    // Another file: opened under the tree's own spelling of the path when it
    // is inside the folder (clangd answers with resolved paths), then the
    // same open-and-reveal as a Find in Folder match.
    std::string path = target;
    const std::string real = realPath(target), rootReal = realPath(root_);
    if (!rootReal.empty() && real.rfind(rootReal + "/", 0) == 0)
        path = root_ + real.substr(rootReal.size());
    gchar* raw = nullptr;
    gsize rawLen = 0;
    if (!g_file_get_contents(path.c_str(), &raw, &rawLen, nullptr)) return;
    const std::string content(raw, rawLen);
    g_free(raw);
    std::string line;
    size_t a = 0, b = 0;
    if (lineOf(content, s.line, line)) {
        a = byteColumn(line, s.character);
        b = e.line == s.line ? byteColumn(line, e.character) : a;
    }
    if (onReveal) onReveal(path, s.line + 1, a, b > a ? b - a : 0);
}

// ---- completion

void LspSession::triggerCompletion() {
    if (noServer()) return;
    requestCompletion("");
}

void LspSession::requestCompletion(const std::string& trigger) {
    if (!server_ || uri_.empty()) return;
    flushChanges();
    GtkTextIter cur, a;
    cursorIter(&cur);
    a = cur;
    for (GtkTextIter p = a; gtk_text_iter_backward_char(&p) && isIdent(gtk_text_iter_get_char(&p));)
        a = p;
    if (!anchor_) anchor_ = gtk_text_buffer_create_mark(buffer_, nullptr, &a, TRUE);
    else gtk_text_buffer_move_mark(buffer_, anchor_, &a);
    completing_ = true;
    gotItems_ = false;
    items_.clear();
    const int gen = ++completionGen_;
    const std::string uri = uri_;
    std::weak_ptr<int> alive = life_;
    server_->client().completion(uri, positionOf(&cur),
        [alive, this, gen, uri](const Json& r, const Json&) {
            if (!alive.expired()) completionArrived(r, gen, uri);
        }, trigger);
}

void LspSession::completionArrived(const Json& result, int gen, const std::string& uri) {
    if (gen != completionGen_ || uri != uri_ || !completing_) return;
    items_ = Lsp::parseCompletion(result);
    gotItems_ = true;
    refilter();
}

void LspSession::scheduleRefilter() {
    if (refilterId_) return;
    refilterId_ = g_idle_add([](gpointer p) -> gboolean {
        LspSession* self = static_cast<LspSession*>(p);
        self->refilterId_ = 0;
        self->refilter();
        return G_SOURCE_REMOVE;
    }, this);
}

// The list follows the word being typed: it narrows as it grows (prefix
// matches first, then subsequences) and closes when the caret leaves it.
void LspSession::refilter() {
    if (!completing_ || !anchor_) return;
    GtkTextIter selA, selB, anc;
    if (gtk_text_buffer_get_selection_bounds(buffer_, &selA, &selB)) {
        closeCompletion();
        return;
    }
    GtkTextIter cur;
    cursorIter(&cur);
    gtk_text_buffer_get_iter_at_mark(buffer_, &anc, anchor_);
    GtkRoot* root = gtk_widget_get_root(view_);
    if (gtk_text_iter_compare(&cur, &anc) < 0 || (root && gtk_root_get_focus(root) != view_)) {
        closeCompletion();
        return;
    }
    for (GtkTextIter p = anc; gtk_text_iter_compare(&p, &cur) < 0; gtk_text_iter_forward_char(&p))
        if (!isIdent(gtk_text_iter_get_char(&p))) {
            closeCompletion();
            return;
        }
    if (!gotItems_) return;   // still waiting for the server
    char* raw = gtk_text_buffer_get_slice(buffer_, &anc, &cur, TRUE);
    const std::string prefix = raw ? raw : "";
    g_free(raw);
    shown_ = Lsp::filterCompletions(items_, prefix);
    // Nothing to offer, or only exactly what is already typed.
    if (shown_.empty() || (shown_.size() == 1 && shown_[0].textToInsert() == prefix)) {
        closeCompletion();
        return;
    }
    if (shown_.size() > 200) shown_.resize(200);

    if (!popover_) buildPopover();
    // Keep the highlighted item while the list narrows, else the first.
    std::string previous;
    GtkListBoxRow* sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(list_));
    if (sel && gtk_widget_get_visible(popover_)) {
        GtkWidget* label = gtk_list_box_row_get_child(sel);
        if (label) previous = static_cast<const char*>(g_object_get_data(G_OBJECT(label), "item"));
    }
    gtk_list_box_remove_all(GTK_LIST_BOX(list_));
    int keep = 0;
    for (size_t i = 0; i < shown_.size(); ++i) {
        const Lsp::CompletionItem& item = shown_[i];
        char* l = g_markup_escape_text(item.label.c_str(), -1);
        std::string markup = l;
        g_free(l);
        if (!item.detail.empty()) {
            char* d = g_markup_escape_text(item.detail.c_str(), -1);
            markup += std::string("   <span foreground=\"#9CA3AF\">") + d + "</span>";
            g_free(d);
        }
        GtkWidget* label = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(label), markup.c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
        g_object_set_data_full(G_OBJECT(label), "item", g_strdup(item.label.c_str()), g_free);
        gtk_list_box_append(GTK_LIST_BOX(list_), label);
        GtkWidget* row = gtk_widget_get_parent(label);
        if (row) gtk_widget_set_focusable(row, FALSE);
        if (!previous.empty() && item.label == previous) keep = (int)i;
    }
    GdkRectangle r;
    caretRect(&anc, &r);
    gtk_popover_set_pointing_to(GTK_POPOVER(popover_), &r);
    if (!gtk_widget_get_visible(popover_)) gtk_popover_popup(GTK_POPOVER(popover_));
    GtkListBoxRow* row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list_), keep);
    gtk_list_box_select_row(GTK_LIST_BOX(list_), row);
    GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(listScroll_));
    if (keep == 0) gtk_adjustment_set_value(adj, 0);
    else moveSelection(0);
}

void LspSession::buildPopover() {
    popover_ = gtk_popover_new();
    gtk_widget_add_css_class(popover_, "minicode-completion");
    // Not autohide: an autohide popover grabs the keyboard, and typing must
    // keep going to the text view, which forwards the list's keys (onKey).
    gtk_popover_set_autohide(GTK_POPOVER(popover_), FALSE);
    gtk_popover_set_has_arrow(GTK_POPOVER(popover_), FALSE);
    gtk_popover_set_position(GTK_POPOVER(popover_), GTK_POS_BOTTOM);
    gtk_widget_set_focusable(popover_, FALSE);
    gtk_widget_set_can_focus(popover_, FALSE);

    list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list_), TRUE);
    gtk_widget_set_focusable(list_, FALSE);
    gtk_widget_set_can_focus(list_, FALSE);
    g_signal_connect(list_, "row-activated", G_CALLBACK(onRowActivated), this);

    listScroll_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(listScroll_), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(listScroll_), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(listScroll_), 220);
    gtk_widget_set_size_request(listScroll_, 420, -1);
    gtk_widget_set_can_focus(listScroll_, FALSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(listScroll_), list_);
    gtk_popover_set_child(GTK_POPOVER(popover_), listScroll_);
    gtk_widget_set_parent(popover_, view_);
}

void LspSession::onRowActivated(GtkListBox*, GtkListBoxRow* row, gpointer selfp) {
    static_cast<LspSession*>(selfp)->acceptCompletion(gtk_list_box_row_get_index(row));
}

int LspSession::selectedRow() const {
    if (!list_) return -1;
    GtkListBoxRow* row = gtk_list_box_get_selected_row(GTK_LIST_BOX(list_));
    return row ? gtk_list_box_row_get_index(row) : -1;
}

// Move the highlight (wrapping around) and keep it in view.
void LspSession::moveSelection(int delta) {
    const int n = (int)shown_.size();
    if (!list_ || n == 0) return;
    const int row = ((std::max(selectedRow(), 0) + delta) % n + n) % n;
    GtkListBoxRow* r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list_), row);
    if (!r) return;
    gtk_list_box_select_row(GTK_LIST_BOX(list_), r);
    graphene_rect_t b;
    if (!gtk_widget_compute_bounds(GTK_WIDGET(r), list_, &b)) return;
    GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(listScroll_));
    const double v = gtk_adjustment_get_value(adj), page = gtk_adjustment_get_page_size(adj);
    const double top = b.origin.y, bottom = top + b.size.height;
    if (top < v) gtk_adjustment_set_value(adj, top);
    else if (bottom > v + page) gtk_adjustment_set_value(adj, bottom - page);
}

void LspSession::closeCompletion() {
    completing_ = false;
    gotItems_ = false;
    items_.clear();
    shown_.clear();
    if (refilterId_) g_source_remove(refilterId_);
    refilterId_ = 0;
    if (popover_ && gtk_widget_get_visible(popover_)) gtk_popover_popdown(GTK_POPOVER(popover_));
}

void LspSession::acceptCompletion(int row) {
    if (row < 0 || (size_t)row >= shown_.size() || !anchor_) {
        closeCompletion();
        return;
    }
    const Lsp::CompletionItem item = shown_[(size_t)row];
    GtkTextIter cur, start;
    cursorIter(&cur);
    gtk_text_buffer_get_iter_at_mark(buffer_, &start, anchor_);
    if (item.hasEdit) {
        // The edit's range was computed on the text as it was when asked;
        // everything before the word is unchanged since, so its start holds.
        GtkTextIter es;
        iterAt(item.editRange.start, &es);
        if (gtk_text_iter_compare(&es, &cur) <= 0) start = es;
    }
    closeCompletion();
    const std::string insert = item.textToInsert();
    gtk_text_buffer_begin_user_action(buffer_);
    gtk_text_buffer_delete(buffer_, &start, &cur);
    gtk_text_buffer_insert(buffer_, &start, insert.c_str(), (int)insert.size());
    gtk_text_buffer_end_user_action(buffer_);
    gtk_text_buffer_place_cursor(buffer_, &start);   // just past the insertion
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(view_), gtk_text_buffer_get_insert(buffer_));
}

bool LspSession::completionVisible() const {
    return popover_ && gtk_widget_get_visible(popover_);
}

std::vector<std::string> LspSession::completionLabels() const {
    std::vector<std::string> out;
    if (!completionVisible()) return out;
    for (const auto& item : shown_) out.push_back(item.label);
    return out;
}

bool LspSession::completionKey(guint keyval) {
    if (!completionVisible()) return false;
    switch (keyval) {
    case GDK_KEY_Up: case GDK_KEY_KP_Up:
        moveSelection(-1);
        return true;
    case GDK_KEY_Down: case GDK_KEY_KP_Down:
        moveSelection(1);
        return true;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_ISO_Enter: case GDK_KEY_Tab:
        acceptCompletion(selectedRow());
        return true;
    case GDK_KEY_Escape:
        closeCompletion();
        return true;
    default:
        return false;
    }
}

gboolean LspSession::onKey(GtkEventControllerKey*, guint keyval, guint, GdkModifierType state,
                           gpointer selfp) {
    if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) return FALSE;
    return static_cast<LspSession*>(selfp)->completionKey(keyval) ? TRUE : FALSE;
}

void LspSession::onCtrlClick(GtkGestureClick* g, int n, double x, double y, gpointer selfp) {
    LspSession* self = static_cast<LspSession*>(selfp);
    GdkModifierType state =
        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    if (n != 1 || !(state & GDK_CONTROL_MASK) || !self->server_ || self->uri_.empty()) return;
    int bx, by;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(self->view_), GTK_TEXT_WINDOW_WIDGET,
                                          (int)x, (int)y, &bx, &by);
    GtkTextIter it;
    if (!gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(self->view_), &it, bx, by)) return;
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    gtk_text_buffer_place_cursor(self->buffer_, &it);
    gtk_widget_grab_focus(self->view_);
    self->definitionAt(&it);
}

void LspSession::onFocusLeave(GtkEventControllerFocus*, gpointer selfp) {
    static_cast<LspSession*>(selfp)->closeCompletion();
}

void LspSession::onMarkSet(GtkTextBuffer* buf, GtkTextIter*, GtkTextMark* mark, gpointer selfp) {
    LspSession* self = static_cast<LspSession*>(selfp);
    if (self->completing_ && mark == gtk_text_buffer_get_insert(buf)) self->scheduleRefilter();
}

void LspSession::caretRect(const GtkTextIter* it, GdkRectangle* r) const {
    GdkRectangle loc;
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(view_), it, &loc);
    int wx, wy;
    gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(view_), GTK_TEXT_WINDOW_WIDGET,
                                          loc.x, loc.y, &wx, &wy);
    r->x = wx;
    r->y = wy;
    r->width = std::max(loc.width, 1);
    r->height = loc.height;
}
