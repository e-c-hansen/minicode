// FileDetails.cpp — see FileDetails.h.
#include "FileDetails.h"

#include "FileSniff.h"
#include "SyntaxHighlighter.h"

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <unordered_map>

#include <dirent.h>
#include <sys/stat.h>

namespace TreeSort {

bool defaultAscending(Key key) { return key == Key::Name; }

Options choose(const Options &current, Key key) {
    if (current.key == key) return current;
    Options o = current;
    o.key = key;
    o.ascending = defaultAscending(key);
    return o;
}

std::string encode(const Options &o) {
    const char *key = o.key == Key::Name       ? "name"
                    : o.key == Key::Modified ? "modified"
                    : o.key == Key::Created  ? "created"
                                             : "size";
    return std::string(key) + (o.ascending ? ",asc" : ",desc") +
           (o.foldersFirst ? ",folders" : ",mixed");
}

bool decode(const std::string &s, Options *out) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        size_t comma = s.find(',', start);
        parts.push_back(s.substr(start, comma == std::string::npos ? std::string::npos
                                                                   : comma - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (parts.size() != 3) return false;
    Options o;
    if (parts[0] == "name") o.key = Key::Name;
    else if (parts[0] == "modified") o.key = Key::Modified;
    else if (parts[0] == "size") o.key = Key::Size;
    else if (parts[0] == "created") o.key = Key::Created;
    else return false;
    if (parts[1] == "asc") o.ascending = true;
    else if (parts[1] == "desc") o.ascending = false;
    else return false;
    if (parts[2] == "folders") o.foldersFirst = true;
    else if (parts[2] == "mixed") o.foldersFirst = false;
    else return false;
    *out = o;
    return true;
}

static unsigned char lowerAscii(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
}

int compareNames(const Entry &a, const Entry &b) {
    if (a.nameRank >= 0 && b.nameRank >= 0)
        return a.nameRank < b.nameRank ? -1 : a.nameRank > b.nameRank ? 1 : 0;
    const std::string &x = a.name, &y = b.name;
    const size_t n = std::min(x.size(), y.size());
    for (size_t i = 0; i < n; i++) {
        unsigned char p = lowerAscii((unsigned char)x[i]), q = lowerAscii((unsigned char)y[i]);
        if (p != q) return p < q ? -1 : 1;
    }
    if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
    return x < y ? -1 : x > y ? 1 : 0;
}

std::vector<size_t> order(const std::vector<Entry> &entries, const Options &o) {
    std::vector<size_t> idx(entries.size());
    std::iota(idx.begin(), idx.end(), 0);
    bool pendingDirs = false;
    if (o.key == Key::Size)
        for (const Entry &e : entries)
            if (e.isDir && e.size < 0) { pendingDirs = true; break; }
    const bool groupDirs = o.foldersFirst || pendingDirs;
    std::stable_sort(idx.begin(), idx.end(), [&](size_t i, size_t j) {
        const Entry &a = entries[i], &b = entries[j];
        if (groupDirs && a.isDir != b.isDir) return a.isDir;
        const int byName = compareNames(a, b);
        switch (o.key) {
        case Key::Name:
            if (byName != 0) return o.ascending ? byName < 0 : byName > 0;
            return false;
        case Key::Modified:
            if (a.modified != b.modified)
                return o.ascending ? a.modified < b.modified : a.modified > b.modified;
            return byName < 0;
        case Key::Created:
            if (a.created != b.created)
                return o.ascending ? a.created < b.created : a.created > b.created;
            return byName < 0;
        case Key::Size: {
            if (pendingDirs && a.isDir && b.isDir) return byName < 0;
            const int64_t sa = std::max<int64_t>(a.size, 0), sb = std::max<int64_t>(b.size, 0);
            if (sa != sb) return o.ascending ? sa < sb : sa > sb;
            return byName < 0;
        }
        }
        return false;
    });
    return idx;
}

}  // namespace TreeSort

namespace Age {

std::string compact(double now, double then, int daysAgo) {
    const double d = now - then;
    if (d < -60) return "";
    if (d < 60) return "now";
    if (d < 3600) return std::to_string((long)(d / 60)) + " min";
    if (daysAgo == 0) return std::to_string((long)(d / 3600)) + " h";
    if (daysAgo == 1) return "yesterday";
    return "";
}

std::string saved(double now, double then, int daysAgo, const std::string &time,
                  const std::string &date) {
    const double d = now - then;
    if (d >= -60 && d < 60) return "Saved just now";
    if (d >= 60 && d < 3600) return "Saved " + std::to_string((long)(d / 60)) + " min ago";
    if (d >= 0 && daysAgo == 0) return "Saved " + time;
    if (d >= 0 && daysAgo == 1) return "Saved yesterday " + time;
    return "Saved " + date + " " + time;
}

}  // namespace Age

namespace FolderSizes {

std::vector<std::string> ancestors(const std::string &path) {
    std::vector<std::string> out;
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    for (;;) {
        size_t slash = p.rfind('/');
        if (slash == std::string::npos) break;
        if (slash == 0) {
            if (p != "/") out.push_back("/");
            break;
        }
        p = p.substr(0, slash);
        out.push_back(p);
    }
    return out;
}

bool SizeCache::lookup(const std::string &path, const Stamp &stamp, Total *out) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = recs_.find(path);
    if (it == recs_.end() || !it->second.valid || it->second.stamp != stamp) return false;
    if (out) *out = it->second.total;
    return true;
}

bool SizeCache::store(const std::string &path, const Stamp &stamp, const Total &total,
                      uint64_t since) {
    std::lock_guard<std::mutex> lock(mu_);
    Rec &r = recs_[path];
    if (r.invalidatedAt > since) return false;
    r.stamp = stamp;
    r.total = total;
    r.valid = true;
    return true;
}

uint64_t SizeCache::clock() const {
    std::lock_guard<std::mutex> lock(mu_);
    return clock_;
}

void SizeCache::markLocked(const std::string &path) {
    Rec &r = recs_[path];
    r.valid = false;
    r.invalidatedAt = clock_;
}

void SizeCache::invalidate(const std::string &path) {
    std::lock_guard<std::mutex> lock(mu_);
    ++clock_;
    markLocked(path);
    for (const std::string &a : ancestors(path)) markLocked(a);
}

void SizeCache::invalidateTree(const std::string &path) {
    std::lock_guard<std::mutex> lock(mu_);
    ++clock_;
    markLocked(path);
    for (const std::string &a : ancestors(path)) markLocked(a);
    const std::string prefix = path == "/" ? "/" : path + "/";
    for (auto it = recs_.lower_bound(prefix);
         it != recs_.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
        it->second.valid = false;
        it->second.invalidatedAt = clock_;
    }
}

void SizeCache::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    ++clock_;
    // Kept as invalid records, so a measure that began before the clear
    // cannot store what it found.
    for (auto &kv : recs_) {
        kv.second.valid = false;
        kv.second.invalidatedAt = clock_;
    }
}

