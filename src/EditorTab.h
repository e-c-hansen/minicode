// EditorTab.h — MCTab, one open document of an editor window.
#import <Cocoa/Cocoa.h>
#import "FileViewers.h"

@class LatexView, MCComparison;
@protocol MCEditorPage;

// ---------------------------------------------------------------------------
// A file on disk as the tabs see it (FolderSearch's FileStamp and stable
// read). Symlinks are followed: a tab on a link watches the file it names.
// ---------------------------------------------------------------------------
@interface MCDiskStamp : NSObject
@property(nonatomic, strong) NSDate *modDate;
@property(nonatomic, assign) uint64_t fileID;   // the inode
@property(nonatomic, assign) uint64_t size;
@property(nonatomic, assign) BOOL regular;
@end
// nil when nothing is at the path.
MCDiskStamp *MCStampOfFile(NSString *path);

// How a file's text is written: FolderSearch::TextEncoding, value for value.
typedef NS_ENUM(NSInteger, MCTextEncoding) {
    MCTextEncodingUtf8,
    MCTextEncodingUtf16LE,
    MCTextEncodingUtf16BE,
};

@interface MCDiskText : NSObject
// nil: missing, unreadable, too large, not UTF-8, or UTF-16 that does not
// decode exactly (a lone surrogate, an odd byte)
@property(nonatomic, copy)   NSString *text;
// The file starts with a byte order mark: UTF-8's, or UTF-16's (every
// UTF-16 file read as text has one; without it, it is not text here).
@property(nonatomic, assign) BOOL bom;
@property(nonatomic, assign) MCTextEncoding encoding;
@property(nonatomic, assign) BOOL tooLarge;     // over the limit asked for: not read
// The file as it was before the read (nil when missing): a writer that
// finished during the read shows as a change at the next look, never as
// the text read.
@property(nonatomic, strong) MCDiskStamp *stamp;
@end
// Read a file as text, UTF-8 or UTF-16 with its byte order mark
// (FolderSearch::decodeFileText), the mark kept apart, stat'ed before
// reading (FolderSearch::readFileStable). Safe off the main thread.
MCDiskText *MCReadDiskText(NSString *path, unsigned long long maxBytes);
#ifdef __cplusplus
#include <string>
// A tab's text as the bytes of its file, in its encoding with its mark
// (FolderSearch::encodeFileText): what a save writes. NO, with *why, when
// the text holds what UTF-8 cannot carry (a lone surrogate).
BOOL MCEncodeFileText(NSString *text, MCTextEncoding encoding, BOOL bom, std::string *bytes,
                      std::string *why);
#endif

