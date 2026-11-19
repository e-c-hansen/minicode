// Bookmarks.mm — see Bookmarks.h.
#import "Bookmarks.h"

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#import "AppSettings.h"
#include "WebBookmark.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static std::string Utf8(NSString *s) { return s.UTF8String ?: ""; }

NSString *MCBookmarksFolder(void) {
    const std::string set = [AppSettings shared].settings.browserBookmarks();
    if (!set.empty()) {
        NSString *path = [NSString stringWithUTF8String:set.c_str()].stringByExpandingTildeInPath;
        // A relative path is from the home folder (a Finder launch's working
        // folder is /).
        if (path.length && !path.absolutePath)
            path = [NSHomeDirectory() stringByAppendingPathComponent:path];
        if (path.length) return path.stringByStandardizingPath;
    }
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Bookmarks"];
}

BOOL MCEnsureBookmarksFolder(NSString *folder, NSString **error) {
    NSFileManager *fm = [NSFileManager defaultManager];
    BOOL dir = NO;
    if ([fm fileExistsAtPath:folder isDirectory:&dir]) {
        if (dir) return YES;
        if (error) *error = [NSString stringWithFormat:@"%@ is a file, not a folder", folder];
        return NO;
    }
    NSError *err = nil;
    if ([fm createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil
                            error:&err])
        return YES;
    if (error) *error = err.localizedDescription ?: @"the folder could not be made";
    return NO;
}

BOOL MCIsBookmarkFile(NSString *path) {
    return path.length && WebBookmark::isBookmarkPath(Utf8(path));
}

BOOL MCIsWebAddress(NSString *url) { return url.length && WebBookmark::isWebAddress(Utf8(url)); }

NSString *MCBookmarkAddress(NSString *path) {
    if (!MCIsBookmarkFile(path)) return nil;
    // A plain file (through a symlink too), and not a large one: a FIFO
    // would block the read for good.
    struct stat st;
    if (stat(path.fileSystemRepresentation, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size > 1024 * 1024)
        return nil;
    NSData *data = [NSData dataWithContentsOfFile:path];
    if (!data) return nil;
    const std::string bytes((const char *)data.bytes, data.length);
    std::string url = WebBookmark::addressIn(Utf8(path), bytes);
    if (url.empty() && [path.pathExtension.lowercaseString isEqualToString:@"webloc"]) {
        // A binary property list, as Safari writes them.
        id plist = [NSPropertyListSerialization propertyListWithData:data options:0 format:nil
                                                               error:nil];
        id u = [plist isKindOfClass:NSDictionary.class] ? plist[@"URL"] : nil;
        if ([u isKindOfClass:NSString.class] && MCIsWebAddress(u)) url = Utf8(u);
    }
    return url.empty() ? nil : [NSString stringWithUTF8String:url.c_str()];
}

NSString *MCWriteBookmark(NSString *folder, NSString *title, NSString *url, NSString **error) {
    if (!MCIsWebAddress(url)) {
        if (error) *error = @"only http and https pages can be bookmarked";
        return nil;
    }
    const std::string dir = Utf8(folder);
    const std::string text = WebBookmark::shortcutText(Utf8(url));
    std::string name = WebBookmark::fileName(Utf8(title ?: @""), Utf8(url));
    auto taken = [&dir](const std::string &n) {
        struct stat st;
        return lstat((dir + "/" + n).c_str(), &st) == 0;
    };
    // A name free now may be taken before the create: O_EXCL refuses it,
    // and the next free name is tried.
    for (int attempt = 0; attempt < 5; attempt++) {
        name = WebBookmark::uniqueName(name, taken);
        const std::string path = dir + "/" + name;
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            if (error) *error = [NSString stringWithUTF8String:strerror(errno)];
            return nil;
        }
        size_t done = 0;
        while (done < text.size()) {
            const ssize_t n = write(fd, text.data() + done, text.size() - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            done += (size_t)n;
        }
        const BOOL ok = done == text.size();
        const int savedErrno = errno;
        close(fd);
        if (!ok) {
            unlink(path.c_str());
            if (error) *error = [NSString stringWithUTF8String:strerror(savedErrno)];
            return nil;
        }
        return [NSString stringWithUTF8String:path.c_str()];
    }
    if (error) *error = @"no free name for the bookmark";
    return nil;
}

// ------------------------------------------------- finding a page's bookmark
// path -> @[size, date, address or NSNull], so an unchanged file is not read
// again. NSCache is safe from any thread.
static NSCache<NSString *, NSArray *> *MCAddressCache(void) {
    static NSCache *cache;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        cache = [NSCache new];
        cache.countLimit = 20000;
    });
    return cache;
}

