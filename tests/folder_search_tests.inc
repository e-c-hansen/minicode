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
    for (const std::string& rel : p.otherEncoding) s += rel + " other encoding\n";
    return s + "total " + std::to_string(p.matches) + (p.cancelled ? " cancelled" : "") + p.error +
           " other " + std::to_string(p.otherEncodingCount);
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
                            ("mutible-search-test-" + std::to_string(::getpid()));
    std::error_code ec;
    fsys::remove_all(root, ec);

    // A tree as varied as the walk's rules: nested folders, files in and
    // out of .gitignore, skipped folders, a symlinked folder and a loop,
    // a file too large, one in Latin-1, binaries, and a pipe.
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
                // big.txt's 1 MB first line is left out (skipped, named):
                // a backreference runs on std::regex there, a start at a
                // time, and under the sanitizers its count of matches on
                // that line was cut short by the 2 s limit at different
                // places, so the answers differed by timing. The limit is
                // checked as pieces are read, so they are shorter than it.
                // Streamed files are compared below.
                o.maxStreamedLine = 64 * 1024;
                o.streamPiece = 16 * 1024;
                o.threads = 1;
                const FolderSearchResult one = search(root.string(), k.query, nullptr, o);
                for (unsigned t : {2u, 8u, 0u}) {
                    o.threads = t;
                    const FolderSearchResult many = search(root.string(), k.query, nullptr, o);
                    CHECK(describe(many) == describe(one));
                    if (describe(many) != describe(one))
                        std::fprintf(stderr, "  %s, limit %zu, %u threads:\n%s\n-- one thread:\n%s\n", k.query,
                                     limit, t, describe(many).c_str(), describe(one).c_str());
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
        // A binary (a NUL byte early on) is left out without a word, large
        // or not. Text not in UTF-8 is searched, read as Windows-1252
        // (until 2026-11-18 it was named and left out). A text file over
        // 1 MB is searched (until 2026-11-16 it was skipped and named),
        // unless a line of it is longer than maxStreamedLine: then it is
        // named.
        FolderSearchOptions o;
        o.threads = 4;
        const FolderSearchResult r = search(root.string(), "return", nullptr, o);
        CHECK(r.skippedTooLarge == 0 && r.skippedNotUtf8 == 0 && r.skipped.empty());
        bool bigLine2 = false, latin1 = false;
        for (const FolderSearchMatch& m : r.matches) {
            if (m.relativePath == "big.txt") bigLine2 = m.line == 2 && m.text == "return";
            if (m.relativePath == "latin1.txt")
                latin1 = m.line == 1 && m.column == 6 && m.text == "caf\xC3\xA9 return";
            CHECK(m.relativePath != "small.bin" && m.relativePath != "big.bin");
        }
        CHECK(bigLine2);
        CHECK(latin1);
        o.maxStreamedLine = 512 * 1024;    // big.txt's first line is longer
        o.streamPiece = 64 * 1024;         // and read in pieces shorter than it
        const FolderSearchResult longLine = search(root.string(), "return", nullptr, o);
        CHECK(longLine.skippedTooLarge == 1 && longLine.skipped.size() == 1 &&
              longLine.skipped[0].relativePath == "big.txt" && longLine.skipped[0].tooLarge);
        // Only what the search reached: a limit of one stops before them.
        const fsys::path two = root / "two";
        writeFile(two / "a.txt", "return\n");
        writeFile(two / "b.txt", std::string(600 * 1024, 'b') + "\n");
        o.maxFileSize = 0;   // read in pieces, so its 600 KB line is over the limit
        o.maxMatches = 1;
        const FolderSearchResult stopped = search(two.string(), "return", nullptr, o);
        CHECK(stopped.truncated && stopped.skipped.empty() && stopped.skippedTooLarge == 0);
        o.maxMatches = 2;
        CHECK(search(two.string(), "return", nullptr, o).skippedTooLarge == 1);
        fsys::remove_all(two, ec);
        // The list stops at kMaxSkipsListed; the count does not.
        const fsys::path many = root / "many";
        for (std::size_t i = 0; i < kMaxSkipsListed + 5; ++i)
            writeFile(many / ("l" + std::to_string(i) + ".txt"), std::string(10000, 'l') + "\n");
        o.maxMatches = 2000;
        o.maxFileSize = 0;          // every file read in pieces (the first 8 KB, then
        o.streamPiece = 1024;       // pieces shorter than its line),
        o.maxStreamedLine = 9000;   // which is over the limit
        const FolderSearchResult m = search(many.string(), "return", nullptr, o);
        CHECK(m.skippedTooLarge == kMaxSkipsListed + 5 && m.skipped.size() == kMaxSkipsListed);
        fsys::remove_all(many, ec);
    }

    GROUP("search:latin1");
    {
        // Text that is not UTF-8 is read as Windows-1252 (Latin-1 but for
        // 0x80..0x9F), as VS Code and Zed find "café" in a Latin-1 file;
        // a NUL anywhere in it still means binary, left out unnamed.
        CHECK(decodeWindows1252("caf\xE9") == "caf\xC3\xA9");
        CHECK(decodeWindows1252("plain ASCII\r\n") == "plain ASCII\r\n");
        CHECK(decodeWindows1252("\x80 \x93q\x94 \x96 \x9F") ==
              "\xE2\x82\xAC \xE2\x80\x9Cq\xE2\x80\x9D \xE2\x80\x93 \xC5\xB8");
        CHECK(decodeWindows1252("\x81\x8D\x8F\x90\x9D") == "\xC2\x81\xC2\x8D\xC2\x8F\xC2\x90\xC2\x9D");
        CHECK(decodeWindows1252("\xA0\xFF") == "\xC2\xA0\xC3\xBF");
        std::string all;
        for (int b = 0; b < 256; ++b) all += static_cast<char>(b);
        const std::string decoded = decodeWindows1252(all);
        std::size_t chars = 0;
        for (const char c : decoded) chars += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
        CHECK(isValidUtf8(decoded) && chars == 256);

        const fsys::path dir = root / "latin";
        writeFile(dir / "old-latin1.log", "first line\nna\xEFve caf\xE9 au lait\n\x93quoted\x94\n");
        writeFile(dir / "notes.txt", "a caf\xC3\xA9 in UTF-8\n");
        // Not UTF-8, its NUL past the first 8 KB: binary all the same.
        writeFile(dir / "late-nul.dat",
                  std::string(9000, 'a') + std::string("\xE9 caf\xE9\0 x\n", 11));
        FolderSearchOptions o;
        FolderSearchResult r = search(dir.string(), "CAF\xC3\x89 au", nullptr, o);
        CHECK(r.matches.size() == 1 && r.skipped.empty() && r.skippedNotUtf8 == 0);
        if (r.matches.size() == 1) {
            const FolderSearchMatch& m = r.matches[0];
            CHECK(m.relativePath == "old-latin1.log" && m.line == 2 && m.column == 7);
            CHECK(m.byteColumn == 7 && m.byteLength == 8);   // in the text decoded to UTF-8
            CHECK(m.text == "na\xC3\xAFve caf\xC3\xA9 au lait");
        }
        r = search(dir.string(), "caf\xC3\xA9", nullptr, o);
        CHECK(r.matches.size() == 2 && r.filesMatched == 2);   // not late-nul.dat
        r = search(dir.string(), "\xE2\x80\x9Cquoted\xE2\x80\x9D", nullptr, o);
        CHECK(r.matches.size() == 1 && r.matches[0].line == 3);
        // A regex and whole word run on the decoded text too.
        o.regex = true;
        CHECK(search(dir.string(), "na.ve", nullptr, o).matches.size() == 1);
        o.regex = false;
        o.wholeWord = true;
        CHECK(search(dir.string(), "caf\xC3\xA9", nullptr, o).matches.size() == 2);
        o.wholeWord = false;
        // Read in pieces (a large file), the same rows, and a file that
        // turns out not to be UTF-8 late switches there: its earlier
        // matches are kept.
        std::string late;
        for (int i = 0; i < 40; ++i) late += "return " + std::to_string(i) + "\n";
        late += "\xE9t\xE9 return\n";
        writeFile(dir / "late.txt", late);
        const std::string whole = describe(search(dir.string(), "return", nullptr, o));
        FolderSearchOptions pieces;
        pieces.maxFileSize = 0;
        for (std::size_t piece : {std::size_t(16), std::size_t(64), std::size_t(1000)}) {
            pieces.streamPiece = piece;
            const std::string got = describe(search(dir.string(), "return", nullptr, pieces));
            if (got != whole) std::printf("  latin1 pieces of %zu:\n%s\n--\n%s\n", piece,
                                          got.c_str(), whole.c_str());
            CHECK(got == whole);
            const FolderSearchResult e = search(dir.string(), "\xC3\xA9t\xC3\xA9", nullptr, pieces);
            CHECK(e.matches.size() == 1 && e.matches[0].line == 41);
            CHECK(search(dir.string(), "caf\xC3\xA9", nullptr, pieces).matches.size() == 2);
        }
        CHECK(search(dir.string(), "return", nullptr, o).matches.size() == 41);
        // Replace in Folder writes UTF-8 or UTF-16 only: a Latin-1 file
        // holding a match is left alone and named, one without is not.
        FolderReplacePlan plan = planReplace(dir.string(), "caf\xC3\xA9", nullptr, o);
        CHECK(plan.files.size() == 1 && plan.files[0].relativePath == "notes.txt");
        CHECK(plan.otherEncodingCount == 1 && plan.otherEncoding.size() == 1 &&
              plan.otherEncoding[0] == "old-latin1.log");
        plan = planReplace(dir.string(), "first", nullptr, o);
        CHECK(plan.files.empty() && plan.otherEncoding.size() == 1);
        plan = planReplace(dir.string(), "in UTF", nullptr, o);
        CHECK(plan.files.size() == 1 && plan.otherEncoding.empty() && plan.otherEncodingCount == 0);
        // The TODO list reads it too.
        writeFile(dir / "todo.txt", "// TODO: caf\xE9\n");
        const FolderSearchResult todos = findTodos(dir.string(), nullptr, o);
        bool todo = false;
        for (const FolderSearchMatch& m : todos.matches)
            if (m.relativePath == "todo.txt") todo = m.text == "// TODO: caf\xC3\xA9";
        CHECK(todo);
        fsys::remove_all(dir, ec);
    }

    GROUP("search:context");
    {
        // With contextLines, each row carries the lines just above and
        // below (display text, as its own), as Zed and VS Code show a hit
        // in its excerpt; a line a neighbouring row shows already is left
        // out, so consecutive hits read as one excerpt.
        const fsys::path dir = root / "context";
        auto ctx = [](const FolderSearchResult& r) {
            std::string s;
            for (const FolderSearchMatch& m : r.matches) {
                s += m.relativePath + ":" + std::to_string(m.line) + " [";
                for (const std::string& b : m.before) s += b + "|";
                s += "] " + m.text + " [";
                for (const std::string& a : m.after) s += a + "|";
                s += "]\n";
            }
            return s;
        };
        writeFile(dir / "a.txt", "one\ntwo\nthree needle\nfour\nfive\n");
        FolderSearchOptions o;
        CHECK(ctx(search(dir.string(), "needle", nullptr, o)) == "a.txt:3 [] three needle []\n");
        o.contextLines = 1;
        CHECK(ctx(search(dir.string(), "needle", nullptr, o)) == "a.txt:3 [two|] three needle [four|]\n");
        o.contextLines = 2;
        CHECK(ctx(search(dir.string(), "needle", nullptr, o)) ==
              "a.txt:3 [one|two|] three needle [four|five|]\n");
        o.contextLines = 3;   // no phantom line after the last line break
        CHECK(ctx(search(dir.string(), "needle", nullptr, o)) ==
              "a.txt:3 [one|two|] three needle [four|five|]\n");
        o.contextLines = 1;
        // The first and last lines; every kind of line break; display text
        // (trimmed, escapes and controls gone).
        fsys::remove_all(dir, ec);
        writeFile(dir / "b.txt", "needle first\nsecond\nlast needle");
        writeFile(dir / "c.txt", "a\r\nb needle\rc\xE2\x80\xA9" "d needle\n");
        writeFile(dir / "d.txt", "   indented \x1b[31mred\x1b[0m  \n\tneedle\n\n");
        CHECK(ctx(search(dir.string(), "needle", nullptr, o)) ==
              "b.txt:1 [] needle first [second|]\n"
              "b.txt:3 [] last needle []\n"
              "c.txt:2 [a|] b needle [c|]\n"
              "c.txt:4 [] d needle []\n"
              "d.txt:2 [indented red|] needle [|]\n");
        // Neighbours: next to each other, one line apart, two apart.
        fsys::remove_all(dir, ec);
        writeFile(dir / "e.txt", "x\nneedle a\nneedle b\ny\nneedle c\nz\nw\nneedle d\nv\n");
        CHECK(ctx(search(dir.string(), "needle", nullptr, o)) ==
              "e.txt:2 [x|] needle a []\n"
              "e.txt:3 [] needle b [y|]\n"
              "e.txt:5 [] needle c [z|]\n"
              "e.txt:8 [w|] needle d [v|]\n");
        // A Latin-1 file's lines around are decoded too; the TODO list has
        // none.
        writeFile(dir / "f.txt", "caf\xE9\nneedle\nna\xEFve\n");
        CHECK(ctx(search(dir.string(), "needle", nullptr, o)).find(
                  "f.txt:2 [caf\xC3\xA9|] needle [na\xC3\xAFve|]") != std::string::npos);
        writeFile(dir / "g.txt", "above\n// TODO: x\nbelow\n");
        const FolderSearchResult todos = findTodos(dir.string(), nullptr, o);
        CHECK(!todos.matches.empty());
        for (const FolderSearchMatch& m : todos.matches) CHECK(m.before.empty() && m.after.empty());
        fsys::remove_all(dir, ec);

        // dropRepeatedContext on rows as a front end might join them.
        std::vector<FolderSearchMatch> rows(4);
        rows[0].path = rows[1].path = rows[2].path = "p";
        rows[3].path = "q";
        rows[0].line = 10; rows[0].before = {"8", "9"}; rows[0].after = {"11", "12"};
        rows[1].line = 12; rows[1].before = {"10", "11"}; rows[1].after = {"13", "14"};
        rows[2].line = 17; rows[2].before = {"15", "16"}; rows[2].after = {"18"};
        rows[3].line = 18; rows[3].before = {"16", "17"};
        dropRepeatedContext(rows);
        CHECK(rows[0].before.size() == 2 && rows[0].after == std::vector<std::string>{"11"});
        CHECK(rows[1].before.empty() && rows[1].after.size() == 2);
        const std::vector<std::string> fifteen = {"15", "16"};
        CHECK(rows[2].before == fifteen && rows[2].after.size() == 1);
        CHECK(rows[3].before.size() == 2);   // another file

        // Read in pieces (a large file), the same context as read whole,
        // for every piece size, limit and number of lines: across a
        // piece's end both ways, and finishing a row's lines after when
        // the limit came in its piece.
        std::mt19937 crng(20261118);
        std::string big;
        for (int i = 0; i < 400; ++i) {
            const int kind = static_cast<int>(crng() % 9);
            big += kind == 0 ? "needle " + std::to_string(i) : kind == 1 ? "" :
                   std::string(1 + crng() % 40, static_cast<char>('a' + crng() % 26));
            big += (crng() % 7 == 0) ? "\r\n" : "\n";
        }
        writeFile(dir / "big.txt", big);
        writeFile(dir / "big2.txt", big + "tail needle");   // no last line break
        int compared = 0, differ = 0;
        for (std::size_t lines : {std::size_t(1), std::size_t(2), std::size_t(3)})
            for (std::size_t limit : {std::size_t(1), std::size_t(7), std::size_t(2000)}) {
                FolderSearchOptions whole;
                whole.contextLines = lines;
                whole.maxMatches = limit;
                const std::string want = ctx(search(dir.string(), "needle", nullptr, whole));
                for (std::size_t piece : {std::size_t(16), std::size_t(64), std::size_t(1000)}) {
                    FolderSearchOptions pieces = whole;
                    pieces.maxFileSize = 0;
                    pieces.streamPiece = piece;
                    const std::string got = ctx(search(dir.string(), "needle", nullptr, pieces));
                    ++compared;
                    if (got != want && ++differ < 3)
                        std::printf("  context in pieces of %zu, %zu lines, limit %zu:\n%s\n--\n%s\n",
                                    piece, lines, limit, got.c_str(), want.c_str());
                }
            }
        CHECK(differ == 0);
        std::printf("  search: %d streamed searches gave the lines around as whole-file ones\n",
                    compared);
        fsys::remove_all(dir, ec);
    }

    GROUP("search:utf16");
    {
        // UTF-16 with a byte order mark is decoded and searched, as ripgrep
        // does (round 5's workspace 3: every such file has NULs, so it fell
        // out as binary without a word). Both byte orders; an astral
        // character before the match; a lone surrogate as U+FFFD.
        auto utf16 = [](const std::u16string& s, bool le) {
            std::string b = le ? std::string("\xFF\xFE", 2) : std::string("\xFE\xFF", 2);
            for (char16_t u : s) {
                const char hi = static_cast<char>(u >> 8), lo = static_cast<char>(u & 0xFF);
                b += le ? lo : hi;
                b += le ? hi : lo;
            }
            return b;
        };
        const fsys::path dir = root / "utf16";
        const std::u16string text = u"first\r\n\U0001F600 x = return 1\r\nlast café return\r\n";
        writeFile(dir / "le.ps1", utf16(text, true));
        writeFile(dir / "be.log", utf16(text, false));
        writeFile(dir / "lone.txt", utf16(std::u16string(u"a ") + char16_t(0xD800) + u" return\n", true));
        writeFile(dir / "odd.txt", utf16(u"return\n", true) + "x");
        writeFile(dir / "big.txt", utf16(std::u16string(600 * 1024, u'a') + u"\nreturn\n", true));
        FolderSearchOptions o;
        o.threads = 2;
        const FolderSearchResult r = search(dir.string(), "return", nullptr, o);
        // be.log, big.txt, le.ps1, lone.txt, odd.txt in name order; big.txt
        // (over 1 MB) read in pieces, its NULs no longer hiding it.
        CHECK(r.filesSearched == 5 && r.matches.size() == 7);
        CHECK(r.skippedTooLarge == 0 && r.skipped.empty());
        bool big = false;
        for (const FolderSearchMatch& m : r.matches)
            if (m.relativePath == "big.txt") big = m.line == 2 && m.text == "return";
        CHECK(big);
        for (const FolderSearchMatch& m : r.matches) {
            if (m.relativePath == "be.log" || m.relativePath == "le.ps1") {
                // Line 2: the emoji (4 UTF-8 bytes, one code point) and " x = ".
                CHECK((m.line == 2 && m.byteColumn == 9 && m.column == 7 &&
                       m.text == "\xF0\x9F\x98\x80 x = return 1") ||
                      (m.line == 3 && m.text == "last caf\xC3\xA9 return"));
            }
        }
        CHECK(!r.matches.empty() && r.matches[0].relativePath == "be.log");
        bool lone = false;
        for (const FolderSearchMatch& m : r.matches)
            if (m.relativePath == "lone.txt") lone = m.text == "a \xEF\xBF\xBD return";
        CHECK(lone);

        // Replace in Folder counts only files it can write back exactly:
        // not the lone surrogate's, not the odd byte's.
        // Nor big.txt, over 1 MB, which it names instead.
        const FolderReplacePlan plan = planReplace(dir.string(), "return", nullptr, o);
        CHECK(describe(plan) == "be.log 2\nle.ps1 2\ntotal 4 other 0");
        CHECK(plan.tooLargeCount == 1 && plan.tooLarge.size() == 1 &&
              plan.tooLarge[0] == "big.txt");
        // A large file without a match is not named.
        CHECK(planReplace(dir.string(), "first", nullptr, o).tooLarge.empty());

        // decodeUtf16 and encodeText round-trip both orders.
        std::string u8;
        TextEncoding enc = TextEncoding::Utf8;
        CHECK(decodeUtf16(utf16(text, true), &u8, &enc, true) && enc == TextEncoding::Utf16LE);
        CHECK(u8 == "first\r\n\xF0\x9F\x98\x80 x = return 1\r\nlast caf\xC3\xA9 return\r\n");
        CHECK(encodeText(u8, enc) == utf16(text, true));
        CHECK(decodeUtf16(utf16(text, false), &u8, &enc, true) && enc == TextEncoding::Utf16BE);
        CHECK(encodeText(u8, enc) == utf16(text, false));
        CHECK(encodeText(u8, TextEncoding::Utf8) == u8);
        CHECK(!decodeUtf16("plain", &u8, &enc, false));
        CHECK(!decodeUtf16(utf16(u"a", true) + "x", &u8, &enc, true));
        CHECK(decodeUtf16(utf16(u"a", true) + "x", &u8, &enc, false) && u8 == "a\xEF\xBF\xBD");
        CHECK(decodeUtf16(std::string("\xFF\xFE", 2), &u8, &enc, true) && u8.empty());
        // readTextFile decodes only when asked for the encoding, so a
        // caller that would write the bytes back as UTF-8 never gets one.
        std::string back;
        CHECK(!readTextFile((dir / "le.ps1").string(), 1 << 20, &back));
        CHECK(readTextFile((dir / "le.ps1").string(), 1 << 20, &back, &enc) &&
              enc == TextEncoding::Utf16LE);
        CHECK(back == "first\r\n\xF0\x9F\x98\x80 x = return 1\r\nlast caf\xC3\xA9 return\r\n");
        CHECK(!readTextFile((dir / "lone.txt").string(), 1 << 20, &back, &enc));
        CHECK(readTextFile((dir / "lone.txt").string(), 1 << 20, &back, nullptr) == false);
        writeFile(dir / "plain.txt", "return\n");
        CHECK(readTextFile((dir / "plain.txt").string(), 1 << 20, &back, &enc) &&
              enc == TextEncoding::Utf8 && back == "return\n");
        fsys::remove_all(dir, ec);
    }

    GROUP("encoding:file-text");
    {
        // A tab's file read as text and written back (round 6's D10): UTF-8,
        // UTF-8 with its mark, UTF-16 in both orders with theirs. An unedited
        // file comes back byte for byte; one that would not decode exactly
        // is refused, so it is never opened as text and saved changed.
        auto utf16 = [](const std::u16string& s, bool le) {
            std::string b = le ? std::string("\xFF\xFE", 2) : std::string("\xFE\xFF", 2);
            for (char16_t u : s) {
                const char hi = static_cast<char>(u >> 8), lo = static_cast<char>(u & 0xFF);
                b += le ? lo : hi;
                b += le ? hi : lo;
            }
            return b;
        };
        const std::u16string text = u"a\r\n\U0001F600 caf\u00E9\n\uFEFFz";
        const std::string text8 = "a\r\n\xF0\x9F\x98\x80 caf\xC3\xA9\n\xEF\xBB\xBFz";
        struct Case { std::string bytes; TextEncoding enc; bool bom; std::string utf8; };
        const Case cases[] = {
            {text8, TextEncoding::Utf8, false, text8},
            {"\xEF\xBB\xBF" + text8, TextEncoding::Utf8, true, text8},
            {utf16(text, true), TextEncoding::Utf16LE, true, text8},
            {utf16(text, false), TextEncoding::Utf16BE, true, text8},
            {"", TextEncoding::Utf8, false, ""},
            {std::string("\xFF\xFE", 2), TextEncoding::Utf16LE, true, ""},
        };
        for (const Case& c : cases) {
            std::string b = c.bytes;
            TextEncoding e = TextEncoding::Utf8;
            bool bom = !c.bom;
            CHECK(decodeFileText(&b, &e, &bom));
            CHECK(e == c.enc && bom == c.bom && b == c.utf8);
            CHECK(encodeFileText(b, e, bom) == c.bytes);
            TextEncoding e2;
            bool bom2;
            CHECK(parseEncodingName(encodingName(e, bom), &e2, &bom2) && e2 == e && bom2 == bom);
        }
        CHECK(std::string(encodingName(TextEncoding::Utf16BE, true)) == "utf16be");
        CHECK(std::string(encodingName(TextEncoding::Utf8, true)) == "utf8bom");
        TextEncoding e = TextEncoding::Utf8;
        bool bom = false;
        CHECK(!parseEncodingName("latin1", &e, &bom) && !parseEncodingName("", &e, &bom));
        // Not exact: a lone surrogate, a lone low half, an odd last byte.
        for (const std::string& bad :
             {utf16(std::u16string(u"a") + char16_t(0xD800) + u"b", true),
              utf16(std::u16string(1, char16_t(0xDC00)), false),
              utf16(u"ab", true) + "x"}) {
            std::string b = bad;
            CHECK(!decodeFileText(&b, &e, &bom));
            CHECK(b == bad);   // left as it was
        }
        // An edit encodes back in the file's own way, mark first.
        CHECK(encodeFileText("A\n", TextEncoding::Utf16LE, true) == utf16(u"A\n", true));
        CHECK(encodeFileText("A\n", TextEncoding::Utf16BE, true) == utf16(u"A\n", false));
        CHECK(encodeFileText("A\n", TextEncoding::Utf8, true) == "\xEF\xBB\xBF" "A\n");
        // Random UTF-16 files: whatever decodes comes back exactly, and only
        // a lone surrogate or an odd length is refused.
        std::mt19937 rng(10);
        int decoded = 0, refused = 0;
        for (int round = 0; round < 4000; ++round) {
            const bool le = rng() & 1;
            std::string b = le ? std::string("\xFF\xFE", 2) : std::string("\xFE\xFF", 2);
            const int n = (int)(rng() % 24);
            bool lone = false;
            for (int i = 0; i < n; ++i) {
                // Mostly characters, some pairs, now and then half of one.
                static const char16_t plain[] = {u'a', u'\n', u'\r', 0, 0xE9, 0xFEFF, 0xFFFF};
                static const char16_t half[] = {0xD83D, 0xDE00, 0xDBFF, 0xDC00};
                std::u16string us;
                const unsigned r = rng() % 40;
                if (r == 0) us = half[rng() % 4];
                else if (r < 5) us = u"\U0001F600";
                else us = plain[rng() % (sizeof(plain) / sizeof(plain[0]))];
                for (char16_t u : us) {
                    b += le ? (char)(u & 0xFF) : (char)(u >> 8);
                    b += le ? (char)(u >> 8) : (char)(u & 0xFF);
                }
            }
            if (rng() % 8 == 0) b += 'x';
            // Lone surrogates, as the decoder must see them.
            const size_t units = (b.size() - 2) / 2;
            auto unitAt = [&](size_t i) -> unsigned {
                const unsigned x = (unsigned char)b[2 + 2 * i], y = (unsigned char)b[3 + 2 * i];
                return le ? (x | (y << 8)) : ((x << 8) | y);
            };
            for (size_t i = 0; i < units; ++i) {
                const unsigned u = unitAt(i);
                if (u >= 0xDC00 && u < 0xE000) { lone = true; break; }
                if (u >= 0xD800 && u < 0xDC00) {
                    const unsigned l = i + 1 < units ? unitAt(i + 1) : 0;
                    if (l < 0xDC00 || l >= 0xE000) { lone = true; break; }
                    ++i;
                }
            }
            const bool exact = !lone && !(b.size() & 1);
            std::string t = b;
            const bool ok = decodeFileText(&t, &e, &bom);
            CHECK(ok == exact);
            if (ok) {
                ++decoded;
                CHECK(encodeFileText(t, e, bom) == b);
            } else {
                ++refused;
            }
        }
        CHECK(decoded > 500 && refused > 500);
    }

    GROUP("search:stream");
    {
        // A file over maxFileSize is read and matched a piece at a time,
        // each piece cut at a line break: whatever the piece's size (some
        // smaller than a line, cutting \r\n, U+2029 and UTF-8 characters
        // anywhere), every query finds what reading the file whole finds,
        // in UTF-8 and in UTF-16 of both orders.
        auto utf16 = [](const std::string& u8, bool le) {
            std::string b = le ? std::string("\xFF\xFE", 2) : std::string("\xFE\xFF", 2);
            for (std::size_t i = 0, len; i < u8.size(); i += len) {
                char32_t c = static_cast<unsigned char>(u8[i]);
                len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
                if (len > 1) {
                    c &= len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07;
                    for (std::size_t k = 1; k < len; ++k) c = (c << 6) | (u8[i + k] & 0x3F);
                }
                std::u16string us;
                if (c >= 0x10000) {
                    us += static_cast<char16_t>(0xD800 + ((c - 0x10000) >> 10));
                    us += static_cast<char16_t>(0xDC00 + ((c - 0x10000) & 0x3FF));
                } else {
                    us += static_cast<char16_t>(c);
                }
                for (char16_t u : us) {
                    b += le ? static_cast<char>(u & 0xFF) : static_cast<char>(u >> 8);
                    b += le ? static_cast<char>(u >> 8) : static_cast<char>(u & 0xFF);
                }
            }
            return b;
        };
        const fsys::path dir = root / "stream";
        std::mt19937 srng(20261116);
        for (int f = 0; f < 6; ++f) {
            std::string text = randomText(srng, 200 + static_cast<int>(srng() % 400));
            if (f == 0) text += std::string(3000, 'q') + " return " + std::string(500, 'r');
            if (f == 1) text += "\r";   // ends in a lone \r
            writeFile(dir / ("u" + std::to_string(f) + ".txt"), text);
            if (f < 3) writeFile(dir / ("w" + std::to_string(f) + ".txt"), utf16(text, f % 2));
        }
        writeFile(dir / "todo.txt", randomText(srng, 300));
        struct Case {
            const char* query;
            bool regex, word, matchCase;
        };
        const Case cases[] = {
            {"return", false, false, false}, {"RETURN", false, false, true},
            {"café", false, false, false},   {"re", false, true, false},
            {"ret[a-z]+n\\b", true, false, false}, {"^\\s*\\(", true, false, false},
            {"日本", false, false, false},   {"turn ret", false, false, false},
        };
        int compared = 0, differ = 0;
        for (std::size_t piece : {std::size_t(16), std::size_t(37), std::size_t(1000)}) {
            for (const Case& k : cases) {
                for (std::size_t limit : {std::size_t(5), std::size_t(100000)}) {
                    FolderSearchOptions whole;
                    whole.regex = k.regex;
                    whole.wholeWord = k.word;
                    whole.caseSensitive = k.matchCase;
                    whole.maxMatches = limit;
                    whole.threads = 2;
                    FolderSearchOptions pieces = whole;
                    pieces.maxFileSize = 0;
                    pieces.streamPiece = piece;
                    const std::string a = describe(search(dir.string(), k.query, nullptr, whole));
                    const std::string b = describe(search(dir.string(), k.query, nullptr, pieces));
                    ++compared;
                    if (a != b && ++differ < 3)
                        std::printf("  stream mismatch for \"%s\", piece %zu:\n%s\n--\n%s\n",
                                    k.query, piece, a.c_str(), b.c_str());
                }
            }
            FolderSearchOptions whole, pieces;
            pieces.maxFileSize = 0;
            pieces.streamPiece = piece;
            const std::string a = describe(findTodos(dir.string(), nullptr, whole));
            const std::string b = describe(findTodos(dir.string(), nullptr, pieces));
            ++compared;
            if (a != b && ++differ < 3) std::printf("  stream TODO mismatch, piece %zu\n", piece);
        }
        CHECK(differ == 0);
        std::printf("  search: %d streamed searches matched whole-file ones\n", compared);
        // A late byte that is not UTF-8 switches the rest of the file to
        // Windows-1252, its earlier matches kept (search:latin1 has more).
        writeFile(dir / "bad" / "bad.txt", randomText(srng, 300) + "\n\xE9 late\n");
        FolderSearchOptions o;
        o.maxFileSize = 0;
        o.streamPiece = 64;
        const FolderSearchResult r = search((dir / "bad").string(), "re", nullptr, o);
        CHECK(r.skippedNotUtf8 == 0 && r.skipped.empty() && !r.matches.empty());
        const FolderSearchResult l = search((dir / "bad").string(), "\xC3\xA9 late", nullptr, o);
        CHECK(l.matches.size() == 1);
        // The cancel flag, raised as a file is read, stops it mid-file.
        std::atomic<bool> stop(false);
        o.threads = 1;
        o.onFileRead = [&stop] { stop = true; };
        CHECK(search(dir.string(), "return", &stop, o).cancelled);
        fsys::remove_all(dir, ec);
    }

    GROUP("search:large-log");
    {
        // A 24 MB log of 260,000 lines, three of them holding the query:
        // found on the right lines, in bounded CPU, and in pieces (the whole
        // file is never held).
        const fsys::path dir = root / "log";
        std::string log;
        log.reserve(25u << 20);
        const int lines = 260000;
        for (int i = 1; i <= lines; ++i) {
            log += "2026-11-15T10:00:00.000Z INFO request id=" + std::to_string(i) +
                   " path=/api/v1/orders status=200 time=12ms user=u" +
                   std::to_string(i % 977) + "\n";
            if (i == 7 || i == 130000 || i == lines - 1)
                log += "2026-11-15T10:00:00.000Z WARN payment declined for order " +
                       std::to_string(i) + "\n";
        }
        writeFile(dir / "app.log", log);
        FolderSearchOptions o;
        const double c0 = processCpuSeconds();
        const FolderSearchResult r = search(dir.string(), "payment declined", nullptr, o);
        const double cpu = processCpuSeconds() - c0;
        CHECK(r.matches.size() == 3 && r.skipped.empty() && r.filesSearched == 1);
        if (r.matches.size() == 3) {
            // Each WARN line follows its request line and the WARNs before it.
            CHECK(r.matches[0].line == 8 && r.matches[1].line == 130002 &&
                  r.matches[2].line == lines + 2);
            CHECK(r.matches[1].text ==
                  "2026-11-15T10:00:00.000Z WARN payment declined for order 130000");
        }
        CHECK(cpu < 2.0);
        std::printf("  search: %.0f MB log, %.0f ms CPU\n", log.size() / 1048576.0, cpu * 1000);
        fsys::remove_all(dir, ec);
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
