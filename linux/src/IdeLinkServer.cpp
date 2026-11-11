// IdeLinkServer.cpp — see IdeLinkServer.h.
#include "IdeLinkServer.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <map>

#include "ControlProtocol.h"
#include "Document.h"
#include "AgentNotes.h"
#include "Editor.h"
#include "IdeLink.h"
#include "Json.h"

using IdeLink::ToolResult;
using Respond = std::function<void(ToolResult)>;

namespace {

std::string gProblem;

// Lock files of this process, removed however it ends.
std::vector<std::string>& lockPaths() {
    static std::vector<std::string> paths;
    return paths;
}
void removeLocksAtExit() {
    for (const std::string& p : lockPaths()) unlink(p.c_str());
}

Json obj(std::initializer_list<std::pair<std::string, Json>> kv) { return Json::object(kv); }
ToolResult jsonResult(const Json& j) { return ToolResult::text(j.dump()); }

std::u16string toU16(const std::string& s) {
    glong n = 0;
    gunichar2* p = g_utf8_to_utf16(s.c_str(), (glong)s.size(), nullptr, &n, nullptr);
    std::u16string out;
    if (p) out.assign(reinterpret_cast<char16_t*>(p), (size_t)n);
    g_free(p);
    return out;
}

// 1-based line and column (in characters) of a UTF-16 offset, as notes count.
void noteLineColumn(const std::u16string& s, size_t offset, int& line, int& column) {
    offset = std::min(offset, s.size());
    line = 1;
    size_t start = 0;
    for (size_t i = 0; i < offset; i++)
        if (s[i] == u'\n') { line++; start = i + 1; }
    column = 1;
    for (size_t i = start; i < offset; i++)
        if (!(s[i] >= 0xDC00 && s[i] <= 0xDFFF)) column++;
}

std::string realPath(const std::string& p) {
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}

bool regularFile(const std::string& path) {
    struct stat st;
    return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// A plain file of UTF-8 text up to 16 MB; false with *error otherwise. A
// missing file is "" with *exists false.
bool readText(const std::string& path, std::string* text, bool* exists, std::string* error) {
    struct stat st;
    *exists = false;
    text->clear();
    if (stat(path.c_str(), &st) != 0) return true;
    *exists = true;
    if (!S_ISREG(st.st_mode)) { *error = "not a plain file: " + path; return false; }
    if (st.st_size > 16 * 1024 * 1024) { *error = path + " is larger than 16 MB"; return false; }
    gchar* data = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(path.c_str(), &data, &len, nullptr)) { *error = "cannot read " + path; return false; }
    text->assign(data, len);
    g_free(data);
    if (!IdeLink::validUtf8(*text)) { *error = path + " is not UTF-8 text"; return false; }
    return true;
}

struct Conn {
    int fd = -1;
    guint readTag = 0, writeTag = 0;
    std::string in, out;
    size_t outOff = 0;
    bool upgraded = false, closing = false, closed = false;
    IdeLink::FrameReader frames;
    std::unique_ptr<IdeLink::McpServer> mcp;
    gint64 lastProgress = 0;
};

// A diff the link offered, and whom it waits for.
struct Offered {
    Document* doc = nullptr;
    std::string title;
    std::weak_ptr<CompareDecision> decision;
    std::weak_ptr<Conn> owner;
};

}  // namespace

struct IdeLinkServer::Impl {
    IdeLinkServer* self;
    IdeLinkHost host;
    std::vector<std::shared_ptr<Conn>> conns;
    std::vector<Offered> offered;
    guint listenTag = 0, selectionTimer = 0;
    std::string writtenFolders, lastSelection;
    Json latestSelection;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    explicit Impl(IdeLinkServer* s, IdeLinkHost h) : self(s), host(std::move(h)) {}

    std::vector<std::string> workspaceFolders() const {
        std::vector<std::string> out;
        if (host.root.empty()) return out;
        out.push_back(host.root);
        const std::string real = realPath(host.root);
        if (std::find(out.begin(), out.end(), real) == out.end()) out.push_back(real);
        return out;
    }

    void note(const std::string& text) { if (host.note) host.note(text); }

    // ------------------------------------------------------------ lock file
    bool fail(const std::string& why) {
        gProblem = why;
        g_printerr("minicode: Claude Code IDE link off: %s\n", why.c_str());
        return false;
    }

