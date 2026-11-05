// EditorFind.mm — see EditorFind.h. Objective-C++.
//
// Colors: the box follows the theme's darkness (Settings::isDark) with VS
// Code's editor widget colors, and the highlights are VS Code's
// findMatchHighlightBackground (every match) and findMatchBackground (the
// one selected), inline like the terminal's find bar.
#import "EditorFind.h"
#import "AppSettings.h"
#import "Lsp.h"
#include "TextFind.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

static NSColor *FHex(unsigned rgb, CGFloat alpha = 1) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:alpha];
}

static std::u16string FU16(NSString *s) {
    std::u16string u(s.length, u'\0');
    [s getCharacters:(unichar *)u.data() range:NSMakeRange(0, s.length)];
    return u;
}

static NSString *FNS(const std::u16string &u) {
    return [NSString stringWithCharacters:(const unichar *)u.data() length:u.size()];
}

// The most matches found and highlighted; past it the count says "20000+".
static const size_t kMatchLimit = 20000;

// =================================================================== the bar
@interface MCEditorFindBar () <NSTextFieldDelegate>
@property(nonatomic, copy) void (^onChange)(void);               // the query or an option
@property(nonatomic, copy) void (^onNext)(BOOL forward);
@property(nonatomic, copy) void (^onReplace)(BOOL all);
@property(nonatomic, copy) void (^onClose)(void);
@property(nonatomic, copy) void (^onToggleReplace)(void);
+ (CGFloat)width;
- (CGFloat)height;
- (void)setReplaceShown:(BOOL)shown;
- (void)setEditable:(BOOL)editable;
- (void)setOptionsCase:(BOOL)c word:(BOOL)w regex:(BOOL)r;
- (void)applySettings;
- (void)showCount:(NSString *)text error:(BOOL)error tip:(NSString *)tip;
- (void)setNavigable:(BOOL)can;
- (BOOL)isEditing;
- (void)focus:(NSTextField *)field;
- (BOOL)caseOn;
- (BOOL)wordOn;
- (BOOL)regexOn;
@end

@implementation MCEditorFindBar {
    NSTextField *_find, *_replace, *_count;
    NSButton *_chevron, *_case, *_word, *_regex, *_prev, *_next, *_close;
    NSButton *_replaceOne, *_replaceAll;
    BOOL _replaceShown, _editable;
    NSColor *_text;
}

static const CGFloat kRowH = 30;

+ (CGFloat)width { return 470; }
- (BOOL)isFlipped { return YES; }

- (NSButton *)symbol:(NSString *)name tip:(NSString *)tip action:(SEL)action {
    NSImage *img = [NSImage imageWithSystemSymbolName:name accessibilityDescription:tip];
    NSButton *b = [NSButton buttonWithImage:img ?: [[NSImage alloc] init] target:self action:action];
    b.bordered = NO;
    b.toolTip = tip;
    b.refusesFirstResponder = YES;   // the keyboard stays in the field
    [self addSubview:b];
    return b;
}

// VS Code's toggles: plain when off, on a tinted square with an accent
// border when on.
- (NSButton *)toggle:(NSString *)title tip:(NSString *)tip action:(SEL)action {
    NSButton *b = [NSButton buttonWithTitle:title target:self action:action];
    [b setButtonType:NSButtonTypePushOnPushOff];
    b.bordered = NO;
    b.wantsLayer = YES;
    b.layer.cornerRadius = 3;
    b.font = [NSFont systemFontOfSize:12 weight:NSFontWeightMedium];
    b.toolTip = tip;
    b.refusesFirstResponder = YES;
    [self addSubview:b];
    return b;
}

- (NSTextField *)field:(NSString *)placeholder {
    NSTextField *f = [[NSTextField alloc] init];
    f.placeholderString = placeholder;
    f.font = [NSFont systemFontOfSize:12];
    f.bezelStyle = NSTextFieldRoundedBezel;
    f.bezeled = YES;
    f.focusRingType = NSFocusRingTypeNone;
    f.usesSingleLineMode = YES;
    f.cell.scrollable = YES;
    f.delegate = self;
    [self addSubview:f];
    return f;
}

