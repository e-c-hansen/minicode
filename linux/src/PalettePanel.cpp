// PalettePanel.cpp — see PalettePanel.h. The decisions (what a query means,
// how rows rank, the recent lists' order, the outline, the walk Quick Open
// lists) are the core's CommandPalette and SymbolOutline, tested in
// tests/run_tests.cpp; this file shows them, as the Mac's PalettePanel.mm
// does.
#include "PalettePanel.h"

#include "Commands.h"
#include "Document.h"
#include "Editor.h"
#include "Json.h"
#include "Keys.h"
#include "SymbolOutline.h"
#include "ThemeCss.h"
#include "Utf8Offsets.h"

#include <glib/gstdio.h>
#include <limits.h>
#include <stdlib.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <utility>

namespace {

const int kRowHeight = 26;
const int kMaxRows = 12;                  // rows on show before the list scrolls
const size_t kQuickOpenMaxFiles = 20000;
const size_t kRecentCommands = 20;        // the Mac's caps
const size_t kRecentFiles = 50;
const char* kAccent = "#4EA1F7";
const char* kMuted = "#9CA3AF";
const char* kPlaceholder = "Search files by name (> commands, : a line, @ a symbol, # in the workspace)";

// ------------------------------------------------------------ recent lists
// The commands run lately (by registry id) and the files Quick Open opened
// (absolute paths), most recent first, for every window, in the state file
// (PARITY.md D11), as the Mac keeps them in its user defaults.
struct Recent {
    bool loaded = false;
    std::vector<std::string> commands, files;
};
Recent& recent() {
    static Recent r;
    return r;
}

std::vector<std::string> stringsOf(const Json& j, size_t cap) {
    std::vector<std::string> out;
    for (const Json& e : j.elements())
        if (e.isString() && !e.asString().empty() && out.size() < cap) out.push_back(e.asString());
    return out;
}

void loadRecent() {
    Recent& r = recent();
    if (r.loaded) return;
    r.loaded = true;
    gchar* raw = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(PalettePanel::statePath().c_str(), &raw, &len, nullptr)) return;
    Json j;
    const bool ok = Json::parse(std::string(raw, len), j);
    g_free(raw);
    if (!ok || !j.isObject()) return;
    r.commands = stringsOf(j["commands"], kRecentCommands);
    r.files = stringsOf(j["files"], kRecentFiles);
}

void saveRecent() {
    const Recent& r = recent();
    Json commands = Json::array(), files = Json::array();
    for (const std::string& s : r.commands) commands.push(s);
    for (const std::string& s : r.files) files.push(s);
    Json j = Json::object();
    j.set("commands", commands);
    j.set("files", files);
    const std::string path = PalettePanel::statePath();
    gchar* dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    const std::string text = j.dump() + "\n";
    // A temporary file renamed over the old one, so a crash leaves either.
    g_file_set_contents(path.c_str(), text.c_str(), (gssize)text.size(), nullptr);
}

void noteRecent(bool command, const std::string& value) {
    loadRecent();
    Recent& r = recent();
    if (command) r.commands = CommandPalette::noteRun(r.commands, value, kRecentCommands);
    else r.files = CommandPalette::noteRun(r.files, value, kRecentFiles);
    saveRecent();
}

// ------------------------------------------------------------ text
std::string escaped(const std::string& s) {
    gchar* e = g_markup_escape_text(s.c_str(), (gssize)s.size());
    std::string out = e;
    g_free(e);
    return out;
}

// A row's label as markup: the part before `titleAt` (a command's menu, a
// file's folder) muted, the matched UTF-16 units in the accent color and
// bold, the rest in the row's color.
std::string marked(const std::u16string& label, size_t titleAt, const std::vector<size_t>& positions) {
    const std::set<size_t> hit(positions.begin(), positions.end());
    std::string out;
    std::u16string run;
    int style = -1;
    auto flush = [&] {
        if (run.empty()) return;
        const std::string t = escaped(Lsp::toUtf8(run));
        if (style == 2)
            out += std::string("<span foreground=\"") + kAccent + "\" weight=\"bold\">" + t + "</span>";
        else if (style == 1)
            out += std::string("<span foreground=\"") + kMuted + "\">" + t + "</span>";
        else
            out += t;
        run.clear();
    };
    for (size_t i = 0; i < label.size();) {
        const size_t n =
            (utf16::isHigh(label[i]) && i + 1 < label.size() && utf16::isLow(label[i + 1])) ? 2 : 1;
        const int s = hit.count(i) ? 2 : (i < titleAt ? 1 : 0);
        if (s != style) {
            flush();
            style = s;
        }
        run.append(label, i, n);
        i += n;
    }
    flush();
    return out;
}

std::string oneLine(const std::string& s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

std::string baseName(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string resolved(const std::string& p) {
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : std::string();
}

std::string withSlash(const std::string& p) {
    return !p.empty() && p.back() == '/' ? p : p + "/";
}

// The iterator at an LSP position (a 0-based line and a UTF-16 column),
// clamped to the buffer and the line.
void iterAt(GtkTextBuffer* b, Lsp::Position p, GtkTextIter* it) {
    const int lines = gtk_text_buffer_get_line_count(b);
    gtk_text_buffer_get_iter_at_line(b, it, std::max(0, std::min(p.line, lines - 1)));
    GtkTextIter end = *it;
    if (!gtk_text_iter_ends_line(&end)) gtk_text_iter_forward_to_line_end(&end);
    char* raw = gtk_text_buffer_get_slice(b, it, &end, TRUE);
    const std::string line = raw ? raw : "";
    g_free(raw);
    gtk_text_iter_set_line_index(it, (int)utf16::byteOffsetOfUtf16(line, (size_t)std::max(0, p.character)));
}

// The text of a buffer's line, without its break.
std::string lineText(GtkTextBuffer* b, int line) {
    GtkTextIter a, e;
    gtk_text_buffer_get_iter_at_line(b, &a, line);
    e = a;
    if (!gtk_text_iter_ends_line(&e)) gtk_text_iter_forward_to_line_end(&e);
    char* raw = gtk_text_buffer_get_slice(b, &a, &e, TRUE);
    std::string out = raw ? raw : "";
    g_free(raw);
    return out;
}

bool onScreen(GtkTextView* tv, const GtkTextIter* it) {
    GdkRectangle vis, at;
    gtk_text_view_get_visible_rect(tv, &vis);
    gtk_text_view_get_iter_location(tv, it, &at);
    return vis.height > 0 && at.y >= vis.y && at.y + at.height <= vis.y + vis.height;
}

// Scroll the view, now, so that `it` is `from` pixels below the top of the
// visible area, or (from < 0) a third of the way down when it is not on
// screen already, as the Mac reveals a symbol. Set on the adjustment
// directly rather than through gtk_text_view_scroll_to_mark, whose scroll
// waits for the next layout and would undo a scroll put back meanwhile.
void scrollTo(GtkTextView* tv, const GtkTextIter* it, int from) {
    if (from < 0 && onScreen(tv, it)) return;
    GdkRectangle vis, at;
    gtk_text_view_get_visible_rect(tv, &vis);
    gtk_text_view_get_iter_location(tv, it, &at);
    GtkAdjustment* v = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tv));
    const double into = from < 0 ? vis.height / 3.0 : from;
    gtk_adjustment_set_value(v, gtk_adjustment_get_value(v) + (at.y - vis.y) - into);
}

