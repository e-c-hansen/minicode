// AttentionSuite.mm — `make guitest SUITES=attention`: "Waiting for you" on
// the right terminal tab (spec feature 7). Every terminal's environment
// (MINICODE_TERMINAL_ID, MINICODE_PID), `minicode notify --from-hook` run
// from inside a terminal and from outside, the tab's orange and green dots
// and its tooltip, the Dock badge across windows, typing clearing a mark,
// Ctrl+Cmd+W, the session recorded for the Agents panel's Show Terminal and
// for paths the session printed, and `minicode hooks --install` on a
// scratch settings file.
//
// The world is scratch: its own folder and window, a transcripts folder and
// temp root named through MINICODE_CLAUDE_PROJECTS and MINICODE_CLAUDE_TMP,
// Claude Code's settings under the harness's scratch HOME (and a scratch
// CLAUDE_CONFIG_DIR). The real ~/.claude is never read or written.
#import "Harness.h"
#import "EditorController.h"
#import "AgentWindow.h"
#import "AgentsPanel.h"
#import "ControlServer.h"
#import "TabStrip.h"
#import "Terminal.h"
#import "TerminalAttention.h"
#import "TerminalPanel.h"
#include <string>
#include <unistd.h>

@interface TerminalView (AttentionSuite)
- (MCTermLink *)linkInLine:(const std::string &)line atByte:(size_t)byte;
@end

