// TreeDetails.mm — see TreeDetails.h.
#import "TreeDetails.h"
#include <memory>

NSNotificationName const MCTreeDetailsDidChangeNotification = @"MCTreeDetailsDidChange";

static NSString *const kTreeDetailsKey = @"MCTreeDetails";
// {resolved folder path: @[encoded options, date chosen]}; only folders
// whose order is not the default are kept, at most kMaxTreeSorts of them.
static NSString *const kTreeSortKey = @"MCTreeSort";
static const NSUInteger kMaxTreeSorts = 100;

NSUserDefaults *MCAppDefaults(void) {
    NSString *suite = NSProcessInfo.processInfo.environment[@"MINICODE_DEFAULTS_SUITE"];
    if (!suite.length) return [NSUserDefaults standardUserDefaults];
    static NSUserDefaults *scratch;
    static NSString *scratchName;
    if (![scratchName isEqualToString:suite]) {
        scratch = [[NSUserDefaults alloc] initWithSuiteName:suite];
        scratchName = suite;
    }
    return scratch;
}

BOOL MCTreeDetailsShown(void) { return [MCAppDefaults() boolForKey:kTreeDetailsKey]; }

void MCSetTreeDetailsShown(BOOL shown) {
    if (shown == MCTreeDetailsShown()) return;
    [MCAppDefaults() setBool:shown forKey:kTreeDetailsKey];
    [[NSNotificationCenter defaultCenter]
        postNotificationName:MCTreeDetailsDidChangeNotification object:nil];
}

static NSString *MCSortKeyFor(NSString *root) {
    return root.stringByResolvingSymlinksInPath ?: @"";
}

TreeSort::Options MCTreeSortForRoot(NSString *root) {
    TreeSort::Options o;
    NSDictionary *all = [MCAppDefaults() dictionaryForKey:kTreeSortKey];
    id entry = all[MCSortKeyFor(root)];
    NSString *code = [entry isKindOfClass:NSArray.class] ? [(NSArray *)entry firstObject] : nil;
    if ([code isKindOfClass:NSString.class]) TreeSort::decode(code.UTF8String ?: "", &o);
    return o;
}

void MCSetTreeSortForRoot(NSString *root, const TreeSort::Options &options) {
    NSUserDefaults *d = MCAppDefaults();
    NSMutableDictionary *all = [[d dictionaryForKey:kTreeSortKey] mutableCopy]
                               ?: [NSMutableDictionary dictionary];
    NSString *key = MCSortKeyFor(root);
    if (options == TreeSort::Options()) {
        [all removeObjectForKey:key];
    } else {
        all[key] = @[@(TreeSort::encode(options).c_str()), [NSDate date]];
        while (all.count > kMaxTreeSorts) {
            // The folder whose order was chosen longest ago goes.
            NSString *oldest = nil;
            NSDate *when = nil;
            for (NSString *k in all) {
                NSArray *e = all[k];
                NSDate *t = [e isKindOfClass:NSArray.class] && e.count > 1 ? e[1] : nil;
                if (![t isKindOfClass:NSDate.class]) t = [NSDate distantPast];
                if (!oldest || [t compare:when] == NSOrderedAscending) { oldest = k; when = t; }
            }
            [all removeObjectForKey:oldest];
        }
    }
    [d setObject:all forKey:kTreeSortKey];
}

// ------------------------------------------------------------ formatting
NSString *MCFormatByteCount(long long bytes) {
    static NSByteCountFormatter *f;
    if (!f) {
        f = [NSByteCountFormatter new];
        f.countStyle = NSByteCountFormatterCountStyleFile;
        f.allowsNonnumericFormatting = NO;   // "0 bytes", not "Zero KB"
    }
    return [f stringFromByteCount:bytes];
}

static int MCDaysBetween(NSDate *then, NSDate *now) {
    NSCalendar *cal = [NSCalendar currentCalendar];
    NSDate *a = [cal startOfDayForDate:then], *b = [cal startOfDayForDate:now];
    return (int)[cal components:NSCalendarUnitDay fromDate:a toDate:b options:0].day;
}

