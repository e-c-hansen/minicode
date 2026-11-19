// TitlePath.h — a file's path as the window's title shows it while the
// editor has the keyboard: from the window's folder for a file inside it
// ("src/main.cpp"), else with "~" for the home folder ("~/notes/todo.md"),
// else in full. Header only, no dependencies, no I/O (paths compared as
// written; symlinks are not followed). The GTK port's updateTitle uses it.
// Tested in tests/run_tests.cpp (title:path).
#pragma once

#include <string>

namespace TitlePath {

// `dir` without the '/'s it may end with ("/" stays "/").
inline std::string trimmed(std::string dir) {
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    return dir;
}

// `path` is inside `dir` (a trimmed folder), not `dir` itself.
inline bool inside(const std::string& path, const std::string& dir) {
    if (dir.empty()) return false;
    if (dir == "/") return path.size() > 1 && path[0] == '/';
    return path.size() > dir.size() + 1 && path.compare(0, dir.size(), dir) == 0 &&
           path[dir.size()] == '/';
}

// `path` from `root` when inside it, else from `home` as "~/...", else as is.
inline std::string shown(const std::string& path, const std::string& root,
                         const std::string& home) {
    const std::string r = trimmed(root), h = trimmed(home);
    if (inside(path, r)) return path.substr(r == "/" ? 1 : r.size() + 1);
    if (h != "/" && inside(path, h)) return "~" + path.substr(h.size());
    return path;
}

}  // namespace TitlePath
