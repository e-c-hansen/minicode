// LspPicker.h — two small pieces of the language server UI that LspSession
// drives (LspSessionActions.cpp), the Mac's src/LspPicker.{h,mm}:
//
// lspgtk::Picker, a list by the caret for Find All References (rows grouped
// under a heading per file) and Quick Fix. Like the completion list it is a
// popover that never takes the keyboard: the editor keeps it, and the
// view's key controller hands the list Up, Down, Page Up, Page Down, Return
// and Escape (LspSession::pickerKey), so typing anything else goes on into
// the text and closes it. A click on a row takes it.
//
// lspgtk::RenameBox, the field Rename Symbol asks for the new name in: a
// popover under the name with it selected, which has the keyboard while it
// shows; Enter renames, Escape or a click elsewhere cancels.
//
// Both are parented to the text view with gtk_widget_set_parent, so their
// owner deletes them when the view is unrealized (Lsp.cpp, the popovers'
// trap), and makes them again on next use.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

namespace lspgtk {

// One row of a Picker.
struct PickerRow {
    std::string markup;     // Pango markup
    bool heading = false;   // drawn, never selected, skipped by the keys
};

class Picker {
public:
    explicit Picker(GtkWidget* view);
    ~Picker();
    Picker(const Picker&) = delete;
    Picker& operator=(const Picker&) = delete;

    using Row = PickerRow;
    // `select` is the first row selected (moved past a heading). The list
    // opens under `at` (the view's coordinates), `width` wide, showing at
    // most `maxRows` rows before it scrolls.
    void show(const std::vector<Row>& rows, int select, const GdkRectangle& at, int width,
              int maxRows);
    void close();
    bool visible() const;
    // By `delta` selectable rows, wrapping at the ends for single steps and
    // stopping there for pages.
    void moveSelection(int delta);
    int selectedRow() const;
    // The rows as plain text, and a row's widget (for the checks).
    std::vector<std::string> rowTexts() const;
    GtkWidget* rowWidget(int row) const;
    // A row taken with Return or a click.
    std::function<void(int row)> onAccept;

private:
    static void onActivated(GtkListBox* box, GtkListBoxRow* row, gpointer self);
    void select(int row);
    GtkWidget* view_;
    GtkWidget* popover_ = nullptr;
    GtkWidget* scroll_ = nullptr;
    GtkWidget* list_ = nullptr;
    std::vector<Row> rows_;
    std::vector<GtkWidget*> labels_;
};

class RenameBox {
public:
    explicit RenameBox(GtkWidget* view);
    ~RenameBox();
    RenameBox(const RenameBox&) = delete;
    RenameBox& operator=(const RenameBox&) = delete;

    // Shows `name`, selected, pointing at `at` (the view's coordinates).
    void show(const std::string& name, const GdkRectangle& at);
    void close();
    bool visible() const;
    GtkWidget* entry() const { return entry_; }
    // Enter with a name typed (trimmed; never empty).
    std::function<void(const std::string& name)> onCommit;
    // The box went, by Enter, Escape or a click elsewhere.
    std::function<void()> onClose;

private:
    static void onActivate(GtkEntry* entry, gpointer self);
    static void onClosed(GtkPopover* popover, gpointer self);
    GtkWidget* view_;
    GtkWidget* popover_ = nullptr;
    GtkWidget* entry_ = nullptr;
};

}  // namespace lspgtk
