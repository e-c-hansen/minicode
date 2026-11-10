// ToolsConsentSuite.cpp — the folder's own Python environment and the
// language servers (`./build/minicode-guitest tools_consent`,
// linux/PARITY.md item 2.3), the counterpart of the Mac's
// tests/gui/ToolsConsentSuite.mm, with real clicks and keys through XTest.
// A .venv in the window's folder holds the only Python server (a script
// that records being run) and a python:
//   - unasked, the bar under the tabs asks about it, nothing from it runs,
//     and Ask Again About This Folder's Tools is off (and not in the
//     palette);
//   - Use It: the server from the .venv runs, the servers get its python,
//     and the answer is kept in $XDG_STATE_HOME/minicode/folder-tools.json;
//   - Ask Again from the palette: the answer goes, the bar asks again, and
//     the servers leave the .venv alone again;
//   - Don't Use It: kept as "deny", nothing from the .venv;
//   - × puts the question off for this launch, through tab switches and an
//     answer given meanwhile, until Ask Again brings it back.
#include "Harness.h"

#include "ConsentBar.h"
#include "Editor.h"
#include "FolderConsent.h"
#include "Lsp.h"
#include "PalettePanel.h"

#include <glib/gstdio.h>
#include <sys/stat.h>

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
LspSession* lsp() { return MCGuiApp().lsp; }
ConsentBar* bar() { return MCGuiApp().consentBar; }
PalettePanel* pal() { return MCGuiApp().palette; }

std::string F(const std::string& name) { return MCGuiProjectFile("tcons/" + name); }
std::string marker() { return MCGuiScratch() + "/tools-consent-server-ran"; }
std::string answer() { return FolderConsent::answer(MCGuiProject()); }

void script(const std::string& path, const std::string& body) {
    MCGuiWriteFile(path, body);
    chmod(path.c_str(), 0755);
}

bool barAsks() {
    return bar()->asking() == "tcons/.venv" && MCGuiOnScreen(bar()->widget()) &&
           MCGuiOnScreen(bar()->allowButton());
}
bool barHidden() { return bar()->asking().empty() && !gtk_widget_get_visible(bar()->widget()); }

bool askAgainEnabled() {
    return g_action_group_get_action_enabled(G_ACTION_GROUP(MCGuiApp().window),
                                             "forgetfoldertools");
}

std::string interpreter(std::string* source = nullptr) {
    return lsp()->serverPythonInterpreter(source);
}

bool fieldHasKeyboard() {
    GtkWidget* f = gtk_root_get_focus(GTK_ROOT(MCGuiApp().window));
    return f && (f == pal()->entry() || gtk_widget_is_ancestor(f, pal()->entry()));
}

std::string selectedLabel() {
    const std::vector<std::string> l = pal()->labels();
    const int i = pal()->selected();
    return i >= 0 && i < (int)l.size() ? l[(size_t)i] : "";
}

const char* kAskAgain = "Edit > Ask Again About This Folder's Tools";

// Ask Again from the palette, with real keys.
void askAgainFromPalette() {
    MCGuiStep("Ctrl+Shift+P", [] { MCGuiKey("ctrl+shift+p"); });
    MCGuiWait("the palette, with the keyboard", 10,
              [] { return pal()->isOpen() && fieldHasKeyboard(); });
    MCGuiStep("\"ask again\"", [] { MCGuiType("ask again"); });
    MCGuiWait("Ask Again ranked first", 10, [] { return selectedLabel() == kAskAgain; });
    MCGuiStep("Enter", [] { MCGuiKey("enter"); });
    MCGuiWait("the palette goes", 10, [] { return !pal()->isOpen(); });
}

bool fileHas(const std::string& path, const std::string& part) {
    return MCGuiReadFile(path).find(part) != std::string::npos;
}

}  // namespace