- (NSButton *)textButton:(NSString *)title tip:(NSString *)tip action:(SEL)action {
    NSButton *b = [NSButton buttonWithTitle:title target:self action:action];
    b.bordered = NO;
    b.font = [NSFont systemFontOfSize:11];
    b.toolTip = tip;
    b.refusesFirstResponder = YES;
    [self addSubview:b];
    return b;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        self.layer.cornerRadius = 5;
        self.layer.borderWidth = 1;
        _editable = YES;
        _chevron = [self symbol:@"chevron.right" tip:@"Toggle Replace (⌥⌘F)"
                         action:@selector(chevronClicked:)];
        _find = [self field:@"Find"];
        _replace = [self field:@"Replace"];
        _find.nextKeyView = _replace;
        _replace.nextKeyView = _find;
        _count = [NSTextField labelWithString:@""];
        _count.font = [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular];
        _count.alignment = NSTextAlignmentCenter;
        _count.lineBreakMode = NSLineBreakByTruncatingTail;
        [self addSubview:_count];
        _case = [self toggle:@"Aa" tip:@"Match Case (⌥⌘C)" action:@selector(optionClicked:)];
        _word = [self toggle:@"ab" tip:@"Match Whole Word (⌥⌘W)" action:@selector(optionClicked:)];
        _regex = [self toggle:@".*" tip:@"Use Regular Expression (⌥⌘R)"
                       action:@selector(optionClicked:)];
        _prev = [self symbol:@"arrow.up" tip:@"Previous Match (⇧⏎)" action:@selector(prevClicked:)];
        _next = [self symbol:@"arrow.down" tip:@"Next Match (⏎)" action:@selector(nextClicked:)];
        _close = [self symbol:@"xmark" tip:@"Close (Esc)" action:@selector(closeClicked:)];
        _replaceOne = [self textButton:@"Replace" tip:@"Replace (⇧⌘1)"
                                action:@selector(replaceClicked:)];
        _replaceAll = [self textButton:@"All" tip:@"Replace All (⌥⌘⏎)"
                                action:@selector(replaceAllClicked:)];
        [self setReplaceShown:NO];
        [self applySettings];
    }
    return self;
}

- (NSTextField *)findField { return _find; }
- (NSTextField *)replaceField { return _replace; }
- (NSTextField *)countLabel { return _count; }
- (BOOL)replaceShown { return _replaceShown; }
- (CGFloat)height { return _replaceShown ? 2 * kRowH + 2 : kRowH + 2; }

- (void)setReplaceShown:(BOOL)shown {
    _replaceShown = shown;
    _replace.hidden = _replaceOne.hidden = _replaceAll.hidden = !shown;
    _chevron.image = [NSImage imageWithSystemSymbolName:shown ? @"chevron.down" : @"chevron.right"
                               accessibilityDescription:@"Toggle Replace"];
    self.needsLayout = YES;
}

- (void)setEditable:(BOOL)editable {
    _editable = editable;
    _replaceOne.enabled = _replaceAll.enabled = editable;
    _replace.enabled = editable;
}

- (void)setOptionsCase:(BOOL)c word:(BOOL)w regex:(BOOL)r {
    _case.state = c ? NSControlStateValueOn : NSControlStateValueOff;
    _word.state = w ? NSControlStateValueOn : NSControlStateValueOff;
    _regex.state = r ? NSControlStateValueOn : NSControlStateValueOff;
    [self showToggleStates];
}

- (void)applySettings {
    const Settings &cfg = [AppSettings shared].settings;
    BOOL dark = cfg.isDark();
    self.layer.backgroundColor = (dark ? FHex(0x252526) : FHex(0xF3F3F3)).CGColor;
    self.layer.borderColor = (dark ? FHex(0x454545) : FHex(0xC8C8C8)).CGColor;
    self.layer.shadowOpacity = 0;
    self.appearance = [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua
                                                         : NSAppearanceNameAqua];
    _text = dark ? FHex(0xCCCCCC) : FHex(0x333333);
    for (NSButton *b in @[_chevron, _prev, _next, _close]) b.contentTintColor = _text;
    for (NSButton *b in @[_replaceOne, _replaceAll])
        b.attributedTitle = [[NSAttributedString alloc] initWithString:b.title attributes:@{
            NSFontAttributeName: b.font, NSForegroundColorAttributeName: _text}];
    [self showToggleStates];
}

- (void)showToggleStates {
    NSColor *accent = FHex(0x4EA1F7);
    NSColor *text = _text ?: [NSColor labelColor];
    for (NSButton *b in @[_case, _word, _regex]) {
        BOOL on = b.state == NSControlStateValueOn;
        b.layer.backgroundColor = on ? [accent colorWithAlphaComponent:0.3].CGColor : nil;
        b.layer.borderColor = accent.CGColor;
        b.layer.borderWidth = on ? 1 : 0;
        NSMutableDictionary *attrs = [@{NSFontAttributeName: b.font,
                                        NSForegroundColorAttributeName: text} mutableCopy];
        if (b == _word) attrs[NSUnderlineStyleAttributeName] = @(NSUnderlineStyleSingle);
        b.attributedTitle = [[NSAttributedString alloc] initWithString:b.title attributes:attrs];
    }
}

