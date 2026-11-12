// TerminalGroup.h — one terminal tab's panes (PARITY.md 4.7, the Mac's
// MCTerminalPanel layout): columns side by side, each a stack of terminals,
// every size a share kept by the core's TerminalLayout. The group lays its
// panes out itself (a custom layout on a box) and owns the drag bars on the
// boundaries, made once per boundary so a drag is never cut short by a
// rebuild. It owns its Terminals (deleting the group deletes them), but a
// pane taken out is handed back for the caller to delete.
//
// Compiled only with MINICODE_ENABLE_TERMINAL.
#pragma once

#ifdef MINICODE_ENABLE_TERMINAL

#include <gtk/gtk.h>

#include <functional>
#include <vector>

#include "Terminal.h"
#include "TerminalLayout.h"

class TerminalGroup {
public:
    // Between panes, where a drag bar sits; and the least a pane may be.
    static constexpr double kGap = 4;
    static constexpr int kMinColumns = 20;
    static constexpr int kMinRows = 5;
    enum class Dir { Left, Right, Up, Down };

    explicit TerminalGroup(Terminal* first);
    ~TerminalGroup();   // deletes its terminals
    TerminalGroup(const TerminalGroup&) = delete;
    TerminalGroup& operator=(const TerminalGroup&) = delete;

    GtkWidget* widget() const { return area_; }
    // Every pane, column by column, each column top to bottom.
    std::vector<Terminal*> panes() const;
    int count() const;
    bool contains(const Terminal* t) const { return find(t, nullptr, nullptr); }
    bool find(const Terminal* t, int* col, int* row) const;
    Terminal* active() const { return active_; }
    void setActive(Terminal* t) { if (contains(t)) active_ = t; }
    const TerminalLayout::Layout& layout() const { return layout_; }

    // What splitting `of` would do (the core's rule: halve, even or refuse)
    // without doing it; `down` stacks, else a new column.
    TerminalLayout::Split plan(const Terminal* of, bool down) const;
    // Adds `fresh` after `of` by that rule; Refused adds nothing and leaves
    // `fresh` the caller's.
    TerminalLayout::Split split(Terminal* of, Terminal* fresh, bool down);
    // Takes `t` out (not deleted) and gives its share to its neighbour;
    // returns the pane that is next to have the keyboard (null: it was the
    // last).
    Terminal* take(Terminal* t);
    // The boundary beside `of` moves `cells` toward Dir (VS Code's
    // resizePane); false when nothing could move.
    bool resize(const Terminal* of, Dir d, int cells);

    // The pane's size in points as laid out, and the bar boxes (for the checks).
    bool frameOf(const Terminal* t, TerminalLayout::Rect* out) const;
    int columnBars() const { return (int)colBars_.size(); }
    int rowBars() const { return (int)rowBars_.size(); }
    // The center of a bar in the group's own coordinates: false when none.
    bool barCenter(bool columnBar, int index, double* x, double* y) const;

private:
    struct Bar {
        TerminalGroup* group;
        GtkWidget* widget;
        bool vertical;     // a column's boundary (a vertical line)
        int col = 0, idx = 0;
        double start = 0;
    };
    void cell(const Terminal* t, double* w, double* h) const;
    void syncBars();
    Bar* makeBar(bool vertical);
    void place(int w, int h);
    double total(bool horizontal) const;
    void dragTo(Bar* b, double offset);
    static void measure(GtkWidget*, GtkOrientation, int, int*, int*, int*, int*);
    static void allocate(GtkWidget*, int w, int h, int);

    GtkWidget* area_ = nullptr;
    std::vector<std::vector<Terminal*>> cols_;
    TerminalLayout::Layout layout_;
    Terminal* active_ = nullptr;
    std::vector<Bar*> colBars_, rowBars_;
    Bar* dragging_ = nullptr;
};

#endif
