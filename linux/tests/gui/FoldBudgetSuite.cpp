// FoldBudgetSuite.cpp — `./build/minicode-guitest fold_budget`: D8's limit on
// what a tab's folds hide (linux/PARITY.md item 1.10, src/Folds.h), with
// real keys. GTK's drawing visits every line between the first and the
// last line on screen, hidden ones too, so the folds closed in a tab hide
// at most 5,000 lines between them. Here 25 functions of 200 lines and one
// of 3: Fold All (Ctrl+Alt+0) closes the 25 (5,000 lines, all on one
// screen) and leaves the last open, saying so; Ctrl+Shift+[ on it stays
// open with a note saying why, and folds once another fold opened makes
// room. At the limit, a frame of the text costs GTK a bounded amount of
// CPU (26 ms on the ThinkPad when D8 was tried), the gutter visits only the
// lines on show, and typing among the folds stays within a CPU bound a key.
#include "Harness.h"

#include "BufferLines.h"
#include "CodeView.h"
#include "Document.h"
#include "Editor.h"
#include "Folds.h"
#include "Gutter.h"

#include <algorithm>
#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
GtkTextView* tv() { return GTK_TEXT_VIEW(ed()->textView()); }
GtkTextBuffer* buf() { return ed()->buffer(); }
DocumentFolds* folds() { return DocumentFolds::of(buf()); }
Gutter* gut() { return ed()->codeView()->gutter(); }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

const int kDefs = 25, kBody = 200;   // 25 x 200 = 5,000 hidden lines, the limit

std::string budgetText() {
    std::string s;
    for (int i = 0; i < kDefs; i++) {
        s += "def f" + std::to_string(i) + "(x):\n";
        for (int k = 0; k < kBody; k++) s += "    x = x + " + std::to_string(k) + "\n";
    }
    s += "def tail():\n    a = 1\n    b = 2\n    return a + b\n";
    return s;
}
const std::string kText = budgetText();

// 0-based lines.
size_t defLine(int i) { return (size_t)i * (kBody + 1); }
size_t tailLine() { return defLine(kDefs); }
int lineStart(size_t line) { return (int)BufferLines::of(buf())->lineStart(line); }

void caretAt(int at) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, at);
    gtk_text_buffer_select_range(buf(), &it, &it);
}

std::string statusText() { return gtk_label_get_text(GTK_LABEL(MCGuiApp().statusLabel)); }

// One frame of the text as GTK draws it, timed in CPU: what D8 measured.
// The view is marked for drawing first, or GTK hands back its last picture.
double frameCpuMs() {
    GtkWidget* view = ed()->textView();
    gtk_widget_queue_draw(view);
    const double t0 = MCGuiThreadCpuMs();
    GtkSnapshot* s = gtk_snapshot_new();
    gtk_widget_snapshot_child(gtk_widget_get_parent(view), view, s);
    GskRenderNode* node = gtk_snapshot_free_to_node(s);
    const double t = MCGuiThreadCpuMs() - t0;
    if (node) gsk_render_node_unref(node);
    return t;
}

// The lines GTK has on screen hide between them, by the index.
size_t hiddenOnScreen() {
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(tv(), &vis);
    GtkTextIter top, bottom;
    int ignore = 0;
    gtk_text_view_get_line_at_y(tv(), &top, vis.y, &ignore);
    gtk_text_view_get_line_at_y(tv(), &bottom, vis.y + vis.height - 1, &ignore);
    BufferLines* l = BufferLines::of(buf());
    const size_t a = l->lineOf((size_t)gtk_text_iter_get_offset(&top));
    const size_t b = l->lineOf((size_t)gtk_text_iter_get_offset(&bottom));
    size_t shown = 0;
    for (const Gutter::Row& r : gut()->drawnRows())
        if (r.line >= a && r.line <= b) shown++;
    return b - a + 1 - shown;
}

double gBaseline = 0, gCpu0 = 0;
const int kTyped = 6;

}  // namespace

