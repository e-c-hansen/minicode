// EditorTabs.h — the rules behind a window's row of open files: where a new
// tab goes, which tab a key selects, which one shows after a close, what of
// a remembered session can be reopened, and what each tab is called; and,
// at the end, Model::Window and Model::Session, which hold the tabs
// themselves by those rules (SESSION-DESIGN.md, stage 3a): a session's
// windows, each window's one or two editor groups, each group's tabs in
// order with the one on show, the preview tabs, and the closed tabs. Plain
// C++ with no I/O, so it is unit-tested. The GTK port keeps its tabs in a
// Model::Window; the Mac's EditorController still keeps its own (stage 3c
// moves it over), by the same rules.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace EditorTabs {

// Where a newly opened tab goes: just right of the active one, or at the
// end when none is active.
int insertionIndex(int active, int count);

// Tab n (1-based) for Shift+Cmd+n or Ctrl+n. 9 is always the last tab, as
// in browsers. -1 when there is no such tab.
int indexForNumber(int n, int count);

// One tab to the right (delta 1) or the left (delta -1) of `active`,
// wrapping around at either end. -1 when there are no tabs.
int step(int active, int count, int delta);

// After tab `closed` is closed, the index, in the list without it, of the
// tab to show: the active one still, when another tab was closed, else the
// one used most recently of those left (the largest `lastUsed`, which holds
// a stamp per tab before the close), or -1 when none is left.
int activeAfterClose(const std::vector<uint64_t> &lastUsed, int active, int closed);

// The tabs a window had open in a folder, as remembered between runs, and
// which of them was the preview tab (-1 for none).
struct Session {
    std::vector<std::string> files;
    int active = -1;
    int preview = -1;
};

// What of a remembered session can be opened again: files that no longer
// exist (`exists` says) and repeats are dropped. The active tab stays with
// its file, or moves to the nearest file before it that is left (the first,
// failing that), and is -1 only when nothing is left. The preview tab stays
// with its file and is -1 when its file was dropped (a repeat of a file
// already listed is that tab, kept: a file open twice was never a preview).
Session restorable(const Session &saved,
                   const std::function<bool(const std::string &)> &exists);

// What each tab shows: a file's name, and for files that share a name,
// enough of their folders to tell them apart ("src/main.cpp" beside
// "tests/main.cpp"). An empty path (a tab that holds no file) gets "".
std::vector<std::string> labels(const std::vector<std::string> &paths);

// Dragging tab `from` along a row of tabs `widths` wide, laid end to end
// from 0: where it goes when let go with the pointer at `x` (the same
// coordinates), as an index in the row after the move. Each other tab whose
// middle is left of the pointer comes before it. `from` itself when out of
// range.
int dropIndex(const std::vector<double> &widths, int from, double x);

// ---- Reopen Closed Tab
// What a closed tab leaves behind, so it can come back where it was: its
// file, its place in the row, whether it showed its preview (a Markdown or
// LaTeX file), and, when it had been on show, its view: the selection and
// the first character on show, in characters, and how far below that
// character's line the view's top was. The Mac keeps the same in
// EditorController+Tabs.mm (rememberClosedTab:); the GTK port keeps it here.
struct ClosedTab {
    std::string path;
    int index = 0;
    bool preview = false;
    bool haveView = false;
    long selStart = 0, selEnd = 0;
    long top = 0;
    double topOffset = 0;
};

// A window's closed tabs, the newest last, at most `limit` of them (50, as
// on the Mac). A tab with no file (a diff) is not remembered.
class ClosedTabs {
public:
    explicit ClosedTabs(size_t limit = 50) : limit_(limit) {}
    void remember(const ClosedTab &t);
    bool empty() const { return list_.empty(); }
    size_t size() const { return list_.size(); }
    // The newest one whose file `reopenable` accepts, taken off the list,
    // with the newer ones it rejected (a file gone, or a folder now)
    // dropped on the way. False, with the list empty, when none is left.
    bool take(ClosedTab *out, const std::function<bool(const std::string &)> &reopenable);
    // Where a reopened tab goes in a row of `count`: its old place, or the
    // end when the row is shorter now.
    static int indexIn(const ClosedTab &t, int count);

private:
    size_t limit_;
    std::vector<ClosedTab> list_;
};

// ---- The preview tab (VS Code's workbench.editor.enablePreview)
// A file opened by a single click in the file tree goes in the window's
// preview tab, its title in italics, which the next such open replaces, so
// browsing does not pile up tabs. An edit, a save, a double-click on its
// title or on the file in the tree, or Keep Open makes it an ordinary
// ("kept") tab. The agents' preview tabs (`minicode open --note`, a diff)
// follow the same rules but are their own: an agent's open never replaces
// the user's preview tab, nor a click in the tree the agent's.

