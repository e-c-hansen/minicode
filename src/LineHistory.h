// LineHistory.h — pure C++17. Who last changed each line of a file: blame
// that knows Claude Code's sessions (the editor's Show Line History, A7).
//
// A line is one of three things:
//
// - a commit: what `git blame --porcelain` says (short hash, author, when,
//   the commit's subject);
// - an uncommitted change a Claude Code session made: one of the session's
//   Edit, MultiEdit or Write calls (AgentEdits.h) put that line's text
//   there, and nothing has changed it since;
// - uncommitted and from no session: "not committed".
//
// Nothing here runs git or reads a transcript: the port runs `git blame
// --porcelain --contents - -- <file>` with the editor's text on stdin (so
// the lines are the buffer's, unsaved edits included) and hands the bytes
// to parsePorcelain, and gives each session's changes (AgentEdits::collect)
// to linesWritten, which says which of the text's lines each change wrote.
//
// A session is credited with a line only when that is exact. Walking the
// session's changes from the last to the first, each change's new text
// must be found exactly once in the text as it was just after it (the
// file now, with the later changes undone in turn); the lines of that
// place that differ from the text it replaced (a line diff of the two, so
// the unchanged lines an Edit quotes around its change are not counted)
// are the change's, as long as no later change of the session took them
// first and they are still in the file now. A change whose text is not
// found once (changed since, or found twice), an edit that only removed
// text, a rewrite of a file the session did not make (the transcript does
// not hold what was there before), and a notebook cell end the walk; it
// starts again from the file now for the earlier changes, so a change
// still standing on its own is still found. Several sessions on one file:
// the latest change wins a line. And only an uncommitted line can be a
// session's: a committed line is its commit's.
//
// Lines are counted as git counts them: each ends at "\n", and a last line
// without one is a line too (a text ending in "\n" has no empty line after
// it). Text is UTF-8.
#pragma once
#include "AgentEdits.h"

#include <string>
#include <vector>

namespace LineHistory {

// ------------------------------------------------------------------ blame
struct Commit {
    std::string hash;          // 40 hex digits; all zeros when not committed
    std::string author;
    std::string authorMail;    // with its angle brackets, as git prints it
    long long time = 0;        // author time, seconds since 1970
    std::string summary;       // the subject line
    bool boundary = false;     // the oldest commit blame reached
    bool uncommitted() const;
};

struct Blame {
    std::vector<Commit> commits;     // each once, in the order first seen
    std::vector<int> lineCommit;     // per line (0-based), an index into commits
};

// `git blame --porcelain` output. False when it does not read (empty
// output, or a header that is not one); a file of no lines reads as an
// empty Blame.
bool parsePorcelain(const std::string &out, Blame *blame);

// ------------------------------------------------------ session edits
// Line starts of `text` (git's lines: see above); one entry per line.
std::vector<size_t> lineStarts(const std::string &text);
size_t lineCount(const std::string &text);

// For each line of `current`, the index (into `changes`) of the change that
// wrote it, or -1. Refused changes are passed over.
std::vector<int> linesWritten(const std::vector<AgentEdits::Change> &changes,
                              const std::string &current);

// ------------------------------------------------------- the answer
enum class Kind { Unknown, Commit, Session, NotCommitted };

struct Line {
    Kind kind = Kind::Unknown;
    int commit = -1;           // Kind::Commit: an index into Blame::commits
    int session = -1;          // Kind::Session: an index into the sessions given
    int change = -1;           //   and the change in that session's list
    bool operator==(const Line &o) const {
        return kind == o.kind && commit == o.commit && session == o.session;
    }
    bool operator!=(const Line &o) const { return !(*this == o); }
};

struct SessionLines {
    const std::vector<AgentEdits::Change> *changes = nullptr;
    std::vector<int> lines;    // linesWritten's answer
};

// The answer for each of `lineCount` lines. `blame` may be null (no
// repository, or a file git does not know): every line is then
// uncommitted. A blame of another line count was not for this text, and
// every line is Unknown. A line a session wrote is that session's only
// while blame says it is not committed; of several sessions, the one whose
// change is latest wins.
std::vector<Line> combine(size_t lineCount, const Blame *blame,
                          const std::vector<SessionLines> &sessions);

// --------------------------------------------------------------- words
// What a session is called: its title, else its first prompt.
struct SessionInfo {
    std::string title;
    std::string transcript;
};

// How a port writes a date (GitUi::dateText's shape).
using DateFormat = std::string (*)(long long t);

// The first line of a prompt with words, the harness's tags dropped,
// whitespace collapsed, cut at `max` characters.
std::string promptLine(const std::string &prompt, size_t max = 100);

// Beside a line, on one line:
//   "a1b2c3d Ann Lee, 3 days ago · Fix the parser"
//   "Claude Code: Add a perimeter method, 2 hours ago"
//   "Not committed"
std::string commitLabel(const Commit &c, long long now);
std::string sessionLabel(const SessionInfo &s, const AgentEdits::Change &c, long long now);
std::string notCommittedLabel();

// The hover's words, several lines:
//   "a1b2c3d · Ann Lee · 3 days ago (Sep 23, 2026 at 14:02)\nFix the parser\nClick to open the commit."
//   "Written by a Claude Code session, not committed\nAdd a perimeter method\nPrompt: ...\nEdit, 2 hours ago (...)\nClick to see what the session changed in this file."
//   "Not committed\nNo Claude Code session's edit wrote this line."
std::string commitDetail(const Commit &c, long long now, DateFormat date);
std::string sessionDetail(const SessionInfo &s, const AgentEdits::Change &c, long long now,
                          DateFormat date);
std::string notCommittedDetail();

}  // namespace LineHistory
