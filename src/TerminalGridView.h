// TerminalGridView.h — draws a TerminalScreen (the cell grid) and turns key
// presses into the bytes the program expects. In grid mode (terminal.mode =
// grid, the default) it is the whole terminal: the shell's own prompt and
// line editor, the scrollback above the screen, selection, copy and paste.
// In line mode TerminalView swaps it in for the log view only while a
// program needs a screen.
#import <Cocoa/Cocoa.h>
#include "TerminalScreen.h"

// Line editing keys the grid gives the shell on the main screen (Command
// Left, Right and Delete); the host decides the bytes.
typedef NS_ENUM(NSInteger, MCLineEdit) {
    MCLineEditStart,          // the start of the line (Ctrl+A)
    MCLineEditEnd,            // its end (Ctrl+E)
    MCLineEditDeleteToStart,  // delete back to its start (Ctrl+U)
};

@interface TerminalGridView : NSView <NSTextInputClient>
// The screen to draw; owned by the TerminalView, which outlives this view.
@property(nonatomic, assign) TerminalScreen *screen;
// Bytes for the pty: keys, pastes, scroll-wheel arrows.
@property(nonatomic, copy) void (^onInput)(NSData *bytes);
// The user typed or pasted something (not a focus or mouse report, nor the
// wheel's arrows on the alternate screen), just before it goes to onInput.
@property(nonatomic, copy) void (^onTyped)(void);
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
// While the program draws a synchronized frame (mode 2026) the view waits
// for it to end, kSyncLimit (0.15 s) at most.
- (void)screenChanged;
// The program is in the middle of a synchronized frame and the view is
// holding its last picture (for tests).
@property(nonatomic, readonly) BOOL waitingForFrame;
// Resize the screen (which reflows it), keeping a scrolled-back view, the
// selection and the current find match on their text. Call screenChanged
// after.
- (void)resizeScreenToCols:(int)cols rows:(int)rows;
// The screen and scrollback were cleared under the view (Cmd+K): the
// selection goes and the view returns to the bottom.
- (void)contentCleared;

// Command Left, Right and Delete on the main screen.
@property(nonatomic, copy) void (^onLineEdit)(MCLineEdit edit);

// Find in the terminal (the find bar is TerminalView's). Matches are kept by
// line number, so they stay on their text as output scrolls it, and they are
// looked for again shortly after output arrives (onFindChanged then).
// findText: starts or refines a search. The current match is the one at or
// before the previous current one, or for a new search the newest at or
// above the bottom of the view; it is scrolled into view.
- (void)findText:(NSString *)text matchCase:(BOOL)matchCase;
// The next match (newer, further down) or the previous one, wrapping.
- (void)findNext:(BOOL)forward;
- (void)endFind;
@property(nonatomic, readonly) NSInteger findCount;   // matches, at most findLimit
@property(nonatomic, readonly) NSInteger findIndex;   // the current one, -1 for none
@property(nonatomic, readonly) NSInteger findLimit;
@property(nonatomic, copy) void (^onFindChanged)(void);
@end
