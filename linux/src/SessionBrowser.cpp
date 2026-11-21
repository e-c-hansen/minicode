// SessionBrowser.cpp — see SessionBrowser.h.
#include "SessionBrowser.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <unistd.h>

#include <gtk/gtk.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#include "Json.h"
#ifdef MUTIBLE_ENABLE_PDF
#include <cairo.h>
#include <poppler.h>

#include <algorithm>
#include <cmath>

#include "Latex.h"
#endif
#include "SessionLink.h"
#include "SessionWeb.h"

const char* const SessionBrowser::kTermNote = "Terminals opened while the session host is on appear here";
std::function<void(std::function<void()>)> SessionBrowser::askDelay;
std::function<void(const std::string&)> SessionBrowser::opener;

#ifdef MUTIBLE_ENABLE_PDF
// ---------------------------------------------------------- pages as pictures
// The page's typeset drawn page by page (Options::typesetPageSizes and
// typesetPageRender, the Mac's TypesetPages.mm). Both run on threads of the
// server's own, so each call opens its own document from the bytes and
// touches no GTK.
namespace {

PopplerDocument* openPdf(const std::string& pdf) {
    if (pdf.empty()) return nullptr;
    GBytes* bytes = g_bytes_new(pdf.data(), pdf.size());
    GError* err = nullptr;
    PopplerDocument* doc = poppler_document_new_from_bytes(bytes, nullptr, &err);
    g_bytes_unref(bytes);
    if (err) g_error_free(err);
    if (doc && poppler_document_get_n_pages(doc) <= 0) {
        g_object_unref(doc);
        doc = nullptr;
    }
    return doc;
}

bool pageSizes(const std::string& pdf, std::vector<Shared::Web::PageSize>* sizes) {
    PopplerDocument* doc = openPdf(pdf);
    if (!doc) return false;
    const int n = poppler_document_get_n_pages(doc);
    sizes->clear();
    bool ok = (std::size_t)n <= Shared::Web::kMaxTypesetPages;
    for (int i = 0; ok && i < n; i++) {
        PopplerPage* page = poppler_document_get_page(doc, i);
        double w = 0, h = 0;
        if (page) poppler_page_get_size(page, &w, &h);   // the crop box, turned by its rotation
        if (page) g_object_unref(page);
        if (!(w > 0 && h > 0)) ok = false;
        else sizes->push_back(Shared::Web::PageSize{w, h});
    }
    g_object_unref(doc);
    return ok && !sizes->empty();
}

cairo_status_t writePng(void* closure, const unsigned char* data, unsigned int length) {
    static_cast<std::string*>(closure)->append(reinterpret_cast<const char*>(data), length);
    return CAIRO_STATUS_SUCCESS;
}

bool renderPage(const std::string& pdf, int pageIndex, int width, std::string* png) {
    if (width <= 0 || pageIndex < 0) return false;
    PopplerDocument* doc = openPdf(pdf);
    if (!doc) return false;
    PopplerPage* page = pageIndex < poppler_document_get_n_pages(doc) ? poppler_document_get_page(doc, pageIndex)
                                                                       : nullptr;
    bool ok = false;
    double w = 0, h = 0;
    if (page) poppler_page_get_size(page, &w, &h);
    if (page && w > 0 && h > 0) {
        const double scale = width / w;
        const double height = std::max(1.0, std::ceil(h * scale));
        if (width * height <= Shared::Web::kMaxPagePixels * 1.01) {
            cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, (int)height);
            cairo_t* cr = cairo_create(surface);
            if (cairo_status(cr) == CAIRO_STATUS_SUCCESS) {
                cairo_set_source_rgb(cr, 1, 1, 1);
                cairo_paint(cr);
                cairo_scale(cr, scale, scale);
                poppler_page_render(page, cr);
                cairo_surface_flush(surface);
                std::string out;
                ok = cairo_surface_write_to_png_stream(surface, writePng, &out) == CAIRO_STATUS_SUCCESS;
                if (ok) *png = std::move(out);
            }
            cairo_destroy(cr);
            cairo_surface_destroy(surface);
        }
    }
    if (page) g_object_unref(page);
    g_object_unref(doc);
    return ok;
}

}  // namespace
#endif

