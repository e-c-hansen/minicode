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
//   - Stage 6, step 3 ("The host's own backups, untitled buffers, one path
//     twice"), headless again: a host started while MUTIBLE_BACKUP_DIR is
//     not in the app's environment still backs up into the app's store
//     (the Mac passes it, MCSessionHostEnvironment); a file left unsaved
//     with Keep Unsaved is backed up by the host, under its pid, and the
//     window's own backup goes. A file saved and then deleted on disk, an
//     untitled buffer and a second document of the unsaved file (a script's,
//     over the socket) are left on the host. The folder's next window gets
//     one tab of the file (the record's document; the other stays on the
//     host), the deleted file unsaved on the host and in the tab, and the
//     untitled buffer as the unsaved tab of Untitled-1, a file not on disk;
//     its own backups are written as it opens and the host's go. Cmd+S
//     writes Untitled-1, and only then does the host have its path.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "EditorTab.h"
#import "BackupStore.h"
#import "SessionLink.h"
#import "SessionWindow.h"
#import "Terminal.h"

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <climits>
#include <memory>
#include <string>

#include "Backups.h"
#include "SessionServer.h"

namespace {

NSString *gDirA, *gDirB, *gFileA, *gFileB;
EditorController *gC;
TerminalView *gTerm;
uint64_t gDocId, gTermId;
pid_t gShell, gHost1, gHost2;
NSString *gHostEnv;   // MUTIBLE_HOST as the suite found it
// Step 3's folder (symlinks resolved, as the host names it), its files, the
// untitled buffer's name, and what the host holds of them.
NSString *gDirC, *gFileC, *gFileD, *gUntitled, *gBackupEnv;
uint64_t gDocC, gDocD, gUntitledId, gOtherId;
pid_t gHost3;
std::string gSocketC;

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

// `mutible attach --folder DIR --dump` on a headless folder (`folder` read
// when the step runs, not when it is queued), its output to `check`.
void Dump(NSString *name, NSString *__strong *folder, void (^check)(int status, NSString *out)) {
    MCGuiAsync(name, 30, ^(MCGuiDone done) {
        MCGuiRunProcess(@[ CLIPath(), @"attach", @"--folder", *folder, @"--dump" ], nil,
                        ^(int status, NSString *out, NSString *err) {
                            if (status != 0) MCGuiNote(@"attach --dump: %d %@ %@", status, out, err);
                            check(status, out);
                            done();
                        });
    });
}

// The line of a dump that starts with `prefix`, nil for none.
NSString *DumpLine(NSString *out, NSString *prefix) {
    for (NSString *line in [out componentsSeparatedByString:@"\n"])
        if ([line hasPrefix:prefix]) return line;
    return nil;
}

NSString *RealPath(NSString *path) {
    char buf[PATH_MAX];
    return realpath(path.fileSystemRepresentation, buf) ? @(buf) : path;
}

// The backup of `path` in the app's store held by `pid` (0: by anyone), its
// text in *text.
BOOL BackupOf(NSString *path, pid_t pid, NSString **text = nil) {
    Backups::Store s;
    s.dir = [MCBackupStore shared].directory.fileSystemRepresentation ?: "";
    s.fs = Backups::posixFs();
    for (const Backups::Entry &e : s.list()) {
        if (e.path != std::string(path.fileSystemRepresentation) || (pid && e.ownerPid != pid)) continue;
        Backups::Entry got;
        std::string t;
        if (!s.load(e.id, &got, &t)) continue;
        if (text) *text = @(t.c_str());
        return YES;
    }
    return NO;
}

// A renderer that is not an app (as a script or the browser's view would
// be), over the host's socket: it leaves a document of `path` ("" for an
// untitled buffer) holding `text` on the host, and goes. Its id, 0 when it
// could not.
struct Quiet : Shared::Client::Listener {
    void hostEdited(Shared::DocId, const Ot::Ops &, int, const Shared::Selection *) override {}
};
uint64_t LeaveOnHost(const std::string &socket, const std::string &path, const std::string &text) {
    std::string error;
    std::unique_ptr<Shared::HostLink> link = Shared::HostLink::open(socket, &error);
    if (!link) {
        MCGuiNote(@"no link to %s: %s", socket.c_str(), error.c_str());
        return 0;
    }
    Shared::Client client(Model::Unit::Utf16, "script");
    link->connect(client);
    Quiet quiet;
    uint64_t id = 0;
    if (client.settle()) {
        id = client.newId();
        client.open(id, path, text, &quiet);
        client.detach(id);
        if (!client.settle()) id = 0;
    }
    link->close();
    return id;
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
    Dump(@"`mutible attach --dump` shows the file unsaved and the shell on the host", &gDirB, ^(int status, NSString *out) {
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
        // Opened anew on the new host (whose ids may come round to the old
        // ones: a new host gives the first slot again).
        GCHECK(t.session.open && t.session.link.link == l);
        GCHECK(t.dirty);
        NSButton *b = ReconnectButton(gC);
        GCHECK(!b || b.hidden);
        GCHECK(gTerm.sessionLink.link == l);
        GCHECK(MCSessionCheckTabs([gC valueForKey:@"tabs"]).length == 0);
    });
    Dump(@"nothing typed is lost: the new host has it all, unsaved", &gDirB, ^(int status, NSString *out) {
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

    // ---------------------------------------------------- stage 6, step 3
    MCGuiStep(@"a headless host started while MUTIBLE_BACKUP_DIR is not in the app's environment", ^{
        gHostEnv = MCEnv(@"MUTIBLE_HOST");
        MCSetEnv("MUTIBLE_HOST", nullptr);
        MCSetEnv("MUTIBLE_HOST_IDLE_MINUTES", "2");
        MCGuiSetSettings(@"lsp.enabled = false\neditor.session-host = headless\n");
        NSString *dir = [MCGuiScratch() stringByAppendingPathComponent:@"reattach-c"];
        MCGuiWriteFile([dir stringByAppendingPathComponent:@"c.txt"], @"gamma\n");
        MCGuiWriteFile([dir stringByAppendingPathComponent:@"d.txt"], @"delta\n");
        gDirC = RealPath(dir);   // as the host names it (/private/tmp, not /tmp)
        gFileC = [gDirC stringByAppendingPathComponent:@"c.txt"];
        gFileD = [gDirC stringByAppendingPathComponent:@"d.txt"];
        gUntitled = [gDirC stringByAppendingPathComponent:@"Untitled-1"];
        // The store is the scratch world's, which the CLI would not find by
        // $HOME: a host that did not get it from the app writes elsewhere.
        NSString *store = [MCBackupStore shared].directory;
        GCHECK(store.length && [store hasPrefix:MCGuiScratch()]);
        NSString *byHome = [MCEnv(@"HOME") stringByAppendingPathComponent:@"Library/Application Support/Mutible/Backups"];
        GCHECK(![byHome.stringByStandardizingPath isEqualToString:store]);
        gBackupEnv = MCEnv(@"MUTIBLE_BACKUP_DIR");
        MCSetEnv("MUTIBLE_BACKUP_DIR", nullptr);
        GCHECK_EQ_STR(MCSessionHostEnvironment()[@"MUTIBLE_BACKUP_DIR"], store);
        gC = OpenWindowOn(gDirC);
        MCSetEnv("MUTIBLE_BACKUP_DIR", gBackupEnv.UTF8String);   // the host read its own at start
        GCHECK(gC != nil);
        std::shared_ptr<MCSession::Link> l = LinkOf(gC);
        GCHECK(l && l->kind() == MCSession::Link::Kind::Socket);
        gHost3 = l ? l->startedHost() : 0;
        gSocketC = l ? l->socketPath() : "";
        GCHECK(gHost3 > 0 && gHost3 != gHost2);
    });
    MCGuiStep(@"one file typed into, another saved", ^{
        GCHECK([gC showFile:gFileD]);
        TypeAtEnd(gC, @"saved-d\n");
        [gC saveCurrentFile:nil];
        MCTab *d = TabFor(gC, gFileD);
        GCHECK(d.session != nil && !d.dirty);
        gDocD = d.session.docId;
        GCHECK([gC showFile:gFileC]);
        TypeAtEnd(gC, @"unsaved-c\n");
        MCTab *c = TabFor(gC, gFileC);
        GCHECK(c.dirty);
        gDocC = c.session.docId;
    });
    MCGuiWait(@"the window's own backup of the unsaved file", 15, ^BOOL {
        return BackupOf(gFileC, getpid()) && !BackupOf(gFileC, gHost3);
    });
    MCGuiStep(@"the window closes: Keep Unsaved", ^{
        MCGuiAnswerAlerts(@[ @(NSAlertSecondButtonReturn) ]);   // Save, Keep Unsaved, Don't Save, Cancel
        [gC.window performClose:nil];
    });
    MCGuiWait(@"gone", 10, ^BOOL { return WindowOn(gDirC) == nil; });
    MCGuiWait(@"the host's backup in the app's store, under its pid; the window's gone", 15, ^BOOL {
        NSString *text = nil;
        return BackupOf(gFileC, gHost3, &text) && [text isEqualToString:@"gamma\nunsaved-c\n"] &&
               !BackupOf(gFileC, getpid());
    });
    MCGuiStep(@"the saved file deleted; a script leaves an untitled buffer and a second document of the "
              @"unsaved file",
              ^{
                  gC = nil;
                  GCHECK([NSFileManager.defaultManager removeItemAtPath:gFileD error:nil]);
                  GCHECK(kill(gHost3, 0) == 0);   // the unsaved file keeps it
                  gUntitledId = LeaveOnHost(gSocketC, "", "scratch text\n");
                  gOtherId = LeaveOnHost(gSocketC, gFileC.fileSystemRepresentation, "other copy\n");
                  GCHECK(gUntitledId != 0 && gOtherId != 0);
              });
    MCGuiWait(@"the host backs the untitled buffer up as Untitled-1", 15, ^BOOL {
        NSString *text = nil;
        return BackupOf(gUntitled, gHost3, &text) && [text isEqualToString:@"scratch text\n"];
    });
    MCGuiStep(@"the folder's next window: one tab of the file, the deleted one unsaved, Untitled-1", ^{
        GCHECK(![NSFileManager.defaultManager fileExistsAtPath:gUntitled]);   // nothing written there
        gC = OpenWindowOn(gDirC);
        GCHECK(gC != nil);
        std::shared_ptr<MCSession::Link> l = LinkOf(gC);
        GCHECK(l && l->kind() == MCSession::Link::Kind::Socket && !l->lost() && l->socketPath() == gSocketC);
        NSArray<MCTab *> *tabs = [gC valueForKey:@"tabs"];
        NSUInteger ofC = 0;
        for (MCTab *t in tabs)
            if ([t.currentPath.stringByResolvingSymlinksInPath isEqualToString:gFileC.stringByResolvingSymlinksInPath])
                ofC++;
        GCHECK(ofC == 1 && tabs.count == 3);
        MCTab *c = TabFor(gC, gFileC);
        GCHECK(c.session.docId == gDocC && c.dirty);   // the record's own, not the script's
        GCHECK_EQ_STR(c.currentText, @"gamma\nunsaved-c\n");
        MCTab *d = TabFor(gC, gFileD);
        GCHECK(d.session.docId == gDocD && d.dirty && d.gone);
        GCHECK_EQ_STR(d.currentText, @"delta\nsaved-d\n");
        MCTab *u = TabFor(gC, gUntitled);
        GCHECK(u.session.docId == gUntitledId && u.untitled && u.dirty && !u.gone);
        GCHECK_EQ_STR(u.currentText, @"scratch text\n");
        GCHECK(![NSFileManager.defaultManager fileExistsAtPath:gUntitled]);
        // The other document of c.txt stays on the host for the next window.
        bool held = false;
        if (l)
            for (const Json &doc : l->client().sessionState()["documents"].elements())
                held = held || (uint64_t)doc["doc"].asInt() == gOtherId;
        GCHECK(held && l && !l->client().isOpen(gOtherId));
        // The window's own backups, written as it opened.
        [[MCBackupStore shared] flush];
        NSString *text = nil;
        GCHECK(BackupOf(gFileC, getpid(), &text) && [text isEqualToString:@"gamma\nunsaved-c\n"]);
        GCHECK(BackupOf(gFileD, getpid()));
        GCHECK(BackupOf(gUntitled, getpid(), &text) && [text isEqualToString:@"scratch text\n"]);
    });
    MCGuiWait(@"the host's backups let go", 15, ^BOOL {
        return !BackupOf(gFileC, gHost3) && !BackupOf(gUntitled, gHost3);
    });
    Dump(@"the host counts the deleted file unsaved and keeps the untitled buffer without a path", &gDirC,
         ^(int status, NSString *out) {
             GCHECK(status == 0);
             NSString *d = DumpLine(out, [NSString stringWithFormat:@"== document %llu %@ (", gDocD, gFileD]);
             GCHECK([d containsString:@"unsaved)"]);
             // No path (two spaces): the host counts it unsaved by its text.
             GCHECK(DumpLine(out, [NSString stringWithFormat:@"== document %llu  (", gUntitledId]) != nil);
             if (![d containsString:@"unsaved)"]) MCGuiNote(@"the dump: %@", out);
         });
    MCGuiStep(@"Cmd+S writes the untitled buffer to Untitled-1", ^{
        MCTab *u = TabFor(gC, gUntitled);
        [gC activateTab:u];
        [gC saveCurrentFile:nil];
        GCHECK_EQ_STR([NSString stringWithContentsOfFile:gUntitled encoding:NSUTF8StringEncoding error:nil],
                      @"scratch text\n");
        GCHECK(!u.untitled && !u.dirty);
    });
    Dump(@"and the host has its path now, saved", &gDirC, ^(int status, NSString *out) {
        GCHECK(status == 0);
        NSString *u = DumpLine(out, [NSString stringWithFormat:@"== document %llu %@ (", gUntitledId, gUntitled]);
        GCHECK([u containsString:@"saved)"] && ![u containsString:@"unsaved)"]);
        if (!u) MCGuiNote(@"the dump: %@", out);
    });
    MCGuiStep(@"the window closes: Don't Save", ^{
        MCGuiAnswerAlerts(@[ @(NSAlertThirdButtonReturn) ]);
        [gC.window performClose:nil];
    });
    MCGuiWait(@"gone", 10, ^BOOL { return WindowOn(gDirC) == nil; });
    MCGuiStep(@"nothing written but Untitled-1; no backups left of the folder; the host ends", ^{
        gC = nil;
        GCHECK_EQ_STR([NSString stringWithContentsOfFile:gFileC encoding:NSUTF8StringEncoding error:nil], @"gamma\n");
        GCHECK(![NSFileManager.defaultManager fileExistsAtPath:gFileD]);
        [[MCBackupStore shared] flush];
        GCHECK(!BackupOf(gFileC, 0) && !BackupOf(gFileD, 0) && !BackupOf(gUntitled, 0));
        if (gHost3 > 0) kill(gHost3, SIGTERM);
        MCSetEnv("MUTIBLE_HOST_IDLE_MINUTES", nullptr);
        MCSetEnv("MUTIBLE_HOST", gHostEnv.UTF8String);
    });
    MCGuiWait(@"the third host is gone", 10, ^BOOL { return gHost3 <= 0 || kill(gHost3, 0) != 0; });
}
