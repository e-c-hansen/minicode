// AgentPages.mm — see AgentPages.h.
#import "AgentPages.h"
#import "EditorFont.h"
#import "AgentModel.h"
#import "AppSettings.h"
#import "Appearance.h"
#import "CompareView.h"

#include "AgentEdits.h"
#include "AgentEvidence.h"
#include "AgentTranscript.h"
#include "InlineAgent.h"
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

static NSColor *Muted() { return PHex(0x9D9D9D); }
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

// One file edit the Evidence page lists among the commands (main thread).
@interface MCEvEdit : NSObject
@property(nonatomic, copy) NSString *tool, *path;
@property(nonatomic) NSTimeInterval time;
@property(nonatomic) BOOL answered, isError;
@end
@implementation MCEvEdit
@end

static NSString *ClockTime(NSTimeInterval t) {
    if (t <= 0) return @"";
    static NSDateFormatter *f;
    if (!f) {
        f = [NSDateFormatter new];
        f.dateStyle = NSDateFormatterNoStyle;
        f.timeStyle = NSDateFormatterMediumStyle;
    }
    return [f stringFromDate:[NSDate dateWithTimeIntervalSince1970:t]];
}

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
    NSArray<MCEvEdit *> *_edits;
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

    NSMutableArray<MCEvEdit *> *edits = [NSMutableArray arrayWithCapacity:ev.edits.size()];
    for (const AgentEvidence::FileEdit &fe : ev.edits) {
        MCEvEdit *e = [MCEvEdit new];
        e.tool = PNS(fe.tool);
        e.path = PNS(fe.path);
        e.time = fe.time;
        e.answered = fe.answered;
        e.isError = fe.isError;
        [edits addObject:e];
    }
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
            s->_edits = edits;
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
    if (_edits.count)
        [info addObject:[NSString stringWithFormat:@"%lu file %@", (unsigned long)_edits.count,
                         _edits.count == 1 ? @"edit" : @"edits"]];
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
    } else if (!_commands.count && !_edits.count) {
        [all appendAttributedString:[[NSAttributedString alloc]
            initWithString:@"This session ran no shell commands."
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:12.5],
                             NSForegroundColorAttributeName: Muted()}]];
    }
    // Commands and file edits in the order they were asked for.
    NSUInteger ci = 0, ei = 0;
    while (ci < _commands.count || ei < _edits.count) {
        const BOOL edit = ei < _edits.count &&
                          (ci >= _commands.count || _edits[ei].time < _commands[ci].time);
        if (edit) {
            [all appendAttributedString:[self editBlock:_edits[ei] index:ei color:text]];
            ei++;
        } else {
            [all appendAttributedString:[self blockFor:_commands[ci] index:ci color:text]];
            ci++;
        }
    }
    [_commandsView.textStorage setAttributedString:all];
    [scroll.contentView scrollToPoint:origin];
    [scroll reflectScrolledClipView:scroll.contentView];
}

