// Demo.h — scripted demo scenes, recorded for the README's GIFs.
//
// Compiled into every build but inert: nothing here runs unless the
// MINICODE_DEMO environment variable is set, which only `make demos`
// (scripts/demos.sh) does. A scene drives the real app the way a person would:
// clicks in the file tree, keystrokes through the text input system, menu
// shortcuts. Meanwhile the window is captured to numbered PNGs, which
// tools/makegif.m turns into a GIF.
#import <Cocoa/Cocoa.h>

@class EditorController;

// MINICODE_DEMO=list prints the scene names, one per line. Returns YES when it
// did, and the caller should exit without starting the app.
BOOL MCDemoListScenes(void);

// MINICODE_DEMO=<scene>: play that scene in the controller's window, write the
// frames to $MINICODE_DEMO_FRAMES, and quit. Returns at once when the variable
// is unset.
void MCDemoStart(EditorController *controller);
