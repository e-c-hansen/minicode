// AgentNotes.h — notes an agent puts over the code (`minicode open --note`,
// `minicode tour`): a range of lines highlighted, scrolled to the upper third
// of the editor, with a one- or two-sentence note above it, labelled with
// who wrote it ("Claude"). A tour is a sequence of such notes, stepped
// through with Option+] and Option+[ and counted in the note ("2 of 5").
//
// Notes are display only. They are a view over the text view and a
// layout-manager highlight, never text in the file or a step in its undo
// history, and they never take the keyboard. A note closes on Escape (with
// the editor's keyboard), its ×, or an edit that touches its lines; edits
// elsewhere move it along with its lines. Closing a tour's note ends the
// tour. Each tab keeps its note while another tab is shown; nothing is kept
// across restarts. One MCAgentNotes per window (EditorController's
// agentNotes).
#import <Cocoa/Cocoa.h>

// What the notes need of the window.
@protocol MCAgentNotesHost <NSObject>
// The editor's text view, when it shows the active tab's source (not a
// preview, picture or diff); else nil.
- (NSTextView *)notesTextView;
// Something that stays the same object while the same tab is on show.
- (id)notesActiveTab;
// Opens a file the way `minicode open --note` does (the agent preview tab,
// the keyboard where it was); YES when its text is on show.
- (BOOL)openFileForAgent:(NSString *)path preview:(BOOL)preview;
@end

@interface MCTourStop : NSObject
@property(nonatomic, copy) NSString *path;
@property(nonatomic) int line, column, toLine;   // 1-based; 0 when not given
@property(nonatomic, copy) NSString *note;
@end

@interface MCAgentNotes : NSObject
- (instancetype)initWithHost:(id<MCAgentNotesHost>)host;

// Highlights the text on show from line:column to toLine:toColumn (a missing
// column means the line's start, or its end for the last line; a missing
// toLine means `line` alone), scrolls it to the upper third, and shows `text`
// above it labelled `from` ("Claude" when empty). Empty text: the highlight
// alone. NO when no text is on show.
- (BOOL)showNote:(NSString *)text from:(NSString *)from
            line:(int)line column:(int)column toLine:(int)toLine toColumn:(int)toColumn;
// The caret to line:column, that line scrolled to the upper third and
// flashed, the keyboard left where it is. NO when no text is on show.
- (BOOL)revealLine:(int)line column:(int)column;

// A tour replaces any earlier one; its first stop opens at once.
- (BOOL)startTour:(NSArray<MCTourStop *> *)stops from:(NSString *)from;
- (BOOL)stepTour:(NSInteger)delta;   // +1 next, -1 previous; NO at either end
@property(nonatomic, readonly) BOOL tourActive;
@property(nonatomic, readonly) NSInteger tourIndex;   // 0-based
@property(nonatomic, readonly) NSInteger tourCount;

// Closes the note on show (and the tour), as its × does.
- (void)closeNote;

// The note on show, for `minicode state` and tests: its text, its label
// ("Claude", "Claude · 2 of 3"), the highlighted range, and the view.
@property(nonatomic, readonly) BOOL noteShown;
@property(nonatomic, readonly) NSString *noteText;
@property(nonatomic, readonly) NSString *noteLabel;
@property(nonatomic, readonly) NSRange noteRange;
@property(nonatomic, readonly) NSView *noteView;

// EditorController's hooks: around a change of the tab on show (the text is
// swapped then, which is not an edit), and the editor's commands (Esc).
- (void)activeTabWillChange;
- (void)activeTabDidChange;
- (BOOL)handleCommand:(SEL)selector;
@end
