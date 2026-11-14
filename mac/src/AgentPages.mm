// AgentPages.mm — see AgentPages.h.
#import "AgentPages.h"
#import "EditorFont.h"
#import "AgentModel.h"
#import "AppSettings.h"
#import "Appearance.h"
#import "CompareView.h"

#include "AgentEvidence.h"
#include "AgentTranscript.h"
#include "AgentView.h"
#include "Settings.h"

#include <map>
#include <memory>

static NSColor *PHex(unsigned rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1];
}
static NSString *PNS(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size()
                                  encoding:NSUTF8StringEncoding] ?: @"";
}
static std::string PU8(NSString *s) { return s.UTF8String ? std::string(s.UTF8String) : ""; }

static NSColor *Muted() { return PHex(0x9CA3AF); }
static NSColor *Red() { return PHex(0xF14C4C); }
static NSColor *Green() { return PHex(0x73C991); }
static NSColor *Accent() { return PHex(0x4EA1F7); }

static const size_t kExpandBytes = 1 << 20;   // a whole output, shown up to 1 MB

// ===================================================================== evidence
// One command as the page shows it (main thread).
@interface MCEvCommand : NSObject
@property(nonatomic, copy) NSString *desc, *command, *cwd, *testName, *summary, *summaryLine, *result;
@property(nonatomic) BOOL answered, isError, commandCut, isTest, summaryFailed;
@property(nonatomic) NSTimeInterval time;
@property(nonatomic) BOOL loaded;                         // head and tail read
@property(nonatomic, copy) NSArray<NSString *> *head, *tail;
@property(nonatomic) long long omitted;
@property(nonatomic) BOOL whole;
@property(nonatomic) unsigned long long bytes;
@property(nonatomic, copy) NSString *full;                // the whole output, once expanded
@property(nonatomic) BOOL fullCut;
@end
@implementation MCEvCommand
@end

// What the queue remembers of an output it read.
struct LoadedOutput {
    AgentTranscript::Output out;
    std::string full;
    bool haveFull = false, fullCut = false;
};

static NSArray<NSString *> *Lines(const std::vector<std::string> &v) {
    NSMutableArray *a = [NSMutableArray arrayWithCapacity:v.size()];
    for (const std::string &l : v) [a addObject:PNS(AgentView::clipLine(AgentEvidence::stripAnsi(l)))];
    return a;
}

@interface MCEvidenceView : NSView
@end
@implementation MCEvidenceView
- (BOOL)isFlipped { return YES; }
@end

@interface MCEvidencePage () <NSTextViewDelegate>
@end

@implementation MCEvidencePage {
    NSString *_name;
    __weak MCAgentModel *_model;
    dispatch_queue_t _queue;
    std::unique_ptr<AgentEvidence::Reader> _reader;          // on _queue
    std::map<std::string, LoadedOutput> _outputs;           // on _queue, by output key
    std::map<size_t, bool> _wantFull;                        // on _queue
    MCEvidenceView *_view;
    NSTextField *_titleLabel, *_infoLabel, *_claimHeading, *_testHeading;
    NSScrollView *_claimScroll, *_commandsScroll;
    NSTextView *_claimView, *_commandsView;
    NSTextField *_testLabel;
    NSView *_claimBox, *_testBox;
    NSArray<MCEvCommand *> *_commands;
    NSString *_cwd, *_final, *_info;
    NSInteger _lastTest;
    BOOL _updating, _again, _closed;
    id _observer;
}

- (instancetype)initWithTranscript:(NSString *)transcript name:(NSString *)name
                             model:(MCAgentModel *)model {
    if ((self = [super init])) {
        _transcript = [transcript copy];
        _name = [name copy] ?: transcript.lastPathComponent;
        _model = model;
        _queue = dispatch_queue_create("minicode.evidence", DISPATCH_QUEUE_SERIAL);
        _reader.reset(new AgentEvidence::Reader(PU8(transcript)));
        _commands = @[];
        _lastTest = -1;
        [self build];
        [self pageApplySettings];
        __weak MCEvidencePage *weakSelf = self;
        _observer = [NSNotificationCenter.defaultCenter
            addObserverForName:MCAgentModelDidChangeNotification object:model queue:nil
                    usingBlock:^(NSNotification *n) { (void)n; [weakSelf update]; }];
        [self update];
    }
    return self;
}

- (void)dealloc {
    if (_observer) [NSNotificationCenter.defaultCenter removeObserver:_observer];
}

// ---- MCEditorPage
- (NSString *)pageTitle { return [NSString stringWithFormat:@"Evidence: %@", _name]; }
- (NSString *)pageKind { return @"evidence"; }
- (NSString *)pageKey { return [@"evidence:" stringByAppendingString:_transcript]; }
- (NSView *)pageView { return _view; }
- (NSView *)pageFocusView { return _commandsView; }
- (void)pageClosed {
    _closed = YES;
    if (_observer) [NSNotificationCenter.defaultCenter removeObserver:_observer];
    _observer = nil;
}