// One file edit: the tool's word, the file (a link to its page), the time.
- (NSAttributedString *)editBlock:(MCEvEdit *)e index:(NSUInteger)i color:(NSColor *)text {
    NSMutableAttributedString *s = [NSMutableAttributedString new];
    NSFont *small = [NSFont systemFontOfSize:11];
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.headIndent = 28;
    ps.paragraphSpacingBefore = 14;
    ps.tabStops = @[[[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft location:28 options:@{}]];
    auto add = [&](NSString *str, NSDictionary *attrs) {
        NSMutableDictionary *a = [attrs mutableCopy];
        a[NSParagraphStyleAttributeName] = ps;
        [s appendAttributedString:[[NSAttributedString alloc] initWithString:str ?: @"" attributes:a]];
    };
    NSString *word = [e.tool isEqualToString:@"Write"] ? @"Wrote"
                   : [e.tool isEqualToString:@"NotebookEdit"] ? @"Edited a cell of" : @"Edited";
    add(@"\t", @{NSFontAttributeName: small});
    add([word stringByAppendingString:@" "],
        @{NSFontAttributeName: [NSFont systemFontOfSize:12.5], NSForegroundColorAttributeName: text});
    NSString *shown = _cwd.length && [e.path hasPrefix:[_cwd stringByAppendingString:@"/"]]
        ? [e.path substringFromIndex:_cwd.length + 1] : [e.path stringByAbbreviatingWithTildeInPath];
    add(shown, @{NSFontAttributeName: MCMonoFont(12, NSFontWeightRegular),
                 NSLinkAttributeName: [NSURL URLWithString:[NSString stringWithFormat:
                     @"minicode-evidence://file/%lu", (unsigned long)i]]});
    if (e.answered && e.isError)
        add(@"   refused", @{NSFontAttributeName: [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold],
                             NSForegroundColorAttributeName: Red()});
    NSString *when = ClockTime(e.time);
    if (when.length)
        add([@"   " stringByAppendingString:when],
            @{NSFontAttributeName: small, NSForegroundColorAttributeName: Muted()});
    add(@"\n", @{NSFontAttributeName: small});
    return s;
}

- (NSUInteger)editCount { return _edits.count; }

- (NSString *)editText:(NSUInteger)index {
    if (index >= _edits.count) return @"";
    return [self editBlock:_edits[index] index:index color:NSColor.textColor].string;
}

- (void)openEdit:(NSUInteger)index {
    if (index < _edits.count && self.onShowFileChanges) self.onShowFileChanges(_edits[index].path);
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
    else if ([url.host isEqualToString:@"file"]) [self openEdit:i];
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

// ================================================================ a session's file
// One change as the page shows it (main thread).
@interface MCFileChange : NSObject
@property(nonatomic) AgentEdits::Tool tool;
@property(nonatomic, copy) NSString *oldText, *putText;   // what it replaced, and put in
@property(nonatomic) NSTimeInterval time;
@property(nonatomic) BOOL replaceAll, refused, answered, created;
@property(nonatomic) NSInteger part, parts;
@property(nonatomic) AgentEdits::Standing standing;
@end
@implementation MCFileChange
@end

static std::u16string U16Of(NSString *s) {
    std::u16string out(s.length, u'\0');
    if (s.length) [s getCharacters:(unichar *)out.data() range:NSMakeRange(0, s.length)];
    return out;
}

@interface MCSessionFilePage () <NSTextViewDelegate>
@end

@implementation MCSessionFilePage {
    NSString *_name;
    __weak MCAgentModel *_model;
    dispatch_queue_t _queue;
    MCEvidenceView *_view;
    NSTextField *_titleLabel, *_infoLabel;
    NSScrollView *_scroll;
    NSTextView *_text;
    NSArray<MCFileChange *> *_changes;
    NSString *_before, *_now, *_why, *_summary;
    BOOL _existedBefore, _existsNow, _updating, _again, _closed;
    NSInteger _standing, _gone, _unreadable;
    id _observer;
}

- (instancetype)initWithTranscript:(NSString *)transcript sessionName:(NSString *)name
                              file:(NSString *)path model:(MCAgentModel *)model {
    if ((self = [super init])) {
        _transcript = [transcript copy];
        _path = [path copy];
        _name = name.length ? [name copy] : transcript.lastPathComponent;
        _model = model;
        _queue = dispatch_queue_create("minicode.session-file", DISPATCH_QUEUE_SERIAL);
        _changes = @[];
        [self build];
        [self pageApplySettings];
        __weak MCSessionFilePage *weakSelf = self;
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
- (NSString *)pageTitle {
    return [NSString stringWithFormat:@"%@, changed by %@", _path.lastPathComponent, _name];
}
- (NSString *)pageKind { return @"session-file"; }
- (NSString *)pageKey {
    return [NSString stringWithFormat:@"session-file:%@#%@", _transcript, _path];
}
- (NSView *)pageView { return _view; }
- (NSView *)pageFocusView { return _text; }
- (void)pageClosed {
    _closed = YES;
    if (_observer) [NSNotificationCenter.defaultCenter removeObserver:_observer];
    _observer = nil;
}

- (void)build {
    _view = [[MCEvidenceView alloc] initWithFrame:NSMakeRect(0, 0, 600, 400)];
    _view.wantsLayer = YES;
    _view.postsFrameChangedNotifications = YES;
    __weak MCSessionFilePage *weakSelf = self;
    [NSNotificationCenter.defaultCenter addObserverForName:NSViewFrameDidChangeNotification
                                                    object:_view queue:nil
                                                usingBlock:^(NSNotification *n) {
        (void)n;
        [weakSelf layout];
    }];
    _titleLabel = [NSTextField labelWithString:@""];
    _titleLabel.font = [NSFont systemFontOfSize:14 weight:NSFontWeightSemibold];
    _titleLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    _titleLabel.selectable = YES;
    _infoLabel = [NSTextField labelWithString:@""];
    _infoLabel.font = [NSFont systemFontOfSize:11];
    _infoLabel.textColor = Muted();
    _infoLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    _infoLabel.selectable = YES;
    [_view addSubview:_titleLabel];
    [_view addSubview:_infoLabel];
    _scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _text = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 100)];
    _text.editable = NO;
    _text.selectable = YES;
    _text.richText = YES;
    _text.drawsBackground = NO;
    _text.usesFontPanel = NO;
    _text.usesFindBar = YES;
    _text.incrementalSearchingEnabled = YES;
    _text.textContainerInset = NSMakeSize(6, 6);
    _text.minSize = NSMakeSize(0, 0);
    _text.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _text.verticallyResizable = YES;
    _text.horizontallyResizable = NO;
    _text.autoresizingMask = NSViewWidthSizable;
    _text.textContainer.widthTracksTextView = YES;
    _text.layoutManager.allowsNonContiguousLayout = YES;
    _text.delegate = self;
    _text.linkTextAttributes = @{NSCursorAttributeName: NSCursor.pointingHandCursor,
                                 NSForegroundColorAttributeName: Accent()};
    _scroll.hasVerticalScroller = YES;
    _scroll.autohidesScrollers = YES;
    _scroll.drawsBackground = NO;
    _scroll.documentView = _text;
    [_view addSubview:_scroll];
    _view.menu = MCAdjustSettingsMenu(@"editor", _view);
    [self layout];
}

- (void)layout {
    const CGFloat W = _view.bounds.size.width, H = _view.bounds.size.height, pad = 12;
    _titleLabel.frame = NSMakeRect(pad, 10, W - 2 * pad, 20);
    _infoLabel.frame = NSMakeRect(pad, 31, W - 2 * pad, 16);
    _scroll.frame = NSMakeRect(pad - 6, 52, W - 2 * pad + 6, MAX(0, H - 56));
}

- (void)pageApplySettings {
    AppSettings *cfg = [AppSettings shared];
    _view.layer.backgroundColor = [cfg background:Surface::Editor].CGColor;
    _titleLabel.textColor = [cfg text:Surface::Editor];
    [self render];
}

- (NSTextView *)textView { return _text; }
- (NSUInteger)changeCount { return _changes.count; }

// ------------------------------------------------------------- reading
- (void)update {
    if (_closed) return;
    if (_updating) { _again = YES; return; }
    _updating = YES;
    NSString *transcript = _transcript, *path = _path;
    __weak MCSessionFilePage *weakSelf = self;
    dispatch_async(_queue, ^{
        // The transcript once from the start, each edit's input once, the
        // file as it is: all off the main thread (an input can be megabytes).
        const AgentTranscript::Fs fs = AgentTranscript::posixFs();
        AgentEdits::FileHistory h = AgentEdits::collect(fs, PU8(transcript), PU8(path));
        NSData *data = [NSData dataWithContentsOfFile:path];
        NSString *now = data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : nil;
        const bool exists = now != nil;
        AgentEdits::Result r = AgentEdits::analyze(h.changes, exists ? PU8(now) : "", exists);
        NSMutableArray<MCFileChange *> *changes = [NSMutableArray array];
        for (const AgentEdits::Change &c : r.changes) {
            MCFileChange *m = [MCFileChange new];
            m.tool = c.tool;
            m.oldText = PNS(c.oldText);
            m.putText = PNS(c.newText);
            m.time = c.time;
            m.replaceAll = c.replaceAll;
            m.refused = c.refused;
            m.answered = c.answered;
            m.created = c.created;
            m.part = c.part;
            m.parts = c.parts;
            m.standing = c.standing;
            [changes addObject:m];
        }
        NSString *before = r.rebuilt ? PNS(r.before) : nil;
        NSString *why = PNS(r.why);
        const BOOL existedBefore = r.existedBefore;
        const NSInteger standing = r.standing, gone = r.gone, unreadable = h.unreadable;
        dispatch_async(dispatch_get_main_queue(), ^{
            MCSessionFilePage *s = weakSelf;
            if (!s) return;
            s->_changes = changes;
            s->_before = before;
            s->_now = now;
            s->_existsNow = exists;
            s->_existedBefore = existedBefore;
            s->_why = why;
            s->_standing = standing;
            s->_gone = gone;
            s->_unreadable = unreadable;
            s->_loaded = YES;
            [s render];
            s->_updating = NO;
            if (s->_again) {
                s->_again = NO;
                [s update];
            }
            if (s.onUpdate) s.onUpdate();
        });
    });
}

- (BOOL)rebuilt { return _before != nil; }

// ------------------------------------------------------------- drawing
static NSString *CountLines(NSString *s) {
    __block NSUInteger n = 0;
    [s enumerateLinesUsingBlock:^(NSString *line, BOOL *stop) { (void)line; (void)stop; n++; }];
    return [NSString stringWithFormat:@"%lu line%@", (unsigned long)n, n == 1 ? @"" : @"s"];
}

- (NSAttributedString *)summaryFor:(NSColor *)text {
    NSMutableAttributedString *s = [NSMutableAttributedString new];
    NSFont *body = [NSFont systemFontOfSize:12.5];
    NSFont *small = [NSFont systemFontOfSize:11];
    auto add = [&](NSString *str, NSDictionary *attrs) {
        [s appendAttributedString:[[NSAttributedString alloc] initWithString:str ?: @"" attributes:attrs]];
    };
    NSUInteger counted = 0, refused = 0;
    for (MCFileChange *c in _changes) {
        if (c.refused) refused++;
        else counted++;
    }
    if (!_changes.count) {
        add(_unreadable ? @"The session's edits of this file could not be read from its transcript."
                        : @"This session did not change this file.",
            @{NSFontAttributeName: body, NSForegroundColorAttributeName: Muted()});
        add(@"\n", @{NSFontAttributeName: body});
    } else {
        NSString *what = [NSString stringWithFormat:@"%lu %@", (unsigned long)counted,
                          counted == 1 ? @"change" : @"changes"];
        NSString *standing = _gone == 0 && _standing == (NSInteger)counted
            ? (counted == 1 ? @"still in the file" : @"all still in the file")
            : [NSString stringWithFormat:@"%ld still in the file, %ld changed since", (long)_standing,
               (long)_gone];
        add([NSString stringWithFormat:@"%@, %@", what, standing],
            @{NSFontAttributeName: [NSFont systemFontOfSize:12.5 weight:NSFontWeightSemibold],
              NSForegroundColorAttributeName: _gone ? PHex(0xE8A33D) : text});
        if (refused)
            add([NSString stringWithFormat:@"; %lu refused and not made", (unsigned long)refused],
                @{NSFontAttributeName: body, NSForegroundColorAttributeName: Muted()});
        add(@"\n", @{NSFontAttributeName: body});
    }
    if (_before) {
        NSString *line = !_existedBefore
            ? [NSString stringWithFormat:@"The session made this file; it has %@ now.",
               CountLines(_now ?: @"")]
            : [NSString stringWithFormat:@"Before the session it had %@; now %@.", CountLines(_before),
               CountLines(_now ?: @"")];
        add(line, @{NSFontAttributeName: body, NSForegroundColorAttributeName: text});
        add(@"   ", @{NSFontAttributeName: small});
        add(@"Compare Before and Now", @{NSFontAttributeName: small,
            NSLinkAttributeName: [NSURL URLWithString:@"minicode-session-file://compare"]});
    } else if (_why.length && _changes.count) {
        add(_why, @{NSFontAttributeName: body, NSForegroundColorAttributeName: Muted()});
    }
    if (_existsNow) {
        add(@"   ", @{NSFontAttributeName: small});
        add(@"Open the File", @{NSFontAttributeName: small,
            NSLinkAttributeName: [NSURL URLWithString:@"minicode-session-file://open"]});
    }
    if (_unreadable && _changes.count)
        add([NSString stringWithFormat:@"\n%ld more could not be read from the transcript.",
             (long)_unreadable], @{NSFontAttributeName: small, NSForegroundColorAttributeName: Muted()});
    add(@"\n", @{NSFontAttributeName: body});
    return s;
}

- (NSString *)summaryText {
    return [self summaryFor:NSColor.textColor].string ?: @"";
}

- (NSAttributedString *)blockFor:(MCFileChange *)c index:(NSUInteger)i color:(NSColor *)text {
    NSMutableAttributedString *s = [NSMutableAttributedString new];
    NSFont *body = [NSFont systemFontOfSize:12.5];
    NSFont *bold = [NSFont systemFontOfSize:12.5 weight:NSFontWeightSemibold];
    NSFont *small = [NSFont systemFontOfSize:11];
    NSFont *mono = MCMonoFont(11.5, NSFontWeightRegular);
    NSMutableParagraphStyle *first = [NSMutableParagraphStyle new];
    first.paragraphSpacingBefore = 16;
    first.headIndent = 28;
    first.tabStops = @[[[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft location:28 options:@{}]];
    NSMutableParagraphStyle *indented = [first mutableCopy];
    indented.paragraphSpacingBefore = 0;
    indented.firstLineHeadIndent = 28;
    auto add = [&](NSString *str, NSDictionary *attrs) {
        [s appendAttributedString:[[NSAttributedString alloc] initWithString:str ?: @"" attributes:attrs]];
    };
    NSString *what;
    switch (c.tool) {
    case AgentEdits::Tool::Write: what = c.created ? @"Made the file" : @"Rewrote the whole file"; break;
    case AgentEdits::Tool::NotebookEdit: what = @"Changed a notebook cell"; break;
    case AgentEdits::Tool::MultiEdit:
        what = [NSString stringWithFormat:@"Edit, part %ld of %ld", (long)c.part + 1, (long)c.parts];
        break;
    case AgentEdits::Tool::Edit: what = c.replaceAll ? @"Edit, every place" : @"Edit"; break;
    }
    add([NSString stringWithFormat:@"%lu\t", (unsigned long)i + 1],
        @{NSFontAttributeName: small, NSForegroundColorAttributeName: Muted(),
          NSParagraphStyleAttributeName: first});
    add(what, @{NSFontAttributeName: bold, NSForegroundColorAttributeName: text,
                NSParagraphStyleAttributeName: first});
    NSString *when = ClockTime(c.time);
    if (when.length)
        add([@"   " stringByAppendingString:when],
            @{NSFontAttributeName: small, NSForegroundColorAttributeName: Muted(),
              NSParagraphStyleAttributeName: first});
    add(@"\n", @{NSFontAttributeName: body, NSParagraphStyleAttributeName: first});
    NSString *note = nil;
    NSColor *noteColor = Muted();
    if (c.refused) {
        note = @"Refused: Claude Code reported an error and did not make this change.";
        noteColor = Red();
    } else if (c.standing == AgentEdits::Standing::Gone) {
        note = c.tool == AgentEdits::Tool::Write
            ? @"No longer what the file holds: something changed it since."
            : @"No longer in the file: something changed this text since.";
        noteColor = PHex(0xE8A33D);
    } else if (!c.answered) {
        note = @"No result yet in the transcript.";
    }
    if (note)
        add([note stringByAppendingString:@"\n"],
            @{NSFontAttributeName: small, NSForegroundColorAttributeName: noteColor,
              NSParagraphStyleAttributeName: indented});
    // The change as a small diff: old lines red, new green, changed words
    // marked (InlineAgent::pendingDiff, as the inline agent's bar shows one).
    NSColor *red = Red(), *green = Green();
    const std::u16string a = U16Of(c.oldText ?: @""), b = U16Of(c.putText ?: @"");
    for (const InlineAgent::DiffLine &d : InlineAgent::pendingDiff(a, b, 2, 200)) {
        NSString *lineBody = [NSString stringWithCharacters:(const unichar *)d.text.data()
                                                     length:d.text.size()];
        NSString *line = nil;
        NSColor *fg = text, *bg = nil, *strong = nil;
        switch (d.kind) {
        case InlineAgent::DiffLine::Gap:
            line = d.more ? [NSString stringWithFormat:@"  ⋯ %zu more line%@", d.more,
                                                       d.more == 1 ? @"" : @"s"]
                          : @"  ⋯";
            fg = Muted();
            break;
        case InlineAgent::DiffLine::Context:
            line = [@"  " stringByAppendingString:lineBody];
            fg = Muted();
            break;
        case InlineAgent::DiffLine::Removed:
            line = [@"- " stringByAppendingString:lineBody];
            bg = [red colorWithAlphaComponent:0.18];
            strong = [red colorWithAlphaComponent:0.45];
            break;
        case InlineAgent::DiffLine::Added:
            line = [@"+ " stringByAppendingString:lineBody];
            bg = [green colorWithAlphaComponent:0.18];
            strong = [green colorWithAlphaComponent:0.45];
            break;
        }
        NSMutableDictionary *attrs = [@{NSFontAttributeName: mono, NSForegroundColorAttributeName: fg,
                                        NSParagraphStyleAttributeName: indented} mutableCopy];
        if (bg) attrs[NSBackgroundColorAttributeName] = bg;
        NSMutableAttributedString *l = [[NSMutableAttributedString alloc]
            initWithString:[line stringByAppendingString:@"\n"] attributes:attrs];
        const NSUInteger at = line.length - lineBody.length;
        for (const auto &w : d.words)
            if (d.kind != InlineAgent::DiffLine::Gap && w.first + w.second <= lineBody.length)
                [l addAttribute:NSBackgroundColorAttributeName value:strong
                          range:NSMakeRange(at + w.first, w.second)];
        [s appendAttributedString:l];
    }
    return s;
}

- (NSString *)changeText:(NSUInteger)index {
    if (index >= _changes.count) return @"";
    return [self blockFor:_changes[index] index:index color:NSColor.textColor].string;
}

- (void)render {
    NSColor *text = [[AppSettings shared] text:Surface::Editor];
    _titleLabel.stringValue = [self pageTitle];
    _infoLabel.stringValue = [_path stringByAbbreviatingWithTildeInPath] ?: @"";
    const NSPoint origin = _scroll.contentView.bounds.origin;
    NSMutableAttributedString *all = [NSMutableAttributedString new];
    if (!_loaded) {
        [all appendAttributedString:[[NSAttributedString alloc] initWithString:@"Reading the transcript…"
            attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:12.5],
                         NSForegroundColorAttributeName: Muted()}]];
    } else {
        [all appendAttributedString:[self summaryFor:text]];
        for (NSUInteger i = 0; i < _changes.count; i++)
            [all appendAttributedString:[self blockFor:_changes[i] index:i color:text]];
    }
    [_text.textStorage setAttributedString:all];
    [_scroll.contentView scrollToPoint:origin];
    [_scroll reflectScrolledClipView:_scroll.contentView];
}

// ------------------------------------------------------------- actions
- (MCComparison *)beforeAndNow {
    if (!_before) return nil;
    MCComparison *c = [MCComparison new];
    c.title = [NSString stringWithFormat:@"%@: before %@ and now", _path.lastPathComponent, _name];
    c.leftLabel = _existedBefore ? [NSString stringWithFormat:@"Before the session: %@",
                                    _path.lastPathComponent]
                                 : @"Before the session: no file";
    c.rightLabel = [@"Now: " stringByAppendingString:_path.lastPathComponent];
    c.leftText = _before;
    c.rightText = _now ?: @"";
    c.path = _path;
    return c;
}

- (void)compareBeforeAndNow {
    MCComparison *c = [self beforeAndNow];
    if (!c) { NSBeep(); return; }
    [c prepare];
    if (self.onShowComparison) self.onShowComparison(c);
}

- (BOOL)textView:(NSTextView *)tv clickedOnLink:(id)link atIndex:(NSUInteger)charIndex {
    (void)tv; (void)charIndex;
    NSURL *url = [link isKindOfClass:NSURL.class] ? link : [NSURL URLWithString:[link description]];
    if (![url.scheme isEqualToString:@"minicode-session-file"]) return NO;
    if ([url.host isEqualToString:@"compare"]) [self compareBeforeAndNow];
    else if ([url.host isEqualToString:@"open"] && self.onOpenFile) self.onOpenFile(_path);
    return YES;
}

@end
