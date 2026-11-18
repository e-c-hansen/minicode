// Terminal.cpp — see Terminal.h. Entirely compiled out unless
// MUTIBLE_ENABLE_TERMINAL is set.
#include <climits>
#include <unistd.h>
#include "EnvLock.h"
#include "Terminal.h"
#include <map>

#ifdef MUTIBLE_ENABLE_TERMINAL

#include "Appearance.h"
#include "BackupStore.h"
#include "ClaudeHooks.h"
#include "Rename.h"
#include "Palette.h"
#include "SessionLink.h"
#include "StartupLog.h"
#include "TerminalAttention.h"
#include "TermLinkPath.h"
#include "TermLinks.h"

#include <vte/vte.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <glib/gstdio.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// The 16 ANSI slots, in VTE's order: black, red, green, yellow, blue, magenta,
// cyan, white, then the eight bright variants.
//
// VS Code ships two Dark+ palettes: the saturated one its integrated terminal
// uses (pure #E5E510 yellow, #CD3131 red) and the muted one its *editor* uses
// for syntax. The editor set is the better neighbour here, since the panel sits
// directly under a view already painted in those exact colors — so most of
// these are literally the values in Palette.h. Slots with no syntax equivalent
// are interpolated in the same register rather than borrowed from the harsher
// terminal set.
const char* kAnsiPalette[16] = {
    "#2A2A2A",   // black: the Markdown code background, not pure black
    "#D16969",   // red: Dark+ regex/invalid, a brick red rather than a signal red
    "#6A9955",   // green: the editor's comment green
    "#D7BA7D",   // yellow: Dark+ annotation tan
    "#569CD6",   // blue: the editor's keyword blue
    "#C586C0",   // magenta: the editor's preprocessor magenta
    "#4EC9B0",   // cyan: the editor's type teal
    "#D4D4D4",   // white: the editor's foreground
    "#808080",   // bright black: dim text still has to be readable
    "#E08A8A",   // bright red
    "#B5CEA8",   // bright green: the editor's number green
    "#DCDCAA",   // bright yellow: the editor's function yellow
    "#9CDCFE",   // bright blue: Dark+ variable blue
    "#D7A3D2",   // bright magenta
    "#7FD8C6",   // bright cyan
    "#F0F0F0",   // bright white
};

// The editor asks GTK for "the monospace font" via
// gtk_text_view_set_monospace, which resolves to the monospace family at the
// desktop font size. VTE has no equivalent flag, so build the same description
// by hand — otherwise the terminal renders a size or two off from the editor
// sitting directly above it. Caller owns the result.
PangoFontDescription* systemMonospaceFont() {
    char* uiFont = nullptr;
    g_object_get(gtk_settings_get_default(), "gtk-font-name", &uiFont, nullptr);
    PangoFontDescription* ui =
        pango_font_description_from_string(uiFont ? uiFont : "Sans 11");
    g_free(uiFont);

    PangoFontDescription* mono = pango_font_description_new();
    pango_font_description_set_family(mono, "monospace");
    const gint size = pango_font_description_get_size(ui);
    if (size > 0) {
        if (pango_font_description_get_size_is_absolute(ui))
            pango_font_description_set_absolute_size(mono, size);
        else
            pango_font_description_set_size(mono, size);
    }
    pango_font_description_free(ui);
    return mono;
}

} // namespace

// The panel is an empty overlay until it is first shown: the terminal and its
// shell are made then (makeTerminal), as the Mac makes its terminal dock when
// it is first needed (EditorController+Panels.mm, makeTerminalPanel). A
// window opening with the panel hidden starts no shell (PARITY.md R3).
static std::map<int, Terminal*>& terminalRegistry() {
    static std::map<int, Terminal*> m;
    return m;
}
Terminal* Terminal::byId(int id) {
    auto it = terminalRegistry().find(id);
    return it == terminalRegistry().end() ? nullptr : it->second;
}

Terminal::Terminal(const std::string& cwd, std::shared_ptr<SessionLink::Link> link)
    : link_(std::move(link)), cwd_(cwd) {
    static int nextId = kFirstId;
    controlId_ = nextId++;
    terminalRegistry()[controlId_] = this;
    // Its host going (a headless one, killed): the shell went with it, and
    // nothing starts another until the host can be reached again (stage 6,
    // step 2 reconnects).
    lostListener_ = link_->addLostListener([this](const std::string&) {
        hostGone_ = true;
        const bool had = hostTerm_ != 0;
        hostTerm_ = 0;
        shellPid_ = -1;
        navLine_ = -1;
        if (vte_ && had) feedNotice("[the session's host went away, and this terminal's shell with it]");
    });
    // The scroller sits in an overlay so the hovered link's underline can be
    // drawn over the terminal: VTE underlines only its own regex matches, and
    // a regex cannot know which paths exist.
    root_ = gtk_overlay_new();
    // The waiting mark (setAttention): hidden until a notify marks the terminal.
    attentionDot_ = gtk_label_new(nullptr);
    gtk_widget_set_halign(attentionDot_, GTK_ALIGN_END);
    gtk_widget_set_valign(attentionDot_, GTK_ALIGN_START);
    gtk_widget_set_margin_end(attentionDot_, 14);
    gtk_widget_set_margin_top(attentionDot_, 2);
    gtk_widget_set_visible(attentionDot_, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(root_), attentionDot_);
    // Only the floor: the height is the paned divider's business now, so the
    // panel must not demand a fixed size (that was what made it unresizable).
    gtk_widget_set_size_request(root_, -1, kMinHeight);
    g_signal_connect(root_, "map", G_CALLBACK(+[](GtkWidget*, gpointer p) {
        static_cast<Terminal*>(p)->makeTerminal();
    }), this);
    GtkEventController* fc = focusCtl_ = gtk_event_controller_focus_new();
    g_signal_connect(fc, "enter", G_CALLBACK(+[](GtkEventControllerFocus*, gpointer p) {
        Terminal* t = static_cast<Terminal*>(p);
        if (t->onFocusIn) t->onFocusIn();
    }), this);
    gtk_widget_add_controller(root_, fc);
}

void Terminal::cellSize(double* w, double* h) const {
    *w = 8;
    *h = 17;
    if (!vte_) return;
    const long cw = vte_terminal_get_char_width(VTE_TERMINAL(vte_));
    const long ch = vte_terminal_get_char_height(VTE_TERMINAL(vte_));
    if (cw > 0) *w = (double)cw;
    if (ch > 0) *h = (double)ch;
}

int Terminal::columns() const { return vte_ ? (int)vte_terminal_get_column_count(VTE_TERMINAL(vte_)) : 0; }
int Terminal::rows() const { return vte_ ? (int)vte_terminal_get_row_count(VTE_TERMINAL(vte_)) : 0; }

void Terminal::setAttention(const std::string& markName, const std::string& tooltip) {
    attentionShown_ = markName;
    if (!attentionDot_) return;
    if (markName.empty()) {
        gtk_widget_set_visible(attentionDot_, FALSE);
        return;
    }
    // The Mac's dots: orange for waiting, green for done.
    const char* color = markName == "waiting" ? "#E8A33D" : "#4EC9B0";
    char* markup = g_markup_printf_escaped("<span foreground=\"%s\" size=\"x-large\">\u25CF</span>", color);
    gtk_label_set_markup(GTK_LABEL(attentionDot_), markup);
    g_free(markup);
    gtk_widget_set_tooltip_text(attentionDot_, tooltip.c_str());
    gtk_widget_set_visible(attentionDot_, TRUE);
}

// ------------------------------------------------------ shell integration

namespace {

// A file the app ships: beside the binary (a build folder), else in the
// installed share/mutible; "" when neither has it.
std::string shippedFile(const char* name) {
    char exe[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return "";
    exe[n] = 0;
    char* dir = g_path_get_dirname(exe);
    char* beside = g_build_filename(dir, name, nullptr);
    char* shared = g_build_filename(dir, "..", "share", "mutible", name, nullptr);
    std::string found = g_file_test(beside, G_FILE_TEST_IS_REGULAR) ? beside
                        : g_file_test(shared, G_FILE_TEST_IS_REGULAR) ? shared : "";
    g_free(beside);
    g_free(shared);
    g_free(dir);
    return found;
}

const char* const kIntegrationFiles[] = {ShellIntegration::kZshScriptName, ".zshenv",
                                         ShellIntegration::kBashScriptName};

void removeIntegrationFolder(const std::string& dir) {
    for (const char* f : kIntegrationFiles) g_remove((dir + "/" + f).c_str());
    g_rmdir(dir.c_str());
}

std::string gIntegrationFolder;

}  // namespace

// The scripts are copied once a process, so a shell started later in this
// process never reads a newer install's script (ShellIntegration.h: the
// protocol is the running app's). In the user's runtime folder (0700, the
// user's own), never the shared /tmp, where another user could have made
// the folder first. The first look removes the folders of processes gone;
// leaving removes this one's.
const std::string& Terminal::integrationFolder() {
    static bool tried = false;
    if (tried) return gIntegrationFolder;
    tried = true;
    const std::string base = g_get_user_runtime_dir();
    if (GDir* d = g_dir_open(base.c_str(), 0, nullptr)) {
        while (const char* name = g_dir_read_name(d)) {
            long long pid = 0, start = 0;
            if (!ShellIntegration::parseFolderName(name, &pid, &start)) continue;
            if (ShellIntegration::ownerGone(start, BackupStore::processStart((pid_t)pid)))
                removeIntegrationFolder(base + "/" + name);
        }
        g_dir_close(d);
    }
    const long long start = (long long)BackupStore::processStart(getpid());
    const std::string dir = base + "/" + ShellIntegration::folderName(true, getpid(), start);
    g_mkdir_with_parents(base.c_str(), 0700);
    g_mkdir(dir.c_str(), 0700);
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 077) != 0)
        return gIntegrationFolder;
    bool any = false;
    for (const char* name : {ShellIntegration::kZshScriptName, ShellIntegration::kBashScriptName}) {
        const std::string from = shippedFile(name);
        gchar* text = nullptr;
        gsize len = 0;
        if (from.empty() || !g_file_get_contents(from.c_str(), &text, &len, nullptr)) continue;
        if (g_file_set_contents((dir + "/" + name).c_str(), text, (gssize)len, nullptr)) any = true;
        g_free(text);
    }
    const std::string stub = ShellIntegration::zshenvStub(true);
    g_file_set_contents((dir + "/.zshenv").c_str(), stub.c_str(), (gssize)stub.size(), nullptr);
    if (!any) {
        removeIntegrationFolder(dir);
        return gIntegrationFolder;
    }
    gIntegrationFolder = dir;
    atexit([] { removeIntegrationFolder(gIntegrationFolder); });
    return gIntegrationFolder;
}

