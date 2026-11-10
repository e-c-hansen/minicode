// TreeGitColors.h — the file tree's git colors on Linux, linux/PARITY.md
// item 2.10: the GTK counterpart of the Mac's MCTreeGitColors
// (src/TreeGitColors.mm), over the core's Git::Decorations (GitStatus.h),
// as VS Code's explorer decorations color a row.
//
// One `git status --porcelain=v2 -z --ignored=matching` per look, on a
// thread of its own, half a second after the first change, so a burst of
// writes costs one status; while one runs, another follows it. The last
// record wins per path; a folder takes the strongest of the changes inside
// it, deletions and ignored files never propagating; an untracked or
// ignored folder is one entry, so a node_modules costs a line. A change
// only inside an ignored folder, or in .git's objects and logs, runs no
// status. A folder outside git runs `git rev-parse` again only when its own
// top folder changes (a git init there).
//
// Changes come from the tree's own folder monitors (FileTree.h, the
// activity callback with the path) and from a watch on the repository's
// git folder (GitDirWatch.h): GIO's monitors see one folder each, so a
// stage, commit or checkout, which writes only the index and HEAD, is seen
// there, where the Mac's FSEvents stream sees it under the root. The tree
// watches only the folders it has listed (the root and the folders opened
// in it): a file another program changes in a folder never opened is
// colored at the next look, which the window coming back to the front, a
// save in the editor (main.cpp tells it of both), git's index or any change
// in a listed folder brings.
//
// The colors are the settings' tree.git-* keys, VS Code's dark or light
// defaults by the theme (Settings::treeGitColor), drawn by the stylesheet
// (ThemeCss.cpp) through a class on the row's name; the tooltip names the
// status ("Modified (git)").
#pragma once

#include <glib.h>

#include <functional>
#include <memory>
#include <string>

#include "GitDirWatch.h"
#include "GitStatus.h"

class TreeGitColors {
public:
    TreeGitColors();
    // git already running finishes on its own thread; its answer is dropped.
    ~TreeGitColors();
    TreeGitColors(const TreeGitColors&) = delete;
    TreeGitColors& operator=(const TreeGitColors&) = delete;

    // The tree's folder, as it spells it; a new one looks at once.
    void setRoot(const std::string& root);
    // Something at `path` changed, as the tree's folder monitors report it.
    void changedOnDisk(const std::string& path);
    // Look again soon, whatever changed: the window came back to the front,
    // as the Source Control panel does then.
    void lookSoon() {
        if (!root_.empty()) schedule();
    }

    // A row's color, for a path as the tree spells it.
    Git::Decoration decorationFor(const std::string& path, bool isDir) const;
    // Its stylesheet class ("minicode-git-modified") and tooltip
    // ("Modified (git)"); null and "" for none.
    static const char* cssClass(Git::Decoration d);
    static std::string tooltip(Git::Decoration d);

    // The colors changed: the tree's rows on show ask again.
    std::function<void()> onChange;

    // For the checks.
    unsigned runs() const { return runs_; }   // statuses applied
    bool inRepository() const { return !top_.empty(); }

private:
    void schedule();
    void look();
    void post(std::function<void()> job);
    void watchGitDir(const std::string& gitDir);

    GThreadPool* pool_ = nullptr;
    std::shared_ptr<bool> alive_;

    std::string root_;       // as the tree spells it
    std::string realRoot_;   // links resolved, as git spells it
    std::string top_;        // the repository's top level, "" when not in one
    std::string prefix_;     // the root from the top level ("src/" or "")
    std::string gitDir_;
    bool looked_ = false;    // a look finished for this root
    std::shared_ptr<const Git::Decorations> decorations_;
    unsigned generation_ = 0;
    bool running_ = false, again_ = false;
    guint timer_ = 0;
    unsigned runs_ = 0;
    std::unique_ptr<GitDirWatch> watch_;
};
