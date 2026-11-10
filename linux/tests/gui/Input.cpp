// Input.cpp — real X events through XTest (Harness.h). These reach GTK the way
// a person's do, so accelerator matching, gestures, double-click counting, and
// focus run for real; a hook that calls the open path directly passed three
// bugs the owner then hit by hand (HANDOFF.md).
//
// Everything here runs on the main thread on GDK's own Display, which is why
// an X error can be trapped with GDK's trap and why no second connection or
// error handler is needed.
//
// GTK deprecated its X11 API in 4.18 with the X11 backend. It is still what
// this display runs on, and nothing else in the port uses it.
#define GDK_DISABLE_DEPRECATION_WARNINGS
#include "Harness.h"

#include <gdk/x11/gdkx.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace {

GdkDisplay* Gdk() { return gdk_display_get_default(); }
Display* Dpy() { return gdk_x11_display_get_xdisplay(Gdk()); }

struct KeyPlace {
    KeyCode code = 0;
    bool shift = false;
};

// Keysym -> where it is on the layout, plain or with Shift; and key codes with
// nothing on them, for characters the layout lacks.
std::unordered_map<KeySym, KeyPlace> gLayout;
std::vector<KeyCode> gSpare;
bool gLayoutRead = false;

void readLayout() {
    if (gLayoutRead) return;
    gLayoutRead = true;
    int minCode = 0, maxCode = 0, perKey = 0;
    XDisplayKeycodes(Dpy(), &minCode, &maxCode);
    KeySym* map = XGetKeyboardMapping(Dpy(), (KeyCode)minCode, maxCode - minCode + 1, &perKey);
    if (!map) return;
    for (int code = minCode; code <= maxCode; code++) {
        const KeySym* syms = map + (code - minCode) * perKey;
        bool any = false;
        for (int i = 0; i < perKey; i++) any = any || syms[i] != NoSymbol;
        if (!any) {
            gSpare.push_back((KeyCode)code);
            continue;
        }
        for (int level = 0; level < perKey && level < 2; level++) {
            if (syms[level] == NoSymbol || gLayout.count(syms[level])) continue;
            gLayout[syms[level]] = KeyPlace{(KeyCode)code, level == 1};
        }
    }
    XFree(map);
}

// Where `keysym` can be pressed, putting it on a spare key code if the layout
// has no place for it. GDK sees the new map in event order, before the key
// press, since the press is requested after the change.
bool placeFor(KeySym keysym, KeyPlace* out) {
    readLayout();
    auto it = gLayout.find(keysym);
    if (it != gLayout.end()) {
        *out = it->second;
        return true;
    }
    if (gSpare.empty()) return false;
    const KeyCode code = gSpare.back();
    gSpare.pop_back();
    KeySym sym = keysym;
    XChangeKeyboardMapping(Dpy(), code, 1, &sym, 1);
    XSync(Dpy(), False);
    gLayout[keysym] = KeyPlace{code, false};
    *out = gLayout[keysym];
    return true;
}

void sendKey(KeyCode code, bool down) { XTestFakeKeyEvent(Dpy(), code, down, CurrentTime); }

KeyCode codeOf(KeySym keysym) {
    KeyPlace p;
    return placeFor(keysym, &p) ? p.code : 0;
}

void pressPlace(const KeyPlace& place, bool withShift) {
    const KeyCode shift = codeOf(XK_Shift_L);
    if (withShift) sendKey(shift, true);
    sendKey(place.code, true);
    sendKey(place.code, false);
    if (withShift) sendKey(shift, false);
}

