// AgentTaskBar.mm — see AgentTaskBar.h.
#import "AgentTaskBar.h"
#import "AgentModel.h"
#import "AppSettings.h"
#import "ChangePanel.h"   // MCRemoveStalePromptFiles
#import "GitPanel.h"
#import "LspServer.h"     // MCResolveCommand
#import "Terminal.h"

#include "AgentTask.h"
#include "LspClient.h"    // Lsp::splitCommand

#include <set>
#include <unistd.h>

static std::string TStd(NSString *s) { return s.UTF8String ?: ""; }
static NSString *TNS(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size()
                                  encoding:NSUTF8StringEncoding] ?: @"";
}
static NSColor *THex(unsigned rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1];
}
static NSString *GitText(MCGitResult *r) {
    if (!r.output.length) return @"";
    return [[[NSString alloc] initWithData:r.output encoding:NSUTF8StringEncoding]
               stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] ?: @"";
}

// ------------------------------------------------------------ the bar
// Blue where Request a Change is amber: an ordinary task in this folder,
// not a change to minicode. Inline hex like the other accents
// (docs/notes/ui.md).
static const unsigned kTaskBlue = 0x4EA1F7;
static const unsigned kProblemRed = 0xF14C4C;
static const CGFloat kStripe = 2;
static const CGFloat kHeadH = 18;
static const CGFloat kNameRowH = 22;
static const CGFloat kMaxLines = 8;

// The prompt box: a text view that wraps, with a placeholder.
@interface MCTaskPromptView : NSTextView
@property(nonatomic, copy) NSString *placeholder;
@property(nonatomic, strong) NSColor *placeholderColor;
@end
@implementation MCTaskPromptView
- (void)drawRect:(NSRect)r {
    [super drawRect:r];
    if (self.string.length || !self.placeholder.length) return;
    const NSPoint o = self.textContainerOrigin;
    [self.placeholder drawAtPoint:NSMakePoint(o.x + self.textContainer.lineFragmentPadding, o.y)
                   withAttributes:@{NSFontAttributeName: self.font,
                                    NSForegroundColorAttributeName:
                                        self.placeholderColor ?: NSColor.grayColor}];
}
@end

@implementation MCAgentTaskBar {
    NSTextField *_title, *_hintLabel, *_nameLabel, *_whereLabel;
    NSScrollView *_box;
    MCTaskPromptView *_text;
    NSTextField *_name;
    NSButton *_start, *_close;
    NSView *_stripe, *_line;
    NSColor *_textColor;
    BOOL _settingName;
}

