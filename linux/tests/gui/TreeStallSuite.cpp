// TreeStallSuite.cpp — the file tree never holds the main loop
// (`./build/minicode-guitest tree_stall`), R1 in linux/PARITY.md: GNOME said
// "minicode is not responding" while the owner clicked around the tree.
//
// The tree patched a folder's rows for each event its GFileMonitor reported,
// through a sort model that costs about a quarter of a millisecond a change
// in a big folder, and GIO hands a burst of events over in one dispatch: with
// the 30,000 files of an open folder deleted the window stood still for two
// minutes, and for thirty-five seconds when they came back. Now an event only
// asks for the folder to be listed again, on a worker thread, and the
// listing goes in as splices a few milliseconds a turn (FileTree.cpp, "one
// folder"; TreeRefresh::splices and Pacer in src/FileDetails).
//
// Everything is real input: clicks and double-clicks on rows, while another
// process churns a folder on show, and while it deletes and then makes again
// the 10,000 files of an open folder. A timer on the main loop records the
// main thread's CPU time between two of its fires, and the longest such gap
// in each phase is held to a bound far above what the tree needs (a few tens
// of milliseconds) and far below what the old code took (13 s for this
// folder) or what GNOME waits before its question (5 s). An open folder and
// the selection inside the busy folder survive the burst, and the rows come
// back in the tree's order.
#include "Harness.h"

#include "Editor.h"
#include "FileTree.h"

#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>

namespace {

// The longest main-thread stall, timed by the main thread's CPU between two
// fires of a timer that asks to fire every 5 ms. reset() starts a phase; a
// stall that begins at once is timed from the reset itself.
struct StallMeter {
    guint id = 0;
    double lastWall = 0, lastCpu = 0;
    double worstCpu = 0, worstWall = 0;   // the gap with the most CPU, and its length

    static gboolean tick(gpointer p) {
        auto* m = static_cast<StallMeter*>(p);
        m->sample();
        return G_SOURCE_CONTINUE;
    }
    void sample() {
        const double w = MCGuiNowMs(), c = MCGuiThreadCpuMs();
        if (c - lastCpu > worstCpu) {
            worstCpu = c - lastCpu;
            worstWall = w - lastWall;
        }
        lastWall = w;
        lastCpu = c;
    }
    void start() {
        if (!id) id = g_timeout_add_full(G_PRIORITY_HIGH, 5, tick, this, nullptr);
        reset();
    }
    void stop() {
        if (id) g_source_remove(id);
        id = 0;
    }
    void reset() {
        lastWall = MCGuiNowMs();
        lastCpu = MCGuiThreadCpuMs();
        worstCpu = worstWall = 0;
    }
};
StallMeter gMeter;

// Far above the tree's own few tens of milliseconds, with room for a loaded
// machine, and far below the old code's seconds.
constexpr double kStallBoundMs = 500;

void checkPhase(const std::string& what) {
    gMeter.sample();
    MCGuiNote("%s: longest main-thread stall %.0f ms of CPU (%.0f ms long)", what.c_str(),
              gMeter.worstCpu, gMeter.worstWall);
    GCHECK_TIME(gMeter.worstCpu, kStallBoundMs, "the longest stall " + what);
    gMeter.reset();
}

std::string dir() { return MCGuiProjectFile("a-tree-stall"); }
std::string at(const std::string& name) { return dir() + "/" + name; }

GtkWidget* row(const std::string& name) {
    return MCGuiFindLabel(GTK_WIDGET(MCGuiApp().window), name);
}

// Rows the tree shows, open folders' entries included.
guint rowCount() {
    GtkWidget* list = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(MCGuiApp().tree->widget()));
    if (!GTK_IS_LIST_VIEW(list)) return 0;
    return g_list_model_get_n_items(G_LIST_MODEL(gtk_list_view_get_model(GTK_LIST_VIEW(list))));
}

// Where a row's label is drawn, top to bottom.
double rowY(const std::string& name) {
    GtkWidget* label = row(name);
    if (!label) return -1;
    graphene_point_t p = GRAPHENE_POINT_INIT(0, 0), out;
    if (!gtk_widget_compute_point(label, GTK_WIDGET(MCGuiApp().window), &p, &out)) return -1;
    return out.y;
}

