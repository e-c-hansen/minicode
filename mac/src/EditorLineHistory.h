// EditorLineHistory.h — Objective-C++. Show Line History (A7): who last
// changed each line of the file on show, one object per window. The words
// are the core's (LineHistory.h): a commit (short hash, author, age,
// subject, from `git blame --porcelain` with the buffer on stdin, through
// the git panel's runner and its time limits), an uncommitted change one of
// Claude Code's sessions made (its title, the prompt the edit answered, and
// when; only when the session's edit is found exactly), or "Not committed".
//
// Quiet by default: the pointer resting on a line's number in the gutter
// shows its line's history in a small box beside it, and a click in the box
// opens the commit on the commit page (the review page in commit mode) or
// the session's page for the file (what it changed there). View > Show
// Line History puts the words beside every line, muted, in every window;
// a click on them does what the box's click does, and the pointer resting
// on them shows the box.
//
// The work is off the main thread, only while wanted (the words shown, or
// the box asked for): git and the transcripts on a serial queue, the text
// copied once a look. A look starts 0.6 seconds after typing stops and at
// once for another file; until it ends, lines from the first edit on show
// nothing rather than another line's words. Each file's last answer is
// kept with the text it was for, so going back to a tab whose text has not
// changed shows it at once. Anything that changes a commit (the git panel
// refreshing) or a session (the Agents model publishing) looks again while
// the words show.
#import <Cocoa/Cocoa.h>

@class CodeTextView, MCAgentModel, MCLineGutter;

@protocol MCLineHistoryHost <NSObject>
- (CodeTextView *)lineHistoryTextView;
- (MCLineGutter *)lineHistoryGutter;
// The file the editor shows as editable source text, or nil.
- (NSString *)lineHistoryPath;
// The window's Agents model (made on first use), whose sessions are matched.
- (MCAgentModel *)lineHistoryAgentModel;
// A click: the commit on the commit page; the session's page for the file.
- (void)lineHistoryShowCommit:(NSString *)hash title:(NSString *)title;
- (void)lineHistoryShowSession:(NSString *)transcript name:(NSString *)name file:(NSString *)path;
@end

// The words beside every line: one switch for every window, kept in the
// app's defaults (MCLineHistoryInline). Changing it posts this.
extern NSNotificationName const MCLineHistoryInlineDidChangeNotification;
BOOL MCLineHistoryInline(void);
void MCSetLineHistoryInline(BOOL on);

@interface MCLineHistory : NSObject
- (instancetype)initWithHost:(id<MCLineHistoryHost>)host;
- (void)stop;           // the window is closing
- (void)refreshSoon;    // look again (while wanted)

// The gutter's hover (MCLineGutter onHoverLine): the box after the pointer
// rests, NSNotFound to close it. Lines are the editor's, 0-based.
- (void)hoverLine:(NSUInteger)line;
// The box for a line now (looking first when the answer is not in yet).
- (void)showPopupForLine:(NSUInteger)line;
- (void)closePopup;
@property(nonatomic, readonly) BOOL popupVisible;
@property(nonatomic, readonly, copy) NSString *popupText;
@property(nonatomic, readonly) NSUInteger popupLine;

// A line's words beside it and in the box; nil while not known (no look
// yet, or edited since).
- (NSString *)labelForLine:(NSUInteger)line;
- (NSString *)detailForLine:(NSUInteger)line;
// What a click on a line's words does: the commit or the session's page.
// NO (with a beep) for a line not committed and from no session.
- (BOOL)openLine:(NSUInteger)line;
// Where a line's words are drawn beside it, in the text view's
// coordinates; NSZeroRect while not shown.
- (NSRect)labelRectForLine:(NSUInteger)line;

// For checks: looks finished, the file of the last, and the main thread's
// CPU time for its start and its end together (ms).
@property(nonatomic, readonly) NSUInteger looks;
@property(nonatomic, readonly, copy) NSString *lookedPath;
@property(nonatomic, readonly) double lastLookMainCpuMs;
@end
