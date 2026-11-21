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
#include "MergeConflicts.h"
#include "SideBySide.h"

namespace ChangeReview {

// Which list a file is in, so which actions its blocks have: Staged
// (Unstage), Changes (Stage, Discard), Plain (none: a commit, a worktree).
enum class Section { Staged, Changes, Plain };

// One version of a picture, as the page shows a binary image file: its
// size in pixels (read by the GUI from the bytes) and in bytes.
struct ImageSide {
    bool shown = false;   // this version exists and is shown
    int width = 0, height = 0;
    size_t bytes = 0;
};

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
    // A binary file whose versions are pictures (showAsImages): the page
    // shows them, side by side when both exist, in `imageRows` rows.
    bool image = false;
    ImageSide oldImage, newImage;
    size_t imageRows = 0;
    std::string key() const;  // section and path: the same file in the same list
};

// ------------------------------------------------------------ pictures
// A binary file with a picture's extension (FileIcon::isImageExtension:
// png, jpg, gif, webp, tiff, bmp, heic, ico, icns; SVG is text, and its
// diff is shown as text) is shown as its pictures, as VS Code shows an
// image diff: a new file's picture, a deleted one's, or the two side by
// side under "Before" and "After", each with its size in pixels and in
// bytes. A picture is shown at its pixel size, never taller than
// kImageMaxHeight points (scaled down to fit, never up), and one of more
// than kImageMaxPixels pixels is not decoded at all: the file keeps the
// binary file's note, as does one whose bytes did not read as a picture.
constexpr double kImageMaxHeight = 320;
constexpr double kImageLabelHeight = 22;   // the label line over each picture
constexpr long long kImageMaxPixels = 64LL * 1000 * 1000;

bool imagePath(const std::string& path);
// Makes `f` a file shown as pictures when it is binary (and not too large,
// a submodule, a link or a conflict), its path is a picture's, and each
// version it has (oldExists, newExists) was read as a picture of at most
// kImageMaxPixels pixels: `oldSide` and `newSide` give their sizes in
// pixels (0 when unread) and bytes. The rows it takes follow from the
// page's row height. True when it is now shown as pictures.
bool showAsImages(File& f, ImageSide oldSide, ImageSide newSide, double rowHeight);
// How tall a version is drawn, in points.
double imageHeight(const ImageSide& s);
// The rows the pictures of `f` take: the label line, the taller picture,
// and a margin under it.
size_t imageRows(const File& f, double rowHeight);
// "Before: 640 × 480 pixels, 12.4 KB" over the old version when both are
// shown ("After: ..." over the new); alone, "640 × 480 pixels, 12.4 KB".
std::string imageLabel(const File& f, bool newSide);
// "1 byte", "75 bytes", "12.4 KB", "3.1 MB" (1024 to a KB).
std::string byteText(size_t bytes);

// Why a file's changes go only whole (the header's Stage File, Discard
// File), "" when its blocks can be taken one by one. Files git cannot
// stage by part: new and deleted ones, binary files, submodules,
// conflicts (whose blocks have Keep buttons instead), and one too large
// to show.
std::string wholeOnlyReason(const File& f);

// Which part of a conflict block a line of a conflicted file is.
enum class ConflictPart { None, CurrentMarker, Current, BaseMarker, Base, Separator, Incoming,
                          IncomingMarker };

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
    // A conflicted file (conflictFile): its blocks, one per entry of
    // `blocks`, and each row's part of one (None outside them).
    std::vector<MergeConflicts::Block> conflicts;
    std::vector<ConflictPart> parts;
};

// `ext` picks the syntax colors (none when empty or unknown, or past 2 MB).
FileDiff diffFile(File f, const std::string& ext);

// A conflicted file in the Changes list, `f.newText` its text in the work
// tree: one row per line (both sides the same line), each conflict block a
// block of its own, shown whole with its markers, the rest folded as
// unchanged lines are. A file that cannot be read as text (binary, too
// large, gone from the work tree) keeps only its note.
FileDiff conflictFile(File f, const std::string& ext);
// The work tree's text with conflict `block` resolved by keeping `keep`.
std::u16string conflictResult(const FileDiff& d, size_t block, MergeConflicts::Keep keep);
// A conflicted file that is text and has no conflict blocks left: staging
// it marks it resolved, and nothing needs asking.
bool canMarkResolved(const FileDiff& d);

// The syntax colors of each line of `text` (lines split at "\n", a "\r"
// before it dropped, as InlineAgent::pendingDiff splits them), each span's
// start counted from its line's start: for a page that shows lines one by
// one, such as a session's changes to a file. Empty for an unknown `ext`
// or a text past 2 MB.
std::vector<std::vector<SideBySide::Span>> syntaxByLine(const std::u16string& text,
                                                        const std::string& ext);

