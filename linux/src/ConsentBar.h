// ConsentBar.h — the bar under the tab strip that asks about the folder's
// own tools for the language servers (PARITY.md 2.3), the Mac's MCInstallBar
// in its consent modes (InstallPanel.mm, EditorController+LanguageServers.mm
// updateInstallBar):
//   - over a Python file, the folder's own environment (".venv"), which the
//     servers leave out until allowed: Use It, Don't Use It, ×;
//   - a server that runs without what would run the folder's code, or waits
//     (rust-analyzer, the TypeScript server): Allow, Don't Allow, ×.
// The answer is the folder's (FolderConsent.h), shared with Format
// Document's question, and every window on the folder follows it. × puts
// the question off until minicode starts again, or until Ask Again About
// This Folder's Tools takes the answer back. Item 2.8 (install hints) is to
// make this the one bar under the strip, offering a missing server too, as
// the Mac's does.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>

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
    // hidden), its words, and its buttons.
    const std::string& asking() const { return asking_; }
    std::string text() const;
    GtkWidget* allowButton() const { return allow_; }
    GtkWidget* denyButton() const { return deny_; }
    GtkWidget* closeButton() const { return close_; }

private:
    void answer(bool allow);
    void putOff();

    Host host_;
    GtkWidget* box_ = nullptr;
    GtkWidget* label_ = nullptr;
    GtkWidget* allow_ = nullptr;
    GtkWidget* deny_ = nullptr;
    GtkWidget* close_ = nullptr;
    std::string asking_;
    bool server_ = false;   // asking about a server, not an environment
    int listener_ = 0;
};
