// EditorFind.mm — see EditorFind.h. Objective-C++.
//
// Colors: the box, fields and toggles are the theme's editor widget colors
// (MCFindStyle, FindWidget.h), and the highlights are VS Code's
// findMatchHighlightBackground (every match), findMatchBackground (the one
// selected) and findRangeHighlightBackground (Find in Selection's scope).
#import "EditorFind.h"
#include "FindHistory.h"
#import "AppSettings.h"
#import "FindWidget.h"
#import "Lsp.h"
#import "PieceString.h"
#include "LargeFile.h"
#include "MultiCaret.h"
#include "TextFind.h"
#include "TextScan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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

// The text a search reads, as of one version of the storage's characters:
// an immutable copy of the string made on the main thread (a copy of the
// characters, about 0.13 ms for 2.4 MB; no copy at all of a large file's,
// whose copy shares its pieces, PieceString.h), read on the search queue a
// block of lines at a time (TextFind's Reader), never turned into one
// UTF-16 string: a 260 MB log's was 520 MB. Made once per edit of the text,
// not once per keystroke in the find field (every keystroke copied the
// whole buffer on the main thread).
struct MCFindText {
    NSString *source;
    size_t length() const { return source.length; }
    TextFind::Reader reader() const {
        NSString *s = source;
        return [s](size_t start, size_t n, char16_t *out) {
            [s getCharacters:(unichar *)out range:NSMakeRange(start, n)];
        };
    }
    // For TextScan: the file's ASCII bytes themselves while the text is
    // the one read (PieceString.h's MCHeldAsciiBytes), else its UTF-16 read
    // in blocks. `source` keeps the bytes alive while the queue reads them.
    TextScan::Text scanText() const {
        NSUInteger n = 0;
        const char *bytes = MCHeldAsciiBytes(source, &n);
        if (bytes) return TextScan::Text::utf8(bytes, n, true);
        return TextScan::Text::utf16(length(), reader());
    }
};

// A text this long is searched in stages (round 9's C3: the find bar took
// about 2 s to show the last line of a 259 MB log, with the old query's
// marks left on every line meanwhile): the lines on screen first, then the
// first match from the caret, selected at once ("1 of …"), then every match
// counted from the top, all through TextScan (the needle found in the bytes
// before any line is matched), on the queue and cancelled by the next key.
// Under it a whole search takes a few ms and arrives at once.
static const NSUInteger kStagedUnits = 1u << 20;
// Units searched each side of a match selected in stages, for its marks.
static const size_t kAroundUnits = 16384;

// What a staged search sends the main thread, a part at a time.
struct MCStagedNews {
    std::vector<TextFind::Match> found;   // in order
    bool hasFirst = false;                // `first` is the match to select
    TextFind::Match first;
    long firstIndex = -1;                 // matches before `first`, once counted
    size_t counted = 0;                   // matches counted so far
    size_t completeTo = 0;                // found holds every match starting before here
    bool counting = false;                // from the counting pass
    bool done = false, truncated = false;
    double firstCpuMs = -1;               // the queue's CPU time to `first`
};

// This thread's CPU time, in ms (a timing that holds on a loaded Mac).
static double MCThreadCpuMs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

// A match wholly in Find in Selection's scope (or any, with none).
static bool MCInScope(const TextFind::Match &m, NSRange scope) {
    return scope.location == NSNotFound ||
           (m.start >= scope.location && m.start + m.length <= NSMaxRange(scope));
}

// The first or last match in scope starting in [from, to); *cancelled when
// the flag stopped it.
static bool MCFirstIn(const TextScan::Text &t, const TextFind::Query &q, size_t from, size_t to,
                      NSRange scope, const TextScan::Options &o, TextFind::Match *out,
                      bool *cancelled) {
    bool got = false;
    *cancelled = !TextScan::scan(t, q, from, to, [&](const TextFind::Match &m) {
        if (!MCInScope(m, scope)) return true;
        *out = m;
        got = true;
        return false;
    }, o);
    return got && !*cancelled;
}
static bool MCLastIn(const TextScan::Text &t, const TextFind::Query &q, size_t from, size_t to,
                     NSRange scope, const TextScan::Options &o, TextFind::Match *out,
                     bool *cancelled) {
    if (scope.location == NSNotFound)
        return TextScan::findLast(t, q, from, to, out, o, cancelled) && !*cancelled;
    bool got = false;
    *cancelled = !TextScan::scan(t, q, from, to, [&](const TextFind::Match &m) {
        if (MCInScope(m, scope)) {
            *out = m;
            got = true;
        }
        return true;
    }, o);
    return got && !*cancelled;
}

// Matches in [from, to) in scope, at most `most`, in order.
static bool MCMatchesIn(const TextScan::Text &t, const TextFind::Query &q, size_t from, size_t to,
                        NSRange scope, const TextScan::Options &o, size_t most,
                        std::vector<TextFind::Match> *out) {
    return TextScan::scan(t, q, from, to, [&](const TextFind::Match &m) {
        if (MCInScope(m, scope)) out->push_back(m);
        return out->size() < most;
    }, o);
}

// `add` (in order) merged into `into` (in order), each match once.
static void MCMergeMatches(std::vector<TextFind::Match> &into, std::vector<TextFind::Match> add) {
    if (add.empty()) return;
    auto before = [](const TextFind::Match &a, const TextFind::Match &b) {
        return a.start < b.start || (a.start == b.start && a.length < b.length);
    };
    if (into.empty() || before(into.back(), add.front())) {
        into.insert(into.end(), std::make_move_iterator(add.begin()),
                    std::make_move_iterator(add.end()));
        return;
    }
    std::vector<TextFind::Match> out;
    out.reserve(into.size() + add.size());
    std::merge(std::make_move_iterator(into.begin()), std::make_move_iterator(into.end()),
               std::make_move_iterator(add.begin()), std::make_move_iterator(add.end()),
               std::back_inserter(out), before);
    out.erase(std::unique(out.begin(), out.end(),
                          [](const TextFind::Match &a, const TextFind::Match &b) {
                              return a.start == b.start && a.length == b.length;
                          }),
              out.end());
    into = std::move(out);
}

// The most matches found and highlighted; past it the count says "20000+".
static const size_t kMatchLimit = 20000;

// VS Code's geometry (findWidget.ts and findWidget.css).
static const CGFloat kWidth = 419;          // FIND_WIDGET_INITIAL_WIDTH
static const CGFloat kNarrowWidth = 257;    // .narrow-find-widget
static const CGFloat kCollapsedWidth = 170; // .collapsed-find-widget
static const CGFloat kCountWidth = 69;      // MAX_MATCHES_COUNT_WIDTH
static const CGFloat kInputH = 25;          // an input with its border
static const CGFloat kTop = 4;              // above the find row, and between rows
static const CGFloat kInputX = 27;          // border 1 + padding 9 + margin 17
static const CGFloat kPartRight = 30;       // border 1 + padding 4 + margin 25

// =================================================================== the bar
@interface MCEditorFindBar () <NSTextFieldDelegate>
@property(nonatomic, copy) void (^onChange)(void);               // the query or an option
@property(nonatomic, copy) void (^onNext)(BOOL forward);
@property(nonatomic, copy) void (^onReplace)(BOOL all);
@property(nonatomic, copy) void (^onClose)(void);
@property(nonatomic, copy) void (^onToggleReplace)(void);
@property(nonatomic, copy) void (^onToggleSelection)(void);
@property(nonatomic, copy) void (^onResized)(void);
- (CGFloat)height;
- (CGFloat)widthForEditorWidth:(CGFloat)editorWidth;
- (void)setReplaceShown:(BOOL)shown;
- (void)setEditable:(BOOL)editable;
- (void)setOptionsCase:(BOOL)c word:(BOOL)w regex:(BOOL)r;
- (void)applySettings;
- (void)showCount:(NSString *)text error:(BOOL)error tip:(NSString *)tip;
- (void)setNoResults:(BOOL)none;
- (void)setNavigable:(BOOL)can;
- (void)setSelectionOn:(BOOL)on enabled:(BOOL)enabled;
- (BOOL)isEditing;
- (void)focus:(NSTextField *)field;
- (BOOL)caseOn;
- (BOOL)wordOn;
- (BOOL)regexOn;
- (BOOL)preserveOn;
- (void)setPreserveOn:(BOOL)on;
@end