MC_GUI_SUITE(tools_consent) {
    MCGuiStep("a .venv whose bin holds the only Python server, and no answer yet", [] {
        FolderConsent::forget(MCGuiProject());
        g_remove(marker().c_str());
        script(F(".venv/bin/python"), "#!/bin/sh\nexit 0\n");
        // Records that it ran, and reads what it is sent until minicode
        // closes its input.
        script(F(".venv/bin/mc-tools-server"),
               "#!/bin/sh\n/usr/bin/touch '" + marker() + "'\nexec /bin/cat > /dev/null\n");
        MCGuiWriteFile(F("a.py"), "x = 1\n");
        MCGuiWriteFile(F("notes.txt"), "notes\n");
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = mc-tools-server\n");
        MCGuiOpenFileKept(F("a.py"));
    });
    MCGuiWait("the bar asks about tcons/.venv", 10, barAsks);
    // Negative check: the server must not start meanwhile.
    MCGuiPause(0.5);
    // A note an earlier suite left in the status bar (lsp_actions' "4
    // references in 3 files") shows for its 3 s before the file's own.
    MCGuiWait("the status bar says no Python server was found", 10, [] {
        return lsp()->statusText().find("No Python language server found") != std::string::npos;
    });
    MCGuiStep("nothing from the .venv ran; no answer; Ask Again is off", [] {
        MCGuiNote("the bar: %s", bar()->text().c_str());
        GCHECK(bar()->text().find("This folder has its own Python environment, tcons/.venv") == 0);
        GCHECK(MCGuiFindButton(bar()->widget(), "Use It") != nullptr);
        GCHECK(MCGuiFindButton(bar()->widget(), "Don't Use It") != nullptr);
        // Not offered in the palette while there is nothing to take back.
        GCHECK(!askAgainEnabled());
        GCHECK(!MCGuiFileExists(marker()));
        GCHECK(answer().empty());
        GCHECK(!askAgainEnabled());
        std::string source;
        const std::string interp = interpreter(&source);
        MCGuiNote("the servers' python: \"%s\" (%s)", interp.c_str(), source.c_str());
        GCHECK(interp.find(".venv") == std::string::npos);
        GCHECK(source.find("waits for your answer") != std::string::npos);
        // The server is looked for outside the folder only, so none is found.
        GCHECK(lsp()->statusText().find("No Python language server found") != std::string::npos);
        MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "tools-consent-bar");
    });
    MCGuiStep("Use It, a real click", [] { MCGuiClick(bar()->allowButton()); });
    MCGuiWait("the server from the .venv runs", 10, [] { return MCGuiFileExists(marker()); });
    MCGuiStep("allowed: kept for the folder, the servers get its python, Ask Again is on", [] {
        GCHECK_EQ_STR(answer(), "allow");
        GCHECK(fileHas(FolderConsent::file(), "\"allow\""));
        GCHECK(fileHas(FolderConsent::file(), FolderConsent::keyFor(MCGuiProject())));
        const std::string interp = interpreter();
        MCGuiNote("the servers' python: %s", interp.c_str());
        GCHECK(interp.find("tcons/.venv/bin/python") != std::string::npos);
        GCHECK(barHidden());
        GCHECK(askAgainEnabled());
        g_remove(marker().c_str());
    });

    askAgainFromPalette();
    MCGuiWait("the bar asks again", 10, barAsks);
    MCGuiStep("forgotten: no answer, the servers leave the .venv alone, a note says so", [] {
        GCHECK(answer().empty());
        GCHECK(!fileHas(FolderConsent::file(), "\"allow\""));
        std::string source;
        GCHECK(interpreter(&source).find(".venv") == std::string::npos);
        GCHECK(source.find("waits for your answer") != std::string::npos);
        const std::string status = gtk_label_get_text(GTK_LABEL(MCGuiApp().statusLabel));
        MCGuiNote("the status bar: %s", status.c_str());
        GCHECK(status.find("ask before running this folder's own tools") != std::string::npos);
        GCHECK(!askAgainEnabled());
    });
    MCGuiWait("the server started again without the .venv: none found", 10, [] {
        return lsp()->statusText().find("No Python language server found") != std::string::npos;
    });
    MCGuiStep("Don't Use It, a real click", [] {
        GCHECK(!MCGuiFileExists(marker()));
        MCGuiClick(bar()->denyButton());
    });
    MCGuiWait("the bar goes", 10, barHidden);
    MCGuiPause(0.5);   // negative check: the server must not start
    MCGuiStep("deny kept; nothing from the .venv", [] {
        GCHECK_EQ_STR(answer(), "deny");
        GCHECK(fileHas(FolderConsent::file(), "\"deny\""));
        std::string source;
        GCHECK(interpreter(&source).find(".venv") == std::string::npos);
        GCHECK(source.find("not used") != std::string::npos);
        GCHECK(!MCGuiFileExists(marker()));
        GCHECK(askAgainEnabled());
    });

    askAgainFromPalette();
    MCGuiWait("the bar asks again", 10, barAsks);
    MCGuiStep("×, a real click", [] {
        GCHECK(answer().empty());
        MCGuiClick(bar()->closeButton());
    });
    MCGuiWait("the bar goes", 10, barHidden);
    MCGuiStep("put off, not answered; another tab and back", [] {
        GCHECK(answer().empty());
        GCHECK(FolderConsent::isPutOff(MCGuiProject()));
        MCGuiOpenFileKept(F("notes.txt"));
    });
    MCGuiWait("notes.txt on show", 10, [] { return ed()->currentPath() == F("notes.txt"); });
    MCGuiStep("back to a.py", [] { MCGuiOpenFileKept(F("a.py")); });
    MCGuiWait("a.py on show", 10, [] { return ed()->currentPath() == F("a.py") && ed()->isSource(); });
    MCGuiPause(0.3);   // negative check: the bar must stay away
    MCGuiStep("still put off; then an answer from elsewhere (Format Document's Don't Allow)", [] {
        GCHECK(barHidden());
        FolderConsent::set(MCGuiProject(), false);
        GCHECK(barHidden());
        GCHECK(askAgainEnabled());
    });
    askAgainFromPalette();
    MCGuiWait("taken back, the question put off with × comes back", 10, barAsks);
    MCGuiStep("tidy up: servers off, no .venv, no answer, the tabs closed", [] {
        GCHECK(answer().empty() && !FolderConsent::isPutOff(MCGuiProject()));
        MCGuiSetSettings("lsp.enabled = false\n");
        for (const char* name : {"a.py", "notes.txt"})
            if (Document* d = ed()->documentFor(F(name))) ed()->closeTab(d);
        g_remove(marker().c_str());
        g_remove(F(".venv/bin/python").c_str());
        g_remove(F(".venv/bin/mc-tools-server").c_str());
        g_remove(F(".venv/bin").c_str());
        g_remove(F(".venv").c_str());
        g_remove(F("a.py").c_str());
        g_remove(F("notes.txt").c_str());
        g_remove(MCGuiProjectFile("tcons").c_str());
    });
    MCGuiWait("the bar goes with the file", 10, barHidden);
}
