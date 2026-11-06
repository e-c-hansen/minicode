// BrowserPages.mm — see BrowserPages.h.
#import "BrowserPages.h"

namespace {

NSString *const kWorldName = @"minicode-page-check";
NSString *const kHandlerName = @"minicodeConsole";

// The page-world wrapper: each console call is passed on as a DOM event
// carrying JSON text (a string crosses worlds as it is), then made as
// usual. It keeps its own copies of what it uses, so a page that replaces
// JSON.stringify or dispatchEvent later changes nothing, and it never
// throws into the page.
NSString *PageWorldShim(NSString *eventName) {
    return [NSString stringWithFormat:@
        "(function(){"
        "var c=window.console;if(!c)return;"
        "var name=%@,D=document,CE=CustomEvent,send=EventTarget.prototype.dispatchEvent,"
        "S=String,J=JSON.stringify,E=Error,A=Array.prototype.join;"
        "function show(a){try{if(typeof a==='string')return a.slice(0,4096);"
        "if(a instanceof E)return S(a);"
        "if(typeof a==='object'&&a!==null){var j=J(a);if(typeof j==='string')return j.slice(0,4096);}"
        "return S(a).slice(0,4096);}catch(x){return '[object]';}}"
        "['log','info','warn','error','debug'].forEach(function(level){"
        "var orig=c[level];if(typeof orig!=='function')return;"
        "c[level]=function(){"
        "try{var parts=[];for(var i=0;i<arguments.length&&i<32;i++)parts.push(show(arguments[i]));"
        "var st='';try{st=S(new E().stack||'').split('\\n').slice(1,8).join('\\n');}catch(x){}"
        "send.call(D,new CE(name,{detail:J({kind:'console',level:level,"
        "text:A.call(parts,' '),stack:st})}));}catch(x){}"
        "return orig.apply(this,arguments);};});"
        "})();",
        [NSString stringWithFormat:@"'%@'", eventName]];
}

// The isolated world's listener: uncaught errors and rejections straight
// from the window's events, console calls from the wrapper's event, all
// handed to the app as JSON text.
NSString *IsolatedListener(NSString *eventName) {
    return [NSString stringWithFormat:@
        "(function(){"
        "var h=window.webkit&&window.webkit.messageHandlers&&window.webkit.messageHandlers.%@;"
        "if(!h)return;"
        "function post(o){try{h.postMessage(JSON.stringify(o));}catch(x){}}"
        "function str(v){try{return String(v).slice(0,4096);}catch(x){return '';}}"
        "window.addEventListener('error',function(e){"
        "if(!(e instanceof ErrorEvent))return;"
        "post({kind:'uncaught',text:str(e.message),source:str(e.filename||''),"
        "line:e.lineno||0,column:e.colno||0});},true);"
        "window.addEventListener('unhandledrejection',function(e){"
        "var r=e.reason,t;try{t=(r&&r.stack)?str(r)+'':str(r);}catch(x){t='';}"
        "post({kind:'rejection',text:t});},true);"
        "document.addEventListener('%@',function(e){"
        "var d=e.detail;if(typeof d==='string'&&d.length<65536)post({relay:d});},true);"
        "})();",
        kHandlerName, eventName];
}

}  // namespace

// The message handler holds the probe weakly: WKUserContentController keeps
// its handlers for as long as the web view lives.
@interface MCPageConsoleHandler : NSObject <WKScriptMessageHandler>
@property(nonatomic, weak) MCPageProbe *probe;
@end

@interface MCPageProbe ()
- (void)addConsoleMessage:(NSString *)json fromPage:(BOOL)fromPage;
@end

@implementation MCPageConsoleHandler
- (void)userContentController:(WKUserContentController *)ucc
      didReceiveScriptMessage:(WKScriptMessage *)message {
    (void)ucc;
    if (![message.body isKindOfClass:NSString.class]) return;
    // A relay from the page-world wrapper carries the call's own JSON.
    NSString *body = message.body;
    NSData *d = [body dataUsingEncoding:NSUTF8StringEncoding];
    NSDictionary *o = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
    const BOOL relay = [o isKindOfClass:NSDictionary.class] && [o[@"relay"] isKindOfClass:NSString.class];
    [self.probe addConsoleMessage:relay ? o[@"relay"] : body fromPage:relay];
}
@end

