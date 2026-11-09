// BackupStore.mm — see BackupStore.h.
#import "BackupStore.h"
#include <sys/sysctl.h>
#include <unistd.h>

// When a process started (Unix seconds), or 0 when there is no such process.
static double MCProcessStart(pid_t pid) {
    struct kinfo_proc info;
    size_t size = sizeof info;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    if (sysctl(mib, 4, &info, &size, NULL, 0) != 0 || size == 0) return 0;
    const struct timeval tv = info.kp_proc.p_starttime;
    return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

static std::string U8(NSString *s) { return s.UTF8String ? s.UTF8String : ""; }

@implementation MCBackupStore {
    Backups::Store _store;
    dispatch_queue_t _queue;
    NSMutableDictionary<NSString *, NSNumber *> *_latest;   // id -> newest request
    uint64_t _generation;
    double _myStart;
    BOOL _cleaned;
    // Backups a window of this process brought back: held here from then
    // on, even before the first write puts this process's name in them.
    NSMutableSet<NSString *> *_claimed;
}

+ (instancetype)shared {
    static MCBackupStore *store;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSDictionary *env = NSProcessInfo.processInfo.environment;
        if (env[@"MINICODE_DEMO"] || env[@"MINICODE_BENCH"]) return;
        store = [MCBackupStore new];
    });
    return store;
}

- (instancetype)init {
    if ((self = [super init])) {
        NSString *dir = NSProcessInfo.processInfo.environment[@"MINICODE_BACKUP_DIR"];
        if (!dir.length)
            dir = [NSHomeDirectory() stringByAppendingPathComponent:
                   @"Library/Application Support/MiniCode/Backups"];
        _directory = dir.stringByStandardizingPath;
        _store.dir = U8(_directory);
        _store.fs = Backups::posixFs();
        _queue = dispatch_queue_create("minicode.backups", DISPATCH_QUEUE_SERIAL);
        _latest = [NSMutableDictionary dictionary];
        _myStart = MCProcessStart(getpid());
        _claimed = [NSMutableSet set];
    }
    return self;
}

- (NSString *)newID { return NSUUID.UUID.UUIDString; }

- (BOOL)isLive:(const Backups::Entry &)e {
    @synchronized(_claimed) {
        if ([_claimed containsObject:@(e.id.c_str())]) return YES;
    }
    if (e.ownerPid == getpid()) return fabs(e.ownerStart - _myStart) < 1.0;
    if (e.ownerPid <= 0) return NO;
    const double start = MCProcessStart((pid_t)e.ownerPid);
    // The same process if it started when the backup says (a reused pid
    // started later); a second's slack for rounding.
    return start > 0 && fabs(start - e.ownerStart) < 1.0;
}

- (Backups::IsLive)liveTest {
    __weak MCBackupStore *weakSelf = self;
    return [weakSelf](const Backups::Entry &e) { return (bool)[weakSelf isLive:e]; };
}

- (Backups::Entry)entryForID:(NSString *)bid path:(NSString *)path root:(NSString *)root
                    diskDate:(NSDate *)diskDate inode:(uint64_t)inode
                fileEncoding:(NSString *)fileEncoding {
    Backups::Entry e;
    e.fileEncoding = U8(fileEncoding ?: @"");
    e.id = U8(bid);
    e.path = U8(path);
    e.root = U8(root);
    e.diskExisted = diskDate != nil;
    e.diskMtime = diskDate ? diskDate.timeIntervalSince1970 : 0;
    e.diskInode = inode;
    e.saved = [NSDate date].timeIntervalSince1970;
    e.ownerPid = getpid();
    e.ownerStart = _myStart;
    return e;
}

- (uint64_t)stamp:(NSString *)bid {
    @synchronized(_latest) {
        _latest[bid] = @(++_generation);
        return _generation;
    }
}
- (BOOL)isNewest:(NSString *)bid stamp:(uint64_t)stamp {
    @synchronized(_latest) {
        if ([_latest[bid] unsignedLongLongValue] != stamp) return NO;
        [_latest removeObjectForKey:bid];
        return YES;
    }
}

