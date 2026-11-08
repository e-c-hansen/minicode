// FolderSearch.cpp — see FolderSearch.h.
#include "FolderSearch.h"
#include "MarkdownTasks.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <iterator>
#include <memory>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <queue>
#include <set>
#include <system_error>
#include <thread>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <copyfile.h>
#include <sys/attr.h>
#endif

namespace fs = std::filesystem;

namespace FolderSearch {

namespace {

// Decode the code point at s[i] (valid UTF-8 assumed) and its byte length.
char32_t decodeAt(const std::string& s, std::size_t i, std::size_t* len) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { *len = 1; return c; }
    std::size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
    if (i + n > s.size()) { *len = 1; return c; }   // defensive: truncated
    char32_t cp = c & (n == 2 ? 0x1F : n == 3 ? 0x0F : 0x07);
    for (std::size_t k = 1; k < n; ++k)
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    *len = n;
    return cp;
}

std::size_t countChars(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

bool isWhitespace(char32_t cp) {
    // NSCharacterSet whitespaceCharacterSet: tab plus the space separators.
    return cp == ' ' || cp == '\t' || cp == 0x00A0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F ||
           cp == 0x3000;
}

bool isControlOrFormat(char32_t cp) {
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) return true;   // C0, DEL, C1
    return cp == 0x00AD || cp == 0x061C || cp == 0x180E ||
           (cp >= 0x200B && cp <= 0x200F) ||   // zero-width, LRM, RLM
           (cp >= 0x202A && cp <= 0x202E) ||   // bidi embeddings and overrides
           (cp >= 0x2060 && cp <= 0x2064) ||
           (cp >= 0x2066 && cp <= 0x206F) ||   // bidi isolates and friends
           cp == 0xFEFF;
}

char32_t fold(char32_t c) { return TextFind::foldCase(c); }

bool isAscii(const std::string& s) {
    for (unsigned char c : s) if (c >= 0x80) return false;
    return true;
}

unsigned char asciiLower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

bool cancelled(const std::atomic<bool>* cancel) {
    return cancel && cancel->load(std::memory_order_relaxed);
}

// ------------------------------------------------------------ the needle
// Text a line must hold to match, looked for in a whole file at once
// (Horspool's search, on bytes, ASCII letters folded when `fold`).
struct Needle {
    std::string bytes;    // lower case when fold
    bool fold = false;
    bool never = false;   // holds a line break, so no line can hold it
    unsigned char skip[256] = {};

    bool empty() const { return bytes.empty(); }

    void prepare() {
        const std::size_t m = bytes.size();
        if (fold)
            for (char& c : bytes) c = static_cast<char>(asciiLower(static_cast<unsigned char>(c)));
        const unsigned char cap = static_cast<unsigned char>(std::min<std::size_t>(m, 255));
        for (unsigned char& k : skip) k = cap;
        for (std::size_t k = 0; k + 1 < m; ++k)
            skip[static_cast<unsigned char>(bytes[k])] =
                static_cast<unsigned char>(std::min<std::size_t>(m - 1 - k, cap));
    }

    unsigned char at(const char* s, std::size_t i) const {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        return fold ? asciiLower(c) : c;
    }

    // The first occurrence in s[from, n), or npos.
    std::size_t find(const char* s, std::size_t n, std::size_t from) const {
        const std::size_t m = bytes.size();
        if (m == 0 || from > n || n - from < m) return std::string::npos;
        const unsigned char* b = reinterpret_cast<const unsigned char*>(bytes.data());
        if (m == 1 && !fold) {
            const void* p = std::memchr(s + from, b[0], n - from);
            return p ? static_cast<std::size_t>(static_cast<const char*>(p) - s) : std::string::npos;
        }
        const unsigned char last = b[m - 1];
        if (!fold) {
            // The first byte by memchr, then the rest: quick when that byte
            // is rare, which a query's first letter usually is per line.
            for (std::size_t i = from; i + m <= n;) {
                const void* p = std::memchr(s + i, b[0], n - m + 1 - i);
                if (!p) return std::string::npos;
                i = static_cast<std::size_t>(static_cast<const char*>(p) - s);
                if (static_cast<unsigned char>(s[i + m - 1]) == last &&
                    std::memcmp(s + i + 1, b + 1, m - 1) == 0)
                    return i;
                ++i;
            }
            return std::string::npos;
        }
        for (std::size_t i = from; i + m <= n;) {
            const unsigned char c = at(s, i + m - 1);
            if (c == last) {
                std::size_t k = 0;
                while (k + 1 < m && at(s, i + k) == b[k]) ++k;
                if (k + 1 == m) return i;
            }
            i += skip[c];
        }
        return std::string::npos;
    }
};

// The needle for text every match holds (TextFind::requiredText, or the
// query): its UTF-8 when matching case; ignoring case, its longest run of
// ASCII characters, which only ASCII characters fold to.
Needle makeNeedle(const std::u32string& text, bool caseSensitive) {
    Needle nd;
    for (char32_t c : text)
        if (c == '\n' || c == '\r' || c == 0x2029) {
            nd.never = true;
            return nd;
        }
    if (caseSensitive) {
        for (char32_t c : text) {
            if (c < 0x80) nd.bytes += static_cast<char>(c);
            else if (c < 0x800) {
                nd.bytes += static_cast<char>(0xC0 | (c >> 6));
                nd.bytes += static_cast<char>(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                nd.bytes += static_cast<char>(0xE0 | (c >> 12));
                nd.bytes += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                nd.bytes += static_cast<char>(0x80 | (c & 0x3F));
            } else {
                nd.bytes += static_cast<char>(0xF0 | (c >> 18));
                nd.bytes += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                nd.bytes += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                nd.bytes += static_cast<char>(0x80 | (c & 0x3F));
            }
        }
    } else {
        std::string run;
        for (char32_t c : text) {
            if (c < 0x80) {
                run += static_cast<char>(c);
                if (run.size() > nd.bytes.size()) nd.bytes = run;
            } else {
                run.clear();
            }
        }
        nd.fold = true;
    }
    nd.prepare();
    return nd;
}

std::u32string decodeAll(const std::string& s) {
    std::u32string out;
    for (std::size_t i = 0, len; i < s.size(); i += len) out.push_back(decodeAt(s, i, &len));
    return out;
}

// Where the line starting at `pos` ends (its break left off) and the next
// begins: lines end at \n, \r\n, \r and U+2029.
void lineEndAt(const char* s, std::size_t n, std::size_t pos, std::size_t* end,
               std::size_t* next) {
    std::size_t e = pos;
    for (; e < n; ++e) {
        const char c = s[e];
        if (c == '\n') { *end = e; *next = e + 1; return; }
        if (c == '\r') {
            *end = e;
            *next = (e + 1 < n && s[e + 1] == '\n') ? e + 2 : e + 1;
            return;
        }
        if (c == '\xE2' && e + 2 < n && s[e + 1] == '\x80' && s[e + 2] == '\xA9') {
            *end = e;
            *next = e + 3;
            return;
        }
    }
    *end = *next = n;
}

// Calls f(lineNo, start, end, hit) for every line holding the needle, hit
// being where (or every line, with hit npos, when there is no needle),
// until f returns false. False when halt() said to stop; it is asked every
// 4096 lines.
template <class Halt, class F>
bool eachLine(const std::string& content, const Needle* nd, Halt halt, F f) {
    const char* s = content.data();
    const std::size_t n = content.size();
    std::size_t pos = 0;
    int lineNo = 1;
    while (pos < n) {
        std::size_t hit = std::string::npos;
        if (nd) {
            hit = nd->find(s, n, pos);
            if (hit == std::string::npos) return true;
        }
        for (;;) {
            std::size_t end, next;
            lineEndAt(s, n, pos, &end, &next);
            if ((lineNo & 4095) == 0 && halt()) return false;
            if (!nd || hit < next) {
                if (!f(lineNo, pos, end, hit)) return true;
                pos = next;
                ++lineNo;
                break;
            }
            pos = next;
            ++lineNo;
        }
    }
    return true;
}

// ------------------------------------------------------------- one file
enum class FileKind { Silent, Searched, TooLarge, NotUtf8 };

struct Job {
    std::string path, rel;
};

// What reading and matching one file found, merged in the walk's order.
struct Outcome {
    FileKind kind = FileKind::Silent;
    std::vector<FolderSearchMatch> rows;
    std::size_t planCount = 0;   // Replace in Folder's count
    bool outside = false;        // reached through a symlink to outside the root
    std::uint64_t dev = 0, ino = 0;
    bool cancelled = false;      // the cancel flag went up while reading it
    std::string path, rel;
};

// A NUL byte among the first 8 KB: a binary file (FileSniff's rule).
bool looksBinary(const char* s, std::size_t n) {
    return std::memchr(s, 0, std::min<std::size_t>(n, 8192)) != nullptr;
}

// Read a file for searching: only a regular file (after symlinks), opened
// without blocking so a pipe swapped in cannot hold the search.
FileKind readForSearch(const std::string& path, std::uintmax_t maxSize, std::string* out,
                       std::uint64_t* dev, std::uint64_t* ino) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return FileKind::Silent;
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return FileKind::Silent;
    }
    *dev = static_cast<std::uint64_t>(st.st_dev);
    *ino = static_cast<std::uint64_t>(st.st_ino);
    // At most one byte past the limit, in case it grew since.
    const std::uintmax_t size = static_cast<std::uintmax_t>(st.st_size);
    std::size_t want = static_cast<std::size_t>(std::min<std::uintmax_t>(size, maxSize) + 1);
    if (size > maxSize) want = 8192;   // only enough to tell text from binary
    out->resize(want);
    std::size_t got = 0;
    while (got < want) {
        const ssize_t r = ::read(fd, &(*out)[got], want - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += static_cast<std::size_t>(r);
    }
    ::close(fd);
    out->resize(got);
    if (size > maxSize || got > maxSize)
        return looksBinary(out->data(), out->size()) ? FileKind::Silent : FileKind::TooLarge;
    if (!isValidUtf8(*out))
        return looksBinary(out->data(), out->size()) ? FileKind::Silent : FileKind::NotUtf8;
    return FileKind::Searched;
}

// Everything the threads share, read only while a search runs.
struct Matcher {
    const std::string& query;
    const FolderSearchOptions& opt;
    const std::atomic<bool>* cancel;
    const std::atomic<bool>* halt = nullptr;   // the walk said to stop
    bool todos = false;                    // the TODO list, not a query
    const TextFind::Query* tq = nullptr;   // whole word or regex: TextFind matches
    const TextFind::Query* counter = nullptr;   // counts a row's matches
    bool plan = false;                     // counting for Replace in Folder
    std::string rootCanon;                 // the root's real path, for `outside`
    Needle needle;                         // what a matching line holds
    bool useNeedle = false;
    bool exact = false;                    // finding the needle is the match

