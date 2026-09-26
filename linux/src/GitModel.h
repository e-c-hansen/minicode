// GitModel.h — pure C++, no GTK. What the Linux Source Control panel shows,
// worked out from what git printed: the rows of the two change lists, the
// line about what is pushed, the argument vectors for every command it runs,
// where the selection goes after a refresh, the tooltips, and the diff and
// commit text with a style per run. The GTK half (GitPanel.cpp) runs git and
// draws; everything here is tested in tests/run_tests.cpp.
//
// The rules are the Mac's (src/GitPanel.mm and "Git panel" in CLAUDE.md),
// and the wording is the same.
#pragma once

#include "GitGraph.h"
#include "GitStatus.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace GitUi {

// ------------------------------------------------------------------ rows
// One line of the change lists: a heading ("Staged changes  2") or a file.
struct Row {
    bool header = false;
    std::string title;        // a heading's text
    bool staged = false;      // which list it is in
    char letter = ' ';
    std::string path;         // relative to the top level
    std::string origPath;     // a rename's source, or empty
    bool untracked = false;
    bool unmerged = false;
    std::string key() const;  // the same file in the same list
};

// What one refresh found.
struct Snapshot {
    bool inRepository = false;
    std::string topLevel;     // the repository's top level
    std::string gitDir;       // its .git directory (absolute), for watching
    std::string branchText;   // "main ↑2 ↓1", "(no commits yet)" added when empty
    bool initial = false;     // no commit yet
    std::vector<Row> rows;
    std::string errorText;    // a command failed
    std::string notice;       // one plain line where the list would be
    // For the graph.
    std::string headOid;      // empty before the first commit
    std::string branch;       // empty when detached
    bool detached = false;
    std::string upstream;     // empty when there is none
    bool hasAheadBehind = false;
    int ahead = 0, behind = 0;
};

// Fill the status part of a snapshot: branch line, headings and file rows
// (staged list first), "No changes." when both lists are empty.
void applyStatus(Snapshot& s, const Git::Status& st);

// `git rev-parse --show-toplevel --absolute-git-dir` printed two lines.
bool parseTopLevel(const std::string& out, std::string& top, std::string& gitDir);

// Whether the graph is shown for this snapshot: in a repository, with a
// commit, and with no status error.
bool graphWanted(const Snapshot& s);

// One line over the graph on what is pushed: "↑ 2 to push, ↓ 1 to pull,
// against origin/main", "Up to date with ...", no upstream, gone, detached.
std::string summaryText(const Snapshot& s);

// What to tell the user about a failed command: stderr without blank lines;
// else stdout's last non-blank line (a commit with nothing staged prints a
// whole status there and ends with the reason); else the exit status.
std::string failureText(int status, const std::string& out, const std::string& err);

// The first line of a successful commit's stdout: "[main 1a2b3c4] Subject".
std::string firstLine(const std::string& out);

// The next file row from `from` going `step` (+1 or -1), skipping headings;
// -1 when there is none. `from` may be -1 (nothing selected): then Down
// starts at the top and Up at the bottom.
int nextFileRow(const std::vector<Row>& rows, int from, int step);

// Where the selection goes after a refresh: the same file in the same list,
// else the row now at its place in that list (or the list's last), else the
// first file anywhere; -1 when nothing was selected or no file is left.
int pickAfterRefresh(const std::vector<Row>& oldRows, int oldSel,
                     const std::vector<Row>& rows);

// The last file row, or -1.
int lastFileRow(const std::vector<Row>& rows);

// "path, staged", "new (renamed from old), changed", "x, conflict".
std::string rowToolTip(const Row& r);

// Status letter colors (VS Code's git decorations), as 0xRRGGBB.
unsigned letterColor(char letter, bool unmerged);

