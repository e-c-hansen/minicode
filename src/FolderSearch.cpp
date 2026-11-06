// FolderSearch.cpp — see FolderSearch.h.
#include "FolderSearch.h"
#include "MarkdownTasks.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <memory>
#include <filesystem>
#include <fstream>
#include <set>
#include <system_error>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

struct Walker {
    const std::string& query;
    const std::atomic<bool>* cancel;
    const FolderSearchOptions& opt;
    FolderSearchResult& out;
    std::set<std::string> visited;   // canonical folders already walked
    bool todos = false;              // the TODO list, not a query
    const TextFind::Query* tq = nullptr;   // whole word or regex: TextFind matches
    FolderReplacePlan* plan = nullptr;     // counting for Replace in Folder
    // Counts the matches on a line with a hit, as the plan counts them.
    const TextFind::Query* counter = nullptr;
    // Replace in Folder: the files already counted, by device and inode, so
    // a file with two names is replaced once; and the real path of the
    // folder searched, to tell a file reached through a symlink to outside.
    std::set<std::pair<std::uint64_t, std::uint64_t>> files = {};
    std::string rootCanon = {};
    // Files to include and exclude (the options' boxes).
    PathFilter include = {}, exclude = {};

    void setFilters(const std::string& root) {
        include = PathFilter(opt.include, root);
        exclude = PathFilter(opt.exclude, root);
    }
    bool wanted(const std::string& rel) const {
        if (!exclude.empty() && exclude.matches(rel)) return false;
        return include.empty() || include.matches(rel);
    }

    bool full() const { return !plan && out.matches.size() >= opt.maxMatches; }

    bool stop() {
        if (cancelled(cancel)) { out.cancelled = true; return true; }
        if (full()) { out.truncated = true; return true; }
        return false;
    }

    void searchFile(const std::string& path, const std::string& rel,
                    std::uintmax_t size) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return;
        std::string content;
        // Read at most one byte past the limit, in case it grew since stat.
        content.resize(static_cast<std::size_t>(size) + 1);
        in.read(&content[0], static_cast<std::streamsize>(content.size()));
        content.resize(static_cast<std::size_t>(in.gcount()));
        if (content.size() > opt.maxFileSize) return;
        if (!isValidUtf8(content)) return;   // binary, or another encoding
        ++out.filesSearched;

        if (plan) {   // Replace in Folder's count: every match
            std::size_t count = 0;
            std::size_t pos = 0, lines = 0;
            const std::size_t n = content.size();
            while (pos <= n) {
                if ((++lines & 4095) == 0 && cancelled(cancel)) { out.cancelled = true; break; }
                std::size_t end = pos;
                while (end < n && content[end] != '\n' && content[end] != '\r' &&
                       !(content[end] == '\xE2' && end + 2 < n && content[end + 1] == '\x80' &&
                         content[end + 2] == '\xA9'))
                    ++end;
                count += TextFind::findAllInLine(content.substr(pos, end - pos), *tq).size();
                if (end >= n) break;
                pos = end + (content[end] == '\xE2' ? 3
                             : (content[end] == '\r' && end + 1 < n && content[end + 1] == '\n') ? 2
                                                                                                 : 1);
            }
            if (count) {
                FolderReplaceFile f{path, rel, count, false};
                char resolved[PATH_MAX];
                if (::realpath(path.c_str(), resolved) && !rootCanon.empty()) {
                    const std::string real = resolved;
                    f.outside = !(real.size() > rootCanon.size() &&
                                  real.compare(0, rootCanon.size(), rootCanon) == 0 &&
                                  (rootCanon.back() == '/' || real[rootCanon.size()] == '/'));
                }
                plan->files.push_back(std::move(f));
                plan->matches += count;
            }
            return;
        }

