// tree_tests.inc — the `tree:changed-folders`, `tree:spelling`,
// `tree:splices`, `tree:pacer`, `git:decorations` and `settings:tree-git`
// groups of run_tests.cpp: which folders the file tree lists again for a
// batch of FSEvents, how a fresh listing goes into a folder's rows and when
// it is made (the GTK port's tree), and the colors git status gives its rows.
// Included there.

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

    GROUP("tree:splices");
    {
        using TreeRefresh::Row;
        using TreeRefresh::Splice;
        using TreeRefresh::splices;
        // Rows as the GTK port makes them, with the name standing in for its
        // collation key (lowercase, as a case-folded key sorts).
        auto row = [](const std::string &name, bool folder = false) {
            std::string key = name;
            for (char &c : key)
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            return Row{folder, key, name};
        };
        auto sorted = [](std::vector<Row> v) {
            std::sort(v.begin(), v.end(), TreeRefresh::rowBefore);
            return v;
        };
        auto same = [](const Row &a, const Row &b) { return a.folder == b.folder && a.name == b.name; };
        // Applies the edits in the order given, as the tree does, checking
        // that no row that stays is ever taken out, and returns the list.
        auto apply = [&](std::vector<Row> list, const std::vector<Row> &fresh,
                         const std::vector<Splice> &edits, bool *keptRowsStayed) {
            *keptRowsStayed = true;
            for (const Splice &s : edits) {
                if (s.at + s.removed > list.size() || s.from + s.added > fresh.size()) {
                    *keptRowsStayed = false;
                    return list;
                }
                for (std::size_t k = s.at; k < s.at + s.removed; k++)
                    for (const Row &f : fresh)
                        if (same(f, list[k])) *keptRowsStayed = false;
                list.erase(list.begin() + (long)s.at, list.begin() + (long)(s.at + s.removed));
                list.insert(list.begin() + (long)s.at, fresh.begin() + (long)s.from,
                            fresh.begin() + (long)(s.from + s.added));
            }
            return list;
        };
        auto equal = [&](const std::vector<Row> &a, const std::vector<Row> &b) {
            if (a.size() != b.size()) return false;
            for (std::size_t k = 0; k < a.size(); k++)
                if (!same(a[k], b[k])) return false;
            return true;
        };

        // Folders first, then the key, then the name: README and readme
        // share a key and still have an order.
        CHECK(TreeRefresh::rowBefore(row("zeta", true), row("alpha")));
        CHECK(TreeRefresh::rowBefore(row("Alpha"), row("beta")));
        CHECK(TreeRefresh::rowBefore(row("README"), row("readme")));
        CHECK(!TreeRefresh::rowBefore(row("readme"), row("README")));
        CHECK(!TreeRefresh::rowBefore(row("a"), row("a")));

        const std::vector<Row> c3 = sorted({row("src", true), row("a.c"), row("b.c"), row("c.c")});
        // Nothing changed: nothing to do.
        CHECK(splices(c3, c3).empty());
        // The first listing is one edit, and so is an emptied folder.
        std::vector<Splice> e = splices({}, c3);
        CHECK(e.size() == 1 && e[0].at == 0 && e[0].removed == 0 && e[0].from == 0 && e[0].added == 4);
        e = splices(c3, {});
        CHECK(e.size() == 1 && e[0].at == 0 && e[0].removed == 4 && e[0].added == 0);
        // An object file beside each source: an edit for each, last first,
        // so each place is still right when its turn comes.
        const std::vector<Row> built = sorted({row("src", true), row("a.c"), row("a.o"), row("b.c"),
                                               row("b.o"), row("c.c"), row("c.o")});
        e = splices(c3, built);
        CHECK(e.size() == 3);
        CHECK(e.size() == 3 && e[0].at == 4 && e[1].at == 3 && e[2].at == 2);
        CHECK(e.size() == 3 && e[0].from == 6 && e[2].from == 2 && e[0].added == 1 && e[0].removed == 0);
        bool stayed = false;
        CHECK(equal(apply(c3, built, e, &stayed), built) && stayed);
        // And back: three removals.
        e = splices(built, c3);
        CHECK(e.size() == 3 && e[0].removed == 1 && e[0].added == 0);
        CHECK(equal(apply(built, c3, e, &stayed), c3) && stayed);
        // A run of changes between rows that stay is one edit: b.c renamed
        // to b2.c is a removal and an addition in one place.
        const std::vector<Row> renamed = sorted({row("src", true), row("a.c"), row("b2.c"), row("c.c")});
        e = splices(c3, renamed);
        CHECK(e.size() == 1 && e[0].at == 2 && e[0].removed == 1 && e[0].added == 1 && e[0].from == 2);
        // A file replaced by a folder of the same name goes and comes back
        // (in another place: folders come first).
        const std::vector<Row> retyped = sorted({row("a.c", true), row("src", true), row("b.c"), row("c.c")});
        e = splices(c3, retyped);
        CHECK(equal(apply(c3, retyped, e, &stayed), retyped) && stayed);
        CHECK(e.size() == 2);

        // Random folders against random changes: the result is the fresh
        // listing, no row that stays is touched, and two edits never touch
        // (between any two lies a row that stays).
        std::mt19937 rng(11);
        bool allRight = true, allStayed = true, apart = true;
        for (int round = 0; round < 300; round++) {
            std::vector<Row> a, b;
            const int n = (int)(rng() % 40);
            for (int k = 0; k < n; k++) {
                const std::string name = std::string(1, (char)('a' + rng() % 6)) + std::to_string(rng() % 8);
                const bool folder = rng() % 4 == 0;
                if (rng() % 3) a.push_back(row(name, folder));
                if (rng() % 3) b.push_back(row(name, rng() % 7 == 0 ? !folder : folder));
            }
            auto dedupe = [&](std::vector<Row> v) {
                v = sorted(v);
                v.erase(std::unique(v.begin(), v.end(), same), v.end());
                // One entry a name, as in a real folder.
                std::vector<Row> out;
                for (const Row &r : v) {
                    bool dup = false;
                    for (const Row &o : out) dup = dup || o.name == r.name;
                    if (!dup) out.push_back(r);
                }
                return sorted(out);
            };
            a = dedupe(a);
            b = dedupe(b);
            const std::vector<Splice> ed = splices(a, b);
            bool st = false;
            if (!equal(apply(a, b, ed, &st), b)) allRight = false;
            if (!st) allStayed = false;
            for (std::size_t k = 1; k < ed.size(); k++)
                if (ed[k].at + ed[k].removed >= ed[k - 1].at) apart = false;
        }
        CHECK(allRight);
        CHECK(allStayed);
        CHECK(apart);

        // A folder of 30,000 entries with every other one new is diffed at once.
        std::vector<Row> big, bigger;
        for (int k = 0; k < 30000; k++) {
            char name[32];
            snprintf(name, sizeof name, "file%05d.c", k);
            big.push_back(row(name));
            bigger.push_back(row(name));
            snprintf(name, sizeof name, "file%05d.o", k);
            bigger.push_back(row(name));
        }
        const double t0 = cpuMs();
        e = splices(big, bigger);
        const double ms = cpuMs() - t0;
        CHECK(e.size() == 30000 && e.front().at == 30000 && e.back().at == 1);
        CHECK_TIME(ms, 300);
    }

    GROUP("tree:pacer");
    {
        TreeRefresh::Pacer p(100, 300);
        CHECK(!p.pending());
        // The first listing of a folder starts at once, when it is opened.
        p.started(0);
        CHECK(p.pending());
        CHECK(p.finished(40) < 0);
        CHECK(!p.pending());
        // A change soon after: the listing waits out the interval.
        CHECK(p.changed(50) == 250);
        // More changes while it waits ask for nothing more.
        CHECK(p.changed(60) < 0 && p.changed(200) < 0);
        p.started(300);
        // Changes while it lists: one more listing once it is on show, no
        // sooner than the interval after this one began.
        CHECK(p.changed(320) < 0);
        CHECK(p.finished(350) == 250);
        CHECK(p.pending());
        p.started(600);
        CHECK(p.finished(620) < 0);
        // A change after a quiet spell is listed after the short wait.
        CHECK(p.changed(5000) == 100);
        p.started(5100);
        // A listing that took longer than the interval: the next one still
        // gathers for the short wait.
        CHECK(p.changed(5200) < 0);
        CHECK(p.finished(5900) == 100);
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
        CHECK_TIME(ms, 5000);
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
