// Latex.h — the LaTeX preview: the document typeset by tectonic, every page
// shown in a PdfView in the editor's slot, and a double-click on the text
// opening the LaTeX behind it for editing. The GTK form of LatexView in the
// macOS build (src/Latex.mm); read "LaTeX preview" in CLAUDE.md first.
//
// How it differs from the Mac, and why:
//   * The preview has no copy of the source. The editor's GtkTextBuffer keeps
//     holding the file's text, highlighted and editable, while the preview is
//     shown in its place; the preview reads the buffer to typeset it, and an
//     edit made in the preview is a splice into the buffer, as one user
//     action. So the buffer's undo, dirty tracking and incremental
//     highlighting all see preview edits as they see typing, and Ctrl+Z in the
//     preview is the buffer's undo, where the Mac keeps a stack of its own.
//   * A double-click is only traced back to the source while the PDF on screen
//     was typeset from the buffer as it is now. SyncTeX's lines belong to the
//     source that was typeset; with the buffer ahead of the PDF they could
//     name the wrong text, so the click waits for the retypeset instead.
//
// Everything else follows the Mac: tectonic from MINICODE_TECTONIC, then
// ~/.local/share/minicode/bin, then PATH, with an offer to download the
// pinned release; the buffer typeset from a hidden sibling
// `.<file name>.<preview number>.minicode.tex` (so relative \input and
// \includegraphics resolve and the user's file is never written; the full
// name and the number keep notes.tex, notes.ltx and a second window's copy
// apart) into $XDG_RUNTIME_DIR, or a private folder in /tmp; debounced 0.6 s and
// generation-counted; the log shown when it does not typeset; Export PDF
// copies exactly the bytes tectonic wrote.
//
// Compiled only with MINICODE_ENABLE_PDF.
#pragma once

#ifdef MINICODE_ENABLE_PDF

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "LatexDoc.h"
#include "SyncTex.h"

class PdfView;

class LatexPreview {
public:
    explicit LatexPreview(GtkTextBuffer* buffer);
    ~LatexPreview();
    LatexPreview(const LatexPreview&) = delete;
    LatexPreview& operator=(const LatexPreview&) = delete;

    GtkWidget* widget() const { return root_; }

    // The .tex file the buffer holds. A different path drops what was shown
    // for the old one and stops its typeset; the same path (a rename that
    // kept it, a reload) keeps the pages on screen.
    void setPath(const std::string& path);
    // In the slot or not. Coming into view typesets the buffer when the PDF
    // shown is not of it; while in view, edits retypeset after a pause.
    void setActive(bool active);
    // Nothing to show any more (another file, or the folder closed): stop any
    // typeset, forget the document, clear the pages.
    void close();

    // Typeset now, skipping the typing pause. Queues one more run if one is
    // going, as the Mac does.
    void compileNow();

    // Ctrl+Shift+S: ask where, then write the PDF for the buffer as it is
    // now, typesetting it first if the one on screen is behind.
    void exportPdf(GtkWindow* parent);
    // The PDF for the buffer as it is now, exactly as tectonic wrote it: at
    // once when it is on screen, otherwise after a typeset. `pdf` is null and
    // `error` says why when it does not typeset. Always called on the main
    // loop, never after the preview is gone.
    using PdfCb = std::function<void(GBytes* pdf, const std::string& error)>;
    void pdfForBuffer(PdfCb done);

    // Where tectonic is, or empty: MINICODE_TECTONIC, then the copy the app
    // downloads into ~/.local/share/minicode/bin, then PATH.
    static std::string tectonicPath();
    static std::string managedTectonicPath();

    // What the click path and the popover do, reachable for tests: the
    // double-click PdfView reports (0-based page, points from the page's
    // top-left), the popover's text, and its Save and Cancel.
    void doubleClick(int page, double x, double y);
    bool editing() const;
    std::string editText() const;
    void setEditText(const std::string& text);
    void commitEdit();
    void startAddItem();
    void cancelEdit();

    // State, for tests and the status line.
    PdfView* pdfView() const { return pdf_; }
    bool busy() const { return proc_ != nullptr || debounce_ != 0; }
    const std::string& pdfSource() const { return pdfSrc_; }
    std::string status() const;
    bool showingLog() const;
    std::string logText() const;

private:
    struct Job;
    struct Download;

