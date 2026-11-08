// GitGutter.mm — see GitGutter.h.
#import "GitGutter.h"
#import "AppSettings.h"
#import "EditorFont.h"
#import "GitPanel.h"
#import "Gutter.h"
#import "Lsp.h"
#include "GitModel.h"
#import <QuartzCore/QuartzCore.h>
#include "QuickDiff.h"
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

std::string Bytes(NSData *d) {
    return d.length ? std::string((const char *)d.bytes, d.length) : std::string();
}

NSArray<NSString *> *Args(const std::vector<std::string> &v) {
    NSMutableArray *a = [NSMutableArray arrayWithCapacity:v.size()];
    for (const std::string &s : v) [a addObject:[NSString stringWithUTF8String:s.c_str()] ?: @""];
    return a;
}

std::u16string U16(NSString *s) {
    std::u16string out(s.length, u'\0');
    if (s.length) [s getCharacters:(unichar *)out.data() range:NSMakeRange(0, s.length)];
    return out;
}

NSString *NS(const std::u16string &s) {
    return [NSString stringWithCharacters:(const unichar *)s.data() length:s.size()];
}

// What the index file's stat says: a stage, a commit or a checkout from
// anywhere changes it.
struct Stamp {
    long long sec = -1, nsec = 0, size = 0, ino = 0;
    bool operator==(const Stamp &o) const {
        return sec == o.sec && nsec == o.nsec && size == o.size && ino == o.ino;
    }
};

Stamp StampOf(const std::string &path) {
    Stamp s;
    struct stat st;
    if (path.empty() || stat(path.c_str(), &st) != 0) return s;
    s.sec = st.st_mtimespec.tv_sec;
    s.nsec = st.st_mtimespec.tv_nsec;
    s.size = st.st_size;
    s.ino = (long long)st.st_ino;
    return s;
}

std::string RealPath(NSString *p) {
    char buf[PATH_MAX];
    return realpath(p.fileSystemRepresentation, buf) ? std::string(buf) : std::string();
}

}  // namespace

// What the queue knows of the file last looked at: its repository, its
// index entry and the text that entry names. Touched only on the queue.
@interface MCGitGutterSource : NSObject {
@public
    std::string path;          // as asked
    std::string top, gitDir, rel;
    bool inRepository;
    Stamp stamp;               // the index file when the entry was read
    GitUi::IndexEntry entry;
    std::u16string original;   // the entry's text
    bool hasOriginal;
}
@end
@implementation MCGitGutterSource
@end

@interface MCGitGutter () <NSPopoverDelegate>
@end

@implementation MCGitGutter {
    __weak id<MCGitGutterHost> _host;
    dispatch_queue_t _queue;
    NSUInteger _generation;            // every edit and every look
    BOOL _scheduled;
    CFTimeInterval _lastEdit;          // the look waits until typing has paused
    BOOL _pauseWanted;                 // ...when an edit asked for it
    NSString *_shownPath;              // the file the bars are of
    MCGitGutterSource *_source;        // the queue's (read on the main thread only as a copy)
    // What the bars on show were made from, for Revert and Stage.
    std::u16string _original;
    std::string _top, _rel;
    GitUi::IndexEntry _entry;
    NSPopover *_popover;
    NSInteger _popoverBar;
}

- (instancetype)initWithHost:(id<MCGitGutterHost>)host {
    if ((self = [super init])) {
        _host = host;
        _queue = dispatch_queue_create("minicode.git-gutter", DISPATCH_QUEUE_SERIAL);
        _popoverBar = -1;
        CodeTextView *tv = host.gitGutterTextView;
        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
        [nc addObserver:self selector:@selector(storageEdited:)
                   name:NSTextStorageDidProcessEditingNotification object:tv.textStorage];
        [nc addObserver:self selector:@selector(settingsChanged:)
                   name:MCSettingsDidChangeNotification object:nil];
        [nc addObserver:self selector:@selector(windowBecameKey:)
                   name:NSWindowDidBecomeKeyNotification object:nil];
        __weak MCGitGutter *weakSelf = self;
        host.gitGutterView.onChangeBar = ^(NSInteger index, NSRect rect) {
            [weakSelf showChange:index rect:rect];
        };
        [self applySettings];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_popover close];
}

