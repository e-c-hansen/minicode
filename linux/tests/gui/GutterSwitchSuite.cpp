// GutterSwitchSuite.cpp — `./build/minicode-guitest gutter_switch`: a tab
// shown and closed before GTK has laid out all of its lines, many times,
// with the editor's drawing (the gutter, CodeView's passes) running between.
// GtkTextLayout keeps a cache of line displays; when the view leaves a
// buffer it drops only the displays of lines it had laid out, so a display
// made for any other line (the gutter once asked for the line below the
// screen; a check on a caret far off screen does, which the suite does
// itself) outlived the buffer, and the next display the cache sorted read
// the freed line: CI's GTK job crashed in `tabs` (2026-11-10). CodeView
// empties the cache before the view leaves a buffer. Freed memory reads
// back as garbage only some of the time, so this check counts under
// MALLOC_PERTURB_ (it crashed in its first round without the fix, three runs
// of three) or AddressSanitizer (a segfault: GTK is not instrumented);
// plainly it may pass either way.
//
// Then a short file opened while the view is far down a long one (or a
// Markdown page) must show its first line: GtkTextView kept the old scroll
// offset across set_buffer and once laid out the new text came up past its
// end, the adjustment at 0 and the gutter's top number the short file's last
// line (CodeView::setViewBuffer; the nightly soak's `symbols` then
// `brackets` left `brackets` with a blank editor). It failed in all four
// rounds without the fix.
#include "Harness.h"

#include "CodeView.h"
#include "DocumentView.h"
#include "Editor.h"
#include "Gutter.h"

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
Gutter* gut() { return ed()->codeView()->gutter(); }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

unsigned gDraws = 0;
int gRound = 0;

}  // namespace

