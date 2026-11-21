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
//   - Stage 7, saving through the host, on that headless host: a page's
//     guest (a script over the socket, as `attach --web --edit`'s server
//     is) saves a file the window shows with typing unsaved in it: the host
//     writes it (the rule that refused a page while an app's window had the
//     file is gone for the Mac's, which saves through the host), and the
//     window's tab is clean, its title too, and in step with the file's
//     stamp. The window types and saves: no question, the file written, and
//     the guest hears that the host wrote it (DocDirty flag 2, its stamp
//     the file's). A UTF-16 file saved from the window stays UTF-16 with
//     its mark. An agent's write, then the window's save: Keep Mine asked,
//     and the host writes over it. The window's dot and the host's count
//     agree throughout.
//   - Terminal splits in the record (the record's "termLayout",
//     TermDockRecord.h), with the process's host: two dock tabs, the first
//     split right and then down in the new column, both bars dragged (the
//     host's record keeps the shares from before a drag until the button
//     is let go, then takes the new ones at once), its top right pane
//     active and the second tab on show. The window closes and the
//     folder's next window has the same tabs: the same terminals in the
//     same columns and rows, the shares within 0.01 and laid out so, the
//     same active pane and tab. Closed again and the record rewritten as
//     GTK writes it (built here from JSON text): another arrangement, with
//     a pane naming a terminal that is not there; the next window has that
//     arrangement, the missing pane gone and its share given to its
//     neighbour, the active pane moved off it to the one below.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "EditorTab.h"
#import "BackupStore.h"
#import "SessionLink.h"
#import "SessionWindow.h"
#import "Terminal.h"
#import "TerminalPanel.h"
#import "EditorPanes.h"

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <climits>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "Backups.h"
#include "FolderSearch.h"
#include "SessionFiles.h"
#include "SessionServer.h"
#include "TermDockRecord.h"

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
// Stage 7: a page's guest on the headless host, and what it heard.
NSString *gFileE, *gFileF;
uint64_t gDocE;
std::unique_ptr<Shared::HostLink> gGuestLink;
std::unique_ptr<Shared::Client> gGuest;
std::vector<std::pair<uint64_t, Json>> gGuestWrites;   // DocDirty flag 2: doc, {path, stamp, ...}
size_t gWritesBefore;
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
// The host's count of a document as unsaved, by its state (the guest's).
bool HostDirty(Shared::Client &c, uint64_t doc) {
    const Json st = c.sessionState();
    for (const Json &d : st["documents"].elements())
        if ((uint64_t)d["doc"].asInt() == doc) return d["dirty"].asBool();
    return false;
}

NSData *Bytes(NSString *path) { return [NSData dataWithContentsOfFile:path] ?: [NSData data]; }

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

// ---- terminal splits

NSString *gDirS;
TerminalView *gA, *gB, *gC2, *gD;   // the panes: tab 0 [[A], [B, C]], tab 1 [[D]]
uint64_t gIdA, gIdB, gIdC, gIdD;
pid_t gShellA;
Shared::TermDock gDockBefore;       // as the first window left it

MCTerminalPanel *PanelOf(EditorController *c) { return [c valueForKey:@"terminalPanel"]; }

BOOL Idle(TerminalView *t) { return t.atPrompt && !t.busy; }

NSEvent *Mouse(NSWindow *w, NSEventType type, NSPoint p) {
    return [NSEvent mouseEventWithType:type location:p modifierFlags:0 timestamp:0
                          windowNumber:w.windowNumber context:nil eventNumber:0
                            clickCount:1 pressure:1];
}

// The window's record on the process's host now, null for none.
Json HostRecord(uint64_t window) {
    const Json st = MCSessionClient().sessionState();
    for (const Json &w : st["windows"].elements())
        if ((uint64_t)w["window"].asInt() == window) return w["tabs"];
    return Json();
}

BOOL Near(double a, double b) { return std::fabs(a - b) <= 0.01; }