// Claude Code starting here: the typed command names it, or a second later,
// with the command still running, the foreground process is its binary.
void Terminal::lookForClaude() {
    if (claudeTimer_) { g_source_remove(claudeTimer_); claudeTimer_ = 0; }
    if (ClaudeHooks::startsClaude(shell_.command)) {
        onClaudeStarted();
        return;
    }
    claudeLookFor_ = shell_.started;
    claudeTimer_ = g_timeout_add(1000, [](gpointer p) -> gboolean {
        Terminal* self = static_cast<Terminal*>(p);
        self->claudeTimer_ = 0;
        if (!self->onClaudeStarted || !self->shell_.busy || self->shell_.started != self->claudeLookFor_ ||
            !self->busy())
            return G_SOURCE_REMOVE;
        const int fg = self->host_.foreground;
        if (fg <= 0) return G_SOURCE_REMOVE;
        char* exe = g_file_read_link(("/proc/" + std::to_string(fg) + "/exe").c_str(), nullptr);
        const bool is = exe && ClaudeHooks::isClaudeProgramPath(exe);
        g_free(exe);
        if (is) self->onClaudeStarted();
        return G_SOURCE_REMOVE;
    }, this);
}

void Terminal::makeTerminal() {
    if (vte_) return;
    StartupLog::mark("terminal: made, on first show");
    listener_.t = this;
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroller, "minicode-terminal");
    gtk_widget_add_css_class(scroller, "minicode-scroller");

    vte_ = vte_terminal_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), vte_);

    underline_ = gtk_drawing_area_new();
    gtk_widget_set_can_target(underline_, FALSE);   // clicks go through to VTE
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(underline_), drawHover, this, nullptr);
    g_signal_connect(underline_, "resize", G_CALLBACK(+[](GtkDrawingArea*, int, int, gpointer p) {
        static_cast<Terminal*>(p)->watchSize();
    }), this);

    applyTheme();
    if (settings_) applyColors(*settings_);   // the app's, told before the terminal existed
    if (fontSet_) applyFont();                // the zoom's (Zoom.h), the same

    VteTerminal* term = VTE_TERMINAL(vte_);
    vte_terminal_set_scrollback_lines(term, 10000);
    vte_terminal_set_scroll_on_keystroke(term, TRUE);
    // Deliberately not scroll-on-output: a long build can then run while you
    // read something further up without yanking the view back down.
    vte_terminal_set_scroll_on_output(term, FALSE);
    vte_terminal_set_audible_bell(term, FALSE);
    vte_terminal_set_mouse_autohide(term, TRUE);
    vte_terminal_set_allow_hyperlink(term, TRUE);
    vte_terminal_set_cursor_blink_mode(term, VTE_CURSOR_BLINK_OFF);

    g_signal_connect(vte_, "window-title-changed", G_CALLBACK(+[](VteTerminal*, gpointer p) {
        Terminal* t = static_cast<Terminal*>(p);
        if (t->onTitleChanged) t->onTitleChanged();
    }), this);
    // Its size is the shell's: VTE takes the rows and columns that fit as it
    // is laid out (and when its font changes), and the host gives them to
    // the pty. The underline's drawing area covers the terminal, so its
    // resize says when.
    g_signal_connect(vte_, "char-size-changed", G_CALLBACK(+[](VteTerminal*, guint, guint, gpointer p) {
        static_cast<Terminal*>(p)->watchSize();
    }), this);
    // Run in Terminal waits for the shell's prompt, and a Return typed here
    // starts the look for the command's end (typeText, onCommandFinished).
    g_signal_connect(vte_, "contents-changed", G_CALLBACK(onContentsChanged), this);
    g_signal_connect(vte_, "commit", G_CALLBACK(onCommit), this);
    // VTE has parsed what it was fed: the host's word on it may go through
    // (vteCaughtUp; termShell and termMarks hold it till then).
    {
        auto caughtUp = +[](VteTerminal*, gpointer p) { static_cast<Terminal*>(p)->vteCaughtUp(); };
        g_signal_connect(vte_, "contents-changed", G_CALLBACK(caughtUp), this);
        g_signal_connect(vte_, "cursor-moved", G_CALLBACK(caughtUp), this);
        // A grid larger than the view (another renderer's size) follows the
        // cursor; nothing is done while it fits.
        auto moved = +[](VteTerminal*, gpointer p) {
            Terminal* self = static_cast<Terminal*>(p);
            if (self->ptyCols_ || self->ptyRows_) self->panToCursor();
        };
        g_signal_connect(vte_, "cursor-moved", G_CALLBACK(moved), this);
    }
    // A key pressed in the terminal: what VTE commits while it is handled
    // is the user's typing, which ends the terminal's mark (onCommit; a
    // key that sends the program nothing, such as Ctrl+Shift+C, does not).
    // Ctrl+Up and Ctrl+Down move between the commands the marks place
    // (VS Code's Linux keys), unless a program has the alternate screen.
    // The controller only watches otherwise: the key goes on to VTE.
    {
        GtkEventController* typed = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(typed, GTK_PHASE_CAPTURE);
        g_signal_connect(typed, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint keyval,
                                                              guint, GdkModifierType mods, gpointer p) -> gboolean {
            Terminal* t = static_cast<Terminal*>(p);
            const GdkModifierType held = (GdkModifierType)(mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK |
                                                                   GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK));
            if (held == GDK_CONTROL_MASK && (keyval == GDK_KEY_Up || keyval == GDK_KEY_Down ||
                                             keyval == GDK_KEY_KP_Up || keyval == GDK_KEY_KP_Down))
                return t->moveToCommand(keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up ? -1 : 1) ? TRUE : FALSE;
            // What the key commits may come through an input method a
            // moment later, so the key counts for a fifth of a second.
            t->userInput_ = true;
            if (t->userInputIdle_) g_source_remove(t->userInputIdle_);
            t->userInputIdle_ = g_timeout_add(200, [](gpointer q) -> gboolean {
                Terminal* self = static_cast<Terminal*>(q);
                self->userInput_ = false;
                self->userInputIdle_ = 0;
                return G_SOURCE_REMOVE;
            }, t);
            return FALSE;
        }), this);
        gtk_widget_add_controller(vte_, typed);
    }
    // The right click: Copy, Paste, Select All, Adjust Settings… (PARITY.md 2.11).
    Appearance::attachTerminalMenu(vte_);

    // Copy and paste, the keys every Linux terminal uses: plain Ctrl+C and
    // Ctrl+V belong to the shell. VTE itself binds neither. A controller on
    // the terminal, so they mean this only while it has the keyboard.
    GtkEventController* keys = gtk_shortcut_controller_new();
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(keys),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_C, (GdkModifierType)(
                             GDK_CONTROL_MASK | GDK_SHIFT_MASK)),
                         gtk_callback_action_new([](GtkWidget* w, GVariant*, gpointer) -> gboolean {
                             vte_terminal_copy_clipboard_format(VTE_TERMINAL(w), VTE_FORMAT_TEXT);
                             return TRUE;
                         }, nullptr, nullptr)));
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(keys),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_V, (GdkModifierType)(
                             GDK_CONTROL_MASK | GDK_SHIFT_MASK)),
                         gtk_callback_action_new([](GtkWidget* w, GVariant*, gpointer p) -> gboolean {
                             // The clipboard comes later, outside the key's
                             // handling: the paste is the user's now.
                             static_cast<Terminal*>(p)->userTyped();
                             vte_terminal_paste_clipboard(VTE_TERMINAL(w));
                             return TRUE;
                         }, this, nullptr)));
    gtk_widget_add_controller(vte_, keys);

    // Ctrl+click on a link. Capture phase, so it is seen before VTE starts a
    // selection or hands the click to a program that tracks the mouse; a
    // click that opens a link is claimed, and VTE never sees it. Without Ctrl,
    // or off a link, the click is left alone.
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick* g, int n,
                                                      double x, double y, gpointer p) {
        Terminal* self = static_cast<Terminal*>(p);
        const GdkModifierType mods =
            gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
        if (!(mods & GDK_CONTROL_MASK)) return;
        if (!self->linkAt(x, y, nullptr)) return;
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
        if (n == 1) self->openLinkAt(x, y);   // a double click opens it once
    }), this);
    gtk_widget_add_controller(vte_, GTK_EVENT_CONTROLLER(click));
    handlers_.push_back(G_OBJECT(click));

    // Where the pointer is, and whether Ctrl is down, for the underline.
    GtkEventController* motion = gtk_event_controller_motion_new();
    auto moved = +[](GtkEventControllerMotion* m, double x, double y, gpointer p) {
        Terminal* self = static_cast<Terminal*>(p);
        self->pointerIn_ = true;
        self->pointerX_ = x;
        self->pointerY_ = y;
        self->ctrlDown_ = gtk_event_controller_get_current_event_state(
                              GTK_EVENT_CONTROLLER(m)) & GDK_CONTROL_MASK;
        self->setHover(self->ctrlDown_);
    };
    g_signal_connect(motion, "enter", G_CALLBACK(moved), this);
    g_signal_connect(motion, "motion", G_CALLBACK(moved), this);
    g_signal_connect(motion, "leave", G_CALLBACK(+[](GtkEventControllerMotion*, gpointer p) {
        Terminal* self = static_cast<Terminal*>(p);
        self->pointerIn_ = false;
        self->setHover(false);
    }), this);
    gtk_widget_add_controller(vte_, motion);
    handlers_.push_back(G_OBJECT(motion));

    // New output, or scrolling, can move text under a pointer that has not
    // moved. Only looked at while Ctrl is down or a link is showing, so
    // ordinary output costs nothing.
    auto recheck = +[](gpointer, gpointer p) {
        Terminal* self = static_cast<Terminal*>(p);
        if (self->ctrlDown_ || !self->hover_.empty()) self->setHover(self->ctrlDown_);
        if (self->navLine_ >= 0) gtk_widget_queue_draw(self->underline_);   // the outline scrolls
    };
    g_signal_connect(vte_, "contents-changed", G_CALLBACK(+[](VteTerminal*, gpointer p) {
        Terminal* self = static_cast<Terminal*>(p);
        self->topKnown_ = false;   // see topRow
        if (self->ctrlDown_ || !self->hover_.empty()) self->setHover(self->ctrlDown_);
    }), this);
    if (GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte_))) {
        g_signal_connect(adj, "value-changed", G_CALLBACK(recheck), this);
        handlers_.push_back(G_OBJECT(adj));
    }

    // Pressing or letting go of Ctrl without moving the mouse shows or hides
    // the underline too. Key events go to the focused widget, which is often
    // the editor, so this listens on the window (the Mac does the same with a
    // local event monitor). The window exists only once the panel is in one.
    g_signal_connect(vte_, "realize", G_CALLBACK(+[](GtkWidget* w, gpointer p) {
        Terminal* self = static_cast<Terminal*>(p);
        GtkWidget* win = GTK_WIDGET(gtk_widget_get_root(w));
        if (!win || win == self->keysWindow_) return;
        GtkEventController* keysCtl = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(keysCtl, GTK_PHASE_CAPTURE);
        auto ctrlKey = +[](GtkEventControllerKey* k, guint keyval, guint, GdkModifierType,
                           gpointer q) -> gboolean {
            Terminal* t = static_cast<Terminal*>(q);
            if (keyval != GDK_KEY_Control_L && keyval != GDK_KEY_Control_R) return FALSE;
            // key-pressed and key-released share the handler; which one ran
            // is in the event itself.
            GdkEvent* ev = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(k));
            t->ctrlDown_ = ev && gdk_event_get_event_type(ev) == GDK_KEY_PRESS;
            t->setHover(t->ctrlDown_);
            return FALSE;   // Ctrl still reaches whoever has the keyboard
        };
        g_signal_connect(keysCtl, "key-pressed", G_CALLBACK(ctrlKey), self);
        g_signal_connect(keysCtl, "key-released", G_CALLBACK(ctrlKey), self);
        gtk_widget_add_controller(win, keysCtl);
        self->keysWindow_ = win;
        self->windowKeys_ = keysCtl;
        g_object_add_weak_pointer(G_OBJECT(win), (gpointer*)&self->keysWindow_);
    }), this);

    // Into the panel last: the panel is on show by now, so the terminal is
    // realized as it goes in, and the handler above must be there for it.
    gtk_overlay_set_child(GTK_OVERLAY(root_), scroller);
    gtk_overlay_add_overlay(GTK_OVERLAY(root_), underline_);
    // A pty larger than the view (another renderer typed last) is drawn
    // larger and clipped here, with no scroll bar of its own
    // (followPtySize).
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
    if (heldTerm_) attachShell();
    else spawnShell();
}

