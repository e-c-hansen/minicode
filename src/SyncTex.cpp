// SyncTex.cpp — see SyncTex.h.
//
// The format is line based. A preamble names the input files and the unit,
// then the content section lists boxes as
//     <type><tag>,<line>[,<column>]:<x>,<y>[:<width>[,<height>,<depth>]]
// in scaled points, with y giving the baseline and height/depth reaching above
// and below it. Pages are bracketed by "{<n>" and "}<n>".
#include "SyncTex.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <initializer_list>

namespace {

const double kSpPerPoint = 65536.0;

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Record types that carry a tag and line.
bool isRecordType(char c) {
    return c == '(' || c == '[' || c == 'h' || c == 'v' || c == 'x' ||
           c == 'k' || c == 'g' || c == 'r' || c == '$';
}

// Read a (possibly negative) integer at `i`, advancing past it.
bool readInt(const std::string &s, std::size_t &i, long &out) {
    std::size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) i++;
    while (i < s.size() && isDigit(s[i])) i++;
    if (i == start) return false;
    out = std::strtol(s.substr(start, i - start).c_str(), nullptr, 10);
    return true;
}

double valueAfter(const std::string &line, const char *key) {
    return std::atof(line.c_str() + std::string(key).size());
}

// `path` against `base` when relative, with "." and ".." steps taken and
// doubled slashes dropped.
std::string absolutePath(const std::string &path, const std::string &base) {
    const std::string whole = (!path.empty() && path[0] == '/') ? path : base + "/" + path;
    std::vector<std::string> parts;
    std::size_t at = 0;
    while (at <= whole.size()) {
        std::size_t slash = whole.find('/', at);
        if (slash == std::string::npos) slash = whole.size();
        const std::string part = whole.substr(at, slash - at);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        at = slash + 1;
    }
    std::string out;
    for (const std::string &p : parts) out += "/" + p;
    return out.empty() ? "/" : out;
}

std::string fileName(const std::string &path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

SyncTexIndex SyncTexIndex::parse(const std::string &text) {
    SyncTexIndex idx;
    int page = 0;
    bool inContent = false;
    std::size_t pos = 0;

    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, (nl == std::string::npos)
                                                ? std::string::npos
                                                : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (line.compare(0, 6, "Input:") == 0) {
            std::size_t i = 6;
            long tag = 0;
            if (readInt(line, i, tag) && i < line.size() && line[i] == ':') {
                std::string path = line.substr(i + 1);
                if (!path.empty()) idx.inputs_[(int)tag] = path;
            }
            continue;
        }
        if (!inContent) {
            if (line == "Content:") { inContent = true; continue; }
            if (line.compare(0, 5, "Unit:") == 0)
                idx.unit_ = valueAfter(line, "Unit:");
            else if (line.compare(0, 9, "X Offset:") == 0)
                idx.xoff_ = valueAfter(line, "X Offset:");
            else if (line.compare(0, 9, "Y Offset:") == 0)
                idx.yoff_ = valueAfter(line, "Y Offset:");
            continue;
        }

        char type = line[0];
        if (type == '{') { page = std::atoi(line.c_str() + 1); continue; }
        if (type == '}') { page = 0; continue; }
        if (!isRecordType(type) || line.size() < 2) continue;

        std::size_t i = 1;
        long tag = 0, srcLine = 0;
        if (!readInt(line, i, tag)) continue;
        if (i >= line.size() || line[i] != ',') continue;
        i++;
        if (!readInt(line, i, srcLine)) continue;
        if (i < line.size() && line[i] == ',') {   // column, unused
            i++;
            long col = 0;
            readInt(line, i, col);
        }
        if (i >= line.size() || line[i] != ':') continue;
        i++;
        long x = 0, y = 0;
        if (!readInt(line, i, x)) continue;
        if (i >= line.size() || line[i] != ',') continue;
        i++;
        if (!readInt(line, i, y)) continue;

        long w = 0, h = 0, d = 0;
        if (i < line.size() && line[i] == ':') {
            i++;
            readInt(line, i, w);
            if (i < line.size() && line[i] == ',') {
                i++;
                readInt(line, i, h);
                if (i < line.size() && line[i] == ',') { i++; readInt(line, i, d); }
            }
        }

        Record r;
        r.type = type;
        r.page = page;
        r.tag = (int)tag;
        r.line = (int)srcLine;
        r.x = ((double)x * idx.unit_ + idx.xoff_) / kSpPerPoint;
        r.y = ((double)y * idx.unit_ + idx.yoff_) / kSpPerPoint;
        r.w = (double)w * idx.unit_ / kSpPerPoint;
        r.h = (double)h * idx.unit_ / kSpPerPoint;
        r.d = (double)d * idx.unit_ / kSpPerPoint;
        if (r.page > 0 && r.line > 0) idx.records_.push_back(r);
    }
    return idx;
}

