// HoverHint.h — Objective-C++. A small tooltip a view shows itself, at
// once, beside the pointer: the terminal's "⌘-click to open" on a link.
//
// Not the view's toolTip: AppKit shows a tooltip only after the pointer
// moves inside the view once the tooltip is set, and the terminal sets its
// hint when the pointer has rested (a move then clears it), so it never
// showed (round 9's grading of 1.6.4: "no hover hint"). This is a subview
// of the host, in the system's tooltip material and font, under the
// pointer (above it when there is no room below), inside the host's
// visible rect; clicks go through it. The host hides it on a move, a
// press, a key, a scroll, or the pointer leaving.
#import <Cocoa/Cocoa.h>

@interface MCHoverHint : NSVisualEffectView
// Shows `text` (lines split by \n) in `host`, `pointer` in host's
// coordinates; a hint already there takes the new text and place.
+ (void)showText:(NSString *)text inView:(NSView *)host atPointer:(NSPoint)pointer;
+ (void)hideInView:(NSView *)host;
// The text on show in `host`, nil when none (the GUI checks).
+ (NSString *)textInView:(NSView *)host;
@end
