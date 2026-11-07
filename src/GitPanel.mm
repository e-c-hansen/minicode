// GitPanel.mm — Objective-C++. See GitPanel.h.
#import "GitPanel.h"
#import "EditorFont.h"
#import "AppSettings.h"
#import "Appearance.h"
#import "Lsp.h"   // MCFindProgram, MCSearchDirs
#include "GitStatus.h"
#include "GitGraph.h"
#include "GitModel.h"
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static NSColor *GHex(unsigned int rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1.0];
}

static NSString *NSFromBytes(const std::string &s) {
    NSString *r = [[NSString alloc] initWithBytes:s.data() length:s.size()
                                         encoding:NSUTF8StringEncoding];
    // Not UTF-8 (a Latin-1 file, a path in some old encoding): show the
    // bytes one per character rather than nothing.
    return r ?: [[NSString alloc] initWithBytes:s.data() length:s.size()
                                       encoding:NSISOLatin1StringEncoding];
}

// The core's argument vectors (GitModel.h) as MCGitRunSync takes them.
static NSArray<NSString *> *NSArgs(const std::vector<std::string> &v) {
    NSMutableArray *out = [NSMutableArray arrayWithCapacity:v.size()];
    for (const std::string &a : v) [out addObject:NSFromBytes(a) ?: @""];
    return out;
}
static std::string Bytes(NSData *d) {
    return d.length ? std::string((const char *)d.bytes, d.length) : std::string();
}

// ------------------------------------------------------------- running git
@implementation MCGitResult
- (BOOL)ok { return self.status == 0; }
@end

NSString *MCGitExecutable(void) {
    static NSString *git;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ git = MCFindProgram(@"git"); });
    return git;
}

static std::vector<std::string> MCGitStdArgs(NSArray<NSString *> *args) {
    std::vector<std::string> out;
    for (NSString *a in args) out.push_back(a.UTF8String ?: "");
    return out;
}

// The limits and their messages are the core's (Git::timeoutFor), shared
// with the GTK port.
NSTimeInterval MCGitTimeoutFor(NSArray<NSString *> *args) {
    return Git::timeoutFor(MCGitStdArgs(args));
}

// Reads `fd` to its end into `into`, then closes it.
static void MCGitDrain(int fd, NSMutableData *into) {
    char buf[65536];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) [into appendBytes:buf length:(NSUInteger)n];
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    close(fd);
}

MCGitResult *MCGitRunSync(NSString *dir, NSArray<NSString *> *args) {
    return MCGitRunSyncWithTimeout(dir, args, MCGitTimeoutFor(args));
}

// posix_spawn rather than NSTask, for the one thing NSTask cannot do: start
// git in a process group of its own, so a timeout stops the hooks, filters
// and credential helpers it started along with it.
MCGitResult *MCGitRunSyncWithTimeout(NSString *dir, NSArray<NSString *> *args,
                                     NSTimeInterval timeout) {
    MCGitResult *r = [MCGitResult new];
    r.status = -1;
    r.output = [NSData data];
    NSString *git = MCGitExecutable();
    if (!git) {
        r.errorText = @"Git was not found. Install the command line tools "
                       "(xcode-select --install) or git from Homebrew.";
        return r;
    }
    NSMutableDictionary *env = [NSProcessInfo.processInfo.environment mutableCopy];
    env[@"PATH"] = [MCSearchDirs() componentsJoinedByString:@":"];
    env[@"GIT_TERMINAL_PROMPT"] = @"0";   // never wait for a password
    env[@"GIT_EDITOR"] = @"true";         // never wait for an editor
    env[@"GIT_OPTIONAL_LOCKS"] = @"0";    // status must not write the index
    env[@"GIT_PAGER"] = @"cat";

    // Everything the child needs, built before the spawn.
    std::vector<std::string> argStore, envStore;
    argStore.push_back(git.fileSystemRepresentation);
    for (NSString *a in args) argStore.push_back(a.UTF8String ?: "");
    for (NSString *k in env)
        envStore.push_back(std::string(k.UTF8String ?: "") + "=" +
                           ([env[k] UTF8String] ?: ""));
    std::vector<char *> argv, envp;
    for (std::string &a : argStore) argv.push_back(a.data());
    argv.push_back(nullptr);
    for (std::string &e : envStore) envp.push_back(e.data());
    envp.push_back(nullptr);

    int outFds[2], errFds[2];
    if (pipe(outFds) != 0) {
        r.errorText = @"Git could not be started (no pipe).";
        return r;
    }
    if (pipe(errFds) != 0) {
        close(outFds[0]);
        close(outFds[1]);
        r.errorText = @"Git could not be started (no pipe).";
        return r;
    }
    for (int fd : {outFds[0], outFds[1], errFds[0], errFds[1]})
        fcntl(fd, F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, outFds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, errFds[1], 2);
    // The _np name is deprecated in macOS 26 for posix_spawn_file_actions_addchdir,
    // which macOS 11 to 15 do not have; the old name still works on all.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    posix_spawn_file_actions_addchdir_np(&fa, dir.fileSystemRepresentation);
#pragma clang diagnostic pop
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    sigdelset(&all, SIGKILL);
    sigdelset(&all, SIGSTOP);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    // A group of its own, led by git. A command that talks to a remote gets
    // a session of its own too (its group is then the same), so it has no
    // controlling terminal: ssh cannot ask for a passphrase or a host key on
    // the terminal minicode was started from, and fails at once instead.
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_CLOEXEC_DEFAULT;
    if (Git::talksToRemote(MCGitStdArgs(args))) {
        flags |= POSIX_SPAWN_SETSID;
    } else {
        posix_spawnattr_setpgroup(&attr, 0);
        flags |= POSIX_SPAWN_SETPGROUP;
    }
    posix_spawnattr_setflags(&attr, flags);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, argv[0], &fa, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(outFds[1]);
    close(errFds[1]);
    if (rc != 0) {
        close(outFds[0]);
        close(errFds[0]);
        r.errorText = [NSString stringWithFormat:@"Git could not be started: %s.",
                                                 strerror(rc)];
        return r;
    }

    // Both pipes are drained at once, or a chatty stderr could fill its pipe
    // and stall git while we wait on stdout. Each reader owns its descriptor,
    // so one given up on below closes it whenever its writer goes.
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    NSMutableData *outData = [NSMutableData data], *errData = [NSMutableData data];
    dispatch_group_t io = dispatch_group_create();
    const int outRead = outFds[0], errRead = errFds[0];
    dispatch_group_async(io, q, ^{ MCGitDrain(outRead, outData); });
    dispatch_group_async(io, q, ^{ MCGitDrain(errRead, errData); });
    // Reaped on a thread of its own: a git stuck in the kernel (a dead NFS
    // server) outlives even SIGKILL, and then only that thread waits on it.
    __block int waitStatus = 0;
    dispatch_semaphore_t exited = dispatch_semaphore_create(0);
    dispatch_async(q, ^{
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        waitStatus = st;
        dispatch_semaphore_signal(exited);
    });

    auto after = [](double seconds) {
        return dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC));
    };
    const dispatch_time_t deadline = after(timeout);
    if (dispatch_semaphore_wait(exited, deadline) != 0) {
        r.timedOut = YES;
        r.errorText = NSFromBytes(Git::timeoutMessage(MCGitStdArgs(args), timeout));
        kill(-pid, SIGTERM);
        if (dispatch_semaphore_wait(exited, after(2)) != 0) {
            kill(-pid, SIGKILL);
            dispatch_semaphore_wait(exited, after(2));
        } else {
            kill(-pid, SIGKILL);   // what git started and left in its group
        }
        return r;
    }
    // git exited. Its output follows at once, unless something it started
    // (outside its group, or still running in it) holds the pipes open,
    // which is a hang of its own.
    if (dispatch_group_wait(io, deadline) != 0 && dispatch_group_wait(io, after(2)) != 0) {
        r.timedOut = YES;
        r.errorText = NSFromBytes(Git::heldOutputMessage(MCGitStdArgs(args), timeout));
        kill(-pid, SIGKILL);
        return r;
    }
    if (WIFEXITED(waitStatus)) r.status = WEXITSTATUS(waitStatus);
    else if (WIFSIGNALED(waitStatus)) r.status = 128 + WTERMSIG(waitStatus);
    r.output = outData;
    std::string e((const char *)errData.bytes, errData.length);
    r.errorText = [NSFromBytes(e) stringByTrimmingCharactersInSet:
                      NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return r;
}

// What to tell the user about a failed command (GitUi::failureText).
static NSString *MCGitFailureText(MCGitResult *r) {
    return NSFromBytes(GitUi::failureText(r.status, Bytes(r.output),
                                          (r.errorText ?: @"").UTF8String ?: ""));
}

// ------------------------------------------------------------- diff colors
// A commit's date in the user's locale and format, for GitUi's texts.
static std::string MCGitDate(long long t) {
    static NSDateFormatter *f;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        f = [NSDateFormatter new];
        f.dateStyle = NSDateFormatterMediumStyle;
        f.timeStyle = NSDateFormatterShortStyle;
    });
    return [f stringFromDate:[NSDate dateWithTimeIntervalSince1970:(double)t]].UTF8String ?: "";
}

static long long MCGitNow(void) { return (long long)NSDate.date.timeIntervalSince1970; }

// The core's styled text (GitUi::StyledText, runs counted in characters) as
// an attributed string: added green, removed red, muted grey, file headers
// muted bold, a hash amber, a subject bold.
static NSAttributedString *MCGitStyledText(const GitUi::StyledText &t, NSFont *font) {
    AppSettings *cfg = [AppSettings shared];
    NSColor *plain = [cfg text:Surface::Editor];
    NSColor *muted = GHex(0x9CA3AF);
    NSFont *bold = [[NSFontManager sharedFontManager] convertFont:font
                                                     toHaveTrait:NSBoldFontMask];
    NSString *text = [[NSString alloc] initWithBytes:t.text.data() length:t.text.size()
                                            encoding:NSUTF8StringEncoding] ?: @"";
    NSMutableAttributedString *out = [[NSMutableAttributedString alloc]
        initWithString:text
            attributes:@{NSFontAttributeName: font, NSForegroundColorAttributeName: plain}];
    // Characters to UTF-16 units in one walk of the (valid) UTF-8.
    size_t byte = 0, ch = 0;
    NSUInteger unit = 0;
    auto advanceTo = [&](size_t target) {
        while (ch < target && byte < t.text.size()) {
            const unsigned char c = (unsigned char)t.text[byte];
            const size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            byte += n;
            unit += n == 4 ? 2 : 1;
            ch++;
        }
    };
    NSColor *added = GHex(0x73C991), *removed = GHex(0xF14C4C), *hash = GHex(0xE2C08D);
    for (const GitUi::StyledText::Run &r : t.runs) {
        advanceTo(r.start);
        const NSUInteger from = unit;
        advanceTo(r.start + r.length);
        const NSRange range = NSMakeRange(from, MIN(unit, out.length) - MIN(from, out.length));
        switch (r.style) {
        case GitUi::Style::Plain: break;
        case GitUi::Style::Added:
            [out addAttribute:NSForegroundColorAttributeName value:added range:range];
            break;
        case GitUi::Style::Removed:
            [out addAttribute:NSForegroundColorAttributeName value:removed range:range];
            break;
        case GitUi::Style::Muted:
            [out addAttribute:NSForegroundColorAttributeName value:muted range:range];
            break;
        case GitUi::Style::FileHeader:
            [out addAttributes:@{NSForegroundColorAttributeName: muted,
                                 NSFontAttributeName: bold} range:range];
            break;
        case GitUi::Style::Hash:
            [out addAttribute:NSForegroundColorAttributeName value:hash range:range];
            break;
        case GitUi::Style::Bold:
            [out addAttribute:NSFontAttributeName value:bold range:range];
            break;
        }
    }
    return out;
}

