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

// The same for a file's text away from the editor (a tab saved in the
// background): the sorted text, or nil when nothing changed. *problem, if
// given, gets the message when some block was left alone.
NSString *MCKeepSortedText(NSString *text, NSString **problem);

// Cmd+] and Cmd+[ (VS Code's Indent Line and Outdent Line): the lines the
// selection or the caret touches, one level in or out by the file's unit.
// NO (and a beep) when nothing could change.
BOOL MCIndentLines(NSTextView *tv, NSString *path, BOOL outdent);
// Cmd+Return and Insert Line Above: a new line below (or above) the caret's
// line, wherever the caret is in it, indented as Return would indent it.
BOOL MCInsertLine(NSTextView *tv, NSString *path, BOOL above, BOOL markdown);
// Toggle Bold ("**") and Toggle Italic ("*") in Markdown
// (MarkdownTasks::toggleWrap).
BOOL MCToggleMarkdownWrap(NSTextView *tv, NSString *marker, NSString *action);