    Matcher(const std::string& q, const FolderSearchOptions& o, const std::atomic<bool>* c)
        : query(q), opt(o), cancel(c) {}

    bool stopping() const {
        return cancelled(cancel) || halt->load(std::memory_order_relaxed) ||
               (tq && tq->tooSlow());
    }

    void run(const Job& job, Outcome& o) const {
        o.path = job.path;
        o.rel = job.rel;
        if (stopping()) return;
        std::string content;
        o.kind = readForSearch(job.path, opt.maxFileSize, &content, &o.dev, &o.ino);
        if (opt.onFileRead) opt.onFileRead();
        if (o.kind != FileKind::Searched) return;
        if (useNeedle && needle.never) return;   // no line can match
        const Needle* nd = useNeedle ? &needle : nullptr;
        auto halted = [this, &o] {
            if (cancelled(cancel)) o.cancelled = true;
            return stopping();
        };
        if (plan) countAll(content, nd, halted, o);
        else rows(content, nd, halted, o);
    }

    // Replace in Folder's count: every match.
    template <class Halt>
    void countAll(const std::string& content, const Needle* nd, Halt halted, Outcome& o) const {
        std::size_t count = 0;
        if (nd) {   // only lines holding the needle can hold a match
            eachLine(content, nd, halted, [&](int, std::size_t start, std::size_t end, std::size_t) {
                count += TextFind::findAllInLine(content.substr(start, end - start), *tq).size();
                return true;
            });
        } else {
            // Every line, the empty one after a last line break too, as
            // replaceInText sees them (^ matches there).
            std::size_t pos = 0, lines = 0;
            const std::size_t n = content.size();
            while (pos <= n) {
                if ((++lines & 4095) == 0 && halted()) break;
                std::size_t end, next;
                lineEndAt(content.data(), n, pos, &end, &next);
                count += TextFind::findAllInLine(content.substr(pos, end - pos), *tq).size();
                if (end >= n) break;
                pos = next;
            }
        }
        o.planCount = count;
        if (count && !rootCanon.empty()) {
            char resolved[PATH_MAX];
            if (::realpath(o.path.c_str(), resolved)) {
                const std::string real = resolved;
                o.outside = !(real.size() > rootCanon.size() &&
                              real.compare(0, rootCanon.size(), rootCanon) == 0 &&
                              (rootCanon.back() == '/' || real[rootCanon.size()] == '/'));
            }
        }
    }

    // A row per matching line (a TODO's line, for the TODO list).
    template <class Halt>
    void rows(const std::string& content, const Needle* nd, Halt halted, Outcome& o) const {
        // For the TODO list, a Markdown file's open tasks, by line.
        const bool markdown = todos && isMarkdownPath(o.path);
        std::vector<MarkdownTasks::OpenTask> tasks;
        if (markdown) tasks = MarkdownTasks::openTasks(content);
        std::size_t nextTask = 0;
        const std::size_t m = needle.bytes.size();
        eachLine(content, nd, halted, [&](int lineNo, std::size_t start, std::size_t end,
                                          std::size_t at) {
            const std::string line = content.substr(start, end - start);
            std::size_t col = 0, len = 0, count = 1;
            bool hit;
            if (todos) {
                while (nextTask < tasks.size() && tasks[nextTask].line < lineNo - 1) nextTask++;
                hit = findTodoInLine(line, markdown, &col, &len);
                if (!hit && nextTask < tasks.size() && tasks[nextTask].line == lineNo - 1) {
                    col = tasks[nextTask].column;
                    len = 3;
                    hit = col + len <= line.size();
                }
            } else if (exact) {
                // Plain text found by the needle: here, and as often as it
                // occurs on the line without overlapping.
                hit = true;
                col = at - start;
                len = m;
                count = 0;
                for (std::size_t p = col; p != std::string::npos && p + m <= line.size();
                     p = needle.find(line.data(), line.size(), p + m))
                    ++count;
            } else if (tq) {
                hit = TextFind::findInLine(line, *tq, &col, &len);
            } else {
                hit = findInLine(line, query, opt.caseSensitive, &col, &len);
            }
            if (!hit) return true;
            if (!todos && !exact && counter)
                count = std::max<std::size_t>(1, TextFind::findAllInLine(line, *counter).size());
            FolderSearchMatch r;
            r.path = o.path;
            r.relativePath = o.rel;
            r.line = lineNo;
            r.byteColumn = col;
            r.byteLength = len;
            r.column = static_cast<int>(countChars(line.substr(0, col))) + 1;
            r.text = displayText(line, opt.maxLineChars);
            r.count = count;
            o.rows.push_back(std::move(r));
            // No file gives more rows than the whole search may hold.
            return o.rows.size() < opt.maxMatches;
        });
    }
};

// ------------------------------------------------------------- the walk
// The serial walk's order: a folder's files by name, then each subfolder's
// whole walk, by name. Folders are listed and files read and matched on a
// few threads, earliest in that order first, and the calling thread takes
// the outcomes in that order, so what a search finds is the serial walk's,
// limit and counts included.

struct Node;

// A file of the walk.
struct FileSlot {
    std::string path, rel;
    std::unique_ptr<Outcome> outcome;   // set when done
};

// A folder of the walk. Its parent lists it and gives it its place; the
// fields below `listed` are written by whoever lists it, before `listed`
// is set under the walk's lock, and read only after.
struct Node {
    std::string path, rel;
    Ignore::Rules parentRules;   // in effect in its parent (the root: its own)
    const Node* parent = nullptr;
    std::vector<std::uint32_t> key;   // its place in the walk's order
    // Reached through a symlink (or a type the listing could not tell):
    // listed only once the merge reaches it, so a loop is never followed.
    bool deferred = false;
    bool listed = false;
    bool opened = false;
    std::uint64_t dev = 0, ino = 0;
    std::vector<FileSlot> files;
    std::vector<std::unique_ptr<Node>> dirs;
};

struct Task {
    std::vector<std::uint32_t> key;
    Node* dir = nullptr;     // list this folder
    FileSlot* file = nullptr;   // or search this file
};
struct LaterTask {   // the priority queue's order: the earliest place first
    bool operator()(const Task& a, const Task& b) const { return b.key < a.key; }
};

struct Walker {
    const FolderSearchOptions& opt;
    const std::atomic<bool>* cancel;
    FolderSearchResult& out;
    Matcher& matcher;
    FolderReplacePlan* plan = nullptr;     // counting for Replace in Folder
    std::atomic<bool> halt{false};
    // Folders already walked, by device and inode, so each real folder is
    // walked once, under the first name the walk meets.
    std::set<std::pair<std::uint64_t, std::uint64_t>> visited = {};
    // Replace in Folder: the files already counted, by device and inode, so
    // a file with two names is replaced once.
    std::set<std::pair<std::uint64_t, std::uint64_t>> files = {};
    // Files to include and exclude (the options' boxes).
    PathFilter include = {}, exclude = {};
    // The ignore files' view: the scope's path from their top, and whether
    // they are read at all.
    std::string ignorePrefix = {};
    bool useIgnore = false;

