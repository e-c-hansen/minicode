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

@interface AppDelegate : NSObject <NSApplicationDelegate, MCRestarting>
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

    // Ctrl+` (toggle terminal) is the one shortcut that isn't a clean menu key
    // equivalent, so a local key monitor handles it regardless of focus. The
    // monitor object is retained so it stays installed. Everything else is a
    // normal menu shortcut, which works in every pane (like Cmd+C does).
    self.keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                          handler:^NSEvent *(NSEvent *e) {
        // Cmd+. is Quick Fix in the editor and cancel everywhere else (a
        // find field, the terminal, a sheet). In the editor the menu gets
        // it; elsewhere the event goes where it went before the menu had
        // the key: the window's own key equivalents (a sheet's Cancel),
        // then the first responder, whose key bindings make Cmd+. the
        // cancelOperation: that Esc also sends.
        if (MCIsCommandPeriod(e) && e.window && !NSApp.modalWindow) {
            NSResponder *fr = e.window.firstResponder;
            if ([self controllerForWindow:e.window] && [fr isKindOfClass:[CodeTextView class]] &&
                ((NSTextView *)fr).editable) {
                if ([NSApp.mainMenu performKeyEquivalent:e]) return nil;
                return e;
            }
            if (![e.window performKeyEquivalent:e]) [fr keyDown:e];
            return nil;
        }
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
        // The Menu (Application) key of a PC keyboard opens the context
        // menu of what has the keyboard, as Shift+F10 does.
        if (e.keyCode == 0x6E /* kVK_ContextualMenu */ && !cmd && !ctrl && !opt) {
            MCShowContextMenuInWindow(e.window);
            return nil;
        }
        // Shift+Option+F is Format Document only in the editor. Anywhere
        // else that takes typing (the terminal, the commit message, a find
        // field) it types its character, as it did before the menu had it.
        // In the editor the menu gets it first: AppKit offers a key without
        // Command or Control to the menu only after the first responder,
        // and the text view takes it as typing (Ï). F9 and Shift+F9 too.
        // Ctrl+G is Go to Line, except in a program on the terminal's grid
        // (nano, emacs and less use it), which gets it as typed.
        if (ctrl && !cmd && !opt && !shift && [ch isEqualToString:@"g"] &&
            [e.window.firstResponder isKindOfClass:NSClassFromString(@"TerminalGridView")]) {
            [e.window.firstResponder keyDown:e];
            return nil;
        }
        // Shift+Option+O (Organize Imports) the same way.
        BOOL formatKey = opt && shift && !cmd && !ctrl &&
                         ([ch isEqualToString:@"f"] || [ch isEqualToString:@"o"]);
        BOOL sortKey = e.keyCode == 101 /* kVK_F9 */ && !cmd && !ctrl && !opt;
        // F2 and Shift+F12 likewise (Rename Symbol, Find All References),
        // and F8 and Shift+F8 (Next and Previous Problem).
        BOOL lspKey = (e.keyCode == 120 /* kVK_F2 */ && !cmd && !ctrl && !opt && !shift) ||
                      (e.keyCode == 111 /* kVK_F12 */ && shift && !cmd && !ctrl && !opt) ||
                      (e.keyCode == 100 /* kVK_F8 */ && !cmd && !ctrl && !opt);
        if (formatKey || sortKey || lspKey) {
            NSResponder *fr = e.window.firstResponder;
            if ([fr isKindOfClass:[CodeTextView class]]) {
                if ([NSApp.mainMenu performKeyEquivalent:e]) return nil;
            } else if (formatKey &&
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
           a == @selector(nextProblem:) || a == @selector(previousProblem:);
}
// The File menu's tab commands and the tree's Sort By items, which the
// window decides (enabled, checked), forwarded when another window is key.
static BOOL MCWindowTabAction(SEL a) {
    return a == @selector(reopenClosedTab:) || a == @selector(closeOtherTabs:) ||
           a == @selector(closeTabsToTheRight:) || a == @selector(closeAllTabs:) ||
           a == @selector(copyActiveFilePath:) || a == @selector(copyActiveFileRelativePath:) ||
           a == @selector(revealActiveFileInFinder:) || a == @selector(revealActiveFileInTree:) ||
           a == @selector(sortTreeBy:) || a == @selector(setTreeSortDirection:) ||
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
- (void)togglePreview:(id)sender   { [[self current] togglePreview:sender]; }
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
    if (item.action == @selector(toggleProblems:)) {
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
    MCShowPalette(c.window, text, ctx);
}
- (void)showCommandPalette:(id)sender { (void)sender; [self showPaletteWithText:@">"]; }
- (void)quickOpen:(id)sender          { (void)sender; [self showPaletteWithText:@""]; }
- (void)goToLinePrompt:(id)sender     { (void)sender; [self showPaletteWithText:@":"]; }
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
    [fileMenu addItemWithTitle:@"Close Other Tabs"
                        action:@selector(closeOtherTabs:) keyEquivalent:@""];
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
    NSString *const optionTitles[] = {@"Match Case", @"Match Whole Word", @"Use Regular Expression"};
    NSString *const optionKeys[] = {@"c", @"w", @"r"};
    SEL const optionActions[] = {@selector(toggleFindMatchCase:), @selector(toggleFindWholeWord:),
                                 @selector(toggleFindRegex:)};
    for (int i = 0; i < 3; i++) {
        NSMenuItem *it = [findOptions addItemWithTitle:optionTitles[i] action:optionActions[i]
                                         keyEquivalent:optionKeys[i]];
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
    // Cmd+K as in Terminal.app and VS Code. Answered by the terminal with
    // the keyboard (TerminalView clearTerminal:), so elsewhere it is off.
    [termMenu addItemWithTitle:@"Clear Terminal"
                        action:@selector(clearTerminal:)
                 keyEquivalent:@"k"];
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