@implementation MCPageProbe {
    PageCheck::ConsoleLog _console;
    NSMapTable<WKNavigation *, NSMutableArray *> *_waiters;
    int _pendingStatus;
    BOOL _hasPendingStatus;
    WKUserContentController *_ucc;
    NSString *_docTitle, *_docTitleURL;   // document.title, read when a load finished
}

+ (WKContentWorld *)world {
    return [WKContentWorld worldWithName:kWorldName];
}

+ (WKWebViewConfiguration *)agentPageConfigurationFor:(MCPageProbe *)probe {
    WKWebViewConfiguration *cfg = [[WKWebViewConfiguration alloc] init];
    WKUserContentController *ucc = [[WKUserContentController alloc] init];
    // A name of its own per page, so the event is not one a page knows of
    // before it reads the wrapper.
    NSString *event = [@"minicode-console-" stringByAppendingString:
                       [[NSUUID UUID].UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""]];
    MCPageConsoleHandler *handler = [MCPageConsoleHandler new];
    handler.probe = probe;
    [ucc addScriptMessageHandler:handler contentWorld:[self world] name:kHandlerName];
    [ucc addUserScript:[[WKUserScript alloc] initWithSource:IsolatedListener(event)
                                              injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                           forMainFrameOnly:YES
                                             inContentWorld:[self world]]];
    [ucc addUserScript:[[WKUserScript alloc] initWithSource:PageWorldShim(event)
                                              injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                           forMainFrameOnly:YES
                                             inContentWorld:WKContentWorld.pageWorld]];
    cfg.userContentController = ucc;
    probe->_ucc = ucc;
    return cfg;
}

- (instancetype)initRecordingConsole:(BOOL)console {
    if ((self = [super init])) {
        _recordsConsole = console;
        _waiters = [NSMapTable weakToStrongObjectsMapTable];
    }
    return self;
}

- (void)detach {
    [_ucc removeAllScriptMessageHandlers];
    [_ucc removeAllUserScripts];
    _ucc = nil;
    // Anyone still waiting hears that the page went.
    NSMutableArray *all = [NSMutableArray array];
    for (WKNavigation *n in _waiters.keyEnumerator) [all addObjectsFromArray:[_waiters objectForKey:n]];
    [_waiters removeAllObjects];
    for (void (^w)(NSString *, BOOL) in all) w(@"the agent page was closed", NO);
}

- (NSString *)url { return self.webView.URL.absoluteString ?: @""; }
// WKWebView's title can lag behind the page's by a moment when a load has
// just finished; the document's own, read then, stands in for it.
- (NSString *)title {
    NSString *t = self.webView.title;
    if (t.length) return t;
    if (_docTitle && [_docTitleURL isEqualToString:self.url]) return _docTitle;
    return @"";
}
- (BOOL)loading { return self.webView.isLoading; }
- (const PageCheck::ConsoleLog *)consoleLog { return &_console; }

- (Json)stateJson {
    return PageCheck::pageState(self.url.UTF8String ?: "", self.status,
                                self.title.UTF8String ?: "", self.loading);
}

- (void)addConsoleMessage:(NSString *)json fromPage:(BOOL)fromPage {
    if (!_recordsConsole) return;
    PageCheck::ConsoleEntry e;
    if (PageCheck::parseConsoleMessage(json.UTF8String ?: "", e, fromPage)) _console.add(e);
}

// ----------------------------------------------------------- navigation
- (void)noteResponse:(WKNavigationResponse *)response {
    if (!response.isForMainFrame) return;
    NSURLResponse *r = response.response;
    _pendingStatus = [r isKindOfClass:NSHTTPURLResponse.class]
        ? (int)((NSHTTPURLResponse *)r).statusCode : 0;
    _hasPendingStatus = YES;
}

- (void)noteCommit:(WKNavigation *)navigation {
    (void)navigation;
    // A page from the back-forward cache brings no response: no status.
    _status = _hasPendingStatus ? _pendingStatus : 0;
    _hasPendingStatus = NO;
    _console.newPage();
}

- (void)answer:(WKNavigation *)navigation error:(NSString *)error {
    if (!navigation) return;
    NSArray *list = [_waiters objectForKey:navigation];
    if (!list) return;
    [_waiters removeObjectForKey:navigation];
    for (void (^w)(NSString *, BOOL) in list) w(error, NO);
}