// The registry's key names (src/Commands.h), and any GDK key name besides.
KeySym keysymForName(std::string name) {
    const std::string asGiven = name;
    if (g_utf8_strlen(name.c_str(), -1) == 1) {
        const gunichar c = g_utf8_get_char(name.c_str());
        return gdk_unicode_to_keyval(g_unichar_tolower(c));
    }
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    static const std::unordered_map<std::string, KeySym> names = {
        {"enter", XK_Return},        {"return", XK_Return},    {"tab", XK_Tab},
        {"space", XK_space},         {"escape", XK_Escape},    {"esc", XK_Escape},
        {"backspace", XK_BackSpace}, {"delete", XK_Delete},    {"up", XK_Up},
        {"down", XK_Down},           {"left", XK_Left},        {"right", XK_Right},
        {"home", XK_Home},           {"end", XK_End},          {"pageup", XK_Page_Up},
        {"pagedown", XK_Page_Down},  {"contextmenu", XK_Menu},
    };
    const auto it = names.find(name);
    if (it != names.end()) return it->second;
    if (name.size() > 1 && name[0] == 'f' && std::isdigit((unsigned char)name[1]))
        return XK_F1 + (KeySym)(atoi(name.c_str() + 1) - 1);
    if (name.rfind("numpad", 0) == 0) return XK_KP_0 + (KeySym)atoi(name.c_str() + 6);
    const KeySym lower = gdk_keyval_from_name(name.c_str());
    // A GDK name with capitals ("KP_Insert", the keypad's 0 with Num Lock off).
    return lower != GDK_KEY_VoidSymbol ? lower : gdk_keyval_from_name(asGiven.c_str());
}

Window xidOf(GtkWindow* w) {
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(w));
    return surface ? gdk_x11_surface_get_xid(surface) : 0;
}

GtkWindow* gKeyWindow = nullptr;

// A point in `widget` on the root window: through the widget's window, its
// surface's offset (the window's shadow), and X's own translation.
bool pointOnRoot(GtkWidget* widget, double x, double y, int* rootX, int* rootY) {
    // A widget not laid out yet has no place (its points map to the window's
    // corner), and one clipped or covered gets clicks meant for something
    // else (MCGuiOnScreen).
    if (!MCGuiOnScreen(widget)) return false;
    GtkNative* native = gtk_widget_get_native(widget);
    if (!native) return false;
    if (x < 0) x = gtk_widget_get_width(widget) / 2.0;
    if (y < 0) y = gtk_widget_get_height(widget) / 2.0;
    graphene_point_t in, out;
    graphene_point_init(&in, (float)x, (float)y);
    if (!gtk_widget_compute_point(widget, GTK_WIDGET(native), &in, &out)) return false;
    double sx = 0, sy = 0;
    gtk_native_get_surface_transform(native, &sx, &sy);
    GdkSurface* surface = gtk_native_get_surface(native);
    if (!surface) return false;
    const double scale = gdk_surface_get_scale(surface);
    Window child;
    return XTranslateCoordinates(Dpy(), gdk_x11_surface_get_xid(surface), DefaultRootWindow(Dpy()),
                                 (int)((out.x + sx) * scale + 0.5), (int)((out.y + sy) * scale + 0.5),
                                 rootX, rootY, &child);
}

void moveTo(int rootX, int rootY) {
    XTestFakeMotionEvent(Dpy(), DefaultScreen(Dpy()), rootX, rootY, CurrentTime);
    XSync(Dpy(), False);
}

void button(int b, bool down) {
    XTestFakeButtonEvent(Dpy(), (unsigned)b, down, CurrentTime);
    XSync(Dpy(), False);
}

}  // namespace

void MCGuiSetKeyWindow(GtkWindow* window) { gKeyWindow = window; }

GtkWindow* MCGuiKeyWindow() {
    if (gKeyWindow && GTK_IS_WINDOW(gKeyWindow) && gtk_widget_get_mapped(GTK_WIDGET(gKeyWindow)))
        return gKeyWindow;
    return MCGuiApp().window;
}

void MCGuiFocusKeyWindow() {
    GtkWindow* w = MCGuiKeyWindow();
    if (!w || !gtk_widget_get_mapped(GTK_WIDGET(w))) return;
    const Window xid = xidOf(w);
    if (!xid) return;
    Window current = 0;
    int revert = 0;
    XGetInputFocus(Dpy(), &current, &revert);
    if (current == xid) return;
    gdk_x11_display_error_trap_push(Gdk());
    XRaiseWindow(Dpy(), xid);
    XSetInputFocus(Dpy(), xid, RevertToParent, CurrentTime);
    XSync(Dpy(), False);
    gdk_x11_display_error_trap_pop_ignored(Gdk());
}

