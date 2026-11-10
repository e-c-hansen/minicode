// MultiCaretSelectSuite.cpp — `./build/minicode-guitest multicaret_select`:
// the keys that make several carets (linux/PARITY.md item 1.9, Carets.h),
// VS Code's on Linux: Ctrl+D (the word at the caret, then each next whole
// word, never "arrays" for "arr", and nothing more once every one is in),
// Ctrl+Shift+L (every whole word, or every place a selection's text
// occurs), Ctrl+Shift+Down and Ctrl+Shift+Up (a caret a line below or above
// each, at its column), and the 10,000 carets VS Code stops at, said in the
// status bar. The counterpart of the Mac's MultiEditSuite's selection steps.
#include "CaretSuites.h"

using namespace caretsuite;

namespace {

std::string F(const std::string& name) { return MCGuiProjectFile(name); }

// 0 "n = arr.size + arrays\n", 22 "arr.push(arr)\n", 36 "total = arr\n"
const std::string kText = "n = arr.size + arrays\narr.push(arr)\ntotal = arr\n";

std::string statusText() {
    GtkWidget* l = MCGuiApp().statusLabel;
    return l ? gtk_label_get_text(GTK_LABEL(l)) : "";
}

}  // namespace

MC_GUI_SUITE(multicaret_select) {
    MCGuiStep("a Python file", [] {
        MCGuiWriteFile(F("select.py"), kText);
        MCGuiOpenFileKept(F("select.py"));
    });
    MCGuiWait("it shows", 10, [] { return ed()->currentPath() == F("select.py") && text() == kText; });
    MCGuiWait("laid out", 5, [] { return gtk_widget_get_height(view()) > 100; });
    MCGuiStep("a click inside the first arr", [] { clickAt(5); });
    MCGuiWait("the editor has the keyboard, the caret there", 5,
              [] { return editorHasKeyboard() && caretsText() == "5"; });

    // ------------------------------------------------ Ctrl+D
    MCGuiStep("Ctrl+D", [] { MCGuiKey("ctrl+d"); });
    waitCarets("the word at the caret selected", "4>7");
    MCGuiStep("Ctrl+D", [] { MCGuiKey("ctrl+d"); });
    waitCarets("the next whole arr, not arrays", "4>7 22>25");
    MCGuiStep("Ctrl+D twice", [] {
        MCGuiKey("ctrl+d");
        MCGuiKey("ctrl+d");
    });
    waitCarets("every arr", "4>7 22>25 31>34 44>47");
    MCGuiStep("Ctrl+D once more", [] { MCGuiKey("ctrl+d"); });
    MCGuiPause(0.3);   // a negative check: nothing more to add
    waitCarets("nothing more", "4>7 22>25 31>34 44>47");
    MCGuiStep("L typed over them", [] { MCGuiType("L"); });
    MCGuiWait("every arr replaced, arrays not", 5,
              [] { return text() == "n = L.size + arrays\nL.push(L)\ntotal = L\n"; });
    MCGuiStep("one Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("all four back", 5, [] { return text() == kText; });

    // ------------------------------------------------ Ctrl+Shift+L
    MCGuiStep("a click in the second line's arr; Ctrl+Shift+L", [] {
        clickAt(23);
        MCGuiKey("ctrl+shift+l");
    });
    waitCarets("every whole arr", "4>7 22>25 31>34 44>47");
    MCGuiStep("ar selected by hand; Ctrl+Shift+L", [] {
        clickAt(4);
        MCGuiKey("shift+right");
        MCGuiKey("shift+right");
        MCGuiKey("ctrl+shift+l");
    });
    waitCarets("every place ar occurs, arrays too", "4>6 15>17 22>24 31>33 44>46");
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    waitCarets("one caret", "6");

    // ------------------------------------------------ Ctrl+Shift+Down and Up
    // A click where the last one was, within the double-click time, would
    // be a double-click: time is the condition.
    MCGuiPause(0.5);
    MCGuiStep("a caret at column 4; Ctrl+Shift+Down twice", [] {
        clickAt(4);
        MCGuiKey("ctrl+shift+down");
        MCGuiKey("ctrl+shift+down");
    });
    waitCarets("a caret at column 4 of each line", "4 26 40");
    MCGuiStep("_ typed", [] { MCGuiType("_"); });
    MCGuiWait("at all three", 5,
              [] { return text() == "n = _arr.size + arrays\narr._push(arr)\ntota_l = arr\n"; });
    MCGuiStep("one Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("all three back", 5, [] { return text() == kText; });
    MCGuiStep("a caret on the last line; Ctrl+Shift+Up", [] {
        clickAt(45);
        MCGuiKey("ctrl+shift+up");
    });
    waitCarets("one above it, at its column", "31 45");
    MCGuiStep("Ctrl+Shift+Up from the first line", [] {
        clickAt(2);
        MCGuiKey("ctrl+shift+up");
    });
    MCGuiPause(0.3);   // a negative check: no line above
    waitCarets("nothing above the first line", "2");

    // ------------------------------------------------ the limit
    MCGuiStep("a file of 12,000 lines of x; Ctrl+Shift+L on the first", [] {
        std::string many;
        for (int i = 0; i < 12000; i++) many += "x\n";
        MCGuiWriteFile(F("many.py"), many);
        MCGuiOpenFileKept(F("many.py"));
    });
    MCGuiWait("it shows", 10, [] { return ed()->currentPath() == F("many.py"); });
    MCGuiStep("Ctrl+Shift+L at the first x", [] {
        clickAt(0);
        MCGuiKey("ctrl+shift+l");
    });
    MCGuiWait("10,000 carets, as VS Code stops", 10,
              [] { return carets()->current().size() == Carets::kMaxCarets; });
    MCGuiStep("the first around where it began, and the status bar says so", [] {
        GCHECK(carets()->current().front().lo() == 0);
        GCHECK(statusText().find("limited to 10000") != std::string::npos);
        MCGuiNote("status: %s", statusText().c_str());
    });
    MCGuiStep("Ctrl+Shift+Down past it", [] { MCGuiKey("ctrl+shift+down"); });
    MCGuiPause(0.3);   // a negative check: no more carets
    MCGuiStep("adds none", [] { GCHECK(carets()->current().size() == Carets::kMaxCarets); });
    MCGuiStep("Escape; the tabs close", [] {
        MCGuiKey("escape");
        ed()->closeAllTabs();
    });
}
