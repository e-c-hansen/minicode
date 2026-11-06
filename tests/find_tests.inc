// find_tests.inc — the `find:*` groups of run_tests.cpp (TextFind.h, and
// FolderSearch's options, Replace in Folder and atomic writes), kept in
// their own file and included there.

namespace {

TextFind::Query q16(const std::u16string &p, bool cs = false, bool word = false, bool re = false) {
    TextFind::Options o;
    o.caseSensitive = cs;
    o.wholeWord = word;
    o.regex = re;
    return TextFind::Query::make(p, o);
}

// Matches as "start+length".
std::vector<std::string> spans(const std::vector<TextFind::Match> &ms) {
    std::vector<std::string> v;
    for (const auto &m : ms) v.push_back(std::to_string(m.start) + "+" + std::to_string(m.length));
    return v;
}

std::u16string replaced(const std::u16string &text, const TextFind::Query &q,
                        const std::u16string &with) {
    return TextFind::applyEdits(text, TextFind::replaceAll(text, q, with));
}

}  // namespace

static void testTextFind() {
    using namespace TextFind;
    using V = std::vector<std::string>;

    GROUP("find:plain");
    {
        const std::u16string t = u"Foo foo FOO food\nfoo";
        CHECK(spans(findAll(t, q16(u"foo"))) == (V{"0+3", "4+3", "8+3", "12+3", "17+3"}));
        CHECK(spans(findAll(t, q16(u"foo", true))) == (V{"4+3", "12+3", "17+3"}));
        CHECK(spans(findAll(t, q16(u"foo", false, true))) == (V{"0+3", "4+3", "8+3", "17+3"}));
        CHECK(findAll(t, q16(u"")).empty());
        CHECK(!q16(u"").usable());
        // Non-overlapping, left to right.
        CHECK(spans(findAll(u"aaaa", q16(u"aa"))) == (V{"0+2", "2+2"}));
        // Folding past ASCII, and UTF-16 offsets around a surrogate pair.
        CHECK(spans(findAll(u"CAFÉ café", q16(u"café"))) == (V{"0+4", "5+4"}));
        CHECK(spans(findAll(u"Жж", q16(u"ж"))) == (V{"0+1", "1+1"}));
        CHECK(spans(findAll(u"\U0001F600x \U0001F600x", q16(u"x"))) == (V{"2+1", "6+1"}));
        CHECK(spans(findAll(u"a\U0001F600b", q16(u"\U0001F600b"))) == (V{"1+3"}));
        // Never across a line break.
        CHECK(findAll(u"ab\ncd", q16(u"b\nc")).empty());
        CHECK(spans(findAll(u"x\r\nx\rx x", q16(u"x"))) == (V{"0+1", "3+1", "5+1", "7+1"}));
        // The limit.
        bool truncated = false;
        CHECK(findAll(u"a a a a", q16(u"a"), 3, &truncated).size() == 3 && truncated);
        findAll(u"a a", q16(u"a"), 3, &truncated);
        CHECK(!truncated);
        // A raised cancel flag stops a long search.
        std::u16string many;
        for (int i = 0; i < 2000; ++i) many += u"ab\n";
        std::atomic<bool> stop(true);
        bool cancelled = false;
        CHECK(findAll(many, q16(u"ab"), 0, nullptr, &stop, &cancelled).size() < 2000 && cancelled);
    }

    GROUP("find:whole-word");
    {
        // VS Code's rule: a boundary is checked only where the match's own
        // edge is a word character.
        CHECK(spans(findAll(u"a.b a .b", q16(u".b", false, true))) == (V{"1+2", "6+2"}));
        CHECK(spans(findAll(u"foo_bar foo", q16(u"foo", false, true))) == (V{"8+3"}));
        CHECK(spans(findAll(u"café caf", q16(u"caf", false, true))) == (V{"5+3"}));
        CHECK(spans(findAll(u"x=1;x", q16(u"x", false, true))) == (V{"0+1", "4+1"}));
        CHECK(isWordChar(U'_') && isWordChar(U'é') && !isWordChar(U'-') &&
              !isWordChar(U' '));
        // With a regex too.
        CHECK(spans(findAll(u"cat concat cats", q16(u"cats?", false, true, true))) ==
              (V{"0+3", "11+4"}));
    }

    GROUP("find:regex");
    {
        CHECK(spans(findAll(u"a1 b22 c333", q16(u"\\d+", false, false, true))) ==
              (V{"1+1", "4+2", "8+3"}));
        CHECK(spans(findAll(u"Ab ab", q16(u"ab", false, false, true))) == (V{"0+2", "3+2"}));
        CHECK(spans(findAll(u"Ab ab", q16(u"ab", true, false, true))) == (V{"3+2"}));
        // Case folding past ASCII, which the C locale cannot do.
        CHECK(spans(findAll(u"CAFÉ", q16(u"café", false, false, true))) == (V{"0+4"}));
        CHECK(spans(findAll(u"Ж", q16(u"[а-я]", false, false, true))) == (V{"0+1"}));
        // . is a character, a surrogate pair included.
        CHECK(spans(findAll(u"\U0001F600!", q16(u"^.!$", false, false, true))) == (V{"0+3"}));
        // ^ and $ are each line's start and end.
        CHECK(spans(findAll(u"ab\nab\n", q16(u"^a", false, false, true))) == (V{"0+1", "3+1"}));
        CHECK(spans(findAll(u"ab\nab", q16(u"b$", false, false, true))) == (V{"1+1", "4+1"}));
        // Empty matches: every line start, the empty last line included.
        CHECK(spans(findAll(u"x\ny\n", q16(u"^", false, false, true))) == (V{"0+0", "2+0", "4+0"}));
        // a* as JavaScript finds it: "a", then empty at 1 and at 2.
        CHECK(spans(findAll(u"ab", q16(u"a*", false, false, true))) == (V{"0+1", "1+0", "2+0"}));
        // \b is ASCII, as in JavaScript.
        CHECK(spans(findAll(u"a-b", q16(u"\\b\\w", false, false, true))) == (V{"0+1", "2+1"}));
        // A bad pattern says why and matches nothing.
        Query bad = q16(u"(ab", false, false, true);
        CHECK(!bad.usable() && bad.error().find("Invalid regular expression") == 0);
        CHECK(findAll(u"(ab", bad).empty());
        // The same pattern as plain text is literal.
        CHECK(spans(findAll(u"x(ab", q16(u"(ab"))) == (V{"1+3"}));
        CHECK(spans(findAll(u"a.c abc", q16(u"a.c"))) == (V{"0+3"}));
    }

    GROUP("find:regex-long-lines");
    {
        // A line over 2,000 characters is searched a start position at a
        // time; the matches are the same as in one search.
        const std::u16string dashes(2500, u'-');
        CHECK(spans(findAll(dashes + u"a1 b22 c333", q16(u"\\d+", false, false, true))) ==
              (V{"2501+1", "2504+2", "2508+3"}));
        CHECK(spans(findAll(dashes, q16(u"^-", false, false, true))) == (V{"0+1"}));
        CHECK(spans(findAll(dashes, q16(u"-$", false, false, true))) == (V{"2499+1"}));
        CHECK(spans(findAll(dashes + u"a-b", q16(u"\\b\\w", false, false, true))) ==
              (V{"2500+1", "2502+1"}));
        // a* finds the empty match at every dash, then "a", then empty at the end.
        const auto stars = findAll(dashes + u"a", q16(u"a*", false, false, true));
        CHECK(stars.size() == 2502 && stars[2500].start == 2500 && stars[2500].length == 1 &&
              stars[2501].start == 2501 && stars[2501].length == 0);
        CHECK(spans(findAll(dashes + u"Ab", q16(u"ab", false, false, true))) == (V{"2500+2"}));
        // Many matches on one long line stay linear (each search used to
        // copy the rest of the line).
        std::u16string commas;
        for (int i = 0; i < 100000; i++) commas += u"a,";
        auto t0 = std::chrono::steady_clock::now();
        CHECK(findAll(commas, q16(u",", false, false, true)).size() == 100000);
        const double manyMs = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count();
        CHECK(manyMs < 1000);
        // A regex that backtracks over a long line gives up on it after
        // about a second (it ran for more than eight before), and the next
        // line is still searched.
        const std::u16string as(50000, u'a');
        t0 = std::chrono::steady_clock::now();
        const auto slow = findAll(as + u"\naz", q16(u"a(.*)z", false, false, true));
        const double slowS = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - t0).count();
        CHECK(spans(slow) == (V{"50001+2"}));
        CHECK(slowS < 4);
        // A cancel stops it within the line.
        std::atomic<bool> cancel(false);
        bool cancelled = false;
        std::thread raiser([&cancel] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            cancel = true;
        });
        t0 = std::chrono::steady_clock::now();
        findAll(as + u"\n" + as + u"\n" + as, q16(u"a(.*)z", false, false, true), 0, nullptr,
                &cancel, &cancelled);
        const double cancelS = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - t0).count();
        raiser.join();
        CHECK(cancelled && cancelS < 2);
        std::printf("  regex on long lines: 100,000 matches on one line %.0f ms, a backtracking "
                    "line given up after %.2f s, cancelled after %.2f s\n",
                    manyMs, slowS, cancelS);
    }

    GROUP("find:replace");
    {
        CHECK(replaced(u"foo Foo food", q16(u"foo", false, true), u"bar") == u"bar bar food");
        CHECK(replaced(u"foo Foo", q16(u"foo", true), u"$1") == u"$1 Foo");   // literal
        // Groups, $& and $0, $$, and an unmatched group.
        const Query kv = q16(u"(\\w+)=(\\w+)(;)?", false, false, true);
        CHECK(replaced(u"a=1 b=2;", kv, u"$2=$1") == u"1=a 2=b");
        CHECK(replaced(u"a=1", kv, u"[$&|$0]") == u"[a=1|a=1]");
        CHECK(replaced(u"a=1", kv, u"$$1 $3.") == u"$1 .");
        CHECK(replaced(u"a=1", kv, u"$9 $") == u"$9 $");      // no group 9: as typed
        // $12 is group 12 only when there are twelve groups; else $1 then 2.
        CHECK(replaced(u"a=1", kv, u"$12") == u"a2");
        // \n, \t and \\, only for a regex.
        CHECK(replaced(u"a,b", q16(u",", false, false, true), u"\\n\\t\\\\") == u"a\n\t\\b");
        CHECK(replaced(u"a,b", q16(u","), u"\\n") == u"a\\nb");
        // Every line's start (VS Code's way to prefix lines).
        CHECK(replaced(u"x\ny", q16(u"^", false, false, true), u"// ") == u"// x\n// y");
        CHECK(replaced(u"ab", q16(u"a*", false, false, true), u"-") == u"--b-");
        // Offsets in UTF-16 units around astral characters.
        CHECK(replaced(u"\U0001F600 cat \U0001F600cat", q16(u"cat"), u"dog") ==
              u"\U0001F600 dog \U0001F600dog");
        // Edits come in ascending order and replace exactly the matches.
        auto e = replaceAll(u"a b a", q16(u"a"), u"xy");
        CHECK(e.size() == 2 && e[0].start == 0 && e[1].start == 4 && e[1].text == u"xy");
        CHECK(replaceAll(u"abc", q16(u"z"), u"y").empty());
    }

    GROUP("find:preserve-case");
    {
        // VS Code's buildReplaceStringWithCasePreserved cases (search.test.ts).
        CHECK(preserveCase(u"abc", u"Def") == u"def");
        CHECK(preserveCase(u"Abc", u"Def") == u"Def");
        CHECK(preserveCase(u"ABC", u"Def") == u"DEF");
        CHECK(preserveCase(u"abc", u"DEF") == u"def");
        CHECK(preserveCase(u"aBC", u"Def") == u"def");   // a small letter first
        CHECK(preserveCase(u"Abc", u"dEF") == u"DEF");   // a capital first
        CHECK(preserveCase(u"Foo-Bar", u"newfoo-newbar") == u"Newfoo-Newbar");
        CHECK(preserveCase(u"Foo-Bar-Abc", u"newfoo-newbar-newabc") == u"Newfoo-Newbar-Newabc");
        CHECK(preserveCase(u"Foo-Bar-abc", u"newfoo-newbar") == u"Newfoo-newbar");
        CHECK(preserveCase(u"foo-Bar", u"newfoo-newbar") == u"newfoo-Newbar");
        CHECK(preserveCase(u"foo_Bar", u"newfoo_newbar") == u"newfoo_Newbar");
        CHECK(preserveCase(u"Foo_Bar", u"newfoo_newbar") == u"Newfoo_Newbar");
        CHECK(preserveCase(u"Foo_Bar-Abc", u"newfoo_newbar-abc") == u"Newfoo_newbar-abc");
        CHECK(preserveCase(u"", u"Def") == u"Def");
        CHECK(preserveCase(u"ÉTÉ", u"hiver") == u"HIVER");
        CHECK(preserveCase(std::string("Été"), std::string("hiver")) == "Hiver");
        CHECK(preserveCase(std::string("ПРИВЕТ"), std::string("мир")) == "МИР");
        // Through Replace: case-insensitive matches each keep their case.
        TextFind::Options o;
        o.preserveCase = true;
        const Query pc = Query::make(std::u16string(u"foo"), o);
        CHECK(replaced(u"foo Foo FOO", pc, u"bar") == u"bar Bar BAR");
        // A regex replacement with a group takes its case from the group.
        o.regex = true;
        CHECK(replaced(u"Foo", Query::make(std::u16string(u"(f)oo"), o), u"$1ix") == u"Fix");
        CHECK(replaced(u"Foo", Query::make(std::u16string(u"foo"), o), u"bar") == u"Bar");
        std::size_t n = 0;
        TextFind::Options o8;
        o8.preserveCase = true;
        CHECK(replaceInText("cat Cat CAT\n", Query::make(std::string("cat"), o8), "dog", &n) ==
                  "dog Dog DOG\n" && n == 3);
    }

    GROUP("find:next");
    {
        const auto ms = findAll(u"ab ab ab", q16(u"ab"));   // 0, 3, 6
        CHECK(nextIndex(ms, 0, 0, true) == 0);    // a caret on a match goes to it
        CHECK(nextIndex(ms, 1, 1, true) == 1);
        CHECK(nextIndex(ms, 0, 2, true) == 1);    // the selection is a match: the next
        CHECK(nextIndex(ms, 6, 8, true) == 0);    // wraps
        CHECK(nextIndex(ms, 7, 7, true) == 0);
        CHECK(nextIndex(ms, 3, 5, false) == 0);
        CHECK(nextIndex(ms, 0, 2, false) == 2);   // wraps back
        CHECK(nextIndex(ms, 4, 4, false) == 1);
        CHECK(nextIndex({}, 0, 0, true) == -1);
        CHECK(indexOf(ms, 3, 2) == 1 && indexOf(ms, 3, 1) == -1);
        // Empty matches do not stick: from one, the next is the next.
        const auto starts = findAll(u"x\ny", q16(u"^", false, false, true));
        CHECK(nextIndex(starts, 0, 0, true) == 1);
    }

    GROUP("find:utf8");
    {
        TextFind::Options o;
        o.regex = true;
        const Query q = Query::make(std::string("(\\w+)\xC3\xA9"), o);   // "...é"
        std::size_t col = 0, len = 0;
        CHECK(findInLine("x caf\xC3\xA9!", q, &col, &len) && col == 2 && len == 5);
        CHECK(findAllInLine("ab\xC3\xA9 cd\xC3\xA9", q).size() == 2);
        std::size_t n = 0;
        CHECK(replaceInText("caf\xC3\xA9\r\nth\xC3\xA9\n", q, "<$1>", &n) == "<caf>\r\n<th>\n" &&
              n == 2);
        CHECK(replaceInText("a\rb\xE2\x80\xA9" "a", Query::make(std::string("a"), {}), "z", &n) ==
                  "z\rb\xE2\x80\xA9z" && n == 2);
        // One match, by line and byte column.
        std::string out;
        const Query a = Query::make(std::string("ab"), {});
        CHECK(replaceOneInText("ab ab\nab ab", a, "X", 2, 3, &out) && out == "ab ab\nab X");
        CHECK(!replaceOneInText("ab ab\nab ab", a, "X", 2, 1, &out));   // no match starts there
        CHECK(!replaceOneInText("ab", a, "X", 5, 0, &out));
        CHECK(replaceOneInText("x\n", Query::make(std::string("^"), o), "y", 2, 0, &out) &&
              out == "x\ny");
        // The same row applied to a tab's UTF-16 text: byte 6 of line 2 is
        // after "é" (two bytes, one unit) and "😀" (four bytes, two units).
        Edit16 e;
        const std::u16string t = u"ab\n\u00e9\U0001F600ab ab";
        CHECK(replaceOneAt(t, a, u"X", 2, 6, &e) && e.start == 6 && e.length == 2 && e.text == u"X");
        CHECK(applyEdits(t, {e}) == u"ab\n\u00e9\U0001F600X ab");
        CHECK(replaceOneAt(t, a, u"X", 2, 9, &e) && e.start == 9);
        CHECK(!replaceOneAt(t, a, u"X", 2, 5, &e));    // inside a character
        CHECK(!replaceOneAt(t, a, u"X", 3, 0, &e));
        CHECK(replaceOneAt(t, a, u"X", 1, 0, &e) && e.start == 0);
    }
}

