// FindWidget.mm — see FindWidget.h. Objective-C++.
#import "FindWidget.h"
#import "AppSettings.h"

#include <cstring>

namespace {

NSColor *Hex(uint32_t rgb, CGFloat alpha = 1) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:alpha];
}

// #RRGGBBAA as one number; alpha 0xFF when written as #RRGGBB.
NSColor *Hexa(uint32_t rgba) {
    return Hex(rgba >> 8, (rgba & 0xFF) / 255.0);
}

// One theme's colors, as its VS Code theme file sets them (or VS Code's
// defaults where it sets none). 0 means none (no border).
struct Palette {
    const char *theme;
    uint32_t widgetBg, widgetBorder, widgetFg, shadow;
    uint32_t inputBg, inputBorder, inputFg, placeholder, focus;
    uint32_t optBg, optBorder, optFg;
    uint32_t errorFg, badgeBg, badgeFg, sideBarBg, selection;
};

// RRGGBBAA throughout.
const Palette kPalettes[] = {
    {"dark-modern", 0x202020FF, 0x313131FF, 0xCCCCCCFF, 0x0000005C,
     0x313131FF, 0x3C3C3CFF, 0xCCCCCCFF, 0x989898FF, 0x0078D4FF,
     0x2489DB82, 0x2488DBFF, 0xFFFFFFFF,
     0xF85149FF, 0x616161FF, 0xF8F8F8FF, 0x181818FF, 0x04395EFF},
    {"dark-plus", 0x252526FF, 0x303031FF, 0xCCCCCCFF, 0x0000005C,
     0x3C3C3CFF, 0, 0xCCCCCCFF, 0xA6A6A6FF, 0x007FD4FF,
     0x007FD466, 0x007ACCFF, 0xFFFFFFFF,
     0xF48771FF, 0x4D4D4DFF, 0xFFFFFFFF, 0x252526FF, 0x04395EFF},
    {"monokai", 0x1E1F1CFF, 0, 0xF8F8F2FF, 0x00000098,
     0x414339FF, 0, 0xF8F8F2FF, 0x90908AFF, 0x99947CFF,
     0x99947C66, 0x75715EFF, 0xFFFFFFFF,
     0xF48771FF, 0x75715EFF, 0xF8F8F2FF, 0x1E1F1CFF, 0x75715EFF},
    {"solarized-dark", 0x00212BFF, 0, 0x93A1A1FF, 0x0000005C,
     0x003847FF, 0, 0x93A1A1FF, 0x93A1A1AA, 0x2AA19999,
     0x2AA19944, 0x2AA19999, 0xFFFFFFFF,
     0xFFEAEAFF, 0x047AA6FF, 0xFFFFFFFF, 0x00212BFF, 0x005A6FFF},
    {"tomorrow-night-blue", 0x001C40FF, 0, 0xFFFFFFFF, 0x0000005C,
     0x001733FF, 0, 0xFFFFFFFF, 0xFFFFFF80, 0xBBDAFFFF,
     0xBBDAFF44, 0xBBDAFFFF, 0xFFFFFFFF,
     0xA92049FF, 0xBBDAFFCC, 0x001733FF, 0x001C40FF, 0xFFFFFF60},
    {"light-modern", 0xF8F8F8FF, 0xE5E5E5FF, 0x3B3B3BFF, 0x00000029,
     0xFFFFFFFF, 0xCECECEFF, 0x3B3B3BFF, 0x767676FF, 0x005FB8FF,
     0xBED6EDFF, 0x005FB8FF, 0x000000FF,
     0xF85149FF, 0xCCCCCCFF, 0x3B3B3BFF, 0xF8F8F8FF, 0xE8E8E8FF},
    {"light-plus", 0xF3F3F3FF, 0xD4D4D4FF, 0x616161FF, 0x00000029,
     0xFFFFFFFF, 0, 0x616161FF, 0x767676FF, 0x0090F1FF,
     0x0090F133, 0x007ACCFF, 0x000000FF,
     0xA1260DFF, 0xC4C4C4FF, 0x333333FF, 0xF3F3F3FF, 0xE4E6F1FF},
    {"quiet-light", 0xF3F3F3FF, 0, 0x333333FF, 0x00000029,
     0xFFFFFFFF, 0, 0x333333FF, 0x767676FF, 0x9769DCFF,
     0x9769DC33, 0xADAFB7FF, 0x000000FF,
     0xA1260DFF, 0x705697AAU, 0xFFFFFFFF, 0xF2F2F2FF, 0xC4D9B1FF},
    {"solarized-light", 0xEEE8D5FF, 0, 0x586E75FF, 0x00000029,
     0xDDD6C1FF, 0, 0x586E75FF, 0x586E75AA, 0xB49471FF,
     0xB4947133, 0xD3AF86FF, 0x000000FF,
     0xA1260DFF, 0xB58900AA, 0xFFFFFFFF, 0xEEE8D5FF, 0xDFCA88FF},
};

