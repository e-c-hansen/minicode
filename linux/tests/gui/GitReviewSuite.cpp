// GitReviewSuite.cpp — `./build/mutible-guitest git_review`, the counterpart
// of the Mac's git_review in tests/gui/GitPanelSuite.mm (the review of every
// uncommitted change, ChangeReviewPage.h), by real clicks: the panel's Review
// Changes button opens one tab that lists every change in one scroll in the
// core's order (the staged file first, then the changed ones, then the
// untracked one), each block of f.txt on a line of its own with Stage and
// Discard; Stage on the second block stages only it (git diff --cached holds
// that line alone and the first stays in the work tree); Discard asks,
// Cancel changes nothing, and Discard Change puts the first block back as
// the index has it with the file as it was in the Trash; Undo Discard on the
// page brings it back; Stage File on the untracked file stages it; a change
// made outside, with the panel hidden, shows on the page by itself; and
// Stage on a block in the middle of a page taller than the pane leaves the
// file's next block where the staged one was on screen (the Mac's #217).
//
// Then commits, in a repository of their own: a single click on a commit in
// the graph shows its page in the preview tab (title, subject, byline, body,
// its files without Stage or Discard), a click on another replaces it, a
// double-click or Return keeps it, a click on a kept one shows that tab, and
// a commit with no files says so.
//
// And F7 and Shift+F7 on the review page (the blocks of two files in order,
// wrapping), the right click on selected lines (Stage, Unstage and Discard
// Selected Lines, none with nothing selected) and on a conflict's rows (Keep
// Current), in repositories of their own.
#include "GitChecks.h"

#include "ChangeReviewPage.h"
#include "Document.h"
#include "GitRun.h"

using namespace gitchecks;

