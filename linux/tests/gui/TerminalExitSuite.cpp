// TerminalExitSuite.cpp — terminal_exit: what a terminal does when its shell
// ends (linux/HANDOFF.md item 117; the Mac's terminal_pty, second half; the
// decision is the core's, src/TerminalExit.h). A real bash in each pane on
// the session's host, typed into with real keys; a shell is "ready" when the
// integration's marks say it is at a prompt.
//   - `exit`, typed, closes its terminal tab, nobody asked; the other tab
//     stays, active, with the keyboard, and the dock stays;
//   - Ctrl+D at an empty prompt does the same;
//   - in a split, `exit 1` closes only that pane (a code other than 0
//     closes too, as on the Mac); the other keeps its shell and the keyboard;
//   - `exit` in the last terminal hides the dock, which starts a new shell
//     when it is shown again;
//   - a shell killed (`kill -9 $$`) stays, saying "The shell ended (signal
//     9). Press Return to start a new one.", and Return starts one;
//   - a ~/.bashrc that exits (code 4) before the first prompt: the terminal
//     says so and stays, with no loop, and Return tries again;
//   - terminal.close-on-exit = off: `exit 3` stays with "Process exited
//     (code 3)", Return starts a new shell, `exit` says "Process exited.",
//     and a line run there (runLine) starts a shell and runs it;
//   - terminal.mac-style-keys on: Close Tab (Ctrl+W) closes an ended
//     terminal unasked, and Super+D at an empty prompt (^D) ends the shell
//     and closes the last terminal, the dock with it.
#include "Harness.h"
#include "AppSeam.h"
#include "Keys.h"
#include "Terminal.h"
#include "TerminalDock.h"
#include "WindowLayout.h"

#include <glib/gstdio.h>

#include <signal.h>

#include <functional>
#include <string>

namespace {

TerminalDock* dock() { return MCGuiApp().termDock; }
bool ready(Terminal* t) { return t && t->shellPid() > 0 && t->shellState().known && t->shellState().atPrompt; }
GtkWidget* focusWidget() { return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)); }
bool hasKeyboard(Terminal* t) {
    return t && MCGuiApp().terminal == t && t->owns(focusWidget()) && gtk_window_is_active(MCGuiApp().window);
}
bool says(Terminal* t, const std::string& text) {
    return t && t->recentText(6).find(text) != std::string::npos;
}
bool gone(int pid) { return pid > 0 && kill(pid, 0) != 0; }
std::string settingsPath() {
    const char* p = g_getenv("MUTIBLE_SETTINGS");
    return p ? p : "";
}

std::string gHome, gOldShell, gSettings;
bool gHadShell = false;
Terminal *gA = nullptr, *gB = nullptr;
int gPidA = 0, gPidB = 0;
int gAsked = 0;
std::function<void(Terminal*, std::function<void()>)> gOldConfirm;

// The keyboard into `t`, waited for.
void keyboardTo(Terminal** t, const std::string& what) {
    MCGuiStep(what + " takes the keyboard", [t] { (*t)->focus(); });
    MCGuiWait(what + " has it", 10, [t] { return hasKeyboard(*t); });
}

// A new tab, its bash ready, its pid in *pid.
void newTab(Terminal** t, int* pid, const std::string& what) {
    MCGuiStep("a new tab, " + what, [t] { *t = dock()->newTerminal(); });
    MCGuiWait(what + "'s bash is at its prompt", 30, [t] { return ready(*t); });
    MCGuiStep(what + "'s pid", [t, pid] { *pid = (*t)->shellPid(); });
    keyboardTo(t, what);
}

}  // namespace

