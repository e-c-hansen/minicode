// MarkdownMath.h — math in the Markdown preview, typeset by tectonic.
//
// Every formula a page shows is typeset in one background tectonic run, one
// page per formula (MathTex writes the document and reads the log), and
// kept as a vector PDF page with its baseline: in memory, and on disk under
// ~/Library/Caches/MiniCode/math (each run's PDF as tectonic wrote it, with
// an index of its pages), so a page opened again draws at once. The
// preview shows a formula's TeX until its picture arrives, as it does a web
// picture's alt text, and a formula TeX cannot read keeps its TeX, with
// TeX's complaint as a tooltip.
#import <Cocoa/Cocoa.h>

// A formula as TeX set it, or the reason it could not.
@interface MCMathFormula : NSObject
@property(nonatomic, readonly) BOOL failed;
@property(nonatomic, readonly, copy) NSString *error;   // why, when it failed
// Its page, in PDF points: the formula's box and a little space around it,
// `margin` on every side, for ink that strays out of the box.
@property(nonatomic, readonly) NSSize size;
@property(nonatomic, readonly) CGFloat margin;
// How far the page reaches below the formula's baseline, in PDF points.
@property(nonatomic, readonly) CGFloat descent;
// Draws the page scaled into `rect`, every mark in `color`.
- (void)drawInRect:(NSRect)rect color:(NSColor *)color flipped:(BOOL)flipped;
@end

// A formula's key: a hash of its TeX, whether it is display math, and the
// version of the preamble it is typeset with.
NSString *MCMathKey(NSString *tex, BOOL display);

// The formula for `tex` when it has been typeset (or has failed) already:
// from memory, else from the disk cache. nil when it has not, or when a
// whole run failed for it a moment ago and it may be tried again. Main
// thread only.
MCMathFormula *MCMathLookup(NSString *tex, BOOL display);

// Whether MCMathLookup would answer with a picture for `key` now.
BOOL MCMathIsReady(NSString *key);

// Asks for formulas to be typeset, each an @[tex, @(display)]. They go into
// one tectonic run in the background, with any others asked for within a
// moment (and those asked for while a run is going go into the next).
// Formulas already typeset, failed or under way are not asked for twice.
// MCMathTypesetNotification follows. Main thread only.
void MCMathTypeset(NSArray<NSArray *> *formulas);

// Posted on the main thread when a run ends. userInfo: MCMathKeysKey, an
// NSSet of the keys it settled (typeset or failed), and MCMathErrorKey when
// the whole run failed (tectonic's first complaint).
extern NSNotificationName const MCMathTypesetNotification;
extern NSString *const MCMathKeysKey;
extern NSString *const MCMathErrorKey;

// Where typeset formulas are kept: $MINICODE_MATH_CACHE, or
// ~/Library/Caches/MiniCode/math. At most 50 MB, the runs used longest ago
// going first.
NSString *MCMathCacheDirectory(void);

// A typeset formula in the text. Its cell sits on the baseline, draws in
// `color` at `scale` screen points per PDF point, and scales itself down to
// the width of its line when it would not fit. It reaches at least as high
// and as low as `font` does, so a line holding nothing but a formula (a
// table cell, say) keeps its baseline where the text's would be.
@interface MCMathAttachment : NSTextAttachment
- (instancetype)initWithFormula:(MCMathFormula *)formula
                          scale:(CGFloat)scale
                          color:(NSColor *)color
                           font:(NSFont *)font;
@property(nonatomic, readonly) MCMathFormula *formula;
@end
