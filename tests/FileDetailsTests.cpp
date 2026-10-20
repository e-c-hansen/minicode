// FileDetailsTests.cpp — included by run_tests.cpp (uses its CHECK and
// GROUP): the file tree's sort orders, the ages it shows, and folder totals
// (the cache and its invalidation, and measuring a scratch tree on disk).
#include "FileDetails.h"

#include <sys/stat.h>

namespace {

TreeSort::Entry fdEntry(const std::string &name, bool dir, int64_t size, double modified) {
    TreeSort::Entry e;
    e.name = name;
    e.isDir = dir;
    e.size = size;
    e.modified = modified;
    return e;
}

// The names in the order TreeSort gives, joined by spaces.
std::string fdOrder(const std::vector<TreeSort::Entry> &es, const TreeSort::Options &o) {
    std::string out;
    for (size_t i : TreeSort::order(es, o)) {
        if (!out.empty()) out += ' ';
        out += es[i].name;
    }
    return out;
}

void fdWrite(const std::string &path, size_t bytes) {
    std::ofstream f(path, std::ios::binary);
    f << std::string(bytes, 'x');
}

}  // namespace

static void testFileDetails() {
    using namespace TreeSort;
    // Folders src (40 KB, oldest), docs (2 KB, newest) and a "Build" folder
    // whose capital must not change the name order; files of known sizes.
    std::vector<Entry> es = {
        fdEntry("zeta.txt", false, 10, 500),
        fdEntry("src", true, 40000, 100),
        fdEntry("Alpha.md", false, 3000, 300),
        fdEntry("docs", true, 2000, 900),
        fdEntry("beta.c", false, 3000, 300),
        fdEntry("Build", true, 7000, 300),
    };

    GROUP("tree:name");
    Options o;   // the defaults: today's order
    CHECK(o.key == Key::Name && o.ascending && o.foldersFirst);
    CHECK(fdOrder(es, o) == "Build docs src Alpha.md beta.c zeta.txt");
    o.ascending = false;
    CHECK(fdOrder(es, o) == "src docs Build zeta.txt beta.c Alpha.md");
    o.ascending = true;
    o.foldersFirst = false;
    CHECK(fdOrder(es, o) == "Alpha.md beta.c Build docs src zeta.txt");
    // The platform's rank wins over the names themselves.
    {
        std::vector<Entry> r = {fdEntry("b", false, 0, 0), fdEntry("a", false, 0, 0)};
        r[0].nameRank = 0;
        r[1].nameRank = 1;
        CHECK(fdOrder(r, Options()) == "b a");
        r[1].nameRank = -1;   // one without a rank: the names decide
        CHECK(fdOrder(r, Options()) == "a b");
    }
    CHECK(compareNames(fdEntry("a", false, 0, 0), fdEntry("B", false, 0, 0)) < 0);
    CHECK(compareNames(fdEntry("B", false, 0, 0), fdEntry("b", false, 0, 0)) < 0);
    CHECK(compareNames(fdEntry("ab", false, 0, 0), fdEntry("abc", false, 0, 0)) < 0);
    CHECK(compareNames(fdEntry("x", false, 0, 0), fdEntry("x", false, 0, 0)) == 0);

    GROUP("tree:modified");
    o = choose(Options(), Key::Modified);
    CHECK(o.key == Key::Modified && !o.ascending && o.foldersFirst);
    // Newest first; Alpha.md and beta.c tie at 300 and go by name.
    CHECK(fdOrder(es, o) == "docs Build src zeta.txt Alpha.md beta.c");
    o.ascending = false;
    o.foldersFirst = false;
    CHECK(fdOrder(es, o) == "docs zeta.txt Alpha.md beta.c Build src");
    o.ascending = true;
    CHECK(fdOrder(es, o) == "src Alpha.md beta.c Build zeta.txt docs");

    GROUP("tree:size");
    o = choose(Options(), Key::Size);
    CHECK(o.key == Key::Size && !o.ascending);
    CHECK(fdOrder(es, o) == "src Build docs Alpha.md beta.c zeta.txt");
    o.ascending = true;
    CHECK(fdOrder(es, o) == "docs Build src zeta.txt Alpha.md beta.c");
    o.foldersFirst = false;
    CHECK(fdOrder(es, o) == "zeta.txt docs Alpha.md beta.c Build src");
    // A folder's total still on its way: the folders stay in name order,
    // ahead of the files even when mixed, and the files sort at once.
    {
        std::vector<Entry> p = es;
        p[3].size = -1;   // docs
        Options d = choose(Options(), Key::Size);
        CHECK(fdOrder(p, d) == "Build docs src Alpha.md beta.c zeta.txt");
        d.foldersFirst = false;
        CHECK(fdOrder(p, d) == "Build docs src Alpha.md beta.c zeta.txt");
        d.ascending = true;
        CHECK(fdOrder(p, d) == "Build docs src zeta.txt Alpha.md beta.c");
    }
    // A file whose size could not be read counts as empty.
    {
        std::vector<Entry> p = {fdEntry("a", false, -1, 0), fdEntry("b", false, 5, 0)};
        CHECK(fdOrder(p, choose(Options(), Key::Size)) == "b a");
    }
    CHECK(fdOrder({}, o).empty());

    GROUP("tree:choose");
    o = Options();
    o.foldersFirst = false;
    Options c = choose(o, Key::Size);
    CHECK(c.key == Key::Size && !c.ascending && !c.foldersFirst);
    c.ascending = true;
    CHECK(choose(c, Key::Size) == c);   // the same key keeps its direction
    c = choose(c, Key::Name);
    CHECK(c.key == Key::Name && c.ascending);
    CHECK(defaultAscending(Key::Name) && !defaultAscending(Key::Modified) &&
          !defaultAscending(Key::Size));

    GROUP("tree:encode");
    for (Key k : {Key::Name, Key::Modified, Key::Size})
        for (bool asc : {true, false})
            for (bool ff : {true, false}) {
                Options a;
                a.key = k;
                a.ascending = asc;
                a.foldersFirst = ff;
                Options b;
                b.foldersFirst = !ff;
                CHECK(decode(encode(a), &b) && b == a);
            }
    CHECK(encode(Options()) == "name,asc,folders");
    {
        Options keep = choose(Options(), Key::Size);
        for (const char *bad : {"", "name", "name,asc", "date,asc,folders",
                                "name,up,folders", "name,asc,all", "name,asc,folders,x"})
            CHECK(!decode(bad, &keep));
        CHECK(keep == choose(Options(), Key::Size));   // untouched
    }

    GROUP("tree:age");
    const double now = 1000000;
    CHECK(Age::compact(now, now, 0) == "now");
    CHECK(Age::compact(now, now - 59, 0) == "now");
    CHECK(Age::compact(now, now + 30, 0) == "now");      // a little ahead
    CHECK(Age::compact(now, now + 600, 0).empty());     // a clock well ahead
    CHECK(Age::compact(now, now - 60, 0) == "1 min");
    CHECK(Age::compact(now, now - 59 * 60, 0) == "59 min");
    CHECK(Age::compact(now, now - 20 * 60, 1) == "20 min");   // just past midnight
    CHECK(Age::compact(now, now - 3600, 0) == "1 h");
    CHECK(Age::compact(now, now - 5 * 3600 - 10, 0) == "5 h");
    CHECK(Age::compact(now, now - 5 * 3600, 1) == "yesterday");
    CHECK(Age::compact(now, now - 40 * 3600, 2).empty());
    CHECK(Age::saved(now, now - 5, 0, "14:32", "12 Oct") == "Saved just now");
    CHECK(Age::saved(now, now - 120, 0, "14:32", "12 Oct") == "Saved 2 min ago");
    CHECK(Age::saved(now, now - 7200, 0, "14:32", "12 Oct") == "Saved 14:32");
    CHECK(Age::saved(now, now - 7200, 1, "14:32", "12 Oct") == "Saved yesterday 14:32");
    CHECK(Age::saved(now, now - 90000, 3, "14:32", "12 Oct") == "Saved 12 Oct 14:32");
    CHECK(Age::saved(now, now + 7200, 0, "14:32", "12 Oct") == "Saved 12 Oct 14:32");

    GROUP("sizes:ancestors");
    using namespace FolderSizes;
    CHECK((ancestors("/a/b/c") == std::vector<std::string>{"/a/b", "/a", "/"}));
    CHECK((ancestors("/a/b/") == std::vector<std::string>{"/a", "/"}));
    CHECK((ancestors("/a") == std::vector<std::string>{"/"}));
    CHECK(ancestors("/").empty());
    CHECK((ancestors("a/b") == std::vector<std::string>{"a"}));

    GROUP("sizes:cache");
    {
        SizeCache cache;
        Stamp s1;
        s1.dev = 1; s1.ino = 2; s1.mtimeNs = 3;
        Stamp s2 = s1;
        s2.mtimeNs = 4;
        Total t;
        t.bytes = 100;
        t.files = 2;
        Total got;
        CHECK(!cache.lookup("/r/a", s1, &got));
        CHECK(cache.store("/r/a", s1, t, cache.clock()));
        CHECK(cache.lookup("/r/a", s1, &got) && got.bytes == 100 && got.files == 2);
        CHECK(!cache.lookup("/r/a", s2, &got));   // the folder changed since
        CHECK(cache.store("/r/a/b", s1, t, cache.clock()));
        CHECK(cache.store("/r/c", s1, t, cache.clock()));
        CHECK(cache.store("/r", s1, t, cache.clock()));
        // A change in /r/a/b: it and everything above it go, /r/c stays.
        cache.invalidate("/r/a/b");
        CHECK(!cache.lookup("/r/a/b", s1, &got));
        CHECK(!cache.lookup("/r/a", s1, &got));
        CHECK(!cache.lookup("/r", s1, &got));
        CHECK(cache.lookup("/r/c", s1, &got));
        // A measure that began before the change may have seen the old disk.
        uint64_t before = cache.clock();
        cache.invalidate("/r/a");
        CHECK(!cache.store("/r/a", s1, t, before));
        CHECK(!cache.store("/r", s1, t, before));
        CHECK(cache.store("/r/c", s1, t, before));   // nothing under it changed
        CHECK(cache.store("/r/a", s1, t, cache.clock()));
        // invalidateTree takes the folders below as well.
        CHECK(cache.store("/r/a/b", s1, t, cache.clock()));
        CHECK(cache.store("/r/ab", s1, t, cache.clock()));
        cache.invalidateTree("/r/a");
        CHECK(!cache.lookup("/r/a/b", s1, &got));
        CHECK(!cache.lookup("/r/a", s1, &got));
        CHECK(cache.lookup("/r/ab", s1, &got));   // a sibling, not below
        CHECK(cache.lookup("/r/c", s1, &got));
        cache.clear();
        CHECK(cache.validCount() == 0);
    }

    GROUP("sizes:measure");
    namespace fsys = std::filesystem;
    const fsys::path root = fsys::temp_directory_path() /
        ("minicode-sizes-" + std::to_string(::getpid()));
    std::error_code ec;
    fsys::remove_all(root, ec);
    fsys::create_directories(root / "a" / "deep", ec);
    fsys::create_directories(root / "b", ec);
    fsys::create_directories(root / ".hidden", ec);
    const std::string R = root.string();
    fdWrite(R + "/top.txt", 100);
    fdWrite(R + "/a/one.bin", 1000);
    fdWrite(R + "/a/deep/two.bin", 2000);
    fdWrite(R + "/a/.dot", 7);
    fdWrite(R + "/b/three.bin", 300);
    fdWrite(R + "/.hidden/h.txt", 50);
    fsys::create_symlink(R + "/a", R + "/link-to-a", ec);          // never followed
    fsys::create_symlink(R + "/a/one.bin", R + "/link-to-one", ec);
    ::mkfifo((R + "/pipe").c_str(), 0600);
    {
        SizeCache cache;
        Limits lim;
        Total t;
        CHECK(measure(R, cache, lim, nullptr, &t));
        CHECK(t.bytes == 100 + 1000 + 2000 + 7 + 300 + 50);   // hidden counted, links not
        CHECK(t.files == 6);
        CHECK(!t.capped);
        Stamp st;
        Total sub;
        CHECK(stampOf(R + "/a", &st) && cache.lookup(R + "/a", st, &sub) && sub.bytes == 3007);
        CHECK(stampOf(R + "/a/deep", &st) && cache.lookup(R + "/a/deep", st, &sub) &&
              sub.bytes == 2000);
        CHECK(stampOf(R + "/b", &st) && cache.lookup(R + "/b", st, &sub) && sub.bytes == 300);
        CHECK(!stampOf(R + "/link-to-a", &st));   // a link to a folder is not one
        CHECK(!stampOf(R + "/top.txt", &st));
        CHECK(!measure(R + "/top.txt", cache, lim, nullptr, &t));
        CHECK(!measure(R + "/missing", cache, lim, nullptr, &t));

        // A file deep down grows. Its folder's own date does not change, so
        // the cache still answers for it until FSEvents says so...
        fdWrite(R + "/a/deep/two.bin", 2500);
        CHECK(measure(R + "/a", cache, lim, nullptr, &t) && t.bytes == 3007);
        // ...and after invalidating its folder, every total above it is
        // taken again, while /b is read from the cache.
        cache.invalidate(R + "/a/deep");
        CHECK(stampOf(R + "/b", &st) && cache.lookup(R + "/b", st, &sub));
        CHECK(measure(R, cache, lim, nullptr, &t) && t.bytes == 3957);
        CHECK(stampOf(R + "/a", &st) && cache.lookup(R + "/a", st, &sub) && sub.bytes == 3507);
        // A new file changes its folder's date, which the cache sees itself.
        fdWrite(R + "/b/four.bin", 4);
        CHECK(stampOf(R + "/b", &st) && !cache.lookup(R + "/b", st, &sub));
        CHECK(measure(R + "/b", cache, lim, nullptr, &t) && t.bytes == 304);

        // Limits: a lower bound, marked.
        SizeCache fresh;
        Limits few;
        few.maxEntries = 3;
        CHECK(measure(R, fresh, few, nullptr, &t));
        CHECK(t.capped && t.bytes < 3957 + 4);
        Stamp rs;
        CHECK(stampOf(R, &rs) && fresh.lookup(R, rs, &sub) && sub.capped);
        Limits shallow;
        shallow.maxDepth = 0;   // only the top folder's own files
        SizeCache fresh2;
        CHECK(measure(R, fresh2, shallow, nullptr, &t) && t.capped);

        // Cancelled: nothing is stored for the folder.
        std::atomic<bool> stop(true);
        SizeCache fresh3;
        CHECK(!measure(R, fresh3, lim, &stop, &t));
        CHECK(stampOf(R, &rs) && !fresh3.lookup(R, rs, &sub));
    }
    fsys::remove_all(root, ec);
}
