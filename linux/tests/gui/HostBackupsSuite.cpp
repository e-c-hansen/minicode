// HostBackupsSuite.cpp — a headless host's unsaved documents outlive it
// (`./build/mutible-guitest host_backups`; docs/notes/session-host.md, "The host's
// own backups, untitled buffers, one path twice, panning", linux/HANDOFF.md
// item 102). The harness pins
// MUTIBLE_HOST=inprocess; this sets `socket` through EnvLock, as host_link
// does, and ends every host it started by its pid. The backups folder is the
// scratch world's ($MUTIBLE_BACKUP_DIR).
//   - a file left unsaved with Keep Unsaved: the host backs it up at once,
//     under the host's pid, and the window's own backup goes; a window
//     opened while the host lives gets the host's document (one tab, the
//     same id), the host's backup goes and the window's own is written;
//   - an untitled buffer another renderer left on the host (a client over
//     the socket here): backed up by the host as Untitled-1, and the next
//     window shows it as the tab of Untitled-1, a file not on disk yet,
//     unsaved, with its text;
//   - the host killed (SIGKILL) with both left unsaved: a window opened on
//     the folder starts a new host and restores both from the backups;
//   - two windows on one file, both closed keeping it unsaved: the next
//     window takes the latest one's text, the one after it the other's,
//     neither ever two tabs of the file.
#include "Harness.h"

#include "Backups.h"
#include "DocumentView.h"
#include "Editor.h"
#include "EnvLock.h"
#include "SessionClient.h"
#include "SessionLink.h"
#include "SessionServer.h"

#include <glib/gstdio.h>
#include <signal.h>

#include <climits>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

const std::string kText = "first line\n";
std::string gHostEnv;
bool gHadHostEnv = false;
std::string gDir, gTwo;
size_t gWindows = 0;
int gHostPid = 0;
uint64_t gDocId = 0, gUntitledId = 0;

std::string rp(const std::string& p) {
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}
bool alive(int pid) {
    if (pid <= 0) return false;
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(in, line)) return false;
    const size_t close = line.rfind(')');
    return close != std::string::npos && close + 2 < line.size() && line[close + 2] != 'Z';
}

Backups::Store store() {
    Backups::Store s;
    const char* dir = getenv("MUTIBLE_BACKUP_DIR");
    s.dir = dir ? dir : "";
    s.fs = Backups::posixFs();
    return s;
}
// The backup of `path` held by `pid` (0: by anyone), its text in *text.
bool backupOf(const std::string& path, int pid, std::string* text = nullptr) {
    const Backups::Store s = store();
    for (const Backups::Entry& e : s.list()) {
        if (e.path != path || (pid && e.ownerPid != pid)) continue;
        Backups::Entry got;
        std::string t;
        if (!s.load(e.id, &got, &t)) continue;
        if (text) *text = t;
        return true;
    }
    return false;
}
// How many backups of files in `dir` there are.
int backupsIn(const std::string& dir) {
    int n = 0;
    for (const Backups::Entry& e : store().list())
        if (e.root == dir) n++;
    return n;
}

MCGuiAppWindow windowOn(const std::string& dir) {
    MCGuiAppWindow found;
    for (const MCGuiAppWindow& w : MCGuiAppWindows())
        if (w.rootDir == dir) found = w;
    return found;
}
std::string textOf(Document* d) {
    if (!d) return "";
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(d->buffer(), &s, &e);
    char* t = gtk_text_buffer_get_text(d->buffer(), &s, &e, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}
// The tabs of `path` in the newest window's editor (never more than one).
int tabsOf(const std::string& path) {
    int n = 0;
    if (Editor* e = MCGuiApp().editor)
        for (Document* d : e->documents())
            if (d->path() == path) n++;
    return n;
}

bool buttonUp(const std::string& label) {
    GtkWindow* top = MCGuiTopWindow();
    for (const MCGuiAppWindow& a : MCGuiAppWindows())
        if (top == a.window) return false;
    GtkWidget* b = MCGuiFindButton(GTK_WIDGET(top), label);
    return b && gtk_widget_get_width(b) > 0;
}
void answer(const std::string& label) {
    MCGuiSetKeyWindow(MCGuiTopWindow());
    MCGuiClick(MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), label));
}

