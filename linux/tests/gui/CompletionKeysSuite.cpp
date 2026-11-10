// CompletionKeysSuite.cpp — `./build/minicode-guitest completion_keys`: Tab
// and Return take the completion list's selected item whenever the list
// shows, as VS Code's do (editor.acceptSuggestionOnEnter "on", Tab
// accepting), linux/PARITY.md item 2.5, the counterpart of the Mac's
// tests/gui/CompletionKeysSuite.mm, with every key a real one (XTest).
//
// Against the scripted server (build/fakelsp), whose list is incomplete
// (asked again at every letter, as basedpyright's): the list coming up by
// itself on a word's second letter; Return on it (no new line, one Ctrl+Z
// takes the word back); Tab (no indentation); a word typed out in full
// that some item is (Return closes the list, a second Return is the new
// line); the arrows then Return; a letter and Tab pressed together, before
// the list was filtered for the letter (the item for the word as typed);
// Shift+Return and Escape (a new line, the list closed); inside
// auto-closed brackets; and at a snippet's tab stop, where the list keeps
// Tab and Return and the stop has them once it is closed.
#include "CompletionChecks.h"

#include <string>

using namespace cmpcheck;

namespace {

const char* kServer = "keys";
std::string F() { return MCGuiProjectFile("cmpkeys/main.py"); }
const std::string kText = "import os\n\ndef main():\n    x = 1\n    \n";
std::string gSettings;

std::string script() {
    const std::string items = joined({
        item("print", 3, "a"), item("property", 7, "b"), item("process_time", 3, "c"),
        item("path", 6, "d"), item("pathsep", 6, "e"), item("PathLike", 7, "f"),
        item("call", 15, "g", ", \"insertTextFormat\": 2, \"insertText\": \"call(${1:arg})$0\"")});
    return "{\"rules\": [{\"method\": \"textDocument/completion\", \"result\": "
           "{\"isIncomplete\": true, \"items\": [" + items + "]}}]}";
}

// The file as it starts, the caret on the indented empty line.
void fresh() { setText(kText, (long)kText.size() - 1); }

}  // namespace

