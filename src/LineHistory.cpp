// LineHistory.cpp — see LineHistory.h.
#include "LineHistory.h"
#include "AgentIndex.h"
#include "GitGraph.h"
#include "LineDiff.h"
#include "Text.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>

namespace LineHistory {

using AgentEdits::Change;
using AgentEdits::Tool;

// ------------------------------------------------------------------ blame
bool Commit::uncommitted() const {
    if (hash.empty()) return true;
    for (char c : hash)
        if (c != '0') return false;
    return true;
}

static bool isHex40(const std::string &s, size_t at) {
    if (s.size() < at + 40) return false;
    for (size_t i = at; i < at + 40; i++)
        if (!std::isxdigit((unsigned char)s[i])) return false;
    return s.size() == at + 40 || s[at + 40] == ' ';
}

bool parsePorcelain(const std::string &out, Blame *blame) {
    *blame = Blame();
    std::map<std::string, int> byHash;
    int current = -1;       // the commit of the entry being read
    long finalLine = -1;    // its line in the file, 1-based
    bool any = false;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t nl = out.find('\n', pos);
        if (nl == std::string::npos) nl = out.size();
        const std::string line = out.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) continue;
        if (line[0] == '\t') {
            // The line's text: the entry ends here.
            if (current < 0 || finalLine < 1) return false;
            const size_t at = (size_t)finalLine - 1;
            if (blame->lineCommit.size() <= at) blame->lineCommit.resize(at + 1, -1);
            blame->lineCommit[at] = current;
            current = -1;
            continue;
        }
        if (current < 0) {
            // "<hash> <original line> <final line> [<lines in group>]"
            if (!isHex40(line, 0)) return false;
            const std::string hash = line.substr(0, 40);
            const char *p = line.c_str() + 40;
            char *end = nullptr;
            std::strtol(p, &end, 10);   // the original line
            if (end == p) return false;
            const char *q = end;
            finalLine = std::strtol(q, &end, 10);
            if (end == q || finalLine < 1) return false;
            auto it = byHash.find(hash);
            if (it == byHash.end()) {
                Commit c;
                c.hash = hash;
                blame->commits.push_back(c);
                it = byHash.emplace(hash, (int)blame->commits.size() - 1).first;
            }
            current = it->second;
            any = true;
            continue;
        }
        // A header field of the current commit.
        Commit &c = blame->commits[(size_t)current];
        const size_t sp = line.find(' ');
        const std::string key = line.substr(0, sp);
        const std::string value = sp == std::string::npos ? std::string() : line.substr(sp + 1);
        if (key == "author") c.author = value;
        else if (key == "author-mail") c.authorMail = value;
        else if (key == "author-time") c.time = std::atoll(value.c_str());
        else if (key == "summary") c.summary = value;
        else if (key == "boundary") c.boundary = true;
    }
    if (current >= 0) return false;   // a header with no text line after it
    for (int c : blame->lineCommit)
        if (c < 0) return false;      // a line no entry gave
    return any || out.empty() || out.find_first_not_of("\n") == std::string::npos;
}

// ------------------------------------------------------ session edits
std::vector<size_t> lineStarts(const std::string &text) {
    std::vector<size_t> s;
    if (text.empty()) return s;
    s.push_back(0);
    for (size_t i = 0; i + 1 < text.size(); i++)
        if (text[i] == '\n') s.push_back(i + 1);
    return s;
}

size_t lineCount(const std::string &text) { return lineStarts(text).size(); }

static size_t lineOf(const std::vector<size_t> &starts, size_t offset) {
    auto it = std::upper_bound(starts.begin(), starts.end(), offset);
    return it == starts.begin() ? 0 : (size_t)(it - starts.begin()) - 1;
}

namespace {

// The walk's state: a text as it was at some point in the session, and for
// each of its lines the line of the file now it still is (or -1).
struct Walk {
    const std::string *current = nullptr;
    std::string text;
    std::vector<size_t> starts;
    std::vector<long> toNow;
    bool pristine = true;   // text is the file now, line for line

