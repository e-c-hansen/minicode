// Lsp+InlayHints.mm — Objective-C++. LspSession (InlayHints): asking the
// file's server for the inlay hints of the lines on show
// (textDocument/inlayHint), as VS Code's editor.inlayHints does: when the
// file opens or its server becomes ready, 0.3 s after typing stops, when
// scrolling leaves the lines asked about, and when the server says its
// hints changed (workspace/inlayHint/refresh). The view's MCInlayHints
// draws them (LspInlayHints.h); the storage keeps them on their text while
// the next answer is on its way.
#import "Lsp+Private.h"
#import "Lsp+InlayHintsIvars.h"
#import "LspInlayHints.h"
#include "InlayHints.h"

// Lines asked about either side of those on show, as VS Code asks.
static const int kInlayExtraLines = 30;

// The start of the line holding offset i, by LSP's breaks (\r\n, \r, \n).
static NSUInteger InlayLineStart(NSString *s, NSUInteger i) {
    while (i > 0) {
        const unichar c = [s characterAtIndex:i - 1];
        if (c == '\n' || c == '\r') break;
        i--;
    }
    return i;
}

// Past the break ending the line holding offset i (the text's end for the
// last line).
static NSUInteger InlayLineEnd(NSString *s, NSUInteger i) {
    const NSUInteger n = s.length;
    while (i < n) {
        const unichar c = [s characterAtIndex:i++];
        if (c == '\n') break;
        if (c == '\r') {
            if (i < n && [s characterAtIndex:i] == '\n') i++;
            break;
        }
    }
    return i;
}

@implementation LspSession (InlayHints)

- (void)startInlayHints {
    _inlayWindow = NSMakeRange(NSNotFound, 0);
    _inlaySetting = [AppSettings shared].settings.inlayHints();
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserver:self selector:@selector(inlayHintsTextEdited:)
               name:NSTextStorageDidProcessEditingNotification object:_tv.textStorage];
    [nc addObserver:self selector:@selector(inlayHintsSettingsChanged:)
               name:MCSettingsDidChangeNotification object:nil];
}

- (NSUInteger)inlayHintRequests { return _inlayRequests; }

- (void)scheduleInlayHints:(NSTimeInterval)delay {
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(requestInlayHints)
                                               object:nil];
    [self performSelector:@selector(requestInlayHints) withObject:nil afterDelay:delay];
}

- (void)forgetInlayRequest {
    if (_inlayRequest) {
        MCLspServer *server = _inlayServer;
        if (server && !server.stopping) server.client.cancel(_inlayRequest);
    }
    _inlayRequest = 0;
    _inlayServer = nil;
}

// The file left the view (or its server went): its hints go with it.
- (void)clearInlayHints {
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(requestInlayHints)
                                               object:nil];
    [self forgetInlayRequest];
    _inlayWindow = NSMakeRange(NSNotFound, 0);
    [_tv.inlayHints clear];
}

// Typing: the answer on its way is for the text before it, and the next is
// asked once typing stops. The hints shown stay on their text meanwhile.
- (void)inlayHintsTextEdited:(NSNotification *)note {
    NSTextStorage *ts = note.object;
    if (!(ts.editedMask & NSTextStorageEditedCharacters) || !_uri) return;
    [self forgetInlayRequest];
    [self scheduleInlayHints:0.3];
}

- (void)inlayHintsSettingsChanged:(NSNotification *)note {
    (void)note;
    const BOOL on = [AppSettings shared].settings.inlayHints();
    if (on == _inlaySetting) return;
    _inlaySetting = on;
    if (on) [self scheduleInlayHints:0];
    else [self clearInlayHints];
}

// The characters on show, without laying out any text that is not.
- (NSRange)inlayVisibleCharacters {
    NSLayoutManager *lm = _tv.layoutManager;
    NSTextContainer *tc = _tv.textContainer;
    if (!lm || !tc) return NSMakeRange(0, 0);
    NSRect visible = _tv.visibleRect;
    const NSPoint origin = _tv.textContainerOrigin;
    NSRange glyphs = [lm glyphRangeForBoundingRect:NSOffsetRect(visible, -origin.x, -origin.y)
                                   inTextContainer:tc];
    return [lm characterRangeForGlyphRange:glyphs actualGlyphRange:NULL];
}

