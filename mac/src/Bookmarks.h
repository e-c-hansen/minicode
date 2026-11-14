// Bookmarks.h — web bookmarks as files on the Mac (the rules are the core's
// WebBookmark.h): where the bookmark folder is, reading a .url or .webloc
// file's address, and writing a new .url file. A bookmark opened anywhere
// (the tree, Finder, `minicode file.url`) loads in a browser tab, and
// `minicode https://...` opens the window on the bookmark folder, the
// browser window, with the page in a tab (EditorController+Bookmarks.mm).
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// The bookmark folder: `browser.bookmarks` with "~" expanded (a relative
// path is from the home folder), else ~/Bookmarks. Not made by this.
NSString *MCBookmarksFolder(void);

// The folder made if it is missing (with its parents). NO, with the
// reason in *error, when it cannot be, or something else has its name.
BOOL MCEnsureBookmarksFolder(NSString *folder, NSString *_Nullable *_Nullable error);

// Whether `path` names a bookmark file by its extension (.url, .webloc).
BOOL MCIsBookmarkFile(NSString *path);

// The http or https address a bookmark file holds (a binary .webloc read
// with Foundation), or nil when it holds none or cannot be read. Files
// over 1 MB are no bookmarks.
NSString *_Nullable MCBookmarkAddress(NSString *path);

// Whether `url` is an address a bookmark or the command line may open
// (WebBookmark::isWebAddress: http or https only).
BOOL MCIsWebAddress(NSString *url);

// Writes a .url file for `url` in `folder`, named for `title` (a number
// added when the name is taken; never over an existing file) and returns
// its path, or nil with the reason in *error.
NSString *_Nullable MCWriteBookmark(NSString *folder, NSString *_Nullable title, NSString *url,
                                    NSString *_Nullable *_Nullable error);

// A bookmark file in `folder` or below it whose address is `url`'s page
// (WebBookmark::sameAddress), or nil. Hidden files and folders are passed
// by, symlinked folders are not followed, and the walk stops after 10,000
// entries. A file's address is read again only when its size or date has
// changed, so asking again is mostly stat calls. Any thread.
NSString *_Nullable MCBookmarkFileFor(NSString *folder, NSString *url);

// The bookmark folder as a menu (the browser's toolbar and the menu bar's
// Bookmarks menu): each folder a submenu, folders first, then the bookmarks
// by name, filled as each opens, so it follows the folder. A bookmark's
// item sends openBookmarkFromMenu: and a folder's last item, Open All in
// Tabs, openBookmarkFolderFromMenu:, each with the path as its
// representedObject, to `target` (nil: the responder chain). `footer`, if
// given, adds items after the top level's own. `folder` nil is
// MCBookmarksFolder() each time the menu opens.
@class NSMenu;
void MCAttachBookmarksMenu(NSMenu *menu, NSString *_Nullable folder, id _Nullable target,
                           void (^_Nullable footer)(NSMenu *menu));

@interface NSObject (MCBookmarkMenuActions)
- (void)openBookmarkFromMenu:(id)sender;
- (void)openBookmarkFolderFromMenu:(id)sender;
@end

// The addresses of the bookmarks directly in `dir`, in Finder's name order.
NSArray<NSString *> *MCBookmarkAddressesIn(NSString *dir);

NS_ASSUME_NONNULL_END
