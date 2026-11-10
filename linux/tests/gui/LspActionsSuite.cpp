// LspActionsSuite.cpp — `./build/minicode-guitest lsp_actions`: rename,
// references and quick fixes (linux/PARITY.md item 2.1, the Mac's
// Lsp+Actions.mm and Lsp+WorkspaceEdit.mm, checked there by
// tests/gui/LspFixesSuite.mm), against the scripted server (build/fakelsp),
// with real keys: VS Code's F2, Shift+F12 and Ctrl+. in the editor, the
// lists' Up, Down, Enter and Escape, and the rename box typed into.
//
//   - Shift+F12 lists the references grouped by file, the one under the
//     caret selected; Down skips a heading; Enter goes to one in another
//     file as a jump Go Back returns from; Escape closes the list.
//   - F2 on a keyword the server will not rename says so; on a name, the
//     box shows the server's placeholder, and the new name renames across
//     a clean tab on show, a tab with unsaved text the server has, a tab
//     the server was never sent whose unsaved change is elsewhere (checked
//     against the disk), and a file with no tab, opened as one, unsaved;
//     nothing is saved, and one Ctrl+Z in each tab takes its part back.
//   - A rename is refused whole, nothing changed anywhere, when a file
//     moved on since the server computed it, when a range no longer reads
//     what the server saw, and when a file it opens shows as a preview,
//     whose tab and the other tab it opened close again.
//   - Ctrl+. asks for the diagnostics on the caret's line (the first, the
//     caret not on it); a fix with an edit goes in, one without is resolved
//     first, and one with a command runs it, and the server's own
//     workspace/applyEdit goes in and is answered applied.
//   - In the terminal none of the three keys is bound, and in the tree F2
//     is Rename File's.
#include "Harness.h"

#include "Editor.h"
#include "FakeLspLog.h"
#include "FileTree.h"
#include "Keys.h"
#include "Lsp.h"
#include "NavHistoryUi.h"
#ifdef MINICODE_ENABLE_TERMINAL
#include "Terminal.h"
#endif

#include <climits>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
LspSession* lsp() { return MCGuiApp().lsp; }
std::string F(const std::string& name) { return MCGuiProjectFile("lspact/" + name); }
const char* kServer = "act";

std::string real(const std::string& p) {
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}

std::string filler(const char* what) {
    std::string s;
    for (int i = 0; i < 20; i++)
        s += std::string("# ") + what + " filler line " + std::to_string(i) + "\n";
    return s;
}

const std::string kA = "def helper():\n    return 1\n\n\nhelper()\n" + filler("a");
const std::string kB = "from a import helper\nhelper()\n" + filler("b");
const std::string kC = "import a\na.helper()\n";
const std::string kD = "x = 1\n";
const std::string kDocs = "Call helper() to get one.\nOther notes.\n";
const std::string kNotes = "# Notes\n\nhelper is here.\n";

// ---- the script, in JSON
std::string str(const std::string& s) { return Json(s).dump(); }
std::string pos(int l, int c) {
    return "{\"line\": " + std::to_string(l) + ", \"character\": " + std::to_string(c) + "}";
}
std::string rng(int l0, int c0, int l1, int c1) {
    return "{\"start\": " + pos(l0, c0) + ", \"end\": " + pos(l1, c1) + "}";
}
std::string edit(int l0, int c0, int l1, int c1, const std::string& text) {
    return "{\"range\": " + rng(l0, c0, l1, c1) + ", \"newText\": " + str(text) + "}";
}
std::string loc(const std::string& uri, int l0, int c0, int l1, int c1) {
    return "{\"uri\": " + str(uri) + ", \"range\": " + rng(l0, c0, l1, c1) + "}";
}
// A file the server has open by its name, else by its absolute path.
std::string open(const char* name) { return std::string("${uri:") + name + "}"; }
std::string disk(const char* name) { return "${uri:" + real(F(name)) + "}"; }
std::string docChange(const std::string& uri, const std::string& version, const std::string& edits) {
    return "{\"textDocument\": {\"uri\": " + str(uri) + ", \"version\": " + version +
           "}, \"edits\": [" + edits + "]}";
}

