// GitConflictsSuite.cpp — `./build/minicode-guitest git_conflicts`: merge
// conflicts resolved in the editor (EditorConflicts.h) and on the review
// page (ChangeReviewPage.h), the counterpart of the Mac's git_conflicts in
// mac/tests/gui/GitPanelSuite.mm, with real clicks and keys.
//
// A real merge in a scratch repository stops on conflicts (diff3 style, so
// each block has its common ancestor): three blocks in c.txt, two in
// r.txt. In the editor, c.txt's blocks are found with their labels and
// tinted (the current side, the ancestor and the incoming side, the markers
// more strongly), with Keep Current, Keep Incoming and Keep Both in a row
// on each "<<<<<<<" line after its text, then "Conflict 1 of 3"; Alt+F5 and
// Shift+Alt+F5 step between the blocks, wrapping, as the change bars skip
// the file; an edit inside a block drops it at once and the next read finds
// it again, and one that takes its separator away leaves it gone; a click
// on Keep Current resolves the first as one undo step ("Keep Current
// Change", undone and done again) and the status bar says how many are
// left; Keep Incoming and Keep Both leave none. On the review page r.txt's
// two blocks show whole, each part tinted, with the three choices on each
// and Side by Side and Open on the file; Keep Incoming and Keep Both write
// the file, and with none left Mark Resolved stages it (c.txt too), after
// which git lists no conflict.
#include "GitChecks.h"

#include "ChangeReviewPage.h"
#include "CompareView.h"
#include "DocumentView.h"
#include "EditorConflicts.h"
#include "GitGutter.h"

#include <cmath>

using namespace gitchecks;

namespace {

std::string R() { return MCGuiScratch() + "/gitconflicts"; }
Editor* Ed() { return RepoWindow().editor; }
EditorConflicts* X() { return Ed() ? Ed()->conflicts() : nullptr; }
GtkTextBuffer* Buf() { return Ed()->buffer(); }
GtkTextView* TV() { return GTK_TEXT_VIEW(Ed()->textView()); }

std::string Text() {
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(Buf(), &a, &b);
    char* t = gtk_text_buffer_get_text(Buf(), &a, &b, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

std::string Status() { return gtk_label_get_text(GTK_LABEL(RepoWindow().statusLabel)); }

std::string Join(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) out += (out.empty() ? "" : " | ") + s;
    return out;
}

// The text from the caret on (the text is ASCII: bytes are characters).
std::string FromCaret() {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(Buf(), &it, gtk_text_buffer_get_insert(Buf()));
    const std::string t = Text();
    const size_t at = (size_t)gtk_text_iter_get_offset(&it);
    return at <= t.size() ? t.substr(at) : "";
}

void CaretTo(size_t offset) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(Buf(), &it, (int)offset);
    gtk_text_buffer_place_cursor(Buf(), &it);
}

// The middle of the line holding character `offset`, in the view's widget
// coordinates (y), and its height.
double LineMidY(size_t offset, int* height = nullptr) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(Buf(), &it, (int)offset);
    int y = 0, h = 0, wx = 0, wy = 0;
    gtk_text_view_get_line_yrange(TV(), &it, &y, &h);
    gtk_text_view_buffer_to_window_coords(TV(), GTK_TEXT_WINDOW_WIDGET, 0, y, &wx, &wy);
    if (height) *height = h;
    return wy + h / 2.0;
}

// The pixel at the view's right side (past any text) on the line holding
// `offset`.
bool LinePixel(size_t offset, int rgba[4]) {
    GtkWidget* w = Ed()->textView();
    const int x = gtk_widget_get_width(w) - 24;
    return MCGuiPixel(w, x, (int)LineMidY(offset), rgba);
}

// Whether a pixel is `over` with `tint` drawn on it (source over).
bool Tinted(const int got[4], const int over[4], const GdkRGBA& tint, const char* what) {
    const double a = tint.alpha;
    const double want[3] = {over[0] * (1 - a) + tint.red * 255 * a,
                            over[1] * (1 - a) + tint.green * 255 * a,
                            over[2] * (1 - a) + tint.blue * 255 * a};
    MCGuiNote("%s: %02X%02X%02X, want about %02X%02X%02X (over %02X%02X%02X)", what, got[0],
              got[1], got[2], (int)want[0], (int)want[1], (int)want[2], over[0], over[1], over[2]);
    for (int i = 0; i < 3; i++)
        if (std::fabs(got[i] - want[i]) > 10) return false;
    return true;
}

