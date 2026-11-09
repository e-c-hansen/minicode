// BrowserTabs.h — the rules behind the browser pane's tabs (the Mac's
// BrowserView keeps the tabs themselves): which tabs keep a live page,
// what each tab is called, and which tabs' records a session keeps. Where
// a new tab goes, which one a key selects, which one shows after a close
// and where a dragged tab lands are the editor's rules (EditorTabs.h).
// Plain C++ with no I/O, so it is unit-tested.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace BrowserTabs {

// How many tabs keep a live page (a web view and the WebKit process behind
// it): the one on show and the one used just before it, so switching back
// and forth between two costs nothing. The others keep only their address
// and WebKit's record of their history and scroll, and load again when
// shown.
constexpr int kLivePages = 2;

// Which tabs keep a live page: `active` (when it is a tab), then the others
// used most recently (the largest `lastUsed`, a stamp per tab; ties go to
// the tab further left), up to `maxLive` in all. One flag per tab.
std::vector<bool> livePages(const std::vector<uint64_t> &lastUsed, int active, int maxLive);

// What a tab is called in the strip: the page's title (spaces at its ends
// and runs of white space inside folded to one space); else its address
// without the scheme, "www." or a trailing slash (a file's name for a file
// address); else "New Tab". At most `maxChars` characters (UTF-8 aware),
// ending in "…" when cut.
std::string tabTitle(const std::string &title, const std::string &url, size_t maxChars = 40);

// Which tabs' records (WebKit's history and scroll, `sizes` bytes each) a
// session keeps: none over `perTab`, and no more than `total` in all, the
// active tab's first, then the others from the most recently used. A tab
// whose record is left out still comes back at its address.
std::vector<bool> keptRecords(const std::vector<size_t> &sizes,
                              const std::vector<uint64_t> &lastUsed, int active, size_t perTab,
                              size_t total);

// The tab to show of `count` restored ones: the saved index when it is one
// of them, else the last (or -1 when there are none).
int restoredActive(int saved, int count);

}  // namespace BrowserTabs