void Terminal::attachHeld(Shared::TermId id, int pid, const Shared::TermShell& shell) {
    heldTerm_ = id;
    heldPid_ = pid;
    shellPid_ = pid;   // for busy(), the processes and the close question meanwhile
    host_ = shell;
}

// The held shell's integration is the host's to read (its marks carry a
// nonce this renderer never saw): its word on the shell is taken as it is,
// and the shell is zsh or bash by its name.
void Terminal::attachShell() {
    const Shared::TermId id = heldTerm_;
    heldTerm_ = 0;
    if (hostGone_ || link_->lost()) {
        hostGone_ = true;
        feedNotice("[the session's host went away: no shell can start here]");
        return;
    }
    std::string comm;
    {
        gchar* raw = nullptr;
        const std::string file = "/proc/" + std::to_string(heldPid_) + "/comm";
        if (g_file_get_contents(file.c_str(), &raw, nullptr, nullptr)) comm = raw;
        g_free(raw);
        while (!comm.empty() && (comm.back() == '\n' || comm.back() == ' ')) comm.pop_back();
    }
    shell_ = ShellIntegration::ShellState();
    const Shared::TermShell held = host_;
    host_ = Shared::TermShell();
    marks_.clear();
    navLine_ = -1;
    hostCursor_ = -1;
    forgetHeld();
    bashShell_ = comm == "bash";
    nonce_ = comm == "zsh" || bashShell_ ? std::string("held") : std::string();
    popOwed_ = 0;
    shellSpoke_ = true;   // at its prompt or running: nothing to wait for
    lastSpawn_ = g_get_monotonic_time();
    hostTerm_ = id;
    shellPid_ = heldPid_;
    sentCols_ = sentRows_ = 0;   // this view's size, sent once attached
    link_->client().attachTerminal(hostTerm_, &listener_);
    applyShell(held);
    watchSize();
}

void Terminal::detachFromHost() {
    if (link_->lost()) return;
    if (hostTerm_) link_->client().detachTerminal(hostTerm_);
    hostTerm_ = 0;
    heldTerm_ = 0;   // never attached here: nothing to let go of
}

void Terminal::rehost(std::shared_ptr<SessionLink::Link> link) {
    if (!link || link == link_) return;
    link_->removeLostListener(lostListener_);
    if (hostTerm_ && !link_->lost()) link_->client().closeTerminal(hostTerm_);
    hostTerm_ = heldTerm_ = 0;
    link_ = std::move(link);
    lostListener_ = link_->addLostListener([this](const std::string&) {
        hostGone_ = true;
        const bool had = hostTerm_ != 0;
        hostTerm_ = 0;
        shellPid_ = -1;
        navLine_ = -1;
        if (vte_ && had) feedNotice("[the session's host went away, and this terminal's shell with it]");
    });
    hostGone_ = false;
    rapidExits_ = 0;
    if (!vte_) return;   // its shell starts when it is first shown
    feedNotice("[the session's host is back: a new shell]");
    spawnShell();
}

bool Terminal::viewCells(int* cols, int* rows) const {
    if (!vte_) return false;
    GtkWidget* scroller = gtk_widget_get_parent(vte_);
    VteTerminal* t = VTE_TERMINAL(vte_);
    const double cw = (double)vte_terminal_get_char_width(t), ch = (double)vte_terminal_get_char_height(t);
    if (!scroller || cw <= 0 || ch <= 0) return false;
    // VTE's padding: what its allocation holds beyond its cells.
    const int padW = std::max(0, gtk_widget_get_width(vte_) - (int)(vte_terminal_get_column_count(t) * cw));
    const int padH = std::max(0, gtk_widget_get_height(vte_) - (int)(vte_terminal_get_row_count(t) * ch));
    *cols = (int)((gtk_widget_get_width(scroller) - padW) / cw);
    *rows = (int)((gtk_widget_get_height(scroller) - padH) / ch);
    return *cols >= 2 && *rows >= 1;
}

// The pty's size as the host says it (TermShell's cols and rows), the Mac's
// rule (docs/notes/session-host.md, "Size"): never reflowing a program's
// screen to this view. Larger than the view: VTE takes the pty's size and
// the scroller clips it (no panning to the cursor yet). Smaller: VTE keeps
// the view's grid, and the program draws in its top left.
void Terminal::followPtySize(int cols, int rows) {
    if (!vte_ || cols < 2 || rows < 1) return;
    // This view's own size on its way (a size it sent), or a terminal not on
    // screen (nothing to draw, and its cells not measured yet): no clipping.
    bool own = !gtk_widget_get_mapped(vte_);
    for (const auto& [c, r] : sentSizes_) own = own || (c == cols && r == rows);
    if (own) {
        if (ptyCols_ || ptyRows_) gtk_widget_set_size_request(vte_, -1, -1);
        ptyCols_ = ptyRows_ = 0;
        applyPan();
        return;
    }
    int vc = 0, vr = 0;
    if (!viewCells(&vc, &vr)) return;
    const bool wider = cols > vc, taller = rows > vr;
    if (own || (!wider && !taller)) {
        if (ptyCols_ || ptyRows_) gtk_widget_set_size_request(vte_, -1, -1);
        ptyCols_ = ptyRows_ = 0;
        applyPan();
        return;
    }
    if (cols == ptyCols_ && rows == ptyRows_) return;
    ptyCols_ = cols;
    ptyRows_ = rows;
    VteTerminal* t = VTE_TERMINAL(vte_);
    const double cw = (double)vte_terminal_get_char_width(t), ch = (double)vte_terminal_get_char_height(t);
    const int padW = std::max(0, gtk_widget_get_width(vte_) - (int)(vte_terminal_get_column_count(t) * cw));
    const int padH = std::max(0, gtk_widget_get_height(vte_) - (int)(vte_terminal_get_row_count(t) * ch));
    gtk_widget_set_size_request(vte_, wider ? (int)(cols * cw) + padW + 1 : -1,
                                taller ? (int)(rows * ch) + padH + 1 : -1);
    // The scroller gives a scrollable child its own size whatever it asks
    // for, so the grid's extra rows and columns are negative margins on the
    // far sides (GTK adds a margin to the allocation's size), and the part
    // on show is chosen by moving the near sides (panToCursor).
    // Counted from VTE's own grid less what it was grown by (the scroller's
    // size in cells leaves out a scroll bar that takes room).
    const int ownCols = (int)vte_terminal_get_column_count(t) - extraCols_;
    const int ownRows = (int)vte_terminal_get_row_count(t) - extraRows_;
    extraCols_ = wider ? std::max(0, cols - ownCols) : 0;
    extraRows_ = taller ? std::max(0, rows - ownRows) : 0;
    applyPan();
    panToCursor();
}

// tmux's pan: the cursor's cell kept in the part of the grid on show, the
// part moved only as far as it takes. The cursor's row is counted from the
// top of the program's screen (VTE's last rows), whatever is scrolled back.
void Terminal::panToCursor() {
    if (!vte_ || (!ptyCols_ && !ptyRows_)) return;
    int vc = 0, vr = 0;
    if (!viewCells(&vc, &vr)) return;
    VteTerminal* t = VTE_TERMINAL(vte_);
    long col = 0, row = 0;
    vte_terminal_get_cursor_position(t, &col, &row);
    const long rowsNow = vte_terminal_get_row_count(t), colsNow = vte_terminal_get_column_count(t);
    long screenTop = 0;
    if (GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte_)))
        screenTop = (long)gtk_adjustment_get_upper(adj) - rowsNow;
    const long r = std::max(0L, row - std::max(0L, screenTop)), c = std::max(0L, col);
    int top = panRow_, left = panCol_;
    if (ptyRows_ && ptyRows_ > vr) {
        if (r < top) top = (int)r;
        else if (r >= top + vr) top = (int)(r - vr + 1);
        top = std::max(0, std::min(top, (int)std::max(0L, std::min<long>(ptyRows_, rowsNow) - vr)));
    } else {
        top = 0;
    }
    if (ptyCols_ && ptyCols_ > vc) {
        if (c < left) left = (int)c;
        else if (c >= left + vc) left = (int)(c - vc + 1);
        left = std::max(0, std::min(left, (int)std::max(0L, std::min<long>(ptyCols_, colsNow) - vc)));
    } else {
        left = 0;
    }
    setPan(top, left);
}

// The grid moved up and left under the scroller, which clips it: negative
// margins, which GTK adds to the allocation's origin and size alike.
void Terminal::setPan(int row, int col) {
    if (row == panRow_ && col == panCol_) return;
    panRow_ = row;
    panCol_ = col;
    applyPan();
}

