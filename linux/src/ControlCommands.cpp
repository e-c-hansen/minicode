// ControlCommands.cpp — see ControlCommands.h.
#include "ControlCommands.h"
#include "TerminalAttention.h"

#include <gtk/gtk.h>

#include <climits>
#include <fcntl.h>
#include <cstring>
#include <memory>
#include <thread>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#include "Document.h"
#include "Browser.h"
#include "BrowserPages.h"
#include "Keys.h"
#include "PageCheck.h"
#include "TermLinks.h"
#include "RunFile.h"
#include "AgentNotes.h"
#include "Editor.h"
#include "GitRun.h"
#include "ProcessMonitor.h"

using ControlChannel::Dispatcher;
using ControlChannel::Request;
using ControlChannel::Result;

namespace ControlCommands {

long offsetOfLineColumn(const std::string& text, int line, int column) {
    long offset = 0;   // in characters
    int at = 1;
    size_t i = 0;
    auto count = [&](size_t upTo) {
        for (; i < upTo; i++)
            if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) offset++;
    };
    while (at < line) {
        const size_t nl = text.find('\n', i);
        if (nl == std::string::npos) {   // past the last line: the end
            count(text.size());
            return offset;
        }
        count(nl + 1);
        at++;
    }
    for (int c = 1; c < column && i < text.size() && text[i] != '\n'; c++) {
        offset++;
        i++;
        while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) i++;
    }
    return offset;
}

}  // namespace ControlCommands

namespace {


std::string resolved(const std::string& path) {
    char buf[PATH_MAX];
    return realpath(path.c_str(), buf) ? std::string(buf) : path;
}

bool isDirectory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isRegularFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool inside(const ControlWindow& w, const std::string& path) {
    const std::string root = resolved(w.folder), real = resolved(path);
    const std::string prefix = root == "/" ? root : root + "/";
    return real.compare(0, prefix.size(), prefix) == 0;
}

// `path` spelled from the window's root when it lies inside it (the tree
// finds a file by that spelling), else as it is.
std::string spelledFrom(const ControlWindow& w, const std::string& path) {
    const std::string root = resolved(w.folder), real = resolved(path);
    const std::string prefix = root == "/" ? root : root + "/";
    if (real.compare(0, prefix.size(), prefix) == 0)
        return (w.folder == "/" ? std::string() : w.folder) + "/" + real.substr(prefix.size());
    return real;
}

const ControlWindow* findById(const std::vector<ControlWindow>& ws, int id) {
    for (const ControlWindow& w : ws)
        if (w.id == id) return &w;
    return nullptr;
}

const ControlWindow* findRootedAt(const std::vector<ControlWindow>& ws, const std::string& dir) {
    const std::string want = resolved(dir);
    for (const ControlWindow& w : ws)
        if (resolved(w.folder) == want) return &w;
    return nullptr;
}

// The window --window or --folder names (a window is made for a folder none
// shows), else the front one. False, with `error`, when there is none.
bool target(const ControlHost& host, const Json& args, ControlWindow* out, std::string* error) {
    std::vector<ControlWindow> ws = host.windows();
    if (args.has("window")) {
        const ControlWindow* w = findById(ws, (int)args["window"].asInt());
        if (!w) {
            *error = "there is no window " + std::to_string(args["window"].asInt());
            return false;
        }
        *out = *w;
        return true;
    }
    if (args.has("folder")) {
        const std::string dir = resolved(args["folder"].asString());
        if (!isDirectory(dir)) {
            *error = "not a folder: " + dir;
            return false;
        }
        if (const ControlWindow* w = findRootedAt(ws, dir)) {
            *out = *w;
            return true;
        }
        if (!host.openWindow(dir, out)) {
            *error = "could not open a window on " + dir;
            return false;
        }
        return true;
    }
    if (ws.empty()) {
        *error = "no window is open";
        return false;
    }
    const ControlWindow* f = findById(ws, host.frontId());
    *out = f ? *f : ws.back();
    return true;
}

Json windowResult(const ControlWindow& w) {
    Json r = Json::object();
    r.set("window", w.id);
    return r;
}

// The keyboard is in the editor's text: the user is typing there, and a
// file an agent opens must not take the place from under them.
bool userIsInDocument(const ControlWindow& w) {
    if (!w.window || !w.editor || !gtk_window_is_active(w.window)) return false;
    GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(w.window));
    return focus && focus == w.editor->textView();
}

// ---- diff

constexpr off_t kMaxDiffBytes = 16 * 1024 * 1024;

// A file's text for `diff`, off the main thread: a plain file of up to 16 MB
// of UTF-8. Opened without blocking and checked again once open, so a FIFO
// put in its place meanwhile cannot hang the read.
bool readDiffText(const std::string& path, std::string* text, std::string* error) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        *error = "no such file: " + path;
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        *error = "not a plain file: " + path;
        return false;
    }
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0) close(fd);
        *error = "could not read " + path;
        return false;
    }
    if (st.st_size > kMaxDiffBytes) {
        close(fd);
        *error = path + " is over 16 MB";
        return false;
    }
    text->clear();
    char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        text->append(buf, (size_t)n);
        if ((off_t)text->size() > kMaxDiffBytes) break;
    }
    close(fd);
    if (n < 0 || (off_t)text->size() > kMaxDiffBytes) {
        *error = n < 0 ? "could not read " + path : path + " is over 16 MB";
        return false;
    }
    if (!g_utf8_validate(text->data(), (gssize)text->size(), nullptr)) {
        *error = path + " is not UTF-8 text";
        return false;
    }
    return true;
}

