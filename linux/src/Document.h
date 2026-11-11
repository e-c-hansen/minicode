// Document.h — one open file of a window's editor (linux/PARITY.md, D2 and
// D3): what belongs to the file rather than to the view, so that a tab keeps
// it while another tab is on show. The window's one GtkTextView shows a
// document with gtk_text_view_set_buffer; the editor keeps its documents in
// tab order (Editor.h, Tabs.cpp). The Mac's counterpart is MCTab
// (src/EditorTab.h).
//
// A document holds:
//   - its source in a GtkTextBuffer of its own, over the tag table every
//     document shares, with GTK's own undo, so each tab keeps its history;
//   - the incremental highlighter and its UTF-8 mirror, fed by every edit of
//     the buffer whether it is on show or not;
//   - whether it differs from the disk: the text as last read or saved is
//     kept, and every edit compares with it, lengths first, so an edit taken
//     back (typed and deleted, undone) leaves nothing unsaved, as on the Mac;
//   - a watch on its file: a clean document takes a change made elsewhere in
//     place, on show or not; one with unsaved edits has its host ask;
//   - for Markdown, a buffer of its own for the rendered page (D3), so the
//     preview never touches the source, its undo or its colors, and an edit
//     made from the page is a splice into the source;
//   - for LaTeX, its own preview (Latex.h), so its pages and typeset stay;
//   - where its view was while another tab was on show;
//   - its backup (BackupStore.h, Persistence.h): the id its unsaved text is
//     kept under on disk, removed when the document goes.
//
// Nothing is ever put in the source buffer that is not in the file (D6).
#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "EditBatch.h"
#include "EditorTabs.h"
#include "Markdown.h"
#include "Settings.h"
#include "CompareDecision.h"
#include "ComparePanes.h"
#include "SyntaxHighlighter.h"

#ifdef MINICODE_ENABLE_PDF
#include "PdfView.h"
#endif

class BracketColors;
class SemanticColors;
namespace Lsp {
struct SemanticSpan;
}
class DocumentFolds;
class Document;
class LatexPreview;

// What every document of an editor shares: one tag table, and of its tags
// the ones the highlighter owns (one per token style, and the settings
// file's swatches as they are made). A retag removes only these.
struct DocumentTags {
    GtkTextTagTable* table = nullptr;
    std::vector<GtkTextTag*> highlight;
};

// The editor, as the documents see it.
class DocumentHost {
public:
    virtual ~DocumentHost() = default;
    // Whether it has unsaved changes, its task count or its name changed.
    virtual void documentStateChanged(Document* d) = 0;
    // Its source was edited; the buffer has changed. `inserted` is the text
    // of an insertion of `len` bytes ending at `at`, or null for a deletion.
    virtual void documentEdited(Document* d, GtkTextIter* at, const char* inserted, int len) = 0;
    // Its file changed on disk while it has unsaved edits. True when the
    // user is being asked, and that version on disk is then not asked about
    // again; false to hear of it again at the next check.
    virtual bool documentChangedOnDisk(Document* d) = 0;
    // For the settings file's swatches.
    virtual const Settings& documentSettings() const = 0;
    virtual const std::string& settingsPath() const = 0;
    // Around a change from the disk taken in place, so the editor can keep
    // the scroll position of the document on show.
    virtual void documentWillReload(Document*) {}
    virtual void documentReloaded(Document*) {}
    // A file read in the background (Document::beginLoading) is in its
    // buffer, or could not be read (`result`); `blank` when it holds only
    // white space.
    virtual void documentLoaded(Document*, int /* Document::Read */ result, bool blank) {
        (void)result;
        (void)blank;
    }
};

class Document {
public:
    enum class Kind {
        Text,      // a file's text, editable
        Message,   // a file that cannot be shown as text: why, in the buffer
        Media,     // an image, a PDF, video or audio (the editor's MediaView)
        Diff,      // a diff or a commit from Source Control (GitDiffView)
    };
    // Reading a file as text (readText).
    enum class Read { Ok, Missing, NotRegular, TooLarge, NotText, Failed };

