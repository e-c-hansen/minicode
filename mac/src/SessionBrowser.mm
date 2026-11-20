// SessionBrowser.mm — see SessionBrowser.h.
#import "SessionBrowser.h"

#import "AppSettings.h"
#import "EditorTab.h"
#import "FileTree.h"   // gShowHidden
#import "GitPanel.h"   // MCGitRunSync
#import "Latex.h"
#import "SessionLink.h"
#import "SessionWindow.h"
#import "TreeDetails.h"   // MCAppDefaults

#include <arpa/inet.h>
#include <libproc.h>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/proc_info.h>
#include <thread>
#include <vector>

#include "Json.h"
#include "SessionWeb.h"

NSString *const MCSessionBrowserTermNote = @"Terminals opened while the session host is on appear here";
void (^MCSessionBrowserOpener)(NSURL *url) = nil;
void (^MCSessionBrowserAskDelay)(dispatch_block_t ask) = nil;

// ---- remembered browsers (Always Allow), in the app's defaults
static NSString *const kRememberedKey = @"MCSessionBrowsersRemembered";

NSArray<NSDictionary *> *MCSessionBrowsersRemembered(void) {
    NSMutableArray *out = [NSMutableArray array];
    for (id e in [MCAppDefaults() arrayForKey:kRememberedKey] ?: @[])
        if ([e isKindOfClass:NSDictionary.class] && [e[@"id"] isKindOfClass:NSString.class]) [out addObject:e];
    return out;
}

static BOOL MCSessionBrowserIsRemembered(NSString *browserId) {
    for (NSDictionary *e in MCSessionBrowsersRemembered())
        if ([e[@"id"] isEqualToString:browserId]) return YES;
    return NO;
}

static void MCSessionBrowserRemember(NSString *browserId, NSString *browser, NSString *address) {
    NSMutableArray *list = [NSMutableArray array];
    for (NSDictionary *e in MCSessionBrowsersRemembered())
        if (![e[@"id"] isEqualToString:browserId]) [list addObject:e];
    [list addObject:@{@"id": browserId, @"browser": browser ?: @"", @"address": address ?: @"",
                      @"since": [NSDate date]}];
    [MCAppDefaults() setObject:list forKey:kRememberedKey];
}

void MCSessionBrowsersForget(void) { [MCAppDefaults() removeObjectForKey:kRememberedKey]; }

void MCSessionBrowsersShowRemembered(void) {
    NSArray<NSDictionary *> *list = MCSessionBrowsersRemembered();
    NSAlert *a = [NSAlert new];
    if (!list.count) {
        a.messageText = @"No browsers are remembered";
        a.informativeText = @"When mutible asks whether a browser may open a window's session in the "
                            @"browser, Always Allow remembers it, and it is listed here.";
        [a addButtonWithTitle:@"OK"];
        [a runModal];
        return;
    }
    NSDateFormatter *f = [NSDateFormatter new];
    f.dateStyle = NSDateFormatterMediumStyle;
    f.timeStyle = NSDateFormatterShortStyle;
    NSMutableArray *lines = [NSMutableArray array];
    for (NSDictionary *e in list) {
        NSString *when = [e[@"since"] isKindOfClass:NSDate.class] ? [f stringFromDate:e[@"since"]] : @"";
        NSString *browser = [e[@"browser"] length] ? e[@"browser"] : @"A browser";
        NSString *line = [e[@"address"] length]
                             ? [NSString stringWithFormat:@"%@, from %@", browser, e[@"address"]]
                             : browser;
        if (when.length) line = [line stringByAppendingFormat:@", since %@", when];
        [lines addObject:line];
    }
    a.messageText = list.count == 1 ? @"One browser is remembered"
                                    : [NSString stringWithFormat:@"%lu browsers are remembered",
                                                                 (unsigned long)list.count];
    a.informativeText = [[lines componentsJoinedByString:@"\n"]
        stringByAppendingString:@"\n\nA remembered browser opens a link to a window's session "
                                @"without being asked about. Forget All asks about each again."];
    [a addButtonWithTitle:@"Done"];
    [a addButtonWithTitle:@"Forget All"];
    if ([a runModal] == NSAlertSecondButtonReturn) MCSessionBrowsersForget();
}