std::string SyncTexIndex::pathForTag(int tag) const {
    auto it = inputs_.find(tag);
    return (it == inputs_.end()) ? std::string() : it->second;
}

int SyncTexIndex::tagForPath(const std::string &path) const {
    std::string name = path;
    std::size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    for (const auto &kv : inputs_) {
        if (kv.second == path) return kv.first;
        std::string other = kv.second;
        std::size_t s2 = other.find_last_of('/');
        if (s2 != std::string::npos) other = other.substr(s2 + 1);
        if (!name.empty() && other == name) return kv.first;
    }
    return 0;
}

int SyncTexIndex::tagForFile(const std::string &path, const std::string &baseDir,
                             const std::function<std::string(const std::string &)> &canonical)
    const {
    if (path.empty()) return 0;
    auto same = [&](std::string p) {
        p = absolutePath(p, baseDir);
        return canonical ? canonical(p) : p;
    };
    const std::string want = same(path);
    const std::string name = fileName(path);
    int byName = 0, named = 0;
    for (const auto &kv : inputs_) {
        if (same(kv.second) == want) return kv.first;
        if (!name.empty() && fileName(kv.second) == name) {
            byName = kv.first;
            named++;
        }
    }
    return named == 1 ? byName : 0;
}

std::vector<SyncTexHit> SyncTexIndex::hitsAtPoint(int page, double x, double y,
                                                  std::size_t maxHits) const {
    std::vector<SyncTexHit> hits;
    for (const Record &r : records_) {
        if (r.page != page) continue;
        double left = r.x, right = r.x + r.w;
        double top = r.y - r.h, bottom = r.y + r.d;
        if (right < left) std::swap(left, right);
        if (bottom < top) std::swap(top, bottom);
        double dx = (x < left) ? left - x : (x > right ? x - right : 0.0);
        double dy = (y < top) ? top - y : (y > bottom ? y - bottom : 0.0);

        SyncTexHit h;
        h.tag = r.tag;
        h.line = r.line;
        h.distance = std::sqrt(dx * dx + dy * dy);
        h.x = left;
        h.y = top;
        h.width = right - left;
        h.height = bottom - top;
        h.area = h.width * h.height;
        hits.push_back(h);
    }

    // Boxes containing the point come first, most specific (smallest) first;
    // then the nearest boxes. A page has many overlapping boxes, so keep only
    // the best hit per source line.
    std::stable_sort(hits.begin(), hits.end(),
                     [](const SyncTexHit &a, const SyncTexHit &b) {
                         bool ain = a.distance <= 0.0, bin = b.distance <= 0.0;
                         if (ain != bin) return ain;
                         if (ain) return a.area < b.area;
                         return a.distance < b.distance;
                     });

    std::vector<SyncTexHit> out;
    for (const SyncTexHit &h : hits) {
        bool seen = false;
        for (const SyncTexHit &k : out)
            if (k.tag == h.tag && k.line == h.line) { seen = true; break; }
        if (seen) continue;
        out.push_back(h);
        if (out.size() >= maxHits) break;
    }
    return out;
}

std::vector<SyncTexHit> SyncTexIndex::textHitsAtPoint(int page, double x,
                                                      double y,
                                                      std::size_t maxHits) const {
    // The innermost horizontal box around the point: a line of text.
    const Record *line = nullptr;
    for (const Record &r : records_) {
        if (r.page != page || r.type != '(') continue;
        if (x < r.x || x > r.x + r.w || y < r.y - r.h || y > r.y + r.d) continue;
        if (!line || r.w * (r.h + r.d) < line->w * (line->h + line->d)) line = &r;
    }

    std::vector<SyncTexHit> out;
    auto add = [&out](const SyncTexHit &h) {
        for (const SyncTexHit &k : out)
            if (k.tag == h.tag && k.line == h.line) return;
        out.push_back(h);
    };
    if (line) {
        // Glue, kerns and math on that line's baseline: the nearest at or
        // right of the point, and the nearest left of it.
        const Record *right = nullptr, *left = nullptr;
        for (const Record &r : records_) {
            if (r.page != page) continue;
            if (r.type != 'g' && r.type != 'k' && r.type != '$' && r.type != 'x')
                continue;
            if (std::fabs(r.y - line->y) > 0.5) continue;
            if (r.x < line->x - 0.5 || r.x > line->x + line->w + 0.5) continue;
            if (r.x >= x) {
                if (!right || r.x < right->x) right = &r;
            } else {
                if (!left || r.x > left->x) left = &r;
            }
        }
        // Glue on the box's end edge closes the box (a table cell, a line)
        // rather than following the word, and may come from a later line.
        const Record *first = right, *second = left;
        if (right && left && std::fabs(right->x - (line->x + line->w)) < 0.5)
            std::swap(first, second);
        for (const Record *r : {first, second}) {
            if (!r) continue;
            SyncTexHit h;
            h.tag = r->tag;
            h.line = r->line;
            h.distance = std::fabs(r->x - x);
            h.x = line->x;                     // the line's box, for anchoring
            h.y = line->y - line->h;
            h.width = line->w;
            h.height = line->h + line->d;
            h.area = h.width * h.height;
            add(h);
        }
    }
    for (const SyncTexHit &h : hitsAtPoint(page, x, y, maxHits)) {
        if (out.size() >= maxHits) break;
        add(h);
    }
    return out;
}