- (BOOL)isFlipped { return YES; }

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.wantsLayer = YES;
    _title = [NSTextField labelWithString:@"New agent task"];
    _title.font = [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold];
    _title.textColor = THex(kTaskBlue);
    _hintLabel = [NSTextField labelWithString:@""];
    _hintLabel.font = [NSFont systemFontOfSize:11];
    _hintLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    _nameLabel = [NSTextField labelWithString:@"Name"];
    _nameLabel.font = [NSFont systemFontOfSize:11];
    _whereLabel = [NSTextField labelWithString:@""];
    _whereLabel.font = [NSFont systemFontOfSize:11];
    _whereLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;

    // TextKit 1, built by hand, as Request a Change's box is.
    NSTextStorage *storage = [NSTextStorage new];
    NSLayoutManager *lm = [NSLayoutManager new];
    [storage addLayoutManager:lm];
    NSTextContainer *tc = [[NSTextContainer alloc] initWithSize:NSMakeSize(200, CGFLOAT_MAX)];
    tc.widthTracksTextView = YES;
    [lm addTextContainer:tc];
    _text = [[MCTaskPromptView alloc] initWithFrame:NSMakeRect(0, 0, 200, 24) textContainer:tc];
    _text.font = [NSFont systemFontOfSize:13];
    _text.richText = NO;
    _text.importsGraphics = NO;
    _text.allowsUndo = YES;
    _text.usesFontPanel = NO;
    _text.automaticQuoteSubstitutionEnabled = NO;
    _text.automaticDashSubstitutionEnabled = NO;
    _text.automaticTextReplacementEnabled = NO;
    _text.textContainerInset = NSMakeSize(4, 4);
    _text.verticallyResizable = YES;
    _text.horizontallyResizable = NO;
    _text.autoresizingMask = NSViewWidthSizable;
    _text.minSize = NSMakeSize(0, 0);
    _text.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    _text.placeholder = @"What should Claude Code do? Return starts it on a worktree of its own; "
                        @"Shift Return adds a line, Esc closes.";
    _text.delegate = self;
    _box = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 200, 24)];
    _box.hasVerticalScroller = YES;
    _box.autohidesScrollers = YES;
    _box.borderType = NSNoBorder;
    _box.documentView = _text;
    _box.wantsLayer = YES;
    _box.layer.cornerRadius = 5;
    _box.layer.borderWidth = 1;
    _box.layer.masksToBounds = YES;

    _name = [NSTextField textFieldWithString:@""];
    _name.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    _name.placeholderString = @"made from the prompt";
    _name.delegate = self;
    _name.bezelStyle = NSTextFieldRoundedBezel;
    _name.toolTip = @"The task's branch and the name of its folder under .claude/worktrees";

    _start = [NSButton buttonWithTitle:@"Start" target:self action:@selector(startPressed:)];
    _start.controlSize = NSControlSizeSmall;
    _start.font = [NSFont systemFontOfSize:11];
    _start.bezelStyle = NSBezelStyleRounded;
    _start.toolTip = @"Make the worktree and start Claude Code in a terminal (Return)";
    _close = [NSButton buttonWithTitle:@"×" target:self action:@selector(closePressed:)];
    _close.bordered = NO;
    _close.toolTip = @"Close (Esc)";
    _stripe = [NSView new];
    _stripe.wantsLayer = YES;
    _stripe.layer.backgroundColor = THex(kTaskBlue).CGColor;
    _line = [NSView new];
    _line.wantsLayer = YES;
    for (NSView *v in @[_title, _hintLabel, _box, _nameLabel, _name, _whereLabel, _start, _close,
                        _stripe, _line])
        [self addSubview:v];
    [self setBackground:THex(0x252526) text:THex(0xCCCCCC) field:THex(0x1E1E1E)];
    [self updateWhere];
    return self;
}

- (NSTextView *)textView { return _text; }
- (NSTextField *)nameField { return _name; }
- (NSButton *)startButton { return _start; }

- (void)clear {
    _text.string = @"";
    _nameEdited = NO;
    [self setName:@""];
    [self textDidChange:[NSNotification notificationWithName:NSTextDidChangeNotification object:_text]];
}

- (void)setHint:(NSString *)hint {
    _hint = [hint copy];
    _hintLabel.stringValue = _hint ?: @"";
    _hintLabel.toolTip = _hint;
    [self applyHintColor];
    self.needsLayout = YES;
}

- (void)setHintIsProblem:(BOOL)problem {
    _hintIsProblem = problem;
    [self applyHintColor];
}

- (void)applyHintColor {
    _hintLabel.textColor = _hintIsProblem ? THex(kProblemRed)
                                          : [(_textColor ?: NSColor.textColor) colorWithAlphaComponent:0.6];
}

- (void)setBackground:(NSColor *)background text:(NSColor *)text field:(NSColor *)field {
    NSColor *blue = THex(kTaskBlue);
    NSColor *bg = [background colorUsingColorSpace:NSColorSpace.sRGBColorSpace] ?: background;
    NSColor *tint = [[bg blendedColorWithFraction:0.06 ofColor:blue]
                        colorWithAlphaComponent:bg.alphaComponent];
    self.layer.backgroundColor = (tint ?: bg).CGColor;
    _textColor = text;
    [self applyHintColor];
    NSColor *muted = [text colorWithAlphaComponent:0.6];
    _nameLabel.textColor = muted;
    _whereLabel.textColor = muted;
    _line.layer.backgroundColor = [blue colorWithAlphaComponent:0.35].CGColor;
    _text.backgroundColor = field;
    _text.drawsBackground = YES;
    _box.backgroundColor = field;
    _box.layer.borderColor = [blue colorWithAlphaComponent:0.75].CGColor;
    _text.textColor = text;
    _text.insertionPointColor = text;
    _text.placeholderColor = [text colorWithAlphaComponent:0.4];
    _text.typingAttributes = @{NSFontAttributeName: _text.font, NSForegroundColorAttributeName: text};
    _name.textColor = text;
    _name.backgroundColor = field;
    _name.drawsBackground = YES;
    _close.attributedTitle = [[NSAttributedString alloc] initWithString:@"×"
        attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:15],
                     NSForegroundColorAttributeName: [text colorWithAlphaComponent:0.7]}];
    _text.needsDisplay = YES;
}

