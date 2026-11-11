// CompareSuite.cpp — the compare view (`./build/minicode-guitest compare`,
// linux/PARITY.md item 3.2, the Mac's compare suite in tests/gui/CompareSuite.mm,
// part one): two texts side by side in a tab of their own, the rows lined up
// (a filler row opposite a line only one side has), each pane in the file's
// syntax colors, the words that changed inside a changed row marked, the
// panes scrolling together, F7 and Shift+F7 by real key presses stepping
// through the runs of changes (wrapping), and a 50,000-line-a-side comparison
// opening within a CPU bound on the main thread. A comparison with a decision
// (part two): a check and a cross per hunk in the gutter pressed with real
// clicks, a choice taken back, the last choice sent after a grace (Send Now,
// the bar's buttons, closing the tab), the accepted text.
#include "Harness.h"

#include "CompareView.h"
#include "Document.h"
#include "Editor.h"
#include "TabStrip.h"

#include <algorithm>
#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
CompareView* cv() { return ed()->compareView(); }

// 200 lines of Python; the right side changes line 20 (a number), removes
// line 100 and adds a line after 180: three runs of changes.
std::string line(int i) { return "value_" + std::to_string(i) + " = compute(" + std::to_string(i) + ")\n"; }

CompareSpec smallSpec() {
    CompareSpec s;
    s.title = "a.py (compare)";
    s.leftLabel = "HEAD: a.py";
    s.rightLabel = "a.py";
    s.fileName = "a.py";
    s.leftText = "def f(a):\n    return a\n";
    s.rightText = "def f(a):\n    return a\n";
    for (int i = 2; i < 200; i++) {
        std::string l = line(i), r = line(i);
        if (i == 20) r = "value_20 = compute(21)\n";
        if (i == 100) r.clear();
        s.leftText += l;
        s.rightText += r;
        if (i == 180) s.rightText += "added = True\n";
    }
    return s;
}

double gCpu0 = 0, gCpu = 0;
size_t gRows = 0;
Document* gDoc = nullptr;
size_t gTabsBefore = 0;

// The decision made: what the callback was told.
int gTold = 0;            // calls
bool gAccepted = false;
std::string gText;
CompareDecision* dec() { return gDoc ? gDoc->decision().get() : nullptr; }

CompareSpec decisionSpec(double grace) {
    CompareSpec s = smallSpec();
    s.title = "a.py (decide)";
    s.decisionPrompt = "Apply this change to a.py?";
    s.lastChoiceGrace = grace;
    gTold = 0;
    gAccepted = false;
    gText.clear();
    s.onDecision = [](bool accepted, const std::string& text) {
        gTold++;
        gAccepted = accepted;
        gText = text;
    };
    return s;
}

// A real click on a hunk's check or cross.
void clickHunk(size_t hunk, bool accept) {
    double x, y, w, h;
    cv()->scrollToRow(std::max(0L, (long)cv()->data()->hunks[hunk].first - 3));
    if (!cv()->hunkButtonRect(hunk, accept, &x, &y, &w, &h)) {
        GCHECK(false && "the hunk's button is on screen");
        return;
    }
    MCGuiClick(cv()->gutter(), x + w / 2, y + h / 2);
}

void openDecision(double grace) {
    gDoc = nullptr;
    ed()->showComparison(decisionSpec(grace), [](Document* d) { gDoc = d; });
}

std::vector<size_t> firsts() {
    std::vector<size_t> v;
    for (const SideBySide::Hunk& h : cv()->data()->hunks) v.push_back(h.first);
    return v;
}

}  // namespace