// The file as git has it at `ref`: false with `error` set when git cannot
// say; `missing` when the commit exists and the file is not in it.
bool textAtRef(const std::string& path, const std::string& ref, std::string* text, bool* missing,
               std::string* error) {
    const size_t slash = path.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
    const std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    GitResult top = runGit(dir, {"rev-parse", "--show-toplevel"});
    if (!top.ok()) {
        *error = gitExecutable().empty() ? "git is not installed"
                                         : name + " is not in a git repository";
        return false;
    }
    if (!runGit(dir, {"rev-parse", "--verify", "--quiet", ref + "^{commit}"}).ok()) {
        *error = "no commit named " + ref;
        return false;
    }
    GitResult show = runGit(dir, {"show", ref + ":./" + name});
    if (!show.ok()) {
        *missing = true;   // the commit exists; the file is not in it
        return true;
    }
    if (!g_utf8_validate(show.out.data(), (gssize)show.out.size(), nullptr)) {
        *error = name + " at " + ref + " is not text";
        return false;
    }
    *text = show.out;
    return true;
}

std::string leaf(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string shortPath(const ControlWindow& w, const std::string& path) {
    const std::string root = resolved(w.folder), real = resolved(path);
    const std::string prefix = root == "/" ? root : root + "/";
    if (root.size() > 1 && real.compare(0, prefix.size(), prefix) == 0)
        return real.substr(prefix.size());
    const char* home = getenv("HOME");
    if (home && *home && path.compare(0, strlen(home) + 1, std::string(home) + "/") == 0)
        return "~" + path.substr(strlen(home));
    return path;
}

const char* kindName(const Document* d) {
    switch (d->kind()) {
    case Document::Kind::Text: return "text";
    case Document::Kind::Message: return "message";
    case Document::Kind::Media: return "media";
    case Document::Kind::Database: return "database";
    case Document::Kind::Diff: return "diff";
    }
    return "text";
}

std::string baseName(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The selection on show, or a null: the active tab's text.
Json selectionOf(const ControlWindow& w) {
    Editor* ed = w.editor;
    Document* d = ed ? ed->active() : nullptr;
    if (!d || d->path().empty() || d->kind() != Document::Kind::Text) return Json();
    GtkTextBuffer* b = ed->sourceBuffer();
    if (!b) return Json();
    GtkTextIter a, e;
    if (!gtk_text_buffer_get_selection_bounds(b, &a, &e)) {
        gtk_text_buffer_get_iter_at_mark(b, &a, gtk_text_buffer_get_insert(b));
        e = a;
    }
    char* raw = gtk_text_buffer_get_text(b, &a, &e, TRUE);
    std::string text = raw ? raw : "";
    g_free(raw);
    bool cut = false;
    if (text.size() > ControlChannel::kMaxSelectionBytes) {
        text = ControlChannel::clipUtf8(text, ControlChannel::kMaxSelectionBytes);
        cut = true;
    }
    Json sel = Json::object();
    sel.set("path", d->path());
    sel.set("text", text);
    Json s = Json::object(), n = Json::object();
    s.set("line", gtk_text_iter_get_line(&a) + 1);
    s.set("column", gtk_text_iter_get_line_offset(&a) + 1);
    n.set("line", gtk_text_iter_get_line(&e) + 1);
    n.set("column", gtk_text_iter_get_line_offset(&e) + 1);
    sel.set("start", s);
    sel.set("end", n);
    sel.set("preview", d->showsPreview());
    sel.set("truncated", cut);
    return sel;
}

Json windowState(const ControlWindow& w, bool front) {
    Json o = Json::object();
    o.set("id", w.id);
    o.set("folder", w.folder);
    o.set("front", front);
    Json tabs = Json::array();
    Document* active = w.editor ? w.editor->active() : nullptr;
    if (w.editor)
        for (Document* d : w.editor->documents()) {
            Json t = Json::object();
            t.set("path", d->path().empty() ? Json() : Json(d->path()));
            t.set("title", baseName(d->path()));
            t.set("kind", kindName(d));
            t.set("dirty", d->dirty());
            t.set("active", d == active);
            t.set("preview", d->isPreviewTab());
            tabs.push(t);
        }
    o.set("tabs", tabs);
    o.set("selection", selectionOf(w));
    if (AgentNotes* n = w.editor ? w.editor->notes() : nullptr) {
        if (n->shown()) {
            Json note = Json::object();
            note.set("text", n->text());
            note.set("label", n->label());
            note.set("line", n->firstLine());
            note.set("to_line", n->lastLine());
            o.set("note", note);
        } else {
            o.set("note", Json());
        }
        if (n->tourActive()) {
            Json t = Json::object();
            t.set("stop", n->tourIndex() + 1);
            t.set("of", n->tourCount());
            o.set("tour", t);
        } else {
            o.set("tour", Json());
        }
    }
    // The terminal's own commands are later bites of 3.1; what it reports is
    // its id (the window's), its Claude Code mark (PARITY.md 3.6) and what
    // the shell integration knows.
    Json terminals = Json::array();
    {
        Json t = Json::object();
        t.set("id", w.id);
        const char* mark = TerminalAttention::markName(w.id);
        t.set("attention", *mark ? Json(std::string(mark)) : Json());
        // The shell integration's (D10): whether a command runs and its
        // text, and the last command's status; all null while the shell
        // has not marked (another shell, ssh, or before its first prompt),
        // since unknown is not idle.
        const ShellIntegration::ShellState& sh = w.shell;
        t.set("busy", sh.known ? Json(sh.busy) : Json());
        t.set("command", sh.known && sh.busy && !sh.command.empty() ? Json(sh.command) : Json());
        t.set("status", sh.known && sh.status >= 0 ? Json(sh.status) : Json());
        terminals.push(t);
    }
    o.set("terminals", terminals);
    o.set("browser", Json());
    return o;
}

// The last file of `open --note`: in the agent's preview tab (a kept tab with
// --keep), beside the user's tab, unshown, while they type; the note is placed
// when it shows (AgentNotes.h).
bool openNoted(const ControlWindow& w, const std::string& path, const Json& a, int line,
               int column, bool* background, std::string* error) {
    Editor* ed = w.editor;
    const bool beside = userIsInDocument(w) &&
                        !(ed->active() && resolved(ed->active()->path()) == resolved(path));
    Document* d = ed->openForAgent(path, !a["keep"].asBool(), beside);
    if (!d) {
        *error = "could not open " + path;
        return false;
    }
    ed->notes()->show(d, ControlChannel::cleanNoteText(a["note"].asString()),
                      a["from"].asString(), line > 0 ? line : 1, column,
                      (int)a["to_line"].asInt(0), (int)a["to_column"].asInt(0));
    if (beside) *background = true;
    return true;
}

// One file of `open`: the tab, and the caret or selection asked for. The
// file is shown unless the user is typing in the editor, then it opens in a
// tab beside theirs (`background`).
bool openOne(const ControlHost& host, const ControlWindow& w, const std::string& path, int line,
             int column, int toLine, int toColumn, bool* background, std::string* error) {
    Editor* ed = w.editor;
    const bool beside = userIsInDocument(w) &&
                        !(ed->active() && resolved(ed->active()->path()) == resolved(path));
    // The characters the place stands for, read from the file, for a tab not
    // on show (or a range, which the tab is given as its selection).
    long selStart = 0, selEnd = 0;
    const bool wantsPlace = line > 0 || toLine > 0;
    if (wantsPlace) {
        std::string text;
        if (Document::readText(path, text) == Document::Read::Ok) {
            selStart = ControlCommands::offsetOfLineColumn(text, line > 0 ? line : 1, column);
            selEnd = toLine > 0 ? ControlCommands::offsetOfLineColumn(
                                      text, toLine, toColumn > 0 ? toColumn : INT_MAX)
                                : selStart;
            if (selEnd < selStart) selEnd = selStart;
        }
    }
    EditorTabs::ClosedTab place;
    place.path = path;
    place.selStart = selStart;
    place.selEnd = selEnd;
    if (beside) {
        Document* d = ed->documentFor(path);
        if (!d) d = ed->addTab(path);
        if (!d) {
            *error = "could not open " + path;
            return false;
        }
        if (wantsPlace) ed->placeTab(d, place);
        *background = true;
        return true;
    }
    host.showFile(w, path);
    Document* d = ed->documentFor(path);
    if (!d || d != ed->active()) {
        *error = "could not open " + path;
        return false;
    }
    if (toLine > 0) {
        ed->placeTab(d, place);
        if (!d->loading() && d->kind() == Document::Kind::Text)
            gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(ed->textView()),
                                         gtk_text_buffer_get_insert(ed->sourceBuffer()), 0.1, TRUE,
                                         0.0, 0.3);
    } else if (line > 0) {
        ed->revealLineColumn(line, column);
    }
    return true;
}


#ifdef MINICODE_ENABLE_BROWSER
// ---- browse and page (PARITY.md 3.7)

// Whether an agent may read the page at `url`.
bool mayRead(const ControlHost& host, const std::string& url, std::string* error) {
    if ((host.pageAnyOrigin && host.pageAnyOrigin()) || PageCheck::isLocalAddress(url)) return true;
    *error = PageCheck::originError(url);
    return false;
}

Json pageResult(PageProbe* probe, bool agent) {
    Json r = probe->stateJson();
    r.set("page", agent ? "agent" : "yours");
    if (agent) r.set("console_seq", probe->console().lastSeq());
    return r;
}

bool keyboardInPane(const ControlWindow& w) {
    if (!w.window || !w.browser) return false;
    GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(w.window));
    return focus && Keys::within(focus, w.browser->widget());
}