- (void)layout {
    [super layout];
    const CGFloat W = self.bounds.size.width, pad = 4, btn = 22, tog = 24, gap = 2;
    const CGFloat y0 = 1 + (kRowH - 22) / 2, y1 = 1 + kRowH + (kRowH - 22) / 2;
    _chevron.frame = NSMakeRect(pad, 1, 16, _replaceShown ? 2 * kRowH : kRowH);
    CGFloat x = W - pad - btn;
    _close.frame = NSMakeRect(x, y0, btn, btn);
    x -= btn + gap;
    _next.frame = NSMakeRect(x, y0, btn, btn);
    x -= btn;
    _prev.frame = NSMakeRect(x, y0, btn, btn);
    x -= 74;
    _count.frame = NSMakeRect(x, y0 + 3, 72, 16);
    x -= tog + gap;
    _regex.frame = NSMakeRect(x, y0 + 1, tog, 20);
    x -= tog;
    _word.frame = NSMakeRect(x, y0 + 1, tog, 20);
    x -= tog;
    _case.frame = NSMakeRect(x, y0 + 1, tog, 20);
    const CGFloat fieldX = pad + 18;
    const CGFloat fieldW = MAX(60, x - gap - fieldX);
    _find.frame = NSMakeRect(fieldX, y0, fieldW, 22);
    _replace.frame = NSMakeRect(fieldX, y1, fieldW, 22);
    _replaceOne.frame = NSMakeRect(fieldX + fieldW + 6, y1 + 1, 54, 20);
    _replaceAll.frame = NSMakeRect(fieldX + fieldW + 62, y1 + 1, 30, 20);
}

- (void)showCount:(NSString *)text error:(BOOL)error tip:(NSString *)tip {
    _count.stringValue = text ?: @"";
    _count.textColor = error ? FHex(0xF14C4C) : [_text colorWithAlphaComponent:0.75];
    _count.toolTip = tip;
}

- (void)setNavigable:(BOOL)can {
    _prev.enabled = _next.enabled = can;
}

- (BOOL)isEditing:(NSTextField *)f {
    id fr = self.window.firstResponder;
    return [fr isKindOfClass:[NSTextView class]] && ((NSTextView *)fr).delegate == (id)f;
}
- (BOOL)isEditing { return [self isEditing:_find] || [self isEditing:_replace]; }

- (void)focus:(NSTextField *)f {
    [self.window makeFirstResponder:f];
    [f.currentEditor selectAll:nil];
}

// ------------------------------------------------------------ actions
- (void)chevronClicked:(id)sender {
    (void)sender;
    if (self.onToggleReplace) self.onToggleReplace();
}
- (void)optionClicked:(id)sender {
    (void)sender;
    [self showToggleStates];
    if (self.onChange) self.onChange();
}
- (BOOL)caseOn { return _case.state == NSControlStateValueOn; }
- (BOOL)wordOn { return _word.state == NSControlStateValueOn; }
- (BOOL)regexOn { return _regex.state == NSControlStateValueOn; }
- (void)prevClicked:(id)sender { (void)sender; if (self.onNext) self.onNext(NO); }
- (void)nextClicked:(id)sender { (void)sender; if (self.onNext) self.onNext(YES); }
- (void)closeClicked:(id)sender { (void)sender; if (self.onClose) self.onClose(); }
- (void)replaceClicked:(id)sender { (void)sender; if (self.onReplace) self.onReplace(NO); }
- (void)replaceAllClicked:(id)sender { (void)sender; if (self.onReplace) self.onReplace(YES); }

- (void)controlTextDidChange:(NSNotification *)note {
    if (note.object == _find && self.onChange) self.onChange();
}

- (BOOL)control:(NSControl *)control textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
    (void)tv;
    if (sel == @selector(insertNewline:) || sel == @selector(insertNewlineIgnoringFieldEditor:) ||
        sel == @selector(insertLineBreak:)) {
        NSEventModifierFlags f = NSApp.currentEvent.modifierFlags;
        if (control == _replace) {
            if (self.onReplace) self.onReplace((f & NSEventModifierFlagCommand) != 0);
        } else if (self.onNext) {
            self.onNext(!(f & NSEventModifierFlagShift));
        }
        return YES;
    }
    if (sel == @selector(cancelOperation:)) {
        if (self.onClose) self.onClose();
        return YES;
    }
    if (sel == @selector(insertTab:) || sel == @selector(insertBacktab:)) {
        if (!_replaceShown) return YES;   // nowhere else to go in the bar
        [self focus:control == _find ? _replace : _find];
        return YES;
    }
    return NO;
}

