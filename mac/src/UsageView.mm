// UsageView.mm — see UsageView.h.
#import "UsageView.h"
#import "AgentModel.h"
#import "StatusLineOffer.h"

#include "ClaudeStatusLine.h"
#include "ClaudeUsage.h"
#include <ctime>
#include <string>
#include <vector>

NSString *const MCUsageModelDidChangeNotification = @"MCUsageModelDidChangeNotification";

static NSString *UNS(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size()
                                  encoding:NSUTF8StringEncoding] ?: @"";
}
static std::string UU8(NSString *s) { return s.UTF8String ? std::string(s.UTF8String) : ""; }

// ------------------------------------------------------------------ model
@interface MCUsageModel ()
@property(nonatomic, readwrite, copy) NSArray<NSString *> *lines;
@property(nonatomic, readwrite) BOOL loaded;
@end

@implementation MCUsageModel {
    dispatch_queue_t _queue;
    // Only on _queue.
    ClaudeUsage::Scanner _scanner;
    ClaudeUsage::Tally _tally;
    std::string _projects;
    // Only on the main thread.
    BOOL _busy, _again;
    NSDate *_last;
    NSMutableArray *_waiting;
}

+ (instancetype)shared {
    static MCUsageModel *model;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ model = [MCUsageModel new]; });
    return model;
}

- (instancetype)init {
    if ((self = [super init])) {
        _queue = dispatch_queue_create("mutible.usage", DISPATCH_QUEUE_SERIAL);
        _lines = @[];
        _waiting = [NSMutableArray array];
    }
    return self;
}

- (void)refresh {
    if (_last && -[_last timeIntervalSinceNow] < 5 && !_busy) return;
    [self look:nil];
}

- (void)refreshThen:(void (^)(void))done {
    [self look:done ?: ^{}];
}

- (void)look:(void (^)(void))done {
    if (done) [_waiting addObject:[done copy]];
    if (_busy) {
        _again = YES;
        return;
    }
    _busy = YES;
    _again = NO;
    NSArray *callbacks = [_waiting copy];
    [_waiting removeAllObjects];
    // Where to look, read on the main thread (the environment can change).
    const std::string projects = UU8([MCAgentModel defaultProjectsDir]);
    const std::string log = ClaudeStatusLine::logPath(UU8(MCUsageDir()));
    dispatch_async(_queue, ^{
        if (projects != self->_projects) {
            self->_scanner = ClaudeUsage::Scanner();
            self->_tally.clear();
            self->_projects = projects;
        }
        const double now = (double)time(nullptr);
        self->_scanner.scan(AgentTranscript::posixFs(), projects, now - ClaudeUsage::kHorizon,
                            self->_tally);
        const std::vector<ClaudeUsage::Sample> samples =
            ClaudeUsage::parseLog(ClaudeStatusLine::readLog(log));
        const std::vector<ClaudeUsage::Entry> entries = self->_tally.sorted();
        const ClaudeUsage::Forecast f = ClaudeUsage::forecast(samples, entries, now);
        NSMutableArray<NSString *> *lines = [NSMutableArray array];
        for (const std::string &l : ClaudeUsage::describe(f, now)) [lines addObject:UNS(l)];
        dispatch_async(dispatch_get_main_queue(), ^{
            self.lines = lines;
            self.loaded = YES;
            self->_last = [NSDate date];
            self->_busy = NO;
            [NSNotificationCenter.defaultCenter postNotificationName:MCUsageModelDidChangeNotification
                                                              object:self];
            for (void (^cb)(void) in callbacks) cb();
            if (self->_again || self->_waiting.count) [self look:nil];
        });
    });
}
@end

// ------------------------------------------------------------------ view
@implementation MCUsageView {
    NSTextField *_heading, *_text;
    NSButton *_action;
    NSColor *_color;
    BOOL _installed;
    id _observer;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _color = NSColor.secondaryLabelColor;
        _heading = [NSTextField labelWithString:@""];
        _heading.attributedStringValue = [[NSAttributedString alloc]
            initWithString:@"PLAN USAGE"
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:10 weight:NSFontWeightSemibold],
                             NSForegroundColorAttributeName: [NSColor colorWithWhite:0.62 alpha:1],
                             NSKernAttributeName: @0.6}];
        [self addSubview:_heading];
        _text = [NSTextField wrappingLabelWithString:@""];
        _text.font = [NSFont systemFontOfSize:11];
        _text.selectable = YES;
        [self addSubview:_text];
        _action = [NSButton buttonWithTitle:@"" target:self action:@selector(act:)];
        _action.bordered = NO;
        _action.alignment = NSTextAlignmentLeft;
        [self addSubview:_action];
        _installed = MCClaudeStatusLineInstalled();
        [self setActionTitle];
        __weak MCUsageView *weakSelf = self;
        _observer = [NSNotificationCenter.defaultCenter
            addObserverForName:MCUsageModelDidChangeNotification object:nil queue:nil
                    usingBlock:^(NSNotification *n) { (void)n; [weakSelf showLines]; }];
        [self showLines];
    }
    return self;
}