    std::mutex mu;
    std::condition_variable work, progress;
    std::priority_queue<Task, std::vector<Task>, LaterTask> tasks;
    bool closed = false;
    std::vector<std::thread> threads;

    Walker(const FolderSearchOptions& o, const std::atomic<bool>* c, FolderSearchResult& r,
           Matcher& m)
        : opt(o), cancel(c), out(r), matcher(m) {
        matcher.halt = &halt;
    }

    bool full() const { return !plan && out.matches.size() >= opt.maxMatches; }

    std::string ignorePath(const std::string& rel) const {
        return ignorePrefix.empty() ? rel : ignorePrefix + "/" + rel;
    }

    // ---- listing a folder (any thread)
    void list(Node& n) {
        DIR* d = ::opendir(n.path.c_str());
        struct Entry {
            std::string name;
            unsigned char type;
        };
        std::vector<Entry> entries;
        bool hasGitignore = false, hasIgnore = false, loop = false;
        if (d) {
            struct stat dst;
            if (::fstat(dirfd(d), &dst) == 0) {
                n.opened = true;
                n.dev = static_cast<std::uint64_t>(dst.st_dev);
                n.ino = static_cast<std::uint64_t>(dst.st_ino);
                for (const Node* p = n.parent; p; p = p->parent)
                    if (p->dev == n.dev && p->ino == n.ino) loop = true;   // the merge skips it
            }
            while (n.opened && !loop && !matcher.stopping()) {
                struct dirent* e = ::readdir(d);
                if (!e) break;
                const char* nm = e->d_name;
                if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
                std::string name = nm;
                if (name == ".gitignore") hasGitignore = true;
                else if (name == ".ignore") hasIgnore = true;
                if (isSkippedFile(name)) continue;
                entries.push_back({std::move(name), e->d_type});
            }
            ::closedir(d);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& a, const Entry& b) { return a.name < b.name; });
        // The folder's own ignore files (the root's came with its rules).
        const Ignore::Rules rules =
            (!n.parent || !useIgnore)
                ? n.parentRules
                : n.parentRules.enter(n.path, ignorePath(n.rel), hasGitignore, hasIgnore);
        for (const Entry& e : entries) {
            const std::string full = joinPath(n.path, e.name);
            const std::string childRel = n.rel.empty() ? e.name : n.rel + "/" + e.name;
            // What it is, symlinks followed (as the Mac's fileExistsAtPath).
            bool isDir = e.type == DT_DIR, isFile = e.type == DT_REG;
            if (e.type == DT_LNK || e.type == DT_UNKNOWN) {
                struct stat st;
                if (::stat(full.c_str(), &st) != 0) continue;   // broken, vanished
                isDir = S_ISDIR(st.st_mode);
                isFile = S_ISREG(st.st_mode);
            }
            if (useIgnore && !rules.empty() && rules.ignored(ignorePath(childRel), isDir))
                continue;
            if (isDir) {
                if (isSkippedDirectory(e.name) || (!exclude.empty() && exclude.matches(childRel)))
                    continue;
                auto child = std::make_unique<Node>();
                child->path = full;
                child->rel = childRel;
                child->parentRules = rules;
                child->parent = &n;
                child->deferred = e.type != DT_DIR;
                n.dirs.push_back(std::move(child));
                continue;
            }
            if (!isFile) continue;   // pipes, sockets, devices
            if (!exclude.empty() && exclude.matches(childRel)) continue;
            if (!include.empty() && !include.matches(childRel)) continue;
            n.files.push_back({full, childRel, nullptr});
        }
        // Places: the files first, then the folders.
        std::lock_guard<std::mutex> lock(mu);
        const std::uint32_t nf = static_cast<std::uint32_t>(n.files.size());
        for (std::uint32_t i = 0; i < nf; ++i) {
            Task t;
            t.key = n.key;
            t.key.push_back(i);
            t.file = &n.files[i];
            tasks.push(std::move(t));
        }
        for (std::uint32_t i = 0; i < n.dirs.size(); ++i) {
            Node& c = *n.dirs[i];
            c.key = n.key;
            c.key.push_back(nf + i);
            if (!c.deferred) tasks.push({c.key, &c, nullptr});
        }
        n.listed = true;
        if (n.files.size() + n.dirs.size() > 1) work.notify_all();
        else work.notify_one();
        progress.notify_all();
    }

    void run(const Task& t) {
        if (t.dir) {
            list(*t.dir);
            return;
        }
        auto o = std::make_unique<Outcome>();
        matcher.run({t.file->path, t.file->rel}, *o);
        std::lock_guard<std::mutex> lock(mu);
        t.file->outcome = std::move(o);
        progress.notify_all();
    }

    void worker() {
        for (;;) {
            Task t;
            {
                std::unique_lock<std::mutex> lock(mu);
                work.wait(lock, [this] { return closed || !tasks.empty(); });
                if (closed) return;
                t = tasks.top();
                tasks.pop();
            }
            run(t);
        }
    }

    // Until ready() holds (asked under the lock), running what is next in
    // the walk's order meanwhile. False when the search is stopping.
    template <class Ready>
    bool await(Ready ready) {
        std::unique_lock<std::mutex> lock(mu);
        for (;;) {
            if (ready()) return true;
            if (matcher.stopping()) return false;
            if (!tasks.empty()) {
                Task t = tasks.top();
                tasks.pop();
                lock.unlock();
                run(t);
                lock.lock();
                continue;
            }
            progress.wait_for(lock, std::chrono::milliseconds(20));
        }
    }

    // ---- the merge (the calling thread), in the walk's order
    // One file's outcome. False once the search is over.
    bool take(Outcome& o) {
        if (o.cancelled) { out.cancelled = true; return false; }
        if (o.kind == FileKind::TooLarge || o.kind == FileKind::NotUtf8) {
            const bool large = o.kind == FileKind::TooLarge;
            ++(large ? out.skippedTooLarge : out.skippedNotUtf8);
            if (out.skipped.size() < kMaxSkipsListed) out.skipped.push_back({o.rel, large});
            return true;
        }
        if (o.kind != FileKind::Searched) return true;
        if (plan) {   // one file under two names is replaced once
            if (!files.insert({o.dev, o.ino}).second) return true;
            ++out.filesSearched;
            if (o.planCount) {
                plan->files.push_back({o.path, o.rel, o.planCount, o.outside});
                plan->matches += o.planCount;
            }
            return true;
        }
        ++out.filesSearched;
        std::size_t added = 0;
        for (FolderSearchMatch& m : o.rows) {
            out.totalMatches += m.count;
            out.matches.push_back(std::move(m));
            ++added;
            if (full()) { out.truncated = true; break; }
        }
        if (added) ++out.filesMatched;
        return !out.truncated;
    }

    // A folder's walk. False once the search is over.
    bool visit(Node& n) {
        if (n.deferred) {   // listed now that the walk has come to it
            std::lock_guard<std::mutex> lock(mu);
            tasks.push({n.key, &n, nullptr});
            n.deferred = false;
            work.notify_one();
        }
        if (!await([&n] { return n.listed; })) return false;
        if (!n.opened || !visited.insert({n.dev, n.ino}).second)
            return true;   // unreadable, a loop, or a folder seen under another name
        for (FileSlot& f : n.files) {
            if (!await([&f] { return f.outcome != nullptr; })) return false;
            const bool more = take(*f.outcome);
            f.outcome.reset();
            if (!more) return false;
            if (matcher.tq && matcher.tq->tooSlow()) return false;
        }
        for (const std::unique_ptr<Node>& c : n.dirs)
            if (!visit(*c)) return false;
        return true;
    }