const Palette &PaletteFor(const char *theme, bool dark) {
    for (const Palette &p : kPalettes)
        if (!strcmp(p.theme, theme)) return p;
    return kPalettes[dark ? 1 : 6];   // dark-plus or light-plus
}

NSColor *Opt(uint32_t rgba) { return rgba ? Hexa(rgba) : nil; }

}  // namespace

// ================================================================== style
@implementation MCFindStyle {
@public
    BOOL _dark;
    NSColor *_widgetBackground, *_widgetBorder, *_widgetForeground, *_shadow;
    NSColor *_inputBackground, *_inputBorder, *_inputForeground, *_placeholder, *_focusBorder;
    NSColor *_optionActiveBackground, *_optionActiveBorder, *_optionActiveForeground,
        *_optionHover;
    NSColor *_toolbarHover, *_errorForeground, *_errorBorder;
    NSColor *_badgeBackground, *_badgeForeground;
    NSColor *_sideBarBackground, *_sideBarForeground, *_muted;
    NSColor *_matchHighlight, *_removedText, *_insertedText, *_selection;
}

+ (MCFindStyle *)current {
    const Settings &cfg = [AppSettings shared].settings;
    const bool dark = cfg.isDark();
    const Palette &p = PaletteFor(cfg.theme().id, dark);
    MCFindStyle *s = [MCFindStyle new];
    s->_dark = dark;
    s->_widgetBackground = Hexa(p.widgetBg);
    s->_widgetBorder = Opt(p.widgetBorder);
    s->_widgetForeground = Hexa(p.widgetFg);
    s->_shadow = Hexa(p.shadow);
    s->_inputBackground = Hexa(p.inputBg);
    s->_inputBorder = Opt(p.inputBorder);
    s->_inputForeground = Hexa(p.inputFg);
    s->_placeholder = Hexa(p.placeholder);
    s->_focusBorder = Hexa(p.focus);
    s->_optionActiveBackground = Hexa(p.optBg);
    s->_optionActiveBorder = Hexa(p.optBorder);
    s->_optionActiveForeground = Hexa(p.optFg);
    s->_optionHover = dark ? Hexa(0x5A5D5E80) : Hexa(0xB8B8B850);
    s->_toolbarHover = dark ? Hexa(0x5A5D5E50) : Hexa(0xB8B8B850);
    s->_errorForeground = Hexa(p.errorFg);
    s->_errorBorder = Hex(0xBE1100);
    s->_badgeBackground = Hexa(p.badgeBg);
    s->_badgeForeground = Hexa(p.badgeFg);
    s->_sideBarBackground = Hexa(p.sideBarBg);
    s->_sideBarForeground = s->_widgetForeground;
    s->_muted = [s->_widgetForeground colorWithAlphaComponent:0.7];
    s->_matchHighlight = Hexa(0xEA5C0055);
    s->_removedText = Hexa(0xFF000033);
    s->_insertedText = dark ? Hexa(0x9CCC2C33) : Hexa(0x9CCC2C40);
    s->_selection = Hexa(p.selection);
    return s;
}

