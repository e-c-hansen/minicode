// folder_search_tests.inc — the `search:*` groups of run_tests.cpp: Find in
// Folder's parallel walk (FolderSearch::search, planReplace and findTodos
// on several threads give what one thread gives), the needle that finds
// candidate lines in a whole file, the files skipped and named, and a
// bound on the CPU a search of a few hundred files takes. Kept in its own
// file and included there.

#include <random>
#include <sys/resource.h>
#include <sys/stat.h>

namespace {

// Everything a search result says, as text, so two can be compared whole.
std::string describe(const FolderSearchResult& r) {
    std::string s;
    for (const FolderSearchMatch& m : r.matches)
        s += m.relativePath + ":" + std::to_string(m.line) + ":" + std::to_string(m.column) +
             ":" + std::to_string(m.byteColumn) + "+" + std::to_string(m.byteLength) + "x" +
             std::to_string(m.count) + " " + m.text + "\n";
    s += "searched " + std::to_string(r.filesSearched) + " matched " +
         std::to_string(r.filesMatched) + " total " + std::to_string(r.totalMatches) +
         (r.truncated ? " truncated" : "") + (r.cancelled ? " cancelled" : "") +
         (r.tooSlow ? " slow" : "") + " error " + r.error + "\nskipped " +
         std::to_string(r.skippedTooLarge) + "/" + std::to_string(r.skippedNotUtf8);
    for (const FolderSearchSkip& k : r.skipped) s += " " + k.relativePath + (k.tooLarge ? "+" : "-");
    return s;
}

std::string describe(const FolderReplacePlan& p) {
    std::string s;
    for (const FolderReplaceFile& f : p.files)
        s += f.relativePath + " " + std::to_string(f.matches) + (f.outside ? " outside" : "") + "\n";
    return s + "total " + std::to_string(p.matches) + (p.cancelled ? " cancelled" : "") + p.error;
}

// The process's CPU time (every thread), in seconds.
double processCpuSeconds() {
    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 + u.ru_stime.tv_sec +
           u.ru_stime.tv_usec / 1e6;
}

// A line of random words, some of them the ones searched for.
std::string randomLine(std::mt19937& rng) {
    static const char* words[] = {"return", "Return", "RETURN", "retrn", "re", "turn", "x",
                                  "café", "CAFÉ", "Ωmega", "ωmega", "naïve", "\t", "  ",
                                  "foo_bar", "foo", "bar", "ret", "returned", "(", ")", "{",
                                  "TODO:", "// TODO", "- [ ] task", "日本語", "\x1b[31m"};
    const std::size_t n = sizeof words / sizeof *words;
    std::string line;
    const int count = static_cast<int>(rng() % 12);
    for (int i = 0; i < count; ++i) {
        if (i) line += rng() % 3 ? " " : "";
        line += words[rng() % n];
    }
    return line;
}

std::string randomText(std::mt19937& rng, int lines) {
    static const char* breaks[] = {"\n", "\n", "\n", "\r\n", "\r", "\xE2\x80\xA9"};
    std::string s;
    for (int i = 0; i < lines; ++i) {
        s += randomLine(rng);
        if (i + 1 < lines || rng() % 2) s += breaks[rng() % 6];
    }
    return s;
}

}  // namespace

