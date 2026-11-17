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
