// TerminalGridView.h — draws a TerminalScreen (the cell grid) and turns key
// presses into the bytes the program expects. In grid mode (terminal.mode =
// grid, the default) it is the whole terminal: the shell's own prompt and
// line editor, the scrollback above the screen, selection, copy and paste.
// In line mode TerminalView swaps it in for the log view only while a
// program needs a screen.
#import <Cocoa/Cocoa.h>
#include "TerminalScreen.h"

@interface TerminalGridView : NSView <NSTextInputClient>
// The screen to draw; owned by the TerminalView, which outlives this view.
@property(nonatomic, assign) TerminalScreen *screen;
// Bytes for the pty: keys, pastes, scroll-wheel arrows.
@property(nonatomic, copy) void (^onInput)(NSData *bytes);
// Cmd+click links. `linkAt` resolves the link at a byte of a row's text (nil
// when there is none or its file does not exist); `openLink` opens one.
@property(nonatomic, copy) id (^linkAt)(const std::string &line, size_t byte);
@property(nonatomic, copy) void (^openLink)(id link);
// The grid is the terminal (grid mode): typing goes through the input system
// (dead keys and input methods compose), Option+Left/Right move by words at
// the shell's prompt, and the right click offers Copy, Paste and Clear
// Scrollback. Off in line mode, where the grid behaves as it always did.
@property(nonatomic) BOOL textInput;
// Option is Meta: ESC before the key (Option+B is ESC b). When off (grid
// mode with terminal.option-as-meta = false) Option types what the layout
// puts on the key.
@property(nonatomic) BOOL optionAsMeta;
// Lines the view is scrolled back from the live screen (0: following the
// output). Set to 0 by typing and pasting.
@property(nonatomic, readonly) int scrollOffset;
- (void)scrollToBottom;
// The selected text, or nil.
- (NSString *)selectedText;
// Cmd went down or up, or the pointer moved: redo the link under it.
- (void)updateLinkHover;

+ (NSFont *)cellFont;
// Size of one cell, and the margin around the grid.
+ (NSSize)cellSize;
+ (CGFloat)padding;

// The screen changed: redraw, keeping a scrolled-back view on the same text.
- (void)screenChanged;
@end