// "12 Oct" this year; another year "Oct 2024" in a row (narrow), or
// "12 Oct 2024" with `withDay`.
static NSString *MCShortDate(NSDate *date, NSDate *now, BOOL withDay) {
    static NSDateFormatter *thisYear, *otherYear, *otherYearDay;
    if (!thisYear) {
        thisYear = [NSDateFormatter new];
        [thisYear setLocalizedDateFormatFromTemplate:@"dMMM"];
        otherYear = [NSDateFormatter new];
        [otherYear setLocalizedDateFormatFromTemplate:@"MMMy"];
        otherYearDay = [NSDateFormatter new];
        [otherYearDay setLocalizedDateFormatFromTemplate:@"dMMMy"];
    }
    NSCalendar *cal = [NSCalendar currentCalendar];
    BOOL same = [cal component:NSCalendarUnitYear fromDate:date] ==
                [cal component:NSCalendarUnitYear fromDate:now];
    return [(same ? thisYear : withDay ? otherYearDay : otherYear) stringFromDate:date];
}

NSString *MCCompactAge(NSDate *date, NSDate *now) {
    if (!date) return @"";
    std::string s = Age::compact(now.timeIntervalSince1970, date.timeIntervalSince1970,
                                 MCDaysBetween(date, now));
    if (!s.empty()) return @(s.c_str());
    return MCShortDate(date, now, NO);
}

NSString *MCFullDate(NSDate *date) {
    static NSDateFormatter *f;
    if (!f) {
        f = [NSDateFormatter new];
        f.dateStyle = NSDateFormatterFullStyle;
        f.timeStyle = NSDateFormatterShortStyle;
    }
    return date ? [f stringFromDate:date] : @"";
}

NSString *MCSavedPhrase(NSDate *date, NSDate *now) {
    static NSDateFormatter *time;
    if (!time) {
        time = [NSDateFormatter new];
        time.dateStyle = NSDateFormatterNoStyle;
        time.timeStyle = NSDateFormatterShortStyle;
    }
    std::string s = Age::saved(now.timeIntervalSince1970, date.timeIntervalSince1970,
                               MCDaysBetween(date, now),
                               [time stringFromDate:date].UTF8String ?: "",
                               MCShortDate(date, now, YES).UTF8String ?: "");
    return @(s.c_str());
}

// ------------------------------------------------------------ folder totals
@implementation MCFolderSizer {
    std::shared_ptr<FolderSizes::SizeCache> _cache;
    std::shared_ptr<std::atomic<bool>> _stop;   // the count under way
    dispatch_queue_t _queue;
    NSMutableArray<NSString *> *_pending;       // most recent last
    NSString *_running;
    NSUInteger _generation;                      // bumped by reset
    BOOL _cancelled;
    // The last total seen per folder, shown while it is counted again.
    NSMutableDictionary<NSString *, NSArray<NSNumber *> *> *_lastKnown;
    NSMutableSet<NSString *> *_arrived;
}

- (instancetype)init {
    if ((self = [super init])) {
        _cache = std::make_shared<FolderSizes::SizeCache>();
        _stop = std::make_shared<std::atomic<bool>>(false);
        _queue = dispatch_queue_create("org.minicode.folder-sizes", DISPATCH_QUEUE_SERIAL);
        dispatch_set_target_queue(_queue, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        _pending = [NSMutableArray array];
        _lastKnown = [NSMutableDictionary dictionary];
        _arrived = [NSMutableSet set];
    }
    return self;
}

- (BOOL)busy { return _running != nil || _pending.count > 0; }

- (BOOL)totalFor:(NSString *)path total:(FolderSizes::Total *)total stale:(BOOL *)stale {
    FolderSizes::Stamp stamp;
    FolderSizes::Total t;
    const std::string p = path.UTF8String ?: "";
    if (FolderSizes::stampOf(p, &stamp) && _cache->lookup(p, stamp, &t)) {
        _lastKnown[path] = @[@(t.bytes), @(t.files), @(t.capped)];
        if (total) *total = t;
        if (stale) *stale = NO;
        return YES;
    }
    NSArray<NSNumber *> *last = _lastKnown[path];
    if (!last) return NO;
    if (total) {
        total->bytes = last[0].longLongValue;
        total->files = last[1].longLongValue;
        total->capped = last[2].boolValue;
    }
    if (stale) *stale = YES;
    return YES;
}

- (void)request:(NSString *)path {
    if (_cancelled || !path.length || [_running isEqualToString:path]) return;
    [_pending removeObject:path];
    [_pending addObject:path];   // last asked, first counted
    [self startNext];
}

- (void)startNext {
    if (_cancelled || _running || !_pending.count) return;
    NSString *path = _pending.lastObject;
    [_pending removeLastObject];
    _running = path;
    auto cache = _cache;
    auto stop = _stop;
    const NSUInteger generation = _generation;
    const std::string p = path.UTF8String ?: "";
    __weak MCFolderSizer *weakSelf = self;
    dispatch_async(_queue, ^{
        FolderSizes::Total t;
        FolderSizes::Limits limits;
        const bool ok = FolderSizes::measure(p, *cache, limits, stop.get(), &t);
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf finished:path generation:generation ok:ok total:t];
        });
    });
}