// What was searched for and replaced with, for Up and Down in the fields
// (VS Code's find widget history), shared by every window's bar.
static FindHistory &MCFindQueryHistory() { static FindHistory h; return h; }
static FindHistory &MCFindReplaceHistory() { static FindHistory h; return h; }

@implementation MCEditorFindBar {
    MCFindStyle *_style;
    MCFindInput *_findInput, *_replaceInput;
    NSTextField *_count;
    MCFindButton *_chevron, *_case, *_word, *_regex, *_preserve;
    MCFindButton *_prev, *_next, *_selection, *_close, *_replaceOne, *_replaceAll;
    BOOL _replaceShown, _editable;
    NSInteger _mode;
    CGFloat _editorWidth;
    CGFloat _userWidth;          // dragged wider by its left edge; 0 when not
}

- (BOOL)isFlipped { return YES; }

- (MCFindButton *)button:(MCFindIcon)icon tip:(NSString *)tip action:(SEL)action {
    MCFindButton *b = [MCFindButton button:icon tip:tip target:self action:action];
    [self addSubview:b];
    return b;
}

- (MCFindButton *)option:(MCFindIcon)icon tip:(NSString *)tip {
    return [MCFindButton toggle:icon tip:tip target:self action:@selector(optionClicked:)];
}

- (MCFindInput *)input:(NSString *)placeholder toggles:(NSArray<MCFindButton *> *)toggles {
    MCFindInput *in = [[MCFindInput alloc] initWithPlaceholder:placeholder toggles:toggles];
    in.field.delegate = self;
    [self addSubview:in];
    return in;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        self.layer.cornerRadius = 8;    // --vscode-cornerRadius-large
        self.layer.borderWidth = 1;
        self.layer.masksToBounds = NO;
        _editable = YES;
        _chevron = [self button:MCFindIconChevronRight tip:@"Toggle Replace (⌥⌘F)"
                         action:@selector(chevronClicked:)];
        _chevron.radius = 4;
        _case = [self option:MCFindIconCase tip:@"Match Case (⌥⌘C)"];
        _word = [self option:MCFindIconWord tip:@"Match Whole Word (⌥⌘W)"];
        _regex = [self option:MCFindIconRegex tip:@"Use Regular Expression (⌥⌘R)"];
        _preserve = [self option:MCFindIconPreserveCase tip:@"Preserve Case (⌥⌘P)"];
        _findInput = [self input:@"Find" toggles:@[_case, _word, _regex]];
        _replaceInput = [self input:@"Replace" toggles:@[_preserve]];
        _findInput.field.nextKeyView = _replaceInput.field;
        _replaceInput.field.nextKeyView = _findInput.field;
        _count = [NSTextField labelWithString:@""];
        _count.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
        _count.alignment = NSTextAlignmentCenter;
        _count.lineBreakMode = NSLineBreakByClipping;
        [self addSubview:_count];
        _prev = [self button:MCFindIconUp tip:@"Previous Match (⇧⏎)" action:@selector(prevClicked:)];
        _next = [self button:MCFindIconDown tip:@"Next Match (⏎)" action:@selector(nextClicked:)];
        _selection = [MCFindButton toggle:MCFindIconSelection tip:@"Find in Selection (⌥⌘L)"
                                   target:self action:@selector(selectionClicked:)];
        _selection.frame = NSMakeRect(0, 0, 22, 22);
        [self addSubview:_selection];
        _close = [self button:MCFindIconClose tip:@"Close (Esc)" action:@selector(closeClicked:)];
        _replaceOne = [self button:MCFindIconReplace tip:@"Replace (⇧⌘1)"
                            action:@selector(replaceClicked:)];
        _replaceAll = [self button:MCFindIconReplaceAll tip:@"Replace All (⌥⌘⏎)"
                            action:@selector(replaceAllClicked:)];
        [self setReplaceShown:NO];
        [self setSelectionOn:NO enabled:NO];
        [self applySettings];
    }
    return self;
}

- (NSTextField *)findField { return _findInput.field; }
- (NSTextField *)replaceField { return _replaceInput.field; }
- (NSTextField *)countLabel { return _count; }
- (BOOL)replaceShown { return _replaceShown; }
- (MCFindInput *)findInput { return _findInput; }
- (MCFindInput *)replaceInput { return _replaceInput; }
- (MCFindButton *)toggleReplaceButton { return _chevron; }
- (MCFindButton *)previousButton { return _prev; }
- (MCFindButton *)nextButton { return _next; }
- (MCFindButton *)selectionButton { return _selection; }
- (MCFindButton *)closeButton { return _close; }
- (MCFindButton *)replaceButton { return _replaceOne; }
- (MCFindButton *)replaceAllButton { return _replaceAll; }
- (MCFindButton *)caseButton { return _case; }
- (MCFindButton *)wordButton { return _word; }
- (MCFindButton *)regexButton { return _regex; }
- (MCFindButton *)preserveCaseButton { return _preserve; }
- (NSInteger)widthMode { return _mode; }

// findWidget.ts, _getHeight: 4 above each row, 4 below the last.
- (CGFloat)height { return _replaceShown ? 3 * kTop + 2 * kInputH : 2 * kTop + kInputH; }

// VS Code's _tryUpdateWidgetWidth: the bar keeps 28 points from the editor's
// right (the scroll bar and a margin) and at least 15 on its left; an editor
// too narrow for the whole bar first drops the count, then the bar narrows
// to 257 and, 50 points further, to 170.
- (CGFloat)widthForEditorWidth:(CGFloat)editorWidth {
    _editorWidth = editorWidth;
    const BOOL reduced = kWidth + 28 >= editorWidth;
    const BOOL narrow = kWidth + 28 - kCountWidth >= editorWidth;
    const BOOL collapsed = kWidth + 28 - kCountWidth >= editorWidth + 50;
    const NSInteger mode = collapsed ? 3 : narrow ? 2 : reduced ? 1 : 0;
    CGFloat w = _userWidth > 0 ? _userWidth : kWidth;
    if (mode < 2) w = MIN(w, editorWidth - 28 - 15);
    if (mode == 2) w = MIN(w, kNarrowWidth);
    if (mode == 3) w = MIN(w, kCollapsedWidth);
    w = MAX(w, MIN(kCollapsedWidth, editorWidth - 8));
    if (mode != _mode) {
        _mode = mode;
        self.needsLayout = YES;
    }
    return round(w);
}

- (void)setReplaceShown:(BOOL)shown {
    _replaceShown = shown;
    _replaceInput.hidden = !shown;
    [self updateReplaceButtons];
    _chevron.icon = shown ? MCFindIconChevronDown : MCFindIconChevronRight;
    self.needsLayout = YES;
}

- (void)updateReplaceButtons {
    _replaceOne.hidden = _replaceAll.hidden = !_replaceShown || _mode == 3;
    const BOOL can = _editable && _findInput.field.stringValue.length > 0;
    _replaceOne.enabled = _replaceAll.enabled = can;
}

- (void)setEditable:(BOOL)editable {
    _editable = editable;
    _replaceInput.field.enabled = editable;
    _chevron.hidden = !editable;   // VS Code: .button.toggle.disabled is display: none
    [self updateReplaceButtons];
}