NSAttributedString *MCGitDiffText(NSData *diff, NSFont *font) {
    return MCGitStyledText(GitUi::diffText(Bytes(diff)), font);
}

NSAttributedString *MCGitCommitText(NSData *show, NSFont *font) {
    return MCGitStyledText(GitUi::commitText(Bytes(show), MCGitNow(), MCGitDate), font);
}

// ------------------------------------------------------------------- rows
// One line of the change lists: the core's row (GitUi::Row), with its text
// as NSStrings for drawing.
@interface MCGitRow : NSObject {
@public
    GitUi::Row model;
}
- (instancetype)initWithModel:(const GitUi::Row &)row;
@property(nonatomic, readonly) BOOL header, staged, untracked, unmerged;
@property(nonatomic, readonly) unichar letter;
@property(nonatomic, readonly, copy) NSString *title;      // a heading's text
@property(nonatomic, readonly, copy) NSString *path;       // relative to the top level
@property(nonatomic, readonly, copy) NSString *origPath;   // a rename's source, or nil
@end
@implementation MCGitRow
- (instancetype)initWithModel:(const GitUi::Row &)row {
    if ((self = [super init])) {
        model = row;
        _title = row.header ? NSFromBytes(row.title) : nil;
        _path = row.path.empty() ? nil : NSFromBytes(row.path);
        _origPath = row.origPath.empty() ? nil : NSFromBytes(row.origPath);
    }
    return self;
}
- (BOOL)header { return model.header; }
- (BOOL)staged { return model.staged; }
- (BOOL)untracked { return model.untracked; }
- (BOOL)unmerged { return model.unmerged; }
- (unichar)letter { return (unichar)(unsigned char)model.letter; }
@end

static std::vector<GitUi::Row> ModelRows(NSArray<MCGitRow *> *rows) {
    std::vector<GitUi::Row> out;
    out.reserve(rows.count);
    for (MCGitRow *r in rows) out.push_back(r->model);
    return out;
}

// What one refresh found, built off the main thread: the core's snapshot.
@interface MCGitSnapshot : NSObject {
@public
    GitUi::Snapshot model;
}
@property(nonatomic, copy) NSArray<MCGitRow *> *rows;
@property(nonatomic, readonly) BOOL inRepository, initial;
@property(nonatomic, readonly, copy) NSString *topLevel, *branchText, *errorText, *notice;
@end
@implementation MCGitSnapshot
static NSString *NonEmpty(const std::string &s) { return s.empty() ? nil : NSFromBytes(s); }
- (BOOL)inRepository { return model.inRepository; }
- (BOOL)initial { return model.initial; }
- (NSString *)topLevel { return NonEmpty(model.topLevel); }
- (NSString *)branchText { return NonEmpty(model.branchText); }
- (NSString *)errorText { return NonEmpty(model.errorText); }
- (NSString *)notice { return NonEmpty(model.notice); }
@end

// ------------------------------------------------------------------ graph
// One load of the commit graph, built off the main thread and not changed
// after: the core's GitUi::Graph (commits, lanes, labels, what is pushed).
@interface MCGitGraph : NSObject {
@public
    GitUi::Graph g;
}
@property(nonatomic, readonly) BOOL hasMore;        // the limit cut the history short
@property(nonatomic, readonly, copy) NSString *errorText;
@end
@implementation MCGitGraph
- (BOOL)hasMore { return g.hasMore; }
- (NSString *)errorText { return NonEmpty(g.errorText); }
@end

static const NSInteger kGraphBatch = GitUi::kGraphBatch;

// Lane colors, the accent blue first (HEAD's lane is usually first).
static NSColor *LaneColor(int i) { return GHex(GitUi::laneColor(i)); }

// One row of the graph, drawn by hand: its lines and dot, the ref labels as
// pills, the subject, and an arrow at the right for a commit to push or pull.
@interface MCGitGraphCell : NSView
@property(nonatomic, strong) MCGitGraph *graph;
@property(nonatomic, assign) NSInteger index;     // == commits.size(): the "Show more" row
@property(nonatomic, strong) NSColor *textColor, *panelColor;
@end
@implementation MCGitGraphCell
- (BOOL)isFlipped { return YES; }

// A label: filled for HEAD's own branch (or a detached HEAD), outlined for
// the rest, blue for local branches, purple for remote ones, amber for tags.
static CGFloat DrawPill(const Git::Ref &r, CGFloat x, CGFloat midY, CGFloat maxW) {
    NSColor *c = GHex(GitUi::pillColor(r.kind));
    NSString *name = NSFromBytes(r.name);
    if (r.kind == Git::RefKind::Tag) name = [@"tag " stringByAppendingString:name];
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineBreakMode = NSLineBreakByTruncatingMiddle;
    NSDictionary *a = @{
        NSFontAttributeName: [NSFont systemFontOfSize:10 weight:NSFontWeightSemibold],
        NSForegroundColorAttributeName: r.current ? GHex(0x1E1E1E) : c,
        NSParagraphStyleAttributeName: ps,
    };
    CGFloat textW = MIN(ceil([name sizeWithAttributes:a].width), MAX(0, maxW - 10));
    if (textW < 12) return 0;
    NSRect box = NSMakeRect(x, midY - 7, textW + 10, 14);
    NSBezierPath *p = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(box, 0.5, 0.5)
                                                      xRadius:6.5 yRadius:6.5];
    if (r.current) {
        [c setFill];
        [p fill];
    } else {
        [[c colorWithAlphaComponent:0.14] setFill];
        [p fill];
        [[c colorWithAlphaComponent:0.7] setStroke];
        p.lineWidth = 1;
        [p stroke];
    }
    [name drawWithRect:NSMakeRect(x + 5, midY - 7 + 1, textW, 13)
               options:NSStringDrawingUsesLineFragmentOrigin |
                       NSStringDrawingTruncatesLastVisibleLine
            attributes:a];
    return box.size.width;
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    MCGitGraph *g = self.graph;
    if (!g) return;
    const NSRect b = self.bounds;
    const CGFloat h = b.size.height, mid = floor(h / 2);
    NSColor *muted = GHex(0x9CA3AF);
    if (self.index >= (NSInteger)g->g.commits.size()) {
        [[NSString stringWithFormat:@"Show %ld more…", (long)kGraphBatch]
            drawAtPoint:NSMakePoint(18, 3)
         withAttributes:@{NSFontAttributeName: [NSFont systemFontOfSize:12],
                          NSForegroundColorAttributeName: GHex(0x4EA1F7)}];
        return;
    }
    const Git::Commit &c = g->g.commits[self.index];
    const Git::GraphRow &row = g->g.rows[self.index];
    const bool outgoing = g->g.divergence.outgoing.count(c.hash) > 0;
    const bool incoming = g->g.divergence.incoming.count(c.hash) > 0;
    const CGFloat lw = GitUi::laneWidth(b.size.width, g->g.maxWidth);
    auto X = [&](int lane) { return 8 + lane * lw + lw / 2; };

    if (outgoing || incoming) {
        [[(outgoing ? GHex(0x4EA1F7) : GHex(0x73C991)) colorWithAlphaComponent:0.09] setFill];
        NSRectFillUsingOperation(b, NSCompositingOperationSourceOver);
    }

    // Lines: straight down within a lane, curves between lanes, meeting the
    // dot sideways the way VS Code's graph draws a merge or a branch-off.
    for (const Git::GraphEdge &e : row.edges) {
        NSBezierPath *p = [NSBezierPath bezierPath];
        p.lineWidth = 1.5;
        p.lineCapStyle = NSLineCapStyleRound;
        CGFloat x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        switch (e.kind) {
        case Git::GraphEdge::Pass: x0 = X(e.from); y0 = 0; x1 = X(e.to); y1 = h; break;
        case Git::GraphEdge::In: x0 = X(e.from); y0 = 0; x1 = X(row.lane); y1 = mid; break;
        case Git::GraphEdge::Out: x0 = X(row.lane); y0 = mid; x1 = X(e.to); y1 = h; break;
        }
        [p moveToPoint:NSMakePoint(x0, y0)];
        if (x0 == x1) {
            [p lineToPoint:NSMakePoint(x1, y1)];
        } else if (e.kind == Git::GraphEdge::Pass) {
            [p curveToPoint:NSMakePoint(x1, y1) controlPoint1:NSMakePoint(x0, mid)
              controlPoint2:NSMakePoint(x1, mid)];
        } else if (e.kind == Git::GraphEdge::In) {
            [p curveToPoint:NSMakePoint(x1, y1) controlPoint1:NSMakePoint(x0, y1)
              controlPoint2:NSMakePoint(x0, y1)];
        } else {
            [p curveToPoint:NSMakePoint(x1, y1) controlPoint1:NSMakePoint(x1, y0)
              controlPoint2:NSMakePoint(x1, y0)];
        }
        [LaneColor(e.color) setStroke];
        [p stroke];
    }

    // The dot: filled; hollow when not pushed or not pulled yet; a merge has
    // a hole in the middle; HEAD gets a ring around it.
    NSColor *dc = LaneColor(row.color);
    NSColor *bg = self.panelColor ?: GHex(0x252526);
    const CGFloat cx = X(row.lane);
    auto circle = [&](CGFloat r) {
        return [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx - r, mid - r, 2 * r, 2 * r)];
    };
    const bool merge = c.parents.size() > 1;
    if (c.hash == g->g.head) {
        [bg setFill];
        [circle(6.5) fill];
        NSBezierPath *ring = circle(6);
        ring.lineWidth = 1.3;
        [dc setStroke];
        [ring stroke];
    }
    [bg setFill];
    [circle(5) fill];
    if (outgoing || incoming) {
        NSBezierPath *ring = circle(3.4);
        ring.lineWidth = 1.6;
        [dc setStroke];
        [ring stroke];
        if (merge) { [dc setFill]; [circle(1.4) fill]; }
    } else {
        [dc setFill];
        [circle(4) fill];
        if (merge) { [bg setFill]; [circle(1.6) fill]; }
    }

    // Labels, then the subject.
    CGFloat x = 8 + row.width * lw + 6;
    const CGFloat right = b.size.width - ((outgoing || incoming) ? 20 : 6);
    auto it = g->g.labels.find(c.hash);
    if (it != g->g.labels.end()) {
        size_t shown = 0;
        for (const Git::Ref &r : it->second) {
            CGFloat room = MIN(120.0, right - x - 40);
            if (room < 30) break;
            CGFloat w = DrawPill(r, x, mid, room);
            if (w <= 0) break;
            x += w + 4;
            shown++;
        }
        if (shown < it->second.size() && right - x > 20) {
            NSString *more = [NSString stringWithFormat:@"+%zu", it->second.size() - shown];
            [more drawAtPoint:NSMakePoint(x, mid - 7)
               withAttributes:@{NSFontAttributeName: [NSFont systemFontOfSize:10],
                                NSForegroundColorAttributeName: muted}];
            x += [more sizeWithAttributes:@{NSFontAttributeName: [NSFont systemFontOfSize:10]}]
                     .width + 4;
        }
    }
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineBreakMode = NSLineBreakByTruncatingTail;
    if (right - x > 8) {
        [NSFromBytes(c.subject) drawWithRect:NSMakeRect(x, mid - 8, right - x, 17)
                                     options:NSStringDrawingUsesLineFragmentOrigin |
                                             NSStringDrawingTruncatesLastVisibleLine
                                  attributes:@{
                                      NSFontAttributeName: [NSFont systemFontOfSize:12.5],
                                      NSForegroundColorAttributeName:
                                          incoming ? muted : (self.textColor ?: NSColor.textColor),
                                      NSParagraphStyleAttributeName: ps,
                                  }];
    }
    if (outgoing || incoming) {
        [(outgoing ? @"↑" : @"↓") drawAtPoint:NSMakePoint(b.size.width - 16, mid - 8)
                                withAttributes:@{
                                    NSFontAttributeName: [NSFont systemFontOfSize:12
                                                                           weight:NSFontWeightSemibold],
                                    NSForegroundColorAttributeName:
                                        outgoing ? GHex(0x4EA1F7) : GHex(0x73C991),
                                }];
    }
}
@end

