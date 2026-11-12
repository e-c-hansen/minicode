// Editor.h — a window's editor: the row of tabs (TabStrip.h) over the slot,
// a stack holding the one GtkTextView and what takes its place (pictures,
// PDFs, video and audio, the LaTeX preview, a diff). Every open file is a
// Document (Document.h) with a buffer of its own; the view shows the active
// one with gtk_text_view_set_buffer (linux/PARITY.md, D2 and D3). The tabs
// follow the core's rules (../src/EditorTabs.h) and are kept in Tabs.cpp, the
// counterpart of the Mac's EditorController+Tabs.mm; this file and
// Editor.cpp are the view: everything below "The document on show" acts on
// the active document, as the Mac's EditorController reads and writes its
// active tab.
#pragma once

#include <gtk/gtk.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Document.h"
#include "EditorPage.h"
#include "EditorTabs.h"
#include "Markdown.h"
#include "MarkdownEdit.h"
#include "Settings.h"

class AgentNotes;
class InlineAgentBar;
class AutoCloseInput;
class BracketView;
class Carets;
class CodeView;
class FoldUi;
class MediaView;
class CsvTable;
class JsonOutlineView;
class MarkdownSide;
class SnippetSession;
class LatexPreview;
class GitDiffView;
class CompareView;
class LargeFileUi;
class TabStrip;

// Told what the view shows and how it changes, for the language server
// session (Lsp.h). A few calls, so the editor knows nothing about LSP.
class EditorObserver {
public:
    virtual ~EditorObserver() = default;
    // The view now shows another document: `path` when it is that file's
    // editable source, "" for a message, a picture, a diff, a Markdown page
    // or no tab. Editor::sourceBuffer() is the buffer by then.
    virtual void documentChanged(const std::string& path) = 0;
    // A tab's file was written, on show or not (Save All, auto-save).
    virtual void documentSaved(Document* d) = 0;
    // A tab opened, closed, was renamed, read, or changed its unsaved state
    // (from the strip's refresh): every tab's file stays open on its
    // language server.
    virtual void tabsChanged() {}
    // The source on show was edited; the buffer has already changed.
    // `inserted` is the text of an insertion (len bytes), or null for a
    // deletion.
    virtual void textEdited(const char* inserted, int len) = 0;
    // The observer has a use for the Return key just pressed (a completion
    // list is open), so the editor must not continue a Markdown list with it.
    virtual bool takesReturn() const { return false; }
    // A key for a list showing by the caret (the completion list, the
    // references or fixes, the signature hint), asked before anything else
    // in the view has it (a snippet's stops, several carets, the
    // indentation): true when the list took it.
    virtual bool listKey(guint keyval, GdkModifierType mods) {
        (void)keyval;
        (void)mods;
        return false;
    }
};

class Editor : public DocumentHost {
public:
    Editor();
    // Runs while the window's widgets still exist (main.cpp deletes the
    // editor from GtkApplication's window-removed).
    ~Editor() override;

    // The widget to drop into the layout: the tabs over the slot.
    GtkWidget* widget() const { return box_; }
    // A widget over the slot (under the tabs), placed by its own alignment
    // and margins: the find bar (FindBar.h).
    void addOverlay(GtkWidget* w) { gtk_overlay_add_overlay(GTK_OVERLAY(slotOverlay_), w); }
    // A bar across the slot's top, under the tabs, as the Mac's install bar
    // (ConsentBar.h).
    void addBar(GtkWidget* w);

    // The GtkTextView, and the buffer it shows: the active document's source,
    // its Markdown page, a message, or the empty editor with no tab open.
    GtkWidget*     textView() const { return view_; }
    GtkTextBuffer* buffer() const { return buffer_; }
    // The view's own drawing passes, the ruler and the line-number gutter
    // (CodeView.h).
    CodeView*      codeView() const { return code_; }
    // The bracket boxes, Go to Bracket and the bracket colors' painting
    // (BracketView.h), and auto-closing pairs (AutoCloseInput.h).
    BracketView*    brackets() const { return brackets_; }
    AutoCloseInput* autoClose() const { return autoClose_; }
    // Several carets and the line commands (Carets.h).
    Carets*         carets() const { return carets_; }
    // A snippet completion's tab stops (SnippetSession.h).
    SnippetSession* snippets() const { return snippets_; }
    // Folding's placeholders, keys and commands (Folds.h).
    FoldUi*         folds() const { return folds_; }
    // The active document's own buffer (its source, while its page shows
    // too), else what the view shows.
    GtkTextBuffer* sourceBuffer() const;

