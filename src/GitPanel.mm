// GitPanel.mm — Objective-C++. See GitPanel.h.
#import "GitPanel.h"
#import "AppSettings.h"
#import "Lsp.h"   // MCFindProgram, MCSearchDirs
#include "GitStatus.h"
#include <string>

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
@end
@implementation MCGitSnapshot
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
@property(nonatomic, copy) BOOL (^selectable)(NSInteger row);
- (void)moveBy:(NSInteger)step;   // to the next file row up or down
@end
@implementation MCGitTable
- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSInteger row = [self rowAtPoint:p];
    [super mouseDown:event];
    if (row >= 0 && self.onClick && event.clickCount == 1) self.onClick(row);
}
- (void)moveBy:(NSInteger)step {
    NSInteger n = self.numberOfRows, r = self.selectedRow;
    if (r < 0) r = step > 0 ? -1 : n;
    for (r += step; r >= 0 && r < n; r += step) {
        if (self.selectable && !self.selectable(r)) continue;
        [self selectRowIndexes:[NSIndexSet indexSetWithIndex:r] byExtendingSelection:NO];
        [self scrollRowToVisible:r];
        return;
    }
}
- (void)keyDown:(NSEvent *)event {
    NSEventModifierFlags m = event.modifierFlags &
        (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption);
    NSString *ch = event.charactersIgnoringModifiers;
    unichar c = ch.length ? [ch characterAtIndex:0] : 0;
    if (!m) {
        if ((c == '\r' || c == 3) && self.onActivate) { self.onActivate(self.selectedRow); return; }
        if (c == ' ' && self.onToggle) { self.onToggle(self.selectedRow); return; }
        if ((c == '\t' || c == 25) && self.onTab) { self.onTab(); return; }
        if (c == NSUpArrowFunctionKey) { [self moveBy:-1]; return; }
        if (c == NSDownArrowFunctionKey) { [self moveBy:1]; return; }
    }
    [super keyDown:event];
}
@end

// The commit message: Command+Return commits, Tab goes back to the list, and
// a muted hint shows while it is empty.
@interface MCCommitTextView : NSTextView
@property(nonatomic, copy) void (^onCommit)(void);
@property(nonatomic, copy) void (^onTab)(void);
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
- (void)insertBacktab:(id)sender { if (self.onTab) self.onTab(); else [super insertBacktab:sender]; }
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
}

- (instancetype)initWithRoot:(NSString *)root {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 260, 400)])) {
        _root = [root copy];
        _queue = dispatch_queue_create("minicode.git", DISPATCH_QUEUE_SERIAL);
        _rows = @[];
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
    _table.onTab = ^{ [weakSelf focusMessage]; };
    _table.selectable = ^BOOL(NSInteger row) { return [weakSelf rowIsFile:row]; };
    _listScroll.documentView = _table;
    [self addSubview:_listScroll];

    _notice = [NSTextField wrappingLabelWithString:@""];
    _notice.font = [NSFont systemFontOfSize:12];
    _notice.textColor = GHex(0x9CA3AF);
    _notice.hidden = YES;
    [self addSubview:_notice];
    [self showMessageArea:NO];
}

- (void)applySettings {
    AppSettings *cfg = [AppSettings shared];
    self.layer.backgroundColor = [cfg background:Surface::Sidebar].CGColor;
    _text = [cfg text:Surface::Sidebar];
    _branchLabel.textColor = _text;
    _message.textColor = _text;
    _message.insertionPointColor = _text;
    _messageScroll.layer.backgroundColor = [cfg background:Surface::Editor].CGColor;
    [_table reloadData];
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
    _listScroll.frame = NSMakeRect(0, y, W, MAX(0, H - y));
    [_table sizeLastColumnToFit];
    if (!_notice.hidden) {
        NSSize fit = [_notice.cell cellSizeForBounds:NSMakeRect(0, 0, inner, 200)];
        _notice.frame = NSMakeRect(pad, y + 6, inner, ceil(fit.height));
    }
}

- (void)setRoot:(NSString *)root {
    _root = [root copy];
    _rows = @[];
    _topLevel = nil;
    _inRepository = NO;
    [_table reloadData];
    [self setError:nil info:NO];
    [self refresh];
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) [self refresh];
}

- (void)focusList {
    if (_table.selectedRow < 0) [_table moveBy:1];
    if (_table.selectedRow < 0 && !_messageScroll.hidden) {
        [self focusMessage];
        return;
    }
    [self.window makeFirstResponder:_table];
}

- (void)focusMessage {
    if (_messageScroll.hidden) return;
    [self.window makeFirstResponder:_message];
}

- (NSTableView *)list { return _table; }
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

- (void)runRefresh {
    NSString *root = self.root;
    if (!root) return;
    dispatch_async(_queue, ^{
        MCGitSnapshot *snap = [MCGitPanel snapshotAt:root];
        dispatch_async(dispatch_get_main_queue(), ^{
            if (![root isEqualToString:self.root]) return;   // folder changed
            [self applySnapshot:snap];
        });
    });
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

// ------------------------------------------------------------ table view
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv {
    (void)tv;
    return (NSInteger)_rows.count;
}

- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)col
                  row:(NSInteger)row {
    (void)col;
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
    (void)tv;
    return [self rowIsFile:row];
}

- (CGFloat)tableView:(NSTableView *)tv heightOfRow:(NSInteger)row {
    (void)tv;
    return _rows[row].header ? 26 : 22;
}

@end
