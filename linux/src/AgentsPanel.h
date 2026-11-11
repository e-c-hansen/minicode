// AgentsPanel.h — the Agents panel (PARITY.md item 3.5, part one; the Mac's
// MCAgentsPanel in src/AgentsPanel.mm). It takes the file tree's place in the
// sidebar as the Processes panel does (main.cpp's act_toggle_agents). Rows
// come from AgentModel: each session that worked in the window's folder,
// newest first, with its subagents nested; a session opens to the files it
// wrote or edited in three groups (In the folder, In its worktree, Outside
// the folder), a file that is gone struck through. A click on a session or
// group opens or closes it, a click on a file opens it. Read-only toward
// Claude Code's data.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "AgentModel.h"

class AgentsPanel {
public:
    explicit AgentsPanel(const std::string& folder);
    ~AgentsPanel();

    GtkWidget* widget() const { return root_; }
    std::function<void(const std::string&)> onOpenFile;       // an existing file
    // transcript path, session name: a click on a session.
    std::function<void(const std::string&, const std::string&)> onShowEvidence;
    // The terminal (an id) a session runs in, 0 for none; and showing it.
    std::function<int(const std::string& sessionId, const std::string& transcript)> terminalFor;
    std::function<void(int)> onShowTerminal;
    std::function<void()> onModelChange;                      // the sessions changed
    std::function<void(const std::string&)> onNote;           // the status bar

    void setActive(bool active);
    void focus();
    AgentModel& model() { return model_; }

    // What the rows say, for the checks.
    struct RowInfo {
        enum Kind { Session, Group, File } kind = Session;
        int depth = 0;
        std::string text, detail, tooltip, key, path, sessionId;
        bool running = false, exists = true, expanded = false;
    };
    std::vector<RowInfo> rows() const { return rows_; }
    GtkWidget* rowWidget(int row) const;   // for the checks' clicks
    GtkWidget* listWidget() const { return list_; }
    std::string summary() const;
    std::string notice() const;
    void activateRow(int row);
    // For the checks: the Evidence of the session at `row`, as a click shows it.
    void showEvidenceFor(int row) { if (row >= 0 && row < (int)rows_.size()) showEvidence(rows_[row]); }
    // Pops the row's menu; the labels it shows are menuLabels().
    void openMenuFor(int row);
    std::string menuLabels() const { return menuLabels_; }
    GActionGroup* actionGroup() const { return G_ACTION_GROUP(actions_); }

private:
    AgentModel model_;
    GtkWidget* root_ = nullptr;
    GtkWidget* summary_ = nullptr;
    GtkWidget* notice_ = nullptr;
    GtkWidget* scroll_ = nullptr;
    GtkWidget* list_ = nullptr;
    GtkWidget* menu_ = nullptr;
    GSimpleActionGroup* actions_ = nullptr;
    std::vector<RowInfo> rows_;
    std::set<std::string> expanded_;
    std::string menuLabels_, menuPath_;
    int menuTerminal_ = 0;
    int menuRow_ = -1;
    bool active_ = false;
    unsigned timer_ = 0;

    void showEvidence(const RowInfo& r);
    void rebuild();
    void popup(GMenu* menu, double x, double y);
    void dropMenu();
    static void onPressed(GtkGestureClick*, int, double, double, gpointer);
    static void onActivated(GtkListBox*, GtkListBoxRow*, gpointer);
    static int tick(void*);
};
