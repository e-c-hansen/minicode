// ProblemsSuite.cpp — `./build/mutible-guitest problems`: the Problems list
// (Ctrl+Shift+M, VS Code's Linux key) and F8 and Shift+F8 (linux/PARITY.md
// item 2.2; the Mac's tests/gui/ProblemsSuite.mm), against the scripted
// language server (build/fakelsp), with real keys and clicks:
//
//   - every file's problems listed under a heading per file with its
//     counts, a file no tab has open included (a server checking the whole
//     folder), files with errors first and each file's errors before its
//     warnings; hints left out; the status bar counts them;
//   - Ctrl+Shift+M shows the list with the keyboard in it; Down and Up
//     move through the problems, a heading skipped; Enter opens a file no
//     tab has at its problem, a click opens another, Escape gives the
//     keyboard back to the editor with the list still showing;
//   - F8 steps through the file's problems from the caret, wrapping, and
//     Shift+F8 back, the message at the caret each time, without taking the
//     keyboard (the next F8 still works);
//   - closing a tab drops what its server said about it while open;
//   - Ctrl+W with the keyboard in the list closes the list, not the tab;
//     the status bar's counts open and close it with a click;
//   - a JSON file no server has is checked by the editor (#213): its
//     trailing comma one error, drawn, listed and counted, gone once taken
//     out and back once typed again; none for tsconfig.json's comments and
//     trailing commas; gone when its tab closes.
#include "Harness.h"

#include "Editor.h"
#include "FakeLspLog.h"
#include "Lsp.h"
#include "ProblemsPanel.h"
#include "WindowLayout.h"

#include <climits>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
LspSession* lsp() { return MCGuiApp().lsp; }
ProblemsPanel* panel() { return MCGuiApp().problems; }
std::string F(const std::string& name) { return MCGuiProjectFile("probs/" + name); }
const char* kServer = "problems";

std::string real(const std::string& p) {
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}

const std::string kFirst = "import os\nx = undefined_name\n\n\ndef f():\n    return y\n";

std::string str(const std::string& s) { return Json(s).dump(); }
std::string diag(int line, int c0, int c1, int severity, const std::string& message,
                 const std::string& code) {
    return "{\"range\": {\"start\": {\"line\": " + std::to_string(line) + ", \"character\": " +
           std::to_string(c0) + "}, \"end\": {\"line\": " + std::to_string(line) +
           ", \"character\": " + std::to_string(c1) + "}}, \"severity\": " +
           std::to_string(severity) + ", \"message\": " + str(message) +
           ", \"source\": \"fake\", \"code\": " + str(code) + "}";
}

std::string script() {
    const std::string unopened = "file://" + real(F("unopened.py"));
    return "{\"name\": \"fakelsp\", \"rules\": ["
           "{\"method\": \"textDocument/didOpen\", \"when\": {\"uriEndsWith\": \"first.py\"},"
           " \"notify\": [{\"method\": \"textDocument/publishDiagnostics\", \"params\": {"
           "\"uri\": \"${uri}\", \"version\": \"${version}\", \"diagnostics\": [" +
           diag(0, 7, 9, 2, "`os` imported but unused", "F401") + ", " +
           diag(1, 4, 18, 1, "`undefined_name` is not defined", "F821") + ", " +
           diag(5, 11, 12, 1, "`y` is not defined", "F821") + ", " +
           diag(4, 4, 5, 4, "`f` is not accessed", "H1") + "]}},"
           " {\"method\": \"textDocument/publishDiagnostics\", \"params\": {\"uri\": " +
           str(unopened) + ", \"diagnostics\": [" + diag(0, 0, 5, 2, "`print` is shadowed", "W1") +
           "]}}]},"
           "{\"method\": \"textDocument/didOpen\", \"when\": {\"uriEndsWith\": \"second.py\"},"
           " \"notify\": [{\"method\": \"textDocument/publishDiagnostics\", \"params\": {"
           "\"uri\": \"${uri}\", \"version\": \"${version}\", \"diagnostics\": [" +
           diag(0, 0, 5, 3, "a note about print", "I1") + "]}}]}]}";
}

