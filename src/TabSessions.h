// TabSessions.h — the tabs each folder had open, kept between runs: what the
// GTK port writes to $XDG_STATE_HOME/minicode/sessions.json
// (linux/src/Persistence.cpp), its counterpart of the Mac's MCTabSessions
// (EditorController+Persistence.mm, kept in the user defaults there). Pure
// C++17 with no I/O: the port reads and writes the file, and this says what
// is in it, by the Mac's rules.
//
// The file is one JSON object keyed by the folder's resolved path:
//
//   {"/home/me/proj": {"used": 1762700000.5, "active": 1, "preview": -1,
//     "tabs": [{"path": "/home/me/proj/a.txt", "page": false,
//               "sel": [3, 3], "top": 120, "into": 4}, ...]}}
//
// "sel" is the selection in characters; "top" and "into" are there only when
// where the tab's view was is known.
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "EditorTabs.h"

namespace TabSessions {

struct Tab {
    std::string path;
    bool page = false;          // it showed its rendered page (Markdown, LaTeX)
    long selStart = 0, selEnd = 0;   // the selection, in characters
    // Where its view was, when that is known: the first character on show,
    // and how far below its line's top the view started (pixels), as
    // EditorTabs::ClosedTab keeps them.
    bool haveView = false;
    long top = 0;
    long into = 0;
};

// One folder's tabs, in tab order, with the one on show and the preview tab
// (EditorTabs.h), -1 for none, and when it was last written (Unix seconds).
struct Session {
    std::vector<Tab> tabs;
    int active = -1;
    int preview = -1;
    double used = 0;
};

// Every folder's, by its resolved path.
using Sessions = std::map<std::string, Session>;

// The Mac keeps the 50 folders used most recently.
constexpr size_t kMaxFolders = 50;

std::string encode(const Sessions &all);
// What of the file reads: a folder or a tab that does not is left out, and a
// file that is not JSON at all gives none, never a half-read session.
Sessions decode(const std::string &text);

// A window's session with the tabs of the other windows open on the same
// folder after its own, each file once, so that whichever window saves last,
// the folder reopens every file any of them had (the Mac's saveSession). The
// tab on show and the preview tab stay this window's.
Session merged(const Session &mine, const std::vector<Session> &others);

// `s` as the folder's session, written at `now`; a session with no tab
// removes the folder. Past `max` folders, those written longest ago go.
void put(Sessions &all, const std::string &key, Session s, double now,
         size_t max = kMaxFolders);

// The files, the tab on show and the preview tab, for EditorTabs::restorable.
EditorTabs::Session files(const Session &s);

// Where a tab's view was, as Reopen Closed Tab keeps it (path, selection,
// first character on show and the page flag; the index is left at 0).
EditorTabs::ClosedTab place(const Tab &t);

}  // namespace TabSessions
