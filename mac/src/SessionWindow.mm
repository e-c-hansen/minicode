// SessionWindow.mm — see SessionWindow.h.
#import "SessionWindow.h"

#import "EditorTab.h"

#include <deque>
#include <string>
#include <vector>

#include "SessionWeb.h"

@implementation MCSessionWindow {
    std::shared_ptr<MCSession::Link> _l;
    std::deque<MCSession::HeldTerm> _heldTerms;
    int _lostToken;
    NSTimer *_timer;
    BOOL _detached;
}

- (instancetype)initWithFolder:(NSString *)folder {
    if ((self = [super init])) {
        _dropOnClose = [NSMutableSet set];
        [self useLink:[[MCSessionLinkRef alloc]
                          initWithLink:MCSession::forFolder(folder.fileSystemRepresentation ?: "")]
               folder:folder];
    }
    return self;
}

- (void)dealloc {
    [_timer invalidate];
    if (_l) _l->removeLostListener(_lostToken);
}

- (void)useLink:(MCSessionLinkRef *)link folder:(NSString *)folder {
    if (_l) _l->removeLostListener(_lostToken);
    _folder = [folder copy] ?: @"";
    _link = link;
    _l = link.link;
    _windowId = _l->newWindow();
    _heldActiveDoc = 0;
    _detached = NO;
    __weak MCSessionWindow *weakSelf = self;
    _lostToken = _l->addLostListener([weakSelf] {
        MCSessionWindow *w = weakSelf;
        if (w && w.onLost) w.onLost();
    });
}

- (void)moveTo:(MCSessionLinkRef *)link folder:(NSString *)folder {
    _l->windowClosed(_windowId);
    _heldTerms.clear();
    [self useLink:link folder:folder];
}

// ---- the record

- (Json)recordLeavingOut:(NSSet<MCTab *> *)leaveOut {
    NSArray<NSArray<MCTab *> *> *groups = _tabs ? _tabs() : @[];
    NSArray *active = _active ? _active() : @[];
    Json jgroups = Json::array();
    for (NSUInteger g = 0; g < groups.count; g++) {
        Json tabs = Json::array();
        for (MCTab *t in groups[g]) {
            if (!t.session || [leaveOut containsObject:t]) continue;
            Json tab = Json::object();
            tab.set("doc", Json((int64_t)t.session.docId));
            tab.set("path", Json(std::string(t.currentPath.fileSystemRepresentation ?: "")));
            tab.set("preview", Json((int)(t.preview ? 1 : 0)));
            tabs.push(std::move(tab));
        }
        id a = g < active.count ? active[g] : nil;
        MCTab *on = [a isKindOfClass:MCTab.class] ? a : nil;   // NSNull: none on show
        if ([leaveOut containsObject:on]) on = nil;
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
    // The window's own terminals: the page's view shows no others, and the
    // folder's next window takes these.
    Json terms = Json::array();
    for (NSNumber *t in _terminals ? _terminals() : @[])
        if (t.unsignedLongLongValue) terms.push(Json((int64_t)t.unsignedLongLongValue));
    w.set("terms", std::move(terms));
    // The browser pane's tabs, when the window has the pane.
    if (NSDictionary *pane = _browserTabs ? _browserTabs() : nil) {
        std::vector<Shared::Web::BrowserTab> tabs;
        for (NSDictionary *t in pane[@"tabs"]) {
            if (![t isKindOfClass:NSDictionary.class]) continue;
            NSString *url = [t[@"url"] isKindOfClass:NSString.class] ? t[@"url"] : @"";
            NSString *title = [t[@"title"] isKindOfClass:NSString.class] ? t[@"title"] : @"";
            tabs.push_back({url.UTF8String ?: "", title.UTF8String ?: ""});
        }
        w.set("browser", Shared::Web::browserReportJson(tabs, [pane[@"active"] intValue]));
    }
    // Which panes it shows, so the page lays itself out alike.
    if (NSDictionary *panes = _panes ? _panes() : nil)
        w.set("panes", Shared::Web::panesReportJson([panes[@"browser"] boolValue], [panes[@"terminal"] boolValue],
                                                    [panes[@"browserShare"] doubleValue]));
    return w;
}

- (void)report {
    if (_detached || !_windowId || _l->lost()) return;
    _l->windowTabs(_windowId, [self recordLeavingOut:nil]);
    if (_onReported) _onReported();
}

- (void)start {
    if (_timer) return;
    __weak MCSessionWindow *weakSelf = self;
    _timer = [NSTimer timerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *) { [weakSelf report]; }];
    [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
}

- (void)stop {
    [_timer invalidate];
    _timer = nil;
}

- (void)detachKeeping:(BOOL)keep leavingOut:(NSSet<MCTab *> *)leaveOut {
    if (_detached) return;
    [self stop];
    _detached = YES;
    _heldTerms.clear();
    _l->windowDetached(_windowId, [self recordLeavingOut:leaveOut], keep);
}

// ---- what the host held

- (MCSession::Held)takeHeld {
    uint64_t window = _windowId;
    MCSession::Held h = _l->takeForWindow(&window, _folder.fileSystemRepresentation ?: "");
    _windowId = window;
    _heldTerms.assign(h.terms.begin(), h.terms.end());
    _heldActiveDoc = 0;
    _heldDocsAttached = 0;
    if (h.tabs.isObject()) {
        const Json &groups = h.tabs["groups"];
        const int64_t focused = h.tabs["focused"].isNumber() ? h.tabs["focused"].asInt() : 0;
        if (focused >= 0 && (size_t)focused < groups.size())
            _heldActiveDoc = (uint64_t)groups[(size_t)focused]["active"].asInt();
    }
    return h;
}

- (NSUInteger)heldTermsLeft { return _heldTerms.size(); }

- (MCSessionTermPlace *)nextTermPlace {
    MCSessionTermPlace *p = [MCSessionTermPlace new];
    p.link = _link;
    if (!_heldTerms.empty() && !_l->lost()) {
        const MCSession::HeldTerm h = _heldTerms.front();
        _heldTerms.pop_front();
        p.heldTerm = h.id;
        p.heldPid = h.pid;
        p.heldCols = h.cols;
        p.heldRows = h.rows;
        p.heldShell = h.shell;
    }
    return p;
}

@end
