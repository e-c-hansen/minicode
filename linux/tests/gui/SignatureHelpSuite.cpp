// SignatureHelpSuite.cpp — `./build/minicode-guitest signature_help`: the
// signature hint while a call's arguments are typed (linux/PARITY.md item
// 2.5, the Mac's signature help in Lsp+Completion.mm), with real keys,
// against the scripted server (build/fakelsp), whose trigger characters
// are "(" and ",". "(" (auto-closed) shows open's signature with its first
// parameter's documentation; "," shows the second's; Escape closes the
// hint and leaves the text alone; a call the server has no signature for
// shows none.
#include "CompletionChecks.h"

#include <string>

using namespace cmpcheck;

namespace {

const char* kServer = "sig";
std::string F() { return MCGuiProjectFile("sig/main.py"); }
const std::string kText = "def f():\n    \n";
std::string gSettings;
int gAsked = 0;

std::string signature(int active) {
    return "{\"signatures\": [{\"label\": \"open(file, mode)\", \"documentation\": \"Open a file.\","
           " \"parameters\": [{\"label\": \"file\", \"documentation\": \"the file's path\"},"
           " {\"label\": \"mode\", \"documentation\": \"how to open it\"}]}],"
           " \"activeSignature\": 0, \"activeParameter\": " + std::to_string(active) + "}";
}

std::string script() {
    return "{\"capabilities\": {\"textDocumentSync\": {\"openClose\": true, \"change\": 1},"
           " \"signatureHelpProvider\": {\"triggerCharacters\": [\"(\", \",\"],"
           " \"retriggerCharacters\": [\")\"]}},"
           " \"rules\": ["
           "{\"method\": \"textDocument/signatureHelp\", \"when\": {\"textContains\": \"len(\"},"
           " \"result\": null},"
           "{\"method\": \"textDocument/signatureHelp\", \"when\": {\"textContains\": \"open(x,\"},"
           " \"result\": " + signature(1) + "},"
           "{\"method\": \"textDocument/signatureHelp\", \"result\": " + signature(0) + "}]}";
}

bool hintHas(const std::string& what) {
    return lsp()->signatureText().find(what) != std::string::npos;
}

}  // namespace

MC_GUI_SUITE(signature_help) {
    MCGuiStep("a Python file and a server with signature help", [] {
        MCGuiSetKeyWindow(nullptr);
        ed()->closeAllTabs();
        MCGuiWriteFile(F(), kText);
        gSettings = MCGuiReadFile(g_getenv("MINICODE_SETTINGS"));
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + MCGuiFakeLsp(kServer, script()) + "\n");
        showFile(F());
    });
    MCGuiWait("the server has the file", 20, [] {
        return showing(F()) && MCGuiFakeLspReceived(kServer, "textDocument/didOpen", "main.py") > 0;
    });
    MCGuiStep("open( typed in the body", [] {
        setText(kText, (long)kText.size() - 1);
        MCGuiType("open(");
    });
    MCGuiWait("the hint: the signature, the first parameter's documentation", 10, [] {
        return hintHas("open(file, mode)") && hintHas("the file's path");
    });
    MCGuiStep("the closer auto-closed, the caret inside", [] {
        GCHECK_EQ_STR(lineAtCaret(), "    open()");
        GCHECK(caret() == text().rfind(')'));
        MCGuiType("x,");
    });
    MCGuiWait("the second parameter's", 10, [] { return hintHas("how to open it"); });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("the hint closed", 10, [] { return lsp()->signatureText().empty(); });
    MCGuiStep("the text left alone", [] { GCHECK_EQ_STR(lineAtCaret(), "    open(x,)"); });
    MCGuiStep("a call the server has no signature for", [] {
        setText(kText, (long)kText.size() - 1);
        gAsked = MCGuiFakeLspReceived(kServer, "textDocument/signatureHelp");
        MCGuiType("len(");
    });
    MCGuiWait("the server asked", 10, [] {
        return MCGuiFakeLspReceived(kServer, "textDocument/signatureHelp") > gAsked;
    });
    MCGuiPause(0.3);   // negative: no hint comes of the empty answer
    MCGuiStep("no hint", [] { GCHECK(lsp()->signatureText().empty()); });

    MCGuiStep("the servers off again; the tab closed unasked", [] {
        setText(kText);
        MCGuiSetSettings(gSettings);
        ed()->closeAllTabs();
    });
    MCGuiWait("no server left", 10, [] { return LspRunningServerCount() == 0; });
}
