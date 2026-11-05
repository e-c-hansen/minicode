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
// text they were on.
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
#import <Carbon/Carbon.h>   // kVK_* key codes (header only, nothing linked)
#include <climits>
#include <utility>
#include <vector>
#include "TerminalStream.h"   // termCharWidth

static const long long kNoLine = LLONG_MIN;

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
    // The link under the pointer while Cmd is held: its line and the cells
    // it covers, [_linkCol0, _linkCol1). _linkId is kNoLine when none.
    MCTermLink *_link;
    long long _linkId;
    int _linkCol0, _linkCol1;
    NSString *_marked;             // input method text being composed
    NSRange _markedSel;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _linkId = kNoLine;
        _optionAsMeta = YES;
    }
    return self;
}

+ (NSFont *)cellFont {
    static NSFont *font;
    if (!font) font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    return font;
}

+ (NSFont *)fontBold:(BOOL)bold italic:(BOOL)italic {
    static NSFont *fonts[4];
    int i = (bold ? 1 : 0) | (italic ? 2 : 0);
    if (!fonts[i]) {
        NSFont *f = [NSFont monospacedSystemFontOfSize:12
            weight:bold ? NSFontWeightBold : NSFontWeightRegular];
        if (italic) {
            NSFontDescriptor *d = [f.fontDescriptor fontDescriptorWithSymbolicTraits:
                f.fontDescriptor.symbolicTraits | NSFontDescriptorTraitItalic];
            f = [NSFont fontWithDescriptor:d size:12] ?: f;
        }
        fonts[i] = f;
    }
    return fonts[i];
}

+ (NSSize)cellSize {
    static NSSize size;
    if (size.width == 0) {
        NSFont *f = [self cellFont];
        CGFloat w = [@"M" sizeWithAttributes:@{NSFontAttributeName: f}].width;
        CGFloat h = [[[NSLayoutManager alloc] init] defaultLineHeightForFont:f];
        size = NSMakeSize(w, ceil(h));
    }
    return size;
}

+ (CGFloat)padding { return 6; }

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }

- (BOOL)becomeFirstResponder { [self setNeedsDisplay:YES]; return YES; }
- (BOOL)resignFirstResponder { [self setNeedsDisplay:YES]; return YES; }