void Terminal::applyPan() {
    if (!vte_) return;
    if (!ptyCols_ && !ptyRows_) extraCols_ = extraRows_ = panRow_ = panCol_ = 0;
    VteTerminal* t = VTE_TERMINAL(vte_);
    const int cw = (int)vte_terminal_get_char_width(t), ch = (int)vte_terminal_get_char_height(t);
    gtk_widget_set_margin_top(vte_, -panRow_ * ch);
    gtk_widget_set_margin_bottom(vte_, -std::max(0, extraRows_ - panRow_) * ch);
    gtk_widget_set_margin_start(vte_, -panCol_ * cw);
    gtk_widget_set_margin_end(vte_, -std::max(0, extraCols_ - panCol_) * cw);
}

std::string Terminal::title() const {
    if (!userTitle_.empty()) return userTitle_;
    if (vte_) {
        const char* t = vte_terminal_get_window_title(VTE_TERMINAL(vte_));
        if (t && *t) return t;
    }
    if (!host_.title.empty()) return host_.title;   // a held shell's, before VTE heard one
    const char* shell = g_getenv("SHELL");
    if (shell && *shell) {
        const char* slash = strrchr(shell, '/');
        return slash ? slash + 1 : shell;
    }
    return "terminal";
}

void Terminal::setUserTitle(const std::string& t) {
    userTitle_ = t;
    if (onTitleChanged) onTitleChanged();
}

Terminal::~Terminal() {
    terminalRegistry().erase(controlId_);
    TerminalAttention::forget(controlId_);   // the mark goes with it
    // The host hangs the shell up and reaps it (a held one never shown is
    // attached first, so the host takes the close from this renderer).
    if (heldTerm_ && !link_->lost()) {
        link_->client().attachTerminal(heldTerm_, nullptr);
        link_->client().closeTerminal(heldTerm_);
    }
    if (hostTerm_) link_->client().closeTerminal(hostTerm_);
    link_->removeLostListener(lostListener_);
    if (ackIdle_) g_source_remove(ackIdle_);
    if (catchUpTimer_) g_source_remove(catchUpTimer_);
    if (sizeTick_ && vte_) gtk_widget_remove_tick_callback(vte_, sizeTick_);
    if (userInputIdle_) g_source_remove(userInputIdle_);
    g_signal_handlers_disconnect_by_data(root_, this);
    if (focusCtl_) g_signal_handlers_disconnect_by_data(focusCtl_, this);
    if (vte_) g_signal_handlers_disconnect_by_data(vte_, this);
    for (GObject* o : handlers_) g_signal_handlers_disconnect_by_data(o, this);
    if (keysWindow_) {
        gtk_widget_remove_controller(keysWindow_, windowKeys_);
        g_object_remove_weak_pointer(G_OBJECT(keysWindow_), (gpointer*)&keysWindow_);
    }
    if (cursorIdle_) g_source_remove(cursorIdle_);
    if (typeTimer_) g_source_remove(typeTimer_);
    if (claudeTimer_) g_source_remove(claudeTimer_);
    if (finishTimer_) g_source_remove(finishTimer_);
    if (runTimer_) {
        g_source_remove(runTimer_);
        if (runDone_) runDone_("the window was closed");
    }
    if (underline_)
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(underline_), nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------- links

namespace {

// The CSS padding VTE insets its cells by (ThemeCss sets 6px, but a theme or
// a later change could make it anything). GTK 4.10 deprecated the style
// context without a replacement for reading a widget's padding.
GtkBorder cellInset(GtkWidget* vte) {
    GtkBorder b{};
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_style_context_get_padding(gtk_widget_get_style_context(vte), &b);
    G_GNUC_END_IGNORE_DEPRECATIONS
    return b;
}

// Text of the buffer from (r0, c0) up to, not including, (r1, c1). Rows that
// wrapped because they were full come back joined; a real line end is "\n".
std::string textRange(VteTerminal* t, long r0, long c0, long r1, long c1) {
    gsize n = 0;
    char* s = vte_terminal_get_text_range_format(t, VTE_FORMAT_TEXT, r0, c0, r1, c1, &n);
    std::string out = s ? std::string(s, n) : std::string();
    g_free(s);
    return out;
}

void trimEnd(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
}

}  // namespace

// The buffer row at the top of the view, in the numbering that
// vte_terminal_get_text_range_format uses. VTE's vertical adjustment counts
// from the oldest row it still keeps, which is row 0 only until scrollback is
// dropped: `clear` drops it (it sends ESC[3J), and so does output past the
// 10,000 kept lines (taking the adjustment as the row put a click after a
// `clear` rows away from the pointer). VTE has no call that gives the
// offset, so it is found by comparing the text on screen with the buffer's
// rows at each place the screen can start (the cursor is always on the last
// screenful), and kept until the contents change. Fractional while a scroll
// is under way.
double Terminal::topRow() {
    VteTerminal* t = VTE_TERMINAL(vte_);
    GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte_));
    if (!adj) return 0;
    const double value = gtk_adjustment_get_value(adj);
    if (!topKnown_) {
        topKnown_ = true;
        topDelta_ = 0;
        const long rows = vte_terminal_get_row_count(t), cols = vte_terminal_get_column_count(t);
        long ccol = 0, crow = 0;
        vte_terminal_get_cursor_position(t, &ccol, &crow);
        // Where the adjustment stands when scrolled to the end.
        const long bottom = std::lround(gtk_adjustment_get_upper(adj) -
                                        gtk_adjustment_get_page_size(adj));
        const long top = std::lround(value);
        char* v = vte_terminal_get_text_format(t, VTE_FORMAT_TEXT);
        std::string shown = v ? v : "";
        g_free(v);
        trimEnd(shown);
        for (long d = std::max(0L, crow - rows + 1 - bottom); d <= crow - bottom; d++) {
            // VTE's "visible" text runs one row past the screen (for a row
            // partly scrolled into view), which only shows when that row
            // has text, so either reading may be the one that matches.
            std::string screen = textRange(t, top + d, 0, top + d + rows - 1, cols);
            std::string screenPlus = textRange(t, top + d, 0, top + d + rows, cols);
            trimEnd(screen);
            trimEnd(screenPlus);
            if (screen == shown || screenPlus == shown) { topDelta_ = d; break; }
        }
    }
    return value + topDelta_;
}

// The directory the shell is in. VTE records the OSC 7 that Ubuntu's
// /etc/profile.d/vte-2.91.sh has bash print at each prompt; a shell without
// that hook still has a working directory the kernel knows; failing both,
// the folder the shell started in.
std::string Terminal::shellDirectory() const {
    // The integration's OSC 7, which the host believed (it carries the
    // nonce, so VTE takes the nonce for part of the address).
    if (!host_.cwd.empty()) return host_.cwd;
#if VTE_CHECK_VERSION(0, 78, 0)
    if (GUri* uri = vte_terminal_ref_termprop_uri(VTE_TERMINAL(vte_),
                                                  VTE_TERMPROP_CURRENT_DIRECTORY_URI)) {
        char* s = g_uri_to_string(uri);
        g_uri_unref(uri);
        char* host = nullptr;
        char* path = s ? g_filename_from_uri(s, &host, nullptr) : nullptr;
        g_free(s);
        // Another machine's directory (ssh) says nothing about files here.
        const bool local = !host || !*host || g_strcmp0(host, "localhost") == 0 ||
                           g_strcmp0(host, g_get_host_name()) == 0;
        std::string dir = (path && local) ? path : "";
        g_free(host);
        g_free(path);
        if (!dir.empty()) return dir;
    }
#endif
    if (shellPid_ > 0) {
        const std::string link = "/proc/" + std::to_string(shellPid_) + "/cwd";
        if (char* dir = g_file_read_link(link.c_str(), nullptr)) {
            std::string out = dir;
            g_free(dir);
            return out;
        }
    }
    return cwd_;
}

bool Terminal::findLink(double x, double y, Link* out, std::vector<Span>* spans) {
    if (!vte_) return false;   // not shown yet
    VteTerminal* t = VTE_TERMINAL(vte_);
    const long cw = vte_terminal_get_char_width(t), ch = vte_terminal_get_char_height(t);
    const long cols = vte_terminal_get_column_count(t);
    if (cw <= 0 || ch <= 0 || cols <= 0) return false;

    // Which cell: VTE lays its cells out from the padding, in whole cells,
    // and the vertical adjustment's value is the buffer row at the top.
    const GtkBorder pad = cellInset(vte_);
    const double fx = (x - pad.left) / cw, fy = (y - pad.top) / ch;
    if (fx < 0 || fy < 0 || fx >= cols || fy >= vte_terminal_get_row_count(t)) return false;
    const long row = (long)std::floor(topRow() + fy), col = (long)fx;
    if (row < 0) return false;

    // The whole line the cell is on, which may have wrapped over several
    // rows: a compiler's long path in a narrow panel. A row that wrapped
    // joins the next without a "\n" (rows VTE no longer keeps, or never
    // had, read as empty lines). Far enough either way for any real
    // reference, and bounded so a huge wrapped line costs little.
    const long kReach = 16;
    long first = row, last = row;
    while (first > 0 && row - first < kReach) {
        const std::string s = textRange(t, first - 1, 0, first, 0);
        if (s.empty() || s.back() == '\n') break;
        first--;
    }
    while (last - row < kReach) {
        const std::string s = textRange(t, last, 0, last + 1, 0);
        if (s.empty() || s.back() == '\n') break;
        last++;
    }
    std::string line = textRange(t, first, 0, last, cols);
    while (!line.empty() && line.back() == '\n') line.pop_back();
    // Everything before the clicked cell is where it sits in the line.
    const size_t offset = textRange(t, first, 0, row, col).size();
    if (offset >= line.size()) return false;

    const std::vector<TermLinks::Link> links = TermLinks::find(line);
    const TermLinks::Link* l = TermLinks::at(links, offset);
    if (!l) return false;

    Link link;
    link.line = l->line;
    link.column = l->column;
    if (l->kind == TermLinks::Link::Url) {
        link.isUrl = true;
        link.target = l->target;
    } else {
        TermLinkPath::Target found;
        std::string written = l->target;
        // Not in the shell's folder: the copy under the session's own
        // scratchpad or worktree is the one its agent printed.
        if (sessionRoots_ && !written.empty() && written[0] != '/' && written[0] != '~') {
            TermLinkPath::Target here;
            if (!TermLinkPath::resolve(written, shellDirectory(), std::string(), g_get_home_dir(), &here)) {
                for (const std::string& r : sessionRoots_()) {
                    TermLinkPath::Target own;
                    if (TermLinkPath::resolve(written, r, std::string(), g_get_home_dir(), &own)) {
                        written = own.path;
                        break;
                    }
                }
            }
        }
        if (!TermLinkPath::resolve(written, shellDirectory(), root_dir_,
                                   g_get_home_dir(), &found))
            return false;
        link.target = found.path;
        link.isDir = found.isDir;
        link.insideRoot = found.insideRoot;
    }
    if (out) *out = link;
    if (!spans) return true;

    // Back from bytes of the line to cells, for the underline: a wide
    // character takes two, a combining mark none, and a wide character that
    // would straddle the right edge starts the next row instead.
    spans->clear();
    const size_t end = l->start + l->length;
    long r = first, c = 0;
    for (size_t i = 0; i < end && i < line.size();) {
        const char* p = line.c_str() + i;
        gunichar u = g_utf8_get_char_validated(p, (gssize)(line.size() - i));
        size_t n = 1;
        if (u == (gunichar)-1 || u == (gunichar)-2) u = '?';
        else n = (size_t)(g_utf8_next_char(p) - p);
        const long w = u == '\t' ? 8 - c % 8
                     : g_unichar_iszerowidth(u) ? 0 : g_unichar_iswide(u) ? 2 : 1;
        if (c + w > cols) { r++; c = 0; }
        if (i >= l->start && w > 0) {
            if (spans->empty() || spans->back().row != r) spans->push_back({r, c, c + w});
            else spans->back().col1 = c + w;
        }
        c += w;
        i += n;
    }
    return true;
}