    // ------------------------------------------------------------ tabs
    // (Tabs.cpp; the rules are EditorTabs.h's, as on the Mac.)

    // Show a file in its tab, else in a new one: the preview tab when `from`
    // opens in preview (replacing the old preview tab, which has no unsaved
    // changes, since an edit keeps it), else a kept tab just right of the
    // active one. The tab that was on show keeps its text, unsaved changes,
    // undo and place. False when the file could not be shown as it is (its
    // tab then says why: missing, not text, too large).
    bool openFile(const std::string& path,
                  EditorTabs::OpenFrom from = EditorTabs::OpenFrom::Other);
    int tabCount() const { return (int)tabs_.size(); }
    Document* document(int index) const;
    const std::vector<Document*>& documents() const { return tabs_; }   // in tab order
    Document* active() const { return active_; }
    int activeIndex() const;
    int indexOf(const Document* d) const;
    Document* documentFor(const std::string& path) const;   // its tab, or null
    Document* documentById(uint64_t id) const;               // by Document::id, or null
    // Put a tab on show (the keyboard stays where it is).
    void activate(Document* d);
    // A tab by index, by step (wrapping) or by number (1 to 8, 9 the last),
    // with the keyboard given to it; the bell when there is none.
    void selectTab(int index);
    void stepTab(int delta);
    void selectTabNumber(int n);
    // The keyboard to what the active tab shows.
    void focusActiveDocument();
    // Close a tab without asking; the caller asks about unsaved changes.
    // The tab shown next is the one used most recently.
    void closeTab(Document* d);
    // Every tab, without asking and without remembering them (Open Folder).
    void closeAllTabs();
    // The tab closed last in this window, where it was, with its preview,
    // selection and scroll; files no longer there are passed over. A file
    // open again meanwhile is only shown. False when none is left.
    bool reopenClosedTab();
    bool canReopenClosedTab() const { return !closed_.empty(); }
    size_t closedTabCount() const { return closed_.size(); }
    // A tab for `path` at the end of the row, not put on show, its file not
    // read until it first is (a session coming back, Persistence.h).
    Document* addTab(const std::string& path);
    // A file for the agent (`minicode open --note`, AgentNotes.h): in the
    // agent's preview tab when `preview` (each such open replaces the last
    // one; an edit keeps it), else a kept tab, just right of the active
    // one. With `quiet` it is only added beside the user's tab, to be read
    // when shown; else shown. A file already in a tab uses that tab. Null
    // when it could not be opened.
    Document* openForAgent(const std::string& path, bool preview, bool quiet);
    Document* agentPreviewTab() const;
    // The window's notes, hanging over the text (AgentNotes.h).
    AgentNotes* notes() const { return notes_; }
    // The inline agent's bar over the text (InlineAgentBar.h).
    InlineAgentBar* inlineAgent() const { return inline_; }
    void removeOverlay(GtkWidget* w) { gtk_overlay_remove_overlay(GTK_OVERLAY(slotOverlay_), w); }
    // Where a tab's view was, to keep (a session, Reopen Closed Tab): its
    // selection, its first character on show and whether it showed its
    // page, the tab on show's taken from the view as it is now. A tab not
    // read yet gives what it came back with.
    EditorTabs::ClosedTab placeOf(Document* d);
    // The other way: the selection and view a tab is given back (a tab not
    // read yet gets them once it is).
    void placeTab(Document* d, const EditorTabs::ClosedTab& c);
    // An ordinary tab now, no longer the preview tab.
    void keepPreviewTab(Document* d);
    Document* previewTab() const;
    std::vector<Document*> dirtyDocuments() const;
    bool anyDirty() const { return !dirtyDocuments().empty(); }
    // A file or folder moved (a rename, a drag in the tree): every tab of it,
    // or under it, follows, unsaved changes included.
    void pathMoved(const std::string& from, const std::string& to);
    // It went to the Trash: tabs of it, or under it, close, unless they hold
    // unsaved changes, which stay, so saving writes the file again (the
    // Mac's rule).
    void pathTrashed(const std::string& path);
    TabStrip* strip() const { return strip_; }
    // A tab's close button or a middle click on it: the caller asks about
    // unsaved changes, then calls closeTab.
    std::function<void(Document*)> onCloseRequest;
    // A command from a tab's right click menu: keep, close, closeothers,
    // closeright, closeall, copypath, reveal, revealtree.
    std::function<void(const std::string& command, Document*)> onTabCommand;
    // Another tab came on show (the file tree follows it), and just before
    // the one on show leaves (Go Back's history, NavHistoryUi.h).
    std::function<void()> onActiveChanged;
    std::function<void()> onActiveWillChange;
    // A document's text was edited (`d`), or the tabs changed: one opened,
    // closed, renamed or put on show, or its unsaved state changed (null).
    // main.cpp keeps the session, the backups and auto-save from it.
    std::function<void(Document* d)> onDocumentsChanged;
    // Every object of the editor's that handlers on its widgets name, for
    // main.cpp's teardown (disconnectOwners): never null.
    std::vector<void*> handlerOwners() const;

