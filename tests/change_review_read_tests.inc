// change_review_read_tests.inc — the `review-read:*` groups of run_tests.cpp:
// ChangeReviewRead.h against a real git in a scratch repository: what the
// review of uncommitted changes reads (the staged list, then the changes,
// each file's block), and what Stage, Unstage and Discard of one block write
// to the index and the work tree, and refuse when either moved since the
// view was read. Skipped without git. Included there.

static void testChangeReviewRead() {
    using namespace ChangeReview;
    GROUP("review-read:git");
    if (std::system("git --version >/dev/null 2>&1") != 0) return;
    char tmpl[] = "/tmp/mc-review-read-XXXXXX";
    const char* made = mkdtemp(tmpl);
    CHECK(made != nullptr);
    if (!made) return;
    const std::string root = made;
    auto shq = [](const std::string& s) {
        std::string o = "'";
        for (char c : s) o += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return o + "'";
    };
    const std::string tmpIn = root + ".in", tmpOut = root + ".out", tmpErr = root + ".err";
    GitRunner run = [&](const std::string& dir, const std::vector<std::string>& args,
                        const std::string& input) {
        std::ofstream(tmpIn, std::ios::binary) << input;
        std::string cmd = "cd " + shq(dir) + " && GIT_OPTIONAL_LOCKS=0 git";
        for (const std::string& a : args) cmd += " " + shq(a);
        cmd += " <" + shq(tmpIn) + " >" + shq(tmpOut) + " 2>" + shq(tmpErr);
        GitOut r;
        r.ok = std::system(cmd.c_str()) == 0;
        auto slurp = [](const std::string& p) {
            std::ifstream f(p, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        };
        r.out = slurp(tmpOut);
        r.err = slurp(tmpErr);
        return r;
    };
    auto sh = [&](const std::string& c) { return std::system(("cd " + shq(root) + " && " + c).c_str()) == 0; };
    auto slurp = [](const std::string& p) {
        std::ifstream f(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    };
    CHECK(sh("git init -q . && git config user.email a@b && git config user.name n && "
             "printf 'a\\nb\\nc\\nd\\ne\\nf\\ng\\nh\\ni\\nj\\nk\\nl\\n' >f.txt && printf 'x\\n' >g.txt && "
             "git add . && git commit -q -m one"));
    // Two edits far apart in f.txt (two blocks), g.txt changed and staged.
    CHECK(sh("printf 'a\\nB\\nc\\nd\\ne\\nf\\ng\\nh\\ni\\nj\\nk\\nL\\n' >f.txt && printf 'y\\n' >g.txt && "
             "git add g.txt && printf 'new\\n' >n.txt"));
    Read r = readUncommitted(root, run);
    CHECK(r.error.empty());
    CHECK(r.diffs.size() == 3 && r.extras.size() == 3);
    if (r.diffs.size() == 3) {
        CHECK(r.diffs[0].file.section == Section::Staged && r.diffs[0].file.path == "g.txt");
        CHECK(r.diffs[1].file.section == Section::Changes && r.diffs[1].file.path == "f.txt");
        CHECK(r.diffs[1].blocks.size() == 2);
        CHECK(r.diffs[2].file.untracked && r.diffs[2].file.path == "n.txt");
        // Stage the first block of f.txt: the index has b changed, l not.
        CHECK(stageText(r, 1, blockResult(r.diffs[1], 0, Op::Stage), run).empty());
        GitOut cached = run(root, {"show", ":f.txt"}, "");
        CHECK(cached.out == "a\nB\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\n");
        CHECK(slurp(root + "/f.txt") == "a\nB\nc\nd\ne\nf\ng\nh\ni\nj\nk\nL\n");
        // The view is now old: another stage of the same file is refused.
        const std::string stale = stageText(r, 1, blockResult(r.diffs[1], 1, Op::Stage), run);
        CHECK(stale == staleText("f.txt", true));
        // Read again; discard the remaining block of f.txt.
        Read r2 = readUncommitted(root, run);
        size_t fi = 99;
        for (size_t i = 0; i < r2.diffs.size(); i++)
            if (r2.diffs[i].file.path == "f.txt" && r2.diffs[i].file.section == Section::Changes) fi = i;
        CHECK(fi < r2.diffs.size());
        if (fi < r2.diffs.size()) {
            CHECK(r2.diffs[fi].blocks.size() == 1);
            std::string trashedTo;
            int trashCalls = 0;
            const std::string why = discardText(
                r2, fi, blockResult(r2.diffs[fi], 0, Op::Discard), run,
                [&](std::string* where, std::string*) {
                    trashCalls++;
                    *where = root + "/copy";
                    return sh("cp f.txt copy");
                },
                &trashedTo);
            CHECK(why.empty() && trashCalls == 1 && trashedTo == root + "/copy");
            CHECK(slurp(root + "/f.txt") == "a\nB\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\n");
            CHECK(slurp(root + "/copy") == "a\nB\nc\nd\ne\nf\ng\nh\ni\nj\nk\nL\n");
            // The file changed meanwhile: nothing is trashed or written.
            CHECK(sh("printf 'z\\n' >>f.txt"));
            Read r3 = r2;
            trashCalls = 0;
            const std::string again = discardText(
                r3, fi, u"x", run, [&](std::string*, std::string*) { trashCalls++; return true; }, nullptr);
            CHECK(again == staleText("f.txt", false) && trashCalls == 0);
        }
        // Unstage g.txt's block: the index goes back to HEAD's text.
        CHECK(stageText(r, 0, blockResult(r.diffs[0], 0, Op::Unstage), run).empty());
        CHECK(run(root, {"show", ":g.txt"}, "").out == "x\n");
    }
    // Not a repository.
    CHECK(readUncommitted("/nonexistent-minicode-review", run).error == "This folder is not in a git repository.");
    CHECK(std::system(("rm -rf " + shq(root) + " " + shq(tmpIn) + " " + shq(tmpOut) + " " + shq(tmpErr)).c_str()) == 0);
}

// A worktree against main (Compare with Main) and Apply to Main, over a real
// scratch repository with a main checkout and a worktree: `review-worktree:*`.
static void testWorktreeReview() {
    using namespace ChangeReview;
    GROUP("review-worktree:git");
    if (std::system("git --version >/dev/null 2>&1") != 0) return;
    char tmpl[] = "/tmp/mc-review-wt-XXXXXX";
    const char* made = mkdtemp(tmpl);
    CHECK(made != nullptr);
    if (!made) return;
    const std::string base = made, mainDir = base + "/main", wtDir = base + "/wt";
    auto shq = [](const std::string& s) {
        std::string o = "'";
        for (char c : s) o += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return o + "'";
    };
    const std::string tmpIn = base + ".in", tmpOut = base + ".out", tmpErr = base + ".err";
    auto slurp = [](const std::string& p) {
        std::ifstream f(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    };
    GitRunner run = [&](const std::string& dir, const std::vector<std::string>& args,
                        const std::string& input) {
        std::ofstream(tmpIn, std::ios::binary) << input;
        std::string cmd = "cd " + shq(dir) + " && GIT_OPTIONAL_LOCKS=0 git";
        for (const std::string& a : args) cmd += " " + shq(a);
        cmd += " <" + shq(tmpIn) + " >" + shq(tmpOut) + " 2>" + shq(tmpErr);
        const int raw = std::system(cmd.c_str());
        GitOut r;
        r.status = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
        r.ok = r.status == 0;
        r.out = slurp(tmpOut);
        r.err = slurp(tmpErr);
        return r;
    };
    auto sh = [&](const std::string& dir, const std::string& c) {
        return std::system(("cd " + shq(dir) + " && " + c).c_str()) == 0;
    };
    const std::string id = "git config user.email a@b && git config user.name n && ";
    CHECK(sh(base, "mkdir main && cd main && git init -q -b main . && " + id +
                       "printf 'one\\ntwo\\nthree\\n' >base.txt && printf 'k\\n' >keep.txt && "
                       "git add . && git commit -q -m one"));
    CHECK(sh(mainDir, "git worktree add -q -b agent ../wt"));
    // The agent: a committed new file and edit, an uncommitted edit, an untracked file.
    CHECK(sh(wtDir, id + "printf 'new\\n' >agent.txt && printf 'one\\ntwo changed\\nthree\\n' >base.txt && "
                       "git add . && git commit -q -m agent && printf 'k2\\n' >keep.txt && "
                       "printf 'u\\n' >untracked.txt"));
    // Main moves on, in a file the agent did not touch.
    CHECK(sh(mainDir, "printf 'later\\n' >later.txt && git add . && git commit -q -m later"));

    WorktreeRead w = readWorktree(wtDir, "main", run);
    CHECK(w.read.error.empty());
    CHECK(w.read.diffs.size() == 4);
    std::vector<std::string> names;
    for (const FileDiff& d : w.read.diffs) {
        names.push_back(d.file.path);
        CHECK(d.file.section == Section::Plain);
    }
    CHECK((names == std::vector<std::string>{"agent.txt", "base.txt", "keep.txt", "untracked.txt"}));
    CHECK(w.baseLabel.find("main at the branch point") == 0);
    // git names main's checkout resolved; on the Mac the scratch folder is
    // under /var, a link to /private/var (CI's macOS core tests, #104).
    auto resolved = [](const std::string& p) {
        char buf[PATH_MAX];
        return realpath(p.c_str(), buf) ? std::string(buf) : p;
    };
    CHECK(resolved(w.mainPath) == resolved(mainDir));
    if (w.read.diffs.size() == 4) {
        CHECK(w.read.diffs[0].file.letter == 'A' && !w.read.diffs[0].file.oldExists);
        // The left side is main where the branch left it, not main's tip.
        CHECK(w.read.diffs[1].file.oldText == u"one\ntwo\nthree\n");
        CHECK(w.read.diffs[1].file.newText == u"one\ntwo changed\nthree\n");
        CHECK(w.read.diffs[2].file.newText == u"k2\n");
        CHECK(w.read.diffs[3].file.untracked && w.read.diffs[3].file.newText == u"u\n");
    }
    CHECK(readWorktree("/nonexistent-minicode-wt", "main", run).read.error ==
          "This worktree is not a git checkout any more.");
    CHECK(readWorktree(wtDir, "", run).read.error == "There is no main branch to compare with.");

    // Refused while main has changes of its own in those files, naming them.
    CHECK(sh(mainDir, "printf 'mine\\n' >>base.txt"));
    GitUi::ApplyPlan refused = planApplyToMain(w, "main", "wt", run);
    CHECK(!refused.refusal.empty() && refused.question.empty());
    CHECK(refused.refusal.find("base.txt") != std::string::npos);
    CHECK(sh(mainDir, "git checkout -q base.txt"));
    GitUi::ApplyPlan plan = planApplyToMain(w, "main", "wt", run);
    CHECK(plan.refusal.empty() && !plan.question.empty() && plan.button == "Apply to Main");
    CHECK(plan.question.find("4 files") != std::string::npos);

    // Applied: main's checkout has the work, uncommitted and unstaged.
    ApplyResult done = applyToMain(w, "main", run);
    CHECK(done.ok && done.conflicts.empty());
    CHECK(done.text == GitUi::applyDoneText(4, {}, "main"));
    CHECK(slurp(mainDir + "/agent.txt") == "new\n");
    CHECK(slurp(mainDir + "/base.txt") == "one\ntwo changed\nthree\n");
    CHECK(slurp(mainDir + "/keep.txt") == "k2\n");
    CHECK(slurp(mainDir + "/untracked.txt") == "u\n");
    CHECK(slurp(mainDir + "/later.txt") == "later\n");
    CHECK(run(mainDir, {"diff", "--cached", "--name-only"}, "").out.empty());
    CHECK(run(mainDir, {"log", "--format=%s"}, "").out == "later\none\n");
    // The same again is refused now: main has those changes.
    CHECK(!planApplyToMain(w, "main", "wt", run).refusal.empty());

    // A clash: main committed a different edit of the same line. The plain
    // apply fails, the three-way one leaves marks in base.txt and the rest
    // arrives unstaged.
    CHECK(sh(mainDir, "git checkout -q . && git clean -fdq && printf 'one\\ntwo main\\nthree\\n' >base.txt && "
                      "git add . && git commit -q -m clash"));
    WorktreeRead w2 = readWorktree(wtDir, "main", run);
    CHECK(planApplyToMain(w2, "main", "wt", run).refusal.empty());
    ApplyResult clash = applyToMain(w2, "main", run);
    CHECK(clash.ok);
    CHECK((clash.conflicts == std::vector<std::string>{"base.txt"}));
    CHECK(clash.text == GitUi::applyDoneText(4, clash.conflicts, "main"));
    CHECK(slurp(mainDir + "/base.txt").find("<<<<<<<") != std::string::npos);
    CHECK(slurp(mainDir + "/agent.txt") == "new\n");
    // Only the conflict is in the index (as unmerged); the rest was unstaged.
    CHECK(run(mainDir, {"diff", "--cached", "--name-only"}, "").out == "base.txt\n");

    // Nothing to bring: a worktree with no changes.
    CHECK(sh(mainDir, "git reset -q --hard && git clean -fdq"));
    CHECK(sh(base, "cd main && git worktree add -q -b quiet ../quiet"));
    WorktreeRead q = readWorktree(base + "/quiet", "main", run);
    CHECK(q.read.error.empty() && q.read.diffs.empty());
    CHECK(!planApplyToMain(q, "main", "quiet", run).refusal.empty());
    CHECK(std::system(("rm -rf " + shq(base) + " " + shq(tmpIn) + " " + shq(tmpOut) + " " + shq(tmpErr)).c_str()) == 0);
}
