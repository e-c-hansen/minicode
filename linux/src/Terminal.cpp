// Terminal.cpp — see Terminal.h. Entirely compiled out unless
// MINICODE_ENABLE_TERMINAL is set.
#include <climits>
#include <unistd.h>
#include "Terminal.h"
#include <map>

#ifdef MINICODE_ENABLE_TERMINAL

#include "Appearance.h"
#include "BackupStore.h"
#include "Palette.h"
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

Terminal::Terminal(const std::string& cwd) : cwd_(cwd) {
    static int nextId = kFirstId;
    controlId_ = nextId++;
    terminalRegistry()[controlId_] = this;
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

void Terminal::installTermprops() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    vte_install_termprop(ShellIntegration::kVteMarkTermprop, VTE_PROPERTY_STRING, VTE_PROPERTY_FLAG_NONE);
}

namespace {

// A file the app ships: beside the binary (a build folder), else in the
// installed share/minicode; "" when neither has it.
std::string shippedFile(const char* name) {
    char exe[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return "";
    exe[n] = 0;
    char* dir = g_path_get_dirname(exe);
    char* beside = g_build_filename(dir, name, nullptr);
    char* shared = g_build_filename(dir, "..", "share", "minicode", name, nullptr);
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
    const std::string stub = ShellIntegration::zshenvStub(true, true);
    g_file_set_contents((dir + "/.zshenv").c_str(), stub.c_str(), (gssize)stub.size(), nullptr);
    if (!any) {
        removeIntegrationFolder(dir);
        return gIntegrationFolder;
    }
    gIntegrationFolder = dir;
    atexit([] { removeIntegrationFolder(gIntegrationFolder); });
    return gIntegrationFolder;
}

// The hooks' mark: believed only with this shell's nonce, read from the
// newest value alone (VTE keeps only the last of a read; ShellIntegration.h).
void Terminal::onTermpropChanged(VteTerminal* term, const char* name, gpointer selfp) {
    Terminal* self = static_cast<Terminal*>(selfp);
    if (!name || strcmp(name, ShellIntegration::kVteMarkTermprop) != 0 || self->nonce_.empty()) return;
    size_t len = 0;
    const char* value = vte_terminal_get_termprop_string(term, ShellIntegration::kVteMarkTermprop, &len);
    if (!value) return;   // unset: a value VTE refused
    ShellIntegration::VteMark m;
    if (!ShellIntegration::parseVteMark(std::string(value, len), self->nonce_, &m)) return;
    // Counters, not the busy flag: a short command's start and finish can
    // arrive in one read, of which VTE keeps only the last value.
    const bool ended = self->shell_.known && m.done > self->shell_.done;
    self->shell_.apply(m);
    if (self->onShellStateChanged) self->onShellStateChanged();
    // The finishing mark ends the command: no polling for an integrated shell.
    self->giveLineBack();
    if (ended && !self->shell_.busy && self->onCommandFinished) self->onCommandFinished();
}

void Terminal::makeTerminal() {
    if (vte_) return;
    StartupLog::mark("terminal: made, on first show");
    installTermprops();   // before the first VteTerminal exists
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroller, "minicode-terminal");
    gtk_widget_add_css_class(scroller, "minicode-scroller");

    vte_ = vte_terminal_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), vte_);

    underline_ = gtk_drawing_area_new();
    gtk_widget_set_can_target(underline_, FALSE);   // clicks go through to VTE
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(underline_), drawHover, this, nullptr);

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

    g_signal_connect(vte_, "child-exited", G_CALLBACK(onChildExited), this);
    g_signal_connect(vte_, "window-title-changed", G_CALLBACK(+[](VteTerminal*, gpointer p) {
        Terminal* t = static_cast<Terminal*>(p);
        if (t->onTitleChanged) t->onTitleChanged();
    }), this);
    {
        const std::string detailed = std::string("termprop-changed::") + ShellIntegration::kVteMarkTermprop;
        g_signal_connect(vte_, detailed.c_str(), G_CALLBACK(onTermpropChanged), this);
    }
    // Run in Terminal waits for the shell's prompt, and a Return typed here
    // starts the look for the command's end (typeText, onCommandFinished).
    g_signal_connect(vte_, "contents-changed", G_CALLBACK(onContentsChanged), this);
    g_signal_connect(vte_, "commit", G_CALLBACK(onCommit), this);
    // A key typed in the terminal ends its mark (a modifier alone is no
    // typing). The controller only watches: the key goes on to the shell.
    {
        GtkEventController* typed = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(typed, GTK_PHASE_CAPTURE);
        g_signal_connect(typed, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint keyval,
                                                              guint, GdkModifierType, gpointer p) -> gboolean {
            switch (keyval) {
            case GDK_KEY_Shift_L: case GDK_KEY_Shift_R: case GDK_KEY_Control_L:
            case GDK_KEY_Control_R: case GDK_KEY_Alt_L: case GDK_KEY_Alt_R:
            case GDK_KEY_Meta_L: case GDK_KEY_Meta_R: case GDK_KEY_Super_L:
            case GDK_KEY_Super_R: case GDK_KEY_ISO_Level3_Shift: case GDK_KEY_Caps_Lock:
                return FALSE;
            }
            Terminal* t = static_cast<Terminal*>(p);
            if (t->controlId_ > 0) TerminalAttention::typed(t->controlId_);
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
                         gtk_callback_action_new([](GtkWidget* w, GVariant*, gpointer) -> gboolean {
                             vte_terminal_paste_clipboard(VTE_TERMINAL(w));
                             return TRUE;
                         }, nullptr, nullptr)));
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
    spawnShell();
}

