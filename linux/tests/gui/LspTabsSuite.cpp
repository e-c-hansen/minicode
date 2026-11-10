// LspTabsSuite.cpp — `./build/minicode-guitest lsp_tabs`: every tab's file
// stays open on its language server (linux/PARITY.md item 2.0, the Mac's
// syncOpenDocuments, checked there by tests/gui/LspFixesSuite.mm), against
// the scripted server (build/fakelsp) taking ranged changes. Three Python
// tabs and a Go tab: each Python file is opened once on the server and stays
// open while another tab is on show; keys typed into two of them reach it,
// as ranges, with each file's own text (a character outside the Basic
// Multilingual Plane included, which sends the whole text); an edit made to
// a tab in the background (as Replace in Folder makes) reaches it too;
// nothing is saved; a new server (the settings changed) is given every
// Python tab at once, unsaved text and all, while the Go tab in the
// background starts no server until it comes on show; and a closed tab is
// closed on the server.
#include "Harness.h"

#include "Editor.h"
#include "FakeLspLog.h"
#include "Lsp.h"
#include "MultiEdit.h"

#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
LspSession* lsp() { return MCGuiApp().lsp; }
std::string F(const std::string& name) { return MCGuiProjectFile("lsptabs/" + name); }

const char* kServer = "tabs";     // the first Python server
const char* kServer2 = "tabs2";   // the one the settings change starts
const char* kGo = "tabsgo";

// Long enough that a change's range costs less than the whole text, which
// is what the client sends when it does not (Client::didChange).
std::string filler(const char* what) {
    std::string s;
    for (int i = 0; i < 20; i++)
        s += std::string("# ") + what + " filler line " + std::to_string(i) + "\n";
    return s;
}
const std::string kA = "def alpha():\n    return 1\n" + filler("a");
const std::string kB = "def beta():\n    return 2\n" + filler("b");
const std::string kC = "GAMMA = 3\n" + filler("c");
const std::string kX = "package main\n";

std::string gSettings, gPython, gPython2, gGo;

// A server taking ranged changes (textDocumentSync.change 2).
std::string script() {
    return "{\"capabilities\": {\"textDocumentSync\": {\"openClose\": true, \"change\": 2,"
           " \"save\": {\"includeText\": false}}, \"hoverProvider\": true}, \"rules\": []}";
}

std::string textOf(const std::string& name) {
    Document* d = ed()->documentFor(F(name));
    if (!d) return "<no tab>";
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(d->buffer(), &a, &b);
    char* t = gtk_text_buffer_get_text(d->buffer(), &a, &b, TRUE);
    std::string s = t ? t : "";
    g_free(t);
    return s;
}

// The server has the file open with the tab's own text.
bool serverHasTab(const char* server, const std::string& name) {
    std::string text;
    return MCGuiFakeLspText(server, name, &text) && text == textOf(name);
}

bool onShow(const std::string& name) {
    return ed()->currentPath() == F(name) && ed()->isSource();
}

bool editorHasKeyboard() { return gtk_widget_has_focus(ed()->textView()); }

std::string openDocs() {
    std::string s;
    for (const std::string& d : lsp()->openDocuments()) {
        const size_t slash = d.find_last_of('/');
        s += (s.empty() ? "" : ", ") + d.substr(0, d.find(' ')) + " " + d.substr(slash + 1);
    }
    return s;
}

// The window has the X focus (there is no window manager to give it), and
// the editor the keyboard.
void showTab(const std::string& name) {
    MCGuiOpenFileKept(F(name));
    MCGuiFocusKeyWindow();
    ed()->focusActiveDocument();
}

void answer(const char* button) {
    MCGuiClick(MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), button));
}

bool asking() {
    GtkWindow* top = MCGuiTopWindow();
    return top && top != MCGuiApp().window && MCGuiFindButton(GTK_WIDGET(top), "Don't Save");
}

}  // namespace

