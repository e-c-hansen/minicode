// AgentModel.h — one window's view of the agents that worked in its folder
// (PARITY.md item 3.5, part one; the Mac's MCAgentModel in src/AgentModel.mm).
// The core's AgentIndex lives on a worker thread of its own (it is not
// thread-safe and its rows point into it); what the main loop sees is an
// immutable Snapshot copied out of it and handed over with g_idle_add.
//
// Read-only toward Claude Code's data: the transcripts and scratchpads are
// only stat'ed and read. Changes are followed with GFileMonitor on the
// transcripts folder, the scratchpads' folder and each project folder in the
// first, plus a slow poll while the panel shows (monitors are not recursive,
// and a subagent's transcript is two folders down); a folder that is not
// there yet is watched through the nearest one above it that is.
//
// Where the files are: $MINICODE_CLAUDE_PROJECTS, else ~/.claude/projects;
// $MINICODE_CLAUDE_TMP, else /tmp/claude-<uid>.
#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "AgentIndex.h"

namespace AgentModelData {

struct File {
    std::string path;
    AgentIndex::Place place = AgentIndex::Place::Outside;
    int writes = 0, edits = 0, failed = 0;
    bool exists = true;
    std::string folderText;   // as AgentView::folderText says
};

struct Group {
    AgentIndex::Place place = AgentIndex::Place::Main;
    std::string title;
    std::vector<File> files;
};

struct Work {
    std::string transcript, sessionId, agentId, parent;
    std::string name, title, agentType, cwd, gitBranch;
    std::string worktreePath, worktreeBranch, scratchpad;
    double started = 0, lastActivity = 0, written = 0;
    bool finished = false;
    int badLines = 0, filesTouched = 0, bashCommands = 0, depth = 0;
    std::vector<Group> groups;
    std::vector<Work> subagents;
    // Written in the last two minutes and not marked finished.
    bool runningAt(double now) const {
        return !finished && now - std::max(lastActivity, written) <= 120;
    }
    std::string whereText() const { return worktreeBranch.empty() ? gitBranch : worktreeBranch; }
};

struct Snapshot {
    std::vector<Work> sessions;     // newest first, subagents nested
    size_t sessionCount = 0;        // with the subagents
    std::vector<std::string> scratchpads;
    bool loaded = false;            // the first refresh has finished
    bool projectsMissing = false;   // the transcripts folder is not there
    std::string projectsDir, tmpRoot;
    unsigned generation = 0;
};

}  // namespace AgentModelData

class AgentModel {
public:
    // `folder` is the window's folder (any spelling).
    explicit AgentModel(const std::string& folder);
    ~AgentModel();   // stops the thread and the monitors

    // Called on the main loop whenever a new snapshot was published.
    std::function<void()> onChange;

    std::shared_ptr<const AgentModelData::Snapshot> snapshot() const;
    void setFolder(const std::string& folder);
    // Reread what changed (coalesced).
    void refresh();
    // The slow poll runs only while the panel shows.
    void setPanelShown(bool shown);
    const std::string& projectsDir() const { return projectsDir_; }
    const std::string& tmpRoot() const { return tmpRoot_; }

    static std::string defaultProjectsDir();
    static std::string defaultTmpRoot();

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
    std::string projectsDir_, tmpRoot_;
};
