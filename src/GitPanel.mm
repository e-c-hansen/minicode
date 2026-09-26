// GitPanel.mm — Objective-C++. See GitPanel.h.
#import "GitPanel.h"
#import "AppSettings.h"
#import "Lsp.h"   // MCFindProgram, MCSearchDirs
#include "GitStatus.h"
#include "GitGraph.h"
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

MCGitResult *MCGitRunSync(NSString *dir, NSArray<NSString *> *args) {
    MCGitResult *r = [MCGitResult new];
    r.status = -1;
    r.output = [NSData data];
    NSString *git = MCGitExecutable();
    if (!git) {
        r.errorText = @"Git was not found. Install the command line tools "
                       "(xcode-select --install) or git from Homebrew.";
        return r;
    }
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:git];
    task.arguments = args;
    task.currentDirectoryURL = [NSURL fileURLWithPath:dir isDirectory:YES];
    NSMutableDictionary *env = [NSProcessInfo.processInfo.environment mutableCopy];
    env[@"PATH"] = [MCSearchDirs() componentsJoinedByString:@":"];
    env[@"GIT_TERMINAL_PROMPT"] = @"0";   // never wait for a password
    env[@"GIT_EDITOR"] = @"true";         // never wait for an editor
    env[@"GIT_OPTIONAL_LOCKS"] = @"0";    // status must not write the index
    env[@"GIT_PAGER"] = @"cat";
    task.environment = env;
    NSPipe *outPipe = [NSPipe pipe], *errPipe = [NSPipe pipe];
    task.standardOutput = outPipe;
    task.standardError = errPipe;
    task.standardInput = [NSFileHandle fileHandleWithNullDevice];
    dispatch_semaphore_t exited = dispatch_semaphore_create(0);
    task.terminationHandler = ^(NSTask *t) {
        (void)t;
        dispatch_semaphore_signal(exited);
    };
    NSError *err = nil;
    if (![task launchAndReturnError:&err]) {
        r.errorText = err.localizedDescription ?: @"Git could not be started.";
        return r;
    }
    // Both pipes are drained at once, or a chatty stderr could fill its pipe
    // and stall git while we wait on stdout.
    __block NSData *errData = nil;
    dispatch_group_t group = dispatch_group_create();
    dispatch_group_async(group, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        errData = [errPipe.fileHandleForReading readDataToEndOfFile];
    });
    NSData *outData = [outPipe.fileHandleForReading readDataToEndOfFile];
    dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
    dispatch_semaphore_wait(exited, DISPATCH_TIME_FOREVER);
    r.status = task.terminationStatus;
    r.output = outData ?: [NSData data];
    std::string e((const char *)errData.bytes, errData.length);
    r.errorText = [NSFromBytes(e) stringByTrimmingCharactersInSet:
                      NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return r;
}

// The non-blank lines of `s`, trimmed.
static NSArray<NSString *> *NonBlankLines(NSString *s) {
    NSMutableArray *out = [NSMutableArray array];
    for (NSString *line in [s componentsSeparatedByCharactersInSet:
                                  NSCharacterSet.newlineCharacterSet]) {
        NSString *t = [line stringByTrimmingCharactersInSet:
                          NSCharacterSet.whitespaceCharacterSet];
        if (t.length) [out addObject:t];
    }
    return out;
}

// What to tell the user about a failed command: git's stderr without its
// blank lines; else the last line of stdout (a commit with nothing staged
// prints a whole status there and ends with the reason); else the exit
// status.
static NSString *MCGitFailureText(MCGitResult *r) {
    NSArray *err = NonBlankLines(r.errorText ?: @"");
    if (err.count) return [err componentsJoinedByString:@"\n"];
    std::string out((const char *)r.output.bytes, r.output.length);
    NSArray *lines = NonBlankLines(NSFromBytes(out));
    if (lines.count) return lines.lastObject;
    return [NSString stringWithFormat:@"git exited with status %d.", r.status];
}

// ------------------------------------------------------------- diff colors
static const size_t kMaxDiffBytes = 4 << 20;