- (NSTextField *)label:(CGFloat)size weight:(NSFontWeight)w {
    NSTextField *l = [NSTextField labelWithString:@""];
    l.font = [NSFont systemFontOfSize:size weight:w];
    l.lineBreakMode = NSLineBreakByTruncatingTail;
    l.selectable = YES;
    return l;
}

- (NSTextField *)heading:(NSString *)text {
    NSTextField *l = [NSTextField labelWithString:@""];
    l.attributedStringValue = [[NSAttributedString alloc]
        initWithString:text
            attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:10 weight:NSFontWeightSemibold],
                         NSForegroundColorAttributeName: Muted(),
                         NSKernAttributeName: @0.6}];
    return l;
}

- (NSTextView *)textViewIn:(NSScrollView *)scroll {
    NSTextView *tv = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 100)];
    tv.editable = NO;
    tv.selectable = YES;
    tv.richText = YES;
    tv.drawsBackground = NO;
    tv.usesFontPanel = NO;
    tv.usesFindBar = YES;
    tv.incrementalSearchingEnabled = YES;
    tv.textContainerInset = NSMakeSize(6, 6);
    tv.minSize = NSMakeSize(0, 0);
    tv.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    tv.verticallyResizable = YES;
    tv.horizontallyResizable = NO;
    tv.autoresizingMask = NSViewWidthSizable;
    tv.textContainer.widthTracksTextView = YES;
    tv.layoutManager.allowsNonContiguousLayout = YES;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = YES;
    scroll.drawsBackground = NO;
    scroll.documentView = tv;
    return tv;
}

- (NSView *)box {
    NSView *b = [[MCEvidenceView alloc] initWithFrame:NSZeroRect];   // flipped: laid out from the top
    b.wantsLayer = YES;
    b.layer.cornerRadius = 5;
    b.layer.borderWidth = 1;
    b.layer.borderColor = PHex(0x3C3C3C).CGColor;
    return b;
}

- (void)build {
    _view = [[MCEvidenceView alloc] initWithFrame:NSMakeRect(0, 0, 600, 400)];
    _view.wantsLayer = YES;
    _view.postsFrameChangedNotifications = YES;
    __weak MCEvidencePage *weakSelf = self;
    [NSNotificationCenter.defaultCenter addObserverForName:NSViewFrameDidChangeNotification
                                                    object:_view queue:nil
                                                usingBlock:^(NSNotification *n) {
        (void)n;
        [weakSelf layout];
    }];
    _titleLabel = [self label:14 weight:NSFontWeightSemibold];
    _infoLabel = [self label:11 weight:NSFontWeightRegular];
    _infoLabel.textColor = Muted();
    [_view addSubview:_titleLabel];
    [_view addSubview:_infoLabel];

    _claimBox = [self box];
    _claimHeading = [self heading:@"FINAL MESSAGE"];
    _claimScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _claimView = [self textViewIn:_claimScroll];
    [_claimBox addSubview:_claimHeading];
    [_claimBox addSubview:_claimScroll];
    [_view addSubview:_claimBox];

    _testBox = [self box];
    _testHeading = [self heading:@"LAST TEST COMMAND"];
    _testLabel = [NSTextField wrappingLabelWithString:@""];
    _testLabel.selectable = YES;
    _testLabel.maximumNumberOfLines = 0;
    [_testBox addSubview:_testHeading];
    [_testBox addSubview:_testLabel];
    [_view addSubview:_testBox];

    _commandsScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _commandsView = [self textViewIn:_commandsScroll];
    _commandsView.delegate = self;
    _commandsView.linkTextAttributes = @{NSCursorAttributeName: NSCursor.pointingHandCursor,
                                         NSForegroundColorAttributeName: Accent()};
    [_view addSubview:_commandsScroll];
    _view.menu = MCAdjustSettingsMenu(@"editor", _view);
    [self layout];
}

- (void)layout {
    const CGFloat W = _view.bounds.size.width, H = _view.bounds.size.height, pad = 12;
    _titleLabel.frame = NSMakeRect(pad, 10, W - 2 * pad, 20);
    _infoLabel.frame = NSMakeRect(pad, 31, W - 2 * pad, 16);
    const CGFloat top = 54, boxH = MIN(190, MAX(90, H * 0.3));
    const CGFloat testW = MAX(180, floor((W - 3 * pad) * 0.38));
    const CGFloat claimW = MAX(100, W - 3 * pad - testW);
    _claimBox.frame = NSMakeRect(pad, top, claimW, boxH);
    _claimHeading.frame = NSMakeRect(8, 6, claimW - 16, 14);
    _claimScroll.frame = NSMakeRect(2, 22, claimW - 4, boxH - 24);
    _testBox.frame = NSMakeRect(pad * 2 + claimW, top, testW, boxH);
    _testHeading.frame = NSMakeRect(8, 6, testW - 16, 14);
    _testLabel.preferredMaxLayoutWidth = testW - 16;
    _testLabel.frame = NSMakeRect(8, 24, testW - 16, boxH - 30);
    const CGFloat listTop = top + boxH + 10;
    _commandsScroll.frame = NSMakeRect(pad - 6, listTop, W - 2 * pad + 6, MAX(0, H - listTop - 4));
}

