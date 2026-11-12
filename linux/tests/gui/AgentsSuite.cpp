// AgentsSuite.cpp — the Agents panel on Linux, part one (`./build/
// minicode-guitest agents`, PARITY.md item 3.5; the Mac's
// tests/gui/AgentsSuite.mm). The committed fixtures in tests/fixtures/claude
// are copied into a scratch transcripts folder (MINICODE_CLAUDE_PROJECTS,
// MINICODE_CLAUDE_TMP), their paths moved to the project:
//   - Ctrl+Shift+A puts the panel in the sidebar; it lists the sessions
//     newest first, a session opening to its files in groups and to its
//     subagents;
//   - a click on a file opens it; a transcript appended to while the panel
//     shows adds the file; a file deleted is struck through (its row says so);
//   - nothing under the fixtures' folder is written (a listing with sizes and
//     dates is the same before and after the panel read it all);
//   - the transcripts folder removed empties the panel with its message.
#include "Harness.h"

#include "AgentsPanel.h"
#include "AgentWorktrees.h"
#include "CompareDecision.h"
#include "Document.h"
#include "EvidencePage.h"
#include "FileTree.h"
#include "Terminal.h"
#include "TerminalAttention.h"
#include "WindowLayout.h"
#include "Json.h"
#include "WorktreeChangesPage.h"
#include <vte/vte.h>
#include "AppSeam.h"
#include "Editor.h"

#include <glib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string gClaude, gProjects, gTmp, gProj, gSessDir, gBefore;
const char* kSid = "1e1f578c-45c6-4cb5-92e5-84e11a306454";
const char* kNew = "sess-new";

std::string stamp(int ago) {
    time_t t = time(nullptr) - ago;
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S.000Z", &tm);
    return buf;
}

std::string head(const char* type, int ago) {
    return std::string("{\"parentUuid\":null,\"isSidechain\":false,\"type\":\"") + type +
           "\",\"timestamp\":\"" + stamp(ago) + "\",\"cwd\":\"" + gProj +
           "\",\"sessionId\":\"" + kNew + "\",\"version\":\"9.9.9\",\"gitBranch\":\"main\",";
}

std::string prompt(const std::string& text, int ago) {
    return head("user", ago) + "\"message\":{\"role\":\"user\",\"content\":\"" + text + "\"}}\n";
}

std::string writeCall(const std::string& id, const std::string& path, int ago) {
    return head("assistant", ago) + "\"message\":{\"id\":\"msg_" + id +
           "\",\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"" + id +
           "\",\"name\":\"Write\",\"input\":{\"file_path\":\"" + path + "\",\"content\":\"x\"}}]}}\n" +
           head("user", ago) + "\"message\":{\"role\":\"user\",\"content\":[{\"tool_use_id\":\"" +
           id + "\",\"type\":\"tool_result\",\"content\":\"ok\",\"is_error\":false}]}}\n";
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size())
        s.replace(at, from.size(), to);
    return s;
}

void setAge(const std::string& path, int ago) {
    struct timeval tv[2];
    tv[0].tv_sec = tv[1].tv_sec = time(nullptr) - ago;
    tv[0].tv_usec = tv[1].tv_usec = 0;
    utimes(path.c_str(), tv);
}

void append(const std::string& path, const std::string& text) {
    std::ofstream(path, std::ios::app) << text;
}

// Every file under a folder with its size and date, for "nothing was written".
void listing(const std::string& dir, std::ostringstream& out) {
    if (GDir* d = g_dir_open(dir.c_str(), 0, nullptr)) {
        std::vector<std::string> names;
        while (const char* n = g_dir_read_name(d)) names.push_back(n);
        g_dir_close(d);
        std::sort(names.begin(), names.end());
        for (const std::string& n : names) {
            struct stat st;
            const std::string p = dir + "/" + n;
            if (stat(p.c_str(), &st) != 0) continue;
            out << p << ' ' << (long long)st.st_size << ' ' << (long long)st.st_mtime << '\n';
            if (S_ISDIR(st.st_mode)) listing(p, out);
        }
    }
}

std::string listingOf(const std::string& dir) {
    std::ostringstream o;
    listing(dir, o);
    return o.str();
}

AgentsPanel* panel() { return MCGuiApp().agents; }