    // ------------------------------------------------ the document on show

    // Write the active document (or `d`) to its file. False, with the reason
    // in *error, when the write failed; it then stays unsaved. Nothing to
    // save (no tab, a picture, a message, a diff) writes nothing and is true.
    bool save(std::string* error = nullptr);
    bool saveDocument(Document* d, std::string* error);
    bool canSave() const { return active_ && active_->canSave(); }

    // Changes made to the open files by something else (the Mac's
    // checkExternalChange). Each file is watched while its tab is open, and
    // the shell also calls this when the window becomes active. A clean tab
    // takes the new text in place, keeping the caret and the scroll; a tab
    // with unsaved edits has the callback put the question to the user, but
    // only while it is on show (a tab in the background is asked about when
    // it is shown). The callback returns false if it cannot ask now (another
    // question is up); the change is then reported again on the next check.
    using ExternalCb = bool(*)(void* user);
    void setExternalChangeCallback(ExternalCb cb, void* user) { extCb_ = cb; extUser_ = user; }
    // The tab the callback is asking about, until the shell says the user
    // answered. The question does not stop the main loop, and the tab already
    // counts the version asked about as its file's, so auto-save must leave
    // it alone meanwhile: its after-delay timer wrote the tab's edits over
    // the very change asked about, and Reload then read them back.
    bool askingAboutDisk(const Document* d) const { return d && askingAbout_ == d->id(); }
    void diskQuestionAnswered() { askingAbout_ = 0; }
    void checkExternalChange();
    // The user chose Reload: the disk's text replaces the active tab's and
    // its edits, in place. False if the file can no longer be read as text.
    bool reloadFromDisk();

    // Images, PDFs, video and audio (MediaView). isMedia() is true while one
    // is shown, and titleSuffix() is what the window title adds for it
    // ("  640 × 480", "  12 pages", "  640 × 360, 0:05"), empty otherwise.
    // canPlayMedia() is true while a video or audio file that can play is
    // shown, and togglePlayMedia() plays or pauses it. The media view calls
    // the title callback when a stream learns its size or fails, so the
    // shell can enable Play/Pause from canPlayMedia().
    bool isMedia() const;
    bool canPlayMedia() const;
    void togglePlayMedia();
    std::string titleSuffix() const;
    MediaView* media() const { return media_; }
    // Zoom for the PDF on screen, the PDF viewer's or the LaTeX preview's:
    // +1 in, -1 out, 0 back to fit width. False, doing nothing, when neither
    // is on screen. A PDF view coming on or off screen calls the title
    // callback, so the shell can enable the zoom keys from showingPdf().
    bool showingPdf() const;
    bool zoomPdf(int step);