MC_GUI_SUITE(completion_keys) {
    MCGuiStep("a Python file and a server that completes", [] {
        MCGuiSetKeyWindow(nullptr);
        ed()->closeAllTabs();
        MCGuiWriteFile(F(), kText);
        gSettings = MCGuiReadFile(g_getenv("MINICODE_SETTINGS"));
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + MCGuiFakeLsp(kServer, script()) + "\n");
        showFile(F());
    });
    MCGuiWait("the server has the file, and the editor the keyboard", 20, [] {
        return showing(F()) && MCGuiFakeLspReceived(kServer, "textDocument/didOpen", "main.py") > 0;
    });

    // ---- Return, on a list that came up by itself
    MCGuiStep("pr typed", [] {
        fresh();
        MCGuiType("pr");
    });
    MCGuiWait("the list came up by itself, print first", 10, [] { return shows("print"); });
    MCGuiStep("Return", [] { MCGuiKey("enter"); });
    MCGuiWait("print taken, no new line", 10, [] { return !lsp()->completionVisible(); });
    MCGuiStep("the word in, the list closed", [] {
        GCHECK_EQ_STR(lineAtCaret(), "    print");
        GCHECK_EQ_STR(text(), "import os\n\ndef main():\n    x = 1\n    print\n");
    });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("one Ctrl+Z takes it back to what was typed", 10, [] { return lineAtCaret() == "    pr"; });

    // ---- Tab
    MCGuiStep("pr typed again", [] {
        fresh();
        MCGuiType("pr");
    });
    MCGuiWait("the list", 10, [] { return shows("print"); });
    MCGuiStep("Tab", [] { MCGuiKey("tab"); });
    MCGuiWait("print taken", 10, [] { return !lsp()->completionVisible(); });
    MCGuiStep("no indentation", [] { GCHECK_EQ_STR(lineAtCaret(), "    print"); });

    // ---- a word typed out that is an item
    MCGuiStep("path typed out", [] {
        fresh();
        MCGuiType("path");
    });
    MCGuiWait("the list, path first", 10, [] { return shows("path"); });
    MCGuiStep("Return", [] { MCGuiKey("enter"); });
    MCGuiWait("the list closed", 10, [] { return !lsp()->completionVisible(); });
    MCGuiStep("the text as it was, the caret after it", [] {
        GCHECK_EQ_STR(lineAtCaret(), "    path");
        GCHECK(caret() == text().find("    path") + 8);
        MCGuiKey("enter");
    });
    MCGuiWait("a second Return is the new line, indented", 10, [] {
        return text() == "import os\n\ndef main():\n    x = 1\n    path\n    \n";
    });

    // ---- the arrows, then Return
    MCGuiStep("pa typed", [] {
        fresh();
        MCGuiType("pa");
    });
    MCGuiWait("the list", 10, [] { return shows("path"); });
    MCGuiStep("Down", [] { MCGuiKey("down"); });
    MCGuiWait("pathsep highlighted", 10, [] { return shows("pathsep"); });
    MCGuiStep("Return", [] { MCGuiKey("enter"); });
    MCGuiWait("pathsep taken", 10, [] { return lineAtCaret() == "    pathsep"; });

    // ---- a letter and Tab pressed before the list caught up
    MCGuiStep("pr typed once more", [] {
        fresh();
        MCGuiType("pr");
    });
    MCGuiWait("the list", 10, [] { return shows("print"); });
    MCGuiStep("o and Tab in one go", [] {
        // Both on their way before the main loop turns, as when a quick
        // typist's keys wait in the queue together.
        MCGuiType("o");
        MCGuiKey("tab");
    });
    MCGuiWait("the item for pro, not print", 10, [] { return !lsp()->completionVisible(); });
    MCGuiStep("property", [] { GCHECK_EQ_STR(lineAtCaret(), "    property"); });
    MCGuiStep("and Return the same way", [] {
        fresh();
        MCGuiType("pr");
    });
    MCGuiWait("the list", 10, [] { return shows("print"); });
    MCGuiStep("o, c and Return in one go", [] {
        MCGuiType("oc");
        MCGuiKey("enter");
    });
    MCGuiWait("process_time", 10, [] { return lineAtCaret() == "    process_time"; });

    // ---- Shift+Return and Escape
    MCGuiStep("pr typed for Shift+Return", [] {
        fresh();
        MCGuiType("pr");
    });
    MCGuiWait("the list", 10, [] { return shows("print"); });
    MCGuiStep("Shift+Return", [] { MCGuiKey("shift+enter"); });
    MCGuiWait("a new line, indented, and the list closed", 10, [] {
        const std::string t = text();
        return !lsp()->completionVisible() && t.size() >= 12 &&
               t.compare(t.size() - 12, 12, "    pr\n    \n") == 0;
    });
    MCGuiStep("pr typed for Escape", [] {
        fresh();
        MCGuiType("pr");
    });
    MCGuiWait("the list", 10, [] { return shows("print"); });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("the list closed", 10, [] { return !lsp()->completionVisible(); });
    MCGuiStep("the text left as typed", [] { GCHECK_EQ_STR(lineAtCaret(), "    pr"); });

    // ---- inside auto-closed brackets
    MCGuiStep("( then pr", [] {
        fresh();
        MCGuiType("(pr");
    });
    MCGuiWait("the list", 10, [] { return shows("print"); });
    MCGuiStep("Return", [] { MCGuiKey("enter"); });
    MCGuiWait("print inside the pair", 10, [] { return lineAtCaret() == "    (print)"; });
    MCGuiStep("the caret before the closer", [] { GCHECK(caret() == text().rfind(')')); });

    // ---- at a snippet's stop
    MCGuiStep("ca typed", [] {
        fresh();
        MCGuiType("ca");
    });
    MCGuiWait("the list", 10, [] { return shows("call"); });
    MCGuiStep("Tab", [] { MCGuiKey("tab"); });
    MCGuiWait("the snippet in, arg selected", 10, [] { return lineAtCaret() == "    call(arg)"; });
    MCGuiStep("a stop, then pr typed over arg", [] {
        GCHECK(snippets()->active());
        GCHECK_EQ_STR(selected(), "arg");
        MCGuiType("pr");
    });
    MCGuiWait("the list at the stop", 10, [] { return shows("print"); });
    MCGuiStep("Return", [] { MCGuiKey("enter"); });
    MCGuiWait("print taken there", 10, [] { return lineAtCaret() == "    call(print)"; });
    MCGuiStep("the stop still there; Tab leaves it", [] {
        GCHECK(snippets()->active());
        MCGuiKey("tab");
    });
    MCGuiWait("the snippet over, the caret after it", 10, [] { return !snippets()->active(); });
    MCGuiStep("after the closer", [] {
        GCHECK(caret() == text().rfind(')') + 1);
        GCHECK_EQ_STR(lineAtCaret(), "    call(print)");
    });
    MCGuiStep("ca once more", [] {
        fresh();
        MCGuiType("ca");
    });
    MCGuiWait("the list", 10, [] { return shows("call"); });
    MCGuiStep("Tab", [] { MCGuiKey("tab"); });
    MCGuiWait("the snippet", 10, [] { return snippets()->active(); });
    MCGuiStep("pr at the stop", [] { MCGuiType("pr"); });
    MCGuiWait("the list at the stop", 10, [] { return shows("print"); });
    MCGuiStep("Tab", [] { MCGuiKey("tab"); });
    MCGuiWait("Tab takes print, not the next stop", 10, [] { return !lsp()->completionVisible(); });
    MCGuiStep("print, the stop kept", [] {
        GCHECK_EQ_STR(lineAtCaret(), "    call(print)");
        GCHECK(snippets()->active());
        MCGuiKey("escape");
    });
    MCGuiWait("Escape ends the snippet", 10, [] { return !snippets()->active(); });

    MCGuiStep("the servers off again; the tab closed unasked", [] {
        MCGuiSetSettings(gSettings);
        ed()->closeAllTabs();
    });
    MCGuiWait("no server left", 10, [] { return LspRunningServerCount() == 0; });
}