// The newest window closes with `label` answered ("Keep Unsaved", "Don't
// Save"), `left` of this suite's windows still open after it.
void closeAnswering(const std::string& label, size_t left = 0) {
    MCGuiStep("the window closes", [] { gtk_window_close(MCGuiApp().window); });
    MCGuiWait("asked, " + label, 10, [label] { return buttonUp(label); });
    MCGuiStep(label, [label] { answer(label); });
    MCGuiWait("gone", 15, [left] { return MCGuiAppWindows().size() == gWindows + left; });
    MCGuiStep("no window pinned", [] { MCGuiSetKeyWindow(nullptr); });
}

// A file opened in the newest window and typed into at its end.
void openAndType(const std::string* path, const std::string& typed) {
    MCGuiStep("opened", [path] { MCGuiOpenFileKept(*path); });
    MCGuiWait("on show", 10, [path] {
        Document* d = MCGuiApp().editor->documentFor(*path);
        return d && textOf(d) == kText;
    });
    MCGuiStep("typed at its end", [typed] {
        gtk_widget_grab_focus(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+end");
        MCGuiType(typed);
    });
    MCGuiWait("unsaved", 10, [path, typed] {
        Document* d = MCGuiApp().editor->documentFor(*path);
        return d && textOf(d) == kText + typed && d->dirty();
    });
}

// A renderer that is not an app (as the browser's view or a script would
// be), over the host's socket: it leaves an untitled buffer on the host.
struct Quiet : Shared::Client::Listener {
    void hostEdited(Shared::DocId, const Ot::Ops&, int, const Shared::Selection*) override {}
};
uint64_t leaveUntitled(const std::string& socket, const std::string& text) {
    std::string error;
    std::unique_ptr<Shared::HostLink> link = Shared::HostLink::open(socket, &error);
    if (!link) {
        MCGuiNote("no link to %s: %s", socket.c_str(), error.c_str());
        return 0;
    }
    Shared::Client client(Model::Unit::Utf16, "script");
    link->connect(client);
    Quiet quiet;
    uint64_t id = 0;
    if (client.settle()) {
        id = client.newId();
        client.open(id, "", text, &quiet);
        client.detach(id);
        if (!client.settle()) id = 0;
    }
    link->close();
    return id;
}

std::string notes() { return gDir + "/notes.txt"; }
std::string untitled() { return gDir + "/Untitled-1"; }
std::string two() { return gTwo + "/a.txt"; }

}  // namespace