NSAttributedString *MCGitDiffText(NSData *diff, NSFont *font) {
    AppSettings *cfg = [AppSettings shared];
    NSColor *plain = [cfg text:Surface::Editor];
    NSColor *muted = GHex(0x9CA3AF);
    NSFont *bold = [[NSFontManager sharedFontManager] convertFont:font
                                                     toHaveTrait:NSBoldFontMask];
    NSDictionary *base = @{NSFontAttributeName: font,
                           NSForegroundColorAttributeName: plain};
    if (diff.length == 0) {
        return [[NSAttributedString alloc]
            initWithString:@"No differences to show."
                attributes:@{NSFontAttributeName: font,
                             NSForegroundColorAttributeName: muted}];
    }
    std::string bytes((const char *)diff.bytes, diff.length);
    bool cut = false;
    if (bytes.size() > kMaxDiffBytes) {
        size_t nl = bytes.rfind('\n', kMaxDiffBytes);
        bytes.resize(nl == std::string::npos ? kMaxDiffBytes : nl + 1);
        cut = true;
    }
    // Build the text once, then color runs of lines, which is far quicker
    // than appending an attributed string per line on a long diff.
    NSMutableString *text = [NSMutableString string];
    struct Run { NSUInteger start, length; Git::LineKind kind; };
    std::vector<Run> runs;
    for (const Git::DiffLine &l : Git::classifyDiff(bytes)) {
        NSUInteger start = text.length;
        [text appendString:NSFromBytes(l.text) ?: @""];
        [text appendString:@"\n"];
        NSUInteger len = text.length - start;
        if (!runs.empty() && runs.back().kind == l.kind &&
            runs.back().start + runs.back().length == start)
            runs.back().length += len;
        else
            runs.push_back({start, len, l.kind});
    }
    if (cut) [text appendString:@"\n(The diff is longer than 4 MB; the rest is not shown.)\n"];
    NSMutableAttributedString *out =
        [[NSMutableAttributedString alloc] initWithString:text attributes:base];
    NSColor *added = GHex(0x73C991), *removed = GHex(0xF14C4C);
    for (const Run &r : runs) {
        NSRange range = NSMakeRange(r.start, r.length);
        switch (r.kind) {
        case Git::LineKind::Added:
            [out addAttribute:NSForegroundColorAttributeName value:added range:range];
            break;
        case Git::LineKind::Removed:
            [out addAttribute:NSForegroundColorAttributeName value:removed range:range];
            break;
        case Git::LineKind::FileHeader:
            [out addAttributes:@{NSForegroundColorAttributeName: muted,
                                 NSFontAttributeName: bold} range:range];
            break;
        case Git::LineKind::HunkHeader:
        case Git::LineKind::NoNewline:
        case Git::LineKind::Other:
            [out addAttribute:NSForegroundColorAttributeName value:muted range:range];
            break;
        case Git::LineKind::Context:
            break;
        }
    }
    if (cut)
        [out addAttribute:NSForegroundColorAttributeName value:muted
                    range:NSMakeRange(runs.empty() ? 0 : runs.back().start + runs.back().length,
                                      out.length - (runs.empty() ? 0 : runs.back().start +
                                                                       runs.back().length))];
    return out;
}

static NSString *MCGitDateText(long long t) {
    static NSDateFormatter *f;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        f = [NSDateFormatter new];
        f.dateStyle = NSDateFormatterMediumStyle;
        f.timeStyle = NSDateFormatterShortStyle;
    });
    return [f stringFromDate:[NSDate dateWithTimeIntervalSince1970:(double)t]];
}

static NSString *MCGitRelativeText(long long t) {
    return NSFromBytes(Git::relativeTime(t, (long long)NSDate.date.timeIntervalSince1970));
}

