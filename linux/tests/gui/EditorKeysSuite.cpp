// EditorKeysSuite.cpp — `./build/mutible-guitest editor_keys`: Home and End
// as VS Code's cursorHome and cursorEnd (the Mac's #213, its
// tests/gui/EditorKeysSuite.mm's Home and End steps), through the core's
// MultiCaret::homeTarget and endTarget (Carets.h), every press real (XTest):
//
//   - End goes to the line's end; Home to its first non-blank character,
//     then to its start, then back; End then Left twice stays on the line;
//   - Shift+End and Shift+Home select from the anchor, Shift+Right after
//     gives a character back, and Shift+End goes on past the anchor;
//   - with several carets each moves, and Shift selects at each;
//   - on a wrapped line End goes to the end of the row on screen first
//     (the caret drawn on that row), then to the line's end, Home on a
//     later row to the row's start, then the line's, and Shift+End selects
//     to the row's end.
#include "CaretSuites.h"

#include "Zoom.h"

#include <functional>
#include <string>

using namespace caretsuite;

namespace {

std::string F(const std::string& name) { return MCGuiProjectFile(name); }

// "def f():\n" is 0..8; "    return a + b" 9..24 (return at 13, its break
// at 25); "    pass" 26..33 (pass at 30, its break at 34).
const std::string kHome = "def f():\n    return a + b\n    pass\n";

void caretAt(size_t offset) {
    carets()->clear();
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)offset);
    gtk_text_buffer_place_cursor(buf(), &it);
}

// The wrapped file: "x\n", then a line of words from 2 to gLineEnd, then
// "y\n". Its first two rows on screen, as GTK laid them out.
std::string gWrapped;
size_t gLineEnd = 0, gRow1 = 0, gRow2 = 0;

size_t nextRowStart(size_t offset) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)offset);
    gtk_text_view_forward_display_line(tv(), &it);
    return (size_t)gtk_text_iter_get_offset(&it);
}

// waitCarets with what is wanted worked out when the wait runs (the rows
// are known only once the file is laid out).
void waitFor(const std::string& what, std::function<std::string()> want) {
    MCGuiWait(what, 5, [want] { return caretsText() == want(); });
    MCGuiStep(what + ": the carets", [want] { GCHECK_EQ_STR(caretsText(), want()); });
}

int yOf(size_t offset) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)offset);
    GdkRectangle r;
    gtk_text_view_get_iter_location(tv(), &it, &r);
    return r.y;
}

}  // namespace

