// main.mm — application bootstrap and menu bar. Objective-C++.
#import <Cocoa/Cocoa.h>
#import "EditorController.h"
#import "Lsp.h"
#import "Demo.h"
#import "Bench.h"
#import "Appearance.h"
#import "TerminalPanel.h"

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSMutableArray<EditorController *> *controllers;
@property(nonatomic, strong) id keyMonitor;
@end

@implementation AppDelegate

// Set up before any window can open: when Finder launches MiniCode to open
// something, application:openURLs: arrives before didFinishLaunching.
- (void)applicationWillFinishLaunching:(NSNotification *)note {
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

    // Ctrl+` (toggle terminal) is the one shortcut that isn't a clean menu key
    // equivalent, so a local key monitor handles it regardless of focus. The
    // monitor object is retained so it stays installed. Everything else is a
    // normal menu shortcut, which works in every pane (like Cmd+C does).
    self.keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                          handler:^NSEvent *(NSEvent *e) {
        EditorController *c = [self controllerForWindow:e.window];
        if (!c) return e;
        NSEventModifierFlags m = e.modifierFlags;
        BOOL cmd = (m & NSEventModifierFlagCommand) != 0;
        BOOL ctrl = (m & NSEventModifierFlagControl) != 0;
        BOOL opt = (m & NSEventModifierFlagOption) != 0;
        NSString *ch = e.charactersIgnoringModifiers.lowercaseString;
        BOOL shift = (m & NSEventModifierFlagShift) != 0;
        // Ctrl+Shift+` (a new terminal) by the key's position, since Shift
        // turns its character into ~ on most layouts.
        if (ctrl && shift && !cmd && !opt &&
            (e.keyCode == 50 || [ch isEqualToString:@"~"])) {
            [c newTerminal:nil];
            return nil;
        }
        if (ctrl && !cmd && !opt && [ch isEqualToString:@"`"]) {
            [c toggleTerminal:nil];
            return nil;   // consume
        }
        // Shift+Option+F is Format Document only in the editor. Anywhere
        // else that takes typing (the terminal, the commit message, a find
        // field) it types its character, as it did before the menu had it.
        if (opt && shift && !cmd && !ctrl && [ch isEqualToString:@"f"]) {
            NSResponder *fr = e.window.firstResponder;
            if (![fr isKindOfClass:[CodeTextView class]] &&
                ([fr conformsToProtocol:@protocol(NSTextInputClient)] ||
                 [fr isKindOfClass:NSClassFromString(@"TerminalGridView")])) {
                [fr keyDown:e];
                return nil;
            }
        }
        return e;
    }];
}

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    // Open what the command line names, else the cwd. A directory becomes the
    // tree's root; a file opens in the editor with the tree rooted at the
    // folder holding it, so its neighbours are still listed. A path that does
    // not exist roots the tree at its parent when that exists, so a mistyped
    // filename still lands in the right folder.
    NSArray *args = [[NSProcessInfo processInfo] arguments];
    // Launched by Finder or the Dock to open something: those windows are
    // already open, and the cwd (/ for such a launch) would only add another.
    if (args.count < 2 && self.controllers.count > 0) {
        if (!MCBenchActive()) [NSApp activateIgnoringOtherApps:YES];
        return;
    }
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *root = fm.currentDirectoryPath;
    NSString *file = nil;
    if (args.count > 1) {
        NSString *arg = [args[1] stringByExpandingTildeInPath];
        if (![arg hasPrefix:@"/"])
            arg = [root stringByAppendingPathComponent:arg];
        // Resolved so the root and the file agree on one spelling (/tmp vs
        // /private/tmp), which revealPath: needs to walk from one to the other.
        arg = [arg stringByResolvingSymlinksInPath];
        BOOL dir = NO;
        if ([fm fileExistsAtPath:arg isDirectory:&dir]) {
            if (dir) root = arg;
            else { root = arg.stringByDeletingLastPathComponent; file = arg; }
        } else {
            NSString *parent = arg.stringByDeletingLastPathComponent;
            if ([fm fileExistsAtPath:parent isDirectory:&dir] && dir) root = parent;
            fprintf(stderr, "minicode: %s: no such file or directory\n",
                    [args[1] UTF8String]);
        }
    }
    EditorController *c = [self openWindowAtPath:root];
    if (file) [c showFile:file];
    // A benchmark runs in the background, so it never takes the keyboard.
    if (!MCBenchActive()) [NSApp activateIgnoringOtherApps:YES];
    MCDemoStart(c);   // no-op unless MINICODE_DEMO is set (make demos)
    MCBenchStart(c);  // no-op unless MINICODE_BENCH is set (make membench)
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a {
    return YES;
}