// Where an open comes from, which decides whether it goes in the preview tab.
enum class OpenFrom {
    Other,             // menus, links, Problems, the agents' kept opens: kept
    TreeClick,         // a single click on a file in the tree
    TreeDoubleClick,   // the second click of a double-click on the same row
    TreeCommandClick,  // Cmd+click on a file in the tree: kept
    TreeReturn,        // Return in the tree, which also moves the keyboard: kept
    QuickOpen,         // Cmd+P (workbench.editor.enablePreviewFromQuickOpen)
    CodeNavigation,    // Go to Definition's or a reference's file
    SearchClick,       // a single click on a Find in Folder or TODO result
    SearchOpen,        // a double-click or Return on one: kept
};

struct PreviewSettings {
    bool enabled = true;          // workbench.enable-preview
    bool fromQuickOpen = false;   // workbench.enable-preview-from-quick-open
};

// Whether an open from `from` goes in the preview tab.
bool opensInPreview(OpenFrom from, const PreviewSettings &s);

// What a click on a file in the tree is. The window server counts clicks
// by time alone, so a second click within half a second is a double-click
// wherever it lands: it counts as one only when the press before it was on
// the same row. Command makes any click a kept open.
OpenFrom treeClick(int clickCount, bool sameRowAsLastPress, bool command);

// Whether a tab that is a preview stays one when its file is opened again:
// a kept open keeps it, an open in preview leaves it as it is.
bool staysPreview(bool isPreview, bool openInPreview);

// Where a new preview tab goes, given the active tab, the preview tab (-1
// for none) and the number of tabs: VS Code's place, just right of the
// active tab, with the old preview tab closed. `index` counts in the list
// after `replace` is taken out (-1: nothing replaced).
struct PreviewPlace {
    int replace = -1;
    int index = 0;
};
PreviewPlace placePreview(int active, int preview, int count);

// ---- Editor groups (VS Code's Split Editor), two at most, side by side
// Each group has its own row of tabs; one file open in both is one
// document shown twice. Group 0 is the left one.

// Where "Move Editor into Next Group" (direction 1) or "into Previous
// Group" (-1) sends the tab on show from group `focused`: the other group,
// a new group to the right when there is only one (as VS Code makes one),
// or -1 when there is none that way.
int moveTarget(int focused, bool split, int direction);

// The left group's width out of `total`, for its share `fraction`: neither
// group narrower than `minWidth` while there is room for both, else halves.
double leftGroupWidth(double total, double fraction, double minWidth);

// A remembered split: each group's files in tab order, each group's file on
// show, which group had the keyboard, and the left group's share of the
// width.
struct GroupsSession {
    std::vector<std::string> groups[2];
    std::string active[2];
    int focused = 0;
    double fraction = 0.5;
};

// The split laid out again over the files that were reopened (`open`, in
// tab order): `groups` hold indices into `open`, each group its own files
// that are open, in its order, once each; a reopened file neither group
// named (a backup's, another window's) joins the focused group. `active`
// is an index into its group (its file, else the first). With a group left
// empty there is no split (`split` false, everything else unset). The share
// is kept between 0.15 and 0.85.
struct GroupLayout {
    bool split = false;
    std::vector<int> groups[2];
    int active[2] = {-1, -1};
    int focused = 0;
    double fraction = 0.5;
};
GroupLayout restoreGroups(const GroupsSession &saved, const std::vector<std::string> &open);

}  // namespace EditorTabs

namespace Model {

// A document as the window's port knows it (the GTK Document's id, the
// Mac's MCTab): one per file in a window, however many groups show it.
// 0 is no document.
using DocId = std::uint64_t;

// Whether a tab is a preview tab, and whose (EditorTabs' preview rules).
enum class Preview : std::uint8_t {
    None,    // an ordinary ("kept") tab
    User,    // the window's preview tab: a tree click, a search result
    Agent,   // the agents' preview tab: `minicode open --note`, a diff
};

// One window's tabs. Group 0 is the left one; there is a group 1 only while
// the editor is split, and closing a group's last tab leaves the other as
// group 0. The focused group is the one the keyboard, the tab keys and the
// strip under the window's title act on; everything a port asks without
// naming a group means it. A document in both groups is one document: its
// path, preview state and last use are kept once.
//
// The window holds what a port needs to draw its strips and decide what
// shows; it reads no files and shows nothing. A port changes it, then shows
// what it says: the focused group's active() in its editor, the other
// group's in its mirror.
class Window {
public:
    explicit Window(std::size_t closedLimit = 50) : closed_(closedLimit) {}

    // The folder the window is on ("" for none), as its session is keyed.
    const std::string &root() const { return root_; }
    void setRoot(const std::string &root) { root_ = root; }

