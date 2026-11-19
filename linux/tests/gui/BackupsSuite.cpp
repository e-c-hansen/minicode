// BackupsSuite.cpp — unsaved tabs survive a SIGKILL (`./build/mutible-guitest
// backups`, linux/PARITY.md item 1.3), the counterpart of the Mac's
// tests/gui/BackupsSuite.mm. The suite runs the app twice more, as second
// apps in the same scratch world: the first (_backups_writer) opens a folder
// of three files, types into each by real key presses without saving, waits
// for their backups, and is then killed by a real `kill -9` from this app;
// meanwhile one file changes on disk and another is deleted. The second
// (_backups_restore) is started on another folder, opens a window of its own
// on the first folder because that folder has backups nobody holds, and
// finds every buffer back unsaved, the disk's text as the saved text and
// nothing to undo, says so in the status bar, asks about the file changed
// since, and holds the text of the file gone; quitting with Don't Save
// leaves no backup and writes nothing.
//
// Then backups by window (round 10's workspace item 1, the Mac's #247): a
// fourth app opens two windows on one folder, each with different unsaved
// text in a.txt (and the second with b.txt too), and is killed. A fifth,
// launched with no path, brings both windows back, each with its own
// unsaved text and nothing of the other's, and no backup deleted; killed in
// turn, it leaves them to a sixth opened on the folder itself, whose one
// window takes them all: a.txt with one window's text and "a (unsaved
// copy).txt" with the other's, not on disk until saved.
#include "Harness.h"

#include "Backups.h"
#include "DocumentView.h"
#include "Editor.h"
#include "TabSessions.h"
#include "TabStrip.h"

#include <glib/gstdio.h>
#include <unistd.h>

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string Folder() { return MCGuiScratch() + "/bk"; }
std::string LaunchFolder() { return MCGuiScratch() + "/bk-launch"; }
std::string In(const std::string& name) { return Folder() + "/" + name; }
std::string PidFile() { return MCGuiScratch() + "/bk-writer.pid"; }

// The backups in the store that name a file in the folder.
int BackupsOfFolder() {
    const char* dir = g_getenv("MUTIBLE_BACKUP_DIR");
    int n = 0;
    GDir* d = dir ? g_dir_open(dir, 0, nullptr) : nullptr;
    if (!d) return 0;
    while (const char* name = g_dir_read_name(d)) {
        const std::string f = name;
        if (f.size() < 9 || f.compare(f.size() - 9, 9, ".mcbackup") != 0) continue;
        if (MCGuiReadFile(std::string(dir) + "/" + f).find("/bk/") != std::string::npos) n++;
    }
    g_dir_close(d);
    return n;
}

// The window on the folder, of the app's windows.
MCGuiAppWindow FolderWindow() {
    for (const MCGuiAppWindow& w : MCGuiAppWindows())
        if (w.rootDir == Folder()) return w;
    return MCGuiAppWindow();
}

// The folder window's editor, else the newest window's, so a check that fails
// does not take the app down with it.
Editor* ed() { return FolderWindow().editor ? FolderWindow().editor : MCGuiApp().editor; }

std::string ViewText() {
    GtkTextBuffer* b = ed()->buffer();
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* t = gtk_text_buffer_get_text(b, &s, &e, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

std::string Status() {
    GtkWidget* l = FolderWindow().statusLabel;
    return l ? gtk_label_get_text(GTK_LABEL(l)) : "";
}

// A question up and laid out, the newest window that is not the app's.
bool QuestionUp() {
    GtkWindow* top = MCGuiTopWindow();
    for (const MCGuiAppWindow& w : MCGuiAppWindows())
        if (w.window == top) return false;
    GtkWidget* b = MCGuiFindButton(GTK_WIDGET(top), "Keep Mine");
    if (!b) b = MCGuiFindButton(GTK_WIDGET(top), "Cancel");
    return b && gtk_widget_get_width(b) > 0;
}

std::string QuestionText() {
    std::string all;
    std::vector<GtkWidget*> todo = {GTK_WIDGET(MCGuiTopWindow())};
    while (!todo.empty()) {
        GtkWidget* w = todo.back();
        todo.pop_back();
        if (GTK_IS_LABEL(w)) all += std::string(gtk_label_get_text(GTK_LABEL(w))) + "\n";
        for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
            todo.push_back(c);
    }
    return all;
}

void Answer(const char* button) { MCGuiClick(MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), button)); }