// The right click. In line mode only the settings item, as before.
- (NSMenu *)menuForEvent:(NSEvent *)event {
    (void)event;
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

- (void)scrollToBottom { [self setOffset:0]; }

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
    // What was under the pointer may have moved; look again, but only when
    // there is a link showing or Cmd is down, so ordinary output costs nothing.
    if (_linkId != kNoLine || (NSEvent.modifierFlags & NSEventModifierFlagCommand))
        [self updateLinkHover];
    [self setNeedsDisplay:YES];
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

    // The Cmd-hovered link, underlined in the text color.
    if (_linkId != kNoLine) {
        int linkRow = (int)(_linkId - base) - top;
        if (linkRow >= firstRow && linkRow <= lastRow) {
            NSRect r = [self rectForRow:linkRow col:_linkCol0 width:_linkCol1 - _linkCol0];
            [[[AppSettings shared] text:Surface::Terminal] setFill];
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
}

// ---------------------------------------------------------------- keys
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
    return [super performKeyEquivalent:e];
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
        [self send:e.keyCode == kVK_LeftArrow ? "\x1b" "b" : "\x1b" "f"];
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
            [self send:TerminalScreen::encodeKey(k.key, keyMods, s->appCursorKeys(),
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
        [self send:out];
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
    if (utf8) [self send:utf8];
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
    if (utf8 && *utf8) [self send:utf8];
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
// A line's text as UTF-8, with the cell each byte came from. The right half
// of a wide character contributes no bytes, so byte offsets from TermLinks
// map back to the cells a link covers.
- (std::string)textOfLine:(int)line cells:(std::vector<int> *)cellOfByte {
    std::string text;
    TerminalScreen *s = self.screen;
    for (int c = 0; c < s->cols(); c++) {
        const TermCell &cell = s->lineCell(line, c);
        if (cell.width == 0) continue;
        const std::string &ch = cell.ch.empty() ? std::string(" ") : cell.ch;
        text += ch;
        cellOfByte->insert(cellOfByte->end(), ch.size(), c);
    }
    return text;
}

// The link at a point, with its line and cells, or nil. Points outside the
// grid find nothing (cellAtPoint: would clamp them onto the edge cells).
- (MCTermLink *)linkAtPoint:(NSPoint)p line:(int *)lineOut col0:(int *)col0 col1:(int *)col1 {
    TerminalScreen *s = self.screen;
    if (!s || !self.linkAt) return nil;
    NSSize cs = [TerminalGridView cellSize];
    CGFloat pad = [TerminalGridView padding];
    int r = (int)floor((p.y - pad) / cs.height);
    int c = (int)floor((p.x - pad) / cs.width);
    if (r < 0 || r >= s->rows() || c < 0 || c >= s->cols()) return nil;
    int line = [self topLine] + r;
    if (s->lineCell(line, c).width == 0 && c > 0) c--;   // right half of a wide character
    std::vector<int> cellOfByte;
    std::string text = [self textOfLine:line cells:&cellOfByte];
    size_t byte = 0;
    while (byte < cellOfByte.size() && cellOfByte[byte] < c) byte++;
    if (byte >= cellOfByte.size()) return nil;
    MCTermLink *link = self.linkAt(text, byte);
    if (!link || link.byteLength == 0 ||
        link.byteStart + link.byteLength > cellOfByte.size()) return nil;
    size_t last = link.byteStart + link.byteLength - 1;
    *lineOut = line;
    *col0 = cellOfByte[link.byteStart];
    *col1 = cellOfByte[last] + (s->lineCell(line, cellOfByte[last]).width == 2 ? 2 : 1);
    return link;
}

- (void)updateLinkHover {
    long long lineId = kNoLine;
    int line = 0, c0 = 0, c1 = 0;
    MCTermLink *link = nil;
    if ((NSEvent.modifierFlags & NSEventModifierFlagCommand) && self.window) {
        NSPoint p = [self convertPoint:[self.window mouseLocationOutsideOfEventStream]
                              fromView:nil];
        if (NSPointInRect(p, self.bounds))
            link = [self linkAtPoint:p line:&line col0:&c0 col1:&c1];
    }
    if (link) lineId = [self base] + line;
    else c0 = c1 = 0;
    BOOL changed = lineId != _linkId || c0 != _linkCol0 || c1 != _linkCol1;
    _link = link;
    _linkId = lineId; _linkCol0 = c0; _linkCol1 = c1;
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

- (void)mouseMoved:(NSEvent *)e { (void)e; [self updateLinkHover]; }
- (void)mouseExited:(NSEvent *)e {
    (void)e;
    if (_linkId != kNoLine) { _link = nil; _linkId = kNoLine; [self setNeedsDisplay:YES]; }
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

// The word (unit 2) or line (unit 3) around a cell, as columns of its line.
- (void)unit:(int)unit atLine:(int)line col:(int)col from:(int *)from to:(int *)to {
    TerminalScreen *s = self.screen;
    int cols = s->cols();
    if (unit == 3) { *from = 0; *to = cols - 1; return; }
    if (s->lineCell(line, col).width == 0 && col > 0) col--;
    if (!IsWordCell(s->lineCell(line, col))) { *from = *to = col; return; }
    int a = col, b = col;
    while (a > 0 && (s->lineCell(line, a - 1).width == 0 ||
                     IsWordCell(s->lineCell(line, a - 1)))) a--;
    while (b + 1 < cols && (s->lineCell(line, b + 1).width == 0 ||
                            IsWordCell(s->lineCell(line, b + 1)))) b++;
    *from = a; *to = b;
}

- (void)mouseDown:(NSEvent *)e {
    if (e.modifierFlags & NSEventModifierFlagCommand) {
        int line, c0, c1;
        NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
        MCTermLink *link = [self linkAtPoint:p line:&line col0:&c0 col1:&c1];
        if (link) {
            _link = nil;
            _linkId = kNoLine;
            [self setNeedsDisplay:YES];
            if (self.openLink) self.openLink(link);
            return;
        }
    }
    [self.window makeFirstResponder:self];
    if (!self.screen) return;
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    int row, col;
    [self cellAtPoint:p row:&row col:&col];
    int line = [self topLine] + row;
    long long lineId = [self base] + line;
    _unit = e.clickCount >= 3 ? 3 : e.clickCount == 2 ? 2 : 1;
    if (_unit > 1) {
        int from, to;
        [self unit:_unit atLine:line col:col from:&from to:&to];
        _unitStartId = _unitEndId = lineId;
        _unitStartCol = from;
        _unitEndCol = to;
        _anchorId = lineId; _anchorCol = from;
        _endId = lineId; _endCol = to;
        _hasSelection = YES;
    } else {
        _anchorId = _endId = lineId;
        _anchorCol = _endCol = col;
        _hasSelection = NO;
    }
    _selecting = YES;
    [self setNeedsDisplay:YES];
}

- (void)mouseDragged:(NSEvent *)e {
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
        int from, to;
        [self unit:_unit atLine:line col:col from:&from to:&to];
        BOOL before = lineId < _unitStartId || (lineId == _unitStartId && from < _unitStartCol);
        if (before) {
            _anchorId = _unitEndId; _anchorCol = _unitEndCol;
            _endId = lineId; _endCol = from;
        } else {
            _anchorId = _unitStartId; _anchorCol = _unitStartCol;
            _endId = lineId; _endCol = to;
        }
        _hasSelection = YES;
    }
    [self setNeedsDisplay:YES];
}

- (void)mouseUp:(NSEvent *)e {
    (void)e;
    _selecting = NO;
}

// On the main screen the wheel scrolls back through the scrollback. In the
// alternate screen it scrolls the program (arrow keys), the way xterm's
// alternateScroll does; there is no scrollback there.
- (void)scrollWheel:(NSEvent *)e {
    TerminalScreen *s = self.screen;
    if (!s) return;
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
    [self send:TerminalScreen::encodePaste(utf8, self.screen->bracketedPaste())];
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
