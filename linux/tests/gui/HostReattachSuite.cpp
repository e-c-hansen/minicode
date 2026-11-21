// HostReattachSuite.cpp — closing a window detaches and opening one
// reattaches (`./build/mutible-guitest host_reattach`; SessionLink.h,
// docs/notes/session-host.md "GTK reattaches (stage 6, step 2)",
// linux/HANDOFF.md item 100).
// The harness pins MUTIBLE_HOST=inprocess; the first part sets it to
// `socket` through EnvLock, as host_link does, with the scratch world's
// socket folder, and ends the host it started by its pid.
//   - headless: a file edited and left unsaved, and a counter running in the
//     terminal; the window closes with Keep Unsaved; `mutible attach --dump`
//     (the real binary) still shows both on the host;
//   - a window opened on the folder: the tab is back under the same id with
//     the unsaved text, unsaved, its undo the host's; the terminal attached,
//     the same shell, its replay showing the counter's earlier output, and
//     new output arriving;
//   - closing that terminal ends its shell; Don't Save on the window's close
//     drops the file on the host;
//   - quit and relaunch: this app's windows closed with Keep Unsaved (its
//     connection gone), a second app process (_host_reattach_child, as the
//     _backups_ suites run one) opens the folder and finds the unsaved file
//     and the running counter, then quits keeping them;
//   - the host killed: Reconnect in the status bar starts a new one, and
//     the file is on it with everything typed, before and after, unsaved;
//   - stage 7, saving through the host, on a fresh headless host: a page's
//     guest (over the socket) saves a file the window shows with typing in
//     it: the tab and its strip dot clean, in step with the file's stamp,
//     the host counting it saved; the window's Ctrl+S asks nothing and the
//     guest hears the host's write with the file's stamp; a UTF-16 file
//     keeps its mark; an agent's write then Keep Mine, the host writing the
//     window's text over it; a directory where the file was, refused by
//     the host, its sentence in the save dialog; the host killed, the
//     window writing the file itself;
//   - in-process: the same within this process, the host kept while the
//     app runs (SessionLink::keepWhileHolding); the reattached shell's own
//     `exit` closes its terminal (linux/HANDOFF.md item 117).
#include "Harness.h"

#include "DocumentView.h"
#include "Editor.h"
#include "EnvLock.h"
#include "SessionClient.h"
#include "SessionFiles.h"
#include "SessionLink.h"
#include "SessionServer.h"
#include "TabStrip.h"
#include "Terminal.h"
#include "TerminalDock.h"

#include <glib/gstdio.h>
#include <signal.h>

#include <climits>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

const std::string kText = "first line\n";
const std::string kCounter =
    "i=0; while [ $i -lt 100000 ]; do i=$((i+1)); echo tick-$i; echo $i > counter.txt; sleep 0.25; done";

std::string gHostEnv;
bool gHadHostEnv = false;
std::string gDir, gDir2;
size_t gWindows = 0;
int gHostPid = 0;
uint64_t gDocId = 0, gTermId = 0;
int gShellPid = 0, gTicksBefore = 0;
Terminal* gHeld = nullptr;   // the reattached terminal whose shell runs `exit`

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
std::string notes() { return gDir + "/notes.txt"; }
std::string expectFile() { return rp(MCGuiScratch()) + "/host_reattach/expect"; }

// The counter's last value, as it wrote it.
int ticks(const std::string& dir) { return std::atoi(MCGuiReadFile(dir + "/counter.txt").c_str()); }

// Whether `s` shows tick-n (not tick-n0).
bool showsTick(const std::string& s, int n) {
    const std::string want = "tick-" + std::to_string(n);
    for (size_t at = s.find(want); at != std::string::npos; at = s.find(want, at + 1)) {
        const size_t end = at + want.size();
        if (end >= s.size() || !isdigit((unsigned char)s[end])) return true;
    }
    return false;
}

// The highest tick-N a terminal's last lines show.
int lastTick(Terminal* t) {
    const std::string s = t ? t->recentText(200) : "";
    int best = 0;
    for (size_t at = s.find("tick-"); at != std::string::npos; at = s.find("tick-", at + 5))
        best = std::max(best, std::atoi(s.c_str() + at + 5));
    return best;
}

// `mutible attach --folder DIR --dump`, the real binary.
struct Dump {
    bool done = false;
    int status = -1;
    std::string out;
};
std::shared_ptr<Dump> gDump;
bool dumpUntil(const std::string& dir, const std::function<bool(const Dump&)>& ok) {
    if (gDump && gDump->done && ok(*gDump)) return true;
    if (!gDump || gDump->done) {
        auto d = std::make_shared<Dump>();
        gDump = d;
        MCGuiRunProcess({MCGuiBuildDir() + "/mutible", "attach", "--folder", dir, "--dump"}, {},
                        [d](int status, std::string out, std::string) {
                            d->status = status;
                            d->out = std::move(out);
                            d->done = true;
                        });
    }
    return false;
}