size_t SizeCache::validCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    size_t n = 0;
    for (const auto &kv : recs_) n += kv.second.valid ? 1 : 0;
    return n;
}

static int64_t mtimeNs(const struct stat &st) {
#ifdef __APPLE__
    return (int64_t)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    return (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
}

static Stamp stampFrom(const struct stat &st) {
    Stamp s;
    s.dev = (uint64_t)st.st_dev;
    s.ino = (uint64_t)st.st_ino;
    s.mtimeNs = mtimeNs(st);
    return s;
}

bool stampOf(const std::string &path, Stamp *out) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    if (out) *out = stampFrom(st);
    return true;
}

namespace {

enum class Status { Done, Capped, Cancelled };

struct Walk {
    SizeCache &cache;
    const Limits &limits;
    const std::atomic<bool> *cancel;
    uint64_t since = 0;
    uint64_t dev = 0;
    int64_t entries = 0;
    std::chrono::steady_clock::time_point deadline;

    Walk(SizeCache &c, const Limits &l, const std::atomic<bool> *stop)
        : cache(c), limits(l), cancel(stop) {}

    Status walk(const std::string &dir, int depth, Total &t) {
        if (depth > limits.maxDepth) return Status::Capped;
        DIR *d = opendir(dir.c_str());
        if (!d) return Status::Done;   // unreadable: nothing we can count
        Status result = Status::Done;
        while (struct dirent *de = readdir(d)) {
            const char *name = de->d_name;
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                result = Status::Cancelled;
                break;
            }
            if (++entries > limits.maxEntries ||
                ((entries & 63) == 0 && std::chrono::steady_clock::now() > deadline)) {
                result = Status::Capped;
                break;
            }
            std::string child = dir == "/" ? "/" + std::string(name) : dir + "/" + name;
            struct stat st;
            if (lstat(child.c_str(), &st) != 0) continue;
            if (S_ISLNK(st.st_mode)) continue;
            if (S_ISREG(st.st_mode)) {
                t.bytes += (int64_t)st.st_size;
                t.files++;
                continue;
            }
            if (!S_ISDIR(st.st_mode) || (uint64_t)st.st_dev != dev) continue;
            const Stamp cs = stampFrom(st);
            Total sub;
            if (cache.lookup(child, cs, &sub)) {
                t.bytes += sub.bytes;
                t.files += sub.files;
                if (sub.capped) t.capped = true;
                continue;
            }
            Status s = walk(child, depth + 1, sub);
            t.bytes += sub.bytes;
            t.files += sub.files;
            if (s != Status::Done) {
                t.capped = true;
                result = s;
                break;
            }
            if (sub.capped) t.capped = true;
            cache.store(child, cs, sub, since);
        }
        closedir(d);
        return result;
    }
};

}  // namespace