// The box's height at `width`: its text laid out, one to kMaxLines lines.
- (CGFloat)boxHeightForWidth:(CGFloat)width {
    static NSTextStorage *storage;
    static NSLayoutManager *lm;
    static NSTextContainer *tc;
    if (!storage) {
        storage = [NSTextStorage new];
        lm = [NSLayoutManager new];
        [storage addLayoutManager:lm];
        tc = [[NSTextContainer alloc] initWithSize:NSMakeSize(100, CGFLOAT_MAX)];
        [lm addTextContainer:tc];
    }
    const NSSize inset = _text.textContainerInset;
    tc.lineFragmentPadding = _text.textContainer.lineFragmentPadding;
    tc.size = NSMakeSize(MAX(20, width - 2 * inset.width), CGFLOAT_MAX);
    [storage setAttributedString:_text.textStorage];
    [storage addAttribute:NSFontAttributeName value:_text.font range:NSMakeRange(0, storage.length)];
    [lm ensureLayoutForTextContainer:tc];
    const CGFloat line = [lm defaultLineHeightForFont:_text.font];
    const CGFloat h = MIN(MAX(ceil([lm usedRectForTextContainer:tc].size.height), line),
                          line * kMaxLines);
    return h + 2 * inset.height;
}

- (CGFloat)heightForWidth:(CGFloat)W {
    return kStripe + 6 + kHeadH + 4 + [self boxHeightForWidth:MAX(60, W - 24)] + 6 + kNameRowH + 8;
}

- (void)layout {
    [super layout];
    const NSRect b = self.bounds;
    const CGFloat W = NSWidth(b), H = NSHeight(b);
    _stripe.frame = NSMakeRect(0, 0, W, kStripe);
    _line.frame = NSMakeRect(0, H - 1, W, 1);
    [_close sizeToFit];
    const NSSize c = _close.frame.size;
    const CGFloat y0 = kStripe + 6;
    _close.frame = NSMakeRect(W - 8 - c.width, y0 + round((kHeadH - c.height) / 2), c.width, c.height);
    [_title sizeToFit];
    const CGFloat tw = ceil(NSWidth(_title.frame));
    _title.frame = NSMakeRect(12, y0 + 1, tw, 16);
    const CGFloat hx = 12 + tw + 10;
    _hintLabel.frame = NSMakeRect(hx, y0 + 2, MAX(0, NSMinX(_close.frame) - 8 - hx), 15);
    const CGFloat boxY = y0 + kHeadH + 4, boxW = MAX(60, W - 24);
    const CGFloat boxH = MAX(0, MIN([self boxHeightForWidth:boxW], H - boxY - 6 - kNameRowH - 8));
    _box.frame = NSMakeRect(12, boxY, boxW, boxH);
    const CGFloat rowY = boxY + boxH + 6;
    [_nameLabel sizeToFit];
    const CGFloat nlw = ceil(NSWidth(_nameLabel.frame));
    _nameLabel.frame = NSMakeRect(12, rowY + 3, nlw, 16);
    _name.frame = NSMakeRect(12 + nlw + 6, rowY, MIN(240, MAX(80, W * 0.3)), kNameRowH);
    [_start sizeToFit];
    const NSSize a = _start.frame.size;
    _start.frame = NSMakeRect(W - 12 - a.width, rowY + round((kNameRowH - a.height) / 2), a.width, a.height);
    const CGFloat wx = NSMaxX(_name.frame) + 8;
    _whereLabel.frame = NSMakeRect(wx, rowY + 3, MAX(0, NSMinX(_start.frame) - 8 - wx), 16);
}

