// EditorTab.h — MCTab, one open document of an editor window.
#import <Cocoa/Cocoa.h>
#import "FileViewers.h"

@class LatexView, MCComparison, MCTab, MCSessionDoc, MCSessionLinkRef, CodeTextView;
@protocol MCEditorPage;

// The window a tab is open in keeps its place among the tabs (the core's
// Model::Window, EditorTabs.h): its tab id, whether it is a preview tab,
// and when it last came on show. The tab asks it.
@protocol MCTabOwner
- (BOOL)tabIsPreview:(MCTab *)t;
- (BOOL)tabIsAgentPreview:(MCTab *)t;
- (uint64_t)tabLastUsed:(MCTab *)t;
- (void)tabPathChanged:(MCTab *)t;
@optional
// The session's host (SessionLink.h): the text view whose storage holds the
// tab's source as an editable text now, else nil; and the tab's text
// changed by the host while not in the storage.
- (CodeTextView *)textViewShowingSourceOf:(MCTab *)t;
- (void)tabTextChangedByHost:(MCTab *)t;
// The link its document goes on: its window's (stage 6).
- (MCSessionLinkRef *)sessionLinkForTab:(MCTab *)t;
@end

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
    MCTextEncodingWindows1252,   // not UTF-8, no NUL: FolderSearch::readsAsWindows1252
};

@interface MCDiskText : NSObject
// nil: missing, unreadable, too large, binary (not UTF-8 and a NUL in
// it), or UTF-16 that does not decode exactly (a lone surrogate, an odd
// byte). Text not in UTF-8 is read as Windows-1252.
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
// Read a file as text, UTF-8, UTF-16 with its byte order mark, or
// Windows-1252 (FolderSearch::decodeFileText), the mark kept apart, stat'ed before
// reading (FolderSearch::readFileStable). Safe off the main thread.
MCDiskText *MCReadDiskText(NSString *path, unsigned long long maxBytes);
#ifdef __cplusplus
#include <string>
// A tab's text as the bytes of its file, in its encoding with its mark
// (FolderSearch::encodeFileText): what a save writes. NO, with *why, when
// the text holds what UTF-8 cannot carry (a lone surrogate).
BOOL MCEncodeFileText(NSString *text, MCTextEncoding encoding, BOOL bom, std::string *bytes,
                      std::string *why);
