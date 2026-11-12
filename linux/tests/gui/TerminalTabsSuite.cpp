// TerminalTabsSuite.cpp — terminal_tabs: the terminal dock's tabs (PARITY.md
// 4.7, the Mac's terminal_splits without the splits). A real bash in each
// tab, started as the app starts it; a shell is "ready" when the shell
// integration's marks say it is at a prompt, and a command "ran" when the
// marks (or the file it writes) say so, never text read off the screen.
//   - New Terminal (Ctrl+Shift+`) makes a tab with a shell of its own; the
//     key and a click on a tab switch, and app->terminal follows;
//   - a command running in a hidden tab keeps running;
//   - Rename Terminal (a user title wins over a program's), Move Left and
//     Right, a drag along the strip;
//   - closing asks only when a program runs; the last tab closing hides the
//     dock and showing it again starts a shell;
//   - Run File goes to the active terminal, and to a new tab when it is
//     busy; `minicode term` makes a tab with its title and command.
#include "Harness.h"
#include "AppSeam.h"
#include "Editor.h"
#include "ShellIntegration.h"
#include "TabStrip.h"
#include "Terminal.h"
#include "TerminalDock.h"
#include "WindowLayout.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <vte/vte.h>

#include <signal.h>

#include <memory>
#include <string>
#include <vector>

namespace {

TerminalDock* dock() { return MCGuiApp().termDock; }
bool ready(Terminal* t) { return t && t->shellPid() > 0 && t->shellState().known && t->shellState().atPrompt; }
bool busyNow(Terminal* t) { return t && t->shellState().busy; }

VteTerminal* findVte(GtkWidget* root) {
    if (!root) return nullptr;
    if (VTE_IS_TERMINAL(root)) return VTE_TERMINAL(root);
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (VteTerminal* hit = findVte(c)) return hit;
    return nullptr;
}

std::string gHome, gOldShell;
bool gHadShell = false;
Terminal* gA = nullptr;
Terminal* gB = nullptr;
Terminal* gC = nullptr;
int gPidA = 0, gPidB = 0;
long long gStarted = 0;
int gAsked = 0;
bool gAnswerClose = false;
std::function<void(Terminal*, std::function<void()>)> gOldConfirm;

void runIn(Terminal* t, const std::string& line) {
    t->runLine(line, [line](const std::string& why) {
        if (!why.empty()) MCGuiNote("runLine refused %s: %s", line.c_str(), why.c_str());
    });
}

void action(const char* name) {
    GAction* a = g_action_map_lookup_action(G_ACTION_MAP(MCGuiApp().window), name);
    GCHECK(a != nullptr);
    if (a) g_action_activate(a, nullptr);
}

struct CliRun {
    bool done = false;
    int exitCode = -1;
    std::string out, err;
};
std::shared_ptr<CliRun> gCli;

void runCli(const std::vector<std::string>& args) {
    gCli = std::make_shared<CliRun>();
    const std::string exe = MCGuiBuildDir() + "/minicode";
    std::vector<const char*> argv = {exe.c_str()};
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    GError* error = nullptr;
    GSubprocess* p = g_subprocess_newv(
        argv.data(), (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE), &error);
    if (!p) {
        gCli->done = true;
        g_clear_error(&error);
        return;
    }
    g_subprocess_communicate_utf8_async(
        p, nullptr, nullptr,
        +[](GObject* src, GAsyncResult* res, gpointer data) {
            auto* holder = static_cast<std::shared_ptr<CliRun>*>(data);
            char *out = nullptr, *err = nullptr;
            g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, &err, nullptr);
            (*holder)->out = out ? out : "";
            (*holder)->err = err ? err : "";
            (*holder)->exitCode = g_subprocess_get_if_exited(G_SUBPROCESS(src))
                                      ? g_subprocess_get_exit_status(G_SUBPROCESS(src)) : -1;
            g_free(out);
            g_free(err);
            (*holder)->done = true;
            delete holder;
            g_object_unref(src);
        },
        new std::shared_ptr<CliRun>(gCli));
}

}  // namespace

