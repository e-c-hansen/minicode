// WindowTitle.h — the window's title: what the window shows, then
// " — mutible". Header only, no dependencies, no I/O; the Mac's
// updateTitle (EditorController+WindowTitle.mm) and the GTK window give it
// what they know and set what it returns. Tested in tests/run_tests.cpp
// (window-title:*, tests/window_title_tests.inc).
//
// The rule (the owner's, 2026-11-18, as VS Code's ${rootName}-relative
// path):
// - The document (the editor, or a pane that is not the terminal's or the
//   browser's, such as the tree, has the keyboard): the file's path from
//   the window's folder, "prep/snippets/agentic-system-design.json —
//   mutible". A file outside the folder shows its path with ~ for home
//   ("~/notes/todo.md — mutible"), else its whole path. An untitled
//   buffer shows its name ("Untitled-1 — mutible"). A tab that is not a
//   file (a diff, a page, a comparison) shows the name the app gives it.
//   No tab: the folder's name. Unsaved changes put "● " in front; the
//   app's own detail (a picture's size, a PDF's pages) goes after.
// - The terminal has the keyboard: its title (the program's, the one the
//   user gave it, else the command running or its folder), else the
//   window's folder's name.
// - The browser has the keyboard: the page's title, else the folder's name.
// Nothing to show at all: "mutible".
#pragma once

#include <string>

namespace WindowTitle {

enum class Focus { Document, Terminal, Browser };

struct Input {
    Focus focus = Focus::Document;
    std::string path;        // the active tab's file, absolute; "" for none
    bool untitled = false;   // never saved: path's last part (or "Untitled-1") names it
    std::string name;        // a tab that is no file (a diff, a page): shown as is
    bool gone = false;       // the file was deleted: " (deleted)" after it
    bool dirty = false;      // unsaved changes: "● " in front
    std::string detail;      // after " — mutible", for the document ("  2 pages")
    std::string folder;      // the window's folder, absolute; "" for none
    std::string home;        // the user's home folder; "" for none
    std::string pane;        // Terminal: its title; Browser: the page's title
};

namespace detail {
inline std::string trimSlash(std::string s) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}
inline std::string lastPart(const std::string &p) {
    const std::string s = trimSlash(p);
    const size_t slash = s.rfind('/');
    return slash == std::string::npos ? s : s.substr(slash + 1);
}
// Whether `path` lies under `dir`, and the part of it below.
inline bool under(const std::string &path, const std::string &dir, std::string &rest) {
    const std::string d = trimSlash(dir);
    if (d.empty() || d == "/") return false;
    if (path.size() <= d.size() + 1 || path.compare(0, d.size(), d) != 0 || path[d.size()] != '/')
        return false;
    rest = path.substr(d.size() + 1);
    return !rest.empty();
}
}  // namespace detail

// A file's path as the title shows it: from the folder, else with ~ for
// home, else whole. The folder itself shows as its name.
inline std::string displayPath(const std::string &path, const std::string &folder,
                               const std::string &home) {
    const std::string p = detail::trimSlash(path);
    std::string rest;
    if (detail::under(p, folder, rest)) return rest;
    if (!folder.empty() && p == detail::trimSlash(folder)) return detail::lastPart(p);
    if (detail::under(p, home, rest)) return "~/" + rest;
    if (!home.empty() && home != "/" && p == detail::trimSlash(home)) return "~";
    return p;
}

// What the window shows, without " — mutible": "" when there is nothing.
inline std::string subject(const Input &in) {
    const std::string folderName = in.folder.empty() ? "" : detail::lastPart(in.folder);
    switch (in.focus) {
    case Focus::Terminal:
    case Focus::Browser:
        return in.pane.empty() ? folderName : in.pane;
    case Focus::Document:
        break;
    }
    std::string s;
    if (!in.name.empty()) s = in.name;
    else if (in.untitled) s = in.path.empty() ? "Untitled-1" : detail::lastPart(in.path);
    else if (!in.path.empty()) s = displayPath(in.path, in.folder, in.home);
    else return folderName;
    if (in.gone) s += " (deleted)";
    return s;
}

// The whole title.
inline std::string title(const Input &in) {
    const std::string s = subject(in);
    if (s.empty()) return "mutible";
    const bool document = in.focus == Focus::Document;
    std::string out = document && in.dirty ? "● " : "";
    out += s;
    out += " — mutible";
    if (document) out += in.detail;
    return out;
}

}  // namespace WindowTitle