- (void)followInlayScrolling {
    NSClipView *clip = _tv.enclosingScrollView.contentView;
    if (!clip || clip == _inlayClip) return;
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    if (_inlayClip)
        [nc removeObserver:self name:NSViewBoundsDidChangeNotification object:_inlayClip];
    _inlayClip = clip;
    clip.postsBoundsChangedNotifications = YES;
    [nc addObserver:self selector:@selector(inlayHintsScrolled:)
               name:NSViewBoundsDidChangeNotification object:clip];
}

// Scrolling that leaves the lines asked about asks again, once it rests.
- (void)inlayHintsScrolled:(NSNotification *)note {
    (void)note;
    if (!_uri || !_inlaySetting || _inlayWindow.location == NSNotFound) return;
    const NSRange vis = [self inlayVisibleCharacters];
    if (vis.location >= _inlayWindow.location && NSMaxRange(vis) <= NSMaxRange(_inlayWindow))
        return;
    [self scheduleInlayHints:0.1];
}

- (void)requestInlayHints {
    [self forgetInlayRequest];
    MCLspServer *server = _uri && _inlaySetting && _tv.editable
                              ? [self serverFor:Lsp::Feature::InlayHint] : nil;
    // Only a server that is up and says it has them is asked.
    if (!server || server.stopping || server.client.state() != Lsp::Client::State::Ready ||
        !server.client.offers(Lsp::Feature::InlayHint)) {
        _inlayWindow = NSMakeRange(NSNotFound, 0);
        [_tv.inlayHints clear];
        return;
    }
    [self followInlayScrolling];
    [self flushChanges];
    NSString *text = _tv.string;
    const NSUInteger length = text.length;
    const NSRange vis = [self inlayVisibleCharacters];
    // The lines on show and 30 either side, whole lines by LSP's breaks.
    NSUInteger a = InlayLineStart(text, MIN(vis.location, length));
    for (int i = 0; i < kInlayExtraLines && a > 0; i++) {
        NSUInteger k = a - 1;   // the previous line's break, \r\n as one
        if (k > 0 && [text characterAtIndex:k] == '\n' && [text characterAtIndex:k - 1] == '\r')
            k--;
        a = InlayLineStart(text, k);
    }
    NSUInteger b = InlayLineEnd(text, MIN(MAX(NSMaxRange(vis), a), length));
    for (int i = 0; i < kInlayExtraLines && b < length; i++) b = InlayLineEnd(text, b);
    // The window's first line, counted a piece at a time (no copy of all
    // the text before it).
    Lsp::LineBreakCounter counter;
    unichar buffer[16384];
    for (NSUInteger at = 0; at < a;) {
        const NSUInteger n = MIN((NSUInteger)16384, a - at);
        [text getCharacters:buffer range:NSMakeRange(at, n)];
        counter.feed((const char16_t *)buffer, n);
        at += n;
    }
    const int firstLine = (int)counter.lines();
    auto window = std::make_shared<std::u16string>(U16Of([text substringWithRange:NSMakeRange(a, b - a)]));
    const Lsp::Position endRel = Lsp::PositionIndex(*window).position(window->size());
    const Lsp::Range range{Lsp::Position{firstLine, 0},
                           Lsp::Position{firstLine + endRel.line, endRel.character}};
    _inlayWindow = NSMakeRange(a, b - a);
    const NSUInteger edits = _edits;
    NSString *uri = _uri;
    __weak LspSession *weakSelf = self;
    // A newer request cancels this one (forgetInlayRequest), so an answer
    // that comes is the latest asked.
    _inlayRequest = server.client.inlayHints(Std(uri), range,
                                             [weakSelf, uri, edits, window, a, firstLine](
                                                 const Json &result, const Json &error) {
        LspSession *me = weakSelf;
        if (!me) return;
        me->_inlayRequest = 0;
        // For the text as it was asked about only: typing since asks again.
        if (!error.isNull() || ![uri isEqualToString:me->_uri] || me->_edits != edits) return;
        std::vector<Lsp::InlayHint> hints = Lsp::parseInlayHints(result);
        Lsp::shortenInlayLabels(hints);
        [me->_tv.inlayHints showHints:hints
                               placed:Lsp::placeInlayHints(hints, *window, a, firstLine)];
    });
    _inlayServer = server;
    _inlayRequests++;
}

@end
