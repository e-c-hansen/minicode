// LineCommandsSuite.cpp — `./build/minicode-guitest line_commands`: the line
// commands (linux/PARITY.md item 1.9, Carets.h), VS Code's on Linux, every
// key real: Ctrl+L selects the line and the next with each press,
// Ctrl+Shift+K deletes it, Alt+Up and Alt+Down move it (nothing at the
// edges), Shift+Alt+Up and Shift+Alt+Down copy it, the last line without a
// final break included; Ctrl+C and Ctrl+X with nothing selected take the
// whole line and such a copy pastes above the caret's line, while text
// copied anywhere else pastes at the caret; at two carets the lines of each
// move. An emoji (outside the BMP, one character to GTK) moves and copies
// whole. The counterpart of the Mac's EditorKeysSuite's line steps and the
// line copy of its MultiCaretView.
#include "CaretSuites.h"

using namespace caretsuite;

namespace {

std::string F(const std::string& name) { return MCGuiProjectFile(name); }

// 0 "alpha 🙂\n", 8 "beta\n", 13 "gamma\n", 19 "delta" (no final break)
const std::string kText = "alpha \xF0\x9F\x99\x82\nbeta\ngamma\ndelta";
const std::string kAlpha = "alpha \xF0\x9F\x99\x82";

std::string gClip;

void queueReset() {
    MCGuiStep("the text as it was", [] {
        carets()->clear();
        gtk_text_buffer_set_text(buf(), kText.c_str(), -1);
        GtkTextIter it;
        gtk_text_buffer_get_start_iter(buf(), &it);
        gtk_text_buffer_place_cursor(buf(), &it);
    });
    MCGuiWait("back", 5, [] { return text() == kText && caretsText() == "0"; });
}

}  // namespace

