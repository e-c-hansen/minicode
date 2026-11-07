// main.mm — application bootstrap and menu bar. Objective-C++.
#import <Cocoa/Cocoa.h>
#import "EditorController.h"
#import "Lsp.h"
#import "Demo.h"
#import "Bench.h"
#import "Appearance.h"
#import "AppSettings.h"
#import "TerminalPanel.h"
#import "TreeDetails.h"
#import "ChangePanel.h"
#import "PalettePanel.h"
#import "BackupStore.h"
#import "EditorFont.h"
#import "IdeLinkServer.h"
#import "ControlServer.h"
#import "Keymap.h"
#import "TerminalAttention.h"
#import "RecentFolders.h"

@interface AppDelegate : NSObject <NSApplicationDelegate, MCRestarting, MCControlAppHost>
@property(nonatomic, strong) NSMutableArray<EditorController *> *controllers;
@property(nonatomic, strong) id keyMonitor;
// Request a Change's restart, between the question and the quit: the app
// to open, and what it reopens (see restartIntoApp:).
@property(nonatomic, copy) NSString *restartApp;
@property(nonatomic, copy) NSDictionary *restartRecord;
@end

// Command and the period key, nothing else held (Quick Fix, or cancel).
static BOOL MCIsCommandPeriod(NSEvent *e) {
    NSEventModifierFlags m = e.modifierFlags &
        (NSEventModifierFlagCommand | NSEventModifierFlagShift |
         NSEventModifierFlagOption | NSEventModifierFlagControl);
    return m == NSEventModifierFlagCommand &&
           [e.charactersIgnoringModifiers isEqualToString:@"."];
}

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

    // Keys that would not reach their menu item by themselves, or not first:
    // which ones and why are in Keymap.h (Ctrl+` and Ctrl+Shift+` for the
    // terminal, Shift+Option+F and the F keys in the editor, a tour's
    // Option+] and Option+[, Cmd+keypad 0, the Menu key, Ctrl+G for a
    // program on the terminal's grid). They follow keys.conf like the menus.
    // Everything else is a normal menu key, which works in every pane.
    self.keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                          handler:^NSEvent *(NSEvent *e) {
        // Cmd+. is Quick Fix in the editor and cancel everywhere else (a
        // find field, the terminal, a sheet). In the editor the menu gets
        // it; elsewhere the event goes where it went before the menu had
        // the key: the window's own key equivalents (a sheet's Cancel),
        // then the first responder, whose key bindings make Cmd+. the
        // cancelOperation: that Esc also sends.
        if (MCIsCommandPeriod(e) && e.window && !NSApp.modalWindow && MCKeymapEventIsBound(e)) {
            NSResponder *fr = e.window.firstResponder;
            if ([self controllerForWindow:e.window] && [fr isKindOfClass:[CodeTextView class]] &&
                ((NSTextView *)fr).editable) {
                if ([NSApp.mainMenu performKeyEquivalent:e]) return nil;
                return e;
            }
            if (![e.window performKeyEquivalent:e]) [fr keyDown:e];
            return nil;
        }
        if (![self controllerForWindow:e.window]) return e;
        return MCKeymapHandleKeyDown(e) ? nil : e;
    }];

    // The control channel (`minicode open`, `state`, ...; ControlServer.h).
    MCControlServerStart(self);
}

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    // After whichever windows this launch opens: folders whose unsaved
    // buffers outlived a crash get theirs (BackupStore).
    dispatch_async(dispatch_get_main_queue(), ^{ [self reopenFoldersWithBackups]; });
    // Started by Request a Change's restart: the windows that were open, and
    // the conversation with Claude where it was.
    NSDictionary *restart = MCTakeRestartRecord();
    if (restart && [NSProcessInfo processInfo].arguments.count < 2 &&
        self.controllers.count == 0 && [self reopenAfterRestart:restart]) {
        if (!MCBenchActive()) [NSApp activateIgnoringOtherApps:YES];
        return;
    }
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

