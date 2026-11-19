// EditorController+PageTabs.mm — EditorController: the tabs that show no
// file, as the sessions keep them (TabSessions::PageTab, 2026-11-18): the
// review of uncommitted changes ("Uncommitted Changes"), a commit's page,
// and a side-by-side comparison. None of them keeps what it showed: each
// comes back with its window or folder by being made again from git and
// the disk (the review and the commit page read when first shown; a
// comparison's two sides are read off the main thread and lined up there).
// A worktree's page, a conflict's two sides, an agent's proposed change and
// the agents' preview tabs do not come back.
#import "EditorController+Private.h"
#import "ChangeReviewPage.h"
#import "GitPanel.h"
#include <stdlib.h>

namespace {
std::string Utf8(NSString *s) { return s.UTF8String ?: ""; }
NSString *NS(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

// The largest side read back, as `mutible diff` reads.
const unsigned long long kMaxSideBytes = 16ull * 1024 * 1024;

// A path with its links resolved the way git names files (realpath keeps
// /private, which stringByResolvingSymlinksInPath drops); a file gone since
// is its folder's real path and its name.
NSString *RealPath(NSString *path) {
    char buf[PATH_MAX];
    if (realpath(path.fileSystemRepresentation, buf)) return @(buf);
    NSString *dir = path.stringByDeletingLastPathComponent;
    if (realpath(dir.fileSystemRepresentation, buf))
        return [@(buf) stringByAppendingPathComponent:path.lastPathComponent];
    return path;
}

// One side's text: the file on disk (rev ""), or git's at a revision or in
// the index (":"). A file not there (added since, deleted, not in that
// commit) or not text reads as empty, as the page showed it.
NSString *SideText(NSString *path, NSString *rev) {
    if (!rev.length) {
        NSDictionary *a = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
        if (![a.fileType isEqualToString:NSFileTypeRegular] || a.fileSize > kMaxSideBytes) return @"";
        NSData *d = [NSData dataWithContentsOfFile:path];
        return (d ? [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] : nil) ?: @"";
    }
    NSString *real = RealPath(path);
    NSString *dir = real.stringByDeletingLastPathComponent;
    BOOL isDir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:dir isDirectory:&isDir] || !isDir) return @"";
    MCGitResult *top = MCGitRunSync(dir, @[@"rev-parse", @"--show-toplevel"]);
    if (!top.ok) return @"";
    NSString *root = [[[NSString alloc] initWithData:top.output encoding:NSUTF8StringEncoding]
        stringByTrimmingCharactersInSet:NSCharacterSet.newlineCharacterSet];
    if (!root.length || ![real hasPrefix:[root stringByAppendingString:@"/"]]) return @"";
    NSString *rel = [real substringFromIndex:root.length + 1];
    NSString *spec = [rev isEqualToString:@":"] ? [@":" stringByAppendingString:rel]
                                               : [NSString stringWithFormat:@"%@:%@", rev, rel];
    MCGitResult *show = MCGitRunSync(root, @[@"show", spec]);
    if (!show.ok || show.output.length > kMaxSideBytes) return @"";
    return [[NSString alloc] initWithData:show.output encoding:NSUTF8StringEncoding] ?: @"";
}
}  // namespace

@implementation EditorController (PageTabs)

