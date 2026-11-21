// RemoteSshSuite.mm — `make guitest SUITES=remote_ssh`: a window over SSH
// (stage 8, docs/notes/session-host.md, "The Mac over SSH"), with a
// stand-in for ssh. MUTIBLE_SSH names a script that records its arguments,
// skips ssh's options to the "--", takes the host, and runs the remote
// command as ssh's remote shell would, with this checkout's mutible-cli as
// `mutible` and a variable of its own exported (so what the shell sees is
// the "remote" environment, not the Mac's). The folder it serves is a
// scratch folder of this Mac's, but the window reads, writes and stamps
// nothing of it itself: every read and write goes through the host.
//   - `ssh://standin/<folder>/notes.txt` through application:openURLs: (as
//     the launcher's request delivers it): one window, on a remote link,
//     its title "notes.txt — standin:<folder> — mutible", the file's tab
//     with its text, clean; ssh run with BatchMode, no pty, `--` before
//     the host, and `mutible host --stdio '<file>'` as the remote command.
//   - Typing and Cmd+S: the host writes the file (the tab clean, the text
//     on disk), and the window's own write never runs (a remote tab has no
//     stamp, and the window refuses to write one itself).
//   - The tree lists the folder through the host (folders first, what
//     .gitignore ignores marked; this Mac's MCTreeList never lists it), a
//     folder opens through it, files made on disk there come into the tree
//     by the host's notices, and a row opens its file through the host.
//   - Cmd+P lists the host's files (the ignored one left out) and opens one;
//     Find in Folder searches on the host, its rows open their files, and
//     Replace All says it does not work over SSH; New Folder says so too.
//   - `ssh://standin/<folder>/src/fresh.txt`, a file not there yet: an
//     empty untitled tab at that path, which Cmd+S makes through the host.
//   - A terminal: its shell is the host's child (`ps` of its parent says
//     mutible-cli) in the host's environment (the stand-in's variable).
//   - The connection drops (the stand-in's process group killed): the
//     window says it is disconnected (its title, Reconnect), the text stays
//     and takes more typing. Reconnect runs ssh again: the file is open on
//     the new host with everything typed, saved there with Cmd+S, and the
//     terminal has a new shell there that runs a command, and the tree
//     follows the new host's notices.
//   - The window closes, and the host it ran ends with it.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "EditorTab.h"
#import "FileTree.h"
#import "PalettePanel.h"
#import "RemoteWorkspace.h"
#import "Search.h"
#import "SessionLink.h"
#import "SessionWindow.h"
#import "Terminal.h"

#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <string>

namespace {

NSString *gFolder, *gFile, *gSsh, *gArgs, *gPidFile;
EditorController *gC;
TerminalView *gTerm;
pid_t gHostA, gHostB;

NSString *RealPath(NSString *path) {
    char buf[PATH_MAX];
    return realpath(path.fileSystemRepresentation, buf) ? @(buf) : path;
}

NSString *Read(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] ?: @"";
}

EditorController *RemoteWindow() {
    for (EditorController *c in MCGuiControllers())
        if (c.sessionIsRemote) return c;
    return nil;
}

MCTab *TabFor(EditorController *c, NSString *path) {
    for (MCTab *t in [c valueForKey:@"tabs"])
        if ([t.currentPath isEqualToString:path]) return t;
    return nil;
}

// The tree's rows: every row's item, and the names of those at the top
// level (dotfiles left out: an earlier suite may have shown them).
NSArray<FileItem *> *TreeRows(EditorController *c) {
    NSMutableArray<FileItem *> *rows = [NSMutableArray array];
    for (NSInteger r = 0; r < c.outline.numberOfRows; r++) {
        id item = [c.outline itemAtRow:r];
        if ([item isKindOfClass:FileItem.class]) [rows addObject:item];
    }
    return rows;
}
NSArray<NSString *> *TopNames(EditorController *c) {
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (FileItem *f in TreeRows(c))
        if ([c.outline levelForItem:f] == 0 && ![f.name hasPrefix:@"."]) [names addObject:f.name];
    return names;
}
FileItem *TreeRow(EditorController *c, NSString *path) {
    for (FileItem *f in TreeRows(c))
        if ([f.path isEqualToString:path]) return f;
    return nil;
}
NSView *Palette(EditorController *c) { return MCCommandPaletteIn(c.window); }
NSTextView *PaletteEditor(EditorController *c) { return (NSTextView *)MCCommandPaletteField(Palette(c)).currentEditor; }

