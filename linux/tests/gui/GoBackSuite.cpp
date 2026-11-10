// GoBackSuite.cpp — Go Back and Go Forward (`./build/minicode-guitest
// go_back`, linux/PARITY.md item 1.11; VS Code's Linux Ctrl+Alt+- and
// Ctrl+Shift+-), the counterpart of the Mac's Go Back checks in
// tests/gui/SymbolsSuite.mm, with real keys and clicks through XTest: a
// caret moved more than ten lines with the text unchanged is a jump and a
// nearer one is not; typing many lines is none; Back and Forward go between
// the places and stop at the ends; a tab switch by its key, Go to Line from
// the palette, a symbol from the palette (what it previews is no jump), a
// Find in Folder hit and the language server's definition (the scripted
// server, build/fakelsp) are jumps; a new jump drops what lay ahead; a file
// renamed in the tree is followed, and one deleted from the disk is passed
// over.
#include "Harness.h"

#include "Editor.h"
#include "FakeLsp.h"
#include "FileTree.h"
#include "Keys.h"
#include "NavHistoryUi.h"
#include "PalettePanel.h"
#include "Search.h"

#include <glib/gstdio.h>

#include <string>
#include <vector>

namespace {

std::string F(const std::string& name) { return MCGuiProjectFile("goback/" + name); }
Editor* ed() { return MCGuiApp().editor; }
NavHistoryUi* nav() { return MCGuiApp().nav; }
bool editorHasKeyboard() { return gtk_widget_has_focus(ed()->textView()); }

// The history as file names and lines, ">" before the current place.
std::string history() {
    std::string s;
    const std::vector<std::string> e = nav()->entries();
    for (size_t i = 0; i < e.size(); i++) {
        std::string name = e[i].substr(e[i].find_last_of('/') + 1);
        if (!s.empty()) s += " ";
        s += (i == nav()->index() ? ">" : "") + name;
    }
    return s;
}

int caretLine() {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(ed()->buffer(), &it, gtk_text_buffer_get_insert(ed()->buffer()));
    return gtk_text_iter_get_line(&it);
}

bool at(const std::string& name, int line) {
    return ed()->currentPath() == F(name) && ed()->isSource() && caretLine() == line;
}

std::string lines(const char* word, int n) {
    std::string s;
    for (int i = 0; i < n; i++) s += std::string(word) + " line " + std::to_string(i) + "\n";
    return s;
}

GtkWidget* labelHolding(GtkWidget* root, const std::string& part) {
    if (!root || !gtk_widget_get_mapped(root)) return nullptr;
    if (GTK_IS_LABEL(root)) {
        const char* text = gtk_label_get_text(GTK_LABEL(root));
        if (text && std::string(text).find(part) != std::string::npos)
            return MCGuiOnScreen(root) ? root : nullptr;
    }
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = labelHolding(c, part)) return hit;
    return nullptr;
}

const char* kServer = "goback";
GtkWindow* gSearch = nullptr;
std::string gHistory;
std::string gSettings;

}  // namespace