- (void)pageApplySettings {
    AppSettings *cfg = [AppSettings shared];
    _view.layer.backgroundColor = [cfg background:Surface::Editor].CGColor;
    NSColor *text = [cfg text:Surface::Editor];
    _titleLabel.textColor = text;
    _claimView.textColor = text;
    [self render];
}

- (NSColor *)textColor { return [[AppSettings shared] text:Surface::Editor]; }

// ------------------------------------------------------------- reading
- (void)update {
    if (_closed) return;
    if (_updating) { _again = YES; return; }
    _updating = YES;
    __weak MCEvidencePage *weakSelf = self;
    dispatch_async(_queue, ^{
        MCEvidencePage *s = weakSelf;
        if (!s) return;
        [s readOnQueue];
    });
}

static std::string OutputKey(const AgentEvidence::Command &c) {
    return c.toolUseId + "@" + std::to_string(c.output.offset) + "+" +
           std::to_string(c.output.length);
}

// On _queue: the commands, then each output's ends, published in two steps
// so the list shows at once and the outputs fill in.
- (void)readOnQueue {
    const AgentTranscript::Fs fs = AgentTranscript::posixFs();
    _reader->update(fs);
    const AgentEvidence::Evidence &ev = _reader->evidence();
    const std::string transcript = PU8(_transcript);

    auto make = [&](BOOL withOutputs) {
        NSMutableArray<MCEvCommand *> *cmds = [NSMutableArray arrayWithCapacity:ev.commands.size()];
        for (size_t i = 0; i < ev.commands.size(); i++) {
            const AgentEvidence::Command &c = ev.commands[i];
            MCEvCommand *m = [MCEvCommand new];
            m.desc = PNS(c.description);
            m.command = PNS(c.command);
            m.commandCut = c.commandCut;
            m.cwd = PNS(c.cwd.empty() ? ev.cwd : c.cwd);
            m.answered = c.answered;
            m.isError = c.isError;
            m.result = @(AgentView::resultWord(c));
            m.time = c.time;
            m.isTest = c.test != AgentEvidence::TestTool::None;
            m.testName = PNS(AgentEvidence::testToolName(c.test));
            m.summary = PNS(AgentView::summaryText(c.summary));
            m.summaryLine = PNS(c.summary.line);
            m.summaryFailed = c.summary.found && (c.summary.failed > 0 || c.summary.errors > 0);
            if (withOutputs && c.answered) {
                const std::string key = OutputKey(c);
                auto it = _outputs.find(key);
                const bool live = c.inBackground;   // still being written: read it again
                if (it == _outputs.end() || live) {
                    LoadedOutput lo;
                    if (it != _outputs.end()) lo = it->second;
                    if (!c.persistedPath.empty()) {
                        lo.out = AgentTranscript::readTextFile(fs, c.persistedPath, 20, 20);
                        lo.out.persistedPath = c.persistedPath;
                    } else if (!c.output.empty()) {
                        lo.out = AgentTranscript::readOutput(fs, transcript, c.output, 20, 20);
                        if (!lo.out.persistedPath.empty()) {
                            std::string p = lo.out.persistedPath;
                            lo.out = AgentTranscript::readTextFile(fs, p, 20, 20);
                            lo.out.persistedPath = p;
                        }
                    }
                    it = _outputs.insert_or_assign(key, lo).first;
                }
                LoadedOutput &lo = it->second;
                if (_wantFull[i] && !lo.haveFull) {
                    bool cut = false;
                    if (!lo.out.persistedPath.empty()) {
                        std::string data;
                        fs.read(lo.out.persistedPath, 0, kExpandBytes + 1, &data);
                        cut = data.size() > kExpandBytes;
                        if (cut) data.resize(AgentView::clipLine(data, kExpandBytes).size() - 3);
                        lo.full = data;
                    } else {
                        lo.full = AgentTranscript::readOutputText(fs, transcript, c.output,
                                                                  kExpandBytes, &cut);
                    }
                    lo.full = AgentEvidence::stripAnsi(lo.full);
                    lo.haveFull = true;
                    lo.fullCut = cut;
                }
                m.loaded = YES;
                m.head = Lines(lo.out.head);
                m.tail = Lines(lo.out.tail);
                m.omitted = lo.out.omitted;
                m.whole = lo.out.whole;
                m.bytes = lo.out.bytes;
                if (_wantFull[i] && lo.haveFull) {
                    m.full = PNS(lo.full);
                    m.fullCut = lo.fullCut;
                }
            } else if (!c.answered) {
                m.loaded = YES;   // nothing to read yet
            }
            [cmds addObject:m];
        }
        return cmds;
    };

    NSString *final = PNS(ev.finalMessage);
    if (ev.finalCut) final = [final stringByAppendingString:@"…"];
    NSMutableArray *versions = [NSMutableArray array];
    for (const std::string &v : ev.versions) [versions addObject:PNS(v)];
    NSString *cwd = PNS(ev.cwd);
    NSInteger lastTest = ev.lastTest;
    int bad = ev.badLines;

    NSArray *first = make(NO);
    __weak MCEvidencePage *weakSelf = self;
    void (^publish)(NSArray *, BOOL) = ^(NSArray *cmds, BOOL done) {
        dispatch_async(dispatch_get_main_queue(), ^{
            MCEvidencePage *s = weakSelf;
            if (!s) return;
            [s showCommands:cmds final:final cwd:cwd versions:versions lastTest:lastTest
                   badLines:bad outputs:done];
        });
    };
    publish(first, NO);
    publish(make(YES), YES);
}

