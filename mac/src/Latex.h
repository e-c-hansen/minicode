// Latex.h — LaTeX preview panel: a compiled PDF you can edit through.
//
// The document is typeset by tectonic (a single external binary, the same way
// the planned LSP client leans on servers the user already has), shown with
// PDFKit, and double-clicking a piece of text opens the LaTeX that produced it.
// Editing that text replaces exactly its bytes in the source; everything the
// parser does not understand is left alone.
#import <Cocoa/Cocoa.h>
#import <Quartz/Quartz.h>

// A problem tectonic reported, placed in a file of the user's (LatexLog.h).
@interface MCLatexProblem : NSObject
@property(nonatomic, copy) NSString *path;         // nil when it names no file found
@property(nonatomic, assign) NSInteger line;       // 1-based; 0 when it gives none
@property(nonatomic, assign) NSInteger severity;   // 1 error, 2 warning
@property(nonatomic, copy) NSString *message;
@end

@interface LatexView : NSView <NSMenuItemValidation>
- (instancetype)initWithPath:(NSString *)texPath source:(NSString *)source;

// The problems the last typeset reported: its errors, then its warnings
// (undefined references and citations, labels defined twice, overfull
// boxes, packages' warnings with a line), each once. The failure panel
// lists them above tectonic's log, and a typeset that worked lists its
// warnings behind the status bar's Warnings button, each row in its
// severity's color with its place a link that calls onOpenProblem;
// onProblemsChanged runs when they change.
@property(nonatomic, readonly) NSArray<MCLatexProblem *> *problems;
@property(nonatomic, copy) void (^onOpenProblem)(NSString *path, NSInteger line);
@property(nonatomic, copy) void (^onProblemsChanged)(void);
// The problems on lines of `path`, while `source` is still the text that
// failed to typeset (after an edit the lines may name other text); for
// the gutter's dots.
- (NSArray<MCLatexProblem *> *)problemsForPath:(NSString *)path source:(NSString *)source;

// Forward search: scroll the page to what line `line` (1-based) of the
// document put there and light it up for a moment. Waits for the typeset
// under way, or the one an edit scheduled, so the page matches the text.
- (void)revealSourceLine:(NSInteger)line;
// The same for a line of another file the document reads (\input,
// \include), through that file's SyncTeX input tag.
- (void)revealSourceLine:(NSInteger)line inFile:(NSString *)path;
// Whether the last typeset read the file at `path` as an input of its own
// (not the document itself), so a forward search from it lands here.
- (BOOL)typesetReadFile:(NSString *)path;

// Inverse search, the page to the source (SyncTeX, as TeXShop and LaTeX
// Workshop do it): Command-click on the page, or Show in Source in its
// context menu, calls onShowSource with the place behind the point: `path`
// nil for the document itself, else the \input file's; `line` 1-based;
// `column` 1-based in that line at the clicked word when it is found in
// the source, else 0 (the line's start). The words are matched as a
// double-click matches them; SyncTeX's line alone when they are not.
@property(nonatomic, copy) void (^onShowSource)(NSString *path, NSInteger line, NSInteger column);
// The same for a point on a page (page space), as the click does it; NO
// when there is no SyncTeX or nothing under the point.
- (BOOL)showSourceForPage:(PDFPage *)page point:(NSPoint)pagePoint;
// The document's line highest in the part of the page on show (1-based),
// for Shift+Cmd+V from the page; 0 when no typeset page shows.
- (NSInteger)sourceLineOnScreen;
// Whether line `line` of the document put something in the part of the
// page on show.
- (BOOL)sourceLineIsOnScreen:(NSInteger)line;
// The page to line `line` of the document, quietly (no light, no status),
// and only when none of it is on show; after the typeset under way when
// the page is not the text's yet. Shift+Cmd+V to the page, and the page
// beside the source following its caret.
- (void)keepSourceLineInView:(NSInteger)line;
// The buffer as it is now, while the page sits beside the source and
// typesets only when saved (nil otherwise): a double-click on a page older
// than it typesets it first and edits nothing, since the page's places
// name text that has moved.
@property(nonatomic, copy) NSString *(^bufferText)(void);
// The tab's text as it is now, wherever it is (the editor beside the page,
// or the tab's buffer behind the full preview). The edit box applies its
// edit to this text, its span found again in it (MarkdownEdit::rebaseSpan)
// when an agent, a reload or Cmd+Z changed it while the box was open, and
// refuses, the text going to the clipboard, when the span itself changed:
// never at the old offsets, never over someone else's change.
@property(nonatomic, copy) NSString *(^sourceNow)(void);

// This preview's own output folder and hidden sibling: each preview has
// both to itself (LatexScratch::Owner), so two windows on one file never
// share a copy or a PDF.
@property(nonatomic, readonly) NSString *workFolder;
@property(nonatomic, readonly) NSString *siblingPath;

// Called when an edit in the preview changed the document.
@property(nonatomic, copy) void (^onSourceEdited)(NSString *newSource);