MC_GUI_SUITE(go_back) {
    MCGuiStep("files to go between, the first with the keyboard, the history empty", [] {
        MCGuiWriteFile(F("gb-a.txt"), lines("a", 200));
        MCGuiWriteFile(F("gb-b.txt"), lines("b", 60));
        MCGuiWriteFile(F("gb-c.txt"), lines("c", 30));
        MCGuiWriteFile(F("gb-d.txt"), lines("d", 30));
        MCGuiWriteFile(F("gb-found.txt"), lines("f", 40) + "the navneedle is here\n" + lines("g", 20));
        std::string py;
        for (int i = 0; i < 80; i++)
            py += i == 10 ? "class Holder:\n" : "# filler " + std::to_string(i) + "\n";
        py += "def target_function():\n    return 1\n\n\nvalue = target_function()\n";
        MCGuiWriteFile(F("gb-def.py"), py);
        std::string lib;
        for (int i = 0; i < 20; i++) lib += "# lib " + std::to_string(i) + "\n";
        MCGuiWriteFile(F("gb-lib.py"), lib + "def target_function():\n    return 2\n");
        gSettings = MCGuiReadFile(g_getenv("MINICODE_SETTINGS"));
        MCGuiOpenFileKept(F("gb-a.txt"));
    });
    MCGuiWait("it shows", 10, [] { return ed()->currentPath() == F("gb-a.txt"); });
    MCGuiStep("a click in it, Ctrl+Home", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+Home");
    });
    MCGuiWait("at its start", 10, [] { return editorHasKeyboard() && at("gb-a.txt", 0); });
    MCGuiStep("the history emptied", [] {
        nav()->clear();
        GCHECK(history().empty());
    });

    // ---- the ten-line rule
    MCGuiStep("Down eight times", [] {
        for (int i = 0; i < 8; i++) MCGuiKey("down");
    });
    MCGuiWait("line 8, and no jump: each move was near", 10, [] {
        return at("gb-a.txt", 8) && history().empty();
    });
    MCGuiStep("Ctrl+End", [] { MCGuiKey("ctrl+End"); });
    MCGuiWait("a jump from line 8 to the end", 10, [] {
        return at("gb-a.txt", 200) && history() == "gb-a.txt:8 >gb-a.txt:200";
    });
    MCGuiStep("Up three times: near, no jump", [] {
        for (int i = 0; i < 3; i++) MCGuiKey("up");
    });
    MCGuiWait("line 197", 10, [] { return at("gb-a.txt", 197); });
    MCGuiStep("Ctrl+Alt+- goes back", [] {
        GCHECK_EQ_STR(history(), "gb-a.txt:8 >gb-a.txt:200");
        MCGuiKey("ctrl+alt+-");
    });
    MCGuiWait("to line 8; where it left (197) kept for Forward", 10, [] {
        return at("gb-a.txt", 8) && history() == ">gb-a.txt:8 gb-a.txt:197" && editorHasKeyboard();
    });
    MCGuiStep("Ctrl+Alt+- again: nowhere further", [] {
        nav()->canGoBack();
        MCGuiKey("ctrl+alt+-");
    });
    MCGuiPause(0.3);   // a negative check: nothing moves
    MCGuiStep("still line 8; Ctrl+Shift+- goes forward", [] {
        GCHECK(at("gb-a.txt", 8) && history() == ">gb-a.txt:8 gb-a.txt:197");
        MCGuiKey("ctrl+shift+-");
    });
    MCGuiWait("to line 197", 10, [] { return at("gb-a.txt", 197) && history() == "gb-a.txt:8 >gb-a.txt:197"; });
    MCGuiStep("typing fifteen lines is no jump", [] {
        MCGuiType("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
    });
    MCGuiWait("typed", 10, [] { return at("gb-a.txt", 212); });
    MCGuiStep("no jump; the typing taken back", [] {
        GCHECK_EQ_STR(history(), "gb-a.txt:8 >gb-a.txt:197");
        for (int i = 0; i < 15; i++) MCGuiKey("ctrl+z");
    });
    MCGuiWait("as it was", 10, [] { return !ed()->dirty(); });

    // ---- a tab switch, Go to Line, a new jump dropping what lay ahead
    MCGuiStep("another file in a tab of its own", [] {
        MCGuiOpenFileKept(F("gb-b.txt"));
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+Home");
    });
    MCGuiWait("b.txt at its start", 10, [] { return at("gb-b.txt", 0) && editorHasKeyboard(); });
    MCGuiStep("the history emptied", [] { nav()->clear(); });
    MCGuiStep("Ctrl+Page Up, the tab before it (a.txt)", [] { MCGuiKey("ctrl+pageup"); });
    MCGuiWait("a.txt, and the switch is a jump", 10, [] {
        return ed()->currentPath() == F("gb-a.txt") && history() == "gb-b.txt:0 >gb-a.txt:" + std::to_string(caretLine());
    });
    MCGuiStep("Ctrl+Alt+- back to b.txt", [] { MCGuiKey("ctrl+alt+-"); });
    MCGuiWait("b.txt at its start", 10, [] { return at("gb-b.txt", 0); });
    MCGuiStep("Go to Line 45 from the palette (Ctrl+G)", [] { MCGuiKey("ctrl+g"); });
    MCGuiWait("the palette", 10, [] { return MCGuiApp().palette->isOpen(); });
    MCGuiStep("45 and Enter", [] { MCGuiType("45\n"); });
    MCGuiWait("line 44 (45 counted from 1), a jump that dropped a.txt, which lay ahead", 10, [] {
        return at("gb-b.txt", 44) && history() == "gb-b.txt:0 >gb-b.txt:44";
    });
    MCGuiStep("back, then Go to Line 30", [] {
        MCGuiKey("ctrl+alt+-");
    });
    MCGuiWait("b.txt at its start", 10, [] { return at("gb-b.txt", 0); });
    MCGuiStep("Ctrl+G 30", [] { MCGuiKey("ctrl+g"); });
    MCGuiWait("the palette", 10, [] { return MCGuiApp().palette->isOpen(); });
    MCGuiStep("30 and Enter", [] { MCGuiType("30\n"); });
    MCGuiWait("line 29; line 44, which lay ahead, gone", 10, [] {
        return at("gb-b.txt", 29) && history() == "gb-b.txt:0 >gb-b.txt:29" && !nav()->canGoForward();
    });
    MCGuiStep("Ctrl+Shift+- has nowhere to go", [] { MCGuiKey("ctrl+shift+-"); });
    MCGuiPause(0.3);   // a negative check: nothing moves
    MCGuiStep("still line 29", [] { GCHECK(at("gb-b.txt", 29) && history() == "gb-b.txt:0 >gb-b.txt:29"); });

    // ---- the palette's symbols
    MCGuiStep("a Python file", [] {
        MCGuiOpenFileKept(F("gb-def.py"));
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+Home");
    });
    MCGuiWait("at its start", 10, [] { return at("gb-def.py", 0) && editorHasKeyboard(); });
    MCGuiStep("the history emptied; Ctrl+Shift+O", [] {
        nav()->clear();
        MCGuiKey("ctrl+shift+o");
    });
    MCGuiWait("its symbols", 10, [] {
        return MCGuiApp().palette->isOpen() && MCGuiApp().palette->labels().size() >= 2;
    });
    MCGuiStep("Down shows one in the editor, then Escape", [] {
        MCGuiKey("down");
        MCGuiKey("down");
    });
    MCGuiWait("a symbol on show", 10, [] { return caretLine() == 10 || caretLine() == 80; });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("the editor back at its start, nothing recorded", 10, [] {
        return !MCGuiApp().palette->isOpen() && at("gb-def.py", 0) && history().empty();
    });
    MCGuiStep("Ctrl+Shift+O, \"target\" and Enter", [] { MCGuiKey("ctrl+shift+o"); });
    MCGuiWait("the symbols", 10, [] {
        return MCGuiApp().palette->isOpen() && MCGuiApp().palette->labels().size() >= 2;
    });
    MCGuiStep("typed", [] { MCGuiType("target\n"); });
    MCGuiWait("at the function, one jump from where the palette opened", 10, [] {
        return at("gb-def.py", 80) && history() == "gb-def.py:0 >gb-def.py:80";
    });
    MCGuiStep("Ctrl+Alt+- goes back", [] { MCGuiKey("ctrl+alt+-"); });
    MCGuiWait("to the start", 10, [] { return at("gb-def.py", 0); });

    // ---- the language server's definition, in another file
    MCGuiStep("a scripted language server for Python, whose definition is in gb-lib.py", [] {
        const std::string script =
            "{\"rules\": [{\"method\": \"textDocument/definition\", \"result\": {\"uri\": \"${uri:" +
            F("gb-lib.py") +
            "}\", \"range\": {\"start\": {\"line\": 20, \"character\": 4}, "
            "\"end\": {\"line\": 20, \"character\": 19}}}}]}";
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + MCGuiFakeLsp(kServer, script) + "\n");
    });
    MCGuiWait("it has the file", 20, [] {
        return MCGuiFakeLspReceived(kServer, "textDocument/didOpen", "gb-def.py") > 0;
    });
    MCGuiStep("the caret on the call; the history emptied", [] {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line_offset(ed()->buffer(), &it, 84, 10);
        gtk_text_buffer_place_cursor(ed()->buffer(), &it);
        nav()->clear();
    });
    MCGuiWait("there", 10, [] { return at("gb-def.py", 84) && history().empty(); });
    MCGuiStep("F12", [] { MCGuiKey("f12"); });
    MCGuiWait("gb-lib.py at the definition, a jump", 10, [] {
        return at("gb-lib.py", 20) && history() == "gb-def.py:84 >gb-lib.py:20";
    });
    MCGuiStep("Ctrl+Alt+- back to the call", [] { MCGuiKey("ctrl+alt+-"); });
    MCGuiWait("there", 10, [] { return at("gb-def.py", 84); });
    MCGuiStep("the server off again", [] { MCGuiSetSettings(gSettings); });

    // ---- a Find in Folder hit
    MCGuiStep("b.txt again, the caret on line 29", [] {
        MCGuiOpenFileKept(F("gb-b.txt"));
        MCGuiClick(ed()->textView());
    });
    MCGuiWait("b.txt with the keyboard", 10, [] {
        return ed()->currentPath() == F("gb-b.txt") && editorHasKeyboard();
    });
    MCGuiStep("the caret put on line 29, the history emptied", [] {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(ed()->buffer(), &it, 29);
        gtk_text_buffer_place_cursor(ed()->buffer(), &it);
        nav()->clear();
    });
    MCGuiWait("b.txt at line 29", 10, [] { return at("gb-b.txt", 29) && history().empty(); });
    // Find in Folder looks in the folder selected in the tree: this suite's,
    // whatever earlier suites left selected.
    MCGuiStep("the suite's folder selected in the tree", [] {
        MCGuiApp().tree->revealPath(MCGuiProjectFile("goback"));
    });
    MCGuiWait("selected", 10, [] {
        return MCGuiApp().tree->selectedDir() == MCGuiProjectFile("goback");
    });
    MCGuiStep("Find in Folder", [] { MCGuiKey("ctrl+shift+f"); });
    MCGuiWait("its window", 10, [] { return MCGuiTopWindow() != MCGuiApp().window; });
    // The window may be the one earlier suites used: their files to include
    // and exclude cleared, their query replaced.
    MCGuiStep("the word typed over whatever was there", [] {
        gSearch = MCGuiTopWindow();
        MCGuiSetKeyWindow(gSearch);
        if (SearchPanel* s = MCGuiApp().search) {
            gtk_editable_set_text(GTK_EDITABLE(s->includeEntry()), "");
            gtk_editable_set_text(GTK_EDITABLE(s->excludeEntry()), "");
        }
        MCGuiKey("ctrl+a");
        MCGuiType("navneedle");
    });
    MCGuiWait("the result", 10, [] { return labelHolding(GTK_WIDGET(gSearch), "the navneedle is here"); });
    MCGuiStep("a click on it", [] {
        MCGuiClick(labelHolding(GTK_WIDGET(gSearch), "the navneedle is here"));
    });
    MCGuiWait("found.txt at the match, a jump from b.txt", 10, [] {
        return ed()->currentPath() == F("gb-found.txt") && caretLine() == 40 &&
               history() == "gb-b.txt:29 >gb-found.txt:40";
    });
    MCGuiStep("the search window closed, the keyboard back in the editor", [] {
        MCGuiSetKeyWindow(gSearch);
        MCGuiKey("escape");
    });
    MCGuiWait("gone", 10, [] { return MCGuiTopWindow() == MCGuiApp().window; });
    MCGuiStep("Ctrl+Alt+-", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
        MCGuiKey("ctrl+alt+-");
    });
    MCGuiWait("back in b.txt at line 29", 10, [] { return at("gb-b.txt", 29); });
    MCGuiStep("Ctrl+Shift+-", [] { MCGuiKey("ctrl+shift+-"); });
    MCGuiWait("forward to the match", 10, [] {
        return ed()->currentPath() == F("gb-found.txt") && caretLine() == 40;
    });

    // ---- a file renamed, a file deleted
    MCGuiStep("gb-c.txt's row brought into view in the tree", [] {
        MCGuiApp().tree->revealPath(F("gb-c.txt"));
    });
    MCGuiWait("its row", 10, [] { return MCGuiFindLabel(MCGuiApp().tree->widget(), "gb-c.txt"); });
    MCGuiStep("a click on it", [] { MCGuiClick(MCGuiFindLabel(MCGuiApp().tree->widget(), "gb-c.txt")); });
    MCGuiWait("c.txt on show, a jump from found.txt", 10, [] {
        return ed()->currentPath() == F("gb-c.txt") && history() == "gb-b.txt:29 gb-found.txt:40 >gb-c.txt:0";
    });
    MCGuiStep("F2 in the tree", [] { MCGuiKey("f2"); });
    MCGuiWait("it asks", 10, [] {
        return MCGuiFindLabel(GTK_WIDGET(MCGuiApp().window), "Rename to") != nullptr;
    });
    MCGuiStep("gb-c-renamed.txt", [] {
        MCGuiKey("ctrl+a");
        MCGuiType("gb-c-renamed.txt\n");
    });
    MCGuiWait("the history follows it", 10, [] {
        return MCGuiFileExists(F("gb-c-renamed.txt")) &&
               history() == "gb-b.txt:29 gb-found.txt:40 >gb-c-renamed.txt:0";
    });
    MCGuiStep("back to found.txt, then forward to the renamed file", [] {
        MCGuiKey("ctrl+alt+-");
    });
    MCGuiWait("gb-found.txt", 10, [] { return ed()->currentPath() == F("gb-found.txt"); });
    MCGuiStep("Ctrl+Shift+-", [] { MCGuiKey("ctrl+shift+-"); });
    MCGuiWait("the renamed file", 10, [] { return ed()->currentPath() == F("gb-c-renamed.txt"); });
    MCGuiStep("d.txt in a tab, then closed (Ctrl+W)", [] {
        MCGuiOpenFileKept(F("gb-d.txt"));
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+w");
    });
    MCGuiWait("closed, the renamed file on show again", 10, [] {
        return ed()->documentFor(F("gb-d.txt")) == nullptr && ed()->currentPath() == F("gb-c-renamed.txt");
    });
    MCGuiStep("d.txt deleted from the disk; Ctrl+Alt+- twice", [] {
        gHistory = history();
        MCGuiNote("history: %s", gHistory.c_str());
        GCHECK(gHistory.find("gb-d.txt") != std::string::npos);
        g_unlink(F("gb-d.txt").c_str());
        MCGuiKey("ctrl+alt+-");
    });
    MCGuiWait("passed over: found.txt", 10, [] {
        return ed()->currentPath() == F("gb-found.txt") && history().find("gb-d.txt") == std::string::npos;
    });
    MCGuiStep("never reopened", [] {
        GCHECK(ed()->documentFor(F("gb-d.txt")) == nullptr);
        GCHECK(!MCGuiFileExists(F("gb-d.txt")));
    });
}
