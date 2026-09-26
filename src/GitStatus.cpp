// GitStatus.cpp — pure C++. See GitStatus.h.
#include "GitStatus.h"
#include <algorithm>

namespace Git {

namespace {

bool startsWith(const std::string& s, const char* prefix) {
    return s.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

// Splits off `n` space-separated fields from the front of `rec`. What is left
// (the path, which may hold spaces) goes to `rest`. False when there are
// fewer fields than that.
bool splitFields(const std::string& rec, size_t n, std::vector<std::string>& fields,
                 std::string& rest) {
    fields.clear();
    size_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        size_t sp = rec.find(' ', pos);
        if (sp == std::string::npos) return false;
        fields.push_back(rec.substr(pos, sp - pos));
        pos = sp + 1;
    }
    rest = rec.substr(pos);
    return true;
}

void readXY(const std::string& xy, Entry& e) {
    if (xy.size() >= 2) { e.staged = xy[0]; e.unstaged = xy[1]; }
}

// A whole number at s[i], moving i past it; `ok` false when there is none.
int readNumber(const std::string& s, size_t& i, bool& ok) {
    size_t start = i;
    long v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        if (v > 100000000) v = 100000000;
        i++;
    }
    if (i == start) ok = false;
    return (int)v;
}

// "@@ -1,3 +1,4 @@ section", or a combined diff's "@@@ -1,3 -1,3 +1,4 @@@".
// `olds` gets one start/count pair per parent.
bool parseHunkHeader(const std::string& line, std::vector<std::pair<int, int>>& olds,
                     int& newStart, int& newCount, std::string& section) {
    size_t ats = 0;
    while (ats < line.size() && line[ats] == '@') ats++;
    if (ats < 2) return false;
    size_t parents = ats - 1;
    olds.clear();
    size_t i = ats;
    bool ok = true;
    for (size_t p = 0; p < parents; p++) {
        if (i + 1 >= line.size() || line[i] != ' ' || line[i + 1] != '-') return false;
        i += 2;
        int start = readNumber(line, i, ok), count = 1;
        if (i < line.size() && line[i] == ',') { i++; count = readNumber(line, i, ok); }
        olds.push_back({start, count});
    }
    if (i + 1 >= line.size() || line[i] != ' ' || line[i + 1] != '+') return false;
    i += 2;
    newStart = readNumber(line, i, ok);
    newCount = 1;
    if (i < line.size() && line[i] == ',') { i++; newCount = readNumber(line, i, ok); }
    if (!ok) return false;
    std::string closing = " " + std::string(ats, '@');
    if (line.compare(i, closing.size(), closing) != 0) return false;
    i += closing.size();
    if (i < line.size() && line[i] == ' ') i++;
    section = line.substr(i);
    return true;
}

// A path token from a header line: unquoted, trailing tab dropped (git adds
// one after a name with a space in it), and "a/" or "b/" taken off.
// /dev/null becomes empty.
std::string headerPath(std::string s, const char* prefix) {
    if (!s.empty() && s.back() == '\t') s.pop_back();
    s = unquotePath(s);
    if (s == "/dev/null") return std::string();
    size_t n = std::char_traits<char>::length(prefix);
    if (n && startsWith(s, prefix)) s = s.substr(n);
    return s;
}

// The two paths of "diff --git a/x b/y", which is ambiguous when the names
// hold " b/"; the ---/+++ and rename lines that follow settle it when they
// are there.
void gitHeaderPaths(const std::string& rest, std::string& a, std::string& b) {
    if (!rest.empty() && rest[0] == '"') {
        size_t i = 1;
        while (i < rest.size() && rest[i] != '"') i += rest[i] == '\\' ? 2 : 1;
        a = headerPath(rest.substr(0, i + 1), "a/");
        b = i + 2 < rest.size() ? headerPath(rest.substr(i + 2), "b/") : a;
        return;
    }
    // The usual case, the same name twice: "a/X b/X".
    if (rest.size() >= 5 && (rest.size() - 5) % 2 == 0) {
        size_t n = (rest.size() - 5) / 2;
        std::string x = rest.substr(2, n);
        if (startsWith(rest, "a/") && rest.compare(2 + n, std::string::npos, " b/" + x) == 0) {
            a = b = x;
            return;
        }
    }
    size_t sp = rest.find(" b/");
    if (sp == std::string::npos) sp = rest.find(" \"b/");
    if (sp == std::string::npos) { a = b = headerPath(rest, "a/"); return; }
    a = headerPath(rest.substr(0, sp), "a/");
    b = headerPath(rest.substr(sp + 1), "b/");
}

}  // namespace

