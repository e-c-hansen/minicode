// AgentModel.mm — see AgentModel.h.
#import "AgentModel.h"
#import "CompareView.h"
#import "GitPanel.h"

#include "AgentIndex.h"
#include "AgentView.h"

#include <CoreServices/CoreServices.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

NSNotificationName const MCAgentModelDidChangeNotification = @"MCAgentModelDidChange";
NSNotificationName const MCAgentModelWorktreesDidChangeNotification =
    @"MCAgentModelWorktreesDidChange";

static std::string U8(NSString *s) { return s.UTF8String ? std::string(s.UTF8String) : std::string(); }
static NSString *NS(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size()
                                  encoding:NSUTF8StringEncoding] ?: @"";
}
static NSString *NSOrNil(const std::string &s) { return s.empty() ? nil : NS(s); }

static NSString *RealPath(NSString *p) {
    if (!p.length) return p;
    char buf[PATH_MAX];
    return realpath(p.fileSystemRepresentation, buf) ? @(buf) : p;
}

// ------------------------------------------------------------ the snapshots
@interface MCAgentFile ()
@property(nonatomic, readwrite, copy) NSString *path, *transcript, *folderText;
@property(nonatomic, readwrite) NSInteger place, writes, edits, failed;
@property(nonatomic, readwrite) BOOL exists;
@end
@implementation MCAgentFile
@end

@interface MCAgentFileGroup ()
@property(nonatomic, readwrite) NSInteger place;
@property(nonatomic, readwrite, copy) NSString *title, *transcript;
@property(nonatomic, readwrite, copy) NSArray<MCAgentFile *> *files;
@end
@implementation MCAgentFileGroup
@end

@interface MCAgentWork ()
@property(nonatomic, readwrite, copy) NSString *transcript, *sessionId, *agentId, *parent;
@property(nonatomic, readwrite, copy) NSString *name, *title, *agentType, *cwd, *gitBranch;
@property(nonatomic, readwrite, copy) NSString *worktreePath, *worktreeBranch, *scratchpad;
@property(nonatomic, readwrite, copy) NSArray<NSString *> *versions;
@property(nonatomic, readwrite) NSTimeInterval started, lastActivity, written;
@property(nonatomic, readwrite) BOOL finished, hasMeta;
@property(nonatomic, readwrite) NSInteger badLines, filesTouched, bashCommands, depth;
@property(nonatomic, readwrite, copy) NSArray<MCAgentFileGroup *> *groups;
@property(nonatomic, readwrite, copy) NSArray<MCAgentWork *> *subagents;
@end
@implementation MCAgentWork
- (BOOL)runningAt:(NSDate *)now {
    return !self.finished && self.written > 0 && now.timeIntervalSince1970 - self.written <= 120;
}
- (NSString *)whereText {
    return self.worktreeBranch.length ? self.worktreeBranch : (self.gitBranch ?: @"");
}
@end

@interface MCAgentWorktree ()
@property(nonatomic, readwrite, copy) NSString *path, *branch, *head, *agentId, *label, *transcript;
@property(nonatomic, readwrite) BOOL running, dirty;
@property(nonatomic, readwrite) NSInteger ahead;
@end
@implementation MCAgentWorktree
- (NSString *)badge {
    return NS(AgentView::worktreeBadge((int)self.ahead, self.dirty));
}
@end

@interface MCWorktreeChange ()
@property(nonatomic, readwrite) unichar status;
@property(nonatomic, readwrite, copy) NSString *path, *from;
@end
@implementation MCWorktreeChange
@end

// What git said about one worktree.
struct WorktreeState {
    int ahead = -1;
    bool dirty = false;
};

// ------------------------------------------------------------------ model
@interface MCAgentModel ()
@property(nonatomic, readwrite) BOOL loaded, projectsMissing, worktreesLoaded;
@property(nonatomic, readwrite, copy) NSArray<MCAgentWork *> *sessions;
@property(nonatomic, readwrite) NSUInteger sessionCount, generation;
@property(nonatomic, readwrite, copy) NSArray<NSString *> *scratchpads;
@property(nonatomic, readwrite, copy) NSArray<MCAgentWorktree *> *worktrees;
@property(nonatomic, readwrite, copy) NSString *mainBranch;
@property(nonatomic, readwrite, copy) NSString *projectsDir, *tmpRoot;
- (void)eventsArrived;
@end

