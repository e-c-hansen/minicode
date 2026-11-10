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
//   LspSessionCompletion.cpp   the completion list, its edits and snippets
//   LspSessionSignature.cpp    the signature hint while typing a call
//   LspSessionNavigation.cpp   hover and go to definition
//   LspSessionActions.cpp      rename, references, quick fixes, applyEdit
//   LspSessionEdits.cpp        the workspace edit applier, every file or none
//   LspPicker.{h,cpp}          the references and fixes list, the rename box
//   LspSessionFolderTools.cpp  the folder's own tools: what a server may run
//   LspSessionFormatting.cpp   Format Document through the file's server
//   LspSessionAlso.cpp         the second server (Ruff beside the Python
//                              server), and which of a file's servers
//                              answers a request
//   LspSessionProblems.cpp     the Problems list's rows, F8 and Shift+F8
//   LspSessionWatch.cpp        files changed on disk, for the servers'
//                              registered watchers (LspWatcher.{h,cpp})
//   LspSessionImports.cpp      Organize Imports, and on save
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

// One row of the Problems list (LspSession::problemRows): a heading for a
// file with its counts, or one of its problems, as the Mac's MCProblem.
struct LspProblem {
    bool heading = false;
    std::string path;              // the file, as the tree spells it
    // Where it is now: 0-based lines, UTF-16 columns.
    int line = 0, column = 0, endLine = 0, endColumn = 0;
    int severity = 1;              // 1 error, 2 warning, 3 information
    std::string message;           // on one line (Lsp::problemOneLine)
    std::string fullMessage;       // as the server sent it
    std::string source, code;
    int errors = 0, warnings = 0, others = 0;   // a heading's counts
};

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

    // ---- the folder's own tools (LspSessionFolderTools.cpp, PARITY.md 2.3)
    // Until the folder's answer (FolderConsent.h) is "allow", the Python
    // servers neither run the folder's own environment nor are looked for
    // in it, rust-analyzer starts without build scripts and proc macros (or
    // waits), and the TypeScript server uses the user's own TypeScript (or
    // waits): the Mac's Lsp+Python.mm and checkProjectCode. An answer given
    // or taken back starts those servers again.
    // The folder's own Python environment to ask about for a Python file on
    // show (".venv"), or "".
    std::string folderEnvironmentAwaitingConsent() const;
    // The file on show's server that runs without what would run the
    // folder's code, or waits for the answer ("rust", "typescript"), or "";
    // and the bar's question about it.
    std::string folderCodeAwaitingConsent() const;
    std::string folderCodeQuestion() const;
    // The interpreter the Python servers get ("" leaves them to choose) and
    // where it came from ("settings", "automatic; the folder's own
    // environment waits for your answer").
    std::string serverPythonInterpreter(std::string* source = nullptr) const;
    const std::string& root() const { return root_; }

    // ---- Format Document through the file's server
    // (LspSessionFormatting.cpp, PARITY.md 2.4), for a file no formatter is
    // planned for (Formatter::serverMayFormat).
    // A running server for the file on show that offers formatting.
    bool canFormatDocument() const;
    // textDocument/formatting (or rangeFormatting for selected characters,
    // where offered), its edits through the workspace edit applier as one
    // undo step. False when no server
    // formats the file; else `done` comes once: with no note, a note
    // ("Already formatted (fakelsp)."), or an error (the server's
    // complaint, or no answer within 5 s).
    bool formatDocument(long selStart, long selEnd,
                        std::function<void(const std::string& note, bool error)> done);

    // ---- the Problems list (LspSessionProblems.cpp, PARITY.md 2.2)
    // Every diagnostic the window's servers published, a heading per file
    // (files with errors first) and a row per problem, placed in each open
    // tab's text as it is now; hints are left out, as VS Code's Problems
    // leaves them. onProblemsChanged comes 50 ms after the last change.
    std::function<void()> onProblemsChanged;
    std::vector<LspProblem> problemRows();
    // "2 errors, 1 warning", "No problems"; and the counts alone.
    std::string problemCountsText();
    int problemErrors();
    int problemWarnings();
    // Something to count: a server publishing or running for the window.
    bool problemsKnown() const;
    // A problem's file opened (a jump for Go Back when it is another) and
    // its range selected.
    void openProblem(const LspProblem& problem);
    // F8 and Shift+F8: the next or previous problem in the file on show,
    // wrapping, its message in the box at the caret, which the next caret
    // move closes.
    void nextProblem();
    void previousProblem();

    // ---- Organize Imports (LspSessionImports.cpp, PARITY.md 2.2)
    // Shift+Alt+O: the servers' source.organizeImports action for the whole
    // file, a tool's own (source.organizeImports.ruff) before a general
    // server's (Lsp::pickOrganizeImports), through the workspace edit
    // applier.
    void organizeImports();
    // editor.organize-imports-on-save, before Ctrl+S formats: false when no
    // server of the file offers code actions (the save goes on); else
    // `done` comes once, after the edit, nothing to do, a failure, or 5 s.
    bool organizeImportsBeforeSave(std::function<void()> done);

    // EditorObserver
    void documentChanged(const std::string& path) override;
    void documentSaved(Document* d) override;
    void textEdited(const char* inserted, int len) override;
    void tabsChanged() override;
    bool takesReturn() const override { return completionVisible() || pickerVisible(); }
    bool listKey(guint keyval, GdkModifierType mods) override;

    // Test hooks.
    bool completionVisible() const;
    std::vector<std::string> completionLabels() const;
    std::string completionSelectedLabel() const;   // "" with none
    // The signature hint, as plain text ("" when none shows).
    std::string signatureText() const;
    const std::string& statusText() const { return status_; }
    const std::string& lastHoverText() const;
    // A completion accepted whose server's resolve answer may still bring
    // an import (a second at most).
    std::size_t pendingAccepts() const;
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
    // The hover box (Ctrl+Alt+I, F8's message) shows.
    bool hoverVisible() const;
    // The file on show's servers by name, the main one first ("fakelsp",
    // "ruff").
    std::vector<std::string> fileServerNames() const;
    // Watched files (LspSessionWatch.cpp): the folders watched (and every
    // folder below them, counted), the batches handed over, those the main
    // thread had to match itself, the main thread's CPU time for the last
    // one, and changes put through the watcher as a batch from the disk
    // would go (type 1 created, 2 changed, 3 deleted).
    std::vector<std::string> watchedFolders() const;
    std::size_t watchedFolderCount() const;
    std::size_t watchedBatches() const;
    std::size_t watchedBatchesMatchedHere() const;
    double lastWatchedBatchCpuMs() const;
    void injectWatchedPaths(const std::vector<std::string>& paths, int type);

    // The parts' state (LspSessionPrivate.h).
    struct SyncState;
    struct DiagState;
    struct CompletionState;
    struct SignatureState;
    struct NavState;
    struct ActionsState;
    struct FolderToolsState;
    struct AlsoState;
    struct ProblemsState;
    struct WatchState;

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
    std::shared_ptr<lspgtk::Server> server_;   // its main server
    std::string currentKey_;   // the file on show's server key, "" for none
    // Its second server (Ruff beside the Python server), when it has one:
    // the key ("python.also") and the server, running.
    std::string alsoKey_;
    std::shared_ptr<lspgtk::Server> alsoServer_;
    std::string note_;
    int docGen_ = 0;   // bumped by every edit of the file on show and every switch
    // The applier, or a completion accepted, is putting edits in: they are not
    // typing (no list or hint opens for them), and no list closes.
    bool applyingEdit_ = false;
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
    // These servers stopped and forgotten, so the next file of theirs
    // starts them again (their notes and checks too).
    void stopServers(const std::vector<std::string>& keys);
    bool languageOf(const std::string& path, Lsp::Language& lang) const;
    // The command lines to try for a key: the settings' own, else the
    // defaults; a second server's the one it was wanted with.
    std::vector<std::string> commandsForKey(const std::string& key) const;

    // ---- the second server, and which of a file's servers answers
    // (LspSessionAlso.cpp)
    std::unique_ptr<AlsoState> also_;
    // The second server a language wants for `path`, as the command to run,
    // remembered for its key, or "": lsp.<server>.also from the settings,
    // else Ruff's own server in a project that configures Ruff, and only
    // when ruff is installed (Lsp::defaultAlsoCommand).
    std::string alsoCommandFor(const std::string& server, const std::string& path);
    // A running second server takes a tab's file: always when the settings
    // name it, else when the project around the file configures Ruff.
    bool alsoWantsPath(const std::string& path, const std::string& key);
    bool projectConfiguresRuff(const std::string& path);
    void forgetRuffConfiguration();
    // The file on show's servers, the main one first; the one that answers
    // a one-server request (Lsp::routeRequest: the main one, else another
    // that offers it); and those asked for code actions.
    std::vector<std::shared_ptr<lspgtk::Server>> fileServers() const;
    std::shared_ptr<lspgtk::Server> serverFor(Lsp::Feature f) const;
    std::vector<std::shared_ptr<lspgtk::Server>> serversFor(Lsp::Feature f) const;
    // The status bar's line for the file's servers (Lsp::statusLine).
    std::string serversStatus(int errors, int warnings) const;

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
    // How many edits a tab's document has had while a server has it open.
    uint64_t documentEdits(uint64_t docId) const;
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
    // What one server said about a file (it closed there), what one server
    // said about any file (it went), and everything.
    void forgetDiagnostics(const std::string& uri, const std::string& serverKey);
    void forgetServerDiagnostics(const std::string& serverKey);
    void forgetAllDiagnostics();
    // The squiggles drawn on the file on show, by severity.
    void countDrawn(int& errors, int& warnings) const;
    bool sameFile(const std::string& uriA, const std::string& uriB) const;
    std::string diagnosticAt(const GtkTextIter* it) const;
    // Every server's diagnostics for a file, and one server's.
    std::vector<Lsp::Diagnostic> diagnosticsOf(const std::string& uri) const;
    std::vector<Lsp::Diagnostic> diagnosticsOf(const std::string& uri,
                                               const std::string& serverKey) const;
    const Lsp::DiagnosticStore& diagnosticStore() const;
    // The squiggles drawn now, hints left out, as character offsets, and
    // their generation (it moves whenever they are drawn again).
    std::vector<std::pair<size_t, size_t>> problemMarks(uint64_t* generation) const;

    // ---- the Problems list (LspSessionProblems.cpp)
    std::unique_ptr<ProblemsState> probs_;
    void problemsChanged();
    std::vector<Lsp::Problem> collectProblems();
    void goToProblem(bool forward);

    // ---- completion (LspSessionCompletion.cpp)
    std::unique_ptr<CompletionState> cmp_;
    // The keys while the list shows: Up, Down, Tab and Return taking the
    // item, Shift+Tab and Shift+Return closing it for the editor, Escape.
    bool completionKey(guint keyval, GdkModifierType mods);
    void completionAfterEdit(const char* inserted, int len);
    void completionFollowsCaret();
    void requestCompletion(const std::string& trigger);
    void requestCompletionSoon(const std::string& trigger);
    void completionArrived(const Json& result, int gen, const std::string& uri);
    std::string typedPrefix() const;
    void refilter();
    void scheduleRefilter();
    void showList(int select, const GtkTextIter* at);
    void closeCompletion();
    void cancelCompletionTrigger();
    void acceptCompletion(int row);
    // A snippet's choice stop (SnippetSession.h): its options listed, and
    // one taken at the stop and its mirrors.
    void showSnippetChoices(const std::vector<std::string>& choices, size_t start, size_t end);
    void acceptChoice(int row);
    // The edits of an accept as one undo step of their own (one Ctrl+Z),
    // never joined to the typing before them; false when refused.
    bool applyAcceptEdits(const std::vector<Lsp::PointEdit>& edits);
    // An item whose additional edits come only from completionItem/resolve:
    // they follow when the answer comes, in the accept's undo step when
    // nothing changed meanwhile, else as a step of their own.
    void resolveAfterAccepting(const Lsp::CompletionItem& item,
                               const std::shared_ptr<lspgtk::Server>& server, size_t start,
                               size_t removed, const std::string& removedText,
                               const std::string& insert, size_t before);
    void pendingAcceptResolved(int key, const Lsp::CompletionItem& item);
    void pendingAcceptTimedOut(int key);
    void dropPendingAccepts();
    void buildPopover();
    void moveSelection(int delta);
    int selectedRow() const;
    void forgetCompletionAnchor();
    void dropCompletionPopover();
    static void onRowActivated(GtkListBox* box, GtkListBoxRow* row, gpointer self);

    // ---- the signature hint (LspSessionSignature.cpp)
    std::unique_ptr<SignatureState> sig_;
    void signatureAfterEdit(const char* inserted, int len);
    void signatureFollowsCaret();
    void requestSignature(const std::string& trigger);
    void signatureArrived(const Json& result, int gen, const std::string& uri, int cursor);
    void scheduleSignatureUpdate();
    void closeSignature();
    bool signatureVisible() const;
    void dropSignaturePopover();

    // ---- hover and definition (LspSessionNavigation.cpp)
    std::unique_ptr<NavState> nav_;
    void forgetHover();
    void closeHover();
    void dropHoverPopover();
    void definitionArrived(const Json& result, int gen, const std::string& uri);
    void hoverArrived(const Json& result, int gen, const std::string& uri, long key,
                      bool popup);
    void showHoverPopover(const std::string& text, bool autohide = true);
    // F8's message in the box at the caret, without the keyboard; the
    // caret moving away closes it (hoverFollowsCaret).
    void showProblemNote(const std::string& text);
    void hoverFollowsCaret();
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
    // The file's server that answers `what` (serverFor), when it is ready
    // and offers it; else null, the bell and a note, `missing` saying what
    // it cannot do.
    std::shared_ptr<lspgtk::Server> serverOffers(Lsp::Feature what, const char* missing);
    // Every one of the file's servers offering code actions asked for those
    // at `range` (each with its own diagnostics there), and `done` given the
    // merged list (Lsp::mergeCodeActions, each action's origin an index
    // into the servers) once all have answered, or after 3 s with what has;
    // never when the file, the caret or the text has moved on meanwhile.
    using CodeActionsDone =
        std::function<void(std::vector<Lsp::CodeAction> actions,
                           std::vector<std::shared_ptr<lspgtk::Server>> servers,
                           const std::string& error)>;
    void askCodeActions(Lsp::Range range, const std::vector<std::string>& only,
                        CodeActionsDone done);
    void rememberAsked();
    bool stillAsAsked(int gen, int wanted, const std::string& uri) const;
    std::pair<int, int> nameRangeAt(int offset) const;
    void openRename(int a, int b, int gen, const std::string& placeholder);
    void commitRename(const std::string& name, int gen);
    std::string editorTextOf(const std::string& path) const;
    void referencesArrived(const Json& result, const Json& error, int gen, const std::string& uri);
    // `then` runs once the action's edit went in, or its command was run
    // (never after a failure).
    void runCodeAction(const Lsp::CodeAction& action, std::shared_ptr<lspgtk::Server> server,
                       std::function<void()> then = nullptr);
    void performCodeAction(const Lsp::CodeAction& action, std::shared_ptr<lspgtk::Server> server,
                           std::function<void()> then);
    void serverAppliesEdit(const std::weak_ptr<lspgtk::Server>& server, const Json& params,
                           Lsp::Client::ApplyEditReply reply);

    // ---- the folder's own tools (LspSessionFolderTools.cpp)
    std::unique_ptr<FolderToolsState> tools_;
    // The folder's answer heard from now on (made), and no longer (destroyed).
    void watchFolderTools();
    void unwatchFolderTools();
    // Where a server is looked for before the usual folders: for Python,
    // the interpreter's own bin.
    std::vector<std::string> firstDirsForKey(const std::string& key) const;
    // Whether the server `program` for `key` may run the folder's code as
    // things stand (Lsp::checkProjectCode), kept for the bar's question
    // until the server stops.
    Lsp::ProjectCodeCheck projectCodeForKey(const std::string& key, const std::string& program);
    Lsp::ProjectCodeCheck checkProjectCodeFor(const std::string& key,
                                              const std::string& program) const;
    void forgetProjectCode(const std::string& key);
    void forgetAllProjectCode();
    void configureServer(Lsp::Client& c, const std::string& key,
                         const std::vector<std::string>& argv, const Lsp::ProjectCodeCheck& pc);
    void folderToolsChanged(const std::string& key);

    // ---- Format Document through the server (LspSessionFormatting.cpp)
    // The file on show's server that formats it (Lsp::routeFormatting over
    // its servers, the main one first), and whether it formats a range.
    std::shared_ptr<lspgtk::Server> formattingServer(bool range, bool* useRange) const;

    // ---- watched files (LspSessionWatch.cpp)
    std::unique_ptr<WatchState> watch_;
    // The folders watched and the servers' watchers, after a server
    // registered or dropped some, or started or stopped (in an idle).
    void scheduleWatcherUpdate();
    void updateFileWatcher();
    std::vector<std::shared_ptr<lspgtk::Server>> watchingServers() const;
    std::vector<Lsp::WatchTarget> watchTargets() const;
    void filesChangedOnDisk(const std::vector<Lsp::FileChange>& changes,
                            const std::vector<Lsp::WatchedBatch>& batches);
    // Every server stopping: no watcher remade for each, then none.
    void stopWatchingFiles();
};

// Stop every server the app started, now: shutdown and exit, then SIGTERM and
// SIGKILL for any still running a moment later. For the application's
// "shutdown" signal; it waits (up to about a second) without the main loop.
void LspTerminateAllServers();
// The folders programs are looked for in after PATH (a launcher from the
// dock may have a short PATH), and a program by name in PATH and those, or
// a path as given (~ expanded) when it can run; "" when none. Format
// Document looks for formatters the same way.
std::vector<std::string> LspSearchDirs();
std::string LspFindProgram(const std::string& name);
// How many server processes are running (tests).
std::size_t LspRunningServerCount();
// Their process ids (tests).
std::vector<int> LspServerPids();