// The bar's keys while one of its fields has the keyboard. The window offers
// key equivalents to every view, so only while editing.
- (BOOL)performKeyEquivalent:(NSEvent *)e {
    if (![self isEditing]) return [super performKeyEquivalent:e];
    NSEventModifierFlags f = e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    NSEventModifierFlags mods = f & (NSEventModifierFlagCommand | NSEventModifierFlagShift |
                                     NSEventModifierFlagOption | NSEventModifierFlagControl);
    const NSEventModifierFlags cmd = NSEventModifierFlagCommand, opt = NSEventModifierFlagOption,
                               shift = NSEventModifierFlagShift;
    NSString *ch = e.charactersIgnoringModifiers.lowercaseString;
    const unsigned short key = e.keyCode;
    if (mods == cmd && [ch isEqualToString:@"f"]) { [self focus:_find]; return YES; }
    if (mods == (cmd | opt) && key == 3 /* kVK_ANSI_F */) {
        if (!_replaceShown && self.onToggleReplace) self.onToggleReplace();
        [self focus:_replace];
        return YES;
    }
    if ([ch isEqualToString:@"g"] && (mods == cmd || mods == (cmd | shift))) {
        if (self.onNext) self.onNext(!(mods & shift));
        return YES;
    }
    if (mods == (cmd | opt)) {
        NSButton *t = key == 8 /* C */ ? _case : key == 13 /* W */ ? _word
                    : key == 15 /* R */ ? _regex : nil;
        if (t) {
            t.state = t.state == NSControlStateValueOn ? NSControlStateValueOff
                                                       : NSControlStateValueOn;
            [self optionClicked:t];
            return YES;
        }
        if (key == 36 || key == 76) {   // Return, keypad Enter
            if (self.onReplace) self.onReplace(YES);
            return YES;
        }
    }
    if (mods == (cmd | shift) && key == 18 /* kVK_ANSI_1 */) {
        if (self.onReplace) self.onReplace(NO);
        return YES;
    }
    if (mods == cmd && (key == 36 || key == 76) && [self isEditing:_replace]) {
        if (self.onReplace) self.onReplace(YES);
        return YES;
    }
    return [super performKeyEquivalent:e];
}

// Clicks on the box itself (between its controls) stay here.
- (void)mouseDown:(NSEvent *)e { (void)e; }

@end

// ============================================================ the controller
@implementation MCEditorFind {
    __weak CodeTextView *_tv;
    MCEditorFindBar *_bar;
    BOOL _shown, _visible;
    dispatch_queue_t _queue;
    std::shared_ptr<std::atomic<bool>> _cancel;

    TextFind::Query _query;
    NSUInteger _queryGen;        // bumped by every change of the query or options
    NSUInteger _textVersion;     // bumped by every edit of the characters
    NSUInteger _resultsGen, _resultsVersion;
    std::vector<TextFind::Match> _matches;
    BOOL _truncated, _searching;
    BOOL _selectOnArrival;       // the query changed: select the next match from the caret
    NSUInteger _anchor;
    void (^_whenCurrent)(void);  // waiting for results for the text as it is
    id _observer;
}

- (instancetype)initWithTextView:(CodeTextView *)textView {
    if ((self = [super init])) {
        _tv = textView;
        _queue = dispatch_queue_create("minicode.editor-find", DISPATCH_QUEUE_SERIAL);
        _bar = [[MCEditorFindBar alloc] initWithFrame:NSMakeRect(0, 0, [MCEditorFindBar width], 32)];
        _bar.hidden = YES;
        __weak MCEditorFind *weakSelf = self;
        _bar.onChange = ^{ [weakSelf queryChanged]; };
        _bar.onNext = ^(BOOL forward) { [weakSelf findNext:forward]; };
        _bar.onReplace = ^(BOOL all) {
            if (all) [weakSelf replaceAll];
            else [weakSelf replaceOne];
        };
        _bar.onClose = ^{ [weakSelf close]; };
        _bar.onToggleReplace = ^{ [weakSelf setReplaceShown:!weakSelf.bar.replaceShown]; };
        textView.drawBehindText = ^(NSRect rect) { [weakSelf drawHighlightsInRect:rect]; };
        textView.onFindAction = ^BOOL(NSInteger tag) { return [weakSelf performFindAction:tag]; };
        textView.validateFindAction = ^BOOL(NSInteger tag) {
            return [weakSelf validateFindAction:tag];
        };
        _observer = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSTextStorageDidProcessEditingNotification
                        object:textView.textStorage
                         queue:nil
                    usingBlock:^(NSNotification *note) {
            NSTextStorage *ts = note.object;
            if (ts.editedMask & NSTextStorageEditedCharacters) [weakSelf textEdited];
        }];
    }
    return self;
}