- (BOOL)dark { return _dark; }
- (NSColor *)widgetBackground { return _widgetBackground; }
- (NSColor *)widgetBorder { return _widgetBorder; }
- (NSColor *)widgetForeground { return _widgetForeground; }
- (NSColor *)shadow { return _shadow; }
- (NSColor *)inputBackground { return _inputBackground; }
- (NSColor *)inputBorder { return _inputBorder; }
- (NSColor *)inputForeground { return _inputForeground; }
- (NSColor *)placeholder { return _placeholder; }
- (NSColor *)focusBorder { return _focusBorder; }
- (NSColor *)optionActiveBackground { return _optionActiveBackground; }
- (NSColor *)optionActiveBorder { return _optionActiveBorder; }
- (NSColor *)optionActiveForeground { return _optionActiveForeground; }
- (NSColor *)optionHover { return _optionHover; }
- (NSColor *)toolbarHover { return _toolbarHover; }
- (NSColor *)errorForeground { return _errorForeground; }
- (NSColor *)errorBorder { return _errorBorder; }
- (NSColor *)badgeBackground { return _badgeBackground; }
- (NSColor *)badgeForeground { return _badgeForeground; }
- (NSColor *)sideBarBackground { return _sideBarBackground; }
- (NSColor *)sideBarForeground { return _sideBarForeground; }
- (NSColor *)muted { return _muted; }
- (NSColor *)matchHighlight { return _matchHighlight; }
- (NSColor *)removedText { return _removedText; }
- (NSColor *)insertedText { return _insertedText; }
- (NSColor *)selection { return _selection; }
- (NSAppearance *)appearance {
    return [NSAppearance appearanceNamed:_dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
}
@end

// ================================================================== icons
// Each in a 16 x 16 box, flipped (y down), in the spirit of the codicons:
// one-point strokes, square ends.
static void Stroke(NSBezierPath *p, CGFloat width = 1.1) {
    p.lineWidth = width;
    p.lineCapStyle = NSLineCapStyleRound;
    p.lineJoinStyle = NSLineJoinStyleRound;
    [p stroke];
}

static void Text(NSString *s, NSRect box, CGFloat size, NSFontWeight weight, NSColor *color,
                 CGFloat dy = 0) {
    NSDictionary *a = @{NSFontAttributeName: [NSFont systemFontOfSize:size weight:weight],
                        NSForegroundColorAttributeName: color};
    NSSize sz = [s sizeWithAttributes:a];
    [s drawAtPoint:NSMakePoint(NSMidX(box) - sz.width / 2, NSMidY(box) - sz.height / 2 + dy)
        withAttributes:a];
}

void MCFindDrawIcon(MCFindIcon icon, NSRect box, NSColor *color) {
    [NSGraphicsContext saveGraphicsState];
    NSAffineTransform *t = [NSAffineTransform transform];
    [t translateXBy:box.origin.x yBy:box.origin.y];
    const BOOL flipped = NSGraphicsContext.currentContext.isFlipped;
    if (!flipped) {   // draw in flipped coordinates either way
        [t translateXBy:0 yBy:16];
        [t scaleXBy:1 yBy:-1];
    }
    [t concat];
    const NSRect b = NSMakeRect(0, 0, 16, 16);
    [color set];
    NSBezierPath *p = [NSBezierPath bezierPath];
    auto line = [&](CGFloat x0, CGFloat y0, CGFloat x1, CGFloat y1) {
        [p moveToPoint:NSMakePoint(x0, y0)];
        [p lineToPoint:NSMakePoint(x1, y1)];
    };
    // Text is drawn in the context's own orientation.
    auto text = [&](NSString *s, CGFloat size, NSFontWeight w, CGFloat dy) {
        [NSGraphicsContext saveGraphicsState];
        if (!flipped) {
            NSAffineTransform *u = [NSAffineTransform transform];
            [u translateXBy:0 yBy:16];
            [u scaleXBy:1 yBy:-1];
            [u concat];
        }
        Text(s, b, size, w, color, flipped ? dy : -dy);
        [NSGraphicsContext restoreGraphicsState];
    };
    switch (icon) {
    case MCFindIconCase:
        text(@"Aa", 11.5, NSFontWeightMedium, 0);
        break;
    case MCFindIconWord:
        text(@"ab", 11, NSFontWeightMedium, -1.5);
        line(2.5, 11.5, 2.5, 13.5);
        line(2.5, 13.5, 13.5, 13.5);
        line(13.5, 13.5, 13.5, 11.5);
        Stroke(p, 1);
        break;
    case MCFindIconRegex: {
        NSRectFill(NSMakeRect(2.5, 11, 2.8, 2.8));
        const CGFloat cx = 10.5, cy = 5.5, r = 3.6;
        for (int k = 0; k < 3; k++) {
            const CGFloat a = M_PI / 2 + k * M_PI / 3;
            line(cx - r * cos(a), cy - r * sin(a), cx + r * cos(a), cy + r * sin(a));
        }
        Stroke(p, 1.3);
        break;
    }
    case MCFindIconPreserveCase:
        text(@"AB", 10.5, NSFontWeightSemibold, 0);
        break;
    case MCFindIconSelection:
        [p appendBezierPathWithRoundedRect:NSMakeRect(1.5, 2.5, 13, 11) xRadius:1 yRadius:1];
        line(4, 5.5, 12, 5.5);
        line(4, 8, 12, 8);
        line(4, 10.5, 9, 10.5);
        Stroke(p, 1);
        break;
    case MCFindIconReplace:
    case MCFindIconReplaceAll: {
        // A small box, an arrow bending down to a filled box.
        const BOOL all = icon == MCFindIconReplaceAll;
        [p appendBezierPathWithRect:NSMakeRect(1.5, 1.5, 5, 5)];
        if (all) [p appendBezierPathWithRect:NSMakeRect(1.5, 8.5, 5, 5)];
        line(8.5, 4, 12, 4);
        line(12, 4, 12, 7.5);
        line(10.3, 5.9, 12, 7.6);
        line(12, 7.6, 13.7, 5.9);
        Stroke(p, 1);
        NSRectFill(NSMakeRect(9, 9.5, 5.5, 5));
        break;
    }
    case MCFindIconUp:
        line(8, 13, 8, 3.2);
        line(3.8, 7.2, 8, 3);
        line(8, 3, 12.2, 7.2);
        Stroke(p);
        break;
    case MCFindIconDown:
        line(8, 3, 8, 12.8);
        line(3.8, 8.8, 8, 13);
        line(8, 13, 12.2, 8.8);
        Stroke(p);
        break;
    case MCFindIconClose:
        line(4, 4, 12, 12);
        line(12, 4, 4, 12);
        Stroke(p);
        break;
    case MCFindIconChevronRight:
        line(6, 3.5, 10.5, 8);
        line(10.5, 8, 6, 12.5);
        Stroke(p);
        break;
    case MCFindIconChevronDown:
        line(3.5, 6, 8, 10.5);
        line(8, 10.5, 12.5, 6);
        Stroke(p);
        break;
    case MCFindIconMore:
        for (int k = 0; k < 3; k++)
            [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(2.6 + 4.4 * k, 7, 2, 2)] fill];
        break;
    case MCFindIconCollapseAll:
    case MCFindIconExpandAll:
        [p appendBezierPathWithRoundedRect:NSMakeRect(4.5, 4.5, 9, 9) xRadius:1 yRadius:1];
        line(2.5, 11.5, 2.5, 2.5);
        line(2.5, 2.5, 11.5, 2.5);
        line(6.8, 9, 11.2, 9);
        if (icon == MCFindIconExpandAll) line(9, 6.8, 9, 11.2);
        Stroke(p, 1);
        break;
    case MCFindIconRefresh: {
        NSBezierPath *arc = [NSBezierPath bezierPath];
        [arc appendBezierPathWithArcWithCenter:NSMakePoint(8, 8.5) radius:5
                                    startAngle:-40 endAngle:250 clockwise:NO];
        Stroke(arc);
        line(11.8, 2.6, 12.1, 5.4);
        line(12.1, 5.4, 9.3, 5.6);
        Stroke(p);
        break;
    }
    case MCFindIconClear:
        [p appendBezierPathWithRect:NSMakeRect(2.5, 3.5, 11, 9)];
        line(5.5, 6.5, 10.5, 9.5);
        line(10.5, 6.5, 5.5, 9.5);
        Stroke(p, 1);
        break;
    }
    [NSGraphicsContext restoreGraphicsState];
}