MC_GUI_SUITE(gutter_switch) {
    MCGuiStep("a long file and a short one", [] {
        ed()->closeAllTabs();
        std::string big;
        for (int i = 0; i < 40000; i++) big += "line " + std::to_string(i) + " of the long file\n";
        MCGuiWriteFile(F("switch-long.txt"), big);
        MCGuiWriteFile(F("switch-short.txt"), "short\n");
        MCGuiOpenFileKept(F("switch-short.txt"));
    });
    MCGuiWait("the short one on show", 10, [] { return ed()->currentPath() == F("switch-short.txt"); });
    for (int round = 0; round < 12; round++) {
        MCGuiStep("the long file, far down", [] {
            gDraws = gut()->drawCount();
            MCGuiOpenFileKept(F("switch-long.txt"));
            GtkTextBuffer* b = ed()->buffer();
            GtkTextIter it;
            gtk_text_buffer_get_iter_at_line(b, &it, 30000);
            gtk_text_buffer_place_cursor(b, &it);
            gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(ed()->textView()),
                                         gtk_text_buffer_get_insert(b), 0, TRUE, 0, 0.5);
        });
        MCGuiWait("drawn with its gutter", 10, [] {
            return ed()->currentPath() == F("switch-long.txt") && gut()->drawCount() > gDraws;
        });
        MCGuiStep("closed at once, the short one on show again", [] {
            // Where a far line is, as a check on a caret off screen asks
            // (Editor's settling does): GTK lays that line out for it alone.
            GtkTextBuffer* b0 = ed()->buffer();
            GtkTextIter far;
            gtk_text_buffer_get_iter_at_line(b0, &far, 39990 - 1000 * (gRound++ % 3));
            GdkRectangle where;
            gtk_text_view_get_iter_location(GTK_TEXT_VIEW(ed()->textView()), &far, &where);
            ed()->closeTab(ed()->documentFor(F("switch-long.txt")));
            GtkTextBuffer* b = ed()->buffer();
            GtkTextIter end;
            gtk_text_buffer_get_end_iter(b, &end);
            gtk_text_buffer_place_cursor(b, &end);
            gtk_widget_grab_focus(ed()->textView());
        });
        MCGuiWait("the short one, drawn", 10, [] {
            return ed()->currentPath() == F("switch-short.txt") && ed()->tabCount() == 1;
        });
        MCGuiStep("typing in it lays its line out again", [] { MCGuiType("x"); });
        MCGuiWait("the text has it", 10, [] {
            return ed()->active() && ed()->active()->text().find("short\nx") != std::string::npos;
        });
    }
    MCGuiStep("the short one has every x", [] {
        GCHECK(ed()->active() && ed()->active()->text() == "short\nxxxxxxxxxxxx");
        ed()->closeAllTabs();
    });

    // A file opened in a new tab while the view was far down another: its
    // first line at the top, numbered 1 in the gutter, whether the far place
    // was a long file's source (Go to Line) or a Markdown page's (set on the
    // adjustment, as the palette's symbol jump does), and whether the short
    // file is opened beside it or after every tab was closed.
    for (int round = 0; round < 4; round++) {
        MCGuiStep("far down a long file", [round] {
            ed()->closeAllTabs();
            if (round == 0) {
                std::string md;
                for (int i = 0; i < 400; i++) md += "## Heading " + std::to_string(i) + "\n\nText.\n\n";
                MCGuiWriteFile(F("switch-long.md"), md);
            }
            if (round % 2 == 0) {
                MCGuiOpenFileKept(F("switch-long.txt"));
                ed()->revealLine(30000, 0, 0);
            } else {
                MCGuiOpenFileKept(F("switch-long.md"));
            }
        });
        MCGuiWait("far down", 10, [round] {
            GtkAdjustment* v = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(ed()->textView()));
            if (round % 2 == 1 && ed()->inPreview() && gtk_adjustment_get_upper(v) > 5000)
                gtk_adjustment_set_value(v, gtk_adjustment_get_upper(v) - gtk_adjustment_get_page_size(v));
            if (gtk_adjustment_get_value(v) <= 3000) return false;
            return round % 2 == 1 ||
                   (!gut()->drawnRows().empty() && gut()->drawnRows().front().line > 100);
        });
        MCGuiStep("a short file opened, the keyboard in it", [round] {
            gDraws = gut()->drawCount();
            if (round >= 2) ed()->closeAllTabs();
            // The settings applied again lay the text out again, as when the
            // soak's next suite set its own.
            MCGuiSetSettings(round % 2 ? "lsp.enabled = false\n" : "lsp.enabled = false\n\n");
            MCGuiWriteFile(F("switch-new.py"), "first = 1\nsecond = 2\nthird = 3\n");
            MCGuiOpenFileKept(F("switch-new.py"));
            gtk_widget_grab_focus(ed()->textView());
            GtkTextIter it;
            gtk_text_buffer_get_start_iter(ed()->buffer(), &it);
            gtk_text_buffer_place_cursor(ed()->buffer(), &it);
            MCGuiKey("right");
            MCGuiKey("right");
        });
        MCGuiWait("drawn", 10, [] {
            return ed()->currentPath() == F("switch-new.py") && gut()->drawCount() > gDraws;
        });
        MCGuiWaitQuiet("the scroll settles", 0.5, 10, [] {
            return (long)gtk_adjustment_get_value(
                gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(ed()->textView())));
        });
        MCGuiStep("its first line at the top, numbered 1", [round] {
            GtkTextView* tv = GTK_TEXT_VIEW(ed()->textView());
            GdkRectangle vis;
            gtk_text_view_get_visible_rect(tv, &vis);
            const auto& rows = gut()->drawnRows();
            const double v = gtk_adjustment_get_value(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tv)));
            MCGuiNote("round %d: scrolled to %.0f, visible from y %d, %zu rows drawn, the first %s",
                      round, v, vis.y, rows.size(), rows.empty() ? "none" : rows.front().label.c_str());
            // At the top, the view and its adjustment agreeing: the view
            // starts in the top margin, above the first line.
            GCHECK(v == 0);
            GCHECK(vis.y == -gtk_text_view_get_top_margin(tv));
            GCHECK(rows.size() == 4 && rows.front().line == 0 && rows.front().label == "1");
        });
    }
    MCGuiStep("done", [] { ed()->closeAllTabs(); });
}