- (void)dealloc {
    if (_observer) [[NSNotificationCenter defaultCenter] removeObserver:_observer];
    if (_cancel) _cancel->store(true);
}

- (MCEditorFindBar *)bar { return _bar; }
- (BOOL)shown { return _shown; }

- (BOOL)matchCase { return [_bar caseOn]; }
- (BOOL)wholeWord { return [_bar wordOn]; }
- (BOOL)regex { return [_bar regexOn]; }
- (void)setMatchCase:(BOOL)v { [_bar setOptionsCase:v word:self.wholeWord regex:self.regex]; [self queryChanged]; }
- (void)setWholeWord:(BOOL)v { [_bar setOptionsCase:self.matchCase word:v regex:self.regex]; [self queryChanged]; }
- (void)setRegex:(BOOL)v { [_bar setOptionsCase:self.matchCase word:self.wholeWord regex:v]; [self queryChanged]; }
- (void)toggleMatchCase { self.matchCase = !self.matchCase; }
- (void)toggleWholeWord { self.wholeWord = !self.wholeWord; }
- (void)toggleRegex { self.regex = !self.regex; }

- (void)applySettings {
    [_bar applySettings];
    [self showCount];
    if (_shown) _tv.needsDisplay = YES;
}

// ------------------------------------------------------------- layout
- (void)layoutInEditorRect:(NSRect)rect visible:(BOOL)visible {
    _visible = visible;
    _bar.hidden = !(_shown && visible);
    if (_bar.hidden) return;
    const CGFloat w = MIN([MCEditorFindBar width], MAX(200, NSWidth(rect) - 28));
    const CGFloat h = [_bar height];
    // At the top right, clear of the vertical scroller, as VS Code puts it.
    _bar.frame = NSMakeRect(NSMaxX(rect) - w - 18, NSMaxY(rect) - h, w, h);
}

- (void)relayout {
    if (self.onLayoutNeeded) self.onLayoutNeeded();
}

- (void)setReplaceShown:(BOOL)shown {
    [_bar setReplaceShown:shown];
    [self relayout];
}

// ------------------------------------------------------------- open, close
- (BOOL)editable { return _tv.editable; }

// The selection, when it is a piece of one line, seeds the query.
- (NSString *)seedFromSelection {
    CodeTextView *tv = _tv;
    NSRange sel = tv.selectedRange;
    if (sel.length == 0 || sel.length > 500) return nil;
    NSString *s = [tv.string substringWithRange:sel];
    if ([s rangeOfCharacterFromSet:[NSCharacterSet newlineCharacterSet]].location != NSNotFound)
        return nil;
    return s;
}

- (void)showFocusing:(NSTextField *)field replace:(BOOL)replace {
    NSString *seed = [self seedFromSelection];
    if (seed) {
        if (self.regex) seed = [NSRegularExpression escapedPatternForString:seed];
        _bar.findField.stringValue = seed;
    }
    BOOL wasShown = _shown;
    _shown = YES;
    [_bar setEditable:[self editable]];
    if (replace && !_bar.replaceShown) [_bar setReplaceShown:YES];
    [self relayout];
    if (field && !_bar.hidden) [_bar focus:field];
    if (!wasShown || seed) [self queryChanged];
}

- (void)close {
    if (!_shown) return;
    BOOL hadFocus = [_bar isEditing];
    _shown = NO;
    _bar.hidden = YES;
    if (_cancel) _cancel->store(true);
    _whenCurrent = nil;
    _selectOnArrival = NO;
    _tv.needsDisplay = YES;
    [self relayout];
    if (hadFocus && _tv.window && !_tv.hiddenOrHasHiddenAncestor)
        [_tv.window makeFirstResponder:_tv];
}

- (BOOL)closeFromEditor {
    if (!_shown) return NO;
    [self close];
    return YES;
}

