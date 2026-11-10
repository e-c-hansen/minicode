// CaretSuites.h — what the suites of several carets and the line commands
// share (linux/PARITY.md item 1.9): MultiCaretSuite, MultiCaretSelectSuite,
// LineCommandsSuite, EditorWordsSuite and MultiCaretPerfSuite. Where a
// character of the editor is on screen, to click it; the carets as text, to
// compare; the clipboard's text.
#pragma once

#include "Harness.h"

#include "Carets.h"
#include "Editor.h"

#include <string>
#include <vector>

namespace caretsuite {

inline Editor* ed() { return MCGuiApp().editor; }
inline GtkTextView* tv() { return GTK_TEXT_VIEW(ed()->textView()); }
inline GtkWidget* view() { return ed()->textView(); }
inline GtkTextBuffer* buf() { return ed()->buffer(); }
inline Carets* carets() { return ed()->carets(); }

inline std::string text() { return MCGuiEditorText(); }

// The point in the view's own coordinates just inside the left of character
// `offset`, at its line's middle: a click there puts the caret before it.
inline void pointAt(size_t offset, double* x, double* y) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)offset);
    GdkRectangle r;
    gtk_text_view_get_iter_location(tv(), &it, &r);
    int wx = 0, wy = 0;
    gtk_text_view_buffer_to_window_coords(tv(), GTK_TEXT_WINDOW_WIDGET, r.x, r.y + r.height / 2,
                                          &wx, &wy);
    *x = wx + 1;
    *y = wy;
}

// Its middle: a double-click there is on the character.
inline void middleOf(size_t offset, double* x, double* y) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)offset);
    GdkRectangle r;
    gtk_text_view_get_iter_location(tv(), &it, &r);
    int wx = 0, wy = 0;
    gtk_text_view_buffer_to_window_coords(tv(), GTK_TEXT_WINDOW_WIDGET, r.x + r.width / 2,
                                          r.y + r.height / 2, &wx, &wy);
    *x = wx;
    *y = wy;
}

inline void clickAt(size_t offset, const std::string& mods = "") {
    double x = 0, y = 0;
    pointAt(offset, &x, &y);
    if (mods.empty()) MCGuiClick(view(), x, y);
    else MCGuiClickWith(mods, view(), x, y);
}

// The carets as "anchor>head" or "at" (empty), in the text's order: every
// caret, or the buffer's selection as one.
inline std::string caretsText() {
    std::string out;
    for (const MultiCaret::Caret& c : carets()->current()) {
        if (!out.empty()) out += " ";
        out += c.empty() ? std::to_string(c.head)
                         : std::to_string(c.anchor) + ">" + std::to_string(c.head);
    }
    return out;
}

// Waits for the carets to be `want`, and says what they were if not.
inline void waitCarets(const std::string& what, const std::string& want) {
    MCGuiWait(what, 5, [want] { return caretsText() == want; });
    MCGuiStep(what + ": the carets", [want] { GCHECK_EQ_STR(caretsText(), want); });
}

inline size_t offsetOf(const std::string& needle, size_t from = 0) {
    const std::string t = text();
    const size_t byte = t.find(needle, from);
    if (byte == std::string::npos) return (size_t)-1;
    return (size_t)g_utf8_strlen(t.c_str(), (gssize)byte);
}

// The clipboard's text, read as an app reads it, into *out.
inline void readClipboard(std::string* out, MCGuiDone done) {
    struct Job {
        std::string* out;
        MCGuiDone done;
    };
    gdk_clipboard_read_text_async(
        gtk_widget_get_clipboard(view()), nullptr,
        [](GObject* src, GAsyncResult* res, gpointer p) {
            Job* j = static_cast<Job*>(p);
            char* t = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, nullptr);
            *j->out = t ? t : "";
            g_free(t);
            j->done();
            delete j;
        },
        new Job{out, std::move(done)});
}

// The window's focus is the view (gtk_widget_has_focus also wants the window
// to have X's focus, which the harness gives it before each key only).
inline bool editorHasKeyboard() {
    return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)) == view();
}

}  // namespace caretsuite
