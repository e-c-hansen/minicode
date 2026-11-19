// TerminalKeysSuite.cpp — terminal_keys: readline's Control keys reach the
// shell while the terminal has the keyboard (linux/HANDOFF.md item 108), as
// on the Mac, where mutible's keys are on Command and Control is the
// shell's. On Linux mutible's keys are on Ctrl, so while the terminal has
// it every plain Ctrl key that is not Global gives way
// (Commands::linuxTerminalTakes, Keys.cpp's bound), as VS Code's terminal
// sends keys to the shell unless they are in its commandsToSkipShell. A
// real bash, started as the app starts it:
//   - none of Ctrl+W, A, E, U, K, R, L, D, C, Z, Y, P, N, B and F (Close
//     Tab, Open Recent, Select Line, Quick Open, the sidebar...) is an
//     accelerator then, and each reaches the program as its control byte,
//     read back from what a raw `head` wrote, with no command run;
//   - Ctrl+W at a bash prompt deletes the word before the cursor;
//   - with the editor focused, Ctrl+W is Close Tab again.
#include "Harness.h"
#include "AppSeam.h"
#include "Editor.h"
#include "EnvLock.h"
#include "Terminal.h"
#include "TerminalDock.h"
#include "WindowLayout.h"

#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include <cstdio>
#include <string>

namespace {

TerminalDock* dock() { return MCGuiApp().termDock; }
bool ready(Terminal* t) { return t && t->shellPid() > 0 && t->shellState().known && t->shellState().atPrompt; }
GtkWidget* focusWidget() { return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)); }
bool terminalHasKeyboard() {
    Terminal* t = MCGuiApp().terminal;
    return t && t->owns(focusWidget()) && gtk_window_is_active(MCGuiApp().window);
}

// readline's keys that are also, or were, mutible's on Linux.
const char kLetters[] = "waeukrldczypnbf";
std::string gHome, gOldShell;
bool gHadShell = false;
Terminal* gT = nullptr;
std::string gCommitted;   // what VTE sent to the shell since the last reset
gulong gCommitId = 0;
int gTabs = 0;
bool gSidebar = false;

std::string accelsFor(const std::string& accel) {
    gchar** names = gtk_application_get_actions_for_accel(MCGuiApplication(), accel.c_str());
    std::string out;
    for (gchar** n = names; n && *n; n++) out += (out.empty() ? "" : " ") + std::string(*n);
    g_strfreev(names);
    return out;
}

std::string hex(const std::string& bytes) {
    std::string out;
    char b[4];
    for (unsigned char c : bytes) {
        std::snprintf(b, sizeof b, "%02x ", c);
        out += b;
    }
    return out;
}

}  // namespace

