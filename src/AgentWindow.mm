// AgentWindow.mm — see AgentWindow.h.
#import "AgentWindow.h"
#import "AgentModel.h"
#import "AgentPages.h"
#import "AgentsPanel.h"
#import "CompareView.h"
#import "ControlServer.h"
#import "LspPicker.h"
#import "Terminal.h"
#import "TreeDetails.h"

#include "AgentView.h"

static std::string WU8(NSString *s) { return s.UTF8String ? std::string(s.UTF8String) : ""; }
static NSString *WNS(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size()
                                  encoding:NSUTF8StringEncoding] ?: @"";
}

@implementation MCAgentWindow {
    __weak id<MCAgentWorkHost> _host;
    NSString *_root;
    id _worktreeObserver;
    id _keyMonitor, _mouseMonitor;
    NSArray<MCTermLinkMatch *> *_pickerMatches;
    MCTermLink *_pickerLink;
    void (^_pickerOpen)(MCTermLink *);
}
@synthesize model = _model, panel = _panel;

- (instancetype)initWithHost:(id<MCAgentWorkHost>)host {
    if ((self = [super init])) {
        _host = host;
        _root = [host.rootPath copy];
    }
    return self;
}

- (void)dealloc {
    [self stop];
}

- (BOOL)started { return _model != nil; }

- (MCAgentModel *)model {
    if (!_model) {
        _model = [[MCAgentModel alloc] initWithFolder:_root];
        __weak MCAgentWindow *weakSelf = self;
        _worktreeObserver = [NSNotificationCenter.defaultCenter
            addObserverForName:MCAgentModelWorktreesDidChangeNotification object:_model queue:nil
                    usingBlock:^(NSNotification *n) {
            (void)n;
            MCAgentWindow *s = weakSelf;
            [s->_host agentWorktreesChanged];
        }];
    }
    return _model;
}

- (MCAgentsPanel *)panel {
    if (!_panel) {
        _panel = [[MCAgentsPanel alloc] initWithModel:self.model];
        __weak MCAgentWindow *weakSelf = self;
        _panel.onShowEvidence = ^(NSString *transcript, NSString *name) {
            [weakSelf showEvidenceForTranscript:transcript name:name];
        };
        _panel.onOpenFile = ^(NSString *path) {
            MCAgentWindow *s = weakSelf;
            MCTermLink *link = [MCTermLink new];
            link.target = path;
            [s->_host openTerminalLink:link];
        };
        _panel.onOpenFolderInNewWindow = ^(NSString *path) { [weakSelf openFolderInNewWindow:path]; };
        _panel.onTab = ^{
            MCAgentWindow *s = weakSelf;
            [s->_host focusEditor:nil];
        };
    }
    return _panel;
}

- (void)setRoot:(NSString *)root {
    _root = [root copy];
    if (_model) _model.folder = root;
}

- (void)windowBecameKey {
    if (!_model) return;
    [_model refresh];
    [_model refreshWorktrees];
}

- (void)folderChangedOnDisk:(NSString *)path {
    if (!_model || !_root.length || !_host.agentWorktreesOnShow) return;
    // Paths come as the disk spells them (/private/tmp); compare by suffix.
    if ([path rangeOfString:@"/.claude/worktrees"].location != NSNotFound ||
        [path rangeOfString:@"/.git"].location != NSNotFound)
        [_model refreshWorktrees];
}

- (void)stop {
    [_model stop];
    if (_worktreeObserver) [NSNotificationCenter.defaultCenter removeObserver:_worktreeObserver];
    _worktreeObserver = nil;
    [self closePicker];
}

// ------------------------------------------------------------- actions
- (void)showEvidenceForTranscript:(NSString *)transcript name:(NSString *)name {
    if (!transcript.length) return;
    MCEvidencePage *page = [[MCEvidencePage alloc] initWithTranscript:transcript name:name
                                                                model:self.model];
    __weak MCAgentWindow *weakSelf = self;
    page.onOpenInTerminal = ^(NSString *command, NSString *cwd) {
        [weakSelf typeCommand:command inFolder:cwd];
    };
    [_host showPage:page];
}

// R3.5: a new terminal at the session's folder with the command typed and
// not run. A folder that is gone (a worktree removed since) falls back to
// the window's.
- (void)typeCommand:(NSString *)command inFolder:(NSString *)cwd {
    BOOL isDir = NO;
    NSString *dir = cwd;
    if (!dir.length || ![NSFileManager.defaultManager fileExistsAtPath:dir isDirectory:&isDir] || !isDir) {
        if (cwd.length)
            [self note:[NSString stringWithFormat:@"%@ is gone; the terminal starts in the folder",
                        [cwd stringByAbbreviatingWithTildeInPath]]];
        dir = _root;
    }
    TerminalView *t = [_host newTerminalForAgentIn:dir beside:NO];
    if (!t) return;
    // Long commands keep their lines; typeCommand: never presses Return.
    [t typeCommand:command];
    [t focusInput];
}

- (MCAgentWorktree *)worktreeAtPath:(NSString *)path {
    return [self.model worktreeAtPath:path];
}

