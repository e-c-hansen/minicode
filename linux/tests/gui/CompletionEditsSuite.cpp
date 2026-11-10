// CompletionEditsSuite.cpp — `./build/minicode-guitest completion_edits`:
// what goes in with a completion besides its word (linux/PARITY.md item
// 2.5), the counterpart of the Mac's lsp_fixes (additionalTextEdits) and
// lsp_sync (the import that comes later) checks, with real keys, against
// the scripted server (build/fakelsp).
//
//   - An item with additionalTextEdits (an import at the top) goes in with
//     its word as one undo step: one Ctrl+Z takes both back, Ctrl+Shift+Z
//     brings both.
//   - A word put in where nothing is typed over, right after typing (the
//     list after "."), is an undo step of its own: one Ctrl+Z leaves what
//     was typed (GtkTextHistory joins a lone insertion to the typing
//     before it).
//   - The server offers Path without its import and sends the import only
//     from completionItem/resolve, after half a second. Tab puts the word in
//     at once, and the import follows when the answer comes: in the same
//     undo step when nothing was typed meanwhile (one Ctrl+Z, and
//     Ctrl+Shift+Z brings both), as a step of its own when typing came
//     between, not at all when the accept was undone before the answer,
//     left out with a note when the text it would change was edited first,
//     and with a note when the server does not answer within a second. The
//     file has an emoji before the word (two UTF-16 units, one character),
//     and the server's copy of it is the editor's after it all.
#include "CompletionChecks.h"

#include <string>

using namespace cmpcheck;

namespace {

const char* kServer = "edits";
std::string F(const std::string& name) { return MCGuiProjectFile("cmpedits/" + name); }
const std::string kImports = "import sys\n\nsys.pa\n";
std::string gSettings;

// The late file: the import goes on the second line, after "import os".
std::string lateText() {
    std::string s = "import os\n\n\xF0\x9F\x98\x80 = 1\np = Pa\n";
    for (int k = 0; k < 20; k++) s += "# line " + std::to_string(k) + " of padding, so ranges pay\n";
    return s;
}
const std::string kImport = "from pathlib import Path\n";

std::string script(int resolveDelayMs) {
    const std::string sysItems = joined({
        item("path", 6, "a", ", \"additionalTextEdits\": [" + edit(0, 0, 0, 0, "import os\n") + "]"),
        // An empty list: nothing to ask resolve for.
        item("argv", 6, "b", ", \"additionalTextEdits\": []")});
    const std::string resolved =
        "{\"label\": \"Path\", \"kind\": 7, \"insertText\": \"Path\", \"additionalTextEdits\": [" +
        edit(1, 0, 1, 0, kImport) + "]}";
    return "{\"capabilities\": {\"textDocumentSync\": {\"openClose\": true, \"change\": 2,"
           " \"save\": {\"includeText\": false}},"
           " \"completionProvider\": {\"triggerCharacters\": [\".\"], \"resolveProvider\": true}},"
           " \"rules\": ["
           "{\"method\": \"textDocument/completion\", \"when\": {\"uriEndsWith\": \"imports.py\"},"
           " \"result\": {\"isIncomplete\": false, \"items\": [" + sysItems + "]}},"
           "{\"method\": \"textDocument/completion\", \"result\": {\"isIncomplete\": false, \"items\": [" +
           item("Path", 7, "a", ", \"insertText\": \"Path\", \"data\": {\"id\": 1}") + "]}},"
           "{\"method\": \"completionItem/resolve\", \"delayMs\": " + std::to_string(resolveDelayMs) +
           ", \"result\": " + resolved + "}]}";
}

std::string gLate;   // lateText(), once

bool hasImport() { return text().find("pathlib") != std::string::npos; }
bool imported() { return text().rfind("import os\n" + kImport, 0) == 0; }

void askAtPa() {
    caretAfter("p = Pa");
    MCGuiKey("ctrl+space");
}

}  // namespace

