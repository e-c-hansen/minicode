// PageMeasure.h — the arithmetic of two pages whose width follows their
// pane: a rendered Markdown page held to a readable line length, centered
// (markdown.max-width), and the shortcut hints (Shift+Cmd+H), laid out in
// as many columns of tab stops as the room holds, their heights balanced.
// Header only, no dependencies; the app measures the strings and draws, the
// core decides.
// Tested in tests/run_tests.cpp (measure:*).
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace PageMeasure {

// The horizontal inset of a text view `viewWidth` points wide whose text
// may be at most `maxText` points wide (0: no limit): at least `minInset`,
// more to center a page narrower than the view. `padding` is the text
// container's line fragment padding, on both sides of the text. Whole
// points, rounded up, so the text is never wider than `maxText`.
inline double centeredInset(double viewWidth, double minInset, double maxText, double padding) {
    if (!(maxText > 0)) return minInset;
    const double inset = std::ceil((viewWidth - maxText - 2 * padding) / 2);
    return std::max(minInset, inset);
}

// markdown.max-width at a zoom: points of text, 0 for no limit.
inline double maxTextWidth(int setting, double zoomScale) {
    return setting > 0 ? setting * std::max(zoomScale, 0.0) : 0;
}

// One item of the hints as measured, in the order they are read: a
// shortcut (its key's width, its label's, and a muted state's, "open"), a
// section's heading (its width in `key`), or a gap. A heading or a gap is
// kept with the item after it: a column never ends on one. `height` is the
// item's own, `spaceBefore` lies above it except at the top of a column.
struct HintItem {
    enum Kind { Shortcut, Heading, Gap };
    Kind kind = Shortcut;
    double key = 0, label = 0;
    bool hasState = false;
    double state = 0;
    double height = 0, spaceBefore = 0;
};

// One column: items [first, end), its left edge `x` from the text's left,
// its stops measured from `x` (a key at 0, its label at `label`, a state
// at `state`), and its width and height.
struct HintsColumn {
    size_t first = 0, end = 0;
    double x = 0, label = 0, state = 0;
    double width = 0, height = 0;
};

// The columns, as many as fit the room, their heights balanced. `textWidth`
// and `textHeight` are the whole text's. When even one column is wider than
// the room, `wraps` says labels wrap under themselves and states follow
// their labels; the one column is then as wide as the room.
struct HintsLayout {
    std::vector<HintsColumn> columns;
    double textWidth = 0, textHeight = 0;
    bool wraps = false;
    int count() const { return (int)columns.size(); }
};

namespace detail {

// The items cut into units that never part (a heading or a gap with what
// follows it); `starts` holds each unit's first item, and one past the end.
inline std::vector<size_t> hintUnits(const std::vector<HintItem>& items) {
    std::vector<size_t> starts;
    for (size_t i = 0; i < items.size(); ++i) {
        const bool glued = i > 0 && items[i - 1].kind != HintItem::Shortcut;
        if (!glued) starts.push_back(i);
    }
    starts.push_back(items.size());
    return starts;
}

// The stops and size of a column of items [first, end).
inline HintsColumn hintColumn(const std::vector<HintItem>& items, size_t first, size_t end,
                              double gap) {
    HintsColumn c;
    c.first = first;
    c.end = end;
    double key = 0, label = 0, labelWithState = 0, state = 0, heading = 0;
    for (size_t i = first; i < end; ++i) {
        const HintItem& it = items[i];
        c.height += it.height + (i == first ? 0 : it.spaceBefore);
        if (it.kind == HintItem::Heading) heading = std::max(heading, it.key);
        if (it.kind != HintItem::Shortcut) continue;
        key = std::max(key, it.key);
        label = std::max(label, it.label);
        if (it.hasState) {
            labelWithState = std::max(labelWithState, it.label);
            state = std::max(state, it.state);
        }
    }
    c.label = std::ceil(key + gap);
    c.state = std::ceil(c.label + labelWithState + gap);
    c.width = std::ceil(std::max({heading, c.label + label, state > 0 ? c.state + state : 0.0}));
    return c;
}

// Units [0, n) cut into `columns` columns, none empty, the tallest as short
// as can be and, among the cuts that keep to that, the heights as even as
// can be (the least sum of their squares). Returns each column's first
// unit, and n; empty when there are fewer units than columns.
inline std::vector<size_t> balance(const std::vector<double>& top, const std::vector<double>& full,
                                   int columns) {
    const size_t n = top.size();
    if (columns < 1 || n < (size_t)columns) return {};
    std::vector<double> sum(n + 1, 0);   // sum of full heights before a unit
    for (size_t i = 0; i < n; ++i) sum[i + 1] = sum[i] + full[i];
    auto height = [&](size_t a, size_t b) { return top[a] + sum[b] - sum[a + 1]; };   // [a, b)
    const double inf = std::numeric_limits<double>::infinity();
    const size_t K = (size_t)columns;
    // The least tallest column: tallest[k][b], units [0, b) in k columns.
    std::vector<std::vector<double>> tallest(K + 1, std::vector<double>(n + 1, inf));
    tallest[0][0] = 0;
    for (size_t k = 1; k <= K; ++k)
        for (size_t b = k; b <= n; ++b)
            for (size_t a = b; a-- > k - 1;) {
                const double h = height(a, b);
                if (h >= tallest[k][b]) break;   // taller still further back
                tallest[k][b] = std::min(tallest[k][b], std::max(tallest[k - 1][a], h));
            }
    const double most = tallest[K][n] + 1e-9;
    // The evenest cut with no column above `most`.
    std::vector<std::vector<double>> squares(K + 1, std::vector<double>(n + 1, inf));
    std::vector<std::vector<size_t>> from(K + 1, std::vector<size_t>(n + 1, 0));
    squares[0][0] = 0;
    for (size_t k = 1; k <= K; ++k)
        for (size_t b = k; b <= n; ++b)
            for (size_t a = b; a-- > k - 1;) {
                const double h = height(a, b);
                if (h > most) break;
                if (squares[k - 1][a] == inf) continue;
                const double v = squares[k - 1][a] + h * h;
                if (v < squares[k][b] - 1e-9) {
                    squares[k][b] = v;
                    from[k][b] = a;
                }
            }
    std::vector<size_t> cuts(K + 1, n);
    for (size_t k = K, b = n; k > 0; --k) {
        b = from[k][b];
        cuts[k - 1] = b;
    }
    return cuts;
}

}  // namespace detail

// Lay the items out in as many columns as fit `room` points of width, at
// most `maxColumns`, each column's stops from its own widest key and label
// (`gap` between a key and its label and before a state), `columnGap`
// between columns. The columns' heights are balanced, a heading never ends
// a column, and a shortcut is never split. With no room for one column the
// text is one column that wraps. The caller scrolls when `textHeight` is
// more than its room.
inline HintsLayout layoutHints(const std::vector<HintItem>& items, double room, double gap,
                               double columnGap, int maxColumns = 16) {
    HintsLayout best;
    const std::vector<size_t> starts = detail::hintUnits(items);
    const size_t units = starts.size() - 1;
    std::vector<double> top(units), full(units);
    double narrowest = std::numeric_limits<double>::infinity();
    for (size_t u = 0; u < units; ++u) {
        top[u] = full[u] = 0;
        double widest = 0;
        for (size_t i = starts[u]; i < starts[u + 1]; ++i) {
            full[u] += items[i].height + items[i].spaceBefore;
            top[u] += items[i].height + (i == starts[u] ? 0 : items[i].spaceBefore);
            widest = std::max(widest, detail::hintColumn(items, i, i + 1, gap).width);
        }
        narrowest = std::min(narrowest, widest);
    }
    auto layoutOf = [&](int n) {
        HintsLayout l;
        const std::vector<size_t> cuts = detail::balance(top, full, n);
        double x = 0;
        for (int c = 0; c < n; ++c) {
            HintsColumn col = detail::hintColumn(items, starts[cuts[c]], starts[cuts[c + 1]], gap);
            col.x = x;
            x = std::ceil(x + col.width + columnGap);
            l.textHeight = std::max(l.textHeight, col.height);
            l.textWidth = col.x + col.width;
            l.columns.push_back(col);
        }
        return l;
    };
    if (units == 0) {
        best.columns.push_back(HintsColumn());
        return best;
    }
    best = layoutOf(1);
    if (best.textWidth > room) {
        // Labels that do not fit wrap under themselves, and a state goes
        // after its own label (the drawing's choice), not at a stop.
        best.wraps = true;
        best.textWidth = std::max(room, best.columns[0].label + 1);
        best.columns[0].width = best.textWidth;
        return best;
    }
    for (int n = 2; n <= maxColumns && (size_t)n <= units; ++n) {
        // No n columns can fit when each is at least as wide as the
        // narrowest unit.
        if (n * narrowest + (n - 1) * columnGap > room) break;
        HintsLayout l = layoutOf(n);
        if (l.textWidth <= room) best = l;
    }
    return best;
}

}  // namespace PageMeasure