MC_GUI_SUITE(terminal_exit) {
    MCGuiStep("a bash, one fresh tab, the dock shown", [] {
        GCHECK(dock() != nullptr);
        if (!dock()) return;
        const char* old = g_getenv("SHELL");
        gHadShell = old != nullptr;
        gOldShell = old ? old : "";
        gHome = g_get_home_dir();
        GCHECK(gHome.find(MCGuiScratch()) == 0);   // never the user's own
        EnvLock::envSet("SHELL", "/bin/bash", TRUE);
        MCGuiWriteFile(gHome + "/.bashrc", "\n");
        gSettings = MCGuiReadFile(settingsPath());
        gOldConfirm = dock()->confirmClose;
        gAsked = 0;
        // Counted, and never closing: nothing here may ask.
        dock()->confirmClose = [](Terminal*, std::function<void()>) { gAsked++; };
        // Earlier suites leave tabs, panes and other shells: one fresh bash.
        dock()->newTerminal();
        while (dock()->count() > 1) dock()->close(0, false);
        GCHECK(dock()->count() == 1);
        if (!MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
        gA = dock()->at(0);
    });
    MCGuiWait("the first bash is at its prompt", 30, [] { return ready(gA); });
    MCGuiStep("its pid", [] { gPidA = gA->shellPid(); });

    // ---- `exit` closes the tab
    newTab(&gB, &gPidB, "the second");
    MCGuiStep("exit, typed", [] {
        GCHECK(dock()->count() == 2 && dock()->activeIndex() == 1);
        MCGuiType("exit\n");
    });
    MCGuiWait("its tab closes", 10, [] { return dock()->count() == 1 && dock()->indexOf(gB) < 0; });
    MCGuiStep("nobody was asked; the first tab is active, the dock still shown", [] {
        gB = nullptr;
        GCHECK(gAsked == 0);
        GCHECK(dock()->at(0) == gA && MCGuiApp().terminal == gA);
        GCHECK(gA->shellPid() == gPidA);
        GCHECK(MCGuiApp().layout->terminalShown());
    });
    MCGuiWait("the first has the keyboard", 10, [] { return hasKeyboard(gA); });
    MCGuiWait("the second's shell is gone", 10, [] { return gone(gPidB); });

    // ---- Ctrl+D at an empty prompt
    newTab(&gB, &gPidB, "a third");
    MCGuiStep("Ctrl+D at its empty prompt", [] { MCGuiKey("ctrl+d"); });
    MCGuiWait("its tab closes", 10, [] { return dock()->count() == 1 && dock()->indexOf(gB) < 0; });
    MCGuiStep("nobody was asked; the first is left", [] {
        gB = nullptr;
        GCHECK(gAsked == 0);
        GCHECK(dock()->at(0) == gA && gA->shellPid() == gPidA);
        GCHECK(MCGuiApp().layout->terminalShown());
    });
    MCGuiWait("the third's shell is gone", 10, [] { return gone(gPidB); });

    // ---- a split: only that pane goes
    MCGuiStep("Split Terminal", [] {
        gB = dock()->split(false);
        GCHECK(gB != nullptr);
    });
    MCGuiWait("the new pane's bash is at its prompt", 30, [] { return ready(gB); });
    MCGuiStep("its pid; two panes in one tab", [] {
        gPidB = gB->shellPid();
        GCHECK(dock()->count() == 1 && dock()->activeGroup()->count() == 2);
    });
    keyboardTo(&gB, "the new pane");
    MCGuiStep("exit 1 in it", [] { MCGuiType("exit 1\n"); });
    MCGuiWait("only that pane closes", 10, [] {
        return dock()->count() == 1 && dock()->activeGroup()->count() == 1 && dock()->indexOf(gB) < 0;
    });
    MCGuiStep("the other pane keeps its shell; nobody was asked", [] {
        gB = nullptr;
        GCHECK(gAsked == 0);
        GCHECK(dock()->at(0) == gA && gA->shellPid() == gPidA && MCGuiApp().terminal == gA);
        GCHECK(MCGuiApp().layout->terminalShown());
    });
    MCGuiWait("the other pane has the keyboard", 10, [] { return hasKeyboard(gA); });
    MCGuiWait("the pane's shell is gone", 10, [] { return gone(gPidB); });

    // ---- the last one: the dock goes
    keyboardTo(&gA, "the last terminal");
    MCGuiStep("exit in the last terminal", [] { MCGuiType("exit\n"); });
    MCGuiWait("the dock hides", 10, [] { return !MCGuiApp().layout->terminalShown(); });
    MCGuiStep("a fresh terminal waits, with no shell until shown", [] {
        GCHECK(gAsked == 0);
        GCHECK(dock()->count() == 1 && dock()->at(0) != gA);
        GCHECK(MCGuiApp().terminal == dock()->at(0));
        GCHECK(!dock()->at(0)->made());
        gA = dock()->at(0);
    });
    MCGuiWait("the last shell is gone", 10, [] { return gone(gPidA); });
    MCGuiStep("showing the dock again", [] { MCGuiApp().layout->toggleTerminal(); });
    MCGuiWait("a new bash is at its prompt", 30, [] { return ready(gA); });
    MCGuiStep("its pid", [] {
        gPidA = gA->shellPid();
        GCHECK(gPidA > 0);
    });

    // ---- killed: the crash is said, the terminal kept
    keyboardTo(&gA, "the terminal");
    MCGuiStep("kill -9 $$", [] { MCGuiType("kill -9 $$\n"); });
    MCGuiWait("the end is said, and it waits", 10, [] {
        return gA->waitingForReturn() && says(gA, "The shell ended (signal 9). Press Return to start a new one.");
    });
    MCGuiStep("still in the dock, no shell", [] {
        GCHECK(dock()->count() == 1 && dock()->at(0) == gA);
        GCHECK(MCGuiApp().layout->terminalShown());
        GCHECK(gA->shellPid() == 0);
        GCHECK(gone(gPidA));
    });
    MCGuiStep("Return", [] { MCGuiKey("enter"); });
    MCGuiWait("a new bash is at its prompt", 30, [] { return ready(gA) && gA->shellPid() != gPidA; });
    MCGuiStep("not waiting any more", [] {
        GCHECK(!gA->waitingForReturn());
        gPidA = gA->shellPid();
    });

    // ---- a startup file that exits: no close, no loop
    MCGuiStep("a ~/.bashrc that exits 4", [] { MCGuiWriteFile(gHome + "/.bashrc", "exit 4\n"); });
    MCGuiStep("a new tab", [] { gB = dock()->newTerminal(); });
    MCGuiWait("it says the shell exited as it started, and waits", 15, [] {
        return gB->waitingForReturn() &&
               says(gB, "The shell exited as it started (code 4). Press Return to try again.");
    });
    MCGuiPause(1.5);   // a negative check: nothing starts again by itself
    MCGuiStep("the tab stayed, and no shell started again", [] {
        GCHECK(dock()->count() == 2 && dock()->indexOf(gB) == 1);
        GCHECK(gB->waitingForReturn() && gB->shellPid() == 0);
        MCGuiWriteFile(gHome + "/.bashrc", "\n");
    });
    keyboardTo(&gB, "the waiting tab");
    MCGuiStep("Return, the file mended", [] { MCGuiKey("enter"); });
    MCGuiWait("its bash is at its prompt", 30, [] { return ready(gB); });
    MCGuiStep("closing it", [] {
        GCHECK(!gB->waitingForReturn());
        gPidB = gB->shellPid();
        dock()->close(1, false);
        gB = nullptr;
        GCHECK(dock()->count() == 1);
    });
    MCGuiWait("its shell is gone", 10, [] { return gone(gPidB); });

    // ---- terminal.close-on-exit = off
    MCGuiStep("terminal.close-on-exit = off", [] {
        MCGuiSetSettings(gSettings + "\nterminal.close-on-exit = off\n");
    });
    keyboardTo(&gA, "the terminal");
    MCGuiStep("exit 3", [] { MCGuiType("exit 3\n"); });
    MCGuiWait("it says so, with the code, and stays", 10, [] {
        return gA->waitingForReturn() &&
               says(gA, "Process exited (code 3). Press Return to start a new shell.");
    });
    MCGuiStep("the same tab, the dock shown", [] {
        GCHECK(dock()->count() == 1 && dock()->at(0) == gA);
        GCHECK(MCGuiApp().layout->terminalShown());
        GCHECK(gA->shellPid() == 0);
    });
    MCGuiStep("Return", [] { MCGuiKey("enter"); });
    MCGuiWait("a new bash is at its prompt", 30, [] { return ready(gA) && gA->shellPid() != gPidA; });
    MCGuiStep("exit", [] {
        gPidA = gA->shellPid();
        MCGuiType("exit\n");
    });
    MCGuiWait("Process exited, no code", 10, [] {
        return gA->waitingForReturn() && says(gA, "Process exited. Press Return to start a new shell.");
    });
    MCGuiStep("a line run there starts a shell and runs it", [] {
        g_remove((gHome + "/again.out").c_str());
        gA->runLine("echo again > \"$HOME/again.out\"", [](const std::string& why) {
            if (!why.empty()) MCGuiNote("runLine refused: %s", why.c_str());
            GCHECK(why.empty());
        });
        GCHECK(!gA->waitingForReturn());
    });
    MCGuiWait("it ran in a new shell", 30, [] {
        return MCGuiReadFile(gHome + "/again.out") == "again\n" && ready(gA);
    });
    MCGuiStep("a new shell", [] {
        GCHECK(gA->shellPid() != gPidA);
        gPidA = gA->shellPid();
    });

    // ---- terminal.mac-style-keys: Close Tab on an ended terminal, Super+D
    MCGuiStep("terminal.mac-style-keys on, close-on-exit still off", [] {
        MCGuiSetSettings(gSettings + "\nterminal.close-on-exit = off\nterminal.mac-style-keys = true\n");
        GCHECK(Keys::terminalMacStyle());
    });
    newTab(&gB, &gPidB, "a second");
    MCGuiStep("exit there", [] { MCGuiType("exit\n"); });
    MCGuiWait("it waits", 10, [] {
        return gB->waitingForReturn() && says(gB, "Process exited. Press Return to start a new shell.");
    });
    MCGuiWait("it has the keyboard", 10, [] { return hasKeyboard(gB); });
    MCGuiStep("Ctrl+W, the Mac's Cmd+W", [] {
        // Only into the terminal: elsewhere it closes a tab, or the window.
        GCHECK(hasKeyboard(gB));
        if (hasKeyboard(gB)) MCGuiKey("ctrl+w");
    });
    MCGuiWait("the ended terminal's tab closes", 10, [] {
        return dock()->count() == 1 && dock()->indexOf(gB) < 0;
    });
    MCGuiStep("nobody was asked; close-on-exit back on", [] {
        gB = nullptr;
        GCHECK(gAsked == 0);
        GCHECK(dock()->at(0) == gA && gA->shellPid() == gPidA);
        MCGuiSetSettings(gSettings + "\nterminal.mac-style-keys = true\n");
    });
    keyboardTo(&gA, "the last terminal");
    MCGuiStep("Super+D at its empty prompt: ^D", [] { MCGuiKey("super+d"); });
    MCGuiWait("the shell ends, the terminal closes and the dock hides", 10, [] {
        return !MCGuiApp().layout->terminalShown() && dock()->indexOf(gA) < 0;
    });
    MCGuiWait("its shell is gone", 10, [] { return gone(gPidA); });

    MCGuiStep("the settings back; the dock shown again", [] {
        GCHECK(gAsked == 0);
        MCGuiSetSettings(gSettings);
        GCHECK(!Keys::terminalMacStyle());
        gA = dock()->at(0);
        MCGuiApp().layout->toggleTerminal();
    });
    MCGuiWait("a fresh bash is at its prompt", 30, [] { return ready(gA); });
    MCGuiStep("the shell and the question put back", [] {
        dock()->confirmClose = gOldConfirm;
        if (gHadShell) EnvLock::envSet("SHELL", gOldShell.c_str(), TRUE);
        else EnvLock::envUnset("SHELL");
    });
}
