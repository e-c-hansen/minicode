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

NS_ASSUME_NONNULL_END
