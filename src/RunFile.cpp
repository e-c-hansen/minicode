// RunFile.cpp — see RunFile.h.
#include "RunFile.h"

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

std::string expand(const std::string& tmpl,
                   const std::map<std::string, std::string>& vars) {
    std::string out;
    size_t i = 0;
    while (i < tmpl.size()) {
        if (tmpl[i] == '{') {
            size_t close = tmpl.find('}', i + 1);
            if (close != std::string::npos) {
                auto it = vars.find(tmpl.substr(i + 1, close - i - 1));
                if (it != vars.end()) {
                    out += shellQuote(it->second);
                    i = close + 1;
                    continue;
                }
            }
        }
        out += tmpl[i++];
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

}  // namespace RunFile
