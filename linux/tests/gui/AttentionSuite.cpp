// AttentionSuite.cpp — "Waiting for you" on Linux (`./build/minicode-guitest
// attention`, PARITY.md item 3.6; the Mac's AttentionSuite in tests/gui):
//   - the shell in the app's terminal has MINICODE_TERMINAL_ID (the window's
//     control id) and MINICODE_PID (the app's), and no stale copies;
//   - `minicode notify --from-hook` with canned hook JSON on stdin marks that
//     terminal (waiting, then done), prints nothing and exits 0, and does the
//     same with no app to talk to;
//   - the dot shows over the terminal, the window title counts the waiting
//     terminals, and `state` reports the mark;
//   - a real key typed in the terminal clears the mark;
//   - Next Waiting Terminal (Ctrl+Alt+W) shows a hidden terminal and gives it
//     the keyboard;
//   - `minicode hooks --install` into the scratch HOME's ~/.claude/settings.json
//     twice gives the same file, and a symlinked settings file stays a link.
// Nothing here touches the real ~/.claude: the harness's HOME is a scratch one.
#include "Harness.h"

#include "AppSeam.h"
#include "Json.h"
#include "Editor.h"
#include "ProcessMonitor.h"
#include "ShellIntegration.h"
#include "Terminal.h"
#include "TerminalAttention.h"
#include "TerminalDock.h"
#include "WindowLayout.h"

#include <gio/gio.h>
#include <vte/vte.h>

#include <map>
#include <set>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct Run {
    bool done = false;
    int exitCode = -1;
    std::string out, err;
};

// The client with `env` added and `input` on its stdin.
std::shared_ptr<Run> client(const std::vector<std::string>& args,
                            const std::map<std::string, std::string>& env = {},
                            const std::vector<std::string>& unset = {},
                            const std::string& input = "") {
    auto run = std::make_shared<Run>();
    GSubprocessLauncher* l = g_subprocess_launcher_new(
        (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                           G_SUBPROCESS_FLAGS_STDERR_PIPE));
    for (const auto& kv : env) g_subprocess_launcher_setenv(l, kv.first.c_str(), kv.second.c_str(), TRUE);
    for (const std::string& u : unset) g_subprocess_launcher_unsetenv(l, u.c_str());
    std::vector<const char*> argv;
    const std::string exe = MCGuiBuildDir() + "/minicode";
    argv.push_back(exe.c_str());
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    GError* error = nullptr;
    GSubprocess* p = g_subprocess_launcher_spawnv(l, argv.data(), &error);
    g_object_unref(l);
    if (!p) {
        run->err = error ? error->message : "spawn failed";
        run->done = true;
        g_clear_error(&error);
        return run;
    }
    GBytes* in = g_bytes_new(input.data(), input.size());
    g_subprocess_communicate_async(
        p, in, nullptr,
        +[](GObject* source, GAsyncResult* res, gpointer data) {
            auto* holder = static_cast<std::shared_ptr<Run>*>(data);
            Run& r = **holder;
            GBytes *out = nullptr, *err = nullptr;
            g_subprocess_communicate_finish(G_SUBPROCESS(source), res, &out, &err, nullptr);
            if (out) r.out.assign((const char*)g_bytes_get_data(out, nullptr), g_bytes_get_size(out));
            if (err) r.err.assign((const char*)g_bytes_get_data(err, nullptr), g_bytes_get_size(err));
            if (out) g_bytes_unref(out);
            if (err) g_bytes_unref(err);
            r.exitCode = g_subprocess_get_if_exited(G_SUBPROCESS(source))
                             ? g_subprocess_get_exit_status(G_SUBPROCESS(source))
                             : -1;
            r.done = true;
            delete holder;
            g_object_unref(source);
        },
        new std::shared_ptr<Run>(run));
    g_bytes_unref(in);
    return run;
}

VteTerminal* findVte(GtkWidget* root) {
    if (!root) return nullptr;
    if (VTE_IS_TERMINAL(root)) return VTE_TERMINAL(root);
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (VteTerminal* hit = findVte(c)) return hit;
    return nullptr;
}

// The mark's label in the terminal's overlay, shown or not.
GtkWidget* findDot(GtkWidget* root) {
    if (!root) return nullptr;
    if (GTK_IS_LABEL(root)) {
        const char* t = gtk_label_get_text(GTK_LABEL(root));
        if (t && std::string(t) == "\u25CF") return root;
    }
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = findDot(c)) return hit;
    return nullptr;
}

