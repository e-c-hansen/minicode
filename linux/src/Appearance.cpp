// Appearance.cpp — see Appearance.h.
#include "Appearance.h"

#include "AppSettings.h"
#include "Document.h"
#include "Editor.h"
#include "Utf8Offsets.h"

#ifdef MINICODE_ENABLE_TERMINAL
#include <vte/vte.h>
#endif

#include <algorithm>
#include <map>
#include <memory>

namespace Appearance {

// ------------------------------------------------------------ the item

void addItem(GMenu* menu, const std::string& surface) {
    if (!menu || !Settings::appearanceSurface(surface)) return;
    GMenu* section = g_menu_new();
    GMenuItem* item = g_menu_item_new(kItemLabel, nullptr);
    g_menu_item_set_action_and_target_value(item, (std::string("win.") + kAction).c_str(),
                                            g_variant_new_string(surface.c_str()));
    g_menu_append_item(section, item);
    g_object_unref(item);
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
    g_object_unref(section);
}

GMenuModel* menuFor(const std::string& surface) {
    GMenu* menu = g_menu_new();
    addItem(menu, surface);
    return G_MENU_MODEL(menu);
}

namespace {

// A widget's right click menu holding only the item, made at its first
// right click. The popover is the widget's child only by
// gtk_widget_set_parent, so it comes off when the widget goes; a reference
// of our own keeps it alive until then, since a box's dispose unparents
// its children before the widget's "destroy" is emitted.
struct Attached {
    std::string surface;
    GtkWidget* popover = nullptr;   // a reference of its own
    ~Attached() { drop(); }
    void drop() {
        if (!popover) return;
        if (gtk_widget_get_parent(popover)) gtk_widget_unparent(popover);
        g_object_unref(popover);
        popover = nullptr;
    }
};

void onAttachedPressed(GtkGestureClick* g, int, double x, double y, gpointer p) {
    auto* a = static_cast<Attached*>(p);
    GtkWidget* w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    // A new popover each time, as the tab strip's: shown a second time, one
    // popover menu kept the keyboard on its item after a click and never ran
    // it (the adjust_settings checks, one run in two).
    a->drop();
    GMenuModel* model = menuFor(a->surface);
    a->popover = GTK_WIDGET(g_object_ref_sink(gtk_popover_menu_new_from_model(model)));
    g_object_unref(model);
    gtk_popover_set_has_arrow(GTK_POPOVER(a->popover), FALSE);
    gtk_widget_set_halign(a->popover, GTK_ALIGN_START);
    gtk_widget_set_parent(a->popover, w);
    GdkRectangle at = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(a->popover), &at);
    gtk_popover_popup(GTK_POPOVER(a->popover));
}

void addTextMenusUnder(GtkWidget* w, GMenuModel* model) {
    if (GTK_IS_POPOVER(w)) return;
    if (GTK_IS_ENTRY(w)) {
        if (!gtk_entry_get_extra_menu(GTK_ENTRY(w))) gtk_entry_set_extra_menu(GTK_ENTRY(w), model);
        return;
    }
    if (GTK_IS_TEXT(w)) {
        if (!gtk_text_get_extra_menu(GTK_TEXT(w))) gtk_text_set_extra_menu(GTK_TEXT(w), model);
        return;
    }
    if (GTK_IS_TEXT_VIEW(w) && !gtk_text_view_get_extra_menu(GTK_TEXT_VIEW(w)))
        gtk_text_view_set_extra_menu(GTK_TEXT_VIEW(w), model);
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        addTextMenusUnder(c, model);
}

// The editor's view: one menu per surface, the one for what it shows set
// before its own right click handling builds the popover.
struct EditorMenu {
    std::function<std::string()> surfaceNow;
    std::map<std::string, GMenuModel*> models;
    std::string current;
    ~EditorMenu() {
        for (auto& [s, m] : models) g_object_unref(m);
    }
    void follow(GtkWidget* view) {
        const std::string s = surfaceNow ? surfaceNow() : std::string("editor");
        if (s == current) return;
        GMenuModel*& m = models[s];
        if (!m) m = menuFor(s);
        current = s;
        gtk_text_view_set_extra_menu(GTK_TEXT_VIEW(view), m);
    }
};

const char* kEditorMenuKey = "minicode-adjust-editor-menu";

}  // namespace

