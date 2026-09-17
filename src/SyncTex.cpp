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
