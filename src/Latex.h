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

// The problems the last typeset reported, in order (none once one works).
// The failure panel lists them above tectonic's log, each with a place a
// link that calls onOpenProblem; onProblemsChanged runs when they change.
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

// Called when an edit in the preview changed the document.
@property(nonatomic, copy) void (^onSourceEdited)(NSString *newSource);

// The source changed elsewhere (the user typed in the editor, or opened a
// different file); re-typeset it.
- (void)setPath:(NSString *)texPath source:(NSString *)source;
// Preview edits have their own undo stack: the PDF is not a text view, so
// Cmd+Z here steps back through the edits made in the preview.
- (BOOL)canUndoEdit;
- (BOOL)canRedoEdit;
- (void)undoEdit;
- (void)redoEdit;
// The source was replaced from disk: the edits before it cannot be undone.
- (void)forgetEdits;
- (void)compileNow;
// The PDF for `source`, exactly as tectonic wrote it. Handed over straight
// away when the preview is already showing that source, otherwise after a
// typeset of it; `error` is set instead when it does not typeset. The
// completion always runs on the main thread.
- (void)pdfForPath:(NSString *)texPath source:(NSString *)source
        completion:(void (^)(NSData *pdf, NSString *error))done;
- (void)applySettings;
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

// Where previews typeset: each document gets a folder of its own under
// MCLatexWorkRoot() (MiniCode-latex in the temporary folder, or
// $MINICODE_LATEX_WORK), named for its resolved path, so two files of one
// name never share a PDF. The folder goes when the last preview of it does.
NSString *MCLatexWorkRoot(void);
NSString *MCLatexWorkFolder(NSString *texPath);
// Removes the hidden siblings that previews of crashed processes left next to
// their documents (only a file with the preview copy's name and exactly the
// bytes recorded), and their records. Runs off the main thread at the first
// typeset of a launch; `done` (main thread) gets how many it removed.
void MCLatexRemoveStaleSiblings(void (^done)(NSUInteger removed));

#ifdef __cplusplus
#import <Quartz/Quartz.h>
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
#endif
