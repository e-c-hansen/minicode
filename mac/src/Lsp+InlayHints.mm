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

// Control and Option held down (VS Code's Ctrl+Alt for the "unless pressed"
// modes of editor.inlayHints.enabled), from one watcher for the whole app,
// posted to every session.
static NSString *const MCInlayHintsPressedNotification = @"MCInlayHintsPressed";
static BOOL gInlayPressed = NO;

static void MCInlayWatchModifiers(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        auto post = ^(BOOL pressed) {
            if (pressed == gInlayPressed) return;
            gInlayPressed = pressed;
            [[NSNotificationCenter defaultCenter]
                postNotificationName:MCInlayHintsPressedNotification object:nil];
        };
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskFlagsChanged
                                              handler:^NSEvent *(NSEvent *event) {
            const NSEventModifierFlags both = NSEventModifierFlagControl | NSEventModifierFlagOption;
            post((event.modifierFlags & both) == both);
            return event;
        }];
        // A release while another app is in front never arrives.
        [[NSNotificationCenter defaultCenter]
            addObserverForName:NSApplicationDidResignActiveNotification object:nil queue:nil
                    usingBlock:^(NSNotification *note) {
            (void)note;
            post(NO);
        }];
    });
}

// Test hook: Control+Option held down, or let go.
void MCInlayHintsSetPressed(BOOL pressed) {
    MCInlayWatchModifiers();
    if (pressed == gInlayPressed) return;
    gInlayPressed = pressed;
    [[NSNotificationCenter defaultCenter] postNotificationName:MCInlayHintsPressedNotification
                                                        object:nil];
}

// View > Inlay Hints: editor.inlay-hints on or off in the settings file,
// checked while hints show without Control+Option held down.
@interface MCInlayHintsMenuTarget : NSObject
@end
@implementation MCInlayHintsMenuTarget
- (void)toggleInlayHints:(id)sender {
    (void)sender;
    const BOOL shown = Settings::inlayHintsShown([AppSettings shared].settings.inlayHintsMode(), NO);
    if (![[AppSettings shared] writeValue:shown ? @"off" : @"on" forKey:@"editor.inlay-hints"])
        NSBeep();
}
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(toggleInlayHints:))
        item.state = Settings::inlayHintsShown([AppSettings shared].settings.inlayHintsMode(), NO)
                         ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}
@end

NSMenuItem *MCInlayHintsMenuItem(void) {
    static MCInlayHintsMenuTarget *target;
    if (!target) target = [MCInlayHintsMenuTarget new];
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Inlay Hints"
                                                  action:@selector(toggleInlayHints:)
                                           keyEquivalent:@""];
    item.target = target;
    item.toolTip = @"What the language server says between the words, such as a variable's "
                   @"type (editor.inlay-hints). Double-click one to write it into the text.";
    return item;
}

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
    MCInlayWatchModifiers();
    _inlayShown = Settings::inlayHintsShown([AppSettings shared].settings.inlayHintsMode(),
                                            gInlayPressed);
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserver:self selector:@selector(inlayHintsTextEdited:)
               name:NSTextStorageDidProcessEditingNotification object:_tv.textStorage];
    [nc addObserver:self selector:@selector(inlayHintsSettingsChanged:)
               name:MCSettingsDidChangeNotification object:nil];
    [nc addObserver:self selector:@selector(inlayHintsSettingsChanged:)
               name:MCInlayHintsPressedNotification object:nil];
    __weak LspSession *weakSelf = self;
    _tv.onInlayHintDoubleClick = ^BOOL(NSUInteger index) {
        LspSession *s = weakSelf;
        return s && [s acceptInlayHintAtIndex:index];
    };
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

// The setting changed, or Control+Option went down or up: hints come
// (asked for at once) or go.
- (void)inlayHintsSettingsChanged:(NSNotification *)note {
    (void)note;
    const BOOL shown = Settings::inlayHintsShown([AppSettings shared].settings.inlayHintsMode(),
                                                 gInlayPressed);
    if (shown == _inlayShown) return;
    _inlayShown = shown;
    if (shown) [self scheduleInlayHints:0];
    else [self clearInlayHints];
}

- (BOOL)inlayHintsShown { return _inlayShown; }

// A double-click on a hint: its textEdits go into the text as one undo
// step (VS Code's), the caret after the hint's own text, and the hint goes
// at once. NO for a hint with nothing to write (the click selects a word).
- (BOOL)acceptInlayHintAtIndex:(NSUInteger)index {
    MCInlayHints *hints = _tv.inlayHints;
    const Lsp::InlayHint *hint = [hints hintAtIndex:index];
    NSArray<NSDictionary *> *shown = hints.shownHints;
    if (!hint || hint->textEdits.empty() || index >= shown.count || !_tv.editable) return NO;
    const NSUInteger at = [shown[index][@"offset"] unsignedIntegerValue];
    const std::vector<Lsp::OffsetEdit> edits =
        Lsp::inlayHintAcceptEdits(*hint, U16Of(_tv.string), _edits == _inlayEdits, at);
    if (edits.empty()) {
        // Its import or its place moved with typing since: ask again.
        NSBeep();
        [self scheduleInlayHints:0];
        return YES;
    }
    // Ascending for replaceRanges: (the edits come last first), and where
    // the caret goes: after what goes in at the hint.
    NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
    NSMutableArray<NSString *> *strings = [NSMutableArray array];
    NSInteger shift = 0;
    NSUInteger caret = at;
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        [ranges addObject:[NSValue valueWithRange:NSMakeRange(it->start, it->length)]];
        [strings addObject:[NSString stringWithCharacters:(const unichar *)it->text.data()
                                                   length:it->text.size()]];
        if (it->start < at || (it->start == at && it->length == 0)) {
            shift += (NSInteger)it->text.size() - (NSInteger)it->length;
            if (it->start == at) caret = (NSUInteger)((NSInteger)at + shift);
        }
    }
    if (caret == at) caret = (NSUInteger)((NSInteger)at + shift);
    [hints removeHintAtIndex:index];
    if (![_tv replaceRanges:ranges withStrings:strings actionName:@"Insert Inlay Hint"]) return YES;
    if (caret <= _tv.string.length) _tv.selectedRange = NSMakeRange(caret, 0);
    return YES;
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
    if (!_uri || !_inlayShown || _inlayWindow.location == NSNotFound) return;
    const NSRange vis = [self inlayVisibleCharacters];
    if (vis.location >= _inlayWindow.location && NSMaxRange(vis) <= NSMaxRange(_inlayWindow))
        return;
    [self scheduleInlayHints:0.1];
}

- (void)requestInlayHints {
    [self forgetInlayRequest];
    MCLspServer *server = _uri && _inlayShown && _tv.editable
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
        if (!error.isNull() || ![uri isEqualToString:me->_uri] || me->_edits != edits ||
            !me->_inlayShown)
            return;
        me->_inlayEdits = edits;
        std::vector<Lsp::InlayHint> hints = Lsp::parseInlayHints(result);
        Lsp::shortenInlayLabels(hints);
        [me->_tv.inlayHints showHints:hints
                               placed:Lsp::placeInlayHints(hints, *window, a, firstLine)];
    });
    _inlayServer = server;
    _inlayRequests++;
}

@end
