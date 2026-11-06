// AgentsSuite.mm — `make guitest SUITES=agents`: the Agents panel
// (Shift+Cmd+A), the Evidence view, the Worktrees node in the file tree with
// Compare with Main and Open in New Window, and terminal paths resolved
// through the folder's worktrees and scratchpads.
//
// The world is scratch: a git repository with a worktree where Claude Code
// would make one, and a transcripts folder and a temp root built from
// tests/fixtures/claude (the paths in them rewritten to the scratch ones),
// named through MINICODE_CLAUDE_PROJECTS and MINICODE_CLAUDE_TMP. The real
// ~/.claude and /private/tmp/claude-<uid> are never read or written.
#import "Harness.h"
#import "EditorController.h"
#import "AgentModel.h"
#import "AgentPages.h"
#import "AgentWindow.h"
#import "AgentsPanel.h"
#import "CompareView.h"
#import "ControlServer.h"
#import "GitPanel.h"
#import "LspPicker.h"
#import "Terminal.h"
#import "TerminalPanel.h"
#include <string>
#include <sys/stat.h>
#include <sys/time.h>

// Private methods the checks reach.
@interface TerminalView (AgentsSuite)
- (MCTermLink *)linkInLine:(const std::string &)line atByte:(size_t)byte;
@end
@interface EditorController (AgentsSuite) <MCAgentWorkHost>
- (void)compareWorktreeWithMain:(id)sender;
- (void)openWorktreeInNewWindow:(id)sender;
- (void)toggleHiddenFiles:(id)sender;
@end