MC_GUI_SUITE(completion_edits) {
    MCGuiStep("two Python files and a server that completes and resolves", [] {
        MCGuiSetKeyWindow(nullptr);
        ed()->closeAllTabs();
        gLate = lateText();
        MCGuiWriteFile(F("imports.py"), kImports);
        MCGuiWriteFile(F("late.py"), gLate);
        gSettings = MCGuiReadFile(g_getenv("MINICODE_SETTINGS"));
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + MCGuiFakeLsp(kServer, script(500)) + "\n");
        showFile(F("imports.py"));
    });
    MCGuiWait("the server has imports.py", 20, [] {
        return showing(F("imports.py")) &&
               MCGuiFakeLspReceived(kServer, "textDocument/didOpen", "imports.py") > 0;
    });

    // ---- additionalTextEdits with the word
    MCGuiStep("ask at sys.pa", [] {
        caretAfter("sys.pa");
        MCGuiKey("ctrl+space");
    });
    MCGuiWait("the list", 10, [] { return shows("path"); });
    MCGuiStep("Tab", [] { MCGuiKey("tab"); });
    MCGuiWait("path and its import in", 10, [] { return text() == "import os\nimport sys\n\nsys.path\n"; });
    MCGuiStep("the caret after path", [] {
        GCHECK(caret() == text().find("sys.path") + 8);
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("one Ctrl+Z takes both back", 10, [] { return text() == kImports; });
    MCGuiStep("Ctrl+Shift+Z", [] { MCGuiKey("ctrl+shift+z"); });
    MCGuiWait("both again", 10, [] { return text() == "import os\nimport sys\n\nsys.path\n"; });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("as it was", 10, [] { return text() == kImports; });

    // ---- a word that only inserts, right after typing
    MCGuiStep("x = sys. typed on a line of its own", [] {
        caretAfter("sys.pa\n");
        MCGuiType("x = sys.");
    });
    MCGuiWait("the list after the dot", 10, [] { return shows("path"); });
    MCGuiStep("Down to argv, Tab", [] {
        MCGuiKey("down");
        MCGuiKey("tab");
    });
    MCGuiWait("argv in", 10, [] { return text() == kImports + "x = sys.argv"; });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("one Ctrl+Z takes the word back and leaves what was typed", 10, [] {
        return text() != kImports + "x = sys.argv";
    });
    MCGuiStep("x = sys. still there", [] { GCHECK_EQ_STR(text(), kImports + "x = sys."); });

    // ---- the import that comes later, nothing typed meanwhile
    MCGuiStep("late.py", [] { showFile(F("late.py")); });
    MCGuiWait("the server has it", 20, [] {
        return showing(F("late.py")) && MCGuiFakeLspReceived(kServer, "textDocument/didOpen", "late.py") > 0;
    });
    MCGuiStep("ask at Pa", askAtPa);
    MCGuiWait("the list", 10, [] { return shows("Path"); });
    MCGuiStep("Tab", [] { MCGuiKey("tab"); });
    MCGuiWait("the word goes in", 10, [] { return text().find("p = Path\n") != std::string::npos; });
    MCGuiStep("at once, before the server's import", [] {
        GCHECK(!hasImport());
        GCHECK(lsp()->pendingAccepts() == 1);
    });
    MCGuiWait("the import follows", 10, imported);
    MCGuiStep("the caret stayed after the word", [] {
        GCHECK(caret() == chars(text(), text().find("p = Path") + 8));
        GCHECK(lsp()->pendingAccepts() == 0);
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("one Ctrl+Z takes the word and its import back", 10, [] { return text() == gLate; });
    MCGuiStep("Ctrl+Shift+Z", [] { MCGuiKey("ctrl+shift+z"); });
    MCGuiWait("redo brings both", 10, [] {
        return imported() && text().find("p = Path\n") != std::string::npos;
    });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("as it was", 10, [] { return text() == gLate; });

    // ---- taken back before the answer
    MCGuiStep("again, taken back at once", askAtPa);
    MCGuiWait("the list", 10, [] { return shows("Path"); });
    MCGuiStep("Tab, then Ctrl+Z", [] {
        MCGuiKey("tab");
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("taken back", 10, [] { return text() == gLate && lsp()->pendingAccepts() == 0; });
    MCGuiPause(0.8);   // negative: the late answer must add nothing
    MCGuiStep("the answer adds nothing", [] { GCHECK_EQ_STR(text(), gLate); });

    // ---- typing right after the word
    MCGuiStep("again, typing right after the word", askAtPa);
    MCGuiWait("the list", 10, [] { return shows("Path"); });
    MCGuiStep("Tab, then ()", [] {
        MCGuiKey("tab");
        MCGuiType("()");
    });
    MCGuiWait("the import still follows", 10, imported);
    MCGuiStep("as its own step: Ctrl+Z takes the import", [] {
        GCHECK(text().find("p = Path()\n") != std::string::npos);
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("the import gone, () kept", 10, [] {
        return !hasImport() && text().find("p = Path()\n") != std::string::npos;
    });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("then ()", 10, [] { return text().find("p = Path\n") != std::string::npos; });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("then the word", 10, [] { return text() == gLate; });

    // ---- the top edited before the answer
    MCGuiStep("again, editing the top before the server answers", askAtPa);
    MCGuiWait("the list", 10, [] { return shows("Path"); });
    MCGuiStep("Tab, Ctrl+Home, #", [] {
        MCGuiKey("tab");
        MCGuiKey("ctrl+Home");
        MCGuiType("#");
    });
    MCGuiWait("the import is left out, and the status bar says so", 10, [] {
        return lsp()->lastNote().find("left out") != std::string::npos;
    });
    MCGuiStep("the word and the edit, no import", [] {
        GCHECK(text().rfind("#import os", 0) == 0);
        GCHECK(!hasImport());
        GCHECK(text().find("p = Path\n") != std::string::npos);
        MCGuiKey("ctrl+z");
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("as it was", 10, [] { return text() == gLate; });

    // ---- a server slower than a second
    MCGuiStep("a slow server: ask once more", [] {
        // The server reads its script again on its next message.
        MCGuiFakeLsp(kServer, script(1600));
        askAtPa();
    });
    MCGuiWait("the list", 10, [] { return shows("Path"); });
    MCGuiStep("Tab", [] { MCGuiKey("tab"); });
    MCGuiWait("after a second, the status bar says the import may be missing", 10, [] {
        return lsp()->lastNote().find("within a second") != std::string::npos;
    });
    MCGuiPause(1.0);   // negative: the late answer must add nothing
    MCGuiStep("and the late answer adds nothing", [] {
        GCHECK(!hasImport());
        GCHECK(text().find("p = Path\n") != std::string::npos);
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("as it was", 10, [] { return text() == gLate; });
    MCGuiWait("the server's copy is the editor's text after it all", 10, [] {
        std::string copy;
        return MCGuiFakeLspText(kServer, "late.py", &copy) && copy == text();
    });

    MCGuiStep("the servers off again; the tabs closed unasked", [] {
        MCGuiSetSettings(gSettings);
        ed()->closeAllTabs();
    });
    MCGuiWait("no server left", 10, [] { return LspRunningServerCount() == 0; });
}
