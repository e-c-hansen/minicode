// GitModel.h — pure C++, part of the portable core. What every port's
// Source Control panel shows, worked out from what git printed: the rows of
// the two change lists, the line about what is pushed, the argument vectors
// for every command it runs, where the selection goes after a refresh, the
// tooltips, the commit graph, and the diff and commit text with a style per
// run. The panels run git and draw: the Mac's src/GitPanel.mm, the GTK
// port's linux/src/GitPanel.cpp and Android's git_jni.cpp. Tested in
// tests/gitmodel_tests.inc (`git-ui:*`). See "Git panel" in CLAUDE.md.
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
    bool submodule = false;   // a gitlink: its change is a commit, or work inside it
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
    // `git remote`, for pushing, pulling and fetching (remoteArgs).
    std::vector<std::string> remotes;
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

// Several rows chosen before a refresh (`chosen`, indexes into oldRows),
// after it: the same files in the same lists while at least two of them are
// still there, else the same files in either list (Stage on them moved them
// all to the other list), else none, and the caller keeps one row as
// pickAfterRefresh says. The Mac's applySnapshot does the same in place.
std::vector<int> keepChosen(const std::vector<Row>& oldRows, const std::vector<int>& chosen,
                            const std::vector<Row>& rows);

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

// ----------------------------------------- Stage All, Unstage All, Discard
// A command that may want a question first: VS Code's git.stageAll,
// git.unstageAll and git.clean. `args` empty and `refusal` set when there is
// nothing to do; `question` empty when it runs without asking.
struct ActionPlan {
    std::vector<std::string> args;
    std::string refusal;
    std::string question, detail, button;   // the alert: message, text, first button
    std::string doneText;                   // under the Commit button afterwards
};

// Stage All: `add -A -- .` from the top level (new, changed and deleted
// files, as VS Code's with git.untrackedChanges mixed). Asks first only when
// some files still have conflicts, since staging one marks it resolved.
ActionPlan stageAllPlan(const Snapshot& s);
// Unstage All: `restore --staged -- .`, or before the first commit, which
// has no HEAD to restore from, `rm --cached -r --force -- .` (both touch
// only the index). Never asks.
ActionPlan unstageAllPlan(const Snapshot& s);

// Discard Changes on a file of the Changes list (git.clean). Always asks,
// naming the file. Undoable: the file as it is goes to the Trash first (an
// untracked file only goes there; a deleted one has nothing to keep), then
// a tracked one is restored from the index with `restore --worktree`,
// which is what that list's diff is against (its staged version, else the
// last commit's). Undo Discard puts the Trash's copy back (Put Back in the
// Trash does too). Refused for a staged row (unstage it first, as in VS
// Code, which offers no discard there), a conflict, a file added with
// `git add -N`, which restore would empty, and a submodule, whose folder is
// another repository's checkout (trashing it and restoring the gitlink left
// an empty folder).
struct DiscardPlan : ActionPlan {
    bool trashFirst = false;   // move the file to the Trash before `args`
};
DiscardPlan discardPlan(const Row& r);
// "Undo Discard of a.txt", for the menus; what is said once it is undone.
std::string undoDiscardTitle(const std::string& path);
std::string undoneDiscardText(const std::string& path);

// ------------------------------------------- several rows chosen at once
// The change lists take several rows (Shift and Command clicks). Stage acts
// on the chosen rows of Changes and Unstage on those of Staged, one command
// for all of them (the files after "--"); empty when none of the rows is
// in that list. Discard asks once, naming the count, and is refused whole
// when any row would be refused alone, naming that file.
std::vector<std::string> stageRowsArgs(const std::vector<Row>& rows, bool initial, bool stage);
struct DiscardBatch : ActionPlan {
    std::vector<DiscardPlan> plans;   // one per row, in order
    std::vector<std::string> paths;   // their paths
};
DiscardBatch discardBatchPlan(const std::vector<Row>& rows);

// The Changes row for a path relative to the top level, or -1: Discard
// Changes from the menu bar acts on the file on show.
int changesRowFor(const std::vector<Row>& rows, const std::string& path);