NSAttributedString *MCGitCommitText(NSData *show, NSFont *font) {
    std::string bytes((const char *)show.bytes, show.length);
    Git::CommitDetail d;
    if (!Git::parseShow(bytes, d)) return MCGitDiffText(show, font);
    AppSettings *cfg = [AppSettings shared];
    NSColor *plain = [cfg text:Surface::Editor];
    NSColor *muted = GHex(0x9CA3AF);
    NSFont *bold = [[NSFontManager sharedFontManager] convertFont:font
                                                     toHaveTrait:NSBoldFontMask];
    NSDictionary *label = @{NSFontAttributeName: font, NSForegroundColorAttributeName: muted};
    NSDictionary *value = @{NSFontAttributeName: font, NSForegroundColorAttributeName: plain};
    NSMutableAttributedString *out = [NSMutableAttributedString new];
    void (^add)(NSString *, NSDictionary *) = ^(NSString *s, NSDictionary *a) {
        [out appendAttributedString:[[NSAttributedString alloc] initWithString:s ?: @""
                                                                    attributes:a]];
    };
    auto line = ^(NSString *name, NSString *text) {
        add([name stringByPaddingToLength:10 withString:@" " startingAtIndex:0], label);
        add(text, value);
        add(@"\n", value);
    };
    add(@"commit    ", label);
    add(NSFromBytes(d.hash), @{NSFontAttributeName: font,
                               NSForegroundColorAttributeName: GHex(0xE2C08D)});
    add(@"\n", value);
    NSMutableArray *parents = [NSMutableArray array];
    for (const std::string &p : d.parents) [parents addObject:NSFromBytes(Git::shortHash(p))];
    if (parents.count > 1)
        line(@"Merge", [parents componentsJoinedByString:@" "]);
    else if (parents.count == 1)
        line(@"Parent", parents[0]);
    line(@"Author", [NSString stringWithFormat:@"%@ <%@>", NSFromBytes(d.author),
                                               NSFromBytes(d.email)]);
    if (!d.committer.empty()) line(@"Committer", NSFromBytes(d.committer));
    line(@"Date", [NSString stringWithFormat:@"%@ (%@)", MCGitDateText(d.time),
                                             MCGitRelativeText(d.time)]);
    add(@"\n", value);
    // The message: its subject bold, the body as written.
    NSString *msg = NSFromBytes(d.message);
    NSRange nl = [msg rangeOfString:@"\n"];
    NSString *subject = nl.location == NSNotFound ? msg : [msg substringToIndex:nl.location];
    add(subject, @{NSFontAttributeName: bold, NSForegroundColorAttributeName: plain});
    if (nl.location != NSNotFound) add([msg substringFromIndex:nl.location], value);
    add(@"\n\n", value);
    if (parents.count > 1)
        add(@"Changes against the first parent, which is what the merge brought in.\n\n",
            label);
    if (d.patch.empty()) {
        add(@"No changes in this commit.\n", label);
    } else {
        NSData *patch = [NSData dataWithBytes:d.patch.data() length:d.patch.size()];
        [out appendAttributedString:MCGitDiffText(patch, font)];
    }
    return out;
}

// ------------------------------------------------------------------- rows
@interface MCGitRow : NSObject
@property(nonatomic, assign) BOOL header;
@property(nonatomic, copy) NSString *title;       // header text
@property(nonatomic, assign) BOOL staged;         // which list it is in
@property(nonatomic, assign) unichar letter;
@property(nonatomic, copy) NSString *path;        // relative to the top level
@property(nonatomic, copy) NSString *origPath;    // a rename's source, or nil
@property(nonatomic, assign) BOOL untracked;
@property(nonatomic, assign) BOOL unmerged;
@end
@implementation MCGitRow
- (NSString *)key {
    return [NSString stringWithFormat:@"%d:%@", self.staged, self.path ?: self.title];
}
@end

// What one refresh found, built off the main thread.
@interface MCGitSnapshot : NSObject
@property(nonatomic, assign) BOOL inRepository;
@property(nonatomic, copy) NSString *topLevel;
@property(nonatomic, copy) NSString *branchText;
@property(nonatomic, assign) BOOL initial;        // no commit yet
@property(nonatomic, copy) NSArray<MCGitRow *> *rows;
@property(nonatomic, copy) NSString *errorText;   // a command failed
@property(nonatomic, copy) NSString *notice;      // one plain line for the list
// What the graph needs from the status.
@property(nonatomic, copy) NSString *headOid;     // nil before the first commit
@property(nonatomic, copy) NSString *branch;      // nil when detached
@property(nonatomic, assign) BOOL detached;
@property(nonatomic, copy) NSString *upstream;    // nil when there is none
@property(nonatomic, assign) BOOL hasAheadBehind; // false when the upstream is gone
@property(nonatomic, assign) int ahead, behind;
@end
@implementation MCGitSnapshot
@end