static void testFolderSearchParallel() {
    using namespace FolderSearch;
    const fsys::path root = fsys::temp_directory_path() /
                            ("minicode-search-test-" + std::to_string(::getpid()));
    std::error_code ec;
    fsys::remove_all(root, ec);

    // A tree as varied as the walk's rules: nested folders, files in and
    // out of .gitignore, skipped folders, a symlinked folder and a loop,
    // a file too large, one not UTF-8, binaries, and a pipe.
    std::mt19937 rng(20261107);
    for (int d = 0; d < 12; ++d) {
        const fsys::path dir = root / ("d" + std::to_string(d)) / (d % 3 ? "sub" : "") ;
        for (int f = 0; f < 9; ++f)
            writeFile(dir / ("f" + std::to_string(f) + (f % 4 ? ".txt" : ".md")),
                      randomText(rng, 1 + static_cast<int>(rng() % 60)));
    }
    writeFile(root / "top.txt", randomText(rng, 30));
    writeFile(root / ".gitignore", "*.log\nd5/\n");
    writeFile(root / "d1" / ".ignore", "f3.txt\n");
    writeFile(root / "d2" / "x.log", "return\n");
    writeFile(root / "node_modules" / "m.txt", "return\n");
    writeFile(root / "big.txt", std::string(1024 * 1024 + 10, 'a') + "\nreturn\n");
    writeFile(root / "big.bin", std::string(1, '\0') + std::string(1024 * 1024 + 10, 'b'));
    writeFile(root / "latin1.txt", "caf\xE9 return\n");
    writeFile(root / "small.bin", std::string("\0\xFF\xFE return", 10));
    writeFile(root / "nul.txt", std::string("return\0here\n", 12));   // valid UTF-8: searched
    fsys::create_directory_symlink(root / "d4", root / "d0" / "link4", ec);
    fsys::create_directory_symlink(root, root / "d7" / "loop", ec);
    ::mkfifo((root / "d8" / "pipe.txt").c_str(), 0600);

    GROUP("search:parallel-order");
    {
        // The same answers on one thread and on several, for every kind of
        // query, with and without a limit.
        struct Case {
            const char* query;
            bool regex, word, matchCase;
        };
        const Case cases[] = {
            {"return", false, false, false}, {"Return", false, false, true},
            {"café", false, false, false},   {"ΩMEGA", false, false, false},
            {"re", false, true, false},      {"ret[a-z]+n\\b", true, false, false},
            {"^\\s*\\(", true, false, false}, {"o+", true, false, true},
            {"(a)\\1", true, false, false},  {"日本", false, false, false},
            {"turn ret", false, false, false},
        };
        int compared = 0;
        for (const Case& k : cases) {
            for (std::size_t limit : {std::size_t(0), std::size_t(1), std::size_t(7), std::size_t(60)}) {
                FolderSearchOptions o;
                o.regex = k.regex;
                o.wholeWord = k.word;
                o.caseSensitive = k.matchCase;
                if (limit) o.maxMatches = limit;
                else o.maxMatches = 1000000;
                o.threads = 1;
                const FolderSearchResult one = search(root.string(), k.query, nullptr, o);
                for (unsigned t : {2u, 8u, 0u}) {
                    o.threads = t;
                    const FolderSearchResult many = search(root.string(), k.query, nullptr, o);
                    CHECK(describe(many) == describe(one));
                    ++compared;
                }
                if (limit) CHECK(one.matches.size() <= limit);
                if (limit && one.truncated) CHECK(one.matches.size() == limit);
            }
            FolderSearchOptions o;
            o.regex = k.regex;
            o.wholeWord = k.word;
            o.caseSensitive = k.matchCase;
            o.threads = 1;
            const std::string one = describe(planReplace(root.string(), k.query, nullptr, o));
            o.threads = 8;
            CHECK(describe(planReplace(root.string(), k.query, nullptr, o)) == one);
        }
        FolderSearchOptions o;
        o.include = "*.md, d1";
        o.exclude = "**/f2*";
        o.threads = 1;
        const std::string one = describe(search(root.string(), "return", nullptr, o));
        o.threads = 8;
        CHECK(describe(search(root.string(), "return", nullptr, o)) == one);
        o.include.clear();
        o.exclude.clear();
        o.useIgnoreFiles = false;
        o.threads = 1;
        const std::string todos = describe(findTodos(root.string(), nullptr, o));
        o.threads = 8;
        CHECK(describe(findTodos(root.string(), nullptr, o)) == todos);

        // The walk's rules hold: ignored files and folders, node_modules,
        // the pipe and the loop are left out; the symlinked folder is
        // searched once, under the name met first.
        o.useIgnoreFiles = true;
        const FolderSearchResult r = search(root.string(), "return", nullptr, o);
        CHECK(!anyHitIn(r, "d5/") && !anyHitIn(r, "d2/x.log") && !anyHitIn(r, "node_modules"));
        CHECK(!anyHitIn(r, "d1/f3.txt") && !anyHitIn(r, "d7/loop") && !anyHitIn(r, "d8/pipe"));
        CHECK(anyHitIn(r, "d0/link4/") && !anyHitIn(r, "d4/"));
        CHECK(anyHitIn(r, "nul.txt"));
        std::printf("  search: %d parallel results compared with one thread's\n", compared);
    }

    GROUP("search:needle");
    {
        // The lines a search finds through its needle are the ones a line
        // by line match finds, for random text and random queries.
        const fsys::path dir = root / "needle";
        std::vector<std::string> texts;
        for (int f = 0; f < 40; ++f) {
            texts.push_back(randomText(rng, 1 + static_cast<int>(rng() % 40)));
            char name[24];   // room for any int, so g++ sees no truncation
            std::snprintf(name, sizeof name, "n%02d.txt", f);
            writeFile(dir / name, texts.back());
        }
        const char* queries[] = {"return", "RETURN", "re", "café", "CAFÉ", "ωMEGA", "naïve",
                                 "foo_bar", "x x", "ret", "日本語", "turn", "TODO",
                                 // regular expressions (mode 4 and up)
                                 "ret[a-z]+n\\b", "(?:foo|bar)_?bar", "caf[ée] ", "RET.*urn",
                                 "\\bre", "x\\s+x", "Ω", "日本語+", "TODO:?$", "(a)\\1"};
        const int plainQueries = 13;
        int lines = 0;
        bool same = true;
        for (int qi = 0; qi < int(sizeof queries / sizeof *queries); ++qi) {
            const char* q = queries[qi];
            for (int mode = qi < plainQueries ? 0 : 4; mode < (qi < plainQueries ? 4 : 6); ++mode) {
                FolderSearchOptions o;
                o.caseSensitive = mode & 1;
                o.wholeWord = mode & 2;
                o.regex = mode & 4;
                o.maxMatches = 1000000;
                const FolderSearchResult r = search(dir.string(), q, nullptr, o);
                const TextFind::Query tq = makeQuery(q, o);
                std::vector<std::string> want;
                for (int f = 0; f < 40; ++f) {
                    char name[24];
                    std::snprintf(name, sizeof name, "n%02d.txt", f);
                    for (int ln = 1;; ++ln) {
                        std::string line;
                        if (!lineAt(texts[f], ln, &line)) break;
                        ++lines;
                        std::size_t col = 0, len = 0;
                        const bool hit = o.wholeWord || o.regex
                                             ? TextFind::findInLine(line, tq, &col, &len)
                                             : findInLine(line, normalizeQuery(q), o.caseSensitive,
                                                          &col, &len);
                        if (hit)
                            want.push_back(std::string(name) + ":" + std::to_string(ln) + ":" +
                                           std::to_string(col) + "+" + std::to_string(len) + "x" +
                                           std::to_string(TextFind::findAllInLine(line, tq).size()));
                    }
                }
                std::vector<std::string> got;
                for (const FolderSearchMatch& m : r.matches)
                    got.push_back(m.relativePath + ":" + std::to_string(m.line) + ":" +
                                  std::to_string(m.byteColumn) + "+" +
                                  std::to_string(m.byteLength) + "x" + std::to_string(m.count));
                if (got != want) {
                    same = false;
                    std::printf("  needle mismatch for \"%s\" mode %d: %zu rows, %zu wanted\n", q,
                                mode, got.size(), want.size());
                }
            }
        }
        CHECK(same);
        // A query holding a line break matches no line.
        FolderSearchOptions o;
        o.regex = true;
        CHECK(search(dir.string(), "return\\nx", nullptr, o).matches.empty());
        std::printf("  search: needle rows matched a line by line search over %d lines\n", lines);
    }

    GROUP("search:skipped");
    {
        // Larger than 1 MB, or text not in UTF-8, is named; a binary (a
        // NUL byte early on) is left out without a word.
        FolderSearchOptions o;
        o.threads = 4;
        const FolderSearchResult r = search(root.string(), "return", nullptr, o);
        CHECK(r.skippedTooLarge == 1 && r.skippedNotUtf8 == 1);
        CHECK(r.skipped.size() == 2);
        if (r.skipped.size() == 2) {
            CHECK(r.skipped[0].relativePath == "big.txt" && r.skipped[0].tooLarge);
            CHECK(r.skipped[1].relativePath == "latin1.txt" && !r.skipped[1].tooLarge);
        }
        // Only what the search reached: a limit of one stops before them.
        const fsys::path two = root / "two";
        writeFile(two / "a.txt", "return\n");
        writeFile(two / "b.txt", "caf\xE9\n");
        o.maxMatches = 1;
        const FolderSearchResult stopped = search(two.string(), "return", nullptr, o);
        CHECK(stopped.truncated && stopped.skipped.empty() && stopped.skippedNotUtf8 == 0);
        o.maxMatches = 2;
        CHECK(search(two.string(), "return", nullptr, o).skippedNotUtf8 == 1);
        fsys::remove_all(two, ec);
        // The list stops at kMaxSkipsListed; the count does not.
        const fsys::path many = root / "many";
        for (std::size_t i = 0; i < kMaxSkipsListed + 5; ++i)
            writeFile(many / ("l" + std::to_string(i) + ".txt"), "caf\xE9\n");
        o.maxMatches = 2000;
        const FolderSearchResult m = search(many.string(), "return", nullptr, o);
        CHECK(m.skippedNotUtf8 == kMaxSkipsListed + 5 && m.skipped.size() == kMaxSkipsListed);
        fsys::remove_all(many, ec);
    }

    GROUP("search:cancel");
    {
        std::atomic<bool> stop(true);
        FolderSearchOptions o;
        CHECK(search(root.string(), "return", &stop, o).cancelled);
        o.regex = true;
        CHECK(planReplace(root.string(), "ret", &stop, o).cancelled);
    }

    GROUP("search:cpu");
    {
        // 400 files of 300 lines each: a search reads 400 files and
        // matches 120,000 lines. Bounded in CPU time, every thread's, so a
        // loaded machine does not fail it; loosely, so a slow one does not.
        const fsys::path dir = root / "cpu";
        for (int f = 0; f < 400; ++f)
            writeFile(dir / ("p" + std::to_string(f / 40)) / ("c" + std::to_string(f) + ".py"),
                      randomText(rng, 300));
        FolderSearchOptions o;
        o.maxMatches = 1000000;
        double cpu[3] = {0, 0, 0};
        std::size_t rows = 0;
        const char* what[] = {"plain", "regex", "one thread"};
        for (int k = 0; k < 3; ++k) {
            o.regex = k == 1;
            o.threads = k == 2 ? 1 : 0;
            const double c0 = processCpuSeconds();
            // The regex over one folder of 40: the engine itself is slow
            // without -O2, which is how the tests are built.
            const FolderSearchResult r =
                k == 1 ? search((dir / "p0").string(), "ret[a-z]+n\\b", nullptr, o)
                       : search(dir.string(), "return", nullptr, o);
            cpu[k] = processCpuSeconds() - c0;
            if (k == 0) rows = r.matches.size();
            CHECK(r.filesSearched == (k == 1 ? 40u : 400u));
        }
        CHECK(rows > 1000);
        CHECK(cpu[0] < 3.0 && cpu[1] < 4.0 && cpu[2] < 3.0);
        for (int k = 0; k < 3; ++k)
            std::printf("  search %s: %d files, %.0f ms CPU\n", what[k], k == 1 ? 40 : 400,
                        cpu[k] * 1000);
    }

    fsys::remove_all(root, ec);
}
