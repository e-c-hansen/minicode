// LargeMemorySuite.cpp — `./build/mutible-guitest large_memory`: what a big
// file leaves in the process once the find bar is closed and its tab is
// gone. The find bar's copy of the text goes with the bar, and glibc is told
// to give freed memory back after a big tab closes (Editor::destroy).
// Pss comes from /proc/self/smaps_rollup; it is noted, and held to a bound
// only where the trim makes it reliable.

#include "Harness.h"

#include "DocumentView.h"
#include "Editor.h"
#include "FindBar.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
FindBar* bar() { return MCGuiApp().findBar; }
std::string path() { return MCGuiScratch() + "/large-memory/big.log"; }

double pssMb() {
    FILE* f = fopen("/proc/self/smaps_rollup", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "Pss: %ld kB", &kb) == 1) break;
    fclose(f);
    return kb / 1024.0;
}

const double kFileMb = 30;
double gBefore = 0, gOpen = 0, gFound = 0, gBarClosed = 0, gBarLater = 0, gTabClosed = 0;
int gTrims0 = 0;

}  // namespace

MC_GUI_SUITE(large_memory) {
    MCGuiStep("a 30 MB log, 80 bytes a line", [] {
        MCGuiSetSettings("lsp.enabled = false\n");
        ed()->closeAllTabs();
        std::string s;
        s.reserve(31u << 20);
        char buf[128];
        for (int i = 0; s.size() < (size_t)(kFileMb * 1048576); i++) {
            snprintf(buf, sizeof buf, "2026-11-21 12:00:00 INFO worker-%04d request %08d served in %05d ms ok\n",
                     i % 1000, i, i % 99999);
            s += buf;
        }
        s += "the final needlexyz line\n";
        MCGuiWriteFile(path(), s);
        gBefore = pssMb();
        gTrims0 = Editor::trimsDone();
        MCGuiOpenFileKept(path());
    });
    MCGuiWait("it is in", 120, [] {
        Document* d = ed()->active();
        return d && d->path() == path() && !d->loading() && ed()->isSource();
    });
    MCGuiWait("laid out", 120, [] { return MCGuiTextLaidOut(ed()->textView()); });
    MCGuiStep("the find bar searches for a word on the last line", [] {
        gOpen = pssMb();
        bar()->showFind();
        gtk_editable_set_text(GTK_EDITABLE(bar()->findField()), "needlexyz");
    });
    MCGuiWait("found", 60, [] {
        return bar()->query() == "needlexyz" && bar()->resultsCurrent() && !bar()->matches().empty();
    });
    MCGuiStep("the bar holds a copy; closing it lets go", [] {
        gFound = pssMb();
        GCHECK(bar()->holdsText());
        GCHECK(bar()->closeFromEditor());
        GCHECK(!bar()->holdsText());
        gBarClosed = pssMb();
    });
    MCGuiPause(1);   // time is the condition: a search still ending lets go of its copy
    MCGuiStep("a moment after", [] {
        gBarLater = pssMb();
        MCGuiNote("Pss MB: bar just closed %.0f, a second later %.0f", gBarClosed, gBarLater);
    });
    MCGuiStep("a second search after reopening still works", [] {
        bar()->showFind();
        gtk_editable_set_text(GTK_EDITABLE(bar()->findField()), "needlexy");
    });
    MCGuiWait("found again", 60, [] {
        return bar()->query() == "needlexy" && bar()->resultsCurrent() && !bar()->matches().empty();
    });
    MCGuiStep("closed again, then the tab", [] {
        GCHECK(bar()->closeFromEditor());
        GCHECK(!bar()->holdsText());
        gBarClosed = pssMb();
        ed()->closeTab(ed()->active());
    });
    MCGuiWait("the trim ran", 30, [] { return Editor::trimsDone() > gTrims0; });
    MCGuiPause(1.5);   // time is the condition: the kernel's accounting settles
    MCGuiStep("what stays", [] {
        gTabClosed = pssMb();
        MCGuiNote("Pss MB: before %.0f, open %.0f, found %.0f, bar closed %.0f, tab closed %.0f (file %.0f)",
                  gBefore, gOpen, gFound, gBarClosed, gTabClosed, kFileMb);
#ifdef __GLIBC__
        // 77 to 91 MB stay (2.6 to 3 times the file: the heap's holes, which
        // a trim cannot give back); 257 MB (8.6 times) without the trims. The
        // bound leaves room for a heap other suites fragmented first (CI, the
        // soak's orders) and still catches the trims gone.
        GCHECK(gTabClosed - gBefore < 5 * kFileMb);
        // The bar's copy went back too: its 30 MB, a trim after its close.
        GCHECK(gBarLater < gFound - 0.5 * kFileMb);
#endif
        std::remove(path().c_str());
    });
}
