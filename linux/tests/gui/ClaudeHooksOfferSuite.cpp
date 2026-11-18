// ClaudeHooksOfferSuite.cpp — the offer of Claude Code's hooks on Linux
// (`./build/mutible-guitest claude_hooks_offer`; the Mac's
// ClaudeHooksOffer in mac/src, over the core's ClaudeHooks):
//   - a command that is not Claude Code puts no bar over the terminals;
//   - Claude Code starting in a terminal (the typed command names it, or the
//     foreground program's binary is called claude) puts the bar up once per
//     launch, with Turn On…, Not Now and ×;
//   - Turn On… asks first (Add the Hooks, Cancel) and Cancel writes nothing;
//     Add the Hooks writes the four hooks into the settings file, says so,
//     and takes the bar away;
//   - Not Now takes the bar away and keeps it away (the install bar's muted
//     list, kept on disk) even in a later launch;
//   - the hooks being in already means no bar.
// Nothing here touches the real ~/.claude: the harness's HOME and
// CLAUDE_CONFIG_DIR are scratch ones, checked before anything is written, and
// the muted list is in the scratch XDG_STATE_HOME.
#include "Harness.h"

#include "AppSeam.h"
#include "ClaudeHooks.h"
#include "ClaudeHooksOffer.h"
#include "ConsentBar.h"
#include "Terminal.h"
#include "TerminalDock.h"
#include "WindowLayout.h"

#include <glib/gstdio.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

TerminalDock* dock() { return MCGuiApp().termDock; }

std::string gOldShell;
bool gHadShell = false;
std::string gSettings, gHome;
Terminal* gT = nullptr;       // the extra tab's terminal, where the commands run
long long gPrompts = 0;       // its prompts before the command
std::string gRan = "(not yet)";

bool atPrompt() {
    return gT && gT->shellPid() > 0 && gT->shellState().known && gT->shellState().atPrompt;
}

// The line goes in at the prompt; the next wait is for it to be typed.
void run(const std::string& line) {
    gRan = "(not yet)";
    gPrompts = gT->shellState().prompts;
    gT->runLine(line, [](const std::string& why) { gRan = why; });
}

bool bar() { return dock() && dock()->notice() != nullptr; }

GtkWidget* button(GtkWidget* root, const char* text) { return MCGuiFindButton(root, text); }

// The bar is up with its buttons laid out: the notice exists a frame or so
// before its buttons can be found and pointed at, and a click then finds no
// widget (CI, d5ea332: "× takes it away" found none).
bool barReady() {
    if (!bar()) return false;
    for (const char* text : {"Turn On…", "Not Now", "×"}) {
        GtkWidget* b = button(dock()->notice(), text);
        if (!b || !gtk_widget_get_mapped(b) || gtk_widget_get_width(b) <= 0) return false;
    }
    return true;
}

// An alert is up with this button showing.
bool questionWith(const char* text) {
    GtkWindow* top = MCGuiTopWindow();
    if (!top || top == MCGuiApp().window) return false;
    GtkWidget* b = button(GTK_WIDGET(top), text);
    return b && gtk_widget_get_width(b) > 0;
}

void answer(const char* text) { MCGuiClick(button(GTK_WIDGET(MCGuiTopWindow()), text)); }

}  // namespace