namespace {

std::string R() { return MCGuiScratch() + "/gitreview"; }
int gActions = 0;
std::string gBefore;   // the page's text before an action
double gPressedY = 0;  // where the pressed row was on screen

ChangeReviewPage* Page() {
    Editor* e = RepoWindow().editor;
    return e ? dynamic_cast<ChangeReviewPage*>(e->currentPage()) : nullptr;
}
bool Ready() { return Page() && Page()->loaded() && Page()->blockCount() > 0; }
// The row of the first block after a row, or -1.
long NextBlockAfter(long row) {
    for (size_t b = 0; Page() && row >= 0 && b < Page()->blockCount(); b++)
        if (Page()->rowOfBlock(b) > row) return Page()->rowOfBlock(b);
    return -1;
}

// The page's rows as the core lays them out for the repository as it is
// now, read by the same git (a check's own read, never the page's).
std::string CoreText(const std::string& dir = R()) {
    ChangeReview::Read r = ChangeReview::readUncommitted(
        dir, [](const std::string& dir, const std::vector<std::string>& args, const std::string& in) {
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

// ------------------------------------------------------------ commit pages
std::string C() { return MCGuiScratch() + "/gitcommits"; }
std::string gShort[3];   // the short hashes: root, second, empty
Editor* Ed() { return RepoWindow().editor; }
std::vector<Document*> CommitTabs() {
    std::vector<Document*> out;
    if (Editor* e = Ed())
        for (Document* d : e->documents())
            if (d->editorPage())
                if (auto* r = dynamic_cast<ChangeReviewPage*>(d->editorPage().get()))
                    if (r->isCommit()) out.push_back(d);
    return out;
}
// The page on show, when it is a commit's.
ChangeReviewPage* CommitPage() {
    ChangeReviewPage* p = Page();
    return p && p->isCommit() && p->loaded() ? p : nullptr;
}
// The graph row whose line has this subject, or -1.
int GraphRowOf(const std::string& subject) {
    const std::vector<std::string> g = Panel() ? Panel()->graphDescriptions() : std::vector<std::string>();
    for (size_t i = 0; i < g.size(); i++)
        if (g[i].find(" " + subject) != std::string::npos) return (int)i;
    return -1;
}
GtkWidget* GraphRowWidget(const std::string& subject) {
    const int row = GraphRowOf(subject);
    GtkWidget* w = row >= 0 && Panel() ? Panel()->graphRowWidget(row) : nullptr;
    return MCGuiOnScreen(w) ? w : nullptr;
}
// A real click on a commit's row in the graph (twice: a double-click).
void ClickCommit(const std::string& subject, bool twice = false) {
    WaitOnScreen("the message box", [] { return Panel()->messageView(); });
    MCGuiStep("the pointer off the rows", [] { MCGuiMoveTo(Panel()->messageView()); });
    MCGuiPause(0.6);   // time is the condition: the last click is no part of this one's double-click
    MCGuiWait(subject + " in the graph, on screen", 15, [subject] { return GraphRowWidget(subject) != nullptr; });
    MCGuiStep(std::string(twice ? "a double-click on " : "a click on ") + subject, [subject, twice] {
        GtkWidget* w = GraphRowWidget(subject);
        GCHECK(w != nullptr);
        if (!w) return;
        if (twice) MCGuiDoubleClick(w);
        else MCGuiClick(w);
    });
}
// A commit page of this title is on show.
bool OnShow(const std::string& title) {
    ChangeReviewPage* p = CommitPage();
    return p && p->title() == title;
}

// ------------------------------------------- a side by side's recipe
// What a side by side opened from the page should carry and show.
struct SideWant {
    std::string leftRev, rightRev;   // "" is the disk, ":" the index
    std::string leftText, rightText;
};
Document* gPageDoc = nullptr;   // the page's tab, to come back to
Document* gCompareDoc = nullptr;
Document* gRemadeDoc = nullptr;

std::string Trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// A click on `file`'s Side by Side; its tab carries the recipe (both paths
// the file's, these revs), and the tab made again from that recipe, off the
// main thread, shows the same texts under the same labels. `want` is read
// when the check runs, for revs that are only known then.
void SideCheck(const std::string& file, bool staged, std::function<SideWant()> want) {
    MCGuiStep("the page's tab noted", [] {
        gPageDoc = Ed()->active();
        gCompareDoc = gRemadeDoc = nullptr;
    });
    Press("Side by Side", [file, staged] { return Page()->rowOfFile(file, staged); });
    MCGuiWait("the comparison of " + file + " opens", 20, [] { return Ed()->isComparison(); });
    MCGuiStep("it carries its recipe, and the recipe makes it again", [file, want] {
        gCompareDoc = Ed()->active();
        const SideWant w = want();
        const CompareData* c = gCompareDoc ? gCompareDoc->compare().get() : nullptr;
        if (!GCHECK(c != nullptr)) return;
        GCHECK(c->spec.left.path.size() >= file.size() &&
               c->spec.left.path.compare(c->spec.left.path.size() - file.size(), file.size(), file) == 0);
        GCHECK_EQ_STR(c->spec.left.path, c->spec.right.path);
        GCHECK_EQ_STR(c->spec.left.rev, w.leftRev);
        GCHECK_EQ_STR(c->spec.right.rev, w.rightRev);
        GCHECK_EQ_STR(c->spec.leftText, w.leftText);
        GCHECK_EQ_STR(c->spec.rightText, w.rightText);
        TabSessions::PageTab page;
        GCHECK(Editor::comparisonPage(gCompareDoc, &page));
        GCHECK_EQ_STR(page.kind, "compare");
        GCHECK_EQ_STR(page.left.rev, w.leftRev);
        GCHECK_EQ_STR(page.right.rev, w.rightRev);
        GCHECK_EQ_STR(page.left.label, c->spec.leftLabel);
        GCHECK_EQ_STR(page.right.label, c->spec.rightLabel);
        Ed()->remakeComparison(page, Ed()->tabCount(), false, [](Document* d) { gRemadeDoc = d; });
    });
    MCGuiWait("the tab is made again", 20, [] { return gRemadeDoc != nullptr; });
    MCGuiStep("with the same texts, labels and title, and not on show", [want] {
        const SideWant w = want();
        const CompareData* a = gCompareDoc->compare().get();
        const CompareData* b = gRemadeDoc->compare().get();
        if (!GCHECK(b != nullptr)) return;
        GCHECK(Ed()->active() == gCompareDoc);
        GCHECK(gRemadeDoc != gCompareDoc);
        GCHECK_EQ_STR(b->spec.leftText, w.leftText);
        GCHECK_EQ_STR(b->spec.rightText, w.rightText);
        GCHECK_EQ_STR(b->spec.leftLabel, a->spec.leftLabel);
        GCHECK_EQ_STR(b->spec.rightLabel, a->spec.rightLabel);
        GCHECK_EQ_STR(b->spec.title, a->spec.title);
        GCHECK_EQ_STR(b->spec.fileName, a->spec.fileName);
        GCHECK_EQ_STR(b->spec.left.rev, a->spec.left.rev);
        GCHECK_EQ_STR(b->spec.right.rev, a->spec.right.rev);
        GCHECK(b->changedRows == a->changedRows && b->rows.size() == a->rows.size());
        Ed()->closeTab(gRemadeDoc);
        Ed()->closeTab(gCompareDoc);
        Ed()->activate(gPageDoc);
    });
    MCGuiWait("the page is on show again", 10, [] { return Page() != nullptr; });
}

// ------------------------------------------------- F7 and the right click
std::string K() { return MCGuiScratch() + "/gitreviewkeys"; }
// The open context menu: a mapped popover under the window showing `label`.
GtkWidget* PopoverWith(GtkWidget* root, const std::string& label) {
    if (!root || !gtk_widget_get_mapped(root)) return nullptr;
    if (GTK_IS_POPOVER(root) && MCGuiFindLabel(root, label)) return root;
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = PopoverWith(c, label)) return hit;
    return nullptr;
}
GtkWidget* Menu(const std::string& label) { return PopoverWith(GTK_WIDGET(RepoWindow().window), label); }
// A real right click on a row's text.
void RightClickRow(const std::string& what, std::function<long()> row) {
    MCGuiStep("the row of " + what + " on screen", [row] {
        const long r = row();
        GCHECK(r >= 0);
        if (r >= 0 && Page()) Page()->scrollToRow((size_t)r);
    });
    MCGuiWait(what + " in view", 10, [row] {
        const long r = row();
        return r >= 0 && Page() && Page()->rowScreenY((size_t)r) >= 0 &&
               Page()->rowScreenY((size_t)r) < Page()->paneHeight() - 20;
    });
    // Time is the condition: a click soon after another near it is a
    // double-click to GTK, which opens no menu.
    MCGuiPause(0.6);
    MCGuiStep("the keyboard in the text view, as after a drag over the lines", [] {
        MCGuiFocusKeyWindow();   // a question just answered left the window without the keyboard
        gtk_widget_grab_focus(GTK_WIDGET(Page()->textView()));
    });
    MCGuiWait("the text view has it", 10, [] { return Focused(GTK_WIDGET(Page()->textView())); });
    MCGuiStep("the pointer on it", [row] {
        MCGuiMoveTo(GTK_WIDGET(Page()->textView()), gtk_widget_get_width(GTK_WIDGET(Page()->textView())) - 60,
                    Page()->rowScreenY((size_t)row()) + 8);   // past the line: its buttons sit before its text
    });
    MCGuiStep("a right click on " + what, [row] {
        gBefore = Page()->pageText();
        MCGuiClick(GTK_WIDGET(Page()->textView()), gtk_widget_get_width(GTK_WIDGET(Page()->textView())) - 60,
                   Page()->rowScreenY((size_t)row()) + 8, 3);
    });
}
// The menu is closed again, for the next click.
void CloseMenu() {
    MCGuiStep("Escape closes the menu", [] { MCGuiKey("escape"); });
    MCGuiWait("the menu closed", 10, [] { return Menu("Copy") == nullptr; });
}
// F7 or Shift+F7 lands on block `b`: its row on screen, the caret on it.
void StepTo(const std::string& chord, size_t b) {
    MCGuiStep(chord, [chord] { MCGuiKey(chord); });
    MCGuiWait(chord + " lands on block " + std::to_string(b), 15, [b] {
        ChangeReviewPage* p = Page();
        const long row = p ? p->rowOfBlock(b) : -1;
        return row >= 0 && p->currentBlockRow() == row && p->caretRow() == row;
    });
    MCGuiStep("block " + std::to_string(b) + "'s row on screen", [b] {
        ChangeReviewPage* p = Page();
        const long row = p->rowOfBlock(b);
        const double y = p->rowScreenY((size_t)row);
        MCGuiNote("block %zu: row %ld at %.0f of %.0f, caret on row %ld", b, row, y, p->paneHeight(),
                  p->caretRow());
        GCHECK(y >= 0 && y < p->paneHeight());
    });
}
// ------------------------------------------------------ animated GIFs
// A GIF89a of one 1 by 1 pixel per frame, each frame one of two palette
// colors, 0.1 s a frame, looping; two frames, or one with no loop block.
std::string Gif(unsigned char r0, unsigned char g0, unsigned char b0, unsigned char r1, unsigned char g1,
                unsigned char b1, int frames) {
    std::string g = "GIF89a";
    g += std::string("\x01\x00\x01\x00\x80\x00\x00", 7);
    g += std::string({(char)r0, (char)g0, (char)b0, (char)r1, (char)g1, (char)b1});
    if (frames > 1) g += std::string("\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00", 19);
    for (int f = 0; f < frames; f++) {
        g += std::string("\x21\xF9\x04\x04\x0A\x00\x00\x00", 8);
        g += std::string("\x2C\x00\x00\x00\x00\x01\x00\x01\x00\x00", 10);
        // LZW, minimum code size 2: clear (4), the pixel, end (5), three bits each.
        g += f % 2 ? std::string("\x02\x02\x4C\x01\x00", 5) : std::string("\x02\x02\x44\x01\x00", 5);
    }
    g += '\x3B';
    return g;
}
std::string Gc() { return MCGuiScratch() + "/gitgifs"; }
int gFrames0 = 0;   // After's frames when the animation was last looked at

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

    // ------------------------------- a side by side keeps its recipe
    SideCheck("g.txt", true, [] { return SideWant{"HEAD", ":", "x\ny\n", "x\nY\n"}; });
    SideCheck("f.txt", false, [] {
        return SideWant{":", "", "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\n",
                        "a\nB\nc\nd\ne\nf\ng\nh\ni\nj\nk\nL\nm\n"};
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

    // ------------------------------- the place after Stage (the Mac's #217)
    // m1.txt's one long block, z2.txt's two blocks, then zz3.txt's long
    // block below: Stage on z2.txt's first block puts its second block where
    // the first was on screen, though a staged list now stands above
    // everything (the walker's page jumped from summary.py up to models.py).
    Shell("all put back; m1.txt, z2.txt and zz3.txt committed, then changed",
          "set -e; cd '" + R() + "'; " + G() + " reset -q; " + G() + " checkout -q -- .; rm -f n.txt; "
          "seq 1 150 | sed 's/^/m /' > m1.txt; seq 1 80 | sed 's/^/z /' > z2.txt; "
          "seq 1 100 | sed 's/^/zz /' > zz3.txt; " + G() + " add m1.txt z2.txt zz3.txt; " + G() +
              " commit -qm three; seq 1 150 | sed 's/^/M /' > m1.txt; "
              "seq 1 80 | sed 's/^/z /' | sed -e 's/^z 2$/z TWO/' -e 's/^z 30$/z THIRTY/' > z2.txt; "
              "seq 1 100 | sed 's/^/ZZ /' > zz3.txt");
    MCGuiWait("three files, four blocks", 30, [] {
        ChangeReviewPage* p = Page();
        return p && p->blockCount() == 4 && p->fileCount() == 3 && p->rowOfFile("z2.txt", false) >= 0;
    });
    // GTK measures a new text's lines in idle slices: until then the page
    // is as tall as the lines measured so far.
    MCGuiWait("laid out, taller than twice the pane", 15, [] {
        ChangeReviewPage* p = Page();
        GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(p->textView()));
        return p->paneHeight() > 0 && gtk_adjustment_get_upper(adj) > 2 * p->paneHeight();
    });
    MCGuiStep("z2.txt's first block in the middle of the pane", [] {
        ChangeReviewPage* p = Page();
        const long block = p->rowOfBlock(1);   // m1.txt's is the first
        GCHECK(block > p->rowOfFile("z2.txt", false));
        GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(p->textView()));
        MCGuiNote("page %.0f tall, pane %.0f", gtk_adjustment_get_upper(adj), p->paneHeight());
        p->scrollRowTo((size_t)block, std::floor(p->paneHeight() / 2));
    });
    MCGuiWait("there, its Stage on screen", 10, [] {
        ChangeReviewPage* p = Page();
        const size_t block = (size_t)p->rowOfBlock(1);
        return std::abs(p->rowScreenY(block) - std::floor(p->paneHeight() / 2)) <= 1 &&
               MCGuiOnScreen(p->buttonWidget("Stage", block));
    });
    MCGuiStep("Stage on it", [] {
        ChangeReviewPage* p = Page();
        const size_t block = (size_t)p->rowOfBlock(1);
        gPressedY = p->rowScreenY(block);
        MCGuiNote("pressed on z2.txt's first block at %.0f", gPressedY);
        GCHECK(gPressedY > 0 && gPressedY < p->paneHeight());
        gBefore = p->pageText();
        MCGuiClick(p->buttonWidget("Stage", block));
    });
    MCGuiWait("staged, and shown", 15, [] {
        return Page()->messageText() == "Staged a change to z2.txt." && Page()->pageText() != gBefore &&
               Page()->rowOfFile("z2.txt", true) >= 0;
    });
    MCGuiWait("z2.txt's other block where the staged one was", 10, [] {
        const long next = NextBlockAfter(Page()->rowOfFile("z2.txt", false));
        return next >= 0 && std::abs(Page()->rowScreenY((size_t)next) - gPressedY) <= 4;
    });
    MCGuiStep("there, z2.txt still in sight, not the staged list at the top", [] {
        ChangeReviewPage* p = Page();
        const long header = p->rowOfFile("z2.txt", false);
        const long next = NextBlockAfter(header);
        const double y = next >= 0 ? p->rowScreenY((size_t)next) : -1;
        MCGuiNote("after Stage: z2.txt's next block at %.0f (pressed at %.0f), first row on show %ld", y,
                  gPressedY, p->firstVisibleRow());
        GCHECK(header >= 0 && next > header);
        GCHECK_EQ_STR(p->rowText((size_t)next), "z 30");
        GCHECK(std::abs(y - gPressedY) <= 4);
        GCHECK(p->firstVisibleRow() > p->rowOfFile("m1.txt", false));
    });
    CloseRepoWindow();

    // ================================== F7 and the right click on lines
    // f.txt changed in two places, g.txt in one (two added lines).
    Shell("a repository; f.txt changed twice, g.txt once (two lines added)",
          "set -e; mkdir -p '" + K() + "'; cd '" + K() + "'; " + G() +
              " init -q -b main; printf 'a\\nb\\nc\\nd\\ne\\nf\\ng\\nh\\ni\\nj\\nk\\nl\\nm\\n' > f.txt; "
              "printf 'x\\ny\\nz\\n' > g.txt; " + G() + " add -A; " + G() + " commit -qm first; "
              "printf 'a\\nB\\nc\\nd\\ne\\nf\\ng\\nh\\ni\\nj\\nk\\nL\\nm\\n' > f.txt; "
              "printf 'x\\nP\\nQ\\ny\\nz\\n' > g.txt");
    MCGuiStep("a Trash of our own", [] { UseScratchTrash(true); });
    OpenRepoWindow(K(), true);
    MCGuiWait("Review Changes offered", 15, [] {
        return Panel()->reviewButtonShown() && MCGuiOnScreen(Panel()->reviewButton());
    });
    MCGuiStep("a click on Review Changes", [] { MCGuiClick(Panel()->reviewButton()); });
    MCGuiWait("the page lists three blocks", 15, [] { return Ready() && Page()->blockCount() == 3; });
    MCGuiWait("the text view on screen", 10, [] { return MCGuiOnScreen(GTK_WIDGET(Page()->textView())); });
    MCGuiStep("the text view has the keyboard", [] { gtk_widget_grab_focus(GTK_WIDGET(Page()->textView())); });
    MCGuiWait("it has it", 10, [] { return Focused(GTK_WIDGET(Page()->textView())); });
    StepTo("f7", 0);
    StepTo("f7", 1);
    StepTo("f7", 2);
    StepTo("f7", 0);         // wraps to the first
    StepTo("shift+f7", 2);   // and back to the last
    StepTo("shift+f7", 1);

    // ---- Stage Selected Lines on one added line of a two-line change
    MCGuiStep("P (the first added line) selected", [] {
        GCHECK_EQ_STR(Page()->rowText((size_t)Page()->rowOfBlock(2)), "P");
        Page()->selectRowText((size_t)Page()->rowOfBlock(2));
    });
    RightClickRow("the selected line", [] { return Page()->rowOfBlock(2); });
    MCGuiWait("the menu offers the line items", 10, [] { return Menu("Stage Selected Lines") != nullptr; });
    MCGuiStep("Stage and Discard (not Unstage), with Copy", [] {
        GtkWidget* m = Menu("Stage Selected Lines");
        GCHECK(MenuHas(m, "Discard Selected Lines…") && MenuHas(m, "Copy"));
        GCHECK(!MenuHas(m, "Unstage Selected Lines"));
        MCGuiSnapshot(GTK_WIDGET(RepoWindow().window), "git-review-lines-menu");
    });
    MCGuiStep("a click on Stage Selected Lines", [] {
        MCGuiClick(MenuItem(Menu("Stage Selected Lines"), "Stage Selected Lines"));
    });
    MCGuiWait("staged, and shown", 15, [] {
        return Page()->messageText() == "Staged the selected lines of g.txt." && Page()->pageText() != gBefore &&
               Page()->rowOfFile("g.txt", true) >= 0;
    });
    MCGuiWait("the page lists the index as it is", 15, [] { return Page()->pageText() == CoreText(K()); });
    MCGuiStep("only that line is in the index", [] {
        const std::string cached = GitText({"diff", "--cached", "--", "g.txt"});
        GCHECK(cached.find("+P") != std::string::npos && cached.find("+Q") == std::string::npos);
        GCHECK_EQ_STR(GitText({"diff", "--cached", "--name-only"}), "g.txt\n");
        const std::string work = GitText({"diff", "--", "g.txt"});
        GCHECK(work.find("+Q") != std::string::npos && work.find("+P") == std::string::npos);
        GCHECK_EQ_STR(Page()->pageText(), CoreText(K()));
    });

    // ---- Unstage Selected Lines in the staged list
    MCGuiStep("the staged P selected", [] {
        const long header = Page()->rowOfFile("g.txt", true);
        const long row = NextBlockAfter(header);
        GCHECK(row > header);
        if (row > header) Page()->selectRowText((size_t)row);
    });
    RightClickRow("the staged line", [] { return NextBlockAfter(Page()->rowOfFile("g.txt", true)); });
    MCGuiWait("the menu offers Unstage Selected Lines", 10, [] { return Menu("Unstage Selected Lines") != nullptr; });
    MCGuiStep("Unstage alone; a click on it", [] {
        GtkWidget* m = Menu("Unstage Selected Lines");
        GCHECK(!MenuHas(m, "Stage Selected Lines") && !MenuHas(m, "Discard Selected Lines…"));
        MCGuiClick(MenuItem(m, "Unstage Selected Lines"));
    });
    MCGuiWait("the index is empty again", 15, [] {
        return Page()->messageText().find("Unstaged") == 0 && Page()->rowOfFile("g.txt", true) < 0 &&
               Page()->rowOfFile("g.txt", false) >= 0;
    });
    MCGuiStep("nothing cached, the work tree as it was", [] {
        MCGuiNote("message: %s", Page()->messageText().c_str());
        GCHECK_EQ_STR(GitText({"diff", "--cached"}), "");
        GCHECK_EQ_STR(MCGuiReadFile(In("g.txt")), "x\nP\nQ\ny\nz\n");
    });

    // ---- Discard Selected Lines... asks, then takes only that line
    for (int pass = 0; pass < 2; pass++) {
        MCGuiStep("g.txt's first added line selected", [] {
            const long row = NextBlockAfter(Page()->rowOfFile("g.txt", false));
            GCHECK(row >= 0);
            if (row >= 0) {
                GCHECK_EQ_STR(Page()->rowText((size_t)row), "P");
                Page()->selectRowText((size_t)row);
            }
        });
        RightClickRow("the line to discard", [] { return NextBlockAfter(Page()->rowOfFile("g.txt", false)); });
        MCGuiWait("the menu", 10, [] { return Menu("Discard Selected Lines…") != nullptr; });
        MCGuiStep("a click on Discard Selected Lines...", [] {
            MCGuiClick(MenuItem(Menu("Discard Selected Lines…"), "Discard Selected Lines…"));
        });
        MCGuiWait("it asks", 10, QuestionUp);
        if (pass == 0) {
            MCGuiStep("naming the file; Cancel", [] {
                MCGuiNote("question: %s", QuestionText().c_str());
                GCHECK(QuestionText().find("g.txt") != std::string::npos);
                Answer("Cancel");
            });
            MCGuiWait("the question goes", 10, QuestionGone);
            MCGuiStep("nothing changed", [] {
                MCGuiSetKeyWindow(nullptr);
                GCHECK_EQ_STR(MCGuiReadFile(In("g.txt")), "x\nP\nQ\ny\nz\n");
            });
        }
    }
    MCGuiStep("Discard Lines", [] { Answer("Discard Lines"); });
    MCGuiWait("discarded", 15, [] {
        return Page()->messageText().find("Discarded") == 0 && MCGuiReadFile(In("g.txt")) == "x\nQ\ny\nz\n";
    });
    MCGuiStep("only that line went; Undo Discard offered", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiNote("message: %s", Page()->messageText().c_str());
        GCHECK_EQ_STR(MCGuiReadFile(In("g.txt")), "x\nQ\ny\nz\n");
        GCHECK(gtk_widget_get_visible(Page()->undoButton()));
    });
    MCGuiWait("Undo Discard on screen", 10, [] { return MCGuiOnScreen(Page()->undoButton()); });
    MCGuiStep("a click on Undo Discard", [] { MCGuiClick(Page()->undoButton()); });
    MCGuiWait("the line is back", 15, [] { return MCGuiReadFile(In("g.txt")) == "x\nP\nQ\ny\nz\n"; });

    // ---- the right click with nothing selected: the text view's own menu
    MCGuiStep("the caret on g.txt's block, nothing selected", [] {
        Page()->stepBlock(true);
        GCHECK(Page()->caretRow() >= 0);
    });
    RightClickRow("g.txt's block, nothing selected", [] { return Page()->rowOfBlock(2); });
    MCGuiWait("the menu, with Copy", 10, [] { return Menu("Copy") != nullptr; });
    MCGuiStep("none of the line items", [] {
        GtkWidget* m = Menu("Copy");
        std::string all;
        for (const std::string& l : MenuLabels(m)) all += l + "|";
        MCGuiNote("menu: %s", all.c_str());
        GCHECK(!MenuHas(m, "Stage Selected Lines") && !MenuHas(m, "Discard Selected Lines…") &&
               !MenuHas(m, "Unstage Selected Lines"));
    });
    CloseMenu();

    CloseRepoWindow();
    Shell("the repository goes", "rm -rf '" + K() + "'");

    // ---- a conflict's rows
    Shell("a repository whose merge stops on a conflict in c.txt",
          "set -e; mkdir -p '" + K() + "'; cd '" + K() + "'; " + G() + " init -q -b main; " + G() +
              " config merge.conflictStyle diff3; printf '1\\n2\\n3\\n4\\n5\\n' > c.txt; " + G() +
              " add -A; " + G() + " commit -qm base; " + G() + " checkout -qb topic; "
              "sed -i 's/^2$/two topic/' c.txt; " + G() + " commit -qam topic; " + G() + " checkout -q main; "
              "sed -i 's/^2$/two main/' c.txt; " + G() + " commit -qam main; " + G() +
              " merge -q topic >/dev/null 2>&1 || true; grep -q '^<<<<<<< HEAD' c.txt");
    OpenRepoWindow(K(), true);
    MCGuiWait("Review Changes offered", 15, [] {
        return Panel()->reviewButtonShown() && MCGuiOnScreen(Panel()->reviewButton());
    });
    MCGuiStep("a click on Review Changes", [] { MCGuiClick(Panel()->reviewButton()); });
    MCGuiWait("c.txt's conflict on the page", 15, [] {
        return Page() && Page()->loaded() && Page()->rowOfFile("c.txt", false) >= 0 && Page()->blockCount() > 0;
    });
    RightClickRow("the conflict's first row", [] { return NextBlockAfter(Page()->rowOfFile("c.txt", false)); });
    MCGuiWait("the menu offers the three choices", 10, [] { return Menu("Keep Current") != nullptr; });
    MCGuiStep("Keep Current, Keep Incoming and Keep Both; a click on Keep Current", [] {
        GtkWidget* m = Menu("Keep Current");
        GCHECK(MenuHas(m, "Keep Incoming") && MenuHas(m, "Keep Both") && MenuHas(m, "Copy"));
        MCGuiClick(MenuItem(m, "Keep Current"));
    });
    MCGuiWait("c.txt resolved to the current side", 15, [] {
        return MCGuiReadFile(In("c.txt")) == "1\ntwo main\n3\n4\n5\n";
    });
    MCGuiStep("no conflict markers", [] {
        MCGuiNote("c.txt: %s; message: %s", MCGuiReadFile(In("c.txt")).c_str(), Page()->messageText().c_str());
        GCHECK(MCGuiReadFile(In("c.txt")).find("<<<<<<<") == std::string::npos);
    });
    CloseRepoWindow();
    Shell("the repository goes", "rm -rf '" + K() + "'");
    // ======================================================= animated GIFs
    // The page plays an animated GIF's frames, in its own repository: a.gif
    // (two frames) committed and changed to another two-frame GIF; s.gif of
    // one frame, changed to another. Before and After of a.gif step; the
    // one-frame ones stay still with no timer; closing the page stops them.
    MCGuiStep("GIFs, and the loader's own word on which animate", [] {
        MCGuiWriteFile(Gc() + "/a.gif", Gif(255, 0, 0, 0, 255, 0, 2));
        MCGuiWriteFile(Gc() + "/s.gif", Gif(255, 0, 0, 0, 255, 0, 1));
        MCGuiWriteFile(Gc() + "/new-a.gif", Gif(0, 0, 255, 255, 255, 0, 2));
        MCGuiWriteFile(Gc() + "/new-s.gif", Gif(0, 0, 255, 255, 255, 0, 1));
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        for (const char* n : {"a.gif", "new-a.gif", "s.gif", "new-s.gif"}) {
            GdkPixbufAnimation* a = gdk_pixbuf_animation_new_from_file((Gc() + "/" + n).c_str(), nullptr);
            const bool animated = a && !gdk_pixbuf_animation_is_static_image(a);
            const bool want = std::string(n).find("-a.") != std::string::npos || std::string(n) == "a.gif";
            GCHECK_EQ_STR(std::string(n) + (animated ? " animates" : " is still"),
                          std::string(n) + (want ? " animates" : " is still"));
            if (a) g_object_unref(a);
        }
        G_GNUC_END_IGNORE_DEPRECATIONS
    });
    Shell("committed; both GIFs changed to others",
          "set -e; cd '" + Gc() + "'; " + G() + " init -q -b main; " + G() + " add a.gif s.gif; " + G() +
              " commit -qm first; mv new-a.gif a.gif; mv new-s.gif s.gif");
    MCGuiStep("a Trash of our own", [] { UseScratchTrash(true); });
    OpenRepoWindow(Gc(), true);
    MCGuiWait("Review Changes offered", 20, [] {
        return Panel()->reviewButtonShown() && MCGuiOnScreen(Panel()->reviewButton());
    });
    MCGuiStep("a click on Review Changes", [] { MCGuiClick(Panel()->reviewButton()); });
    MCGuiWait("four pictures, Before and After of each", 20, [] {
        return Page() && Page()->loaded() && Page()->imageLabels().size() == 4;
    });
    MCGuiWait("a.gif's After steps to another frame", 20, [] {
        const std::vector<int> f = Page() ? Page()->imageFrames() : std::vector<int>();
        return f.size() == 4 && f[1] >= 1;
    });
    MCGuiStep("a.gif's two pictures play, s.gif's two are still", [] {
        const std::vector<int> f = Page()->imageFrames();
        MCGuiNote("frames %d %d %d %d, timers %d", f[0], f[1], f[2], f[3], ChangeReviewPage::runningAnimations());
        GCHECK(f[0] >= 0 && f[1] >= 1);
        GCHECK_EQ_STR(std::to_string(f[2]) + " " + std::to_string(f[3]), "-1 -1");
        GCHECK(Page()->imageLabels()[1].rfind("After:", 0) == 0);
        GCHECK(Page()->imageLabels()[3].rfind("After:", 0) == 0);
        GCHECK_EQ_STR(std::to_string(ChangeReviewPage::runningAnimations()), "2");
        gFrames0 = f[1];
    });
    MCGuiWait("it goes on stepping", 20, [] { return Page()->imageFrames()[1] >= gFrames0 + 3; });
    MCGuiStep("the page closes", [] { Ed()->closeAllTabs(); });
    MCGuiWait("no tabs", 10, [] { return Ed()->tabCount() == 0; });
    MCGuiPause(0.5);   // a negative check: time for a stray timer to fire
    MCGuiStep("no frame timer runs", [] {
        GCHECK_EQ_STR(std::to_string(ChangeReviewPage::runningAnimations()), "0");
    });
    Shell("the repository goes", "rm -rf '" + Gc() + "'");
    CloseRepoWindow();

    // ============================================================ commits
    // A click on a commit in the graph opens it as a page (the Mac's
    // initForCommitsWithRoot:): single click in the preview tab, replaced by
    // the next; a double-click or Return keeps it; a kept one is shown again.
    Shell("a repository: a root commit, one with a body (a file changed, one added), an empty one",
          "set -e; mkdir -p '" + C() + "'; cd '" + C() + "'; " + G() +
              " init -q -b main; printf 'one\\n' > a.txt; " + G() + " add -A; " + G() +
              " commit -qm 'Root commit'; printf 'one\\ntwo\\n' > a.txt; printf 'bee\\n' > b.txt; " + G() +
              " add -A; " + G() + " commit -q -m 'Second commit' -m 'The body of it.'; " + G() +
              " commit -q --allow-empty -m 'Nothing here'");
    MCGuiStep("a Trash of our own", [] { UseScratchTrash(true); });
    OpenRepoWindow(C(), true);
    MCGuiWait("the graph lists the three commits", 20, [] {
        return GraphRowOf("Root commit") >= 0 && GraphRowOf("Second commit") >= 0 && GraphRowOf("Nothing here") >= 0;
    });
    MCGuiStep("their short hashes, from git", [] {
        for (int i = 0; i < 3; i++) {
            const std::string rev = i == 0 ? "HEAD~2" : i == 1 ? "HEAD~1" : "HEAD";
            gShort[i] = GitText({"rev-parse", "--short=7", rev});
            while (!gShort[i].empty() && gShort[i].back() == '\n') gShort[i].pop_back();
            GCHECK_EQ_STR(std::to_string(gShort[i].size()), "7");
        }
        Ed()->closeAllTabs();
    });

    ClickCommit("Second commit");
    MCGuiWait("its page, in the preview tab", 20, [] {
        return OnShow(gShort[1] + " Second commit") && Ed()->previewTab() != nullptr && CommitTabs().size() == 1;
    });
    MCGuiWait("both files listed", 15, [] { return CommitPage() && CommitPage()->fileCount() == 2; });
    MCGuiStep("the subject, the byline, the body, the files, and no Stage or Discard", [] {
        ChangeReviewPage* p = CommitPage();
        if (!GCHECK(p != nullptr)) return;
        GCHECK_EQ_STR(p->headerTitle(), "Second commit");
        std::string author = GitText({"log", "-1", "--format=%an", "HEAD~1"});
        while (!author.empty() && author.back() == '\n') author.pop_back();
        const std::string by = p->headerDetail();
        MCGuiNote("byline: %s (author %s)", by.c_str(), author.c_str());
        GCHECK(by.find(gShort[1]) != std::string::npos);
        GCHECK(!author.empty() && by.find(author) != std::string::npos);
        GCHECK(by.find("2 files") != std::string::npos);
        GCHECK_EQ_STR(p->bodyText(), "The body of it.");
        GCHECK(p->shows("a.txt") && p->shows("b.txt"));
        GCHECK(p->rowOfFile("a.txt", false) >= 0 && p->rowOfFile("b.txt", false) >= 0);
        for (const char* f : {"a.txt", "b.txt"})
            for (const std::string& b : p->buttonsOfRow((size_t)p->rowOfFile(f, false)))
                GCHECK(b.find("Stage") == std::string::npos && b.find("Discard") == std::string::npos);
        for (size_t b = 0; b < p->blockCount(); b++)
            GCHECK(p->buttonsOfRow((size_t)p->rowOfBlock(b)).empty());
        GCHECK_EQ_STR(std::to_string(Ed()->tabCount()), "1");
    });
    SideCheck("a.txt", false, [] {
        // A commit against its first parent.
        return SideWant{Trimmed(GitText({"rev-parse", "HEAD~2"})), Trimmed(GitText({"rev-parse", "HEAD~1"})),
                        "one\n", "one\ntwo\n"};
    });

    // ------------------------------------- the root commit replaces it
    ClickCommit("Root commit");
    MCGuiWait("the root commit's page, 1 file, in the one preview tab", 20, [] {
        return OnShow(gShort[0] + " Root commit") && CommitPage()->fileCount() == 1 && Ed()->tabCount() == 1 &&
               Ed()->previewTab() != nullptr;
    });
    MCGuiStep("one commit tab", [] { GCHECK_EQ_STR(std::to_string(CommitTabs().size()), "1"); });
    SideCheck("a.txt", false, [] {
        // The root commit has no parent: its own hash and "^", a side git lacks.
        return SideWant{Trimmed(GitText({"rev-parse", "HEAD~2"})) + "^", Trimmed(GitText({"rev-parse", "HEAD~2"})),
                        "", "one\n"};
    });

    // ---------------------------------------- a double-click keeps a page
    ClickCommit("Second commit", true);
    MCGuiWait("the second commit's page, kept (not the preview)", 20, [] {
        return OnShow(gShort[1] + " Second commit") && Ed()->previewTab() == nullptr && Ed()->tabCount() == 1;
    });
    ClickCommit("Root commit");
    MCGuiWait("another click: a second tab, the root commit's, in preview", 20, [] {
        return OnShow(gShort[0] + " Root commit") && Ed()->tabCount() == 2 && Ed()->previewTab() != nullptr;
    });

    // ------------------------------------ a commit already kept shows it
    ClickCommit("Second commit");
    MCGuiWait("the kept tab shows, no new tab", 20, [] {
        return OnShow(gShort[1] + " Second commit") && Ed()->tabCount() == 2 && Ed()->previewTab() != Ed()->active();
    });
    MCGuiStep("two commit tabs still", [] { GCHECK_EQ_STR(std::to_string(CommitTabs().size()), "2"); });

    // --------------------------- a commit with no files; Return keeps it
    ClickCommit("Nothing here");
    MCGuiWait("its page replaces the preview tab and says so", 20, [] {
        ChangeReviewPage* p = CommitPage();
        return OnShow(gShort[2] + " Nothing here") && Ed()->tabCount() == 2 &&
               p->pageText() == "This commit changes no files." && Ed()->previewTab() != nullptr;
    });
    MCGuiStep("Return on the selected graph row", [] { MCGuiKey("enter"); });
    MCGuiWait("kept: no preview tab, still two tabs", 15, [] {
        return OnShow(gShort[2] + " Nothing here") && Ed()->previewTab() == nullptr && Ed()->tabCount() == 2;
    });
    // ------------- graph rows read as their commits; an activate opens one
    // (the Mac's 3c02cbe0: the label is GitUi::graphRowLabel, the subject first)
    MCGuiWait("Root commit's row on screen, labelled by its commit", 15, [] {
        const int row = GraphRowOf("Root commit");
        if (row < 0 || !Panel()->graphRowWidget(row)) return false;
        const std::string l = Panel()->graphRowAccessibleLabel(row);
        return l.rfind("Root commit, commit " + gShort[0], 0) == 0;
    });
    MCGuiStep("the label names the commit's hash, and its branch", [] {
        const std::string l = Panel()->graphRowAccessibleLabel(GraphRowOf("Nothing here"));
        MCGuiNote("label: %s", l.c_str());
        GCHECK(l.rfind("Nothing here, commit " + gShort[2] + ", ", 0) == 0);
        GCHECK(l.find("main (current)") != std::string::npos);
    });
    MCGuiPause(0.8);   // time is the condition: a pointer's press within 600 ms is not an assistive activate
    MCGuiStep("an accessibility activate on Root commit's row", [] {
        GtkWidget* list = Panel()->graphRowWidget(GraphRowOf("Root commit"));
        while (list && !GTK_IS_LIST_VIEW(list)) list = gtk_widget_get_parent(list);
        if (!GCHECK(list != nullptr)) return;
        g_signal_emit_by_name(list, "activate", (guint)GraphRowOf("Root commit"));
    });
    MCGuiWait("its page opens in the preview tab, its row selected", 20, [] {
        return OnShow(gShort[0] + " Root commit") && Panel()->selectedGraphRow() == GraphRowOf("Root commit");
    });

    MCGuiStep("the pages close", [] { Ed()->closeAllTabs(); });
    MCGuiWait("no tabs", 10, [] { return Ed()->tabCount() == 0 && CommitTabs().empty(); });
    Shell("the repository goes", "rm -rf '" + C() + "'");
    // The copies this suite's discards put in the scratch Trash go too: the
    // next suite that checks the Trash empty (git_actions) may run after.
    Shell("our Trash emptied", "rm -rf '" + ScratchTrash() + "'/*");
    CloseRepoWindow();
}
