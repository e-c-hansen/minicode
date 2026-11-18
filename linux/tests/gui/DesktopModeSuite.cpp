// DesktopModeSuite.cpp — mutible as the whole desktop (DesktopMode.h,
// linux/desktop/README.md): `./build/mutible-guitest desktop_mode`.
// MUTIBLE_DESKTOP is set through EnvLock for the suite and put back, and
// logind is a recorder (DesktopMode::setLogindCall); the checks' build has no
// way to the real one, so nothing here can suspend or power off the machine.
//   - without the variable the File menu has no power items and the actions
//     are disabled; with it, Suspend…, Restart…, Power Off… and Log Out…
//     end the File menu, enabled;
//   - Suspend asks logind CanSuspend first, then asks the user: Cancel sends
//     nothing, Suspend sends Suspend;
//   - a "challenge" from CanPowerOff is said in the status bar, with no
//     question and no call;
//   - in a second app of its own (_desktop_mode_child, which may close its
//     only window): closing the last window opens a fresh one on the home
//     folder; Restart quits as Quit does, and a Cancel at "Save changes?"
//     keeps the window and drops the restart; Power Off with Don't Save
//     closes the window, opens none, and logind's PowerOff goes out at the
//     shutdown, synchronously.
#include "Harness.h"

#include "DesktopMode.h"
#include "Editor.h"
#include "EnvLock.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::vector<std::string> gCalls;   // "Can…" and the actions, in order
std::string gCanAnswer = "yes";
bool gHad = false;
std::string gOld;

void record() {
    gCalls.clear();
    DesktopMode::setLogindCall([](const std::string& method, DesktopMode::Done done) {
        gCalls.push_back(method + (done ? "" : " (sync)"));
        if (done) done(method.rfind("Can", 0) == 0 ? gCanAnswer : "", "");
    });
}

void setDesktop(bool on) {
    if (on) EnvLock::envSet("MUTIBLE_DESKTOP", "1");
    else EnvLock::envUnset("MUTIBLE_DESKTOP");
    DesktopMode::refresh();
}

// The File menu's rows as "label=action", sections flattened.
void rows(GMenuModel* m, std::vector<std::string>* out) {
    for (int i = 0; i < g_menu_model_get_n_items(m); i++) {
        if (GMenuModel* sec = g_menu_model_get_item_link(m, i, G_MENU_LINK_SECTION)) {
            rows(sec, out);
            g_object_unref(sec);
            continue;
        }
        char* label = nullptr;
        char* action = nullptr;
        g_menu_model_get_item_attribute(m, i, "label", "s", &label);
        g_menu_model_get_item_attribute(m, i, "action", "s", &action);
        out->push_back(std::string(label ? label : "") + "=" + (action ? action : ""));
        g_free(label);
        g_free(action);
    }
}
std::vector<std::string> fileMenu() {
    std::vector<std::string> out;
    GMenuModel* bar = gtk_application_get_menubar(MCGuiApplication());
    for (int i = 0; bar && i < g_menu_model_get_n_items(bar); i++) {
        char* label = nullptr;
        g_menu_model_get_item_attribute(bar, i, "label", "s", &label);
        const bool file = label && std::string(label) == "File";
        g_free(label);
        if (!file) continue;
        GMenuModel* sub = g_menu_model_get_item_link(bar, i, G_MENU_LINK_SUBMENU);
        if (sub) rows(sub, &out);
        if (sub) g_object_unref(sub);
    }
    return out;
}
bool has(const std::vector<std::string>& v, const std::string& s) {
    for (const std::string& x : v)
        if (x == s) return true;
    return false;
}
bool enabled(const char* action) {
    return g_action_group_get_action_enabled(G_ACTION_GROUP(MCGuiApplication()), action);
}
void activate(const char* action) {
    g_action_group_activate_action(G_ACTION_GROUP(MCGuiApplication()), action, nullptr);
}
// The question's window (an alert over the app's), or null.
GtkWidget* question(const std::string& button) {
    GtkWindow* top = MCGuiTopWindow();
    if (!top || top == MCGuiApp().window) return nullptr;
    return MCGuiFindButton(GTK_WIDGET(top), button);
}
std::string status() {
    return gtk_label_get_text(GTK_LABEL(MCGuiApp().statusLabel));
}

}  // namespace