// ------------------------------------------------- hunks and lines of a diff
// The diff a row shows (diffArgs) offers actions per hunk, as VS Code's
// git.stageSelectedRanges, git.unstageSelectedRanges and
// git.revertSelectedRanges do: a Changes diff (index to work tree) Stage
// and Discard, a staged diff (HEAD to index) Unstage, an untracked file's
// none (it is staged whole). Each is a patch made from the diff text and
// given to `git apply` on stdin: Stage applies it to the index, Unstage
// applies it reversed to the index, Discard reversed to the work tree. A
// patch holds one hunk, or the changed lines a selection covers: the hunk
// is rewritten so that only they change (VS Code's way), and its counts and
// starts are made right again, so git applies it exactly where it was.
enum class DiffSide { Changes, Staged, Untracked };
enum class HunkOp { Stage, Unstage, Discard };
DiffSide diffSideOf(const Row& r);
std::vector<HunkOp> hunkOps(DiffSide side);
// "Stage Hunk", "Discard Selected Lines", ...
std::string hunkOpTitle(HunkOp op, bool lines);

// `apply --cached -`, `apply --cached --reverse -`, `apply --reverse -`;
// --whitespace=nowarn, since apply.whitespace=fix would change the lines
// while staging them.
std::vector<std::string> applyArgs(HunkOp op);

// Where a hunk is in the diff's lines (line i is the i-th line of the text
// diffText shows, which is the i-th line git printed): the header first.
struct DiffHunk {
    size_t file = 0, hunk = 0;    // the file in the diff, the hunk in the file
    size_t firstLine = 0;         // the "@@" line
    size_t lineCount = 0;         // with the header
    // Each line's kind: '@' the header, ' ' context, '+', '-', and '\' for
    // the "No newline at end of file" line.
    std::string kinds;
    bool changes(size_t line) const;   // `line` is one of its + or - lines
};
std::vector<DiffHunk> diffHunks(const std::string& diff);
// The hunk holding `line` (its header or any of its lines), or -1.
int hunkAt(const std::vector<DiffHunk>& hunks, size_t line);

// The patch that does `op` to the changed lines among lines first..last of
// the diff (all of one hunk when given its range). `refusal` says why there
// is none: no changed line chosen, a new, deleted or binary file (staged or
// unstaged whole), a conflict's combined diff, or lines at an end of file
// with no newline that cannot change apart from the rest.
std::string buildPatch(const std::string& diff, HunkOp op, size_t first, size_t last,
                       std::string* refusal);

struct HunkPlan : ActionPlan {
    std::string patch;   // for apply's stdin
    std::string path;    // the file, relative to the top level
};
// One hunk, the one holding `line`; or with `lines`, the changed lines
// among first..last. Discard asks first, naming the file; the file as it
// is goes to the Trash before (DiscardPlan's trashFirst, a copy here), so
// Undo Discard puts it back.
HunkPlan hunkPlan(const std::string& diff, DiffSide side, HunkOp op, size_t first, size_t last,
                  bool lines);
// What a failed apply means: the file changed since its diff was shown.
std::string hunkFailureText(int status, const std::string& out, const std::string& err,
                            const std::string& path);

// The diff's right click (or the keyboard's menu key): for each of the
// side's actions, the hunk holding `clicked` (a line of the diff), then the
// changed lines among first..last when `selection` covers some, as the
// Mac's diff menu orders them ("Stage Hunk", "Stage Selected Lines",
// "Discard Hunk…", "Discard Selected Lines…"). `first` and `last` are what
// hunkPlan is given. Empty when the side has no actions, or the click is in
// no hunk and no changed line is chosen.
struct HunkMenuItem {
    HunkOp op = HunkOp::Stage;
    bool lines = false;
    size_t first = 0, last = 0;
    std::string title;
};
std::vector<HunkMenuItem> hunkMenu(const std::string& diff, DiffSide side, size_t clicked,
                                   size_t first, size_t last, bool selection);


