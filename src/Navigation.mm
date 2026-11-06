// Navigation.mm — see Navigation.h. Objective-C++.
#import "Navigation.h"
#include "NavHistory.h"

#include <algorithm>

@implementation MCPlace
@end

// Line breaks between two offsets of a text, counting no further than
// `enough` (so a far jump in a large file costs no more than it must).
static NSInteger MCLineBreaks(NSString *s, NSUInteger a, NSUInteger b, NSInteger enough) {
    if (a > b) std::swap(a, b);
    b = MIN(b, s.length);
    CFStringInlineBuffer buf;
    CFStringInitInlineBuffer((__bridge CFStringRef)s, &buf, CFRangeMake(0, (CFIndex)b));
    NSInteger n = 0;
    for (NSUInteger i = a; i < b; i++) {
        if (CFStringGetCharacterFromInlineBuffer(&buf, (CFIndex)i) == '\n' && ++n >= enough)
            break;
    }
    return n;
}

static Nav::Place NavPlace(MCPlace *p) {
    Nav::Place out;
    out.path = p.path.UTF8String ?: "";
    out.location = p.selection.location;
    out.length = p.selection.length;
    out.line = (int)p.line;
    out.hasText = p.hasText;
    return out;
}

static MCPlace *PlaceFrom(const Nav::Place &p) {
    MCPlace *out = [MCPlace new];
    out.path = [NSString stringWithUTF8String:p.path.c_str()];
    out.selection = NSMakeRange(p.location, p.length);
    out.line = p.line;
    out.hasText = p.hasText;
    return out;
}

@implementation MCNavigator {
    __weak NSTextView *_tv;
    __weak id<MCNavigatorHost> _host;
    Nav::History _history;
    NSInteger _jumpDepth, _quiet, _switching;
    BOOL _navigating;
    MCPlace *_jumpFrom, *_switchFrom;
    // The caret as last seen, for telling a far move from typing.
    NSString *_lastPath;
    NSRange _lastSelection;
    NSUInteger _lastTextLength;
}

- (instancetype)initWithTextView:(NSTextView *)textView host:(id<MCNavigatorHost>)host {
    if (!(self = [super init])) return nil;
    _tv = textView;
    _host = host;
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(selectionChanged:)
                                                 name:NSTextViewDidChangeSelectionNotification
                                               object:textView];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(storageEdited:)
                                                 name:NSTextStorageDidProcessEditingNotification
                                               object:textView.textStorage];
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (MCPlace *)currentPlace {
    BOOL hasText = NO;
    NSString *path = [_host navigationPath:&hasText];
    if (!path.length) return nil;
    MCPlace *p = [MCPlace new];
    p.path = path;
    p.hasText = hasText;
    if (hasText) {
        NSString *s = _tv.string;
        NSRange sel = _tv.selectedRange;
        p.selection = sel;
        p.line = MCLineBreaks(s, 0, sel.location, NSIntegerMax);
    }
    return p;
}

// Remember where the caret is, for the next selection change to compare with.
- (void)noteCaret {
    BOOL hasText = NO;
    NSString *path = [_host navigationPath:&hasText];
    _lastPath = hasText ? path : nil;
    _lastSelection = _tv.selectedRange;
    _lastTextLength = _tv.string.length;
}

// The text on show changed: its places move with their text. A change of
// the whole text is another file (or the same one read again) coming in,
// which no place follows.
- (void)storageEdited:(NSNotification *)note {
    NSTextStorage *ts = note.object;
    if (!(ts.editedMask & NSTextStorageEditedCharacters)) return;
    const NSRange er = ts.editedRange;
    if (er.location == 0 && er.length == ts.length) return;
    BOOL hasText = NO;
    NSString *path = [_host navigationPath:&hasText];
    if (!hasText || !path.length) return;
    const NSInteger delta = ts.changeInLength;
    _history.edited(path.UTF8String ?: "", er.location,
                    (size_t)((NSInteger)er.length - delta), er.length);
}

// Lines an edit may have moved are counted again, in one pass over the
// text, before the history compares or shows them.
- (void)fixLines {
    BOOL hasText = NO;
    NSString *path = [_host navigationPath:&hasText];
    if (!hasText || !path.length) return;
    const std::string p = path.UTF8String ?: "";
    if (!_history.hasStaleLines(p)) return;
    NSString *s = _tv.string;
    _history.fixLines(p, [s](const std::vector<size_t> &offsets) {
        std::vector<int> out;
        NSUInteger at = 0;
        NSInteger line = 0;
        for (size_t o : offsets) {
            const NSUInteger to = MIN((NSUInteger)o, s.length);
            line += MCLineBreaks(s, at, to, NSIntegerMax);
            at = to;
            out.push_back((int)line);
        }
        return out;
    });
}

- (void)record:(MCPlace *)from to:(MCPlace *)to {
    if (!from || !to) return;
    [self fixLines];
    if ([from.path isEqualToString:to.path] && from.hasText == to.hasText &&
        NSEqualRanges(from.selection, to.selection))
        return;   // nothing moved (no definition found, a refused open)
    _history.jump(NavPlace(from), NavPlace(to));
}

