// ServerSymbolsSuite.cpp — symbols from the language servers (`./build/
// minicode-guitest server_symbols`, linux/PARITY.md item 2.12), the language
// server half of the Mac's tests/gui/SymbolsSuite.mm, against two scripted
// servers (FakeLsp.h) and by real keys: Ctrl+Shift+O lists the Python
// server's documentSymbol tree nested (flattened parent first, each child
// with its container), the caret's symbol chosen, Down previews and Escape
// puts the place back, typing and Return go there; a server that does not
// list symbols (the C++ one) and one that refuses leave it to minicode's own
// outline. Ctrl+T ("#") asks both servers for workspace/symbol: a slow
// server's answer is waited for while the other's shows, a new query cancels
// the question still out, the answers are merged with a symbol both name
// listed once, and Return opens the pick in another file, a jump Ctrl+Alt+-
// comes back from.
#include "FakeLsp.h"
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "Lsp.h"
#include "PalettePanel.h"

#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
PalettePanel* pal() { return MCGuiApp().palette; }
std::string F(const std::string& name) { return MCGuiProjectFile("ssym/" + name); }
const char* kA = "ssymA";   // the Python server
const char* kB = "ssymB";   // the C++ server

bool editorHasKeyboard() { return gtk_widget_has_focus(ed()->textView()); }
bool onShow(const std::string& name) { return ed()->currentPath() == F(name) && ed()->isSource(); }

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); i++) s += (i ? "|" : "") + v[i];
    return s;
}
std::string selectedLabel() {
    const std::vector<std::string> l = pal()->labels();
    const int i = pal()->selected();
    return i >= 0 && i < (int)l.size() ? l[(size_t)i] : "";
}

const std::string kPy =
    "class Widget:\n"                // 0
    "    def draw(self):\n"          // 1
    "        return 1\n"             // 2
    "\n"                             // 3
    "    def size(self):\n"          // 4
    "        return 2\n"             // 5
    "\n"                             // 6
    "\n"                             // 7
    "def helper():\n"                // 8
    "    return Widget()\n"          // 9
    "\n"                             // 10
    "VERSION = 3\n";                 // 11
const std::string kC =
    "struct Engine {\n"              // 0
    "    int power = 0;\n"           // 1
    "};\n"                           // 2
    "\n"                             // 3
    "int start(Engine& e) {\n"       // 4
    "    return e.power;\n"          // 5
    "}\n";                           // 6
const std::string kRefused = "def lonely():\n    return 0\n";

std::string range(int l1, int c1, int l2, int c2) {
    return "{\"start\": {\"line\": " + std::to_string(l1) + ", \"character\": " +
           std::to_string(c1) + "}, \"end\": {\"line\": " + std::to_string(l2) +
           ", \"character\": " + std::to_string(c2) + "}}";
}
std::string docSym(const std::string& name, int kind, const std::string& whole,
                   const std::string& sel, const std::string& children = "") {
    return "{\"name\": \"" + name + "\", \"kind\": " + std::to_string(kind) + ", \"range\": " +
           whole + ", \"selectionRange\": " + sel +
           (children.empty() ? "" : ", \"children\": [" + children + "]") + "}";
}
std::string wsSym(const std::string& name, int kind, const std::string& container,
                  const std::string& uri, int line, int c1, int c2) {
    return "{\"name\": \"" + name + "\", \"kind\": " + std::to_string(kind) +
           ", \"containerName\": \"" + container + "\", \"location\": {\"uri\": \"" + uri +
           "\", \"range\": " + range(line, c1, line, c2) + "}}";
}