void MCGuiKey(const std::string& chord) {
    MCGuiFocusKeyWindow();
    // Modifiers before the last "+", except that a chord may end in "+" itself.
    std::vector<std::string> mods;
    std::string key = chord;
    for (;;) {
        const size_t plus = key.find('+');
        if (plus == std::string::npos || plus + 1 == key.size()) break;
        mods.push_back(key.substr(0, plus));
        key.erase(0, plus + 1);
    }
    std::vector<KeySym> held;
    for (std::string m : mods) {
        for (char& c : m) c = (char)std::tolower((unsigned char)c);
        if (m == "ctrl" || m == "control") held.push_back(XK_Control_L);
        else if (m == "shift") held.push_back(XK_Shift_L);
        else if (m == "alt" || m == "opt" || m == "option") held.push_back(XK_Alt_L);
        else if (m == "super" || m == "cmd" || m == "meta") held.push_back(XK_Super_L);
        else MCGuiCheck(false, "known modifier in \"" + chord + "\": " + m, __FILE__, __LINE__);
    }
    const KeySym sym = keysymForName(key);
    KeyPlace place;
    if (sym == NoSymbol || sym == GDK_KEY_VoidSymbol || !placeFor(sym, &place)) {
        MCGuiCheck(false, "known key in \"" + chord + "\": " + key, __FILE__, __LINE__);
        return;
    }
    if (place.shift && std::find(held.begin(), held.end(), (KeySym)XK_Shift_L) == held.end())
        held.push_back(XK_Shift_L);
    for (KeySym m : held) sendKey(codeOf(m), true);
    sendKey(place.code, true);
    sendKey(place.code, false);
    for (auto it = held.rbegin(); it != held.rend(); ++it) sendKey(codeOf(*it), false);
    XSync(Dpy(), False);
}

void MCGuiType(const std::string& utf8) {
    MCGuiFocusKeyWindow();
    readLayout();
    // Every character placed before the first press, so that GDK has reloaded
    // the map for all of them by the time it reads the first key event.
    std::vector<KeyPlace> places;
    for (const char* p = utf8.c_str(); *p; p = g_utf8_next_char(p)) {
        const gunichar c = g_utf8_get_char(p);
        KeySym sym = c == '\n' ? XK_Return
                   : c == '\t' ? XK_Tab
                   : c == '\b' ? XK_BackSpace
                   : c == 0x1B ? XK_Escape
                               : gdk_unicode_to_keyval(c);
        KeyPlace place;
        if (!placeFor(sym, &place)) {
            MCGuiCheck(false, "a key code for a character in \"" + utf8 + "\"", __FILE__, __LINE__);
            return;
        }
        places.push_back(place);
    }
    for (const KeyPlace& place : places) pressPlace(place, place.shift);
    XSync(Dpy(), False);
}

void MCGuiMoveTo(GtkWidget* widget, double x, double y) {
    int rx = 0, ry = 0;
    if (!widget || !pointOnRoot(widget, x, y, &rx, &ry)) {
        MCGuiCheck(false, "a widget on screen to point at", __FILE__, __LINE__);
        return;
    }
    moveTo(rx, ry);
}

void MCGuiClick(GtkWidget* widget, double x, double y, int b) {
    MCGuiMoveTo(widget, x, y);
    button(b, true);
    button(b, false);
}

void MCGuiPress(GtkWidget* widget, double x, double y, int b) {
    MCGuiMoveTo(widget, x, y);
    button(b, true);
}

void MCGuiRelease(int b) { button(b, false); }

