// ShellIntegrationSuite.cpp — shell_integration: the terminals' shell
// integration (PARITY.md 3.1, D10; Terminal.cpp, ShellIntegration.h). A real
// bash in the app's terminal, started as the app starts it (bash --rcfile
// with the script and a nonce), with a .bashrc of the scratch home's that
// sets its own PROMPT_COMMAND and an alias:
//   - at the prompt the terminal's state says so, and `state` reports it;
//   - `sleep 2` is busy with its text, then done with status 0; `false`
//     gives status 1; the user's PROMPT_COMMAND still sees each status and
//     their alias works;
//   - a mark without the nonce, or with another, changes nothing, printed
//     by a command or fed to the terminal directly;
//   - three commands on one line, and three lines in one write, leave the
//     counters consistent;
//   - zsh's half, where zsh is installed (a note otherwise).
// The shell the app starts is $SHELL, set here before the shell is
// (re)started; the old value is put back at the end.

#include "Harness.h"
#include "AppSeam.h"
#include "Json.h"
#include "ShellIntegration.h"
#include "Terminal.h"
#include "WindowLayout.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <vte/vte.h>

#include <memory>
#include <string>

namespace {

VteTerminal* findVte(GtkWidget* root) {
    if (!root) return nullptr;
    if (VTE_IS_TERMINAL(root)) return VTE_TERMINAL(root);
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (VteTerminal* hit = findVte(c)) return hit;
    return nullptr;
}

Terminal* term() { return MCGuiApp().terminal; }
const ShellIntegration::ShellState& sh() { return term()->shellState(); }

// Types a line into the shell, as keys would (Ctrl+U first: an empty line).
void typeLine(const std::string& line) {
    VteTerminal* vte = findVte(term()->widget());
    if (!vte) return;
    const std::string bytes = "\x15" + line + "\r";
    vte_terminal_feed_child(vte, bytes.c_str(), (gssize)bytes.size());
}

struct StateRun {
    bool done = false;
    std::string out;
};
std::shared_ptr<StateRun> gState;

void askState() {
    gState = std::make_shared<StateRun>();
    const std::string exe = MCGuiBuildDir() + "/minicode";
    GError* error = nullptr;
    GSubprocess* p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error, exe.c_str(), "state", nullptr);
    if (!p) {
        gState->done = true;
        g_clear_error(&error);
        return;
    }
    g_subprocess_communicate_utf8_async(
        p, nullptr, nullptr,
        +[](GObject* src, GAsyncResult* res, gpointer data) {
            auto* holder = static_cast<std::shared_ptr<StateRun>*>(data);
            char* out = nullptr;
            g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, nullptr, nullptr);
            (*holder)->out = out ? out : "";
            g_free(out);
            (*holder)->done = true;
            delete holder;
            g_object_unref(src);
        },
        new std::shared_ptr<StateRun>(gState));
}

// This terminal's entry in the answer, null when it is not there.
Json stateEntry() {
    Json j;
    std::string err;
    if (!gState || !Json::parse(gState->out, j, &err)) return Json();
    const Json& terms = j["windows"].elements().empty() ? j["terminals"] : j["windows"].elements()[0]["terminals"];
    for (const Json& t : terms.elements())
        if (t["id"].asInt() == term()->controlId()) return t;
    return Json();
}

std::string gOldShell;
bool gHadShell = false;
int gOldPid = 0;
int gAccepted = 0;   // marks believed (onShellStateChanged)
ShellIntegration::ShellState gBefore;
std::string gHome;

// Starts the terminal's shell as `shell`: the terminal is made if need be,
// else its shell is told to exit and the app starts the new one.
void startShell(const std::string& shell) {
    g_setenv("SHELL", shell.c_str(), TRUE);
    gOldPid = term()->shellPid();
    if (!term()->made()) {
        if (!MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
        term()->focus();
    } else {
        typeLine("exit");
    }
}

bool shellAtPrompt() {
    return term()->shellPid() > 0 && term()->shellPid() != gOldPid && sh().known && sh().atPrompt;
}

bool sameState(const ShellIntegration::ShellState& a, const ShellIntegration::ShellState& b) {
    return a.known == b.known && a.busy == b.busy && a.atPrompt == b.atPrompt && a.command == b.command &&
           a.status == b.status && a.prompts == b.prompts && a.started == b.started && a.done == b.done;
}

std::string forged(const std::string& nonce) {
    return "\x1b]666;vte.ext.minicode.mark=p=99\\sx=99\\sd=0\\ss=77\\sc=forged\\smc=" + nonce + "\x1b\\";
}

}  // namespace