- (void)showCommands:(NSArray<MCEvCommand *> *)cmds final:(NSString *)final cwd:(NSString *)cwd
            versions:(NSArray<NSString *> *)versions lastTest:(NSInteger)lastTest
            badLines:(int)bad outputs:(BOOL)done {
    _commands = cmds;
    _final = final;
    _cwd = cwd;
    _lastTest = lastTest;
    NSMutableArray<NSString *> *info = [NSMutableArray array];
    if (cwd.length) [info addObject:[cwd stringByAbbreviatingWithTildeInPath]];
    NSUInteger tests = 0;
    for (MCEvCommand *c in cmds) if (c.isTest) tests++;
    [info addObject:[NSString stringWithFormat:@"%lu %@, %lu %@", (unsigned long)cmds.count,
                     cmds.count == 1 ? @"command" : @"commands", (unsigned long)tests,
                     tests == 1 ? @"test run" : @"test runs"]];
    if (versions.count)
        [info addObject:[@"Claude Code " stringByAppendingString:[versions componentsJoinedByString:@", "]]];
    if (bad > 0)
        [info addObject:[NSString stringWithFormat:@"%d unreadable %@ skipped", bad,
                         bad == 1 ? @"line" : @"lines"]];
    _info = [info componentsJoinedByString:@"  ·  "];
    if (done) {
        _loaded = YES;
        _outputsLoaded = YES;
    } else {
        _loaded = YES;
    }
    [self render];
    if (done) {
        _updating = NO;
        if (_again) {
            _again = NO;
            [self update];
        }
    }
    if (self.onUpdate) self.onUpdate();
}

// ------------------------------------------------------------- drawing
- (NSString *)finalMessageText { return _final ?: @""; }

- (NSString *)lastTestText { return _testLabel.stringValue ?: @""; }

- (NSUInteger)commandCount { return _commands.count; }

- (NSTextView *)commandsView { return _commandsView; }

- (void)render {
    NSColor *text = [self textColor];
    _titleLabel.stringValue = _name ?: @"";
    _infoLabel.stringValue = _info ?: @"";
    NSString *final = _final.length ? _final : @"(The session has no reply with text yet.)";
    [_claimView.textStorage setAttributedString:[[NSAttributedString alloc]
        initWithString:final
            attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:12.5],
                         NSForegroundColorAttributeName: _final.length ? text : Muted()}]];
    [self renderLastTest:text];
    [self renderCommands:text];
}

- (void)renderLastTest:(NSColor *)text {
    NSMutableAttributedString *s = [NSMutableAttributedString new];
    NSFont *mono = MCMonoFont(11.5, NSFontWeightRegular);
    NSFont *body = [NSFont systemFontOfSize:12.5];
    auto add = [&](NSString *str, NSFont *f, NSColor *c) {
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:str attributes:@{NSFontAttributeName: f, NSForegroundColorAttributeName: c}]];
    };
    if (_lastTest < 0 || _lastTest >= (NSInteger)_commands.count) {
        add(@"No test command was run.", body, Muted());
    } else {
        MCEvCommand *c = _commands[(NSUInteger)_lastTest];
        if (c.summary.length) {
            add(c.summary, [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold],
                c.summaryFailed ? Red() : Green());
            add(@"\n", body, text);
        }
        NSString *result = !c.answered ? @"still running"
                         : c.isError   ? @"the command reported an error"
                                       : @"the command reported no error";
        add(result, body, !c.answered ? Muted() : c.isError ? Red() : text);
        if (!c.summary.length && c.answered) add(@"\nno test summary found in its output", body, Muted());
        add(@"\n", body, text);
        if (c.desc.length) {
            add(c.desc, body, text);
            add(@"\n", body, text);
        }
        NSString *cmd = c.command.length > 300
            ? [[c.command substringToIndex:300] stringByAppendingString:@"…"] : c.command;
        add([@"$ " stringByAppendingString:cmd ?: @""], mono, Muted());
        if (c.summaryLine.length && ![c.summaryLine isEqualToString:c.summary]) {
            add(@"\n", body, text);
            add(c.summaryLine, mono, Muted());
        }
    }
    _testLabel.attributedStringValue = s;
}

