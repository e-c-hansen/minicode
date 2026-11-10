// FoldingSuite.cpp — `./build/minicode-guitest folding`: code folding in the
// editor (linux/PARITY.md item 1.10, src/Folds.h), the counterpart of the
// Mac's tests/gui/FoldingSuite.mm, with real keys and clicks through XTest:
// Ctrl+Shift+[ folding the innermost region and then the next one out, the
// text and undo untouched, the gutter numbering only the lines on show and
// walking none of the hidden ones, typing beside a fold moving it, the
// arrows stepping over it, a copy across it taking what it hides, Go to
// Line and Find opening it, Fold All (Ctrl+Alt+0), Unfold All (Ctrl+Alt+J)
// and Fold Level 2 (Ctrl+Alt+2), a chevron's click and a placeholder's,
// Backspace after and Delete before it opening it and deleting nothing,
// Return at the end of a folded line making a line below the fold (and its
// undo leaving it closed), an edit inside opening it, the folds staying
// with their tab and with the source while its Markdown page shows, a
// reload from disk forgetting them, and none in large-file mode.
#include "Harness.h"

#include "BufferLines.h"
#include "CodeView.h"
#include "Document.h"
#include "Editor.h"
#include "FindBar.h"
#include "Folds.h"
#include "Gutter.h"
#include "PalettePanel.h"
#include "Zoom.h"

#ifdef MINICODE_ENABLE_TERMINAL
#include "Terminal.h"
#include <vte/vte.h>
#endif

#include <string>
#include <unistd.h>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
GtkTextView* tv() { return GTK_TEXT_VIEW(ed()->textView()); }
GtkTextBuffer* buf() { return ed()->buffer(); }
DocumentFolds* folds() { return DocumentFolds::of(buf()); }
Gutter* gut() { return ed()->codeView()->gutter(); }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

const std::string kPy =
    "import os\n"              // 1
    "\n"                       // 2
    "def f(x):\n"              // 3
    "    if x:\n"              // 4
    "        return 1\n"       // 5
    "    return 2\n"           // 6
    "\n"                       // 7
    "class A:\n"               // 8
    "    def g(self):\n"       // 9
    "        target = 1\n"     // 10
    "        return target\n"  // 11
    "\n"                       // 12
    "x = 3\n";                 // 13
const std::string kC =
    "int main() {\n    if (a) {\n        b();\n    } else {\n        c();\n    }\n    return 0;\n}\n";
const std::string kMd = "# One\ntext\n\n## Sub\nmore\n\n# Two\nend\n";

// Where a 1-based line starts, in characters (the files are ASCII).
int L(size_t line) { return (int)BufferLines::of(buf())->lineStart(line - 1); }

void caretAt(int at) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, at);
    gtk_text_buffer_select_range(buf(), &it, &it);
}

int caret() {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(buf(), &it, gtk_text_buffer_get_insert(buf()));
    return gtk_text_iter_get_offset(&it);
}

std::string selected() {
    GtkTextIter a, b;
    gtk_text_buffer_get_selection_bounds(buf(), &a, &b);
    char* t = gtk_text_buffer_get_text(buf(), &a, &b, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

GtkTextTag* foldTag() {
    return gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buf()), "minicode-fold");
}

// The 1-based lines whose start the fold tag does not hide.
std::string shownByTag() {
    BufferLines* l = BufferLines::of(buf());
    GtkTextTag* t = foldTag();
    std::string s;
    for (size_t i = 0; l && i < l->lineCount(); i++) {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)l->lineStart(i));
        if (t && gtk_text_iter_has_tag(&it, t)) continue;
        s += (s.empty() ? "" : " ") + std::to_string(i + 1);
    }
    return s;
}

// The 1-based lines GTK gives a height: what is on show.
std::string shownByHeight() {
    BufferLines* l = BufferLines::of(buf());
    std::string s;
    for (size_t i = 0; l && i < l->lineCount(); i++) {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)l->lineStart(i));
        int y = 0, h = 0;
        gtk_text_view_get_line_yrange(tv(), &it, &y, &h);
        if (h <= 0) continue;
        s += (s.empty() ? "" : " ") + std::to_string(i + 1);
    }
    return s;
}

bool shows(const std::string& want) { return shownByTag() == want && shownByHeight() == want; }