MC_GUI_SUITE(terminal_tabs) {
    MCGuiStep("a bash, and the dock shown", [] {
        GCHECK(dock() != nullptr);
        if (!dock()) return;
        const char* old = g_getenv("SHELL");
        gHadShell = old != nullptr;
        gOldShell = old ? old : "";
        gHome = g_get_home_dir();
        GCHECK(gHome.find(MCGuiScratch()) == 0);   // never the user's own
        g_setenv("SHELL", "/bin/bash", TRUE);
        MCGuiWriteFile(gHome + "/.bashrc", "\n");
        gOldConfirm = dock()->confirmClose;
        // Other suites in the same run leave tabs behind: start from one.
        while (dock()->count() > 1) dock()->close(dock()->count() - 1, false);
        // ... and a terminal whose shell (shell_integration leaves zsh, or an
        // exec'd sh with no marks) is not this suite's bash is replaced.
        if (dock()->at(0)->made()) {
            dock()->newTerminal();
            dock()->close(0, false);
        }
        GCHECK(dock()->count() == 1);
        GCHECK(dock()->strip()->count() == 1);
        if (!MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
        gA = dock()->at(0);
    });
    MCGuiWait("the first shell marks its prompt", 30, [] { return ready(gA); });
    MCGuiStep("New Terminal by its key makes a second tab, active, with a shell of its own", [] {
        gPidA = gA->shellPid();
        GCHECK(MCGuiApp().terminal == gA);
        MCGuiKey("ctrl+shift+`");
    });
    MCGuiWait("two tabs", 10, [] { return dock()->count() == 2; });
    MCGuiWait("the second shell marks its prompt", 30, [] { return ready(dock()->at(1)); });
    MCGuiStep("the new tab is active and app->terminal follows", [] {
        gB = dock()->at(1);
        gPidB = gB->shellPid();
        GCHECK(gB != gA);
        GCHECK(dock()->activeIndex() == 1);
        GCHECK(MCGuiApp().terminal == gB);
        GCHECK(dock()->strip()->count() == 2 && dock()->strip()->activeIndex() == 1);
        GCHECK(gPidA > 0 && gPidB > 0 && gPidA != gPidB);
        GCHECK(kill(gPidA, 0) == 0);   // the hidden shell still lives
    });
    MCGuiStep("a command typed through the active terminal runs in its shell only", [] {
        gStarted = gA->shellState().started;
        runIn(MCGuiApp().terminal, "echo $$ > \"$HOME/pid-b.out\"");
    });
    MCGuiWait("it ran in the second shell", 15, [] {
        return MCGuiFileExists(gHome + "/pid-b.out") && ready(gB) && gB->shellState().started >= 1;
    });
    MCGuiStep("and the pid it wrote is the second tab's", [] {
        GCHECK_EQ_STR(MCGuiReadFile(gHome + "/pid-b.out"), std::to_string(gPidB) + "\n");
        GCHECK(gA->shellState().started == gStarted);
        MCGuiKey("ctrl+alt+pageup");   // Previous Terminal
    });
    MCGuiWait("the first tab is active", 10, [] { return dock()->activeIndex() == 0; });
    MCGuiStep("a command in the first tab, which then goes hidden by a click on the second", [] {
        GCHECK(MCGuiApp().terminal == gA);
        runIn(gA, "sleep 3; echo hidden-done > \"$HOME/hid.out\"");
    });
    MCGuiWait("the first shell is busy with it", 15, [] { return busyNow(gA); });
    MCGuiStep("click the second tab", [] { MCGuiClick(dock()->strip()->titleWidget(1)); });
    MCGuiWait("the second tab shows", 10, [] { return dock()->activeIndex() == 1; });
    MCGuiStep("the hidden command is still running, in the same shell", [] {
        GCHECK(MCGuiApp().terminal == gB);
        GCHECK(busyNow(gA));
        GCHECK(gA->shellPid() == gPidA);
        GCHECK(!MCGuiFileExists(gHome + "/hid.out") || ready(gA));
    });
    MCGuiWait("it finished while hidden", 20, [] { return MCGuiFileExists(gHome + "/hid.out") && ready(gA); });
    MCGuiStep("Next Terminal wraps", [] {
        GCHECK_EQ_STR(MCGuiReadFile(gHome + "/hid.out"), "hidden-done\n");
        MCGuiKey("ctrl+alt+pagedown");
    });
    MCGuiWait("wrapped to the first", 10, [] { return dock()->activeIndex() == 0; });
    MCGuiStep("a user title wins over a program's", [] {
        dock()->rename(0, "build");
        GCHECK_EQ_STR(dock()->strip()->item(0).title, "build");
        VteTerminal* vte = findVte(gB->widget());
        GCHECK(vte != nullptr);
        if (vte) {
            const std::string osc = "\x1b]0;from-program\x07";
            vte_terminal_feed(vte, osc.c_str(), (gssize)osc.size());
        }
        VteTerminal* va = findVte(gA->widget());
        if (va) {
            const std::string osc = "\x1b]0;ignored\x07";
            vte_terminal_feed(va, osc.c_str(), (gssize)osc.size());
        }
    });
    MCGuiWait("the second tab took the program's title", 10, [] {
        return dock()->strip()->item(1).title == "from-program";
    });
    MCGuiStep("the renamed one kept its own; clearing the name gives the program's back", [] {
        GCHECK_EQ_STR(dock()->strip()->item(0).title, "build");
        GCHECK_EQ_STR(gA->title(), "build");
        dock()->rename(0, "");
        GCHECK_EQ_STR(dock()->strip()->item(0).title, "ignored");
        dock()->rename(0, "build");
    });
    MCGuiStep("Move Terminal Right moves the active tab and it stays active", [] {
        action("moveterminalright");
        GCHECK(dock()->at(0) == gB && dock()->at(1) == gA);
        GCHECK(dock()->activeIndex() == 1 && MCGuiApp().terminal == gA);
        GCHECK_EQ_STR(dock()->strip()->item(1).title, "build");
    });
    MCGuiStep("a drag along the strip moves it back", [] {
        dock()->strip()->dropAt(1, 0);
    });
    MCGuiWait("moved by the drop", 10, [] { return dock()->at(0) == gA && dock()->at(1) == gB; });
    MCGuiStep("Move Terminal Left at the edge does nothing", [] {
        GCHECK(dock()->activeIndex() == 0);
        action("moveterminalleft");
        GCHECK(dock()->at(0) == gA);
    });

    // ---- closing
    MCGuiStep("a third tab by the + button; closing an idle one asks nothing", [] {
        gAsked = 0;
        gAnswerClose = false;
        dock()->confirmClose = [](Terminal*, std::function<void()> proceed) {
            gAsked++;
            if (gAnswerClose) proceed();
        };
        MCGuiClick(dock()->newTabButton());
    });
    MCGuiWait("three tabs", 10, [] { return dock()->count() == 3; });
    MCGuiWait("the third shell is ready", 30, [] { return ready(dock()->at(1)); });
    MCGuiStep("closing the idle third by its button", [] {
        gC = dock()->at(1);   // made right of the active (first) tab
        GCHECK(dock()->activeIndex() == 1);
        MCGuiClick(dock()->strip()->closeWidget(1));
    });
    MCGuiWait("closed", 10, [] { return dock()->count() == 2; });
    MCGuiStep("nobody was asked; the others are intact", [] {
        GCHECK(gAsked == 0);
        GCHECK(dock()->at(0) == gA && dock()->at(1) == gB);
        GCHECK(dock()->activeIndex() == 0 && MCGuiApp().terminal == gA);
        GCHECK(gA->shellPid() == gPidA && gB->shellPid() == gPidB);
        runIn(gB, "sleep 30");
    });
    MCGuiWait("the second shell runs sleep", 15, [] { return busyNow(gB); });
    MCGuiStep("closing it asks; Cancel keeps it", [] {
        dock()->close(1);
        GCHECK(gAsked == 1);
        GCHECK(dock()->count() == 2 && busyNow(gB));
    });
    MCGuiStep("closing it again and agreeing closes it", [] {
        gAnswerClose = true;
        dock()->close(1);
        GCHECK(gAsked == 2);
    });
    MCGuiWait("it is gone", 10, [] { return dock()->count() == 1; });
    MCGuiStep("the busy shell was hung up", [] {
        GCHECK(MCGuiApp().terminal == gA);
        GCHECK(dock()->strip()->count() == 1);
    });
    MCGuiWait("its pid is gone", 10, [] { return kill(gPidB, 0) != 0; });

    // ---- Run File and the control channel
    MCGuiStep("Run File goes to the active terminal", [] {
        gStarted = gA->shellState().started;
        MCGuiWriteFile(gHome + "/rf.sh", "echo ran-in-$PPID > \"$HOME/rf.out\"\n");
        GCHECK(MCGuiApp().editor->openFile(gHome + "/rf.sh"));
        action("runfile");
    });
    MCGuiWait("it ran there", 15, [] {
        return MCGuiReadFile(gHome + "/rf.out").find('\n') != std::string::npos && ready(gA) && gA->shellState().started > gStarted;
    });
    MCGuiStep("a new tab was not made; then a program runs and Run File finds the terminal busy", [] {
        GCHECK(dock()->count() == 1);
        GCHECK(MCGuiReadFile(gHome + "/rf.out").find("ran-in-") == 0);
        g_remove((gHome + "/rf.out").c_str());
        runIn(gA, "sleep 5");
    });
    MCGuiWait("busy", 15, [] { return busyNow(gA); });
    MCGuiStep("Run File while busy", [] { action("runfile"); });
    // The line, not the file: the shell makes rf.out (the redirection) before
    // the command writes to it, and CI once read it empty in between.
    MCGuiWait("a new tab ran the file", 30, [] {
        return dock()->count() == 2 && MCGuiReadFile(gHome + "/rf.out").find('\n') != std::string::npos;
    });
    MCGuiStep("the busy terminal was left alone, the new one is active", [] {
        GCHECK(busyNow(gA));
        GCHECK(dock()->activeIndex() == 1 && MCGuiApp().terminal == dock()->at(1));
        GCHECK(dock()->at(1) != gA);
        const std::string got = MCGuiReadFile(gHome + "/rf.out");
        if (got.find("ran-in-" + std::to_string(dock()->at(1)->shellPid()) + "\n") != 0)
            MCGuiNote("rf.out is '%s', the new shell is %d, the busy one %d", got.c_str(),
                      dock()->at(1)->shellPid(), gA->shellPid());
        GCHECK(got.find("ran-in-" + std::to_string(dock()->at(1)->shellPid())) == 0);
    });
    MCGuiWait("sleep 5 finished, in the hidden tab", 20, [] { return ready(gA); });
    MCGuiStep("minicode term makes a tab with its title and runs the command there", [] {
        runCli({"term", "--title", "cli", "--run", "echo termcli > \"$HOME/tc.out\""});
    });
    MCGuiWait("term answered", 30, [] { return gCli->done; });
    MCGuiStep("a third tab, called cli, active", [] {
        if (gCli->exitCode != 0) MCGuiNote("term said: %s", gCli->err.c_str());
        GCHECK(gCli->exitCode == 0);
        GCHECK(dock()->count() == 3);
        GCHECK(dock()->activeIndex() == 2);
        GCHECK_EQ_STR(dock()->strip()->item(2).title, "cli");
    });
    MCGuiWait("the command ran", 20, [] { return MCGuiFileExists(gHome + "/tc.out"); });
    MCGuiStep("term-output reads the active terminal", [] {
        GCHECK_EQ_STR(MCGuiReadFile(gHome + "/tc.out"), "termcli\n");
        runCli({"term-output", std::to_string(MCGuiApp().terminal->controlId()), "--lines", "20"});
    });
    MCGuiWait("term-output answered", 20, [] { return gCli->done; });
    MCGuiStep("it holds the command typed in the third tab", [] {
        GCHECK(gCli->exitCode == 0);
        GCHECK(gCli->out.find("termcli") != std::string::npos);
    });

    // ---- the last tab
    MCGuiStep("closing tabs down to one, unasked", [] {
        dock()->close(2, false);
        dock()->close(1, false);
        GCHECK(dock()->count() == 1);
    });
    MCGuiStep("closing the last hides the dock and leaves a terminal to show", [] {
        GCHECK(MCGuiApp().layout->terminalShown());
        gPidA = dock()->at(0)->shellPid();
        dock()->close(0, false);
        GCHECK(!MCGuiApp().layout->terminalShown());
        GCHECK(dock()->count() == 1);
        GCHECK(MCGuiApp().terminal == dock()->at(0));
        GCHECK(!dock()->at(0)->made());   // no shell until it is shown
    });
    MCGuiWait("the old shell is gone", 10, [] { return kill(gPidA, 0) != 0; });
    MCGuiStep("showing the dock again starts a shell", [] { MCGuiApp().layout->toggleTerminal(); });
    MCGuiWait("a fresh shell is ready", 30, [] { return ready(dock()->at(0)); });
    MCGuiStep("the shell is not the old one; the settings are put back", [] {
        GCHECK(dock()->at(0)->shellPid() != gPidA);
        dock()->confirmClose = gOldConfirm;
        if (gHadShell) g_setenv("SHELL", gOldShell.c_str(), TRUE);
        else g_unsetenv("SHELL");
    });
}