bool Terminal::linkAt(double x, double y, Link* out) {
    return findLink(x, y, out, nullptr);
}

bool Terminal::openLinkAt(double x, double y) {
    Link link;
    if (!findLink(x, y, &link, nullptr)) return false;
    hover_.clear();
    gtk_widget_queue_draw(underline_);
    if (onLink_) onLink_(link);
    return true;
}

// Underline the link under the pointer while Ctrl is held, and show the
// pointing hand over it.
void Terminal::setHover(bool ctrl) {
    std::vector<Span> spans;
    if (ctrl && pointerIn_ && gtk_widget_get_mapped(vte_))
        findLink(pointerX_, pointerY_, nullptr, &spans);
    bool same = spans.size() == hover_.size();
    for (size_t i = 0; same && i < spans.size(); i++)
        same = spans[i].row == hover_[i].row && spans[i].col0 == hover_[i].col0 &&
               spans[i].col1 == hover_[i].col1;
    hover_ = std::move(spans);
    if (!same) gtk_widget_queue_draw(underline_);

    // VTE sets its own cursor as the pointer moves, possibly after this
    // handler has run, so the hand goes on once the event is done with.
    const bool hand = !hover_.empty();
    if (!hand && !handCursor_) return;
    handCursor_ = hand;
    if (cursorIdle_) return;
    cursorIdle_ = g_idle_add_full(G_PRIORITY_HIGH_IDLE, [](gpointer p) -> gboolean {
        Terminal* self = static_cast<Terminal*>(p);
        self->cursorIdle_ = 0;
        // "text" is VTE's own pointer over the cells.
        gtk_widget_set_cursor_from_name(self->vte_, self->handCursor_ ? "pointer" : "text");
        return G_SOURCE_REMOVE;
    }, this, nullptr);
}

void Terminal::drawHover(GtkDrawingArea*, cairo_t* cr, int, int, gpointer p) {
    Terminal* self = static_cast<Terminal*>(p);
    if (self->hover_.empty() && self->navLine_ < 0) return;
    VteTerminal* t = VTE_TERMINAL(self->vte_);
    const long cw = vte_terminal_get_char_width(t), ch = vte_terminal_get_char_height(t);
    const double top = self->topRow();
    const GtkBorder pad = cellInset(self->vte_);
    const int height = gtk_widget_get_height(self->vte_);
    // The command moved to (moveToCommand): its prompt to the line before
    // the next prompt, or to the cursor's line, outlined.
    if (self->navLine_ >= 0 && !self->altScreen_) {
        long long last = self->hostCursor_;
        for (const Shared::TermMark& m : self->marks_)
            if (m.prompt > self->navLine_) {
                last = m.prompt - 1;
                break;
            }
        const double y0 = pad.top + ((double)self->rowOfLine(self->navLine_) - top) * ch;
        const double y1 = pad.top + ((double)self->rowOfLine(std::max(last, self->navLine_)) + 1 - top) * ch;
        graphene_point_t a = GRAPHENE_POINT_INIT((float)pad.left, (float)y0), at;
        if (gtk_widget_compute_point(self->vte_, self->underline_, &a, &at)) {
            GdkRGBA c = self->linkColor_;
            c.alpha = 0.55f;
            gdk_cairo_set_source_rgba(cr, &c);
            cairo_set_line_width(cr, 1);
            cairo_rectangle(cr, std::floor(at.x) + 0.5, std::floor(at.y) + 0.5,
                            (double)vte_terminal_get_column_count(t) * cw - 1, std::max(1.0, y1 - y0 - 1));
            cairo_stroke(cr);
        }
    }
    if (self->hover_.empty()) return;
    gdk_cairo_set_source_rgba(cr, &self->linkColor_);
    for (const Span& s : self->hover_) {
        // In the terminal's coordinates, then the overlay's; one pixel, just
        // above the bottom of the cell, where a text underline would go.
        const double y = pad.top + (s.row - top) * ch + ch - 2;
        if (y < 0 || y >= height) continue;
        graphene_point_t in = GRAPHENE_POINT_INIT((float)(pad.left + s.col0 * cw), (float)y);
        graphene_point_t at;
        if (!gtk_widget_compute_point(self->vte_, self->underline_, &in, &at)) continue;
        cairo_rectangle(cr, std::floor(at.x), std::floor(at.y),
                        (double)(s.col1 - s.col0) * cw, 1);
    }
    cairo_fill(cr);
}

bool Terminal::owns(GtkWidget* w) const {
    return w && (w == root_ || gtk_widget_is_ancestor(w, root_));
}

// ---- find (PARITY.md 4.7, third part)

void Terminal::buildFind() {
    if (findBox_) return;
    findBox_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(findBox_, "minicode-terminal-find");
    gtk_widget_add_css_class(findBox_, "osd");
    gtk_widget_set_halign(findBox_, GTK_ALIGN_END);
    gtk_widget_set_valign(findBox_, GTK_ALIGN_START);
    gtk_widget_set_margin_end(findBox_, 28);
    gtk_widget_set_margin_top(findBox_, 4);
    findEntry_ = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(findEntry_), "Find");
    gtk_widget_set_size_request(findEntry_, 200, -1);
    findLabel_ = gtk_label_new("");
    gtk_widget_set_margin_end(findLabel_, 6);
    gtk_box_append(GTK_BOX(findBox_), findEntry_);
    gtk_box_append(GTK_BOX(findBox_), findLabel_);
    gtk_widget_set_visible(findBox_, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(root_), findBox_);
    g_signal_connect(findEntry_, "changed", G_CALLBACK(+[](GtkEditable*, gpointer q) {
        static_cast<Terminal*>(q)->runFind(true, true);
    }), this);
    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint,
                                                         GdkModifierType mods, gpointer q) -> gboolean {
        Terminal* t = static_cast<Terminal*>(q);
        const bool shift = (mods & GDK_SHIFT_MASK) != 0;
        switch (keyval) {
        case GDK_KEY_Escape:
            t->closeFind(true);
            return TRUE;
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
        case GDK_KEY_F3:
            t->findStep(!shift);
            return TRUE;
        default:
            return FALSE;
        }
    }), this);
    gtk_widget_add_controller(findEntry_, keys);
}

static std::string escapeForRegex(const std::string& q) {
    std::string out;
    for (char c : q) {
        if (std::strchr("\\^$.[]|()?*+{}-", c)) out += '\\';
        out += c;
    }
    return out;
}

void Terminal::runFind(bool forward, bool fresh) {
    if (!vte_ || !findEntry_) return;
    VteTerminal* t = VTE_TERMINAL(vte_);
    const std::string q = gtk_editable_get_text(GTK_EDITABLE(findEntry_));
    findMiss_ = false;
    if (q.empty()) {
        vte_terminal_search_set_regex(t, nullptr, 0);
        vte_terminal_unselect_all(t);
    } else {
        if (fresh) {
            // PCRE2_CASELESS | PCRE2_MULTILINE; VTE adds UTF itself.
            GError* err = nullptr;
            VteRegex* re = vte_regex_new_for_search(escapeForRegex(q).c_str(), -1, 0x8u | 0x400u, &err);
            if (err) g_error_free(err);
            vte_terminal_search_set_regex(t, re, 0);
            if (re) vte_regex_unref(re);
            vte_terminal_search_set_wrap_around(t, TRUE);
            vte_terminal_unselect_all(t);
        }
        const gboolean found = forward ? vte_terminal_search_find_next(t)
                                       : vte_terminal_search_find_previous(t);
        if (!found) {
            findMiss_ = true;
            vte_terminal_unselect_all(t);
        }
    }
    gtk_label_set_text(GTK_LABEL(findLabel_), findMiss_ ? "No results" : "");
    if (findMiss_) gtk_widget_add_css_class(findEntry_, "error");
    else gtk_widget_remove_css_class(findEntry_, "error");
}

void Terminal::showFind() {
    if (!vte_) return;
    buildFind();
    gtk_widget_set_visible(findBox_, TRUE);
    // Seeded from a selection in the terminal, as the Mac's bar is.
    if (vte_terminal_get_has_selection(VTE_TERMINAL(vte_))) {
        char* sel = vte_terminal_get_text_selected(VTE_TERMINAL(vte_), VTE_FORMAT_TEXT);
        if (sel && *sel && !strchr(sel, '\n')) gtk_editable_set_text(GTK_EDITABLE(findEntry_), sel);
        g_free(sel);
    }
    gtk_widget_grab_focus(findEntry_);
    gtk_editable_select_region(GTK_EDITABLE(findEntry_), 0, -1);
    if (*gtk_editable_get_text(GTK_EDITABLE(findEntry_))) runFind(true, true);
}

void Terminal::closeFind(bool keyboardBack) {
    if (!findOpen()) return;
    gtk_widget_set_visible(findBox_, FALSE);
    if (vte_) {
        vte_terminal_search_set_regex(VTE_TERMINAL(vte_), nullptr, 0);
        vte_terminal_unselect_all(VTE_TERMINAL(vte_));
        if (keyboardBack) gtk_widget_grab_focus(vte_);
    }
}

