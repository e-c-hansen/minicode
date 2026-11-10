// SymbolsSuite.cpp — Go to Symbol in File, the palette's "@" (`./build/
// minicode-guitest symbols`, linux/PARITY.md item 1.5), the counterpart of
// the outline half of the Mac's tests/gui/SymbolsSuite.mm, by real keys:
// Ctrl+Shift+O lists the file's outline in its order with the caret's
// symbol chosen; Up and Down show each symbol in the editor while the field
// keeps the keyboard; Escape puts the selection and the scroll back; typing
// ranks and Return goes there with the keyboard in the editor; "@:" groups by
// kind with headings never chosen; a Markdown page lists its headings and
// scrolls to them, and Escape scrolls back. The Mac's language server half
// (and Go to Symbol in Workspace, and Go Back) waits for Stage 2 and item
// 1.11 here.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "PalettePanel.h"

#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
PalettePanel* pal() { return MCGuiApp().palette; }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }
GtkWidget* focusWidget() { return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)); }
bool editorHasKeyboard() { return gtk_widget_has_focus(ed()->textView()); }
bool fieldHasKeyboard() {
    GtkWidget* f = focusWidget();
    return f && (f == pal()->entry() || gtk_widget_is_ancestor(f, pal()->entry()));
}

std::string selectedLabel() {
    const std::vector<std::string> l = pal()->labels();
    const int i = pal()->selected();
    return i >= 0 && i < (int)l.size() ? l[(size_t)i] : "";
}
std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : "|") + x;
    return s;
}

const char* kShapes =
    "\"\"\"Shapes.\"\"\"\n"                        // 1
    "import math\n"                                // 2
    "\n"                                           // 3
    "\n"                                           // 4
    "class Shape:\n"                               // 5
    "    def area(self):\n"                        // 6
    "        return 0\n"                           // 7
    "\n"                                           // 8
    "    def describe(self):\n"                    // 9
    "        return f\"{self.area()}\"\n"          // 10
    "\n"                                           // 11
    "\n"                                           // 12
    "class Circle(Shape):\n"                       // 13
    "    def __init__(self, r):\n"                 // 14
    "        self.r = r\n"                         // 15
    "\n"                                           // 16
    "    def area(self):\n";                       // 17

std::string shapesText() {
    std::string s = kShapes;
    s += "        return math.pi * self.r ** 2\n";   // 18
    for (int i = 19; i <= 79; i++) s += "# filler " + std::to_string(i) + "\n";
    s += "def main():\n";                             // 80
    s += "    c = Circle(2)\n";
    s += "    return c.describe()\n";
    return s;
}

std::string notesText() {
    std::string md = "# Notes\n\nIntro.\n\n## First\n\n";
    for (int i = 0; i < 120; i++) md += "Paragraph " + std::to_string(i) + " of the first part.\n\n";
    md += "## Second\n\nThe end.\n";
    return md;
}

// Character offsets of the first `word` on 1-based `line`, as [start, end).
std::pair<int, int> wordOnLine(int line, const std::string& word) {
    GtkTextBuffer* b = ed()->sourceBuffer();
    GtkTextIter a, e;
    gtk_text_buffer_get_iter_at_line(b, &a, line - 1);
    e = a;
    if (!gtk_text_iter_ends_line(&e)) gtk_text_iter_forward_to_line_end(&e);
    char* raw = gtk_text_buffer_get_slice(b, &a, &e, TRUE);
    const std::string text = raw ? raw : "";
    g_free(raw);
    const size_t at = text.find(word);
    if (at == std::string::npos) return {-1, -1};
    const int start = gtk_text_iter_get_offset(&a) + (int)g_utf8_strlen(text.c_str(), (gssize)at);
    return {start, start + (int)g_utf8_strlen(word.c_str(), -1)};
}

std::pair<int, int> selection() {
    GtkTextBuffer* b = ed()->sourceBuffer();
    GtkTextIter a, e;
    gtk_text_buffer_get_selection_bounds(b, &a, &e);
    return {gtk_text_iter_get_offset(&a), gtk_text_iter_get_offset(&e)};
}

void select(std::pair<int, int> r) {
    GtkTextBuffer* b = ed()->sourceBuffer();
    GtkTextIter a, e;
    gtk_text_buffer_get_iter_at_offset(b, &a, r.first);
    gtk_text_buffer_get_iter_at_offset(b, &e, r.second);
    gtk_text_buffer_select_range(b, &a, &e);
}