    // Toggle between the source and the rendered Markdown page (a buffer of
    // the document's own, D3), or for LaTeX between the source and the
    // typeset pages. No-op for other files.
    void togglePreview();
    // The Markdown file's page right of its source (MarkdownSide.h), per
    // tab; null until the first one is shown.
    void toggleSidePreview();
    MarkdownSide* sidePane() const { return side_; }
    bool showsSide() const { return sideDoc_ != nullptr; }
    // A CSV or TSV tab shown as its table (CsvTable.h) rather than the text.
    // The table finishes a cell being typed before the tab goes.
    bool showsTable() const { return csvDoc_ != nullptr; }
    // A JSON file's outline beside the text (JsonOutlineView.h); null until
    // the first one is shown, and then hidden when the tab has none.
    JsonOutlineView* jsonOutline() const { return outline_; }
    bool showsOutline() const { return outlineDoc_ != nullptr; }
    CsvTable* csvTable() const { return csv_; }
    bool isMarkdown() const { return active_ && active_->isMarkdown(); }
    bool inPreview() const { return active_ && active_->showsPreview(); }
    // The math line over the preview ("Typesetting 2 formulas…"), "" when hidden, and
    // the text color formulas are drawn in ("#rrggbb").
    std::string mathNote() const;
    std::string mathColor() const;
    // A .tex/.ltx/.latex file, in a build with poppler: it opens in the LaTeX
    // preview (Latex.h), which takes the slot while the buffer keeps the
    // source. latex() is the active tab's, null until it first shows.
    bool isLatex() const { return active_ && active_->isLatex(); }
    LatexPreview* latex() const { return active_ ? active_->latexPreview() : nullptr; }
    // Ctrl+Shift+S: save the typeset PDF somewhere of the user's choosing.
    // False when the open file is not LaTeX.
    bool exportPdf(GtkWindow* parent);
    // File > Export as HTML…: the active Markdown file (unsaved changes
    // included) as a standalone page, chosen in a save dialog, or at
    // $MINICODE_HTML_EXPORT_PATH when that is set (the checks).
    bool exportHtml(GtkWindow* parent);
    // The page written to `path` through the atomic writer; false with `error`.
    bool exportHtmlTo(const std::string& path, std::string* error);
    // Ctrl+Alt+Shift+J: show the caret's line in the PDF (forward search). From
    // an \\input file the main document is the `% !TEX root` comment's, else
    // the open tab whose typeset read this file; its tab is shown. False when
    // there is nothing to search from.
    bool showLineInPdf();

    // A file's diff or a commit from the Source Control panel, read-only in
    // the editor's slot. Diffs and commits share one tab, as on the Mac.
    // `title` is what the tab and the window title show: "name (diff)" or
    // "<short hash> <subject>".
    void showDiff(const std::string& title, const std::string& bytes, bool commit);
    bool isDiff() const { return active_ && active_->kind() == Document::Kind::Diff; }
    const std::string& diffTitle() const;
    GitDiffView* diffView() const { return diff_; }
    // A comparison of two texts in a tab of its own (CompareView.h): lined up
    // and colored on a thread, the tab opening when that is done (`done`, if
    // given, is called then with the tab's document). `minicode diff`, Compare
    // with Main and the IDE link's openDiff come in later items.
    void showComparison(const CompareSpec& spec, std::function<void(Document*)> done = nullptr);
    // A page (EditorPage.h) in a tab of its own, reused by its key.
    Document* showPage(std::shared_ptr<EditorPage> page);
    EditorPage* currentPage() const;   // the active tab's page, or null
    CompareView* compareView() const { return compare_; }
    bool isComparison() const { return active_ && active_->compare(); }
    // The diff view was made (on the first diff): the window's Source
    // Control panel gives it the hunk actions (GitPanel::attachDiffView).
    std::function<void(GitDiffView*)> onDiffViewMade;

    // The active tab's file ("" with none, or a diff), and whether it holds
    // unsaved changes.
    const std::string& currentPath() const;
    bool dirty() const { return active_ && active_->dirty(); }

    // Optional: called whenever what the title shows changes (the tab on
    // show, its unsaved changes, its tasks). Set by main.cpp.
    using TitleCb = void(*)(void* user);
    void setTitleCallback(TitleCb cb, void* user) { titleCb_ = cb; titleUser_ = user; }

    // Colors from the settings file: syntax and Markdown tags, which every
    // tab shares, and the swatches shown while the settings file itself is
    // open. The panel background and text come from the stylesheet.
    void applySettings(const Settings& s);
    // The settings file's path. While that file is open its color values are
    // clickable swatches that open a color picker, and its opacity values
    // open a slider.
    void setSettingsPath(const std::string& path) { settingsPath_ = path; }
    // The color picker's or the opacity slider's popover while one is up
    // (for the checks).
    GtkWidget* pickerPopover() const { return colorPop_; }

    // Ctrl+/ : comment or uncomment the lines the selection touches. Returns
    // false when there is nothing to do it to (preview, message, a file type
    // with no line comments).
    bool toggleComment();

