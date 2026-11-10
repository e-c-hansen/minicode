// Lsp.h — the GTK half of the language server client: the servers a window
// runs, the documents they have open, and what the editor shows from them
// (error underlines, the completion list, go to definition, hover info). The
// protocol itself is the shared ../src/LspClient.{h,cpp}; the macOS
// counterpart of this file is src/Lsp.h, and of its parts src/Lsp+*.mm.
//
// main.cpp owns one LspSession per window and makes it the Editor's observer,
// so the session hears about every file opened, saved and edited without
// hooks of its own in main.cpp's open and save paths. What main.cpp does
// call: setRoot (Open Folder), applySettings (settings file changed),
// shutdown (window closing), the actions, and LspTerminateAllServers at
// quit.
//
// Every tab's file stays open on its server (PARITY.md item 2.0, the Mac's
// syncOpenDocuments): each document's own buffer is its text, its edits go
// to the server as they are made (ranged where the server takes ranges),
// and it is closed there when its tab closes. Only a server already running
// takes a tab in the background; the file on show is what starts one.
//
// The session is split by concern, as the Mac's LspSession is split into
// categories (PARITY.md item 2.0). Each part is its own file, with its state
// in a struct of its own (LspSessionPrivate.h), and the methods below are
// grouped by the file that has them:
//   Lsp.cpp                    the session's life: made, destroyed, its buffer
//   LspProcess.{h,cpp}         one server process (lspgtk::Server)
//   LspSessionServers.cpp      choosing, starting and stopping servers; status
//   LspSessionSync.cpp         the documents the servers have open
//   LspSessionDiagnostics.cpp  squiggles and their messages
//   LspSessionCompletion.cpp   the completion list
//   LspSessionNavigation.cpp   hover and go to definition
//   LspSessionActions.cpp      rename, references, quick fixes, applyEdit
//   LspSessionEdits.cpp        the workspace edit applier, every file or none
//   LspPicker.{h,cpp}          the references and fixes list, the rename box
// A later item adds a file of its own and a struct for its state.
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

namespace lspgtk {
class Server;
class Picker;
struct PickerRow;
class RenameBox;
}

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
    // The window is closing: close the documents, shut the servers down
    // (shutdown, exit, then signals if they linger).
    void shutdown();

    // Actions. Each rings the bell and says why in the status bar when the
    // file has no server.
    void triggerCompletion();   // Ctrl+Space
    void goToDefinition();      // F12, at the caret
    void showHoverInfo();       // Ctrl+Alt+I, at the caret
    void renameSymbol();        // F2: prepareRename where offered, then a box for the name
    void findReferences();      // Shift+F12: a list by the caret, grouped by file
    void showQuickFixes();      // Ctrl+.: the server's code actions for the caret

    // The workspace edit applier (LspSessionEdits.cpp), which rename, quick
    // fixes, a server's workspace/applyEdit and Format Document's server
    // fallback share: every file of the edit or none, each read and checked
    // (a tab's text, else the disk's; a range the server computed on other
    // text than the editor has must still read the same), a file with no
    // tab opened in one, then each file's edits as one multi-range
    // replacement (MultiEdit::apply), one undo step per file. A failure part
    // way puts back what was edited and closes the tabs it opened. Nothing
    // is saved, and the tab on show stays on show. Returns how many files
    // changed, or -1 with *failure saying why.
    int applyWorkspaceEdit(const Lsp::WorkspaceEdit& we, std::string* failure);

    // EditorObserver
    void documentChanged(const std::string& path) override;
    void documentSaved(Document* d) override;
    void textEdited(const char* inserted, int len) override;
    void tabsChanged() override;
    bool takesReturn() const override { return completionVisible() || pickerVisible(); }

    // Test hooks.
    bool completionVisible() const;
    std::vector<std::string> completionLabels() const;
    const std::string& statusText() const { return status_; }
    const std::string& lastHoverText() const;
    bool completionKey(guint keyval);   // what the key controller does
    // The tooltip text at a buffer position; *requested is set when a hover
    // request went out for it (the tooltip is re-queried when it answers).
    std::string tooltipAt(const GtkTextIter* it, bool* requested = nullptr);
    void definitionAt(const GtkTextIter* it);
    // The documents open on the servers, as "<server key> <path>", sorted;
    // and how many didChange notifications went out ranged and whole.
    std::vector<std::string> openDocuments() const;
    std::size_t rangedChangesSent() const;
    std::size_t wholeChangesSent() const;
    // The last note in the status bar ("Renamed 4 occurrences in 2 files").
    const std::string& lastNote() const { return lastNote_; }
    // The references or quick fix list: "references", "fixes" or "" when
    // none shows; its rows as text, the selected one, and a row's widget.
    bool pickerVisible() const;
    std::string pickerKind() const;
    std::vector<std::string> pickerRows() const;
    int pickerSelectedRow() const;
    GtkWidget* pickerRowWidget(int row) const;
    // The rename box, and its field.
    bool renameBoxVisible() const;
    GtkWidget* renameEntry() const;

    // The parts' state (LspSessionPrivate.h).
    struct SyncState;
    struct DiagState;
    struct CompletionState;
    struct NavState;
    struct ActionsState;