MC_GUI_SUITE(host_backups) {
    static std::string notesPath, twoPath;
    MCGuiStep("scratch folders, and MUTIBLE_HOST=socket", [] {
        gWindows = MCGuiAppWindows().size();
        const std::string base = rp(MCGuiScratch()) + "/host_backups";
        for (const char* d : {"/one", "/two"}) g_mkdir_with_parents((base + d).c_str(), 0700);
        gDir = rp(base + "/one");
        gTwo = rp(base + "/two");
        MCGuiWriteFile(notes(), kText);
        MCGuiWriteFile(two(), kText);
        notesPath = notes();
        twoPath = two();
        const char* host = getenv("MUTIBLE_HOST");
        gHadHostEnv = host != nullptr;
        gHostEnv = host ? host : "";
        GCHECK(!store().dir.empty() && store().dir.find(MCGuiScratch()) == 0);   // the scratch world's, never the user's
        EnvLock::envSet("MUTIBLE_HOST", "socket");
        EnvLock::envSet("MUTIBLE_HOST_IDLE_MINUTES", "2");
        MCGuiOpenWindow(gDir);
        MCGuiSetKeyWindow(nullptr);
    });

    // ---- left unsaved: the host's backup at once, the window's gone
    openAndType(&notesPath, "unsaved");
    MCGuiStep("the host noted", [] {
        SessionLink::Link* l = MCGuiApp().editor->sessionLink();
        GCHECK(l && l->kind() == SessionLink::Link::Kind::Socket);
        if (!l) return;
        gHostPid = (int)l->startedHost();
        GCHECK(gHostPid > 0 && alive(gHostPid));
        gDocId = MCGuiApp().editor->documentFor(notes())->id();
    });
    MCGuiWait("the window's own backup, none of the host's", 15, [] {
        return backupOf(notes(), getpid()) && !backupOf(notes(), gHostPid);
    });
    closeAnswering("Keep Unsaved");
    MCGuiWait("the host's backup, the unsaved text, the window's gone", 15, [] {
        std::string text;
        return backupOf(notes(), gHostPid, &text) && text == kText + "unsaved" && !backupOf(notes(), getpid());
    });

    // ---- reattached while the host lives: its document, its backup let go
    MCGuiStep("a window opened on the folder", [] {
        MCGuiOpenWindow(gDir);
        MCGuiSetKeyWindow(nullptr);
        Document* d = MCGuiApp().editor->documentFor(notes());
        GCHECK(d && d->id() == gDocId && d->dirty());
        GCHECK_EQ_STR(textOf(d), kText + "unsaved");
        GCHECK(tabsOf(notes()) == 1 && MCGuiApp().editor->tabCount() == 1);
    });
    MCGuiWait("the window's backup, the host's gone", 15, [] {
        std::string text;
        return backupOf(notes(), getpid(), &text) && text == kText + "unsaved" && !backupOf(notes(), gHostPid);
    });

    // ---- an untitled buffer another renderer left on the host
    MCGuiStep("a script leaves an untitled buffer on the host", [] {
        SessionLink::Link* l = MCGuiApp().editor->sessionLink();
        gUntitledId = l ? leaveUntitled(l->socketPath(), "scratch text\n") : 0;
        GCHECK(gUntitledId != 0);
    });
    MCGuiWait("the host backs it up as Untitled-1", 15, [] {
        std::string text;
        return backupOf(untitled(), gHostPid, &text) && text == "scratch text\n";
    });
    MCGuiStep("no file made for it", [] { GCHECK(!g_file_test(untitled().c_str(), G_FILE_TEST_EXISTS)); });
    closeAnswering("Keep Unsaved");
    MCGuiStep("a window opened again: the file, and the buffer as Untitled-1", [] {
        MCGuiOpenWindow(gDir);
        MCGuiSetKeyWindow(nullptr);
        Editor* e = MCGuiApp().editor;
        Document* d = e->documentFor(untitled());
        GCHECK(d && d->id() == gUntitledId && d->dirty());
        GCHECK_EQ_STR(textOf(d), "scratch text\n");
        GCHECK(tabsOf(untitled()) == 1 && tabsOf(notes()) == 1 && e->tabCount() == 2);
    });
    MCGuiWait("both backed up by the window, none by the host", 15, [] {
        return backupOf(untitled(), getpid()) && backupOf(notes(), getpid()) && !backupOf(untitled(), gHostPid) &&
               !backupOf(notes(), gHostPid);
    });
    closeAnswering("Keep Unsaved");
    MCGuiWait("both the host's again", 15, [] {
        return backupOf(untitled(), gHostPid) && backupOf(notes(), gHostPid) && backupsIn(gDir) == 2;
    });

    // ---- the host killed: the backups bring them back
    MCGuiStep("the host killed by its pid", [] {
        GCHECK(alive(gHostPid));
        kill(gHostPid, SIGKILL);
    });
    MCGuiWait("gone", 15, [] { return !alive(gHostPid); });
    MCGuiStep("a window opened on the folder: a new host, both restored", [] {
        MCGuiOpenWindow(gDir);
        MCGuiSetKeyWindow(nullptr);
        SessionLink::Link* l = MCGuiApp().editor->sessionLink();
        GCHECK(l && !l->lost() && l->startedHost() > 0 && (int)l->startedHost() != gHostPid);
        if (l) gHostPid = (int)l->startedHost();
        Editor* e = MCGuiApp().editor;
        Document* n = e->documentFor(notes());
        GCHECK(n && n->dirty());
        GCHECK_EQ_STR(textOf(n), kText + "unsaved");
        Document* u = e->documentFor(untitled());
        GCHECK(u && u->dirty());
        GCHECK_EQ_STR(textOf(u), "scratch text\n");
        GCHECK(tabsOf(notes()) == 1 && tabsOf(untitled()) == 1);
    });
    MCGuiWait("the restored backups this window's now", 15, [] {
        return backupOf(notes(), getpid()) && backupOf(untitled(), getpid()) && backupsIn(gDir) == 2;
    });
    closeAnswering("Don't Save");
    MCGuiWait("nothing kept: no backups of the folder", 15, [] { return backupsIn(gDir) == 0; });
    MCGuiStep("the files as they were on disk; the host ended by its pid", [] {
        GCHECK_EQ_STR(MCGuiReadFile(notes()), kText);
        GCHECK(!g_file_test(untitled().c_str(), G_FILE_TEST_EXISTS));
        GCHECK(alive(gHostPid));
        kill(gHostPid, SIGTERM);
    });
    MCGuiWait("ended", 15, [] { return !alive(gHostPid); });

    // ---- two windows on one file, both closed keeping it unsaved
    MCGuiStep("a window on the second folder", [] {
        MCGuiOpenWindow(gTwo);
        MCGuiSetKeyWindow(nullptr);
    });
    openAndType(&twoPath, "one");
    MCGuiStep("a second window on it", [] {
        gHostPid = (int)MCGuiApp().editor->sessionLink()->startedHost();
        MCGuiOpenWindow(gTwo);
        MCGuiSetKeyWindow(nullptr);
    });
    openAndType(&twoPath, "two");
    MCGuiStep("two documents of the file", [] {
        const std::vector<MCGuiAppWindow> ws = MCGuiAppWindows();
        GCHECK(ws.size() == gWindows + 2);
        if (ws.size() < 2) return;
        Document* a = ws[ws.size() - 2].editor->documentFor(two());
        Document* b = ws[ws.size() - 1].editor->documentFor(two());
        GCHECK(a && b && a->id() != b->id());
        GCHECK_EQ_STR(textOf(a), kText + "one");
    });
    MCGuiStep("the first closes", [] {
        const std::vector<MCGuiAppWindow> ws = MCGuiAppWindows();
        gtk_window_close(ws[ws.size() - 2].window);
    });
    MCGuiWait("asked", 10, [] { return buttonUp("Keep Unsaved"); });
    MCGuiStep("Keep Unsaved", [] { answer("Keep Unsaved"); });
    MCGuiWait("one left", 15, [] { return MCGuiAppWindows().size() == gWindows + 1; });
    MCGuiStep("no window pinned", [] { MCGuiSetKeyWindow(nullptr); });
    closeAnswering("Keep Unsaved");
    MCGuiWait("both the host's", 15, [] { return backupsIn(gTwo) == 2; });
    MCGuiStep("a window: the last one's text, one tab", [] {
        MCGuiOpenWindow(gTwo);
        MCGuiSetKeyWindow(nullptr);
        Document* d = MCGuiApp().editor->documentFor(two());
        GCHECK(d != nullptr);
        GCHECK_EQ_STR(textOf(d), kText + "two");
        GCHECK(tabsOf(two()) == 1 && MCGuiApp().editor->tabCount() == 1);
    });
    MCGuiStep("another: the first one's", [] {
        MCGuiOpenWindow(gTwo);
        MCGuiSetKeyWindow(nullptr);
        Document* d = MCGuiApp().editor->documentFor(two());
        GCHECK(d != nullptr);
        GCHECK_EQ_STR(textOf(d), kText + "one");
        GCHECK(tabsOf(two()) == 1 && MCGuiApp().editor->tabCount() == 1);
    });
    MCGuiWait("each the window's own backup", 15, [] {
        int mine = 0;
        for (const Backups::Entry& e : store().list())
            if (e.root == gTwo && e.ownerPid == getpid()) mine++;
        return mine == 2 && !backupOf(two(), gHostPid);
    });
    closeAnswering("Don't Save", 1);
    closeAnswering("Don't Save");
    MCGuiWait("nothing kept", 15, [] { return backupsIn(gTwo) == 0; });

    MCGuiStep("everything put back", [] {
        if (gHadHostEnv) EnvLock::envSet("MUTIBLE_HOST", gHostEnv.c_str());
        else EnvLock::envUnset("MUTIBLE_HOST");
        EnvLock::envUnset("MUTIBLE_HOST_IDLE_MINUTES");
        if (alive(gHostPid)) kill(gHostPid, SIGTERM);
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
    });
    MCGuiWait("the host ended", 15, [] { return !alive(gHostPid); });
}
