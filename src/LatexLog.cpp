// LatexLog.cpp — see LatexLog.h.
#include "LatexLog.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace LatexLog {
namespace {

std::string trim(const std::string &s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool startsWith(const std::string &s, const char *prefix) {
    return s.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

std::vector<std::string> splitLines(const std::string &text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        std::string line =
            text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return lines;
}

// TeX's "! Undefined control sequence." reads as "Undefined control
// sequence": the bang (one or two: a LaTeX error comes as "! ! LaTeX
// Error") and the closing full stop go.
std::string cleanMessage(std::string m) {
    m = trim(m);
    while (!m.empty() && m[0] == '!') m = trim(m.substr(1));
    if (m.size() > 1 && m.back() == '.' && m[m.size() - 2] != '.') m.pop_back();
    return trim(m);
}

// tectonic's own lines that say only that something failed.
bool isChatter(const std::string &rest) {
    return startsWith(rest, "something bad happened inside XeTeX") ||
           startsWith(rest, "warnings were issued by the TeX engine") ||
           startsWith(rest, "the XeTeX engine had an unrecoverable error") ||
           startsWith(rest, "halted on potentially-recoverable error") ||
           startsWith(rest, "halted after ");
}

// "<file>:<line>: <message>", the first ":digits:" deciding (a file name
// may itself hold a colon, rarely, but not one followed by digits and
// another colon).
bool splitPlace(const std::string &rest, std::string *file, int *line, std::string *msg) {
    for (std::size_t p = rest.find(':'); p != std::string::npos; p = rest.find(':', p + 1)) {
        std::size_t q = p + 1;
        while (q < rest.size() && std::isdigit((unsigned char)rest[q])) q++;
        if (q == p + 1 || q >= rest.size() || rest[q] != ':' || p == 0) continue;
        if (q - p > 9) continue;
        *file = rest.substr(0, p);
        *line = std::atoi(rest.substr(p + 1, q - p - 1).c_str());
        *msg = rest.substr(q + 1);
        return *line > 0;
    }
    return false;
}

void add(std::vector<Problem> &out, const Problem &p) {
    if (p.message.empty()) return;
    for (const Problem &k : out)
        if (k.file == p.file && k.line == p.line && k.message == p.message) return;
    out.push_back(p);
}

// The files TeX has open, from the transcript's "(name" and ")" marks:
// "(article.cls", "(size10.clo))", "(./child.tex". Text in a line can hold
// parentheses too; the callers skip the lines that are not TeX's own.
struct OpenFiles {
    std::vector<std::string> stack;
    void scan(const std::string &l) {
        for (std::size_t k = 0; k < l.size(); k++) {
            if (l[k] == ')') {
                if (!stack.empty()) stack.pop_back();
            } else if (l[k] == '(') {
                std::size_t e = k + 1;
                while (e < l.size() && l[e] != ' ' && l[e] != '(' && l[e] != ')') e++;
                stack.push_back(l.substr(k + 1, e - k - 1));
                k = e - 1;
            }
        }
    }
    std::string top() const { return stack.empty() ? std::string() : stack.back(); }
};

// The transcript's "! message" blocks, each with the "l.N" line that
// follows within a few lines and the file TeX had open there.
std::vector<Problem> transcriptProblems(const std::vector<std::string> &lines) {
    std::vector<Problem> out;
    OpenFiles files;   // the "(name" groups still open
    for (std::size_t i = 0; i < lines.size(); i++) {
        const std::string &l = lines[i];
        if (startsWith(l, "error:") || startsWith(l, "warning:") || startsWith(l, "caused by:"))
            continue;
        if (startsWith(l, "! ")) {
            Problem p;
            p.message = cleanMessage(l);
            p.file = files.top();
            for (std::size_t j = i + 1; j < lines.size() && j <= i + 12; j++) {
                const std::string &c = lines[j];
                if (startsWith(c, "! ")) break;
                if (c.size() > 2 && c[0] == 'l' && c[1] == '.' &&
                    std::isdigit((unsigned char)c[2])) {
                    p.line = std::atoi(c.c_str() + 2);
                    break;
                }
            }
            if (p.line > 0) add(out, p);
            continue;
        }
        // Files opening and closing: "(article.cls", "(size10.clo))".
        files.scan(l);
    }
    return out;
}

// "Overfull \hbox (12pt too wide) in paragraph at lines 13--17": 13, the
// paragraph's first line; 0 when the message gives no range.
int paragraphStart(const std::string &message) {
    const std::string key = "in paragraph at lines ";
    const std::size_t at = message.find(key);
    if (at == std::string::npos) return 0;
    const char *digits = message.c_str() + at + key.size();
    return std::isdigit((unsigned char)*digits) ? std::atoi(digits) : 0;
}

std::size_t namedWarningPrefix(const std::string &l);
bool isOtherLatexWarning(const std::string &l);

// TeX wraps its transcript at 79 characters.
const std::size_t kLogWidth = 79;

// A warning's lines from `i`, joined: a line TeX wrapped is continued as it
// is, a line the warning broke itself ("(mypkg)    over two lines") after a
// space with the "(name)" padding gone. Stops at a blank line (TeX puts
// one after each warning) or another warning; `end` gets the index after
// the last line read.
std::string joinWarning(const std::vector<std::string> &lines, std::size_t i, std::size_t *end) {
    std::string text = lines[i];
    std::size_t j = i + 1;
    bool wrapped = lines[i].size() == kLogWidth;
    for (; j < lines.size() && j < i + 12 && !lines[j].empty(); j++) {
        std::string next = lines[j];
        if (!wrapped && (startsWith(next, "LaTeX Warning: ") || namedWarningPrefix(next) ||
                         isOtherLatexWarning(next) || startsWith(next, "! ")))
            break;
        if (wrapped) {
            text += next;
        } else {
            if (next[0] == '(') {
                const std::size_t close = next.find(')');
                if (close != std::string::npos) next = next.substr(close + 1);
            }
            text += " " + trim(next);
        }
        wrapped = lines[j].size() == kLogWidth;
    }
    *end = j;
    return text;
}

// "... on input line 16." -> 16, with the words and the full stop gone from
// `message`; 0 when it names no line.
int takeInputLine(std::string *message) {
    std::string m = trim(*message);
    if (!m.empty() && m.back() == '.') m.pop_back();
    const std::string key = " on input line ";
    const std::size_t at = m.rfind(key);
    if (at == std::string::npos) return 0;
    const std::string digits = m.substr(at + key.size());
    if (digits.empty() || digits.size() > 9) return 0;
    for (char c : digits)
        if (!std::isdigit((unsigned char)c)) return 0;
    *message = trim(m.substr(0, at));
    return std::atoi(digits.c_str());
}

// "Package mypkg Warning: " or "Class foo Warning: ": the prefix's length,
// or 0.
std::size_t namedWarningPrefix(const std::string &l) {
    for (const char *kind : {"Package ", "Class "}) {
        if (!startsWith(l, kind)) continue;
        const std::size_t name = std::char_traits<char>::length(kind);
        const std::size_t space = l.find(' ', name);
        if (space == std::string::npos || space == name) return 0;
        if (l.compare(space, 10, " Warning: ") == 0) return space + 10;
    }
    return 0;
}

// "LaTeX Font Warning: " and its kind (a substitution, LaTeX's notes on
// its own work): left out.
bool isOtherLatexWarning(const std::string &l) {
    if (!startsWith(l, "LaTeX ") || startsWith(l, "LaTeX Warning: ")) return false;
    const std::size_t at = l.find(" Warning: ");
    return at != std::string::npos && at < 30 && l.find(' ', 6) == at;
}

std::string folderOf(const std::string &path) {
    std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string nameOf(const std::string &path) {
    std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string withoutTex(const std::string &name) {
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tex") == 0)
        return name.substr(0, name.size() - 4);
    return name;
}

}  // namespace

std::vector<Problem> parse(const std::string &output) {
    const std::vector<std::string> lines = splitLines(output);
    std::vector<Problem> out;
    bool anyLine = false;
    for (std::size_t i = 0; i < lines.size(); i++) {
        const std::string &l = lines[i];
        Severity severity;
        std::string rest;
        if (startsWith(l, "error:")) {
            severity = Severity::Error;
            rest = trim(l.substr(6));
        } else if (startsWith(l, "warning:")) {
            severity = Severity::Warning;
            rest = trim(l.substr(8));
        } else {
            continue;
        }
        if (rest.empty() || isChatter(rest)) continue;
        Problem p;
        p.severity = severity;
        std::string msg;
        if (splitPlace(rest, &p.file, &p.line, &msg)) {
            p.message = cleanMessage(msg);
            if (severity == Severity::Warning && startsWith(p.message, "Underfull")) continue;
            if (const int first = paragraphStart(p.message))
                if (first < p.line) p.line = first;
            if (severity == Severity::Error) anyLine = true;
        } else {
            p.file.clear();
            p.line = 0;
            p.message = cleanMessage(rest);
        }
        // TeX's "*** (job aborted, no legal \end found)" explains the
        // emergency stop on the line before.
        if (i + 1 < lines.size() && startsWith(lines[i + 1], "*** "))
            p.message += " " + trim(lines[i + 1].substr(4));
        add(out, p);
    }
    if (!anyLine) {
        // No summary line placed anything: TeX's own blocks may, and come
        // first, since they say where.
        std::vector<Problem> placed = transcriptProblems(lines);
        for (const Problem &p : out) add(placed, p);
        out = placed;
    }
    return out;
}

std::vector<Problem> parseTranscriptWarnings(const std::string &log) {
    const std::vector<std::string> lines = splitLines(log);
    std::vector<Problem> out;
    OpenFiles files;
    for (std::size_t i = 0; i < lines.size(); i++) {
        const std::string &l = lines[i];
        // A box's report is followed by the box's own text, which may hold
        // any parentheses: up to the blank line, it is not TeX's marks.
        if (startsWith(l, "Overfull \\") || startsWith(l, "Underfull \\") ||
            startsWith(l, "Tight \\") || startsWith(l, "Loose \\")) {
            while (i + 1 < lines.size() && !lines[i + 1].empty()) i++;
            continue;
        }
        // An error's block, through the source line it quotes after "l.N".
        if (startsWith(l, "! ")) {
            for (std::size_t j = i + 1; j < lines.size() && j <= i + 12; j++) {
                const std::string &c = lines[j];
                if (c.size() > 2 && c[0] == 'l' && c[1] == '.' &&
                    std::isdigit((unsigned char)c[2])) {
                    i = j + 1 < lines.size() ? j + 1 : j;
                    break;
                }
            }
            continue;
        }
        const bool latex = startsWith(l, "LaTeX Warning: ");
        const std::size_t prefix = latex ? 15 : namedWarningPrefix(l);
        if (prefix == 0 && !isOtherLatexWarning(l)) {
            files.scan(l);
            continue;
        }
        std::size_t end = i;
        std::string message = joinWarning(lines, i, &end);
        i = end - 1;
        if (prefix == 0) continue;   // a font substitution
        const std::string body = message.substr(prefix);
        Problem p;
        p.severity = Severity::Warning;
        p.line = takeInputLine(&message);
        p.message = trim(message);
        if (p.line > 0) {
            p.file = files.top();
        } else if (latex && startsWith(body, "Label `") &&
                   body.find("' multiply defined") != std::string::npos) {
            p.message = cleanMessage(message);   // about the document, at no line
        } else {
            continue;   // "There were undefined references", a package's note
        }
        add(out, p);
    }
    return out;
}

std::vector<Problem> collect(const std::string &output, const std::string &transcript) {
    std::vector<Problem> all = parse(output);
    for (const Problem &p : parseTranscriptWarnings(transcript)) add(all, p);
    std::stable_partition(all.begin(), all.end(),
                          [](const Problem &p) { return p.severity == Severity::Error; });
    return all;
}

std::string rootComment(const std::string &text) {
    for (const std::string &raw : splitLines(text)) {
        std::size_t k = 0;
        while (k < raw.size() && (raw[k] == ' ' || raw[k] == '\t')) k++;
        if (k >= raw.size() || raw[k] != '%') continue;
        k++;
        while (k < raw.size() && raw[k] == ' ') k++;
        if (k >= raw.size() || raw[k] != '!') continue;
        k++;
        while (k < raw.size() && raw[k] == ' ') k++;
        if (raw.size() < k + 3) continue;
        std::string word = raw.substr(k, 3);
        for (char &c : word) c = (char)std::tolower((unsigned char)c);
        if (word != "tex") continue;
        k += 3;
        if (k >= raw.size() || raw[k] != ' ') continue;
        while (k < raw.size() && raw[k] == ' ') k++;
        std::string key = raw.substr(k, 4);
        for (char &c : key) c = (char)std::tolower((unsigned char)c);
        if (key != "root") continue;
        k += 4;
        while (k < raw.size() && raw[k] == ' ') k++;
        if (k >= raw.size() || raw[k] != '=') continue;
        const std::string value = trim(raw.substr(k + 1));
        if (!value.empty()) return value;
    }
    return std::string();
}

std::string resolveRoot(const std::string &partPath, const std::string &value) {
    if (value.empty()) return std::string();
    const std::string whole = value[0] == '/' ? value : folderOf(partPath) + "/" + value;
    std::vector<std::string> parts;
    std::size_t at = 0;
    while (at <= whole.size()) {
        std::size_t slash = whole.find('/', at);
        if (slash == std::string::npos) slash = whole.size();
        const std::string part = whole.substr(at, slash - at);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        at = slash + 1;
    }
    std::string out;
    for (const std::string &p : parts) out += "/" + p;
    return out.empty() ? "/" : out;
}

std::string sourcePath(const std::string &file, const std::string &texPath,
                       const std::string &siblingPath,
                       const std::function<bool(const std::string &)> &exists) {
    if (file.empty() || file == siblingPath) return texPath;
    const std::string siblingName = nameOf(siblingPath);
    if (!siblingName.empty() &&
        (nameOf(file) == siblingName || nameOf(file) == withoutTex(siblingName)) &&
        (file.find('/') == std::string::npos || folderOf(file) == folderOf(siblingPath) ||
         startsWith(file, "./")))
        return texPath;
    std::string path = file;
    if (startsWith(path, "./")) path = path.substr(2);
    if (path.empty() || path[0] != '/') {
        const std::string dir = folderOf(texPath);
        path = dir.empty() ? path : dir + "/" + path;
    }
    if (exists(path)) return path;
    if (withoutTex(path) == path && exists(path + ".tex")) return path + ".tex";
    return std::string();
}

}  // namespace LatexLog