namespace {

NSString *gDir, *gReal, *gProjects, *gTmp, *gPad, *gOther;
NSString *const kSid = @"5e55a0b1-7a1e-4c3e-9f00-000000000777";

NSString *CLI() { return [MCGuiBuildDir() stringByAppendingPathComponent:@"minicode-cli"]; }

NSString *Real(NSString *p) {
    char buf[PATH_MAX];
    return p && realpath(p.fileSystemRepresentation, buf) ? @(buf) : p;
}

NSString *JSONText(id obj) {
    NSData *d = [NSJSONSerialization dataWithJSONObject:obj options:0 error:nil];
    return [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
}

EditorController *Win(NSString *root) {
    for (EditorController *c in MCGuiControllers())
        if ([c.rootPath isEqualToString:root]) return c;
    return nil;
}

MCTerminalPanel *Panel(EditorController *c) { return [c valueForKey:@"terminalPanel"]; }
MCTabStrip *Strip(EditorController *c) { return [Panel(c) valueForKey:@"strip"]; }

MCTabStripItem *Item(EditorController *c, NSUInteger tab) {
    NSArray<MCTabStripItem *> *items = Strip(c).items;
    return tab < items.count ? items[tab] : nil;
}

NSString *Badge() { return NSApp.dockTile.badgeLabel ?: @""; }

NSString *Hook(NSString *event, NSString *message) {
    NSMutableDictionary *d = [@{@"hook_event_name": event, @"session_id": kSid,
                                @"transcript_path": [gProjects stringByAppendingFormat:@"/proj/%@.jsonl", kSid],
                                @"cwd": gReal, @"permission_mode": @"default"} mutableCopy];
    if (message) d[@"message"] = message;
    return JSONText(d);
}

// A hook file in the scratch folder, for `< file`.
NSString *HookFile(NSString *name, NSString *event, NSString *message) {
    NSString *p = [MCGuiScratch() stringByAppendingFormat:@"/attention-%@.json", name];
    MCGuiWriteFile(p, Hook(event, message));
    return p;
}

NSString *Quote(NSString *s) {
    return [NSString stringWithFormat:@"'%@'", [s stringByReplacingOccurrencesOfString:@"'"
                                                                           withString:@"'\\''"]];
}

// The environment a hook run outside any minicode terminal would have.
NSDictionary *NoVariables() { return @{@"MINICODE_TERMINAL_ID": @"", @"MINICODE_PID": @""}; }

void BuildWorld() {
    gDir = [MCGuiScratch() stringByAppendingPathComponent:@"attention-proj"];
    gOther = [MCGuiScratch() stringByAppendingPathComponent:@"attention-other"];
    [NSFileManager.defaultManager createDirectoryAtPath:gOther withIntermediateDirectories:YES
                                             attributes:nil error:nil];
    MCGuiWriteFile([gDir stringByAppendingPathComponent:@"README.md"], @"# attention\n");
    gReal = Real(gDir);
    // A session of Claude Code's in this folder, its scratchpad, and a
    // worktree another agent left with a file of the same name.
    gProjects = [MCGuiScratch() stringByAppendingPathComponent:@"attention-projects"];
    gTmp = [MCGuiScratch() stringByAppendingPathComponent:@"attention-ctmp"];
    NSISO8601DateFormatter *f = [NSISO8601DateFormatter new];
    NSString *line = JSONText(@{@"type": @"user", @"sessionId": kSid, @"cwd": gReal,
                                @"timestamp": [f stringFromDate:[NSDate date]], @"version": @"9.9.9",
                                @"gitBranch": @"main",
                                @"message": @{@"role": @"user", @"content": @"Wait for me in tab two"}});
    MCGuiWriteFile([gProjects stringByAppendingFormat:@"/proj/%@.jsonl", kSid],
                   [line stringByAppendingString:@"\n"]);
    gPad = [gTmp stringByAppendingFormat:@"/proj/%@/scratchpad", kSid];
    MCGuiWriteFile([gPad stringByAppendingPathComponent:@"notes/r.md"], @"one\ntwo\nthree\n");
    MCGuiWriteFile([gDir stringByAppendingPathComponent:@".claude/worktrees/agent-zz/notes/r.md"],
                   @"another agent's\n");
    setenv("MINICODE_CLAUDE_PROJECTS", gProjects.fileSystemRepresentation, 1);
    setenv("MINICODE_CLAUDE_TMP", gTmp.fileSystemRepresentation, 1);
}

NSString *ReadTrimmed(NSString *path) {
    NSString *s = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    return [s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

}  // namespace

MC_GUI_SUITE(attention) {
    __block EditorController *c = nil, *c2 = nil;
    __block TerminalView *t1 = nil, *t2 = nil, *t3 = nil, *w2 = nil;
    __block CFAbsoluteTime started = 0;

    MCGuiStep(@"a window on a scratch folder with three terminal tabs", ^{
        BuildWorld();
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:gDir];
        c = Win(gDir);
        GCHECK(c != nil);
        MCGuiSetKeyWindow(c.window);
        [c newTerminal:nil];
        [c newTerminal:nil];
        [c newTerminal:nil];
        NSArray<TerminalView *> *all = Panel(c).terminals;
        GCHECK(all.count == 3 && Panel(c).tabCount == 3);
        if (all.count == 3) { t1 = all[0]; t2 = all[1]; t3 = all[2]; }
        GCHECK(t1.terminalID > 0 && t2.terminalID == t1.terminalID + 1 &&
               t3.terminalID == t2.terminalID + 1);
    });
    MCGuiWait(@"their shells start", 20, ^BOOL { return t1.atPrompt && t2.atPrompt && t3.atPrompt; });

    // ------------------------------------------------------------ R7.1
    MCGuiStep(@"each shell has its own MINICODE_TERMINAL_ID, and MINICODE_PID", ^{
        for (TerminalView *t in @[t1, t2]) {
            NSString *out = [MCGuiScratch() stringByAppendingFormat:@"/attention-env-%ld",
                             (long)t.terminalID];
            [t runCommand:[NSString stringWithFormat:
                @"printf '%%s %%s\\n' \"$MINICODE_TERMINAL_ID\" \"$MINICODE_PID\" > %@", Quote(out)]];
        }
    });
    MCGuiWait(@"they wrote them", 10, ^BOOL {
        for (TerminalView *t in @[t1, t2])
            if (!ReadTrimmed([MCGuiScratch() stringByAppendingFormat:@"/attention-env-%ld",
                              (long)t.terminalID]).length)
                return NO;
        return YES;
    });
    MCGuiAsync(@"the same ids `minicode state` reports", 20, ^(MCGuiDone done) {
        for (TerminalView *t in @[t1, t2]) {
            NSString *got = ReadTrimmed([MCGuiScratch() stringByAppendingFormat:@"/attention-env-%ld",
                                         (long)t.terminalID]);
            GCHECK_EQ_STR(got, ([NSString stringWithFormat:@"%ld %d", (long)t.terminalID, getpid()]));
        }
        MCGuiRunProcess(@[CLI(), @"state", @"--pid", [NSString stringWithFormat:@"%d", getpid()]], nil,
                        ^(int status, NSString *out, NSString *) {
            GCHECK(status == 0);
            NSDictionary *state = [NSJSONSerialization
                JSONObjectWithData:[out dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
            NSArray *ids = nil;
            for (NSDictionary *w in state[@"windows"])
                if ([w[@"folder"] isEqual:gDir]) ids = [w[@"terminals"] valueForKey:@"id"];
            GCHECK(([ids isEqual:@[@(t1.terminalID), @(t2.terminalID), @(t3.terminalID)]]));
            done();
        });
    });

    // ------------------------------------------------- R7.2, R7.3, the acceptance
    MCGuiStep(@"a Notification hook run in terminal 2 (its own variables)", ^{
        GCHECK(Item(c, 1).attention == MCTabAttentionNone && [Badge() isEqualToString:@""]);
        NSString *file = HookFile(@"n", @"Notification", @"Claude needs your permission to use Bash");
        started = CFAbsoluteTimeGetCurrent();
        [t2 runCommand:[NSString stringWithFormat:@"%@ notify --from-hook < %@", Quote(CLI()),
                        Quote(file)]];
    });
    MCGuiWait(@"tab 2's dot is orange and the Dock says 1 within a second", 1.0, ^BOOL {
        return Item(c, 1).attention == MCTabAttentionWaiting && [Badge() isEqualToString:@"1"];
    });
    MCGuiStep(@"its tooltip is the message; the other tabs have no dot", ^{
        MCGuiNote(@"marked %.0f ms after the command was typed",
                  (CFAbsoluteTimeGetCurrent() - started) * 1000);
        GCHECK([Item(c, 1).toolTip hasPrefix:@"Claude needs your permission to use Bash\n"]);
        GCHECK(Item(c, 0).attention == MCTabAttentionNone && Item(c, 2).attention == MCTabAttentionNone);
        NSRect dot = [Strip(c) attentionRectForTab:1];
        GCHECK(!NSIsEmptyRect(dot) && NSIsEmptyRect([Strip(c) attentionRectForTab:0]));
        GCHECK(NSMinX(dot) > NSMinX([Strip(c) rectForTab:1]) &&
               NSMaxX(dot) < NSMinX([Strip(c) closeRectForTab:1]));
        // The dot is drawn, in its color.
        NSBitmapImageRep *rep = [Strip(c) bitmapImageRepForCachingDisplayInRect:Strip(c).bounds];
        [Strip(c) cacheDisplayInRect:Strip(c).bounds toBitmapImageRep:rep];
        CGFloat sx = rep.pixelsWide / Strip(c).bounds.size.width;
        NSColor *px = [[rep colorAtX:(NSInteger)(NSMidX(dot) * sx) y:(NSInteger)(NSMidY(dot) * sx)]
            colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
        NSColor *want = [MCTabStrip.waitingColor colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
        // Orange (the display's color conversion moves it a little).
        if (!(px.redComponent > 0.85 && px.greenComponent > 0.5 && px.greenComponent < 0.78 &&
              px.blueComponent < 0.42))
            MCGuiNote(@"the dot's pixel: %@ (want about %@)", px, want);
        GCHECK(px.redComponent > 0.85 && px.greenComponent > 0.5 && px.greenComponent < 0.78 &&
               px.blueComponent < 0.42);
        MCGuiSnapshot(Strip(c), @"attention-waiting");
        GCHECK([[[MCTerminalAttention shared] sessionIdForTerminal:t2] isEqualToString:kSid]);
    });
    MCGuiAsync(@"`minicode state` reports the mark and the session", 20, ^(MCGuiDone done) {
        MCGuiRunProcess(@[CLI(), @"state", @"--pid", [NSString stringWithFormat:@"%d", getpid()]], nil,
                        ^(int, NSString *out, NSString *) {
            NSDictionary *state = [NSJSONSerialization
                JSONObjectWithData:[out dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
            NSDictionary *mine = nil;
            for (NSDictionary *w in state[@"windows"])
                for (NSDictionary *t in w[@"terminals"])
                    if ([t[@"id"] integerValue] == t2.terminalID) mine = t;
            GCHECK([mine[@"attention"] isEqual:@"waiting"] && [mine[@"session"] isEqual:kSid] &&
                   [mine[@"message"] isEqual:@"Claude needs your permission to use Bash"]);
            done();
        });
    });
    MCGuiAsync(@"approving: PostToolUse clears the dot and the badge, quietly", 20, ^(MCGuiDone done) {
        CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
        NSDictionary *env = @{@"MINICODE_TERMINAL_ID": [NSString stringWithFormat:@"%ld", (long)t2.terminalID],
                              @"MINICODE_PID": [NSString stringWithFormat:@"%d", getpid()]};
        NSString *cmd = [NSString stringWithFormat:@"%@ notify --from-hook < %@", Quote(CLI()),
                         Quote(HookFile(@"p", @"PostToolUse", nil))];
        MCGuiRunProcess(@[@"/bin/sh", @"-c", cmd], env, ^(int status, NSString *out, NSString *err) {
            double ms = (CFAbsoluteTimeGetCurrent() - t0) * 1000;
            GCHECK(status == 0 && out.length == 0 && err.length == 0);
            GCHECK(Item(c, 1).attention == MCTabAttentionNone && [Badge() isEqualToString:@""]);
            GCHECK(ms < 1000);
            MCGuiNote(@"PostToolUse: the hook ran and the mark went in %.0f ms", ms);
            done();
        });
    });
    MCGuiAsync(@"Stop: a green dot, not counted in the badge", 20, ^(MCGuiDone done) {
        NSDictionary *env = @{@"MINICODE_TERMINAL_ID": [NSString stringWithFormat:@"%ld", (long)t2.terminalID],
                              @"MINICODE_PID": [NSString stringWithFormat:@"%d", getpid()]};
        NSString *cmd = [NSString stringWithFormat:@"%@ notify --from-hook < %@", Quote(CLI()),
                         Quote(HookFile(@"s", @"Stop", nil))];
        MCGuiRunProcess(@[@"/bin/sh", @"-c", cmd], env, ^(int status, NSString *out, NSString *) {
            GCHECK(status == 0 && out.length == 0);
            GCHECK(Item(c, 1).attention == MCTabAttentionDone && [Badge() isEqualToString:@""]);
            GCHECK([Item(c, 1).toolTip hasPrefix:@"Done\n"]);
            done();
        });
    });
    MCGuiStep(@"typing in the terminal clears it", ^{
        GCHECK(Item(c, 1).attention == MCTabAttentionDone);
        [Panel(c) selectTab:1];
        TerminalView *t = t2;
        id grid = [t valueForKey:@"grid"];
        [grid insertText:@"x" replacementRange:NSMakeRange(NSNotFound, 0)];
        GCHECK(Item(c, 1).attention == MCTabAttentionNone);
        [grid insertText:@"\x15" replacementRange:NSMakeRange(NSNotFound, 0)];   // Ctrl+U: the line again
    });
    MCGuiAsync(@"UserPromptSubmit clears a waiting tab too", 20, ^(MCGuiDone done) {
        NSString *pid = [NSString stringWithFormat:@"%d", getpid()];
        NSString *id2 = [NSString stringWithFormat:@"%ld", (long)t2.terminalID];
        MCGuiRunProcess(@[CLI(), @"notify", @"--terminal", id2, @"--state", @"waiting", @"--pid", pid],
                        nil, ^(int s1, NSString *, NSString *) {
            GCHECK(s1 == 0 && Item(c, 1).attention == MCTabAttentionWaiting &&
                   [Item(c, 1).toolTip hasPrefix:@"Waiting for you\n"]);
            NSString *cmd = [NSString stringWithFormat:@"%@ notify --from-hook < %@", Quote(CLI()),
                             Quote(HookFile(@"u", @"UserPromptSubmit", nil))];
            MCGuiRunProcess(@[@"/bin/sh", @"-c", cmd], @{@"MINICODE_TERMINAL_ID": id2, @"MINICODE_PID": pid},
                            ^(int s2, NSString *, NSString *) {
                GCHECK(s2 == 0 && Item(c, 1).attention == MCTabAttentionNone);
                done();
            });
        });
    });

    // ------------------------------------------------------- never noisy
    MCGuiAsync(@"echo '{}' | minicode notify --from-hook outside minicode: exit 0, nothing printed",
               20, ^(MCGuiDone done) {
        NSString *cmd = [NSString stringWithFormat:@"echo '{}' | %@ notify --from-hook", Quote(CLI())];
        MCGuiRunProcess(@[@"/bin/sh", @"-c", cmd], NoVariables(), ^(int status, NSString *out, NSString *err) {
            GCHECK(status == 0 && out.length == 0 && err.length == 0);
            done();
        });
    });
    MCGuiAsync(@"and with variables naming a minicode that is gone, or a terminal that is not there",
               20, ^(MCGuiDone done) {
        NSString *cmd = [NSString stringWithFormat:
            @"MINICODE_TERMINAL_ID=1 MINICODE_PID=99999999 %1$@ notify --from-hook < %2$@ && "
            @"MINICODE_TERMINAL_ID=4242 MINICODE_PID=%3$d %1$@ notify --from-hook < %2$@ && "
            @"echo garbage | MINICODE_TERMINAL_ID=1 MINICODE_PID=%3$d %1$@ notify --from-hook && "
            @"%1$@ notify --from-hook --what < /dev/null",
            Quote(CLI()), Quote(HookFile(@"n2", @"Notification", @"x")), getpid()];
        MCGuiRunProcess(@[@"/bin/sh", @"-c", cmd], nil, ^(int status, NSString *out, NSString *err) {
            GCHECK(status == 0 && out.length == 0 && err.length == 0);
            GCHECK([Badge() isEqualToString:@""]);
            done();
        });
    });
    MCGuiAsync(@"the launcher without a client in its bundle exits 0 too", 20, ^(MCGuiDone done) {
        NSString *launcher = [[MCGuiBuildDir() stringByDeletingLastPathComponent]
            stringByAppendingPathComponent:@"scripts/minicode-launcher"];
        NSString *cmd = [NSString stringWithFormat:@"echo '{}' | /bin/bash %@ notify --from-hook",
                         Quote(launcher)];
        MCGuiRunProcess(@[@"/bin/sh", @"-c", cmd], nil, ^(int status, NSString *out, NSString *err) {
            GCHECK(status == 0 && out.length == 0 && err.length == 0);
            done();
        });
    });

    // --------------------------------------------- the badge, Ctrl+Cmd+W
    MCGuiStep(@"a second window with a terminal", ^{
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:gOther];
        c2 = Win(gOther);
        GCHECK(c2 != nil);
        [c2 newTerminal:nil];
        w2 = Panel(c2).terminals.firstObject;
        MCGuiSetKeyWindow(c.window);
    });
    MCGuiWait(@"its shell starts", 20, ^BOOL { return w2.atPrompt; });
    MCGuiAsync(@"terminals 2 and 3 and the other window's wait: the Dock says 3", 20, ^(MCGuiDone done) {
        NSString *pid = [NSString stringWithFormat:@"%d", getpid()];
        NSString *cmd = [NSString stringWithFormat:
            @"for t in %ld %ld %ld; do %@ notify --terminal $t --state waiting --message 'Allow?' "
            @"--pid %@ || exit 1; done", (long)t2.terminalID, (long)t3.terminalID,
            (long)w2.terminalID, Quote(CLI()), pid];
        MCGuiRunProcess(@[@"/bin/sh", @"-c", cmd], nil, ^(int status, NSString *, NSString *) {
            GCHECK(status == 0);
            GCHECK_EQ_STR(Badge(), @"3");
            GCHECK(Item(c2, 0).attention == MCTabAttentionWaiting);
            GCHECK([MCTerminalAttention shared].waitingCount == 3);
            done();
        });
    });
    MCGuiStep(@"Ctrl+Cmd+W is Terminal > Next Waiting Terminal, and no other item's key", ^{
        NSMutableArray *titles = [NSMutableArray array];
        __block void (^walk)(NSMenu *) = nil;
        void (^w)(NSMenu *) = ^(NSMenu *menu) {
            for (NSMenuItem *item in menu.itemArray) {
                if (item.submenu) walk(item.submenu);
                NSEventModifierFlags m = item.keyEquivalentModifierMask &
                    (NSEventModifierFlagCommand | NSEventModifierFlagShift |
                     NSEventModifierFlagOption | NSEventModifierFlagControl);
                if ([item.keyEquivalent isEqualToString:@"w"] && !item.isAlternate &&
                    m == (NSEventModifierFlagControl | NSEventModifierFlagCommand))
                    [titles addObject:item.title];
            }
        };
        walk = w;
        walk(NSApp.mainMenu);
        walk = nil;
        GCHECK([titles isEqualToArray:@[@"Next Waiting Terminal"]]);
        // From terminal 1, the keyboard in it.
        [Panel(c) selectTab:0];
        [t1 focusInput];
        GCHECK([t1 hasFocus]);
        GCHECK(MCGuiPerformMenuKey(c.window, 13 /* W */,
                                   NSEventModifierFlagControl | NSEventModifierFlagCommand, nil, nil));
        GCHECK(Panel(c).selectedTab == 1 && [t2 hasFocus]);
        // A jump is not typing: the mark stays.
        GCHECK(Item(c, 1).attention == MCTabAttentionWaiting);
    });
    MCGuiStep(@"again: terminal 3, then the other window's", ^{
        MCGuiPerformMenuKey(c.window, 13, NSEventModifierFlagControl | NSEventModifierFlagCommand, nil, nil);
        GCHECK(Panel(c).selectedTab == 2 && [t3 hasFocus]);
        MCGuiPerformMenuKey(c.window, 13, NSEventModifierFlagControl | NSEventModifierFlagCommand, nil, nil);
        GCHECK([w2 hasFocus] && c2.window.firstResponder != nil);
    });
    MCGuiStep(@"closing a waiting terminal takes it off the badge", ^{
        MCGuiSetKeyWindow(c2.window);
        [Panel(c2) closeTerminal:w2];
        GCHECK_EQ_STR(Badge(), @"2");
        [c2.window close];
        c2 = nil;
        w2 = nil;
        MCGuiSetKeyWindow(c.window);
    });
    MCGuiStep(@"typing in each clears them, and with nothing waiting the key only beeps", ^{
        for (TerminalView *t in @[t2, t3]) {
            id grid = [t valueForKey:@"grid"];
            [grid insertText:@" " replacementRange:NSMakeRange(NSNotFound, 0)];
            [grid insertText:@"\x15" replacementRange:NSMakeRange(NSNotFound, 0)];
        }
        GCHECK_EQ_STR(Badge(), @"");
        [Panel(c) selectTab:0];
        [t1 focusInput];
        MCGuiPerformMenuKey(c.window, 13, NSEventModifierFlagControl | NSEventModifierFlagCommand, nil, nil);
        GCHECK(Panel(c).selectedTab == 0 && [t1 hasFocus]);
    });

    // ------------------------------------------------------------ R7.4
    MCGuiStep(@"the Agents panel lists the session", ^{
        [c toggleAgents:nil];
    });
    MCGuiWait(@"its row shows", 10, ^BOOL {
        MCAgentsPanel *p = ((MCAgentWindow *)[c valueForKey:@"agents"]).panel;
        for (NSString *r in [p rowDescriptions])
            if ([r containsString:@"Wait for me in tab two"]) return YES;
        return NO;
    });
    MCGuiStep(@"its menu has Show Terminal, which brings terminal 2 forward", ^{
        MCAgentsPanel *p = ((MCAgentWindow *)[c valueForKey:@"agents"]).panel;
        NSInteger row = -1;
        NSArray<NSString *> *rows = [p rowDescriptions];
        for (NSUInteger i = 0; i < rows.count; i++)
            if ([rows[i] containsString:@"Wait for me in tab two"]) row = (NSInteger)i;
        NSMenu *m = [p menuForRow:row];
        NSMenuItem *show = [m itemWithTitle:@"Show Terminal"];
        GCHECK(show != nil);
        GCHECK(Panel(c).selectedTab == 0);
        if (show) [NSApp sendAction:show.action to:show.target from:show];
        GCHECK(Panel(c).selectedTab == 1 && [t2 hasFocus]);
        [c toggleAgents:nil];
    });
    MCGuiStep(@"a path the session printed: its scratchpad's copy, not another agent's", ^{
        MCTermLink *mine = [t2 linkInLine:"see notes/r.md:2" atByte:6];
        GCHECK(mine.matches.count == 1);
        GCHECK([Real(mine.matches.firstObject.path) isEqualToString:
                Real([gPad stringByAppendingPathComponent:@"notes/r.md"])]);
        GCHECK(mine.line == 2);
        // The same text in a terminal no session ran in finds both copies.
        MCTermLink *other = [t1 linkInLine:"see notes/r.md:2" atByte:6];
        GCHECK(other.matches.count == 2);
        if (other.matches.count != 2) MCGuiNote(@"t1 matches: %@", [other.matches valueForKey:@"path"]);
    });

    // ------------------------------------------------------------ R7.5
    MCGuiAsync(@"hooks --print is the configuration, as JSON", 20, ^(MCGuiDone done) {
        MCGuiRunProcess(@[CLI(), @"hooks", @"--print"], nil, ^(int status, NSString *out, NSString *) {
            NSDictionary *j = [NSJSONSerialization JSONObjectWithData:[out dataUsingEncoding:NSUTF8StringEncoding]
                                                              options:0 error:nil];
            GCHECK(status == 0);
            GCHECK(([[j[@"hooks"] allKeys] sortedArrayUsingSelector:@selector(compare:)].count == 4));
            GCHECK([j[@"hooks"][@"Notification"][0][@"hooks"][0][@"command"]
                       isEqual:@"minicode notify --from-hook"]);
            done();
        });
    });
    __block NSString *settings = nil, *original = nil;
    MCGuiAsync(@"hooks --install into a settings file with hooks of the user's", 20, ^(MCGuiDone done) {
        NSString *home = NSProcessInfo.processInfo.environment[@"HOME"];
        GCHECK([home hasPrefix:MCGuiScratch()]);   // never the real home
        settings = [home stringByAppendingPathComponent:@".claude/settings.json"];
        original = @"{\n  \"model\": \"opus\",\n  \"hooks\": {\n    \"Stop\": [\n      {\n"
                   @"        \"hooks\": [\n          {\n            \"type\": \"command\",\n"
                   @"            \"command\": \"say done\"\n          }\n        ]\n      }\n    ],\n"
                   @"    \"PreToolUse\": [\n      {\n        \"matcher\": \"Bash\",\n"
                   @"        \"hooks\": [\n          {\n            \"type\": \"command\",\n"
                   @"            \"command\": \"~/bin/check.sh\"\n          }\n        ]\n      }\n"
                   @"    ]\n  },\n  \"ratio\": 0.1\n}\n";
        MCGuiWriteFile(settings, original);
        MCGuiRunProcess(@[CLI(), @"hooks", @"--install"], @{@"CLAUDE_CONFIG_DIR": @""},
                        ^(int status, NSString *out, NSString *) {
            GCHECK(status == 0);
            GCHECK([out containsString:@"Backed up"] && [out containsString:settings]);
            MCGuiRunProcess(@[CLI(), @"hooks", @"--install"], @{@"CLAUDE_CONFIG_DIR": @""},
                            ^(int status2, NSString *out2, NSString *) {
                GCHECK(status2 == 0 && [out2 containsString:@"nothing changed"]);
                done();
            });
        });
    });
    MCGuiStep(@"one copy of each hook, the user's untouched, one backup of the original", ^{
        NSData *d = [NSData dataWithContentsOfFile:settings];
        NSDictionary *j = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
        NSDictionary *h = j[@"hooks"];
        for (NSString *ev in @[@"Notification", @"Stop", @"UserPromptSubmit", @"PostToolUse"]) {
            NSUInteger ours = 0;
            for (NSDictionary *e in h[ev])
                for (NSDictionary *x in e[@"hooks"])
                    ours += [x[@"command"] isEqual:@"minicode notify --from-hook"];
            GCHECK(ours == 1);
        }
        GCHECK([h[@"Stop"][0][@"hooks"][0][@"command"] isEqual:@"say done"] && [h[@"Stop"] count] == 2);
        GCHECK([h[@"PreToolUse"] count] == 1 && [h[@"PreToolUse"][0][@"matcher"] isEqual:@"Bash"]);
        GCHECK([j[@"model"] isEqual:@"opus"] && [j[@"ratio"] doubleValue] == 0.1);
        NSString *text = [NSString stringWithContentsOfFile:settings encoding:NSUTF8StringEncoding error:nil];
        GCHECK([text containsString:@"\"ratio\": 0.1\n"]);
        NSString *dir = settings.stringByDeletingLastPathComponent;
        NSArray *backups = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil]
            filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:
                @"SELF BEGINSWITH 'settings.json.minicode-backup-'"]];
        GCHECK(backups.count == 1);
        if (backups.count == 1)
            GCHECK([[NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:backups[0]]
                                              encoding:NSUTF8StringEncoding error:nil]
                       isEqualToString:original]);
    });
    MCGuiAsync(@"CLAUDE_CONFIG_DIR is where Claude Code reads settings then; a broken file is left alone",
               20, ^(MCGuiDone done) {
        NSString *cfg = [MCGuiScratch() stringByAppendingPathComponent:@"attention-claude-config"];
        MCGuiWriteFile([cfg stringByAppendingPathComponent:@"settings.json"], @"{ \"model\": ");
        MCGuiRunProcess(@[CLI(), @"hooks", @"--install"], @{@"CLAUDE_CONFIG_DIR": cfg},
                        ^(int status, NSString *, NSString *err) {
            GCHECK(status == 1 && [err containsString:@"left alone"]);
            GCHECK([[NSString stringWithContentsOfFile:[cfg stringByAppendingPathComponent:@"settings.json"]
                                              encoding:NSUTF8StringEncoding error:nil]
                       isEqualToString:@"{ \"model\": "]);
            [NSFileManager.defaultManager removeItemAtPath:[cfg stringByAppendingPathComponent:@"settings.json"]
                                                     error:nil];
            MCGuiRunProcess(@[CLI(), @"hooks", @"--install"], @{@"CLAUDE_CONFIG_DIR": cfg},
                            ^(int s2, NSString *, NSString *) {
                GCHECK(s2 == 0);
                NSData *d = [NSData dataWithContentsOfFile:[cfg stringByAppendingPathComponent:@"settings.json"]];
                NSDictionary *j = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
                GCHECK([j[@"hooks"] count] == 4);
                done();
            });
        });
    });

    MCGuiStep(@"the suite's window closes, leaving the first window to the next suite", ^{
        [c.window close];
        c = nil;
        t1 = t2 = t3 = nil;
        MCGuiSetKeyWindow(nil);
        unsetenv("MINICODE_CLAUDE_PROJECTS");
        unsetenv("MINICODE_CLAUDE_TMP");
        GCHECK(Win(gDir) == nil);
        GCHECK_EQ_STR(Badge(), @"");
    });
}