namespace {

NSString *gBase, *gProj, *gReal, *gProjects, *gTmp, *gSessDir, *gWorktree, *gWorktreeReal;
NSString *const kSid = @"1e1f578c-45c6-4cb5-92e5-84e11a306454";
NSString *const kAgent = @"a1ad4e3e0a7ec7fd2";

NSString *Json(id obj) {
    NSData *d = [NSJSONSerialization dataWithJSONObject:obj options:0 error:nil];
    return [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
}

NSString *Stamp(NSTimeInterval ago) {
    NSISO8601DateFormatter *f = [NSISO8601DateFormatter new];
    f.formatOptions = NSISO8601DateFormatWithInternetDateTime | NSISO8601DateFormatWithFractionalSeconds;
    return [f stringFromDate:[NSDate dateWithTimeIntervalSinceNow:-ago]];
}

NSMutableDictionary *Head(NSString *type, NSString *sid, NSTimeInterval ago) {
    return [@{@"parentUuid": NSNull.null, @"isSidechain": @NO, @"type": type,
              @"timestamp": Stamp(ago), @"cwd": gReal, @"sessionId": sid,
              @"version": @"9.9.9", @"gitBranch": @"main"} mutableCopy];
}
NSString *Prompt(NSString *sid, NSTimeInterval ago, NSString *text) {
    NSMutableDictionary *d = Head(@"user", sid, ago);
    d[@"message"] = @{@"role": @"user", @"content": text};
    return Json(d);
}
NSString *Use(NSString *sid, NSTimeInterval ago, NSString *ident, NSString *name, NSDictionary *input) {
    NSMutableDictionary *d = Head(@"assistant", sid, ago);
    d[@"message"] = @{@"id": [@"msg_" stringByAppendingString:ident], @"role": @"assistant",
                      @"content": @[@{@"type": @"tool_use", @"id": ident, @"name": name, @"input": input}]};
    return Json(d);
}
NSString *Result(NSString *sid, NSTimeInterval ago, NSString *ident, NSString *content, BOOL error) {
    NSMutableDictionary *d = Head(@"user", sid, ago);
    d[@"message"] = @{@"role": @"user",
                      @"content": @[@{@"tool_use_id": ident, @"type": @"tool_result",
                                      @"content": content, @"is_error": @(error)}]};
    return Json(d);
}
NSString *Reply(NSString *sid, NSTimeInterval ago, NSString *ident, NSString *text) {
    NSMutableDictionary *d = Head(@"assistant", sid, ago);
    d[@"message"] = @{@"id": ident, @"role": @"assistant",
                      @"content": @[@{@"type": @"text", @"text": text}]};
    return Json(d);
}

void Append(NSString *path, NSString *lines) {
    NSFileHandle *h = [NSFileHandle fileHandleForWritingAtPath:path];
    [h seekToEndOfFile];
    [h writeData:[lines dataUsingEncoding:NSUTF8StringEncoding]];
    [h closeFile];
}

void SetAge(NSString *path, NSTimeInterval ago) {
    struct timeval tv[2];
    tv[0].tv_sec = tv[1].tv_sec = (time_t)([NSDate date].timeIntervalSince1970 - ago);
    tv[0].tv_usec = tv[1].tv_usec = 0;
    utimes(path.fileSystemRepresentation, tv);
}

BOOL Git(NSString *dir, NSArray<NSString *> *args) {
    MCGitResult *r = MCGitRunSync(dir, args);
    if (!r.ok) MCGuiNote(@"git %@ failed: %@", [args componentsJoinedByString:@" "], r.errorText);
    return r.ok;
}

NSString *Lines(int from, int to, NSString *prefix) {
    NSMutableString *s = [NSMutableString string];
    for (int i = from; i <= to; i++) [s appendFormat:@"%@ line %d\n", prefix, i];
    return s;
}

// The scratch world (see the top of the file).
void BuildWorld() {
    NSFileManager *fm = NSFileManager.defaultManager;
    gBase = [MCGuiScratch() stringByAppendingPathComponent:@"agents"];
    gProj = [gBase stringByAppendingPathComponent:@"proj"];
    gProjects = [gBase stringByAppendingPathComponent:@"claude/projects"];
    gTmp = [gBase stringByAppendingPathComponent:@"claude-tmp"];
    [fm createDirectoryAtPath:gProj withIntermediateDirectories:YES attributes:nil error:nil];
    [fm createDirectoryAtPath:gTmp withIntermediateDirectories:YES attributes:nil error:nil];
    gReal = gProj.stringByResolvingSymlinksInPath;
    gTmp = gTmp.stringByResolvingSymlinksInPath;
    char buf[PATH_MAX];
    if (realpath(gProj.fileSystemRepresentation, buf)) gReal = @(buf);
    if (realpath(gTmp.fileSystemRepresentation, buf)) gTmp = @(buf);

    // The repository: main has src/Terminal.mm and src/only-main.txt; the
    // worktree changes Terminal.mm and adds LspClient.h in a commit, and has
    // an untracked notes.md.
    MCGuiWriteFile([gProj stringByAppendingPathComponent:@"src/Terminal.mm"], Lines(1, 1500, @"main"));
    MCGuiWriteFile([gProj stringByAppendingPathComponent:@"scripts/minicode-launcher"], @"#!/bin/sh\n");
    MCGuiWriteFile([gProj stringByAppendingPathComponent:@".gitignore"], @".claude/\n");
    Git(gProj, @[@"init", @"-q", @"-b", @"main"]);
    Git(gProj, @[@"add", @"-A"]);
    Git(gProj, @[@"commit", @"-q", @"-m", @"first"]);
    gWorktree = [gProj stringByAppendingFormat:@"/.claude/worktrees/agent-%@", kAgent];
    gWorktreeReal = [gReal stringByAppendingFormat:@"/.claude/worktrees/agent-%@", kAgent];
    Git(gProj, @[@"worktree", @"add", @"-q", @"-b", [@"worktree-agent-" stringByAppendingString:kAgent],
                 gWorktree]);
    MCGuiWriteFile([gWorktree stringByAppendingPathComponent:@"src/Terminal.mm"],
                   Lines(1, 1500, @"worktree"));
    MCGuiWriteFile([gWorktree stringByAppendingPathComponent:@"src/LspClient.h"], @"// client\n");
    MCGuiWriteFile([gWorktree stringByAppendingPathComponent:@"src/LspPicker.h"], @"// picker\n");
    Git(gWorktree, @[@"add", @"src"]);
    Git(gWorktree, @[@"commit", @"-q", @"-m", @"agent work"]);
    MCGuiWriteFile([gWorktree stringByAppendingPathComponent:@"notes.md"], @"# notes\n");
    // Only in the folder, made after the worktree left it.
    MCGuiWriteFile([gProj stringByAppendingPathComponent:@"src/only-main.txt"], @"only here\n");
    // The worktree's copy of Terminal.mm is the newer one.
    SetAge([gProj stringByAppendingPathComponent:@"src/Terminal.mm"], 3600);

    // The transcripts: the fixtures, with their paths moved here.
    NSString *fixtures = [MCGuiBuildDir().stringByDeletingLastPathComponent
        stringByAppendingPathComponent:@"tests/fixtures/claude/projects/-Users-user-Code-public-minicode"];
    gSessDir = [gProjects stringByAppendingPathComponent:@"-agents-proj"];
    NSDirectoryEnumerator *e = [fm enumeratorAtPath:fixtures];
    for (NSString *rel in e) {
        NSString *src = [fixtures stringByAppendingPathComponent:rel];
        BOOL isDir = NO;
        [fm fileExistsAtPath:src isDirectory:&isDir];
        if (isDir) continue;
        NSString *text = [NSString stringWithContentsOfFile:src encoding:NSUTF8StringEncoding error:nil];
        text = [text stringByReplacingOccurrencesOfString:@"/Users/user/Code/public/minicode"
                                               withString:gReal];
        text = [text stringByReplacingOccurrencesOfString:@"/private/tmp/claude-501" withString:gTmp];
        MCGuiWriteFile([gSessDir stringByAppendingPathComponent:rel], text);
    }
    // Both fixture transcripts were last written an hour ago: finished.
    NSString *mainT = [gSessDir stringByAppendingFormat:@"/%@.jsonl", kSid];
    SetAge(mainT, 3600);
    SetAge([gSessDir stringByAppendingFormat:@"/%@/subagents/agent-%@.jsonl", kSid, kAgent], 3600);
    // The main session's scratchpad exists; the file the subagent wrote
    // there does not (struck through in the panel).
    [fm createDirectoryAtPath:[gTmp stringByAppendingFormat:@"/-Users-user-Code-public-minicode/%@/scratchpad", kSid]
  withIntermediateDirectories:YES attributes:nil error:nil];

    // A session whose last test run failed while its last message says all
    // is well (the spec's acceptance check for R3.3).
    NSString *fail = @"sess-fail";
    NSMutableString *f = [NSMutableString string];
    [f appendFormat:@"%@\n", Prompt(fail, 900, @"Make the parser faster")];
    [f appendFormat:@"%@\n", Use(fail, 890, @"t1", @"Bash",
                                 @{@"command": @"make test 2>&1 | tail -3", @"description": @"Run the tests"})];
    [f appendFormat:@"%@\n", Result(fail, 880, @"t1", @"building\n12 passed, 0 failed", NO)];
    [f appendFormat:@"%@\n", Use(fail, 870, @"t2", @"Bash",
                                 @{@"command": @"make test", @"description": @"Run the tests again"})];
    [f appendFormat:@"%@\n", Result(fail, 860, @"t2", @"building\nFAIL parser\n10 passed, 2 failed", YES)];
    [f appendFormat:@"%@\n", Reply(fail, 850, @"m9", @"Done: the parser is faster and all tests pass.")];
    NSString *failT = [gSessDir stringByAppendingPathComponent:@"sess-fail.jsonl"];
    MCGuiWriteFile(failT, f);
    SetAge(failT, 850);

    // A session with a large output (about 1.3 MB, kept in the transcript)
    // and a command to type into a terminal.
    NSString *big = @"sess-big";
    NSMutableString *b = [NSMutableString string];
    [b appendFormat:@"%@\n", Prompt(big, 700, @"Count to a lot")];
    [b appendFormat:@"%@\n", Use(big, 690, @"b1", @"Bash",
                                 @{@"command": @"seq 1 120000 | sed 's/^/number /'",
                                   @"description": @"Print many lines"})];
    [b appendFormat:@"%@\n", Result(big, 680, @"b1", Lines(1, 120000, @"number"), NO)];
    [b appendFormat:@"%@\n", Use(big, 670, @"b2", @"Bash",
                                 @{@"command": @"echo agents-typed-$((6*7))",
                                   @"description": @"Say the answer"})];
    [b appendFormat:@"%@\n", Result(big, 660, @"b2", @"agents-typed-42", NO)];
    [b appendFormat:@"%@\n", Reply(big, 650, @"m8", @"Counted.")];
    NSString *bigT = [gSessDir stringByAppendingPathComponent:@"sess-big.jsonl"];
    MCGuiWriteFile(bigT, b);
    SetAge(bigT, 650);

    setenv("MINICODE_CLAUDE_PROJECTS", gProjects.fileSystemRepresentation, 1);
    setenv("MINICODE_CLAUDE_TMP", gTmp.fileSystemRepresentation, 1);
}

NSString *Real(NSString *p) {
    char buf[PATH_MAX];
    return p && realpath(p.fileSystemRepresentation, buf) ? @(buf) : p;
}

EditorController *Win() {
    for (EditorController *c in MCGuiControllers())
        if ([c.rootPath isEqualToString:gProj]) return c;
    return nil;
}
MCAgentWindow *Agents(EditorController *c) { return [c valueForKey:@"agents"]; }
MCAgentsPanel *Panel(EditorController *c) { return Agents(c).panel; }

NSInteger RowWith(MCAgentsPanel *p, NSString *text) {
    NSArray<NSString *> *rows = [p rowDescriptions];
    for (NSUInteger i = 0; i < rows.count; i++)
        if ([rows[i] containsString:text]) return (NSInteger)i;
    return -1;
}

NSDictionary *ActiveTab(EditorController *c) {
    for (NSDictionary *t in [c controlTabs])
        if ([t[@"active"] boolValue]) return t;
    return nil;
}

id ActivePage(EditorController *c) {
    id tab = [c valueForKey:@"activeTab"];
    return [tab valueForKey:@"page"];
}

NSEvent *Key(NSWindow *w, unsigned short code, NSString *chars) {
    return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0
                           timestamp:NSProcessInfo.processInfo.systemUptime
                        windowNumber:w.windowNumber context:nil characters:chars
         charactersIgnoringModifiers:chars isARepeat:NO keyCode:code];
}

NSString *Arrow(unichar c) { return [NSString stringWithCharacters:&c length:1]; }

void CollectKeys(NSMenu *menu, NSMutableDictionary<NSString *, NSMutableArray *> *out) {
    for (NSMenuItem *item in menu.itemArray) {
        if (item.submenu) CollectKeys(item.submenu, out);
        if (!item.keyEquivalent.length || item.isAlternate) continue;
        NSEventModifierFlags m = item.keyEquivalentModifierMask &
            (NSEventModifierFlagCommand | NSEventModifierFlagShift | NSEventModifierFlagOption |
             NSEventModifierFlagControl);
        NSString *key = item.keyEquivalent;
        if (![key.lowercaseString isEqualToString:key]) {
            m |= NSEventModifierFlagShift;
            key = key.lowercaseString;
        }
        NSString *k = [NSString stringWithFormat:@"%lu:%@", (unsigned long)m, key];
        if (!out[k]) out[k] = [NSMutableArray array];
        [out[k] addObject:item.title];
    }
}

NSMenuItem *MenuItem(NSMenu *menu, NSString *title) {
    for (NSMenuItem *item in menu.itemArray) {
        if ([item.title isEqualToString:title]) return item;
        if (item.submenu)
            if (NSMenuItem *found = MenuItem(item.submenu, title)) return found;
    }
    return nil;
}

// The longest the main thread went without a turn while `until` was false.
struct Gaps {
    CFAbsoluteTime last = 0;
    double worst = 0;
};

}  // namespace