std::vector<SyncTexBox> SyncTexIndex::boxesForLine(int tag, int line,
                                                   int *foundLine) const {
    if (foundLine) *foundLine = 0;
    auto matches = [tag](const Record &r, int l) {
        return r.line == l && (tag == 0 || r.tag == tag);
    };
    auto putSomething = [&](int l) {
        for (const Record &r : records_)
            if (matches(r, l)) return true;
        return false;
    };
    // The line itself, else the nearest that put something on a page: a
    // line below before one above at the same distance, since what follows
    // a blank line or a comment is what the caret is about to reach.
    const int kReach = 40;
    int chosen = 0;
    for (int d = 0; d <= kReach && !chosen; d++) {
        if (line + d > 0 && putSomething(line + d)) chosen = line + d;
        else if (d > 0 && line - d > 0 && putSomething(line - d)) chosen = line - d;
    }
    if (!chosen) return {};
    if (foundLine) *foundLine = chosen;

    // The first page it reached with something other than a vertical box
    // (a page's own boxes carry the line that ended the page: a \newpage's).
    auto vertical = [](const Record &r) { return r.type == '[' || r.type == 'v'; };
    int page = 0, anyPage = 0;
    for (const Record &r : records_) {
        if (!matches(r, chosen)) continue;
        if (!anyPage || r.page < anyPage) anyPage = r.page;
        if (!vertical(r) && (!page || r.page < page)) page = r.page;
    }
    if (!page) page = anyPage;

    auto sized = [](const Record &r) { return r.w > 0 && r.h + r.d > 0; };
    // The innermost line of text (a sized horizontal box) around a point.
    auto lineAround = [&](const Record &at) -> const Record * {
        const Record *best = nullptr;
        for (const Record &r : records_) {
            if (r.page != page || r.type != '(' || &r == &at || !sized(r)) continue;
            if (at.x < r.x - 0.5 || at.x > r.x + r.w + 0.5) continue;
            if (at.y < r.y - r.h - 0.5 || at.y > r.y + r.d + 0.5) continue;
            if (!best || r.w * (r.h + r.d) < best->w * (best->h + best->d)) best = &r;
        }
        return best;
    };
    std::vector<SyncTexBox> out;
    auto push = [&out, page](const Record &r) {
        SyncTexBox b;
        b.page = page;
        b.x = r.x;
        b.y = r.y - r.h;
        b.width = r.w;
        b.height = r.h + r.d;
        for (const SyncTexBox &k : out)
            if (std::fabs(k.x - b.x) < 0.01 && std::fabs(k.y - b.y) < 0.01 &&
                std::fabs(k.width - b.width) < 0.01 && std::fabs(k.height - b.height) < 0.01)
                return;
        out.push_back(b);
    };
    // The glue, kerns, math and void boxes the line set, each in its line
    // of text. A line of text's own record is left out: it carries the
    // line its paragraph ended on, so it would light up the paragraph's
    // every line for its last.
    for (const Record &r : records_) {
        if (r.page != page || !matches(r, chosen) || r.type == '(' || vertical(r)) continue;
        if (const Record *around = lineAround(r)) push(*around);
        else if (sized(r)) push(r);
    }
    // Nothing set inside a line of text (a display, a picture): the boxes
    // themselves, then the smallest vertical one rather than a whole page.
    if (out.empty())
        for (const Record &r : records_)
            if (r.page == page && r.type == '(' && matches(r, chosen) && sized(r)) push(r);
    if (out.empty()) {
        const Record *small = nullptr;
        for (const Record &r : records_)
            if (r.page == page && vertical(r) && matches(r, chosen) && sized(r) &&
                (!small || r.w * (r.h + r.d) < small->w * (small->h + small->d)))
                small = &r;
        if (small) push(*small);
    }
    std::stable_sort(out.begin(), out.end(), [](const SyncTexBox &a, const SyncTexBox &b) {
        if (std::fabs(a.y - b.y) > 0.01) return a.y < b.y;
        return a.x < b.x;
    });
    return out;
}