    // Search `root`, with the ignore files the options name.
    void start(const std::string& root) {
        include = PathFilter(opt.include, root);
        exclude = PathFilter(opt.exclude, root);
        Node top;
        top.path = root;
        useIgnore = opt.useIgnoreFiles;
        if (useIgnore) top.parentRules = Ignore::rulesFor(opt.ignoreTop, root, &ignorePrefix);
        tasks.push({top.key, &top, nullptr});
        // The calling thread works too, whenever it waits.
        const unsigned n = opt.threads ? opt.threads : defaultThreads();
        for (unsigned i = 1; i < n; ++i) {
            try {
                threads.emplace_back([this] { worker(); });
            } catch (...) {   // no more threads to be had: fewer will do
                break;
            }
        }
        visit(top);
        if (cancelled(cancel)) out.cancelled = true;
        if (matcher.tq && matcher.tq->tooSlow()) out.tooSlow = true;
        halt = true;   // whatever is still running ends at once
        {
            std::lock_guard<std::mutex> lock(mu);
            closed = true;
        }
        work.notify_all();
        for (std::thread& t : threads) t.join();
    }
};

}  // namespace

std::string normalizeQuery(const std::string& query) {
    if (!isValidUtf8(query)) return std::string();
    std::size_t b = 0, e = query.size();
    while (b < e) {
        std::size_t len;
        if (!isWhitespace(decodeAt(query, b, &len))) break;
        b += len;
    }
    while (e > b) {
        std::size_t start = e - 1;
        while (start > b && (static_cast<unsigned char>(query[start]) & 0xC0) == 0x80)
            --start;
        std::size_t len;
        if (!isWhitespace(decodeAt(query, start, &len))) break;
        e = start;
    }
    return query.substr(b, e - b);
}

bool isSearchable(const std::string& query) {
    return countChars(normalizeQuery(query)) >= kMinQueryChars;
}

bool isSkippedDirectory(const std::string& name) {
    static const char* const kSkip[] = {".git", ".svn", ".hg", "node_modules", "build",
                                        ".build", "DerivedData", "dist", ".venv", "venv",
                                        "__pycache__", ".mypy_cache", ".pytest_cache",
                                        ".ruff_cache", ".tox", ".nox"};
    for (const char* s : kSkip) if (name == s) return true;
    return false;
}

bool isSkippedFile(const std::string& name) { return name == ".DS_Store"; }

