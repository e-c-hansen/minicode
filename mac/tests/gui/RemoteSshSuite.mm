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
//   - A terminal: its shell is the host's child (`ps` of its parent says
//     mutible-cli) in the host's environment (the stand-in's variable).
//   - The connection drops (the stand-in's process group killed): the
//     window says it is disconnected (its title, Reconnect), the text stays
//     and takes more typing. Reconnect runs ssh again: the file is open on
//     the new host with everything typed, saved there with Cmd+S, and the
//     terminal has a new shell there that runs a command.
//   - The window closes, and the host it ran ends with it.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "EditorTab.h"
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

void TypeAtEnd(EditorController *c, NSString *s) {
    NSTextView *tv = [c valueForKey:@"textView"];
    MCGuiUndoGroup(tv.undoManager, ^{ [tv insertText:s replacementRange:NSMakeRange(tv.string.length, 0)]; });
}

// The stand-in's pid, as it wrote it (the process group ssh's spawn made).
pid_t StandInPid() { return (pid_t)Read(gPidFile).intValue; }

BOOL Gone(pid_t pid) { return pid > 0 && kill(pid, 0) != 0 && errno == ESRCH; }

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
        // The session-host setting stays off: a window over SSH needs none.
        MCGuiSetSettings(@"lsp.enabled = false\n");
        NSFileManager *fm = NSFileManager.defaultManager;
        NSString *scratch = MCGuiScratch();
        NSString *made = [scratch stringByAppendingPathComponent:@"remote-proj"];
        [fm createDirectoryAtPath:made withIntermediateDirectories:YES attributes:nil error:nil];
        gFolder = RealPath(made);   // as the host names it
        gFile = [gFolder stringByAppendingPathComponent:@"notes.txt"];
        MCGuiWriteFile(gFile, @"first line\n");
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
        if (MCGuiAlertsShown().count) MCGuiNote(@"alerts: %@ (%@)", MCGuiAlertsShown(), MCGuiLastAlertInformative());
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
    MCGuiStep(@"still no stamp, and no question was asked", ^{
        if (!gC) return;
        GCHECK(TabFor(gC, gFile).fileModDate == nil);
        GCHECK(MCGuiAlertsShown().count == 0);
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
    MCGuiStep(@"the window closes", ^{
        GCHECK(MCGuiAlertsShown().count == 0);
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