// ------------------------------------------------------------------ status
Status parseStatus(const std::string& out) {
    Status st;
    std::vector<std::string> recs;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t nul = out.find('\0', pos);
        if (nul == std::string::npos) nul = out.size();
        recs.push_back(out.substr(pos, nul - pos));
        pos = nul + 1;
    }

    std::vector<std::string> f;
    std::string rest;
    for (size_t r = 0; r < recs.size(); r++) {
        const std::string& rec = recs[r];
        if (rec.size() < 2 || rec[1] != ' ') continue;
        switch (rec[0]) {
        case '#': {
            if (!splitFields(rec, 2, f, rest)) break;
            const std::string& key = f[1];
            if (key == "branch.oid") {
                if (rest == "(initial)") st.initial = true;
                else st.oid = rest;
            } else if (key == "branch.head") {
                if (rest == "(detached)") st.detached = true;
                else st.branch = rest;
            } else if (key == "branch.upstream") {
                st.upstream = rest;
            } else if (key == "branch.ab") {
                // "+3 -1"
                size_t i = 0;
                bool ok = true;
                if (i < rest.size() && rest[i] == '+') i++;
                int a = readNumber(rest, i, ok);
                if (i + 1 < rest.size() && rest[i] == ' ' && rest[i + 1] == '-') i += 2;
                else ok = false;
                int b = readNumber(rest, i, ok);
                if (ok) { st.hasAheadBehind = true; st.ahead = a; st.behind = b; }
            }
            break;
        }
        case '1': {
            if (!splitFields(rec, 8, f, rest) || rest.empty()) break;
            Entry e;
            readXY(f[1], e);
            e.submodule = !f[2].empty() && f[2][0] == 'S';
            e.path = rest;
            st.entries.push_back(e);
            break;
        }
        case '2': {
            if (!splitFields(rec, 9, f, rest) || rest.empty()) break;
            Entry e;
            readXY(f[1], e);
            e.submodule = !f[2].empty() && f[2][0] == 'S';
            e.path = rest;
            // With -z the original path is the next record.
            if (r + 1 < recs.size()) e.origPath = recs[++r];
            st.entries.push_back(e);
            break;
        }
        case 'u': {
            if (!splitFields(rec, 10, f, rest) || rest.empty()) break;
            Entry e;
            readXY(f[1], e);
            e.submodule = !f[2].empty() && f[2][0] == 'S';
            e.unmerged = true;
            e.path = rest;
            st.entries.push_back(e);
            break;
        }
        case '?': {
            Entry e;
            e.untracked = true;
            e.path = rec.substr(2);
            if (!e.path.empty()) st.entries.push_back(e);
            break;
        }
        default:
            break;   // '!' (ignored) and anything newer
        }
    }
    return st;
}

bool isStaged(const Entry& e) {
    return !e.unmerged && !e.untracked && e.staged != '.';
}

bool hasUnstaged(const Entry& e) {
    return e.untracked || e.unmerged || e.unstaged != '.';
}

char stagedLetter(const Entry& e) { return e.staged; }

char unstagedLetter(const Entry& e) {
    if (e.untracked) return 'U';
    if (e.unmerged) return 'C';
    return e.unstaged;
}

std::string branchLabel(const Status& s) {
    std::string label;
    if (s.detached) {
        label = s.oid.empty() ? "HEAD (detached)"
                              : "HEAD (detached at " + s.oid.substr(0, 7) + ")";
    } else {
        label = s.branch;
    }
    if (s.hasAheadBehind) {
        if (s.ahead) label += " ↑" + std::to_string(s.ahead);
        if (s.behind) label += " ↓" + std::to_string(s.behind);
    }
    return label;
}

// -------------------------------------------------------------------- diff
std::string unquotePath(const std::string& s) {
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') return s;
    std::string out;
    for (size_t i = 1; i + 1 < s.size(); i++) {
        char c = s[i];
        if (c != '\\' || i + 2 >= s.size()) { out += c; continue; }
        char n = s[++i];
        switch (n) {
        case 'a': out += '\a'; break;
        case 'b': out += '\b'; break;
        case 't': out += '\t'; break;
        case 'n': out += '\n'; break;
        case 'v': out += '\v'; break;
        case 'f': out += '\f'; break;
        case 'r': out += '\r'; break;
        default:
            if (n >= '0' && n <= '7') {
                int v = n - '0';
                for (int k = 0; k < 2 && i + 2 < s.size() && s[i + 1] >= '0' &&
                                s[i + 1] <= '7'; k++)
                    v = v * 8 + (s[++i] - '0');
                out += (char)v;
            } else {
                out += n;   // \" and \\ and anything else: the character
            }
        }
    }
    return out;
}

