// ControlCommands.cpp — see ControlCommands.h.
#include "ControlCommands.h"

#include <gtk/gtk.h>

#include <climits>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#include "Document.h"
#include "Editor.h"

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

const char* kindName(const Document* d) {
    switch (d->kind()) {
    case Document::Kind::Text: return "text";
    case Document::Kind::Message: return "message";
    case Document::Kind::Media: return "media";
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
    o.set("terminals", Json::array());   // the terminal's commands are later bites of 3.1
    o.set("browser", Json());
    return o;
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
        if (a.has("note") || a.has("from")) {
            respond(Result::failure("notes are not on Linux yet: open without --note and --from "
                                    "(PARITY.md 3.4)"));
            return;
        }
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
            if (!openOne(host, w, spelled, (int)files[i]["line"].asInt(0),
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