MC_GUI_SUITE(terminal_keys) {
    MCGuiStep("a bash in the only terminal, the dock shown", [] {
        GCHECK(dock() != nullptr);
        if (!dock()) return;
        const char* old = g_getenv("SHELL");
        gHadShell = old != nullptr;
        gOldShell = old ? old : "";
        gHome = g_get_home_dir();
        GCHECK(gHome.find(MCGuiScratch()) == 0);   // never the user's own
        EnvLock::envSet("SHELL", "/bin/bash", TRUE);
        MCGuiWriteFile(gHome + "/.bashrc", "PS1='$ '\n");
        while (dock()->count() > 1) dock()->close(dock()->count() - 1, false);
        if (dock()->at(0)->made()) {   // another suite's shell: a fresh bash
            dock()->newTerminal();
            dock()->close(0, false);
        }
        if (!MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
        gT = dock()->at(0);
        gT->focus();
    });
    MCGuiWait("bash marks its prompt", 30, [] { return ready(gT); });
    MCGuiStep("the keyboard in the terminal; what VTE sends the shell recorded", [] {
        gT->focus();
        gCommitId = g_signal_connect(gT->vteWidget(), "commit",
                                     G_CALLBACK(+[](GtkWidget*, char* text, guint size, gpointer) {
                                         gCommitted.append(text, size);
                                     }),
                                     nullptr);
    });
    MCGuiWait("the terminal has the keyboard", 10, terminalHasKeyboard);
    MCGuiStep("none of readline's Ctrl keys is an accelerator there", [] {
        for (const char* l = kLetters; *l; l++) {
            const std::string accel = std::string("<Control>") + *l;
            const std::string bound = accelsFor(accel);
            if (!bound.empty()) MCGuiNote("%s runs %s in the terminal", accel.c_str(), bound.c_str());
            GCHECK(bound.empty());
        }
        // mutible's ways out of the terminal, Global, still are.
        GCHECK(!accelsFor("<Control>grave").empty());
        GCHECK(!accelsFor("<Control>0").empty());
        gTabs = MCGuiApp().editor->tabCount();
        gSidebar = MCGuiApp().layout->sidebarShown();
    });
    MCGuiStep("a program that writes the bytes it reads, raw", [] {
        g_remove(MCGuiProjectFile("ctrl-keys.bin").c_str());
        MCGuiType("stty raw -echo -iexten; head -c 15 > ctrl-keys.bin; stty sane\n");
    });
    MCGuiWait("it is reading", 20, [] {
        return MCGuiFileExists(MCGuiProjectFile("ctrl-keys.bin")) && gT->shellState().busy;
    });
    MCGuiStep("Ctrl+W, A, E, U, K, R, L, D, C, Z, Y, P, N, B, F pressed", [] {
        gCommitted.clear();
        for (const char* l = kLetters; *l; l++) MCGuiKey(std::string("ctrl+") + *l);
    });
    MCGuiWait("VTE sent all fifteen", 20, [] { return gCommitted.size() >= 15; });
    MCGuiWait("the program read them", 20, [] {
        return MCGuiReadFile(MCGuiProjectFile("ctrl-keys.bin")).size() == 15;
    });
    MCGuiStep("each as its control byte, in order, and no command ran", [] {
        std::string want;
        for (const char* l = kLetters; *l; l++) want += (char)(*l - 'a' + 1);
        const std::string got = MCGuiReadFile(MCGuiProjectFile("ctrl-keys.bin"));
        if (got != want) MCGuiNote("read %s, wanted %s", hex(got).c_str(), hex(want).c_str());
        GCHECK(got == want);
        GCHECK(gCommitted == want);
        GCHECK(MCGuiApp().editor->tabCount() == gTabs);          // Ctrl+W, Ctrl+N
        GCHECK(MCGuiApp().layout->sidebarShown() == gSidebar);   // Ctrl+B
        GCHECK(terminalHasKeyboard());                           // Ctrl+P, Ctrl+F, Ctrl+R
    });
    MCGuiWait("the prompt is back", 20, [] { return ready(gT); });
    MCGuiStep("at the prompt: echo one two, Ctrl+W, three", [] {
        g_remove(MCGuiProjectFile("ctrl-w.txt").c_str());
        MCGuiType("echo one two");
        MCGuiKey("ctrl+w");
        MCGuiType("three > ctrl-w.txt\n");
    });
    MCGuiWait("Ctrl+W took \"two\" back", 20, [] {
        return MCGuiReadFile(MCGuiProjectFile("ctrl-w.txt")) == "one three\n";
    });
    MCGuiStep("a file open, the editor with the keyboard", [] {
        g_signal_handler_disconnect(gT->vteWidget(), gCommitId);
        gCommitId = 0;
        MCGuiWriteFile(MCGuiProjectFile("close-me.txt"), "close me\n");
        MCGuiOpenFileKept(MCGuiProjectFile("close-me.txt"));
    });
    MCGuiWait("it is on show", 10, [] {
        return MCGuiApp().editor->currentPath() == MCGuiProjectFile("close-me.txt");
    });
    MCGuiStep("the editor takes the keyboard", [] {
        gtk_widget_grab_focus(MCGuiApp().editor->textView());
    });
    MCGuiWait("it has it", 10, [] {
        return focusWidget() == MCGuiApp().editor->textView() && !terminalHasKeyboard();
    });
    MCGuiStep("there Ctrl+W is Close Tab", [] {
        GCHECK(!accelsFor("<Control>w").empty());
        gTabs = MCGuiApp().editor->tabCount();
        MCGuiKey("ctrl+w");
    });
    MCGuiWait("the tab closed", 10, [] {
        return MCGuiApp().editor->tabCount() == gTabs - 1 &&
               MCGuiApp().editor->currentPath() != MCGuiProjectFile("close-me.txt");
    });
    MCGuiStep("SHELL as it was", [] {
        if (gHadShell) EnvLock::envSet("SHELL", gOldShell.c_str(), TRUE);
        else EnvLock::envUnset("SHELL");
    });
}
