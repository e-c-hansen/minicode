// GitDiffView.h — a diff or a commit, read-only and colored, in the editor's
// slot (Editor makes one on the first diff), the Mac's diffScroll in
// src/EditorController+Rendering.mm with MCGitDiffText's colors.
//
// A file's diff that the Source Control panel showed last also carries the
// panel's hunk actions (PARITY.md 2.9, the Mac's W26 in src/GitPanel.mm):
// "Stage Hunk · Discard Hunk", or "Unstage Hunk" in a staged diff, as links
// after each hunk's header on the same line, so line i of the text stays line
// i of what git printed (GitUi::diffTextWithActions); and a right click, the
// Menu key or Shift+F10 offers the actions for the hunk there and for the
// changed lines selected (GitUi::hunkMenu) above Copy and Select All. Which
// diff that is and what an action does are the panel's (GitPanel::
// attachDiffView, hunksFor and onHunk); a commit, or any other diff, shows
// plain, with the text view's own menu.
#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "GitModel.h"

class GitDiffView {
public:
    GitDiffView();
    // While the window's widgets still exist (Editor's destructor).
    ~GitDiffView();
    GitDiffView(const GitDiffView&) = delete;
    GitDiffView& operator=(const GitDiffView&) = delete;

    GtkWidget* widget() const { return scroller_; }
    // `commit`: `git show` output (header, message, stat and diff); else a
    // plain diff.
    void show(const std::string& bytes, bool commit);
    GtkTextBuffer* buffer() const { return buffer_; }
    GtkWidget* textView() const { return view_; }

    // Whether `bytes` is the diff the panel showed last, with its side; asked
    // on every show of a plain diff.
    std::function<bool(const std::string& bytes, GitUi::DiffSide* side)> hunksFor;
    // A link or a menu item: `op` on the hunk holding `first`, or with
    // `lines` on the changed lines among first..last (lines of the diff).
    std::function<void(GitUi::HunkOp op, size_t first, size_t last, bool lines)> onHunk;

    // ---- for the checks
    struct Link {
        GitUi::HunkOp op;
        size_t line;      // the hunk's header line
        int start, end;   // its words, in characters of the buffer
    };
    const std::vector<Link>& links() const { return links_; }
    GtkWidget* menu() const { return menu_; }   // the hunk menu, while it is up
    std::string text() const;

private:
    void build();
    size_t lineOf(int offset) const;   // a buffer offset's line of the diff
    int linkAt(int offset) const;      // the link holding it, or -1
    bool iterAtPoint(double x, double y, GtkTextIter* it) const;
    // The hunk menu for a click on `line` (or the caret's), at `x`, `y`;
    // false when there is nothing to offer, and the view's own menu shows.
    bool popupHunkMenu(size_t line, double x, double y);
    void dropMenu();

    static void onPressed(GtkGestureClick*, int n, double x, double y, gpointer self);
    static void onReleased(GtkGestureClick*, int n, double x, double y, gpointer self);
    static void onMenuPressed(GtkGestureClick*, int n, double x, double y, gpointer self);
    static gboolean onKey(GtkEventControllerKey*, guint key, guint code, GdkModifierType mods,
                          gpointer self);
    static void onMotion(GtkEventControllerMotion*, double x, double y, gpointer self);
    static gboolean onTooltip(GtkWidget*, int x, int y, gboolean keyboard, GtkTooltip* tip,
                              gpointer self);
    static void onHunkAction(GSimpleAction*, GVariant* target, gpointer self);

    GtkWidget* scroller_ = nullptr;
    GtkWidget* view_ = nullptr;
    GtkTextBuffer* buffer_ = nullptr;
    GtkWidget* menu_ = nullptr;
    GSimpleActionGroup* actions_ = nullptr;

    // What is on show.
    std::string bytes_;               // git's output, when it has actions
    GitUi::DiffSide side_ = GitUi::DiffSide::Untracked;
    bool hunks_ = false;              // the panel's diff, with its actions
    std::vector<int> lineStarts_;     // each line's first character
    std::vector<Link> links_;
    int pressOffset_ = -1;            // where a left press landed, or -1
    bool overLink_ = false;
};