- (void)setOptionsCase:(BOOL)c word:(BOOL)w regex:(BOOL)r {
    _case.on = c;
    _word.on = w;
    _regex.on = r;
}

- (void)applySettings {
    _style = [MCFindStyle current];
    self.appearance = _style.appearance;
    self.layer.backgroundColor = _style.widgetBackground.CGColor;
    self.layer.borderColor = (_style.widgetBorder ?: _style.widgetBackground).CGColor;
    // --vscode-shadow-lg: 0 0 12px rgba(0, 0, 0, 0.14).
    self.layer.shadowColor = NSColor.blackColor.CGColor;
    self.layer.shadowOpacity = _style.dark ? 0.36 : 0.14;
    self.layer.shadowRadius = 6;
    self.layer.shadowOffset = CGSizeZero;
    for (MCFindButton *b in @[_chevron, _case, _word, _regex, _preserve, _prev, _next, _selection,
                              _close, _replaceOne, _replaceAll])
        b.style = _style;
    _findInput.style = _style;
    _replaceInput.style = _style;
    _count.textColor = _style.widgetForeground;
    self.needsDisplay = YES;
}

- (void)layout {
    [super layout];
    const CGFloat W = self.bounds.size.width, H = self.bounds.size.height;
    CGPathRef shadow = CGPathCreateWithRoundedRect(self.bounds, 8, 8, NULL);
    self.layer.shadowPath = shadow;   // the layer copies it
    CGPathRelease(shadow);
    const CGFloat y0 = kTop, y1 = 2 * kTop + kInputH;
    const CGFloat by0 = y0 + (kInputH - 22) / 2, by1 = y1 + (kInputH - 22) / 2;
    // .button.toggle: the left edge, the whole height.
    _chevron.frame = NSMakeRect(1, 1, 18, H - 2);
    // .codicon-widget-close: top 5, right 4.
    _close.frame = NSMakeRect(W - 1 - 4 - 22, 1 + 5, 22, 22);

    const BOOL showCount = _mode == 0;
    const BOOL arrows = _mode != 3;
    _count.hidden = !showCount;
    _prev.hidden = _next.hidden = !arrows;
    _findInput.togglesHidden = _mode == 3;
    _replaceInput.togglesHidden = _mode == 3;
    [self updateReplaceButtons];

    // The find row's actions, right to left: Find in Selection, Next,
    // Previous, the count (margin 3, padding 2, at least 69 wide).
    CGFloat x = W - kPartRight;
    x -= 22;
    _selection.frame = NSMakeRect(x, by0, 22, 22);
    x -= 3;
    if (arrows) {
        x -= 22;
        _next.frame = NSMakeRect(x, by0, 22, 22);
        x -= 3 + 22;
        _prev.frame = NSMakeRect(x, by0, 22, 22);
        x -= 3;
    }
    if (showCount) {
        x -= kCountWidth + 2;
        const CGFloat h = 16;
        _count.frame = NSMakeRect(x + 2, y0 + (kInputH - h) / 2 + 1, kCountWidth, h);
        x -= 3;
    }
    const CGFloat inputW = MAX(40, x - kInputX);
    _findInput.frame = NSMakeRect(kInputX, y0, inputW, kInputH);
    // The replace row: an input as wide as the find input, then Replace
    // and Replace All (margin 3 each).
    _replaceInput.frame = NSMakeRect(kInputX, y1, inputW, kInputH);
    CGFloat rx = kInputX + inputW + 3;
    _replaceOne.frame = NSMakeRect(rx, by1, 22, 22);
    _replaceAll.frame = NSMakeRect(rx + 25, by1, 22, 22);
}

- (void)showCount:(NSString *)text error:(BOOL)error tip:(NSString *)tip {
    _count.stringValue = text ?: @"";
    _count.textColor = error ? _style.errorForeground : _style.widgetForeground;
    _count.toolTip = tip;
    _findInput.error = error;
}

- (void)setNoResults:(BOOL)none {
    _findInput.error = none;
    if (none) _count.textColor = _style.errorForeground;
}

- (void)setNavigable:(BOOL)can {
    _prev.enabled = _next.enabled = can;
    [self updateReplaceButtons];
}

// VS Code's _updateToggleSelectionFindButton: enabled while it is on or
// there is a selection.
- (void)setSelectionOn:(BOOL)on enabled:(BOOL)enabled {
    _selection.on = on;
    _selection.enabled = on || enabled;
}

- (BOOL)isEditing { return _findInput.editing || _replaceInput.editing; }

- (void)focus:(NSTextField *)f {
    [self.window makeFirstResponder:f];
    [f.currentEditor selectAll:nil];
    _findInput.needsDisplay = _replaceInput.needsDisplay = YES;
}

// ------------------------------------------------------------ actions
- (void)chevronClicked:(id)sender {
    (void)sender;
    if (self.onToggleReplace) self.onToggleReplace();
}
- (void)optionClicked:(id)sender {
    (void)sender;
    if (self.onChange) self.onChange();
}
- (BOOL)caseOn { return _case.on; }
- (BOOL)wordOn { return _word.on; }
- (BOOL)regexOn { return _regex.on; }
- (BOOL)preserveOn { return _preserve.on; }
- (void)setPreserveOn:(BOOL)on { _preserve.on = on; }
- (void)selectionClicked:(id)sender {
    (void)sender;
    // The click flipped it; the controller sets it as it decides.
    if (self.onToggleSelection) self.onToggleSelection();
}
- (void)prevClicked:(id)sender { (void)sender; if (self.onNext) self.onNext(NO); }
- (void)nextClicked:(id)sender { (void)sender; if (self.onNext) self.onNext(YES); }
- (void)closeClicked:(id)sender { (void)sender; if (self.onClose) self.onClose(); }
- (void)replaceClicked:(id)sender { (void)sender; if (self.onReplace) self.onReplace(NO); }
- (void)replaceAllClicked:(id)sender { (void)sender; if (self.onReplace) self.onReplace(YES); }

- (FindHistory &)historyFor:(NSControl *)field {
    return field == _replaceInput.field ? MCFindReplaceHistory() : MCFindQueryHistory();
}

// A field's text goes into its history once it has stood half a second, as
// VS Code adds it, or at once on Return.
- (void)rememberFields {
    MCFindQueryHistory().add(_findInput.field.stringValue.UTF8String ?: "");
    if (_replaceShown) MCFindReplaceHistory().add(_replaceInput.field.stringValue.UTF8String ?: "");
}
- (void)rememberFieldsSoon {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(rememberFields)
                                               object:nil];
    [self performSelector:@selector(rememberFields) withObject:nil afterDelay:0.5];
}

- (void)controlTextDidChange:(NSNotification *)note {
    [self historyFor:note.object].reset();   // typing ends a walk through it
    [self rememberFieldsSoon];
    if (note.object == _findInput.field && self.onChange) self.onChange();
}

// Up and Down: an earlier or later entry of the field's history.
- (BOOL)stepHistory:(NSControl *)field back:(BOOL)back {
    FindHistory &h = [self historyFor:field];
    std::string out;
    const std::string now = [(NSTextField *)field stringValue].UTF8String ?: "";
    if (!(back ? h.previous(now, out) : h.next(out))) return NO;
    NSString *text = [NSString stringWithUTF8String:out.c_str()] ?: @"";
    NSTextField *f = (NSTextField *)field;
    f.stringValue = text;
    NSText *editor = [f currentEditor];
    editor.selectedRange = NSMakeRange(text.length, 0);
    if (field == _findInput.field && self.onChange) self.onChange();
    return YES;
}

