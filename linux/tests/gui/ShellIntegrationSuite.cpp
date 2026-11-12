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
//   - part two (term, term-output, Run File): `term --run 'echo hi'` at the
//     prompt runs visibly and `term-output` shows hi; while `sleep 3` runs
//     it is refused with the reason and nothing is typed; Run File while
//     busy says so in the status bar and types nothing, and at the prompt
//     runs the file; after `exec sh` (no more marks) it is refused;
//   - the finishing mark calls onCommandFinished (no polling).
// The shell the app starts is $SHELL, set here before the shell is
// (re)started; the old value is put back at the end.

#include "Harness.h"
#include "AppSeam.h"
#include "Editor.h"
#include "Json.h"
#include "ShellIntegration.h"
#include "Terminal.h"
#include "TerminalDock.h"
#include "WindowLayout.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <vte/vte.h>

#include <functional>
#include <memory>
#include <unistd.h>
#include <string>
#include <vector>

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

// A client run with its output and exit status.
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

std::string statusText() { return gtk_label_get_text(GTK_LABEL(MCGuiApp().statusLabel)); }
int gFinished = 0;   // onCommandFinished calls

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

Terminal* gOrig = nullptr;   // the terminal part two began in
std::function<void(Terminal*)> gOldActive;

// The first terminal on show again, the tabs the commands made closed.
void backToOriginal() {
    TerminalDock* d = MCGuiApp().termDock;
    const int at = d->indexOf(gOrig);
    if (at >= 0) d->select(at, false);
    for (int i = d->count() - 1; i >= 0; i--)
        if (d->at(i) != gOrig) d->close(i, false);
}

bool shellAtPrompt() {
    return term()->shellPid() > 0 && term()->shellPid() != gOldPid && sh().known && sh().atPrompt;
}

bool sameState(const ShellIntegration::ShellState& a, const ShellIntegration::ShellState& b) {
    return a.known == b.known && a.busy == b.busy && a.atPrompt == b.atPrompt && a.command == b.command &&
           a.status == b.status && a.prompts == b.prompts && a.started == b.started && a.done == b.done;
}

std::string vteText() {
    VteTerminal* vte = findVte(term()->widget());
    char* t = vte ? vte_terminal_get_text_format(vte, VTE_FORMAT_TEXT) : nullptr;
    std::string r = t ? t : "";
    g_free(t);
    return r;
}

// True when every occurrence of `word` ends its line (never `LEFT echo RIGHT`,
// never `LEFTecho`): the half-typed line stayed apart from ours.
bool alwaysEndsLine(const std::string& text, const std::string& word) {
    for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
        size_t e = at + word.size();
        while (e < text.size() && text[e] == ' ') e++;
        if (e < text.size() && text[e] != '\n') return false;
    }
    return true;
}

bool hasLine(const std::string& text, const std::string& line) {
    return text.find("\n" + line + "\n") != std::string::npos || text.compare(0, line.size() + 1, line + "\n") == 0;
}

// The text of the last non-empty line.
std::string lastLine(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    const size_t nl = text.rfind('\n');
    return nl == std::string::npos ? text : text.substr(nl + 1);
}