static void MCAgentFSCallback(ConstFSEventStreamRef stream, void *info, size_t n, void *paths,
                              const FSEventStreamEventFlags flags[],
                              const FSEventStreamEventId ids[]) {
    (void)stream; (void)n; (void)paths; (void)flags; (void)ids;
    MCAgentModel *model = (__bridge MCAgentModel *)info;
    [model eventsArrived];
}

@implementation MCAgentModel {
    dispatch_queue_t _queue;      // the index
    dispatch_queue_t _gitQueue;   // git
    std::unique_ptr<AgentIndex::Index> _index;   // only on _queue
    std::vector<AgentIndex::Worktree> _rawWorktrees;   // only on _queue
    std::string _mainRef;                              // only on _queue
    std::map<std::string, WorktreeState> _states;      // by real path, only on _queue
    std::string _signature;                            // only on _queue
    NSString *_realFolder;
    FSEventStreamRef _stream;
    NSArray<NSString *> *_watched;
    BOOL _refreshQueued, _gitQueued, _stopped;
    NSUInteger _folderGeneration;   // drops results for a folder no longer shown
    NSDictionary<NSString *, MCAgentWork *> *_byTranscript;
}

+ (NSString *)defaultProjectsDir {
    NSString *env = NSProcessInfo.processInfo.environment[@"MINICODE_CLAUDE_PROJECTS"];
    if (env.length) return env.stringByExpandingTildeInPath;
    return [NSHomeDirectory() stringByAppendingPathComponent:@".claude/projects"];
}

+ (NSString *)defaultTmpRoot {
    NSString *env = NSProcessInfo.processInfo.environment[@"MINICODE_CLAUDE_TMP"];
    if (env.length) return env.stringByExpandingTildeInPath;
    return [NSString stringWithFormat:@"/private/tmp/claude-%u", getuid()];
}

- (instancetype)initWithFolder:(NSString *)folder {
    if ((self = [super init])) {
        _queue = dispatch_queue_create("minicode.agents", DISPATCH_QUEUE_SERIAL);
        _gitQueue = dispatch_queue_create("minicode.agents.git", DISPATCH_QUEUE_SERIAL);
        _projectsDir = [MCAgentModel defaultProjectsDir];
        _tmpRoot = [MCAgentModel defaultTmpRoot];
        _sessions = @[];
        _scratchpads = @[];
        _worktrees = @[];
        AgentIndex::Options opt;
        opt.projectsDir = U8(_projectsDir);
        opt.tmpRoot = U8(_tmpRoot);
        _index.reset(new AgentIndex::Index(AgentTranscript::posixFs(), opt));
        self.folder = folder;
    }
    return self;
}

- (void)dealloc {
    [self stopStream];
}

- (void)stop {
    _stopped = YES;
    [self stopStream];
}

- (void)setFolder:(NSString *)folder {
    _folder = [folder copy];
    _realFolder = RealPath(folder);
    _folderGeneration++;
    _loaded = NO;
    _worktreesLoaded = NO;
    _sessions = @[];
    _byTranscript = @{};
    _worktrees = @[];
    _scratchpads = @[];
    _mainBranch = nil;
    std::string real = U8(_realFolder);
    dispatch_async(_queue, ^{
        self->_index->setFolder(real);
        self->_rawWorktrees.clear();
        self->_states.clear();
        self->_mainRef.clear();
        self->_signature.clear();
    });
    [self startStream];
    _refreshQueued = NO;
    _gitQueued = NO;
    [self refreshNow];
    [self refreshWorktrees];
}

// ------------------------------------------------------------- FSEvents
// The transcripts folder and the scratchpads' folder; one that is not there
// yet is watched through the nearest folder above it that is, so its
// creation is seen (and the stream is made again then).
- (NSArray<NSString *> *)pathsToWatch {
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    NSFileManager *fm = NSFileManager.defaultManager;
    for (NSString *want in @[self.projectsDir ?: @"", self.tmpRoot ?: @""]) {
        NSString *p = want;
        while (p.length > 1 && ![fm fileExistsAtPath:p]) p = p.stringByDeletingLastPathComponent;
        if (p.length > 1 && ![out containsObject:p]) [out addObject:p];
    }
    return out;
}

