// RunFile.cpp — see RunFile.h.
#include "RunFile.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include <sys/stat.h>
#include <unistd.h>

namespace RunFile {

std::string shellQuote(const std::string& s) {
    if (s.empty()) return "''";
    bool plain = true;
    for (unsigned char c : s) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_' ||
                  c == '-' || c == '+' || c == ',' || c == ':' || c == '@' ||
                  c == '%' || c >= 0x80;
        if (!ok) { plain = false; break; }
    }
    if (plain) return s;
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string quoteInside(const std::string& s, char quote) {
    std::string out;
    if (quote == '\'') {
        // A ' cannot appear between single quotes: close, an escaped ', reopen.
        for (char c : s) {
            if (c == '\'') out += "'\\''";
            else out += c;
        }
        return out;
    }
    // Between double quotes the shell still expands $ and `; a backslash
    // keeps them, " and itself literal. History expansion still sees a !
    // there, and what a backslash does to it differs between shells, so it
    // steps out of the double quotes into single ones.
    for (char c : s) {
        if (c == '!') {
            out += "\"'!'\"";
            continue;
        }
        if (c == '"' || c == '$' || c == '`' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

std::string expand(const std::string& tmpl,
                   const std::map<std::string, std::string>& vars) {
    std::string out;
    char quote = 0;   // the quote the template has open at this point, if any
    size_t i = 0;
    while (i < tmpl.size()) {
        const char c = tmpl[i];
        if (c == '{') {
            size_t close = tmpl.find('}', i + 1);
            if (close != std::string::npos) {
                auto it = vars.find(tmpl.substr(i + 1, close - i - 1));
                if (it != vars.end()) {
                    // A placeholder the user already quoted ("{file}") gets
                    // what is right inside those quotes, not quotes of its own.
                    out += quote ? quoteInside(it->second, quote) : shellQuote(it->second);
                    i = close + 1;
                    continue;
                }
            }
        }
        if (quote == '\'') {
            if (c == '\'') quote = 0;
        } else if (c == '\\' && i + 1 < tmpl.size() && tmpl[i + 1] != '{') {
            out += c;            // an escaped character, in or out of double quotes
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

namespace {

bool isExecutableFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
           access(path.c_str(), X_OK) == 0;
}

std::string parentOf(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return "";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

std::string withoutTrailingSlash(std::string s) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

}  // namespace

std::string findVirtualenvPython(const std::string& file, const std::string& root) {
    std::string top = withoutTrailingSlash(root);
    std::string dir = parentOf(file);
    bool inside = !top.empty() &&
                  (dir == top || dir.compare(0, top.size() + 1, top + "/") == 0);
    static const char* const kNames[] = {".venv", "venv", "env"};
    while (!dir.empty()) {
        for (const char* name : kNames) {
            std::string python = (dir == "/" ? "" : dir) + "/" + name + "/bin/python";
            if (isExecutableFile(python)) return python;
        }
        if (!inside || dir == top || dir == "/") break;
        dir = parentOf(dir);
    }
    return "";
}

std::vector<std::string> pythonReplLines(const std::string& code, bool bracketed,
                                         bool* extraReturn) {
    if (extraReturn) *extraReturn = false;
    std::vector<std::string> lines;
    size_t at = 0;
    while (at <= code.size()) {
        size_t nl = code.find('\n', at);
        std::string line = code.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? code.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    auto blank = [](const std::string& l) {
        return l.find_first_not_of(" \t") == std::string::npos;
    };
    while (!lines.empty() && blank(lines.back())) lines.pop_back();
    while (!lines.empty() && blank(lines.front())) lines.erase(lines.begin());
    // The common indentation of the lines with text, by characters.
    size_t common = std::string::npos;
    for (const std::string& l : lines) {
        if (blank(l)) continue;
        common = std::min(common, l.find_first_not_of(" \t"));
    }
    std::vector<std::string> out;
    for (const std::string& l : lines) {
        if (blank(l)) {
            if (bracketed) out.push_back("");
            continue;
        }
        out.push_back(common == std::string::npos ? l : l.substr(common));
    }
    if (extraReturn && !out.empty()) {
        const std::string& last = out.back();
        *extraReturn = !last.empty() && (last[0] == ' ' || last[0] == '\t');
        // A one-line block ("for x in y: f(x)") ends at once; a block header
        // alone ("if x:") still waits for its body, which is not ours to give.
    }
    return out;
}

bool activationFor(const std::string& python, Activation* out) {
    // bin/python -> the environment's folder.
    auto parent = [](const std::string& p) {
        size_t slash = p.find_last_of('/');
        return slash == std::string::npos || slash == 0 ? std::string() : p.substr(0, slash);
    };
    const std::string bin = parent(python), prefix = parent(bin);
    if (prefix.empty() || bin.substr(bin.find_last_of('/') + 1) != "bin") return false;
    std::string cfg;
    {
        struct stat st;
        const std::string path = prefix + "/pyvenv.cfg";
        if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 65536)
            return false;
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) cfg.append(buf, n);
        std::fclose(f);
    }
    std::string prompt;
    size_t at = 0;
    while (at < cfg.size()) {
        size_t nl = cfg.find('\n', at);
        std::string line = cfg.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? cfg.size() : nl + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        if (trim(key) != "prompt") continue;
        prompt = trim(value);
        // venv writes prompt = 'name' with quotes on some versions.
        if (prompt.size() >= 2 && (prompt.front() == '\'' || prompt.front() == '"') &&
            prompt.back() == prompt.front())
            prompt = prompt.substr(1, prompt.size() - 2);
    }
    if (prompt.empty()) prompt = prefix.substr(prefix.find_last_of('/') + 1);
    std::string safe;
    for (char c : prompt) {
        const unsigned char u = (unsigned char)c;
        if (std::isalnum(u) || std::strchr(" ._-+@:", c)) safe += c;
    }
    if (out) {
        out->prefix = prefix;
        out->prompt = safe.empty() ? "venv" : safe;
    }
    return true;
}

}  // namespace RunFile
