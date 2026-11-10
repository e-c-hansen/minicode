// SecondServerSuite.cpp — `./build/minicode-guitest second_server`: Ruff
// beside the Python server (linux/PARITY.md item 2.2; the Mac's in
// Lsp+Servers.mm, checked there by tests/gui/LspFixesSuite.mm and
// OrganizeOnSaveSuite.mm), against two scripted servers (build/fakelsp):
// the main one as lsp.python, and a stand-in `ruff` on PATH that the
// built-in rule runs as "ruff server" in a project that configures Ruff.
//
//   - A file below a ruff.toml gets both servers, the status bar naming
//     both ("fakelsp + ruff: 1 error, 1 warning"); a file outside it only
//     the main one, and Ruff is never given it.
//   - Both servers' diagnostics stand together: both squiggles drawn, both
//     in the Problems list, each saying which server it is from ("(Ruff
//     F401)" in the box at the caret).
//   - Hover (Ctrl+Alt+I) goes to the main server only, though Ruff offers
//     it too.
//   - Ctrl+. asks both servers, each with its own diagnostics there, and
//     lists both servers' fixes in one list, each naming its server; Enter
//     on Ruff's runs it through Ruff's edit.
#include "Harness.h"

#include "Editor.h"
#include "FakeLspLog.h"
#include "Lsp.h"
#include "ProblemsPanel.h"

#include <sys/stat.h>

#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
LspSession* lsp() { return MCGuiApp().lsp; }
std::string F(const std::string& name) { return MCGuiProjectFile("also/" + name); }
std::string bin() { return MCGuiScratch() + "/alsobin"; }
const char* kMain = "alsomain";
const char* kRuff = "alsoruff";

const std::string kA = "import os\nimport sys\n\nprint(sys.argv, undefined)\n";
const std::string kB = "import os\nprint(1)\n";

std::string str(const std::string& s) { return Json(s).dump(); }
std::string range(int l0, int c0, int l1, int c1) {
    return "{\"start\": {\"line\": " + std::to_string(l0) + ", \"character\": " +
           std::to_string(c0) + "}, \"end\": {\"line\": " + std::to_string(l1) +
           ", \"character\": " + std::to_string(c1) + "}}";
}
std::string edit(int l0, int c0, int l1, int c1, const std::string& text) {
    return "{\"range\": " + range(l0, c0, l1, c1) + ", \"newText\": " + str(text) + "}";
}

// The main server: an error on line 3, hover, and a fix of its own.
std::string mainScript() {
    return "{\"name\": \"fakelsp\", \"capabilities\": {\"textDocumentSync\": 1,"
           " \"hoverProvider\": true, \"codeActionProvider\": true}, \"rules\": ["
           "{\"method\": \"textDocument/didOpen\", \"when\": {\"uriEndsWith\": \"/a.py\"},"
           " \"notify\": [{\"method\": \"textDocument/publishDiagnostics\", \"params\": {"
           "\"uri\": \"${uri}\", \"version\": \"${version}\", \"diagnostics\": [{\"range\": " +
           range(3, 16, 3, 25) + ", \"severity\": 1, \"message\": \"\\\"undefined\\\" is not defined\","
           " \"source\": \"basedpyright\", \"code\": \"reportUndefinedVariable\"}]}}]},"
           "{\"method\": \"textDocument/hover\", \"result\": {\"contents\": \"(module) os\"}},"
           "{\"method\": \"textDocument/codeAction\", \"result\": [{\"title\": \"Ignore this line\","
           " \"kind\": \"quickfix\", \"edit\": {\"changes\": {\"${uri}\": [" +
           edit(0, 9, 0, 9, "  # type: ignore") + "]}}}]}]}";
}

