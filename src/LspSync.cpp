// LspSync.cpp — see LspClient.h. Keeping the servers' copies of the files
// right: the ChangeTracker's regions, the ranged contentChanges made from
// them, and watched files (globs, the servers' watchers, what FSEvents
// reports turned into changes, coalescing, and what is never watched).
#include "LspClient.h"
#include <algorithm>
#include <iterator>
#include <map>

namespace Lsp {

// ------------------------------------------------------ incremental sync
void ChangeTracker::edit(size_t start, size_t removed, size_t inserted) {
    if (overflowed_) return;
    const size_t end = start + removed;
    // The regions this edit touches or overlaps, in the text now: [i, j).
    size_t i = 0;
    while (i < regions_.size() && regions_[i].newEnd < start) i++;
    size_t j = i;
    while (j < regions_.size() && regions_[j].newStart <= end) j++;
    Region m;
    if (i == j) {
        // Untouched text: where it was in the text as sent follows from
        // the region before it (the text between them is unchanged).
        const size_t oldAt = i == 0 ? start : regions_[i - 1].oldEnd + (start - regions_[i - 1].newEnd);
        m = Region{oldAt, oldAt + removed, start, start + inserted};
    } else {
        const Region& first = regions_[i];
        const Region& last = regions_[j - 1];
        const size_t newStart = std::min(start, first.newStart);
        const size_t newEnd = std::max(end, last.newEnd);
        m.newStart = newStart;
        m.oldStart = first.oldStart - (first.newStart - newStart);
        m.oldEnd = last.oldEnd + (newEnd - last.newEnd);
        m.newEnd = newEnd - removed + inserted;
    }
    const long delta = (long)inserted - (long)removed;
    for (size_t k = j; k < regions_.size(); k++) {
        regions_[k].newStart = (size_t)((long)regions_[k].newStart + delta);
        regions_[k].newEnd = (size_t)((long)regions_[k].newEnd + delta);
    }
    regions_.erase(regions_.begin() + (long)i, regions_.begin() + (long)j);
    regions_.insert(regions_.begin() + (long)i, m);
    if (regions_.size() > kMaxRegions) {
        regions_.clear();
        overflowed_ = true;
    }
}

namespace {
// Walks UTF-8 text in UTF-16 units, keeping the byte offset and the LSP
// position (lines end at \n, \r\n or \r, as PositionIndex counts them).
struct Utf8Walk {
    const std::string& s;
    size_t byte = 0, unit = 0, lineStart = 0;
    int line = 0;
    explicit Utf8Walk(const std::string& text) : s(text) {}
    // On to `target` units; false past the end, inside a character, or
    // between \r and \n.
    bool to(size_t target) {
        while (unit < target) {
            if (byte >= s.size()) return false;
            const unsigned char c = (unsigned char)s[byte];
            if (c == '\n') {
                byte++, unit++, line++, lineStart = unit;
                continue;
            }
            if (c == '\r') {
                if (byte + 1 < s.size() && s[byte + 1] == '\n') {
                    if (unit + 1 == target) return false;
                    byte += 2, unit += 2;
                } else {
                    byte++, unit++;
                }
                line++, lineStart = unit;
                continue;
            }
            size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
                     : (c & 0xF8) == 0xF0 ? 4 : 1;
            const size_t units = n == 4 ? 2 : 1;
            if (unit + units > target) return false;
            if (byte + n > s.size()) n = s.size() - byte;
            byte += n, unit += units;
        }
        return true;
    }
    Position position() const { return Position{line, (int)(unit - lineStart)}; }
    // Between \r and \n: the next unit is a \n after a \r already passed.
    bool betweenCrLf() const {
        return byte > 0 && byte < s.size() && s[byte - 1] == '\r' && s[byte] == '\n';
    }
};
}  // namespace

bool rangedChanges(const std::string& before, const std::string& after,
                   const std::vector<ChangeTracker::Region>& regions,
                   std::vector<ContentChange>& out) {
    out.clear();
    if (regions.empty()) return before == after;
    struct Placed { Range range; size_t oldA, oldB, newA, newB; };
    std::vector<Placed> placed;
    placed.reserve(regions.size());
    Utf8Walk old(before);
    size_t oldEndByte = 0, newEndByte = 0;   // where the last region ended, in bytes
    size_t lastOldEnd = 0, lastNewEnd = 0;   // ...and in units
    for (size_t k = 0; k < regions.size(); k++) {
        const ChangeTracker::Region& r = regions[k];
        if (r.oldEnd < r.oldStart || r.newEnd < r.newStart) return false;
        if (k > 0 && (r.oldStart < lastOldEnd || r.newStart < lastNewEnd)) return false;
        // The text between regions is the same in both, so it is as long.
        if (r.oldStart - lastOldEnd != r.newStart - lastNewEnd) return false;
        if (!old.to(r.oldStart)) return false;
        Placed p;
        p.range.start = old.position();
        p.oldA = old.byte;
        // The gap's bytes, at the same distance on in the new text.
        p.newA = newEndByte + (old.byte - oldEndByte);
        if (p.newA > after.size()) return false;
        if (before.compare(oldEndByte, old.byte - oldEndByte, after, newEndByte,
                           old.byte - oldEndByte) != 0)
            return false;
        if (!old.to(r.oldEnd) || old.betweenCrLf()) return false;
        p.range.end = old.position();
        p.oldB = old.byte;
        Utf8Walk now(after);
        now.byte = p.newA;
        now.unit = 0;
        if (!now.to(r.newEnd - r.newStart)) return false;
        p.newB = now.byte;
        placed.push_back(p);
        oldEndByte = p.oldB, newEndByte = p.newB;
        lastOldEnd = r.oldEnd, lastNewEnd = r.newEnd;
    }
    // The text after the last region.
    if (before.size() - oldEndByte != after.size() - newEndByte) return false;
    if (before.compare(oldEndByte, std::string::npos, after, newEndByte, std::string::npos) != 0)
        return false;
    // A boundary of the new text between \r and \n would make a server
    // that splits lines on its own read a line break that is not there.
    for (const Placed& p : placed) {
        if (p.newA > 0 && p.newA < after.size() && after[p.newA - 1] == '\r' && after[p.newA] == '\n')
            return false;
        if (p.newB > 0 && p.newB < after.size() && after[p.newB - 1] == '\r' && after[p.newB] == '\n')
            return false;
    }
    out.reserve(placed.size());
    for (size_t k = placed.size(); k-- > 0;)
        out.push_back(ContentChange{placed[k].range,
                                    after.substr(placed[k].newA, placed[k].newB - placed[k].newA)});
    return true;
}

std::u16string applyContentChanges(const std::u16string& text,
                                   const std::vector<ContentChange>& changes) {
    std::u16string out = text;
    for (const ContentChange& c : changes) {
        size_t a = offsetForPosition(out, c.range.start), b = offsetForPosition(out, c.range.end);
        if (b < a) std::swap(a, b);
        out.replace(a, b - a, toUtf16(c.text));
    }
    return out;
}

SyncedText::SyncedText(const std::u16string& text) : size_(text.size()) {
    chunks_.reserve(text.size() / kChunk + 1);
    for (size_t at = 0; at < text.size(); at += kChunk) {
        Chunk c;
        c.text = text.substr(at, kChunk);
        chunks_.push_back(std::move(c));
    }
    for (size_t k = 0; k < chunks_.size(); k++) rescan(k);
    rebuild();
}

void SyncedText::add(std::vector<size_t>& tree, size_t k, long delta) {
    if (!delta) return;
    for (size_t i = k + 1; i < tree.size(); i += i & (~i + 1))
        tree[i] = (size_t)((long)tree[i] + delta);
}

size_t SyncedText::before(const std::vector<size_t>& tree, size_t k) {
    size_t sum = 0;
    for (size_t i = k; i > 0; i -= i & (~i + 1)) sum += tree[i];
    return sum;
}

size_t SyncedText::find(const std::vector<size_t>& tree, size_t value) const {
    const size_t n = chunks_.size();
    if (n == 0) return 0;
    size_t step = 1;
    while (step * 2 <= n) step *= 2;
    size_t pos = 0, rest = value;
    for (; step; step /= 2)
        if (pos + step <= n && tree[pos + step] <= rest) {
            pos += step;
            rest -= tree[pos];
        }
    return std::min(pos, n - 1);
}

void SyncedText::rebuild() {
    const size_t n = chunks_.size();
    units_.assign(n + 1, 0);
    startsIn_.assign(n + 1, 0);
    startCount_ = 0;
    for (size_t k = 0; k < n; k++) {
        units_[k + 1] += chunks_[k].text.size();
        startsIn_[k + 1] += chunks_[k].starts.size();
        startCount_ += chunks_[k].starts.size();
        const size_t up = (k + 1) + ((k + 1) & (~(k + 1) + 1));
        if (up <= n) {
            units_[up] += units_[k + 1];
            startsIn_[up] += startsIn_[k + 1];
        }
    }
}

// A line starts after \n, and after a \r not followed by \n (the unit after
// a chunk's last is the next chunk's first).
void SyncedText::rescan(size_t k) {
    Chunk& c = chunks_[k];
    c.starts.clear();
    const size_t n = c.text.size();
    const char16_t* t = c.text.data();
    for (size_t i = 0; i < n; i++) {
        if (t[i] == u'\n') {
            c.starts.push_back((uint32_t)(i + 1));
        } else if (t[i] == u'\r') {
            const bool nextIsNewline = i + 1 < n ? t[i + 1] == u'\n'
                                       : k + 1 < chunks_.size() && chunks_[k + 1].text[0] == u'\n';
            if (!nextIsNewline) c.starts.push_back((uint32_t)(i + 1));
        }
    }
}

Position SyncedText::position(size_t offset) const {
    offset = std::min(offset, size_);
    if (offset == 0) return Position{0, 0};
    // The starts at or before `offset` are those of the chunks before the
    // one holding the unit before it, and that chunk's own up to it.
    const size_t k = chunkAt(offset - 1);
    const Chunk& c = chunks_[k];
    const size_t chunkStart = before(units_, k);
    auto it = std::upper_bound(c.starts.begin(), c.starts.end(), (uint32_t)(offset - chunkStart));
    const size_t inChunk = (size_t)(it - c.starts.begin());
    const size_t earlier = before(startsIn_, k);
    size_t start = 0;
    if (inChunk) {
        start = chunkStart + c.starts[inChunk - 1];
    } else if (earlier) {
        // The chunk holding the last start before this one.
        const size_t j = find(startsIn_, earlier - 1);
        start = before(units_, j) + chunks_[j].starts.back();
    }
    return Position{(int)(earlier + inChunk), (int)(offset - start)};
}

void SyncedText::replace(size_t a, size_t b, const std::u16string& with) {
    a = std::min(a, size_);
    b = std::min(std::max(a, b), size_);
    if (a == b && with.empty()) return;
    snapshot_.reset();
    if (chunks_.empty()) {
        *this = SyncedText(with);
        return;
    }
    size_t ka = chunkAt(a), kb = chunkAt(b);
    std::u16string merged = chunks_[ka].text.substr(0, a - before(units_, ka));
    merged += with;
    merged.append(chunks_[kb].text, b - before(units_, kb), std::u16string::npos);
    // A small piece joins a neighbour, so chunks stay a quarter full at
    // least and their number follows the text's size.
    const size_t kMin = kChunk / 4, kMax = kChunk * 2;
    if (merged.size() < kMin && kb + 1 < chunks_.size()) {
        merged += chunks_[++kb].text;
    } else if (merged.size() < kMin && ka > 0) {
        merged.insert(0, chunks_[--ka].text);
    }
    const size_t count = merged.empty()        ? 0
                         : merged.size() <= kMax ? 1
                                                 : (merged.size() + kChunk - 1) / kChunk;
    size_ = size_ - (b - a) + with.size();
    auto piece = [&](size_t i, size_t& at) {
        const size_t len = (merged.size() - at) / (count - i);
        std::u16string p = merged.substr(at, len);
        at += len;
        return p;
    };
    const size_t first = ka > 0 ? ka - 1 : 0;   // may end in a \r meeting another unit
    if (count == kb - ka + 1) {
        // As many chunks as before: rewritten in place, the trees adjusted.
        size_t at = 0;
        for (size_t i = 0; i < count; i++) {
            Chunk& c = chunks_[ka + i];
            const long was = (long)c.text.size();
            c.text = piece(i, at);
            add(units_, ka + i, (long)c.text.size() - was);
        }
        for (size_t k = first; k < ka + count; k++) {
            const long was = (long)chunks_[k].starts.size();
            rescan(k);
            const long delta = (long)chunks_[k].starts.size() - was;
            add(startsIn_, k, delta);
            startCount_ = (size_t)((long)startCount_ + delta);
        }
        return;
    }
    std::vector<Chunk> pieces(count);
    size_t at = 0;
    for (size_t i = 0; i < count; i++) pieces[i].text = piece(i, at);
    chunks_.erase(chunks_.begin() + (long)ka, chunks_.begin() + (long)kb + 1);
    chunks_.insert(chunks_.begin() + (long)ka, std::make_move_iterator(pieces.begin()),
                   std::make_move_iterator(pieces.end()));
    for (size_t k = first; k < ka + count && k < chunks_.size(); k++) rescan(k);
    rebuild();
}

char16_t SyncedText::at(size_t i) const {
    const size_t k = chunkAt(i);
    return chunks_[k].text[i - before(units_, k)];
}

std::u16string SyncedText::str() const {
    std::u16string out;
    out.reserve(size_);
    for (const Chunk& c : chunks_) out += c.text;
    return out;
}

std::shared_ptr<const std::u16string> SyncedText::snapshot() const {
    if (!snapshot_) snapshot_ = std::make_shared<const std::u16string>(str());
    return snapshot_;
}

bool SyncedText::operator==(const std::u16string& s) const {
    if (s.size() != size_) return false;
    size_t at = 0;
    for (const Chunk& c : chunks_) {
        if (s.compare(at, c.text.size(), c.text) != 0) return false;
        at += c.text.size();
    }
    return true;
}

// ------------------------------------------------------- watched files
namespace {

// The pattern with each {a,b} group spelled out, nested groups too. An
// unclosed brace stands for itself; the list stops growing at 1,024.
void expandBraces(const std::string& p, std::vector<std::string>& out) {
    if (out.size() >= 1024) return;
    size_t open = std::string::npos;
    int depth = 0;
    bool inClass = false;
    for (size_t i = 0; i < p.size(); i++) {
        char c = p[i];
        if (inClass) {
            if (c == ']') inClass = false;
        } else if (c == '[') {
            inClass = p.find(']', i + 1) != std::string::npos;
        } else if (c == '{') {
            if (depth++ == 0) open = i;
        } else if (c == '}' && depth > 0) {
            if (--depth == 0) {
                std::string head = p.substr(0, open), tail = p.substr(i + 1);
                std::string body = p.substr(open + 1, i - open - 1);
                std::vector<std::string> alts;
                int d = 0;
                size_t start = 0;
                for (size_t k = 0; k < body.size(); k++) {
                    if (body[k] == '{') d++;
                    else if (body[k] == '}') d--;
                    else if (body[k] == ',' && d == 0) {
                        alts.push_back(body.substr(start, k - start));
                        start = k + 1;
                    }
                }
                alts.push_back(body.substr(start));
                for (const std::string& a : alts) expandBraces(head + a + tail, out);
                return;
            }
        }
    }
    out.push_back(p);
}

// One character of a [...] set at p[pi] ('[' itself); `end` gets the index
// after the ']'. False when the set is not closed (then '[' is literal).
bool classAt(const std::string& p, size_t pi, char c, bool& matched, size_t& end) {
    size_t i = pi + 1;
    bool negate = i < p.size() && (p[i] == '!' || p[i] == '^');
    if (negate) i++;
    size_t first = i;
    bool hit = false;
    while (i < p.size() && (p[i] != ']' || i == first)) {
        unsigned char lo = (unsigned char)p[i];
        if (i + 2 < p.size() && p[i + 1] == '-' && p[i + 2] != ']') {
            unsigned char hi = (unsigned char)p[i + 2];
            if ((unsigned char)c >= lo && (unsigned char)c <= hi) hit = true;
            i += 3;
        } else {
            if ((unsigned char)c == lo) hit = true;
            i++;
        }
    }
    if (i >= p.size()) return false;
    end = i + 1;
    matched = hit != negate;
    return true;
}

struct GlobMatcher {
    const std::string& p;
    const std::string& s;
    std::vector<signed char> memo;   // -1 unknown, else the answer
    GlobMatcher(const std::string& pat, const std::string& str)
        : p(pat), s(str), memo((pat.size() + 1) * (str.size() + 1), -1) {}

