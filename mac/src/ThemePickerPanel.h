// ThemePickerPanel.h — Objective-C++. The color theme picker, VS Code's
// Preferences: Color Theme (workbench.action.selectTheme, Ctrl+Cmd+T, View >
// Theme > Choose Theme…): the palette lists the themes, dark ones then
// light ones (ThemePicker::rows), with the one in use selected. Each row the
// list comes to, by the arrow keys, by typing, or by the pointer over it,
// shows in every window at once (AppSettings previewTheme:), the settings
// file untouched; Return keeps it and writes the theme line as View > Theme
// does (MCChooseTheme: comments and layout kept, and the question about the
// file's own colors); Esc, a click outside, or the keyboard going elsewhere
// puts the theme in use back and writes nothing.
#import <Cocoa/Cocoa.h>

// Opens the picker over window (it must have a content view).
void MCShowThemePicker(NSWindow *window);
