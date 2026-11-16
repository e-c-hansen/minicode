// Runnables.cpp — see Runnables.h.
#include "Runnables.h"

#include "RunFile.h"
#include "SyntaxHighlighter.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <sys/stat.h>

namespace Runnables {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool endsWith(const std::string& s, const std::string& end) {
    return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
}

bool startsWith(const std::string& s, const std::string& start) {
    return s.compare(0, start.size(), start) == 0;
}

std::string baseName(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string parentOf(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return "";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

bool identChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || (unsigned char)c >= 0x80;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

// The text with what is in comments and strings blanked out (line breaks
// kept, so lines and offsets stay), and the strings' spans, for a name.
struct Masked {
    std::string code;
    std::vector<std::pair<size_t, size_t>> strings;   // [start, end), quotes included
    std::vector<size_t> lineStarts;
    size_t lineOf(size_t at) const {
        auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), at);
        return (size_t)(it - lineStarts.begin()) - 1;
    }
};

Masked mask(const std::string& text, const char* ext) {
    Masked m;
    m.code = text;
    // Line starts, as LineIndex counts them.
    m.lineStarts.push_back(0);
    for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (c == '\n') m.lineStarts.push_back(i + 1);
        else if (c == '\r' && (i + 1 >= text.size() || text[i + 1] != '\n')) m.lineStarts.push_back(i + 1);
        else if ((unsigned char)c == 0xE2 && i + 2 < text.size() && (unsigned char)text[i + 1] == 0x80 &&
                 (unsigned char)text[i + 2] == 0xA9) {
            m.lineStarts.push_back(i + 3);
            i += 2;
        }
    }
    const std::vector<Token> tokens = SyntaxHighlighter::highlight(text, ext);
    for (size_t k = 0; k < tokens.size(); k++) {
        const Token& t = tokens[k];
        const bool str = isStringStyle(t.style);
        if (!str && t.style != TokenStyle::Comment) continue;
        size_t start = t.start, end = t.start + t.length;
        if (str) {
            // A string's escapes are tokens of their own: one span.
            while (k + 1 < tokens.size() && isStringStyle(tokens[k + 1].style) &&
                   tokens[k + 1].start == end) {
                end = tokens[k + 1].start + tokens[k + 1].length;
                k++;
            }
            m.strings.push_back({start, end});
        }
        for (size_t i = start; i < end && i < m.code.size(); i++)
            if (m.code[i] != '\n' && m.code[i] != '\r') m.code[i] = ' ';
    }
    return m;
}

// The string whose span starts at `at`, without its quotes and with a
// backslash's escape taken as the character; false for none, or for a
// template literal with a ${} field in it.
bool stringAt(const Masked& m, const std::string& text, size_t at, std::string* out) {
    auto it = std::lower_bound(m.strings.begin(), m.strings.end(), std::make_pair(at, (size_t)0));
    if (it == m.strings.end() || it->first != at) return false;
    const size_t end = it->second;
    if (end < at + 2) return false;
    const char q = text[at];
    if ((q != '\'' && q != '"' && q != '`') || text[end - 1] != q) return false;
    std::string s = text.substr(at + 1, end - at - 2);
    if (q == '`' && s.find("${") != std::string::npos) return false;
    std::string name;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char e = s[++i];
            name += e == 'n' ? '\n' : e == 't' ? '\t' : e;
            continue;
        }
        name += s[i];
    }
    *out = name;
    return true;
}

std::string identAt(const std::string& s, size_t i) {
    size_t j = i;
    while (j < s.size() && identChar(s[j])) j++;
    return s.substr(i, j - i);
}

size_t skipSpace(const std::string& s, size_t i) {
    while (i < s.size() && isSpace(s[i])) i++;
    return i;
}