- (void)dealloc {
    if (_observer) [NSNotificationCenter.defaultCenter removeObserver:_observer];
}

- (BOOL)isFlipped { return YES; }

- (NSButton *)actionButton { return _action; }
- (NSString *)forecastText { return _text.stringValue ?: @""; }
- (NSString *)fitText { return _text.toolTip ?: @""; }

- (void)applyTextColor:(NSColor *)color {
    _color = color ?: NSColor.secondaryLabelColor;
    _text.textColor = [_color colorWithAlphaComponent:0.8];
}

- (void)setActionTitle {
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineBreakMode = NSLineBreakByTruncatingTail;
    NSString *title = _installed ? @"Stop logging plan usage…"
                                 : @"Log plan usage from Claude Code's status line…";
    _action.attributedTitle = [[NSAttributedString alloc]
        initWithString:title
            attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11.5],
                         NSForegroundColorAttributeName: [NSColor colorWithSRGBRed:0x4E / 255.0
                                                                             green:0xA1 / 255.0
                                                                              blue:0xF7 / 255.0
                                                                             alpha:1],
                         NSParagraphStyleAttributeName: ps}];
    _action.toolTip = _installed
        ? @"Puts Claude Code's status line back as it was before mutible changed it, after asking."
        : @"Changes Claude Code's status line, after showing you how, so it notes the plan usage "
           "Claude Code reports; your own status line keeps running.";
}

- (void)showLines {
    NSArray<NSString *> *lines = [MCUsageModel shared].lines;
    NSMutableArray<NSString *> *shown = [NSMutableArray array];
    NSString *fit = nil;
    for (NSString *l in lines) {
        if ([l hasPrefix:@"Fit: "]) fit = l;
        else [shown addObject:l];
    }
    NSString *text = [MCUsageModel shared].loaded ? [shown componentsJoinedByString:@"\n"]
                                                  : @"Reading Claude Code's transcripts…";
    _text.toolTip = fit;
    if ([text isEqualToString:_text.stringValue]) return;
    _text.stringValue = text;
    _text.textColor = [_color colorWithAlphaComponent:0.8];
    if (self.onHeightChange) self.onHeightChange();
}

- (void)refresh {
    const BOOL installed = MCClaudeStatusLineInstalled();
    if (installed != _installed) {
        _installed = installed;
        [self setActionTitle];
    }
    [[MCUsageModel shared] refresh];
}

- (void)act:(id)sender {
    (void)sender;
    __weak MCUsageView *weakSelf = self;
    void (^after)(BOOL) = ^(BOOL installed) {
        MCUsageView *s = weakSelf;
        if (!s) return;
        s->_installed = installed;
        [s setActionTitle];
        [[MCUsageModel shared] refreshThen:nil];
    };
    if (_installed) MCUninstallClaudeStatusLine(self.window, after);
    else MCInstallClaudeStatusLine(self.window, after);
}

- (CGFloat)heightForWidth:(CGFloat)width {
    const CGFloat w = MAX(40, width);
    _text.preferredMaxLayoutWidth = w;
    const CGFloat textH = ceil([_text.cell cellSizeForBounds:NSMakeRect(0, 0, w, 10000)].height);
    return 16 + 2 + textH + 4 + 18 + 4;
}

- (void)resizeSubviewsWithOldSize:(NSSize)old {
    (void)old;
    const CGFloat W = self.bounds.size.width;
    const CGFloat textH = [self heightForWidth:W] - (16 + 2 + 4 + 18 + 4);
    _heading.frame = NSMakeRect(0, 0, W, 16);
    _text.frame = NSMakeRect(0, 18, W, textH);
    _action.frame = NSMakeRect(-2, 18 + textH + 4, W + 2, 18);
}
@end