MC_GUI_SUITE(fold_budget) {
    MCGuiStep("25 functions of 200 lines and one of 3", [] {
        ed()->closeAllTabs();
        MCGuiWriteFile(F("budget.py"), kText);
        MCGuiOpenFileKept(F("budget.py"));
    });
    MCGuiWait("its regions", 10, [] {
        return ed()->currentPath() == F("budget.py") && folds() && folds()->regionsCurrent() &&
               folds()->startsRegion(tailLine());
    });
    MCGuiStep("the keyboard in the text, the window focused", [] {
        MCGuiSetKeyWindow(nullptr);   // an earlier suite may have left another one focused
        MCGuiFocusKeyWindow();
        MCGuiClick(ed()->textView());
    });
    MCGuiWait("the editor has it", 5, [] { return gtk_widget_has_focus(ed()->textView()); });
    MCGuiWaitQuiet("GTK has laid it out", 0.5, 30, [] { return (long)(MCGuiThreadCpuMs() / 20); });
    MCGuiStep("a frame unfolded, for comparison; Ctrl+Alt+0", [] {
        gBaseline = frameCpuMs();
        MCGuiNote("a frame of the text with nothing folded: %.2f ms of CPU", gBaseline);
        caretAt(0);
        MCGuiKey("ctrl+alt+0");
    });
    MCGuiWait("the 25 folded, the last left open, and the status bar says so", 5, [] {
        return folds()->outermost().size() == (size_t)kDefs && !folds()->startsClosedFold(tailLine()) &&
               statusText().find("Folded 25 regions; 1 stayed open") != std::string::npos;
    });
    MCGuiStep("at the limit, not past it", [] {
        MCGuiNote("%s", statusText().c_str());
        GCHECK(statusText().find("5,000 lines at most") != std::string::npos);
        GCHECK(folds()->hiddenLines() == DocumentFolds::kMaxHiddenLines);
        GCHECK(folds()->startsClosedFold(defLine(0)) && folds()->startsClosedFold(defLine(kDefs - 1)));
    });
    MCGuiWaitQuiet("GTK has measured the hidden lines again", 0.5, 30,
                   [] { return (long)(MCGuiThreadCpuMs() / 20); });
    MCGuiWait("every folded function on one screen", 5, [] {
        const auto& rows = gut()->drawnRows();
        return rows.size() >= (size_t)kDefs && rows.front().line == 0 &&
               rows[kDefs - 1].line == defLine(kDefs - 1);
    });
    MCGuiStep("a frame there: within a CPU bound, and the gutter walks only what shows", [] {
        MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "fold-budget");
        const size_t hidden = hiddenOnScreen();
        double worst = 0;
        for (int k = 0; k < 3; k++) worst = std::max(worst, frameCpuMs());
        MCGuiNote("%zu lines hidden on screen: a frame of the text %.2f ms of CPU at worst of 3 "
                  "(%.2f with nothing folded); the gutter walked %u lines for %zu rows",
                  hidden, worst, gBaseline, gut()->linesWalked(), gut()->drawnRows().size());
        GCHECK(hidden >= 4000);
        // D8 measured 26 ms here; a loaded machine may take several times
        // that, and an unbounded cost (every line laid out) far more.
        GCHECK_TIME(worst, 250, "a frame with 5,000 hidden lines on screen");
        GCHECK(gut()->linesWalked() <= gut()->drawnRows().size() + 2);
    });

    // ---- typing among the folds, at the limit
    MCGuiStep("typing in the name of the 12th function", [] {
        caretAt(lineStart(defLine(12)) + 5);   // "def f|12"
        gCpu0 = MCGuiThreadCpuMs();
        MCGuiType(std::string(kTyped, 'z'));
    });
    MCGuiWait("typed", 10, [] {
        return MCGuiEditorText().find("def fzzzzzz12(x):") != std::string::npos;
    });
    MCGuiWaitQuiet("drawn", 0.3, 10, [] { return (long)gut()->drawCount(); });
    MCGuiStep("within a CPU bound a key, the folds as they were", [] {
        const double total = MCGuiThreadCpuMs() - gCpu0;
        MCGuiNote("%d keys among 5,000 hidden lines on screen: %.1f ms of the main thread "
                  "(%.2f a key)", kTyped, total, total / kTyped);
        GCHECK_TIME(total, 100.0 * kTyped, "the main thread's CPU time over the typing");
        GCHECK(folds()->outermost().size() == (size_t)kDefs);
        for (int i = 0; i < kTyped; i++) MCGuiKey("ctrl+z");
    });
    MCGuiWait("undone", 10, [] { return MCGuiEditorText() == kText; });

    // ---- one fold past the limit
    MCGuiStep("Ctrl+Shift+[ on the last function", [] {
        caretAt(lineStart(tailLine() + 1));
        MCGuiKey("ctrl+shift+[");
    });
    MCGuiWait("it stays open, and the status bar says why", 5, [] {
        return statusText().find("Not folded") != std::string::npos;
    });
    MCGuiStep("", [] {
        MCGuiNote("%s", statusText().c_str());
        GCHECK(statusText().find("5,003 lines") != std::string::npos);
        GCHECK(!folds()->startsClosedFold(tailLine()));
        GCHECK(folds()->hiddenLines() == DocumentFolds::kMaxHiddenLines);
        // Room made: the first function unfolded with Ctrl+Shift+].
        caretAt(lineStart(defLine(0)));
        MCGuiKey("ctrl+shift+]");
    });
    MCGuiWait("the first open", 5, [] { return !folds()->startsClosedFold(defLine(0)); });
    MCGuiStep("now it fits", [] {
        caretAt(lineStart(tailLine() + 1));
        MCGuiKey("ctrl+shift+[");
    });
    MCGuiWait("folded", 5, [] {
        return folds()->startsClosedFold(tailLine()) &&
               folds()->hiddenLines() == DocumentFolds::kMaxHiddenLines - kBody + 3;
    });
    MCGuiStep("unfolded, closed", [] {
        MCGuiKey("ctrl+alt+j");
    });
    MCGuiWait("nothing folded", 5, [] { return !folds()->hasClosedFolds(); });
    MCGuiStep("", [] { ed()->closeAllTabs(); });
}
