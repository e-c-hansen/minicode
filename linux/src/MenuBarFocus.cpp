// MenuBarFocus.cpp — see MenuBarFocus.h.
#include "MenuBarFocus.h"

#include <vector>

namespace {

const char* kStateKey = "minicode-menubar-focus";
const char* kHookedKey = "minicode-menubar-focus-hooked";

struct State {
    // The keyboard's place outside every menu, last seen, then its
    // ancestors up to the window: when a refresh has made the widget again
    // meanwhile (the Source Control panel's rows), the keyboard goes to the
    // nearest of them still on show.
    std::vector<GWeakRef*> last;
    GtkWidget* bar = nullptr;    // the window's menu bar, once found (not owned)
    ~State() { forget(); }
    void forget() {
        for (GWeakRef* r : last) {
            g_weak_ref_clear(r);
            g_free(r);
        }
        last.clear();
    }
    void remember(GtkWidget* focus) {
        forget();
        for (GtkWidget* w = focus; w; w = gtk_widget_get_parent(w)) {
            auto* r = g_new0(GWeakRef, 1);
            g_weak_ref_init(r, w);
            last.push_back(r);
        }
    }
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

// The keyboard to the place remembered, or the nearest of its ancestors
// still in this window and on show, as GTK does when the widget with the
// keyboard is hidden (a list view gives it to its cursor's row).
void giveBack(GtkWindow* window, State* s) {
    for (size_t i = 0; i < s->last.size(); i++) {
        GtkWidget* w = GTK_WIDGET(g_weak_ref_get(s->last[i]));
        if (!w) continue;
        bool done = false;
        if (gtk_widget_get_root(w) == GTK_ROOT(window) && gtk_widget_get_mapped(w)) {
            if (i == 0) {
                gtk_widget_grab_focus(w);
                done = true;
            } else if (w != GTK_WIDGET(window)) {
                done = gtk_widget_grab_focus(w) ||
                       gtk_widget_child_focus(w, GTK_DIR_TAB_FORWARD);
            }
        }
        g_object_unref(w);
        if (done) return;
    }
}

// After the turn a menu of the bar closed in (the command it chose has run
// by then): the keyboard back where it was, when it is still in the bar.
gboolean restore(gpointer p) {
    auto* ref = static_cast<GWeakRef*>(p);
    GtkWindow* window = GTK_WINDOW(g_weak_ref_get(ref));
    if (!window) return G_SOURCE_REMOVE;
    auto* s = static_cast<State*>(g_object_get_data(G_OBJECT(window), kStateKey));
    GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(window));
    if (s && s->bar && focus && inside(focus, s->bar) && !anyOpen(s->bar)) giveBack(window, s);
    g_object_unref(window);
    return G_SOURCE_REMOVE;
}

void queueRestore(GtkRoot* root) {
    auto* ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, root);
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, restore, ref, [](gpointer p) {
        g_weak_ref_clear(static_cast<GWeakRef*>(p));
        g_free(p);
    });
}

void onClosed(GtkPopover* popover, gpointer) {
    if (GtkRoot* root = gtk_widget_get_root(GTK_WIDGET(popover))) queueRestore(root);
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

GtkWidget* bar(GtkWindow* window) { return findBar(GTK_WIDGET(window)); }

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
            if (!inPopover(focus)) st->remember(focus);
            return;
        }
        findAndHook(GTK_WINDOW(w), st);   // menus the bar made since
        // A row of a menu no longer on show: GTK's model button opens its
        // submenu 80 ms after the pointer last moved over it, by a timer
        // the menu closing does not stop, and that grabs the keyboard for
        // the row. After a quick click it fired once the menu had closed
        // (and the keyboard been given back), so typing went nowhere.
        if (!gtk_widget_get_mapped(focus) && !anyOpen(st->bar)) queueRestore(GTK_ROOT(w));
    }), nullptr);
}

}  // namespace MenuBarFocus