void clickRow(const std::string& name) {
    MCGuiWait(name + " is on show", 20, [name] { return row(name) != nullptr; });
    MCGuiStep("a click on " + name, [name] { MCGuiClick(row(name)); });
}

pid_t gChurn = 0;
guint gRowsOpen = 0;   // with the big folder open and full
constexpr int kBigFiles = 10000;

void stopChurn() {
    if (gChurn > 0) {
        kill(-gChurn, SIGKILL);   // its process group: the shell and its mv or rm
        waitpid(gChurn, nullptr, 0);
    }
    gChurn = 0;
}

}  // namespace

MC_GUI_SUITE(tree_stall) {
    MCGuiStep("a folder of its own, first in the tree", [] {
        MCGuiWriteFile(at("a-files/note.txt"), "a note\n");
        MCGuiWriteFile(at("big/keep/inside.txt"), "kept open\n");
        std::string bytes(4096, '\0');
        for (size_t i = 0; i < bytes.size(); i++) bytes[i] = (char)(i * 37);
        MCGuiWriteFile(at("a-files/data.bin"), bytes);
        GCHECK(mkfifo(at("a-files/pipe").c_str(), 0600) == 0);
        GCHECK(mkdir(at("churn").c_str(), 0755) == 0);
    });
    MCGuiAsync("10,000 files in a folder", 60, [](MCGuiDone done) {
        MCGuiRunProcess({"sh", "-c",
                         "cd a-tree-stall/big && i=0; while [ $i -lt 10000 ]; do : > f$i; i=$((i+1)); done"},
                        {}, [done](int status, std::string, std::string err) {
                            GCHECK(status == 0);
                            if (status) MCGuiNote("%s", err.c_str());
                            done();
                        });
    });
    MCGuiStep("the stall meter starts", [] { gMeter.start(); });
    clickRow("a-tree-stall");
    MCGuiWait("its entries are listed", 20, [] { return row("a-files") && row("big") && row("churn"); });
    // The files to click sort above the churning folder, so its rows coming
    // and going never move them under the pointer.
    clickRow("a-files");
    MCGuiWait("the files are listed", 20, [] { return row("note.txt") && row("pipe"); });
    MCGuiStep("", [] { checkPhase("opening two folders"); });

    // A folder whose entries change many times a second, open, while files
    // are clicked: a process makes four files, renames them, and removes them,
    // over and over, as a build does.
    MCGuiStep("a process churns a folder", [] {
        GPid pid = 0;
        const char* argv[] = {"sh", "-c",
                              "cd a-tree-stall/churn || exit 1; while :; do "
                              "for i in 0 1 2 3; do : > t$i; mv t$i o$i; done; rm -f o0 o1 o2 o3; done",
                              nullptr};
        GError* err = nullptr;
        const bool ok = g_spawn_async(MCGuiProject().c_str(), const_cast<char**>(argv), nullptr,
            (GSpawnFlags)(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD),
            [](gpointer) { setpgid(0, 0); }, nullptr, &pid, &err);
        GCHECK(ok);
        if (err) MCGuiNote("%s", err->message);
        g_clear_error(&err);
        gChurn = pid;
    });
    clickRow("churn");
    // A pause where time is the condition: the folder churns while open.
    MCGuiPause(2);
    MCGuiStep("", [] { checkPhase("with a churning folder open"); });
    clickRow("note.txt");
    MCGuiWait("the note opens", 10, [] { return MCGuiApp().editor->currentPath() == at("a-files/note.txt"); });
    clickRow("data.bin");
    MCGuiWait("the binary is refused", 10, [] { return MCGuiApp().editor->currentPath() == at("a-files/data.bin"); });
    clickRow("pipe");
    MCGuiWait("the pipe is refused, never opened", 10,
              [] { return MCGuiApp().editor->currentPath() == at("a-files/pipe"); });
    MCGuiStep("a double-click on the note", [] { MCGuiDoubleClick(row("note.txt")); });
    MCGuiWait("the note is back", 10, [] { return MCGuiApp().editor->currentPath() == at("a-files/note.txt"); });
    MCGuiPause(1);   // time is the condition: still churning
    MCGuiStep("", [] { checkPhase("clicking files while a folder churns"); });
    MCGuiStep("the churn stops", [] { stopChurn(); });
    clickRow("churn");   // closed again, so the big folder's rows are on show
    MCGuiPause(0.5);     // time is the condition: the last events arrive
    MCGuiStep("", [] { checkPhase("after the churn"); });

    // A folder of 10,000 entries, open, with a folder open inside it and the
    // file in that folder selected.
    MCGuiStep("", [] { gRowsOpen = rowCount(); });
    clickRow("big");
    // All of them, not a pause in their coming: the folder is listed on a
    // worker, and with three runs and four busy loops at once no row had
    // come for half a second when the count was taken, which then made
    // every later count wrong (their rows "go" waited 60 s for a count the
    // tree could not reach).
    MCGuiWait("its rows arrive", 30, [] { return rowCount() == gRowsOpen + kBigFiles + 1; });
    MCGuiStep("", [] {
        GCHECK(rowCount() == gRowsOpen + kBigFiles + 1);
        checkPhase("opening a folder of 10,000 entries");
    });
    clickRow("keep");
    clickRow("inside.txt");
    MCGuiWait("it opens", 10, [] { return MCGuiApp().editor->currentPath() == at("big/keep/inside.txt"); });
    MCGuiStep("", [] {
        gRowsOpen = rowCount();
        GCHECK_EQ_STR(MCGuiApp().tree->selectedPath(), at("big/keep/inside.txt"));
        checkPhase("opening the folder inside it");
    });

    // Another program deletes the 10,000 files in one go.
    MCGuiAsync("rm deletes them", 60, [](MCGuiDone done) {
        MCGuiRunProcess({"sh", "-c", "cd a-tree-stall/big && rm -f f*"}, {},
                        [done](int status, std::string, std::string) {
                            GCHECK(status == 0);
                            done();
                        });
    });
    MCGuiWait("their rows go", 60, [] { return rowCount() == gRowsOpen - kBigFiles; });
    MCGuiPause(0.5);   // time is the condition: nothing more arrives
    MCGuiStep("", [] {
        checkPhase("while 10,000 files of an open folder are deleted");
        GCHECK(rowCount() == gRowsOpen - kBigFiles);
        // The folder inside stays open, and its file selected.
        GCHECK(row("inside.txt") != nullptr);
        GCHECK_EQ_STR(MCGuiApp().tree->selectedPath(), at("big/keep/inside.txt"));
    });

    // And makes them again.
    MCGuiAsync("a shell makes them again", 60, [](MCGuiDone done) {
        MCGuiRunProcess({"sh", "-c",
                         "cd a-tree-stall/big && i=0; while [ $i -lt 10000 ]; do : > f$i; i=$((i+1)); done"},
                        {}, [done](int status, std::string, std::string) {
                            GCHECK(status == 0);
                            done();
                        });
    });
    MCGuiWait("their rows come back", 60, [] { return rowCount() == gRowsOpen; });
    MCGuiPause(0.5);   // time is the condition: nothing more arrives
    MCGuiStep("", [] {
        checkPhase("while 10,000 files of an open folder are made");
        GCHECK(rowCount() == gRowsOpen);
        GCHECK(row("inside.txt") != nullptr);
        GCHECK_EQ_STR(MCGuiApp().tree->selectedPath(), at("big/keep/inside.txt"));
        // In the tree's order: the open folder's file, then f0, f1, f10.
        const double keep = rowY("inside.txt"), f0 = rowY("f0"), f1 = rowY("f1"), f10 = rowY("f10");
        GCHECK(keep >= 0 && f0 > keep && f1 > f0 && f10 > f1);
    });
    clickRow("f100");
    MCGuiWait("a new file opens", 10, [] { return MCGuiApp().editor->currentPath() == at("big/f100"); });
    MCGuiStep("", [] { checkPhase("opening a file in it"); });
    MCGuiStep("the meter stops, and the big folder closes", [] {
        stopChurn();
        gMeter.stop();
        MCGuiClick(row("big"));
    });
    MCGuiWait("closed", 10, [] { return row("f0") == nullptr; });
}
