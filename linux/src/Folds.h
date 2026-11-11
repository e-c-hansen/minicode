// Folds.h — code folding in the GTK port, linux/PARITY.md item 1.10 (D8).
// The regions and the closed folds are the core's (../src/Folding.h); the
// Mac's counterpart is src/FoldView.mm (MCFolds) with CodeTextView's hooks.
//
// How a fold hides its lines. The text never changes (D6): an invisible
// tag, `minicode-fold`, lies over the whole buffer lines a fold hides, from
// the start of its region's second line to the start of the line after its
// last, so GTK gives those lines no height and the caret steps over them.
// The gutter, undo, the highlighter, the language server and a copy (the
// view's copy handler, LargeFileUi::copyWhole) all see the text whole.
// Folds are Fold::Collapsed spans in characters (GtkTextIter's offsets, a
// unit a code point), from the break that ends a fold's first line to the
// end of its last line's text, as on the Mac, so they follow edits; an edit
// touching what one hides opens it (Collapsed::edit), and the tag follows.
// The long lines' tag (LargeFileView.h, `minicode-long-line-rest`) is
// another invisible tag over other text: neither touches the other's.
//
// The limit (D8). GTK's drawing visits every line between the first and the
// last line on screen, hidden ones too, about 6 µs each a frame, so the
// folds closed in a tab hide at most kMaxHiddenLines lines between them. A
// fold that would pass it stays open, with a note in the status bar; Fold
// All, Fold Level N, Fold Recursively and Fold All Comments close what fits
// (Fold::pickWithinBudget: outermost first, from the top of the file down,
// a region too big left open and its inner regions tried at the next
// depth) and say how many stayed open.
//
// Per document (DocumentFolds, which Document owns): the regions, computed
// by Fold::compute on a worker thread 0.3 s after the text last changed
// (generation counted, the latest result kept), and computed on the spot
// for a command when they are not for the text as it is, or the language
// server's (textDocument/foldingRange, asked 0.6 s after typing by
// LspSessionFolding.cpp) while they are for the text; the closed folds
// and the tag; the fold lane's source for the gutter (BufferLines.h);
// the caret or a selection landing in hidden text (Find, Go to Line, a
// definition, an undo) opening what hides it; and none of it for anything
// but a text file's source, nor in large-file mode
// (Document::largeFileFeaturesOff). A reload from disk forgets the folds.
// Folds stay with their tab, and with the source while its Markdown page
// shows (the page is a buffer of its own, D3).
//
// Per editor (FoldUi): the placeholder after a folded line (a CodeView pass,
// AboveText order 20) and a click on it, the keys beside a fold (Backspace
// just after it or Delete just before it opens it, deleting nothing; Return
// at the end of a folded line makes a line below the fold, as on the Mac),
// and the View > Folding commands with their notes.
#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "BufferLines.h"
#include "Folding.h"

class Document;
class Editor;

class DocumentFolds : public GutterFolds {
public:
    // D8's limit on the lines a tab's closed folds hide between them.
    static constexpr size_t kMaxHiddenLines = 5000;

    // For `doc`'s source buffer, made once in Document's constructor after
    // the buffer's BufferLines.
    DocumentFolds(Document* doc, GtkTextBuffer* buffer);
    ~DocumentFolds() override;
    DocumentFolds(const DocumentFolds&) = delete;
    DocumentFolds& operator=(const DocumentFolds&) = delete;
    // The folds of a source buffer, else null.
    static DocumentFolds* of(GtkTextBuffer* buffer);

    // ---- GutterFolds (the fold lane)
    bool hasClosedFolds() const override { return !folds_.empty(); }
    bool startsRegion(size_t line) const override;
    bool startsClosedFold(size_t line) const override;
    void toggle(size_t line) override;   // a click on a chevron
    size_t lastHiddenLine(size_t line) const override;

