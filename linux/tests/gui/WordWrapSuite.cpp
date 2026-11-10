// WordWrapSuite.cpp — word wrap in the editor (`./build/minicode-guitest
// word_wrap`, linux/PARITY.md item 1.11), the counterpart of the end of the
// Mac's zoomfit suite (tests/gui/ZoomFitSuite.mm), with real keys through
// XTest: a long line wraps at the pane's edge by default (editor.word-wrap
// on) with nothing to scroll sideways; Alt+Z turns that over for the
// session, in every window, without typing anything or touching the
// settings file; a Markdown page still wraps meanwhile; editor.word-wrap =
// off keeps it off, a setting changed since Alt+Z wins over it; and a line
// too long to lay out whole is still shown a part at a time (LongLines),
// wrapped or not.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "LargeFileView.h"
#include "Zoom.h"

#include <string>

namespace {

std::string F(const std::string& name) { return MCGuiProjectFile(name); }
Editor* ed() { return MCGuiApp().editor; }
GtkTextView* tv() { return GTK_TEXT_VIEW(ed()->textView()); }
bool editorHasKeyboard() { return gtk_widget_has_focus(ed()->textView()); }
bool wraps() { return gtk_text_view_get_wrap_mode(tv()) != GTK_WRAP_NONE; }

// The height a buffer line takes on screen (its display lines together).
int lineHeight(int line) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line(ed()->buffer(), &it, line);
    int y = 0, h = 0;
    gtk_text_view_get_line_yrange(tv(), &it, &y, &h);
    return h;
}

// The long line on several rows, and nothing to scroll sideways; or one row
// running past the pane's edge.
bool wrapped() {
    GtkAdjustment* h = gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(tv()));
    return wraps() && lineHeight(1) >= 3 * lineHeight(0) &&
           gtk_adjustment_get_upper(h) <= gtk_adjustment_get_page_size(h) + 1;
}
bool runsOn() {
    GtkAdjustment* h = gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(tv()));
    return !wraps() && lineHeight(0) > 0 && lineHeight(1) == lineHeight(0) &&
           gtk_adjustment_get_upper(h) > gtk_adjustment_get_page_size(h) + 100;
}

std::string longText() {
    std::string line;
    for (int i = 0; i < 60; i++) line += "word" + std::to_string(i) + " wraps ";
    return "a short line\n" + line + "\nthe last line\n";
}

std::string settingsText() { return MCGuiReadFile(g_getenv("MINICODE_SETTINGS")); }

std::string gSettings, gText;
size_t gWindows = 0;

}  // namespace