// What the server's threads and the main loop's sources hold of the object:
// `self` is cleared, on the main loop, as it goes, so a callback that comes
// later does nothing.
struct SessionBrowser::Guard {
    SessionBrowser* self = nullptr;
};

namespace {

// `f` run on the main loop (from any thread).
void onMain(std::function<void()> f) {
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        [](gpointer p) -> gboolean {
            (*static_cast<std::function<void()>*>(p))();
            return G_SOURCE_REMOVE;
        },
        new std::function<void()>(std::move(f)), [](gpointer p) { delete static_cast<std::function<void()>*>(p); });
}

// ---- the state file: {"editable": bool, "remembered": [{id, browser,
// address, since}]}, read by the server's thread too.
std::mutex gStateMu;

Json readState() {
    gchar* text = nullptr;
    gsize len = 0;
    Json j;
    if (g_file_get_contents(SessionBrowser::stateFile().c_str(), &text, &len, nullptr))
        Json::parse(std::string(text, len), j);
    g_free(text);
    return j.isObject() ? j : Json::object();
}

void writeState(const Json& j) {
    const std::string path = SessionBrowser::stateFile();
    gchar* dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    const std::string text = j.dump() + "\n";
    GError* err = nullptr;
    // It names the browsers that may open the user's files without asking:
    // theirs alone.
    if (!g_file_set_contents_full(path.c_str(), text.data(), (gssize)text.size(), G_FILE_SET_CONTENTS_CONSISTENT,
                                  0600, &err)) {
        g_printerr("mutible: could not write %s: %s\n", path.c_str(), err ? err->message : "unknown error");
        g_clear_error(&err);
    }
}

bool isRemembered(const std::string& browserId) {
    if (browserId.empty()) return false;
    for (const SessionBrowser::Remembered& r : SessionBrowser::remembered())
        if (r.id == browserId) return true;
    return false;
}

void remember(const std::string& browserId, const std::string& browser, const std::string& address) {
    if (browserId.empty()) return;
    std::lock_guard<std::mutex> lk(gStateMu);
    Json state = readState();
    Json list = Json::array();
    const Json& had = state["remembered"];
    for (size_t i = 0; had.isArray() && i < had.size(); i++)
        if (had[i].isObject() && had[i]["id"].asString() != browserId) list.push(had[i]);
    Json e = Json::object();
    e.set("id", Json(browserId));
    e.set("browser", Json(browser));
    e.set("address", Json(address));
    e.set("since", Json((int64_t)(g_get_real_time() / G_USEC_PER_SEC)));
    list.push(std::move(e));
    state.set("remembered", std::move(list));
    writeState(state);
}

// ---- where a connection comes from (/proc/net/tcp and tcp6)
struct TcpRow {
    bool v6 = false;
    unsigned char local[16] = {}, remote[16] = {};
    int localPort = 0, remotePort = 0, state = 0;
    unsigned long inode = 0;
};

// "0100007F:1F90" as the kernel prints it: the address's 32-bit words in
// the machine's order, then the port.
bool parseEndpoint(const std::string& s, bool v6, unsigned char* addr, int* port) {
    const size_t colon = s.find(':');
    const size_t hexLen = v6 ? 32 : 8;
    if (colon != hexLen) return false;
    for (size_t w = 0; w < hexLen / 8; w++) {
        const uint32_t word = (uint32_t)std::strtoul(s.substr(w * 8, 8).c_str(), nullptr, 16);
        std::memcpy(addr + w * 4, &word, 4);
    }
    *port = (int)std::strtol(s.c_str() + colon + 1, nullptr, 16);
    return true;
}

