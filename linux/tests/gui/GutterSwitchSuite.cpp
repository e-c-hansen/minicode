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
#include "Harness.h"

#include "CodeView.h"
#include "Document.h"
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
}
