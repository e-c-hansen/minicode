// TerminalDock.h — the terminal panel's tabs (PARITY.md 4.7, the Mac's
// MCTerminalPanel): a TabStrip and a + above a stack of tabs, each a
// TerminalGroup of panes (columns side by side, each a stack), every pane a
// Terminal with a shell of its own. The tab on show is the active
// terminal; hidden tabs keep their shells running. A user title (Rename
// Terminal…) wins over the title a program sets, tabs reorder by a drag or
// Move Terminal Left and Right, and a terminal with a running program asks
// before it closes. The last tab closing hides the dock (onEmpty) and leaves
// a fresh terminal whose shell starts when the dock is shown again, so there
// is always one to ask.
//
// Compiled only with MINICODE_ENABLE_TERMINAL.
#pragma once

#ifdef MINICODE_ENABLE_TERMINAL

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Terminal.h"
#include "TerminalGroup.h"

class TabStrip;

class TerminalDock {
public:
    // `make` builds one terminal with the window's hooks on it (its folder,
    // control id, links, settings); the dock starts with one.
    explicit TerminalDock(std::function<Terminal*()> make);
    ~TerminalDock();   // called while the window still exists
    TerminalDock(const TerminalDock&) = delete;
    TerminalDock& operator=(const TerminalDock&) = delete;

    GtkWidget* widget() const { return root_; }
    TabStrip* strip() const { return strip_; }
    GtkWidget* newTabButton() const { return plus_; }

    // Tabs. A tab's terminal (`at`) is the pane of it last used.
    int count() const { return (int)groups_.size(); }
    int activeIndex() const { return active_; }
    Terminal* active() const { return groups_.empty() ? nullptr : groups_[(size_t)active_]->active(); }
    Terminal* at(int i) const { return i >= 0 && i < count() ? groups_[(size_t)i]->active() : nullptr; }
    TerminalGroup* group(int i) const { return i >= 0 && i < count() ? groups_[(size_t)i].get() : nullptr; }
    TerminalGroup* activeGroup() const { return group(active_); }
    // The tab holding `t`, -1 for none.
    int indexOf(const Terminal* t) const;
    // Every pane of every tab, tab by tab.
    std::vector<Terminal*> all() const;

    // Splits (the Mac's Cmd+\ and Split Terminal Down): a new pane beside
    // (or below) the active one by the core's rule, with the keyboard.
    // Null, with a beep, when it would leave a pane under 20 columns or 5
    // rows.
    Terminal* split(bool down, bool focus = true);
    // Closes one pane: the tab with its last. `ask` false skips the question.
    void closePane(Terminal* t, bool ask = true);
    // The active pane's boundary moves 4 cells; false when nothing moved.
    bool resizeActive(TerminalGroup::Dir d);
    // The active pane is alone in its column: Up and Down resize the dock.
    bool activeAloneInColumn() const;
    // The keyboard to `t`, in its tab (shown first).
    void focusPane(Terminal* t);

    // A new terminal right of the active one, shown. `title` is a user title.
    Terminal* newTerminal(const std::string& title = std::string());
    // Shows tab `i`; the keyboard goes to it when `focus`.
    void select(int i, bool focus = true);
    void next();       // across panes and tabs, wrapping
    void previous();
    // Closes tab `i`, asking first when a program runs in it (confirmClose);
    // `ask` false closes at once.
    void close(int i, bool ask = true);
    void rename(int i, const std::string& title);   // "" gives the tab its own title back
    void askRename(int i);                          // a popover on the tab
    void move(int from, int to);
    void moveActive(int by);

    // Called whenever the active terminal is another (app->terminal follows).
    std::function<void(Terminal*)> onActiveChanged;
    // The last tab was closed: hide the dock.
    std::function<void()> onEmpty;
    // A tab added, closed, moved or renamed.
    std::function<void()> onChanged;
    // Asked before a busy terminal closes; `proceed` closes it. Unset: closes.
    std::function<void(Terminal*, std::function<void()> proceed)> confirmClose;

private:
    void add(Terminal* t, int at);
    void hook(Terminal* t);
    void refresh();
    void activate(int i);
    void remove(int i);          // a whole tab
    void removePane(Terminal* t);
    void step(int by);

    std::function<Terminal*()> make_;
    GtkWidget* root_ = nullptr;
    GtkWidget* stack_ = nullptr;
    GtkWidget* plus_ = nullptr;
    GtkWidget* rename_ = nullptr;   // the rename popover while it is up
    TabStrip* strip_ = nullptr;
    std::vector<std::unique_ptr<TerminalGroup>> groups_;
    int active_ = 0;
    bool closingAll_ = false;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

#endif
