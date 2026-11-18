// SessionBrowser.mm — see SessionBrowser.h.
#import "SessionBrowser.h"

#import "AppSettings.h"
#import "EditorTab.h"
#import "FileTree.h"   // gShowHidden
#import "SessionLink.h"
#import "SessionWindow.h"

#include <memory>
#include <string>
#include <thread>

#include "Json.h"
#include "SessionWeb.h"

NSString *const MCSessionBrowserTermNote = @"Terminals opened while the session host is on appear here";
void (^MCSessionBrowserOpener)(NSURL *url) = nil;

@implementation MCSessionBrowser {
    __weak NSWindow *_window;
    MCSessionWindow *_session;
    std::unique_ptr<Shared::Web::Server> _server;
    std::thread _thread;
    NSTimer *_timer;
    id _closeObserver;
    id _settingsObserver;
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

- (NSString *)url {
    if (!_server) return nil;
    return [NSString stringWithUTF8String:_server->url().c_str()];
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
    __weak MCSessionBrowser *weakSelf = self;
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

- (void)lookNow {
    if (_server) _server->lookNow();
}

- (void)stop {
    [_timer invalidate];
    _timer = nil;
    if (_server) {
        _server->stop();
        if (_thread.joinable()) _thread.join();
        _server.reset();   // detaches the view from the host
    }
}

@end
