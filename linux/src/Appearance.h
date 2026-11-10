// Appearance.h — the GTK port's counterpart of the Mac's Appearance.mm and
// the Adjust Settings half of EditorController+Settings.mm (PARITY.md 2.11):
//
//   - "Adjust Settings…" at the end of every part of the window's right
//     click menu, opening the settings file at the keys that color that part
//     (Settings::appearanceSurfaces has the map), as the owner asked on
//     2026-11-09: "This should point to the relevant part of the minicode
//     config."
//   - View > Adjust Settings (a row per part) and View > Theme (the nine
//     themes, the one in use checked), added to the registry's View menu.
//   - What choosing a theme does: `theme = <id>` written through the atomic
//     writer (AppSettings.h, writeConfigFile), after asking, when the file
//     sets colors of its own, whether to comment those out; refused, with
//     the reason in the status bar, while a tab holds the file with unsaved
//     changes. And GTK's own widgets going dark or light with the theme
//     (Settings::isDark), as the Mac sets the window's NSAppearance.
//   - In the settings file: the surface's keys' commented defaults added
//     when the file is older than them, the caret on the first key's value,
//     and the keys' lines tinted for about three seconds.
//
// A new part of the window adds the item with one call where its menu is
// made: addItem(menu, "terminal") at the end of a GMenu it builds,
// attachMenu(widget, "statusbar") for a widget with no menu of its own (a
// right click there pops one holding only the item), addToTextMenus(root,
// "browser") for the text fields under a widget (their editing menu ends
// with it), or addToWebMenu for a WebKit page's context-menu signal. The
// items name the window's action win.adjustsettings with the surface's id
// as its target, which main.cpp answers.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

#ifdef MINICODE_ENABLE_BROWSER
#include <webkit/webkit.h>
#endif

#include "Settings.h"

class AppSettings;
class Document;
class Editor;

namespace Appearance {

// The window's action every item names, with the surface's id as target.
constexpr const char* kAction = "adjustsettings";
constexpr const char* kItemLabel = "Adjust Settings…";
// The tag that tints a surface's lines in the settings file for a moment.
constexpr const char* kTintTag = "minicode-adjust-tint";

// ------------------------------------------------------------ the item
// A section holding "Adjust Settings…" for `surface` at the end of `menu`
// (nothing for an unknown surface).
void addItem(GMenu* menu, const std::string& surface);
// A menu holding only that item. New reference.
GMenuModel* menuFor(const std::string& surface);
// A right click on `widget` that none of its children took pops a menu
// holding only the item, at the pointer.
void attachMenu(GtkWidget* widget, const std::string& surface);
// Every text field and text view under `root` (now) gets the item at the
// end of its own editing menu, as the Mac's MCSurfaceTextField.
void addToTextMenus(GtkWidget* root, const std::string& surface);
// The editor's view: its menu ends with the item for `surfaceNow()`
// ("editor", or "markdown" while it shows a Markdown page), asked when the
// view changes buffer and when a right click starts.
void attachEditorMenu(GtkWidget* view, std::function<std::string()> surfaceNow);
// The menu the view's right click shows now ends with the item for this
// surface (for the checks, and for a caller that knows better).
std::string editorMenuSurface(GtkWidget* view);
#ifdef MINICODE_ENABLE_TERMINAL
// The terminal's right click: Copy, Paste, Select All, and the item.
void attachTerminalMenu(GtkWidget* vte);
#endif
#ifdef MINICODE_ENABLE_BROWSER
// A page's right click (WebKitWebView's context-menu signal): its own items,
// then the item for "browser".
void addToWebMenu(WebKitContextMenu* menu, GtkWidget* webView);
#endif

// ------------------------------------------------------------ the View menu
// After the View menu's commands: Adjust Settings (a row per surface) and
// Theme (a row per theme, dark ones then light ones, app.theme with the
// theme's id; the one in use checked). Keys::menuBar's `extend`.
void extendMenu(const std::string& top, GMenu* menu);

// ------------------------------------------------------------ the settings file
struct FileHost {
    // Writes the document to its file (the caller shows a failure).
    std::function<bool(Document*)> save;
    // A message in the window's status bar.
    std::function<void(const std::string&)> note;
};
// With the settings file on show in `editor` (main.cpp opens it first): a
// file without some of `surface`'s keys gets their commented defaults at
// its end (Settings::adjustSettingsAddition), as one undo step, saved at
// once when it had no unsaved changes; then the caret goes to the first
// key's value, selected, and every key's line is tinted for about three
// seconds (kTintTag). "No <part> settings in this file." when none is there.
void showSurfaceKeys(Editor* editor, const std::string& surface, const FileHost& host);
// The lines of `buffer` the tint is on now (for the checks).
std::vector<int> tintedLines(GtkTextBuffer* buffer);

// ------------------------------------------------------------ themes
struct ThemeHost {
    AppSettings* settings = nullptr;
    // The window a question goes over and a refusal's note goes to.
    std::function<GtkWindow*()> frontWindow;
    std::function<void(const std::string&)> note;
};
// app.theme: a stateful action (the theme's id) on the application, whose
// activation chooses a theme (chooseTheme). Once, before the menu bar.
void installThemeAction(GtkApplication* app, ThemeHost host);
// The settings changed: the menu's check follows the theme in use, and
// GTK's widgets its darkness.
void settingsChanged(GtkApplication* app, const Settings& settings);
// View > Theme's row: when the file sets colors of its own
// (Settings::colorOverrides), asks whether to comment them out (Use the
// Theme's Colors, Keep My Colors, Cancel), then writes `theme = <id>`
// (AppSettings::writeTheme). A refusal (a tab holding the file with unsaved
// changes) or a failure goes to the status bar with the bell.
void chooseTheme(const std::string& id);
// GTK's own widgets (menus, dialogs, entries, scroll bars it draws) light
// or dark with the theme as drawn (Settings::isDark).
void applyColorScheme(const Settings& settings);

}  // namespace Appearance
