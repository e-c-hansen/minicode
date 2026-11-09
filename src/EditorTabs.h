// EditorTabs.h — the rules behind a window's row of open files: where a new
// tab goes, which tab a key selects, which one shows after a close, what of
// a remembered session can be reopened, and what each tab is called. Plain
// C++ with no I/O, so it is unit-tested; the Mac's EditorController keeps
// the tabs themselves.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace EditorTabs {

// Where a newly opened tab goes: just right of the active one, or at the
// end when none is active.
int insertionIndex(int active, int count);

// Tab n (1-based) for Shift+Cmd+n or Ctrl+n. 9 is always the last tab, as
// in browsers. -1 when there is no such tab.
int indexForNumber(int n, int count);

// One tab to the right (delta 1) or the left (delta -1) of `active`,
// wrapping around at either end. -1 when there are no tabs.
int step(int active, int count, int delta);

// After tab `closed` is closed, the index, in the list without it, of the
// tab to show: the active one still, when another tab was closed, else the
// one used most recently of those left (the largest `lastUsed`, which holds
// a stamp per tab before the close), or -1 when none is left.
int activeAfterClose(const std::vector<uint64_t> &lastUsed, int active, int closed);

// The tabs a window had open in a folder, as remembered between runs, and
// which of them was the preview tab (-1 for none).
struct Session {
    std::vector<std::string> files;
    int active = -1;
    int preview = -1;
};

// What of a remembered session can be opened again: files that no longer
// exist (`exists` says) and repeats are dropped. The active tab stays with
// its file, or moves to the nearest file before it that is left (the first,
// failing that), and is -1 only when nothing is left. The preview tab stays
// with its file and is -1 when its file was dropped (a repeat of a file
// already listed is that tab, kept: a file open twice was never a preview).
Session restorable(const Session &saved,
                   const std::function<bool(const std::string &)> &exists);

// What each tab shows: a file's name, and for files that share a name,
// enough of their folders to tell them apart ("src/main.cpp" beside
// "tests/main.cpp"). An empty path (a tab that holds no file) gets "".
std::vector<std::string> labels(const std::vector<std::string> &paths);

// Dragging tab `from` along a row of tabs `widths` wide, laid end to end
// from 0: where it goes when let go with the pointer at `x` (the same
// coordinates), as an index in the row after the move. Each other tab whose
// middle is left of the pointer comes before it. `from` itself when out of
// range.
int dropIndex(const std::vector<double> &widths, int from, double x);

// ---- The preview tab (VS Code's workbench.editor.enablePreview)
// A file opened by a single click in the file tree goes in the window's
// preview tab, its title in italics, which the next such open replaces, so
// browsing does not pile up tabs. An edit, a save, a double-click on its
// title or on the file in the tree, or Keep Open makes it an ordinary
// ("kept") tab. The agents' preview tabs (`minicode open --note`, a diff)
// follow the same rules but are their own: an agent's open never replaces
// the user's preview tab, nor a click in the tree the agent's.

// Where an open comes from, which decides whether it goes in the preview tab.
enum class OpenFrom {
    Other,             // menus, links, Problems, the agents' kept opens: kept
    TreeClick,         // a single click on a file in the tree
    TreeDoubleClick,   // the second click of a double-click on the same row
    TreeCommandClick,  // Cmd+click on a file in the tree: kept
    TreeReturn,        // Return in the tree, which also moves the keyboard: kept
    QuickOpen,         // Cmd+P (workbench.editor.enablePreviewFromQuickOpen)
    CodeNavigation,    // Go to Definition's or a reference's file
    SearchClick,       // a single click on a Find in Folder or TODO result
    SearchOpen,        // a double-click or Return on one: kept
};

struct PreviewSettings {
    bool enabled = true;          // workbench.enable-preview
    bool fromQuickOpen = false;   // workbench.enable-preview-from-quick-open
};

// Whether an open from `from` goes in the preview tab.
bool opensInPreview(OpenFrom from, const PreviewSettings &s);

// What a click on a file in the tree is. The window server counts clicks
// by time alone, so a second click within half a second is a double-click
// wherever it lands: it counts as one only when the press before it was on
// the same row. Command makes any click a kept open.
OpenFrom treeClick(int clickCount, bool sameRowAsLastPress, bool command);

// Whether a tab that is a preview stays one when its file is opened again:
// a kept open keeps it, an open in preview leaves it as it is.
bool staysPreview(bool isPreview, bool openInPreview);

// Where a new preview tab goes, given the active tab, the preview tab (-1
// for none) and the number of tabs: VS Code's place, just right of the
// active tab, with the old preview tab closed. `index` counts in the list
// after `replace` is taken out (-1: nothing replaced).
struct PreviewPlace {
    int replace = -1;
    int index = 0;
};
PreviewPlace placePreview(int active, int preview, int count);

// ---- Editor groups (VS Code's Split Editor), two at most, side by side
// Each group has its own row of tabs; one file open in both is one
// document shown twice. Group 0 is the left one.

// Where "Move Editor into Next Group" (direction 1) or "into Previous
// Group" (-1) sends the tab on show from group `focused`: the other group,
// a new group to the right when there is only one (as VS Code makes one),
// or -1 when there is none that way.
int moveTarget(int focused, bool split, int direction);

// The left group's width out of `total`, for its share `fraction`: neither
// group narrower than `minWidth` while there is room for both, else halves.
double leftGroupWidth(double total, double fraction, double minWidth);

// A remembered split: each group's files in tab order, each group's file on
// show, which group had the keyboard, and the left group's share of the
// width.
struct GroupsSession {
    std::vector<std::string> groups[2];
    std::string active[2];
    int focused = 0;
    double fraction = 0.5;
};

// The split laid out again over the files that were reopened (`open`, in
// tab order): `groups` hold indices into `open`, each group its own files
// that are open, in its order, once each; a reopened file neither group
// named (a backup's, another window's) joins the focused group. `active`
// is an index into its group (its file, else the first). With a group left
// empty there is no split (`split` false, everything else unset). The share
// is kept between 0.15 and 0.85.
struct GroupLayout {
    bool split = false;
    std::vector<int> groups[2];
    int active[2] = {-1, -1};
    int focused = 0;
    double fraction = 0.5;
};
GroupLayout restoreGroups(const GroupsSession &saved, const std::vector<std::string> &open);

}  // namespace EditorTabs