    void reset() {
        text = *current;
        starts = lineStarts(text);
        toNow.resize(starts.size());
        for (size_t i = 0; i < toNow.size(); i++) toNow[i] = (long)i;
        pristine = true;
    }
};

// For each line of `fresh` (the text just after a change), the line of
// `stale` (just before it) it is, or -1 when the change wrote it.
std::vector<long> matchLines(const std::string &stale, const std::string &fresh) {
    const std::u16string a = Model::utf16FromUtf8(stale), b = Model::utf16FromUtf8(fresh);
    const std::vector<LineDiff::Hunk> hunks = LineDiff::diff(a, b, 40000, 0.25, false);
    const size_t nb = lineCount(fresh);
    std::vector<long> m(nb, -1);
    size_t i = 0, j = 0;   // old and new lines outside the hunks pair up in turn
    for (const LineDiff::Hunk &h : hunks) {
        while (j < h.newLine && j < nb) m[j++] = (long)i++;
        i = h.oldLine + h.oldLines;
        j = h.newLine + h.newLines;
    }
    while (j < nb) m[j++] = (long)i++;
    return m;
}

// Undoes one Edit (or MultiEdit part) in the walk, crediting the lines it
// wrote. False when it cannot be undone exactly.
bool undoEdit(Walk &w, const Change &c, int index, std::vector<int> &out) {
    if (c.newText.empty()) return false;   // only removed text: nothing to find
    // Found exactly once (AgentEdits::occurrences' rule, in two finds).
    const size_t at = w.text.find(c.newText);
    if (at == std::string::npos) return false;
    const size_t end = at + c.newText.size();
    if (w.text.find(c.newText, end) != std::string::npos) return false;
    const size_t first = lineOf(w.starts, at);
    size_t last = lineOf(w.starts, end - 1);
    auto lineEnd = [&](size_t l) { return l + 1 < w.starts.size() ? w.starts[l + 1] : w.text.size(); };
    const size_t ls = w.starts[first];
    std::string oldRegion;
    for (;;) {
        const size_t le = lineEnd(last);
        oldRegion = w.text.substr(ls, at - ls) + c.oldText + w.text.substr(end, le - end);
        // The text put back must end its last line, or it would join the
        // line after it: take that line into the region too.
        if (oldRegion.empty() || oldRegion.back() == '\n' || le >= w.text.size()) break;
        last++;
    }
    const size_t le = lineEnd(last);
    const std::string newRegion = w.text.substr(ls, le - ls);
    const std::vector<long> m = matchLines(oldRegion, newRegion);
    for (size_t j = 0; j < m.size(); j++) {
        if (m[j] >= 0) continue;
        const long now = w.toNow[first + j];
        if (now >= 0 && out[(size_t)now] < 0) out[(size_t)now] = index;
    }
    // The text before the change, and where its lines are now.
    const size_t oldLines = lineCount(oldRegion);
    std::vector<long> regionNow(oldLines, -1);
    for (size_t j = 0; j < m.size(); j++)
        if (m[j] >= 0 && (size_t)m[j] < oldLines) regionNow[(size_t)m[j]] = w.toNow[first + j];
    std::vector<long> toNow;
    toNow.reserve(w.toNow.size() - m.size() + oldLines);
    toNow.insert(toNow.end(), w.toNow.begin(), w.toNow.begin() + (long)first);
    toNow.insert(toNow.end(), regionNow.begin(), regionNow.end());
    toNow.insert(toNow.end(), w.toNow.begin() + (long)last + 1, w.toNow.end());
    std::vector<size_t> starts;
    starts.reserve(toNow.size());
    starts.insert(starts.end(), w.starts.begin(), w.starts.begin() + (long)first);
    for (size_t s : lineStarts(oldRegion)) starts.push_back(ls + s);
    const long delta = (long)oldRegion.size() - (long)newRegion.size();
    for (size_t l = last + 1; l < w.starts.size(); l++) starts.push_back((size_t)((long)w.starts[l] + delta));
    w.text.replace(ls, le - ls, oldRegion);
    // A region put back empty at the very end leaves no line there.
    w.starts = std::move(starts);
    w.toNow = std::move(toNow);
    if (w.starts.size() != w.toNow.size() || (w.text.empty() != w.starts.empty())) {
        // Never expected; recount rather than trust a wrong map.
        w.starts = lineStarts(w.text);
        w.toNow.assign(w.starts.size(), -1);
    }
    w.pristine = false;
    return true;
}

}  // namespace