// ----------------------------------------------------------- arguments
// Every command the panel runs, as argument vectors (no shell, ever). Paths
// always follow "--", and add, restore and rm take --literal-pathspecs so a
// file called "sp*ecial.txt" never matches "spXecial.txt" too.
std::vector<std::string> topLevelArgs();
std::vector<std::string> statusArgs();
std::vector<std::string> refArgs();
std::vector<std::string> logArgs(int limit, bool allBranches, bool compareUpstream);
std::vector<std::string> leftRightArgs();
// Space on a row: add -A in Changes; restore --staged in Staged (both halves
// of a rename); rm --cached --force before the first commit, where restore
// has no HEAD to restore from.
std::vector<std::string> stageArgs(const Row& r, bool initial);
// Return on a row: the staged, work tree or untracked (--no-index against
// /dev/null, where exit status 1 means "differs") diff.
std::vector<std::string> diffArgs(const Row& r);
std::vector<std::string> showArgs(const std::string& hash);
std::vector<std::string> commitArgs(const std::string& message);

// The environment every git gets on top of the app's own.
struct EnvVar { const char* name; const char* value; };
const std::vector<EnvVar>& gitEnvironment();

// ---------------------------------------------------------------- graph
// One load of the commit graph, built off the main thread and not changed
// after.
struct Graph {
    std::vector<Git::Commit> commits;
    std::vector<Git::GraphRow> rows;
    std::unordered_map<std::string, std::vector<Git::Ref>> labels;
    Git::Divergence divergence;
    std::string head;
    int maxWidth = 1;
    bool hasMore = false;     // the limit cut the history short
    int limit = 0;
    std::string key;          // what it was built from; same key, same graph
    std::string errorText;
};

constexpr int kGraphBatch = 200;

// Everything the graph depends on besides for-each-ref's output: HEAD,
// branch, whether it is compared with an upstream, the limit, the switch.
std::string graphKey(const Snapshot& s, int limit, bool all, bool compare,
                     const std::string& refsOutput);

// Whether the log includes @{upstream} and rev-list is asked: there is an
// upstream, it still exists, and HEAD is on a branch.
bool compareWithUpstream(const Snapshot& s);

// Fills commits, lanes, labels and the widest row from log and refs output.
void buildGraph(Graph& g, const Snapshot& s, const std::string& logOut,
                const std::string& refsOut, int limit);

// The width of one lane: 12 pixels, narrower when many lanes would take
// more than two fifths of the row, never under 4.
double laneWidth(double rowWidth, int lanes);

// Lane colors: the accent blue, then VS Code's graph colors. 0xRRGGBB.
unsigned laneColor(int i);

// A ref pill's color: HEAD orange, remote purple, tag amber, branch blue.
unsigned pillColor(Git::RefKind k);

// "a1b2c3d  Ann, 3 days ago (Sep 23, 2026 at 14:02)\nSubject", then "Not
// pushed yet..." or "Not pulled yet..." and the labels; the "Show more" row
// gets "Load the next 200 commits".
std::string graphToolTip(const Graph& g, size_t row, long long now);

// For tests and the debug log: "a1b2c3d lane=0 head out Subject [*main,v1]".
std::vector<std::string> graphDescriptions(const Graph& g);

// "Sep 23, 2026 at 14:02", in local time.
std::string dateText(long long t);

// -------------------------------------------------------- styled text
enum class Style { Plain, Added, Removed, Muted, FileHeader, Hash, Bold };

// Text with a style per run. Offsets are in characters (what GtkTextBuffer
// counts in), and the text is always valid UTF-8.
struct StyledText {
    struct Run { size_t start, length; Style style; };
    std::string text;
    std::vector<Run> runs;
    size_t chars = 0;
    void add(const std::string& s, Style style);   // s must be valid UTF-8
};

// Valid UTF-8 as it is; anything else read as Latin-1, one byte per
// character, so the bytes still show rather than nothing.
std::string validUtf8(const std::string& s);
size_t utf8Length(const std::string& s);

constexpr size_t kMaxDiffBytes = 4u << 20;

// A diff colored the Mac's way: added green, removed red, hunk headers and
// "\ No newline" muted, file headers muted bold. Past 4 MB it is cut at a
// line end with a muted note. Empty output: "No differences to show."
StyledText diffText(const std::string& bytes);
void appendDiff(StyledText& out, const std::string& bytes);

// `git show` output (showArgs): hash, parents, author, a different
// committer, date with relative time, the message with its subject bold,
// then the stat and diff. Falls back to diffText if it does not parse.
StyledText commitText(const std::string& show, long long now);

// "a1b2c3d Subject": the title of a commit view.
std::string commitTitle(const Git::Commit& c);

}  // namespace GitUi
