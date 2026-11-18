// EditorLineHistory.mm — see EditorLineHistory.h.
#import "EditorLineHistory.h"
#import "AgentModel.h"
#import "EditorFont.h"
#import "GitPanel.h"
#import "Gutter.h"
#import "Lsp.h"
#import "TreeDetails.h"
#import <QuartzCore/QuartzCore.h>
#include "AgentEdits.h"
#include "GitGraph.h"
#include "LineHistory.h"
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <time.h>
#include <vector>

NSNotificationName const MCLineHistoryInlineDidChangeNotification =
    @"MCLineHistoryInlineDidChangeNotification";
static NSString *const kInlineKey = @"MCLineHistoryInline";

BOOL MCLineHistoryInline(void) { return [MCAppDefaults() boolForKey:kInlineKey]; }

void MCSetLineHistoryInline(BOOL on) {
    if (MCLineHistoryInline() == on) return;
    [MCAppDefaults() setBool:on forKey:kInlineKey];
    [NSNotificationCenter.defaultCenter postNotificationName:MCLineHistoryInlineDidChangeNotification
                                                      object:nil];
}

namespace {

NSString *NS(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

std::string Std(NSString *s) {
    const char *p = s.UTF8String;
    return p ? std::string(p) : std::string();
}

std::string Fs(NSString *s) {
    const char *p = s.length ? s.fileSystemRepresentation : nullptr;
    return p ? std::string(p) : std::string();
}

double ThreadCpuMs() {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// The Mac's way of writing a date (the git panel's): medium date, short time.
std::string MacDate(long long t) {
    static NSDateFormatter *f;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        f = [NSDateFormatter new];
        f.dateStyle = NSDateFormatterMediumStyle;
        f.timeStyle = NSDateFormatterShortStyle;
    });
    return [f stringFromDate:[NSDate dateWithTimeIntervalSince1970:(double)t]].UTF8String ?: "";
}

// One look's answer, kept with the text it was for.
struct Answer {
    std::vector<NSUInteger> newlines;   // UTF-16 offsets of every "\n" in the text
    bool hasBlame = false;
    LineHistory::Blame blame;
    struct Session {
        LineHistory::SessionInfo info;
        std::string file;               // the file as the session's calls named it
        std::vector<AgentEdits::Change> changes;
    };
    std::vector<Session> sessions;
    std::vector<LineHistory::Line> lines;
    std::string trouble;                // why nothing is known, when git could not say
};

// A session's calls on one file, kept while its transcript is unchanged.
struct Collected {
    uint64_t size = 0;
    double mtime = 0;
    std::vector<AgentEdits::Change> changes;
};

const CGFloat kLabelGapChars = 3;     // between a line's end and its words
const NSTimeInterval kHoverDelay = 0.5;
const NSTimeInterval kTypingPause = 0.6;

}  // namespace

// A file's last answer and the text it was for.
@interface MCLineHistoryEntry : NSObject {
@public
    std::shared_ptr<const Answer> answer;
}
@property(nonatomic, copy) NSString *path, *text;
@property(nonatomic) NSUInteger modelGeneration, gitGeneration;
@end
@implementation MCLineHistoryEntry
@end

// ------------------------------------------------------------------ the box
@interface MCLineHistoryPanel : NSPanel
@end
@implementation MCLineHistoryPanel
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

@interface MCLineHistoryBoxView : NSView
@property(nonatomic, copy) NSAttributedString *text;
@property(nonatomic, copy) void (^onClick)(void);
@property(nonatomic, copy) void (^onExit)(void);
@property(nonatomic, readonly) BOOL pointerInside;
@end
@implementation MCLineHistoryBoxView {
    NSTrackingArea *_area;
}
- (BOOL)isFlipped { return YES; }
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_area) [self removeTrackingArea:_area];
    _area = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways |
                     NSTrackingInVisibleRect | NSTrackingCursorUpdate
               owner:self userInfo:nil];
    [self addTrackingArea:_area];
}
- (void)mouseEntered:(NSEvent *)e { (void)e; _pointerInside = YES; }
- (void)mouseExited:(NSEvent *)e {
    (void)e;
    _pointerInside = NO;
    if (self.onExit) self.onExit();
}
- (void)cursorUpdate:(NSEvent *)e { (void)e; [NSCursor.pointingHandCursor set]; }
- (void)mouseDown:(NSEvent *)e {
    (void)e;
    if (self.onClick) self.onClick();
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    [[NSColor colorWithSRGBRed:0x25 / 255.0 green:0x25 / 255.0 blue:0x26 / 255.0 alpha:1] set];
    NSRectFill(self.bounds);
    [[NSColor colorWithSRGBRed:0x45 / 255.0 green:0x45 / 255.0 blue:0x45 / 255.0 alpha:1] set];
    NSFrameRect(self.bounds);
    [self.text drawWithRect:NSInsetRect(self.bounds, 9, 6)
                    options:NSStringDrawingUsesLineFragmentOrigin];
}
@end