// A text's UTF-8 a block at a time, straight into `out` (no NSData of all
// of it on the way). NO for a lone surrogate.
BOOL MCUtf8Bytes(NSString *text, std::string *out);
#endif
// The encoding a save writes `text` in: `encoding`, unless it is
// Windows-1252 and the text holds a character Windows-1252 cannot store
// (FolderSearch::canEncode); then UTF-8, and *unheld that character. The
// save says so (writeText:toTab:interactive:) rather than asking.
MCTextEncoding MCEncodingToWrite(NSString *text, MCTextEncoding encoding, NSString **unheld);
// What the status bar names it: "Windows-1252", "UTF-16 LE", "UTF-8 with
// BOM" (FolderSearch::encodingLabel); nil for plain UTF-8, which it leaves
// unnamed.
NSString *MCEncodingStatusLabel(MCTextEncoding encoding, BOOL bom);

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
// Its id in its window's Model::Window, given when it is made, never reused.
@property(nonatomic, readonly) uint64_t docId;
// The window it is open in, while it is (set and cleared by the window).
@property(nonatomic, weak) id<MCTabOwner> owner;
@property(nonatomic, copy)   NSString *currentPath;   // nil for a diff or commit
@property(nonatomic, assign) BOOL loaded;             // read from disk yet
// The buffer's text, immutable. While the tab on show is edited it is the
// editor's storage's (followStorage:, from textDidChange:) and is copied
// from it only when read: a copy of the storage's CFString is a memcpy
// while it is 8-bit and a scan of every character once a non-ASCII one
// has been in it, even undone (1.3 to 3 ms a key at 2.6 MB), and a copy a
// keystroke was most of a keystroke's cost. Each read after a change
// gives a new object and later reads the same one until the next change,
// so a pointer compare still tells whether the text changed.
@property(nonatomic, copy)   NSString *sourceText;
// The storage holds this tab's text, changed since sourceText was last
// read or set. Kept until the text is read, set, or the storage is about
// to be replaced whole (MCSourceStorage willReplaceAllCharacters), which
// copies it first.
- (void)followStorage:(NSTextStorage *)storage;
@property(nonatomic, readonly) BOOL followsStorage;
// The text without a copy: the storage's own (mutable) string while the
// tab follows it, else sourceText. For a look on the spot (a length, a
// compare), never to keep or hand to another thread.
@property(nonatomic, readonly) NSString *currentText;
// sourceText is not nil, asked without a copy.
@property(nonatomic, readonly) BOOL hasSourceText;
// Counts changes of the text: each set to another object and each
// followStorage:. Never 0.
@property(nonatomic, readonly) uint64_t textGeneration;
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
// One of the window's two agent preview tabs: what `mutible open --note`
// shows (Model::Preview::Agent), or a diff (Model::Preview::AgentDiff),
// replaced by the next such open of its kind; an edit or a double-click on
// its title keeps it as an ordinary tab. Its window's; NO for a tab in no
// window.
@property(nonatomic, readonly) BOOL agentPreview;
// Its group's preview tab (workbench.enable-preview): a file opened by a
// single click in the tree, a search result or a code navigation, replaced
// by the next such open; an edit, a save, a double-click on its title or on
// the file in the tree, or Keep Open makes it an ordinary tab. Kept apart
// from agentPreview on purpose (EditorTabs.h): neither replaces the other.
// Its window's (Model::Preview::User).
@property(nonatomic, readonly) BOOL preview;
@property(nonatomic, strong) NSUndoManager *undoManager;
// Its document on the session's host (SessionLink.h): made when its text is
// first set while editor.session-host is on, nil otherwise. The host then
// owns its text's history; every change of sourceText not made by the host
// goes to it as an edit, and the editor's storage edits too while the tab
// is on show (EditorController+Session.mm).
@property(nonatomic, readonly) MCSessionDoc *session;
// sourceText set to the text of the storage that showed it (the window
// putting the tab away): the host has had every edit of that storage, so
// nothing goes to it. The tab's own copy may be older (a storage replaced
// whole outside the text view leaves the tab its text before), and its
// difference sent again would be applied twice. Without a document on the
// host, the same as setting sourceText.
- (void)takeTextFromStorage:(NSString *)text;
// A document the session's host holds (a window opening takes what a
// closed one left, EditorController+Session.mm): the tab's document is that
// one, and its text the host's, set without telling the host anything. NO
// when the host did not answer. heldFromHost stays set until the tab is
// first shown (loadActiveTab reads the file then, keeping this text).
- (BOOL)attachHeld:(uint64_t)docId link:(MCSessionLinkRef *)link;
@property(nonatomic, assign) BOOL heldFromHost;
// A buffer with no file yet: the session's host held it without a path
// (another renderer's untitled document), and the tab shows it under a name
// of its own in the folder (Backups::untitledPath: Untitled-1, ...), a file
// not on disk, unsaved but not "(deleted)", and left alone by auto-save.
// The host is told the path (DocPath) once Cmd+S has written the file
// there, which clears this.
@property(nonatomic, assign) BOOL untitled;
// When it last came on show in its window (Model::Window::lastUsed), larger
// for later: the tab shown after a close, the files Quick Open lists first.
@property(nonatomic, readonly) uint64_t lastActive;
// Where the view was when another tab took over.
@property(nonatomic, assign) BOOL haveView;
@property(nonatomic, assign) NSRange selection;
@property(nonatomic, assign) NSUInteger topCharacter; // first character in view
@property(nonatomic, assign) CGFloat topOffset;       // its distance below the pane's top
// Brought back with its window (EditorController+Persistence.mm) and not
// shown since: the place above is the last run's, put back when the file is
// first read, with its page (YES) or its source on show.
@property(nonatomic, assign) BOOL placeFromSession;
@property(nonatomic, assign) BOOL pageFromSession;
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
// A Markdown page's <details> blocks opened or closed by a click, by the
// source line of each one's tag (MarkdownParser::fold); the others are as
// their tags say. Kept while the tab is open.
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, NSNumber *> *detailsOpen;
// Its closed folds (FoldView.h), as UTF-16 spans of the source, while
// another tab is shown; forgotten when the text is read again from disk.
@property(nonatomic, copy)   NSArray<NSValue *> *folds;
// Its backup (BackupStore): the id, the textGeneration and path last
// written (so an unchanged buffer is not written again; 0 for none),
// whether one is on disk, and whether the buffer came back from one and
// waits to be shown over the disk's text (loadActiveTab).
@property(nonatomic, copy)   NSString *backupID;
@property(nonatomic, assign) uint64_t backedUpGeneration;
@property(nonatomic, copy)   NSString *backedUpPath;
@property(nonatomic, assign) BOOL hasBackup, restoredBackup;
@end
