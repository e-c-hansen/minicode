// Zoom.cpp — see Zoom.h.
#include "Zoom.h"

#include "ViewZoom.h"

#include <glib/gstdio.h>

#include <cmath>
#include <map>
#include <vector>

namespace Zoom {

namespace {

const char* kZoomClass = "minicode-zoom";   // the editor's view (attachEditor)
const char* kBaseKey = "minicode-zoom-base-points";

struct State {
    bool loaded = false;
    ViewZoom::State zoom;
    ViewZoom::Wrap wrap;
    bool wrapSetting = true;
    bool editorSet = false, terminalSet = false;
    double editorSetting = 13, terminalSetting = 12;
    GtkCssProvider* css = nullptr;
    std::string cssText;
    std::map<int, Listener> listeners;
    int nextId = 1;
};

State& S() {
    static State s;
    return s;
}

void load() {
    State& s = S();
    if (s.loaded) return;
    s.loaded = true;
    gchar* raw = nullptr;
    gsize len = 0;
    if (g_file_get_contents(statePath().c_str(), &raw, &len, nullptr)) {
        s.zoom = ViewZoom::decode(std::string(raw, len));
        g_free(raw);
    }
}

void save() {
    const std::string path = statePath();
    gchar* dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    const std::string text = ViewZoom::encode(S().zoom) + "\n";
    // A temporary file renamed over the old one, so a crash leaves either.
    g_file_set_contents(path.c_str(), text.c_str(), (gssize)text.size(), nullptr);
}

void tell(const std::string& what) {
    // A listener may take itself off (a window closing in the middle).
    std::vector<Listener> now;
    for (const auto& [id, l] : S().listeners) now.push_back(l);
    for (const Listener& l : now) l(what);
}

std::string points(double pt) {
    char buf[32];
    g_snprintf(buf, sizeof buf, "%.2fpt", pt);
    return buf;
}

// The editor's rule, for every view attachEditor marked: its source at its
// size, and its prose (a Markdown page, a message, the welcome text) at
// the desktop's size scaled with the preview. Unzoomed with no size set,
// no rule at all, so the view is exactly as the port always drew it.
void applyCss() {
    State& s = S();
    if (!s.css) {
        GdkDisplay* display = gdk_display_get_default();
        if (!display) return;
        s.css = gtk_css_provider_new();
        // Above the theme's stylesheet (ThemeCss, at APPLICATION), which
        // sets nothing about sizes on the view itself.
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(s.css),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
    std::string css;
    if (s.zoom.editor != 0 || s.editorSet) {
        css += std::string(".") + kZoomClass + " { font-size: " + points(editorPoints()) + "; }\n";
        css += std::string(".") + kZoomClass + ".minicode-prose { font-size: " +
               points(desktopPoints() * previewScale()) + "; }\n";
    }
    if (css == s.cssText) return;
    s.cssText = css;
    gtk_css_provider_load_from_string(s.css, css.c_str());
}

}  // namespace

std::string statePath() { return std::string(g_get_user_state_dir()) + "/minicode/zoom.json"; }

void applySettings(const Settings& settings) {
    State& s = S();
    load();
    const bool wrapBefore = wordWrap();
    const double editorBefore = editorPoints(), terminalBefore = terminalPoints();
    s.wrapSetting = settings.wordWrap();
    s.editorSet = settings.editorFontSizeSet();
    s.terminalSet = settings.terminalFontSizeSet();
    s.editorSetting = settings.editorFontSize();
    s.terminalSetting = settings.terminalFontSize();
    applyCss();
    if (wordWrap() != wrapBefore) tell("wrap");
    if (editorPoints() != editorBefore) tell("editor");
    if (terminalPoints() != terminalBefore) tell("terminal");
}

void attachEditor(GtkWidget* view) {
    load();
    applyCss();
    gtk_widget_add_css_class(view, kZoomClass);
}

double desktopPoints() {
    char* name = nullptr;
    if (GtkSettings* gs = gtk_settings_get_default()) g_object_get(gs, "gtk-font-name", &name, nullptr);
    PangoFontDescription* fd = pango_font_description_from_string(name ? name : "Sans 10");
    g_free(name);
    double pt = 10;
    const int size = pango_font_description_get_size(fd);
    if (size > 0) {
        pt = (double)size / PANGO_SCALE;
        // Pixels at 96 to the inch.
        if (pango_font_description_get_size_is_absolute(fd)) pt = pt * 72.0 / 96.0;
    }
    pango_font_description_free(fd);
    return pt;
}

double editorZoom() {
    load();
    return S().zoom.editor;
}

double terminalZoom() {
    load();
    return S().zoom.terminal;
}

static double editorBase() { return S().editorSet ? S().editorSetting : desktopPoints(); }

double terminalBasePoints() { return S().terminalSet ? S().terminalSetting : desktopPoints(); }

double editorPoints() { return ViewZoom::size(editorBase(), editorZoom()); }

double terminalPoints() { return ViewZoom::size(terminalBasePoints(), terminalZoom()); }

double previewScale() { return ViewZoom::previewScale(editorZoom()); }

bool zoomEditor(int step) {
    load();
    State& s = S();
    double next = s.zoom.editor;
    if (!ViewZoom::step(editorBase(), s.zoom.editor, step, &next)) return false;
    if (next == s.zoom.editor) return true;   // a reset with nothing to reset
    s.zoom.editor = next;
    save();
    applyCss();
    tell("editor");
    return true;
}

bool zoomTerminal(int step) {
    load();
    State& s = S();
    double next = s.zoom.terminal;
    if (!ViewZoom::step(terminalBasePoints(), s.zoom.terminal, step, &next)) return false;
    if (next == s.zoom.terminal) return true;
    s.zoom.terminal = next;
    save();
    tell("terminal");
    return true;
}

void scaleTags(GtkTextTagTable* table) {
    const double zoom = editorZoom();
    gtk_text_tag_table_foreach(table, [](GtkTextTag* tag, gpointer data) {
        const double zoom = *static_cast<const double*>(data);
        double* base = static_cast<double*>(g_object_get_data(G_OBJECT(tag), kBaseKey));
        if (!base) {
            gboolean set = FALSE;
            double pt = 0;
            g_object_get(tag, "size-set", &set, "size-points", &pt, nullptr);
            if (!set || pt <= 0) return;
            base = new double(pt);
            g_object_set_data_full(G_OBJECT(tag), kBaseKey, base,
                                   [](gpointer p) { delete static_cast<double*>(p); });
        }
        const double want = ViewZoom::zoomed(*base, zoom);
        double now = 0;
        g_object_get(tag, "size-points", &now, nullptr);
        if (std::fabs(now - want) > 0.01) g_object_set(tag, "size-points", want, nullptr);
    }, const_cast<double*>(&zoom));
}

bool wordWrap() {
    State& s = S();
    return s.wrap.on(s.wrapSetting);
}

void toggleWordWrap() {
    State& s = S();
    s.wrap.toggle(s.wrapSetting);
    tell("wrap");
}

int listen(Listener l) {
    State& s = S();
    const int id = s.nextId++;
    s.listeners[id] = std::move(l);
    return id;
}

void unlisten(int id) { S().listeners.erase(id); }

}  // namespace Zoom