// Answers once the load is done (with --wait), or at once.
void answerLoad(PageProbe* probe, bool wait, bool agent, Json extra, Dispatcher::Respond respond) {
    if (!wait) {
        respond(Result::success(extra));
        return;
    }
    probe->waitForLoad([probe, agent, extra, respond](const std::string& error, bool timedOut) {
        if (!error.empty()) {
            const std::string u = probe->url();
            respond(Result::failure(error + (u.empty() ? "" : " (" + u + ")")));
            return;
        }
        Json r = pageResult(probe, agent);
        if (timedOut) r.set("timed_out", true);
        if (extra["pane_busy"].asBool()) r.set("pane_busy", true);
        respond(Result::success(r));
    });
}

void registerPageCommands(Dispatcher& d, const ControlHost& host) {
    d.on("browse", [host](const Request& req, Dispatcher::Respond respond) {
        const Json& a = req.args;
        if (a["split"].asString() == "below") {
            respond(Result::failure("the browser opens beside the editor: use --right"));
            return;
        }
        std::string url, error;
        auto exists = [](const std::string& p) {
            struct stat st;
            return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
        };
        if (!PageCheck::agentAddress(a["url"].asString(), exists, url, error)) {
            respond(Result::failure(error));
            return;
        }
        ControlWindow w;
        if (!target(host, a, &w, &error)) {
            respond(Result::failure(error));
            return;
        }
        if (!w.browser) {
            respond(Result::failure("this window has no browser pane"));
            return;
        }
        PageProbe* probe = w.browser->openAgentPage();
        if (!probe->load(url)) {
            respond(Result::failure("could not load " + url));
            return;
        }
        Json extra = windowResult(w);
        extra.set("page", "agent");
        // The pane shows the agent page unless the user is typing in it.
        if (keyboardInPane(w)) {
            extra.set("pane_busy", true);
        } else {
            w.browser->showAgentPage(true);
            if (host.showBrowser) host.showBrowser(w);
        }
        answerLoad(probe, a["wait"].asBool(), true, extra, respond);
    });

    d.on("page", [host](const Request& req, Dispatcher::Respond respond) {
        const Json& a = req.args;
        const std::string action = a["action"].asString();
        if (action != "url" && action != "text" && action != "console" &&
            action != "screenshot" && action != "reload") {
            respond(Result::failure("page does url, text, console, screenshot or reload, not \"" +
                                    action + "\""));
            return;
        }
        std::string error;
        ControlWindow w;
        if (!target(host, a, &w, &error)) {
            respond(Result::failure(error));
            return;
        }
        const bool yours = a["yours"].asBool();
        Browser* b = w.browser;
        PageProbe* probe = !b ? nullptr : yours ? b->userProbe() : b->agentProbe();
        if (!probe) {
            respond(Result::failure(yours ? "the browser pane is not open"
                                          : "there is no agent page: minicode browse opens one"));
            return;
        }
        if (action == "url") {
            respond(Result::success(pageResult(probe, !yours)));
            return;
        }
        if (action == "reload") {
            if (yours && !mayRead(host, probe->url(), &error)) {
                respond(Result::failure(error));
                return;
            }
            if (!probe->reload()) {
                respond(Result::failure("there is no page to reload"));
                return;
            }
            Json extra = windowResult(w);
            if (!yours && keyboardInPane(w)) extra.set("pane_busy", true);
            else if (!yours) b->showAgentPage(true);
            answerLoad(probe, a["wait"].asBool(), !yours, extra, respond);
            return;
        }
        if (!mayRead(host, probe->url(), &error)) {
            respond(Result::failure(error));
            return;
        }
        const std::string readFrom = probe->url();
        // The page may have moved on, or gone, while it was read: what was
        // read must still be from a page the agent may read. The pane is
        // found again by window id, since the page may have been closed.
        const int windowId = w.id;
        const bool wantAgent = !yours;
        auto stillAllowed = [host, windowId, wantAgent, readFrom](std::string* why) {
            for (const ControlWindow& cw : host.windows()) {
                if (cw.id != windowId || !cw.browser) continue;
                PageProbe* p = wantAgent ? cw.browser->agentProbe() : cw.browser->userProbe();
                if (!p) break;
                return p->url() == readFrom || mayRead(host, p->url(), why);
            }
            *why = "the page was closed";
            return false;
        };

        if (action == "console") {
            if (yours) {
                respond(Result::failure("the console is recorded for the agent page only"));
                return;
            }
            Json entries = Json::array();
            for (const PageCheck::ConsoleEntry& e : probe->console().since(a["since"].asInt(0)))
                entries.push(PageCheck::entryJson(e));
            Json r = Json::object();
            r.set("entries", entries);
            r.set("last", probe->console().lastSeq());
            if (probe->console().dropped()) r.set("dropped", (int64_t)probe->console().dropped());
            respond(Result::success(r));
            return;
        }
        if (action == "text") {
            probe->readText([respond, stillAllowed](const std::string& text, bool cut, const std::string& readError) {
                std::string why;
                if (!readError.empty()) {
                    respond(Result::failure("could not read the page: " + readError));
                    return;
                }
                if (!stillAllowed(&why)) {
                    respond(Result::failure(why));
                    return;
                }
                Json r = Json::object();
                r.set("text", text);
                if (cut) r.set("truncated", true);
                respond(Result::success(r));
            });
            return;
        }
        // screenshot
        const std::string path = a["path"].asString();
        const bool overwrite = a["overwrite"].asBool();
        struct stat st;
        PageCheck::PathAt at = PageCheck::PathAt::Nothing;
        if (!path.empty() && lstat(path.c_str(), &st) == 0)
            at = S_ISDIR(st.st_mode)   ? PageCheck::PathAt::Folder
                 : S_ISREG(st.st_mode) ? PageCheck::PathAt::File
                                       : PageCheck::PathAt::Other;
        if (!PageCheck::screenshotPath(path, at, overwrite, error)) {
            respond(Result::failure(error));
            return;
        }
        probe->snapshotTo(path, a["full"].asBool(), overwrite,
                          [respond, stillAllowed, path](int width, int height, bool cut, const std::string& snapError) {
            std::string why;
            if (!snapError.empty()) {
                respond(Result::failure("no screenshot: " + snapError));
                return;
            }
            if (!stillAllowed(&why)) {
                unlink(path.c_str());
                respond(Result::failure(why));
                return;
            }
            Json r = Json::object();
            r.set("path", path);
            r.set("width", width);
            r.set("height", height);
            if (cut) r.set("cut", true);
            respond(Result::success(r));
        });
    });

    // `minicode open-url`, which BROWSER runs in minicode's terminals: a web
    // address in the user's page, in the window of the terminal the program
    // runs in (a window has one terminal, so its id), else the front one.
    d.on("open-url", [host](const Request& req, Dispatcher::Respond respond) {
        const Json& a = req.args;
        const std::string url = TermLinks::browserAddress(a["url"].asString());
        TermLinks::Link link;
        if (!TermLinks::hyperlink(url, "", link)) {
            respond(Result::failure("open-url takes an http, https or file address, not " + url));
            return;
        }
        std::string error;
        ControlWindow w;
        Json t = a;
        if (a.has("terminal") && !a.has("window")) t.set("window", a["terminal"].asInt());
        if (!target(host, t, &w, &error)) {
            respond(Result::failure(error));
            return;
        }
        if (!w.browser || !host.openUrl) {
            respond(Result::failure("this window has no browser pane"));
            return;
        }
        host.openUrl(w, url);
        Json r = windowResult(w);
        r.set("page", "yours");
        r.set("url", url);
        respond(Result::success(r));
    });
}
#endif  // MINICODE_ENABLE_BROWSER

}  // namespace

