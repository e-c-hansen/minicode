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
}