- (NSAttributedString *)blockFor:(MCEvCommand *)c index:(NSUInteger)i color:(NSColor *)text {
    NSMutableAttributedString *s = [NSMutableAttributedString new];
    NSFont *body = [NSFont systemFontOfSize:12.5];
    NSFont *bold = [NSFont systemFontOfSize:12.5 weight:NSFontWeightSemibold];
    NSFont *mono = MCMonoFont(11.5, NSFontWeightRegular);
    NSFont *small = [NSFont systemFontOfSize:11];
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.headIndent = 28;
    ps.firstLineHeadIndent = 0;
    ps.paragraphSpacingBefore = 0;
    ps.tabStops = @[[[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft location:28 options:@{}]];
    NSMutableParagraphStyle *indented = [ps mutableCopy];
    indented.firstLineHeadIndent = 28;
    NSMutableParagraphStyle *first = [ps mutableCopy];
    first.paragraphSpacingBefore = 14;
    auto add = [&](NSString *str, NSDictionary *attrs) {
        [s appendAttributedString:[[NSAttributedString alloc] initWithString:str ?: @"" attributes:attrs]];
    };
    auto link = [&](NSString *title, NSString *action) {
        add(title, @{NSFontAttributeName: small, NSParagraphStyleAttributeName: first,
                     NSLinkAttributeName: [NSURL URLWithString:[NSString stringWithFormat:
                         @"minicode-evidence://%@/%lu", action, (unsigned long)i]]});
    };
    // The heading line: number, description, marks, Open in Terminal.
    add([NSString stringWithFormat:@"%lu\t", (unsigned long)i + 1],
        @{NSFontAttributeName: small, NSForegroundColorAttributeName: Muted(),
          NSParagraphStyleAttributeName: first});
    add(c.desc.length ? c.desc : @"(no description)",
        @{NSFontAttributeName: c.desc.length ? bold : body,
          NSForegroundColorAttributeName: c.desc.length ? text : Muted(),
          NSParagraphStyleAttributeName: first});
    if (c.isTest) {
        add(@"   ", @{NSFontAttributeName: small, NSParagraphStyleAttributeName: first});
        add([NSString stringWithFormat:@" TEST %@ ", c.testName ?: @""],
            @{NSFontAttributeName: [NSFont systemFontOfSize:10 weight:NSFontWeightBold],
              NSForegroundColorAttributeName: PHex(0x1E1E1E),
              NSBackgroundColorAttributeName: PHex(0xE2C08D),
              NSParagraphStyleAttributeName: first});
    }
    NSString *word = c.result ?: @"";
    add(@"   ", @{NSFontAttributeName: small, NSParagraphStyleAttributeName: first});
    add(word, @{NSFontAttributeName: [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold],
                NSForegroundColorAttributeName: !c.answered ? Muted() : c.isError ? Red() : Green(),
                NSParagraphStyleAttributeName: first});
    add(@"   ", @{NSFontAttributeName: small, NSParagraphStyleAttributeName: first});
    link(@"Open in Terminal", @"terminal");
    add(@"\n", @{NSFontAttributeName: body, NSParagraphStyleAttributeName: first});

    // The command.
    NSString *cmd = c.command ?: @"";
    if (c.commandCut) cmd = [cmd stringByAppendingString:@" …(cut)"];
    add([@"$ " stringByAppendingString:cmd],
        @{NSFontAttributeName: mono, NSForegroundColorAttributeName: Muted(),
          NSParagraphStyleAttributeName: indented});
    add(@"\n", @{NSFontAttributeName: mono, NSParagraphStyleAttributeName: indented});
    if (c.isTest && c.answered) {
        NSString *sum = c.summary.length ? c.summary : @"no test summary found in its output";
        add(sum, @{NSFontAttributeName: bold,
                   NSForegroundColorAttributeName: !c.summary.length ? Muted()
                                                 : c.summaryFailed ? Red() : Green(),
                   NSParagraphStyleAttributeName: indented});
        add(@"\n", @{NSFontAttributeName: body, NSParagraphStyleAttributeName: indented});
    }

    // The output: its ends, or all of it once expanded.
    NSDictionary *outAttrs = @{NSFontAttributeName: mono, NSForegroundColorAttributeName: text,
                               NSParagraphStyleAttributeName: indented};
    if (!c.answered) {
        add(@"(no result yet)\n", @{NSFontAttributeName: small, NSForegroundColorAttributeName: Muted(),
                                    NSParagraphStyleAttributeName: indented});
    } else if (!c.loaded) {
        add(@"Reading the output…\n", @{NSFontAttributeName: small,
                                        NSForegroundColorAttributeName: Muted(),
                                        NSParagraphStyleAttributeName: indented});
    } else if (c.full) {
        add(c.full, outAttrs);
        if (![c.full hasSuffix:@"\n"]) add(@"\n", outAttrs);
        if (c.fullCut)
            add(@"(cut at 1 MB)\n", @{NSFontAttributeName: small, NSForegroundColorAttributeName: Muted(),
                                      NSParagraphStyleAttributeName: indented});
        add(@"Show less", @{NSFontAttributeName: small, NSParagraphStyleAttributeName: indented,
                            NSLinkAttributeName: [NSURL URLWithString:[NSString stringWithFormat:
                                @"minicode-evidence://less/%lu", (unsigned long)i]]});
        add(@"\n", @{NSFontAttributeName: small, NSParagraphStyleAttributeName: indented});
    } else if (!c.head.count && !c.tail.count) {
        add(@"(no output)\n", @{NSFontAttributeName: small, NSForegroundColorAttributeName: Muted(),
                                NSParagraphStyleAttributeName: indented});
    } else {
        for (NSString *l in c.head) { add(l, outAttrs); add(@"\n", outAttrs); }
        if (!c.whole) {
            add([NSString stringWithFormat:@"⋯ %@  ", PNS(AgentView::moreLinesText(c.omitted))],
                @{NSFontAttributeName: small, NSForegroundColorAttributeName: Muted(),
                  NSParagraphStyleAttributeName: indented});
            add(@"Show all", @{NSFontAttributeName: small, NSParagraphStyleAttributeName: indented,
                               NSLinkAttributeName: [NSURL URLWithString:[NSString stringWithFormat:
                                   @"minicode-evidence://expand/%lu", (unsigned long)i]]});
            add(@"\n", @{NSFontAttributeName: small, NSParagraphStyleAttributeName: indented});
        }
        for (NSString *l in c.tail) { add(l, outAttrs); add(@"\n", outAttrs); }
    }
    return s;
}

- (void)renderCommands:(NSColor *)text {
    NSScrollView *scroll = _commandsScroll;
    const NSPoint origin = scroll.contentView.bounds.origin;
    NSMutableAttributedString *all = [NSMutableAttributedString new];
    if (!_loaded) {
        [all appendAttributedString:[[NSAttributedString alloc] initWithString:@"Reading the transcript…"
            attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:12.5],
                         NSForegroundColorAttributeName: Muted()}]];
    } else if (!_commands.count) {
        [all appendAttributedString:[[NSAttributedString alloc]
            initWithString:@"This session ran no shell commands."
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:12.5],
                             NSForegroundColorAttributeName: Muted()}]];
    }
    for (NSUInteger i = 0; i < _commands.count; i++)
        [all appendAttributedString:[self blockFor:_commands[i] index:i color:text]];
    [_commandsView.textStorage setAttributedString:all];
    [scroll.contentView scrollToPoint:origin];
    [scroll reflectScrolledClipView:scroll.contentView];
}

