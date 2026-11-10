// EditorWordsSuite.cpp — `./build/minicode-guitest editor_words`: words by
// VS Code's default editor.wordSeparators in source text (linux/PARITY.md
// item 1.9, Carets.h; MultiCaret::wordSelection), the counterpart of the
// Mac's EditorKeysSuite. A double-click on arr in arr.size selects arr, $
// separates and _ does not (GTK's words, Pango's, stop at the underscore
// of value_2); a double-click dragged selects by these words either way; a
// triple click takes the line with its break; Ctrl+Right and Ctrl+Left
// stop at the dot, with Shift selecting, and Ctrl+Backspace and
// Ctrl+Delete delete by the same words (GTK's took ".size" whole), at one
// caret or two; a double-click in a Markdown page still edits the block.
// Ctrl+Down and Ctrl+Up scroll a line with the caret staying, VS Code's
// scrollLineDown and scrollLineUp (GTK's move by paragraphs).
#include "CaretSuites.h"

#include <cmath>

using namespace caretsuite;

namespace {

std::string F(const std::string& name) { return MCGuiProjectFile(name); }

// "n = arr.size": arr is 4..7, size 8..12; line 2 starts at 13:
// "x = $el.value_2 + 1": $ 17, el 18..20, value_2 21..28.
const std::string kText = "n = arr.size\nx = $el.value_2 + 1\ndone\n";

void doubleClickOn(size_t offset) {
    double x = 0, y = 0;
    middleOf(offset, &x, &y);
    MCGuiDoubleClick(view(), x, y);
}

double scrollY() {
    return gtk_adjustment_get_value(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view())));
}

double gY = 0;
size_t gPage = 0;

}  // namespace

