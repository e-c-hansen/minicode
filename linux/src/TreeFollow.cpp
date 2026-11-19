// TreeFollow.cpp — see TreeFollow.h.
#include "TreeFollow.h"

#include "FileTree.h"

void TreeFollow::activeChanged() {
    // A tab closed because its file went from the tree (Move to Trash): the
    // tree keeps its selection, the row after the one that went, and its
    // scroll (HANDOFF item 110).
    if (quiet_ > 0) return;
    const std::string path = host_.followablePath();
    if (held_ > 0 || !host_.enabled()) {
        tree_->selectIfListed(path);
        return;
    }
    // The row a click or Enter in the tree is opening: selected as clicked,
    // never scrolled.
    if (!path.empty() && path == tree_->opening()) {
        tree_->selectIfListed(path);
        return;
    }
    if (!follower_.activeChanged(path, host_.treeOnShow())) {
        tree_->selectIfListed(path);
        return;
    }
    tree_->revealPath(path);   // never widens: outside the focus, left alone
}

void TreeFollow::treeMaybeShown() {
    if (!host_.treeOnShow()) return;
    const std::string waiting = follower_.treeShown();
    if (waiting.empty() || !host_.enabled()) return;
    // Still the file on show (it waited for the tree): revealed now.
    if (host_.followablePath() == waiting) tree_->revealPath(waiting);
}

void TreeFollow::hold(bool on) {
    held_ += on ? 1 : -1;
    if (held_ < 0) held_ = 0;
}

void TreeFollow::quiet(bool on) {
    quiet_ += on ? 1 : -1;
    if (quiet_ < 0) quiet_ = 0;
}

void TreeFollow::settingsApplied() {
    const bool on = host_.enabled();
    const bool turnedOn = on && !on_ && seen_;
    on_ = on;
    seen_ = true;
    if (!turnedOn || held_ > 0 || !host_.treeOnShow()) return;
    const std::string path = host_.followablePath();
    if (!path.empty()) tree_->revealPath(path);
}