// The folder and branch the name gives, as it will be made fit for git.
- (void)updateWhere {
    std::string n = AgentTask::sanitizeName(TStd(_name.stringValue));
    if (n.empty()) n = AgentTask::nameFromPrompt(TStd(_text.string));
    _whereLabel.stringValue = [NSString stringWithFormat:@"branch %@ in .claude/worktrees/%@",
                               TNS(n), TNS(n)];
}

- (void)setName:(NSString *)name {
    _settingName = YES;
    _name.stringValue = name ?: @"";
    _settingName = NO;
    [self updateWhere];
}

- (void)startPressed:(id)sender { (void)sender; if (self.onStart) self.onStart(); }
- (void)closePressed:(id)sender { (void)sender; if (self.onClose) self.onClose(); }

// The prompt changed: the name follows it until the user types one; the
// box may have grown a line.
- (void)textDidChange:(NSNotification *)note {
    (void)note;
    _text.needsDisplay = YES;
    if (!_nameEdited) {
        NSString *trimmed = [_text.string stringByTrimmingCharactersInSet:
                                 NSCharacterSet.whitespaceAndNewlineCharacterSet];
        [self setName:trimmed.length ? TNS(AgentTask::nameFromPrompt(TStd(trimmed))) : @""];
    } else {
        [self updateWhere];
    }
    if (!self.superview) return;
    if (fabs([self heightForWidth:NSWidth(self.bounds)] - NSHeight(self.bounds)) > 0.5) {
        if (self.onResize) self.onResize();
    } else {
        self.needsLayout = YES;
    }
}

// The name field: typing makes it the user's; emptied, it follows the
// prompt again.
- (void)controlTextDidChange:(NSNotification *)note {
    if (note.object != _name || _settingName) return;
    _nameEdited = _name.stringValue.length > 0;
    [self updateWhere];
}

// Return starts, Shift or Option+Return adds a line, Esc closes, Tab goes
// to the name.
- (BOOL)textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
    if (sel == @selector(insertNewline:)) {
        const NSEventModifierFlags f = NSApp.currentEvent.modifierFlags;
        if (!(f & (NSEventModifierFlagShift | NSEventModifierFlagOption))) {
            [self startPressed:nil];
            return YES;
        }
        sel = @selector(insertLineBreak:);
    }
    if (sel == @selector(insertLineBreak:) || sel == @selector(insertNewlineIgnoringFieldEditor:)) {
        [tv insertText:@"\n" replacementRange:tv.selectedRange];
        return YES;
    }
    if (sel == @selector(cancelOperation:) || sel == @selector(complete:)) {
        [self closePressed:nil];
        return YES;
    }
    if (sel == @selector(insertTab:) || sel == @selector(insertBacktab:)) {
        [tv.window makeFirstResponder:_name];
        return YES;
    }
    return NO;
}

// The same keys in the name field (its field editor asks here).
- (BOOL)control:(NSControl *)control textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
    (void)control;
    if (sel == @selector(insertNewline:)) { [self startPressed:nil]; return YES; }
    if (sel == @selector(cancelOperation:)) { [self closePressed:nil]; return YES; }
    if (sel == @selector(insertTab:) || sel == @selector(insertBacktab:)) {
        [tv.window makeFirstResponder:_text];
        return YES;
    }
    return NO;
}
@end

// ------------------------------------------------------------ the session
// Whether the agent's command can be run: its program found as the
// language servers' are, or Claude Code's old local install, which a shell
// alias names (read only; nothing under ~/.claude is written).
static BOOL MCAgentProgramFound(const std::string &command) {
    if (MCResolveCommand(command, nil)) return YES;
    const std::vector<std::string> words = Lsp::splitCommand(command);
    if (!words.empty() && words[0] == "claude") {
        NSString *local = [NSHomeDirectory() stringByAppendingPathComponent:@".claude/local/claude"];
        if ([NSFileManager.defaultManager isExecutableFileAtPath:local]) return YES;
    }
    return NO;
}

static NSString *MCNotInstalledText(const std::string &command) {
    return [NSString stringWithFormat:
        @"Claude Code (%@) was not found. Install it from claude.com/claude-code, "
        @"or name the command in the settings file's agent.command.", TNS(command)];
}

