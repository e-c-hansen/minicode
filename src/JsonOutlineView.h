// JsonOutlineView.h — a JSON file's outline beside the editor (JsonOutline.h
// reads it). The text stays the place to edit; the outline follows it,
// parsed again in the background a moment after typing stops, and keeps
// what was open. Selecting a node selects its text (a member from its key)
// and scrolls to it, the keyboard staying in the outline; moving the caret
// in the text selects the node it is in. While the text is not valid JSON,
// the last good outline stays, dimmed and selecting nothing (its places
// in the text are out of date), under a line saying where the error is.
#import <Cocoa/Cocoa.h>

@interface MCJsonOutlinePane : NSView
@property(nonatomic, readonly) NSOutlineView *outline;
@property(nonatomic, readonly) NSString *statusText;   // the line over the outline
@property(nonatomic, readonly) BOOL parsing;           // a parse is on its way
// The text changed since the outline was read (or stopped being JSON):
// the outline is behind, and selecting in it selects no text.
@property(nonatomic, readonly) BOOL stale;
// Comments and trailing commas are allowed (JsonOutline::allowsComments);
// set before attaching.
@property(nonatomic, assign) BOOL allowsComments;

// Follows a text view's text and caret; nil lets go.
- (void)attachToTextView:(NSTextView *)textView;
// The outline's rows as "label summary", for checks and the like.
- (NSString *)rowText:(NSInteger)row;
// Selects the row and its text, as a click does.
- (void)selectRow:(NSInteger)row;
@end