    bool writeLock() {
        const std::string dir = IdeLinkServer::folder();
        const std::string config = dir.substr(0, dir.rfind('/'));
        struct stat st;
        if (stat(config.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
            return fail(config + " is not there (is Claude Code installed?)");
        if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
            return fail("cannot make " + dir + ": " + strerror(errno));
        if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid())
            return fail(dir + " is not a folder of yours");
        static bool once = false;
        if (!once) {
            once = true;
            IdeLinkServer::removeStaleLocks(dir);
            atexit(removeLocksAtExit);
        }
        IdeLink::LockInfo info;
        info.pid = getpid();
        info.workspaceFolders = workspaceFolders();
        info.ideName = IdeLink::kIdeName;
        info.transport = "ws";
        info.authToken = self->token_;
        const std::string text = IdeLink::lockFileText(info);
        const std::string path = dir + "/" + IdeLink::lockFileName(self->port_);
        const std::string tmp = dir + "/." + std::to_string(self->port_) + ".lock." +
                                std::to_string(getpid()) + ".tmp";
        unlink(tmp.c_str());
        int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) return fail("cannot write " + tmp + ": " + strerror(errno));
        size_t done = 0;
        while (done < text.size()) {
            ssize_t n = write(fd, text.data() + done, text.size() - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            done += (size_t)n;
        }
        fchmod(fd, 0600);
        close(fd);
        if (done != text.size() || rename(tmp.c_str(), path.c_str()) != 0) {
            unlink(tmp.c_str());
            return fail("cannot write " + path);
        }
        if (self->lockPath_.empty()) lockPaths().push_back(path);
        self->lockPath_ = path;
        std::string joined;
        for (const std::string& f : info.workspaceFolders) joined += f + "\n";
        writtenFolders = joined;
        return true;
    }

    void removeLock() {
        if (self->lockPath_.empty()) return;
        unlink(self->lockPath_.c_str());
        auto& paths = lockPaths();
        paths.erase(std::remove(paths.begin(), paths.end(), self->lockPath_), paths.end());
        self->lockPath_.clear();
    }

    void refreshLockIfFolderChanged() {
        if (self->listenFd_ < 0 || self->lockPath_.empty()) return;
        std::string joined;
        for (const std::string& f : workspaceFolders()) joined += f + "\n";
        if (joined != writtenFolders) writeLock();
    }

    // ------------------------------------------------------------- sockets
    bool start() {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return fail(std::string("no socket: ") + strerror(errno));
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        socklen_t len = sizeof addr;
        if (bind(fd, (struct sockaddr*)&addr, sizeof addr) != 0 || listen(fd, 8) != 0 ||
            getsockname(fd, (struct sockaddr*)&addr, &len) != 0) {
            const std::string why = strerror(errno);
            close(fd);
            return fail("cannot listen on 127.0.0.1: " + why);
        }
        self->listenFd_ = fd;
        self->port_ = ntohs(addr.sin_port);
        unsigned char raw[24];
        FILE* r = fopen("/dev/urandom", "rb");
        if (!r || fread(raw, 1, sizeof raw, r) != sizeof raw) {
            if (r) fclose(r);
            stop();
            return fail("no random bytes for the secret");
        }
        fclose(r);
        self->token_ = IdeLink::toHex(std::string((const char*)raw, sizeof raw));
        if (!writeLock()) { stop(); return false; }
        listenTag = g_unix_fd_add(fd, G_IO_IN, [](gint, GIOCondition, gpointer p) -> gboolean {
            static_cast<Impl*>(p)->acceptConnections();
            return G_SOURCE_CONTINUE;
        }, this);
        selectionTimer = g_timeout_add(300, [](gpointer p) -> gboolean {
            static_cast<Impl*>(p)->sendSelectionIfChanged();
            return G_SOURCE_CONTINUE;
        }, this);
        gProblem.clear();
        return true;
    }

    void stop() {
        for (auto c : std::vector<std::shared_ptr<Conn>>(conns)) closeConn(c);
        conns.clear();
        // Diffs waiting for Claude Code wait for nobody now.
        for (Offered& o : offered)
            if (auto dec = o.decision.lock()) dec->endDecision("The IDE link is off.");
        offered.clear();
        if (listenTag) g_source_remove(listenTag);
        listenTag = 0;
        if (selectionTimer) g_source_remove(selectionTimer);
        selectionTimer = 0;
        if (self->listenFd_ >= 0) close(self->listenFd_);
        self->listenFd_ = -1;
        self->port_ = 0;
        removeLock();
    }

    void acceptConnections() {
        for (;;) {
            int fd = accept4(self->listenFd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) return;
            size_t live = 0;
            for (auto& c : conns) if (!c->closed) live++;
            if (live >= 8) { close(fd); continue; }
            serve(fd);
        }
    }

    void serve(int fd) {
        auto conn = std::make_shared<Conn>();
        conn->fd = fd;
        conns.push_back(conn);
        struct Ctx { Impl* impl; std::weak_ptr<Conn> conn; };
        Ctx* ctx = new Ctx{this, conn};
        conn->readTag = g_unix_fd_add_full(G_PRIORITY_DEFAULT, fd, G_IO_IN,
            [](gint, GIOCondition, gpointer p) -> gboolean {
                Ctx* c = static_cast<Ctx*>(p);
                if (auto s = c->conn.lock()) if (!s->closed) c->impl->readFrom(s);
                return G_SOURCE_CONTINUE;
            }, ctx, [](gpointer p) { delete static_cast<Ctx*>(p); });
        // A peer that has not finished its handshake in 10 s is let go.
        struct Late { Impl* impl; std::weak_ptr<Conn> conn; std::weak_ptr<bool> alive; };
        Late* late = new Late{this, conn, alive};
        g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 10, [](gpointer p) -> gboolean {
            Late* l = static_cast<Late*>(p);
            if (!l->alive.expired())
                if (auto c = l->conn.lock()) if (!c->closed && !c->upgraded) l->impl->closeConn(c);
            return G_SOURCE_REMOVE;
        }, late, [](gpointer p) { delete static_cast<Late*>(p); });
    }

