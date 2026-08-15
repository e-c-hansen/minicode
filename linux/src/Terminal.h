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
#include <vte/vte.h>   // VteTerminal appears in the signal handler signatures
#include <string>

class Terminal {
public:
    // Height limits for the draggable divider above the panel. The macOS build
    // clamps a terminal drag to 80…H-120 and opens at 220 (EditorController.mm
    // -relayoutRightArea / -mouseDragged); the Linux paned uses the same
    // numbers, so the panel feels the same on both platforms.
    static constexpr int kMinHeight     = 80;
    static constexpr int kDefaultHeight = 220;

    explicit Terminal(const std::string& cwd);
    GtkWidget* widget() const { return root_; }
    void focus();

private:
    void spawnShell();
    void applyTheme();
    void feedNotice(const std::string& text);

    // The shell is restarted in place when it exits, so Ctrl+D does not leave a
    // dead panel behind. lastSpawn_/rapidExits_ break the loop a shell that
    // cannot start would otherwise spin in.
    static void onChildExited(VteTerminal* term, gint status, gpointer self);
    static void onSpawned(VteTerminal* term, GPid pid, GError* error, gpointer self);

    std::string cwd_;
    GtkWidget*  root_ = nullptr;   // container
    GtkWidget*  vte_  = nullptr;   // VteTerminal
    gint64      lastSpawn_ = 0;    // g_get_monotonic_time of the last spawn
    int         rapidExits_ = 0;   // consecutive exits inside a second
};

#endif // MINICODE_ENABLE_TERMINAL