// What a look at the repository found, on the queue.
struct TaskRepoLook {
    std::string problem;   // "" when a task can start here
    std::string branch;    // "" on a detached HEAD
    int changed = 0;
};

static TaskRepoLook MCLookAtRepository(NSString *root) {
    TaskRepoLook look;
    if (!MCGitExecutable()) {
        look.problem = "A task's worktree needs git, which is not installed. Install Apple's "
                       "command line tools with xcode-select --install.";
        return look;
    }
    MCGitResult *top = MCGitRunSync(root, @[@"rev-parse", @"--show-toplevel"]);
    if (!top.ok) {
        look.problem = "This folder is not in a git repository, so there is nothing to make a "
                       "task's worktree from.";
        return look;
    }
    MCGitResult *head = MCGitRunSync(root, @[@"rev-parse", @"--verify", @"--quiet", @"HEAD"]);
    if (!head.ok) {
        look.problem = "The repository has no commit yet, and a task's worktree starts from the "
                       "last commit.";
        return look;
    }
    MCGitResult *branch = MCGitRunSync(root, @[@"symbolic-ref", @"--quiet", @"--short", @"HEAD"]);
    if (branch.ok) look.branch = TStd(GitText(branch));
    MCGitResult *status = MCGitRunSync(root, @[@"status", @"--porcelain", @"--untracked-files=normal"]);
    if (status.ok)
        for (NSString *l in [GitText(status) componentsSeparatedByString:@"\n"])
            if (l.length) look.changed++;
    return look;
}

@implementation MCAgentTaskSession {
    __weak id<MCAgentTaskHost> _host;
    __weak MCAgentModel *_model;
    dispatch_queue_t _queue;
    NSUInteger _lookGeneration;
}
@synthesize bar = _bar;

- (instancetype)initWithHost:(id<MCAgentTaskHost>)host model:(MCAgentModel *)model {
    if ((self = [super init])) {
        _host = host;
        _model = model;
        _queue = dispatch_queue_create("minicode.agent-task", DISPATCH_QUEUE_SERIAL);
    }
    return self;
}

- (MCAgentTaskBar *)bar {
    if (!_bar) {
        _bar = [[MCAgentTaskBar alloc] initWithFrame:NSMakeRect(0, 0, 600, 100)];
        __weak MCAgentTaskSession *weakSelf = self;
        _bar.onStart = ^{ [weakSelf start]; };
        _bar.onClose = ^{ [weakSelf close]; };
        _bar.onResize = ^{
            MCAgentTaskSession *s = weakSelf;
            [s->_host agentTaskBarDidChange];
        };
    }
    return _bar;
}

- (void)setProblem:(NSString *)problem {
    _lastProblem = [problem copy];
    self.bar.hint = problem;
    self.bar.hintIsProblem = YES;
}

- (void)begin {
    MCAgentTaskBar *bar = self.bar;
    _barShown = YES;
    _lastProblem = nil;
    [_host agentTaskBarDidChange];
    [_host.window makeFirstResponder:bar.textView];
    if (_busy) return;
    const std::string command = [AppSettings shared].settings.agentCommand();
    if (!MCAgentProgramFound(command)) {
        _repositoryChecked = YES;
        [self setProblem:MCNotInstalledText(command)];
        return;
    }
    bar.hint = @"Looking at the repository…";
    bar.hintIsProblem = NO;
    _repositoryChecked = NO;
    NSString *root = [_host.rootPath copy];
    const NSUInteger gen = ++_lookGeneration;
    __weak MCAgentTaskSession *weakSelf = self;
    dispatch_async(_queue, ^{
        const TaskRepoLook look = root.length ? MCLookAtRepository(root) : TaskRepoLook{
            "Open a folder first: a task's worktree is made from the folder's repository.", "", 0};
        dispatch_async(dispatch_get_main_queue(), ^{
            MCAgentTaskSession *s = weakSelf;
            if (!s || gen != s->_lookGeneration || s->_busy) return;
            s->_repositoryChecked = YES;
            if (!look.problem.empty()) {
                [s setProblem:TNS(look.problem)];
            } else {
                s.bar.hint = TNS(AgentTask::startsFromText(look.branch, look.changed));
                s.bar.hintIsProblem = NO;
            }
        });
    });
}