std::shared_ptr<Run> gRun;
std::string gHome, gSettings, gDotfile, gFirstText;
std::string gTermId, gAppPid;

const char* kPermission =
    R"({"hook_event_name":"Notification","message":"Claude needs your permission to use Bash",)"
    R"("session_id":"s-1","transcript_path":"/tmp/s-1.jsonl","cwd":"/tmp"})";
const char* kStop = R"({"hook_event_name":"Stop","session_id":"s-1","cwd":"/tmp"})";

std::map<std::string, std::string> hookEnv() {
    return {{"MINICODE_TERMINAL_ID", gTermId}, {"MINICODE_PID", gAppPid}};
}

std::string stateAttention() {
    // `state` for the front window: the terminal's mark, "" for none.
    return MCGuiApp().terminal ? MCGuiApp().terminal->attentionShown() : "?";
}

bool hasAll(const std::string& text) {
    for (const char* e : {"Notification", "Stop", "UserPromptSubmit", "PostToolUse"})
        if (text.find(e) == std::string::npos) return false;
    return text.find("minicode notify --from-hook") != std::string::npos;
}

}  // namespace

MC_GUI_SUITE(attention) {
    MCGuiStep("the shell is made", [] {
        GCHECK(MCGuiApp().terminal != nullptr);
        if (!MCGuiApp().terminal) return;
        if (!MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
        MCGuiApp().terminal->typeText("true");
    });
    MCGuiWait("the terminal has its shell", 30, [] {
        VteTerminal* vte = findVte(MCGuiApp().terminal->widget());
        if (!vte || MCGuiApp().terminal->shellPid() <= 0) return false;
        char* text = vte_terminal_get_text_format(vte, VTE_FORMAT_TEXT);
        const bool there = text && std::string(text).find("true") != std::string::npos;
        g_free(text);
        return there;
    });
    MCGuiStep("the shell writes its two variables to a file", [] {
        gTermId = std::to_string(MCGuiApp().terminal->controlId());
        gAppPid = std::to_string((int)getpid());
        const std::string out = MCGuiProjectFile("ids.txt");
        // Other suites leave an unrun line at the prompt: clear it first.
        VteTerminal* vte = findVte(MCGuiApp().terminal->widget());
        vte_terminal_feed_child(vte, "\x15", 1);
        const std::string cmd =
            "echo \"$MINICODE_TERMINAL_ID:$MINICODE_PID\" > '" + out + "'\n";
        vte_terminal_feed_child(vte, cmd.c_str(), cmd.size());
    });
    MCGuiWait("the file is written", 15, [] { return MCGuiFileExists(MCGuiProjectFile("ids.txt")) &&
                                                     !MCGuiReadFile(MCGuiProjectFile("ids.txt")).empty(); });
    MCGuiStep("it holds this terminal's id and the app's pid", [] {
        std::string got = MCGuiReadFile(MCGuiProjectFile("ids.txt"));
        while (!got.empty() && (got.back() == '\n' || got.back() == '\r')) got.pop_back();
        GCHECK(MCGuiApp().terminal->controlId() > 0);
        GCHECK_EQ_STR(got, gTermId + ":" + gAppPid);
    });

    MCGuiStep("a permission hook is sent on stdin", [] {
        gRun = client({"notify", "--from-hook"}, hookEnv(), {}, kPermission);
    });
    MCGuiWait("the client is done", 15, [] { return gRun->done; });
    MCGuiStep("it exited 0 and printed nothing", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK_EQ_STR(gRun->out, "");
        GCHECK_EQ_STR(gRun->err, "");
    });
    MCGuiWait("the terminal shows it waiting", 5, [] { return stateAttention() == "waiting"; });
    MCGuiStep("the title counts it and the dot has Claude's words", [] {
        const char* title = gtk_window_get_title(MCGuiApp().window);
        GCHECK(title && std::string(title).find("(1 waiting)") == 0);
        GtkWidget* dot = findDot(MCGuiApp().terminal->widget());
        GCHECK(dot != nullptr);
        if (!dot) return;
        GCHECK(gtk_widget_get_visible(dot));
        const char* tip = gtk_widget_get_tooltip_text(dot);
        GCHECK(tip && std::string(tip).find("needs your permission") != std::string::npos);
    });
    MCGuiStep("a Stop hook says done", [] {
        gRun = client({"notify", "--from-hook"}, hookEnv(), {}, kStop);
    });
    MCGuiWait("the client is done (2)", 15, [] { return gRun->done; });
    MCGuiWait("the terminal shows it done", 5, [] { return stateAttention() == "done"; });
    MCGuiStep("the count is gone from the title", [] {
        const char* title = gtk_window_get_title(MCGuiApp().window);
        GCHECK(title && std::string(title).find("waiting") == std::string::npos);
    });

    MCGuiStep("with no app to talk to, and the same input, it is silent", [] {
        gRun = client({"notify", "--from-hook"},
                      {{"MINICODE_TERMINAL_ID", gTermId}, {"MINICODE_PID", "999999"},
                       {"MINICODE_CONTROL_DIR", MCGuiScratch() + "/no-such-sockets"}},
                      {}, kPermission);
    });
    MCGuiWait("the client is done (3)", 15, [] { return gRun->done; });
    MCGuiStep("exit 0, no output, the mark unchanged", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK_EQ_STR(gRun->out + gRun->err, "");
        GCHECK_EQ_STR(stateAttention(), "done");
    });
    MCGuiStep("outside minicode (no variables) it is silent too", [] {
        gRun = client({"notify", "--from-hook"}, {}, {"MINICODE_TERMINAL_ID", "MINICODE_PID"},
                      kPermission);
    });
    MCGuiWait("the client is done (4)", 15, [] { return gRun->done; });
    MCGuiStep("exit 0, no output", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK_EQ_STR(gRun->out + gRun->err, "");
    });

    MCGuiStep("waiting again, and `state` reports the terminal's mark", [] {
        gRun = client({"notify", "--from-hook"}, hookEnv(), {}, kPermission);
    });
    MCGuiWait("the client is done (5)", 15, [] { return gRun->done; });
    MCGuiWait("waiting again", 5, [] { return stateAttention() == "waiting"; });
    MCGuiStep("state is asked", [] { gRun = client({"state"}, hookEnv()); });
    MCGuiWait("state answered", 15, [] { return gRun->done; });
    MCGuiStep("its terminal has the mark", [] {
        Json j;
        std::string err;
        GCHECK(Json::parse(gRun->out, j, &err));
        const Json& terms = j["windows"].elements().empty() ? j["terminals"] : j["windows"].elements()[0]["terminals"];
        GCHECK(!terms.elements().empty());
        if (terms.elements().empty()) return;
        GCHECK_EQ_STR(terms.elements()[0]["attention"].asString(), "waiting");
        GCHECK_EQ_STR(terms.elements()[0]["id"].asInt() == MCGuiApp().terminal->controlId() ? "yes" : "no", "yes");
    });

    MCGuiStep("a key typed in the terminal", [] {
        MCGuiApp().terminal->focus();
    });
    MCGuiWait("the terminal has the keyboard", 5, [] {
        GtkWidget* f = gtk_window_get_focus(MCGuiApp().window);
        return f && MCGuiApp().terminal->owns(f);
    });
    MCGuiWait("the dot is on screen", 5, [] {
        GtkWidget* dot = findDot(MCGuiApp().terminal->widget());
        return dot && gtk_widget_get_mapped(dot);
    });
    MCGuiStep("types", [] { MCGuiType("x"); });
    MCGuiWait("the mark is gone", 5, [] { return stateAttention().empty(); });
    MCGuiStep("the dot is hidden", [] {
        GtkWidget* dot = findDot(MCGuiApp().terminal->widget());
        GCHECK(dot == nullptr || !gtk_widget_get_visible(dot));
        VteTerminal* vte = findVte(MCGuiApp().terminal->widget());
        vte_terminal_feed_child(vte, "\x15", 1);   // the x is not left at the prompt
    });

    MCGuiStep("waiting once more, with the terminal hidden", [] {
        if (MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
        gRun = client({"notify", "--from-hook"}, hookEnv(), {}, kPermission);
    });
    MCGuiWait("the client is done (6)", 15, [] { return gRun->done; });
    MCGuiWait("marked", 5, [] { return stateAttention() == "waiting"; });
    MCGuiStep("Ctrl+Alt+W is pressed", [] {
        GCHECK(!MCGuiApp().layout->terminalShown());
        gtk_widget_grab_focus(MCGuiApp().editor->widget());
        MCGuiKey("ctrl+alt+w");
    });
    MCGuiWait("the terminal is shown and has the keyboard", 5, [] {
        GtkWidget* f = gtk_window_get_focus(MCGuiApp().window);
        return MCGuiApp().layout->terminalShown() && f && MCGuiApp().terminal->owns(f);
    });
    MCGuiStep("it is cleared by the next key", [] { MCGuiType("y"); });
    MCGuiWait("cleared", 5, [] { return stateAttention().empty(); });
    MCGuiStep("with nothing waiting the command only beeps", [] {
        MCGuiKey("ctrl+alt+w");
        GCHECK_EQ_STR(stateAttention(), "");
        VteTerminal* vte = findVte(MCGuiApp().terminal->widget());
        vte_terminal_feed_child(vte, "\x15", 1);
    });

    // ------------------------------------------------------------- hooks
    MCGuiStep("hooks --print shows the command", [] {
        gRun = client({"hooks", "--print"});
    });
    MCGuiWait("print is done", 15, [] { return gRun->done; });
    MCGuiStep("it names the four events", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(hasAll(gRun->out));
    });
    MCGuiStep("hooks --install into the scratch HOME's settings", [] {
        const char* h = getenv("HOME");
        gHome = h ? h : "";
        GCHECK(!gHome.empty() && gHome.compare(0, MCGuiScratch().size(), MCGuiScratch()) == 0);
        gSettings = gHome + "/.claude/settings.json";
        unlink(gSettings.c_str());
        gRun = client({"hooks", "--install"}, {{"HOME", gHome}}, {"CLAUDE_CONFIG_DIR"});
    });
    MCGuiWait("install is done", 15, [] { return gRun->done; });
    MCGuiStep("the file has the four hooks, and it is installed again", [] {
        GCHECK(gRun->exitCode == 0);
        gFirstText = MCGuiReadFile(gSettings);
        GCHECK(hasAll(gFirstText));
        gRun = client({"hooks", "--install"}, {{"HOME", gHome}}, {"CLAUDE_CONFIG_DIR"});
    });
    MCGuiWait("second install is done", 15, [] { return gRun->done; });
    MCGuiStep("nothing changed the second time", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(gRun->out.find("nothing changed") != std::string::npos);
        GCHECK_EQ_STR(MCGuiReadFile(gSettings), gFirstText);
    });
    MCGuiStep("a symlinked settings file with other settings in it", [] {
        unlink(gSettings.c_str());
        gDotfile = gHome + "/dotfiles/claude-settings.json";
        MCGuiWriteFile(gDotfile, "{\"theme\": \"dark\"}\n");
        GCHECK(symlink(gDotfile.c_str(), gSettings.c_str()) == 0);
        gRun = client({"hooks", "--install"}, {{"HOME", gHome}}, {"CLAUDE_CONFIG_DIR"});
    });
    MCGuiWait("link install is done", 15, [] { return gRun->done; });
    MCGuiStep("it stays a link and its target has both", [] {
        GCHECK(gRun->exitCode == 0);
        struct stat st;
        GCHECK(lstat(gSettings.c_str(), &st) == 0 && S_ISLNK(st.st_mode));
        const std::string text = MCGuiReadFile(gDotfile);
        GCHECK(hasAll(text));
        GCHECK(text.find("\"theme\"") != std::string::npos);
        gRun = client({"hooks", "--install"}, {{"HOME", gHome}}, {"CLAUDE_CONFIG_DIR"});
    });
    MCGuiWait("link reinstall is done", 15, [] { return gRun->done; });
    MCGuiStep("and a second run leaves it", [] {
        GCHECK(gRun->out.find("nothing changed") != std::string::npos);
        struct stat st;
        GCHECK(lstat(gSettings.c_str(), &st) == 0 && S_ISLNK(st.st_mode));
    });
}