// Quitting asks about unsaved changes in every window's tabs, as closing
// each window would; windows are not closed on the way out, so nothing
// else would.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    // One question for every command still running in any window's
    // terminals, before the unsaved files.
    NSMutableArray<NSString *> *running = [NSMutableArray array];
    for (EditorController *c in self.controllers)
        [running addObjectsFromArray:[c runningTerminalCommands]];
    if (!MCConfirmClosingRunning(running, @"Quit MiniCode?", @"Quit")) return NSTerminateCancel;
    for (EditorController *c in [self.controllers copy]) {
        if (!c.hasUnsavedChanges) continue;
        [c.window makeKeyAndOrderFront:nil];
        if (![c confirmCloseAllTabs]) return NSTerminateCancel;
    }
    return NSTerminateNow;
}

// Language servers are child processes; none may outlive the app.
- (void)applicationWillTerminate:(NSNotification *)note {
    MCLspTerminateAllServers();
}

// ------------------------------------------- opening from Finder and the Dock
// Launch Services sends every open here: Open With, a double-click on a file
// MiniCode is the default app for, and a drop on the Dock icon. Handling it
// keeps AppKit from passing it to NSDocumentController, which MiniCode does
// not use and which answered "MiniCode cannot open files in the “Markdown”
// format".
- (void)application:(NSApplication *)app openURLs:(NSArray<NSURL *> *)urls {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSMutableArray<NSString *> *folders = [NSMutableArray array];
    NSMutableArray<NSString *> *files = [NSMutableArray array];
    for (NSURL *url in urls) {
        if (!url.isFileURL) continue;
        // Resolved, as a path on the command line is, so that /tmp and
        // /private/tmp name one folder.
        NSString *p = url.path.stringByResolvingSymlinksInPath;
        BOOL dir = NO;
        if (![fm fileExistsAtPath:p isDirectory:&dir]) continue;
        [(dir ? folders : files) addObject:p];
    }
    if (!folders.count && !files.count) return;
    // Before any "Save changes?" alert, so it is not behind another app.
    if (!MCBenchActive()) [NSApp activateIgnoringOtherApps:YES];

    // Folders first, so a file dropped together with its folder lands in
    // that folder's window.
    EditorController *front = nil;
    for (NSString *dir in folders)
        front = [self controllerRootedAt:dir] ?: [self openWindowAtPath:dir];

    // Files from one folder share a window, each in a tab, the last one on
    // show. (showFile: no longer refuses, since a tab with unsaved changes
    // simply stays open, but a window that did is not asked again.)
    NSMutableSet<EditorController *> *declined = [NSMutableSet set];
    for (NSString *file in files) {
        NSString *inRoot = nil;
        EditorController *c = [self controllerContaining:file spelled:&inRoot];
        if (!c) {
            c = [self openWindowAtPath:file.stringByDeletingLastPathComponent];
            inRoot = file;
        }
        front = c;
        if ([declined containsObject:c]) continue;
        [c.window makeKeyAndOrderFront:nil];
        if (![c showFile:inRoot]) [declined addObject:c];
    }
    [front.window makeKeyAndOrderFront:nil];
}

// MiniCode has no untitled documents. Now that Info.plist declares document
// types, AppKit would otherwise ask NSDocumentController for one on every
// launch that opens nothing.
- (BOOL)applicationShouldOpenUntitledFile:(NSApplication *)sender {
    return NO;
}

// The path spelled the way the tree roots compare: symlinks resolved and
// /private dropped, as for paths from the command line and from Finder.
static NSString *MCComparablePath(NSString *path) {
    return path.stringByResolvingSymlinksInPath;
}

// The part of path below root ("a/b.md"), or nil when path is not inside it.
static NSString *MCPathBelow(NSString *root, NSString *path) {
    if ([root isEqualToString:@"/"])
        return path.length > 1 ? [path substringFromIndex:1] : nil;
    NSString *prefix = [root stringByAppendingString:@"/"];
    return [path hasPrefix:prefix] ? [path substringFromIndex:prefix.length] : nil;
}

