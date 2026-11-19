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
// Keys are spelled by the core (Commands::keyLabel's Linux spelling:
// "Ctrl ⇧ P", thin spaces between the parts) and drawn in the keys' blue,
// Ctrl, Alt and Super each in a colour of its own from the theme
// (theme::hintsModifierColors), the spaces in a key half a monospace cell
// wide (a whole one spreads a key out, a proportional thin space is 2 pixels).
//
// While the search box holds a query, what it finds comes first, under
// its sections' headings, and the rest after it, dimmed
// (HintsSearch::bubbleMatches, the same layoutHints over the list so
// reordered, in no more columns than without a query, the panel no smaller
// than it was), the box's marks (HintsSearchBox::attributes) on top. The
// list is laid out again only when what the query finds changes; a query
// that finds the same rows only marks them again. Drawn in a
// GtkDrawingArea as large as the text, which main.cpp puts in the panel's
// scrolled window.
#pragma once

#include <gtk/gtk.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "HintsSearch.h"
#include "HintsSearchBox.h"
#include "PageMeasure.h"
#include "ThemeCss.h"

class HintsColumns {
public:
    // Deleted with its widget.
    HintsColumns();

    GtkWidget* widget() const { return area_; }

    // The colours of Ctrl, Alt and Super in the keys, from the next layout.
    void setModifierColors(const theme::HintsModifierColors& colors) { colors_ = colors; }
    // Lays `rows` out for `room` pixels of width, keys and labels at
    // `points` points, everything else at `scale` (the editor's zoom, 1
    // without one), what `query` finds first and marked, the rest after it
    // dimmed. rows() is the list as laid out, each row told its column and
    // its bytes there. The widget's size becomes the text's.
    void show(const std::vector<HintRow>& rows, double room, double points, double scale,
              const std::string& query);
    // The query changed: the list laid out again when what it finds is not
    // what it found (back as it was for an empty one), then its marks. The
    // top of the highest mark in the widget, or -1 with none.
    double mark(const std::string& query);

    // What the checks read: the columns, each one's text, left edge and
    // layout; the text's size; whether the one column wraps; the list as
    // laid out (a row of the rest, after what the query found, has `rest`);
    // how many times it was laid out.
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
    int layouts() const { return layouts_; }
    const theme::HintsModifierColors& modifierColors() const { return colors_; }

private:
    ~HintsColumns();
    static void destroyed(gpointer self);
    static void draw(GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer self);
    void clearColumns();
    void setFonts(double points, double scale);
    double measure(int font, const std::string& text);
    // A key's fonts and colours from byte `at` of `list` (the spaces in the
    // keys' font at half its size, Ctrl, Alt and Super in their colours).
    void keyAttributes(PangoAttrList* list, const std::string& key, size_t at, bool colors) const;
    // The list's items in the order `query` puts them; the whole list in
    // its order, `rest` its size, for an empty query.
    HintsSearch::Bubbled orderFor(const std::string& query) const;
    // The items laid out in `order_`; then the marks for `query_`, which
    // return the top of the highest, or -1.
    void layOut();
    double applyMarks();

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
    double points_ = 0, scale_ = 0, lineH_ = 0, room_ = 0;
    theme::HintsModifierColors colors_;
    // The rows as given, the core's items made of them (each one's rows,
    // and as measured), the order laid out, and the query.
    std::vector<HintRow> given_;
    std::vector<std::vector<size_t>> items_;
    std::vector<PageMeasure::HintItem> measured_;
    HintsSearch::Bubbled order_;
    std::string query_;
    // The list's layout without a query: its columns and size, which a
    // query's never exceeds and never falls short of.
    int plainCount_ = 1;
    int plainWidth_ = 0, plainHeight_ = 0;
    std::vector<Column> columns_;
    std::vector<HintRow> rows_;
    std::vector<size_t> rowAt_;   // each row's place in order_
    int width_ = 0, height_ = 0, layouts_ = 0;
    bool wraps_ = false;
};
