// StatusBranchItem.h — the GTK counterpart of the Mac's MCStatusBranchItem
// (mac/src/StatusBranchItem.h). The status bar's branch item, at its left
// end, as VS Code's bottom left: the branch (the short hash when HEAD is
// detached, "*" after it with uncommitted changes), then, when the branch
// has an upstream, what there is to push and pull ("↑2 ↓1"). A click on the
// name switches branch, one on the counts syncs (pull, then push); main.cpp
// connects them to the Source Control panel's own Switch Branch and Sync.
// The words come from the core (Git::statusBranch, GitStatus.h), read from
// the tree's git status (TreeGitColors.h), so the item runs no git of its
// own. Hidden outside a repository.
#pragma once

#include <gtk/gtk.h>

#include <functional>

#include "GitStatus.h"

class StatusBranchItem {
public:
    StatusBranchItem();
    StatusBranchItem(const StatusBranchItem&) = delete;
    StatusBranchItem& operator=(const StatusBranchItem&) = delete;

    // The box to put in the status bar.
    GtkWidget* widget() const { return box_; }

    // Shows the words of `branch`; the whole item hides when it is not
    // shown, and the counts alone without an upstream.
    void show(const Git::StatusBranch& branch);

    // The two clicks.
    std::function<void()> onSwitchBranch;
    std::function<void()> onSync;

    // For the checks.
    GtkWidget* nameButton() const { return name_; }
    GtkWidget* countsButton() const { return counts_; }

private:
    GtkWidget* box_ = nullptr;
    GtkWidget* name_ = nullptr;
    GtkWidget* counts_ = nullptr;
};