bool measure(const std::string &path, SizeCache &cache, const Limits &limits,
             const std::atomic<bool> *cancel, Total *out) {
    Stamp stamp;
    if (!stampOf(path, &stamp)) return false;
    Walk w(cache, limits, cancel);
    w.since = cache.clock();
    w.dev = stamp.dev;
    w.deadline = std::chrono::steady_clock::now() +
                 std::chrono::microseconds((int64_t)(limits.maxSeconds * 1e6));
    Total t;
    Status s = w.walk(path, 0, t);
    if (s == Status::Cancelled) return false;
    if (s == Status::Capped) t.capped = true;
    cache.store(path, stamp, t, w.since);
    if (out) *out = t;
    return true;
}

}  // namespace FolderSizes

namespace TreeRefresh {

namespace {
std::string withoutTrailingSlashes(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}
// Whether `path` is `dir` or inside it.
bool within(const std::string &path, const std::string &dir) {
    if (path == dir) return true;
    if (dir == "/") return !path.empty() && path[0] == '/';
    return path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 &&
           path[dir.size()] == '/';
}
std::string parentOf(const std::string &path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) return "";
    return slash == 0 ? "/" : path.substr(0, slash);
}
}  // namespace

std::string treeSpelling(const std::string &path, const std::string &root,
                         const std::string &realRoot) {
    std::string p = withoutTrailingSlashes(path);
    const std::string real = withoutTrailingSlashes(realRoot);
    const std::string tree = withoutTrailingSlashes(root);
    if (real.empty() || tree.empty() || real == tree || !within(p, real)) return p;
    return tree + p.substr(real.size());
}

Plan plan(const std::vector<Event> &events, const std::set<std::string> &loaded,
          const std::string &rootPath) {
    Plan out;
    const std::string root = withoutTrailingSlashes(rootPath);
    std::set<std::string> list, restat;
    for (const Event &e : events) {
        const std::string p = withoutTrailingSlashes(e.path);
        if (p.empty()) continue;
        // Events dropped or a root moved: a folder above the root, with
        // everything below it to look at.
        if (e.subtree && within(root, p)) {
            out.everything = true;
            return out;
        }
        if (!within(p, root)) continue;
        if (e.subtree) {
            for (auto it = loaded.lower_bound(p); it != loaded.end() && it->compare(0, p.size(), p) == 0; ++it)
                if (within(*it, p)) list.insert(*it);
        } else if (loaded.count(p)) {
            list.insert(p);
        }
        if (p != root && loaded.count(parentOf(p))) restat.insert(p);
    }
    for (const std::string &r : restat)
        if (!list.count(parentOf(r))) out.restat.push_back(r);
    out.list.assign(list.begin(), list.end());
    return out;
}

bool rowBefore(const Row &a, const Row &b) {
    if (a.folder != b.folder) return a.folder;
    if (int c = a.key.compare(b.key)) return c < 0;
    return a.name < b.name;
}

std::vector<Splice> splices(const std::vector<Row> &now, const std::vector<Row> &fresh) {
    std::vector<Splice> out;
    Splice run;
    bool open = false;
    auto close = [&] {
        if (open) out.push_back(run);
        open = false;
    };
    auto begin = [&](std::size_t at, std::size_t from) {
        if (open) return;
        run = Splice{at, 0, from, 0};
        open = true;
    };
    std::size_t i = 0, j = 0;
    while (i < now.size() || j < fresh.size()) {
        if (i < now.size() && j < fresh.size() && now[i].folder == fresh[j].folder &&
            now[i].name == fresh[j].name) {
            close();   // the same entry: it stays
            ++i;
            ++j;
        } else if (j == fresh.size() || (i < now.size() && rowBefore(now[i], fresh[j]))) {
            begin(i, j);   // gone
            ++run.removed;
            ++i;
        } else {
            begin(i, j);   // new
            ++run.added;
            ++j;
        }
    }
    close();
    std::reverse(out.begin(), out.end());
    return out;
}

double Pacer::wait(double now) const {
    const double sinceLast = started_ ? lastStart_ + minInterval_ - now : 0;
    return std::max(firstDelay_, sinceLast);
}

double Pacer::changed(double now) {
    switch (state_) {
    case State::Idle:
        state_ = State::Waiting;
        return wait(now);
    case State::Listing:
        state_ = State::ListingChanged;
        return -1;
    case State::Waiting:
    case State::ListingChanged:
        return -1;
    }
    return -1;
}

void Pacer::started(double now) {
    state_ = State::Listing;
    lastStart_ = now;
    started_ = true;
}

double Pacer::finished(double now) {
    if (state_ == State::ListingChanged) {
        state_ = State::Waiting;
        return wait(now);
    }
    state_ = State::Idle;
    return -1;
}

}  // namespace TreeRefresh