void MCGuiClickWith(const std::string& modifiers, GtkWidget* widget, double x, double y, int b) {
    MCGuiFocusKeyWindow();
    std::vector<KeySym> held;
    std::string rest = modifiers;
    while (!rest.empty()) {
        const size_t plus = rest.find('+');
        std::string m = rest.substr(0, plus);
        rest = plus == std::string::npos ? "" : rest.substr(plus + 1);
        for (char& c : m) c = (char)std::tolower((unsigned char)c);
        if (m == "ctrl" || m == "control") held.push_back(XK_Control_L);
        else if (m == "shift") held.push_back(XK_Shift_L);
        else if (m == "alt") held.push_back(XK_Alt_L);
        else MCGuiCheck(false, "known modifier in \"" + modifiers + "\": " + m, __FILE__, __LINE__);
    }
    MCGuiMoveTo(widget, x, y);
    for (KeySym m : held) sendKey(codeOf(m), true);
    XSync(Dpy(), False);
    button(b, true);
    button(b, false);
    for (auto it = held.rbegin(); it != held.rend(); ++it) sendKey(codeOf(*it), false);
    XSync(Dpy(), False);
}

void MCGuiDoubleClick(GtkWidget* widget, double x, double y) {
    MCGuiClick(widget, x, y);
    MCGuiClick(widget, x, y);
}

// A drag is played over several turns of the main loop, as a hand moves. Sent
// in one go (press, motion, release), a drag in the text view right after a
// double-click selected nothing in 3 runs of 8 (GDK merges motion events that
// arrive together); played this way it selected in 12 of 12. The runner waits
// until it is over (MCGuiInputPending).
namespace {
int gPendingGestures = 0;
struct Gesture {
    std::vector<std::function<void()>> parts;
    size_t next = 0;
};
gboolean playGesturePart(gpointer data) {
    auto* g = static_cast<Gesture*>(data);
    g->parts[g->next++]();
    if (g->next < g->parts.size()) return G_SOURCE_CONTINUE;
    delete g;
    gPendingGestures--;
    return G_SOURCE_REMOVE;
}
}  // namespace

bool MCGuiInputPending() { return gPendingGestures > 0; }

void MCGuiDrag(GtkWidget* widget, double x0, double y0, double x1, double y1) {
    int ax = 0, ay = 0, bx = 0, by = 0;
    if (!widget || !pointOnRoot(widget, x0, y0, &ax, &ay) || !pointOnRoot(widget, x1, y1, &bx, &by)) {
        MCGuiCheck(false, "a widget on screen to drag in", __FILE__, __LINE__);
        return;
    }
    moveTo(ax, ay);
    button(1, true);
    auto* g = new Gesture;
    const int steps = 8;
    for (int i = 1; i <= steps; i++)
        g->parts.push_back([=] { moveTo(ax + (bx - ax) * i / steps, ay + (by - ay) * i / steps); });
    g->parts.push_back([] { button(1, false); });
    gPendingGestures++;
    g_timeout_add(20, playGesturePart, g);
}

void MCGuiDragWith(const std::string& modifiers, GtkWidget* widget, double x0, double y0,
                   double x1, double y1, int b) {
    int ax = 0, ay = 0, bx = 0, by = 0;
    if (!widget || !pointOnRoot(widget, x0, y0, &ax, &ay) || !pointOnRoot(widget, x1, y1, &bx, &by)) {
        MCGuiCheck(false, "a widget on screen to drag in", __FILE__, __LINE__);
        return;
    }
    std::vector<KeySym> held;
    std::string rest = modifiers;
    while (!rest.empty()) {
        const size_t plus = rest.find('+');
        std::string m = rest.substr(0, plus);
        rest = plus == std::string::npos ? "" : rest.substr(plus + 1);
        for (char& c : m) c = (char)std::tolower((unsigned char)c);
        if (m == "ctrl" || m == "control") held.push_back(XK_Control_L);
        else if (m == "shift") held.push_back(XK_Shift_L);
        else if (m == "alt") held.push_back(XK_Alt_L);
        else MCGuiCheck(false, "known modifier in \"" + modifiers + "\": " + m, __FILE__, __LINE__);
    }
    MCGuiFocusKeyWindow();
    moveTo(ax, ay);
    for (KeySym m : held) sendKey(codeOf(m), true);
    XSync(Dpy(), False);
    button(b, true);
    auto* g = new Gesture;
    const int steps = 8;
    for (int i = 1; i <= steps; i++)
        g->parts.push_back([=] { moveTo(ax + (bx - ax) * i / steps, ay + (by - ay) * i / steps); });
    g->parts.push_back([held, b] {
        button(b, false);
        for (auto it = held.rbegin(); it != held.rend(); ++it) sendKey(codeOf(*it), false);
        XSync(Dpy(), False);
    });
    gPendingGestures++;
    g_timeout_add(20, playGesturePart, g);
}

