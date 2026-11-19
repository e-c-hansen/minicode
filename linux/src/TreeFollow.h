// TreeFollow.h — the file tree following the active tab (explorer.auto-reveal,
// on by default, as VS Code's explorer.autoReveal but centered; the Mac's
// EditorController+TreeReveal.mm). When another tab comes on show (the
// editor's onActiveChanged, which every way there goes through: a click on
// a tab, Ctrl+1 to 9, the next and previous tab, a click into the other
// editor group, a close showing the next, Quick Open, a definition, a search
// result), its file's folders open, its row is selected, and the tree
// scrolls the row to its middle only when it is not wholly in view
// (FileTree::revealPath, the core's TreeReveal::scrollTop), so clicking
// between two files on show never moves it. The keyboard stays where it is.
//
// The tree is left alone for a file outside every focused folder (the focus
// never widens; only Reveal Active File in File Tree does), a page, a
// comparison, an untitled tab, or a file outside the window's folder; with
// the setting off it only selects the file's row when it is listed already.
// A click or Enter in the tree selects its row without scrolling. While the
// tree is hidden (the sidebar closed, or Source Control, Processes or Agents
// in its place) the change waits (TreeReveal::Follower) and is revealed once
// when the tree shows again. A window's tabs coming back (hold) put the
// tree back as the window's record has it, not moved to the file.
#pragma once

#include <functional>
#include <string>

#include "FileDetails.h"   // TreeReveal

class FileTree;

class TreeFollow {
public:
    struct Host {
        // The active tab's file as the tree lists it, or "" for none the
        // tree has (a page, an untitled tab, a file gone or outside the
        // window's folder).
        std::function<std::string()> followablePath;
        // Whether the tree's rows are on screen: the sidebar open, with the
        // tree in it.
        std::function<bool()> treeOnShow;
        // explorer.auto-reveal.
        std::function<bool()> enabled;
    };
    TreeFollow(FileTree* tree, Host host) : tree_(tree), host_(std::move(host)) {}

    // Another tab is on show: its file revealed, centered unless in view,
    // or kept for when the tree shows again.
    void activeChanged();
    // The tree may have come on show (the sidebar opened, the tree back in
    // it): a file that came on show while it was hidden is revealed, once.
    void treeMaybeShown();
    // Held while a window's tabs come back (Persistence::restoreSession).
    void hold(bool on);
    // Held while the tabs of a file that went from the tree close (Move to
    // Trash): the tab shown next is neither revealed nor selected, so the
    // tree keeps its scroll and the row after the one that went.
    void quiet(bool on);
    // The settings were applied: explorer.auto-reveal turned on reveals the
    // file on show, as the Mac's treeAutoRevealSettingsApplied does.
    void settingsApplied();

private:
    FileTree* tree_;
    Host host_;
    TreeReveal::Follower follower_;
    int held_ = 0;
    int quiet_ = 0;
    bool on_ = false, seen_ = false;   // the setting as last applied, once it was
};