namespace FileIcon {

namespace {
bool among(const std::string &ext, std::initializer_list<const char *> list) {
    for (const char *e : list)
        if (ext == e) return true;
    return false;
}
}  // namespace

bool isImageExtension(const std::string &ext) {
    return among(ext, {"png", "jpg", "jpeg", "gif", "tif", "tiff", "bmp", "heic", "heif", "webp",
                       "ico", "icns"});
}

bool isVideoExtension(const std::string &ext) {
    return among(ext, {"mp4", "m4v", "mov", "3gp", "avi"});
}

bool isAudioExtension(const std::string &ext) {
    return among(ext, {"mp3", "wav", "m4a", "aac", "aif", "aiff", "flac", "ogg", "opus"});
}

Kind kindOf(const std::string &path) {
    const std::string ext = FileSniff::extensionOf(path);
    if (ext.empty()) return Kind::Text;
    if (isImageExtension(ext)) return Kind::Image;
    if (isVideoExtension(ext)) return Kind::Video;
    if (isAudioExtension(ext)) return Kind::Audio;
    if (ext == "pdf") return Kind::Pdf;
    if (ext == "csv" || ext == "tsv") return Kind::Table;
    if (ext == "ipynb") return Kind::Notebook;
    if (among(ext, {"sqlite", "sqlite3", "db", "db3", "s3db", "sl3"})) return Kind::Database;
    if (among(ext, {"zip", "tar", "gz", "tgz", "bz2", "tbz", "tbz2", "xz", "txz", "zst", "7z",
                    "rar", "jar", "dmg"}))
        return Kind::Archive;
    if (among(ext, {"html", "htm", "xhtml"})) return Kind::Web;
    if (ext == "url" || ext == "webloc") return Kind::Bookmark;
    // Markdown and LaTeX open as pages, and plain text is text.
    if (among(ext, {"md", "markdown", "txt", "text", "rst", "tex", "ltx", "latex", "log"}))
        return Kind::Text;
    if (SyntaxHighlighter::supports(ext)) return Kind::Code;
    return Kind::Text;
}

namespace {

std::string lowered(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

// `name` is `stem`, or `stem` followed by '.', '-' or '_' and more
// (LICENSE.md, LICENSE-MIT, README_ja).
bool stemIs(const std::string &name, const char *stem) {
    const std::size_t n = std::strlen(stem);
    if (name.compare(0, n, stem) != 0) return false;
    return name.size() == n || name[n] == '.' || name[n] == '-' || name[n] == '_';
}

const std::unordered_map<std::string, Type> &typesByName() {
    static const std::unordered_map<std::string, Type> m = [] {
        std::unordered_map<std::string, Type> t;
        auto add = [&t](Type type, std::initializer_list<const char *> names) {
            for (const char *n : names) t[n] = type;
        };
        add(Type::Makefile, {"makefile", "gnumakefile", "bsdmakefile", "makefile.am",
                             "makefile.in", "justfile", ".justfile", "kbuild"});
        add(Type::CMake, {"cmakelists.txt", "cmakecache.txt"});
        add(Type::Meson, {"meson.build", "meson_options.txt", "meson.options"});
        add(Type::Dockerfile, {"dockerfile", "containerfile", ".dockerignore",
                               "docker-compose.yml", "docker-compose.yaml", "compose.yml",
                               "compose.yaml"});
        add(Type::Lockfile, {"package-lock.json", "npm-shrinkwrap.json", "pnpm-lock.yaml",
                             "go.sum", "packages.lock.json", "gemfile.lock", "cargo.lock",
                             "poetry.lock", "pipfile.lock", "composer.lock", "flake.lock",
                             "uv.lock", "yarn.lock", "bun.lockb", "bun.lock", "deno.lock",
                             "mix.lock", "podfile.lock", "pubspec.lock", "package.resolved"});
        add(Type::Git, {".gitignore", ".gitattributes", ".gitmodules", ".gitconfig",
                        ".gitkeep", ".gitmessage", ".git-blame-ignore-revs", ".mailmap",
                        ".git"});
        add(Type::Ruby, {"gemfile", "rakefile", "podfile", "vagrantfile", "brewfile",
                         "guardfile", "fastfile"});
        add(Type::Python, {"sconstruct", "sconscript", "wscript"});
        add(Type::Toml, {"pipfile"});
        add(Type::Go, {"go.mod", "go.work"});
        add(Type::Shell, {".bashrc", ".bash_profile", ".bash_login", ".bash_logout",
                          ".bash_aliases", ".profile", ".zshrc", ".zshenv", ".zprofile",
                          ".zlogin", ".zlogout", ".kshrc", ".cshrc", ".tcshrc", ".envrc",
                          "pkgbuild"});
        return t;
    }();
    return m;
}

const std::unordered_map<std::string, Type> &typesByExtension() {
    static const std::unordered_map<std::string, Type> m = [] {
        std::unordered_map<std::string, Type> t;
        auto add = [&t](Type type, std::initializer_list<const char *> exts) {
            for (const char *e : exts) t[e] = type;
        };
        add(Type::C, {"c"});
        add(Type::CHeader, {"h"});
        add(Type::Cpp, {"cpp", "cc", "cxx", "c++", "cp", "ino", "cu", "tpp", "ipp", "inl"});
        add(Type::CppHeader, {"hpp", "hh", "hxx", "h++", "cuh"});
        add(Type::ObjC, {"m", "mm"});
        add(Type::CSharp, {"cs", "csx"});
        add(Type::Python, {"py", "pyi", "pyw", "pyx", "pxd"});
        add(Type::JavaScript, {"js", "mjs", "cjs", "jsx"});
        add(Type::TypeScript, {"ts", "mts", "cts", "tsx"});
        add(Type::Rust, {"rs"});
        add(Type::Go, {"go"});
        add(Type::Java, {"java"});
        add(Type::Kotlin, {"kt", "kts"});
        add(Type::Swift, {"swift"});
        add(Type::Ruby, {"rb", "rake", "gemspec", "ru", "erb"});
        add(Type::Php, {"php", "phtml"});
        add(Type::Perl, {"pl", "pm", "pod"});
        add(Type::Lua, {"lua"});
        add(Type::Shell, {"sh", "bash", "zsh", "fish", "ksh", "csh", "tcsh", "command", "bats",
                          "ps1", "psm1", "psd1"});
        add(Type::Haskell, {"hs", "lhs"});
        add(Type::Scala, {"scala", "sc"});
        add(Type::Dart, {"dart"});
        add(Type::R, {"r", "rmd"});
        add(Type::Julia, {"jl"});
        add(Type::Elixir, {"ex", "exs"});
        add(Type::Erlang, {"erl", "hrl"});
        add(Type::Zig, {"zig"});
        add(Type::Nim, {"nim", "nims"});
        add(Type::Vala, {"vala", "vapi"});
        add(Type::Fortran, {"f", "f90", "f95", "f03", "f08", "for"});
        add(Type::OCaml, {"ml", "mli"});
        add(Type::Lisp, {"el", "lisp", "lsp", "scm", "ss", "rkt", "clj", "cljs", "cljc", "edn",
                         "fnl"});
        add(Type::Qml, {"qml"});
        add(Type::Vue, {"vue", "svelte"});
        add(Type::Markdown, {"md", "markdown", "mdown", "mkd", "mdx"});
        add(Type::Html, {"html", "htm", "xhtml", "shtml"});
        add(Type::Css, {"css"});
        add(Type::Sass, {"scss", "sass"});
        add(Type::Less, {"less"});
        add(Type::Json, {"json", "jsonc", "json5", "jsonl", "ndjson", "geojson", "topojson",
                         "webmanifest", "har"});
        add(Type::Yaml, {"yml", "yaml"});
        add(Type::Toml, {"toml"});
        add(Type::Xml, {"xml", "xsd", "xsl", "xslt", "plist", "xaml", "csproj", "vbproj",
                        "fsproj", "resx", "storyboard", "xib", "wsdl", "dtd", "xliff", "xlf",
                        "rss", "atom"});
        add(Type::Csv, {"csv", "tsv"});
        add(Type::Sql, {"sql", "psql", "mysql", "pgsql", "plsql"});
        add(Type::Latex, {"tex", "ltx", "latex", "sty", "cls", "bib", "bst", "dtx", "ins"});
        add(Type::Notebook, {"ipynb"});
        add(Type::Svg, {"svg", "svgz"});
        add(Type::Video, {"mkv", "webm", "wmv", "flv", "mpg", "mpeg", "ogv"});
        add(Type::Audio, {"mid", "midi", "wma", "aifc", "caf", "weba"});
        add(Type::Pdf, {"pdf"});
        add(Type::Archive, {"zip", "tar", "gz", "tgz", "bz2", "tbz", "tbz2", "xz", "txz", "zst",
                            "7z", "rar", "jar", "war", "ear", "dmg", "iso", "deb", "rpm", "apk",
                            "whl", "lz", "lzma", "cab", "xpi", "snap", "flatpak"});
        add(Type::Font, {"ttf", "otf", "ttc", "woff", "woff2", "pfb", "pfa", "fon"});
        add(Type::Executable, {"exe", "msi", "appimage", "run", "out", "elf", "bin"});
        add(Type::Library, {"so", "dll", "dylib", "a", "lib", "o", "obj", "ko", "pyc", "class"});
        add(Type::Database, {"sqlite", "sqlite3", "db", "db3", "s3db", "sl3", "mdb", "accdb"});
        add(Type::Document, {"doc", "docx", "docm", "odt", "rtf", "pages"});
        add(Type::Spreadsheet, {"xls", "xlsx", "xlsm", "xlsb", "ods", "numbers"});
        add(Type::Presentation, {"ppt", "pptx", "pptm", "odp", "key"});
        add(Type::Ebook, {"epub", "mobi", "azw3"});
        add(Type::Makefile, {"mk", "mak", "make"});
        add(Type::CMake, {"cmake"});
        add(Type::Dockerfile, {"dockerfile", "containerfile"});
        add(Type::Lockfile, {"lock", "lockb"});
        add(Type::Config, {"ini", "conf", "cfg", "cnf", "config", "properties", "env", "desktop",
                           "service", "socket", "timer", "mount", "target", "editorconfig",
                           "reg", "inf"});
        add(Type::Patch, {"patch", "diff", "rej"});
        add(Type::Log, {"log"});
        add(Type::Key, {"pem", "crt", "cer", "der", "pub", "asc", "gpg", "sig", "p12", "pfx",
                        "csr"});
        add(Type::Bookmark, {"url", "webloc"});
        add(Type::Translation, {"po", "pot"});
        add(Type::Text, {"txt", "text", "rst", "adoc", "asciidoc", "org", "nfo"});
        return t;
    }();
    return m;
}

}  // namespace

Type typeOf(const std::string &path, bool executable) {
    const std::size_t slash = path.find_last_of('/');
    const std::string name = lowered(slash == std::string::npos ? path : path.substr(slash + 1));
    if (name.empty()) return Type::Text;
    const auto &byName = typesByName();
    auto n = byName.find(name);
    if (n != byName.end()) return n->second;
    const std::string ext = FileSniff::extensionOf(name);
    const auto &byExt = typesByExtension();
    auto e = ext.empty() ? byExt.end() : byExt.find(ext);
    Type byExtension = Type::Text;
    bool known = false;
    if (!ext.empty()) {
        known = true;
        if (isImageExtension(ext)) byExtension = Type::Image;
        else if (isVideoExtension(ext)) byExtension = Type::Video;
        else if (isAudioExtension(ext)) byExtension = Type::Audio;
        else if (e != byExt.end()) byExtension = e->second;
        else known = SyntaxHighlighter::supports(ext);
    }
    // LICENSE, LICENSE-MIT, README.md, Dockerfile.dev: the stem names the
    // file when what follows is no extension, a text one, or none known
    // (license_check.py is Python, readme.png a picture).
    const bool textual = !known || byExtension == Type::Text || byExtension == Type::Markdown;
    if (textual) {
        if (stemIs(name, "license") || stemIs(name, "licence") || stemIs(name, "copying") ||
            stemIs(name, "unlicense") || stemIs(name, "copyright"))
            return Type::License;
        if (stemIs(name, "readme")) return Type::Readme;
        if (stemIs(name, "authors") || stemIs(name, "contributors") || stemIs(name, "credits") ||
            stemIs(name, "maintainers"))
            return Type::Authors;
        if (stemIs(name, "dockerfile") || stemIs(name, "containerfile")) return Type::Dockerfile;
    }
    // docker-compose.override.yml
    if (stemIs(name, "docker-compose") && (ext == "yml" || ext == "yaml"))
        return Type::Dockerfile;
    if (known && byExtension != Type::Text) return byExtension;
    if (e != byExt.end()) return e->second;   // .txt and the like
    // A dotfile no table names is some program's settings (.env.local,
    // .prettierrc, .npmrc).
    if (name[0] == '.') return Type::Config;
    if (ext.empty() && executable) return Type::Executable;
    if (!ext.empty() && SyntaxHighlighter::supports(ext)) return Type::Code;
    return Type::Text;
}

const char *typeName(Type t) {
    switch (t) {
    case Type::Text: return "text";
    case Type::Code: return "code";
    case Type::C: return "c";
    case Type::CHeader: return "c-header";
    case Type::Cpp: return "cpp";
    case Type::CppHeader: return "cpp-header";
    case Type::ObjC: return "objc";
    case Type::CSharp: return "csharp";
    case Type::Python: return "python";
    case Type::JavaScript: return "javascript";
    case Type::TypeScript: return "typescript";
    case Type::Rust: return "rust";
    case Type::Go: return "go";
    case Type::Java: return "java";
    case Type::Kotlin: return "kotlin";
    case Type::Swift: return "swift";
    case Type::Ruby: return "ruby";
    case Type::Php: return "php";
    case Type::Perl: return "perl";
    case Type::Lua: return "lua";
    case Type::Shell: return "shell";
    case Type::Haskell: return "haskell";
    case Type::Scala: return "scala";
    case Type::Dart: return "dart";
    case Type::R: return "r";
    case Type::Julia: return "julia";
    case Type::Elixir: return "elixir";
    case Type::Erlang: return "erlang";
    case Type::Zig: return "zig";
    case Type::Nim: return "nim";
    case Type::Vala: return "vala";
    case Type::Fortran: return "fortran";
    case Type::OCaml: return "ocaml";
    case Type::Lisp: return "lisp";
    case Type::Qml: return "qml";
    case Type::Vue: return "vue";
    case Type::Markdown: return "markdown";
    case Type::Html: return "html";
    case Type::Css: return "css";
    case Type::Sass: return "sass";
    case Type::Less: return "less";
    case Type::Json: return "json";
    case Type::Yaml: return "yaml";
    case Type::Toml: return "toml";
    case Type::Xml: return "xml";
    case Type::Csv: return "csv";
    case Type::Sql: return "sql";
    case Type::Latex: return "latex";
    case Type::Notebook: return "notebook";
    case Type::Image: return "image";
    case Type::Svg: return "svg";
    case Type::Audio: return "audio";
    case Type::Video: return "video";
    case Type::Pdf: return "pdf";
    case Type::Archive: return "archive";
    case Type::Font: return "font";
    case Type::Executable: return "executable";
    case Type::Library: return "library";
    case Type::Database: return "database";
    case Type::Document: return "document";
    case Type::Spreadsheet: return "spreadsheet";
    case Type::Presentation: return "presentation";
    case Type::Ebook: return "ebook";
    case Type::Makefile: return "makefile";
    case Type::CMake: return "cmake";
    case Type::Meson: return "meson";
    case Type::Dockerfile: return "dockerfile";
    case Type::License: return "license";
    case Type::Readme: return "readme";
    case Type::Authors: return "authors";
    case Type::Lockfile: return "lockfile";
    case Type::Git: return "git";
    case Type::Config: return "config";
    case Type::Patch: return "patch";
    case Type::Log: return "log";
    case Type::Key: return "key";
    case Type::Bookmark: return "bookmark";
    case Type::Translation: return "translation";
    }
    return "text";
}

const std::vector<Type> &allTypes() {
    static const std::vector<Type> all = [] {
        std::vector<Type> v;
        for (int i = (int)Type::Text; i <= (int)Type::Translation; i++) v.push_back((Type)i);
        return v;
    }();
    return all;
}

const char *kindName(Kind k) {
    switch (k) {
    case Kind::Text: return "text";
    case Kind::Code: return "code";
    case Kind::Image: return "image";
    case Kind::Video: return "video";
    case Kind::Audio: return "audio";
    case Kind::Pdf: return "pdf";
    case Kind::Table: return "table";
    case Kind::Notebook: return "notebook";
    case Kind::Database: return "database";
    case Kind::Archive: return "archive";
    case Kind::Web: return "web";
    case Kind::Bookmark: return "bookmark";
    }
    return "text";
}

}  // namespace FileIcon

namespace TreeFocus {

namespace {

std::vector<std::string> names(const std::string &rel) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= rel.size()) {
        size_t slash = rel.find('/', at);
        if (slash == std::string::npos) slash = rel.size();
        if (slash > at) out.push_back(rel.substr(at, slash - at));
        at = slash + 1;
    }
    return out;
}

std::string joined(const std::vector<std::string> &parts, size_t count) {
    std::string out;
    for (size_t i = 0; i < count && i < parts.size(); i++) {
        if (i) out += '/';
        out += parts[i];
    }
    return out;
}

}  // namespace

