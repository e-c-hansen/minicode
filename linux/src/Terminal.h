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
#include <functional>
#include <string>
#include <vector>

#include "Settings.h"

class Terminal {
public:
    // Height limits for the draggable divider above the panel. The macOS build
    // clamps a terminal drag to 80…H-120 and opens at 220 (EditorController.mm
    // -relayoutRightArea / -mouseDragged); the Linux paned uses the same
    // numbers, so the panel feels the same on both platforms.
    static constexpr int kMinHeight     = 80;
    static constexpr int kDefaultHeight = 220;

    explicit Terminal(const std::string& cwd);
    // Called while the window's widgets still exist (main.cpp tears a window
    // down from GtkApplication's window-removed). VTE hangs the shell up when
    // its widget goes; this only makes sure it is not restarted.
    ~Terminal();
    GtkWidget* widget() const { return root_; }
    void focus();
    // Is `w` the terminal itself or inside it? The window asks this of its
    // focus widget to decide which shortcuts belong to the shell.
    bool owns(GtkWidget* w) const;
    // Background (with its opacity) and text color from the settings file.
    // VTE redraws everything already on screen in the new colors.
    void applySettings(const Settings& s);

    // Ctrl+click on terminal output (the Mac's Cmd+click, Terminal.mm). The
    // shared TermLinks core finds URLs and file references such as
    // src/foo.cpp:42:7 in the clicked line; a file reference counts only when
    // it names something that exists, tried against the shell's directory and
    // then the project folder. Holding Ctrl over one underlines it.
    struct Link {
        bool isUrl = false;
        bool isDir = false;
        bool insideRoot = false;   // a file or folder in the project folder
        std::string target;        // the URL, or the resolved absolute path
        int line = 0;              // 1-based, 0 when the reference named none
        int column = 0;            // 1-based characters, 0 when none
    };
    void setLinkHandler(std::function<void(const Link&)> handler) { onLink_ = std::move(handler); }
    // The folder relative references fall back to after the shell's own.
    void setProjectRoot(const std::string& dir) { root_dir_ = dir; }
    // The link at a point in the terminal's own coordinates, if any.
    bool linkAt(double x, double y, Link* out);
    // What a Ctrl+click at that point does: true when there was a link to open.
    bool openLinkAt(double x, double y);

private:
    // One row's share of the hovered link: cells [col0, col1) of `row`, a
    // row of the whole buffer (scrollback included), not of the screen.
    struct Span { long row, col0, col1; };
    bool findLink(double x, double y, Link* out, std::vector<Span>* spans);
    std::string shellDirectory() const;
    double topRow();                               // buffer row at the top of the view
    void setHover(bool ctrl);                      // re-evaluate at the pointer
    static void drawHover(GtkDrawingArea* area, cairo_t* cr, int w, int h, gpointer self);

    void spawnShell();
    void applyTheme();
    void feedNotice(const std::string& text);

    // The shell is restarted in place when it exits, so Ctrl+D does not leave a
    // dead panel behind. lastSpawn_/rapidExits_ break the loop a shell that
    // cannot start would otherwise spin in.
    static void onChildExited(VteTerminal* term, gint status, gpointer self);
    static void onSpawned(VteTerminal* term, GPid pid, GError* error, gpointer self);

    std::string cwd_;
    std::string root_dir_;          // the window's folder, for links
    GtkWidget*  root_ = nullptr;   // container: the scroller, the underline over it
    GtkWidget*  vte_  = nullptr;   // VteTerminal
    GtkWidget*  underline_ = nullptr;   // draws the hovered link's underline
    GPid        shellPid_ = -1;

    std::function<void(const Link&)> onLink_;
    // The pointer over the terminal, in its coordinates, and the link under
    // it while Ctrl is held (empty spans: none).
    bool   pointerIn_ = false;
    double pointerX_ = 0, pointerY_ = 0;
    bool   ctrlDown_ = false;
    std::vector<Span> hover_;
    bool   handCursor_ = false;
    guint  cursorIdle_ = 0;
    GtkEventController* windowKeys_ = nullptr;   // on the window, for Ctrl alone
    GtkWidget* keysWindow_ = nullptr;
    GdkRGBA    linkColor_{};
    long       topDelta_ = 0;       // topRow's offset from the adjustment
    bool       topKnown_ = false;
    std::vector<GObject*> handlers_;   // objects with signals of ours to take off
    gint64      lastSpawn_ = 0;    // g_get_monotonic_time of the last spawn
    int         rapidExits_ = 0;   // consecutive exits inside a second
    GdkRGBA     palette_[16];
    GCancellable* spawnCancel_ = g_cancellable_new();
};

#endif // MINICODE_ENABLE_TERMINAL