- (void)finished:(NSString *)path generation:(NSUInteger)generation ok:(bool)ok
           total:(FolderSizes::Total)t {
    if (generation != _generation) return;   // reset meanwhile; startNext ran then
    _running = nil;
    if (ok && !_cancelled) {
        _lastKnown[path] = @[@(t.bytes), @(t.files), @(t.capped)];
        if (!_arrived.count)
            [self performSelector:@selector(deliver) withObject:nil afterDelay:0.1];
        [_arrived addObject:path];
    }
    [self startNext];
}

- (void)deliver {
    NSSet<NSString *> *paths = [_arrived copy];
    [_arrived removeAllObjects];
    if (paths.count && !_cancelled && self.onTotals) self.onTotals(paths);
}

- (void)invalidate:(NSString *)path subtree:(BOOL)subtree {
    const std::string p = path.UTF8String ?: "";
    if (subtree) _cache->invalidateTree(p);
    else _cache->invalidate(p);
}

- (void)reset {
    _stop->store(true);
    _stop = std::make_shared<std::atomic<bool>>(false);
    _cache->clear();
    [_pending removeAllObjects];
    _running = nil;
    _generation++;
}

- (void)cancel {
    _cancelled = YES;
    [self reset];
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
    [_arrived removeAllObjects];
}

- (void)dealloc { _stop->store(true); }
@end

// ------------------------------------------------------------ the row view
static const CGFloat kIconX = 2, kNameX = 23, kRightPad = 2, kGap = 4;
static const CGFloat kSizeWidth = 58, kAgeWidth = 54, kMinNameWidth = 72;

@implementation MCTreeCell {
    BOOL _details;
}

static NSTextField *MCDetailField(void) {
    NSTextField *f = [NSTextField labelWithString:@""];
    f.font = [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular];
    f.alignment = NSTextAlignmentRight;
    f.lineBreakMode = NSLineBreakByClipping;
    f.hidden = YES;
    return f;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        NSImageView *iv = [[NSImageView alloc] initWithFrame:NSZeroRect];
        NSTextField *tf = [NSTextField labelWithString:@""];
        tf.lineBreakMode = NSLineBreakByClipping;
        [self addSubview:iv];
        [self addSubview:tf];
        self.imageView = iv;
        self.textField = tf;
        _sizeField = MCDetailField();
        _ageField = MCDetailField();
        [self addSubview:_sizeField];
        [self addSubview:_ageField];
    }
    return self;
}

- (void)setSize:(NSString *)size age:(NSString *)age color:(NSColor *)color {
    _details = size != nil || age != nil;
    _sizeField.stringValue = size ?: @"";
    _ageField.stringValue = age ?: @"";
    _sizeField.textColor = color;
    _ageField.textColor = color;
    // Middle truncation keeps a name's extension in view beside the
    // details; without them the name is clipped, as it always was.
    self.textField.lineBreakMode = _details ? NSLineBreakByTruncatingMiddle
                                            : NSLineBreakByClipping;
    [self place];
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self place];
}
- (void)layout {
    [super layout];
    [self place];
}

