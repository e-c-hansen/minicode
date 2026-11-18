// RecentList.h — the folders windows were opened on, newest first, for
// File > Open Recent and the dock icon's menu (DockMenu.h). The Mac's is
// src/RecentFolders.mm in its defaults; this is the port's, a JSON list in
// $XDG_STATE_HOME/mutible/recent-folders.json (PARITY.md D11), written
// atomically. Every window of the process, and every process, shares it.
//
// The rules are the Mac's: 20 folders at most, the newest first, one entry
// a folder by its resolved path, folders that are gone left out when
// listed (and still in the file, so a folder on an unmounted drive comes
// back), and nothing noted under MUTIBLE_BENCH or MUTIBLE_DEMO.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace RecentList {

constexpr size_t kMax = 20;

// Pure: `list` with `resolved` in front and any other entry for it out,
// cut to kMax.
std::vector<std::string> noted(const std::vector<std::string>& list, const std::string& resolved);
// The file's text and back; decode ignores anything that is not an array
// of non-empty strings.
std::string encode(const std::vector<std::string>& list);
std::vector<std::string> decode(const std::string& text);
// `path` with the home folder written ~ (a whole path component only).
std::string tilded(const std::string& path, const std::string& home);

// False under MUTIBLE_BENCH or MUTIBLE_DEMO (set to anything but "" or 0).
bool enabled();
// $XDG_STATE_HOME (else ~/.local/state), read each time, + /mutible/recent-folders.json.
std::string file();

// A window's folder, now the most recent.
void note(const std::string& folder);
// The folders, newest first, that still exist as folders.
std::vector<std::string> folders();
void clear();

// Told after the list changed (not under enabled() == false).
int listen(std::function<void()> fn);
void unlisten(int id);

}  // namespace RecentList
