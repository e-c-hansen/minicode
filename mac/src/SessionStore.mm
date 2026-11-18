// SessionStore.mm — see SessionStore.h.
#import "SessionStore.h"
#import "Environment.h"

#include <map>
#include <optional>
#include <sys/stat.h>

#include "FolderSearch.h"
#import "BackupStore.h"

namespace {

std::string U8(NSString *s) { return s.UTF8String ?: ""; }
NSString *NS(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

// Where the records of mutible 1.5.30 and before are: the user defaults,
// or the scratch suite the checks point them at.
NSUserDefaults *OldDefaults() {
    NSString *suite = MCEnvironment()[@"MUTIBLE_DEFAULTS_SUITE"];
    return suite.length ? [[NSUserDefaults alloc] initWithSuiteName:suite]
                        : [NSUserDefaults standardUserDefaults];
}
NSString *const kOldTabSessionsKey = @"MCTabSessions";
NSString *const kOldOpenWindowsKey = @"MCOpenWindows";
// Set once the old records have been carried over (they stay, for a
// mutible from before to read).
NSString *const kMovedKey = @"MCSessionsMovedToFile";

dispatch_queue_t Queue() {
    static dispatch_queue_t q = dispatch_queue_create("mutible.sessions", DISPATCH_QUEUE_SERIAL);
    return q;
}

// ---- on the queue only
// Changes handed over and not written yet: a folder's session (nullopt to
// remove it, as put does for an empty one) with when it was saved, and the
// windows.
std::map<std::string, std::pair<TabSessions::Session, double>> gFolders;
std::optional<std::vector<TabSessions::Window>> gWindows;
double gWindowsSaved = 0;   // when they were handed over
bool gWriteQueued = false;
bool gChecked = false;

TabSessions::Store ReadFile() {
    const std::string path = U8(MCSessionsFilePath());
    std::string text;
    FolderSearch::FileStamp stamp;
    const FolderSearch::ReadStatus st = FolderSearch::readFileStable(path, 256u << 20, &text, &stamp);
    if (st != FolderSearch::ReadStatus::Ok) return TabSessions::Store();
    return TabSessions::decodeStore(text);
}

void WriteFile(const TabSessions::Store &store) {
    NSString *path = MCSessionsFilePath();
    [[NSFileManager defaultManager] createDirectoryAtPath:path.stringByDeletingLastPathComponent
                              withIntermediateDirectories:YES attributes:nil error:nil];
    std::string error;
    if (!FolderSearch::writeFileAtomically(U8(path), TabSessions::encodeStore(store), &error))
        NSLog(@"mutible: could not write %@: %s", path, error.c_str());
}

void Apply(TabSessions::Store &store) {
    for (auto &kv : gFolders)
        TabSessions::put(store.folders, kv.first, kv.second.first, kv.second.second);
    // This process's windows over its own and the last run's; another
    // mutible's, running or quit since this one started, stay.
    if (gWindows)
        store.windows = TabSessions::mergeWindows(*gWindows, store.windows, MCSessionsThisProcess(),
                                                  gWindowsSaved, MCSessionsIsRunning, nullptr);
}

// The first time: a mutible with no file yet takes the user defaults'
// records over, once.
void CarryOverOnce() {
    if (gChecked) return;
    gChecked = true;
    NSUserDefaults *d = OldDefaults();
    if ([d boolForKey:kMovedKey]) return;
    struct stat sb;
    if (stat(U8(MCSessionsFilePath()).c_str(), &sb) != 0) {
        NSDictionary *folders = [d dictionaryForKey:kOldTabSessionsKey];
        NSString *windows = [d stringForKey:kOldOpenWindowsKey];
        if (folders.count || windows.length) WriteFile(MCSessionsFromDefaults(folders, windows));
    }
    [d setBool:YES forKey:kMovedKey];
}

void WriteNow() {
    if (gFolders.empty() && !gWindows) return;   // nothing handed over (a flush)
    CarryOverOnce();
    TabSessions::Store store = ReadFile();
    Apply(store);
    gFolders.clear();
    gWindows.reset();
    WriteFile(store);
}

void QueueWrite() {
    if (gWriteQueued) return;
    gWriteQueued = true;
    dispatch_async(Queue(), ^{
        gWriteQueued = false;
        WriteNow();
    });
}

double WallNow() { return [NSDate date].timeIntervalSince1970; }

// An index into `count`, else -1.
int IndexIn(id n, NSUInteger count) {
    if (![n respondsToSelector:@selector(integerValue)]) return -1;
    const NSInteger i = [n integerValue];
    return i >= 0 && (NSUInteger)i < count ? (int)i : -1;
}

}  // namespace

TabSessions::Process MCSessionsThisProcess(void) {
    static const TabSessions::Process me = {(long)getpid(), MCProcessStart(getpid())};
    return me;
}

bool MCSessionsIsRunning(long pid, double start, void *) {
    if (pid <= 0) return false;
    const double now = MCProcessStart((pid_t)pid);
    return now > 0 && fabs(now - start) < 1.0;
}

std::vector<TabSessions::Window> MCSessionsWindowsToRestore(void) {
    return TabSessions::toRestore(MCSessionsRead().windows, MCSessionsThisProcess(),
                                  MCSessionsIsRunning, nullptr);
}

NSString *MCSessionsFilePath(void) {
    NSString *env = MCEnvironment()[@"MUTIBLE_SESSIONS_FILE"];
    if (env.length) return env.stringByStandardizingPath;
    return [NSHomeDirectory()
        stringByAppendingPathComponent:@"Library/Application Support/Mutible/sessions.json"];
}

TabSessions::Store MCSessionsRead(void) {
    __block TabSessions::Store store;
    dispatch_sync(Queue(), ^{
        CarryOverOnce();
        store = ReadFile();
        Apply(store);
    });
    return store;
}

void MCSessionsPutFolder(const std::string &key, const TabSessions::Session &s) {
    // Copies the block holds: a block captures a C++ reference as the
    // reference, which the caller's temporary outlives only until it returns.
    const std::string folder = key;
    const TabSessions::Session copy = s;
    const double now = WallNow();
    dispatch_async(Queue(), ^{
        gFolders[folder] = {copy, now};
        QueueWrite();
    });
}

void MCSessionsPutWindows(const std::vector<TabSessions::Window> &windows) {
    const std::vector<TabSessions::Window> copy = windows;
    const double now = WallNow();
    dispatch_async(Queue(), ^{
        gWindows = copy;
        gWindowsSaved = now;
        QueueWrite();
    });
}

void MCSessionsFlush(void) {
    dispatch_sync(Queue(), ^{ WriteNow(); });
}

// ---------------------------------------------------- the browser pane

void MCSessionPutBrowserPage(NSDictionary *page, TabSessions::Session *s) {
    s->browser.clear();
    s->browserActive = -1;
    if (![page isKindOfClass:NSDictionary.class]) return;
    NSArray *entries = [page[@"tabs"] isKindOfClass:NSArray.class] ? page[@"tabs"] : nil;
    int active = IndexIn(page[@"active"], entries.count);
    if (!entries) {   // the one page an older mutible kept
        entries = @[page];
        active = 0;
    }
    std::vector<int> kept;
    for (NSDictionary *e in entries) {
        NSString *url = [e isKindOfClass:NSDictionary.class] && [e[@"url"] isKindOfClass:NSString.class]
                            ? e[@"url"] : nil;
        if (!url.length) {
            kept.push_back(-1);
            continue;
        }
        TabSessions::BrowserTab b;
        b.url = U8(url);
        if ([e[@"title"] isKindOfClass:NSString.class]) b.title = U8(e[@"title"]);
        if ([e[@"state"] isKindOfClass:NSData.class] && [e[@"state"] length])
            b.state = U8([(NSData *)e[@"state"] base64EncodedStringWithOptions:0]);
        b.archived = [e[@"archived"] respondsToSelector:@selector(boolValue)] && [e[@"archived"] boolValue];
        kept.push_back((int)s->browser.size());
        s->browser.push_back(b);
    }
    s->browserActive = active >= 0 && active < (int)kept.size() && kept[(size_t)active] >= 0
                           ? kept[(size_t)active]
                           : (s->browser.empty() ? -1 : 0);
}

NSDictionary *MCSessionBrowserPage(const TabSessions::Session &s) {
    if (s.browser.empty()) return nil;
    NSMutableArray *tabs = [NSMutableArray array];
    for (const TabSessions::BrowserTab &b : s.browser) {
        NSMutableDictionary *e = [NSMutableDictionary dictionaryWithObject:NS(b.url) forKey:@"url"];
        if (!b.title.empty()) e[@"title"] = NS(b.title);
        NSData *state = b.state.empty() ? nil
                                        : [[NSData alloc] initWithBase64EncodedString:NS(b.state)
                                                                              options:0];
        if (state) {
            e[@"state"] = state;
            e[@"archived"] = @(b.archived);
        }
        [tabs addObject:e];
    }
    return @{@"tabs": tabs, @"active": @(s.browserActive)};
}

// ------------------------------------------- the user defaults, carried over

TabSessions::Store MCSessionsFromDefaults(NSDictionary *tabSessions, NSString *openWindows) {
    TabSessions::Store store;
    if ([openWindows isKindOfClass:NSString.class] && openWindows.length)
        store.windows = TabSessions::decodeWindows(U8(openWindows));
    if (![tabSessions isKindOfClass:NSDictionary.class]) return store;
    for (NSString *key in tabSessions) {
        NSDictionary *e = tabSessions[key];
        if (![key isKindOfClass:NSString.class] || !key.length ||
            ![e isKindOfClass:NSDictionary.class])
            continue;
        TabSessions::Session s;
        NSArray *files = [e[@"files"] isKindOfClass:NSArray.class] ? e[@"files"] : @[];
        NSArray *besides = [e[@"sidePreview"] isKindOfClass:NSArray.class] ? e[@"sidePreview"] : @[];
        NSString *preview = [e[@"preview"] isKindOfClass:NSString.class] ? e[@"preview"] : nil;
        const int active = IndexIn(e[@"active"], files.count);
        for (NSUInteger i = 0; i < files.count; i++) {
            NSString *f = files[i];
            if (![f isKindOfClass:NSString.class] || !f.length) continue;
            if ((int)i == active) s.active = (int)s.tabs.size();
            if (preview && [f isEqualToString:preview]) s.preview = (int)s.tabs.size();
            TabSessions::Tab t;
            t.path = U8(f);
            t.side = [besides containsObject:f];
            s.tabs.push_back(t);
        }
        if (s.active < 0 && !s.tabs.empty()) s.active = 0;
        NSDictionary *g = [e[@"groups"] isKindOfClass:NSDictionary.class] ? e[@"groups"] : nil;
        NSArray *lists = [g[@"groups"] isKindOfClass:NSArray.class] ? g[@"groups"] : nil;
        NSArray *shown = [g[@"active"] isKindOfClass:NSArray.class] ? g[@"active"] : nil;
        if (lists.count == 2) {
            for (int k = 0; k < 2; k++) {
                if ([lists[k] isKindOfClass:NSArray.class])
                    for (NSString *f in lists[k])
                        if ([f isKindOfClass:NSString.class] && f.length)
                            s.groups.groups[k].push_back(U8(f));
                if (shown.count == 2 && [shown[k] isKindOfClass:NSString.class])
                    s.groups.active[k] = U8(shown[k]);
            }
            s.groups.focused = [g[@"focused"] intValue] == 1 ? 1 : 0;
            const double fr = [g[@"fraction"] doubleValue];
            s.groups.fraction = fr > 0 && fr < 1 ? fr : 0.5;
            s.split = !s.groups.groups[0].empty() && !s.groups.groups[1].empty();
            if (!s.split) s.groups = EditorTabs::GroupsSession();
        }
        NSDictionary *b = [e[@"browser"] isKindOfClass:NSDictionary.class] ? e[@"browser"] : nil;
        if (b) {
            MCSessionPutBrowserPage(b[@"page"], &s);
            s.browserShown = [b[@"visible"] boolValue];
            const double fr = [b[@"fraction"] doubleValue];
            s.browserFraction = fr > 0 && fr < 1 ? fr : 0;
        }
        const double used = [e[@"used"] respondsToSelector:@selector(doubleValue)]
                                ? [e[@"used"] doubleValue] : 0;
        TabSessions::put(store.folders, U8(key), s, used, (size_t)-1);
    }
    // The 50 used last, as the defaults kept them.
    while (store.folders.size() > TabSessions::kMaxFolders) {
        auto oldest = store.folders.begin();
        for (auto it = store.folders.begin(); it != store.folders.end(); ++it)
            if (it->second.used < oldest->second.used) oldest = it;
        store.folders.erase(oldest);
    }
    return store;
}
