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
//
// A first row that looks like a header (Csv::looksLikeHeader) is shown as the
// columns' names, bold, in the table's header, which stays in view as the
// rows scroll; the right click menu's First Row Is Header turns that on and
// off, and the choice is remembered for the file (a list of files and their
// choices in $XDG_STATE_HOME/mutible/csv-headers.txt, the 500 most recent).
// Without one the columns are named A, B, C, as a spreadsheet's, and so is
// a column whose header field is empty. Rows and columns in this API are the
// file's (row 0 is the header row when there is one); the grid's rows are
// those under the header.
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

    // Shows `text`, the file `path`'s (empty for none); any cell being typed
    // is dropped. The first row is the header as the reader said for the
    // path, else as the core guesses.
    void show(const std::string& text, char delimiter, const std::string& path = "");
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

    // Whether the first row is shown as the columns' names. Setting it
    // remembers the choice for the file.
    bool firstRowIsHeader() const { return header_; }
    void setFirstRowIsHeader(bool on);
    // The title of file column `c` (0-based), as shown.
    std::string columnTitle(size_t c) const;
    // The right click menu's First Row Is Header, a check button in a
    // popover; toggling it is choosing the item.
    GtkWidget* headerMenuItem() const { return headerItem_; }

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
    static void onRightClick(GtkGestureClick*, int, double, double, gpointer);
    static void onHeaderToggled(GtkCheckButton*, gpointer);
    void build();
    void load(const std::string& text, char delimiter);
    // The rows above the grid's first: 1 when the first row is the header.
    size_t headerRows() const { return header_ && doc_.rows() > 0 ? 1 : 0; }

    GtkWidget* scroller_ = nullptr;
    GtkWidget* view_ = nullptr;
    GtkStringList* rowsModel_ = nullptr;
    Csv::Document doc_;
    std::string text_;
    GtkWidget* editing_ = nullptr;   // the label being typed in
    bool binding_ = false;
    std::string path_;               // the file shown, for remembering the header choice
    bool header_ = false;            // the first row shows as the columns' names
    GtkWidget* menu_ = nullptr;      // the right click popover
    GtkWidget* headerItem_ = nullptr;
    bool syncing_ = false;           // the menu item set from the table, not chosen
};