    // The task key in a Markdown file's source: MarkdownTasks::toggle over
    // the lines the selection touches, as one undo step. False when there is
    // nothing to do it to (not Markdown, the preview, a heading).
    bool toggleTask();

    // F9: the selected lines sorted, or with no selection the keep-sorted
    // block around the caret (KeepSorted::sortCommand). Shift+F9: every
    // keep-sorted block in the file. One undo step each. `message` gets what
    // to tell the user ("Already sorted", or why nothing was done). False
    // when nothing could be done (preview, message, an unmatched marker).
    bool sortLines(std::string* message);
    bool fixKeepSorted(std::string* message);
    // editor.keep-sorted-on-save: every block sorted before a save, as one
    // undo step. Returns a problem to show, or "".
    std::string sortBlocksBeforeSave();
    // The active Markdown file's tasks, done and in all; 0 and 0 otherwise.
    int tasksDone() const { return active_ ? active_->tasksDone() : 0; }
    int tasksTotal() const { return active_ ? active_->tasksTotal() : 0; }

    // Put the caret on a 1-based line and select `byteLength` bytes starting
    // `byteColumn` bytes into it (a Find in Folder match), scrolled into view
    // and focused. A Markdown preview switches to the source first. A column
    // that no longer fits the line (the file changed since the search) selects
    // nothing and leaves the caret at the line's start. Returns false when no
    // editable text is showing.
    bool revealLine(int line, std::size_t byteColumn, std::size_t byteLength);
    // The same for a compiler's line:column (a terminal link): the caret goes
    // `column` characters into the 1-based line, 0 or 1 meaning its start,
    // and no further than its end. The Mac counts the column the same way.
    bool revealLineColumn(int line, int column);
    // The scroll stays where it is now: a tab's place still coming back, a
    // revealed line still being kept in view, or a Markdown page's top line
    // held while its pictures load stop following. For the palette, whose
    // symbol previews scroll the view themselves and put it back.
    void stopSettling();

    // A link clicked in a Markdown preview that the editor does not handle
    // itself ("#section" it scrolls to): a web address, or a file or folder,
    // resolved against the Markdown file's folder. `line` is from GitHub's
    // "#L12", else 0. The shell opens it as it opens terminal links.
    struct Link {
        bool isUrl = false;
        std::string target;
        bool isDir = false;
        int line = 0;
    };
    void setLinkHandler(std::function<void(const Link&)> h) { linkHandler_ = std::move(h); }

    // Big files (LargeFileView.h): the status bar's "Large file: features off"
    // (main.cpp puts the widget in the status bar), and its click, which
    // turns the features on for the tab on show.
    GtkWidget* largeFileNote() const;
    LargeFileUi* largeFileUi() const { return large_; }
    void turnOnLargeFileFeatures();

    // Word wrap turned over (Alt+Z) or set: the view follows (Zoom.h).
    void applyWordWrap() { setProseFont(prose_); }

    // One observer (the language server session), not owned.
    void setObserver(EditorObserver* o) { observer_ = o; }
    // True while the view shows the active file's editable source.
    bool isSource() const;
    // An input method is composing text in the view, not yet in the buffer.
    bool composing() const { return composing_; }
    // The active file's whole source as UTF-8.
    std::string text() const;

    // DocumentHost
    void documentStateChanged(Document* d) override;
    void documentEdited(Document* d, GtkTextIter* at, const char* inserted, int len) override;
    bool documentChangedOnDisk(Document* d) override;
    const Settings& documentSettings() const override { return settings_; }
    const std::string& settingsPath() const override { return settingsPath_; }
    void documentWillReload(Document* d) override;
    void documentReloaded(Document* d) override;
    void documentLoaded(Document* d, int result, bool blank) override;   // LargeFileView.cpp

private:
    // ---- tabs (Tabs.cpp)
    bool loadInto(Document* d, const std::string& path);   // reads the file
    bool loadText(Document* d, const std::string& path);
    // The file as read (`result`, a Document::Read): in the document, or a
    // message saying why not.
    bool textRead(Document* d, const std::string& path, int result, const std::string& content);
    bool opensInPreview(EditorTabs::OpenFrom from) const;
    void dropTab(Document* d, bool remember);   // out of the row, and deleted
    void destroy(Document* d);
    void refreshStrip();
    GMenuModel* menuForTab(int index);
    void tabMenuCommand(const std::string& name, GVariant* param);

