// FormatOfferSuite.cpp — `./build/minicode-guitest format_offer`: the offer of
// format on save and Edit > Format on Save, the counterpart of the Mac's
// FormatOnSave.mm (FormatOffer.h), with real keys and clicks:
//   - a stand-in ruff on PATH and a pyproject.toml with [tool.ruff]: after
//     Ctrl+S saves a Python file while format.on-save is off, the bar says
//     "This project formats Python files with ruff, set up in
//     pyproject.toml. Format them each time you save?"; Save All does not
//     offer it;
//   - × closes it until minicode starts again (not offered twice a launch);
//   - Turn On writes format.on-save = project and says so in the status bar;
//   - Don't Ask Again is kept with the install bar's (install-hints.json) and
//     the offer never comes again, even in a new launch;
//   - the settings turning format on some other way closes the bar;
//   - Edit > Format on Save is a checked item that writes false or project.
#include "Harness.h"

#include "ConsentBar.h"
#include "Editor.h"
#include "FormatOffer.h"
#include "FormatRunner.h"
#include "Keys.h"

#include <glib/gstdio.h>
#include <sys/stat.h>

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
FormatOffer* offer() { return MCGuiApp().formatOffer; }

std::string F(const std::string& name) { return MCGuiProjectFile("foff/" + name); }
std::string bin() { return MCGuiScratch() + "/foffbin"; }
std::string settingsFile() { return MCGuiReadFile(g_getenv("MINICODE_SETTINGS")); }

const char* kQuestion =
    "This project formats Python files with ruff, set up in pyproject.toml. "
    "Format them each time you save?";

std::string gPath;

bool dirty() { return ed()->active() && ed()->active()->dirty(); }
bool editorHasKeyboard() { return gtk_widget_has_focus(ed()->textView()); }

std::string statusText() {
    return gtk_label_get_text(GTK_LABEL(MCGuiApp().statusLabel));
}

// Types a line at the end, with the editor's keyboard.
void typeLine(const std::string& line) {
    MCGuiWait("the editor has the keyboard", 10, [] {
        if (!editorHasKeyboard()) ed()->focusActiveDocument();
        return editorHasKeyboard();
    });
    MCGuiStep("type " + line, [line] {
        MCGuiKey("ctrl+end");
        MCGuiType(line);
    });
    MCGuiWait("typed", 10, [] { return dirty(); });
}

// Is there an item "Format on Save" for `action` in a menu model?
bool hasItem(GMenuModel* m, const std::string& label, const std::string& action) {
    for (int i = 0; i < g_menu_model_get_n_items(m); i++) {
        gchar* l = nullptr;
        gchar* a = nullptr;
        g_menu_model_get_item_attribute(m, i, "label", "s", &l);
        g_menu_model_get_item_attribute(m, i, "action", "s", &a);
        const bool hit = l && a && label == l && action == a;
        g_free(l);
        g_free(a);
        if (hit) return true;
        GMenuModel* sub = g_menu_model_get_item_link(m, i, G_MENU_LINK_SUBMENU);
        if (!sub) sub = g_menu_model_get_item_link(m, i, G_MENU_LINK_SECTION);
        if (sub) {
            const bool found = hasItem(sub, label, action);
            g_object_unref(sub);
            if (found) return true;
        }
    }
    return false;
}

bool actionOn() {
    GVariant* v = g_action_group_get_action_state(G_ACTION_GROUP(MCGuiApplication()), "formatonsave");
    const bool on = v && g_variant_get_boolean(v);
    if (v) g_variant_unref(v);
    return on;
}

// A save of a changed line with Ctrl+S, and the offer ready to come again.
void saveWithOffer(const std::string& line) {
    MCGuiStep("a new offer may come; ruff and the file", [] {
        FormatOffer::forgetOffered();
    });
    typeLine(line);
    MCGuiStep("Ctrl+S", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("saved", 10, [] { return !dirty(); });
}

}  // namespace

