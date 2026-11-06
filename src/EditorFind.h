// EditorFind.h — the editor's find and replace bar, as VS Code's find
// widget: a box at the top right of the editor with the query, "3 of 17",
// Match Case, Whole Word and Regular Expression, previous, next and close,
// and (from the chevron or Option+Cmd+F) a second row with the replacement,
// Replace and Replace All. It took over from NSTextFinder, which has none of
// those options. The matching is TextFind's (TextFind.h), run off the main
// thread; every match is highlighted behind the text and the current one
// is the selection.
//
// Keys while one of the bar's fields has the keyboard: typing searches as
// you go (from the caret), Return is the next match and Shift+Return the
// previous one (in the replace field, Return replaces), Cmd+G and
// Shift+Cmd+G the same, Option+Cmd+C / W / R the three options,
// Shift+Cmd+1 Replace, Option+Cmd+Return (or Cmd+Return in the replace
// field) Replace All, Cmd+F and Option+Cmd+F the two fields, Tab between
// them, Esc closes. The window offers key equivalents to the bar before
// the menu, which is how those reach it while its field editor has the
// keyboard. From the editor the menu's items do the same (Edit > Find…,
// Replace…, Find Next, Use Selection for Find, Find Options), and Esc
// closes the bar when nothing else (a completion list) wanted it.
//
// Edits go through CodeTextView's replaceRanges:withStrings:actionName:,
// as Format Document's and several carets' do: each match its own storage
// edit, so the highlighter, the folds, the language server, the dirty mark
// and undo (one step per Replace All) follow as for typing.
#import <Cocoa/Cocoa.h>

@class CodeTextView;

// What the bar shows; for tests and the hints.
@interface MCEditorFindBar : NSView
@property(nonatomic, readonly) NSTextField *findField;
@property(nonatomic, readonly) NSTextField *replaceField;
@property(nonatomic, readonly) NSTextField *countLabel;
@property(nonatomic, readonly) BOOL replaceShown;
@end

@interface MCEditorFind : NSObject
- (instancetype)initWithTextView:(CodeTextView *)textView;

@property(nonatomic, readonly) MCEditorFindBar *bar;
@property(nonatomic, readonly) BOOL shown;
@property(nonatomic) BOOL matchCase, wholeWord, regex;
// The controller relays its views out (the bar opened, closed, grew).
@property(nonatomic, copy) void (^onLayoutNeeded)(void);
// A line for the status bar ("Replaced 17 matches").
@property(nonatomic, copy) void (^onNote)(NSString *note);

// Place the bar in the editor's rect (rightArea coordinates); `visible` is
// NO while something other than the text view has the editor's slot.
- (void)layoutInEditorRect:(NSRect)rect visible:(BOOL)visible;

// The menu's find items, by NSTextFinderAction value: show (1), next (2),
// previous (3), replace all (4), replace and find (6), use selection (7),
// show replace (12). NO when the action does not apply now.
- (BOOL)performFindAction:(NSInteger)tag;
- (BOOL)validateFindAction:(NSInteger)tag;
- (void)toggleMatchCase;
- (void)toggleWholeWord;
- (void)toggleRegex;

// Esc in the editor: closes the bar if it is open.
- (BOOL)closeFromEditor;
- (void)close;
- (void)applySettings;

// For tests: the matches found for the text as it is now (start, length),
// whether they are current, and the index of the selected one (-1).
@property(nonatomic, readonly) NSArray<NSValue *> *matchRanges;
@property(nonatomic, readonly) BOOL resultsCurrent;
@property(nonatomic, readonly) NSInteger currentIndex;
@end