    Document(DocumentHost* host, DocumentTags* tags);
    // Takes every handler off its buffers and its file's watch, and removes
    // its backup: a document goes when its tab closes (asked about first) or
    // its window does. The editor takes its LaTeX preview's widget off the
    // screen first.
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    // A regular file of at most 16 MB, as UTF-8 text. The type and size are
    // checked before it is opened, so a named pipe is never opened (that
    // blocks until a writer comes) and a disk image is never read.
    static Read readText(const std::string& path, std::string& out);
    static std::string extensionOf(const std::string& path);   // lowercase, no dot

    // What it holds. Each sets the kind; loadText watches the file.
    void loadText(const std::string& path, const std::string& content);
    void setMessage(const std::string& path, const std::string& message);
    void setMedia(const std::string& path);
    void setDiff(const std::string& title, const std::string& bytes, bool commit);
    // A tab come back from a session whose file is not read until it is
    // first shown (Editor::showActive reads it): a Message with nothing in
    // it meanwhile, so it is never saved or counted unsaved.
    void setUnread(const std::string& path);
    bool unread() const { return unread_; }

    // How the file stood when last read or saved: whether it was there, and
    // its date, inode and size.
    struct DiskStamp {
        bool exists = false;
        double mtime = 0;
        uint64_t inode = 0;
        int64_t size = -1;
    };
    const DiskStamp& diskStamp() const { return diskStamp_; }
    // A tab come back from its backup (Persistence.h): `text` is the buffer,
    // with nothing to undo; `disk` the file's text now, which counts as
    // saved, or null when it cannot be read (gone, or no longer text), and
    // then it stays unsaved until it is saved. `base` is what the backup was
    // based on, kept as the stamp when the file is gone or `changed` since.
    void restoreText(const std::string& path, const std::string& text, const std::string* disk,
                     const DiskStamp& base, bool changed);
    // Its file changed on disk since the text it holds was based on it: the
    // next check while it is on show asks Keep Mine or Reload, even though
    // its saved text is the disk's.
    void askAboutDiskChange();
    // For auto-save: the file is gone since it was read, or it is not what
    // was last read or saved (by its date, inode and size first, then by its
    // text, so a file rewritten unchanged is no change).
    bool goneFromDisk() const;
    bool changedOnDisk() const;
    // Counts every change to its text, for knowing what a backup holds.
    uint64_t edits() const { return edits_; }

    // Its backup (Persistence.h): the id, whether one is on disk, and the
    // edit count and path it was written at; and what to say in the status
    // bar when it is first shown after coming back from one.
    struct Backup {
        std::string id;
        bool written = false;
        uint64_t writtenEdits = 0;
        std::string writtenPath;
        std::string note;
    };
    Backup& backup() { return backup_; }

    Kind kind() const { return kind_; }
    const std::string& path() const { return path_; }
    const std::string& ext() const { return ext_; }
    bool isMarkdown() const { return kind_ == Kind::Text && markdown_; }
    bool isLatex() const { return kind_ == Kind::Text && latex_; }
    // The tab shows the Markdown page or the LaTeX pages rather than the
    // source. Only the flag: the editor shows it.
    bool showsPreview() const { return showsPreview_; }
    void setShowsPreview(bool on) { showsPreview_ = on; }
    // The window's preview tab (EditorTabs.h): its title in italics, and
    // the next file opened in preview replaces it.
    bool isPreviewTab() const { return previewTab_; }
    void setPreviewTab(bool on) { previewTab_ = on; }
    // The agent's own preview tab (`minicode open --note`, AgentNotes.h):
    // italic like the preview tab, replaced by the next such open, kept by
    // an edit, a save or a double-click on its title.
    bool isAgentPreview() const { return agentPreview_; }
    void setAgentPreview(bool on) { agentPreview_ = on; }

    // The source (Text) or the message (Message). Media and Diff have an
    // empty one, which is what the view holds while they are shown.
    GtkTextBuffer* buffer() const { return buffer_; }
    // The rendered Markdown page, made on first use; never undone into.
    GtkTextBuffer* page();
    Markdown::Page& pageInfo() { return page_; }
    // The whole source as UTF-8: the highlighter's mirror when it is in
    // step, else a copy out of the buffer.
    std::string text() const;
    // Replace the source with `to`: only the part between the common start
    // and end, as one user action, so undo, the highlighter and the language
    // server see one edit. An edit from the Markdown page, or its undo.
    void spliceText(const std::string& to);

