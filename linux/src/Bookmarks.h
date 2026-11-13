// Bookmarks.h — web bookmarks as files (PARITY.md 4.8; the Mac's
// Bookmarks.mm): where the bookmark folder is, which files are bookmarks and
// what addresses they hold, a .url written once and never over a file, a
// page's bookmark found again by address, and the folder's listing for the
// book, the Bookmarks menu and the palette. What an address means and how
// the files read and are named is the core's WebBookmark; nothing here
// touches GTK widgets.
#pragma once

#include <gio/gio.h>

#include <string>
#include <vector>

namespace Bookmarks {

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

// One row of a folder's listing: a subfolder or a bookmark file.
struct Entry {
    std::string name;     // the file's name
    std::string path;     // its full path
    bool isFolder = false;
};

// What is directly in `dir` for a menu: subfolders first, then bookmark
// files, each group in the tree's order, hidden entries left out. At most
// `limit` entries.
std::vector<Entry> list(const std::string& dir, size_t limit = 2000);

// Every bookmark under `dir` (subfolders too, symlink loops cut), with its
// address, for the palette: at most `limit` files looked at.
struct Found {
    std::string path;
    std::string menuPath;  // "Folder/Name" relative to `dir`, no extension
    std::string address;
};
std::vector<Found> all(const std::string& dir, size_t limit = 3000);

// The folder as a menu model, for the book and the Bookmarks menu: folders
// first, each a submenu opening with "Open All in Tabs"
// (`win.openbookmarkfolder`, the folder's path as its target), then the
// bookmark files titled by WebBookmark::menuTitle (`win.openbookmark`, the
// file's path as its target). Read from the folder when called, nothing
// kept; at most `limit` entries in all and six levels. An empty folder is
// one disabled row (`win.nobookmarks`). The caller owns the result.
GMenu* menu(const std::string& dir, size_t limit = 2000);

// Writes a bookmark of `url` titled `title` in `dir` under its title's name
// (a number added when taken), never over a file. Returns the path, or ""
// with `error`.
std::string write(const std::string& dir, const std::string& title, const std::string& url,
                  std::string* error);

// The bookmark file anywhere under `dir` (hidden entries skipped, 10,000
// at most) holding the same page as `url`, or "".
std::string fileFor(const std::string& dir, const std::string& url);

// `path` with the home folder written "~".
std::string shortPath(const std::string& path);

// A folder "Tabs <when>" (a number added when taken) in `parent`, with a
// bookmark in it for each of `urls` that is a web address, once each, named
// from the matching title. Returns its path, or "" with `error`; `saved`
// counts the files written.
std::string writeTabs(const std::string& parent, const std::string& when,
                      const std::vector<std::string>& titles, const std::vector<std::string>& urls,
                      size_t* saved, std::string* error);

}  // namespace Bookmarks