- (void)close {
    if (!_barShown) return;
    _barShown = NO;
    _lookGeneration++;
    NSWindow *w = _host.window;
    NSResponder *fr = w.firstResponder;
    const BOOL hadKeyboard = [fr isKindOfClass:NSView.class] && [(NSView *)fr isDescendantOf:_bar];
    [_host agentTaskBarDidChange];
    if (hadKeyboard) [w makeFirstResponder:nil];
}

- (NSRect)layoutIn:(NSRect)area {
    if (!_bar) return area;
    if (!_barShown) {
        _bar.hidden = YES;
        return area;
    }
    _bar.hidden = NO;
    const CGFloat h = MIN([_bar heightForWidth:NSWidth(area)], NSHeight(area));
    _bar.frame = NSMakeRect(NSMinX(area), NSMaxY(area) - h, NSWidth(area), h);
    return NSMakeRect(NSMinX(area), NSMinY(area), NSWidth(area), NSHeight(area) - h);
}

- (void)finish {
    _busy = NO;
    _bar.startButton.enabled = YES;
    if (self.onFinished) self.onFinished();
}

- (void)start {
    if (_busy) return;
    MCAgentTaskBar *bar = self.bar;
    const std::string prompt = AgentTask::trimPrompt(TStd(bar.textView.string));
    _lastProblem = nil;
    if (prompt.empty()) {
        NSBeep();
        [self setProblem:@"Type what Claude Code should do first."];
        [self finish];
        return;
    }
    const std::string command = [AppSettings shared].settings.agentCommand();
    if (!MCAgentProgramFound(command)) {
        [self setProblem:MCNotInstalledText(command)];
        [self finish];
        return;
    }
    NSString *root = [_host.rootPath copy];
    if (!root.length) {
        [self setProblem:@"Open a folder first: a task's worktree is made from the folder's repository."];
        [self finish];
        return;
    }
    if (AgentTask::insideClaudeHome(AgentTask::worktreePath(TStd(root), "x"), TStd(NSHomeDirectory()))) {
        [self setProblem:@"A task's worktree would go in ~/.claude, Claude Code's own folder, which "
                         @"minicode never writes. Open the project's folder instead."];
        [self finish];
        return;
    }
    std::string base = AgentTask::sanitizeName(TStd(bar.nameField.stringValue));
    if (base.empty()) base = AgentTask::nameFromPrompt(prompt);
    _busy = YES;
    _lookGeneration++;
    bar.startButton.enabled = NO;
    bar.hint = @"Making the worktree…";
    bar.hintIsProblem = NO;
    __weak MCAgentTaskSession *weakSelf = self;
    dispatch_async(_queue, ^{
        std::string problem, name, path;
        const TaskRepoLook look = MCLookAtRepository(root);
        if (!look.problem.empty()) {
            problem = look.problem;
        } else {
            // A name no branch and no folder has yet.
            std::set<std::string> branches;
            MCGitResult *refs = MCGitRunSync(root, @[@"for-each-ref", @"--format=%(refname:short)",
                                                     @"refs/heads"]);
            for (NSString *l in [GitText(refs) componentsSeparatedByString:@"\n"])
                if (l.length) branches.insert(TStd(l));
            NSFileManager *fm = [NSFileManager new];
            const std::string rootStd = TStd(root);
            name = AgentTask::uniqueName(base, [&](const std::string &n) {
                return branches.count(n) > 0 ||
                       [fm fileExistsAtPath:TNS(AgentTask::worktreePath(rootStd, n))];
            });
            path = AgentTask::worktreePath(rootStd, name);
            NSString *dir = TNS(path).stringByDeletingLastPathComponent;
            [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
            NSMutableArray<NSString *> *args = [NSMutableArray array];
            for (const std::string &a : AgentTask::worktreeAddArgs(name, path)) [args addObject:TNS(a)];
            MCGitResult *add = MCGitRunSyncWithTimeout(root, args, 300);
            if (!add.ok) {
                problem = "git could not make the worktree: " +
                          TStd(add.errorText.length ? add.errorText : @"it stopped with an error.");
            } else {
                // Keep the worktrees out of the main checkout's changes:
                // a local exclude (never .gitignore, which is shared).
                MCGitResult *ign = MCGitRunSync(root, @[@"check-ignore", @"-q", TNS(path)]);
                MCGitResult *top = MCGitRunSync(root, @[@"rev-parse", @"--show-toplevel"]);
                MCGitResult *common = MCGitRunSync(root, @[@"rev-parse", @"--git-common-dir"]);
                NSString *topPath = GitText(top), *commonDir = GitText(common);
                if (ign.status == 1 && topPath.length && commonDir.length) {
                    if (!commonDir.absolutePath) commonDir = [root stringByAppendingPathComponent:commonDir];
                    NSString *realRoot = root.stringByResolvingSymlinksInPath;
                    NSString *realTop = topPath.stringByResolvingSymlinksInPath;
                    NSString *rel = [realRoot isEqualToString:realTop] ? @""
                        : [realRoot hasPrefix:[realTop stringByAppendingString:@"/"]]
                            ? [realRoot substringFromIndex:realTop.length + 1] : nil;
                    if (rel) {
                        NSString *pattern = [NSString stringWithFormat:@"/%@%@.claude/worktrees/",
                                             rel, rel.length ? @"/" : @""];
                        NSString *exclude = [commonDir stringByAppendingPathComponent:@"info/exclude"];
                        NSString *now = [NSString stringWithContentsOfFile:exclude
                                                                  encoding:NSUTF8StringEncoding
                                                                     error:nil] ?: @"";
                        if (![[now componentsSeparatedByString:@"\n"] containsObject:pattern]) {
                            [fm createDirectoryAtPath:exclude.stringByDeletingLastPathComponent
                          withIntermediateDirectories:YES attributes:nil error:nil];
                            NSString *add2 = [NSString stringWithFormat:@"%@%@# minicode's agent tasks\n%@\n",
                                              now, (now.length && ![now hasSuffix:@"\n"]) ? @"\n" : @"",
                                              pattern];
                            [add2 writeToFile:exclude atomically:YES encoding:NSUTF8StringEncoding error:nil];
                        }
                    }
                }
            }
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            MCAgentTaskSession *s = weakSelf;
            if (!s) return;
            if (!problem.empty()) {
                [s setProblem:TNS(problem)];
                [s finish];
                return;
            }
            [s launchName:TNS(name) path:TNS(path) prompt:prompt command:command];
        });
    });
}

