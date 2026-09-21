// TerminalGridView.mm — see TerminalGridView.h. Objective-C++.
//
// Drawing: the panel's own layer paints the background, so cells in the
// default background draw nothing (a see-through panel stays see-through).
// ASCII runs in one style are drawn as one string; every other character is
// drawn in its own cell, so a fallback font's different advance can never
// push the rest of the row out of its columns.
//
// Keys: Cmd shortcuts are left to the menu (they arrive through
// performKeyEquivalent: before keyDown:), everything else is encoded by
// TerminalScreen::encodeKey / encodeChar. Option is Meta (ESC prefix).
#import "TerminalGridView.h"
#import "AppSettings.h"
#import <Carbon/Carbon.h>   // kVK_* key codes (header only, nothing linked)

@implementation TerminalGridView {
    BOOL _selecting, _hasSelection;
    int _anchorRow, _anchorCol, _endRow, _endCol;
    CGFloat _scrollAccum;
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

- (void)screenChanged {
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
- (NSRect)rectForRow:(int)row col:(int)col width:(int)w {
    NSSize cs = [TerminalGridView cellSize];
    CGFloat pad = [TerminalGridView padding];
    return NSMakeRect(pad + col * cs.width, pad + row * cs.height,
                      w * cs.width, cs.height);
}

- (void)cellAtPoint:(NSPoint)p row:(int *)row col:(int *)col {
    NSSize cs = [TerminalGridView cellSize];
    CGFloat pad = [TerminalGridView padding];
    int r = (int)floor((p.y - pad) / cs.height);
    int c = (int)floor((p.x - pad) / cs.width);
    *row = MAX(0, MIN(r, (self.screen ? self.screen->rows() : 1) - 1));
    *col = MAX(0, MIN(c, (self.screen ? self.screen->cols() : 1) - 1));
}

- (BOOL)isSelectedRow:(int)r col:(int)c {
    if (!_hasSelection) return NO;
    int r0 = _anchorRow, c0 = _anchorCol, r1 = _endRow, c1 = _endCol;
    if (r1 < r0 || (r1 == r0 && c1 < c0)) { std::swap(r0, r1); std::swap(c0, c1); }
    if (r < r0 || r > r1) return NO;
    if (r == r0 && c < c0) return NO;
    if (r == r1 && c > c1) return NO;
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

    for (int r = firstRow; r <= lastRow; r++) {
        // Backgrounds and the selection, merged into runs.
        int c = 0;
        while (c < cols) {
            const TermCell &cell = s->cell(r, c);
            NSColor *bg = [self isSelectedRow:r col:c] ? selColor
                                                        : ColorsFor(cell.style).bg;
            int start = c++;
            while (c < cols) {
                NSColor *next = [self isSelectedRow:r col:c] ? selColor
                                    : ColorsFor(s->cell(r, c).style).bg;
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
            const TermCell &cell = s->cell(r, c);
            if (cell.width == 0) { c++; continue; }
            bool ascii = cell.ch.size() == 1 && (unsigned char)cell.ch[0] < 0x80;
            int start = c;
            std::string text = cell.ch;
            c += cell.width == 2 ? 2 : 1;
            if (ascii) {
                while (c < cols) {
                    const TermCell &n = s->cell(r, c);
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

    // The cursor: a block while focused, an outline otherwise.
    int cr = s->cursorRow(), cc = s->cursorCol();
    if (s->cursorVisible() && cr >= firstRow && cr <= lastRow) {
        const TermCell &cell = s->cell(cr, cc);
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
}

// ---------------------------------------------------------------- keys
- (void)send:(const std::string &)bytes {
    if (bytes.empty() || !self.onInput) return;
    self.onInput([NSData dataWithBytes:bytes.data() length:bytes.size()]);
}

- (void)keyDown:(NSEvent *)e {
    TerminalScreen *s = self.screen;
    NSEventModifierFlags f = e.modifierFlags;
    if ((f & NSEventModifierFlagCommand) || !s) { [super keyDown:e]; return; }
    int mods = ((f & NSEventModifierFlagShift) ? TermModShift : 0) |
               ((f & NSEventModifierFlagOption) ? TermModAlt : 0) |
               ((f & NSEventModifierFlagControl) ? TermModCtrl : 0);
    if (_hasSelection) { _hasSelection = NO; [self setNeedsDisplay:YES]; }
    [NSCursor setHiddenUntilMouseMoves:YES];

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
    for (const auto &k : keys) {
        if (k.code == e.keyCode) {
            [self send:TerminalScreen::encodeKey(k.key, mods, s->appCursorKeys(),
                                                 s->appKeypad())];
            return;
        }
    }

    if (mods & (TermModCtrl | TermModAlt)) {
        // Control codes and Meta come from the key itself, not from what
        // Option would have typed (Option+b is ESC b, not ∫).
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
            out += TerminalScreen::encodeChar(cp, mods & (TermModCtrl | TermModAlt));
            i = NSMaxRange(r);
        }
        [self send:out];
        return;
    }

    NSString *chars = e.characters;
    if (!chars.length) return;
    unichar first = [chars characterAtIndex:0];
    if (first >= 0xF700 && first <= 0xF8FF) return;      // unmapped function key
    const char *utf8 = chars.UTF8String;
    if (utf8) [self send:utf8];
}

// ---------------------------------------------------------------- mouse
- (void)mouseDown:(NSEvent *)e {
    [self.window makeFirstResponder:self];
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    [self cellAtPoint:p row:&_anchorRow col:&_anchorCol];
    _endRow = _anchorRow;
    _endCol = _anchorCol;
    _selecting = YES;
    if (_hasSelection) { _hasSelection = NO; [self setNeedsDisplay:YES]; }
}

- (void)mouseDragged:(NSEvent *)e {
    if (!_selecting) return;
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    [self cellAtPoint:p row:&_endRow col:&_endCol];
    _hasSelection = _endRow != _anchorRow || _endCol != _anchorCol;
    [self setNeedsDisplay:YES];
}

- (void)mouseUp:(NSEvent *)e {
    (void)e;
    _selecting = NO;
}

// In the alternate screen the wheel scrolls the program (arrow keys), the
// way xterm's alternateScroll does; there is no scrollback of its own.
- (void)scrollWheel:(NSEvent *)e {
    TerminalScreen *s = self.screen;
    if (!s || !s->altScreen()) return;
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
- (void)copy:(id)sender {
    (void)sender;
    if (!_hasSelection || !self.screen) return;
    std::string text = self.screen->textBetween(_anchorRow, _anchorCol,
                                                _endRow, _endCol);
    NSString *str = [[NSString alloc] initWithBytes:text.data() length:text.size()
                                           encoding:NSUTF8StringEncoding];
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

- (void)selectAll:(id)sender {
    (void)sender;
    if (!self.screen) return;
    _anchorRow = 0; _anchorCol = 0;
    _endRow = self.screen->rows() - 1; _endCol = self.screen->cols() - 1;
    _hasSelection = YES;
    [self setNeedsDisplay:YES];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(copy:)) return _hasSelection;
    if (item.action == @selector(paste:))
        return [[NSPasteboard generalPasteboard]
                   stringForType:NSPasteboardTypeString] != nil;
    if (item.action == @selector(selectAll:)) return YES;
    return NO;
}

@end
