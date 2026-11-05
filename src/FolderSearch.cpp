// FolderSearch.cpp — see FolderSearch.h.
#include "FolderSearch.h"
#include "MarkdownTasks.h"

#include <algorithm>
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
                plan->files.push_back({path, rel, count});
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
                out.matches.push_back(std::move(m));
                if (full()) { out.truncated = true; break; }
            }
            pos = next;
        }
        if (out.matches.size() > before) ++out.filesMatched;
    }

    void walk(const std::string& dir, const std::string& rel) {
        std::error_code ec;
        std::vector<std::string> names;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) return;
        for (fs::directory_iterator end; it != end; it.increment(ec)) {
            if (ec) break;
            std::string name = it->path().filename().string();
            if (name.empty() || name[0] == '.') continue;   // hidden
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
            if (fs::is_directory(st)) {
                if (!isSkippedDirectory(name)) subdirs.push_back(name);
                continue;
            }
            if (!fs::is_regular_file(st)) continue;   // pipes, sockets, devices
            const std::uintmax_t size = fs::file_size(full, ec);
            if (ec || size > opt.maxFileSize) continue;
            searchFile(full, childRel, size);
        }
        for (const std::string& name : subdirs) {
            if (stop()) return;
            const std::string full = joinPath(dir, name);
            const fs::path canon = fs::canonical(full, ec);
            if (ec) continue;
            if (!visited.insert(canon.string()).second) continue;   // loop / seen
            walk(full, rel.empty() ? name : rel + "/" + name);
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
    static const char* const kSkip[] = {".git", "node_modules", "build", ".build",
                                        "DerivedData", "dist", ".venv", "venv",
                                        "__pycache__"};
    for (const char* s : kSkip) if (name == s) return true;
    return false;
}

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
    const fs::path canon = fs::canonical(root, ec);
    if (!ec) w.visited.insert(canon.string());
    w.walk(root, "");
    return out;
}

TextFind::Query makeQuery(const std::string& query, const FolderSearchOptions& options) {
    TextFind::Options o;
    o.caseSensitive = options.caseSensitive;
    o.wholeWord = options.wholeWord;
    o.regex = options.regex;
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
    if (!ec) w.visited.insert(canon.string());
    w.walk(root, "");
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

bool replaceMatchInText(const std::string& content, const std::string& query,
                        const std::string& replacement, const FolderSearchOptions& options,
                        int line, std::size_t byteColumn, std::string* out) {
    if (!isSearchable(query, options)) return false;
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

}  // namespace

bool writeFileAtomically(const std::string& path, const std::string& content,
                         std::string* error) {
    // The file a symlink names, so the link stays a link.
    char resolved[PATH_MAX];
    const std::string target = ::realpath(path.c_str(), resolved) ? std::string(resolved) : path;
    struct stat st;
    const bool exists = ::stat(target.c_str(), &st) == 0;
    if (exists && !S_ISREG(st.st_mode)) {
        if (error) *error = "not a regular file";
        return false;
    }
    if (exists && st.st_nlink > 1) {
        // Hard links: a rename would part this name from the others.
        const int fd = ::open(target.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
        if (fd < 0) return fail(error, "could not open the file");
        const bool ok = writeAll(fd, content) && ::fsync(fd) == 0;
        const int saved = errno;
        ::close(fd);
        errno = saved;
        return ok || fail(error, "could not write the file");
    }
    const std::size_t slash = target.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : target.substr(0, slash + 1);
    const std::string base = slash == std::string::npos ? target : target.substr(slash + 1);
    std::string tmp = dir + "." + base + ".minicode-XXXXXX";
    std::vector<char> name(tmp.begin(), tmp.end());
    name.push_back('\0');
    const int fd = ::mkstemp(name.data());
    if (fd < 0) return fail(error, "could not make a temporary file beside it");
    tmp = name.data();
    bool ok = true;
    if (exists) {
        ok = ::fchmod(fd, st.st_mode & 07777) == 0;
        if (ok && ::fchown(fd, st.st_uid, st.st_gid) != 0) {}   // only root may give it away
    } else {
        const mode_t mask = ::umask(0);
        ::umask(mask);
        ok = ::fchmod(fd, 0666 & ~mask) == 0;
    }
    ok = ok && writeAll(fd, content) && ::fsync(fd) == 0;
    int saved = errno;
    ::close(fd);
    if (ok && ::rename(tmp.c_str(), target.c_str()) == 0) return true;
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
    w.walk(root, "");
    return out;
}

}  // namespace FolderSearch
