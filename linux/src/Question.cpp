// Question.cpp — see Question.h.
#include "Question.h"

#include <algorithm>

namespace Question {

namespace {

const char* const kAsked = "mc-question";
const char* const kIndex = "mc-question-button";

struct Asked {
    std::function<void(int)> answered;
    int cancel = 0;
    bool done = false;
};

Asked* askedOf(GtkWidget* window) {
    return static_cast<Asked*>(g_object_get_data(G_OBJECT(window), kAsked));
}

// The answer, once. The window goes first, so a question asked in the
// answer (a notice after the hooks were added) is the one on top.
void answer(GtkWidget* window, int choice) {
    Asked* a = askedOf(window);
    if (!a || a->done) return;
    a->done = true;
    std::function<void(int)> f = std::move(a->answered);
    gtk_window_destroy(GTK_WINDOW(window));
    if (f) f(choice);
}

// The monitor the parent is on, else the display's first: the window keeps
// within it.
GdkRectangle screenOf(GtkWindow* parent) {
    GdkRectangle r = {0, 0, 1024, 768};
    GdkDisplay* display = parent ? gtk_widget_get_display(GTK_WIDGET(parent)) : gdk_display_get_default();
    if (!display) return r;
    GdkSurface* surface = parent && gtk_widget_get_realized(GTK_WIDGET(parent))
                              ? gtk_native_get_surface(GTK_NATIVE(parent))
                              : nullptr;
    if (GdkMonitor* m = surface ? gdk_display_get_monitor_at_surface(display, surface) : nullptr) {
        gdk_monitor_get_geometry(m, &r);
        return r;
    }
    GListModel* all = gdk_display_get_monitors(display);
    if (all && g_list_model_get_n_items(all) > 0) {
        GdkMonitor* first = GDK_MONITOR(g_list_model_get_item(all, 0));
        gdk_monitor_get_geometry(first, &r);
        g_object_unref(first);
    }
    return r;
}

// Wrapped at word breaks, and inside a word (a path) that does not fit.
GtkWidget* wrapped(const std::string& text, int maxChars) {
    GtkWidget* l = gtk_label_new(text.c_str());
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(l), maxChars);
    return l;
}

// Scrolls past `maxHeight`; as tall as its text below it.
GtkWidget* scrolling(GtkWidget* child, int maxHeight) {
    GtkWidget* s = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(s), TRUE);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(s), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(s), maxHeight);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s), child);
    return s;
}

}  // namespace