MC_GUI_SUITE(line_commands) {
    MCGuiStep("a file of four lines, the last without a break", [] {
        MCGuiWriteFile(F("lines.txt"), kText);
        MCGuiOpenFileKept(F("lines.txt"));
    });
    MCGuiWait("it shows", 10, [] { return ed()->currentPath() == F("lines.txt") && text() == kText; });
    MCGuiWait("laid out", 5, [] { return gtk_widget_get_height(view()) > 100; });

    // ------------------------------------------------ Ctrl+L, Ctrl+Shift+K
    MCGuiStep("a click in the first line; Ctrl+L", [] {
        clickAt(1);
        MCGuiKey("ctrl+l");
    });
    waitCarets("the line with its break", "0>8");
    MCGuiStep("Ctrl+L again", [] { MCGuiKey("ctrl+l"); });
    waitCarets("and the next", "0>13");
    MCGuiStep("a click in beta; Ctrl+Shift+K", [] {
        clickAt(9);
        MCGuiKey("ctrl+shift+k");
    });
    MCGuiWait("the line gone, the caret on the next at its column", 5, [] {
        return text() == kAlpha + "\ngamma\ndelta" && caretsText() == "9";
    });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("back", 5, [] { return text() == kText; });

    // ------------------------------------------------ Alt+Up, Alt+Down
    queueReset();
    MCGuiStep("a click in the first line; Alt+Down", [] {
        clickAt(2);
        MCGuiKey("alt+down");
    });
    MCGuiWait("it moves below beta, the emoji whole, the caret with it", 5, [] {
        return text() == "beta\n" + kAlpha + "\ngamma\ndelta" && caretsText() == "7";
    });
    MCGuiStep("Alt+Up", [] { MCGuiKey("alt+up"); });
    MCGuiWait("back", 5, [] { return text() == kText && caretsText() == "2"; });
    MCGuiStep("Alt+Up at the top", [] { MCGuiKey("alt+up"); });
    MCGuiPause(0.3);   // a negative check: nothing moves at the edge
    MCGuiStep("moves nothing", [] {
        GCHECK_EQ_STR(text(), kText);
        GCHECK_EQ_STR(caretsText(), "2");
    });
    MCGuiStep("a click in the last line; Alt+Down", [] {
        clickAt(21);
        MCGuiKey("alt+down");
    });
    MCGuiPause(0.3);   // a negative check: nothing moves at the edge
    MCGuiStep("moves nothing either", [] {
        GCHECK_EQ_STR(text(), kText);
        GCHECK_EQ_STR(caretsText(), "21");
    });
    MCGuiStep("Alt+Up from the last line", [] { MCGuiKey("alt+up"); });
    MCGuiWait("it swaps with gamma, which takes the break", 5, [] {
        return text() == kAlpha + "\nbeta\ndelta\ngamma" && caretsText() == "15";
    });

    // ------------------------------------------------ Shift+Alt+Up and Down
    queueReset();
    MCGuiStep("a click in beta; Shift+Alt+Down", [] {
        clickAt(9);
        MCGuiKey("shift+alt+down");
    });
    MCGuiWait("a copy below, the caret on it", 5, [] {
        return text() == kAlpha + "\nbeta\nbeta\ngamma\ndelta" && caretsText() == "14";
    });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("back", 5, [] { return text() == kText; });
    MCGuiStep("a click in the first line; Shift+Alt+Up", [] {
        clickAt(2);
        MCGuiKey("shift+alt+up");
    });
    MCGuiWait("a copy above, the emoji whole, the caret on the copy", 5, [] {
        return text() == kAlpha + "\n" + kText && caretsText() == "2";
    });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("back", 5, [] { return text() == kText; });
    MCGuiStep("a click in the last line; Shift+Alt+Down", [] {
        clickAt(21);
        MCGuiKey("shift+alt+down");
    });
    MCGuiWait("a copy below the last line, a break between", 5, [] {
        return text() == kText + "\ndelta" && caretsText() == "27";
    });

    // ------------------------------------------------ whole lines on the clipboard
    queueReset();
    MCGuiStep("a click in beta; Ctrl+C with nothing selected", [] {
        clickAt(10);
        MCGuiKey("ctrl+c");
    });
    MCGuiAsync("the clipboard", 5, [](MCGuiDone done) { readClipboard(&gClip, done); });
    MCGuiStep("holds the line with its break, the text unchanged", [] {
        GCHECK_EQ_STR(gClip, "beta\n");
        GCHECK_EQ_STR(text(), kText);
    });
    MCGuiStep("a click in delta; Ctrl+V", [] {
        clickAt(21);
        MCGuiKey("ctrl+v");
    });
    MCGuiWait("the line goes in above, the caret on its character", 5, [] {
        return text() == kAlpha + "\nbeta\ngamma\nbeta\ndelta" && caretsText() == "26";
    });
    queueReset();
    MCGuiStep("a click in the first line; Ctrl+X with nothing selected", [] {
        clickAt(3);
        MCGuiKey("ctrl+x");
    });
    MCGuiWait("the whole line cut", 5, [] { return text() == "beta\ngamma\ndelta" && caretsText() == "3"; });
    MCGuiAsync("the clipboard", 5, [](MCGuiDone done) { readClipboard(&gClip, done); });
    MCGuiStep("holds it, the emoji whole", [] { GCHECK_EQ_STR(gClip, kAlpha + "\n"); });
    MCGuiStep("a click in gamma; Ctrl+V", [] {
        clickAt(7);
        MCGuiKey("ctrl+v");
    });
    MCGuiWait("above gamma", 5, [] { return text() == "beta\n" + kAlpha + "\ngamma\ndelta"; });
    MCGuiStep("text copied elsewhere; Ctrl+V with nothing selected", [] {
        gdk_clipboard_set_text(gtk_widget_get_clipboard(view()), "beta\n");
        clickAt(1);
        MCGuiKey("ctrl+v");
    });
    MCGuiWait("goes in at the caret, not above", 5, [] {
        return text() == "bbeta\neta\n" + kAlpha + "\ngamma\ndelta";
    });

    // ------------------------------------------------ at two carets
    queueReset();
    // A click where the last one was, within the double-click time, would
    // be a double-click: time is the condition.
    MCGuiPause(0.5);
    MCGuiStep("carets in alpha and gamma; Alt+Down", [] {
        clickAt(1);
        clickAt(14, "alt");
    });
    waitCarets("two", "1 14");
    MCGuiStep("Alt+Down", [] { MCGuiKey("alt+down"); });
    MCGuiWait("each line moves down one, the carets with them", 5, [] {
        return text() == "beta\n" + kAlpha + "\ndelta\ngamma" && caretsText() == "6 20";
    });
    MCGuiStep("Ctrl+Shift+K", [] { MCGuiKey("ctrl+shift+k"); });
    MCGuiWait("both lines gone", 5, [] { return text() == "beta\ndelta"; });
    MCGuiStep("the tab closes", [] { ed()->closeAllTabs(); });
}