- (void)writeID:(NSString *)bid path:(NSString *)path root:(NSString *)root
       diskDate:(NSDate *)diskDate inode:(uint64_t)inode
   fileEncoding:(NSString *)fileEncoding text:(NSString *)text {
    if (!bid.length || !path) return;
    const Backups::Entry e = [self entryForID:bid path:path root:root
                                     diskDate:diskDate inode:inode fileEncoding:fileEncoding];
    NSString *copy = [text copy] ?: @"";   // immutable: safe on the queue
    const uint64_t stamp = [self stamp:bid];
    dispatch_async(_queue, ^{
        if (![self isNewest:bid stamp:stamp]) return;   // a newer one follows
        // UTF-8 here, off the main thread: a large buffer costs nothing there.
        NSData *bytes = [copy dataUsingEncoding:NSUTF8StringEncoding];
        if (!bytes) return;
        std::string t((const char *)bytes.bytes, bytes.length);
        if (!self->_store.save(e, t))
            NSLog(@"minicode: could not write the backup of %@ in %@", path, self.directory);
    });
}

- (BOOL)writeNowID:(NSString *)bid path:(NSString *)path root:(NSString *)root
          diskDate:(NSDate *)diskDate inode:(uint64_t)inode
      fileEncoding:(NSString *)fileEncoding text:(NSString *)text {
    if (!bid.length || !path) return NO;
    const Backups::Entry e = [self entryForID:bid path:path root:root
                                     diskDate:diskDate inode:inode fileEncoding:fileEncoding];
    NSData *bytes = [text ?: @"" dataUsingEncoding:NSUTF8StringEncoding];
    if (!bytes) return NO;
    [self stamp:bid];   // anything still queued for this id is older
    __block BOOL ok = NO;
    dispatch_sync(_queue, ^{
        ok = self->_store.save(e, std::string((const char *)bytes.bytes, bytes.length));
    });
    return ok;
}

- (void)removeID:(NSString *)bid {
    if (!bid.length) return;
    const uint64_t stamp = [self stamp:bid];
    dispatch_async(_queue, ^{
        if (![self isNewest:bid stamp:stamp]) return;
        self->_store.discard(U8(bid));
    });
}

- (void)flush { dispatch_sync(_queue, ^{}); }

- (Backups::Restore)planForRoot:(NSString *)root
                   sessionFiles:(const std::vector<std::string> &)files {
    [self flush];   // this process's own writes are on disk first
    Backups::Restore r = Backups::plan(_store.list(), U8(root), files, [self liveTest]);
    for (const std::string &id : r.discard) _store.discard(id);
    @synchronized(_claimed) {
        for (const auto &item : r.items) [_claimed addObject:@(item.id.c_str())];
    }
    return r;
}

- (BOOL)loadID:(NSString *)bid entry:(Backups::Entry *)entry text:(NSString **)text {
    std::string t;
    if (!_store.load(U8(bid), entry, &t)) return NO;
    NSString *s = [[NSString alloc] initWithBytes:t.data() length:t.size()
                                         encoding:NSUTF8StringEncoding];
    if (!s) return NO;
    *text = s;
    return YES;
}

- (NSArray<NSString *> *)foldersToReopenExcept:(NSArray<NSString *> *)open {
    std::vector<std::string> roots;
    for (NSString *r in open) roots.push_back(U8(r));
    NSMutableArray *out = [NSMutableArray array];
    for (const std::string &r : Backups::foldersToReopen(_store.list(), roots, [self liveTest]))
        if (NSString *s = [NSString stringWithUTF8String:r.c_str()]) [out addObject:s];
    return out;
}

- (void)cleanUpOnce {
    if (_cleaned) return;
    _cleaned = YES;
    dispatch_async(_queue, ^{
        const double now = [NSDate date].timeIntervalSince1970;
        for (const std::string &id :
             Backups::expired(self->_store.list(), now, Backups::kMaxAge, [self liveTest]))
            self->_store.discard(id);
        // A temporary left by a crash mid-write is debris after a day; a
        // backup that does not read is kept as long as an orphan, in case a
        // person wants to look inside.
        self->_store.sweepDebris(now, 24 * 3600, Backups::kMaxAge);
    });
}

@end