static NSString *MCCachedAddress(NSString *path, const struct stat &st) {
    NSNumber *size = @((long long)st.st_size);
    NSNumber *date = @((double)st.st_mtimespec.tv_sec + st.st_mtimespec.tv_nsec / 1e9);
    NSArray *hit = [MCAddressCache() objectForKey:path];
    if (hit.count == 3 && [hit[0] isEqual:size] && [hit[1] isEqual:date])
        return hit[2] == NSNull.null ? nil : hit[2];
    NSString *url = MCBookmarkAddress(path);
    [MCAddressCache() setObject:@[size, date, url ?: NSNull.null] forKey:path];
    return url;
}

NSString *MCBookmarkFileFor(NSString *folder, NSString *url) {
    if (!folder.length || !MCIsWebAddress(url)) return nil;
    const std::string want = Utf8(url);
    NSDirectoryEnumerator<NSURL *> *e = [[NSFileManager defaultManager]
                   enumeratorAtURL:[NSURL fileURLWithPath:folder]
        includingPropertiesForKeys:@[]
                           options:NSDirectoryEnumerationSkipsHiddenFiles |
                                   NSDirectoryEnumerationSkipsPackageDescendants
                      errorHandler:^BOOL(NSURL *u, NSError *err) { (void)u; (void)err; return YES; }];
    // The enumerator answers with the folder's real path (/private/tmp for
    // /tmp); answer in the folder's own spelling, as the tree shows it.
    char real[PATH_MAX];
    NSString *base = realpath(folder.fileSystemRepresentation, real)
                         ? [[NSFileManager defaultManager]
                               stringWithFileSystemRepresentation:real length:strlen(real)]
                         : nil;
    NSUInteger seen = 0;
    for (NSURL *u in e) {
        if (++seen > 10000) break;
        NSString *path = u.path;
        if (base && [path hasPrefix:[base stringByAppendingString:@"/"]])
            path = [folder stringByAppendingPathComponent:[path substringFromIndex:base.length + 1]];
        if (!MCIsBookmarkFile(path)) continue;
        struct stat st;
        if (stat(path.fileSystemRepresentation, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        NSString *have = MCCachedAddress(path, st);
        if (have && WebBookmark::sameAddress(Utf8(have), want)) return path;
    }
    return nil;
}

NSArray<NSString *> *MCBookmarkAddressesIn(NSString *dir) {
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

// ------------------------------------------------ per-workspace folders
static NSString *Ns(const std::string &s) { return s.empty() ? nil : @(s.c_str()); }

// The window on the bookmark folder is the browser window however its
// folder is spelled (/tmp, /private/tmp, a symlink).
static WebBookmark::Places MCPlaces(NSString *root) {
    NSString *shared = MCBookmarksFolder();
    NSString *r = root.length ? root : nil;
    if (r && [r.stringByResolvingSymlinksInPath
                 isEqualToString:shared.stringByResolvingSymlinksInPath])
        r = shared;
    return WebBookmark::places(Utf8(r ?: @""), Utf8(shared));
}

NSString *MCSaveBookmarksFolder(NSString *root) {
    return Ns(MCPlaces(root).save) ?: MCBookmarksFolder();
}

NSString *MCSharedBookmarksFolder(NSString *root) { return Ns(MCPlaces(root).shared); }

NSString *MCWorkspaceBookmarkFileFor(NSString *root, NSString *url) {
    for (const std::string &dir : WebBookmark::lookupOrder(MCPlaces(root)))
        if (NSString *have = MCBookmarkFileFor(@(dir.c_str()), url)) return have;
    return nil;
}

NSString *MCMoveBookmarkToWorkspace(NSString *file, NSString *root, NSString **error) {
    const WebBookmark::Places places = MCPlaces(root);
    NSString *save = Ns(places.save);
    auto taken = [save](const std::string &n) {
        struct stat st;
        return lstat([save stringByAppendingPathComponent:@(n.c_str())].fileSystemRepresentation,
                     &st) == 0;
    };
    NSString *to = save ? Ns(WebBookmark::moveDestination(places, Utf8(file), taken)) : nil;
    if (!to) {
        if (error) *error = @"it is not one of the shared bookmarks";
        return nil;
    }
    if (!MCEnsureBookmarksFolder(save, error)) return nil;
    NSError *err = nil;
    if (![[NSFileManager defaultManager] moveItemAtPath:file toPath:to error:&err]) {
        if (error) *error = err.localizedDescription ?: @"the file could not be moved";
        return nil;
    }
    return to;
}

// ------------------------------------------------------------ the menu
static const NSUInteger kMaxMenuEntries = 1000;

// `folder`'s entries as the core takes them, in Finder's name order: a
// folder that is no package is a folder, anything else a file.
static std::vector<WebBookmark::Name> MCListNames(NSString *folder) {
    std::vector<WebBookmark::Name> out;
    if (!folder.length) return out;
    NSFileManager *fm = [NSFileManager defaultManager];
    NSArray<NSString *> *names = [[fm contentsOfDirectoryAtPath:folder error:nil]
        sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
    for (NSString *n in names) {
        if ([n hasPrefix:@"."]) continue;
        NSString *path = [folder stringByAppendingPathComponent:n];
        BOOL isDir = NO;
        if (![fm fileExistsAtPath:path isDirectory:&isDir]) continue;
        out.push_back({Utf8(n), isDir && ![[NSWorkspace sharedWorkspace] isFilePackageAtPath:path]});
    }
    return out;
}

static void MCAttachFolderMenu(NSMenu *menu, NSString *folder, id target, BOOL shared,
                               void (^footer)(NSMenu *));

// The rows in order: a folder a submenu of its own (filled as it opens), a
// bookmark an item sending openBookmarkFromMenu:. Returns how many files.
static NSUInteger MCAddEntries(NSMenu *menu, const std::vector<WebBookmark::Entry> &entries,
                               id target, BOOL shared) {
    NSUInteger files = 0;
    for (const WebBookmark::Entry &e : entries) {
        NSString *title = @(e.title.c_str()) ?: @"";
        NSString *path = @(e.path.c_str());
        if (e.folder) {
            NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
            it.image = [NSImage imageWithSystemSymbolName:@"folder" accessibilityDescription:nil];
            NSMenu *sub = [[NSMenu alloc] initWithTitle:title];
            MCAttachFolderMenu(sub, path, target, shared, nil);
            it.submenu = sub;
            [menu addItem:it];
        } else {
            NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:title
                                                        action:@selector(openBookmarkFromMenu:)
                                                 keyEquivalent:@""];
            it.target = target;
            it.representedObject = path;
            [menu addItem:it];
            files++;
        }
    }
    return files;
}

// A shared folder's "Move to This Workspace": a submenu of its bookmarks.
static void MCAddMoveItems(NSMenu *menu, const std::vector<WebBookmark::Entry> &entries,
                           id target) {
    NSMenu *sub = [[NSMenu alloc] initWithTitle:@"Move to This Workspace"];
    for (const WebBookmark::Entry &e : entries) {
        if (e.folder) continue;
        NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:@(e.title.c_str()) ?: @""
                                                    action:@selector(moveBookmarkToWorkspace:)
                                             keyEquivalent:@""];
        it.target = target;
        it.representedObject = @(e.path.c_str());
        [sub addItem:it];
    }
    if (!sub.numberOfItems) return;
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *move = [[NSMenuItem alloc] initWithTitle:@"Move to This Workspace" action:nil
                                           keyEquivalent:@""];
    move.submenu = sub;
    [menu addItem:move];
}

static void MCAddOpenAll(NSMenu *menu, NSString *folder, id target) {
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *all = [[NSMenuItem alloc] initWithTitle:@"Open All in Tabs"
                                                 action:@selector(openBookmarkFolderFromMenu:)
                                          keyEquivalent:@""];
    all.target = target;
    all.representedObject = folder;
    [menu addItem:all];
}

static void MCAddNone(NSMenu *menu) {
    NSMenuItem *none = [[NSMenuItem alloc] initWithTitle:@"No Bookmarks" action:nil
                                           keyEquivalent:@""];
    none.enabled = NO;
    [menu addItem:none];
}

// Fills its menu from its folder each time the menu opens.
@interface MCBookmarksMenuFiller : NSObject <NSMenuDelegate>
@property(nonatomic, copy) NSString *folder;   // nil: MCBookmarksFolder()
@property(nonatomic, weak) id target;
@property(nonatomic) BOOL shared;              // a shared folder: Move to This Workspace
@property(nonatomic, copy) void (^footer)(NSMenu *);
@end

// Fills a window's menu: its own bookmarks, then Shared.
@interface MCWorkspaceBookmarksMenuFiller : NSObject <NSMenuDelegate>
@property(nonatomic, copy) NSString * (^root)(void);
@property(nonatomic, weak) id target;
@property(nonatomic, copy) void (^footer)(NSMenu *);
@end

static char kFillerKey;

@implementation MCBookmarksMenuFiller
- (void)menuNeedsUpdate:(NSMenu *)menu {
    [menu removeAllItems];
    NSString *folder = self.folder ?: MCBookmarksFolder();
    const std::vector<WebBookmark::Entry> entries =
        WebBookmark::entries(Utf8(folder), MCListNames(folder), self.shared, kMaxMenuEntries);
    const NSUInteger files = MCAddEntries(menu, entries, self.target, self.shared);
    if (entries.empty()) MCAddNone(menu);
    if (files > 1) MCAddOpenAll(menu, folder, self.target);
    if (self.shared) MCAddMoveItems(menu, entries, self.target);
    if (self.footer) self.footer(menu);
}
@end

@implementation MCWorkspaceBookmarksMenuFiller
- (void)menuNeedsUpdate:(NSMenu *)menu {
    [menu removeAllItems];
    NSString *root = self.root ? self.root() : nil;
    const WebBookmark::Places places = MCPlaces(root);
    NSString *save = Ns(places.save), *shared = Ns(places.shared);
    const std::vector<WebBookmark::Entry> all = WebBookmark::mergedEntries(
        places, MCListNames(save), MCListNames(shared), kMaxMenuEntries);
    std::vector<WebBookmark::Entry> own, others;
    for (const WebBookmark::Entry &e : all) (e.shared ? others : own).push_back(e);
    const NSUInteger files = MCAddEntries(menu, own, self.target, NO);
    if (own.empty()) MCAddNone(menu);
    if (files > 1) MCAddOpenAll(menu, save, self.target);
    if (!others.empty()) {
        [menu addItem:[NSMenuItem separatorItem]];
        NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:@"Shared" action:nil keyEquivalent:@""];
        it.image = [NSImage imageWithSystemSymbolName:@"folder" accessibilityDescription:nil];
        it.toolTip = shared.stringByAbbreviatingWithTildeInPath;
        NSMenu *sub = [[NSMenu alloc] initWithTitle:@"Shared"];
        MCAttachFolderMenu(sub, shared, self.target, YES, nil);
        it.submenu = sub;
        [menu addItem:it];
    }
    if (self.footer) self.footer(menu);
    if (shared) {
        NSMenuItem *show = [menu addItemWithTitle:@"Show Bookmarks Folder"
                                           action:@selector(showWorkspaceBookmarksFolder:)
                                    keyEquivalent:@""];
        show.representedObject = save;
        [menu addItemWithTitle:@"Show Shared Bookmarks Folder"
                        action:@selector(showBookmarksFolder:) keyEquivalent:@""];
    } else {
        [menu addItemWithTitle:@"Show Bookmarks Folder" action:@selector(showBookmarksFolder:)
                 keyEquivalent:@""];
    }
}
@end

static void MCAttachFolderMenu(NSMenu *menu, NSString *folder, id target, BOOL shared,
                               void (^footer)(NSMenu *)) {
    MCBookmarksMenuFiller *filler = [MCBookmarksMenuFiller new];
    filler.folder = folder;
    filler.target = target;
    filler.shared = shared;
    filler.footer = footer;
    menu.autoenablesItems = YES;
    menu.delegate = filler;
    // The menu holds its delegate only weakly.
    objc_setAssociatedObject(menu, &kFillerKey, filler, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

void MCAttachBookmarksMenu(NSMenu *menu, NSString *folder, id target, void (^footer)(NSMenu *)) {
    MCAttachFolderMenu(menu, folder, target, NO, footer);
}

void MCAttachWorkspaceBookmarksMenu(NSMenu *menu, NSString * (^root)(void), id target,
                                    void (^footer)(NSMenu *)) {
    MCWorkspaceBookmarksMenuFiller *filler = [MCWorkspaceBookmarksMenuFiller new];
    filler.root = root;
    filler.target = target;
    filler.footer = footer;
    menu.autoenablesItems = YES;
    menu.delegate = filler;
    objc_setAssociatedObject(menu, &kFillerKey, filler, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