BOOL SharesNear(const TerminalLayout::Fractions &a, const TerminalLayout::Fractions &b) {
    if (a.size() != b.size()) return NO;
    for (size_t i = 0; i < a.size(); i++)
        if (!Near(a[i], b[i])) return NO;
    return YES;
}

BOOL JsonSharesNear(const Json &j, const TerminalLayout::Fractions &b) {
    if (!j.isArray() || j.size() != b.size()) return NO;
    for (size_t i = 0; i < b.size(); i++)
        if (!Near(j[i].asNumber(-1), b[i])) return NO;
    return YES;
}

typedef std::vector<std::vector<uint64_t>> Columns;

Columns IdsIn(MCTerminalPanel *panel, NSUInteger tab) {
    Columns out;
    for (NSArray<TerminalView *> *column in [panel columnsInTab:tab]) {
        std::vector<uint64_t> ids;
        for (TerminalView *t in column) ids.push_back(t.sessionTerminal);
        out.push_back(ids);
    }
    return out;
}

NSString *ColumnsText(const Columns &cols) {
    NSMutableArray *parts = [NSMutableArray array];
    for (const auto &c : cols) {
        NSMutableArray *ids = [NSMutableArray array];
        for (uint64_t id : c) [ids addObject:@(id)];
        [parts addObject:[NSString stringWithFormat:@"[%@]", [ids componentsJoinedByString:@","]]];
    }
    return [NSString stringWithFormat:@"[%@]", [parts componentsJoinedByString:@","]];
}

