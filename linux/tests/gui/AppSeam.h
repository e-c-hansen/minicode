// AppSeam.h — what the GUI checks may reach of the running app.
//
// main.cpp keeps the App struct and the shared state to itself. When it is
// compiled for mutible-guitest (MUTIBLE_GUITEST), AppSeam.inc is included at
// the end of that file and implements this interface, so there is one place
// that hands out the app's windows and nothing else has to be global. A later
// item that needs more of a window adds a field here and a line in
// AppSeam.inc.
#pragma once

#include <gtk/gtk.h>
#include <functional>
#include <string>
#include <vector>

class Editor;
class AgentWorktrees;
class FileTree;
class FindBar;
class SearchPanel;
class GitPanel;
class LspSession;
class PalettePanel;
class Terminal;
class TerminalDock;
class Browser;
class WindowLayout;
class NavHistoryUi;
class ConsentBar;
class Formatting;
class FormatOffer;
class GitGutter;
class TestGutter;
class TreeGitColors;
class ProblemsPanel;
class ProcessesPanel;
class IdeLinkServer;
class AgentsPanel;

// One editor window's parts. Pointers are the app's own and stay valid until
// the window closes; terminal and browser are null in a build without them.
struct MCGuiAppWindow {
    GtkWindow*  window = nullptr;
    int         controlId = 0;     // what the control channel calls the window
    Editor*     editor = nullptr;
    FileTree*   tree = nullptr;
    GitPanel*   git = nullptr;   // null until the Source Control panel was first shown
    ProcessesPanel* procs = nullptr;   // null until View > Toggle Processes was first used
    bool        procsShown = false;
    IdeLinkServer* ide = nullptr;      // Claude Code's /ide for the window (3.9)
    AgentsPanel* agents = nullptr;     // null until View > Toggle Agents was first used
    AgentWorktrees* worktrees = nullptr;   // the tree's Worktrees node (3.5)
    bool        agentsShown = false;
    LspSession* lsp = nullptr;
    Terminal*   terminal = nullptr;     // the active one of termDock
    TerminalDock* termDock = nullptr;   // the terminal tabs (4.7)
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
    GtkWidget*  hintsSearch = nullptr;     // their search box (HintsSearchBox.h), a GtkSearchEntry
    PalettePanel* palette = nullptr;       // the palette over the panes
    NavHistoryUi* nav = nullptr;           // Go Back and Go Forward's history
    ConsentBar* consentBar = nullptr;      // the folder's own tools' question (2.3)
    Formatting* format = nullptr;          // Format Document (2.4)
    FormatOffer* formatOffer = nullptr;    // the bar offering format on save
    ProblemsPanel* problems = nullptr;     // the Problems list and its counts (2.2)
    GitGutter*  changeBars = nullptr;      // the editor's change bars (2.10)
    TestGutter* tests = nullptr;           // the ▶ beside each test (TestGutter.h)
    TreeGitColors* treeColors = nullptr;   // the tree's colors (2.10)
    std::string rootDir;
};

GtkApplication* MCGuiApplication();
// The menu bar's Bookmarks menu (4.8): Bookmark This Page, then the window
// in front's bookmarks: its .mutible/bookmarks, then "Shared" (~/Bookmarks).
GMenu* MCGuiBookmarksMenu();
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
// A new window on `folder`, as a second `mutible <folder>` opens one (the
// Mac's suites open theirs through application:openURLs:).
void MCGuiOpenWindow(const std::string& folder);
// A window's record as a launch keeps it (Persistence::windowRecord, written
// as TabSessions::encodeWindows writes the list), and a new window on
// `folder` brought back from such a record, as a launch that opens nothing
// brings its windows back (the Mac's openWindowAtPath:restoring:).
std::string MCGuiWindowRecord(GtkWindow* window);
void MCGuiOpenWindowRestoring(const std::string& folder, const std::string& record);
// What `mutible <args...>` does in the running app (args without argv[0]);
// what it would print for the terminal is appended to `*errors`.
void MCGuiCommandLine(const std::vector<std::string>& args, const std::string& cwd,
                      std::string* errors);
// File > Open Recent's menu (its folders, then Clear Recently Opened).
GMenu* MCGuiRecentMenu();
// The folder chooser (File > Open Folder, `mutible --open-folder`) answered
// by the check instead of the portal: `answer` gets the folder the chooser
// would start in and returns the one "chosen", "" for a cancel. An empty
// function puts the real chooser back.
void MCGuiAnswerFolderChooser(std::function<std::string(const std::string& start)> answer);
