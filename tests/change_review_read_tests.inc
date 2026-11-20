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
    // Pictures (`review-read:images`): binary files with a picture's
    // extension whose versions the GUI's reader sizes are shown as them,
    // their bytes kept; a version it cannot read keeps the binary note.
    GROUP("review-read:images");
    {
        const std::string pics = root + "/pics";
        CHECK(sh("mkdir pics && cd pics && git init -q . && git config user.email a@b && "
                 "git config user.name n && printf 'PIC\\000\\004\\003one' >pic.png && "
                 "printf 'PIC\\000\\002\\002bad' >bad.png && git add . && git commit -q -m one && "
                 "printf 'PIC\\000\\010\\006two!' >pic.png && printf '\\000junk' >bad.png && "
                 "printf 'PIC\\000\\002\\002' >new.png && git add new.png"));
        int asked = 0;
        ImageReading images;
        images.rowHeight = 20;
        images.size = [&](const std::string& b) {
            asked++;
            ImageSide s;
            if (b.size() >= 6 && b.compare(0, 4, std::string("PIC\0", 4)) == 0) {
                s.width = (unsigned char)b[4];
                s.height = (unsigned char)b[5];
            }
            return s;
        };
        Read r = readUncommitted(pics, run, images);
        auto find = [&](const Read& rd, const std::string& path) -> long {
            for (size_t i = 0; i < rd.diffs.size(); i++)
                if (rd.diffs[i].file.path == path) return (long)i;
            return -1;
        };
        const long pic = find(r, "pic.png"), bad = find(r, "bad.png"), added = find(r, "new.png");
        CHECK(pic >= 0 && bad >= 0 && added >= 0);
        if (pic >= 0 && bad >= 0 && added >= 0) {
            const File& f = r.diffs[(size_t)pic].file;
            CHECK(f.binary && f.image && f.oldImage.shown && f.newImage.shown);
            CHECK(f.oldImage.width == 4 && f.oldImage.height == 3 && f.oldImage.bytes == 9);
            CHECK(f.newImage.width == 8 && f.newImage.height == 6 && f.newImage.bytes == 10);
            CHECK(f.imageRows == imageRows(f, 20) && f.imageRows >= 2);
            CHECK(r.extras[(size_t)pic].oldImage == std::string("PIC\0\x04\x03one", 9));
            CHECK(r.extras[(size_t)pic].newImage == std::string("PIC\0\x08\x06two!", 10));
            CHECK(imageLabel(f, false) == "Before: 4 \xC3\x97 3 pixels, 9 bytes");
            // Its new version unread as a picture: the binary file's note.
            const File& b = r.diffs[(size_t)bad].file;
            CHECK(b.binary && !b.image && r.extras[(size_t)bad].oldImage.empty());
            // A new file staged: its one picture, against HEAD's nothing.
            const File& n = r.diffs[(size_t)added].file;
            CHECK(n.section == Section::Staged && n.image && !n.oldImage.shown && n.newImage.shown);
            CHECK(n.newImage.width == 2 && n.newImage.height == 2 && n.newImage.bytes == 6);
            const Page page = layout(r.diffs, {});
            size_t imageRowsLaid = 0;
            for (const PageRow& row : page.rows)
                if (row.kind == RowKind::Image && row.file == (size_t)pic) imageRowsLaid++;
            CHECK(imageRowsLaid == f.imageRows);
        }
        // Without a reader nothing is asked and no file is a picture.
        const int before = asked;
        Read plain = readUncommitted(pics, run);
        CHECK(asked == before);
        for (const FileDiff& d : plain.diffs) CHECK(!d.file.image && plain.extras.size() == plain.diffs.size());
    }
    // A commit's page (the Mac's ReadCommit, now the core's): its header,
    // its files against its first parent, Plain, from blobs only; a root
    // commit against nothing; its pictures as pictures.
    GROUP("review-read:commit");
    {
        const std::string cdir = root + "/commits";
        CHECK(sh("mkdir commits && cd commits && git init -q . && git config user.email a@b && "
                 "git config user.name Ann && git config commit.gpgsign false && "
                 "printf 'one\\ntwo\\nthree\\n' >a.txt && printf 'PIC\\000\\002\\001x' >p.png && "
                 "git add . && git commit -q -m first && "
                 "printf 'one\\nTWO\\nthree\\n' >a.txt && printf 'PIC\\000\\004\\003xy' >p.png && "
                 "printf 'new\\n' >n.txt && git add . && "
                 "git commit -q -m 'Second subject' -m 'The body line.' && "
                 "printf 'one\\nwork tree\\nthree\\n' >a.txt"));
        const std::string head = run(cdir, {"rev-parse", "HEAD"}, "").out.substr(0, 40);
        const std::string first = run(cdir, {"rev-parse", "HEAD~1"}, "").out.substr(0, 40);
        ImageReading images;
        images.size = [](const std::string& b) {
            ImageSide s;
            if (b.size() >= 6 && b.compare(0, 4, std::string("PIC\0", 4)) == 0) {
                s.width = (unsigned char)b[4];
                s.height = (unsigned char)b[5];
            }
            return s;
        };
        CommitRead c = readCommit(cdir, head, run, images);
        CHECK(c.read.error.empty() && c.commit.hash == head && c.commit.author == "Ann");
        CHECK(c.commit.message.find("Second subject") == 0 && c.commit.message.find("The body line.") != std::string::npos);
        CHECK(c.commit.parents.size() == 1 && c.commit.parents[0] == first);
        CHECK(c.read.diffs.size() == 3 && c.read.extras.size() == 3);
        for (size_t i = 0; i < c.read.diffs.size(); i++) {
            const File& f = c.read.diffs[i].file;
            CHECK(f.section == Section::Plain);
            if (f.path == "a.txt") {
                // The commit's text, not the work tree's.
                CHECK(f.letter == 'M' && QuickDiff::toUtf8(f.newText) == "one\nTWO\nthree\n");
                CHECK(c.read.diffs[i].added == 1 && c.read.diffs[i].removed == 1);
            }
            if (f.path == "n.txt") CHECK(f.letter == 'A' && !f.oldExists && f.newExists);
            if (f.path == "p.png") {
                CHECK(f.image && f.oldImage.width == 2 && f.newImage.width == 4 && f.newImage.height == 3);
                CHECK(c.read.extras[i].oldImage == std::string("PIC\0\x02\x01x", 7));
                CHECK(c.read.extras[i].newImage == std::string("PIC\0\x04\x03xy", 8));
            }
        }
        // The root commit: every file added.
        CommitRead r1 = readCommit(cdir, first, run);
        CHECK(r1.read.error.empty() && r1.commit.parents.empty() && r1.read.diffs.size() == 2);
        for (const FileDiff& d : r1.read.diffs) CHECK(d.file.letter == 'A' && !d.file.image);
        // A name that is no commit, and a folder that is no repository.
        CHECK(!readCommit(cdir, std::string(40, 'e'), run).read.error.empty());
        CHECK(readCommit("/nonexistent-mutible-review", head, run).read.error ==
              "This folder is not in a git repository.");
    }

    // Not a repository.
    CHECK(readUncommitted("/nonexistent-mutible-review", run).error == "This folder is not in a git repository.");
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
    CHECK(readWorktree("/nonexistent-mutible-wt", "main", run).read.error ==
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

    // Round 10's defect 6: the files checked are the files applied. The
    // page read a.txt (new) and keep.txt; then the agent changed m.txt,
    // where the user has an edit of their own in main, and made b.txt. The
    // apply is refused, naming both, and main keeps only the user's edit.
    GROUP("review-worktree:apply-checked");
    CHECK(sh(mainDir, "printf '1\\n2\\n3\\n4\\n5\\n6\\n7\\n8\\n9\\n' >m.txt && git add m.txt && "
                      "git commit -q -m m && git worktree add -q -b late ../late && "
                      "git worktree add -q -b mv ../mv"));
    const std::string lateDir = base + "/late", mvDir = base + "/mv";
    CHECK(sh(lateDir, "printf 'agent a\\n' >a.txt && printf 'k3\\n' >keep.txt"));
    WorktreeRead l = readWorktree(lateDir, "main", run);
    CHECK(l.read.error.empty() && l.read.diffs.size() == 2);
    CHECK(planApplyToMain(l, "main", "late", run).refusal.empty());
    CHECK(sh(lateDir, "printf '1\\n2\\n3\\n4\\n5\\n6\\n7\\n8\\nagent 9\\n' >m.txt && printf 'b\\n' >b.txt"));
    CHECK(sh(mainDir, "printf 'mine 1\\n2\\n3\\n4\\n5\\n6\\n7\\n8\\n9\\n' >m.txt"));
    // What the diff is held to: keep.txt only, never m.txt.
    const std::string held = run(lateDir, GitUi::worktreeDiffArgs(l.baseRef, {"keep.txt"}), "").out;
    CHECK(held.find("keep.txt") != std::string::npos && held.find("m.txt") == std::string::npos);
    ApplyResult stale = applyToMain(l, "main", run);
    CHECK(!stale.ok);
    CHECK(stale.text == GitUi::applyStaleText({"b.txt", "m.txt"}));
    CHECK(stale.text.rfind("b.txt and m.txt were changed in the worktree after this page was read", 0) == 0);
    CHECK(slurp(mainDir + "/m.txt") == "mine 1\n2\n3\n4\n5\n6\n7\n8\n9\n");
    CHECK(slurp(mainDir + "/a.txt").empty() && slurp(mainDir + "/b.txt").empty());
    CHECK(slurp(mainDir + "/keep.txt") == "k\n");
    // The pure half: a rename's source counts, as does an untracked file.
    {
        ApplyFiles asked;
        asked.paths = {"x.txt", "u.txt"};
        asked.untracked = {"u.txt"};
        const std::string z(40, '0');
        std::string raw = ":100644 100644 " + z + " " + z + " M";
        for (const char* p : {"x.txt"}) raw += std::string(1, '\0') + p + std::string(1, '\0');
        raw += ":100644 100644 " + z + " " + z + " R100";
        for (const char* p : {"old.txt", "new.txt"}) raw += std::string(1, '\0') + p;
        raw += std::string(1, '\0');
        ApplyNow n = applyNow(asked, raw, std::string("u.txt\0v.txt\0", 12));
        CHECK((n.extra == std::vector<std::string>{"new.txt", "old.txt", "v.txt"}));
        CHECK((n.untracked == std::vector<std::string>{"u.txt"}));
    }

    // Read again, everything in it; main's own edit put back, so the
    // question is asked; then the user edits keep.txt in main before the
    // answer: refused, naming it, nothing brought in.
    CHECK(sh(mainDir, "git checkout -q -- m.txt"));
    WorktreeRead l2 = readWorktree(lateDir, "main", run);
    CHECK(l2.read.diffs.size() == 4);
    CHECK(planApplyToMain(l2, "main", "late", run).refusal.empty());
    CHECK(sh(mainDir, "printf 'k mine\\n' >keep.txt"));
    ApplyResult busy = applyToMain(l2, "main", run);
    CHECK(!busy.ok && busy.text.find("uncommitted changes of its own in keep.txt") != std::string::npos);
    CHECK(slurp(mainDir + "/a.txt").empty() && slurp(mainDir + "/m.txt") == "1\n2\n3\n4\n5\n6\n7\n8\n9\n");
    CHECK(sh(mainDir, "git checkout -q -- keep.txt"));
    ApplyResult all = applyToMain(l2, "main", run);
    CHECK(all.ok && all.text == GitUi::applyDoneText(4, {}, "main"));
    CHECK(slurp(mainDir + "/m.txt") == "1\n2\n3\n4\n5\n6\n7\n8\nagent 9\n");
    CHECK(slurp(mainDir + "/b.txt") == "b\n" && slurp(mainDir + "/a.txt") == "agent a\n");
    CHECK(sh(mainDir, "git reset -q --hard && git clean -fdq"));

    // A rename that applies cleanly beside a clash: after `apply --3way`
    // both of its sides are unstaged, not only the new path (the source's
    // deletion stayed staged).
    GROUP("review-worktree:apply-rename");
    CHECK(sh(mvDir, id + "git mv m.txt m2.txt && printf 'one\\ntwo agent\\nthree\\n' >base.txt && "
                         "git commit -qam mv"));
    CHECK(sh(mainDir, "printf 'one\\ntwo again\\nthree\\n' >base.txt && git commit -qam again"));
    WorktreeRead mv = readWorktree(mvDir, "main", run);
    CHECK(mv.read.error.empty());
    CHECK(planApplyToMain(mv, "main", "mv", run).refusal.empty());
    ApplyResult moved = applyToMain(mv, "main", run);
    CHECK(moved.ok && (moved.conflicts == std::vector<std::string>{"base.txt"}));
    const std::string cached = run(mainDir, {"diff", "--cached", "--name-only"}, "").out;
    if (cached != "base.txt\n") std::printf("    cached after the rename: %s\n", cached.c_str());
    CHECK(cached == "base.txt\n");
    const std::string st = run(mainDir, {"status", "--porcelain"}, "").out;
    CHECK(st.find(" D m.txt") != std::string::npos && st.find("?? m2.txt") != std::string::npos);
    CHECK(slurp(mainDir + "/m2.txt") == "1\n2\n3\n4\n5\n6\n7\n8\n9\n");
    CHECK(std::system(("rm -rf " + shq(base) + " " + shq(tmpIn) + " " + shq(tmpOut) + " " + shq(tmpErr)).c_str()) == 0);
}