GtkCssProvider* gCss = nullptr;

struct Later {
    std::weak_ptr<int> alive;
    std::function<void()> fn;
};

}  // namespace

// What one listing carries into its worker thread and back; the GTask owns it.
struct PalettePanel::ListJob {
    unsigned gen = 0;
    std::string root;
    bool useIgnore = true;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::weak_ptr<int> alive;
    std::vector<std::string> files;
    bool truncated = false;
    bool offMain = false;
};

std::string PalettePanel::statePath() {
    return std::string(g_get_user_state_dir()) + "/minicode/palette.json";
}

// ------------------------------------------------------------ building
PalettePanel::PalettePanel(GtkWindow* window, GtkOverlay* overlay, Editor* editor)
    : window_(window), overlay_(overlay), editor_(editor) {
    build();
    focusHandler_ = g_signal_connect(window_, "notify::focus-widget", G_CALLBACK(onFocusWidget), this);
    positionHandler_ =
        g_signal_connect(overlay_, "get-child-position", G_CALLBACK(onChildPosition), this);
}

PalettePanel::~PalettePanel() {
    life_.reset();
    if (wsTimer_) g_source_remove(wsTimer_);
    wsTimer_ = 0;
    if (cancelListing_) cancelListing_->store(true);
    forgetRestore();
    // By data, not by id: main.cpp's teardown may have taken them off
    // already (disconnectOwners), and a second disconnect by id warns.
    g_signal_handlers_disconnect_by_data(window_, this);
    g_signal_handlers_disconnect_by_data(overlay_, this);
    if (factory_) g_signal_handlers_disconnect_by_data(factory_, this);
    if (list_) g_object_set_data(G_OBJECT(list_), "mc-palette", nullptr);
    for (GtkWidget* w : {entry_, sheet_}) {
        if (!w) continue;
        g_signal_handlers_disconnect_by_data(w, this);
        GListModel* ctrls = gtk_widget_observe_controllers(w);
        for (guint i = 0; i < g_list_model_get_n_items(ctrls); i++) {
            GObject* c = static_cast<GObject*>(g_list_model_get_item(ctrls, i));
            g_signal_handlers_disconnect_by_data(c, this);
            g_object_unref(c);
        }
        g_object_unref(ctrls);
    }
    bound_.clear();
    if (model_) g_object_unref(model_);
    if (factory_) g_object_unref(factory_);
}

void PalettePanel::build() {
    // The sheet under the box: a click on it, outside the box, closes the
    // palette and goes no further.
    sheet_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(sheet_, "minicode-palette-sheet");
    GtkGesture* press = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(press), 0);
    g_signal_connect(press, "pressed", G_CALLBACK(onSheetPressed), this);
    gtk_widget_add_controller(sheet_, GTK_EVENT_CONTROLLER(press));
    gtk_widget_set_visible(sheet_, FALSE);
    gtk_overlay_add_overlay(overlay_, sheet_);

    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(box_, "minicode-palette");

    entry_ = gtk_entry_new();
    gtk_widget_add_css_class(entry_, "minicode-palette-field");
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_), kPlaceholder);
    changedHandler_ = g_signal_connect(entry_, "changed", G_CALLBACK(onChanged), this);
    // Before the field's own keys: Up and Down would otherwise move the
    // keyboard out of it, and Tab leave it open without the keyboard.
    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(onKey), this);
    gtk_widget_add_controller(entry_, keys);
    gtk_box_append(GTK_BOX(box_), entry_);

    // The rows: a list view (only the rows on show have widgets, for a
    // folder's 500 best files or a file's 2,000 symbols) over a list of
    // indexes into items_. It never takes the keyboard; the field keeps it
    // while rows are clicked.
    model_ = gtk_string_list_new(nullptr);
    factory_ = gtk_signal_list_item_factory_new();
    g_signal_connect(factory_, "setup", G_CALLBACK(onSetup), this);
    g_signal_connect(factory_, "bind", G_CALLBACK(onBind), this);
    g_signal_connect(factory_, "unbind", G_CALLBACK(onUnbind), this);
    GtkNoSelection* selection = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(model_)));
    list_ = gtk_list_view_new(GTK_SELECTION_MODEL(selection),
                              GTK_LIST_ITEM_FACTORY(g_object_ref(factory_)));
    gtk_widget_add_css_class(list_, "minicode-palette-list");
    gtk_widget_set_focusable(list_, FALSE);
    gtk_widget_set_focus_on_click(list_, FALSE);
    g_object_set_data(G_OBJECT(list_), "mc-palette", this);

    scroll_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll_), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll_), kRowHeight * kMaxRows);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll_), list_);
    gtk_box_append(GTK_BOX(box_), scroll_);

    empty_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(empty_), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(empty_), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(empty_, "minicode-palette-empty");
    gtk_widget_set_visible(empty_, FALSE);
    gtk_box_append(GTK_BOX(box_), empty_);

    gtk_widget_set_visible(box_, FALSE);
    gtk_overlay_add_overlay(overlay_, box_);
}

void PalettePanel::applySettings(const Settings& s) {
    GdkDisplay* display = gdk_display_get_default();
    if (!display) return;
    if (!gCss) {
        gCss = gtk_css_provider_new();
        // Above the application's stylesheet, whose broad rules (entries,
        // lists) would otherwise reach into the box.
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(gCss),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
    // The box in the sidebar's color and the field in the editor's, both
    // opaque even when the window is see-through, as on the Mac.
    Rgba box = s.background(Surface::Sidebar), field = s.background(Surface::Editor);
    box.a = 1;
    field.a = 1;
    const std::string text = theme::cssColor(s.text(Surface::Sidebar));
    const std::string fieldText = theme::cssColor(s.text(Surface::Editor));
    std::string css;
    css += ".minicode-palette-sheet { background: none; }";
    css += ".minicode-palette { background-color: " + theme::cssColor(box) +
           "; color: " + text + "; border: 1px solid #454545; border-radius: 7px; padding: 6px;"
           " box-shadow: 0 4px 14px rgba(0,0,0,0.45); }";
    css += ".minicode-palette entry.minicode-palette-field { background-color: " +
           theme::cssColor(field) + "; background-image: none; color: " + fieldText +
           "; border: 1px solid " + kAccent + "; border-radius: 4px; box-shadow: none;"
           " outline: none; min-height: 28px; padding: 0 8px; font-size: 14px; }";
    css += ".minicode-palette entry.minicode-palette-field placeholder { color: " +
           std::string(kMuted) + "; }";
    css += ".minicode-palette scrolledwindow, .minicode-palette listview { background: none; }";
    css += ".minicode-palette listview > row { padding: 0; min-height: 0; background: none;"
           " outline: none; }";
    css += ".minicode-palette-row { min-height: " + std::to_string(kRowHeight) +
           "px; padding: 0 10px; border-radius: 4px; }";
    css += ".minicode-palette-row label { color: " + text + "; font-size: 13px; }";
    css += ".minicode-palette-row:hover { background-color: rgba(255,255,255,0.05); }";
    css += ".minicode-palette-row.selected { background-color: rgba(78,161,247,0.28); }";
    css += ".minicode-palette-row label.minicode-palette-keys { color: " + std::string(kMuted) +
           "; font-size: 12px; }";
    css += ".minicode-palette-row.heading label { color: " + std::string(kMuted) +
           "; font-size: 11px; font-weight: bold; }";
    css += ".minicode-palette-row.heading:hover { background: none; }";
    css += ".minicode-palette-empty { color: " + std::string(kMuted) +
           "; font-size: 13px; padding: 4px 10px; }";
    gtk_css_provider_load_from_string(gCss, css.c_str());
}

// ------------------------------------------------------------ open, close
void PalettePanel::show(const std::string& text, const Context& context) {
    // A pick list is its own palette: one never takes over the other.
    if (open_ && pick_) close(true);
    if (open_) {
        gtk_widget_grab_focus(entry_);
        takeText(text);
        return;
    }
    ctx_ = context;
    // Read before the keyboard moves, so each action's enabled state is the
    // one for where the keyboard was (Play/Pause is off while it types).
    collectCommands();
    open(text);
}

void PalettePanel::showPick(const Pick& pick) {
    if (open_) close(true);
    ctx_ = Context();   // no folder: nothing is listed
    commands_.clear();
    entries_.clear();
    pick_ = std::make_unique<Pick>(pick);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_), pick_->placeholder.c_str());
    open("");
}