MC_GUI_SUITE(agents) {
    __block EditorController *c = nil;
    __block CFAbsoluteTime appendedAt = 0;
    __block double liveDelay = -1;
    __block NSTimer *gapTimer = nil;
    __block Gaps *gaps = new Gaps();
    __block TerminalView *term = nil;
    __block NSUInteger windowsBefore = 0;

    MCGuiStep(@"a scratch repository with a worktree, and transcripts from the fixtures", ^{
        BuildWorld();
        GCHECK([NSFileManager.defaultManager fileExistsAtPath:[gWorktree stringByAppendingPathComponent:@"src/LspClient.h"]]);
        GCHECK(([NSFileManager.defaultManager fileExistsAtPath:[gSessDir stringByAppendingFormat:@"/%@.jsonl", kSid]]));
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:gProj];
        c = Win();
        GCHECK(c != nil);
        MCGuiSetKeyWindow(c.window);
    });

    // ------------------------------------------------------------- the key
    MCGuiStep(@"Shift+Cmd+A is View > Toggle Agents and no other item's key", ^{
        NSMutableDictionary<NSString *, NSMutableArray *> *keys = [NSMutableDictionary dictionary];
        CollectKeys(NSApp.mainMenu, keys);
        NSString *k = [NSString stringWithFormat:@"%lu:a",
                       (unsigned long)(NSEventModifierFlagCommand | NSEventModifierFlagShift)];
        GCHECK([keys[k] isEqualToArray:@[@"Toggle Agents"]]);
        for (NSString *key in keys)
            if ([keys[key] count] > 1) MCGuiNote(@"shared key %@: %@", key, keys[key]);
        // The item's action through the responder chain, as the menu sends it
        // (an app that is never active gets no menu key equivalents here).
        NSMenuItem *item = MenuItem(NSApp.mainMenu, @"Toggle Agents");
        GCHECK(item != nil && [(id)NSApp.delegate validateMenuItem:item]);
        GCHECK([NSApp sendAction:item.action to:item.target from:item]);
        GCHECK([[c valueForKey:@"agentsVisible"] boolValue]);
        GCHECK(Panel(c).superview != nil && Panel(c).window == c.window);
        GCHECK(Panel(c).frame.size.width > 100);
    });

    // ----------------------------------------------------------- the rows
    MCGuiWait(@"the sessions are listed", 10, ^BOOL {
        return RowWith(Panel(c), @"Placeholder request for the main session.") >= 0 &&
               RowWith(Panel(c), @"Make the parser faster") >= 0 &&
               RowWith(Panel(c), @"Count to a lot") >= 0;
    });

    MCGuiStep(@"rows: newest first, state, branch, age and counts", ^{
        MCAgentsPanel *p = Panel(c);
        NSArray<NSString *> *rows = [p rowDescriptions];
        if (rows.count != 3) MCGuiNote(@"rows: %@", rows);
        GCHECK(rows.count == 3);   // subagents only show under their parent
        GCHECK(RowWith(p, @"Count to a lot") < RowWith(p, @"Make the parser faster"));
        GCHECK(RowWith(p, @"Make the parser faster") < RowWith(p, @"Placeholder request"));
        NSString *main = rows[(NSUInteger)RowWith(p, @"Placeholder request")];
        GCHECK([main hasPrefix:@"S 0 finished "]);
        GCHECK([main containsString:@"1 file, 1 command"]);
        GCHECK([main containsString:@"· main ·"]);
        GCHECK_EQ_STR(p.summaryText, @"4 sessions");
        GCHECK_EQ_STR(p.noticeText, @"");
        NSMenu *m = [p menuForRow:RowWith(p, @"Placeholder request")];
        GCHECK([m itemWithTitle:@"Show Evidence"] != nil);
        GCHECK([m itemWithTitle:@"Adjust Settings…"] != nil);
        MCGuiSnapshot(p, @"agents-panel");
    });

    MCGuiStep(@"Space and Right expand, subagents nest, files group by place", ^{
        MCAgentsPanel *p = Panel(c);
        NSOutlineView *ov = p.outline;
        [p focusList];
        NSInteger mainRow = RowWith(p, @"Placeholder request");
        [ov selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)mainRow] byExtendingSelection:NO];
        [ov keyDown:Key(c.window, 49, @" ")];   // Space
        NSArray<NSString *> *rows = [p rowDescriptions];
        NSInteger sub = RowWith(p, @"S 1 finished LSP rename, references, quick fixes");
        GCHECK(sub > mainRow);
        GCHECK(RowWith(p, @"G In the folder") > mainRow);
        GCHECK(RowWith(p, @"scripts/minicode-launcher") < 0);   // the group is closed
        // Right on the subagent opens it.
        [ov selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)sub] byExtendingSelection:NO];
        [ov keyDown:Key(c.window, 124, Arrow(NSRightArrowFunctionKey))];
        rows = [p rowDescriptions];
        NSInteger inWt = RowWith(p, @"G In its worktree");
        NSInteger outside = RowWith(p, @"G Outside the folder");
        GCHECK(inWt > sub && outside > inWt);
        for (NSInteger r = (NSInteger)rows.count - 1; r > sub; r--)   // open the groups
            if ([rows[(NSUInteger)r] hasPrefix:@"G "]) [ov expandItem:[ov itemAtRow:r]];
        rows = [p rowDescriptions];
        NSInteger client = RowWith(p, [gWorktreeReal stringByAppendingPathComponent:@"src/LspClient.h"]);
        NSInteger picker = RowWith(p, [gWorktreeReal stringByAppendingPathComponent:@"src/LspPicker.h"]);
        NSInteger pad = RowWith(p, @"scratchpad/edit1.py (missing)");
        GCHECK(client > RowWith(p, @"G In its worktree") && picker > client);
        GCHECK(pad > RowWith(p, @"G Outside the folder"));
        GCHECK(RowWith(p, @"src/LspClient.h (missing)") < 0);
        MCGuiSnapshot(p, @"agents-panel-expanded");
    });

    MCGuiStep(@"Return on a file opens it; a missing one opens nothing", ^{
        MCAgentsPanel *p = Panel(c);
        NSOutlineView *ov = p.outline;
        NSInteger client = RowWith(p, @"src/LspClient.h");
        [ov selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)client] byExtendingSelection:NO];
        [ov keyDown:Key(c.window, 36, @"\r")];
        GCHECK([[ActiveTab(c)[@"path"] description] hasSuffix:@"src/LspClient.h"]);
        NSUInteger tabs = [c controlTabs].count;
        [p activateRow:RowWith(p, @"edit1.py (missing)")];
        GCHECK([c controlTabs].count == tabs);
    });

    // ------------------------------------------------------------ evidence
    MCGuiStep(@"Return on the subagent shows its Evidence in a tab", ^{
        MCAgentsPanel *p = Panel(c);
        NSOutlineView *ov = p.outline;
        NSInteger sub = RowWith(p, @"LSP rename, references, quick fixes");
        [ov selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)sub] byExtendingSelection:NO];
        [ov keyDown:Key(c.window, 36, @"\r")];
        GCHECK_EQ_STR(ActiveTab(c)[@"kind"], @"evidence");
        GCHECK([ActiveTab(c)[@"title"] containsString:@"LSP rename"]);
        GCHECK([ActivePage(c) isKindOfClass:MCEvidencePage.class]);
    });
    MCGuiWait(@"the evidence and its outputs are read", 10, ^BOOL {
        MCEvidencePage *e = ActivePage(c);
        return [e isKindOfClass:MCEvidencePage.class] && e.outputsLoaded;
    });
    MCGuiStep(@"commands in order, errors, test marks and summaries, the final message", ^{
        MCEvidencePage *e = ActivePage(c);
        GCHECK(e.commandCount == 3);
        NSString *c0 = [e commandText:0], *c1 = [e commandText:1], *c2 = [e commandText:2];
        GCHECK([c0 containsString:@"Implement core LSP editing features"]);
        GCHECK([c0 containsString:@"$ ls src"]);
        GCHECK([c0 containsString:@"error"]);
        GCHECK([c0 containsString:@"placeholder output line 2"]);
        GCHECK([c1 containsString:@"TEST make"] && [c1 containsString:@"1 failed, 3,630 passed"]);
        GCHECK([c2 containsString:@"3,631 passed, 0 failed"] && [c2 containsString:@"ok"]);
        GCHECK([c1 containsString:@"Open in Terminal"]);
        GCHECK_EQ_STR(e.finalMessageText, @"Placeholder reply.");
        GCHECK([e.lastTestText containsString:@"3,631 passed, 0 failed"]);
        GCHECK([e.lastTestText containsString:@"Fix the right line and rerun"]);
        GCHECK(e.commandsView.window == c.window && !e.pageView.hidden);
        MCGuiSnapshot(e.pageView, @"agents-evidence");
    });

    MCGuiStep(@"a failed last test sits beside a final message that says all pass", ^{
        [Agents(c) showEvidenceForTranscript:[gSessDir stringByAppendingPathComponent:@"sess-fail.jsonl"]
                                        name:@"Make the parser faster"];
    });
    MCGuiWait(@"the failing session's evidence", 10, ^BOOL {
        MCEvidencePage *e = ActivePage(c);
        return [e isKindOfClass:MCEvidencePage.class] && e.outputsLoaded &&
               [e.transcript hasSuffix:@"sess-fail.jsonl"];
    });
    MCGuiStep(@"the failure count shows, whatever the message claims", ^{
        MCEvidencePage *e = ActivePage(c);
        GCHECK([e.finalMessageText containsString:@"all tests pass"]);
        GCHECK([e.lastTestText containsString:@"2 failed, 10 passed"]);
        GCHECK([e.lastTestText containsString:@"reported an error"]);
        GCHECK([[e commandText:0] containsString:@"12 passed, 0 failed"]);
        MCGuiSnapshot(e.pageView, @"agents-evidence-failed");
    });

    MCGuiStep(@"a 1.3 MB output opens without holding up the window", ^{
        gaps->last = CFAbsoluteTimeGetCurrent();
        gaps->worst = 0;
        Gaps *g = gaps;
        gapTimer = [NSTimer scheduledTimerWithTimeInterval:0.01 repeats:YES block:^(NSTimer *t) {
            (void)t;
            CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
            g->worst = MAX(g->worst, now - g->last);
            g->last = now;
        }];
        [[NSRunLoop currentRunLoop] addTimer:gapTimer forMode:NSRunLoopCommonModes];
        CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
        [Agents(c) showEvidenceForTranscript:[gSessDir stringByAppendingPathComponent:@"sess-big.jsonl"]
                                        name:@"Count to a lot"];
        double opened = CFAbsoluteTimeGetCurrent() - t0;
        MCGuiNote(@"evidence page made in %.1f ms", opened * 1000);
        GCHECK(opened < 0.25);
    });
    MCGuiWait(@"its ends are read", 15, ^BOOL {
        MCEvidencePage *e = ActivePage(c);
        return [e isKindOfClass:MCEvidencePage.class] && e.outputsLoaded &&
               [e.transcript hasSuffix:@"sess-big.jsonl"];
    });
    MCGuiStep(@"the first and last 20 lines, the rest a click away", ^{
        MCEvidencePage *e = ActivePage(c);
        NSString *c0 = [e commandText:0];
        GCHECK([c0 containsString:@"number line 1\n"] && [c0 containsString:@"number line 20\n"]);
        GCHECK(![c0 containsString:@"number line 21\n"]);
        GCHECK([c0 containsString:@"number line 120000"]);
        GCHECK([c0 containsString:@"Show all"]);
        GCHECK(![c0 containsString:@"number line 30000\n"]);
        MCGuiNote(@"longest main-thread gap while reading: %.0f ms", gaps->worst * 1000);
        GCHECK(gaps->worst < 0.25);
        gaps->worst = 0;
        gaps->last = CFAbsoluteTimeGetCurrent();
        [e expandCommand:0];
    });
    MCGuiWait(@"Show all reads the whole output", 20, ^BOOL {
        MCEvidencePage *e = ActivePage(c);
        return [e isKindOfClass:MCEvidencePage.class] && [e isExpanded:0];
    });
    MCGuiStep(@"the whole output is on show, cut at 1 MB, and Show less folds it", ^{
        MCEvidencePage *e = ActivePage(c);
        NSString *all = e.commandsView.string;
        GCHECK([all containsString:@"number line 30000\n"]);
        GCHECK([all containsString:@"(cut at 1 MB)"]);
        GCHECK([all containsString:@"Show less"]);
        MCGuiNote(@"longest main-thread gap while expanding: %.0f ms", gaps->worst * 1000);
        GCHECK(gaps->worst < 1.0);
        [gapTimer invalidate];
        gapTimer = nil;
        NSURL *less = [NSURL URLWithString:@"minicode-evidence://less/0"];
        [(id<NSTextViewDelegate>)e textView:e.commandsView clickedOnLink:less atIndex:0];
        GCHECK(![e isExpanded:0]);
        GCHECK(![e.commandsView.string containsString:@"number line 30000\n"]);
    });

    MCGuiStep(@"Open in Terminal types the command at the session's folder and does not run it", ^{
        MCEvidencePage *e = ActivePage(c);
        NSUInteger before = [c controlTerminals].count;
        NSURL *open = [NSURL URLWithString:@"minicode-evidence://terminal/1"];
        [(id<NSTextViewDelegate>)e textView:e.commandsView clickedOnLink:open atIndex:0];
        GCHECK([c controlTerminals].count == before + 1);
        term = [c controlTerminals].lastObject;
    });
    // A new shell, typed into once its line editor is ready. In a whole run
    // on a loaded machine 20 s was not always enough for a fresh zsh to come
    // up (the suite passed alone); the wait is longer, and says what the
    // terminal had when it still gives up.
    MCGuiWait(@"the command waits at the prompt", 60, ^BOOL {
        return [[term recentText:20] containsString:@"echo agents-typed-$((6*7))"];
    });
    MCGuiPause(1.0);
    MCGuiStep(@"nothing ran, and the terminal is in the session's folder", ^{
        NSString *text = [term recentText:20];
        if (![text containsString:@"echo agents-typed-$((6*7))"])
            MCGuiNote(@"the new terminal: at a prompt %d, busy %d, waiting \"%@\", directory %@, text:\n%@",
                      term.atPrompt, term.busy, term.pendingCommand ?: @"", term.directory, text);
        GCHECK(![text containsString:@"agents-typed-42"]);
        GCHECK(!term.busy);
        GCHECK_EQ_STR(Real(term.directory), gReal);
    });

    // --------------------------------------------------- following the disk
    MCGuiStep(@"a scratchpad file written by a session appears within 2 s", ^{
        NSString *pad = [gTmp stringByAppendingFormat:@"/-Users-user-Code-public-minicode/%@/scratchpad", kSid];
        NSString *report = [pad stringByAppendingPathComponent:@"report.md"];
        MCGuiWriteFile(report, @"# report\n");
        NSMutableString *lines = [NSMutableString string];
        [lines appendFormat:@"%@\n", Use(kSid, 1, @"w9", @"Write", @{@"file_path": report, @"content": @"x"})];
        [lines appendFormat:@"%@\n", Result(kSid, 0, @"w9", @"File created successfully", NO)];
        appendedAt = CFAbsoluteTimeGetCurrent();
        Append([gSessDir stringByAppendingFormat:@"/%@.jsonl", kSid], lines);
    });
    MCGuiWait(@"report.md in the panel", 5, ^BOOL {
        MCAgentsPanel *p = Panel(c);
        // Its group may be new: open every group as it comes.
        NSOutlineView *ov = p.outline;
        for (NSInteger r = 0; r < ov.numberOfRows; r++)
            if ([[p rowDescriptions][(NSUInteger)r] hasPrefix:@"G Outside"] &&
                ![ov isItemExpanded:[ov itemAtRow:r]])
                [ov expandItem:[ov itemAtRow:r]];
        BOOL there = RowWith(p, @"scratchpad/report.md") >= 0;
        if (there && liveDelay < 0) liveDelay = CFAbsoluteTimeGetCurrent() - appendedAt;
        return there;
    });
    MCGuiStep(@"under Outside the folder, on disk, and the session now running", ^{
        MCAgentsPanel *p = Panel(c);
        MCGuiNote(@"the panel followed the transcript in %.2f s", liveDelay);
        GCHECK(liveDelay >= 0 && liveDelay < 2.0);
        NSInteger row = RowWith(p, @"scratchpad/report.md");
        GCHECK(row > RowWith(p, @"G Outside the folder"));
        GCHECK(RowWith(p, @"report.md (missing)") < 0);
        GCHECK(RowWith(p, @"S 0 running Placeholder request") >= 0);
        GCHECK([p.summaryText hasPrefix:@"1 running · "]);
    });

    // ------------------------------------------------------------ worktrees
    MCGuiStep(@"Shift+Cmd+A gives the tree back", ^{
        NSMenuItem *item = MenuItem(NSApp.mainMenu, @"Toggle Agents");
        GCHECK([NSApp sendAction:item.action to:item.target from:item]);
        GCHECK(![[c valueForKey:@"agentsVisible"] boolValue]);
        GCHECK(Panel(c).superview == nil);
        NSOutlineView *tree = [c valueForKey:@"outline"];
        GCHECK(tree.window == c.window);
        [c refreshTree:nil];
    });
    MCGuiWait(@"git has answered about the worktree", 15, ^BOOL {
        MCAgentWorktree *w = [Agents(c) worktreeAtPath:gWorktree];
        return w != nil && w.ahead >= 0;
    });
    MCGuiStep(@"one Worktrees node, labelled from the agent, ahead and modified", ^{
        NSOutlineView *tree = [c valueForKey:@"outline"];
        id first = [tree itemAtRow:0];
        GCHECK_EQ_STR([first valueForKey:@"path"], [gProj stringByAppendingPathComponent:@".claude/worktrees"]);
        [tree expandItem:first];
        id wtItem = [tree itemAtRow:1];
        GCHECK_EQ_STR([wtItem valueForKey:@"path"], gWorktree);
        NSTableCellView *cell = [tree viewAtColumn:0 row:1 makeIfNecessary:YES];
        NSString *label = cell.textField.stringValue;
        GCHECK([label hasPrefix:@"LSP rename, references, quick fixes"]);
        GCHECK([label containsString:@"1 ahead · modified"]);
        NSTableCellView *node = [tree viewAtColumn:0 row:0 makeIfNecessary:YES];
        GCHECK_EQ_STR(node.textField.stringValue, @"Worktrees");
        MCAgentWorktree *w = [Agents(c) worktreeAtPath:gWorktree];
        GCHECK_EQ_STR(w.branch, ([@"worktree-agent-" stringByAppendingString:kAgent]));
        GCHECK(w.ahead == 1 && w.dirty);
        MCGuiSnapshot(tree, @"agents-tree-worktrees");
    });
    MCGuiStep(@"with hidden files shown, .claude does not list the worktrees again", ^{
        [c toggleHiddenFiles:nil];
        NSOutlineView *tree = [c valueForKey:@"outline"];
        NSInteger claude = -1;
        for (NSInteger r = 0; r < tree.numberOfRows; r++)
            if ([[[tree itemAtRow:r] valueForKey:@"path"] isEqualToString:
                    [gProj stringByAppendingPathComponent:@".claude"]]) claude = r;
        GCHECK(claude > 0);
        if (claude > 0) {
            id item = [tree itemAtRow:claude];
            [tree expandItem:item];
            GCHECK([tree numberOfChildrenOfItem:item] == 0);
            [tree collapseItem:item];
        }
        GCHECK_EQ_STR([[tree itemAtRow:0] valueForKey:@"path"],
                      [gProj stringByAppendingPathComponent:@".claude/worktrees"]);
        [c toggleHiddenFiles:nil];
    });
    MCGuiStep(@"the right click on a worktree has its two items, other rows not", ^{
        NSOutlineView *tree = [c valueForKey:@"outline"];
        NSMenu *menu = tree.menu;
        NSInteger wtRow = -1, other = -1;
        for (NSInteger r = 0; r < tree.numberOfRows; r++) {
            NSString *path = [[tree itemAtRow:r] valueForKey:@"path"];
            if ([path isEqualToString:gWorktree]) wtRow = r;
            if ([path hasSuffix:@"/src"] && ![path containsString:@".claude"]) other = r;
        }
        GCHECK(wtRow >= 0 && other >= 0);
        [tree selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)other] byExtendingSelection:NO];
        [(id<NSMenuDelegate>)c menuNeedsUpdate:menu];
        GCHECK([menu itemWithTitle:@"Compare with Main"].hidden);
        [tree selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)wtRow] byExtendingSelection:NO];
        [(id<NSMenuDelegate>)c menuNeedsUpdate:menu];
        GCHECK(![menu itemWithTitle:@"Compare with Main"].hidden);
        GCHECK(![menu itemWithTitle:@"Open in New Window"].hidden);
        [c compareWorktreeWithMain:nil];
        GCHECK_EQ_STR(ActiveTab(c)[@"kind"], @"worktree-changes");
    });
    MCGuiWait(@"the changed files are listed", 15, ^BOOL {
        MCWorktreeChangesPage *pg = ActivePage(c);
        return [pg isKindOfClass:MCWorktreeChangesPage.class] && pg.loaded;
    });
    MCGuiStep(@"committed, added and untracked files against main", ^{
        MCWorktreeChangesPage *pg = ActivePage(c);
        NSArray *rows = pg.rowDescriptions;
        GCHECK(pg.errorText == nil);
        GCHECK([rows containsObject:@"M src/Terminal.mm"]);
        GCHECK([rows containsObject:@"A src/LspClient.h"]);
        GCHECK([rows containsObject:@"A src/LspPicker.h"]);
        GCHECK([rows containsObject:@"? notes.md"]);
        GCHECK(![rows containsObject:@"M src/only-main.txt"]);
        MCGuiSnapshot(pg.pageView, @"agents-worktree-changes");
        [pg openChangeAtRow:(NSInteger)[rows indexOfObject:@"M src/Terminal.mm"]];
    });
    MCGuiWait(@"the side-by-side comparison shows", 15, ^BOOL {
        return [ActiveTab(c)[@"kind"] isEqualToString:@"diff"];
    });
    MCGuiStep(@"main's version on the left, the worktree's on the right", ^{
        MCCompareView *cv = [c valueForKey:@"compareView"];
        GCHECK([cv.comparison.leftText hasPrefix:@"main line 1\n"]);
        GCHECK([cv.comparison.rightText hasPrefix:@"worktree line 1\n"]);
        // Main where the branch left it (AgentCommandsSuite has a main that
        // moved on since).
        GCHECK([cv.comparison.leftLabel hasPrefix:@"main at the branch point ("] &&
               [cv.comparison.leftLabel hasSuffix:@"): src/Terminal.mm"]);
        GCHECK([ActiveTab(c)[@"preview"] boolValue]);
    });
    MCGuiStep(@"Open in New Window opens the worktree as its own folder", ^{
        windowsBefore = MCGuiControllers().count;
        NSOutlineView *tree = [c valueForKey:@"outline"];
        for (NSInteger r = 0; r < tree.numberOfRows; r++)
            if ([[[tree itemAtRow:r] valueForKey:@"path"] isEqualToString:gWorktree])
                [tree selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)r] byExtendingSelection:NO];
        [c openWorktreeInNewWindow:nil];
        GCHECK(MCGuiControllers().count == windowsBefore + 1);
        EditorController *w = MCGuiControllers().lastObject;
        GCHECK_EQ_STR(w.rootPath, gWorktree);
        [w.window close];   // closing one crashed 50 ms later before LspSession dealloc was fixed
        MCGuiSetKeyWindow(c.window);
    });

    // ------------------------------------------------------ terminal links
    MCGuiWait(@"the terminal knows its folder", 60, ^BOOL {
        return term.atPrompt && term.directory.length > 0;
    });
    MCGuiStep(@"a path under the shell's own folder opens that copy, whatever the worktrees hold", ^{
        MCTermLink *link = [term linkInLine:"error at src/Terminal.mm:1454 here" atByte:12];
        GCHECK(link.matches.count == 1);
        if (link.matches.count == 1)
            GCHECK_EQ_STR(Real(link.matches[0].path),
                          ([gReal stringByAppendingPathComponent:@"src/Terminal.mm"]));
    });
    MCGuiStep(@"the shell elsewhere", ^{ [term runCommand:@"cd /"]; });
    MCGuiWait(@"it is there", 10, ^BOOL {
        return term.atPrompt && [term.directory isEqualToString:@"/"];
    });
    MCGuiStep(@"a path in both the folder and the worktree offers both, newest first", ^{
        std::string line = "error at src/Terminal.mm:1454 here";
        MCTermLink *link = [term linkInLine:line atByte:12];
        GCHECK(link != nil && !link.isURL);
        GCHECK(link.matches.count == 2);
        if (link.matches.count == 2) {
            GCHECK_EQ_STR(Real(link.matches[0].path),
                          ([gWorktreeReal stringByAppendingPathComponent:@"src/Terminal.mm"]));
            GCHECK_EQ_STR(Real(link.matches[1].path),
                          ([gReal stringByAppendingPathComponent:@"src/Terminal.mm"]));
        }
        GCHECK(link.line == 1454);
        [c openTerminalLink:link];
        MCAgentWindow *a = Agents(c);
        GCHECK(a.picker.visible);
        GCHECK(a.pickerPaths.count == 2);
        GCHECK([a.picker.rows.firstObject.string containsString:@"modified"]);
        MCGuiSnapshot(a.picker.contentView, @"agents-link-picker");
        // Return goes to the picker, not the terminal.
        MCGuiSendKeyToApp(c.window, 36, 0, @"\r", @"\r");
        GCHECK(!a.picker.visible);
        NSString *path = [ActiveTab(c)[@"path"] description];
        GCHECK_EQ_STR(Real(path), ([gWorktreeReal stringByAppendingPathComponent:@"src/Terminal.mm"]));
        NSTextView *tv = [c valueForKey:@"textView"];
        NSString *s = tv.string;
        NSRange sel = tv.selectedRange;
        NSRange lineRange = [s lineRangeForRange:NSMakeRange(MIN(sel.location, s.length), 0)];
        GCHECK([[s substringWithRange:lineRange] hasPrefix:@"worktree line 1454"]);
    });
    MCGuiStep(@"one match opens directly; none says where it looked", ^{
        MCAgentWindow *a = Agents(c);
        MCTermLink *one = [term linkInLine:"see src/only-main.txt:1" atByte:6];
        GCHECK(one.matches.count == 1);
        [c openTerminalLink:one];
        GCHECK(!a.picker.visible);
        GCHECK([[ActiveTab(c)[@"path"] description] hasSuffix:@"src/only-main.txt"]);
        MCTermLink *none = [term linkInLine:"see src/nothere.py:3" atByte:6];
        GCHECK(none != nil && none.matches.count == 0);
        [c openTerminalLink:none];
        GCHECK(!a.picker.visible);
        GCHECK([a.lastNote hasPrefix:@"Not found: src/nothere.py (looked in "]);
        GCHECK([a.lastNote containsString:@".claude/worktrees/agent-"]);
        GCHECK([a.lastNote containsString:@"/scratchpad"]);
        MCTermLink *word = [term linkInLine:"see nothere.py" atByte:6];
        GCHECK(word == nil);   // a bare name found nowhere is no link
        [term runCommand:[NSString stringWithFormat:@"cd '%@'", gProj]];
    });

    // ------------------------------------------- the transcripts folder goes
    MCGuiStep(@"deleting the transcripts folder empties the panel", ^{
        [c toggleAgents:nil];
        [NSFileManager.defaultManager moveItemAtPath:gProjects
                                              toPath:[gBase stringByAppendingPathComponent:@"projects-away"]
                                               error:nil];
    });
    MCGuiWait(@"the panel says the folder is gone", 10, ^BOOL {
        return [Panel(c) rowDescriptions].count == 0 &&
               [Panel(c).noticeText containsString:@"does not exist"];
    });
    MCGuiStep(@"and it comes back with the folder", ^{
        [NSFileManager.defaultManager moveItemAtPath:[gBase stringByAppendingPathComponent:@"projects-away"]
                                              toPath:gProjects error:nil];
    });
    MCGuiWait(@"the sessions are back", 10, ^BOOL {
        return RowWith(Panel(c), @"Placeholder request") >= 0;
    });
    MCGuiStep(@"the tree again", ^{
        [c toggleAgents:nil];
        GCHECK(![[c valueForKey:@"agentsVisible"] boolValue]);
        delete gaps;
    });
    MCGuiStep(@"the suite's window closes, leaving the first window to the next suite", ^{
        [c.window close];
        c = nil;
        term = nil;
        MCGuiSetKeyWindow(nil);
        unsetenv("MINICODE_CLAUDE_PROJECTS");
        unsetenv("MINICODE_CLAUDE_TMP");
        GCHECK(Win() == nil);
    });
}