// ------------------------------------------------------------------ the object
@implementation MCLineHistory {
    __weak id<MCLineHistoryHost> _host;
    dispatch_queue_t _queue;
    // The answer on show, the file and text it is for, and how far into
    // the text it still holds (edits since move this down).
    std::shared_ptr<const Answer> _answer;
    NSString *_answerPath;
    NSUInteger _validBefore;
    // A look under way: its number, and the first edit since it began.
    NSUInteger _lookNumber;
    BOOL _looking, _lookAgain;
    NSUInteger _editedSinceLook;
    BOOL _scheduled;
    CFTimeInterval _lastEdit;
    NSMutableDictionary<NSString *, MCLineHistoryEntry *> *_cache;   // by path
    NSMutableArray<NSString *> *_cacheOrder;
    NSUInteger _gitGeneration;
    __weak MCAgentModel *_model;
    // Only touched on _queue.
    std::map<std::string, Collected> _collected;
    // The box.
    MCLineHistoryPanel *_panel;
    MCLineHistoryBoxView *_box;
    __weak NSWindow *_parent;
    NSUInteger _hoverLine, _wantPopupLine;
    NSRect _hoverAnchor;   // in screen coordinates
    NSTimer *_hoverTimer, *_closeTimer;
    BOOL _stopped;
}

- (instancetype)initWithHost:(id<MCLineHistoryHost>)host {
    if ((self = [super init])) {
        _host = host;
        _queue = dispatch_queue_create("mutible.line-history", DISPATCH_QUEUE_SERIAL);
        _cache = [NSMutableDictionary dictionary];
        _cacheOrder = [NSMutableArray array];
        _hoverLine = _wantPopupLine = _popupLine = NSNotFound;
        _editedSinceLook = NSNotFound;
        CodeTextView *tv = host.lineHistoryTextView;
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        [nc addObserver:self selector:@selector(storageEdited:)
                   name:NSTextStorageDidProcessEditingNotification object:tv.textStorage];
        [nc addObserver:self selector:@selector(inlineChanged:)
                   name:MCLineHistoryInlineDidChangeNotification object:nil];
        [nc addObserver:self selector:@selector(gitRefreshed:)
                   name:MCGitPanelDidRefreshNotification object:nil];
        __weak MCLineHistory *weakSelf = self;
        host.lineHistoryGutter.onHoverLine = ^(NSUInteger line) { [weakSelf hoverLine:line]; };
        tv.drawOverText = ^(NSRect rect) { [weakSelf drawLabelsIn:rect]; };
        tv.onMouseDownFirst = ^BOOL(NSEvent *event) { return [weakSelf pressed:event]; };
        tv.onPointerMoved = ^(NSPoint p, BOOL inside) { [weakSelf pointerAt:p inside:inside]; };
        if (MCLineHistoryInline()) [self scheduleAfter:0.05];
    }
    return self;
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)stop {
    _stopped = YES;
    [self closePopup];
    [_hoverTimer invalidate];
    [_closeTimer invalidate];
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

// ------------------------------------------------------------ triggers
- (BOOL)wanted {
    return !_stopped && (MCLineHistoryInline() || _wantPopupLine != NSNotFound || _popupVisible);
}

- (void)storageEdited:(NSNotification *)note {
    NSTextStorage *ts = note.object;
    if (!(ts.editedMask & NSTextStorageEditedCharacters)) return;
    const NSUInteger at = ts.editedRange.location;
    _validBefore = MIN(_validBefore, at);
    if (_looking) _editedSinceLook = MIN(_editedSinceLook, at);
    [self closePopup];
    _wantPopupLine = NSNotFound;
    // The whole text replaced is another tab shown (or the same again):
    // its kept answer, when its text is unchanged, shows at once.
    const BOOL whole = at == 0 && ts.editedRange.length == ts.length;
    if (!whole) _lastEdit = CACurrentMediaTime();   // typing: wait for the pause
    if (whole && [self takeKeptAnswer]) return;
    if (MCLineHistoryInline()) [self scheduleAfter:whole ? 0.05 : kTypingPause];
}

- (void)inlineChanged:(NSNotification *)note {
    (void)note;
    [self redrawLabels];
    if (MCLineHistoryInline()) [self scheduleAfter:0.01];
}

- (void)gitRefreshed:(NSNotification *)note {
    (void)note;
    _gitGeneration++;
    if (MCLineHistoryInline() || _popupVisible) [self scheduleAfter:0.3];
}

- (void)modelChanged:(NSNotification *)note {
    (void)note;
    if (MCLineHistoryInline() || _popupVisible) [self scheduleAfter:0.3];
}

- (void)refreshSoon { [self scheduleAfter:0.05]; }

- (void)scheduleAfter:(NSTimeInterval)delay {
    if (_scheduled || _stopped) return;
    _scheduled = YES;
    __weak MCLineHistory *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        MCLineHistory *s = weakSelf;
        if (!s) return;
        s->_scheduled = NO;
        const CFTimeInterval since = CACurrentMediaTime() - s->_lastEdit;
        if (since < kTypingPause && s->_wantPopupLine == NSNotFound) {
            [s scheduleAfter:kTypingPause - since + 0.01];   // still typing
            return;
        }
        if ([s wanted]) [s look];
    });
}