// ------------------------------------------------------------- the menu
- (BOOL)validateFindAction:(NSInteger)tag {
    switch (tag) {
    case NSTextFinderActionShowFindInterface:
    case NSTextFinderActionNextMatch:
    case NSTextFinderActionPreviousMatch:
        return YES;
    case NSTextFinderActionSetSearchString:
        return _tv.selectedRange.length > 0;
    case NSTextFinderActionShowReplaceInterface:
    case NSTextFinderActionReplaceAll:
    case NSTextFinderActionReplaceAndFind:
    case NSTextFinderActionReplace:
        return [self editable];
    default:
        return NO;
    }
}

- (BOOL)performFindAction:(NSInteger)tag {
    if (![self validateFindAction:tag]) {
        NSBeep();
        return YES;
    }
    switch (tag) {
    case NSTextFinderActionShowFindInterface:
        [self showFocusing:_bar.findField replace:NO];
        return YES;
    case NSTextFinderActionShowReplaceInterface:
        [self showFocusing:_bar.replaceField replace:YES];
        return YES;
    case NSTextFinderActionNextMatch:
    case NSTextFinderActionPreviousMatch: {
        BOOL forward = tag == NSTextFinderActionNextMatch;
        if (!_bar.findField.stringValue.length) {
            [self showFocusing:_bar.findField replace:NO];
            return YES;
        }
        if (!_shown) {   // the last query again, the keyboard left where it is
            _shown = YES;
            [_bar setEditable:[self editable]];
            [self relayout];
            [self queryChanged];
            _selectOnArrival = NO;
        }
        [self findNext:forward];
        return YES;
    }
    case NSTextFinderActionSetSearchString: {
        NSString *s = [_tv.string substringWithRange:_tv.selectedRange];
        if (self.regex) s = [NSRegularExpression escapedPatternForString:s];
        _bar.findField.stringValue = s;
        [self queryChanged];
        _selectOnArrival = NO;   // the selection is the match already
        return YES;
    }
    case NSTextFinderActionReplaceAndFind:
    case NSTextFinderActionReplace:
        if (!_shown) [self showFocusing:nil replace:YES];
        [self replaceOne];
        return YES;
    case NSTextFinderActionReplaceAll:
        if (!_shown) [self showFocusing:nil replace:YES];
        [self replaceAll];
        return YES;
    }
    return NO;
}

// ------------------------------------------------------------- searching
- (void)textEdited {
    _textVersion++;
    if (!_shown) return;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(search)
                                               object:nil];
    [self performSelector:@selector(search) withObject:nil afterDelay:0.1];
}

- (void)queryChanged {
    TextFind::Options o;
    o.caseSensitive = self.matchCase;
    o.wholeWord = self.wholeWord;
    o.regex = self.regex;
    _query = TextFind::Query::make(FU16(_bar.findField.stringValue), o);
    _queryGen++;
    _anchor = _tv.selectedRange.location;
    _selectOnArrival = YES;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(search)
                                               object:nil];
    [self search];
}

- (BOOL)resultsCurrent {
    return _resultsGen == _queryGen && _resultsVersion == _textVersion;
}

- (void)search {
    if (_cancel) _cancel->store(true);
    _cancel = nullptr;
    if (!_shown) return;
    const NSUInteger gen = _queryGen, version = _textVersion;
    if (!_query.usable()) {
        [self arrived:std::vector<TextFind::Match>() truncated:NO gen:gen version:version];
        return;
    }
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    _cancel = cancel;
    _searching = YES;
    std::u16string text = FU16(_tv.string);
    TextFind::Query query = _query;
    __weak MCEditorFind *weakSelf = self;
    dispatch_async(_queue, ^{
        if (cancel->load()) return;
        bool truncated = false, cancelled = false;
        std::vector<TextFind::Match> found =
            TextFind::findAll(text, query, kMatchLimit, &truncated, cancel.get(), &cancelled);
        if (cancelled) return;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (cancel->load()) return;
            [weakSelf arrived:found truncated:truncated gen:gen version:version];
        });
    });
    [self showCount];
}

- (void)arrived:(const std::vector<TextFind::Match> &)found truncated:(BOOL)truncated
            gen:(NSUInteger)gen version:(NSUInteger)version {
    if (gen != _queryGen || version != _textVersion) return;   // a newer one is on its way
    _searching = NO;
    _cancel = nullptr;
    _matches = found;
    _truncated = truncated;
    _resultsGen = gen;
    _resultsVersion = version;
    if (_selectOnArrival) {
        _selectOnArrival = NO;
        long i = TextFind::nextIndex(_matches, _anchor, _anchor, true);
        if (i >= 0) [self selectMatch:(size_t)i];
    }
    [self showCount];
    _tv.needsDisplay = YES;
    void (^then)(void) = _whenCurrent;
    _whenCurrent = nil;
    if (then) then();
}