// ---- pytest
std::vector<Test> findPython(const std::string& text) {
    const Masked m = mask(text, "py");
    const std::string& code = m.code;
    struct Scope {
        size_t indent;
        bool isClass, testClass;
        std::string name;
    };
    std::vector<Scope> scopes;
    std::vector<Test> out;
    int depth = 0;   // open brackets: a line inside them continues a statement
    for (size_t line = 0; line < m.lineStarts.size(); line++) {
        const size_t start = m.lineStarts[line];
        const size_t end = line + 1 < m.lineStarts.size() ? m.lineStarts[line + 1] : code.size();
        const std::string row = code.substr(start, end - start);
        const bool continued = depth > 0;
        for (char c : row) {
            if (c == '(' || c == '[' || c == '{') depth++;
            else if ((c == ')' || c == ']' || c == '}') && depth > 0) depth--;
        }
        if (continued) continue;
        size_t indent = 0, i = 0;
        for (; i < row.size() && (row[i] == ' ' || row[i] == '\t'); i++)
            indent = row[i] == '\t' ? (indent / 8 + 1) * 8 : indent + 1;
        if (i >= row.size() || row[i] == '\n' || row[i] == '\r' || row[i] == '\\') continue;
        if (row[i] == '@') continue;   // a decorator: the def follows
        while (!scopes.empty() && indent <= scopes.back().indent) scopes.pop_back();
        std::string word = identAt(row, i);
        size_t after = i + word.size();
        if (word == "async") {
            size_t j = after;
            while (j < row.size() && (row[j] == ' ' || row[j] == '\t')) j++;
            if (identAt(row, j) == "def") { word = "def"; after = j + 3; }
        }
        if (word != "def" && word != "class") continue;
        size_t j = after;
        if (j >= row.size() || (row[j] != ' ' && row[j] != '\t')) continue;
        while (j < row.size() && (row[j] == ' ' || row[j] == '\t')) j++;
        const std::string name = identAt(row, j);
        if (name.empty()) continue;
        bool inTestClasses = true;   // every scope around is a test class
        for (const Scope& s : scopes)
            if (!s.isClass || !s.testClass) inTestClasses = false;
        if (word == "class") {
            const size_t open = row.find('(', j + name.size());
            const bool testCase =
                open != std::string::npos && row.find("TestCase", open) != std::string::npos;
            scopes.push_back({indent, true, inTestClasses && (startsWith(name, "Test") || testCase), name});
            continue;
        }
        if (startsWith(name, "test") && inTestClasses) {
            Test t;
            t.line = line;
            t.name = name;
            for (const Scope& s : scopes) t.scope.push_back(s.name);
            out.push_back(t);
        }
        scopes.push_back({indent, false, false, name});
    }
    return out;
}

// ---- go test
std::vector<Test> findGo(const std::string& text) {
    const Masked m = mask(text, "go");
    const std::string& code = m.code;
    std::vector<Test> out;
    for (size_t line = 0; line < m.lineStarts.size(); line++) {
        const size_t start = m.lineStarts[line];
        if (code.compare(start, 5, "func ") != 0) continue;
        size_t i = skipSpace(code, start + 5);
        const std::string name = identAt(code, i);
        // Test, then nothing or anything but a lowercase letter.
        if (!startsWith(name, "Test") || (name.size() > 4 && name[4] >= 'a' && name[4] <= 'z'))
            continue;
        i = skipSpace(code, i + name.size());
        if (i >= code.size() || code[i] != '(') continue;   // generics, a method
        i = skipSpace(code, i + 1);
        const std::string param = identAt(code, i);
        if (param.empty()) continue;
        i = skipSpace(code, i + param.size());
        if (code.compare(i, 1, "*") != 0) continue;
        i = skipSpace(code, i + 1);
        if (code.compare(i, 9, "testing.T") != 0) continue;
        i = skipSpace(code, i + 9);
        if (i >= code.size() || code[i] != ')') continue;
        Test t;
        t.line = line;
        t.name = name;
        out.push_back(t);
    }
    return out;
}