// How many files the diffs are about: a file with staged and unstaged
// changes is in both lists, and counted once.
size_t fileCount(const std::vector<FileDiff>& diffs);

// A fingerprint of what the diffs show (each file's list, path, status and
// both texts): two reads that show the same give the same number, so a
// note about the last action can go once the changes have moved on.
size_t fingerprint(const std::vector<FileDiff>& diffs);

// The same over only the diffs about `paths` (a file's path or a rename's
// source); every diff when `paths` is empty.
size_t fingerprintOf(const std::vector<FileDiff>& diffs, const std::vector<std::string>& paths);

// The note at the top of the page about the last action ("Staged g.py."):
// it speaks of `paths` (every file when empty) on the commit HEAD named as
// the action ran. It stays while the page shows those files as the
// action left them, and goes once they show otherwise (a commit, a
// discard, an unstage, an edit or git from outside) or HEAD has moved (no
// action of the page's moves HEAD). The page delivers reads in the order
// they were started, after any action queued before them, so every read
// shown after the note is the action's result or later: the first one
// settles what the action left, unless that was known at once (a refusal
// changed nothing: `settle` with the page as it is).
struct ActionNote {
    std::vector<std::string> paths;
    bool headKnown = false;
    std::string head;
    bool settled = false;
    size_t print = 0;
    void settle(const std::vector<FileDiff>& diffs);
    // A read going on show (one that failed is not asked about): true when
    // the note goes with it.
    bool goesWith(const std::vector<FileDiff>& diffs, bool readHeadKnown, const std::string& readHead);
};

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
    Conflict,  // a line of a conflicted file's block: its `part` says which
    Image,     // an empty row under a picture file's header: the GUI draws the pictures over them
};

struct PageRow {
    RowKind kind = RowKind::Context;
    size_t file = 0;          // index into the files (Section: the first file after it)
    int block = -1;           // Removed and Added: its block
    bool blockStart = false;  // the block's first row: its buttons go here
    int oldLine = -1, newLine = -1;   // 0-based, -1 where that side has none
    int gap = -1;             // Gap: its id (opened.count(id) shows it)
    size_t hidden = 0;        // Gap: how many lines
    ConflictPart part = ConflictPart::None;   // Conflict: which part of the block
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
// A page button's name for a screen reader (its accessibility label): the
// button's title, then what it acts on. A block's (`block` its index in the
// file, from 0): "Stage change 1 in app.py"; a conflict's: "Keep Current for
// conflict 2 in a.py"; a file's (`block` -1): "Stage File app.py".
std::string buttonLabel(const std::string& title, const std::string& path, int block,
                        bool conflict = false);

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
// Where a drawn button's row is in another layout of the same changes (a
// read landing between a press and its action, a stretch opened above):
// the header row of the file with this key for a file's button (`block`
// -1), else the first row of its block `block` (a conflict's too); -1
// when that layout has no such row.
long buttonRow(const Page& p, const std::vector<FileDiff>& files, const std::string& key, int block);

// Where the eye goes after Stage, Unstage or Discard on a block (or Stage
// File, Discard File, ... on a file's header row), so the page does not
// jump: the GUI puts the row rowAfterAction gives where the acted-on row
// was on screen. That row is the next block of the same file at or after
// the acted-on block's place, counted in the lines of the side the action
// leaves alone (the work tree's for Stage, the index's for Discard,
// HEAD's for Unstage), so the block that followed takes its place; past
// the file's last block, the row that followed the file (the next file's
// header); at the page's end, the file's last block. A file that went
// from its list (its last change staged, a whole file staged or
// discarded) gives way to the nearest file after it that is still there,
// else the nearest before it. Keys, not places: a staged list appearing
// above moves every file down.
struct ActedPlace {
    std::string fileKey;
    bool newSide = true;   // which side's lines `line` counts
    long line = -1;        // the block's place on that side; -1: the whole file
    std::vector<std::string> after, before;   // the other files' keys, nearest first
    bool valid = false;
};
ActedPlace actedPlace(const Page& p, const std::vector<FileDiff>& files, size_t row, Op op);
size_t rowAfterAction(const Page& p, const std::vector<FileDiff>& files, const ActedPlace& a);

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

// A worktree against main (Compare with Main): `git diff --raw` against the
// merge base, whose new side is the worktree's files on disk, and its
// untracked files (GitUi::untrackedArgs). The main checkout is the first
// entry of `git worktree list --porcelain`.
std::vector<std::string> worktreeRawArgs(const std::string& base);
std::vector<std::string> worktreeListArgs();
std::string mainWorktreePath(const std::string& porcelain);
// `ls-files -z` output as paths.
std::vector<std::string> nulPaths(const std::string& out);

// Whether bytes read from git or the disk hold a NUL in their first 8 KB
// (and are not UTF-16 with its byte order mark): a binary file.
bool looksBinary(const std::string& bytes);

}  // namespace ChangeReview