std::string normalize(const std::string &focus) {
    std::vector<std::string> kept;
    for (const std::string &n : names(focus)) {
        if (n == ".") continue;
        if (n == "..") return "";
        kept.push_back(n);
    }
    return joined(kept, kept.size());
}

bool relative(const std::string &root, const std::string &path, std::string *out) {
    std::string r = root;
    while (r.size() > 1 && r.back() == '/') r.pop_back();
    if (r.empty()) return false;
    if (path == r) {
        *out = "";
        return true;
    }
    const std::string prefix = r == "/" ? r : r + "/";
    if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0) return false;
    *out = normalize(path.substr(prefix.size()));
    return true;
}

bool contains(const std::string &focus, const std::string &rel) {
    if (focus.empty() || rel == focus) return true;
    return rel.size() > focus.size() && rel.compare(0, focus.size(), focus) == 0 &&
           rel[focus.size()] == '/';
}

std::string parent(const std::string &rel) {
    const size_t slash = rel.rfind('/');
    return slash == std::string::npos ? "" : rel.substr(0, slash);
}

std::vector<Part> breadcrumb(const std::string &rootName, const std::string &focus) {
    std::vector<Part> out{{rootName, ""}};
    const std::vector<std::string> parts = names(normalize(focus));
    for (size_t i = 0; i < parts.size(); i++) out.push_back({parts[i], joined(parts, i + 1)});
    return out;
}