std::string script() {
    const std::string caps =
        "{\"textDocumentSync\": {\"openClose\": true, \"change\": 2,"
        " \"save\": {\"includeText\": false}},"
        " \"renameProvider\": {\"prepareProvider\": true}, \"referencesProvider\": true,"
        " \"codeActionProvider\": {\"resolveProvider\": true},"
        " \"executeCommandProvider\": {\"commands\": [\"fake.fixAll\"]}, \"hoverProvider\": true}";
    std::vector<std::string> rules;
    // a.py's diagnostic, on its name: what Ctrl+. sends as the context.
    rules.push_back(
        "{\"method\": \"textDocument/didOpen\", \"when\": {\"uriEndsWith\": \"/a.py\"},"
        " \"notify\": [{\"method\": \"textDocument/publishDiagnostics\", \"params\": {\"uri\": "
        "\"${uri}\", \"version\": \"${version}\", \"diagnostics\": [{\"range\": " +
        rng(0, 4, 0, 10) + ", \"severity\": 2, \"message\": \"helper needs a docstring\"}]}}]}");
    // prepareRename: nothing for line 1's keyword, the name on line 0.
    rules.push_back("{\"method\": \"textDocument/prepareRename\", \"when\": {\"paramsContain\":"
                    " \"\\\"line\\\":1\"}, \"result\": null}");
    rules.push_back("{\"method\": \"textDocument/prepareRename\", \"result\": {\"range\": " +
                    rng(0, 4, 0, 10) + ", \"placeholder\": \"helper\"}}");
    // 1. Across a.py (on show), b.py (unsaved, its lines one down), docs.txt
    // (unsaved elsewhere, never sent) and c.py (no tab).
    rules.push_back(
        "{\"method\": \"textDocument/rename\", \"times\": 1, \"result\": {\"changes\": {" +
        str(open("a.py")) + ": [" + edit(0, 4, 0, 10, "assist") + ", " + edit(4, 0, 4, 6, "assist") +
        "], " + str(open("b.py")) + ": [" + edit(1, 14, 1, 20, "assist") + ", " +
        edit(2, 0, 2, 6, "assist") + "], " + str(disk("docs.txt")) + ": [" +
        edit(0, 5, 0, 11, "assist") + "], " + str(disk("c.py")) + ": [" +
        edit(1, 2, 1, 8, "assist") + "]}}}");
    // 2. b.py's part computed on its version before the one it has now.
    rules.push_back(
        "{\"method\": \"textDocument/rename\", \"times\": 1, \"result\": {\"documentChanges\": [" +
        docChange(open("a.py"), "\"${version:a.py}\"", edit(0, 4, 0, 10, "other")) + ", " +
        docChange(open("b.py"), "\"${version:b.py-1}\"", edit(2, 0, 2, 6, "other")) + "]}}");
    // 3. docs.txt's range, which reads "assist" now and "helper" on disk.
    rules.push_back(
        "{\"method\": \"textDocument/rename\", \"times\": 1, \"result\": {\"changes\": {" +
        str(open("a.py")) + ": [" + edit(0, 4, 0, 10, "other") + "], " + str(disk("docs.txt")) +
        ": [" + edit(0, 5, 0, 11, "other") + "]}}}");
    // 4. d.py and notes.md, which have no tabs; notes.md opens as a preview.
    rules.push_back(
        "{\"method\": \"textDocument/rename\", \"times\": 1, \"result\": {\"documentChanges\": [" +
        docChange(open("a.py"), "\"${version:a.py}\"", edit(0, 4, 0, 10, "other")) + ", " +
        docChange(disk("d.py"), "null", edit(0, 0, 0, 1, "y")) + ", " +
        docChange(disk("notes.md"), "null", edit(2, 0, 2, 6, "other")) + "]}}");
    rules.push_back("{\"method\": \"textDocument/references\", \"result\": [" +
                    loc(open("a.py"), 0, 4, 0, 10) + ", " + loc(open("a.py"), 4, 0, 4, 6) + ", " +
                    loc(open("b.py"), 2, 0, 2, 6) + ", " + loc(disk("c.py"), 1, 2, 1, 8) + "]}");
    // Three fixes, only for the diagnostic sent as the context.
    rules.push_back(
        "{\"method\": \"textDocument/codeAction\", \"when\": {\"paramsContain\": "
        "\"helper needs a docstring\"}, \"result\": ["
        "{\"title\": \"Add a comment\", \"kind\": \"quickfix\", \"isPreferred\": true,"
        " \"edit\": {\"changes\": {\"${uri}\": [" + edit(0, 0, 0, 0, "# fixed\n") + "]}}},"
        "{\"title\": \"Add a docstring\", \"kind\": \"quickfix\", \"data\": {\"id\": 2}},"
        "{\"title\": \"Fix all\", \"kind\": \"source.fixAll\", \"command\": {\"title\": "
        "\"Fix all\", \"command\": \"fake.fixAll\", \"arguments\": [\"${uri}\"]}}]}");
    rules.push_back("{\"method\": \"textDocument/codeAction\", \"result\": []}");
    rules.push_back(
        "{\"method\": \"codeAction/resolve\", \"when\": {\"paramsContain\": \"Add a docstring\"},"
        " \"result\": {\"title\": \"Add a docstring\","
        " \"kind\": \"quickfix\", \"edit\": {\"changes\": {" + str(open("a.py")) + ": [" +
        edit(1, 0, 1, 0, "    \"\"\"Help.\"\"\"\n") + "]}}}}");
    rules.push_back(
        "{\"method\": \"workspace/executeCommand\", \"result\": null, \"request\": [{\"method\":"
        " \"workspace/applyEdit\", \"params\": {\"label\": \"Fix all\", \"edit\": {\"changes\": {" +
        str(open("a.py")) + ": [" + edit(0, 0, 0, 0, "# all fixed\n") + "]}}}}]}");
    std::string all;
    for (const std::string& r : rules) all += (all.empty() ? "" : ",\n") + r;
    return "{\"capabilities\": " + caps + ", \"rules\": [" + all + "]}";
}

