// WrapIndentContainer.mm — see WrapIndentContainer.h.
#import "WrapIndentContainer.h"
#include <vector>

// Leading whitespace read past this many characters is wider than half of
// any pane, so the indent falls back to none without reading the rest.
static const NSUInteger kMaxLeading = 4096;
// Indent's one level more, in spaces (VS Code's default tab size).
static const int kLevelColumns = 4;

static BOOL MCIsParagraphBreak(unichar c) { return c == '\n' || c == '\r' || c == 0x2029; }

@implementation MCWrapIndentContainer {
    WrapIndent::Mode _mode;
    // The last paragraph asked about: [_start, _end) without its break, the
    // room its indent was worked out for, and the indent.
    BOOL _known;
    NSUInteger _start, _end;
    CGFloat _room, _indent;
    // A space's width in the last font seen.
    NSFont *_font;
    CGFloat _space;
}

- (instancetype)initWithSize:(NSSize)size {
    if ((self = [super initWithSize:size])) _mode = WrapIndent::Mode::None;
    return self;
}

- (WrapIndent::Mode)mode { return _mode; }

- (void)setMode:(WrapIndent::Mode)mode {
    if (mode == _mode) return;
    _mode = mode;
    _known = NO;
    [self.layoutManager textContainerChangedGeometry:self];
}

- (void)textDidChange { _known = NO; }

// A rectangle still (see the header): TextKit gives up non-contiguous
// layout for a container that says otherwise, and asks this one for each
// line's rect either way.
- (BOOL)isSimpleRectangularTextContainer { return YES; }

- (CGFloat)room { return MAX(0, self.size.width - 2 * self.lineFragmentPadding); }

- (CGFloat)spaceWidthIn:(NSFont *)font {
    if (!font) return 0;
    if (font != _font) {
        _font = font;
        NSGlyph g = [font glyphWithName:@"space"];
        _space = g ? [font advancementForGlyph:g].width : 0;
        if (_space <= 0) _space = [@" " sizeWithAttributes:@{NSFontAttributeName: font}].width;
    }
    return _space;
}

// The continuation indent of the paragraph holding `index`, for the room
// a line has now; remembered until the text or the room changes.
- (CGFloat)paragraphIndentAt:(NSUInteger)index {
    if (_mode == WrapIndent::Mode::None) return 0;
    NSTextStorage *ts = self.layoutManager.textStorage;
    NSString *s = ts.string;
    const NSUInteger n = s.length;
    if (!n || index > n) return 0;
    const CGFloat room = [self room];
    if (_known && room == _room && index >= _start && index <= _end) return _indent;
    NSUInteger start = 0, end = 0, contentsEnd = 0;
    [s getParagraphStart:&start end:&end contentsEnd:&contentsEnd
                forRange:NSMakeRange(MIN(index, n - 1), 0)];
    if (index == n && n && MCIsParagraphBreak([s characterAtIndex:n - 1])) start = contentsEnd = n;
    CGFloat indent = 0;
    if (start < n) {
        const NSUInteger count = MIN(contentsEnd - start, kMaxLeading);
        std::vector<unichar> chars(count);
        if (count) [s getCharacters:chars.data() range:NSMakeRange(start, count)];
        const char16_t *c = reinterpret_cast<const char16_t *>(chars.data());
        const size_t blank = WrapIndent::leadingLength(c, count);
        if (blank < kMaxLeading && blank < contentsEnd - start) {
            const CGFloat space = [self spaceWidthIn:[ts attribute:NSFontAttributeName atIndex:start
                                                     effectiveRange:NULL]];
            NSParagraphStyle *ps = [ts attribute:NSParagraphStyleAttributeName atIndex:start
                                  effectiveRange:NULL] ?: [NSParagraphStyle defaultParagraphStyle];
            std::vector<double> stops;
            for (NSTextTab *t in ps.tabStops) stops.push_back(t.location);
            const double level = kLevelColumns * space;
            const double leading =
                WrapIndent::leadingWidth(c, blank, space, stops, ps.defaultTabInterval, room);
            indent = WrapIndent::continuationIndent(leading, _mode, level, room);
        }
    }
    _known = YES;
    _start = start;
    _end = contentsEnd;
    _room = room;
    _indent = indent;
    return indent;
}

- (CGFloat)continuationIndentAt:(NSUInteger)index { return [self paragraphIndentAt:index]; }

- (NSRect)lineFragmentRectForProposedRect:(NSRect)proposedRect
                                  atIndex:(NSUInteger)characterIndex
                         writingDirection:(NSWritingDirection)baseWritingDirection
                            remainingRect:(NSRect *)remainingRect {
    NSRect r = [super lineFragmentRectForProposedRect:proposedRect atIndex:characterIndex
                                     writingDirection:baseWritingDirection
                                        remainingRect:remainingRect];
    // Off, a paragraph's first line, or no wrapping (an endless container).
    if (_mode == WrapIndent::Mode::None || characterIndex == 0 || self.size.width > 1e6)
        return r;
    NSString *s = self.layoutManager.textStorage.string;
    if (characterIndex > s.length || MCIsParagraphBreak([s characterAtIndex:characterIndex - 1]))
        return r;
    const CGFloat indent = [self paragraphIndentAt:characterIndex];
    if (indent <= 0 || indent >= NSWidth(r)) return r;
    r.origin.x += indent;
    r.size.width -= indent;
    return r;
}

@end