// A caret that went far without the text changing is a jump of its own.
- (void)selectionChanged:(NSNotification *)note {
    (void)note;
    if (_jumpDepth || _quiet || _switching || _navigating) return;
    BOOL hasText = NO;
    NSString *path = [_host navigationPath:&hasText];
    NSString *s = _tv.string;
    const NSRange sel = _tv.selectedRange;
    if (hasText && _lastPath && [path isEqualToString:_lastPath] &&
        s.length == _lastTextLength &&
        MCLineBreaks(s, _lastSelection.location, sel.location, Nav::kFarLines + 1) >
            Nav::kFarLines) {
        MCPlace *from = [MCPlace new];
        from.path = path;
        from.hasText = YES;
        from.selection = _lastSelection;
        from.line = MCLineBreaks(s, 0, _lastSelection.location, NSIntegerMax);
        [self record:from to:[self currentPlace]];
    }
    _lastPath = hasText ? path : nil;
    _lastSelection = sel;
    _lastTextLength = s.length;
}

- (void)jump:(dispatch_block_t)body {
    [self jumpFrom:nil body:body];
}

- (void)jumpFrom:(MCPlace *)from body:(dispatch_block_t)body {
    const BOOL outer = _jumpDepth == 0 && !_navigating;
    if (outer) _jumpFrom = from ?: [self currentPlace];
    _jumpDepth++;
    if (body) body();
    _jumpDepth--;
    if (outer) {
        [self record:_jumpFrom to:[self currentPlace]];
        _jumpFrom = nil;
        [self noteCaret];
    }
}

- (void)beQuiet:(dispatch_block_t)body {
    _quiet++;
    if (body) body();
    _quiet--;
    if (!_quiet) [self noteCaret];
}

- (void)tabWillSwitch {
    if (_switching == 0) [self fixLines];   // while the text is still the one they count in
    if (_switching++ == 0 && !_jumpDepth && !_navigating && !_quiet)
        _switchFrom = [self currentPlace];
}

- (void)tabDidSwitch {
    if (_switching == 0) return;
    if (--_switching > 0) return;
    if (_switchFrom && !_jumpDepth && !_navigating && !_quiet)
        [self record:_switchFrom to:[self currentPlace]];
    _switchFrom = nil;
    if (!_jumpDepth) [self noteCaret];
}

// Files deleted since they were recorded are no places to go.
- (void)dropMissing {
    NSFileManager *fm = [NSFileManager defaultManager];
    _history.removeIf([fm](const std::string &path) {
        return ![fm fileExistsAtPath:[NSString stringWithUTF8String:path.c_str()]];
    });
}

- (BOOL)canGoBack {
    [self fixLines];
    MCPlace *here = [self currentPlace];
    if (!here) return !_history.entries().empty();
    return _history.canGoBack(NavPlace(here));
}

- (BOOL)canGoForward { return _history.canGoForward(); }

- (void)go:(BOOL)back {
    [self dropMissing];
    [self fixLines];
    for (int tries = 0; tries < 50; tries++) {
        MCPlace *here = [self currentPlace];
        Nav::Place target;
        if (!here) {
            // Nothing on show (every tab closed): back to the last place.
            if (!back || _history.entries().empty()) break;
            target = _history.entries()[_history.index()];
        } else if (!(back ? _history.back(NavPlace(here), &target)
                          : _history.forward(NavPlace(here), &target))) {
            break;
        }
        _navigating = YES;
        BOOL shown = [_host showNavigationPlace:PlaceFrom(target)];
        _navigating = NO;
        [self noteCaret];
        if (shown) return;
        // It could not be opened: forget it and go on past it.
        const std::string gone = target.path;
        _history.removeIf([gone](const std::string &p) { return p == gone; });
    }
    NSBeep();
}

- (void)goBack { [self go:YES]; }
- (void)goForward { [self go:NO]; }

- (void)pathMovedFrom:(NSString *)from to:(NSString *)to {
    if (!from.length || !to.length) return;
    _history.moved(from.UTF8String, to.UTF8String);
    if ([_lastPath isEqualToString:from]) _lastPath = to;
}

- (void)pathRemoved:(NSString *)path {
    if (!path.length) return;
    const std::string p = path.UTF8String, prefix = p + "/";
    _history.removeIf([p, prefix](const std::string &x) {
        return x == p || x.compare(0, prefix.size(), prefix) == 0;
    });
}

- (void)clear {
    _history.clear();
    _lastPath = nil;
}

- (NSArray<NSString *> *)entries {
    [self fixLines];
    NSMutableArray *out = [NSMutableArray array];
    for (const Nav::Place &p : _history.entries())
        [out addObject:[NSString stringWithFormat:@"%s:%d", p.path.c_str(), p.line]];
    return out;
}

- (NSInteger)index { return (NSInteger)_history.index(); }
@end