static NSColor *LetterColor(unichar c, BOOL unmerged) {
    return GHex(GitUi::letterColor((char)c, unmerged));
}

// One line of the list, drawn by hand: a file's name, its folder (muted)
// and its status letter at the right; or a section heading.
@interface MCGitCell : NSView
@property(nonatomic, strong) MCGitRow *row;
@property(nonatomic, strong) NSColor *textColor;
@end
@implementation MCGitCell
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect b = self.bounds;
    MCGitRow *r = self.row;
    if (!r) return;
    NSColor *muted = GHex(0x9CA3AF);
    if (r.header) {
        NSDictionary *a = @{
            NSFontAttributeName: [NSFont systemFontOfSize:10 weight:NSFontWeightSemibold],
            NSForegroundColorAttributeName: muted,
            NSKernAttributeName: @0.6,
        };
        [r.title.uppercaseString drawWithRect:NSMakeRect(10, 5, b.size.width - 20, 16)
                                      options:NSStringDrawingUsesLineFragmentOrigin |
                                              NSStringDrawingTruncatesLastVisibleLine
                                   attributes:a];
        return;
    }
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineBreakMode = NSLineBreakByTruncatingTail;
    NSString *name = r.path.lastPathComponent;
    NSString *dir = r.path.stringByDeletingLastPathComponent;
    if (r.origPath.length)
        dir = [NSString stringWithFormat:@"%@%@from %@", dir, dir.length ? @"  " : @"",
               r.origPath];
    NSMutableAttributedString *s = [[NSMutableAttributedString alloc]
        initWithString:name ?: @""
            attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:12.5],
                         NSForegroundColorAttributeName: self.textColor ?: NSColor.textColor,
                         NSParagraphStyleAttributeName: ps}];
    if (dir.length)
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:[@"  " stringByAppendingString:dir]
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11],
                             NSForegroundColorAttributeName: muted,
                             NSParagraphStyleAttributeName: ps}]];
    const CGFloat letterW = 22;
    [s drawWithRect:NSMakeRect(18, 3, MAX(0, b.size.width - 18 - letterW - 4), 17)
            options:NSStringDrawingUsesLineFragmentOrigin |
                    NSStringDrawingTruncatesLastVisibleLine];
    unichar ch = r.letter;
    NSString *letter = [NSString stringWithCharacters:&ch length:1];
    [letter drawAtPoint:NSMakePoint(b.size.width - letterW, 3)
         withAttributes:@{
             NSFontAttributeName: MCMonoFont(12, NSFontWeightSemibold),
             NSForegroundColorAttributeName: LetterColor(r.letter, r.unmerged),
         }];
}
@end

// The list: Return shows a diff, Space stages or unstages, arrows skip the
// headings, Tab goes to the message. A click acts from mouseDown, like the
// file tree, since table actions were not reliable there.
@interface MCGitTable : NSTableView
@property(nonatomic, copy) void (^onActivate)(NSInteger row);
@property(nonatomic, copy) void (^onToggle)(NSInteger row);
@property(nonatomic, copy) void (^onClick)(NSInteger row);
@property(nonatomic, copy) void (^onTab)(void);
@property(nonatomic, copy) void (^onBackTab)(void);            // Shift+Tab; else onTab
@property(nonatomic, copy) void (^onPastEnd)(NSInteger step);  // an arrow off either end
@property(nonatomic, copy) BOOL (^selectable)(NSInteger row);
@property(nonatomic, copy) NSMenu *(^menuForRow)(NSInteger row);   // a row's right click
- (BOOL)moveBy:(NSInteger)step;   // to the next file row up or down; NO if none
@end
@implementation MCGitTable
- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSInteger row = [self rowAtPoint:p];
    [super mouseDown:event];
    if (row >= 0 && self.onClick && event.clickCount == 1) self.onClick(row);
}
// A file row's menu (Open File, Stage, Discard, ...) above Adjust Settings,
// the row selected as in VS Code; elsewhere only Adjust Settings.
- (NSMenu *)menuForEvent:(NSEvent *)event {
    NSInteger row = event ? [self rowAtPoint:[self convertPoint:event.locationInWindow
                                                       fromView:nil]]
                          : self.selectedRow;
    NSMenu *menu = row >= 0 && self.menuForRow ? self.menuForRow(row) : nil;
    if (!menu) return MCAdjustSettingsMenu(@"git", self);
    [self selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
    MCAddAdjustSettingsItem(menu, @"git", self);
    return menu;
}
- (BOOL)moveBy:(NSInteger)step {
    NSInteger n = self.numberOfRows, r = self.selectedRow;
    if (r < 0) r = step > 0 ? -1 : n;
    for (r += step; r >= 0 && r < n; r += step) {
        if (self.selectable && !self.selectable(r)) continue;
        [self selectRowIndexes:[NSIndexSet indexSetWithIndex:r] byExtendingSelection:NO];
        [self scrollRowToVisible:r];
        return YES;
    }
    return NO;
}
- (void)keyDown:(NSEvent *)event {
    NSEventModifierFlags m = event.modifierFlags &
        (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption);
    NSString *ch = event.charactersIgnoringModifiers;
    unichar c = ch.length ? [ch characterAtIndex:0] : 0;
    if (!m) {
        if ((c == '\r' || c == 3) && self.onActivate) { self.onActivate(self.selectedRow); return; }
        if (c == ' ' && self.onToggle) { self.onToggle(self.selectedRow); return; }
        if (c == 25 && (self.onBackTab || self.onTab)) {
            if (self.onBackTab) self.onBackTab(); else self.onTab();
            return;
        }
        if (c == '\t' && self.onTab) { self.onTab(); return; }
        if (c == NSUpArrowFunctionKey || c == NSDownArrowFunctionKey) {
            NSInteger step = c == NSUpArrowFunctionKey ? -1 : 1;
            if (![self moveBy:step] && self.onPastEnd) self.onPastEnd(step);
            return;
        }
    }
    [super keyDown:event];
}
@end

// The commit message: Command+Return commits, Tab goes back to the list, and
// a muted hint shows while it is empty.
@interface MCCommitTextView : NSTextView
@property(nonatomic, copy) void (^onCommit)(void);
@property(nonatomic, copy) void (^onTab)(void);
@property(nonatomic, copy) void (^onBackTab)(void);
@end
@implementation MCCommitTextView
- (BOOL)isCommandReturn:(NSEvent *)e {
    NSEventModifierFlags m = e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    // Not with Shift: Shift+Cmd+Return is Run File.
    return (m & NSEventModifierFlagCommand) &&
           !(m & (NSEventModifierFlagControl | NSEventModifierFlagOption |
                  NSEventModifierFlagShift)) &&
           (e.keyCode == 36 || e.keyCode == 76);
}
- (BOOL)performKeyEquivalent:(NSEvent *)e {
    if (self.window.firstResponder == self && [self isCommandReturn:e] && self.onCommit) {
        self.onCommit();
        return YES;
    }
    return [super performKeyEquivalent:e];
}
- (void)keyDown:(NSEvent *)e {
    if ([self isCommandReturn:e] && self.onCommit) { self.onCommit(); return; }
    [super keyDown:e];
}
- (void)insertTab:(id)sender { if (self.onTab) self.onTab(); else [super insertTab:sender]; }
- (void)insertBacktab:(id)sender {
    if (self.onBackTab) self.onBackTab();
    else if (self.onTab) self.onTab();
    else [super insertBacktab:sender];
}
- (void)didChangeText { [super didChangeText]; [self setNeedsDisplay:YES]; }
- (NSMenu *)menuForEvent:(NSEvent *)event {
    NSMenu *menu = [super menuForEvent:event];
    MCAddAdjustSettingsItem(menu, @"git", self);
    return menu;
}
- (void)drawRect:(NSRect)dirty {
    [super drawRect:dirty];
    if (self.string.length) return;
    NSPoint p = NSMakePoint(self.textContainerInset.width +
                                self.textContainer.lineFragmentPadding,
                            self.textContainerInset.height);
    [@"Message (⌘⏎ to commit)" drawAtPoint:p withAttributes:@{
        NSFontAttributeName: self.font ?: [NSFont systemFontOfSize:12],
        NSForegroundColorAttributeName: GHex(0x6B7280),
    }];
}
@end

// ------------------------------------------------------- the branch picker
@interface MCGitBranches () {
@public
    std::vector<GitUi::BranchItem> items;
    GitUi::Snapshot snap;   // the status they were read with (branch, remotes)
}
@property(nonatomic, copy) NSString *top;
@property(nonatomic, readwrite, copy) NSArray<NSString *> *labels, *details;
@end
@implementation MCGitBranches
- (NSString *)createLabelFor:(NSString *)typed {
    const std::string label = GitUi::createBranchLabel(typed.UTF8String ?: "", items, snap);
    return label.empty() ? nil : NSFromBytes(label);
}
@end

// ------------------------------------------------------------------- panel
@interface MCGitPanel () <NSTableViewDataSource, NSTableViewDelegate>
@end

// One Discard Changes that can be undone: where the file is, and where the
// Trash put the copy (nil for a deleted file brought back).
@interface MCGitDiscard : NSObject
@property(nonatomic, copy) NSString *path, *rel;
@property(nonatomic, strong) NSURL *trashed;
@end
@implementation MCGitDiscard
@end

@implementation MCGitPanel {
    dispatch_queue_t _queue;          // one git at a time, in order
    BOOL _refreshScheduled;
    NSUInteger _diffGeneration;
    NSArray<MCGitRow *> *_rows;
    BOOL _initial;
    BOOL _errorIsInfo;                // the line under the button is news, not a failure
    BOOL _errorFromStatus;            // ...and it came from a failing status
    NSTextField *_branchLabel;
    NSScrollView *_messageScroll;
    MCCommitTextView *_message;
    NSButton *_commitButton;
    NSTextField *_errorLabel;
    NSScrollView *_listScroll;
    MCGitTable *_table;
    NSTextField *_notice;
    NSColor *_text;
    // The graph, under the change lists.
    BOOL _graphShown;
    NSView *_graphLine;               // the rule above its heading
    NSTextField *_graphHeading;
    NSButton *_allToggle;
    NSTextField *_summaryLabel;       // pushed or not, against the upstream
    NSScrollView *_graphScroll;
    MCGitTable *_graphTable;
    MCGitGraph *_graph;
    NSInteger _graphLimit;
    NSUInteger _graphGeneration;
    // Pushing and pulling, under the Commit button: "Sync Changes ↓ 1 ↑ 2"
    // or "Publish Branch" (GitUi::syncButtonTitle), and what runs.
    NSButton *_syncButton;
    MCGitSnapshot *_shown;            // the last status applied
    BOOL _syncing;                    // a fetch, pull, push or sync asked for runs
    // Autofetch (git.autofetch): a timer looks every 30 s whether a fetch
    // is due (GitUi::autoFetchDue).
    NSTimer *_autoTimer;
    BOOL _autoFetching;               // a background fetch runs (no button, no words)
    NSString *_autoKey;               // the repository's top level, once known
    NSTimeInterval _autoNow;          // the tick that started the autofetch running
    // Discards that Undo Discard can put back, the last one last.
    NSMutableArray<MCGitDiscard *> *_discards;
}

- (instancetype)initWithRoot:(NSString *)root {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 260, 400)])) {
        _root = [root copy];
        _queue = dispatch_queue_create("minicode.git", DISPATCH_QUEUE_SERIAL);
        _rows = @[];
        _graphLimit = kGraphBatch;
        self.wantsLayer = YES;
        [self build];
        [self applySettings];
        __weak MCGitPanel *weakSelf = self;
        _autoTimer = [NSTimer scheduledTimerWithTimeInterval:30 repeats:YES
                                                       block:^(NSTimer *t) {
            (void)t;
            [weakSelf autoFetchTickAt:NSDate.timeIntervalSinceReferenceDate];
        }];
        _autoTimer.tolerance = 5;
    }
    return self;
}

