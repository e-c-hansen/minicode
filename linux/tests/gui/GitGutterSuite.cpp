// GitGutterSuite.cpp — `./build/minicode-guitest git_gutter`: the editor's
// change bars (GitGutter.h, the core's QuickDiff.h) on a scratch repository
// with real git, PARITY.md item 2.10, the Mac's tests/gui/GitGutterSuite.mm
// with real keys and clicks.
//
// A tracked file edited in three places by typing gets a changed, an added
// and a deleted bar; a line typed above them moves them at once (looks held,
// so only the edit moves them) and the next diff agrees; Alt+F5 and
// Shift+Alt+F5 move the caret between them, wrapping, typing nothing; a
// click on a bar shows its old lines with Revert Change and Stage Change;
// the bars draw in VS Code's colors and in editor.git-modified's; Revert
// Change puts a change back as one undo step; Stage Change puts one change
// of the unsaved buffer in the index and nothing else; none for an
// untracked file, in large-file mode, or with editor.git-gutter off, which
// also takes the lane away; git changing the index from outside, with the
// panel hidden and no key pressed, moves them (the watch on the
// repository's folder); and Stage Change is refused, the index left as it
// is, when the index entry moved on after the bars were drawn.
#include "GitChecks.h"

#include "BufferLines.h"
#include "CodeView.h"
#include "Document.h"
#include "GitGutter.h"
#include "Gutter.h"

#include <cmath>

using namespace gitchecks;

namespace {

GitGutter* Bars() { return RepoWindow().changeBars; }
Editor* Ed() { return RepoWindow().editor; }
Gutter* Gut() { return Ed()->codeView()->gutter(); }
GtkTextBuffer* Buf() { return Ed()->buffer(); }

std::string Text() {
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(Buf(), &a, &b);
    char* t = gtk_text_buffer_get_text(Buf(), &a, &b, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

size_t CaretLine() {
    BufferLines* l = BufferLines::of(Buf());
    if (!l) return (size_t)-1;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(Buf(), &it, gtk_text_buffer_get_insert(Buf()));
    return l->lineOf((size_t)gtk_text_iter_get_offset(&it));
}

// The caret to the start of a line, the editor with the keyboard: where
// the typing that follows goes.
void CaretTo(size_t line) {
    BufferLines* l = BufferLines::of(Buf());
    GCHECK(l != nullptr);
    if (!l) return;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(Buf(), &it, (int)l->lineStart(line));
    gtk_text_buffer_place_cursor(Buf(), &it);
    gtk_widget_grab_focus(Ed()->textView());
}

std::string Join(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) out += (out.empty() ? "" : " | ") + s;
    return out;
}

bool Descs(const std::vector<std::string>& want) { return Bars()->barDescriptions() == want; }

std::string IndexText(const std::string& rel) { return GitText({"show", ":" + rel}); }

// What the window's status bar says.
std::string Status() { return gtk_label_get_text(GTK_LABEL(RepoWindow().statusLabel)); }

// A click on bar `index` in the gutter, as a hand makes it: in the lane, on
// the bar's lines (a deletion's mark sits on the boundary below the line
// above it, so a little above that boundary).
void ClickBar(const std::string& what, int index) {
    MCGuiWait("bar " + std::to_string(index) + " drawn", 10, [index] {
        const graphene_rect_t r = Gut()->rectOfChangeBar(index);
        return r.size.height > 0 && MCGuiOnScreen(Gut()->widget());
    });
    MCGuiStep(what, [index] {
        const graphene_rect_t r = Gut()->rectOfChangeBar(index);
        const bool deletion = Bars()->barDescriptions()[(size_t)index].rfind("deleted", 0) == 0;
        const double y = deletion ? r.origin.y + r.size.height / 2 - 2
                                  : r.origin.y + r.size.height / 2;
        MCGuiClick(Gut()->widget(), Gutter::kDotLane + 1, y);
    });
    MCGuiWait("its popover", 10, [] {
        return Bars()->popoverShown() && MCGuiOnScreen(Bars()->revertButton());
    });
}

// The gutter as drawn, and a pixel of it (as GutterSuite.cpp reads one).
struct Picture {
    int width = 0, height = 0;
    double scale = 1;
    std::vector<guchar> bytes;
    bool at(double x, double y, int* r, int* g, int* b) const {
        const int px = (int)(x * scale), py = (int)(y * scale);
        if (px < 0 || py < 0 || px >= width || py >= height) return false;
        const guchar* p = &bytes[(size_t)(py * width + px) * 4];
        *b = p[0];
        *g = p[1];
        *r = p[2];
        return true;
    }
};
Picture PictureOf(GtkWidget* w) {
    Picture pic;
    const int ww = gtk_widget_get_width(w), wh = gtk_widget_get_height(w);
    if (ww <= 0 || wh <= 0) return pic;
    GdkPaintable* paintable = gtk_widget_paintable_new(w);
    GtkSnapshot* snap = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snap, ww, wh);
    GskRenderNode* node = gtk_snapshot_free_to_node(snap);
    GskRenderer* renderer = gtk_native_get_renderer(gtk_widget_get_native(w));
    if (node && renderer) {
        GdkTexture* t = gsk_renderer_render_texture(renderer, node, nullptr);
        pic.width = gdk_texture_get_width(t);
        pic.height = gdk_texture_get_height(t);
        pic.scale = (double)pic.width / ww;
        pic.bytes.resize((size_t)pic.width * pic.height * 4);
        gdk_texture_download(t, pic.bytes.data(), (size_t)pic.width * 4);
        g_object_unref(t);
    }
    if (node) gsk_render_node_unref(node);
    g_object_unref(paintable);
    return pic;
}
// The middle of bar `index`, as drawn, near a color.
bool BarColored(int index, int r, int g, int b) {
    const graphene_rect_t rect = Gut()->rectOfChangeBar(index);
    const Picture p = PictureOf(Gut()->widget());
    int pr = 0, pg = 0, pb = 0;
    if (!p.at(rect.origin.x + rect.size.width / 2, rect.origin.y + rect.size.height / 2, &pr, &pg,
              &pb))
        return false;
    MCGuiNote("bar %d's pixel: %02X%02X%02X", index, pr, pg, pb);
    return std::abs(pr - r) <= 6 && std::abs(pg - g) <= 6 && std::abs(pb - b) <= 6;
}

unsigned gDiffs = 0, gLooks = 0, gChanges = 0;
std::string gStaged = "(not yet)";

const char* kSettings = "lsp.enabled = false\n";

}  // namespace