// Ruff: an unused import, its fix and Fix all, and hover it is never asked.
std::string ruffScript() {
    return "{\"name\": \"ruff\", \"capabilities\": {\"textDocumentSync\": 1,"
           " \"hoverProvider\": true, \"codeActionProvider\": {\"codeActionKinds\": [\"quickfix\","
           " \"source.fixAll.ruff\"]}}, \"rules\": ["
           "{\"method\": \"textDocument/didOpen\", \"when\": {\"uriEndsWith\": \"/a.py\"},"
           " \"notify\": [{\"method\": \"textDocument/publishDiagnostics\", \"params\": {"
           "\"uri\": \"${uri}\", \"version\": \"${version}\", \"diagnostics\": [{\"range\": " +
           range(0, 7, 0, 9) + ", \"severity\": 2, \"message\": \"`os` imported but unused\","
           " \"source\": \"Ruff\", \"code\": \"F401\"}]}}]},"
           "{\"method\": \"textDocument/hover\", \"result\": {\"contents\": \"ruff hover\"}},"
           "{\"method\": \"textDocument/codeAction\", \"when\": {\"paramsContain\": \"F401\"},"
           " \"result\": [{\"title\": \"Remove unused import: `os`\", \"kind\": \"quickfix\","
           " \"edit\": {\"changes\": {\"${uri}\": [" + edit(0, 0, 1, 0, "") + "]}}},"
           " {\"title\": \"Ruff: Fix all auto-fixable problems\", \"kind\": \"source.fixAll.ruff\","
           " \"edit\": {\"changes\": {\"${uri}\": [" + edit(0, 0, 1, 0, "") + "]}}}]},"
           "{\"method\": \"textDocument/codeAction\", \"result\": []}]}";
}

bool onShow(const std::string& name) { return ed()->currentPath() == F(name) && ed()->isSource(); }

void showTab(const std::string& name) {
    MCGuiOpenFileKept(F(name));
    MCGuiFocusKeyWindow();
    ed()->focusActiveDocument();
}

bool editorHasKeyboard() {
    return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)) == ed()->textView();
}

void caretAt(int line, int column) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line_offset(ed()->buffer(), &it, line, column);
    gtk_text_buffer_place_cursor(ed()->buffer(), &it);
}

std::string textOf(const std::string& name) {
    Document* d = ed()->documentFor(F(name));
    return d ? d->text() : "<no tab>";
}

std::string names() {
    std::string s;
    for (const std::string& n : lsp()->fileServerNames()) s += (s.empty() ? "" : " ") + n;
    return s;
}

std::string rowsJoined(const std::vector<std::string>& rows) {
    std::string s;
    for (const std::string& r : rows) s += (s.empty() ? "" : " | ") + r;
    return s;
}

// Whether a codeAction request to `server` carried `part` in its context.
int codeActionsWith(const std::string& server, const std::string& part) {
    int n = 0;
    for (const Json& m : MCGuiFakeLspMessages(server))
        if (m["method"].asString() == "textDocument/codeAction" &&
            m["params"]["context"].dump().find(part) != std::string::npos)
            n++;
    return n;
}

std::string gSettings, gPath;

}  // namespace

