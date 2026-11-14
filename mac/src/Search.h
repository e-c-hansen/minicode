// Search.h — Find in Folder, Replace in Folder and the TODO list, one window
// per kind per editor window. Objective-C++.
//
// The window is laid out as VS Code's search view (Search.mm has the
// measurements): the query with Match Case, Whole Word and Regular
// Expression inside it (or Option+Cmd+C / W / R while the window has the
// keyboard, as the Find Options menu items reach it as the window's
// delegate); the replace row under it, shown by the chevron down the left
// edge, with Preserve Case inside (Option+Cmd+P) and Replace All beside it;
// "files to include" and "files to exclude" behind the "..." button, read as
// VS Code reads them (FolderSearch::PathFilter); and the results as a tree,
// a row per file with its match count over a row per line with the match
// highlighted (struck through and followed by its replacement while the
// replace row shows). A row's buttons replace it or dismiss it. Find in
// Folder on a folder inside the window's puts "./that/folder" in files to
// include, as VS Code does; a folder outside it is searched instead.
//
// The search is FolderSearch's (src/FolderSearch.h).
//
// Replace in Folder (the replace field under the query): Replace All counts
// every match first (FolderSearch::planReplace) and asks "Replace 42
// matches in 7 files?"; a result's right-click menu (or the buttons) replaces
// only that match or only that file. Files open in a tab of the editor
// window are changed in the tab's buffer, unsaved and undoable, as rename
// changes them; other files are written to disk atomically (a temporary
// renamed over the file, its permissions kept). A file whose tab holds
// changes the disk does not have is left alone and named. The results are
// searched again afterwards.
#import <Cocoa/Cocoa.h>

// How a file is open in the editor windows, for Replace in Folder.
typedef NS_ENUM(NSInteger, MCOpenFileState) {
    MCOpenFileNone,       // in no tab, or in tabs that hold the disk's text in other windows
    MCOpenFileHere,       // in a tab of this window holding the disk's text: edit the tab
    MCOpenFileConflict,   // a tab holds text the disk does not have: leave it alone
};

@interface SearchPanel : NSObject
- (instancetype)initWithRoot:(NSString *)root
                 openHandler:(void (^)(NSString *path, NSInteger line))handler;
// The TODO list: every TODO comment and open Markdown task under the folder
// (FolderSearch::findTodos), filtered by what is typed.
- (instancetype)initTodosWithRoot:(NSString *)root
                      openHandler:(void (^)(NSString *path, NSInteger line))handler;
- (void)show;
// Show with the keyboard in the replace field (Replace in Folder).
- (void)showReplace;
// Its window, which belongs to the editor window that made it: keys pressed
// in it are that editor window's menu targets.
@property(nonatomic, strong, readonly) NSWindow *window;
- (void)setScope:(NSString *)dir;   // point search at a specific folder
// A single click on a result line: shows it in the editor's preview tab,
// leaving the keyboard in this window, as VS Code's search view does (a
// double-click or Return goes to the open handler, which keeps the tab).
// Unset, a single click does nothing.
@property(nonatomic, copy) void (^previewHandler)(NSString *path, NSInteger line);

// Replace in Folder's way into the editor's tabs (set by EditorController).
@property(nonatomic, copy) MCOpenFileState (^openState)(NSString *path);
// Edit tabs of this window: paths[i] gets ranges[i] (UTF-16, ascending)
// replaced by strings[i], every file or none. Files changed, or -1.
@property(nonatomic, copy) NSInteger (^editTabs)(NSArray<NSString *> *paths,
                                                 NSArray<NSArray<NSValue *> *> *ranges,
                                                 NSArray<NSArray<NSString *> *> *strings,
                                                 NSString **failure);
// The text a tab of this window holds for a path (MCOpenFileHere files).
@property(nonatomic, copy) NSString *(^tabText)(NSString *path);

// The query, from the editor's selection (Find in Folder with text
// selected); searched at once.
- (void)seedQuery:(NSString *)query;

// For tests.
@property(nonatomic, readonly) NSTextField *field;
@property(nonatomic, readonly) NSTextField *replaceField;
@property(nonatomic, readonly) NSTextField *includeField;
@property(nonatomic, readonly) NSTextField *excludeField;
@property(nonatomic, readonly) NSTextField *status;
// Under the results: the files the search left out (larger than 1 MB, or
// not UTF-8), named in its tooltip. Hidden when there were none.
@property(nonatomic, readonly) NSTextField *skippedNote;
@property(nonatomic, readonly) NSTableView *table;   // an NSOutlineView: files, then their lines
@property(nonatomic, readonly) BOOL replaceShown, detailsShown;
@property(nonatomic) BOOL matchCase, wholeWord, regex, preserveCase;
- (void)runSearch:(id)sender;
- (void)replaceAll:(id)sender;
// `row` indexes the results' lines (the `hits` key), not the tree's rows.
- (void)replaceMatchAtRow:(NSInteger)row;
- (void)replaceFileAtRow:(NSInteger)row;
@property(nonatomic, readonly) BOOL busy;   // a search or a replace is running
@end
