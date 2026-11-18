// DesktopMode.cpp — see DesktopMode.h.
#include "DesktopMode.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace DesktopMode {
namespace {

struct Item {
    const char* action;   // app.<action>
    const char* label;
    const char* method;   // logind's, "" for Log Out
    const char* question;
    const char* detail;
    const char* button;
};

const Item kItems[] = {
    {"desktop-suspend", "Suspend…", "Suspend", "Suspend this computer?",
     "Your windows stay as they are.", "Suspend"},
    {"desktop-restart", "Restart…", "Reboot", "Restart this computer?",
     "mutible closes first, asking about any unsaved changes.", "Restart"},
    {"desktop-poweroff", "Power Off…", "PowerOff", "Power off this computer?",
     "mutible closes first, asking about any unsaved changes.", "Power Off"},
    {"desktop-logout", "Log Out…", "", "Log out?",
     "mutible closes, asking about any unsaved changes, and starts again.", "Log Out"},
};

Host gHost;
GtkApplication* gApp = nullptr;
GMenu* gSection = nullptr;
Call gCall;
std::string gPending;   // "Reboot", "PowerOff", "LogOut" or ""
bool gAsking = false;

void note(const std::string& message) {
    if (gHost.note) gHost.note(message);
}

#ifndef MUTIBLE_GUITEST
// The real call: org.freedesktop.login1.Manager on the system bus, never
// interactive (no polkit agent runs in the session).
void systemCall(const std::string& method, Done done) {
    GError* err = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &err);
    if (!bus) {
        const std::string why = err ? err->message : "no system bus";
        g_clear_error(&err);
        if (done) done("", why);
        else std::fprintf(stderr, "mutible: %s: %s\n", method.c_str(), why.c_str());
        return;
    }
    const bool can = method.rfind("Can", 0) == 0;
    GVariant* params = can ? nullptr : g_variant_new("(b)", FALSE);
    const GVariantType* reply = can ? G_VARIANT_TYPE("(s)") : nullptr;
    if (!done) {
        GVariant* r = g_dbus_connection_call_sync(bus, "org.freedesktop.login1",
                                                  "/org/freedesktop/login1",
                                                  "org.freedesktop.login1.Manager",
                                                  method.c_str(), params, reply,
                                                  G_DBUS_CALL_FLAGS_NONE, 10000, nullptr, &err);
        if (r) g_variant_unref(r);
        if (err) {
            std::fprintf(stderr, "mutible: %s: %s\n", method.c_str(), err->message);
            g_clear_error(&err);
        }
        g_object_unref(bus);
        return;
    }
    g_dbus_connection_call(
        bus, "org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", method.c_str(), params, reply,
        G_DBUS_CALL_FLAGS_NONE, 10000, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer data) {
            Done* d = static_cast<Done*>(data);
            GError* e = nullptr;
            GVariant* r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
            std::string answer, error;
            if (r) {
                if (g_variant_is_of_type(r, G_VARIANT_TYPE("(s)"))) {
                    const char* s = nullptr;
                    g_variant_get(r, "(&s)", &s);
                    answer = s ? s : "";
                }
                g_variant_unref(r);
            } else {
                error = e ? e->message : "no answer";
                g_clear_error(&e);
            }
            (*d)(answer, error);
            delete d;
        },
        new Done(std::move(done)));
    g_object_unref(bus);   // the call holds its own reference
}
#endif

void call(const std::string& method, Done done) {
    if (gCall) { gCall(method, std::move(done)); return; }
#ifdef MUTIBLE_GUITEST
    // The checks' build has no way to the real logind.
    if (done) done("", "logind is not reached from the GUI checks");
#else
    systemCall(method, std::move(done));
#endif
}