std::string widened(const std::string &focus, const std::string &rel) {
    if (rel != focus && contains(focus, rel)) return focus;
    const std::vector<std::string> a = names(focus), b = names(parent(rel));
    size_t common = 0;
    while (common < a.size() && common < b.size() && a[common] == b[common]) common++;
    return joined(a, common);
}

std::string restored(const std::string &saved,
                     const std::function<bool(const std::string &)> &isFolder) {
    const std::string focus = normalize(saved);
    if (focus.empty() || !isFolder || !isFolder(focus)) return "";
    return focus;
}

std::vector<int> fit(const std::vector<double> &widths, double separator, double ellipsis,
                     double available) {
    std::vector<int> out;
    const int n = (int)widths.size();
    if (n == 0) return out;
    double all = separator * (n - 1);
    for (double w : widths) all += w;
    if (all <= available) {
        for (int i = 0; i < n; i++) out.push_back(i);
        return out;
    }
    // The first, an ellipsis, then the most of the last parts that fit.
    for (int keep = n - 2; keep >= 1; keep--) {
        double w = widths[0] + separator + ellipsis;
        for (int i = n - keep; i < n; i++) w += separator + widths[(size_t)i];
        if (w <= available) {
            out = {0, -1};
            for (int i = n - keep; i < n; i++) out.push_back(i);
            return out;
        }
    }
    out = {0};
    if (n > 2) out.push_back(-1);
    if (n > 1) out.push_back(n - 1);
    return out;
}