// ---- where a connection comes from
// The user's own processes' TCP sockets: which holds the other end of the
// connection from `peerPort` to `serverPort`, and, when that is sshd's
// forwarding, the address its SSH connection comes from.
NSString *MCSessionBrowserPeer(int peerPort, int serverPort) {
    int bytes = proc_listallpids(nullptr, 0);
    if (bytes <= 0) return @"this Mac";
    std::vector<pid_t> pids((size_t)bytes + 64);
    const int count = proc_listallpids(pids.data(), (int)(pids.size() * sizeof(pid_t)));
    auto sockets = [](pid_t pid, std::vector<struct socket_fdinfo> &out) {
        out.clear();
        int n = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
        if (n <= 0) return;
        std::vector<struct proc_fdinfo> fds((size_t)n / sizeof(struct proc_fdinfo) + 16);
        n = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(), (int)(fds.size() * sizeof(struct proc_fdinfo)));
        for (size_t i = 0; n > 0 && i < (size_t)n / sizeof(struct proc_fdinfo); i++) {
            if (fds[i].proc_fdtype != PROX_FDTYPE_SOCKET) continue;
            struct socket_fdinfo si;
            if (proc_pidfdinfo(pid, fds[i].proc_fd, PROC_PIDFDSOCKETINFO, &si, sizeof si) == (int)sizeof si &&
                si.psi.soi_kind == SOCKINFO_TCP)
                out.push_back(si);
        }
    };
    std::vector<struct socket_fdinfo> socks;
    for (int i = 0; i < count && pids[(size_t)i] > 0; i++) {
        const pid_t pid = pids[(size_t)i];
        if (pid == getpid()) continue;
        sockets(pid, socks);
        bool holds = false;
        for (const auto &si : socks) {
            const struct in_sockinfo &in = si.psi.soi_proto.pri_tcp.tcpsi_ini;
            holds = holds || (ntohs((uint16_t)in.insi_lport) == peerPort &&
                              ntohs((uint16_t)in.insi_fport) == serverPort);
        }
        if (!holds) continue;
        char name[2 * MAXCOMLEN + 1] = "";
        proc_name(pid, name, sizeof name);
        if (std::string(name).compare(0, 4, "sshd") != 0) return @"this Mac";
        // sshd forwarding a tunnel: its own connection's far end.
        for (const auto &si : socks) {
            const struct tcp_sockinfo &tcp = si.psi.soi_proto.pri_tcp;
            if (tcp.tcpsi_state != TSI_S_ESTABLISHED) continue;
            const struct in_sockinfo &in = tcp.tcpsi_ini;
            char addr[INET6_ADDRSTRLEN] = "";
            if (in.insi_vflag & INI_IPV4) {
                struct in_addr fa = in.insi_faddr.ina_46.i46a_addr4;
                if (ntohl(fa.s_addr) >> 24 == 127) continue;
                inet_ntop(AF_INET, &fa, addr, sizeof addr);
            } else if (in.insi_vflag & INI_IPV6) {
                struct in6_addr fa = in.insi_faddr.ina_6;
                if (IN6_IS_ADDR_LOOPBACK(&fa)) continue;
                inet_ntop(AF_INET6, &fa, addr, sizeof addr);
            }
            if (addr[0]) return [NSString stringWithFormat:@"%s, through an SSH tunnel", addr];
        }
        return @"another machine, through an SSH tunnel";
    }
    return @"this Mac";
}

@implementation MCSessionBrowser {
    __weak NSWindow *_window;
    MCSessionWindow *_session;
    std::unique_ptr<Shared::Web::Server> _server;
    std::thread _thread;
    NSTimer *_timer;
    id _closeObserver;
    id _settingsObserver;
    NSUInteger _generation;   // one per server started: an answer reaches only the one that asked
    NSString *_url;
    // Previews of the server's own, for LaTeX documents no tab previews,
    // by path, the oldest used first in the order.
    NSMutableDictionary<NSString *, LatexView *> *_typesetViews;
    NSMutableArray<NSString *> *_typesetOrder;
}