void ask(GtkWindow* parent, const Spec& spec, std::function<void(int)> answered) {
    std::vector<std::string> buttons = spec.buttons;
    if (buttons.empty()) buttons.push_back("OK");
    const int n = (int)buttons.size();
    const int cancel = spec.cancelButton >= 0 && spec.cancelButton < n ? spec.cancelButton : n - 1;
    const int fallback = spec.defaultButton >= 0 && spec.defaultButton < n ? spec.defaultButton : 0;

    // About 60 characters a line, as GTK's own alerts, fewer on a narrow
    // screen; the text and preview scroll where the screen is too short for
    // them and the buttons.
    // The buttons, margins and gaps take under 200 points; the message and
    // detail scroll together in what the preview (at most 260, as the Mac's)
    // leaves.
    const GdkRectangle screen = screenOf(parent);
    const int chars = std::clamp((screen.width - 120) / 9, 20, 60);
    const int room = std::max(160, screen.height - 200);
    const int previewMax = spec.preview.empty() ? 0 : std::min(260, room / 2);
    const int textMax = std::max(80, room - previewMax - (spec.preview.empty() ? 0 : 30));

    GtkWidget* win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), spec.message.c_str());
    gtk_widget_add_css_class(win, "message");
    gtk_widget_add_css_class(win, "dialog");
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(win), parent);
        gtk_window_set_destroy_with_parent(GTK_WINDOW(win), TRUE);
    }
    // No title bar, as GTK's alerts: the message says what it is.
    GtkWidget* noTitle = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_visible(noTitle, FALSE);
    gtk_window_set_titlebar(GTK_WINDOW(win), noTitle);

    GtkWidget* all = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(all, 24);
    gtk_widget_set_margin_bottom(all, 16);
    gtk_widget_set_margin_start(all, 24);
    gtk_widget_set_margin_end(all, 24);

    GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget* message = wrapped(spec.message, chars);
    gtk_label_set_justify(GTK_LABEL(message), GTK_JUSTIFY_CENTER);
    PangoAttrList* bold = pango_attr_list_new();
    pango_attr_list_insert(bold, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(bold, pango_attr_scale_new(1.15));
    gtk_label_set_attributes(GTK_LABEL(message), bold);
    pango_attr_list_unref(bold);
    gtk_box_append(GTK_BOX(text), message);
    if (!spec.detail.empty()) {
        GtkWidget* detail = wrapped(spec.detail, chars);
        gtk_label_set_justify(GTK_LABEL(detail), GTK_JUSTIFY_CENTER);
        gtk_box_append(GTK_BOX(text), detail);
    }
    gtk_box_append(GTK_BOX(all), scrolling(text, textMax));
    if (!spec.preview.empty()) {
        GtkWidget* lines = wrapped(spec.preview, chars);
        gtk_label_set_xalign(GTK_LABEL(lines), 0);
        gtk_label_set_selectable(GTK_LABEL(lines), TRUE);
        gtk_widget_add_css_class(lines, "monospace");
        gtk_widget_set_margin_top(lines, 6);
        gtk_widget_set_margin_bottom(lines, 6);
        gtk_widget_set_margin_start(lines, 8);
        gtk_widget_set_margin_end(lines, 8);
        GtkWidget* frame = gtk_frame_new(nullptr);
        gtk_frame_set_child(GTK_FRAME(frame), scrolling(lines, previewMax));
        gtk_box_append(GTK_BOX(all), frame);
    }

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
    gtk_widget_set_margin_top(row, 6);
    GtkWidget* chosen = nullptr;
    for (int i = 0; i < n; i++) {
        GtkWidget* b = gtk_button_new_with_label(buttons[i].c_str());
        gtk_widget_set_hexpand(b, TRUE);
        g_object_set_data(G_OBJECT(b), kIndex, GINT_TO_POINTER(i));
        g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer w) {
            answer(GTK_WIDGET(w), GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), kIndex)));
        }), win);
        gtk_box_append(GTK_BOX(row), b);
        if (i == fallback) chosen = b;
    }
    gtk_box_append(GTK_BOX(all), row);
    gtk_window_set_child(GTK_WINDOW(win), all);

    g_object_set_data_full(G_OBJECT(win), kAsked, new Asked{std::move(answered), cancel, false},
                           [](gpointer p) { delete static_cast<Asked*>(p); });

    // Esc, and the window manager's close, answer the cancel button.
    GtkEventController* keys = gtk_shortcut_controller_new();
    gtk_shortcut_controller_add_shortcut(
        GTK_SHORTCUT_CONTROLLER(keys),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, (GdkModifierType)0),
                         gtk_callback_action_new(
                             [](GtkWidget* w, GVariant*, gpointer) -> gboolean {
                                 if (Asked* a = askedOf(w)) answer(w, a->cancel);
                                 return TRUE;
                             },
                             nullptr, nullptr)));
    gtk_widget_add_controller(win, keys);
    g_signal_connect(win, "close-request", G_CALLBACK(+[](GtkWindow* w, gpointer) -> gboolean {
        Asked* a = askedOf(GTK_WIDGET(w));
        if (!a) return FALSE;
        answer(GTK_WIDGET(w), a->cancel);
        return TRUE;
    }), nullptr);
    // Closed with its parent (or by anything else): the cancel button, on
    // the next turn, so the answer never runs inside the parent's teardown.
    g_signal_connect(win, "destroy", G_CALLBACK(+[](GtkWidget* w, gpointer) {
        Asked* a = askedOf(w);
        if (!a || a->done) return;
        a->done = true;
        struct Late {
            std::function<void(int)> f;
            int choice;
        };
        g_idle_add(
            [](gpointer p) -> gboolean {
                Late* late = static_cast<Late*>(p);
                if (late->f) late->f(late->choice);
                delete late;
                return G_SOURCE_REMOVE;
            },
            new Late{std::move(a->answered), a->cancel});
    }), nullptr);

    // Return answers the default button: it has the keyboard.
    gtk_window_set_default_widget(GTK_WINDOW(win), chosen);
    gtk_window_set_focus(GTK_WINDOW(win), chosen);
    gtk_window_present(GTK_WINDOW(win));
}

void tell(GtkWindow* parent, const std::string& message, const std::string& detail,
          std::function<void()> then) {
    Spec s;
    s.message = message;
    s.detail = detail;
    s.buttons = {"OK"};
    ask(parent, s, [then](int) { if (then) then(); });
}

}  // namespace Question
