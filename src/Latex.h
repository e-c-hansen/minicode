// Latex.h — LaTeX preview panel: a compiled PDF you can edit through.
//
// The document is typeset by tectonic (a single external binary, the same way
// the planned LSP client leans on servers the user already has), shown with
// PDFKit, and double-clicking a piece of text opens the LaTeX that produced it.
// Editing that text replaces exactly its bytes in the source; everything the
// parser does not understand is left alone.
#import <Cocoa/Cocoa.h>

@interface LatexView : NSView <NSMenuItemValidation>
- (instancetype)initWithPath:(NSString *)texPath source:(NSString *)source;

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
- (void)compileNow;
- (void)applySettings;
@end

// Where tectonic was found, or nil. Exposed for the status line and tests.
NSString *MCTectonicPath(void);

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
