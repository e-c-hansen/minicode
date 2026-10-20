// EditingCommands.h — Objective-C++. The editor's indentation keys and line
// sorting on an NSTextView, over the pure-C++ AutoIndent and KeepSorted
// cores. EditorController calls these from a few hooks; everything they do
// is one replacement in the text view, so each is one undo step.
#import <Cocoa/Cocoa.h>

// Return, Tab, Shift+Tab and Backspace (insertNewline:, insertTab:,
// insertBacktab:, deleteBackward:) by the rules for `path`'s language, with
// Return in a Markdown list (`markdown`) continuing the list first. YES when
// the command was handled; NO lets the text view do its own.
BOOL MCIndentCommand(NSTextView *tv, SEL command, NSString *path, BOOL markdown);

// Typed closers and Python's ':' can move their line (AutoIndent::typed).
// The first notes a typed character from the text view's
// shouldChangeTextInRange delegate call; the second, from textDidChange:,
// makes the move once the character is in.
void MCIndentWillChange(NSTextView *tv, NSRange range, NSString *replacement);
void MCIndentDidChange(NSTextView *tv, NSString *path);

// Sort Lines (F9): the selected lines, or the keep-sorted block around the
// caret. Fix keep-sorted Blocks (Shift+F9): every block in the file. Each
// returns what to tell the user, or nil, and beeps when nothing could be done.
NSString *MCSortLines(NSTextView *tv);
NSString *MCFixKeepSorted(NSTextView *tv);

// editor.keep-sorted-on-save: sort every block before a save, if the file
// has any. Returns a message when some block was left alone, else nil.
NSString *MCKeepSortedBeforeSave(NSTextView *tv);
