// CsvTable.h — a CSV or TSV file as a table in the editor's slot, the GTK
// side of the Mac's CsvTable.mm over the core's Csv.
//
// The table is a view over the tab's text. It parses the text it is given
// (Csv::Document keeps every row's and field's bytes), and a cell edited in
// it becomes one replacement of the tab's GtkTextBuffer: the serialized
// table is compared with the text before it, and only the bytes between
// their common start and end are replaced (whole characters), so one cell
// changes one line and an unedited file is never rewritten. The editor makes
// the replacement one user action, so undo, dirty, backups and saving work as
// for text; text changed from elsewhere (an undo, a reload) comes back through
// reload().
//
// A cell is a GtkEditableLabel: a click or Enter on it starts typing, Enter
// or leaving it commits, Esc drops. commit() finishes a cell being typed
// (the editor calls it before the tab goes), so a cell is never committed
// into the next file.
#pragma once

#include <gtk/gtk.h>
#include <functional>
#include <string>

#include "Csv.h"

class CsvTable {
public:
    // Files over this are shown as text (the Mac reads those read-only).
    static constexpr size_t kMaxTableBytes = 50u * 1024 * 1024;

    CsvTable();
    ~CsvTable();
    CsvTable(const CsvTable&) = delete;
    CsvTable& operator=(const CsvTable&) = delete;

    GtkWidget* widget() const { return scroller_; }

    // Shows `text`; any cell being typed is dropped.
    void show(const std::string& text, char delimiter);
    // The same text changed elsewhere, shown again keeping the scroll.
    void reload(const std::string& text);
    // Forgets the table.
    void clear();
    // Finishes the cell being typed, if any: the edit goes to onReplace.
    void commit();
    bool editing() const { return editing_ != nullptr; }

    // A cell's new text, as typing it and pressing Enter does. False when
    // it changed nothing.
    bool setCell(size_t row, size_t column, const std::string& value);

    size_t rows() const { return doc_.rows(); }
    size_t columns() const { return doc_.columns(); }
    std::string cell(size_t r, size_t c) const { return doc_.cell(r, c); }
    char delimiter() const { return doc_.delimiter(); }
    const std::string& text() const { return text_; }

    // The text from `start` characters in, `removed` characters long, is
    // replaced by `inserted`.
    std::function<void(int start, int removed, const std::string& inserted)> onReplace;
    // Ctrl+Z and Ctrl+Shift+Z / Ctrl+Y with no cell being typed.
    std::function<void(bool redo)> onUndo;

private:
    static void bindCell(GtkListItemFactory*, GtkListItem*, gpointer);
    static void setupCell(GtkListItemFactory*, GtkListItem*, gpointer);
    static void bindNumber(GtkListItemFactory*, GtkListItem*, gpointer);
    static void setupNumber(GtkListItemFactory*, GtkListItem*, gpointer);
    static void onEditing(GObject*, GParamSpec*, gpointer);
    static gboolean onKey(GtkEventControllerKey*, guint, guint, GdkModifierType, gpointer);
    void build();

    GtkWidget* scroller_ = nullptr;
    GtkWidget* view_ = nullptr;
    GtkStringList* rowsModel_ = nullptr;
    Csv::Document doc_;
    std::string text_;
    GtkWidget* editing_ = nullptr;   // the label being typed in
    bool binding_ = false;
};