- (void)noteFinish:(WKNavigation *)navigation {
    if (!navigation || ![_waiters objectForKey:navigation] || self.webView.title.length) {
        [self answer:navigation error:nil];
        return;
    }
    NSString *url = self.url;
    [self evaluate:@"document.title" done:^(id value, NSString *error) {
        (void)error;
        if ([value isKindOfClass:NSString.class]) {
            self->_docTitle = value;
            self->_docTitleURL = url;
        }
        [self answer:navigation error:nil];
    }];
}

- (void)noteFail:(WKNavigation *)navigation error:(NSError *)error {
    _hasPendingStatus = NO;
    NSString *why = error.code == NSURLErrorCancelled && [error.domain isEqualToString:NSURLErrorDomain]
        ? @"the load was stopped, or replaced by another"
        : [NSString stringWithFormat:@"the page did not load: %@", error.localizedDescription];
    [self answer:navigation error:why];
}

- (void)waitFor:(WKNavigation *)navigation done:(void (^)(NSString *, BOOL))done {
    if (!navigation) { done(nil, NO); return; }
    __block BOOL answered = NO;
    void (^once)(NSString *, BOOL) = ^(NSString *error, BOOL timedOut) {
        if (answered) return;
        answered = YES;
        done(error, timedOut);
    };
    NSMutableArray *list = [_waiters objectForKey:navigation];
    if (!list) {
        list = [NSMutableArray array];
        [_waiters setObject:list forKey:navigation];
    }
    [list addObject:once];
    __weak MCPageProbe *weakSelf = self;
    __weak WKNavigation *weakNav = navigation;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(PageCheck::kWaitSeconds * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        MCPageProbe *me = weakSelf;
        if (me && weakNav) [[me->_waiters objectForKey:weakNav] removeObject:once];
        once(nil, YES);
    });
}

// ----------------------------------------------------------------- reads
- (void)evaluate:(NSString *)script done:(void (^)(id value, NSString *error))done {
    WKWebView *web = self.webView;
    if (!web) { done(nil, @"there is no page"); return; }
    [web evaluateJavaScript:script inFrame:nil inContentWorld:[MCPageProbe world]
          completionHandler:^(id value, NSError *error) {
        done(value, error ? error.localizedDescription : nil);
    }];
}

- (void)readWidths:(void (^)(double, double, NSString *))done {
    NSString *script =
        @"(function(){var d=document.documentElement,b=document.body;"
        @"if(!d)return [0,0];"
        @"return [d.clientWidth,Math.max(d.scrollWidth,b?b.scrollWidth:0)];})()";
    [self evaluate:script done:^(id value, NSString *error) {
        NSArray *a = [value isKindOfClass:NSArray.class] ? value : nil;
        if (error || a.count != 2 || ![a[0] isKindOfClass:NSNumber.class] ||
            ![a[1] isKindOfClass:NSNumber.class]) {
            done(0, 0, error ?: @"the page has no width");
            return;
        }
        done([a[0] doubleValue], [a[1] doubleValue], nil);
    }];
}

- (void)readText:(void (^)(NSString *, BOOL, NSString *))done {
    NSString *script = [NSString stringWithFormat:
        @"(function(){var b=document.body;var t=b?b.innerText:'';"
        @"return typeof t==='string'?t.slice(0,%zu):'';})()", PageCheck::kMaxTextBytes];
    [self evaluate:script done:^(id value, NSString *error) {
        if (error) { done(nil, NO, error); return; }
        NSString *text = [value isKindOfClass:NSString.class] ? value : @"";
        const std::string utf8 = text.UTF8String ?: "";
        const std::string clipped = PageCheck::clipUtf8(utf8, PageCheck::kMaxTextBytes);
        done(@(clipped.c_str()), clipped.size() < utf8.size() ||
                                     text.length >= PageCheck::kMaxTextBytes, nil);
    }];
}

// Writes a PNG of `cg`; the answer `done` gets.
static void MCWritePNG(CGImageRef cg, NSString *path, BOOL cut,
                       void (^done)(NSDictionary *, NSString *)) {
    if (!cg) { done(nil, @"WebKit took no picture"); return; }
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:cg];
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    NSError *writeError = nil;
    if (!png || ![png writeToFile:path options:NSDataWritingAtomic error:&writeError]) {
        done(nil, [NSString stringWithFormat:@"could not write %@: %@", path,
                   writeError.localizedDescription ?: @"no PNG"]);
        return;
    }
    done(@{@"path": path, @"width": @(rep.pixelsWide), @"height": @(rep.pixelsHigh), @"cut": @(cut)},
         nil);
}

