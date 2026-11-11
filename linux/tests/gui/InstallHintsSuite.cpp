// InstallHintsSuite.cpp — the install offer for a missing language server
// (`./build/minicode-guitest install_hints`, linux/PARITY.md item 2.8), the
// counterpart of the Mac's install bar, with real clicks through XTest.
// $MINICODE_INSTALL_HAVE fakes the installers (uv here). lsp.python names a
// server that is not there yet:
//   - the bar offers Install…; its popover shows the first way's command;
//   - Copy puts that command on the clipboard;
//   - Run in Terminal types it at the shell's prompt and never presses Return;
//   - the server appearing (the window becoming active, a command finishing:
//     lookAgainForMissingServer) takes the bar away with no restart;
//   - × puts the offer off for this launch, Don't Show Again for good (kept
//     in install-hints.json), and the offer comes back for neither.
#include "Harness.h"

#include "ConsentBar.h"
#include "Editor.h"
#include "InstallView.h"
#include "Lsp.h"
#include "Terminal.h"

#include <glib/gstdio.h>
#include <sys/stat.h>
#ifdef MINICODE_ENABLE_TERMINAL
#include <vte/vte.h>
#endif

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
LspSession* lsp() { return MCGuiApp().lsp; }
ConsentBar* bar() { return MCGuiApp().consentBar; }

std::string F(const std::string& name) { return MCGuiProjectFile("ihints/" + name); }

bool offers() {
    return bar()->installing() == "python" && MCGuiOnScreen(bar()->widget()) &&
           MCGuiOnScreen(bar()->installButton());
}
bool hidden() { return bar()->installing().empty() && !gtk_widget_get_visible(bar()->widget()); }

std::string gClip;
bool readClipboard() {
    GdkContentProvider* p = gdk_clipboard_get_content(gtk_widget_get_clipboard(ed()->textView()));
    if (!p) return false;
    GValue v = G_VALUE_INIT;
    g_value_init(&v, G_TYPE_STRING);
    if (gdk_content_provider_get_value(p, &v, nullptr) && g_value_get_string(&v))
        gClip = g_value_get_string(&v);
    g_value_unset(&v);
    return !gClip.empty();
}

#ifdef MINICODE_ENABLE_TERMINAL
VteTerminal* findVte(GtkWidget* root) {
    if (!root) return nullptr;
    if (VTE_IS_TERMINAL(root)) return VTE_TERMINAL(root);
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (VteTerminal* hit = findVte(c)) return hit;
    return nullptr;
}
std::string terminalText() {
    VteTerminal* vte = findVte(MCGuiApp().terminal->widget());
    if (!vte) return "";
    char* text = vte_terminal_get_text_format(vte, VTE_FORMAT_TEXT);
    std::string out = text ? text : "";
    g_free(text);
    return out;
}
#endif

void openPython() {
    MCGuiOpenFileKept(F("a.py"));
}

}  // namespace

MC_GUI_SUITE(install_hints) {
    MCGuiStep("a Python file, lsp.python naming a server that is not installed", [] {
        g_setenv("MINICODE_INSTALL_HAVE", "uv", TRUE);
        ConsentBar::setInstallMuted("python", false);
        ConsentBar::forgetInstallOffers();
        MCGuiWriteFile(F("a.py"), "x = 1\n");
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + F("bin/mc-late-server") + "\n");
        openPython();
    });
    MCGuiWait("the bar offers an install", 10, offers);
    MCGuiStep("the words and buttons", [] {
        MCGuiNote("the bar: %s", bar()->text().c_str());
        GCHECK(bar()->text().find("Python") != std::string::npos);
        GCHECK(MCGuiFindButton(bar()->widget(), "Don't Show Again") != nullptr);
        GCHECK(bar()->installButton() != nullptr);
        GCHECK(lsp()->missingServer() == "python");
        MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "install-bar");
    });
    MCGuiStep("Install…, a real click", [] { MCGuiClick(bar()->installButton()); });
    MCGuiWait("the popover shows the command", 10, [] {
        return bar()->installView() && !bar()->installView()->command().empty() &&
               MCGuiOnScreen(bar()->installView()->copyButton());
    });
    MCGuiStep("it is uv's, and mentions the settings' server", [] {
        MCGuiNote("command: %s", bar()->installView()->command().c_str());
        GCHECK(bar()->installView()->command().find("uv tool install") != std::string::npos);
        GCHECK(bar()->installView()->shownText().find("lsp.python") != std::string::npos);
        gClip.clear();
        MCGuiClick(bar()->installView()->copyButton());
    });
    MCGuiWait("the command is on the clipboard", 5, readClipboard);
    MCGuiStep("Copy copied the command", [] {
        GCHECK(gClip.find("uv tool install") != std::string::npos);
    });
