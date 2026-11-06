// TerminalGridView.mm — see TerminalGridView.h. Objective-C++.
//
// Drawing: the panel's own layer paints the background, so cells in the
// default background draw nothing (a see-through panel stays see-through).
// ASCII runs in one style are drawn as one string; every other character is
// drawn in its own cell, so a fallback font's different advance can never
// push the rest of the row out of its columns.
//
// Lines: the view shows the screen's lines from -scrollOffset down, so rows
// above the screen come from the scrollback (TerminalScreen::lineCell). A
// selection, the scroll position and the Cmd-hovered link are held as line
// numbers counted from the first line ever (historyBase() + line), which
// stay put while output scrolls the text up, so none of them slide off the
// text they were on. A resize reflows the lines (resizeScreenToCols:rows:),
// and carries them along through TerminalScreen::resize.
//
// Soft wraps: rows the terminal wrapped are one line to a double-click, a
// triple-click, a Cmd-click link and find, and copy without a line break.
//
// Mouse reporting: when a program asks (vim's mouse=a, htop, less with
// --mouse), clicks, drags and the wheel go to it as TerminalScreen encodes
// them; Shift keeps them for selection, and Cmd-click still opens links.
//
// Keys: Cmd shortcuts are left to the menu (they arrive through
// performKeyEquivalent: before keyDown:), everything else is encoded by
// TerminalScreen::encodeKey / encodeChar. Option is Meta (ESC prefix) unless
// optionAsMeta is off. With textInput on, ordinary typing goes through
// interpretKeyEvents:, so dead keys and input methods compose (the marked
// text is drawn at the cursor) before the result goes to the pty.
#import "TerminalGridView.h"
#import "Terminal.h"
#import "AppSettings.h"
#import "Appearance.h"
#import "EditorFont.h"
#import <Carbon/Carbon.h>   // kVK_* key codes, EnableSecureEventInput
#include <algorithm>
#include <climits>
#include <utility>
#include <vector>
#include "TerminalStream.h"   // termCharWidth
#include "TermLinks.h"
#include <unistd.h>          // gethostname

static const long long kNoLine = LLONG_MIN;
// Synchronized output: the longest a frame may keep the view from drawing.
static const NSTimeInterval kSyncLimit = 0.15;

static NSMenuItem *MCMenuItemForKey(NSMenu *menu, NSEvent *e);
// Grid views with secure keyboard entry on (see updateSecureInput).
static NSInteger gSecureGrids;

@implementation TerminalGridView {
    BOOL _selecting, _hasSelection;
    long long _anchorId, _endId;   // selection ends, as line numbers
    int _anchorCol, _endCol;
    int _unit;                     // what a drag extends by: 1 cell, 2 word, 3 line
    long long _unitStartId, _unitEndId;   // the word or line first clicked
    int _unitStartCol, _unitEndCol;
    CGFloat _scrollAccum;
    int _offset;                   // lines scrolled back
    long long _lastBase;           // historyBase() when last drawn
    // The link under the pointer while Cmd is held: from _linkCol0 of line
    // _linkId to _linkCol1 (exclusive) of line _linkEndId, which is a later
    // line when the link runs over a soft wrap. _linkId is kNoLine when none.
    MCTermLink *_link;
    long long _linkId, _linkEndId;
    int _linkCol0, _linkCol1;
    // The address of the OSC 8 hyperlink linkAtPoint: last found (nil for a
    // link read from the text), shown as the tooltip while Cmd hovers it.
    NSString *_hyperlinkAddress;
    // Mouse reporting: the button held down and reported (-1 none), and the
    // cell last reported, so motion is sent once per cell.
    int _reportButton;
    int _reportRow, _reportCol;
    BOOL _focusReported, _focusKnown;
    NSString *_marked;             // input method text being composed
    NSRange _markedSel;
    // Synchronized output (mode 2026): when the view began waiting for the
    // program to finish a frame (0: not waiting), and, after a frame took
    // longer than kSyncLimit, the frame count it gave up at: the mode is
    // then ignored until a frame ends, so a program that never ends one
    // (killed mid-frame) cannot slow the view down for good.
    CFTimeInterval _syncSince;
    BOOL _redrawQueued;
    BOOL _syncIgnored;
    int _syncIgnoredAt;
    // Find: the matches in reading order, by line number (historyBase() +
    // line), the current one, and what was asked for.
    struct Found { long long id; int col, width; long long endId; int endCol; };
    std::vector<Found> _found;
    NSInteger _foundIndex;
    BOOL _finding, _findRefreshQueued;
    std::string _findQuery;
    BOOL _findCase;
    BOOL _secureOn;                // EnableSecureEventInput called, not yet undone
    // Moving between commands (moveToCommand:select:): the outlined
    // command's prompt line, and where a Shift selection started, both as
    // line numbers (kNoLine: none). The start is a boundary between lines:
    // the selection takes whole lines from one boundary to the other.
    long long _commandId;
    long long _commandAnchorId;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _linkId = _linkEndId = kNoLine;
        _commandId = _commandAnchorId = kNoLine;
        _reportButton = -1;
        _optionAsMeta = YES;
        _foundIndex = -1;
    }
    return self;
}

// The fonts and the cell follow terminal.font-size and the editor's zoom
// (MCTerminalFont keeps the fonts per size); the cell is measured again
// whenever the size has changed.
+ (NSFont *)cellFont { return MCTerminalFont(NO, NO); }

+ (NSFont *)fontBold:(BOOL)bold italic:(BOOL)italic { return MCTerminalFont(bold, italic); }

+ (NSSize)cellSize {
    static NSSize size;
    static CGFloat measuredAt;
    const CGFloat pt = MCTerminalFontSize();
    if (size.width == 0 || measuredAt != pt) {
        NSFont *f = [self cellFont];
        CGFloat w = [@"M" sizeWithAttributes:@{NSFontAttributeName: f}].width;
        CGFloat h = [[[NSLayoutManager alloc] init] defaultLineHeightForFont:f];
        size = NSMakeSize(w, ceil(h));
        measuredAt = pt;
    }
    return size;
}

+ (CGFloat)padding { return 6; }

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }

- (BOOL)becomeFirstResponder {
    [self setNeedsDisplay:YES];
    [self performSelector:@selector(reportFocus) withObject:nil afterDelay:0];
    return YES;
}
- (BOOL)resignFirstResponder {
    [self setNeedsDisplay:YES];
    [self performSelector:@selector(reportFocus) withObject:nil afterDelay:0];
    return YES;
}

// Focus events (DECSET 1004): the program hears "CSI I" when the terminal
// gets the keyboard (it is first responder in the key window) and "CSI O"
// when it loses it, once per change.
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc removeObserver:self name:NSWindowDidBecomeKeyNotification object:nil];
    [nc removeObserver:self name:NSWindowDidResignKeyNotification object:nil];
    [self updateSecureInput];   // out of a window: off
    if (!self.window) return;
    [nc addObserver:self selector:@selector(reportFocus)
               name:NSWindowDidBecomeKeyNotification object:self.window];
    [nc addObserver:self selector:@selector(reportFocus)
               name:NSWindowDidResignKeyNotification object:self.window];
}

- (BOOL)hasKeyboard {
    return self.window.isKeyWindow && self.window.firstResponder == self && !self.hidden;
}

- (void)reportFocus {
    [self updateSecureInput];
    TerminalScreen *s = self.screen;
    BOOL focused = [self hasKeyboard];
    if (!s || !s->focusEvents()) { _focusKnown = NO; return; }
    if (_focusKnown && focused == _focusReported) return;
    _focusKnown = YES;
    _focusReported = focused;
    if (self.onInput) {
        std::string bytes = TerminalScreen::encodeFocus(focused);
        self.onInput([NSData dataWithBytes:bytes.data() length:bytes.size()]);
    }
}

// The right click. In line mode only the settings item, as before. A
// Control-click goes to a program that asked for mouse events, as a click
// with Control.
- (NSMenu *)menuForEvent:(NSEvent *)event {
    if (event.type == NSEventTypeLeftMouseDown && [self reportsMouse:event]) return nil;
    if (!self.textInput) return MCAdjustSettingsMenu(@"terminal", self);
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];
    [menu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@""].target = self;
    [menu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@""].target = self;
    [menu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@""].target = self;
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"Clear Scrollback" action:@selector(clearScrollback:)
             keyEquivalent:@""].target = self;
    MCAddAdjustSettingsItem(menu, @"terminal", self);
    return menu;
}

// ---------------------------------------------------------------- lines
- (long long)base { return self.screen ? self.screen->historyBase() : 0; }

// The first line shown (row 0 of the view), as TerminalScreen numbers lines.
- (int)topLine { return -_offset; }

- (int)scrollOffset { return _offset; }

- (int)maxOffset {
    TerminalScreen *s = self.screen;
    return (!s || s->altScreen()) ? 0 : s->historySize();
}

- (void)setOffset:(int)offset {
    offset = MAX(0, MIN(offset, [self maxOffset]));
    if (offset == _offset) return;
    _offset = offset;
    if (_linkId != kNoLine) [self updateLinkHover];
    [self setNeedsDisplay:YES];
}

- (void)scrollToBottom {
    [self forgetCommand];
    [self setOffset:0];
}

// No command outlined any more (typing, a click, the bottom).
- (void)forgetCommand {
    if (_commandId != kNoLine) [self setNeedsDisplay:YES];
    _commandId = _commandAnchorId = kNoLine;
}