- (void)controlTextDidBeginEditing:(NSNotification *)note {
    (void)note;
    _findInput.needsDisplay = _replaceInput.needsDisplay = YES;
}
- (void)controlTextDidEndEditing:(NSNotification *)note {
    (void)note;
    _findInput.needsDisplay = _replaceInput.needsDisplay = YES;
}

- (BOOL)control:(NSControl *)control textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
    (void)tv;
    if (sel == @selector(insertNewline:) || sel == @selector(insertNewlineIgnoringFieldEditor:) ||
        sel == @selector(insertLineBreak:)) {
        NSEventModifierFlags f = NSApp.currentEvent.modifierFlags;
        [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(rememberFields)
                                                   object:nil];
        [self rememberFields];
        if (control == _replaceInput.field) {
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
    if (sel == @selector(moveUp:) || sel == @selector(moveDown:)) {
        [self stepHistory:control back:sel == @selector(moveUp:)];
        return YES;   // a one-line field has nowhere else to go
    }
    if (sel == @selector(insertTab:) || sel == @selector(insertBacktab:)) {
        if (!_replaceShown) return YES;   // nowhere else to go in the bar
        [self focus:control == _findInput.field ? _replaceInput.field : _findInput.field];
        return YES;
    }
    return NO;
}

- (void)toggle:(MCFindButton *)t {
    t.on = !t.on;
    [self optionClicked:t];
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
    if (mods == cmd && [ch isEqualToString:@"f"]) { [self focus:_findInput.field]; return YES; }
    if (mods == (cmd | opt) && key == 3 /* kVK_ANSI_F */) {
        if (!_editable) return YES;
        if (!_replaceShown && self.onToggleReplace) self.onToggleReplace();
        [self focus:_replaceInput.field];
        return YES;
    }
    if ([ch isEqualToString:@"g"] && (mods == cmd || mods == (cmd | shift))) {
        if (self.onNext) self.onNext(!(mods & shift));
        return YES;
    }
    if (mods == (cmd | opt)) {
        MCFindButton *t = key == 8 /* C */ ? _case : key == 13 /* W */ ? _word
                        : key == 15 /* R */ ? _regex : key == 35 /* P */ ? _preserve : nil;
        if (t) {
            [self toggle:t];
            return YES;
        }
        if (key == 37 /* kVK_ANSI_L */) {
            [self selectionClicked:_selection];
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
    if (mods == cmd && (key == 36 || key == 76) && _replaceInput.editing) {
        if (self.onReplace) self.onReplace(YES);
        return YES;
    }
    return [super performKeyEquivalent:e];
}

// ------------------------------------------------------------ the sash
// VS Code's sash on the bar's left edge: dragging it makes the bar wider
// (never narrower than 419, never wider than the editor allows); a double
// click gives it the editor's width, or its own back.
static const CGFloat kSashWidth = 4;

- (void)resetCursorRects {
    [super resetCursorRects];
    [self addCursorRect:NSMakeRect(0, 0, kSashWidth, self.bounds.size.height)
                 cursor:[NSCursor resizeLeftRightCursor]];
}

- (CGFloat)maxWidth { return MAX(kWidth, _editorWidth - 28 - 15); }

- (void)mouseDown:(NSEvent *)e {
    const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    if (p.x >= kSashWidth || _mode >= 2) return;   // clicks on the box stay here
    if (e.clickCount == 2) {
        _userWidth = self.bounds.size.width < [self maxWidth] ? [self maxWidth] : 0;
        if (self.onResized) self.onResized();
        return;
    }
    const CGFloat startX = e.locationInWindow.x, startW = self.bounds.size.width;
    while (true) {
        NSEvent *ev = [self.window nextEventMatchingMask:NSEventMaskLeftMouseDragged |
                                                         NSEventMaskLeftMouseUp];
        if (!ev || ev.type == NSEventTypeLeftMouseUp) break;
        const CGFloat w = startW + startX - ev.locationInWindow.x;
        _userWidth = MIN([self maxWidth], MAX(kWidth, w));
        if (self.onResized) self.onResized();
    }
}

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
    NSRange _scope;              // Find in Selection's, NSNotFound when off
    void (^_whenCurrent)(void);  // waiting for results for the text as it is
    id _observer, _selectionObserver;
    std::shared_ptr<MCFindText> _text;   // the characters at _textAt (MCFindText)
    NSUInteger _textAt;
    NSUInteger _textCopies;              // copies made (tests)

    // What _matches are for, perhaps only in part (a staged search's so
    // far); marks are drawn only for the query and text as they are.
    NSUInteger _shownGen, _shownVersion;
    // A staged search (kStagedUnits): Find Next on its own queue while the
    // count goes on, the matches counted, every match before _completeTo in
    // _matches, and the match it selected with the number before it.
    BOOL _staged, _countDone;
    dispatch_queue_t _navQueue;
    std::shared_ptr<std::atomic<bool>> _navCancel;
    size_t _counted, _completeTo;
    NSRange _first;
    long _firstIndex;
    void (^_afterFirst)(void);   // Find Next pressed before the first match came
    // A word seeded from the caret into a large file's bar, not searched
    // until the user types, presses Return or changes an option.
    BOOL _seedPending;
    NSRange _seedWord;
    double _firstMatchCpuMs;
}

- (instancetype)initWithTextView:(CodeTextView *)textView {
    if ((self = [super init])) {
        _tv = textView;
        _scope = NSMakeRange(NSNotFound, 0);
        _queue = dispatch_queue_create("mutible.editor-find", DISPATCH_QUEUE_SERIAL);
        _navQueue = dispatch_queue_create("mutible.editor-find-next", DISPATCH_QUEUE_SERIAL);
        _first = NSMakeRange(NSNotFound, 0);
        _firstIndex = -1;
        _bar = [[MCEditorFindBar alloc] initWithFrame:NSMakeRect(0, 0, kWidth, 33)];
        _bar.hidden = YES;
        __weak MCEditorFind *weakSelf = self;
        _bar.onChange = ^{ [weakSelf queryChanged]; };
        _bar.onNext = ^(BOOL forward) { [weakSelf findNext:forward]; };
        _bar.onReplace = ^(BOOL all) {
            if (all) [weakSelf replaceAll];
            else [weakSelf replaceOne];
        };
        _bar.onClose = ^{ [weakSelf close]; };
        _bar.onToggleReplace = ^{
            MCEditorFind *s = weakSelf;
            if (s && [s editable]) [s setReplaceShown:!s.bar.replaceShown];
        };
        _bar.onToggleSelection = ^{ [weakSelf toggleFindInSelection]; };
        _bar.onResized = ^{ [weakSelf relayout]; };
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
            if (ts.editedMask & NSTextStorageEditedCharacters)
                [weakSelf textEditedAt:ts.editedRange change:ts.changeInLength];
        }];
        _selectionObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSTextViewDidChangeSelectionNotification
                        object:textView
                         queue:nil
                    usingBlock:^(NSNotification *note) {
            (void)note;
            [weakSelf updateSelectionButton];
        }];
    }
    return self;
}

- (void)dealloc {
    if (_observer) [[NSNotificationCenter defaultCenter] removeObserver:_observer];
    if (_selectionObserver) [[NSNotificationCenter defaultCenter] removeObserver:_selectionObserver];
    if (_cancel) _cancel->store(true);
    if (_navCancel) _navCancel->store(true);
}

- (MCEditorFindBar *)bar { return _bar; }
- (BOOL)shown { return _shown; }
- (NSUInteger)textCopies { return _textCopies; }
- (double)firstMatchCpuMs { return _firstMatchCpuMs; }
- (BOOL)counting {
    return _staged && !_countDone && _shownGen == _queryGen && _shownVersion == _textVersion &&
           _searching;
}
- (BOOL)seedPending { return _seedPending; }