// ------------------------------------- amend, undo the last commit, stash
// VS Code's git.commitAmend (Commit and Amend), git.undoCommit, git.stash
// (with untracked files) and git.stashPop, in the panel's menu and the
// palette, with no keys. Each is an ActionPlan: `args` empty with a
// refusal, or the command with a question first when `question` is set.
//
// Whether HEAD's commit is already on its upstream: an upstream that
// exists, and nothing ahead of it. Amending or undoing such a commit means
// the next push needs a force, so both ask first.
bool headIsPushed(const Snapshot& s);
// Amend takes the message box's text as the new message, or keeps the old
// one (--no-edit) when it is empty; what is staged joins the commit.
ActionPlan amendPlan(const Snapshot& s, const std::string& message);
// Undo Last Commit: `reset --soft HEAD~1`, so the commit's changes stay
// staged, and the panel puts its message back in the box (VS Code does).
// `parents` is HEAD's parent count (parentCount): 0 for a repository's
// first commit, undone with `update-ref -d HEAD` (its files stay staged);
// 2 or more for a merge, which asks first, saying where HEAD goes and that
// the merged branch keeps its commits. Refused before the first commit and
// on a detached HEAD.
ActionPlan undoCommitPlan(const Snapshot& s, int parents);
std::vector<std::string> headParentsArgs();          // rev-list --parents -n 1 HEAD --
int parentCount(const std::string& revListOut);      // its output; -1 if unreadable
std::vector<std::string> headMessageArgs();          // log -1 --format=%B HEAD --
// Stash: `stash push --include-untracked`, with `-m` and the message box's
// text when there is some. Refused with nothing to stash and before the
// first commit (git cannot stash then).
ActionPlan stashPlan(const Snapshot& s, const std::string& message);
// Pop Stash: `stash pop` of the newest. `stashes` is how many there are
// (stashListArgs prints one a line); refused with none. A pop that
// conflicts keeps the stash and leaves the conflicts as rows.
ActionPlan popStashPlan(const Snapshot& s, int stashes);
std::vector<std::string> stashListArgs();   // stash list --format=%gd
size_t countLines(const std::string& out);   // non-blank lines

// --------------------------------------------- the editor's change bars
// The gutter's original (QuickDiff.h) is the file's index entry, read as
// `ls-files --stage` then `cat-file blob` of its object, so the text and
// the object it came from are known together; Stage Change writes the new
// text as an object (`hash-object -w --stdin --path`, so the file's clean
// filters apply as on add) and points the entry at it (`update-index
// --cacheinfo`), as VS Code's git extension stages selected ranges, after
// checking that the entry still names the object the bars were made from.
std::vector<std::string> indexEntryArgs(const std::string& rel);
struct IndexEntry {
    bool ok = false;      // one entry, stage 0, a regular file (not a link, a
                          // submodule or a conflict)
    std::string mode;     // "100644", "100755"
    std::string object;   // its blob
};
IndexEntry parseIndexEntry(const std::string& lsFilesOut);
std::vector<std::string> blobArgs(const std::string& object);
std::vector<std::string> hashObjectArgs(const std::string& rel);
std::vector<std::string> updateIndexArgs(const IndexEntry& e, const std::string& object,
                                         const std::string& rel);

// ------------------------------------------------------ the branch picker
// VS Code's git.checkout: the local branches, most recently committed to
// first, then the remote branches that have no local branch of their name;
// choosing one switches to it (a remote one by making a local branch that
// tracks it), and a name typed that is no branch makes one there.
std::vector<std::string> branchListArgs();   // for-each-ref, newest first

struct BranchItem {
    std::string name;     // "main", "origin/feature"
    std::string detail;   // "current · a1b2c3d", "remote · a1b2c3d"
    bool remote = false;
    bool current = false;
};
std::vector<BranchItem> branchItems(const std::string& refsOut, const Snapshot& s);

// `switch <branch>`, or for a remote one `switch -c <name> --track
// <remote>/<name>`; empty for the current branch (nothing to do).
std::vector<std::string> switchArgs(const BranchItem& b, const Snapshot& s);
// The local name a remote branch gets: "feature" for "origin/feature".
std::string localNameFor(const std::string& remoteBranch, const Snapshot& s);

// A typed name as a new branch's: trimmed, spaces made dashes (VS Code's
// git.branchWhitespaceChar).
std::string newBranchName(const std::string& typed);
// Why git would refuse `name` as a branch (check-ref-format's rules), or "".
std::string branchNameProblem(const std::string& name);
// The picker's last row for what is typed: "Create branch “x” from main";
// "" when nothing is typed or a local branch has that name.
std::string createBranchLabel(const std::string& typed, const std::vector<BranchItem>& items,
                              const Snapshot& s);
std::vector<std::string> createBranchArgs(const std::string& name);   // switch -c
// "Switched to feature.", "Created feature and switched to it."
std::string switchedText(const std::string& branch, bool created);

// ------------------------------------------------ pushing and pulling
// The panel's Fetch, Pull, Push and Sync (pull, then push), as VS Code's
// git.fetch, git.pull, git.push and git.sync do them, without asking first
// (the owner's git.confirmSync is off). Plain `git pull` and `git push`,
// so the user's pull.rebase, push.default and remotes' push URLs apply as
// they do in a terminal. A branch with no upstream is published instead:
// `push --set-upstream` to origin, or to the only remote. Nothing is ever
// forced. The commands run with GIT_TERMINAL_PROMPT=0 and no terminal
// (Git::talksToRemote), so a missing credential fails, and is shown
// (networkFailureText), rather than waiting for a password.
enum class SyncOp { Fetch, Pull, Push, Sync };