- (void)launchName:(NSString *)name path:(NSString *)path prompt:(const std::string &)prompt
           command:(const std::string &)command {
    NSString *file = @"";
    if (!AgentTask::promptFitsOnLine(prompt)) {
        // A long prompt is read from a file, as Request a Change's is.
        NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"MiniCode-task"];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                                   attributes:@{NSFilePosixPermissions: @0700}
                                                        error:nil];
        MCRemoveStalePromptFiles(dir);
        static NSUInteger serial;
        file = [dir stringByAppendingPathComponent:
            [NSString stringWithFormat:@"task-%d-%lu.txt", getpid(), (unsigned long)++serial]];
        if (![TNS(prompt) writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:nil]) {
            [self setProblem:[NSString stringWithFormat:
                @"The worktree is made (%@), but the prompt could not be written to a file in the "
                @"temporary folder.", name]];
            [self finish];
            return;
        }
    }
    NSString *line = TNS(AgentTask::startCommand(command, TStd(path), prompt, TStd(file)));
    MCAgentTask *task = [MCAgentTask new];
    task.name = name;
    task.path = path;
    task.prompt = TNS(prompt);
    task.started = [NSDate date].timeIntervalSince1970;
    _lastTask = task;
    [_bar clear];
    [self close];
    task.terminal = [_host changeTerminalRunning:line focus:YES];
    [_model addTask:task];
    [_host showStatusNote:[NSString stringWithFormat:
        @"Claude Code is starting on %@ in .claude/worktrees/%@; the Agents panel lists it", name, name]];
    [self finish];
}
@end