- (void)place {
    const NSRect b = self.bounds;
    const CGFloat H = b.size.height, right = b.size.width - kRightPad;
    self.imageView.frame = NSMakeRect(kIconX, floor((H - 16) / 2), 16, 16);
    const CGFloat room = right - kNameX;
    const BOOL showSize = _details && _sizeField.stringValue.length &&
                          room >= kMinNameWidth + kGap + kSizeWidth;
    const BOOL showAge = showSize && _ageField.stringValue.length &&
                         room >= kMinNameWidth + 2 * kGap + kSizeWidth + kAgeWidth;
    const CGFloat lineH = 16;
    const CGFloat y = floor((H - lineH) / 2);
    CGFloat edge = right;
    if (showAge) {
        _ageField.frame = NSMakeRect(edge - kAgeWidth, y, kAgeWidth, lineH);
        edge -= kAgeWidth + kGap;
    }
    if (showSize) {
        _sizeField.frame = NSMakeRect(edge - kSizeWidth, y, kSizeWidth, lineH);
        edge -= kSizeWidth + kGap;
    } else {
        edge = b.size.width - 2;
    }
    _sizeField.hidden = !showSize;
    _ageField.hidden = !showAge;
    self.textField.frame = NSMakeRect(kNameX, y, MAX(0, edge - kNameX), lineH);
}
@end

// ------------------------------------------------- context menu by keyboard
BOOL MCShowContextMenuInWindow(NSWindow *w) {
    if (!w) { NSBeep(); return NO; }
    NSView *v = nil;
    for (NSResponder *r = w.firstResponder; r; r = r.nextResponder)
        if ([r isKindOfClass:NSView.class] && ((NSView *)r).window == w) { v = (NSView *)r; break; }
    if (!v) v = w.contentView;
    if (!v) { NSBeep(); return NO; }

    // Where the menu opens, in v's coordinates: under the selected row (at
    // its name, past the indentation), under the caret, else near the top
    // left of what shows of the view.
    const NSRect vis = v.visibleRect;
    NSPoint p = NSMakePoint(NSMinX(vis) + 8, v.isFlipped ? NSMinY(vis) + 8 : NSMaxY(vis) - 8);
    if ([v isKindOfClass:NSTableView.class]) {
        NSTableView *t = (NSTableView *)v;
        const NSInteger row = t.selectedRow;
        if (row >= 0) {
            [t scrollRowToVisible:row];
            NSRect rr = [t rectOfRow:row];
            CGFloat x = NSMinX(rr) + 8;
            if ([t isKindOfClass:NSOutlineView.class] && t.numberOfColumns > 0)
                x = NSMinX([t frameOfCellAtColumn:0 row:row]) + 22;
            // Just inside the row's bottom edge, so the row is the one
            // clicked and the menu opens under its name.
            p = NSMakePoint(x, t.isFlipped ? NSMaxY(rr) - 1 : NSMinY(rr) + 1);
        }
    } else if ([v isKindOfClass:NSTextView.class]) {
        NSTextView *tv = (NSTextView *)v;
        const NSRange sel = tv.selectedRange;
        [tv scrollRangeToVisible:sel];
        NSRect screen = [tv firstRectForCharacterRange:NSMakeRange(sel.location, 0)
                                           actualRange:NULL];
        if (!NSEqualRects(screen, NSZeroRect)) {
            NSRect r = [tv convertRect:[w convertRectFromScreen:screen] fromView:nil];
            p = NSMakePoint(NSMinX(r), tv.isFlipped ? NSMaxY(r) : NSMinY(r));
        }
    }
    const NSPoint inWindow = [v convertPoint:p toView:nil];
    NSEvent *ev = [NSEvent mouseEventWithType:NSEventTypeRightMouseDown
                                     location:inWindow
                                modifierFlags:0
                                    timestamp:NSProcessInfo.processInfo.systemUptime
                                 windowNumber:w.windowNumber
                                      context:nil
                                  eventNumber:0
                                   clickCount:1
                                     pressure:1];
    // The view's own menu, as a right click there would get it (a table
    // marks the row as clicked, a text view asks its delegate), else the
    // nearest view above it that has one.
    NSView *host = v;
    // A text view takes a right click as a reason to select the word under
    // it; from the keyboard the selection stays as it was.
    NSTextView *text = [v isKindOfClass:NSTextView.class] ? (NSTextView *)v : nil;
    NSArray<NSValue *> *selection = text.selectedRanges;
    NSMenu *menu = [v menuForEvent:ev];
    if (text && selection && ![text.selectedRanges isEqualToArray:selection])
        text.selectedRanges = selection;
    while (!menu && host.superview) {
        host = host.superview;
        menu = [host menuForEvent:ev];
    }
    if (!menu) { NSBeep(); return NO; }
    // After the key event is done: menus opened from inside a key
    // equivalent's handling would track inside the menu bar's.
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSMenu popUpContextMenu:menu withEvent:ev forView:host];
    });
    return YES;
}