- (void)applySettings {
    MCLineGutter *g = _host.gitGutterView;
    const Settings &st = [AppSettings shared].settings;
    g.changeLane = st.gitGutter();
    [g setChangeColorsAdded:MCColor(st.gitGutterColor(0)) modified:MCColor(st.gitGutterColor(1))
                    deleted:MCColor(st.gitGutterColor(2))];
}

- (void)settingsChanged:(NSNotification *)note {
    (void)note;
    [self applySettings];
    [self refreshSoon];
}

- (void)windowBecameKey:(NSNotification *)note {
    if (note.object == _host.gitGutterTextView.window) [self refreshSoon];
}

- (void)storageEdited:(NSNotification *)note {
    NSTextStorage *ts = note.object;
    if (!(ts.editedMask & NSTextStorageEditedCharacters)) return;
    _generation++;
    _lastEdit = CACurrentMediaTime();
    [self closePopover];
    // Another file put in the view: its bars come with the next look, soon.
    NSString *path = _host.gitGutterPath;
    const BOOL other = !(path == _shownPath || [path isEqualToString:_shownPath]);
    if (other) {
        std::vector<QuickDiff::Bar> none;
        [_host.gitGutterView setChangeBars:none];
    }
    if (!other) _pauseWanted = YES;
    [self scheduleAfter:other ? 0.05 : 0.3];
}

- (void)refreshSoon { [self scheduleAfter:0.05]; }

- (void)scheduleAfter:(NSTimeInterval)delay {
    if (_scheduled) return;
    _scheduled = YES;
    __weak MCGitGutter *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        MCGitGutter *s = weakSelf;
        if (!s) return;
        s->_scheduled = NO;
        // Still typing: wait for the pause (one timer, not one a key).
        const CFTimeInterval since = CACurrentMediaTime() - s->_lastEdit;
        if (s->_pauseWanted && since < 0.3) {
            [s scheduleAfter:0.3 - since + 0.01];
            return;
        }
        s->_pauseWanted = NO;
        [s look];
    });
}

// On the main thread: what to diff; on the queue: the original (read
// again only when the file or the index changed) and the diff; back on the
// main thread, the bars, if nothing was typed meanwhile.
- (void)look {
    id<MCGitGutterHost> host = _host;
    MCLineGutter *g = host.gitGutterView;
    CodeTextView *tv = host.gitGutterTextView;
    NSString *path = [AppSettings shared].settings.gitGutter() && MCGitExecutable()
                         ? host.gitGutterPath : nil;
    const NSUInteger gen = ++_generation;
    _looks++;
    if (!path || !tv || !g) {
        _shownPath = path;
        std::vector<QuickDiff::Bar> none;
        [g setChangeBars:none];
        return;
    }
    NSString *text = [tv.textStorage.string copy];
    MCGitGutterSource *previous = _source;
    __weak MCGitGutter *weakSelf = self;
    dispatch_async(_queue, ^{
        MCGitGutterSource *src = [MCGitGutter sourceFor:path previous:previous];
        std::vector<QuickDiff::Bar> bars;
        if (src->hasOriginal) bars = QuickDiff::bars(src->original, U16(text));
        dispatch_async(dispatch_get_main_queue(), ^{
            MCGitGutter *s = weakSelf;
            if (!s) return;
            s->_source = src;
            if (gen != s->_generation) return;   // typed meanwhile: the next look
            if (![path isEqualToString:s->_host.gitGutterPath]) return;
            s->_shownPath = path;
            s->_original = src->original;
            s->_top = src->top;
            s->_rel = src->rel;
            s->_entry = src->entry;
            [s->_host.gitGutterView setChangeBars:bars];
            s->_diffs++;
        });
    });
}

