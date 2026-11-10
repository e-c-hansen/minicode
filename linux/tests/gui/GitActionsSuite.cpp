// GitActionsSuite.cpp — `./build/minicode-guitest git_actions` (PARITY.md
// 2.9), the counterpart of the Mac's git_actions in tests/gui/
// GitPanelSuite.mm, by real keys and clicks: Stage All Changes from the
// palette and Unstage All from the panel's right click, which with nothing
// staged says so; Discard Changes asks (Cancel changes nothing), restores a
// changed file from the index, not HEAD, with its changed copy in the
// Trash, brings a deleted one back and moves an untracked one to the Trash,
// and Undo Discard puts each back; the same through the real Trash of the
// scratch world (GIO's, found again afterwards); Discard Changes from the
// palette acts on the file on show, whose clean tab follows; a file row's
// right click has its commands, a heading's and the graph's only Adjust
// Settings…, which ends every menu; the branch picker (a click on the branch,
// and the palette) switches branches, makes one from a typed name, refuses
// a bad name, and shows git's words when a switch would overwrite a change;
// a submodule's Discard is refused with what to do instead, from its menu
// (disabled, the reason as its tooltip) and the palette, its diff has no
// hunk links, and staging it still runs; a file added with git add -N and a
// conflict are refused the same way, a staged row has no Discard and is
// refused from the palette, and Stage All asks first while a conflict is
// left. The Trash is a folder of the scratch world but where the real one is
// said.
#include "GitChecks.h"

using namespace gitchecks;

namespace {

std::string R() { return MCGuiScratch() + "/gitactions"; }
const std::vector<std::string> kChanges3 = {"# Changes  3", "W M a.txt", "W D b.txt", "W U c.txt"};
int gActions = 0;

// The row's menu up, then a click on `item`.
void RowMenu(const std::string& row, const std::string& item) {
    ClickRow(row, "", 3);
    MCGuiWait("its menu has " + item, 10, [item] { return MenuHas(Panel()->menu(), item); });
    MCGuiStep(item, [item] {
        gActions = Panel()->actionsDone();
        MCGuiClick(MenuItem(Panel()->menu(), item));
    });
}

void PanelMenu(const std::string& item) {
    ClickBranch("right click on the panel", 3);
    MCGuiWait("its menu has " + item, 10, [item] { return MenuHas(Panel()->menu(), item); });
    MCGuiStep(item, [item] {
        gActions = Panel()->actionsDone();
        MCGuiClick(MenuItem(Panel()->menu(), item));
    });
}

bool Done() { return Panel()->actionsDone() > gActions; }

// The graph: the panel's list view that is not the change list.
GtkWidget* GraphView() {
    std::vector<GtkWidget*> todo = {Panel()->widget()};
    while (!todo.empty()) {
        GtkWidget* w = todo.back();
        todo.pop_back();
        if (GTK_IS_LIST_VIEW(w) && w != Panel()->list()) return w;
        for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
            todo.push_back(c);
    }
    return nullptr;
}

std::string TrashDir() { return std::string(g_get_user_data_dir()) + "/Trash"; }

}  // namespace