bool endsWith(const std::string& s, const std::string& t) {
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
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
                       "bind '\"\\C-x\\C-t\": \"USERBIND-OK\"'\n"
                       "alias mcalias='echo alias-works > \"$HOME/alias.out\"'\n" +
                           // MC_SHELL_TEST_BASHRC adds lines (shopt -u promptvars, set -o posix) to
                           // try the integration under a user's odd options.
                           std::string(g_getenv("MC_SHELL_TEST_BASHRC") ? g_getenv("MC_SHELL_TEST_BASHRC") : "") + "\n");
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

    // ---- part two: term, term-output, Run File, onCommandFinished. Since the
    // terminal tabs (item 56) `minicode term` and Run File into a busy shell
    // open a tab of their own: each check about such a command follows the
    // tab that ran it (its own shell's marks), and the original terminal's
    // checks are that it was left alone.
    MCGuiStep("term --run 'echo hi' makes a tab", [] {
        gOrig = term();
        gBefore = sh();
        gFinished = 0;
        TerminalDock* dock = MCGuiApp().termDock;
        gOldActive = dock->onActiveChanged;
        dock->onActiveChanged = [](Terminal* t) {   // set before the new shell starts
            if (gOldActive) gOldActive(t);
            if (t != gOrig) t->onCommandFinished = [] { gFinished++; };
        };
        runCli({"term", "--run", "echo hi"});
    });
    MCGuiWait("term answered", 20, [] { return gCli->done; });
    MCGuiStep("it typed and entered the line in the new tab", [] {
        GCHECK(gCli->exitCode == 0);
        if (gCli->exitCode != 0) MCGuiNote("term said: %s", gCli->err.c_str());
        GCHECK(std::stoi(gCli->out) == term()->controlId());   // the window's id
        GCHECK(MCGuiApp().termDock->count() == 2 && MCGuiApp().termDock->active() == term() && term() != gOrig);
    });
    MCGuiWait("the line ran in the new tab", 25, [] { return sh().known && sh().atPrompt && sh().done >= 1; });
    MCGuiStep("its own marks: one command, status 0, finished once; term-output shows it", [] {
        GCHECK(sh().started == 1 && sh().done == 1 && sh().status == 0);
        GCHECK(gFinished >= 1);   // from the finishing mark of that tab
        runCli({"term-output", std::to_string(term()->controlId()), "--lines", "20"});
    });
    MCGuiWait("term-output answered", 15, [] { return gCli->done; });
    MCGuiStep("echo hi, then hi", [] {
        GCHECK(gCli->exitCode == 0);
        GCHECK(gCli->out.find("echo hi") != std::string::npos);
        GCHECK(gCli->out.find("echo hi\nhi") != std::string::npos);
        // and the newest row after them, the prompt the line came back to
        // (it was left out: VTE's end column is exclusive on the last row)
        const size_t at = gCli->out.find("echo hi\nhi");
        const std::string after = at == std::string::npos ? "" : gCli->out.substr(at + 10);
        GCHECK(after.find_first_not_of(" \n") != std::string::npos);
        backToOriginal();
        GCHECK(term() == gOrig && sameState(sh(), gBefore));   // the first terminal was not touched
    });
    // ---- the half-typed line (HANDOFF item 48): set aside, then given back
    MCGuiStep("the user's own bind still works", [] {
        VteTerminal* vte = findVte(term()->widget());
        vte_terminal_feed_child(vte, "\x15\x18\x14", 3);
    });
    MCGuiWait("its macro typed", 10, [] { return endsWith(lastLine(vteText()), "USERBIND-OK"); });
    MCGuiStep("half-type echo LEFT, no Return", [] {
        VteTerminal* vte = findVte(term()->widget());
        const std::string bytes = "\x15" "echo LEFT";
        vte_terminal_feed_child(vte, bytes.c_str(), (gssize)bytes.size());
    });
    MCGuiWait("it shows on the line", 10, [] { return endsWith(lastLine(vteText()), "echo LEFT"); });
    MCGuiStep("term --run 'echo RIGHT' beside it", [] {
        gBefore = sh();
        runCli({"term", "--run", "echo RIGHT"});
    });
    MCGuiWait("term answered (half line)", 20, [] { return gCli->done; });
    MCGuiWait("RIGHT ran in its own tab", 25, [] {
        return term() != gOrig && sh().known && sh().atPrompt && sh().done >= 1 && hasLine(vteText(), "RIGHT");
    });
    MCGuiStep("the half line stayed where it was", [] {
        GCHECK(gCli->exitCode == 0);
        GCHECK(sh().started == 1);   // the new tab ran only ours
        backToOriginal();
        const std::string t = vteText();
        GCHECK(endsWith(lastLine(t), "echo LEFT"));
        GCHECK(!hasLine(t, "RIGHT"));   // nothing of ours reached it
        GCHECK(sh().started == gBefore.started && sh().prompts == gBefore.prompts);
        VteTerminal* vte = findVte(term()->widget());
        vte_terminal_feed_child(vte, "\x15", 1);
    });
    MCGuiStep("half-type echo LEFTTWO, then Run File", [] {
        VteTerminal* vte = findVte(term()->widget());
        vte_terminal_feed_child(vte, "echo LEFTTWO", -1);
        MCGuiWriteFile(gHome + "/rf2.sh", "echo RUNFILE-OUT\n");
        GCHECK(MCGuiApp().editor->openFile(gHome + "/rf2.sh"));
    });
    MCGuiWait("LEFTTWO shows", 10, [] { return endsWith(lastLine(vteText()), "echo LEFTTWO"); });
    MCGuiStep("Run File", [] {
        gBefore = sh();
        GAction* a = g_action_map_lookup_action(G_ACTION_MAP(MCGuiApp().window), "runfile");
        GCHECK(a != nullptr);
        if (a) g_action_activate(a, nullptr);
    });
    MCGuiWait("the file ran and LEFTTWO came back", 15, [] {
        return sh().atPrompt && sh().prompts > gBefore.prompts && hasLine(vteText(), "RUNFILE-OUT") &&
               endsWith(lastLine(vteText()), "echo LEFTTWO");
    });
    MCGuiStep("Run File's output alone, LEFTTWO apart", [] {
        const std::string t = vteText();
        GCHECK(term() == gOrig);   // an idle terminal is used, no tab
        GCHECK(alwaysEndsLine(t, "LEFTTWO"));
        GCHECK(t.find("LEFTTWOecho") == std::string::npos && t.find("LEFTTWObash") == std::string::npos);
        GCHECK(sh().started == gBefore.started + 1);
        VteTerminal* vte = findVte(term()->widget());
        vte_terminal_feed_child(vte, "\x15", 1);
        gBefore = sh();
        typeLine("sleep 3");
    });
    MCGuiWait("sleep 3 runs", 10, [] { return sh().busy && sh().command == "sleep 3"; });
    MCGuiStep("term --run while it runs", [] { runCli({"term", "--run", "echo nope > \"$HOME/nope.out\""}); });
    MCGuiWait("term answered (busy)", 20, [] { return gCli->done; });
    MCGuiWait("the new tab ran it", 25, [] { return MCGuiFileExists(gHome + "/nope.out"); });
    MCGuiStep("not refused, and the busy shell is left alone", [] {
        GCHECK(gCli->exitCode == 0);
        GCHECK(term() != gOrig && MCGuiApp().termDock->count() == 2);
        backToOriginal();
        GCHECK(sh().busy && sh().command == "sleep 3" && sh().started == gBefore.started + 1);   // nothing else started in it
    });
    MCGuiStep("Run File while it runs", [] {
        MCGuiWriteFile(gHome + "/rf.sh", "echo runfile-ran > \"$HOME/rf.out\"\n");
        GCHECK(MCGuiApp().editor->openFile(gHome + "/rf.sh"));
        GAction* a = g_action_map_lookup_action(G_ACTION_MAP(MCGuiApp().window), "runfile");
        GCHECK(a != nullptr);
        if (a) g_action_activate(a, nullptr);
    });
    MCGuiWait("Run File ran in a tab of its own", 25, [] { return MCGuiFileExists(gHome + "/rf.out"); });
    MCGuiStep("the busy shell was not typed into", [] {
        GCHECK(term() != gOrig);
        GCHECK(statusText().find("Not run:") == std::string::npos);
        GCHECK_EQ_STR(MCGuiReadFile(gHome + "/rf.out"), "runfile-ran\n");
        backToOriginal();
        GCHECK(sh().started == gBefore.started + 1);
    });
    MCGuiWait("sleep 3 done", 15, [] { return sh().atPrompt; });
    MCGuiStep("sleep 3 was the only command the first shell ran", [] {
        GCHECK(sh().started == gBefore.started + 1);
        unlink((gHome + "/rf.out").c_str());
        GAction* a = g_action_map_lookup_action(G_ACTION_MAP(MCGuiApp().window), "runfile");
        if (a) g_action_activate(a, nullptr);   // at the prompt now
    });
    // The counter too: the file can be on disk before VTE has read the line's
    // start mark, while the prompt before it still says atPrompt (a full run
    // on two cores, November 2026).
    MCGuiWait("Run File ran the file", 15, [] {
        return MCGuiFileExists(gHome + "/rf.out") && sh().atPrompt && sh().started > gBefore.started + 1;
    });
    MCGuiStep("it ran through the shell as a command", [] {
        GCHECK(term() == gOrig);   // idle again: no tab
        GCHECK_EQ_STR(MCGuiReadFile(gHome + "/rf.out"), "runfile-ran\n");
        GCHECK(sh().started == gBefore.started + 2);
        typeLine("exec sh");
    });
    MCGuiWait("exec sh is the last mark", 10, [] { return sh().busy && sh().command == "exec sh"; });
    MCGuiPause(0.7);   // the shell has been replaced; no further mark can come
    MCGuiStep("term --run after exec sh", [] {
        gBefore = sh();
        runCli({"term", "--run", "echo nope > \"$HOME/nope2.out\""});
    });
    MCGuiWait("term answered (exec'd)", 20, [] { return gCli->done; });
    MCGuiWait("the new tab ran it", 25, [] { return MCGuiFileExists(gHome + "/nope2.out"); });
    MCGuiStep("not refused; nothing typed into the exec'd shell", [] {
        GCHECK(gCli->exitCode == 0);
        GCHECK(term() != gOrig);
        MCGuiApp().termDock->onActiveChanged = gOldActive;
        backToOriginal();
        GCHECK(sh().busy && sh().command == "exec sh" && sh().started == gBefore.started);
        term()->onCommandFinished = nullptr;
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