void attachMenu(GtkWidget* widget, const std::string& surface) {
    if (!widget || !Settings::appearanceSurface(surface)) return;
    auto* a = new Attached{surface, nullptr};
    GtkGesture* g = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), GDK_BUTTON_SECONDARY);
    g_signal_connect_data(g, "pressed", G_CALLBACK(onAttachedPressed), a,
                          [](gpointer d, GClosure*) { delete static_cast<Attached*>(d); },
                          GConnectFlags(0));
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(g));
    // Emitted before the widget lets go of its controllers, so `a` is alive.
    g_signal_connect(widget, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer d) {
        static_cast<Attached*>(d)->drop();
    }), a);
    addToTextMenus(widget, surface);
}

void addToTextMenus(GtkWidget* root, const std::string& surface) {
    if (!root || !Settings::appearanceSurface(surface)) return;
    GMenuModel* model = menuFor(surface);
    addTextMenusUnder(root, model);
    g_object_unref(model);
}

void attachEditorMenu(GtkWidget* view, std::function<std::string()> surfaceNow) {
    if (!view || !GTK_IS_TEXT_VIEW(view)) return;
    auto* em = new EditorMenu;
    em->surfaceNow = std::move(surfaceNow);
    // Freed with the view, after its handlers and controllers are gone.
    g_object_set_data_full(G_OBJECT(view), kEditorMenuKey, em,
                           [](gpointer d) { delete static_cast<EditorMenu*>(d); });
    em->follow(view);
    // A Markdown page and its source are two buffers (D3): the menu follows
    // the view from one to the other, for the keyboard's menu key too.
    g_signal_connect(view, "notify::buffer", G_CALLBACK(+[](GObject* v, GParamSpec*, gpointer d) {
        static_cast<EditorMenu*>(d)->follow(GTK_WIDGET(v));
    }), em);
    // And once more as a right click starts (capture phase, before the
    // view's own handling builds its menu), never taking the click.
    GtkGesture* g = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(g), GTK_PHASE_CAPTURE);
    g_signal_connect(g, "pressed", G_CALLBACK(+[](GtkGestureClick* gc, int, double, double,
                                                  gpointer d) {
        static_cast<EditorMenu*>(d)->follow(
            gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gc)));
    }), em);
    gtk_widget_add_controller(view, GTK_EVENT_CONTROLLER(g));
}

std::string editorMenuSurface(GtkWidget* view) {
    auto* em = view ? static_cast<EditorMenu*>(g_object_get_data(G_OBJECT(view), kEditorMenuKey))
                    : nullptr;
    return em ? em->current : std::string();
}

#ifdef MINICODE_ENABLE_TERMINAL
void attachTerminalMenu(GtkWidget* vte) {
    if (!vte || !VTE_IS_TERMINAL(vte)) return;
    // Copy, Paste and Select All, as the Mac's grid offers, on actions of
    // the terminal's own; the window's keys for them stay Ctrl+Shift+C and V.
    GSimpleActionGroup* group = g_simple_action_group_new();
    auto add = [&](const char* name, GCallback run) {
        GSimpleAction* a = g_simple_action_new(name, nullptr);
        g_signal_connect(a, "activate", run, vte);
        g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(a));
        g_object_unref(a);
    };
    add("copy", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer t) {
        vte_terminal_copy_clipboard_format(VTE_TERMINAL(t), VTE_FORMAT_TEXT);
    }));
    add("paste", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer t) {
        vte_terminal_paste_clipboard(VTE_TERMINAL(t));
    }));
    add("selectall", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer t) {
        vte_terminal_select_all(VTE_TERMINAL(t));
    }));
    gtk_widget_insert_action_group(vte, "term", G_ACTION_GROUP(group));
    g_object_unref(group);

    GMenu* menu = g_menu_new();
    GMenu* edit = g_menu_new();
    g_menu_append(edit, "Copy", "term.copy");
    g_menu_append(edit, "Paste", "term.paste");
    g_menu_append(edit, "Select All", "term.selectall");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(edit));
    g_object_unref(edit);
    addItem(menu, "terminal");
    vte_terminal_set_context_menu_model(VTE_TERMINAL(vte), G_MENU_MODEL(menu));
    g_object_unref(menu);
}
#endif

#ifdef MINICODE_ENABLE_BROWSER
void addToWebMenu(WebKitContextMenu* menu, GtkWidget* webView) {
    GtkRoot* root = webView ? gtk_widget_get_root(webView) : nullptr;
    GAction* action = root && G_IS_ACTION_MAP(root)
                          ? g_action_map_lookup_action(G_ACTION_MAP(root), kAction)
                          : nullptr;
    if (!menu || !action) return;
    if (webkit_context_menu_get_n_items(menu) > 0)
        webkit_context_menu_append(menu, webkit_context_menu_item_new_separator());
    webkit_context_menu_append(menu, webkit_context_menu_item_new_from_gaction(
                                         action, kItemLabel, g_variant_new_string("browser")));
}
#endif