// A window rooted at dir, the frontmost if there are several.
- (EditorController *)controllerRootedAt:(NSString *)dir {
    EditorController *found = nil;
    for (EditorController *c in self.controllers) {
        if (![MCComparablePath(c.rootPath) isEqualToString:dir]) continue;
        if (!found || c == [self current]) found = c;
    }
    return found;
}

// The window whose folder holds file, the innermost if several do (the
// frontmost among equals). *spelled gets the file's path from that window's
// root as the window spells it, which revealPath: needs to walk the tree.
- (EditorController *)controllerContaining:(NSString *)file
                                   spelled:(NSString **)spelled {
    EditorController *best = nil;
    NSUInteger bestDepth = 0;
    NSString *bestBelow = nil;
    for (EditorController *c in self.controllers) {
        NSString *root = MCComparablePath(c.rootPath);
        NSString *below = MCPathBelow(root, file);
        if (!below) continue;
        NSUInteger depth = root.pathComponents.count;
        if (!best || depth > bestDepth ||
            (depth == bestDepth && c == [self current])) {
            best = c; bestDepth = depth; bestBelow = below;
        }
    }
    if (best) *spelled = [best.rootPath stringByAppendingPathComponent:bestBelow];
    return best;
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

// The controller whose window is frontmost (menu actions target it). Find
// in Folder and TODOs belong to the editor window that opened them, so
// their keys go to that one, not to whichever window was made last.
- (EditorController *)current {
    NSWindow *w = NSApp.keyWindow ?: NSApp.mainWindow;
    for (EditorController *c in self.controllers)
        if ([c ownsWindow:w]) return c;
    return [self controllerForWindow:NSApp.mainWindow] ?: self.controllers.lastObject;
}

// Keys that edit the file on show (Format Document, Sort Lines, Run File,
// Toggle Comment, completion...) act only from the editor window. In Find in
// Folder or TODOs their items are disabled, so the key reaches the field
// there: Shift+Option+F types its character, as in any other text field.
static BOOL MCEditorOnlyAction(SEL a) {
    return a == @selector(formatDocument:) || a == @selector(sortLines:) ||
           a == @selector(fixKeepSorted:) || a == @selector(runCurrentFile:) ||
           a == @selector(toggleComment:) || a == @selector(toggleTask:) ||
           a == @selector(triggerCompletion:) || a == @selector(goToDefinition:) ||
           a == @selector(showHoverInfo:);
}
- (BOOL)keyWindowIsEditor {
    NSWindow *key = NSApp.keyWindow;
    return !key || [self controllerForWindow:key] != nil;
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
- (void)exportPDF:(id)sender       { [[self current] exportPDF:sender]; }
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (MCEditorOnlyAction(item.action)) return [self keyWindowIsEditor];
    if (item.action == @selector(exportPDF:)) return [self current].canExportPDF;
    if (item.action == @selector(togglePlayback:))
        return [self current].canTogglePlayback;
    // The tab items, and Cmd+W's title, are the window's to decide.
    if (item.action == @selector(closeFocused:) || item.action == @selector(selectTabNumber:) ||
        item.action == @selector(selectNextTab:) || item.action == @selector(selectPreviousTab:)) {
        EditorController *c = [self current];
        if (!c) return item.action == @selector(closeFocused:);
        if (item.action == @selector(closeFocused:) && NSApp.keyWindow &&
            ![self controllerForWindow:NSApp.keyWindow]) {
            item.title = @"Close Window";   // another window, which just closes
            return YES;
        }
        return [c validateMenuItem:item];
    }
    return YES;
}
- (void)togglePlayback:(id)sender  { [[self current] togglePlayback:sender]; }
- (void)toggleHints:(id)sender     { [[self current] toggleHints:sender]; }
- (void)toggleTerminal:(id)sender  { [[self current] toggleTerminal:sender]; }
- (void)newTerminal:(id)sender     { [[self current] newTerminal:sender]; }
- (void)splitTerminal:(id)sender   { [[self current] splitTerminal:sender]; }
- (void)nextTerminal:(id)sender    { [[self current] nextTerminal:sender]; }
- (void)previousTerminal:(id)sender { [[self current] previousTerminal:sender]; }
- (void)closeTerminal:(id)sender   { [[self current] closeTerminal:sender]; }
- (void)runCurrentFile:(id)sender  { [[self current] runCurrentFile:sender]; }
- (void)toggleBrowser:(id)sender   { [[self current] toggleBrowser:sender]; }
- (void)toggleSidebar:(id)sender   { [[self current] toggleSidebar:sender]; }
- (void)toggleSourceControl:(id)sender { [[self current] toggleSourceControl:sender]; }
- (void)toggleEditor:(id)sender    { [[self current] toggleEditor:sender]; }
- (void)toggleHiddenFiles:(id)sender { [[self current] toggleHiddenFiles:sender]; }
- (void)openSearch:(id)sender      { [[self current] openSearch:sender]; }
- (void)focusTree:(id)sender       { [[self current] focusTree:sender]; }
- (void)focusEditor:(id)sender     { [[self current] focusEditor:sender]; }
- (void)focusTerminal:(id)sender   { [[self current] focusTerminal:sender]; }
- (void)focusBrowser:(id)sender    { [[self current] focusBrowser:sender]; }
- (void)selectNextTab:(id)sender     { [[self current] selectNextTab:sender]; }
- (void)selectPreviousTab:(id)sender { [[self current] selectPreviousTab:sender]; }
- (void)selectTabNumber:(id)sender   { [[self current] selectTabNumber:sender]; }
// Cmd+W reaches the editor window's controller first, as its delegate, so
// this runs for another window (Find in Folder, say), which just closes.
- (void)closeFocused:(id)sender {
    NSWindow *key = NSApp.keyWindow;
    if (key && ![self controllerForWindow:key]) [key performClose:sender];
    else [[self current] closeFocused:sender];
}
- (void)newFile:(id)sender         { [[self current] newFile:sender]; }
- (void)newFolder:(id)sender       { [[self current] newFolder:sender]; }
- (void)renameSelected:(id)sender  { [[self current] renameSelected:sender]; }
- (void)deleteSelected:(id)sender  { [[self current] deleteSelected:sender]; }
- (void)refreshTree:(id)sender     { [[self current] refreshTree:sender]; }
- (void)openSettings:(id)sender    { [[self current] openSettings:sender]; }
- (void)adjustAppearance:(id)sender { [[self current] adjustAppearance:sender]; }
- (void)toggleComment:(id)sender   { [[self current] toggleComment:sender]; }
- (void)formatDocument:(id)sender  { [[self current] formatDocument:sender]; }
- (void)toggleTask:(id)sender      { [[self current] toggleTask:sender]; }
- (void)sortLines:(id)sender       { [[self current] sortLines:sender]; }
- (void)fixKeepSorted:(id)sender   { [[self current] fixKeepSorted:sender]; }
- (void)showTodos:(id)sender       { [[self current] showTodos:sender]; }
- (void)triggerCompletion:(id)sender { [[self current] triggerCompletion:sender]; }
- (void)goToDefinition:(id)sender  { [[self current] goToDefinition:sender]; }
- (void)showHoverInfo:(id)sender   { [[self current] showHoverInfo:sender]; }
- (void)showLanguageServers:(id)sender { [[self current] showLanguageServers:sender]; }
- (void)restartLanguageServer:(id)sender { [[self current] restartLanguageServer:sender]; }
- (void)selectPythonInterpreter:(id)sender {
    [[self current] selectPythonInterpreter:sender];
}

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
    [appMenu addItemWithTitle:@"Settings…"
                       action:@selector(openSettings:)
                keyEquivalent:@","];
    // Beside Settings, since the settings file is where servers are named;
    // the status bar's server item opens the same popover.
    [appMenu addItemWithTitle:@"Language Servers…"
                       action:@selector(showLanguageServers:)
                keyEquivalent:@""];
    [appMenu addItemWithTitle:@"Restart Language Server"
                       action:@selector(restartLanguageServer:)
                keyEquivalent:@""];
    [appMenu addItemWithTitle:@"Select Python Interpreter…"
                       action:@selector(selectPythonInterpreter:)
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
    NSMenuItem *exportPDF =
        [[NSMenuItem alloc] initWithTitle:@"Export PDF…"
                                   action:@selector(exportPDF:) keyEquivalent:@"s"];
    exportPDF.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [fileMenu addItem:exportPDF];
    [fileMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *newFile =
        [[NSMenuItem alloc] initWithTitle:@"New File…"
                                   action:@selector(newFile:) keyEquivalent:@"n"];
    newFile.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagControl;
    [fileMenu addItem:newFile];
    NSMenuItem *newFolder =
        [[NSMenuItem alloc] initWithTitle:@"New Folder…"
                                   action:@selector(newFolder:) keyEquivalent:@"n"];
    newFolder.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [fileMenu addItem:newFolder];
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

    // Cmd+W closes what has the keyboard: the tab on show, or the terminal
    // or browser panel. The title says which (validateMenuItem:).
    [fileMenu addItemWithTitle:@"Close Tab"
                        action:@selector(closeFocused:)
                 keyEquivalent:@"w"];

    // Cmd+Shift+W closes the whole window, asking about unsaved tabs first.
    NSMenuItem *closeWindow =
        [[NSMenuItem alloc] initWithTitle:@"Close Window"
                                   action:@selector(performClose:)
                            keyEquivalent:@"w"];
    closeWindow.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [fileMenu addItem:closeWindow];

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
    [editMenu addItemWithTitle:@"Toggle Comment"
                        action:@selector(toggleComment:) keyEquivalent:@"/"];
    [editMenu addItemWithTitle:@"Toggle Task"
                        action:@selector(toggleTask:) keyEquivalent:@"l"];
    // VS Code's key on the Mac. It has no Command, so it is also a character
    // (Ï on a US keyboard); the key monitor above lets it type that outside
    // the editor. Nothing else here uses Option.
    NSMenuItem *format = [[NSMenuItem alloc] initWithTitle:@"Format Document"
                                                    action:@selector(formatDocument:)
                                             keyEquivalent:@"f"];
    format.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagShift;
    [editMenu addItem:format];
    // F9 as in Sublime Text (fn+F9 on a laptop keyboard): the selected
    // lines, or the keep-sorted block around the caret.
    NSString *f9 = [NSString stringWithFormat:@"%C", (unichar)NSF9FunctionKey];
    NSMenuItem *sortLines = [[NSMenuItem alloc] initWithTitle:@"Sort Lines"
                                                       action:@selector(sortLines:)
                                                keyEquivalent:f9];
    sortLines.keyEquivalentModifierMask = 0;
    [editMenu addItem:sortLines];
    NSMenuItem *fixSorted = [[NSMenuItem alloc] initWithTitle:@"Sort keep-sorted Blocks"
                                                       action:@selector(fixKeepSorted:)
                                                keyEquivalent:f9];
    fixSorted.keyEquivalentModifierMask = NSEventModifierFlagShift;
    [editMenu addItem:fixSorted];
    // Language server completion. Ctrl+Space is also the system shortcut for
    // switching input sources when more than one is enabled; Option+Esc
    // (the text view's complete:) reaches the same list either way.
    NSMenuItem *complete =
        [[NSMenuItem alloc] initWithTitle:@"Complete"
                                   action:@selector(triggerCompletion:)
                            keyEquivalent:@" "];
    complete.keyEquivalentModifierMask = NSEventModifierFlagControl;
    [editMenu addItem:complete];
    [editMenu addItemWithTitle:@"Show Hover Info"
                        action:@selector(showHoverInfo:) keyEquivalent:@"i"];
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
    NSMenuItem *findInFolder =
        [[NSMenuItem alloc] initWithTitle:@"Find in Folder…"
                                   action:@selector(openSearch:) keyEquivalent:@"f"];
    findInFolder.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [editMenu addItem:findInFolder];
    NSMenuItem *todos =
        [[NSMenuItem alloc] initWithTitle:@"TODOs in Folder…"
                                   action:@selector(showTodos:) keyEquivalent:@"l"];
    todos.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [editMenu addItem:todos];
    editItem.submenu = editMenu;

    // View menu
    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    [menubar addItem:viewItem];
    NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem *toggle =
        [[NSMenuItem alloc] initWithTitle:@"Toggle Preview"
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

    NSMenuItem *sidebar =
        [[NSMenuItem alloc] initWithTitle:@"Toggle Sidebar"
                                   action:@selector(toggleSidebar:)
                            keyEquivalent:@"b"];
    sidebar.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    [viewMenu addItem:sidebar];

    // Ctrl+Shift+G as in VS Code (Shift+Cmd+G is Find Previous).
    NSMenuItem *git =
        [[NSMenuItem alloc] initWithTitle:@"Toggle Source Control"
                                   action:@selector(toggleSourceControl:)
                            keyEquivalent:@"g"];
    git.keyEquivalentModifierMask =
        NSEventModifierFlagControl | NSEventModifierFlagShift;
    [viewMenu addItem:git];

    NSMenuItem *editorPane =
        [[NSMenuItem alloc] initWithTitle:@"Toggle Editor"
                                   action:@selector(toggleEditor:)
                            keyEquivalent:@"e"];
    editorPane.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [viewMenu addItem:editorPane];

    NSMenuItem *hidden =
        [[NSMenuItem alloc] initWithTitle:@"Show Hidden Files"
                                   action:@selector(toggleHiddenFiles:)
                            keyEquivalent:@"."];
    hidden.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [viewMenu addItem:hidden];

    // Terminal: Shift+Cmd+T in the menu, like the other pane toggles; Ctrl+`
    // also works, caught by the local key monitor above.
    NSMenuItem *term =
        [[NSMenuItem alloc] initWithTitle:@"Toggle Terminal"
                                   action:@selector(toggleTerminal:)
                            keyEquivalent:@"t"];
    term.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [viewMenu addItem:term];

    NSMenuItem *browser =
        [[NSMenuItem alloc] initWithTitle:@"Toggle Browser"
                                   action:@selector(toggleBrowser:)
                            keyEquivalent:@"b"];
    browser.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [viewMenu addItem:browser];

    [viewMenu addItem:[NSMenuItem separatorItem]];

    // Video and audio. Not Cmd+Return, which commits in the git panel's
    // message box, and not Space alone, which the tree and text need; the
    // player's own Space works once it has the focus (Cmd+1).
    NSMenuItem *play =
        [[NSMenuItem alloc] initWithTitle:@"Play/Pause"
                                   action:@selector(togglePlayback:)
                            keyEquivalent:@" "];
    play.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [viewMenu addItem:play];

    [viewMenu addItem:[NSMenuItem separatorItem]];
    // The same as a right click's Adjust Settings, for each part of the window.
    NSMenuItem *adjust = [[NSMenuItem alloc] initWithTitle:@"Adjust Settings"
                                                    action:nil keyEquivalent:@""];
    adjust.submenu = MCAdjustSettingsSubmenu();
    [viewMenu addItem:adjust];

    viewItem.submenu = viewMenu;

    // Terminal menu: several terminals, and running the open file in one.
    NSMenuItem *termItem = [[NSMenuItem alloc] init];
    [menubar addItem:termItem];
    NSMenu *termMenu = [[NSMenu alloc] initWithTitle:@"Terminal"];
    // Ctrl+Shift+` as in VS Code; the key monitor above catches it, since a
    // shifted backtick is no clean key equivalent. Shown here for reference.
    NSMenuItem *newTerm =
        [[NSMenuItem alloc] initWithTitle:@"New Terminal"
                                   action:@selector(newTerminal:)
                            keyEquivalent:@"`"];
    newTerm.keyEquivalentModifierMask =
        NSEventModifierFlagControl | NSEventModifierFlagShift;
    [termMenu addItem:newTerm];
    [termMenu addItemWithTitle:@"Split Terminal"
                        action:@selector(splitTerminal:)
                 keyEquivalent:@"\\"];
    NSMenuItem *nextTerm =
        [[NSMenuItem alloc] initWithTitle:@"Next Terminal"
                                   action:@selector(nextTerminal:)
                            keyEquivalent:@"]"];
    nextTerm.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [termMenu addItem:nextTerm];
    NSMenuItem *prevTerm =
        [[NSMenuItem alloc] initWithTitle:@"Previous Terminal"
                                   action:@selector(previousTerminal:)
                            keyEquivalent:@"["];
    prevTerm.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [termMenu addItem:prevTerm];
    NSMenuItem *closeTerm =
        [[NSMenuItem alloc] initWithTitle:@"Close Terminal"
                                   action:@selector(closeTerminal:)
                            keyEquivalent:@"w"];
    closeTerm.keyEquivalentModifierMask =
        NSEventModifierFlagControl | NSEventModifierFlagShift;
    [termMenu addItem:closeTerm];
    [termMenu addItem:[NSMenuItem separatorItem]];
    // Not Cmd+Return, which commits in the git panel's message box.
    NSMenuItem *runFile =
        [[NSMenuItem alloc] initWithTitle:@"Run File"
                                   action:@selector(runCurrentFile:)
                            keyEquivalent:@"\r"];
    runFile.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [termMenu addItem:runFile];
    termItem.submenu = termMenu;

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
    [navMenu addItemWithTitle:@"Focus Terminal"
                       action:@selector(focusTerminal:)
                keyEquivalent:@"2"];
    [navMenu addItemWithTitle:@"Focus Browser"
                       action:@selector(focusBrowser:)
                keyEquivalent:@"3"];
    [navMenu addItem:[NSMenuItem separatorItem]];
    // Tabs. Each action has a second key on a hidden item: Ctrl+Tab as in
    // VS Code and browsers, Shift+Cmd+] as in Safari and Terminal.
    NSMenuItem *(^addKey)(NSString *, SEL, NSString *, NSEventModifierFlags, BOOL) =
        ^NSMenuItem *(NSString *title, SEL action, NSString *key,
                      NSEventModifierFlags mask, BOOL hidden) {
        NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:title action:action
                                             keyEquivalent:key];
        it.keyEquivalentModifierMask = mask;
        if (hidden) {
            it.hidden = YES;
            it.allowsKeyEquivalentWhenHidden = YES;
        }
        [navMenu addItem:it];
        return it;
    };
    const NSEventModifierFlags ctrl = NSEventModifierFlagControl;
    const NSEventModifierFlags shiftCmd = NSEventModifierFlagShift | NSEventModifierFlagCommand;
    addKey(@"Next Tab", @selector(selectNextTab:), @"\t", ctrl, NO);
    addKey(@"Previous Tab", @selector(selectPreviousTab:), @"\t",
           ctrl | NSEventModifierFlagShift, NO);
    addKey(@"Next Tab", @selector(selectNextTab:), @"]", shiftCmd, YES);
    addKey(@"Previous Tab", @selector(selectPreviousTab:), @"[", shiftCmd, YES);
    // Shift+Cmd+1 to 9 as asked, with Ctrl+1 to 9 (VS Code's keys on the
    // Mac) beside them, because macOS keeps Shift+Cmd+3, 4 and 5 for
    // screenshots and the app never sees them. 9 is the last tab.
    NSMenuItem *goItem = [[NSMenuItem alloc] initWithTitle:@"Go to Tab" action:nil
                                             keyEquivalent:@""];
    NSMenu *goMenu = [[NSMenu alloc] initWithTitle:@"Go to Tab"];
    for (int n = 1; n <= 9; n++) {
        NSString *digit = [NSString stringWithFormat:@"%d", n];
        NSString *title = n == 9 ? @"Last Tab" : [NSString stringWithFormat:@"Tab %d", n];
        NSMenuItem *a = [[NSMenuItem alloc] initWithTitle:title
                                                   action:@selector(selectTabNumber:)
                                            keyEquivalent:digit];
        a.keyEquivalentModifierMask = shiftCmd;
        a.tag = n;
        [goMenu addItem:a];
        NSMenuItem *b = [[NSMenuItem alloc] initWithTitle:title
                                                   action:@selector(selectTabNumber:)
                                            keyEquivalent:digit];
        b.keyEquivalentModifierMask = ctrl;
        b.tag = n;
        b.hidden = YES;
        b.allowsKeyEquivalentWhenHidden = YES;
        [goMenu addItem:b];
    }
    goItem.submenu = goMenu;
    [navMenu addItem:goItem];
    [navMenu addItem:[NSMenuItem separatorItem]];
    // F12 as in most editors (fn+F12 on a laptop keyboard); Cmd+click works too.
    NSMenuItem *definition =
        [[NSMenuItem alloc] initWithTitle:@"Go to Definition"
                                   action:@selector(goToDefinition:)
                            keyEquivalent:[NSString stringWithFormat:@"%C",
                                              (unichar)NSF12FunctionKey]];
    definition.keyEquivalentModifierMask = 0;
    [navMenu addItem:definition];
    navItem.submenu = navMenu;
}

int main(int argc, const char *argv[]) {
    (void)argc; (void)argv;   // args are read via NSProcessInfo
    @autoreleasepool {
        if (MCDemoListScenes()) return 0;   // MINICODE_DEMO=list
        // A path on the command line (the minicode launcher, make run) is read
        // in applicationDidFinishLaunching. Left to AppKit, it would also be
        // taken as a file to open and handed on a second time.
        [[NSUserDefaults standardUserDefaults]
            registerDefaults:@{@"NSTreatUnknownArgumentsAsOpen": @"NO"}];
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppDelegate *delegate = [[AppDelegate alloc] init];
        app.delegate = delegate;
        BuildMenu();
        [app run];
    }
    return 0;
}