- (NSString *)commandText:(NSUInteger)index {
    if (index >= _commands.count) return @"";
    return [self blockFor:_commands[index] index:index color:NSColor.textColor].string;
}

// ------------------------------------------------------------- actions
- (BOOL)textView:(NSTextView *)tv clickedOnLink:(id)link atIndex:(NSUInteger)charIndex {
    (void)tv; (void)charIndex;
    NSURL *url = [link isKindOfClass:NSURL.class] ? link : [NSURL URLWithString:[link description]];
    if (![url.scheme isEqualToString:@"minicode-evidence"]) return NO;
    NSUInteger i = (NSUInteger)url.lastPathComponent.integerValue;
    if ([url.host isEqualToString:@"expand"]) [self expandCommand:i];
    else if ([url.host isEqualToString:@"less"]) [self collapseCommand:i];
    else if ([url.host isEqualToString:@"terminal"]) [self openCommandInTerminal:i];
    return YES;
}

- (void)expandCommand:(NSUInteger)index {
    if (index >= _commands.count) return;
    dispatch_async(_queue, ^{ self->_wantFull[index] = true; });
    [self update];
}

- (void)collapseCommand:(NSUInteger)index {
    if (index >= _commands.count) return;
    dispatch_async(_queue, ^{ self->_wantFull[index] = false; });
    _commands[index].full = nil;
    [self render];
}