// ---- the editor
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

bool onShow(const std::string& name) {
    return ed()->currentPath() == F(name) && ed()->isSource();
}

bool editorHasKeyboard() { return gtk_widget_has_focus(ed()->textView()); }

void showTab(const std::string& name) {
    MCGuiOpenFileKept(F(name));
    MCGuiFocusKeyWindow();
    ed()->focusActiveDocument();
}

void caretAt(int line, int column) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line_offset(ed()->buffer(), &it, line, column);
    gtk_text_buffer_place_cursor(ed()->buffer(), &it);
}

int caretLine() {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(ed()->buffer(), &it, gtk_text_buffer_get_insert(ed()->buffer()));
    return gtk_text_iter_get_line(&it);
}

std::string selection() {
    GtkTextIter a, b;
    if (!gtk_text_buffer_get_selection_bounds(ed()->buffer(), &a, &b)) return "";
    char* t = gtk_text_buffer_get_text(ed()->buffer(), &a, &b, TRUE);
    std::string s = t ? t : "";
    g_free(t);
    return s;
}

std::string history() {
    std::string s;
    const std::vector<std::string> e = MCGuiApp().nav->entries();
    for (size_t i = 0; i < e.size(); i++) {
        std::string name = e[i].substr(e[i].find_last_of('/') + 1);
        if (!s.empty()) s += " ";
        s += (i == MCGuiApp().nav->index() ? ">" : "") + name;
    }
    return s;
}

std::string tabNames() {
    std::string s;
    for (Document* d : ed()->documents()) {
        const std::string& p = d->path();
        s += (s.empty() ? "" : " ") + p.substr(p.find_last_of('/') + 1);
    }
    return s;
}

// Every tab's text, and what is on disk, to see that nothing changed.
std::map<std::string, std::string> gBefore;
std::map<std::string, std::string> everything() {
    std::map<std::string, std::string> all;
    for (const char* n : {"a.py", "b.py", "c.py", "d.py", "docs.txt", "notes.md"}) {
        all[std::string("tab ") + n] = textOf(n);
        all[std::string("disk ") + n] = MCGuiReadFile(F(n));
    }
    all["tabs"] = tabNames();
    return all;
}
void nothingChanged() {
    const std::map<std::string, std::string> now = everything();
    for (const auto& [what, text] : gBefore) {
        auto it = now.find(what);
        GCHECK_EQ_STR(it == now.end() ? std::string("<none>") : it->second, text);
    }
}

std::string rowsJoined() {
    std::string s;
    for (const std::string& r : lsp()->pickerRows()) s += (s.empty() ? "" : " | ") + r;
    return s;
}

bool answeredApplied() {
    for (const Json& m : MCGuiFakeLspMessages(kServer))
        if (!m.has("method") && m["result"]["applied"].asBool()) return true;
    return false;
}

