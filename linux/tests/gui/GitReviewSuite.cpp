// GitReviewSuite.cpp — `./build/minicode-guitest git_review`, the counterpart
// of the Mac's git_review in tests/gui/GitPanelSuite.mm (the review of every
// uncommitted change, ChangeReviewPage.h), by real clicks: the panel's Review
// Changes button opens one tab that lists every change in one scroll in the
// core's order (the staged file first, then the changed ones, then the
// untracked one), each block of f.txt on a line of its own with Stage and
// Discard; Stage on the second block stages only it (git diff --cached holds
// that line alone and the first stays in the work tree); Discard asks,
// Cancel changes nothing, and Discard Change puts the first block back as
// the index has it with the file as it was in the Trash; Undo Discard on the
// page brings it back; Stage File on the untracked file stages it; and a
// change made outside, with the panel hidden, shows on the page by itself.
#include "GitChecks.h"

#include "ChangeReviewPage.h"
#include "GitRun.h"

using namespace gitchecks;

namespace {

std::string R() { return MCGuiScratch() + "/gitreview"; }
int gActions = 0;
std::string gBefore;   // the page's text before an action

ChangeReviewPage* Page() {
    Editor* e = RepoWindow().editor;
    return e ? dynamic_cast<ChangeReviewPage*>(e->currentPage()) : nullptr;
}
bool Ready() { return Page() && Page()->loaded() && Page()->blockCount() > 0; }

// The page's rows as the core lays them out for the repository as it is
// now, read by the same git (a check's own read, never the page's).
std::string CoreText() {
    ChangeReview::Read r = ChangeReview::readUncommitted(
        R(), [](const std::string& dir, const std::vector<std::string>& args, const std::string& in) {
            GitResult g = runGit(dir, args, in.empty() ? nullptr : &in);
            ChangeReview::GitOut o;
            o.ok = g.ok();
            o.out = g.out;
            o.err = g.err;
            return o;
        });
    return QuickDiff::toUtf8(ChangeReview::layout(r.diffs, {}).text);
}

// A press on a row's button, for real: the row scrolled to the top, the
// button on screen, then a click on it.
void Press(const std::string& title, std::function<long()> row) {
    MCGuiStep("scroll to the row of " + title, [row] {
        const long r = row();
        GCHECK(r >= 0);
        if (r >= 0 && Page()) Page()->scrollToRow((size_t)r);
    });
    MCGuiWait(title + " on screen", 10, [title, row] {
        const long r = row();
        return r >= 0 && Page() && MCGuiOnScreen(Page()->buttonWidget(title, (size_t)r));
    });
    MCGuiStep("a click on " + title, [title, row] {
        gActions = Panel()->actionsDone();
        gBefore = Page()->pageText();
        MCGuiClick(Page()->buttonWidget(title, (size_t)row()));
    });
}

}  // namespace

