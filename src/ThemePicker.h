// ThemePicker.h — the color theme picker, VS Code's Preferences: Color Theme
// (workbench.action.selectTheme): the order its list shows the themes in,
// and what moving through the list, choosing a row, and backing out do to
// the theme on show. Pure logic, no I/O; the Mac shows it in the palette
// (mac/src/ThemePickerPanel.mm) and applies each preview without writing
// the settings file, which only a choice changes.
//
// A session starts from the theme the settings file sets. Each row the list
// comes to (an arrow key, the pointer over a row, typing that filters the
// list) is shown at once; Return keeps the row's theme, which the caller
// then writes as View > Theme does; Esc, a click outside, or the keyboard
// going elsewhere puts the set theme back and writes nothing.
#pragma once

#include <string>
#include <vector>

namespace ThemePicker {

struct Row {
    std::string id;      // "monokai", the settings file's theme value
    std::string title;   // "Monokai"
    bool dark = true;
    // "dark themes" or "light themes" on the first row of each group, as
    // VS Code's list marks them at the right; "" on the rest.
    std::string group;
};

// Every theme (Settings::themes), the dark ones first, then the light ones,
// as View > Theme lists them; each group by title, case ignored.
std::vector<Row> rows();
// The index of the row with this theme id; -1 for none.
int rowOf(const std::vector<Row>& rows, const std::string& id);

class Session {
public:
    // setTheme: the id the settings file sets now (Settings::theme().id).
    explicit Session(const std::string& setTheme);

    const std::vector<Row>& rows() const { return rows_; }
    // The row selected when the list opens: the set theme's, else the first.
    int initialRow() const;
    const std::string& setTheme() const { return set_; }
    // The theme on show now: the set one until a preview, then the last
    // one previewed.
    const std::string& showing() const { return showing_; }
    // Whether something other than the set theme is on show.
    bool previewing() const { return showing_ != set_; }
    bool isOpen() const { return open_; }

    // The list came to a row: the theme to show now, or "" when there is
    // nothing to do (already on show, no such row, or the session is over).
    std::string preview(int row);
    // Return on a row: the theme to write, or "" when nothing need be
    // written (the set theme chosen, no such row, or the session is over).
    // The session ends (unless there is no such row); the caller ends the
    // preview after writing, so a write that fails or is refused leaves the
    // set theme on show.
    std::string choose(int row);
    // Closed without a choice: the theme to put back on show, or "" when
    // the set theme is on show already. The session ends.
    std::string cancel();

private:
    std::vector<Row> rows_;
    std::string set_, showing_;
    bool open_ = true;
};

}  // namespace ThemePicker