bool renameBoxReady() { return lsp()->renameBoxVisible() && lsp()->renameEntry(); }

std::string gSettings;
#ifdef MINICODE_ENABLE_TERMINAL
bool gTerminalWasOpen = false;
#endif

void renameTo(const char* name) {
    MCGuiType(std::string(name) + "\n");
}

}  // namespace

MC_GUI_SUITE(lsp_actions) {
    MCGuiStep("a project, Python's scripted server", [] {
        MCGuiSetKeyWindow(nullptr);
        ed()->closeAllTabs();
        MCGuiWriteFile(F("a.py"), kA);
        MCGuiWriteFile(F("b.py"), kB);
        MCGuiWriteFile(F("c.py"), kC);
        MCGuiWriteFile(F("d.py"), kD);
        MCGuiWriteFile(F("docs.txt"), kDocs);
        MCGuiWriteFile(F("notes.md"), kNotes);
        gSettings = MCGuiReadFile(g_getenv("MINICODE_SETTINGS"));
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + MCGuiFakeLsp(kServer, script()) + "\n");
        showTab("b.py");
    });
    MCGuiWait("b.py with the keyboard", 20, [] { return onShow("b.py") && editorHasKeyboard(); });
    MCGuiStep("a line typed at b.py's top, not saved", [] {
        MCGuiKey("ctrl+Home");
        MCGuiType("# new\n");
    });
    MCGuiWait("typed", 10, [] { return textOf("b.py") == "# new\n" + kB; });
    MCGuiStep("docs.txt, a word typed at the end of its second line", [] { showTab("docs.txt"); });
    MCGuiWait("docs.txt with the keyboard", 10, [] { return onShow("docs.txt") && editorHasKeyboard(); });
    MCGuiStep("Ctrl+End, Left, typed", [] {
        MCGuiKey("ctrl+End");
        MCGuiKey("left");
        MCGuiType(" More.");
    });
    MCGuiWait("typed", 10, [] {
        return textOf("docs.txt") == "Call helper() to get one.\nOther notes. More.\n";
    });
    MCGuiStep("a.py on show", [] { showTab("a.py"); });
    MCGuiWait("the server has a.py and b.py's unsaved text, and a.py's diagnostic", 20, [] {
        std::string a, b;
        return onShow("a.py") && editorHasKeyboard() && MCGuiFakeLspText(kServer, "a.py", &a) &&
               a == kA && MCGuiFakeLspText(kServer, "b.py", &b) && b == "# new\n" + kB &&
               lsp()->statusText().find("1 warning") != std::string::npos;
    });

    // ---- references
    MCGuiStep("the caret on helper; Go Back's history emptied; Shift+F12", [] {
        caretAt(0, 6);
        MCGuiApp().nav->clear();
        MCGuiKey("shift+F12");
    });
    MCGuiWait("the references", 10, [] { return lsp()->pickerKind() == "references"; });
    MCGuiStep("grouped by file, the one under the caret selected, the keyboard kept", [] {
        const std::vector<std::string> rows = lsp()->pickerRows();
        MCGuiNote("rows: %s", rowsJoined().c_str());
        GCHECK(rows.size() == 7);
        if (rows.size() == 7) {
            GCHECK(rows[0].rfind("a.py", 0) == 0 && rows[0].find("2") != std::string::npos);
            GCHECK(rows[1].find("def helper():") != std::string::npos);
            GCHECK(rows[2].find("helper()") != std::string::npos);
            GCHECK(rows[3].rfind("b.py", 0) == 0);
            GCHECK(rows[4].find("3") != std::string::npos && rows[4].find("helper()") != std::string::npos);
            GCHECK(rows[5].rfind("c.py", 0) == 0);
            GCHECK(rows[6].find("a.helper()") != std::string::npos);
        }
        GCHECK(lsp()->pickerSelectedRow() == 1);
        GCHECK(editorHasKeyboard());
        GCHECK_EQ_STR(lsp()->lastNote(), "4 references in 3 files");
        MCGuiKey("down");
    });
    MCGuiWait("the next", 5, [] { return lsp()->pickerSelectedRow() == 2; });
    MCGuiStep("Down again skips b.py's heading", [] { MCGuiKey("down"); });
    MCGuiWait("b.py's reference", 5, [] { return lsp()->pickerSelectedRow() == 4; });
    MCGuiStep("Enter", [] { MCGuiKey("enter"); });
    MCGuiWait("b.py at the reference, its unsaved line counted, a jump", 10, [] {
        return onShow("b.py") && caretLine() == 2 && selection() == "helper" &&
               !lsp()->pickerVisible() && history() == "a.py:0 >b.py:2";
    });
    MCGuiStep("Ctrl+Alt+- goes back", [] { MCGuiKey("ctrl+alt+-"); });
    MCGuiWait("a.py again", 10, [] { return onShow("a.py") && caretLine() == 0; });
    MCGuiStep("the caret on helper, Shift+F12, then Escape", [] {
        MCGuiFocusKeyWindow();
        ed()->focusActiveDocument();
        caretAt(0, 6);
        MCGuiKey("shift+F12");
    });
    MCGuiWait("the list", 10, [] { return lsp()->pickerKind() == "references"; });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("closed, the caret where it was", 10, [] {
        return !lsp()->pickerVisible() && onShow("a.py") && caretLine() == 0 && editorHasKeyboard();
    });

    // ---- rename
    MCGuiStep("F2 on line 1's keyword", [] {
        caretAt(1, 5);
        MCGuiKey("F2");
    });
    MCGuiWait("the server will not", 10, [] { return lsp()->lastNote() == "This can't be renamed"; });
    MCGuiStep("no box", [] {
        GCHECK(!lsp()->renameBoxVisible());
        GCHECK(MCGuiFakeLspReceived(kServer, "textDocument/rename") == 0);
        caretAt(0, 6);
        MCGuiKey("F2");
    });
    MCGuiWait("F2 on helper: the box", 10, renameBoxReady);
    MCGuiStep("it holds the server's placeholder; assist typed over it, Enter", [] {
        GCHECK_EQ_STR(gtk_editable_get_text(GTK_EDITABLE(lsp()->renameEntry())), "helper");
        gBefore = everything();
        renameTo("assist");
    });
    MCGuiWait("renamed", 10, [] { return lsp()->lastNote().rfind("Renamed", 0) == 0; });
    MCGuiStep("in four files, each where the server meant, nothing saved, a.py on show", [] {
        GCHECK_EQ_STR(lsp()->lastNote(), "Renamed 6 occurrences in 4 files");
        GCHECK(onShow("a.py"));
        GCHECK_EQ_STR(textOf("a.py"), "def assist():\n    return 1\n\n\nassist()\n" + filler("a"));
        GCHECK_EQ_STR(textOf("b.py"), "# new\nfrom a import assist\nassist()\n" + filler("b"));
        GCHECK_EQ_STR(textOf("docs.txt"), "Call assist() to get one.\nOther notes. More.\n");
        GCHECK_EQ_STR(textOf("c.py"), "import a\na.assist()\n");
        Document* c = ed()->documentFor(F("c.py"));
        GCHECK(c && c->dirty() && !c->isPreviewTab());
        for (const char* n : {"a.py", "b.py", "c.py", "docs.txt"})
            GCHECK_EQ_STR(MCGuiReadFile(F(n)), gBefore[std::string("disk ") + n]);
        GCHECK(!lsp()->renameBoxVisible());
        GCHECK(editorHasKeyboard());
    });
    MCGuiWait("the server has every renamed Python file", 10, [] {
        std::string a, b, c;
        return MCGuiFakeLspText(kServer, "a.py", &a) && a == textOf("a.py") &&
               MCGuiFakeLspText(kServer, "b.py", &b) && b == textOf("b.py") &&
               MCGuiFakeLspText(kServer, "c.py", &c) && c == textOf("c.py");
    });

    // A file moved on since the server computed its part.
    MCGuiStep("F2 again, other", [] {
        gBefore = everything();
        caretAt(0, 6);
        MCGuiKey("F2");
    });
    MCGuiWait("the box", 10, renameBoxReady);
    MCGuiStep("typed", [] { renameTo("other"); });
    MCGuiWait("refused", 10, [] {
        return lsp()->lastNote() == "b.py changed while the language server worked on it; try again";
    });
    MCGuiStep("nothing changed anywhere", [] { nothingChanged(); });

    // A range that no longer reads what the server saw.
    MCGuiStep("F2 again, other", [] {
        caretAt(0, 6);
        MCGuiKey("F2");
    });
    MCGuiWait("the box", 10, renameBoxReady);
    MCGuiStep("typed", [] { renameTo("other"); });
    MCGuiWait("refused", 10, [] {
        return lsp()->lastNote() ==
               "docs.txt has changes the language server has not seen; save it and try again";
    });
    MCGuiStep("nothing changed anywhere", [] { nothingChanged(); });

    // A file that opens as a preview, after another the edit opened.
    MCGuiStep("F2 again, other", [] {
        caretAt(0, 6);
        MCGuiKey("F2");
    });
    MCGuiWait("the box", 10, renameBoxReady);
    MCGuiStep("typed", [] { renameTo("other"); });
    MCGuiWait("refused", 10, [] {
        return lsp()->lastNote() == "notes.md is not open as text to edit";
    });
    MCGuiStep("nothing changed anywhere, d.py's and notes.md's tabs gone, a.py on show", [] {
        nothingChanged();
        GCHECK(!ed()->documentFor(F("d.py")) && !ed()->documentFor(F("notes.md")));
        GCHECK(onShow("a.py"));
    });

    // One Ctrl+Z a file.
    MCGuiStep("Ctrl+Z in a.py", [] {
        MCGuiFocusKeyWindow();
        ed()->focusActiveDocument();
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("a.py as it was", 10, [] { return textOf("a.py") == kA; });
    MCGuiStep("b.py, Ctrl+Z", [] { showTab("b.py"); });
    MCGuiWait("on show", 10, [] { return onShow("b.py") && editorHasKeyboard(); });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("b.py as it was before the rename, unsaved line and all", 10, [] {
        return textOf("b.py") == "# new\n" + kB;
    });
    MCGuiStep("c.py, Ctrl+Z", [] { showTab("c.py"); });
    MCGuiWait("on show", 10, [] { return onShow("c.py") && editorHasKeyboard(); });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("c.py as on disk, its tab clean", 10, [] {
        Document* c = ed()->documentFor(F("c.py"));
        return textOf("c.py") == kC && c && !c->dirty();
    });
    MCGuiStep("docs.txt, Ctrl+Z", [] { showTab("docs.txt"); });
    MCGuiWait("on show", 10, [] { return onShow("docs.txt") && editorHasKeyboard(); });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("docs.txt as before the rename", 10, [] {
        return textOf("docs.txt") == "Call helper() to get one.\nOther notes. More.\n";
    });

    // ---- quick fixes
    MCGuiStep("a.py, the caret at the start of the diagnostic's line; Ctrl+.", [] {
        showTab("a.py");
        caretAt(0, 0);
        MCGuiKey("ctrl+.");
    });
    MCGuiWait("the fixes", 10, [] { return lsp()->pickerKind() == "fixes"; });
    MCGuiStep("three, the preferred first; asked about the line's diagnostic", [] {
        const std::vector<std::string> rows = lsp()->pickerRows();
        MCGuiNote("fixes: %s", rowsJoined().c_str());
        GCHECK(rows.size() == 3);
        if (rows.size() == 3) {
            GCHECK(rows[0].find("Add a comment") != std::string::npos &&
                   rows[0].find("preferred") != std::string::npos);
            GCHECK(rows[1].find("Add a docstring") != std::string::npos);
            GCHECK(rows[2].find("Fix all") != std::string::npos);
        }
        GCHECK(lsp()->pickerSelectedRow() == 0);
        // The request's range is the diagnostic's, not the caret's.
        bool asked = false;
        for (const Json& m : MCGuiFakeLspMessages(kServer))
            if (m["method"].asString() == "textDocument/codeAction" &&
                m["params"]["range"]["start"]["character"].asInt() == 4 &&
                m["params"]["context"]["diagnostics"].size() == 1)
                asked = true;
        GCHECK(asked);
        MCGuiKey("enter");
    });
    MCGuiWait("the comment goes in", 10, [] { return textOf("a.py") == "# fixed\n" + kA; });
    MCGuiStep("Ctrl+Z; Ctrl+., Down, Enter: the one without an edit", [] {
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("undone", 10, [] { return textOf("a.py") == kA; });
    MCGuiStep("Ctrl+.", [] {
        caretAt(0, 6);
        MCGuiKey("ctrl+.");
    });
    MCGuiWait("the fixes", 10, [] { return lsp()->pickerKind() == "fixes"; });
    MCGuiStep("Down, Enter", [] {
        MCGuiKey("down");
        MCGuiKey("enter");
    });
    MCGuiWait("resolved, then its edit in", 10, [] {
        return textOf("a.py") == "def helper():\n    \"\"\"Help.\"\"\"\n    return 1\n\n\nhelper()\n" +
                                     filler("a");
    });
    MCGuiStep("it was resolved; Ctrl+Z", [] {
        GCHECK(MCGuiFakeLspReceived(kServer, "codeAction/resolve") == 1);
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("undone", 10, [] { return textOf("a.py") == kA; });
    MCGuiStep("Ctrl+., Down twice, Enter: the command", [] {
        caretAt(0, 6);
        MCGuiKey("ctrl+.");
    });
    MCGuiWait("the fixes", 10, [] { return lsp()->pickerKind() == "fixes"; });
    MCGuiStep("Down, Down, Enter", [] {
        MCGuiKey("down");
        MCGuiKey("down");
        MCGuiKey("enter");
    });
    MCGuiWait("the server's workspace/applyEdit goes in", 10, [] {
        return textOf("a.py") == "# all fixed\n" + kA;
    });
    MCGuiWait("and is answered applied", 10, answeredApplied);
    MCGuiStep("the command ran once; Ctrl+Z", [] {
        GCHECK(MCGuiFakeLspReceived(kServer, "workspace/executeCommand") == 1);
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("undone", 10, [] { return textOf("a.py") == kA; });

    // ---- a click on a reference
    MCGuiStep("Shift+F12 on helper again", [] {
        caretAt(0, 6);
        MCGuiKey("shift+F12");
    });
    MCGuiWait("the references, c.py's on screen", 10, [] {
        return lsp()->pickerKind() == "references" && lsp()->pickerRowWidget(6) &&
               MCGuiOnScreen(lsp()->pickerRowWidget(6));
    });
    MCGuiStep("a click on c.py's", [] { MCGuiClick(lsp()->pickerRowWidget(6)); });
    MCGuiWait("c.py at it", 10, [] {
        return onShow("c.py") && caretLine() == 1 && selection() == "helper" &&
               !lsp()->pickerVisible();
    });

    // ---- the keys' places
    MCGuiStep("the tree's selection on a.py, the tree with the keyboard", [] {
        MCGuiApp().tree->revealPath(F("a.py"));
        MCGuiKey("ctrl+0");
    });
    MCGuiWait("the tree has it", 10, [] {
        return Keys::within(gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)),
                            MCGuiApp().tree->widget());
    });
    MCGuiStep("F2 there", [] { MCGuiKey("F2"); });
    MCGuiWait("Rename File asks", 10, [] {
        return MCGuiFindLabel(GTK_WIDGET(MCGuiApp().window), "Rename to") != nullptr;
    });
    MCGuiStep("Escape; Rename Symbol was not asked for", [] {
        MCGuiKey("escape");
        GCHECK(MCGuiFakeLspReceived(kServer, "textDocument/prepareRename") == 5);
    });
    MCGuiWait("the question goes", 10, [] {
        return MCGuiFindLabel(GTK_WIDGET(MCGuiApp().window), "Rename to") == nullptr;
    });
#ifdef MINICODE_ENABLE_TERMINAL
    MCGuiStep("the terminal with the keyboard", [] {
        gTerminalWasOpen = gtk_widget_get_mapped(MCGuiApp().terminal->widget());
        if (gTerminalWasOpen) MCGuiApp().terminal->focus();
        else MCGuiKey("ctrl+`");
    });
    MCGuiWait("it has it", 10, [] {
        return MCGuiApp().terminal->owns(gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)));
    });
    MCGuiStep("F2, Shift+F12 and Ctrl+. are the program's there", [] {
        for (const char* action : {"win.renamesymbol", "win.references", "win.quickfix"}) {
            gchar** accels = gtk_application_get_accels_for_action(MCGuiApplication(), action);
            GCHECK(accels && !accels[0]);
            g_strfreev(accels);
        }
        if (!gTerminalWasOpen) MCGuiKey("ctrl+`");   // as it was
        else gtk_widget_grab_focus(ed()->textView());
    });
#endif

    MCGuiStep("the server off again; the tabs closed unasked", [] {
        MCGuiSetSettings(gSettings);
        ed()->closeAllTabs();
    });
    MCGuiWait("no server left", 10, [] { return LspRunningServerCount() == 0; });
}
