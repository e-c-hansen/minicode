// ConsentBar.h — the one bar under the tab strip, the Mac's MCInstallBar
// (InstallPanel.mm, EditorController+LanguageServers.mm updateInstallBar).
// It asks about the folder's own tools for the language servers (PARITY.md
// 2.3), and otherwise offers to install a missing server (2.8):
//   - over a Python file, the folder's own environment (".venv"), which the
//     servers leave out until allowed: Use It, Don't Use It, ×;
//   - a server that runs without what would run the folder's code, or waits
//     (rust-analyzer, the TypeScript server): Allow, Don't Allow, ×;
//   - the file on show's server is not installed: what that costs,
//     Install… (a popover with the commands, InstallView.h), Don't Show
//     Again, ×.
// The answer is the folder's (FolderConsent.h), shared with Format
// Document's question, and every window on the folder follows it. × puts
// the question off until minicode starts again, or until Ask Again About
// This Folder's Tools takes the answer back. The install offer comes once a
// language a launch, across windows, until ×, Don't Show Again (kept in
// $XDG_STATE_HOME/minicode/install-hints.json, D11), or the server turning
// up (LspSession::lookAgainForMissingServer).
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <set>
#include <string>

class InstallView;

class ConsentBar {
public:
    struct Host {
        std::function<std::string()> root;   // the window's folder
        // What to ask about now, "" for nothing (LspSession's
        // folderEnvironmentAwaitingConsent, folderCodeAwaitingConsent and
        // folderCodeQuestion).
        std::function<std::string()> environment;
        std::function<std::string()> server;
        std::function<std::string()> serverQuestion;
        // The install offer: the file on show's server key whose server is
        // not installed, "" for none (LspSession::missingServer); the file on
        // show's server key; the settings' own command for a key ("" when
        // none); and Run in Terminal, which types a command for the user.
        std::function<std::string()> missing;
        std::function<std::string()> language;
        std::function<std::string(const std::string& key)> setting;
        std::function<void(const std::string& command)> runInTerminal;
    };

    explicit ConsentBar(Host host);
    ~ConsentBar();
    ConsentBar(const ConsentBar&) = delete;
    ConsentBar& operator=(const ConsentBar&) = delete;

    // The bar, for main.cpp to put under the tab strip (Editor::addBar).
    GtkWidget* widget() const { return box_; }
    // Shows, changes or hides it for what the session wants asked now.
    // Called whenever the language server status changes.
    void refresh();

    // For checks: the environment or server key it asks about ("" when
    // hidden or offering an install), its words, and its buttons.
    const std::string& asking() const { return asking_; }
    std::string text() const;
    GtkWidget* allowButton() const { return allow_; }
    GtkWidget* denyButton() const { return deny_; }
    GtkWidget* closeButton() const { return close_; }
    // The server key whose install it offers ("" when not offering), its
    // Install… button (a menu button whose popover holds an InstallView),
    // Don't Show Again (denyButton) and ×; the view while its popover shows.
    const std::string& installing() const { return installing_; }
    GtkWidget* installButton() const { return install_; }
    InstallView* installView() const;

    // Don't Show Again, per server key, as kept on disk (read each time).
    static bool installMuted(const std::string& key);
    static void setInstallMuted(const std::string& key, bool muted);
    // For checks: every language may be offered again this launch.
    static void forgetInstallOffers();

private:
    void answer(bool allow);
    void putOff();
    void deny();
    bool refreshInstall();
    void fillInstallPopover();

    Host host_;
    GtkWidget* box_ = nullptr;
    GtkWidget* label_ = nullptr;
    GtkWidget* allow_ = nullptr;
    GtkWidget* install_ = nullptr;   // GtkMenuButton, shown for the install offer
    GtkWidget* popover_ = nullptr;
    GtkWidget* deny_ = nullptr;
    GtkWidget* close_ = nullptr;
    std::string asking_;
    bool server_ = false;   // asking about a server, not an environment
    // The install offer: the key this window took the offer for (the
    // Mac's _installBarKey), and the key the bar shows it for now.
    std::string installKey_, installing_;
    int listener_ = 0;
};