- (void)stop {
    [_autoTimer invalidate];
    _autoTimer = nil;
}

- (void)dealloc { [_autoTimer invalidate]; }

- (BOOL)isFlipped { return YES; }

- (void)build {
    __weak MCGitPanel *weakSelf = self;
    _branchLabel = [NSTextField labelWithString:@""];
    _branchLabel.font = [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold];
    _branchLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    // A click on the branch opens the branch picker, as VS Code's status
    // bar item does.
    [_branchLabel addGestureRecognizer:[[NSClickGestureRecognizer alloc]
                                           initWithTarget:self
                                                   action:@selector(branchClicked:)]];
    _branchLabel.toolTip = @"Switch Branch…";
    [self addSubview:_branchLabel];

    _messageScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 200, 60)];
    _messageScroll.hasVerticalScroller = YES;
    _messageScroll.autohidesScrollers = YES;
    _messageScroll.drawsBackground = NO;
    _messageScroll.wantsLayer = YES;
    _messageScroll.layer.cornerRadius = 4;
    _messageScroll.layer.borderWidth = 1;
    _messageScroll.layer.borderColor = GHex(0x3C3C3C).CGColor;
    _message = [[MCCommitTextView alloc] initWithFrame:NSMakeRect(0, 0, 200, 60)];
    _message.font = [NSFont systemFontOfSize:12];
    _message.drawsBackground = NO;
    _message.richText = NO;
    _message.allowsUndo = YES;
    _message.automaticQuoteSubstitutionEnabled = NO;
    _message.automaticDashSubstitutionEnabled = NO;
    _message.automaticSpellingCorrectionEnabled = NO;
    _message.textContainerInset = NSMakeSize(2, 4);
    _message.verticallyResizable = YES;
    _message.horizontallyResizable = NO;
    _message.autoresizingMask = NSViewWidthSizable;
    _message.textContainer.widthTracksTextView = YES;
    _message.onCommit = ^{ [weakSelf commit]; };
    _message.onTab = ^{ [weakSelf focusList]; };
    _message.onBackTab = ^{ [weakSelf focusGraph]; };
    _messageScroll.documentView = _message;
    [self addSubview:_messageScroll];

    _commitButton = [NSButton buttonWithTitle:@"Commit" target:self action:@selector(commit)];
    _commitButton.bezelStyle = NSBezelStyleRounded;
    _commitButton.controlSize = NSControlSizeRegular;
    [self addSubview:_commitButton];

    _syncButton = [NSButton buttonWithTitle:@"" target:self action:@selector(syncFromButton:)];
    _syncButton.bezelStyle = NSBezelStyleRounded;
    _syncButton.controlSize = NSControlSizeRegular;
    _syncButton.hidden = YES;
    [self addSubview:_syncButton];

    _errorLabel = [NSTextField wrappingLabelWithString:@""];
    _errorLabel.font = [NSFont systemFontOfSize:11];
    _errorLabel.selectable = YES;
    _errorLabel.maximumNumberOfLines = 6;
    _errorLabel.hidden = YES;
    [self addSubview:_errorLabel];

    _listScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _listScroll.hasVerticalScroller = YES;
    _listScroll.autohidesScrollers = YES;
    _listScroll.drawsBackground = NO;
    _listScroll.automaticallyAdjustsContentInsets = NO;
    _table = [[MCGitTable alloc] initWithFrame:NSZeroRect];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"git"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_table addTableColumn:col];
    _table.headerView = nil;
    _table.backgroundColor = NSColor.clearColor;
    _table.rowHeight = 22;
    _table.intercellSpacing = NSMakeSize(0, 0);
    _table.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
    _table.style = NSTableViewStylePlain;
    _table.dataSource = self;
    _table.delegate = self;
    _table.onActivate = ^(NSInteger row) { [weakSelf showDiffAtRow:row]; };
    _table.onClick = ^(NSInteger row) { [weakSelf showDiffAtRow:row]; };
    _table.onToggle = ^(NSInteger row) { [weakSelf toggleStageAtRow:row]; };
    _table.onTab = ^{ [weakSelf focusGraph]; };
    _table.onBackTab = ^{ [weakSelf focusMessage]; };
    _table.onPastEnd = ^(NSInteger step) { if (step > 0) [weakSelf focusGraph]; };
    _table.selectable = ^BOOL(NSInteger row) { return [weakSelf rowIsFile:row]; };
    _table.menuForRow = ^NSMenu *(NSInteger row) { return [weakSelf menuForRow:row]; };
    _listScroll.documentView = _table;
    [self addSubview:_listScroll];

    _notice = [NSTextField wrappingLabelWithString:@""];
    _notice.font = [NSFont systemFontOfSize:12];
    _notice.textColor = GHex(0x9CA3AF);
    _notice.hidden = YES;
    [self addSubview:_notice];

    // The graph: a heading with the "All branches" switch, one line saying
    // what is pushed, then the commits.
    _graphLine = [[NSView alloc] initWithFrame:NSZeroRect];
    _graphLine.wantsLayer = YES;
    _graphLine.layer.backgroundColor = GHex(0x333333).CGColor;
    [self addSubview:_graphLine];
    _graphHeading = [NSTextField labelWithString:@""];
    _graphHeading.attributedStringValue = [[NSAttributedString alloc]
        initWithString:@"GRAPH"
            attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:10
                                                                weight:NSFontWeightSemibold],
                         NSForegroundColorAttributeName: GHex(0x9CA3AF),
                         NSKernAttributeName: @0.6}];
    [self addSubview:_graphHeading];
    _allToggle = [NSButton checkboxWithTitle:@"All branches" target:self
                                      action:@selector(toggleAllBranches:)];
    _allToggle.controlSize = NSControlSizeSmall;
    _allToggle.font = [NSFont systemFontOfSize:11];
    _allToggle.toolTip = @"Show every local and remote branch, not only this branch "
                          "and its upstream";
    [self addSubview:_allToggle];
    _summaryLabel = [NSTextField labelWithString:@""];
    _summaryLabel.font = [NSFont systemFontOfSize:11];
    _summaryLabel.textColor = GHex(0x9CA3AF);
    _summaryLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [self addSubview:_summaryLabel];

    _graphScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _graphScroll.hasVerticalScroller = YES;
    _graphScroll.autohidesScrollers = YES;
    _graphScroll.drawsBackground = NO;
    _graphScroll.automaticallyAdjustsContentInsets = NO;
    _graphTable = [[MCGitTable alloc] initWithFrame:NSZeroRect];
    NSTableColumn *gcol = [[NSTableColumn alloc] initWithIdentifier:@"graph"];
    gcol.resizingMask = NSTableColumnAutoresizingMask;
    [_graphTable addTableColumn:gcol];
    _graphTable.headerView = nil;
    _graphTable.backgroundColor = NSColor.clearColor;
    _graphTable.rowHeight = 22;
    _graphTable.intercellSpacing = NSMakeSize(0, 0);
    _graphTable.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
    _graphTable.style = NSTableViewStylePlain;
    _graphTable.dataSource = self;
    _graphTable.delegate = self;
    _graphTable.onActivate = ^(NSInteger row) { [weakSelf showCommitAtRow:row]; };
    _graphTable.onClick = ^(NSInteger row) { [weakSelf showCommitAtRow:row]; };
    _graphTable.onTab = ^{ [weakSelf focusMessageOrList]; };
    _graphTable.onBackTab = ^{ [weakSelf focusListFromGraph:YES]; };
    _graphTable.onPastEnd = ^(NSInteger step) {
        if (step < 0) [weakSelf focusListFromGraph:NO];
    };
    _graphScroll.documentView = _graphTable;
    [self addSubview:_graphScroll];
    [self showGraph:NO];
    [self showMessageArea:NO];
}

- (void)showGraph:(BOOL)show {
    _graphShown = show;
    _graphLine.hidden = _graphHeading.hidden = _allToggle.hidden = !show;
    _summaryLabel.hidden = _graphScroll.hidden = !show;
}

// A right click on the headings, the button or the panel's edges.
- (NSMenu *)menuForEvent:(NSEvent *)event {
    (void)event;
    NSMenu *menu = MCAdjustSettingsMenu(@"git", self);
    if (!_inRepository) return menu;
    // VS Code's ... menu: Pull, Push, Fetch, and Sync.
    NSArray *items = @[@[@"Pull", @((int)GitUi::SyncOp::Pull)],
                       @[@"Push", @((int)GitUi::SyncOp::Push)],
                       @[@"Fetch", @((int)GitUi::SyncOp::Fetch)],
                       @[@"Sync", @((int)GitUi::SyncOp::Sync)]];
    NSInteger at = 0;
    for (NSArray *pair in items) {
        NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:pair[0]
                                                    action:@selector(syncFromMenu:)
                                             keyEquivalent:@""];
        it.target = self;
        it.tag = [pair[1] integerValue];
        [menu insertItem:it atIndex:at++];
    }
    [menu insertItem:[NSMenuItem separatorItem] atIndex:at++];
    // Then the changes as a whole, and the branch.
    NSArray *more = @[@[@"Stage All Changes", NSStringFromSelector(@selector(stageAllFromMenu:))],
                      @[@"Unstage All Changes", NSStringFromSelector(@selector(unstageAllFromMenu:))],
                      @[[self undoDiscardTitle] ?: @"Undo Discard",
                        NSStringFromSelector(@selector(undoDiscardFromMenu:))],
                      @[@"Switch Branch…", NSStringFromSelector(@selector(branchClicked:))]];
    for (NSArray *pair in more) {
        NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:pair[0]
                                                    action:NSSelectorFromString(pair[1])
                                             keyEquivalent:@""];
        it.target = self;
        [menu insertItem:it atIndex:at++];
    }
    [menu insertItem:[NSMenuItem separatorItem] atIndex:at];
    return menu;
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(syncFromMenu:)) return !_syncing;
    if (item.action == @selector(undoDiscardFromMenu:)) return _discards.count > 0;
    if (item.action == @selector(menuOpenFile:)) {
        MCGitRow *r = [self fileRow:item.tag];
        return r && _topLevel &&
               [[NSFileManager defaultManager]
                   fileExistsAtPath:[_topLevel stringByAppendingPathComponent:r.path]];
    }
    if (item.action == @selector(menuDiscard:)) {
        MCGitRow *r = [self fileRow:item.tag];
        return r && GitUi::discardPlan(r->model).refusal.empty();
    }
    return YES;
}