// The panes of a shown tab laid out at its shares: each column's width, and
// each pane's height in its column, as a share of the whole.
void CheckLaidOut(MCTerminalPanel *panel, NSUInteger tab, const TerminalLayout::Layout &l) {
    NSArray<NSArray<TerminalView *> *> *cols = [panel columnsInTab:tab];
    GCHECK(cols.count == l.columns.size());
    if (cols.count != l.columns.size()) return;
    const CGFloat W = panel.bounds.size.width;
    CGFloat H = 0;
    for (TerminalView *t in cols[0]) H += t.frame.size.height + 1;
    H -= 1;
    for (NSUInteger c = 0; c < cols.count; c++) {
        const CGFloat w = cols[c].firstObject.frame.size.width;
        // The layout gives each pane its share of what the 1pt gaps leave.
        const double share = w / (W - (cols.count - 1));
        if (std::fabs(share - l.columns[c]) > 0.02) MCGuiNote(@"column %lu: %.3f, %.3f wanted", (unsigned long)c, share, l.columns[c]);
        GCHECK(std::fabs(share - l.columns[c]) <= 0.02);
        for (NSUInteger r = 0; r < cols[c].count && r < l.rows[c].size(); r++) {
            const double h = cols[c][r].frame.size.height / (H - (cols[c].count - 1));
            if (std::fabs(h - l.rows[c][r]) > 0.02) MCGuiNote(@"pane %lu,%lu: %.3f, %.3f wanted", (unsigned long)c, (unsigned long)r, h, l.rows[c][r]);
            GCHECK(std::fabs(h - l.rows[c][r]) <= 0.02);
        }
    }
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

    // ---------------------------------------------------- terminal splits in the record
    MCGuiStep(@"splits: a window with a terminal split right, then down in the new column", ^{
        gC = nil;
        gDirS = [MCGuiScratch() stringByAppendingPathComponent:@"reattach-splits"];
        [NSFileManager.defaultManager createDirectoryAtPath:gDirS withIntermediateDirectories:YES attributes:nil error:nil];
        gC = OpenWindowOn(gDirS);
        GCHECK(gC != nil);
        [gC newTerminal:nil];
        MCTerminalPanel *panel = PanelOf(gC);
        GCHECK(panel != nil);
        // A dock tall enough for a stack of two with room to move its bar.
        [gC setValue:@(3 * [MCTerminalPanel minPaneHeight] + MCTabStripHeight() + 60) forKey:@"terminalHeight"];
        [gC relayoutRightArea];
        MCGuiNote(@"window %@, dock %@", NSStringFromSize(gC.window.frame.size), NSStringFromSize(panel.frame.size));
        gA = panel.activeTerminal;
        gB = [panel splitActive];
        gC2 = [panel splitActiveDown];
        GCHECK(gA && gB && gC2);
        NSArray *cols = [panel columnsInTab:0];
        GCHECK(cols.count == 2 && [cols[0] isEqual:@[ gA ]] && [cols[1] isEqual:(@[ gB, gC2 ])]);
        GCHECK(gA.onSessionHost && gA.sessionTerminal && gB.sessionTerminal && gC2.sessionTerminal);
    });
    MCGuiWait(@"three prompts", 20, ^BOOL { return Idle(gA) && Idle(gB) && Idle(gC2); });
    MCGuiStep(@"the bar between the columns dragged: the host's record keeps the old shares till it is let go", ^{
        MCSessionWindow *w = [gC sessionWindow];
        [w report];
        MCTerminalPanel *panel = PanelOf(gC);
        DragBar *bar = nil;
        for (DragBar *b in [panel splitBars])
            if (b.vertical) bar = b;
        GCHECK(bar != nil);
        if (!bar) return;
        GCHECK(JsonSharesNear(HostRecord(w.windowId)["termLayout"]["tabs"][0]["columnShares"], {0.5, 0.5}));
        NSPoint a = [bar convertPoint:NSMakePoint(NSMidX(bar.bounds), NSMidY(bar.bounds)) toView:nil];
        const CGFloat by = 0.1 * panel.bounds.size.width;   // to about 60:40
        NSPoint b = NSMakePoint(a.x + by, a.y);
        [bar mouseDown:Mouse(gC.window, NSEventTypeLeftMouseDown, a)];
        [bar mouseDragged:Mouse(gC.window, NSEventTypeLeftMouseDragged, NSMakePoint(a.x + by / 2, a.y))];
        [bar mouseDragged:Mouse(gC.window, NSEventTypeLeftMouseDragged, b)];
        GCHECK(gA.frame.size.width > gB.frame.size.width + 0.1 * by);   // the panes moved
        [w report];   // what the half second's timer sends, mid-drag
        const Json mid = HostRecord(w.windowId)["termLayout"]["tabs"][0]["columnShares"];
        MCGuiNote(@"mid-drag, the host's record: %s", mid.dump().c_str());
        GCHECK(JsonSharesNear(mid, {0.5, 0.5}));
        [bar mouseUp:Mouse(gC.window, NSEventTypeLeftMouseUp, b)];
    });
    MCGuiWait(@"let go: the host's record has the new shares, without waiting for the timer", 2, ^BOOL {
        const Json shares = HostRecord([gC sessionWindow].windowId)["termLayout"]["tabs"][0]["columnShares"];
        return shares.isArray() && shares.size() == 2 && shares[0].asNumber(0) > 0.55;
    });
    MCGuiStep(@"the bar in the right column dragged down; its top pane active; a second tab on show", ^{
        MCTerminalPanel *panel = PanelOf(gC);
        DragBar *bar = nil;
        for (DragBar *b in [panel splitBars])
            if (!b.vertical) bar = b;
        GCHECK(bar != nil);
        if (bar) {
            NSPoint a = [bar convertPoint:NSMakePoint(NSMidX(bar.bounds), NSMidY(bar.bounds)) toView:nil];
            NSPoint b = NSMakePoint(a.x, a.y - 0.15 * (panel.bounds.size.height - MCTabStripHeight()));
            [bar mouseDown:Mouse(gC.window, NSEventTypeLeftMouseDown, a)];
            [bar mouseDragged:Mouse(gC.window, NSEventTypeLeftMouseDragged, b)];
            [bar mouseUp:Mouse(gC.window, NSEventTypeLeftMouseUp, b)];
        }
        GCHECK(gB.frame.size.height > gC2.frame.size.height + 10);
        [panel selectTerminal:gB];
        [gC newTerminal:nil];
        gD = panel.activeTerminal;
        GCHECK(gD && gD != gC2 && panel.tabCount == 2 && panel.selectedTab == 1);
        gIdA = gA.sessionTerminal;
        gIdB = gB.sessionTerminal;
        gIdC = gC2.sessionTerminal;
        gIdD = gD.sessionTerminal;
        gShellA = gA.shellPid;
        [panel sessionDock:&gDockBefore];
        GCHECK(gDockBefore.tabs.size() == 2);
        if (gDockBefore.tabs.size() != 2) return;
        const Shared::TermDock::Tab &t = gDockBefore.tabs[0];
        GCHECK((t.columns == Columns{{gIdA}, {gIdB, gIdC}}));
        GCHECK(t.activeColumn == 1 && t.activeRow == 0);
        GCHECK(t.layout.columns[0] > 0.55 && t.layout.rows[1][0] > 0.55);
        GCHECK(gDockBefore.activeTab == 1);
        MCGuiNote(@"shares: columns %.4f %.4f, right column %.4f %.4f", t.layout.columns[0], t.layout.columns[1],
                  t.layout.rows[1][0], t.layout.rows[1][1]);
    });
    MCGuiWait(@"the new tab's prompt", 20, ^BOOL { return Idle(gD); });
    MCGuiStep(@"the window's record has the layout beside its terminals", ^{
        const Json r = [[gC sessionWindow] recordLeavingOut:nil];
        MCGuiNote(@"record: %s", r.dump().c_str());
        GCHECK(r["terms"].size() == 4);
        const Json &l = r["termLayout"];
        GCHECK(l["tabs"].size() == 2 && l["activeTab"].asInt() == 1);
        Json left = Json::array(), right = Json::array(), cols = Json::array();
        left.push(Json((int64_t)gIdA));
        right.push(Json((int64_t)gIdB));
        right.push(Json((int64_t)gIdC));
        cols.push(left);
        cols.push(right);
        GCHECK(l["tabs"][0]["columns"].dump() == cols.dump());
        if (gDockBefore.tabs.size() != 2) return;
        GCHECK(l["tabs"][0]["activeColumn"].asInt() == 1 && l["tabs"][0]["activeRow"].asInt() == 0);
        GCHECK(JsonSharesNear(l["tabs"][0]["columnShares"], gDockBefore.tabs[0].layout.columns));
        GCHECK(JsonSharesNear(l["tabs"][0]["rowShares"][1], gDockBefore.tabs[0].layout.rows[1]));
        GCHECK(l["tabs"][1]["columns"][0][0].asInt() == (int64_t)gIdD);
    });
    MCGuiStep(@"the window closes", ^{
        gA = gB = gC2 = gD = nil;
        [gC.window performClose:nil];
    });
    MCGuiWait(@"gone", 10, ^BOOL { return WindowOn(gDirS) == nil; });
    MCGuiStep(@"the folder's next window: the same tabs, panes, shares, active pane and tab", ^{
        gC = OpenWindowOn(gDirS);
        GCHECK(gC != nil);
        MCTerminalPanel *panel = PanelOf(gC);
        GCHECK(panel != nil && panel.tabCount == 2);
        if (!panel || panel.tabCount != 2) return;
        MCGuiNote(@"tabs %@ %@", ColumnsText(IdsIn(panel, 0)), ColumnsText(IdsIn(panel, 1)));
        GCHECK((IdsIn(panel, 0) == Columns{{gIdA}, {gIdB, gIdC}}));
        GCHECK((IdsIn(panel, 1) == Columns{{gIdD}}));
        GCHECK(panel.selectedTab == 1);
        if (gDockBefore.tabs.size() != 2) return;
        Shared::TermDock now;
        [panel sessionDock:&now];
        const Shared::TermDock::Tab &t = now.tabs[0], &was = gDockBefore.tabs[0];
        GCHECK(SharesNear(t.layout.columns, was.layout.columns));
        GCHECK(t.layout.rows.size() == 2 && SharesNear(t.layout.rows[1], was.layout.rows[1]));
        GCHECK(t.activeColumn == 1 && t.activeRow == 0);
        GCHECK(now.activeTab == 1);
        NSArray<NSArray<TerminalView *> *> *cols = [panel columnsInTab:0];
        gA = cols[0][0];
        GCHECK(gA.shellPid == gShellA);
        // Laid out at those shares when its tab is shown.
        [gC setValue:@(3 * [MCTerminalPanel minPaneHeight] + MCTabStripHeight() + 60) forKey:@"terminalHeight"];
        [gC relayoutRightArea];
        [panel selectTab:0];
        GCHECK(panel.activeTerminal == cols[1][0]);
        CheckLaidOut(panel, 0, was.layout);
        [panel selectTab:1];
    });
    MCGuiWait(@"the top left pane's replay", 20, ^BOOL { return [gA recentText:200].length > 0 && gA.atPrompt; });

    // A record as GTK writes it: another arrangement of the same terminals,
    // a pane naming one that is not there (it drops out, its share to its
    // neighbour), and the active pane on it (the keyboard's next: below).
    __block uint64_t splitsRecord = 0;
    MCGuiStep(@"closed again", ^{
        gA = nil;
        splitsRecord = [gC sessionWindow].windowId;
        [gC.window performClose:nil];
    });
    MCGuiWait(@"gone", 10, ^BOOL { return WindowOn(gDirS) == nil; });
    MCGuiStep(@"the record rewritten as GTK writes it", ^{
        gC = nil;
        const uint64_t window = splitsRecord;
        Json tabs = HostRecord(window);
        GCHECK(tabs.isObject() && tabs["detached"].asBool() && tabs.has("termLayout"));
        if (!tabs.isObject()) return;
        NSString *text = [NSString stringWithFormat:
            @"{\"tabs\":[{\"columns\":[[%llu,%llu],[999999,%llu]],\"columnShares\":[0.3,0.7],"
            @"\"rowShares\":[[0.6,0.4],[0.5,0.5]],\"activeColumn\":1,\"activeRow\":0},"
            @"{\"columns\":[[%llu]],\"columnShares\":[1],\"rowShares\":[[1]],\"activeColumn\":0,\"activeRow\":0}],"
            @"\"activeTab\":0}",
            (unsigned long long)gIdC, (unsigned long long)gIdA, (unsigned long long)gIdD, (unsigned long long)gIdB];
        Json layout;
        std::string error;
        GCHECK(Json::parse(text.UTF8String, layout, &error));
        Json rewritten = Json::object();
        for (const auto &[key, value] : tabs.items())
            if (key != "termLayout") rewritten.set(key, value);
        rewritten.set("termLayout", layout);
        MCSessionClient().windowTabs(window, rewritten);
        GCHECK(MCSessionClient().settle());
        GCHECK(HostRecord(window)["termLayout"]["tabs"][0]["columns"][1][0].asInt() == 999999);
    });
    MCGuiStep(@"the next window has GTK's arrangement, the missing pane gone", ^{
        gC = OpenWindowOn(gDirS);
        GCHECK(gC != nil);
        MCTerminalPanel *panel = PanelOf(gC);
        GCHECK(panel != nil && panel.tabCount == 2);
        if (!panel || panel.tabCount != 2) return;
        MCGuiNote(@"tabs %@ %@", ColumnsText(IdsIn(panel, 0)), ColumnsText(IdsIn(panel, 1)));
        GCHECK((IdsIn(panel, 0) == Columns{{gIdC, gIdA}, {gIdD}}));
        GCHECK((IdsIn(panel, 1) == Columns{{gIdB}}));
        GCHECK(panel.selectedTab == 0);
        Shared::TermDock now;
        [panel sessionDock:&now];
        const Shared::TermDock::Tab &t = now.tabs[0];
        GCHECK(SharesNear(t.layout.columns, {0.3, 0.7}));
        GCHECK(t.layout.rows.size() == 2 && SharesNear(t.layout.rows[0], {0.6, 0.4}) && SharesNear(t.layout.rows[1], {1}));
        GCHECK(t.activeColumn == 1 && t.activeRow == 0);
        GCHECK(panel.activeTerminal.sessionTerminal == gIdD);
        [gC setValue:@(3 * [MCTerminalPanel minPaneHeight] + MCTabStripHeight() + 60) forKey:@"terminalHeight"];
        [gC relayoutRightArea];
        CheckLaidOut(panel, 0, t.layout);
    });
    MCGuiStep(@"the window closes", ^{ [gC.window performClose:nil]; });
    MCGuiWait(@"gone", 10, ^BOOL { return WindowOn(gDirS) == nil; });

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
        for (MCTab *t in tabs) MCGuiNote(@"tab %@ (document %llu)", t.currentPath, t.session.docId);
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
        const Json st = l ? l->client().sessionState() : Json();
        for (const Json &doc : st["documents"].elements())
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
        GCHECK(u != nil);
        if (!u) return;   // nothing else to save
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

    // ---------------------------------------------------- stage 7: saving through the host
    MCGuiStep(@"a file typed into in the window; a page's guest connects over the host's socket", ^{
        gFileE = [gDirC stringByAppendingPathComponent:@"e.txt"];
        MCGuiWriteFile(gFileE, @"epsilon\n");
        GCHECK([gC showFile:gFileE]);
        TypeAtEnd(gC, @"typed-e\n");
        MCTab *e = TabFor(gC, gFileE);
        GCHECK(e.dirty && e.session.hostSavesFile);
        gDocE = e.session.docId;
        std::shared_ptr<MCSession::Link> l = LinkOf(gC);
        GCHECK(l && l->client().settle());
        std::string error;
        gGuestLink = Shared::HostLink::open(gSocketC, &error);
        GCHECK(gGuestLink != nullptr);
        if (!gGuestLink) return;
        gGuest = std::make_unique<Shared::Client>(Model::Unit::Utf16, "page");
        gGuest->setGuest(true);
        gGuest->hearWrites([](Shared::DocId doc, std::uint64_t, const Json &j) { gGuestWrites.push_back({doc, j}); });
        gGuestLink->connect(*gGuest);
        GCHECK(gGuest->settle() && gGuest->hostSaves());
        static Quiet quiet;
        gGuest->view(gDocE, &quiet);
        GCHECK(gGuest->settle());
        GCHECK(HostDirty(*gGuest, gDocE));   // the window's dot and the host's count agree
    });
    MCGuiStep(@"the page saves it: the host writes, though the window has the file open", ^{
        if (!gGuest) return;
        const std::uint64_t req = gGuest->requestSave(gDocE, 0, false);
        GCHECK(req != 0 && gGuest->settle());
        Shared::Client::SaveAnswer a;
        GCHECK(gGuest->saveAnswer(req, &a) && a.ok);
        if (!a.ok) MCGuiNote(@"the page's save: %s", a.json.dump().c_str());
        GCHECK_EQ_STR([NSString stringWithContentsOfFile:gFileE encoding:NSUTF8StringEncoding error:nil],
                      @"epsilon\ntyped-e\n");
    });
    MCGuiWait(@"the window's tab follows: clean, its title too", 10, ^BOOL {
        MCTab *e = TabFor(gC, gFileE);
        return e && !e.dirty && !gC.window.documentEdited && [e.savedText isEqualToString:@"epsilon\ntyped-e\n"];
    });
    MCGuiStep(@"in step with the file as the host wrote it; the host counts it saved", ^{
        MCTab *e = TabFor(gC, gFileE);
        GCHECK(![gC tab:e changedOnDisk:MCStampOfFile(gFileE)]);
        if (gGuest) GCHECK(!HostDirty(*gGuest, gDocE));
    });
    MCGuiStep(@"the window types and saves: no question, and the host wrote it", ^{
        TypeAtEnd(gC, @"more\n");
        MCTab *e = TabFor(gC, gFileE);
        GCHECK(e.dirty);
        gWritesBefore = gGuestWrites.size();
        const NSUInteger alerts = MCGuiAlertsShown().count;
        [gC saveCurrentFile:nil];
        GCHECK(MCGuiAlertsShown().count == alerts);
        GCHECK(!e.dirty);
        GCHECK_EQ_STR([NSString stringWithContentsOfFile:gFileE encoding:NSUTF8StringEncoding error:nil],
                      @"epsilon\ntyped-e\nmore\n");
    });
    MCGuiWait(@"the page hears the host's write, with the file's stamp", 10, ^BOOL {
        if (!gGuest) return YES;
        gGuest->dispatch();
        return gGuestWrites.size() > gWritesBefore;
    });
    MCGuiStep(@"its stamp is the file's; the host counts it saved", ^{
        if (!gGuest || gGuestWrites.size() <= gWritesBefore) return;
        const auto &w = gGuestWrites.back();
        GCHECK(w.first == gDocE);
        GCHECK(Shared::stampFromJson(w.second["stamp"]) == FolderSearch::stampOf(gFileE.fileSystemRepresentation));
        GCHECK(!HostDirty(*gGuest, gDocE));
    });
    MCGuiStep(@"a UTF-16 file saved from the window stays UTF-16 with its mark", ^{
        gFileF = [gDirC stringByAppendingPathComponent:@"f16.txt"];
        const unsigned char le[] = {0xFF, 0xFE, 'f', 0, '\n', 0};
        [[NSData dataWithBytes:le length:sizeof le] writeToFile:gFileF atomically:YES];
        GCHECK([gC showFile:gFileF]);
        TypeAtEnd(gC, @"g\n");
        [gC saveCurrentFile:nil];
        const unsigned char want[] = {0xFF, 0xFE, 'f', 0, '\n', 0, 'g', 0, '\n', 0};
        GCHECK([Bytes(gFileF) isEqualToData:[NSData dataWithBytes:want length:sizeof want]]);
        MCTab *f = TabFor(gC, gFileF);
        GCHECK(f && !f.dirty && f.encoding == MCTextEncodingUtf16LE && f.bom);
    });
    MCGuiStep(@"an agent's write, then the window's save: Keep Mine, and the host writes over it", ^{
        GCHECK([gC showFile:gFileE]);
        MCTab *e = TabFor(gC, gFileE);
        // Another size than the window's save left: changed, whatever the clock.
        GCHECK([@"an agent's edit\n" writeToFile:gFileE atomically:NO encoding:NSUTF8StringEncoding error:nil]);
        TypeAtEnd(gC, @"mine\n");
        MCGuiAnswerAlerts(@[ @(NSAlertFirstButtonReturn) ]);   // Keep Mine
        const NSUInteger alerts = MCGuiAlertsShown().count;
        [gC saveCurrentFile:nil];
        GCHECK(MCGuiAlertsShown().count == alerts + 1);
        GCHECK_EQ_STR([NSString stringWithContentsOfFile:gFileE encoding:NSUTF8StringEncoding error:nil],
                      @"epsilon\ntyped-e\nmore\nmine\n");
        GCHECK(!e.dirty);
        if (gGuest) GCHECK(!HostDirty(*gGuest, gDocE));
    });
    MCGuiStep(@"the page's guest goes", ^{
        if (gGuestLink) gGuestLink->close();
        gGuestLink.reset();
        gGuest.reset();
        // The window's tabs of the files saved here are clean: only the
        // question's two unsaved files remain.
        for (NSString *path in @[ gFileE, gFileF ]) GCHECK(!TabFor(gC, path).dirty);
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
