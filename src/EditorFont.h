// EditorFont.h — Objective-C++. The editor's font and its zoom.
//
// The size is editor.font-size (13 by default) plus the zoom, which Zoom In
// (Cmd+=), Zoom Out (Cmd+-) and Reset Zoom (Option+Cmd+0, and VS Code's
// Cmd+keypad 0) change a point at a time, for every window at once. The
// zoom is kept in the user defaults, so it lasts across launches without
// writing the settings file. What follows the size: the editor's text, the
// line numbers, the ruler's column, the completion list and the hover,
// signature and documentation boxes (scaled from their sizes at 13 points).
// The Markdown preview's sizes are scaled by the zoom (MCZoomScale) as well.
//
// The terminals have a zoom of their own, in the user defaults too
// (MCTerminalZoom), which the same keys change while a terminal has the
// keyboard (Terminal > Zoom In, Zoom Out, Reset Zoom): their size is
// terminal.font-size (12) plus it, for every terminal in every window, so
// shrinking the terminal leaves the editor alone and the other way round.
// The first time it is read it starts at the editor's zoom, which the
// terminals followed before they had their own.
#import <Cocoa/Cocoa.h>

// Posted (object nil) when the zoom changes; a change of editor.font-size
// comes as MCSettingsDidChangeNotification instead.
extern NSNotificationName const MCEditorZoomDidChangeNotification;
// Posted (object nil) when the terminals' zoom changes.
extern NSNotificationName const MCTerminalZoomDidChangeNotification;

// +[NSFont monospacedSystemFontOfSize:weight:], never nil. AppKit returns
// nil now and then when another thread is making fonts at the same time
// (about once in 100,000 calls with eight threads at it), and a nil font
// in an attributes dictionary literal throws: inside a view's drawRect:
// that ended the app (the git panel's status letters, 2026-11-06). Asked
// again, then the user's fixed-pitch font, then the system font.
NSFont *MCMonoFont(CGFloat size, NSFontWeight weight);

CGFloat MCEditorFontSize(void);
// The monospaced system font at that size.
NSFont *MCEditorFont(void);
// The size over 13 points, for things drawn beside the text.
CGFloat MCEditorScale(void);
// The zoom alone as a factor ((13 + zoom) / 13, 1 when not zoomed), for
// what has sizes of its own and follows the zoom: the Markdown preview and
// the preview popovers' text.
CGFloat MCZoomScale(void);
// `points` times MCZoomScale, to the half point.
CGFloat MCZoomed(CGFloat points);
// Points added to editor.font-size (0 when not zoomed).
CGFloat MCEditorZoom(void);
// Points added to terminal.font-size (0 when not zoomed).
CGFloat MCTerminalZoom(void);
// terminal.font-size plus the terminals' zoom, within the same limits.
CGFloat MCTerminalFontSize(void);
// The terminal's monospaced font at that size, regular, bold, italic or
// both; kept per size, so asking is cheap.
NSFont *MCTerminalFont(BOOL bold, BOOL italic);
// Zoom in or out by `points` (0 resets), kept within 6 to 72 points in all.
// NO, and nothing changes, at a limit.
BOOL MCEditorZoomBy(CGFloat points);
// The same for the terminals' zoom.
BOOL MCTerminalZoomBy(CGFloat points);

// Word wrap in the editor: editor.word-wrap (on by default), unless View >
// Word Wrap (Option+Z) turned it over since, for every window, until the
// setting itself changes or minicode quits, as VS Code's toggle does.
extern NSNotificationName const MCEditorWrapDidChangeNotification;
BOOL MCEditorWordWrap(void);
void MCEditorToggleWordWrap(void);

// The menu items' actions, answered by the application itself at the end of
// the responder chain, so they work whatever has the keyboard.
@interface NSApplication (MCEditorZoom)
- (IBAction)editorZoomIn:(id)sender;
- (IBAction)editorZoomOut:(id)sender;
- (IBAction)editorZoomReset:(id)sender;
- (IBAction)terminalZoomIn:(id)sender;
- (IBAction)terminalZoomOut:(id)sender;
- (IBAction)terminalZoomReset:(id)sender;
@end