MC_GUI_SUITE(compare) {
    MCGuiStep("a comparison opens in a tab of its own", [] {
        ed()->closeAllTabs();
        gTabsBefore = ed()->tabCount();
        gDoc = nullptr;
        ed()->showComparison(smallSpec(), [](Document* d) { gDoc = d; });
    });
    MCGuiWait("the tab shows it", 10, [] { return gDoc && ed()->isComparison() && cv(); });
    MCGuiWaitQuiet("it is laid out", 0.3, 5, [] { return (long)gtk_widget_get_height(cv()->widget()); });
    MCGuiStep("the tab is titled, read-only and has no file", [] {
        GCHECK_EQ_STR(ed()->diffTitle(), "a.py (compare)");
        GCHECK((size_t)ed()->tabCount() == gTabsBefore + 1);
        GCHECK(ed()->currentPath().empty());
        GCHECK(!ed()->canSave());
        TabStrip* s = ed()->strip();
        GCHECK_EQ_STR(s->item(s->count() - 1).title, "a.py (compare)");
        GCHECK(MCGuiOnScreen(cv()->widget()));
    });
    MCGuiStep("rows line up: a filler opposite a line only one side has", [] {
        const CompareData* d = cv()->data();
        GCHECK(d && d->rows.size() == 201);   // 200 lines a side, one row more for the added line
        GCHECK(d->panes[0].line.size() == d->rows.size() && d->panes[1].line.size() == d->rows.size());
        size_t fillers = 0, same = 0;
        for (size_t r = 0; r < d->rows.size(); r++) {
            if (d->panes[0].line[r] < 0 || d->panes[1].line[r] < 0) fillers++;
            if (d->rows[r].kind == SideBySide::Kind::Same) {
                same++;
                GCHECK(d->panes[0].rowText(r) == d->panes[1].rowText(r));
            }
        }
        GCHECK(fillers == 2);   // the removed line 100 and the added line after 180
        GCHECK(same == 198);
        GCHECK(d->hunks.size() == 3);
        GCHECK(d->changedRows == 3);
    });
    MCGuiStep("the changed words are marked, and syntax colors follow the file's name", [] {
        const CompareData* d = cv()->data();
        long changed = -1;
        for (size_t r = 0; r < d->rows.size(); r++)
            if (d->rows[r].kind == SideBySide::Kind::Changed) { changed = (long)r; break; }
        GCHECK(changed == 20);
        const auto l = cv()->wordsOnRow(true, (size_t)changed), r = cv()->wordsOnRow(false, (size_t)changed);
        GCHECK(l.size() == 1 && l[0] == "20");
        GCHECK(r.size() == 1 && r[0] == "21");
        GCHECK(cv()->wordsOnRow(true, 5).empty());
        GCHECK(cv()->styleAt(true, 0, 0) == (int)TokenStyle::Keyword);        // def
        GCHECK(cv()->styleAt(false, 0, 0) == (int)TokenStyle::Keyword);
        GCHECK(cv()->styleAt(true, 0, 4) == (int)TokenStyle::Function);       // f
        GCHECK(cv()->styleAt(true, 0, 3) == -1);                              // a blank
    });
    MCGuiStep("every row is one fixed height: the second pane scrolls with the first", [] {
        MCGuiScroll(cv()->pane(true), 4);
    });
    MCGuiWait("the right pane follows the left", 5, [] {
        const double a = gtk_adjustment_get_value(cv()->vadjustment(true));
        return a > 0 && a == gtk_adjustment_get_value(cv()->vadjustment(false));
    });
    MCGuiStep("and the left follows the right", [] {
        MCGuiScroll(cv()->pane(false), -4);
    });
    MCGuiWait("both back at the top", 5, [] {
        return gtk_adjustment_get_value(cv()->vadjustment(true)) == 0 &&
               gtk_adjustment_get_value(cv()->vadjustment(false)) == 0;
    });
    MCGuiStep("F7 by a real key press goes to the first change", [] {
        MCGuiClick(cv()->pane(false), 200, 40);   // the pane has the keyboard
    });
    MCGuiWait("the pane has the keyboard", 5, [] {
        return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)) == cv()->focusWidget();
    });
    MCGuiStep("F7", [] { MCGuiKey("f7"); });
    MCGuiWait("the first run of changes", 5, [] { return cv()->currentRow() == (long)firsts()[0]; });
    MCGuiStep("it is scrolled into view, in both panes", [] {
        GCHECK(cv()->topRow(true) == (long)firsts()[0] - 3 && cv()->topRow(false) == cv()->topRow(true));
        MCGuiKey("f7");
    });
    MCGuiWait("the second", 5, [] { return cv()->currentRow() == (long)firsts()[1]; });
    MCGuiStep("and the third", [] {
        GCHECK(cv()->topRow(true) == (long)firsts()[1] - 3 && cv()->topRow(false) == cv()->topRow(true));
        MCGuiKey("f7");
    });
    MCGuiWait("the third", 5, [] { return cv()->currentRow() == (long)firsts()[2]; });
    MCGuiStep("F7 again wraps to the first", [] { MCGuiKey("f7"); });
    MCGuiWait("wrapped", 5, [] { return cv()->currentRow() == (long)firsts()[0]; });
    MCGuiStep("Shift+F7 wraps back to the last", [] { MCGuiKey("shift+f7"); });
    MCGuiWait("the last", 5, [] { return cv()->currentRow() == (long)firsts()[2]; });
    MCGuiStep("and Shift+F7 again steps to the one before", [] { MCGuiKey("shift+f7"); });
    MCGuiWait("the second", 5, [] { return cv()->currentRow() == (long)firsts()[1]; });
    MCGuiStep("a picture of it", [] { MCGuiSnapshot(cv()->widget(), "compare"); });

    MCGuiStep("50,000 lines a side open within a CPU bound on the main thread", [] {
        CompareSpec s;
        s.title = "big.py (compare)";
        s.fileName = "big.py";
        for (int i = 0; i < 50000; i++) {
            s.leftText += line(i);
            s.rightText += i % 10 == 0 ? "value_x = compute(" + std::to_string(i) + ", 1)\n" : line(i);
        }
        gDoc = nullptr;
        gCpu0 = MCGuiThreadCpuMs();
        ed()->showComparison(s, [](Document* d) { gDoc = d; });
    });
    MCGuiWait("it shows", 60, [] { return gDoc && ed()->diffTitle() == "big.py (compare)"; });
    MCGuiWaitQuiet("it is drawn", 0.3, 10, [] { return (long)gtk_widget_get_height(cv()->widget()); });
    MCGuiStep("rows, hunks and the bound", [] {
        gCpu = MCGuiThreadCpuMs() - gCpu0;
        gRows = cv()->data()->rows.size();
        GCHECK(gRows == 50000);
        GCHECK(cv()->data()->hunks.size() == 5000);
        MCGuiNote("main thread CPU for 50,000 lines a side: %.0f ms", gCpu);
        GCHECK(MCGuiTimingSkipped() || gCpu < 500);
    });
    MCGuiStep("scrolling to the end is as cheap, and F7 reaches a change far down", [] {
        gCpu0 = MCGuiThreadCpuMs();
        cv()->scrollToRow(49990);
        GCHECK(cv()->topRow(true) == cv()->topRow(false));
    });
    MCGuiWaitQuiet("drawn", 0.2, 5, [] { return (long)cv()->topRow(true); });
    MCGuiStep("it cost little", [] {
        const double ms = MCGuiThreadCpuMs() - gCpu0;
        MCGuiNote("scroll to the end and draw: %.0f ms", ms);
        GCHECK(MCGuiTimingSkipped() || ms < 300);
    });
    MCGuiStep("closing the tabs", [] { ed()->closeAllTabs(); });

    // ---- a comparison with a decision
    MCGuiStep("a comparison with a prompt opens with a bar and a gutter", [] {
        ed()->closeAllTabs();
        openDecision(0.8);
    });
    MCGuiWait("it shows", 10, [] { return gDoc && ed()->isComparison() && cv() && dec(); });
    MCGuiWaitQuiet("it is laid out", 0.3, 5, [] { return (long)gtk_widget_get_height(cv()->widget()); });
    MCGuiStep("the bar says its prompt and the gutter shows", [] {
        GCHECK(MCGuiOnScreen(cv()->bar()) && MCGuiOnScreen(cv()->gutter()));
        GCHECK_EQ_STR(cv()->barText(), "Apply this change to a.py?");
        GCHECK(MCGuiFindButton(cv()->bar(), "Accept") && MCGuiFindButton(cv()->bar(), "Reject"));
        double x, y, w, h;
        cv()->scrollToRow(10);
        GCHECK(cv()->hunkButtonRect(0, true, &x, &y, &w, &h));
        GCHECK(cv()->hunkButtonRect(0, false, &x, &y, &w, &h));
    });
    MCGuiStep("the first change is shown", [] { cv()->step(true); });
    MCGuiWaitQuiet("it is drawn", 0.3, 5, [] { return (long)cv()->topRow(true); });
    MCGuiStep("a picture of the bar and the gutter", [] {
        MCGuiSnapshot(cv()->widget(), "compare-decide");
    });
    MCGuiStep("a click on the first hunk's check accepts it", [] { clickHunk(0, true); });
    MCGuiWait("accepted", 5, [] { return dec()->choiceOfHunk(0) == 1; });
    MCGuiStep("the bar counts, and the choice is taken back by a click on it", [] {
        GCHECK(cv()->barText().find("1 accepted, 0 rejected, 2 to decide") != std::string::npos);
        GCHECK(MCGuiFindButton(cv()->bar(), "Accept the Rest"));
        clickHunk(0, true);
    });
    MCGuiWait("undecided again", 5, [] { return dec()->choiceOfHunk(0) == 0; });
    MCGuiStep("a click on the other button of a decided hunk changes nothing; the cross rejects", [] {
        clickHunk(0, false);
    });
    MCGuiWait("rejected", 5, [] { return dec()->choiceOfHunk(0) == -1; });
    MCGuiStep("the second is accepted", [] { clickHunk(1, true); });
    MCGuiWait("accepted", 5, [] { return dec()->choiceOfHunk(1) == 1; });
    MCGuiStep("and the third, the last choice starting the grace", [] { clickHunk(2, true); });
    MCGuiWait("the answer is about to go", 5, [] { return dec()->sending(); });
    MCGuiStep("the bar says so and offers Send Now; nothing is sent yet", [] {
        GCHECK(cv()->barText().find("sending in a moment") != std::string::npos);
        GCHECK(MCGuiFindButton(cv()->bar(), "Send Now"));
        GCHECK(gTold == 0);
        clickHunk(2, true);   // a misclick taken back
    });
    MCGuiWait("not sending any more", 5, [] { return !dec()->sending() && dec()->choiceOfHunk(2) == 0; });
    MCGuiPause(1.2);   // time is the condition: the grace has passed and nothing was sent
    MCGuiStep("nothing was sent", [] {
        GCHECK(gTold == 0 && dec()->decision() == 0);
        clickHunk(2, false);   // decided again, rejected
    });
    MCGuiWait("sending again", 5, [] { return dec()->sending(); });
    MCGuiWait("sent after the grace", 10, [] { return gTold == 1; });
    MCGuiStep("one accepted: the text has the second hunk only", [] {
        GCHECK(gAccepted && dec()->decision() == 1);
        GCHECK(gText.find("value_20 = compute(20)") != std::string::npos);   // hunk 0 rejected
        GCHECK(gText.find("value_20 = compute(21)") == std::string::npos);
        GCHECK(gText.find("value_100 = ") == std::string::npos);             // hunk 1: removed
        GCHECK(gText.find("added = True") == std::string::npos);             // hunk 2 rejected
        GCHECK(!gtk_widget_get_visible(cv()->gutter()));
        GCHECK(cv()->barText().find("Accepted 1 of 3") != std::string::npos);
    });

    MCGuiStep("Send Now ends the grace at once, all accepted gives the right text", [] {
        ed()->closeAllTabs();
        openDecision(30);
    });
    MCGuiWait("it shows", 10, [] { return gDoc && ed()->isComparison() && cv() && dec(); });
    MCGuiWaitQuiet("laid out", 0.3, 5, [] { return (long)gtk_widget_get_height(cv()->widget()); });
    MCGuiStep("the first check", [] { clickHunk(0, true); });
    MCGuiWait("accepted", 5, [] { return dec()->choiceOfHunk(0) == 1; });
    MCGuiStep("the second", [] { clickHunk(1, true); });
    MCGuiWait("accepted", 5, [] { return dec()->choiceOfHunk(1) == 1; });
    MCGuiStep("the third", [] { clickHunk(2, true); });
    MCGuiWait("sending", 5, [] { return dec()->sending(); });
    MCGuiStep("the bar's Send Now button", [] {
        GCHECK(gTold == 0);
        MCGuiClick(MCGuiFindButton(cv()->bar(), "Send Now"));
    });
    MCGuiWait("sent at once", 5, [] { return gTold == 1; });
    MCGuiStep("with all accepted the text is the right text", [] {
        GCHECK(gAccepted);
        GCHECK(gText == gDoc->compare()->spec.rightText);
    });

    MCGuiStep("the bar's Reject rejects the whole change", [] {
        ed()->closeAllTabs();
        openDecision(30);
    });
    MCGuiWait("it shows", 10, [] { return gDoc && ed()->isComparison() && cv() && dec(); });
    MCGuiWaitQuiet("laid out", 0.3, 5, [] { return (long)gtk_widget_get_height(cv()->widget()); });
    MCGuiStep("Reject", [] { MCGuiClick(MCGuiFindButton(cv()->bar(), "Reject")); });
    MCGuiWait("rejected", 5, [] { return gTold == 1; });
    MCGuiStep("told no", [] {
        GCHECK(!gAccepted && dec()->decision() == -1);
        GCHECK_EQ_STR(cv()->barText(), "Rejected.");
    });

    MCGuiStep("closing the tab before the last choice is a rejection", [] {
        ed()->closeAllTabs();
        openDecision(30);
    });
    MCGuiWait("it shows", 10, [] { return gDoc && ed()->isComparison() && cv() && dec(); });
    MCGuiWaitQuiet("laid out", 0.3, 5, [] { return (long)gtk_widget_get_height(cv()->widget()); });
    MCGuiStep("one hunk accepted, then the tab closes", [] {
        clickHunk(0, true);
        ed()->closeTab(gDoc);
    });
    MCGuiWait("told no", 5, [] { return gTold == 1; });
    MCGuiStep("rejected", [] { GCHECK(!gAccepted); });

    MCGuiStep("closing the tab with the answer about to go sends it as decided", [] {
        openDecision(30);
    });
    MCGuiWait("it shows", 10, [] { return gDoc && ed()->isComparison() && cv() && dec(); });
    MCGuiWaitQuiet("laid out", 0.3, 5, [] { return (long)gtk_widget_get_height(cv()->widget()); });
    MCGuiStep("the first check", [] { clickHunk(0, true); });
    MCGuiWait("accepted", 5, [] { return dec()->choiceOfHunk(0) == 1; });
    MCGuiStep("the second", [] { clickHunk(1, true); });
    MCGuiWait("accepted", 5, [] { return dec()->choiceOfHunk(1) == 1; });
    MCGuiStep("the third", [] { clickHunk(2, true); });
    MCGuiWait("sending", 5, [] { return dec()->sending(); });
    MCGuiStep("every hunk accepted, then the tab closes in the grace", [] {
        ed()->closeTab(gDoc);
    });
    MCGuiWait("sent as decided", 5, [] { return gTold == 1; });
    MCGuiStep("accepted", [] { GCHECK(gAccepted); });
    MCGuiStep("done", [] { ed()->closeAllTabs(); });
}