std::vector<DiffLine> classifyDiff(const std::string& out) {
    std::vector<DiffLine> lines;
    bool inFile = false;
    // Inside a hunk: lines still expected from each parent and the result.
    std::vector<int> oldLeft, oldAt;
    int newLeft = 0, newAt = 0;
    size_t parents = 0;
    bool lastWasHunkLine = false;

    size_t pos = 0;
    while (pos < out.size()) {
        size_t nl = out.find('\n', pos);
        if (nl == std::string::npos) nl = out.size();
        DiffLine d{LineKind::Other, out.substr(pos, nl - pos)};
        pos = nl + 1;
        const std::string& t = d.text;

        bool inHunk = newLeft > 0;
        for (int n : oldLeft) inHunk = inHunk || n > 0;

        if (!t.empty() && t[0] == '\\' && lastWasHunkLine) {
            d.kind = LineKind::NoNewline;
            lines.push_back(d);
            continue;   // belongs to the line before; counts for nothing
        }
        if (inHunk) {
            // An empty line is a context line whose leading space was lost.
            std::string cols = t.substr(0, std::min(parents, t.size()));
            cols.resize(parents, ' ');
            bool minus = cols.find('-') != std::string::npos;
            bool plus = cols.find('+') != std::string::npos;
            bool valid = cols.find_first_not_of(" +-") == std::string::npos;
            if (valid) {
                d.kind = minus ? LineKind::Removed
                       : plus  ? LineKind::Added : LineKind::Context;
                for (size_t p = 0; p < parents; p++) {
                    if (cols[p] == '+') continue;   // not in this parent
                    if (p == 0) d.oldLine = oldAt[p];
                    oldAt[p]++;
                    oldLeft[p]--;
                }
                if (!minus) { d.newLine = newAt++; newLeft--; }
                lines.push_back(d);
                lastWasHunkLine = true;
                continue;
            }
            // Not a hunk line after all (a short hunk): fall through.
            newLeft = 0;
            oldLeft.assign(oldLeft.size(), 0);
        }
        lastWasHunkLine = false;

        if (startsWith(t, "diff ")) {
            inFile = true;
            d.kind = LineKind::FileHeader;
        } else if (inFile && startsWith(t, "@@")) {
            std::vector<std::pair<int, int>> olds;
            int ns = 0, nc = 0;
            std::string section;
            if (parseHunkHeader(t, olds, ns, nc, section)) {
                d.kind = LineKind::HunkHeader;
                parents = olds.size();
                oldLeft.clear(); oldAt.clear();
                for (auto& o : olds) { oldAt.push_back(o.first); oldLeft.push_back(o.second); }
                newAt = ns; newLeft = nc;
                lastWasHunkLine = true;   // a "\" line may follow an empty side
            } else {
                d.kind = LineKind::FileHeader;   // not a hunk git would write
            }
        } else if (inFile && startsWith(t, "Binary files ")) {
            d.kind = LineKind::Other;
        } else if (inFile) {
            d.kind = LineKind::FileHeader;
        }
        lines.push_back(d);
    }
    return lines;
}

std::vector<FileDiff> parseDiff(const std::string& out) {
    std::vector<FileDiff> files;
    for (DiffLine& d : classifyDiff(out)) {
        const std::string& t = d.text;
        if (d.kind == LineKind::FileHeader && startsWith(t, "diff ")) {
            files.emplace_back();
            FileDiff& f = files.back();
            if (startsWith(t, "diff --git ")) {
                gitHeaderPaths(t.substr(11), f.oldPath, f.newPath);
            } else {
                // "diff --cc path" / "diff --combined path"
                size_t sp = t.find(' ', 5);
                std::string p = sp == std::string::npos ? "" : headerPath(t.substr(sp + 1), "");
                f.oldPath = f.newPath = p;
            }
            f.header.push_back(d);
            continue;
        }
        if (files.empty()) continue;   // text before the first file
        FileDiff& f = files.back();
        if (d.kind == LineKind::HunkHeader) {
            Hunk h;
            std::vector<std::pair<int, int>> olds;
            parseHunkHeader(t, olds, h.newStart, h.newCount, h.section);
            if (!olds.empty()) { h.oldStart = olds[0].first; h.oldCount = olds[0].second; }
            h.lines.push_back(d);
            f.hunks.push_back(h);
            continue;
        }
        if (!f.hunks.empty() && d.kind != LineKind::FileHeader &&
            d.kind != LineKind::Other) {
            f.hunks.back().lines.push_back(d);
            continue;
        }
        if (startsWith(t, "--- ")) {
            f.oldPath = headerPath(t.substr(4), "a/");
            if (f.oldPath.empty()) f.isNew = true;
        } else if (startsWith(t, "+++ ")) {
            f.newPath = headerPath(t.substr(4), "b/");
            if (f.newPath.empty()) f.isDeleted = true;
        } else if (startsWith(t, "new file mode")) {
            f.isNew = true;
        } else if (startsWith(t, "deleted file mode")) {
            f.isDeleted = true;
        } else if (startsWith(t, "rename from ")) {
            f.isRename = true;
            f.oldPath = unquotePath(t.substr(12));
        } else if (startsWith(t, "rename to ")) {
            f.isRename = true;
            f.newPath = unquotePath(t.substr(10));
        } else if (startsWith(t, "Binary files ")) {
            f.binary = true;
        }
        f.header.push_back(d);
    }
    return files;
}

}  // namespace Git