- (instancetype)initWithWindow:(NSWindow *)window session:(MCSessionWindow *)session {
    if ((self = [super init])) {
        _window = window;
        _session = session;
        __weak MCSessionBrowser *weakSelf = self;
        _closeObserver = [NSNotificationCenter.defaultCenter
            addObserverForName:NSWindowWillCloseNotification
                        object:window
                         queue:nil
                    usingBlock:^(NSNotification *) { [weakSelf stop]; }];
        // The page takes the settings' colors and sizes: a change recolors
        // it at once (the server also looks at the file every second).
        _settingsObserver = [NSNotificationCenter.defaultCenter
            addObserverForName:MCSettingsDidChangeNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification *) {
                        MCSessionBrowser *s = weakSelf;
                        if (s && s->_server) s->_server->reloadSettings();
                    }];
    }
    return self;
}

- (void)dealloc {
    [self stop];
    if (_closeObserver) [NSNotificationCenter.defaultCenter removeObserver:_closeObserver];
    if (_settingsObserver) [NSNotificationCenter.defaultCenter removeObserver:_settingsObserver];
}

- (uint64_t)windowId { return _session.windowId; }

- (void)setEditable:(BOOL)editable {
    if (editable == _editable) return;
    [self stop];   // a server serves one way: the next start serves the other
    _editable = editable;
}
- (int)port { return _server ? _server->port() : 0; }
- (int)browsers { return _server ? _server->browsers() : 0; }
- (NSUInteger)asking { return _server ? _server->access().questions() : 0; }

- (NSString *)url { return _server ? _url : nil; }

- (NSString *)newLink {
    if (!_server) return nil;
    _url = [NSString stringWithUTF8String:_server->newLink().c_str()];
    return _url;
}

// A browser this Mac does not remember is opening the page: a sheet on the
// window asks, and the answer goes to the server that asked.
- (void)askAbout:(uint64_t)question
         browser:(NSString *)browser
       browserId:(NSString *)browserId
            from:(NSString *)from
            peer:(NSString *)peer
      generation:(NSUInteger)generation {
    if (generation != _generation || !_server) return;
    NSWindow *window = _window;
    if (!window) {
        _server->answer(question, false);
        return;
    }
    NSString *where = [from isEqualToString:@"this Mac"] ? [NSString stringWithFormat:@"this Mac (%@)", peer] : from;
    NSAlert *a = [NSAlert new];
    a.messageText = [NSString stringWithFormat:@"Allow %@ to open this window's session?", browser];
    a.informativeText = [NSString
        stringWithFormat:@"A browser is opening the link to this window's session from %@. Allow lets "
                         @"it %@ this window's files and terminals; Deny turns it away. Always Allow "
                         @"also remembers this browser, so its later links open without asking "
                         @"(View > Remembered Browsers… forgets it).",
                         where, _editable ? @"see and edit" : @"see"];
    [a addButtonWithTitle:@"Allow"];
    NSButton *deny = [a addButtonWithTitle:@"Deny"];
    deny.keyEquivalent = @"\033";
    [a addButtonWithTitle:@"Always Allow"];
    __weak MCSessionBrowser *weakSelf = self;
    __weak NSWindow *weakWindow = window;
    dispatch_block_t show = ^{
        MCSessionBrowser *s = weakSelf;
        NSWindow *w = weakWindow;
        if (!s || generation != s->_generation || !s->_server) return;
        if (!w) {
            s->_server->answer(question, false);
            return;
        }
        [a beginSheetModalForWindow:w
                  completionHandler:^(NSModalResponse r) {
                      MCSessionBrowser *t = weakSelf;
                      const BOOL allow = r == NSAlertFirstButtonReturn || r == NSAlertThirdButtonReturn;
                      if (r == NSAlertThirdButtonReturn) MCSessionBrowserRemember(browserId, browser, from);
                      if (t && generation == t->_generation && t->_server) t->_server->answer(question, allow);
                  }];
        if (!NSApp.isActive) [NSApp requestUserAttention:NSInformationalRequest];
    };
    if (MCSessionBrowserAskDelay) MCSessionBrowserAskDelay(show);
    else show();
}

