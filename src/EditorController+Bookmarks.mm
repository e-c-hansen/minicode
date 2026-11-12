// EditorController+Bookmarks.mm — the browser's bookmarks (Bookmarks.h,
// WebBookmark.h): a .url or .webloc file opened in this window loads in
// the browser pane; Save Page as Bookmark (Cmd+D in the page) and Save All
// Tabs as Bookmarks write .url files; the tree's Open All in Browser Tabs
// opens a folder of them. The window on the bookmark folder is the browser
// window, where the page takes the editor's side while no file is open.
#import "EditorController+Private.h"
#import "Bookmarks.h"
#import "Browser.h"

#include "WebBookmark.h"

// Open as Text: while set, a bookmark file opens as its text, so a wrong
// address can be mended. Main thread only, for one synchronous open.
static BOOL gOpenBookmarkAsText = NO;

@implementation EditorController (Bookmarks)

- (BOOL)isBrowserWindow {
    NSString *root = self.rootPath.stringByResolvingSymlinksInPath;
    return root.length &&
           [root isEqualToString:MCBookmarksFolder().stringByResolvingSymlinksInPath];
}

// ---------------------------------------------------------------- opening
- (BOOL)openBookmarkAtPath:(NSString *)path focusPage:(BOOL)focus {
    if (gOpenBookmarkAsText || !MCIsBookmarkFile(path)) return NO;
    NSString *url = MCBookmarkAddress(path);
    if (!url) return NO;   // no address it may open: shown as text instead
    [self showWebAddresses:@[url] focusPage:focus];
    return YES;
}

- (void)showWebAddresses:(NSArray<NSString *> *)urls focusPage:(BOOL)focus {
    std::vector<std::string> in;
    for (NSString *u in urls) in.push_back(u.UTF8String ?: "");
    NSMutableArray<NSString *> *addresses = [NSMutableArray array];
    for (const std::string &u : WebBookmark::tabAddresses(in))
        [addresses addObject:[NSString stringWithUTF8String:u.c_str()]];
    if (!addresses.count) return;
    [self keepKeyboardDuring:^{
        [self makeBrowserWithHome:@""];
        [self leaveMaximizedPaneFor:MCPaneBrowser];
        self.browserVisible = YES;
        // In the browser window the page has the whole right side until a
        // file is opened beside it.
        if (self.isBrowserWindow && !self.currentPath) self.editorHidden = YES;
        [self showRightArea];
        [self relayoutRightArea];
        [self.browser openAddresses:addresses];
    }];
    if (self.hintsVisible) [self updateHints];
    NSResponder *fr = self.window.firstResponder;
    const BOOL keyboardHidden = [fr isKindOfClass:NSView.class] &&
                                ((NSView *)fr).isHiddenOrHasHiddenAncestor;
    if (focus || keyboardHidden) [self.browser focusPage];
}

// ---------------------------------------------------------------- saving
// Where a bookmark is saved: in the browser window the tree's selected
// folder (a selected file's folder), else the bookmark folder itself, made
// if missing. nil, with a note in the status bar, when it cannot be made.
- (NSString *)bookmarkFolderForSaving {
    if (self.isBrowserWindow) return [self targetDirectory];
    NSString *folder = MCBookmarksFolder();
    NSString *err = nil;
    if (MCEnsureBookmarksFolder(folder, &err)) return folder;
    [self flashStatus:[NSString stringWithFormat:@"No bookmark folder: %@", err]];
    NSBeep();
    return nil;
}

// "~/Bookmarks" for a folder in the home folder.
static NSString *MCShortFolder(NSString *path) {
    return path.stringByAbbreviatingWithTildeInPath;
}

// A saved bookmark (or folder of them) selected in the tree when this
// window's folder holds it.
- (void)revealSavedBookmark:(NSString *)path {
    NSString *root = self.rootPath.stringByResolvingSymlinksInPath;
    NSString *real = path.stringByResolvingSymlinksInPath;
    if (![real hasPrefix:[root stringByAppendingString:@"/"]]) return;
    [self refreshTree:nil];
    [self revealPath:[self.rootPath stringByAppendingPathComponent:
                                        [real substringFromIndex:root.length + 1]]
             andOpen:NO];
}

- (BOOL)canSaveBrowserBookmark {
    return self.browser && self.browserVisible && MCIsWebAddress(self.browser.pageURL ?: @"");
}

- (void)saveBrowserBookmark:(id)sender {
    (void)sender;
    if (!self.canSaveBrowserBookmark) {
        NSBeep();
        return;
    }
    NSString *folder = [self bookmarkFolderForSaving];
    if (!folder) return;
    NSString *err = nil;
    NSString *path = MCWriteBookmark(folder, self.browser.pageTitle, self.browser.pageURL, &err);
    if (!path) {
        [self flashStatus:[NSString stringWithFormat:@"The bookmark was not saved: %@", err]];
        NSBeep();
        return;
    }
    [self flashStatus:[NSString stringWithFormat:@"Saved “%@” in %@",
                                                 path.lastPathComponent, MCShortFolder(folder)]];
    [self revealSavedBookmark:path];
}