    // ---- the groups
    bool split() const { return groups_[1].tabs.size() > 0; }
    int groupCount() const { return split() ? 2 : 1; }
    int focused() const { return focused_; }
    int other() const { return split() ? 1 - focused_ : -1; }
    // A group's tabs in order (empty for no such group), and its tab on
    // show (0 for none).
    const std::vector<DocId> &tabs(int group) const;
    DocId active(int group) const;
    // The focused group's.
    const std::vector<DocId> &tabs() const { return tabs(focused_); }
    DocId active() const { return active(focused_); }
    int count() const { return (int)tabs().size(); }
    int activeIndex() const { return indexOf(active()); }
    DocId at(int index) const;   // the focused group's, 0 for none
    int indexOf(DocId id) const { return indexOf(id, focused_); }
    int indexOf(DocId id, int group) const;
    bool inGroup(DocId id, int group) const { return indexOf(id, group) >= 0; }
    bool isOpen(DocId id) const { return inGroup(id, 0) || inGroup(id, 1); }
    bool inBothGroups(DocId id) const { return inGroup(id, 0) && inGroup(id, 1); }
    // The group showing `id`'s tab: the focused one when both do, -1 when
    // neither does.
    int groupOf(DocId id) const;
    // Every document of the window once: the focused group's in tab order,
    // then the other's (backups, sessions, auto-save, the disk, the
    // language servers).
    std::vector<DocId> documents() const;
    // The left group's share of the width, 0 to 1.
    double fraction() const { return fraction_; }
    void setFraction(double f);

    // ---- what the window knows of each document
    // Its file, "" for a tab that holds none (a diff, a comparison, a
    // page), which never takes part in labels, Reopen Closed Tab or find.
    const std::string &path(DocId id) const;
    void setPath(DocId id, const std::string &path);
    // A file's tab, 0 for none.
    DocId find(const std::string &path) const;
    Preview preview(DocId id) const;
    bool isPreview(DocId id) const { return preview(id) != Preview::None; }
    void setPreview(DocId id, Preview p);
    // The window's preview tab of that kind, 0 for none.
    DocId previewTab(Preview kind = Preview::User) const;
    // An ordinary tab now (an edit, a save, a double-click on its title,
    // Keep Open, a kept open of its file). True when it was a preview.
    bool keep(DocId id);
    // A stamp of when it last came on show, larger for later (0: never).
    std::uint64_t lastUsed(DocId id) const;
    // The labels of a group's tabs (EditorTabs::labels over their paths).
    std::vector<std::string> labels(int group) const;

    // ---- opening
    // What an open of `path` from `from` does before anything is read: it
    // goes in the preview tab or not (EditorTabs::opensInPreview); a code
    // navigation in preview keeps the preview tab it starts from, unless
    // that is the file (VS Code's enablePreviewFromCodeNavigation, off);
    // and a file already in a tab gives that tab, which a kept open keeps
    // (EditorTabs::staysPreview). The tab is not put on show: activate()
    // does that.
    struct Open {
        DocId existing = 0;
        bool inPreview = false;
    };
    Open prepareOpen(const std::string &path, EditorTabs::OpenFrom from,
                     const EditorTabs::PreviewSettings &settings);
    // A new tab for `id` in the focused group. An ordinary one goes just
    // right of the tab on show. A User preview goes in VS Code's place
    // (EditorTabs::placePreview) and replaces the window's preview tab; an
    // Agent preview goes right of the tab on show and replaces the agents'
    // preview tab; but an old preview tab `keepOld` holds back (unsaved
    // changes; the tab being typed in) is kept instead, and stays. The
    // replaced tab leaves every group, is not remembered for Reopen Closed
    // Tab (VS Code leaves a replaced preview out of its recently closed),
    // and is returned for the port to let go of (0: none). The tab on show
    // stays as it was, unless it was the one replaced: then the new tab is.
    DocId add(DocId id, const std::string &path, Preview kind = Preview::None,
              const std::function<bool(DocId)> &keepOld = nullptr);
    // A tab at the end of the focused group, not on show (a session coming
    // back), or at `index` (clamped; Reopen Closed Tab's
    // ClosedTabs::indexIn).
    void append(DocId id, const std::string &path, Preview kind = Preview::None);
    void insert(DocId id, const std::string &path, int index);

    // ---- on show
    // `id` comes on show in the focused group and is stamped as used. A tab
    // only the other group has joins the focused group first, just right of
    // its tab on show (VS Code opens a file in the group you are in). False
    // when the window has no such tab.
    bool activate(DocId id);
    // By index, by step (wrapping, EditorTabs::step) and by number
    // (EditorTabs::indexForNumber): the tab put on show, 0 for none.
    DocId select(int index);
    DocId step(int delta);
    DocId selectNumber(int n);
    // A tab dragged along a group's row (-1: the focused group's), from
    // `from` to `to` (EditorTabs::dropIndex's answer). False when either is
    // out of range.
    bool move(int from, int to, int group = -1);

