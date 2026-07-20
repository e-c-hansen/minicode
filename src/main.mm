// main.mm — application bootstrap and menu bar. Objective-C++.
#import <Cocoa/Cocoa.h>
#import "EditorController.h"

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSMutableArray<EditorController *> *controllers;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    self.controllers = [NSMutableArray array];

    // Keep each window independent. Otherwise macOS auto-merges them into
    // native tabs, and Cmd+W closes the whole tab group instead of one window.
    [NSWindow setAllowsAutomaticWindowTabbing:NO];

    // Drop a controller when its window closes so it can be released.
    [[NSNotificationCenter defaultCenter]
        addObserver:self
           selector:@selector(windowClosing:)
               name:NSWindowWillCloseNotification
             object:nil];

    // Custom hotkeys that aren't standard responder actions (Ctrl+`, Cmd+B)
    // must work no matter which pane holds focus — the terminal input, the
    // browser URL bar, the editor, or the tree. A local key monitor sees every
    // keystroke before it is dispatched, so focus never blocks these.
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                          handler:^NSEvent *(NSEvent *e) {
        EditorController *c = [self controllerForWindow:e.window];
        if (!c) return e;
        NSEventModifierFlags m = e.modifierFlags;
        BOOL cmd = (m & NSEventModifierFlagCommand) != 0;
        BOOL shift = (m & NSEventModifierFlagShift) != 0;
        BOOL ctrl = (m & NSEventModifierFlagControl) != 0;
        BOOL opt = (m & NSEventModifierFlagOption) != 0;
        NSString *ch = e.charactersIgnoringModifiers.lowercaseString;
        if (ctrl && !cmd && !opt && [ch isEqualToString:@"`"]) {
            [c toggleTerminal:nil];
            return nil;   // consume
        }
        if (cmd && !shift && !ctrl && !opt && [ch isEqualToString:@"b"]) {
            [c toggleSidebar:nil];
            return nil;
        }
        if (cmd && ctrl && !shift && !opt && [ch isEqualToString:@"n"]) {
            [c newFile:nil];           // Ctrl+Cmd+N
            return nil;
        }
        if (cmd && shift && !ctrl && !opt && [ch isEqualToString:@"n"]) {
            [c newFolder:nil];         // Shift+Cmd+N
            return nil;
        }
        return e;
    }];

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

// The controller owning a specific window, or nil.
- (EditorController *)controllerForWindow:(NSWindow *)w {
    for (EditorController *c in self.controllers)
        if (c.window == w) return c;
    return nil;
}