// A window for every folder with backups that no window holds (a crash in a
// window on another folder than this launch's), behind the windows this
// launch opened.
- (void)reopenFoldersWithBackups {
    MCBackupStore *store = [MCBackupStore shared];
    if (!store) return;
    NSMutableArray<NSString *> *open = [NSMutableArray array];
    for (EditorController *c in self.controllers)
        if (c.rootPath) [open addObject:c.rootPath.stringByResolvingSymlinksInPath];
    NSWindow *front = self.controllers.lastObject.window;
    BOOL opened = NO;
    for (NSString *root in [store foldersToReopenExcept:open]) {
        BOOL dir = NO;
        if (![[NSFileManager defaultManager] fileExistsAtPath:root isDirectory:&dir] || !dir)
            continue;   // its backups wait, and go after 30 days
        [self openWindowAtPath:root];
        opened = YES;
    }
    if (opened && front) [front makeKeyAndOrderFront:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a {
    return YES;
}

// Quitting asks about unsaved changes in every window's tabs, as closing
// each window would; windows are not closed on the way out, so nothing
// else would.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    const BOOL restarting = self.restartApp != nil;
    NSApplicationTerminateReply reply = [self confirmQuitRestarting:restarting];
    if (reply == NSTerminateCancel) {
        self.restartApp = nil;
        self.restartRecord = nil;
    }
    return reply;
}

- (NSApplicationTerminateReply)confirmQuitRestarting:(BOOL)restarting {
    // One question for every command still running in any window's
    // terminals, before the unsaved files. Claude's terminal is not asked
    // about on a restart: the new build continues the conversation.
    NSMutableArray<NSString *> *running = [NSMutableArray array];
    for (EditorController *c in self.controllers)
        [running addObjectsFromArray:restarting ? [c runningTerminalCommandsExceptChange]
                                                : [c runningTerminalCommands]];
    if (!MCConfirmClosingRunning(running, restarting ? @"Restart minicode?" : @"Quit MiniCode?",
                                 restarting ? @"Restart" : @"Quit"))
        return NSTerminateCancel;
    for (EditorController *c in [self.controllers copy]) {
        if (!c.hasUnsavedChanges) continue;
        if (restarting && [c keepUnsavedTabsForRestart]) continue;   // they come back
        [c.window makeKeyAndOrderFront:nil];
        if (![c confirmCloseAllTabs]) return NSTerminateCancel;
    }
    return NSTerminateNow;
}

// Language servers are child processes; none may outlive the app.
- (void)applicationWillTerminate:(NSNotification *)note {
    // Every window's session as it is now: the browser's page moves without
    // anything else saving it.
    for (EditorController *c in self.controllers) [c saveSession];
    MCControlServerStop();
    MCLspTerminateAllServers();
    if (self.restartApp) {
        MCSaveRestartRecord(self.restartRecord ?: @{});
        MCLaunchAfterExit(self.restartApp);
    }
}

// ------------------------------------------------ restarting into a build
// Request a Change's Restart button. The questions quitting asks come first
// (Cancel at any of them leaves everything as it was); then each window's
// folder is written down for the next launch (their tabs come back with the
// folders' sessions), and a detached shell opens the new build once this
// process has gone.
- (void)restartIntoApp:(NSString *)appPath resumingIn:(id)controller
                source:(NSString *)source command:(NSString *)command {
    if (![[NSFileManager defaultManager] fileExistsAtPath:appPath]) { NSBeep(); return; }
    NSMutableArray *windows = [NSMutableArray array];
    // The window asking first, so it comes back in front.
    NSMutableArray<EditorController *> *order = [self.controllers mutableCopy];
    if ([order containsObject:controller]) {
        [order removeObject:controller];
        [order insertObject:controller atIndex:0];
    }
    for (EditorController *c in order) {
        if (!c.rootPath) continue;
        [windows addObject:@{@"root": c.rootPath, @"resume": @(c == controller)}];
    }
    self.restartRecord = @{@"windows": windows, @"source": source ?: @"",
                           @"command": command ?: @""};
    self.restartApp = appPath;
    [NSApp terminate:nil];
    // Still here: a question was cancelled (applicationShouldTerminate
    // cleared the restart).
}

- (BOOL)reopenAfterRestart:(NSDictionary *)record {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *source = [record[@"source"] isKindOfClass:NSString.class] ? record[@"source"] : nil;
    NSString *command = [record[@"command"] isKindOfClass:NSString.class] ? record[@"command"] : nil;
    EditorController *front = nil, *resume = nil;
    NSArray *windows = [record[@"windows"] isKindOfClass:NSArray.class] ? record[@"windows"] : @[];
    // Opened back to front, so the first ends up in front.
    for (NSDictionary *w in windows.reverseObjectEnumerator) {
        if (![w isKindOfClass:NSDictionary.class]) continue;
        NSString *root = w[@"root"];
        BOOL dir = NO;
        if (![root isKindOfClass:NSString.class] ||
            ![fm fileExistsAtPath:root isDirectory:&dir] || !dir)
            continue;
        EditorController *c = [self openWindowAtPath:root];
        front = c;
        if ([w[@"resume"] boolValue]) resume = c;
    }
    if (!front) return NO;
    [front.window makeKeyAndOrderFront:nil];
    if (resume && source.length) [resume resumeChangeWithSource:source command:command];
    return YES;
}

// ------------------------------------------- opening from Finder and the Dock
// `minicode -r` cannot say "reuse the window" through Launch Services, which
// only passes paths, so it writes its request to a file in
// $TMPDIR/MiniCode-open and opens that: "minicode-open-1", "reuse", then the
// paths, each ended by a NUL. Only files in that folder of this user's own
// temporary folder are read, and each is deleted once read.
static NSArray<NSString *> *MCTakeOpenRequest(NSURL *url, BOOL *reuse) {
    if (!url.isFileURL || ![url.pathExtension isEqualToString:@"minicode-open"]) return nil;
    NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"MiniCode-open"]
                        .stringByResolvingSymlinksInPath;
    NSString *path = url.path.stringByResolvingSymlinksInPath;
    if (![path.stringByDeletingLastPathComponent isEqualToString:dir]) return nil;
    NSData *data = [NSData dataWithContentsOfFile:path];
    [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    NSString *text = data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : nil;
    const unichar nul = 0;
    NSArray<NSString *> *fields =
        [text componentsSeparatedByString:[NSString stringWithCharacters:&nul length:1]];
    if (fields.count < 2 || ![fields[0] isEqualToString:@"minicode-open-1"]) return @[];
    *reuse = [fields[1] isEqualToString:@"reuse"];
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    for (NSUInteger i = 2; i < fields.count; i++)
        if ([fields[i] hasPrefix:@"/"]) [paths addObject:fields[i]];
    return paths;
}

// Launch Services sends every open here: Open With, a double-click on a file
// MiniCode is the default app for, and a drop on the Dock icon. Handling it
// keeps AppKit from passing it to NSDocumentController, which MiniCode does
// not use and which answered "MiniCode cannot open files in the “Markdown”
// format".
- (void)application:(NSApplication *)app openURLs:(NSArray<NSURL *> *)urls {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSMutableArray<NSString *> *folders = [NSMutableArray array];
    NSMutableArray<NSString *> *files = [NSMutableArray array];
    BOOL reuse = NO;
    NSMutableArray<NSURL *> *all = [NSMutableArray array];
    for (NSURL *url in urls) {
        NSArray<NSString *> *requested = MCTakeOpenRequest(url, &reuse);
        if (!requested) { [all addObject:url]; continue; }
        for (NSString *p in requested) [all addObject:[NSURL fileURLWithPath:p]];
    }
    for (NSURL *url in all) {
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
    // that folder's window. With -r the window in front takes the first
    // folder and every file, as VS Code's --reuse-window does.
    EditorController *front = nil;
    EditorController *reused = reuse ? [self current] : nil;
    for (NSString *dir in folders) {
        if (reused && !front) {
            if (![reused switchToFolder:dir]) return;   // Cancel at its unsaved tabs
            front = reused;
            continue;
        }
        front = [self controllerRootedAt:dir] ?: [self openWindowAtPath:dir];
    }
    if (reused && !front) front = reused;

    // Files from one folder share a window, each in a tab, the last one on
    // show. A file no window's folder holds becomes a tab of the window in
    // front (or of the folder just opened), as in VS Code: `minicode x.py`
    // typed in a window's terminal opens it there. Only with no window at
    // all does a new one open on the file's folder. (showFile: no longer
    // refuses, since a tab with unsaved changes simply stays open, but a
    // window that did is not asked again.)
    NSMutableSet<EditorController *> *declined = [NSMutableSet set];
    for (NSString *file in files) {
        NSString *inRoot = nil;
        EditorController *c = reused ? nil : [self controllerContaining:file spelled:&inRoot];
        if (reused && front) {
            c = front;
            NSString *below = MCPathBelow(MCComparablePath(c.rootPath), file);
            inRoot = below ? [c.rootPath stringByAppendingPathComponent:below] : file;
        }
        if (!c && (c = front ?: [self current])) inRoot = file;
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

// ------------------------------------------- the control channel's host
- (NSArray<EditorController *> *)controlControllers { return [self.controllers copy]; }
- (EditorController *)controlFrontController { return self.controllers.count ? [self current] : nil; }
- (EditorController *)controlControllerContaining:(NSString *)file spelled:(NSString **)spelled {
    NSString *inRoot = nil;
    EditorController *c = [self controllerContaining:MCComparablePath(file) spelled:&inRoot];
    if (spelled) *spelled = inRoot;
    return c;
}
- (EditorController *)controlControllerRootedAt:(NSString *)dir {
    return [self controllerRootedAt:MCComparablePath(dir)];
}
- (EditorController *)controlOpenWindowAtPath:(NSString *)dir { return [self openWindowAtPath:dir]; }

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
           a == @selector(showHoverInfo:) || a == @selector(renameSymbol:) ||
           a == @selector(findReferences:) || a == @selector(showQuickFixes:) ||
           a == @selector(organizeImports:) ||
           a == @selector(indentLines:) || a == @selector(outdentLines:) ||
           a == @selector(insertLineBelow:) || a == @selector(insertLineAbove:) ||
           a == @selector(toggleBold:) || a == @selector(toggleItalic:) ||
           a == @selector(runSelectionInTerminal:) ||
           a == @selector(nextProblem:) || a == @selector(previousProblem:) ||
           a == @selector(goToBracket:) || a == @selector(toggleSidePreview:);
}
// The File menu's tab commands and the tree's Sort By items, which the
// window decides (enabled, checked), forwarded when another window is key.
static BOOL MCWindowTabAction(SEL a) {
    return a == @selector(reopenClosedTab:) || a == @selector(closeOtherTabs:) ||
           a == @selector(closeTabsToTheRight:) || a == @selector(closeAllTabs:) ||
           a == @selector(copyActiveFilePath:) || a == @selector(copyActiveFileRelativePath:) ||
           a == @selector(revealActiveFileInFinder:) || a == @selector(revealActiveFileInTree:) ||
           a == @selector(sortTreeBy:) || a == @selector(setTreeSortDirection:) ||
           a == @selector(saveFileAs:) || a == @selector(saveAllFiles:) ||
           a == @selector(toggleFoldersFirst:);
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
- (void)saveFileAs:(id)sender      { [[self current] saveFileAs:sender]; }
- (void)saveAllFiles:(id)sender    { [[self current] saveAllFiles:sender]; }
- (void)indentLines:(id)sender     { [[self current] indentLines:sender]; }
- (void)outdentLines:(id)sender    { [[self current] outdentLines:sender]; }
- (void)insertLineBelow:(id)sender { [[self current] insertLineBelow:sender]; }
- (void)insertLineAbove:(id)sender { [[self current] insertLineAbove:sender]; }
- (void)toggleBold:(id)sender      { [[self current] toggleBold:sender]; }
- (void)toggleItalic:(id)sender    { [[self current] toggleItalic:sender]; }
- (void)runSelectionInTerminal:(id)sender { [[self current] runSelectionInTerminal:sender]; }
// Ctrl+R, VS Code's Open Recent: the palette on File > Open Recent's
// folders (RecentFolders.h), filtered as it is typed into.
- (void)openRecent:(id)sender { (void)sender; [self showPaletteWithText:@">Open Recent > "]; }
// A folder from File > Open Recent: the window already on it comes
// forward, else the window in front moves to it (asking about unsaved tabs,
// as Open Folder does), else a window opens on it.
- (void)openRecentFolder:(id)sender {
    NSString *path = [sender respondsToSelector:@selector(representedObject)]
                         ? [sender representedObject] : nil;
    if (![path isKindOfClass:[NSString class]] || !path.length) return;
    BOOL dir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&dir] || !dir) {
        NSBeep();
        return;
    }
    NSString *real = path.stringByResolvingSymlinksInPath;
    for (EditorController *c in self.controllers)
        if ([c.rootPath.stringByResolvingSymlinksInPath isEqualToString:real]) {
            [c.window makeKeyAndOrderFront:nil];
            return;
        }
    EditorController *c = [self current];
    if (c) [c switchToFolder:path];
    else [self openWindowAtPath:path];
}
- (void)clearRecentFolders:(id)sender { (void)sender; MCClearRecentFolders(); }
- (void)togglePreview:(id)sender   { [[self current] togglePreview:sender]; }
- (void)toggleSidePreview:(id)sender { [[self current] toggleSidePreview:sender]; }
- (void)exportPDF:(id)sender       { [[self current] exportPDF:sender]; }
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (MCEditorOnlyAction(item.action)) return [self keyWindowIsEditor];
    if (item.action == @selector(toggleTreeColumn:)) {
        item.state = MCTreeColumnShown((MCTreeColumns)item.tag) ? NSControlStateValueOn
                                                                 : NSControlStateValueOff;
        return YES;
    }
    if (item.action == @selector(showContextMenu:))
        return (NSApp.keyWindow ?: NSApp.mainWindow) != nil;
    if (item.action == @selector(exportPDF:)) return [self current].canExportPDF;
    if (item.action == @selector(togglePlayback:))
        return [self current].canTogglePlayback;
    if (item.action == @selector(nextTourStop:) || item.action == @selector(previousTourStop:))
        return [self current].tourActive;
    if (item.action == @selector(toggleProblems:) || item.action == @selector(gitClean:) ||
        item.action == @selector(gitUndoDiscard:)) {
        EditorController *c = [self current];
        return c ? [c validateMenuItem:item] : NO;
    }
    // The tab items, and Cmd+W's title, are the window's to decide.
    if (item.action == @selector(closeFocused:) || item.action == @selector(selectTabNumber:) ||
        item.action == @selector(selectNextTab:) || item.action == @selector(selectPreviousTab:) ||
        MCWindowTabAction(item.action)) {
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
- (void)nextTourStop:(id)sender    { [[self current] nextTourStop:sender]; }
- (void)previousTourStop:(id)sender { [[self current] previousTourStop:sender]; }
- (void)toggleHints:(id)sender     { [[self current] toggleHints:sender]; }
- (void)toggleTerminal:(id)sender  { [[self current] toggleTerminal:sender]; }
- (void)newTerminal:(id)sender     { [[self current] newTerminal:sender]; }
- (void)splitTerminal:(id)sender   { [[self current] splitTerminal:sender]; }
- (void)nextTerminal:(id)sender    { [[self current] nextTerminal:sender]; }
- (void)previousTerminal:(id)sender { [[self current] previousTerminal:sender]; }
- (void)closeTerminal:(id)sender   { [[self current] closeTerminal:sender]; }
- (void)nextWaitingTerminal:(id)sender { MCJumpToNextWaitingTerminal(); }
- (void)runCurrentFile:(id)sender  { [[self current] runCurrentFile:sender]; }
- (void)toggleBrowser:(id)sender   { [[self current] toggleBrowser:sender]; }
- (void)toggleSidebarVisibility:(id)sender   { [[self current] toggleSidebarVisibility:sender]; }
- (void)toggleSourceControl:(id)sender { [[self current] toggleSourceControl:sender]; }
- (void)gitFetch:(id)sender { [[self current] gitFetch:sender]; }
- (void)gitPull:(id)sender  { [[self current] gitPull:sender]; }
- (void)gitPush:(id)sender  { [[self current] gitPush:sender]; }
- (void)gitSync:(id)sender  { [[self current] gitSync:sender]; }
- (void)gitStageAll:(id)sender    { [[self current] gitStageAll:sender]; }
- (void)gitUnstageAll:(id)sender  { [[self current] gitUnstageAll:sender]; }
- (void)gitClean:(id)sender       { [[self current] gitClean:sender]; }
- (void)gitUndoDiscard:(id)sender { [[self current] gitUndoDiscard:sender]; }
- (void)gitCheckout:(id)sender    { [[self current] gitCheckout:sender]; }
- (void)toggleAgents:(id)sender    { [[self current] toggleAgents:sender]; }
- (void)toggleProcesses:(id)sender { [[self current] toggleProcesses:sender]; }
- (void)toggleEditor:(id)sender    { [[self current] toggleEditor:sender]; }
- (void)toggleHiddenFiles:(id)sender { [[self current] toggleHiddenFiles:sender]; }
- (void)requestChange:(id)sender   { [[self current] requestChange:sender]; }
// The palette (PalettePanel.h): commands (Shift+Cmd+P), the folder's files
// (Cmd+P) and Go to Line (Ctrl+G) are one view, chosen by its first character.
- (void)showPaletteWithText:(NSString *)text {
    EditorController *c = [self current];
    if (!c) return;
    MCPaletteContext *ctx = [MCPaletteContext new];
    ctx.root = c.rootPath;
    ctx.recentFiles = [c recentTabPaths];
    __weak EditorController *weak = c;
    ctx.openFile = ^(NSString *path) {
        EditorController *e = weak;
        [e revealPath:path andOpen:YES];   // as a click in the tree
    };
    if (c.canGoToLine)
        ctx.goToLine = ^(NSInteger line, NSInteger column) {
            [weak goToLine:(int)line column:(int)column];
        };
    [c addSymbolsToPaletteContext:ctx];
    MCShowPalette(c.window, text, ctx);
}
- (void)showCommandPalette:(id)sender { (void)sender; [self showPaletteWithText:@">"]; }
- (void)quickOpen:(id)sender          { (void)sender; [self showPaletteWithText:@""]; }
- (void)goToLinePrompt:(id)sender     { (void)sender; [self showPaletteWithText:@":"]; }
- (void)goToSymbolInFile:(id)sender   { (void)sender; [self showPaletteWithText:@"@"]; }
- (void)goToSymbolInWorkspace:(id)sender { (void)sender; [self showPaletteWithText:@"#"]; }
- (void)navigateBack:(id)sender       { [[self current] navigateBack:sender]; }
- (void)navigateForward:(id)sender    { [[self current] navigateForward:sender]; }
// App-wide, so it works from any window.
- (void)toggleTreeColumn:(id)sender { MCToggleTreeColumn((MCTreeColumns)[sender tag]); }
- (void)showContextMenu:(id)sender {
    (void)sender;
    MCShowContextMenuInWindow(NSApp.keyWindow ?: NSApp.mainWindow);
}
- (void)openSearch:(id)sender      { [[self current] openSearch:sender]; }
- (void)openReplaceInFolder:(id)sender { [[self current] openReplaceInFolder:sender]; }
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
// Cmd+N with no window open opens one, as New Window does.
- (void)newFile:(id)sender {
    EditorController *c = [self current];
    if (c) [c newFile:sender];
    else [self newWindow:sender];
}
- (void)revealInFinder:(id)sender  { [[self current] revealInFinder:sender]; }
- (void)copyPath:(id)sender        { [[self current] copyPath:sender]; }
- (void)copyRelativePath:(id)sender { [[self current] copyRelativePath:sender]; }
- (void)sortTreeBy:(id)sender      { [[self current] sortTreeBy:sender]; }
- (void)setTreeSortDirection:(id)sender { [[self current] setTreeSortDirection:sender]; }
- (void)toggleFoldersFirst:(id)sender { [[self current] toggleFoldersFirst:sender]; }
- (void)reopenClosedTab:(id)sender { [[self current] reopenClosedTab:sender]; }
- (void)closeOtherTabs:(id)sender  { [[self current] closeOtherTabs:sender]; }
- (void)closeTabsToTheRight:(id)sender { [[self current] closeTabsToTheRight:sender]; }
- (void)closeAllTabs:(id)sender    { [[self current] closeAllTabs:sender]; }
- (void)copyActiveFilePath:(id)sender { [[self current] copyActiveFilePath:sender]; }
- (void)copyActiveFileRelativePath:(id)sender {
    [[self current] copyActiveFileRelativePath:sender];
}
- (void)revealActiveFileInFinder:(id)sender { [[self current] revealActiveFileInFinder:sender]; }
- (void)revealActiveFileInTree:(id)sender { [[self current] revealActiveFileInTree:sender]; }
- (void)newFolder:(id)sender       { [[self current] newFolder:sender]; }
- (void)renameSelected:(id)sender  { [[self current] renameSelected:sender]; }
- (void)deleteSelected:(id)sender  { [[self current] deleteSelected:sender]; }
- (void)refreshTree:(id)sender     { [[self current] refreshTree:sender]; }
- (void)openSettings:(id)sender    { [[self current] openSettings:sender]; }
- (void)openKeyboardShortcuts:(id)sender { [[self current] openKeyboardShortcuts:sender]; }
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
- (void)goToBracket:(id)sender     { [[self current] goToBracket:sender]; }
- (void)inlineAgent:(id)sender     { [[self current] inlineAgent:sender]; }
// Restart minicode, as after a rebuild from Request a Change but on demand:
// the same questions, then the same bundle opened again with every window's
// folder, its tabs, unsaved changes and browser page. A conversation with
// Claude from Request a Change still running in a window goes on there.
- (void)restartMinicode:(id)sender {
    (void)sender;
    NSString *app = NSBundle.mainBundle.bundlePath;
    if (![app.pathExtension isEqualToString:@"app"]) { NSBeep(); return; }
    EditorController *resume = [self current];
    NSString *source = nil;
    for (EditorController *c in self.controllers) {
        NSString *s = [c changeSourceToResume];
        if (s.length) { resume = c; source = s; break; }
    }
    NSString *command = source
        ? @([AppSettings shared].settings.developCommand().c_str()) : nil;
    [self restartIntoApp:app resumingIn:resume source:source command:command];
}
- (void)renameSymbol:(id)sender    { [[self current] renameSymbol:sender]; }
- (void)findReferences:(id)sender  { [[self current] findReferences:sender]; }
- (void)showQuickFixes:(id)sender  { [[self current] showQuickFixes:sender]; }
- (void)organizeImports:(id)sender { [[self current] organizeImports:sender]; }
- (void)nextProblem:(id)sender     { [[self current] nextProblem:sender]; }
- (void)previousProblem:(id)sender { [[self current] previousProblem:sender]; }
- (void)toggleProblems:(id)sender  { [[self current] toggleProblems:sender]; }
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
    // keys.conf (Keymap.h), written with every command when there is none.
    // VS Code's key is a Cmd+K chord, and minicode has no chords; the
    // palette finds it.
    [appMenu addItemWithTitle:@"Keyboard Shortcuts…"
                       action:@selector(openKeyboardShortcuts:)
                keyEquivalent:@""];
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
    // Hands a change to Claude Code in a terminal of minicode's own source
    // folder, then restarts into the build (ChangePanel.h).
    NSMenuItem *change = [[NSMenuItem alloc] initWithTitle:@"Request a Change to minicode…"
                                                    action:@selector(requestChange:)
                                             keyEquivalent:@"k"];
    change.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [appMenu addItem:change];
    // Ad hoc, as VS Code's Reload Window: no key of its own (Cmd+R refreshes
    // the file tree); the palette and the status bar's ↻ reach it.
    [appMenu addItemWithTitle:@"Restart minicode"
                       action:@selector(restartMinicode:)
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
    // Cmd+N is New File, as in VS Code. It was New Window, which moved to
    // Option+Cmd+N (VS Code's Shift+Cmd+N is New Folder here).
    [fileMenu addItemWithTitle:@"New File…"
                        action:@selector(newFile:)
                 keyEquivalent:@"n"];
    NSMenuItem *newWindow =
        [[NSMenuItem alloc] initWithTitle:@"New Window"
                                   action:@selector(newWindow:) keyEquivalent:@"n"];
    newWindow.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [fileMenu addItem:newWindow];
    [fileMenu addItemWithTitle:@"Open Folder…"
                        action:@selector(openFolder:)
                 keyEquivalent:@"o"];
    // The folders opened before (RecentFolders.h), and Ctrl+R, VS Code's
    // key, for the same list in the palette.
    NSMenuItem *recentItem = [[NSMenuItem alloc] initWithTitle:@"Open Recent" action:nil
                                                 keyEquivalent:@""];
    NSMenu *recentMenu = [[NSMenu alloc] initWithTitle:@"Open Recent"];
    MCInstallRecentFoldersMenu(recentMenu);
    recentItem.submenu = recentMenu;
    [fileMenu addItem:recentItem];
    NSMenuItem *openRecent = [[NSMenuItem alloc] initWithTitle:@"Open Recent…"
                                                        action:@selector(openRecent:)
                                                 keyEquivalent:@"r"];
    openRecent.keyEquivalentModifierMask = NSEventModifierFlagControl;
    [fileMenu addItem:openRecent];
    [fileMenu addItemWithTitle:@"Save"
                        action:@selector(saveCurrentFile:)
                 keyEquivalent:@"s"];
    // VS Code's Save As key, Shift+Cmd+S, is Export PDF here.
    [fileMenu addItemWithTitle:@"Save As…"
                        action:@selector(saveFileAs:) keyEquivalent:@""];
    NSMenuItem *saveAll = [[NSMenuItem alloc] initWithTitle:@"Save All"
                                                     action:@selector(saveAllFiles:)
                                              keyEquivalent:@"s"];
    saveAll.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [fileMenu addItem:saveAll];
    NSMenuItem *exportPDF =
        [[NSMenuItem alloc] initWithTitle:@"Export PDF…"
                                   action:@selector(exportPDF:) keyEquivalent:@"s"];
    exportPDF.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [fileMenu addItem:exportPDF];
    [fileMenu addItem:[NSMenuItem separatorItem]];

    // The file tree's commands, on its clicked or selected row (else the
    // open folder), as in its right click menu.
    NSMenuItem *newFolder =
        [[NSMenuItem alloc] initWithTitle:@"New Folder…"
                                   action:@selector(newFolder:) keyEquivalent:@"n"];
    newFolder.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [fileMenu addItem:newFolder];
    [fileMenu addItemWithTitle:@"Rename…"
                        action:@selector(renameSelected:) keyEquivalent:@""];
    // No key equivalent: a menu key works from every pane, and Cmd+Delete
    // in the editor or a text field deletes to the start of the line. The
    // file tree takes Cmd+Delete itself (ClickOutline keyDown:).
    [fileMenu addItemWithTitle:@"Move to Trash"
                        action:@selector(deleteSelected:) keyEquivalent:@""];
    [fileMenu addItemWithTitle:@"Reveal in Finder"
                        action:@selector(revealInFinder:) keyEquivalent:@""];
    [fileMenu addItemWithTitle:@"Copy Path"
                        action:@selector(copyPath:) keyEquivalent:@""];
    [fileMenu addItemWithTitle:@"Copy Relative Path"
                        action:@selector(copyRelativePath:) keyEquivalent:@""];
    [fileMenu addItemWithTitle:@"Refresh File Tree"
                        action:@selector(refreshTree:) keyEquivalent:@"r"];
    [fileMenu addItem:[NSMenuItem separatorItem]];

    // A tab's right click menu, for the tab on show (VS Code's names).
    [fileMenu addItemWithTitle:@"Copy Path of Active File"
                        action:@selector(copyActiveFilePath:) keyEquivalent:@""];
    [fileMenu addItemWithTitle:@"Copy Relative Path of Active File"
                        action:@selector(copyActiveFileRelativePath:) keyEquivalent:@""];
    [fileMenu addItemWithTitle:@"Reveal Active File in Finder"
                        action:@selector(revealActiveFileInFinder:) keyEquivalent:@""];
    [fileMenu addItemWithTitle:@"Reveal Active File in File Tree"
                        action:@selector(revealActiveFileInTree:) keyEquivalent:@""];
    [fileMenu addItem:[NSMenuItem separatorItem]];

    // Cmd+W closes what has the keyboard: the tab on show, or the terminal
    // or browser panel. The title says which (validateMenuItem:).
    [fileMenu addItemWithTitle:@"Close Tab"
                        action:@selector(closeFocused:)
                 keyEquivalent:@"w"];
    // VS Code's Shift+Cmd+T is Toggle Terminal here, so Option joins it.
    NSMenuItem *reopen =
        [[NSMenuItem alloc] initWithTitle:@"Reopen Closed Tab"
                                   action:@selector(reopenClosedTab:) keyEquivalent:@"t"];
    reopen.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift | NSEventModifierFlagOption;
    [fileMenu addItem:reopen];
    NSMenuItem *closeOthers = [[NSMenuItem alloc] initWithTitle:@"Close Other Tabs"
                                                         action:@selector(closeOtherTabs:)
                                                  keyEquivalent:@"t"];
    closeOthers.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [fileMenu addItem:closeOthers];
    [fileMenu addItemWithTitle:@"Close Tabs to the Right"
                        action:@selector(closeTabsToTheRight:) keyEquivalent:@""];
    [fileMenu addItemWithTitle:@"Close All Tabs"
                        action:@selector(closeAllTabs:) keyEquivalent:@""];

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
    // VS Code's multi-cursor keys on the Mac, answered by the editor's text
    // view (CodeTextView) through the responder chain, so they are enabled
    // only while it has the keyboard.
    [editMenu addItemWithTitle:@"Add Next Occurrence"
                        action:@selector(addNextOccurrence:) keyEquivalent:@"d"];
    NSMenuItem *allOccurrences =
        [[NSMenuItem alloc] initWithTitle:@"Select All Occurrences"
                                   action:@selector(selectAllOccurrences:) keyEquivalent:@"l"];
    allOccurrences.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [editMenu addItem:allOccurrences];
    NSMenuItem *cursorAbove =
        [[NSMenuItem alloc] initWithTitle:@"Add Cursor Above"
                                   action:@selector(addCursorAbove:)
                            keyEquivalent:[NSString stringWithFormat:@"%C",
                                              (unichar)NSUpArrowFunctionKey]];
    cursorAbove.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [editMenu addItem:cursorAbove];
    NSMenuItem *cursorBelow =
        [[NSMenuItem alloc] initWithTitle:@"Add Cursor Below"
                                   action:@selector(addCursorBelow:)
                            keyEquivalent:[NSString stringWithFormat:@"%C",
                                              (unichar)NSDownArrowFunctionKey]];
    cursorBelow.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [editMenu addItem:cursorBelow];
    // VS Code's Cmd+L, and its Shift+Cmd+K with Control for Command, since
    // Shift+Cmd+K is Request a Change. Enabled while the editor's text view
    // has the keyboard, as the items above.
    [editMenu addItemWithTitle:@"Select Line"
                        action:@selector(expandLineSelection:) keyEquivalent:@"l"];
    NSMenuItem *deleteLine =
        [[NSMenuItem alloc] initWithTitle:@"Delete Line"
                                   action:@selector(deleteLine:) keyEquivalent:@"k"];
    deleteLine.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagShift;
    [editMenu addItem:deleteLine];
    // VS Code's Indent Line and Outdent Line, and Insert Line Below (its
    // Insert Line Above, Shift+Cmd+Return, is Run File here).
    [editMenu addItemWithTitle:@"Indent Lines" action:@selector(indentLines:) keyEquivalent:@"]"];
    [editMenu addItemWithTitle:@"Outdent Lines" action:@selector(outdentLines:) keyEquivalent:@"["];
    [editMenu addItemWithTitle:@"Insert Line Below"
                        action:@selector(insertLineBelow:) keyEquivalent:@"\r"];
    [editMenu addItemWithTitle:@"Insert Line Above"
                        action:@selector(insertLineAbove:) keyEquivalent:@""];
    [editMenu addItem:[NSMenuItem separatorItem]];
    // VS Code's inline chat key. It works from every pane, and every window:
    // what has the keyboard decides what the agent does (AgentPanel.h).
    [editMenu addItemWithTitle:@"Inline Agent…"
                        action:@selector(inlineAgent:) keyEquivalent:@"i"];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItemWithTitle:@"Toggle Comment"
                        action:@selector(toggleComment:) keyEquivalent:@"/"];
    // Option+Cmd+L: Cmd+L went to Select Line, VS Code's.
    NSMenuItem *task = [[NSMenuItem alloc] initWithTitle:@"Toggle Task"
                                                  action:@selector(toggleTask:)
                                           keyEquivalent:@"l"];
    task.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [editMenu addItem:task];
    // In Markdown. No keys: markdown-all-in-one's Cmd+B and Cmd+I are the
    // sidebar and the inline agent here, as in VS Code itself.
    [editMenu addItemWithTitle:@"Toggle Bold" action:@selector(toggleBold:) keyEquivalent:@""];
    [editMenu addItemWithTitle:@"Toggle Italic" action:@selector(toggleItalic:) keyEquivalent:@""];
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
    // Option+Cmd+I: Cmd+I is VS Code's inline chat key, and the agent's here.
    NSMenuItem *hover = [[NSMenuItem alloc] initWithTitle:@"Show Hover Info"
                                                   action:@selector(showHoverInfo:)
                                            keyEquivalent:@"i"];
    hover.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [editMenu addItem:hover];
    // VS Code's keys: F2 (fn+F2 on a laptop) renames the name at the cursor,
    // or in the file tree the selected file; Cmd+. lists the quick fixes.
    // Both are also caught by the key monitor above, so the editor gets
    // them first and Cmd+. stays cancel everywhere else.
    NSMenuItem *renameSymbol =
        [[NSMenuItem alloc] initWithTitle:@"Rename Symbol…"
                                   action:@selector(renameSymbol:)
                            keyEquivalent:[NSString stringWithFormat:@"%C",
                                              (unichar)NSF2FunctionKey]];
    renameSymbol.keyEquivalentModifierMask = 0;
    [editMenu addItem:renameSymbol];
    [editMenu addItemWithTitle:@"Quick Fix…"
                        action:@selector(showQuickFixes:) keyEquivalent:@"."];
    // VS Code's key. Like Format Document it has no Command, so it is also
    // a character (Ø on a US keyboard), which the key monitor above lets it
    // type outside the editor.
    NSMenuItem *organize = [[NSMenuItem alloc] initWithTitle:@"Organize Imports"
                                                      action:@selector(organizeImports:)
                                               keyEquivalent:@"o"];
    organize.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagShift;
    [editMenu addItem:organize];
    [editMenu addItem:[NSMenuItem separatorItem]];

    // Find and replace. The items send performTextFinderAction: with the
    // NSTextFinderAction value as the tag, which the editor (its find bar,
    // EditorFind.h), a terminal (find, next and previous) and the diff view
    // (NSTextFinder) each answer. VS Code's keys, except Replace's
    // Shift+Cmd+1, which is Go to Tab 1 here and Replace only while the
    // find bar has the keyboard (the bar takes it first).
    NSMenuItem *find = [editMenu addItemWithTitle:@"Find…"
                                           action:@selector(performTextFinderAction:)
                                    keyEquivalent:@"f"];
    find.tag = NSTextFinderActionShowFindInterface;
    NSMenuItem *replace = [editMenu addItemWithTitle:@"Replace…"
                                              action:@selector(performTextFinderAction:)
                                       keyEquivalent:@"f"];
    replace.tag = NSTextFinderActionShowReplaceInterface;
    replace.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    NSMenuItem *findNext = [editMenu addItemWithTitle:@"Find Next"
                                               action:@selector(performTextFinderAction:)
                                        keyEquivalent:@"g"];
    findNext.tag = NSTextFinderActionNextMatch;
    NSMenuItem *findPrev = [editMenu addItemWithTitle:@"Find Previous"
                                               action:@selector(performTextFinderAction:)
                                        keyEquivalent:@"g"];
    findPrev.tag = NSTextFinderActionPreviousMatch;
    findPrev.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    NSMenuItem *useSelection = [editMenu addItemWithTitle:@"Use Selection for Find"
                                                   action:@selector(performTextFinderAction:)
                                            keyEquivalent:@"e"];
    useSelection.tag = NSTextFinderActionSetSearchString;
    NSMenuItem *replaceOne = [editMenu addItemWithTitle:@"Replace"
                                                 action:@selector(performTextFinderAction:)
                                          keyEquivalent:@""];
    replaceOne.tag = NSTextFinderActionReplaceAndFind;
    NSMenuItem *replaceAll = [editMenu addItemWithTitle:@"Replace All"
                                                 action:@selector(performTextFinderAction:)
                                          keyEquivalent:@"\r"];
    replaceAll.tag = NSTextFinderActionReplaceAll;
    replaceAll.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    // The editor's find bar and Find in Folder each answer these for
    // themselves (EditorController and SearchPanel, as window delegates).
    NSMenu *findOptions = [[NSMenu alloc] initWithTitle:@"Find Options"];
    // Find in Selection has no key here: VS Code's Option+Cmd+L is Toggle
    // Task's, so the find bar takes it only while one of its fields has the
    // keyboard.
    NSString *const optionTitles[] = {@"Match Case", @"Match Whole Word", @"Use Regular Expression",
                                      @"Preserve Case", @"Find in Selection"};
    NSString *const optionKeys[] = {@"c", @"w", @"r", @"p", @""};
    SEL const optionActions[] = {@selector(toggleFindMatchCase:), @selector(toggleFindWholeWord:),
                                 @selector(toggleFindRegex:), @selector(toggleFindPreserveCase:),
                                 @selector(toggleFindInSelection:)};
    for (int i = 0; i < 5; i++) {
        NSMenuItem *it = [findOptions addItemWithTitle:optionTitles[i] action:optionActions[i]
                                         keyEquivalent:optionKeys[i]];
        if (optionKeys[i].length)
            it.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    }
    NSMenuItem *findOptionsItem = [[NSMenuItem alloc] initWithTitle:@"Find Options"
                                                              action:nil keyEquivalent:@""];
    findOptionsItem.submenu = findOptions;
    [editMenu addItem:findOptionsItem];
    NSMenuItem *findInFolder =
        [[NSMenuItem alloc] initWithTitle:@"Find in Folder…"
                                   action:@selector(openSearch:) keyEquivalent:@"f"];
    findInFolder.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [editMenu addItem:findInFolder];
    // VS Code's Replace in Files is Shift+Cmd+H, which is the hints here.
    NSMenuItem *replaceInFolder =
        [[NSMenuItem alloc] initWithTitle:@"Replace in Folder…"
                                   action:@selector(openReplaceInFolder:) keyEquivalent:@"f"];
    replaceInFolder.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift | NSEventModifierFlagOption;
    [editMenu addItem:replaceInFolder];
    // Shift+Cmd+D ("to do"): Shift+Cmd+L is VS Code's Select All
    // Occurrences, and VS Code's Shift+Cmd+D opens its debugger, which
    // minicode has no counterpart for.
    NSMenuItem *todos =
        [[NSMenuItem alloc] initWithTitle:@"TODOs in Folder…"
                                   action:@selector(showTodos:) keyEquivalent:@"d"];
    todos.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [editMenu addItem:todos];
    editItem.submenu = editMenu;

    // View menu
    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    [menubar addItem:viewItem];
    NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    // Shift+Cmd+P as in VS Code: every menu command by name (PalettePanel.h).
    NSMenuItem *palette =
        [[NSMenuItem alloc] initWithTitle:@"Command Palette…"
                                   action:@selector(showCommandPalette:)
                            keyEquivalent:@"p"];
    palette.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [viewMenu addItem:palette];
    // Shift+Cmd+V, VS Code's Markdown preview key (Shift+Cmd+P until 1.5.4).
    NSMenuItem *toggle =
        [[NSMenuItem alloc] initWithTitle:@"Toggle Preview"
                                   action:@selector(togglePreview:)
                            keyEquivalent:@"v"];
    toggle.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [viewMenu addItem:toggle];
    // Control+Command+V, for VS Code's Cmd+K V chord: the page beside the
    // source of a Markdown file, which keeps the keyboard.
    NSMenuItem *side =
        [[NSMenuItem alloc] initWithTitle:@"Open Preview to the Side"
                                   action:@selector(toggleSidePreview:)
                            keyEquivalent:@"v"];
    side.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagControl;
    [viewMenu addItem:side];

    NSMenuItem *hints =
        [[NSMenuItem alloc] initWithTitle:@"Toggle Shortcut Hints"
                                   action:@selector(toggleHints:)
                            keyEquivalent:@"h"];
    hints.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [viewMenu addItem:hints];

    // The editor's font, VS Code's keys: Cmd+= (and Shift+Cmd+=, Cmd++),
    // Cmd+-, and reset on Option+Cmd+0, since Cmd+0 focuses the file tree
    // as VS Code's focuses its side bar (Cmd+keypad 0, VS Code's reset, is
    // caught by the key monitor above). Answered by NSApp (EditorFont.h).
    NSMenuItem *zoomIn = [[NSMenuItem alloc] initWithTitle:@"Zoom In"
                                                    action:@selector(editorZoomIn:)
                                             keyEquivalent:@"="];
    zoomIn.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    [viewMenu addItem:zoomIn];
    NSMenuItem *zoomIn2 = [[NSMenuItem alloc] initWithTitle:@"Zoom In"
                                                     action:@selector(editorZoomIn:)
                                              keyEquivalent:@"="];
    zoomIn2.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    zoomIn2.hidden = YES;
    zoomIn2.allowsKeyEquivalentWhenHidden = YES;
    [viewMenu addItem:zoomIn2];
    NSMenuItem *zoomOut = [[NSMenuItem alloc] initWithTitle:@"Zoom Out"
                                                     action:@selector(editorZoomOut:)
                                              keyEquivalent:@"-"];
    zoomOut.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    [viewMenu addItem:zoomOut];
    NSMenuItem *zoomReset = [[NSMenuItem alloc] initWithTitle:@"Reset Zoom"
                                                       action:@selector(editorZoomReset:)
                                                keyEquivalent:@"0"];
    zoomReset.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [viewMenu addItem:zoomReset];

    // The browser's page or a PDF (EditorController+ZoomFit.mm). Cmd+=,
    // Cmd+- and Cmd+0 do these while one has the keyboard or the pointer;
    // the keys stay off the items, which would take them from the editor.
    NSMenuItem *pageZoomItem = [[NSMenuItem alloc] initWithTitle:@"Page Zoom" action:nil
                                                   keyEquivalent:@""];
    NSMenu *pageZoomMenu = [[NSMenu alloc] initWithTitle:@"Page Zoom"];
    [pageZoomMenu addItemWithTitle:@"Zoom In" action:@selector(zoomPageIn:) keyEquivalent:@""];
    [pageZoomMenu addItemWithTitle:@"Zoom Out" action:@selector(zoomPageOut:) keyEquivalent:@""];
    [pageZoomMenu addItemWithTitle:@"Actual Size" action:@selector(zoomPageToActualSize:)
                     keyEquivalent:@""];
    [pageZoomMenu addItem:[NSMenuItem separatorItem]];
    [pageZoomMenu addItemWithTitle:@"Zoom to Fit Width" action:@selector(zoomPageToFitWidth:)
                     keyEquivalent:@""];
    [pageZoomMenu addItemWithTitle:@"Zoom to Fit Page" action:@selector(zoomPageToFit:)
                     keyEquivalent:@""];
    pageZoomItem.submenu = pageZoomMenu;
    [viewMenu addItem:pageZoomItem];

    // Option+Z, VS Code's: long lines wrap or run on (editor.word-wrap).
    NSMenuItem *wrap = [[NSMenuItem alloc] initWithTitle:@"Word Wrap"
                                                  action:@selector(toggleWordWrap:)
                                           keyEquivalent:@"z"];
    wrap.keyEquivalentModifierMask = NSEventModifierFlagOption;
    [viewMenu addItem:wrap];

    // Folding (FoldView.h). VS Code's Option+Cmd+[ and ] are minicode's
    // previous and next terminal, and its other folding keys are Cmd+K
    // chords, which minicode has none of; so Control+Command with the
    // chord's second key: [ fold, ] unfold, L toggle, 0 fold all, J unfold
    // all, / fold comments, 1 to 7 a level. Answered by the editor's text
    // view while it has the keyboard.
    NSMenuItem *foldingItem = [[NSMenuItem alloc] initWithTitle:@"Folding" action:nil
                                                  keyEquivalent:@""];
    NSMenu *foldingMenu = [[NSMenu alloc] initWithTitle:@"Folding"];
    const NSEventModifierFlags ctrlCmd = NSEventModifierFlagControl | NSEventModifierFlagCommand;
    NSMenuItem *(^addFold)(NSString *, SEL, NSString *) =
        ^NSMenuItem *(NSString *title, SEL action, NSString *key) {
            NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:title action:action
                                                 keyEquivalent:key];
            it.keyEquivalentModifierMask = key.length ? ctrlCmd : 0;
            [foldingMenu addItem:it];
            return it;
        };
    addFold(@"Fold", @selector(foldRegion:), @"[");
    addFold(@"Unfold", @selector(unfoldRegion:), @"]");
    addFold(@"Toggle Fold", @selector(toggleFold:), @"l");
    addFold(@"Fold Recursively", @selector(foldRecursively:), @"");
    addFold(@"Unfold Recursively", @selector(unfoldRecursively:), @"");
    [foldingMenu addItem:[NSMenuItem separatorItem]];
    addFold(@"Fold All", @selector(foldAll:), @"0");
    addFold(@"Unfold All", @selector(unfoldAll:), @"j");
    addFold(@"Fold All Comments", @selector(foldAllComments:), @"/");
    for (NSInteger level = 1; level <= 7; level++) {
        NSMenuItem *it = addFold([NSString stringWithFormat:@"Fold Level %ld", (long)level],
                                 @selector(foldLevel:),
                                 [NSString stringWithFormat:@"%ld", (long)level]);
        it.tag = level;
    }
    foldingItem.submenu = foldingMenu;
    [viewMenu addItem:foldingItem];

    [viewMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *sidebar =
        [[NSMenuItem alloc] initWithTitle:@"Toggle Sidebar"
                                   action:@selector(toggleSidebarVisibility:)
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
    // Pushing and pulling (git.fetch, git.pull, git.push, git.sync), staging
    // everything, discarding and switching branches, with no keys, as in VS
    // Code; the panel's button and menus run them too.
    NSMenuItem *gitItem = [[NSMenuItem alloc] initWithTitle:@"Source Control" action:nil
                                              keyEquivalent:@""];
    NSMenu *gitMenu = [[NSMenu alloc] initWithTitle:@"Source Control"];
    [gitMenu addItemWithTitle:@"Fetch" action:@selector(gitFetch:) keyEquivalent:@""];
    [gitMenu addItemWithTitle:@"Pull" action:@selector(gitPull:) keyEquivalent:@""];
    [gitMenu addItemWithTitle:@"Push" action:@selector(gitPush:) keyEquivalent:@""];
    [gitMenu addItemWithTitle:@"Sync" action:@selector(gitSync:) keyEquivalent:@""];
    [gitMenu addItem:[NSMenuItem separatorItem]];
    [gitMenu addItemWithTitle:@"Stage All Changes" action:@selector(gitStageAll:)
                keyEquivalent:@""];
    [gitMenu addItemWithTitle:@"Unstage All Changes" action:@selector(gitUnstageAll:)
                keyEquivalent:@""];
    [gitMenu addItemWithTitle:@"Discard Changes…" action:@selector(gitClean:) keyEquivalent:@""];
    [gitMenu addItemWithTitle:@"Undo Discard" action:@selector(gitUndoDiscard:)
                keyEquivalent:@""];
    [gitMenu addItem:[NSMenuItem separatorItem]];
    [gitMenu addItemWithTitle:@"Switch Branch…" action:@selector(gitCheckout:) keyEquivalent:@""];

    gitItem.submenu = gitMenu;
    [viewMenu addItem:gitItem];

    // Shift+Cmd+A: the agent sessions that worked in the folder, in the
    // sidebar's place (AgentsPanel.h).
    NSMenuItem *agents =
        [[NSMenuItem alloc] initWithTitle:@"Toggle Agents"
                                   action:@selector(toggleAgents:)
                            keyEquivalent:@"a"];
    agents.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [viewMenu addItem:agents];

    // What this window's terminals started and what runs in its folder, in
    // the sidebar's place (ProcessesPanel.h). No key by default: keys.conf
    // can give it one (minicode.toggleProcesses).
    [viewMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Toggle Processes"
                                                 action:@selector(toggleProcesses:)
                                          keyEquivalent:@""]];
    // Claude Code's /ide for each window (agent.ide-link; IdeLinkServer.h).
    [viewMenu addItem:MCIdeLinkMenuItem()];

    // Shift+Cmd+M as in VS Code: every problem the language servers report,
    // above the terminal.
    NSMenuItem *problems =
        [[NSMenuItem alloc] initWithTitle:@"Problems"
                                   action:@selector(toggleProblems:)
                            keyEquivalent:@"m"];
    problems.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [viewMenu addItem:problems];

    NSMenuItem *editorPane =
        [[NSMenuItem alloc] initWithTitle:@"Toggle Editor"
                                   action:@selector(toggleEditor:)
                            keyEquivalent:@"e"];
    editorPane.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [viewMenu addItem:editorPane];

    // The pane with the keyboard takes the whole right area, and back.
    NSMenuItem *maximize = [[NSMenuItem alloc] initWithTitle:@"Maximize Pane"
                                                      action:@selector(toggleMaximizedPane:)
                                               keyEquivalent:@"m"];
    maximize.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    [viewMenu addItem:maximize];

    NSMenuItem *hidden =
        [[NSMenuItem alloc] initWithTitle:@"Show Hidden Files"
                                   action:@selector(toggleHiddenFiles:)
                            keyEquivalent:@"."];
    hidden.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [viewMenu addItem:hidden];

    // Columns at the right of the file tree's rows, each on its own key:
    // sizes, when last saved (which also shows the open file's last save in
    // the status bar) and when created. Cmd+I alone is the inline agent,
    // and Option+Cmd+I Show Hover Info.
    for (NSMenuItem *it in MCTreeColumnMenuItems()) [viewMenu addItem:it];
    // The tree's order, as its right click menu's Sort By has it.
    NSMenuItem *sortTree = [[NSMenuItem alloc] initWithTitle:@"Sort File Tree By"
                                                      action:nil keyEquivalent:@""];
    sortTree.submenu = [EditorController treeSortMenu];
    [viewMenu addItem:sortTree];

    // Terminal: Shift+Cmd+T in the menu, like the other pane toggles; Ctrl+`
    // also works, caught by the local key monitor above.
    NSMenuItem *term =
        [[NSMenuItem alloc] initWithTitle:@"Toggle Terminal"
                                   action:@selector(toggleTerminal:)
                            keyEquivalent:@"t"];
    term.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [viewMenu addItem:term];
    // Cmd+J too, VS Code's Toggle Panel (hidden: one item, two keys).
    NSMenuItem *term2 =
        [[NSMenuItem alloc] initWithTitle:@"Toggle Terminal"
                                   action:@selector(toggleTerminal:)
                            keyEquivalent:@"j"];
    term2.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    term2.hidden = YES;
    term2.allowsKeyEquivalentWhenHidden = YES;
    [viewMenu addItem:term2];

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
    // The right click's menu from the keyboard, for what has the keyboard:
    // Shift+F10 as on Windows and Linux, and Ctrl+Return (hidden), which a
    // text view would otherwise take for a Unicode line separator.
    NSMenuItem *contextMenu =
        [[NSMenuItem alloc] initWithTitle:@"Show Context Menu"
                                   action:@selector(showContextMenu:)
                            keyEquivalent:[NSString stringWithFormat:@"%C",
                                              (unichar)NSF10FunctionKey]];
    contextMenu.keyEquivalentModifierMask = NSEventModifierFlagShift;
    [viewMenu addItem:contextMenu];
    NSMenuItem *contextMenu2 =
        [[NSMenuItem alloc] initWithTitle:@"Show Context Menu"
                                   action:@selector(showContextMenu:)
                            keyEquivalent:@"\r"];
    contextMenu2.keyEquivalentModifierMask = NSEventModifierFlagControl;
    contextMenu2.hidden = YES;
    contextMenu2.allowsKeyEquivalentWhenHidden = YES;
    [viewMenu addItem:contextMenu2];
    // The same as a right click's Adjust Settings, for each part of the window.
    NSMenuItem *adjust = [[NSMenuItem alloc] initWithTitle:@"Adjust Settings"
                                                    action:nil keyEquivalent:@""];
    adjust.submenu = MCAdjustSettingsSubmenu();
    [viewMenu addItem:adjust];
    // VS Code's built-in color themes; the choice is the settings file's
    // theme line, so it stays one plain line there.
    NSMenuItem *themes = [[NSMenuItem alloc] initWithTitle:@"Theme" action:nil keyEquivalent:@""];
    themes.submenu = MCThemeSubmenu();
    [viewMenu addItem:themes];

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
    // A terminal whose Claude Code waits for an answer (TerminalAttention.h),
    // in any window.
    NSMenuItem *nextWaiting =
        [[NSMenuItem alloc] initWithTitle:@"Next Waiting Terminal"
                                   action:@selector(nextWaitingTerminal:)
                            keyEquivalent:@"w"];
    nextWaiting.keyEquivalentModifierMask =
        NSEventModifierFlagControl | NSEventModifierFlagCommand;
    [termMenu addItem:nextWaiting];
    // Cmd+K as in Terminal.app and VS Code. Answered by the terminal with
    // the keyboard (TerminalView clearTerminal:), so elsewhere it is off.
    [termMenu addItemWithTitle:@"Clear Terminal"
                        action:@selector(clearTerminal:)
                 keyEquivalent:@"k"];
    // What the last command printed, from the shell integration's marks
    // (TerminalView copyLastCommandOutput:); no key, as in VS Code.
    [termMenu addItemWithTitle:@"Copy Last Command Output"
                        action:@selector(copyLastCommandOutput:)
                 keyEquivalent:@""];
    [termMenu addItem:[NSMenuItem separatorItem]];
    // Not Cmd+Return, which commits in the git panel's message box.
    NSMenuItem *runFile =
        [[NSMenuItem alloc] initWithTitle:@"Run File"
                                   action:@selector(runCurrentFile:)
                            keyEquivalent:@"\r"];
    runFile.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [termMenu addItem:runFile];
    // Shift+Return in a Python file, which the key monitor runs (Keymap.h):
    // a menu key without Command would take Shift+Return from every field.
    [termMenu addItemWithTitle:@"Run Selection or Line in Python REPL"
                        action:@selector(runSelectionInTerminal:) keyEquivalent:@""];
    termItem.submenu = termMenu;

    // Navigate menu — keyboard-driven movement.
    NSMenuItem *navItem = [[NSMenuItem alloc] init];
    [menubar addItem:navItem];
    NSMenu *navMenu = [[NSMenu alloc] initWithTitle:@"Navigate"];
    // VS Code's keys: Cmd+P lists the folder's files, Ctrl+G asks for a
    // line (the key monitor leaves Ctrl+G to a program on the terminal grid).
    [navMenu addItemWithTitle:@"Go to File…"
                       action:@selector(quickOpen:)
                keyEquivalent:@"p"];
    NSMenuItem *goLine = [[NSMenuItem alloc] initWithTitle:@"Go to Line…"
                                                    action:@selector(goToLinePrompt:)
                                             keyEquivalent:@"g"];
    goLine.keyEquivalentModifierMask = NSEventModifierFlagControl;
    [navMenu addItem:goLine];
    // VS Code's keys: Shift+Cmd+O the file's symbols ("@" in the palette),
    // Cmd+T the workspace's ("#"), from the language servers or, for the
    // file, the built-in outline.
    NSMenuItem *fileSymbol = [[NSMenuItem alloc] initWithTitle:@"Go to Symbol in File…"
                                                        action:@selector(goToSymbolInFile:)
                                                 keyEquivalent:@"o"];
    fileSymbol.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [navMenu addItem:fileSymbol];
    [navMenu addItemWithTitle:@"Go to Symbol in Workspace…"
                       action:@selector(goToSymbolInWorkspace:)
                keyEquivalent:@"t"];
    // Ctrl+- and Ctrl+Shift+-, VS Code's Mac keys. A program on the
    // terminal's grid gets Ctrl+- instead (TerminalGridView claims Ctrl).
    NSMenuItem *back = [[NSMenuItem alloc] initWithTitle:@"Go Back"
                                                  action:@selector(navigateBack:)
                                           keyEquivalent:@"-"];
    back.keyEquivalentModifierMask = NSEventModifierFlagControl;
    [navMenu addItem:back];
    NSMenuItem *forward = [[NSMenuItem alloc] initWithTitle:@"Go Forward"
                                                     action:@selector(navigateForward:)
                                              keyEquivalent:@"-"];
    forward.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagShift;
    [navMenu addItem:forward];
    // VS Code's Go to Bracket. Shift+Cmd+\ (Cmd+\ splits the terminal).
    NSMenuItem *goBracket = [[NSMenuItem alloc] initWithTitle:@"Go to Bracket"
                                                       action:@selector(goToBracket:)
                                                keyEquivalent:@"\\"];
    goBracket.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [navMenu addItem:goBracket];
    [navMenu addItem:[NSMenuItem separatorItem]];
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
    NSMenuItem *references =
        [[NSMenuItem alloc] initWithTitle:@"Find All References"
                                   action:@selector(findReferences:)
                            keyEquivalent:[NSString stringWithFormat:@"%C",
                                              (unichar)NSF12FunctionKey]];
    references.keyEquivalentModifierMask = NSEventModifierFlagShift;
    [navMenu addItem:references];
    // F8 and Shift+F8 as in VS Code: the next or previous problem in the
    // file, its message shown at the cursor. Caught by the key monitor too.
    NSString *f8 = [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];
    NSMenuItem *nextProblem = [[NSMenuItem alloc] initWithTitle:@"Next Problem"
                                                         action:@selector(nextProblem:)
                                                  keyEquivalent:f8];
    nextProblem.keyEquivalentModifierMask = 0;
    [navMenu addItem:nextProblem];
    NSMenuItem *previousProblem = [[NSMenuItem alloc] initWithTitle:@"Previous Problem"
                                                             action:@selector(previousProblem:)
                                                      keyEquivalent:f8];
    previousProblem.keyEquivalentModifierMask = NSEventModifierFlagShift;
    [navMenu addItem:previousProblem];
    // F7 and Shift+F7 as in VS Code's diff editor: the compare view's next
    // or previous run of changes, wrapping. The compare view answers them.
    NSString *f7 = [NSString stringWithFormat:@"%C", (unichar)NSF7FunctionKey];
    NSMenuItem *nextDiff = [[NSMenuItem alloc] initWithTitle:@"Next Difference"
                                                      action:@selector(goToNextDifference:)
                                               keyEquivalent:f7];
    nextDiff.keyEquivalentModifierMask = 0;
    [navMenu addItem:nextDiff];
    NSMenuItem *prevDiff = [[NSMenuItem alloc] initWithTitle:@"Previous Difference"
                                                      action:@selector(goToPreviousDifference:)
                                               keyEquivalent:f7];
    prevDiff.keyEquivalentModifierMask = NSEventModifierFlagShift;
    [navMenu addItem:prevDiff];
    // An agent's tour (`minicode tour`): enabled only while one is on, and
    // the key monitor takes the keys only then, so Option+] still types ‘.
    [navMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *nextStop = [[NSMenuItem alloc] initWithTitle:@"Next Tour Stop"
                                                      action:@selector(nextTourStop:)
                                               keyEquivalent:@"]"];
    nextStop.keyEquivalentModifierMask = NSEventModifierFlagOption;
    [navMenu addItem:nextStop];
    NSMenuItem *prevStop = [[NSMenuItem alloc] initWithTitle:@"Previous Tour Stop"
                                                      action:@selector(previousTourStop:)
                                               keyEquivalent:@"["];
    prevStop.keyEquivalentModifierMask = NSEventModifierFlagOption;
    [navMenu addItem:prevStop];
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
        MCKeymapInstall();   // the keys in effect, from keys.conf (Keymap.h)
        [app run];
    }
    return 0;
}