std::vector<TcpRow> tcpRows() {
    std::vector<TcpRow> out;
    for (int v6 = 0; v6 < 2; v6++) {
        std::ifstream f(v6 ? "/proc/net/tcp6" : "/proc/net/tcp");
        std::string line;
        std::getline(f, line);   // the heading
        while (std::getline(f, line)) {
            std::istringstream in(line);
            std::string sl, local, remote, st, txrx, trtm, retr, uid, timeout;
            TcpRow r;
            r.v6 = v6;
            if (!(in >> sl >> local >> remote >> st >> txrx >> trtm >> retr >> uid >> timeout >> r.inode)) continue;
            if (!parseEndpoint(local, v6, r.local, &r.localPort) || !parseEndpoint(remote, v6, r.remote, &r.remotePort))
                continue;
            r.state = (int)std::strtol(st.c_str(), nullptr, 16);
            out.push_back(r);
        }
    }
    return out;
}

// The socket inodes a process holds; empty for one not the user's.
std::set<unsigned long> socketsOf(const std::string& pid) {
    std::set<unsigned long> out;
    const std::string dir = "/proc/" + pid + "/fd";
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        char target[64];
        const ssize_t n = readlink((dir + "/" + e->d_name).c_str(), target, sizeof target - 1);
        if (n <= 0) continue;
        target[n] = 0;
        unsigned long inode = 0;
        if (std::sscanf(target, "socket:[%lu]", &inode) == 1) out.insert(inode);
    }
    closedir(d);
    return out;
}

bool loopback(const TcpRow& r) {
    if (!r.v6) return r.remote[0] == 127;
    struct in6_addr a;
    std::memcpy(&a, r.remote, 16);
    return IN6_IS_ADDR_LOOPBACK(&a) || (IN6_IS_ADDR_V4MAPPED(&a) && r.remote[12] == 127);
}

std::string addressOf(const TcpRow& r) {
    char buf[INET6_ADDRSTRLEN] = "";
    struct in6_addr a6;
    std::memcpy(&a6, r.remote, 16);
    if (!r.v6) inet_ntop(AF_INET, r.remote, buf, sizeof buf);
    else if (IN6_IS_ADDR_V4MAPPED(&a6)) inet_ntop(AF_INET, r.remote + 12, buf, sizeof buf);
    else inet_ntop(AF_INET6, r.remote, buf, sizeof buf);
    return buf;
}

}  // namespace

// ---------------------------------------------------------------- statics

std::string SessionBrowser::stateFile() {
    gchar* p = g_build_filename(g_get_user_state_dir(), "mutible", "session-browser.json", nullptr);
    std::string out = p;
    g_free(p);
    return out;
}

std::vector<SessionBrowser::Remembered> SessionBrowser::remembered() {
    std::lock_guard<std::mutex> lk(gStateMu);
    const Json state = readState();
    const Json& list = state["remembered"];
    std::vector<Remembered> out;
    for (size_t i = 0; list.isArray() && i < list.size(); i++) {
        const Json& e = list[i];
        if (!e.isObject() || e["id"].asString().empty()) continue;
        Remembered r;
        r.id = e["id"].asString();
        r.browser = e["browser"].asString();
        r.address = e["address"].asString();
        r.since = (std::int64_t)e["since"].asNumber();
        out.push_back(std::move(r));
    }
    return out;
}

void SessionBrowser::forgetAll() {
    std::lock_guard<std::mutex> lk(gStateMu);
    Json state = readState();
    if (!state.has("remembered")) return;
    state.set("remembered", Json::array());
    writeState(state);
}

bool SessionBrowser::editableChoice() {
    std::lock_guard<std::mutex> lk(gStateMu);
    const Json state = readState();
    return state["editable"].isBool() ? state["editable"].asBool() : true;
}

void SessionBrowser::setEditableChoice(bool editable) {
    std::lock_guard<std::mutex> lk(gStateMu);
    Json state = readState();
    state.set("editable", Json(editable));
    writeState(state);
}