- (void)screenChanged {
    // New lines went into the scrollback: a view scrolled back moves with
    // them, so the text it shows stays put. One at the bottom follows.
    long long base = [self base];
    if (_offset > 0 && base != _lastBase) _offset += (int)(base - _lastBase);
    _lastBase = base;
    _offset = MAX(0, MIN(_offset, [self maxOffset]));
    // A selection whose lines left the scrollback goes.
    if (_hasSelection && self.screen) {
        long long first = base - self.screen->historySize();
        if (MIN(_anchorId, _endId) < first) _hasSelection = NO;
    }
    if (self.screen && _commandId != kNoLine &&
        MIN(_commandId, _commandAnchorId == kNoLine ? _commandId : _commandAnchorId) <
            base - self.screen->historySize())
        [self forgetCommand];
    // What was under the pointer may have moved; look again, but only when
    // there is a link showing or Cmd is down, so ordinary output costs nothing.
    if (_linkId != kNoLine || (NSEvent.modifierFlags & NSEventModifierFlagCommand))
        [self updateLinkHover];
    // Output under an open find bar: look again shortly, not on every piece.
    if (_finding && !_findRefreshQueued) {
        _findRefreshQueued = YES;
        [self performSelector:@selector(refreshFind) withObject:nil afterDelay:0.2
                      inModes:@[NSRunLoopCommonModes]];
    }
    // Whether to draw is decided once the output in hand is parsed: the
    // terminal parses it in pieces, calling this after each, and a redraw
    // asked for after one piece cannot be taken back (the layer keeps the
    // request) when the next piece begins a synchronized frame.
    if (!_redrawQueued) {
        _redrawQueued = YES;
        [self performSelector:@selector(redrawQueued) withObject:nil afterDelay:0
                      inModes:@[NSRunLoopCommonModes]];
    }
}

- (void)redrawQueued {
    _redrawQueued = NO;
    [self redrawUnlessMidFrame];
}

- (void)contentCleared {
    [self forgetCommand];
    _hasSelection = NO;
    _selecting = NO;
    _offset = 0;
    [self screenChanged];
    if (_finding) [self refreshFind];
}

// Synchronized output, DEC private mode 2026 (Claude Code uses it): between
// "CSI ? 2026 h" and "l" the program is drawing a frame, and showing the
// screen then shows half of it. So the view does not draw while the mode is
// set, and draws when it ends, or after kSyncLimit at most.
- (void)redrawUnlessMidFrame {
    TerminalScreen *s = self.screen;
    BOOL synced = s && s->synchronizedOutput();
    if (_syncIgnored && (!synced || s->synchronizedFramesEnded() != _syncIgnoredAt))
        _syncIgnored = NO;
    if (synced && !_syncIgnored) {
        if (_syncSince == 0) {
            _syncSince = CACurrentMediaTime();
            [self performSelector:@selector(syncTimedOut) withObject:nil
                       afterDelay:kSyncLimit inModes:@[NSRunLoopCommonModes]];
        }
        return;
    }
    if (_syncSince) {
        _syncSince = 0;
        [NSObject cancelPreviousPerformRequestsWithTarget:self
                                                 selector:@selector(syncTimedOut) object:nil];
    }
    [self setNeedsDisplay:YES];
}

- (void)syncTimedOut {
    if (!_syncSince) return;
    _syncSince = 0;
    TerminalScreen *s = self.screen;
    if (s && s->synchronizedOutput()) {
        _syncIgnored = YES;
        _syncIgnoredAt = s->synchronizedFramesEnded();
    }
    [self setNeedsDisplay:YES];
}

// Waiting on a frame (for tests and the host): the view holds its last
// picture until the program finishes drawing.
- (BOOL)waitingForFrame { return _syncSince != 0; }

- (void)dealloc {
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    if (_secureOn) {
        DisableSecureEventInput();
        gSecureGrids--;
    }
}

// ---------------------------------------------------------------- secure input
// Secure keyboard entry while the program asks for hidden input (a password)
// and this view has the keyboard: other processes cannot read the keys
// (event taps, keyloggers), as Terminal.app and iTerm2 do. The calls are
// counted for the whole process, so each view keeps one flag and calls
// Enable and Disable in pairs. Off the moment the view loses the keyboard:
// another view, another window, another app, hidden, or gone.
- (void)setHiddenInput:(BOOL)hiddenInput {
    if (_hiddenInput == hiddenInput) return;
    _hiddenInput = hiddenInput;
    [self updateSecureInput];
}

- (BOOL)secureInputOn { return _secureOn; }
+ (NSInteger)secureInputViews { return gSecureGrids; }

- (void)updateSecureInput {
    const BOOL want = _hiddenInput && [self hasKeyboard];
    if (want == _secureOn) return;
    _secureOn = want;
    if (want) {
        EnableSecureEventInput();
        gSecureGrids++;
    } else {
        DisableSecureEventInput();
        gSecureGrids--;
    }
    [self setNeedsDisplay:YES];   // the lock in the corner
}

- (void)viewDidHide {
    [super viewDidHide];
    [self updateSecureInput];
}

- (void)viewDidUnhide {
    [super viewDidUnhide];
    [self updateSecureInput];
}

// The lock drawn in the top right corner while secure entry is on, in the
// terminal's text color, faded.
- (void)drawSecureInputLock {
    static NSImage *symbol;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        symbol = [NSImage imageWithSystemSymbolName:@"lock.fill"
                           accessibilityDescription:@"Secure keyboard entry"];
    });
    if (!symbol) return;
    NSColor *color = [[[AppSettings shared] text:Surface::Terminal] colorWithAlphaComponent:0.6];
    const NSSize sz = symbol.size;
    const CGFloat h = 12, w = sz.height > 0 ? sz.width * h / sz.height : h;
    const NSRect r = NSMakeRect(self.bounds.size.width - w - 10, 6, w, h);
    NSImage *tinted = [NSImage imageWithSize:r.size flipped:NO drawingHandler:^BOOL(NSRect dst) {
        [symbol drawInRect:dst];
        [color set];
        NSRectFillUsingOperation(dst, NSCompositingOperationSourceAtop);
        return YES;
    }];
    [tinted drawInRect:r fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
              fraction:1 respectFlipped:YES hints:nil];
}

// A new size: the screen reflows, and the view's places (the top line of a
// scrolled-back view, the selection, the current find match) go with their
// text, so a view scrolled back stays on what it showed.
- (void)resizeScreenToCols:(int)cols rows:(int)rows {
    TerminalScreen *s = self.screen;
    if (!s) return;
    long long base = s->historyBase();
    auto rel = [&](long long lineId) {
        long long l = lineId - base;
        return (int)MAX((long long)INT_MIN / 2, MIN((long long)INT_MAX / 2, l));
    };
    BOOL hasMatch = _foundIndex >= 0 && _foundIndex < (NSInteger)_found.size();
    std::vector<TermPos> keep = {
        {-_offset, 0},
        {rel(_anchorId), _anchorCol}, {rel(_endId), _endCol},
        {rel(_unitStartId), _unitStartCol}, {rel(_unitEndId), _unitEndCol},
        {hasMatch ? rel(_found[_foundIndex].id) : 0, hasMatch ? _found[_foundIndex].col : 0},
        {_commandId == kNoLine ? 0 : rel(_commandId), 0},
        {_commandAnchorId == kNoLine ? 0 : rel(_commandAnchorId), 0},
    };
    s->resize(cols, rows, &keep);
    long long nb = s->historyBase();
    int first = -s->historySize();
    _lastBase = nb;
    if (_offset > 0) _offset = MAX(0, MIN(-keep[0].line, [self maxOffset]));
    if (_hasSelection && (keep[1].line < first || keep[2].line < first)) _hasSelection = NO;
    _anchorId = nb + keep[1].line; _anchorCol = keep[1].col;
    _endId = nb + keep[2].line; _endCol = keep[2].col;
    _unitStartId = nb + keep[3].line; _unitStartCol = keep[3].col;
    _unitEndId = nb + keep[4].line; _unitEndCol = keep[4].col;
    _link = nil;
    _linkId = _linkEndId = kNoLine;
    if (hasMatch) {
        _found[_foundIndex].id = nb + keep[5].line;
        _found[_foundIndex].col = keep[5].col;
    }
    if (_commandId != kNoLine) _commandId = nb + keep[6].line;
    if (_commandAnchorId != kNoLine) _commandAnchorId = nb + keep[7].line;
    if ((_commandId != kNoLine && keep[6].line < first) ||
        (_commandAnchorId != kNoLine && keep[7].line < first))
        [self forgetCommand];
    if (_finding) [self refreshFind];
    [self setNeedsDisplay:YES];
}

// ---------------------------------------------------------------- commands
- (int)currentCommandLine {
    if (_commandId == kNoLine || !self.screen) return INT_MIN;
    return (int)(_commandId - self.screen->historyBase());
}

// The bottom, as a boundary between lines: the prompt waiting for a command
// (not part of any command's text), else below the cursor's line.
- (int)bottomBoundary:(const std::vector<TermCommand> &)cmds {
    TerminalScreen *s = self.screen;
    if (!cmds.empty() && !cmds.back().ran) return cmds.back().promptLine;
    return s->cursorRow() + 1;
}