// ------------------------------------------------------------------ graph
// One load of the commit graph, built off the main thread and not changed
// after: the commits, their lanes, labels and which ones are not pushed.
@interface MCGitGraph : NSObject {
@public
    std::vector<Git::Commit> commits;
    std::vector<Git::GraphRow> rows;
    std::unordered_map<std::string, std::vector<Git::Ref>> labels;
    Git::Divergence divergence;
    std::string head;
    int maxWidth;
}
@property(nonatomic, strong) NSData *key;         // what it was built from; same key, same graph
@property(nonatomic, assign) BOOL hasMore;        // the limit cut the history short
@property(nonatomic, assign) NSInteger limit;
@property(nonatomic, copy) NSString *errorText;
@end
@implementation MCGitGraph
@end

static const NSInteger kGraphBatch = 200;

// Lane colors: the first is the accent blue (HEAD's lane is usually first),
// the rest VS Code's graph colors.
static NSColor *LaneColor(int i) {
    static const unsigned int palette[] = {0x59A4F9, 0xFFB000, 0xDC267F, 0x40B0A6,
                                           0xB66DFF, 0xE0823D, 0x8FCB5A};
    const int n = sizeof(palette) / sizeof(palette[0]);
    return GHex(palette[((i % n) + n) % n]);
}

// The width of one lane: 12 points, narrower when many lanes would take
// more than two fifths of the row.
static CGFloat LaneWidth(CGFloat rowWidth, int lanes) {
    if (lanes <= 0) return 12;
    return MAX(4.0, MIN(12.0, (rowWidth * 0.4 - 8) / lanes));
}

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
    NSColor *c = r.kind == Git::RefKind::Head     ? GHex(0xEA5C00)
               : r.kind == Git::RefKind::Remote   ? GHex(0xB180D7)
               : r.kind == Git::RefKind::Tag      ? GHex(0xE2C08D)
                                                  : GHex(0x59A4F9);
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
    if (self.index >= (NSInteger)g->commits.size()) {
        [[NSString stringWithFormat:@"Show %ld more…", (long)kGraphBatch]
            drawAtPoint:NSMakePoint(18, 3)
         withAttributes:@{NSFontAttributeName: [NSFont systemFontOfSize:12],
                          NSForegroundColorAttributeName: GHex(0x4EA1F7)}];
        return;
    }
    const Git::Commit &c = g->commits[self.index];
    const Git::GraphRow &row = g->rows[self.index];
    const bool outgoing = g->divergence.outgoing.count(c.hash) > 0;
    const bool incoming = g->divergence.incoming.count(c.hash) > 0;
    const CGFloat lw = LaneWidth(b.size.width, g->maxWidth);
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
    if (c.hash == g->head) {
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
    auto it = g->labels.find(c.hash);
    if (it != g->labels.end()) {
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
    if (unmerged) return GHex(0xE4676B);
    switch (c) {
    case 'M': case 'T': return GHex(0xE2C08D);
    case 'A': return GHex(0x81B88B);
    case 'D': return GHex(0xC74E39);
    case 'R': case 'C': case 'U': return GHex(0x73C991);
    default: return GHex(0x9CA3AF);
    }
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
             NSFontAttributeName: [NSFont monospacedSystemFontOfSize:12
                                                              weight:NSFontWeightSemibold],
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
- (BOOL)moveBy:(NSInteger)step;   // to the next file row up or down; NO if none
@end
@implementation MCGitTable
- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSInteger row = [self rowAtPoint:p];
    [super mouseDown:event];
    if (row >= 0 && self.onClick && event.clickCount == 1) self.onClick(row);
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
    return (m & NSEventModifierFlagCommand) &&
           !(m & (NSEventModifierFlagControl | NSEventModifierFlagOption)) &&
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

// ------------------------------------------------------------------- panel
@interface MCGitPanel () <NSTableViewDataSource, NSTableViewDelegate>
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
    }
    return self;
}

- (BOOL)isFlipped { return YES; }

