// AgentWorktrees.h — the worktrees under the window's folder, for the file
// tree's Worktrees node and Compare with Main (PARITY.md item 3.5, part two;
// the Mac's worktree half of MCAgentModel in src/AgentModel.mm). git runs off
// the main thread: `worktree list --porcelain` in the folder, then for each
// worktree inside it `rev-list --count <main>..<head>` (how far ahead) and
// `status --porcelain` (changes). A worktree is named for its agent when the
// AgentModel's sessions know it (a session or subagent whose worktree path
// it is), else by its folder. Read-only: nothing here writes to the repository
// or to Claude Code's files.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "AgentModel.h"
#include "AgentView.h"
#include "ComparePanes.h"

struct WorktreeInfo {
    std::string path, branch, head, label;
    bool running = false, dirty = false;
    int ahead = -1;   // -1 until git has answered
    std::string badge() const { return AgentView::worktreeBadge(ahead, dirty); }
};

class AgentWorktrees {
public:
    AgentWorktrees(const std::string& folder, AgentModel& model);
    ~AgentWorktrees();
    // Called on the main loop when the list changed.
    std::function<void()> onChange;
    std::vector<WorktreeInfo> list() const { return list_; }
    const WorktreeInfo* find(const std::string& path) const;
    const std::string& mainBranch() const { return mainBranch_; }
    bool loaded() const { return loaded_; }
    void setFolder(const std::string& folder);
    // Look again (coalesced; the first one at once).
    void refresh();
    // The same, 2 s from now (agents write into their worktrees all the time).
    void refreshSoon();

    // The files changed against the merge base with main, with git's error
    // text when it failed (main thread in, main thread out).
    using ChangesDone = std::function<void(std::vector<AgentView::Change>, std::string error)>;
    void changes(const WorktreeInfo& w, ChangesDone done);
    // One change as a comparison spec: main at the branch point on the left,
    // the worktree's file on the right.
    void comparison(const WorktreeInfo& w, const AgentView::Change& c,
                    std::function<void(CompareSpec)> done);

private:
    AgentModel& model_;
    std::string folder_, real_;
    std::vector<WorktreeInfo> list_;
    std::string mainBranch_;
    bool loaded_ = false, busy_ = false, again_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    unsigned timer_ = 0;
    void run();
};
