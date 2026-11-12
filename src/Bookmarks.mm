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

// ------------------------------------------------------------ the menu
static const NSUInteger kMaxMenuEntries = 1000;

// Fills its menu from its folder each time the menu opens.
@interface MCBookmarksMenuFiller : NSObject <NSMenuDelegate>
@property(nonatomic, copy) NSString *folder;   // nil: MCBookmarksFolder()
@property(nonatomic, weak) id target;
@property(nonatomic, copy) void (^footer)(NSMenu *);
@end

static char kFillerKey;

@implementation MCBookmarksMenuFiller
- (void)menuNeedsUpdate:(NSMenu *)menu {
    [menu removeAllItems];
    NSString *folder = self.folder ?: MCBookmarksFolder();
    NSFileManager *fm = [NSFileManager defaultManager];
    NSArray<NSString *> *names = [[fm contentsOfDirectoryAtPath:folder error:nil]
        sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
    NSMutableArray<NSString *> *dirs = [NSMutableArray array], *files = [NSMutableArray array];
    for (NSString *n in names) {
        if ([n hasPrefix:@"."]) continue;
        NSString *path = [folder stringByAppendingPathComponent:n];
        BOOL isDir = NO;
        if (![fm fileExistsAtPath:path isDirectory:&isDir]) continue;
        if (isDir && ![[NSWorkspace sharedWorkspace] isFilePackageAtPath:path]) [dirs addObject:n];
        else if (!isDir && MCIsBookmarkFile(n)) [files addObject:n];
    }
    NSUInteger shown = 0;
    for (NSString *n in dirs) {
        if (shown++ >= kMaxMenuEntries) break;
        NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:n action:nil keyEquivalent:@""];
        it.image = [NSImage imageWithSystemSymbolName:@"folder" accessibilityDescription:nil];
        NSMenu *sub = [[NSMenu alloc] initWithTitle:n];
        MCAttachBookmarksMenu(sub, [folder stringByAppendingPathComponent:n], self.target, nil);
        it.submenu = sub;
        [menu addItem:it];
    }
    for (NSString *n in files) {
        if (shown++ >= kMaxMenuEntries) break;
        const std::string title = WebBookmark::menuTitle(Utf8(n));
        NSMenuItem *it = [[NSMenuItem alloc]
            initWithTitle:[NSString stringWithUTF8String:title.c_str()] ?: n
                   action:@selector(openBookmarkFromMenu:)
            keyEquivalent:@""];
        it.target = self.target;
        it.representedObject = [folder stringByAppendingPathComponent:n];
        [menu addItem:it];
    }
    if (!dirs.count && !files.count) {
        NSMenuItem *none = [[NSMenuItem alloc] initWithTitle:@"No Bookmarks" action:nil
                                               keyEquivalent:@""];
        none.enabled = NO;
        [menu addItem:none];
    }
    if (files.count > 1) {
        [menu addItem:[NSMenuItem separatorItem]];
        NSMenuItem *all = [[NSMenuItem alloc] initWithTitle:@"Open All in Tabs"
                                                     action:@selector(openBookmarkFolderFromMenu:)
                                              keyEquivalent:@""];
        all.target = self.target;
        all.representedObject = folder;
        [menu addItem:all];
    }
    if (self.footer) self.footer(menu);
}
@end

void MCAttachBookmarksMenu(NSMenu *menu, NSString *folder, id target, void (^footer)(NSMenu *)) {
    MCBookmarksMenuFiller *filler = [MCBookmarksMenuFiller new];
    filler.folder = folder;
    filler.target = target;
    filler.footer = footer;
    menu.autoenablesItems = YES;
    menu.delegate = filler;
    // The menu holds its delegate only weakly.
    objc_setAssociatedObject(menu, &kFillerKey, filler, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
