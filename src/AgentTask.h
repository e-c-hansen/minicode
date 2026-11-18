// AgentTask.h — pure C++. The parts of New Agent Task that need no GUI:
// a short name from the prompt, that name made fit to be a git branch and a
// folder, a free one when it is taken, where the worktree goes, the git
// arguments that make it, the line typed into the terminal to start Claude
// Code there with the prompt, and the words the box shows. The Mac's
// AgentTaskBar.mm drives it.
//
// The flow: the user types a prompt (and keeps or edits the name made from
// it); minicode adds a worktree from HEAD at <folder>/.claude/worktrees/<name>
// on a new branch <name>, where Claude Code's own worktrees are and where the
// Agents panel and the file tree's Worktrees node look, then opens a
// terminal tab running `cd <worktree> && <agent.command> <prompt>`. The
// review and Apply to Main are the worktree's, as for any other.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace AgentTask {

// A name from the prompt: its first few words that say something (articles,
// "please" and the like left out), lowercased, ASCII letters and digits
// only, joined by '-', at most 32 bytes and cut at a word. "task" when
// nothing is left.
std::string nameFromPrompt(const std::string &prompt);

// `typed` made fit to be a branch and a folder name: letters, digits, '.',
// '_' and '-' kept, anything else (spaces, '/', quotes, non-ASCII) a '-';
// runs of '-' one, no "..", no '-' or '.' at either end, no ".lock" at the
// end, at most 60 bytes. "" when nothing is left.
std::string sanitizeName(const std::string &typed);

// `base` when it is free, else base-2, base-3, ... the first `taken` says
// no to.
std::string uniqueName(const std::string &base,
                       const std::function<bool(const std::string &)> &taken);

// <folder>/.claude/worktrees/<name>.
std::string worktreePath(const std::string &folder, const std::string &name);

// Whether `path` is in Claude Code's own folder, <home>/.claude, which
// minicode never writes (a window on the home folder would put its tasks
// there). Paths compared as given, with or without a trailing '/'.
bool insideClaudeHome(const std::string &path, const std::string &home);

// `worktree add -b <branch> <path> HEAD`, for git run without a shell.
std::vector<std::string> worktreeAddArgs(const std::string &branch, const std::string &path);

// Whether the prompt goes on the typed line itself: one line, at most 160
// bytes, no control characters. A longer one is read from a file, so the
// line, the tab's title and the shell's history stay short (Request a
// Change's lesson).
bool promptFitsOnLine(const std::string &prompt);

// The line typed into the terminal: `cd <dir> && <command> <prompt>` with
// the prompt shell-quoted when it fits on the line, else `cd <dir> &&
// <command> "$(cat <promptFile>)"`. The folder and the file are quoted
// too; `command` is the agent.command setting, a whole command line
// ("claude" by default), typed as written. Nothing of the prompt ever
// reaches the shell unquoted.
std::string startCommand(const std::string &command, const std::string &dir,
                         const std::string &prompt, const std::string &promptFile);

// The line under the box's heading: where the task starts from, and that
// uncommitted changes stay behind. `branch` is "" on a detached HEAD;
// `changed` is how many paths `git status --porcelain` listed.
// "From the last commit on main" or "From the last commit on main; your 3
// uncommitted changes stay here and are not in the task's worktree".
std::string startsFromText(const std::string &branch, int changed);

// The prompt as given to Claude Code: trimmed of blank lines and spaces at
// both ends, nothing else changed.
std::string trimPrompt(const std::string &prompt);

}  // namespace AgentTask