    void closeConn(const std::shared_ptr<Conn>& c) {
        if (c->closed) return;
        c->closed = true;
        if (c->readTag) g_source_remove(c->readTag);
        if (c->writeTag) g_source_remove(c->writeTag);
        c->readTag = c->writeTag = 0;
        if (c->fd >= 0) close(c->fd);
        c->fd = -1;
        const bool wasClient = c->mcp && c->mcp->initialized();
        c->mcp.reset();   // an answer still to come goes nowhere
        conns.erase(std::remove(conns.begin(), conns.end(), c), conns.end());
        for (Offered& o : offered) {
            auto owner = o.owner.lock();
            if (!owner || owner == c)
                if (auto dec = o.decision.lock())
                    dec->endDecision("Claude Code is no longer waiting for an answer.");
        }
        prune();
        if (wasClient) note("Claude Code left the IDE link");
    }

    bool docExists(Document* d) const {
        const auto& docs = host.editor->documents();
        return std::find(docs.begin(), docs.end(), d) != docs.end();
    }

    void prune() {
        offered.erase(std::remove_if(offered.begin(), offered.end(), [&](const Offered& o) {
            auto dec = o.decision.lock();
            return !dec || dec->decision() != 0 || !docExists(o.doc);
        }), offered.end());
    }

    void send(const std::shared_ptr<Conn>& c, const std::string& bytes) {
        if (c->closed) return;
        c->out += bytes;
        flush(c);
    }