// The controller whose window is frontmost (menu actions target it).
- (EditorController *)current {
    NSWindow *w = NSApp.keyWindow ?: NSApp.mainWindow;
    return [self controllerForWindow:w] ?: self.controllers.lastObject;
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
- (void)toggleSidebar:(id)sender   { [[self current] toggleSidebar:sender]; }
- (void)toggleHiddenFiles:(id)sender { [[self current] toggleHiddenFiles:sender]; }
- (void)focusTree:(id)sender       { [[self current] focusTree:sender]; }
- (void)focusEditor:(id)sender     { [[self current] focusEditor:sender]; }
- (void)switchToPreviousFile:(id)sender {
    [[self current] switchToPreviousFile:sender];
}
- (void)newFile:(id)sender         { [[self current] newFile:sender]; }
- (void)newFolder:(id)sender       { [[self current] newFolder:sender]; }
- (void)renameSelected:(id)sender  { [[self current] renameSelected:sender]; }
- (void)deleteSelected:(id)sender  { [[self current] deleteSelected:sender]; }
- (void)refreshTree:(id)sender     { [[self current] refreshTree:sender]; }

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

    // New File / New Folder shortcuts are handled by the global key monitor
    // (see applicationDidFinishLaunching), since Cmd+N is already New Window and
    // menu matching is unreliable for keys that differ only by a modifier.
    [fileMenu addItemWithTitle:@"New File…  (⌃⌘N)"
                        action:@selector(newFile:) keyEquivalent:@""];
    [fileMenu addItemWithTitle:@"New Folder…  (⇧⌘N)"
                        action:@selector(newFolder:) keyEquivalent:@""];
    [fileMenu addItemWithTitle:@"Rename…"
                        action:@selector(renameSelected:) keyEquivalent:@""];
    NSMenuItem *trash =
        [[NSMenuItem alloc] initWithTitle:@"Move to Trash"
                                   action:@selector(deleteSelected:)
                            keyEquivalent:@"\b"];   // Cmd+Delete
    trash.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    [fileMenu addItem:trash];
    [fileMenu addItemWithTitle:@"Refresh File Tree"
                        action:@selector(refreshTree:) keyEquivalent:@"r"];
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
    [editMenu addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
    NSMenuItem *redo = [editMenu addItemWithTitle:@"Redo"
                                           action:@selector(redo:)
                                    keyEquivalent:@"z"];
    redo.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [editMenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [editMenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [editMenu addItemWithTitle:@"Select All"
                        action:@selector(selectAll:) keyEquivalent:@"a"];
    [editMenu addItem:[NSMenuItem separatorItem]];

    // Find bar (handled by NSTextView via performTextFinderAction:; the tag is
    // the NSTextFinderAction raw value).
    NSMenuItem *find = [editMenu addItemWithTitle:@"Find…"
                                           action:@selector(performTextFinderAction:)
                                    keyEquivalent:@"f"];
    find.tag = 1;   // NSTextFinderActionShowFindInterface
    NSMenuItem *findNext = [editMenu addItemWithTitle:@"Find Next"
                                               action:@selector(performTextFinderAction:)
                                        keyEquivalent:@"g"];
    findNext.tag = 2;   // NSTextFinderActionNextMatch
    NSMenuItem *findPrev = [editMenu addItemWithTitle:@"Find Previous"
                                               action:@selector(performTextFinderAction:)
                                        keyEquivalent:@"g"];
    findPrev.tag = 3;   // NSTextFinderActionPreviousMatch
    findPrev.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
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
    hints.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [viewMenu addItem:hints];

    [viewMenu addItem:[NSMenuItem separatorItem]];

    // Cmd+B is handled by the global key monitor (see applicationDidFinishLaunching)
    // so it works in every pane; the menu item is here for discoverability.
    [viewMenu addItemWithTitle:@"Toggle Sidebar (Cmd+B)"
                        action:@selector(toggleSidebar:)
                 keyEquivalent:@""];

    NSMenuItem *hidden =
        [[NSMenuItem alloc] initWithTitle:@"Show Hidden Files"
                                   action:@selector(toggleHiddenFiles:)
                            keyEquivalent:@"."];
    hidden.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [viewMenu addItem:hidden];

    // Terminal: Cmd+T in the menu; Ctrl+` also works, caught in the view (see
    // EditorController performKeyEquivalent).
    NSMenuItem *term =
        [[NSMenuItem alloc] initWithTitle:@"Toggle Terminal"
                                   action:@selector(toggleTerminal:)
                            keyEquivalent:@"t"];
    term.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    [viewMenu addItem:term];

    NSMenuItem *browser =
        [[NSMenuItem alloc] initWithTitle:@"Toggle Browser"
                                   action:@selector(toggleBrowser:)
                            keyEquivalent:@"b"];
    browser.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [viewMenu addItem:browser];

    viewItem.submenu = viewMenu;

    // Navigate menu — keyboard-driven movement.
    NSMenuItem *navItem = [[NSMenuItem alloc] init];
    [menubar addItem:navItem];
    NSMenu *navMenu = [[NSMenu alloc] initWithTitle:@"Navigate"];
    [navMenu addItemWithTitle:@"Focus File Tree"
                       action:@selector(focusTree:)
                keyEquivalent:@"0"];
    [navMenu addItemWithTitle:@"Focus Editor"
                       action:@selector(focusEditor:)
                keyEquivalent:@"1"];
    NSMenuItem *prev =
        [[NSMenuItem alloc] initWithTitle:@"Previous File"
                                   action:@selector(switchToPreviousFile:)
                            keyEquivalent:@"\t"];
    prev.keyEquivalentModifierMask = NSEventModifierFlagControl;
    [navMenu addItem:prev];
    navItem.submenu = navMenu;
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