    // A diff's title ("name (diff)", "<hash> <subject>"), bytes, and whether
    // it is a commit.
    const std::string& diffTitle() const { return diffTitle_; }
    const std::string& diffBytes() const { return diffBytes_; }
    bool diffIsCommit() const { return diffCommit_; }
    // A tab that compares two texts (CompareView.h) is a diff tab with this
    // set: titled by `diffTitle`, no file, read-only. Null for a git diff.
    void setCompare(const std::string& title, std::shared_ptr<const CompareData> data);
    const std::shared_ptr<const CompareData>& compare() const { return compare_; }
    // The comparison's decision (CompareDecision.h), or null.
    const std::shared_ptr<CompareDecision>& decision() const { return decision_; }
    void setDecision(std::shared_ptr<CompareDecision> d) { decision_ = std::move(d); }

    // Text only; a message, a picture or a diff is never written, nor a file
    // still being read.
    bool canSave() const { return kind_ == Kind::Text && !path_.empty() && !loading(); }
    // Differs from the file as last read or saved.
    bool dirty() const { return kind_ == Kind::Text && dirty_; }
    // Write the source to the file: through a symlink to what it names, in
    // place for a file with other hard links or another owner, else as an
    // atomic rename. False, with the reason, when it failed; it stays dirty.
    bool save(std::string* error);

    // The file now lives at `path` (a rename): the buffer, its undo and the
    // watch stay; the grammar changes with the extension, and a file that is
    // no longer Markdown or LaTeX loses its preview flag.
    void setPath(const std::string& path);

    // Changes made to the file by something else. Compares what is on disk
    // with the text last read or saved: no change, nothing; a clean document
    // takes the new text in place; one with edits asks its host.
    void checkExternalChange();
    // Reload chosen: the disk's text replaces the buffer's, in place, as one
    // undoable edit. False when it can no longer be read as text.
    bool reloadFromDisk();

    // Highlighting from scratch (the settings changed: the swatches).
    void restartHighlighting();
    // It is the settings file, whose color values are swatches, and whose
    // opacity values carry this tag, a click opening a slider.
    bool isSettingsFile() const;
    static constexpr const char* kOpacityTag = "minicode-opacity-value";
    // The view showing the source buffer, or null: the lines on screen are
    // retagged first.
    void setShownIn(GtkTextView* view) { shownIn_ = view; }
    GtkTextView* shownIn() const { return shownIn_; }

    // The highlighter and the UTF-8 text it reads, while they are in step
    // with the buffer: null without a grammar, and during a user action too
    // large to follow edit by edit. Bracket matching and auto-closing ask it
    // about strings and comments (BufferText.h).
    IncrementalHighlighter<char>* highlighter() const { return hl_ && !bulk_ ? hl_.get() : nullptr; }
    const std::string& highlighterText() const { return mirror_; }
    // Bracket-pair colors (BracketColors.h): on while the settings have them
    // and the file has a grammar and is not the settings file.
    BracketColors* bracketColors() const { return brackets_.get(); }
    // The lines on screen get the colors they lack (a scroll, a new size).
    void paintBracketsOnScreen();
    // editor.bracket-pair-colorization changed: on or off from now on.
    void bracketSettingChanged();
    // A language server's semantic tokens (SemanticColors.h, PARITY.md
    // 2.6): the spans for the text as it is now, in UTF-8 bytes of it,
    // `textBytes` its length. Kept and painted on screen when the document
    // has a highlighter in step with that text (so none in large-file mode
    // or without a grammar); false, nothing kept, otherwise.
    bool takeSemanticSpans(std::vector<Lsp::SemanticSpan>&& spans, size_t textBytes);
    SemanticColors* semanticColors() const { return semantic_.get(); }
    // The lines on screen get the semantic colors they lack (a scroll).
    void paintSemanticOnScreen();
    // editor.semantic-highlighting turned off: every semantic color goes.
    void semanticSettingChanged();
    // Its folds (Folds.h, PARITY.md 1.10): the regions, the closed folds and
    // their tag, made with the buffer.
    DocumentFolds* folds() const { return folds_.get(); }

    // A Markdown file's tasks, done and in all, recounted 0.3 s after typing
    // stops; 0 and 0 for any other file.
    int tasksDone() const { return tasksDone_; }
    int tasksTotal() const { return tasksTotal_; }
    void refreshTaskCount();

