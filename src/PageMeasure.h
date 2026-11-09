// PageMeasure.h — the arithmetic of two pages whose width follows their
// pane: a rendered Markdown page held to a readable line length, centered
// (markdown.max-width), and the shortcut hints (Shift+Cmd+H), laid out in
// one or two columns of tab stops by the room there is. Header only, no
// dependencies; the Mac measures the strings and draws, the core decides.
// Tested in tests/run_tests.cpp (measure:*).
#pragma once

#include <algorithm>
#include <cmath>
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

// One row of the hints as measured: a heading (its width in `key1`), or a
// shortcut with an optional second one (`key2`, `label2`) or a muted state
// ("open", "hidden") in the second's place.
struct HintRow {
    bool heading = false;
    double key1 = 0, label1 = 0;
    bool hasKey2 = false;
    double key2 = 0, label2 = 0;
    bool hasState = false;
    double state = 0;
};

// Where the tab stops go, measured from the text's left edge. Two
// columns: key1 | label1 | key2 | label2, a state at key2's stop. One
// column: every shortcut on a row of its own, key | label, a state at
// `state`. `textWidth` is the widest row. When even one column is wider
// than the room, `wraps` says labels wrap under themselves and states
// follow their labels.
struct HintsLayout {
    int columns = 2;
    double label1 = 0, key2 = 0, label2 = 0, state = 0;
    double textWidth = 0;
    bool wraps = false;
};

// `room` is the width the text may take; `gap` lies between a key and its
// label, `columnGap` between the two columns.
inline HintsLayout layoutHints(const std::vector<HintRow>& rows, double room, double gap,
                               double columnGap) {
    double key1 = 0, label1Paired = 0, key2 = 0, label2 = 0, state = 0;
    double label1Alone = 0, heading = 0, keyAny = 0;
    for (const HintRow& r : rows) {
        if (r.heading) { heading = std::max(heading, r.key1); continue; }
        key1 = std::max(key1, r.key1);
        keyAny = std::max(keyAny, r.key1);
        if (r.hasKey2) {
            label1Paired = std::max(label1Paired, r.label1);
            key2 = std::max(key2, r.key2);
            label2 = std::max(label2, r.label2);
            keyAny = std::max(keyAny, r.key2);
        } else if (r.hasState) {
            label1Paired = std::max(label1Paired, r.label1);
            state = std::max(state, r.state);
        } else {
            label1Alone = std::max(label1Alone, r.label1);
        }
    }
    HintsLayout two;
    two.columns = 2;
    two.label1 = std::ceil(key1 + gap);
    two.key2 = std::ceil(two.label1 + label1Paired + columnGap);
    two.label2 = std::ceil(two.key2 + key2 + gap);
    two.state = two.key2;
    two.textWidth = std::ceil(std::max({heading, two.label2 + label2, two.key2 + state,
                                        two.label1 + label1Alone}));
    if (two.textWidth <= room) return two;

    // One column: the pairs split, every key under every key.
    HintsLayout one;
    one.columns = 1;
    one.label1 = std::ceil(keyAny + gap);
    one.key2 = one.label1;   // unused: no second shortcut on a row
    one.label2 = one.label1;
    double longestLabel = 0, withState = 0;
    for (const HintRow& r : rows) {
        if (r.heading) continue;
        longestLabel = std::max(longestLabel, r.label1);
        if (r.hasKey2) longestLabel = std::max(longestLabel, r.label2);
    }
    one.state = std::ceil(one.label1 + longestLabel + gap);
    for (const HintRow& r : rows)
        if (!r.heading && r.hasState) withState = std::max(withState, one.state + r.state);
    one.textWidth = std::ceil(std::max({heading, one.label1 + longestLabel, withState}));
    if (one.textWidth > room) {
        // Labels that do not fit wrap under themselves, and a state goes
        // after its own label (the drawing's choice), not at a stop.
        one.wraps = true;
        one.textWidth = std::max(room, one.label1 + 1);
    }
    return one;
}

}  // namespace PageMeasure