void SessionBrowser::showRemembered(GtkWindow* parent) {
    const std::vector<Remembered> list = remembered();
    GtkAlertDialog* dlg = nullptr;
    if (list.empty()) {
        dlg = gtk_alert_dialog_new("No browsers are remembered");
        gtk_alert_dialog_set_detail(dlg, "When mutible asks whether a browser may open a window's session in the "
                                         "browser, Always Allow remembers it, and it is listed here.");
        const char* buttons[] = {"OK", nullptr};
        gtk_alert_dialog_set_buttons(dlg, buttons);
        gtk_alert_dialog_show(dlg, parent);
        g_object_unref(dlg);
        return;
    }
    std::string detail;
    for (const Remembered& r : list) {
        std::string line = r.browser.empty() ? "A browser" : r.browser;
        if (!r.address.empty()) line += ", from " + r.address;
        if (r.since > 0)
            if (GDateTime* t = g_date_time_new_from_unix_local(r.since)) {
                if (gchar* s = g_date_time_format(t, "%-d %b %Y, %H:%M")) {
                    line += std::string(", since ") + s;
                    g_free(s);
                }
                g_date_time_unref(t);
            }
        detail += line + "\n";
    }
    detail += "\nA remembered browser opens a link to a window's session without being asked about. "
              "Forget All asks about each again.";
    const std::string title =
        list.size() == 1 ? "One browser is remembered" : std::to_string(list.size()) + " browsers are remembered";
    dlg = gtk_alert_dialog_new("%s", title.c_str());
    gtk_alert_dialog_set_detail(dlg, detail.c_str());
    const char* buttons[] = {"Done", "Forget All", nullptr};
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_default_button(dlg, 0);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_choose(
        dlg, parent, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer) {
            GError* err = nullptr;
            const int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, &err);
            if (err) g_error_free(err);
            else if (choice == 1) forgetAll();
        },
        nullptr);
    g_object_unref(dlg);
}

void SessionBrowser::openInBrowser(GtkWindow* parent, const std::string& url) {
    if (opener) {
        opener(url);
        return;
    }
    GtkUriLauncher* launcher = gtk_uri_launcher_new(url.c_str());
    gtk_uri_launcher_launch(
        launcher, parent, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer) {
            GError* err = nullptr;
            if (!gtk_uri_launcher_launch_finish(GTK_URI_LAUNCHER(src), res, &err) && err) {
                g_printerr("mutible: could not open the session's page: %s\n", err->message);
                g_error_free(err);
            }
        },
        nullptr);
    g_object_unref(launcher);
}

std::string SessionBrowser::peerOf(int peerPort, int serverPort) {
    const std::vector<TcpRow> rows = tcpRows();
    // The socket at the other end: from `peerPort` to the server's port.
    unsigned long inode = 0;
    for (const TcpRow& r : rows)
        if (r.localPort == peerPort && r.remotePort == serverPort && r.inode) inode = r.inode;
    if (!inode) return "this computer";
    // Which of the user's processes holds it.
    const std::string self = std::to_string(getpid());
    DIR* d = opendir("/proc");
    if (!d) return "this computer";
    std::string holder;
    std::set<unsigned long> held;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9' || self == e->d_name) continue;
        std::set<unsigned long> s = socketsOf(e->d_name);
        if (!s.count(inode)) continue;
        holder = e->d_name;
        held = std::move(s);
        break;
    }
    closedir(d);
    if (holder.empty()) return "this computer";
    std::string comm;
    std::ifstream("/proc/" + holder + "/comm") >> comm;
    if (comm.compare(0, 4, "sshd") != 0) return "this computer";
    // sshd forwarding a tunnel: its own connection's far end.
    for (const TcpRow& r : rows)
        if (r.state == 1 /* ESTABLISHED */ && held.count(r.inode) && !loopback(r)) {
            const std::string a = addressOf(r);
            if (!a.empty()) return a + ", through an SSH tunnel";
        }
    return "another machine, through an SSH tunnel";
}

// ---------------------------------------------------------------- the server

SessionBrowser::SessionBrowser(GtkWindow* window, Host host)
    : window_(window), host_(std::move(host)), guard_(std::make_shared<Guard>()) {
    guard_->self = this;
}

SessionBrowser::~SessionBrowser() {
    stop();
    guard_->self = nullptr;
}