void TypeAtEnd(EditorController *c, NSString *s) {
    NSTextView *tv = [c valueForKey:@"textView"];
    MCGuiUndoGroup(tv.undoManager, ^{ [tv insertText:s replacementRange:NSMakeRange(tv.string.length, 0)]; });
}

// The stand-in's pid, as it wrote it (the process group ssh's spawn made).
pid_t StandInPid() { return (pid_t)Read(gPidFile).intValue; }

BOOL Gone(pid_t pid) { return pid > 0 && kill(pid, 0) != 0 && errno == ESRCH; }

// The alerts shown since this suite began: MCGuiAlertsShown is every alert
// of the run so far, the suites before this one's included (CI, #314's
// first run: fourteen of theirs, none of this suite's).
NSUInteger gAlertsBefore;
NSArray<NSString *> *NewAlerts() {
    NSArray<NSString *> *all = MCGuiAlertsShown();
    return all.count > gAlertsBefore ? [all subarrayWithRange:NSMakeRange(gAlertsBefore, all.count - gAlertsBefore)]
                                     : @[];
}

// `command` typed into the terminal until `expect` shows on its screen,
// again every 3 s: a remote shell has no integration's marks to say its
// line editor is ready, and one starting may drop what came before it.
void RunUntil(NSString *what, NSString *command, NSString *expect) {
    __block double sent = 0;
    MCGuiWait(what, 30, ^BOOL {
        if ([[gTerm recentText:30] containsString:expect]) return YES;
        if (MCGuiNowMs() - sent > 3000) {
            sent = MCGuiNowMs();
            [gTerm runCommand:command];
        }
        return NO;
    });
}

}  // namespace

