// GitPanelSuite.cpp — `./build/mutible-guitest git_panel`, the counterpart
// of the Mac's git_panel checks from #217 in tests/gui/GitPanelSuite.mm (a
// click opens, pictures show), by real clicks and keys: a Ctrl click and the
// arrows open nothing; a single click on a changed file opens the review of
// every change (ChangeReviewPage.h) in the preview tab at that file, the
// keyboard left in the list; a click on another file shows it in the same
// tab; a double-click keeps the tab and gives the page the keyboard; Open
// as git Diff in a row's menu shows git's own diff; and a changed PNG shows
// its two versions side by side, labelled Before and After with their
// sizes, a new one and a deleted one their one picture each.
#include "GitChecks.h"

#include "ChangeReviewPage.h"

using namespace gitchecks;

namespace {

std::string R() { return MCGuiScratch() + "/gitpanel"; }
size_t gOldPic = 0, gGone = 0;   // the committed pictures' sizes in bytes

ChangeReviewPage* Page() {
    Editor* e = RepoWindow().editor;
    return e ? dynamic_cast<ChangeReviewPage*>(e->currentPage()) : nullptr;
}
std::vector<Document*> ReviewTabs() {
    std::vector<Document*> out;
    if (Editor* e = RepoWindow().editor)
        for (Document* d : e->documents())
            if (d->editorPage() && dynamic_cast<ChangeReviewPage*>(d->editorPage().get()))
                out.push_back(d);
    return out;
}
Document* ReviewTab() {
    const std::vector<Document*> t = ReviewTabs();
    return t.size() == 1 ? t[0] : nullptr;
}
// The file's header row is on show in the pane.
bool Shows(const std::string& path) {
    ChangeReviewPage* p = Page();
    if (!p || !p->loaded()) return false;
    const long row = p->rowOfFile(path, false);
    if (row < 0) return false;
    const double y = p->rowScreenY((size_t)row);
    return y >= 0 && y < p->paneHeight() - 10;
}

// A PNG of w by h pixels, of one color, through GTK's own encoder; its size.
size_t WritePng(const std::string& path, int w, int h, unsigned char shade) {
    std::string px((size_t)w * h * 4, '\0');
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = (char)shade;
        px[i + 1] = (char)(255 - shade);
        px[i + 2] = (char)0x40;
        px[i + 3] = (char)0xFF;
    }
    GBytes* bytes = g_bytes_new(px.data(), px.size());
    GdkTexture* t = gdk_memory_texture_new(w, h, GDK_MEMORY_R8G8B8A8, bytes, (gsize)w * 4);
    g_bytes_unref(bytes);
    GCHECK(gdk_texture_save_to_png(t, path.c_str()));
    g_object_unref(t);
    return MCGuiReadFile(path).size();
}
std::string Pixels(int w, int h, size_t bytes) {
    return std::to_string(w) + " \xC3\x97 " + std::to_string(h) + " pixels, " + ChangeReview::byteText(bytes);
}

}  // namespace

