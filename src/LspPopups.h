// LspPopups.h — the windows by the caret: the completion list with its
// documentation (MCCompletionPopup), hover info (MCHoverPopup) and the
// signature hint (MCSignaturePopup). For the Lsp files only.
#import <Cocoa/Cocoa.h>

@interface MCCompletionPopup : NSObject <NSTableViewDataSource, NSTableViewDelegate>
@property(nonatomic, copy) void (^onAccept)(NSInteger row);
@property(nonatomic, copy) void (^onSelectionChanged)(void);
@property(nonatomic, readonly) BOOL visible;
@property(nonatomic, readonly) NSArray<NSString *> *labels;
@property(nonatomic, readonly) NSString *docText;   // nil when the doc panel is hidden
// `select` is the row to highlight (the caller decides: the best match, or
// the item the arrows chose).
- (void)showLabels:(NSArray<NSString *> *)labels details:(NSArray<NSString *> *)details
               select:(NSInteger)select atScreenRect:(NSRect)caret parent:(NSWindow *)parent;
// The selected item's documentation beside the list; nil hides it.
- (void)showDoc:(NSAttributedString *)doc;
- (void)close;
- (void)moveSelection:(NSInteger)delta;
- (NSInteger)selectedRow;
- (NSRect)frame;
@end

@interface MCHoverPopup : NSObject
@property(nonatomic, readonly) BOOL visible;
@property(nonatomic, copy) void (^onExit)(void);
- (void)show:(NSAttributedString *)text atScreenRect:(NSRect)line parent:(NSWindow *)parent;
- (void)close;
- (NSRect)frame;
@end

// ------------------------------------------------------- MCSignaturePopup
// The signature of the call being typed, above the caret's line, with the
// argument being typed in bold.
@interface MCSignaturePopup : NSObject
@property(nonatomic, readonly) BOOL visible;
@property(nonatomic, readonly) NSString *text;
- (void)show:(NSAttributedString *)text atScreenRect:(NSRect)caret parent:(NSWindow *)parent;
- (void)close;
@end