// ---------------------------------------------------------------------------
// MCTab: one open document of a window, and everything that belongs to it
// rather than to the window: the file, its text and mode, whether it has
// unsaved changes, its own undo stack and LaTeX preview, and where its view
// was. The window has one text view (and one image, PDF, player and diff
// view), which shows the active tab; EditorController's document properties
// (currentPath, dirty, isMarkdown, ...) read and write the active tab, so
// with no tab open they read as nothing.
// ---------------------------------------------------------------------------
@interface MCTab : NSObject
@property(nonatomic, copy)   NSString *currentPath;   // nil for a diff or commit
@property(nonatomic, assign) BOOL loaded;             // read from disk yet
@property(nonatomic, copy)   NSString *sourceText;
// The text as it is on disk: as read, or as last saved. `dirty` means the
// buffer differs from it, so typing a letter and deleting it again, or
// undoing back to it, leaves nothing to save.
@property(nonatomic, copy)   NSString *savedText;
@property(nonatomic, copy)   NSString *currentExt;
@property(nonatomic, assign) BOOL isMarkdown, isLatex, previewMode, dirty;
@property(nonatomic, strong) NSDate *fileModDate;
// The file's inode when last read or saved, to follow a rename made outside.
@property(nonatomic, assign) uint64_t fileID;
// Its size then, or -1 when not known (a tab restored from a backup): with
// the date and inode, what tells a change made outside (a date alone misses
// a write in the same tick on SMB or exFAT).
@property(nonatomic, assign) int64_t fileSize;
// The file starts with a byte order mark, kept out of the text and
// written back on save (Excel's "CSV UTF-8" files have one), and how it is
// written: UTF-8, or UTF-16 in either order (always with its mark). Saves
// write it back the same way, so an unedited file stays byte for byte.
@property(nonatomic, assign) BOOL bom;
@property(nonatomic, assign) MCTextEncoding encoding;
// A read of the file in the background (a clean tab following its file):
// a newer one, or an edit meanwhile, drops the older result.
@property(nonatomic, assign) NSUInteger diskReadGeneration;
// The file changed on disk while too large to follow live: said once, and
// read again when the tab is shown or the window comes forward.
@property(nonatomic, assign) BOOL tooLargeToFollow;
// Over the large-file thresholds when it was read (LargeFile.h: 20 MB or
// 300,000 lines), and whether the user turned its features on anyway from
// the status bar (EditorController+LargeFile.mm).
@property(nonatomic, assign) BOOL largeFile, largeFileFeatures;
// The file is no longer at its path (deleted, or moved where it could not
// be followed): the tab says "(deleted)", counts as unsaved, and Cmd+S asks
// before writing it again.
@property(nonatomic, assign) BOOL gone;
// Saved sorted (keep-sorted-on-save) while in the background: the sort is
// put into the buffer as an undoable edit when the tab is next shown, so its
// undo history still matches the text.
@property(nonatomic, copy)   NSString *pendingSort;
@property(nonatomic, strong) LatexView *latex;        // its own, so its undo and page stay
@property(nonatomic, assign) BOOL isImage, isPDF, isMedia;
// A viewer of FileViewers.h (Quick Look and the like), or MCViewerNone.
@property(nonatomic, assign) MCViewerKind viewerKind;
// A viewer over the text (a CSV table) whose text is on show: the text
// when it was put on show. Going back to the viewer turns what was typed
// since into one undo step from it (togglePreview:).
@property(nonatomic, copy)   NSString *textSessionStart;
@property(nonatomic, assign) NSSize imagePixels, mediaPixels;
@property(nonatomic, assign) double mediaSeconds;
@property(nonatomic, assign) BOOL showingMessage;
@property(nonatomic, copy)   NSString *message;       // what the slot says instead
@property(nonatomic, assign) BOOL isDiff, diffIsCommit;
@property(nonatomic, copy)   NSString *diffName;
@property(nonatomic, strong) NSData *diffData;
// A side-by-side diff (isDiff too): what it compares (CompareView.h).
@property(nonatomic, strong) MCComparison *comparison;
// A page of its own in the editor's slot (isDiff too): the Evidence view, a
// worktree's changes (EditorPage.h).
@property(nonatomic, strong) id<MCEditorPage> page;
// The window's agent preview tab: what `minicode open --note` and the like
// show, replaced by the next such open; an edit or a double-click on its
// title keeps it as an ordinary tab.
@property(nonatomic, assign) BOOL agentPreview;
@property(nonatomic, strong) NSUndoManager *undoManager;
@property(nonatomic, assign) uint64_t lastActive;     // for the tab shown after a close
// Where the view was when another tab took over.
@property(nonatomic, assign) BOOL haveView;
@property(nonatomic, assign) NSRange selection;
@property(nonatomic, assign) NSUInteger topCharacter; // first character in view
@property(nonatomic, assign) CGFloat topOffset;       // its distance below the pane's top
@property(nonatomic, assign) NSPoint diffScroll;
@property(nonatomic, assign) NSUInteger pdfPage;
@property(nonatomic, assign) NSPoint pdfPoint;
@property(nonatomic, assign) CGFloat pdfScale;
@property(nonatomic, assign) BOOL pdfAutoScales;
@property(nonatomic, assign) double mediaTime;
// The preview's memory of the source view (see togglePreview:).
@property(nonatomic, assign) NSRange sourceSelection;
@property(nonatomic, assign) CGFloat sourceCaretOffset;
@property(nonatomic, assign) BOOL haveSourcePosition;
@property(nonatomic, assign) NSPoint previewEntryScroll;
// A Markdown tab's page beside its source (Open Preview to the Side, kept
// in the session), and where it was scrolled while another tab showed.
@property(nonatomic, assign) BOOL sidePreview;
@property(nonatomic, assign) CGFloat sidePreviewY;
// Its closed folds (FoldView.h), as UTF-16 spans of the source, while
// another tab is shown; forgotten when the text is read again from disk.
@property(nonatomic, copy)   NSArray<NSValue *> *folds;
// Its backup (BackupStore): the id, the text and path last written (so an
// unchanged buffer is not written again), whether one is on disk, and
// whether the buffer came back from one and waits to be shown over the
// disk's text (loadActiveTab).
@property(nonatomic, copy)   NSString *backupID;
@property(nonatomic, strong) NSString *backedUpText;
@property(nonatomic, copy)   NSString *backedUpPath;
@property(nonatomic, assign) BOOL hasBackup, restoredBackup;
@end