void SessionBrowser::setEditable(bool editable) {
    if (editable == editable_) return;
    stop();   // a server serves one way: the next start serves the other
    editable_ = editable;
}

int SessionBrowser::port() const { return server_ ? server_->port() : 0; }
int SessionBrowser::browsers() const { return server_ ? server_->browsers() : 0; }
std::size_t SessionBrowser::asking() const { return server_ ? server_->access().questions() : 0; }

std::string SessionBrowser::newLink() {
    if (!server_) return std::string();
    url_ = server_->newLink();
    return url_;
}

void SessionBrowser::reportTabs() {
    if (host_.report) host_.report();
    if (server_) server_->lookNow();
}

void SessionBrowser::reloadSettings() {
    if (server_) server_->reloadSettings();
}

bool SessionBrowser::start(const std::shared_ptr<SessionLink::Link>& link, std::uint64_t window, bool showHidden,
                           const std::string& settingsPath, std::string* error) {
    if (server_ && link_ != link) stop();
    if (server_) return true;
    if (!link) {
        if (error) *error = "the window has no session host";
        return false;
    }
    if (link->lost()) {
        if (error) *error = "the window's session host went away";
        return false;
    }
    link_ = link;
    windowId_ = window;
    // The tabs first, so the first browser sees them.
    if (host_.report) host_.report();
    hostWindow_ = link->hostWindowId(window);

    auto server = std::make_unique<Shared::Web::Server>();
    Shared::Web::Options o;
    // The window's host: the process's own, or its folder's headless one
    // over its socket. link_ outlives the server (stop resets it after).
    SessionLink::Link* raw = link.get();
    if (link->kind() == SessionLink::Link::Kind::Socket) o.hostSocket = link->socketPath();
    else o.connect = [raw](Shared::Client& c) { return raw->connectViewer(c); };
    o.folder = link->folder();
    o.window = hostWindow_;
    o.editable = editable_;
    const unsigned generation = ++generation_;
    std::shared_ptr<Guard> guard = guard_;
    // The server's thread to the main loop, for the server that asked only.
    auto later = [guard, generation](std::function<void(SessionBrowser&)> f) {
        onMain([guard, generation, f] {
            SessionBrowser* s = guard->self;
            if (s && s->generation_ == generation && s->server_) f(*s);
        });
    };
    if (editable_) {   // a read-only page changes nothing in the window
        if (host_.save)
            o.save = [later](Shared::DocId d) {
                later([d](SessionBrowser& s) { s.host_.save(d); });
                return true;
            };
        if (host_.open)
            o.open = [later](const std::string& path, bool keep) {
                later([path, keep](SessionBrowser& s) { s.host_.open(path, keep); });
                return true;
            };
        if (host_.close)
            o.close = [later](Shared::DocId d) {
                later([d](SessionBrowser& s) { s.host_.close(d); });
                return true;
            };
        if (host_.browse)
            o.browse = [later](const std::string& url) {
                later([url](SessionBrowser& s) { s.host_.browse(url); });
                return true;
            };
    }
    // A browser this computer does not remember waits for the window's
    // dialog. Who holds the other end is looked up here, off the main loop.
    Shared::Web::Server* asking = server.get();
    o.ask = [later, asking, generation](const Shared::Web::Access::Question& q) {
        const std::string from = peerOf(q.peerPort, asking->port());
        const std::string browser = q.browser.empty() ? "A browser" : q.browser;
        const uint64_t id = q.id;
        const std::string browserId = q.browserId, peer = q.peer;
        later([id, browser, browserId, from, peer, generation](SessionBrowser& s) {
            s.askAbout(id, browser, browserId, from, peer, generation);
        });
    };
#ifdef MUTIBLE_ENABLE_PDF
    // A page showing a LaTeX document: the window says what it has (any
    // page: a typeset reads a copy and changes nothing of the user's).
    if (host_.previewOf && host_.source)
        o.typeset = [later](const std::string& path) {
            later([path](SessionBrowser& s) { s.typesetFor(path); });
        };
    o.typesetPageSizes = pageSizes;
    o.typesetPageRender = renderPage;
#endif
    o.remembered = [](const std::string& browserId) { return isRemembered(browserId); };
    o.showHidden = showHidden;
    o.termNote = kTermNote;
    o.port = 0;   // any free port
    o.settingsPath = settingsPath;
    std::string why;
    if (!server->start(o, &why)) {
        if (error) *error = why;
        link_.reset();
        return false;
    }
    server_ = std::move(server);
    Shared::Web::Server* running = server_.get();
    // Serves until stopped, or until the host goes: then it stops here too,
    // so the next Show starts anew.
    thread_ = std::thread([running, later] {
        if (running->run()) later([](SessionBrowser& s) { s.stop(); });
    });
    timer_ = g_timeout_add_full(
        G_PRIORITY_DEFAULT, 500,
        [](gpointer p) -> gboolean {
            SessionBrowser* s = (*static_cast<std::shared_ptr<Guard>*>(p))->self;
            if (!s || !s->server_) return G_SOURCE_REMOVE;
            s->reportTabs();
            return G_SOURCE_CONTINUE;
        },
        new std::shared_ptr<Guard>(guard_), [](gpointer p) { delete static_cast<std::shared_ptr<Guard>*>(p); });
    return true;
}