// ----------------------------------------------------------- ignore files
namespace Ignore {

namespace {

enum { kMatch = 0, kNoMatch = 1, kAbortAll = -1, kAbortToStarStar = -2 };

unsigned char fold(unsigned char c, bool caseFold) {
    return caseFold && c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

bool inClass(const std::string& name, unsigned char c) {
    if (name == "alnum") return std::isalnum(c);
    if (name == "alpha") return std::isalpha(c);
    if (name == "blank") return c == ' ' || c == '\t';
    if (name == "cntrl") return std::iscntrl(c);
    if (name == "digit") return std::isdigit(c);
    if (name == "graph") return std::isgraph(c);
    if (name == "lower") return std::islower(c);
    if (name == "print") return std::isprint(c);
    if (name == "punct") return std::ispunct(c);
    if (name == "space") return std::isspace(c);
    if (name == "upper") return std::isupper(c);
    if (name == "xdigit") return std::isxdigit(c);
    return false;
}

// git's dowild (wildmatch.c), with WM_PATHNAME always on.
int dowild(const unsigned char* p, const unsigned char* text, bool caseFold) {
    const unsigned char* pattern = p;
    for (unsigned char pc; (pc = *p) != '\0'; text++, p++) {
        unsigned char tc = *text;
        if (tc == '\0' && pc != '*') return kAbortAll;
        tc = fold(tc, caseFold);
        switch (pc) {
        case '\\':
            pc = *++p;
            if (pc == '\0') return kAbortAll;
            [[fallthrough]];
        default:
            if (tc != fold(pc, caseFold)) return kNoMatch;
            continue;
        case '?':
            if (tc == '/') return kNoMatch;
            continue;
        case '*': {
            bool matchSlash;
            if (*++p == '*') {
                const unsigned char* prev = p - 2;
                while (*++p == '*') {}
                if ((prev < pattern || *prev == '/') &&
                    (*p == '\0' || *p == '/' || (p[0] == '\\' && p[1] == '/'))) {
                    // "**/" also matches no folder at all.
                    if (p[0] == '/' && dowild(p + 1, text, caseFold) == kMatch) return kMatch;
                    matchSlash = true;
                } else {
                    matchSlash = false;   // "**" inside a component is "*"
                }
            } else {
                matchSlash = false;
            }
            if (*p == '\0') {
                if (!matchSlash && std::strchr((const char*)text, '/')) return kNoMatch;
                return kMatch;
            }
            if (!matchSlash && *p == '/') {
                // One "*" then "/": the rest of this component.
                const char* slash = std::strchr((const char*)text, '/');
                if (!slash) return kNoMatch;
                text = (const unsigned char*)slash;
                break;   // the loop's increments step over both slashes
            }
            for (;;) {
                if (tc == '\0') break;
                const int m = dowild(p, text, caseFold);
                if (m != kNoMatch) {
                    if (!matchSlash || m != kAbortToStarStar) return m;
                } else if (!matchSlash && tc == '/') {
                    return kAbortToStarStar;
                }
                tc = fold(*++text, caseFold);
            }
            return kAbortAll;
        }
        case '[': {
            pc = *++p;
            if (pc == '^') pc = '!';
            const bool negated = pc == '!';
            if (negated) pc = *++p;
            unsigned char prevc = 0;
            bool matched = false;
            do {
                if (!pc) return kAbortAll;
                if (pc == '\\') {
                    pc = *++p;
                    if (!pc) return kAbortAll;
                    if (tc == fold(pc, caseFold)) matched = true;
                } else if (pc == '-' && prevc && p[1] && p[1] != ']') {
                    pc = *++p;
                    if (pc == '\\') {
                        pc = *++p;
                        if (!pc) return kAbortAll;
                    }
                    if (tc <= pc && tc >= prevc) {
                        matched = true;
                    } else if (caseFold && tc >= 'a' && tc <= 'z') {
                        // tc is folded; "A-Z" takes it in its upper case
                        const unsigned char up = (unsigned char)(tc - 'a' + 'A');
                        if (up <= pc && up >= prevc) matched = true;
                    }
                    pc = 0;   // so prevc becomes 0
                } else if (pc == '[' && p[1] == ':') {
                    const unsigned char* s = p + 2;
                    const unsigned char* e = s;
                    while (*e && *e != ']') e++;
                    if (!*e) return kAbortAll;
                    if (e - s < 1 || e[-1] != ':') {
                        // Not a class after all: a literal "[".
                        if (tc == '[') matched = true;
                    } else {
                        const std::string name((const char*)s, (size_t)(e - s - 1));
                        if (inClass(name, *text) || (caseFold && inClass(name, tc))) matched = true;
                        p = e;
                        pc = 0;
                    }
                } else if (tc == fold(pc, caseFold)) {
                    matched = true;
                }
                prevc = pc;
            } while ((pc = *++p) != ']');
            if (matched == negated || tc == '/') return kNoMatch;
            continue;
        }
        }
    }
    return *text ? kNoMatch : kMatch;
}

std::string readSmall(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 1024 * 1024)
        return std::string();
    std::ifstream in(path, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return s;
}

}  // namespace

bool wildmatch(const std::string& pattern, const std::string& text, bool caseFold) {
    return dowild((const unsigned char*)pattern.c_str(), (const unsigned char*)text.c_str(),
                  caseFold) == kMatch;
}

bool defaultCaseFold() {
#ifdef __APPLE__
    return true;
#else
    return false;
#endif
}

std::vector<Rule> parse(const std::string& text) {
    std::vector<Rule> out;
    std::size_t at = 0;
    while (at <= text.size()) {
        std::size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? text.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        // Trailing spaces go, unless the last one is escaped ("a\ ").
        while (!line.empty() && line.back() == ' ') {
            std::size_t backslashes = 0;
            for (std::size_t i = line.size() - 1; i > 0 && line[i - 1] == '\\'; i--) backslashes++;
            if (backslashes % 2) break;
            line.pop_back();
        }
        Rule r;
        if (!line.empty() && line[0] == '!') {
            r.negate = true;
            line.erase(0, 1);
        }
        if (!line.empty() && line.back() == '/') {
            r.dirOnly = true;
            line.pop_back();
        }
        if (line.empty()) continue;
        r.anchored = line.find('/') != std::string::npos;
        if (line[0] == '/') line.erase(0, 1);
        if (line.empty()) continue;
        r.pattern = line;
        out.push_back(std::move(r));
    }
    return out;
}

struct Rules::Node {
    std::string base;   // the file's folder from the top, with a trailing '/'; "" at the top
    std::vector<Rule> rules;
    std::shared_ptr<const Node> parent;
};

Rules Rules::with(const std::string& relDir, const std::string& gitignore,
                  const std::string& ignore) const {
    Rules r = *this;
    const std::string base = relDir.empty() ? std::string() : relDir + "/";
    // .gitignore first, then .ignore over it: the head is checked first.
    for (const std::string* text : {&gitignore, &ignore}) {
        if (text->empty()) continue;
        std::vector<Rule> rules = parse(*text);
        if (rules.empty()) continue;
        auto n = std::make_shared<Node>();
        n->base = base;
        n->rules = std::move(rules);
        n->parent = r.head_;
        r.head_ = std::move(n);
    }
    return r;
}

Rules Rules::enter(const std::string& dirPath, const std::string& relDir) const {
    return with(relDir, readSmall(joinPath(dirPath, ".gitignore")),
                readSmall(joinPath(dirPath, ".ignore")));
}

Rules Rules::enter(const std::string& dirPath, const std::string& relDir, bool hasGitignore,
                   bool hasIgnore) const {
    if (!hasGitignore && !hasIgnore) return *this;
    return with(relDir, hasGitignore ? readSmall(joinPath(dirPath, ".gitignore")) : std::string(),
                hasIgnore ? readSmall(joinPath(dirPath, ".ignore")) : std::string());
}

Rules Rules::atTop(const std::string& top, bool caseFold) {
    Rules r;
    r.fold_ = caseFold;
    // The repository's own excludes, below its .gitignore files.
    const std::string exclude = readSmall(joinPath(joinPath(top, ".git"), "info/exclude"));
    r = r.with("", exclude, "");
    return r.enter(top, "");
}

bool Rules::ignored(const std::string& relPath, bool isDir) const {
    const std::size_t slash = relPath.rfind('/');
    const std::string name = slash == std::string::npos ? relPath : relPath.substr(slash + 1);
    for (const Node* n = head_.get(); n; n = n->parent.get()) {
        if (relPath.compare(0, n->base.size(), n->base) != 0) continue;
        const std::string fromBase = relPath.substr(n->base.size());
        for (auto it = n->rules.rbegin(); it != n->rules.rend(); ++it) {
            if (it->dirOnly && !isDir) continue;
            const bool hit = it->anchored ? wildmatch(it->pattern, fromBase, fold_)
                                          : wildmatch(it->pattern, name, fold_);
            if (hit) return !it->negate;
        }
    }
    return false;
}

Rules rulesFor(const std::string& top, const std::string& scope, std::string* scopeRel,
               bool caseFold) {
    if (scopeRel) scopeRel->clear();
    auto trimmed = [](std::string s) {
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        return s;
    };
    const std::string t = trimmed(top), sc = trimmed(scope);
    std::string rel;
    if (!t.empty() && sc != t) {
        // The scope's path from the top, by spelling, then by real paths
        // (the tree may say /tmp where the scope says /private/tmp).
        auto inside = [&](const std::string& a, const std::string& b, std::string* r) {
            if (b.size() > a.size() + 1 && b.compare(0, a.size(), a) == 0 && b[a.size()] == '/') {
                *r = b.substr(a.size() + 1);
                return true;
            }
            return false;
        };
        if (!inside(t, sc, &rel)) {
            std::error_code e1, e2;
            const std::string ct = fs::canonical(t, e1).string(), cs = fs::canonical(sc, e2).string();
            if (e1 || e2 || !inside(ct, cs, &rel)) return Rules::atTop(sc, caseFold);
        }
    } else {
        return Rules::atTop(sc, caseFold);
    }
    Rules r = Rules::atTop(t, caseFold);
    std::string dir = t, sofar;
    std::size_t at = 0;
    while (at <= rel.size()) {
        std::size_t slash = rel.find('/', at);
        const std::string part = rel.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        at = slash == std::string::npos ? rel.size() + 1 : slash + 1;
        if (part.empty()) continue;
        dir = joinPath(dir, part);
        sofar = sofar.empty() ? part : sofar + "/" + part;
        r = r.enter(dir, sofar);
    }
    if (scopeRel) *scopeRel = rel;
    return r;
}

}  // namespace Ignore

bool isValidUtf8(const std::string& s) {
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        // Eight ASCII bytes at a time, which is most of any source file.
        while (i + 8 <= n) {
            std::uint64_t w;
            std::memcpy(&w, s.data() + i, 8);
            if (w & 0x8080808080808080ULL) break;
            i += 8;
        }
        if (i >= n) break;
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        std::size_t len;
        char32_t min;
        char32_t cp;
        if ((c & 0xE0) == 0xC0)      { len = 2; min = 0x80;    cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; min = 0x800;   cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; min = 0x10000; cp = c & 0x07; }
        else return false;
        if (i + len > n) return false;
        for (std::size_t k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += len;
    }
    return true;
}

std::string cleanLine(const std::string& line) {
    std::string out;
    out.reserve(line.size());
    const std::size_t n = line.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(line[i]);
        if (c == 0x1B && i + 1 < n) {
            // The Mac's pattern: \x1b[@-_][0-?]*[ -/]*[@-~]
            const unsigned char k = static_cast<unsigned char>(line[i + 1]);
            if (k >= 0x40 && k <= 0x5F) {
                std::size_t j = i + 2;
                while (j < n && line[j] >= 0x30 && line[j] <= 0x3F) ++j;
                while (j < n && line[j] >= 0x20 && line[j] <= 0x2F) ++j;
                if (j < n && line[j] >= 0x40 && line[j] <= 0x7E) {
                    i = j + 1;
                    continue;
                }
            }
        }
        std::size_t len;
        const char32_t cp = decodeAt(line, i, &len);
        if (!isControlOrFormat(cp)) out.append(line, i, len);
        i += len;
    }
    return out;
}

std::string displayText(const std::string& rawLine, std::size_t maxChars) {
    std::string s = normalizeQuery(cleanLine(rawLine));   // same trim rules
    std::size_t chars = 0, i = 0;
    while (i < s.size()) {
        if (chars == maxChars) { s.resize(i); break; }
        std::size_t len;
        decodeAt(s, i, &len);
        i += len;
        ++chars;
    }
    return s;
}

bool findInLine(const std::string& line, const std::string& query,
                bool caseSensitive, std::size_t* byteColumn,
                std::size_t* byteLength) {
    if (query.empty()) return false;
    if (caseSensitive) {
        const std::size_t at = line.find(query);
        if (at == std::string::npos) return false;
        *byteColumn = at;
        *byteLength = query.size();
        return true;
    }
    if (isAscii(query)) {
        // Fast path. No non-ASCII character folds to ASCII here, and ASCII
        // bytes never occur inside a multi-byte sequence, so a byte-wise
        // comparison is exact.
        const std::size_t m = query.size();
        if (line.size() < m) return false;
        const unsigned char q0 = asciiLower(static_cast<unsigned char>(query[0]));
        for (std::size_t i = 0; i + m <= line.size(); ++i) {
            if (asciiLower(static_cast<unsigned char>(line[i])) != q0) continue;
            std::size_t k = 1;
            while (k < m && asciiLower(static_cast<unsigned char>(line[i + k])) ==
                                asciiLower(static_cast<unsigned char>(query[k])))
                ++k;
            if (k == m) { *byteColumn = i; *byteLength = m; return true; }
        }
        return false;
    }
    // General path: compare folded code points.
    std::vector<char32_t> q;
    for (std::size_t i = 0, len; i < query.size(); i += len)
        q.push_back(fold(decodeAt(query, i, &len)));
    std::vector<char32_t> cps;
    std::vector<std::size_t> offs;
    for (std::size_t i = 0, len; i < line.size(); i += len) {
        cps.push_back(fold(decodeAt(line, i, &len)));
        offs.push_back(i);
    }
    offs.push_back(line.size());
    if (cps.size() < q.size()) return false;
    for (std::size_t i = 0; i + q.size() <= cps.size(); ++i) {
        if (std::equal(q.begin(), q.end(), cps.begin() + static_cast<std::ptrdiff_t>(i))) {
            *byteColumn = offs[i];
            *byteLength = offs[i + q.size()] - offs[i];
            return true;
        }
    }
    return false;
}

