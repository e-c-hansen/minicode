// GitModel.cpp — see GitModel.h.
#include "GitModel.h"

#include <algorithm>
#include <ctime>

namespace GitUi {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' ||
                     s[b - 1] == '\n'))
        --b;
    return s.substr(a, b - a);
}

std::vector<std::string> nonBlankLines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        if (nl == std::string::npos) nl = s.size();
        std::string t = trim(s.substr(start, nl - start));
        if (!t.empty()) out.push_back(t);
        start = nl + 1;
    }
    return out;
}

}  // namespace

// ------------------------------------------------------------------ rows
std::string Row::key() const {
    return std::string(staged ? "1:" : "0:") + (header ? title : path);
}

void applyStatus(Snapshot& s, const Git::Status& st) {
    s.initial = st.initial;
    s.headOid = st.oid;
    s.branch = st.branch;
    s.detached = st.detached;
    s.upstream = st.upstream;
    s.hasAheadBehind = st.hasAheadBehind;
    s.ahead = st.ahead;
    s.behind = st.behind;
    s.branchText = Git::branchLabel(st);
    if (st.initial) s.branchText += "  (no commits yet)";

    std::vector<Row> staged, changes;
    for (const Git::Entry& e : st.entries) {
        if (Git::isStaged(e)) {
            Row r;
            r.staged = true;
            r.letter = Git::stagedLetter(e);
            r.path = e.path;
            r.origPath = e.origPath;
            staged.push_back(r);
        }
        if (Git::hasUnstaged(e)) {
            Row r;
            r.letter = Git::unstagedLetter(e);
            r.path = e.path;
            r.untracked = e.untracked;
            r.unmerged = e.unmerged;
            changes.push_back(r);
        }
    }
    s.rows.clear();
    auto section = [&](const char* title, const std::vector<Row>& items, bool isStaged) {
        if (items.empty()) return;
        Row h;
        h.header = true;
        h.staged = isStaged;
        h.title = std::string(title) + "  " + std::to_string(items.size());
        s.rows.push_back(h);
        s.rows.insert(s.rows.end(), items.begin(), items.end());
    };
    section("Staged changes", staged, true);
    section("Changes", changes, false);
    if (s.rows.empty()) s.notice = "No changes.";
}

bool parseTopLevel(const std::string& out, std::string& top, std::string& gitDir) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < out.size()) {
        size_t nl = out.find('\n', start);
        if (nl == std::string::npos) nl = out.size();
        std::string l = out.substr(start, nl - start);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        start = nl + 1;
    }
    if (lines.empty() || lines[0].empty()) return false;
    top = lines[0];
    gitDir = lines.size() > 1 ? lines[1] : "";
    return true;
}

bool graphWanted(const Snapshot& s) {
    return s.inRepository && !s.initial && !s.headOid.empty() && s.errorText.empty();
}

std::string summaryText(const Snapshot& s) {
    if (s.detached) return "HEAD is detached, so there is no upstream to compare with.";
    if (s.upstream.empty())
        return (s.branch.empty() ? std::string("This branch") : s.branch) +
               " has no upstream, so nothing here is marked as pushed or not.";
    if (!s.hasAheadBehind) return "The upstream " + s.upstream + " is gone.";
    if (!s.ahead && !s.behind) return "Up to date with " + s.upstream + ".";
    std::string parts;
    if (s.ahead) parts = "↑ " + std::to_string(s.ahead) + " to push";
    if (s.behind) {
        if (!parts.empty()) parts += ", ";
        parts += "↓ " + std::to_string(s.behind) + " to pull";
    }
    return parts + ", against " + s.upstream;
}

std::string failureText(int status, const std::string& out, const std::string& err) {
    std::vector<std::string> e = nonBlankLines(err);
    if (!e.empty()) {
        std::string r;
        for (const std::string& l : e) r += (r.empty() ? "" : "\n") + l;
        return validUtf8(r);
    }
    std::vector<std::string> o = nonBlankLines(out);
    if (!o.empty()) return validUtf8(o.back());
    return "git exited with status " + std::to_string(status) + ".";
}

std::string firstLine(const std::string& out) {
    size_t nl = out.find('\n');
    return validUtf8(nl == std::string::npos ? out : out.substr(0, nl));
}

int nextFileRow(const std::vector<Row>& rows, int from, int step) {
    const int n = (int)rows.size();
    int r = from;
    if (r < 0) r = step > 0 ? -1 : n;
    for (r += step; r >= 0 && r < n; r += step)
        if (!rows[r].header) return r;
    return -1;
}

int lastFileRow(const std::vector<Row>& rows) {
    return nextFileRow(rows, -1, -1);
}

