// SnippetsSuite.cpp — `./build/minicode-guitest snippets`: snippet
// completions with tab stops (linux/PARITY.md item 2.5; Snippet.h,
// SnippetSession.h), the counterpart of the Mac's tests/gui/SnippetsSuite.mm,
// with real keys, against the scripted server (build/fakelsp), which offers
//
//   for ${1:i} in ${2:range(10)}:\n\t$0        (insertTextFormat 2)
//   ${1|alpha,beta,gamma|} = $1                 (a choice, mirrored)
//   ${1:name} = $1                              (a mirror)
//   ${1:x} = $CLIPBOARD                         (a variable)
//
// The server is told snippets are taken. Accepting the first puts it in
// indented as its line (a tab at a line's start becomes the file's four
// spaces) with "i" selected and the stops drawn; one Ctrl+Z takes it all
// back. Typed over, the first stop holds what was typed; Tab goes to
// "range(10)", Shift+Tab back, Tab again to the end of the indented body,
// where the session ends; undoing the typing and then the insertion goes
// back to the text as it was. Escape ends a session, and Tab then indents
// as usual; so does the caret leaving the stop. The choice opens the list
// with its options at both of its places (two carets), Down and Tab take
// one at both, and Tab again goes to the end. A mirror follows what is
// typed at its stop. $CLIPBOARD is minicode's own clipboard's text.
#include "CompletionChecks.h"

#include <string>

using namespace cmpcheck;

namespace {

const char* kServer = "snip";
std::string F() { return MCGuiProjectFile("snip/main.py"); }
const std::string kOriginal = "def f():\n    fo\n    pi\n    na\n    cl\n";
std::string gSettings, gBefore;

std::string script() {
    const std::string items = joined({
        item("for", 15, "a", ", \"filterText\": \"for\", \"insertTextFormat\": 2, \"insertText\": " +
                                 str("for ${1:i} in ${2:range(10)}:\n\t$0")),
        item("pick", 15, "b", ", \"filterText\": \"pick\", \"insertTextFormat\": 2, \"insertText\": " +
                                  str("${1|alpha,beta,gamma|} = $1")),
        item("name", 15, "c", ", \"filterText\": \"name\", \"insertTextFormat\": 2, \"insertText\": " +
                                  str("${1:name} = $1")),
        item("clip", 15, "d", ", \"filterText\": \"clip\", \"insertTextFormat\": 2, \"insertText\": " +
                                  str("${1:x} = $CLIPBOARD"))});
    return "{\"rules\": [{\"method\": \"textDocument/completion\", \"result\": "
           "{\"isIncomplete\": false, \"items\": [" + items + "]}}]}";
}

// The caret after `word` (on its own line), and the list asked for.
void askAfter(const std::string& word) {
    caretAfter(word);
    MCGuiKey("ctrl+space");
}

// Undone, a step at a time, until the text is the original (or nothing is
// left to undo): how many steps it took. Each step but the last still has
// `still` in the text when it is given.
int undoToOriginal(const std::string& still = "") {
    int undos = 0;
    while (text() != kOriginal && undos < 20 && gtk_text_buffer_get_can_undo(buf())) {
        gtk_text_buffer_undo(buf());
        undos++;
        if (text() != kOriginal && !still.empty())
            GCHECK(text().find(still) != std::string::npos);
    }
    return undos;
}

bool rangeIs(const std::pair<size_t, size_t>& r, const std::string& what) {
    const std::string t = text();
    const size_t at = t.find(what);
    return at != std::string::npos && r.first == chars(t, at) && r.second == chars(t, at + what.size());
}

}  // namespace

