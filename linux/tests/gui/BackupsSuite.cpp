// BackupsSuite.cpp — unsaved tabs survive a SIGKILL (`./build/minicode-guitest
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
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "TabStrip.h"

#include <glib/gstdio.h>
#include <unistd.h>

#include <memory>
#include <string>

namespace {

std::string Folder() { return MCGuiScratch() + "/bk"; }
std::string LaunchFolder() { return MCGuiScratch() + "/bk-launch"; }
std::string In(const std::string& name) { return Folder() + "/" + name; }
std::string PidFile() { return MCGuiScratch() + "/bk-writer.pid"; }

// The backups in the store that name a file in the folder.
int BackupsOfFolder() {
    const char* dir = g_getenv("MINICODE_BACKUP_DIR");
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
        const std::string s = MCGuiReadFile(MCGuiScratch() + "/state/minicode/sessions.json");
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
        GCHECK(!gtk_text_buffer_get_can_undo(ed()->buffer()));
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