- (void)startStream {
    if (_stopped) return;
    NSArray<NSString *> *paths = [self pathsToWatch];
    if (_stream && [paths isEqualToArray:_watched]) return;
    [self stopStream];
    _watched = paths;
    if (!paths.count) return;
    FSEventStreamContext ctx = {0, (__bridge void *)self, NULL, NULL, NULL};
    _stream = FSEventStreamCreate(NULL, &MCAgentFSCallback, &ctx, (__bridge CFArrayRef)paths,
                                  kFSEventStreamEventIdSinceNow, 0.3,
                                  kFSEventStreamCreateFlagWatchRoot |
                                      kFSEventStreamCreateFlagNoDefer);
    if (!_stream) return;
    FSEventStreamSetDispatchQueue(_stream, dispatch_get_main_queue());
    FSEventStreamStart(_stream);
}

- (void)stopStream {
    if (!_stream) return;
    FSEventStreamStop(_stream);
    FSEventStreamInvalidate(_stream);
    FSEventStreamRelease(_stream);
    _stream = NULL;
    _watched = nil;
}

- (void)eventsArrived {
    [self refresh];
}

// ------------------------------------------------------------- refreshing
- (void)refresh {
    if (_stopped || _refreshQueued) return;
    _refreshQueued = YES;
    __weak MCAgentModel *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.2 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        MCAgentModel *s = weakSelf;
        if (!s) return;
        s->_refreshQueued = NO;
        [s refreshNow];
    });
}

- (void)refreshNow {
    if (_stopped) return;
    const NSUInteger gen = _folderGeneration;
    NSString *projects = self.projectsDir;
    __weak MCAgentModel *weakSelf = self;
    dispatch_async(_queue, ^{
        MCAgentModel *s = weakSelf;
        if (!s) return;
        [s refreshOnQueueForGeneration:gen projects:projects force:NO];
    });
}