ChangeReviewPage* Page() {
    Editor* e = Ed();
    return e ? dynamic_cast<ChangeReviewPage*>(e->currentPage()) : nullptr;
}

// The first row after a file's header whose buttons are the Keep choices.
long FirstBlockAfter(long header) {
    ChangeReviewPage* p = Page();
    if (!p || header < 0) return -1;
    for (size_t r = (size_t)header + 1; r < p->rowCount(); r++) {
        const std::vector<std::string> b = p->buttonsOfRow(r);
        if (!b.empty() && b[0] == "Keep Current") return (long)r;
        if (!p->buttonsOfRow(r).empty() && p->rowText(r).find(".txt") != std::string::npos)
            return -1;   // the next file's header
    }
    return -1;
}

int gReads = 0;
int gPixels[5][4];   // the lines' pixels: plain, current, ancestor, incoming, end marker

// A press on a row's button, for real: the row scrolled to the top, the
// button on screen, then a click on it.
void Press(const std::string& title, std::function<long()> row) {
    // The page reads again after an action, and again when the panel's
    // refresh that follows it lands; a row's button moves when the files
    // change (Mark Resolved moves a file to Staged Changes), so a click
    // aimed between two reads found the old place.
    MCGuiWaitQuiet("the page read and settled", 0.6, 10, [] {
        ChangeReviewPage* p = Page();
        return p ? (long)p->reads() * 2 + (p->loaded() ? 0 : 1) : -1L;
    });
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
        gReads = Page()->reads();
        MCGuiClick(Page()->buttonWidget(title, (size_t)row()));
    });
}

const char* kSettings = "lsp.enabled = false\n";

}  // namespace

