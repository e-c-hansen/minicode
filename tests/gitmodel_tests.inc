// gitmodel_tests.inc — the `git-ui:*` groups of run_tests.cpp: GitModel.h,
// the Source Control panels' model (the Mac's, the GTK port's and
// Android's), kept in their own file and included there. They were the GTK
// port's tests (linux/tests/run_tests.cpp) until GitModel moved to src/.
#include "GitModel.h"

// The characters of a UTF-8 string, one string per character, so a test
// can rebuild a run from its CHARACTER offsets.
static std::vector<std::string> gitUiChars(const std::string &s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        size_t len = 1;
        unsigned char c = (unsigned char)s[i];
        if (c >= 0xF0)      len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        if (i + len > s.size()) len = s.size() - i;
        out.push_back(s.substr(i, len));
        i += len;
    }
    return out;
}
static std::string nul(std::initializer_list<std::string> recs) {
    std::string out;
    for (const std::string& r : recs) { out += r; out += '\0'; }
    return out;
}

static bool hasArg(const std::vector<std::string>& a, const std::string& x) {
    for (const std::string& s : a) if (s == x) return true;
    return false;
}

// The index of `x` in `a`, or -1.
static int argAt(const std::vector<std::string>& a, const std::string& x) {
    for (size_t i = 0; i < a.size(); ++i) if (a[i] == x) return (int)i;
    return -1;
}

// The text a style run covers, counted in characters as GtkTextBuffer does.
static std::string runText(const GitUi::StyledText& t, size_t i) {
    std::vector<std::string> cs = gitUiChars(t.text);
    std::string out;
    for (size_t k = t.runs[i].start; k < t.runs[i].start + t.runs[i].length && k < cs.size(); ++k)
        out += cs[k];
    return out;
}

static GitUi::Style styleOf(const GitUi::StyledText& t, const std::string& piece) {
    for (size_t i = 0; i < t.runs.size(); ++i)
        if (runText(t, i).find(piece) != std::string::npos) return t.runs[i].style;
    return GitUi::Style::Plain;
}