// The kept answer for the file on show, when its text is the text it was
// for and nothing it depends on has changed since.
- (BOOL)takeKeptAnswer {
    NSString *path = _host.lineHistoryPath;
    MCLineHistoryEntry *e = path ? _cache[path] : nil;
    if (!e || e.gitGeneration != _gitGeneration || e.modelGeneration != _model.generation)
        return NO;
    NSString *now = _host.lineHistoryTextView.textStorage.string;
    if (now.length != e.text.length || ![now isEqualToString:e.text]) return NO;
    _answer = e->answer;
    _answerPath = path;
    _validBefore = now.length + 1;
    [self redrawLabels];
    return YES;
}

- (void)keep:(std::shared_ptr<const Answer>)answer path:(NSString *)path text:(NSString *)text
    modelGeneration:(NSUInteger)mg gitGeneration:(NSUInteger)gg {
    MCLineHistoryEntry *e = [MCLineHistoryEntry new];
    e->answer = answer;
    e.path = path;
    e.text = text;
    e.modelGeneration = mg;
    e.gitGeneration = gg;
    _cache[path] = e;
    [_cacheOrder removeObject:path];
    [_cacheOrder addObject:path];
    while (_cacheOrder.count > 8) {
        [_cache removeObjectForKey:_cacheOrder.firstObject];
        [_cacheOrder removeObjectAtIndex:0];
    }
}

// ------------------------------------------------------------ a look
// The sessions that wrote or edited a file of this name (the queue checks
// the paths properly): transcript, name, the path as the session gave it.
- (NSArray<NSArray<NSString *> *> *)sessionsFor:(NSString *)path model:(MCAgentModel *)model {
    NSMutableArray *out = [NSMutableArray array];
    NSString *name = path.lastPathComponent;
    NSMutableArray<MCAgentWork *> *todo = [model.sessions mutableCopy] ?: [NSMutableArray array];
    while (todo.count) {
        MCAgentWork *w = todo.lastObject;
        [todo removeLastObject];
        [todo addObjectsFromArray:w.subagents ?: @[]];
        NSString *title = w.title.length ? w.title : (w.name.length ? w.name : @"");
        for (MCAgentFileGroup *g in w.groups)
            for (MCAgentFile *f in g.files)
                if ((f.writes + f.edits) > 0 && [f.path.lastPathComponent isEqualToString:name])
                    [out addObject:@[w.transcript ?: @"", title, f.path ?: @""]];
    }
    return out;
}

- (void)look {
    if (_looking) {
        _lookAgain = YES;
        return;
    }
    id<MCLineHistoryHost> host = _host;
    NSString *path = host.lineHistoryPath;
    CodeTextView *tv = host.lineHistoryTextView;
    if (!path || !tv) {
        _answer = nullptr;
        _answerPath = nil;
        [self redrawLabels];
        return;
    }
    const double c0 = ThreadCpuMs();
    MCAgentModel *model = host.lineHistoryAgentModel;
    if (model && model != _model) {
        if (_model)
            [NSNotificationCenter.defaultCenter removeObserver:self
                                                          name:MCAgentModelDidChangeNotification
                                                        object:_model];
        _model = model;
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(modelChanged:)
                                                   name:MCAgentModelDidChangeNotification
                                                 object:model];
    }
    NSString *text = [tv.textStorage.string copy];
    NSArray *candidates = [self sessionsFor:path model:model];
    const NSUInteger number = ++_lookNumber;
    const NSUInteger mg = model.generation, gg = _gitGeneration;
    _looking = YES;
    _editedSinceLook = NSNotFound;
    const double mainBefore = ThreadCpuMs() - c0;
    __weak MCLineHistory *weakSelf = self;
    dispatch_async(_queue, ^{
        MCLineHistory *s0 = weakSelf;
        if (!s0) return;
        auto answer = std::make_shared<Answer>();
        [s0 answer:answer.get() path:path text:text candidates:candidates];
        dispatch_async(dispatch_get_main_queue(), ^{
            MCLineHistory *s = weakSelf;
            if (!s) return;
            const double c1 = ThreadCpuMs();
            s->_looking = NO;
            if (number == s->_lookNumber && !s->_stopped &&
                [path isEqualToString:s->_host.lineHistoryPath]) {
                s->_answer = answer;
                s->_answerPath = path;
                // Lines from the first edit since the look began wait for the next.
                s->_validBefore = s->_editedSinceLook == NSNotFound ? text.length + 1
                                                                     : s->_editedSinceLook;
                [s keep:answer path:path text:text modelGeneration:mg gitGeneration:gg];
                s->_lookedPath = path;
                s->_looks++;
                [s redrawLabels];
                if (s->_wantPopupLine != NSNotFound) [s showPopupForLine:s->_wantPopupLine];
                else if (s->_popupVisible) [s showPopupForLine:s->_popupLine];
                s->_lastLookMainCpuMs = mainBefore + (ThreadCpuMs() - c1);
            }
            if (s->_lookAgain || s->_editedSinceLook != NSNotFound) {
                s->_lookAgain = NO;
                if ([s wanted]) [s scheduleAfter:0.05];
            }
        });
    });
}

