// TerminalFindBar.mm — see TerminalFindBar.h. Objective-C++.
//
// Colors: the box follows the theme's darkness (Settings::isDark) with VS
// Code's editor widget colors, inline like the other widgets without
// settings keys; the text is the terminal's text color.
#import "TerminalFindBar.h"
#import "AppSettings.h"

static NSColor *Hex(unsigned rgb, CGFloat alpha = 1) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:alpha];
}

@implementation MCTerminalFindBar {
    NSTextField *_field;
    NSTextField *_count;
    NSButton *_caseButton, *_prev, *_next, *_close;
}

+ (NSSize)preferredSize { return NSMakeSize(352, 32); }

- (NSButton *)button:(NSString *)symbol tip:(NSString *)tip action:(SEL)action {
    NSImage *img = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:tip];
    NSButton *b = [NSButton buttonWithImage:img ?: [[NSImage alloc] init]
                                     target:self action:action];
    b.bordered = NO;
    b.toolTip = tip;
    b.refusesFirstResponder = YES;   // the keyboard stays in the field
    [self addSubview:b];
    return b;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        self.layer.cornerRadius = 5;
        self.layer.borderWidth = 1;

        _field = [[NSTextField alloc] init];
        _field.placeholderString = @"Find";
        _field.font = [NSFont systemFontOfSize:12];
        _field.bezelStyle = NSTextFieldRoundedBezel;
        _field.bezeled = YES;
        _field.focusRingType = NSFocusRingTypeNone;
        _field.usesSingleLineMode = YES;
        _field.cell.scrollable = YES;
        _field.delegate = self;
        [self addSubview:_field];

        _count = [NSTextField labelWithString:@""];
        _count.font = [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular];
        _count.alignment = NSTextAlignmentCenter;
        _count.lineBreakMode = NSLineBreakByClipping;
        [self addSubview:_count];

        // Match Case, drawn as VS Code draws its toggles: plain when off, on
        // a tinted square with an accent border when on.
        _caseButton = [NSButton buttonWithTitle:@"Aa" target:self action:@selector(caseToggled:)];
        [_caseButton setButtonType:NSButtonTypePushOnPushOff];
        _caseButton.bordered = NO;
        _caseButton.wantsLayer = YES;
        _caseButton.layer.cornerRadius = 3;
        _caseButton.font = [NSFont systemFontOfSize:12 weight:NSFontWeightMedium];
        _caseButton.toolTip = @"Match Case";
        _caseButton.refusesFirstResponder = YES;
        [self addSubview:_caseButton];

        _prev = [self button:@"arrow.up" tip:@"Previous Match (⇧⏎)" action:@selector(prevClicked:)];
        _next = [self button:@"arrow.down" tip:@"Next Match (⏎)" action:@selector(nextClicked:)];
        _close = [self button:@"xmark" tip:@"Close (Esc)" action:@selector(closeClicked:)];
        [self applySettings];
        [self showCount:0 index:-1 limit:0];
    }
    return self;
}

- (NSTextField *)field { return _field; }
- (BOOL)matchCase { return _caseButton.state == NSControlStateValueOn; }

- (void)applySettings {
    const Settings &cfg = [AppSettings shared].settings;
    BOOL dark = cfg.isDark();
    self.layer.backgroundColor = (dark ? Hex(0x252526) : Hex(0xF3F3F3)).CGColor;
    self.layer.borderColor = (dark ? Hex(0x454545) : Hex(0xC8C8C8)).CGColor;
    self.appearance = [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua
                                                         : NSAppearanceNameAqua];
    NSColor *text = MCColor(cfg.text(Surface::Terminal));
    if (!dark) text = Hex(0x333333);
    _count.textColor = [text colorWithAlphaComponent:0.75];
    for (NSButton *b in @[_prev, _next, _close]) b.contentTintColor = text;
    [self showCaseState];
}

- (void)layout {
    [super layout];
    CGFloat W = self.bounds.size.width, H = self.bounds.size.height;
    const CGFloat pad = 5, btn = 22, gap = 2;
    CGFloat x = W - pad - btn;
    _close.frame = NSMakeRect(x, (H - btn) / 2, btn, btn);
    x -= btn + gap;
    _next.frame = NSMakeRect(x, (H - btn) / 2, btn, btn);
    x -= btn;
    _prev.frame = NSMakeRect(x, (H - btn) / 2, btn, btn);
    x -= 28 + gap;
    _caseButton.frame = NSMakeRect(x, (H - 20) / 2, 28, 20);
    x -= 70;
    _count.frame = NSMakeRect(x, (H - 16) / 2, 68, 16);
    CGFloat fieldW = MAX(40, x - pad - gap);
    _field.frame = NSMakeRect(pad, (H - 22) / 2, fieldW, 22);
}

