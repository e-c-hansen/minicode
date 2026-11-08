// GitStatus.h — pure C++. Reads what git prints, for the Source Control
// panel: `git status --porcelain=v2 --branch -z` into a branch and a list of
// changed files, and `git diff` output into files, hunks and lines with a
// kind each, for coloring.
//
// Nothing here runs git. Each port runs it (argument arrays, never a shell)
// and hands the bytes over, so this parses text and is tested on its own.
#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    // Ignored paths (`!` records, only asked for with --ignored), a folder
    // with its trailing '/'.
    std::vector<std::string> ignored;
};

// Parses `git status --porcelain=v2 --branch -z` output (NUL-terminated
// records). Records it does not know are skipped, so a newer git that adds
// some cannot break it. Ignored paths (`!`) go to `ignored`, apart from the
// entries.
Status parseStatus(const std::string& out);

// ------------------------------------------------------- the tree's colors
// What the file tree colors a row by, as VS Code's explorer decorations:
// weakest first, so a folder takes the strongest of what is inside it.
enum class Decoration { None, Ignored, Untracked, Added, Deleted, Modified, Conflict };

// "modified", "untracked", ...; "" for None.
const char* decorationName(Decoration d);

// The arguments of the one status the tree runs per refresh: an untracked
// folder as one entry and an ignored folder as one (--ignored=matching), so
// a node_modules costs a line, not a walk.
std::vector<std::string> decorationArgs();

// One entry's color: a conflict; untracked; a deletion in the index or the
// work tree; added (in the index, renamed, copied or intent to add);
// modified (or its type changed); else none.
Decoration decorationOf(const Entry& e);

// Every path's color from one status (paths relative to the top level, no
// trailing slash, "" being the top level itself).
class Decorations {
public:
    static Decorations fromStatus(const Status& st);
    // A file's color; a file inside an untracked or ignored folder has it too.
    Decoration file(const std::string& rel) const;
    // A folder's: its own when git names it (untracked, ignored, a
    // submodule), else the strongest of the changes inside it, deletions and
    // ignored files left out (VS Code does not propagate those either).
    Decoration folder(const std::string& rel) const;
    // Whether `rel` is ignored or inside an ignored folder: a change there
    // needs no new status.
    bool ignored(const std::string& rel) const;
    bool empty() const { return files_.empty() && untrackedDirs_.empty() && ignoredDirs_.empty(); }
    bool operator==(const Decorations& o) const;
    bool operator!=(const Decorations& o) const { return !(*this == o); }

private:
    Decoration underFolder(const std::string& rel, bool self) const;
    std::unordered_map<std::string, Decoration> files_;
    std::unordered_map<std::string, Decoration> folders_;
    std::unordered_set<std::string> untrackedDirs_, ignoredDirs_;
};

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

// ------------------------------------------------------- running, its limits
// How long each port lets one git command run before stopping it and its
// process group (hooks, filters and credential helpers included). Still no
// I/O here: the ports run git and ask these for the limit and the words.

// The subcommand in `args` (git's arguments, without "git"): the first word
// that is not an option, skipping the value of each -c and -C. "" if none.
std::string subcommand(const std::vector<std::string>& args);

// Seconds a command may take: 600 for commit (its hooks may run a test
// suite, its signing may wait on a passphrase) and push (its pre-push hook
// may too, and a slow network), 300 for pull, 120 for fetch, and for add,
// restore, rm, switch, stash, hash-object and update-index (clean and
// smudge filters such as Git LFS, a post-checkout hook, another process's
// index.lock), 30 for everything else, which only reads.
double timeoutFor(const std::vector<std::string>& args);

// Does the command talk to a remote (fetch, pull, push, ls-remote)? The
// ports start those in a session of their own, with no terminal, so ssh
// cannot ask for a passphrase on one either: with GIT_TERMINAL_PROMPT=0 as
// well, a missing credential fails at once instead of waiting.
bool talksToRemote(const std::vector<std::string>& args);

// "30 seconds", "2 minutes", "1 second", "0.5 seconds".
std::string durationText(double seconds);

// The command as the user would type it ("git status --porcelain=v2 ..."),
// line breaks made spaces and cut to 160 characters with an ellipsis, since
// a commit message can be long.
std::string commandLine(const std::vector<std::string>& args);

// "git did not answer within 30 seconds: git status ..."
std::string timeoutMessage(const std::vector<std::string>& args, double seconds);
// git exited but something it started still holds its output open:
// "git exited, but something it started kept its output open for 30
// seconds: git ..."
std::string heldOutputMessage(const std::vector<std::string>& args, double seconds);

}  // namespace Git