- (void)build {
    __weak MCGitPanel *weakSelf = self;
    _branchLabel = [NSTextField labelWithString:@""];
    _branchLabel.font = [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold];
    _branchLabel.lineBreakMode = NSLineBreakByTruncatingTail;
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
    [self showGraph:NO];
    [_table reloadData];
    [_graphTable reloadData];
    [self setError:nil info:NO];
    [self refresh];
}

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
        if (!snap.inRepository || snap.initial || !snap.headOid) return;
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
    BOOL compare = s.upstream.length && s.hasAheadBehind && !s.detached;
    MCGitResult *refs = MCGitRunSync(dir, @[@"for-each-ref",
                                            [NSString stringWithFormat:@"--format=%s",
                                                                       Git::refFormat()],
                                            @"refs/heads", @"refs/remotes", @"refs/tags"]);
    // Everything the graph depends on, so an unchanged repository (most
    // refreshes come from saving a file) costs one for-each-ref.
    NSMutableData *key = [NSMutableData data];
    NSString *head = [NSString stringWithFormat:@"%@ %@ %d %ld %d %@\n", s.headOid,
                                                s.branch ?: @"", compare, (long)limit, all,
                                                s.upstream ?: @""];
    [key appendData:[head dataUsingEncoding:NSUTF8StringEncoding]];
    [key appendData:refs.output];
    if (previous && [previous.key isEqualToData:key]) return previous;

    MCGitGraph *g = [MCGitGraph new];
    g.key = key;
    g.limit = limit;
    NSMutableArray *args = [@[@"-c", @"log.showSignature=false", @"log", @"-z",
                              @"--topo-order", @"--no-color",
                              [NSString stringWithFormat:@"--format=%s", Git::logFormat()],
                              [NSString stringWithFormat:@"--max-count=%ld", (long)limit]]
                               mutableCopy];
    if (all) [args addObjectsFromArray:@[@"--branches", @"--remotes"]];
    [args addObject:@"HEAD"];
    if (compare) [args addObject:@"@{upstream}"];
    [args addObject:@"--"];
    MCGitResult *log = MCGitRunSync(dir, args);
    if (!log.ok) {
        g.errorText = MCGitFailureText(log);
        return g;
    }
    g->commits = Git::parseLog(std::string((const char *)log.output.bytes, log.output.length));
    g.hasMore = (NSInteger)g->commits.size() >= limit;
    g->rows = Git::layoutGraph(g->commits);
    g->maxWidth = 1;
    for (const Git::GraphRow &r : g->rows) g->maxWidth = std::max(g->maxWidth, r.width);
    g->head = s.headOid.UTF8String;
    std::string refBytes((const char *)refs.output.bytes, refs.output.length);
    g->labels = Git::labelsByCommit(Git::parseRefs(refBytes), g->head,
                                    s.branch ? std::string(s.branch.UTF8String) : "",
                                    s.detached);
    if (compare && (s.ahead || s.behind)) {
        MCGitResult *lr = MCGitRunSync(dir, @[@"rev-list", @"--left-right",
                                              @"HEAD...@{upstream}", @"--"]);
        if (lr.ok)
            g->divergence = Git::parseLeftRight(
                std::string((const char *)lr.output.bytes, lr.output.length));
    }
    return g;
}

// What is pushed, in one line over the graph.
- (NSString *)summaryFor:(MCGitSnapshot *)s {
    if (s.detached) return @"HEAD is detached, so there is no upstream to compare with.";
    if (!s.upstream.length)
        return [NSString stringWithFormat:@"%@ has no upstream, so nothing here is marked "
                                           "as pushed or not.", s.branch ?: @"This branch"];
    if (!s.hasAheadBehind)
        return [NSString stringWithFormat:@"The upstream %@ is gone.", s.upstream];
    if (!s.ahead && !s.behind)
        return [NSString stringWithFormat:@"Up to date with %@.", s.upstream];
    NSMutableArray *parts = [NSMutableArray array];
    if (s.ahead) [parts addObject:[NSString stringWithFormat:@"↑ %d to push", s.ahead]];
    if (s.behind) [parts addObject:[NSString stringWithFormat:@"↓ %d to pull", s.behind]];
    return [NSString stringWithFormat:@"%@, against %@", [parts componentsJoinedByString:@", "],
                                      s.upstream];
}