// On the queue: blame, the sessions' edits, and the answer per line.
- (void)answer:(Answer *)a path:(NSString *)path text:(NSString *)text
    candidates:(NSArray<NSArray<NSString *> *> *)candidates {
    // Where each line starts, in the view's UTF-16 offsets.
    const NSUInteger n = text.length;
    std::vector<unichar> buf(16384);
    for (NSUInteger at = 0; at < n; at += buf.size()) {
        const NSUInteger len = MIN((NSUInteger)buf.size(), n - at);
        [text getCharacters:buf.data() range:NSMakeRange(at, len)];
        for (NSUInteger i = 0; i < len; i++)
            if (buf[i] == '\n') a->newlines.push_back(at + i);
    }
    const std::string utf8 = Std(text);
    const size_t count = LineHistory::lineCount(utf8);

    // git blame of the buffer as it is.
    NSString *dir = path.stringByDeletingLastPathComponent;
    NSData *input = [NSData dataWithBytes:utf8.data() length:utf8.size()];
    MCGitResult *r = MCGitRunSyncWithInput(
        dir, @[@"blame", @"--porcelain", @"--contents", @"-", @"--", path.lastPathComponent], input);
    bool known = true;
    if (r.ok) {
        a->hasBlame = LineHistory::parsePorcelain(Std([[NSString alloc] initWithData:r.output
                                                                             encoding:NSUTF8StringEncoding] ?: @""),
                                                  &a->blame);
        if (!a->hasBlame) {
            // Bytes that are not UTF-8 in a line: read them one by one.
            std::string raw((const char *)r.output.bytes, r.output.length);
            a->hasBlame = LineHistory::parsePorcelain(raw, &a->blame);
        }
        if (!a->hasBlame) {
            known = false;
            a->trouble = "git blame answered in a way mutible does not read.";
        }
    } else {
        NSString *why = r.errorText ?: @"";
        // A file git does not track, or no repository: nothing is committed.
        const BOOL notTracked = [why containsString:@"no such path"] ||
                                [why containsString:@"not a git repository"] ||
                                [why containsString:@"HEAD"] || [why containsString:@"no such ref"];
        if (r.timedOut || r.status == -1 || !notTracked) {
            known = false;
            a->trouble = why.length ? Std(why) : std::string("git could not say.");
        }
    }

    // The sessions that changed this file, each to the lines its edits wrote.
    AgentTranscript::Fs fs = AgentTranscript::posixFs();
    const std::string file = Fs(path);
    std::vector<LineHistory::SessionLines> lines;
    for (NSArray<NSString *> *c in candidates) {
        const std::string transcript = Fs(c[0]), named = Fs(c[2]);
        if (transcript.empty() || !AgentEdits::samePath(fs, named, file)) continue;
        const AgentTranscript::Stat st = fs.stat(transcript);
        if (!st.exists) continue;
        const std::string key = transcript + "\n" + file;
        Collected &got = _collected[key];
        if (got.size != st.size || got.mtime != st.mtime) {
            got.changes = AgentEdits::collect(fs, transcript, named).changes;
            got.size = st.size;
            got.mtime = st.mtime;
        }
        Answer::Session sess;
        sess.info.title = Std(c[1]);
        sess.info.transcript = transcript;
        sess.file = named;
        sess.changes = got.changes;
        a->sessions.push_back(std::move(sess));
    }
    lines.reserve(a->sessions.size());
    for (const Answer::Session &sess : a->sessions)
        lines.push_back(LineHistory::SessionLines{&sess.changes, LineHistory::linesWritten(sess.changes, utf8)});
    if (known) {
        a->lines = LineHistory::combine(count, a->hasBlame ? &a->blame : nullptr, lines);
    } else {
        a->lines.assign(count, LineHistory::Line());
        // git could not say what is committed; a session's line is still its.
        std::vector<LineHistory::Line> sessionsOnly = LineHistory::combine(count, nullptr, lines);
        for (size_t i = 0; i < count; i++)
            if (sessionsOnly[i].kind == LineHistory::Kind::Session) a->lines[i] = sessionsOnly[i];
    }
}

