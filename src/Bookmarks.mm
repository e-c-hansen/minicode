// Bookmarks.mm — see Bookmarks.h.
#import "Bookmarks.h"

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