MC_GUI_SUITE(claude_hooks_offer) {
    MCGuiStep("a bash in a tab of its own, with a program called claude to run", [] {
        const char* old = g_getenv("SHELL");
        gHadShell = old != nullptr;
        gOldShell = old ? old : "";
        gHome = g_get_home_dir();
        GCHECK(gHome.find(MCGuiScratch()) == 0);   // never the user's own
        gSettings = ClaudeHooksOffer::settingsPath();
        GCHECK(gSettings.find(MCGuiScratch()) == 0);
        unlink(gSettings.c_str());
        ConsentBar::setInstallMuted(ClaudeHooksOffer::kKey, false);
        ClaudeHooksOffer::forgetOffer();
        GCHECK(!ClaudeHooksOffer::installed());
        // A script named claude (the typed command names it), and a copy of
        // the shell named claude behind a link of another name (only the
        // foreground program's binary does).
        MCGuiWriteFile(MCGuiProjectFile("fakebin/claude"), "#!/bin/sh\nexec sleep 3\n");
        chmod(MCGuiProjectFile("fakebin/claude").c_str(), 0755);
        MCGuiWriteFile(gHome + "/.bashrc", "\n");
        EnvLock::envSet("SHELL", "/bin/bash", TRUE);
        GCHECK(g_file_test("/bin/sh", G_FILE_TEST_IS_EXECUTABLE));
        if (!MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
        gT = dock()->newTerminal();
        GCHECK(gT != nullptr && !bar());
    });
    MCGuiWait("its shell marks its first prompt", 30, [] { return atPrompt(); });
    MCGuiStep("a copy of sh named claude, and a link to it by another name", [] {
        const std::string dir = MCGuiProjectFile("exe");
        mkdir(dir.c_str(), 0755);
        run("cp /bin/sh '" + dir + "/claude' && ln -sf claude '" + dir + "/ab'");
    });
    MCGuiWait("typed", 10, [] { return gRan != "(not yet)"; });
    MCGuiWait("done", 10, [] { return atPrompt() && gT->shellState().prompts > gPrompts; });
    MCGuiStep("sleep 2, which is not Claude Code", [] {
        GCHECK_EQ_STR(gRan, "");
        run("sleep 2");
    });
    MCGuiWait("it runs", 10, [] { return gT->shellState().busy; });
    // Negative check: the foreground program is looked at one second after the
    // command starts, so time is the condition here.
    MCGuiPause(2.0);
    MCGuiStep("no bar was put up", [] { GCHECK(!bar()); });
    MCGuiWait("sleep is done", 10, [] { return atPrompt() && gT->shellState().prompts > gPrompts; });

    MCGuiStep("the claude script runs", [] { run(MCGuiProjectFile("fakebin/claude")); });
    MCGuiWait("the bar is over the terminals", 10, [] { return barReady(); });
    MCGuiStep("it offers Turn On…, Not Now and ×; nothing is written yet", [] {
        GtkWidget* b = dock()->notice();
        GCHECK(button(b, "Turn On…") != nullptr && button(b, "Not Now") != nullptr && button(b, "×") != nullptr);
        GCHECK(!MCGuiFileExists(gSettings));
    });
    MCGuiClickWhenReady("the bar's Turn On…", [] { return bar() ? button(dock()->notice(), "Turn On…") : nullptr; });
    MCGuiWait("it asks first", 10, [] { return questionWith("Add the Hooks") && questionWith("Cancel"); });
    MCGuiStep("Cancel writes nothing and keeps the bar", [] { answer("Cancel"); });
    MCGuiWait("the question is gone", 10, [] { return MCGuiTopWindow() == MCGuiApp().window; });
    MCGuiStep("no file, and Turn On… again", [] {
        GCHECK(!MCGuiFileExists(gSettings));
        GCHECK(bar());
    });
    MCGuiClickWhenReady("the bar's Turn On…, again", [] { return bar() ? button(dock()->notice(), "Turn On…") : nullptr; });
    MCGuiWait("it asks again", 10, [] { return questionWith("Add the Hooks"); });
    MCGuiStep("Add the Hooks", [] { answer("Add the Hooks"); });
    MCGuiWait("it says where it wrote", 10, [] { return questionWith("OK"); });
    MCGuiStep("the four hooks are in the scratch settings file", [] {
        const std::string text = MCGuiReadFile(gSettings);
        for (const std::string& e : ClaudeHooks::events()) GCHECK(text.find("\"" + e + "\"") != std::string::npos);
        GCHECK(text.find("notify --from-hook") != std::string::npos);
        GCHECK(ClaudeHooksOffer::installed());
        answer("OK");
    });
    MCGuiWait("the bar is gone", 10, [] { return !bar() && MCGuiTopWindow() == MCGuiApp().window; });
    MCGuiWait("the claude script is done", 10, [] { return atPrompt() && gT->shellState().prompts > gPrompts; });

    MCGuiStep("with the hooks in, a later start offers nothing", [] {
        ClaudeHooksOffer::forgetOffer();
        run(MCGuiProjectFile("fakebin/claude"));
    });
    MCGuiWait("it runs", 10, [] { return gT->shellState().busy; });
    MCGuiPause(1.0);   // negative check: the offer would come with the command
    MCGuiStep("no bar", [] { GCHECK(!bar()); });
    MCGuiWait("done", 10, [] { return atPrompt() && gT->shellState().prompts > gPrompts; });

    MCGuiStep("without them, Not Now: the bar comes up again", [] {
        unlink(gSettings.c_str());
        ClaudeHooksOffer::forgetOffer();
        run(MCGuiProjectFile("fakebin/claude"));
    });
    MCGuiWait("the bar", 10, [] { return barReady(); });
    MCGuiClickWhenReady("the bar's Not Now", [] { return bar() ? button(dock()->notice(), "Not Now") : nullptr; });
    MCGuiWait("the bar is gone", 10, [] { return !bar(); });
    MCGuiStep("it is kept, on disk, and nothing was written", [] {
        GCHECK(ConsentBar::installMuted(ClaudeHooksOffer::kKey));
        GCHECK(!MCGuiFileExists(gSettings));
        ClaudeHooksOffer::forgetOffer();   // a later launch
    });
    MCGuiWait("the script is done", 10, [] { return atPrompt() && gT->shellState().prompts > gPrompts; });
    MCGuiStep("a start in a later launch", [] { run(MCGuiProjectFile("fakebin/claude")); });
    MCGuiWait("it runs (muted)", 10, [] { return gT->shellState().busy; });
    MCGuiPause(1.0);   // negative check
    MCGuiStep("no bar: it stays away", [] { GCHECK(!bar()); });
    MCGuiWait("done (muted)", 10, [] { return atPrompt() && gT->shellState().prompts > gPrompts; });

    MCGuiStep("a program that is claude by its binary, run by another name", [] {
        ConsentBar::setInstallMuted(ClaudeHooksOffer::kKey, false);
        ClaudeHooksOffer::forgetOffer();
        GCHECK(!bar());
        run("'" + MCGuiProjectFile("exe/ab") + "' -c 'sleep 4; :'");
    });
    MCGuiWait("the bar, from the foreground program", 15, [] { return barReady(); });
    MCGuiStep("× takes it away and writes nothing", [] {
        GCHECK(!MCGuiFileExists(gSettings));
    });
    MCGuiClickWhenReady("the bar's ×", [] { return bar() ? button(dock()->notice(), "×") : nullptr; });
    MCGuiWait("gone", 10, [] { return !bar(); });
    MCGuiWait("the program is done", 15, [] { return atPrompt() && gT->shellState().prompts > gPrompts; });
    MCGuiStep("the tab closes and the settings are put back", [] {
        ConsentBar::setInstallMuted(ClaudeHooksOffer::kKey, false);
        ClaudeHooksOffer::forgetOffer();
        unlink(gSettings.c_str());
        const int at = dock()->indexOf(gT);
        if (at >= 0) dock()->close(at, false);
        GCHECK(dock()->count() >= 1);
        if (gHadShell) EnvLock::envSet("SHELL", gOldShell.c_str(), TRUE);
        else EnvLock::envUnset("SHELL");
        g_remove((gHome + "/.bashrc").c_str());
        // The questions above took the X focus and gave it to no one when
        // they closed; without this the next suite's window is not active
        // and nothing in it has the keyboard (format_offer, run right
        // after, timed out on "the editor has the keyboard").
        MCGuiFocusKeyWindow();
    });
}