// The files the document reads (\input, \include): a double-click on
// their text in the PDF edits them. inputText gives a file's text as it is
// now (its tab's buffer, else nil for the disk's); onInputEdited puts an
// edit into the file's tab (`range` is UTF-16 in that text), opening the
// tab in the background when need be, unsaved, NO when it could not;
// inputUnsaved says whether a file has unsaved changes, which the typeset
// does not see, and the bar says so.
@property(nonatomic, copy) NSString *(^inputText)(NSString *path);
@property(nonatomic, copy) BOOL (^onInputEdited)(NSString *path, NSRange range, NSString *text);
@property(nonatomic, copy) BOOL (^inputUnsaved)(NSString *path);
// The files of the user's the last typeset read besides the document, as
// the document's folder spells them.
- (NSArray<NSString *> *)inputFiles;
// The bar's page count and notes again (an \input file was saved or
// edited elsewhere), when the page is on show.
- (void)refreshStatus;

// The source changed elsewhere (the user typed in the editor, or opened a
// different file); re-typeset it.
- (void)setPath:(NSString *)texPath source:(NSString *)source;
// Preview edits have their own undo stack: the PDF is not a text view, so
// Cmd+Z here steps back through the edits made in the full preview (an
// edit beside the source is the source's own undo step instead). The
// stack is cleared when the text changes from elsewhere (typing, a
// reload), since its snapshots would put the old text back over it.
- (BOOL)canUndoEdit;
- (BOOL)canRedoEdit;
- (void)undoEdit;
- (void)redoEdit;
// The source was replaced from disk: the edits before it cannot be undone.
- (void)forgetEdits;
- (void)compileNow;
// The window closes: a typeset under way is stopped, none starts again, and
// the output folder goes (once that typeset has ended), whenever the view
// itself is freed.
- (void)stopPreview;
// The PDF for `source`, exactly as tectonic wrote it. Handed over straight
// away when the preview is already showing that source, otherwise after a
// typeset of it; `error` is set instead when it does not typeset. The
// completion always runs on the main thread.
- (void)pdfForPath:(NSString *)texPath source:(NSString *)source
        completion:(void (^)(NSData *pdf, NSString *error))done;
- (void)applySettings;
// The page's edit box (a double-click): Return or Apply puts the edit in
// the document, never on disk; Esc or Cancel drop it; switching apps
// leaves the box open, and anything else that closes it applies what was
// typed (one undo step). editPending says it holds a change; commitPending
// applies it (or closes an unchanged box): the tab leaving, Cmd+S, a close.
@property(nonatomic, readonly) BOOL editPending;
- (void)commitPendingEdit;
// The PDF on show (PageZoom.h zooms it with Cmd+=, Cmd+- and Cmd+0).
- (PDFView *)latexPDFView;
@end

// Where tectonic was found, or nil. Exposed for the status line and tests.
NSString *MCTectonicPath(void);

// Downloads the pinned tectonic release into Application Support, where
// MCTectonicPath finds it. `done` runs on the main thread with nil, or with
// why it failed; a download already under way is shared, not repeated.
void MCDownloadTectonic(void (^done)(NSString *problem));
// Whether a download is under way.
BOOL MCTectonicDownloading(void);

// Where previews typeset: each preview gets a folder of its own under
// MCLatexWorkRoot() (Mutible-latex in the temporary folder, or
// $MUTIBLE_LATEX_WORK), named for the document's resolved path and the
// preview (this process and the preview's number, `view`), so two files of
// one name, or two previews of one file, never share a PDF. The folder goes
// with its preview; one a crashed process left goes at the next launch.
NSString *MCLatexWorkRoot(void);
NSString *MCLatexWorkFolder(NSString *texPath, NSUInteger view);
// Removes the hidden siblings that previews of crashed processes left next to
// their documents (only a file with the preview copy's name and exactly the
// bytes recorded), their records, and the output folders of processes that
// have gone. Runs off the main thread at the first typeset of a launch;
// `done` (main thread) gets how many siblings it removed.
void MCLatexRemoveStaleSiblings(void (^done)(NSUInteger removed));

#ifdef __cplusplus
#import <Quartz/Quartz.h>
#include <functional>
#include <vector>
struct LatexDoc;
struct LatexSpan;
class SyncTexIndex;
struct SyncTexHit;
// The whole click-to-source path: SyncTeX's candidate lines for the point,
// the word PDFKit finds under it, and LatexDoc's choice of span (nullptr when
// it refuses). `pagePoint` is in page space, `pageNumber` is 1-based, and
// `tag` is the SyncTeX input of the typeset file (0 = any). Shared with
// tests/latex/sweep.mm, which clicks every word of a document through it.
const LatexSpan *MCLatexSpanAtPoint(const LatexDoc &doc, const SyncTexIndex &sync,
                                    int tag, PDFPage *page, int pageNumber,
                                    NSPoint pagePoint,
                                    std::vector<SyncTexHit> *hitsOut);
// The same over a document of several files: the inputs the boxes under the
// point name, the likeliest first, then the document (`mainTag`)
// (SyncTexIndex::tagsAtPoint), each tried with its own source from `docFor`
// (nullptr for an input that is no file of the user's), until one places
// the click. *tagOut is the input whose span it is (0 when refused).
const LatexSpan *MCLatexSpanAtPointInFiles(
    const SyncTexIndex &sync, int mainTag, PDFPage *page, int pageNumber, NSPoint pagePoint,
    const std::function<const LatexDoc *(int tag)> &docFor, int *tagOut,
    std::vector<SyncTexHit> *hitsOut);
#endif