// ------------------------------------------------------------ the View menu

void extendMenu(const std::string& top, GMenu* menu) {
    if (top != "View" || !menu) return;
    GMenu* section = g_menu_new();

    // The same as a right click's Adjust Settings, for each part of the window.
    GMenu* adjust = g_menu_new();
    for (const AppearanceSurface& s : Settings::appearanceSurfaces()) {
        if (s.placeholder) continue;
        GMenuItem* item = g_menu_item_new(s.title, nullptr);
        g_menu_item_set_action_and_target_value(item, (std::string("win.") + kAction).c_str(),
                                                g_variant_new_string(s.id));
        g_menu_append_item(adjust, item);
        g_object_unref(item);
    }
    g_menu_append_submenu(section, "Adjust Settings", G_MENU_MODEL(adjust));
    g_object_unref(adjust);

    // VS Code's built-in themes, dark then light; the choice is the settings
    // file's theme line, so it stays one plain line there.
    GMenu* themes = g_menu_new();
    GMenu* part = nullptr;
    bool lastDark = true;
    for (const Theme& t : Settings::themes()) {
        if (!part || (lastDark && !t.dark)) {
            if (part) {
                g_menu_append_section(themes, nullptr, G_MENU_MODEL(part));
                g_object_unref(part);
            }
            part = g_menu_new();
        }
        lastDark = t.dark;
        GMenuItem* item = g_menu_item_new(t.title, nullptr);
        g_menu_item_set_action_and_target_value(item, "app.theme", g_variant_new_string(t.id));
        g_menu_append_item(part, item);
        g_object_unref(item);
    }
    if (part) {
        g_menu_append_section(themes, nullptr, G_MENU_MODEL(part));
        g_object_unref(part);
    }
    g_menu_append_submenu(section, "Theme", G_MENU_MODEL(themes));
    g_object_unref(themes);

    g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
    g_object_unref(section);
}

// ------------------------------------------------------------ the settings file

namespace {

// The tint's fade, the Mac's: 0.24 for 2.4 s, then 0.16, 0.09 and 0.04 a
// few tenths apart, then gone at 3.3 s. A later Adjust Settings takes over.
guint gTintGeneration = 0;

GtkTextTag* tintTag(GtkTextBuffer* buffer) {
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buffer);
    GtkTextTag* tag = gtk_text_tag_table_lookup(table, kTintTag);
    if (!tag) {
        tag = gtk_text_buffer_create_tag(buffer, kTintTag, nullptr);
        // Under every other tag, so a swatch keeps its own color.
        gtk_text_tag_set_priority(tag, 0);
    }
    return tag;
}

void setTintAlpha(GtkTextTag* tag, double alpha) {
    const GdkRGBA tint = {0x4E / 255.0f, 0xA1 / 255.0f, 0xF7 / 255.0f, (float)alpha};
    g_object_set(tag, "background-rgba", &tint, NULL);
}

void clearTint(GtkTextBuffer* buffer) {
    GtkTextTag* tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), kTintTag);
    if (!tag) return;
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer, &a, &b);
    gtk_text_buffer_remove_tag(buffer, tag, &a, &b);
}

struct FadeStep {
    guint generation;
    GtkTextBuffer* buffer;   // a reference of its own
    double alpha;            // 0: the tint goes
};

std::string lineText(GtkTextBuffer* buffer, int line, GtkTextIter* start, GtkTextIter* end) {
    gtk_text_buffer_get_iter_at_line(buffer, start, line);
    *end = *start;
    if (!gtk_text_iter_ends_line(end)) gtk_text_iter_forward_to_line_end(end);
    char* raw = gtk_text_buffer_get_text(buffer, start, end, FALSE);
    std::string out = raw ? raw : "";
    g_free(raw);
    return out;
}