bool onScreen(int offset) {
    GtkTextView* tv = GTK_TEXT_VIEW(ed()->textView());
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(ed()->buffer(), &it, offset);
    GdkRectangle vis, at;
    gtk_text_view_get_visible_rect(tv, &vis);
    gtk_text_view_get_iter_location(tv, &it, &at);
    return vis.height > 0 && at.y >= vis.y && at.y + at.height <= vis.y + vis.height;
}

double scrollY() {
    return gtk_adjustment_get_value(
        gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(ed()->textView())));
}

std::pair<int, int> gStart{-1, -1};
double gTop = 0;

const std::string kSeven = "Shape|area|describe|Circle|__init__|area|main";

}  // namespace

MC_GUI_SUITE(symbols) {
    MCGuiStep("files to look through, shapes.py open", [] {
        ed()->closeAllTabs();
        MCGuiWriteFile(F("sym/shapes.py"), shapesText());
        MCGuiWriteFile(F("sym/notes.md"), notesText());
        MCGuiWriteFile(F("sym/other.txt"), "one\ntwo\n");
        MCGuiOpenFileKept(F("sym/shapes.py"));
    });
    MCGuiWait("it shows", 10, [] {
        return ed()->currentPath() == F("sym/shapes.py") && MCGuiEditorText() == shapesText();
    });
    MCGuiStep("the keyboard in it, the caret in Circle.__init__", [] { MCGuiClick(ed()->textView()); });
    MCGuiWait("the editor has the keyboard", 10, editorHasKeyboard);
    // A tab just shown settles on its place for a moment; where it was is
    // read once it has.
    MCGuiWaitQuiet("the scroll settles", 0.5, 10, [] { return (long)scrollY(); });

    // ------------------------------------------------- the built-in outline
    MCGuiStep("Ctrl+Shift+O", [] {
        const auto w = wordOnLine(15, "self.r");
        gStart = {w.first, w.first};
        select(gStart);
        MCGuiKey("ctrl+shift+o");
    });
    MCGuiWait("the symbols arrive", 10, [] {
        return pal()->isOpen() && pal()->text() == "@" && pal()->labels().size() == 7;
    });
    MCGuiStep("in the file's order, the caret's method chosen, nothing moved yet", [] {
        GCHECK_EQ_STR(joined(pal()->labels()), kSeven);
        GCHECK_EQ_STR(selectedLabel(), "__init__");
        GCHECK_EQ_STR(pal()->keys()[4], "14");   // its line
        GCHECK_EQ_STR(pal()->symbolSource(), "outline");
        GCHECK(selection() == gStart);
        GCHECK(fieldHasKeyboard());
        gTop = scrollY();
        MCGuiSnapshot(pal()->box(), "symbols-outline");
        MCGuiKey("down");
    });
    MCGuiWait("Down shows Circle's area in the editor, the keyboard staying", 10, [] {
        return selectedLabel() == "area" && selection() == wordOnLine(17, "area");
    });
    MCGuiStep("and main, far down", [] {
        GCHECK(fieldHasKeyboard());
        MCGuiKey("down");
    });
    MCGuiWait("main selected, in view", 10, [] {
        const auto main = wordOnLine(80, "main");
        return selectedLabel() == "main" && selection() == main && onScreen(main.first);
    });
    MCGuiStep("Escape", [] {
        GCHECK(fieldHasKeyboard());
        GCHECK(scrollY() > gTop);
        MCGuiKey("escape");
    });
    MCGuiWait("the editor back as it was, with the keyboard", 10, [] {
        return !pal()->isOpen() && selection() == gStart && editorHasKeyboard();
    });
    MCGuiStep("the scroll too", [] { GCHECK(scrollY() == gTop); });

    MCGuiStep("typing ranks, Return goes there", [] { MCGuiKey("ctrl+shift+o"); });
    MCGuiWait("the symbols arrive", 10, [] { return pal()->labels().size() == 7; });
    MCGuiStep("\"mai\"", [] { MCGuiType("mai"); });
    MCGuiWait("main, shown as typed", 10, [] {
        return pal()->text() == "@mai" && selectedLabel() == "main" &&
               selection() == wordOnLine(80, "main");
    });
    MCGuiStep("Return", [] { MCGuiKey("enter"); });
    MCGuiWait("at main, in view, the keyboard in the editor", 10, [] {
        const auto main = wordOnLine(80, "main");
        return !pal()->isOpen() && selection() == main && onScreen(main.first) &&
               editorHasKeyboard();
    });

    MCGuiStep("\"@:\" groups by kind", [] {
        select(gStart);
        MCGuiKey("ctrl+shift+o");
    });
    MCGuiWait("open", 10, [] { return pal()->isOpen() && pal()->labels().size() == 7; });
    MCGuiStep("\":\"", [] { MCGuiType(":"); });
    MCGuiWait("grouped", 10, [] { return pal()->labels().size() == 10; });
    MCGuiStep("classes, methods, functions; the caret's chosen", [] {
        const std::vector<std::string> l = pal()->labels();
        GCHECK_EQ_STR(joined(l), "classes (2)|Shape|Circle|methods (4)|area|describe|__init__|area|"
                                 "functions (1)|main");
        GCHECK_EQ_STR(selectedLabel(), "__init__");
        MCGuiSnapshot(pal()->box(), "symbols-by-kind");
        MCGuiKey("up");   // describe
        MCGuiKey("up");   // Shape's area
        MCGuiKey("up");   // over "methods (4)" to Circle
    });
    MCGuiWait("headings never chosen", 10, [] { return selectedLabel() == "Circle"; });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("back where it was", 10, [] {
        return !pal()->isOpen() && selection() == gStart && editorHasKeyboard();
    });

    // ------------------------------------------------------ no outline
    MCGuiStep("a plain text file has none", [] { MCGuiOpenFileKept(F("sym/other.txt")); });
    MCGuiWait("it shows", 10, [] { return ed()->currentPath() == F("sym/other.txt"); });
    MCGuiStep("Ctrl+Shift+O", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+shift+o");
    });
    MCGuiWait("said so", 10, [] {
        return pal()->isOpen() && pal()->symbolSource() == "outline" && pal()->labels().empty() &&
               pal()->emptyText() == "No symbols found in this file";
    });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("closed", 10, [] { return !pal()->isOpen(); });

    // ------------------------------------------------- a Markdown page
    MCGuiStep("a Markdown page lists its headings", [] { MCGuiOpenFileKept(F("sym/notes.md")); });
    MCGuiWait("rendered", 10, [] {
        return ed()->currentPath() == F("sym/notes.md") && ed()->inPreview();
    });
    MCGuiStep("the keyboard in it", [] { MCGuiClick(ed()->textView()); });
    MCGuiWait("it has it", 10, editorHasKeyboard);
    MCGuiWaitQuiet("the scroll settles", 0.5, 10, [] { return (long)scrollY(); });
    MCGuiStep("Ctrl+Shift+O", [] {
        gTop = scrollY();
        MCGuiKey("ctrl+shift+o");
    });
    MCGuiWait("the headings", 10, [] { return pal()->isOpen() && pal()->labels().size() == 3; });
    MCGuiStep("Notes, First, Second; Down to Second", [] {
        GCHECK_EQ_STR(joined(pal()->labels()), "Notes|First|Second");
        MCGuiKey("down");
        MCGuiKey("down");
    });
    MCGuiWait("the page scrolls to it", 10, [] {
        return selectedLabel() == "Second" && scrollY() > gTop + 500;
    });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("scrolled back, the keyboard in the page", 10, [] {
        return !pal()->isOpen() && scrollY() == gTop && editorHasKeyboard();
    });
    MCGuiStep("Return on a heading goes there for good", [] { MCGuiKey("ctrl+shift+o"); });
    MCGuiWait("the headings", 10, [] { return pal()->isOpen() && pal()->labels().size() == 3; });
    MCGuiStep("\"sec\"", [] { MCGuiType("sec"); });
    MCGuiWait("Second, scrolled to", 10, [] {
        return selectedLabel() == "Second" && scrollY() > gTop + 500;
    });
    MCGuiStep("Return", [] { MCGuiKey("enter"); });
    MCGuiWait("it stays there, the keyboard in the page", 10, [] {
        return !pal()->isOpen() && scrollY() > gTop + 500 && editorHasKeyboard();
    });
}
