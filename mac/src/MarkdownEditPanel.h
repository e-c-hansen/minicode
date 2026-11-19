// MarkdownEditPanel.h — the popover that edits one block of Markdown from
// the rendered preview, as the LaTeX preview's popover does for LaTeX.
#import <Cocoa/Cocoa.h>

@interface MCMarkdownEditPanel : NSObject
// Shows `text` (the block's Markdown) for editing, pointing at `rect` in
// `view`. Return or Apply calls `commit` with the new text (into the
// document, not to disk); Shift+Return types a newline; Escape or Cancel
// drop the edit. Switching to another app leaves the box open with its
// text; a click elsewhere in the window, or anything else that closes it,
// applies what was typed (one undo step), so a draft is never lost. With
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
// as tall as the text needs, set in `font` (nil for the code font). With
// `left` (an x in `view`, NAN for none) the box's text starts where the
// page's lines start, kept within the part of `view` on show, rather than
// centered on `rect`, which ran a wide box past a page's left edge for a
// word near it; its arrow then points into the paragraph, at rect's line.
- (void)showText:(NSString *)text
           title:(NSString *)title
          inView:(NSView *)view
            rect:(NSRect)rect
       selection:(NSRange)selection
           width:(CGFloat)width
            left:(CGFloat)left
            font:(NSFont *)font
          commit:(void (^)(NSString *text))commit
         addItem:(void (^)(NSString *text))addItem;
// Closes it, dropping the draft (Escape and Cancel).
- (void)close;
// Applies the draft if it was changed, then closes: the tab is leaving, or
// another block is about to be opened.
- (void)commitEditing;
// Whether the box holds text changed from what it opened with.
@property(nonatomic, readonly) BOOL edited;
@property(nonatomic, readonly) BOOL shown;
// Where the box last pointed, in its view, and how wide its content is.
@property(nonatomic, readonly) NSRect anchor;
@property(nonatomic, readonly) CGFloat boxWidth;
@end