// Off the main thread. The previous source is reused while the file and
// the index file are as they were.
+ (MCGitGutterSource *)sourceFor:(NSString *)path previous:(MCGitGutterSource *)previous {
    const std::string want = path.UTF8String ?: "";
    MCGitGutterSource *s = [MCGitGutterSource new];
    s->path = want;
    if (previous && previous->path == want) {
        s->top = previous->top;
        s->gitDir = previous->gitDir;
        s->rel = previous->rel;
        s->inRepository = previous->inRepository;
        if (!s->inRepository) return s;
    } else {
        // Its own repository (a submodule's or a nested one's when it is
        // in one), and its path there, links resolved as git has them.
        NSString *dir = path.stringByDeletingLastPathComponent;
        MCGitResult *r = MCGitRunSync(dir, Args(GitUi::topLevelArgs()));
        if (!r.ok || !GitUi::parseTopLevel(Bytes(r.output), s->top, s->gitDir)) return s;
        const std::string full = RealPath(path);
        const std::string top = RealPath([NSString stringWithUTF8String:s->top.c_str()]);
        if (full.empty() || top.empty() || full.compare(0, top.size() + 1, top + "/") != 0)
            return s;
        s->rel = full.substr(top.size() + 1);
        s->inRepository = true;
    }
    s->stamp = StampOf(s->gitDir + "/index");
    if (previous && previous->path == want && previous->stamp == s->stamp &&
        previous->stamp.sec >= 0) {
        s->entry = previous->entry;
        s->original = previous->original;
        s->hasOriginal = previous->hasOriginal;
        return s;
    }
    NSString *top = [NSString stringWithUTF8String:s->top.c_str()];
    MCGitResult *ls = MCGitRunSync(top, Args(GitUi::indexEntryArgs(s->rel)));
    if (!ls.ok) return s;
    s->entry = GitUi::parseIndexEntry(Bytes(ls.output));
    if (!s->entry.ok) return s;   // untracked, a conflict, a link, a submodule
    MCGitResult *blob = MCGitRunSync(top, Args(GitUi::blobArgs(s->entry.object)));
    if (!blob.ok) return s;
    s->original = QuickDiff::fromUtf8(Bytes(blob.output));
    s->hasOriginal = true;
    return s;
}

// ------------------------------------------------------------- for tests
- (NSUInteger)barCount { return _host.gitGutterView.changeBars.size(); }

- (NSArray<NSString *> *)barDescriptions {
    NSMutableArray *out = [NSMutableArray array];
    for (const QuickDiff::Bar &b : _host.gitGutterView.changeBars) {
        const char *k = b.kind == QuickDiff::Kind::Added ? "added"
                      : b.kind == QuickDiff::Kind::Modified ? "modified" : "deleted";
        [out addObject:[NSString stringWithFormat:@"%s %zu+%zu (was %zu+%zu)", k, b.line, b.lines,
                                                  b.oldLine, b.oldLines]];
    }
    return out;
}

// ------------------------------------------------------------- moving
- (NSUInteger)caretLine {
    CodeTextView *tv = _host.gitGutterTextView;
    return [_host.gitGutterView lineOfCharacter:tv.selectedRange.location];
}

- (BOOL)goToNextChange:(BOOL)forward {
    MCLineGutter *g = _host.gitGutterView;
    CodeTextView *tv = _host.gitGutterTextView;
    const std::vector<QuickDiff::Bar> &bars = g.changeBars;
    const int at = forward ? QuickDiff::nextBar(bars, self.caretLine)
                           : QuickDiff::previousBar(bars, self.caretLine);
    if (at < 0 || !tv) { NSBeep(); return NO; }
    const QuickDiff::Bar &b = bars[(size_t)at];
    // A deletion's caret goes to the line below it, where the old lines were.
    const size_t line = b.kind == QuickDiff::Kind::Deleted ? MIN(b.line, g.lineCount - 1)
                                                           : b.line;
    const NSUInteger start = [g startOfLine:line];
    [tv setSelectedRange:NSMakeRange(MIN(start, tv.string.length), 0)];
    [tv scrollRangeToVisible:tv.selectedRange];
    return YES;
}

