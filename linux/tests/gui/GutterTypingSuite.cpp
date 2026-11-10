// GutterTypingSuite.cpp — `./build/minicode-guitest gutter_typing`: what the
// gutter (linux/PARITY.md item 1.4) costs while typing in a 100,000-line
// file, the counterpart of the Mac's KeystrokeSuite for the line numbers.
// Characters and Returns typed through XTest in the middle of the file; the
// numbers on screen are then held to an index made afresh, and the CPU time
// of the main thread is bounded twice: what the gutter's own handlers and
// drawing took (BufferLines and Gutter keep it), which must not grow with
// the file (an index counted through the buffer again per key, 2.7 MB of
// it, would), and the whole thread per key, which catches a gutter that
// made GTK lay out or measure everything per key.
#include "Harness.h"

#include "BufferLines.h"
#include "CodeView.h"
#include "Editor.h"
#include "Gutter.h"
#include "LineIndex.h"

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
Gutter* gut() { return ed()->codeView()->gutter(); }
GtkTextView* tv() { return GTK_TEXT_VIEW(ed()->textView()); }
GtkTextBuffer* buf() { return ed()->buffer(); }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

const int kLines = 100000;
const std::string kTyped = "typed here";   // and a Return after each, five times
const int kKeys = 5 * ((int)kTyped.size() + 1);

double gCpu0 = 0, gGutter0 = 0;

std::string bufferText() {
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buf(), &a, &b);
    char* t = gtk_text_buffer_get_text(buf(), &a, &b, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

size_t caretLine() {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(buf(), &it, gtk_text_buffer_get_insert(buf()));
    return BufferLines::of(buf())->lineOf((size_t)gtk_text_iter_get_offset(&it));
}

// The rows drawn are the lines on screen, numbered as a fresh index numbers
// them, the caret's active.
bool rowsRight() {
    const std::string text = bufferText();
    LineIndex ix;
    ix.resetUtf8(text.data(), text.size());
    BufferLines* lines = BufferLines::of(buf());
    if (!lines || lines->lineCount() != ix.lineCount()) return false;
    const auto& rows = gut()->drawnRows();
    if (rows.empty()) return false;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(buf(), &it, gtk_text_buffer_get_insert(buf()));
    const size_t caret = ix.lineOf((size_t)gtk_text_iter_get_offset(&it));
    bool sawCaret = false;
    for (const Gutter::Row& r : rows) {
        if (lines->lineStart(r.line) != ix.lineStart(r.line)) return false;
        if (r.label != std::to_string(r.line + 1) || r.active != (r.line == caret)) return false;
        sawCaret = sawCaret || r.line == caret;
        GtkTextIter at;
        gtk_text_buffer_get_iter_at_offset(buf(), &at, (int)ix.lineStart(r.line));
        GdkRectangle loc;
        gtk_text_view_get_iter_location(tv(), &at, &loc);
        int wx = 0, wy = 0;
        gtk_text_view_buffer_to_window_coords(tv(), GTK_TEXT_WINDOW_LEFT, 0, loc.y, &wx, &wy);
        if (r.y != wy) return false;
    }
    return sawCaret;
}

}  // namespace

MC_GUI_SUITE(gutter_typing) {
    MCGuiStep("a file of 100,000 lines", [] {
        ed()->closeAllTabs();
        std::string big;
        big.reserve((size_t)kLines * 28);
        for (int i = 0; i < kLines; i++) big += "    value = compute(i) + 1\n";
        MCGuiWriteFile(F("gutter-big.py"), big);
        MCGuiOpenFileKept(F("gutter-big.py"));
    });
    MCGuiWait("open, its gutter six digits wide", 20, [] {
        return ed()->currentPath() == F("gutter-big.py") && gut()->shown() && gut()->digits() == 6 &&
               !gut()->drawnRows().empty();
    });
    // The scroll below waits, doing nothing, until GTK has laid out the
    // whole file (MCGuiTextLaidOut), which took more than its 20 s with
    // three runs and four busy loops at once. How long the layout takes is
    // the machine's load, not what is checked here.
    MCGuiWait("GTK has laid out the 100,000 lines", 120, [] { return MCGuiTextLaidOut(ed()->textView()); });
    MCGuiStep("the caret to line 50,000", [] {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(buf(), &it, 49999);
        gtk_text_buffer_place_cursor(buf(), &it);
    });
    // GTK scrolls towards where it guesses a line is in a file it has not
    // laid out, and lands short; asked again as the guesses firm up, it gets
    // there (Editor's settleOnCaret does the same for a search result).
    MCGuiWait("line 50,000 on screen, numbered and active", 20, [] {
        // Where GTK has the line, from its heights: asking where a character
        // is would lay out a line far off screen (CodeView::setBuffer).
        GdkRectangle vis;
        gtk_text_view_get_visible_rect(tv(), &vis);
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_mark(buf(), &it, gtk_text_buffer_get_insert(buf()));
        int ly = 0, lh = 0;
        gtk_text_view_get_line_yrange(tv(), &it, &ly, &lh);
        if (lh <= 0 || ly < vis.y || ly + lh > vis.y + vis.height) {
            gtk_text_view_scroll_to_mark(tv(), gtk_text_buffer_get_insert(buf()), 0, TRUE, 0, 0.5);
            return false;
        }
        return caretLine() == 49999 && rowsRight();
    });
    // Opening highlights in idle slices; the typing is measured from when the
    // main thread is idle again.
    MCGuiWaitQuiet("the main thread idle", 0.5, 30, [] { return (long)(MCGuiThreadCpuMs() / 20); });
    MCGuiStep("typing, with a Return after each word", [] {
        gCpu0 = MCGuiThreadCpuMs();
        gGutter0 = gutterCpuMs();
        gtk_widget_grab_focus(ed()->textView());
        for (int i = 0; i < 5; i++) MCGuiType(kTyped + "\n");
    });
    MCGuiWait("the text has it all", 30, [] {
        return BufferLines::of(buf())->lineCount() == (size_t)kLines + 6 && caretLine() == 50004;
    });
    MCGuiWaitQuiet("the gutter drawn for the last of it", 0.3, 10,
                   [] { return (long)gut()->drawCount(); });
    MCGuiStep("within bounds of CPU time", [] {
        const double total = MCGuiThreadCpuMs() - gCpu0;
        const double gutter = gutterCpuMs() - gGutter0;
        MCGuiNote("%d keys: %.1f ms of the main thread (%.2f a key), %.2f ms of it the gutter's "
                  "(%.3f a key)", kKeys, total, total / kKeys, gutter, gutter / kKeys);
        // The gutter's own work stays well under a millisecond a key at
        // 100,000 lines (0.05 ms on the ThinkPad): an index counted through
        // the buffer again per key cost 38. The whole thread took 8 to 10 ms
        // a key there, gutter or not; the bound leaves room for a slower
        // runner and still catches a relayout of the file per key.
        GCHECK_TIME(gutter, 0.5 * kKeys, "the gutter's CPU time over the typing");
        GCHECK_TIME(total, 50.0 * kKeys, "the main thread's CPU time over the typing");
    });
    MCGuiWait("the numbers on screen are right, the caret's line active", 10, rowsRight);
    MCGuiStep("and the text is as typed", [] {
        const std::string text = bufferText();
        GCHECK(text.find("    value = compute(i) + 1\n" + kTyped + "\n" + kTyped + "\n") !=
               std::string::npos);
    });
    MCGuiStep("the tab closes", [] { ed()->closeAllTabs(); });
}