MC_GUI_SUITE(shell_integration) {
    MCGuiStep("a bash with a .bashrc of the scratch home's", [] {
        GCHECK(term() != nullptr);
        if (!term()) return;
        const char* old = g_getenv("SHELL");
        gHadShell = old != nullptr;
        gOldShell = old ? old : "";
        gHome = g_get_home_dir();
        GCHECK(gHome.find(MCGuiScratch()) == 0);   // never the user's own
        MCGuiWriteFile(gHome + "/.bashrc",
                       "PROMPT_COMMAND='echo \"$?\" >> \"$HOME/pc.log\"'\n"
                       "alias mcalias='echo alias-works > \"$HOME/alias.out\"'\n");
        term()->onShellStateChanged = [] { gAccepted++; };
        GCHECK(g_file_test("/bin/bash", G_FILE_TEST_IS_EXECUTABLE));
        startShell("/bin/bash");
    });
    MCGuiWait("the shell marks its first prompt", 30, shellAtPrompt);
    MCGuiStep("at the prompt: known, not busy, no command, no status yet", [] {
        GCHECK(sh().known && sh().atPrompt && !sh().busy);
        GCHECK_EQ_STR(sh().command, "");
        GCHECK(sh().status == -1);
        GCHECK(sh().prompts >= 1 && sh().started == sh().done);
        GCHECK(!Terminal::integrationFolder().empty());
        askState();
    });
    MCGuiWait("state answered (idle)", 15, [] { return gState->done; });
    MCGuiStep("state says not busy, no command", [] {
        Json t = stateEntry();
        GCHECK(t["busy"].isBool() && !t["busy"].asBool());
        GCHECK(t["command"].isNull());
    });

    MCGuiStep("sleep 2", [] { typeLine("sleep 2"); });
    MCGuiWait("busy with its text", 10, [] { return sh().busy && sh().command == "sleep 2"; });
    MCGuiStep("busy: not at the prompt, and state says so", [] {
        GCHECK(!sh().atPrompt && sh().started == sh().done + 1);
        askState();
    });
    MCGuiWait("state answered (busy)", 15, [] { return gState->done; });
    MCGuiStep("state reports busy and the command", [] {
        Json t = stateEntry();
        GCHECK(t["busy"].asBool());
        GCHECK_EQ_STR(t["command"].asString(), "sleep 2");
    });
    MCGuiWait("done afterwards", 15, [] { return sh().atPrompt; });
    MCGuiStep("status 0, no command", [] {
        GCHECK(sh().status == 0 && !sh().busy && sh().started == sh().done);
        GCHECK_EQ_STR(sh().command, "");
        gBefore = sh();
        typeLine("false");
    });
    MCGuiWait("false is done", 10, [] { return sh().atPrompt && sh().prompts > gBefore.prompts; });
    MCGuiStep("status 1", [] {
        GCHECK(sh().status == 1);
        typeLine("mcalias");
    });
    MCGuiWait("the alias ran", 10, [] { return MCGuiFileExists(gHome + "/alias.out") && sh().atPrompt && sh().status == 0; });
    MCGuiStep("the user's alias and PROMPT_COMMAND still work", [] {
        GCHECK_EQ_STR(MCGuiReadFile(gHome + "/alias.out"), "alias-works\n");
        const std::string log = MCGuiReadFile(gHome + "/pc.log");
        GCHECK(log.find("0\n0\n1\n0\n") != std::string::npos);
    });

    MCGuiStep("marks fed to the terminal without the nonce, or with another", [] {
        gBefore = sh();
        gAccepted = 0;
        VteTerminal* vte = findVte(term()->widget());
        for (const std::string& f : {forged(""), forged("0123456789abcdef0123456789abcdef"),
                                     std::string("\x1b]666;vte.ext.minicode.mark=p=99\x1b\\")}) {
            vte_terminal_feed(vte, f.c_str(), (gssize)f.size());
        }
    });
    MCGuiPause(0.5);   // negative check: nothing may arrive
    MCGuiStep("change nothing", [] {
        GCHECK(sameState(sh(), gBefore));
        GCHECK(gAccepted == 0);
        // Printed by a command (a file shown with cat would be the same),
        // half a second in: output in the same read as the start's mark
        // would leave VTE only the later value (D10), hiding the start.
        typeLine("sleep 0.5; printf '\\e]666;vte.ext.minicode.mark=p=99\\\\sx=99\\\\sd=0\\\\ss=77\\\\sc=forged\\\\smc=bad\\e\\\\'; sleep 2");
    });
    MCGuiWait("the printing command runs", 10, [] { return sh().busy; });
    MCGuiPause(1.2);   // while it sleeps, after its forged mark
    MCGuiStep("still its own text, not the forged one", [] {
        VteTerminal* vte = findVte(term()->widget());
        const char* now = vte_terminal_get_termprop_string(vte, ShellIntegration::kVteMarkTermprop, nullptr);
        GCHECK(now && std::string(now).find("c=forged;mc=bad") != std::string::npos);   // VTE has it
        GCHECK(sh().busy && sh().command.find("sleep 0.5; printf") == 0);
        GCHECK(sh().prompts == gBefore.prompts && sh().started == gBefore.started + 1);
    });
    MCGuiWait("it finishes", 10, [] { return sh().atPrompt; });
    MCGuiStep("only the shell's two marks were believed", [] {
        GCHECK(gAccepted == 2);
        GCHECK(sh().status == 0 && sh().prompts == gBefore.prompts + 1);
        gBefore = sh();
        typeLine("true; true; true");
    });
    MCGuiWait("three commands on one line done", 10, [] { return sh().atPrompt && sh().prompts > gBefore.prompts; });
    MCGuiStep("they count as one line", [] {
        GCHECK(sh().prompts == gBefore.prompts + 1 && sh().started == gBefore.started + 1);
        GCHECK(sh().done == sh().started && sh().status == 0);
        gBefore = sh();
        VteTerminal* vte = findVte(term()->widget());
        const std::string three = "\x15true\rfalse\rtrue\r";
        vte_terminal_feed_child(vte, three.c_str(), (gssize)three.size());
    });
    MCGuiWait("three lines in one write done", 10,
              [] { return sh().atPrompt && sh().prompts == gBefore.prompts + 3; });
    MCGuiStep("the counters agree, the last status is the last line's", [] {
        GCHECK(sh().started == gBefore.started + 3 && sh().done == sh().started);
        GCHECK(sh().status == 0 && !sh().busy);
    });

    MCGuiStep("zsh, where it is installed", [] {
        gchar* zsh = g_find_program_in_path("zsh");
        if (!zsh && g_file_test("/bin/zsh", G_FILE_TEST_IS_EXECUTABLE)) zsh = g_strdup("/bin/zsh");
        if (!zsh) {
            MCGuiNote("no zsh here: its half of shell_integration was not run");
            gOldPid = -1;
            return;
        }
        startShell(zsh);
        g_free(zsh);
    });
    MCGuiWait("zsh at its prompt (or no zsh)", 30, [] { return gOldPid == -1 || shellAtPrompt(); });
    MCGuiStep("zsh: sleep 1", [] {
        if (gOldPid == -1) return;
        typeLine("sleep 1");
    });
    MCGuiWait("zsh busy with its text", 10, [] { return gOldPid == -1 || (sh().busy && sh().command == "sleep 1"); });
    MCGuiWait("zsh done", 10, [] { return gOldPid == -1 || sh().atPrompt; });
    MCGuiStep("zsh: status 0, then false", [] {
        if (gOldPid == -1) return;
        GCHECK(sh().status == 0);
        gBefore = sh();
        typeLine("false");
    });
    MCGuiWait("zsh: false done", 10, [] { return gOldPid == -1 || (sh().atPrompt && sh().prompts > gBefore.prompts); });
    MCGuiStep("zsh: status 1; SHELL as it was", [] {
        if (gOldPid != -1) GCHECK(sh().status == 1);
        term()->onShellStateChanged = nullptr;
        if (gHadShell) g_setenv("SHELL", gOldShell.c_str(), TRUE);
        else g_unsetenv("SHELL");
        g_remove((gHome + "/.bashrc").c_str());
    });
}