// The Python server: a tree for a.py (a class with two methods, then a
// function and a constant the outline does not list), a refusal for c.py,
// and three workspace symbols, one of them also the C++ server's.
std::string scriptA() {
    const std::string tree =
        docSym("Widget", 5, range(0, 0, 5, 16), range(0, 6, 0, 12),
               docSym("draw", 6, range(1, 4, 2, 16), range(1, 8, 1, 12)) + ", " +
                   docSym("size", 6, range(4, 4, 5, 16), range(4, 8, 4, 12))) +
        ", " + docSym("helper", 12, range(8, 0, 9, 19), range(8, 4, 8, 10)) + ", " +
        docSym("VERSION", 14, range(11, 0, 11, 11), range(11, 0, 11, 7));
    const std::string ws = wsSym("Widget", 5, "a", "${uri:a.py}", 0, 6, 12) + ", " +
                           wsSym("helper", 12, "a", "${uri:a.py}", 8, 4, 10) + ", " +
                           wsSym("Shared", 5, "lib", "${uri:" + F("lib.py") + "}", 2, 6, 12);
    return "{\"name\": \"ssymA\", \"rules\": ["
           "{\"method\": \"textDocument/documentSymbol\", \"when\": {\"uriEndsWith\": \"/c.py\"},"
           " \"error\": {\"code\": -32603, \"message\": \"no\"}},"
           "{\"method\": \"textDocument/documentSymbol\", \"result\": [" + tree + "]},"
           "{\"method\": \"workspace/symbol\", \"result\": [" + ws + "]}]}";
}

// The C++ server: no documentSymbol, and workspace symbols, slowly for "ed".
std::string scriptB() {
    const std::string ws = wsSym("Engine", 23, "engine", "${uri:b.cpp}", 0, 7, 13) + ", " +
                           wsSym("Shared", 5, "lib", "${uri:" + F("lib.py") + "}", 2, 6, 12);
    return "{\"name\": \"ssymB\", \"capabilities\": {\"textDocumentSync\": 1,"
           " \"workspaceSymbolProvider\": true},"
           " \"rules\": ["
           "{\"method\": \"workspace/symbol\", \"when\": {\"paramsContain\": \"\\\"query\\\":\\\"ed\\\"\"},"
           " \"delayMs\": 3000, \"result\": [" + ws + "]},"
           "{\"method\": \"workspace/symbol\", \"result\": [" + ws + "]}]}";
}

std::pair<int, int> selection() {
    GtkTextBuffer* b = ed()->sourceBuffer();
    GtkTextIter a, e;
    gtk_text_buffer_get_selection_bounds(b, &a, &e);
    return {gtk_text_iter_get_offset(&a), gtk_text_iter_get_offset(&e)};
}
// [start, end) of the characters `col` to `col + n` on `line` of the source.
std::pair<int, int> at(int line, int col, int n) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line_offset(ed()->sourceBuffer(), &it, line, col);
    const int o = gtk_text_iter_get_offset(&it);
    return {o, o + n};
}
void caretAt(int line, int col) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line_offset(ed()->sourceBuffer(), &it, line, col);
    gtk_text_buffer_place_cursor(ed()->sourceBuffer(), &it);
}
void showTab(const std::string& name) {
    MCGuiOpenFileKept(F(name));
    MCGuiFocusKeyWindow();
    ed()->focusActiveDocument();
}
bool has(const std::vector<std::string>& v, const std::string& x) {
    for (const std::string& s : v)
        if (s == x) return true;
    return false;
}

std::string gSettings;
std::pair<int, int> gStart;

}  // namespace