MC_GUI_SUITE(editor_keys) {
    MCGuiStep("a Python file with indented lines, the editor with the keyboard", [] {
        MCGuiWriteFile(F("home.py"), kHome);
        MCGuiOpenFileKept(F("home.py"));
    });
    MCGuiWait("it shows", 10, [] { return ed()->currentPath() == F("home.py") && text() == kHome; });
    MCGuiWait("laid out", 5, [] { return gtk_widget_get_height(view()) > 100; });
    MCGuiStep("a click at the start", [] { clickAt(0); });
    MCGuiWait("the editor has the keyboard", 5, [] { return editorHasKeyboard(); });
    waitCarets("the caret at 0", "0");

    // ------------------------------------------------ one caret
    MCGuiStep("in \"return\"; End", [] {
        caretAt(15);
        MCGuiKey("end");
    });
    waitCarets("the line's end", "25");
    MCGuiStep("Home", [] { MCGuiKey("home"); });
    waitCarets("its first non-blank character", "13");
    MCGuiStep("Home again", [] { MCGuiKey("home"); });
    waitCarets("the line's start", "9");
    MCGuiStep("and again", [] { MCGuiKey("home"); });
    waitCarets("the first non-blank character again", "13");
    // A grader's walk on the Mac: End, Left, Left from the line's start
    // typed a line up when End had scrolled instead.
    MCGuiStep("from the line's start: End, Left, Left", [] {
        caretAt(9);
        MCGuiKey("end");
        MCGuiKey("left");
        MCGuiKey("left");
    });
    waitCarets("two before the line's end, on the line", "23");
    MCGuiStep("nothing typed", [] {
        GCHECK_EQ_STR(text(), kHome);
        GCHECK(!ed()->dirty());
    });

    // ------------------------------------------------ Shift
    MCGuiStep("Shift+End", [] {
        caretAt(15);
        MCGuiKey("shift+end");
    });
    waitCarets("selected to the line's end", "15>25");
    MCGuiStep("Shift+Home from 15", [] {
        caretAt(15);
        MCGuiKey("shift+home");
    });
    waitCarets("selected back to the first non-blank character", "15>13");
    MCGuiStep("Shift+Home again", [] { MCGuiKey("shift+home"); });
    waitCarets("to the line's start", "15>9");
    MCGuiStep("Shift+Right", [] { MCGuiKey("shift+right"); });
    waitCarets("a character given back, the anchor kept", "15>10");
    MCGuiStep("Shift+End", [] { MCGuiKey("shift+end"); });
    waitCarets("past the anchor to the line's end", "15>25");
    MCGuiStep("Shift+Left", [] { MCGuiKey("shift+left"); });
    waitCarets("a character back from the end", "15>24");
    MCGuiStep("nothing typed", [] { GCHECK_EQ_STR(text(), kHome); });

    // ------------------------------------------------ several carets
    MCGuiStep("carets in \"return\" and \"pass\"; End", [] {
        caretAt(0);
        carets()->set({{15, 15}, {32, 32}});
        GCHECK(carets()->active());
        MCGuiKey("end");
    });
    waitCarets("each at its line's end", "25 34");
    MCGuiStep("Home", [] { MCGuiKey("home"); });
    waitCarets("each at its first non-blank character", "13 30");
    MCGuiStep("Home again", [] { MCGuiKey("home"); });
    waitCarets("each at its line's start", "9 26");
    MCGuiStep("again from 15 and 32: Shift+End", [] {
        carets()->set({{15, 15}, {32, 32}});
        MCGuiKey("shift+end");
    });
    waitCarets("each selected to its line's end", "15>25 32>34");
    MCGuiStep("again: Shift+Home", [] {
        carets()->set({{15, 15}, {32, 32}});
        MCGuiKey("shift+home");
    });
    waitCarets("each selected to its first non-blank character", "15>13 32>30");
    MCGuiStep("still several, nothing typed; Escape", [] {
        GCHECK(carets()->active());
        GCHECK_EQ_STR(text(), kHome);
        MCGuiKey("escape");
    });
    MCGuiWait("one caret", 5, [] { return !carets()->active(); });

    // ------------------------------------------------ a wrapped line
    MCGuiStep("a file with a line of 600 characters, wrapping on", [] {
        GCHECK(Zoom::wordWrap());
        std::string line;
        while (line.size() < 600) line += "word ";
        line += "end";
        gWrapped = "x\n" + line + "\ny\n";
        gLineEnd = 2 + line.size();
        MCGuiWriteFile(F("wrapped.txt"), gWrapped);
        MCGuiOpenFileKept(F("wrapped.txt"));
    });
    MCGuiWait("it shows, wrapped", 10, [] {
        return ed()->currentPath() == F("wrapped.txt") && text() == gWrapped &&
               gtk_text_view_get_wrap_mode(tv()) != GTK_WRAP_NONE &&
               nextRowStart(2) < gLineEnd;
    });
    MCGuiStep("its rows", [] {
        gRow1 = nextRowStart(2);
        gRow2 = nextRowStart(gRow1);
        MCGuiNote("the line %d..%d; its second row from %d, its third from %d", 2, (int)gLineEnd,
                  (int)gRow1, (int)gRow2);
        GCHECK(gRow1 > 2 && gRow1 < gLineEnd && gRow2 > gRow1 + 4);
        // The first row broke after a space, which ends it.
        GCHECK(gWrapped[gRow1 - 1] == ' ');
        gtk_widget_grab_focus(view());
        caretAt(5);
        MCGuiKey("end");
    });
    waitFor("End: the first row's end, before the space it broke at",
            [] { return std::to_string(gRow1 - 1); });
    MCGuiStep("the caret drawn on the first row", [] {
        MCGuiNote("y of the line's start %d, of the caret %d, of the next row %d", yOf(2),
                  yOf(gRow1 - 1), yOf(gRow1));
        GCHECK(yOf(gRow1 - 1) == yOf(2) && yOf(gRow1) > yOf(2));
        MCGuiKey("end");
    });
    waitFor("End again: the line's end", [] { return std::to_string(gLineEnd); });
    MCGuiStep("in the second row; Home", [] {
        caretAt(gRow1 + 3);
        MCGuiKey("home");
    });
    waitFor("the second row's start", [] { return std::to_string(gRow1); });
    MCGuiStep("Home again", [] { MCGuiKey("home"); });
    waitFor("the line's start", [] { return std::string("2"); });
    MCGuiStep("in the second row; Shift+End", [] {
        caretAt(gRow1 + 3);
        MCGuiKey("shift+end");
    });
    waitFor("selected to the second row's end",
            [] { return std::to_string(gRow1 + 3) + ">" + std::to_string(gRow2 - 1); });
    MCGuiStep("two carets on the first two rows; End", [] {
        carets()->set({{4, 4}, {gRow1 + 2, gRow1 + 2}});
        MCGuiKey("end");
    });
    waitFor("each at its row's end",
            [] { return std::to_string(gRow1 - 1) + " " + std::to_string(gRow2 - 1); });
    MCGuiStep("nothing typed; Escape", [] {
        GCHECK_EQ_STR(text(), gWrapped);
        GCHECK(!ed()->dirty());
        MCGuiKey("escape");
    });
    MCGuiWait("one caret", 5, [] { return !carets()->active(); });
}