Set added(const Set &set, const std::string &folder) {
    const std::string f = normalize(folder);
    if (f.empty()) return {};
    for (const std::string &s : set)
        if (contains(s, f)) return set;   // already shown (outer wins)
    Set out;
    bool placed = false;
    for (const std::string &s : set) {
        if (contains(f, s)) {   // inside the new folder: it takes the first's place
            if (!placed) out.push_back(f);
            placed = true;
            continue;
        }
        out.push_back(s);
    }
    if (!placed) out.push_back(f);
    return out;
}

Set removed(const Set &set, const std::string &folder) {
    const std::string f = normalize(folder);
    Set out;
    for (const std::string &s : set)
        if (s != f) out.push_back(s);
    return out;
}

Set merged(const Set &list) {
    Set out;
    for (const std::string &f : list) {
        if (normalize(f).empty()) continue;   // "" or one leaving the folder: no folder
        out = added(out, f);
    }
    return out;
}

int rootOf(const Set &set, const std::string &rel) {
    for (size_t i = 0; i < set.size(); i++)
        if (!set[i].empty() && contains(set[i], rel)) return (int)i;
    return -1;
}

bool shows(const Set &set, const std::string &rel) {
    if (set.empty()) return true;
    if (set.size() == 1) return rel != normalize(set[0]) && contains(set[0], rel);
    return rootOf(set, rel) >= 0;
}