// The top `height` points of a PDF's pages stacked top to bottom, as pixels
// at `scale`. WebKit ends a page at 14,400 points (PDF's largest page) and
// goes on in the next, so a page taller than that comes as several. Drawn
// in sRGB; the colors come out as in a snapshot of the visible area.
static CGImageRef MCRasterizePDF(NSData *pdf, double visibleHeight, double scale, BOOL *cut)
    CF_RETURNS_RETAINED {
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)pdf);
    CGPDFDocumentRef doc = provider ? CGPDFDocumentCreateWithProvider(provider) : NULL;
    if (provider) CGDataProviderRelease(provider);
    const size_t pages = doc ? CGPDFDocumentGetNumberOfPages(doc) : 0;
    if (!pages) {
        if (doc) CGPDFDocumentRelease(doc);
        return NULL;
    }
    double width = 0, total = 0;
    for (size_t i = 1; i <= pages; i++) {
        const CGRect box = CGPDFPageGetBoxRect(CGPDFDocumentGetPage(doc, i), kCGPDFMediaBox);
        width = MAX(width, box.size.width);
        total += box.size.height;
    }
    PageCheck::SnapshotPlan plan = PageCheck::snapshotPlan(visibleHeight, total, scale, true);
    *cut = plan.cut;
    const double h = MIN(plan.height, total);
    const size_t pw = (size_t)ceil(width * scale), ph = (size_t)ceil(h * scale);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(NULL, pw, ph, 8, 0, space,
                                             kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(space);
    CGImageRef image = NULL;
    if (ctx) {
        CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
        CGContextFillRect(ctx, CGRectMake(0, 0, pw, ph));
        CGContextScaleCTM(ctx, scale, scale);
        // PDF space starts at the bottom: each page's top goes where the
        // pages above it ended, measured down from the picture's top.
        double top = 0;
        for (size_t i = 1; i <= pages && top < h; i++) {
            CGPDFPageRef page = CGPDFDocumentGetPage(doc, i);
            const CGRect box = CGPDFPageGetBoxRect(page, kCGPDFMediaBox);
            CGContextSaveGState(ctx);
            CGContextTranslateCTM(ctx, -box.origin.x, h - top - box.size.height - box.origin.y);
            CGContextDrawPDFPage(ctx, page);
            CGContextRestoreGState(ctx);
            top += box.size.height;
        }
        image = CGBitmapContextCreateImage(ctx);
        CGContextRelease(ctx);
    }
    CGPDFDocumentRelease(doc);
    return image;
}

- (void)snapshotTo:(NSString *)path full:(BOOL)full
              done:(void (^)(NSDictionary *, NSString *))done {
    WKWebView *web = self.webView;
    if (!web) { done(nil, @"there is no page"); return; }
    NSString *dir = path.stringByDeletingLastPathComponent;
    BOOL isDir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:dir isDirectory:&isDir] || !isDir) {
        done(nil, [NSString stringWithFormat:@"no such folder: %@", dir]);
        return;
    }
    const double scale = web.window.backingScaleFactor > 0 ? web.window.backingScaleFactor : 2;
    const NSRect bounds = web.bounds;
    if (!full) {
        WKSnapshotConfiguration *cfg = [WKSnapshotConfiguration new];
        cfg.rect = bounds;
        [web takeSnapshotWithConfiguration:cfg completionHandler:^(NSImage *image, NSError *error) {
            if (!image) { done(nil, error.localizedDescription ?: @"WebKit took no picture"); return; }
            MCWritePNG([image CGImageForProposedRect:NULL context:nil hints:nil], path, NO, done);
        }];
        return;
    }
    // The whole page: a snapshot paints only what is on screen, so WebKit
    // draws the page as a PDF of its full height, which is drawn here.
    [web createPDFWithConfiguration:[WKPDFConfiguration new]
                  completionHandler:^(NSData *pdf, NSError *error) {
        if (!pdf.length) { done(nil, error.localizedDescription ?: @"WebKit drew no page"); return; }
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            BOOL cut = NO;
            CGImageRef image = MCRasterizePDF(pdf, bounds.size.height, scale, &cut);
            dispatch_async(dispatch_get_main_queue(), ^{
                MCWritePNG(image, path, cut, done);
                if (image) CGImageRelease(image);
            });
        });
    }];
}

@end