// Run `block` once there are results for the query and text as they are.
- (void)whenCurrent:(void (^)(void))block {
    if ([self resultsCurrent]) {
        block();
        return;
    }
    _whenCurrent = [block copy];
    if (!_cancel) [self search];
}

- (NSInteger)currentIndex {
    if (![self resultsCurrent]) return -1;
    NSRange sel = _tv.selectedRange;
    return TextFind::indexOf(_matches, sel.location, sel.length);
}

- (NSArray<NSValue *> *)matchRanges {
    NSMutableArray *a = [NSMutableArray arrayWithCapacity:_matches.size()];
    for (const TextFind::Match &m : _matches)
        [a addObject:[NSValue valueWithRange:NSMakeRange(m.start, m.length)]];
    return a;
}

- (void)showCount {
    if (!_bar.findField.stringValue.length) {
        [_bar showCount:@"" error:NO tip:nil];
        [_bar setNavigable:NO];
        return;
    }
    if (!_query.error().empty()) {
        [_bar showCount:@"Invalid" error:YES tip:[NSString stringWithUTF8String:_query.error().c_str()]];
        [_bar setNavigable:NO];
        return;
    }
    if (![self resultsCurrent]) {
        if (_resultsGen != _queryGen) [_bar showCount:@"…" error:NO tip:nil];
        return;   // the old count stands while the text is searched again
    }
    const size_t n = _matches.size();
    [_bar setNavigable:n > 0];
    if (n == 0) {
        [_bar showCount:@"No results" error:NO tip:nil];
        return;
    }
    NSString *total = _truncated ? [NSString stringWithFormat:@"%zu+", n]
                                 : [NSString stringWithFormat:@"%zu", n];
    NSInteger i = [self currentIndex];
    [_bar showCount:i >= 0 ? [NSString stringWithFormat:@"%ld of %@", (long)i + 1, total]
                           : [NSString stringWithFormat:@"? of %@", total]
              error:NO
                tip:_truncated ? @"Only the first 20,000 matches are counted and highlighted"
                               : nil];
}

- (void)selectMatch:(size_t)i {
    CodeTextView *tv = _tv;
    if (i >= _matches.size()) return;
    NSRange r = NSMakeRange(_matches[i].start, _matches[i].length);
    if (NSMaxRange(r) > tv.string.length) return;
    [tv setSelectedRange:r];
    [tv scrollRangeToVisible:r];
    [self showCount];
}

- (void)findNext:(BOOL)forward {
    if (!_query.usable()) {
        NSBeep();
        return;
    }
    __weak MCEditorFind *weakSelf = self;
    [self whenCurrent:^{
        MCEditorFind *s = weakSelf;
        if (!s) return;
        NSRange sel = s->_tv.selectedRange;
        long i = TextFind::nextIndex(s->_matches, sel.location, NSMaxRange(sel), forward);
        if (i < 0) {
            NSBeep();
            return;
        }
        [s selectMatch:(size_t)i];
    }];
}

// ------------------------------------------------------------- replacing
- (std::u16string)replacement { return FU16(_bar.replaceField.stringValue); }

// VS Code's Replace: the selected match is replaced and the next one
// selected; with no match selected it only goes to the next.
- (void)replaceOne {
    if (![self editable] || !_query.usable()) {
        NSBeep();
        return;
    }
    __weak MCEditorFind *weakSelf = self;
    [self whenCurrent:^{
        MCEditorFind *s = weakSelf;
        if (!s) return;
        NSInteger i = [s currentIndex];
        if (i < 0) {
            [s findNext:YES];
            return;
        }
        std::u16string text = FU16(s->_tv.string);
        const TextFind::Match m = s->_matches[(size_t)i];
        std::u16string with = TextFind::expand([s replacement], text, m, s->_query);
        std::vector<TextFind::Edit16> edits{{m.start, m.length, with}};
        if (![s applyEdits:edits actionName:@"Replace"]) return;
        // On to the next match after the replacement.
        s->_anchor = m.start + with.size();
        s->_selectOnArrival = YES;
        [s->_tv setSelectedRange:NSMakeRange(s->_anchor, 0)];
        [s search];
    }];
}