void PalettePanel::open(const std::string& text) {
    rememberFocus();
    place_ = Place();
    openDocId_ = editor_->active() ? editor_->active()->id() : 0;
    loadRecent();
    recentKeys_ = recentFileKeys();
    session_++;
    files_.clear();
    fileKeys_.clear();
    filesListed_ = filesOffMain_ = filesTruncated_ = false;
    askedSymbols_ = haveSymbols_ = false;
    symbols_.clear();
    caretSymbol_ = preselect_ = -1;
    symbolSource_.clear();
    previewed_ = ran_ = false;
    wsSymbols_.clear();
    wsPaths_.clear();
    wsComplete_ = false;
    wsNote_.clear();
    if (onOpenChanged) onOpenChanged(true);   // before anything is previewed
    open_ = true;
    gtk_widget_set_visible(sheet_, TRUE);
    gtk_widget_set_visible(box_, TRUE);
    gtk_widget_grab_focus(entry_);
    takeText(text);
    listFiles();
}

void PalettePanel::close(bool restoreFocus) {
    if (!open_ || closing_) return;
    closing_ = true;
    if (cancelListing_) cancelListing_->store(true);
    cancelListing_.reset();
    stopWorkspace();
    // Closed without going to a symbol: the editor goes back as it was.
    if (previewed_ && !ran_) endPreview();
    if (restoreFocus) this->restoreFocus();
    gtk_widget_set_visible(box_, FALSE);
    gtk_widget_set_visible(sheet_, FALSE);
    items_.clear();
    selected_ = -1;
    gtk_string_list_splice(model_, 0, g_list_model_get_n_items(G_LIST_MODEL(model_)), nullptr);
    forgetRestore();
    if (pick_) {
        pick_.reset();
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry_), kPlaceholder);
    }
    open_ = false;
    closing_ = false;
    if (onOpenChanged) onOpenChanged(false);
}

void PalettePanel::move(const std::function<void()>& body) {
    if (aroundMove) aroundMove(body);
    else body();
}

// New text in the field, the caret after it, and the list to match.
void PalettePanel::takeText(const std::string& text) {
    g_signal_handler_block(entry_, changedHandler_);
    gtk_editable_set_text(GTK_EDITABLE(entry_), text.c_str());
    g_signal_handler_unblock(entry_, changedHandler_);
    refilter();
    gtk_editable_set_position(GTK_EDITABLE(entry_), -1);
}

void PalettePanel::rememberFocus() {
    forgetRestore();
    GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(window_));
    if (!focus || focus == box_ || gtk_widget_is_ancestor(focus, box_)) return;
    restore_ = focus;
    g_object_add_weak_pointer(G_OBJECT(restore_), reinterpret_cast<gpointer*>(&restore_));
    restoreStart_ = restoreEnd_ = -1;
    if (GTK_IS_TEXT(focus))
        gtk_editable_get_selection_bounds(GTK_EDITABLE(focus), &restoreStart_, &restoreEnd_);
}

void PalettePanel::forgetRestore() {
    if (restore_)
        g_object_remove_weak_pointer(G_OBJECT(restore_), reinterpret_cast<gpointer*>(&restore_));
    restore_ = nullptr;
}

// The keyboard back where it was. A text field gets it without selecting
// all its text, as it would, and its caret or selection as it was.
void PalettePanel::restoreFocus() {
    GtkWidget* r = restore_;
    const bool ok = r && gtk_widget_get_root(r) == GTK_ROOT(window_) && gtk_widget_is_visible(r) &&
                    gtk_widget_get_sensitive(r);
    if (!ok) {
        gtk_root_set_focus(GTK_ROOT(window_), nullptr);
        return;
    }
    if (GTK_IS_TEXT(r)) {
        gtk_text_grab_focus_without_selecting(GTK_TEXT(r));
        if (restoreStart_ >= 0) gtk_editable_select_region(GTK_EDITABLE(r), restoreStart_, restoreEnd_);
    } else {
        gtk_widget_grab_focus(r);
    }
}

// ------------------------------------------------------------ the commands
// Every command the port has (Keys.h's table, in the registry's order) whose
// action is enabled now, but the palette itself.
void PalettePanel::collectCommands() {
    commands_.clear();
    entries_.clear();
    GActionGroup* win = G_ACTION_GROUP(window_);
    GtkApplication* gapp = gtk_window_get_application(window_);
    GActionGroup* app = gapp ? G_ACTION_GROUP(gapp) : nullptr;
    for (const Keys::Action& a : Keys::actions()) {
        const std::string id = a.id;
        if (id == "workbench.action.showCommands") continue;
        const Commands::Command* c = Commands::find(id);
        if (!c) continue;
        const std::string full = a.action;
        const size_t dot = full.find('.');
        if (dot == std::string::npos) continue;
        GActionGroup* group = full.compare(0, dot, "app") == 0 ? app : win;
        const std::string name = full.substr(dot + 1);
        if (!group || !g_action_group_has_action(group, name.c_str()) ||
            !g_action_group_get_action_enabled(group, name.c_str()))
            continue;
        Cmd cmd;
        cmd.id = id;
        cmd.action = full;
        cmd.title = Commands::titleOf(*c, Commands::Platform::Linux);
        cmd.label = std::string(Commands::menuOf(*c, Commands::Platform::Linux)) + " > " + cmd.title;
        cmd.keys = Keys::label(id, "+");
        CommandPalette::Entry e;
        e.label = Lsp::toUtf16(cmd.label);
        e.title = Lsp::toUtf16(cmd.title);
        e.id = id;
        entries_.push_back(std::move(e));
        commands_.push_back(std::move(cmd));
    }
}