- (void)stageAllFromMenu:(id)sender { (void)sender; [self stageAll]; }
- (void)unstageAllFromMenu:(id)sender { (void)sender; [self unstageAll]; }
- (void)undoDiscardFromMenu:(id)sender { (void)sender; [self undoDiscard]; }
- (void)branchClicked:(id)sender {
    (void)sender;
    if (self.onChooseBranch) self.onChooseBranch();
}

- (void)syncFromMenu:(NSMenuItem *)item { [self runSync:(GitUi::SyncOp)item.tag]; }
- (void)syncFromButton:(id)sender { (void)sender; [self runSync:GitUi::SyncOp::Sync]; }

- (void)applySettings {
    AppSettings *cfg = [AppSettings shared];
    self.layer.backgroundColor = [cfg background:Surface::Sidebar].CGColor;
    _text = [cfg text:Surface::Sidebar];
    _branchLabel.textColor = _text;
    _message.textColor = _text;
    _message.insertionPointColor = _text;
    _messageScroll.layer.backgroundColor = [cfg background:Surface::Editor].CGColor;
    _allToggle.contentTintColor = _text;
    _allToggle.attributedTitle = [[NSAttributedString alloc]
        initWithString:@"All branches"
            attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11],
                         NSForegroundColorAttributeName: GHex(0x9CA3AF)}];
    [_table reloadData];
    [_graphTable reloadData];
    [self updateErrorColor];
}

- (void)updateErrorColor {
    _errorLabel.textColor = _errorIsInfo ? GHex(0x9CA3AF) : GHex(0xF48771);
}

- (void)showMessageArea:(BOOL)show {
    _messageScroll.hidden = !show;
    _commitButton.hidden = !show;
}

- (void)resizeSubviewsWithOldSize:(NSSize)old {
    (void)old;
    [self layoutPanel];
}

- (void)layoutPanel {
    const CGFloat W = self.bounds.size.width, H = self.bounds.size.height, pad = 10;
    const CGFloat inner = MAX(0, W - 2 * pad);
    CGFloat y = 8;
    _branchLabel.frame = NSMakeRect(pad, y, inner, 18);
    y += 24;
    if (!_messageScroll.hidden) {
        _messageScroll.frame = NSMakeRect(pad, y, inner, 60);
        y += 66;
        _commitButton.frame = NSMakeRect(pad - 4, y, inner + 8, 28);
        y += 32;
    }
    if (!_syncButton.hidden) {
        _syncButton.frame = NSMakeRect(pad - 4, y, inner + 8, 28);
        y += 32;
    }
    if (!_errorLabel.hidden) {
        NSSize fit = [_errorLabel.cell cellSizeForBounds:NSMakeRect(0, 0, inner, 200)];
        CGFloat h = MIN(ceil(fit.height), 120);
        _errorLabel.frame = NSMakeRect(pad, y, inner, h);
        y += h + 6;
    }
    CGFloat noticeH = 0;
    if (!_notice.hidden) {
        NSSize fit = [_notice.cell cellSizeForBounds:NSMakeRect(0, 0, inner, 200)];
        noticeH = ceil(fit.height);
        _notice.frame = NSMakeRect(pad, y + 6, inner, noticeH);
    }
    if (!_graphShown) {
        _listScroll.frame = NSMakeRect(0, y, W, MAX(0, H - y));
        [_table sizeLastColumnToFit];
        return;
    }
    // The change lists take what their rows need, up to about half of what
    // is left; the graph gets the rest and scrolls.
    CGFloat content = 0;
    for (MCGitRow *r in _rows) content += r.header ? 26 : 22;
    if (noticeH > 0) content = MAX(content, noticeH + 12);
    const CGFloat headH = 46;
    CGFloat listH = MIN(content + 4, MAX(70.0, (H - y - headH) * 0.5));
    _listScroll.frame = NSMakeRect(0, y, W, MAX(0, listH));
    [_table sizeLastColumnToFit];
    y += listH + 4;
    _graphLine.frame = NSMakeRect(0, y, W, 1);
    y += 6;
    NSSize tog = _allToggle.fittingSize;
    _graphHeading.frame = NSMakeRect(pad, y + 2, MAX(0, inner - tog.width - 4), 14);
    _allToggle.frame = NSMakeRect(W - pad - tog.width, y, tog.width, 18);
    y += 20;
    _summaryLabel.frame = NSMakeRect(pad, y, inner, 15);
    y += 19;
    _graphScroll.frame = NSMakeRect(0, y, W, MAX(0, H - y));
    [_graphTable sizeLastColumnToFit];
}

- (void)setRoot:(NSString *)root {
    _root = [root copy];
    _rows = @[];
    _topLevel = nil;
    _inRepository = NO;
    _graph = nil;
    _graphLimit = kGraphBatch;
    _shown = nil;
    _autoKey = nil;
    [self showGraph:NO];
    [self updateSyncButton];
    [_table reloadData];
    [_graphTable reloadData];
    [self setError:nil info:NO];
    [self refresh];
}

- (void)fetch { [self runSync:GitUi::SyncOp::Fetch]; }
- (void)pull { [self runSync:GitUi::SyncOp::Pull]; }
- (void)push { [self runSync:GitUi::SyncOp::Push]; }
- (void)sync { [self runSync:GitUi::SyncOp::Sync]; }

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) [self refresh];
}

// Focus order: the change lists, the graph, the message box (Tab), and back
// (Shift+Tab). With no changes the list is skipped for the graph, since there
// is nothing to stage or commit.
- (void)focusList {
    if (_table.selectedRow < 0) [_table moveBy:1];
    if (_table.selectedRow < 0) {
        if ([self graphHasRows]) [self focusGraph];
        else if (!_messageScroll.hidden) [self focusMessage];
        else [self.window makeFirstResponder:_table];
        return;
    }
    [self.window makeFirstResponder:_table];
}

- (void)focusMessage {
    if (_messageScroll.hidden) return;
    [self.window makeFirstResponder:_message];
}

- (BOOL)graphHasRows { return _graphShown && _graphTable.numberOfRows > 0; }

- (void)focusGraph {
    if (![self graphHasRows]) { [self focusMessage]; return; }
    if (_graphTable.selectedRow < 0) {
        [_graphTable selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
        [_graphTable scrollRowToVisible:0];
    }
    [self.window makeFirstResponder:_graphTable];
}

// Up from the graph's first row, or Shift+Tab: the last file in the lists.
// With no changes, Shift+Tab (`wrap`) goes round to the message and Up stays.
- (void)focusListFromGraph:(BOOL)wrap {
    if (_table.selectedRow < 0) {
        for (NSInteger i = (NSInteger)_rows.count - 1; i >= 0; i--) {
            if (_rows[i].header) continue;
            [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:i] byExtendingSelection:NO];
            [_table scrollRowToVisible:i];
            break;
        }
    }
    if (_table.selectedRow < 0) {
        if (wrap) [self focusMessage];
        return;
    }
    [self.window makeFirstResponder:_table];
}

- (void)focusMessageOrList {
    if (!_messageScroll.hidden) [self focusMessage];
    else [self focusList];
}

- (NSTableView *)list { return _table; }
- (NSTableView *)graphList { return _graphTable; }

- (void)toggleAllBranches:(id)sender {
    (void)sender;
    self.allBranches = _allToggle.state == NSControlStateValueOn;
}

- (void)setAllBranches:(BOOL)all {
    if (_allBranches == all) return;
    _allBranches = all;
    _allToggle.state = all ? NSControlStateValueOn : NSControlStateValueOff;
    _graphLimit = kGraphBatch;
    [self runRefresh];
}

- (void)showMore {
    _graphLimit += kGraphBatch;
    [self runRefresh];
}
- (NSTextView *)messageView { return _message; }
- (NSString *)errorLine { return _errorLabel.hidden ? @"" : _errorLabel.stringValue; }

- (void)setError:(NSString *)text info:(BOOL)info {
    _errorIsInfo = info;
    _errorFromStatus = NO;
    _errorLabel.stringValue = text ?: @"";
    _errorLabel.toolTip = text.length ? text : nil;   // the whole of a long one
    _errorLabel.hidden = text.length == 0;
    [self updateErrorColor];
    [self layoutPanel];
}

// --------------------------------------------------------------- refresh
- (void)refresh {
    if (!self.window || _refreshScheduled) return;
    _refreshScheduled = YES;
    // A short wait gathers a burst of requests (FSEvents, focus, an action)
    // into one status.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        self->_refreshScheduled = NO;
        [self runRefresh];
    });
}

// The status first, shown as soon as it is read; then the graph, which on a
// big history takes longer, and which is rebuilt only when HEAD, the refs,
// the limit or the "All branches" switch changed.
- (void)runRefresh {
    NSString *root = self.root;
    if (!root) return;
    NSUInteger gen = ++_graphGeneration;
    NSInteger limit = _graphLimit;
    BOOL all = _allBranches;
    MCGitGraph *previous = _graph;
    dispatch_async(_queue, ^{
        MCGitSnapshot *snap = [MCGitPanel snapshotAt:root];
        dispatch_async(dispatch_get_main_queue(), ^{
            if (![root isEqualToString:self.root]) return;   // folder changed
            [self applySnapshot:snap];
        });
        if (!GitUi::graphWanted(snap->model)) return;
        MCGitGraph *graph = [MCGitPanel graphFor:snap limit:limit all:all previous:previous];
        dispatch_async(dispatch_get_main_queue(), ^{
            if (![root isEqualToString:self.root] || gen != self->_graphGeneration) return;
            [self applyGraph:graph];
        });
    });
}