std::vector<int> linesWritten(const std::vector<Change> &changes, const std::string &current) {
    std::vector<int> out(lineCount(current), -1);
    Walk w;
    w.current = &current;
    w.reset();
    for (size_t k = changes.size(); k-- > 0;) {
        const Change &c = changes[k];
        if (c.refused) continue;
        for (int attempt = 0; attempt < 2; attempt++) {
            bool undone = false;
            switch (c.tool) {
            case Tool::Edit:
            case Tool::MultiEdit:
                undone = undoEdit(w, c, (int)k, out);
                break;
            case Tool::Write:
                // A file the session made: every line still there from it
                // is its. A rewrite of a file it did not make holds no text
                // to put back, so nothing is credited to it.
                if (c.created && w.text == c.newText) {
                    for (long now : w.toNow)
                        if (now >= 0 && out[(size_t)now] < 0) out[(size_t)now] = (int)k;
                    w.text.clear();
                    w.starts.clear();
                    w.toNow.clear();
                    w.pristine = false;
                    undone = true;
                }
                break;
            case Tool::NotebookEdit:
                break;
            }
            if (undone) break;
            // Not undone: start again from the file now, once, for this
            // change and the ones before it.
            if (w.pristine) break;
            w.reset();
        }
    }
    return out;
}

// ------------------------------------------------------- the answer
std::vector<Line> combine(size_t lineCount, const Blame *blame,
                          const std::vector<SessionLines> &sessions) {
    std::vector<Line> out(lineCount);
    const bool known = blame == nullptr || blame->lineCommit.size() == lineCount;
    for (size_t i = 0; i < lineCount; i++) {
        Line &l = out[i];
        if (!known) continue;   // Unknown: blame was for another text
        if (blame) {
            const int ci = blame->lineCommit[i];
            if (ci >= 0 && (size_t)ci < blame->commits.size() &&
                !blame->commits[(size_t)ci].uncommitted()) {
                l.kind = Kind::Commit;
                l.commit = ci;
                continue;
            }
        }
        double best = -1;
        for (size_t s = 0; s < sessions.size(); s++) {
            const SessionLines &sl = sessions[s];
            if (!sl.changes || i >= sl.lines.size()) continue;
            const int k = sl.lines[i];
            if (k < 0 || (size_t)k >= sl.changes->size()) continue;
            const double t = (*sl.changes)[(size_t)k].time;
            if (t > best || l.session < 0) {
                best = t;
                l.session = (int)s;
                l.change = k;
            }
        }
        l.kind = l.session >= 0 ? Kind::Session : Kind::NotCommitted;
    }
    return out;
}

// --------------------------------------------------------------- words
std::string promptLine(const std::string &prompt, size_t max) {
    size_t pos = 0;
    while (pos <= prompt.size()) {
        size_t nl = prompt.find('\n', pos);
        if (nl == std::string::npos) nl = prompt.size();
        const std::string said = AgentIndex::describePrompt(prompt.substr(pos, nl - pos), max);
        if (!said.empty()) return said;
        pos = nl + 1;
    }
    return "";
}

static std::string sessionName(const SessionInfo &s) {
    return s.title.empty() ? std::string("a Claude Code session") : s.title;
}

std::string commitLabel(const Commit &c, long long now) {
    std::string s = Git::shortHash(c.hash) + " " + (c.author.empty() ? "unknown" : c.author) +
                    ", " + Git::relativeTime(c.time, now);
    if (!c.summary.empty()) s += " \xC2\xB7 " + c.summary;   // " · "
    return s;
}

std::string sessionLabel(const SessionInfo &s, const Change &c, long long now) {
    return "Claude Code: " + sessionName(s) + ", " + Git::relativeTime((long long)c.time, now);
}

std::string notCommittedLabel() { return "Not committed"; }

std::string commitDetail(const Commit &c, long long now, DateFormat date) {
    std::string s = Git::shortHash(c.hash) + " \xC2\xB7 " + (c.author.empty() ? "unknown" : c.author) +
                    " \xC2\xB7 " + Git::relativeTime(c.time, now);
    const std::string when = date ? date(c.time) : std::string();
    if (!when.empty()) s += " (" + when + ")";
    s += "\n" + (c.summary.empty() ? std::string("(no subject)") : c.summary);
    s += "\nClick to open the commit.";
    return s;
}

std::string sessionDetail(const SessionInfo &s, const Change &c, long long now, DateFormat date) {
    std::string out = "Written by a Claude Code session, not committed\n" + sessionName(s);
    const std::string prompt = promptLine(c.prompt);
    if (!prompt.empty()) out += "\nPrompt: " + prompt;
    out += std::string("\n") + (c.tool == Tool::Write ? "Written " : "Edited ") +
           Git::relativeTime((long long)c.time, now);
    const std::string when = date ? date((long long)c.time) : std::string();
    if (!when.empty()) out += " (" + when + ")";
    out += "\nClick to see what the session changed in this file.";
    return out;
}

std::string notCommittedDetail() {
    return "Not committed\nNo Claude Code session's edit wrote this line.";
}

}  // namespace LineHistory
