// GitVerbsSuite.cpp — `./build/minicode-guitest git_verbs` (PARITY.md 2.9),
// the counterpart of the Mac's git_verbs in tests/gui/GitPanelSuite.mm, by
// real keys and clicks: Shift with the arrows chooses rows and lets them go,
// and Shift+F10 opens the menu for them; two rows chosen with a Ctrl click stage together on
// Space and stay chosen, and the row menu names them and unstages both
// (Discard disabled, since they are staged); three chosen with a Shift
// click (the heading left out) discard after one question, which Cancel
// leaves alone, each its own Undo Discard; Commit from the message box with
// Ctrl+Return; Commit and Amend from the palette rewrites the last commit's
// message; Undo Last Commit from the panel's menu leaves its changes staged
// and its message in the box; Stash takes every change, the untracked file
// too, under the box's text, and Pop Stash brings them back; a second Pop
// is refused.
#include "GitChecks.h"

using namespace gitchecks;

namespace {

std::string R() { return MCGuiScratch() + "/gitverbs"; }
const std::vector<std::string> kChanges3 = {"# Changes  3", "W M a.txt", "W M b.txt", "W U n.txt"};
int gActions = 0;
bool Done() { return Panel()->actionsDone() > gActions; }

size_t Chosen() { return Panel()->chosenRows().size(); }

void PanelMenu(const std::string& item) {
    MCGuiStep("right click on the panel", [] { MCGuiClick(Panel()->branchLabel(), -1, -1, 3); });
    MCGuiWait("its menu has " + item, 10, [item] { return MenuHas(Panel()->menu(), item); });
    MCGuiStep(item, [item] {
        gActions = Panel()->actionsDone();
        MCGuiClick(MenuItem(Panel()->menu(), item));
    });
}

}  // namespace