- (void)moveToCommand:(int)direction select:(BOOL)select {
    TerminalScreen *s = self.screen;
    if (!s || s->altScreen()) return;
    const long long base = s->historyBase();
    const std::vector<TermCommand> cmds = s->commands();
    const int bottom = [self bottomBoundary:cmds];
    // A Shift selection goes on from where the last one left off only while
    // it is still the one on show.
    if (!select || !_hasSelection) _commandAnchorId = kNoLine;
    if (select && _commandAnchorId == kNoLine)
        _commandAnchorId = _commandId != kNoLine ? _commandId : base + bottom;
    const int from = _commandId != kNoLine ? (int)(_commandId - base) : INT_MAX;
    int target = direction < 0 ? s->previousCommandLine(from)
                               : (from == INT_MAX ? INT_MIN : s->nextCommandLine(from));
    if (target == INT_MIN && direction < 0) {
        NSBeep();                           // no command above
        return;
    }
    if (target == INT_MIN) {                // past the newest: the bottom
        long long anchor = _commandAnchorId;
        [self forgetCommand];
        [self setOffset:0];
        if (select && anchor != kNoLine) {
            _commandAnchorId = anchor;
            [self selectLinesFrom:(int)(anchor - base) to:bottom];
        } else {
            _hasSelection = NO;
        }
        [self setNeedsDisplay:YES];
        return;
    }
    _commandId = base + target;
    [self setOffset:-target];               // its prompt at the top, as far as there is room
    if (select) [self selectLinesFrom:(int)(_commandAnchorId - base) to:target];
    else _hasSelection = NO;
    [self setNeedsDisplay:YES];
}

// Whole lines between two boundaries (a boundary is the start of a line).
- (void)selectLinesFrom:(int)a to:(int)b {
    TerminalScreen *s = self.screen;
    int first = MIN(a, b), last = MAX(a, b) - 1;
    if (last < first) { _hasSelection = NO; return; }
    long long base = s->historyBase();
    _anchorId = base + first;
    _anchorCol = 0;
    _endId = base + last;
    _endCol = MAX(s->lineWidth(last), s->cols()) - 1;
    _hasSelection = YES;
    _selecting = NO;
}

// The outlined command's lines: from its prompt to the line before the
// next prompt (or the bottom).
- (void)commandRows:(const std::vector<TermCommand> &)cmds first:(int *)first last:(int *)last {
    int line = self.currentCommandLine;
    *first = line;
    *last = line;
    for (size_t i = 0; i < cmds.size(); i++) {
        if (cmds[i].promptLine != line) continue;
        int end = i + 1 < cmds.size() ? cmds[i + 1].promptLine : [self bottomBoundary:cmds];
        *last = MAX(line, end - 1);
        break;
    }
}

// Beside the prompt of each command that failed, a red bar in the left
// margin (VS Code's terminalCommandDecoration.errorBackground); the
// outlined command (Cmd+Up and Cmd+Down) in a thin frame.
- (void)drawCommandMarksFrom:(int)firstRow to:(int)lastRow {
    TerminalScreen *s = self.screen;
    if (!s || s->altScreen() || s->markCount() == 0) return;
    const std::vector<TermCommand> cmds = s->commands();
    const NSSize cs = [TerminalGridView cellSize];
    const CGFloat pad = [TerminalGridView padding];
    const int top = [self topLine];
    [[NSColor colorWithSRGBRed:0xF1 / 255.0 green:0x4C / 255.0 blue:0x4C / 255.0 alpha:1] setFill];
    for (const TermCommand &c : cmds) {
        if (!c.ended || c.status == 0) continue;
        int row = c.promptLine - top;
        if (row < firstRow || row > lastRow) continue;
        NSRectFill(NSMakeRect(1, pad + row * cs.height + 1, 2.5, cs.height - 2));
    }
    if (_commandId == kNoLine) return;
    int first, last;
    [self commandRows:cmds first:&first last:&last];
    int r0 = MAX(first - top, 0), r1 = MIN(last - top, s->rows() - 1);
    if (r1 < r0) return;
    NSRect box = NSMakeRect(pad - 2, pad + r0 * cs.height,
                            s->cols() * cs.width + 4, (r1 - r0 + 1) * cs.height);
    [[[[AppSettings shared] text:Surface::Terminal] colorWithAlphaComponent:0.45] setStroke];
    NSBezierPath *p = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(box, 0.5, 0.5)
                                                      xRadius:3 yRadius:3];
    p.lineWidth = 1;
    [p stroke];
}

// ---------------------------------------------------------------- find
- (NSInteger)findLimit { return 10000; }
- (NSInteger)findCount { return (NSInteger)_found.size(); }
- (NSInteger)findIndex { return _foundIndex; }

// The matches for the current query, as line numbers.
- (std::vector<Found>)matches {
    std::vector<Found> out;
    TerminalScreen *s = self.screen;
    if (!s || _findQuery.empty()) return out;
    long long base = s->historyBase();
    for (const TermMatch &m : s->find(_findQuery, _findCase, (size_t)[self findLimit]))
        out.push_back({base + m.line, m.col, m.width, base + m.endLine, m.endCol});
    return out;
}

// The last match at or before a place (line number, column), else the first.
- (NSInteger)matchAtOrBefore:(long long)lineId col:(int)col {
    if (_found.empty()) return -1;
    NSInteger pick = 0;
    for (size_t i = 0; i < _found.size(); i++) {
        const Found &f = _found[i];
        if (f.id < lineId || (f.id == lineId && f.col <= col)) pick = (NSInteger)i;
        else break;
    }
    return pick;
}

- (void)revealCurrentMatch {
    TerminalScreen *s = self.screen;
    if (!s || _foundIndex < 0 || _foundIndex >= (NSInteger)_found.size()) return;
    int line = (int)(_found[_foundIndex].id - s->historyBase());
    int top = [self topLine], rows = s->rows();
    if (line < top || line > top + rows - 1)
        [self setOffset:rows / 2 - line];   // the match in the middle of the view
}

- (void)findText:(NSString *)text matchCase:(BOOL)matchCase {
    TerminalScreen *s = self.screen;
    const char *utf8 = text.UTF8String;
    // Where the last search was, or the bottom of the view for a new one.
    long long anchorId;
    int anchorCol;
    if (_foundIndex >= 0 && _foundIndex < (NSInteger)_found.size()) {
        anchorId = _found[_foundIndex].id;
        anchorCol = _found[_foundIndex].col;
    } else {
        anchorId = [self base] + [self topLine] + (s ? s->rows() - 1 : 0);
        anchorCol = INT_MAX;
    }
    _finding = YES;
    _findQuery = utf8 ? utf8 : "";
    _findCase = matchCase;
    _found = [self matches];
    _foundIndex = [self matchAtOrBefore:anchorId col:anchorCol];
    [self revealCurrentMatch];
    [self setNeedsDisplay:YES];
}

- (void)findNext:(BOOL)forward {
    if (_found.empty()) { NSBeep(); return; }
    NSInteger n = (NSInteger)_found.size();
    if (_foundIndex < 0) _foundIndex = forward ? 0 : n - 1;
    else _foundIndex = (_foundIndex + (forward ? 1 : n - 1)) % n;
    [self revealCurrentMatch];
    [self setNeedsDisplay:YES];
    if (self.onFindChanged) self.onFindChanged();
}

// Output arrived: the same search again, the current match kept where it
// still is (or the one before its place), the view left where it is.
- (void)refreshFind {
    _findRefreshQueued = NO;
    if (!_finding) return;
    long long anchorId = LLONG_MAX;
    int anchorCol = INT_MAX;
    if (_foundIndex >= 0 && _foundIndex < (NSInteger)_found.size()) {
        anchorId = _found[_foundIndex].id;
        anchorCol = _found[_foundIndex].col;
    }
    _found = [self matches];
    _foundIndex = [self matchAtOrBefore:anchorId col:anchorCol];
    [self setNeedsDisplay:YES];
    if (self.onFindChanged) self.onFindChanged();
}

- (void)endFind {
    _finding = NO;
    _found.clear();
    _foundIndex = -1;
    _findQuery.clear();
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(refreshFind)
                                               object:nil];
    _findRefreshQueued = NO;
    [self setNeedsDisplay:YES];
}

// The matches on one line, highlighted under the text: the current one
// strongly, the rest faintly (VS Code's find colors). A match that runs over
// a soft wrap is drawn on each of its rows. Matches never overlap, so their
// ends are in order as their starts are.
- (void)drawMatchesOnLine:(long long)lineId row:(int)r {
    if (_found.empty()) return;
    auto it = std::lower_bound(_found.begin(), _found.end(), lineId,
                               [](const Found &f, long long id) { return f.endId < id; });
    BOOL dark = [AppSettings shared].settings.isDark();
    int cols = self.screen ? self.screen->cols() : 0;
    for (; it != _found.end() && it->id <= lineId; ++it) {
        BOOL current = (NSInteger)(it - _found.begin()) == _foundIndex;
        NSColor *c = current
            ? (dark ? [NSColor colorWithSRGBRed:0x9E / 255.0 green:0x6A / 255.0 blue:0x03 / 255.0 alpha:1]
                    : [NSColor colorWithSRGBRed:0xF6 / 255.0 green:0xB9 / 255.0 blue:0x4D / 255.0 alpha:1])
            : [NSColor colorWithSRGBRed:0xEA / 255.0 green:0x5C / 255.0 blue:0x00 / 255.0 alpha:0.33];
        [c setFill];
        int c0 = it->id == lineId ? it->col : 0;
        int c1 = it->endId == lineId ? it->endCol : cols;
        if (c1 > c0)
            NSRectFillUsingOperation([self rectForRow:r col:c0 width:c1 - c0],
                                     NSCompositingOperationSourceOver);
    }
}

// ---------------------------------------------------------------- colors
struct CellColors {
    NSColor *fg;
    NSColor *bg;      // nil: the panel's own background shows through
};

