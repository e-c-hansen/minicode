// TerminalLayout.cpp — see TerminalLayout.h.
#include "TerminalLayout.h"

#include <algorithm>
#include <cmath>

namespace TerminalLayout {

namespace {

// What the parts share once the gaps are taken out.
double available(size_t n, double total, double gap) {
    return n == 0 ? 0 : std::max(0.0, total - gap * (double)(n - 1));
}

// A share in whole points. The small allowance keeps a share computed as
// k / avail (a drag's or a nudge's) from rounding down to k - 1.
double whole(double x) { return std::floor(x + 1e-6); }

bool fits(const Fractions &fr, double total, double gap, double min) {
    for (double len : lengths(fr, total, gap))
        if (len + 1e-6 < min) return false;
    return true;
}

// Parts j and j+1 share their lengths anew, part j taking `first` points,
// each kept at least `min` (rounded up to whole points).
bool setFirstOfPair(Fractions &fr, size_t j, double first, double total, double gap, double min) {
    if (j + 1 >= fr.size()) return false;
    const double avail = available(fr.size(), total, gap);
    if (avail <= 0) return false;
    std::vector<double> len = lengths(fr, total, gap);
    const double pair = len[j] + len[j + 1];
    const double lo = std::ceil(min - 1e-6);
    if (pair < 2 * lo) return false;
    first = std::min(std::max(std::floor(first + 0.5), lo), pair - lo);
    if (first == len[j]) return false;
    len[j] = first;
    len[j + 1] = pair - first;
    // Every share from the lengths as they now are, so the parts the bar
    // does not touch keep their exact points (shares rounded down lose up
    // to a point each, which the pair would otherwise take from the last).
    for (size_t k = 0; k < fr.size(); k++) fr[k] = len[k] / avail;
    return true;
}

}  // namespace

std::vector<double> lengths(const Fractions &fr, double total, double gap) {
    const size_t n = fr.size();
    std::vector<double> out(n, 0);
    if (n == 0) return out;
    const double avail = available(n, total, gap);
    double used = 0;
    for (size_t i = 0; i + 1 < n; i++) {
        out[i] = std::max(0.0, whole(fr[i] * avail));
        used += out[i];
    }
    out[n - 1] = std::max(0.0, avail - used);
    return out;
}

double startOf(const Fractions &fr, size_t i, double total, double gap) {
    const std::vector<double> len = lengths(fr, total, gap);
    double x = 0;
    for (size_t k = 0; k < i && k < len.size(); k++) x += len[k] + gap;
    return x;
}

double boundaryAt(const Fractions &fr, size_t i, double total, double gap) {
    const std::vector<double> len = lengths(fr, total, gap);
    if (i >= len.size()) return total;
    return startOf(fr, i, total, gap) + len[i] + gap / 2;
}

Fractions even(size_t n) { return Fractions(n, n ? 1.0 / (double)n : 0); }

Split split(Fractions &fr, size_t i, double total, double gap, double min) {
    if (fr.empty()) {
        fr = {1.0};
        return Split::Halved;
    }
    if (i >= fr.size()) i = fr.size() - 1;
    Fractions halved = fr;
    halved[i] /= 2;
    halved.insert(halved.begin() + (long)i + 1, halved[i]);
    // Not laid out yet: nothing to judge by.
    if (total <= 0 || fits(halved, total, gap, min)) {
        fr = halved;
        return Split::Halved;
    }
    Fractions evened = even(fr.size() + 1);
    if (fits(evened, total, gap, min)) {
        fr = evened;
        return Split::Evened;
    }
    return Split::Refused;
}

void remove(Fractions &fr, size_t i) {
    if (i >= fr.size()) return;
    const double share = fr[i];
    fr.erase(fr.begin() + (long)i);
    if (fr.empty()) return;
    fr[i > 0 ? i - 1 : 0] += share;
}

bool dragBoundary(Fractions &fr, size_t i, double pos, double total, double gap, double min) {
    if (i + 1 >= fr.size()) return false;
    return setFirstOfPair(fr, i, pos - gap / 2 - startOf(fr, i, total, gap), total, gap, min);
}

bool nudge(Fractions &fr, size_t i, bool towardEnd, double amount, double total, double gap,
           double min) {
    if (fr.size() < 2 || i >= fr.size()) return false;
    // The boundary after part j moves: part i's own far edge, or for the
    // last part its near edge.
    const size_t j = i + 1 < fr.size() ? i : i - 1;
    const std::vector<double> len = lengths(fr, total, gap);
    return setFirstOfPair(fr, j, len[j] + (towardEnd ? amount : -amount), total, gap, min);
}

std::vector<std::vector<Rect>> frames(const Layout &l, double width, double height, double gap) {
    std::vector<std::vector<Rect>> out;
    const std::vector<double> colW = lengths(l.columns, width, gap);
    double x = 0;
    for (size_t c = 0; c < l.columns.size(); c++) {
        std::vector<Rect> column;
        const Fractions rows = c < l.rows.size() ? l.rows[c] : Fractions{1.0};
        const std::vector<double> rowH = lengths(rows, height, gap);
        double y = 0;
        for (size_t r = 0; r < rows.size(); r++) {
            column.push_back(Rect{x, y, colW[c], rowH[r]});
            y += rowH[r] + gap;
        }
        out.push_back(column);
        x += colW[c] + gap;
    }
    return out;
}

void removePane(Layout &l, size_t col, size_t row) {
    if (col >= l.columns.size() || col >= l.rows.size()) return;
    remove(l.rows[col], row);
    if (!l.rows[col].empty()) return;
    l.rows.erase(l.rows.begin() + (long)col);
    remove(l.columns, col);
}

}  // namespace TerminalLayout