void registerControlCommands(Dispatcher& d, ControlHost host) {
    d.on("ping", [](const Request&, Dispatcher::Respond respond) {
        Json r = Json::object();
        r.set("pid", (int)getpid());
#ifdef MINICODE_VERSION
        r.set("version", MINICODE_VERSION);
#else
        r.set("version", "");
#endif
        respond(Result::success(r));
    });

    d.on("window", [host](const Request& req, Dispatcher::Respond respond) {
        const Json& a = req.args;
        ControlWindow w;
        std::vector<ControlWindow> ws = host.windows();
        const ControlWindow* f = findById(ws, host.frontId());
        bool ok = true;
        if (a.has("folder")) {
            const std::string dir = resolved(a["folder"].asString());
            if (!isDirectory(dir)) {
                respond(Result::failure("not a folder: " + dir));
                return;
            }
            const ControlWindow* have = a["new"].asBool() ? nullptr : findRootedAt(ws, dir);
            if (have) w = *have;
            else ok = host.openWindow(dir, &w);
        } else if (a["new"].asBool()) {
            ok = host.openWindow(f ? f->folder : std::string(g_get_home_dir()), &w);
        } else if (!ws.empty()) {
            w = f ? *f : ws.back();
        } else {
            respond(Result::failure("no window is open"));
            return;
        }
        if (!ok) {
            respond(Result::failure("could not open a window"));
            return;
        }
        host.present(w);
        Json r = windowResult(w);
        r.set("folder", w.folder);
        respond(Result::success(r));
    });

    d.on("open", [host](const Request& req, Dispatcher::Respond respond) {
        const Json& a = req.args;
        if (a.has("split")) {
            respond(Result::failure("the editor has one pane: open the file without split"));
            return;
        }
        const Json& files = a["files"];
        if (files.size() == 0) {
            respond(Result::failure("open needs a path"));
            return;
        }
        // Everything is checked before anything opens: a folder, or a plain
        // file (a FIFO or a device would hang the app as it opens).
        for (size_t i = 0; i < files.size(); i++) {
            const std::string p = files[i]["path"].asString();
            struct stat st;
            if (stat(p.c_str(), &st) != 0) {
                respond(Result::failure("no such file or folder: " + p));
                return;
            }
            if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
                respond(Result::failure("not a plain file: " + p));
                return;
            }
        }
        const bool targeted = a.has("window") || a.has("folder");
        ControlWindow forced;
        std::string error;
        if (targeted && !target(host, a, &forced, &error)) {
            respond(Result::failure(error));
            return;
        }
        ControlWindow last;
        bool haveLast = false, besideUser = false;
        Json opened = Json::array();
        for (size_t i = 0; i < files.size(); i++) {
            const std::string path = resolved(files[i]["path"].asString());
            std::vector<ControlWindow> ws = host.windows();
            ControlWindow w;
            if (isDirectory(path)) {   // a folder: the window showing it, as the launcher does
                if (targeted) w = forced;
                else if (const ControlWindow* have = findRootedAt(ws, path)) w = *have;
                else if (!host.openWindow(path, &w)) {
                    respond(Result::failure("could not open a window on " + path));
                    return;
                }
                host.present(w);
                last = w;
                haveLast = true;
                continue;
            }
            bool found = targeted;
            if (targeted) {
                w = forced;
            } else {
                // The window whose folder holds it (the newest first), else
                // the front one, else a window on its folder.
                for (size_t k = ws.size(); k-- > 0 && !found;)
                    if (inside(ws[k], path)) {
                        w = ws[k];
                        found = true;
                    }
                if (!found)
                    if (const ControlWindow* f = findById(ws, host.frontId())) {
                        w = *f;
                        found = true;
                    }
                if (!found) {
                    const size_t slash = path.rfind('/');
                    const std::string dir = slash == 0 ? "/" : path.substr(0, slash);
                    if (!host.openWindow(dir, &w)) {
                        respond(Result::failure("could not open a window on " + dir));
                        return;
                    }
                }
            }
            if (!isRegularFile(path)) {
                respond(Result::failure("not a plain file: " + path));
                return;
            }
            const std::string spelled = spelledFrom(w, path);
            const bool lastFile = i + 1 == files.size();
            bool beside = false;
            const bool noted = lastFile && a.has("note");
            if (noted ? !openNoted(w, spelled, a, (int)files[i]["line"].asInt(0),
                                   (int)files[i]["column"].asInt(0), &beside, &error)
                      : !openOne(host, w, spelled, (int)files[i]["line"].asInt(0),
                                 (int)files[i]["column"].asInt(0),
                                 lastFile ? (int)a["to_line"].asInt(0) : 0,
                                 lastFile ? (int)a["to_column"].asInt(0) : 0, &beside, &error)) {
                respond(Result::failure(error));
                return;
            }
            besideUser = besideUser || beside;
            last = w;
            haveLast = true;
            opened.push(Json(spelled));
        }
        Json r = haveLast ? windowResult(last) : Json::object();
        r.set("opened", opened);
        if (besideUser) r.set("background", true);
        respond(Result::success(r));
    });

    d.on("tour", [host](const Request& req, Dispatcher::Respond respond) {
        const Json& a = req.args;
        const Json& list = a["stops"];
        if (list.size() == 0) {
            respond(Result::failure("a tour needs stops"));
            return;
        }
        std::vector<AgentNotes::Stop> stops;
        for (size_t i = 0; i < list.size(); i++) {
            const std::string p = resolved(list[i]["path"].asString());
            if (!isRegularFile(p)) {
                respond(Result::failure("stop " + std::to_string(i + 1) + ": no such file: " + p));
                return;
            }
            AgentNotes::Stop s;
            s.path = p;
            s.line = (int)list[i]["line"].asInt(1);
            s.column = (int)list[i]["column"].asInt(0);
            s.toLine = (int)list[i]["to"].asInt(0);
            s.note = ControlChannel::cleanNoteText(list[i]["note"].asString());
            stops.push_back(s);
        }
        std::string error;
        ControlWindow w;
        if (a.has("window") || a.has("folder")) {
            if (!target(host, a, &w, &error)) {
                respond(Result::failure(error));
                return;
            }
        } else {
            std::vector<ControlWindow> ws = host.windows();
            bool found = false;
            for (size_t k = ws.size(); k-- > 0 && !found;)
                if (inside(ws[k], stops[0].path)) {
                    w = ws[k];
                    found = true;
                }
            if (!found)
                if (const ControlWindow* f = findById(ws, host.frontId())) {
                    w = *f;
                    found = true;
                }
            if (!found) {
                const size_t slash = stops[0].path.rfind('/');
                const std::string dir = slash == 0 ? "/" : stops[0].path.substr(0, slash);
                if (!host.openWindow(dir, &w)) {
                    respond(Result::failure("could not open a window on " + dir));
                    return;
                }
            }
        }
        for (AgentNotes::Stop& s : stops) s.path = spelledFrom(w, s.path);
        const bool beside = userIsInDocument(w) &&
                            !(w.editor->active() && resolved(w.editor->active()->path()) ==
                                                        resolved(stops[0].path));
        if (!w.editor->notes()->startTour(stops, a["from"].asString(), beside, &error)) {
            respond(Result::failure(error));
            return;
        }
        Json r = windowResult(w);
        r.set("stops", (int)stops.size());
        if (beside) r.set("background", true);
        respond(Result::success(r));
    });

    d.on("state", [host](const Request&, Dispatcher::Respond respond) {
        std::vector<ControlWindow> ws = host.windows();
        const int front = host.frontId();
        Json windows = Json::array();
        for (const ControlWindow& w : ws) windows.push(windowState(w, w.id == front));
        Json r = Json::object();
        r.set("pid", (int)getpid());
        r.set("windows", windows);
        respond(Result::success(r));
    });

    d.on("selection", [host](const Request& req, Dispatcher::Respond respond) {
        ControlWindow w;
        std::string error;
        if (!target(host, req.args, &w, &error)) {
            respond(Result::failure(error));
            return;
        }
        Json r = selectionOf(w);
        if (!r.isObject()) r = Json::object();
        if (!r.has("text")) r.set("text", "");
        r.set("window", w.id);
        respond(Result::success(r));
    });

    // procs (PARITY.md 3.8): the window's terminals' processes and what runs
    // in its folder, from a look taken now. No window is opened for a read.
    d.on("procs", [host](const Request& req, Dispatcher::Respond respond) {
        const Json& a = req.args;
        std::vector<ControlWindow> ws = host.windows();
        const ControlWindow* w = nullptr;
        std::string folder;
        if (a.has("window")) {
            w = findById(ws, (int)a["window"].asInt());
            if (!w) {
                respond(Result::failure("there is no window " + std::to_string(a["window"].asInt())));
                return;
            }
        } else if (a.has("folder")) {
            folder = resolved(a["folder"].asString());
            if (!isDirectory(folder)) {
                respond(Result::failure("not a folder: " + a["folder"].asString()));
                return;
            }
            w = findRootedAt(ws, folder);
        } else {
            w = findById(ws, host.frontId());
            if (!w && !ws.empty()) w = &ws.back();
            if (!w) {
                respond(Result::failure("no window is open; give --folder"));
                return;
            }
        }
        if (w) folder = resolved(w->folder);
        const int window = w ? w->id : 0;
        ProcessMonitor::shared().sampleNow([respond, window, folder] {
            Procs::View v = ProcessMonitor::shared().view(window, folder);
            respond(Result::success(Procs::toJson(v, folder, window)));
        });
    });

    d.on("diff", [host](const Request& req, Dispatcher::Respond respond) {
        const Json& a = req.args;
        const std::string path = resolved(a["path"].asString());
        if (a.has("base") && !ControlChannel::isSafeRef(a["base"].asString())) {
            respond(Result::failure("--base needs a revision, not " + a["base"].asString()));
            return;
        }
        std::string error;
        ControlWindow w;
        bool found = false;
        if (!a.has("window") && !a.has("folder")) {
            std::vector<ControlWindow> ws = host.windows();
            for (size_t k = ws.size(); k-- > 0 && !found;)
                if (inside(ws[k], path)) {
                    w = ws[k];
                    found = true;
                }
        }
        if (!found && !target(host, a, &w, &error)) {
            respond(Result::failure(error));
            return;
        }
        const std::string other = a.has("other") ? resolved(a["other"].asString()) : "";
        const std::string ref = a.has("base") ? a["base"].asString() : "HEAD";
        const std::string shortA = shortPath(w, path), shortB = other.empty() ? "" : shortPath(w, other);
        const int windowId = w.id;
        // The files are read and git asked off the main thread (a big file,
        // or a git slow to answer, would hold it up); the answer waits for
        // that, and the lining up is Editor::showComparison's own thread.
        struct Job {
            ControlHost host;
            Dispatcher::Respond respond;
            int window;
            std::string failure;
            CompareSpec spec;
        };
        Job* job = new Job{host, respond, windowId, "", CompareSpec()};
        std::thread([job, path, other, ref, shortA, shortB] {
            CompareSpec& spec = job->spec;
            std::string right, err;
            if (!readDiffText(path, &right, &err)) {
                job->failure = err;
            } else if (!other.empty()) {
                std::string otherText;
                if (!readDiffText(other, &otherText, &err)) {
                    job->failure = err;
                } else {
                    spec.leftText = right;
                    spec.rightText = otherText;
                    spec.leftLabel = shortA;
                    spec.rightLabel = shortB;
                    spec.path = other;
                    spec.title = leaf(path) + " \xE2\x86\x94 " + leaf(other);
                }
            } else {
                bool missing = false;
                std::string left;
                if (!textAtRef(path, ref, &left, &missing, &err)) {
                    job->failure = err.empty() ? "git failed" : err;
                } else {
                    spec.leftText = left;
                    spec.rightText = right;
                    spec.leftLabel = missing ? ref + ": (not in " + ref + ")" : ref + ": " + shortA;
                    spec.rightLabel = shortA + " (working copy)";
                    spec.path = path;
                    spec.title = leaf(path) + " (" + ref + " \xE2\x86\x94 working copy)";
                }
            }
            g_idle_add(
                [](gpointer p) -> gboolean {
                    std::unique_ptr<Job> j(static_cast<Job*>(p));
                    if (!j->failure.empty()) {
                        j->respond(Result::failure(j->failure));
                        return G_SOURCE_REMOVE;
                    }
                    ControlWindow win;
                    bool have = false;
                    for (const ControlWindow& c : j->host.windows())
                        if (c.id == j->window) {
                            win = c;
                            have = true;
                        }
                    if (!have) {
                        j->respond(Result::failure("the window closed"));
                        return G_SOURCE_REMOVE;
                    }
                    // The user typing in the editor keeps the place: the tab
                    // opens beside theirs.
                    const bool beside = userIsInDocument(win);
                    j->spec.quiet = beside;
                    Dispatcher::Respond respond = j->respond;
                    win.editor->showComparison(j->spec, [respond, win, beside](Document* d) {
                        Json r = windowResult(win);
                        r.set("changed", (int)(d->compare() ? d->compare()->changedRows : 0));
                        if (beside) r.set("background", true);
                        respond(Result::success(r));
                    });
                    return G_SOURCE_REMOVE;
                },
                job);
        }).detach();
    });

    // `minicode notify`: a Claude Code hook (or another tool) marks a
    // terminal as waiting or done, or clears it (PARITY.md 3.6). A window has
    // one terminal here, so its id is the window's.
    d.on("notify", [host](const Request& req, Dispatcher::Respond respond) {
        const Json& a = req.args;
        const int want = (int)a["terminal"].asInt();
        bool found = false;
        for (const ControlWindow& w : host.windows())
            if (w.id == want) found = true;
        if (!found) {
            respond(Result::failure("there is no terminal " + std::to_string(want)));
            return;
        }
        std::string error;
        if (!TerminalAttention::apply(want, a, &error)) {
            respond(Result::failure(error));
            return;
        }
        Json r = Json::object();
        r.set("terminal", want);
        const char* mark = TerminalAttention::markName(want);
        r.set("attention", *mark ? Json(std::string(mark)) : Json());
        r.set("waiting", TerminalAttention::waitingCount());
        respond(Result::success(r));
    });