// ---- cargo test
std::vector<Test> findRust(const std::string& text) {
    const Masked m = mask(text, "rs");
    const std::string& code = m.code;
    struct Mod { std::string name; int depth; };   // depth inside its braces
    std::vector<Mod> mods;
    std::vector<Test> out;
    int depth = 0;
    bool pending = false;   // a test attribute waits for its fn
    std::string pendingMod;  // `mod name` waiting for its {
    size_t i = 0;
    while (i < code.size()) {
        const char c = code[i];
        if (c == '#') {
            size_t j = skipSpace(code, i + 1);
            const bool inner = j < code.size() && code[j] == '!';
            if (inner) j = skipSpace(code, j + 1);
            if (j < code.size() && code[j] == '[') {
                // To the matching ].
                int d = 0;
                size_t k = j;
                for (; k < code.size(); k++) {
                    if (code[k] == '[') d++;
                    else if (code[k] == ']' && --d == 0) break;
                }
                std::string attr = code.substr(j + 1, (k < code.size() ? k : code.size()) - j - 1);
                const size_t paren = attr.find('(');
                if (paren != std::string::npos) attr = attr.substr(0, paren);
                std::string path;
                for (char a : attr)
                    if (!isSpace(a)) path += a;
                if (!inner && (path == "test" || endsWith(path, "::test"))) pending = true;
                i = k + 1;
                continue;
            }
        }
        if (identChar(c) && (i == 0 || !identChar(code[i - 1]))) {
            const std::string word = identAt(code, i);
            size_t after = i + word.size();
            if (word == "fn" || word == "mod") {
                const size_t j = skipSpace(code, after);
                const std::string name = identAt(code, j);
                if (word == "fn") {
                    if (pending && !name.empty()) {
                        Test t;
                        t.line = m.lineOf(j);
                        t.name = name;
                        for (const Mod& md : mods) t.scope.push_back(md.name);
                        out.push_back(t);
                    }
                    pending = false;
                } else if (!name.empty()) {
                    pendingMod = name;
                }
                after = j + name.size();
            }
            i = after;
            continue;
        }
        if (c == '{') {
            depth++;
            if (!pendingMod.empty()) mods.push_back({pendingMod, depth});
            pendingMod.clear();
            pending = false;
        } else if (c == '}') {
            while (!mods.empty() && mods.back().depth >= depth) mods.pop_back();
            if (depth > 0) depth--;
            pending = false;
            pendingMod.clear();
        } else if (c == ';') {
            pending = false;
            pendingMod.clear();
        }
        i++;
    }
    return out;
}

// ---- Jest and Vitest
std::vector<Test> findJs(const std::string& text) {
    const Masked m = mask(text, "js");
    const std::string& code = m.code;
    struct Describe { std::string name; int openDepth; };
    std::vector<Describe> describes;
    std::vector<Test> out;
    int depth = 0;
    size_t i = 0;
    while (i < code.size()) {
        const char c = code[i];
        if (c == '(' || c == '[' || c == '{') {
            depth++;
            i++;
            continue;
        }
        if (c == ')' || c == ']' || c == '}') {
            if (depth > 0) depth--;
            while (!describes.empty() && describes.back().openDepth > depth) describes.pop_back();
            i++;
            continue;
        }
        if (!(identChar(c) || c == '$') || (i > 0 && (identChar(code[i - 1]) || code[i - 1] == '$' ||
                                                      code[i - 1] == '.'))) {
            i++;
            continue;
        }
        const std::string word = identAt(code, i);
        size_t j = i + word.size();
        if (word.empty()) { i++; continue; }
        if (word != "describe" && word != "test" && word != "it") {
            i = j;
            continue;
        }
        // .only, .skip, .concurrent, .sequential, .fails
        size_t k = skipSpace(code, j);
        while (k < code.size() && code[k] == '.') {
            const size_t w = skipSpace(code, k + 1);
            const std::string mod = identAt(code, w);
            if (mod != "only" && mod != "skip" && mod != "concurrent" && mod != "sequential" &&
                mod != "fails")
                break;
            k = skipSpace(code, w + mod.size());
        }
        if (k >= code.size() || code[k] != '(') {
            i = j;
            continue;
        }
        const size_t nameAt = skipSpace(text, k + 1);   // the name is blank in code
        std::string name;
        if (!stringAt(m, text, nameAt, &name)) {
            i = j;
            continue;
        }
        if (word == "describe") {
            // Open until the call's ( closes: the depth inside it.
            describes.push_back({name, depth + 1});
        } else {
            Test t;
            t.line = m.lineOf(i);
            t.name = name;
            for (const Describe& d : describes) t.scope.push_back(d.name);
            out.push_back(t);
        }
        i = k;   // the ( is counted
    }
    return out;
}

bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// The nearest folder from the file's up, to root when the file is inside
// it, holding one of `markers`; else root, or the file's own folder.
std::string projectDir(const std::string& file, const std::string& root,
                       const std::vector<const char*>& markers,
                       const std::function<bool(const std::string&)>& exists) {
    std::string r = root;
    while (r.size() > 1 && r.back() == '/') r.pop_back();
    const bool inside = !r.empty() && startsWith(file, r == "/" ? "/" : r + "/");
    std::string dir = parentOf(file);
    while (!dir.empty()) {
        for (const char* mk : markers)
            if (exists(dir == "/" ? std::string("/") + mk : dir + "/" + mk)) return dir;
        if ((inside && dir == r) || dir == "/") break;
        dir = parentOf(dir);
    }
    return inside ? r : parentOf(file);
}

std::string relativeTo(const std::string& path, const std::string& base) {
    if (base == "/") return path.size() > 1 ? path.substr(1) : path;
    if (startsWith(path, base + "/")) return path.substr(base.size() + 1);
    return path;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (const std::string& p : parts) {
        if (p.empty()) continue;
        if (!out.empty()) out += sep;
        out += p;
    }
    return out;
}

}  // namespace

Kind kindFor(const std::string& path) {
    const std::string name = baseName(path), l = lower(name);
    const size_t dot = l.find_last_of('.');
    const std::string ext = dot == std::string::npos ? "" : l.substr(dot + 1);
    if (ext == "py")
        return startsWith(l, "test_") || endsWith(l, "_test.py") || l == "tests.py" ? Kind::Pytest
                                                                                     : Kind::None;
    if (ext == "go") return endsWith(l, "_test.go") ? Kind::Go : Kind::None;
    if (ext == "rs") return Kind::Rust;
    static const char* js[] = {"js", "jsx", "ts", "tsx", "mjs", "cjs", "mts", "cts"};
    for (const char* e : js) {
        if (ext != e) continue;
        const std::string stem = l.substr(0, dot);
        if (endsWith(stem, ".test") || endsWith(stem, ".spec") ||
            path.find("/__tests__/") != std::string::npos)
            return Kind::Js;
        return Kind::None;
    }
    return Kind::None;
}

std::string languageOf(Kind kind) {
    switch (kind) {
        case Kind::Pytest: return "python";
        case Kind::Go: return "go";
        case Kind::Rust: return "rust";
        case Kind::Js: return "js";
        case Kind::None: break;
    }
    return "";
}

std::string Test::label(Kind kind) const {
    std::vector<std::string> parts = scope;
    parts.push_back(name);
    return join(parts, kind == Kind::Js ? " > " : "::");
}

std::vector<Test> find(const std::string& text, Kind kind) {
    switch (kind) {
        case Kind::Pytest: return findPython(text);
        case Kind::Go: return findGo(text);
        case Kind::Rust: return findRust(text);
        case Kind::Js: return findJs(text);
        case Kind::None: break;
    }
    return {};
}

std::string defaultTemplate(Kind kind) {
    switch (kind) {
        case Kind::Pytest: return "{python} -m pytest {tests}";
        case Kind::Go: return "go test -run {pattern} {package}";
        case Kind::Rust: return "cargo test {target} -- --exact {tests}";
        case Kind::Js: return "{runner} {path} {filter}";
        case Kind::None: break;
    }
    return "";
}

std::string regexEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (std::string("\\^$.|?*+()[]{}/").find(c) != std::string::npos) out += '\\';
        out += c;
    }
    return out;
}