MC_GUI_SUITE(git_verbs) {
    if (GitPath().empty()) {
        MCGuiStep("no git", [] { MCGuiNote("no git here: the verb checks are skipped"); });
        return;
    }
    Shell("a repository: two files changed, one untracked",
          "set -e; mkdir -p '" + R() + "'; cd '" + R() + "'; " + G() +
              " init -q -b main; echo one > a.txt; echo one > b.txt; " + G() + " add -A; " + G() +
              " commit -qm first; echo two > a.txt; echo two > b.txt; echo n > n.txt");
    MCGuiStep("a Trash of our own", [] { UseScratchTrash(true); });
    OpenRepoWindow(R(), true);
    MCGuiWait("three changes", 15, [] { return Rows(kChanges3) && RowWidget("W M b.txt"); });

    // ------------------------------------------------- several rows
    ClickRow("W M a.txt");
    MCGuiWait("chosen", 10, [] { return Chosen() == 1 && Panel()->selectedRow() == RowOf("W M a.txt"); });
    MCGuiStep("Shift+Down chooses b.txt too", [] { MCGuiKey("shift+down"); });
    MCGuiWait("two chosen", 10, [] { return Chosen() == 2; });
    MCGuiStep("and n.txt", [] { MCGuiKey("shift+down"); });
    MCGuiWait("three chosen", 10, [] { return Chosen() == 3; });
    MCGuiStep("Shift+Up lets n.txt go", [] { MCGuiKey("shift+up"); });
    MCGuiWait("two again", 10, [] {
        const std::vector<int> c = Panel()->chosenRows();
        return c == std::vector<int>({RowOf("W M a.txt"), RowOf("W M b.txt")});
    });
    MCGuiStep("Shift+F10: the menu for both", [] { MCGuiKey("shift+f10"); });
    MCGuiWait("it names them", 10, [] { return MenuHas(Panel()->menu(), "Stage Changes (2 Files)"); });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("closed", 10, [] { return !gtk_widget_get_mapped(Panel()->menu()); });
    ClickRow("W M a.txt");
    MCGuiWait("one chosen", 10, [] { return Chosen() == 1; });
    ClickRow("W M b.txt", "ctrl");
    MCGuiWait("both chosen", 10, [] { return Chosen() == 2; });
    MCGuiStep("Space stages both", [] { MCGuiKey("space"); });
    MCGuiWait("both staged", 15, [] {
        return Rows({"# Staged changes  2", "S M a.txt", "S M b.txt", "# Changes  1", "W U n.txt"});
    });
    MCGuiStep("both still chosen", [] { GCHECK(Chosen() == 2); });
    ClickRow("S M b.txt", "", 3);
    MCGuiWait("its menu", 10, [] { return MenuHas(Panel()->menu(), "Unstage Changes (2 Files)"); });
    MCGuiStep("Discard disabled while they are staged; Unstage both", [] {
        GCHECK(Chosen() == 2);   // a row among the chosen keeps them chosen
        const std::vector<std::string> l = MenuLabels(Panel()->menu());
        std::string all;
        for (const std::string& s : l) all += s + " | ";
        MCGuiNote("menu for two: %s", all.c_str());
        GtkWidget* discard = MenuItem(Panel()->menu(), "Discard Changes (2 Files)…");
        GCHECK(discard && !gtk_widget_is_sensitive(discard));
        MCGuiClick(MenuItem(Panel()->menu(), "Unstage Changes (2 Files)"));
    });
    MCGuiWait("both back in Changes", 15, [] { return Rows(kChanges3); });
    ClickRow("W M a.txt");
    ClickRow("W U n.txt", "shift");
    MCGuiWait("three chosen, the heading not", 10, [] { return Chosen() == 3; });
    ClickRow("W U n.txt", "", 3);
    MCGuiWait("its menu", 10, [] { return MenuHas(Panel()->menu(), "Discard Changes (3 Files)…"); });
    MCGuiStep("Discard Changes (3 Files)…", [] {
        MCGuiClick(MenuItem(Panel()->menu(), "Discard Changes (3 Files)…"));
    });
    MCGuiWait("it asks", 10, QuestionUp);
    MCGuiStep("naming the count; Cancel", [] {
        GCHECK(QuestionText().find("Discard the changes to 3 files?") != std::string::npos);
        Answer("Cancel");
    });
    MCGuiWait("the question goes", 10, QuestionGone);
    MCGuiStep("nothing gone; yes this time", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK(MCGuiFileExists(In("n.txt")));
        GCHECK(Chosen() == 3);
    });
    ClickRow("W M a.txt", "", 3);
    MCGuiWait("its menu", 10, [] { return MenuHas(Panel()->menu(), "Discard Changes (3 Files)…"); });
    MCGuiStep("Discard Changes (3 Files)…", [] {
        gActions = Panel()->actionsDone();
        MCGuiClick(MenuItem(Panel()->menu(), "Discard Changes (3 Files)…"));
    });
    MCGuiWait("it asks", 10, QuestionUp);
    MCGuiStep("Discard 3 Files", [] { Answer("Discard 3 Files"); });
    MCGuiWait("no changes left", 15, [] {
        return Done() && Panel()->inRepository() && RowsNow().empty();
    });
    MCGuiStep("each its own Undo Discard: the last, n.txt, first", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK_EQ_STR(MCGuiReadFile(In("a.txt")), "one\n");
        GCHECK(Panel()->errorLine().find("3 files") != std::string::npos);
        GCHECK_EQ_STR(Panel()->undoDiscardTitle(), "Undo Discard of n.txt");
        gActions = Panel()->actionsDone();
    });
    RunFromPalette("undo discard", "View > Source Control > Undo Discard");
    MCGuiWait("n.txt back", 15, [] { return Done() && RowOf("W U n.txt") >= 0; });

    // ------------------------------------------------------ the verbs
    MCGuiStep("the panel's menu has the verbs", [] { MCGuiClick(Panel()->branchLabel(), -1, -1, 3); });
    MCGuiWait("its menu", 10, [] { return MenuHas(Panel()->menu(), "Pull"); });
    MCGuiStep("Commit and Amend, Undo Last Commit, Stash, Pop Stash", [] {
        for (const char* t : {"Commit and Amend", "Undo Last Commit", "Stash", "Pop Stash"})
            GCHECK(MenuHas(Panel()->menu(), t));
        MCGuiKey("escape");
        MCGuiWriteFile(In("a.txt"), "three\n");
    });
    MCGuiWait("a.txt changed", 15, [] {
        if (RowOf("W M a.txt") < 0) Panel()->refresh();
        return RowOf("W M a.txt") >= 0 && RowWidget("W M a.txt");
    });
    ClickRow("W M a.txt");
    MCGuiWait("chosen", 10, [] { return Panel()->selectedRow() == RowOf("W M a.txt"); });
    MCGuiStep("Space", [] { MCGuiKey("space"); });
    MCGuiWait("staged", 15, [] { return RowOf("S M a.txt") >= 0; });
    MCGuiStep("a message, typed", [] { MCGuiClick(Panel()->messageView()); });
    MCGuiWait("the box has the keyboard", 10, [] { return gtk_widget_has_focus(Panel()->messageView()); });
    MCGuiStep("\"second\"", [] { MCGuiType("second"); });
    MCGuiWait("typed", 10, [] { return Panel()->message() == "second"; });
    MCGuiStep("Ctrl+Return commits", [] { MCGuiKey("ctrl+enter"); });
    MCGuiWait("committed", 15, [] { return GitText({"rev-list", "--count", "HEAD"}) == "2\n"; });
    MCGuiWait("the box empty, the commit said", 10, [] {
        return Panel()->message().empty() && Panel()->errorLine().find("second") != std::string::npos;
    });
    MCGuiStep("a new message for Commit and Amend from the palette", [] { MCGuiType("second, amended"); });
    MCGuiWait("typed", 10, [] { return Panel()->message() == "second, amended"; });
    MCGuiStep("counting", [] { gActions = Panel()->actionsDone(); });
    RunFromPalette("amend", "View > Source Control > Commit and Amend");
    MCGuiWait("amended", 15, Done);
    MCGuiStep("the last commit has the new message; still two commits", [] {
        MCGuiNote("after amend: %s", Panel()->errorLine().c_str());
        GCHECK_EQ_STR(GitText({"log", "-1", "--format=%s"}), "second, amended\n");
        GCHECK_EQ_STR(GitText({"rev-list", "--count", "HEAD"}), "2\n");
        GCHECK(Panel()->message().empty());
    });
    PanelMenu("Undo Last Commit");
    MCGuiWait("undone", 15, [] { return Done() && RowOf("S M a.txt") >= 0; });
    MCGuiStep("one commit; its change staged; its message back in the box", [] {
        MCGuiNote("after undo: %s", Panel()->errorLine().c_str());
        GCHECK_EQ_STR(GitText({"rev-list", "--count", "HEAD"}), "1\n");
        GCHECK_EQ_STR(Panel()->message(), "second, amended");
        GCHECK_EQ_STR(MCGuiReadFile(In("a.txt")), "three\n");
        Panel()->setMessage("half done");
        gActions = Panel()->actionsDone();
    });
    RunFromPalette("stash", "View > Source Control > Stash");
    MCGuiWait("stashed", 15, [] { return Done() && Panel()->inRepository() && RowsNow().empty(); });
    MCGuiStep("every change in the stash, the untracked file too", [] {
        GCHECK_EQ_STR(Panel()->errorLine(), "Stashed the changes as “half done”.");
        GCHECK(GitText({"stash", "list"}).find("half done") != std::string::npos);
        GCHECK(!MCGuiFileExists(In("n.txt")));
        GCHECK_EQ_STR(MCGuiReadFile(In("a.txt")), "one\n");
    });
    PanelMenu("Pop Stash");
    MCGuiWait("popped", 15, [] {
        return Done() && RowOf("W U n.txt") >= 0 && RowOf("W M a.txt") >= 0;
    });
    MCGuiStep("back", [] {
        GCHECK_EQ_STR(Panel()->errorLine(), "Popped the stash.");
        GCHECK_EQ_STR(MCGuiReadFile(In("a.txt")), "three\n");
    });
    PanelMenu("Pop Stash");
    MCGuiWait("a second pop is refused", 10, Done);
    MCGuiStep("said why", [] { GCHECK_EQ_STR(Panel()->errorLine(), "There is no stash to pop."); });
    CloseRepoWindow();
}
