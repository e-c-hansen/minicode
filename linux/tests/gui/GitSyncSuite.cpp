// GitSyncSuite.cpp — `./build/minicode-guitest git_sync` (PARITY.md 2.9):
// Fetch, Pull, Push and Sync in the Source Control panel, and autofetch
// (git.autofetch), against a bare repository in the scratch world, the
// counterpart of the Mac's tests/gui/GitSyncSuite.mm, by real keys and
// clicks. Fetch from the palette with the panel hidden says so in the status
// bar; the panel then shows what is new on both sides, its right click has
// the four; the Sync Changes button merges and pushes without asking; a
// branch with no upstream is published from the right click's Push; the
// branch picker tracks a remote branch; Commit and Amend asks for a commit
// already pushed; a push whose ssh wants a key fails at once, in a session
// of its own (no terminal to ask on), with ssh's words and what to do;
// autofetch waits its three minutes, then the period, backs off after
// failures and recovers; a repository with no remote is never fetched.
#include "GitChecks.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>

using namespace gitchecks;

namespace {

std::string Base() { return MCGuiScratch() + "/gitsync"; }
std::string Work() { return Base() + "/work"; }
std::string Lonely() { return Base() + "/lonely"; }

// A commit made in the other clone and pushed, so `work` is behind.
std::string PushFromOther(const std::string& name) {
    return "cd '" + Base() + "/other' && " + G() + " pull -q && echo " + name + " > " + name +
           ".txt && " + G() + " add -A && " + G() + " commit -qm '" + name + "' && " + G() +
           " push -q";
}

double gT0 = 0;
double gPushed = 0;
int gActive = 1;   // the window has the keyboard, as autofetch asks
MCGuiAppWindow gWork;

std::string StatusText() {
    GtkWidget* l = RepoWindow().statusLabel;
    return l ? gtk_label_get_text(GTK_LABEL(l)) : "";
}

}  // namespace