- (BOOL)start:(NSString **)error {
    if (_server) return YES;
    // The tabs first, so the first browser sees them.
    [self reportTabs];
    auto server = std::make_unique<Shared::Web::Server>();
    Shared::Web::Options o;
    // The window's host: the process's, or its folder's headless one over
    // its socket.
    std::shared_ptr<MCSession::Link> link = _session.link.link;
    if (link->kind() == MCSession::Link::Kind::Socket) o.hostSocket = link->socketPath();
    else o.connect = [](Shared::Client &c) { return MCSessionConnectViewer(c); };
    o.editable = _editable;
    if (_editable && _save) {
        void (^save)(uint64_t) = _save;
        o.save = [save](Shared::DocId d) {
            dispatch_async(dispatch_get_main_queue(), ^{ save(d); });
            return true;
        };
    }
    if (_editable && _open) {   // a read-only page changes nothing in the window
        void (^open)(NSString *, BOOL) = _open;
        o.open = [open](const std::string &path, bool keep) {
            NSString *p = [NSString stringWithUTF8String:path.c_str()];
            if (!p) return false;
            dispatch_async(dispatch_get_main_queue(), ^{ open(p, keep); });
            return true;
        };
    }
    if (_editable && _close) {
        void (^close)(uint64_t) = _close;
        o.close = [close](Shared::DocId d) {
            dispatch_async(dispatch_get_main_queue(), ^{ close(d); });
            return true;
        };
    }
    if (_editable && _browse) {
        void (^browse)(NSString *) = _browse;
        o.browse = [browse](const std::string &url) {
            NSString *u = [NSString stringWithUTF8String:url.c_str()];
            if (!u) return false;
            dispatch_async(dispatch_get_main_queue(), ^{ browse(u); });
            return true;
        };
    }
    // A page showing a LaTeX document: the window says what it has
    // (any page: a typeset reads a copy and changes nothing of the user's).
    if (_typeset) {
        void (^typeset)(NSString *) = _typeset;
        o.typeset = [typeset](const std::string &path) {
            NSString *p = [NSFileManager.defaultManager stringWithFileSystemRepresentation:path.c_str()
                                                                                    length:path.size()];
            if (!p) return;
            dispatch_async(dispatch_get_main_queue(), ^{ typeset(p); });
        };
    }
    // The page's Source Control runs git as the app's own panel does: the
    // git it found, its environment and time limits, with the core's
    // arguments (on a thread of the server's).
    o.git = [](const std::string &dir, const std::vector<std::string> &args, const std::string &input,
               std::string *out, std::string *err, int *status) {
        NSFileManager *fm = NSFileManager.defaultManager;
        auto ns = [fm](const std::string &s) -> NSString * {
            return [NSString stringWithUTF8String:s.c_str()]
                       ?: [fm stringWithFileSystemRepresentation:s.c_str() length:s.size()];
        };
        NSMutableArray<NSString *> *a = [NSMutableArray arrayWithCapacity:args.size()];
        for (const std::string &s : args) {
            NSString *x = ns(s);
            if (!x) {
                *err = "an argument git could not be given";
                return false;
            }
            [a addObject:x];
        }
        NSString *d = ns(dir);
        if (!d) return false;
        MCGitResult *r = input.empty()
            ? MCGitRunSync(d, a)
            : MCGitRunSyncWithInput(d, a, [NSData dataWithBytes:input.data() length:input.size()]);
        if (r.output.length) out->assign((const char *)r.output.bytes, r.output.length);
        *err = r.errorText.UTF8String ?: "";
        *status = r.status;
        return (bool)r.ok;
    };
    // A browser this Mac does not remember waits for the window's sheet.
    const NSUInteger generation = ++_generation;
    __weak MCSessionBrowser *weakSelf = self;
    Shared::Web::Server *asking = server.get();
    void (^ask)(uint64_t, NSString *, NSString *, NSString *, NSString *) =
        ^(uint64_t q, NSString *browser, NSString *browserId, NSString *from, NSString *peer) {
            [weakSelf askAbout:q browser:browser browserId:browserId from:from peer:peer generation:generation];
        };
    o.ask = [ask, asking](const Shared::Web::Access::Question &q) {
        // Who holds the other end is looked up here, off the main thread.
        NSString *from = MCSessionBrowserPeer(q.peerPort, asking->port());
        NSString *browser = [NSString stringWithUTF8String:q.browser.c_str()] ?: @"A browser";
        NSString *browserId = [NSString stringWithUTF8String:q.browserId.c_str()] ?: @"";
        NSString *peer = [NSString stringWithUTF8String:q.peer.c_str()] ?: @"";
        const uint64_t id = q.id;
        dispatch_async(dispatch_get_main_queue(), ^{ ask(id, browser, browserId, from, peer); });
    };
    o.remembered = [](const std::string &browserId) {
        NSString *b = [NSString stringWithUTF8String:browserId.c_str()];
        return b && MCSessionBrowserIsRemembered(b);
    };
    o.showHidden = gShowHidden;
    o.folder = link->kind() == MCSession::Link::Kind::Socket ? link->folder()
                                                             : std::string(_session.folder.fileSystemRepresentation ?: "");
    o.window = _session.windowId;
    o.termNote = MCSessionBrowserTermNote.UTF8String;
    o.port = 0;   // any free port
    o.settingsPath = [AppSettings shared].path.fileSystemRepresentation ?: "";
    std::string why;
    if (!server->start(o, &why)) {
        if (error) *error = [NSString stringWithUTF8String:why.c_str()];
        return NO;
    }
    _server = std::move(server);
    Shared::Web::Server *raw = _server.get();
    _thread = std::thread([raw] { raw->run(); });
    _timer = [NSTimer timerWithTimeInterval:0.5
                                    repeats:YES
                                      block:^(NSTimer *) { [weakSelf reportTabs]; }];
    [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
    return YES;
}

// The window's record, sent when it changed; the view follows now, not at
// its next half-second look.
- (void)reportTabs {
    [_session report];
    [self lookNow];
}

- (void)gitChanged {
    if (_server) _server->gitChanged();
}

- (void)lookNow {
    if (_server) _server->lookNow();
}

// ---- the LaTeX preview's typesets (R3's third slice)

- (void)typesetOf:(NSString *)path pdf:(NSData *)pdf error:(NSString *)error {
    if (!_server || !path.length) return;
    const std::string p = path.fileSystemRepresentation;
    if (pdf) _server->typesetReady(p, std::make_shared<const std::string>((const char *)pdf.bytes, pdf.length));
    else if (error) _server->typesetFailed(p, error.UTF8String ?: "");
    else _server->typesetWorking(p);
}

- (void)typesetOf:(NSString *)path view:(LatexView *)view {
    NSData *pdf = view.typesetPdf;
    NSString *failure = view.typesetFailure;
    // The last PDF first, so a page has it to show while another runs.
    if (pdf) [self typesetOf:path pdf:pdf error:nil];
    if (view.typesetting || (!pdf && !failure)) [self typesetOf:path pdf:nil error:nil];
    else if (failure) [self typesetOf:path pdf:nil error:failure];
}

- (void)typesetInBackground:(NSString *)path source:(NSString *)source {
    if (!_server || !path.length) return;
    if (!_typesetViews) {
        _typesetViews = [NSMutableDictionary dictionary];
        _typesetOrder = [NSMutableArray array];
    }
    LatexView *v = _typesetViews[path];
    [_typesetOrder removeObject:path];
    if (!v) {
        while (_typesetOrder.count >= 4) {
            NSString *old = _typesetOrder.firstObject;
            [_typesetViews[old] stopPreview];
            [_typesetViews removeObjectForKey:old];
            [_typesetOrder removeObjectAtIndex:0];
        }
        // Typesets at once, as a tab's preview does when it is made.
        v = [[LatexView alloc] initWithPath:path source:source ?: @""];
        __weak MCSessionBrowser *weakSelf = self;
        v.onTypeset = ^(NSString *p, NSData *pdf, NSString *error) {
            [weakSelf typesetOf:p pdf:pdf error:error];
        };
        _typesetViews[path] = v;
    } else {
        [v setPath:path source:source ?: @""];   // typesets again only when the text changed
    }
    [_typesetOrder addObject:path];
    [self typesetOf:path view:v];
}

- (void)stopTypesetViews {
    for (LatexView *v in _typesetViews.allValues) [v stopPreview];
    _typesetViews = nil;
    _typesetOrder = nil;
}

- (void)stop {
    [self stopTypesetViews];
    [_timer invalidate];
    _timer = nil;
    if (_server) {
        _server->stop();
        if (_thread.joinable()) _thread.join();
        _server.reset();   // detaches the view from the host
    }
    _url = nil;
}

@end