std::string gutterLabels() {
    std::string s;
    for (const Gutter::Row& r : gut()->drawnRows()) s += (s.empty() ? "" : " ") + r.label;
    return s;
}

const Gutter::Row* gutterRow(size_t line0) {
    for (const Gutter::Row& r : gut()->drawnRows())
        if (r.line == line0) return &r;
    return nullptr;
}

std::string statusText() { return gtk_label_get_text(GTK_LABEL(MCGuiApp().statusLabel)); }

bool editorHasKeyboard() { return gtk_widget_has_focus(ed()->textView()); }

std::string gClip;

bool readClipboard() {
    GdkContentProvider* p = gdk_clipboard_get_content(gtk_widget_get_clipboard(ed()->textView()));
    if (!p) return false;
    GValue v = G_VALUE_INIT;
    g_value_init(&v, G_TYPE_STRING);
    if (gdk_content_provider_get_value(p, &v, nullptr) && g_value_get_string(&v))
        gClip = g_value_get_string(&v);
    g_value_unset(&v);
    return !gClip.empty();
}

// A point in the view's own coordinates for a point of the buffer.
void bufferToWidget(int bx, int by, int* wx, int* wy) {
    gtk_text_view_buffer_to_window_coords(tv(), GTK_TEXT_WINDOW_WIDGET, bx, by, wx, wy);
}

#ifdef MINICODE_ENABLE_TERMINAL
VteTerminal* findVte(GtkWidget* root) {
    if (!root) return nullptr;
    if (VTE_IS_TERMINAL(root)) return VTE_TERMINAL(root);
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (VteTerminal* hit = findVte(c)) return hit;
    return nullptr;
}

bool terminalHasKeyboard() {
    return MCGuiApp().terminal->owns(gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)));
}

std::string terminalText() {
    VteTerminal* vte = findVte(MCGuiApp().terminal->widget());
    if (!vte) return "";
    char* text = vte_terminal_get_text_format(vte, VTE_FORMAT_TEXT);
    std::string out = text ? text : "";
    g_free(text);
    return out;
}

std::vector<std::string> accels(const char* action) {
    std::vector<std::string> out;
    gchar** list = gtk_application_get_accels_for_action(MCGuiApplication(), action);
    for (gchar** a = list; a && *a; a++) out.push_back(*a);
    g_strfreev(list);
    return out;
}

// An accelerator as GTK writes it back ("<Control>braceleft" for
// "<Ctrl>braceleft"), so the two spellings compare.
std::string canon(const std::string& accel) {
    guint key = 0;
    GdkModifierType mods = (GdkModifierType)0;
    if (!gtk_accelerator_parse(accel.c_str(), &key, &mods)) return "(unparsable " + accel + ")";
    gchar* name = gtk_accelerator_name(key, mods);
    std::string out = name;
    g_free(name);
    return out;
}

bool gTerminalWasOpen = false;
#endif

}  // namespace