MC_GUI_SUITE(desktop_mode) {
    MCGuiStep("without MUTIBLE_DESKTOP: no power items, the actions off", [] {
        const char* v = getenv("MUTIBLE_DESKTOP");
        gHad = v != nullptr;
        gOld = v ? v : "";
        record();
        setDesktop(false);
        const std::vector<std::string> menu = fileMenu();
        GCHECK(!menu.empty());
        GCHECK(!has(menu, "Power Off…=app.desktop-poweroff"));
        GCHECK(!enabled("desktop-suspend"));
        GCHECK(!enabled("desktop-poweroff"));
        GCHECK(!DesktopMode::keepsLastWindow(false));
    });
    MCGuiStep("with it: Suspend, Restart, Power Off and Log Out end the File menu", [] {
        setDesktop(true);
        const std::vector<std::string> menu = fileMenu();
        GCHECK(menu.size() >= 4);
        if (menu.size() < 4) return;
        const std::vector<std::string> tail(menu.end() - 4, menu.end());
        GCHECK_EQ_STR(tail[0], "Suspend…=app.desktop-suspend");
        GCHECK_EQ_STR(tail[1], "Restart…=app.desktop-restart");
        GCHECK_EQ_STR(tail[2], "Power Off…=app.desktop-poweroff");
        GCHECK_EQ_STR(tail[3], "Log Out…=app.desktop-logout");
        for (const char* a : {"desktop-suspend", "desktop-restart", "desktop-poweroff", "desktop-logout"})
            GCHECK(enabled(a));
        GCHECK(DesktopMode::keepsLastWindow(false));
        GCHECK(!DesktopMode::keepsLastWindow(true));   // a quit still quits
    });

    MCGuiStep("Suspend asks logind whether it may, then asks", [] {
        gCalls.clear();
        activate("desktop-suspend");
    });
    MCGuiClickWhenReady("the question's Cancel", [] { return question("Cancel"); });
    MCGuiWait("the question is gone", 10, [] { return MCGuiTopWindow() == MCGuiApp().window; });
    MCGuiStep("Cancel sent nothing", [] {
        GCHECK(gCalls.size() == 1);
        if (!gCalls.empty()) GCHECK_EQ_STR(gCalls[0], "CanSuspend");
        gCalls.clear();
        activate("desktop-suspend");
    });
    MCGuiClickWhenReady("the question's Suspend", [] { return question("Suspend"); });
    MCGuiWait("Suspend goes to logind", 10, [] { return gCalls.size() == 2; });
    MCGuiStep("CanSuspend, then Suspend, and no quit", [] {
        GCHECK_EQ_STR(gCalls[0], "CanSuspend");
        GCHECK_EQ_STR(gCalls[1], "Suspend");
        GCHECK(DesktopMode::pending().empty());
        GCHECK(MCGuiApp().window != nullptr);
    });

    MCGuiStep("CanPowerOff says challenge", [] {
        gCalls.clear();
        gCanAnswer = "challenge";
        activate("desktop-poweroff");
    });
    MCGuiWait("the status bar says why", 10, [] {
        return status().find("administrator") != std::string::npos;
    });
    MCGuiPause(0.3);   // a negative check: no question comes up
    MCGuiStep("no question and nothing sent past the Can", [] {
        GCHECK(MCGuiTopWindow() == MCGuiApp().window);
        GCHECK(gCalls.size() == 1);
        if (!gCalls.empty()) GCHECK_EQ_STR(gCalls[0], "CanPowerOff");
        GCHECK(DesktopMode::pending().empty());
        gCanAnswer = "yes";
    });

    MCGuiAsync("a second app as the desktop: its last window, Restart, Power Off", 120,
               [](MCGuiDone done) {
        MCGuiWriteFile(MCGuiProjectFile("desktop/notes.txt"), "a line\n");
        MCGuiRunChild("_desktop_mode_child", MCGuiProjectFile("desktop"),
                      [done](int status, std::string out) {
            GCHECK(status == 0);
            if (status != 0) MCGuiNote("%s", out.c_str());
            GCHECK_EQ_STR(MCGuiReadFile(MCGuiScratch() + "/desktop-child-done.txt"), "ok\n");
            done();
        });
    });

    MCGuiStep("put back: the variable and logind", [] {
        if (gHad) EnvLock::envSet("MUTIBLE_DESKTOP", gOld.c_str());
        else EnvLock::envUnset("MUTIBLE_DESKTOP");
        DesktopMode::refresh();
        DesktopMode::setLogindCall(nullptr);
        GCHECK(!enabled("desktop-suspend"));
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();   // the questions took the X focus with them
    });
    MCGuiWait("the window is active again", 10, [] { return gtk_window_is_active(MCGuiApp().window); });
}