std::string Terminal::title() const {
    if (!userTitle_.empty()) return userTitle_;
    if (vte_) {
        const char* t = vte_terminal_get_window_title(VTE_TERMINAL(vte_));
        if (t && *t) return t;
    }
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
    g_cancellable_cancel(spawnCancel_);
    g_object_unref(spawnCancel_);
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
    if (self->hover_.empty()) return;
    VteTerminal* t = VTE_TERMINAL(self->vte_);
    const long cw = vte_terminal_get_char_width(t), ch = vte_terminal_get_char_height(t);
    const double top = self->topRow();
    const GtkBorder pad = cellInset(self->vte_);
    const int height = gtk_widget_get_height(self->vte_);
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
// no program holds the terminal's foreground (busy(), from tcgetpgrp). Not
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
// (Terminal.mm -startShell), so this does too.
void Terminal::onChildExited(VteTerminal*, gint /*status*/, gpointer selfp) {
    Terminal* self = static_cast<Terminal*>(selfp);
    self->shellPid_ = -1;

    // A shell that dies the instant it starts — $SHELL uninstalled, a profile
    // that exits, a bad interpreter — would otherwise respawn in a tight loop
    // and peg a core. Three fast deaths in a row is taken as "not coming back".
    const gint64 now = g_get_monotonic_time();
    if (now - self->lastSpawn_ < 1000000) {   // under a second counts as fast
        if (++self->rapidExits_ >= 3) {
            self->feedNotice("[shell keeps exiting immediately — not restarting]");
            return;
        }
    } else {
        self->rapidExits_ = 0;
    }

    self->feedNotice("[shell exited — restarting]");
    self->spawnShell();
}

// Reports a spawn that never got off the ground. Without this the panel just
// sits there blank and there is nothing to tell you why.
void Terminal::onSpawned(VteTerminal*, GPid pid, GError* error, gpointer selfp) {
    if (pid != -1 && !error) {
        static_cast<Terminal*>(selfp)->shellPid_ = pid;   // for its directory
        StartupLog::once("shell", "terminal: shell started (pid %d)", (int)pid);
        return;
    }
    // The window closed while the shell was starting: this object is gone.
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) return;
    Terminal* self = static_cast<Terminal*>(selfp);
    self->feedNotice(std::string("[could not start the shell: ") +
                     (error && error->message ? error->message : "unknown error") + "]");
}

