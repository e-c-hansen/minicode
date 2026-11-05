// EditorFont.h — Objective-C++. The editor's font and its zoom.
//
// The size is editor.font-size (13 by default) plus the zoom, which Zoom In
// (Cmd+=), Zoom Out (Cmd+-) and Reset Zoom (Option+Cmd+0, and VS Code's
// Cmd+keypad 0) change a point at a time, for every window at once. The
// zoom is kept in the user defaults, so it lasts across launches without
// writing the settings file. What follows the size: the editor's text, the
// line numbers, the ruler's column, the completion list and the hover,
// signature and documentation boxes (scaled from their sizes at 13 points).
// The Markdown preview and the terminal keep their own sizes.
#import <Cocoa/Cocoa.h>

// Posted (object nil) when the zoom changes; a change of editor.font-size
// comes as MCSettingsDidChangeNotification instead.
extern NSNotificationName const MCEditorZoomDidChangeNotification;

CGFloat MCEditorFontSize(void);
// The monospaced system font at that size.
NSFont *MCEditorFont(void);
// The size over 13 points, for things drawn beside the text.
CGFloat MCEditorScale(void);
// Points added to editor.font-size (0 when not zoomed).
CGFloat MCEditorZoom(void);
// Zoom in or out by `points` (0 resets), kept within 6 to 72 points in all.
// NO, and nothing changes, at a limit.
BOOL MCEditorZoomBy(CGFloat points);

// The menu items' actions, answered by the application itself at the end of
// the responder chain, so they work whatever has the keyboard.
@interface NSApplication (MCEditorZoom)
- (IBAction)editorZoomIn:(id)sender;
- (IBAction)editorZoomOut:(id)sender;
- (IBAction)editorZoomReset:(id)sender;
@end
