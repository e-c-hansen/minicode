// GitHunksSuite.cpp — `./build/minicode-guitest git_hunks` (PARITY.md 2.9),
// the counterpart of the Mac's git_hunks in tests/gui/GitPanelSuite.mm, by
// real clicks and keys: a file changed in two places shows Stage Hunk and
// Discard Hunk as links on each hunk's header line, one line of text a line
// of the diff; a click on the second's Stage Hunk stages only it (git diff
// --cached holds it alone) and the diff shows what is left; in the staged
// diff, one of the two added lines selected by clicks, the right click
// unstages just it; Discard Hunk asks, puts the hunk back as git has it
// with a copy of the file in the Trash, and Undo Discard brings it back; a
// diff gone stale since it was shown is refused with the Mac's words and
// shown again; Shift+F10 opens the hunk menu at the caret.
#include "GitChecks.h"

using namespace gitchecks;

namespace {

std::string R() { return MCGuiScratch() + "/githunks"; }
const std::string kBoth = "a\nB\nc\nd\ne\nf\ng\nh\ni\nj\nk\nk2\nk3\nl\nm\n";
int gActions = 0;
bool Done() { return Panel()->actionsDone() > gActions; }
std::string Index() { return GitText({"show", ":f.txt"}); }

// The character offset in the diff's text where `piece` starts, or -1.
int OffsetOf(const std::string& piece) {
    const std::string t = Diff()->text();
    const size_t at = t.find(piece);
    return at == std::string::npos ? -1 : (int)g_utf8_pointer_to_offset(t.c_str(), t.c_str() + at);
}

}  // namespace