std::vector<std::string> remoteArgs();   // `git remote`
// Its output, one name a line; names that would read as options are left out.
std::vector<std::string> parseRemotes(const std::string& out);

struct SyncPlan {
    // The commands, in order; the first that fails stops the rest.
    std::vector<std::vector<std::string>> steps;
    std::string refusal;     // why nothing can run, when steps is empty
    std::string busyText;    // "Pulling from origin/main…", while it runs
    std::string doneText;    // "Pulled from origin/main.", when all went well
    bool fetches = false;    // it fetches (Fetch, Pull, Sync): autofetch may wait
};
SyncPlan syncPlan(SyncOp op, const Snapshot& s);

// The remote a push without an upstream goes to: origin when there is one,
// else the only remote; "" when there is none or no way to choose.
std::string publishRemote(const Snapshot& s);
// The remote the upstream is on ("origin" for origin/main, the longest
// remote name that fits); "" without an upstream or on a detached HEAD.
std::string upstreamRemote(const Snapshot& s);

// The button under Commit: "Sync Changes ↓ 1 ↑ 2" when the branch is behind
// or ahead of its upstream, "Publish Branch" when it has none and there is
// a remote to publish to, "" when there is nothing to do (no upstream and
// no remote, a detached HEAD, no commit yet, up to date).
std::string syncButtonTitle(const Snapshot& s);
std::string syncButtonToolTip(const Snapshot& s);

// A failed push, pull or fetch, as failureText says it, with a line on
// what to do when git wanted a password, a token or an ssh key it could
// not ask for: the Mac's keychain, or with `forLinux` what a Linux desktop
// has (a libsecret credential helper, plain ssh-add).
std::string networkFailureText(int status, const std::string& out, const std::string& err,
                               bool forLinux = false);

// ------------------------------------------------------------ autofetch
// git.autofetch, as VS Code's: the folder's repository is fetched in the
// background while its window has the keyboard, never more often than every
// git.autofetch-period seconds (180, VS Code's default, at least), and not
// at all without a remote. A failure (offline, a credential) doubles the
// wait each time, up to an hour, so a repository that cannot be fetched is
// not asked every three minutes; a fetch that works resets it.
constexpr int kAutoFetchMinPeriod = 180;
constexpr int kAutoFetchMaxBackoff = 3600;
struct AutoFetchState {
    double lastStart = -1;   // when the last fetch began (any fetch), seconds; -1 never
    int failures = 0;        // autofetches that failed in a row
};
// Seconds to wait after the last fetch before the next.
double autoFetchWait(int period, int failures);
// Whether to fetch now: autofetch is on, the window has the keyboard, the
// folder is a repository with a remote, nothing runs, and the wait is over.
bool autoFetchDue(bool enabled, bool windowActive, const Snapshot& s, bool busy,
                  const AutoFetchState& st, double now, int period);

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

// How a port writes a commit's date: dateText below by default; the Mac
// passes its own, in the user's locale.
using DateFormat = std::string (*)(long long t);
std::string dateText(long long t);

// "a1b2c3d  Ann, 3 days ago (Sep 23, 2026 at 14:02)\nSubject", then "Not
// pushed yet..." or "Not pulled yet..." and the labels; the "Show more" row
// gets "Load the next 200 commits".
std::string graphToolTip(const Graph& g, size_t row, long long now,
                         DateFormat date = dateText);

// For tests and the debug log: "a1b2c3d lane=0 head out Subject [*main,v1]".
std::vector<std::string> graphDescriptions(const Graph& g);

// dateText: "Sep 23, 2026 at 14:02", in local time (declared above).

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

// diffText with the hunk actions on each hunk's header line, after the
// "@@ ... @@": "Stage Hunk · Discard Hunk". `actions` gets where each
// action's words are (in characters, as StyledText's runs), which hunk it
// is for (by its header line) and what it does; a port makes them links.
struct DiffAction {
    size_t start = 0, length = 0;
    HunkOp op = HunkOp::Stage;
    size_t line = 0;
};
StyledText diffTextWithActions(const std::string& bytes, DiffSide side,
                               std::vector<DiffAction>& actions);

// `git show` output (showArgs): hash, parents, author, a different
// committer, date with relative time, the message with its subject bold,
// then the stat and diff. Falls back to diffText if it does not parse.
StyledText commitText(const std::string& show, long long now, DateFormat date = dateText);

// "a1b2c3d Subject": the title of a commit view.
std::string commitTitle(const Git::Commit& c);

}  // namespace GitUi