// On _queue: reads what changed and, when anything shown changed, copies the
// rows and publishes them.
- (void)refreshOnQueueForGeneration:(NSUInteger)gen projects:(NSString *)projects force:(BOOL)force {
    struct stat st;
    const BOOL missing = stat(projects.fileSystemRepresentation, &st) != 0;
    _index->refresh();
    const double now = [NSDate date].timeIntervalSince1970;

    // The rows, nested: each row's subagents follow it at depth + 1.
    std::vector<AgentIndex::Index::Row> rows = _index->rows();
    NSMutableArray<MCAgentWork *> *top = [NSMutableArray array];
    NSMutableDictionary<NSString *, MCAgentWork *> *by = [NSMutableDictionary dictionary];
    std::string sig = missing ? "M" : "P";
    const std::string real = U8(_realFolder), home = U8(NSHomeDirectory());
    for (const AgentIndex::Index::Row &r : rows) {
        const AgentIndex::Session &x = *r.session;
        MCAgentWork *s = [MCAgentWork new];
        s.transcript = NS(x.transcript);
        s.sessionId = NS(x.sessionId);
        s.agentId = NSOrNil(x.agentId);
        s.parent = NSOrNil(x.parent);
        s.name = x.description.empty() ? @"(no prompt)" : NS(x.description);
        s.title = NSOrNil(x.title);
        s.agentType = NSOrNil(x.agentType);
        s.cwd = NSOrNil(x.cwd);
        s.gitBranch = NSOrNil(x.gitBranch);
        s.worktreePath = NSOrNil(x.worktreePath);
        s.worktreeBranch = NSOrNil(x.worktreeBranch);
        s.scratchpad = NSOrNil(x.scratchpad);
        NSMutableArray *versions = [NSMutableArray array];
        for (const std::string &v : x.versions) [versions addObject:NS(v)];
        s.versions = versions;
        s.started = x.started;
        s.lastActivity = x.lastActivity;
        s.written = x.written;
        s.finished = x.finished;
        s.hasMeta = x.hasMeta;
        s.badLines = x.badLines;
        s.filesTouched = x.filesTouched();
        s.bashCommands = x.bashCommands;
        s.depth = r.depth;
        NSMutableArray<MCAgentFileGroup *> *groups = [NSMutableArray array];
        for (const AgentView::FileGroup &g : AgentView::groupFiles(x.files)) {
            MCAgentFileGroup *fg = [MCAgentFileGroup new];
            fg.place = (NSInteger)g.place;
            fg.title = @(AgentIndex::placeName(g.place));
            fg.transcript = s.transcript;
            NSMutableArray<MCAgentFile *> *files = [NSMutableArray array];
            for (size_t i : g.files) {
                const AgentIndex::FileTouch &t = x.files[i];
                MCAgentFile *f = [MCAgentFile new];
                f.path = NS(t.path);
                f.place = (NSInteger)t.place;
                f.writes = t.writes;
                f.edits = t.edits;
                f.failed = t.failed;
                f.exists = stat(t.path.c_str(), &st) == 0;
                f.transcript = s.transcript;
                f.folderText = NS(AgentView::folderText(t.path, t.place, real, x.worktreePath,
                                                        x.scratchpad, home));
                [files addObject:f];
                sig += f.exists ? '1' : '0';
            }
            fg.files = files;
            [groups addObject:fg];
        }
        s.groups = groups;
        sig += x.transcript + '|' + std::to_string(x.lastActivity) + '|' +
               std::to_string(x.written) + '|' + std::to_string(x.files.size()) + '|' +
               std::to_string(x.bashCommands) + '|' + (x.finished ? "f" : "") + x.description +
               x.scratchpad + '\n';

        if (r.depth == 0) [top addObject:s];
        by[s.transcript] = s;
    }
    // Subagents: rows are each session followed by its own, so a row's
    // parent is the nearest row above it one level up.
    {
        std::vector<MCAgentWork *> chain;
        NSMutableDictionary<NSString *, NSMutableArray<MCAgentWork *> *> *children =
            [NSMutableDictionary dictionary];
        for (const AgentIndex::Index::Row &r : rows) {
            MCAgentWork *s = by[NS(r.session->transcript)];
            if (!s) continue;
            chain.resize((size_t)r.depth);
            if (r.depth > 0 && chain.size() == (size_t)r.depth && chain.back()) {
                NSString *key = chain.back().transcript;
                if (!children[key]) children[key] = [NSMutableArray array];
                [children[key] addObject:s];
            }
            chain.push_back(s);
        }
        for (NSString *key in children) by[key].subagents = children[key];
        for (MCAgentWork *s in by.allValues)
            if (!s.subagents) s.subagents = @[];
    }

    NSMutableArray<NSString *> *pads = [NSMutableArray array];
    for (const std::string &p : _index->scratchpads()) [pads addObject:NS(p)];
    NSArray<MCAgentWorktree *> *worktrees = [self labelledWorktreesAt:now];

    if (!force && sig == _signature) return;
    _signature = sig;
    NSArray<MCAgentWork *> *published = [top copy];
    NSDictionary *byCopy = [by copy];
    __weak MCAgentModel *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        MCAgentModel *s = weakSelf;
        if (!s || s->_stopped || gen != s->_folderGeneration) return;
        const BOOL wasMissing = s.projectsMissing;
        s.projectsMissing = missing;
        s.sessions = published;
        s->_byTranscript = byCopy;
        s.sessionCount = byCopy.count;
        s.scratchpads = pads;
        s.loaded = YES;
        if (s.worktreesLoaded) s.worktrees = worktrees;
        s.generation++;
        if (wasMissing != missing) [s startStream];   // it came or went
        [NSNotificationCenter.defaultCenter postNotificationName:MCAgentModelDidChangeNotification
                                                          object:s];
        if (s.worktreesLoaded)
            [NSNotificationCenter.defaultCenter
                postNotificationName:MCAgentModelWorktreesDidChangeNotification object:s];
    });
}

// On _queue: git's worktrees under the folder, named from the index, in the
// window's spelling of the folder.
- (NSArray<MCAgentWorktree *> *)labelledWorktreesAt:(double)now {
    NSMutableArray<MCAgentWorktree *> *out = [NSMutableArray array];
    if (_rawWorktrees.empty()) return out;
    std::string real = U8(_realFolder);
    NSString *folder = self.folder ?: @"";
    for (const AgentIndex::Worktree &w :
         AgentIndex::worktreesUnder(_rawWorktrees, real, _index.get(), now)) {
        MCAgentWorktree *t = [MCAgentWorktree new];
        NSString *path = NS(w.path);
        NSString *realFolder = _realFolder ?: @"";
        if (realFolder.length && [path hasPrefix:[realFolder stringByAppendingString:@"/"]])
            path = [folder stringByAppendingString:[path substringFromIndex:realFolder.length]];
        t.path = path;
        t.branch = NSOrNil(w.branch);
        t.head = NSOrNil(w.head);
        t.agentId = NSOrNil(w.agentId);
        t.label = w.description.empty() ? path.lastPathComponent : NS(w.description);
        t.transcript = NSOrNil(w.transcript);
        t.running = w.running;
        auto st = _states.find(w.path);
        t.ahead = st == _states.end() ? -1 : st->second.ahead;
        t.dirty = st != _states.end() && st->second.dirty;
        [out addObject:t];
    }
    return out;
}