// ================================================================= button
@implementation MCFindButton {
    BOOL _hover, _toggle, _on;
    MCFindIcon _icon;
    NSTrackingArea *_tracking;
}

+ (instancetype)make:(MCFindIcon)icon tip:(NSString *)tip target:(id)target action:(SEL)action
              toggle:(BOOL)toggle {
    MCFindButton *b = [[self alloc] initWithFrame:NSMakeRect(0, 0, toggle ? 20 : 22, toggle ? 20 : 22)];
    b->_icon = icon;
    b->_toggle = toggle;
    b.radius = toggle ? 3 : 5;
    b.bordered = NO;
    b.title = @"";
    b.toolTip = tip;
    b.target = target;
    b.action = action;
    b.refusesFirstResponder = YES;   // the keyboard stays in the field
    b.style = [MCFindStyle current];
    b.accessibilityLabel = tip;
    return b;
}

+ (instancetype)button:(MCFindIcon)icon tip:(NSString *)tip target:(id)target action:(SEL)action {
    return [self make:icon tip:tip target:target action:action toggle:NO];
}
+ (instancetype)toggle:(MCFindIcon)icon tip:(NSString *)tip target:(id)target action:(SEL)action {
    return [self make:icon tip:tip target:target action:action toggle:YES];
}

- (BOOL)isFlipped { return YES; }
- (BOOL)isToggle { return _toggle; }
- (MCFindIcon)icon { return _icon; }
- (void)setIcon:(MCFindIcon)icon { _icon = icon; self.needsDisplay = YES; }
- (BOOL)on { return _on; }
- (void)setOn:(BOOL)on { _on = on; self.needsDisplay = YES; }
- (void)setStyle:(MCFindStyle *)style { _style = style; self.needsDisplay = YES; }
- (void)setEnabled:(BOOL)enabled { [super setEnabled:enabled]; self.needsDisplay = YES; }