    // Writes what the peer takes now; the rest waits for the write source. A
    // peer that takes nothing for 30 s is let go.
    void flush(const std::shared_ptr<Conn>& c) {
        while (c->outOff < c->out.size()) {
            ssize_t n = ::send(c->fd, c->out.data() + c->outOff, c->out.size() - c->outOff, MSG_NOSIGNAL);
            if (n > 0) { c->outOff += (size_t)n; c->lastProgress = g_get_monotonic_time(); continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && errno == EAGAIN) {
                if (!c->writeTag) {
                    struct Ctx { Impl* impl; std::weak_ptr<Conn> conn; };
                    Ctx* ctx = new Ctx{this, c};
                    c->writeTag = g_unix_fd_add_full(G_PRIORITY_DEFAULT, c->fd, G_IO_OUT,
                        [](gint, GIOCondition, gpointer p) -> gboolean {
                            Ctx* x = static_cast<Ctx*>(p);
                            if (auto s = x->conn.lock()) if (!s->closed) x->impl->flush(s);
                            return G_SOURCE_CONTINUE;
                        }, ctx, [](gpointer p) { delete static_cast<Ctx*>(p); });
                    struct Dog { Impl* impl; std::weak_ptr<Conn> conn; std::weak_ptr<bool> alive; };
                    Dog* dog = new Dog{this, c, alive};
                    g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 30, [](gpointer p) -> gboolean {
                        Dog* d = static_cast<Dog*>(p);
                        if (d->alive.expired()) return G_SOURCE_REMOVE;
                        auto s = d->conn.lock();
                        if (!s || s->closed || !s->writeTag) return G_SOURCE_REMOVE;
                        if (g_get_monotonic_time() - s->lastProgress >= 29 * G_USEC_PER_SEC) {
                            d->impl->closeConn(s);
                            return G_SOURCE_REMOVE;
                        }
                        return G_SOURCE_CONTINUE;
                    }, dog, [](gpointer p) { delete static_cast<Dog*>(p); });
                }
                return;
            }
            closeConn(c);
            return;
        }
        c->out.clear();
        c->outOff = 0;
        if (c->writeTag) { g_source_remove(c->writeTag); c->writeTag = 0; }
        if (c->closing) closeConn(c);
    }

    void readFrom(const std::shared_ptr<Conn>& c) {
        char buf[65536];
        ssize_t n = read(c->fd, buf, sizeof buf);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        if (n <= 0) { closeConn(c); return; }
        if (c->closing) return;   // said goodbye: the rest is not read
        if (!c->upgraded) {
            c->in.append(buf, (size_t)n);
            IdeLink::Handshake h = IdeLink::readHandshake(c->in, self->token_);
            if (h.state == IdeLink::Handshake::Incomplete) return;
            if (h.state == IdeLink::Handshake::Refused) {
                g_printerr("minicode: IDE link refused a connection: %s\n", h.error.c_str());
                c->closing = true;
                send(c, h.response);
                return;
            }
            c->upgraded = true;
            std::string rest = c->in.substr(h.consumed);
            c->in.clear();
            send(c, h.response);
            startMcp(c);
            if (!rest.empty()) c->frames.feed(rest);
        } else {
            c->frames.feed(buf, (size_t)n);
        }
        readFrames(c);
    }

    void readFrames(const std::shared_ptr<Conn>& c) {
        IdeLink::Message m;
        for (;;) {
            if (c->closed || c->closing) return;
            IdeLink::FrameReader::Result r = c->frames.next(m);
            if (r == IdeLink::FrameReader::NeedMore) return;
            if (r == IdeLink::FrameReader::Error) {
                c->closing = true;
                send(c, IdeLink::encodeClose(c->frames.errorCode(), c->frames.error()));
                return;
            }
            switch (m.op) {
                case IdeLink::Opcode::Text:
                    if (c->mcp) c->mcp->handle(m.payload);
                    break;
                case IdeLink::Opcode::Ping:
                    send(c, IdeLink::encodeFrame(IdeLink::Opcode::Pong, m.payload));
                    break;
                case IdeLink::Opcode::Pong:
                    break;
                case IdeLink::Opcode::Close:
                    c->closing = true;
                    send(c, IdeLink::encodeClose(m.closeCode == 1005 ? 1000 : m.closeCode));
                    return;
                default:   // binary: MCP is text
                    c->closing = true;
                    send(c, IdeLink::encodeClose(1003, "text frames only"));
                    return;
            }
        }
    }

    void broadcast(const std::string& message) {
        const std::string frame = IdeLink::encodeFrame(IdeLink::Opcode::Text, message);
        for (auto c : std::vector<std::shared_ptr<Conn>>(conns))
            if (c->mcp && c->mcp->initialized() && !c->closing) send(c, frame);
    }

    // ------------------------------------------------------------ selection
    bool userIsInDocument() const {
        if (!host.window || !host.editor || !gtk_window_is_active(host.window)) return false;
        GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(host.window));
        return focus && focus == host.editor->textView();
    }

    // The line's UTF-16 column of an iterator.
    static int utf16Column(GtkTextBuffer* b, const GtkTextIter& at) {
        GtkTextIter s = at;
        gtk_text_iter_set_line_offset(&s, 0);
        int col = 0;
        while (gtk_text_iter_compare(&s, &at) < 0) {
            col += gtk_text_iter_get_char(&s) > 0xFFFF ? 2 : 1;
            gtk_text_iter_forward_char(&s);
        }
        (void)b;
        return col;
    }

    // The editor's selection in VS Code's shape, or null when no text file is
    // on show. A preview gives the file with an empty selection at its top.
    Json currentSelection() const {
        Editor* ed = host.editor;
        Document* d = ed ? ed->active() : nullptr;
        if (!d || d->path().empty() || d->kind() != Document::Kind::Text) return Json();
        IdeLink::Selection s;
        s.filePath = d->path();
        GtkTextBuffer* b = ed->sourceBuffer();
        if (b && !d->showsPreview()) {
            GtkTextIter a, e;
            if (!gtk_text_buffer_get_selection_bounds(b, &a, &e)) {
                gtk_text_buffer_get_iter_at_mark(b, &a, gtk_text_buffer_get_insert(b));
                e = a;
            }
            char* raw = gtk_text_buffer_get_text(b, &a, &e, TRUE);
            s.text = raw ? raw : "";
            g_free(raw);
            if (s.text.size() > ControlChannel::kMaxSelectionBytes)
                s.text = ControlChannel::clipUtf8(s.text, ControlChannel::kMaxSelectionBytes);
            s.startLine = gtk_text_iter_get_line(&a);
            s.startCharacter = utf16Column(b, a);
            s.endLine = gtk_text_iter_get_line(&e);
            s.endCharacter = utf16Column(b, e);
        }
        return IdeLink::selectionJson(s);
    }

    void sendSelectionTo(const std::shared_ptr<Conn>& c) {
        Json sel = currentSelection();
        if (sel.isNull() || !c->mcp || !c->mcp->initialized()) return;
        send(c, IdeLink::encodeFrame(IdeLink::Opcode::Text,
                                     IdeLink::McpServer::notification("selection_changed", sel)));
    }

    void sendSelectionIfChanged() {
        bool anyone = false;
        for (auto& c : conns) if (c->mcp && c->mcp->initialized()) anyone = true;
        refreshLockIfFolderChanged();
        if (!anyone) return;
        Json sel = currentSelection();
        if (sel.isNull()) return;
        latestSelection = sel;
        const std::string text = sel.dump();
        if (text == lastSelection) return;
        lastSelection = text;
        broadcast(IdeLink::McpServer::notification("selection_changed", sel));
    }

    // ----------------------------------------------------------------- MCP
    void startMcp(const std::shared_ptr<Conn>& c) {
        c->mcp.reset(new IdeLink::McpServer(IdeLink::kIdeName, "1"));
        std::weak_ptr<Conn> weak = c;
        std::weak_ptr<bool> weakAlive = alive;
        Impl* me = this;
        c->mcp->send = [weak, weakAlive, me](const std::string& text) {
            auto s = weak.lock();
            if (s && !weakAlive.expired())
                me->send(s, IdeLink::encodeFrame(IdeLink::Opcode::Text, text));
        };
        c->mcp->onNotification = [weak, weakAlive, me](const std::string& method, const Json& params) {
            auto s = weak.lock();
            if (!s || weakAlive.expired()) return;
            (void)params;
            if (method == "ide_connected") me->note("Claude Code joined through the IDE link");
            else if (method == "notifications/initialized") me->sendSelectionTo(s);
        };
        addTools(*c->mcp, c);
    }

    bool dirtyPath(const std::string& path, bool* known) const {
        const std::string want = realPath(path);
        for (Document* d : host.editor->documents())
            if (!d->path().empty() && realPath(d->path()) == want) { *known = true; return d->dirty(); }
        *known = false;
        return false;
    }

    // A file's text: the buffer of the tab on show, else the disk's.
    bool fileText(const std::string& path, std::string* text) const {
        Editor* ed = host.editor;
        Document* a = ed->active();
        if (a && a->kind() == Document::Kind::Text && !a->path().empty() &&
            realPath(a->path()) == realPath(path) && ed->sourceBuffer() && !a->showsPreview()) {
            GtkTextBuffer* b = ed->sourceBuffer();
            GtkTextIter s, e;
            gtk_text_buffer_get_bounds(b, &s, &e);
            char* raw = gtk_text_buffer_get_text(b, &s, &e, TRUE);
            *text = raw ? raw : "";
            g_free(raw);
            return true;
        }
        bool exists;
        std::string error;
        return readText(path, text, &exists, &error) && exists;
    }

    Json diagnostics(const std::string& uri) const {
        std::string only;
        if (!uri.empty()) {
            only = IdeLink::pathFromUri(uri);
            if (only.empty()) return Json::array();
        }
        const std::string onlyReal = only.empty() ? "" : realPath(only);
        std::vector<IdeLink::FileDiagnostics> files;
        std::map<std::string, size_t> index;
        std::vector<LspProblem> rows = host.problems ? host.problems() : std::vector<LspProblem>();
        for (const LspProblem& p : rows) {
            if (p.heading || p.path.empty()) continue;
            std::string name = p.path;
            if (!only.empty()) {
                if (realPath(p.path) != onlyReal) continue;
                name = only;
            }
            auto it = index.find(name);
            if (it == index.end()) {
                it = index.emplace(name, files.size()).first;
                IdeLink::FileDiagnostics f;
                f.path = name;
                files.push_back(f);
            }
            IdeLink::Diagnostic d;
            d.line = p.line;
            d.character = p.column;
            d.endLine = p.endLine;
            d.endCharacter = p.endColumn;
            d.severity = p.severity;
            d.message = p.fullMessage.empty() ? p.message : p.fullMessage;
            d.source = p.source;
            d.code = p.code;
            files[it->second].diagnostics.push_back(d);
        }
        if (!only.empty() && files.empty()) {
            IdeLink::FileDiagnostics f;
            f.path = only;
            files.push_back(f);
        }
        return IdeLink::diagnosticsJson(files);
    }

    // Closes the diff tabs the link offered (named `name`, or all of them).
    size_t closeOffered(const std::string& name, bool all) {
        size_t n = 0;
        std::vector<Offered> list = offered;
        for (const Offered& o : list) {
            if (!docExists(o.doc) || (!all && o.title != name)) continue;
            host.editor->closeTab(o.doc);
            n++;
        }
        prune();
        return n;
    }

    void openDiff(const Json& args, const std::shared_ptr<Conn>& conn, Respond respond) {
        const std::string root = host.root;
        const std::string newPath = IdeLink::resolvePath(args["new_file_path"].asString(), root);
        const std::string oldPath = args.has("old_file_path")
            ? IdeLink::resolvePath(args["old_file_path"].asString(), root) : newPath;
        if (newPath.empty()) { respond(ToolResult::failure("openDiff needs new_file_path")); return; }
        std::string left, right, error;
        bool exists = false, there = false;
        if (!readText(oldPath, &left, &exists, &error)) { respond(ToolResult::failure(error)); return; }
        if (args["new_file_contents"].isString()) right = args["new_file_contents"].asString();
        else if (!readText(newPath, &right, &there, &error)) { respond(ToolResult::failure(error)); return; }
        const std::string base = newPath.substr(newPath.rfind('/') == std::string::npos ? 0 : newPath.rfind('/') + 1);
        const std::string tabName = args["tab_name"].isString() ? args["tab_name"].asString()
                                                                : base + " (Claude Code)";
        std::string shortPath = newPath;
        const std::string realRoot = realPath(root);
        const std::string realNew = realPath(newPath);
        if (realRoot.size() > 1 && realNew.compare(0, realRoot.size() + 1, realRoot + "/") == 0)
            shortPath = realNew.substr(realRoot.size() + 1);
        CompareSpec spec;
        spec.title = tabName;
        spec.leftLabel = shortPath + (exists ? " (on disk)" : " (a new file)");
        spec.rightLabel = shortPath + " (Claude Code's change)";
        spec.leftText = left;
        spec.rightText = right;
        spec.path = newPath;
        spec.decisionPrompt = std::string("Claude Code wants to ") + (exists ? "change " : "create ") + base + ".";
        spec.quiet = userIsInDocument();
        std::weak_ptr<bool> weakAlive = alive;
        Impl* me = this;
        const std::string answer = right;
        spec.onDecision = [respond, tabName, weakAlive, me](bool accepted, const std::string& text) {
            respond(accepted ? IdeLink::diffAccepted(text) : IdeLink::diffRejected(tabName));
            if (!weakAlive.expired()) me->prune();
        };
        (void)answer;
        std::weak_ptr<Conn> weakConn = conn;
        host.editor->showComparison(spec, [weakAlive, me, weakConn, tabName](Document* d) {
            if (weakAlive.expired() || !d) return;
            Offered o;
            o.doc = d;
            o.title = tabName;
            o.decision = d->decision();
            o.owner = weakConn;
            auto owner = weakConn.lock();
            if (!owner || owner->closed) {
                if (d->decision()) d->decision()->endDecision("Claude Code is no longer waiting for an answer.");
                return;
            }
            me->offered.push_back(o);
        });
    }

    void openFile(const Json& args, Respond respond) {
        Editor* ed = host.editor;
        const std::string path = IdeLink::resolvePath(args["filePath"].asString(), host.root);
        if (!regularFile(path)) { respond(ToolResult::failure("File not found: " + path)); return; }
        Document* active = ed->active();
        const bool beside = userIsInDocument() &&
                            !(active && !active->path().empty() && realPath(active->path()) == realPath(path));
        Document* tab = ed->openForAgent(path, args["preview"].asBool(false), beside);
        if (!tab) { respond(ToolResult::failure("Could not open " + path)); return; }
        std::string message = "Opened file: " + path;
        const std::string start = args["startText"].asString();
        if (!start.empty()) {
            std::string text;
            const bool read = fileText(path, &text);
            const std::string end = args["endText"].asString();
            IdeLink::TextRange r;
            std::u16string u;
            if (read) {
                u = toU16(text);
                r = IdeLink::findTextRange(u, toU16(start), toU16(end), args["selectToEndOfLine"].asBool(false));
            }
            if (r.found) {
                int line, col, toLine, toCol;
                noteLineColumn(u, r.start, line, col);
                noteLineColumn(u, r.end, toLine, toCol);
                ed->notes()->show(tab, "", "Claude Code", line, col, toLine, toCol);
                if (end.empty()) message = "Opened file and selected text \"" + start + "\"";
                else if (r.endFound)
                    message = "Opened file and selected text from \"" + start + "\" to \"" + end + "\"";
                else
                    message = "Opened file and positioned at \"" + start + "\" (end text \"" + end + "\" not found)";
            } else {
                message = "Opened file, but text \"" + start + "\" not found";
            }
            respond(ToolResult::text(message));
            return;
        }
        if (!args["makeFrontmost"].asBool(true)) {
            bool known;
            const bool dirty = dirtyPath(path, &known);
            respond(jsonResult(obj({{"success", true}, {"filePath", path},
                                    {"fileUrl", IdeLink::fileUri(path)}, {"message", message},
                                    {"isDirty", dirty}})));
            return;
        }
        respond(ToolResult::text(message));
    }

    void addTools(IdeLink::McpServer& mcp, const std::shared_ptr<Conn>& conn) {
        using IdeLink::objectSchema;
        using IdeLink::schemaProperty;
        using IdeLink::Tool;
        std::weak_ptr<bool> weakAlive = alive;
        std::weak_ptr<Conn> weakConn = conn;
        Impl* me = this;
        auto add = [&](const char* name, const char* description, Json schema,
                       std::function<void(const Json&, Respond)> run) {
            Tool t;
            t.name = name;
            t.description = description;
            t.inputSchema = schema;
            t.run = [weakAlive, me, run](const Json& args, Respond respond) {
                if (weakAlive.expired()) { respond(ToolResult::failure("the window has closed")); return; }
                me->refreshLockIfFolderChanged();
                run(args, respond);
            };
            mcp.addTool(std::move(t));
        };

        add("openFile", "Open a file in the editor and optionally select a range of text",
            objectSchema({{"filePath", schemaProperty("string", "Path to the file to open")},
                          {"preview", schemaProperty("boolean", "Whether to open the file in preview mode")},
                          {"startText", schemaProperty("string", "Text pattern to find the start of the selection range. Selects from the beginning of this match.")},
                          {"endText", schemaProperty("string", "Text pattern to find the end of the selection range. Selects up to the end of this match. If not provided, only the startText match will be selected.")},
                          {"selectToEndOfLine", schemaProperty("boolean", "If true, selection will extend to the end of the line containing the endText match.")},
                          {"makeFrontmost", schemaProperty("boolean", "Whether to make the file the active editor tab. If false, the file will be opened in the background without changing focus.")}},
                         {"filePath"}),
            [me](const Json& args, Respond respond) { me->openFile(args, respond); });

        add("openDiff", "Open a git diff for the file",
            objectSchema({{"old_file_path", schemaProperty("string", "Path to the file to show diff for. If not provided, uses active editor.")},
                          {"new_file_path", schemaProperty("string", "Path to the file to show diff for. If not provided, uses active editor.")},
                          {"new_file_contents", schemaProperty("string", "Contents of the new file. If not provided then the current file contents of new_file_path will be used.")},
                          {"tab_name", schemaProperty("string", "Path to the file to show diff for. If not provided, uses active editor.")}}),
            [me, weakConn](const Json& args, Respond respond) {
                auto c = weakConn.lock();
                if (!c) { respond(ToolResult::failure("the connection has closed")); return; }
                me->openDiff(args, c, respond);
            });

        add("getDiagnostics", "Get language diagnostics from VS Code",
            objectSchema({{"uri", schemaProperty("string", "Optional file URI to get diagnostics for. If not provided, gets diagnostics for all files.")}}),
            [me](const Json& args, Respond respond) { respond(jsonResult(me->diagnostics(args["uri"].asString()))); });

        add("close_tab", "Close a diff tab Claude Code opened, by its name",
            objectSchema({{"tab_name", schemaProperty("string", "The tab's name")}}, {"tab_name"}),
            [me](const Json& args, Respond respond) {
                me->closeOffered(args["tab_name"].asString(), false);
                respond(ToolResult::text("TAB_CLOSED"));
            });

        add("closeAllDiffTabs", "Close all diff tabs in the editor", objectSchema({}),
            [me](const Json&, Respond respond) {
                const size_t n = me->closeOffered("", true);
                respond(ToolResult::text("CLOSED_" + std::to_string(n) + "_DIFF_TABS"));
            });

        add("getCurrentSelection", "Get the current text selection in the active editor", objectSchema({}),
            [me](const Json&, Respond respond) {
                Json sel = me->currentSelection();
                if (sel.isNull()) {
                    respond(jsonResult(obj({{"success", false}, {"message", "No active editor found"}})));
                    return;
                }
                sel.set("success", true);
                respond(jsonResult(sel));
            });

        add("getLatestSelection", "Get the most recent text selection (even if not in the active editor)",
            objectSchema({}), [me](const Json&, Respond respond) {
                Json sel = me->currentSelection();
                if (sel.isNull()) sel = me->latestSelection;
                if (sel.isNull()) {
                    respond(jsonResult(obj({{"success", false}, {"message", "No selection available"}})));
                    return;
                }
                sel.set("success", true);
                respond(jsonResult(sel));
            });

        add("getOpenEditors", "Get information about currently open editors", objectSchema({}),
            [me](const Json&, Respond respond) {
                Json tabs = Json::array();
                Editor* ed = me->host.editor;
                for (Document* d : ed->documents()) {
                    if (d->path().empty() || d->kind() != Document::Kind::Text) continue;
                    const std::string& p = d->path();
                    tabs.push(obj({{"uri", IdeLink::fileUri(p)},
                                   {"isActive", d == ed->active()},
                                   {"isPinned", false},
                                   {"isPreview", d->isPreviewTab()},
                                   {"isDirty", d->dirty()},
                                   {"label", p.substr(p.rfind('/') == std::string::npos ? 0 : p.rfind('/') + 1)},
                                   {"groupIndex", 0},
                                   {"viewColumn", 1},
                                   {"isGroupActive", true},
                                   {"fileName", p}}));
                }
                respond(jsonResult(obj({{"tabs", tabs}})));
            });

        add("getWorkspaceFolders", "Get all workspace folders currently open in the IDE", objectSchema({}),
            [me](const Json&, Respond respond) {
                const std::string root = me->host.root;
                Json folders = Json::array();
                if (!root.empty())
                    folders.push(obj({{"name", root.substr(root.rfind('/') == std::string::npos ? 0 : root.rfind('/') + 1)},
                                      {"uri", IdeLink::fileUri(root)}, {"path", root}, {"index", 0}}));
                respond(jsonResult(obj({{"success", true}, {"folders", folders},
                                        {"rootPath", root.empty() ? Json() : Json(root)},
                                        {"workspaceFile", Json()}})));
            });

        add("checkDocumentDirty", "Check if a document has unsaved changes (is dirty)",
            objectSchema({{"filePath", schemaProperty("string", "Path to the file to check")}}, {"filePath"}),
            [me](const Json& args, Respond respond) {
                const std::string path = IdeLink::resolvePath(args["filePath"].asString(), me->host.root);
                bool known;
                const bool dirty = me->dirtyPath(path, &known);
                if (!known) {
                    respond(jsonResult(obj({{"success", false}, {"message", "Document not open: " + path}})));
                    return;
                }
                respond(jsonResult(obj({{"success", true}, {"filePath", path}, {"isDirty", dirty},
                                        {"isUntitled", false}})));
            });
    }
};