- (void)replaceAll {
    if (![self editable] || !_query.usable()) {
        NSBeep();
        return;
    }
    const NSUInteger version = _textVersion;
    std::u16string text = FU16(_tv.string);
    std::u16string with = [self replacement];
    TextFind::Query query = _query;
    __weak MCEditorFind *weakSelf = self;
    dispatch_async(_queue, ^{
        std::vector<TextFind::Edit16> edits = TextFind::replaceAll(text, query, with);
        dispatch_async(dispatch_get_main_queue(), ^{
            MCEditorFind *s = weakSelf;
            if (!s) return;
            if (version != s->_textVersion) {   // typed meanwhile: again, on the new text
                [s replaceAll];
                return;
            }
            if (edits.empty()) {
                NSBeep();
                if (s.onNote) s.onNote(@"Nothing to replace");
                return;
            }
            if ([s applyEdits:edits actionName:@"Replace All"] && s.onNote)
                s.onNote([NSString stringWithFormat:@"Replaced %zu %@", edits.size(),
                                                    edits.size() == 1 ? @"match" : @"matches"]);
        });
    });
}

// The edits (ascending) in one undo step, through the text view, as Format
// Document and rename make theirs. The caret keeps its place in the text
// around it.
- (BOOL)applyEdits:(const std::vector<TextFind::Edit16> &)edits actionName:(NSString *)name {
    CodeTextView *tv = _tv;
    if (!tv || edits.empty()) return NO;
    NSMutableArray<NSValue *> *ranges = [NSMutableArray arrayWithCapacity:edits.size()];
    NSMutableArray<NSString *> *strings = [NSMutableArray arrayWithCapacity:edits.size()];
    for (const TextFind::Edit16 &e : edits) {
        [ranges addObject:[NSValue valueWithRange:NSMakeRange(e.start, e.length)]];
        [strings addObject:FNS(e.text)];
    }
    NSRange sel = tv.selectedRange;
    NSUndoManager *um = tv.undoManager;
    [tv breakUndoCoalescing];
    [um beginUndoGrouping];
    const BOOL ok = [tv shouldChangeTextInRanges:ranges replacementStrings:strings];
    if (ok) {
        NSTextStorage *ts = tv.textStorage;
        [ts beginEditing];
        for (NSInteger k = (NSInteger)ranges.count - 1; k >= 0; k--)   // last first
            [ts replaceCharactersInRange:ranges[(NSUInteger)k].rangeValue
                              withString:strings[(NSUInteger)k]];
        [ts endEditing];
        [tv didChangeText];
        NSUInteger at = sel.location;
        for (auto e = edits.rbegin(); e != edits.rend(); ++e) {
            const NSUInteger end = e->start + e->length;
            if (at >= end) at = at - e->length + e->text.size();
            else if (at > e->start) at = e->start + MIN(at - e->start, (NSUInteger)e->text.size());
        }
        if (at <= tv.string.length) [tv setSelectedRange:NSMakeRange(at, 0)];
    }
    [um setActionName:name];
    [um endUndoGrouping];
    [tv breakUndoCoalescing];
    if (!ok) NSBeep();
    return ok;
}

// ------------------------------------------------------------- drawing
// Every match on screen, behind the text (the selection, the current match,
// is drawn over it by the text view).
- (void)drawHighlightsInRect:(NSRect)rect {
    if (!_shown || ![self resultsCurrent] || _matches.empty()) return;
    CodeTextView *tv = _tv;
    NSLayoutManager *lm = tv.layoutManager;
    if (!lm) return;
    NSRect r = NSOffsetRect(rect, -tv.textContainerOrigin.x, -tv.textContainerOrigin.y);
    NSRange glyphs = [lm glyphRangeForBoundingRectWithoutAdditionalLayout:r
                                                          inTextContainer:tv.textContainer];
    NSRange chars = [lm characterRangeForGlyphRange:glyphs actualGlyphRange:NULL];
    const BOOL dark = [AppSettings shared].settings.isDark();
    NSColor *other = FHex(0xEA5C00, 0.33);
    NSColor *current = dark ? FHex(0x515C6A) : FHex(0xA8AC94);
    const NSInteger cur = [self currentIndex];
    auto it = std::lower_bound(_matches.begin(), _matches.end(), chars.location,
                               [](const TextFind::Match &m, size_t at) {
                                   return m.start + m.length < at;
                               });
    for (; it != _matches.end() && it->start <= NSMaxRange(chars); ++it) {
        if (it->length == 0) continue;
        NSColor *c = (it - _matches.begin()) == cur ? current : other;
        [c setFill];
        [tv enumerateRectsForRange:NSMakeRange(it->start, it->length) block:^(NSRect box) {
            if (NSIntersectsRect(box, rect))
                NSRectFillUsingOperation(box, NSCompositingOperationSourceOver);
        }];
    }
}

@end
