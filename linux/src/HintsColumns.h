// HintsColumns.h — the shortcut hints' list (Ctrl+Shift+H) in as many
// columns as fit the window's width, the Mac's (EditorController+StatusBar.mm,
// -hintsTextForRows:) on GTK. main.cpp writes the rows (HintRow,
// HintsSearchBox.h); this measures every key, label and heading with Pango
// in the fonts it draws them in (keys in the panel's monospace, the rest in
// the desktop's font, at the editor's zoom), and the core's
// PageMeasure::layoutHints cuts the list into as many columns as fit the
// room (16 at most): the tallest column as short as can be, then the
// heights as even as can be, a heading, a line of text or the blank line
// before "Hide these hints" kept with the row after it, a row's own
// second line (a shortcut with no key) with it, and each column's tab
// stops from its own widest key and label. Each column is one PangoLayout
// with its own stops, drawn a line at a time, line n at n line heights
// down, so the lines of the columns line up whatever fonts a line mixes; a
// heading has a blank line above it except at a column's top. With no room
// for one column the list is one column whose labels wrap under
// themselves, a state after its label.
//
// The search box's marks (HintsSearchBox::attributes) go on top of the
// fonts and colors, which neither move nor resize anything, so the layout
// stays as it was while searching. Drawn in a GtkDrawingArea as large as
// the text, which main.cpp puts in the panel's scrolled window.
#pragma once

#include <gtk/gtk.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "HintsSearchBox.h"

class HintsColumns {
public:
    // Deleted with its widget.
    HintsColumns();

    GtkWidget* widget() const { return area_; }

    // Lays `rows` out for `room` pixels of width, keys and labels at
    // `points` points, everything else at `scale` (the editor's zoom, 1
    // without one): each row told its column and its bytes there. The marks
    // for `query` go on top. The widget's size becomes the text's.
    void show(std::vector<HintRow>& rows, double room, double points, double scale,
              const std::string& query);
    // The marks for `query` again, nothing else changed. The top of the
    // highest mark in the widget, or -1 with none.
    double mark(const std::string& query);

    // What the checks read: the columns, each one's text, left edge and
    // layout; the text's size; whether the one column wraps.
    int count() const { return (int)columns_.size(); }
    std::string text(int column) const;
    double x(int column) const;
    PangoLayout* layout(int column) const;
    int textWidth() const { return width_; }
    int textHeight() const { return height_; }
    double lineHeight() const { return lineH_; }
    // The top of the line holding byte `index` of a column's text.
    double lineTop(int column, int index) const;
    bool wraps() const { return wraps_; }
    double points() const { return points_; }
    const std::vector<HintRow>& rows() const { return rows_; }

private:
    ~HintsColumns();
    static void destroyed(gpointer self);
    static void draw(GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer self);
    void clearColumns();
    void setFonts(double points, double scale);
    double measure(int font, const std::string& text);

    struct Column {
        double x = 0;
        std::string text;
        PangoLayout* layout = nullptr;
        PangoAttrList* base = nullptr;   // the fonts and colors, without marks
    };

    GtkWidget* area_ = nullptr;
    PangoLayout* scratch_ = nullptr;   // for measuring
    PangoFontDescription* fonts_[3] = {nullptr, nullptr, nullptr};   // key, label, heading
    double letterSpacing_ = 0;   // a heading's, in pixels
    double baseline_ = 0;        // from a line's top
    std::unordered_map<std::string, double> widths_[3];   // measured at points_
    double points_ = 0, scale_ = 0, lineH_ = 0;
    std::vector<Column> columns_;
    std::vector<HintRow> rows_;
    int width_ = 0, height_ = 0;
    bool wraps_ = false;
};