private:
    // ---- shared by the parts (Lsp.cpp)
    Editor*        editor_;
    GtkWidget*     view_;
    GtkTextBuffer* buffer_ = nullptr;   // the tab on show's source, held
    std::string    root_;
    Settings       settings_;
    std::shared_ptr<int> life_ = std::make_shared<int>(0);   // callbacks check it
    // The file on show: its path even with no server, its URI and server
    // when one has it open, and why there is none.
    std::string path_;
    std::string uri_;
    std::shared_ptr<lspgtk::Server> server_;
    std::string note_;
    int docGen_ = 0;   // bumped by every edit of the file on show and every switch
    bool applyingEdit_ = false;   // the applier is at work: no typing, no lists closing
    std::string lastNote_;
    void useBuffer(GtkTextBuffer* b);
    // `fn` once what is being handled now is done (an answer read, a key
    // press), unless the session is gone by then.
    void later(std::function<void()> fn);
    Lsp::Position positionOf(const GtkTextIter* it) const;
    void iterAt(Lsp::Position p, GtkTextIter* it) const;
    void cursorIter(GtkTextIter* it) const;
    void caretRect(const GtkTextIter* it, GdkRectangle* r) const;
    static gboolean onKey(GtkEventControllerKey* c, guint keyval, guint keycode,
                          GdkModifierType state, gpointer self);
    static void onFocusLeave(GtkEventControllerFocus* c, gpointer self);
    static void onMarkSet(GtkTextBuffer* b, GtkTextIter* it, GtkTextMark* m, gpointer self);
    static void onUnrealize(GtkWidget* view, gpointer self);
    void dropPopovers();

    // ---- servers and the status bar (LspSessionServers.cpp)
    // key -> running server; notes_ holds why a key has none ("" = switched off).
    std::map<std::string, std::shared_ptr<lspgtk::Server>> servers_;
    std::map<std::string, std::string> notes_;
    std::string signature_;   // the lsp.* settings the servers were started with
    bool drawn_ = false;      // the window has drawn its first frame (WhenDrawn.h)
    bool held_ = false;       // a document came before that, and waits for it
    std::string transient_;   // a brief message ("No definition found")
    std::string status_;
    guint flashId_ = 0;
    void updateStatus();
    void flash(const std::string& message);
    void note(const std::string& message);   // flash, kept as lastNote_
    bool noServer();              // bell + note when there is no server
    std::string settingsSignature(const Settings& s) const;
    // The running server for a settings key, started when there is none;
    // null (and note_ says why) when it cannot be.
    std::shared_ptr<lspgtk::Server> serverForKey(const std::string& key);
    // The one place a server's program is chosen and started (item 2.3's
    // consent hooks in here): null when none could be, with the note why.
    std::shared_ptr<lspgtk::Server> launchServer(const std::string& key, std::string* note);
    std::shared_ptr<lspgtk::Server> runningServer(const std::string& key) const;
    void serverExited(lspgtk::Server* s);
    void initializeFailed(lspgtk::Server* s, const std::string& message);
    void stopAllServers();
    bool languageOf(const std::string& path, Lsp::Language& lang) const;

    // ---- documents (LspSessionSync.cpp)
    std::unique_ptr<SyncState> sync_;
    // The file on show leaves the view; it stays open on its server while its
    // tab is open.
    void detachDocument();
    void closeDocument();
    // Every tab's file open on its running server with its own text, files
    // whose tabs went closed; called on show, on a server's start, before a
    // request about other files, and when the tabs change.
    void syncOpenDocuments();
    void scheduleSync();
    void forgetServerDocuments(lspgtk::Server* s);
    void flushChanges();
    void scheduleChange();
    void watchBuffers();
    void noteEdit(uint64_t docId, size_t start, size_t removed, const char* inserted, int len);
    static void onSyncInsert(GtkTextBuffer* b, GtkTextIter* at, char* text, int len,
                             gpointer watch);
    static void onSyncDelete(GtkTextBuffer* b, GtkTextIter* a, GtkTextIter* z, gpointer watch);

    // ---- diagnostics (LspSessionDiagnostics.cpp)
    std::unique_ptr<DiagState> diag_;
    void makeDiagnosticTags();
    void diagnosticsArrived(lspgtk::Server* s, const std::string& uri,
                            const std::vector<Lsp::Diagnostic>& list);
    void paintDiagnostics();
    void clearMarks();
    void forgetDiagnostics(const std::string& uri);
    void forgetAllDiagnostics();
    void countProblems(int& errors, int& warnings) const;
    bool sameFile(const std::string& uriA, const std::string& uriB) const;
    std::string diagnosticAt(const GtkTextIter* it) const;
    std::vector<Lsp::Diagnostic> diagnosticsOf(const std::string& uri) const;

    // ---- completion (LspSessionCompletion.cpp)
    std::unique_ptr<CompletionState> cmp_;
    void completionAfterEdit(const char* inserted, int len);
    void completionFollowsCaret();
    void requestCompletion(const std::string& trigger);
    void completionArrived(const Json& result, int gen, const std::string& uri);
    void refilter();
    void scheduleRefilter();
    void closeCompletion();
    void cancelCompletionTrigger();
    void acceptCompletion(int row);
    void buildPopover();
    void moveSelection(int delta);
    int selectedRow() const;
    void forgetCompletionAnchor();
    void dropCompletionPopover();
    static void onRowActivated(GtkListBox* box, GtkListBoxRow* row, gpointer self);

    // ---- hover and definition (LspSessionNavigation.cpp)
    std::unique_ptr<NavState> nav_;
    void forgetHover();
    void closeHover();
    void dropHoverPopover();
    void definitionArrived(const Json& result, int gen, const std::string& uri);
    void hoverArrived(const Json& result, int gen, const std::string& uri, long key,
                      bool popup);
    void showHoverPopover(const std::string& text);
    void installNavigation();
    // A location's file opened (through onReveal, a jump for Go Back, when
    // it is another file) and its range selected.
    void revealLocation(const Lsp::Location& loc);
    static void onCtrlClick(GtkGestureClick* g, int n, double x, double y, gpointer self);
    static gboolean onQueryTooltip(GtkWidget* w, int x, int y, gboolean keyboard,
                                   GtkTooltip* tip, gpointer self);

    // ---- the workspace edit applier (LspSessionEdits.cpp)
    // `server` computed the edit and `asked` holds its documents' versions
    // when it was asked for (null: not known), to refuse a file that moved
    // on since.
    int applyEdit(const Lsp::WorkspaceEdit& we, lspgtk::Server* server,
                  const Lsp::DocVersions* asked, std::string* failure);
    std::string treePathForUri(const std::string& uri) const;
    Document* documentForPath(const std::string& path) const;
    std::shared_ptr<const std::u16string> serverTextOf(const std::string& uri) const;

    // ---- rename, references, quick fixes (LspSessionActions.cpp)
    std::unique_ptr<ActionsState> act_;
    void closeTransients();
    void closePicker();
    void closeRenameBox();
    void forgetActionRequests();
    void dropActionPopovers();
    void actionsFollowCaret();
    bool pickerKey(guint keyval);
    void showPicker(const std::vector<lspgtk::PickerRow>& rows, int select, int width,
                    int maxRows);
    void acceptPickerRow(int row);
    bool serverOffers(Lsp::Feature what, const char* missing);
    void rememberAsked();
    bool stillAsAsked(int gen, int wanted, const std::string& uri) const;
    std::pair<int, int> nameRangeAt(int offset) const;
    void openRename(int a, int b, int gen, const std::string& placeholder);
    void commitRename(const std::string& name, int gen);
    std::string editorTextOf(const std::string& path) const;
    void referencesArrived(const Json& result, const Json& error, int gen, const std::string& uri);
    void runCodeAction(const Lsp::CodeAction& action, std::shared_ptr<lspgtk::Server> server);
    void performCodeAction(const Lsp::CodeAction& action, std::shared_ptr<lspgtk::Server> server);
    void serverAppliesEdit(const std::weak_ptr<lspgtk::Server>& server, const Json& params,
                           Lsp::Client::ApplyEditReply reply);
};

// Stop every server the app started, now: shutdown and exit, then SIGTERM and
// SIGKILL for any still running a moment later. For the application's
// "shutdown" signal; it waits (up to about a second) without the main loop.
void LspTerminateAllServers();
// How many server processes are running (tests).
std::size_t LspRunningServerCount();
// Their process ids (tests).
std::vector<int> LspServerPids();