MC_GUI_SUITE(second_server) {
    MCGuiStep("a project configuring Ruff in withruff/, and a stand-in ruff on PATH", [] {
        MCGuiSetKeyWindow(nullptr);
        ed()->closeAllTabs();
        MCGuiWriteFile(F("withruff/ruff.toml"), "[lint]\nselect = [\"F\"]\n");
        MCGuiWriteFile(F("withruff/a.py"), kA);
        MCGuiWriteFile(F("plain/b.py"), kB);
        // `ruff server`, as the built-in rule runs it: the scripted server
        // under the name ruff, so the status bar says ruff.
        const std::string ruff = bin() + "/ruff";
        MCGuiWriteFile(ruff, "#!/bin/sh\nexec " + MCGuiFakeLsp(kRuff, ruffScript()) + " \"$@\"\n");
        chmod(ruff.c_str(), 0755);
        const char* p = g_getenv("PATH");
        gPath = p ? p : "";
        g_setenv("PATH", (bin() + ":" + gPath).c_str(), TRUE);
        gSettings = MCGuiReadFile(g_getenv("MINICODE_SETTINGS"));
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + MCGuiFakeLsp(kMain, mainScript()) +
                         "\n");
        showTab("withruff/a.py");
    });
    MCGuiWait("both servers run for a.py, and both published", 20, [] {
        return onShow("withruff/a.py") && names() == "fakelsp ruff" &&
               lsp()->statusText() == "fakelsp + ruff: 1 error, 1 warning";
    });
    MCGuiStep("each was given a.py; the Problems list has both, each tagged", [] {
        GCHECK(MCGuiFakeLspReceived(kMain, "textDocument/didOpen", "/a.py") == 1);
        GCHECK(MCGuiFakeLspReceived(kRuff, "textDocument/didOpen", "/a.py") == 1);
        const std::vector<LspProblem> rows = lsp()->problemRows();
        GCHECK(rows.size() == 3);
        if (rows.size() == 3) {
            GCHECK(rows[0].heading && rows[0].errors == 1 && rows[0].warnings == 1);
            GCHECK(rows[1].source == "basedpyright" && rows[1].severity == 1);
            GCHECK(rows[2].source == "Ruff" && rows[2].code == "F401" && rows[2].severity == 2);
        }
        caretAt(0, 7);
        MCGuiKey("ctrl+alt+i");
    });
    MCGuiWait("the box at the caret: Ruff's problem, tagged, and the main server's hover", 10, [] {
        return lsp()->hoverVisible() &&
               lsp()->lastHoverText().find("Warning: `os` imported but unused (Ruff F401)") !=
                   std::string::npos &&
               lsp()->lastHoverText().find("(module) os") != std::string::npos;
    });
    MCGuiStep("hover went to the main server only", [] {
        MCGuiNote("box: %s", lsp()->lastHoverText().c_str());
        GCHECK(MCGuiFakeLspReceived(kMain, "textDocument/hover") >= 1);
        GCHECK(MCGuiFakeLspReceived(kRuff, "textDocument/hover") == 0);
        GCHECK(lsp()->lastHoverText().find("ruff hover") == std::string::npos);
        MCGuiKey("escape");
    });
    MCGuiWait("the box closes, the editor has the keyboard", 10, [] {
        return !lsp()->hoverVisible() && editorHasKeyboard();
    });

    // Code actions from both.
    MCGuiStep("Ctrl+. on the import", [] {
        caretAt(0, 8);
        MCGuiKey("ctrl+.");
    });
    MCGuiWait("one list of both servers' fixes", 10, [] { return lsp()->pickerKind() == "fixes"; });
    MCGuiStep("each names its server; each server had its own diagnostics as context", [] {
        const std::vector<std::string> rows = lsp()->pickerRows();
        MCGuiNote("fixes: %s", rowsJoined(rows).c_str());
        GCHECK(rows.size() == 3);
        if (rows.size() == 3) {
            GCHECK(rows[0].find("Ignore this line") != std::string::npos &&
                   rows[0].find("fakelsp") != std::string::npos);
            GCHECK(rows[1].find("Remove unused import: `os`") != std::string::npos &&
                   rows[1].find("fix, ruff") != std::string::npos);
            GCHECK(rows[2].find("Fix all") != std::string::npos &&
                   rows[2].find("ruff") != std::string::npos);
        }
        GCHECK(codeActionsWith(kRuff, "F401") == 1);
        GCHECK(codeActionsWith(kMain, "F401") == 0);
        MCGuiKey("down");
    });
    MCGuiWait("Ruff's fix selected", 5, [] { return lsp()->pickerSelectedRow() == 1; });
    MCGuiStep("Enter", [] { MCGuiKey("enter"); });
    MCGuiWait("the import is gone, by Ruff's edit", 10, [] {
        return textOf("withruff/a.py") == kA.substr(10) && !lsp()->pickerVisible();
    });

    // A file outside the Ruff project.
    MCGuiStep("plain/b.py", [] { showTab("plain/b.py"); });
    MCGuiWait("only the main server has it", 10, [] {
        return onShow("plain/b.py") && names() == "fakelsp" &&
               MCGuiFakeLspReceived(kMain, "textDocument/didOpen", "/b.py") == 1;
    });
    MCGuiStep("Ruff was never given it; the status bar names the main server alone", [] {
        GCHECK(MCGuiFakeLspReceived(kRuff, "textDocument/didOpen", "/b.py") == 0);
        GCHECK(lsp()->statusText().rfind("fakelsp: ", 0) == 0);
    });

    MCGuiStep("PATH and the settings as they were; the tabs closed unasked", [] {
        g_setenv("PATH", gPath.c_str(), TRUE);
        MCGuiSetSettings(gSettings);
        ed()->closeAllTabs();
    });
    MCGuiWait("no server left", 10, [] { return LspRunningServerCount() == 0; });
}