- (void)compareWorktreeAt:(NSString *)path {
    MCAgentWorktree *w = [self.model worktreeAtPath:path];
    if (!w) {
        [self note:@"git has not listed that folder as a worktree yet"];
        [self.model refreshWorktrees];
        return;
    }
    MCWorktreeChangesPage *page = [[MCWorktreeChangesPage alloc] initWithWorktree:w model:self.model];
    __weak MCAgentWindow *weakSelf = self;
    page.onShowComparison = ^(MCComparison *c) {
        MCAgentWindow *s = weakSelf;
        [s->_host showComparison:c preview:YES];
    };
    [_host showPage:page];
}

- (void)openFolderInNewWindow:(NSString *)path {
    id delegate = NSApp.delegate;
    if (path.length && [delegate respondsToSelector:@selector(controlOpenWindowAtPath:)])
        [(id<MCControlAppHost>)delegate controlOpenWindowAtPath:path];
}

- (void)note:(NSString *)text {
    _lastNote = [text copy];
    [_host showStatusNote:text];
}

// ------------------------------------------------------------- terminal links
- (NSArray<NSString *> *)linkRoots {
    return [self.model worktreeRoots];
}

- (BOOL)handleLink:(MCTermLink *)link open:(void (^)(MCTermLink *))open {
    if (link.isURL || link.matches.count == 1 || !link.matches) return NO;
    if (link.matches.count == 0) {
        std::vector<std::string> tried;
        for (NSString *t in link.tried) tried.push_back(WU8(t));
        [self note:WNS(AgentView::notFoundText(WU8(link.target), tried, WU8(NSHomeDirectory())))];
        return YES;
    }
    [self showPickerFor:link open:open];
    return YES;
}

- (void)showPickerFor:(MCTermLink *)link open:(void (^)(MCTermLink *))open {
    [self closePicker];
    if (!_picker) _picker = [MCPickerPopup new];
    _pickerMatches = link.matches;
    _pickerLink = link;
    _pickerOpen = [open copy];
    NSDate *now = [NSDate date];
    NSMutableArray<NSAttributedString *> *rows = [NSMutableArray array];
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    // Each row: when it was modified, then its whole path, which gives way in
    // its middle so the file's name stays in view.
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineBreakMode = NSLineBreakByTruncatingMiddle;
    for (MCTermLinkMatch *m in link.matches) {
        NSDate *d = [NSDate dateWithTimeIntervalSince1970:m.modified];
        NSString *age = MCCompactAge(d, now);
        NSMutableAttributedString *s = [[NSMutableAttributedString alloc]
            initWithString:[NSString stringWithFormat:@"modified %@   ", age.length ? age : MCFullDate(d)]
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11],
                             NSForegroundColorAttributeName: [NSColor colorWithWhite:0.6 alpha:1],
                             NSParagraphStyleAttributeName: ps}];
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:[m.path stringByAbbreviatingWithTildeInPath]
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:12.5],
                             NSForegroundColorAttributeName: [NSColor colorWithWhite:0.86 alpha:1],
                             NSParagraphStyleAttributeName: ps}]];
        [rows addObject:s];
        [paths addObject:m.path];
    }
    _pickerPaths = paths;
    __weak MCAgentWindow *weakSelf = self;
    _picker.onAccept = ^(NSInteger row) { [weakSelf acceptPickerRow:row]; };
    NSPoint at = [NSEvent mouseLocation];
    NSWindow *parent = _host.window;
    [_picker showRows:rows headers:[NSIndexSet indexSet] select:0 width:640 maxRows:10
         atScreenRect:NSMakeRect(at.x, at.y - 8, 1, 16) parent:parent];
    // The picker never takes the keyboard: Up, Down, Return and Esc reach it
    // through this monitor while it shows; any other key or a click elsewhere
    // closes it.
    _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                        handler:^NSEvent *(NSEvent *e) {
        MCAgentWindow *s = weakSelf;
        if (!s || !s->_picker.visible) return e;
        NSString *ch = e.charactersIgnoringModifiers;
        unichar c = ch.length ? [ch characterAtIndex:0] : 0;
        if (c == NSUpArrowFunctionKey) { [s->_picker moveSelection:-1]; return nil; }
        if (c == NSDownArrowFunctionKey) { [s->_picker moveSelection:1]; return nil; }
        if (c == '\r' || c == 3) { [s acceptPickerRow:s->_picker.selectedRow]; return nil; }
        if (c == 27) { [s closePicker]; return nil; }
        [s closePicker];
        return e;
    }];
    _mouseMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown |
                                                                  NSEventMaskRightMouseDown
                                                          handler:^NSEvent *(NSEvent *e) {
        MCAgentWindow *s = weakSelf;
        if (s && s->_picker.visible && e.window != s->_picker.contentView.window) [s closePicker];
        return e;
    }];
}

- (void)acceptPickerRow:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_pickerMatches.count) return;
    MCTermLinkMatch *m = _pickerMatches[(NSUInteger)row];
    MCTermLink *chosen = [MCTermLink new];
    chosen.target = m.path;
    chosen.isDirectory = m.isDirectory;
    chosen.line = _pickerLink.line;
    chosen.column = _pickerLink.column;
    chosen.matches = @[m];
    void (^open)(MCTermLink *) = _pickerOpen;
    [self closePicker];
    if (open) open(chosen);
}

- (void)closePicker {
    if (_keyMonitor) [NSEvent removeMonitor:_keyMonitor];
    if (_mouseMonitor) [NSEvent removeMonitor:_mouseMonitor];
    _keyMonitor = _mouseMonitor = nil;
    if (_picker.visible) [_picker close];
    _pickerOpen = nil;
}

@end