    // Whether the document folds at all now: a text file's source, read,
    // with a name, and not in large-file mode.
    bool applies() const;
    // The regions as last computed (maybe for the text a moment ago), and
    // for the text as it is now (computed on the spot if need be). The
    // language server's, while they are for the text as it is, win over
    // Fold::compute's (PARITY.md 2.13).
    const std::vector<Fold::Range>& regions() const {
        return serverRegionsCurrent() ? server_ : core_;
    }
    const std::vector<Fold::Range>& freshRegions();
    bool regionsCurrent() const { return serverRegionsCurrent() || coreCurrent(); }
    // textDocument/foldingRange's answer (LspSessionFolding.cpp), asked
    // when generation() was `generation`: Fold::sanitize'd and taken over
    // the computed regions until the text changes, as the Mac's MCFolds
    // setServerRanges. False (nothing taken) when the text moved on since,
    // or the document does not fold. An empty answer leaves the computed
    // regions.
    bool setServerRanges(std::vector<Fold::Range> ranges, uint64_t generation);
    bool serverRegionsCurrent() const { return haveServer_ && serverGen_ == generation_; }
    const Fold::Collapsed& collapsed() const { return folds_; }
    const std::vector<Fold::Span>& outermost() const { return outer_; }
    // The lines the closed folds hide between them.
    size_t hiddenLines() const;
    // Where a fold's tag lies: from the start of its second line to the
    // start of the line after its last (or the text's end).
    void tagRange(const Fold::Span& f, size_t* start, size_t* end) const;

    // ---- commands, at the carets (the insert mark's selection and
    // BufferLines::extraCarets). Each returns whether it changed anything;
    // a note for the status bar (the limit) goes to onNote.
    bool foldAtCarets();          // the innermost region not folded yet
    bool unfoldAtCarets();        // the fold on the caret's line, or each starting in the selection
    bool toggleAtCarets();
    bool foldRecursivelyAtCarets();
    bool unfoldRecursivelyAtCarets();
    bool foldAll();
    bool unfoldAll();
    bool foldLevel(int level);    // depth `level`, those holding a caret left open
    bool foldAllComments();
    bool toggleLine(size_t line);
    // The keys beside a fold (FoldUi): Backspace at the start of the line
    // after a fold, Delete at the end of a folded line. True when a fold
    // opened (the key then does nothing else).
    bool openFoldEndingBefore(size_t caret);
    bool openFoldStartingAt(size_t caret);
    // Return at the end of a folded line: the caret goes to the end of the
    // fold's last line for the key, so the new line comes below the fold
    // (true), and comes back if the key put nothing in.
    bool moveCaretBelowForReturn(size_t caret);
    // A reload from disk: every fold forgotten.
    void forget();
    // The file's name or large-file mode changed: regions again.
    void sourceChanged();

    std::function<void(const std::string&)> onNote;   // FoldUi's, for the buffer on show
    std::function<void()> onFoldsChanged;             // FoldUi's: the placeholders draw again

    // For the checks.
    uint64_t generation() const { return generation_; }
    unsigned computes() const { return computes_; }

private:
    BufferLines* lines() const;
    size_t charCount() const;
    gunichar charAt(size_t offset) const;
    size_t breakLength(size_t offset) const;   // 2 for "\r\n", 1 for a break, 0 otherwise
    size_t contentEnd(size_t line) const;
    Fold::Span spanOf(const Fold::Range& r) const;
    const Fold::Span* foldStartingOnLine(size_t line) const;   // outermost or not
    std::vector<size_t> caretOffsets() const;   // the insert mark's selection start first
    std::vector<size_t> caretLines() const;
    std::vector<Fold::Range> closedLines() const;   // the outermost folds, as lines

    // Closing under the limit: the candidates (indexes into `rs`) that fit,
    // and a note when some do not (`single`: one region was asked for).
    bool closeWithinLimit(const std::vector<Fold::Range>& rs, const std::vector<size_t>& candidates,
                          bool single);
    bool openFold(const Fold::Span& f);
    void foldsChanged();          // outer_ again, the gutter and placeholders told
    void moveCaretsOutOfFolds();  // a caret left in what was just hidden goes to its fold's start
    void revealSelection();       // the carets in hidden text: what hides them opens
    bool hidden(size_t offset) const;   // inside some fold's tag
    // The tag made right over [a, b) (characters, the text as it is): taken
    // off, then put back over each outermost fold's part of it.
    void retag(size_t a, size_t b);
    void applyTag(size_t a, size_t b);
    void note(const std::string& message);

