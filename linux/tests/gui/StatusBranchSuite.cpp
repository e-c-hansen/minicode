// StatusBranchSuite.cpp — `./build/mutible-guitest status_branch`: the status
// bar's branch item (StatusBranchItem.h), the counterpart of the Mac's checks
// in GitPanelSuite.mm. Outside a repository it is hidden; in one it shows the
// branch with the counts of an upstream ("↑0 ↓0"), gains "*" once the tree's
// status is read after an edit, counts a local commit ("↑1 ↓0"), shows the
// short hash with HEAD detached; a click on the counts syncs (the bare
// remote gets the commit, the counts go level) and a click on the name opens
// the branch picker.
#include "GitChecks.h"

#include "StatusBranchItem.h"
#include "TreeGitColors.h"

using namespace gitchecks;

namespace {

std::string Base() { return MCGuiScratch() + "/statusbranch"; }
std::string Work() { return Base() + "/work"; }
std::string Plain() { return Base() + "/plain"; }

StatusBranchItem* Item() { return RepoWindow().branchItem; }
std::string Name() { return gtk_button_get_label(GTK_BUTTON(Item()->nameButton())); }
std::string Counts() {
    return gtk_widget_get_visible(Item()->countsButton())
               ? gtk_button_get_label(GTK_BUTTON(Item()->countsButton()))
               : "";
}
bool Shown() { return Item() && gtk_widget_get_visible(Item()->widget()); }
// The tree's status read again soon, as the window coming back to the front.
void LookAgain() {
    if (RepoWindow().treeColors) RepoWindow().treeColors->lookSoon();
}

}  // namespace

MC_GUI_SUITE(status_branch) {
    if (GitPath().empty()) {
        MCGuiStep("no git", [] { MCGuiNote("no git here: the branch item checks are skipped"); });
        return;
    }
    Shell("a folder outside git, a bare remote and a clone",
          "set -e; mkdir -p '" + Plain() + "'; echo x > '" + Plain() + "/x.txt'; cd '" + Base() + "'; " +
              G() + " init -q --bare -b main remote.git; " + G() +
              " clone -q remote.git work 2>/dev/null; cd work; " + G() +
              " checkout -q -b main 2>/dev/null || true; " + G() + " config pull.rebase false; echo one > a.txt; " +
              G() + " add -A; " + G() + " commit -qm first; " + G() + " push -q -u origin main");

    // ------------------------------------------------ outside a repository
    OpenRepoWindow(Plain(), false);
    MCGuiWait("the tree's status was read", 15, [] {
        return RepoWindow().treeColors && RepoWindow().treeColors->runs() >= 1;
    });
    MCGuiStep("the item is hidden", [] {
        GCHECK(Item() != nullptr);
        GCHECK(!Shown());
    });
    CloseRepoWindow();

    // ------------------------------------------------------- in a repository
    OpenRepoWindow(Work(), false);
    MCGuiWait("the branch with its counts", 15, [] {
        return Shown() && Name() == "main" && Counts() == "↑0 ↓0";
    });
    MCGuiStep("the tip is there", [] {
        MCGuiNote("name %s counts %s", Name().c_str(), Counts().c_str());
        GCHECK(Name() == "main");
        GCHECK(Counts() == "↑0 ↓0");
        const char* tip = gtk_widget_get_tooltip_text(Item()->widget());
        GCHECK(tip && *tip);
    });
    Shell("an edit to a tracked file", "cd '" + Work() + "' && echo two >> a.txt");
    MCGuiStep("the status read again", [] { LookAgain(); });
    MCGuiWait("a star after the name", 15, [] { return Name() == "main*"; });
    Shell("committed, not pushed", "cd '" + Work() + "' && " + G() + " commit -qam mine");
    MCGuiStep("the status read again", [] { LookAgain(); });
    MCGuiWait("one to push, the star gone", 15, [] { return Name() == "main" && Counts() == "↑1 ↓0"; });
    MCGuiStep("what it shows", [] { MCGuiNote("name %s counts %s", Name().c_str(), Counts().c_str()); });

    // --------------------------------------------- a click on the counts syncs
    WaitOnScreen("the counts", [] { return Item() ? Item()->countsButton() : nullptr; });
    MCGuiStep("a click on the counts", [] { MCGuiClick(Item()->countsButton()); });
    MCGuiWait("level again", 60, [] { return Counts() == "↑0 ↓0" && !Panel()->syncing(); });
    MCGuiAsync("the remote has the commit", 20, [](MCGuiDone done) {
        MCGuiRunProcess({"/bin/sh", "-c", "cd '" + Base() + "/remote.git' && " + G() + " log --format=%s main"}, {},
                        [done](int s, std::string out, std::string) {
                            GCHECK(s == 0 && out.find("mine") != std::string::npos);
                            done();
                        });
    });

    // ---------------------------------------- a click on the name: the picker
    WaitOnScreen("the name", [] { return Item() ? Item()->nameButton() : nullptr; });
    MCGuiStep("a click on the name", [] { MCGuiClick(Item()->nameButton()); });
    MCGuiWait("the branch picker", 15, [] { return Pal() && Pal()->isOpen() && Pal()->isPick(); });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("the picker closes", 10, [] { return !Pal()->isOpen(); });

    // ----------------------------------------------------------- detached
    Shell("HEAD detached", "cd '" + Work() + "' && " + G() + " checkout -q --detach");
    MCGuiStep("the status read again", [] { LookAgain(); });
    MCGuiWait("the short hash, no counts", 15, [] {
        const std::string hash = GitText({"rev-parse", "--short", "HEAD"});
        return !hash.empty() && Name() == hash.substr(0, hash.find('\n')) && Counts().empty();
    });
    MCGuiStep("what it shows", [] { MCGuiNote("name %s counts '%s'", Name().c_str(), Counts().c_str()); });
    CloseRepoWindow();
}
