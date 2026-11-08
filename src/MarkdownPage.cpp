// MarkdownPage.cpp — see MarkdownPage.h.
#include "MarkdownPage.h"

#include <string>

namespace MarkdownPage {

namespace {

// Whether a piece may start at run i.
bool startsPiece(const std::vector<MdRun>& runs, size_t i) {
    if (i == 0) return true;
    const MdRun& prev = runs[i - 1];
    const MdRun& r = runs[i];
    // The space between two blocks goes with the block after it.
    if (prev.text.empty() || prev.text.back() != '\n' || prev.hardBreak || prev.gap) return false;
    if (r.quote || r.codeBlock || prev.codeBlock) return false;
    if (r.tableId != 0 && r.tableId == prev.tableId) return false;
    return true;
}

// Two unrelated 64-bit hashes over the same bytes (FNV-1a, and a rotate,
// xor and multiply by the golden ratio's constant), so two pieces are taken
// for the same only when 128 bits agree.
struct Hash {
    uint64_t h = 1469598103934665603ull, g = 0x243F6A8885A308D3ull;
    void bytes(const void* p, size_t n) {
        const unsigned char* c = static_cast<const unsigned char*>(p);
        for (size_t k = 0; k < n; k++) {
            h ^= c[k];
            h *= 1099511628211ull;
            g = ((g << 5) | (g >> 59)) ^ c[k];
            g *= 0x9E3779B97F4A7C15ull;
        }
    }
    void str(const std::string& s) {
        const uint64_t n = s.size();
        bytes(&n, sizeof n);
        bytes(s.data(), s.size());
    }
    void num(long v) { bytes(&v, sizeof v); }
};

long relLine(const MdRun& r, int base) { return r.line < 0 ? -1 : (long)r.line - base; }
long relTable(const MdRun& r, int first) { return r.tableId == 0 ? 0 : (long)r.tableId - first + 1; }

void keyOf(const std::vector<MdRun>& runs, Piece& p) {
    Hash h;
    for (size_t k = p.begin; k < p.end; k++) {
        const MdRun& r = runs[k];
        h.str(r.text);
        h.str(r.lang);
        h.str(r.url);
        h.str(r.src);
        h.str(r.anchor);
        const long nums[] = {r.heading, r.bold, r.italic, r.strike, r.code, r.codeBlock, r.quote,
                             r.quoteDepth, r.quoteIndent, r.rule, r.table, relTable(r, p.firstTable),
                             r.tableRow, r.tableCol, r.tableCols, r.tableAlign, r.listDepth,
                             r.ordered, r.marker, r.task, r.link, relLine(r, p.base), r.image,
                             r.gap, r.hardBreak, r.math, r.footnote, r.footnotes};
        for (long v : nums) h.num(v);
    }
    p.key = h.h;
    p.check = h.g ^ (uint64_t)(p.end - p.begin);
}

bool sameRun(const MdRun& a, int baseA, int tableA, const MdRun& b, int baseB, int tableB) {
    return a.text == b.text && a.heading == b.heading && a.bold == b.bold && a.italic == b.italic &&
           a.strike == b.strike && a.code == b.code && a.codeBlock == b.codeBlock &&
           a.lang == b.lang && a.quote == b.quote && a.quoteDepth == b.quoteDepth &&
           a.quoteIndent == b.quoteIndent && a.rule == b.rule && a.table == b.table &&
           relTable(a, tableA) == relTable(b, tableB) && a.tableRow == b.tableRow &&
           a.tableCol == b.tableCol && a.tableCols == b.tableCols && a.tableAlign == b.tableAlign &&
           a.listDepth == b.listDepth && a.ordered == b.ordered && a.marker == b.marker &&
           a.task == b.task && a.link == b.link && a.url == b.url &&
           relLine(a, baseA) == relLine(b, baseB) && a.image == b.image && a.src == b.src &&
           a.gap == b.gap && a.hardBreak == b.hardBreak && a.math == b.math &&
           a.footnote == b.footnote && a.footnotes == b.footnotes && a.anchor == b.anchor;
}

}  // namespace

std::vector<Piece> split(const std::vector<MdRun>& runs) {
    std::vector<Piece> out;
    for (size_t i = 0; i < runs.size(); i++) {
        if (startsPiece(runs, i)) {
            if (!out.empty()) out.back().end = i;
            Piece p;
            p.begin = i;
            out.push_back(p);
        }
    }
    if (!out.empty()) out.back().end = runs.size();
    for (Piece& p : out) {
        bool line = false, table = false;
        for (size_t k = p.begin; k < p.end && !(line && table); k++) {
            if (!line && runs[k].line >= 0) {
                p.base = runs[k].line;
                line = true;
            }
            if (!table && runs[k].tableId != 0) {
                p.firstTable = runs[k].tableId;
                table = true;
            }
        }
        keyOf(runs, p);
    }
    return out;
}

bool same(const std::vector<MdRun>& a, const Piece& pa, const std::vector<MdRun>& b,
          const Piece& pb) {
    if (!alike(pa, pb)) return false;
    for (size_t k = 0; k < pa.end - pa.begin; k++)
        if (!sameRun(a[pa.begin + k], pa.base, pa.firstTable, b[pb.begin + k], pb.base,
                     pb.firstTable))
            return false;
    return true;
}

bool alike(const Piece& a, const Piece& b) {
    return a.key == b.key && a.check == b.check && a.end - a.begin == b.end - b.begin;
}

std::vector<long> match(const std::vector<Piece>& oldPieces, const std::vector<bool>& keep,
                        const std::vector<Piece>& newPieces) {
    std::vector<long> out(newPieces.size(), -1);
    const size_t n = oldPieces.size(), m = newPieces.size();
    size_t top = 0;
    while (top < n && top < m && alike(oldPieces[top], newPieces[top])) top++;
    size_t bottom = 0;
    while (bottom < n - top && bottom < m - top &&
           alike(oldPieces[n - 1 - bottom], newPieces[m - 1 - bottom]))
        bottom++;
    auto pair = [&](size_t newIndex, size_t oldIndex) {
        if (oldIndex < keep.size() && !keep[oldIndex]) return;
        out[newIndex] = (long)oldIndex;
    };
    for (size_t k = 0; k < top; k++) pair(k, k);
    for (size_t k = 0; k < bottom; k++) pair(m - 1 - k, n - 1 - k);
    return out;
}

}  // namespace MarkdownPage