MC_GUI_SUITE(editor_words) {
    MCGuiStep("a Python file", [] {
        MCGuiWriteFile(F("words.py"), kText);
        MCGuiOpenFileKept(F("words.py"));
    });
    MCGuiWait("it shows", 10, [] { return ed()->currentPath() == F("words.py") && text() == kText; });
    MCGuiWait("laid out", 5, [] { return gtk_widget_get_height(view()) > 100; });

    // ------------------------------------------------ double-clicks
    MCGuiStep("a double-click on arr", [] { doubleClickOn(5); });
    waitCarets("selects arr, not arr.size", "4>7");
    // GTK, and the carets' own click counting, take a press within the
    // double-click time of the last as part of it: time is the condition.
    MCGuiPause(0.6);
    MCGuiStep("a double-click on size", [] { doubleClickOn(9); });
    waitCarets("selects size", "8>12");
    MCGuiPause(0.6);
    MCGuiStep("a double-click on el", [] { doubleClickOn(19); });
    waitCarets("$ separates: el alone", "18>20");
    MCGuiPause(0.6);
    MCGuiStep("a double-click on value_2's underscore", [] { doubleClickOn(26); });
    waitCarets("_ does not: value_2 whole", "21>28");
    MCGuiPause(0.6);
    MCGuiStep("a double-click on arr dragged to size", [] {
        double x0, y0, x1, y1;
        middleOf(5, &x0, &y0);
        middleOf(9, &x1, &y1);
        MCGuiClick(view(), x0, y0);
        MCGuiDrag(view(), x0, y0, x1, y1);
    });
    waitCarets("whole words, arr to size", "4>12");
    MCGuiPause(0.6);
    MCGuiStep("a double-click on size dragged back to arr", [] {
        double x0, y0, x1, y1;
        middleOf(10, &x0, &y0);
        middleOf(5, &x1, &y1);
        MCGuiClick(view(), x0, y0);
        MCGuiDrag(view(), x0, y0, x1, y1);
    });
    waitCarets("still whole words, the other way", "12>4");
    MCGuiPause(0.6);
    MCGuiStep("a triple click on arr", [] {
        double x, y;
        middleOf(5, &x, &y);
        MCGuiClick(view(), x, y);
        MCGuiClick(view(), x, y);
        MCGuiClick(view(), x, y);
    });
    waitCarets("the whole line with its break", "0>13");
    MCGuiPause(0.6);

    // ------------------------------------------------ Ctrl+arrows
    MCGuiStep("the caret before arr; Ctrl+Right", [] {
        clickAt(4);
        MCGuiKey("ctrl+right");
    });
    waitCarets("stops at the dot", "7");
    MCGuiStep("the caret at the end of the line; Ctrl+Left", [] {
        clickAt(12);
        MCGuiKey("ctrl+left");
    });
    waitCarets("stops after the dot", "8");
    MCGuiStep("again from the end, with Shift", [] {
        MCGuiKey("end");
        MCGuiKey("ctrl+shift+left");
    });
    waitCarets("size selected", "12>8");
    MCGuiStep("the end again; Ctrl+Backspace", [] {
        MCGuiKey("end");
        MCGuiKey("ctrl+backspace");
    });
    MCGuiWait("size deleted, the dot kept", 5,
              [] { return text() == "n = arr.\nx = $el.value_2 + 1\ndone\n"; });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("back", 5, [] { return text() == kText; });
    MCGuiStep("carets before arr and el; Ctrl+Right", [] {
        clickAt(4);
        clickAt(18, "alt");
    });
    waitCarets("two", "4 18");
    MCGuiStep("Ctrl+Right", [] { MCGuiKey("ctrl+right"); });
    waitCarets("the same words at both", "7 20");
    MCGuiStep("Ctrl+Delete", [] { MCGuiKey("ctrl+delete"); });
    MCGuiWait("the dot after each, which is a word of its own", 5,
              [] { return text() == "n = arrsize\nx = $elvalue_2 + 1\ndone\n"; });
    MCGuiStep("Escape; Ctrl+Z", [] {
        MCGuiKey("escape");
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("back", 5, [] { return text() == kText; });

    // ------------------------------------------------ a Markdown page
    MCGuiStep("a Markdown file, its page on show", [] {
        MCGuiWriteFile(F("words.md"), "Call don't here.\n");
        MCGuiOpenFileKept(F("words.md"));
    });
    MCGuiWait("the page", 10, [] {
        return ed()->currentPath() == F("words.md") && ed()->inPreview() &&
               MCGuiEditorText().find("don't") != std::string::npos;
    });
    MCGuiWait("laid out", 5, [] { return gtk_widget_get_height(view()) > 100; });
    // On the Mac a read-only page keeps AppKit's words; here a page has no
    // caret to move by words, and its double-click edits the block
    // (Editor.cpp's editMarkdownBlock), which the carets leave alone.
    MCGuiStep("a double-click on don't in the page", [] {
        gPage = (size_t)g_utf8_strlen(MCGuiEditorText().c_str(), (gssize)MCGuiEditorText().find("don"));
        doubleClickOn(gPage + 1);
    });
    MCGuiWait("the block's editor opens, as before", 5, [] {
        for (GtkWidget* c = gtk_widget_get_first_child(view()); c; c = gtk_widget_get_next_sibling(c))
            if (GTK_IS_POPOVER(c) && gtk_widget_get_visible(c)) return true;
        return false;
    });
    MCGuiStep("Escape closes it", [] { MCGuiKey("escape"); });
    MCGuiWait("closed", 5, [] {
        for (GtkWidget* c = gtk_widget_get_first_child(view()); c; c = gtk_widget_get_next_sibling(c))
            if (GTK_IS_POPOVER(c) && gtk_widget_get_visible(c)) return false;
        return true;
    });

    // ------------------------------------------------ Ctrl+Down and Ctrl+Up
    MCGuiStep("a file of 300 lines; the caret on line 3", [] {
        std::string s;
        for (int i = 0; i < 300; i++) s += "line " + std::to_string(i) + "\n";
        MCGuiWriteFile(F("scroll.py"), s);
        MCGuiOpenFileKept(F("scroll.py"));
    });
    MCGuiWait("it shows", 10, [] { return ed()->currentPath() == F("scroll.py"); });
    MCGuiWait("laid out", 5, [] { return gtk_widget_get_height(view()) > 100; });
    MCGuiStep("a click on line 3; Ctrl+Down three times", [] {
        clickAt(offsetOf("line 2"));
        gY = scrollY();
        MCGuiKey("ctrl+down");
        MCGuiKey("ctrl+down");
        MCGuiKey("ctrl+down");
    });
    MCGuiWait("the view three lines down", 5, [] { return scrollY() > gY + 30; });
    MCGuiStep("the caret where it was", [] {
        MCGuiNote("scrolled %.0f px for three lines", scrollY() - gY);
        GCHECK(caretsText() == std::to_string(offsetOf("line 2")));
        GCHECK(scrollY() < gY + 80);
    });
    MCGuiStep("Ctrl+Up three times", [] {
        MCGuiKey("ctrl+up");
        MCGuiKey("ctrl+up");
        MCGuiKey("ctrl+up");
    });
    MCGuiWait("back at the top", 5, [] { return std::fabs(scrollY() - gY) < 1; });
    MCGuiStep("the caret still there; the tabs close", [] {
        GCHECK(caretsText() == std::to_string(offsetOf("line 2")));
        ed()->closeAllTabs();
    });
}