MC_GUI_SUITE(snippets) {
    MCGuiStep("a Python file and a server that sends snippets", [] {
        MCGuiSetKeyWindow(nullptr);
        ed()->closeAllTabs();
        MCGuiWriteFile(F(), kOriginal);
        gSettings = MCGuiReadFile(g_getenv("MINICODE_SETTINGS"));
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + MCGuiFakeLsp(kServer, script()) + "\n");
        showFile(F());
    });
    MCGuiWait("the server has the file, and the editor the keyboard", 20, [] {
        return showing(F()) && MCGuiFakeLspReceived(kServer, "textDocument/didOpen", "main.py") > 0;
    });
    MCGuiStep("it was told snippets are taken", [] {
        bool told = false;
        for (const Json& m : MCGuiFakeLspMessages(kServer))
            if (m["method"].asString() == "initialize")
                told = m["params"]["capabilities"]["textDocument"]["completion"]["completionItem"]
                        ["snippetSupport"].asBool();
        GCHECK(told);
    });

    // ---- the insertion is one undo step
    MCGuiStep("ask at fo", [] { askAfter("    fo"); });
    MCGuiWait("the list", 10, [] { return shows("for"); });
    MCGuiStep("Tab", [] { MCGuiKey("tab"); });
    MCGuiWait("the snippet in", 10, [] { return snippets()->active(); });
    MCGuiStep("indented as its line, the body a unit deeper, i selected", [] {
        GCHECK_EQ_STR(text(), "def f():\n    for i in range(10):\n        \n    pi\n    na\n    cl\n");
        GCHECK(snippets()->currentStopIndex() == 1);
        GCHECK_EQ_STR(selected(), "i");
    });
    MCGuiWait("its stops drawn", 10, [] { return snippets()->drawnBoxes() >= 3; });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("one Ctrl+Z takes it all back and ends the session", 10, [] {
        return text() == kOriginal && !snippets()->active();
    });

    // ---- typing, Tab, Shift+Tab, Tab to the end
    MCGuiStep("again", [] { askAfter("    fo"); });
    MCGuiWait("the list", 10, [] { return shows("for"); });
    MCGuiStep("Tab, then key typed over the first stop", [] {
        MCGuiKey("tab");
        MCGuiType("key");
    });
    MCGuiWait("key in", 10, [] { return text().find("    for key in range(10):\n") != std::string::npos; });
    MCGuiStep("the first stop holds what was typed", [] {
        GCHECK(snippets()->active() && snippets()->currentStopIndex() == 1);
        const auto r = snippets()->currentRanges();
        GCHECK(r.size() == 1 && rangeIs(r[0], "key"));
        MCGuiKey("tab");
    });
    MCGuiWait("Tab goes to the second stop", 10, [] { return snippets()->currentStopIndex() == 2; });
    MCGuiStep("range(10) selected; d typed over it", [] {
        GCHECK_EQ_STR(selected(), "range(10)");
        MCGuiType("d");
    });
    MCGuiWait("d in", 10, [] { return text().find("    for key in d:\n") != std::string::npos; });
    MCGuiStep("still the second stop", [] {
        GCHECK(snippets()->active() && snippets()->currentStopIndex() == 2);
        const auto r = snippets()->currentRanges();
        const size_t at = chars(text(), text().find(" d:") + 1);
        GCHECK(r.size() == 1 && r[0].first == at && r[0].second == at + 1);
        MCGuiKey("shift+tab");
    });
    MCGuiWait("Shift+Tab goes back to the first", 10, [] { return snippets()->currentStopIndex() == 1; });
    MCGuiStep("key selected; Tab forward again", [] {
        GCHECK_EQ_STR(selected(), "key");
        MCGuiKey("tab");
    });
    MCGuiWait("d selected", 10, [] { return selected() == "d"; });
    MCGuiStep("Tab again", [] { MCGuiKey("tab"); });
    MCGuiWait("the session is over", 10, [] { return !snippets()->active(); });
    MCGuiStep("the caret at the end of the body; typing there is plain typing", [] {
        const std::string expected = "def f():\n    for key in d:\n        \n    pi\n    na\n    cl\n";
        GCHECK_EQ_STR(text(), expected);
        GCHECK(caret() == expected.find("d:\n        ") + 11);
        GCHECK(selected().empty());
        MCGuiType("pass");
    });
    MCGuiWait("pass typed", 10, [] { return text().find("        pass\n") != std::string::npos; });
    MCGuiStep("undo takes the typing back, then the snippet in one step", [] {
        MCGuiNote("%d undo steps", undoToOriginal("    for "));
        GCHECK_EQ_STR(text(), kOriginal);
    });

    // ---- Escape, then Tab indents again
    MCGuiStep("once more", [] { askAfter("    fo"); });
    MCGuiWait("the list", 10, [] { return shows("for"); });
    MCGuiStep("Tab", [] { MCGuiKey("tab"); });
    MCGuiWait("the snippet", 10, [] { return snippets()->active(); });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("Escape ends the session", 10, [] { return !snippets()->active(); });
    MCGuiStep("the caret after for i, Tab", [] {
        caretAfter("for i");
        gBefore = text();
        MCGuiKey("tab");
    });
    MCGuiWait("Tab indents", 10, [] {
        return text().size() > gBefore.size() && text().find("for i ") != std::string::npos;
    });
    MCGuiStep("back to the original", [] {
        GCHECK(text().find("range(10)") != std::string::npos);
        GCHECK(selected().empty());
        undoToOriginal();
        GCHECK_EQ_STR(text(), kOriginal);
    });

    // ---- the caret leaving the stop ends it
    MCGuiStep("and once more", [] { askAfter("    fo"); });
    MCGuiWait("the list", 10, [] { return shows("for"); });
    MCGuiStep("Tab", [] { MCGuiKey("tab"); });
    MCGuiWait("the snippet", 10, [] { return snippets()->active(); });
    MCGuiStep("Ctrl+Home", [] { MCGuiKey("ctrl+Home"); });
    MCGuiWait("the session ends", 10, [] { return !snippets()->active() && caret() == 0; });
    MCGuiStep("and Tab is the editor's", [] {
        MCGuiKey("tab");
    });
    MCGuiWait("an indent at the top", 10, [] { return text().rfind("    def f():", 0) == 0; });
    MCGuiStep("back", [] {
        undoToOriginal();
        GCHECK_EQ_STR(text(), kOriginal);
    });

    // ---- a choice at two places
    MCGuiStep("ask at pi", [] { askAfter("    pi"); });
    MCGuiWait("the list", 10, [] { return shows("pick"); });
    MCGuiStep("Tab", [] { MCGuiKey("tab"); });
    MCGuiWait("the options listed", 10, [] { return snippets()->active() && shows("alpha"); });
    MCGuiStep("the first option at both places, two carets", [] {
        GCHECK(text().find("    alpha = alpha\n") != std::string::npos);
        GCHECK(snippets()->currentStopIndex() == 1);
        GCHECK(ed()->carets()->active() && ed()->carets()->current().size() == 2);
        const std::vector<std::string> labels = lsp()->completionLabels();
        GCHECK(labels == std::vector<std::string>({"alpha", "beta", "gamma"}));
        MCGuiKey("down");
    });
    MCGuiWait("beta highlighted", 10, [] { return shows("beta"); });
    MCGuiStep("Tab", [] { MCGuiKey("tab"); });
    MCGuiWait("beta at both", 10, [] { return text().find("    beta = beta\n") != std::string::npos; });
    MCGuiStep("the list closed, the session on", [] {
        GCHECK(!lsp()->completionVisible());
        GCHECK(snippets()->active());
        MCGuiKey("tab");
    });
    MCGuiWait("Tab goes to the end", 10, [] { return !snippets()->active(); });
    MCGuiStep("one caret, after beta = beta", [] {
        const std::string t = text();
        GCHECK(caret() == t.find("beta = beta") + 11);
        GCHECK(!ed()->carets()->active());
        undoToOriginal();
        GCHECK_EQ_STR(text(), kOriginal);
    });

    // ---- a mirror follows typing
    MCGuiStep("ask at na", [] { askAfter("    na"); });
    MCGuiWait("the list", 10, [] { return shows("name"); });
    MCGuiStep("Tab", [] { MCGuiKey("tab"); });
    MCGuiWait("name = name, both selected", 10, [] {
        return snippets()->active() && text().find("    name = name\n") != std::string::npos;
    });
    MCGuiStep("two carets; key typed", [] {
        GCHECK(ed()->carets()->active() && ed()->carets()->current().size() == 2);
        MCGuiType("key");
    });
    MCGuiWait("the mirror follows", 10, [] { return text().find("    key = key\n") != std::string::npos; });
    MCGuiStep("both places are the stop's", [] {
        const auto r = snippets()->currentRanges();
        GCHECK(r.size() == 2);
        if (r.size() == 2) GCHECK(r[1].first - r[0].first == 6 && r[0].second - r[0].first == 3);
        MCGuiKey("escape");
    });
    MCGuiWait("Escape ends the session", 10, [] { return !snippets()->active(); });
    MCGuiStep("back", [] {
        undoToOriginal();
        GCHECK_EQ_STR(text(), kOriginal);
    });

    // ---- $CLIPBOARD
    MCGuiStep("minicode's clipboard holds hunter2; ask at cl", [] {
        gdk_clipboard_set_text(gtk_widget_get_clipboard(ed()->textView()), "hunter2");
        askAfter("    cl");
    });
    MCGuiWait("the list", 10, [] { return shows("clip"); });
    MCGuiStep("Tab", [] { MCGuiKey("tab"); });
    MCGuiWait("the clipboard's text in the snippet", 10, [] {
        return text().find("    x = hunter2\n") != std::string::npos;
    });
    MCGuiStep("x selected", [] {
        GCHECK_EQ_STR(selected(), "x");
        MCGuiKey("escape");
    });

    // Left as found: the text back, no tabs, the servers off.
    MCGuiStep("the servers off again; the tab closed unasked", [] {
        undoToOriginal();
        GCHECK_EQ_STR(text(), kOriginal);
        MCGuiSetSettings(gSettings);
        ed()->closeAllTabs();
    });
    MCGuiWait("no server left", 10, [] { return LspRunningServerCount() == 0; });
}
