// MenuBarFocus.cpp — see MenuBarFocus.h.
#include "MenuBarFocus.h"

namespace {

const char* kStateKey = "minicode-menubar-focus";
const char* kHookedKey = "minicode-menubar-focus-hooked";

struct State {
    GWeakRef last;               // the keyboard's place outside every menu, last seen
    GtkWidget* bar = nullptr;    // the window's menu bar, once found (not owned)
    State() { g_weak_ref_init(&last, nullptr); }
    ~State() { g_weak_ref_clear(&last); }
};

bool inside(GtkWidget* w, GtkWidget* area) {
    for (; w; w = gtk_widget_get_parent(w))
        if (w == area) return true;
    return false;
}

bool inPopover(GtkWidget* w) {
    for (; w; w = gtk_widget_get_parent(w))
        if (GTK_IS_POPOVER(w)) return true;
    return false;
}

GtkWidget* findBar(GtkWidget* w) {
    if (!w) return nullptr;
    if (GTK_IS_POPOVER_MENU_BAR(w)) return w;
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = findBar(c)) return hit;
    return nullptr;
}

bool anyOpen(GtkWidget* w) {
    if (GTK_IS_POPOVER(w) && gtk_widget_get_visible(w)) return true;
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (anyOpen(c)) return true;
    return false;
}

// After the turn a menu of the bar closed in (the command it chose has run
// by then): the keyboard back where it was, when it is still in the bar.
gboolean restore(gpointer p) {
    auto* ref = static_cast<GWeakRef*>(p);
    GtkWindow* window = GTK_WINDOW(g_weak_ref_get(ref));
    if (!window) return G_SOURCE_REMOVE;
    auto* s = static_cast<State*>(g_object_get_data(G_OBJECT(window), kStateKey));
    GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(window));
    if (s && s->bar && focus && inside(focus, s->bar) && !anyOpen(s->bar)) {
        GtkWidget* last = GTK_WIDGET(g_weak_ref_get(&s->last));
        if (last && gtk_widget_get_root(last) == GTK_ROOT(window) && gtk_widget_get_mapped(last))
            gtk_widget_grab_focus(last);
        if (last) g_object_unref(last);
    }
    g_object_unref(window);
    return G_SOURCE_REMOVE;
}

void onClosed(GtkPopover* popover, gpointer) {
    GtkRoot* root = gtk_widget_get_root(GTK_WIDGET(popover));
    if (!root) return;
    auto* ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, root);
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, restore, ref, [](gpointer p) {
        g_weak_ref_clear(static_cast<GWeakRef*>(p));
        g_free(p);
    });
}

void hookMenus(GtkWidget* w) {
    if (GTK_IS_POPOVER(w) && !g_object_get_data(G_OBJECT(w), kHookedKey)) {
        g_object_set_data(G_OBJECT(w), kHookedKey, GINT_TO_POINTER(1));
        g_signal_connect(w, "closed", G_CALLBACK(onClosed), nullptr);
    }
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        hookMenus(c);
}

void findAndHook(GtkWindow* window, State* s) {
    if (!s->bar) s->bar = findBar(GTK_WIDGET(window));
    if (s->bar) hookMenus(s->bar);
}

}  // namespace

namespace MenuBarFocus {

void attach(GtkWindow* window) {
    if (!window || g_object_get_data(G_OBJECT(window), kStateKey)) return;
    auto* s = new State;
    g_object_set_data_full(G_OBJECT(window), kStateKey, s,
                           [](gpointer p) { delete static_cast<State*>(p); });
    g_signal_connect(window, "map", G_CALLBACK(+[](GtkWidget* w, gpointer) {
        auto* st = static_cast<State*>(g_object_get_data(G_OBJECT(w), kStateKey));
        if (st) findAndHook(GTK_WINDOW(w), st);
    }), nullptr);
    g_signal_connect(window, "notify::focus-widget", G_CALLBACK(+[](GObject* w, GParamSpec*,
                                                                    gpointer) {
        auto* st = static_cast<State*>(g_object_get_data(w, kStateKey));
        if (!st) return;
        GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(w));
        if (!focus) return;
        if (!st->bar) findAndHook(GTK_WINDOW(w), st);
        if (!st->bar || !inside(focus, st->bar)) {
            if (!inPopover(focus)) g_weak_ref_set(&st->last, focus);
            return;
        }
        findAndHook(GTK_WINDOW(w), st);   // menus the bar made since
    }), nullptr);
}

}  // namespace MenuBarFocus