+ (MCGitGraph *)graphFor:(MCGitSnapshot *)s limit:(NSInteger)limit all:(BOOL)all
                previous:(MCGitGraph *)previous {
    NSString *dir = s.topLevel;
    const GitUi::Snapshot &snap = s->model;
    const bool compare = GitUi::compareWithUpstream(snap);
    MCGitResult *refs = MCGitRunSync(dir, NSArgs(GitUi::refArgs()));
    MCGitGraph *g = [MCGitGraph new];
    if (refs.timedOut) {   // a log now would only wait as long again
        g->g.errorText = MCGitFailureText(refs).UTF8String ?: "";
        return g;
    }
    // Everything the graph depends on, so an unchanged repository (most
    // refreshes come from saving a file) costs one for-each-ref.
    const std::string refBytes = Bytes(refs.output);
    std::string key = GitUi::graphKey(snap, (int)limit, all, compare, refBytes);
    if (previous && previous->g.key == key) return previous;
    g->g.key = std::move(key);
    g->g.limit = (int)limit;
    MCGitResult *log = MCGitRunSync(dir, NSArgs(GitUi::logArgs((int)limit, all, compare)));
    if (!log.ok) {
        g->g.errorText = MCGitFailureText(log).UTF8String ?: "";
        return g;
    }
    GitUi::buildGraph(g->g, snap, Bytes(log.output), refBytes, (int)limit);
    if (compare && (snap.ahead || snap.behind)) {
        MCGitResult *lr = MCGitRunSync(dir, NSArgs(GitUi::leftRightArgs()));
        if (lr.ok) g->g.divergence = Git::parseLeftRight(Bytes(lr.output));
    }
    return g;
}

// What is pushed, in one line over the graph.
- (NSString *)summaryFor:(MCGitSnapshot *)s {
    return NSFromBytes(GitUi::summaryText(s->model));
}

- (void)applyGraph:(MCGitGraph *)g {
    if (g == _graph) {
        if (self.onGraphLoaded) self.onGraphLoaded();
        return;
    }
    // Keep the selection on the same commit.
    std::string was;
    NSInteger sel = _graphTable.selectedRow;
    if (_graph && sel >= 0 && sel < (NSInteger)_graph->g.commits.size())
        was = _graph->g.commits[sel].hash;
    BOOL wasMoreRow = _graph && sel == (NSInteger)_graph->g.commits.size();
    _graph = g;
    [_graphTable reloadData];
    NSInteger pick = -1;
    for (size_t i = 0; !was.empty() && i < g->g.commits.size(); i++)
        if (g->g.commits[i].hash == was) { pick = (NSInteger)i; break; }
    // After "Show more", the first of the new commits.
    if (pick < 0 && wasMoreRow && sel < (NSInteger)g->g.commits.size()) pick = sel;
    if (pick >= 0) {
        [_graphTable selectRowIndexes:[NSIndexSet indexSetWithIndex:pick]
                 byExtendingSelection:NO];
        if (wasMoreRow) [_graphTable scrollRowToVisible:pick];
    } else {
        [_graphTable deselectAll:nil];
    }
    if (g.errorText.length) [self setError:g.errorText info:NO];
    if (self.onGraphLoaded) self.onGraphLoaded();
}

+ (MCGitSnapshot *)snapshotAt:(NSString *)root {
    MCGitSnapshot *s = [MCGitSnapshot new];
    s.rows = @[];
    GitUi::Snapshot &m = s->model;
    if (!MCGitExecutable()) {
        m.notice = "Git was not found. Install the command line tools "
                   "(xcode-select --install) or git from Homebrew.";
        return s;
    }
    MCGitResult *top = MCGitRunSync(root, NSArgs(GitUi::topLevelArgs()));
    if (!top.ok || !GitUi::parseTopLevel(Bytes(top.output), m.topLevel, m.gitDir)) {
        if ([top.errorText rangeOfString:@"not a git repository"].location != NSNotFound)
            m.notice = "This folder is not in a git repository.";
        else
            m.errorText = MCGitFailureText(top).UTF8String ?: "";
        return s;
    }
    m.inRepository = true;
    MCGitResult *st = MCGitRunSync(s.topLevel, NSArgs(GitUi::statusArgs()));
    if (!st.ok) {
        m.errorText = MCGitFailureText(st).UTF8String ?: "";
        return s;
    }
    GitUi::applyStatus(m, Git::parseStatus(Bytes(st.output)));
    // The remotes, for Publish Branch and for what Fetch and autofetch use.
    MCGitResult *remotes = MCGitRunSync(s.topLevel, NSArgs(GitUi::remoteArgs()));
    if (remotes.ok) m.remotes = GitUi::parseRemotes(Bytes(remotes.output));
    NSMutableArray<MCGitRow *> *rows = [NSMutableArray arrayWithCapacity:m.rows.size()];
    for (const GitUi::Row &r : m.rows) [rows addObject:[[MCGitRow alloc] initWithModel:r]];
    s.rows = rows;
    return s;
}

- (void)applySnapshot:(MCGitSnapshot *)s {
    // Keep the selection on the same file, or failing that at the same place
    // in the same list, so Space can be pressed down a list of files.
    NSInteger sel = _table.selectedRow;
    const std::vector<GitUi::Row> oldRows = ModelRows(_rows);

    _inRepository = s.inRepository;
    _topLevel = [s.topLevel copy];
    _branchText = [s.branchText copy];
    _initial = s.initial;
    _rows = s.rows ?: @[];
    _branchLabel.stringValue = s.inRepository ? (s.branchText ?: @"") : @"Source Control";
    [self showMessageArea:s.inRepository];
    _notice.stringValue = s.notice ?: @"";
    _notice.hidden = s.notice.length == 0;
    // A failing status says why until one succeeds; a failed action's
    // message stays until the next action.
    if (s.errorText.length) {
        [self setError:s.errorText info:NO];
        _errorFromStatus = YES;
    } else if (_errorFromStatus) {
        [self setError:nil info:NO];
    }
    [_table reloadData];
    BOOL graph = GitUi::graphWanted(s->model);
    [self showGraph:graph];
    _graphSummary = graph ? [self summaryFor:s] : nil;
    _summaryLabel.stringValue = _graphSummary ?: @"";
    _summaryLabel.toolTip = _graphSummary;
    _shown = s;
    if (s.topLevel) _autoKey = s.topLevel;
    [self updateSyncButton];
    if (!graph && _graph) {
        _graph = nil;
        [_graphTable reloadData];
    }

    NSInteger pick = GitUi::pickAfterRefresh(oldRows, (int)sel, ModelRows(_rows));
    if (pick >= 0) {
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:pick] byExtendingSelection:NO];
        [_table scrollRowToVisible:pick];
    } else {
        [_table deselectAll:nil];
    }
    [self layoutPanel];
    if (self.onRefreshed) self.onRefreshed();
}

- (NSArray<NSString *> *)rowDescriptions {
    NSMutableArray *out = [NSMutableArray array];
    for (MCGitRow *r in _rows) {
        if (r.header) [out addObject:[@"# " stringByAppendingString:r.title]];
        else [out addObject:[NSString stringWithFormat:@"%@ %C %@%@", r.staged ? @"S" : @"W",
                             r.letter, r.path,
                             r.origPath ? [@" <- " stringByAppendingString:r.origPath] : @""]];
    }
    return out;
}

// ---------------------------------------------------------------- actions
- (MCGitRow *)fileRow:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_rows.count || _rows[row].header) return nil;
    return _rows[row];
}
- (BOOL)rowIsFile:(NSInteger)row { return [self fileRow:row] != nil; }

// Run one git command that changes the index or makes a commit, then
// refresh. `done` gets the result on the main thread first.
- (void)runAction:(NSArray<NSString *> *)args done:(void (^)(MCGitResult *))done {
    [self runAction:args in:_topLevel done:done];
}

- (void)runAction:(NSArray<NSString *> *)args in:(NSString *)dir
             done:(void (^)(MCGitResult *))done {
    NSString *root = self.root;
    if (!dir) return;

    dispatch_async(_queue, ^{
        MCGitResult *r = MCGitRunSync(dir, args);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (![root isEqualToString:self.root]) return;
            if (r.ok) [self setError:nil info:NO];
            else [self say:MCGitFailureText(r) info:NO];
            if (done) done(r);
            [self runRefresh];
        });
    });
}

- (void)toggleStageAtRow:(NSInteger)row {
    MCGitRow *r = [self fileRow:row];
    if (!r) { NSBeep(); return; }
    // add -A (a deletion staged as one); restore --staged, both halves of a
    // rename; rm --cached --force before the first commit, which has no HEAD.
    [self runAction:NSArgs(GitUi::stageArgs(r->model, _initial)) done:nil];
}

- (void)commit {
    if (!_inRepository) return;
    NSString *msg = _message.string ?: @"";
    [self runAction:NSArgs(GitUi::commitArgs(msg.UTF8String ?: "")) done:^(MCGitResult *r) {
        if (!r.ok) return;
        [self->_message setString:@""];
        [self->_message setNeedsDisplay:YES];
        // "[main 1a2b3c4] Message"
        [self setError:NSFromBytes(GitUi::firstLine(Bytes(r.output))) info:YES];
    }];
}

// -------------------------------------- Stage All, Unstage All, Discard
// The line under the Commit button, and while the panel is hidden (an
// action from the menu bar) a note in the status bar too.
- (void)say:(NSString *)text info:(BOOL)info {
    [self setError:text info:info];
    if (text.length && !self.window && self.onNote) self.onNote(text);
}

// An alert over the panel's window (or the window it belongs to while
// hidden); `go` runs when the first button is chosen.
- (void)ask:(const GitUi::ActionPlan &)plan then:(void (^)(void))go {
    NSAlert *a = [NSAlert new];
    a.alertStyle = NSAlertStyleWarning;
    a.messageText = NSFromBytes(plan.question);
    a.informativeText = NSFromBytes(plan.detail);
    NSButton *yes = [a addButtonWithTitle:NSFromBytes(plan.button)];
    [a addButtonWithTitle:@"Cancel"];
    if (@available(macOS 11.0, *)) yes.hasDestructiveAction = YES;
    NSWindow *w = self.window ?: self.hostWindow;
    if (w) {
        [a beginSheetModalForWindow:w completionHandler:^(NSModalResponse r) {
            if (r == NSAlertFirstButtonReturn) go();
        }];
    } else if ([a runModal] == NSAlertFirstButtonReturn) {
        go();
    }
}

- (void)actionDone {
    if (self.onActionDone) self.onActionDone();
}

// Stage All and Unstage All: planned from a status read now, so the counts
// are current and the panel need not be shown.
- (void)stageAll { [self runAllStaging:YES]; }
- (void)unstageAll { [self runAllStaging:NO]; }

- (void)runAllStaging:(BOOL)stage {
    NSString *root = self.root;
    if (!root) return;
    dispatch_async(_queue, ^{
        MCGitSnapshot *snap = [MCGitPanel snapshotAt:root];
        GitUi::ActionPlan plan = stage ? GitUi::stageAllPlan(snap->model)
                                       : GitUi::unstageAllPlan(snap->model);
        NSString *top = snap.topLevel;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (![root isEqualToString:self.root]) return;
            if (plan.args.empty()) {
                NSString *why = snap.inRepository ? NSFromBytes(plan.refusal)
                                                  : snap.notice ?: snap.errorText;
                [self say:why info:NO];
                NSBeep();
                [self actionDone];
                return;
            }
            NSArray *args = NSArgs(plan.args);
            NSString *done = NSFromBytes(plan.doneText);
            void (^go)(void) = ^{
                [self runAction:args in:top done:^(MCGitResult *r) {
                    if (r.ok) [self say:done info:YES];
                    [self actionDone];
                }];
            };
            if (plan.question.empty()) go();
            else [self ask:plan then:go];
        });
    });
}

