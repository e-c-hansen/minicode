// LspPicker.h — Objective-C++. Two small pieces of the language server UI
// that LspSession (Lsp+Actions.mm) drives:
//
// MCPickerPopup, a list by the caret for Find All References (rows grouped
// under a heading per file) and Quick Fixes. Like the completion list it is
// a child panel that never takes the keyboard: the editor keeps it and
// forwards Up, Down, Page Up, Page Down, Return and Esc (LspSession
// handleCommand:), so a click elsewhere or typing simply closes it.
//
// MCRenameBox, the field Rename Symbol asks for the new name in: a small
// panel over the name itself, as VS Code's inline rename input, in the
// editor's font with the old name selected, so the line below stays in
// view; it has the keyboard while it shows; Return renames, Esc or a
// click elsewhere cancels.
//
// Both, like the popups in LspPopups.h, take the theme's widget colors
// (MCFindStyle, FindWidget.h) each time they show.
#import <Cocoa/Cocoa.h>

// A row of the popups' lists whose selection is drawn in `selectionColor`
// (the theme's list selection), the same whether or not its window has the
// keyboard (it never does: the editor keeps it), with the row's text left
// in its own colors.
@interface MCPopupRowView : NSTableRowView
@property(nonatomic, strong) NSColor *selectionColor;
@end

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
// The box went, by Return, Esc or a click elsewhere. Switching to another
// app and back keeps it, with what was typed, and gives it the keyboard
// again.
@property(nonatomic, copy) void (^onClose)(void);
@property(nonatomic, readonly) NSTextField *field;
@property(nonatomic, readonly) BOOL visible;
// Between the app resigning active with the box showing and its return.
@property(nonatomic, readonly) BOOL waitingForApp;
// Over `rect` (the name, in `view`'s coordinates), the field's text in
// `font` (the editor's) at the name's place.
- (void)showWithName:(NSString *)name relativeToRect:(NSRect)rect ofView:(NSView *)view
                font:(NSFont *)font;
- (void)close;
// The box's content, for pictures in tests.
- (NSView *)contentView;
@end