void ask(const Item& item, std::function<void()> proceed) {
    if (gAsking) return;
    gAsking = true;
    GtkAlertDialog* dlg = gtk_alert_dialog_new("%s", item.question);
    gtk_alert_dialog_set_detail(dlg, item.detail);
    const char* buttons[] = {item.button, "Cancel", nullptr};
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_default_button(dlg, 0);
    gtk_alert_dialog_set_cancel_button(dlg, 1);
    gtk_alert_dialog_set_modal(dlg, TRUE);
    GtkWindow* parent = gHost.front ? gHost.front() : nullptr;
    gtk_alert_dialog_choose(dlg, parent, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer data) {
            auto* go = static_cast<std::function<void()>*>(data);
            gAsking = false;
            const int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr);
            if (choice == 0) (*go)();
            delete go;
        }, new std::function<void()>(std::move(proceed)));
    g_object_unref(dlg);
}

// After the question: Suspend at once; the others when the app has quit.
void carryOut(const Item& item) {
    const std::string method = item.method;
    if (method == "Suspend") {
        call(method, [](const std::string&, const std::string& error) {
            if (!error.empty()) note("Suspend failed: " + error);
        });
        return;
    }
    gPending = method.empty() ? "LogOut" : method;
    if (gHost.quit) gHost.quit();
}

std::string refusal(const Item& item, const std::string& answer) {
    const std::string what = std::string(item.button);
    if (answer == "challenge")
        return what + " needs an administrator's password here; run `systemctl " +
               (std::string(item.method) == "Reboot" ? "reboot"
                : std::string(item.method) == "PowerOff" ? "poweroff" : "suspend") +
               "` in a terminal";
    return what + " is not available on this computer (logind: " + answer + ")";
}

void activate(const Item& item) {
    if (!active()) return;
    if (!*item.method) {   // Log Out: nothing to ask logind
        ask(item, [&item] { carryOut(item); });
        return;
    }
    call(std::string("Can") + item.method, [&item](const std::string& answer, const std::string& error) {
        if (!error.empty()) { note(std::string(item.button) + ": " + error); return; }
        if (answer != "yes") { note(refusal(item, answer)); return; }
        ask(item, [&item] { carryOut(item); });
    });
}

}  // namespace

bool active() {
    const char* v = g_getenv("MUTIBLE_DESKTOP");
    return v && !std::strcmp(v, "1");
}

void install(GtkApplication* app, Host host) {
    gApp = app;
    gHost = std::move(host);
    for (const Item& item : kItems) {
        GSimpleAction* a = g_simple_action_new(item.action, nullptr);
        g_signal_connect(a, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer p) {
            activate(*static_cast<const Item*>(p));
        }), const_cast<Item*>(&item));
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(a));
        g_object_unref(a);
    }
    refresh();
}

void adjustWindow(GtkWindow* window) {
    if (active()) gtk_window_set_decorated(window, FALSE);
}

void extendFileMenu(GMenu* file) {
    if (!gSection) gSection = g_menu_new();   // kept for refresh, for the app's life
    g_menu_append_section(file, nullptr, G_MENU_MODEL(gSection));
    refresh();
}

void refresh() {
    const bool on = active();
    if (gApp)
        for (const Item& item : kItems) {
            GAction* a = g_action_map_lookup_action(G_ACTION_MAP(gApp), item.action);
            if (a) g_simple_action_set_enabled(G_SIMPLE_ACTION(a), on);
        }
    if (!gSection) return;
    const bool shown = g_menu_model_get_n_items(G_MENU_MODEL(gSection)) > 0;
    if (on == shown) return;
    if (on)
        for (const Item& item : kItems)
            g_menu_append(gSection, item.label, (std::string("app.") + item.action).c_str());
    else
        g_menu_remove_all(gSection);
}

bool keepsLastWindow(bool quitting) {
    return active() && !quitting;
}

void quitCancelled() {
    gPending.clear();
}

void atShutdown() {
    const std::string method = gPending;
    gPending.clear();
    if (method == "Reboot" || method == "PowerOff") call(method, nullptr);
}

void setLogindCall(Call c) {
    gCall = std::move(c);
}

std::string pending() {
    return gPending;
}

}  // namespace DesktopMode