// A click flips a toggle before the action runs.
- (BOOL)sendAction:(SEL)action to:(id)target {
    if (_toggle) _on = !_on;
    self.needsDisplay = YES;
    return [super sendAction:action to:target];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow |
                     NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
}
- (void)mouseEntered:(NSEvent *)e { (void)e; _hover = YES; self.needsDisplay = YES; }
- (void)mouseExited:(NSEvent *)e { (void)e; _hover = NO; self.needsDisplay = YES; }

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    MCFindStyle *s = _style ?: [MCFindStyle current];
    const NSRect r = NSInsetRect(self.bounds, 0.5, 0.5);
    NSBezierPath *box = [NSBezierPath bezierPathWithRoundedRect:r xRadius:_radius yRadius:_radius];
    const BOOL enabled = self.enabled;
    if (_toggle && _on) {
        [s.optionActiveBackground setFill];
        [box fill];
        [s.optionActiveBorder setStroke];
        box.lineWidth = 1;
        [box stroke];
    } else if (_hover && enabled) {
        [(_toggle ? s.optionHover : s.toolbarHover) setFill];
        [box fill];
    }
    NSColor *fg = _toggle && _on ? s.optionActiveForeground : s.widgetForeground;
    if (!enabled) fg = [fg colorWithAlphaComponent:_toggle ? 0.4 : 0.35];
    const NSRect b = self.bounds;
    MCFindDrawIcon(_icon, NSMakeRect(round(NSMidX(b) - 8), round(NSMidY(b) - 8), 16, 16), fg);
}
@end

// ================================================================== input
@interface MCFindInputField : NSTextField
@property(nonatomic, weak) MCFindInput *owner;
@end

@interface MCFindInput ()
- (void)focusChanged;
@end

@implementation MCFindInputField
- (BOOL)becomeFirstResponder {
    BOOL ok = [super becomeFirstResponder];
    [self.owner focusChanged];
    return ok;
}
- (void)textDidEndEditing:(NSNotification *)note {
    [super textDidEndEditing:note];
    [self.owner focusChanged];
}
@end

@implementation MCFindInput {
    MCFindInputField *_field;
    NSArray<MCFindButton *> *_toggles;
    BOOL _error, _togglesHidden;
}