bool onShow(const std::string& name) { return ed()->currentPath() == F(name) && ed()->isSource(); }

bool editorHasKeyboard() {
    return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)) == ed()->textView();
}

void showTab(const std::string& name) {
    MCGuiOpenFileKept(F(name));
    MCGuiFocusKeyWindow();
    ed()->focusActiveDocument();
}

int caretOffset() {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(ed()->buffer(), &it, gtk_text_buffer_get_insert(ed()->buffer()));
    return gtk_text_iter_get_offset(&it);
}

int caretLine() {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(ed()->buffer(), &it, gtk_text_buffer_get_insert(ed()->buffer()));
    return gtk_text_iter_get_line(&it);
}

int offsetOf(int line, int column) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line_offset(ed()->buffer(), &it, line, column);
    return gtk_text_iter_get_offset(&it);
}

void caretAt(int offset) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(ed()->buffer(), &it, offset);
    gtk_text_buffer_place_cursor(ed()->buffer(), &it);
}

std::string rowsJoined() {
    std::string s;
    for (const std::string& r : panel()->rowTexts()) s += (s.empty() ? "" : " | ") + r;
    return s;
}

int rowOf(const std::string& part) {
    const std::vector<std::string> rows = panel()->rowTexts();
    for (size_t i = 0; i < rows.size(); i++)
        if (rows[i].find(part) != std::string::npos) return (int)i;
    return -1;
}

bool listHasKeyboard() { return panel()->shown() && panel()->hasFocus(); }

std::string gSettings;
int gStep = 0;   // F8's expected places, in order

const std::string kPackage = "{\n  \"name\": \"x\",\n  \"version\": \"1.0.0\",\n}\n";

// Whether an error's squiggle is on the character at `offset`.
bool errorTagAt(int offset) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(ed()->buffer(), &it, offset);
    GtkTextTag* t = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(ed()->buffer()),
                                              "lsp-error");
    return t && gtk_text_iter_has_tag(&it, t);
}

}  // namespace