void tintLines(GtkTextBuffer* buffer, const std::vector<int>& lines) {
    const guint generation = ++gTintGeneration;
    clearTint(buffer);
    GtkTextTag* tag = tintTag(buffer);
    setTintAlpha(tag, 0.24);
    const int count = gtk_text_buffer_get_line_count(buffer);
    for (int line : lines) {
        if (line < 0 || line >= count) continue;
        GtkTextIter start, end;
        const std::u16string text = utf16::fromUtf8(lineText(buffer, line, &start, &end));
        // A color's swatch keeps its own color: the tint goes around it.
        ColorSpan swatch;
        long skip0 = (long)gtk_text_iter_get_line_offset(&end), skip1 = skip0;
        if (Settings::findColor(text, swatch)) {
            skip0 = utf16::toCharOffset(text, swatch.start);
            skip1 = utf16::toCharOffset(text, swatch.start + swatch.length);
        }
        GtkTextIter a = start, b = start;
        gtk_text_iter_set_line_offset(&b, (int)skip0);
        if (!gtk_text_iter_equal(&a, &b)) gtk_text_buffer_apply_tag(buffer, tag, &a, &b);
        a = start;
        gtk_text_iter_set_line_offset(&a, (int)skip1);
        if (gtk_text_iter_compare(&a, &end) < 0) gtk_text_buffer_apply_tag(buffer, tag, &a, &end);
    }
    const struct { guint ms; double alpha; } steps[] = {
        {2400, 0.16}, {2700, 0.09}, {3000, 0.04}, {3300, 0}};
    for (const auto& s : steps) {
        auto* step = new FadeStep{generation, GTK_TEXT_BUFFER(g_object_ref(buffer)), s.alpha};
        g_timeout_add_full(G_PRIORITY_DEFAULT, s.ms, [](gpointer p) -> gboolean {
            auto* st = static_cast<FadeStep*>(p);
            if (st->generation != gTintGeneration) return G_SOURCE_REMOVE;
            if (st->alpha <= 0) {
                clearTint(st->buffer);
            } else if (GtkTextTag* t = gtk_text_tag_table_lookup(
                           gtk_text_buffer_get_tag_table(st->buffer), kTintTag)) {
                setTintAlpha(t, st->alpha);
            }
            return G_SOURCE_REMOVE;
        }, step, [](gpointer p) {
            auto* st = static_cast<FadeStep*>(p);
            g_object_unref(st->buffer);
            delete st;
        });
    }
}

std::string bufferText(GtkTextBuffer* buffer) {
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer, &a, &b);
    char* raw = gtk_text_buffer_get_text(buffer, &a, &b, FALSE);
    std::string out = raw ? raw : "";
    g_free(raw);
    return out;
}

}  // namespace

void showSurfaceKeys(Editor* editor, const std::string& surface, const FileHost& host) {
    const AppearanceSurface* s = Settings::appearanceSurface(surface);
    Document* d = editor ? editor->active() : nullptr;
    if (!s || !d || !d->isSettingsFile() || d->kind() != Document::Kind::Text) return;
    if (d->loading()) {   // read in the background (it never is, at its size)
        d->whenLoaded([editor, d, surface, host] {
            if (editor->active() == d) showSurfaceKeys(editor, surface, host);
        });
        return;
    }
    GtkTextBuffer* buffer = d->buffer();
    // A file written before some of these keys existed has no line for
    // them: their commented-out defaults go at its end first, so the
    // swatches and sliders are there to use. One undo step; saved at once
    // only when the file had no unsaved changes (the lines are comments, so
    // nothing changes but the file).
    const std::string add = Settings::adjustSettingsAddition(bufferText(buffer), surface);
    if (!add.empty() && editor->isSource() &&
        gtk_text_view_get_editable(GTK_TEXT_VIEW(editor->textView()))) {
        const bool wasClean = !d->dirty();
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buffer, &end);
        gtk_text_buffer_begin_user_action(buffer);
        gtk_text_buffer_insert(buffer, &end, add.c_str(), (int)add.size());
        gtk_text_buffer_end_user_action(buffer);
        if (wasClean && host.save) host.save(d);
    }

    const std::string text = bufferText(buffer);
    const std::vector<int> lines = Settings::surfaceKeyLines(text, surface);
    if (lines.empty()) {
        if (host.note) host.note(std::string("No ") + s->title + " settings in this file.");
        return;
    }
    // The first key's value selected, its line in view, the keyboard there.
    GtkTextIter start, end;
    const std::string first = lineText(buffer, lines[0], &start, &end);
    const std::u16string wide = utf16::fromUtf8(first);
    ValueSpan value;
    size_t byteColumn = 0, byteLength = 0;
    if (Settings::findValue(wide, value)) {
        byteColumn = utf16::byteOffsetOfUtf16(first, value.start);
        byteLength = utf16::byteOffsetOfUtf16(first, value.start + value.length) - byteColumn;
    }
    editor->revealLine(lines[0] + 1, byteColumn, byteLength);
    tintLines(buffer, lines);
}

std::vector<int> tintedLines(GtkTextBuffer* buffer) {
    std::vector<int> out;
    GtkTextTag* tag =
        buffer ? gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), kTintTag)
               : nullptr;
    if (!tag) return out;
    GtkTextIter it;
    gtk_text_buffer_get_start_iter(buffer, &it);
    for (;;) {
        if (gtk_text_iter_has_tag(&it, tag)) {
            const int line = gtk_text_iter_get_line(&it);
            if (out.empty() || out.back() != line) out.push_back(line);
        }
        if (!gtk_text_iter_forward_to_tag_toggle(&it, tag)) break;
    }
    return out;
}