// The text as it is now, for the search queue (MCFindText).
- (std::shared_ptr<MCFindText>)currentText {
    if (!_text || _textAt != _textVersion) {
        _text = std::make_shared<MCFindText>();
        _text->source = [_tv.string copy] ?: @"";
        _textAt = _textVersion;
        _textCopies++;
    }
    return _text;
}

- (BOOL)matchCase { return [_bar caseOn]; }
- (BOOL)wholeWord { return [_bar wordOn]; }
- (BOOL)regex { return [_bar regexOn]; }
- (BOOL)preserveCase { return [_bar preserveOn]; }
- (void)setMatchCase:(BOOL)v { [_bar setOptionsCase:v word:self.wholeWord regex:self.regex]; [self queryChanged]; }
- (void)setWholeWord:(BOOL)v { [_bar setOptionsCase:self.matchCase word:v regex:self.regex]; [self queryChanged]; }
- (void)setRegex:(BOOL)v { [_bar setOptionsCase:self.matchCase word:self.wholeWord regex:v]; [self queryChanged]; }
- (void)setPreserveCase:(BOOL)v { [_bar setPreserveOn:v]; [self queryChanged]; }
- (void)toggleMatchCase { self.matchCase = !self.matchCase; }
- (void)toggleWholeWord { self.wholeWord = !self.wholeWord; }
- (void)toggleRegex { self.regex = !self.regex; }
- (void)togglePreserveCase { self.preserveCase = !self.preserveCase; }

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
    const CGFloat w = [_bar widthForEditorWidth:NSWidth(rect)];
    const CGFloat h = [_bar height];
    // At the top right, against the vertical scroll bar (VS Code's overlay
    // widget TOP_RIGHT_CORNER, 14 points of scroll bar), 4 points down.
    NSScroller *sc = _tv.enclosingScrollView.verticalScroller;
    CGFloat scroller = 14;
    if (sc && !sc.hidden && sc.scrollerStyle == NSScrollerStyleLegacy)
        scroller = MAX(scroller, NSWidth(sc.frame));
    _bar.frame = NSMakeRect(MAX(NSMinX(rect) + 4, NSMaxX(rect) - w - scroller),
                            NSMaxY(rect) - kTop - h, w, h);
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

// VS Code's getSelectionSearchString, seedSearchStringFromSelection
// "always": a selection within one line (under 524,288 characters), else
// for an empty selection the word at the caret; nothing for a selection
// over several lines. *word is set for a word.
- (NSString *)seedFromSelectionWord:(NSRange *)word {
    CodeTextView *tv = _tv;
    NSString *text = tv.string;
    NSRange sel = tv.selectedRange;
    if (word) *word = NSMakeRange(NSNotFound, 0);
    if (sel.location > text.length) return nil;
    if (sel.length == 0) {
        MultiCaret::Caret c = MultiCaret::wordAt(
            MultiCaret::Text(text.length, [text](size_t start, size_t n, char16_t *out) {
                [text getCharacters:(unichar *)out range:NSMakeRange(start, n)];
            }),
            sel.location);
        if (c.empty()) return nil;
        NSRange r = NSMakeRange(c.lo(), c.hi() - c.lo());
        if (word) *word = r;
        return [text substringWithRange:r];
    }
    if (sel.length >= 524288 || NSMaxRange(sel) > text.length) return nil;
    NSString *s = [text substringWithRange:sel];
    if ([s rangeOfCharacterFromSet:[NSCharacterSet newlineCharacterSet]].location != NSNotFound)
        return nil;
    return s;
}

- (void)showFocusing:(NSTextField *)field replace:(BOOL)replace {
    NSRange word;
    NSString *seed = [self seedFromSelectionWord:&word];
    if (seed) {
        if (self.regex) seed = [NSRegularExpression escapedPatternForString:seed];
        if ([seed isEqualToString:_bar.findField.stringValue] && _shown) seed = nil;
        else _bar.findField.stringValue = seed;
    }
    BOOL wasShown = _shown;
    _shown = YES;
    [_bar setEditable:[self editable]];
    if (replace && !_bar.replaceShown && [self editable]) [_bar setReplaceShown:YES];
    [self updateSelectionButton];
    [self relayout];
    if (field && !_bar.hidden) [_bar focus:field];
    if (!wasShown || seed) {
        // A word at the caret in a large file (VS Code's 20 MB) goes in the
        // field but is not searched until the user types, presses Return
        // or changes an option: its marks on every line of a log (the date
        // "2026" in round 9's walk) were a search nobody asked for. A
        // selection is the user's choice, and is searched.
        if (seed && word.location != NSNotFound &&
            _tv.string.length >= LargeFile::kSizeThreshold) {
            [self seedWithoutSearching:word];
            return;
        }
        [self queryChanged];
        // A word at the caret: its own occurrence is the one to select.
        if (seed && word.location != NSNotFound) _anchor = word.location;
    }
}

- (void)close {
    if (!_shown) return;
    BOOL hadFocus = [_bar isEditing];
    _shown = NO;
    _bar.hidden = YES;
    if (_cancel) _cancel->store(true);
    if (_navCancel) _navCancel->store(true);
    _whenCurrent = nil;
    _afterFirst = nil;
    _text = nullptr;   // the copy goes with the bar
    _selectOnArrival = NO;
    _seedPending = NO;
    if (_scope.location != NSNotFound) {   // VS Code drops the scope with the widget
        _scope = NSMakeRange(NSNotFound, 0);
        [_bar setSelectionOn:NO enabled:NO];
        _queryGen++;
    }
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

// ------------------------------------------------------------- Find in Selection
- (NSRange)searchScope { return _scope; }
- (BOOL)findInSelection { return _scope.location != NSNotFound; }
- (void)setFindInSelection:(BOOL)on {
    if (on == self.findInSelection) {
        [self updateSelectionButton];
        return;
    }
    if (on) {
        NSRange sel = _tv.selectedRange;
        NSString *text = _tv.string;
        // VS Code: a selection ending at the start of a line ends at the
        // end of the line before.
        if (sel.length > 1 && NSMaxRange(sel) <= text.length &&
            [text characterAtIndex:NSMaxRange(sel) - 1] == '\n')
            sel.length -= 1;
        if (sel.length == 0) {
            NSBeep();
            [self updateSelectionButton];
            return;
        }
        _scope = sel;
    } else {
        _scope = NSMakeRange(NSNotFound, 0);
    }
    [self updateSelectionButton];
    _tv.needsDisplay = YES;
    [self queryChanged];
    _anchor = on ? _scope.location : _anchor;
}
- (void)toggleFindInSelection {
    if (!_shown && !self.findInSelection) {
        // From the menu with the bar closed: open it, as VS Code's command does.
        _shown = YES;
        [_bar setEditable:[self editable]];
        [self relayout];
    }
    self.findInSelection = !self.findInSelection;
}

- (void)updateSelectionButton {
    [_bar setSelectionOn:self.findInSelection enabled:_tv.selectedRange.length > 0];
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
            [self updateSelectionButton];
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
- (void)textEditedAt:(NSRange)edited change:(NSInteger)delta {
    _textVersion++;
    if (_scope.location != NSNotFound) {
        // The scope follows the text: an edit before it moves it, one inside
        // (or touching its end) grows or shrinks it.
        const NSInteger oldEnd = (NSInteger)NSMaxRange(edited) - delta;   // in the old text
        NSInteger s = (NSInteger)_scope.location, e = (NSInteger)NSMaxRange(_scope);
        const NSInteger es = (NSInteger)edited.location;
        if (oldEnd <= s) {
            s += delta;
            e += delta;
        } else if (es >= e) {
            // after it: nothing moves
        } else {
            if (es < s) s = es;
            e = MAX(s, e + delta);
        }
        s = MAX((NSInteger)0, s);
        e = MIN((NSInteger)_tv.string.length, MAX(s, e));
        _scope = NSMakeRange((NSUInteger)s, (NSUInteger)(e - s));
    }
    if (!_shown) return;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(search)
                                               object:nil];
    [self performSelector:@selector(search) withObject:nil afterDelay:0.1];
}

- (void)queryChanged { [self queryChangedSearching:YES]; }

// The query made again from the bar; searched unless `run` is NO (a
// seeded word in a large file, seedWithoutSearching:).
- (void)queryChangedSearching:(BOOL)run {
    TextFind::Options o;
    o.caseSensitive = self.matchCase;
    o.wholeWord = self.wholeWord;
    o.regex = self.regex;
    o.preserveCase = self.preserveCase;
    _query = TextFind::Query::make(FU16(_bar.findField.stringValue), o);
    _queryGen++;
    _anchor = _tv.selectedRange.location;
    _selectOnArrival = YES;
    _seedPending = !run;
    _afterFirst = nil;
    if (_navCancel) _navCancel->store(true);
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(search)
                                               object:nil];
    [self search];
}