bool Terminal::findHasKeyboard() const {
    if (!findBox_ || !root_) return false;
    GtkRoot* r = gtk_widget_get_root(root_);
    GtkWidget* f = r ? gtk_window_get_focus(GTK_WINDOW(r)) : nullptr;
    return f && (f == findBox_ || gtk_widget_is_ancestor(f, findBox_));
}

void Terminal::findStep(bool forward) {
    if (!findOpen()) return;
    runFind(forward, false);
}

std::string Terminal::findStatus() const {
    return findMiss_ ? "No results" : "";
}

// Ctrl+L, which bash's and zsh's line editors take as clear-screen, whenever
// no program holds the terminal's foreground (busy(), the host's tcgetpgrp). Not
// the integration's marks: a mark that comes in one read with output is lost
// (HANDOFF 48), and on CI that left Clear refusing at an idle prompt; and a
// shell without the integration has a prompt to clear too.
bool Terminal::clearScreen() {
    if (!vte_ || busy()) return false;
    scrollToEnd();
    vte_terminal_feed_child(VTE_TERMINAL(vte_), "\x0c", 1);
    return true;
}

// What the app feeds VTE is no key press, so scroll-on-keystroke never moves
// the view, and with scroll-on-output off a view left above the end (a find's
// match, the user reading back) stayed there: the command ran or the screen
// cleared below it and none of it showed (CI's terminal_find, HANDOFF 60).
// At the end, VTE follows the output by itself.
void Terminal::scrollToEnd() {
    if (!vte_) return;
    GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte_));
    if (!adj) return;
    gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj));
}

// Colors and font. Everything here mirrors the macOS terminal panel or the
// editor next to it; nothing is left at a VTE default.
void Terminal::applyTheme() {
    VteTerminal* term = VTE_TERMINAL(vte_);

    GdkRGBA cursor, highlight;
    gdk_rgba_parse(&cursor,    pal::TermCursor);
    gdk_rgba_parse(&highlight, pal::Selection);   // same blue as editor selection
    for (int i = 0; i < 16; ++i) gdk_rgba_parse(&palette_[i], kAnsiPalette[i]);

    applyColors(Settings::parse(""));   // the defaults until the app applies its own
    vte_terminal_set_color_cursor(term, &cursor);
    vte_terminal_set_color_highlight(term, &highlight);
    // Bold text picks the bright slot instead of only thickening, which is what
    // makes the usual colored shell prompts look right.
    vte_terminal_set_bold_is_bright(term, TRUE);

    PangoFontDescription* font = systemMonospaceFont();
    vte_terminal_set_font(term, font);
    pango_font_description_free(font);
}

void Terminal::setFontSize(double basePoints, double scale) {
    fontBase_ = basePoints;
    fontScale_ = scale;
    fontSet_ = true;
    if (vte_) applyFont();   // else when it is made
}

void Terminal::applyFont() {
    VteTerminal* term = VTE_TERMINAL(vte_);
    const double scale = fontScale_;
    PangoFontDescription* font = systemMonospaceFont();
    if (fontBase_ > 0)
        pango_font_description_set_size(font, (gint)std::lround(fontBase_ * PANGO_SCALE));
    const PangoFontDescription* now = vte_terminal_get_font(term);
    if (!now || !pango_font_description_equal(now, font)) vte_terminal_set_font(term, font);
    pango_font_description_free(font);
    if (vte_terminal_get_font_scale(term) != scale) vte_terminal_set_font_scale(term, scale);
}

void Terminal::applySettings(const Settings& s) {
    settings_ = std::make_unique<Settings>(s);   // for a terminal not made yet
    if (vte_) applyColors(s);
}

void Terminal::applyColors(const Settings& s) {
    auto gdk = [](const Rgba& c) {
        return GdkRGBA{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (float)c.a};
    };
    const Rgba bgColor = s.background(Surface::Terminal);
    GdkRGBA fg = gdk(s.text(Surface::Terminal));
    GdkRGBA bg = gdk(bgColor);
    // The block cursor's text is the background color, solid so it stays legible.
    GdkRGBA cursorFg = gdk(Rgba::hex(bgColor.rgb()));
    vte_terminal_set_colors(VTE_TERMINAL(vte_), &fg, &bg, palette_, 16);
    vte_terminal_set_color_cursor_foreground(VTE_TERMINAL(vte_), &cursorFg);
    linkColor_ = fg;   // the underline under a hovered link
}

// Write a status line of our own into the terminal, in the palette's green —
// the same color and register the macOS panel uses for these
// (Terminal.mm, THex(0x6A9955), which is now ANSI slot 2).
void Terminal::feedNotice(const std::string& text) {
    const std::string line = "\r\n\033[32m" + text + "\033[0m\r\n";
    vte_terminal_feed(VTE_TERMINAL(vte_), line.c_str(), (gssize)line.size());
}

// The shell exiting used to leave a dead rectangle until the app was
// restarted: Ctrl+D and the panel was finished. The macOS build restarts it
// (Terminal.mm -startShell), so this does too. `error`: it never started.
void Terminal::shellExited(const std::string& error) {
    shellPid_ = -1;
    hostTerm_ = 0;
    navLine_ = -1;
    if (!error.empty()) {
        // Without this the panel just sits there blank and there is nothing
        // to tell you why.
        feedNotice("[could not start the shell: " + error + "]");
        return;
    }

    // A shell that dies the instant it starts — $SHELL uninstalled, a profile
    // that exits, a bad interpreter — would otherwise respawn in a tight loop
    // and peg a core. Three fast deaths in a row is taken as "not coming back".
    const gint64 now = g_get_monotonic_time();
    if (now - lastSpawn_ < 1000000) {   // under a second counts as fast
        if (++rapidExits_ >= 3) {
            feedNotice("[shell keeps exiting immediately — not restarting]");
            return;
        }
    } else {
        rapidExits_ = 0;
    }

    feedNotice("[shell exited — restarting]");
    spawnShell();
}

// ---- the host's side of the terminal (SessionLink's client, on the main loop)

void Terminal::HostListener::termOpened(Shared::TermId id, int pid) {
    if (id != t->hostTerm_) return;
    t->shellPid_ = pid;   // for its directory, the processes, busy()
    StartupLog::once("shell", "terminal: shell started (pid %d)", pid);
}

// The shell's bytes, for VTE to draw; acknowledged once the main loop is
// idle (VTE parses what it is fed in its own time), so a program printing
// without pause is held back by the host rather than queued here.
void Terminal::HostListener::termOutput(Shared::TermId id, const std::string& bytes, long long cursorLine,
                                        bool alt, bool) {
    if (id != t->hostTerm_ || !t->vte_) return;
    vte_terminal_feed(VTE_TERMINAL(t->vte_), bytes.data(), (gssize)bytes.size());
    // The cursor line and the alternate screen are VTE's once it has parsed
    // these bytes (vteCaughtUp).
    t->fedCursor_ = cursorLine;
    t->fedAlt_ = alt;
    t->vteBehind_ = true;
    if (!t->catchUpTimer_)
        t->catchUpTimer_ = g_timeout_add(200, [](gpointer q) -> gboolean {
            Terminal* self = static_cast<Terminal*>(q);
            self->catchUpTimer_ = 0;
            self->vteCaughtUp();
            return G_SOURCE_REMOVE;
        }, t);
    t->shellSpoke_ = true;
    t->unacked_ += bytes.size();
    if (!t->ackIdle_)
        t->ackIdle_ = g_idle_add_full(G_PRIORITY_LOW, [](gpointer q) -> gboolean {
            Terminal* self = static_cast<Terminal*>(q);
            self->ackIdle_ = 0;
            if (self->hostTerm_ && self->unacked_) self->link_->client().ackTerminal(self->hostTerm_, self->unacked_);
            self->unacked_ = 0;
            return G_SOURCE_REMOVE;
        }, t, nullptr);
    if (t->navLine_ >= 0) gtk_widget_queue_draw(t->underline_);
}

// What the host read of the shell from its marks (and the pty's foreground
// group, which busy() reads). The counters, not the busy flag, say a
// command ended: a short one's start and end come in one message.
void Terminal::HostListener::termShell(Shared::TermId id, const Shared::TermShell& s) {
    if (id != t->hostTerm_) return;
    if (t->vteBehind_) {   // after the bytes before it are on VTE's screen
        t->heldShell_ = s;
        t->heldShellSet_ = true;
        return;
    }
    t->applyShell(s);
}

void Terminal::applyShell(const Shared::TermShell& s) {
    Terminal* t = this;
    t->host_ = s;
    followPtySize(s.cols, s.rows);
    if (t->nonce_.empty() || !s.known) return;
    ShellIntegration::ShellState& sh = t->shell_;
    const bool changed = !sh.known || sh.prompts != s.prompts || sh.started != s.started || sh.done != s.done ||
                         sh.busy != s.busy || sh.status != s.status || sh.command != s.command;
    if (!changed) return;   // the foreground group or the tty's modes only
    const bool ended = sh.known && s.done > sh.done;
    const bool began = s.started > s.done && (!sh.known || s.started > sh.started);
    sh.known = true;
    sh.prompts = s.prompts;
    sh.started = s.started;
    sh.done = s.done;
    sh.busy = s.busy;
    sh.atPrompt = s.atPrompt;
    if (ended) finished_ = sh.command;   // the line seen with its start, if it was
    sh.command = s.busy ? s.command : std::string();
    sh.status = s.status;
    if (began && t->onClaudeStarted) t->lookForClaude();
    if (t->onShellStateChanged) t->onShellStateChanged();
    // The finishing mark ends the command: no polling for an integrated shell.
    t->giveLineBack();
    if (ended && !sh.busy && t->onCommandFinished) t->onCommandFinished();
}

void Terminal::HostListener::termMarks(Shared::TermId id, const std::vector<Shared::TermMark>& m) {
    if (id != t->hostTerm_) return;
    if (t->vteBehind_) {
        t->heldMarks_ = m;
        t->heldMarksSet_ = true;
        return;
    }
    t->applyMarks(m);
}

// VTE has parsed what it was fed (or the fallback timer says it changed
// nothing VTE reports): the host's cursor line, then what it said about the
// marks and the shell, in the order it sent them.
void Terminal::vteCaughtUp() {
    if (!vteBehind_) return;
    vteBehind_ = false;
    if (catchUpTimer_) {
        g_source_remove(catchUpTimer_);
        catchUpTimer_ = 0;
    }
    hostCursor_ = fedCursor_;
    altScreen_ = fedAlt_;
    if (heldMarksSet_) {
        heldMarksSet_ = false;
        applyMarks(heldMarks_);
        heldMarks_.clear();
    }
    if (heldShellSet_) {
        heldShellSet_ = false;
        applyShell(heldShell_);
    }
    if (navLine_ >= 0 && underline_) gtk_widget_queue_draw(underline_);
}

