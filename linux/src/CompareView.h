// CompareView.h — a side-by-side diff in the editor's slot, the Mac's
// MCCompareView (src/CompareView.mm), linux/PARITY.md item 3.2: the old text
// on the left, the new on the right, each read-only with its line numbers,
// lined up row by row (ComparePanes.h) so equal lines sit side by side and a
// line only one side has faces a hatched filler row. Removed lines are tinted
// red on the left, added ones green on the right (the git panel's diff
// colors), the words that changed inside a changed line more strongly, both
// panes take the file's syntax colors, and the two scroll together. F7 and
// Shift+F7 (VS Code's Go to Next and Previous Difference) step through the
// runs of changes, wrapping around.
//
// The panes are drawn by hand, never laid out as text: every row has the
// one fixed height, so a row's place is arithmetic and only the rows on
// screen are shaped (the Mac's trap was 1.5 s laying out 50,000 lines a
// side). Each pane is a GtkDrawingArea the size of what is on show (never of
// the text: a widget a million pixels tall cost seconds), with scroll bars of
// its own beside it; the two panes share one vertical adjustment, so they
// scroll together, and each has a horizontal one.
//
// Accept and Reject per hunk, the decision bar and its grace (the Mac's
// MCComparison decision) are part two: `data()` has the hunks and rows they
// need, and a gutter between the panes is where the buttons go.
#pragma once

#include <gtk/gtk.h>

#include <memory>
#include <string>
#include <vector>

#include "Settings.h"
#include "SyntaxHighlighter.h"
#include "ComparePanes.h"

class CompareView {
public:
    struct Color { double r, g, b, a; };   // 0..1
    CompareView();
    ~CompareView();
    CompareView(const CompareView&) = delete;
    CompareView& operator=(const CompareView&) = delete;

    GtkWidget* widget() const { return box_; }
    // What is on show, shared with its tab; at the top, no hunk current.
    void show(std::shared_ptr<const CompareData> data);
    const CompareData* data() const { return data_.get(); }
    // The editor's colors (Editor::applySettings).
    void applySettings(const Settings& s);
    // F7 (forward) and Shift+F7: the next or previous run of changes after the
    // current one, wrapping, scrolled into view. False when there are none.
    bool step(bool forward);
    // The row of the hunk last stepped to, or -1.
    long currentRow() const { return current_; }
    // Where the panes are, in rows (the first on show).
    long topRow(bool left) const;
    void scrollToRow(long row);
    GtkWidget* focusWidget() const { return area_[1]; }

    // ---- for the checks
    GtkWidget* pane(bool left) const { return area_[left ? 0 : 1]; }
    GtkAdjustment* vadjustment(bool) const { return vadj_; }   // one for both panes
    int rowHeight() const { return rowH_; }
    // The strings drawn marked as changed words on a row of a pane.
    std::vector<std::string> wordsOnRow(bool left, size_t row) const;
    // The syntax style of the byte `col` of row `row`'s text, or -1 for none.
    int styleAt(bool left, size_t row, size_t col) const;

private:
    void measure();
    void draw(int side, cairo_t* cr, int width, int height);
    void resized(int side, int w, int h);
    void fitContent();
    static void onDraw(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer self);
    static void onResize(GtkDrawingArea*, int w, int h, gpointer self);
    static gboolean onScroll(GtkEventControllerScroll*, double dx, double dy, gpointer self);
    static void onAdjusted(GtkAdjustment*, gpointer self);
    static gboolean onKey(GtkEventControllerKey*, guint key, guint code, GdkModifierType mods,
                          gpointer self);
    static void onPressed(GtkGestureClick*, int n, double x, double y, gpointer self);

    GtkWidget* box_ = nullptr;
    GtkAdjustment* vadj_ = nullptr;
    GtkAdjustment* hadj_[2] = {nullptr, nullptr};
    GtkWidget* area_[2] = {nullptr, nullptr};
    std::shared_ptr<const CompareData> data_;
    int rowH_ = 18, charW_ = 8, gutterW_ = 48;
    PangoFontDescription* font_ = nullptr;
    long current_ = -1;
    Color bg_{0.12, 0.12, 0.12, 1}, fg_{0.83, 0.83, 0.83, 1}, muted_{0.55, 0.55, 0.55, 1};
    Color syntax_[kTokenStyleCount];
};
