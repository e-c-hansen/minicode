// ProblemsPanel.h — the Problems list (Ctrl+Shift+M, VS Code's Linux key;
// PARITY.md item 2.2), the Mac's src/ProblemsPanel.{h,mm} and its hooks in
// EditorController+LanguageServers.mm: every diagnostic the window's
// language servers have published, a heading per file and a row per
// problem (severity mark, message, source and code, line and column),
// docked across the right side above the terminal (WindowLayout), and the
// status bar's counts beside it, which a click opens and closes it with.
// LspSession gives the rows (problemRows); this shows them and opens one
// through the session.
//
// Keyboard first: Ctrl+Shift+M shows it with the keyboard in the list,
// pressed in it hides it, pressed elsewhere while it shows gives it the
// keyboard. Up and Down move through the problems (headings are skipped),
// Page Up, Page Down, Home and End too; Enter or a click opens one; Escape
// gives the keyboard back to the editor, the list staying open; Ctrl+W
// (main.cpp's Close Tab) and the × in its heading close it.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Lsp.h"

class ProblemsPanel {
public:
    struct Host {
        std::function<std::string()> root;           // the window's folder
        std::function<void(bool shown)> show;        // the layout shows or hides it
        std::function<void()> focusEditor;           // Escape
        std::function<void()> focusAfterClosing;     // it closed with the keyboard in it
    };

    ProblemsPanel(LspSession* lsp, Host host);
    ~ProblemsPanel();
    ProblemsPanel(const ProblemsPanel&) = delete;
    ProblemsPanel& operator=(const ProblemsPanel&) = delete;

    // The panel, for the layout, and the counts, for the status bar.
    GtkWidget* widget() const { return root_; }
    GtkWidget* statusLabel() const { return status_; }

    // Ctrl+Shift+M (`fromKey`) and the counts' click: shown with the
    // keyboard in the list; with the keyboard in it already (or from the
    // click), hidden; from the key elsewhere while it shows, the keyboard
    // given to it.
    void toggle(bool fromKey);
    void hide();
    bool shown() const { return shown_; }
    bool hasFocus() const;
    void focusList();
    // The servers' diagnostics changed (LspSession::onProblemsChanged), or
    // their state (onStatus): the counts, and the rows while it shows.
    void update();

    // For checks: each row as text ("# main.py — 2 errors" for a heading,
    // "E 3:5 message [source(code)]" for a problem), the selected row, the
    // heading's counts, the status bar's text, and a row's widget on screen
    // (null when none is bound to it).
    std::vector<std::string> rowTexts() const;
    int selectedRow() const;
    std::string countsText() const;
    std::string statusText() const;
    GtkWidget* rowWidget(int row) const;
    GtkWidget* list() const { return list_; }
    const std::vector<LspProblem>& rows() const { return rows_; }

private:
    void setRows(std::vector<LspProblem> rows, const std::string& counts);
    void updateCounts();
    bool isProblem(int row) const;
    bool moveBy(int step);
    void select(int row);
    void open(int row);
    std::string folderOf(const std::string& path) const;
    void bindRow(GtkListItem* item);
    static gboolean onKey(GtkEventControllerKey* c, guint keyval, guint keycode,
                          GdkModifierType state, gpointer self);

    LspSession* lsp_;
    Host host_;
    GtkWidget* root_ = nullptr;
    GtkWidget* counts_ = nullptr;   // the heading's
    GtkWidget* empty_ = nullptr;
    GtkWidget* scroll_ = nullptr;
    GtkWidget* list_ = nullptr;
    GtkWidget* status_ = nullptr;   // the status bar's
    GtkStringList* model_ = nullptr;
    GtkSingleSelection* selection_ = nullptr;
    std::vector<LspProblem> rows_;
    std::vector<GtkWidget*> bound_;   // each row's widget while one is bound to it
    std::string countsText_, statusText_;
    bool shown_ = false;
    // What the widgets' callbacks hold: this panel, set null when it goes
    // (a window's objects are deleted while its widgets still exist).
    std::shared_ptr<ProblemsPanel*> ref_;
};