    // ---- the view
    void stash();                 // the active tab leaves the screen: its place kept
    void capturePlace(Document* d);   // where the view is, into the tab on show's place
    EditorTabs::ClosedTab closedTabOf(Document* d) const;   // from its place as kept
    void showActive();            // the active tab (or the empty editor) on screen
    void setViewBuffer(GtkTextBuffer* b, bool prose, bool editable);
    void showSource(Document* d);
    void restorePlace(Document* d);
    void notifyDocument();        // tell the observer what the view shows now
    void titleChanged();
    bool inMarkdownPage() const;  // the view shows the active Markdown page
    void updateMathNote();
    void downloadTectonicForMath();

    void renderPreview();        // build the active Markdown page into its buffer
    void rerenderPreview();      // the same again, at the same scroll
    void fitPage();              // size the preview's tables and pictures to the pane
    void clearPage(Document* d); // its widgets off the view first, then its page forgotten
    // Web pictures (WebImages.h): the page's pending ones are fetched after
    // a render; those that arrive are put in together, shortly after the
    // first of them, in place of their alt text.
    void fetchWebPictures();
    void placeArrivedPictures();
    // Where the preview comes to rest (place().previewEntryV), measured 700 ms on.
    void notePreviewRest();
    // Keeping the line at the top of the view in place while pictures placed
    // above it are laid out (see holdTopLine in Editor.cpp).
    void holdTopLine(GtkTextMark* mark, int into);
    void stopHold();
    static gboolean onHoldTick(GtkWidget* w, GdkFrameClock* clock, gpointer self);
    // A tab shown again, back at its place: `mark`'s line at the top, `into`
    // pixels into it (see settleOnPlace in Editor.cpp). The mark is the
    // editor's to delete.
    void settleOnPlace(GtkTextMark* mark, int into);
    void stopPlace();
    static gboolean onPlaceTick(GtkWidget* w, GdkFrameClock* clock, gpointer self);
    void openMarkdownLink(const std::string& url);
    bool scrollToAnchor(const std::string& anchor);
    // Editing from the preview: the block on source `line` (a cell, when
    // `column` is not -1) in a popover pointing at `at` (view coordinates).
    void editMarkdownBlock(int line, int column, const GdkRectangle& at);
    void showMdPopover(const std::string& title, const std::string& text, bool allowsItem);
    void commitMdEdit();
    void dropMdPopover();
    // A new source from a preview edit: spliced into the source buffer as one
    // undoable edit (D3), then the page shown again at the same scroll.
    void applyMarkdownSource(const std::string& source);
    // Ctrl+Z, Ctrl+Shift+Z in the Markdown page: the source buffer's undo.
    void undoFromPage(bool redo);
    // A click on a task's box in the preview: that one byte of the source
    // flipped (MarkdownTasks::toggleBox), as a preview edit.
    void toggleTaskBox(int line);
    // Return in a Markdown list item (MarkdownTasks::newline): true when it
    // made the new item, or ended the list, itself.
    bool continueList();
    // One replacement from MarkdownTasks in the UTF-16 `text` the buffer
    // held, as one user action, and the selection it gives. `text` starts
    // `base` characters into the buffer (a slice, see sourceText).
    void applyTaskEdit(const std::u16string& text, std::size_t replaceStart,
                       std::size_t replaceLength, const std::u16string& replacement,
                       std::size_t selStart, std::size_t selEnd, long base = 0);
    // The editable source as UTF-16 and the selection in it; false when the
    // view does not show editable source. Given `base`, a big buffer gives
    // only the lines around the selection, starting `*base` characters in
    // (LargeFileView::indentSlice).
    bool sourceText(std::u16string& text, std::size_t& a, std::size_t& b,
                    long* base = nullptr) const;
    // Return, Tab, Shift+Tab or Backspace by AutoIndent's rules: true when
    // it acted (or, for Shift+Tab, when the key must not move the focus).
    bool indentKey(guint key);
    // A closer or ':' just typed, before `loc`: the line moved back where
    // AutoIndent::typed says, inside the same user action; loc stays valid.
    void electric(GtkTextIter* loc);
    bool runKeepSorted(bool all, std::string* message);
    static gboolean onViewKey(GtkEventControllerKey* c, guint key, guint code,
                              GdkModifierType mods, gpointer self);
    void ensureTags();           // the per-style and Markdown tags, in the shared table
    void setProseFont(bool prose);  // proportional for preview/messages, mono for code