void PalettePanel::commandItems(const std::u16string& query, std::string* empty) {
    for (const CommandPalette::Match& m : CommandPalette::rank(entries_, query, recent().commands)) {
        const Cmd& c = commands_[m.index];
        const CommandPalette::Entry& e = entries_[m.index];
        Item it;
        it.label = c.label;
        it.markup = marked(e.label, e.label.size() - e.title.size(), m.positions);
        it.keys = c.keys;
        const std::string id = c.id, action = c.action;
        GtkWindow* window = window_;
        it.run = [this, id, action, window] {
            noteRecent(true, id);
            close(true);
            // On the next turn, out of the field's own key handling, with the
            // keyboard back where it was, so the command acts on what had
            // it, as from the menu bar.
            later([window, action] {
                gtk_widget_activate_action(GTK_WIDGET(window), action.c_str(), nullptr);
            });
        };
        items_.push_back(std::move(it));
    }
    if (items_.empty()) *empty = "No matching commands";
}

// ------------------------------------------------------------ Go to Line
void PalettePanel::lineItem(const CommandPalette::Parsed& p) {
    Item it;
    Document* d = editor_->active();
    const bool canGo = d && d->kind() == Document::Kind::Text;
    if (!canGo) {
        it.label = "Open a file to go to one of its lines";
    } else if (!p.lineValid) {
        it.label = "Type a line number, and a column after a colon if you like";
    } else if (p.line <= 0) {
        it.label = "Type a line number to go to";
    } else {
        it.label = p.column > 0 ? "Go to line " + std::to_string(p.line) + ", column " +
                                      std::to_string(p.column)
                                : "Go to line " + std::to_string(p.line);
        const int line = p.line, column = p.column;
        it.run = [this, line, column] {
            close(true);
            later([this, line, column] {
                move([this, line, column] { editor_->revealLineColumn(line, column); });
            });
        };
    }
    it.markup = it.run ? escaped(it.label)
                       : std::string("<span foreground=\"") + kMuted + "\">" + escaped(it.label) +
                             "</span>";
    items_.push_back(std::move(it));
}

// ------------------------------------------------------------ Quick Open
void PalettePanel::listFiles() {
    if (cancelListing_) cancelListing_->store(true);
    cancelListing_.reset();
    if (ctx_.root.empty()) {
        filesListed_ = true;
        return;
    }
    auto* job = new ListJob;
    job->gen = ++listGen_;
    job->root = ctx_.root;
    job->useIgnore = ctx_.useIgnoreFiles;
    job->cancel = std::make_shared<std::atomic<bool>>(false);
    job->alive = life_;
    cancelListing_ = job->cancel;
    GTask* task = g_task_new(nullptr, nullptr, onListed, this);
    g_task_set_task_data(task, job, [](gpointer p) { delete static_cast<ListJob*>(p); });
    g_task_run_in_thread(task, listWorker);
    g_object_unref(task);   // the running task holds its own reference
}

// On a GLib worker thread; touches nothing but the job.
void PalettePanel::listWorker(GTask* task, gpointer, gpointer data, GCancellable*) {
    ListJob* job = static_cast<ListJob*>(data);
    job->offMain = !g_main_context_is_owner(g_main_context_default());
    job->files = CommandPalette::listFiles(job->root, kQuickOpenMaxFiles, job->cancel.get(),
                                           &job->truncated, job->useIgnore);
    g_task_return_boolean(task, TRUE);
}

void PalettePanel::onListed(GObject*, GAsyncResult* res, gpointer selfp) {
    ListJob* job = static_cast<ListJob*>(g_task_get_task_data(G_TASK(res)));
    if (job->alive.expired() || job->cancel->load()) return;   // closed, or the window went
    PalettePanel* self = static_cast<PalettePanel*>(selfp);
    if (job->gen != self->listGen_ || !self->open_) return;
    self->files_ = std::move(job->files);
    self->fileKeys_.clear();
    self->fileKeys_.reserve(self->files_.size());
    for (const std::string& f : self->files_) self->fileKeys_.push_back(Lsp::toUtf16(f));
    self->filesTruncated_ = job->truncated;
    self->filesOffMain_ = job->offMain;
    self->filesListed_ = true;
    self->cancelListing_.reset();
    const std::string text = gtk_editable_get_text(GTK_EDITABLE(self->entry_));
    if (CommandPalette::parse(Lsp::toUtf16(text)).mode == CommandPalette::Mode::Files)
        self->refilter();
}

// The files the window opened lately (its tabs, most recently on show
// first), then those Quick Open opened before, as paths relative to the
// root; compared with symlinks resolved too, as /tmp and /private/tmp are
// one on the Mac.
std::vector<std::u16string> PalettePanel::recentFileKeys() const {
    std::vector<std::u16string> out;
    if (ctx_.root.empty()) return out;
    std::vector<Document*> tabs = editor_->documents();
    std::stable_sort(tabs.begin(), tabs.end(),
                     [](const Document* a, const Document* b) { return a->lastUsed > b->lastUsed; });
    std::vector<std::string> abs;
    for (const Document* d : tabs)
        if (d->kind() != Document::Kind::Diff && !d->path().empty()) abs.push_back(d->path());
    for (const std::string& f : recent().files) abs.push_back(f);
    const std::string prefix = withSlash(ctx_.root);
    const std::string real = resolved(ctx_.root);
    const std::string realPrefix = real.empty() ? std::string() : withSlash(real);
    for (const std::string& a : abs) {
        if (a.compare(0, prefix.size(), prefix) == 0) {
            out.push_back(Lsp::toUtf16(a.substr(prefix.size())));
            continue;
        }
        if (realPrefix.empty()) continue;
        const std::string r = resolved(a);
        if (!r.empty() && r.compare(0, realPrefix.size(), realPrefix) == 0)
            out.push_back(Lsp::toUtf16(r.substr(realPrefix.size())));
    }
    return out;
}

void PalettePanel::fileItems(const std::u16string& query, std::string* empty) {
    if (!filesListed_) {
        *empty = "Listing the folder's files…";
        return;
    }
    const std::set<std::u16string> recentSet(recentKeys_.begin(), recentKeys_.end());
    const std::string prefix = withSlash(ctx_.root);
    for (const CommandPalette::Match& m : CommandPalette::rankFiles(fileKeys_, query, recentKeys_)) {
        const std::string& rel = files_[m.index];
        const std::u16string& key = fileKeys_[m.index];
        Item it;
        it.label = rel;
        const size_t slash = key.rfind(u'/');
        it.markup = marked(key, slash == std::u16string::npos ? 0 : slash + 1, m.positions);
        if (recentSet.count(key)) it.keys = "recently opened";
        const std::string abs = prefix + rel;
        auto open = ctx_.openFile;
        it.run = [this, abs, open] {
            noteRecent(false, abs);
            close(true);
            later([open, abs] {
                if (open) open(abs);
            });
        };
        items_.push_back(std::move(it));
    }
    if (items_.empty()) *empty = files_.empty() ? "No files in this folder" : "No matching files";
}