int rowWhere(const std::string& text, AgentsPanel::RowInfo::Kind kind) {
    if (!panel()) return -1;
    auto rows = panel()->rows();
    for (size_t i = 0; i < rows.size(); i++)
        if (rows[i].kind == kind && rows[i].text.find(text) != std::string::npos) return (int)i;
    return -1;
}

int gClickAt = -1;
bool rowShown(int i) {
    GtkWidget* w = panel() ? panel()->rowWidget(i) : nullptr;
    return w && MCGuiOnScreen(w);
}

void clickRow(int i) {
    GtkWidget* w = panel() ? panel()->rowWidget(i) : nullptr;
    GCHECK(w != nullptr);
    if (w) MCGuiClick(w);
}


std::string gFixturesBefore, gWt;
EvidencePage* evidence() { return dynamic_cast<EvidencePage*>(MCGuiApp().editor->currentPage()); }
WorktreeChangesPage* changesPage() {
    return dynamic_cast<WorktreeChangesPage*>(MCGuiApp().editor->currentPage());
}

std::string fixturesDir() { return MCGuiBuildDir() + "/../../tests/fixtures/claude"; }

bool has(const std::string& s, const std::string& w) { return s.find(w) != std::string::npos; }

// The tree's labels, to find a row by what it says.
void labelsIn(GtkWidget* w, std::vector<GtkWidget*>* out) {
    if (GTK_IS_LABEL(w)) out->push_back(w);
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        labelsIn(c, out);
}

GtkWidget* treeLabel(const std::string& startsWith) {
    std::vector<GtkWidget*> ls;
    labelsIn(MCGuiApp().tree->widget(), &ls);
    for (GtkWidget* l : ls)
        if (std::string(gtk_label_get_text(GTK_LABEL(l))).compare(0, startsWith.size(), startsWith) == 0)
            return l;
    return nullptr;
}

VteTerminal* findVte(GtkWidget* root) {
    if (!root) return nullptr;
    if (VTE_IS_TERMINAL(root)) return VTE_TERMINAL(root);
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (VteTerminal* hit = findVte(c)) return hit;
    return nullptr;
}

int newRow() { return rowWhere("Add the new file", AgentsPanel::RowInfo::Session); }

std::string terminalText() {
    VteTerminal* v = findVte(MCGuiApp().terminal ? MCGuiApp().terminal->widget() : nullptr);
    if (!v) return "";
    char* t = vte_terminal_get_text_format(v, VTE_FORMAT_TEXT);
    std::string s = t ? t : "";
    g_free(t);
    return s;
}

void git(const std::string& dir, const std::string& args) {
    const std::string cmd = "git -C '" + dir + "' -c user.name=t -c user.email=t@t -c commit.gpgsign=false " +
                            args + " >/dev/null 2>&1";
    GCHECK(system(cmd.c_str()) == 0);
}

}  // namespace

