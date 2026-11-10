// OrganizeImportsSuite.cpp — `./build/minicode-guitest organize_imports`:
// Organize Imports and editor.organize-imports-on-save (linux/PARITY.md item
// 2.2; the Mac's tests/gui/OrganizeOnSaveSuite.mm), against two scripted
// servers (build/fakelsp): a main Python server offering the bare
// source.organizeImports, and Ruff beside it (lsp.python.also, a stand-in
// named ruff) offering its own source.organizeImports.ruff, with real keys:
//
//   - Shift+Alt+O (VS Code's Linux key) runs Ruff's, the tool's own, not the
//     main server's bare one, as one undo step, and says so.
//   - With the setting on, Ctrl+S organizes the imports through Ruff, then
//     formats (format.on-save, a stand-in that adds a line) and writes both
//     to disk; Ruff was asked about the text as typed, before the formatter.
//   - With the setting off, Ctrl+S leaves the imports alone and asks
//     nothing; an auto-save with it on never asks either.
//   - A server that never answers holds a save up for 3 s (5 at most), and
//     its late answer changes nothing.
#include "Harness.h"

#include "Editor.h"
#include "FakeLspLog.h"
#include "Lsp.h"

#include <sys/stat.h>

#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
LspSession* lsp() { return MCGuiApp().lsp; }
std::string F(const std::string& name) { return MCGuiProjectFile("organize/" + name); }
std::string bin() { return MCGuiScratch() + "/organizebin"; }
const char* kMain = "orgmain";
const char* kRuff = "orgruff";

const std::string kUnsorted = "import sys\nimport os\n\nprint(sys, os)\n";
const std::string kSorted = "import os\nimport sys\n\nprint(sys, os)\n";

std::string str(const std::string& s) { return Json(s).dump(); }
std::string edit(int l0, int c0, int l1, int c1, const std::string& text) {
    return "{\"range\": {\"start\": {\"line\": " + std::to_string(l0) + ", \"character\": " +
           std::to_string(c0) + "}, \"end\": {\"line\": " + std::to_string(l1) +
           ", \"character\": " + std::to_string(c1) + "}}, \"newText\": " + str(text) + "}";
}

// The main server offers the bare kind, whose edit would leave a mark.
std::string mainScript() {
    return "{\"name\": \"fakelsp\", \"capabilities\": {\"textDocumentSync\": 1,"
           " \"codeActionProvider\": true}, \"rules\": ["
           "{\"method\": \"textDocument/codeAction\", \"when\": {\"paramsContain\":"
           " \"source.organizeImports\"}, \"result\": [{\"title\": \"Organize Imports\","
           " \"kind\": \"source.organizeImports\", \"edit\": {\"changes\": {\"${uri}\": [" +
           edit(0, 0, 0, 0, "# the main server's\n") + "]}}}]},"
           "{\"method\": \"textDocument/codeAction\", \"result\": []}]}";
}

std::string ruffScript(int delayMs) {
    return "{\"name\": \"ruff\", \"capabilities\": {\"textDocumentSync\": 1,"
           " \"codeActionProvider\": {\"codeActionKinds\": [\"quickfix\","
           " \"source.organizeImports.ruff\"]}}, \"rules\": ["
           "{\"method\": \"textDocument/codeAction\", " +
           (delayMs ? "\"delayMs\": " + std::to_string(delayMs) + ", " : std::string()) +
           "\"when\": {\"paramsContain\": \"source.organizeImports\", \"textContains\":"
           " \"import sys\\nimport os\"}, \"result\": [{\"title\": \"Ruff: Organize imports\","
           " \"kind\": \"source.organizeImports.ruff\", \"edit\": {\"changes\": {\"${uri}\": [" +
           edit(0, 0, 2, 0, "import os\nimport sys\n") + "]}}}]},"
           "{\"method\": \"textDocument/codeAction\", \"result\": []}]}";
}

// The stand-in ruff: the scripted server under that name.
std::string ruffCommand(int delayMs) {
    const std::string ruff = bin() + "/ruff";
    MCGuiWriteFile(ruff, "#!/bin/sh\nexec " + MCGuiFakeLsp(kRuff, ruffScript(delayMs)) + " \"$@\"\n");
    chmod(ruff.c_str(), 0755);
    return ruff + " server";
}

// How many organize-imports requests the server named `name` has had.
int organizeAsks(const std::string& name) {
    int n = 0;
    for (const Json& m : MCGuiFakeLspMessages(name))
        if (m["method"].asString() == "textDocument/codeAction" &&
            m["params"].dump().find("source.organizeImports") != std::string::npos)
            n++;
    return n;
}