// ------------------------------------------------------------ lines
// The answer's line for an editor line (by where it starts), or -1 when
// there is no answer for it now.
- (long)answerLineForEditorLine:(NSUInteger)line {
    MCLineGutter *g = _host.lineHistoryGutter;
    if (!_answer || !g || line >= g.lineCount) return -1;
    if (![_answerPath isEqualToString:_host.lineHistoryPath]) return -1;
    return [self answerLineAtCharacter:[g startOfLine:line]];
}

- (long)answerLineAtCharacter:(NSUInteger)ch {
    if (!_answer || ch >= _validBefore) return -1;
    const std::vector<NSUInteger> &nl = _answer->newlines;
    const size_t k = (size_t)(std::lower_bound(nl.begin(), nl.end(), ch) - nl.begin());
    if (k >= _answer->lines.size()) return -1;
    // The line must end before the first edit since too.
    const NSUInteger end = k < nl.size() ? nl[k] + 1 : NSUIntegerMax;
    if (end != NSUIntegerMax && end > _validBefore) return -1;
    if (end == NSUIntegerMax && _validBefore != NSUIntegerMax &&
        _validBefore <= _host.lineHistoryTextView.textStorage.length)
        return -1;
    return (long)k;
}

- (NSString *)labelForAnswerLine:(long)k {
    if (k < 0 || !_answer) return nil;
    const LineHistory::Line &l = _answer->lines[(size_t)k];
    const long long now = (long long)NSDate.date.timeIntervalSince1970;
    switch (l.kind) {
        case LineHistory::Kind::Commit:
            return NS(LineHistory::commitLabel(_answer->blame.commits[(size_t)l.commit], now));
        case LineHistory::Kind::Session: {
            const Answer::Session &s = _answer->sessions[(size_t)l.session];
            return NS(LineHistory::sessionLabel(s.info, s.changes[(size_t)l.change], now));
        }
        case LineHistory::Kind::NotCommitted: return NS(LineHistory::notCommittedLabel());
        case LineHistory::Kind::Unknown: return nil;
    }
    return nil;
}

- (NSString *)detailForAnswerLine:(long)k {
    if (k < 0 || !_answer) return nil;
    const LineHistory::Line &l = _answer->lines[(size_t)k];
    const long long now = (long long)NSDate.date.timeIntervalSince1970;
    switch (l.kind) {
        case LineHistory::Kind::Commit:
            return NS(LineHistory::commitDetail(_answer->blame.commits[(size_t)l.commit], now, MacDate));
        case LineHistory::Kind::Session: {
            const Answer::Session &s = _answer->sessions[(size_t)l.session];
            return NS(LineHistory::sessionDetail(s.info, s.changes[(size_t)l.change], now, MacDate));
        }
        case LineHistory::Kind::NotCommitted: return NS(LineHistory::notCommittedDetail());
        case LineHistory::Kind::Unknown:
            return _answer->trouble.empty()
                       ? nil
                       : [@"Line history is not available: " stringByAppendingString:NS(_answer->trouble)];
    }
    return nil;
}

- (NSString *)labelForLine:(NSUInteger)line {
    return [self labelForAnswerLine:[self answerLineForEditorLine:line]];
}

- (NSString *)detailForLine:(NSUInteger)line {
    return [self detailForAnswerLine:[self answerLineForEditorLine:line]];
}

- (BOOL)openAnswerLine:(long)k {
    if (k < 0 || !_answer) {
        NSBeep();
        return NO;
    }
    const LineHistory::Line &l = _answer->lines[(size_t)k];
    id<MCLineHistoryHost> host = _host;
    if (l.kind == LineHistory::Kind::Commit) {
        const LineHistory::Commit &c = _answer->blame.commits[(size_t)l.commit];
        [self closePopup];
        [host lineHistoryShowCommit:NS(c.hash)
                              title:NS(Git::shortHash(c.hash) + " " + c.summary)];
        return YES;
    }
    if (l.kind == LineHistory::Kind::Session) {
        const Answer::Session &s = _answer->sessions[(size_t)l.session];
        [self closePopup];
        [host lineHistoryShowSession:NS(s.info.transcript) name:NS(s.info.title) file:NS(s.file)];
        return YES;
    }
    NSBeep();
    return NO;
}

- (BOOL)openLine:(NSUInteger)line { return [self openAnswerLine:[self answerLineForEditorLine:line]]; }

// ------------------------------------------------------------ beside the lines
- (NSFont *)labelFont { return MCEditorFont(); }

- (NSColor *)labelColor {
    NSColor *c = _host.lineHistoryGutter.numberColor ?: NSColor.grayColor;
    return [c colorWithAlphaComponent:MIN(c.alphaComponent, 0.85)];
}

