// ChangeReview.h — pure C++, part of the portable core. The review view of
// uncommitted changes (the Mac's ChangeReviewPage), as Zed's project diff
// and VS Code's per-change Stage and Revert buttons show them: every
// changed file in one scroll, each file a header and then its changes, one
// block per run of changed lines (never git's three lines of context, which
// made three nearby edits one hunk), unchanged stretches folded to a row
// that says how many lines it hides, removed lines before added ones, with
// the words that changed marked and the file's syntax colors on both.
//
// It also answers what staging, unstaging or discarding one block (or the
// changed lines a selection covers) makes of the file: the index's new
// text, or the work tree's. The Mac writes the index text as the change
// bars' Stage Change does (hash-object, then update-index, after checking
// the entry still names the object the view was drawn from) and the work
// tree's through the atomic writer after a copy goes to the Trash, so
// nothing needs `git apply` or a patch.
//
// A file is given as its two texts: the staged list's HEAD and index, the
// Changes list's index and work tree, or (Section::Plain, no buttons) any
// two versions, such as a commit's parent and the commit. Tested in
// tests/change_review_tests.inc (`review:*`).
#pragma once
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "GitModel.h"
#include "SideBySide.h"

namespace ChangeReview {

// Which list a file is in, so which actions its blocks have: Staged
// (Unstage), Changes (Stage, Discard), Plain (none: a commit, a worktree).
enum class Section { Staged, Changes, Plain };

struct File {
    std::string path;         // relative to the top level
    std::string origPath;     // a rename's source, or empty
    Section section = Section::Changes;
    char letter = 'M';        // the panel's status letter
    bool untracked = false;   // not in the index yet
    bool conflict = false;    // unmerged
    bool submodule = false;   // a gitlink
    bool binary = false;      // a NUL in either version
    bool tooLarge = false;    // past what the view reads
    bool symlink = false;     // a symbolic link (mode 120000): its target, not text
    bool oldExists = true;    // a new file has no old version
    bool newExists = true;    // a deleted one no new version
    std::u16string oldText, newText;
    std::string key() const;  // section and path: the same file in the same list
};

// Why a file's changes go only whole (the header's Stage File, Discard
// File), "" when its blocks can be taken one by one. Files git cannot
// stage by part: new and deleted ones, binary files, submodules,
// conflicts, and one too large to show.
std::string wholeOnlyReason(const File& f);

// One file's changes, worked out once (aligning and coloring are the
// costly part; laying the page out again from them is not).
struct FileDiff {
    File file;
    std::vector<SideBySide::Row> rows;     // aligned, "\r" before "\n" ignored
    std::vector<SideBySide::Hunk> blocks;  // each run of rows that are not Same
    std::vector<size_t> oldStarts, newStarts;  // where each line starts, then the end
    std::vector<SideBySide::Span> oldSyntax, newSyntax;   // style: a TokenStyle
    size_t added = 0, removed = 0;         // lines
    bool lastNewlineChanged = false;       // only one side ends in "\n"
};

// `ext` picks the syntax colors (none when empty or unknown, or past 2 MB).
FileDiff diffFile(File f, const std::string& ext);

// The texts' lines as a page shows them: without "\n" and a "\r" before it.
std::u16string oldLine(const FileDiff& d, int line);
std::u16string newLine(const FileDiff& d, int line);

// --------------------------------------------------------------- acting
enum class Op { Stage, Unstage, Discard };
std::vector<Op> opsFor(Section s);   // Changes: Stage, Discard; Staged: Unstage

// The old text with the chosen changes made: each old line removed in the
// diff and marked in `takeOld` is left out, each new line added and marked
// in `takeNew` goes in, everything else is the old text's. Unchanged lines
// come from `baseNew` ? the new text : the old, and lines from the other
// side take the base's line breaks when it has only one kind (CRLF stays
// CRLF), so only the chosen lines change.
std::u16string pick(const FileDiff& d, const std::vector<bool>& takeOld,
                    const std::vector<bool>& takeNew, bool baseNew);

// What `op` on block `block` makes: Stage, the index with that block in
// it (old text plus the block); Unstage, the index without it (the new
// text less the block); Discard, the work tree without it.
std::u16string blockResult(const FileDiff& d, size_t block, Op op);
// The same for changed lines chosen by a selection: old lines (removed)
// and new lines (added), 0-based; lines that are not changed are ignored,
// and a line of a changed pair (an old line replaced by a new one) takes
// its partner, so choosing the new line takes that line's change.
std::u16string linesResult(const FileDiff& d, const std::vector<int>& oldLines,
                           const std::vector<int>& newLines, Op op);
// Whether any of those lines is a changed one.
bool anyChanged(const FileDiff& d, const std::vector<int>& oldLines,
                const std::vector<int>& newLines);

// The question, the words afterwards, or why it cannot be done (refusal),
// for `op` on a block (or on lines) of `d`. Discard always asks.
GitUi::ActionPlan plan(const FileDiff& d, Op op, bool lines);

// What a failed action says when the file or the index moved since the
// view was drawn: nothing was done, and the view shows it as it is now.
std::string staleText(const std::string& path, bool index);

// ------------------------------------------------------------- the page
enum class RowKind {
    Section,   // "Staged Changes", "Changes"
    File,      // a file's header: its path and "+12 -3"; the buttons over it
    Context,   // an unchanged line
    Removed,   // an old line
    Added,     // a new line
    Gap,       // "14 unchanged lines", opened by a click
    Note,      // a line about the file: binary, a conflict, too large, ...
};

struct PageRow {
    RowKind kind = RowKind::Context;
    size_t file = 0;          // index into the files (Section: the first file after it)
    int block = -1;           // Removed and Added: its block
    bool blockStart = false;  // the block's first row: its buttons go here
    int oldLine = -1, newLine = -1;   // 0-based, -1 where that side has none
    int gap = -1;             // Gap: its id (opened.count(id) shows it)
    size_t hidden = 0;        // Gap: how many lines
};

struct Page {
    std::u16string text;               // one line per row, joined by "\n"
    std::vector<size_t> starts;        // where each row starts
    std::vector<PageRow> rows;
    std::vector<SideBySide::Span> syntax;   // style: a TokenStyle
    std::vector<SideBySide::Span> words;    // style 0: removed, 1: added
    std::vector<size_t> blockRows;     // the row each block starts on, in page order
};

// The opened gaps, by file key: a gap's id is the new line its hidden lines
// start on, so it stays open while the file's other blocks come and go.
using Opened = std::map<std::string, std::set<int>>;

// Lays the files out in order, a section row before the first file of each
// section (none for Plain), `context` unchanged lines kept beside each block
// and the rest folded (a single hidden line is shown rather than folded).
Page layout(const std::vector<FileDiff>& files, const Opened& opened, size_t context = 3);

// "src/a.py  +12 -3", "new.txt (new)  +4", "b.txt (renamed from a.txt)".
std::u16string fileHeaderText(const FileDiff& d);
std::string sectionTitle(Section s, size_t files);   // "Staged Changes  2"
std::string gapText(size_t hidden);                  // "14 unchanged lines"

// F7 and Shift+F7 across files: the next or previous block's row after or
// before `row`, wrapping; -1 with no blocks.
long stepBlock(const Page& p, long row, bool forward);

// Where the eye is, so it stays there when the page is laid out again
// after an action: the file (by key, and by place in case it went), and a
// line within it.
struct Anchor {
    std::string fileKey;
    size_t fileIndex = 0;
    int newLine = -1, oldLine = -1;   // -1: the file's header
    long offset = 0;                  // rows below that line
    bool valid = false;
};
Anchor anchorAt(const Page& p, const std::vector<FileDiff>& files, size_t row);
size_t rowFor(const Page& p, const std::vector<FileDiff>& files, const Anchor& a);
// The header row of the file with this key, or -1.
long fileRow(const Page& p, const std::vector<FileDiff>& files, const std::string& key);

// ------------------------------------------------------ reading from git
// The index entries and HEAD's objects of the files shown, and their blobs
// in one `cat-file --batch`, rather than a git per file.
std::vector<std::string> lsFilesArgs(const std::vector<std::string>& paths);
std::vector<std::string> lsTreeArgs(const std::string& commit,
                                    const std::vector<std::string>& paths);
std::vector<std::string> batchArgs();   // cat-file --batch, object names on stdin

struct Entry {
    std::string mode, object;
};
// `ls-files --stage -z`: stage-0 entries by path (a conflict's stages are
// left out).
std::map<std::string, Entry> parseLsFiles(const std::string& out);
// `ls-tree -r -z`: blobs and gitlinks by path.
std::map<std::string, Entry> parseLsTree(const std::string& out);
// `cat-file --batch` output: each object's bytes by name; missing ones
// left out.
std::map<std::string, std::string> parseBatch(const std::string& out);

// A commit as the review shows it (Section::Plain, no buttons): its header
// (`show -s`, read by Git::parseShow) and what it changed against its first
// parent, or against nothing for a root commit, as diff-tree's raw records,
// whose object names the blobs come from.
std::vector<std::string> commitHeaderArgs(const std::string& commit);
std::vector<std::string> commitDiffArgs(const std::string& commit, const std::string& parent);

// One raw record of `diff-tree -r -z -M` (or `diff --raw -z -M`): modes
// "000000" where a side has no file, object names all zeros for the work
// tree's side of a `git diff <commit>`.
struct RawChange {
    std::string oldMode, newMode, oldObject, newObject;
    char letter = 'M';        // M, A, D, R, C, T
    std::string path;         // the new path
    std::string origPath;     // a rename's or copy's source
};
std::vector<RawChange> parseRawDiff(const std::string& out);
// Whether an object name is the all-zeros one (no object: the work tree).
bool nullObject(const std::string& object);

// The line under a commit's subject: "Ann, 3 days ago (Sep 23, 2026 at
// 14:02)  a1b2c3d", and for a merge that the changes are against its first
// parent, which is what the merge brought in.
std::string commitByline(const std::string& author, long long time, const std::string& hash,
                         size_t parents, long long now, GitUi::DateFormat date = GitUi::dateText);

// Whether bytes read from git or the disk hold a NUL in their first 8 KB
// (and are not UTF-16 with its byte order mark): a binary file.
bool looksBinary(const std::string& bytes);

}  // namespace ChangeReview