    // ---- closing
    struct Closed {
        bool closed = false;        // `id` had a tab in that group
        bool stillOpen = false;     // the other group still has it
        bool groupClosed = false;   // it was its group's last: the other is the only group now
        bool activeChanged = false; // the focused group's tab on show is another now
    };
    // `id`'s tab in `group` (-1: the focused group) closes. The group's tab
    // used most recently comes on show if it was on show
    // (EditorTabs::activeAfterClose); a split group left empty closes, and
    // the other group, now group 0, has the keyboard. With `remember`, a
    // file's tab the other group does not still have is kept for Reopen
    // Closed Tab, with the place the port gives (its view) and its index.
    Closed close(DocId id, int group, const EditorTabs::ClosedTab *remember);
    // `id` out of every group, nothing remembered (its file went to the
    // Trash, a preview replaced).
    void remove(DocId id);
    // Every tab, the split gone, nothing remembered (Open Folder). The
    // closed tabs stay.
    void clear();
    // The tabs Close Other Tabs and Close Tabs to the Right close, in the
    // focused group, in order.
    std::vector<DocId> othersThan(DocId id) const;
    std::vector<DocId> rightOf(DocId id) const;

    // ---- Reopen Closed Tab
    bool canReopen() const { return !closed_.empty(); }
    std::size_t closedCount() const { return closed_.size(); }
    // The newest closed tab `reopenable` accepts (EditorTabs::ClosedTabs).
    // The port opens it: activate() when the file is open again meanwhile,
    // else insert() at ClosedTabs::indexIn(c, count()).
    bool takeClosed(EditorTabs::ClosedTab *out,
                    const std::function<bool(const std::string &)> &reopenable);
    EditorTabs::ClosedTabs &closedTabs() { return closed_; }

    // ---- the split editor (two groups at most)
    // The focused group's tab on show goes into the other group too (one
    // made on the right when not split), just right of that group's tab on
    // show unless it has it already, and comes on show there. The focus
    // stays: focus() moves it, as Split Editor then does. False with no tab
    // on show.
    bool putInOther(DocId id);
    // The keyboard to `group`, whose tab on show is stamped as used. False
    // when there is no such group.
    bool focus(int group);
    // Split Editor: putInOther(active()) and the focus to that group.
    bool splitEditor();
    // Move Editor into Next (1) or Previous (-1) Group: the tab on show
    // leaves this group for the other (EditorTabs::moveTarget), which takes
    // the keyboard; the group it left closes if it was its last. Refused
    // (false) when there is no group that way, or it is the only tab of the
    // only group.
    bool canMove(int direction) const;
    bool moveActive(int direction);
    // For a session: each group's files and file on show, the focused
    // group and the share (files only, as EditorTabs::GroupsSession). And
    // back, over the tabs as they are now (EditorTabs::restoreGroups gives
    // `layout`, indices into documents()), with each group's tab on show.
    EditorTabs::GroupsSession groupsSession() const;
    bool restoreGroups(const EditorTabs::GroupLayout &layout);

private:
    struct Group {
        std::vector<DocId> tabs;
        DocId active = 0;
    };
    struct Info {
        std::string path;
        Preview preview = Preview::None;
        std::uint64_t lastUsed = 0;
    };
    std::string root_;
    Group groups_[2];
    int focused_ = 0;
    double fraction_ = 0.5;
    std::vector<std::pair<DocId, Info>> info_;   // the window's documents, few
    std::uint64_t stamp_ = 0;
    EditorTabs::ClosedTabs closed_;

    Info *infoOf(DocId id);
    const Info *infoOf(DocId id) const;
    void ensure(DocId id, const std::string &path, Preview kind);
    void forgetIfClosed(DocId id);
    void touch(DocId id);
    void insertAt(int group, int index, DocId id);
    void dropGroup(int group);
    DocId nextAfterClose(int group, int index) const;
};

// Every window of the app, each with its tabs. A window is made when it
// opens and goes when it closes; its Window stays where it is meanwhile, so
// a port may keep a pointer to it.
class Session {
public:
    using WindowId = std::uint64_t;
    WindowId addWindow();
    void removeWindow(WindowId id);
    Window *window(WindowId id);
    const Window *window(WindowId id) const;
    std::vector<WindowId> windows() const;   // in the order they opened
    // The first window with a tab of `path`, 0 for none (an open from the
    // file manager goes to the window that has the file).
    WindowId windowWith(const std::string &path) const;
    // The windows on a folder, in the order they opened (windows on one
    // folder share its remembered session).
    std::vector<WindowId> windowsOn(const std::string &root) const;

private:
    std::vector<std::pair<WindowId, std::unique_ptr<Window>>> windows_;
    WindowId next_ = 0;
};

}  // namespace Model