// ------------------------------------------------------------ pick lists
// The caller's rows that match what is typed, best first (the palette's
// commands' rules, with nothing run lately), then the row made from the
// typed text, if any; the Mac's pickItems:.
void PalettePanel::pickItems(std::string* empty) {
    const Pick& pick = *pick_;
    const std::string text = gtk_editable_get_text(GTK_EDITABLE(entry_));
    std::vector<CommandPalette::Entry> entries;
    entries.reserve(pick.labels.size());
    for (const std::string& label : pick.labels) {
        CommandPalette::Entry e;
        e.label = e.title = Lsp::toUtf16(label);
        e.id = label;
        entries.push_back(std::move(e));
    }
    for (const CommandPalette::Match& m : CommandPalette::rank(entries, Lsp::toUtf16(text), {})) {
        const int index = (int)m.index;
        Item it;
        it.label = pick.labels[(size_t)index];
        it.markup = marked(entries[(size_t)index].label, 0, m.positions);
        if ((size_t)index < pick.details.size()) it.keys = pick.details[(size_t)index];
        auto choose = pick.choose;
        it.run = [this, choose, index] {
            close(true);
            later([choose, index] {
                if (choose) choose(index);
            });
        };
        items_.push_back(std::move(it));
    }
    const std::string typed = pick.typedRow ? pick.typedRow(text) : std::string();
    if (!typed.empty()) {
        Item it;
        it.label = typed;
        it.markup = escaped("+ " + typed);
        auto chooseTyped = pick.chooseTyped;
        it.run = [this, chooseTyped, text] {
            close(true);
            later([chooseTyped, text] {
                if (chooseTyped) chooseTyped(text);
            });
        };
        items_.push_back(std::move(it));
    }
    if (items_.empty()) *empty = pick.emptyText;
}

// ------------------------------------------------------------ symbols
// The file on show has an outline to list: its editable source, or its
// rendered Markdown page (the headings, from the source). A LaTeX file's
// typeset pages have none, as on the Mac.
bool PalettePanel::symbolsAvailable() const {
    Document* d = editor_->active();
    if (!d || d->kind() != Document::Kind::Text || d->path().empty()) return false;
    if (d->showsPreview()) return d->isMarkdown() && editor_->buffer() != d->buffer();
    return editor_->isSource();
}

// Where the caret is in the file on show's source, as LSP counts it; false
// on a Markdown page.
bool PalettePanel::caretPosition(Document* d, Lsp::Position* p) const {
    if (!d || d->showsPreview()) return false;
    GtkTextBuffer* b = d->buffer();
    GtkTextIter at, start;
    gtk_text_buffer_get_iter_at_mark(b, &at, gtk_text_buffer_get_insert(b));
    start = at;
    gtk_text_iter_set_line_offset(&start, 0);
    char* raw = gtk_text_buffer_get_slice(b, &start, &at, TRUE);
    p->line = gtk_text_iter_get_line(&at);
    p->character = (int)Lsp::toUtf16(raw ? raw : "").size();
    g_free(raw);
    return true;
}

// minicode's own outline of a file, and the symbol the caret is in.
std::vector<Lsp::Symbol> PalettePanel::outlineSymbols(Document* d, int* caret) const {
    std::vector<Lsp::Symbol> syms = Outline::outline(baseName(d->path()), Lsp::toUtf16(d->text()));
    Lsp::Position p;
    *caret = caretPosition(d, &p) ? Lsp::symbolAt(syms, p) : -1;
    return syms;
}

// The file's symbols, from its language server when one lists them (not on
// a Markdown page, whose headings are the outline's), else the outline, and
// the symbol the caret is in; handed over on a later turn, never in the
// middle of a refilter.
void PalettePanel::askSymbols() {
    Document* d = editor_->active();
    if (!d) return;
    const unsigned session = session_;
    const uint64_t docId = d->id();
    if (!d->showsPreview() && ctx_.documentSymbols) {
        std::weak_ptr<int> alive = life_;
        auto done = [this, alive, session, docId](std::vector<Lsp::Symbol> syms, bool refused,
                                                   const std::string& source) {
            if (alive.expired()) return;
            auto shared = std::make_shared<std::vector<Lsp::Symbol>>(std::move(syms));
            later([this, session, docId, shared, refused, source] {
                if (!open_ || session != session_) return;
                Document* doc = editor_->active();
                if (!doc || doc->id() != docId) return;
                int caret = -1;
                if (refused) {   // the outline stands in
                    std::vector<Lsp::Symbol> outline = outlineSymbols(doc, &caret);
                    setSymbols(std::move(outline), caret, "outline");
                    return;
                }
                Lsp::Position p;
                if (caretPosition(doc, &p)) caret = Lsp::symbolAt(*shared, p);
                setSymbols(std::move(*shared), caret, source);
            });
        };
        if (ctx_.documentSymbols(done)) return;
    }
    int caret = -1;
    auto shared = std::make_shared<std::vector<Lsp::Symbol>>(outlineSymbols(d, &caret));
    later([this, session, shared, caret] {
        if (!open_ || session != session_) return;
        setSymbols(std::move(*shared), caret, "outline");
    });
}

void PalettePanel::setSymbols(std::vector<Lsp::Symbol> symbols, int caret,
                              const std::string& source) {
    symbols_ = std::move(symbols);
    caretSymbol_ = caret;
    haveSymbols_ = true;
    symbolSource_ = source;
    const std::string text = gtk_editable_get_text(GTK_EDITABLE(entry_));
    if (CommandPalette::parse(Lsp::toUtf16(text)).mode == CommandPalette::Mode::Symbols) refilter();
}

void PalettePanel::symbolItems(const std::u16string& query, bool byKind, std::string* empty) {
    preselect_ = -1;
    if (!symbolsAvailable()) {
        *empty = "Open a file to go to one of its symbols";
        return;
    }
    if (!askedSymbols_) {
        askedSymbols_ = true;
        askSymbols();
    }
    if (!haveSymbols_) {
        *empty = "Listing the file's symbols…";
        return;
    }
    int pre = -1;
    const std::vector<CommandPalette::SymbolRow> rows =
        CommandPalette::symbolRows(symbols_, query, byKind, caretSymbol_, &pre);
    for (const CommandPalette::SymbolRow& r : rows) {
        Item it;
        if (r.heading) {
            it.heading = true;
            it.label = Lsp::toUtf8(r.text);
            it.markup = escaped(it.label);
            items_.push_back(std::move(it));
            continue;
        }
        const size_t index = r.symbol;
        const Lsp::Symbol& s = symbols_[index];
        const std::string extra = !s.container.empty() ? s.container : oneLine(s.detail);
        char color[16];
        std::snprintf(color, sizeof color, "#%06X", Lsp::symbolKindColor(s.kind) & 0xFFFFFF);
        it.symbol = true;
        it.label = s.name;
        it.markup = std::string("<span foreground=\"") + color + "\" weight=\"bold\">" +
                    escaped(Lsp::toUtf8(Lsp::symbolKindGlyph(s.kind))) + "</span>  " +
                    marked(Lsp::toUtf16(s.name), 0, r.match.positions);
        if (!extra.empty())
            it.markup += std::string("   <span foreground=\"") + kMuted + "\" size=\"small\">" +
                         escaped(extra) + "</span>";
        it.detail = extra;
        it.keys = std::to_string(s.selectionRange.start.line + 1);
        it.preview = [this, index] {
            if (index >= symbols_.size()) return;
            previewed_ = true;
            previewSymbol(symbols_[index]);
        };
        it.run = [this, index] {
            if (index >= symbols_.size()) return;
            const Lsp::Symbol s = symbols_[index];
            ran_ = true;
            close(true);
            later([this, s] { move([this, s] { goToSymbol(s); }); });
        };
        items_.push_back(std::move(it));
    }
    preselect_ = pre;
    if (!items_.empty()) return;
    if (!symbols_.empty()) *empty = "No matching symbols";
    else if (symbolSource_ == "outline") *empty = "No symbols found in this file";
    else *empty = "The language server lists no symbols here";
}

