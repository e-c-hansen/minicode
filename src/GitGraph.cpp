// GitGraph.cpp — pure C++. See GitGraph.h.
#include "GitGraph.h"
#include <algorithm>

namespace Git {

namespace {

bool startsWith(const std::string& s, const char* prefix) {
    return s.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

// `s` cut at NUL into fields. A final field with no NUL after it is kept only
// when it is not empty, so "a\0b\0" gives two fields, as does "a\0b".
std::vector<std::string> nulFields(const std::string& s, size_t from = 0,
                                   size_t maxFields = std::string::npos,
                                   size_t* end = nullptr) {
    std::vector<std::string> out;
    size_t pos = from;
    while (pos < s.size() && out.size() < maxFields) {
        size_t z = s.find('\0', pos);
        if (z == std::string::npos) {
            if (maxFields != std::string::npos) break;   // a field must be ended
            out.push_back(s.substr(pos));
            pos = s.size();
            break;
        }
        out.push_back(s.substr(pos, z - pos));
        pos = z + 1;
    }
    if (end) *end = pos;
    return out;
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r')) i++;
        size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\n' && s[i] != '\r') i++;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

long long toNumber(const std::string& s) {
    long long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') break;
        if (v < 100000000000000LL) v = v * 10 + (c - '0');
    }
    return v;
}

}  // namespace

// -------------------------------------------------------------------- log
const char* logFormat() { return "%H%x00%P%x00%an%x00%at%x00%s"; }

std::vector<Commit> parseLog(const std::string& out) {
    std::vector<Commit> commits;
    std::vector<std::string> f = nulFields(out);
    // -z ends each commit with a NUL. An empty field where a hash belongs (a
    // stray separator) is skipped, since a hash is never empty.
    size_t i = 0;
    while (i < f.size()) {
        std::string h = f[i];
        while (!h.empty() && (h[0] == '\n' || h[0] == '\r')) h.erase(0, 1);
        if (h.empty()) { i++; continue; }
        if (i + 4 >= f.size()) break;   // cut short
        Commit c;
        c.hash = h;
        c.parents = words(f[i + 1]);
        c.author = f[i + 2];
        c.time = toNumber(f[i + 3]);
        c.subject = f[i + 4];
        commits.push_back(std::move(c));
        i += 5;
    }
    return commits;
}

// ------------------------------------------------------------------- refs
const char* refFormat() {
    return "%(objectname)%00%(*objectname)%00%(refname)%00%(symref)";
}

std::vector<Ref> parseRefs(const std::string& out) {
    std::vector<Ref> refs;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t nl = out.find('\n', pos);
        if (nl == std::string::npos) nl = out.size();
        std::string line = out.substr(pos, nl - pos);
        pos = nl + 1;
        std::vector<std::string> f = nulFields(line);
        if (f.size() < 3) continue;
        if (f.size() >= 4 && !f[3].empty()) continue;   // origin/HEAD and the like
        Ref r;
        r.target = f[1].empty() ? f[0] : f[1];          // an annotated tag, peeled
        const std::string& name = f[2];
        if (startsWith(name, "refs/heads/")) {
            r.kind = RefKind::Branch;
            r.name = name.substr(11);
        } else if (startsWith(name, "refs/remotes/")) {
            r.kind = RefKind::Remote;
            r.name = name.substr(13);
            // A remote's HEAD that is not a symref (some clones write one).
            if (r.name.size() >= 5 && r.name.compare(r.name.size() - 5, 5, "/HEAD") == 0)
                continue;
        } else if (startsWith(name, "refs/tags/")) {
            r.kind = RefKind::Tag;
            r.name = name.substr(10);
        } else {
            continue;   // refs/stash, refs/notes, ...
        }
        if (r.target.empty() || r.name.empty()) continue;
        refs.push_back(std::move(r));
    }
    return refs;
}

std::unordered_map<std::string, std::vector<Ref>>
labelsByCommit(const std::vector<Ref>& refs, const std::string& headOid,
               const std::string& branch, bool detached) {
    std::vector<Ref> all = refs;
    for (Ref& r : all)
        r.current = !detached && r.kind == RefKind::Branch && r.name == branch;
    if (detached && !headOid.empty()) {
        Ref h;
        h.kind = RefKind::Head;
        h.name = "HEAD";
        h.target = headOid;
        h.current = true;
        all.push_back(h);
    }
    auto rank = [](const Ref& r) {
        if (r.current) return 0;
        switch (r.kind) {
        case RefKind::Head: return 0;
        case RefKind::Branch: return 1;
        case RefKind::Remote: return 2;
        case RefKind::Tag: return 3;
        }
        return 4;
    };
    std::stable_sort(all.begin(), all.end(), [&](const Ref& a, const Ref& b) {
        int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        return a.name < b.name;
    });
    std::unordered_map<std::string, std::vector<Ref>> out;
    for (Ref& r : all) out[r.target].push_back(std::move(r));
    return out;
}

// ------------------------------------------------------ pushed or not
Divergence parseLeftRight(const std::string& out) {
    Divergence d;
    for (const std::string& w : words(out)) {
        if (w.size() < 2) continue;
        if (w[0] == '<') d.outgoing.insert(w.substr(1));
        else if (w[0] == '>') d.incoming.insert(w.substr(1));
    }
    return d;
}

// ------------------------------------------------------------------ lanes
std::vector<GraphRow> layoutGraph(const std::vector<Commit>& commits) {
    struct Slot {
        std::string want;   // the commit this lane is waiting for; empty = free
        int color = 0;
    };
    std::vector<GraphRow> rows;
    rows.reserve(commits.size());
    std::vector<Slot> lanes;   // the state at the top edge of the next row
    int nextColor = 0;

    for (const Commit& c : commits) {
        GraphRow row;
        const size_t topWidth = lanes.size();

        // Every lane waiting for this commit ends at its dot; the leftmost
        // carries on as the commit's own lane.
        std::vector<int> hits;
        for (size_t i = 0; i < lanes.size(); i++)
            if (lanes[i].want == c.hash) hits.push_back((int)i);
        int lane;
        if (hits.empty()) {
            // A branch tip: nothing above points here. Lanes at the top are
            // packed, so it goes at the right.
            lane = (int)lanes.size();
            lanes.push_back({c.hash, nextColor++});
        } else {
            lane = hits[0];
        }
        row.lane = lane;
        row.color = lanes[lane].color;
        for (int i : hits)
            row.edges.push_back({GraphEdge::In, i, lane, lanes[i].color});

        // The state at the bottom edge, before free lanes are closed up.
        std::vector<Slot> bottom = lanes;
        for (int i : hits) bottom[i] = Slot();
        bottom[lane] = Slot();
        std::vector<GraphEdge> outs;
        for (size_t k = 0; k < c.parents.size(); k++) {
            const std::string& p = c.parents[k];
            int to = -1;
            if (k == 0) {
                // The first parent continues the commit's own lane, even when
                // another lane already waits for it (the two meet there).
                to = lane;
                bottom[lane] = {p, row.color};
            } else {
                for (size_t j = 0; j < bottom.size(); j++)
                    if (bottom[j].want == p) { to = (int)j; break; }
                if (to < 0) {
                    for (size_t j = lane + 1; j < bottom.size(); j++)
                        if (bottom[j].want.empty()) { to = (int)j; break; }
                    if (to < 0) {
                        to = (int)bottom.size();
                        bottom.push_back(Slot());
                    }
                    bottom[to] = {p, nextColor++};
                }
            }
            bool dup = false;
            for (const GraphEdge& e : outs) dup = dup || e.to == to;
            if (!dup) outs.push_back({GraphEdge::Out, lane, to, bottom[to].color});
        }

        // Close up free lanes: everything right of a gap moves left.
        std::vector<int> newIndex(bottom.size(), -1);
        std::vector<Slot> packed;
        for (size_t j = 0; j < bottom.size(); j++) {
            if (bottom[j].want.empty()) continue;
            newIndex[j] = (int)packed.size();
            packed.push_back(bottom[j]);
        }
        for (size_t i = 0; i < topWidth; i++) {
            if (std::find(hits.begin(), hits.end(), (int)i) != hits.end()) continue;
            if (lanes[i].want.empty()) continue;
            row.edges.push_back({GraphEdge::Pass, (int)i, newIndex[i], lanes[i].color});
        }
        for (GraphEdge& e : outs) {
            e.to = newIndex[e.to];
            row.edges.push_back(e);
        }
        row.width = std::max({(int)topWidth, lane + 1, (int)packed.size()});
        lanes = std::move(packed);
        rows.push_back(std::move(row));
    }
    return rows;
}

// ------------------------------------------------------------------- show
const char* showFormat() {
    return "%H%x00%P%x00%an%x00%ae%x00%at%x00%cn%x00%ce%x00%B%x00";
}

bool parseShow(const std::string& out, CommitDetail& d) {
    size_t end = 0;
    std::vector<std::string> f = nulFields(out, 0, 8, &end);
    if (f.size() < 8 || f[0].size() < 4) return false;
    d.hash = f[0];
    d.parents = words(f[1]);
    d.author = f[2];
    d.email = f[3];
    d.time = toNumber(f[4]);
    d.committer = (f[5] == f[2] && f[6] == f[3]) ? std::string()
                  : f[5] + " <" + f[6] + ">";
    d.message = f[7];
    while (!d.message.empty() && (d.message.back() == '\n' || d.message.back() == '\r' ||
                                  d.message.back() == ' '))
        d.message.pop_back();
    std::string rest = out.substr(end);
    // git puts a newline after the format, and "---" before a --stat.
    size_t i = 0;
    while (i < rest.size() && rest[i] == '\n') i++;
    if (rest.compare(i, 4, "---\n") == 0) i += 4;
    d.patch = rest.substr(i);
    return true;
}

// ------------------------------------------------------------------- text
std::string relativeTime(long long then, long long now) {
    long long s = now - then;
    if (s < 45) return "just now";
    auto say = [](long long n, const char* unit) {
        return std::to_string(n) + " " + unit + (n == 1 ? "" : "s") + " ago";
    };
    const long long M = 60, H = 3600, D = 86400;
    if (s < 90) return "a minute ago";
    if (s < 45 * M) return say((s + M / 2) / M, "minute");
    if (s < 90 * M) return "an hour ago";
    if (s < 22 * H) return say((s + H / 2) / H, "hour");
    if (s < 36 * H) return "yesterday";
    if (s < 7 * D) return say((s + D / 2) / D, "day");
    if (s < 30 * D) return say(std::max(1LL, (s + 3 * D) / (7 * D)), "week");
    if (s < 365 * D) return say(std::max(1LL, (s + 15 * D) / (30 * D)), "month");
    return say(std::max(1LL, (s + 182 * D) / (365 * D)), "year");
}

std::string shortHash(const std::string& hash, size_t n) {
    return hash.substr(0, std::min(n, hash.size()));
}

}  // namespace Git