static void testFolderReplace() {
    using namespace FolderSearch;
    const fsys::path root = fsys::temp_directory_path() /
                            ("minicode-replace-test-" + std::to_string(::getpid()));
    std::error_code ec;
    fsys::remove_all(root, ec);
    writeFile(root / "a.txt", "foo bar\nFoo foo food\n");
    writeFile(root / "src" / "b.py", "x = foo(1)\n");
    writeFile(root / "src" / "c.md", "nothing here\n");
    writeFile(root / "node_modules" / "d.js", "foo\n");   // skipped, as by search
    writeFile(root / ".hidden", "foo\n");

    GROUP("find:folder-options");
    {
        FolderSearchOptions o;
        auto r = search(root.string(), "foo", nullptr, o);
        CHECK(r.matches.size() == 3);   // a row per line
        // But the count is every match, the number Replace All gives (the
        // status said "3 matches" while Replace All replaced 5).
        CHECK(r.totalMatches == 5 && r.filesMatched == 2);
        CHECK(r.matches.size() == 3 && r.matches[0].count + r.matches[1].count +
                                               r.matches[2].count == 5);
        CHECK(r.totalMatches == planReplace(root.string(), "foo", nullptr, o).matches);
        o.caseSensitive = true;
        CHECK(search(root.string(), "Foo", nullptr, o).matches.size() == 1);
        o.caseSensitive = false;
        o.wholeWord = true;
        r = search(root.string(), "food", nullptr, o);
        CHECK(r.matches.size() == 1 && r.matches[0].line == 2 && r.matches[0].column == 9);
        CHECK(search(root.string(), "foo", nullptr, o).totalMatches ==
              planReplace(root.string(), "foo", nullptr, o).matches);
        o.wholeWord = false;
        o.regex = true;
        r = search(root.string(), "fo+\\(", nullptr, o);
        CHECK(r.matches.size() == 1 && r.matches[0].relativePath == "src/b.py" &&
              r.matches[0].byteColumn == 4 && r.matches[0].byteLength == 4);
        r = search(root.string(), "fo(", nullptr, o);
        CHECK(r.matches.empty() && !r.error.empty());
        CHECK(isSearchable(".", o) && !isSearchable(".", FolderSearchOptions{}));
        // A regex is not trimmed.
        CHECK(search(root.string(), " bar", nullptr, o).matches.size() == 1);
        CHECK(search(root.string(), "o b", nullptr, o).matches.size() == 1);
    }

    GROUP("find:globs");
    {
        CHECK(globMatch("*.py", "b.py") && !globMatch("*.py", "src/b.py"));
        CHECK(globMatch("**/*.py", "src/b.py") && globMatch("**/*.py", "b.py"));
        CHECK(globMatch("src/**", "src/a/b.c") && !globMatch("src/**", "srcx/a"));
        CHECK(globMatch("a/**/b", "a/b") && globMatch("a/**/b", "a/x/y/b") && !globMatch("a/**/b", "a/xb"));
        CHECK(globMatch("?.txt", "a.txt") && !globMatch("?.txt", "ab.txt") && !globMatch("?", "/"));
        CHECK(globMatch("[ab].c", "b.c") && !globMatch("[ab].c", "c.c"));
        CHECK(globMatch("[!ab].c", "c.c") && !globMatch("[^ab].c", "a.c"));
        CHECK(globMatch("f[0-9]", "f7") && !globMatch("f[0-9]", "fx"));
        CHECK(globMatch("[", "[") && globMatch("a\\*", "a*") && !globMatch("a\\*", "ab"));
        CHECK(!globMatch("*.PY", "b.py"));   // case counts
        using V = std::vector<std::string>;
        CHECK(splitGlobList(" *.ts , src/{a,b}/** ,, [,]x ") == (V{"*.ts", "src/{a,b}/**", "[,]x"}));
        CHECK(splitGlobList("").empty());

        // VS Code's expansions (queryBuilder.ts).
        CHECK(PathFilter("foo", "/r").globs() == (V{"**/foo/**", "**/foo"}));
        CHECK(PathFilter(".js", "/r").globs() == (V{"**/*.js/**", "**/*.js"}));
        CHECK(PathFilter("src/", "/r").globs() == (V{"**/src/**", "**/src"}));
        CHECK(PathFilter("**/*.ts", "/r").globs() == (V{"**/*.ts/**", "**/*.ts"}));
        CHECK(PathFilter("./src", "/r").globs() == (V{"src", "src/**"}));
        CHECK(PathFilter("./src/**", "/r/").globs() == (V{"src/**"}));
        CHECK(PathFilter("/r/lib", "/r").globs() == (V{"lib", "lib/**"}));
        CHECK(PathFilter("./", "/r").globs() == (V{"**"}));
        CHECK(PathFilter("*.{c,h}", "/r").globs() == (V{"**/*.c/**", "**/*.h/**", "**/*.c", "**/*.h"}));
        PathFilter src("src", "/r");
        CHECK(src.matches("src") && src.matches("src/x.py") && src.matches("a/src/b/c"));
        CHECK(!src.matches("srcs/x.py") && !src.matches("x/src.py"));
        PathFilter rooted("./src", "/r");
        CHECK(rooted.matches("src/b.py") && !rooted.matches("a/src/b.py"));
        PathFilter outside("../elsewhere, /etc", "/r");
        CHECK(!outside.empty() && !outside.matches("elsewhere") && !outside.matches("etc"));
        PathFilter up("./src/../lib", "/r");
        CHECK(up.matches("lib/a") && !up.matches("src/a"));
        CHECK(PathFilter("", "/r").empty() && PathFilter(" , ", "/r").empty());

        // In the walk: files to include, files to exclude, and both.
        FolderSearchOptions o;
        o.include = "*.py";
        auto r = search(root.string(), "foo", nullptr, o);
        CHECK(r.matches.size() == 1 && r.matches[0].relativePath == "src/b.py");
        o.include = "./src";
        CHECK(search(root.string(), "foo", nullptr, o).matches.size() == 1);
        o.include = "";
        o.exclude = "src";
        r = search(root.string(), "foo", nullptr, o);
        CHECK(r.matches.size() == 2 && r.matches[0].relativePath == "a.txt");
        o.exclude = "*.txt, *.py";
        CHECK(search(root.string(), "foo", nullptr, o).matches.empty());
        o.include = "src";
        o.exclude = "*.md";
        CHECK(search(root.string(), "here", nullptr, o).matches.empty());
        o.exclude = "";
        CHECK(search(root.string(), "here", nullptr, o).matches.size() == 1);
        // Replace in Folder counts the same files.
        o.include = "a.txt";
        CHECK(planReplace(root.string(), "foo", nullptr, o).matches == 4);
        o.include = "";
        o.exclude = "a.txt";
        CHECK(planReplace(root.string(), "foo", nullptr, o).matches == 1);
        // And the TODO list.
        writeFile(root / "src" / "t.py", "# TODO: one\n");
        FolderSearchOptions t;
        CHECK(findTodos(root.string(), nullptr, t).matches.size() == 1);
        t.exclude = "*.py";
        CHECK(findTodos(root.string(), nullptr, t).matches.empty());
        fsys::remove(root / "src" / "t.py", ec);
        // Preserve case through Replace in Folder.
        FolderSearchOptions pc;
        pc.preserveCase = true;
        std::size_t n = 0;
        CHECK(replaceInText("Foo foo\n", "foo", "bar", pc, &n) == "Bar bar\n" && n == 2);
    }

    GROUP("find:folder-plan");
    {
        FolderSearchOptions o;
        FolderReplacePlan p = planReplace(root.string(), "foo", nullptr, o);
        CHECK(p.matches == 5 && p.files.size() == 2);   // every match, not a row per line
        CHECK(p.files.size() == 2 && p.files[0].relativePath == "a.txt" &&
              p.files[0].matches == 4 && p.files[1].matches == 1);
        o.wholeWord = true;
        CHECK(planReplace(root.string(), "foo", nullptr, o).matches == 4);
        o.regex = true;
        CHECK(!planReplace(root.string(), "(", nullptr, o).error.empty());
        std::atomic<bool> stop(true);
        CHECK(planReplace(root.string(), "foo", &stop, {}).cancelled);
    }

    GROUP("find:folder-replace");
    {
        FolderSearchOptions o;
        std::size_t n = 0;
        CHECK(replaceInText("foo Foo\r\nfoo", "foo", "bar", o, &n) == "bar bar\r\nbar" && n == 3);
        CHECK(replaceInText("foo", "f", "x", o, &n) == "foo" && n == 0);   // one character
        o.regex = true;
        CHECK(replaceInText("k=v", "(\\w)=(\\w)", "$2=$1", o, &n) == "v=k" && n == 1);
        std::string out;
        CHECK(replaceMatchInText("foo foo", "foo", "X", {}, 1, 4, &out) && out == "foo X");
    }

#ifndef _WIN32
    GROUP("find:atomic-write");
    {
        const fsys::path f = root / "w.txt";
        writeFile(f, "old");
        ::chmod(f.c_str(), 0640);
        std::string err;
        CHECK(writeFileAtomically(f.string(), "new text", &err));
        std::string back;
        CHECK(readTextFile(f.string(), 1 << 20, &back) && back == "new text");
        struct stat st;
        CHECK(::stat(f.c_str(), &st) == 0 && (st.st_mode & 07777) == 0640);
        // No temporary left beside it.
        int extra = 0;
        for (const auto &e : fsys::directory_iterator(root))
            if (e.path().filename().string().find(".minicode-") != std::string::npos) extra++;
        CHECK(extra == 0);
        // A symlink stays a link, and its target changes.
        const fsys::path link = root / "link.txt";
        fsys::create_symlink(f, link, ec);
        CHECK(!ec && writeFileAtomically(link.string(), "via link", &err));
        CHECK(fsys::is_symlink(link) && readTextFile(f.string(), 1 << 20, &back) &&
              back == "via link");
        // A hard link is written in place, so both names see it.
        const fsys::path hard = root / "hard.txt";
        fsys::create_hard_link(f, hard, ec);
        CHECK(!ec && writeFileAtomically(f.string(), "both", &err));
        CHECK(readTextFile(hard.string(), 1 << 20, &back) && back == "both");
        // A folder that cannot be written to: nothing written, a reason given.
        CHECK(!writeFileAtomically((root / "no" / "such.txt").string(), "x", &err) &&
              !err.empty());
        // readTextFile refuses what search skips.
        writeFile(root / "bin.dat", std::string("\xFF\xFE", 2));
        CHECK(!readTextFile((root / "bin.dat").string(), 1 << 20, &back));
        CHECK(!readTextFile(root.string(), 1 << 20, &back));
        CHECK(!readTextFile(f.string(), 2, &back));   // over the size limit
    }
#endif
    fsys::remove_all(root, ec);
}