- (NSInteger)barAtCaret {
    return QuickDiff::barAt(_host.gitGutterView.changeBars, self.caretLine);
}

// ------------------------------------------------------------- acting
- (BOOL)revertChange:(NSInteger)index {
    MCLineGutter *g = _host.gitGutterView;
    CodeTextView *tv = _host.gitGutterTextView;
    const std::vector<QuickDiff::Bar> &bars = g.changeBars;
    if (index < 0 || (size_t)index >= bars.size() || !tv || !tv.editable) {
        NSBeep();
        return NO;
    }
    const QuickDiff::Bar b = bars[(size_t)index];
    const std::u16string buffer = U16(tv.string);
    const QuickDiff::Edit e = QuickDiff::revertEdit(b, _original, buffer);
    if (e.start + e.length > buffer.size()) { NSBeep(); return NO; }
    [self closePopover];
    return [tv replaceRanges:@[[NSValue valueWithRange:NSMakeRange(e.start, e.length)]]
                 withStrings:@[NS(e.text)]
                  actionName:@"Revert Change"];
}

- (BOOL)stageChange:(NSInteger)index {
    MCLineGutter *g = _host.gitGutterView;
    CodeTextView *tv = _host.gitGutterTextView;
    const std::vector<QuickDiff::Bar> &bars = g.changeBars;
    if (index < 0 || (size_t)index >= bars.size() || !tv || _top.empty() || !_entry.ok) {
        NSBeep();
        return NO;
    }
    [self closePopover];
    const std::string staged = QuickDiff::toUtf8(
        QuickDiff::stagedText(bars[(size_t)index], _original, U16(tv.string)));
    NSString *top = [NSString stringWithUTF8String:_top.c_str()];
    const std::string rel = _rel;
    const GitUi::IndexEntry entry = _entry;
    __weak MCGitGutter *weakSelf = self;
    dispatch_async(_queue, ^{
        NSString *failure = nil;
        // The index must still hold what the bars were made from.
        MCGitResult *ls = MCGitRunSync(top, Args(GitUi::indexEntryArgs(rel)));
        const GitUi::IndexEntry now = GitUi::parseIndexEntry(Bytes(ls.output));
        if (!ls.ok || !now.ok || now.object != entry.object) {
            failure = @"The staged version of this file changed since its bars were drawn, so "
                       "nothing was staged. The bars now show it as it is.";
        } else {
            NSData *data = [NSData dataWithBytes:staged.data() length:staged.size()];
            MCGitResult *h = MCGitRunSyncWithInput(top, Args(GitUi::hashObjectArgs(rel)), data);
            if (!h.ok) {
                failure = h.errorText.length ? h.errorText : @"git hash-object failed.";
            } else {
                MCGitResult *u = MCGitRunSync(
                    top, Args(GitUi::updateIndexArgs(entry, Bytes(h.output), rel)));
                if (!u.ok) failure = u.errorText.length ? u.errorText : @"git update-index failed.";
            }
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            MCGitGutter *s = weakSelf;
            if (!s) return;
            if (failure) {
                NSBeep();
                if (s.onNote) s.onNote(failure);
            } else if (s.onNote) {
                s.onNote([NSString stringWithFormat:@"Staged a change to %@.",
                                                    [NSString stringWithUTF8String:rel.c_str()]
                                                        .lastPathComponent]);
            }
            [s->_host gitGutterChangedIndex];
            [s refreshSoon];
            if (s.onStaged) s.onStaged(failure);
        });
    });
    return YES;
}