- (void)redrawLabels {
    CodeTextView *tv = _host.lineHistoryTextView;
    if (tv) [tv setNeedsDisplayInRect:tv.visibleRect];
}

// Each line fragment that ends a line, in `rect` (view coordinates), with
// the answer's line its line starts on.
- (void)forEachLineEndIn:(NSRect)rect
                   block:(void (^)(NSRect fragment, NSRect used, long line, NSUInteger start))block {
    CodeTextView *tv = _host.lineHistoryTextView;
    NSLayoutManager *lm = tv.layoutManager;
    NSTextContainer *tc = tv.textContainer;
    if (!lm || !tc || !_answer) return;
    NSString *s = tv.textStorage.string;
    const NSUInteger n = s.length;
    if (n == 0) return;
    const NSPoint origin = tv.textContainerOrigin;
    const NSRect inContainer = NSOffsetRect(rect, -origin.x, -origin.y);
    const NSRange glyphs = [lm glyphRangeForBoundingRectWithoutAdditionalLayout:inContainer
                                                                 inTextContainer:tc];
    if (glyphs.length == 0) return;
    [lm enumerateLineFragmentsForGlyphRange:glyphs usingBlock:^(NSRect frag, NSRect used,
                                                                NSTextContainer *c, NSRange gr,
                                                                BOOL *stop) {
        (void)c; (void)stop;
        const NSRange cr = [lm characterRangeForGlyphRange:gr actualGlyphRange:NULL];
        if (cr.length == 0) return;
        const NSUInteger last = NSMaxRange(cr) - 1;
        const unichar ch = [s characterAtIndex:last];
        if (ch != '\n' && NSMaxRange(cr) < n) return;   // a wrapped line goes on
        // The line this fragment's first character is on (a fold's
        // placeholder line holds the folded lines too: its first is shown).
        NSUInteger start = cr.location;
        const long k = [self answerLineAtCharacter:start];
        if (k < 0) return;
        block(NSOffsetRect(frag, origin.x, origin.y), NSOffsetRect(used, origin.x, origin.y), k, start);
    }];
}

- (NSDictionary *)labelAttributes {
    return @{NSFontAttributeName: self.labelFont, NSForegroundColorAttributeName: self.labelColor};
}

- (NSRect)labelRectWithUsed:(NSRect)used fragment:(NSRect)frag label:(NSString *)label {
    NSDictionary *attrs = [self labelAttributes];
    const CGFloat zero = [@"0" sizeWithAttributes:attrs].width;
    const NSSize size = [label sizeWithAttributes:attrs];
    const CGFloat x = NSMaxX(used) + kLabelGapChars * zero;
    return NSMakeRect(x, NSMinY(frag), ceil(size.width), NSHeight(frag));
}

- (BOOL)labelsShown {
    return MCLineHistoryInline() && !_stopped && _answer &&
           [_answerPath isEqualToString:_host.lineHistoryPath];
}

- (void)drawLabelsIn:(NSRect)dirty {
    if (![self labelsShown]) return;
    NSDictionary *attrs = [self labelAttributes];
    NSFont *font = self.labelFont;
    CodeTextView *tv = _host.lineHistoryTextView;
    NSLayoutManager *lm = tv.layoutManager;
    [self forEachLineEndIn:NSInsetRect(dirty, 0, -4) block:^(NSRect frag, NSRect used, long k,
                                                             NSUInteger start) {
        NSString *label = [self labelForAnswerLine:k];
        if (!label.length) return;
        const NSRect r = [self labelRectWithUsed:used fragment:frag label:label];
        // On the text's baseline: the fragment's first glyph's.
        const NSUInteger g = [lm glyphIndexForCharacterAtIndex:start];
        const CGFloat baseline = NSMinY(frag) + [lm locationForGlyphAtIndex:g].y;
        [label drawAtPoint:NSMakePoint(NSMinX(r), baseline - font.ascender) withAttributes:attrs];
    }];
}

- (NSRect)labelRectForLine:(NSUInteger)line {
    if (![self labelsShown]) return NSZeroRect;
    const long want = [self answerLineForEditorLine:line];
    if (want < 0) return NSZeroRect;
    CodeTextView *tv = _host.lineHistoryTextView;
    // Asked before anything was drawn (checks, offscreen): lay out what is
    // on show first, never more.
    const NSPoint origin = tv.textContainerOrigin;
    [tv.layoutManager ensureLayoutForBoundingRect:NSOffsetRect(tv.visibleRect, -origin.x, -origin.y)
                                  inTextContainer:tv.textContainer];
    __block NSRect found = NSZeroRect;
    [self forEachLineEndIn:tv.visibleRect block:^(NSRect frag, NSRect used, long k, NSUInteger start) {
        (void)start;
        if (k != want) return;
        NSString *label = [self labelForAnswerLine:k];
        if (label.length) found = [self labelRectWithUsed:used fragment:frag label:label];
    }];
    return found;
}

