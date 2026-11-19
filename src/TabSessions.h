// TabSessions.h — the tabs each folder had open, kept between runs: what the
// GTK port writes to $XDG_STATE_HOME/minicode/sessions.json
// (linux/src/Persistence.cpp), its counterpart of the Mac's MCTabSessions
// (EditorController+Persistence.mm, kept in the user defaults there). Pure
// C++17 with no I/O: the port reads and writes the file, and this says what
// is in it, by the Mac's rules.
//
// The file is one JSON object keyed by the folder's resolved path (the
// Mac's is ~/Library/Application Support/Mutible/sessions.json since stage
// 3c, which carried its user defaults' MCTabSessions and MCOpenWindows over
// once):
//
//   {"/home/me/proj": {"used": 1762700000.5, "active": 1, "preview": -1,
//     "tabs": [{"path": "/home/me/proj/a.txt", "page": false,
//               "sel": [3, 3], "top": 120, "into": 4}, ...]}}
//
// "sel" is the selection in characters (code points on Linux, UTF-16 units
// on the Mac, each reading back only what it wrote); "top" and "into" are
// there only when where the tab's view was is known; "side" only when the
// page shows beside the source (the Mac's preview to the side).
//
// Below the sessions, the windows a launch brings back (encodeWindows):
// every window open at the end of the last run, front first, each with its
// frame, folder, tabs (a Session as above, carets and scroll included), its
// panels and their sizes, its terminals' folders and splits, and the tree's
// open folders and selected row: VS Code's window.restoreWindows "all".
// Both ports keep the list in sessions.json, under the key "windows" beside
// the folders (encodeStore). Folder keys are absolute paths, so
// "windows" is never one; a sessions.json from before has no windows, and a
// mutible from before skips the key (it is no session) and keeps the
// folders.
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "EditorTabs.h"
#include "TerminalLayout.h"