// ------------------------------------------------------------- the popover
- (void)showChange:(NSInteger)index rect:(NSRect)rect {
    MCLineGutter *g = _host.gitGutterView;
    const std::vector<QuickDiff::Bar> &bars = g.changeBars;
    if (index < 0 || (size_t)index >= bars.size()) return;
    const QuickDiff::Bar b = bars[(size_t)index];
    [self closePopover];
    _popoverBar = index;
    _popoverTitle = [NSString stringWithUTF8String:QuickDiff::describe(b).c_str()];
    NSMutableArray *lines = [NSMutableArray array];
    for (const std::u16string &l : QuickDiff::oldLines(b, _original)) [lines addObject:NS(l)];
    _popoverText = [lines componentsJoinedByString:@"\n"];

    const CGFloat w = 420;
    NSView *content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, w, 10)];
    NSTextField *title = [NSTextField labelWithString:_popoverTitle];
    title.font = [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold];
    NSButton *revert = [NSButton buttonWithTitle:@"Revert Change" target:self
                                          action:@selector(revertFromPopover:)];
    NSButton *stage = [NSButton buttonWithTitle:@"Stage Change" target:self
                                         action:@selector(stageFromPopover:)];
    revert.controlSize = stage.controlSize = NSControlSizeSmall;
    revert.font = stage.font = [NSFont systemFontOfSize:11];
    stage.enabled = _entry.ok;
    [revert sizeToFit];
    [stage sizeToFit];
    CGFloat textH = 0;
    NSScrollView *scroll = nil;
    if (_popoverText.length) {
        NSFont *mono = MCMonoFont(MAX(10.0, MCEditorFont().pointSize - 1), NSFontWeightRegular);
        const CGFloat lineH = ceil(mono.ascender - mono.descender + mono.leading) + 1;
        textH = MIN(lines.count, (NSUInteger)12) * lineH + 8;
        scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(8, 36, w - 16, textH)];
        scroll.hasVerticalScroller = lines.count > 12;
        scroll.hasHorizontalScroller = YES;
        scroll.autohidesScrollers = YES;
        scroll.drawsBackground = NO;
        NSTextView *tv = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, w - 16, textH)];
        tv.editable = NO;
        tv.selectable = YES;
        tv.drawsBackground = YES;
        tv.backgroundColor = [MCColor([AppSettings shared].settings.gitGutterColor(2))
            colorWithAlphaComponent:0.15];
        tv.font = mono;
        tv.textColor = NSColor.labelColor;
        tv.textContainerInset = NSMakeSize(2, 4);
        tv.horizontallyResizable = YES;
        tv.textContainer.widthTracksTextView = NO;
        tv.textContainer.containerSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
        tv.string = _popoverText;
        scroll.documentView = tv;
        [content addSubview:scroll];
    }
    const CGFloat H = 36 + textH + (textH ? 8 : 0);
    content.frame = NSMakeRect(0, 0, w, H);
    // Flipped by hand: the title and buttons along the top.
    title.frame = NSMakeRect(10, H - 26, w - 30 - NSWidth(revert.frame) - NSWidth(stage.frame), 16);
    stage.frame = NSMakeRect(w - 8 - NSWidth(stage.frame), H - 30, NSWidth(stage.frame), 24);
    revert.frame = NSMakeRect(NSMinX(stage.frame) - 4 - NSWidth(revert.frame), H - 30,
                              NSWidth(revert.frame), 24);
    if (scroll) scroll.frame = NSMakeRect(8, 8, w - 16, textH);
    [content addSubview:title];
    [content addSubview:revert];
    [content addSubview:stage];

    NSViewController *vc = [NSViewController new];
    vc.view = content;
    _popover = [NSPopover new];
    _popover.contentViewController = vc;
    _popover.behavior = NSPopoverBehaviorTransient;
    _popover.delegate = self;
    if (g.window && !NSIsEmptyRect(rect))
        [_popover showRelativeToRect:rect ofView:g preferredEdge:NSRectEdgeMaxX];
}

- (void)revertFromPopover:(id)sender {
    (void)sender;
    [self revertChange:_popoverBar];
}

- (void)stageFromPopover:(id)sender {
    (void)sender;
    [self stageChange:_popoverBar];
}

- (void)closePopover {
    if (_popover.shown) [_popover close];
    _popover = nil;
    _popoverBar = -1;
}

- (void)popoverDidClose:(NSNotification *)note {
    (void)note;
    _popover = nil;
}

@end
