// EditorMirror.h — Objective-C++. The editor group that does not have the
// keyboard, when the editor is split (View > Split Editor, Cmd+\).
//
// The window has one editor (CodeTextView over MCSourceStorage, with the
// highlighter, the language servers, folds, carets, the find bar, notes and
// the viewers), and it is always in the focused group. The other group
// shows its tab here, read-only, until it is clicked or focused (Cmd+1,
// Cmd+2), when the editor moves there and this takes the group it left.
//
// It shows one of three things:
//  - the editor's own text storage, through a layout manager of its own
//    (TextKit 1 lets several lay out one storage): the same file in both
//    groups, so typing, undo and colors show in both as they happen
//    (shareStorage:);
//  - a copy of another tab's text, colors and all, in a storage of its own
//    (showText:);
//  - a picture of what the editor showed for a tab that is not text there
//    (a preview, an image, a PDF, a diff, a table: showPicture:).
// A press anywhere in it is handed to onPress before anything else, and it
// never takes the keyboard. Its line numbers are drawn from a LineIndex fed
// by its storage's edits, in the editor gutter's measures.
#import <Cocoa/Cocoa.h>
#import "EditorPanes.h"

@interface MCEditorMirror : NSObject
// Adds its views to `host` (hidden until laid out).
- (instancetype)initInView:(NSView *)host;
@property(nonatomic, readonly) PanelScrollView *scrollView;
@property(nonatomic, readonly) NSTextView *textView;
@property(nonatomic, readonly) NSImageView *pictureView;
// A mouse-down in the text or the picture, with the character under it
// (NSNotFound in a picture), before it does anything else.
@property(nonatomic, copy) void (^onPress)(NSEvent *event, NSUInteger character);

- (void)layoutInRect:(NSRect)rect hidden:(BOOL)hidden;

// What it shows. `wraps` is the editor's word wrap.
- (void)shareStorage:(NSTextStorage *)storage wraps:(BOOL)wraps;
- (void)showText:(NSAttributedString *)text wraps:(BOOL)wraps;
- (void)showPicture:(NSImage *)picture;
// Nothing (a group with no tab yet): an empty text.
- (void)clear;
// The storage shared with the editor, else nil.
@property(nonatomic, readonly) NSTextStorage *sharedStorage;
// Stops sharing: the text as it is now becomes its own copy.
- (void)keepCopyOfSharedText;
@property(nonatomic, readonly) BOOL showsPicture;

// The character at the top of the pane, and how far below the pane's top
// its line starts (as the editor keeps a tab's place, MCTab topOffset).
- (NSUInteger)topCharacter:(CGFloat *)offset;
- (void)scrollToCharacter:(NSUInteger)character offset:(CGFloat)offset;
// A character's box in window coordinates (NSZeroRect when out of range).
- (NSRect)windowRectOfCharacter:(NSUInteger)character;

// The editor's look: its background, the gutter's numbers (font, colors,
// and whether line numbers show), and the width of the editor gutter's
// lanes beside the digits.
- (void)applyBackground:(NSColor *)background gutterBackground:(NSColor *)gutterBackground
            numberColor:(NSColor *)numberColor font:(NSFont *)font
            lineNumbers:(BOOL)lineNumbers laneWidth:(CGFloat)laneWidth;
@end
