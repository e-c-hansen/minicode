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
- (void)close;
@property(nonatomic, readonly) BOOL shown;
@end