unsigned defaultThreads() {
    // Every core, efficiency cores too: on an M3 (four of each) eight
    // threads searched a 7,600-file tree in 140 to 160 ms against 185 to
    // 210 ms with four, as much of the time is opening files.
    const unsigned n = std::thread::hardware_concurrency();
    return std::max(1u, std::min(n ? n : 1u, 8u));
}

FolderSearchResult search(const std::string& root, const std::string& query,
                          const std::atomic<bool>* cancel,
                          const FolderSearchOptions& options) {
    FolderSearchResult out;
    if (!isSearchable(query, options) || options.maxMatches == 0) return out;
    const std::string q = options.regex ? query : normalizeQuery(query);
    TextFind::Query tq;
    if (options.regex || options.wholeWord) {
        tq = makeQuery(query, options);
        if (!tq.usable()) {
            out.error = tq.error();
            return out;
        }
    }
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    // A row is a line; its count is every match on it, as Replace in
    // Folder's plan counts them, so the two say the same number.
    const TextFind::Query counter = tq.usable() ? tq : makeQuery(query, options);
    Matcher m(q, options, cancel);
    if (counter.usable()) m.counter = &counter;
    if (tq.usable()) {
        m.tq = &tq;
        m.needle = makeNeedle(TextFind::requiredText(tq), options.caseSensitive);
        m.useNeedle = m.needle.never || !m.needle.empty();
    } else {
        m.needle = makeNeedle(decodeAll(q), options.caseSensitive);
        m.useNeedle = m.needle.never || !m.needle.empty();
        // Plain text matching case, or in ASCII (which only ASCII folds
        // to): the needle is the query.
        m.exact = m.useNeedle && !m.needle.never && (options.caseSensitive || isAscii(q));
    }
    Walker w(options, cancel, out, m);
    w.start(root);
    return out;
}

TextFind::Query makeQuery(const std::string& query, const FolderSearchOptions& options) {
    TextFind::Options o;
    o.caseSensitive = options.caseSensitive;
    o.wholeWord = options.wholeWord;
    o.regex = options.regex;
    o.preserveCase = options.preserveCase;
    return TextFind::Query::make(options.regex ? query : normalizeQuery(query), o);
}

bool isSearchable(const std::string& query, const FolderSearchOptions& options) {
    if (options.regex) return !query.empty() && isValidUtf8(query);
    return isSearchable(query);
}

FolderReplacePlan planReplace(const std::string& root, const std::string& query,
                              const std::atomic<bool>* cancel,
                              const FolderSearchOptions& options) {
    FolderReplacePlan plan;
    if (!isSearchable(query, options)) return plan;
    const TextFind::Query tq = makeQuery(query, options);
    if (!tq.usable()) {
        plan.error = tq.error();
        return plan;
    }
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return plan;
    FolderSearchResult out;
    const std::string none;
    Matcher m(none, options, cancel);
    m.tq = &tq;
    m.plan = true;
    m.needle = makeNeedle(TextFind::requiredText(tq), options.caseSensitive);
    m.useNeedle = m.needle.never || !m.needle.empty();
    const fs::path canon = fs::canonical(root, ec);
    if (!ec) m.rootCanon = canon.string();
    Walker w(options, cancel, out, m);
    w.plan = &plan;
    w.start(root);
    plan.cancelled = out.cancelled;
    return plan;
}

std::string replaceInText(const std::string& content, const std::string& query,
                          const std::string& replacement, const FolderSearchOptions& options,
                          std::size_t* count) {
    if (count) *count = 0;
    if (!isSearchable(query, options)) return content;
    return TextFind::replaceInText(content, makeQuery(query, options), replacement, count);
}

bool lineAt(const std::string& content, int line, std::string* out) {
    const std::size_t n = content.size();
    std::size_t pos = 0;
    for (int at = 1; pos < n; ++at) {   // as the walker counts lines
        std::size_t end = pos, next = n;
        for (; end < n; ++end) {
            const char c = content[end];
            if (c == '\n') { next = end + 1; break; }
            if (c == '\r') {
                next = (end + 1 < n && content[end + 1] == '\n') ? end + 2 : end + 1;
                break;
            }
            if (c == '\xE2' && end + 2 < n && content[end + 1] == '\x80' &&
                content[end + 2] == '\xA9') {
                next = end + 3;
                break;
            }
        }
        if (at == line) {
            *out = content.substr(pos, end - pos);
            return true;
        }
        pos = next;
    }
    return false;
}

bool replaceMatchInText(const std::string& content, const std::string& query,
                        const std::string& replacement, const FolderSearchOptions& options,
                        int line, std::size_t byteColumn, std::string* out,
                        const std::string* expectedText) {
    if (!isSearchable(query, options)) return false;
    if (expectedText) {
        std::string now;
        if (!lineAt(content, line, &now) ||
            displayText(now, options.maxLineChars) != *expectedText)
            return false;
    }
    return TextFind::replaceOneInText(content, makeQuery(query, options), replacement, line,
                                      byteColumn, out);
}

bool readTextFile(const std::string& path, std::uintmax_t maxFileSize, std::string* out) {
    std::error_code ec;
    const fs::file_status st = fs::status(path, ec);
    if (ec || !fs::is_regular_file(st)) return false;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > maxFileSize) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string content;
    content.resize(static_cast<std::size_t>(size) + 1);
    in.read(&content[0], static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<std::size_t>(in.gcount()));
    if (content.size() > maxFileSize || !isValidUtf8(content)) return false;
    *out = std::move(content);
    return true;
}

