// ignore_tests.inc — the `ignore:*` groups of run_tests.cpp: .gitignore and
// .ignore files in Find in Folder, the TODO list and Quick Open
// (FolderSearch::Ignore, FolderSearchOptions::useIgnoreFiles), kept in their
// own file and included there.

static void testIgnoreFiles() {
    using namespace FolderSearch;
    using FolderSearch::Ignore::Rules;
    using FolderSearch::Ignore::wildmatch;

    GROUP("ignore:parse");
    {
        auto rules = Ignore::parse("# a comment\n\n*.o\n!keep.o\nbuild/\n/top.txt\ndoc/*.md\n"
                                   "trail   \nspace\\ \n\\#hash\r\n");
        CHECK(rules.size() == 8);
        if (rules.size() == 8) {
            CHECK(rules[0].pattern == "*.o" && !rules[0].negate && !rules[0].anchored);
            CHECK(rules[1].pattern == "keep.o" && rules[1].negate);
            CHECK(rules[2].pattern == "build" && rules[2].dirOnly && !rules[2].anchored);
            CHECK(rules[3].pattern == "top.txt" && rules[3].anchored);
            CHECK(rules[4].pattern == "doc/*.md" && rules[4].anchored);
            CHECK(rules[5].pattern == "trail");          // trailing spaces go
            CHECK(rules[6].pattern == "space\\ ");       // unless escaped
            CHECK(rules[7].pattern == "\\#hash");        // CRLF, and an escaped #
        }
        CHECK(Ignore::parse("!\n/\n   \n").empty());
    }

    GROUP("ignore:wildmatch");
    CHECK(wildmatch("*.o", "a.o"));
    CHECK(!wildmatch("*.o", "a/b.o"));          // "*" stops at "/"
    CHECK(!wildmatch("*.o", "a.obj"));
    CHECK(wildmatch("a?c", "abc") && !wildmatch("a?c", "a/c"));
    CHECK(wildmatch("**/foo", "foo") && wildmatch("**/foo", "a/b/foo"));
    CHECK(wildmatch("a/**", "a/x/y") && !wildmatch("a/**", "a"));
    CHECK(wildmatch("a/**/b", "a/b") && wildmatch("a/**/b", "a/x/y/b"));
    CHECK(!wildmatch("a/**/b", "a/x/c"));
    CHECK(wildmatch("foo/*", "foo/bar") && !wildmatch("foo/*", "foo/bar/baz"));
    CHECK(wildmatch("[abc].txt", "b.txt") && !wildmatch("[abc].txt", "d.txt"));
    CHECK(wildmatch("[!a]x", "bx") && !wildmatch("[!a]x", "ax"));
    CHECK(wildmatch("[^a]x", "bx"));
    CHECK(wildmatch("[a-c]1", "b1") && !wildmatch("[a-c]1", "d1"));
    CHECK(wildmatch("[[:digit:]]x", "7x") && !wildmatch("[[:digit:]]x", "ax"));
    CHECK(wildmatch("\\*", "*") && !wildmatch("\\*", "a"));
    CHECK(wildmatch("a**b", "axxb") && !wildmatch("a**b", "a/b"));   // "**" inside a name is "*"
    CHECK(!wildmatch("*.TXT", "a.txt") && wildmatch("*.TXT", "a.txt", true));
    CHECK(wildmatch("[A-Z]x", "bx", true) && !wildmatch("[A-Z]x", "bx"));

    GROUP("ignore:rules");
    {
        Rules r = Rules().with("", "build/\n*.log\n!keep.log\n/root-only.txt\ndocs/*.tmp\n", "")
                         .with("a", "!b.log\nlocal/\n", "");
        CHECK(r.ignored("x.log", false));
        CHECK(!r.ignored("keep.log", false));
        CHECK(!r.ignored("a/b.log", false));         // the deeper file wins
        CHECK(r.ignored("a/c.log", false));
        CHECK(r.ignored("root-only.txt", false));
        CHECK(!r.ignored("a/root-only.txt", false));   // anchored to its folder
        CHECK(r.ignored("docs/x.tmp", false));
        CHECK(!r.ignored("docs/sub/x.tmp", false));
        CHECK(r.ignored("build", true) && !r.ignored("build", false));   // folders only
        CHECK(r.ignored("x/y/build", true));          // a name at any depth
        CHECK(r.ignored("a/local", true));
        CHECK(!r.ignored("local", true));             // a's rules stay in a
        CHECK(!r.ignored("plain.txt", false));
        CHECK(Rules().empty() && !Rules().ignored("x.log", false));
        // .ignore over .gitignore in one folder.
        Rules both = Rules().with("", "*.txt\n", "!keep.txt\n");
        CHECK(both.ignored("a.txt", false) && !both.ignored("keep.txt", false));
        // Case folds where asked (the Mac's default).
        Rules folded;
        folded.setCaseFold(true);
        folded = folded.with("", "*.LOG\n", "");
        CHECK(folded.ignored("x.log", false));
        CHECK(!Rules().with("", "*.LOG\n", "").ignored("x.log", false));
#ifdef __APPLE__
        CHECK(Ignore::defaultCaseFold());
#endif
    }

    GROUP("ignore:walk");
    {
        std::random_device rd;
        const fsys::path root = fsys::temp_directory_path() /
            ("minicode-ignore-test-" + std::to_string(rd()));
        fsys::remove_all(root);
        writeFile(root / ".gitignore", "*.log\nout/\n.env\n");
        writeFile(root / ".ignore", "a2.txt\n");
        writeFile(root / ".git" / "info" / "exclude", "secret.txt\n");
        writeFile(root / "a.txt", "hello\n");
        writeFile(root / "a2.txt", "hello\n");
        writeFile(root / "x.log", "hello\n");
        writeFile(root / "secret.txt", "hello\n");
        writeFile(root / ".env", "hello\n");
        writeFile(root / ".DS_Store", "hello\n");
        writeFile(root / "out" / "y.txt", "hello\n");
        writeFile(root / ".github" / "ci.yml", "hello\n");
        writeFile(root / "sub" / ".gitignore", "!keep.log\n");
        writeFile(root / "sub" / "keep.log", "hello\n");
        writeFile(root / "sub" / "z.log", "hello\n");
        // A tool's cache folder that ignores itself, as mypy's does.
        writeFile(root / "cache" / ".gitignore", "*\n");
        writeFile(root / "cache" / "c.txt", "hello\n");

        auto paths = [](const FolderSearchResult &r) {
            std::vector<std::string> v;
            for (const auto &m : r.matches) v.push_back(m.relativePath);
            std::sort(v.begin(), v.end());
            return v;
        };
        FolderSearchResult r = search(root.string(), "hello");
        CHECK(paths(r) == (std::vector<std::string>{".github/ci.yml", "a.txt", "sub/keep.log"}));
        FolderSearchOptions all;
        all.useIgnoreFiles = false;
        r = search(root.string(), "hello", nullptr, all);
        CHECK(paths(r) == (std::vector<std::string>{".env", ".github/ci.yml", "a.txt", "a2.txt",
                                                    "cache/c.txt", "out/y.txt", "secret.txt",
                                                    "sub/keep.log", "sub/z.log", "x.log"}));
        // A scope inside the window's folder keeps the folder's rules.
        FolderSearchOptions scoped;
        scoped.ignoreTop = root.string();
        r = search((root / "sub").string(), "hello", nullptr, scoped);
        CHECK(paths(r) == (std::vector<std::string>{"keep.log"}));
        r = search((root / "sub").string(), "hello");   // its own rules only
        CHECK(paths(r) == (std::vector<std::string>{"keep.log", "z.log"}));
        // The TODO list and Replace in Folder walk the same way.
        writeFile(root / "x.log", "// TODO: ignored\n");
        writeFile(root / "a.txt", "// TODO: kept\nhello\n");
        r = findTodos(root.string());
        CHECK(paths(r) == (std::vector<std::string>{"a.txt"}));
        FolderReplacePlan plan = planReplace(root.string(), "hello");
        CHECK(plan.files.size() == 3);
        // Quick Open lists dotfiles, minus what is ignored.
        std::vector<std::string> files = CommandPalette::listFiles(root.string());
        std::sort(files.begin(), files.end());
        CHECK(files == (std::vector<std::string>{".github/ci.yml", ".gitignore", ".ignore", "a.txt",
                                                 "sub/.gitignore", "sub/keep.log"}));
        files = CommandPalette::listFiles(root.string(), 100, nullptr, nullptr, false);
        CHECK(std::find(files.begin(), files.end(), ".env") != files.end());
        CHECK(std::find(files.begin(), files.end(), ".DS_Store") == files.end());
        CHECK(std::find(files.begin(), files.end(), ".git/info/exclude") == files.end());
        fsys::remove_all(root);
    }
}
