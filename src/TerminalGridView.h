// TerminalGridView.h — draws a TerminalScreen (the cell grid full-screen
// programs use) and turns key presses into the bytes the program expects.
// TerminalView swaps it in for the log view while a program needs a screen.
#import <Cocoa/Cocoa.h>
#include "TerminalScreen.h"

@interface TerminalGridView : NSView
// The screen to draw; owned by the TerminalView, which outlives this view.
@property(nonatomic, assign) TerminalScreen *screen;
// Bytes for the pty: keys, pastes, scroll-wheel arrows.
@property(nonatomic, copy) void (^onInput)(NSData *bytes);

+ (NSFont *)cellFont;
// Size of one cell, and the margin around the grid.
+ (NSSize)cellSize;
+ (CGFloat)padding;

// The screen changed: redraw (and drop a selection the change moved under).
- (void)screenChanged;
@end
