// GitRun.h — running git for the Source Control panel (GitPanel.h), the GTK
// counterpart of the Mac's MCGitRunSync in src/GitPanel.mm. Only ever called
// on the panel's git thread: each call blocks until git is done or its time
// limit (Git::timeoutFor) is over.
//
// An argument vector, never a shell. stdin is /dev/null, or `input` through
// a pipe (git apply's patch). stdout and stderr are read together, so a
// chatty stderr cannot fill its pipe and stall git. git leads a process group
// of its own, so a time limit stops the hooks, filters and credential helpers
// it started along with it; a command that talks to a remote
// (Git::talksToRemote: fetch, pull, push) gets a session of its own too, as
// the Mac's POSIX_SPAWN_SETSID gives it, so it has no controlling terminal:
// ssh cannot ask for a passphrase or a host key on the terminal minicode was
// started from, and fails at once instead, as git does for a password with
// GIT_TERMINAL_PROMPT=0 (GitUi::gitEnvironment).
#pragma once

#include <string>
#include <vector>

#include "GitModel.h"

struct GitResult {
    int status = -1;
    std::string out, err;
    bool timedOut = false;   // stopped at its time limit; err says so
    bool ok() const { return status == 0; }
};

// Where git is, found once on PATH. Empty when it is not installed.
const std::string& gitExecutable();

// git `args` in `dir`, with `input` on its stdin when given, within `limit`
// seconds; past it git's group gets SIGTERM, then SIGKILL 2 seconds later,
// and the result says "git did not answer within 30 seconds: git status ...".
GitResult runGitWithin(const std::string& dir, const std::vector<std::string>& args,
                       double limit, const std::string* input = nullptr);
// With the Mac's limit for the command.
GitResult runGit(const std::string& dir, const std::vector<std::string>& args,
                 const std::string* input = nullptr);

// What to tell the user about a failed command (GitUi::failureText).
std::string gitFailure(const GitResult& r);

// What the panel shows about the folder at `root`: its top level, the
// status and the remotes (for Publish Branch, Fetch and autofetch). Read off
// the main thread.
GitUi::Snapshot gitSnapshotAt(const std::string& root);