static CellColors ColorsFor(const TermStyle &st) {
    const Settings &cfg = [AppSettings shared].settings;
    bool defaultFg = st.fg.kind == TermColor::Default;
    bool hasBg = st.bg.kind != TermColor::Default;
    Rgba fg = defaultFg ? cfg.text(Surface::Terminal) : Rgba::hex(st.fg.rgb());
    // Inverse text gets a solid background even on a see-through panel.
    Rgba bg = Rgba::hex(hasBg ? st.bg.rgb()
                              : cfg.background(Surface::Terminal).rgb());
    if (st.inverse) {
        std::swap(fg, bg);
        fg.a = 1.0;
        bg.a = 1.0;
        hasBg = true;
    }
    NSColor *f = MCColor(fg);
    if (st.dim) f = [f colorWithAlphaComponent:f.alphaComponent * 0.6];
    return {f, hasBg ? MCColor(bg) : nil};
}

- (NSDictionary *)attributesFor:(const TermStyle &)st fg:(NSColor *)fg {
    NSMutableDictionary *a = [NSMutableDictionary dictionary];
    a[NSFontAttributeName] = [TerminalGridView fontBold:st.bold italic:st.italic];
    a[NSForegroundColorAttributeName] = fg;
    if (st.underline) a[NSUnderlineStyleAttributeName] = @(NSUnderlineStyleSingle);
    if (st.strike) a[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);
    return a;
}

// ---------------------------------------------------------------- geometry
// Text an input method or dictation is still composing, drawn over the grid
// from the cursor, a character to a cell, wrapping at the right edge onto
// the rows below, as the program will wrap it once it arrives. Dictation
// keeps a whole sentence composing while you speak, and drawn as one line
// it ran off the edge. Past the bottom, the rows go up so its end shows.
- (void)drawMarkedTextAtRow:(int)row col:(int)col rows:(int)rows cols:(int)cols {
    if (cols <= 0 || rows <= 0) return;
    struct Piece { NSString *text; int width; };
    __block std::vector<Piece> pieces;
    [_marked enumerateSubstringsInRange:NSMakeRange(0, _marked.length)
                                options:NSStringEnumerationByComposedCharacterSequences
                             usingBlock:^(NSString *sub, NSRange r, NSRange e, BOOL *stop) {
        (void)r; (void)e; (void)stop;
        uint32_t cp = 0;
        [sub getBytes:&cp maxLength:4 usedLength:NULL encoding:NSUTF32LittleEndianStringEncoding
              options:0 range:NSMakeRange(0, sub.length) remainingRange:NULL];
        const BOOL newline = [sub isEqualToString:@"\n"] || [sub isEqualToString:@"\r\n"] ||
                             [sub isEqualToString:@"\r"];
        const int w = termCharWidth(cp);
        pieces.push_back({sub, newline ? -1 : (w <= 0 ? 1 : w)});   // -1: a new row
    }];
    // Where each piece goes, starting at the cursor.
    std::vector<std::pair<int, int>> at;
    int r = 0, c = col;
    for (const Piece &p : pieces) {
        if (p.width < 0) { r++; c = 0; at.push_back({-1, -1}); continue; }
        if (c + p.width > cols) { r++; c = 0; }
        at.push_back({r, c});
        c += p.width;
    }
    const int used = r + 1;
    const int first = MAX(0, MIN(row, rows - used));   // up, if it would pass the bottom
    NSColor *fg = [[AppSettings shared] text:Surface::Terminal];
    NSColor *bg = MCColor([AppSettings shared].settings.background(Surface::Terminal));
    NSDictionary *attrs = @{NSFontAttributeName: [TerminalGridView cellFont],
                            NSForegroundColorAttributeName: fg,
                            NSUnderlineStyleAttributeName: @(NSUnderlineStyleSingle)};
    // Each row's cells cleared first, so the grid's text never shows through.
    for (int k = 0; k < used; k++) {
        const int y = first + k;
        if (y >= rows) break;
        int from = k == 0 ? col : 0, to = from;
        for (size_t i = 0; i < pieces.size(); i++)
            if (at[i].first == k) to = MAX(to, at[i].second + pieces[i].width);
        if (to > from) {
            [bg setFill];
            NSRectFill([self rectForRow:y col:from width:to - from]);
        }
    }
    for (size_t i = 0; i < pieces.size(); i++) {
        if (at[i].first < 0 || first + at[i].first >= rows) continue;
        NSRect cell = [self rectForRow:first + at[i].first col:at[i].second width:pieces[i].width];
        [pieces[i].text drawAtPoint:cell.origin withAttributes:attrs];
    }
}

- (NSRect)rectForRow:(int)row col:(int)col width:(int)w {
    NSSize cs = [TerminalGridView cellSize];
    CGFloat pad = [TerminalGridView padding];
    return NSMakeRect(pad + col * cs.width, pad + row * cs.height,
                      w * cs.width, cs.height);
}

// The view row and column under a point, clamped onto the grid.
- (void)cellAtPoint:(NSPoint)p row:(int *)row col:(int *)col {
    NSSize cs = [TerminalGridView cellSize];
    CGFloat pad = [TerminalGridView padding];
    int r = (int)floor((p.y - pad) / cs.height);
    int c = (int)floor((p.x - pad) / cs.width);
    *row = MAX(0, MIN(r, (self.screen ? self.screen->rows() : 1) - 1));
    *col = MAX(0, MIN(c, (self.screen ? self.screen->cols() : 1) - 1));
}

// Selection ends in reading order.
- (void)selectionStart:(long long *)id0 col:(int *)c0 end:(long long *)id1 col:(int *)c1 {
    long long a = _anchorId, b = _endId;
    int ca = _anchorCol, cb = _endCol;
    if (b < a || (b == a && cb < ca)) { std::swap(a, b); std::swap(ca, cb); }
    *id0 = a; *c0 = ca; *id1 = b; *c1 = cb;
}

- (BOOL)isSelectedLine:(long long)lineId col:(int)c {
    if (!_hasSelection) return NO;
    long long r0, r1;
    int c0, c1;
    [self selectionStart:&r0 col:&c0 end:&r1 col:&c1];
    if (lineId < r0 || lineId > r1) return NO;
    if (lineId == r0 && c < c0) return NO;
    if (lineId == r1 && c > c1) return NO;
    return YES;
}

// ---------------------------------------------------------------- drawing
- (void)drawRect:(NSRect)dirty {
    TerminalScreen *s = self.screen;
    if (!s) return;
    NSSize cs = [TerminalGridView cellSize];
    CGFloat pad = [TerminalGridView padding];
    int rows = s->rows(), cols = s->cols();
    int firstRow = MAX(0, (int)floor((NSMinY(dirty) - pad) / cs.height));
    int lastRow = MIN(rows - 1, (int)ceil((NSMaxY(dirty) - pad) / cs.height));
    BOOL focused = self.window.isKeyWindow && self.window.firstResponder == self;
    NSColor *selColor = [NSColor selectedTextBackgroundColor];
    int top = [self topLine];
    long long base = s->historyBase();

    for (int r = firstRow; r <= lastRow; r++) {
        int line = top + r;
        long long lineId = base + line;
        // Backgrounds and the selection, merged into runs.
        int c = 0;
        while (c < cols) {
            const TermCell &cell = s->lineCell(line, c);
            NSColor *bg = [self isSelectedLine:lineId col:c] ? selColor
                                                             : ColorsFor(cell.style).bg;
            int start = c++;
            while (c < cols) {
                NSColor *next = [self isSelectedLine:lineId col:c] ? selColor
                                    : ColorsFor(s->lineCell(line, c).style).bg;
                if (next != bg && ![next isEqual:bg]) break;
                c++;
            }
            if (bg) {
                [bg setFill];
                NSRectFillUsingOperation([self rectForRow:r col:start width:c - start],
                                         NSCompositingOperationSourceOver);
            }
        }
        [self drawMatchesOnLine:lineId row:r];
        // Text: runs of plain ASCII in one style, other characters one by one.
        c = 0;
        while (c < cols) {
            const TermCell &cell = s->lineCell(line, c);
            if (cell.width == 0) { c++; continue; }
            bool ascii = cell.ch.size() == 1 && (unsigned char)cell.ch[0] < 0x80;
            int start = c;
            std::string text = cell.ch;
            c += cell.width == 2 ? 2 : 1;
            if (ascii) {
                while (c < cols) {
                    const TermCell &n = s->lineCell(line, c);
                    if (n.width != 1 || n.ch.size() != 1 ||
                        (unsigned char)n.ch[0] >= 0x80 || n.style != cell.style)
                        break;
                    text += n.ch;
                    c++;
                }
            }
            bool blank = text.find_first_not_of(' ') == std::string::npos;
            if (blank && !cell.style.underline && !cell.style.strike) continue;
            NSString *str = [[NSString alloc] initWithBytes:text.data()
                                                     length:text.size()
                                                   encoding:NSUTF8StringEncoding];
            if (!str) continue;
            NSDictionary *attrs = [self attributesFor:cell.style
                                                   fg:ColorsFor(cell.style).fg];
            [str drawAtPoint:NSMakePoint(pad + start * cs.width, pad + r * cs.height)
              withAttributes:attrs];
        }
    }

    [self drawCommandMarksFrom:firstRow to:lastRow];

    // The Cmd-hovered link, underlined in the text color, on each row it
    // covers.
    if (_linkId != kNoLine) {
        [[[AppSettings shared] text:Surface::Terminal] setFill];
        for (long long id = _linkId; id <= _linkEndId; id++) {
            int line = (int)(id - base);
            int linkRow = line - top;
            if (linkRow < firstRow || linkRow > lastRow) continue;
            int c0 = id == _linkId ? _linkCol0 : 0;
            int c1 = id == _linkEndId ? _linkCol1 : s->lineTextWidth(line);
            if (c1 <= c0) continue;
            NSRect r = [self rectForRow:linkRow col:c0 width:c1 - c0];
            NSRectFill(NSMakeRect(NSMinX(r), NSMaxY(r) - 1.5, NSWidth(r), 1));
        }
    }

    // The cursor: a block while focused, an outline otherwise. Text being
    // composed by an input method sits at the cursor, underlined.
    int cr = s->cursorRow() - top, cc = s->cursorCol();
    if (_marked.length && cr >= 0 && cr < rows) {
        [self drawMarkedTextAtRow:cr col:cc rows:rows cols:cols];
    } else if (s->cursorVisible() && cr >= firstRow && cr <= lastRow) {
        const TermCell &cell = s->cell(cr + top, cc);
        int w = cell.width == 2 ? 2 : 1;
        NSRect rect = [self rectForRow:cr col:cc width:w];
        NSColor *cursor = [[AppSettings shared] text:Surface::Terminal];
        if (focused) {
            [[cursor colorWithAlphaComponent:1.0] setFill];
            NSRectFill(rect);
            if (!cell.ch.empty() && cell.ch != " ") {
                NSColor *under = MCColor(Rgba::hex(
                    [AppSettings shared].settings.background(Surface::Terminal).rgb()));
                NSString *str = [[NSString alloc] initWithBytes:cell.ch.data()
                                                         length:cell.ch.size()
                                                       encoding:NSUTF8StringEncoding];
                [str drawAtPoint:rect.origin
                  withAttributes:[self attributesFor:cell.style fg:under]];
            }
        } else {
            [[cursor colorWithAlphaComponent:0.8] setStroke];
            NSBezierPath *p = [NSBezierPath bezierPathWithRect:NSInsetRect(rect, 0.5, 0.5)];
            p.lineWidth = 1;
            [p stroke];
        }
    }

    // Scrolled back: a slim bar at the right edge says where in the
    // scrollback the view is.
    int hist = [self maxOffset];
    if (_offset > 0 && hist > 0) {
        CGFloat H = self.bounds.size.height - 4;
        CGFloat total = hist + rows;
        CGFloat h = MAX(20, H * rows / total);
        CGFloat y = 2 + (H - h) * (CGFloat)(hist - _offset) / hist;
        NSRect bar = NSMakeRect(self.bounds.size.width - 6, y, 4, h);
        [[[[AppSettings shared] text:Surface::Terminal] colorWithAlphaComponent:0.35] setFill];
        [[NSBezierPath bezierPathWithRoundedRect:bar xRadius:2 yRadius:2] fill];
    }
    if (_secureOn) [self drawSecureInputLock];
}

