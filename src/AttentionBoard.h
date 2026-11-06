// AttentionBoard.h — pure C++. Which terminals are waiting for the user and
// which have finished (spec feature 7), and which Claude Code session runs
// in which terminal, as `minicode notify` reports them. The app keeps one
// board for all its windows (TerminalAttention.mm) and only draws it: the
// dot on a terminal's tab, its tooltip, the Dock badge, Ctrl+Cmd+W.
//
// Terminals are the control channel's ids (`minicode state`). A mark stays
// until a later event clears it, the user types in that terminal, or the
// terminal closes (forget).
#pragma once
#include "ClaudeHooks.h"
#include <map>
#include <string>
#include <vector>

namespace Attention {

enum class Mark { None, Waiting, Done };

// What a tab of several panes shows: waiting over done over nothing.
Mark strongest(Mark a, Mark b);

struct Session {
    std::string sessionId, transcriptPath, cwd;
    bool empty() const { return sessionId.empty() && transcriptPath.empty(); }
};

struct Entry {
    Mark mark = Mark::None;
    std::string message;    // a waiting terminal's Notification message
    Session session;        // the last one a notify named
    double sessionTime = 0; // when it was named
    double markTime = 0;    // when the mark was set
};

class Board {
public:
    // A notify for `terminal`. The session is recorded whenever it names
    // one. True when what is shown (the mark or its message) changed.
    bool apply(int terminal, ClaudeHooks::Action action, const std::string &message,
               const Session &session, double now);
    // The user typed in the terminal: its mark goes. True when there was one.
    bool typed(int terminal);
    // The terminal closed: everything about it goes. True when it had a mark.
    bool forget(int terminal);

    Mark mark(int terminal) const;
    std::string message(int terminal) const;
    // The session last named for the terminal; null when none was.
    const Session *session(int terminal) const;
    // The terminal a session runs in, by its id, else by its transcript (a
    // subagent's transcript names its parent's session): the one that named
    // it last when several did (a session resumed elsewhere). 0 for none.
    int terminalFor(const std::string &sessionId, const std::string &transcriptPath) const;

    int waitingCount() const;
    // The next waiting terminal after `current` in `order` (every terminal,
    // window by window and tab by tab), wrapping around, `current` itself
    // last. 0 when none is waiting.
    int nextWaiting(const std::vector<int> &order, int current) const;

private:
    std::map<int, Entry> entries_;
};

// Of the places a path in terminal output was found (R2.10), the ones under
// the terminal's session's own roots (its scratchpad, its worktrees), when
// any is: that session printed the path, so its copy is the one meant
// (R7.4). Otherwise every match. A flag per match.
std::vector<bool> preferSessionMatches(const std::vector<std::string> &matchPaths,
                                       const std::vector<std::string> &sessionRoots);

}  // namespace Attention
