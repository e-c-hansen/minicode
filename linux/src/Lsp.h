// Lsp.h — the GTK half of the language server client: the server processes,
// and what the editor shows from them (error underlines, the completion list,
// go to definition, hover info). The protocol itself is the shared
// ../src/LspClient.{h,cpp}; the macOS counterpart of this file is src/Lsp.mm.
//
// main.cpp owns one LspSession per window and makes it the Editor's observer,
// so the session hears about every file opened, saved and edited without
// hooks of its own in main.cpp's open and save paths. What main.cpp does call:
// setRoot (Open Folder), applySettings (settings file changed), shutdown
// (window closing), the three actions, and LspTerminateAllServers at quit.
#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Editor.h"
#include "LspClient.h"
#include "Settings.h"

namespace lspgtk { class Server; }

class LspSession : public EditorObserver {
public:
    LspSession(Editor* editor, const std::string& root, const Settings& settings);
    ~LspSession() override;
    LspSession(const LspSession&) = delete;
    LspSession& operator=(const LspSession&) = delete;

    // Status bar text: the server's name and the file's problem counts, or a
    // short note when there is no server. Called on every change.
    std::function<void(const std::string&)> onStatus;
    // Open another file at a 1-based line, selecting byteLength bytes from
    // byteColumn: the Find in Folder path (openSearchMatch in main.cpp).
    std::function<void(const std::string& path, int line, std::size_t byteColumn,
                       std::size_t byteLength)> onReveal;

    // The folder changed: every server stops, they belong to the old root.
    void setRoot(const std::string& root);
    // A different lsp.* setting restarts the servers.
    void applySettings(const Settings& settings);
    // The window is closing: close the document, shut the servers down
    // (shutdown, exit, then signals if they linger).
    void shutdown();

    // Actions. Each rings the bell and says why in the status bar when the
    // file has no server.
    void triggerCompletion();   // Ctrl+Space
    void goToDefinition();      // F12, at the caret
    void showHoverInfo();       // Ctrl+I, at the caret

    // EditorObserver
    void documentChanged(const std::string& path) override;
    void documentSaved() override;
    void textEdited(const char* inserted, int len) override;

    // Test hooks.
    bool completionVisible() const;
    std::vector<std::string> completionLabels() const;
    const std::string& statusText() const { return status_; }
    const std::string& lastHoverText() const { return lastHover_; }
    bool completionKey(guint keyval);   // what the key controller does
    // The tooltip text at a buffer position; *requested is set when a hover
    // request went out for it (the tooltip is re-queried when it answers).
    std::string tooltipAt(const GtkTextIter* it, bool* requested = nullptr);
    void definitionAt(const GtkTextIter* it);

private:
    struct Mark {
        GtkTextMark* start = nullptr;
        GtkTextMark* end = nullptr;
        int severity = 1;
        std::string message;
    };

    Editor*        editor_;
    GtkWidget*     view_;
    GtkTextBuffer* buffer_;
    std::string    root_;
    Settings       settings_;
    std::string    signature_;   // the lsp.* settings the servers were started with
    std::shared_ptr<int> life_ = std::make_shared<int>(0);   // callbacks check it

    // key -> running server; notes_ holds why a key has none ("" = switched off).
    std::map<std::string, std::shared_ptr<lspgtk::Server>> servers_;
    std::map<std::string, std::string> notes_;
    std::string path_;       // the file in the editor, even with no server
    std::string uri_;        // its URI, when a server has it open
    std::shared_ptr<lspgtk::Server> server_;
    std::string note_;       // why there is no server
    std::string transient_;  // a brief message ("No definition found")
    std::string status_;
    guint flashId_ = 0;
    guint changeTimer_ = 0;
    std::map<std::string, std::vector<Lsp::Diagnostic>> diags_;
    std::vector<Mark> marks_;
    GtkTextTag* tags_[4] = {};   // error, warning, information, hint

    // Completion.
    GtkWidget* popover_ = nullptr;
    GtkWidget* list_ = nullptr;
    GtkWidget* listScroll_ = nullptr;
    GtkTextMark* anchor_ = nullptr;   // where the word being completed starts
    std::vector<Lsp::CompletionItem> items_, shown_;
    bool completing_ = false;
    bool gotItems_ = false;
    int completionGen_ = 0, definitionGen_ = 0, hoverGen_ = 0;
    guint refilterId_ = 0;
    guint triggerId_ = 0;         // a typed trigger character's request, next idle
    std::string pendingTrigger_;

    // Hover.
    GtkWidget* hoverPopover_ = nullptr;
    GtkWidget* hoverLabel_ = nullptr;
    long hoverKey_ = -1;          // word start (characters) the hover text is for
    long hoverAsked_ = -1;        // word start a request is out for
    std::string hoverText_;
    std::string lastHover_;
    int docGen_ = 0;              // bumped by every edit and document switch

    void updateStatus();
    void flash(const std::string& message);
    bool noServer();              // bell + note when there is no server
    std::string settingsSignature(const Settings& s) const;
    std::shared_ptr<lspgtk::Server> serverForKey(const std::string& key);
    void serverExited(lspgtk::Server* s);
    void initializeFailed(lspgtk::Server* s, const std::string& message);
    void stopAllServers();
    void closeDocument();
    void flushChanges();
    void scheduleChange();

    void diagnosticsArrived(const std::string& uri, const std::vector<Lsp::Diagnostic>& list);
    void clearMarks();
    bool sameFile(const std::string& uriA, const std::string& uriB) const;

    Lsp::Position positionOf(const GtkTextIter* it) const;
    void iterAt(Lsp::Position p, GtkTextIter* it) const;
    void cursorIter(GtkTextIter* it) const;
    std::string diagnosticAt(const GtkTextIter* it) const;

    void requestCompletion(const std::string& trigger);
    void completionArrived(const Json& result, int gen, const std::string& uri);
    void refilter();
    void scheduleRefilter();
    void closeCompletion();
    void acceptCompletion(int row);
    void buildPopover();
    void moveSelection(int delta);
    int selectedRow() const;
    void caretRect(const GtkTextIter* it, GdkRectangle* r) const;

    void definitionArrived(const Json& result, int gen, const std::string& uri);
    void hoverArrived(const Json& result, int gen, const std::string& uri, long key,
                      bool popup);
    void showHoverPopover(const std::string& text);

    static gboolean onKey(GtkEventControllerKey* c, guint keyval, guint keycode,
                          GdkModifierType state, gpointer self);
    static void onCtrlClick(GtkGestureClick* g, int n, double x, double y, gpointer self);
    static gboolean onQueryTooltip(GtkWidget* w, int x, int y, gboolean keyboard,
                                   GtkTooltip* tip, gpointer self);
    static void onFocusLeave(GtkEventControllerFocus* c, gpointer self);
    static void onMarkSet(GtkTextBuffer* b, GtkTextIter* it, GtkTextMark* m, gpointer self);
    static void onRowActivated(GtkListBox* box, GtkListBoxRow* row, gpointer self);
    static void onUnrealize(GtkWidget* view, gpointer self);
    void dropPopovers();
};

// Stop every server the app started, now: shutdown and exit, then SIGTERM and
// SIGKILL for any still running a moment later. For the application's
// "shutdown" signal; it waits (up to about a second) without the main loop.
void LspTerminateAllServers();
// How many server processes are running (tests).
std::size_t LspRunningServerCount();
// Their process ids (tests).
std::vector<int> LspServerPids();
