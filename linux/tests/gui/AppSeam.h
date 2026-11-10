// AppSeam.h — what the GUI checks may reach of the running app.
//
// main.cpp keeps the App struct and the shared state to itself. When it is
// compiled for minicode-guitest (MINICODE_GUITEST), AppSeam.inc is included at
// the end of that file and implements this interface, so there is one place
// that hands out the app's windows and nothing else has to be global. A later
// item that needs more of a window adds a field here and a line in
// AppSeam.inc.
#pragma once

#include <gtk/gtk.h>
#include <string>
#include <vector>

class Editor;
class FileTree;
class FindBar;
class SearchPanel;
class GitPanel;
class LspSession;
class PalettePanel;
class Terminal;
class Browser;
class WindowLayout;
class NavHistoryUi;
class ConsentBar;
class Formatting;

// One editor window's parts. Pointers are the app's own and stay valid until
// the window closes; terminal and browser are null in a build without them.
struct MCGuiAppWindow {
    GtkWindow*  window = nullptr;
    Editor*     editor = nullptr;
    FileTree*   tree = nullptr;
    GitPanel*   git = nullptr;   // null until the Source Control panel was first shown
    LspSession* lsp = nullptr;
    Terminal*   terminal = nullptr;
    Browser*    browser = nullptr;
    WindowLayout* layout = nullptr;        // the panes and their bars (WindowLayout.h)
    GtkWidget*  statusLabel = nullptr;
    GtkWidget*  settingsLabel = nullptr;   // problems in settings.conf and keys.conf
    GtkWidget*  statusBar = nullptr;       // the bar holding both (its right click: 2.11)
    FindBar*    findBar = nullptr;         // the editor's find and replace bar
    SearchPanel* search = nullptr;         // Find in Folder, null until first shown
    GtkWidget*  hintsPanel = nullptr;      // the shortcut hints, and their text
    GtkWidget*  hintsLabel = nullptr;
    GtkWidget*  hintsHint = nullptr;       // the status bar's note on them
    PalettePanel* palette = nullptr;       // the palette over the panes
    NavHistoryUi* nav = nullptr;           // Go Back and Go Forward's history
    ConsentBar* consentBar = nullptr;      // the folder's own tools' question (2.3)
    Formatting* format = nullptr;          // Format Document (2.4)
    std::string rootDir;
};

GtkApplication* MCGuiApplication();
std::vector<MCGuiAppWindow> MCGuiAppWindows();   // open windows, oldest first
MCGuiAppWindow MCGuiApp();                       // the newest; empty with none open

// The file in the newest window, as a single click in the tree opens it: in
// its tab if it has one, else in the preview tab (EditorTabs.h). The window's
// tabs and the one on show are its editor's (Editor::documents, active).
void MCGuiOpenFile(const std::string& path);
// The same in a tab of its own, as a double-click or a link opens it.
void MCGuiOpenFileKept(const std::string& path);
// The settings file, read again now (the app also watches it).
void MCGuiReloadSettings();
// keys.conf, the same.
void MCGuiReloadKeys();
// Every window, closed as the app closes one, with no questions about unsaved
// text.
void MCGuiCloseAllWindows();
// A new window on `folder`, as a second `minicode <folder>` opens one (the
// Mac's suites open theirs through application:openURLs:).
void MCGuiOpenWindow(const std::string& folder);
