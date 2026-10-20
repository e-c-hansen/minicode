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

// The tabs a window had open in a folder, as remembered between runs.
struct Session {
    std::vector<std::string> files;
    int active = -1;
};

// What of a remembered session can be opened again: files that no longer
// exist (`exists` says) and repeats are dropped. The active tab stays with
// its file, or moves to the nearest file before it that is left (the first,
// failing that), and is -1 only when nothing is left.
Session restorable(const Session &saved,
                   const std::function<bool(const std::string &)> &exists);

// What each tab shows: a file's name, and for files that share a name,
// enough of their folders to tell them apart ("src/main.cpp" beside
// "tests/main.cpp"). An empty path (a tab that holds no file) gets "".
std::vector<std::string> labels(const std::vector<std::string> &paths);

}  // namespace EditorTabs
