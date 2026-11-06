// AgentView.h — pure C++17, no dependencies. The small decisions behind the
// Agents panel, the worktrees in the file tree and the Evidence view
// (AgentsPanel.mm, AgentWindow.mm, AgentPages.mm), kept here so they are
// tested: how a session's files are grouped, the words a row shows, what
// `git diff --name-status -z` and `git worktree list` mean for a worktree,
// and how a test summary is put into words. The model itself is
// AgentIndex.h and AgentEvidence.h.
#pragma once
#include "AgentEvidence.h"
#include "AgentIndex.h"

#include <string>
#include <vector>

namespace AgentView {

// ------------------------------------------------------------ the panel
// A session's written or edited files in the panel's three groups (R2.6),
// in the order In the folder, In its worktree, Outside the folder; empty
// groups left out, and files only read or whose every call failed left out.
struct FileGroup {
    AgentIndex::Place place;
    std::vector<size_t> files;   // indexes into Session::files, in the order first touched
};
std::vector<FileGroup> groupFiles(const std::vector<AgentIndex::FileTouch> &files);

// The folder a file row shows beside its name: from the window's folder
// ("src", "" at its top) for a file in it, from its worktree for one in a
// worktree (the session's own, else whichever .claude/worktrees/<x> holds
// it), "scratchpad/..." in the session's scratchpad, else the whole folder
// with the home folder as "~".
std::string folderText(const std::string &path, AgentIndex::Place place,
                       const std::string &folder, const std::string &worktree,
                       const std::string &scratchpad, const std::string &home);

// "3 files, 12 commands", "1 file, 1 command", "no files, 2 commands",
// "no files or commands".
std::string countsText(int files, int commands);

// The row's second line, from its parts: state ("running" or "finished"),
// the age as the GUI words it ("5 min"), where it works (the worktree's
// branch, else the session's own branch), and the counts. Empty parts are
// left out; they are joined with " · ".
std::string detailLine(bool running, const std::string &age, const std::string &where,
                       int files, int commands);

// What the panel shows when it has no rows.
//   projectsMissing: the transcripts folder is not there.
std::string emptyText(bool projectsMissing, bool loaded, const std::string &projectsDir);

// --------------------------------------------------------- the worktrees
// A worktree row's badge: "3 ahead · modified", "up to date", "1 ahead",
// "modified"; "" while git has not answered (ahead < 0 and not dirty).
std::string worktreeBadge(int ahead, bool dirty);

// The branch a worktree is compared with (R2.9): the main worktree's branch
// ("main"), or its commit when it is detached; "" when there is none.
std::string mainRef(const std::vector<AgentIndex::Worktree> &all);

// One changed file of a worktree against its main branch.
struct Change {
    char status = 'M';      // M, A, D, R, C, T, U, or '?' for untracked
    std::string path;       // as git prints it, relative to the worktree
    std::string from;       // a rename's or copy's old path
};
// `git diff --name-status -z -M <base>` (status, NUL, path, NUL; R and C
// with a score and two paths), then `git ls-files --others
// --exclude-standard -z` as untracked files. Sorted by path, no duplicates.
std::vector<Change> parseChanges(const std::string &nameStatusZ, const std::string &untrackedZ);

// ---------------------------------------------------------- the evidence
// A test run's summary in words, failures first: "1 failed, 3630 passed",
// "12 passed, 1 skipped", "2 errors, 5 passed"; "" when none was found.
std::string summaryText(const AgentEvidence::Summary &s);

// What the Evidence view says about a command's result: "running" (no
// result yet), "error" or "ok".
const char *resultWord(const AgentEvidence::Command &c);

// Lines between an output's head and tail, as the expander says it:
// "1,234 more lines", "1 more line", or "more lines" when unknown (-1).
std::string moreLinesText(long long omitted);

// A line of output cut for display: at most `max` bytes, on a UTF-8
// character boundary, with "…" added when cut.
std::string clipLine(const std::string &line, size_t max = 400);

// ------------------------------------------------- paths in the terminal
// The "not found in …" note (R2.11): the path as written and the roots
// tried, as "Not found: src/a.py (looked in ~/p, ~/p/.claude/worktrees/x)".
// `home` is shortened to "~".
std::string notFoundText(const std::string &target, const std::vector<std::string> &tried,
                         const std::string &home);

}  // namespace AgentView