std::string expand(const std::string& tmpl,
                   const std::vector<std::pair<std::string, std::string>>& scalars,
                   const std::vector<std::pair<std::string, std::vector<std::string>>>& lists) {
    std::string out;
    char quote = 0;   // the quote the template has open here, as RunFile::expand
    size_t i = 0;
    while (i < tmpl.size()) {
        const char c = tmpl[i];
        if (c == '{') {
            const size_t close = tmpl.find('}', i + 1);
            if (close != std::string::npos) {
                const std::string key = tmpl.substr(i + 1, close - i - 1);
                bool done = false;
                for (const auto& s : scalars) {
                    if (s.first != key) continue;
                    out += quote ? RunFile::quoteInside(s.second, quote) : RunFile::shellQuote(s.second);
                    done = true;
                    break;
                }
                for (const auto& l : lists) {
                    if (done || l.first != key) continue;
                    if (l.second.empty()) {
                        // Nothing: the placeholder goes with the space before it.
                        if (!quote && !out.empty() && out.back() == ' ' &&
                            (close + 1 >= tmpl.size() || tmpl[close + 1] == ' '))
                            out.pop_back();
                    } else {
                        for (size_t w = 0; w < l.second.size(); w++) {
                            if (w) out += ' ';
                            out += quote ? RunFile::quoteInside(l.second[w], quote)
                                         : RunFile::shellQuote(l.second[w]);
                        }
                    }
                    done = true;
                }
                if (done) {
                    i = close + 1;
                    continue;
                }
            }
        }
        if (quote == '\'') {
            if (c == '\'') quote = 0;
        } else if (c == '\\' && i + 1 < tmpl.size() && tmpl[i + 1] != '{') {
            out += c;
            out += tmpl[++i];
            i++;
            continue;
        } else if (quote == '"') {
            if (c == '"') quote = 0;
        } else if (c == '\'' || c == '"') {
            quote = c;
        }
        out += c;
        i++;
    }
    return out;
}