int pickAfterRefresh(const std::vector<Row>& oldRows, int sel, const std::vector<Row>& rows) {
    if (sel < 0 || sel >= (int)oldRows.size() || oldRows[sel].header) return -1;
    const Row& was = oldRows[sel];
    int wasIndex = 0;   // its place in its own list
    for (int i = sel; i >= 0 && !oldRows[i].header; --i) wasIndex = sel - i;
    const int n = (int)rows.size();
    for (int i = 0; i < n; ++i)
        if (!rows[i].header && rows[i].key() == was.key()) return i;
    int head = -1;
    for (int i = 0; i < n; ++i)
        if (rows[i].header && rows[i].staged == was.staged) { head = i; break; }
    if (head >= 0) {
        int count = 0;
        while (head + 1 + count < n && !rows[head + 1 + count].header) ++count;
        if (count) return head + 1 + std::min(wasIndex, count - 1);
    }
    for (int i = 0; i < n; ++i)
        if (!rows[i].header) return i;
    return -1;
}

std::string rowToolTip(const Row& r) {
    if (r.header) return "";
    const char* what = r.unmerged ? "conflict" : r.untracked ? "untracked"
                     : r.staged ? "staged" : "changed";
    if (!r.origPath.empty())
        return r.path + " (renamed from " + r.origPath + "), " + what;
    return r.path + ", " + what;
}

unsigned letterColor(char c, bool unmerged) {
    if (unmerged) return 0xE4676B;
    switch (c) {
    case 'M': case 'T': return 0xE2C08D;
    case 'A': return 0x81B88B;
    case 'D': return 0xC74E39;
    case 'R': case 'C': case 'U': return 0x73C991;
    default: return 0x9CA3AF;
    }
}

// ----------------------------------------------------------- arguments
std::vector<std::string> topLevelArgs() {
    return {"rev-parse", "--show-toplevel", "--absolute-git-dir"};
}

std::vector<std::string> statusArgs() {
    return {"status", "--porcelain=v2", "--branch", "-z", "--untracked-files=all"};
}

std::vector<std::string> refArgs() {
    return {"for-each-ref", std::string("--format=") + Git::refFormat(), "refs/heads",
            "refs/remotes", "refs/tags"};
}

std::vector<std::string> logArgs(int limit, bool all, bool compare) {
    std::vector<std::string> a = {"-c", "log.showSignature=false", "log", "-z",
                                  "--topo-order", "--no-color",
                                  std::string("--format=") + Git::logFormat(),
                                  "--max-count=" + std::to_string(limit)};
    if (all) { a.push_back("--branches"); a.push_back("--remotes"); }
    a.push_back("HEAD");
    if (compare) a.push_back("@{upstream}");
    a.push_back("--");
    return a;
}

std::vector<std::string> leftRightArgs() {
    return {"rev-list", "--left-right", "HEAD...@{upstream}", "--"};
}

std::vector<std::string> stageArgs(const Row& r, bool initial) {
    if (r.staged) {
        if (initial)
            return {"--literal-pathspecs", "rm", "--cached", "--force", "-q", "--", r.path};
        std::vector<std::string> a = {"--literal-pathspecs", "restore", "--staged", "--",
                                      r.path};
        if (!r.origPath.empty()) a.push_back(r.origPath);
        return a;
    }
    return {"--literal-pathspecs", "add", "-A", "--", r.path};
}

std::vector<std::string> diffArgs(const Row& r) {
    std::vector<std::string> a = {"-c", "core.quotePath=false", "--literal-pathspecs",
                                  "diff", "--no-color", "--no-ext-diff",
                                  "--src-prefix=a/", "--dst-prefix=b/"};
    if (r.untracked) {
        a.insert(a.end(), {"--no-index", "--", "/dev/null", r.path});
    } else if (r.staged) {
        a.insert(a.end(), {"--cached", "-M", "--", r.path});
        if (!r.origPath.empty()) a.push_back(r.origPath);
    } else {
        a.insert(a.end(), {"--", r.path});
    }
    return a;
}

std::vector<std::string> showArgs(const std::string& hash) {
    return {"-c", "core.quotePath=false", "show", "--no-color", "--no-ext-diff",
            "--src-prefix=a/", "--dst-prefix=b/", "-M", "--diff-merges=first-parent",
            "--stat", "--patch", std::string("--format=") + Git::showFormat(), hash, "--"};
}

std::vector<std::string> commitArgs(const std::string& message) {
    return {"commit", "-m", message};
}

