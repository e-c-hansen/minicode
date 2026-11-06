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
}