static void testGitModel() {
    using namespace GitUi;
    const std::string h40 = std::string(40, 'a'), z40 = std::string(40, '0');
    const std::string modes = " 100644 100644 100644 ";

    GROUP("git-ui:rows");
    Snapshot s;
    s.inRepository = true;
    applyStatus(s, Git::parseStatus(nul({
        "# branch.oid " + h40, "# branch.head main", "# branch.upstream origin/main",
        "# branch.ab +2 -1",
        "1 .M N..." + modes + h40 + " " + h40 + " src/main.cpp",
        "1 MM N..." + modes + h40 + " " + h40 + " both.txt",
        "2 R. N..." + modes + h40 + " " + h40 + " R100 new name.txt", "old name.txt",
        "? sp*ecial.txt"})));
    CHECK(s.branchText == "main \xe2\x86\x91" "2 \xe2\x86\x93" "1");
    CHECK(s.rows.size() == 7);   // 2 headings, 2 staged, 3 changes
    CHECK(s.rows[0].header && s.rows[0].title == "Staged changes  2" && s.rows[0].staged);
    CHECK(s.rows[1].path == "both.txt" && s.rows[1].staged && s.rows[1].letter == 'M');
    CHECK(s.rows[2].path == "new name.txt" && s.rows[2].origPath == "old name.txt" &&
          s.rows[2].letter == 'R');
    CHECK(s.rows[3].header && s.rows[3].title == "Changes  3" && !s.rows[3].staged);
    CHECK(s.rows[4].path == "src/main.cpp" && s.rows[4].letter == 'M');
    CHECK(s.rows[5].path == "both.txt" && !s.rows[5].staged);   // in both lists
    CHECK(s.rows[6].untracked && s.rows[6].letter == 'U');
    CHECK(s.notice.empty());
    CHECK(rowToolTip(s.rows[2]) == "new name.txt (renamed from old name.txt), staged");
    CHECK(rowToolTip(s.rows[6]) == "sp*ecial.txt, untracked");
    CHECK(rowToolTip(s.rows[0]).empty());
    CHECK(letterColor('M', false) == 0xE2C08D && letterColor('A', false) == 0x81B88B);
    CHECK(letterColor('C', true) == 0xE4676B && letterColor('D', false) == 0xC74E39);

    Snapshot clean;
    clean.inRepository = true;
    applyStatus(clean, Git::parseStatus(nul({"# branch.oid " + h40, "# branch.head main"})));
    CHECK(clean.rows.empty() && clean.notice == "No changes.");
    Snapshot empty;
    empty.inRepository = true;
    applyStatus(empty, Git::parseStatus(nul({"# branch.oid (initial)", "# branch.head main",
                                             "1 A. N... 000000 100644 100644 " + z40 + " " +
                                                 h40 + " a.txt"})));
    CHECK(empty.initial && empty.branchText == "main  (no commits yet)");
    CHECK(!graphWanted(empty));
    CHECK(graphWanted(s) && graphWanted(clean));
    Snapshot failed = clean;
    failed.errorText = "fatal: index file corrupt";
    CHECK(!graphWanted(failed));

    GROUP("git-ui:toplevel");
    std::string top, gitDir;
    CHECK(parseTopLevel("/tmp/r\n/tmp/r/.git\n", top, gitDir) && top == "/tmp/r" &&
          gitDir == "/tmp/r/.git");
    CHECK(parseTopLevel("/tmp/a b\n", top, gitDir) && top == "/tmp/a b" && gitDir.empty());
    CHECK(!parseTopLevel("", top, gitDir));

    GROUP("git-ui:keys");
    const std::vector<Row>& r = s.rows;
    CHECK(nextFileRow(r, -1, 1) == 1);      // Down with nothing selected: the first file
    CHECK(nextFileRow(r, -1, -1) == 6);     // Up: the last
    CHECK(nextFileRow(r, 2, 1) == 4);       // over the "Changes" heading
    CHECK(nextFileRow(r, 4, -1) == 2);
    CHECK(nextFileRow(r, 6, 1) == -1);      // past the end: the graph's turn
    CHECK(nextFileRow(r, 1, -1) == -1);
    CHECK(lastFileRow(r) == 6 && lastFileRow(clean.rows) == -1);

    GROUP("git-ui:selection-after-refresh");
    // Space on src/main.cpp in Changes: it moves to Staged. The selection
    // stays at the same place in Changes, so Space can go down the list.
    Snapshot after;
    after.inRepository = true;
    applyStatus(after, Git::parseStatus(nul({
        "# branch.oid " + h40, "# branch.head main",
        "1 M. N..." + modes + h40 + " " + h40 + " src/main.cpp",
        "1 MM N..." + modes + h40 + " " + h40 + " both.txt",
        "2 R. N..." + modes + h40 + " " + h40 + " R100 new name.txt", "old name.txt",
        "? sp*ecial.txt"})));
    // Staged: main.cpp, both, new name; Changes: both, sp*ecial.
    CHECK(after.rows.size() == 7);
    CHECK(pickAfterRefresh(r, 4, after.rows) == 5 && after.rows[5].path == "both.txt" &&
          !after.rows[5].staged);
    CHECK(pickAfterRefresh(r, 6, after.rows) == 6);   // the same file, still there
    CHECK(pickAfterRefresh(r, 1, after.rows) == 2);   // both.txt in Staged
    CHECK(pickAfterRefresh(r, 3, after.rows) == -1);  // a heading was never selected
    CHECK(pickAfterRefresh(r, -1, after.rows) == -1);
    // The last file of a list left it: the list's last row now.
    std::vector<Row> one = {r[3], r[6]};
    std::vector<Row> none = {after.rows[0], after.rows[1]};
    CHECK(pickAfterRefresh(one, 1, none) == 1);   // nothing left in Changes: first file
    CHECK(pickAfterRefresh(one, 1, clean.rows) == -1);

    GROUP("git-ui:arguments");
    auto stage = stageArgs(r[6], false);
    CHECK(stage[0] == "--literal-pathspecs" && stage[1] == "add" && hasArg(stage, "-A"));
    CHECK(argAt(stage, "--") >= 0 && stage.back() == "sp*ecial.txt" &&
          argAt(stage, "--") < argAt(stage, "sp*ecial.txt"));
    auto unstage = stageArgs(r[2], false);
    CHECK(unstage[1] == "restore" && hasArg(unstage, "--staged"));
    CHECK(unstage[unstage.size() - 2] == "new name.txt" && unstage.back() == "old name.txt");
    auto rmc = stageArgs(r[1], true);
    CHECK(rmc[0] == "--literal-pathspecs" && rmc[1] == "rm" && hasArg(rmc, "--cached") &&
          hasArg(rmc, "--force") && rmc.back() == "both.txt");
    auto d1 = diffArgs(r[6]);
    CHECK(hasArg(d1, "--no-index") && d1[d1.size() - 2] == "/dev/null" &&
          d1.back() == "sp*ecial.txt");
    auto d2 = diffArgs(r[2]);
    CHECK(hasArg(d2, "--cached") && hasArg(d2, "-M") && d2.back() == "old name.txt");
    auto d3 = diffArgs(r[4]);
    CHECK(!hasArg(d3, "--cached") && d3.back() == "src/main.cpp" && hasArg(d3, "--no-ext-diff"));
    CHECK(hasArg(d3, "--literal-pathspecs") && hasArg(d3, "--src-prefix=a/"));
    CHECK(hasArg(d1, "--submodule=short") && hasArg(d2, "--submodule=short") &&
          hasArg(d3, "--submodule=short"));   // whatever diff.submodule says
    auto lg = logArgs(200, false, true);
    CHECK(hasArg(lg, "--topo-order") && hasArg(lg, "-z") && hasArg(lg, "--max-count=200"));
    CHECK(hasArg(lg, "@{upstream}") && !hasArg(lg, "--branches") && lg.back() == "--");
    auto lgAll = logArgs(400, true, false);
    CHECK(hasArg(lgAll, "--branches") && hasArg(lgAll, "--remotes") &&
          !hasArg(lgAll, "@{upstream}") && hasArg(lgAll, "--max-count=400"));
    auto sh = showArgs(h40);
    CHECK(hasArg(sh, "--diff-merges=first-parent") && hasArg(sh, "--stat") &&
          sh[sh.size() - 2] == h40 && sh.back() == "--");
    CHECK(commitArgs("a\nb") == std::vector<std::string>({"commit", "-m", "a\nb"}));
    CHECK(statusArgs()[0] == "status" && hasArg(statusArgs(), "--porcelain=v2"));
    CHECK(leftRightArgs()[2] == "HEAD...@{upstream}");
    // Only the commands the Mac runs.
    for (const auto& a : {stage, unstage, rmc, d1, d2, d3, lg, sh, statusArgs(), refArgs(),
                          topLevelArgs(), leftRightArgs(), commitArgs("x")}) {
        size_t i = 0;
        while (i < a.size() && (a[i] == "-c" || a[i] == "--literal-pathspecs")) i += a[i] == "-c" ? 2 : 1;
        const std::string cmd = i < a.size() ? a[i] : "";
        CHECK(cmd == "add" || cmd == "restore" || cmd == "rm" || cmd == "diff" ||
              cmd == "log" || cmd == "show" || cmd == "status" || cmd == "for-each-ref" ||
              cmd == "rev-parse" || cmd == "rev-list" || cmd == "commit");
    }
    bool locks = false;
    for (const EnvVar& e : gitEnvironment())
        if (std::string(e.name) == "GIT_OPTIONAL_LOCKS" && std::string(e.value) == "0") locks = true;
    CHECK(locks && gitEnvironment().size() == 4);

    GROUP("git-ui:summary");
    CHECK(summaryText(s) == "\xe2\x86\x91 2 to push, \xe2\x86\x93 1 to pull, against origin/main");
    Snapshot t = s;
    t.ahead = 0; t.behind = 0;
    CHECK(summaryText(t) == "Up to date with origin/main.");
    t.hasAheadBehind = false;
    CHECK(summaryText(t) == "The upstream origin/main is gone.");
    t.upstream.clear();
    CHECK(summaryText(t) == "main has no upstream, so nothing here is marked as pushed or not.");
    t.detached = true;
    CHECK(summaryText(t) == "HEAD is detached, so there is no upstream to compare with.");
    CHECK(compareWithUpstream(s) && !compareWithUpstream(t));

    GROUP("git-ui:sync");
    {
        CHECK((parseRemotes("origin\nupstream\n\n-x\nweird name\n") ==
               std::vector<std::string>{"origin", "upstream"}));
        CHECK(remoteArgs() == std::vector<std::string>{"remote"});
        Snapshot u;
        u.inRepository = true;
        u.branch = "main";
        u.headOid = h40;
        u.upstream = "origin/main";
        u.hasAheadBehind = true;
        u.ahead = 2;
        u.behind = 1;
        u.remotes = {"fork", "origin"};
        CHECK(upstreamRemote(u) == "origin" && publishRemote(u) == "origin");
        SyncPlan sync = syncPlan(SyncOp::Sync, u);
        CHECK(sync.refusal.empty() && sync.fetches);
        CHECK((sync.steps == std::vector<std::vector<std::string>>{{"pull"}, {"push"}}));
        CHECK(sync.busyText == "Syncing with origin/main\xe2\x80\xa6");
        CHECK(syncPlan(SyncOp::Pull, u).steps == std::vector<std::vector<std::string>>{{"pull"}});
        SyncPlan push = syncPlan(SyncOp::Push, u);
        CHECK(push.steps == std::vector<std::vector<std::string>>{{"push"}} && !push.fetches);
        CHECK(push.doneText == "Pushed to origin/main.");
        // Never forced, whatever the plan.
        for (SyncOp op : {SyncOp::Fetch, SyncOp::Pull, SyncOp::Push, SyncOp::Sync})
            for (const auto& step : syncPlan(op, u).steps)
                CHECK(!hasArg(step, "--force") && !hasArg(step, "-f") &&
                      !hasArg(step, "--force-with-lease"));
        CHECK((syncPlan(SyncOp::Fetch, u).steps ==
               std::vector<std::vector<std::string>>{{"fetch", "origin"}}));
        // The upstream on a remote whose name holds a slash: the longest fits.
        Snapshot sl = u;
        sl.remotes = {"team", "team/eu"};
        sl.upstream = "team/eu/main";
        CHECK(upstreamRemote(sl) == "team/eu");
        CHECK(syncButtonTitle(u) == "Sync Changes  \xe2\x86\x93 1  \xe2\x86\x91 2");
        CHECK(syncButtonToolTip(u) == "Pull 1 commit, then push 2 commits from and to origin/main");
        Snapshot ahead = u;
        ahead.behind = 0;
        CHECK(syncButtonTitle(ahead) == "Sync Changes  \xe2\x86\x91 2");
        CHECK(syncButtonToolTip(ahead) == "Push 2 commits to origin/main");
        ahead.ahead = 0;
        CHECK(syncButtonTitle(ahead).empty());   // up to date
        // No upstream: published to origin (or the only remote).
        Snapshot nu = u;
        nu.upstream.clear();
        nu.hasAheadBehind = false;
        nu.remotes = {"origin", "fork"};
        CHECK(syncButtonTitle(nu) == "Publish Branch");
        CHECK((syncPlan(SyncOp::Push, nu).steps ==
               std::vector<std::vector<std::string>>{{"push", "--set-upstream", "origin", "main"}}));
        CHECK(syncPlan(SyncOp::Sync, nu).doneText == "Published main to origin.");
        CHECK(syncPlan(SyncOp::Pull, nu).refusal == "main has no upstream branch to pull from.");
        CHECK((syncPlan(SyncOp::Fetch, nu).steps ==
               std::vector<std::vector<std::string>>{{"fetch", "origin"}}));
        nu.remotes = {"fork"};
        CHECK(publishRemote(nu) == "fork" && syncButtonTitle(nu) == "Publish Branch");
        CHECK((syncPlan(SyncOp::Fetch, nu).steps ==
               std::vector<std::vector<std::string>>{{"fetch", "fork"}}));
        nu.remotes = {"fork", "work"};
        CHECK(publishRemote(nu).empty() && syncButtonTitle(nu).empty());
        CHECK(syncPlan(SyncOp::Push, nu).steps.empty() &&
              syncPlan(SyncOp::Push, nu).refusal.find("git push -u <remote> main") !=
                  std::string::npos);
        nu.remotes.clear();
        CHECK(syncButtonTitle(nu).empty());
        CHECK(syncPlan(SyncOp::Push, nu).refusal == "This repository has no remote to push to.");
        CHECK(syncPlan(SyncOp::Fetch, nu).refusal ==
              "This repository has no remote to fetch from.");
        // Detached, gone, no commit yet, not a repository.
        Snapshot d = u;
        d.detached = true;
        d.branch.clear();
        CHECK(syncButtonTitle(d).empty() && syncPlan(SyncOp::Push, d).steps.empty());
        CHECK(syncPlan(SyncOp::Fetch, d).steps ==
              (std::vector<std::vector<std::string>>{{"fetch", "origin"}}));
        Snapshot gone = u;
        gone.hasAheadBehind = false;
        CHECK(syncButtonTitle(gone).empty());
        CHECK(syncPlan(SyncOp::Sync, gone).refusal.find("is gone") != std::string::npos);
        Snapshot init = u;
        init.initial = true;
        CHECK(syncButtonTitle(init).empty() && syncPlan(SyncOp::Pull, init).steps.empty());
        CHECK(syncPlan(SyncOp::Sync, Snapshot()).refusal ==
              "This folder is not in a git repository.");
        // What a credential failure says.
        std::string https = networkFailureText(
            128, "", "fatal: could not read Username for 'https://github.com': terminal prompts disabled\n");
        CHECK(https.find("terminal prompts disabled\nminicode never asks for a password") !=
              std::string::npos);
        std::string ssh = networkFailureText(
            128, "", "git@github.com: Permission denied (publickey).\nfatal: Could not read from "
                     "remote repository.\n");
        CHECK(ssh.find("ssh-add --apple-use-keychain") != std::string::npos);
        CHECK(networkFailureText(1, "", "! [rejected] main -> main (fetch first)\n") ==
              "! [rejected] main -> main (fetch first)");
    }

    GROUP("git-ui:autofetch");
    {
        CHECK(autoFetchWait(180, 0) == 180);
        CHECK(autoFetchWait(10, 0) == 180);      // never more often than 3 minutes
        CHECK(autoFetchWait(600, 0) == 600);
        CHECK(autoFetchWait(180, 1) == 360 && autoFetchWait(180, 2) == 720);
        CHECK(autoFetchWait(180, 5) == 3600 && autoFetchWait(180, 40) == 3600);
        CHECK(autoFetchWait(7200, 3) == 7200);
        Snapshot r;
        r.inRepository = true;
        r.remotes = {"origin"};
        AutoFetchState st;
        CHECK(autoFetchDue(true, true, r, false, st, 1000, 180));      // never fetched
        CHECK(!autoFetchDue(false, true, r, false, st, 1000, 180));    // off
        CHECK(!autoFetchDue(true, false, r, false, st, 1000, 180));    // window in the background
        CHECK(!autoFetchDue(true, true, r, true, st, 1000, 180));      // something runs
        Snapshot none = r;
        none.remotes.clear();
        CHECK(!autoFetchDue(true, true, none, false, st, 1000, 180));  // no remote
        CHECK(!autoFetchDue(true, true, Snapshot(), false, st, 1000, 180));
        st.lastStart = 1000;
        CHECK(!autoFetchDue(true, true, r, false, st, 1179, 180));
        CHECK(autoFetchDue(true, true, r, false, st, 1180, 180));
        CHECK(!autoFetchDue(true, true, r, false, st, 1100, 10));      // clamped to 180
        st.failures = 2;
        CHECK(!autoFetchDue(true, true, r, false, st, 1700, 180));
        CHECK(autoFetchDue(true, true, r, false, st, 1720, 180));
    }

    GROUP("git-ui:failure-text");
    CHECK(failureText(1, "", "\nAborting commit due to empty commit message.\n\n") ==
          "Aborting commit due to empty commit message.");
    CHECK(failureText(1, "On branch main\nChanges not staged:\n\n"
                         "no changes added to commit (use \"git add\")\n", "") ==
          "no changes added to commit (use \"git add\")");
    CHECK(failureText(128, "", "") == "git exited with status 128.");
    CHECK(failureText(1, "", "a\n  \nb") == "a\nb");
    CHECK(firstLine("[main 1a2b3c4] Subject\n 1 file changed\n") == "[main 1a2b3c4] Subject");

    GROUP("git-ui:utf8");
    CHECK(validUtf8("caf\xc3\xa9") == "caf\xc3\xa9");
    CHECK(validUtf8("caf\xe9") == "caf\xc3\xa9");           // Latin-1 read as such
    CHECK(validUtf8("a\xc3") == "a\xc3\x83");               // cut sequence
    CHECK(validUtf8(std::string("a\0b", 3)) == "ab");       // no NULs in a text buffer
    CHECK(validUtf8("\xed\xa0\x80") != "\xed\xa0\x80");     // a surrogate is not UTF-8
    CHECK(utf8Length("h\xc3\xa9\xe2\x86\x91") == 3);

    GROUP("git-ui:diff-text");
    const std::string diff =
        "diff --git a/x.txt b/x.txt\nindex 1..2 100644\n--- a/x.txt\n+++ b/x.txt\n"
        "@@ -1,2 +1,2 @@\n ctx \xc3\xa9\n-old\n+new\n\\ No newline at end of file\n";
    StyledText dt = diffText(diff);
    CHECK(dt.text == diff);
    CHECK(styleOf(dt, "diff --git") == Style::FileHeader);
    CHECK(styleOf(dt, "@@ -1,2") == Style::Muted);
    CHECK(styleOf(dt, "-old") == Style::Removed);
    CHECK(styleOf(dt, "+new") == Style::Added);
    CHECK(styleOf(dt, "No newline") == Style::Muted);
    // Runs are in characters: "é" is one, so everything after it lines up.
    bool found = false;
    for (size_t i = 0; i < dt.runs.size(); ++i)
        if (dt.runs[i].style == Style::Removed) found = runText(dt, i) == "-old\n";
    CHECK(found);
    size_t total = 0;
    for (const auto& run : dt.runs) total += run.length;
    CHECK(total == dt.chars && dt.chars == utf8Length(diff));
    CHECK(diffText("").text == "No differences to show.");
    std::string huge;
    while (huge.size() <= kMaxDiffBytes) huge += "+" + std::string(99, 'x') + "\n";
    StyledText cut = diffText("@@ -0,0 +1,50000 @@\n" + huge);
    CHECK(cut.text.size() < kMaxDiffBytes + 200);
    CHECK(cut.text.find("longer than 4 MB") != std::string::npos);
    CHECK(styleOf(cut, "longer than 4 MB") == Style::Muted);
    StyledText latin = diffText("+caf\xe9\n");
    CHECK(latin.text == "+caf\xc3\xa9\n");

    GROUP("git-ui:commit-text");
    const std::string p1 = std::string(40, '1'), p2 = std::string(40, '2');
    std::string show = h40 + '\0' + p1 + " " + p2 + '\0' + "Ann" + '\0' + "ann@x.org" + '\0' +
                       "1700000000" + '\0' + "Bob" + '\0' + "bob@x.org" + '\0' +
                       "Merge branch 'feature'\n\nBody line.\n" + '\0' +
                       "\n---\n x.txt | 1 +\n\n" + diff;
    StyledText ct = commitText(show, 1700000000 + 3 * 86400);
    CHECK(ct.text.find("commit    " + h40) == 0);
    CHECK(styleOf(ct, h40) == Style::Hash);
    CHECK(ct.text.find("Merge     1111111 2222222\n") != std::string::npos);
    CHECK(ct.text.find("Author    Ann <ann@x.org>\n") != std::string::npos);
    CHECK(ct.text.find("Committer Bob <bob@x.org>\n") != std::string::npos);
    CHECK(ct.text.find("(3 days ago)") != std::string::npos);
    CHECK(styleOf(ct, "Merge branch 'feature'") == Style::Bold);
    CHECK(ct.text.find("\n\nBody line.") != std::string::npos);
    CHECK(ct.text.find("against the first parent") != std::string::npos);
    CHECK(styleOf(ct, "+new") == Style::Added);
    CHECK(ct.text.find(" x.txt | 1 +") != std::string::npos);
    std::string root = h40 + '\0' + "" + '\0' + "Ann" + '\0' + "a@x" + '\0' + "1700000000" +
                       '\0' + "Ann" + '\0' + "a@x" + '\0' + "First\n" + '\0' + "\n";
    StyledText rt = commitText(root, 1700000000);
    CHECK(rt.text.find("Parent") == std::string::npos && rt.text.find("Committer") == std::string::npos);
    CHECK(rt.text.find("No changes in this commit.") != std::string::npos);
    CHECK(commitText("not a show", 0).text == "not a show\n");   // falls back to a diff
    Git::Commit c;
    c.hash = h40;
    c.subject = "Fix it";
    CHECK(commitTitle(c) == "aaaaaaa Fix it");
    CHECK(dateText(1700000000).find("2023") != std::string::npos);
    CHECK(dateText(1700000000).find("  ") == std::string::npos);

    GROUP("git-ui:graph");
    // A merge: m has parents a and f; f's parent is a.
    const std::string H = std::string(40, 'c'), F = std::string(40, 'f'), A = std::string(40, 'b');
    std::string log;
    auto commitRec = [&](const std::string& h, const std::string& parents, const std::string& subj) {
        log += h + '\0' + parents + '\0' + "Ann" + '\0' + "1700000000" + '\0' + subj + '\0';
    };
    commitRec(H, A + " " + F, "Merge feature");
    commitRec(F, A, "Feature work");
    commitRec(A, "", "First");
    const std::string refs =
        H + '\0' + '\0' + "refs/heads/main" + '\0' + '\n' +
        F + '\0' + '\0' + "refs/heads/feature" + '\0' + '\n' +
        H + '\0' + '\0' + "refs/remotes/origin/main" + '\0' + '\n' +
        std::string(40, 'd') + '\0' + A + '\0' + "refs/tags/v1.0" + '\0' + '\n';
    Snapshot gs;
    gs.inRepository = true;
    gs.headOid = H;
    gs.branch = "main";
    gs.upstream = "origin/main";
    gs.hasAheadBehind = true;
    Graph g;
    buildGraph(g, gs, log, refs, 200);
    CHECK(g.commits.size() == 3 && !g.hasMore && g.maxWidth == 2);
    std::vector<std::string> desc = graphDescriptions(g);
    CHECK(desc.size() == 3);
    CHECK(desc[0] == "ccccccc lane=0 head Merge feature [*main,origin/main]");
    CHECK(desc[1] == "fffffff lane=1 Feature work [feature]");
    CHECK(desc[2] == "bbbbbbb lane=0 First [v1.0]");
    Graph small;
    buildGraph(small, gs, log, refs, 2);
    CHECK(small.hasMore && graphDescriptions(small).back() == "(more)");
    g.divergence = Git::parseLeftRight("<" + H + "\n>" + F + "\n");
    std::string tip = graphToolTip(g, 0, 1700000000 + 7200);
    CHECK(tip.find("ccccccc  Ann, 2 hours ago (") == 0);
    CHECK(tip.find("\nMerge feature") != std::string::npos);
    CHECK(tip.find("Not pushed yet") != std::string::npos);
    CHECK(tip.find("main, origin/main") != std::string::npos);
    CHECK(graphToolTip(g, 1, 0).find("Not pulled yet") != std::string::npos);
    CHECK(graphToolTip(g, 3, 0) == "Load the next 200 commits");
    CHECK(graphKey(gs, 200, false, true, refs) != graphKey(gs, 400, false, true, refs));
    CHECK(graphKey(gs, 200, false, true, refs) != graphKey(gs, 200, true, true, refs));
    CHECK(graphKey(gs, 200, false, true, refs) == graphKey(gs, 200, false, true, refs));
    CHECK(graphKey(gs, 200, false, true, refs) != graphKey(gs, 200, false, true, refs + "x"));
    CHECK(laneWidth(300, 2) == 12 && laneWidth(300, 40) == 4);
    CHECK(laneWidth(200, 10) > 7 && laneWidth(200, 10) < 7.3);
    CHECK(laneColor(0) == 0x59A4F9 && laneColor(7) == 0x59A4F9 && laneColor(-1) == 0x8FCB5A);
    CHECK(pillColor(Git::RefKind::Head) == 0xEA5C00 && pillColor(Git::RefKind::Tag) == 0xE2C08D);
    CHECK(pillColor(Git::RefKind::Remote) == 0xB180D7 && pillColor(Git::RefKind::Branch) == 0x59A4F9);

    GROUP("git-ui:stage-all");
    {
        // s: 2 staged, 3 in Changes (one untracked).
        ActionPlan p = stageAllPlan(s);
        CHECK(p.args == std::vector<std::string>({"--literal-pathspecs", "add", "-A", "--", "."}));
        CHECK(p.refusal.empty() && p.question.empty() && p.doneText == "Staged 3 files.");
        CHECK(Git::timeoutFor(p.args) == 120);
        CHECK(stageAllPlan(clean).args.empty() &&
              stageAllPlan(clean).refusal == "There are no changes to stage.");
        Snapshot outside;
        CHECK(stageAllPlan(outside).refusal == "This folder is not in a git repository.");
        CHECK(unstageAllPlan(outside).args.empty());
        // A conflict asks first: staging it marks it resolved.
        Snapshot conflict;
        conflict.inRepository = true;
        applyStatus(conflict, Git::parseStatus(nul({
            "# branch.oid " + h40, "# branch.head main",
            "u UU N... 100644 100644 100644 100644 " + h40 + " " + h40 + " " + h40 + " c.txt",
            "1 .M N..." + modes + h40 + " " + h40 + " m.txt"})));
        p = stageAllPlan(conflict);
        CHECK(p.question == "Stage 1 file that still has conflicts?");
        CHECK(p.detail.find("conflict markers") != std::string::npos && p.button == "Stage All");
        CHECK(p.doneText == "Staged 2 files." && !p.args.empty());

        ActionPlan u = unstageAllPlan(s);
        CHECK(u.args == std::vector<std::string>(
                            {"--literal-pathspecs", "restore", "--staged", "--", "."}));
        CHECK(u.doneText == "Unstaged 2 files." && u.question.empty());
        // Before the first commit there is no HEAD to restore from.
        u = unstageAllPlan(empty);
        CHECK(u.args == std::vector<std::string>({"--literal-pathspecs", "rm", "--cached", "-r",
                                                  "--force", "-q", "--", "."}));
        CHECK(u.doneText == "Unstaged 1 file.");
        CHECK(unstageAllPlan(clean).refusal == "Nothing is staged.");
        CHECK(unstageAllPlan(conflict).args.empty());   // a conflict is not staged
    }

    GROUP("git-ui:discard");
    {
        Snapshot d;
        d.inRepository = true;
        applyStatus(d, Git::parseStatus(nul({
            "# branch.oid " + h40, "# branch.head main",
            "1 .M N..." + modes + h40 + " " + h40 + " src/a b.txt",
            "1 .D N..." + modes + h40 + " " + h40 + " gone.txt",
            "1 .T N..." + modes + h40 + " " + h40 + " link",
            "1 .A N... 000000 000000 100644 " + z40 + " " + z40 + " later.txt",
            "1 M. N..." + modes + h40 + " " + h40 + " staged.txt",
            "u UU N... 100644 100644 100644 100644 " + h40 + " " + h40 + " " + h40 + " c.txt",
            "? -new.txt"})));
        auto row = [&](const std::string& path, bool staged) -> const Row& {
            for (const Row& x : d.rows)
                if (!x.header && x.path == path && x.staged == staged) return x;
            static Row none;
            return none;
        };
        // Modified: the file goes to the Trash, then back from the index.
        DiscardPlan m = discardPlan(row("src/a b.txt", false));
        CHECK(m.trashFirst && m.refusal.empty());
        CHECK(m.args == std::vector<std::string>(
                            {"--literal-pathspecs", "restore", "--worktree", "--", "src/a b.txt"}));
        CHECK(!hasArg(m.args, "--source=HEAD") && !hasArg(m.args, "--staged"));
        CHECK(m.question == "Discard the changes to a b.txt?" && m.button == "Discard Changes");
        CHECK(m.detail.find("Trash") != std::string::npos &&
              m.detail.find("Undo Discard") != std::string::npos);
        CHECK(m.doneText.find("Trash") != std::string::npos);
        CHECK(Git::timeoutFor(m.args) == 120);
        // A type change is restored the same way.
        CHECK(discardPlan(row("link", false)).trashFirst &&
              discardPlan(row("link", false)).args.back() == "link");
        // Deleted: nothing on disk to keep; restored.
        DiscardPlan g = discardPlan(row("gone.txt", false));
        CHECK(!g.trashFirst && g.args.back() == "gone.txt" && hasArg(g.args, "--worktree"));
        CHECK(g.question == "Restore gone.txt?" && g.button == "Restore");
        CHECK(g.detail.find("deletes it again") != std::string::npos);
        // Untracked: only the Trash; git runs nothing, so the leading dash
        // never reaches a command line.
        DiscardPlan u = discardPlan(row("-new.txt", false));
        CHECK(u.trashFirst && u.args.empty() && u.refusal.empty());
        CHECK(u.question == "Move -new.txt to the Trash?" && u.button == "Move to Trash");
        CHECK(u.detail.find("not tracked") != std::string::npos);
        // Refused: staged, a conflict, intent to add, a heading.
        CHECK(discardPlan(row("staged.txt", true)).refusal.find("unstage it first") !=
              std::string::npos);
        CHECK(discardPlan(row("staged.txt", true)).args.empty());
        CHECK(discardPlan(row("c.txt", false)).refusal.find("conflict") != std::string::npos);
        CHECK(discardPlan(row("later.txt", false)).refusal.find("git add -N") !=
              std::string::npos);
        CHECK(discardPlan(row("later.txt", false)).args.empty());
        CHECK(!discardPlan(d.rows[0]).refusal.empty() && discardPlan(d.rows[0]).args.empty());
        // Every plan that runs git restores the work tree only.
        for (const Row& x : d.rows) {
            DiscardPlan p = discardPlan(x);
            if (p.args.empty()) continue;
            CHECK(p.args[1] == "restore" && hasArg(p.args, "--worktree") &&
                  argAt(p.args, "--") == (int)p.args.size() - 2);
        }
        CHECK(undoDiscardTitle("src/a b.txt") == "Undo Discard of a b.txt");
        CHECK(undoneDiscardText("x/y.txt").find("y.txt back") != std::string::npos);
        // From the menu bar: the file on show's Changes row.
        CHECK(changesRowFor(d.rows, "src/a b.txt") >= 0 &&
              !d.rows[changesRowFor(d.rows, "src/a b.txt")].staged);
        CHECK(changesRowFor(d.rows, "staged.txt") == -1);
        CHECK(changesRowFor(d.rows, "nope") == -1);
    }

    GROUP("git-ui:switch");
    {
        auto line = [](const std::string& obj, const std::string& name, const std::string& sym) {
            return obj + std::string("\0", 1) + std::string("\0", 1) + name +
                   std::string("\0", 1) + sym + "\n";
        };
        const std::string a(40, 'a'), b(40, 'b'), c(40, 'c');
        const std::string out = line(b, "refs/heads/feature", "") +
                                line(a, "refs/heads/main", "") +
                                line(c, "refs/remotes/origin/HEAD", "refs/remotes/origin/main") +
                                line(c, "refs/remotes/origin/main", "") +
                                line(c, "refs/remotes/origin/fix/login", "") +
                                line(c, "refs/remotes/my/fork/topic", "") +
                                line(c, "refs/heads/-x", "");
        Snapshot bs;
        bs.inRepository = true;
        bs.branch = "main";
        bs.remotes = {"origin", "my/fork"};
        CHECK(hasArg(branchListArgs(), "--sort=-committerdate") &&
              branchListArgs()[0] == "for-each-ref" && hasArg(branchListArgs(), "refs/heads") &&
              hasArg(branchListArgs(), "refs/remotes") && !hasArg(branchListArgs(), "refs/tags"));
        std::vector<BranchItem> items = branchItems(out, bs);
        // The current branch first, then the other locals in git's order
        // (newest first), then remote branches with no local one; origin/HEAD,
        // origin/main (main is local) and a name like an option are left out.
        CHECK(items.size() == 4);
        CHECK(items[0].name == "main" && items[0].current && items[0].detail == "current · aaaaaaa");
        CHECK(items[1].name == "feature" && !items[1].current && items[1].detail == "bbbbbbb");
        CHECK(items[2].name == "origin/fix/login" && items[2].remote &&
              items[2].detail == "remote · ccccccc");
        CHECK(items[3].name == "my/fork/topic" && items[3].remote);
        CHECK(switchArgs(items[0], bs).empty());
        CHECK(switchArgs(items[1], bs) == std::vector<std::string>({"switch", "feature"}));
        CHECK(switchArgs(items[2], bs) == std::vector<std::string>(
                                              {"switch", "-c", "fix/login", "--track",
                                               "origin/fix/login"}));
        // The longest remote that fits names the branch.
        CHECK(localNameFor("my/fork/topic", bs) == "topic");
        CHECK(localNameFor("up/x", bs) == "x");   // remotes not known: up to the first slash
        // Detached: no branch is current.
        Snapshot det = bs;
        det.detached = true;
        det.branch.clear();
        CHECK(!branchItems(out, det)[0].current && branchItems(out, det)[0].name == "feature");

        CHECK(newBranchName("  my new  branch ") == "my-new--branch");
        CHECK(branchNameProblem("feature/x").empty() && branchNameProblem("v1.2").empty());
        CHECK(!branchNameProblem("").empty());
        for (const char* bad : {"-f", "a..b", "a b", "a~1", "x^", "a:b", "a?", "a*", "a[b", "a\\b",
                                "/a", "a/", "a.", "a//b", "a/.b", ".a", "a.lock", "x/y.lock/z",
                                "a@{1}", "@", "HEAD"})
            CHECK(!branchNameProblem(bad).empty());
        CHECK(createBranchLabel("new thing", items, bs) == "Create branch “new-thing” from main");
        CHECK(createBranchLabel("feature", items, bs).empty());     // a local branch already
        CHECK(!createBranchLabel("fix/login", items, bs).empty());  // only a remote one has it
        CHECK(createBranchLabel("  ", items, bs).empty());
        CHECK(createBranchLabel("x", items, det) == "Create branch “x” from the detached HEAD");
        CHECK(createBranchArgs("x") == std::vector<std::string>({"switch", "-c", "x"}));
        CHECK(switchedText("x", false) == "Switched to x." &&
              switchedText("x", true) == "Created x and switched to it.");
    }

    GROUP("git-ui:hunk-patch");
    {
        const std::string hdr = "diff --git a/f.txt b/f.txt\n--- a/f.txt\n+++ b/f.txt\n";
        // Lines: 0 diff, 1 index, 2 ---, 3 +++, 4 @@, 5 a, 6 -b, 7 +B, 8 c,
        // 9 d, 10 @@, 11 j, 12 k, 13 +k2, 14 +k3, 15 l.
        const std::string two =
            "diff --git a/f.txt b/f.txt\nindex 1111111..2222222 100644\n--- a/f.txt\n"
            "+++ b/f.txt\n@@ -1,4 +1,4 @@\n a\n-b\n+B\n c\n d\n@@ -10,3 +10,5 @@ fn\n"
            " j\n k\n+k2\n+k3\n l\n";
        std::vector<DiffHunk> hs = diffHunks(two);
        CHECK(hs.size() == 2 && hs[0].firstLine == 4 && hs[0].lineCount == 6 &&
              hs[1].firstLine == 10 && hs[1].lineCount == 6 && hs[1].hunk == 1 &&
              hs[0].file == 0 && hs[0].kinds == "@ -+  ");
        CHECK(hunkAt(hs, 4) == 0 && hunkAt(hs, 9) == 0 && hunkAt(hs, 15) == 1 &&
              hunkAt(hs, 3) == -1 && hunkAt(hs, 16) == -1);
        CHECK(hs[1].changes(13) && !hs[1].changes(12) && !hs[1].changes(10));

        // The second hunk alone, staged: the index's numbers stand.
        std::string why;
        CHECK(buildPatch(two, HunkOp::Stage, 10, 15, &why) ==
              hdr + "@@ -10,3 +10,5 @@\n j\n k\n+k2\n+k3\n l\n");
        CHECK(why.empty());
        // Both hunks in one patch.
        CHECK(buildPatch(two, HunkOp::Stage, 0, 15, nullptr) ==
              hdr + "@@ -1,4 +1,4 @@\n a\n-b\n+B\n c\n d\n"
                    "@@ -10,3 +10,5 @@\n j\n k\n+k2\n+k3\n l\n");
        // The plans: one hunk from any of its lines, and what each runs.
        HunkPlan st = hunkPlan(two, DiffSide::Changes, HunkOp::Stage, 12, 12, false);
        CHECK(st.refusal.empty() && st.path == "f.txt" && st.question.empty() &&
              st.patch == hdr + "@@ -10,3 +10,5 @@\n j\n k\n+k2\n+k3\n l\n");
        CHECK(st.args == std::vector<std::string>({"apply", "--cached", "--whitespace=nowarn", "-"}));
        CHECK(st.doneText == "Staged a hunk of f.txt.");
        HunkPlan un = hunkPlan(two, DiffSide::Staged, HunkOp::Unstage, 4, 4, false);
        CHECK(un.args == std::vector<std::string>({"apply", "--cached", "--reverse",
                                                   "--whitespace=nowarn", "-"}));
        CHECK(un.patch == hdr + "@@ -1,4 +1,4 @@\n a\n-b\n+B\n c\n d\n");
        HunkPlan dis = hunkPlan(two, DiffSide::Changes, HunkOp::Discard, 6, 6, false);
        CHECK(dis.args == std::vector<std::string>({"apply", "--reverse", "--whitespace=nowarn", "-"}));
        CHECK(dis.question == "Discard this hunk of f.txt?" && dis.button == "Discard Hunk" &&
              dis.detail.find("Undo Discard") != std::string::npos);
        // Wrong side, outside any hunk.
        CHECK(!hunkPlan(two, DiffSide::Staged, HunkOp::Stage, 6, 6, false).refusal.empty());
        CHECK(hunkPlan(two, DiffSide::Changes, HunkOp::Unstage, 6, 6, false).args.empty());
        CHECK(!hunkPlan(two, DiffSide::Untracked, HunkOp::Stage, 6, 6, false).refusal.empty());
        CHECK(hunkPlan(two, DiffSide::Changes, HunkOp::Stage, 1, 1, false).refusal ==
              "Put the caret in a hunk first.");
        CHECK(hunkOps(DiffSide::Changes) == std::vector<HunkOp>({HunkOp::Stage, HunkOp::Discard}));
        CHECK(hunkOps(DiffSide::Staged) == std::vector<HunkOp>({HunkOp::Unstage}));
        CHECK(hunkOps(DiffSide::Untracked).empty());
        CHECK(hunkOpTitle(HunkOp::Unstage, false) == "Unstage Hunk" &&
              hunkOpTitle(HunkOp::Discard, true) == "Discard Selected Lines");
        Row wr; wr.path = "f.txt";
        Row sr = wr; sr.staged = true;
        Row ur = wr; ur.untracked = true;
        CHECK(diffSideOf(wr) == DiffSide::Changes && diffSideOf(sr) == DiffSide::Staged &&
              diffSideOf(ur) == DiffSide::Untracked);

        // A hunk header with its counts left out (one line), at the start.
        const std::string one = "diff --git a/o b/o\n--- a/o\n+++ b/o\n@@ -1 +1,2 @@\n a\n+b\n";
        CHECK(buildPatch(one, HunkOp::Stage, 3, 5, nullptr) ==
              "diff --git a/o b/o\n--- a/o\n+++ b/o\n@@ -1,1 +1,2 @@\n a\n+b\n");

        // A staged rename: unstaging a hunk changes the file under its new name.
        const std::string ren =
            "diff --git a/old.txt b/new.txt\nsimilarity index 80%\nrename from old.txt\n"
            "rename to new.txt\nindex 1111111..2222222 100644\n--- a/old.txt\n+++ b/new.txt\n"
            "@@ -1,3 +1,3 @@\n x\n-y\n+Y\n z\n";
        HunkPlan rp = hunkPlan(ren, DiffSide::Staged, HunkOp::Unstage, 9, 9, false);
        CHECK(rp.path == "new.txt" &&
              rp.patch == "diff --git a/new.txt b/new.txt\n--- a/new.txt\n+++ b/new.txt\n"
                          "@@ -1,3 +1,3 @@\n x\n-y\n+Y\n z\n");

        // The end of a file with no newline: the marker follows its line.
        const std::string nonl =
            "diff --git a/n.txt b/n.txt\n--- a/n.txt\n+++ b/n.txt\n@@ -1,2 +1,3 @@\n a\n-b\n"
            "\\ No newline at end of file\n+b\n+c\n\\ No newline at end of file\n";
        CHECK(buildPatch(nonl, HunkOp::Stage, 3, 9, nullptr) ==
              "diff --git a/n.txt b/n.txt\n--- a/n.txt\n+++ b/n.txt\n@@ -1,2 +1,3 @@\n a\n-b\n"
              "\\ No newline at end of file\n+b\n+c\n\\ No newline at end of file\n");

        // Files that are staged or unstaged whole, and a conflict.
        const std::string added = "diff --git a/n b/n\nnew file mode 100644\nindex 0000000..1111111\n"
                                  "--- /dev/null\n+++ b/n\n@@ -0,0 +1,2 @@\n+a\n+b\n";
        CHECK(hunkPlan(added, DiffSide::Staged, HunkOp::Unstage, 5, 5, false).refusal ==
              "n is a new file: stage or unstage it whole.");
        const std::string gone = "diff --git a/g b/g\ndeleted file mode 100644\nindex 1111111..0000000\n"
                                 "--- a/g\n+++ /dev/null\n@@ -1 +0,0 @@\n-a\n";
        CHECK(hunkPlan(gone, DiffSide::Changes, HunkOp::Stage, 5, 5, false).refusal.find("deleted") !=
              std::string::npos);
        const std::string cc = "diff --cc c.txt\nindex 1111111,2222222..0000000\n--- a/c.txt\n"
                               "+++ b/c.txt\n@@@ -1,1 -1,1 +1,5 @@@\n++<<<<<<< HEAD\n + a\n"
                               "++=======\n+ b\n++>>>>>>> x\n";
        CHECK(hunkPlan(cc, DiffSide::Changes, HunkOp::Stage, 4, 4, false).refusal.find("conflict") !=
              std::string::npos);
        CHECK(hunkPlan("Binary files a/p.png and b/p.png differ\n", DiffSide::Changes,
                       HunkOp::Stage, 0, 0, false).refusal == "Put the caret in a hunk first.");
        // A path git quotes is quoted again in the patch.
        const std::string q = "diff --git \"a/a\\\"b.txt\" \"b/a\\\"b.txt\"\n--- \"a/a\\\"b.txt\"\n"
                              "+++ \"b/a\\\"b.txt\"\n@@ -1 +1 @@\n-x\n+y\n";
        HunkPlan qp = hunkPlan(q, DiffSide::Changes, HunkOp::Stage, 3, 3, false);
        const std::string qWant = "diff --git \"a/a\\\"b.txt\" \"b/a\\\"b.txt\"\n--- \"a/a\\\"b.txt\"\n";
        CHECK(qp.path == "a\"b.txt" && qp.patch.compare(0, qWant.size(), qWant) == 0);

        CHECK(hunkFailureText(1, "", "error: patch failed: f.txt:1\nerror: f.txt: patch does not apply",
                              "src/f.txt") ==
              "f.txt changed since this diff was shown, so nothing was done. "
              "The diff now shows it as it is.");
        CHECK(hunkFailureText(128, "", "fatal: not a git repository", "f.txt") ==
              "fatal: not a git repository");

        // The actions on the hunk headers, and the text keeps one line per diff line.
        std::vector<DiffAction> acts;
        StyledText t = diffTextWithActions(two, DiffSide::Changes, acts);
        CHECK(acts.size() == 4 && acts[0].line == 4 && acts[0].op == HunkOp::Stage &&
              acts[1].op == HunkOp::Discard && acts[2].line == 10);
        const std::vector<std::string> cs = gitUiChars(t.text);
        auto words = [&](const DiffAction& a) {
            std::string s;
            for (size_t k = a.start; k < a.start + a.length && k < cs.size(); ++k) s += cs[k];
            return s;
        };
        CHECK(words(acts[0]) == "Stage Hunk" && words(acts[3]) == "Discard Hunk");
        CHECK(std::count(t.text.begin(), t.text.end(), '\n') == 16);
        CHECK(t.text.find("@@ -10,3 +10,5 @@ fn    Stage Hunk \xC2\xB7 Discard Hunk\n") !=
              std::string::npos);
        CHECK(diffTextWithActions(two, DiffSide::Staged, acts).text.find("Unstage Hunk") !=
                  std::string::npos && acts.size() == 2 && acts[1].op == HunkOp::Unstage);
        CHECK(diffTextWithActions(two, DiffSide::Untracked, acts).text == diffText(two).text &&
              acts.empty());
        diffTextWithActions(added, DiffSide::Staged, acts);
        CHECK(acts.empty());
        CHECK(diffTextWithActions("", DiffSide::Changes, acts).text == "No differences to show.");
    }

    GROUP("git-ui:line-patch");
    {
        const std::string hdr = "diff --git a/f.txt b/f.txt\n--- a/f.txt\n+++ b/f.txt\n";
        const std::string two =
            "diff --git a/f.txt b/f.txt\nindex 1111111..2222222 100644\n--- a/f.txt\n"
            "+++ b/f.txt\n@@ -1,4 +1,4 @@\n a\n-b\n+B\n c\n d\n@@ -10,3 +10,5 @@ fn\n"
            " j\n k\n+k2\n+k3\n l\n";
        std::string why;
        // Staging one added line: the other is left out.
        CHECK(buildPatch(two, HunkOp::Stage, 13, 13, &why) ==
              hdr + "@@ -10,3 +10,4 @@\n j\n k\n+k2\n l\n");
        // One removed line: its replacement is left out, so the line goes.
        CHECK(buildPatch(two, HunkOp::Stage, 6, 6, nullptr) == hdr + "@@ -1,4 +1,3 @@\n a\n-b\n c\n d\n");
        // Across two hunks: a removed line not chosen stays as context, and
        // the second hunk's new start moves by the line the first adds.
        CHECK(buildPatch(two, HunkOp::Stage, 7, 13, nullptr) ==
              hdr + "@@ -1,4 +1,5 @@\n a\n b\n+B\n c\n d\n@@ -10,3 +11,4 @@\n j\n k\n+k2\n l\n");
        // Reversed (unstage, discard): an added line not chosen stays as
        // context and a removed one is left out; the + side keeps its numbers.
        CHECK(buildPatch(two, HunkOp::Unstage, 13, 13, nullptr) ==
              hdr + "@@ -10,4 +10,5 @@\n j\n k\n+k2\n k3\n l\n");
        CHECK(buildPatch(two, HunkOp::Discard, 6, 6, nullptr) ==
              hdr + "@@ -1,5 +1,4 @@\n a\n-b\n B\n c\n d\n");
        CHECK(buildPatch(two, HunkOp::Discard, 7, 13, nullptr) ==
              hdr + "@@ -1,3 +1,4 @@\n a\n+B\n c\n d\n@@ -9,4 +10,5 @@\n j\n k\n+k2\n k3\n l\n");
        // Context only, or nothing: refused.
        CHECK(buildPatch(two, HunkOp::Stage, 11, 12, &why).empty() &&
              why == "Choose lines that changed (the + and - lines) first.");
        HunkPlan lp = hunkPlan(two, DiffSide::Changes, HunkOp::Stage, 14, 13, true);
        CHECK(lp.patch == hdr + "@@ -10,3 +10,5 @@\n j\n k\n+k2\n+k3\n l\n" &&
              lp.doneText == "Staged the selected lines of f.txt.");
        HunkPlan ld = hunkPlan(two, DiffSide::Changes, HunkOp::Discard, 13, 13, true);
        CHECK(ld.question == "Discard the selected lines of f.txt?" && ld.button == "Discard Lines");

        // No newline at the end: lines that would leave the marker in the
        // middle are refused; the newline added on its own is fine.
        const std::string nonl =
            "diff --git a/n.txt b/n.txt\n--- a/n.txt\n+++ b/n.txt\n@@ -1,2 +1,3 @@\n a\n-b\n"
            "\\ No newline at end of file\n+b\n+c\n\\ No newline at end of file\n";
        CHECK(buildPatch(nonl, HunkOp::Stage, 8, 8, &why).empty() &&
              why.find("no newline") != std::string::npos);
        CHECK(buildPatch(nonl, HunkOp::Stage, 5, 7, nullptr) ==
              "diff --git a/n.txt b/n.txt\n--- a/n.txt\n+++ b/n.txt\n@@ -1,2 +1,2 @@\n a\n-b\n"
              "\\ No newline at end of file\n+b\n");
        // A line whose text is empty in the diff (its space lost) is context.
        const std::string blank = "diff --git a/e b/e\n--- a/e\n+++ b/e\n@@ -1,3 +1,3 @@\n x\n\n-y\n+z\n";
        CHECK(buildPatch(blank, HunkOp::Stage, 6, 6, nullptr) ==
              "diff --git a/e b/e\n--- a/e\n+++ b/e\n@@ -1,3 +1,2 @@\n x\n \n-y\n");
        // CRLF lines keep their carriage returns.
        const std::string crlf = "diff --git a/w b/w\n--- a/w\n+++ b/w\n@@ -1,2 +1,2 @@\n a\r\n-b\r\n+c\r\n";
        CHECK(buildPatch(crlf, HunkOp::Stage, 3, 6, nullptr) ==
              "diff --git a/w b/w\n--- a/w\n+++ b/w\n@@ -1,2 +1,2 @@\n a\r\n-b\r\n+c\r\n");
    }

    GROUP("git-ui:submodule-refused");
    {
        // A submodule with a change inside it, and one at another commit:
        // Discard is refused (it trashed the checkout and left an empty
        // folder), Stage still works (a gitlink stages as a commit).
        Snapshot sm;
        sm.inRepository = true;
        applyStatus(sm, Git::parseStatus(nul({
            "# branch.oid " + h40, "# branch.head main",
            "1 .M S.M. 160000 160000 160000 " + h40 + " " + h40 + " lib",
            "1 .M SC.. 160000 160000 160000 " + h40 + " " + h40 + " vendor/other",
            "1 .M N..." + modes + h40 + " " + h40 + " plain.txt"})));
        CHECK(sm.rows.size() == 4 && sm.rows[1].path == "lib" && sm.rows[1].submodule &&
              sm.rows[2].submodule && !sm.rows[3].submodule);
        DiscardPlan d = discardPlan(sm.rows[1]);
        CHECK(d.args.empty() && !d.trashFirst);
        CHECK(d.refusal == "lib is a submodule: discard inside it (open it as a folder) or run "
                           "git submodule update.");
        CHECK(!discardPlan(sm.rows[2]).refusal.empty() && discardPlan(sm.rows[3]).refusal.empty());
        CHECK(stageArgs(sm.rows[1], false) ==
              std::vector<std::string>({"--literal-pathspecs", "add", "-A", "--", "lib"}));
        // Its diff is a commit a side: no hunk or line patches.
        const std::string gitlink =
            "diff --git a/lib b/lib\nindex 1111111..2222222 160000\n--- a/lib\n+++ b/lib\n"
            "@@ -1 +1 @@\n-Subproject commit 1111111111111111111111111111111111111111\n"
            "+Subproject commit 2222222222222222222222222222222222222222-dirty\n";
        std::string why;
        CHECK(buildPatch(gitlink, HunkOp::Stage, 4, 6, &why).empty());
        CHECK(why == "lib is a submodule: stage or unstage it whole, and discard inside it "
                     "(open it as a folder).");
        HunkPlan hp = hunkPlan(gitlink, DiffSide::Changes, HunkOp::Discard, 5, 5, false);
        CHECK(hp.patch.empty() && hp.refusal == why && hp.question.empty());
        // The mode decides, wherever the header gives it: a file becoming
        // a submodule, and a submodule added.
        const std::string typechange =
            "diff --git a/lib b/lib\nold mode 100644\nnew mode 160000\n"
            "index 3333333..2222222\n--- a/lib\n+++ b/lib\n@@ -1 +1 @@\n-text\n"
            "+Subproject commit 2222222222222222222222222222222222222222\n";
        CHECK(hunkPlan(typechange, DiffSide::Staged, HunkOp::Unstage, 7, 7, false).refusal == why);
        const std::string added =
            "diff --git a/lib b/lib\nnew file mode 160000\nindex 0000000..2222222\n"
            "--- /dev/null\n+++ b/lib\n@@ -0,0 +1 @@\n"
            "+Subproject commit 2222222222222222222222222222222222222222\n";
        CHECK(hunkPlan(added, DiffSide::Staged, HunkOp::Unstage, 6, 6, false).refusal == why);
        // A plain file whose lines read "Subproject commit <object name>"
        // (pasted git output) is a text file: its mode says so (round 6's
        // workspace 3 refused it as a submodule).
        const std::string pasted =
            "diff --git a/notes.txt b/notes.txt\nindex 1111111..2222222 100644\n"
            "--- a/notes.txt\n+++ b/notes.txt\n@@ -1,2 +1,2 @@\n"
            " Subproject commit 3333333333333333333333333333333333333333\n"
            "-Subproject commit 1111111111111111111111111111111111111111\n"
            "+Subproject commit 2222222222222222222222222222222222222222-dirty\n";
        HunkPlan text = hunkPlan(pasted, DiffSide::Changes, HunkOp::Stage, 6, 7, false);
        CHECK(text.refusal.empty() && !text.patch.empty() && text.path == "notes.txt");
        CHECK(!buildPatch(pasted, HunkOp::Stage, 7, 7, &why).empty());
        // Without an index line (git writes none when both sides name one
        // commit and only "-dirty" differs, as for lib above with a change
        // inside it), one "Subproject commit" line a side is the gitlink.
        const std::string bare =
            "diff --git a/lib b/lib\n--- a/lib\n+++ b/lib\n@@ -1 +1 @@\n"
            "-Subproject commit 1111111111111111111111111111111111111111\n"
            "+Subproject commit 1111111111111111111111111111111111111111-dirty\n";
        CHECK(hunkPlan(bare, DiffSide::Changes, HunkOp::Discard, 4, 4, false).refusal == why);
        // ...but not lines of that form among others.
        const std::string mixed =
            "diff --git a/n.txt b/n.txt\n--- a/n.txt\n+++ b/n.txt\n@@ -1,2 +1,2 @@\n"
            " text\n-Subproject commit 1111111111111111111111111111111111111111\n"
            "+Subproject commit 2222222222222222222222222222222222222222\n";
        CHECK(!hunkPlan(mixed, DiffSide::Changes, HunkOp::Stage, 5, 6, false).patch.empty());
        const std::string words = "diff --git a/n.txt b/n.txt\n--- a/n.txt\n+++ b/n.txt\n"
                                  "@@ -1 +1 @@\n-Subproject commits\n+Subproject commit x\n";
        CHECK(!hunkPlan(words, DiffSide::Changes, HunkOp::Stage, 3, 3, false).patch.empty());
    }

    GROUP("git-ui:multi-discard");
    {
        Snapshot d;
        d.inRepository = true;
        applyStatus(d, Git::parseStatus(nul({
            "# branch.oid " + h40, "# branch.head main",
            "1 .M N..." + modes + h40 + " " + h40 + " a.txt",
            "1 .D N..." + modes + h40 + " " + h40 + " b.txt",
            "1 M. N..." + modes + h40 + " " + h40 + " s.txt",
            "2 R. N..." + modes + h40 + " " + h40 + " R100 new.txt", "old.txt",
            "1 .M S.M. 160000 160000 160000 " + h40 + " " + h40 + " lib",
            "? u.txt"})));
        auto row = [&](const std::string& path, bool staged) {
            for (const Row& x : d.rows)
                if (!x.header && x.path == path && x.staged == staged) return x;
            return Row();
        };
        const Row a = row("a.txt", false), b = row("b.txt", false), u = row("u.txt", false);
        const Row st = row("s.txt", true), rn = row("new.txt", true), lib = row("lib", false);
        // Three rows: one question naming the count, one plan a row.
        DiscardBatch three = discardBatchPlan({a, b, u});
        CHECK(three.refusal.empty() && three.plans.size() == 3);
        CHECK(three.paths == std::vector<std::string>({"a.txt", "b.txt", "u.txt"}));
        CHECK(three.question == "Discard the changes to 3 files?" &&
              three.button == "Discard 3 Files");
        CHECK(three.detail.find("1 file git does not track goes to the Trash") != std::string::npos);
        CHECK(three.plans[0].trashFirst && three.plans[2].trashFirst && three.plans[2].args.empty());
        CHECK(three.doneText.find("3 files") != std::string::npos);
        // One row is the single discard, word for word.
        DiscardBatch one = discardBatchPlan({a});
        CHECK(one.question == "Discard the changes to a.txt?" && one.plans.size() == 1);
        // Any refusal refuses all, naming the file.
        DiscardBatch bad = discardBatchPlan({a, lib, b});
        CHECK(bad.plans.empty() && bad.paths.empty() && bad.question.empty());
        CHECK(bad.refusal == "Nothing was discarded: lib is a submodule: discard inside it "
                             "(open it as a folder) or run git submodule update.");
        CHECK(discardBatchPlan({a, st}).refusal.find("s.txt is staged") != std::string::npos);
        CHECK(discardBatchPlan({}).refusal == "Choose a file to discard.");
        // Stage the Changes rows of a selection, unstage its Staged rows (a
        // rename by both names), one command each.
        CHECK(stageRowsArgs({a, st, u}, false, true) ==
              std::vector<std::string>({"--literal-pathspecs", "add", "-A", "--", "a.txt", "u.txt"}));
        CHECK(stageRowsArgs({a, st, rn}, false, false) ==
              std::vector<std::string>({"--literal-pathspecs", "restore", "--staged", "--", "s.txt",
                                        "new.txt", "old.txt"}));
        CHECK(stageRowsArgs({st}, true, false) ==
              std::vector<std::string>({"--literal-pathspecs", "rm", "--cached", "--force", "-q",
                                        "--", "s.txt"}));
        CHECK(stageRowsArgs({a, u}, false, false).empty() && stageRowsArgs({st}, false, true).empty());
    }

    GROUP("git-ui:amend");
    {
        Snapshot h;   // a commit on main, ahead of origin/main by one
        h.inRepository = true;
        applyStatus(h, Git::parseStatus(nul({
            "# branch.oid " + h40, "# branch.head main", "# branch.upstream origin/main",
            "# branch.ab +1 -0", "1 M. N..." + modes + h40 + " " + h40 + " a.txt"})));
        CHECK(!headIsPushed(h));
        ActionPlan p = amendPlan(h, "New message\n\nbody");
        CHECK(p.args == std::vector<std::string>({"commit", "--amend", "-m", "New message\n\nbody"}));
        CHECK(p.question.empty() && p.refusal.empty() && Git::timeoutFor(p.args) == 600);
        CHECK(amendPlan(h, "  \n").args ==
              std::vector<std::string>({"commit", "--amend", "--no-edit"}));
        // Pushed already: asks, and says why.
        Snapshot pushed = h;
        pushed.ahead = 0;
        CHECK(headIsPushed(pushed));
        p = amendPlan(pushed, "");
        CHECK(p.question == "Amend a commit that is already pushed?" && p.button == "Amend" &&
              p.detail.find("origin/main") != std::string::npos &&
              p.detail.find("force") != std::string::npos);
        // No upstream, a gone one, detached: nothing to ask.
        Snapshot local = h;
        local.upstream.clear();
        local.ahead = 0;
        CHECK(!headIsPushed(local) && amendPlan(local, "").question.empty());
        Snapshot gone = pushed;
        gone.hasAheadBehind = false;
        CHECK(!headIsPushed(gone));
        // Before the first commit, and outside a repository: refused.
        Snapshot first;
        first.inRepository = true;
        applyStatus(first, Git::parseStatus(nul({"# branch.oid (initial)", "# branch.head main"})));
        CHECK(amendPlan(first, "x").args.empty() &&
              amendPlan(first, "x").refusal == "There is no commit to amend yet: commit first.");
        CHECK(amendPlan(Snapshot(), "x").refusal == "This folder is not in a git repository.");

        // Undo Last Commit.
        ActionPlan u = undoCommitPlan(h, 1);
        CHECK(u.args == std::vector<std::string>({"reset", "--soft", "HEAD~1"}));
        CHECK(u.question.empty() && u.doneText.find("staged") != std::string::npos);
        CHECK(Git::timeoutFor(u.args) == 30);
        u = undoCommitPlan(h, 0);   // the first commit: no HEAD~1
        CHECK(u.args == std::vector<std::string>({"update-ref", "-d", "HEAD"}) && u.question.empty());
        u = undoCommitPlan(h, 2);   // a merge
        CHECK(u.question == "Undo the merge commit?" && u.button == "Undo Merge Commit");
        CHECK(u.detail.find("first parent") != std::string::npos &&
              u.detail.find("merged branch keeps its own commits") != std::string::npos);
        CHECK(u.args == std::vector<std::string>({"reset", "--soft", "HEAD~1"}));
        u = undoCommitPlan(pushed, 1);
        CHECK(u.question == "Undo a commit that is already pushed?" &&
              u.detail.find("force") != std::string::npos);
        CHECK(undoCommitPlan(first, 0).refusal == "There is no commit to undo yet.");
        CHECK(undoCommitPlan(h, -1).args.empty() && !undoCommitPlan(h, -1).refusal.empty());
        Snapshot det = h;
        det.detached = true;
        det.branch.clear();
        CHECK(undoCommitPlan(det, 1).refusal.find("detached") != std::string::npos);
        // What rev-list --parents prints: the commit, then its parents.
        CHECK(headParentsArgs() ==
              std::vector<std::string>({"rev-list", "--parents", "-n", "1", "HEAD", "--"}));
        CHECK(parentCount(h40 + "\n") == 0 && parentCount(h40 + " " + z40 + "\n") == 1 &&
              parentCount(h40 + " " + z40 + " " + h40) == 2);
        CHECK(parentCount("") == -1 && parentCount("fatal: bad\n") == -1 &&
              parentCount(h40 + "\n" + h40 + "\n") == -1);
        CHECK(headMessageArgs()[0] == "log" && hasArg(headMessageArgs(), "--format=%B"));
    }

    GROUP("git-ui:stash");
    {
        Snapshot h;
        h.inRepository = true;
        applyStatus(h, Git::parseStatus(nul({
            "# branch.oid " + h40, "# branch.head main",
            "1 .M N..." + modes + h40 + " " + h40 + " a.txt", "? new.txt"})));
        ActionPlan p = stashPlan(h, "");
        CHECK(p.args == std::vector<std::string>({"stash", "push", "--include-untracked"}));
        CHECK(p.doneText == "Stashed the changes." && p.question.empty());
        CHECK(Git::timeoutFor(p.args) == 120);
        p = stashPlan(h, "  half done \n");
        CHECK(p.args == std::vector<std::string>({"stash", "push", "--include-untracked", "-m",
                                                  "half done"}));
        CHECK(p.doneText == "Stashed the changes as “half done”.");
        Snapshot clean;
        clean.inRepository = true;
        applyStatus(clean, Git::parseStatus(nul({"# branch.oid " + h40, "# branch.head main"})));
        CHECK(stashPlan(clean, "").refusal == "There are no changes to stash.");
        Snapshot first;
        first.inRepository = true;
        applyStatus(first, Git::parseStatus(nul({"# branch.oid (initial)", "# branch.head main",
                                                 "? new.txt"})));
        CHECK(stashPlan(first, "").args.empty() &&
              stashPlan(first, "").refusal.find("before the first commit") != std::string::npos);
        // Pop: the newest; none to pop is refused.
        CHECK(popStashPlan(clean, 0).refusal == "There is no stash to pop.");
        ActionPlan pop = popStashPlan(clean, 1);
        CHECK(pop.args == std::vector<std::string>({"stash", "pop"}) &&
              pop.doneText == "Popped the stash." && Git::timeoutFor(pop.args) == 120);
        CHECK(popStashPlan(clean, 3).doneText == "Popped the newest stash; 2 stashes are left.");
        CHECK(popStashPlan(clean, 2).doneText == "Popped the newest stash; 1 stash is left.");
        CHECK(stashListArgs() == std::vector<std::string>({"stash", "list", "--format=%gd"}));
        CHECK(countLines("stash@{0}\nstash@{1}\n") == 2 && countLines("") == 0 &&
              countLines("\n\n") == 0);
    }

    GROUP("git-ui:index-entry");
    {
        CHECK(indexEntryArgs("a b.txt") == std::vector<std::string>(
                  {"--literal-pathspecs", "ls-files", "--stage", "-z", "--", "a b.txt"}));
        IndexEntry e = parseIndexEntry("100644 " + h40 + " 0\ta b.txt" + std::string(1, '\0'));
        CHECK(e.ok && e.mode == "100644" && e.object == h40);
        CHECK(parseIndexEntry("100755 " + h40 + " 0\tx" + std::string(1, '\0')).ok);
        // Not tracked, a conflict (three stages), a link, a submodule: no bars.
        CHECK(!parseIndexEntry("").ok);
        CHECK(!parseIndexEntry(nul({"100644 " + h40 + " 1\tc", "100644 " + h40 + " 2\tc",
                                    "100644 " + h40 + " 3\tc"})).ok);
        CHECK(!parseIndexEntry("120000 " + h40 + " 0\tlink" + std::string(1, '\0')).ok);
        CHECK(!parseIndexEntry("160000 " + h40 + " 0\tlib" + std::string(1, '\0')).ok);
        CHECK(!parseIndexEntry("100644 nothex 0\tx" + std::string(1, '\0')).ok);
        CHECK(blobArgs(h40) == std::vector<std::string>({"cat-file", "blob", h40}));
        CHECK(Git::timeoutFor(blobArgs(h40)) == 30);
        CHECK(hashObjectArgs("d/f.txt") ==
              std::vector<std::string>({"hash-object", "-w", "--stdin", "--path", "d/f.txt"}));
        CHECK(updateIndexArgs(e, z40 + "\n", "a,b.txt") ==
              std::vector<std::string>({"update-index", "--cacheinfo", "100644," + z40 + ",a,b.txt"}));
        CHECK(Git::timeoutFor(hashObjectArgs("x")) == 120 &&
              Git::timeoutFor(updateIndexArgs(e, z40, "x")) == 120);
    }
}