const std::vector<EnvVar>& gitEnvironment() {
    static const std::vector<EnvVar> env = {
        {"GIT_OPTIONAL_LOCKS", "0"},   // status must not write the index
        {"GIT_TERMINAL_PROMPT", "0"},  // never wait for a password
        {"GIT_EDITOR", "true"},        // never wait for an editor
        {"GIT_PAGER", "cat"},
    };
    return env;
}

// ---------------------------------------------------------------- graph
bool compareWithUpstream(const Snapshot& s) {
    return !s.upstream.empty() && s.hasAheadBehind && !s.detached;
}

std::string graphKey(const Snapshot& s, int limit, bool all, bool compare,
                     const std::string& refsOutput) {
    return s.headOid + " " + s.branch + " " + (compare ? "1" : "0") + " " +
           std::to_string(limit) + " " + (all ? "1" : "0") + " " + s.upstream + "\n" +
           refsOutput;
}

void buildGraph(Graph& g, const Snapshot& s, const std::string& logOut,
                const std::string& refsOut, int limit) {
    g.limit = limit;
    g.commits = Git::parseLog(logOut);
    g.hasMore = (int)g.commits.size() >= limit;
    g.rows = Git::layoutGraph(g.commits);
    g.maxWidth = 1;
    for (const Git::GraphRow& r : g.rows) g.maxWidth = std::max(g.maxWidth, r.width);
    g.head = s.headOid;
    g.labels = Git::labelsByCommit(Git::parseRefs(refsOut), g.head, s.branch, s.detached);
}

double laneWidth(double rowWidth, int lanes) {
    if (lanes <= 0) return 12;
    return std::max(4.0, std::min(12.0, (rowWidth * 0.4 - 8) / lanes));
}

unsigned laneColor(int i) {
    static const unsigned palette[] = {0x59A4F9, 0xFFB000, 0xDC267F, 0x40B0A6,
                                       0xB66DFF, 0xE0823D, 0x8FCB5A};
    const int n = sizeof(palette) / sizeof(palette[0]);
    return palette[((i % n) + n) % n];
}

unsigned pillColor(Git::RefKind k) {
    switch (k) {
    case Git::RefKind::Head: return 0xEA5C00;
    case Git::RefKind::Remote: return 0xB180D7;
    case Git::RefKind::Tag: return 0xE2C08D;
    case Git::RefKind::Branch: default: return 0x59A4F9;
    }
}

std::string dateText(long long t) {
    time_t tt = (time_t)t;
    struct tm tmv;
    if (!localtime_r(&tt, &tmv)) return "";
    char buf[64];
    if (!strftime(buf, sizeof buf, "%b %e, %Y at %H:%M", &tmv)) return "";
    std::string s = buf;
    size_t dbl = s.find("  ");   // %e pads a one-digit day with a space
    if (dbl != std::string::npos) s.erase(dbl, 1);
    return s;
}

std::string graphToolTip(const Graph& g, size_t row, long long now) {
    if (row >= g.commits.size())
        return "Load the next " + std::to_string(kGraphBatch) + " commits";
    const Git::Commit& c = g.commits[row];
    std::string t = Git::shortHash(c.hash) + "  " + validUtf8(c.author) + ", " +
                    Git::relativeTime(c.time, now) + " (" + dateText(c.time) + ")\n" +
                    validUtf8(c.subject);
    if (g.divergence.outgoing.count(c.hash))
        t += "\nNot pushed yet: on this branch but not its upstream.";
    if (g.divergence.incoming.count(c.hash))
        t += "\nNot pulled yet: on the upstream but not this branch.";
    auto it = g.labels.find(c.hash);
    if (it != g.labels.end()) {
        std::string names;
        for (const Git::Ref& r : it->second) names += (names.empty() ? "" : ", ") + r.name;
        t += "\n" + validUtf8(names);
    }
    return t;
}

std::vector<std::string> graphDescriptions(const Graph& g) {
    std::vector<std::string> out;
    for (size_t i = 0; i < g.commits.size(); ++i) {
        const Git::Commit& c = g.commits[i];
        std::string line = Git::shortHash(c.hash) + " lane=" + std::to_string(g.rows[i].lane);
        if (c.hash == g.head) line += " head";
        if (g.divergence.outgoing.count(c.hash)) line += " out";
        if (g.divergence.incoming.count(c.hash)) line += " in";
        line += " " + c.subject;
        auto it = g.labels.find(c.hash);
        if (it != g.labels.end()) {
            std::string names;
            for (const Git::Ref& r : it->second)
                names += (names.empty() ? "" : ",") + std::string(r.current ? "*" : "") + r.name;
            line += " [" + names + "]";
        }
        out.push_back(line);
    }
    if (g.hasMore) out.push_back("(more)");
    return out;
}