// ------------------------------------------------------------------ public
IdeLinkServer::IdeLinkServer(IdeLinkHost host) : d_(new Impl(this, std::move(host))) {}

IdeLinkServer::~IdeLinkServer() {
    d_->alive.reset();
    d_->stop();
    delete d_;
}

void IdeLinkServer::setEnabled(bool on) {
    if (on && listenFd_ < 0) d_->start();
    else if (!on && listenFd_ >= 0) d_->stop();
}

size_t IdeLinkServer::connectionCount() const {
    size_t n = 0;
    for (auto& c : d_->conns) if (c->upgraded && !c->closed) n++;
    return n;
}

size_t IdeLinkServer::clientCount() const {
    size_t n = 0;
    for (auto& c : d_->conns) if (c->mcp && c->mcp->initialized() && !c->closed) n++;
    return n;
}

std::vector<std::string> IdeLinkServer::terminalEnvironment() const {
    if (listenFd_ < 0 || !port_) return {};
    return {"CLAUDE_CODE_SSE_PORT=" + std::to_string(port_)};
}

const std::string& IdeLinkServer::problem() { return gProblem; }

std::string IdeLinkServer::folder() {
    const char* dir = getenv("CLAUDE_CONFIG_DIR");
    if (dir && *dir) return std::string(dir) + "/ide";
    return std::string(g_get_home_dir()) + "/.claude/ide";
}

unsigned IdeLinkServer::removeStaleLocks(const std::string& dir) {
    unsigned removed = 0;
    DIR* d = opendir(dir.c_str());
    if (!d) return 0;
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (IdeLink::portFromLockName(name) == 0) continue;
        const std::string path = dir + "/" + name;
        struct stat st;
        if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
            st.st_size > 65536)
            continue;
        gchar* data = nullptr;
        gsize len = 0;
        if (!g_file_get_contents(path.c_str(), &data, &len, nullptr)) continue;
        IdeLink::LockInfo info;
        const bool ok = IdeLink::parseLockFile(std::string(data, len), info);
        g_free(data);
        if (!ok) continue;
        const bool alive = info.pid > 0 && (kill(info.pid, 0) == 0 || errno != ESRCH);
        if (!IdeLink::isStaleOwnLock(info, getpid(), alive)) continue;
        if (unlink(path.c_str()) == 0) removed++;
    }
    closedir(d);
    return removed;
}
