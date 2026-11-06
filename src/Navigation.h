// Navigation.h — Objective-C++. Go Back and Go Forward (Ctrl+- and
// Ctrl+Shift+-, VS Code's Mac keys): one MCNavigator per window, over the
// rules in NavHistory.h.
//
// It records a jump when the window moves somewhere far: a tab switch (a
// click on a tab, Quick Open, the tree, Ctrl+Tab), anything run inside
// -jump: (go to definition, a reference, a problem, a symbol, Go to Line, a
// terminal link, a Find in Folder result), and a caret that moves more than
// Nav::kFarLines lines at once without the text changing (Cmd+End, a click
// far down, the find bar's next match, F8). Going back and forth itself, and
// what happens inside a jump, records nothing more.
//
// The window's controller is the host: it says where the window is and
// shows a place again.
#import <Cocoa/Cocoa.h>

@interface MCPlace : NSObject
@property(nonatomic, copy) NSString *path;
@property(nonatomic, assign) NSRange selection;   // in the file's text
@property(nonatomic, assign) NSInteger line;      // 0-based, where it starts
@property(nonatomic, assign) BOOL hasText;        // NO: a picture, a PDF, a preview
@end

@protocol MCNavigatorHost <NSObject>
// The file on show and whether the editor holds its text (a caret to put
// back), or nil path when no file is on show.
- (NSString *)navigationPath:(BOOL *)hasText;
// Show a place: its file (opening it if need be) and, when it has text, its
// selection, scrolled into view, the keyboard in the editor. NO when the
// file could not be shown.
- (BOOL)showNavigationPlace:(MCPlace *)place;
@end

@interface MCNavigator : NSObject
- (instancetype)initWithTextView:(NSTextView *)textView host:(id<MCNavigatorHost>)host;

// Where the window is now, or nil.
- (MCPlace *)currentPlace;

// Run a jump: where the window was before it and where it is after are
// recorded (once, however deeply jumps nest). `from`, when given, stands
// for where it was (Go to Symbol has moved the caret while previewing).
- (void)jump:(dispatch_block_t)body;
- (void)jumpFrom:(MCPlace *)from body:(dispatch_block_t)body;
// Nothing is recorded while quiet (a symbol shown while the palette moves).
- (void)beQuiet:(dispatch_block_t)body;
// Around a tab switch: the window's controller calls these from its own
// activateTab:, so every way of switching counts.
- (void)tabWillSwitch;
- (void)tabDidSwitch;

- (BOOL)canGoBack;
- (BOOL)canGoForward;
// Beep when there is nowhere to go. Files deleted meanwhile are skipped
// (and dropped).
- (void)goBack;
- (void)goForward;

// A file or folder was renamed or moved; one was deleted.
- (void)pathMovedFrom:(NSString *)from to:(NSString *)to;
- (void)pathRemoved:(NSString *)path;
- (void)clear;   // another folder opened

// Tests: the history's paths and lines, "path:line" each, and which is current.
@property(nonatomic, readonly) NSArray<NSString *> *entries;
@property(nonatomic, readonly) NSInteger index;
@end