MC_GUI_SUITE(git_review) {
    if (GitPath().empty()) {
        MCGuiStep("no git", [] { MCGuiNote("no git here: the review checks are skipped"); });
        return;
    }
    Shell("a repository; f.txt changed twice, g.txt staged, n.txt new",
          "set -e; mkdir -p '" + R() + "'; cd '" + R() + "'; " + G() +
              " init -q -b main; printf 'a\\nb\\nc\\nd\\ne\\nf\\ng\\nh\\ni\\nj\\nk\\nl\\nm\\n' > f.txt; "
              "printf 'x\\ny\\n' > g.txt; " + G() + " add -A; " + G() + " commit -qm first; "
              "printf 'a\\nB\\nc\\nd\\ne\\nf\\ng\\nh\\ni\\nj\\nk\\nL\\nm\\n' > f.txt; printf 'x\\nY\\n' > g.txt; " +
              G() + " add g.txt; printf 'new\\n' > n.txt");
    MCGuiStep("a Trash of our own", [] { UseScratchTrash(true); });
    OpenRepoWindow(R(), true);
    MCGuiWait("three files changed, and Review Changes offered", 15, [] {
        return Rows({"# Staged changes  1", "S M g.txt", "# Changes  2", "W M f.txt", "W U n.txt"}) &&
               Panel()->reviewButtonShown() && MCGuiOnScreen(Panel()->reviewButton());
    });
    MCGuiStep("a click on Review Changes", [] { MCGuiClick(Panel()->reviewButton()); });
    MCGuiWait("the page lists four blocks", 15, [] { return Ready() && Page()->blockCount() == 4; });
    MCGuiStep("every change in one scroll, in the core's order", [] {
        ChangeReviewPage* p = Page();
        GCHECK_EQ_STR(p->pageText(), CoreText());
        const std::string t = p->pageText();
        const size_t staged = t.find("Staged Changes  1"), changes = t.find("Changes  2");
        const size_t g = t.find("g.txt"), f = t.find("f.txt  +2 -2"), n = t.find("n.txt  (new)");
        GCHECK(staged != std::string::npos && staged < g && g < changes && changes < f && f < n);
        GCHECK_EQ_STR(p->headerDetail(), "3 files, +4 -3");
        // A block's own buttons: Stage and Discard beside its first line, an
        // Unstage in the staged list, nothing on a new file's lines.
        GCHECK((p->buttonsOfRow((size_t)p->rowOfBlock(0)) == std::vector<std::string>{"Unstage"}));
        GCHECK((p->buttonsOfRow((size_t)p->rowOfBlock(1)) == std::vector<std::string>{"Stage", "Discard"}));
        GCHECK((p->buttonsOfRow((size_t)p->rowOfBlock(2)) == std::vector<std::string>{"Stage", "Discard"}));
        GCHECK(p->buttonsOfRow((size_t)p->rowOfBlock(3)).empty());
        GCHECK((p->buttonsOfRow((size_t)p->rowOfFile("n.txt", false)) ==
                std::vector<std::string>{"Side by Side", "Open", "Discard File", "Stage File"}));
        GCHECK(p->rowTint((size_t)p->rowOfBlock(1)) == "removed");
        GCHECK(p->rowText((size_t)p->rowOfBlock(1)) == "b");
        MCGuiSnapshot(GTK_WIDGET(p->widget()), "git-review-page");
    });

    // --------------------------------------------- Stage one block of two
    Press("Stage", [] { return Page()->rowOfBlock(2); });
    MCGuiWait("the second block staged", 15, [] {
        return Panel()->actionsDone() > gActions || Page()->messageText().find("Staged") == 0;
    });
    MCGuiWait("the page shows it", 15, [] {
        return Page()->messageText() == "Staged a change to f.txt." && Page()->pageText() != gBefore &&
               Page()->pageText().find("Staged Changes  2") != std::string::npos;
    });
    MCGuiStep("only that block is in the index", [] {
        const std::string cached = GitText({"diff", "--cached", "--", "f.txt"});
        GCHECK(cached.find("+L") != std::string::npos && cached.find("+B") == std::string::npos);
        const std::string work = GitText({"diff", "--", "f.txt"});
        GCHECK(work.find("+B") != std::string::npos && work.find("+L") == std::string::npos);
        GCHECK_EQ_STR(Page()->pageText(), CoreText());
    });

    // ------------------------------------------ Discard asks, then restores
    // The first block of f.txt, now the only one of it in the Changes list:
    // g.txt's, f.txt's staged one, then this.
    Press("Discard", [] { return Page()->rowOfBlock(2); });
    MCGuiWait("it asks", 10, QuestionUp);
    MCGuiStep("naming the change; Cancel", [] {
        GCHECK(QuestionText().find("Discard this change to f.txt?") != std::string::npos);
        Answer("Cancel");
    });
    MCGuiWait("the question goes", 10, QuestionGone);
    MCGuiStep("nothing changed", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK_EQ_STR(MCGuiReadFile(In("f.txt")), "a\nB\nc\nd\ne\nf\ng\nh\ni\nj\nk\nL\nm\n");
        GCHECK(!Trashed("a\nB\nc\nd\ne\nf\ng\nh\ni\nj\nk\nL\nm\n"));   // (other suites' copies stay)
        GCHECK(Page()->undoButton() && !gtk_widget_get_visible(Page()->undoButton()));
    });
    Press("Discard", [] { return Page()->rowOfBlock(2); });
    MCGuiWait("it asks", 10, QuestionUp);
    MCGuiStep("Discard Change", [] { Answer("Discard Change"); });
    MCGuiWait("that block is gone from the page", 15, [] {
        return Page()->messageText().find("Discarded a change to f.txt") == 0 &&
               Page()->pageText() != gBefore && Page()->rowOfFile("f.txt", false) < 0;
    });
    MCGuiStep("the file is as the index has it, the copy in the Trash, Undo Discard offered", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK_EQ_STR(MCGuiReadFile(In("f.txt")), "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nL\nm\n");
        GCHECK(Trashed("a\nB\nc\nd\ne\nf\ng\nh\ni\nj\nk\nL\nm\n"));
        GCHECK_EQ_STR(Panel()->undoDiscardTitle(), "Undo Discard of f.txt");
        GCHECK(gtk_widget_get_visible(Page()->undoButton()));
    });
    MCGuiWait("Undo Discard on screen", 10, [] { return MCGuiOnScreen(Page()->undoButton()); });
    MCGuiStep("a click on Undo Discard", [] {
        gBefore = Page()->pageText();
        MCGuiClick(Page()->undoButton());
    });
    MCGuiWait("the block is back on the page", 15, [] {
        return Page()->rowOfFile("f.txt", false) >= 0 && Page()->pageText() != gBefore &&
               !gtk_widget_get_visible(Page()->undoButton());
    });
    MCGuiStep("the file has its change again", [] {
        GCHECK_EQ_STR(MCGuiReadFile(In("f.txt")), "a\nB\nc\nd\ne\nf\ng\nh\ni\nj\nk\nL\nm\n");
        GCHECK_EQ_STR(Page()->pageText(), CoreText());
    });

    // ------------------------------------------------------ a whole file
    Press("Stage File", [] { return Page()->rowOfFile("n.txt", false); });
    MCGuiWait("n.txt staged", 15, [] {
        return Page()->messageText() == "Staged n.txt." && Page()->rowOfFile("n.txt", true) >= 0 &&
               Page()->rowOfFile("n.txt", false) < 0;
    });
    MCGuiStep("it is in the index", [] {
        GCHECK(GitText({"diff", "--cached", "--name-only"}).find("n.txt") != std::string::npos);
    });

    // ------------------------------- a change made outside, the panel hidden
    MCGuiStep("the panel hidden (the tree back)", [] { MCGuiKey("ctrl+shift+g"); });
    MCGuiWait("hidden", 10, [] { return !Panel()->active(); });
    Shell("g.txt changed by something else", "cd '" + R() + "' && printf 'x\\nY\\nZ\\n' > g.txt");
    MCGuiWait("the page shows the new line without being asked", 20, [] {
        return Page() && Page()->pageText().find("Z") != std::string::npos;
    });
    MCGuiStep("what the core lays out now", [] { GCHECK_EQ_STR(Page()->pageText(), CoreText()); });
    CloseRepoWindow();
}