// ---------------------------------------------------------------- keys
// Keys and pastes: what the user typed, which onTyped hears of first.
- (void)sendTyped:(const std::string &)bytes {
    if (bytes.empty()) return;
    if (self.onTyped) self.onTyped();
    [self send:bytes];
}

- (void)send:(const std::string &)bytes {
    if (bytes.empty() || !self.onInput) return;
    [self scrollToBottom];   // typing and pasting go back to the live screen
    self.onInput([NSData dataWithBytes:bytes.data() length:bytes.size()]);
}

// Grid mode: Control with a letter, a symbol or Space belongs to the shell
// (Ctrl+R, Ctrl+W, Ctrl+Space for set-mark), even where a menu item has the
// same key (Complete is Ctrl+Space in the editor). The window asks its views
// before the menu, so taking them here keeps them. Ctrl+Tab, Ctrl+Return,
// Ctrl+digits and anything with Shift or Command stay the app's.
- (BOOL)performKeyEquivalent:(NSEvent *)e {
    NSEventModifierFlags f = e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    if (self.textInput && self.window.firstResponder == self &&
        (f & NSEventModifierFlagControl) &&
        !(f & (NSEventModifierFlagCommand | NSEventModifierFlagShift)) &&
        e.keyCode != kVK_Tab && e.keyCode != kVK_Return && e.keyCode != kVK_ANSI_KeypadEnter) {
        NSString *base = e.charactersIgnoringModifiers;
        unichar c = base.length ? [base characterAtIndex:0] : 0;
        if (!(c >= '0' && c <= '9') && !(c >= 0xF700 && c <= 0xF8FF)) {
            [self keyDown:e];
            return YES;
        }
    }
    // Ctrl with Shift or Option is the app's when a menu item that can act
    // has it, and the program's otherwise: a disabled item still takes its
    // key, so the editor's Delete Line (Ctrl+Shift+K) swallowed it here.
    if (self.textInput && self.window.firstResponder == self &&
        (f & NSEventModifierFlagControl) && !(f & NSEventModifierFlagCommand) &&
        (f & (NSEventModifierFlagShift | NSEventModifierFlagOption))) {
        NSMenuItem *item = MCMenuItemForKey(NSApp.mainMenu, e);
        if (item) {
            [item.menu update];   // validation against the grid's responder chain
            if (!item.enabled) {
                [self keyDown:e];
                return YES;
            }
        }
    }
    return [super performKeyEquivalent:e];
}

// The menu item, enabled or not, whose key equivalent is this key event's
// (Command, Control, Option and Shift compared; nil when none).
static NSMenuItem *MCMenuItemForKey(NSMenu *menu, NSEvent *e) {
    const NSEventModifierFlags mods = NSEventModifierFlagCommand | NSEventModifierFlagControl |
                                      NSEventModifierFlagOption | NSEventModifierFlagShift;
    const NSEventModifierFlags flags = e.modifierFlags & mods;
    NSString *key = e.charactersIgnoringModifiers.lowercaseString;
    // The key without Shift too: Ctrl+Shift+- reads "_", its item says "-".
    NSString *plain = e.type == NSEventTypeKeyDown
        ? [e charactersByApplyingModifiers:0].lowercaseString : nil;
    if (!key.length && !plain.length) return nil;
    for (NSMenuItem *item in menu.itemArray) {
        if (item.submenu) {
            NSMenuItem *found = MCMenuItemForKey(item.submenu, e);
            if (found) return found;
            continue;
        }
        NSString *ke = item.keyEquivalent;
        if (!ke.length || item.isAlternate) continue;
        NSEventModifierFlags m = item.keyEquivalentModifierMask & mods;
        // "K" in a key equivalent means Shift+K.
        if (![ke.lowercaseString isEqualToString:ke]) m |= NSEventModifierFlagShift;
        NSString *k = ke.lowercaseString;
        if (m == flags && ([k isEqualToString:key] || [k isEqualToString:plain])) return item;
    }
    return nil;
}

