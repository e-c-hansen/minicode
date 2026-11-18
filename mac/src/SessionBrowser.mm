// SessionBrowser.mm — see SessionBrowser.h.
#import "SessionBrowser.h"

#import "AppSettings.h"
#import "EditorTab.h"
#import "SessionLink.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "Json.h"
#include "SessionWeb.h"

NSString *const MCSessionBrowserTermNote = @"Terminals opened while the session host is on appear here";
void (^MCSessionBrowserOpener)(NSURL *url) = nil;

namespace {
std::atomic<uint64_t> gNextWindow{1000};   // clear of GTK's and the checks' small numbers
}

@implementation MCSessionBrowser {
    __weak NSWindow *_window;
    NSString *_folder;
    NSArray<NSArray<MCTab *> *> * (^_tabs)(void);
    NSArray<MCTab *> * (^_active)(void);
    std::unique_ptr<Shared::Web::Server> _server;
    std::thread _thread;
    NSTimer *_timer;
    std::string _sentTabs;
    uint64_t _windowId;
    id _closeObserver;
    id _settingsObserver;
}

- (instancetype)initWithWindow:(NSWindow *)window
                        folder:(NSString *)folder
                          tabs:(NSArray<NSArray<MCTab *> *> * (^)(void))tabs
                        active:(NSArray<MCTab *> * (^)(void))active {
    if ((self = [super init])) {
        _window = window;
        _folder = [folder copy] ?: @"";
        _tabs = [tabs copy];
        _active = [active copy];
        _windowId = gNextWindow++;
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

- (uint64_t)windowId { return _windowId; }
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
    o.connect = [](Shared::Client &c) { return MCSessionConnectViewer(c); };
    o.folder = _folder.fileSystemRepresentation ?: "";
    o.window = _windowId;
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

// The window's tabs as the host keeps a window's (Shared::windowJson's
// shape): the tabs that have a document on the host, by group.
- (void)reportTabs {
    NSArray<NSArray<MCTab *> *> *groups = _tabs ? _tabs() : @[];
    NSArray<MCTab *> *active = _active ? _active() : @[];
    Json jgroups = Json::array();
    for (NSUInteger g = 0; g < groups.count; g++) {
        Json tabs = Json::array();
        for (MCTab *t in groups[g]) {
            if (!t.session) continue;
            Json tab = Json::object();
            tab.set("doc", Json((int64_t)t.session.docId));
            tab.set("path", Json(std::string(t.currentPath.fileSystemRepresentation ?: "")));
            tab.set("preview", Json((int)(t.preview ? 1 : 0)));
            tabs.push(std::move(tab));
        }
        id a = g < active.count ? active[g] : nil;
        MCTab *on = [a isKindOfClass:MCTab.class] ? a : nil;   // NSNull: none on show
        Json group = Json::object();
        group.set("tabs", std::move(tabs));
        group.set("active", Json((int64_t)(on.session ? on.session.docId : 0)));
        jgroups.push(std::move(group));
    }
    Json w = Json::object();
    w.set("root", Json(std::string(_folder.fileSystemRepresentation ?: "")));
    w.set("focused", Json(0));
    w.set("groups", std::move(jgroups));
    w.set("closed", Json(0));
    std::string dump = w.dump();
    if (dump == _sentTabs) return;
    _sentTabs = std::move(dump);
    MCSessionClient().windowTabs(_windowId, std::move(w));
}

- (void)stop {
    [_timer invalidate];
    _timer = nil;
    if (_server) {
        _server->stop();
        if (_thread.joinable()) _thread.join();
        _server.reset();   // detaches the view from the host
    }
    if (!_sentTabs.empty()) {
        _sentTabs.clear();
        MCSessionClient().windowClosed(_windowId);
    }
}

@end
