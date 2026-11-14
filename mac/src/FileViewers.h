// FileViewers.h — the viewers that take the editor's slot for files that
// are not plain text, beyond the image, PDF and media viewers in
// EditorController: Quick Look for anything minicode has no viewer for,
// and a table for CSV and TSV files. EditorController keeps one
// MCFileViewers per window and calls it from a few hooks (loading and
// showing a tab, laying out the slot, letting go); which viewer a file gets
// comes from FileSniff, by content first.
//
// Two sorts of viewer:
//   - without text (Quick Look, a CSV too large to edit): the tab holds no
//     text and nothing is saved from it, as for an image;
//   - over the tab's text (a CSV table): the tab's text is the document,
//     the viewer shows it in place of the text view while the tab is in
//     preview mode (Shift+Cmd+V flips to the text), and an edit made in the
//     viewer comes back through onTextEdited as the file's whole new text.
#import <Cocoa/Cocoa.h>

@class MCQuickLookSlot, MCCsvTableView, MCJsonOutlinePane, MCSqliteView;

typedef NS_ENUM(NSInteger, MCViewerKind) {
    MCViewerNone = 0,      // the text view, as before
    MCViewerQuickLook,     // without text: QLPreviewView
    MCViewerCsv,           // over the text: a table, comma (or ; |) separated
    MCViewerTsv,           // over the text: a table, tab separated
    MCViewerCsvLarge,      // without text: a CSV or TSV too large to edit
    MCViewerJson,          // beside the text: an outline of a JSON file
    MCViewerNotebook,      // over the text: a Jupyter notebook, rendered
    MCViewerSqlite,        // without text: a SQLite database, read-only
};

// Files over this many bytes open read-only in the table (MCViewerCsvLarge).
extern const unsigned long long MCCsvEditableBytes;
// How many rows of such a file are shown.
extern const NSUInteger MCCsvLargeRows;

// The viewer for a file, from its first bytes, its name and its size.
// Images, PDFs, video and audio are routed before this is asked.
MCViewerKind MCViewerKindForFile(NSString *path);
// A viewer of a file that has no text in the tab (nothing to save).
BOOL MCViewerKindWithoutText(MCViewerKind kind);
// A viewer shown over the tab's text in preview mode, which Shift+Cmd+V
// flips to the text and back.
BOOL MCViewerKindOverText(MCViewerKind kind);
// A viewer shown beside the editable text (the JSON outline), which
// Shift+Cmd+V hides and shows again.
BOOL MCViewerKindBesideText(MCViewerKind kind);
// A file shown, in preview mode, as Markdown made from it (a notebook):
// the Markdown preview renders it, read-only, in the text view.
BOOL MCViewerKindRendersMarkdown(MCViewerKind kind);
// That Markdown, for a notebook's text (Notebook.h).
NSString *MCNotebookMarkdown(NSString *json);

@interface MCFileViewers : NSObject
// Views are made on first use, as subviews of `container` below `sibling`.
- (instancetype)initWithContainer:(NSView *)container below:(NSView *)sibling;
@property(nonatomic, readonly) MCViewerKind shownKind;
@property(nonatomic, readonly) MCQuickLookSlot *quickLook;   // nil until used
@property(nonatomic, readonly) MCCsvTableView *csv;          // nil until used
@property(nonatomic, readonly) MCJsonOutlinePane *json;      // nil until used
@property(nonatomic, readonly) MCSqliteView *sqlite;         // nil until used
// Whether a viewer beside the text shows (Shift+Cmd+V), for the window.
@property(nonatomic, assign) BOOL besideTextShown;

// An edit made in a viewer over the text: the file's whole new text, and
// a name for Undo.
@property(nonatomic, copy) void (^onTextEdited)(NSString *text, NSString *actionName);
// The undo manager a viewer's edits are undone with (the active tab's).
@property(nonatomic, copy) NSUndoManager * (^undoManagerProvider)(void);

// A viewer with no text behind it; NO when it cannot show the file.
- (BOOL)showWithoutText:(MCViewerKind)kind path:(NSString *)path;
// A viewer over the tab's text; shown again with new text (an undo, a
// reload) it keeps its place.
- (void)showText:(NSString *)text kind:(MCViewerKind)kind path:(NSString *)path;
// A viewer beside the editable text, following it.
- (void)showBesideTextView:(NSTextView *)textView kind:(MCViewerKind)kind path:(NSString *)path;
// Nothing shown any more (another tab, another kind of file, the text).
- (void)clear;
// What is being typed into a viewer (a CSV cell) goes into its document
// now, before the tab or the mode changes.
- (void)commitEditing;
// Lays the viewers out in the editor's slot and returns what is left of it
// for the text view; *coversText is YES when a viewer takes all of it.
- (NSRect)layoutInRect:(NSRect)rect editorShown:(BOOL)shown coversText:(BOOL *)coversText;
// The view that should get the keyboard (Cmd+1), or nil.
- (NSView *)focusView;
@end
