// TermLinks.cpp — see TermLinks.h.
#include "TermLinks.h"

#include <cctype>

namespace TermLinks {
namespace {

bool isSpace(char c) { return c == ' ' || c == '\t'; }

// Characters a path is made of. Anything outside ASCII counts too, so a
// path with accented or CJK names is found whole.
bool isPathChar(char c) {
    const auto u = static_cast<unsigned char>(c);
    return u >= 0x80 || std::isalnum(u) || c == '/' || c == '.' || c == '_' ||
           c == '-' || c == '~' || c == '+' || c == '@' || c == '\\';
}

// Punctuation that ends a sentence rather than a URL or path.
bool isTrailing(char c) {
    return c == '.' || c == ',' || c == ';' || c == ':' || c == '!' ||
           c == '?' || c == '\'' || c == '"' || c == ')' || c == ']' ||
           c == '>' || c == '}';
}

// Reads 1-based digits at `i`; returns the number and moves `i` past them,
// or 0 with `i` unchanged when there are none.
int readNumber(const std::string& s, size_t& i) {
    size_t j = i;
    long n = 0;
    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])) &&
           j - i < 9)
        n = n * 10 + (s[j++] - '0');
    if (j == i) return 0;
    i = j;
    return static_cast<int>(n);
}

// Whether a word looks like a file rather than a plain word: it has a
// slash, or a dot followed by an extension-like tail ("main.cpp", not "e.g"
// at a sentence end, which the caller trims). A bare word is never a file.
bool looksLikePath(const std::string& p) {
    if (p.empty() || p == "." || p == "..") return false;
    if (p.find('/') != std::string::npos) {
        // A lone slash or a run of dots and slashes is not a file.
        for (char c : p)
            if (std::isalnum(static_cast<unsigned char>(c))) return true;
        return false;
    }
    const size_t dot = p.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= p.size()) return false;
    for (size_t k = dot + 1; k < p.size(); k++)
        if (!std::isalnum(static_cast<unsigned char>(p[k]))) return false;
    // "1.5" and "v2.0" are numbers, not files.
    bool letter = false;
    for (size_t k = dot + 1; k < p.size(); k++)
        if (std::isalpha(static_cast<unsigned char>(p[k]))) letter = true;
    return letter;
}

size_t urlEnd(const std::string& s, size_t i) {
    size_t j = i;
    int parens = 0;
    while (j < s.size()) {
        const char c = s[j];
        if (isSpace(c) || c == '"' || c == '<' || c == '>' || c == '`') break;
        if (c == '(') parens++;
        if (c == ')' && parens-- == 0) break;
        j++;
    }
    while (j > i && isTrailing(s[j - 1]) && !(s[j - 1] == ')' && parens < 0))
        j--;
    return j;
}

}  // namespace

std::vector<Link> find(const std::string& s) {
    std::vector<Link> out;
    size_t i = 0;
    while (i < s.size()) {
        // URLs first: they contain colons and slashes a path would claim.
        if ((s.compare(i, 7, "http://") == 0 || s.compare(i, 8, "https://") == 0 ||
             s.compare(i, 7, "file://") == 0) &&
            (i == 0 || !isPathChar(s[i - 1]))) {
            const size_t end = urlEnd(s, i);
            const size_t scheme = s.find("://", i) + 3;
            if (end > scheme) {
                out.push_back({Link::Url, i, end - i, s.substr(i, end - i)});
                i = end;
                continue;
            }
        }

        // Python: File "path", line N
        if (s.compare(i, 6, "File \"") == 0) {
            const size_t open = i + 6;
            const size_t close = s.find('"', open);
            if (close != std::string::npos && s.compare(close, 8, "\", line ") == 0) {
                size_t k = close + 8;
                const int line = readNumber(s, k);
                if (line > 0) {
                    out.push_back({Link::File, open, close - open,
                                   s.substr(open, close - open), line, 0});
                    i = k;
                    continue;
                }
            }
        }

        if (!isPathChar(s[i]) || (i > 0 && isPathChar(s[i - 1]))) {
            i++;
            continue;
        }

        // A word of path characters, then an optional :line[:col] or
        // (line[,col]) right after it.
        size_t j = i;
        while (j < s.size() && isPathChar(s[j])) j++;
        size_t pathEnd = j;
        // Trailing dots belong to the sentence ("see main.cpp.").
        while (pathEnd > i && s[pathEnd - 1] == '.') pathEnd--;
        std::string path = s.substr(i, pathEnd - i);

        int line = 0, column = 0;
        size_t end = pathEnd;
        if (pathEnd == j && j < s.size() && s[j] == ':') {
            size_t k = j + 1;
            line = readNumber(s, k);
            if (line > 0) {
                end = k;
                if (k < s.size() && s[k] == ':') {
                    size_t m = k + 1;
                    const int col = readNumber(s, m);
                    if (col > 0) { column = col; end = m; }
                }
            }
        } else if (pathEnd == j && j < s.size() && s[j] == '(') {
            size_t k = j + 1;
            const int l = readNumber(s, k);
            if (l > 0) {
                int c = 0;
                if (k < s.size() && s[k] == ',') {
                    size_t m = k + 1;
                    c = readNumber(s, m);
                    if (c > 0) k = m;
                }
                if (k < s.size() && s[k] == ')') {
                    line = l;
                    column = c;
                    end = k + 1;
                }
            }
        }

        // False positives ("e.g.") are cheap: a port shows a link only
        // when the path names a file that exists.
        if (looksLikePath(path))
            out.push_back({Link::File, i, end - i, path, line, column});
        i = end > j ? end : j;
    }
    return out;
}

const Link* at(const std::vector<Link>& links, size_t offset) {
    for (const Link& l : links)
        if (offset >= l.start && offset < l.start + l.length) return &l;
    return nullptr;
}

}  // namespace TermLinks