- (void)applyGraph:(MCGitGraph *)g {
    if (g == _graph) {
        if (self.onGraphLoaded) self.onGraphLoaded();
        return;
    }
    // Keep the selection on the same commit.
    std::string was;
    NSInteger sel = _graphTable.selectedRow;
    if (_graph && sel >= 0 && sel < (NSInteger)_graph->commits.size())
        was = _graph->commits[sel].hash;
    BOOL wasMoreRow = _graph && sel == (NSInteger)_graph->commits.size();
    _graph = g;
    [_graphTable reloadData];
    NSInteger pick = -1;
    for (size_t i = 0; !was.empty() && i < g->commits.size(); i++)
        if (g->commits[i].hash == was) { pick = (NSInteger)i; break; }
    // After "Show more", the first of the new commits.
    if (pick < 0 && wasMoreRow && sel < (NSInteger)g->commits.size()) pick = sel;
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
    if (!MCGitExecutable()) {
        s.notice = @"Git was not found. Install the command line tools "
                    "(xcode-select --install) or git from Homebrew.";
        return s;
    }
    MCGitResult *top = MCGitRunSync(root, @[@"rev-parse", @"--show-toplevel"]);
    if (!top.ok) {
        if ([top.errorText rangeOfString:@"not a git repository"].location != NSNotFound)
            s.notice = @"This folder is not in a git repository.";
        else
            s.errorText = MCGitFailureText(top);
        return s;
    }
    std::string t((const char *)top.output.bytes, top.output.length);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    s.topLevel = NSFromBytes(t);
    s.inRepository = YES;
    MCGitResult *st = MCGitRunSync(s.topLevel, @[@"status", @"--porcelain=v2", @"--branch",
                                                 @"-z", @"--untracked-files=all"]);
    if (!st.ok) {
        s.errorText = MCGitFailureText(st);
        return s;
    }
    Git::Status status = Git::parseStatus(
        std::string((const char *)st.output.bytes, st.output.length));
    s.initial = status.initial;
    s.headOid = status.oid.empty() ? nil : NSFromBytes(status.oid);
    s.branch = status.branch.empty() ? nil : NSFromBytes(status.branch);
    s.detached = status.detached;
    s.upstream = status.upstream.empty() ? nil : NSFromBytes(status.upstream);
    s.hasAheadBehind = status.hasAheadBehind;
    s.ahead = status.ahead;
    s.behind = status.behind;
    NSString *branch = NSFromBytes(Git::branchLabel(status));
    if (status.initial) branch = [branch stringByAppendingString:@"  (no commits yet)"];
    s.branchText = branch;

    NSMutableArray<MCGitRow *> *staged = [NSMutableArray array];
    NSMutableArray<MCGitRow *> *changes = [NSMutableArray array];
    for (const Git::Entry &e : status.entries) {
        if (Git::isStaged(e)) {
            MCGitRow *r = [MCGitRow new];
            r.staged = YES;
            r.letter = (unichar)Git::stagedLetter(e);
            r.path = NSFromBytes(e.path);
            r.origPath = e.origPath.empty() ? nil : NSFromBytes(e.origPath);
            [staged addObject:r];
        }
        if (Git::hasUnstaged(e)) {
            MCGitRow *r = [MCGitRow new];
            r.letter = (unichar)Git::unstagedLetter(e);
            r.path = NSFromBytes(e.path);
            r.untracked = e.untracked;
            r.unmerged = e.unmerged;
            [changes addObject:r];
        }
    }
    NSMutableArray<MCGitRow *> *rows = [NSMutableArray array];
    void (^section)(NSString *, NSArray<MCGitRow *> *, BOOL) =
        ^(NSString *title, NSArray<MCGitRow *> *items, BOOL isStaged) {
            if (!items.count) return;
            MCGitRow *h = [MCGitRow new];
            h.header = YES;
            h.staged = isStaged;
            h.title = [NSString stringWithFormat:@"%@  %lu", title,
                                                 (unsigned long)items.count];
            [rows addObject:h];
            [rows addObjectsFromArray:items];
        };
    section(@"Staged changes", staged, YES);
    section(@"Changes", changes, NO);
    s.rows = rows;
    if (!rows.count) s.notice = @"No changes.";
    return s;
}