// ------------------------------------------------------------ workspace
// Go to Symbol in Workspace ("#"): the last answer ranked against what is
// typed now, while the servers are asked again a moment after typing stops,
// as on the Mac.
void PalettePanel::workspaceItems(const std::u16string& query, std::string* empty) {
    if (!ctx_.workspaceSymbols) {
        *empty = "No language server is running in this window";
        return;
    }
    const std::string q = Lsp::toUtf8(query);
    if (q.empty()) {
        stopWorkspace();
        wsSymbols_.clear();
        wsPaths_.clear();
        wsComplete_ = false;
        *empty = "Type the name of a symbol to look for it in the workspace";
        return;
    }
    if (q != wsAsked_) {
        wsAsked_ = q;
        wsComplete_ = false;
        if (wsTimer_) g_source_remove(wsTimer_);
        wsTimer_ = g_timeout_add(150, [](gpointer p) -> gboolean {
            PalettePanel* self = static_cast<PalettePanel*>(p);
            self->wsTimer_ = 0;
            self->askWorkspace(self->wsAsked_);
            return G_SOURCE_REMOVE;
        }, this);
    }
    std::string prefix = ctx_.root;
    if (!prefix.empty() && prefix.back() != '/') prefix += '/';
    const auto open = ctx_.openWorkspaceSymbol;
    int pre = -1;
    for (const CommandPalette::SymbolRow& r :
         CommandPalette::symbolRows(wsSymbols_, query, false, -1, &pre, 300)) {
        if (r.heading || r.symbol >= wsSymbols_.size()) continue;
        const Lsp::Symbol& s = wsSymbols_[r.symbol];
        std::string path = r.symbol < wsPaths_.size() ? wsPaths_[r.symbol] : "";
        if (!prefix.empty() && path.rfind(prefix, 0) == 0) path = path.substr(prefix.size());
        Item it;
        char color[16];
        std::snprintf(color, sizeof color, "#%06X", Lsp::symbolKindColor(s.kind) & 0xFFFFFF);
        it.symbol = true;
        it.label = s.name;
        it.markup = std::string("<span foreground=\"") + color + "\" weight=\"bold\">" +
                    escaped(Lsp::toUtf8(Lsp::symbolKindGlyph(s.kind))) + "</span>  " +
                    marked(Lsp::toUtf16(s.name), 0, r.match.positions);
        if (!s.container.empty())
            it.markup += std::string("   <span foreground=\"") + kMuted + "\" size=\"small\">" +
                         escaped(s.container) + "</span>";
        it.detail = s.container;
        it.keys = path + ":" + std::to_string(s.selectionRange.start.line + 1);
        const Lsp::Symbol sym = s;
        it.run = [this, sym, open] {
            ran_ = true;
            close(true);
            later([this, sym, open] {
                if (open) move([open, sym] { open(sym); });
            });
        };
        items_.push_back(std::move(it));
    }
    if (!items_.empty()) return;
    if (!wsComplete_) *empty = "Searching the workspace's symbols…";
    else *empty = wsNote_.empty() ? "No matching symbols" : wsNote_;
}

void PalettePanel::askWorkspace(const std::string& query) {
    if (!open_ || closing_ || query != wsAsked_ || !ctx_.workspaceSymbols) return;
    wsQuestions_++;
    const unsigned session = session_;
    std::weak_ptr<int> alive = life_;
    ctx_.workspaceSymbols(query, [this, alive, session, query](
                                     std::vector<Lsp::Symbol> symbols,
                                     std::vector<std::string> paths, bool complete,
                                     const std::string& note) {
        if (alive.expired() || !open_ || session != session_ || query != wsAsked_) return;
        // A partial answer with nothing in it leaves the last list up, so a
        // slow server never empties what another has filled.
        if (!symbols.empty() || complete) {
            wsSymbols_ = std::move(symbols);
            wsPaths_ = std::move(paths);
        }
        wsComplete_ = complete;
        wsNote_ = note;
        const std::string text = gtk_editable_get_text(GTK_EDITABLE(entry_));
        if (CommandPalette::parse(Lsp::toUtf16(text)).mode ==
            CommandPalette::Mode::WorkspaceSymbols)
            refilter();
    });
}

// Out of "#", or closed: no question waits, and those out are cancelled.
void PalettePanel::stopWorkspace() {
    if (wsTimer_) g_source_remove(wsTimer_);
    wsTimer_ = 0;
    if (wsAsked_.empty()) return;
    wsAsked_.clear();
    if (ctx_.cancelWorkspaceSymbols) ctx_.cancelWorkspaceSymbols();
}

// Where the editor is, before the first symbol is shown in it: the document
// on show, its selection and its scroll. Taken then rather than when the
// palette opens, once the editor has stopped settling its scroll (a tab just
// shown is still coming back to its place for a moment, and would otherwise
// undo the preview's scroll, or the putting back).
void PalettePanel::capturePlace() {
    editor_->stopSettling();
    place_ = Place();
    Document* d = editor_->active();
    if (!d) return;
    GtkTextBuffer* b = editor_->buffer();
    GtkTextIter ins, bound;
    gtk_text_buffer_get_iter_at_mark(b, &ins, gtk_text_buffer_get_insert(b));
    gtk_text_buffer_get_iter_at_mark(b, &bound, gtk_text_buffer_get_selection_bound(b));
    GtkScrollable* s = GTK_SCROLLABLE(editor_->textView());
    place_.docId = d->id();
    place_.buffer = b;
    place_.insert = gtk_text_iter_get_offset(&ins);
    place_.bound = gtk_text_iter_get_offset(&bound);
    place_.v = gtk_adjustment_get_value(gtk_scrollable_get_vadjustment(s));
    place_.h = gtk_adjustment_get_value(gtk_scrollable_get_hadjustment(s));
    place_.valid = true;
}

// Back as it was, when the same document still shows the same buffer.
void PalettePanel::endPreview() {
    previewed_ = false;
    Document* d = editor_->active();
    if (!place_.valid || !d || d->id() != place_.docId || editor_->buffer() != place_.buffer)
        return;
    GtkTextBuffer* b = place_.buffer;
    GtkTextIter ins, bound;
    gtk_text_buffer_get_iter_at_offset(b, &ins, place_.insert);
    gtk_text_buffer_get_iter_at_offset(b, &bound, place_.bound);
    gtk_text_buffer_select_range(b, &ins, &bound);
    GtkScrollable* s = GTK_SCROLLABLE(editor_->textView());
    gtk_adjustment_set_value(gtk_scrollable_get_vadjustment(s), place_.v);
    gtk_adjustment_set_value(gtk_scrollable_get_hadjustment(s), place_.h);
}

