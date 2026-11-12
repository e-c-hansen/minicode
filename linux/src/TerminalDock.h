// TerminalDock.h — the terminal panel's tabs (PARITY.md 4.7, the Mac's
// MCTerminalPanel without its splits): a TabStrip and a + above a stack of
// Terminals, each with a shell of its own. The tab on show is the active
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

    int count() const { return (int)terms_.size(); }
    int activeIndex() const { return active_; }
    Terminal* active() const { return terms_.empty() ? nullptr : terms_[(size_t)active_]; }
    Terminal* at(int i) const { return i >= 0 && i < count() ? terms_[(size_t)i] : nullptr; }
    int indexOf(const Terminal* t) const;
    const std::vector<Terminal*>& all() const { return terms_; }

    // A new terminal right of the active one, shown. `title` is a user title.
    Terminal* newTerminal(const std::string& title = std::string());
    // Shows tab `i`; the keyboard goes to it when `focus`.
    void select(int i, bool focus = true);
    void next();       // wrapping
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
    void refresh();
    void activate(int i);
    void remove(int i);

    std::function<Terminal*()> make_;
    GtkWidget* root_ = nullptr;
    GtkWidget* stack_ = nullptr;
    GtkWidget* plus_ = nullptr;
    GtkWidget* rename_ = nullptr;   // the rename popover while it is up
    TabStrip* strip_ = nullptr;
    std::vector<Terminal*> terms_;
    int active_ = 0;
    bool closingAll_ = false;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

#endif