- (void)keyDown:(NSEvent *)e {
    TerminalScreen *s = self.screen;
    NSEventModifierFlags f = e.modifierFlags;
    if (!s) { [super keyDown:e]; return; }
    if (f & NSEventModifierFlagCommand) {
        // Cmd+Home and Cmd+End: the top and the bottom of the scrollback, as
        // in Terminal.app. Other Cmd keys the menu did not take beep.
        if (e.keyCode == kVK_Home) { [self setOffset:[self maxOffset]]; return; }
        if (e.keyCode == kVK_End) { [self scrollToBottom]; return; }
        // Cmd+Up and Cmd+Down to the previous and next command, Shift to
        // select (VS Code's scrollToPreviousCommand and selectToPreviousCommand).
        // A full-screen program keeps its keys.
        if (self.textInput && !s->altScreen() &&
            (e.keyCode == kVK_UpArrow || e.keyCode == kVK_DownArrow) &&
            !(f & (NSEventModifierFlagOption | NSEventModifierFlagControl))) {
            [self moveToCommand:e.keyCode == kVK_UpArrow ? -1 : 1
                         select:(f & NSEventModifierFlagShift) != 0];
            return;
        }
        // Cmd+Left, Cmd+Right and Cmd+Delete edit the line at the shell's
        // prompt (or a REPL's, Claude Code's), as in VS Code and iTerm:
        // its start, its end, and delete back to its start. A full-screen
        // program keeps its own keys.
        NSEventModifierFlags others = f & (NSEventModifierFlagShift | NSEventModifierFlagOption |
                                          NSEventModifierFlagControl);
        if (self.textInput && !others && !s->altScreen() && self.onLineEdit &&
            (e.keyCode == kVK_LeftArrow || e.keyCode == kVK_RightArrow ||
             e.keyCode == kVK_Delete)) {
            if (_hasSelection) { _hasSelection = NO; [self setNeedsDisplay:YES]; }
            [self scrollToBottom];
            self.onLineEdit(e.keyCode == kVK_LeftArrow ? MCLineEditStart
                            : e.keyCode == kVK_RightArrow ? MCLineEditEnd
                                                          : MCLineEditDeleteToStart);
            return;
        }
        [super keyDown:e];
        return;
    }
    // An input method composing gets every key until it is done.
    if (self.textInput && _marked.length) {
        [self interpretKeyEvents:@[e]];
        return;
    }
    int mods = ((f & NSEventModifierFlagShift) ? TermModShift : 0) |
               ((f & NSEventModifierFlagOption) ? TermModAlt : 0) |
               ((f & NSEventModifierFlagControl) ? TermModCtrl : 0);
    // Shift+Page Up / Page Down page through the scrollback (on the main
    // screen; a full-screen program gets them).
    if (mods == TermModShift && !s->altScreen() &&
        (e.keyCode == kVK_PageUp || e.keyCode == kVK_PageDown)) {
        int page = MAX(1, s->rows() - 1);
        [self setOffset:_offset + (e.keyCode == kVK_PageUp ? page : -page)];
        return;
    }
    if (_hasSelection) { _hasSelection = NO; [self setNeedsDisplay:YES]; }
    [NSCursor setHiddenUntilMouseMoves:YES];

    // Option+Left and Right move by words at the shell's prompt, as they do
    // in Terminal.app (zsh binds ESC b and ESC f, not xterm's Alt+arrows).
    if (self.textInput && mods == TermModAlt && !s->altScreen() &&
        (e.keyCode == kVK_LeftArrow || e.keyCode == kVK_RightArrow)) {
        [self sendTyped:e.keyCode == kVK_LeftArrow ? "\x1b" "b" : "\x1b" "f"];
        return;
    }

    static const struct { unsigned short code; TermKey key; } keys[] = {
        {kVK_UpArrow, TermKey::Up}, {kVK_DownArrow, TermKey::Down},
        {kVK_RightArrow, TermKey::Right}, {kVK_LeftArrow, TermKey::Left},
        {kVK_Home, TermKey::Home}, {kVK_End, TermKey::End},
        {kVK_PageUp, TermKey::PageUp}, {kVK_PageDown, TermKey::PageDown},
        {kVK_Help, TermKey::Insert}, {kVK_ForwardDelete, TermKey::Delete},
        {kVK_F1, TermKey::F1}, {kVK_F2, TermKey::F2}, {kVK_F3, TermKey::F3},
        {kVK_F4, TermKey::F4}, {kVK_F5, TermKey::F5}, {kVK_F6, TermKey::F6},
        {kVK_F7, TermKey::F7}, {kVK_F8, TermKey::F8}, {kVK_F9, TermKey::F9},
        {kVK_F10, TermKey::F10}, {kVK_F11, TermKey::F11}, {kVK_F12, TermKey::F12},
        {kVK_Return, TermKey::Enter}, {kVK_ANSI_KeypadEnter, TermKey::KeypadEnter},
        {kVK_Delete, TermKey::Backspace}, {kVK_Tab, TermKey::Tab},
        {kVK_Escape, TermKey::Escape},
    };
    int keyMods = self.optionAsMeta ? mods : (mods & ~TermModAlt);
    for (const auto &k : keys) {
        if (k.code == e.keyCode) {
            [self sendTyped:TerminalScreen::encodeKey(k.key, keyMods, s->appCursorKeys(),
                                                 s->appKeypad())];
            return;
        }
    }

    BOOL meta = (mods & TermModAlt) && self.optionAsMeta;
    if ((mods & TermModCtrl) || meta) {
        // Control codes and Meta come from the key itself, not from what
        // Option would have typed (Option+b is ESC b, not ∫).
        int charMods = (mods & TermModCtrl) | (meta ? TermModAlt : 0);
        NSString *base = e.charactersIgnoringModifiers;
        std::string out;
        NSUInteger n = base.length;
        for (NSUInteger i = 0; i < n; ) {
            NSRange r = [base rangeOfComposedCharacterSequenceAtIndex:i];
            NSString *one = [base substringWithRange:r];
            uint32_t cp = 0;
            [one getBytes:&cp maxLength:4 usedLength:NULL
                 encoding:NSUTF32LittleEndianStringEncoding options:0
                    range:NSMakeRange(0, one.length) remainingRange:NULL];
            if (cp >= 0xF700 && cp <= 0xF8FF) return;   // unmapped function key
            out += TerminalScreen::encodeChar(cp, charMods);
            i = NSMaxRange(r);
        }
        [self sendTyped:out];
        return;
    }

    if (self.textInput) {        // through the input system: dead keys, IMEs
        [self interpretKeyEvents:@[e]];
        return;
    }
    NSString *chars = e.characters;
    if (!chars.length) return;
    unichar first = [chars characterAtIndex:0];
    if (first >= 0xF700 && first <= 0xF8FF) return;      // unmapped function key
    const char *utf8 = chars.UTF8String;
    if (utf8) [self sendTyped:utf8];
}

// ------------------------------------------------------- NSTextInputClient
// Only used with textInput on (keyDown: calls interpretKeyEvents: then).
- (void)insertText:(id)string replacementRange:(NSRange)range {
    (void)range;
    NSString *str = [string isKindOfClass:[NSAttributedString class]]
        ? [(NSAttributedString *)string string] : string;
    BOOL hadMarked = _marked.length > 0;
    _marked = nil;
    if (hadMarked) [self setNeedsDisplay:YES];
    const char *utf8 = str.UTF8String;
    if (utf8 && *utf8) [self sendTyped:utf8];
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)sel
     replacementRange:(NSRange)range {
    (void)range;
    _marked = [[string isKindOfClass:[NSAttributedString class]]
        ? [(NSAttributedString *)string string] : string copy];
    _markedSel = sel;
    if (_marked.length) [self scrollToBottom];
    [self setNeedsDisplay:YES];
}

- (void)unmarkText {
    _marked = nil;
    [self setNeedsDisplay:YES];
}

- (BOOL)hasMarkedText { return _marked.length > 0; }
- (NSRange)markedRange {
    return _marked.length ? NSMakeRange(0, _marked.length) : NSMakeRange(NSNotFound, 0);
}
- (NSRange)selectedRange {
    return _marked.length ? _markedSel : NSMakeRange(0, 0);
}
- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText { return @[]; }
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range
                                                actualRange:(NSRangePointer)actual {
    (void)range; (void)actual;
    return nil;
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    (void)point;
    return NSNotFound;
}
// The cursor's cell on screen, where an input method puts its candidates.
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actual {
    (void)range; (void)actual;
    TerminalScreen *s = self.screen;
    if (!s || !self.window) return NSZeroRect;
    NSRect r = [self rectForRow:s->cursorRow() + _offset col:s->cursorCol() width:1];
    return [self.window convertRectToScreen:[self convertRect:r toView:nil]];
}
// Keys the input system turned into commands were all mapped in keyDown:
// already; anything else is not for the terminal (and must not beep).
- (void)doCommandBySelector:(SEL)selector { (void)selector; }

// ---------------------------------------------------------------- links
// Soft-wrapped rows are looked through as one line, this far each way.
static const int kMaxJoinedRows = 64;

// The first and last line of the soft-wrapped line holding `line`.
- (void)logicalLineAt:(int)line first:(int *)first last:(int *)last {
    TerminalScreen *s = self.screen;
    int lo = -s->historySize(), hi = s->rows() - 1;
    int a = line, b = line;
    while (a > lo && line - a < kMaxJoinedRows && s->lineWraps(a - 1)) a--;
    while (b < hi && b - line < kMaxJoinedRows && s->lineWraps(b)) b++;
    *first = a;
    *last = b;
}

// A line's text as UTF-8, rows joined where they wrapped, with the line and
// cell each byte came from. The right half of a wide character contributes
// no bytes, so byte offsets from TermLinks map back to the cells a link
// covers.
struct CellAt { int line, col; };
- (std::string)textOfLines:(int)first to:(int)last cells:(std::vector<CellAt> *)cellOfByte {
    std::string text;
    TerminalScreen *s = self.screen;
    for (int line = first; line <= last; line++) {
        int width = line < last ? s->lineTextWidth(line) : s->cols();
        for (int c = 0; c < width; c++) {
            const TermCell cell = s->lineCell(line, c);
            if (cell.width == 0) continue;
            const std::string ch = cell.ch.empty() ? std::string(" ") : cell.ch;
            text += ch;
            cellOfByte->insert(cellOfByte->end(), ch.size(), CellAt{line, c});
        }
    }
    return text;
}

// What an OSC 8 hyperlink's address opens (TermLinks::hyperlink): a web
// address, or a file or folder on this Mac that exists; nil otherwise.
static MCTermLink *MCHyperlinkFor(const std::string &uri) {
    char host[256] = "";
    gethostname(host, sizeof host - 1);
    TermLinks::Link l;
    if (!TermLinks::hyperlink(uri, host, l)) return nil;
    NSString *target = [[NSString alloc] initWithBytes:l.target.data() length:l.target.size()
                                              encoding:NSUTF8StringEncoding];
    if (!target.length) return nil;
    MCTermLink *link = [MCTermLink new];
    if (l.kind == TermLinks::Link::Url) {
        link.isURL = YES;
        link.target = target;
        return link;
    }
    BOOL dir = NO;
    if (![NSFileManager.defaultManager fileExistsAtPath:target isDirectory:&dir]) return nil;
    link.target = target.stringByStandardizingPath;
    link.isDirectory = dir;
    return link;
}