        const std::size_t before = out.matches.size();
        // For the TODO list, a Markdown file's open tasks, by line.
        const bool markdown = todos && isMarkdownPath(path);
        std::vector<MarkdownTasks::OpenTask> tasks;
        if (markdown) tasks = MarkdownTasks::openTasks(content);
        std::size_t nextTask = 0;
        int lineNo = 0;
        std::size_t pos = 0;
        const std::size_t n = content.size();
        while (pos < n) {
            // Find the end of this line and the start of the next.
            std::size_t end = pos, next = n;
            for (; end < n; ++end) {
                const char c = content[end];
                if (c == '\n') { next = end + 1; break; }
                if (c == '\r') {
                    next = (end + 1 < n && content[end + 1] == '\n') ? end + 2 : end + 1;
                    break;
                }
                if (c == '\xE2' && end + 2 < n && content[end + 1] == '\x80' &&
                    content[end + 2] == '\xA9') {   // U+2029 paragraph separator
                    next = end + 3;
                    break;
                }
            }
            ++lineNo;
            if ((lineNo & 4095) == 0 && cancelled(cancel)) {
                out.cancelled = true;
                break;
            }
            const std::string line = content.substr(pos, end - pos);
            std::size_t col = 0, len = 0;
            bool hit;
            if (todos) {
                while (nextTask < tasks.size() && tasks[nextTask].line < lineNo - 1) nextTask++;
                hit = findTodoInLine(line, markdown, &col, &len);
                if (!hit && nextTask < tasks.size() && tasks[nextTask].line == lineNo - 1) {
                    col = tasks[nextTask].column;
                    len = 3;
                    hit = col + len <= line.size();
                }
            } else if (tq) {
                hit = TextFind::findInLine(line, *tq, &col, &len);
            } else {
                hit = findInLine(line, query, opt.caseSensitive, &col, &len);
            }
            if (hit) {
                FolderSearchMatch m;
                m.path = path;
                m.relativePath = rel;
                m.line = lineNo;
                m.byteColumn = col;
                m.byteLength = len;
                m.column = static_cast<int>(countChars(line.substr(0, col))) + 1;
                m.text = displayText(line, opt.maxLineChars);
                if (counter)
                    m.count = std::max<std::size_t>(1, TextFind::findAllInLine(line, *counter).size());
                out.totalMatches += m.count;
                out.matches.push_back(std::move(m));
                if (full()) { out.truncated = true; break; }
            }
            pos = next;
        }
        if (out.matches.size() > before) ++out.filesMatched;
    }

    // The ignore files' view: the scope's path from their top, and whether
    // they are read at all.
    std::string ignorePrefix = {};
    bool useIgnore = false;

    std::string ignorePath(const std::string& rel) const {
        return ignorePrefix.empty() ? rel : ignorePrefix + "/" + rel;
    }

    // Start at root, with the ignore files the options name.
    void start(const std::string& root) {
        Ignore::Rules rules;
        useIgnore = opt.useIgnoreFiles;
        if (useIgnore) rules = Ignore::rulesFor(opt.ignoreTop, root, &ignorePrefix);
        walk(root, "", rules);
    }

    void walk(const std::string& dir, const std::string& rel, const Ignore::Rules& rules) {
        std::error_code ec;
        std::vector<std::string> names;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) return;
        for (fs::directory_iterator end; it != end; it.increment(ec)) {
            if (ec) break;
            std::string name = it->path().filename().string();
            if (name.empty() || isSkippedFile(name)) continue;
            names.push_back(std::move(name));
            if (cancelled(cancel)) { out.cancelled = true; return; }
        }
        std::sort(names.begin(), names.end());

        std::vector<std::string> subdirs;
        for (const std::string& name : names) {
            if (stop()) return;
            const std::string full = joinPath(dir, name);
            const std::string childRel = rel.empty() ? name : rel + "/" + name;
            // status() follows symlinks, as the Mac's fileExistsAtPath does.
            const fs::file_status st = fs::status(full, ec);
            if (ec) continue;   // broken symlink, vanished, unreadable
            const bool isDir = fs::is_directory(st);
            if (useIgnore && !rules.empty() && rules.ignored(ignorePath(childRel), isDir))
                continue;
            if (isDir) {
                if (!isSkippedDirectory(name) && !(!exclude.empty() && exclude.matches(childRel)))
                    subdirs.push_back(name);
                continue;
            }
            if (!fs::is_regular_file(st)) continue;   // pipes, sockets, devices
            if (!wanted(childRel)) continue;
            const std::uintmax_t size = fs::file_size(full, ec);
            if (ec || size > opt.maxFileSize) continue;
            if (plan) {   // one file under two names is replaced once
                struct stat fst;
                if (::stat(full.c_str(), &fst) != 0) continue;
                if (!files.insert({static_cast<std::uint64_t>(fst.st_dev),
                                   static_cast<std::uint64_t>(fst.st_ino)}).second)
                    continue;
            }
            searchFile(full, childRel, size);
        }
        for (const std::string& name : subdirs) {
            if (stop()) return;
            const std::string full = joinPath(dir, name);
            const fs::path canon = fs::canonical(full, ec);
            if (ec) continue;
            if (!visited.insert(canon.string()).second) continue;   // loop / seen
            const std::string childRel = rel.empty() ? name : rel + "/" + name;
            walk(full, childRel, useIgnore ? rules.enter(full, ignorePath(childRel)) : rules);
        }
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
    Walker w{q, cancel, options, out, {}};
    if (tq.usable()) w.tq = &tq;
    // A row is a line; its count is every match on it, as Replace in
    // Folder's plan counts them, so the two say the same number.
    const TextFind::Query counter = tq.usable() ? tq : makeQuery(query, options);
    if (counter.usable()) w.counter = &counter;
    const fs::path canon = fs::canonical(root, ec);
    if (!ec) w.visited.insert(canon.string());
    w.setFilters(root);
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
    Walker w{none, cancel, options, out, {}};
    w.tq = &tq;
    w.plan = &plan;
    const fs::path canon = fs::canonical(root, ec);
    if (!ec) {
        w.visited.insert(canon.string());
        w.rootCanon = canon.string();
    }
    w.setFilters(root);
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
    Walker w{none, cancel, options, out, {}, true};
    const fs::path canon = fs::canonical(root, ec);
    if (!ec) w.visited.insert(canon.string());
    w.setFilters(root);
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
