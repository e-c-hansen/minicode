// TreeGitColors.mm — see TreeGitColors.h.
#import "TreeGitColors.h"
#import "GitPanel.h"
#include <memory>
#include <stdlib.h>

namespace {
std::string MCUtf8(NSString *s) { return s.UTF8String ?: ""; }
}  // namespace

@implementation MCTreeGitColors {
    dispatch_queue_t _queue;
    NSString *_root;           // as the tree spells it
    NSString *_realRoot;       // symbolic links resolved, as FSEvents and git spell it
    NSString *_topLevel;       // the repository's top level, nil when not in one
    NSString *_prefix;         // the root from the top level ("src/" or "")
    BOOL _looked;              // a look finished for this root
    std::shared_ptr<const Git::Decorations> _decorations;
    NSUInteger _generation;
    BOOL _running, _again, _scheduled;
    NSUInteger _runs;
}

- (instancetype)init {
    if ((self = [super init]))
        _queue = dispatch_queue_create("minicode.tree-git",
                                       dispatch_queue_attr_make_with_qos_class(
                                           DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    return self;
}

- (NSUInteger)runs { return _runs; }
- (BOOL)inRepository { return _topLevel != nil; }

- (void)setRoot:(NSString *)root {
    if (root == _root || [root isEqualToString:_root]) return;
    _generation++;
    _root = [root copy];
    char real[PATH_MAX];
    _realRoot = root && realpath(root.fileSystemRepresentation, real) ? @(real) : root;
    _topLevel = nil;
    _prefix = @"";
    _looked = NO;
    _again = NO;
    const BOOL had = _decorations && !_decorations->empty();
    _decorations.reset();
    if (had && self.onChange) self.onChange();
    if (root) [self look];
}

- (void)stop {
    self.onChange = nil;
    [self setRoot:nil];
}

// The path from the top level, nil when it is not inside it.
static NSString *MCRelativeTo(NSString *path, NSString *top) {
    while (path.length > 1 && [path hasSuffix:@"/"]) path = [path substringToIndex:path.length - 1];
    if ([path isEqualToString:top]) return @"";
    NSString *prefix = [top hasSuffix:@"/"] ? top : [top stringByAppendingString:@"/"];
    return [path hasPrefix:prefix] ? [path substringFromIndex:prefix.length] : nil;
}

- (void)changedOnDisk:(NSArray<NSString *> *)paths {
    if (!_root) return;
    BOOL wanted = NO;
    for (NSString *p in paths) {
        if (!_looked) { wanted = YES; break; }
        if (!_topLevel) {
            // Not in a repository: only a git init in the folder itself
            // (its own top folder changes) can make it one.
            NSString *q = p;
            while (q.length > 1 && [q hasSuffix:@"/"]) q = [q substringToIndex:q.length - 1];
            if ([q isEqualToString:_realRoot]) { wanted = YES; break; }
            continue;
        }
        NSString *rel = MCRelativeTo(p, _topLevel);
        if (!rel) continue;
        // git writes these while it works; the index, HEAD and refs, which
        // change what status says, are elsewhere in .git.
        if ([rel isEqualToString:@".git/objects"] || [rel hasPrefix:@".git/objects/"] ||
            [rel isEqualToString:@".git/logs"] || [rel hasPrefix:@".git/logs/"])
            continue;
        if (rel.length && _decorations && _decorations->ignored(MCUtf8(rel))) continue;
        wanted = YES;
        break;
    }
    if (wanted) [self schedule];
}

// Half a second after the first change, so a burst of writes costs one
// status; while one runs, another follows it.
- (void)schedule {
    if (_running) { _again = YES; return; }
    if (_scheduled) return;
    _scheduled = YES;
    const NSUInteger gen = _generation;
    __weak MCTreeGitColors *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        MCTreeGitColors *s = weakSelf;
        if (!s) return;
        s->_scheduled = NO;
        if (gen == s->_generation) [s look];
    });
}

- (void)look {
    if (!_root) return;
    if (_running) { _again = YES; return; }
    _running = YES;
    const NSUInteger gen = _generation;
    NSString *realRoot = _realRoot, *knownTop = _topLevel, *knownPrefix = _prefix;
    const std::shared_ptr<const Git::Decorations> old = _decorations;   // compared off the main thread
    __weak MCTreeGitColors *weakSelf = self;
    dispatch_async(_queue, ^{
        NSString *top = knownTop, *prefix = knownPrefix ?: @"";
        if (!top) {
            MCGitResult *r = MCGitRunSync(realRoot, @[@"rev-parse", @"--show-toplevel", @"--show-prefix"]);
            if (r.ok) {
                NSString *out = [[NSString alloc] initWithData:r.output encoding:NSUTF8StringEncoding];
                NSArray<NSString *> *lines = [out componentsSeparatedByString:@"\n"];
                if (lines.count >= 1 && lines[0].length) {
                    top = lines[0];
                    prefix = lines.count >= 2 ? lines[1] : @"";
                }
            }
        }
        std::shared_ptr<const Git::Decorations> decorations;
        if (top) {
            NSMutableArray<NSString *> *args = [NSMutableArray array];
            for (const std::string &a : Git::decorationArgs()) [args addObject:@(a.c_str())];
            MCGitResult *st = MCGitRunSync(top, args);
            if (st.ok) {
                const std::string bytes((const char *)st.output.bytes, st.output.length);
                decorations = std::make_shared<const Git::Decorations>(
                    Git::Decorations::fromStatus(Git::parseStatus(bytes)));
            } else {
                top = nil;   // the repository went (or status failed): look again next time
                prefix = @"";
            }
        }
        const bool changed = (bool)decorations != (bool)old || (decorations && *decorations != *old);
        dispatch_async(dispatch_get_main_queue(), ^{
            MCTreeGitColors *s = weakSelf;
            if (!s) return;
            s->_running = NO;
            if (gen != s->_generation) {
                if (s->_root) [s look];   // a new folder came while this ran
                return;
            }
            s->_looked = YES;
            s->_runs++;
            s->_topLevel = top;
            s->_prefix = prefix;
            s->_decorations = decorations;
            if (changed && s.onChange) s.onChange();
            if (s->_again) {
                s->_again = NO;
                [s schedule];
            }
        });
    });
}

- (Git::Decoration)decorationForPath:(NSString *)path isDir:(BOOL)isDir {
    if (!_decorations || !_root || !path) return Git::Decoration::None;
    NSString *tail = MCRelativeTo(path, _root);
    if (!tail) return Git::Decoration::None;
    // The agents' worktrees are repositories of their own.
    if ([tail isEqualToString:@".claude/worktrees"] || [tail hasPrefix:@".claude/worktrees/"])
        return Git::Decoration::None;
    std::string rel = MCUtf8(_prefix) + MCUtf8(tail);
    while (!rel.empty() && rel.back() == '/') rel.pop_back();
    return isDir ? _decorations->folder(rel) : _decorations->file(rel);
}

@end
