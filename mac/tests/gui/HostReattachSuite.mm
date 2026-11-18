// HostReattachSuite.mm — `make guitest SUITES=host_reattach`: windows
// detach from the session's host as they close, and the folder's next
// window reattaches (stage 6; docs/notes/session-host.md, "The Mac
// reattaches"), as GTK's host_reattach checks on Linux.
//   - editor.session-host = on (the process's own host): a window with a
//     saved file and a terminal that ran a command closes; a new window on
//     the folder gets the same document (its id on the host, its text, not
//     unsaved) and the same shell (its pid, the command's output in the
//     replay), which runs another command.
//   - editor.session-host = headless: the window starts the folder's
//     `mutible host` (a process of its own, from build/mutible-cli). A
//     file typed into and a terminal that ran a command; the window closes
//     with the question's Keep Unsaved; `mutible attach --dump` shows the
//     file unsaved on the host and the shell there; the folder's next
//     window has the tab back with the typed text, marked unsaved while the
//     disk still has the old text, and the same shell with its output.
//   - The host killed: the window says so and shows Reconnect; typing goes
//     on; Reconnect starts a new host, and the file is open on it with
//     everything typed, unsaved (`attach --dump`), and the terminal has a
//     new shell. The window closes with Don't Save, and the hosts this
//     suite started are ended by their pids.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "EditorTab.h"
#import "SessionLink.h"
#import "SessionWindow.h"
#import "Terminal.h"

#include <signal.h>

namespace {

NSString *gDirA, *gDirB, *gFileA, *gFileB;
EditorController *gC;
TerminalView *gTerm;
uint64_t gDocId, gTermId;
pid_t gShell, gHost1, gHost2;
NSString *gHostEnv;   // MUTIBLE_HOST as the suite found it

NSString *CLIPath() { return [MCGuiBuildDir() stringByAppendingPathComponent:@"mutible-cli"]; }

NSTextView *TV(EditorController *c) { return [c valueForKey:@"textView"]; }

EditorController *WindowOn(NSString *dir) {
    for (EditorController *c in MCGuiControllers())
        if ([c.rootPath.stringByResolvingSymlinksInPath isEqualToString:dir.stringByResolvingSymlinksInPath]) return c;
    return nil;
}

EditorController *OpenWindowOn(NSString *dir) {
    [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp openURLs:@[ [NSURL fileURLWithPath:dir] ]];
    return WindowOn(dir);
}

MCTab *TabFor(EditorController *c, NSString *path) {
    for (MCTab *t in [c valueForKey:@"tabs"])
        if ([t.currentPath.stringByResolvingSymlinksInPath isEqualToString:path.stringByResolvingSymlinksInPath])
            return t;
    return nil;
}

std::shared_ptr<MCSession::Link> LinkOf(EditorController *c) {
    MCSessionWindow *w = [c sessionWindow];
    return w ? w.link.link : nullptr;
}

NSButton *ReconnectButton(EditorController *c) {
    NSView *bar = [c valueForKey:@"statusBar"];
    for (NSView *v in bar.subviews)
        if ([v isKindOfClass:NSButton.class] && [((NSButton *)v).title isEqualToString:@"Reconnect"])
            return (NSButton *)v;
    return nil;
}

void TypeAtEnd(EditorController *c, NSString *s) {
    NSTextView *tv = TV(c);
    MCGuiUndoGroup(tv.undoManager, ^{ [tv insertText:s replacementRange:NSMakeRange(tv.string.length, 0)]; });
}

// `mutible attach --folder DIR --dump`, its output to `check`.
void Dump(NSString *name, NSString *dir, void (^check)(int status, NSString *out)) {
    MCGuiAsync(name, 30, ^(MCGuiDone done) {
        MCGuiRunProcess(@[ CLIPath(), @"attach", @"--folder", dir, @"--dump" ], nil,
                        ^(int status, NSString *out, NSString *err) {
                            if (status != 0) MCGuiNote(@"attach --dump: %d %@ %@", status, out, err);
                            check(status, out);
                            done();
                        });
    });
}

}  // namespace