// A browser this computer does not remember is opening the page: a dialog
// on the window asks, and the answer goes to the server that asked.
void SessionBrowser::askAbout(std::uint64_t question, const std::string& browser, const std::string& browserId,
                              const std::string& from, const std::string& peer, unsigned generation) {
    if (generation != generation_ || !server_) return;
    struct Pending {
        std::shared_ptr<Guard> guard;
        unsigned generation;
        std::uint64_t question;
        std::string browser, browserId, from;
    };
    const std::string where = from == "this computer" ? "this computer (" + peer + ")" : from;
    const std::string detail =
        "A browser is opening the link to this window's session from " + where + ". Allow lets it " +
        (editable_ ? "see and edit" : "see") +
        " this window's files and terminals; Deny turns it away. Always Allow also remembers this "
        "browser, so its later links open without asking (View > Remembered Browsers… forgets it).";
    const std::string title = "Allow " + browser + " to open this window's session?";
    auto* pending = new Pending{guard_, generation, question, browser, browserId, from};
    std::function<void()> show = [pending, title, detail] {
        SessionBrowser* s = pending->guard->self;
        if (!s || pending->generation != s->generation_ || !s->server_) {
            delete pending;
            return;
        }
        GtkAlertDialog* dlg = gtk_alert_dialog_new("%s", title.c_str());
        gtk_alert_dialog_set_detail(dlg, detail.c_str());
        const char* buttons[] = {"Allow", "Deny", "Always Allow", nullptr};
        gtk_alert_dialog_set_buttons(dlg, buttons);
        gtk_alert_dialog_set_default_button(dlg, 0);
        gtk_alert_dialog_set_cancel_button(dlg, 1);
        gtk_alert_dialog_set_modal(dlg, TRUE);
        gtk_alert_dialog_choose(
            dlg, s->window_, nullptr,
            [](GObject* src, GAsyncResult* res, gpointer p) {
                Pending* pd = static_cast<Pending*>(p);
                GError* err = nullptr;
                const int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, &err);
                if (err) g_error_free(err);
                const bool allow = !err && (choice == 0 || choice == 2);
                if (!err && choice == 2) remember(pd->browserId, pd->browser, pd->from);
                SessionBrowser* t = pd->guard->self;
                if (t && pd->generation == t->generation_ && t->server_) t->server_->answer(pd->question, allow);
                delete pd;
            },
            pending);
        g_object_unref(dlg);
    };
    if (askDelay) askDelay(std::move(show));
    else show();
}

void SessionBrowser::gitChanged() {
    if (server_) server_->gitChanged();
}

