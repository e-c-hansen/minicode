// EditorFind.h — the editor's find and replace bar, laid out as VS Code's
// find widget (findWidget.ts and its CSS), out of FindWidget.h's pieces: a
// box 419 points wide hanging 4 points below the top of the editor, by the
// scroll bar. On its left edge the chevron that shows or hides the replace
// row; then the query with Match Case (Aa), Whole Word (ab) and Regular
// Expression (.*) inside its right end; "3 of 17" (or "No results", red,
// with a red border round the query); Previous and Next; Find in Selection;
// and the close button at the top right. The replace row below holds the
// replacement with Preserve Case (AB) inside it, then Replace and Replace
// All, as icons. Every button's tooltip names its key. As in VS Code, a
// narrow editor first drops the count, then makes the bar 257 points wide,
// then 170 with only the fields and Find in Selection; and the bar's left
// edge drags it wider (a double click there gives it the editor's width
// and back). Colors are the theme's (MCFindStyle).
//
// It took over from NSTextFinder, which has none of those options. The
// matching is TextFind's (TextFind.h), run off the main thread; every match
// is highlighted behind the text and the current one is the selection.
//
// Cmd+F seeds the query as VS Code's editor.find.seedSearchStringFromSelection
// does by default ("always"): a selection within one line becomes the query
// (escaped while Regular Expression is on), an empty selection gives the word
// at the caret, and a selection over several lines leaves the query as it
// was. It does so from the editor whether or not the bar is open; from the
// bar's own field Cmd+F only selects the query.
//
// Find in Selection (the button, Option+Cmd+L while the bar has the
// keyboard, or Edit > Find Options) keeps the matches to the selection it
// was turned on with, a range that follows edits and is tinted behind the
// text; turning it on needs a selection. It is off again when the bar
// closes. Preserve Case (its toggle, Option+Cmd+P) makes a replacement take
// the case of each match it replaces (TextFind's preserveCase).
//
// Keys while one of the bar's fields has the keyboard: typing searches as
// you go (from the caret), Return is the next match and Shift+Return the
// previous one (in the replace field, Return replaces), Cmd+G and
// Shift+Cmd+G the same, Option+Cmd+C / W / R the three options,
// Option+Cmd+L and Option+Cmd+P, Shift+Cmd+1 Replace, Option+Cmd+Return
// (or Cmd+Return in the replace field) Replace All, Cmd+F and Option+Cmd+F
// the two fields, Tab between them, Esc closes. The window offers key
// equivalents to the bar before the menu, which is how those reach it while
// its field editor has the keyboard. From the editor the menu's items do the
// same (Edit > Find…, Replace…, Find Next, Use Selection for Find, Find
// Options), and Esc closes the bar when nothing else (a completion list)
// wanted it.
//
// Edits go through CodeTextView's replaceRanges:withStrings:actionName:,
// as Format Document's and several carets' do: each match its own storage
// edit, so the highlighter, the folds, the language server, the dirty mark
// and undo (one step per Replace All) follow as for typing.
#import <Cocoa/Cocoa.h>

@class CodeTextView, MCFindButton, MCFindInput;

// What the bar shows; for tests, pictures and the hints.
@interface MCEditorFindBar : NSView
@property(nonatomic, readonly) NSTextField *findField;
@property(nonatomic, readonly) NSTextField *replaceField;
@property(nonatomic, readonly) NSTextField *countLabel;
@property(nonatomic, readonly) BOOL replaceShown;
@property(nonatomic, readonly) MCFindInput *findInput, *replaceInput;
@property(nonatomic, readonly) MCFindButton *toggleReplaceButton, *previousButton, *nextButton,
    *selectionButton, *closeButton, *replaceButton, *replaceAllButton;
@property(nonatomic, readonly) MCFindButton *caseButton, *wordButton, *regexButton,
    *preserveCaseButton;
// VS Code's widths: 0 the whole bar, 1 reduced (no count), 2 narrow (at
// most 257 points), 3 collapsed (170: no arrows, replace buttons or toggles).
@property(nonatomic, readonly) NSInteger widthMode;
@end

@interface MCEditorFind : NSObject
- (instancetype)initWithTextView:(CodeTextView *)textView;

@property(nonatomic, readonly) MCEditorFindBar *bar;
@property(nonatomic, readonly) BOOL shown;
@property(nonatomic) BOOL matchCase, wholeWord, regex, preserveCase;
// On with the selection as the scope (NO, with a beep, when there is none).
@property(nonatomic) BOOL findInSelection;
// The scope while Find in Selection is on; location NSNotFound otherwise.
@property(nonatomic, readonly) NSRange searchScope;
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
- (void)togglePreserveCase;
- (void)toggleFindInSelection;

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