    bool at(size_t pi, size_t si) {
        signed char& m = memo[pi * (s.size() + 1) + si];
        if (m < 0) m = compute(pi, si) ? 1 : 0;
        return m == 1;
    }

    bool compute(size_t pi, size_t si) {
        const size_t P = p.size(), S = s.size();
        if (pi == P) return si == S;
        char c = p[pi];
        if (c == '*') {
            bool globstar = pi + 1 < P && p[pi + 1] == '*' && (pi == 0 || p[pi - 1] == '/') &&
                            (pi + 2 == P || p[pi + 2] == '/');
            if (globstar) {
                if (pi + 2 == P) return true;   // the rest, whatever it is
                // "**/": no segment, or one more and stay here.
                if (at(pi + 3, si)) return true;
                size_t slash = s.find('/', si);
                return slash != std::string::npos && at(pi, slash + 1);
            }
            size_t next = pi + 1;
            while (next < P && p[next] == '*') next++;   // "a**b" is "a*b"
            for (size_t k = si; k <= S; k++) {
                if (at(next, k)) return true;
                if (k < S && s[k] == '/') break;
            }
            return false;
        }
        if (si == S) {
            // "a/**" also matches "a" itself.
            return p.compare(pi, std::string::npos, "/**") == 0;
        }
        if (c == '?') return s[si] != '/' && at(pi + 1, si + 1);
        if (c == '[') {
            bool matched = false;
            size_t end = 0;
            if (classAt(p, pi, s[si], matched, end))
                return s[si] != '/' && matched && at(end, si + 1);
        }
        return s[si] == c && at(pi + 1, si + 1);
    }
};

std::string withoutLeadingSlash(const std::string& s) {
    return !s.empty() && s[0] == '/' ? s.substr(1) : s;
}

// The path below `base`, or "" when it is not below it.
std::string below(const std::string& path, const std::string& base) {
    std::string b = base;
    while (b.size() > 1 && b.back() == '/') b.pop_back();
    if (b == "/") return withoutLeadingSlash(path);
    if (path.size() <= b.size() + 1 || path.compare(0, b.size(), b) != 0 || path[b.size()] != '/')
        return "";
    return path.substr(b.size() + 1);
}

}  // namespace

bool globMatch(const std::string& pattern, const std::string& path) {
    std::vector<std::string> alts;
    expandBraces(pattern, alts);
    for (const std::string& a : alts) {
        GlobMatcher m(a, path);
        if (m.at(0, 0)) return true;
    }
    return false;
}

std::vector<FileWatcher> parseFileWatchers(const Json& registerOptions) {
    std::vector<FileWatcher> out;
    for (const Json& w : registerOptions["watchers"].elements()) {
        FileWatcher fw;
        const Json& g = w["globPattern"];
        if (g.isString()) {
            fw.pattern = g.asString();
        } else if (g.isObject() && g["pattern"].isString()) {
            fw.pattern = g["pattern"].asString();
            // baseUri is a URI or a WorkspaceFolder ({uri, name}).
            const Json& b = g["baseUri"];
            fw.base = pathFromUri(b.isString() ? b.asString() : b["uri"].asString());
            if (fw.base.empty()) continue;
        } else {
            continue;
        }
        if (fw.pattern.empty()) continue;
        if (w["kind"].isNumber()) fw.kind = (int)w["kind"].asInt() & 7;
        out.push_back(fw);
    }
    return out;
}

bool watcherMatches(const FileWatcher& w, const std::string& root, const FileChange& change) {
    int bit = change.type == FileChangeType::Created ? 1
              : change.type == FileChangeType::Changed ? 2 : 4;
    if (!(w.kind & bit)) return false;
    const std::string& path = change.path;
    if (!w.base.empty()) {
        std::string rel = below(path, w.base);
        return !rel.empty() && globMatch(w.pattern, rel);
    }
    if (w.pattern[0] == '/') return globMatch(withoutLeadingSlash(w.pattern), withoutLeadingSlash(path));
    if (w.pattern.find('/') == std::string::npos) {
        size_t slash = path.rfind('/');
        return globMatch(w.pattern, slash == std::string::npos ? path : path.substr(slash + 1));
    }
    if (globMatch(w.pattern, withoutLeadingSlash(path))) return true;
    std::string rel = root.empty() ? "" : below(path, root);
    return !rel.empty() && globMatch(w.pattern, rel);
}

FileChangeType FileEventClassifier::classify(const std::string& path, bool exists,
                                             bool createdOrRenamed) {
    if (!exists) {
        present_.erase(path);
        return FileChangeType::Deleted;
    }
    if (present_.size() >= 200000) present_.clear();
    bool known = !present_.insert(path).second;
    return createdOrRenamed && !known ? FileChangeType::Created : FileChangeType::Changed;
}

std::vector<FileChange> coalesceFileChanges(const std::vector<FileChange>& changes) {
    std::vector<FileChange> out;
    std::map<std::string, size_t> where;   // path -> index in out
    std::vector<bool> dropped;
    for (const FileChange& c : changes) {
        auto it = where.find(c.path);
        if (it == where.end() || dropped[it->second]) {
            where[c.path] = out.size();
            out.push_back(c);
            dropped.push_back(false);
            continue;
        }
        FileChange& was = out[it->second];
        if (was.type == FileChangeType::Created && c.type == FileChangeType::Deleted)
            dropped[it->second] = true;
        else if (was.type == FileChangeType::Deleted && c.type == FileChangeType::Created)
            was.type = FileChangeType::Changed;
        else if (was.type == FileChangeType::Created && c.type == FileChangeType::Changed)
            ;   // still new to the server
        else
            was.type = c.type;
    }
    std::vector<FileChange> kept;
    for (size_t i = 0; i < out.size(); i++)
        if (!dropped[i]) kept.push_back(out[i]);
    return kept;
}

std::vector<FileChange> matchWatched(const WatchTarget& t, const std::vector<FileChange>& changes) {
    std::vector<FileChange> out;
    if (t.watchers.empty()) return out;
    auto wanted = [&](const FileChange& c) {
        for (const FileWatcher& w : t.watchers)
            if (watcherMatches(w, t.root, c)) return true;
        return false;
    };
    for (const FileChange& c : changes)
        if (wanted(c)) out.push_back(c);
    for (const auto& [real, base] : t.aliases) {
        if (real.empty() || real == base) continue;
        for (const FileChange& c : changes)
            if (c.path.size() > real.size() && c.path.compare(0, real.size(), real) == 0 &&
                c.path[real.size()] == '/') {
                FileChange respelled{base + c.path.substr(real.size()), c.type};
                if (wanted(respelled)) out.push_back(std::move(respelled));
            }
    }
    return out;
}

std::string watchedFilesMessage(const std::vector<FileChange>& changes) {
    Json list = Json::array();
    for (const FileChange& c : changes)
        list.push(Json::object({{"uri", uriFromPath(c.path)}, {"type", (int)c.type}}));
    Json msg = Json::object({{"jsonrpc", "2.0"},
                             {"method", "workspace/didChangeWatchedFiles"},
                             {"params", Json::object({{"changes", std::move(list)}})}});
    return Framer::frame(msg.dump());
}

WatchedBatch prepareWatched(const WatchTarget& t, const std::vector<FileChange>& changes) {
    WatchedBatch b;
    b.id = t.id;
    b.generation = t.generation;
    const std::vector<FileChange> wanted = matchWatched(t, changes);
    b.count = wanted.size();
    if (b.count) b.message = watchedFilesMessage(wanted);
    return b;
}

bool watchExcluded(const std::string& path) {
    auto has = [&](const std::string& part) {
        // "/part/" anywhere, or the path ending in "/part" for a folder.
        size_t at = path.find("/" + part + "/");
        if (at != std::string::npos) return true;
        return path.size() >= part.size() + 1 &&
               path.compare(path.size() - part.size() - 1, std::string::npos, "/" + part) == 0;
    };
    if (has(".git") || has(".hg/store")) return true;
    // node_modules/<package>/anything: the package folders themselves are
    // reported (an install adding one), not what is inside them.
    for (size_t at = path.find("/node_modules/"); at != std::string::npos;
         at = path.find("/node_modules/", at + 1)) {
        size_t pkg = at + 14;
        size_t slash = path.find('/', pkg);
        if (slash != std::string::npos && slash + 1 < path.size()) return true;
    }
    return false;
}

}  // namespace Lsp