    void scheduleCompute();
    static gboolean onComputeTimer(gpointer self);
    void computeNow();            // on the worker thread, a copy of the text
    void computed(uint64_t gen, std::vector<Fold::Range> ranges, const std::string& name);

    static void onInsertBefore(GtkTextBuffer* b, GtkTextIter* at, char* text, int len, gpointer self);
    static void onDeleteBefore(GtkTextBuffer* b, GtkTextIter* from, GtkTextIter* to, gpointer self);
    static void onInsertAfter(GtkTextBuffer* b, GtkTextIter* at, char* text, int len, gpointer self);
    static void onDeleteAfter(GtkTextBuffer* b, GtkTextIter* from, GtkTextIter* to, gpointer self);
    static void onApplyTag(GtkTextBuffer* b, GtkTextTag* tag, GtkTextIter* a, GtkTextIter* z,
                           gpointer self);
    static void onMarkSet(GtkTextBuffer* b, GtkTextIter* at, GtkTextMark* mark, gpointer self);

    Document*      doc_;
    GtkTextBuffer* buffer_;
    GtkTextTag*    tag_ = nullptr;
    Fold::Collapsed folds_;
    std::vector<Fold::Span> outer_;
    // The regions: computed for the text at coreGen_; generation_ counts edits.
    std::vector<Fold::Range> core_;
    uint64_t coreGen_ = 0, generation_ = 1;
    bool     haveCore_ = false;
    bool coreCurrent() const { return haveCore_ && coreGen_ == generation_; }
    // The language server's, for the text at serverGen_.
    std::vector<Fold::Range> server_;
    uint64_t serverGen_ = 0;
    bool     haveServer_ = false;
    guint    timer_ = 0;
    gint64   lastEdit_ = 0;       // monotonic µs
    bool     computing_ = false, again_ = false;
    unsigned computes_ = 0;
    // The edit under way: where it is, how long its text is, and the folds
    // it opened (between the before and after handlers).
    bool     editing_ = false;
    size_t   editPos_ = 0, editNewLen_ = 0;
    std::vector<Fold::Span> opened_;
    bool     applyingTag_ = false;
    // Return's trip below a fold (moveCaretBelowForReturn).
    bool     returning_ = false;
    size_t   returnFrom_ = 0;
    guint    returnIdle_ = 0;
    guint    revealIdle_ = 0;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

class FoldUi {
public:
    explicit FoldUi(Editor* editor);
    ~FoldUi();
    FoldUi(const FoldUi&) = delete;
    FoldUi& operator=(const FoldUi&) = delete;

    // The View > Folding commands, as "win." actions in `map` (main.cpp).
    void addActions(GActionMap* map);
    // The view's key handler (Editor::onViewKey) asks first: true when the
    // key was taken here.
    bool keyPressed(guint keyval, GdkModifierType mods);
    // A note for the status bar (the limit), set by main.cpp.
    std::function<void(const std::string&)> onNote;

    // The folds of the buffer on show, when it is editable source.
    DocumentFolds* folds() const;
    // Runs a command on the buffer on show: false (the bell) when the editor
    // does not have the keyboard or shows no source.
    bool run(const std::function<bool(DocumentFolds*)>& command);

    // For the checks: the placeholders the last snapshot drew, in buffer
    // coordinates, with their fold's start.
    struct Placeholder {
        GdkRectangle rect;
        size_t start;
    };
    const std::vector<Placeholder>& drawnPlaceholders() const { return drawn_; }

private:
    void drawPlaceholders(GtkSnapshot* s, const GdkRectangle& visible);
    void watchBuffer();
    void updateActions();
    static void onPressed(GtkGestureClick* g, int n, double x, double y, gpointer self);

    Editor*     editor_;
    GtkWidget*  view_;
    int         pass_ = -1;
    GtkTextBuffer* buffer_ = nullptr;   // the view's, whose folds' callbacks are ours
    std::vector<Placeholder> drawn_;
    std::vector<GSimpleAction*> actions_;
};