    bool isSettingsFile() const;
    bool swatchAt(double x, double y, int* line) const;   // widget coords
    void pickColor(int line);
    void setLineColor(int line, const Rgba& c);
    void dropColorPopover();
    void scheduleColorSave();
    void saveColorNow();
    static void onColorPicked(GObject* chooser, GParamSpec* pspec, gpointer self);
    // The settings file's opacity values: a click opens a slider in the
    // color picker's place (OpacityPicker.cpp, PARITY.md 2.11).
    bool opacityAt(double x, double y, int* line) const;   // widget coords
    void pickOpacity(int line);
    void setLineOpacity(int line, double value);
    static void onOpacityChanged(GtkRange* range, gpointer self);
    static void onViewUnrealize(GtkWidget* view, gpointer self);
    static void onPressed(GtkGestureClick* g, int n, double x, double y, gpointer self);
    static void onMotion(GtkEventControllerMotion* m, double x, double y, gpointer self);

    void showLatex(bool on, bool focus);   // the LaTeX preview in the slot, or the source
    LatexPreview* makeLatex(Document* d);  // its own, made on first use
    static void onMediaChanged(void* self);   // reloaded from disk
    static void onPdfShown(void* self);       // a PDF view came on or off screen

    GtkWidget*     box_      = nullptr;   // the strip over the slot
    TabStrip*      strip_    = nullptr;
    GtkWidget*     slot_     = nullptr;   // GtkStack: scroller_, media, LaTeX previews, diff
    AgentNotes*    notes_ = nullptr;
    InlineAgentBar* inline_ = nullptr;
    friend class InlineAgentBar;
    GtkWidget*     slotOverlay_ = nullptr;   // the slot, with the find bar over it
    GitDiffView*   diff_     = nullptr;   // made on the first diff
    CompareView*   compare_  = nullptr;   // made on the first comparison
    std::shared_ptr<bool> compareAlive_ = std::make_shared<bool>(true);   // for the prepare thread
    MediaView*     media_    = nullptr;
    CsvTable*      csv_      = nullptr;   // made on the first table
    Document*      csvDoc_   = nullptr;   // the tab it shows
    gulong         csvChanged_ = 0;       // csvDoc_'s buffer "changed"
    bool           csvApplying_ = false;  // the table's own edit is going in
    void showTable(Document* d);
    JsonOutlineView* outline_ = nullptr;
    GtkWidget*     outlinePaned_ = nullptr;
    Document*      outlineDoc_ = nullptr;   // the tab it shows
    gulong         outlineChanged_ = 0, outlineCursor_ = 0;
    bool           outlineSelecting_ = false;   // the outline's own choice is going in
    MarkdownSide*  side_ = nullptr;
    Document*      sideDoc_ = nullptr;      // the tab it shows
    gulong         sideChanged_ = 0;
    void syncSide();
    void dropSide();
    void syncOutline();
    void dropOutline();
    void dropTable();
    GtkWidget*     scroller_ = nullptr;
    GtkWidget*     view_     = nullptr;   // GtkTextView: code_'s
    CodeView*      code_     = nullptr;   // owned by the view
    BracketView*    brackets_  = nullptr;
    AutoCloseInput* autoClose_ = nullptr;
    Carets*         carets_    = nullptr;
    SnippetSession* snippets_  = nullptr;
    FoldUi*         folds_ = nullptr;
    GtkTextBuffer* buffer_   = nullptr;   // what the view shows
    GtkTextBuffer* welcome_  = nullptr;   // the view's buffer with no tab open
    char           typedKey_ = 0;         // ':' or a closer being typed (electric)
    bool           prose_ = true;         // the view shows prose, not source (setProseFont)
    bool           composing_ = false;    // an input method's text in the view

    // The documents, in tab order, and the one on show.
    DocumentTags           tags_;
    std::vector<Document*> tabs_;
    Document*              active_ = nullptr;
    Document*              diffDoc_ = nullptr;   // the tab diffs and commits share
    uint64_t               stamp_ = 0;           // for Document::lastUsed
    EditorTabs::ClosedTabs closed_;
    GSimpleActionGroup*    tabActions_ = nullptr;   // "tab.", the tab menu's

