// main.mm — application bootstrap and menu bar. Objective-C++.
#import <Cocoa/Cocoa.h>
#import "EditorController.h"

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSMutableArray<EditorController *> *controllers;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    self.controllers = [NSMutableArray array];

    // Drop a controller when its window closes so it can be released.
    [[NSNotificationCenter defaultCenter]
        addObserver:self
           selector:@selector(windowClosing:)
               name:NSWindowWillCloseNotification
             object:nil];

    // Open the folder passed on the command line, else the cwd.
    NSArray *args = [[NSProcessInfo processInfo] arguments];
    NSString *root = [[NSFileManager defaultManager] currentDirectoryPath];
    if (args.count > 1) {
        NSString *arg = args[1];
        if (![arg hasPrefix:@"/"])
            arg = [root stringByAppendingPathComponent:arg];
        BOOL dir = NO;
        if ([[NSFileManager defaultManager] fileExistsAtPath:arg isDirectory:&dir]
            && dir)
            root = arg;
    }
    [self openWindowAtPath:root];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a {
    return YES;
}

// ------------------------------------------------------- window management
- (EditorController *)openWindowAtPath:(NSString *)path {
    EditorController *c = [[EditorController alloc] initWithRootPath:path];
    [self.controllers addObject:c];
    [c showWindow];
    return c;
}

// The controller whose window is frontmost (menu actions target it).
- (EditorController *)current {
    NSWindow *w = NSApp.keyWindow ?: NSApp.mainWindow;
    for (EditorController *c in self.controllers)
        if (c.window == w) return c;
    return self.controllers.lastObject;
}

- (void)windowClosing:(NSNotification *)note {
    NSWindow *w = note.object;
    NSMutableArray *keep = [NSMutableArray array];
    for (EditorController *c in self.controllers)
        if (c.window != w) [keep addObject:c];
    self.controllers = keep;
}

- (void)newWindow:(id)sender {
    EditorController *cur = [self current];
    NSString *root = cur ? cur.rootPath : NSHomeDirectory();
    [self openWindowAtPath:root];
    [NSApp activateIgnoringOtherApps:YES];
}

// -------------------------------- menu actions -> frontmost window's controller
- (void)openFolder:(id)sender      { [[self current] openFolder:sender]; }
- (void)saveCurrentFile:(id)sender { [[self current] saveCurrentFile:sender]; }
- (void)togglePreview:(id)sender   { [[self current] togglePreview:sender]; }
- (void)toggleHints:(id)sender     { [[self current] toggleHints:sender]; }
- (void)toggleTerminal:(id)sender  { [[self current] toggleTerminal:sender]; }
- (void)toggleBrowser:(id)sender   { [[self current] toggleBrowser:sender]; }

@end

static void BuildMenu(void) {
    NSMenu *menubar = [[NSMenu alloc] init];
    [NSApp setMainMenu:menubar];

    // App menu
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    [menubar addItem:appItem];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"About MiniCode"
                       action:@selector(orderFrontStandardAboutPanel:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit MiniCode"
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    // File menu
    NSMenuItem *fileItem = [[NSMenuItem alloc] init];
    [menubar addItem:fileItem];
    NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    [fileMenu addItemWithTitle:@"New Window"
                        action:@selector(newWindow:)
                 keyEquivalent:@"n"];
    [fileMenu addItemWithTitle:@"Open Folder…"
                        action:@selector(openFolder:)
                 keyEquivalent:@"o"];
    [fileMenu addItemWithTitle:@"Save"
                        action:@selector(saveCurrentFile:)
                 keyEquivalent:@"s"];
    [fileMenu addItem:[NSMenuItem separatorItem]];

    // Cmd+W closes the focused window.
    [fileMenu addItemWithTitle:@"Close Window"
                        action:@selector(performClose:)
                 keyEquivalent:@"w"];

    // Cmd+Shift+W quits the whole application.
    NSMenuItem *closeApp =
        [[NSMenuItem alloc] initWithTitle:@"Close Application"
                                   action:@selector(terminate:)
                            keyEquivalent:@"w"];
    closeApp.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [fileMenu addItem:closeApp];

    fileItem.submenu = fileMenu;

    // Edit menu (gives Copy/Select-All to the text view for free)
    NSMenuItem *editItem = [[NSMenuItem alloc] init];
    [menubar addItem:editItem];
    NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [editMenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [editMenu addItemWithTitle:@"Select All"
                        action:@selector(selectAll:) keyEquivalent:@"a"];
    editItem.submenu = editMenu;

    // View menu
    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    [menubar addItem:viewItem];
    NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem *toggle =
        [[NSMenuItem alloc] initWithTitle:@"Toggle Markdown Preview"
                                   action:@selector(togglePreview:)
                            keyEquivalent:@"p"];
    toggle.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [viewMenu addItem:toggle];

    NSMenuItem *hints =
        [[NSMenuItem alloc] initWithTitle:@"Toggle Shortcut Hints"
                                   action:@selector(toggleHints:)
                            keyEquivalent:@"h"];
    hints.keyEquivalentModifierMask = NSEventModifierFlagControl;
    [viewMenu addItem:hints];

    [viewMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *term =
        [[NSMenuItem alloc] initWithTitle:@"Toggle Terminal"
                                   action:@selector(toggleTerminal:)
                            keyEquivalent:@"`"];
    term.keyEquivalentModifierMask = NSEventModifierFlagControl;
    [viewMenu addItem:term];

    NSMenuItem *browser =
        [[NSMenuItem alloc] initWithTitle:@"Toggle Browser"
                                   action:@selector(toggleBrowser:)
                            keyEquivalent:@"b"];
    browser.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [viewMenu addItem:browser];

    viewItem.submenu = viewMenu;
}

int main(int argc, const char *argv[]) {
    (void)argc; (void)argv;   // args are read via NSProcessInfo
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppDelegate *delegate = [[AppDelegate alloc] init];
        app.delegate = delegate;
        BuildMenu();
        [app run];
    }
    return 0;
}