- (BOOL)isExpanded:(NSUInteger)index {
    return index < _commands.count && _commands[index].full != nil;
}

- (void)openCommandInTerminal:(NSUInteger)index {
    if (index >= _commands.count || !self.onOpenInTerminal) return;
    MCEvCommand *c = _commands[index];
    self.onOpenInTerminal(c.command ?: @"", c.cwd.length ? c.cwd : _cwd);
}

@end

// ================================================================ worktree changes
@interface MCChangesTable : NSTableView
@property(nonatomic, copy) void (^onActivate)(NSInteger row);
@end
@implementation MCChangesTable
- (void)keyDown:(NSEvent *)event {
    NSString *ch = event.charactersIgnoringModifiers;
    unichar c = ch.length ? [ch characterAtIndex:0] : 0;
    if ((c == '\r' || c == 3) && self.onActivate) { self.onActivate(self.selectedRow); return; }
    [super keyDown:event];
}
- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSInteger row = [self rowAtPoint:p];
    [super mouseDown:event];
    if (row >= 0 && event.clickCount == 1 && self.onActivate) self.onActivate(row);
}
- (NSMenu *)menuForEvent:(NSEvent *)event {
    (void)event;
    return MCAdjustSettingsMenu(@"editor", self);
}
@end

@interface MCWorktreeChangesPage () <NSTableViewDataSource, NSTableViewDelegate>
@end

@implementation MCWorktreeChangesPage {
    __weak MCAgentModel *_model;
    MCEvidenceView *_view;
    NSTextField *_titleLabel, *_infoLabel, *_notice;
    NSScrollView *_scroll;
    MCChangesTable *_table;
    NSArray<MCWorktreeChange *> *_changes;
}

- (instancetype)initWithWorktree:(MCAgentWorktree *)worktree model:(MCAgentModel *)model {
    if ((self = [super init])) {
        _worktree = worktree;
        _model = model;
        _changes = @[];
        [self build];
        [self pageApplySettings];
        __weak MCWorktreeChangesPage *weakSelf = self;
        [model changesInWorktree:worktree
                      completion:^(NSArray<MCWorktreeChange *> *changes, NSString *error) {
            [weakSelf showChanges:changes error:error];
        }];
    }
    return self;
}

- (NSString *)pageTitle {
    return [NSString stringWithFormat:@"%@ vs %@", _worktree.path.lastPathComponent,
            _model.mainBranch ?: @"main"];
}
- (NSString *)pageKind { return @"worktree-changes"; }
- (NSString *)pageKey { return [@"worktree:" stringByAppendingString:_worktree.path ?: @""]; }
- (NSView *)pageView { return _view; }
- (NSView *)pageFocusView { return _table; }

- (void)build {
    _view = [[MCEvidenceView alloc] initWithFrame:NSMakeRect(0, 0, 600, 400)];
    _view.wantsLayer = YES;
    _view.postsFrameChangedNotifications = YES;
    __weak MCWorktreeChangesPage *weakSelf = self;
    [NSNotificationCenter.defaultCenter addObserverForName:NSViewFrameDidChangeNotification
                                                    object:_view queue:nil
                                                usingBlock:^(NSNotification *n) {
        (void)n;
        [weakSelf layout];
    }];
    _titleLabel = [NSTextField labelWithString:@""];
    _titleLabel.font = [NSFont systemFontOfSize:14 weight:NSFontWeightSemibold];
    _titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    _infoLabel = [NSTextField labelWithString:@""];
    _infoLabel.font = [NSFont systemFontOfSize:11];
    _infoLabel.textColor = Muted();
    _infoLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    _infoLabel.selectable = YES;
    _notice = [NSTextField wrappingLabelWithString:@"Asking git…"];
    _notice.font = [NSFont systemFontOfSize:12];
    _notice.textColor = Muted();
    [_view addSubview:_titleLabel];
    [_view addSubview:_infoLabel];
    [_view addSubview:_notice];

    _scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _scroll.hasVerticalScroller = YES;
    _scroll.autohidesScrollers = YES;
    _scroll.drawsBackground = NO;
    _table = [[MCChangesTable alloc] initWithFrame:NSZeroRect];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"change"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_table addTableColumn:col];
    _table.headerView = nil;
    _table.backgroundColor = NSColor.clearColor;
    _table.rowHeight = 22;
    _table.intercellSpacing = NSMakeSize(0, 0);
    _table.style = NSTableViewStylePlain;
    _table.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
    _table.dataSource = self;
    _table.delegate = self;
    _table.onActivate = ^(NSInteger row) { [weakSelf openChangeAtRow:row]; };
    _scroll.documentView = _table;
    [_view addSubview:_scroll];
    _view.menu = MCAdjustSettingsMenu(@"editor", _view);
    [self layout];
}

