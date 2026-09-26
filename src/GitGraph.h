// GitGraph.h — pure C++. The commit graph for the Source Control panel:
// reads what `git log`, `git for-each-ref`, `git rev-list --left-right` and
// `git show` print, and lays the history out in lanes the way
// `git log --graph` and VS Code's graph do.
//
// Nothing here runs git. The port runs it (argument arrays, never a shell)
// and hands the bytes over, so all of this is tested on its own.
#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Git {

// -------------------------------------------------------------------- log
struct Commit {
    std::string hash;                  // full object name
    std::vector<std::string> parents;  // first parent first; none for a root
    std::string author;
    long long time = 0;                // author date, seconds since 1970
    std::string subject;
};

// The format string that parseLog reads, for
// `git log -z --topo-order --format=<logFormat()>`: five NUL-separated fields
// per commit (hash, parents, author, author time, subject), and -z ends each
// commit with a NUL.
const char* logFormat();
std::vector<Commit> parseLog(const std::string& out);

// ------------------------------------------------------------------- refs
enum class RefKind { Head, Branch, Remote, Tag };

struct Ref {
    RefKind kind = RefKind::Branch;
    std::string name;     // short: "main", "origin/main", "v1.0", "HEAD"
    std::string target;   // the commit it points at (a tag is peeled)
    bool current = false; // the branch HEAD is on
};

// For `git for-each-ref --format=<refFormat()>`: object, peeled object,
// refname and symref, NUL-separated, one ref per line. Branches, remote
// branches and tags are kept; symbolic refs (origin/HEAD), stashes and notes
// are not.
const char* refFormat();
std::vector<Ref> parseRefs(const std::string& out);

// The labels to draw on each commit, keyed by hash: HEAD's branch first
// (marked current), or a "HEAD" label when detached, then other local
// branches, remote branches and tags, each group in name order.
// `headOid` empty means there is no commit yet.
std::unordered_map<std::string, std::vector<Ref>>
labelsByCommit(const std::vector<Ref>& refs, const std::string& headOid,
               const std::string& branch, bool detached);

// ------------------------------------------------------ pushed or not
// `git rev-list --left-right HEAD...@{upstream}`: "<hash" is on HEAD only
// (outgoing, not pushed), ">hash" on the upstream only (incoming).
struct Divergence {
    std::unordered_set<std::string> outgoing, incoming;
};
Divergence parseLeftRight(const std::string& out);

// ------------------------------------------------------------------ lanes
// One line in a row of the graph. A row is split at its middle, where the
// commit's dot is:
//   Pass  from lane `from` at the row's top edge to lane `to` at its bottom
//         edge (a line going past the commit; from != to when lanes shift)
//   In    from lane `from` at the top edge into the dot (the commit's child
//         above, or a branch merging into it)
//   Out   from the dot to lane `to` at the bottom edge, toward a parent
struct GraphEdge {
    enum Kind { Pass, In, Out } kind;
    int from = 0, to = 0;
    int color = 0;   // index into the port's palette, taken modulo its size
};

struct GraphRow {
    int lane = 0;    // the dot's column
    int color = 0;   // the dot's color
    int width = 0;   // columns used by this row (top edge, dot or bottom edge)
    std::vector<GraphEdge> edges;
};

// Lays out `commits`, which must be in topological order (children before
// parents, as `git log --topo-order` gives them). One row per commit.
// A commit no loaded child points at starts a new lane; a merge sends a line
// to each parent; several children of one commit meet at its dot. A parent
// that is not in `commits` (the window of loaded history ends first) keeps
// its line to the bottom of the last row. Empty lanes are closed up, so
// lines to the right move left.
std::vector<GraphRow> layoutGraph(const std::vector<Commit>& commits);

// ------------------------------------------------------------------- show
struct CommitDetail {
    std::string hash;
    std::vector<std::string> parents;
    std::string author, email;
    long long time = 0;
    std::string committer;     // empty when the same as the author
    std::string message;       // the whole message, trailing newlines trimmed
    std::string patch;         // what git printed after the header: stat and diff
};

// For `git show --format=<showFormat()> --stat --patch ...`.
const char* showFormat();
// False when the output does not start with the fields showFormat asks for.
bool parseShow(const std::string& out, CommitDetail& d);

// ------------------------------------------------------------------- text
// "just now", "5 minutes ago", "yesterday", "3 weeks ago", "2 years ago";
// a time in the future (a skewed clock) reads "just now". UTF-8.
std::string relativeTime(long long then, long long now);

// The first `n` characters of a hash.
std::string shortHash(const std::string& hash, size_t n = 7);

}  // namespace Git