// Show a symbol while the list is on it, the keyboard staying in the field:
// its name selected and in view, or in a Markdown page its heading at the
// top.
void PalettePanel::previewSymbol(const Lsp::Symbol& s) {
    Document* d = editor_->active();
    if (!d || d->id() != openDocId_ || !symbolsAvailable()) return;
    if (!place_.valid) capturePlace();
    GtkTextView* tv = GTK_TEXT_VIEW(editor_->textView());
    if (d->showsPreview()) {
        GtkTextBuffer* b = editor_->buffer();
        const int off = d->pageInfo().offsetForLine(s.selectionRange.start.line,
                                                    gtk_text_buffer_get_char_count(b));
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_offset(b, &it, off);
        scrollTo(tv, &it, 8);
        return;
    }
    GtkTextBuffer* b = d->buffer();
    GtkTextIter a, e;
    iterAt(b, s.selectionRange.start, &a);
    iterAt(b, s.selectionRange.end, &e);
    gtk_text_buffer_select_range(b, &a, &e);
    scrollTo(tv, &a, -1);
}

// Go there for good, the palette closed: the name selected with the
// keyboard in the editor (a Markdown page: scrolled to, the keyboard in it).
void PalettePanel::goToSymbol(const Lsp::Symbol& s) {
    Document* d = editor_->active();
    if (!d || d->id() != openDocId_ || !symbolsAvailable()) return;
    if (d->showsPreview()) {
        previewSymbol(s);
        editor_->focusActiveDocument();
        return;
    }
    GtkTextBuffer* b = d->buffer();
    GtkTextIter a, e;
    iterAt(b, s.selectionRange.start, &a);
    iterAt(b, s.selectionRange.end, &e);
    if (onScreen(GTK_TEXT_VIEW(editor_->textView()), &a)) {
        // Left where it is when it shows, as the Mac does.
        gtk_text_buffer_select_range(b, &a, &e);
        gtk_widget_grab_focus(editor_->textView());
        return;
    }
    const int line = gtk_text_iter_get_line(&a);
    const std::size_t from = (std::size_t)gtk_text_iter_get_line_index(&a);
    const std::size_t to = gtk_text_iter_get_line(&e) == line
                               ? (std::size_t)gtk_text_iter_get_line_index(&e)
                               : lineText(b, line).size();
    editor_->revealLine(line + 1, from, to > from ? to - from : 0);
}

// ------------------------------------------------------------ the list
void PalettePanel::refilter() {
    const std::string text = gtk_editable_get_text(GTK_EDITABLE(entry_));
    CommandPalette::Parsed p = CommandPalette::parse(Lsp::toUtf16(text));
    if (pick_) p.mode = CommandPalette::Mode::Files;   // a pick list has no prefixes
    items_.clear();
    std::string empty;
    if (pick_) pickItems(&empty);
    else switch (p.mode) {
    case CommandPalette::Mode::Commands: commandItems(p.query, &empty); break;
    case CommandPalette::Mode::Line: lineItem(p); break;
    case CommandPalette::Mode::Symbols: symbolItems(p.query, p.byKind, &empty); break;
    case CommandPalette::Mode::WorkspaceSymbols: workspaceItems(p.query, &empty); break;
    case CommandPalette::Mode::Files: fileItems(p.query, &empty); break;
    }
    // Out of "@", the editor goes back to where it was; out of "#", the
    // servers' questions are cancelled.
    if (p.mode != CommandPalette::Mode::Symbols && previewed_) endPreview();
    if (p.mode != CommandPalette::Mode::WorkspaceSymbols) stopWorkspace();
    int first = -1;
    for (size_t i = 0; i < items_.size() && first < 0; i++)
        if (!items_[i].heading) first = (int)i;
    if (p.mode == CommandPalette::Mode::Symbols && preselect_ >= 0 &&
        preselect_ < (int)items_.size())
        first = preselect_;
    selected_ = first;

    std::vector<std::string> indexes;
    indexes.reserve(items_.size());
    for (size_t i = 0; i < items_.size(); i++) indexes.push_back(std::to_string(i));
    std::vector<const char*> strings;
    strings.reserve(indexes.size() + 1);
    for (const std::string& s : indexes) strings.push_back(s.c_str());
    strings.push_back(nullptr);
    gtk_string_list_splice(model_, 0, g_list_model_get_n_items(G_LIST_MODEL(model_)),
                           strings.data());
    gtk_label_set_text(GTK_LABEL(empty_), empty.c_str());
    gtk_widget_set_visible(empty_, items_.empty());
    gtk_widget_set_visible(scroll_, !items_.empty());
    if (selected_ >= 0) gtk_list_view_scroll_to(GTK_LIST_VIEW(list_), (guint)selected_,
                                                GTK_LIST_SCROLL_NONE, nullptr);
    // Typing in "@" shows the best match in the editor, as VS Code does.
    if (p.mode == CommandPalette::Mode::Symbols && !p.query.empty() && selected_ >= 0 &&
        items_[(size_t)selected_].preview)
        items_[(size_t)selected_].preview();
}

void PalettePanel::markRow(int row, bool on) {
    auto it = bound_.find(row);
    if (it == bound_.end()) return;
    if (on) gtk_widget_add_css_class(it->second, "selected");
    else gtk_widget_remove_css_class(it->second, "selected");
}

// Up and Down wrap, as VS Code's list does, stepping over the headings of
// "@:" in the direction moved.
void PalettePanel::select(int row) {
    const int n = (int)items_.size();
    if (n == 0) return;
    const int step = row < selected_ ? -1 : 1;
    row = ((row % n) + n) % n;
    for (int k = 0; k < n && items_[(size_t)row].heading; k++) row = ((row + step) % n + n) % n;
    if (items_[(size_t)row].heading) return;
    const int old = selected_;
    selected_ = row;
    markRow(old, false);
    markRow(row, true);
    gtk_list_view_scroll_to(GTK_LIST_VIEW(list_), (guint)row, GTK_LIST_SCROLL_NONE, nullptr);
    if (row != old && items_[(size_t)row].preview) {
        auto preview = items_[(size_t)row].preview;
        preview();
    }
}

// Page moves stop at the ends instead of wrapping.
void PalettePanel::page(int delta) {
    const int n = (int)items_.size();
    if (n == 0) return;
    select(std::max(0, std::min(n - 1, selected_ + delta)));
}

void PalettePanel::runRow(int row) {
    if (row < 0 || row >= (int)items_.size() || !items_[(size_t)row].run) {
        gtk_widget_error_bell(entry_);
        return;
    }
    // A copy: running it closes the palette, which empties items_.
    auto run = items_[(size_t)row].run;
    run();
}

void PalettePanel::later(std::function<void()> fn) {
    auto* l = new Later{life_, std::move(fn)};
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        [](gpointer p) -> gboolean {
            auto* l = static_cast<Later*>(p);
            if (!l->alive.expired()) l->fn();
            return G_SOURCE_REMOVE;
        },
        l, [](gpointer p) { delete static_cast<Later*>(p); });
}

// ------------------------------------------------------------ signals
void PalettePanel::onSetup(GtkSignalListItemFactory*, GtkListItem* item, gpointer) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(row, "minicode-palette-row");
    GtkWidget* label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
    gtk_widget_set_hexpand(label, TRUE);
    GtkWidget* keys = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(keys), 1.0f);
    gtk_widget_add_css_class(keys, "minicode-palette-keys");
    gtk_box_append(GTK_BOX(row), label);
    gtk_box_append(GTK_BOX(row), keys);
    // A click runs the row; the press is claimed so the list never takes
    // the keyboard or a selection of its own.
    GtkGesture* click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick* g, int, double, double,
                                                       gpointer) {
                         gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
                     }),
                     nullptr);
    g_signal_connect(click, "released", G_CALLBACK(onRowReleased), nullptr);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
    gtk_list_item_set_child(item, row);
    gtk_list_item_set_focusable(item, FALSE);
    gtk_list_item_set_activatable(item, FALSE);
    gtk_list_item_set_selectable(item, FALSE);
}