MC_GUI_SUITE(agents) {
    MCGuiStep("fixtures copied into a scratch Claude world, paths moved to the project", [] {
        gProj = MCGuiProject();
        char buf[4096];
        if (realpath(gProj.c_str(), buf)) gProj = buf;
        gClaude = MCGuiScratch() + "/claude-agents";
        gProjects = gClaude + "/projects";
        gTmp = gClaude + "/tmp";
        gSessDir = gProjects + "/-agents-proj";
        const std::string fixtures = MCGuiBuildDir() + "/../../tests/fixtures/claude/projects/"
                                     "-Users-user-Code-public-minicode";
        for (const char* rel :
             {"1e1f578c-45c6-4cb5-92e5-84e11a306454.jsonl",
              "1e1f578c-45c6-4cb5-92e5-84e11a306454/subagents/agent-a1ad4e3e0a7ec7fd2.jsonl",
              "1e1f578c-45c6-4cb5-92e5-84e11a306454/subagents/agent-a1ad4e3e0a7ec7fd2.meta.json"}) {
            std::string text = MCGuiReadFile(fixtures + "/" + rel);
            GCHECK(!text.empty());
            text = replaceAll(text, "/Users/user/Code/public/minicode", gProj);
            text = replaceAll(text, "/private/tmp/claude-501", gTmp);
            MCGuiWriteFile(gSessDir + "/" + rel, text);
        }
        setAge(gSessDir + "/" + kSid + ".jsonl", 3600);
        setAge(gSessDir + "/" + kSid + "/subagents/agent-a1ad4e3e0a7ec7fd2.jsonl", 3600);
        // A newer session, still running: two files, one of them never made.
        MCGuiWriteFile(gProj + "/new.txt", "new\n");
        MCGuiWriteFile(gSessDir + "/" + kNew + ".jsonl",
                       prompt("Add the new file", 40) + writeCall("w1", gProj + "/new.txt", 30) +
                           writeCall("w2", gProj + "/gone.txt", 20));
        g_mkdir_with_parents((gTmp + "/-Users-user-Code-public-minicode").c_str(), 0755);
        g_setenv("MINICODE_CLAUDE_PROJECTS", gProjects.c_str(), TRUE);
        g_setenv("MINICODE_CLAUDE_TMP", gTmp.c_str(), TRUE);
        gBefore = listingOf(gClaude);
        gFixturesBefore = listingOf(fixturesDir());
        GCHECK(!gFixturesBefore.empty());
        GCHECK(!gBefore.empty());
    });
    MCGuiStep("Ctrl+Shift+A puts the Agents panel in the sidebar", [] {
        GCHECK(panel() == nullptr && !MCGuiApp().agentsShown);
        MCGuiKey("ctrl+shift+a");
    });
    MCGuiWait("the panel shows both sessions", 15, [] {
        return panel() && MCGuiApp().agentsShown && panel()->rows().size() >= 2;
    });
    MCGuiStep("newest first, the running one says so, subagents wait inside their session", [] {
        auto rows = panel()->rows();
        GCHECK_EQ_STR(rows[0].text, "Add the new file");
        GCHECK(rows[0].running && rows[0].detail.find("running") == 0);
        GCHECK_EQ_STR(rows[1].text, "Placeholder request for the main session.");
        GCHECK(!rows[1].running && rows[1].detail.find("finished") == 0);
        GCHECK_EQ_STR(panel()->summary(), "1 running · 3 sessions");
        GCHECK_EQ_STR(panel()->notice(), "");
        MCGuiSnapshot(MCGuiApp().window ? GTK_WIDGET(MCGuiApp().window) : nullptr, "agents-collapsed");
        gClickAt = 1;
    });
    MCGuiWait("the row is on screen", 10, [] { return rowShown(gClickAt); });
    MCGuiStep("click the finished session", [] { clickRow(gClickAt); });
    MCGuiWait("the finished session opens to its groups and its subagent", 10, [] {
        return rowWhere("LSP rename", AgentsPanel::RowInfo::Session) >= 0;
    });
    MCGuiStep("the subagent is nested one level down, the files in groups", [] {
        int sub = rowWhere("LSP rename", AgentsPanel::RowInfo::Session);
        auto rows = panel()->rows();
        GCHECK(sub >= 0 && rows[sub].depth == 1);
        // The main session's own files are "In the folder", as on the Mac;
        // under a checkout in .claude/worktrees the core calls the whole
        // project a worktree (MC_GUI_ROOT=/tmp gives CI's layout).
        const bool checkoutIsWorktree = gProj.find("/.claude/worktrees/") != std::string::npos;
        GCHECK(rowWhere(checkoutIsWorktree ? "In its worktree" : "In the folder",
                        AgentsPanel::RowInfo::Group) >= 0);
        GCHECK(rows[2].kind == AgentsPanel::RowInfo::Group && rows[2].depth == 1);
        gClickAt = 0;
    });
    MCGuiWait("the first row is on screen", 10, [] { return rowShown(gClickAt); });
    MCGuiStep("click the new session", [] { clickRow(gClickAt); });
    MCGuiWait("the new session opens to its folder group", 10, [] {
        // (Under a checkout in .claude/worktrees the core calls the project a
        // worktree, so the group is named by position, not by title.)
        return panel()->rows().size() > 3 && panel()->rows()[1].kind == AgentsPanel::RowInfo::Group;
    });
    MCGuiStep("click the group for its files", [] {
        auto rows = panel()->rows();
        int g = -1;
        for (size_t i = 0; i < rows.size(); i++)
            if (rows[i].kind == AgentsPanel::RowInfo::Group && rows[i].depth == 1 && i < 3)
                g = (int)i;
        GCHECK(g == 1);
        gClickAt = g;
    });
    MCGuiWait("the group is on screen", 10, [] { return rowShown(gClickAt); });
    MCGuiStep("click the group", [] { clickRow(gClickAt); });
    MCGuiWait("its files are listed", 10, [] { return rowWhere("new.txt", AgentsPanel::RowInfo::File) >= 0; });
    MCGuiStep("the missing file is marked, the existing one is not; nothing was written", [] {
        int a = rowWhere("new.txt", AgentsPanel::RowInfo::File);
        int b = rowWhere("gone.txt", AgentsPanel::RowInfo::File);
        auto rows = panel()->rows();
        GCHECK(a >= 0 && b >= 0 && rows[a].exists && !rows[b].exists);
        GCHECK_EQ_STR(listingOf(gClaude), gBefore);
        MCGuiSnapshot(MCGuiApp().window ? GTK_WIDGET(MCGuiApp().window) : nullptr, "agents");
        gClickAt = a;
    });
    MCGuiWait("the file is on screen", 10, [] { return rowShown(gClickAt); });
    MCGuiStep("click the file", [] { clickRow(gClickAt); });
    MCGuiWait("a click on the file opens it", 10, [] {
        return MCGuiApp().editor->currentPath() == MCGuiProjectFile("new.txt") ||
               MCGuiApp().editor->currentPath() == gProj + "/new.txt";
    });
    MCGuiStep("a transcript appended to while shown adds a file", [] {
        MCGuiWriteFile(gProj + "/third.txt", "3\n");
        append(gSessDir + "/" + kNew + ".jsonl", writeCall("w3", gProj + "/third.txt", 5));
    });
    MCGuiWait("the panel follows", 15, [] { return rowWhere("third.txt", AgentsPanel::RowInfo::File) >= 0; });
    MCGuiStep("a file deleted afterwards is struck through", [] {
        GCHECK(unlink((gProj + "/third.txt").c_str()) == 0);
    });
    MCGuiWait("its row says it is gone", 15, [] {
        int r = rowWhere("third.txt", AgentsPanel::RowInfo::File);
        return r >= 0 && !panel()->rows()[r].exists;
    });
    // ---- Show Terminal (item 3.6's board): only for a session a terminal named
    MCGuiStep("a session no terminal named has no Show Terminal in its menu", [] {
        GCHECK(panel() != nullptr && newRow() >= 0);
        panel()->openMenuFor(newRow());
        GCHECK(panel()->menuLabels().find("Show Terminal") == std::string::npos);
    });
    MCGuiStep("a notify names the first session in the terminal; the terminal is hidden", [] {
        Json args;
        GCHECK(Json::parse("{\"state\":\"waiting\",\"session_id\":\"" + panel()->rows()[newRow()].sessionId + "\"}",
                           args));
        std::string error;
        GCHECK(TerminalAttention::apply(MCGuiApp().terminal->controlId(), args, &error));
        if (MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
        panel()->openMenuFor(newRow());
        GCHECK(panel()->menuLabels().find("Show Terminal") != std::string::npos);
    });
    MCGuiStep("Show Terminal shows the terminal", [] {
        GCHECK(!MCGuiApp().layout->terminalShown());
        g_action_group_activate_action(panel()->actionGroup(), "terminal", nullptr);
        // Choosing the item closes its menu; running the action by name does not,
        // and the open popover sat over row 0 and took the click below (it
        // reached "Show Evidence" in the menu by luck here, nothing on CI).
        panel()->closeMenu();
    });
    MCGuiWait("the terminal is shown", 5, [] { return MCGuiApp().layout->terminalShown(); });
    // ---- Part two: the Evidence page
    MCGuiStep("a test run and a final message are appended to the new session", [] {
        std::string out;
        for (int i = 1; i <= 60; i++) out += "line " + std::to_string(i) + " of the output\\n";
        out += "=== 3 failed, 41 passed in 1.20s ===";
        const std::string id = "t1";
        append(gSessDir + "/" + kNew + ".jsonl",
               head("assistant", 4) + "\"message\":{\"id\":\"msg_t1\",\"role\":\"assistant\",\"content\":["
               "{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"Bash\",\"input\":{\"command\":\"pytest -q\","
               "\"description\":\"Run the suite\"}}]}}\n" +
                   head("user", 3) + "\"message\":{\"role\":\"user\",\"content\":[{\"tool_use_id\":\"t1\","
                   "\"type\":\"tool_result\",\"content\":\"" + out + "\",\"is_error\":false}]}}\n" +
                   head("assistant", 2) + "\"message\":{\"id\":\"msg_t2\",\"role\":\"assistant\",\"content\":["
                   "{\"type\":\"text\",\"text\":\"Everything is green, I promise.\"}]}}\n");
        (void)id;
    });
    MCGuiWait("the panel lists the session again (row 0 is the new one)", 15, [] {
        return panel()->rows().size() > 0 && panel()->rows()[0].text == "Add the new file" && rowShown(0);
    });
    MCGuiStep("a click on the session shows its Evidence in a tab", [] { clickRow(0); });
    MCGuiWait("the page has read the transcript and its outputs", 15, [] {
        return evidence() && evidence()->loaded() && evidence()->outputsLoaded() &&
               evidence()->commandCount() == 1 && has(evidence()->finalMessageText(), "green");
    });
    MCGuiStep("the final message beside the last test's own summary; the ends of the output", [] {
        GCHECK_EQ_STR(MCGuiApp().editor->active()->diffTitle(), "Evidence: Add the new file");
        GCHECK(has(evidence()->finalMessageText(), "Everything is green, I promise."));
        const std::string t = evidence()->lastTestText();
        GCHECK(has(t, "3 failed, 41 passed"));
        GCHECK(has(t, "$ pytest -q"));
        const std::string c = evidence()->commandsText();
        GCHECK(has(c, "Run the suite") && has(c, "TEST pytest") && has(c, "Open in Terminal"));
        GCHECK(has(c, "line 1 of the output") && has(c, "line 20 of the output"));
        GCHECK(!has(c, "line 30 of the output") && has(c, "line 60 of the output"));
        GCHECK(has(c, "Show all"));
        MCGuiSnapshot(MCGuiApp().window ? GTK_WIDGET(MCGuiApp().window) : nullptr, "evidence");
        evidence()->expand(0);
    });
    MCGuiWait("Show all reads the whole output", 15, [] { return evidence() && evidence()->isExpanded(0); });
    MCGuiStep("all of it shows, and Show less puts the ends back", [] {
        GCHECK(has(evidence()->commandsText(), "line 30 of the output"));
        GCHECK(!has(evidence()->commandsText(), "Show all"));
        evidence()->collapse(0);
        GCHECK(!has(evidence()->commandsText(), "line 30 of the output"));
    });
    MCGuiStep("a second click on the session shows the same tab", [] {
        const int tabs = MCGuiApp().editor->tabCount();
        MCGuiApp().agents->showEvidenceFor(0);
        GCHECK((MCGuiApp().editor->tabCount()) == (tabs));
    });
    MCGuiStep("Open in Terminal types the command and does not run it", [] {
        GCHECK(!MCGuiApp().terminal->busy());
        evidence()->openInTerminal(0);
    });
    MCGuiWait("the command is at the prompt", 20, [] { return has(terminalText(), "pytest -q"); });
    MCGuiPause(1.5);   // time is the condition: a command that ran would have printed by now
    MCGuiStep("it waits for Return: no output of its own appeared", [] {
        GCHECK(!has(terminalText(), "command not found") && !has(terminalText(), "no tests ran") &&
               !has(terminalText(), "not found"));
        GCHECK_EQ_STR(listingOf(fixturesDir()), gFixturesBefore);
    });
    MCGuiStep("the transcripts folder removed", [] {
        GCHECK(system(("rm -rf '" + gProjects + "'").c_str()) == 0);
    });
    MCGuiWait("the panel is empty and says why", 15, [] {
        return panel()->rows().empty() && panel()->notice().find("not") != std::string::npos;
    });
    MCGuiStep("the message names the folder", [] {
        GCHECK(panel()->notice().find("claude-agents/projects") != std::string::npos);
        GCHECK_EQ_STR(panel()->summary(), "");
        MCGuiKey("ctrl+shift+a");
    });
    MCGuiWait("the same key closes the panel", 5, [] { return !MCGuiApp().agentsShown; });
    // ---- Part two: the Worktrees node
    // .claude exists, listed in the tree, before any worktree does: the node
    // must come from watching .claude/worktrees, not from the one relisting.
    MCGuiStep("an empty .claude appears in the project", [] {
        g_mkdir_with_parents((MCGuiProject() + "/.claude").c_str(), 0755);
    });
    MCGuiPause(2.5);   // time is the condition: the tree's relisting for .claude comes and goes
    MCGuiStep("a git repository with an agent's worktree, one ahead, and main moved on", [] {
        gWt = gProj + "/.claude/worktrees/agent-wt";
        GCHECK(system(("cd '" + gProj + "' && git init -q -b main >/dev/null 2>&1").c_str()) == 0);
        MCGuiWriteFile(gProj + "/base.txt", "one\ntwo\nthree\n");
        MCGuiWriteFile(gProj + "/.gitignore", ".claude/\n");
        git(gProj, "add base.txt .gitignore");
        git(gProj, "commit -q -m base");
        git(gProj, "worktree add -q .claude/worktrees/agent-wt -b agent-branch");
        MCGuiWriteFile(gWt + "/base.txt", "one\ntwo changed\nthree\n");
        MCGuiWriteFile(gWt + "/agent.txt", "agent\n");
        git(gWt, "add agent.txt base.txt");
        git(gWt, "commit -q -m agent");
        MCGuiWriteFile(gWt + "/untracked.txt", "u\n");
        // main's later work, which is not the agent's
        MCGuiWriteFile(gProj + "/base.txt", "one\ntwo\nthree\nmain later\n");
        MCGuiWriteFile(gProj + "/later.txt", "later\n");
        git(gProj, "add base.txt later.txt");
        git(gProj, "commit -q -m later");
        GCHECK(MCGuiApp().worktrees != nullptr);
        MCGuiApp().worktrees->refresh();
    });
    MCGuiWait("git has listed the worktree with its ahead count and changes", 20, [] {
        const WorktreeInfo* w = MCGuiApp().worktrees->find(gWt);
        return w && w->ahead == 1 && w->dirty && MCGuiApp().worktrees->mainBranch() == "main";
    });
    // Earlier suites leave the tree scrolled down, and a row added at the top of
    // a scrolled list leaves it above the pane: a person scrolls up to find it
    // (the old setRoot did it by rebuilding), so each look scrolls up first.
    MCGuiWait("the Worktrees node is first in the tree", 15, [] {
        gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(MCGuiApp().tree->widget())), 0);
        GtkWidget* l = treeLabel("Worktrees");
        return l && MCGuiOnScreen(l);
    });
    MCGuiStep("it opens to the worktree", [] { MCGuiClick(treeLabel("Worktrees")); });
    MCGuiWait("the worktree row says how it stands", 15, [] { GtkWidget* l = treeLabel("agent-wt"); return l && MCGuiOnScreen(l); });
    MCGuiStep("named for its folder with the badge; select it", [] {
        GtkWidget* l = treeLabel("agent-wt");
        GCHECK(l != nullptr);
        GCHECK(has(gtk_label_get_text(GTK_LABEL(l)), "1 ahead · modified"));
        GCHECK(!gtk_widget_has_css_class(l, "minicode-git-modified") &&
               !gtk_widget_has_css_class(l, "minicode-git-untracked"));
        MCGuiClick(l);
    });
    MCGuiWait("the worktree is the selection", 10, [] {
        return MCGuiApp().tree->selectedPath() == gWt || has(MCGuiApp().tree->selectedPath(), "agent-wt");
    });
    MCGuiStep("Compare with Main", [] {
        g_action_group_activate_action(G_ACTION_GROUP(MCGuiApp().window), "compareworktree", nullptr);
    });
    MCGuiWait("a page lists the changes", 20, [] { return changesPage() && changesPage()->loaded(); });
    MCGuiStep("the agent's files, none of main's later work", [] {
        auto rows = changesPage()->rowDescriptions();
        GCHECK(((int)rows.size()) == (3));
        bool agent = false, base = false, untracked = false, later = false;
        for (const std::string& r : rows) {
            agent |= r == "A agent.txt";
            base |= r == "M base.txt";
            untracked |= r == "? untracked.txt";
            later |= has(r, "later.txt");
        }
        GCHECK(agent && base && untracked && !later);
        for (size_t i = 0; i < rows.size(); i++)
            if (rows[i] == "M base.txt") changesPage()->openChangeAtRow((int)i);
    });
    MCGuiWait("the compare view opens that file", 20, [] { return MCGuiApp().editor->isComparison(); });
    MCGuiStep("main at the branch point on the left: its later line is not taken out", [] {
        const CompareSpec& sp = MCGuiApp().editor->active()->compare()->spec;
        GCHECK_EQ_STR(sp.leftText, "one\ntwo\nthree\n");
        GCHECK_EQ_STR(sp.rightText, "one\ntwo changed\nthree\n");
        GCHECK(has(sp.leftLabel, "main at the branch point"));
        GCHECK(has(sp.title, "base.txt"));
        GCHECK_EQ_STR(listingOf(fixturesDir()), gFixturesBefore);
    });
}