MC_GUI_SUITE(git_actions) {
    if (GitPath().empty()) {
        MCGuiStep("no git", [] { MCGuiNote("no git here: the git action checks are skipped"); });
        return;
    }
    Shell("a repository on main, with a branch called feature",
          "set -e; mkdir -p '" + R() + "'; cd '" + R() + "'; " + G() +
              " init -q -b main; printf 'one\\ntwo\\n' > a.txt; echo bee > b.txt; " + G() +
              " add -A; " + G() + " commit -qm first; " + G() + " checkout -q -b feature; echo f > f.txt; " +
              G() + " add -A; " + G() + " commit -qm feat; " + G() +
              " checkout -q main; printf 'one\\nTWO\\n' > a.txt; rm b.txt; echo new > c.txt");
    MCGuiStep("a Trash of our own", [] { UseScratchTrash(true); });
    OpenRepoWindow(R(), true);
    MCGuiWait("three changes", 15, [] { return Rows(kChanges3); });

    // ------------------------------------------- Stage All, Unstage All
    ClickRow("W M a.txt");   // the keyboard in the list
    RunFromPalette("stage all", "View > Source Control > Stage All Changes");
    MCGuiWait("all three staged", 15, [] {
        return Rows({"# Staged changes  3", "S M a.txt", "S D b.txt", "S A c.txt"});
    });
    MCGuiStep("it says so", [] { GCHECK_EQ_STR(Panel()->errorLine(), "Staged 3 files."); });
    PanelMenu("Unstage All Changes");
    MCGuiWait("all three back in Changes", 15, [] { return Rows(kChanges3) && Done(); });
    MCGuiStep("said", [] { GCHECK_EQ_STR(Panel()->errorLine(), "Unstaged 3 files."); });
    PanelMenu("Unstage All Changes");
    MCGuiWait("refused", 10, Done);
    MCGuiStep("with nothing staged, it says so", [] {
        GCHECK_EQ_STR(Panel()->errorLine(), "Nothing is staged.");
    });

    // ------------------------------------------------------------ Discard
    RowMenu("W M a.txt", "Discard Changes…");
    MCGuiWait("it asks", 10, QuestionUp);
    MCGuiStep("naming the file; Cancel", [] {
        GCHECK(QuestionText().find("Discard the changes to a.txt?") != std::string::npos);
        Answer("Cancel");
    });
    MCGuiWait("the question goes", 10, QuestionGone);
    MCGuiStep("nothing changed", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK_EQ_STR(MCGuiReadFile(In("a.txt")), "one\nTWO\n");
        GCHECK(Panel()->undoDiscardTitle().empty());
        GCHECK(TrashedTexts().empty());
    });
    RowMenu("W M a.txt", "Discard Changes…");
    MCGuiWait("it asks", 10, QuestionUp);
    MCGuiStep("Discard Changes", [] { Answer("Discard Changes"); });
    MCGuiWait("a.txt as committed", 15, [] {
        return Done() && Rows({"# Changes  2", "W D b.txt", "W U c.txt"});
    });
    MCGuiStep("its changed copy in the Trash; Undo Discard from the palette", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK_EQ_STR(MCGuiReadFile(In("a.txt")), "one\ntwo\n");
        GCHECK(Trashed("one\nTWO\n"));
        GCHECK(Panel()->errorLine().find("in the Trash") != std::string::npos);
        GCHECK_EQ_STR(Panel()->undoDiscardTitle(), "Undo Discard of a.txt");
        gActions = Panel()->actionsDone();
    });
    RunFromPalette("undo discard", "View > Source Control > Undo Discard");
    MCGuiWait("a.txt changed again", 15, [] { return Done() && Rows(kChanges3); });
    MCGuiStep("its text is back", [] {
        GCHECK_EQ_STR(MCGuiReadFile(In("a.txt")), "one\nTWO\n");
        GCHECK(Panel()->undoDiscardTitle().empty());
        // Nothing left to undo: the palette no longer offers it.
        GAction* a = g_action_map_lookup_action(G_ACTION_MAP(RepoWindow().window), "gitundodiscard");
        GCHECK(a && !g_action_get_enabled(a));
    });

    // The index's version, not HEAD's: what the Changes list's diff is against.
    Shell("a.txt staged as STAGED, then changed again",
          "cd '" + R() + "' && printf 'one\\nSTAGED\\n' > a.txt && " + G() +
              " add a.txt && printf 'one\\nWORK\\n' > a.txt");
    MCGuiWait("in both lists", 15, [] {
        if (RowOf("S M a.txt") < 0) Panel()->refresh();
        return Rows({"# Staged changes  1", "S M a.txt", "# Changes  3", "W M a.txt", "W D b.txt",
                     "W U c.txt"});
    });
    RowMenu("W M a.txt", "Discard Changes…");
    MCGuiWait("it asks", 10, QuestionUp);
    MCGuiStep("Discard Changes", [] { Answer("Discard Changes"); });
    MCGuiWait("only staged now", 15, [] {
        return Done() &&
               Rows({"# Staged changes  1", "S M a.txt", "# Changes  2", "W D b.txt", "W U c.txt"});
    });
    MCGuiStep("restored from the index, not from HEAD", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK_EQ_STR(MCGuiReadFile(In("a.txt")), "one\nSTAGED\n");
        GCHECK(Trashed("one\nWORK\n"));
    });
    Shell("as before", "cd '" + R() + "' && " + G() + " reset -q -- a.txt && printf 'one\\nTWO\\n' > a.txt");
    MCGuiWait("three changes again", 15, [] {
        if (!Rows(kChanges3)) Panel()->refresh();
        return Rows(kChanges3);
    });

    RowMenu("W U c.txt", "Discard (Move to Trash)…");
    MCGuiWait("it asks", 10, QuestionUp);
    MCGuiStep("to move it to the Trash", [] {
        GCHECK(QuestionText().find("Move c.txt to the Trash?") != std::string::npos);
        Answer("Move to Trash");
    });
    MCGuiWait("c.txt gone", 15, [] { return Done() && Rows({"# Changes  2", "W M a.txt", "W D b.txt"}); });
    MCGuiStep("in the Trash", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK(!MCGuiFileExists(In("c.txt")));
        GCHECK(Trashed("new\n"));
        GCHECK_EQ_STR(Panel()->undoDiscardTitle(), "Undo Discard of c.txt");
    });
    PanelMenu("Undo Discard of c.txt");
    MCGuiWait("c.txt back", 15, [] { return Done() && Rows(kChanges3); });
    MCGuiStep("its text", [] { GCHECK_EQ_STR(MCGuiReadFile(In("c.txt")), "new\n"); });

    // ------------------------------------- the real Trash of the scratch world
    // GIO's g_file_trash, and the file found again where it went. Only where
    // the scratch world's home is the Trash's, so nothing can reach the
    // owner's.
    MCGuiStep("the real Trash, the scratch world's own", [] {
        UseScratchTrash(false);
        const std::string scratch = MCGuiScratch() + "/";
        const bool ours = std::string(g_get_home_dir()).rfind(scratch, 0) == 0 &&
                          TrashDir().rfind(scratch, 0) == 0;
        GCHECK(ours);
        if (!ours) UseScratchTrash(true);   // never the owner's
    });
    RowMenu("W U c.txt", "Discard (Move to Trash)…");
    MCGuiWait("it asks", 10, QuestionUp);
    MCGuiStep("Move to Trash", [] { Answer("Move to Trash"); });
    MCGuiWait("c.txt gone", 15, [] { return Done() && Rows({"# Changes  2", "W M a.txt", "W D b.txt"}); });
    MCGuiStep("in the Trash, with what it knows of it", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK_EQ_STR(MCGuiReadFile(TrashDir() + "/files/c.txt"), "new\n");
        const std::string info = MCGuiReadFile(TrashDir() + "/info/c.txt.trashinfo");
        GCHECK(info.find("Path=") != std::string::npos && info.find("c.txt") != std::string::npos);
    });
    RunFromPalette("undo discard", "View > Source Control > Undo Discard");
    MCGuiWait("c.txt back from it", 15, [] { return Done() && Rows(kChanges3); });
    MCGuiStep("and gone from the Trash; our Trash again", [] {
        GCHECK_EQ_STR(MCGuiReadFile(In("c.txt")), "new\n");
        GCHECK(!MCGuiFileExists(TrashDir() + "/files/c.txt"));
        GCHECK(!MCGuiFileExists(TrashDir() + "/info/c.txt.trashinfo"));
        UseScratchTrash(true);
    });

    RowMenu("W D b.txt", "Discard Changes…");
    MCGuiWait("it asks", 10, QuestionUp);
    MCGuiStep("to restore it", [] {
        GCHECK(QuestionText().find("Restore b.txt?") != std::string::npos);
        Answer("Restore");
    });
    MCGuiWait("b.txt restored", 15, [] { return Done() && Rows({"# Changes  2", "W M a.txt", "W U c.txt"}); });

    // -------------------- Discard Changes from the palette: the file on show
    MCGuiStep("a.txt in the editor, with the keyboard", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK_EQ_STR(MCGuiReadFile(In("b.txt")), "bee\n");
        MCGuiOpenFile(In("a.txt"));
    });
    MCGuiWait("on show", 10, [] { return RepoWindow().editor->currentPath() == In("a.txt"); });
    ClickEditor("a click in it");
    MCGuiWait("it has the keyboard", 10, [] { return Focused(RepoWindow().editor->textView()); });
    MCGuiStep("counting", [] { gActions = Panel()->actionsDone(); });
    RunFromPalette("discard", "View > Source Control > Discard Changes…");
    MCGuiWait("it asks", 10, QuestionUp);
    MCGuiStep("about a.txt", [] {
        GCHECK(QuestionText().find("Discard the changes to a.txt?") != std::string::npos);
        Answer("Discard Changes");
    });
    MCGuiWait("a.txt discarded", 15, [] { return Done() && Rows({"# Changes  1", "W U c.txt"}); });
    MCGuiStep("as committed", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK_EQ_STR(MCGuiReadFile(In("a.txt")), "one\ntwo\n");
    });
    MCGuiWait("the clean tab follows the disk", 10, [] { return MCGuiEditorText() == "one\ntwo\n"; });

    // ---------------------------------------------- a file row's right click
    ClickRow("W U c.txt", "", 3);
    MCGuiWait("its menu", 10, [] { return MenuHas(Panel()->menu(), "Open File"); });
    MCGuiStep("its commands", [] {
        const std::vector<std::string> l = MenuLabels(Panel()->menu());
        std::string all;
        for (const std::string& s : l) all += s + " | ";
        MCGuiNote("row menu: %s", all.c_str());
        GCHECK(l == std::vector<std::string>({"Open File", "Open Changes", "Stage Changes",
                                              "Discard (Move to Trash)…", "Reveal in File Tree",
                                              "Adjust Settings…"}));
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 10, [] { return !Panel()->menu() || !gtk_widget_get_mapped(Panel()->menu()); });
    // A heading of the list and the graph have nothing of their own, as on
    // the Mac: Adjust Settings… alone.
    ClickRow("# Changes  1", "", 3);
    MCGuiWait("its menu", 10, [] { return MenuHas(Panel()->menu(), "Adjust Settings…"); });
    MCGuiStep("Adjust Settings… alone", [] {
        GCHECK(MenuLabels(Panel()->menu()) == std::vector<std::string>({"Adjust Settings…"}));
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 10, [] { return !Panel()->menu() || !gtk_widget_get_mapped(Panel()->menu()); });
    WaitOnScreen("the graph", GraphView);
    MCGuiStep("a right click on the graph", [] { MCGuiClick(GraphView(), -1, -1, 3); });
    MCGuiWait("its menu", 10, [] { return MenuHas(Panel()->menu(), "Adjust Settings…"); });
    MCGuiStep("Adjust Settings… alone there too", [] {
        GCHECK(MenuLabels(Panel()->menu()) == std::vector<std::string>({"Adjust Settings…"}));
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 10, [] { return !Panel()->menu() || !gtk_widget_get_mapped(Panel()->menu()); });
    ClickBranch("the panel's own has the whole-list commands", 3);
    MCGuiWait("its menu", 10, [] { return MenuHas(Panel()->menu(), "Pull"); });
    MCGuiStep("Stage All, Unstage All, Switch Branch; Adjust Settings… last", [] {
        for (const char* t : {"Stage All Changes", "Unstage All Changes", "Switch Branch…"})
            GCHECK(MenuHas(Panel()->menu(), t));
        const std::vector<std::string> l = MenuLabels(Panel()->menu());
        GCHECK(!l.empty() && l.back() == "Adjust Settings…");
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 10, [] { return !Panel()->menu() || !gtk_widget_get_mapped(Panel()->menu()); });

    // ---------------------------------------------------- the branch picker
    RunFromPalette("switch branch", "View > Source Control > Switch Branch…");
    MCGuiWait("the picker: main first, then feature", 10, [] {
        return Pal()->isPick() && Pal()->labels() == std::vector<std::string>({"main", "feature"});
    });
    MCGuiStep("\"feat\"", [] {
        GCHECK(!Pal()->keys().empty() && Pal()->keys()[0].rfind("current", 0) == 0);
        MCGuiType("feat");
    });
    MCGuiWait("feature first", 10, [] { return PaletteSelected() == "feature"; });
    MCGuiStep("Return", [] {
        gActions = Panel()->actionsDone();
        MCGuiKey("enter");
    });
    MCGuiWait("on feature", 15, [] { return Done() && Panel()->branchText().rfind("feature", 0) == 0; });
    MCGuiStep("its file is there; a click on the branch for a new one", [] {
        GCHECK(!Pal()->isOpen());
        GCHECK_EQ_STR(Panel()->errorLine(), "Switched to feature.");
        GCHECK(MCGuiFileExists(In("f.txt")));
    });
    ClickBranch("a click on the branch");
    MCGuiWait("the picker again", 10, [] { return Pal()->isPick() && Pal()->labels().size() == 2; });
    MCGuiStep("\"new topic\"", [] { MCGuiType("new topic"); });
    MCGuiWait("offers to make new-topic", 10, [] {
        return Pal()->labels() == std::vector<std::string>({"Create branch “new-topic” from feature"});
    });
    MCGuiStep("Return", [] {
        gActions = Panel()->actionsDone();
        MCGuiKey("enter");
    });
    MCGuiWait("on new-topic", 15, [] { return Done() && Panel()->branchText().rfind("new-topic", 0) == 0; });
    MCGuiStep("a name git would refuse is refused before it runs", [] {
        GCHECK_EQ_STR(Panel()->errorLine(), "Created new-topic and switched to it.");
    });
    MCGuiPause(0.5);   // the double-click interval: the label was clicked last
    ClickBranch("a click on the branch");
    MCGuiWait("the picker, three branches", 10, [] {
        return Pal()->isPick() && Pal()->labels().size() == 3;
    });
    MCGuiStep("\"bad..name\"", [] { MCGuiType("bad..name"); });
    MCGuiWait("its row", 10, [] { return Pal()->labels().size() == 1; });
    MCGuiStep("Return", [] {
        gActions = Panel()->actionsDone();
        MCGuiKey("enter");
    });
    MCGuiWait("refused", 10, Done);
    MCGuiStep("said why", [] {
        MCGuiNote("bad name: %s", Panel()->errorLine().c_str());
        GCHECK(Panel()->errorLine().find("cannot hold") != std::string::npos);
        GCHECK(Panel()->branchText().rfind("new-topic", 0) == 0);
    });
    Shell("a commit on new-topic, and a change to the same file left uncommitted",
          "cd '" + R() + "' && echo X > a.txt && " + G() + " commit -qam x && echo Y > a.txt");
    MCGuiPause(0.5);   // the double-click interval: the label was clicked last
    ClickBranch("switching to main would overwrite it");
    MCGuiWait("the picker", 10, [] { return Pal()->isPick() && Pal()->labels().size() == 3; });
    MCGuiStep("\"main\"", [] { MCGuiType("main"); });
    MCGuiWait("main first", 10, [] { return PaletteSelected() == "main"; });
    MCGuiStep("Return", [] {
        gActions = Panel()->actionsDone();
        MCGuiKey("enter");
    });
    MCGuiWait("git refused", 15, Done);
    MCGuiStep("git's own words; still on new-topic, the change kept", [] {
        MCGuiNote("refused switch: %s", Panel()->errorLine().c_str());
        GCHECK(Panel()->errorLine().find("would be overwritten") != std::string::npos);
        GCHECK_EQ_STR(MCGuiReadFile(In("a.txt")), "Y\n");
    });
    MCGuiWait("still on new-topic", 10, [] { return Panel()->branchText().rfind("new-topic", 0) == 0; });

    // A submodule with a change inside it: Discard trashed its whole
    // checkout and restored the gitlink, leaving an empty folder, on the Mac.
    Shell("a submodule, lib, and a change inside it",
          "set -e; mkdir -p '" + MCGuiScratch() + "/gitactions-sub'; cd '" + MCGuiScratch() +
              "/gitactions-sub'; " + G() + " init -q -b main; echo inner > x.txt; " + G() + " add -A; " +
              G() + " commit -qm sub; cd '" + R() + "'; " + G() +
              " -c protocol.file.allow=always submodule add -q '" + MCGuiScratch() +
              "/gitactions-sub' lib; " + G() + " commit -qm 'add lib'; echo changed > lib/x.txt");
    MCGuiWait("lib listed as changed", 15, [] {
        if (RowOf("W M lib") < 0) Panel()->refresh();
        return RowOf("W M lib") >= 0 && RowWidget("W M lib") != nullptr;
    });
    ClickRow("W M lib", "", 3);
    MCGuiWait("its menu", 10, [] { return MenuHas(Panel()->menu(), "Discard Changes…"); });
    MCGuiStep("Discard disabled, saying what to do instead", [] {
        GtkWidget* item = MenuItem(Panel()->menu(), "Discard Changes…");
        GCHECK(item && !gtk_widget_is_sensitive(item));
        const char* tip = item ? gtk_widget_get_tooltip_text(item) : nullptr;
        MCGuiNote("its tooltip: %s", tip ? tip : "(none)");
        GCHECK(tip && std::string(tip).find("submodule") != std::string::npos);
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 10, [] { return !Panel()->menu() || !gtk_widget_get_mapped(Panel()->menu()); });
    ClickRow("W M lib");   // its diff, the keyboard in the list
    MCGuiWait("its diff", 10, [] {
        return DiffOnShow() && Diff()->text().find("Subproject commit") != std::string::npos;
    });
    MCGuiStep("no hunk links on a gitlink's diff", [] {
        GCHECK(Diff()->links().empty());
    });
    ClickRow("W M lib");   // the keyboard back in the list
    MCGuiStep("counting", [] { gActions = Panel()->actionsDone(); });
    RunFromPalette("discard", "View > Source Control > Discard Changes…");
    MCGuiWait("refused", 10, Done);
    MCGuiStep("nothing asked, the checkout untouched", [] {
        MCGuiNote("refusal: %s", Panel()->errorLine().c_str());
        GCHECK(Panel()->errorLine().rfind("lib is a submodule", 0) == 0);
        GCHECK(QuestionGone());
        GCHECK_EQ_STR(MCGuiReadFile(In("lib/x.txt")), "changed\n");
        GCHECK(MCGuiFileExists(In("lib/.git")));
        GCHECK(!Trashed("changed\n"));
        // A hunk action on its diff is refused the same way.
        Panel()->runHunkOp(GitUi::HunkOp::Discard, 5, 5, false);
        GCHECK(Panel()->errorLine().rfind("lib is a submodule", 0) == 0);
        GCHECK_EQ_STR(MCGuiReadFile(In("lib/x.txt")), "changed\n");
    });
    ClickRow("W M lib");
    MCGuiStep("Space stages it", [] { MCGuiKey("space"); });
    MCGuiWait("add ran (a change inside a submodule stays one: nothing to stage)", 15, [] {
        return Panel()->errorLine().empty() && RowOf("W M lib") >= 0;
    });

    // ----------- refused too: a file added with git add -N, a staged row,
    // and a conflict (its three stages put in the index by hand)
    Shell("ita.txt added with -N, s.txt staged, k.txt in conflict",
          "set -e; cd '" + R() + "'; echo i > ita.txt; " + G() + " add -N ita.txt; echo s > s.txt; " +
              G() + " add s.txt; b=$(echo base | " + G() + " hash-object -w --stdin); o=$(echo ours | " +
              G() + " hash-object -w --stdin); t=$(echo theirs | " + G() +
              " hash-object -w --stdin); printf '100644 %s 1\\tk.txt\\n100644 %s 2\\tk.txt\\n100644 %s "
              "3\\tk.txt\\n' \"$b\" \"$o\" \"$t\" | " + G() +
              " update-index --index-info; echo '<<<<<<< ours' > k.txt");
    MCGuiWait("all three listed", 15, [] {
        const bool there = RowOf("W A ita.txt") >= 0 && RowOf("S A s.txt") >= 0 && RowOf("W C k.txt") >= 0;
        if (!there) Panel()->refresh();
        return there;
    });
    ClickRow("W A ita.txt", "", 3);
    MCGuiWait("its menu", 10, [] { return MenuHas(Panel()->menu(), "Discard Changes…"); });
    MCGuiStep("Discard disabled: unstage it first", [] {
        GtkWidget* item = MenuItem(Panel()->menu(), "Discard Changes…");
        GCHECK(item && !gtk_widget_is_sensitive(item));
        const char* tip = item ? gtk_widget_get_tooltip_text(item) : nullptr;
        MCGuiNote("-N's tooltip: %s", tip ? tip : "(none)");
        GCHECK(tip && std::string(tip).find("git add -N") != std::string::npos);
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 10, [] { return !Panel()->menu() || !gtk_widget_get_mapped(Panel()->menu()); });
    ClickRow("W C k.txt", "", 3);
    MCGuiWait("its menu", 10, [] { return MenuHas(Panel()->menu(), "Discard Changes…"); });
    MCGuiStep("Discard disabled: a conflict", [] {
        GtkWidget* item = MenuItem(Panel()->menu(), "Discard Changes…");
        GCHECK(item && !gtk_widget_is_sensitive(item));
        const char* tip = item ? gtk_widget_get_tooltip_text(item) : nullptr;
        MCGuiNote("conflict's tooltip: %s", tip ? tip : "(none)");
        GCHECK(tip && std::string(tip).find("has a conflict") != std::string::npos);
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 10, [] { return !Panel()->menu() || !gtk_widget_get_mapped(Panel()->menu()); });
    ClickRow("S A s.txt", "", 3);
    MCGuiWait("its menu", 10, [] { return MenuHas(Panel()->menu(), "Unstage Changes"); });
    MCGuiStep("a staged row has no Discard", [] {
        GCHECK(!MenuHas(Panel()->menu(), "Discard Changes…"));
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 10, [] { return !Panel()->menu() || !gtk_widget_get_mapped(Panel()->menu()); });
    ClickRow("S A s.txt");   // the keyboard in the list, on it
    MCGuiStep("counting", [] { gActions = Panel()->actionsDone(); });
    RunFromPalette("discard", "View > Source Control > Discard Changes…");
    MCGuiWait("refused", 10, Done);
    MCGuiStep("unstage it first, it says; nothing asked", [] {
        GCHECK_EQ_STR(Panel()->errorLine(), "s.txt is staged: unstage it first, then discard its changes.");
        GCHECK(QuestionGone());
        GCHECK_EQ_STR(MCGuiReadFile(In("s.txt")), "s\n");
        gActions = Panel()->actionsDone();
    });
    RunFromPalette("stage all", "View > Source Control > Stage All Changes");
    MCGuiWait("Stage All asks first, with a conflict left", 10, QuestionUp);
    MCGuiStep("that staging marks it resolved; Cancel", [] {
        MCGuiNote("question: %s", QuestionText().c_str());
        GCHECK(QuestionText().find("Stage 1 file that still has conflicts?") != std::string::npos);
        Answer("Cancel");
    });
    MCGuiWait("the question goes", 10, QuestionGone);
    MCGuiStep("still in conflict", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK(RowOf("W C k.txt") >= 0);
    });
    CloseRepoWindow();
}
