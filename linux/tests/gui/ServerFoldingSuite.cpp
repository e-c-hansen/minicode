// ServerFoldingSuite.cpp — the language server's folding ranges (`./build/
// minicode-guitest server_folding`, linux/PARITY.md item 2.13), against the
// scripted server (FakeLsp.h): textDocument/foldingRange is asked once the
// file is on show, its answer is nested properly (Fold::sanitize: a range
// crossing another is dropped) and takes the place of the regions minicode
// computes, so a region only the server knows of folds and unfolds with
// View > Folding's keys; typing makes them stale (the computed regions stand
// in) until the server is asked again 0.6 s after typing stops and answers
// for the new text.
#include "FakeLsp.h"
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "Folds.h"
#include "Lsp.h"

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& name) { return MCGuiProjectFile("sfold/" + name); }
DocumentFolds* folds() { return DocumentFolds::of(ed()->buffer()); }
const char* kServer = "sfold";

bool editorHasKeyboard() { return gtk_widget_has_focus(ed()->textView()); }

const std::string kPy =
    "data = [\n"      // 0
    "    1,\n"        // 1
    "    2,\n"        // 2
    "]\n"             // 3
    "# one\n"         // 4
    "# two\n"         // 5
    "x = 1\n"         // 6
    "y = 2\n"         // 7
    "z = 3\n";        // 8

std::string fr(int a, int b, const std::string& kind = "") {
    return "{\"startLine\": " + std::to_string(a) + ", \"endLine\": " + std::to_string(b) +
           (kind.empty() ? "" : ", \"kind\": \"" + kind + "\"") + "}";
}

// Lines 0 to 2, the comments, and x to z, which minicode's own rules do
// not fold; 1 to 7 crosses 0 to 2 and is left out.
std::string script() {
    return "{\"capabilities\": {\"textDocumentSync\": 1, \"foldingRangeProvider\": true},"
           " \"rules\": [{\"method\": \"textDocument/foldingRange\", \"result\": [" +
           fr(0, 2) + ", " + fr(1, 7) + ", " + fr(4, 5, "comment") + ", " + fr(6, 8, "region") +
           "]}]}";
}

std::string regionsText() {
    std::string s;
    for (const Fold::Range& r : folds()->regions())
        s += (s.empty() ? "" : " ") + std::to_string(r.start) + "-" + std::to_string(r.end) +
             (r.kind == Fold::Kind::Comment ? "c" : r.kind == Fold::Kind::Region ? "r" : "");
    return s;
}

void caretAt(int line, int col) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line_offset(ed()->buffer(), &it, line, col);
    gtk_text_buffer_place_cursor(ed()->buffer(), &it);
}

std::string gSettings;
int gAsked = 0;

}  // namespace

MC_GUI_SUITE(server_folding) {
    MCGuiStep("a Python file, its scripted server offering folding ranges", [] {
        MCGuiSetKeyWindow(nullptr);
        ed()->closeAllTabs();
        MCGuiWriteFile(F("srv.py"), kPy);
        gSettings = MCGuiReadFile(g_getenv("MINICODE_SETTINGS"));
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + MCGuiFakeLsp(kServer, script()) +
                         "\n");
        MCGuiOpenFileKept(F("srv.py"));
        MCGuiFocusKeyWindow();
        ed()->focusActiveDocument();
    });
    MCGuiWait("the server asked, its regions taken", 20, [] {
        return ed()->currentPath() == F("srv.py") && folds() && editorHasKeyboard() &&
               MCGuiFakeLspReceived(kServer, "textDocument/foldingRange", "/srv.py") >= 1 &&
               folds()->serverRegionsCurrent();
    });
    MCGuiStep("nested properly, in place of the computed ones", [] {
        GCHECK_EQ_STR(regionsText(), "0-2 4-5c 6-8r");
        GCHECK(folds()->regionsCurrent() && folds()->startsRegion(6) && !folds()->startsRegion(1));
        GCHECK(!folds()->hasClosedFolds());
        caretAt(7, 2);
        MCGuiKey("ctrl+shift+[");
    });
    MCGuiWait("Ctrl+Shift+[ in y folds the server's region of x to z", 5, [] {
        return folds()->startsClosedFold(6) && folds()->hiddenLines() == 2;
    });
    MCGuiStep("Ctrl+Shift+] opens it", [] {
        GCHECK(!ed()->dirty());
        MCGuiKey("ctrl+shift+]");
    });
    MCGuiWait("open", 5, [] { return !folds()->hasClosedFolds(); });

    // ---- typing: stale, then asked again for the new text
    MCGuiStep("a line typed at the end", [] {
        gAsked = MCGuiFakeLspReceived(kServer, "textDocument/foldingRange", "/srv.py");
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(ed()->buffer(), &end);
        gtk_text_buffer_place_cursor(ed()->buffer(), &end);
        MCGuiType("w = 4\n");
    });
    MCGuiWait("typed; the server's regions no longer for the text", 5, [] {
        return MCGuiEditorText() == kPy + "w = 4\n" && !folds()->serverRegionsCurrent();
    });
    MCGuiWait("asked again once typing stopped, and taken for the new text", 10, [] {
        return MCGuiFakeLspReceived(kServer, "textDocument/foldingRange", "/srv.py") > gAsked &&
               folds()->serverRegionsCurrent() && regionsText() == "0-2 4-5c 6-8r";
    });
    MCGuiStep("Fold All folds the server's regions", [] { MCGuiKey("ctrl+alt+0"); });
    MCGuiWait("all three", 5, [] {
        return folds()->startsClosedFold(0) && folds()->startsClosedFold(4) &&
               folds()->startsClosedFold(6) && folds()->hiddenLines() == 5;
    });
    MCGuiStep("the settings as they were; the tab closed unasked", [] {
        MCGuiSetSettings(gSettings);
        ed()->closeAllTabs();
    });
    MCGuiWait("no server left", 10, [] { return LspRunningServerCount() == 0; });
}
