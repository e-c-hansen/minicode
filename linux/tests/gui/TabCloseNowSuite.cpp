// TabCloseNowSuite.cpp — `./build/mutible-guitest tab_close_now`: a tab
// opened and closed in the same turn of the main loop, before the view has
// drawn it. GtkTextLayout kept line displays of the closed buffer, and the
// next frame read its freed lines (signal 11, or "gtk_text_btree_line_number
// couldn't find line"), every time under MALLOC_PERTURB_ until CodeView
// emptied the cache before the view left a buffer (2026-11-10; the case
// came from writing item 1.7's suites). GutterSwitchSuite.cpp has the
// longer story.
//
// Then a tab scrolled down, its whole text replaced and its caret left at
// the end, closed in the same step (semantic_tokens' last step, which
// crashed main's GTK job twice after #165, linux/HANDOFF.md item 121).
// Scrolling the view to its top as it leaves the buffer asks GTK for the
// caret's place, which lays out the caret's line; a line the text had just
// gained has no height yet, so set_buffer kept its display, and the next
// frame sorted a display in beside it and read the freed line
// (_gtk_text_line_get_number under gtk_text_line_display_cache_get). With
// #165's order in CodeView::setViewBuffer (the scroll after the cache was
// emptied) it crashed five runs of five under MALLOC_PERTURB_, which
// `meson test` sets; CodeView now also empties the cache after set_buffer.
#include "Harness.h"

#include "Editor.h"

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
GtkAdjustment* vadj() { return gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(ed()->textView())); }
gint64 frames() {
    GdkFrameClock* c = gtk_widget_get_frame_clock(ed()->textView());
    return c ? gdk_frame_clock_get_frame_counter(c) : 0;
}
std::string longText() {
    std::string t;
    for (int i = 0; i < 2000; i++) t += "row " + std::to_string(i) + " of a file closed at once\n";
    return t;
}
gint64 gFrame = 0;

}  // namespace

MC_GUI_SUITE(tab_close_now) {
    for (int round = 0; round < 5; round++) {
        MCGuiStep("a file opened and every tab closed in one step", [] {
            MCGuiWriteFile(MCGuiProjectFile("close-now.txt"), "hello\nworld\n");
            MCGuiOpenFileKept(MCGuiProjectFile("close-now.txt"));
            MCGuiApp().editor->closeAllTabs();
        });
        MCGuiStep("the next frames draw", [] { GCHECK(MCGuiApp().editor->tabCount() == 0); });
    }

    for (int round = 0; round < 5; round++) {
        MCGuiStep("a long file on show", [] {
            MCGuiWriteFile(MCGuiProjectFile("close-replaced.txt"), longText());
            MCGuiOpenFileKept(MCGuiProjectFile("close-replaced.txt"));
        });
        MCGuiWait("laid out, and scrolled down a little", 10, [] {
            if (ed()->currentPath() != MCGuiProjectFile("close-replaced.txt")) return false;
            GtkAdjustment* v = vadj();
            if (gtk_adjustment_get_upper(v) - gtk_adjustment_get_page_size(v) > 1000 &&
                gtk_adjustment_get_value(v) < 300)
                gtk_adjustment_set_value(v, 400);
            return gtk_adjustment_get_value(v) >= 300;
        });
        MCGuiStep("its text replaced, the caret at its end, and every tab closed", [] {
            // The same text again (a reload from disk does this), so the tab
            // closes unmodified; its lines are new to GTK all the same.
            GCHECK(gtk_adjustment_get_value(vadj()) >= 300);   // the view leaves it scrolled
            GtkTextBuffer* b = ed()->buffer();
            const std::string t = longText();
            gtk_text_buffer_set_text(b, t.c_str(), (int)t.size());
            GtkTextIter end;
            gtk_text_buffer_get_end_iter(b, &end);
            gtk_text_buffer_place_cursor(b, &end);
            gFrame = frames();
            ed()->closeAllTabs();
        });
        MCGuiWait("the next frames draw", 10, [] {
            gtk_widget_queue_draw(ed()->textView());
            return ed()->tabCount() == 0 && frames() >= gFrame + 2;
        });
    }
}
