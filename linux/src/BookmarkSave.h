// BookmarkSave.h — saving web pages as bookmarks (PARITY.md 4.8), the file
// half of the Mac's Bookmarks.mm: where the folder is, a .url written once
// and never over a file, a page's bookmark found again by address. No GTK;
// the names and the file text are the core's WebBookmark.
#pragma once

#include <string>
#include <vector>

namespace BookmarkSave {

// `browser.bookmarks` as the settings file wrote it ("" for the default),
// as a path: "~" expanded, a relative one from the home folder, else
// ~/Bookmarks.
std::string folderFor(const std::string& setting);

// Makes `folder` when missing; false with `error` when it is a file or
// cannot be made.
bool ensureFolder(const std::string& folder, std::string* error);

// Writes a bookmark of `url` titled `title` in `folder` under its title's
// name (a number added when taken), never over a file. Returns the path, or
// "" with `error`.
std::string write(const std::string& folder, const std::string& title, const std::string& url,
                  std::string* error);

// The bookmark file anywhere under `folder` (hidden entries skipped, 10,000
// at most) holding the same page as `url`, or "".
std::string fileFor(const std::string& folder, const std::string& url);

// `path` with the home folder written "~".
std::string shortPath(const std::string& path);

// A folder "Tabs <when>" (a number added when taken) in `parent`, with a
// bookmark in it for each of `urls` that is a web address, once each, named
// from the matching title. Returns its path, or "" with `error`; `saved`
// counts the files written.
std::string writeTabs(const std::string& parent, const std::string& when,
                      const std::vector<std::string>& titles, const std::vector<std::string>& urls,
                      size_t* saved, std::string* error);

}  // namespace BookmarkSave