- (void)showCount:(NSInteger)count index:(NSInteger)index limit:(NSInteger)limit {
    if (!_field.stringValue.length) _count.stringValue = @"";
    else if (count == 0) _count.stringValue = @"No results";
    else {
        NSString *total = limit > 0 && count >= limit
            ? [NSString stringWithFormat:@"%ld+", (long)count]
            : [NSString stringWithFormat:@"%ld", (long)count];
        _count.stringValue = index >= 0
            ? [NSString stringWithFormat:@"%ld of %@", (long)index + 1, total]
            : total;
    }
    _prev.enabled = _next.enabled = count > 0;
}

- (void)focusField {
    [self.window makeFirstResponder:_field];
    [_field.currentEditor selectAll:nil];
}

- (BOOL)isEditing {
    id fr = self.window.firstResponder;
    return [fr isKindOfClass:[NSTextView class]] && ((NSTextView *)fr).delegate == (id)_field;
}

// ------------------------------------------------------------- actions
- (void)caseToggled:(id)sender {
    (void)sender;
    [self showCaseState];
    if (self.onChange) self.onChange();
}

- (void)showCaseState {
    BOOL on = self.matchCase;
    NSColor *accent = Hex(0x4EA1F7);
    _caseButton.layer.backgroundColor = on ? [accent colorWithAlphaComponent:0.3].CGColor : nil;
    _caseButton.layer.borderColor = accent.CGColor;
    _caseButton.layer.borderWidth = on ? 1 : 0;
    NSColor *text = _count.textColor ?: [NSColor labelColor];
    _caseButton.attributedTitle = [[NSAttributedString alloc] initWithString:@"Aa" attributes:@{
        NSFontAttributeName: _caseButton.font,
        NSForegroundColorAttributeName: on ? [text colorWithAlphaComponent:1] : text,
    }];
}
- (void)prevClicked:(id)sender { (void)sender; if (self.onNext) self.onNext(NO); }
- (void)nextClicked:(id)sender { (void)sender; if (self.onNext) self.onNext(YES); }
- (void)closeClicked:(id)sender { (void)sender; if (self.onClose) self.onClose(); }

- (void)controlTextDidChange:(NSNotification *)note {
    (void)note;
    if (self.onChange) self.onChange();
}

- (BOOL)control:(NSControl *)control textView:(NSTextView *)tv
    doCommandBySelector:(SEL)sel {
    (void)control; (void)tv;
    if (sel == @selector(insertNewline:) || sel == @selector(insertNewlineIgnoringFieldEditor:) ||
        sel == @selector(insertLineBreak:)) {
        BOOL shift = (NSApp.currentEvent.modifierFlags & NSEventModifierFlagShift) != 0;
        if (self.onNext) self.onNext(!shift);
        return YES;
    }
    if (sel == @selector(cancelOperation:)) {
        if (self.onClose) self.onClose();
        return YES;
    }
    return NO;
}

// Cmd+G, Shift+Cmd+G and Cmd+F while the field is being edited. The window
// offers key equivalents to every view, so only while editing.
- (BOOL)performKeyEquivalent:(NSEvent *)e {
    if (![self isEditing]) return [super performKeyEquivalent:e];
    NSEventModifierFlags f = e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    NSEventModifierFlags mods = f & (NSEventModifierFlagCommand | NSEventModifierFlagShift |
                                     NSEventModifierFlagOption | NSEventModifierFlagControl);
    NSString *ch = e.charactersIgnoringModifiers.lowercaseString;
    if (mods == NSEventModifierFlagCommand && [ch isEqualToString:@"f"]) {
        [_field.currentEditor selectAll:nil];
        return YES;
    }
    if ([ch isEqualToString:@"g"] && (mods == NSEventModifierFlagCommand ||
                                    mods == (NSEventModifierFlagCommand | NSEventModifierFlagShift))) {
        if (self.onNext) self.onNext(!(mods & NSEventModifierFlagShift));
        return YES;
    }
    return [super performKeyEquivalent:e];
}

// Clicks on the box itself (between its controls) stay here.
- (void)mouseDown:(NSEvent *)e { (void)e; }

@end