// The bar's query made from a seeded word without searching it
// (showFocusing:): the last search stopped and its marks gone.
- (void)seedWithoutSearching:(NSRange)word {
    _seedWord = word;
    [self queryChangedSearching:NO];   // search stops the last one and returns
    _selectOnArrival = NO;
    _searching = NO;
    _matches.clear();
    _first = NSMakeRange(NSNotFound, 0);
    _tv.needsDisplay = YES;
    [self showCount];
}

- (BOOL)resultsCurrent {
    return _resultsGen == _queryGen && _resultsVersion == _textVersion;
}

// Only the matches wholly inside the scope, when there is one.
static std::vector<TextFind::Match> InScope(std::vector<TextFind::Match> found, NSRange scope) {
    if (scope.location == NSNotFound) return found;
    found.erase(std::remove_if(found.begin(), found.end(),
                               [&](const TextFind::Match &m) {
                                   return m.start < scope.location ||
                                          m.start + m.length > NSMaxRange(scope);
                               }),
                found.end());
    return found;
}

- (void)search {
    if (_cancel) _cancel->store(true);
    _cancel = nullptr;
    if (!_shown) return;
    if (_seedPending) {
        [self showCount];
        return;
    }
    const NSUInteger gen = _queryGen, version = _textVersion;
    if (!_query.usable()) {
        [self arrived:std::vector<TextFind::Match>() truncated:NO gen:gen version:version];
        return;
    }
    if (_tv.string.length >= kStagedUnits) {
        [self searchInStages];
        return;
    }
    _staged = NO;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    _cancel = cancel;
    _searching = YES;
    std::shared_ptr<MCFindText> text = [self currentText];
    TextFind::Query query = _query;
    const NSRange scope = _scope;
    __weak MCEditorFind *weakSelf = self;
    dispatch_async(_queue, ^{
        if (cancel->load()) return;
        bool truncated = false, cancelled = false;
        std::vector<TextFind::Match> found = InScope(
            TextFind::findAll(text->length(), text->reader(), query,
                              scope.location == NSNotFound ? kMatchLimit : 0,
                              &truncated, cancel.get(), &cancelled),
            scope);
        if (cancelled) return;
        if (found.size() > kMatchLimit) {
            found.resize(kMatchLimit);
            truncated = true;
        }
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
    _staged = NO;
    _matches = found;
    _truncated = truncated;
    _resultsGen = _shownGen = gen;
    _resultsVersion = _shownVersion = version;
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

// ------------------------------------------------------------- in stages
// The characters on screen, laid out already (nothing is laid out here).
- (NSRange)visibleCharacters {
    CodeTextView *tv = _tv;
    NSLayoutManager *lm = tv.layoutManager;
    if (!lm || !tv.textContainer) return NSMakeRange(NSNotFound, 0);
    NSRect r = NSOffsetRect(tv.visibleRect, -tv.textContainerOrigin.x, -tv.textContainerOrigin.y);
    NSRange glyphs = [lm glyphRangeForBoundingRectWithoutAdditionalLayout:r
                                                          inTextContainer:tv.textContainer];
    return [lm characterRangeForGlyphRange:glyphs actualGlyphRange:NULL];
}

// A large text (kStagedUnits): the old marks go at once; on the queue the
// lines on screen are searched and sent, then the first match from the
// anchor (wrapping), selected as it comes with the matches around it, then
// every match is counted from the top, sent in parts, the first kMatchLimit
// kept for the marks and Find Next. The next key cancels it all.
- (void)searchInStages {
    const NSUInteger gen = _queryGen, version = _textVersion;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    _cancel = cancel;
    _searching = YES;
    _staged = YES;
    _countDone = NO;
    _matches.clear();
    _truncated = NO;
    _counted = _completeTo = 0;
    _first = NSMakeRange(NSNotFound, 0);
    _firstIndex = -1;
    _shownGen = gen;
    _shownVersion = version;
    _tv.needsDisplay = YES;   // the last query's marks go now, not when this one is done
    std::shared_ptr<MCFindText> text = [self currentText];
    TextFind::Query query = _query;
    const NSRange scope = _scope;
    const NSRange visible = [self visibleCharacters];
    const size_t anchor = _anchor;
    __weak MCEditorFind *weakSelf = self;
    auto send = [weakSelf, cancel, gen, version](MCStagedNews news) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (cancel->load()) return;
            [weakSelf staged:news gen:gen version:version];
        });
    };
    dispatch_async(_queue, ^{
        if (cancel->load()) return;
        const double c0 = MCThreadCpuMs();
        const TextScan::Text t = text->scanText();
        TextScan::Options o;
        o.cancel = cancel.get();
        const size_t len = t.length;
        const size_t lo = scope.location == NSNotFound ? 0 : MIN(scope.location, len);
        const size_t hi = scope.location == NSNotFound ? len : MIN(NSMaxRange(scope), len);
        const size_t toEnd = hi >= len ? SIZE_MAX : hi;   // a match at the very end counts
        // 1. The lines on screen.
        if (visible.location != NSNotFound) {
            MCStagedNews news;
            const size_t vlo = MAX(lo, visible.location), vhi = MIN(hi, NSMaxRange(visible));
            if (vlo < vhi && !MCMatchesIn(t, query, vlo, vhi, scope, o, 4096, &news.found)) return;
            if (!news.found.empty()) send(std::move(news));
        }
        // 2. The first match from the anchor, then from the top.
        TextFind::Match first;
        bool cancelled = false;
        const size_t a = MIN(MAX(anchor, lo), hi);
        bool got = MCFirstIn(t, query, a, toEnd, scope, o, &first, &cancelled);
        if (cancelled) return;
        if (!got && a > lo) got = MCFirstIn(t, query, lo, a, scope, o, &first, &cancelled);
        if (cancelled) return;
        if (!got) {
            MCStagedNews news;
            news.done = true;
            news.firstCpuMs = MCThreadCpuMs() - c0;
            send(std::move(news));
            return;
        }
        {
            MCStagedNews news;
            news.hasFirst = true;
            news.first = first;
            news.firstCpuMs = MCThreadCpuMs() - c0;
            const size_t alo = first.start > lo + kAroundUnits ? first.start - kAroundUnits : lo;
            const size_t ahi = MIN(hi, first.start + kAroundUnits);
            if (!MCMatchesIn(t, query, alo, ahi, scope, o, 4096, &news.found)) return;
            send(std::move(news));
        }
        // 3. Every match, counted from the top.
        MCStagedNews part;
        part.counting = true;
        size_t counted = 0, kept = 0;
        long firstIndex = -1;
        auto lastSent = std::chrono::steady_clock::now();
        const bool finished = TextScan::scan(t, query, lo, toEnd, [&](const TextFind::Match &m) {
            if (!MCInScope(m, scope)) return true;
            if (firstIndex < 0 && m.start == first.start) firstIndex = (long)counted;
            counted++;
            if (kept < kMatchLimit) {
                part.found.push_back(m);
                kept++;
                part.completeTo = m.start + 1;
            }
            if ((counted & 1023) == 0) {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastSent > std::chrono::milliseconds(100)) {
                    part.counted = counted;
                    part.firstIndex = firstIndex;
                    send(part);
                    part.found.clear();
                    lastSent = now;
                }
            }
            return true;
        }, o);
        if (!finished) return;
        part.counted = counted;
        part.firstIndex = firstIndex;
        part.done = true;
        part.truncated = counted > kept;
        if (!part.truncated) part.completeTo = SIZE_MAX;
        send(std::move(part));
    });
    [self showCount];
}