MC_GUI_SUITE(lsp_tabs) {
    MCGuiStep("three Python files and a Go file; Python's scripted server, Go's off", [] {
        MCGuiSetKeyWindow(nullptr);
        ed()->closeAllTabs();
        MCGuiWriteFile(F("a.py"), kA);
        MCGuiWriteFile(F("b.py"), kB);
        MCGuiWriteFile(F("c.py"), kC);
        MCGuiWriteFile(F("x.go"), kX);
        gSettings = MCGuiReadFile(g_getenv("MINICODE_SETTINGS"));
        gPython = MCGuiFakeLsp(kServer, script());
        gPython2 = MCGuiFakeLsp(kServer2, script());
        gGo = MCGuiFakeLsp(kGo, script());
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + gPython + "\nlsp.go = off\n");
        showTab("a.py");
    });
    MCGuiWait("the server has a.py", 20, [] {
        return MCGuiFakeLspReceived(kServer, "initialized") > 0 && serverHasTab(kServer, "a.py");
    });
    MCGuiStep("b.py, c.py and x.go in tabs of their own, x.go on show", [] {
        showTab("b.py");
        showTab("c.py");
        showTab("x.go");
    });
    MCGuiWait("each Python file open on the server", 10, [] {
        return onShow("x.go") && serverHasTab(kServer, "b.py") && serverHasTab(kServer, "c.py");
    });
    MCGuiStep("each opened once, none closed by leaving it; no server for Go", [] {
        GCHECK(MCGuiFakeLspReceived(kServer, "textDocument/didOpen", "a.py") == 1);
        GCHECK(MCGuiFakeLspReceived(kServer, "textDocument/didOpen", "b.py") == 1);
        GCHECK(MCGuiFakeLspReceived(kServer, "textDocument/didOpen", "c.py") == 1);
        GCHECK(MCGuiFakeLspReceived(kServer, "textDocument/didClose") == 0);
        GCHECK_EQ_STR(openDocs(), "python a.py, python b.py, python c.py");
        GCHECK(LspRunningServerCount() == 1);
    });

    // ---- typing into two tabs
    MCGuiStep("b.py on show; a line typed at its top", [] { showTab("b.py"); });
    MCGuiWait("b.py with the keyboard", 10, [] { return onShow("b.py") && editorHasKeyboard(); });
    MCGuiStep("Ctrl+Home, typed", [] {
        MCGuiKey("ctrl+Home");
        MCGuiType("# edited b\n");
    });
    MCGuiWait("typed", 10, [] { return textOf("b.py") == "# edited b\n" + kB; });
    MCGuiStep("a.py on show; a line typed at its end, with an emoji", [] { showTab("a.py"); });
    MCGuiWait("a.py with the keyboard", 10, [] { return onShow("a.py") && editorHasKeyboard(); });
    MCGuiStep("Ctrl+End, typed", [] {
        MCGuiKey("ctrl+End");
        MCGuiType("x = 1\n");
    });
    MCGuiWait("typed", 10, [] { return textOf("a.py") == kA + "x = 1\n"; });
    MCGuiWait("both edits reach the server, b.py's while it is in the background", 10, [] {
        return serverHasTab(kServer, "a.py") && serverHasTab(kServer, "b.py");
    });
    MCGuiStep("as ranges, for a.py and b.py only, nothing closed or saved", [] {
        GCHECK(MCGuiFakeLspRangedChanges(kServer, "a.py") >= 1);
        GCHECK(MCGuiFakeLspRangedChanges(kServer, "b.py") >= 1);
        GCHECK(MCGuiFakeLspReceived(kServer, "textDocument/didChange", "c.py") == 0);
        GCHECK(MCGuiFakeLspReceived(kServer, "textDocument/didClose") == 0);
        GCHECK(MCGuiFakeLspReceived(kServer, "textDocument/didOpen", "b.py") == 1);
        GCHECK(lsp()->rangedChangesSent() >= 2);
        GCHECK_EQ_STR(MCGuiReadFile(F("a.py")), kA);
        GCHECK_EQ_STR(MCGuiReadFile(F("b.py")), kB);
        MCGuiType("s = '😀'\n");
    });
    MCGuiWait("the emoji line typed", 10, [] {
        return textOf("a.py") == kA + "x = 1\ns = '😀'\n";
    });
    MCGuiWait("and on the server, two units for one character", 10, [] {
        return serverHasTab(kServer, "a.py");
    });
    MCGuiStep("an edit to c.py in the background, as Replace in Folder makes", [] {
        GCHECK(onShow("a.py"));
        Document* c = ed()->documentFor(F("c.py"));
        GCHECK(c != nullptr);
        if (c) GCHECK(MultiEdit::apply(c->buffer(), {{0, 0, "# c\n"}}));
    });
    MCGuiWait("it reaches the server", 10, [] {
        return textOf("c.py") == "# c\n" + kC && serverHasTab(kServer, "c.py");
    });

    // ---- a new server gets every tab; a background tab starts none
    MCGuiStep("the settings change: a new Python server, and Go's on", [] {
        GCHECK(onShow("a.py"));
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + gPython2 + "\nlsp.go = " + gGo + "\n");
    });
    MCGuiWait("the new server has every Python tab, unsaved text and all", 20, [] {
        return serverHasTab(kServer2, "a.py") && serverHasTab(kServer2, "b.py") &&
               serverHasTab(kServer2, "c.py");
    });
    // A pause before a negative check: a server x.go's tab would have started
    // in the background would have been sent initialize by now.
    MCGuiPause(1.0);
    MCGuiStep("x.go, in the background, started no server", [] {
        GCHECK(MCGuiFakeLspReceived(kGo, "initialize") == 0);
        GCHECK(LspRunningServerCount() == 1);
        GCHECK_EQ_STR(openDocs(), "python a.py, python b.py, python c.py");
        GCHECK(MCGuiFakeLspReceived(kServer2, "textDocument/didOpen") == 3);
        showTab("x.go");
    });
    MCGuiWait("on show, it starts Go's server", 20, [] {
        return onShow("x.go") && serverHasTab(kGo, "x.go");
    });
    MCGuiStep("two servers, four files open", [] {
        GCHECK(LspRunningServerCount() == 2);
        GCHECK_EQ_STR(openDocs(), "go x.go, python a.py, python b.py, python c.py");
    });

    // ---- closing tabs
    MCGuiStep("c.py on show, Ctrl+W", [] {
        showTab("c.py");
        MCGuiKey("ctrl+w");
    });
    MCGuiWait("asked about its unsaved change", 10, asking);
    MCGuiStep("Don't Save", [] { answer("Don't Save"); });
    MCGuiWait("c.py closed on the server", 10, [] {
        return !ed()->documentFor(F("c.py")) &&
               MCGuiFakeLspReceived(kServer2, "textDocument/didClose", "c.py") == 1;
    });
    MCGuiStep("b.py on show, Ctrl+W", [] {
        MCGuiSetKeyWindow(nullptr);
        showTab("b.py");
        MCGuiKey("ctrl+w");
    });
    MCGuiWait("asked", 10, asking);
    MCGuiStep("Don't Save", [] { answer("Don't Save"); });
    MCGuiWait("b.py closed on the server, the others still open", 10, [] {
        return !ed()->documentFor(F("b.py")) &&
               MCGuiFakeLspReceived(kServer2, "textDocument/didClose", "b.py") == 1;
    });
    MCGuiStep("a.py and x.go still open; b.py and c.py as they were on disk", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK_EQ_STR(openDocs(), "go x.go, python a.py");
        GCHECK(MCGuiFakeLspReceived(kServer2, "textDocument/didClose", "a.py") == 0);
        GCHECK_EQ_STR(MCGuiReadFile(F("b.py")), kB);
        GCHECK_EQ_STR(MCGuiReadFile(F("c.py")), kC);
    });

    MCGuiStep("the servers off again; the tabs closed unasked", [] {
        MCGuiSetSettings(gSettings);
        ed()->closeAllTabs();
    });
    MCGuiWait("no server left", 10, [] { return LspRunningServerCount() == 0; });
}
