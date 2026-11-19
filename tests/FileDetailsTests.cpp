// FileDetailsTests.cpp — included by run_tests.cpp (uses its CHECK and
// GROUP): the file tree's sort orders, the ages it shows, and folder totals
// (the cache and its invalidation, and measuring a scratch tree on disk).
#include "FileDetails.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <ctime>

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

    GROUP("tree:icons");
    {
        using FileIcon::Kind;
        struct Case { const char *path; Kind kind; };
        const Case cases[] = {
            {"/p/shot.PNG", Kind::Image},       {"/p/a.jpeg", Kind::Image},
            {"/p/clip.mp4", Kind::Video},       {"/p/clip.MOV", Kind::Video},
            {"/p/song.mp3", Kind::Audio},       {"/p/take.flac", Kind::Audio},
            {"/p/paper.pdf", Kind::Pdf},        {"/p/data.csv", Kind::Table},
            {"/p/data.tsv", Kind::Table},       {"/p/run.ipynb", Kind::Notebook},
            {"/p/app.sqlite", Kind::Database},  {"/p/app.db", Kind::Database},
            {"/p/src.zip", Kind::Archive},      {"/p/src.tar.gz", Kind::Archive},
            {"/p/index.html", Kind::Web},       {"/p/docs.url", Kind::Bookmark},
            {"/p/site.webloc", Kind::Bookmark}, {"/p/main.cpp", Kind::Code},
            {"/p/tool.py", Kind::Code},         {"/p/conf.json", Kind::Code},
            {"/p/app.ts", Kind::Code},          {"/p/logo.svg", Kind::Code},
            {"/p/README.md", Kind::Text},       {"/p/notes.txt", Kind::Text},
            {"/p/paper.tex", Kind::Text},       {"/p/Makefile", Kind::Text},
            {"/p/strange.qqq", Kind::Text},     {"/p/noext", Kind::Text},
        };
        for (const Case &c : cases) {
            const Kind k = FileIcon::kindOf(c.path);
            if (k != c.kind)
                std::printf("    %s: %s, wanted %s\n", c.path, FileIcon::kindName(k),
                            FileIcon::kindName(c.kind));
            CHECK(k == c.kind);
        }
        // The viewers' own tables: what opens as a picture or in the player.
        CHECK(FileIcon::isImageExtension("heic") && !FileIcon::isImageExtension("svg"));
        CHECK(!FileIcon::isImageExtension("pdf"));
        CHECK(FileIcon::isVideoExtension("m4v") && !FileIcon::isVideoExtension("ts"));
        CHECK(FileIcon::isAudioExtension("opus") && !FileIcon::isAudioExtension("mp4"));
        CHECK(std::string(FileIcon::kindName(Kind::Database)) == "database");
    }

    GROUP("tree:icon-types");
    {
        struct Case { const char *path; const char *type; bool exec; };
        const Case cases[] = {
            // source
            {"/p/main.c", "c", false},              {"/p/main.h", "c-header", false},
            {"/p/main.cpp", "cpp", false},          {"/p/Main.CC", "cpp", false},
            {"/p/main.hpp", "cpp-header", false},   {"/p/App.mm", "objc", false},
            {"/p/App.cs", "csharp", false},         {"/p/tool.py", "python", false},
            {"/p/app.js", "javascript", false},     {"/p/app.mjs", "javascript", false},
            {"/p/app.ts", "typescript", false},     {"/p/app.tsx", "typescript", false},
            {"/p/lib.rs", "rust", false},           {"/p/main.go", "go", false},
            {"/p/go.mod", "go", false},             {"/p/App.java", "java", false},
            {"/p/App.kt", "kotlin", false},         {"/p/build.gradle.kts", "kotlin", false},
            {"/p/App.swift", "swift", false},       {"/p/app.rb", "ruby", false},
            {"/p/Gemfile", "ruby", false},          {"/p/index.php", "php", false},
            {"/p/tool.pl", "perl", false},          {"/p/init.lua", "lua", false},
            {"/p/run.sh", "shell", false},          {"/p/.zshrc", "shell", false},
            {"/p/.bashrc", "shell", false},         {"/p/Main.hs", "haskell", false},
            {"/p/App.scala", "scala", false},       {"/p/main.dart", "dart", false},
            {"/p/plot.R", "r", false},              {"/p/fit.jl", "julia", false},
            {"/p/app.ex", "elixir", false},         {"/p/app.erl", "erlang", false},
            {"/p/build.zig", "zig", false},         {"/p/app.nim", "nim", false},
            {"/p/app.vala", "vala", false},         {"/p/solve.f90", "fortran", false},
            {"/p/main.ml", "ocaml", false},         {"/p/core.clj", "lisp", false},
            {"/p/Main.qml", "qml", false},          {"/p/App.vue", "vue", false},
            {"/p/schema.proto", "code", false},     {"/p/SConstruct", "python", false},
            // markup and data
            {"/p/notes.md", "markdown", false},     {"/p/index.html", "html", false},
            {"/p/site.css", "css", false},          {"/p/site.scss", "sass", false},
            {"/p/site.less", "less", false},        {"/p/conf.json", "json", false},
            {"/p/ci.yml", "yaml", false},           {"/p/ci.YAML", "yaml", false},
            {"/p/Cargo.toml", "toml", false},       {"/p/Pipfile", "toml", false},
            {"/p/pom.xml", "xml", false},           {"/p/data.csv", "csv", false},
            {"/p/data.tsv", "csv", false},          {"/p/q.sql", "sql", false},
            {"/p/paper.tex", "latex", false},       {"/p/refs.bib", "latex", false},
            {"/p/run.ipynb", "notebook", false},
            // media, documents and binaries
            {"/p/shot.PNG", "image", false},        {"/p/logo.svg", "svg", false},
            {"/p/song.mp3", "audio", false},        {"/p/tune.mid", "audio", false},
            {"/p/clip.mp4", "video", false},        {"/p/clip.mkv", "video", false},
            {"/p/paper.pdf", "pdf", false},         {"/p/src.tar.gz", "archive", false},
            {"/p/src.zip", "archive", false},       {"/p/app.jar", "archive", false},
            {"/p/Inter.ttf", "font", false},        {"/p/Inter.woff2", "font", false},
            {"/p/setup.exe", "executable", false},  {"/p/libz.so", "library", false},
            {"/p/main.o", "library", false},        {"/p/app.sqlite", "database", false},
            {"/p/memo.docx", "document", false},    {"/p/sheet.xlsx", "spreadsheet", false},
            {"/p/deck.pptx", "presentation", false},{"/p/book.epub", "ebook", false},
            // by name, and the rest
            {"/p/Makefile", "makefile", false},     {"/p/GNUmakefile", "makefile", false},
            {"/p/rules.mk", "makefile", false},     {"/p/justfile", "makefile", false},
            {"/p/CMakeLists.txt", "cmake", false},  {"/p/meson.build", "meson", false},
            {"/p/Dockerfile", "dockerfile", false}, {"/p/Dockerfile.dev", "dockerfile", false},
            {"/p/app.dockerfile", "dockerfile", false},
            {"/p/docker-compose.yml", "dockerfile", false},
            {"/p/docker-compose.override.yaml", "dockerfile", false},
            {"/p/LICENSE", "license", false},       {"/p/LICENSE.md", "license", false},
            {"/p/LICENSE-MIT", "license", false},   {"/p/COPYING", "license", false},
            {"/p/license_check.py", "python", false},
            {"/p/README", "readme", false},         {"/p/README.md", "readme", false},
            {"/p/readme.txt", "readme", false},     {"/p/readme.png", "image", false},
            {"/p/AUTHORS", "authors", false},
            {"/p/package-lock.json", "lockfile", false},
            {"/p/yarn.lock", "lockfile", false},    {"/p/Cargo.lock", "lockfile", false},
            {"/p/go.sum", "lockfile", false},       {"/p/pnpm-lock.yaml", "lockfile", false},
            {"/p/whatever.lock", "lockfile", false},
            {"/p/.gitignore", "git", false},        {"/p/.gitattributes", "git", false},
            {"/p/.gitmodules", "git", false},       {"/p/.mailmap", "git", false},
            {"/p/.env", "config", false},           {"/p/.env.local", "config", false},
            {"/p/.editorconfig", "config", false},  {"/p/.prettierrc", "config", false},
            {"/p/setup.cfg", "config", false},      {"/p/app.ini", "config", false},
            {"/p/nginx.conf", "config", false},     {"/p/fix.patch", "patch", false},
            {"/p/fix.diff", "patch", false},        {"/p/build.log", "log", false},
            {"/p/id_ed25519.pub", "key", false},    {"/p/cert.pem", "key", false},
            {"/p/docs.url", "bookmark", false},     {"/p/fr.po", "translation", false},
            {"/p/notes.txt", "text", false},        {"/p/strange.qqq", "text", false},
            {"/p/noext", "text", false},
            // A file with no extension that can run; with one, its extension wins.
            {"/p/mutible-guitest", "executable", true},
            {"/p/run.sh", "shell", true},           {"/p/Makefile", "makefile", true},
            {"/p/.envrc", "shell", true},
        };
        for (const Case &c : cases) {
            const char *t = FileIcon::typeName(FileIcon::typeOf(c.path, c.exec));
            if (std::string(t) != c.type)
                std::printf("    %s%s: %s, wanted %s\n", c.path, c.exec ? " (x)" : "", t, c.type);
            CHECK(std::string(t) == c.type);
        }
        // Every type has a name of its own, and allTypes lists each once.
        std::set<std::string> names;
        for (FileIcon::Type t : FileIcon::allTypes()) names.insert(FileIcon::typeName(t));
        CHECK(names.size() == FileIcon::allTypes().size());
        CHECK(FileIcon::allTypes().front() == FileIcon::Type::Text);
        CHECK(FileIcon::allTypes().back() == FileIcon::Type::Translation);
    }

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
          !defaultAscending(Key::Size) && !defaultAscending(Key::Created));

    GROUP("tree:created");
    {
        // Birth times differ from the modification times above: a file made
        // long ago and saved just now sorts by when it was made.
        std::vector<Entry> b = es;
        const double born[] = {50, 400, 20, 10, 600, 20};
        for (size_t i = 0; i < b.size(); i++) b[i].created = born[i];
        o = choose(Options(), Key::Created);
        CHECK(o.key == Key::Created && !o.ascending && o.foldersFirst);
        CHECK(fdOrder(b, o) == "src Build docs beta.c zeta.txt Alpha.md");
        o.ascending = true;
        CHECK(fdOrder(b, o) == "docs Build src Alpha.md zeta.txt beta.c");
        o.foldersFirst = false;
        CHECK(fdOrder(b, o) == "docs Alpha.md Build zeta.txt src beta.c");
        CHECK(choose(o, Key::Created) == o);
        CHECK(choose(o, Key::Modified).key == Key::Modified);
        // Unknown birth times (0) sort as the oldest.
        std::vector<Entry> u = {fdEntry("a", false, 0, 0), fdEntry("b", false, 0, 0)};
        u[1].created = 5;
        CHECK(fdOrder(u, choose(Options(), Key::Created)) == "b a");
    }

    GROUP("tree:encode");
    for (Key k : {Key::Name, Key::Modified, Key::Size, Key::Created})
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
    CHECK(encode(choose(Options(), Key::Created)) == "created,desc,folders");
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
        ("mutible-sizes-" + std::to_string(::getpid()));
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
        // The folder is dated a minute back first: on a kernel with coarse
        // timestamps (Launchpad's builders), a write a few milliseconds
        // after the folder's last change leaves its date as it was, and the
        // 1.5.13 package failed here.
        {
            Stamp before;
            CHECK(stampOf(R + "/b", &before));
            struct timespec back[2];
            back[0].tv_sec = back[1].tv_sec = ::time(nullptr) - 60;
            back[0].tv_nsec = back[1].tv_nsec = 0;
            CHECK(::utimensat(AT_FDCWD, (R + "/b").c_str(), back, 0) == 0);
            Total again;
            CHECK(measure(R + "/b", cache, lim, nullptr, &again) && again.bytes == 300);
        }
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

    GROUP("tree:focus");
    {
        using namespace TreeFocus;
        CHECK(normalize("mac/src") == "mac/src");
        CHECK(normalize("/mac//src/") == "mac/src");
        CHECK(normalize("./mac/./src") == "mac/src");
        CHECK(normalize("") == "" && normalize(".") == "" && normalize("/") == "");
        CHECK(normalize("mac/../etc") == "" && normalize("..") == "");
        CHECK(normalize("a..b/.c") == "a..b/.c");   // dots inside a name are a name

        std::string rel = "x";
        CHECK(relative("/r/proj", "/r/proj/mac/src", &rel) && rel == "mac/src");
        CHECK(relative("/r/proj/", "/r/proj/mac", &rel) && rel == "mac");
        CHECK(relative("/r/proj", "/r/proj", &rel) && rel.empty());
        CHECK(!relative("/r/proj", "/r/project/mac", &rel));
        CHECK(!relative("/r/proj", "/r", &rel) && !relative("", "/r", &rel));
        CHECK(relative("/", "/etc/x", &rel) && rel == "etc/x");

        CHECK(contains("", "anything/at/all") && contains("", ""));
        CHECK(contains("mac", "mac") && contains("mac", "mac/src/a.mm"));
        CHECK(!contains("mac", "macos/a") && !contains("mac/src", "mac") && !contains("mac", ""));

        CHECK(parent("mac/src/a.mm") == "mac/src" && parent("a.txt") == "" && parent("") == "");

        std::vector<Part> parts = breadcrumb("mutible", "mac/src");
        CHECK(parts.size() == 3);
        CHECK(parts[0].label == "mutible" && parts[0].focus.empty());
        CHECK(parts[1].label == "mac" && parts[1].focus == "mac");
        CHECK(parts[2].label == "src" && parts[2].focus == "mac/src");
        CHECK(breadcrumb("mutible", "").size() == 1);
        CHECK(breadcrumb("m", "/a//b/").size() == 3 && breadcrumb("m", "/a//b/")[2].focus == "a/b");

        // Inside the focus: no change. Outside: the nearest folder above both.
        CHECK(widened("mac/src", "mac/src/deep/a.mm") == "mac/src");
        CHECK(widened("mac/src", "mac/tests/gui/X.mm") == "mac");
        CHECK(widened("mac/src", "src/Json.cpp") == "");
        CHECK(widened("mac/src", "README.md") == "");
        CHECK(widened("mac/src", "mac") == "");            // a row of the whole folder
        CHECK(widened("mac/src", "mac/src") == "mac");     // the focus itself is no row
        CHECK(widened("mac/src", "mac/srcx/a") == "mac");  // a name, not a prefix
        CHECK(widened("", "a/b/c") == "");

        auto folders = [](const std::string &f) { return f == "mac" || f == "mac/src"; };
        CHECK(restored("mac/src", folders) == "mac/src");
        CHECK(restored("/mac/src/", folders) == "mac/src");
        CHECK(restored("mac/gone", folders).empty());    // gone: the whole folder
        CHECK(restored("../mac", folders).empty() && restored("", folders).empty());
        CHECK(restored("mac", nullptr).empty());

        // 3 parts of 40, separators of 10, an ellipsis of 12.
        const std::vector<double> w{40, 40, 40};
        CHECK((fit(w, 10, 12, 140) == std::vector<int>{0, 1, 2}));
        CHECK((fit(w, 10, 12, 139) == std::vector<int>{0, -1, 2}));   // 40+10+12+10+40 = 112
        CHECK((fit(w, 10, 12, 100) == std::vector<int>{0, -1, 2}));   // the last is cut short
        const std::vector<double> five{60, 20, 20, 20, 30};
        CHECK((fit(five, 5, 10, 200) == std::vector<int>{0, 1, 2, 3, 4}));   // 170
        CHECK((fit(five, 5, 10, 160) == std::vector<int>{0, -1, 2, 3, 4}));  // 60+5+10+15+70 = 160
        CHECK((fit(five, 5, 10, 135) == std::vector<int>{0, -1, 3, 4}));     // 75+25+35 = 135
        CHECK((fit(five, 5, 10, 130) == std::vector<int>{0, -1, 4}));        // 75+35 = 110
        CHECK((fit({50, 50}, 10, 12, 60) == std::vector<int>{0, 1}));
        CHECK(fit({}, 10, 12, 60).empty());
    }

    GROUP("tree:focus-set");
    {
        using namespace TreeFocus;
        using S = Set;
        // Added in order; a folder already there, or inside one, changes nothing.
        CHECK((added({}, "mac/src") == S{"mac/src"}));
        CHECK((added({"mac/src"}, "linux/src") == S{"mac/src", "linux/src"}));
        CHECK((added({"mac/src", "linux/src"}, "/linux/src/") == S{"mac/src", "linux/src"}));
        CHECK((added({"mac/src", "linux/src"}, "mac/src/deep") == S{"mac/src", "linux/src"}));
        // One that holds some takes the first one's place; the others go.
        CHECK((added({"mac/src", "linux/src", "mac/tests"}, "mac") == S{"mac", "linux/src"}));
        CHECK((added({"docs", "mac/src"}, "mac") == S{"docs", "mac"}));
        // A name, not a prefix.
        CHECK((added({"mac/src"}, "mac/srcx") == S{"mac/src", "mac/srcx"}));
        CHECK((added({"mac"}, "macos") == S{"mac", "macos"}));
        // The whole folder, or one leaving it, is no folder: the empty set.
        CHECK(added({"mac/src", "linux/src"}, "").empty());
        CHECK(added({"mac/src"}, ".").empty() && added({"mac/src"}, "../x").empty());

        CHECK((removed({"mac/src", "linux/src"}, "mac/src") == S{"linux/src"}));
        CHECK((removed({"mac/src", "linux/src"}, "/linux/src") == S{"mac/src"}));
        CHECK((removed({"mac/src"}, "docs") == S{"mac/src"}));
        CHECK(removed({"mac/src"}, "mac/src").empty());

        CHECK((merged({"mac/src", "", "mac", "linux/src", "../up", "linux/src/x", "/docs/"}) ==
               S{"mac", "linux/src", "docs"}));
        CHECK(merged({}).empty() && merged({"", "."}).empty());

        CHECK(rootOf({"mac/src", "linux/src"}, "linux/src/a.c") == 1);
        CHECK(rootOf({"mac/src", "linux/src"}, "linux/src") == 1);   // a root is a row
        CHECK(rootOf({"mac/src", "linux/src"}, "mac") == -1);
        CHECK(rootOf({"mac/src", "linux/src"}, "mac/srcx/a") == -1);
        CHECK(rootOf({}, "a") == -1);

        // Reveals: none, nothing; one, #144's widening; several, the
        // file's folder added.
        CHECK(revealed({}, "src/a.cpp").empty());
        CHECK((revealed({"mac/src"}, "mac/src/a.mm") == S{"mac/src"}));
        CHECK((revealed({"mac/src"}, "mac/tests/t.mm") == S{"mac"}));
        CHECK(revealed({"mac/src"}, "src/core.cpp").empty());
        CHECK((revealed({"mac/src", "linux/src"}, "linux/src/deep/a.c") == S{"mac/src", "linux/src"}));
        CHECK((revealed({"mac/src", "linux/src"}, "linux/src") == S{"mac/src", "linux/src"}));
        CHECK((revealed({"mac/src", "linux/src"}, "src/core.cpp") == S{"mac/src", "linux/src", "src"}));
        CHECK((revealed({"mac/src", "linux/src"}, "mac/tests/t.mm") ==
               S{"mac/src", "linux/src", "mac/tests"}));
        CHECK((revealed({"mac/src/deep", "linux/src"}, "mac/src/a.mm") == S{"mac/src", "linux/src"}));
        CHECK(revealed({"mac/src", "linux/src"}, "README.md").empty());   // the top: the whole folder

        auto folders = [](const std::string &f) {
            return f == "mac" || f == "mac/src" || f == "linux/src" || f == "docs";
        };
        CHECK((restoredSet({"mac/src", "gone", "linux/src"}, folders) == S{"mac/src", "linux/src"}));
        CHECK((restoredSet({"/mac/src/", "mac"}, folders) == S{"mac"}));
        CHECK(restoredSet({"gone", "../mac"}, folders).empty());
        CHECK(restoredSet({"mac"}, nullptr).empty());

        CHECK((shortLabels({"mac/src", "linux/src"}) == std::vector<std::string>{"mac/src", "linux/src"}));
        CHECK((shortLabels({"mac/src/deep", "linux/tests"}) == std::vector<std::string>{"deep", "tests"}));
        CHECK((shortLabels({"src", "mac/src"}) == std::vector<std::string>{"src", "mac/src"}));
        CHECK((shortLabels({"a/x/src", "b/x/src", "docs"}) ==
               std::vector<std::string>{"a/x/src", "b/x/src", "docs"}));
        CHECK(shortLabels({}).empty());

        CHECK((shared({40, 50}, 100) == std::vector<double>{40, 50}));
        CHECK((shared({20, 100, 100}, 120) == std::vector<double>{20, 50, 50}));
        CHECK((shared({60, 60}, 100) == std::vector<double>{50, 50}));
        CHECK((shared({10, 30, 90}, 70) == std::vector<double>{10, 30, 30}));
        CHECK((shared({10, 10}, 0) == std::vector<double>{0, 0}));
        CHECK(shared({}, 10).empty());
    }

    GROUP("tree:auto-reveal");
    {
        using TreeFocus::shows;
        // No focus: every file of the folder shows.
        CHECK(shows({}, "mac/src/a.mm") && shows({}, "README.md"));
        // One focused folder is the root, no row: inside it shows, the
        // folder itself and anything outside leave the tree as it is.
        CHECK(shows({"mac/src"}, "mac/src/a.mm") && shows({"mac/src"}, "mac/src/deep/d.txt"));
        CHECK(!shows({"mac/src"}, "mac/src") && !shows({"mac/src"}, "src/core.cpp"));
        CHECK(!shows({"mac/src"}, "mac/srcx/a"));   // a name, not a prefix
        CHECK(!shows({"mac/src"}, "README.md"));
        // Several: each is a root row, so the folder itself shows too.
        CHECK(shows({"mac/src", "linux/src"}, "linux/src/x.cpp"));
        CHECK(shows({"mac/src", "linux/src"}, "mac/src"));
        CHECK(!shows({"mac/src", "linux/src"}, "src/core.cpp"));

        using TreeReveal::scrollTop;
        // Rows 20 tall, a view 200 tall at 400 over 2000 of content.
        CHECK(scrollTop(400, 20, 400, 200, 2000) == 400);   // the first row in view: no move
        CHECK(scrollTop(580, 20, 400, 200, 2000) == 400);   // the last whole row: no move
        CHECK(scrollTop(590, 20, 400, 200, 2000) == 500);   // cut by the bottom: centered
        CHECK(scrollTop(390, 20, 400, 200, 2000) == 300);   // cut by the top: centered
        CHECK(scrollTop(1500, 20, 400, 200, 2000) == 1410);
        CHECK(scrollTop(20, 20, 400, 200, 2000) == 0);      // near the start: held at 0
        CHECK(scrollTop(1980, 20, 0, 200, 2000) == 1800);   // near the end: held at the end
        CHECK(scrollTop(100, 20, 0, 200, 150) == 0);        // content shorter than the view
        CHECK(scrollTop(10, 20, 0, 0, 2000) == 0);          // no view at all

        TreeReveal::Follower f;
        CHECK(f.activeChanged("/r/a.cpp", true));            // on show: revealed now
        CHECK(f.treeShown().empty());                        // nothing waits
        CHECK(!f.activeChanged("", true));                   // a page: nothing to reveal
        CHECK(!f.activeChanged("/r/b.cpp", false));          // hidden: kept
        CHECK(!f.activeChanged("/r/c.cpp", false));          // the last one wins
        CHECK(f.treeShown() == "/r/c.cpp");
        CHECK(f.treeShown().empty());                        // once
        CHECK(!f.activeChanged("/r/d.cpp", false));
        CHECK(!f.activeChanged("", false));                  // then a page: nothing to reveal
        CHECK(f.treeShown().empty());
        CHECK(!f.activeChanged("/r/e.cpp", false));
        CHECK(f.activeChanged("/r/f.cpp", true));            // on show again first: nothing waits
        CHECK(f.treeShown().empty());
    }
}
