// Search.h — Find in Folder, Replace in Folder and the TODO list, one window
// per kind per editor window. Objective-C++.
//
// The search is FolderSearch's (src/FolderSearch.h), with VS Code's three
// options: Match Case, Whole Word and Regular Expression (the toggles beside
// the query, or Option+Cmd+C / W / R while the window has the keyboard, as
// the Find Options menu items reach it as the window's delegate).
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

// For tests.
@property(nonatomic, readonly) NSTextField *field;
@property(nonatomic, readonly) NSTextField *replaceField;
@property(nonatomic, readonly) NSTextField *status;
@property(nonatomic, readonly) NSTableView *table;
@property(nonatomic) BOOL matchCase, wholeWord, regex;
- (void)runSearch:(id)sender;
- (void)replaceAll:(id)sender;
- (void)replaceMatchAtRow:(NSInteger)row;
- (void)replaceFileAtRow:(NSInteger)row;
@property(nonatomic, readonly) BOOL busy;   // a search or a replace is running
@end