#ifdef MINICODE_ENABLE_BROWSER
    registerPageCommands(d, host);
#endif

    // `minicode term` (PARITY.md 3.1): a window has one terminal (no tabs
    // yet, item 4.7), so it is the terminal used, when its shell is at a
    // prompt; busy, or not reporting (another shell, ssh), is an error that
    // says why, and nothing is typed. --run goes through the shell's history,
    // typed and entered where the user can see it; --cwd is a `cd` typed the
    // same way (with --run: one line, `cd DIR && COMMAND`).
    d.on("term", [host](const Request& req, Dispatcher::Respond respond) {
        const Json& a = req.args;
        if (a.has("split")) {
            respond(Result::failure("split is not on Linux yet"));
            return;
        }
        ControlWindow w;
        std::string error;
        if (!target(host, a, &w, &error)) {
            respond(Result::failure(error));
            return;
        }
        std::string line = a["run"].asString();
        const std::string cwd = a.has("cwd") ? a["cwd"].asString() : std::string();
        if (!cwd.empty()) {
            const std::string dir = resolved(cwd);
            if (!isDirectory(dir)) {
                respond(Result::failure("not a folder: " + dir));
                return;
            }
            const std::string cd = "cd " + RunFile::shellQuote(dir);
            line = line.empty() ? cd : cd + " && " + line;
        }
        if (!host.terminalRun) {
            respond(Result::failure("this window has no terminal"));
            return;
        }
        const bool titled = a.has("title") && !a["title"].asString().empty();
        const int id = w.id;
        if (line.empty()) {   // no command: show the terminal, if it can be used
            host.showTerminal(w);
            Json r = Json::object();
            r.set("terminal", id);
            r.set("reused", true);
            respond(Result::success(r));
            return;
        }
        host.showTerminal(w);
        host.terminalRun(w, line, [respond, id, titled, line](const std::string& why) {
            if (!why.empty()) {
                respond(Result::failure("the terminal was left alone: " + why));
                return;
            }
            Json r = Json::object();
            r.set("terminal", id);
            r.set("reused", true);   // the window's one terminal, as there are no tabs yet
            r.set("typed", line);
            if (titled) r.set("title_ignored", true);
            respond(Result::success(r));
        });
    });

    d.on("term-output", [host](const Request& req, Dispatcher::Respond respond) {
        const int want = (int)req.args["terminal"].asInt();
        const int64_t lines = req.args["lines"].asInt(200);
        if (lines < 1 || lines > 100000) {
            respond(Result::failure("lines is from 1 to 100000"));
            return;
        }
        std::string text;
        if (!host.terminalOutput || !host.terminalOutput(want, (int)lines, &text)) {
            respond(Result::failure("there is no terminal " + std::to_string(want)));
            return;
        }
        Json r = Json::object();
        r.set("terminal", want);
        r.set("text", text);
        respond(Result::success(r));
    });

    // The rest of the table is later items of PARITY.md 3: a clear answer
    // until each arrives.
    for (const ControlChannel::CommandSpec& spec : ControlChannel::commands()) {
        if (d.handles(spec.name)) continue;
        const std::string name = spec.name;
        d.on(name, [name](const Request&, Dispatcher::Respond respond) {
            respond(Result::failure(name + " is not on Linux yet"));
        });
    }
}