void Terminal::spawnShell() {
    shellSpoke_ = false;   // a text to type waits for this shell's prompt
    const char* shell = g_getenv("SHELL");
    if (!shell || !*shell) shell = "/bin/bash";

    lastSpawn_ = g_get_monotonic_time();

    // The environment the app has, less any MINICODE_TERMINAL_ID and
    // MINICODE_PID it inherited (from a minicode this one was started in),
    // plus this terminal's (PARITY.md 3.6): `minicode notify --from-hook`
    // finds its terminal by them. Then what the window adds, such as the IDE
    // link's CLAUDE_CODE_SSE_PORT (PARITY.md 3.9).
    gchar** envv = g_get_environ();
    envv = g_environ_unsetenv(envv, "MINICODE_TERMINAL_ID");
    envv = g_environ_unsetenv(envv, "MINICODE_PID");
    for (const char* inherited : {"MINICODE_NONCE", "MINICODE_USER_ZDOTDIR", "MINICODE_EXEC"})
        envv = g_environ_unsetenv(envv, inherited);
    if (controlId_ > 0)
        envv = g_environ_setenv(envv, "MINICODE_TERMINAL_ID", std::to_string(controlId_).c_str(), TRUE);
    envv = g_environ_setenv(envv, "MINICODE_PID", std::to_string((int)getpid()).c_str(), TRUE);
    // BROWSER: programs opening a web page open it here (PARITY.md 3.7). The
    // script beside the binary (a build folder) or in the installed share
    // folder, and the app's own binary for it to run.
    {
        char exe[PATH_MAX];
        const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = 0;
            char* dir = g_path_get_dirname(exe);
            char* beside = g_build_filename(dir, "minicode-browser.js", nullptr);
            char* shared = g_build_filename(dir, "..", "share", "minicode", "minicode-browser.js", nullptr);
            char* app = g_build_filename(dir, "minicode", nullptr);
            const char* script = g_file_test(beside, G_FILE_TEST_IS_REGULAR) ? beside
                                 : g_file_test(shared, G_FILE_TEST_IS_REGULAR) ? shared : nullptr;
            if (script) envv = g_environ_setenv(envv, "BROWSER", script, TRUE);
            envv = g_environ_setenv(envv, "MINICODE_EXE",
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
    // user's ZDOTDIR back (MINICODE_USER_ZDOTDIR when they had one).
    shell_ = ShellIntegration::ShellState();
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
            envv = g_environ_setenv(envv, "MINICODE_NONCE", nonce_.c_str(), TRUE);
            bashShell_ = bash;
            if (bash) {
                args.push_back("--rcfile");
                args.push_back(dir + "/" + ShellIntegration::kBashScriptName);
            } else {
                if (const char* user = g_environ_getenv(envv, "ZDOTDIR")) {
                    const std::string theirs = user;   // envv changes below
                    envv = g_environ_setenv(envv, "MINICODE_USER_ZDOTDIR", theirs.c_str(), TRUE);
                }
                envv = g_environ_setenv(envv, "ZDOTDIR", dir.c_str(), TRUE);
            }
        }
    }
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);

    // vte_terminal_spawn_async is the current API (the sync variant is
    // deprecated in recent VTE).
    vte_terminal_spawn_async(
        VTE_TERMINAL(vte_),
        VTE_PTY_DEFAULT,
        cwd_.empty() ? nullptr : cwd_.c_str(),  // working directory
        argv.data(),
        envv,                                    // the inherited, with the ids and the window's
        G_SPAWN_DEFAULT,
        nullptr, nullptr, nullptr,               // child setup
        -1,                                      // timeout
        spawnCancel_,                            // cancelled if the window closes first
        onSpawned, this);                        // callback / user data
    g_strfreev(envv);   // spawn_async has copied the environment
}

void Terminal::focus() {
    makeTerminal();
    gtk_widget_grab_focus(vte_);
}

// ---------------------------------------------------- typing for the user

bool Terminal::busy() const {
    if (!vte_ || shellPid_ <= 0) return false;
    VtePty* pty = vte_terminal_get_pty(VTE_TERMINAL(vte_));
    if (!pty) return false;
    // The shell leads its own session and process group (VTE spawns it so);
    // a command it runs gets the foreground group of its own.
    const pid_t group = tcgetpgrp(vte_pty_get_fd(pty));
    return group > 0 && group != shellPid_;
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

void Terminal::onCommit(VteTerminal*, char* text, guint size, gpointer selfp) {
    Terminal* self = static_cast<Terminal*>(selfp);
    if (!text || (!memchr(text, '\r', size) && !memchr(text, '\n', size))) return;
    if (self->finishTimer_) return;   // already looking
    if (!self->nonce_.empty()) return;   // the integration's marks tell (onTermpropChanged)
    self->finishTimer_ = g_timeout_add(250, [](gpointer p) -> gboolean {
        Terminal* t = static_cast<Terminal*>(p);
        if (t->busy()) return G_SOURCE_CONTINUE;
        t->finishTimer_ = 0;
        if (t->onCommandFinished) t->onCommandFinished();
        return G_SOURCE_REMOVE;
    }, self);
}

#endif // MINICODE_ENABLE_TERMINAL