- (NSInteger)selectedFileRow {
    return [self rowIsFile:_table.selectedRow] ? _table.selectedRow : -1;
}

- (void)discardAtRow:(NSInteger)row {
    MCGitRow *r = [self fileRow:row];
    if (!r || !_topLevel) { NSBeep(); return; }
    [self discardRow:r->model top:_topLevel];
}

// Discard Changes from the menu bar: the file on show, if it has changes
// in the Changes list, by a status read now.
- (void)discardPath:(NSString *)path {
    NSString *root = self.root;
    if (!root || !path.length) { NSBeep(); return; }
    dispatch_async(_queue, ^{
        MCGitSnapshot *snap = [MCGitPanel snapshotAt:root];
        std::string rel;
        if (snap.inRepository) {
            // git names the top level with links resolved (/private/tmp);
            // the file may be gone, so its folder is resolved instead.
            char dirBuf[PATH_MAX], topBuf[PATH_MAX];
            NSString *dir = path.stringByDeletingLastPathComponent;
            if (realpath(dir.fileSystemRepresentation, dirBuf) &&
                realpath(snap.topLevel.fileSystemRepresentation, topBuf)) {
                std::string full = std::string(dirBuf) + "/" +
                                   (path.lastPathComponent.UTF8String ?: "");
                std::string top = topBuf;
                if (full.compare(0, top.size() + 1, top + "/") == 0)
                    rel = full.substr(top.size() + 1);
            }
        }
        const int at = rel.empty() ? -1 : GitUi::changesRowFor(snap->model.rows, rel);
        GitUi::Row row;
        if (at >= 0) row = snap->model.rows[(size_t)at];
        NSString *top = snap.topLevel;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (![root isEqualToString:self.root]) return;
            if (at < 0) {
                NSString *why = !snap.inRepository ? (snap.notice ?: snap.errorText)
                    : [NSString stringWithFormat:@"%@ has no changes to discard.",
                                                 path.lastPathComponent];
                [self say:why info:NO];
                NSBeep();
                [self actionDone];
                return;
            }
            [self discardRow:row top:top];
        });
    });
}

- (void)discardRow:(const GitUi::Row &)row top:(NSString *)top {
    const GitUi::DiscardPlan plan = GitUi::discardPlan(row);
    if (!plan.refusal.empty()) {
        [self say:NSFromBytes(plan.refusal) info:NO];
        NSBeep();
        [self actionDone];
        return;
    }
    const GitUi::DiscardPlan copy = plan;
    NSString *rel = NSFromBytes(row.path);
    [self ask:plan then:^{ [self runDiscard:copy rel:rel top:top]; }];
}

// The file as it is goes to the Trash first, then git restores it; if git
// fails, the Trash's copy comes back, so a refusal leaves the file alone.
- (void)runDiscard:(const GitUi::DiscardPlan &)plan rel:(NSString *)rel top:(NSString *)top {
    NSString *root = self.root;
    const GitUi::DiscardPlan p = plan;
    dispatch_async(_queue, ^{
        NSFileManager *fm = [NSFileManager defaultManager];
        NSString *path = [top stringByAppendingPathComponent:rel];
        NSURL *trashed = nil;
        NSString *failure = nil;
        if (p.trashFirst) {
            NSError *error = nil;
            NSURL *into = nil;
            if ([fm trashItemAtURL:[NSURL fileURLWithPath:path] resultingItemURL:&into
                             error:&error])
                trashed = into;
            else
                failure = [NSString stringWithFormat:
                    @"%@ could not be moved to the Trash (%@), so nothing was discarded.",
                    rel.lastPathComponent, error.localizedDescription ?: @"no reason given"];
        }
        if (!failure && !p.args.empty()) {
            MCGitResult *r = MCGitRunSync(top, NSArgs(p.args));
            if (!r.ok) {
                failure = MCGitFailureText(r);
                if (trashed) [fm moveItemAtURL:trashed toURL:[NSURL fileURLWithPath:path] error:nil];
                trashed = nil;
            }
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!failure) {
                MCGitDiscard *d = [MCGitDiscard new];
                d.path = path;
                d.rel = rel;
                d.trashed = trashed;
                if (!self->_discards) self->_discards = [NSMutableArray array];
                [self->_discards addObject:d];
                if (self->_discards.count > 20) [self->_discards removeObjectAtIndex:0];
            }
            if ([root isEqualToString:self.root]) {
                [self say:failure ?: NSFromBytes(p.doneText) info:failure == nil];
                [self runRefresh];
            }
            [self actionDone];
        });
    });
}

- (NSString *)undoDiscardTitle {
    MCGitDiscard *d = _discards.lastObject;
    return d ? NSFromBytes(GitUi::undoDiscardTitle(d.rel.UTF8String ?: "")) : nil;
}

// Undo Discard: what is at the path now (git's version, or edits since)
// goes to the Trash, and the discarded copy comes back from it.
- (void)undoDiscard {
    MCGitDiscard *d = _discards.lastObject;
    if (!d) { NSBeep(); return; }
    [_discards removeLastObject];
    NSString *root = self.root;
    dispatch_async(_queue, ^{
        NSFileManager *fm = [NSFileManager defaultManager];
        NSString *failure = nil;
        NSString *name = d.rel.lastPathComponent;
        if (d.trashed && ![fm attributesOfItemAtPath:d.trashed.path error:nil])
            failure = [NSString stringWithFormat:
                @"The discarded copy of %@ is no longer in the Trash.", name];
        NSURL *at = [NSURL fileURLWithPath:d.path];
        NSError *error = nil;
        if (!failure && [fm attributesOfItemAtPath:d.path error:nil] &&
            ![fm trashItemAtURL:at resultingItemURL:nil error:&error])
            failure = [NSString stringWithFormat:@"%@ could not be moved to the Trash (%@).",
                                                 name, error.localizedDescription];
        if (!failure && d.trashed) {
            [fm createDirectoryAtPath:d.path.stringByDeletingLastPathComponent
                withIntermediateDirectories:YES attributes:nil error:nil];
            if (![fm moveItemAtURL:d.trashed toURL:at error:&error])
                failure = [NSString stringWithFormat:@"%@ could not be put back (%@).", name,
                                                     error.localizedDescription];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if ([root isEqualToString:self.root]) {
                [self say:failure ?: NSFromBytes(GitUi::undoneDiscardText(d.rel.UTF8String ?: ""))
                     info:failure == nil];
                [self runRefresh];
            }
            [self actionDone];
        });
    });
}

// ------------------------------------------------------ a file row's menu
- (NSMenu *)menuForRow:(NSInteger)row {
    MCGitRow *r = [self fileRow:row];
    if (!r) return nil;
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];
    auto add = [&](NSString *title, SEL action) {
        NSMenuItem *it = [menu addItemWithTitle:title action:action keyEquivalent:@""];
        it.target = self;
        it.tag = row;
        return it;
    };
    add(@"Open File", @selector(menuOpenFile:));
    add(@"Open Changes", @selector(menuOpenChanges:));
    add(r.staged ? @"Unstage Changes" : @"Stage Changes", @selector(menuToggleStage:));
    if (!r.staged) {
        NSMenuItem *it = add(r.untracked ? @"Discard (Move to Trash)…" : @"Discard Changes…",
                             @selector(menuDiscard:));
        const std::string why = GitUi::discardPlan(r->model).refusal;
        if (!why.empty()) it.toolTip = NSFromBytes(why);
    }
    [menu addItem:[NSMenuItem separatorItem]];
    add(@"Reveal in File Tree", @selector(menuReveal:));
    return menu;
}

- (NSString *)pathOfRow:(NSInteger)row {
    MCGitRow *r = [self fileRow:row];
    return r && _topLevel ? [_topLevel stringByAppendingPathComponent:r.path] : nil;
}
- (void)menuOpenFile:(NSMenuItem *)item {
    NSString *path = [self pathOfRow:item.tag];
    if (path && self.onOpenFile) self.onOpenFile(path);
}
- (void)menuOpenChanges:(NSMenuItem *)item { [self showDiffAtRow:item.tag]; }
- (void)menuToggleStage:(NSMenuItem *)item { [self toggleStageAtRow:item.tag]; }
- (void)menuDiscard:(NSMenuItem *)item { [self discardAtRow:item.tag]; }
- (void)menuReveal:(NSMenuItem *)item {
    NSString *path = [self pathOfRow:item.tag];
    if (path && self.onRevealFile) self.onRevealFile(path);
}

// ------------------------------------------------------ the branch picker
- (void)listBranches:(void (^)(MCGitBranches *))done {
    NSString *root = self.root;
    if (!root) { if (done) done(nil); return; }
    dispatch_async(_queue, ^{
        MCGitSnapshot *snap = [MCGitPanel snapshotAt:root];
        MCGitBranches *list = nil;
        NSString *failure = nil;
        if (!snap.inRepository) {
            failure = snap.notice ?: snap.errorText;
        } else {
            MCGitResult *refs = MCGitRunSync(snap.topLevel, NSArgs(GitUi::branchListArgs()));
            if (!refs.ok) {
                failure = MCGitFailureText(refs);
            } else {
                list = [MCGitBranches new];
                list.top = snap.topLevel;
                list->snap = snap->model;
                list->items = GitUi::branchItems(Bytes(refs.output), snap->model);
                NSMutableArray *labels = [NSMutableArray array], *details = [NSMutableArray array];
                for (const GitUi::BranchItem &b : list->items) {
                    [labels addObject:NSFromBytes(b.name) ?: @""];
                    [details addObject:NSFromBytes(b.detail) ?: @""];
                }
                list.labels = labels;
                list.details = details;
            }
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (![root isEqualToString:self.root]) return;
            if (failure) {
                [self say:failure info:NO];
                NSBeep();
            }
            if (done) done(list);
        });
    });
}

// A refused switch (local changes git would overwrite) says what git said.
- (void)switchToBranchAtIndex:(NSInteger)index of:(MCGitBranches *)list {
    if (!list || index < 0 || index >= (NSInteger)list->items.size()) return;
    const GitUi::BranchItem &b = list->items[(size_t)index];
    const std::vector<std::string> args = GitUi::switchArgs(b, list->snap);
    if (args.empty()) {
        [self say:[NSString stringWithFormat:@"Already on %@.", NSFromBytes(b.name)] info:YES];
        [self actionDone];
        return;
    }
    NSString *text = NSFromBytes(b.remote ? GitUi::switchedText(GitUi::localNameFor(b.name,
                                                                                    list->snap),
                                                                true)
                                          : GitUi::switchedText(b.name, false));
    [self runAction:NSArgs(args) in:list.top done:^(MCGitResult *r) {
        if (r.ok) [self say:text info:YES];
        [self actionDone];
    }];
}

- (void)createBranchNamed:(NSString *)typed of:(MCGitBranches *)list {
    if (!list) return;
    const std::string name = GitUi::newBranchName(typed.UTF8String ?: "");
    const std::string problem = GitUi::branchNameProblem(name);
    if (!problem.empty()) {
        [self say:NSFromBytes(problem) info:NO];
        NSBeep();
        [self actionDone];
        return;
    }
    NSString *text = NSFromBytes(GitUi::switchedText(name, true));
    [self runAction:NSArgs(GitUi::createBranchArgs(name)) in:list.top done:^(MCGitResult *r) {
        if (r.ok) [self say:text info:YES];
        [self actionDone];
    }];
}

