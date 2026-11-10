// FoldPerfSuite.cpp — `./build/minicode-guitest foldperf`: folding's timings
// (linux/PARITY.md item 1.10, src/Folds.h), the counterpart of the Mac's
// foldperf in tests/gui/FoldingSuite.mm. 40,000 lines of Python, 20,000
// regions (100,000 lines and 50,000 regions with MC_GUI_FULL_PERF=1, as on
// the Mac): Fold All closes what fits under D8's limit of 5,000 hidden lines
// (the outermost first, from the top) and says how many stayed open, in one
// merge, within a bound of the main thread's CPU time; a keystroke among the
// folds; Unfold All, one removal of the tag, within a bound too. GTK then
// measures the lines it hid or showed again in idle slices; that is noted,
// not bounded (the Mac notes its Unfold All for the same reason).
#include "Harness.h"

#include "BufferLines.h"
#include "CodeView.h"
#include "Document.h"
#include "Editor.h"
#include "Folds.h"
#include "Gutter.h"

#include <cstdlib>
#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
GtkTextView* tv() { return GTK_TEXT_VIEW(ed()->textView()); }
GtkTextBuffer* buf() { return ed()->buffer(); }
DocumentFolds* folds() { return DocumentFolds::of(buf()); }
Gutter* gut() { return ed()->codeView()->gutter(); }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

int defs() {
    const char* full = getenv("MC_GUI_FULL_PERF");
    return full && *full ? 25000 : 10000;
}

std::string statusText() { return gtk_label_get_text(GTK_LABEL(MCGuiApp().statusLabel)); }

double gCpu0 = 0;
std::string gText;
const int kTyped = 3;

}  // namespace

MC_GUI_SUITE(foldperf) {
    MCGuiStep("a long file of small functions", [] {
        ed()->closeAllTabs();
        gText.clear();
        for (int i = 0; i < defs(); i++)
            gText += "def f" + std::to_string(i) + "(x):\n    if x:\n        return " +
                     std::to_string(i) + "\n\n";
        MCGuiNote("%d lines, %d regions", defs() * 4, defs() * 2);
        MCGuiWriteFile(F("big.py"), gText);
        MCGuiOpenFileKept(F("big.py"));
    });
    MCGuiWait("its regions, off the main thread", 30, [] {
        return ed()->currentPath() == F("big.py") && folds() && folds()->regionsCurrent() &&
               folds()->startsRegion((size_t)(4 * (defs() - 1)));
    });
    MCGuiStep("the keyboard in the text, the window focused", [] {
        MCGuiSetKeyWindow(nullptr);   // an earlier suite may have left another one focused
        MCGuiFocusKeyWindow();
        MCGuiClick(ed()->textView());
    });
    MCGuiWaitQuiet("GTK has laid it out", 0.5, 60, [] { return (long)(MCGuiThreadCpuMs() / 20); });
    MCGuiStep("Fold All", [] {
        const unsigned computes = folds()->computes();
        const double c0 = MCGuiThreadCpuMs();
        GCHECK(ed()->folds()->run([](DocumentFolds* f) { return f->foldAll(); }));
        const double cpu = MCGuiThreadCpuMs() - c0;
        MCGuiNote("Fold All of %d regions: %.0f ms of the main thread's CPU", defs() * 2, cpu);
        MCGuiNote("%s", statusText().c_str());
        GCHECK(folds()->computes() == computes);   // the regions were current: none worked out again
        // The top of the file: 2,500 functions and their ifs, 5,000 lines.
        GCHECK(folds()->hiddenLines() == DocumentFolds::kMaxHiddenLines);
        GCHECK(folds()->collapsed().size() == 5000 && folds()->outermost().size() == 2500);
        GCHECK(folds()->startsClosedFold(0) && folds()->startsClosedFold(4 * 2499));
        GCHECK(!folds()->startsClosedFold(4 * 2500));
        const std::string open = defs() > 10000 ? "45,000" : "15,000";
        GCHECK(statusText().find("Folded 5,000 regions; " + open + " stayed open") != std::string::npos);
        GCHECK_TIME(cpu, defs() > 10000 ? 3000 : 1500, "Fold All");
        gCpu0 = MCGuiThreadCpuMs();
    });
    MCGuiWaitQuiet("GTK has measured the lines again", 0.5, 60,
                   [] { return (long)(MCGuiThreadCpuMs() / 20); });
    MCGuiStep("a keystroke among the folds, with the view there", [] {
        MCGuiNote("GTK measuring the lines again afterwards: %.0f ms of CPU", MCGuiThreadCpuMs() - gCpu0);
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_offset(buf(), &it,
                                           (int)BufferLines::of(buf())->lineStart(4 * 1000) + 5);
        gtk_text_buffer_place_cursor(buf(), &it);
        gtk_text_view_scroll_to_mark(tv(), gtk_text_buffer_get_insert(buf()), 0, TRUE, 0, 0.5);
    });
    MCGuiWait("on screen", 20, [] {
        GdkRectangle vis;
        gtk_text_view_get_visible_rect(tv(), &vis);
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_mark(buf(), &it, gtk_text_buffer_get_insert(buf()));
        int y = 0, h = 0;
        gtk_text_view_get_line_yrange(tv(), &it, &y, &h);
        if (h <= 0 || y < vis.y || y + h > vis.y + vis.height) {
            gtk_text_view_scroll_to_mark(tv(), gtk_text_buffer_get_insert(buf()), 0, TRUE, 0, 0.5);
            return false;
        }
        return true;
    });
    MCGuiWaitQuiet("still", 0.5, 30, [] { return (long)(MCGuiThreadCpuMs() / 20); });
    MCGuiStep("typed", [] {
        gCpu0 = MCGuiThreadCpuMs();
        MCGuiType(std::string(kTyped, 'z'));
    });
    MCGuiWait("in", 10, [] { return MCGuiEditorText().find("def fzzz1000(x):") != std::string::npos; });
    MCGuiWaitQuiet("drawn", 0.3, 10, [] { return (long)gut()->drawCount(); });
    MCGuiStep("within a CPU bound a key; Unfold All", [] {
        const double total = MCGuiThreadCpuMs() - gCpu0;
        MCGuiNote("%d keys among %zu folds: %.1f ms of the main thread (%.2f a key)", kTyped,
                  folds()->collapsed().size(), total, total / kTyped);
        GCHECK_TIME(total, 50.0 * kTyped, "a keystroke among the folds");
        GCHECK(folds()->collapsed().size() == 5000);
        const double c0 = MCGuiThreadCpuMs();
        GCHECK(ed()->folds()->run([](DocumentFolds* f) { return f->unfoldAll(); }));
        const double cpu = MCGuiThreadCpuMs() - c0;
        MCGuiNote("Unfold All of 5,000 folds: %.0f ms of CPU", cpu);
        GCHECK(!folds()->hasClosedFolds());
        GCHECK_TIME(cpu, 500, "Unfold All");
        gCpu0 = MCGuiThreadCpuMs();
    });
    MCGuiWaitQuiet("GTK has measured the lines again", 0.5, 60,
                   [] { return (long)(MCGuiThreadCpuMs() / 20); });
    MCGuiStep("undone, closed", [] {
        MCGuiNote("GTK measuring the lines again afterwards: %.0f ms of CPU", MCGuiThreadCpuMs() - gCpu0);
        for (int i = 0; i < kTyped; i++) MCGuiKey("ctrl+z");
    });
    MCGuiWait("the file again", 10, [] { return MCGuiEditorText() == gText; });
    MCGuiStep("", [] { ed()->closeAllTabs(); });
}