- (void)staged:(const MCStagedNews &)news gen:(NSUInteger)gen version:(NSUInteger)version {
    if (gen != _queryGen || version != _textVersion || !_staged) return;
    MCMergeMatches(_matches, news.found);
    if (news.counting) _completeTo = MAX(_completeTo, news.completeTo);
    if (news.hasFirst) {
        _first = NSMakeRange(news.first.start, news.first.length);
        MCMergeMatches(_matches, {news.first});
        if (_selectOnArrival) {
            _selectOnArrival = NO;
            if (NSMaxRange(_first) <= _tv.string.length) {
                [_tv setSelectedRange:_first];
                [_tv scrollRangeToVisible:_first];
            }
        }
    }
    if (news.firstIndex >= 0) _firstIndex = news.firstIndex;
    if (news.firstCpuMs >= 0) _firstMatchCpuMs = news.firstCpuMs;
    if (news.counting) _counted = MAX(_counted, news.counted);
    if (news.done) {
        _counted = news.counted;
        _countDone = YES;
        _truncated = news.truncated;
        _searching = NO;
        _cancel = nullptr;
        _selectOnArrival = NO;
        _resultsGen = gen;
        _resultsVersion = version;
    }
    [self showCount];
    _tv.needsDisplay = YES;
    if (_afterFirst && (news.hasFirst || news.done)) {
        void (^next)(void) = _afterFirst;
        _afterFirst = nil;
        next();
    }
    if (news.done) {
        void (^then)(void) = _whenCurrent;
        _whenCurrent = nil;
        if (then) then();
    }
}

// The selection's number among all the matches (0-based) in a staged
// search, as far as it is known: -1 until the matches before it are counted.
- (long)stagedIndex {
    const NSRange sel = _tv.selectedRange;
    if (_shownGen != _queryGen || _shownVersion != _textVersion) return -1;
    if (sel.location < _completeTo) {
        const long i = TextFind::indexOf(_matches, sel.location, sel.length);
        if (i >= 0) return i;
    }
    if (NSEqualRanges(sel, _first) && _firstIndex >= 0) return _firstIndex;
    return -1;
}

