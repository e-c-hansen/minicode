// HintsSearchBox.h — the search box at the top of the shortcut hints
// (Ctrl+Shift+H), the Mac's HintsSearchBox.mm on GTK. While the panel
// shows, every key without Ctrl or Super, and not a function key or the
// Menu key, types into the box wherever the keyboard was: main.cpp's
// controller in the window's capture phase hands each key press to
// takeKey, which takes the keyboard at the first such key (the key goes on
// to the box's text, an Alt+letter as its letter, since an input method
// types nothing for one) and gives it back when the panel closes. No
// command and no view sees those keys. Esc clears the box, then closes the
// panel; Return and Tab do nothing. A click puts the keyboard in the box
// too.
//
// What a query finds is the core's (HintsSearch.h, Spelling::Linux, as the
// hints write keys: "Ctrl+Shift+H", Commands::keyLabel); `attributes` marks
// it on the panel's columns (HintsColumns.h) and dims the rest, which stays
// where it was.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

// A line of the hints panel as main.cpp writes it, and where HintsColumns
// put it: its column, and its bytes in that column's text. A heading; a
// shortcut (its key at the line's start, then what it does, then a pane's
// state, "open", drawn muted at its own stop); a line of text; or, as a
// Text with no text, the blank line before the last row. A shortcut with
// no key continues the one before it, a line under its text.
struct HintRow {
    enum Kind { Heading, Row, Text } kind;
    std::string key, text, state;
    int column = 0;
    size_t start = 0, textAt = 0, stateAt = 0, end = 0;
};

class HintsSearchBox {
public:
    // The box is deleted with its widget.
    HintsSearchBox();

    GtkWidget* widget() const { return entry_; }
    std::string query() const;
    // Empties the box (onChange runs when it held something).
    void clear();

    std::function<void()> onChange;   // the query changed
    std::function<void()> onClose;    // Esc with the box empty

    // A key press in the panel's window, seen in its capture phase while
    // the panel shows: true when the box took it (typed, cleared, closed,
    // or a Return or Tab it swallowed), false when it goes on as before.
    bool takeKey(GtkEventControllerKey* controller, guint keyval, GdkModifierType state);
    // The box has the keyboard.
    bool hasKeyboard() const;
    // Where the box took the keyboard from, while it has it; null when a
    // click put it there or that widget is gone.
    GtkWidget* previous() const { return previous_; }
    // Puts the keyboard back where the box took it from, if the box has it.
    // False when it had it but cannot tell where from (a click put it there).
    bool giveBackKeyboard();

    // For `query`, the marks on the hints' columns laid out from `rows`,
    // added to `lists`, one a column (a row's `column` says which): where
    // each shortcut or line found matched (a background), and every
    // shortcut or line not found, and a heading with nothing found under
    // it, dimmed (a foreground alpha). False, with nothing added, for an
    // empty query. `hits` gets each mark's column and byte index.
    static bool attributes(const std::vector<HintRow>& rows, const std::string& query,
                           const std::vector<PangoAttrList*>& lists,
                           std::vector<std::pair<int, size_t>>* hits = nullptr);

private:
    ~HintsSearchBox();
    static void destroyed(gpointer self);
    static void changed(GtkEditable*, gpointer self);
    static void focusLeft(GtkEventControllerFocus*, gpointer self);
    GtkWidget* text() const;
    void setPrevious(GtkWidget* w);
    void escape();

    GtkWidget* entry_ = nullptr;
    GtkWidget* previous_ = nullptr;   // weak
    std::string last_;
};