namespace {

bool writeAll(int fd, const std::string& content) {
    const char* p = content.data();
    std::size_t left = content.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    return true;
}

bool fail(std::string* error, const std::string& what) {
    if (error) *error = what + ": " + std::strerror(errno);
    return false;
}

void fillStamp(const struct stat& st, FileStamp* out) {
    out->exists = true;
    out->regular = S_ISREG(st.st_mode);
#ifdef __APPLE__
    out->mtimeSec = static_cast<std::int64_t>(st.st_mtimespec.tv_sec);
    out->mtimeNsec = st.st_mtimespec.tv_nsec;
#else
    out->mtimeSec = static_cast<std::int64_t>(st.st_mtim.tv_sec);
    out->mtimeNsec = st.st_mtim.tv_nsec;
#endif
    out->device = static_cast<std::uint64_t>(st.st_dev);
    out->inode = static_cast<std::uint64_t>(st.st_ino);
    out->size = static_cast<std::uint64_t>(st.st_size);
}

// Through the drive's own cache where the system can (F_FULLFSYNC on the
// Mac; plain fsync leaves the data in the drive's cache), else fsync.
bool syncToDisk(int fd) {
#ifdef F_FULLFSYNC
    if (::fcntl(fd, F_FULLFSYNC) == 0) return true;   // unsupported on some volumes
#endif
    return ::fsync(fd) == 0;
}

void syncFolder(const std::string& dir) {
    const int d = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (d < 0) return;
    syncToDisk(d);
    ::close(d);
}

// Where a write to `path` lands: the file at the end of any chain of
// symlinks at it, even one that does not exist yet (a dangling link, such
// as a settings file kept in a dotfiles folder not filled in yet), so the
// link stays a link. A relative link is read against its own folder. `path`
// itself when it is no link; "" for a loop.
std::string linkTarget(const std::string& path) {
    char resolved[PATH_MAX];
    if (::realpath(path.c_str(), resolved)) return resolved;
    std::string p = path;
    for (int hops = 0; hops < 40; ++hops) {
        struct stat st;
        if (::lstat(p.c_str(), &st) != 0 || !S_ISLNK(st.st_mode)) return p;
        char buf[PATH_MAX];
        const ssize_t n = ::readlink(p.c_str(), buf, sizeof buf - 1);
        if (n <= 0) return p;
        std::string next(buf, static_cast<std::size_t>(n));
        if (next[0] != '/') {
            const std::size_t slash = p.rfind('/');
            next = (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + next;
        }
        p = next;
    }
    errno = ELOOP;
    return std::string();
}

bool writeInPlace(const std::string& target, const std::string& content, std::string* error,
                  FileStamp* written) {
    const int fd = ::open(target.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return fail(error, "could not open the file");
    bool ok = writeAll(fd, content) && syncToDisk(fd);
    struct stat st;
    if (ok && written && ::fstat(fd, &st) == 0) fillStamp(st, written);
    const int saved = errno;
    ::close(fd);
    errno = saved;
    return ok || fail(error, "could not write the file");
}

// What the file had that a new file would not: its extended attributes
// (Finder tags, quarantine), its ACL and its creation date. The Mac only;
// elsewhere a new file keeps only the mode and owner.
void copyMetadata(const std::string& from, int to) {
#ifdef __APPLE__
    const int src = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (src >= 0) {
        ::fcopyfile(src, to, nullptr, COPYFILE_ACL | COPYFILE_XATTR);
        ::close(src);
    }
    struct attrlist al = {};
    al.bitmapcount = ATTR_BIT_MAP_COUNT;
    al.commonattr = ATTR_CMN_CRTIME;
    struct {
        std::uint32_t length;
        struct timespec created;
    } __attribute__((aligned(4), packed)) buf;
    if (::getattrlist(from.c_str(), &al, &buf, sizeof buf, 0) == 0)
        ::fsetattrlist(to, &al, &buf.created, sizeof buf.created, 0);
#else
    (void)from;
    (void)to;
#endif
}

}  // namespace

FileStamp stampOf(const std::string& path) {
    FileStamp s;
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) fillStamp(st, &s);
    return s;
}

ReadStatus readFileStable(const std::string& path, std::uintmax_t maxFileSize,
                          std::string* out, FileStamp* stamp) {
    FileStamp before = stampOf(path);
    for (int attempt = 0;; ++attempt) {
        if (stamp) *stamp = before;
        if (!before.exists) return ReadStatus::Missing;
        if (!before.regular) return ReadStatus::NotRegular;
        if (before.size > maxFileSize) return ReadStatus::TooLarge;
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) return ReadStatus::Unreadable;
        std::string content;
        // One byte past the limit, to notice a file that grew past it.
        const std::uintmax_t cap = maxFileSize < UINTMAX_MAX ? maxFileSize + 1 : maxFileSize;
        content.resize(static_cast<std::size_t>(
            std::min<std::uintmax_t>(cap, std::max<std::uint64_t>(before.size + 1, 4096))));
        std::size_t got = 0;
        bool ok = true;
        for (;;) {
            if (got == content.size()) {
                if (content.size() >= cap) break;
                content.resize(static_cast<std::size_t>(
                    std::min<std::uintmax_t>(cap, content.size() * 2)));
            }
            const ssize_t n = ::read(fd, &content[got], content.size() - got);
            if (n < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            if (n == 0) break;
            got += static_cast<std::size_t>(n);
        }
        ::close(fd);
        if (!ok) return ReadStatus::Unreadable;
        content.resize(got);
        const FileStamp after = stampOf(path);
        if (after != before && attempt < 2) {   // written meanwhile: again
            before = after;
            continue;
        }
        if (content.size() > maxFileSize) return ReadStatus::TooLarge;
        *out = std::move(content);
        return ReadStatus::Ok;
    }
}

bool stripUtf8Bom(std::string* bytes) {
    if (bytes->size() < 3 || bytes->compare(0, 3, "\xEF\xBB\xBF") != 0) return false;
    bytes->erase(0, 3);
    return true;
}

bool writeFileAtomically(const std::string& path, const std::string& content,
                         std::string* error, FileStamp* written) {
    // The file a symlink names, so the link stays a link (a dangling one
    // too: renamed over, it would become a plain file).
    const std::string target = linkTarget(path);
    if (target.empty()) return fail(error, "could not follow the link");
    struct stat st;
    const bool exists = ::stat(target.c_str(), &st) == 0;
    if (exists && !S_ISREG(st.st_mode)) {
        if (error) *error = "not a regular file";
        return false;
    }
    // Hard links: a rename would part this name from the others.
    if (exists && st.st_nlink > 1) return writeInPlace(target, content, error, written);
    const std::size_t slash = target.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : target.substr(0, slash + 1);
    const std::string base = slash == std::string::npos ? target : target.substr(slash + 1);
    std::string tmp = dir + "." + base + ".minicode-XXXXXX";
    std::vector<char> name(tmp.begin(), tmp.end());
    name.push_back('\0');
    const int fd = ::mkstemp(name.data());
    if (fd < 0) {
        // A folder this user may not write in, holding a file it may.
        if (exists) return writeInPlace(target, content, error, written);
        return fail(error, "could not make a temporary file beside it");
    }
    tmp = name.data();
    bool ok = true;
    if (exists) {
        ok = ::fchmod(fd, st.st_mode & 07777) == 0;
        struct stat mine;
        if (ok && ::fstat(fd, &mine) == 0 && (mine.st_uid != st.st_uid || mine.st_gid != st.st_gid) &&
            ::fchown(fd, st.st_uid, st.st_gid) != 0) {
            // Only root may give a file away: written in place, it keeps
            // its owner (and group) instead of quietly becoming this user's.
            ::close(fd);
            ::unlink(tmp.c_str());
            return writeInPlace(target, content, error, written);
        }
        if (ok) copyMetadata(target, fd);
    } else {
        const mode_t mask = ::umask(0);
        ::umask(mask);
        ok = ::fchmod(fd, 0666 & ~mask) == 0;
    }
    ok = ok && writeAll(fd, content) && syncToDisk(fd);
    struct stat now;
    FileStamp stamp;
    if (ok && ::fstat(fd, &now) == 0) fillStamp(now, &stamp);
    int saved = errno;
    ::close(fd);
    if (ok && ::rename(tmp.c_str(), target.c_str()) == 0) {
        syncFolder(dir);   // the rename itself survives a power loss
        if (written) *written = stamp;
        return true;
    }
    if (ok) saved = errno;
    ::unlink(tmp.c_str());
    errno = saved;
    return fail(error, "could not write the file");
}

bool isMarkdownPath(const std::string& path) {
    const std::size_t dot = path.rfind('.');
    const std::size_t slash = path.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return false;
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(asciiLower(static_cast<unsigned char>(c)));
    return ext == "md" || ext == "markdown" || ext == "mdown" || ext == "mkd" ||
           ext == "mkdn";
}

namespace {

bool isWordByte(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c >= 0x80;
}

// Whether line[0, end) ends in a comment opener, spaces after it allowed:
// //, #, --, ;, /*, <!--, %, or a line starting with * (inside a block
// comment). In Markdown only <!-- counts, since # starts a heading there.
bool afterCommentOpener(const std::string& line, std::size_t end, bool markdown) {
    std::size_t e = end;
    while (e > 0 && (line[e - 1] == ' ' || line[e - 1] == '\t')) --e;
    if (e == 0) return false;
    auto endsWith = [&](const char* s) {
        const std::size_t n = std::char_traits<char>::length(s);
        return e >= n && line.compare(e - n, n, s) == 0;
    };
    if (endsWith("<!--")) return true;
    if (markdown) return false;
    if (endsWith("//") || endsWith("--") || endsWith("/*")) return true;
    const char c = line[e - 1];
    if (c == '#' || c == ';' || c == '%') return true;
    if (c == '*') {
        // "/**" opens a doc comment; a line of only stars and spaces before
        // the tag is the middle of a block comment.
        std::size_t k = e;
        while (k > 0 && line[k - 1] == '*') --k;
        if (k > 0 && line[k - 1] == '/') return true;
        for (std::size_t j = 0; j < k; ++j)
            if (line[j] != ' ' && line[j] != '\t') return false;
        return true;
    }
    return false;
}

}  // namespace

bool findTodoInLine(const std::string& line, bool markdown, std::size_t* byteColumn,
                    std::size_t* byteLength) {
    static const char* const kTags[] = {"TODO", "FIXME", "HACK", "XXX", "BUG"};
    for (std::size_t i = 0; i < line.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(line[i]);
        if (c < 'B' || c > 'X') continue;
        if (i > 0 && isWordByte(static_cast<unsigned char>(line[i - 1]))) continue;
        for (const char* tag : kTags) {
            const std::size_t n = std::char_traits<char>::length(tag);
            if (line.compare(i, n, tag) != 0) continue;
            if (i + n < line.size() && isWordByte(static_cast<unsigned char>(line[i + n])))
                continue;
            const char next = i + n < line.size() ? line[i + n] : '\0';
            bool atStart = true;
            for (std::size_t j = 0; j < i; ++j)
                if (line[j] != ' ' && line[j] != '\t') { atStart = false; break; }
            // After a comment opener, or first on a line when a colon
            // follows ("TODO: ..." in a docstring or a block comment).
            if (afterCommentOpener(line, i, markdown) || (atStart && next == ':')) {
                *byteColumn = i;
                *byteLength = n;
                return true;
            }
        }
    }
    return false;
}

FolderSearchResult findTodos(const std::string& root, const std::atomic<bool>* cancel,
                             const FolderSearchOptions& options) {
    FolderSearchResult out;
    std::error_code ec;
    if (options.maxMatches == 0 || !fs::is_directory(root, ec)) return out;
    const std::string none;
    Matcher m(none, options, cancel);
    m.todos = true;
    Walker w(options, cancel, out, m);
    w.start(root);
    return out;
}

// ------------------------------------------- files to include / exclude
namespace {

// One character class at p ('[' ... ']'): whether c is in it, and where the
// pattern goes on. False with *next == p when it is not a class at all (no
// closing bracket), so the '[' is taken literally.
bool classMatch(const std::string& p, std::size_t at, char c, std::size_t* next, bool* in) {
    std::size_t i = at + 1;
    bool negate = false;
    if (i < p.size() && (p[i] == '!' || p[i] == '^')) {
        negate = true;
        ++i;
    }
    bool found = false;
    bool first = true;
    for (; i < p.size(); ++i) {
        if (p[i] == ']' && !first) {
            *next = i + 1;
            *in = found != negate;
            return true;
        }
        first = false;
        char lo = p[i];
        if (lo == '\\' && i + 1 < p.size()) lo = p[++i];
        char hi = lo;
        if (i + 2 < p.size() && p[i + 1] == '-' && p[i + 2] != ']') {
            hi = p[i + 2];
            i += 2;
        }
        if (c >= lo && c <= hi) found = true;
    }
    *next = at;
    return false;
}

bool globAt(const std::string& p, std::size_t pi, const std::string& s, std::size_t si) {
    while (pi < p.size()) {
        const char c = p[pi];
        if (c == '*') {
            const bool star2 = pi + 1 < p.size() && p[pi + 1] == '*' &&
                               (pi == 0 || p[pi - 1] == '/') &&
                               (pi + 2 == p.size() || p[pi + 2] == '/');
            if (star2) {
                if (pi + 2 == p.size()) return true;   // everything below
                const std::size_t rest = pi + 3;       // after "**/"
                if (globAt(p, rest, s, si)) return true;
                for (std::size_t k = si; k < s.size(); ++k)
                    if (s[k] == '/' && globAt(p, rest, s, k + 1)) return true;
                return false;
            }
            while (pi < p.size() && p[pi] == '*') ++pi;
            for (std::size_t k = si;; ++k) {
                if (globAt(p, pi, s, k)) return true;
                if (k >= s.size() || s[k] == '/') return false;
            }
        }
        if (si >= s.size()) return false;
        if (c == '?') {
            if (s[si] == '/') return false;
            ++pi;
            ++si;
            continue;
        }
        if (c == '[') {
            std::size_t next;
            bool in;
            if (classMatch(p, pi, s[si], &next, &in)) {
                if (!in || s[si] == '/') return false;
                pi = next;
                ++si;
                continue;
            }
        }
        char lit = c;
        if (c == '\\' && pi + 1 < p.size()) lit = p[++pi];
        if (lit != s[si]) return false;
        ++pi;
        ++si;
    }
    return si == s.size();
}

// {a,b} alternatives spelled out: "x.{c,h}" -> "x.c", "x.h". Nested braces
// expand from the inside out; a brace without its partner is literal.
void expandBraces(const std::string& p, std::vector<std::string>* out) {
    int depth = 0;
    std::size_t open = std::string::npos;
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '\\') {
            ++i;
            continue;
        }
        if (p[i] == '{') {
            if (depth++ == 0) open = i;
        } else if (p[i] == '}' && depth > 0 && --depth == 0) {
            std::vector<std::string> alts;
            std::string cur;
            int d = 0;
            for (std::size_t k = open + 1; k < i; ++k) {
                const char ch = p[k];
                if (ch == '{') ++d;
                if (ch == '}') --d;
                if (ch == ',' && d == 0) {
                    alts.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(ch);
                }
            }
            alts.push_back(cur);
            const std::string head = p.substr(0, open), tail = p.substr(i + 1);
            for (const std::string& a : alts) expandBraces(head + a + tail, out);
            return;
        }
    }
    out->push_back(p);
}