// ------------------------------------------------- pushing and pulling

// Each repository's autofetch state, by its top level, shared by the
// windows on it so two of them do not fetch twice. Main thread only.
static std::unordered_map<std::string, GitUi::AutoFetchState> &MCAutoFetchStates() {
    static std::unordered_map<std::string, GitUi::AutoFetchState> states;
    return states;
}

static const char *MCSyncOpName(GitUi::SyncOp op) {
    switch (op) {
    case GitUi::SyncOp::Fetch: return "fetch";
    case GitUi::SyncOp::Pull: return "pull";
    case GitUi::SyncOp::Push: return "push";
    case GitUi::SyncOp::Sync: return "sync";
    }
    return "";
}

- (void)updateSyncButton {
    NSString *title = _shown ? NSFromBytes(GitUi::syncButtonTitle(_shown->model)) : @"";
    BOOL show = _inRepository && !_messageScroll.hidden && (title.length > 0 || _syncing);
    if (!_syncing) {
        _syncButton.title = title;
        _syncButton.toolTip = title.length ? NSFromBytes(GitUi::syncButtonToolTip(_shown->model))
                                           : nil;
    }
    _syncButton.enabled = !_syncing;
    if (_syncButton.hidden == show) {
        _syncButton.hidden = !show;
        [self layoutPanel];
    }
}

- (BOOL)syncing { return _syncing || _autoFetching; }
- (NSString *)syncButtonTitle { return _syncButton.hidden ? @"" : _syncButton.title; }

// Fetch, Pull, Push or Sync, from the panel's button and menu, and the
// commands (git.fetch and so on). The plan is made from a status read
// just now, so the counts and the upstream are current even when the panel
// has not been shown; nothing is asked first (VS Code's git.confirmSync
// off). One at a time; a second while one runs beeps.
- (void)runSync:(GitUi::SyncOp)op {
    [self runSync:op quietly:NO];
}

- (void)runSync:(GitUi::SyncOp)op quietly:(BOOL)quiet {
    NSString *root = self.root;
    if (!root) return;
    // One asked for at a time; one asked for while a background fetch runs
    // waits behind it on the queue.
    if (quiet ? (_syncing || _autoFetching) : _syncing) {
        if (!quiet) NSBeep();
        return;
    }
    if (quiet) _autoFetching = YES;
    else _syncing = YES;
    [self updateSyncButton];
    dispatch_async(_queue, ^{
        MCGitSnapshot *snap = [MCGitPanel snapshotAt:root];
        GitUi::SyncPlan plan = GitUi::syncPlan(op, snap->model);
        if (quiet && (!snap.inRepository || snap->model.remotes.empty())) plan = GitUi::SyncPlan();
        NSString *top = snap.topLevel;
        if (plan.steps.empty()) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self finishSync:op plan:plan result:nil top:top root:root quietly:quiet];
            });
            return;
        }
        if (!quiet) {
            NSString *busy = NSFromBytes(plan.busyText);
            dispatch_async(dispatch_get_main_queue(), ^{
                if (![root isEqualToString:self.root]) return;
                self->_syncButton.title = busy;
                [self setError:busy info:YES];
            });
        }
        MCGitResult *last = nil;
        for (const std::vector<std::string> &step : plan.steps) {
            last = MCGitRunSync(snap.topLevel, NSArgs(step));
            if (!last.ok) break;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [self finishSync:op plan:plan result:last top:top root:root quietly:quiet];
        });
    });
}

- (void)finishSync:(GitUi::SyncOp)op plan:(const GitUi::SyncPlan &)plan
            result:(MCGitResult *)r top:(NSString *)top root:(NSString *)root
           quietly:(BOOL)quiet {
    if (quiet) _autoFetching = NO;
    else _syncing = NO;
    if (top) _autoKey = top;
    const BOOL ran = r != nil;
    const BOOL ok = ran && r.ok;
    // Any fetch counts for autofetch's wait; an autofetch that found no
    // remote too (it was keyed by the folder until the top level was known).
    if (top && (quiet || (ran && plan.fetches))) {
        GitUi::AutoFetchState &st = MCAutoFetchStates()[top.UTF8String ?: ""];
        st.lastStart = quiet ? _autoNow : NSDate.timeIntervalSinceReferenceDate;
        if (ran && ok) st.failures = 0;
        else if (ran && quiet) st.failures++;
    }
    if (ran && quiet) _autoFetches++;
    _lastSync = [NSString stringWithFormat:@"%s %s", MCSyncOpName(op),
                                           !ran ? "refused" : ok ? "ok" : "failed"];
    if (![root isEqualToString:self.root]) {
        [self updateSyncButton];
        return;
    }
    NSString *text = nil;
    BOOL info = YES;
    if (!ran) {
        text = quiet ? nil : NSFromBytes(plan.refusal);
        info = NO;
        if (!quiet) NSBeep();
    } else if (!ok) {
        text = NSFromBytes(GitUi::networkFailureText(r.status, Bytes(r.output),
                                                     (r.errorText ?: @"").UTF8String ?: ""));
        if (r.timedOut) text = r.errorText;
        if (quiet) text = [@"Autofetch: " stringByAppendingString:text];
        info = NO;
    } else if (!quiet) {
        text = NSFromBytes(plan.doneText);
    }
    if (text || !quiet) [self setError:text info:info];
    // A background fetch that works again takes its old failure away.
    else if (ok && [self.errorLine hasPrefix:@"Autofetch: "]) [self setError:nil info:NO];
    [self updateSyncButton];
    if (self.onSynced) self.onSynced();
    // The counts, the graph and the lists as they are now.
    if (ran && self.window) [self runRefresh];
}

// git.autofetch: called every 30 s by the panel's timer (and by tests with
// a time of their own). YES when a fetch was started.
- (BOOL)autoFetchTickAt:(NSTimeInterval)now {
    const Settings &cfg = [AppSettings shared].settings;
    if (!cfg.gitAutoFetch() || !self.root || _syncing || _autoFetching) return NO;
    BOOL active = self.isActive ? self.isActive() : self.window.isKeyWindow;
    if (!active) return NO;
    // By what is known so far: the repository's top level once a status
    // has said it, else the folder. The fetch itself reads the remotes
    // again and fetches nothing without one.
    std::string key = (_autoKey ?: self.root).UTF8String ?: "";
    GitUi::AutoFetchState &st = MCAutoFetchStates()[key];
    GitUi::Snapshot probe;
    probe.inRepository = true;
    probe.remotes = {"?"};
    if (!GitUi::autoFetchDue(true, true, probe, false, st, now, cfg.gitAutoFetchPeriod()))
        return NO;
    // A folder with no repository or no remote is looked at again only
    // when the wait is over.
    st.lastStart = now;
    _autoNow = now;
    [self runSync:GitUi::SyncOp::Fetch quietly:YES];
    return YES;
}

- (void)showDiffAtRow:(NSInteger)row {
    MCGitRow *r = [self fileRow:row];
    if (!r || !_topLevel) return;
    // Exit status 1 means "differs" for an untracked file's --no-index diff.
    NSArray *args = NSArgs(GitUi::diffArgs(r->model));
    NSUInteger gen = ++_diffGeneration;
    NSString *dir = _topLevel, *root = self.root;
    NSString *path = [dir stringByAppendingPathComponent:r.path];
    NSString *name = r.path.lastPathComponent;
    BOOL untracked = r.untracked;
    dispatch_async(_queue, ^{
        MCGitResult *res = MCGitRunSync(dir, args);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (gen != self->_diffGeneration || ![root isEqualToString:self.root]) return;
            if (!(res.ok || (untracked && res.status == 1))) {
                [self setError:MCGitFailureText(res)
                          info:NO];
                return;
            }
            if (self.onShowDiff) self.onShowDiff(name, path, res.output);
        });
    });
}

// A commit's header, message and diff (against its first parent for a
// merge, which is what the merge brought in; git's default for a merge is a
// combined diff that is usually empty).
- (void)showCommitAtRow:(NSInteger)row {
    if (!_graph || row < 0 || !_topLevel) return;
    if (row >= (NSInteger)_graph->g.commits.size()) {
        if (_graph.hasMore) [self showMore];
        return;
    }
    const Git::Commit &c = _graph->g.commits[row];
    NSString *title = NSFromBytes(GitUi::commitTitle(c));
    NSArray *args = NSArgs(GitUi::showArgs(c.hash));
    NSUInteger gen = ++_diffGeneration;
    NSString *dir = _topLevel, *root = self.root;
    dispatch_async(_queue, ^{
        MCGitResult *res = MCGitRunSync(dir, args);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (gen != self->_diffGeneration || ![root isEqualToString:self.root]) return;
            if (!res.ok) {
                [self setError:MCGitFailureText(res) info:NO];
                return;
            }
            if (self.onShowCommit) self.onShowCommit(title, res.output);
        });
    });
}

- (NSArray<NSString *> *)graphDescriptions {
    NSMutableArray *out = [NSMutableArray array];
    if (!_graph) return out;
    for (const std::string &line : GitUi::graphDescriptions(_graph->g))
        [out addObject:NSFromBytes(line) ?: @""];
    return out;
}

- (NSString *)graphToolTip:(NSInteger)row {
    return NSFromBytes(GitUi::graphToolTip(_graph->g, (size_t)row, MCGitNow(), MCGitDate));
}

// ------------------------------------------------------------ table view
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv {
    if (tv == _graphTable)
        return _graph ? (NSInteger)_graph->g.commits.size() + (_graph.hasMore ? 1 : 0) : 0;
    return (NSInteger)_rows.count;
}

- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)col
                  row:(NSInteger)row {
    (void)col;
    if (tv == _graphTable) {
        MCGitGraphCell *cell = [tv makeViewWithIdentifier:@"graphcell" owner:self];
        if (!cell) {
            cell = [[MCGitGraphCell alloc] initWithFrame:NSMakeRect(0, 0, 200, 22)];
            cell.identifier = @"graphcell";
        }
        cell.graph = _graph;
        cell.index = row;
        cell.textColor = _text;
        cell.panelColor = [[AppSettings shared] background:Surface::Sidebar];
        cell.toolTip = [self graphToolTip:row];
        [cell setNeedsDisplay:YES];
        return cell;
    }
    MCGitCell *cell = [tv makeViewWithIdentifier:@"gitcell" owner:self];
    if (!cell) {
        cell = [[MCGitCell alloc] initWithFrame:NSMakeRect(0, 0, 200, 22)];
        cell.identifier = @"gitcell";
    }
    MCGitRow *r = _rows[row];
    cell.row = r;
    cell.textColor = _text;
    cell.toolTip = r.header ? nil : NSFromBytes(GitUi::rowToolTip(r->model));
    [cell setNeedsDisplay:YES];
    return cell;
}

- (BOOL)tableView:(NSTableView *)tv shouldSelectRow:(NSInteger)row {
    if (tv == _graphTable) return YES;
    return [self rowIsFile:row];
}

- (CGFloat)tableView:(NSTableView *)tv heightOfRow:(NSInteger)row {
    if (tv == _graphTable) return 22;
    return _rows[row].header ? 26 : 22;
}

@end