Set revealed(const Set &set, const std::string &rel) {
    if (set.empty()) return set;
    if (set.size() == 1) {
        const std::string w = widened(set[0], rel);
        return w.empty() ? Set{} : Set{w};
    }
    if (rootOf(set, rel) >= 0) return set;
    return added(set, parent(rel));
}

Set restoredSet(const Set &saved, const std::function<bool(const std::string &)> &isFolder) {
    Set out;
    if (!isFolder) return out;
    for (const std::string &f : merged(saved))
        if (isFolder(f)) out.push_back(f);
    return out;
}

std::vector<std::string> shortLabels(const Set &set) {
    std::vector<std::vector<std::string>> parts;
    for (const std::string &s : set) parts.push_back(names(s));
    auto suffix = [&](size_t i, size_t k) {
        const std::vector<std::string> &p = parts[i];
        const size_t from = p.size() > k ? p.size() - k : 0;
        std::vector<std::string> tail(p.begin() + (long)from, p.end());
        return joined(tail, tail.size());
    };
    std::vector<std::string> out;
    for (size_t i = 0; i < set.size(); i++) {
        size_t k = 1;
        for (; k < parts[i].size(); k++) {
            bool unique = true;
            for (size_t j = 0; j < set.size() && unique; j++)
                if (j != i && suffix(j, k) == suffix(i, k)) unique = false;
            if (unique) break;
        }
        out.push_back(suffix(i, k));
    }
    return out;
}

std::vector<double> shared(const std::vector<double> &widths, double available) {
    double all = 0;
    for (double w : widths) all += w;
    if (all <= available || widths.empty()) return widths;
    if (available <= 0) return std::vector<double>(widths.size(), 0);
    // The cap: the narrow ones whole, what is left shared by the rest.
    std::vector<double> sorted = widths;
    std::sort(sorted.begin(), sorted.end());
    double left = available, cap = 0;
    for (size_t i = 0; i < sorted.size(); i++) {
        const double share = left / (double)(sorted.size() - i);
        if (sorted[i] > share) {
            cap = share;
            break;
        }
        left -= sorted[i];
        cap = sorted[i];
    }
    std::vector<double> out;
    for (double w : widths) out.push_back(std::min(w, cap));
    return out;
}

}  // namespace TreeFocus

namespace TreeReveal {

double scrollTop(double rowTop, double rowHeight, double viewTop, double viewHeight,
                 double contentHeight) {
    if (viewHeight <= 0) return viewTop;   // no view to show it in (a collapsed sidebar)
    if (rowTop >= viewTop && rowTop + rowHeight <= viewTop + viewHeight) return viewTop;
    double top = rowTop + rowHeight / 2 - viewHeight / 2;
    const double most = contentHeight - viewHeight;
    if (top > most) top = most;
    if (top < 0) top = 0;
    return top;
}

bool Follower::activeChanged(const std::string &path, bool treeOnShow) {
    if (treeOnShow) {
        pending_ = false;
        file_.clear();
        return !path.empty();
    }
    pending_ = true;
    file_ = path;
    return false;
}

std::string Follower::treeShown() {
    if (!pending_) return "";
    pending_ = false;
    std::string out;
    out.swap(file_);
    return out;
}

}  // namespace TreeReveal
