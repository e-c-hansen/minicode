// StatusBranchItem.mm — see StatusBranchItem.h.
#import "StatusBranchItem.h"

namespace {
NSString *NS(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }
const CGFloat kGap = 8;   // between the name and the counts
}  // namespace

@implementation MCStatusBranchItem {
    NSButton *_name, *_counts;
    Git::StatusBranch _shown;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _name = [self part:@selector(nameClicked:)];
        _counts = [self part:@selector(countsClicked:)];
        // A branch, drawn as the system draws one (SF Symbols, macOS 11).
        NSImage *glyph = [NSImage imageWithSystemSymbolName:@"arrow.triangle.branch"
                                   accessibilityDescription:@"Branch"];
        NSImageSymbolConfiguration *cfg =
            [NSImageSymbolConfiguration configurationWithPointSize:10 weight:NSFontWeightRegular];
        _name.image = [glyph imageWithSymbolConfiguration:cfg] ?: glyph;
        _name.imagePosition = NSImageLeading;
        _name.imageHugsTitle = YES;
        self.hidden = YES;
    }
    return self;
}

- (NSButton *)part:(SEL)action {
    NSButton *b = [NSButton buttonWithTitle:@"" target:self action:action];
    b.bordered = NO;
    b.font = [NSFont systemFontOfSize:11];
    b.lineBreakMode = NSLineBreakByTruncatingTail;
    b.hidden = YES;
    [self addSubview:b];
    return b;
}

- (NSButton *)nameButton { return _name; }
- (NSButton *)countsButton { return _counts; }

- (void)nameClicked:(id)sender {
    (void)sender;
    if (self.onSwitchBranch) self.onSwitchBranch();
}
- (void)countsClicked:(id)sender {
    (void)sender;
    if (self.onSync) self.onSync();
}

- (void)setTextColor:(NSColor *)color {
    _textColor = color;
    [self render];
}

- (void)render {
    NSColor *color = _textColor ?: [NSColor labelColor];
    NSDictionary *attrs = @{NSFontAttributeName: [NSFont systemFontOfSize:11],
                            NSForegroundColorAttributeName: color};
    _name.attributedTitle = [[NSAttributedString alloc] initWithString:NS(_shown.name)
                                                            attributes:attrs];
    _counts.attributedTitle = [[NSAttributedString alloc] initWithString:NS(_shown.counts)
                                                              attributes:attrs];
    _name.contentTintColor = color;
    NSString *tip = _shown.tip.empty() ? nil : NS(_shown.tip);
    _name.toolTip = tip;
    _counts.toolTip = tip;
    _name.hidden = !_shown.shown;
    _counts.hidden = !_shown.shown || _shown.counts.empty();
    self.hidden = !_shown.shown;
    [self layoutParts];
}

- (BOOL)showBranch:(const Git::StatusBranch &)branch {
    if (branch == _shown) return NO;
    _shown = branch;
    [self render];
    return YES;
}

- (CGFloat)nameWidth { return _name.hidden ? 0 : ceil(_name.fittingSize.width); }
- (CGFloat)countsWidth { return _counts.hidden ? 0 : ceil(_counts.fittingSize.width); }

- (CGFloat)fittingWidth {
    if (self.hidden) return 0;
    const CGFloat c = [self countsWidth];
    return [self nameWidth] + (c > 0 ? kGap + c : 0);
}

- (void)layoutParts {
    const CGFloat h = self.bounds.size.height;
    // The name gives way first: it truncates, the counts never do.
    const CGFloat cw = [self countsWidth];
    const CGFloat room = MAX(0, self.bounds.size.width - (cw > 0 ? kGap + cw : 0));
    const CGFloat nw = MIN([self nameWidth], room);
    _name.frame = NSMakeRect(0, 0, nw, h);
    _counts.frame = NSMakeRect(nw + kGap, 0, cw, h);
}

- (void)resizeSubviewsWithOldSize:(NSSize)oldSize {
    (void)oldSize;
    [self layoutParts];
}

@end