MC_GUI_SUITE(format_offer) {
    MCGuiStep("a ruff stand-in on PATH, a project that sets up ruff, format.on-save off", [] {
        MCGuiWriteFile(bin() + "/ruff", "#!/bin/sh\nexec /bin/cat -\n");
        chmod((bin() + "/ruff").c_str(), 0755);
        const char* p = g_getenv("PATH");
        gPath = p ? p : "";
        g_setenv("PATH", (bin() + ":" + gPath).c_str(), TRUE);
        MCGuiWriteFile(F("pyproject.toml"), "[tool.ruff]\nline-length = 88\n");
        MCGuiWriteFile(F("a.py"), "x = 1\n");
        ConsentBar::setInstallMuted(FormatOffer::key(), false);
        FormatOffer::forgetOffered();
        MCGuiSetSettings("lsp.enabled = false\n");
        MCGuiOpenFileKept(F("a.py"));
    });
    MCGuiWait("a.py on show", 10, [] { return ed()->currentPath() == F("a.py") && ed()->isSource(); });

    // ---- the offer comes after Ctrl+S, not after Save All
    typeLine("y = 2\n");
    MCGuiStep("Ctrl+Alt+S (Save All)", [] { MCGuiKey("ctrl+alt+s"); });
    MCGuiWait("saved", 10, [] { return !dirty(); });
    MCGuiPause(0.5);   // negative check: a look for the offer would answer in this time
    MCGuiStep("Save All offered nothing", [] {
        GCHECK(!offer()->shown());
        GCHECK(!offer()->checking());
    });
    saveWithOffer("z = 3\n");
    MCGuiWait("the bar shows", 15, [] { return offer()->shown(); });
    MCGuiStep("its words and buttons are the Mac's", [] {
        GCHECK(offer()->text() == kQuestion);
        GCHECK(std::string(gtk_button_get_label(GTK_BUTTON(offer()->turnOnButton()))) == "Turn On");
        GCHECK(std::string(gtk_button_get_label(GTK_BUTTON(offer()->muteButton()))) ==
               "Don't Ask Again");
        GCHECK(std::string(gtk_button_get_label(GTK_BUTTON(offer()->closeButton()))) == "×");
        GCHECK(settingsFile().find("format.on-save") == std::string::npos);
        MCGuiClick(offer()->closeButton());
    });
    MCGuiWait("× closes it", 10, [] { return !offer()->shown(); });
    MCGuiStep("nothing was written, and a second save the same launch offers nothing", [] {
        GCHECK(settingsFile().find("format.on-save") == std::string::npos);
        GCHECK(!ConsentBar::installMuted(FormatOffer::key()));
    });
    typeLine("w = 4\n");
    MCGuiStep("Ctrl+S again", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("saved", 10, [] { return !dirty(); });
    MCGuiStep("not offered twice in a launch", [] { GCHECK(!offer()->shown() && !offer()->checking()); });

    // ---- Turn On
    saveWithOffer("v = 5\n");
    MCGuiWait("the bar shows again", 15, [] { return offer()->shown(); });
    MCGuiStep("Turn On", [] { MCGuiClick(offer()->turnOnButton()); });
    MCGuiWait("the bar goes and the settings say project", 10, [] {
        return !offer()->shown() && settingsFile().find("format.on-save = project") != std::string::npos;
    });
    MCGuiStep("the status bar says so, and the menu item is checked", [] {
        GCHECK(statusText() == "Format on save is on for formatters a project sets up. "
                               "Edit > Format on Save turns it off.");
    });
    MCGuiWait("the item is checked", 10, [] { return actionOn(); });

    // ---- Edit > Format on Save
    MCGuiStep("the Edit menu has it, with no key", [] {
        GMenuModel* bar = Keys::menuBar();
        GCHECK(hasItem(bar, "Format on Save", "app.formatonsave"));
        g_object_unref(bar);
        GCHECK(Keys::label("minicode.toggleFormatOnSave").empty());
    });
    MCGuiStep("Edit > Format on Save turns it off", [] {
        g_action_group_activate_action(G_ACTION_GROUP(MCGuiApplication()), "formatonsave", nullptr);
    });
    MCGuiWait("false in the file, unchecked", 10, [] {
        return !actionOn() && settingsFile().find("format.on-save = false") != std::string::npos;
    });
    MCGuiStep("and on again", [] {
        g_action_group_activate_action(G_ACTION_GROUP(MCGuiApplication()), "formatonsave", nullptr);
    });
    MCGuiWait("project in the file, checked", 10, [] {
        return actionOn() && settingsFile().find("format.on-save = project") != std::string::npos;
    });

    // ---- with format on save on, nothing is offered; the settings closing the bar
    MCGuiStep("format on save is on: Ctrl+S formats (the stand-in changes nothing), no offer", [] {
        FormatOffer::forgetOffered();
    });
    typeLine("u = 6\n");
    MCGuiStep("Ctrl+S", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("saved", 15, [] { return !dirty(); });
    MCGuiStep("no bar", [] { GCHECK(!offer()->shown() && !offer()->checking()); });
    MCGuiStep("format.on-save off in the file", [] { MCGuiSetSettings("lsp.enabled = false\n"); });
    MCGuiWait("off", 10, [] { return !actionOn(); });
    saveWithOffer("t = 7\n");
    MCGuiWait("the bar shows", 15, [] { return offer()->shown(); });
    MCGuiStep("format.on-save = true written in the file by hand", [] {
        MCGuiSetSettings("lsp.enabled = false\nformat.on-save = true\n");
    });
    MCGuiWait("the bar goes by itself", 10, [] { return !offer()->shown(); });

    // ---- Don't Ask Again
    MCGuiStep("format.on-save off again", [] { MCGuiSetSettings("lsp.enabled = false\n"); });
    MCGuiWait("off", 10, [] { return !actionOn(); });
    saveWithOffer("s = 8\n");
    MCGuiWait("the bar shows", 15, [] { return offer()->shown(); });
    MCGuiStep("Don't Ask Again", [] { MCGuiClick(offer()->muteButton()); });
    MCGuiWait("the bar goes and the key is kept", 10, [] {
        return !offer()->shown() && ConsentBar::installMuted(FormatOffer::key());
    });
    saveWithOffer("r = 9\n");
    MCGuiStep("a new launch would not offer it either", [] {
        GCHECK(!offer()->shown() && !offer()->checking());
        GCHECK(settingsFile().find("format.on-save") == std::string::npos);
    });

    MCGuiStep("tidy up", [] {
        ConsentBar::setInstallMuted(FormatOffer::key(), false);
        FormatOffer::forgetOffered();
        MCGuiSetSettings("lsp.enabled = false\n");
        g_setenv("PATH", gPath.c_str(), TRUE);
        if (Document* d = ed()->documentFor(F("a.py"))) ed()->closeTab(d);
    });
}