MC_GUI_SUITE(git_hunks) {
    if (GitPath().empty()) {
        MCGuiStep("no git", [] { MCGuiNote("no git here: the hunk checks are skipped"); });
        return;
    }
    Shell("a repository; f.txt changed near its top and near its end",
          "set -e; mkdir -p '" + R() + "'; cd '" + R() + "'; " + G() +
              " init -q -b main; printf 'a\\nb\\nc\\nd\\ne\\nf\\ng\\nh\\ni\\nj\\nk\\nl\\nm\\n' > f.txt; " +
              G() + " add -A; " + G() + " commit -qm first; "
              "printf 'a\\nB\\nc\\nd\\ne\\nf\\ng\\nh\\ni\\nj\\nk\\nk2\\nk3\\nl\\nm\\n' > f.txt");
    MCGuiStep("a Trash of our own", [] { UseScratchTrash(true); });
    OpenRepoWindow(R(), true);
    MCGuiWait("f.txt changed", 15, [] {
        return Rows({"# Changes  1", "W M f.txt"}) && RowWidget("W M f.txt");
    });
    ClickRow("W M f.txt");   // its diff
    MCGuiWait("two hunks, each with Stage Hunk and Discard Hunk", 10, [] { return LinkCount() == 4; });
    MCGuiStep("the links read as they say; a click on the second's Stage Hunk", [] {
        GCHECK_EQ_STR(LinkText(0), "Stage Hunk");
        GCHECK_EQ_STR(LinkText(3), "Discard Hunk");
        const std::vector<GitDiffView::Link>& l = Diff()->links();
        GCHECK(l.size() == 4 && l[0].op == GitUi::HunkOp::Stage &&
               l[1].op == GitUi::HunkOp::Discard && l[2].line > l[0].line);
        // Still one line of text per line of the diff.
        const std::string text = Diff()->text(), git = GitText({"diff", "--", "f.txt"});
        GCHECK(std::count(text.begin(), text.end(), '\n') == std::count(git.begin(), git.end(), '\n'));
        MCGuiSnapshot(Diff()->widget(), "git-hunk-links");
        gActions = Panel()->actionsDone();
        ClickLink(2);
    });
    MCGuiWait("staged, and the diff shows the hunk left", 15, [] {
        return Done() && LinkCount() == 2 &&
               Rows({"# Staged changes  1", "S M f.txt", "# Changes  1", "W M f.txt"});
    });
    MCGuiStep("only that hunk is in the index", [] {
        GCHECK_EQ_STR(Panel()->errorLine(), "Staged a hunk of f.txt.");
        const std::string cached = GitText({"diff", "--cached"});
        GCHECK(cached.find("+k2\n+k3\n") != std::string::npos && cached.find("+B") == std::string::npos);
        GCHECK_EQ_STR(Index(), "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nk2\nk3\nl\nm\n");
        GCHECK_EQ_STR(MCGuiReadFile(In("f.txt")), kBoth);
        GCHECK(Diff()->text().find("+k2") == std::string::npos &&
               Diff()->text().find("+B") != std::string::npos);
    });
    ClickRow("S M f.txt");
    MCGuiWait("the staged diff, with Unstage Hunk", 10, [] {
        return LinkCount() == 1 && Diff()->links()[0].op == GitUi::HunkOp::Unstage;
    });
    MCGuiStep("+k2 selected: a click at its start", [] {
        double x = 0, y = 0;
        PointOf(OffsetOf("+k2\n"), &x, &y);
        MCGuiClick(Diff()->textView(), x, y);
    });
    MCGuiPause(0.6);   // past the double-click time, so the next is a click of its own
    MCGuiStep("and a Shift click at its end", [] {
        double x = 0, y = 0;
        PointOf(OffsetOf("+k2\n") + 3, &x, &y);
        MCGuiClickWith("shift", Diff()->textView(), x, y);
    });
    MCGuiWait("selected", 10, [] {
        GtkTextIter a, b;
        if (!gtk_text_buffer_get_selection_bounds(Diff()->buffer(), &a, &b)) return false;
        char* t = gtk_text_buffer_get_text(Diff()->buffer(), &a, &b, TRUE);
        const bool ok = t && std::string(t) == "+k2";
        g_free(t);
        return ok;
    });
    MCGuiStep("the right click on it", [] {
        double x = 0, y = 0;
        PointOf(OffsetOf("+k2\n") + 1, &x, &y);
        MCGuiClick(Diff()->textView(), x, y, 3);
    });
    MCGuiWait("the hunk menu", 10, [] { return MenuHas(Diff()->menu(), "Unstage Selected Lines"); });
    MCGuiStep("the hunk's, then the lines'; Unstage Selected Lines", [] {
        const std::vector<std::string> l = MenuLabels(Diff()->menu());
        std::string all;
        for (const std::string& s : l) all += s + " | ";
        MCGuiNote("diff menu: %s", all.c_str());
        GCHECK(l.size() >= 2 && l[0] == "Unstage Hunk" && l[1] == "Unstage Selected Lines");
        GCHECK(MenuHas(Diff()->menu(), "Copy"));
        gActions = Panel()->actionsDone();
        MCGuiClick(MenuItem(Diff()->menu(), "Unstage Selected Lines"));
    });
    MCGuiWait("k2 unstaged, k3 still staged", 15, [] {
        return Done() && Index() == "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nk3\nl\nm\n";
    });
    MCGuiStep("it says so; the work tree untouched; the changes' diff", [] {
        GCHECK_EQ_STR(Panel()->errorLine(), "Unstaged the selected lines of f.txt.");
        GCHECK_EQ_STR(MCGuiReadFile(In("f.txt")), kBoth);
    });
    ClickRow("W M f.txt");
    MCGuiWait("two hunks again (B, and k2)", 10, [] {
        return LinkCount() == 4 && Diff()->text().find("+k2") != std::string::npos;
    });
    MCGuiStep("Discard Hunk on the first", [] {
        gActions = Panel()->actionsDone();
        ClickLink(1);
    });
    MCGuiWait("it asks", 10, QuestionUp);
    MCGuiStep("about the hunk; yes", [] {
        GCHECK(QuestionText().find("Discard this hunk of f.txt?") != std::string::npos);
        Answer("Discard Hunk");
    });
    MCGuiWait("discarded", 15, Done);
    MCGuiStep("b is back, k2 kept; the file as it was is in the Trash", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiNote("after discard: %s", Panel()->errorLine().c_str());
        GCHECK_EQ_STR(MCGuiReadFile(In("f.txt")), "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nk2\nk3\nl\nm\n");
        GCHECK(Trashed(kBoth));
        GCHECK_EQ_STR(Panel()->undoDiscardTitle(), "Undo Discard of f.txt");
        gActions = Panel()->actionsDone();
    });
    ClickBranch("the panel's right click", 3);
    MCGuiWait("the panel's menu", 10, [] { return MenuHas(Panel()->menu(), "Undo Discard of f.txt"); });
    MCGuiStep("Undo Discard of f.txt", [] { MCGuiClick(MenuItem(Panel()->menu(), "Undo Discard of f.txt")); });
    MCGuiWait("undone", 15, [] { return Done() && MCGuiReadFile(In("f.txt")) == kBoth; });
    // The diff shown again after the discard (the panel says the discard is
    // done as it asks git for it), laid out, and not shown again by the undo.
    MCGuiWait("the diff on show is the one from before the undo", 10, [] {
        return LinkCount() == 2 && Diff()->text().find("+B") == std::string::npos;
    });
    Shell("f.txt staged from outside, so the diff on show is stale",
          "cd '" + R() + "' && " + G() + " add f.txt");
    MCGuiStep("Stage Hunk on the stale diff", [] {
        gActions = Panel()->actionsDone();
        ClickLink(0);
    });
    MCGuiWait("refused", 15, Done);
    MCGuiStep("said why; nothing changed", [] {
        MCGuiNote("stale: %s", Panel()->errorLine().c_str());
        GCHECK(Panel()->errorLine().rfind("f.txt changed since this diff was shown", 0) == 0);
        GCHECK_EQ_STR(Index(), kBoth);
        GCHECK_EQ_STR(MCGuiReadFile(In("f.txt")), kBoth);
    });
    MCGuiWait("and the diff shown again: nothing left to stage", 10, [] {
        return DiffOnShow() && Diff()->links().empty() &&
               Diff()->text().find("No differences to show.") != std::string::npos;
    });

    // ------------------------------------------- the menu from the keyboard
    ClickRow("S M f.txt");   // the staged diff
    MCGuiWait("its two hunks", 10, [] { return LinkCount() == 2; });
    MCGuiStep("a click on a line of the second hunk", [] {
        double x = 0, y = 0;
        PointOf(OffsetOf(" l\n") + 1, &x, &y);
        MCGuiClick(Diff()->textView(), x, y);
    });
    MCGuiWait("the diff has the keyboard", 10, [] { return gtk_widget_has_focus(Diff()->textView()); });
    MCGuiStep("Shift+F10", [] { MCGuiKey("shift+f10"); });
    MCGuiWait("the hunk menu at the caret", 10, [] { return MenuHas(Diff()->menu(), "Unstage Hunk"); });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("closed", 10, [] { return !Diff()->menu() || !gtk_widget_get_mapped(Diff()->menu()); });
    CloseRepoWindow();
}