- (void)refreshWorktrees {
    if (_stopped || _gitQueued) return;
    _gitQueued = YES;
    __weak MCAgentModel *weakSelf = self;
    // The first time at once; later ones gathered over two seconds (agents
    // write into their worktrees all the time).
    const double delay = self.worktreesLoaded ? 2.0 : 0.0;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        MCAgentModel *s = weakSelf;
        if (!s) return;
        s->_gitQueued = NO;
        [s runWorktreeGit];
    });
}

- (void)runWorktreeGit {
    if (_stopped || !self.folder.length) return;
    const NSUInteger gen = _folderGeneration;
    NSString *folder = self.folder;
    std::string real = U8(_realFolder);
    __weak MCAgentModel *weakSelf = self;
    dispatch_async(_gitQueue, ^{
        std::vector<AgentIndex::Worktree> all;
        std::map<std::string, WorktreeState> states;
        std::string mainRef;
        if (MCGitExecutable()) {
            MCGitResult *list = MCGitRunSync(folder, @[@"worktree", @"list", @"--porcelain"]);
            if (list.ok) {
                std::string out((const char *)list.output.bytes, list.output.length);
                all = AgentIndex::parseWorktreeList(out);
                mainRef = AgentView::mainRef(all);
            }
            for (const AgentIndex::Worktree &w : all) {
                if (w.main || w.bare || w.prunable || !AgentIndex::isInside(w.path, real) ||
                    w.path == real)
                    continue;
                WorktreeState st;
                NSString *wpath = NS(w.path);
                if (!mainRef.empty() && !w.head.empty()) {
                    MCGitResult *r = MCGitRunSync(
                        folder, @[@"rev-list", @"--count",
                                  [NSString stringWithFormat:@"%@..%@", NS(mainRef), NS(w.head)],
                                  @"--"]);
                    if (r.ok) {
                        NSString *n = [[NSString alloc] initWithData:r.output
                                                            encoding:NSUTF8StringEncoding];
                        st.ahead = n.intValue;
                    }
                }
                MCGitResult *status =
                    MCGitRunSync(wpath, @[@"status", @"--porcelain", @"--untracked-files=normal"]);
                st.dirty = status.ok && status.output.length > 0;
                states[w.path] = st;
            }
        }
        MCAgentModel *s = weakSelf;
        if (!s) return;
        dispatch_async(s->_queue, ^{
            MCAgentModel *m = weakSelf;
            if (!m) return;
            m->_rawWorktrees = all;
            m->_states = states;
            m->_mainRef = mainRef;
            NSArray<MCAgentWorktree *> *wts =
                [m labelledWorktreesAt:[NSDate date].timeIntervalSince1970];
            NSString *mainBranch = NSOrNil(mainRef);
            dispatch_async(dispatch_get_main_queue(), ^{
                MCAgentModel *mm = weakSelf;
                if (!mm || mm->_stopped || gen != mm->_folderGeneration) return;
                mm.worktrees = wts;
                mm.mainBranch = mainBranch;
                mm.worktreesLoaded = YES;
                mm.generation++;
                [NSNotificationCenter.defaultCenter
                    postNotificationName:MCAgentModelWorktreesDidChangeNotification
                                  object:mm];
            });
        });
    });
}

// ------------------------------------------------------------- lookups
- (MCAgentWork *)sessionForTranscript:(NSString *)transcript {
    return transcript ? _byTranscript[transcript] : nil;
}

- (MCAgentWorktree *)worktreeAtPath:(NSString *)path {
    if (!path.length) return nil;
    NSString *p = path.stringByStandardizingPath;
    for (MCAgentWorktree *w in self.worktrees)
        if ([w.path isEqualToString:p]) return w;
    NSString *real = RealPath(p);
    for (MCAgentWorktree *w in self.worktrees)
        if ([RealPath(w.path) isEqualToString:real]) return w;
    return nil;
}

- (NSArray<NSString *> *)worktreeRoots {
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    for (MCAgentWorktree *w in self.worktrees) [out addObject:w.path];
    // Before git has answered (or outside a repository), the folders Claude
    // Code makes for worktrees are worktrees all the same.
    NSString *dir = [self.folder stringByAppendingPathComponent:@".claude/worktrees"];
    for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil]) {
        if ([name hasPrefix:@"."]) continue;
        NSString *p = [dir stringByAppendingPathComponent:name];
        BOOL isDir = NO;
        if ([NSFileManager.defaultManager fileExistsAtPath:p isDirectory:&isDir] && isDir &&
            ![out containsObject:p])
            [out addObject:p];
    }
    for (NSString *p in self.scratchpads)
        if (![out containsObject:p]) [out addObject:p];
    return out;
}