MC_GUI_SUITE(remote_ssh) {
    MCGuiStep(@"a folder for the remote, and a stand-in for ssh", ^{
        gAlertsBefore = MCGuiAlertsShown().count;
        // The session-host setting stays off: a window over SSH needs none.
        MCGuiSetSettings(@"lsp.enabled = false\n");
        NSFileManager *fm = NSFileManager.defaultManager;
        NSString *scratch = MCGuiScratch();
        NSString *made = [scratch stringByAppendingPathComponent:@"remote-proj"];
        [fm createDirectoryAtPath:made withIntermediateDirectories:YES attributes:nil error:nil];
        gFolder = RealPath(made);   // as the host names it
        gFile = [gFolder stringByAppendingPathComponent:@"notes.txt"];
        MCGuiWriteFile(gFile, @"first line\n");
        // What the tree, Quick Open and Find in Folder find there.
        [fm createDirectoryAtPath:[gFolder stringByAppendingPathComponent:@"src"]
            withIntermediateDirectories:YES attributes:nil error:nil];
        MCGuiWriteFile([gFolder stringByAppendingPathComponent:@"src/main.c"], @"int needle = 1;\n");
        MCGuiWriteFile([gFolder stringByAppendingPathComponent:@"src/util.c"], @"// a needle here\n");
        MCGuiWriteFile([gFolder stringByAppendingPathComponent:@".gitignore"], @"*.log\n");
        MCGuiWriteFile([gFolder stringByAppendingPathComponent:@"debug.log"], @"a needle in a log\n");
        NSString *bin = [scratch stringByAppendingPathComponent:@"ssh-bin"];
        [fm createDirectoryAtPath:bin withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *cli = [MCGuiBuildDir() stringByAppendingPathComponent:@"mutible-cli"];
        GCHECK([fm isExecutableFileAtPath:cli]);
        [fm createSymbolicLinkAtPath:[bin stringByAppendingPathComponent:@"mutible"] withDestinationPath:cli error:nil];
        gArgs = [scratch stringByAppendingPathComponent:@"ssh-args.txt"];
        gPidFile = [scratch stringByAppendingPathComponent:@"ssh-pid.txt"];
        gSsh = [scratch stringByAppendingPathComponent:@"ssh-stand-in"];
        NSString *script = [NSString stringWithFormat:
            @"#!/bin/sh\n"
            @"# A stand-in for ssh (RemoteSshSuite.mm): its arguments recorded,\n"
            @"# its options skipped to the --, then the host, then the remote\n"
            @"# command run as ssh's remote shell runs it.\n"
            @"printf '%%s\\n' \"$@\" > '%@'\n"
            @"while [ $# -gt 0 ] && [ \"$1\" != -- ]; do shift; done\n"
            @"shift\n"
            @"shift\n"
            @"echo $$ > '%@'\n"
            @"SSH_STANDIN=yes\n"
            @"export SSH_STANDIN\n"
            @"PATH='%@':$PATH\n"
            @"export PATH\n"
            @"exec /bin/sh -c \"$*\"\n",
            gArgs, gPidFile, bin];
        MCGuiWriteFile(gSsh, script);
        chmod(gSsh.fileSystemRepresentation, 0755);
        MCSetEnv("MUTIBLE_SSH", gSsh.fileSystemRepresentation);
    });
    MCGuiStep(@"ssh://standin/<folder>/notes.txt opens a window on the remote host", ^{
        NSURL *url = [NSURL URLWithString:[@"ssh://standin" stringByAppendingString:gFile]];
        GCHECK(url != nil);
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp openURLs:@[ url ]];
        gC = RemoteWindow();
        GCHECK(gC != nil);
        if (NewAlerts().count) MCGuiNote(@"alerts in this suite: %@ (%@)", NewAlerts(), MCGuiLastAlertInformative());
        if (!gC) return;
        gHostA = StandInPid();
        GCHECK(gHostA > 0);
        GCHECK([gC.rootPath isEqualToString:gFolder]);
        NSString *label = [@"standin:" stringByAppendingString:gFolder];
        GCHECK_EQ_STR(gC.sessionRemoteLabel, label);
        MCTab *t = TabFor(gC, gFile);
        GCHECK(t != nil && t.session != nil);
        GCHECK_EQ_STR(t.currentText, @"first line\n");
        GCHECK(!t.dirty);
        GCHECK(t.session.hostSavesFile);
        GCHECK(t.fileModDate == nil);   // no stamp of this Mac's file at that path
        GCHECK_EQ_STR(gC.window.title, ([NSString stringWithFormat:@"notes.txt — %@ — mutible", label]));
        // How ssh was run.
        NSArray<NSString *> *args = [Read(gArgs) componentsSeparatedByString:@"\n"];
        GCHECK([args containsObject:@"-T"] && [args containsObject:@"BatchMode=yes"]);
        const NSUInteger dash = [args indexOfObject:@"--"];
        GCHECK(dash != NSNotFound && dash + 2 < args.count);
        if (dash != NSNotFound && dash + 2 < args.count) {
            GCHECK_EQ_STR(args[dash + 1], @"standin");
            GCHECK_EQ_STR(args[dash + 2], ([NSString stringWithFormat:@"mutible host --stdio '%@'", gFile]));
        }
    });
    MCGuiStep(@"typing and Cmd+S: the host writes the file", ^{
        if (!gC) return;
        TypeAtEnd(gC, @"typed over ssh\n");
        GCHECK(TabFor(gC, gFile).dirty);
        [gC saveCurrentFile:nil];
    });
    MCGuiWait(@"on disk, written by the host", 10, ^BOOL {
        return [Read(gFile) isEqualToString:@"first line\ntyped over ssh\n"] && !TabFor(gC, gFile).dirty;
    });
    // ---- the tree through the host
    MCGuiStep(@"the tree lists the remote folder through its host", ^{
        if (!gC) return;
        MCRemoteWorkspace *ws = [gC remoteWorkspace];
        GCHECK(ws != nil && ws.listings > 0);
        NSArray<NSString *> *top = TopNames(gC);
        MCGuiNote(@"the tree's top: %@", top);
        GCHECK([top isEqualToArray:(@[ @"src", @"debug.log", @"notes.txt" ])]);   // folders first
        FileItem *log = TreeRow(gC, [gFolder stringByAppendingPathComponent:@"debug.log"]);
        GCHECK(log != nil && log.ignored && log.source == ws);   // .gitignore's, marked by the host
        GCHECK(!TreeRow(gC, gFile).ignored);
        // Never listed from this Mac's disk, though the stand-in's folder is on it.
        for (NSString *listed in MCTreeListedFolders()) GCHECK(![listed hasPrefix:gFolder]);
        FileItem *src = TreeRow(gC, [gFolder stringByAppendingPathComponent:@"src"]);
        GCHECK(src != nil && src.isDir);
        if (src) [gC.outline expandItem:src];
        GCHECK(TreeRow(gC, [gFolder stringByAppendingPathComponent:@"src/main.c"]) != nil);
        GCHECK(TreeRow(gC, [gFolder stringByAppendingPathComponent:@"src/util.c"]) != nil);
    });
    // The step after: the folders the tree opened are told to the host on
    // the main queue's next turn.
    MCGuiStep(@"files made on disk there", ^{
        MCGuiWriteFile([gFolder stringByAppendingPathComponent:@"later.txt"], @"made on disk\n");
        MCGuiWriteFile([gFolder stringByAppendingPathComponent:@"src/made-there.c"], @"int there;\n");
    });
    MCGuiWait(@"files made on disk appear in the tree", 15, ^BOOL {
        return TreeRow(gC, [gFolder stringByAppendingPathComponent:@"later.txt"]) != nil &&
               TreeRow(gC, [gFolder stringByAppendingPathComponent:@"src/made-there.c"]) != nil;
    });
    MCGuiStep(@"by the host's notices; a row opens its file through the host", ^{
        GCHECK([gC remoteWorkspace].notices > 0);
        for (NSString *listed in MCTreeListedFolders()) GCHECK(![listed hasPrefix:gFolder]);
        NSString *mainC = [gFolder stringByAppendingPathComponent:@"src/main.c"];
        const NSInteger row = [gC.outline rowForItem:TreeRow(gC, mainC)];
        GCHECK(row >= 0);
        if (row >= 0) [gC activateRow:row];   // Return in the tree
        MCTab *t = TabFor(gC, mainC);
        GCHECK(t != nil && t.session != nil);
        GCHECK_EQ_STR(t.currentText, @"int needle = 1;\n");
        GCHECK(t.fileModDate == nil);
        // What would change this Mac's disk at the folder's path says why not.
        [gC newFolder:nil];
        NSString *note = [[gC valueForKey:@"settingsLabel"] stringValue];
        MCGuiNote(@"New Folder: %@", note);
        GCHECK([note hasPrefix:@"New Folder does not work in a window over SSH yet"]);
    });
    // ---- Quick Open and Find in Folder on the host
    MCGuiStep(@"Cmd+P lists the host's files", ^{
        MCGuiSetKeyWindow(gC.window);
        GCHECK(MCGuiPerformMenuKey(gC.window, 35, NSEventModifierFlagCommand, nil, nil));
        GCHECK(Palette(gC) != nil);
    });
    MCGuiWait(@"listed by the host", 15, ^BOOL { return Palette(gC) && MCCommandPaletteFilesListed(Palette(gC)); });
    MCGuiStep(@"the ignored file left out; \"util\" and Return opens src/util.c", ^{
        GCHECK([gC remoteWorkspace].fileLists > 0);
        NSArray<NSString *> *labels = MCCommandPaletteLabels(Palette(gC));
        MCGuiNote(@"Quick Open lists: %@", labels);
        BOOL log = NO, util = NO;
        for (NSString *l in labels) {
            log = log || [l containsString:@"debug.log"];
            util = util || [l containsString:@"util.c"];
        }
        GCHECK(util && !log);
        [PaletteEditor(gC) insertText:@"util" replacementRange:PaletteEditor(gC).selectedRange];
        [PaletteEditor(gC) doCommandBySelector:@selector(insertNewline:)];
    });
    MCGuiWait(@"src/util.c open, read through the host", 10, ^BOOL {
        return [gC.currentPath isEqualToString:[gFolder stringByAppendingPathComponent:@"src/util.c"]] &&
               [TabFor(gC, gC.currentPath).currentText isEqualToString:@"// a needle here\n"];
    });
    MCGuiStep(@"Shift+Cmd+F searches on the host", ^{
        MCGuiSetKeyWindow(nil);
        [gC openSearch:nil];
        SearchPanel *p = gC.searchPanel;
        GCHECK(p != nil && p.hostSearch != nil);
        p.field.stringValue = @"needle";
        [p runSearch:nil];
    });
    MCGuiWait(@"its rows", 15, ^BOOL {
        SearchPanel *p = gC.searchPanel;
        return !p.busy && [p.status.stringValue containsString:@"match"];
    });
    MCGuiStep(@"two files, the ignored one left out; a row opens its file", ^{
        SearchPanel *p = gC.searchPanel;
        MCGuiNote(@"status: %@", p.status.stringValue);
        GCHECK_EQ_STR(p.status.stringValue, @"2 matches in 2 files");
        GCHECK([gC remoteWorkspace].searches > 0);
        NSOutlineView *t = (NSOutlineView *)p.table;
        NSInteger hit = -1;
        for (NSInteger r = 0; r < t.numberOfRows && hit < 0; r++) {
            id item = [t itemAtRow:r];
            if ([item respondsToSelector:@selector(line)] &&
                [[item valueForKey:@"path"] hasSuffix:@"/src/main.c"])
                hit = r;
        }
        GCHECK(hit >= 0);
        if (hit >= 0) {
            [t selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)hit] byExtendingSelection:NO];
            [NSApp sendAction:t.doubleAction to:t.target from:t];
        }
        GCHECK([gC.currentPath hasSuffix:@"/src/main.c"]);
        // Replace in Folder is not offered over SSH, and says why.
        p.replaceField.stringValue = @"pin";
        [p replaceAll:nil];
        MCGuiNote(@"Replace All: %@", p.status.stringValue);
        GCHECK([p.status.stringValue hasPrefix:@"Replace in Folder does not work in a window over SSH yet"]);
        GCHECK_EQ_STR(Read([gFolder stringByAppendingPathComponent:@"src/main.c"]), @"int needle = 1;\n");
        [p.window orderOut:nil];
    });
    // ---- a new file's address
    MCGuiStep(@"ssh://standin/<folder>/src/fresh.txt, not there yet, opens an empty untitled tab", ^{
        NSString *fresh = [gFolder stringByAppendingPathComponent:@"src/fresh.txt"];
        NSURL *url = [NSURL URLWithString:[@"ssh://standin" stringByAppendingString:fresh]];
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp openURLs:@[ url ]];
        if (NewAlerts().count) MCGuiNote(@"alerts in this suite: %@ (%@)", NewAlerts(), MCGuiLastAlertInformative());
        GCHECK(NewAlerts().count == 0);
        GCHECK(RemoteWindow() == gC);   // the folder's window, already open
        MCTab *t = TabFor(gC, fresh);
        GCHECK(t != nil && t.untitled && !t.dirty);
        GCHECK_EQ_STR(t.currentText, @"");
        GCHECK(![NSFileManager.defaultManager fileExistsAtPath:fresh]);
        GCHECK([gC.currentPath isEqualToString:fresh]);
        TypeAtEnd(gC, @"fresh\n");
        GCHECK(t.dirty);
        [gC saveCurrentFile:nil];
    });
    MCGuiWait(@"made by the host's save", 10, ^BOOL {
        NSString *fresh = [gFolder stringByAppendingPathComponent:@"src/fresh.txt"];
        MCTab *t = TabFor(gC, fresh);
        return [Read(fresh) isEqualToString:@"fresh\n"] && t && !t.dirty && !t.untitled;
    });
    MCGuiStep(@"still no stamp, and no question was asked", ^{
        if (!gC) return;
        GCHECK(TabFor(gC, gFile).fileModDate == nil);
        GCHECK(NewAlerts().count == 0);
        if (NewAlerts().count) MCGuiNote(@"alerts in this suite: %@", NewAlerts());
        [gC newTerminal:nil];
        gTerm = [gC valueForKey:@"terminal"];
        GCHECK(gTerm != nil && gTerm.onSessionHost && gTerm.sessionTerminal != 0);
        GCHECK(gTerm.shellPid == 0);   // a remote shell has no pid of this Mac's
    });
    // The parent's name as ps gives it: the stand-in's `mutible`, the host
    // itself; the variable is the stand-in's, not the Mac's.
    RunUntil(@"the terminal runs in the host's shell and environment",
             @"echo \"remote-$SSH_STANDIN-$((6*7))\" \"parent-$(basename \"$(ps -o comm= -p $PPID)\")\"",
             @"remote-yes-42 parent-mutible");
    MCGuiStep(@"the connection drops", ^{
        if (!gC) return;
        MCGuiNote(@"terminal: %@", [gTerm recentText:10]);
        GCHECK(!gC.sessionHostLost);
        // The stand-in is the leader of its own process group (the spawn's
        // session): the host and anything it started go with it.
        if (gHostA > 0) kill(-gHostA, SIGKILL);
    });
    MCGuiWait(@"the window says it is disconnected", 15, ^BOOL {
        return gC.sessionHostLost && [gC.window.title containsString:@"(disconnected)"];
    });
    MCGuiStep(@"the text stays, and takes more typing", ^{
        MCTab *t = TabFor(gC, gFile);
        GCHECK_EQ_STR(t.currentText, @"first line\ntyped over ssh\n");
        GCHECK(!t.session.hostSavesFile);
        TypeAtEnd(gC, @"while away\n");
        GCHECK(t.dirty);
        GCHECK_EQ_STR(t.currentText, @"first line\ntyped over ssh\nwhile away\n");
        // Its button in the status bar.
        BOOL button = NO;
        for (NSView *v in [[gC valueForKey:@"statusBar"] subviews])
            if ([v isKindOfClass:NSButton.class] && [((NSButton *)v).title isEqualToString:@"Reconnect"] && !v.hidden)
                button = YES;
        GCHECK(button);
        [gC reconnectSessionHost:nil];
    });
    MCGuiWait(@"connected again", 20, ^BOOL { return !gC.sessionHostLost && TabFor(gC, gFile).session.hostSavesFile; });
    MCGuiStep(@"the file is open on the new host with everything typed; Cmd+S saves it there", ^{
        gHostB = StandInPid();
        GCHECK(gHostB > 0 && gHostB != gHostA);
        GCHECK(Gone(gHostA));
        GCHECK(![gC.window.title containsString:@"(disconnected)"]);
        MCTab *t = TabFor(gC, gFile);
        GCHECK(t.dirty);
        GCHECK_EQ_STR(MCSessionCheckTabs(@[ t ]), @"");
        [gC saveCurrentFile:nil];
    });
    MCGuiWait(@"saved by the new host", 10, ^BOOL {
        return [Read(gFile) isEqualToString:@"first line\ntyped over ssh\nwhile away\n"] && !TabFor(gC, gFile).dirty;
    });
    MCGuiStep(@"the terminal's new shell, there", ^{
        gTerm = [gC valueForKey:@"terminal"];
        GCHECK(gTerm.sessionTerminal != 0);
    });
    // Typed again every 3 s until it shows: nothing says when the new
    // shell's line editor is ready, and one starting may drop what came
    // before it.
    RunUntil(@"it runs a command on the new host", @"echo \"again-$SSH_STANDIN-$((7*8))\"", @"again-yes-56");
    MCGuiStep(@"a file made after Reconnect", ^{
        MCGuiWriteFile([gFolder stringByAppendingPathComponent:@"after-reconnect.txt"], @"again\n");
    });
    MCGuiWait(@"comes into the tree by the new host's notices", 15, ^BOOL {
        return TreeRow(gC, [gFolder stringByAppendingPathComponent:@"after-reconnect.txt"]) != nil;
    });
    MCGuiStep(@"the window closes", ^{
        GCHECK(NewAlerts().count == 0);
        if (NewAlerts().count) MCGuiNote(@"alerts in this suite: %@", NewAlerts());
        gTerm = nil;
        [gC.window performClose:nil];
        gC = nil;
    });
    MCGuiWait(@"gone", 10, ^BOOL { return RemoteWindow() == nil; });
    MCGuiWait(@"the host it ran ends with it", 15, ^BOOL { return Gone(gHostB); });
    MCGuiStep(@"nothing left", ^{
        if (gHostB > 0 && !Gone(gHostB)) kill(-gHostB, SIGKILL);   // the suite's own, by its recorded pid
        MCSetEnv("MUTIBLE_SSH", nullptr);
    });
}
