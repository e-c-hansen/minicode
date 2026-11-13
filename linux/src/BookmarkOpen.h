// BookmarkOpen.h — the files side of opening web bookmarks (PARITY.md 4.8;
// the Mac's Bookmarks.mm): where the bookmark folder is, which files in a
// folder are bookmarks and what addresses they hold. What the address means
// (WebBookmark::isWebAddress) and how a .url or .webloc is read is the
// core's WebBookmark; nothing here touches GTK.
#pragma once

#include <string>
#include <vector>

namespace BookmarkOpen {

// The bookmark folder: the `browser.bookmarks` setting as written ("" for
// the default), "~" expanded, a relative path from the home folder,
// standardized; ~/Bookmarks when unset.
std::string folder(const std::string& setting);

// The folder made (with its parents) when missing. False, with `*error`,
// when it is a file or cannot be made.
bool ensureFolder(const std::string& dir, std::string* error);

// Whether `path` and `dir` name the same folder once symlinks are resolved.
bool sameFolder(const std::string& path, const std::string& dir);

// The address the bookmark file at `path` holds, or "" when the file is not
// a bookmark, cannot be read, or holds none that may open (a binary .webloc
// too: those open as text here).
std::string addressOf(const std::string& path);

// The addresses of the bookmarks directly in `dir`, in the tree's order
// (names case folded and collated), hidden files left out, each file's
// address only when it may open.
std::vector<std::string> addressesIn(const std::string& dir);

// Whether `dir` has a bookmark file (by name) directly in it.
bool hasBookmarkIn(const std::string& dir);

}  // namespace BookmarkOpen