void MCGuiDragThrough(GtkWidget* widget, const std::vector<std::pair<double, double>>& points) {
    std::vector<std::pair<int, int>> root;
    for (const auto& [x, y] : points) {
        int rx = 0, ry = 0;
        if (!widget || !pointOnRoot(widget, x, y, &rx, &ry)) {
            MCGuiCheck(false, "a widget on screen to drag in", __FILE__, __LINE__);
            return;
        }
        root.push_back({rx, ry});
    }
    if (root.size() < 2) {
        MCGuiCheck(false, "two points or more to drag through", __FILE__, __LINE__);
        return;
    }
    moveTo(root[0].first, root[0].second);
    button(1, true);
    auto* g = new Gesture;
    const int steps = 8;
    for (size_t k = 1; k < root.size(); k++) {
        const auto [ax, ay] = root[k - 1];
        const auto [bx, by] = root[k];
        for (int i = 1; i <= steps; i++)
            g->parts.push_back([=] { moveTo(ax + (bx - ax) * i / steps, ay + (by - ay) * i / steps); });
    }
    g->parts.push_back([] { button(1, false); });
    gPendingGestures++;
    g_timeout_add(20, playGesturePart, g);
}

void MCGuiScroll(GtkWidget* widget, int clicks) {
    MCGuiMoveTo(widget);
    const int b = clicks > 0 ? 5 : 4;
    for (int i = 0; i < (clicks > 0 ? clicks : -clicks); i++) {
        button(b, true);
        button(b, false);
    }
}

// Another process's top-level windows on this display, by the _NET_WM_PID
// GDK puts on each (there is no window manager, so they are the root's
// children). On GDK's own Display, with its error trap, since a window can
// go between the listing and the reading.
std::vector<MCGuiOtherWindow> MCGuiWindowsOfProcess(int pid) {
    std::vector<MCGuiOtherWindow> out;
    Display* d = Dpy();
    gdk_x11_display_error_trap_push(Gdk());
    const Atom pidAtom = XInternAtom(d, "_NET_WM_PID", False);
    const Atom nameAtom = XInternAtom(d, "_NET_WM_NAME", False);
    const Atom utf8 = XInternAtom(d, "UTF8_STRING", False);
    Window rootOut = 0, parent = 0, *kids = nullptr;
    unsigned n = 0;
    if (XQueryTree(d, DefaultRootWindow(d), &rootOut, &parent, &kids, &n)) {
        for (unsigned i = 0; i < n; i++) {
            Atom type = 0;
            int format = 0;
            unsigned long items = 0, after = 0;
            unsigned char* data = nullptr;
            if (XGetWindowProperty(d, kids[i], pidAtom, 0, 1, False, XA_CARDINAL, &type, &format,
                                   &items, &after, &data) != Success || !data)
                continue;
            const long got = items == 1 && format == 32 ? (long)*(unsigned long*)data : -1;
            XFree(data);
            if (got != pid) continue;
            MCGuiOtherWindow w;
            w.id = kids[i];
            XWindowAttributes attrs;
            w.viewable = XGetWindowAttributes(d, kids[i], &attrs) && attrs.map_state == IsViewable;
            data = nullptr;
            if (XGetWindowProperty(d, kids[i], nameAtom, 0, 1024, False, utf8, &type, &format,
                                   &items, &after, &data) == Success && data) {
                if (format == 8) w.title.assign((const char*)data, items);
                XFree(data);
            }
            out.push_back(w);
        }
        if (kids) XFree(kids);
    }
    gdk_x11_display_error_trap_pop_ignored(Gdk());
    return out;
}