MC_GUI_SUITE(git_conflicts) {
    if (GitPath().empty()) {
        MCGuiStep("no git", [] { MCGuiNote("no git here: the conflict checks are skipped"); });
        return;
    }
    // Eighteen lines; main and topic each change lines 2, 8 and 14 (r.txt:
    // 2 and 8), so the merge stops with three blocks in c.txt, two in r.txt.
    const std::string lines = "1\\n2\\n3\\n4\\n5\\n6\\n7\\n8\\n9\\n10\\n11\\n12\\n13\\n14\\n15\\n16\\n17\\n18\\n";
    MCGuiStep("the settings: no language servers", [] { MCGuiSetSettings(kSettings); });
    Shell("a repository whose merge of topic stops on conflicts in c.txt and r.txt",
          "set -e; mkdir -p '" + R() + "'; cd '" + R() + "'; " + G() + " init -q -b main; " + G() +
              " config merge.conflictStyle diff3; printf '" + lines + "' > c.txt; printf '" + lines +
              "' > r.txt; " + G() + " add -A; " + G() + " commit -qm base; " + G() +
              " checkout -qb topic; "
              "sed -i -e 's/^2$/two topic/' -e 's/^8$/eight topic/' -e 's/^14$/fourteen topic/' c.txt; "
              "sed -i -e 's/^2$/two topic/' -e 's/^8$/eight topic/' r.txt; " +
              G() + " commit -qam topic; " + G() + " checkout -q main; "
              "sed -i -e 's/^2$/two main/' -e 's/^8$/eight main/' -e 's/^14$/fourteen main/' c.txt; "
              "sed -i -e 's/^2$/two main/' -e 's/^8$/eight main/' r.txt; " +
              G() + " commit -qam main; " + G() + " merge -q topic >/dev/null 2>&1 || true; "
              "grep -q '^<<<<<<< HEAD' c.txt; grep -q '^||||||| ' r.txt");
    OpenRepoWindow(R(), false);
    MCGuiStep("c.txt open", [] { MCGuiOpenFileKept(In("c.txt")); });
    MCGuiWait("three blocks found in the editor", 10, [] {
        return Ed()->currentPath() == In("c.txt") && X() && X()->count() == 3;
    });
    MCGuiStep("labelled; the change bars skip the file", [] {
        MCGuiNote("blocks: %s", Join(X()->blockDescriptions()).c_str());
        GCHECK_EQ_STR(X()->blockDescriptions()[0], "current HEAD 1, incoming topic 1, base");
        GCHECK(RepoWindow().changeBars->barCount() == 0);
        CaretTo(0);
        gtk_text_view_scroll_to_mark(TV(), gtk_text_buffer_get_insert(Buf()), 0, TRUE, 0, 0);
        gtk_widget_grab_focus(Ed()->textView());
    });
    MCGuiWait("the first block's buttons on screen", 10, [] {
        const graphene_rect_t r = X()->buttonRect(0, 0);
        return r.size.width > 0 && MCGuiOnScreen(Ed()->textView()) &&
               MCGuiTextLaidOut(Ed()->textView());
    });

    // ---- the tints and the buttons
    MCGuiStep("in a row on the <<<<<<< line, after its text", [] {
        const graphene_rect_t cur = X()->buttonRect(0, 0), inc = X()->buttonRect(1, 0),
                              both = X()->buttonRect(2, 0), pos = X()->positionRect(0);
        MCGuiNote("buttons: %.0f+%.0f, %.0f+%.0f, %.0f+%.0f at y %.0f; the label at %.0f",
                  cur.origin.x, cur.size.width, inc.origin.x, inc.size.width, both.origin.x,
                  both.size.width, cur.origin.y, pos.origin.x);
        GCHECK(cur.size.width > 0 && inc.size.width > 0 && both.size.width > 0);
        GCHECK(inc.origin.x > cur.origin.x + cur.size.width);
        GCHECK(both.origin.x > inc.origin.x + inc.size.width);
        GCHECK(pos.origin.x > both.origin.x + both.size.width);
        const std::string t = Text();
        const size_t marker = t.find("<<<<<<< HEAD");
        GCHECK(marker != std::string::npos);
        GCHECK(std::fabs(cur.origin.y + cur.size.height / 2 - LineMidY(marker)) < 4);
        // After the line's text: right of where "<<<<<<< HEAD" ends.
        GtkTextIter end;
        gtk_text_buffer_get_iter_at_offset(Buf(), &end, (int)(marker + 12));
        GdkRectangle er;
        gtk_text_view_get_iter_location(TV(), &end, &er);
        int wx = 0, wy = 0;
        gtk_text_view_buffer_to_window_coords(TV(), GTK_TEXT_WINDOW_WIDGET, er.x, er.y, &wx, &wy);
        GCHECK(cur.origin.x > wx);
    });
    // The tints: line "1" is plain; the current side, the ancestor and the
    // incoming side tinted over it, the end marker more strongly. A picture
    // of the view now and then comes back empty while the machine is busy
    // (MCGuiPixel false, seen once with the core's tests running beside),
    // so the five pixels are read until one picture of each has come.
    MCGuiWait("the lines' pixels read", 10, [] {
        const std::string t = Text();
        const size_t at[5] = {0, t.find("two main"), t.find("\n2\n") + 1, t.find("two topic"),
                              t.find(">>>>>>> topic")};
        for (int i = 0; i < 5; i++)
            if (!LinePixel(at[i], gPixels[i])) return false;
        return true;
    });
    MCGuiStep("the sides tinted", [] {
        const int* plain = gPixels[0];
        const int* current = gPixels[1];
        const int* base = gPixels[2];
        const int* incoming = gPixels[3];
        const int* endMarker = gPixels[4];
        GCHECK(Tinted(current, plain, X()->currentColor(), "the current side"));
        GCHECK(Tinted(incoming, plain, X()->incomingColor(), "the incoming side"));
        GCHECK(Tinted(endMarker, plain, EditorConflicts::headerOf(X()->incomingColor()),
                      "the end marker"));
        GCHECK(Tinted(base, plain, GdkRGBA{0x60 / 255.0f, 0x60 / 255.0f, 0x60 / 255.0f, 0x29 / 255.0f},
                      "the ancestor"));
        MCGuiSnapshot(GTK_WIDGET(RepoWindow().window), "git-conflicts-editor");
    });

    // ---- Alt+F5 and Shift+Alt+F5 step between the blocks
    MCGuiStep("Alt+F5 from the top", [] {
        CaretTo(0);
        gtk_widget_grab_focus(Ed()->textView());
        MCGuiKey("alt+f5");
    });
    MCGuiWait("the first block", 5, [] { return FromCaret().rfind("<<<<<<< HEAD\ntwo main", 0) == 0; });
    MCGuiStep("Alt+F5 again", [] { MCGuiKey("alt+f5"); });
    MCGuiWait("the second", 5, [] { return FromCaret().rfind("<<<<<<< HEAD\neight main", 0) == 0; });
    MCGuiStep("Shift+Alt+F5", [] { MCGuiKey("shift+alt+f5"); });
    MCGuiWait("the first again", 5, [] { return FromCaret().rfind("<<<<<<< HEAD\ntwo main", 0) == 0; });
    MCGuiStep("Shift+Alt+F5 wraps", [] { MCGuiKey("shift+alt+f5"); });
    MCGuiWait("the last", 5, [] { return FromCaret().rfind("<<<<<<< HEAD\nfourteen main", 0) == 0; });
    MCGuiStep("nothing typed", [] {
        GCHECK(Text().find("<<<<<<< HEAD\ntwo main\n") != std::string::npos);
        GCHECK_EQ_STR(Ed()->active()->undoName(), "");
    });

    // ---- an edit inside a block drops it at once; read again, it is back
    MCGuiStep("a letter typed in the first block's current side", [] {
        const size_t at = Text().find("two main") + 3;
        CaretTo(at);
        gtk_widget_grab_focus(Ed()->textView());
        MCGuiType("X");
    });
    MCGuiWait("typed", 5, [] {
        return Text().find("twoX main") != std::string::npos;
    });
    MCGuiWait("three again after the pause", 5, [] { return X()->count() == 3; });
    MCGuiStep("undone", [] { Ed()->undoInSource(false); });
    MCGuiWait("as it was", 5, [] {
        return Text().find("two main") != std::string::npos && X()->count() == 3;
    });
    MCGuiStep("the first block's ======= line deleted", [] {
        const std::string t = Text();
        const size_t sep = t.find("=======\n");
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(Buf(), &a, (int)sep);
        gtk_text_buffer_get_iter_at_offset(Buf(), &b, (int)(sep + 8));
        gtk_text_buffer_begin_user_action(Buf());
        gtk_text_buffer_delete(Buf(), &a, &b);
        gtk_text_buffer_end_user_action(Buf());
        // At once: the block it touched is dropped.
        GCHECK(X()->count() == 2);
    });
    MCGuiPause(0.6);   // a negative check: the read after the pause finds no third block
    MCGuiStep("still two once read again; undone", [] {
        GCHECK(X()->count() == 2);
        Ed()->undoInSource(false);
    });
    MCGuiWait("three again", 5, [] { return X()->count() == 3; });

    // ---- the choices
    MCGuiStep("the top on screen again", [] {
        CaretTo(0);
        gtk_text_view_scroll_to_mark(TV(), gtk_text_buffer_get_insert(Buf()), 0, TRUE, 0, 0);
    });
    MCGuiWait("Keep Current on screen", 10, [] { return X()->buttonRect(0, 0).size.width > 0; });
    MCGuiStep("a click on Keep Current", [] {
        const graphene_rect_t r = X()->buttonRect(0, 0);
        MCGuiClick(Ed()->textView(), r.origin.x + r.size.width / 2, r.origin.y + r.size.height / 2);
    });
    MCGuiWait("the first block resolved", 10, [] { return X()->count() == 2; });
    MCGuiStep("one undo step; the status bar says what is left", [] {
        GCHECK(Text().rfind("1\ntwo main\n3\n", 0) == 0);
        GCHECK_EQ_STR(Ed()->active()->undoName(), "Keep Current Change");
        GCHECK_EQ_STR(Status(), "2 conflicts left in c.txt.");
        Ed()->undoInSource(false);
    });
    MCGuiWait("undone: the block is back", 5, [] {
        return Text().rfind("1\n<<<<<<< HEAD\ntwo main\n||||||| ", 0) == 0 && X()->count() == 3;
    });
    MCGuiStep("Keep Current again, then Keep Incoming on the next", [] {
        GCHECK(X()->keep(0, 0));
        GCHECK(X()->keep(1, 0));
        GCHECK(X()->count() == 1);
        GCHECK(Text().rfind("1\ntwo main\n3\n4\n5\n6\n7\neight topic\n9\n", 0) == 0);
        GCHECK_EQ_STR(Ed()->active()->undoName(), "Keep Incoming Change");
    });
    MCGuiStep("Alt+F5 goes to the last block", [] {
        CaretTo(0);
        MCGuiKey("alt+f5");
    });
    MCGuiWait("there", 5, [] { return FromCaret().rfind("<<<<<<< HEAD\nfourteen main", 0) == 0; });
    MCGuiStep("Keep Both there: none left", [] {
        GCHECK(X()->keep(2, 0));
        GCHECK(X()->count() == 0);
        GCHECK_EQ_STR(Text(), "1\ntwo main\n3\n4\n5\n6\n7\neight topic\n9\n10\n11\n12\n13\n"
                              "fourteen main\nfourteen topic\n15\n16\n17\n18\n");
        GCHECK(Status().rfind("No conflicts left in c.txt.", 0) == 0);
        GCHECK_EQ_STR(Ed()->active()->undoName(), "Keep Both Changes");
        gtk_widget_grab_focus(Ed()->textView());
        MCGuiKey("ctrl+s");
    });
    MCGuiWait("saved", 10, [] { return MCGuiReadFile(In("c.txt")).rfind("1\ntwo main\n", 0) == 0; });

    // ---- the review page
    MCGuiStep("Ctrl+Shift+G", [] { MCGuiKey("ctrl+shift+g"); });
    MCGuiWait("Review Changes offered", 15, [] {
        return Panel() && Panel()->active() && Panel()->reviewButtonShown() &&
               MCGuiOnScreen(Panel()->reviewButton());
    });
    MCGuiStep("a click on Review Changes", [] { MCGuiClick(Panel()->reviewButton()); });
    MCGuiWait("read, both files listed", 15, [] {
        return Page() && Page()->loaded() && Page()->rowOfFile("r.txt", false) >= 0 &&
               Page()->rowOfFile("c.txt", false) >= 0;
    });
    MCGuiStep("r.txt's blocks whole, tinted, with the three choices; c.txt has Mark Resolved", [] {
        ChangeReviewPage* p = Page();
        const long header = p->rowOfFile("r.txt", false), cHeader = p->rowOfFile("c.txt", false);
        MCGuiNote("r.txt: %s; c.txt: %s", p->rowText((size_t)header).c_str(),
                  p->rowText((size_t)cHeader).c_str());
        GCHECK(p->rowText((size_t)header).find("2 conflicts left") != std::string::npos);
        GCHECK(p->rowText((size_t)cHeader).find("no conflicts left") != std::string::npos);
        const long block = FirstBlockAfter(header);
        GCHECK(block > header);
        if (block < 0) return;
        const std::vector<std::pair<std::string, std::string>> want = {
            {"<<<<<<< HEAD", "conflict-current-marker"}, {"two main", "conflict-current"},
            {"|||||||", "conflict-base-marker"},          {"2", "conflict-base"},
            {"=======", ""},                              {"two topic", "conflict-incoming"},
            {">>>>>>> topic", "conflict-incoming-marker"}};
        for (size_t i = 0; i < want.size(); i++) {
            const std::string text = p->rowText((size_t)block + i), tint = p->rowTint((size_t)block + i);
            MCGuiNote("row %zu: \"%s\" %s", i, text.c_str(), tint.c_str());
            GCHECK(text.find(want[i].first) != std::string::npos);
            GCHECK_EQ_STR(tint, want[i].second);
        }
        GCHECK((p->buttonsOfRow((size_t)block) ==
                std::vector<std::string>{"Keep Current", "Keep Incoming", "Keep Both"}));
        GCHECK((p->buttonsOfRow((size_t)header) == std::vector<std::string>{"Side by Side", "Open"}));
        GCHECK((p->buttonsOfRow((size_t)cHeader) == std::vector<std::string>{"Open", "Mark Resolved"}));
        MCGuiSnapshot(GTK_WIDGET(RepoWindow().window), "git-conflicts-review");
    });
    Press("Side by Side", [] { return Page()->rowOfFile("r.txt", false); });
    MCGuiWait("the two sides compared", 10, [] {
        CompareView* v = Ed()->compareView();
        return v && v->data() && v->data()->spec.title == "r.txt (current and incoming)";
    });
    MCGuiStep("every current side against every incoming one; back to the review", [] {
        const CompareSpec& c = Ed()->compareView()->data()->spec;
        MCGuiNote("labels: %s | %s", c.leftLabel.c_str(), c.rightLabel.c_str());
        GCHECK_EQ_STR(c.leftLabel, "Current (HEAD): r.txt");
        GCHECK_EQ_STR(c.rightLabel, "Incoming (topic): r.txt");
        GCHECK(c.leftText.rfind("1\ntwo main\n3\n4\n5\n6\n7\neight main\n9\n", 0) == 0);
        GCHECK(c.rightText.rfind("1\ntwo topic\n3\n4\n5\n6\n7\neight topic\n9\n", 0) == 0);
        GCHECK(c.path.empty());   // neither side is the file on disk
        for (Document* d : Ed()->documents())
            if (d->editorPage() && dynamic_cast<ChangeReviewPage*>(d->editorPage().get()))
                Ed()->activate(d);
    });
    MCGuiWait("the review again", 10, [] { return Page() && Page()->loaded(); });
    Press("Keep Incoming", [] { return FirstBlockAfter(Page()->rowOfFile("r.txt", false)); });
    MCGuiWait("the first block resolved and read again", 15, [] {
        ChangeReviewPage* p = Page();
        const long h = p ? p->rowOfFile("r.txt", false) : -1;
        return p && p->reads() > gReads && p->loaded() && h >= 0 &&
               p->rowText((size_t)h).find("1 conflict left") != std::string::npos;
    });
    MCGuiStep("written", [] {
        MCGuiNote("after Keep Incoming: %s", Page()->messageText().c_str());
        GCHECK_EQ_STR(Page()->messageText(), "1 conflict left in r.txt.");
        GCHECK(MCGuiReadFile(In("r.txt")).rfind("1\ntwo topic\n3\n", 0) == 0);
    });
    Press("Keep Both", [] { return FirstBlockAfter(Page()->rowOfFile("r.txt", false)); });
    MCGuiWait("none left in r.txt", 15, [] {
        ChangeReviewPage* p = Page();
        const long h = p ? p->rowOfFile("r.txt", false) : -1;
        return p && p->reads() > gReads && p->loaded() && h >= 0 &&
               p->rowText((size_t)h).find("no conflicts left") != std::string::npos;
    });
    MCGuiStep("written; Mark Resolved offered", [] {
        GCHECK_EQ_STR(MCGuiReadFile(In("r.txt")),
                      "1\ntwo topic\n3\n4\n5\n6\n7\neight main\neight topic\n9\n10\n11\n12\n"
                      "13\n14\n15\n16\n17\n18\n");
        GCHECK(Page()->messageText().rfind("No conflicts left in r.txt.", 0) == 0);
        GCHECK((Page()->buttonsOfRow((size_t)Page()->rowOfFile("r.txt", false)) ==
                std::vector<std::string>{"Open", "Mark Resolved"}));
    });
    Press("Mark Resolved", [] { return Page()->rowOfFile("r.txt", false); });
    MCGuiWait("r.txt staged", 15, [] { return GitText({"ls-files", "-u", "--", "r.txt"}).empty(); });
    MCGuiWait("said so, and read again: r.txt staged", 10, [] {
        return Page()->messageText() == "Marked r.txt resolved." && Page()->loaded() &&
               Page()->rowOfFile("r.txt", true) >= 0;
    });
    Press("Mark Resolved", [] { return Page()->rowOfFile("c.txt", false); });
    MCGuiWait("no conflicts left in git", 15, [] {
        return GitText({"ls-files", "-u"}).empty() &&
               GitText({"diff", "--cached", "--name-only"}).find("c.txt") != std::string::npos;
    });
    MCGuiStep("the panel hidden again", [] { MCGuiKey("ctrl+shift+g"); });
    MCGuiWait("hidden", 10, [] { return !Panel()->active(); });
    CloseRepoWindow();
}