    // Its LaTeX preview, null until the editor makes one.
    LatexPreview* latexPreview() const { return latexPreview_; }
    void setLatexPreview(LatexPreview* p) { latexPreview_ = p; }

    // Where its view was while another tab took over (the editor keeps it).
    struct Place {
        bool haveView = false;
        GtkTextMark* top = nullptr;   // the source's first line on show
        int topInto = 0;              // how far into that line the view started
        double hscroll = 0;
        // The Markdown page: its first character on show, rendered again.
        bool havePage = false;
        int pageTop = 0;
        int pageInto = 0;
        // Ctrl+Shift+V keeping its place (see Editor::togglePreview).
        bool haveSrcPos = false;
        double srcCaretFrac = 0.3;
        double previewEntryV = -1;
#ifdef MINICODE_ENABLE_PDF
        bool havePdf = false;
        PdfView::Anchor pdf;
#endif
        // A tab come back from a session, not read yet: its selection and
        // view, given to it once it is (Editor::placeTab).
        bool haveRestore = false;
        EditorTabs::ClosedTab restore;
    };
    Place& place() { return place_; }

    // A stamp of when it was last on show (EditorTabs::activeAfterClose).
    uint64_t lastUsed = 0;
    // Its id, for the tab's right click menu (unique in the process).
    uint64_t id() const { return id_; }

    // ---- Big files (LargeFileView.cpp, PARITY.md item 1.12)
    // Over ../src/LargeFile.h's thresholds (20 MB or 300,000 lines) when it was
    // read. With editor.large-file-optimizations on, and until the user
    // turns them on for the tab, its features are off: every feature that
    // VS Code turns off for such a file (highlighting, folding, bracket boxes
    // and colors) and the language server check largeFileFeaturesOff().
    bool isLargeFile() const { return largeFile_; }
    bool largeFileFeaturesOff() const { return largeFile_ && largeOff_ && !largeOn_; }
    // A language server may have its text: not while it is read, not with
    // its features off, and never past VS Code's 50 MB sync limit.
    bool languageServerMayRead() const;
    // The status bar's note: everything a small file has, for this tab.
    void turnOnLargeFileFeatures();
    // A file over LargeFileView::kBackgroundBytes: read, checked and lexed on
    // a worker thread, then put in the buffer a piece at a time. Meanwhile it
    // is Text but not editable, saved, or given to a server, and the view
    // shows loadingBuffer() ("Opening…"). The host hears documentLoaded
    // when it is done or failed; whenLoaded runs `f` then (now, if it is not
    // loading).
    void beginLoading(const std::string& path);
    bool loading() const { return load_ != nullptr; }
    GtkTextBuffer* loadingBuffer() const;
    void whenLoaded(std::function<void()> f);

private:
    struct Load;
    void noteLargeFile(const std::string& content);   // decided as it is read
    void cancelLoading();
    void loadRead();                                  // the worker's result is in
    bool putPieces();                                 // a turn's pieces; true when all are in
    void finishLoading();
    static gboolean onLoadRead(gpointer job);
    static gboolean onLoadIdle(gpointer self);
    Load*  load_ = nullptr;
    bool   largeFile_ = false, largeOff_ = false, largeOn_ = false;
    size_t utf16Length_ = 0;   // as read, for the sync limit
    void clearSource();
    void fill(const std::string& text);   // set whole: no edit, nothing unsaved
    void markDirty(bool d);
    void checkDirty();
    bool matchesSaved();

    // Highlighting (see the comment at the top of the highlighting section in
    // Document.cpp).
    void startHighlighting();
    void stopHighlighting();
    void flushHighlighting();
    void retagSpan(size_t firstLine, size_t endLine);
    void retagLines(size_t firstLine, size_t endLine, const std::vector<Token>& tokens);
    void visibleLines(size_t& first, size_t& end) const;
    void cancelDeferred();
    void resyncAfterBulk();
    // A MultiEdit of many ranges (typing at many carets): its changes taken
    // in bytes before the first edit, given to the highlighter edit by edit
    // at its end (EditBatch.h) instead of read back whole.
    void startBatch();
    void finishBatch();
    void removeHighlightTags(const GtkTextIter* a, const GtkTextIter* b);
    size_t byteOffsetOf(const GtkTextIter* it) const;
    bool linesMatch() const;
    bool isHighlightTag(GtkTextTag* tag) const;
    void noteEdit(size_t pos, size_t oldLen, size_t newLen);
    void decorateColors(int firstLine, int endLine);
    bool wantsBracketColors() const;