void Terminal::forgetHeld() {
    vteBehind_ = false;
    heldShellSet_ = heldMarksSet_ = false;
    heldMarks_.clear();
    fedCursor_ = -1;
    fedAlt_ = false;
    if (catchUpTimer_) {
        g_source_remove(catchUpTimer_);
        catchUpTimer_ = 0;
    }
}

void Terminal::applyMarks(const std::vector<Shared::TermMark>& m) {
    Terminal* t = this;
    t->marks_ = m;
    if (t->navLine_ >= 0) {
        bool still = false;
        for (const Shared::TermMark& k : m) still = still || k.prompt == t->navLine_;
        if (!still) t->navLine_ = -1;   // its lines left the scrollback, or were cleared
        gtk_widget_queue_draw(t->underline_);
    }
}

void Terminal::HostListener::termExited(Shared::TermId id, int, const std::string& error) {
    if (id != t->hostTerm_) return;
    t->vteCaughtUp();   // what the host said before the exit, in its order
    t->shellExited(error);
}

void Terminal::sendSize() {
    if (!vte_ || !hostTerm_) return;
    VteTerminal* t = VTE_TERMINAL(vte_);
    int cols = (int)vte_terminal_get_column_count(t), rows = (int)vte_terminal_get_row_count(t);
    // VTE held at a larger pty's size: the view's own size is the scroller's.
    if ((ptyCols_ || ptyRows_) && !viewCells(&cols, &rows)) return;
    if (cols < 2 || rows < 1 || (cols == sentCols_ && rows == sentRows_)) return;
    sentCols_ = cols;
    sentRows_ = rows;
    sentSizes_.push_back({cols, rows});
    if (sentSizes_.size() > 4) sentSizes_.erase(sentSizes_.begin());
    link_->client().resizeTerminal(hostTerm_, cols, rows);
    if (navLine_ >= 0) gtk_widget_queue_draw(underline_);
}

// VTE takes its new rows and columns when it is next laid out (after its
// widget or its font changed): looked at in each of the next ten frames.
void Terminal::watchSize() {
    sendSize();
    sizeTicks_ = 10;
    if (sizeTick_ || !vte_) return;
    sizeTick_ = gtk_widget_add_tick_callback(vte_, [](GtkWidget*, GdkFrameClock*, gpointer p) -> gboolean {
        Terminal* t = static_cast<Terminal*>(p);
        t->sendSize();
        if (--t->sizeTicks_ > 0) return G_SOURCE_CONTINUE;
        t->sizeTick_ = 0;
        return G_SOURCE_REMOVE;
    }, this, nullptr);
}

// The user typed or pasted into the terminal: its mark goes, and so does
// the outline of a command moved to.
void Terminal::userTyped() {
    if (controlId_ > 0) TerminalAttention::typed(controlId_);
    if (navLine_ >= 0) {
        navLine_ = -1;
        gtk_widget_queue_draw(underline_);
    }
}

// ---- command marks

int Terminal::commandsRun() const {
    int n = 0;
    for (const Shared::TermMark& m : marks_) n += m.output >= 0;
    return n;
}

// The host's lines and VTE's rows both count every line from the first, so
// one is the other plus what the cursor's line says between them.
long Terminal::rowOfLine(long long hostLine) const {
    if (!vte_ || hostCursor_ < 0) return (long)hostLine;
    long col = 0, row = 0;
    vte_terminal_get_cursor_position(VTE_TERMINAL(vte_), &col, &row);
    return (long)(hostLine - hostCursor_) + row;
}

bool Terminal::moveToCommand(int direction) {
    if (!vte_ || altScreen_) return false;
    const long long from = navLine_ >= 0 ? navLine_ : LLONG_MAX;
    long long target = -1;
    for (const Shared::TermMark& m : marks_) {
        if (m.output < 0) continue;   // a prompt still waiting: no command
        if (direction < 0 && m.prompt < from) target = std::max(target, m.prompt);
        if (direction > 0 && from != LLONG_MAX && m.prompt > from && (target < 0 || m.prompt < target))
            target = m.prompt;
    }
    if (target < 0 && direction < 0) {
        gtk_widget_error_bell(vte_);   // none above
        return true;
    }
    if (target < 0) {   // past the newest: the end, as before
        navLine_ = -1;
        scrollToEnd();
        gtk_widget_queue_draw(underline_);
        return true;
    }
    navLine_ = target;
    // Its prompt at the top, as far as there is room.
    GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte_));
    if (adj) {
        topRow();   // the adjustment's offset from VTE's rows (topDelta_)
        double value = (double)rowOfLine(target) - (double)topDelta_;
        value = std::max(gtk_adjustment_get_lower(adj),
                         std::min(value, gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj)));
        gtk_adjustment_set_value(adj, value);
    }
    gtk_widget_queue_draw(underline_);
    return true;
}

void Terminal::spawnShell() {
    if (hostGone_ || link_->lost()) {
        hostGone_ = true;
        feedNotice("[the session's host went away: no shell can start here]");
        return;
    }
    shellSpoke_ = false;   // a text to type waits for this shell's prompt
    std::string shellText;
    {
        EnvLock::EnvRead env;
        if (const char* v = g_getenv("SHELL")) shellText = v;
    }
    const char* shell = shellText.empty() ? "/bin/bash" : shellText.c_str();

    lastSpawn_ = g_get_monotonic_time();

    // The environment the app has, less any MUTIBLE_TERMINAL_ID and
    // MUTIBLE_PID it inherited (from a mutible this one was started in),
    // plus this terminal's (PARITY.md 3.6): `mutible notify --from-hook`
    // finds its terminal by them. Then what the window adds, such as the IDE
    // link's CLAUDE_CODE_SSE_PORT (PARITY.md 3.9).
    gchar** envv;
    {
        EnvLock::EnvRead env;
        envv = g_get_environ();
    }
    // Each under its name before 1.6.0 too (src/Rename.h), as a minicode's
    // terminal that started this app would have left them.
    for (const char* inherited : {"MUTIBLE_TERMINAL_ID", "MUTIBLE_PID", "MUTIBLE_NONCE",
                                  "MUTIBLE_USER_ZDOTDIR", "MUTIBLE_EXEC"}) {
        envv = g_environ_unsetenv(envv, inherited);
        envv = g_environ_unsetenv(envv, Rename::oldEnvName(inherited).c_str());
    }
    if (controlId_ > 0)
        envv = g_environ_setenv(envv, "MUTIBLE_TERMINAL_ID", std::to_string(controlId_).c_str(), TRUE);
    envv = g_environ_setenv(envv, "MUTIBLE_PID", std::to_string((int)getpid()).c_str(), TRUE);
    // BROWSER: programs opening a web page open it here (PARITY.md 3.7). The
    // script beside the binary (a build folder) or in the installed share
    // folder, and the app's own binary for it to run.
    {
        char exe[PATH_MAX];
        const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = 0;
            char* dir = g_path_get_dirname(exe);
            char* beside = g_build_filename(dir, "mutible-browser.js", nullptr);
            char* shared = g_build_filename(dir, "..", "share", "mutible", "mutible-browser.js", nullptr);
            char* app = g_build_filename(dir, "mutible", nullptr);
            const char* script = g_file_test(beside, G_FILE_TEST_IS_REGULAR) ? beside
                                 : g_file_test(shared, G_FILE_TEST_IS_REGULAR) ? shared : nullptr;
            if (script) envv = g_environ_setenv(envv, "BROWSER", script, TRUE);
            envv = g_environ_setenv(envv, "MUTIBLE_EXE",
                                    g_file_test(app, G_FILE_TEST_IS_EXECUTABLE) ? app : exe, TRUE);
            g_free(beside);
            g_free(shared);
            g_free(app);
            g_free(dir);
        }
    }
    for (const std::string& kv : extraEnvironment ? extraEnvironment() : std::vector<std::string>()) {
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) envv = g_environ_setenv(envv, kv.substr(0, eq).c_str(), kv.c_str() + eq + 1, TRUE);
    }

    // The shell integration (PARITY.md 3.1, D10): bash and zsh get the
    // script and a nonce of their own; any other shell runs as it is and
    // stays unknown. bash: `bash --rcfile <script>`, a non-login shell as
    // before, the script reading ~/.bashrc after bash's own
    // /etc/bash.bashrc. zsh: ZDOTDIR at the folder, whose .zshenv hands the
    // user's ZDOTDIR back (MUTIBLE_USER_ZDOTDIR when they had one).
    shell_ = ShellIntegration::ShellState();
    host_ = Shared::TermShell();
    marks_.clear();
    navLine_ = -1;
    hostCursor_ = -1;
    forgetHeld();   // the old shell's word, held for bytes it fed
    nonce_.clear();
    bashShell_ = false;
    popOwed_ = 0;
    std::vector<std::string> args = {shell};
    {
        char* baseName = g_path_get_basename(shell);
        const std::string name = baseName;
        g_free(baseName);
        const std::string& dir = integrationFolder();
        const bool bash = name == "bash" && !dir.empty() &&
                          g_file_test((dir + "/" + ShellIntegration::kBashScriptName).c_str(), G_FILE_TEST_IS_REGULAR);
        const bool zsh = name == "zsh" && !dir.empty() &&
                         g_file_test((dir + "/" + ShellIntegration::kZshScriptName).c_str(), G_FILE_TEST_IS_REGULAR);
        unsigned char raw[16];
        if ((bash || zsh) && getrandom(raw, sizeof raw, 0) == (ssize_t)sizeof raw) {
            nonce_ = ShellIntegration::nonceHex(raw, sizeof raw);
            envv = g_environ_setenv(envv, "MUTIBLE_NONCE", nonce_.c_str(), TRUE);
            bashShell_ = bash;
            if (bash) {
                args.push_back("--rcfile");
                args.push_back(dir + "/" + ShellIntegration::kBashScriptName);
            } else {
                if (const char* user = g_environ_getenv(envv, "ZDOTDIR")) {
                    const std::string theirs = user;   // envv changes below
                    envv = g_environ_setenv(envv, "MUTIBLE_USER_ZDOTDIR", theirs.c_str(), TRUE);
                }
                envv = g_environ_setenv(envv, "ZDOTDIR", dir.c_str(), TRUE);
            }
        }
    }
    // What VTE's own spawn added: the terminal it is, and its version for
    // the distributions' vte.sh.
    envv = g_environ_setenv(envv, "TERM", "xterm-256color", TRUE);
    envv = g_environ_setenv(envv, "COLORTERM", "truecolor", TRUE);
    envv = g_environ_setenv(envv, "VTE_VERSION",
                            std::to_string(vte_get_major_version() * 10000 + vte_get_minor_version() * 100 +
                                           vte_get_micro_version()).c_str(),
                            TRUE);

    // The host starts it, on a pty of its own, the size VTE has now (it
    // says when that changes, sendSize).
    Shared::TermSpec spec;
    spec.program = shell;
    spec.argv = args;
    for (gchar** e = envv; e && *e; e++) spec.env.push_back(*e);
    g_strfreev(envv);
    if (!cwd_.empty()) {
        spec.cwd = cwd_;
    } else {
        gchar* here = g_get_current_dir();
        spec.cwd = here;
        g_free(here);
    }
    VteTerminal* t = VTE_TERMINAL(vte_);
    spec.cols = std::max(2, (int)vte_terminal_get_column_count(t));
    spec.rows = std::max(1, (int)vte_terminal_get_row_count(t));
    spec.scrollback = (int)vte_terminal_get_scrollback_lines(t);
    spec.nonce = nonce_;
    hostTerm_ = link_->client().newId();   // from the client's range on the host
    sentCols_ = spec.cols;
    sentRows_ = spec.rows;
    sentSizes_ = {{spec.cols, spec.rows}};
    shellPid_ = -1;
    link_->client().openTerminal(hostTerm_, spec, &listener_);
}

