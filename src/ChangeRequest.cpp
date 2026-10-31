// ChangeRequest.cpp — see ChangeRequest.h.
#include "ChangeRequest.h"

#include "RunFile.h"

namespace ChangeRequest {

static std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (a < b && space(s[a])) a++;
    while (b > a && space(s[b - 1])) b--;
    return s.substr(a, b - a);
}

static std::string stripSlashes(std::string s) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

std::string prompt(const Context &c) {
    std::string request = trim(c.request);
    if (request.empty()) return "";
    std::string running = "minicode";
    if (!c.version.empty()) running += " " + c.version;
    if (!c.appPath.empty()) running += " is running from " + c.appPath;
    else running += " is running";
    return request +
           "\n\n(Sent from minicode's Request a Change bar. " + running +
           ". Make the change in this repository, following CLAUDE.md. When it "
           "builds, run `make` (and `make test` if you changed the C++ core) so that "
           "MiniCode.app in this folder is rebuilt: minicode watches for the new "
           "build and offers to restart into it, reopening its windows and this "
           "conversation. Do not commit, push, or release unless asked.)";
}

std::string startCommand(const std::string &command, const std::string &source,
                         const std::string &promptFile) {
    return "cd " + RunFile::shellQuote(source) + " && " + trim(command) + " \"$(cat " +
           RunFile::shellQuote(promptFile) + ")\"";
}

std::string resumeCommand(const std::string &command, const std::string &source) {
    return "cd " + RunFile::shellQuote(source) + " && " + trim(command) + " --continue";
}

bool looksLikeSource(const std::string &dir,
                     const std::function<bool(const std::string &)> &exists) {
    if (dir.empty()) return false;
    const std::string d = stripSlashes(dir);
    const std::string base = d == "/" ? "" : d;
    return exists(base + "/Makefile") && exists(base + "/src/EditorController.mm");
}

std::string checkoutOfBundle(const std::string &bundlePath) {
    const std::string b = stripSlashes(bundlePath);
    const size_t slash = b.rfind('/');
    if (slash == std::string::npos || slash == 0) return "";
    return b.substr(0, slash);
}

std::string builtApp(const std::string &source) {
    return stripSlashes(source) + "/MiniCode.app";
}

std::string buildStamp(const std::string &source) {
    return builtApp(source) + "/Contents/_CodeSignature/CodeResources";
}

}  // namespace ChangeRequest