void SessionBrowser::typesetOf(const std::string& path, GBytes* pdf, const std::string& error) {
    if (!server_ || path.empty()) return;
    if (pdf) {
        gsize n = 0;
        const char* d = static_cast<const char*>(g_bytes_get_data(pdf, &n));
        server_->typesetReady(path, std::make_shared<const std::string>(d ? d : "", n));
    } else if (!error.empty()) {
        server_->typesetFailed(path, error);
    } else {
        server_->typesetWorking(path);
    }
}

#ifdef MUTIBLE_ENABLE_PDF
// A typeset the server made for a page with no tab's preview: a text buffer
// of its own under a LatexPreview that is never shown.
struct SessionBrowser::Hidden {
    std::string path;
    GtkTextBuffer* buffer = nullptr;
    LatexPreview* preview = nullptr;
    ~Hidden() {
        delete preview;   // before the buffer it reads
        if (buffer) g_object_unref(buffer);
    }
};

static const size_t kHiddenTypesets = 4;

// What `preview` has: the last PDF first, so a page has it to show while
// another typeset runs, then working or failed.
void SessionBrowser::tellPreview(const std::string& path, LatexPreview* preview) {
    GBytes* pdf = preview->typesetPdf();
    const std::string failure = preview->typesetFailure();
    if (pdf) typesetOf(path, pdf, "");
    if (preview->typesetting() || (!pdf && failure.empty())) {
        typesetOf(path, nullptr, "");
        if (!preview->typesetting()) preview->compileNow();   // nothing was running: begin
    } else if (!failure.empty()) {
        typesetOf(path, nullptr, failure);
    }
}

void SessionBrowser::typesetFor(const std::string& path) {
    if (!server_ || path.empty()) return;
    if (LatexPreview* l = host_.previewOf(path)) {
        tellPreview(path, l);
        return;
    }
    std::string text;
    if (!host_.source(path, &text) || !g_utf8_validate(text.data(), (gssize)text.size(), nullptr)) {
        server_->typesetFailed(path, "The file could not be read as UTF-8 text.");
        return;
    }
    std::shared_ptr<Hidden> h;
    for (size_t i = 0; i < hidden_.size(); i++)
        if (hidden_[i]->path == path) {
            h = hidden_[i];
            hidden_.erase(hidden_.begin() + (long)i);   // now the newest
            break;
        }
    if (!h) {
        while (hidden_.size() >= kHiddenTypesets) hidden_.erase(hidden_.begin());   // the oldest, stopped
        h = std::make_shared<Hidden>();
        h->path = path;
        h->buffer = gtk_text_buffer_new(nullptr);
        gtk_text_buffer_set_text(h->buffer, text.data(), (gint)text.size());
        h->preview = new LatexPreview(h->buffer);
        h->preview->setPath(path);
        h->preview->setOnTypeset([this](const std::string& p, GBytes* pdf, const std::string& error) {
            typesetOf(p, pdf, error);
        });
        hidden_.push_back(h);
        h->preview->compileNow();   // at once, as a tab's preview does when it is made
        return;
    }
    hidden_.push_back(h);
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(h->buffer, &a, &b);
    char* old = gtk_text_buffer_get_text(h->buffer, &a, &b, FALSE);
    const bool same = text == old;
    g_free(old);
    if (!same) {   // typesets again only when the text changed
        gtk_text_buffer_set_text(h->buffer, text.data(), (gint)text.size());
        h->preview->compileNow();
    }
    tellPreview(path, h->preview);
}

void SessionBrowser::stopHidden() { hidden_.clear(); }
#else
void SessionBrowser::typesetFor(const std::string&) {}
void SessionBrowser::tellPreview(const std::string&, LatexPreview*) {}
void SessionBrowser::stopHidden() {}
#endif

void SessionBrowser::stop() {
    stopHidden();
    if (timer_) {
        g_source_remove(timer_);
        timer_ = 0;
    }
    if (server_) {
        server_->stop();
        if (thread_.joinable()) thread_.join();
        server_.reset();   // detaches the view from the host
    }
    link_.reset();
    hostWindow_ = 0;
    url_.clear();
}