// ------------------------------------------------------------ terminal_ids
// PARITY.md 4.7: every terminal has an id of its own. Two tabs and a split
// (three shells), each shell's $MINICODE_TERMINAL_ID written to a file by the
// shell itself, `state` listing all three, a notify marking one terminal only,
// term-output by id, the process monitor under each id, and a closing tab
// taking its mark and its id away.
namespace {
Terminal *gT1 = nullptr, *gT2 = nullptr, *gT3 = nullptr;   // tab 1; tab 2; tab 2's split
int gId2 = 0, gId3 = 0;
std::string gOldShellEnv;
bool gHadShellEnv = false;
bool gIdsReady(Terminal* t) { return t && t->shellPid() > 0 && t->shellState().known && t->shellState().atPrompt; }
std::string idFile(int n) { return MCGuiProjectFile("tid" + std::to_string(n) + ".txt"); }
std::string trimmed(std::string v) {
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
    return v;
}
// The `state` terminal entry for `id`, from gRun's answer; null Json when none.
Json stateTerminal(int id, int* count = nullptr) {
    Json j;
    std::string err;
    if (!Json::parse(gRun->out, j, &err)) return Json();
    Json found;
    int n = 0;
    for (const Json& w : j["windows"].elements())
        for (const Json& t : w["terminals"].elements()) {
            n++;
            if (t["id"].asInt() == id) found = t;
        }
    if (count) *count = n;
    return found;
}
}  // namespace

