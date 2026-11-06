// AgentView.cpp — see AgentView.h.
#include "AgentView.h"

#include <algorithm>
#include <cstdio>
#include <set>

namespace AgentView {

using AgentIndex::Place;

std::vector<FileGroup> groupFiles(const std::vector<AgentIndex::FileTouch> &files) {
    std::vector<FileGroup> out;
    for (Place p : {Place::Main, Place::Worktree, Place::Outside}) {
        FileGroup g{p, {}};
        for (size_t i = 0; i < files.size(); i++)
            if (files[i].place == p && files[i].touched()) g.files.push_back(i);
        if (!g.files.empty()) out.push_back(std::move(g));
    }
    return out;
}

static std::string parentOf(const std::string &path) {
    size_t slash = path.rfind('/');
    if (slash == std::string::npos) return "";
    return slash == 0 ? "/" : path.substr(0, slash);
}

// `dir` below `root`: "" for root itself, "a/b" below it; npos-like false
// when it is not inside.
static bool below(const std::string &dir, const std::string &root, std::string *out) {
    if (root.empty()) return false;
    if (dir == root) { *out = ""; return true; }
    if (dir.size() > root.size() && dir.compare(0, root.size(), root) == 0 && dir[root.size()] == '/') {
        *out = dir.substr(root.size() + 1);
        return true;
    }
    return false;
}

std::string folderText(const std::string &path, AgentIndex::Place place,
                       const std::string &folder, const std::string &worktree,
                       const std::string &scratchpad, const std::string &home) {
    const std::string dir = parentOf(AgentIndex::normalizePath(path));
    std::string rel;
    if (place == AgentIndex::Place::Worktree) {
        if (below(dir, AgentIndex::normalizePath(worktree), &rel)) return rel;
        const std::string mark = "/.claude/worktrees/";
        size_t at = dir.find(mark);
        if (at != std::string::npos) {
            size_t start = at + mark.size();
            size_t end = dir.find('/', start);
            return end == std::string::npos ? "" : dir.substr(end + 1);
        }
    }
    if (place == AgentIndex::Place::Main && below(dir, AgentIndex::normalizePath(folder), &rel))
        return rel;
    if (!scratchpad.empty() && below(dir, AgentIndex::normalizePath(scratchpad), &rel))
        return rel.empty() ? "scratchpad" : "scratchpad/" + rel;
    if (below(dir, home, &rel)) return rel.empty() ? "~" : "~/" + rel;
    return dir;
}

static std::string plural(int n, const char *one, const char *many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string countsText(int files, int commands) {
    if (files <= 0 && commands <= 0) return "no files or commands";
    std::string f = files > 0 ? plural(files, "file", "files") : "no files";
    std::string c = commands > 0 ? plural(commands, "command", "commands") : "no commands";
    return f + ", " + c;
}

std::string detailLine(bool running, const std::string &age, const std::string &where,
                       int files, int commands) {
    std::vector<std::string> parts;
    parts.push_back(running ? "running" : "finished");
    if (!age.empty()) parts.push_back(age);
    if (!where.empty()) parts.push_back(where);
    parts.push_back(countsText(files, commands));
    std::string s;
    for (const std::string &p : parts) {
        if (!s.empty()) s += " \xC2\xB7 ";   // " · "
        s += p;
    }
    return s;
}

std::string emptyText(bool projectsMissing, bool loaded, const std::string &projectsDir) {
    if (!loaded) return "Reading agent transcripts\xE2\x80\xA6";
    if (projectsMissing)
        return "No Claude Code transcripts: " + projectsDir +
               " does not exist. Sessions show here once Claude Code has worked in this folder.";
    return "No agent sessions have worked in this folder.";
}

std::string worktreeBadge(int ahead, bool dirty) {
    std::string s;
    if (ahead > 0) s = std::to_string(ahead) + " ahead";
    if (dirty) s += s.empty() ? "modified" : " \xC2\xB7 modified";
    if (s.empty() && ahead == 0) s = "up to date";
    return s;
}

std::string mainRef(const std::vector<AgentIndex::Worktree> &all) {
    for (const AgentIndex::Worktree &w : all) {
        if (!w.main) continue;
        if (!w.branch.empty()) return w.branch;
        return w.head;
    }
    return "";
}

static std::vector<std::string> splitNul(const std::string &s) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a < s.size()) {
        size_t b = s.find('\0', a);
        if (b == std::string::npos) b = s.size();
        out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

std::vector<Change> parseChanges(const std::string &nameStatusZ, const std::string &untrackedZ) {
    std::vector<Change> out;
    std::set<std::string> seen;
    std::vector<std::string> f = splitNul(nameStatusZ);
    for (size_t i = 0; i < f.size();) {
        const std::string &st = f[i];
        if (st.empty()) { i++; continue; }
        Change c;
        c.status = st[0];
        bool two = c.status == 'R' || c.status == 'C';
        if (two) {
            if (i + 2 >= f.size()) break;
            c.from = f[i + 1];
            c.path = f[i + 2];
            i += 3;
        } else {
            if (i + 1 >= f.size()) break;
            c.path = f[i + 1];
            i += 2;
        }
        if (c.path.empty() || !seen.insert(c.path).second) continue;
        out.push_back(std::move(c));
    }
    for (const std::string &p : splitNul(untrackedZ)) {
        if (p.empty() || !seen.insert(p).second) continue;
        Change c;
        c.status = '?';
        c.path = p;
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(),
              [](const Change &a, const Change &b) { return a.path < b.path; });
    return out;
}

static std::string grouped(long long n) {
    std::string digits = std::to_string(n < 0 ? -n : n), out;
    int k = 0;
    for (size_t i = digits.size(); i-- > 0;) {
        out.insert(out.begin(), digits[i]);
        if (++k % 3 == 0 && i > 0) out.insert(out.begin(), ',');
    }
    return n < 0 ? "-" + out : out;
}

std::string summaryText(const AgentEvidence::Summary &s) {
    if (!s.found) return "";
    std::vector<std::string> parts;
    if (s.failed > 0) parts.push_back(grouped(s.failed) + " failed");
    if (s.errors > 0) parts.push_back(grouped(s.errors) + (s.errors == 1 ? " error" : " errors"));
    parts.push_back(grouped(s.passed) + " passed");
    if (s.skipped > 0) parts.push_back(grouped(s.skipped) + " skipped");
    // minicode's own "N passed, 0 failed" says so even with none failed.
    if (s.failed == 0 && s.format == "count") parts.push_back("0 failed");
    std::string out;
    for (const std::string &p : parts) {
        if (!out.empty()) out += ", ";
        out += p;
    }
    return out;
}

const char *resultWord(const AgentEvidence::Command &c) {
    if (!c.answered) return "running";
    return c.isError ? "error" : "ok";
}

std::string moreLinesText(long long omitted) {
    if (omitted < 0) return "more lines";
    return grouped(omitted) + (omitted == 1 ? " more line" : " more lines");
}

std::string clipLine(const std::string &line, size_t max) {
    if (line.size() <= max) return line;
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(line[n]) & 0xC0) == 0x80) n--;
    return line.substr(0, n) + "\xE2\x80\xA6";
}

std::string notFoundText(const std::string &target, const std::vector<std::string> &tried,
                         const std::string &home) {
    std::string s = "Not found: " + target;
    if (tried.empty()) return s;
    s += " (looked in ";
    for (size_t i = 0; i < tried.size(); i++) {
        std::string t = tried[i];
        if (!home.empty() && (t == home || t.compare(0, home.size() + 1, home + "/") == 0))
            t = "~" + t.substr(home.size());
        if (i) s += ", ";
        s += t;
    }
    return s + ")";
}

}  // namespace AgentView