void Terminal::focus() {
    makeTerminal();
    gtk_widget_grab_focus(vte_);
}

// ---------------------------------------------------- typing for the user

// The shell leads its own session and process group (the host starts it
// so); a command it runs gets the foreground group of its own. The host
// reads the pty's foreground group after each output and every 200 ms, so
// a program just started may not have it yet: a command the integration's
// marks say is running counts too (bash prints its start mark before it
// forks the command).
bool Terminal::busy() const {
    if (!vte_ || shellPid_ <= 0) return false;
    if (host_.foreground > 0 && host_.foreground != shellPid_) return true;
    return shell_.known && shell_.busy;
}

int Terminal::foregroundJob() const {
    return busy() && host_.foreground != shellPid_ ? host_.foreground : 0;
}

bool Terminal::typeText(const std::string& text) {
    makeTerminal();
    if (busy()) return false;
    pendingType_ = text;
    typeTries_ = 0;
    typePending();
    if (!pendingType_.empty() && !typeTimer_)
        typeTimer_ = g_timeout_add(50, [](gpointer p) -> gboolean {
            Terminal* self = static_cast<Terminal*>(p);
            self->typePending();
            if (!self->pendingType_.empty()) return G_SOURCE_CONTINUE;
            self->typeTimer_ = 0;
            return G_SOURCE_REMOVE;
        }, this);
    return true;
}

// Typed once the shell has started and printed (its prompt), else bytes
// sent while it reads its startup files would echo before the prompt. Ten
// seconds at most: a shell that prints nothing gets the text then.
void Terminal::typePending() {
    if (pendingType_.empty() || !vte_) return;
    const bool ready = shellPid_ > 0 && shellSpoke_;
    if (!ready && ++typeTries_ < 200) return;
    if (busy()) {   // a program took the terminal meanwhile: never type into it
        pendingType_.clear();
        return;
    }
    scrollToEnd();
    setLineAside();
    // A paste, so a shell that asked for brackets (bash, zsh) shows it as
    // pasted and Return is the user's to press.
    vte_terminal_paste_text(VTE_TERMINAL(vte_), pendingType_.c_str());
    pendingType_.clear();
}

// Only to a shell the integration is in and that its marks say is at a
// prompt (never into a program, where the bytes would be input); one that
// has not drawn its first prompt has an empty line.
void Terminal::setLineAside() {
    if (!vte_ || nonce_.empty() || !shell_.known || !shell_.atPrompt || busy()) return;
    vte_terminal_feed_child(VTE_TERMINAL(vte_), "\033[9999~", -1);
    if (bashShell_) {
        if (popOwed_ == 0) pushPrompts_ = shell_.prompts;
        ++popOwed_;
    }
}

void Terminal::giveLineBack() {
    if (popOwed_ <= 0 || !vte_ || !shell_.atPrompt || shell_.prompts <= pushPrompts_) return;
    if (busy()) return;
    for (; popOwed_ > 0; --popOwed_)
        vte_terminal_feed_child(VTE_TERMINAL(vte_), "\033[9998~", -1);
}

std::string Terminal::runBlocker(bool* wait) const {
    if (wait) *wait = false;
    auto soft = [&](const char* why) {
        if (wait) *wait = true;
        return std::string(why);
    };
    if (!vte_ || shellPid_ <= 0) return soft("the terminal's shell is starting");
    if (nonce_.empty())
        return "the terminal's shell has no integration (another shell than bash or zsh, or one started without it), so nobody can tell whether it is at a prompt";
    if (!shell_.known) {
        if (g_get_monotonic_time() - lastSpawn_ < 15 * G_USEC_PER_SEC)
            return soft("the shell has not shown its first prompt yet");
        return "the terminal's shell has not reported a prompt (its integration did not load, or an exec'd program or ssh has the terminal)";
    }
    if (shell_.busy)
        return shell_.command.empty() ? std::string("the terminal is busy running a command")
                                      : "the terminal is busy running: " + shell_.command;
    if (runStarted_ >= 0 && shell_.started == runStarted_ &&
        g_get_monotonic_time() - runTypedAt_ < 2 * G_USEC_PER_SEC)
        return soft("a command was just typed and has not started");
    if (busy()) return soft("a program other than the shell has the terminal");
    if (!shell_.atPrompt) return "the shell is not at a prompt";
    return "";
}

void Terminal::runLine(const std::string& line, std::function<void(const std::string&)> done) {
    makeTerminal();
    if (line.empty() || line.find_first_of("\r\n") != std::string::npos) {
        done("the command must be one line");
        return;
    }
    if (runTimer_) {
        done("another command is already waiting to be typed into the terminal");
        return;
    }
    auto attempt = [this, line](gint64 started, std::string* why) -> int {   // 0 typed, 1 wait, 2 refused
        bool wait = false;
        *why = runBlocker(&wait);
        if (!why->empty()) {
            // A foreground other than the shell's is waited for half a second only.
            const bool foreign = why->find("other than the shell") != std::string::npos;
            const gint64 limit = foreign ? 500000 : (why->find("just typed") != std::string::npos ? 2000000 : 15000000);
            if (wait && g_get_monotonic_time() - started < limit) return 1;
            return 2;
        }
        VteTerminal* t = VTE_TERMINAL(vte_);
        scrollToEnd();
        setLineAside();
        vte_terminal_paste_text(t, line.c_str());
        vte_terminal_feed_child(t, "\r", 1);
        runStarted_ = shell_.started;
        runTypedAt_ = g_get_monotonic_time();
        return 0;
    };
    std::string why;
    const gint64 started = g_get_monotonic_time();
    const int first = attempt(started, &why);
    if (first == 0) { done(""); return; }
    if (first == 2) { done(why); return; }
    runAttempt_ = attempt;
    runDone_ = std::move(done);
    runBegan_ = started;
    runTimer_ = g_timeout_add(50, [](gpointer p) -> gboolean {
        Terminal* self = static_cast<Terminal*>(p);
        std::string why;
        const int r = self->runAttempt_(self->runBegan_, &why);
        if (r == 1) return G_SOURCE_CONTINUE;
        self->runTimer_ = 0;
        auto finish = std::move(self->runDone_);
        self->runDone_ = nullptr;
        finish(r == 0 ? std::string() : why);
        return G_SOURCE_REMOVE;
    }, this);
}

std::string Terminal::recentText(int lines) {
    if (!vte_ || lines < 1) return "";
    VteTerminal* t = VTE_TERMINAL(vte_);
    GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte_));
    if (!adj) return "";
    const long lower = std::lround(gtk_adjustment_get_lower(adj));
    const long upper = std::lround(gtk_adjustment_get_upper(adj));
    // Wrapped rows are one line of text; reading a margin of rows first and
    // cutting to the last `lines` lines keeps a wrapped one whole.
    const long start = std::max(lower, upper - (long)lines - 200);
    gsize len = 0;
    // The end column is exclusive on the last row (VTE's get_text): -1 there
    // left out the newest row, the cursor's, so `term-output` lost the line
    // a command printed without a newline and the prompt after the output.
    char* raw = vte_terminal_get_text_range_format(t, VTE_FORMAT_TEXT, start, 0, upper - 1,
                                                   vte_terminal_get_column_count(t), &len);
    std::string text = raw ? std::string(raw, len) : std::string();
    g_free(raw);
    trimEnd(text);
    size_t pos = text.size();
    int n = 0;
    while (pos > 0 && n < lines) {
        const size_t nl = text.rfind('\n', pos - 1);
        if (nl == std::string::npos) { pos = 0; break; }
        pos = nl;
        n++;
    }
    if (n == lines && pos < text.size() && text[pos] == '\n') pos++;
    return text.substr(pos);
}

void Terminal::onContentsChanged(VteTerminal*, gpointer selfp) {
    static_cast<Terminal*>(selfp)->shellSpoke_ = true;
}

// Everything VTE would write to a pty of its own: the user's keys and
// pastes, the app's typing (feed_child, paste_text), and VTE's answers to a
// program's queries. All of it goes to the host's pty, in order.
void Terminal::onCommit(VteTerminal*, char* text, guint size, gpointer selfp) {
    Terminal* self = static_cast<Terminal*>(selfp);
    if (!text || !size) return;
    // VTE's size first: a key typed right after a resize (a zoom) is read by
    // a program that asks for the width at once.
    self->sendSize();
    // VTE's answers to a program's queries (DA, DSR, OSC colors) are not
    // typing, so they leave the pty's size where it is (Client::
    // terminalInput's `answer`): what comes with no key pressed and reads
    // as one.
    std::string bytes(text, size);
    const bool answer = !self->userInput_ && Shared::termLooksLikeAnswer(bytes);
    if (self->hostTerm_) self->link_->client().terminalInput(self->hostTerm_, std::move(bytes), answer);
    if (self->userInput_) self->userTyped();
    if (!memchr(text, '\r', size) && !memchr(text, '\n', size)) return;
    if (self->finishTimer_) return;   // already looking
    if (!self->nonce_.empty()) return;   // the integration's marks tell (termShell)
    self->finishTimer_ = g_timeout_add(250, [](gpointer p) -> gboolean {
        Terminal* t = static_cast<Terminal*>(p);
        if (t->busy()) return G_SOURCE_CONTINUE;
        t->finishTimer_ = 0;
        if (t->onCommandFinished) t->onCommandFinished();
        return G_SOURCE_REMOVE;
    }, self);
}

#endif // MUTIBLE_ENABLE_TERMINAL