- (BOOL)pageTabOf:(MCTab *)t into:(TabSessions::PageTab *)out {
    if (!t.isDiff || t.agentPreview) return NO;
    TabSessions::PageTab p;
    if ([t.page isKindOfClass:MCChangeReviewPage.class]) {
        MCChangeReviewPage *page = (MCChangeReviewPage *)t.page;
        if (page.worktree) return NO;
        if (page.commit.length) {
            p.kind = "commit";
            p.ref = Utf8(page.commit);
            p.title = Utf8(page.pageTitle);
        } else {
            p.kind = "review";
        }
    } else if (t.comparison && !t.page) {
        MCComparison *c = t.comparison;
        if (!c.leftPath.length || !c.rightPath.length || c.decisionPrompt) return NO;
        p.kind = "compare";
        p.title = Utf8(c.title);
        p.fileName = Utf8(c.fileName ?: c.path);
        p.left = TabSessions::ComparedSide{Utf8(c.leftPath), Utf8(c.leftRev), Utf8(c.leftLabel)};
        p.right = TabSessions::ComparedSide{Utf8(c.rightPath), Utf8(c.rightRev), Utf8(c.rightLabel)};
        if (!TabSessions::isComparedRev(p.left.rev) || !TabSessions::isComparedRev(p.right.rev))
            return NO;   // a revision the record cannot carry (HEAD@{1}): not brought back
    } else {
        return NO;
    }
    *out = p;
    return YES;
}

- (MCTab *)tabForRestoredPage:(const TabSessions::PageTab &)p {
    MCTab *tab = [MCTab new];
    tab.loaded = YES;
    tab.isDiff = YES;
    if (p.kind == "review" || p.kind == "commit") {
        // One review per window and one page per commit: a second in the
        // record is left out.
        NSString *key = p.kind == "review"
            ? [@"review:" stringByAppendingString:_root.path ?: @""]
            : [MCChangeReviewPage commitKeyForRoot:_root.path hash:NS(p.ref)];
        for (MCTab *t in [self tabs])
            if ([t.page.pageKey isEqualToString:key]) return nil;
        MCChangeReviewPage *page = p.kind == "review" ? [self newChangeReviewPage] : [self newCommitReviewPage];
        if (p.kind == "commit") [page showCommit:NS(p.ref) title:NS(p.title)];
        tab.page = page;
        tab.diffName = page.pageTitle;
        return tab;
    }
    if (p.kind != "compare") return nil;
    MCComparison *c = [MCComparison new];
    c.title = NS(p.title);
    c.fileName = p.fileName.empty() ? nil : NS(p.fileName);
    c.leftLabel = NS(p.left.label);
    c.rightLabel = NS(p.right.label);
    c.leftPath = NS(p.left.path);
    c.leftRev = NS(p.left.rev);
    c.rightPath = NS(p.right.path);
    c.rightRev = NS(p.right.rev);
    c.path = p.right.rev.empty() ? c.rightPath : nil;   // the file on the right, when it is one
    c.leftText = @"";
    c.rightText = @"";
    tab.comparison = c;
    tab.diffName = c.title;
    [self compareViewMade];
    [self remakeComparisonOfTab:tab];
    return tab;
}

// The two sides read again, off the main thread, and lined up there; the
// tab then shows them (at once when it is on show).
- (void)remakeComparisonOfTab:(MCTab *)tab {
    MCComparison *shell = tab.comparison;
    NSString *lp = shell.leftPath, *lr = shell.leftRev ?: @"", *rp = shell.rightPath, *rr = shell.rightRev ?: @"";
    NSString *title = shell.title, *ll = shell.leftLabel, *rl = shell.rightLabel;
    NSString *fileName = shell.fileName, *path = shell.path;
    __weak EditorController *weakSelf = self;
    __weak MCTab *weakTab = tab;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        MCComparison *c = [MCComparison new];
        c.title = title;
        c.leftLabel = ll;
        c.rightLabel = rl;
        c.fileName = fileName;
        c.path = path;
        c.leftPath = lp;
        c.leftRev = lr;
        c.rightPath = rp;
        c.rightRev = rr;
        c.leftText = SideText(lp, lr);
        c.rightText = SideText(rp, rr);
        [c prepare];
        dispatch_async(dispatch_get_main_queue(), ^{
            EditorController *s = weakSelf;
            MCTab *t = weakTab;
            if (!s || !t || t.comparison != shell || ![s holdsTab:t]) return;
            t.comparison = c;
            if (t == s->_activeTab) [s renderDiff];
        });
    });
}

@end