MC_GUI_SUITE(host_reattach) {
    // ---------------------------------------------------- the process's host
    MCGuiStep(@"the session's host on; a window on a folder of its own", ^{
        MCGuiSetSettings(@"lsp.enabled = false\neditor.session-host = on\n");
        GCHECK(MCSessionHostEnabled());
        gDirA = [MCGuiScratch() stringByAppendingPathComponent:@"reattach-a"];
        gFileA = [gDirA stringByAppendingPathComponent:@"a.txt"];
        MCGuiWriteFile(gFileA, @"alpha\n");
        gC = OpenWindowOn(gDirA);
        GCHECK(gC != nil);
        GCHECK(LinkOf(gC) && LinkOf(gC)->kind() == MCSession::Link::Kind::InProcess);
        GCHECK([gC showFile:gFileA]);
        TypeAtEnd(gC, @"saved line\n");
        [gC saveCurrentFile:nil];
        MCTab *t = TabFor(gC, gFileA);
        GCHECK(t.session != nil && !t.dirty);
        gDocId = t.session.docId;
        [gC newTerminal:nil];
        gTerm = [gC valueForKey:@"terminal"];
        GCHECK(gTerm.onSessionHost && gTerm.sessionTerminal != 0);
    });
    MCGuiWait(@"its prompt", 20, ^BOOL { return gTerm.atPrompt && !gTerm.busy; });
    MCGuiStep(@"a command in it", ^{ [gTerm runCommand:@"echo kept-$((3*4))"]; });
    MCGuiWait(@"it ran", 20, ^BOOL {
        return [[gTerm recentText:20] containsString:@"\nkept-12"] && gTerm.atPrompt && !gTerm.busy;
    });
    MCGuiStep(@"the window closes", ^{
        gTermId = gTerm.sessionTerminal;
        gShell = gTerm.shellPid;
        GCHECK(gShell > 0);
        gTerm = nil;
        [gC.window performClose:nil];
    });
    MCGuiWait(@"gone", 10, ^BOOL { return WindowOn(gDirA) == nil; });
    MCGuiStep(@"the host keeps the document, the shell and the window's record", ^{
        gC = nil;
        GCHECK(kill(gShell, 0) == 0);   // the shell runs on
        const Json st = MCSessionClient().sessionState();
        bool doc = false, term = false, record = false;
        for (const Json &d : st["documents"].elements()) doc = doc || (uint64_t)d["doc"].asInt() == gDocId;
        for (const Json &t : st["terminals"].elements()) term = term || (uint64_t)t["term"].asInt() == gTermId;
        for (const Json &w : st["windows"].elements())
            if (w["tabs"]["detached"].asBool())
                for (const Json &t : w["tabs"]["terms"].elements()) record = record || (uint64_t)t.asInt() == gTermId;
        GCHECK(doc);
        GCHECK(term);
        GCHECK(record);
    });
    MCGuiStep(@"a new window on the folder takes them back", ^{
        gC = OpenWindowOn(gDirA);
        GCHECK(gC != nil);
        MCTab *t = TabFor(gC, gFileA);
        GCHECK(t != nil && t.session.docId == gDocId);
        GCHECK_EQ_STR(t.currentText, @"alpha\nsaved line\n");
        GCHECK(!t.dirty);
        gTerm = [gC valueForKey:@"terminal"];
        GCHECK(gTerm != nil && gTerm.sessionTerminal == gTermId && gTerm.shellPid == gShell);
    });
    MCGuiWait(@"the terminal shows what ran before, from the host's replay", 20, ^BOOL {
        return [[gTerm recentText:40] containsString:@"kept-12"];
    });
    MCGuiStep(@"another command in the same shell", ^{ [gTerm runCommand:@"echo again-$((6*7))"]; });
    MCGuiWait(@"it ran there", 20, ^BOOL {
        return [[gTerm recentText:20] containsString:@"\nagain-42"] && gTerm.shellPid == gShell;
    });
    MCGuiStep(@"the window closes again", ^{
        gTerm = nil;
        [gC.window performClose:nil];
    });
    MCGuiWait(@"gone", 10, ^BOOL { return WindowOn(gDirA) == nil; });

    // ---------------------------------------------------- a headless host
    MCGuiStep(@"editor.session-host = headless: the window starts the folder's host", ^{
        gC = nil;
        gHostEnv = MCEnv(@"MUTIBLE_HOST");
        MCSetEnv("MUTIBLE_HOST", nullptr);   // the setting decides
        MCSetEnv("MUTIBLE_HOST_IDLE_MINUTES", "2");   // one left behind ends by itself
        MCGuiSetSettings(@"lsp.enabled = false\neditor.session-host = headless\n");
        GCHECK(MCSessionHostEnabled());
        gDirB = [MCGuiScratch() stringByAppendingPathComponent:@"reattach-b"];
        gFileB = [gDirB stringByAppendingPathComponent:@"b.txt"];
        MCGuiWriteFile(gFileB, @"beta\n");
        gC = OpenWindowOn(gDirB);
        GCHECK(gC != nil);
        std::shared_ptr<MCSession::Link> l = LinkOf(gC);
        GCHECK(l && l->kind() == MCSession::Link::Kind::Socket);
        gHost1 = l ? l->startedHost() : 0;
        GCHECK(gHost1 > 0 && gHost1 != getpid());
        GCHECK([gC showFile:gFileB]);
        TypeAtEnd(gC, @"unsaved-edit\n");
        MCTab *t = TabFor(gC, gFileB);
        GCHECK(t.dirty && t.session.link.link == l);
        gDocId = t.session.docId;
        [gC newTerminal:nil];
        gTerm = [gC valueForKey:@"terminal"];
        GCHECK(gTerm.onSessionHost && gTerm.sessionLink.link == l);
    });
    MCGuiWait(@"its prompt", 20, ^BOOL { return gTerm.atPrompt && !gTerm.busy; });
    MCGuiStep(@"a command in it", ^{ [gTerm runCommand:@"echo held-$((5*5))"]; });
    MCGuiWait(@"it ran", 20, ^BOOL {
        return [[gTerm recentText:20] containsString:@"\nheld-25"] && gTerm.atPrompt && !gTerm.busy;
    });
    __block NSUInteger alertsBefore = 0;
    MCGuiStep(@"the window closes: Keep Unsaved", ^{
        gTermId = gTerm.sessionTerminal;
        gShell = gTerm.shellPid;
        gTerm = nil;
        alertsBefore = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[ @(NSAlertSecondButtonReturn) ]);   // Save, Keep Unsaved, Don't Save, Cancel
        [gC.window performClose:nil];
    });
    MCGuiWait(@"gone", 10, ^BOOL { return WindowOn(gDirB) == nil; });
    MCGuiStep(@"the question offered Keep Unsaved; the file is untouched on disk", ^{
        gC = nil;
        GCHECK(MCGuiAlertsShown().count == alertsBefore + 1);
        GCHECK([MCGuiLastAlertInformative() containsString:@"Keep Unsaved leaves your changes unsaved"]);
        GCHECK_EQ_STR([NSString stringWithContentsOfFile:gFileB encoding:NSUTF8StringEncoding error:nil], @"beta\n");
        GCHECK(kill(gHost1, 0) == 0 && kill(gShell, 0) == 0);
    });
    Dump(@"`mutible attach --dump` shows the file unsaved and the shell on the host", gDirB, ^(int status, NSString *out) {
        GCHECK(status == 0);
        GCHECK([out containsString:@"unsaved)"]);
        GCHECK([out containsString:@"beta\nunsaved-edit\n"]);
        NSString *shell = [NSString stringWithFormat:@"shell %d", gShell];
        GCHECK([out containsString:shell]);
        GCHECK([out containsString:@"held-25"]);
    });
    MCGuiStep(@"the folder's next window has the tab and the shell back", ^{
        gC = OpenWindowOn(gDirB);
        GCHECK(gC != nil);
        std::shared_ptr<MCSession::Link> l = LinkOf(gC);
        GCHECK(l && l->kind() == MCSession::Link::Kind::Socket && !l->lost());
        MCTab *t = TabFor(gC, gFileB);
        GCHECK(t != nil && t.session.docId == gDocId);
        GCHECK_EQ_STR(t.currentText, @"beta\nunsaved-edit\n");
        GCHECK(t.dirty);   // the disk still has the old text
        GCHECK_EQ_STR(TV(gC).string, @"beta\nunsaved-edit\n");
        gTerm = [gC valueForKey:@"terminal"];
        GCHECK(gTerm != nil && gTerm.sessionTerminal == gTermId && gTerm.shellPid == gShell);
    });
    MCGuiWait(@"its screen has what ran before", 20, ^BOOL { return [[gTerm recentText:40] containsString:@"held-25"]; });
    MCGuiStep(@"another command in the same shell", ^{ [gTerm runCommand:@"echo back-$((4*4))"]; });
    MCGuiWait(@"it ran there", 20, ^BOOL {
        return [[gTerm recentText:20] containsString:@"\nback-16"] && gTerm.shellPid == gShell;
    });

    // ---------------------------------------------------- the host killed
    MCGuiStep(@"the host is killed", ^{ GCHECK(kill(gHost1, SIGKILL) == 0); });
    MCGuiWait(@"the window offers Reconnect", 20, ^BOOL {
        NSButton *b = ReconnectButton(gC);
        return b && !b.hidden && LinkOf(gC)->lost();
    });
    MCGuiWait(@"and the terminal says its shell went", 10, ^BOOL {
        return [[gTerm recentText:10] containsString:@"session's host went away"];
    });
    MCGuiStep(@"typing goes on while it is gone", ^{
        TypeAtEnd(gC, @"after-kill\n");
        GCHECK([TV(gC).string hasSuffix:@"after-kill\n"]);
    });
    MCGuiStep(@"Reconnect", ^{
        [gC reconnectSessionHost:nil];
        std::shared_ptr<MCSession::Link> l = LinkOf(gC);
        GCHECK(l && !l->lost() && l->kind() == MCSession::Link::Kind::Socket);
        gHost2 = l ? l->startedHost() : 0;
        GCHECK(gHost2 > 0 && gHost2 != gHost1);
        MCTab *t = TabFor(gC, gFileB);
        GCHECK(t.session.open && t.session.link.link == l && t.session.docId != gDocId);
        GCHECK(t.dirty);
        NSButton *b = ReconnectButton(gC);
        GCHECK(!b || b.hidden);
        GCHECK(gTerm.sessionLink.link == l);
        GCHECK(MCSessionCheckTabs([gC valueForKey:@"tabs"]).length == 0);
    });
    Dump(@"nothing typed is lost: the new host has it all, unsaved", gDirB, ^(int status, NSString *out) {
        GCHECK(status == 0);
        GCHECK([out containsString:@"beta\nunsaved-edit\nafter-kill\n"]);
        GCHECK([out containsString:@"unsaved)"]);
    });
    MCGuiWait(@"the terminal has a new shell", 20, ^BOOL {
        return gTerm.shellPid > 0 && gTerm.shellPid != gShell && gTerm.atPrompt && !gTerm.busy;
    });
    MCGuiStep(@"which runs a command", ^{ [gTerm runCommand:@"echo new-$((9*9))"]; });
    MCGuiWait(@"it ran", 20, ^BOOL { return [[gTerm recentText:20] containsString:@"\nnew-81"]; });
    MCGuiStep(@"the window closes: Don't Save", ^{
        gTerm = nil;
        MCGuiAnswerAlerts(@[ @(NSAlertThirdButtonReturn) ]);
        [gC.window performClose:nil];
    });
    MCGuiWait(@"gone", 10, ^BOOL { return WindowOn(gDirB) == nil; });
    MCGuiStep(@"the file was not written; the hosts this suite started end", ^{
        gC = nil;
        GCHECK_EQ_STR([NSString stringWithContentsOfFile:gFileB encoding:NSUTF8StringEncoding error:nil], @"beta\n");
        if (gHost2 > 0) kill(gHost2, SIGTERM);   // gHost1 was killed above
        MCSetEnv("MUTIBLE_HOST_IDLE_MINUTES", nullptr);
        MCSetEnv("MUTIBLE_HOST", gHostEnv.UTF8String);
    });
    MCGuiWait(@"the second host is gone", 10, ^BOOL { return gHost2 <= 0 || kill(gHost2, 0) != 0; });
}