- (void)layout {
    const CGFloat W = _view.bounds.size.width, H = _view.bounds.size.height, pad = 12;
    _titleLabel.frame = NSMakeRect(pad, 10, W - 2 * pad, 20);
    _infoLabel.frame = NSMakeRect(pad, 31, W - 2 * pad, 16);
    _notice.frame = NSMakeRect(pad, 56, W - 2 * pad, 40);
    _scroll.frame = NSMakeRect(pad - 4, 56, W - 2 * pad + 4, MAX(0, H - 60));
}

- (void)pageApplySettings {
    AppSettings *cfg = [AppSettings shared];
    _view.layer.backgroundColor = [cfg background:Surface::Editor].CGColor;
    _titleLabel.textColor = [cfg text:Surface::Editor];
    [self updateLabels];
    [_table reloadData];
}

- (void)updateLabels {
    MCAgentWorktree *w = _worktree;
    _titleLabel.stringValue = [NSString stringWithFormat:@"%@ compared with %@", w.label ?: @"",
                               _model.mainBranch ?: @"main"];
    NSMutableArray *info = [NSMutableArray arrayWithObject:[w.path stringByAbbreviatingWithTildeInPath] ?: @""];
    if (w.branch.length) [info addObject:w.branch];
    if (w.badge.length) [info addObject:w.badge];
    if (_loaded)
        [info addObject:[NSString stringWithFormat:@"%lu changed %@", (unsigned long)_changes.count,
                         _changes.count == 1 ? @"file" : @"files"]];
    _infoLabel.stringValue = [info componentsJoinedByString:@"  ·  "];
}

- (void)showChanges:(NSArray<MCWorktreeChange *> *)changes error:(NSString *)error {
    _changes = changes ?: @[];
    _errorText = [error copy];
    _loaded = YES;
    _notice.stringValue = error.length ? error
                        : _changes.count ? @"" : @"No changes against the main branch.";
    _notice.textColor = error.length ? PHex(0xF48771) : Muted();
    _notice.hidden = _changes.count > 0 && !error.length;
    _scroll.hidden = !_notice.hidden;
    [_table reloadData];
    if (_changes.count) [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
    [self updateLabels];
    if (self.onLoaded) self.onLoaded();
}

- (NSArray<NSString *> *)rowDescriptions {
    NSMutableArray *out = [NSMutableArray array];
    for (MCWorktreeChange *c in _changes) {
        unichar s = c.status;
        [out addObject:[NSString stringWithFormat:@"%@ %@", [NSString stringWithCharacters:&s length:1],
                        c.path]];
    }
    return out;
}

- (NSTableView *)list { return _table; }

- (void)openChangeAtRow:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_changes.count) return;
    __weak MCWorktreeChangesPage *weakSelf = self;
    [_model comparisonForChange:_changes[(NSUInteger)row] inWorktree:_worktree
                     completion:^(MCComparison *c) {
        MCWorktreeChangesPage *s = weakSelf;
        if (s && s.onShowComparison && c) s.onShowComparison(c);
    }];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv {
    (void)tv;
    return (NSInteger)_changes.count;
}

- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    (void)col;
    NSTextField *f = [tv makeViewWithIdentifier:@"c" owner:self];
    if (!f) {
        f = [NSTextField labelWithString:@""];
        f.identifier = @"c";
        f.lineBreakMode = NSLineBreakByTruncatingMiddle;
    }
    MCWorktreeChange *c = _changes[(NSUInteger)row];
    unichar st = c.status;
    NSColor *letter = st == 'A' || st == '?' ? PHex(0x81B88B) : st == 'D' ? PHex(0xC74E39)
                    : st == 'R' || st == 'C' ? PHex(0x73C991) : PHex(0xE2C08D);
    NSMutableAttributedString *s = [[NSMutableAttributedString alloc]
        initWithString:[NSString stringWithFormat:@"%@   ", [NSString stringWithCharacters:&st length:1]]
            attributes:@{NSFontAttributeName: MCMonoFont(12, NSFontWeightSemibold),
                         NSForegroundColorAttributeName: letter}];
    [s appendAttributedString:[[NSAttributedString alloc]
        initWithString:c.path
            attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:12.5],
                         NSForegroundColorAttributeName: [[AppSettings shared] text:Surface::Editor]}]];
    if (c.from.length)
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:[@"  from " stringByAppendingString:c.from]
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11],
                             NSForegroundColorAttributeName: Muted()}]];
    f.attributedStringValue = s;
    return f;
}

@end