void PalettePanel::onBind(GtkSignalListItemFactory*, GtkListItem* item, gpointer selfp) {
    PalettePanel* self = static_cast<PalettePanel*>(selfp);
    GtkWidget* row = gtk_list_item_get_child(item);
    GtkStringObject* so = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
    if (!row || !so) return;
    const int index = atoi(gtk_string_object_get_string(so));
    if (index < 0 || index >= (int)self->items_.size()) return;
    const Item& it = self->items_[(size_t)index];
    GtkWidget* label = gtk_widget_get_first_child(row);
    GtkWidget* keys = gtk_widget_get_next_sibling(label);
    gtk_label_set_markup(GTK_LABEL(label), it.markup.c_str());
    // A command's menu and a file's folder give way first; a symbol keeps
    // its name and loses what contains it.
    gtk_label_set_ellipsize(GTK_LABEL(label), it.symbol ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_START);
    gtk_label_set_text(GTK_LABEL(keys), it.keys.c_str());
    if (it.heading) gtk_widget_add_css_class(row, "heading");
    else gtk_widget_remove_css_class(row, "heading");
    if (index == self->selected_) gtk_widget_add_css_class(row, "selected");
    else gtk_widget_remove_css_class(row, "selected");
    g_object_set_data(G_OBJECT(row), "mc-index", GINT_TO_POINTER(index + 1));
    self->bound_[index] = row;
}

void PalettePanel::onUnbind(GtkSignalListItemFactory*, GtkListItem* item, gpointer selfp) {
    PalettePanel* self = static_cast<PalettePanel*>(selfp);
    GtkWidget* row = gtk_list_item_get_child(item);
    if (!row) return;
    const int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "mc-index")) - 1;
    auto it = self->bound_.find(index);
    if (it != self->bound_.end() && it->second == row) self->bound_.erase(it);
    g_object_set_data(G_OBJECT(row), "mc-index", nullptr);
}

void PalettePanel::onRowReleased(GtkGestureClick* g, int, double, double, gpointer) {
    GtkWidget* row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    GtkWidget* list = gtk_widget_get_ancestor(row, GTK_TYPE_LIST_VIEW);
    PalettePanel* self =
        list ? static_cast<PalettePanel*>(g_object_get_data(G_OBJECT(list), "mc-palette")) : nullptr;
    if (!self || !self->open_) return;
    const int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "mc-index")) - 1;
    if (index >= 0 && index < (int)self->items_.size() && self->items_[(size_t)index].heading) return;
    self->runRow(index);
}

void PalettePanel::onSheetPressed(GtkGestureClick* g, int, double, double, gpointer selfp) {
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    static_cast<PalettePanel*>(selfp)->close(true);
}

gboolean PalettePanel::onKey(GtkEventControllerKey*, guint keyval, guint, GdkModifierType,
                             gpointer selfp) {
    PalettePanel* self = static_cast<PalettePanel*>(selfp);
    switch (keyval) {
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up: self->select(self->selected_ - 1); return TRUE;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down: self->select(self->selected_ + 1); return TRUE;
    case GDK_KEY_Page_Up:
    case GDK_KEY_KP_Page_Up: self->page(-(kMaxRows - 1)); return TRUE;
    case GDK_KEY_Page_Down:
    case GDK_KEY_KP_Page_Down: self->page(kMaxRows - 1); return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter: self->runRow(self->selected_); return TRUE;
    case GDK_KEY_Escape: self->close(true); return TRUE;
    // Tab would move the keyboard on and leave the palette open without it.
    case GDK_KEY_Tab:
    case GDK_KEY_KP_Tab:
    case GDK_KEY_ISO_Left_Tab: return TRUE;
    default: return FALSE;
    }
}

void PalettePanel::onChanged(GtkEditable*, gpointer selfp) {
    PalettePanel* self = static_cast<PalettePanel*>(selfp);
    if (self->open_) self->refilter();
}

// The keyboard went elsewhere (a click in the tree, Alt+2 to another tab):
// the palette goes, and the keyboard stays where it went. Looked at again a
// turn later, as the Mac does, so a focus passing through settles first.
void PalettePanel::onFocusWidget(GObject*, GParamSpec*, gpointer selfp) {
    PalettePanel* self = static_cast<PalettePanel*>(selfp);
    if (!self->open_ || self->closing_) return;
    auto outside = [self] {
        GtkWidget* f = gtk_root_get_focus(GTK_ROOT(self->window_));
        return !(f && (f == self->box_ || gtk_widget_is_ancestor(f, self->box_)));
    };
    if (!outside()) return;
    const unsigned session = self->session_;
    self->later([self, session, outside] {
        if (self->open_ && !self->closing_ && self->session_ == session && outside())
            self->close(false);
    });
}

// The box at the top centre of the panes, at most 600 pixels wide and 40
// narrower than they are, as tall as it asks up to their height. Other
// overlay children (the sheet, the hints) are placed as GTK places them.
gboolean PalettePanel::onChildPosition(GtkOverlay* overlay, GtkWidget* child,
                                       GdkRectangle* allocation, gpointer selfp) {
    PalettePanel* self = static_cast<PalettePanel*>(selfp);
    if (child != self->box_) return FALSE;
    const int W = gtk_widget_get_width(GTK_WIDGET(overlay));
    const int H = gtk_widget_get_height(GTK_WIDGET(overlay));
    int minW = 0;
    gtk_widget_measure(child, GTK_ORIENTATION_HORIZONTAL, -1, &minW, nullptr, nullptr, nullptr);
    const int w = std::max(minW, std::min(600, std::max(200, W - 40)));
    int minH = 0, natH = 0;
    gtk_widget_measure(child, GTK_ORIENTATION_VERTICAL, w, &minH, &natH, nullptr, nullptr);
    allocation->x = std::max(0, (W - w) / 2);
    allocation->y = 8;
    allocation->width = w;
    allocation->height = std::max(minH, std::min(natH, H - 16));
    return TRUE;
}

// ------------------------------------------------------------ the checks
std::string PalettePanel::text() const { return gtk_editable_get_text(GTK_EDITABLE(entry_)); }

std::vector<std::string> PalettePanel::labels() const {
    std::vector<std::string> out;
    for (const Item& it : items_) out.push_back(it.label);
    return out;
}

std::string PalettePanel::emptyText() const {
    return gtk_widget_get_visible(empty_) ? gtk_label_get_text(GTK_LABEL(empty_)) : "";
}

std::vector<std::string> PalettePanel::keys() const {
    std::vector<std::string> out;
    for (const Item& it : items_) out.push_back(it.keys);
    return out;
}

std::vector<std::string> PalettePanel::details() const {
    std::vector<std::string> out;
    for (const Item& it : items_) out.push_back(it.detail);
    return out;
}