- (void)applySnapshot:(MCGitSnapshot *)s {
    // Keep the selection on the same file, or failing that at the same place
    // in the same list, so Space can be pressed down a list of files.
    NSInteger sel = _table.selectedRow;
    MCGitRow *was = sel >= 0 && sel < (NSInteger)_rows.count ? _rows[sel] : nil;
    NSInteger wasIndex = 0;
    if (was) {
        for (NSInteger i = sel; i >= 0 && !_rows[i].header; i--) wasIndex = sel - i;
    }

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
    BOOL graph = s.inRepository && !s.initial && s.headOid && !s.errorText.length;
    [self showGraph:graph];
    _graphSummary = graph ? [self summaryFor:s] : nil;
    _summaryLabel.stringValue = _graphSummary ?: @"";
    _summaryLabel.toolTip = _graphSummary;
    if (!graph && _graph) {
        _graph = nil;
        [_graphTable reloadData];
    }

    NSInteger pick = -1, n = (NSInteger)_rows.count;
    if (was) {
        for (NSInteger i = 0; i < n; i++)
            if (!_rows[i].header && [[_rows[i] key] isEqualToString:[was key]]) { pick = i; break; }
        if (pick < 0) {
            // The file left its list: the row now at its place in that list
            // (or the list's last), else the first file anywhere.
            NSInteger head = -1;
            for (NSInteger i = 0; i < n; i++)
                if (_rows[i].header && _rows[i].staged == was.staged) { head = i; break; }
            if (head >= 0) {
                NSInteger count = 0;
                while (head + 1 + count < n && !_rows[head + 1 + count].header) count++;
                if (count) pick = head + 1 + MIN(wasIndex, count - 1);
            }
            for (NSInteger i = 0; pick < 0 && i < n; i++)
                if (!_rows[i].header) pick = i;
        }
    }
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
    NSString *dir = _topLevel, *root = self.root;
    if (!dir) return;
    dispatch_async(_queue, ^{
        MCGitResult *r = MCGitRunSync(dir, args);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (![root isEqualToString:self.root]) return;
            if (r.ok) [self setError:nil info:NO];
            else [self setError:MCGitFailureText(r) info:NO];
            if (done) done(r);
            [self runRefresh];
        });
    });
}

- (void)toggleStageAtRow:(NSInteger)row {
    MCGitRow *r = [self fileRow:row];
    if (!r) { NSBeep(); return; }
    NSMutableArray *args;
    if (r.staged) {
        if (_initial) {
            // restore --staged needs a HEAD; before the first commit rm
            // --cached does the same. --force only lets it drop an entry that
            // differs from the file; with --cached the file is never touched.
            args = [@[@"--literal-pathspecs", @"rm", @"--cached", @"--force", @"-q", @"--",
                      r.path] mutableCopy];
        } else {
            args = [@[@"--literal-pathspecs", @"restore", @"--staged", @"--", r.path]
                       mutableCopy];
            if (r.origPath) [args addObject:r.origPath];   // both halves of a rename
        }
    } else {
        // -A so a deleted file is staged as a deletion.
        args = [@[@"--literal-pathspecs", @"add", @"-A", @"--", r.path] mutableCopy];
    }
    [self runAction:args done:nil];
}

- (void)commit {
    if (!_inRepository) return;
    NSString *msg = _message.string ?: @"";
    [self runAction:@[@"commit", @"-m", msg] done:^(MCGitResult *r) {
        if (!r.ok) return;
        [self->_message setString:@""];
        [self->_message setNeedsDisplay:YES];
        std::string out((const char *)r.output.bytes, r.output.length);
        size_t nl = out.find('\n');
        if (nl != std::string::npos) out.resize(nl);
        [self setError:NSFromBytes(out) info:YES];   // "[main 1a2b3c4] Message"
    }];
}