// ------------------------------------------------------- compare with main
- (void)changesInWorktree:(MCAgentWorktree *)worktree
               completion:(void (^)(NSArray<MCWorktreeChange *> *, NSString *))done {
    NSString *main = self.mainBranch;
    NSString *path = worktree.path;
    if (!main.length || !path.length) {
        done(@[], @"There is no main branch to compare with.");
        return;
    }
    dispatch_async(_gitQueue, ^{
        NSString *error = nil;
        std::vector<AgentView::Change> changes;
        MCGitResult *base = MCGitRunSync(path, @[@"merge-base", main, @"HEAD"]);
        NSString *baseRef = base.ok
            ? [[[NSString alloc] initWithData:base.output encoding:NSUTF8StringEncoding]
                  stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
            : main;
        MCGitResult *diff = MCGitRunSync(
            path, @[@"-c", @"core.quotePath=false", @"diff", @"--name-status", @"-z", @"-M",
                    @"--no-ext-diff", baseRef ?: main, @"--"]);
        MCGitResult *untracked =
            MCGitRunSync(path, @[@"ls-files", @"--others", @"--exclude-standard", @"-z"]);
        if (!diff.ok) {
            error = diff.errorText.length ? diff.errorText : @"git diff failed.";
        } else {
            std::string ns((const char *)diff.output.bytes, diff.output.length);
            std::string un = untracked.ok
                ? std::string((const char *)untracked.output.bytes, untracked.output.length)
                : std::string();
            changes = AgentView::parseChanges(ns, un);
        }
        NSMutableArray<MCWorktreeChange *> *out = [NSMutableArray array];
        for (const AgentView::Change &c : changes) {
            MCWorktreeChange *x = [MCWorktreeChange new];
            x.status = (unichar)c.status;
            x.path = NS(c.path);
            x.from = NSOrNil(c.from);
            [out addObject:x];
        }
        dispatch_async(dispatch_get_main_queue(), ^{ done(out, error); });
    });
}

static NSString *TextOf(NSData *data) {
    if (!data) return @"";
    if (memchr(data.bytes, 0, MIN(data.length, (NSUInteger)8192)))
        return @"(a binary file)";
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]
        ?: [[NSString alloc] initWithData:data encoding:NSISOLatin1StringEncoding] ?: @"";
}

- (void)comparisonForChange:(MCWorktreeChange *)change
                 inWorktree:(MCAgentWorktree *)worktree
                 completion:(void (^)(MCComparison *))done {
    NSString *main = self.mainBranch ?: @"main";
    NSString *wt = worktree.path;
    NSString *label = worktree.label ?: wt.lastPathComponent;
    NSString *branch = worktree.branch ?: wt.lastPathComponent;
    dispatch_async(_gitQueue, ^{
        NSString *oldPath = change.from ?: change.path;
        NSString *left = @"", *right = @"";
        BOOL onMain = change.status != 'A' && change.status != '?';
        if (onMain) {
            MCGitResult *show = MCGitRunSync(
                wt, @[@"show", [NSString stringWithFormat:@"%@:%@", main, oldPath]]);
            if (show.ok) left = TextOf(show.output);
            else onMain = NO;
        }
        NSString *file = [wt stringByAppendingPathComponent:change.path];
        BOOL onDisk = change.status != 'D' &&
                      [NSFileManager.defaultManager fileExistsAtPath:file];
        if (onDisk) right = TextOf([NSData dataWithContentsOfFile:file]);
        MCComparison *c = [MCComparison new];
        c.title = [NSString stringWithFormat:@"%@ (%@ vs %@)", change.path.lastPathComponent,
                   label.length > 40 ? [[label substringToIndex:40] stringByAppendingString:@"…"]
                                     : label,
                   main];
        c.leftLabel = onMain ? [NSString stringWithFormat:@"%@: %@", main, oldPath]
                             : [NSString stringWithFormat:@"%@: (not there)", main];
        c.rightLabel = onDisk ? [NSString stringWithFormat:@"%@: %@", branch, change.path]
                              : [NSString stringWithFormat:@"%@: (deleted)", branch];
        c.leftText = left;
        c.rightText = right;
        c.path = onDisk ? file : nil;
        dispatch_async(dispatch_get_main_queue(), ^{ done(c); });
    });
}

@end
