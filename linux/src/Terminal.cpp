// Terminal.cpp — see Terminal.h. Entirely compiled out unless
// MINICODE_ENABLE_TERMINAL is set.
#include "Terminal.h"

#ifdef MINICODE_ENABLE_TERMINAL

#include <vte/vte.h>
#include <cstdlib>

Terminal::Terminal(const std::string& cwd) : cwd_(cwd) {
    root_ = gtk_scrolled_window_new();
    gtk_widget_set_size_request(root_, -1, 200);

    vte_ = vte_terminal_new();
    vte_terminal_set_scrollback_lines(VTE_TERMINAL(vte_), 10000);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(root_), vte_);

    spawnShell();
}

void Terminal::spawnShell() {
    const char* shell = g_getenv("SHELL");
    if (!shell || !*shell) shell = "/bin/bash";

    char* argv[] = { (char*)shell, nullptr };

    // vte_terminal_spawn_async is the current API (the sync variant is
    // deprecated in recent VTE). NULL callback = fire and forget.
    vte_terminal_spawn_async(
        VTE_TERMINAL(vte_),
        VTE_PTY_DEFAULT,
        cwd_.empty() ? nullptr : cwd_.c_str(),  // working directory
        argv,
        nullptr,                                 // envv (inherit)
        G_SPAWN_DEFAULT,
        nullptr, nullptr, nullptr,               // child setup
        -1,                                      // timeout
        nullptr,                                 // cancellable
        nullptr, nullptr);                       // callback / user data
}

void Terminal::focus() {
    if (vte_) gtk_widget_grab_focus(vte_);
}

#endif // MINICODE_ENABLE_TERMINAL