bool command(const Request& r, Kind kind, const std::vector<Test>& tests, bool wholeFile,
             Command* out, std::string* problem) {
    auto fail = [&](const std::string& why) {
        if (problem) *problem = why;
        return false;
    };
    if (kind == Kind::None) return fail("This is not a test file");
    const std::string language = languageOf(kind);
    std::string tmpl = r.tmpl;
    {
        std::string v = lower(tmpl);
        if (v == "off" || v == "none" || v == "false")
            return fail("Running tests is off for this kind of file (test." + language + " = off)");
    }
    if (tmpl.empty()) tmpl = defaultTemplate(kind);
    if (tests.empty() && !(wholeFile && kind == Kind::Js)) return fail("No tests found in this file");
    auto exists = r.exists ? r.exists : std::function<bool(const std::string&)>(fileExists);

    std::vector<const char*> markers;
    switch (kind) {
        case Kind::Pytest: markers = {"pyproject.toml", "pytest.ini", "setup.cfg", "tox.ini", "setup.py"}; break;
        case Kind::Go: markers = {"go.mod"}; break;
        case Kind::Rust: markers = {"Cargo.toml"}; break;
        case Kind::Js: markers = {"package.json"}; break;
        case Kind::None: break;
    }
    const std::string project = projectDir(r.file, r.root, markers, exists);
    const std::string path = relativeTo(r.file, project);

    std::vector<std::pair<std::string, std::string>> scalars = {
        {"file", r.file},
        {"dir", parentOf(r.file)},
        {"root", r.root.empty() ? parentOf(r.file) : r.root},
        {"name", baseName(r.file)},
        {"project", project},
        {"path", path},
    };
    if (tmpl.find("{python}") != std::string::npos)
        scalars.push_back({"python", r.python.empty() ? "python3" : r.python});
    std::vector<std::pair<std::string, std::vector<std::string>>> lists;

    switch (kind) {
        case Kind::Pytest: {
            std::vector<std::string> ids;
            if (wholeFile) {
                ids.push_back(path);
            } else {
                for (const Test& t : tests) {
                    std::vector<std::string> parts = {path};
                    for (const std::string& s : t.scope) parts.push_back(s);
                    parts.push_back(t.name);
                    ids.push_back(join(parts, "::"));
                }
            }
            lists.push_back({"tests", ids});
            break;
        }
        case Kind::Go: {
            std::vector<std::string> names;
            for (const Test& t : tests) names.push_back(regexEscape(t.name));
            const std::string pattern =
                names.size() == 1 ? "^" + names[0] + "$" : "^(" + join(names, "|") + ")$";
            const std::string pkgDir = relativeTo(parentOf(r.file), project);
            scalars.push_back({"pattern", pattern});
            scalars.push_back(
                {"package", parentOf(r.file) == project ? "." : "./" + pkgDir});
            break;
        }
        case Kind::Rust: {
            // The file's module path in its crate, and its target when that
            // is not the library (an integration test, a binary).
            std::vector<std::string> target;
            std::string mod;
            std::string rel = path;
            auto strip = [](std::string p) {
                if (endsWith(p, ".rs")) p = p.substr(0, p.size() - 3);
                if (p == "lib" || p == "main") return std::string();
                if (endsWith(p, "/mod")) p = p.substr(0, p.size() - 4);
                std::string out;
                for (char ch : p) {
                    if (ch == '/') out += "::";
                    else out += ch;
                }
                return out;
            };
            if (startsWith(rel, "src/bin/")) {
                std::string b = rel.substr(8);
                const size_t slash = b.find('/');
                const std::string bin = slash == std::string::npos ? b.substr(0, b.size() - 3)
                                                                   : b.substr(0, slash);
                target = {"--bin", bin};
                mod = slash == std::string::npos ? "" : strip(b.substr(slash + 1));
            } else if (startsWith(rel, "src/")) {
                mod = strip(rel.substr(4));
            } else if (startsWith(rel, "tests/")) {
                std::string b = rel.substr(6);
                const size_t slash = b.find('/');
                const std::string test = slash == std::string::npos ? b.substr(0, b.size() - 3)
                                                                    : b.substr(0, slash);
                target = {"--test", test};
                mod = slash == std::string::npos ? "" : strip(b.substr(slash + 1));
            }
            std::vector<std::string> paths;
            for (const Test& t : tests) {
                std::vector<std::string> parts = {mod};
                for (const std::string& s : t.scope) parts.push_back(s);
                parts.push_back(t.name);
                paths.push_back(join(parts, "::"));
            }
            lists.push_back({"tests", paths});
            lists.push_back({"target", target});
            break;
        }
        case Kind::Js: {
            const bool localV = exists(project + "/node_modules/.bin/vitest");
            const bool localJ = exists(project + "/node_modules/.bin/jest");
            const bool saysV = r.text.find("'vitest'") != std::string::npos ||
                               r.text.find("\"vitest\"") != std::string::npos ||
                               exists(project + "/vitest.config.ts") ||
                               exists(project + "/vitest.config.js") ||
                               exists(project + "/vitest.config.mts") ||
                               exists(project + "/vitest.config.mjs");
            const bool vitest = localV && localJ ? saysV : localV || (!localJ && saysV);
            std::vector<std::string> runner;
            if (vitest) runner = localV ? std::vector<std::string>{"node_modules/.bin/vitest", "run"}
                                        : std::vector<std::string>{"npx", "vitest", "run"};
            else runner = localJ ? std::vector<std::string>{"node_modules/.bin/jest"}
                                 : std::vector<std::string>{"npx", "jest"};
            std::vector<std::string> filter;
            if (!wholeFile && tests.size() == 1) {
                std::vector<std::string> parts = tests[0].scope;
                parts.push_back(tests[0].name);
                std::string full;
                for (size_t k = 0; k < parts.size(); k++) full += (k ? " " : "") + parts[k];
                filter = {"-t", "^" + regexEscape(full) + "$"};
            }
            lists.push_back({"runner", runner});
            lists.push_back({"filter", filter});
            break;
        }
        case Kind::None: break;
    }
    out->line = expand(tmpl, scalars, lists);
    out->dir = project;
    return true;
}

int testAt(const std::vector<Test>& tests, size_t line) {
    int found = -1;
    for (size_t i = 0; i < tests.size(); i++)
        if (tests[i].line <= line) found = (int)i;
    return found;
}

}  // namespace Runnables