// ------------------------------------------------------------ themes

namespace {

ThemeHost gThemeHost;
GSimpleAction* gThemeAction = nullptr;

void tell(const std::string& message) {
    if (gThemeHost.note) gThemeHost.note(message);
    GtkWindow* w = gThemeHost.frontWindow ? gThemeHost.frontWindow() : nullptr;
    if (w) gtk_widget_error_bell(GTK_WIDGET(w));
}

void writeTheme(const std::string& id, bool commentColors) {
    if (!gThemeHost.settings) return;
    std::string why;
    if (!gThemeHost.settings->writeTheme(id, commentColors, &why)) tell(why);
}

}  // namespace

void installThemeAction(GtkApplication* app, ThemeHost host) {
    gThemeHost = std::move(host);
    const char* current =
        gThemeHost.settings ? gThemeHost.settings->settings().theme().id : "dark-plus";
    gThemeAction =
        g_simple_action_new_stateful("theme", G_VARIANT_TYPE_STRING, g_variant_new_string(current));
    // A row's choice is a request: the check moves when the file says so.
    g_signal_connect(gThemeAction, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant* p, gpointer) {
        if (p) chooseTheme(g_variant_get_string(p, nullptr));
    }), nullptr);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(gThemeAction));
    g_object_unref(gThemeAction);   // the application holds it
}

void settingsChanged(GtkApplication*, const Settings& settings) {
    if (gThemeAction) g_simple_action_set_state(gThemeAction,
                                                g_variant_new_string(settings.theme().id));
    applyColorScheme(settings);
}

void chooseTheme(const std::string& id) {
    AppSettings* cfg = gThemeHost.settings;
    const Theme* theme = Settings::findTheme(id);
    if (!cfg || !theme) return;
    // A tab holding the file with unsaved changes: refused before asking.
    if (AppSettings::unsavedTextCheck && AppSettings::unsavedTextCheck(cfg->path())) {
        writeTheme(id, false);   // says why
        return;
    }
    const int own = Settings::colorOverrides(cfg->fileText());
    if (own <= 0) {
        writeTheme(id, false);
        return;
    }
    GtkAlertDialog* dlg = gtk_alert_dialog_new("Your settings file sets %d color%s of its own",
                                               own, own == 1 ? "" : "s");
    const std::string detail = std::string(theme->title) +
                               " covers only what they leave. Comment them out to see the whole "
                               "theme; they stay in the file, so you can bring any of them back.";
    gtk_alert_dialog_set_detail(dlg, detail.c_str());
    const char* buttons[] = {"Use the Theme's Colors", "Keep My Colors", "Cancel", nullptr};
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_cancel_button(dlg, 2);
    gtk_alert_dialog_set_default_button(dlg, 0);
    GtkWindow* parent = gThemeHost.frontWindow ? gThemeHost.frontWindow() : nullptr;
    gtk_alert_dialog_choose(dlg, parent, nullptr, [](GObject* src, GAsyncResult* res, gpointer p) {
        std::unique_ptr<std::string> chosen(static_cast<std::string*>(p));
        const int answer = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr);
        if (answer == 0) writeTheme(*chosen, true);
        else if (answer == 1) writeTheme(*chosen, false);
    }, new std::string(id));
    g_object_unref(dlg);
}

void applyColorScheme(const Settings& settings) {
    GtkSettings* gs = gtk_settings_get_default();
    if (!gs) return;
    const bool dark = settings.isDark();
#if GTK_CHECK_VERSION(4, 20, 0)
    // What GTK's own theme reads since 4.20 (a throwaway program on the
    // ThinkPad, 2026-11-10: the Default theme went dark and light with it
    // and ignored gtk-application-prefer-dark-theme).
    int want = dark ? GTK_INTERFACE_COLOR_SCHEME_DARK : GTK_INTERFACE_COLOR_SCHEME_LIGHT;
    int now = -1;
    g_object_get(gs, "gtk-interface-color-scheme", &now, NULL);
    if (now != want) g_object_set(gs, "gtk-interface-color-scheme", want, NULL);
#else
    gboolean now = FALSE;
    g_object_get(gs, "gtk-application-prefer-dark-theme", &now, NULL);
    if (!now != !dark) g_object_set(gs, "gtk-application-prefer-dark-theme", (gboolean)dark, NULL);
#endif
}

}  // namespace Appearance