    std::string settingsPath_;
    Settings    settings_;    // for tag colors and swatch text contrast
    bool        overSwatch_ = false;

    // The live color picker: a popover on the text view holding a
    // GtkColorChooserWidget. Every pick rewrites the swatch's line at once;
    // the file is saved 150 ms after the last one, as on the Mac.
    GtkWidget*  colorPop_ = nullptr;
    int         colorLine_ = -1;
    Document*   colorDoc_ = nullptr;    // the tab the popover was opened for
    guint       colorSaveTimer_ = 0;

    ExternalCb    extCb_ = nullptr;
    void*         extUser_ = nullptr;
    uint64_t      askingAbout_ = 0;   // a Document's id (from 1), 0 for none
    // The scroll of the tab on show around a change from the disk.
    double        reloadH_ = 0, reloadV_ = 0;
    bool          reloadKeep_ = false;

    // After revealLine: the caret is checked every frame until it has stayed
    // on screen for a few frames, and scrolled to again if a layout that was
    // not ready yet left it off screen (see settleOnCaret).
    guint  settleTick_ = 0;
    gint64 settleUntil_ = 0;       // monotonic µs; give up after this
    int    settleOffset_ = -1;     // the caret's offset; if it moves, the user has
    double settleLastV_ = -1;      // the scroll position at the last frame
    int    settleStill_ = 0;       // frames in a row with the caret seen, not moving
    int    settleTries_ = 0;
    double settleUpper_ = -1;      // the height last seen (see settleOnPlace)
    gint64 settleCap_ = 0;         // never later than this
    bool   settleTop_ = false;     // its line put at the top while GTK lays out
    void settleOnCaret();
    void stopSettle();
    static gboolean onSettleTick(GtkWidget* w, GdkFrameClock* clock, gpointer self);

    std::function<void(const Link&)> linkHandler_;
    // Editing from the preview (editMarkdownBlock): the popover, the block
    // being edited, and the source it was found in; the edit is refused if
    // the source, or the tab, changed meanwhile.
    GtkWidget*  mdPop_ = nullptr;
    GtkWidget*  mdPopLabel_ = nullptr;
    GtkWidget*  mdPopText_ = nullptr;
    GtkWidget*  mdPopAdd_ = nullptr;
    MarkdownEdit::Block mdBlock_;
    std::string mdEditSrc_;
    Document*   mdEditDoc_ = nullptr;
    bool        mdEditValid_ = false;
    bool        mdAdding_ = false;
    GdkRectangle mdAnchor_{0, 0, 1, 1};
    guint  previewEntryTimer_ = 0;

    // Web pictures that arrived since the last placing, and the timer that
    // places them. The fetches' callbacks can outlive the editor, so they
    // hold a weak reference to alive_ and do nothing once it is gone.
    std::vector<std::pair<std::string, WebImages::PicturePtr>> arrived_;
    guint arrivedTimer_ = 0;
    int mermaidListener_ = 0;   // Mermaid.h: a diagram arrived
    int mathListener_ = 0;      // Math.h: a formula typeset or failed
    GtkWidget* mathNote_ = nullptr;   // the math line over the preview
    bool mathDownloading_ = false;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    GtkTextMark* holdMark_ = nullptr;   // the line that was at the top
    int    holdInto_ = 0;               // how far into it the view started
    guint  holdTick_ = 0;
    gint64 holdUntil_ = 0;
    int    holdStill_ = 0;              // frames in a row it needed no move
    int    holdMoves_ = 0;              // moves made so far
    GtkTextMark* placeMark_ = nullptr;  // settleOnPlace's line
    int    placeInto_ = 0;
    guint  placeTick_ = 0;
    gint64 placeUntil_ = 0;
    double placeLastV_ = -1;
    int    placeStill_ = 0;
    int    placeTries_ = 0;
    double placeUpper_ = -1;            // the height last seen: still growing while GTK lays out
    gint64 placeCap_ = 0;               // never later than this
    int    placeCaret_ = -1;            // the caret's offset; if it moves, the user has
    bool   placeFollowCaret_ = false;   // the user moved the caret first: show it

    EditorObserver* observer_ = nullptr;
    LargeFileUi*    large_ = nullptr;   // LargeFileView.h

    TitleCb titleCb_ = nullptr;
    void*   titleUser_ = nullptr;
};