// -------------------------------------------------------- styled text
namespace {
// Length of the valid UTF-8 sequence at s[i], or 0.
size_t seqLen(const std::string& s, size_t i) {
    const unsigned char c = (unsigned char)s[i];
    size_t n;
    unsigned min;
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) { n = 2; min = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; min = 0x10000; }
    else return 0;
    if (i + n > s.size()) return 0;
    unsigned cp = c & (0xFF >> (n + 1));
    for (size_t k = 1; k < n; ++k) {
        const unsigned char d = (unsigned char)s[i + k];
        if ((d & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (d & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return n;
}
}  // namespace

std::string validUtf8(const std::string& s) {
    bool ok = true;
    for (size_t i = 0; i < s.size();) {
        size_t n = seqLen(s, i);
        if (!n || s[i] == '\0') { ok = false; break; }
        i += n;
    }
    if (ok) return s;
    std::string out;
    out.reserve(s.size() + s.size() / 4);
    for (unsigned char c : s) {
        if (c == 0) continue;   // GtkTextBuffer cannot hold a NUL
        if (c < 0x80) {
            out += (char)c;
        } else {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

size_t utf8Length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

void StyledText::add(const std::string& s, Style style) {
    if (s.empty()) return;
    const size_t len = utf8Length(s);
    if (!runs.empty() && runs.back().style == style &&
        runs.back().start + runs.back().length == chars)
        runs.back().length += len;
    else
        runs.push_back({chars, len, style});
    text += s;
    chars += len;
}

void appendDiff(StyledText& out, const std::string& input) {
    if (input.empty()) {
        out.add("No differences to show.", Style::Muted);
        return;
    }
    std::string bytes = input;
    bool cut = false;
    if (bytes.size() > kMaxDiffBytes) {
        size_t nl = bytes.rfind('\n', kMaxDiffBytes);
        bytes.resize(nl == std::string::npos ? kMaxDiffBytes : nl + 1);
        cut = true;
    }
    for (const Git::DiffLine& l : Git::classifyDiff(bytes)) {
        Style st = Style::Plain;
        switch (l.kind) {
        case Git::LineKind::Added: st = Style::Added; break;
        case Git::LineKind::Removed: st = Style::Removed; break;
        case Git::LineKind::FileHeader: st = Style::FileHeader; break;
        case Git::LineKind::HunkHeader:
        case Git::LineKind::NoNewline:
        case Git::LineKind::Other: st = Style::Muted; break;
        case Git::LineKind::Context: st = Style::Plain; break;
        }
        out.add(validUtf8(l.text) + "\n", st);
    }
    if (cut) out.add("\n(The diff is longer than 4 MB; the rest is not shown.)\n", Style::Muted);
}

StyledText diffText(const std::string& bytes) {
    StyledText out;
    appendDiff(out, bytes);
    return out;
}

StyledText commitText(const std::string& show, long long now) {
    Git::CommitDetail d;
    if (!Git::parseShow(show, d)) return diffText(show);
    StyledText out;
    auto line = [&](const std::string& name, const std::string& text) {
        std::string label = name;
        if (label.size() < 10) label.append(10 - label.size(), ' ');
        out.add(label, Style::Muted);
        out.add(validUtf8(text) + "\n", Style::Plain);
    };
    out.add("commit    ", Style::Muted);
    out.add(validUtf8(d.hash), Style::Hash);
    out.add("\n", Style::Plain);
    std::string parents;
    for (const std::string& p : d.parents) parents += (parents.empty() ? "" : " ") + Git::shortHash(p);
    if (d.parents.size() > 1) line("Merge", parents);
    else if (d.parents.size() == 1) line("Parent", parents);
    line("Author", d.author + " <" + d.email + ">");
    if (!d.committer.empty()) line("Committer", d.committer);
    line("Date", dateText(d.time) + " (" + Git::relativeTime(d.time, now) + ")");
    out.add("\n", Style::Plain);
    const std::string msg = validUtf8(d.message);
    const size_t nl = msg.find('\n');
    out.add(nl == std::string::npos ? msg : msg.substr(0, nl), Style::Bold);
    if (nl != std::string::npos) out.add(msg.substr(nl), Style::Plain);
    out.add("\n\n", Style::Plain);
    if (d.parents.size() > 1)
        out.add("Changes against the first parent, which is what the merge brought in.\n\n",
                Style::Muted);
    if (d.patch.empty()) out.add("No changes in this commit.\n", Style::Muted);
    else appendDiff(out, d.patch);
    return out;
}

std::string commitTitle(const Git::Commit& c) {
    return Git::shortHash(c.hash) + " " + validUtf8(c.subject);
}

}  // namespace GitUi