MC_GUI_SUITE(word_wrap) {
    MCGuiStep("a file with a long line, the editor with the keyboard", [] {
        MCGuiWriteFile(F("wrap.txt"), longText());
        MCGuiWriteFile(F("wrap.md"),
                       "# Wrapping\n\n" + std::string(60, 'w') + " " + longText() + "\n");
        // A line of 30,000 characters: shown 10,000 at a time (LargeFileView.h).
        std::string huge;
        for (int i = 0; i < 3000; i++) huge += "longword" + std::to_string(i % 10) + " ";
        MCGuiWriteFile(F("wrap-long.txt"), "first\n" + huge + "\nlast\n");
        gSettings = settingsText();
        GCHECK(Zoom::wordWrap());
        MCGuiOpenFileKept(F("wrap.txt"));
    });
    MCGuiWait("it shows", 10, [] { return ed()->currentPath() == F("wrap.txt"); });
    MCGuiStep("a click in it", [] { MCGuiClick(ed()->textView()); });
    MCGuiWait("the editor has the keyboard", 10, editorHasKeyboard);
    MCGuiWait("by default the long line wraps at the pane's edge", 10, wrapped);
    MCGuiStep("Alt+Z", [] {
        MCGuiNote("wrapped: the long line %d px high, a line %d px", lineHeight(1), lineHeight(0));
        gText = MCGuiEditorText();
        MCGuiKey("alt+z");
    });
    MCGuiWait("the long line runs on past the edge", 10, runsOn);
    MCGuiStep("nothing typed, the settings file as it was", [] {
        GCHECK_EQ_STR(MCGuiEditorText(), gText);
        GCHECK(!ed()->dirty());
        GCHECK_EQ_STR(settingsText(), gSettings);
        GCHECK(!Zoom::wordWrap() && editorHasKeyboard());
    });
    MCGuiStep("a Markdown page meanwhile", [] { MCGuiOpenFileKept(F("wrap.md")); });
    MCGuiWait("it still wraps, as a read-only page always does", 10, [] {
        return ed()->currentPath() == F("wrap.md") && ed()->inPreview() && wraps();
    });
    MCGuiStep("back to the text", [] { MCGuiOpenFileKept(F("wrap.txt")); });
    MCGuiWait("it runs on still", 10, [] { return ed()->currentPath() == F("wrap.txt") && runsOn(); });
    // For the session: a new window's editor has it too.
    MCGuiStep("a new window, the file in it", [] {
        gWindows = MCGuiAppWindows().size();
        MCGuiKey("ctrl+shift+n");
    });
    MCGuiWait("the window", 10, [] { return MCGuiAppWindows().size() == gWindows + 1; });
    MCGuiStep("the file in it", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
        MCGuiOpenFileKept(F("wrap.txt"));
    });
    MCGuiWait("unwrapped there too", 10, [] { return ed()->currentPath() == F("wrap.txt") && runsOn(); });
    MCGuiStep("Alt+Z from the new window", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("alt+z");
    });
    MCGuiWait("it wraps again", 10, wrapped);
    MCGuiStep("every window does: the new one closed", [] {
        const std::vector<MCGuiAppWindow> all = MCGuiAppWindows();
        GCHECK(all.size() == gWindows + 1);
        if (all.size() >= 2) {
            Editor* first = all[all.size() - 2].editor;
            GCHECK(gtk_text_view_get_wrap_mode(GTK_TEXT_VIEW(first->textView())) != GTK_WRAP_NONE);
        }
        MCGuiKey("ctrl+shift+w");
    });
    MCGuiWait("closed", 10, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("the first window has the keyboard", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
        MCGuiClick(ed()->textView());
    });
    MCGuiWait("it has", 10, [] { return gtk_window_is_active(MCGuiApp().window) && editorHasKeyboard(); });
    MCGuiWait("wrapping", 10, wrapped);

    // ---- the setting
    MCGuiStep("editor.word-wrap = off", [] {
        MCGuiSetSettings(gSettings + "\neditor.word-wrap = off\n");
    });
    MCGuiWait("off", 10, runsOn);
    MCGuiStep("Alt+Z over it", [] { MCGuiKey("alt+z"); });
    MCGuiWait("wrapping for the session", 10, wrapped);
    MCGuiStep("the setting back on", [] { MCGuiSetSettings(gSettings); });
    MCGuiWait("still wrapping", 10, wrapped);
    MCGuiStep("and off again: the setting changed since Alt+Z, so it wins", [] {
        GCHECK(Zoom::wordWrap());
        MCGuiSetSettings(gSettings + "\neditor.word-wrap = off\n");
    });
    MCGuiWait("off", 10, runsOn);
    MCGuiStep("the setting as it was", [] { MCGuiSetSettings(gSettings); });
    MCGuiWait("wrapping", 10, wrapped);

    // ---- a line too long to lay out whole: never wrapped (LongLines)
    MCGuiStep("a line of 30,000 characters", [] { MCGuiOpenFileKept(F("wrap-long.txt")); });
    MCGuiWait("shown a part at a time, on one row while the view wraps", 10, [] {
        if (ed()->currentPath() != F("wrap-long.txt")) return false;
        LongLines* lines = LongLines::of(ed()->buffer());
        LongLines::Part part;
        return lines && lines->partOf(1, &part) && part.length > part.end - part.start && wraps() &&
               lineHeight(0) > 0 && lineHeight(1) == lineHeight(0);
    });
    MCGuiStep("a click in it", [] { MCGuiClick(ed()->textView()); });
    MCGuiWait("the editor has the keyboard", 10, editorHasKeyboard);
    MCGuiStep("End from its start, with wrapping on", [] {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(ed()->buffer(), &it, 1);
        gtk_text_buffer_place_cursor(ed()->buffer(), &it);
        MCGuiKey("end");
    });
    MCGuiWait("the line's end, past the part, as before wrapping", 10, [] {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_mark(ed()->buffer(), &it, gtk_text_buffer_get_insert(ed()->buffer()));
        return gtk_text_iter_get_line(&it) == 1 && gtk_text_iter_ends_line(&it);
    });
    MCGuiStep("text typed at its start, and Home", [] {
        MCGuiKey("Home");
        MCGuiKey("Home");
    });
    MCGuiWait("at its start", 10, [] {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_mark(ed()->buffer(), &it, gtk_text_buffer_get_insert(ed()->buffer()));
        return gtk_text_iter_get_line(&it) == 1 && gtk_text_iter_starts_line(&it);
    });
    MCGuiStep("typed", [] { MCGuiType("abc"); });
    MCGuiWait("still one row", 10, [] {
        return MCGuiEditorText().find("\nabclongword") != std::string::npos && wraps() &&
               lineHeight(1) == lineHeight(0);
    });
    MCGuiStep("taken back", [] {
        for (int i = 0; i < 3; i++) MCGuiKey("BackSpace");
    });
    MCGuiWait("as it was", 10, [] { return !ed()->dirty() && lineHeight(1) == lineHeight(0); });
    MCGuiStep("most of the line selected, and Delete", [] {
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_line_offset(ed()->buffer(), &a, 1, 100);
        gtk_text_buffer_get_iter_at_line_offset(ed()->buffer(), &b, 1, 29000);
        gtk_text_buffer_select_range(ed()->buffer(), &a, &b);
        MCGuiKey("Delete");
    });
    MCGuiWait("no longer a long line: it wraps as any other", 10, [] {
        LongLines::Part part;
        return ed()->dirty() && !LongLines::of(ed()->buffer())->partOf(1, &part) &&
               lineHeight(1) > 3 * lineHeight(0);
    });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("long again, on one row", 10, [] {
        LongLines::Part part;
        return !ed()->dirty() && LongLines::of(ed()->buffer())->partOf(1, &part) &&
               lineHeight(1) == lineHeight(0);
    });

    // ---- large-file mode: wrapping is one of the features off
    MCGuiStep("a file of 300,001 lines", [] {
        std::string big;
        big.reserve(300001 * 3);
        for (int i = 0; i < 300001; i++) big += "x\n";
        MCGuiWriteFile(F("wrap-big.txt"), big);
        MCGuiOpenFileKept(F("wrap-big.txt"));
    });
    MCGuiWait("in large-file mode, not wrapped", 60, [] {
        return ed()->currentPath() == F("wrap-big.txt") && ed()->active() &&
               ed()->active()->largeFileFeaturesOff() && ed()->isSource() && !wraps();
    });
    MCGuiStep("back to the short file", [] { MCGuiOpenFileKept(F("wrap.txt")); });
    MCGuiWait("wrapped again", 10, wrapped);
}