MC_GUI_SUITE(folding) {
    MCGuiStep("the files; fold.py in a tab", [] {
        ed()->closeAllTabs();
        MCGuiWriteFile(F("fold.py"), kPy);
        MCGuiWriteFile(F("fold.c"), kC);
        MCGuiWriteFile(F("fold.md"), kMd);
        MCGuiOpenFileKept(F("fold.py"));
    });
    MCGuiWait("its regions", 10, [] {
        return ed()->currentPath() == F("fold.py") && folds() && folds()->startsRegion(2) &&
               folds()->regionsCurrent();
    });
    MCGuiStep("the keyboard in the text, the window focused", [] {
        MCGuiSetKeyWindow(nullptr);   // an earlier suite may have left another one focused
        MCGuiFocusKeyWindow();
        MCGuiClick(ed()->textView());
    });
    MCGuiWait("the editor has it", 5, editorHasKeyboard);

    // ---- by key
    MCGuiStep("Ctrl+Shift+[ in \"return 1\"", [] {
        GCHECK(!folds()->hasClosedFolds());
        caretAt(L(5) + 10);
        MCGuiKey("ctrl+shift+[");
    });
    MCGuiWait("the innermost, the if on line 4, hides line 5", 5, [] {
        return folds()->startsClosedFold(3) && shows("1 2 3 4 6 7 8 9 10 11 12 13 14");
    });
    MCGuiStep("the caret at the end of the fold's first line, the text untouched; again", [] {
        GCHECK(caret() == L(5) - 1);
        GCHECK_EQ_STR(MCGuiEditorText(), kPy);
        GCHECK(!ed()->dirty());
        MCGuiKey("ctrl+shift+[");
    });
    MCGuiWait("the next one out, the def", 5, [] {
        return folds()->startsClosedFold(2) && shows("1 2 3 7 8 9 10 11 12 13 14");
    });
    MCGuiStep("nothing to undo: folding is not an edit", [] {
        GCHECK(caret() == L(4) - 1);
        GCHECK(!gtk_text_buffer_get_can_undo(buf()));
        GCHECK_EQ_STR(MCGuiEditorText(), kPy);
    });
    MCGuiWaitQuiet("the gutter drawn", 0.3, 5, [] { return (long)gut()->drawCount(); });
    MCGuiStep("the gutter numbers only the lines on show, each on its line", [] {
        GCHECK_EQ_STR(gutterLabels(), "1 2 3 7 8 9 10 11 12 13 14");
        const Gutter::Row* r = gutterRow(2);
        GCHECK(r && r->chevron == 2);   // closed, so its chevron shows without the pointer
        for (const Gutter::Row& row : gut()->drawnRows()) {
            GtkTextIter at;
            gtk_text_buffer_get_iter_at_offset(buf(), &at, L(row.line + 1));
            int y = 0, h = 0;
            gtk_text_view_get_line_yrange(tv(), &at, &y, &h);
            int wx = 0, wy = 0;
            gtk_text_view_buffer_to_window_coords(tv(), GTK_TEXT_WINDOW_LEFT, 0, y, &wx, &wy);
            GCHECK(h > 0 && row.y == wy);
        }
        // It visited the lines on show and jumped the hidden ones.
        GCHECK(gut()->linesWalked() <= 12);
        MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "fold-def");
    });

    // ---- the arrows
    MCGuiStep("Right from the end of the folded line", [] {
        caretAt(L(4) - 1);
        MCGuiKey("right");
    });
    MCGuiWait("the start of the line after the fold", 5, [] { return caret() == L(7); });
    MCGuiStep("Left back", [] { MCGuiKey("left"); });
    MCGuiWait("the end of the folded line", 5, [] { return caret() == L(4) - 1; });
    MCGuiStep("Down from the folded line", [] {
        caretAt(L(3));
        MCGuiKey("down");
    });
    MCGuiWait("the line after the fold", 5, [] { return caret() == L(7); });
    MCGuiStep("Up back", [] { MCGuiKey("up"); });
    MCGuiWait("the folded line", 5, [] { return caret() == L(3); });
    MCGuiStep("nothing opened on the way", [] {
        GCHECK(folds()->startsClosedFold(2) && folds()->startsClosedFold(3));
    });

    // ---- typing beside it
    MCGuiStep("a line typed above the fold", [] {
        caretAt(0);
        MCGuiType("# top\n");
    });
    MCGuiWait("the fold moved down with its text", 5, [] {
        return MCGuiEditorText() == "# top\n" + kPy && folds()->startsClosedFold(3) &&
               shows("1 2 3 4 8 9 10 11 12 13 14 15");
    });
    MCGuiStep("typing at the end of the folded line", [] {
        caretAt(L(5) - 1);
        MCGuiType("  # c");
    });
    MCGuiWait("is typed there, and the fold stays", 5, [] {
        return MCGuiEditorText().find("def f(x):  # c\n    if x:") != std::string::npos &&
               folds()->startsClosedFold(3) && shows("1 2 3 4 8 9 10 11 12 13 14 15");
    });
    MCGuiStep("undone", [] {
        for (int i = 0; i < 12; i++) MCGuiKey("ctrl+z");
    });
    MCGuiWait("back to the file, folded as before", 5, [] {
        return MCGuiEditorText() == kPy && folds()->startsClosedFold(2) &&
               shows("1 2 3 7 8 9 10 11 12 13 14");
    });

    // ---- copying across it
    MCGuiStep("Shift+Down twice from the folded line's start, then Ctrl+C", [] {
        caretAt(L(3));
        MCGuiKey("shift+down");
        MCGuiKey("shift+down");
    });
    MCGuiWait("selected over the fold, which stays", 5, [] {
        return selected() == kPy.substr((size_t)L(3), (size_t)(L(8) - L(3)));
    });
    MCGuiStep("copied", [] {
        GCHECK(folds()->startsClosedFold(2));
        gClip.clear();
        MCGuiKey("ctrl+c");
    });
    MCGuiWait("something on the clipboard", 5, readClipboard);
    MCGuiStep("what the fold hides is copied too", [] {
        GCHECK_EQ_STR(gClip, "def f(x):\n    if x:\n        return 1\n    return 2\n\n");
    });

    // ---- Go to Line
    MCGuiStep("Ctrl+G \"5:5\", into the folds", [] {
        caretAt(L(1));
        MCGuiKey("ctrl+g");
    });
    MCGuiWait("the palette", 5, [] { return MCGuiApp().palette->isOpen(); });
    MCGuiStep("typed, Return", [] {
        MCGuiType("5:5");
        MCGuiKey("enter");
    });
    MCGuiWait("both folds open, the caret on line 5, column 5", 5, [] {
        return !folds()->hasClosedFolds() && caret() == L(5) + 4 &&
               shows("1 2 3 4 5 6 7 8 9 10 11 12 13 14");
    });

    // ---- Fold All, Find, Unfold All
    MCGuiStep("Ctrl+Alt+0", [] {
        gtk_widget_grab_focus(ed()->textView());
        MCGuiKey("ctrl+alt+0");
    });
    MCGuiWait("every region folded", 5, [] { return shows("1 2 3 7 8 12 13 14"); });
    MCGuiStep("Find \"target = 1\", inside two folds", [] {
        GCHECK(statusText().find("stayed open") == std::string::npos);   // all of it fitted
        MCGuiKey("ctrl+f");
    });
    MCGuiWait("the find bar, the keyboard in its query", 5, [] {
        FindBar* b = MCGuiApp().findBar;
        return b->shown() && gtk_widget_get_mapped(b->widget()) && b->editing() &&
               gtk_widget_has_focus(b->findField()) && b->resultsCurrent();
    });
    MCGuiStep("typed", [] {
        MCGuiKey("ctrl+a");
        MCGuiType("target = 1");
    });
    MCGuiWait("the match is selected, and what hid it opened", 5, [] {
        return selected() == "target = 1" && shows("1 2 3 7 8 9 10 11 12 13 14") &&
               folds()->startsClosedFold(2) && !folds()->startsClosedFold(7);
    });
    MCGuiStep("Escape; Ctrl+Alt+J", [] {
        MCGuiKey("escape");
        MCGuiKey("ctrl+alt+j");
    });
    MCGuiWait("nothing folded", 5, [] {
        return !folds()->hasClosedFolds() && shows("1 2 3 4 5 6 7 8 9 10 11 12 13 14") &&
               editorHasKeyboard();
    });

    // ---- Fold Level 2
    MCGuiStep("Ctrl+Alt+2 with the caret in def g", [] {
        caretAt(L(10));
        MCGuiKey("ctrl+alt+2");
    });
    MCGuiWait("the if folded, def g (the caret's) left open", 5, [] {
        return folds()->startsClosedFold(3) && !folds()->startsClosedFold(8) &&
               shows("1 2 3 4 6 7 8 9 10 11 12 13 14");
    });
    MCGuiStep("unfolded", [] { MCGuiKey("ctrl+alt+j"); });
    MCGuiWait("nothing folded", 5, [] { return !folds()->hasClosedFolds(); });

    // ---- the chevrons
    MCGuiStep("the pointer over the gutter", [] { MCGuiMoveTo(gut()->widget()); });
    MCGuiWait("open regions' chevrons drawn", 5, [] {
        const Gutter::Row* r = gutterRow(7);
        return r && r->chevron == 1;
    });
    MCGuiStep("a click on class A's chevron", [] {
        MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "fold-chevrons");
        const Gutter::Row* r = gutterRow(7);
        const double x = gtk_widget_get_width(gut()->widget()) - Gutter::kFoldLane / 2 - 1;
        MCGuiClick(gut()->widget(), x, r->y + r->height / 2);
    });
    MCGuiWait("class A folded", 5, [] {
        return folds()->startsClosedFold(7) && shows("1 2 3 4 5 6 7 8 12 13 14");
    });
    MCGuiStep("a click on line 3's number", [] {
        const Gutter::Row* r = gutterRow(2);
        MCGuiClick(gut()->widget(), Gutter::kDotLane + 4, r->y + r->height / 2);
    });
    MCGuiWait("selects the line, the fold staying", 5, [] {
        return selected() == "def f(x):\n" && folds()->startsClosedFold(7);
    });
    MCGuiPause(0.5);   // GTK's double-click time: the next click is a click of its own
    MCGuiStep("the chevron again", [] {
        const Gutter::Row* r = gutterRow(7);
        const double x = gtk_widget_get_width(gut()->widget()) - Gutter::kFoldLane / 2 - 1;
        MCGuiClick(gut()->widget(), x, r->y + r->height / 2);
    });
    MCGuiWait("unfolded", 5, [] { return !folds()->hasClosedFolds() && shows("1 2 3 4 5 6 7 8 9 10 11 12 13 14"); });

    // ---- the placeholder
    MCGuiStep("class A folded by its line", [] { GCHECK(folds()->toggleLine(7)); });
    MCGuiWait("its placeholder drawn after \"class A:\"", 5, [] {
        const auto& ph = ed()->folds()->drawnPlaceholders();
        return ph.size() == 1 && ph[0].start == (size_t)(L(9) - 1) && shows("1 2 3 4 5 6 7 8 12 13 14");
    });
    MCGuiPause(0.5);   // the double-click time again
    MCGuiStep("a click on it", [] {
        MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "fold-placeholder");
        const GdkRectangle r = ed()->folds()->drawnPlaceholders()[0].rect;
        GtkTextIter end;
        gtk_text_buffer_get_iter_at_offset(buf(), &end, L(9) - 1);
        GdkRectangle loc;
        gtk_text_view_get_iter_location(tv(), &end, &loc);
        GCHECK(r.x >= loc.x && r.width > 10);   // after the line's text
        int wx = 0, wy = 0;
        bufferToWidget(r.x + r.width / 2, r.y + r.height / 2, &wx, &wy);
        MCGuiClick(ed()->textView(), wx, wy);
    });
    MCGuiWait("the fold opens, the caret where it was", 5, [] {
        return !folds()->hasClosedFolds() && caret() == L(9) - 1 &&
               shows("1 2 3 4 5 6 7 8 9 10 11 12 13 14");
    });

    // ---- Backspace, Delete and Return beside it
    MCGuiStep("Backspace at the start of the line after the fold", [] {
        GCHECK(folds()->toggleLine(7));
        caretAt(L(12));
        MCGuiKey("backspace");
    });
    MCGuiWait("opens it and deletes nothing", 5, [] {
        return !folds()->hasClosedFolds() && MCGuiEditorText() == kPy && caret() == L(12);
    });
    MCGuiStep("Delete at the end of the folded line", [] {
        GCHECK(folds()->toggleLine(7));
        caretAt(L(9) - 1);
        MCGuiKey("delete");
    });
    MCGuiWait("opens it and deletes nothing", 5, [] {
        return !folds()->hasClosedFolds() && MCGuiEditorText() == kPy && caret() == L(9) - 1;
    });
    MCGuiStep("Return at the end of the folded line", [] {
        GCHECK(folds()->toggleLine(7));
        caretAt(L(9) - 1);
        MCGuiKey("enter");
    });
    MCGuiWait("a new line below the fold, which stays closed", 5, [] {
        // After "return target", class A's last line, with the indentation
        // AutoIndent gives there; the rest of the file after it.
        const std::string t = MCGuiEditorText();
        const size_t a = kPy.find("        return target") + std::string("        return target").size();
        if (t.size() <= kPy.size() || t.compare(0, a, kPy, 0, a) != 0 || t[a] != '\n') return false;
        const size_t rest = kPy.size() - a;
        if (t.compare(t.size() - rest, rest, kPy, a, rest) != 0) return false;
        const std::string added = t.substr(a + 1, t.size() - rest - a - 1);
        return added.find_first_not_of(' ') == std::string::npos && folds()->startsClosedFold(7) &&
               shows("1 2 3 4 5 6 7 8 12 13 14 15");
    });
    MCGuiStep("the caret on the new line, on show; undone", [] {
        GCHECK(BufferLines::of(buf())->lineOf((size_t)caret()) == 11);
        GCHECK(caret() == L(13) - 1);   // after the indentation
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("undone, the fold still closed", 5, [] {
        return MCGuiEditorText() == kPy && folds()->startsClosedFold(7) &&
               shows("1 2 3 4 5 6 7 8 12 13 14");
    });

    // ---- an edit inside
    MCGuiStep("\"target\" replaced from elsewhere, inside the fold", [] {
        const int at = (int)kPy.find("target = 1");
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buf(), &a, at);
        gtk_text_buffer_get_iter_at_offset(buf(), &b, at + 6);
        gtk_text_buffer_begin_user_action(buf());
        gtk_text_buffer_delete(buf(), &a, &b);
        gtk_text_buffer_insert(buf(), &a, "aim", 3);
        gtk_text_buffer_end_user_action(buf());
    });
    MCGuiWait("opens it", 5, [] {
        return !folds()->hasClosedFolds() && shows("1 2 3 4 5 6 7 8 9 10 11 12 13 14");
    });
    MCGuiStep("undone", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("the file again", 5, [] { return MCGuiEditorText() == kPy; });

    // ---- a command that reads the whole text, with a fold above
    MCGuiStep("def f folded; Ctrl+/ on \"x = 3\"", [] {
        GCHECK(folds()->toggleLine(2));
        caretAt(L(13));
        MCGuiKey("ctrl+/");
    });
    MCGuiWait("commented where the caret is, the hidden text counted", 5, [] {
        const size_t x3 = kPy.find("x = 3\n");
        return MCGuiEditorText() == kPy.substr(0, x3) + "# x = 3\n" && folds()->startsClosedFold(2);
    });
    MCGuiStep("undone; unfolded", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("the file again", 5, [] { return MCGuiEditorText() == kPy; });
    MCGuiStep("", [] { MCGuiKey("ctrl+alt+j"); });
    MCGuiWait("nothing folded", 5, [] { return !folds()->hasClosedFolds(); });

    // ---- tabs
    MCGuiStep("def f and class A folded; fold.c opened", [] {
        GCHECK(folds()->toggleLine(2) && folds()->toggleLine(7));
        MCGuiOpenFileKept(F("fold.c"));
    });
    MCGuiWait("fold.c, its regions, and no folds of fold.py's", 10, [] {
        return ed()->currentPath() == F("fold.c") && folds() && folds()->startsRegion(0) &&
               !folds()->hasClosedFolds();
    });
    MCGuiStep("} else { closes one region and opens the next; the first folded", [] {
        GCHECK(folds()->startsRegion(1) && folds()->startsRegion(3));
        gtk_widget_grab_focus(ed()->textView());
        caretAt(L(3));
        MCGuiKey("ctrl+shift+[");
    });
    MCGuiWait("folded", 5, [] { return folds()->startsClosedFold(1) && shows("1 2 4 5 6 7 8 9"); });
    MCGuiStep("back to fold.py", [] { MCGuiKey("ctrl+pageup"); });
    MCGuiWait("its folds as they were", 5, [] {
        return ed()->currentPath() == F("fold.py") && folds()->startsClosedFold(2) &&
               folds()->startsClosedFold(7) && shows("1 2 3 7 8 12 13 14");
    });
    MCGuiStep("and to fold.c", [] { MCGuiKey("ctrl+pagedown"); });
    MCGuiWait("its fold too", 5, [] {
        return ed()->currentPath() == F("fold.c") && folds()->startsClosedFold(1) &&
               shows("1 2 4 5 6 7 8 9");
    });

    // ---- Markdown
    MCGuiStep("fold.md, from its page to the source", [] { MCGuiOpenFileKept(F("fold.md")); });
    MCGuiWait("open", 10, [] { return ed()->currentPath() == F("fold.md"); });
    MCGuiStep("the source", [] {
        if (ed()->inPreview()) ed()->togglePreview();
    });
    MCGuiWait("its regions", 10, [] {
        return !ed()->inPreview() && folds() && folds()->startsRegion(0) && folds()->regionsCurrent();
    });
    MCGuiStep("headings fold to the next of their level", [] {
        GCHECK(folds()->startsRegion(3) && folds()->startsRegion(6));
        GCHECK(folds()->toggleLine(0));
    });
    MCGuiWait("# One folded", 5, [] { return shows("1 6 7 8 9"); });
    MCGuiStep("the page and back: the source keeps its fold (a page of its own, D3)", [] {
        ed()->togglePreview();
        GCHECK(ed()->inPreview());
        GCHECK(ed()->folds()->folds() == nullptr);   // nothing to fold on a page
        ed()->togglePreview();
    });
    MCGuiWait("folded still", 5, [] {
        return !ed()->inPreview() && folds() && folds()->startsClosedFold(0) && shows("1 6 7 8 9");
    });

    // ---- a reload from disk
    MCGuiStep("fold.py, clean and folded, changed on disk", [] {
        MCGuiOpenFileKept(F("fold.py"));
    });
    MCGuiWait("on show, folded", 5, [] {
        return ed()->currentPath() == F("fold.py") && folds()->startsClosedFold(2) && !ed()->dirty();
    });
    MCGuiStep("the file rewritten", [] { MCGuiWriteFile(F("fold.py"), kPy + "y = 4\n"); });
    MCGuiWait("the tab follows, its folds forgotten", 10, [] {
        return MCGuiEditorText() == kPy + "y = 4\n" && !folds()->hasClosedFolds() &&
               shows("1 2 3 4 5 6 7 8 9 10 11 12 13 14 15");
    });

#ifdef MINICODE_ENABLE_TERMINAL
    // ---- the terminal keeps the keys (editor commands, D1)
    MCGuiStep("the caret in def f's body; the terminal, with the keyboard", [] {
        caretAt(L(5) + 9);
        gTerminalWasOpen = gtk_widget_get_mapped(MCGuiApp().terminal->widget());
        if (gTerminalWasOpen) MCGuiClick(GTK_WIDGET(findVte(MCGuiApp().terminal->widget())));
        else MCGuiKey("ctrl+`");
    });
    MCGuiWait("it has it", 10, terminalHasKeyboard);
    MCGuiWait("the shell is up", 20, [] {
        return terminalText().find_first_not_of(" \n") != std::string::npos;
    });
    MCGuiStep("no folding key is an accelerator there; a program reads what comes", [] {
        for (const char* a : {"win.fold", "win.unfold", "win.foldrecursively", "win.unfoldrecursively",
                              "win.foldall", "win.unfoldall", "win.foldcomments", "win.foldlevel1",
                              "win.foldlevel7"})
            GCHECK(accels(a).empty());
        // Ctrl+Alt+0 is the terminals' zoom reset there (PARITY.md D1).
        unlink(F("fold-ready").c_str());
        unlink(F("fold-keys.bin").c_str());
        MCGuiType("cd '" + MCGuiProject() + "'; stty raw -echo; : > fold-ready; "
                  "IFS= read -r -d q k; stty sane; printf '%s' \"$k\" > fold-keys.bin\n");
    });
    MCGuiWait("it is reading", 20, [] { return MCGuiFileExists(F("fold-ready")); });
    MCGuiStep("Ctrl+Shift+[ and Ctrl+Alt+1, then q", [] {
        MCGuiKey("ctrl+shift+[");
        MCGuiKey("ctrl+alt+1");
        MCGuiKey("q");
    });
    MCGuiWait("the program got them", 20, [] { return MCGuiFileExists(F("fold-keys.bin")); });
    MCGuiStep("as bytes, and nothing folded; the terminal put away", [] {
        const std::string got = MCGuiReadFile(F("fold-keys.bin"));
        for (unsigned char c : got) MCGuiNote("the program read %02x", c);
        GCHECK(got.size() >= 2);
        GCHECK(!folds()->hasClosedFolds());
        if (!gTerminalWasOpen) MCGuiKey("ctrl+`");
        else MCGuiClick(ed()->textView());
    });
    MCGuiWait("the terminal put away", 10, [] { return !terminalHasKeyboard(); });
    MCGuiStep("in the editor the keys are back, Ctrl+Shift+[ as the brace Shift makes", [] {
        gtk_widget_grab_focus(ed()->textView());
    });
    MCGuiWait("the accelerators follow the keyboard", 5, [] {
        const std::vector<std::string> fold = accels("win.fold"), all = accels("win.foldall");
        bool resetOnAlt0 = false;
        for (const std::string& a : accels("win.editorzoomreset"))
            resetOnAlt0 |= canon(a) == canon("<Ctrl><Alt>0");
        return fold.size() == 1 && canon(fold[0]) == canon("<Ctrl>braceleft") && all.size() == 1 &&
               canon(all[0]) == canon("<Ctrl><Alt>0") && !resetOnAlt0 &&
               accels("win.zoomfit").empty() && accels("win.terminalzoomreset").empty();
    });
#endif

    // ---- Ctrl+Alt+0 in the editor is Fold All's, not the zoom's (D1)
    MCGuiStep("zoomed in a point; Ctrl+Alt+0", [] {
        gtk_widget_grab_focus(ed()->textView());
        GCHECK(Zoom::editorZoom() == 0);
        MCGuiKey("ctrl+=");
        MCGuiKey("ctrl+alt+0");
    });
    MCGuiWait("everything folded, the zoom left as it was", 5, [] {
        return shows("1 2 3 7 8 12 13 14 15") && Zoom::editorZoom() == 1;
    });
    MCGuiStep("Ctrl+keypad 0 puts the zoom back; Ctrl+Alt+J unfolds", [] {
        // The placeholders take the zoomed line's height and the zoomed
        // font's "000".
        const auto& ph = ed()->folds()->drawnPlaceholders();
        GCHECK(!ph.empty());
        MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "fold-zoomed");
        MCGuiKey("ctrl+KP_Insert");
        MCGuiKey("ctrl+alt+j");
    });
    MCGuiWait("no zoom, nothing folded", 5, [] {
        return Zoom::editorZoom() == 0 && !folds()->hasClosedFolds();
    });

    // ---- a long line inside a fold: its own hidden part stays its own
    MCGuiStep("a function holding a line of 30,000 characters", [] {
        MCGuiWriteFile(F("fold-long.py"), "def f():\n    s = '" + std::string(30000, 'q') + "'\n    return s\n\nx = 1\n");
        MCGuiOpenFileKept(F("fold-long.py"));
    });
    MCGuiWait("its region, the long line shown in part", 10, [] {
        GtkTextTag* rest = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buf()),
                                                     "minicode-long-line-rest");
        GtkTextIter far;
        gtk_text_buffer_get_iter_at_offset(buf(), &far, L(2) + 20000);
        return ed()->currentPath() == F("fold-long.py") && folds()->regionsCurrent() &&
               folds()->startsRegion(0) && rest && gtk_text_iter_has_tag(&far, rest);
    });
    MCGuiStep("folded and unfolded", [] {
        GCHECK(folds()->toggleLine(0));
        GCHECK(shownByTag() == "1 4 5 6");
        GCHECK(folds()->toggleLine(0));
    });
    MCGuiWait("all on show again, the long line still in part", 5, [] {
        GtkTextTag* rest = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buf()),
                                                     "minicode-long-line-rest");
        GtkTextIter near, far;
        gtk_text_buffer_get_iter_at_offset(buf(), &near, L(2) + 100);
        gtk_text_buffer_get_iter_at_offset(buf(), &far, L(2) + 20000);
        return shows("1 2 3 4 5 6") && !gtk_text_iter_has_tag(&near, rest) &&
               gtk_text_iter_has_tag(&far, rest) && !gtk_text_iter_has_tag(&far, foldTag());
    });

    // ---- large-file mode
    MCGuiStep("a file of 300,001 lines", [] {
        std::string big;
        big.reserve(300001 * 7);
        for (int i = 0; i < 300001; i++) big += i % 3 ? "    x\n" : "def f:\n";
        MCGuiWriteFile(F("fold-large.py"), big);
        MCGuiOpenFileKept(F("fold-large.py"));
    });
    MCGuiWait("open in large-file mode", 30, [] {
        Document* d = ed()->active();
        return d && d->path() == F("fold-large.py") && !d->loading() && d->largeFileFeaturesOff();
    });
    MCGuiStep("Fold All does nothing there", [] {
        gtk_widget_grab_focus(ed()->textView());
        GCHECK(!folds()->applies());
        MCGuiKey("ctrl+alt+0");
    });
    MCGuiPause(0.5);   // a negative check: nothing may fold
    MCGuiStep("no regions, no folds", [] {
        GCHECK(!folds()->hasClosedFolds());
        GCHECK(folds()->regions().empty());
        ed()->closeAllTabs();
    });
}