MC_GUI_SUITE(git_panel) {
    if (GitPath().empty()) {
        MCGuiStep("no git", [] { MCGuiNote("no git here: the panel's checks are skipped"); });
        return;
    }
    Shell("a scratch repository", "set -e; mkdir -p '" + R() + "'");
    MCGuiStep("pictures of 4 by 3 pixels, and of 2 by 2", [] {
        gOldPic = WritePng(R() + "/pic.png", 4, 3, 0x20);
        gGone = WritePng(R() + "/gone.png", 2, 2, 0x90);
    });
    // a.txt's long change makes the page taller than the pane, so a file's
    // place on it shows.
    Shell("committed; a.txt (150 lines) and b.txt changed, gone.png deleted",
          "set -e; cd '" + R() + "'; seq 1 150 | sed 's/^/a /' > a.txt; printf 'bee\\n' > b.txt; " + G() +
              " init -q -b main; " + G() + " add -A; " + G() + " commit -qm first; "
              "seq 1 150 | sed 's/^/A /' > a.txt; printf 'BEE\\n' > b.txt; " + G() + " rm -q gone.png");
    MCGuiStep("pic.png redrawn at 8 by 6, and new.png of 5 by 5", [] {
        WritePng(R() + "/pic.png", 8, 6, 0xC0);
        WritePng(R() + "/new.png", 5, 5, 0x60);
    });
    MCGuiStep("a Trash of our own", [] { UseScratchTrash(true); });
    OpenRepoWindow(R(), true);
    MCGuiWait("the changes listed", 15, [] {
        return Panel()->inRepository() && RowOf("S D gone.png") >= 0 && RowOf("W M a.txt") >= 0 &&
               RowOf("W M b.txt") >= 0 && RowOf("W M pic.png") >= 0 && RowOf("W U new.png") >= 0;
    });

    // ------------------------------------- a Ctrl click and the arrows
    ClickRow("W M a.txt", "ctrl");
    MCGuiPause(0.8);   // a negative check: time for a review to open, if it did
    MCGuiStep("a Ctrl click chooses the row and opens nothing", [] {
        GCHECK(ReviewTabs().empty());
        const std::vector<int> chosen = Panel()->chosenRows();
        GCHECK(std::find(chosen.begin(), chosen.end(), RowOf("W M a.txt")) != chosen.end());
    });
    MCGuiStep("Down", [] { MCGuiKey("down"); });
    MCGuiWait("b.txt chosen", 10, [] { return Panel()->selectedRow() == RowOf("W M b.txt"); });
    MCGuiPause(0.8);   // a negative check, as above
    MCGuiStep("the arrows only move the selection", [] { GCHECK(ReviewTabs().empty()); });

    // ------------------------------------------ a click opens, in preview
    ClickRow("W M pic.png");
    MCGuiWait("the review in the preview tab, at pic.png", 15, [] {
        Document* t = ReviewTab();
        return t && t->isPreviewTab() && Page() && Shows("pic.png") && !Shows("a.txt");
    });
    MCGuiStep("one tab, the keyboard still in the list", [] {
        GCHECK(RepoWindow().editor->previewTab() == ReviewTab());
        GCHECK(Focused(Panel()->list()));
        GCHECK(Panel()->selectedRow() == RowOf("W M pic.png"));
        MCGuiNote("tabs %d; pic.png's header %.0f px down a pane of %.0f", RepoWindow().editor->tabCount(),
                  Page()->rowScreenY((size_t)Page()->rowOfFile("pic.png", false)), Page()->paneHeight());
    });

    // ------------------------------------------------------- the pictures
    MCGuiWait("four pictures, pic.png's on show", 15, [] {
        return Page() && Page()->imageWidgets().size() == 4 && MCGuiOnScreen(Page()->imageWidgets()[1]) &&
               MCGuiOnScreen(Page()->imageWidgets()[2]);
    });
    MCGuiStep("Before and After side by side with their sizes; the new and the deleted alone", [] {
        ChangeReviewPage* p = Page();
        const std::vector<std::string> labels = p->imageLabels();
        const std::vector<std::pair<int, int>> sizes = p->imageSizes();
        for (size_t i = 0; i < labels.size(); i++)
            MCGuiNote("picture %zu: %s, drawn %d x %d", i, labels[i].c_str(), sizes[i].first,
                      sizes[i].second);
        // The staged gone.png, then pic.png's two, then the untracked new.png.
        GCHECK(labels.size() == 4 && sizes.size() == 4);
        if (labels.size() == 4 && sizes.size() == 4) {
            GCHECK_EQ_STR(labels[0], Pixels(2, 2, gGone));
            GCHECK_EQ_STR(labels[1], "Before: " + Pixels(4, 3, gOldPic));
            GCHECK_EQ_STR(labels[2], "After: " + Pixels(8, 6, MCGuiReadFile(In("pic.png")).size()));
            GCHECK_EQ_STR(labels[3], Pixels(5, 5, MCGuiReadFile(In("new.png")).size()));
            GCHECK(sizes[1] == std::make_pair(4, 3) && sizes[2] == std::make_pair(8, 6));
            GCHECK(sizes[0] == std::make_pair(2, 2) && sizes[3] == std::make_pair(5, 5));
            // Side by side: After right of Before, on the same line.
            graphene_rect_t a, b;
            GCHECK(gtk_widget_compute_bounds(p->imageWidgets()[1], p->widget(), &a));
            GCHECK(gtk_widget_compute_bounds(p->imageWidgets()[2], p->widget(), &b));
            MCGuiNote("Before at %.0f,%.0f, After at %.0f,%.0f", a.origin.x, a.origin.y, b.origin.x,
                      b.origin.y);
            GCHECK(b.origin.x >= a.origin.x + a.size.width + 20);
            GCHECK(std::abs(b.origin.y - a.origin.y) < 1);
        }
        GCHECK(p->pageText().find("A binary file") == std::string::npos);
        MCGuiSnapshot(GTK_WIDGET(p->widget()), "git-panel-pictures");
    });

    // ---------------------------- another file's click: the same tab
    ClickRow("W M b.txt");
    MCGuiWait("the same preview tab, at b.txt", 15, [] {
        Document* t = ReviewTab();
        return t && t->isPreviewTab() && Shows("b.txt");
    });
    MCGuiStep("one review tab, the keyboard in the list", [] {
        GCHECK(ReviewTabs().size() == 1);
        GCHECK(Focused(Panel()->list()));
    });

    // ------------------------------------- a double-click keeps the tab
    MCGuiStep("the pointer off the rows", [] { MCGuiMoveTo(Panel()->messageView()); });
    MCGuiPause(0.6);   // time is the condition: no double-click with the last click
    MCGuiWait("a.txt on screen", 10, [] { return RowWidget("W M a.txt") != nullptr; });
    MCGuiStep("a double-click on a.txt", [] { MCGuiDoubleClick(RowWidget("W M a.txt")); });
    MCGuiWait("kept, at a.txt, the page has the keyboard", 15, [] {
        Document* t = ReviewTab();
        return t && !t->isPreviewTab() && Shows("a.txt") && Focused(Page()->focusWidget());
    });
    MCGuiStep("still one review tab, and no preview tab", [] {
        GCHECK(ReviewTabs().size() == 1);
        GCHECK(RepoWindow().editor->previewTab() == nullptr);
    });

    // ------------------------------------ Open as git Diff: git's own diff
    ClickRow("W M a.txt", "", 3);
    MCGuiWait("its menu has Open as git Diff", 10, [] { return MenuHas(Panel()->menu(), "Open as git Diff"); });
    MCGuiStep("its commands; Open as git Diff", [] {
        const std::vector<std::string> l = MenuLabels(Panel()->menu());
        GCHECK(l.size() >= 3 && l[0] == "Open File" && l[1] == "Open Changes" && l[2] == "Open as git Diff");
        MCGuiClick(MenuItem(Panel()->menu(), "Open as git Diff"));
    });
    MCGuiWait("git's diff, with its hunk links", 10, [] {
        return DiffOnShow() && Diff()->text().find("+A 150") != std::string::npos && LinkCount() == 2;
    });
    MCGuiStep("the review still open beside it", [] { GCHECK(ReviewTabs().size() == 1); });
    CloseRepoWindow();
}