MC_GUI_SUITE(terminal_ids) {
    MCGuiStep("a bash; tab one, tab two and a split of it", [] {
        TerminalDock* d = MCGuiApp().termDock;
        GCHECK(d != nullptr);
        if (!d) return;
        const char* old = g_getenv("SHELL");
        gHadShellEnv = old != nullptr;
        gOldShellEnv = old ? old : "";
        GCHECK(std::string(g_get_home_dir()).find(MCGuiScratch()) == 0);   // never the user's own
        g_setenv("SHELL", "/bin/bash", TRUE);
        MCGuiWriteFile(std::string(g_get_home_dir()) + "/.bashrc", "\n");
        d->confirmClose = nullptr;
        d->newTerminal();
        while (d->count() > 1) d->close(0, false);
        if (!MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
        gT1 = d->at(0);
        gT2 = d->newTerminal();
        gT3 = d->split(false, true);
        GCHECK(d->count() == 2 && gT1 && gT2 && gT3);
        gRun = nullptr;
    });
    MCGuiWait("three shells at their prompts", 40, [] { return gIdsReady(gT1) && gIdsReady(gT2) && gIdsReady(gT3); });
    MCGuiStep("each shell writes its $MINICODE_TERMINAL_ID to a file", [] {
        int n = 1;
        for (Terminal* t : {gT1, gT2, gT3}) {
            unlink(idFile(n).c_str());
            t->runLine("echo \"$MINICODE_TERMINAL_ID:$MINICODE_PID\" > '" + idFile(n) + "'", [](const std::string&) {});
            n++;
        }
    });
    MCGuiWait("three files", 20, [] {
        for (int n = 1; n <= 3; n++)
            if (trimmed(MCGuiReadFile(idFile(n))).empty()) return false;
        return true;
    });
    MCGuiStep("the ids are distinct, each the terminal's own, none a window's", [] {
        const std::string pid = std::to_string((int)getpid());
        int n = 1;
        std::set<std::string> seen;
        for (Terminal* t : {gT1, gT2, gT3}) {
            const std::string got = trimmed(MCGuiReadFile(idFile(n++)));
            GCHECK_EQ_STR(got, std::to_string(t->controlId()) + ":" + pid);
            seen.insert(got);
            GCHECK(t->controlId() >= Terminal::kFirstId);
            GCHECK(t->controlId() != MCGuiApp().controlId);
            GCHECK(Terminal::byId(t->controlId()) == t);
        }
        GCHECK(seen.size() == 3);
        gRun = client({"state"});
    });
    MCGuiWait("state answered", 15, [] { return gRun->done; });
    MCGuiStep("state lists all three, with titles, and the split pane active", [] {
        int count = 0;
        Json t1 = stateTerminal(gT1->controlId(), &count);
        Json t2 = stateTerminal(gT2->controlId());
        Json t3 = stateTerminal(gT3->controlId());
        GCHECK(count == 3);
        GCHECK(t1["id"].asInt() == gT1->controlId());
        GCHECK(t2["id"].asInt() == gT2->controlId());
        GCHECK(t3["id"].asInt() == gT3->controlId());
        GCHECK(t3["active"].asBool());
        GCHECK(!t1["active"].asBool() && !t2["active"].asBool());
        GCHECK(t1.has("title") && t1.has("busy"));
    });
    MCGuiStep("a notify for the second tab's terminal", [] {
        gRun = client({"notify", "--terminal", std::to_string(gT2->controlId()), "--state", "waiting",
                       "--message", "second"});
    });
    MCGuiWait("notify answered", 15, [] { return gRun->done; });
    MCGuiStep("only that terminal is waiting; state and the board say so", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK_EQ_STR(gT2->attentionShown(), "waiting");
        GCHECK_EQ_STR(gT1->attentionShown(), "");
        GCHECK_EQ_STR(gT3->attentionShown(), "");
        GCHECK_EQ_STR(TerminalAttention::markName(gT1->controlId()), "");
        GCHECK_EQ_STR(TerminalAttention::markName(gT2->controlId()), "waiting");
        gRun = client({"state"});
    });
    MCGuiWait("state answered (2)", 15, [] { return gRun->done; });
    MCGuiStep("state has the mark on the second terminal alone", [] {
        GCHECK_EQ_STR(stateTerminal(gT2->controlId())["attention"].asString(), "waiting");
        GCHECK(stateTerminal(gT1->controlId())["attention"].isNull());
        GCHECK(stateTerminal(gT3->controlId())["attention"].isNull());
        // A window's id is no terminal's: a clear error, no terminal marked.
        gRun = client({"notify", "--terminal", std::to_string(MCGuiApp().controlId), "--state", "done"});
    });
    MCGuiWait("notify by window id answered", 15, [] { return gRun->done; });
    MCGuiStep("which fails, saying so", [] {
        GCHECK(gRun->exitCode != 0);
        GCHECK(gRun->err.find("there is no terminal") != std::string::npos);
        GCHECK_EQ_STR(gT1->attentionShown() + gT3->attentionShown(), "");
        gT3->runLine("echo ZZC$((20+3))", [](const std::string&) {});
    });
    MCGuiWait("pane three shows its output", 15, [] { return gT3->recentText(20).find("ZZC23") != std::string::npos; });
    MCGuiStep("term-output by id reads the right pane", [] {
        gRun = client({"term-output", std::to_string(gT3->controlId()), "--lines", "20"});
    });
    MCGuiWait("term-output answered", 15, [] { return gRun->done; });
    MCGuiStep("the split's text, and not the first tab's", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(gRun->out.find("ZZC23") != std::string::npos);
        gRun = client({"term-output", std::to_string(gT1->controlId()), "--lines", "20"});
    });
    MCGuiWait("term-output (2) answered", 15, [] { return gRun->done; });
    MCGuiStep("tab one has none of it; the window's id reads nothing", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(gRun->out.find("ZZC23") == std::string::npos);
        gRun = client({"term-output", std::to_string(MCGuiApp().controlId)});
    });
    MCGuiWait("term-output (3) answered", 15, [] { return gRun->done; });
    MCGuiStep("an error for it", [] {
        GCHECK(gRun->exitCode != 0);
        GCHECK(gRun->err.find("there is no terminal") != std::string::npos);
    });
    MCGuiStep("the process monitor looks", [] { ProcessMonitor::shared().sampleNow([] {}); });
    MCGuiWait("it lists each shell under its terminal's id", 30, [] {
        int ok = 0;
        for (const Procs::Terminal& t : ProcessMonitor::shared().terminals()) {
            if (t.id == gT1->controlId() && t.shellPid == gT1->shellPid()) ok++;
            if (t.id == gT2->controlId() && t.shellPid == gT2->shellPid()) ok++;
            if (t.id == gT3->controlId() && t.shellPid == gT3->shellPid()) ok++;
        }
        return ok == 3;
    });
    MCGuiStep("tab two closes, with its waiting mark and its split", [] {
        TerminalDock* d = MCGuiApp().termDock;
        gId2 = gT2->controlId();
        gId3 = gT3->controlId();
        d->close(d->indexOf(gT2), false);
        GCHECK(d->count() == 1);
        GCHECK(Terminal::byId(gId2) == nullptr && Terminal::byId(gId3) == nullptr);
        GCHECK_EQ_STR(TerminalAttention::markName(gId2), "");
        GCHECK(TerminalAttention::waitingCount() == 0);
        gRun = client({"state"});
    });
    MCGuiWait("state answered (3)", 15, [] { return gRun->done; });
    MCGuiStep("state lists the first tab's terminal only", [] {
        int count = 0;
        GCHECK(!stateTerminal(gT1->controlId(), &count).isNull());
        GCHECK(count == 1);
        gRun = client({"notify", "--terminal", std::to_string(gId2), "--state", "waiting"});
    });
    MCGuiWait("notify for the closed one answered", 15, [] { return gRun->done; });
    MCGuiStep("a closed terminal's id is refused; the settings are put back", [] {
        GCHECK(gRun->exitCode != 0);
        if (gHadShellEnv) g_setenv("SHELL", gOldShellEnv.c_str(), TRUE);
        else g_unsetenv("SHELL");
    });
}