// The answer's line whose words are under a point of the view, or -1.
- (long)labelLineAtPoint:(NSPoint)p rect:(NSRect *)rectOut {
    if (![self labelsShown]) return -1;
    __block long hit = -1;
    __block NSRect hitRect = NSZeroRect;
    [self forEachLineEndIn:NSMakeRect(p.x, p.y - 1, 1, 2) block:^(NSRect frag, NSRect used, long k,
                                                                    NSUInteger start) {
        (void)start;
        NSString *label = [self labelForAnswerLine:k];
        if (!label.length) return;
        const NSRect r = [self labelRectWithUsed:used fragment:frag label:label];
        if (NSPointInRect(p, r)) {
            hit = k;
            hitRect = r;
        }
    }];
    if (rectOut) *rectOut = hitRect;
    return hit;
}

- (BOOL)pressed:(NSEvent *)event {
    if (![self labelsShown]) return NO;
    CodeTextView *tv = _host.lineHistoryTextView;
    const NSUInteger mods = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask &
                            (NSEventModifierFlagCommand | NSEventModifierFlagShift |
                             NSEventModifierFlagOption | NSEventModifierFlagControl);
    if (mods || event.clickCount != 1) return NO;
    const NSPoint p = [tv convertPoint:event.locationInWindow fromView:nil];
    const long k = [self labelLineAtPoint:p rect:NULL];
    if (k < 0) return NO;
    [self openAnswerLine:k];
    return YES;
}

- (void)pointerAt:(NSPoint)p inside:(BOOL)inside {
    if (![self labelsShown]) return;
    CodeTextView *tv = _host.lineHistoryTextView;
    NSRect r = NSZeroRect;
    const long k = inside ? [self labelLineAtPoint:p rect:&r] : -1;
    if (k < 0) {
        if (_hoverLine != NSNotFound) [self hoverLine:NSNotFound];
        return;
    }
    // The editor line the words are on.
    MCLineGutter *g = _host.lineHistoryGutter;
    const NSUInteger ch = [tv characterIndexForInsertionAtPoint:NSMakePoint(NSMinX(r) - 1, NSMidY(r))];
    const NSUInteger line = [g lineOfCharacter:MIN(ch, tv.textStorage.length)];
    NSRect inWindow = [tv convertRect:r toView:nil];
    [self hoverLine:line anchor:[tv.window convertRectToScreen:inWindow]];
}

// ------------------------------------------------------------ the box
- (NSRect)anchorForLine:(NSUInteger)line {
    MCLineGutter *g = _host.lineHistoryGutter;
    NSRect row = [g rowRectOfLine:line];
    if (NSIsEmptyRect(row) || !g.window) return NSZeroRect;
    return [g.window convertRectToScreen:[g convertRect:row toView:nil]];
}

- (void)hoverLine:(NSUInteger)line {
    [self hoverLine:line anchor:line == NSNotFound ? NSZeroRect : [self anchorForLine:line]];
}

- (void)hoverLine:(NSUInteger)line anchor:(NSRect)anchor {
    if (line == _hoverLine && line != NSNotFound) return;
    _hoverLine = line;
    _hoverAnchor = anchor;
    [_hoverTimer invalidate];
    _hoverTimer = nil;
    if (line == NSNotFound) {
        // Leave the box a moment, so the pointer can move into it.
        [_closeTimer invalidate];
        __weak MCLineHistory *weakSelf = self;
        _closeTimer = [NSTimer scheduledTimerWithTimeInterval:0.3 repeats:NO block:^(NSTimer *t) {
            (void)t;
            MCLineHistory *s = weakSelf;
            if (s && s->_hoverLine == NSNotFound && !s->_box.pointerInside) [s closePopup];
        }];
        return;
    }
    [_closeTimer invalidate];
    _closeTimer = nil;
    if (_popupVisible) {
        [self showPopupForLine:line];   // moving down the gutter: follow at once
        return;
    }
    __weak MCLineHistory *weakSelf = self;
    _hoverTimer = [NSTimer scheduledTimerWithTimeInterval:kHoverDelay repeats:NO block:^(NSTimer *t) {
        (void)t;
        MCLineHistory *s = weakSelf;
        if (s && s->_hoverLine == line) [s showPopupForLine:line];
    }];
}

- (void)showPopupForLine:(NSUInteger)line {
    if (_stopped || line == NSNotFound) return;
    NSString *detail = [self detailForLine:line];
    if (!detail) {
        // Not known yet: look, and show it when the answer comes.
        _wantPopupLine = line;
        if (!_host.lineHistoryPath) return;
        if (_looking) _lookAgain = YES;
        else [self look];
        return;
    }
    _wantPopupLine = NSNotFound;
    NSRect anchor = NSIsEmptyRect(_hoverAnchor) || line != _hoverLine ? [self anchorForLine:line]
                                                                      : _hoverAnchor;
    [self showBox:detail line:line anchor:anchor];
}