MC_GUI_SUITE(server_symbols) {
    MCGuiStep("a project; a C++ and a Python scripted server", [] {
        MCGuiSetKeyWindow(nullptr);
        ed()->closeAllTabs();
        MCGuiWriteFile(F("a.py"), kPy);
        MCGuiWriteFile(F("b.cpp"), kC);
        MCGuiWriteFile(F("c.py"), kRefused);
        MCGuiWriteFile(F("lib.py"), "# lib\n\nclass Shared:\n    pass\n");
        gSettings = MCGuiReadFile(g_getenv("MINICODE_SETTINGS"));
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + MCGuiFakeLsp(kA, scriptA()) +
                         "\nlsp.cpp = " + MCGuiFakeLsp(kB, scriptB()) + "\n");
        showTab("b.cpp");
    });
    MCGuiWait("b.cpp on its server, with the keyboard", 20, [] {
        return onShow("b.cpp") && editorHasKeyboard() &&
               MCGuiFakeLspReceived(kB, "textDocument/didOpen", "/b.cpp") == 1;
    });

    // ------------------------------------- a server that lists no symbols
    MCGuiStep("Ctrl+Shift+O where the server lists none", [] { MCGuiKey("ctrl+shift+o"); });
    MCGuiWait("minicode's own outline stands in", 10, [] {
        return pal()->isOpen() && pal()->symbolSource() == "outline" && !pal()->labels().empty();
    });
    MCGuiStep("the struct and the function; the server never asked", [] {
        GCHECK(has(pal()->labels(), "Engine") && has(pal()->labels(), "start"));
        if (!has(pal()->labels(), "Engine")) std::printf("  rows: %s\n", joined(pal()->labels()).c_str());
        GCHECK(MCGuiFakeLspReceived(kB, "textDocument/documentSymbol") == 0);
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 10, [] { return !pal()->isOpen() && editorHasKeyboard(); });

    // ------------------------------------------- the server's own symbols
    MCGuiStep("a.py, the caret in Widget.size", [] { showTab("a.py"); });
    MCGuiWait("a.py on its server, with the keyboard", 20, [] {
        return onShow("a.py") && editorHasKeyboard() &&
               MCGuiFakeLspReceived(kA, "textDocument/didOpen", "/a.py") == 1;
    });
    MCGuiStep("Ctrl+Shift+O", [] {
        caretAt(5, 10);
        gStart = selection();
        MCGuiKey("ctrl+shift+o");
    });
    MCGuiWait("the symbols arrive", 10, [] {
        return pal()->isOpen() && !pal()->symbolSource().empty();
    });
    MCGuiStep("the server's, nested, parent first, the caret's method chosen, nothing moved", [] {
        GCHECK_EQ_STR(pal()->symbolSource(), "fakelsp");   // the server, as the status bar names it
        GCHECK_EQ_STR(joined(pal()->labels()), "Widget|draw|size|helper|VERSION");
        GCHECK_EQ_STR(joined(pal()->details()), "|Widget|Widget||");
        GCHECK_EQ_STR(selectedLabel(), "size");
        GCHECK(pal()->keys().size() == 5 && pal()->keys()[4] == "12");
        GCHECK(MCGuiFakeLspReceived(kA, "textDocument/documentSymbol", "/a.py") == 1);
        GCHECK(selection() == gStart);
        MCGuiSnapshot(pal()->box(), "server-symbols");
        MCGuiKey("down");
    });
    MCGuiWait("Down shows helper's name in the editor", 10, [] {
        return selectedLabel() == "helper" && selection() == at(8, 4, 6);
    });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("the caret back where it was, with the keyboard", 10, [] {
        return !pal()->isOpen() && selection() == gStart && editorHasKeyboard();
    });
    MCGuiStep("typing ranks: \"ver\"", [] {
        MCGuiKey("ctrl+shift+o");
    });
    MCGuiWait("listed", 10, [] { return pal()->isOpen() && pal()->labels().size() == 5; });
    MCGuiStep("\"ver\"", [] { MCGuiType("ver"); });
    MCGuiWait("the constant the outline does not list", 10, [] {
        return selectedLabel() == "VERSION" && selection() == at(11, 0, 7);
    });
    MCGuiStep("Return", [] { MCGuiKey("enter"); });
    MCGuiWait("there, the keyboard in the editor", 10, [] {
        return !pal()->isOpen() && selection() == at(11, 0, 7) && editorHasKeyboard();
    });

    // ------------------------------------------------- a refusal
    MCGuiStep("c.py, whose symbols the server refuses", [] { showTab("c.py"); });
    MCGuiWait("on its server", 20, [] {
        return onShow("c.py") && editorHasKeyboard() &&
               MCGuiFakeLspReceived(kA, "textDocument/didOpen", "/c.py") == 1;
    });
    MCGuiStep("Ctrl+Shift+O", [] { MCGuiKey("ctrl+shift+o"); });
    MCGuiWait("the outline stands in", 10, [] {
        return pal()->isOpen() && pal()->symbolSource() == "outline" &&
               joined(pal()->labels()) == "lonely";
    });
    MCGuiStep("the server was asked; Escape", [] {
        GCHECK(MCGuiFakeLspReceived(kA, "textDocument/documentSymbol", "/c.py") == 1);
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 10, [] { return !pal()->isOpen(); });

    // ----------------------------------------------- the workspace's
    MCGuiStep("a.py, the caret on line 3; Ctrl+T", [] { showTab("a.py"); });
    MCGuiWait("a.py with the keyboard", 10, [] { return onShow("a.py") && editorHasKeyboard(); });
    MCGuiStep("Ctrl+T", [] {
        caretAt(2, 4);
        gStart = selection();
        MCGuiKey("ctrl+t");
    });
    MCGuiWait("\"#\", asking for a name", 10, [] {
        return pal()->isOpen() && pal()->text() == "#" &&
               pal()->emptyText() == "Type the name of a symbol to look for it in the workspace";
    });
    MCGuiStep("\"e\"", [] { MCGuiType("e"); });
    MCGuiWait("both servers answered, merged", 10, [] {
        return pal()->workspaceComplete() && pal()->labels().size() == 4;
    });
    MCGuiStep("each symbol once, its file and line beside it", [] {
        const std::vector<std::string> l = pal()->labels(), k = pal()->keys();
        for (const char* name : {"Widget", "helper", "Shared", "Engine"}) GCHECK(has(l, name));
        GCHECK(has(k, "ssym/lib.py:3") && has(k, "ssym/b.cpp:1") && has(k, "ssym/a.py:9"));
        if (l.size() != 4 || !has(k, "ssym/b.cpp:1"))
            std::printf("  rows: %s / %s\n", joined(l).c_str(), joined(k).c_str());
        GCHECK(MCGuiFakeLspReceived(kA, "workspace/symbol") >= 1);
        GCHECK(MCGuiFakeLspReceived(kB, "workspace/symbol") >= 1);
        GCHECK(MCGuiFakeLspReceived(kB, "$/cancelRequest") == 0);
        MCGuiType("d");
    });
    MCGuiWait("\"ed\": the fast server's answer shows while the slow one's is out", 10, [] {
        return pal()->text() == "#ed" && joined(pal()->labels()) == "Shared" &&
               !pal()->workspaceComplete() && MCGuiFakeLspReceived(kB, "workspace/symbol") >= 2;
    });
    MCGuiStep("Backspace before it answers", [] { MCGuiKey("backspace"); });
    MCGuiWait("the question still out is cancelled, and \"e\" answered again", 10, [] {
        return MCGuiFakeLspReceived(kB, "$/cancelRequest") == 1 && pal()->text() == "#e" &&
               pal()->workspaceComplete() && pal()->labels().size() == 4;
    });
    MCGuiStep("\"ngi\"", [] { MCGuiType("ngi"); });
    MCGuiWait("Engine first", 10, [] {
        return pal()->workspaceComplete() && selectedLabel() == "Engine";
    });
    MCGuiStep("Return", [] { MCGuiKey("enter"); });
    MCGuiWait("b.cpp, Engine's name selected", 10, [] {
        return !pal()->isOpen() && onShow("b.cpp") && selection() == at(0, 7, 6);
    });
    MCGuiStep("Ctrl+Alt+- goes back", [] {
        MCGuiFocusKeyWindow();
        ed()->focusActiveDocument();
        MCGuiKey("ctrl+alt+-");
    });
    MCGuiWait("a.py, where the caret was", 10, [] {
        return onShow("a.py") && selection() == gStart;
    });

    MCGuiStep("the settings as they were; the tabs closed unasked", [] {
        MCGuiSetSettings(gSettings);
        ed()->closeAllTabs();
    });
    MCGuiWait("no server left", 10, [] { return LspRunningServerCount() == 0; });
}