// In the writer: a file in a tab of its own, and typed into at its start.
void OpenAndType(const std::string& name) {
    MCGuiStep("open " + name, [name] { MCGuiOpenFileKept(In(name)); });
    MCGuiWait(name + " on show", 10, [name] { return MCGuiApp().editor->currentPath() == In(name); });
    MCGuiStep("type at its start, unsaved", [name] {
        MCGuiClick(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+Home");
        MCGuiType("unsaved " + name + " ");
    });
    MCGuiWait(name + " is unsaved", 10, [name] {
        const std::string t = MCGuiEditorText();
        return MCGuiApp().editor->dirty() && t.rfind("unsaved " + name + " ", 0) == 0;
    });
}

// ---- two windows on one folder (round 10's workspace item 1, the Mac's #247)
std::string Folder2() { return MCGuiScratch() + "/bk2"; }
std::string In2(const std::string& name) { return Folder2() + "/" + name; }
std::string Copy2() { return In2("a (unsaved copy).txt"); }
std::string WriterPid2() { return MCGuiScratch() + "/bk2-writer.pid"; }
std::string RestorePid2() { return MCGuiScratch() + "/bk2-restore.pid"; }
const char* const kFirstA = "first window shared\n";
const char* const kSecondA = "second window shared\n";
const char* const kSecondB = "only second beta two\n";

// The backups in the store of files in the second folder, decoded.
std::vector<std::pair<Backups::Entry, std::string>> Backups2() {
    std::vector<std::pair<Backups::Entry, std::string>> out;
    const char* dir = g_getenv("MUTIBLE_BACKUP_DIR");
    GDir* d = dir ? g_dir_open(dir, 0, nullptr) : nullptr;
    if (!d) return out;
    while (const char* name = g_dir_read_name(d)) {
        const std::string f = name;
        if (f.size() < 9 || f.compare(f.size() - 9, 9, ".mcbackup") != 0) continue;
        Backups::Entry e;
        std::string text;
        if (Backups::decode(MCGuiReadFile(std::string(dir) + "/" + f), &e, &text) &&
            e.path.rfind(Folder2() + "/", 0) == 0)
            out.push_back({e, text});
    }
    g_dir_close(d);
    return out;
}

// A window's tabs, "name=text|..." in order, a '*' after an unsaved one's name.
std::string TabsOf(Editor* e) {
    std::string out;
    for (int i = 0; e && i < e->tabCount(); i++) {
        Document* d = e->document(i);
        if (!d) continue;
        const std::string p = d->path();
        out += (out.empty() ? "" : "|") + p.substr(p.find_last_of('/') + 1) +
               (d->dirty() ? "*" : "") + "=" + d->text();
    }
    return out;
}

std::vector<MCGuiAppWindow> WindowsOn2() {
    std::vector<MCGuiAppWindow> out;
    for (const MCGuiAppWindow& w : MCGuiAppWindows())
        if (w.rootDir == Folder2() && w.window && gtk_widget_get_mapped(GTK_WIDGET(w.window)))
            out.push_back(w);
    return out;
}

MCGuiAppWindow FolderWindow2() {
    const std::vector<MCGuiAppWindow> ws = WindowsOn2();
    return ws.empty() ? MCGuiAppWindow() : ws.front();
}

// In the two-window writer: `name` typed into at its start in the newest window.
void TypeInto(const std::string& name, const std::string& words) {
    MCGuiStep("open " + name + " in the newest window", [name] {
        MCGuiSetKeyWindow(MCGuiApp().window);
        MCGuiOpenFileKept(In2(name));
    });
    MCGuiWait(name + " on show", 10, [name] { return MCGuiApp().editor->currentPath() == In2(name); });
    MCGuiStep("type \"" + words + "\" at its start", [words] {
        MCGuiClick(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+Home");
        MCGuiType(words);
    });
    MCGuiWait(name + " is unsaved", 10, [words] {
        return MCGuiApp().editor->dirty() && MCGuiEditorText().rfind(words, 0) == 0;
    });
}

// When this app's own backups are written (SavePlan.h), in its window.
std::string TimingFile() { return MCGuiProjectFile("bk-timing.txt"); }
Document* Shown() { return MCGuiApp().editor->active(); }
double gTypedAt = 0, gBurstStart = 0, gWrittenDuring = -1;
uint64_t gBurstEdits = 0;

struct Writer {
    bool done = false;
    int status = -1;
    std::string out;
};

}  // namespace

// ------------------------------------------------------- the parent's part
MC_GUI_SUITE(backups) {
    auto writer = std::make_shared<Writer>();
    MCGuiStep("a folder of three files, and one to launch the third app on", [] {
        MCGuiWriteFile(In("a.txt"), "alpha\n");
        MCGuiWriteFile(In("b.txt"), "beta\n");
        MCGuiWriteFile(In("c.txt"), "gamma\n");
        MCGuiWriteFile(LaunchFolder() + "/readme.txt", "elsewhere\n");
        g_remove(PidFile().c_str());
    });
    MCGuiStep("a second app types into all three", [writer] {
        MCGuiRunChild("_backups_writer", Folder(), [writer](int status, std::string out) {
            writer->status = status;
            writer->out = out;
            writer->done = true;
        });
    });
    MCGuiWait("its backups are written, and it says where it is", 60, [writer] {
        return MCGuiFileExists(PidFile()) || writer->done;
    });
    MCGuiAsync("kill -9 by its PID", 20, [writer](MCGuiDone done) {
        GCHECK(!writer->done);
        if (writer->done) MCGuiNote("the writer ended first:\n%s", writer->out.c_str());
        const std::string pid = MCGuiReadFile(PidFile());
        GCHECK(!pid.empty() && pid.find_first_not_of("0123456789") == std::string::npos);
        MCGuiRunProcess({"/bin/sh", "-c", "kill -9 " + pid}, {},
                        [done](int status, std::string, std::string err) {
                            GCHECK(status == 0);
                            if (status != 0) MCGuiNote("kill: %s", err.c_str());
                            done();
                        });
    });
    MCGuiWait("the writer is gone", 30, [writer] { return writer->done; });
    MCGuiStep("it died of SIGKILL, its backups are on disk, the files unchanged", [writer] {
        // dbus-run-session passes on the app's end: 128 + the signal.
        GCHECK(writer->status == 128 + 9);
        GCHECK(writer->out.find("FAIL") == std::string::npos);
        if (writer->status != 128 + 9 || writer->out.find("FAIL") != std::string::npos)
            MCGuiNote("writer (%d):\n%s", writer->status, writer->out.c_str());
        GCHECK(BackupsOfFolder() == 3);
        GCHECK_EQ_STR(MCGuiReadFile(In("a.txt")), "alpha\n");
        GCHECK_EQ_STR(MCGuiReadFile(In("b.txt")), "beta\n");
        // Meanwhile, outside: b.txt changes, c.txt goes.
        MCGuiWriteFile(In("b.txt"), "beta, changed outside\n");
        g_remove(In("c.txt").c_str());
    });
    MCGuiAsync("a third app, opened on another folder, restores them", 90, [](MCGuiDone done) {
        MCGuiRunChild("_backups_restore", LaunchFolder(), [done](int status, std::string out) {
            GCHECK(status == 0);
            if (status != 0) MCGuiNote("restore (%d):\n%s", status, out.c_str());
            done();
        });
    });
    MCGuiStep("quitting with Don't Save left no backup, and wrote nothing", [] {
        GCHECK(BackupsOfFolder() == 0);
        GCHECK_EQ_STR(MCGuiReadFile(In("a.txt")), "alpha\n");
        GCHECK_EQ_STR(MCGuiReadFile(In("b.txt")), "beta, changed outside\n");
        GCHECK(!MCGuiFileExists(In("c.txt")));
    });

    // ---- two windows on one folder, both unsaved in a.txt, killed
    auto two = std::make_shared<Writer>();
    MCGuiStep("a folder for two windows", [] {
        MCGuiWriteFile(In2("a.txt"), "shared\n");
        MCGuiWriteFile(In2("b.txt"), "beta two\n");
        g_remove(WriterPid2().c_str());
        g_remove(RestorePid2().c_str());
    });
    MCGuiStep("a fourth app opens two windows on it and types into a.txt in both", [two] {
        MCGuiRunChild("_backups_two_writer", Folder2(), [two](int status, std::string out) {
            two->status = status;
            two->out = out;
            two->done = true;
        });
    });
    MCGuiWait("its backups are written, and it says where it is", 90, [two] {
        return MCGuiFileExists(WriterPid2()) || two->done;
    });
    MCGuiAsync("kill -9 by its PID", 20, [two](MCGuiDone done) {
        GCHECK(!two->done);
        if (two->done) MCGuiNote("the writer ended first:\n%s", two->out.c_str());
        const std::string pid = MCGuiReadFile(WriterPid2());
        GCHECK(!pid.empty() && pid.find_first_not_of("0123456789") == std::string::npos);
        MCGuiRunProcess({"/bin/sh", "-c", "kill -9 " + pid}, {},
                        [done](int status, std::string, std::string) {
                            GCHECK(status == 0);
                            done();
                        });
    });
    MCGuiWait("the writer is gone", 30, [two] { return two->done; });
    MCGuiStep("three backups, each naming its window, as the windows' records do", [two] {
        GCHECK(two->status == 128 + 9);
        GCHECK(two->out.find("FAIL") == std::string::npos);
        if (two->status != 128 + 9 || two->out.find("FAIL") != std::string::npos)
            MCGuiNote("writer (%d):\n%s", two->status, two->out.c_str());
        const auto all = Backups2();
        GCHECK(all.size() == 3);
        std::set<std::string> keys;
        for (const auto& b : all) {
            MCGuiNote("backup of %s by window %s", b.first.path.c_str(), b.first.window.c_str());
            GCHECK(!b.first.window.empty());
            keys.insert(b.first.window);
        }
        GCHECK(keys.size() == 2);
        std::set<std::string> recorded;
        const TabSessions::Store st =
            TabSessions::decodeStore(MCGuiReadFile(MCGuiScratch() + "/state/mutible/sessions.json"));
        for (const TabSessions::Window& w : st.windows)
            if (w.root == Folder2()) recorded.insert(w.backupKey);
        GCHECK(recorded == keys);
        GCHECK_EQ_STR(MCGuiReadFile(In2("a.txt")), "shared\n");
    });
    auto again = std::make_shared<Writer>();
    MCGuiStep("a fifth app, launched with no path, brings both windows back", [again] {
        MCGuiRunChild("_backups_two_restore", "", [again](int status, std::string out) {
            again->status = status;
            again->out = out;
            again->done = true;
        });
    });
    MCGuiWait("each window has its own, and it says where it is", 90, [again] {
        return MCGuiFileExists(RestorePid2()) || again->done;
    });
    MCGuiAsync("kill -9 by its PID", 20, [again](MCGuiDone done) {
        GCHECK(!again->done);
        if (again->done) MCGuiNote("the restore ended first (%d):\n%s", again->status, again->out.c_str());
        const std::string pid = MCGuiReadFile(RestorePid2());
        GCHECK(!pid.empty() && pid.find_first_not_of("0123456789") == std::string::npos);
        MCGuiRunProcess({"/bin/sh", "-c", "kill -9 " + pid}, {},
                        [done](int status, std::string, std::string) {
                            GCHECK(status == 0);
                            done();
                        });
    });
    MCGuiWait("it is gone", 30, [again] { return again->done; });
    MCGuiStep("killed, nothing of it failed, the three backups still there", [again] {
        GCHECK(again->status == 128 + 9);
        GCHECK(again->out.find("FAIL") == std::string::npos);
        if (again->status != 128 + 9 || again->out.find("FAIL") != std::string::npos)
            MCGuiNote("restore (%d):\n%s", again->status, again->out.c_str());
        GCHECK(Backups2().size() == 3);
    });
    MCGuiAsync("a sixth app, opened on the folder, takes all three in one window", 90,
               [](MCGuiDone done) {
        MCGuiRunChild("_backups_two_copy", Folder2(), [done](int status, std::string out) {
            GCHECK(status == 0);
            if (status != 0) MCGuiNote("copy (%d):\n%s", status, out.c_str());
            done();
        });
    });
    MCGuiStep("Don't Save there left no backup, no copy on disk, the files as they were", [] {
        GCHECK(Backups2().empty());
        GCHECK(!MCGuiFileExists(Copy2()));
        GCHECK_EQ_STR(MCGuiReadFile(In2("a.txt")), "shared\n");
        GCHECK_EQ_STR(MCGuiReadFile(In2("b.txt")), "beta two\n");
    });

    // ---- when the backups are written (SavePlan.h), in this app
    MCGuiStep("a file of the project, typed into once", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiWriteFile(TimingFile(), "timing\n");
        MCGuiOpenFileKept(TimingFile());
    });
    MCGuiWait("on show", 10, [] { return MCGuiApp().editor->currentPath() == TimingFile(); });
    MCGuiStep("a key", [] {
        MCGuiClick(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+End");
        MCGuiType("x");
        gTypedAt = MCGuiNowMs();
    });
    // The key is an X event the app handles after the step that sent it, so
    // the tab turns dirty a moment later (on a quick machine the next step
    // ran 11 to 25 ms after the key and found it clean every time).
    MCGuiWait("the key lands", 5, [] { return Shown() && Shown()->dirty(); });
    MCGuiStep("no backup the moment after it", [] {
        // Typing has to pause for a second first; a step later than that
        // (a loaded machine) checks nothing here.
        const double since = MCGuiNowMs() - gTypedAt;
        MCGuiNote("%.0f ms after the key", since);
        if (since < 800) GCHECK(!Shown()->backup().written);
    });
    MCGuiWait("its backup, once typing paused", 10, [] { return Shown() && Shown()->backup().written; });
    MCGuiStep("typing that never pauses for a second", [] {
        gBurstStart = MCGuiNowMs();
        gWrittenDuring = -1;
        gBurstEdits = Shown()->backup().writtenEdits;
    });
    for (int i = 0; i < 24; i++) {
        MCGuiStep("a key", [] {
            if (gWrittenDuring < 0 && Shown() && Shown()->backup().writtenEdits != gBurstEdits)
                gWrittenDuring = MCGuiNowMs() - gBurstStart;
            MCGuiType("y");
        });
        MCGuiPause(0.3);   // time is the condition: a key every 0.3 s, never the 1 s pause
    }
    MCGuiStep("written while the keys went on, within 5 s of the first", [] {
        MCGuiNote("written %.0f ms into %.0f ms of typing", gWrittenDuring, MCGuiNowMs() - gBurstStart);
        GCHECK(gWrittenDuring >= 0);
    });
    MCGuiStep("saved", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("clean, and its backup gone at once", 10, [] {
        return Shown() && !Shown()->dirty() && !Shown()->backup().written;
    });
    MCGuiStep("", [] { MCGuiApp().editor->closeAllTabs(); });
}

// ------------------------------------------- the first child: type and die
MC_GUI_SUITE(_backups_writer) {
    MCGuiStep("the window is on the folder", [] { GCHECK(MCGuiApp().rootDir == Folder()); });
    // c.txt last: it is the tab on show when the writer dies.
    OpenAndType("a.txt");
    OpenAndType("b.txt");
    OpenAndType("c.txt");
    // Written about a second after typing stops.
    MCGuiWait("three backups", 15, [] { return BackupsOfFolder() == 3; });
    MCGuiWait("the folder's session lists the three", 15, [] {
        const std::string s = MCGuiReadFile(MCGuiScratch() + "/state/mutible/sessions.json");
        return s.find(In("a.txt")) != std::string::npos && s.find(In("c.txt")) != std::string::npos;
    });
    MCGuiStep("say where it is", [] {
        GCHECK(MCGuiApp().editor->dirtyDocuments().size() == 3);
        MCGuiWriteFile(PidFile(), std::to_string((long)getpid()));
    });
    // Killed here. A wait that ends is a failure: the kill never came.
    MCGuiWait("the SIGKILL", 40, [] { return false; });
}

// ------------------------------------------ the second child: find them back
MC_GUI_SUITE(_backups_restore) {
    MCGuiWait("a window of its own on the folder with the backups", 20, [] {
        return MCGuiAppWindows().size() == 2 && FolderWindow().window &&
               gtk_widget_get_mapped(GTK_WIDGET(FolderWindow().window));
    });
    MCGuiStep("the three tabs are back, unsaved", [] {
        MCGuiSetKeyWindow(FolderWindow().window);
        GCHECK(ed()->tabCount() == 3);
        GCHECK(ed()->document(0) && ed()->document(0)->path() == In("a.txt"));
        GCHECK(ed()->document(1) && ed()->document(1)->path() == In("b.txt"));
        GCHECK(ed()->document(2) && ed()->document(2)->path() == In("c.txt"));
        GCHECK(ed()->dirtyDocuments().size() == 3);
        // c.txt, on show when the writer died, is gone, so the nearest tab
        // is shown instead.
        GCHECK(ed()->currentPath() == In("b.txt"));
        for (int i = 0; i < ed()->tabCount(); i++)
            GCHECK(ed()->strip()->item(i).dirty);
    });
    // The backup was made from the file as it was; it has changed since, so
    // the window asks Keep Mine or Reload once it is up, and nothing is
    // written meanwhile.
    MCGuiWait("the question about b.txt", 15, QuestionUp);
    MCGuiStep("b.txt: the unsaved text, and the question about the disk", [] {
        const std::string q = QuestionText();
        MCGuiNote("%s", q.c_str());
        GCHECK(q.find("“b.txt” changed on disk.") != std::string::npos);
        GCHECK_EQ_STR(ViewText(), "unsaved b.txt beta\n");
        GCHECK(ed()->dirty());
        GCHECK_EQ_STR(MCGuiReadFile(In("b.txt")), "beta, changed outside\n");
        Answer("Keep Mine");
    });
    MCGuiWait("the question goes", 10, [] { return !QuestionUp(); });
    MCGuiStep("a.txt", [] {
        MCGuiSetKeyWindow(FolderWindow().window);
        MCGuiKey("alt+1");
    });
    MCGuiWait("on show", 10, [] { return ed()->currentPath() == In("a.txt"); });
    MCGuiStep("a.txt: the unsaved text, nothing to undo, and the note", [] {
        GCHECK_EQ_STR(ViewText(), "unsaved a.txt alpha\n");
        GCHECK(ed()->dirty());
        GCHECK(ed()->active() && !ed()->active()->canUndo());
        GCHECK_EQ_STR(Status(), "Restored unsaved changes to a.txt");
    });
    MCGuiStep("the restored words deleted by hand", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+Home");
        for (int i = 0; i < 14; i++) MCGuiKey("Delete");
    });
    MCGuiWait("back to the disk's text, which is the saved text: clean", 10, [] {
        return ViewText() == "alpha\n" && !ed()->dirty();
    });
    MCGuiStep("c.txt", [] { MCGuiKey("alt+3"); });
    MCGuiWait("on show", 10, [] { return ed()->currentPath() == In("c.txt"); });
    MCGuiStep("c.txt: gone from disk, the text held, unsaved", [] {
        GCHECK_EQ_STR(ViewText(), "unsaved c.txt gamma\n");
        GCHECK(ed()->dirty());
        GCHECK_EQ_STR(Status(), "Restored unsaved changes to c.txt; the file is no longer on disk");
        GCHECK(!MCGuiFileExists(In("c.txt")));
    });
    MCGuiStep("quit", [] { MCGuiKey("ctrl+q"); });
    MCGuiWait("one question, for the two still unsaved", 15, QuestionUp);
    MCGuiStep("Don't Save", [] {
        const std::string q = QuestionText();
        GCHECK(q.find("Save changes to 2 files?") != std::string::npos);
        GCHECK(q.find("b.txt") != std::string::npos && q.find("c.txt") != std::string::npos);
        Answer("Don't Save");
    });
    MCGuiWait("every window closes", 15, [] { return MCGuiAppWindows().empty(); });
    MCGuiWait("the backups are gone", 10, [] { return BackupsOfFolder() == 0; });
}

// ------------------------- two windows on one folder: type in both and die
MC_GUI_SUITE(_backups_two_writer) {
    MCGuiStep("the window is on the folder", [] { GCHECK(MCGuiApp().rootDir == Folder2()); });
    TypeInto("a.txt", "first window ");
    MCGuiStep("a second window on the same folder", [] { MCGuiOpenWindow(Folder2()); });
    MCGuiWait("it is up", 10, [] { return WindowsOn2().size() == 2; });
    TypeInto("a.txt", "second window ");
    TypeInto("b.txt", "only second ");
    // Written about a second after typing stops, each naming its window.
    MCGuiWait("three backups, from two windows", 15, [] {
        std::set<std::string> keys;
        for (const auto& b : Backups2()) keys.insert(b.first.window);
        return Backups2().size() == 3 && keys.size() == 2;
    });
    MCGuiWait("both windows' records kept, with their tabs", 15, [] {
        const TabSessions::Store st = TabSessions::decodeStore(
            MCGuiReadFile(MCGuiScratch() + "/state/mutible/sessions.json"));
        int on = 0;
        bool second = false;
        for (const TabSessions::Window& w : st.windows) {
            if (w.root != Folder2() || w.backupKey.empty()) continue;
            on++;
            for (const TabSessions::Tab& t : w.tabs.tabs)
                if (t.path == In2("b.txt")) second = true;
        }
        return on == 2 && second;
    });
    MCGuiStep("say where it is", [] {
        const std::vector<MCGuiAppWindow> ws = WindowsOn2();
        GCHECK(ws.size() == 2);
        for (const MCGuiAppWindow& w : ws) MCGuiNote("%s", TabsOf(w.editor).c_str());
        MCGuiWriteFile(WriterPid2(), std::to_string((long)getpid()));
    });
    // Killed here. A wait that ends is a failure: the kill never came.
    MCGuiWait("the SIGKILL", 40, [] { return false; });
}

// --------------- both windows back, launched with no path: each its own
MC_GUI_SUITE(_backups_two_restore) {
    MCGuiWait("both windows are back on the folder", 20, [] {
        const std::vector<MCGuiAppWindow> ws = WindowsOn2();
        return ws.size() == 2 && ws[0].editor->tabCount() > 0 && ws[1].editor->tabCount() > 0;
    });
    MCGuiStep("each with its own unsaved text, nothing of the other's", [] {
        const std::vector<MCGuiAppWindow> ws = WindowsOn2();
        GCHECK(ws.size() == 2);
        if (ws.size() != 2) return;
        const std::string one = TabsOf(ws[0].editor), other = TabsOf(ws[1].editor);
        MCGuiNote("one: %s", one.c_str());
        MCGuiNote("other: %s", other.c_str());
        const std::string first = std::string("a.txt*=") + kFirstA;
        const std::string second = std::string("a.txt*=") + kSecondA + "|b.txt*=" + kSecondB;
        GCHECK((one == first && other == second) || (one == second && other == first));
        for (const MCGuiAppWindow& w : MCGuiAppWindows())
            GCHECK(w.rootDir == Folder2());   // no window for unheld backups
    });
    MCGuiStep("a window's restore deleted none of the other's backups", [] {
        GCHECK(Backups2().size() == 3);
        MCGuiWriteFile(RestorePid2(), std::to_string((long)getpid()));
    });
    MCGuiWait("the SIGKILL", 40, [] { return false; });
}

// ------- one window opened on the folder: the clash shown as a copy
MC_GUI_SUITE(_backups_two_copy) {
    MCGuiWait("the window, its three tabs back", 20, [] {
        return FolderWindow2().editor && FolderWindow2().editor->tabCount() == 3 &&
               gtk_widget_get_mapped(GTK_WIDGET(FolderWindow2().window));
    });
    MCGuiStep("a.txt with one window's text, a (unsaved copy).txt with the other's", [] {
        Editor* e = FolderWindow2().editor;
        MCGuiNote("%s", TabsOf(e).c_str());
        Document* a = e->documentFor(In2("a.txt"));
        Document* copy = e->documentFor(Copy2());
        Document* b = e->documentFor(In2("b.txt"));
        GCHECK(a && copy && b);
        if (!a || !copy || !b) return;
        const std::set<std::string> texts = {a->text(), copy->text()};
        GCHECK(texts == std::set<std::string>({kFirstA, kSecondA}));
        GCHECK_EQ_STR(b->text(), kSecondB);
        GCHECK(a->dirty() && copy->dirty() && b->dirty());
        GCHECK(e->document(2) == copy);   // after the session's tabs
        GCHECK(!MCGuiFileExists(Copy2()));   // not on disk until saved
    });
    MCGuiStep("every backup kept, the copy's under its own name", [] {
        const auto all = Backups2();
        GCHECK(all.size() == 3);
        int copies = 0;
        for (const auto& bk : all)
            if (bk.first.path == Copy2()) {
                copies++;
                GCHECK(!bk.first.diskExisted);
                GCHECK(bk.second == kFirstA || bk.second == kSecondA);
            }
        GCHECK(copies == 1);
    });
    MCGuiStep("the copy's tab", [] {
        MCGuiSetKeyWindow(FolderWindow2().window);
        FolderWindow2().editor->activate(FolderWindow2().editor->documentFor(Copy2()));
    });
    MCGuiWait("on show, and the status bar says what it is", 10, [] {
        GtkWidget* l = FolderWindow2().statusLabel;
        return FolderWindow2().editor->currentPath() == Copy2() && l &&
               std::string(gtk_label_get_text(GTK_LABEL(l))) ==
                   "Unsaved changes to a.txt from another window, kept apart in a (unsaved copy).txt";
    });
    MCGuiStep("quit", [] { MCGuiKey("ctrl+q"); });
    MCGuiWait("one question, for the three unsaved", 15, QuestionUp);
    MCGuiStep("Don't Save", [] {
        const std::string q = QuestionText();
        MCGuiNote("%s", q.c_str());
        GCHECK(q.find("Save changes to 3 files?") != std::string::npos);
        Answer("Don't Save");
    });
    MCGuiWait("every window closes", 15, [] { return MCGuiAppWindows().empty(); });
    MCGuiWait("the backups are gone", 10, [] { return Backups2().empty(); });
}