// The link at a point, with the line and cell it starts at and the line and
// cell after its end, or nil. Points outside the grid find nothing
// (cellAtPoint: would clamp them onto the edge cells).
- (MCTermLink *)linkAtPoint:(NSPoint)p line:(int *)lineOut col0:(int *)col0
                    endLine:(int *)endLineOut col1:(int *)col1 {
    TerminalScreen *s = self.screen;
    if (!s || !self.linkAt) return nil;
    NSSize cs = [TerminalGridView cellSize];
    CGFloat pad = [TerminalGridView padding];
    int r = (int)floor((p.y - pad) / cs.height);
    int c = (int)floor((p.x - pad) / cs.width);
    if (r < 0 || r >= s->rows() || c < 0 || c >= s->cols()) return nil;
    int line = [self topLine] + r;
    if (s->lineCell(line, c).width == 0 && c > 0) c--;   // right half of a wide character
    int first, last;
    [self logicalLineAt:line first:&first last:&last];
    _hyperlinkAddress = nil;
    // An OSC 8 hyperlink comes first: it spans the cells around the point
    // that carry its id, within the soft-wrapped line, whatever they say.
    if (uint32_t id = s->lineCell(line, c).style.link) {
        MCTermLink *link = MCHyperlinkFor(s->linkURI(id));
        if (!link) return nil;
        std::vector<CellAt> cells;
        size_t at = 0;
        for (int l = first; l <= last; l++) {
            int width = l < last ? s->lineTextWidth(l) : s->cols();
            for (int col = 0; col < width; col++) {
                if (s->lineCell(l, col).width == 0) continue;
                if (l == line && col == c) at = cells.size();
                cells.push_back(CellAt{l, col});
            }
        }
        if (cells.empty()) return nil;
        auto idAt = [&](size_t i) { return s->lineCell(cells[i].line, cells[i].col).style.link; };
        size_t a = at, z = at;
        while (a > 0 && idAt(a - 1) == id) a--;
        while (z + 1 < cells.size() && idAt(z + 1) == id) z++;
        *lineOut = cells[a].line;
        *col0 = cells[a].col;
        *endLineOut = cells[z].line;
        *col1 = cells[z].col + (s->lineCell(cells[z].line, cells[z].col).width == 2 ? 2 : 1);
        _hyperlinkAddress = @(s->linkURI(id).c_str());
        return link;
    }
    std::vector<CellAt> cellOfByte;
    std::string text = [self textOfLines:first to:last cells:&cellOfByte];
    size_t byte = 0;
    while (byte < cellOfByte.size() &&
           (cellOfByte[byte].line < line || (cellOfByte[byte].line == line && cellOfByte[byte].col < c)))
        byte++;
    if (byte >= cellOfByte.size() || cellOfByte[byte].line != line) return nil;
    MCTermLink *link = self.linkAt(text, byte);
    if (!link || link.byteLength == 0 ||
        link.byteStart + link.byteLength > cellOfByte.size()) return nil;
    size_t lastByte = link.byteStart + link.byteLength - 1;
    CellAt a = cellOfByte[link.byteStart], z = cellOfByte[lastByte];
    *lineOut = a.line;
    *col0 = a.col;
    *endLineOut = z.line;
    *col1 = z.col + (s->lineCell(z.line, z.col).width == 2 ? 2 : 1);
    return link;
}

- (void)updateLinkHover {
    long long lineId = kNoLine, endId = kNoLine;
    int line = 0, endLine = 0, c0 = 0, c1 = 0;
    MCTermLink *link = nil;
    if ((NSEvent.modifierFlags & NSEventModifierFlagCommand) && self.window) {
        NSPoint p = [self convertPoint:[self.window mouseLocationOutsideOfEventStream]
                              fromView:nil];
        if (NSPointInRect(p, self.bounds))
            link = [self linkAtPoint:p line:&line col0:&c0 endLine:&endLine col1:&c1];
    }
    if (link) { lineId = [self base] + line; endId = [self base] + endLine; }
    else c0 = c1 = 0;
    BOOL changed = lineId != _linkId || endId != _linkEndId || c0 != _linkCol0 || c1 != _linkCol1;
    _link = link;
    _linkId = lineId; _linkEndId = endId; _linkCol0 = c0; _linkCol1 = c1;
    // A hyperlink's text may say anything: its address shows as the tooltip.
    NSString *tip = link ? _hyperlinkAddress : nil;
    if (tip != self.toolTip && ![tip isEqualToString:self.toolTip]) self.toolTip = tip;
    if (link) [[NSCursor pointingHandCursor] set];
    else if (changed) [[NSCursor arrowCursor] set];
    if (changed) [self setNeedsDisplay:YES];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea *a in self.trackingAreas)
        if (a.owner == self) [self removeTrackingArea:a];
    [self addTrackingArea:[[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                     NSTrackingCursorUpdate | NSTrackingActiveInKeyWindow |
                     NSTrackingInVisibleRect
               owner:self userInfo:nil]];
}

- (void)mouseMoved:(NSEvent *)e {
    [self updateLinkHover];
    // DECSET 1003: motion with no button down too.
    TerminalScreen *s = self.screen;
    if (s && s->mouseTracking() == 1003 && _reportButton < 0 && [self reportsMouse:e] &&
        NSPointInRect([self convertPoint:e.locationInWindow fromView:nil], self.bounds))
        [self report:TermMouse::Motion button:-1 event:e];
}
- (void)mouseExited:(NSEvent *)e {
    (void)e;
    if (_linkId != kNoLine) { _link = nil; _linkId = _linkEndId = kNoLine; [self setNeedsDisplay:YES]; }
}
- (void)cursorUpdate:(NSEvent *)e {
    (void)e;
    [(_linkId != kNoLine ? [NSCursor pointingHandCursor] : [NSCursor arrowCursor]) set];
}

// ---------------------------------------------------------------- mouse
// Characters a double-click takes as part of a word: anything but blanks,
// brackets, quotes and the shell's separators, so a path or a URL is one.
static BOOL IsWordCell(const TermCell &cell) {
    if (cell.ch.empty() || cell.ch == " ") return NO;
    if (cell.ch.size() > 1) return YES;
    return strchr("()[]{}<>\"'`|;,", cell.ch[0]) == nullptr;
}

// The word (unit 2) or line (unit 3) around a cell, from (fromLine, from) to
// (toLine, to) inclusive. Both run over soft wraps: a triple-click takes the
// whole wrapped line, a double-click a path the terminal wrapped.
- (void)unit:(int)unit atLine:(int)line col:(int)col
    fromLine:(int *)fromLine from:(int *)from toLine:(int *)toLine to:(int *)to {
    TerminalScreen *s = self.screen;
    int first, last;
    [self logicalLineAt:line first:&first last:&last];
    if (unit == 3) {
        *fromLine = first; *from = 0;
        *toLine = last; *to = s->cols() - 1;
        return;
    }
    if (s->lineCell(line, col).width == 0 && col > 0) col--;
    *fromLine = *toLine = line;
    if (!IsWordCell(s->lineCell(line, col))) { *from = *to = col; return; }
    auto wordAt = [&](int l, int c) {
        TermCell cell = s->lineCell(l, c);
        return cell.width == 0 || IsWordCell(cell);
    };
    int al = line, a = col, bl = line, b = col;
    for (;;) {
        if (a > 0 && wordAt(al, a - 1)) { a--; continue; }
        if (a == 0 && al > first) {
            int w = s->lineTextWidth(al - 1);
            if (w > 0 && wordAt(al - 1, w - 1)) { al--; a = w - 1; continue; }
        }
        break;
    }
    for (;;) {
        int w = bl < last ? s->lineTextWidth(bl) : s->cols();
        if (b + 1 < w && wordAt(bl, b + 1)) { b++; continue; }
        if (b + 1 >= w && bl < last && wordAt(bl + 1, 0)) { bl++; b = 0; continue; }
        break;
    }
    *fromLine = al; *from = a;
    *toLine = bl; *to = b;
}

- (void)mouseDown:(NSEvent *)e {
    [self forgetCommand];
    if (e.modifierFlags & NSEventModifierFlagCommand) {
        int line, endLine, c0, c1;
        NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
        MCTermLink *link = [self linkAtPoint:p line:&line col0:&c0 endLine:&endLine col1:&c1];
        if (link) {
            _link = nil;
            _linkId = _linkEndId = kNoLine;
            [self setNeedsDisplay:YES];
            if (self.openLink) self.openLink(link);
            return;
        }
    }
    [self.window makeFirstResponder:self];
    if (!self.screen) return;
    if ([self reportsMouse:e]) {
        if (_hasSelection) { _hasSelection = NO; [self setNeedsDisplay:YES]; }
        _reportButton = 0;
        [self report:TermMouse::Press button:0 event:e];
        return;
    }
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    int row, col;
    [self cellAtPoint:p row:&row col:&col];
    int line = [self topLine] + row;
    long long lineId = [self base] + line;
    _unit = e.clickCount >= 3 ? 3 : e.clickCount == 2 ? 2 : 1;
    if (_unit > 1) {
        int fromLine, from, toLine, to;
        [self unit:_unit atLine:line col:col fromLine:&fromLine from:&from toLine:&toLine to:&to];
        long long base = [self base];
        _unitStartId = base + fromLine;
        _unitEndId = base + toLine;
        _unitStartCol = from;
        _unitEndCol = to;
        _anchorId = _unitStartId; _anchorCol = from;
        _endId = _unitEndId; _endCol = to;
        _hasSelection = YES;
    } else {
        _anchorId = _endId = lineId;
        _anchorCol = _endCol = col;
        _hasSelection = NO;
    }
    _selecting = YES;
    [self setNeedsDisplay:YES];
}

// ---------------------------------------------------------- mouse reporting
// A mouse event goes to the program when it asked for mouse events, the view
// shows the live screen, and neither Shift (select, as in other terminals)
// nor Cmd (links) is held.
- (BOOL)reportsMouse:(NSEvent *)e {
    TerminalScreen *s = self.screen;
    if (!s || !s->mouseTracking() || _offset != 0) return NO;
    return !(e.modifierFlags & (NSEventModifierFlagShift | NSEventModifierFlagCommand));
}

// Sends one mouse event at the event's cell; motion only when the cell
// changed. Option is Meta and Control is Control, as xterm reports them.
- (void)report:(TermMouse)kind button:(int)button event:(NSEvent *)e {
    TerminalScreen *s = self.screen;
    if (!s) return;
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    int row, col;
    [self cellAtPoint:p row:&row col:&col];
    if (kind == TermMouse::Motion && row == _reportRow && col == _reportCol) return;
    _reportRow = row;
    _reportCol = col;
    NSEventModifierFlags f = e.modifierFlags;
    int mods = ((f & NSEventModifierFlagOption) ? TermModAlt : 0) |
               ((f & NSEventModifierFlagControl) ? TermModCtrl : 0);
    std::string bytes = s->encodeMouse(kind, button, col, row, mods);
    if (!bytes.empty() && self.onInput)
        self.onInput([NSData dataWithBytes:bytes.data() length:bytes.size()]);
}