#ifdef MINICODE_ENABLE_TERMINAL
    MCGuiStep("Run in Terminal, a real click", [] {
        MCGuiClick(bar()->installView()->runButton());
    });
    MCGuiWait("the command is typed at the prompt", 30, [] {
        return terminalText().find("uv tool install") != std::string::npos;
    });
    MCGuiPause(0.5);   // negative check: no Return, so nothing runs
    MCGuiStep("typed, not run: the terminal still waits at the line", [] {
        const std::string t = terminalText();
        size_t first = t.find("uv tool install");
        GCHECK(first != std::string::npos);
        // Run, it would print uv's output or an error and a second prompt.
        GCHECK(t.find("uv tool install", first + 1) == std::string::npos);
        GCHECK(t.find("command not found") == std::string::npos);
    });
#endif
    MCGuiStep("the server appears: a script at the settings' path", [] {
        MCGuiWriteFile(F("bin/mc-late-server"), "#!/bin/sh\nexec /bin/cat > /dev/null\n");
        chmod(F("bin/mc-late-server").c_str(), 0755);
        lsp()->lookAgainForMissingServer();
    });
    MCGuiWait("the bar goes with no restart", 10, hidden);
    MCGuiStep("the offer is gone and a note says it was found", [] {
        GCHECK(lsp()->missingServer().empty());
    });

    // ×: put off for this launch.
    MCGuiStep("the server leaves again and the file is opened anew", [] {
        ConsentBar::forgetInstallOffers();
        // A server that stays missing (the other one runs until the settings change).
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + F("bin/mc-never-server") + "\n");
        if (Document* d = ed()->documentFor(F("a.py"))) ed()->closeTab(d);
        openPython();
    });
    MCGuiWait("the bar offers again", 10, offers);
    MCGuiStep("×, a real click", [] { MCGuiClick(bar()->closeButton()); });
    MCGuiWait("the bar goes", 10, hidden);
    MCGuiStep("not offered again this launch, and not muted for good", [] {
        GCHECK(!ConsentBar::installMuted("python"));
        if (Document* d = ed()->documentFor(F("a.py"))) ed()->closeTab(d);
        openPython();
    });
    MCGuiPause(0.8);   // negative check: the offer must not come back
    MCGuiStep("still hidden", [] { GCHECK(hidden()); });

    // Don't Show Again: kept on disk.
    MCGuiStep("a new launch's offers", [] {
        ConsentBar::forgetInstallOffers();
        if (Document* d = ed()->documentFor(F("a.py"))) ed()->closeTab(d);
        openPython();
    });
    MCGuiWait("the bar offers again", 10, offers);
    MCGuiStep("Don't Show Again, a real click", [] {
        MCGuiClick(MCGuiFindButton(bar()->widget(), "Don't Show Again"));
    });
    MCGuiWait("the bar goes", 10, hidden);
    MCGuiStep("muted for good, even for a new launch", [] {
        GCHECK(ConsentBar::installMuted("python"));
        ConsentBar::forgetInstallOffers();
        if (Document* d = ed()->documentFor(F("a.py"))) ed()->closeTab(d);
        openPython();
    });
    MCGuiPause(0.8);   // negative check
    MCGuiStep("tidy up", [] {
        GCHECK(hidden());
        ConsentBar::setInstallMuted("python", false);
        ConsentBar::forgetInstallOffers();
        g_unsetenv("MINICODE_INSTALL_HAVE");
        MCGuiSetSettings("lsp.enabled = false\n");
        if (Document* d = ed()->documentFor(F("a.py"))) ed()->closeTab(d);
        g_remove(F("a.py").c_str());
        g_remove(F("bin/mc-late-server").c_str());
        g_remove(F("bin").c_str());
        g_remove(MCGuiProjectFile("ihints").c_str());
    });
}
