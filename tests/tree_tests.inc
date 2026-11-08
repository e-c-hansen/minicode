// tree_tests.inc — the `tree:changed-folders`, `tree:spelling`,
// `git:decorations` and `settings:tree-git` groups of run_tests.cpp: which
// folders the file tree lists again for a batch of FSEvents, and the colors
// git status gives its rows. Included there.

static void testTreeRefresh() {
    using TreeRefresh::Event;
    using TreeRefresh::plan;
    GROUP("tree:spelling");
    {
        using TreeRefresh::treeSpelling;
        // The disk's /private/tmp is the tree's /tmp; trailing slashes go.
        CHECK(treeSpelling("/private/tmp/p/src/", "/tmp/p", "/private/tmp/p") == "/tmp/p/src");
        CHECK(treeSpelling("/private/tmp/p", "/tmp/p", "/private/tmp/p") == "/tmp/p");
        CHECK(treeSpelling("/private/tmp/p//", "/tmp/p", "/private/tmp/p/") == "/tmp/p");
        // Not under the real root, or the same spelling: unchanged.
        CHECK(treeSpelling("/private/tmp/pq/x", "/tmp/p", "/private/tmp/p") == "/private/tmp/pq/x");
        CHECK(treeSpelling("/Users/me/p/a/", "/Users/me/p", "/Users/me/p") == "/Users/me/p/a");
        CHECK(treeSpelling("/a/b", "", "") == "/a/b");
        CHECK(treeSpelling("/", "/tmp/p", "/private/tmp/p") == "/");
    }

    GROUP("tree:changed-folders");
    {
        const std::string root = "/p";
        const std::set<std::string> loaded = {"/p", "/p/src", "/p/src/deep", "/p/src x",
                                              "/p/docs"};
        auto join = [](const std::vector<std::string> &v) {
            std::string s;
            for (const std::string &x : v) s += (s.empty() ? "" : " ") + x;
            return s;
        };
        // A loaded folder named: it is listed again, and its own row (in a
        // listed parent) restatted for its date.
        TreeRefresh::Plan pl = plan({{"/p/src/", false}}, loaded, root);
        CHECK(!pl.everything && join(pl.list) == "/p/src" && join(pl.restat) == "/p/src");
        // A folder not loaded but on show (its parent is): only its row.
        pl = plan({{"/p/build", false}}, loaded, root);
        CHECK(pl.list.empty() && join(pl.restat) == "/p/build");
        // Deeper than anything loaded: nothing at all.
        pl = plan({{"/p/build/obj/x", false}, {"/p/node_modules/a/b", false}}, loaded, root);
        CHECK(pl.list.empty() && pl.restat.empty() && !pl.everything);
        // The root: listed, no row of its own.
        pl = plan({{"/p", false}}, loaded, root);
        CHECK(join(pl.list) == "/p" && pl.restat.empty());
        // A row in a folder that is listed anyway needs no stat of its own.
        pl = plan({{"/p/src", false}, {"/p/src/deep", false}, {"/p/src/new", false}}, loaded, root);
        CHECK(join(pl.list) == "/p/src /p/src/deep");
        CHECK(join(pl.restat) == "/p/src");
        // Each once, and in order, a folder before those inside it.
        pl = plan({{"/p/src/deep", false}, {"/p/docs/", false}, {"/p/src/deep/", false},
                   {"/p/src", false}},
                  loaded, root);
        CHECK(join(pl.list) == "/p/docs /p/src /p/src/deep");
        // A subtree event lists every loaded folder at or below it, and no
        // folder that only shares its name's start ("/p/src x").
        pl = plan({{"/p/src", true}}, loaded, root);
        CHECK(join(pl.list) == "/p/src /p/src/deep" && join(pl.restat) == "/p/src");
        pl = plan({{"/p/src/gone", true}}, loaded, root);
        CHECK(pl.list.empty() && join(pl.restat) == "/p/src/gone");
        // A subtree event for the root (or the events the system dropped)
        // is a rescan of everything.
        pl = plan({{"/p/src", false}, {"/p/", true}}, loaded, root);
        CHECK(pl.everything);
        // Outside the root: left out, a parent of the root too, unless
        // everything below it is to be looked at (events were dropped).
        pl = plan({{"/", false}, {"/q/src", false}, {"/pq", false}, {"", false}, {"/q", true}},
                  loaded, root);
        CHECK(!pl.everything && pl.list.empty() && pl.restat.empty());
        CHECK(plan({{"/", true}}, loaded, root).everything);
        // A root of "/" holds everything.
        pl = plan({{"/etc", false}}, {"/", "/etc"}, "/");
        CHECK(join(pl.list) == "/etc" && join(pl.restat) == "/etc");
        // A batch of 10,000 events in a tree of 10,000 loaded folders is
        // planned at once.
        std::set<std::string> many;
        std::vector<Event> evs;
        for (int i = 0; i < 10000; i++) {
            many.insert("/p/d" + std::to_string(i));
            evs.push_back({"/p/d" + std::to_string(i) + "/sub", false});
        }
        many.insert("/p");
        const double t0 = cpuMs();
        pl = plan(evs, many, root);
        const double ms = cpuMs() - t0;
        CHECK(pl.list.empty() && pl.restat.size() == 10000);
        CHECK_TIME(ms, 500);
    }

    GROUP("git:decorations");
    {
        using Git::Decoration;
        // What `git status --porcelain=v2 -z --ignored=matching` prints for
        // a repository with a change of every kind.
        auto rec = [](const std::string &s) { return s + std::string(1, '\0'); };
        const std::string h = " N... 100644 100644 100644 aaaa bbbb ";
        std::string out = rec("1 .M" + h + "src/main.c") +          // modified in the work tree
                          rec("1 M." + h + "src/staged.c") +        // modified in the index
                          rec("1 A." + h + "src/new.c") +           // added
                          rec("1 AM" + h + "src/newer.c") +         // added, then changed
                          rec("1 .D" + h + "docs/gone.md") +        // deleted
                          rec("1 D." + h + "kept.txt") +            // removed from the index...
                          rec("1 .T" + h + "link") +                // a type change
                          rec("1 .A" + h + "intent.txt") +          // git add -N
                          rec("2 R. N... 100644 100644 100644 aaaa bbbb R100 lib/renamed.c") +
                          rec("lib/old.c") +
                          rec("u UU N... 100644 100644 100644 100644 a b c deep/x/conflict.c") +
                          rec("? kept.txt") +                       // ...and left untracked
                          rec("? notes.txt") + rec("? scratch/") + rec("? scratch2/inner/") +
                          rec("! build/") + rec("! src/main.o") + rec("! scratch/cache/");
        Git::Status st = Git::parseStatus(out);
        CHECK(st.ignored.size() == 3 && st.ignored[0] == "build/");
        // The panel's list is what it was: ignored paths are not entries.
        CHECK(st.entries.size() == 14);
        Git::Decorations d = Git::Decorations::fromStatus(st);
        CHECK(d.file("src/main.c") == Decoration::Modified);
        CHECK(d.file("src/staged.c") == Decoration::Modified);
        CHECK(d.file("src/new.c") == Decoration::Added && d.file("src/newer.c") == Decoration::Added);
        CHECK(d.file("docs/gone.md") == Decoration::Deleted);
        CHECK(d.file("kept.txt") == Decoration::Untracked);   // the later record
        CHECK(d.file("link") == Decoration::Modified && d.file("intent.txt") == Decoration::Added);
        CHECK(d.file("lib/renamed.c") == Decoration::Added && d.file("lib/old.c") == Decoration::None);
        CHECK(d.file("deep/x/conflict.c") == Decoration::Conflict);
        CHECK(d.file("notes.txt") == Decoration::Untracked);
        CHECK(d.file("src/clean.c") == Decoration::None && d.file("README.md") == Decoration::None);
        // Inside an untracked or ignored folder, at any depth.
        CHECK(d.file("scratch/a.txt") == Decoration::Untracked);
        CHECK(d.file("scratch/x/y/z.txt") == Decoration::Untracked);
        CHECK(d.file("scratch2/inner/f") == Decoration::Untracked);
        CHECK(d.file("scratch2/other") == Decoration::None);
        CHECK(d.file("build/out/app.o") == Decoration::Ignored);
        CHECK(d.file("src/main.o") == Decoration::Ignored);
        // The nearest folder git names wins.
        CHECK(d.file("scratch/cache/blob") == Decoration::Ignored);
        CHECK(d.folder("scratch/cache") == Decoration::Ignored);
        CHECK(d.file("buildx/a") == Decoration::None);   // a name's start is not a folder
        // Folders: their own state, else the strongest inside them;
        // deletions and ignored files color no folder.
        CHECK(d.folder("build") == Decoration::Ignored && d.folder("build/out") == Decoration::Ignored);
        CHECK(d.folder("scratch") == Decoration::Untracked);
        CHECK(d.folder("scratch2") == Decoration::Untracked);   // holds an untracked folder
        CHECK(d.folder("src") == Decoration::Modified);         // modified beats added
        CHECK(d.folder("lib") == Decoration::Added);
        CHECK(d.folder("deep") == Decoration::Conflict && d.folder("deep/x") == Decoration::Conflict);
        CHECK(d.folder("docs") == Decoration::None);
        CHECK(d.folder("") == Decoration::None && d.folder("nowhere") == Decoration::None);
        CHECK(std::string(Git::decorationName(Decoration::Conflict)) == "conflict" &&
              std::string(Git::decorationName(Decoration::None)).empty());
        // What a change on disk needs: none inside ignored folders.
        CHECK(d.ignored("build") && d.ignored("build/a/b") && d.ignored("src/main.o"));
        CHECK(!d.ignored("src") && !d.ignored("src/main.c") && !d.ignored("scratch"));
        // Comparing two statuses: the tree recolors only when they differ.
        Git::Decorations same = Git::Decorations::fromStatus(Git::parseStatus(out));
        CHECK(same == d);
        Git::Decorations other =
            Git::Decorations::fromStatus(Git::parseStatus(out + rec("? another.txt")));
        CHECK(other != d);
        Git::Decorations none = Git::Decorations::fromStatus(Git::parseStatus(""));
        CHECK(none.empty() && none.file("a") == Decoration::None && !none.ignored("a"));
        // A submodule with changes colors its own folder.
        Git::Decorations sub = Git::Decorations::fromStatus(
            Git::parseStatus(rec("1 .M S.M. 160000 160000 160000 aaaa bbbb vendor/lib")));
        CHECK(sub.folder("vendor/lib") == Decoration::Modified &&
              sub.folder("vendor") == Decoration::Modified);
        // The arguments: one status, ignored and untracked folders whole.
        const std::vector<std::string> args = Git::decorationArgs();
        CHECK(args.size() == 5 && args[0] == "status" && args[1] == "--porcelain=v2" &&
              args[2] == "-z" && args[3] == "--ignored=matching" &&
              args[4] == "--untracked-files=normal");
        CHECK(Git::timeoutFor(args) == 30);
        // 100,000 changed files are read and rolled up quickly.
        std::string big;
        for (int i = 0; i < 100000; i++)
            big += rec("1 .M" + h + "dir" + std::to_string(i % 100) + "/sub/f" + std::to_string(i));
        const double t0 = cpuMs();
        Git::Decorations bd = Git::Decorations::fromStatus(Git::parseStatus(big));
        const double ms = cpuMs() - t0;
        std::printf("  git:decorations: 100,000 changed files read in %.1f ms\n", ms);
        CHECK(bd.folder("dir7/sub") == Decoration::Modified && bd.file("dir7/sub/f7") == Decoration::Modified);
        CHECK_TIME(ms, 2000);
    }

    GROUP("settings:tree-git");
    {
        // VS Code's colors: its dark ones in a dark theme, light in a light one.
        Settings dark = Settings::parse("");
        CHECK(dark.treeGitColor(TreeGitColor::Modified).rgb() == 0xE2C08D);
        CHECK(dark.treeGitColor(TreeGitColor::Added).rgb() == 0x81B88B);
        CHECK(dark.treeGitColor(TreeGitColor::Untracked).rgb() == 0x73C991);
        CHECK(dark.treeGitColor(TreeGitColor::Deleted).rgb() == 0xC74E39);
        CHECK(dark.treeGitColor(TreeGitColor::Ignored).rgb() == 0x8C8C8C);
        CHECK(dark.treeGitColor(TreeGitColor::Conflict).rgb() == 0xE4676B);
        Settings light = Settings::parse("theme = light-plus\n");
        CHECK(light.treeGitColor(TreeGitColor::Modified).rgb() == 0x895503);
        CHECK(light.treeGitColor(TreeGitColor::Ignored).rgb() == 0x8E8E90);
        CHECK(light.treeGitColor(TreeGitColor::Conflict).rgb() == 0xAD0707);
        Settings monokai = Settings::parse("theme = monokai\n");
        CHECK(monokai.treeGitColor(TreeGitColor::Untracked).rgb() == 0x73C991);
        // The file's own color wins over the theme's.
        std::vector<SettingsError> errs;
        Settings mine = Settings::parse("theme = light-plus\ntree.git-modified = #112233\n"
                                        "tree.git-ignored = #44556680\n", &errs);
        CHECK(errs.empty());
        CHECK(mine.treeGitColor(TreeGitColor::Modified).rgb() == 0x112233);
        CHECK(mine.treeGitColor(TreeGitColor::Ignored).rgb() == 0x445566 &&
              mine.treeGitColor(TreeGitColor::Ignored).a < 0.6);
        CHECK(mine.treeGitColor(TreeGitColor::Added).rgb() == 0x587C0C);
        Settings::parse("tree.git-added = green\n", &errs);
        CHECK(errs.size() == 1 && errs[0].message.find("not a color") != std::string::npos);
        CHECK(Settings::isColorKey("tree.git-conflict") && Settings::isColorKey("tree.git-ignored"));
        // Adjust Settings on the tree marks them.
        const AppearanceSurface *tree = Settings::appearanceSurface("tree");
        int found = 0;
        for (const char *k : tree->keys)
            if (std::string(k).rfind("tree.git-", 0) == 0) found++;
        CHECK(found == kTreeGitColorCount);
    }
}
