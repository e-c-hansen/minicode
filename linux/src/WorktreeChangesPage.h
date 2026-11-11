// WorktreeChangesPage.h — Compare with Main (PARITY.md item 3.5, part two;
// the Mac's MCWorktreeChangesPage): a page in a tab listing the files a
// worktree changed against where its branch left main, one row each (status
// letter, path, a rename's old path). A click on a row opens that file side
// by side with main at the branch point in the compare view, through
// `onShowComparison`.
#pragma once
#include <gtk/gtk.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "AgentWorktrees.h"
#include "EditorPage.h"

class WorktreeChangesPage : public EditorPage {
public:
    WorktreeChangesPage(AgentWorktrees* model, const WorktreeInfo& worktree);
    ~WorktreeChangesPage() override;
    GtkWidget* widget() override { return root_; }
    GtkWidget* focusWidget() override { return list_; }
    std::string key() const override { return "worktree:" + wt_.path; }
    std::string title() const override { return wt_.label + " vs " + model_->mainBranch(); }
    std::function<void(const CompareSpec&)> onShowComparison;
    std::function<void()> onLoaded;
    // For the checks.
    bool loaded() const { return loaded_; }
    std::vector<std::string> rowDescriptions() const;
    std::string noticeText() const;
    void openChangeAtRow(int row);

private:
    AgentWorktrees* model_;
    WorktreeInfo wt_;
    GtkWidget *root_ = nullptr, *title_ = nullptr, *info_ = nullptr, *notice_ = nullptr,
              *scroll_ = nullptr, *list_ = nullptr;
    std::vector<AgentView::Change> changes_;
    bool loaded_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    void show(std::vector<AgentView::Change> changes, const std::string& error);
};