- (NSAttributedString *)boxTextFor:(NSString *)detail {
    const CGFloat size = 12;
    NSMutableAttributedString *out = [NSMutableAttributedString new];
    NSArray<NSString *> *lines = [detail componentsSeparatedByString:@"\n"];
    NSColor *ink = [NSColor colorWithWhite:0.86 alpha:1];
    NSColor *muted = [NSColor colorWithSRGBRed:0x9D / 255.0 green:0x9D / 255.0 blue:0x9D / 255.0 alpha:1];
    NSMutableParagraphStyle *para = [NSMutableParagraphStyle new];
    para.paragraphSpacing = 2;
    for (NSUInteger i = 0; i < lines.count; i++) {
        const BOOL first = i == 0, last = i + 1 == lines.count && lines.count > 1;
        NSFont *font = first ? [NSFont systemFontOfSize:size weight:NSFontWeightSemibold]
                             : [NSFont systemFontOfSize:size];
        NSString *piece = i + 1 < lines.count ? [lines[i] stringByAppendingString:@"\n"] : lines[i];
        [out appendAttributedString:[[NSAttributedString alloc] initWithString:piece attributes:@{
            NSFontAttributeName: font,
            NSForegroundColorAttributeName: last ? muted : ink,
            NSParagraphStyleAttributeName: para}]];
    }
    return out;
}

- (void)showBox:(NSString *)detail line:(NSUInteger)line anchor:(NSRect)anchor {
    NSWindow *parent = _host.lineHistoryTextView.window;
    if (!parent) return;
    if (!_panel) {
        _panel = [[MCLineHistoryPanel alloc]
            initWithContentRect:NSMakeRect(0, 0, 300, 60)
                      styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                        backing:NSBackingStoreBuffered defer:YES];
        _panel.hasShadow = YES;
        _panel.releasedWhenClosed = NO;
        _panel.backgroundColor = NSColor.clearColor;
        _panel.opaque = NO;
        _panel.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        _panel.becomesKeyOnlyIfNeeded = YES;
        _box = [[MCLineHistoryBoxView alloc] initWithFrame:_panel.contentView.bounds];
        _box.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        _panel.contentView = _box;
        __weak MCLineHistory *weakSelf = self;
        _box.onClick = ^{
            MCLineHistory *s = weakSelf;
            if (s) [s openLine:s->_popupLine];
        };
        _box.onExit = ^{
            MCLineHistory *s = weakSelf;
            if (s && s->_hoverLine == NSNotFound) [s closePopup];
        };
    }
    NSAttributedString *text = [self boxTextFor:detail];
    const NSRect fit = [text boundingRectWithSize:NSMakeSize(460, 1000)
                                          options:NSStringDrawingUsesLineFragmentOrigin];
    const NSSize size = NSMakeSize(ceil(fit.size.width) + 20, ceil(fit.size.height) + 14);
    // Right of the gutter, just below the line; above it when there is no room.
    NSRect gutterOnScreen = anchor;
    MCLineGutter *g = _host.lineHistoryGutter;
    if (g.window) {
        const NSRect gr = [g.window convertRectToScreen:[g convertRect:g.bounds toView:nil]];
        if (NSMinX(anchor) < NSMaxX(gr)) gutterOnScreen.origin.x = NSMaxX(gr) + 4;
    }
    NSRect frame = NSMakeRect(NSMinX(gutterOnScreen), NSMinY(anchor) - size.height - 2, size.width,
                              size.height);
    NSScreen *screen = parent.screen ?: NSScreen.mainScreen;
    const NSRect vis = screen.visibleFrame;
    if (NSMinY(frame) < NSMinY(vis)) frame.origin.y = NSMaxY(anchor) + 2;
    if (NSMaxX(frame) > NSMaxX(vis)) frame.origin.x = NSMaxX(vis) - size.width;
    if (NSMinX(frame) < NSMinX(vis)) frame.origin.x = NSMinX(vis);
    _box.text = text;
    [_panel setFrame:frame display:YES];
    _box.needsDisplay = YES;
    if (!_popupVisible || _parent != parent) {
        [_parent removeChildWindow:_panel];
        [parent addChildWindow:_panel ordered:NSWindowAbove];
        _parent = parent;
    }
    [_panel orderFront:nil];
    _popupVisible = YES;
    _popupLine = line;
    _popupText = [detail copy];
}

- (void)closePopup {
    _wantPopupLine = NSNotFound;
    if (!_popupVisible) return;
    _popupVisible = NO;
    _popupText = nil;
    _popupLine = NSNotFound;
    [_parent removeChildWindow:_panel];
    [_panel orderOut:nil];
}

@end
