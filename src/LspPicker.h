// LspPicker.h — Objective-C++. Two small pieces of the language server UI
// that LspSession (Lsp.mm) drives:
//
// MCPickerPopup, a list by the caret for Find All References (rows grouped
// under a heading per file) and Quick Fixes. Like the completion list it is
// a child panel that never takes the keyboard: the editor keeps it and
// forwards Up, Down, Page Up, Page Down, Return and Esc (LspSession
// handleCommand:), so a click elsewhere or typing simply closes it.
//
// MCRenameBox, the field Rename Symbol asks for the new name in: a small
// panel under the name with it selected, which has the keyboard while it
// shows; Return renames, Esc or a click elsewhere cancels.
#import <Cocoa/Cocoa.h>

@interface MCPickerPopup : NSObject
// A row chosen with Return or a click.
@property(nonatomic, copy) void (^onAccept)(NSInteger row);
@property(nonatomic, readonly) BOOL visible;
@property(nonatomic, readonly) NSArray<NSAttributedString *> *rows;
// Rows in `headers` are headings: drawn, never selected, skipped by the keys.
// `select` is the first row to select (moved past a heading). The list
// opens under `caret` (screen coordinates), above it when there is no room.
- (void)showRows:(NSArray<NSAttributedString *> *)rows
         headers:(NSIndexSet *)headers
          select:(NSInteger)select
           width:(CGFloat)width
         maxRows:(NSInteger)maxRows
    atScreenRect:(NSRect)caret
          parent:(NSWindow *)parent;
- (void)close;
// Move by `delta` selectable rows, wrapping at the ends for single steps and
// stopping there for pages.
- (void)moveSelection:(NSInteger)delta;
- (NSInteger)selectedRow;
- (NSRect)frame;
// The panel's content, for pictures in tests.
- (NSView *)contentView;
@end

@interface MCRenameBox : NSObject <NSTextFieldDelegate>
// Return with a name typed (trimmed; never empty).
@property(nonatomic, copy) void (^onCommit)(NSString *name);
// The box went, by Return, Esc or a click elsewhere.
@property(nonatomic, copy) void (^onClose)(void);
@property(nonatomic, readonly) NSTextField *field;
@property(nonatomic, readonly) BOOL visible;
- (void)showWithName:(NSString *)name relativeToRect:(NSRect)rect ofView:(NSView *)view;
- (void)close;
// The box's content, for pictures in tests.
- (NSView *)contentView;
@end
