// WebBookmark.h — web bookmarks kept as files, the browser's side of
// minicode: a page saved from the browser pane is a `.url` file (the
// Internet Shortcut every browser and file manager knows), and opening one,
// from the tree, Finder or `minicode https://...`, loads its address in a
// browser tab. Safari's and Chrome's `.webloc` files (a property list with a
// URL key) open too. The folder of them (~/Bookmarks unless
// `browser.bookmarks` says otherwise) is the bookmark manager: folders,
// renames, the Trash and git all work as for any file.
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

// Whether `url` is an address a bookmark or `minicode <address>` may open:
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

}  // namespace WebBookmark