- (void)rightMouseDown:(NSEvent *)e {
    if (![self reportsMouse:e]) { [super rightMouseDown:e]; return; }
    [self.window makeFirstResponder:self];
    _reportButton = 2;
    [self report:TermMouse::Press button:2 event:e];
}

- (void)rightMouseDragged:(NSEvent *)e {
    if (_reportButton == 2) [self report:TermMouse::Motion button:2 event:e];
    else [super rightMouseDragged:e];
}

- (void)rightMouseUp:(NSEvent *)e {
    if (_reportButton != 2) { [super rightMouseUp:e]; return; }
    [self report:TermMouse::Release button:2 event:e];
    _reportButton = -1;
}

- (void)otherMouseDown:(NSEvent *)e {
    if (e.buttonNumber != 2 || ![self reportsMouse:e]) { [super otherMouseDown:e]; return; }
    [self.window makeFirstResponder:self];
    _reportButton = 1;
    [self report:TermMouse::Press button:1 event:e];
}

- (void)otherMouseDragged:(NSEvent *)e {
    if (_reportButton == 1) [self report:TermMouse::Motion button:1 event:e];
    else [super otherMouseDragged:e];
}

- (void)otherMouseUp:(NSEvent *)e {
    if (_reportButton != 1) { [super otherMouseUp:e]; return; }
    [self report:TermMouse::Release button:1 event:e];
    _reportButton = -1;
}

- (void)mouseDragged:(NSEvent *)e {
    if (_reportButton == 0) { [self report:TermMouse::Motion button:0 event:e]; return; }
    if (!_selecting || !self.screen) return;
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    // Past the top or bottom edge, the view scrolls through the scrollback.
    NSSize cs = [TerminalGridView cellSize];
    if (p.y < 0) [self setOffset:_offset + MAX(1, (int)(-p.y / cs.height))];
    else if (p.y > self.bounds.size.height)
        [self setOffset:_offset - MAX(1, (int)((p.y - self.bounds.size.height) / cs.height))];
    int row, col;
    [self cellAtPoint:p row:&row col:&col];
    int line = [self topLine] + row;
    long long lineId = [self base] + line;
    if (_unit == 1) {
        _endId = lineId;
        _endCol = col;
        _hasSelection = _endId != _anchorId || _endCol != _anchorCol;
    } else {
        int fromLine, from, toLine, to;
        [self unit:_unit atLine:line col:col fromLine:&fromLine from:&from toLine:&toLine to:&to];
        long long base = [self base];
        long long fromId = base + fromLine, toId = base + toLine;
        BOOL before = fromId < _unitStartId || (fromId == _unitStartId && from < _unitStartCol);
        if (before) {
            _anchorId = _unitEndId; _anchorCol = _unitEndCol;
            _endId = fromId; _endCol = from;
        } else {
            _anchorId = _unitStartId; _anchorCol = _unitStartCol;
            _endId = toId; _endCol = to;
        }
        _hasSelection = YES;
    }
    [self setNeedsDisplay:YES];
}

- (void)mouseUp:(NSEvent *)e {
    if (_reportButton == 0) {
        [self report:TermMouse::Release button:0 event:e];
        _reportButton = -1;
        return;
    }
    _selecting = NO;
}

// On the main screen the wheel scrolls back through the scrollback. In the
// alternate screen it scrolls the program (arrow keys), the way xterm's
// alternateScroll does; there is no scrollback there. A program that asked
// for mouse events gets the wheel as buttons 4 and 5 instead (Shift keeps
// the old behavior).
- (void)scrollWheel:(NSEvent *)e {
    TerminalScreen *s = self.screen;
    if (!s) return;
    if ([self reportsMouse:e]) {
        CGFloat lines = e.hasPreciseScrollingDeltas
            ? e.scrollingDeltaY / [TerminalGridView cellSize].height
            : e.scrollingDeltaY;
        _scrollAccum += lines;
        _reportRow = _reportCol = -1;
        while (_scrollAccum >= 1) {
            [self report:TermMouse::WheelUp button:-1 event:e];
            _scrollAccum -= 1;
        }
        while (_scrollAccum <= -1) {
            [self report:TermMouse::WheelDown button:-1 event:e];
            _scrollAccum += 1;
        }
        return;
    }
    if (!s->altScreen()) {
        CGFloat lines = e.hasPreciseScrollingDeltas
            ? e.scrollingDeltaY / [TerminalGridView cellSize].height
            : e.scrollingDeltaY * 3;
        _scrollAccum += lines;
        int whole = (int)_scrollAccum;   // toward zero
        if (whole) {
            _scrollAccum -= whole;
            [self setOffset:_offset + whole];
        }
        return;
    }
    CGFloat lines = e.hasPreciseScrollingDeltas
        ? e.scrollingDeltaY / [TerminalGridView cellSize].height
        : e.scrollingDeltaY;
    _scrollAccum += lines;
    std::string out;
    while (_scrollAccum >= 1) {
        out += TerminalScreen::encodeKey(TermKey::Up, 0, s->appCursorKeys());
        _scrollAccum -= 1;
    }
    while (_scrollAccum <= -1) {
        out += TerminalScreen::encodeKey(TermKey::Down, 0, s->appCursorKeys());
        _scrollAccum += 1;
    }
    [self send:out];
}

// ---------------------------------------------------------------- edit menu
- (NSString *)selectedText {
    if (!_hasSelection || !self.screen) return nil;
    long long id0, id1;
    int c0, c1;
    [self selectionStart:&id0 col:&c0 end:&id1 col:&c1];
    long long base = [self base];
    std::string text = self.screen->textBetween((int)(id0 - base), c0,
                                                (int)(id1 - base), c1);
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return [[NSString alloc] initWithBytes:text.data() length:text.size()
                                  encoding:NSUTF8StringEncoding];
}

- (void)copy:(id)sender {
    (void)sender;
    NSString *str = [self selectedText];
    if (!str) return;
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:str forType:NSPasteboardTypeString];
}

- (void)paste:(id)sender {
    (void)sender;
    NSString *str = [[NSPasteboard generalPasteboard]
                     stringForType:NSPasteboardTypeString];
    const char *utf8 = str.UTF8String;
    if (!utf8 || !self.screen) return;
    const bool bracketed = self.screen->bracketedPaste();
    TerminalScreen::PasteCheck check = TerminalScreen::checkPaste(
        utf8, bracketed, [AppSettings shared].settings.terminalPasteWarning());
    std::string text = check.text;
    if (check.ask) {
        switch ([self askBeforePasting:check]) {
        case NSAlertFirstButtonReturn: break;
        case NSAlertSecondButtonReturn: text = TerminalScreen::pasteAsOneLine(text); break;
        default: return;
        }
        // The screen may have gone, or the program changed, while asking.
        if (!self.screen) return;
    }
    [self sendTyped:TerminalScreen::encodePaste(text, self.screen->bracketedPaste())];
}

// VS Code's question (terminal.integrated.enableMultiLinePasteWarning):
// Paste, Paste as One Line, or Cancel, with a preview of the first lines.
// "Do not ask again" writes terminal.multiline-paste-warning = never.
- (NSModalResponse)askBeforePasting:(const TerminalScreen::PasteCheck &)check {
    NSAlert *alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = [NSString stringWithFormat:@"Paste %d lines into the terminal?", check.lines];
    NSString *preview = [[NSString alloc] initWithBytes:check.preview.data()
                                                 length:check.preview.size()
                                               encoding:NSUTF8StringEncoding] ?: @"";
    NSString *why = self.screen && self.screen->bracketedPaste()
        ? @""
        : @"The program running here may run each line as it arrives.\n\n";
    alert.informativeText = [NSString stringWithFormat:@"%@Preview:\n%@", why, preview];
    [alert addButtonWithTitle:@"Paste"];
    [alert addButtonWithTitle:@"Paste as One Line"];
    [alert addButtonWithTitle:@"Cancel"];
    alert.showsSuppressionButton = YES;
    alert.suppressionButton.title = @"Do not ask again";
    NSModalResponse r = [alert runModal];
    if ((r == NSAlertFirstButtonReturn || r == NSAlertSecondButtonReturn) &&
        alert.suppressionButton.state == NSControlStateValueOn)
        [[AppSettings shared] writeValue:@"never" forKey:@"terminal.multiline-paste-warning"];
    return r;
}

// Everything: the scrollback and the screen.
- (void)selectAll:(id)sender {
    (void)sender;
    TerminalScreen *s = self.screen;
    if (!s) return;
    long long base = s->historyBase();
    _anchorId = base - (s->altScreen() ? 0 : s->historySize());
    _anchorCol = 0;
    _endId = base + s->rows() - 1;
    _endCol = s->cols() - 1;
    _hasSelection = YES;
    [self setNeedsDisplay:YES];
}

- (void)clearScrollback:(id)sender {
    (void)sender;
    if (!self.screen) return;
    self.screen->clearHistory();
    [self screenChanged];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(copy:)) return _hasSelection;
    if (item.action == @selector(paste:))
        return [[NSPasteboard generalPasteboard]
                   stringForType:NSPasteboardTypeString] != nil;
    if (item.action == @selector(selectAll:)) return YES;
    if (item.action == @selector(clearScrollback:))
        return self.screen && self.screen->historySize() > 0;
    return NO;
}

@end