- (instancetype)initWithPlaceholder:(NSString *)placeholder
                            toggles:(NSArray<MCFindButton *> *)toggles {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 200, 25)])) {
        _fontSize = 13;
        _style = [MCFindStyle current];
        _field = [[MCFindInputField alloc] initWithFrame:NSZeroRect];
        _field.owner = self;
        _field.bordered = NO;
        _field.bezeled = NO;
        _field.drawsBackground = NO;
        _field.focusRingType = NSFocusRingTypeNone;
        _field.usesSingleLineMode = YES;
        _field.cell.scrollable = YES;
        _field.cell.wraps = NO;
        _field.font = [NSFont systemFontOfSize:_fontSize];
        _field.placeholderString = placeholder;
        [self addSubview:_field];
        _toggles = [toggles copy];
        for (MCFindButton *t in _toggles) [self addSubview:t];
        [self applyStyle];
    }
    return self;
}

- (BOOL)isFlipped { return YES; }
- (NSTextField *)field { return _field; }
- (NSArray<MCFindButton *> *)toggles { return _toggles; }
- (BOOL)error { return _error; }
- (void)setError:(BOOL)error {
    if (_error == error) return;
    _error = error;
    self.needsDisplay = YES;
}
- (BOOL)togglesHidden { return _togglesHidden; }
- (void)setTogglesHidden:(BOOL)hidden {
    _togglesHidden = hidden;
    for (MCFindButton *t in _toggles) t.hidden = hidden;
    self.needsLayout = YES;
}
- (void)setStyle:(MCFindStyle *)style {
    _style = style;
    for (MCFindButton *t in _toggles) t.style = style;
    [self applyStyle];
}
- (void)setFontSize:(CGFloat)size {
    _fontSize = size;
    _field.font = [NSFont systemFontOfSize:size];
    [self applyStyle];
}

- (void)applyStyle {
    _field.textColor = _style.inputForeground;
    if (_field.placeholderString.length || _field.placeholderAttributedString.length) {
        NSString *p = _field.placeholderString ?: _field.placeholderAttributedString.string;
        _field.placeholderAttributedString = [[NSAttributedString alloc] initWithString:p attributes:@{
            NSFontAttributeName: _field.font, NSForegroundColorAttributeName: _style.placeholder}];
    }
    self.needsDisplay = YES;
    self.needsLayout = YES;
}

- (BOOL)editing {
    id fr = self.window.firstResponder;
    return [fr isKindOfClass:[NSTextView class]] && ((NSTextView *)fr).delegate == (id)_field;
}

- (void)focusChanged {
    // The field editor arrives after becomeFirstResponder returns.
    dispatch_async(dispatch_get_main_queue(), ^{ self.needsDisplay = YES; });
    self.needsDisplay = YES;
}

- (void)layout {
    [super layout];
    const NSRect b = self.bounds;
    CGFloat right = NSMaxX(b) - 2;
    if (!_togglesHidden) {
        for (MCFindButton *t in _toggles.reverseObjectEnumerator) {
            right -= t.frame.size.width;
            t.frame = NSMakeRect(right, round((b.size.height - t.frame.size.height) / 2),
                                 t.frame.size.width, t.frame.size.height);
            right -= 2;
        }
    }
    // An NSTextField cell insets its text 2 points: text at 6 from the
    // edge, VS Code's input padding.
    const CGFloat h = ceil(_field.font.ascender - _field.font.descender + _field.font.leading) + 3;
    _field.frame = NSMakeRect(4, round((b.size.height - h) / 2), MAX(10, right - 4 - 2), h);
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect r = NSInsetRect(self.bounds, 0.5, 0.5);
    NSBezierPath *box = [NSBezierPath bezierPathWithRoundedRect:r xRadius:4 yRadius:4];
    [_style.inputBackground setFill];
    [box fill];
    NSColor *border = _error ? _style.errorBorder : [self editing] ? _style.focusBorder
                                                                   : _style.inputBorder;
    if (border) {
        [border setStroke];
        box.lineWidth = 1;
        [box stroke];
    }
}

// A click on the box but outside the field puts the keyboard in the field.
- (void)mouseDown:(NSEvent *)e {
    (void)e;
    [self.window makeFirstResponder:_field];
}
@end
