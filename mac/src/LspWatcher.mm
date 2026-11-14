// LspWatcher.mm — see LspWatcher.h.
#import "LspWatcher.h"
#import <CoreServices/CoreServices.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <sys/stat.h>

// What the stream's callback holds (retained by the stream through its
// context): the handler, whether the watcher still wants batches (a batch
// already on its way to the main thread when -stop runs is dropped there),
// which paths it has reported as there (used on the stream's queue only),
// and the servers' watchers to match against (set from the main thread,
// read on the queue, under the lock).
@interface MCLspWatchBox : NSObject {
@public
    std::atomic<bool> active;
    Lsp::FileEventClassifier classifier;
    std::mutex lock;
    std::shared_ptr<const std::vector<Lsp::WatchTarget>> targets;
}
@property(nonatomic, copy) MCLspWatcherHandler handler;
@property(nonatomic, weak) MCLspWatcher *owner;
@end
@implementation MCLspWatchBox
@end

@interface MCLspWatcher ()
- (void)delivered;
@end

static void MCLspWatchDeliver(MCLspWatchBox *box, std::vector<Lsp::FileChange> changes);

static void MCLspWatchCallback(ConstFSEventStreamRef stream, void *info, size_t n,
                               void *eventPaths, const FSEventStreamEventFlags flags[],
                               const FSEventStreamEventId ids[]) {
    (void)stream; (void)ids;
    MCLspWatchBox *box = (__bridge MCLspWatchBox *)info;
    if (!box->active.load()) return;
    char **paths = (char **)eventPaths;
    // Only these say a file's content or existence changed; an event with
    // just metadata (xattrs, owner, a touch) is left out.
    const FSEventStreamEventFlags content =
        kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemRemoved |
        kFSEventStreamEventFlagItemRenamed | kFSEventStreamEventFlagItemModified;
    std::vector<Lsp::FileChange> changes;
    for (size_t i = 0; i < n; i++) {
        FSEventStreamEventFlags f = flags[i];
        // The root moved, a volume came or went, the kernel dropped events
        // (MustScanSubDirs: no path to report exactly), or replay ended.
        if (f & (kFSEventStreamEventFlagRootChanged | kFSEventStreamEventFlagMount |
                 kFSEventStreamEventFlagUnmount | kFSEventStreamEventFlagHistoryDone |
                 kFSEventStreamEventFlagMustScanSubDirs))
            continue;
        if (!(f & content)) continue;
        std::string path = paths[i];
        while (path.size() > 1 && path.back() == '/') path.pop_back();
        if (Lsp::watchExcluded(path)) continue;
        struct stat st;
        bool exists = lstat(path.c_str(), &st) == 0;
        bool created = (f & (kFSEventStreamEventFlagItemCreated |
                             kFSEventStreamEventFlagItemRenamed)) != 0;
        changes.push_back({path, box->classifier.classify(path, exists, created)});
    }
    MCLspWatchDeliver(box, std::move(changes));
}

// On the stream's queue: coalesce, match against every target, and hand
// the batch and the ready messages to the main thread.
static void MCLspWatchDeliver(MCLspWatchBox *box, std::vector<Lsp::FileChange> changes) {
    changes = Lsp::coalesceFileChanges(changes);
    if (changes.empty()) return;
    std::shared_ptr<const std::vector<Lsp::WatchTarget>> targets;
    {
        std::lock_guard<std::mutex> hold(box->lock);
        targets = box->targets;
    }
    auto batches = std::make_shared<std::vector<Lsp::WatchedBatch>>();
    if (targets)
        for (const Lsp::WatchTarget &t : *targets) batches->push_back(Lsp::prepareWatched(t, changes));
    auto shared = std::make_shared<const std::vector<Lsp::FileChange>>(std::move(changes));
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!box->active.load()) return;
        [box.owner delivered];
        if (box.handler) box.handler(*shared, *batches);
    });
}

@implementation MCLspWatcher {
    FSEventStreamRef _stream;
    MCLspWatchBox *_box;
    dispatch_queue_t _queue;
}

- (instancetype)initWithPaths:(NSArray<NSString *> *)paths handler:(MCLspWatcherHandler)handler {
    if (!(self = [super init])) return nil;
    // Existing folders, each once, none inside another (the outer one's
    // stream already reports it).
    NSMutableArray<NSString *> *kept = [NSMutableArray array];
    NSArray *sorted = [paths sortedArrayUsingSelector:@selector(compare:)];
    for (NSString *p in sorted) {
        BOOL dir = NO;
        if (!p.length || ![[NSFileManager defaultManager] fileExistsAtPath:p isDirectory:&dir] || !dir)
            continue;
        BOOL inside = NO;
        for (NSString *k in kept)
            if ([p isEqualToString:k] || [p hasPrefix:[k stringByAppendingString:@"/"]]) inside = YES;
        if (!inside) [kept addObject:p];
    }
    if (!kept.count) return nil;
    _paths = [kept copy];
    _box = [MCLspWatchBox new];
    _box->active.store(true);
    _box.handler = handler;
    _box.owner = self;
    _queue = dispatch_queue_create("minicode.lsp-watch", DISPATCH_QUEUE_SERIAL);
    FSEventStreamContext ctx = {0, (__bridge void *)_box, CFRetain, CFRelease, NULL};
    _stream = FSEventStreamCreate(
        NULL, &MCLspWatchCallback, &ctx, (__bridge CFArrayRef)_paths,
        kFSEventStreamEventIdSinceNow, 0.3 /* seconds of coalescing */,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagWatchRoot |
            kFSEventStreamCreateFlagNoDefer);
    if (!_stream) return nil;
    FSEventStreamSetDispatchQueue(_stream, _queue);
    if (!FSEventStreamStart(_stream)) {
        FSEventStreamInvalidate(_stream);
        FSEventStreamRelease(_stream);
        _stream = NULL;
        return nil;
    }
    return self;
}

- (void)delivered {
    _batches++;
}

- (void)setTargets:(const std::vector<Lsp::WatchTarget> &)targets {
    auto copy = std::make_shared<const std::vector<Lsp::WatchTarget>>(targets);
    std::lock_guard<std::mutex> hold(_box->lock);
    _box->targets = std::move(copy);
}

- (void)injectChanges:(const std::vector<Lsp::FileChange> &)changes {
    MCLspWatchBox *box = _box;
    std::vector<Lsp::FileChange> copy = changes;
    dispatch_async(_queue, ^{
        if (box->active.load()) MCLspWatchDeliver(box, copy);
    });
}

- (void)stop {
    if (_box) _box->active.store(false);
    if (!_stream) return;
    FSEventStreamStop(_stream);
    FSEventStreamInvalidate(_stream);
    FSEventStreamRelease(_stream);
    _stream = NULL;
}

- (void)dealloc {
    [self stop];
}

@end
