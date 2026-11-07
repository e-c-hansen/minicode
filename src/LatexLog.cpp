// LatexLog.cpp — see LatexLog.h.
#include "LatexLog.h"

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

// The transcript's "! message" blocks, each with the "l.N" line that
// follows within a few lines and the file TeX had open there.
std::vector<Problem> transcriptProblems(const std::vector<std::string> &lines) {
    std::vector<Problem> out;
    std::vector<std::string> files;   // the "(name" groups still open
    for (std::size_t i = 0; i < lines.size(); i++) {
        const std::string &l = lines[i];
        if (startsWith(l, "error:") || startsWith(l, "warning:") || startsWith(l, "caused by:"))
            continue;
        if (startsWith(l, "! ")) {
            Problem p;
            p.message = cleanMessage(l);
            p.file = files.empty() ? std::string() : files.back();
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
        for (std::size_t k = 0; k < l.size(); k++) {
            if (l[k] == ')') {
                if (!files.empty()) files.pop_back();
            } else if (l[k] == '(') {
                std::size_t e = k + 1;
                while (e < l.size() && l[e] != ' ' && l[e] != '(' && l[e] != ')') e++;
                files.push_back(l.substr(k + 1, e - k - 1));
                k = e - 1;
            }
        }
    }
    return out;
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
            anyLine = true;
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