    static void onInsertText(GtkTextBuffer* buf, GtkTextIter* loc, char* text, int len,
                             gpointer self);
    static void onDeleteRange(GtkTextBuffer* buf, GtkTextIter* a, GtkTextIter* b, gpointer self);
    static void onInsertTextAfter(GtkTextBuffer* buf, GtkTextIter* loc, char* text, int len,
                                  gpointer self);
    static void onDeleteRangeAfter(GtkTextBuffer* buf, GtkTextIter* a, GtkTextIter* b,
                                   gpointer self);
    static void onApplyTag(GtkTextBuffer* buf, GtkTextTag* tag, GtkTextIter* a, GtkTextIter* b,
                           gpointer self);
    static void onBeginUserAction(GtkTextBuffer* buf, gpointer self);
    static void onEndUserAction(GtkTextBuffer* buf, gpointer self);
    static void onChanged(GtkTextBuffer* buf, gpointer self);
    static gboolean onIdleRetag(gpointer self);

    // The disk.
    void watch();
    void stopWatching();
    void rememberDisk(const std::string& content);
    void applyDiskText(const std::string& content);
    static void onFileChanged(GFileMonitor* m, GFile* f, GFile* other, GFileMonitorEvent ev,
                              gpointer self);
    void scheduleTaskCount();

    DocumentHost* host_;
    DocumentTags* tags_;
    uint64_t      id_;
    Kind          kind_ = Kind::Text;
    std::string   path_, ext_;
    bool          markdown_ = false, latex_ = false;
    bool          showsPreview_ = false;
    bool          previewTab_ = false;
    bool          agentPreview_ = false;
    bool          unread_ = false;

    GtkTextBuffer* buffer_ = nullptr;
    bool           filling_ = false;
    GtkTextBuffer* pageBuffer_ = nullptr;
    Markdown::Page page_;
    GtkTextView*   shownIn_ = nullptr;

    std::string diffTitle_, diffBytes_;
    std::shared_ptr<const CompareData> compare_;
    std::shared_ptr<CompareDecision> decision_;
    bool        diffCommit_ = false;

    // The file as last read or saved, and whether the buffer differs.
    bool        diskKnown_ = false;
    std::string saved_;
    long        savedChars_ = 0;
    bool        dirty_ = false;
    bool        dirtyCheckPending_ = false;   // an edit inside a user action
    DiskStamp   diskStamp_;
    bool        diskQuestion_ = false;        // askAboutDiskChange
    uint64_t    edits_ = 0;
    Backup      backup_;
    GFileMonitor* monitor_ = nullptr;
    guint         checkTimer_ = 0;

    // Highlighting, live while the buffer holds text of a language the lexer
    // knows. mirror_ is a UTF-8 copy of the buffer kept in step edit by
    // edit, the TextSource the highlighter reads.
    std::unique_ptr<IncrementalHighlighter<char>> hl_;
    std::string mirror_;
    size_t pendStart_ = 0, pendEnd_ = 0;
    bool   pending_ = false;
    size_t defStart_ = 0, defEnd_ = 0;
    bool   deferred_ = false;
    guint  idleId_ = 0;
    bool   inAction_ = false;
    int    actionEdits_ = 0;
    bool   bulk_ = false;
    std::vector<EditBatch::Change> batch_;   // startBatch's, in the text before it
    bool   batching_ = false;
    int    batchEdits_ = 0, batchExpected_ = 0;   // buffer edits seen, and to come
    // Lines a batch re-lexed off screen, retagged in idle time after the
    // deferred range: byte ranges, ascending and apart, carried through
    // every later edit.
    std::vector<std::pair<size_t, size_t>> later_;
    void carryLater(size_t pos, size_t oldLen, size_t newLen);
    bool   applyingTags_ = false;
    std::unique_ptr<BracketColors> brackets_;
    std::unique_ptr<SemanticColors> semantic_;
    std::unique_ptr<DocumentFolds> folds_;

    int   tasksDone_ = 0, tasksTotal_ = 0;
    guint taskTimer_ = 0;

    LatexPreview* latexPreview_ = nullptr;
    Place place_;
};
