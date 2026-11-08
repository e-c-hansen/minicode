// TerminalLayout.h — the arithmetic of a terminal tab's panes: columns side
// by side, each a stack of panes one above the other (one level deep, as VS
// Code's terminal groups hold one direction each), every size a fraction of
// what holds it, so a window made larger or smaller keeps the proportions.
// Splitting, closing, dragging a bar and resizing with the keyboard work on
// one axis at a time, a list of fractions that sums to 1, and each keeps a
// pane at least its minimum (20 columns wide, 5 rows tall, in points as the
// caller measures them). Plain C++ with no I/O, so it is unit-tested; the
// Mac's MCTerminalPanel keeps the terminals and lays them out from it.
#pragma once

#include <cstddef>
#include <vector>

namespace TerminalLayout {

// One axis: the parts' shares of it, first to last. Never empty in use.
using Fractions = std::vector<double>;

// Whole points for each part when `total` points are shared among them with
// `gap` points between neighbours: each part's share rounded down, the last
// taking what is left, so the parts and gaps fill `total` exactly.
std::vector<double> lengths(const Fractions &fr, double total, double gap);

// Where part i starts, in points from the axis's start.
double startOf(const Fractions &fr, size_t i, double total, double gap);

// The middle of the gap after part i (where its drag bar sits).
double boundaryAt(const Fractions &fr, size_t i, double total, double gap);

// How a split went.
enum class Split {
    Refused,   // even equal shares would leave a part under `min`; unchanged
    Halved,    // part i was cut in two, the others kept their shares
    Evened,    // halving would not fit, so every part got an equal share
};
// A new part right after part i: part i's share halved when both halves and
// every other part keep `min`, else equal shares for all when each keeps
// `min`, else nothing changes.
Split split(Fractions &fr, size_t i, double total, double gap, double min);

// Part i taken out, its share given to the part before it (after it, for
// the first), as VS Code's split view does. An only part leaves {} behind.
void remove(Fractions &fr, size_t i);

// Dragging the bar after part i to `pos` (the gap's middle, in points from
// the axis's start): parts i and i+1 share their two lengths anew, each at
// least `min`. NO when nothing changed (no room for two of `min`, or the bar
// is already there).
bool dragBoundary(Fractions &fr, size_t i, double pos, double total, double gap, double min);

// A resize from the keyboard (VS Code's resizePane): the boundary between
// part i and its neighbour (the part after it; the part before it, for the
// last) moves `amount` points toward the end of the axis (Right, Down) or
// toward its start, each part keeping `min`. NO with one part, or when
// neither may give way.
bool nudge(Fractions &fr, size_t i, bool towardEnd, double amount, double total, double gap,
           double min);

// Equal shares for n parts.
Fractions even(size_t n);

// A tab: its columns' shares of the width, and each column's panes' shares
// of the height, top first.
struct Layout {
    Fractions columns{1.0};
    std::vector<Fractions> rows{Fractions{1.0}};
};

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;   // y from the top
};

// Every pane's frame, [column][row], in a tab `width` by `height` points
// with `gap` points between panes.
std::vector<std::vector<Rect>> frames(const Layout &l, double width, double height, double gap);

// Pane (col, row) closed: a stack loses that row, and a column whose last
// pane it was goes, its width given to its neighbour.
void removePane(Layout &l, size_t col, size_t row);

}  // namespace TerminalLayout