namespace TabSessions {

struct Tab {
    std::string path;
    bool page = false;          // it showed its rendered page (Markdown, LaTeX)
    bool side = false;          // its page showed beside its source
    long selStart = 0, selEnd = 0;   // the selection, in characters
    // Where its view was, when that is known: the first character on show,
    // and how far below its line's top the view started (pixels), as
    // EditorTabs::ClosedTab keeps them.
    bool haveView = false;
    long top = 0;
    long into = 0;
};

// One side of a comparison as it is made again when its tab comes back:
// the file on disk (rev ""), or the file as git has it at a revision (rev
// "HEAD", a commit's hash) or in the index (rev ":"). A side git does not
// have (a file added since) is empty.
struct ComparedSide {
    std::string path;    // absolute
    std::string rev;
    std::string label;   // its pane's header, as it was
};

// A tab that shows no file (2026-11-18), brought back by making it again
// from git and the disk rather than from what it showed: "review" (the
// window's Uncommitted Changes), "commit" (a commit's page: `ref` its hash,
// `title` the tab's) or "compare" (a side-by-side comparison of `left` and
// `right`: `title` the tab's, `fileName` the name its colors go by). Kept
// apart from the files (Session::pages), so a mutible from before, or the
// GTK port, reads the files as it always did and leaves these out.
struct PageTab {
    std::string kind;
    int at = 0;   // its place among the window's tabs, the files counted too
    std::string ref;
    std::string title;
    std::string fileName;
    ComparedSide left, right;
};
// The kinds above; a page of another kind does not read.
bool isPageKind(const std::string &kind);
// A revision a side may name: "", ":" (the index), or a ref of letters,
// digits and . _ / ~ ^ - that starts with none of - . / (no option, no
// path), at most 200 bytes.
bool isComparedRev(const std::string &rev);

// One browser tab of a window: its address, its title and WebKit's record
// of its history and scroll (base64; empty when there is none). A tab whose
// record is left out still comes back at its address (BrowserTabs.h).
struct BrowserTab {
    std::string url;
    std::string title;
    std::string state;
    // The Mac's record in NSKeyedArchiver's form rather than WebKit's own
    // (Browser.mm MCRecordData), written only when set.
    bool archived = false;
};

// One folder's tabs, in tab order, with the one on show and the preview tab
// (EditorTabs.h), -1 for none, and when it was last written (Unix seconds).
struct Session {
    std::vector<Tab> tabs;
    int active = -1;
    int preview = -1;
    double used = 0;
    // The browser pane's tabs and the one on show (-1 for none). A session
    // written before the pane had tabs has none.
    std::vector<BrowserTab> browser;
    int browserActive = -1;
    // Whether the pane showed, and its share of the width (0: unknown), with
    // the browser's tabs (the Mac's; GTK keeps them in its window record).
    bool browserShown = false;
    double browserFraction = 0;
    // The split editor's groups (EditorTabs::GroupsSession), when the
    // editor was split; a session from before the split editor has none and
    // comes back as one group.
    bool split = false;
    EditorTabs::GroupsSession groups;
    // The tree's focused folders (Focus on Folder and Add to Focus,
    // FileDetails.h's TreeFocus), each a path from the folder, in the order
    // they were added; none for the whole folder. Kept as a list; the
    // single path written before 2026-11-18 reads as a list of one. The
    // port checks each still names a folder (TreeFocus::restoredSet)
    // before it uses them.
    std::vector<std::string> treeFocus;
    // Its tabs that show no file (PageTab), in tab order, and which of them
    // was on show (-1: a file was, or none); `active` then names the file
    // shown last, which a mutible from before shows instead.
    std::vector<PageTab> pages;
    int activePage = -1;
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
// (and no page, browser tab or tree focus) removes the folder. Past `max` folders, those written longest ago go.
void put(Sessions &all, const std::string &key, Session s, double now,
         size_t max = kMaxFolders);

// The files, the tab on show and the preview tab, for EditorTabs::restorable.
EditorTabs::Session files(const Session &s);

// Where a tab's view was, as Reopen Closed Tab keeps it (path, selection,
// first character on show and the page flag; the index is left at 0).
EditorTabs::ClosedTab place(const Tab &t);

// ------------------------------------------------- the windows, next launch

// A window's frame in screen points, AppKit's way (y up from the bottom of
// the main screen).
struct Frame {
    double x = 0, y = 0, w = 0, h = 0;
};

// One terminal of a terminal tab: the folder its shell was in, where a
// fresh shell starts (a running program is not brought back), and the name
// the user gave it ("" for none).
struct TerminalPane {
    std::string cwd;
    std::string title;
};

// One tab of the terminal dock: its panes column by column, each top to
// bottom, with their shares (TerminalLayout.h) and the active pane.
struct TerminalTab {
    std::vector<std::vector<TerminalPane>> columns;
    TerminalLayout::Layout layout;
    int activeColumn = 0, activeRow = 0;
};

struct Window {
    std::string root;   // its folder
    bool haveFrame = false;
    Frame frame;        // as it was out of full screen
    bool fullScreen = false;
    Session tabs;       // its own tabs, by the rules of a folder's session
    // The sidebar: what fills it ("tree", "git", "agents" or "processes"),
    // its width, and whether Cmd+B hid it.
    std::string sidebar = "tree";
    double sidebarWidth = 0;
    bool sidebarHidden = false;
    bool editorHidden = false;
    bool problems = false;      // the Problems list above the terminal
    // The terminal dock: shown or not, its height, and its tabs (none when
    // the window had no terminal).
    bool terminal = false;
    double terminalHeight = 0;
    std::vector<TerminalTab> terminals;
    int terminalTab = -1;
    // The tree: the folders open in it, the selected row and how far it was
    // scrolled.
    std::vector<std::string> expanded;
    std::string selected;
    double treeScroll = 0;
    // Which mutible wrote it down (its pid and start time, Unix seconds),
    // and when (Unix seconds). Several mutibles running at once share the
    // record, each keeping the others' windows (mergeWindows); 0 in a
    // record from before, which counts as the last run's.
    long ownerPid = 0;
    double ownerStart = 0;
    double saved = 0;
    // The window's name in the backups (Backups::Entry::window): kept from
    // launch to launch, so the window brought back takes its own unsaved
    // buffers and leaves another window's on the same folder to it. ""
    // in a record from before 2026-11-19.
    std::string backupKey{};
};

// Front window first. A window, or a part of one, that does not read is left
// out; text that is not JSON gives none.
std::string encodeWindows(const std::vector<Window> &windows);
std::vector<Window> decodeWindows(const std::string &text);

// The whole of sessions.json: every folder's session and the windows to
// bring back. Text that is not JSON gives neither; a file of folders only
// (as every sessions.json was before) gives its folders and no window.
constexpr const char *kWindowsKey = "windows";
struct Store {
    Sessions folders;
    std::vector<Window> windows;
};
std::string encodeStore(const Store &store);
Store decodeStore(const std::string &text);

// The windows to keep as a window closes: the others, or the closing one
// alone when it was the last (closing it quits mutible, and the next
// launch brings it back, as quitting would have).
std::vector<Window> afterClosing(const std::vector<Window> &open, size_t closing);

// A running mutible, as the record names one: its pid and start time.
struct Process {
    long pid = 0;
    double start = 0;
};
// Whether a process (pid, start time) is running now; the Mac asks the
// kernel. Same pid and a start within a second (rounding) is the same
// process; a reused pid started later is not.
using IsRunning = bool (*)(long pid, double start, void *context);
bool sameProcess(long pid, double start, const Process &p);

// What a mutible writes as the windows to bring back: its own (`mine`,
// front first, stamped as `me`'s at `now`), then the windows another
// mutible wrote down that are still its to keep: one that is running
// (its windows are open there), or one that wrote since `me` started (it
// quit while `me` ran, and its windows are what it left). The windows of a
// mutible gone before `me` started are the last run's, which `me`'s
// replace, as one mutible's always have. Untagged windows (written by
// mutible 1.5.31 and before) count as the last run's. Without this, two
// mutibles at once (a build beside the Homebrew copy, `mutible -n`)
// each wrote its own windows over the other's: quitting one with four
// windows and the other three seconds later left a record of one.
std::vector<Window> mergeWindows(const std::vector<Window> &mine, const std::vector<Window> &stored,
                                 const Process &me, double now, IsRunning running, void *context);

// The stored windows a launch with nothing to open brings back: all but
// those another running mutible holds open (opening them twice would
// give two windows on one folder, each writing over the other).
std::vector<Window> toRestore(const std::vector<Window> &stored, const Process &me,
                              IsRunning running, void *context);

// The stored window a folder opened by name (the command line, Finder, a
// window per folder with backups) comes back as, VS Code's way of reopening
// a folder with its editors and panels (2026-11-18): the first on `root`
// that neither another running mutible holds open nor `me` wrote (a window
// this process closed went with its own tabs to the folder's session); -1
// for none, and the folder's session alone brings its tabs back.
int toReopen(const std::vector<Window> &stored, const std::string &root, const Process &me,
             IsRunning running, void *context);

// Where a saved frame goes on the screens there are now (their visible
// frames, the main screen first): where it was when it lies on a screen,
// else onto the screen it overlapped most, or the main one when it
// overlaps none (a screen unplugged since), shrunk to fit and never under
// the window's minimum size. No screens: as saved.
Frame fitFrame(const Frame &saved, const std::vector<Frame> &screens, double minW, double minH);

// A terminal tab as it reads back: at least one pane, and shares that
// match the panes (even ones when the saved shares do not).
TerminalTab sanitize(TerminalTab t);

}  // namespace TabSessions
