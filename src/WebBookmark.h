// WebBookmark.h — web bookmarks kept as files, the browser's side of
// mutible: a page saved from the browser pane is a `.url` file (the
// Internet Shortcut every browser and file manager knows), and opening one,
// from the tree, Finder or `mutible https://...`, loads its address in a
// browser tab. Safari's and Chrome's `.webloc` files (a property list with a
// URL key) open too. The folder of them (~/Bookmarks unless
// `browser.bookmarks` says otherwise) is the bookmark manager: folders,
// renames, the Trash and git all work as for any file.
//
// Bookmarks are kept per workspace (2026-11-18): a page saved in a window
// on a folder goes in that folder's `.mutible/bookmarks/`, so they travel
// with the repository and can be committed, and the window's Bookmarks
// menu lists them first. The bookmark folder of before (~/Bookmarks) is
// the shared one: every workspace lists it under "Shared", with Move to
// This Workspace, and the browser window (the window on it) works on it
// alone, as before (`places`, `mergedEntries`, `moveDestination`).
//
// Plain C++ with no I/O, so every port can use it and it is unit-tested
// (`bookmarks:*`).
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace WebBookmark {

// Whether `path` names a bookmark file by its extension: .url or .webloc,
// in any case.
bool isBookmarkPath(const std::string &path);

// Whether `url` is an address a bookmark or `mutible <address>` may open:
// http:// or https:// (the scheme in any case), a host after it, no spaces
// or control characters, at most 8,192 bytes. Nothing else (file:,
// javascript:, a custom scheme) is ever opened from a file.
bool isWebAddress(const std::string &url);

// The address a bookmark file holds, or "" when it holds none that
// isWebAddress accepts. `path` decides the format by its extension:
//  - .url: the [InternetShortcut] section's URL key (sections and keys
//    in any case, CRLF or LF, a UTF-8 byte order mark, spaces around '=').
//  - .webloc: an XML property list's string after <key>URL</key>, its XML
//    entities decoded. A binary property list ("bplist") gives ""; the Mac
//    reads those with Foundation and checks the result with isWebAddress.
std::string addressIn(const std::string &path, const std::string &bytes);

// The bytes of a .url file for `url` (CRLF line ends, as Windows writes
// them; every reader takes them).
std::string shortcutText(const std::string &url);

// A file name for a bookmark of the page titled `title` at `url`, ending in
// ".url": the title as the browser's tab shows it (white space folded), else
// the address without its scheme; '/' and ':' become '-', control
// characters go, leading dots and trailing dots and spaces are dropped, and
// it is cut at a character to at most `maxBytes` bytes before the
// extension. "Bookmark.url" when nothing is left.
std::string fileName(const std::string &title, const std::string &url, size_t maxBytes = 120);

// `name` as it is when `taken(name)` is false, else with " 2", " 3", ...
// before its extension, the first one not taken.
std::string uniqueName(const std::string &name,
                       const std::function<bool(const std::string &)> &taken);

// The name of a folder for "Save All Tabs as Bookmarks", from a local time
// already formatted as "2026-11-12 14.03": "Tabs 2026-11-12 14.03".
std::string tabsFolderName(const std::string &when);

// The addresses among `urls` worth a tab each, in order: those isWebAddress
// accepts, each once.
std::vector<std::string> tabAddresses(const std::vector<std::string> &urls);

// Whether two addresses name the same page, for "is this page bookmarked?":
// equal once the scheme and host are lower case and an empty path is "/"
// (WebKit reports https://example.com as https://example.com/, a bookmark
// written elsewhere may not). Nothing else is normalized.
bool sameAddress(const std::string &a, const std::string &b);

// A bookmark's name in a menu: its file name without the .url or .webloc
// extension (the name itself when nothing else is left).
std::string menuTitle(const std::string &fileName);

// ------------------------------------------------ per-workspace bookmarks
// The folder inside a workspace that keeps its bookmarks.
extern const char *const kWorkspaceFolder;   // ".mutible/bookmarks"

// `root`'s bookmark folder, `root`/.mutible/bookmarks ("" for no root). A
// trailing slash on `root` is dropped.
std::string workspaceFolder(const std::string &root);

// The folders of a window on `root` given the shared folder `shared`
// (browser.bookmarks, ~/Bookmarks), both spelled alike by the caller
// (symlinks resolved, or not, in both):
//  - save: where Save Page as Bookmark writes, made on the first save:
//    the workspace's folder (workspaceFolder(root)); the shared folder
//    itself in the window on it (the browser window) or with no root.
//  - shared: the folder listed under "Shared", "" when the window's own
//    folder is the shared one (or there is none).
struct Places {
    std::string save;
    std::string shared;
};
Places places(const std::string &root, const std::string &shared);

// The folders "is this page bookmarked?" looks in, in order: the
// workspace's, then the shared one (each once, none empty).
std::vector<std::string> lookupOrder(const Places &p);

// A folder's entry, as the platform lists it (in its own name order).
struct Name {
    std::string name;
    bool folder = false;
};

// A row of a bookmarks menu: a folder (a submenu) or a bookmark file.
struct Entry {
    std::string path;    // dir + "/" + name
    std::string title;   // a folder's name; a bookmark's menuTitle
    bool folder = false;
    bool shared = false; // from the shared folder (the "Shared" section)
};

// The rows of the folder `dir` listed as `names`: hidden names dropped,
// folders first, then bookmark files (isBookmarkPath; other files
// dropped), each group in the order given, at most `max` in all.
std::vector<Entry> entries(const std::string &dir, const std::vector<Name> &names, bool shared,
                           size_t max = 1000);

// The workspace's bookmarks menu: the rows of p.save listed as `own`, then
// those of p.shared listed as `shared` (marked shared; none when p.shared
// is ""), at most `max` in each part.
std::vector<Entry> mergedEntries(const Places &p, const std::vector<Name> &own,
                                 const std::vector<Name> &shared, size_t max = 1000);

// Where Move to This Workspace puts `file`, a bookmark in p.shared or a
// folder below it: p.save + "/" + its name, a number added when
// `taken(name)` (uniqueName). "" when `file` is no bookmark file below
// p.shared, or p.shared is "" (the browser window has nowhere to move to).
std::string moveDestination(const Places &p, const std::string &file,
                            const std::function<bool(const std::string &)> &taken);

}  // namespace WebBookmark