MC_GUI_SUITE(git_sync) {
    if (GitPath().empty()) {
        MCGuiStep("no git", [] { MCGuiNote("no git here: the sync checks are skipped"); });
        return;
    }
    Shell("a bare remote, two clones, and a repository with no remote",
          "set -e; mkdir -p '" + Base() + "'; cd '" + Base() + "'; " + G() +
              " init -q --bare -b main remote.git; " + G() +
              " clone -q remote.git work 2>/dev/null; cd work; " + G() +
              " checkout -q -b main 2>/dev/null || true; " + G() +
              " config pull.rebase false; echo one > a.txt; " + G() + " add -A; " + G() +
              " commit -qm first; " + G() + " push -q -u origin main; cd ..; " + G() +
              " clone -q remote.git other; mkdir lonely; cd lonely; " + G() +
              " init -q -b main; echo x > x.txt; " + G() + " add -A; " + G() + " commit -qm alone");
    Shell("one commit on each side",
          PushFromOther("theirs") + " && cd '" + Work() + "' && echo two > a.txt && " + G() +
              " commit -qam mine");
    OpenRepoWindow(Work(), false);
    ClickEditor("a click in the editor, so the palette has the keyboard to give back");
    RunFromPalette("fetch", "View > Source Control > Fetch");
    MCGuiWait("fetched, with the panel hidden", 30, [] {
        return Panel()->lastSync() == "fetch ok";
    });
    MCGuiStep("the status bar says so", [] {
        MCGuiNote("status bar: %s", StatusText().c_str());
        GCHECK(StatusText().find("Fetched from origin.") != std::string::npos);
        MCGuiKey("ctrl+shift+g");
    });
    MCGuiWait("the panel shows one to pull and one to push", 15, [] {
        return Panel()->active() &&
               Panel()->graphSummary().find("↑ 1 to push, ↓ 1 to pull") != std::string::npos;
    });
    MCGuiStep("the button says so", [] {
        GCHECK_EQ_STR(Panel()->syncButtonTitle(), "Sync Changes  ↓ 1  ↑ 1");
        GCHECK(Panel()->errorLine().rfind("Fetched from origin", 0) == 0);
    });
    ClickBranch("the panel's right click has the four", 3);   // the panel's own menu
    MCGuiWait("the menu", 10, [] { return MenuHas(Panel()->menu(), "Pull"); });
    MCGuiStep("Pull, Push, Fetch, Sync first; Escape", [] {
        const std::vector<std::string> l = MenuLabels(Panel()->menu());
        std::string all;
        for (const std::string& s : l) all += s + " | ";
        MCGuiNote("panel menu: %s", all.c_str());
        GCHECK(l.size() >= 4 && std::vector<std::string>(l.begin(), l.begin() + 4) ==
                                    std::vector<std::string>({"Pull", "Push", "Fetch", "Sync"}));
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 10, [] { return !Panel()->menu() || !gtk_widget_get_mapped(Panel()->menu()); });
    WaitOnScreen("the Sync Changes button", [] { return Panel()->syncButton(); });
    MCGuiStep("the Sync Changes button", [] { MCGuiClick(Panel()->syncButton()); });
    MCGuiWait("synced", 60, [] { return Panel()->lastSync() == "sync ok" && !Panel()->syncing(); });
    MCGuiStep("merged and pushed, nothing asked", [] {
        MCGuiNote("sync: %s", Panel()->errorLine().c_str());
        GCHECK(Panel()->errorLine().rfind("Synced with origin/main", 0) == 0);
        GCHECK(QuestionGone());
    });
    MCGuiWait("up to date, the button gone", 15, [] {
        return Panel()->graphSummary().rfind("Up to date with origin/main", 0) == 0 &&
               Panel()->syncButtonTitle().empty();
    });
    MCGuiAsync("the remote has both commits", 20, [](MCGuiDone done) {
        MCGuiRunProcess({"/bin/sh", "-c", "cd '" + Base() + "/remote.git' && " + G() + " log --format=%s main"}, {},
                        [done](int s, std::string out, std::string) {
                            GCHECK(s == 0 && out.find("mine") != std::string::npos &&
                                   out.find("theirs") != std::string::npos);
                            done();
                        });
    });

    // ------------------------------------- amending a commit already pushed
    MCGuiStep("a message for Commit and Amend", [] { Panel()->setMessage("rewritten"); });
    ClickBranch("Commit and Amend from the right click on a pushed commit", 3);
    MCGuiWait("the menu", 10, [] { return MenuHas(Panel()->menu(), "Commit and Amend"); });
    MCGuiStep("Commit and Amend", [] { MCGuiClick(MenuItem(Panel()->menu(), "Commit and Amend")); });
    MCGuiWait("it asks first", 10, QuestionUp);
    MCGuiStep("naming the upstream; Cancel", [] {
        MCGuiNote("question: %s", QuestionText().c_str());
        GCHECK(QuestionText().find("Amend a commit that is already pushed?") != std::string::npos);
        GCHECK(QuestionText().find("origin/main") != std::string::npos);
        Answer("Cancel");
    });
    MCGuiWait("the question goes", 10, QuestionGone);
    MCGuiStep("nothing amended", [] {
        GCHECK(GitText({"log", "-1", "--format=%s"}).rfind("Merge", 0) == 0);
        GCHECK_EQ_STR(Panel()->message(), "rewritten");
        Panel()->setMessage("");
        MCGuiSetKeyWindow(nullptr);
    });

    // ------------------------------------------------- Publish Branch
    Shell("a branch of its own", "cd '" + Work() + "' && " + G() + " checkout -q -b feature && echo f > f.txt && " +
                                     G() + " add -A && " + G() + " commit -qm feature");
    MCGuiWait("Publish Branch", 15, [] {
        if (Panel()->syncButtonTitle() != "Publish Branch") Panel()->refresh();
        return Panel()->syncButtonTitle() == "Publish Branch";
    });
    ClickBranch("the right click's Push publishes it", 3);
    MCGuiWait("the menu", 10, [] { return MenuHas(Panel()->menu(), "Push"); });
    MCGuiStep("Push", [] { MCGuiClick(MenuItem(Panel()->menu(), "Push")); });
    MCGuiWait("published", 30, [] { return Panel()->lastSync() == "push ok"; });
    MCGuiAsync("origin has it, and it tracks origin/feature", 20, [](MCGuiDone done) {
        GCHECK_EQ_STR(Panel()->errorLine(), "Published feature to origin.");
        MCGuiRunProcess({"/bin/sh", "-c", "cd '" + Work() + "' && " + G() +
                                              " rev-parse --abbrev-ref @{upstream} && cd ../remote.git && " +
                                              G() + " rev-parse --verify -q refs/heads/feature"},
                        {}, [done](int s, std::string out, std::string) {
                            GCHECK(s == 0 && out.rfind("origin/feature\n", 0) == 0);
                            done();
                        });
    });

    // ---------------------------------- the branch picker: a remote branch
    Shell("a branch pushed from the other clone, fetched here",
          "cd '" + Base() + "/other' && " + G() + " checkout -q -b topic && echo t > t.txt && " + G() +
              " add -A && " + G() + " commit -qm topic && " + G() + " push -q -u origin topic && " +
              G() + " checkout -q main && cd '" + Work() + "' && " + G() + " fetch -q");
    ClickBranch("a click on the branch opens the picker");
    MCGuiWait("the picker, origin/topic among the branches", 10, [] {
        const std::vector<std::string> l = Pal()->isPick() ? Pal()->labels() : std::vector<std::string>();
        return std::find(l.begin(), l.end(), "origin/topic") != l.end();
    });
    MCGuiStep("\"topic\" and Return", [] {
        std::string all;
        for (const std::string& s : Pal()->labels()) all += s + " | ";
        MCGuiNote("picker: %s", all.c_str());
        // A remote branch whose name a local branch has is left out.
        const std::vector<std::string> l = Pal()->labels();
        GCHECK(std::find(l.begin(), l.end(), "origin/main") == l.end());
        GCHECK(l.size() >= 1 && l[0] == "feature");
        MCGuiType("topic");
    });
    MCGuiWait("origin/topic first", 10, [] {
        return PaletteSelected() == "origin/topic";
    });
    MCGuiStep("its detail; Return", [] {
        const std::vector<std::string> k = Pal()->keys();
        GCHECK(!k.empty() && k[0].rfind("remote · ", 0) == 0);
        MCGuiKey("enter");
    });
    MCGuiWait("on topic, tracking origin/topic", 15, [] {
        return Panel()->branchText().rfind("topic", 0) == 0;
    });
    MCGuiStep("it says so", [] {
        GCHECK_EQ_STR(Panel()->errorLine(), "Created topic and switched to it.");
        GCHECK_EQ_STR(GitText({"rev-parse", "--abbrev-ref", "@{upstream}"}), "origin/topic\n");
    });
    Shell("back on feature", "cd '" + Work() + "' && " + G() + " checkout -q feature");

    // ------------------------------------- a credential it cannot ask for
    MCGuiStep("a push URL over ssh whose key needs a passphrase", [] {
        const std::string ssh = Base() + "/fake-ssh";
        // Notes its session, asks the terminal, as ssh does, which only works
        // where there is one; then says what ssh says without a key.
        MCGuiWriteFile(ssh, "#!/bin/sh\nps -o sid= -p $$ > '" + Base() + "/ssh-session'\n"
                            "(read -r answer < /dev/tty) 2>/dev/null\n"
                            "echo 'git@example.invalid: Permission denied (publickey).' >&2\n"
                            "exit 255\n");
        chmod(ssh.c_str(), 0755);
        MCGuiWriteFile(Work() + "/f.txt", "changed\n");
    });
    Shell("configured", "cd '" + Work() + "' && " + G() + " config core.sshCommand '" + Base() +
                            "/fake-ssh' && " + G() +
                            " remote set-url --push origin ssh://git@example.invalid/x.git && " + G() +
                            " commit -qam again");
    ClickEditor("a click in the editor, for Push from the palette");
    MCGuiStep("the time", [] { gPushed = MCGuiNowMs(); });
    RunFromPalette("push", "View > Source Control > Push");
    MCGuiWait("refused", 30, [] { return Panel()->lastSync() == "push failed"; });
    MCGuiStep("at once, with ssh's words and what to do, ssh in a session of its own", [] {
        const double ms = MCGuiNowMs() - gPushed;
        MCGuiNote("push failed after %.1f s: %s", ms / 1000, Panel()->errorLine().c_str());
        GCHECK(ms < 15000);
        GCHECK(Panel()->errorLine().find("Permission denied (publickey)") != std::string::npos);
        GCHECK(Panel()->errorLine().find("add the key to the agent (ssh-add) or connect once from "
                                       "the terminal.") != std::string::npos);
        GCHECK(QuestionGone());
        // Not minicode's session (as the Mac's POSIX_SPAWN_SETSID), so no
        // terminal of minicode's for ssh to ask on.
        const std::string sid = MCGuiReadFile(Base() + "/ssh-session");
        MCGuiNote("ssh's session %s, minicode's %d", sid.c_str(), (int)getsid(0));
        GCHECK(!sid.empty() && atoi(sid.c_str()) != (int)getsid(0));
    });
    Shell("the push URL put back", "cd '" + Work() + "' && " + G() +
                                       " config --unset remote.origin.pushurl && " + G() +
                                       " config --unset core.sshCommand");

    // ------------------------------------------------------- autofetch
    Shell("another commit on the remote's main", PushFromOther("later"));
    MCGuiStep("autofetch off: nothing", [] {
        Panel()->isActive = [] { return gActive != 0; };
        gT0 = g_get_monotonic_time() / 1e6 + 100000;
        GCHECK(!Panel()->autoFetchTickAt(gT0));
        GCHECK(Panel()->autoFetches() == 0);
        MCGuiSetSettings("lsp.enabled = false\ngit.autofetch = true\n");
        gActive = 0;
        GCHECK(!Panel()->autoFetchTickAt(gT0));   // the window in the background
        gActive = 1;
        // The window has the keyboard: due, as nothing was fetched by it yet
        // at this (later) time.
        GCHECK(Panel()->autoFetchTickAt(gT0));
    });
    MCGuiWait("fetched in the background", 30, [] {
        return Panel()->autoFetches() == 1 && !Panel()->syncing();
    });
    MCGuiStep("quietly; not again within three minutes", [] {
        GCHECK_EQ_STR(Panel()->lastSync(), "fetch ok");
        GCHECK(!Panel()->autoFetchTickAt(gT0 + 60));
        GCHECK(!Panel()->autoFetchTickAt(gT0 + 179));
        GCHECK(Panel()->autoFetchTickAt(gT0 + 181));
    });
    MCGuiWait("the second", 30, [] { return Panel()->autoFetches() == 2 && !Panel()->syncing(); });
    MCGuiStep("the panel again; a period of ten minutes", [] {
        MCGuiKey("ctrl+shift+g");   // the tree again
    });
    MCGuiWait("the tree", 10, [] { return !Panel()->active(); });
    MCGuiStep("and the panel, refreshed", [] { MCGuiKey("ctrl+shift+g"); });
    MCGuiWait("the panel", 10, [] { return Panel()->active(); });
    MCGuiStep("a period of ten minutes waits ten minutes", [] {
        MCGuiSetSettings("lsp.enabled = false\ngit.autofetch = true\ngit.autofetch-period = 600\n");
        GCHECK(!Panel()->autoFetchTickAt(gT0 + 181 + 400));
        GCHECK(Panel()->autoFetchTickAt(gT0 + 181 + 600));
    });
    MCGuiWait("the third", 30, [] { return Panel()->autoFetches() == 3 && !Panel()->syncing(); });

    // ------------------------------------------- failures back off
    Shell("the remote gone", "cd '" + Work() + "' && " + G() + " remote set-url origin '" + Base() +
                                 "/missing.git'");
    MCGuiStep("due after the period", [] { GCHECK(Panel()->autoFetchTickAt(gT0 + 781 + 600)); });
    MCGuiWait("failed, quietly but said", 30, [] {
        return Panel()->lastSync() == "fetch failed" && !Panel()->syncing();
    });
    MCGuiStep("under the button; the next waits twice the period", [] {
        MCGuiNote("autofetch failure: %s", Panel()->errorLine().c_str());
        GCHECK(Panel()->errorLine().rfind("Autofetch: ", 0) == 0);
        GCHECK(Panel()->autoFetches() == 4);
        const double t = gT0 + 1381;
        GCHECK(!Panel()->autoFetchTickAt(t + 600));
        GCHECK(!Panel()->autoFetchTickAt(t + 1199));
        GCHECK(Panel()->autoFetchTickAt(t + 1200));
    });
    MCGuiWait("failed again", 30, [] { return Panel()->autoFetches() == 5 && !Panel()->syncing(); });
    MCGuiStep("then four times; the remote back", [] {
        const double t = gT0 + 1381 + 1200;
        GCHECK(!Panel()->autoFetchTickAt(t + 1200));
        GCHECK(!Panel()->autoFetchTickAt(t + 2399));
    });
    Shell("the remote back", "cd '" + Work() + "' && " + G() + " remote set-url origin '" + Base() +
                                 "/remote.git'");
    MCGuiStep("due after the doubled wait", [] {
        GCHECK(Panel()->autoFetchTickAt(gT0 + 1381 + 1200 + 2400));
    });
    MCGuiWait("fetched", 30, [] {
        return Panel()->autoFetches() == 6 && !Panel()->syncing() && Panel()->lastSync() == "fetch ok";
    });
    MCGuiStep("its failure taken away; the period again", [] {
        GCHECK(Panel()->errorLine().rfind("Autofetch: ", 0) != 0);
        const double t = gT0 + 1381 + 1200 + 2400;
        GCHECK(!Panel()->autoFetchTickAt(t + 599));
        gWork = RepoWindow();
    });

    // ------------------------------------------- no remote: never fetched
    OpenRepoWindow(Lonely(), false);
    MCGuiStep("autofetch looks, once the wait is over", [] {
        Panel()->isActive = [] { return true; };
        GCHECK(Panel()->autoFetchTickAt(gT0));
    });
    MCGuiWait("looked", 20, [] { return !Panel()->lastSync().empty() && !Panel()->syncing(); });
    MCGuiStep("nothing fetched, nothing said, and not looked at again soon", [] {
        GCHECK_EQ_STR(Panel()->lastSync(), "fetch refused");
        GCHECK(Panel()->autoFetches() == 0);
        GCHECK(Panel()->errorLine().empty());
        GCHECK(!Panel()->autoFetchTickAt(gT0 + 60));
    });
    ClickEditor("a click in the editor, for Fetch from the palette");
    RunFromPalette("fetch", "View > Source Control > Fetch");
    MCGuiWait("refused by hand", 20, [] {
        return Panel()->lastSync() == "fetch refused" && !Panel()->errorLine().empty();
    });
    MCGuiStep("with the reason", [] {
        GCHECK_EQ_STR(Panel()->errorLine(), "This repository has no remote to fetch from.");
        MCGuiSetSettings("lsp.enabled = false\n");
        GCHECK(gWork.git && !gWork.git->autoFetchTickAt(gT0 + 100000));   // off again
    });
    CloseRepoWindow();
    MCGuiStep("and the clone's", [] {
        Repo() = Work();
        if (GtkWindow* w = RepoWindow().window) gtk_window_close(w);
    });
    MCGuiWait("gone", 10, [] { return RepoWindow().window == nullptr; });
    MCGuiStep("the first window has the keyboard", [] { MCGuiSetKeyWindow(nullptr); });
}
