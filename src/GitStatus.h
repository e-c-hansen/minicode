// GitStatus.h — pure C++. Reads what git prints, for the Source Control
// panel: `git status --porcelain=v2 --branch -z` into a branch and a list of
// changed files, and `git diff` output into files, hunks and lines with a
// kind each, for coloring.
//
// Nothing here runs git. Each port runs it (argument arrays, never a shell)
// and hands the bytes over, so this parses text and is tested on its own.
#pragma once
#include <string>
#include <vector>

namespace Git {

// ------------------------------------------------------------------ status
struct Entry {
    std::string path;       // relative to the repository's top level
    std::string origPath;   // a rename or copy's source; empty otherwise
    // The two columns of `git status`: X is the index against HEAD, Y the
    // work tree against the index. '.' means unchanged; otherwise M, T, A,
    // D, R, C (and U in both for a conflict).
    char staged = '.';
    char unstaged = '.';
    bool untracked = false;
    bool unmerged = false;  // a conflict still to resolve
    bool submodule = false;
};

struct Status {
    std::string branch;     // empty when HEAD is detached
    bool detached = false;
    std::string oid;        // HEAD's commit, empty before the first commit
    bool initial = false;   // no commit yet (an empty repository)
    std::string upstream;   // empty when there is none
    bool hasAheadBehind = false;
    int ahead = 0, behind = 0;
    std::vector<Entry> entries;   // in git's order
};

// Parses `git status --porcelain=v2 --branch -z` output (NUL-terminated
// records). Records it does not know are skipped, so a newer git that adds
// some cannot break it. Ignored files (`!`) are left out.
Status parseStatus(const std::string& out);

// Whether an entry belongs in the "Staged" list, the "Changes" list, or
// both (a file changed again after it was staged).
bool isStaged(const Entry& e);
bool hasUnstaged(const Entry& e);

// The one letter shown beside a file: its staged change in the staged list,
// otherwise its work tree change. Untracked files are 'U' and conflicts 'C'.
char stagedLetter(const Entry& e);
char unstagedLetter(const Entry& e);

// "main", "main ↑2 ↓1", or "HEAD (detached at 1a2b3c4)". UTF-8.
std::string branchLabel(const Status& s);

// -------------------------------------------------------------------- diff
enum class LineKind {
    FileHeader,   // diff --git, index, ---, +++, mode and rename lines
    HunkHeader,   // @@ -1,3 +1,4 @@
    Context,
    Added,
    Removed,
    NoNewline,    // "\ No newline at end of file"
    Other,        // "Binary files ... differ", or text outside any file
};

struct DiffLine {
    LineKind kind;
    std::string text;   // the whole line as git printed it, no newline
    int oldLine = 0;    // 1-based line in the old file, 0 when not in it
    int newLine = 0;    // 1-based line in the new file, 0 when not in it
};

struct Hunk {
    int oldStart = 0, oldCount = 0, newStart = 0, newCount = 0;
    std::string section;   // text after the closing @@ (a function name)
    std::vector<DiffLine> lines;   // the header line first
};

struct FileDiff {
    std::string oldPath, newPath;   // without a/ and b/; empty for /dev/null
    bool isNew = false, isDeleted = false, isRename = false, binary = false;
    std::vector<DiffLine> header;   // diff --git ... up to the first hunk
    std::vector<Hunk> hunks;
};

// Every line of a diff, in order, each with its kind. A combined diff (a
// conflict, `diff --cc`) is understood too: a line is Added or Removed when
// any of its columns is. Lines inside a hunk are counted against the hunk
// header, so a removed line that reads "-- x" is never taken for a header.
std::vector<DiffLine> classifyDiff(const std::string& out);

// The same, grouped into files and hunks.
std::vector<FileDiff> parseDiff(const std::string& out);

// A path as git writes it in a diff header: plain, or in double quotes with
// C escapes (\t, \", \\, \303\251) when it holds unusual characters.
std::string unquotePath(const std::string& s);

}  // namespace Git