// Saving and the disk (Replace in Folder over files with two names, the
// stable read, the BOM, what an atomic write keeps): `save:*`,
// `find:names` and `find:replace-match-line`.
static void testSavingToDisk() {
    using namespace FolderSearch;
#ifndef _WIN32
    const fsys::path root = fsys::temp_directory_path() /
                            ("minicode-save-test-" + std::to_string(::getpid()));
    const fsys::path outside = fsys::temp_directory_path() /
                               ("minicode-save-outside-" + std::to_string(::getpid()));
    std::error_code ec;
    fsys::remove_all(root, ec);
    fsys::remove_all(outside, ec);
    writeFile(root / "a.py", "log.info('x')\n");
    writeFile(outside / "far.py", "log = 1\n");
    fsys::create_symlink(root / "a.py", root / "link.py", ec);
    fsys::create_directories(root / "sub", ec);
    fsys::create_hard_link(root / "a.py", root / "sub" / "hard.py", ec);
    fsys::create_directory_symlink(outside, root / "ext", ec);

    GROUP("find:names");
    {
        // a.py, link.py and sub/hard.py are one file: counted once, under
        // the first name met; ext/far.py is outside the folder, and says so.
        FolderReplacePlan p = planReplace(root.string(), "log", nullptr, {});
        CHECK(p.files.size() == 2 && p.matches == 2);
        CHECK(p.files.size() == 2 && p.files[0].relativePath == "a.py" && !p.files[0].outside);
        CHECK(p.files.size() == 2 && p.files[1].relativePath == "ext/far.py" &&
              p.files[1].outside);
        // Search still lists every name.
        CHECK(search(root.string(), "log", nullptr, {}).matches.size() == 4);
    }

    GROUP("find:replace-match-line");
    {
        std::string line;
        CHECK(lineAt("a\r\nb\rc\xE2\x80\xA9" "d\n", 4, &line) && line == "d");
        CHECK(lineAt("a\r\nb\rc", 2, &line) && line == "b");
        CHECK(!lineAt("a\n", 2, &line) && !lineAt("", 1, &line));
        // The row's line as searched: "foo = foo" at line 2, column 6.
        const std::string text = displayText("foo = foo");
        std::string out;
        CHECK(replaceMatchInText("x\nfoo = foo\n", "foo", "X", {}, 2, 6, &out, &text) &&
              out == "x\nfoo = X\n");
        // A line added above: line 2 is now another line with a foo at
        // column 6, which is not the row's match.
        CHECK(!replaceMatchInText("y\nbar = foo\nfoo = foo\n", "foo", "X", {}, 2, 6, &out, &text));
        // Without the text, the old behaviour.
        CHECK(replaceMatchInText("y\nbar = foo\n", "foo", "X", {}, 2, 6, &out) &&
              out == "y\nbar = X\n");
    }

    GROUP("save:stamp");
    {
        const fsys::path f = root / "s.txt";
        writeFile(f, "one");
        FileStamp a = stampOf(f.string());
        CHECK(a.exists && a.regular && a.size == 3 && a.inode != 0);
        CHECK(stampOf(f.string()) == a);
        fsys::create_symlink(f, root / "s-link.txt", ec);
        CHECK(stampOf((root / "s-link.txt").string()) == a);   // the file, not the link
        CHECK(!stampOf((root / "none").string()).exists);
        CHECK(stampOf(root.string()).exists && !stampOf(root.string()).regular);
        // An atomic write's stamp is the file's as it stands afterwards.
        FileStamp w;
        std::string err;
        CHECK(writeFileAtomically(f.string(), "three", &err, &w));
        CHECK(w.exists && w.size == 5 && w == stampOf(f.string()) && w.inode != a.inode);
        // In place (a hard link) keeps the inode.
        fsys::create_hard_link(f, root / "s-hard.txt", ec);
        const FileStamp before = stampOf(f.string());
        CHECK(writeFileAtomically(f.string(), "fourfour", &err, &w));
        CHECK(w.inode == before.inode && w.size == 8 && w == stampOf(f.string()));
    }

    GROUP("save:read-stable");
    {
        const fsys::path f = root / "r.txt";
        writeFile(f, std::string("\xEF\xBB\xBF") + "a,b\n1,2\n");
        std::string bytes;
        FileStamp st;
        CHECK(readFileStable(f.string(), 1 << 20, &bytes, &st) == ReadStatus::Ok);
        CHECK(st == stampOf(f.string()) && bytes.size() == 11);
        CHECK(stripUtf8Bom(&bytes) && bytes == "a,b\n1,2\n");
        CHECK(!stripUtf8Bom(&bytes) && bytes == "a,b\n1,2\n");
        std::string two = "\xEF\xBB";
        CHECK(!stripUtf8Bom(&two) && two.size() == 2);
        CHECK(readFileStable((root / "none").string(), 1 << 20, &bytes, &st) ==
                  ReadStatus::Missing && !st.exists);
        CHECK(readFileStable(root.string(), 1 << 20, &bytes, &st) == ReadStatus::NotRegular);
        CHECK(readFileStable(f.string(), 4, &bytes, &st) == ReadStatus::TooLarge);
        // Larger than the first buffer, read whole.
        const std::string big(300000, 'x');
        writeFile(f, big);
        CHECK(readFileStable(f.string(), 1 << 20, &bytes, nullptr) == ReadStatus::Ok &&
              bytes == big);
        CHECK(readFileStable(f.string(), UINTMAX_MAX, &bytes, nullptr) == ReadStatus::Ok &&
              bytes == big);
        writeFile(f, "");
        CHECK(readFileStable(f.string(), 1 << 20, &bytes, &st) == ReadStatus::Ok &&
              bytes.empty() && st.size == 0);
    }

#ifdef __APPLE__
    GROUP("save:write-metadata");
    {
        // An extended attribute (a Finder tag, say) and the creation date
        // stay, though the file is a new one (another inode).
        const fsys::path f = root / "m.txt";
        writeFile(f, "old");
        const char tag[] = "minicode-test";
        CHECK(::setxattr(f.c_str(), "com.minicode.test", tag, sizeof tag, 0, 0) == 0);
        struct stat st0;
        ::stat(f.c_str(), &st0);
        // A modification date before the creation date moves the creation
        // date back too (as `touch -t` does): 2001, so a new file would show.
        struct timeval old[2] = {{1000000000, 0}, {1000000000, 0}};
        CHECK(::utimes(f.c_str(), old) == 0);
        std::string err;
        CHECK(writeFileAtomically(f.string(), "new", &err));
        char back[64] = {};
        CHECK(::getxattr(f.c_str(), "com.minicode.test", back, sizeof back, 0, 0) ==
                  (ssize_t)sizeof tag && std::string(back) == tag);
        struct stat st1;
        ::stat(f.c_str(), &st1);
        CHECK(st1.st_ino != st0.st_ino);
        CHECK(st1.st_birthtimespec.tv_sec == 1000000000);
        std::string text;
        CHECK(readTextFile(f.string(), 1 << 20, &text) && text == "new");
    }
#endif
    fsys::remove_all(root, ec);
    fsys::remove_all(outside, ec);
#endif
}