MC_GUI_SUITE(problems) {
    MCGuiStep("three files, a server with problems in each and in one no tab has", [] {
        MCGuiSetKeyWindow(nullptr);
        ed()->closeAllTabs();
        MCGuiWriteFile(F("first.py"), kFirst);
        MCGuiWriteFile(F("second.py"), "print('two')\n");
        MCGuiWriteFile(F("unopened.py"), "print('three')\n");
        gSettings = MCGuiReadFile(g_getenv("MUTIBLE_SETTINGS"));
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + MCGuiFakeLsp(kServer, script()) +
                         "\nlsp.python.also = off\n");
        GCHECK(!panel()->shown());
        showTab("second.py");
        showTab("first.py");
    });
    MCGuiWait("the problems arrive, the squiggles drawn", 20, [] {
        return onShow("first.py") && lsp()->problemErrors() == 2 &&
               lsp()->problemWarnings() == 2 &&
               lsp()->statusText().find("2 errors, 1 warning") != std::string::npos;
    });
    MCGuiWait("the status bar counts every file's", 5, [] {
        return panel()->statusText() == "2 errors, 2 warnings";
    });
    // The label is laid out at the frame after its text changes, which a
    // loaded machine draws late: wait for it rather than check at once.
    MCGuiWait("the counts are on screen", 5, [] { return MCGuiOnScreen(panel()->statusLabel()); });
    MCGuiStep("Ctrl+Shift+M", [] {
        MCGuiNote("status bar: %s", panel()->statusText().c_str());
        GCHECK(editorHasKeyboard());
        MCGuiKey("ctrl+shift+m");
    });
    MCGuiWait("the list, with the keyboard", 10, [] {
        return panel()->shown() && MCGuiOnScreen(panel()->widget()) && listHasKeyboard() &&
               panel()->rowTexts().size() >= 7;
    });
    MCGuiStep("by file, errors first; a heading per file, the unopened one too; no hint", [] {
        MCGuiNote("rows: %s", rowsJoined().c_str());
        const std::vector<std::string> rows = panel()->rowTexts();
        GCHECK(rows.size() == 8);
        if (rows.size() == 8) {
            GCHECK_EQ_STR(rows[0], "# first.py \xE2\x80\x94 2 errors, 1 warning");
            GCHECK_EQ_STR(rows[1], "E 2:5 `undefined_name` is not defined [fake(F821)]");
            GCHECK_EQ_STR(rows[2], "E 6:12 `y` is not defined [fake(F821)]");
            GCHECK_EQ_STR(rows[3], "W 1:8 `os` imported but unused [fake(F401)]");
            GCHECK_EQ_STR(rows[4], "# unopened.py \xE2\x80\x94 1 warning");
            GCHECK_EQ_STR(rows[5], "W 1:1 `print` is shadowed [fake(W1)]");
            GCHECK_EQ_STR(rows[6], "# second.py \xE2\x80\x94 1 other");
            GCHECK_EQ_STR(rows[7], "I 1:1 a note about print [fake(I1)]");
        }
        GCHECK(rowsJoined().find("not accessed") == std::string::npos);
        GCHECK_EQ_STR(panel()->countsText(), "2 errors, 2 warnings");
        GCHECK(panel()->selectedRow() == 1);
        // The list sits between the editor and the status bar, across the
        // right side.
        GCHECK(MCGuiApp().layout->problemsShown());
        MCGuiKey("down");
    });
    MCGuiWait("the next problem", 5, [] { return panel()->selectedRow() == 2; });
    MCGuiStep("Down twice: past unopened.py's heading", [] {
        MCGuiKey("down");
        MCGuiKey("down");
    });
    MCGuiWait("unopened.py's problem", 5, [] { return panel()->selectedRow() == 5; });
    MCGuiStep("Up and Down again", [] {
        MCGuiKey("up");
        MCGuiKey("down");
    });
    MCGuiWait("back on it", 5, [] { return panel()->selectedRow() == 5; });
    MCGuiStep("Enter", [] { MCGuiKey("enter"); });
    MCGuiWait("unopened.py on show at its problem, a tab of its own", 10, [] {
        return onShow("unopened.py") && caretLine() == 0 && panel()->shown();
    });

    // A click on a row.
    MCGuiStep("Ctrl+Shift+M gives the list the keyboard again", [] {
        MCGuiFocusKeyWindow();
        MCGuiKey("ctrl+shift+m");
    });
    MCGuiWait("it has it, still showing", 10, [] {
        return listHasKeyboard() && panel()->rowWidget(rowOf("`y` is not defined")) &&
               MCGuiOnScreen(panel()->rowWidget(rowOf("`y` is not defined")));
    });
    MCGuiStep("a click on first.py's `y`", [] {
        MCGuiClick(panel()->rowWidget(rowOf("`y` is not defined")));
    });
    MCGuiWait("first.py at line 6, column 12", 10, [] {
        return onShow("first.py") && caretOffset() == offsetOf(5, 11);
    });

    // Escape.
    MCGuiStep("Ctrl+Shift+M, then Escape", [] {
        MCGuiFocusKeyWindow();
        MCGuiKey("ctrl+shift+m");
    });
    MCGuiWait("the list has the keyboard", 10, listHasKeyboard);
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("the editor has it back, the list still showing", 10, [] {
        return editorHasKeyboard() && panel()->shown();
    });

    // F8 and Shift+F8.
    MCGuiStep("the caret at first.py's start; F8", [] {
        caretAt(0);
        gStep = 0;
        MCGuiKey("F8");
    });
    MCGuiWait("on `os`, its message at the caret", 10, [] {
        return caretOffset() == 7 && lsp()->hoverVisible() &&
               lsp()->lastHoverText().find("`os` imported but unused") != std::string::npos;
    });
    MCGuiStep("the box leaves the keyboard alone; F8", [] {
        GCHECK(editorHasKeyboard());
        MCGuiKey("F8");
    });
    MCGuiWait("on undefined_name", 10, [] {
        return caretOffset() == offsetOf(1, 4) &&
               lsp()->lastHoverText().find("`undefined_name` is not defined (fake F821)") !=
                   std::string::npos;
    });
    MCGuiStep("F8", [] { MCGuiKey("F8"); });
    MCGuiWait("on y", 10, [] { return caretOffset() == offsetOf(5, 11); });
    MCGuiStep("F8 again wraps (the hint is not a stop)", [] { MCGuiKey("F8"); });
    MCGuiWait("on `os` again", 10, [] { return caretOffset() == 7; });
    MCGuiStep("Shift+F8 goes back round", [] { MCGuiKey("shift+F8"); });
    MCGuiWait("on y, its message shown", 10, [] {
        return caretOffset() == offsetOf(5, 11) && lsp()->hoverVisible() &&
               lsp()->lastHoverText().find("`y` is not defined") != std::string::npos;
    });
    MCGuiStep("the caret moved away with a key", [] { MCGuiKey("ctrl+Home"); });
    MCGuiWait("the box closes", 10, [] { return caretOffset() == 0 && !lsp()->hoverVisible(); });

    // Closing a tab.
    MCGuiStep("second.py's tab closed with Ctrl+W", [] {
        showTab("second.py");
    });
    MCGuiWait("second.py with the keyboard", 10, [] { return onShow("second.py") && editorHasKeyboard(); });
    MCGuiStep("Ctrl+W", [] { MCGuiKey("ctrl+w"); });
    MCGuiWait("its heading gone from the list", 10, [] {
        return !ed()->documentFor(F("second.py")) && rowOf("second.py") < 0 &&
               rowOf("first.py") >= 0;
    });

    // Ctrl+W in the list closes the list.
    MCGuiStep("Ctrl+Shift+M, then Ctrl+W", [] {
        MCGuiFocusKeyWindow();
        MCGuiKey("ctrl+shift+m");
    });
    MCGuiWait("the list has the keyboard", 10, listHasKeyboard);
    MCGuiStep("Ctrl+W", [] { MCGuiKey("ctrl+w"); });
    MCGuiWait("the list closed, the tab still open", 10, [] {
        return !panel()->shown() && !MCGuiApp().layout->problemsShown() &&
               ed()->documentFor(F("first.py")) && ed()->active() != nullptr;
    });

    // The status bar's counts.
    MCGuiStep("a click on the status bar's counts", [] { MCGuiClick(panel()->statusLabel()); });
    MCGuiWait("the list shows", 10, [] { return panel()->shown() && MCGuiOnScreen(panel()->widget()); });
    MCGuiStep("another", [] { MCGuiClick(panel()->statusLabel()); });
    MCGuiWait("it closes", 10, [] { return !panel()->shown(); });

    MCGuiStep("the server off again; the tabs closed unasked", [] {
        MCGuiSetSettings(gSettings);
        ed()->closeAllTabs();
    });
    MCGuiWait("no server left, nothing counted", 10, [] {
        return LspRunningServerCount() == 0 && !gtk_widget_get_visible(panel()->statusLabel());
    });

    // ---------------------------------------------- JSON, which no server has
    // The editor checks it itself, as VS Code does (#213): a trailing comma
    // is one error, drawn on the comma, listed and counted; taken out, it
    // goes, and typed back it returns; JSON with comments (tsconfig.json)
    // may have comments and trailing commas; its tab closed, it goes.
    MCGuiStep("package.json with a trailing comma, and no server", [] {
        MCGuiWriteFile(F("package.json"), kPackage);
        MCGuiWriteFile(F("tsconfig.json"), "// the compiler's options\n{\n  /* target */ "
                                           "\"target\": \"es2022\",\n  \"lib\": [\"dom\",],\n}\n");
        showTab("package.json");
    });
    MCGuiWait("one problem, and the status bar says so", 10, [] {
        return onShow("package.json") && lsp()->problemErrors() == 1 &&
               panel()->statusText() == "1 error" && gtk_widget_get_visible(panel()->statusLabel());
    });
    MCGuiStep("Ctrl+Shift+M", [] { MCGuiKey("ctrl+shift+m"); });
    MCGuiWait("the list shows it", 10, [] {
        return panel()->shown() && listHasKeyboard() &&
               rowOf("E 3:21 Trailing comma [json]") >= 0;
    });
    MCGuiStep("drawn on the comma, listed at its line under its file; Escape", [] {
        MCGuiNote("rows: %s", rowsJoined().c_str());
        const int comma = offsetOf(2, 20);   // after "1.0.0"
        GCHECK(MCGuiEditorText().substr((size_t)comma, 1) == ",");
        GCHECK(errorTagAt(comma) && !errorTagAt(comma - 1) && !errorTagAt(comma + 1));
        GCHECK_EQ_STR(rowsJoined(), "# package.json \xE2\x80\x94 1 error | E 3:21 Trailing comma [json]");
        MCGuiKey("escape");
    });
    MCGuiWait("the editor has the keyboard, the list still showing", 10, [] {
        return editorHasKeyboard() && panel()->shown();
    });
    MCGuiStep("the comma taken out with Delete", [] {
        caretAt(offsetOf(2, 20));
        MCGuiKey("Delete");
    });
    MCGuiWait("the comma taken out: no problems, and the status bar says so", 10, [] {
        return MCGuiEditorText().find("\"1.0.0\"\n}") != std::string::npos &&
               lsp()->problemErrors() == 0 && rowOf("package.json") < 0 &&
               panel()->statusText() == "No problems" && !errorTagAt(offsetOf(2, 19));
    });
    MCGuiStep("typed back", [] { MCGuiType(","); });
    MCGuiWait("the problem is back", 10, [] {
        return MCGuiEditorText() == kPackage && lsp()->problemErrors() == 1 &&
               errorTagAt(offsetOf(2, 20)) && rowOf("E 3:21 Trailing comma [json]") >= 0;
    });
    MCGuiStep("tsconfig.json: JSON with comments", [] { showTab("tsconfig.json"); });
    MCGuiWait("on show; package.json's error still counted", 10, [] {
        return onShow("tsconfig.json") && panel()->statusText() == "1 error";
    });
    // A negative check: tsconfig.json is checked at once, on a thread; a
    // second is long enough for it to have answered.
    MCGuiPause(1.0);
    MCGuiStep("none for tsconfig.json; package.json's listed while its tab is open", [] {
        MCGuiNote("rows: %s", rowsJoined().c_str());
        GCHECK(rowOf("tsconfig.json") < 0);
        GCHECK(rowOf("# package.json") >= 0);
        GCHECK(!errorTagAt(offsetOf(3, 15)));   // the comma before ]
    });
    MCGuiStep("package.json's tab closed with Ctrl+W", [] { showTab("package.json"); });
    MCGuiWait("it with the keyboard", 10, [] { return onShow("package.json") && editorHasKeyboard(); });
    MCGuiStep("Ctrl+W", [] { MCGuiKey("ctrl+w"); });
    MCGuiWait("its problem gone from the list", 10, [] {
        return !ed()->documentFor(F("package.json")) && rowOf("package.json") < 0 &&
               lsp()->problemErrors() == 0;
    });
    MCGuiStep("the tabs closed unasked", [] { ed()->closeAllTabs(); });
    MCGuiWait("nothing counted", 10, [] { return !gtk_widget_get_visible(panel()->statusLabel()); });
    MCGuiStep("the list given the keyboard, then Ctrl+W", [] {
        MCGuiFocusKeyWindow();
        MCGuiKey("ctrl+shift+m");
    });
    MCGuiWait("the list has the keyboard", 10, listHasKeyboard);
    MCGuiStep("Ctrl+W", [] { MCGuiKey("ctrl+w"); });
    MCGuiWait("the list closed", 10, [] { return !panel()->shown(); });
}