// The file's text as the server had it when it was first asked to organize.
std::string textWhenAsked(const std::string& server) {
    std::string text;
    for (const Json& m : MCGuiFakeLspMessages(server)) {
        const std::string method = m["method"].asString();
        const Json& p = m["params"];
        if (!MCGuiUriEndsWith(p["textDocument"]["uri"].asString(), "imports.py")) continue;
        if (method == "textDocument/didOpen") text = p["textDocument"]["text"].asString();
        else if (method == "textDocument/didChange")
            text = p["contentChanges"][p["contentChanges"].size() - 1]["text"].asString();
        else if (method == "textDocument/codeAction") return text;
    }
    return "<never asked>";
}

std::string textOf() {
    Document* d = ed()->documentFor(F("imports.py"));
    return d ? d->text() : "<no tab>";
}

bool onShow() { return ed()->currentPath() == F("imports.py") && ed()->isSource(); }

bool editorHasKeyboard() {
    return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)) == ed()->textView();
}

void selectAll() {
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(ed()->buffer(), &a, &b);
    gtk_text_buffer_select_range(ed()->buffer(), &a, &b);
}

std::string gSettings, gBase;
gint64 gSaveAsked = 0;

}  // namespace

MC_GUI_SUITE(organize_imports) {
    MCGuiStep("Python with Ruff beside a main server", [] {
        MCGuiSetKeyWindow(nullptr);
        ed()->closeAllTabs();
        gSettings = MCGuiReadFile(g_getenv("MINICODE_SETTINGS"));
        // A formatter that adds a line, so its run shows in the file.
        const std::string fmt = bin() + "/add-a-line";
        MCGuiWriteFile(fmt, "#!/bin/sh\n/bin/cat\necho '# formatted'\n");
        chmod(fmt.c_str(), 0755);
        gBase = "lsp.enabled = true\nlsp.python = " + MCGuiFakeLsp(kMain, mainScript()) +
                "\nlsp.python.also = " + ruffCommand(0) + "\nformat.python = " + fmt + "\n";
        MCGuiSetSettings(gBase);
        MCGuiWriteFile(F("imports.py"), kUnsorted);
        MCGuiOpenFileKept(F("imports.py"));
        MCGuiFocusKeyWindow();
        ed()->focusActiveDocument();
    });
    MCGuiWait("both servers run", 20, [] {
        std::vector<std::string> names = lsp()->fileServerNames();
        return onShow() && editorHasKeyboard() && names.size() == 2 &&
               MCGuiFakeLspReceived(kRuff, "initialized") > 0 &&
               MCGuiFakeLspReceived(kMain, "initialized") > 0;
    });
    MCGuiStep("Shift+Alt+O", [] { MCGuiKey("shift+alt+o"); });
    MCGuiWait("organized by Ruff's own action, not the main server's", 10, [] {
        return textOf() == kSorted && lsp()->lastNote() == "Organized imports (ruff)";
    });
    MCGuiStep("both were asked; one undo step, unsaved; again", [] {
        GCHECK(organizeAsks(kRuff) == 1 && organizeAsks(kMain) == 1);
        GCHECK(MCGuiReadFile(F("imports.py")) == kUnsorted);
        GCHECK(ed()->active() && ed()->active()->dirty());
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("undone at once", 10, [] { return textOf() == kUnsorted; });
    MCGuiStep("Ctrl+Shift+Z", [] { MCGuiKey("ctrl+shift+z"); });
    MCGuiWait("redone", 10, [] { return textOf() == kSorted; });

    // On save.
    MCGuiStep("organize and format on save; the imports unsorted again; Ctrl+S", [] {
        MCGuiSetSettings(gBase + "editor.organize-imports-on-save = true\nformat.on-save = true\n");
        MCGuiFakeLspClearLog(kRuff);
        selectAll();
        MCGuiType(kUnsorted);
    });
    MCGuiWait("typed", 10, [] { return textOf() == kUnsorted; });
    MCGuiStep("Ctrl+S", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("organized, formatted and written", 15, [] {
        return MCGuiReadFile(F("imports.py")) == kSorted + "# formatted\n";
    });
    MCGuiStep("Ruff was asked before the formatter ran, about the text as typed; clean", [] {
        GCHECK(organizeAsks(kRuff) == 1);
        GCHECK_EQ_STR(textWhenAsked(kRuff), kUnsorted);
        GCHECK_EQ_STR(textOf(), kSorted + "# formatted\n");
        GCHECK(ed()->active() && !ed()->active()->dirty());
        GCHECK_EQ_STR(lsp()->lastNote(), "Organized imports (ruff)");
    });

    // Off: nothing asked.
    MCGuiStep("the setting off, format.on-save still on; unsorted; Ctrl+S", [] {
        MCGuiSetSettings(gBase + "format.on-save = true\n");
        MCGuiFakeLspClearLog(kRuff);
        selectAll();
        MCGuiType(kUnsorted);
    });
    MCGuiWait("typed", 10, [] { return textOf() == kUnsorted; });
    MCGuiStep("Ctrl+S", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("formatted and written as typed", 15, [] {
        return MCGuiReadFile(F("imports.py")) == kUnsorted + "# formatted\n";
    });
    MCGuiStep("no organize request went out", [] { GCHECK(organizeAsks(kRuff) == 0); });

    // Auto-save never asks.
    MCGuiStep("auto-save with the setting on", [] {
        MCGuiSetSettings(gBase + "editor.organize-imports-on-save = true\n"
                                 "editor.auto-save = after-delay\neditor.auto-save-delay = 200\n");
        MCGuiKey("ctrl+End");
        MCGuiType("# more\n");
    });
    MCGuiWait("auto-saved", 10, [] {
        return MCGuiReadFile(F("imports.py")) == kUnsorted + "# formatted\n# more\n";
    });
    MCGuiPause(0.5);   // negative: no organize request follows the auto-save
    MCGuiStep("still no organize request, the imports as typed", [] {
        GCHECK(organizeAsks(kRuff) == 0);
        GCHECK_EQ_STR(textOf(), kUnsorted + "# formatted\n# more\n");
    });

    // A server that never answers in time.
    MCGuiStep("Ruff answering after 4 s, the main server with nothing to offer", [] {
        MCGuiFakeLsp(kMain, "{\"name\": \"fakelsp\", \"capabilities\": {\"textDocumentSync\": 1,"
                            " \"codeActionProvider\": true}, \"rules\": [{\"method\":"
                            " \"textDocument/codeAction\", \"result\": []}]}");
        MCGuiSetSettings(gBase.substr(0, gBase.find("lsp.python.also")) +
                         "lsp.python.also = " + ruffCommand(4000) + "\n" +
                         gBase.substr(gBase.find("format.python")) +
                         "editor.organize-imports-on-save = true\n");
    });
    MCGuiWait("both servers run again", 20, [] {
        return onShow() && lsp()->fileServerNames().size() == 2 &&
               lsp()->statusText().find("starting") == std::string::npos;
    });
    MCGuiStep("a line typed; Ctrl+S", [] {
        MCGuiFocusKeyWindow();
        ed()->focusActiveDocument();
        MCGuiKey("ctrl+End");
        MCGuiType("# slow\n");
    });
    MCGuiWait("typed", 10, [] { return textOf().find("# slow\n") != std::string::npos; });
    MCGuiStep("Ctrl+S", [] {
        gSaveAsked = g_get_monotonic_time();
        MCGuiKey("ctrl+s");
    });
    MCGuiWait("saved anyway", 12, [] {
        const std::string disk = MCGuiReadFile(F("imports.py"));
        return disk.size() >= 7 && disk.compare(disk.size() - 7, 7, "# slow\n") == 0;
    });
    MCGuiStep("when the servers' 3 s were up, not before, and unorganized", [] {
        const double took = (g_get_monotonic_time() - gSaveAsked) / 1e6;
        MCGuiNote("the save took %.1f s", took);
        GCHECK(took >= 2.5 && took < 6);
        GCHECK(MCGuiReadFile(F("imports.py")).rfind("import sys\nimport os\n", 0) == 0);
    });
    MCGuiWait("the late answer has come", 10, [] {
        for (const Json& m : MCGuiFakeLspMessages(kRuff))
            if (m["method"].asString() == "textDocument/codeAction") return true;
        return false;
    });
    MCGuiPause(4.5);   // negative: the slow answer arrives (about 4 s after asking) and is dropped
    MCGuiStep("the late answer changed nothing", [] {
        GCHECK(textOf().rfind("import sys\nimport os\n", 0) == 0);
        GCHECK(ed()->active() && !ed()->active()->dirty());
    });

    MCGuiStep("the settings as they were; the tabs closed unasked", [] {
        MCGuiSetSettings(gSettings);
        ed()->closeAllTabs();
    });
    MCGuiWait("no server left", 10, [] { return LspRunningServerCount() == 0; });
}