MC_GUI_SUITE(git_gutter) {
    if (GitPath().empty()) {
        MCGuiStep("no git", [] { MCGuiNote("no git here: the change bar checks are skipped"); });
        return;
    }
    const std::string repo = MCGuiScratch() + "/gitgutter";
    const std::vector<std::string> three = {"modified 2+1 (was 2+1)", "added 6+1 (was 6+0)",
                                            "deleted 9+0 (was 8+1)"};
    const std::vector<std::string> moved = {"modified 3+1 (was 2+1)", "added 7+1 (was 6+0)",
                                            "deleted 10+0 (was 8+1)"};

    MCGuiStep("the settings: the bars on, as by default", [] { MCGuiSetSettings(kSettings); });
    Shell("a repository: g.txt of ten lines, and a file of 300,001 lines",
          "set -e; mkdir -p '" + repo + "'; cd '" + repo + "'; " + G() +
              " init -q -b main; for i in 0 1 2 3 4 5 6 7 8 9; do echo l$i; done > g.txt; "
              "awk 'BEGIN { for (i = 0; i <= 300000; i++) print \"x\" }' > big.txt; " +
              G() + " add -A; " + G() + " commit -qm first");
    OpenRepoWindow(repo, false);
    MCGuiStep("g.txt shown; the lane is there", [] {
        GCHECK(Bars() != nullptr);
        GCHECK(Gut()->changeLane());
        gDiffs = Bars()->diffs();
        MCGuiOpenFileKept(In("g.txt"));
    });
    MCGuiWait("a first diff: no bars", 15, [] {
        return Ed()->currentPath() == In("g.txt") && Bars()->diffs() > gDiffs;
    });

    // ---- typing makes the bars, and moves them
    // Each edit waits for the one before: the keys are only on their way
    // when a step ends, and the caret is put in place at once.
    MCGuiStep("a line changed: l2's l typed over", [] {
        GCHECK(Bars()->barCount() == 0);
        gDiffs = Bars()->diffs();
        CaretTo(2);
        MCGuiKey("shift+right");
        MCGuiType("L");
    });
    MCGuiWait("typed", 10, [] { return Text().rfind("l0\nl1\nL2\n", 0) == 0; });
    MCGuiStep("a line added below l5", [] {
        CaretTo(6);
        MCGuiType("new\n");
    });
    MCGuiWait("typed", 10, [] { return Text().find("l5\nnew\nl6\n") != std::string::npos; });
    MCGuiStep("l8 deleted, selected with Shift+Down", [] {
        CaretTo(9);   // l8, below the new line
        MCGuiKey("shift+down");
        MCGuiKey("delete");
    });
    MCGuiWait("the text as typed", 10, [] {
        return Text() == "l0\nl1\nL2\nl3\nl4\nl5\nnew\nl6\nl7\nl9\n";
    });
    MCGuiWait("three bars", 15, [three] { return Bars()->diffs() > gDiffs && Descs(three); });
    MCGuiStep("looks held: a line typed above them", [] {
        MCGuiNote("bars: %s", Join(Bars()->barDescriptions()).c_str());
        Bars()->holdLooksForChecks(true);
        gDiffs = Bars()->diffs();
        CaretTo(0);
        MCGuiType("top\n");
    });
    MCGuiWait("the line typed", 10, [] { return Text().rfind("top\nl0\n", 0) == 0; });
    MCGuiStep("it moved them at once, with no diff", [moved] {
        MCGuiNote("moved: %s", Join(Bars()->barDescriptions()).c_str());
        GCHECK(Descs(moved));
        GCHECK(Bars()->diffs() == gDiffs);
        Bars()->holdLooksForChecks(false);
    });
    MCGuiWait("the next diff agrees, and marks the new line", 15, [moved] {
        std::vector<std::string> want = {"added 0+1 (was 0+0)"};
        want.insert(want.end(), moved.begin(), moved.end());
        return Bars()->diffs() > gDiffs && Descs(want);
    });

    // ---- Alt+F5 and Shift+Alt+F5
    MCGuiStep("Alt+F5 from the top", [] {
        CaretTo(0);
        MCGuiKey("alt+f5");
    });
    MCGuiWait("the changed line", 10, [] { return CaretLine() == 3; });
    MCGuiStep("Alt+F5", [] { MCGuiKey("alt+f5"); });
    MCGuiWait("the added line", 10, [] { return CaretLine() == 7; });
    MCGuiStep("Alt+F5", [] { MCGuiKey("alt+f5"); });
    MCGuiWait("below the deletion", 10, [] { return CaretLine() == 10; });
    MCGuiStep("Alt+F5", [] { MCGuiKey("alt+f5"); });
    MCGuiWait("wrapped to the top line's bar", 10, [] { return CaretLine() == 0; });
    MCGuiStep("Shift+Alt+F5", [] { MCGuiKey("alt+shift+f5"); });
    MCGuiWait("wrapped back to the deletion", 10, [] { return CaretLine() == 10; });
    MCGuiStep("the keys typed nothing", [] {
        GCHECK(Text().rfind("top\nl0\nl1\nL2\n", 0) == 0);
    });

    // ---- a click on a bar
    ClickBar("a click on the changed bar", 1);
    MCGuiStep("its old line, with the buttons", [] {
        GCHECK_EQ_STR(Bars()->popoverTitle(), "1 line changed");
        GCHECK_EQ_STR(Bars()->popoverText(), "l2");
        GCHECK(gtk_widget_get_sensitive(Bars()->stageButton()));
        MCGuiKey("escape");
    });
    MCGuiWait("closed by Escape", 10, [] { return !Bars()->popoverShown(); });
    ClickBar("a click on the deletion", 3);
    MCGuiStep("the deleted line", [] {
        GCHECK_EQ_STR(Bars()->popoverTitle(), "1 line deleted");
        GCHECK_EQ_STR(Bars()->popoverText(), "l8");
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 10, [] { return !Bars()->popoverShown(); });

    // ---- colors
    // A picture of the gutter taken at once can predate the popover's close.
    MCGuiWait("the bars draw in VS Code's colors", 10, [] {
        return BarColored(1, 0x1B, 0x81, 0xA8) &&    // modified
               BarColored(0, 0x48, 0x7E, 0x02);      // added
    });
    MCGuiStep("the bars' color setting", [] {
        MCGuiSetSettings(std::string(kSettings) + "editor.git-modified = #FF00FF\n");
    });
    MCGuiWait("and in editor.git-modified's", 10, [] { return BarColored(1, 0xFF, 0x00, 0xFF); });
    MCGuiStep("the colors back", [] { MCGuiSetSettings(kSettings); });
    MCGuiWait("VS Code's again", 10, [] { return BarColored(1, 0x1B, 0x81, 0xA8); });

    // ---- Revert Change, from the popover
    ClickBar("a click on the changed bar", 1);
    MCGuiStep("Revert Change", [] { MCGuiClick(Bars()->revertButton()); });
    MCGuiWait("the line put back", 10, [] {
        return Text().rfind("top\nl0\nl1\nl2\nl3\n", 0) == 0 && !Bars()->popoverShown();
    });
    MCGuiStep("one undo step: Ctrl+Z", [] {
        gtk_widget_grab_focus(Ed()->textView());
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("the change back", 10, [] { return Text().rfind("top\nl0\nl1\nL2\nl3\n", 0) == 0; });
    MCGuiStep("and Ctrl+Shift+Z", [] {
        gDiffs = Bars()->diffs();
        MCGuiKey("ctrl+shift+z");
    });
    MCGuiWait("reverted again", 10, [] { return Text().rfind("top\nl0\nl1\nl2\nl3\n", 0) == 0; });
    MCGuiWait("its bar gone", 15, [] { return Bars()->diffs() > gDiffs && Bars()->barCount() == 3; });

    // ---- Stage Change, from the popover
    MCGuiStep("the file was never saved: the buffer's text is staged", [] {
        GCHECK(MCGuiReadFile(In("g.txt")).find("new") == std::string::npos);
        Bars()->onStaged = [](const std::string& failure) { gStaged = failure; };
        gStaged = "(not yet)";
        gDiffs = Bars()->diffs();
        MCGuiNote("bars: %s", Join(Bars()->barDescriptions()).c_str());
    });
    ClickBar("a click on the added line's bar", 1);
    MCGuiStep("Stage Change", [] {
        GCHECK_EQ_STR(Bars()->popoverTitle(), "1 line added");
        MCGuiClick(Bars()->stageButton());
    });
    MCGuiWait("staged", 15, [] { return gStaged.empty(); });
    MCGuiStep("the index has that one line added, nothing else; the status bar says so", [] {
        GCHECK_EQ_STR(IndexText("g.txt"), "l0\nl1\nl2\nl3\nl4\nl5\nnew\nl6\nl7\nl8\nl9\n");
        GCHECK_EQ_STR(Status(), "Staged a change to g.txt.");
    });
    MCGuiWait("its bar gone too", 15, [] {
        return Bars()->diffs() > gDiffs &&
               Descs({"added 0+1 (was 0+0)", "deleted 10+0 (was 9+1)"});
    });

    // ---- where there are none
    MCGuiStep("an untracked file", [] {
        MCGuiWriteFile(In("u.txt"), "a\nb\n");
        gLooks = Bars()->looks();
        MCGuiOpenFileKept(In("u.txt"));
    });
    MCGuiWait("looked at", 15, [] {
        return Ed()->currentPath() == In("u.txt") && Bars()->looks() > gLooks;
    });
    MCGuiStep("edited", [] {
        CaretTo(0);
        MCGuiType("zz\n");
        gDiffs = Bars()->diffs();
    });
    MCGuiWait("diffed against nothing", 15, [] {
        return Text() == "zz\na\nb\n" && Bars()->diffs() > gDiffs;
    });
    MCGuiStep("no bars", [] { GCHECK(Bars()->barCount() == 0); });

    MCGuiStep("a file in large-file mode", [] {
        gLooks = Bars()->looks();
        MCGuiOpenFileKept(In("big.txt"));
    });
    MCGuiWait("looked at", 30, [] {
        return Ed()->currentPath() == In("big.txt") && Ed()->active() &&
               !Ed()->active()->loading() && Bars()->looks() > gLooks;
    });
    MCGuiStep("edited", [] {
        GCHECK(Ed()->active()->largeFileFeaturesOff());
        CaretTo(0);
        MCGuiType("zz\n");
        gLooks = Bars()->looks();
    });
    MCGuiWait("looked at again", 15, [] {
        return Text().rfind("zz\nx\n", 0) == 0 && Bars()->looks() > gLooks;
    });
    MCGuiStep("no bars there either", [] { GCHECK(Bars()->barCount() == 0); });

    MCGuiStep("g.txt again; editor.git-gutter off", [] {
        MCGuiOpenFileKept(In("g.txt"));
        GCHECK(Ed()->currentPath() == In("g.txt"));
        MCGuiSetSettings(std::string(kSettings) + "editor.git-gutter = off\n");
    });
    MCGuiWait("no lane, no bars", 10, [] {
        return !Gut()->changeLane() && Bars()->barCount() == 0 &&
               BufferLines::of(Buf())->changeBars().empty();
    });
    MCGuiStep("Alt+F5 moves nothing while off", [] {
        CaretTo(0);
        MCGuiKey("alt+f5");
    });
    MCGuiPause(0.5);   // before a negative check: the caret is to stay put
    MCGuiStep("on again", [] {
        GCHECK(CaretLine() == 0);
        gDiffs = Bars()->diffs();
        MCGuiSetSettings(kSettings);
    });
    MCGuiWait("the lane and the bars again", 15, [] {
        return Gut()->changeLane() && Bars()->diffs() > gDiffs && Bars()->barCount() == 2;
    });

    // ---- git outside, the panel hidden, no key pressed: the watch on the
    // repository's folder
    MCGuiStep("the buffer's text written to the index from outside", [] {
        GCHECK(!Panel()->active());
        gChanges = Bars()->indexChanges();
        gDiffs = Bars()->diffs();
        MCGuiWriteFile(MCGuiScratch() + "/gitgutter-buffer.txt", Text());
    });
    Shell("git hash-object and update-index, as a terminal would",
          "set -e; cd '" + repo + "'; o=$(" + G() + " hash-object -w '" + MCGuiScratch() +
              "/gitgutter-buffer.txt'); " + G() + " update-index --cacheinfo 100644,$o,g.txt");
    MCGuiWait("the bars go by themselves", 15, [] {
        return Bars()->diffs() > gDiffs && Bars()->barCount() == 0;
    });
    MCGuiStep("seen as an index change", [] {
        MCGuiNote("index changes seen: %u", Bars()->indexChanges() - gChanges);
        GCHECK(Bars()->indexChanges() > gChanges);
    });

    // ---- Stage Change refused when the index moved on under the bars
    MCGuiStep("a line typed at the top", [] {
        gDiffs = Bars()->diffs();
        CaretTo(0);
        MCGuiType("again\n");
    });
    MCGuiWait("its bar", 15, [] {
        return Bars()->diffs() > gDiffs && Descs({"added 0+1 (was 0+0)"});
    });
    MCGuiStep("looks held; the index changed from outside, under the bar", [] {
        Bars()->holdLooksForChecks(true);
        gChanges = Bars()->indexChanges();
        MCGuiWriteFile(MCGuiScratch() + "/gitgutter-other.txt", "something else\n");
    });
    Shell("another version staged, as a terminal would",
          "set -e; cd '" + repo + "'; o=$(" + G() + " hash-object -w '" + MCGuiScratch() +
              "/gitgutter-other.txt'); " + G() + " update-index --cacheinfo 100644,$o,g.txt");
    MCGuiWait("the watch saw it (the look waits)", 15, [] {
        return Bars()->indexChanges() > gChanges;
    });
    ClickBar("a click on the bar drawn before", 0);
    MCGuiStep("Stage Change", [] {
        gStaged = "(not yet)";
        MCGuiClick(Bars()->stageButton());
    });
    MCGuiWait("refused", 15, [] { return gStaged != "(not yet)"; });
    MCGuiStep("nothing staged; the status bar says why", [] {
        MCGuiNote("refusal: %s", gStaged.c_str());
        GCHECK(gStaged.find("changed since its bars were drawn") != std::string::npos);
        GCHECK_EQ_STR(IndexText("g.txt"), "something else\n");
        GCHECK(Status().find("nothing was staged") != std::string::npos);
        gDiffs = Bars()->diffs();
        Bars()->holdLooksForChecks(false);
    });
    MCGuiWait("the bars show the index as it is", 15, [] {
        return Bars()->diffs() > gDiffs && Bars()->barCount() == 1 &&
               Bars()->barDescriptions()[0].rfind("modified 0+", 0) == 0;
    });

    MCGuiStep("the window closes, its tabs not saved", [] {
        Bars()->onStaged = nullptr;
        gtk_window_close(RepoWindow().window);
    });
    MCGuiWait("the question", 10, [] { return QuestionUp(); });
    MCGuiStep("Don't Save", [] { Answer("Don't Save"); });
    MCGuiWait("gone", 10, [] { return RepoWindow().window == nullptr; });
    MCGuiStep("the first window has the keyboard again", [] { MCGuiSetKeyWindow(nullptr); });
}
