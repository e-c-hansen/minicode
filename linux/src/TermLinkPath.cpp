// TermLinkPath.cpp — see TermLinkPath.h.
#include "TermLinkPath.h"

#include <climits>
#include <cstdlib>
#include <sys/stat.h>
#include <vector>

namespace TermLinkPath {
namespace {

std::string join(const std::string& dir, const std::string& rel) {
    if (dir.empty()) return rel;
    return dir.back() == '/' ? dir + rel : dir + "/" + rel;
}

// The path with symlinks, "." and ".." resolved, or "" when it does not exist.
std::string real(const std::string& p) {
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : std::string();
}

}  // namespace

bool resolve(const std::string& written, const std::string& cwd,
             const std::string& root, const std::string& home, Target* out) {
    if (written.empty()) return false;

    std::string expanded = written;
    if (written == "~" || written.compare(0, 2, "~/") == 0) {
        if (home.empty()) return false;
        expanded = home + written.substr(1);
    }

    std::vector<std::string> candidates;
    if (expanded[0] == '/') {
        candidates.push_back(expanded);
    } else {
        if (!cwd.empty()) candidates.push_back(join(cwd, expanded));
        if (!root.empty()) candidates.push_back(join(root, expanded));
    }

    for (const std::string& c : candidates) {
        const std::string r = real(c);
        struct stat st;
        if (r.empty() || stat(r.c_str(), &st) != 0) continue;

        Target t;
        t.path = r;
        t.isDir = S_ISDIR(st.st_mode);
        // The shell may spell the folder differently from the window (a
        // symlinked checkout, say). A path inside the project is put back in
        // the root's own spelling, which is what the tree and the editor
        // compare against.
        const std::string rootReal = root.empty() ? std::string() : real(root);
        if (!rootReal.empty()) {
            if (r == rootReal) {
                t.path = root;
                t.insideRoot = true;
            } else if (rootReal == "/") {
                t.insideRoot = true;
            } else if (r.compare(0, rootReal.size() + 1, rootReal + "/") == 0) {
                t.path = join(root, r.substr(rootReal.size() + 1));
                t.insideRoot = true;
            }
        }
        *out = t;
        return true;
    }
    return false;
}

}  // namespace TermLinkPath
