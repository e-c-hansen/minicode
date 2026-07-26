// Terminal.h — a collapsible bottom panel embedding a real VTE terminal that
// spawns the user's $SHELL. Unlike the macOS build (a command runner with a
// sentinel protocol over a pipe), VTE is a genuine terminal emulator, so this
// is much simpler.
//
// The whole feature is compiled only when MINICODE_ENABLE_TERMINAL is defined
// (i.e. when libvte-2.91-gtk4 was found at configure time). Callers must guard
// their use of this class with the same macro.
#pragma once

#ifdef MINICODE_ENABLE_TERMINAL

#include <gtk/gtk.h>
#include <string>

class Terminal {
public:
    explicit Terminal(const std::string& cwd);
    GtkWidget* widget() const { return root_; }
    void focus();

private:
    void spawnShell();
    std::string cwd_;
    GtkWidget*  root_ = nullptr;   // container
    GtkWidget*  vte_  = nullptr;   // VteTerminal
};

#endif // MINICODE_ENABLE_TERMINAL