- (BOOL)canSaveBrowserTabs {
    if (!self.browser) return NO;
    for (NSString *u in self.browser.tabAddresses)
        if (MCIsWebAddress(u)) return YES;
    return NO;
}

- (void)saveBrowserTabsAsBookmarks:(id)sender {
    (void)sender;
    if (!self.canSaveBrowserTabs) {
        NSBeep();
        return;
    }
    NSString *parent = [self bookmarkFolderForSaving];
    if (!parent) return;
    NSDateFormatter *f = [NSDateFormatter new];
    f.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    f.dateFormat = @"yyyy-MM-dd HH.mm";
    const std::string base = WebBookmark::tabsFolderName([f stringFromDate:[NSDate date]].UTF8String);
    NSFileManager *fm = [NSFileManager defaultManager];
    const std::string name = WebBookmark::uniqueName(base, [&](const std::string &n) {
        return [fm fileExistsAtPath:[parent stringByAppendingPathComponent:
                                                @(n.c_str())]] == YES;
    });
    NSString *folder = [parent stringByAppendingPathComponent:@(name.c_str())];
    NSError *mkErr = nil;
    if (![fm createDirectoryAtPath:folder withIntermediateDirectories:NO attributes:nil
                             error:&mkErr]) {
        [self flashStatus:[NSString stringWithFormat:@"The tabs were not saved: %@",
                                                     mkErr.localizedDescription]];
        NSBeep();
        return;
    }
    NSArray<NSString *> *addresses = self.browser.tabAddresses;
    NSArray<NSString *> *titles = self.browser.tabPageTitles;
    NSUInteger saved = 0;
    NSString *firstError = nil;
    for (NSUInteger i = 0; i < addresses.count; i++) {
        if (!MCIsWebAddress(addresses[i])) continue;
        NSString *err = nil;
        if (MCWriteBookmark(folder, i < titles.count ? titles[i] : nil, addresses[i], &err))
            saved++;
        else if (!firstError)
            firstError = err;
    }
    NSString *note = [NSString stringWithFormat:@"Saved %lu %@ in “%@”", (unsigned long)saved,
                                                saved == 1 ? @"tab" : @"tabs", MCShortFolder(folder)];
    if (firstError) note = [note stringByAppendingFormat:@" (not all: %@)", firstError];
    [self flashStatus:note];
    [self revealSavedBookmark:folder];
}

// ------------------------------------------------- the tree's own items
// The tree's clicked (else selected) folder, or nil.
- (NSString *)bookmarkFolderInTree {
    FileItem *node = [self clickedOrSelectedItem];
    return node && node.isDir ? node.path : nil;
}

// The tree's clicked (else selected) bookmark file, or nil.
- (NSString *)bookmarkFileInTree {
    FileItem *node = [self clickedOrSelectedItem];
    return node && !node.isDir && MCIsBookmarkFile(node.path) ? node.path : nil;
}

// Only a folder with a bookmark directly in it (by name; read when opened).
- (BOOL)canOpenBookmarksInTabs {
    NSString *dir = [self bookmarkFolderInTree];
    if (!dir) return NO;
    for (NSString *n in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil])
        if (![n hasPrefix:@"."] && MCIsBookmarkFile(n)) return YES;
    return NO;
}
- (BOOL)canOpenBookmarkAsText { return [self bookmarkFileInTree] != nil; }

// The addresses of the bookmarks directly in `dir`, in Finder's name order.
static NSArray<NSString *> *MCBookmarkAddressesIn(NSString *dir) {
    NSArray<NSString *> *names =
        [[[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil]
            sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    for (NSString *n in names) {
        if ([n hasPrefix:@"."]) continue;
        NSString *u = MCBookmarkAddress([dir stringByAppendingPathComponent:n]);
        if (u) [out addObject:u];
    }
    return out;
}

- (void)openBookmarksInTabs:(id)sender {
    (void)sender;
    NSString *dir = [self bookmarkFolderInTree];
    NSArray<NSString *> *urls = dir ? MCBookmarkAddressesIn(dir) : @[];
    if (!urls.count) {
        if (dir) [self flashStatus:[NSString stringWithFormat:@"No bookmarks in “%@”",
                                                              dir.lastPathComponent]];
        NSBeep();
        return;
    }
    [self showWebAddresses:urls focusPage:YES];
}

- (void)openBookmarkAsText:(id)sender {
    (void)sender;
    NSString *path = [self bookmarkFileInTree];
    if (!path) {
        NSBeep();
        return;
    }
    gOpenBookmarkAsText = YES;
    [self openFileAtPath:path];
    gOpenBookmarkAsText = NO;
    [self focusEditor:nil];
}

@end
