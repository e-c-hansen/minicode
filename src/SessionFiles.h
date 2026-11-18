// SessionFiles.h — the session host's reads of the workspace
// (SESSION-DESIGN.md, stage 7, "workspace I/O in the host"). A renderer asks
// with FsRead {req, window, json: the request} and the host answers FsData
// {req, json: the answer} (SessionProtocol.h). The first read is a folder's
// listing, one level at a time, for the browser's tree (SessionWeb.h); stage
// 7 adds reads by giving the request another "op".
//
// Every read is confined to the session's folder: the headless host's own
// (Host::setFolder), else the folder of the window the request names (its
// "root", as the window's renderer reported it). A path is '/'-separated
// names from that folder, "" for the folder itself; an absolute path, an
// empty, "." or ".." name, and a path whose real place (symlinks followed)
// is outside the folder's real place are refused.
//
// A listing follows the apps' trees (the Mac's MCTreeList, GTK's FileTree):
// dotfiles only when asked for (Show Hidden Files), folders first, then
// names as TreeSort orders them. What the folder's .gitignore and .ignore
// files ignore (FolderSearch.h's Ignore, as Find in Folder reads them) is
// listed and marked, as the apps' trees show it in tree.git-ignored's
// color rather than leaving it out. Pure C++17 and POSIX.
#pragma once

#include <cstddef>
#include <string>

#include "Json.h"

namespace Shared {

// The most entries one listing names; the rest are counted ("more").
constexpr std::size_t kMaxListed = 10000;

// `rel` held inside `root`: true with *abs = root + "/" + rel (spelled
// under `root`, as the windows spell their tabs' paths; `root` itself for
// ""), false with *why for a path that is not one of the folder's own.
bool confinedPath(const std::string& root, const std::string& rel, std::string* abs,
                  std::string* why);

// One read (FsRead's json) answered (FsData's json), in `root`:
//   {"op": "list", "path": rel, "hidden": bool}
//     -> {"op": "list", "path": rel, "root": root, "entries": [{"name",
//        "dir", "link", "ignored", "outside", "kind"}, ...], "more": n}
// where "dir", "link" (a symlink), "ignored" (by the ignore files) and
// "outside" (a symlink leading out of the folder: never listed or opened)
// are there only when true, "kind" is a file's FileIcon kind ("text",
// "code", "image", ...), and "more" counts entries past kMaxListed. A read
// that fails answers {"op", "path", "error": why}; an unknown op too.
Json readWorkspace(const std::string& root, const Json& request);

}  // namespace Shared
