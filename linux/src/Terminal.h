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
#include <memory>
#include <string>
#include <vector>

#include "Settings.h"
#include "ShellIntegration.h"

class Terminal {
public:
    // Height limits for the draggable divider above the panel. The macOS build
    // clamps a terminal drag to 80…H-120 and opens at 220 (EditorController.mm
    // -relayoutRightArea / -mouseDragged); the Linux paned uses the same
    // numbers, so the panel feels the same on both platforms.
    static constexpr int kMinHeight     = 80;
    static constexpr int kDefaultHeight = 220;

    // The panel's widget at once; the terminal in it and its shell when the
    // panel is first shown (or focused), as the Mac makes its terminal dock
    // when first needed (PARITY.md R3).
    explicit Terminal(const std::string& cwd);
    // Called while the window's widgets still exist (main.cpp tears a window
    // down from GtkApplication's window-removed). VTE hangs the shell up when
    // its widget goes; this only makes sure it is not restarted.
    ~Terminal();
    GtkWidget* widget() const { return root_; }
    // Whether the terminal and its shell were made yet (for the checks).
    bool made() const { return vte_ != nullptr; }
    void focus();
    // The keyboard came into the terminal (a click, Tab, focus()): the dock
    // makes it the active one (PARITY.md 4.7, splits).
    std::function<void()> onFocusIn;
    // One character cell in points (a guess before the shell is made), and
    // the columns and rows VTE has been given (0 before).
    void cellSize(double* w, double* h) const;
    int columns() const;
    int rows() const;
    // Is `w` the terminal itself or inside it? The window asks this of its
    // focus widget to decide which shortcuts belong to the shell.
    bool owns(GtkWidget* w) const;
    // Background (with its opacity) and text color from the settings file.
    // VTE redraws everything already on screen in the new colors.
    void applySettings(const Settings& s);
    // The font: the desktop's monospace at `basePoints` (0: the desktop's
    // own size, as before), scaled by VTE's font scale for the terminals'
    // zoom (Zoom.h). VTE gives the shell the rows and columns that fit.
    void setFontSize(double basePoints, double scale);

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
    // The session's own folders (its scratchpad, its worktree) for the
    // session running here, asked at each click: a relative path that is not
    // in the shell's folder is looked for there before the project's.
    void setSessionRoots(std::function<std::vector<std::string>()> f) { sessionRoots_ = std::move(f); }
    void setLinkHandler(std::function<void(const Link&)> handler) { onLink_ = std::move(handler); }
    // The folder relative references fall back to after the shell's own.
    void setProjectRoot(const std::string& dir) { root_dir_ = dir; }
    // The link at a point in the terminal's own coordinates, if any.
    bool linkAt(double x, double y, Link* out);
    // What a Ctrl+click at that point does: true when there was a link to open.
    bool openLinkAt(double x, double y);

    // Run in Terminal (PARITY.md 2.8, the Mac's typeCommand:): `text` at
    // the shell's prompt as a paste (in brackets when the shell asked for
    // them), never with Return, for the user to read and run. The terminal
    // and its shell are made first if need be, and the text waits for the
    // shell's first output (its prompt). False, with nothing typed, while
    // a program other than the shell has the terminal.
    bool typeText(const std::string& text);
    // A program other than the shell has the terminal (its foreground
    // process group is not the shell's).
    bool busy() const;
    // `minicode term --run` and Run File (PARITY.md 3.1): `line` (one line)
    // typed at the shell's prompt as a paste and entered with Return, so it
    // runs visibly and goes through the shell's history. Nothing is typed
    // unless the integration says the shell is at a prompt and the shell
    // has the terminal's foreground: a running command, a shell that does
    // not report (another shell, ssh, an exec'd program) or an unknown state
    // is refused with the reason. A shell still starting, a command just
    // typed, and a foreground other than the shell's for under half a second
    // are waited for (15 s, 2 s and 0.5 s at most). `done` gets "" once the
    // line is typed, else the reason, possibly before this returns. The
    // terminal is made first if need be.
    void runLine(const std::string& line, std::function<void(const std::string&)> done);
    // Why runLine would not type now, "" when it would (for the status bar
    // and the checks); `*wait` says the reason is one runLine waits out.
    std::string runBlocker(bool* wait = nullptr) const;
    // The last `lines` lines the terminal shows, scrollback included,
    // trailing blank lines left out (`minicode term-output`).
    std::string recentText(int lines);
    // The tab's name (PARITY.md 4.7): the user's own title if one was given
    // (Rename Terminal…), else the title the running program set (OSC 0/2),
    // else the shell's name.
    std::string title() const;
    const std::string& userTitle() const { return userTitle_; }
    void setUserTitle(const std::string& t);
    // The title changed (a program set one, or the user did).
    std::function<void()> onTitleChanged;
    // The shell's pid (the VTE child), 0 while there is none (PARITY.md 3.8).
    int shellPid() const { return shellPid_ > 0 ? (int)shellPid_ : 0; }

