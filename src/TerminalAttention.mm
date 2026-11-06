// TerminalAttention.mm — see TerminalAttention.h.
#import "TerminalAttention.h"
#import "AgentWindow.h"
#import "ControlServer.h"
#import "EditorController.h"
#import "Terminal.h"
#include "AttentionBoard.h"
#include "ControlProtocol.h"

NSNotificationName const MCTerminalAttentionDidChangeNotification =
    @"MCTerminalAttentionDidChangeNotification";

static NSString *NSFrom(const std::string &s) {
    return s.empty() ? nil : [NSString stringWithUTF8String:s.c_str()];
}

static std::string U8(NSString *s) { return s.UTF8String ?: ""; }

@implementation MCTerminalAttention {
    Attention::Board _board;
    // Every terminal the board has heard of, by id; weak, so a terminal that
    // went without closing (its window freed) is simply not found.
    NSMapTable<NSNumber *, TerminalView *> *_terminals;
}

+ (instancetype)shared {
    static MCTerminalAttention *shared;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ shared = [MCTerminalAttention new]; });
    return shared;
}

- (instancetype)init {
    if ((self = [super init])) {
        _terminals = [NSMapTable strongToWeakObjectsMapTable];
        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
        [nc addObserver:self selector:@selector(terminalTyped:)
                   name:MCTerminalUserDidTypeNotification object:nil];
        [nc addObserver:self selector:@selector(terminalClosed:)
                   name:MCTerminalDidCloseNotification object:nil];
    }
    return self;
}

- (void)changed:(TerminalView *)terminal {
    // The Dock icon counts the waiting terminals of every window.
    const int n = _board.waitingCount();
    NSString *badge = n > 0 ? [NSString stringWithFormat:@"%d", n] : nil;
    NSDockTile *tile = NSApp.dockTile;
    if (!(badge == tile.badgeLabel || [badge isEqualToString:tile.badgeLabel]))
        tile.badgeLabel = badge;
    [[NSNotificationCenter defaultCenter]
        postNotificationName:MCTerminalAttentionDidChangeNotification object:terminal];
}

- (BOOL)applyNotify:(const Json &)args toTerminal:(TerminalView *)terminal
              error:(NSString **)error {
    const ClaudeHooks::Action action = ClaudeHooks::actionNamed(args["state"].asString());
    if (action == ClaudeHooks::Action::None) {
        if (error) *error = @"state is waiting, done or clear";
        return NO;
    }
    if (!terminal) return NO;
    [_terminals setObject:terminal forKey:@(terminal.terminalID)];
    Attention::Session s;
    s.sessionId = args["session_id"].asString();
    s.transcriptPath = args["transcript_path"].asString();
    s.cwd = args["cwd"].asString();
    // Whoever sent it, a message is shown as a note's text is: one line.
    const std::string message = ControlChannel::cleanNoteText(args["message"].asString());
    if (_board.apply((int)terminal.terminalID, action, message, s,
                     [NSDate timeIntervalSinceReferenceDate]))
        [self changed:terminal];
    return YES;
}

- (void)terminalTyped:(NSNotification *)note {
    TerminalView *t = note.object;
    if (_board.typed((int)t.terminalID)) [self changed:t];
}

- (void)terminalClosed:(NSNotification *)note {
    TerminalView *t = note.object;
    [_terminals removeObjectForKey:@(t.terminalID)];
    if (_board.forget((int)t.terminalID)) [self changed:nil];
}

- (MCAttentionMark)markForTerminal:(TerminalView *)terminal {
    if (!terminal) return MCAttentionNone;
    switch (_board.mark((int)terminal.terminalID)) {
    case Attention::Mark::Waiting: return MCAttentionWaiting;
    case Attention::Mark::Done: return MCAttentionDone;
    case Attention::Mark::None: break;
    }
    return MCAttentionNone;
}

- (NSString *)markNameForTerminal:(TerminalView *)terminal {
    switch ([self markForTerminal:terminal]) {
    case MCAttentionWaiting: return @"waiting";
    case MCAttentionDone: return @"done";
    case MCAttentionNone: break;
    }
    return nil;
}

- (NSString *)messageForTerminal:(TerminalView *)terminal {
    return terminal ? NSFrom(_board.message((int)terminal.terminalID)) : nil;
}

- (NSInteger)waitingCount { return _board.waitingCount(); }

- (NSString *)sessionIdForTerminal:(TerminalView *)terminal {
    const Attention::Session *s = terminal ? _board.session((int)terminal.terminalID) : nullptr;
    return s ? NSFrom(s->sessionId) : nil;
}

- (NSString *)transcriptForTerminal:(TerminalView *)terminal {
    const Attention::Session *s = terminal ? _board.session((int)terminal.terminalID) : nullptr;
    return s ? NSFrom(s->transcriptPath) : nil;
}

- (NSString *)sessionCwdForTerminal:(TerminalView *)terminal {
    const Attention::Session *s = terminal ? _board.session((int)terminal.terminalID) : nullptr;
    return s ? NSFrom(s->cwd) : nil;
}

- (TerminalView *)terminalForSession:(NSString *)sessionId transcript:(NSString *)transcript {
    int id = _board.terminalFor(U8(sessionId), U8(transcript));
    return id ? [_terminals objectForKey:@(id)] : nil;
}

// The board's order: every terminal of every window.
- (TerminalView *)nextWaitingAfter:(TerminalView *)current in:(NSArray<TerminalView *> *)all {
    std::vector<int> order;
    for (TerminalView *t in all) order.push_back((int)t.terminalID);
    const int next = _board.nextWaiting(order, current ? (int)current.terminalID : 0);
    for (TerminalView *t in all)
        if (t.terminalID == next) return t;
    return nil;
}
@end

// ----------------------------------------------------------- moving there
static id<MCControlAppHost> AppHost(void) {
    id d = NSApp.delegate;
    return [d respondsToSelector:@selector(controlControllers)] ? d : nil;
}

TerminalView *MCNextWaitingTerminal(void) {
    id<MCControlAppHost> host = AppHost();
    NSMutableArray<TerminalView *> *all = [NSMutableArray array];
    TerminalView *current = nil;
    EditorController *front = [host controlFrontController];
    // The front window's terminals first, so "next" starts where the user is.
    NSMutableArray<EditorController *> *windows = [[host controlControllers] mutableCopy];
    if (front) {
        [windows removeObject:front];
        [windows insertObject:front atIndex:0];
    }
    for (EditorController *c in windows)
        for (TerminalView *t in [c controlTerminals]) {
            [all addObject:t];
            if (c == front && [t hasFocus]) current = t;
        }
    return [[MCTerminalAttention shared] nextWaitingAfter:current in:all];
}

BOOL MCRevealTerminal(TerminalView *terminal) {
    if (!terminal) return NO;
    for (EditorController *c in [AppHost() controlControllers])
        if ([[c controlTerminals] containsObject:terminal]) {
            [c.window makeKeyAndOrderFront:nil];
            return [c revealTerminal:terminal];
        }
    return NO;
}

void MCJumpToNextWaitingTerminal(void) {
    if (MCRevealTerminal(MCNextWaitingTerminal())) return;
    NSBeep();
    id<MCAgentWorkHost> front = (id<MCAgentWorkHost>)[AppHost() controlFrontController];
    if ([front respondsToSelector:@selector(showStatusNote:)])
        [front showStatusNote:@"No terminal is waiting for you"];
}