// Find Next in a staged search still counting, or with more matches than
// it keeps: the text itself searched from the selection on Find Next's own
// queue (the count goes on), the match selected with the marks around it.
- (void)findByScanning:(BOOL)forward from:(NSRange)sel {
    if (_navCancel) _navCancel->store(true);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    _navCancel = cancel;
    const NSUInteger gen = _queryGen, version = _textVersion;
    std::shared_ptr<MCFindText> text = [self currentText];
    TextFind::Query query = _query;
    const NSRange scope = _scope;
    __weak MCEditorFind *weakSelf = self;
    dispatch_async(_navQueue, ^{
        if (cancel->load()) return;
        const TextScan::Text t = text->scanText();
        TextScan::Options o;
        o.cancel = cancel.get();
        const size_t len = t.length;
        const size_t lo = scope.location == NSNotFound ? 0 : MIN(scope.location, len);
        const size_t hi = scope.location == NSNotFound ? len : MIN(NSMaxRange(scope), len);
        const size_t toEnd = hi >= len ? SIZE_MAX : hi;
        TextFind::Match m;
        bool cancelled = false, got = false;
        if (forward) {
            // VS Code's next: a match starting at or after the selection's
            // end (past an empty match at an empty selection), then from the top.
            size_t from = MIN(MAX((size_t)NSMaxRange(sel), lo), hi);
            got = MCFirstIn(t, query, from, toEnd, scope, o, &m, &cancelled);
            if (got && m.length == 0 && sel.length == 0 && m.start == sel.location)
                got = MCFirstIn(t, query, from + 1, toEnd, scope, o, &m, &cancelled);
            if (!got && !cancelled) got = MCFirstIn(t, query, lo, from, scope, o, &m, &cancelled);
        } else {
            const size_t to = MIN(MAX((size_t)sel.location, lo), hi);
            got = MCLastIn(t, query, lo, to, scope, o, &m, &cancelled);
            if (!got && !cancelled) got = MCLastIn(t, query, to, toEnd, scope, o, &m, &cancelled);
        }
        if (cancelled) return;
        std::vector<TextFind::Match> around;
        if (got) {
            const size_t alo = m.start > lo + kAroundUnits ? m.start - kAroundUnits : lo;
            if (!MCMatchesIn(t, query, alo, MIN(hi, m.start + kAroundUnits), scope, o, 4096,
                             &around))
                return;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            MCEditorFind *s = weakSelf;
            if (!s || cancel->load() || gen != s->_queryGen || version != s->_textVersion) return;
            if (!got) {
                NSBeep();
                return;
            }
            MCMergeMatches(s->_matches, around);
            MCMergeMatches(s->_matches, {m});
            const NSRange r = NSMakeRange(m.start, m.length);
            if (NSMaxRange(r) > s->_tv.string.length) return;
            [s->_tv setSelectedRange:r];
            [s->_tv scrollRangeToVisible:r];
            [s showCount];
            s->_tv.needsDisplay = YES;
        });
    });
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
    if (_staged) return [self stagedIndex];
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
        [_bar showCount:@"No results" error:NO tip:nil];
        [_bar setNavigable:NO];
        return;
    }
    if (!_query.error().empty()) {
        [_bar showCount:@"No results" error:YES
                    tip:[NSString stringWithUTF8String:_query.error().c_str()]];
        _bar.findInput.toolTip = _bar.countLabel.toolTip;
        [_bar setNavigable:NO];
        return;
    }
    _bar.findInput.toolTip = nil;
    if (_seedPending) {
        // Seeded in a large file and not searched yet (showFocusing:).
        [_bar showCount:@"" error:NO tip:@"Press Return to find it, or type to search"];
        [_bar setNavigable:YES];
        return;
    }
    if (_staged && !_countDone && _shownGen == _queryGen && _shownVersion == _textVersion) {
        // Counting: the match selected and its number once the matches
        // before it are counted ("1 of …", "? of …").
        if (_first.location == NSNotFound) {
            [_bar showCount:@"…" error:NO tip:nil];
            return;
        }
        [_bar setNavigable:YES];
        const long i = [self stagedIndex];
        [_bar showCount:i >= 0 ? [NSString stringWithFormat:@"%ld of …", i + 1] : @"? of …"
                  error:NO
                    tip:[NSString stringWithFormat:@"%zu matches counted so far", _counted]];
        return;
    }
    if (![self resultsCurrent]) {
        if (_resultsGen != _queryGen) [_bar showCount:@"…" error:NO tip:nil];
        return;   // the old count stands while the text is searched again
    }
    const size_t n = _staged ? _counted : _matches.size();
    [_bar setNavigable:n > 0];
    if (n == 0 && _query.tooSlow()) {   // std::regex's two seconds ran out (TextFind.h)
        [_bar showCount:@"Too slow" error:YES
                    tip:@"This pattern took too long. One with a backreference or a lookahead "
                        @"runs on a slower engine, which stops after two seconds."];
        _bar.findInput.toolTip = _bar.countLabel.toolTip;
        return;
    }
    if (n == 0) {
        // VS Code's .no-results: the count in errorForeground; and the query
        // outlined in red, as the owner asked.
        [_bar showCount:@"No results" error:NO tip:nil];
        [_bar setNoResults:YES];
        return;
    }
    // A staged search counts every match and keeps the first 20,000.
    NSString *total = _truncated && !_staged ? [NSString stringWithFormat:@"%zu+", n]
                                             : [NSString stringWithFormat:@"%zu", n];
    NSInteger i = [self currentIndex];
    NSString *tip = nil;
    if (_truncated)
        tip = _staged ? @"Only the first 20,000 matches are highlighted"
                      : @"Only the first 20,000 matches are counted and highlighted";
    [_bar showCount:i >= 0 ? [NSString stringWithFormat:@"%ld of %@", (long)i + 1, total]
                           : [NSString stringWithFormat:@"? of %@", total]
              error:NO
                tip:tip];
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
    if (_seedPending) {
        // The seeded word searched now: on from the word itself, as if its
        // search had run when the bar opened (VS Code's Enter after Cmd+F).
        const NSRange word = _seedWord;
        [self queryChanged];
        _selectOnArrival = NO;
        if (!_query.usable()) return;
        if (_staged) {
            [self findByScanning:forward
                            from:forward ? NSMakeRange(NSMaxRange(word), 0)
                                         : NSMakeRange(word.location, 0)];
            return;
        }
    }
    if (!_query.usable()) {
        NSBeep();
        return;
    }
    if (_staged && _shownGen == _queryGen && _shownVersion == _textVersion &&
        !(_countDone && !_truncated)) {
        if (_selectOnArrival && _first.location == NSNotFound && !_countDone) {
            // Typed, then Return before the first match came: it is selected
            // as it comes, and this goes on from there.
            __weak MCEditorFind *weakSelf = self;
            _afterFirst = ^{
                MCEditorFind *s = weakSelf;
                if (!s) return;
                if (s->_first.location == NSNotFound) NSBeep();
                else [s findByScanning:forward from:s->_tv.selectedRange];
            };
            return;
        }
        [self findByScanning:forward from:_tv.selectedRange];
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
        const NSRange sel = s->_tv.selectedRange;
        const long i = TextFind::indexOf(s->_matches, sel.location, sel.length);
        if (i < 0) {
            [s findNext:YES];
            return;
        }
        const TextFind::Match m = s->_matches[(size_t)i];
        // Only what the replacement can take from: the match and its
        // groups (a lookahead's may lie past it), not a copy of the buffer.
        size_t lo = m.start, hi = m.start + m.length;
        for (const auto &grp : m.groups)
            if (grp.first != std::u16string::npos) {
                lo = std::min(lo, grp.first);
                hi = std::max(hi, grp.first + grp.second);
            }
        NSString *string = s->_tv.string;
        if (hi > string.length) return;
        TextFind::Match local = m;
        local.start -= lo;
        for (auto &grp : local.groups)
            if (grp.first != std::u16string::npos) grp.first -= lo;
        std::u16string with = TextFind::expand(
            [s replacement], FU16([string substringWithRange:NSMakeRange(lo, hi - lo)]), local,
            s->_query);
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
    std::shared_ptr<MCFindText> text = [self currentText];
    std::u16string with = [self replacement];
    TextFind::Query query = _query;
    const NSRange scope = _scope;
    __weak MCEditorFind *weakSelf = self;
    dispatch_async(_queue, ^{
        std::vector<TextFind::Edit16> edits =
            TextFind::replaceAll(text->length(), text->reader(), query, with);
        if (scope.location != NSNotFound)
            edits.erase(std::remove_if(edits.begin(), edits.end(),
                                       [&](const TextFind::Edit16 &e) {
                                           return e.start < scope.location ||
                                                  e.start + e.length > NSMaxRange(scope);
                                       }),
                        edits.end());
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

// The edits (ascending) in one undo step, through the text view's
// replaceRanges: (Lsp.h). The caret keeps its place in the text around it.
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
    const BOOL ok = [tv replaceRanges:ranges withStrings:strings actionName:name];
    if (ok) {
        NSUInteger at = sel.location;
        for (auto e = edits.rbegin(); e != edits.rend(); ++e) {
            const NSUInteger end = e->start + e->length;
            if (at >= end) at = at - e->length + e->text.size();
            else if (at > e->start) at = e->start + MIN(at - e->start, (NSUInteger)e->text.size());
        }
        if (at <= tv.string.length) [tv setSelectedRange:NSMakeRange(at, 0)];
    }
    if (!ok) NSBeep();
    return ok;
}

// ------------------------------------------------------------- drawing
// Find in Selection's scope, then every match on screen, behind the text
// (the selection, the current match, is drawn over it by the text view).
- (void)drawHighlightsInRect:(NSRect)rect {
    if (!_shown) return;
    CodeTextView *tv = _tv;
    const BOOL dark = [AppSettings shared].settings.isDark();
    if (_scope.location != NSNotFound && NSMaxRange(_scope) <= tv.string.length) {
        // findRangeHighlightBackground: #3a3d4166 dark, #b4b4b44d light.
        [(dark ? FHex(0x3A3D41, 0.4) : FHex(0xB4B4B4, 0.3)) setFill];
        [tv enumerateRectsForRange:_scope block:^(NSRect box) {
            if (NSIntersectsRect(box, rect))
                NSRectFillUsingOperation(box, NSCompositingOperationSourceOver);
        }];
    }
    NSColor *other = FHex(0xEA5C00, 0.33);
    NSColor *current = dark ? FHex(0x515C6A) : FHex(0xA8AC94);
    const NSRange sel = tv.selectedRange;
    [self forEachMarkInRect:rect do:^(NSRange m) {
        [(NSEqualRanges(m, sel) ? current : other) setFill];
        [tv enumerateRectsForRange:m block:^(NSRect box) {
            if (NSIntersectsRect(box, rect))
                NSRectFillUsingOperation(box, NSCompositingOperationSourceOver);
        }];
    }];
}

// The matches marked in `rect` (the text view's coordinates): none while
// they are for another query or text, so a query's marks never outlive it.
- (void)forEachMarkInRect:(NSRect)rect do:(void (^)(NSRange m))f {
    CodeTextView *tv = _tv;
    if (!_shown || _matches.empty() || _shownGen != _queryGen || _shownVersion != _textVersion)
        return;
    NSLayoutManager *lm = tv.layoutManager;
    if (!lm) return;
    NSRect r = NSOffsetRect(rect, -tv.textContainerOrigin.x, -tv.textContainerOrigin.y);
    NSRange glyphs = [lm glyphRangeForBoundingRectWithoutAdditionalLayout:r
                                                          inTextContainer:tv.textContainer];
    NSRange chars = [lm characterRangeForGlyphRange:glyphs actualGlyphRange:NULL];
    auto it = std::lower_bound(_matches.begin(), _matches.end(), chars.location,
                               [](const TextFind::Match &m, size_t at) {
                                   return m.start + m.length < at;
                               });
    for (; it != _matches.end() && it->start <= NSMaxRange(chars); ++it)
        if (it->length) f(NSMakeRange(it->start, it->length));
}

- (NSArray<NSValue *> *)markedRangesOnScreen {
    NSMutableArray<NSValue *> *a = [NSMutableArray array];
    [self forEachMarkInRect:_tv.visibleRect do:^(NSRange m) {
        [a addObject:[NSValue valueWithRange:m]];
    }];
    return a;
}

@end