// The window on `dir` (the newest), and its editor's tab of notes.txt.
MCGuiAppWindow windowOn(const std::string& dir) {
    MCGuiAppWindow found;
    for (const MCGuiAppWindow& w : MCGuiAppWindows())
        if (w.rootDir == dir) found = w;
    return found;
}
Document* notesDoc(const std::string& dir) {
    const MCGuiAppWindow w = windowOn(dir);
    return w.editor ? w.editor->documentFor(dir + "/notes.txt") : nullptr;
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

// A window on `dir` with notes.txt typed into, unsaved, and the counter
// running in its terminal (the steps of the first and last parts).
void openEditAndCount(const std::string* dirp, const std::string& typed) {
    MCGuiStep("a window on the folder, notes.txt open", [dirp] {
        const std::string& dir = *dirp;
        MCGuiOpenWindow(dir);
        MCGuiSetKeyWindow(nullptr);
        MCGuiOpenFileKept(dir + "/notes.txt");
    });
    MCGuiWait("on show", 10, [dirp] { return textOf(notesDoc(*dirp)) == kText; });
    MCGuiStep("typed at its end", [] {
        gtk_widget_grab_focus(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+end");
    });
    MCGuiStep("typing", [typed] { MCGuiType(typed); });
    MCGuiWait("unsaved", 10, [dirp, typed] {
        Document* d = notesDoc(*dirp);
        return d && textOf(d) == kText + typed && d->dirty();
    });
    MCGuiStep("the counter started in the terminal", [] {
        Terminal* t = MCGuiApp().terminal;
        GCHECK(t != nullptr);
        if (t) t->runLine(kCounter, [](const std::string& why) { GCHECK_EQ_STR(why, ""); });
    });
    MCGuiWait("counting", 30, [dirp] { return lastTick(MCGuiApp().terminal) >= 3 && ticks(*dirp) >= 3; });
}

void putBack() {
    if (gHadHostEnv) EnvLock::envSet("MUTIBLE_HOST", gHostEnv.c_str());
    else EnvLock::envUnset("MUTIBLE_HOST");
    EnvLock::envUnset("MUTIBLE_HOST_IDLE_MINUTES");
}

// ---- stage 7: saving through the host, on a headless host (the part's steps
// are between the host killed and the in-process part).
struct Quiet : Shared::Client::Listener {
    void hostEdited(Shared::DocId, const Ot::Ops&, int, const Shared::Selection*) override {}
};
std::string gDir3, gE3, gF3, gK3;
int gHost3 = 0;
uint64_t gDocE3 = 0;
std::unique_ptr<Shared::HostLink> gGuestLink;
std::unique_ptr<Shared::Client> gGuest;
std::vector<std::pair<uint64_t, Json>> gGuestWrites;
size_t gWritesBefore = 0;
std::string gRefusal;

// The host's count of a document as unsaved, by its state (the guest's).
bool hostDirty(Shared::Client& c, uint64_t doc) {
    const Json st = c.sessionState();
    for (const Json& d : st["documents"].elements())
        if ((uint64_t)d["doc"].asInt() == doc) return d["dirty"].asBool();
    return false;
}
Document* doc3(const std::string& path) {
    return MCGuiApp().editor ? MCGuiApp().editor->documentFor(path) : nullptr;
}
// Whether the tab's dot in the strip shows for `path`.
bool stripDot(const std::string& path) {
    TabStrip* s = MCGuiApp().editor->strip();
    for (int i = 0; i < s->count(); i++)
        if (s->item(i).tooltip.rfind(path, 0) == 0) return s->item(i).dirty;
    return false;
}
bool inStepWithFile(Document* d, const std::string& path) {
    const FolderSearch::FileStamp f = FolderSearch::stampOf(path);
    return d && d->diskStamp().exists && d->diskStamp().inode == f.inode &&
           d->diskStamp().size == (int64_t)f.size &&
           std::abs(d->diskStamp().mtime - ((double)f.mtimeSec + f.mtimeNsec / 1e9)) < 1e-6;
}
// Every label's text under `w`, joined.
std::string allText(GtkWidget* w) {
    std::string out;
    if (GTK_IS_LABEL(w)) out += std::string(gtk_label_get_text(GTK_LABEL(w))) + "\n";
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        out += allText(c);
    return out;
}
// Types `s` at the end of `path`'s tab, which is shown.
void typeIn(const std::string& what, const std::string& pathRef, const std::string& s) {
    const std::string* pp = &pathRef;   // a global read when the step runs
    MCGuiStep(what + ": at its end", [] {
        gtk_widget_grab_focus(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+end");
    });
    MCGuiStep(what + ": typing", [s] { MCGuiType(s); });
    MCGuiWait(what + ": typed", 10, [pp, s] {
        const std::string& path = *pp;
        Document* d = doc3(path);
        const std::string t = textOf(d);
        return d && d->dirty() && t.size() >= s.size() && t.compare(t.size() - s.size(), s.size(), s) == 0;
    });
}
void show3(const std::string& what, const std::string& pathRef) {
    const std::string* path = &pathRef;
    MCGuiStep(what, [path] { MCGuiOpenFileKept(*path); });
    MCGuiWait(what + ": on show", 10, [path] {
        Document* d = doc3(*path);
        return d && MCGuiApp().editor->active() == d;
    });
}

}  // namespace

MC_GUI_SUITE(host_reattach) {
    MCGuiStep("scratch folders, and MUTIBLE_HOST=socket", [] {
        gWindows = MCGuiAppWindows().size();
        const std::string base = rp(MCGuiScratch()) + "/host_reattach";
        gDir = base + "/one";
        gDir2 = base + "/two";
        for (const std::string& d : {gDir, gDir2}) g_mkdir_with_parents(d.c_str(), 0700);
        MCGuiWriteFile(gDir + "/notes.txt", kText);
        MCGuiWriteFile(gDir2 + "/notes.txt", kText);
        gDir = rp(gDir);
        gDir2 = rp(gDir2);
        const char* host = getenv("MUTIBLE_HOST");
        gHadHostEnv = host != nullptr;
        gHostEnv = host ? host : "";
        const char* runtime = getenv("XDG_RUNTIME_DIR");
        GCHECK(runtime && std::string(runtime).rfind("/tmp/mcgui-", 0) == 0);
        EnvLock::envSet("MUTIBLE_HOST", "socket");
        EnvLock::envSet("MUTIBLE_HOST_IDLE_MINUTES", "2");
    });

    // ---- headless: closed with Keep Unsaved, then opened again
    openEditAndCount(&gDir, "unsaved");
    MCGuiStep("the host, the document and the shell noted", [] {
        SessionLink::Link* l = MCGuiApp().editor->sessionLink();
        GCHECK(l && l->kind() == SessionLink::Link::Kind::Socket);
        if (!l) return;
        gHostPid = (int)l->startedHost();
        GCHECK(gHostPid > 0 && alive(gHostPid));
        gDocId = notesDoc(gDir)->id();
        gTermId = MCGuiApp().terminal->hostTerminal();
        gShellPid = MCGuiApp().terminal->shellPid();
        GCHECK(gDocId && gTermId && gShellPid > 0);
    });
    MCGuiStep("the window closes", [] { gtk_window_close(MCGuiApp().window); });
    MCGuiWait("asked, with Keep Unsaved", 10, [] { return buttonUp("Keep Unsaved"); });
    MCGuiStep("Keep Unsaved", [] { answer("Keep Unsaved"); });
    MCGuiWait("gone", 15, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiWait("`attach --dump`: the file unsaved and the counter running, on the host", 30, [] {
        return dumpUntil(gDir, [](const Dump& d) {
            return d.status == 0 && d.out.find(" " + notes() + " (") != std::string::npos &&
                   d.out.find(", unsaved)\n" + kText + "unsaved") != std::string::npos &&
                   d.out.find("== terminal " + std::to_string(gTermId) + " (") != std::string::npos &&
                   d.out.find("shell " + std::to_string(gShellPid)) != std::string::npos &&
                   d.out.find("tick-") != std::string::npos;
        });
    });
    MCGuiStep("the host and the shell run on", [] {
        gDump.reset();
        MCGuiSetKeyWindow(nullptr);
        GCHECK(alive(gHostPid) && alive(gShellPid));
        gTicksBefore = ticks(gDir);
    });
    MCGuiWait("counting with no window", 15, [] { return ticks(gDir) > gTicksBefore + 2; });

    MCGuiStep("a window opened on the folder", [] {
        gTicksBefore = ticks(gDir);
        MCGuiOpenWindow(gDir);
        MCGuiSetKeyWindow(nullptr);
    });
    MCGuiStep("the tab is back: the host's document, its unsaved text, its undo", [] {
        Document* d = notesDoc(gDir);
        GCHECK(d != nullptr);
        if (!d) return;
        GCHECK(d->id() == gDocId);
        GCHECK_EQ_STR(textOf(d), kText + "unsaved");
        GCHECK(d->dirty());
        GCHECK(MCGuiApp().editor->active() == d);
        GCHECK(d->canUndo());
    });
    MCGuiStep("the terminal is the host's, the same shell; shown", [] {
        Terminal* t = MCGuiApp().terminal;
        GCHECK(t && t->hostTerminal() == gTermId && t->shellPid() == gShellPid);
        if (t) t->focus();   // made: attached, its replay fed
    });
    MCGuiWait("its replay shows the output from before it attached", 30, [] {
        Terminal* t = MCGuiApp().terminal;
        return t && showsTick(t->recentText(200), std::max(1, gTicksBefore - 1));
    });
    MCGuiWait("new output, past what the counter had written as it opened", 30, [] {
        return lastTick(MCGuiApp().terminal) > gTicksBefore + 2;
    });
    MCGuiStep("still the same shell, running", [] {
        Terminal* t = MCGuiApp().terminal;
        GCHECK(t->shellPid() == gShellPid && alive(gShellPid) && t->busy());
        // Undo is the host's, through the history the first window made.
        GCHECK(notesDoc(gDir)->undo() == Document::UndoResult::Done);
    });
    MCGuiWait("the typing undone", 10, [] { return textOf(notesDoc(gDir)) == kText; });
    MCGuiStep("typed again", [] {
        gtk_widget_grab_focus(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+end");
        MCGuiType("again");
    });
    MCGuiWait("unsaved again", 10, [] { return textOf(notesDoc(gDir)) == kText + "again"; });

    MCGuiStep("the terminal closed: its shell ends", [] {
        MCGuiApp().termDock->closePane(MCGuiApp().terminal, false);
    });
    MCGuiWait("ended", 15, [] { return !alive(gShellPid); });
    MCGuiStep("the window closes", [] { gtk_window_close(MCGuiApp().window); });
    MCGuiWait("asked", 10, [] { return buttonUp("Don't Save"); });
    MCGuiStep("Don't Save", [] { answer("Don't Save"); });
    MCGuiWait("gone", 15, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiWait("`attach --dump`: no document left on the host", 30, [] {
        return dumpUntil(gDir, [](const Dump& d) {
            return d.status == 0 && d.out.rfind("== host " + gDir + "\n", 0) == 0 &&
                   d.out.find("== document ") == std::string::npos &&
                   d.out.find("== terminal " + std::to_string(gTermId) + " ") == std::string::npos;
        });
    });

    // ---- quit and relaunch: a second app process reattaches
    MCGuiStep("the file left as it was on disk", [] {
        gDump.reset();
        MCGuiSetKeyWindow(nullptr);
        GCHECK_EQ_STR(MCGuiReadFile(notes()), kText);
    });
    openEditAndCount(&gDir, "kept");
    MCGuiStep("what the second app should find", [] {
        Document* d = notesDoc(gDir);
        Terminal* t = MCGuiApp().terminal;
        GCHECK(d && t);
        if (!d || !t) return;
        gDocId = d->id();
        gTermId = t->hostTerminal();
        gShellPid = t->shellPid();
        std::ostringstream o;
        o << gDir << "\n" << gDocId << "\n" << gTermId << "\n" << gShellPid << "\n";
        MCGuiWriteFile(expectFile(), o.str());
    });
    MCGuiStep("this app's window closes", [] { gtk_window_close(MCGuiApp().window); });
    MCGuiWait("asked", 10, [] { return buttonUp("Keep Unsaved"); });
    MCGuiStep("Keep Unsaved", [] { answer("Keep Unsaved"); });
    MCGuiWait("gone, and this app no longer on the host", 15, [] {
        if (MCGuiAppWindows().size() != gWindows) return false;
        for (const auto& l : SessionLink::all())
            if (l->folder() == gDir) return false;
        return true;
    });
    MCGuiAsync("a second app opens the folder and finds both", 120, [](MCGuiDone done) {
        MCGuiSetKeyWindow(nullptr);
        MCGuiRunChild("_host_reattach_child", gDir, [done](int status, std::string out) {
            GCHECK(status == 0);
            if (status != 0) MCGuiNote("the second app (%d):\n%s", status, out.c_str());
            done();
        });
    });
    MCGuiWait("it quit keeping them: still on the host", 30, [] {
        return dumpUntil(gDir, [](const Dump& d) {
            return d.status == 0 && d.out.find(", unsaved)\n" + kText + "kept") != std::string::npos &&
                   d.out.find("== terminal " + std::to_string(gTermId) + " (") != std::string::npos;
        });
    });
    // ---- the host gone, then Reconnect
    MCGuiStep("a window takes them back, then the host is killed", [] {
        gDump.reset();
        MCGuiOpenWindow(gDir);
        MCGuiSetKeyWindow(nullptr);
        GCHECK_EQ_STR(textOf(notesDoc(gDir)), kText + "kept");
        GCHECK(alive(gHostPid));
        kill(gHostPid, SIGKILL);
    });
    MCGuiWait("Reconnect in the status bar", 15, [] {
        GtkWidget* b = MCGuiFindButton(MCGuiApp().statusBar, "Reconnect");
        return MCGuiApp().editor->sessionLink()->lost() && b && gtk_widget_get_visible(b);
    });
    MCGuiStep("typed meanwhile, then Reconnect", [] {
        gtk_widget_grab_focus(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+end");
        MCGuiType("!");
    });
    MCGuiStep("Reconnect", [] { MCGuiClick(MCGuiFindButton(MCGuiApp().statusBar, "Reconnect")); });
    MCGuiWait("a new host, the file on it with everything typed", 30, [] {
        SessionLink::Link* l = notesDoc(gDir) ? notesDoc(gDir)->sessionLink() : nullptr;
        if (!l || l->lost() || l->startedHost() <= 0) return false;
        return dumpUntil(gDir, [](const Dump& d) {
            return d.status == 0 && d.out.find(", unsaved)\n" + kText + "kept!") != std::string::npos;
        });
    });
    MCGuiStep("its button gone; the window closes, Don't Save", [] {
        gDump.reset();
        gHostPid = (int)notesDoc(gDir)->sessionLink()->startedHost();
        GtkWidget* b = MCGuiFindButton(MCGuiApp().statusBar, "Reconnect");   // only a shown one is found
        GCHECK(!b || !gtk_widget_get_visible(b));
        GCHECK(MCGuiApp().editor->sessionLink() == notesDoc(gDir)->sessionLink());
        gtk_window_close(MCGuiApp().window);
    });
    MCGuiWait("asked", 10, [] { return buttonUp("Don't Save"); });
    MCGuiStep("Don't Save", [] { answer("Don't Save"); });
    MCGuiWait("gone", 15, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("the new host ended by its pid", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK(alive(gHostPid));
        kill(gHostPid, SIGTERM);
    });
    MCGuiWait("ended, and the old shells with the old host", 15, [] {
        return !alive(gHostPid) && !alive(gShellPid);
    });

    // ---- stage 7, saving through the host, on a fresh headless host: a page's
    // guest over the socket saves a file the window shows with typing in it,
    // the window saves it back, a UTF-16 file, Keep Mine, a refused save and
    // a host lost (linux/HANDOFF.md item 132).
    MCGuiStep("stage 7: a window on a new folder, e.txt typed into", [] {
        gWindows = MCGuiAppWindows().size();
        const std::string base = rp(MCGuiScratch()) + "/host_reattach";
        gDir3 = base + "/three";
        g_mkdir_with_parents(gDir3.c_str(), 0700);
        gDir3 = rp(gDir3);
        gE3 = gDir3 + "/e.txt";
        gF3 = gDir3 + "/f16.txt";
        gK3 = gDir3 + "/k.txt";
        MCGuiWriteFile(gE3, "epsilon\n");
        MCGuiWriteFile(gK3, "kappa\n");
        MCGuiWriteFile(gF3, std::string("\xFF\xFE" "f\0\n\0", 6));
        MCGuiOpenWindow(gDir3);
        MCGuiSetKeyWindow(nullptr);
        MCGuiOpenFileKept(gE3);
    });
    MCGuiWait("e.txt on show", 10, [] { return textOf(doc3(gE3)) == "epsilon\n"; });
    typeIn("e.txt", gE3, "typed-e\n");
    MCGuiStep("a page's guest connects over the host's socket and views it", [] {
        SessionLink::Link* l = MCGuiApp().editor->sessionLink();
        GCHECK(l && l->kind() == SessionLink::Link::Kind::Socket && l->startedHost() > 0);
        Document* d = doc3(gE3);
        GCHECK(d && d->hostSavesFile());
        if (!l || !d) return;
        gHost3 = (int)l->startedHost();
        gDocE3 = d->id();
        GCHECK(l->client().settle());
        std::string error;
        gGuestLink = Shared::HostLink::open(l->socketPath(), &error);
        GCHECK(gGuestLink != nullptr);
        if (!gGuestLink) return;
        gGuest = std::make_unique<Shared::Client>(Model::Unit::Utf16, "page");
        gGuest->setGuest(true);
        gGuest->hearWrites([](Shared::DocId doc, std::uint64_t, const Json& j) { gGuestWrites.push_back({doc, j}); });
        gGuestLink->connect(*gGuest);
        GCHECK(gGuest->settle() && gGuest->hostSaves());
        static Quiet quiet;
        gGuest->view(gDocE3, &quiet);
        GCHECK(gGuest->settle());
        GCHECK(hostDirty(*gGuest, gDocE3));   // the window's dot and the host's count agree
        GCHECK(stripDot(gE3));
    });
    MCGuiStep("the page saves it: the host writes, though the window has the file open", [] {
        if (!gGuest) return;
        const std::uint64_t req = gGuest->requestSave(gDocE3, 0, false);
        GCHECK(req != 0 && gGuest->settle());
        Shared::Client::SaveAnswer a;
        GCHECK(gGuest->saveAnswer(req, &a) && a.ok);
        if (!a.ok) MCGuiNote("the page's save: %s", a.json.dump().c_str());
        GCHECK_EQ_STR(MCGuiReadFile(gE3), "epsilon\ntyped-e\n");
    });
    MCGuiWait("the window's tab follows: clean, its dot in the strip gone, in step with the file", 10, [] {
        Document* d = doc3(gE3);
        return d && !d->dirty() && !stripDot(gE3) && inStepWithFile(d, gE3) && textOf(d) == "epsilon\ntyped-e\n";
    });
    MCGuiStep("the host counts it saved", [] {
        if (gGuest) GCHECK(!hostDirty(*gGuest, gDocE3));
        GCHECK(buttonUp("Keep Mine") == false);   // no question
    });
    typeIn("e.txt again", gE3, "more\n");
    MCGuiStep("saved (Ctrl+S): the host's write", [] {
        gWritesBefore = gGuestWrites.size();
        MCGuiKey("ctrl+s");
    });
    MCGuiWait("written, clean, with no question", 10, [] {
        Document* d = doc3(gE3);
        GCHECK(MCGuiTopWindow() == MCGuiApp().window);
        return d && !d->dirty() && MCGuiReadFile(gE3) == "epsilon\ntyped-e\nmore\n" && inStepWithFile(d, gE3);
    });
    MCGuiWait("the page hears the host's write, with the file's stamp", 10, [] {
        if (!gGuest) return true;
        gGuest->dispatch();
        return gGuestWrites.size() > gWritesBefore;
    });
    MCGuiStep("its stamp is the file's; the host counts it saved", [] {
        if (!gGuest || gGuestWrites.size() <= gWritesBefore) return;
        const auto& w = gGuestWrites.back();
        GCHECK(w.first == gDocE3);
        GCHECK(Shared::stampFromJson(w.second["stamp"]) == FolderSearch::stampOf(gE3));
        GCHECK(!hostDirty(*gGuest, gDocE3));
    });
    MCGuiStep("the page's guest goes", [] {
        if (gGuestLink) gGuestLink->close();
        gGuestLink.reset();
        gGuest.reset();
    });
    show3("f16.txt", gF3);
    typeIn("f16.txt", gF3, "g\n");
    MCGuiStep("a UTF-16 file saved from the window", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("written byte for byte with its mark, the tab clean", 10, [] {
        Document* d = doc3(gF3);
        return d && !d->dirty() &&
               MCGuiReadFile(gF3) == std::string("\xFF\xFE" "f\0\n\0g\0\n\0", 10);
    });
    // Keep Mine: the file changed (another size) under a tab with edits.
    show3("e.txt for Keep Mine", gE3);
    MCGuiStep("an agent's write, then typing", [] {
        MCGuiWriteFile(gE3, "an agent's edit\n");
    });
    typeIn("Keep Mine's text", gE3, "mine\n");
    MCGuiWait("asked: changed on disk", 15, [] { return buttonUp("Keep Mine"); });
    MCGuiStep("Keep Mine, then saved", [] {
        answer("Keep Mine");
        MCGuiSetKeyWindow(nullptr);
    });
    MCGuiWait("the question gone", 10, [] { return MCGuiTopWindow() == MCGuiApp().window; });
    MCGuiStep("Ctrl+S: the host writes the window's text over the agent's", [] {
        gtk_widget_grab_focus(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+s");
    });
    MCGuiWait("the window's text is the file's, clean", 10, [] {
        Document* d = doc3(gE3);
        return d && !d->dirty() && MCGuiReadFile(gE3) == "epsilon\ntyped-e\nmore\nmine\n" &&
               inStepWithFile(d, gE3);
    });
    // A refused save: a directory where the file was, which the window's own
    // checks let through and the host refuses.
    show3("k.txt for the refusal", gK3);
    typeIn("k.txt", gK3, "lambda\n");
    MCGuiStep("a directory where k.txt was, then Ctrl+S", [] {
        GCHECK(g_remove(gK3.c_str()) == 0);
        GCHECK(g_mkdir(gK3.c_str(), 0700) == 0);
        gtk_widget_grab_focus(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+s");
    });
    MCGuiWait("the save dialog shows", 15, [] {
        if (!buttonUp("OK")) return false;
        gRefusal = allText(GTK_WIDGET(MCGuiTopWindow()));
        return true;
    });
    MCGuiStep("it says the host's sentence; the tab is still unsaved", [] {
        MCGuiNote("the dialog: %s", gRefusal.c_str());
        GCHECK(gRefusal.find("Could not save “k.txt”") != std::string::npos);
        GCHECK(gRefusal.find("Your changes are still here") != std::string::npos);
        GCHECK(gRefusal.find("did not answer") == std::string::npos);
        // Something between the title and the closing line: the host's why.
        const size_t a = gRefusal.find("Could not save “k.txt”\n");
        const size_t b = gRefusal.find("Your changes are still here");
        GCHECK(a != std::string::npos && b != std::string::npos &&
               b - (a + std::string("Could not save “k.txt”\n").size()) > 3);
        Document* d = doc3(gK3);
        GCHECK(d && d->dirty());
        answer("OK");
        MCGuiSetKeyWindow(nullptr);
    });
    MCGuiStep("the directory put back as the file", [] {
        GCHECK(g_rmdir(gK3.c_str()) == 0);
        MCGuiWriteFile(gK3, "kappa\n");
    });
    // A lost host: the window writes the file itself.
    show3("e.txt, the host to be lost", gE3);
    MCGuiStep("the host killed by its pid", [] {
        GCHECK(gHost3 > 0 && alive(gHost3));
        if (gHost3 > 0) kill(gHost3, SIGKILL);
    });
    MCGuiWait("the link lost", 15, [] { return MCGuiApp().editor->sessionLink()->lost(); });
    typeIn("e.txt, host lost", gE3, "alone\n");
    MCGuiStep("Ctrl+S with no host", [] {
        gtk_widget_grab_focus(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+s");
    });
    MCGuiWait("written by the window itself, clean", 10, [] {
        Document* d = doc3(gE3);
        return d && !d->dirty() && MCGuiReadFile(gE3) == "epsilon\ntyped-e\nmore\nmine\nalone\n";
    });
    MCGuiStep("the window closes", [] { gtk_window_close(MCGuiApp().window); });
    MCGuiWait("asked about k.txt", 10, [] { return buttonUp("Don't Save"); });
    MCGuiStep("Don't Save", [] { answer("Don't Save"); });
    MCGuiWait("gone", 15, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("nothing written to k.txt; the host gone", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK_EQ_STR(MCGuiReadFile(gK3), "kappa\n");
    });
    MCGuiWait("the host ended", 15, [] { return !alive(gHost3); });

    // ---- in-process: within this app
    MCGuiStep("MUTIBLE_HOST=inprocess", [] {
        putBack();
        EnvLock::envSet("MUTIBLE_HOST", "inprocess");
        MCGuiOpenWindow(gDir2);
        MCGuiSetKeyWindow(nullptr);
        MCGuiOpenFileKept(gDir2 + "/notes.txt");
    });
    MCGuiWait("on show", 10, [] { return textOf(notesDoc(gDir2)) == kText; });
    MCGuiStep("typed, then saved", [] {
        GCHECK(MCGuiApp().editor->sessionLink()->kind() == SessionLink::Link::Kind::InProcess);
        gtk_widget_grab_focus(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+end");
        MCGuiType("inside");
    });
    MCGuiWait("typed", 10, [] { return textOf(notesDoc(gDir2)) == kText + "inside"; });
    MCGuiStep("saved", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("clean", 10, [] { return !notesDoc(gDir2)->dirty(); });
    MCGuiStep("the counter started", [] {
        gDocId = notesDoc(gDir2)->id();
        MCGuiApp().terminal->runLine(kCounter, [](const std::string&) {});
    });
    MCGuiWait("counting", 30, [] { return lastTick(MCGuiApp().terminal) >= 3; });
    MCGuiStep("the window closes, nothing asked", [] {
        gTermId = MCGuiApp().terminal->hostTerminal();
        gShellPid = MCGuiApp().terminal->shellPid();
        GCHECK(gShellPid > 0);
        gtk_window_close(MCGuiApp().window);
    });
    MCGuiWait("gone", 15, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("the in-process host kept, holding both", [] {
        MCGuiSetKeyWindow(nullptr);
        std::shared_ptr<SessionLink::Link> link;
        for (const auto& l : SessionLink::all())
            if (l->folder() == gDir2) link = l;
        GCHECK(link != nullptr);
        if (!link) return;
        const Json st = link->client().sessionState();
        GCHECK(st["documents"].size() == 1 && st["terminals"].size() >= 1);
        GCHECK(alive(gShellPid));
        gTicksBefore = ticks(gDir2);
    });
    MCGuiStep("a window opened on the folder again", [] {
        MCGuiOpenWindow(gDir2);
        MCGuiSetKeyWindow(nullptr);
        Document* d = notesDoc(gDir2);
        GCHECK(d && d->id() == gDocId && !d->dirty() && d->canUndo());
        Terminal* t = MCGuiApp().terminal;
        GCHECK(t && t->hostTerminal() == gTermId && t->shellPid() == gShellPid);
        if (t) t->focus();
    });
    MCGuiWait("its terminal attached, counting on", 30, [] {
        return lastTick(MCGuiApp().terminal) > gTicksBefore + 2;
    });
    // The held shell's own `exit` closes its terminal (linux/HANDOFF.md
    // item 117), as the first part's close by hand ends it.
    MCGuiStep("the counter stopped (^C)", [] {
        gHeld = MCGuiApp().terminal;
        GCHECK(gHeld && gHeld->onShellExit != nullptr);
        if (gHeld && gHeld->vteWidget()) vte_terminal_feed_child(VTE_TERMINAL(gHeld->vteWidget()), "\x03", 1);
    });
    MCGuiWait("the held shell at its prompt", 15, [] {
        return gHeld->shellState().known && gHeld->shellState().atPrompt && !gHeld->busy();
    });
    MCGuiStep("exit, run in the held shell", [] {
        gHeld->runLine("exit", [](const std::string& why) { GCHECK_EQ_STR(why, ""); });
    });
    MCGuiWait("it closes its terminal; a fresh one waits", 15, [] {
        return MCGuiApp().termDock->indexOf(gHeld) < 0 && MCGuiApp().termDock->count() == 1 &&
               MCGuiApp().terminal->hostTerminal() != gTermId;
    });
    MCGuiWait("ended", 15, [] {
        gHeld = nullptr;
        return !alive(gShellPid);
    });
    MCGuiStep("the window closes", [] { gtk_window_close(MCGuiApp().window); });
    MCGuiWait("gone", 15, [] { return MCGuiAppWindows().size() == gWindows; });

    MCGuiStep("everything put back", [] {
        putBack();
        if (alive(gHostPid)) kill(gHostPid, SIGKILL);
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
        gDump.reset();
    });
}

// ------------------------------------------ the second app: finds them back
namespace {
std::vector<std::string> gExpect;
Terminal* heldTerminal() {
    const MCGuiAppWindow w = windowOn(gExpect.size() > 0 ? gExpect[0] : "");
    if (!w.termDock) return nullptr;
    for (Terminal* t : w.termDock->all())
        if (gExpect.size() > 2 && t->hostTerminal() == std::strtoull(gExpect[2].c_str(), nullptr, 10)) return t;
    return nullptr;
}
}  // namespace

MC_GUI_SUITE(_host_reattach_child) {
    MCGuiStep("what to find", [] {
        std::istringstream in(MCGuiReadFile(expectFile()));
        for (std::string line; std::getline(in, line);) gExpect.push_back(line);
        GCHECK(gExpect.size() == 4);
        gDir = gExpect.size() > 0 ? gExpect[0] : "";
    });
    MCGuiWait("a window on the folder", 30, [] {
        const MCGuiAppWindow w = windowOn(gDir);
        return w.window && gtk_widget_get_mapped(GTK_WIDGET(w.window));
    });
    MCGuiStep("on the folder's headless host, the file's tab back with its unsaved text", [] {
        const MCGuiAppWindow w = windowOn(gDir);
        MCGuiSetKeyWindow(w.window);
        GCHECK(w.editor->sessionLink() && w.editor->sessionLink()->kind() == SessionLink::Link::Kind::Socket);
        Document* d = notesDoc(gDir);
        GCHECK(d != nullptr);
        if (!d) return;
        GCHECK(std::to_string(d->id()) == gExpect[1]);
        GCHECK_EQ_STR(textOf(d), kText + "kept");
        GCHECK(d->dirty());
        Terminal* t = heldTerminal();
        GCHECK(t != nullptr);
        if (!t) return;
        GCHECK(std::to_string(t->shellPid()) == gExpect[3]);
        gTicksBefore = ticks(gDir);
        w.termDock->focusPane(t);
        t->focus();
    });
    MCGuiWait("its terminal attached: the replay of what came before", 30, [] {
        Terminal* t = heldTerminal();
        return t && showsTick(t->recentText(200), std::max(1, gTicksBefore - 1));
    });
    MCGuiWait("then new output", 30, [] { return lastTick(heldTerminal()) > gTicksBefore + 2; });
    // The app's Quit (the terminal has the keyboard, and Ctrl+Q is its).
    MCGuiStep("quit", [] {
        g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()), "quit", nullptr);
    });
    MCGuiWait("asked", 15, [] { return buttonUp("Keep Unsaved"); });
    MCGuiStep("Keep Unsaved", [] { answer("Keep Unsaved"); });
    MCGuiWait("every window closes", 15, [] { return windowOn(gDir).window == nullptr; });
}
