// MarkdownEditPanel.h — the popover that edits one block of Markdown from
// the rendered preview, as the LaTeX preview's popover does for LaTeX.
#import <Cocoa/Cocoa.h>

@interface MCMarkdownEditPanel : NSObject
// Shows `text` (the block's Markdown) for editing, pointing at `rect` in
// `view`. Return or Save calls `commit` with the new text; Shift+Return types
// a newline; Escape, Cancel or a click elsewhere drop the edit. With
// `addItem` set, an "Add item" button asks for a new list entry's text and
// hands it over instead.
- (void)showText:(NSString *)text
           title:(NSString *)title
          inView:(NSView *)view
            rect:(NSRect)rect
          commit:(void (^)(NSString *text))commit
         addItem:(void (^)(NSString *text))addItem;
// The same, opening with `selection` of the text selected (the word that
// was double-clicked; a location of NSNotFound puts the caret at the end,
// nothing selected), the box `width` points wide (0 for the default; a
// paragraph's width on the page, so its lines wrap as they did there) and
// as tall as the text needs, set in `font` (nil for the code font).
- (void)showText:(NSString *)text
           title:(NSString *)title
          inView:(NSView *)view
            rect:(NSRect)rect
       selection:(NSRange)selection
           width:(CGFloat)width
            font:(NSFont *)font
          commit:(void (^)(NSString *text))commit
         addItem:(void (^)(NSString *text))addItem;
- (void)close;
@property(nonatomic, readonly) BOOL shown;
@end