- (void)showDiffAtRow:(NSInteger)row {
    MCGitRow *r = [self fileRow:row];
    if (!r || !_topLevel) return;
    NSMutableArray *args = [@[@"-c", @"core.quotePath=false", @"--literal-pathspecs",
                              @"diff", @"--no-color", @"--no-ext-diff",
                              @"--src-prefix=a/", @"--dst-prefix=b/"] mutableCopy];
    if (r.untracked) {
        // The whole file as added lines. Exit status 1 means "differs".
        [args addObjectsFromArray:@[@"--no-index", @"--", @"/dev/null", r.path]];
    } else if (r.staged) {
        [args addObjectsFromArray:@[@"--cached", @"-M", @"--", r.path]];
        if (r.origPath) [args addObject:r.origPath];
    } else {
        [args addObjectsFromArray:@[@"--", r.path]];
    }
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
    if (row >= (NSInteger)_graph->commits.size()) {
        if (_graph.hasMore) [self showMore];
        return;
    }
    const Git::Commit &c = _graph->commits[row];
    NSString *hash = NSFromBytes(c.hash);
    NSString *title = [NSString stringWithFormat:@"%@ %@", NSFromBytes(Git::shortHash(c.hash)),
                                                 NSFromBytes(c.subject)];
    NSArray *args = @[@"-c", @"core.quotePath=false", @"show", @"--no-color", @"--no-ext-diff",
                      @"--src-prefix=a/", @"--dst-prefix=b/", @"-M",
                      @"--diff-merges=first-parent", @"--stat", @"--patch",
                      [NSString stringWithFormat:@"--format=%s", Git::showFormat()],
                      hash, @"--"];
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
    for (size_t i = 0; i < _graph->commits.size(); i++) {
        const Git::Commit &c = _graph->commits[i];
        NSMutableString *line = [NSMutableString stringWithFormat:@"%@ lane=%d",
            NSFromBytes(Git::shortHash(c.hash)), _graph->rows[i].lane];
        if (c.hash == _graph->head) [line appendString:@" head"];
        if (_graph->divergence.outgoing.count(c.hash)) [line appendString:@" out"];
        if (_graph->divergence.incoming.count(c.hash)) [line appendString:@" in"];
        [line appendFormat:@" %@", NSFromBytes(c.subject)];
        auto it = _graph->labels.find(c.hash);
        if (it != _graph->labels.end()) {
            NSMutableArray *names = [NSMutableArray array];
            for (const Git::Ref &r : it->second)
                [names addObject:[NSString stringWithFormat:@"%@%@",
                                  r.current ? @"*" : @"", NSFromBytes(r.name)]];
            [line appendFormat:@" [%@]", [names componentsJoinedByString:@","]];
        }
        [out addObject:line];
    }
    if (_graph.hasMore) [out addObject:@"(more)"];
    return out;
}

- (NSString *)graphToolTip:(NSInteger)row {
    if (row >= (NSInteger)_graph->commits.size())
        return [NSString stringWithFormat:@"Load the next %ld commits", (long)kGraphBatch];
    const Git::Commit &c = _graph->commits[row];
    NSMutableString *t = [NSMutableString stringWithFormat:@"%@  %@, %@ (%@)\n%@",
        NSFromBytes(Git::shortHash(c.hash)), NSFromBytes(c.author), MCGitRelativeText(c.time),
        MCGitDateText(c.time), NSFromBytes(c.subject)];
    if (_graph->divergence.outgoing.count(c.hash))
        [t appendString:@"\nNot pushed yet: on this branch but not its upstream."];
    if (_graph->divergence.incoming.count(c.hash))
        [t appendString:@"\nNot pulled yet: on the upstream but not this branch."];
    auto it = _graph->labels.find(c.hash);
    if (it != _graph->labels.end()) {
        NSMutableArray *names = [NSMutableArray array];
        for (const Git::Ref &r : it->second) [names addObject:NSFromBytes(r.name)];
        [t appendFormat:@"\n%@", [names componentsJoinedByString:@", "]];
    }
    return t;
}

// ------------------------------------------------------------ table view
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv {
    if (tv == _graphTable)
        return _graph ? (NSInteger)_graph->commits.size() + (_graph.hasMore ? 1 : 0) : 0;
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
    if (r.header) {
        cell.toolTip = nil;
    } else {
        NSString *what = r.unmerged ? @"conflict" : r.untracked ? @"untracked"
                       : r.staged ? @"staged" : @"changed";
        cell.toolTip = r.origPath
            ? [NSString stringWithFormat:@"%@ (renamed from %@), %@", r.path, r.origPath, what]
            : [NSString stringWithFormat:@"%@, %@", r.path, what];
    }
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