    std::string bufferText() const;
    std::string scratchPath() const;   // the hidden sibling
    std::string outDir() const;        // where tectonic writes, per document
    void scheduleCompile();
    void stopRun();                    // kill a running typeset, drop its result
    void finishCompile(Job* job, GBytes* output, bool ok);
    void answerWaiters(const std::string& error);
    void showDocument();
    void showFailure(const std::string& log);
    void showTectonicMissing();
    void setStatus(const std::string& text, bool busy);
    void setButton(const char* label, bool sensitive);
    void downloadTectonic();
    void finishDownload(const std::string& problem);

    void showPopover(const std::string& title, const std::string& text, bool allowsItem,
                     const GdkRectangle& anchor);
    void spliceBuffer(const std::string& from, const std::string& to);

    static void onBufferChanged(GtkTextBuffer* buf, gpointer self);
    static void onPdfClick(void* self, int page, double x, double y, int nPress);
    static void onButton(GtkButton* b, gpointer self);
    static void onShutdown(GApplication* app, gpointer self);
    static gboolean onEditKey(GtkEventControllerKey* c, guint keyval, guint keycode,
                              GdkModifierType state, gpointer self);
    static gboolean onUndo(GtkWidget* w, GVariant* args, gpointer self);
    static gboolean onRedo(GtkWidget* w, GVariant* args, gpointer self);

    GtkTextBuffer* buffer_ = nullptr;

    GtkWidget* root_    = nullptr;   // vbox: status bar, then the stack
    GtkWidget* status_  = nullptr;   // the label
    GtkWidget* spinner_ = nullptr;
    GtkWidget* button_  = nullptr;   // Recompile / Download…
    GtkWidget* stack_   = nullptr;   // the pages, or the log
    GtkWidget* logView_ = nullptr;
    GtkWidget* logScroll_ = nullptr;
    PdfView*   pdf_     = nullptr;

    GtkWidget* popover_  = nullptr;
    GtkWidget* popLabel_ = nullptr;
    GtkWidget* popText_  = nullptr;   // GtkTextView holding the span's LaTeX
    GtkWidget* popAdd_   = nullptr;   // "Add item", for spans in a list

    std::string path_;
    bool active_ = false;
    guint debounce_ = 0;
    gulong changedId_ = 0;
    gulong shutdownId_ = 0;

    // The run in progress, if any. generation_ counts runs; a result whose
    // generation is not the latest is dropped.
    GSubprocess* proc_ = nullptr;
    std::string runScratch_;
    // The run's hidden sibling, removed through its folder rather than by
    // path: an open descriptor of the folder follows it through a rename or
    // a move to the trash, so the copy never outlives the run wherever its
    // folder went (removeScratch).
    int         runDirFd_ = -1;
    std::string runScratchName_;
    void        removeScratch();
    // Distinguishes this preview's sibling from another window's of the
    // same file (see scratchPath).
    unsigned    instance_ = 0;
    unsigned generation_ = 0;
    bool queued_ = false;
    bool downloading_ = false;
    bool missing_ = false;          // the last attempt found no tectonic

    // What is on screen: the PDF's bytes, the source it was typeset from, and
    // that source's SyncTeX and structure.
    GBytes*      pdfBytes_ = nullptr;
    std::string  pdfSrc_;
    SyncTexIndex sync_;
    std::string  syncScratch_;      // the file that SyncTeX data was typeset from
    LatexDoc     doc_;
    std::vector<PdfCb> waiters_;

    // The popover's edit: the span, the source it indexes into, and whether
    // it is composing a new \item instead.
    LatexSpan   editSpan_;
    std::string editSrc_;
    bool        editValid_ = false;
    bool        addingItem_ = false;
    LatexList   editList_;
    int         editItem_ = -1;
    GdkRectangle editAnchor_{0, 0, 1, 1};

    // Cleared when the preview goes, so a late callback (a typeset or a
    // download finishing) knows not to touch it.
    std::shared_ptr<LatexPreview*> alive_;
};

#endif  // MINICODE_ENABLE_PDF