    // Waiting for you (PARITY.md 3.6). The terminal's id is the control
    // channel's (`minicode state`); the shell gets it as MINICODE_TERMINAL_ID
    // beside MINICODE_PID, set before the shell starts (an id given after the
    // shell is running reaches only the next one). The mark is a dot over the
    // terminal's top right corner with Claude's message as its tooltip:
    // "waiting" (orange), "done" (green), "" (none). Typing in the terminal
    // clears the board's mark, and so does the terminal going away.
    void setControlId(int id) { controlId_ = id; }
    int controlId() const { return controlId_; }
    void setAttention(const std::string& markName, const std::string& tooltip);
    // The mark shown now, for the checks: "waiting", "done" or "".
    const std::string& attentionShown() const { return attentionShown_; }
    // A command run with Return has finished: the shell has the terminal
    // again (the Mac's MCTerminalCommandDidFinishNotification). With the
    // shell integration it is the finishing mark; for a shell without it
    // each Return typed into the terminal starts a look, four times a
    // second, until the shell is in the foreground.
    std::function<void()> onCommandFinished;
    // "NAME=value" lines added to the shell's environment each time it is
    // started (CLAUDE_CODE_SSE_PORT while the IDE link is on).
    std::function<std::vector<std::string>()> extraEnvironment;

    // The shell integration (PARITY.md 3.1, D10). A bash or zsh shell is
    // started with minicode's script (bash --rcfile, zsh through ZDOTDIR's
    // .zshenv, both copied once a process into a folder of its own in the
    // user's runtime folder) and a nonce of 128 random bits in
    // MINICODE_NONCE; its hooks set the termprop vte.ext.minicode.mark,
    // and only a value carrying that nonce is believed. What the newest
    // one said: `known` stays false for another shell (fish, sh), one whose
    // integration did not load, or before the first prompt; unknown is
    // never idle. Every restart of the shell starts unknown with a new
    // nonce.
    const ShellIntegration::ShellState& shellState() const { return shell_; }
    // After each believed mark.
    std::function<void()> onShellStateChanged;
    // The folder holding this process's copies of the scripts ("" when it
    // could not be made), for the checks.
    static const std::string& integrationFolder();

private:
    // One row's share of the hovered link: cells [col0, col1) of `row`, a
    // row of the whole buffer (scrollback included), not of the screen.
    struct Span { long row, col0, col1; };
    bool findLink(double x, double y, Link* out, std::vector<Span>* spans);
    std::string shellDirectory() const;
    double topRow();                               // buffer row at the top of the view
    void setHover(bool ctrl);                      // re-evaluate at the pointer
    static void drawHover(GtkDrawingArea* area, cairo_t* cr, int w, int h, gpointer self);

    void makeTerminal();   // the terminal and its shell, once
    // The termprop the hooks set, installed once, before the first VteTerminal.
    static void installTermprops();
    static void onTermpropChanged(VteTerminal* term, const char* name, gpointer self);
    ShellIntegration::ShellState shell_;
    std::string nonce_;   // this shell's, "" when it runs without the integration
    void spawnShell();
    void applyTheme();
    void applyColors(const Settings& s);
    void feedNotice(const std::string& text);

    // The shell is restarted in place when it exits, so Ctrl+D does not leave a
    // dead panel behind. lastSpawn_/rapidExits_ break the loop a shell that
    // cannot start would otherwise spin in.
    static void onChildExited(VteTerminal* term, gint status, gpointer self);
    static void onSpawned(VteTerminal* term, GPid pid, GError* error, gpointer self);

    int         controlId_ = 0;
    std::string userTitle_;
    GtkEventController* focusCtl_ = nullptr;   // onFocusIn's
    GtkWidget*  attentionDot_ = nullptr;   // the mark, in the overlay
    std::string attentionShown_;
    std::string cwd_;
    std::string root_dir_;          // the window's folder, for links
    GtkWidget*  root_ = nullptr;   // container: the scroller, the underline over it
    GtkWidget*  vte_  = nullptr;   // VteTerminal
    GtkWidget*  underline_ = nullptr;   // draws the hovered link's underline
    GPid        shellPid_ = -1;

    std::function<void(const Link&)> onLink_;
    std::function<std::vector<std::string>()> sessionRoots_;
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
    std::unique_ptr<Settings> settings_;   // the app's last, for a terminal made later
    // setFontSize's, for a terminal made later.
    void applyFont();
    double fontBase_ = 0, fontScale_ = 1;
    bool   fontSet_ = false;
    GCancellable* spawnCancel_ = g_cancellable_new();
    // Run in Terminal's text, waiting for the shell's first output; and
    // the look for a command's end after Return (typeText, onCommandFinished).
    std::string pendingType_;
    guint  typeTimer_ = 0;
    int    typeTries_ = 0;
    bool   shellSpoke_ = false;   // the shell printed something since it started
    guint  finishTimer_ = 0;
    // runLine's wait, and the count of lines started when it last typed (-1
    // for none), so a second line waits for the first one's mark.
    guint  runTimer_ = 0;
    long long runStarted_ = -1;
    gint64 runTypedAt_ = 0, runBegan_ = 0;
    std::function<int(gint64, std::string*)> runAttempt_;
    std::function<void(const std::string&)> runDone_;
    void typePending();
    // The half-typed line (HANDOFF item 48): ESC [9999~ sets it aside before
    // a typed command, and in bash ESC [9998~ gives it back at the first
    // prompt after (zsh's push-input returns it by itself).
    bool bashShell_ = false;
    int  popOwed_ = 0;
    long long pushPrompts_ = 0;
    void setLineAside();
    void giveLineBack();
    static void onCommit(VteTerminal* term, char* text, guint size, gpointer self);
    static void onContentsChanged(VteTerminal* term, gpointer self);
};

#endif // MINICODE_ENABLE_TERMINAL