std::string collapseGlobstars(std::string p) {
    for (std::size_t at; (at = p.find("**/**")) != std::string::npos;) p.erase(at, 3);
    return p;
}

// "/a/./b/../c" -> "/a/c".
std::string normalizePath(const std::string& path) {
    std::vector<std::string> parts;
    std::size_t i = 0;
    while (i <= path.size()) {
        std::size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        const std::string seg = path.substr(i, j - i);
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!seg.empty() && seg != ".") {
            parts.push_back(seg);
        }
        i = j + 1;
    }
    std::string out;
    for (const std::string& s : parts) out += "/" + s;
    return out.empty() ? "/" : out;
}

}  // namespace

bool globMatch(const std::string& pattern, const std::string& path) {
    return globAt(pattern, 0, path, 0);
}

std::vector<std::string> splitGlobList(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    int braces = 0;
    bool inClass = false;
    auto flush = [&] {
        std::size_t a = cur.find_first_not_of(" \t\r\n");
        std::size_t b = cur.find_last_not_of(" \t\r\n");
        if (a != std::string::npos) out.push_back(cur.substr(a, b - a + 1));
        cur.clear();
    };
    for (char c : text) {
        if (c == '[' && !inClass) inClass = true;
        else if (c == ']' && inClass) inClass = false;
        else if (c == '{' && !inClass) ++braces;
        else if (c == '}' && !inClass && braces > 0) --braces;
        if (c == ',' && braces == 0 && !inClass) {
            flush();
            continue;
        }
        cur.push_back(c);
    }
    flush();
    return out;
}

PathFilter::PathFilter(const std::string& text, const std::string& root) {
    std::string base = root;
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    base = normalizePath(base);
    auto add = [&](const std::string& glob) {
        std::vector<std::string> alts;
        expandBraces(collapseGlobstars(glob), &alts);
        for (const std::string& a : alts) globs_.push_back(a);
    };
    for (std::string p : splitGlobList(text)) {
        for (char& c : p)
            if (c == '\\') c = '/';
        if (p[0] == '~' && (p.size() == 1 || p[1] == '/')) {
            const char* home = std::getenv("HOME");
            if (home) p = std::string(home) + p.substr(1);
        }
        const bool searchPath = p[0] == '/' || p == "." || p == ".." ||
                                p.rfind("./", 0) == 0 || p.rfind("../", 0) == 0;
        if (searchPath) {
            const std::string full = normalizePath(p[0] == '/' ? p : base + "/" + p);
            std::string rel;
            if (full == base) {
                add("**");
                continue;
            }
            const std::string prefix = base == "/" ? "/" : base + "/";
            if (full.compare(0, prefix.size(), prefix) != 0) {
                // Outside the folder: a glob no relative path can match.
                globs_.push_back(std::string(1, '\0'));
                continue;
            }
            rel = full.substr(prefix.size());
            add(rel);
            if (rel.size() < 2 || rel.compare(rel.size() - 2, 2, "**") != 0) add(rel + "/**");
            continue;
        }
        while (!p.empty() && p.back() == '/') p.pop_back();
        if (p.empty()) continue;
        if (p[0] == '.') p = "*" + p;   // ".js" is "*.js"
        add("**/" + p + "/**");
        add("**/" + p);
    }
}

bool PathFilter::matches(const std::string& relativePath) const {
    for (const std::string& g : globs_)
        if (globMatch(g, relativePath)) return true;
    return false;
}

}  // namespace FolderSearch