// The second app: MUTIBLE_DESKTOP=1 came from the parent's environment.
MC_GUI_SUITE(_desktop_mode_child) {
    MCGuiStep("in desktop mode, logind a recorder", [] {
        GCHECK(DesktopMode::active());
        record();
        GCHECK(MCGuiAppWindows().size() == 1);
    });
    MCGuiStep("its only window closed", [] { MCGuiCloseAllWindows(); });
    MCGuiWait("a fresh window, on the home folder", 20, [] {
        const std::vector<MCGuiAppWindow> w = MCGuiAppWindows();
        return w.size() == 1 && w[0].rootDir == g_get_home_dir();
    });

    MCGuiStep("an unsaved tab in it", [] {
        MCGuiWriteFile(std::string(g_get_home_dir()) + "/draft.txt", "draft\n");
        MCGuiOpenFileKept(std::string(g_get_home_dir()) + "/draft.txt");
    });
    MCGuiWait("it shows", 10, [] {
        return MCGuiApp().editor->currentPath() == std::string(g_get_home_dir()) + "/draft.txt";
    });
    MCGuiStep("a click in the text", [] { MCGuiClick(MCGuiApp().editor->textView()); });
    MCGuiWait("the text has the keyboard", 5, [] {
        return gtk_widget_has_focus(MCGuiApp().editor->textView());
    });
    MCGuiStep("typed", [] { MCGuiType("x"); });
    MCGuiWait("unsaved", 10, [] { return MCGuiApp().editor->dirty(); });

    MCGuiStep("Restart", [] { activate("desktop-restart"); });
    MCGuiClickWhenReady("the question's Restart", [] { return question("Restart"); });
    MCGuiWait("waiting for the app to quit, which asks about the tab", 10, [] {
        return DesktopMode::pending() == "Reboot" && question("Don't Save") != nullptr;
    });
    MCGuiClickWhenReady("Save changes?'s Cancel", [] {
        return question("Don't Save") ? question("Cancel") : nullptr;
    });
    MCGuiWait("the question is gone", 10, [] { return MCGuiTopWindow() == MCGuiApp().window; });
    MCGuiStep("the window stays, the restart is dropped, nothing sent", [] {
        GCHECK(MCGuiAppWindows().size() == 1);
        GCHECK(DesktopMode::pending().empty());
        GCHECK(gCalls.size() == 1);
        if (!gCalls.empty()) GCHECK_EQ_STR(gCalls[0], "CanReboot");
        DesktopMode::atShutdown();   // with nothing pending, nothing goes
        GCHECK(gCalls.size() == 1);
        gCalls.clear();
        MCGuiSetKeyWindow(nullptr);
    });

    MCGuiStep("Power Off", [] { activate("desktop-poweroff"); });
    MCGuiClickWhenReady("the question's Power Off", [] { return question("Power Off"); });
    MCGuiClickWhenReady("Save changes?'s Don't Save", [] { return question("Don't Save"); });
    MCGuiWait("the window closes", 20, [] { return MCGuiAppWindows().empty(); });
    MCGuiPause(0.5);   // a negative check: a quit opens no fresh window
    MCGuiStep("no window came back; PowerOff waits for the shutdown", [] {
        GCHECK(MCGuiAppWindows().empty());
        GCHECK_EQ_STR(DesktopMode::pending(), "PowerOff");
        GCHECK(gCalls.size() == 1);
        if (!gCalls.empty()) GCHECK_EQ_STR(gCalls[0], "CanPowerOff");
        DesktopMode::atShutdown();   // what GApplication's shutdown runs
        GCHECK(gCalls.size() == 2);
        if (gCalls.size() == 2) GCHECK_EQ_STR(gCalls[1], "PowerOff (sync)");
        GCHECK(DesktopMode::pending().empty());
        MCGuiWriteFile(MCGuiScratch() + "/desktop-child-done.txt", "ok\n");
    });
}
