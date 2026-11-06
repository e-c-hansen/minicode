// GitSyncSuite.mm — `make guitest SUITES=git_sync`: Fetch, Pull, Push and
// Sync in the Source Control panel, and autofetch (git.autofetch), against
// a bare repository in the scratch folder. A fetch shows what is new on
// both sides, Sync Changes merges and pushes, a branch with no upstream is
// published, a push whose ssh wants a key fails at once with the reason and
// what to do (no terminal to ask on), the commands run from the menu bar
// with the panel hidden, autofetch waits its three minutes between
// fetches, and a repository with no remote is never fetched.
#import "Harness.h"
#import "EditorController.h"
#import "GitPanel.h"
#include <sys/stat.h>

namespace {
MCGitPanel *Git(EditorController *c) { return [c valueForKey:@"gitPanel"]; }

// Runs a shell line in the scratch world; checks it worked.
void Shell(NSString *what, NSString *line) {
    MCGuiAsync(what, 60, ^(MCGuiDone done) {
        MCGuiRunProcess(@[@"/bin/sh", @"-c", line], nil, ^(int s, NSString *out, NSString *err) {
            GCHECK(s == 0);
            if (s) MCGuiNote(@"%@: %@%@", what, out, err);
            done();
        });
    });
}
}  // namespace

MC_GUI_SUITE(git_sync) {
    __block EditorController *c = nil, *lonely = nil;
    NSString *git = MCGitExecutable();
    NSString *base = [MCGuiScratch() stringByAppendingPathComponent:@"gitsync"];
    NSString *work = [base stringByAppendingPathComponent:@"work"];
    NSString *lonelyRepo = [base stringByAppendingPathComponent:@"lonely"];
    NSString *G = git ? [NSString stringWithFormat:@"'%@'", git] : @"git";
    // A commit made in the other clone and pushed, so `work` is behind.
    NSString *(^pushFromOther)(NSString *) = ^NSString *(NSString *name) {
        return [NSString stringWithFormat:
            @"cd '%@/other' && %@ pull -q && echo %@ > %@.txt && %@ add -A && "
             "%@ commit -qm '%@' && %@ push -q", base, G, name, name, G, G, name, G];
    };
    __block NSTimeInterval t0 = 0;
    __block NSUInteger alertsBefore = 0;

    if (!git) {
        MCGuiStep(@"no git", ^{ MCGuiNote(@"no git here: the sync checks are skipped"); });
        return;
    }
    Shell(@"a bare remote, two clones, and a repository with no remote",
          [NSString stringWithFormat:
              @"set -e; mkdir -p '%@'; cd '%@'; %@ init -q --bare -b main remote.git; "
               "%@ clone -q remote.git work 2>/dev/null; cd work; %@ checkout -q -b main 2>/dev/null || true; "
               "%@ config pull.rebase false; echo one > a.txt; %@ add -A; %@ commit -qm first; "
               "%@ push -q -u origin main; cd ..; %@ clone -q remote.git other; "
               "mkdir lonely; cd lonely; %@ init -q -b main; echo x > x.txt; %@ add -A; "
               "%@ commit -qm alone",
              base, base, G, G, G, G, G, G, G, G, G, G, G]);
    Shell(@"one commit on each side", [NSString stringWithFormat:
              @"%@ && cd '%@' && echo two > a.txt && %@ commit -qam mine", pushFromOther(@"theirs"),
              work, G]);
    MCGuiStep(@"a window on the clone; its panel exists before it is shown", ^{
        NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:work]]];
        GCHECK(MCGuiControllers().count == before + 1);
        c = MCGuiController();
        alertsBefore = MCGuiAlertsShown().count;
        GCHECK(Git(c) != nil);
        GCHECK(![[c valueForKey:@"gitPanelVisible"] boolValue]);
        // Fetch from the menu bar's item, with the panel hidden.
        GCHECK([NSApp sendAction:@selector(gitFetch:) to:nil from:nil]);
    });
    MCGuiWait(@"fetched", 30, ^BOOL { return [Git(c).lastSync isEqualToString:@"fetch ok"]; });
    MCGuiStep(@"the panel shows one to pull and one to push", ^{
        [c toggleSourceControl:nil];
    });
    MCGuiWait(@"the counts", 15, ^BOOL {
        return [Git(c).graphSummary containsString:@"↑ 1 to push, ↓ 1 to pull"];
    });
    MCGuiStep(@"the button says so; the panel's menu has the four", ^{
        GCHECK_EQ_STR(Git(c).syncButtonTitle, @"Sync Changes  ↓ 1  ↑ 1");
        GCHECK([Git(c).errorLine hasPrefix:@"Fetched from origin"]);
        NSEvent *right = [NSEvent mouseEventWithType:NSEventTypeRightMouseDown location:NSZeroPoint
                                       modifierFlags:0 timestamp:0 windowNumber:0 context:nil
                                         eventNumber:0 clickCount:1 pressure:1];
        NSMenu *menu = [Git(c) menuForEvent:right];
        NSMutableArray *titles = [NSMutableArray array];
        for (NSMenuItem *it in menu.itemArray) if (it.title.length) [titles addObject:it.title];
        MCGuiNote(@"panel menu: %@", titles);
        GCHECK(titles.count >= 4 && [[titles subarrayWithRange:NSMakeRange(0, 4)]
                                        isEqualToArray:(@[@"Pull", @"Push", @"Fetch", @"Sync"])]);
        [Git(c) sync];
        GCHECK(Git(c).syncing);
    });
    MCGuiWait(@"synced", 60, ^BOOL { return !Git(c).syncing; });
    MCGuiStep(@"merged and pushed, nothing asked", ^{
        MCGuiNote(@"sync: %@, %@", Git(c).lastSync, Git(c).errorLine);
        GCHECK_EQ_STR(Git(c).lastSync, @"sync ok");
        GCHECK([Git(c).errorLine hasPrefix:@"Synced with origin/main"]);
        GCHECK(MCGuiAlertsShown().count == alertsBefore);
    });
    MCGuiWait(@"up to date, the button gone", 15, ^BOOL {
        return [Git(c).graphSummary hasPrefix:@"Up to date with origin/main"] &&
               Git(c).syncButtonTitle.length == 0;
    });
    MCGuiAsync(@"the remote has both commits", 20, ^(MCGuiDone done) {
        NSString *sh = [NSString stringWithFormat:@"cd '%@/remote.git' && %@ log --format=%%s main",
                                                  base, G];
        MCGuiRunProcess(@[@"/bin/sh", @"-c", sh], nil, ^(int s, NSString *out, NSString *) {
            GCHECK(s == 0 && [out containsString:@"mine"] && [out containsString:@"theirs"]);
            done();
        });
    });

    // ------------------------------------------------- Publish Branch
    Shell(@"a branch of its own", [NSString stringWithFormat:
              @"cd '%@' && %@ checkout -q -b feature && echo f > f.txt && %@ add -A && "
               "%@ commit -qm feature", work, G, G, G]);
    MCGuiWait(@"Publish Branch", 15, ^BOOL {
        if (![Git(c).syncButtonTitle isEqualToString:@"Publish Branch"]) [Git(c) refresh];
        return [Git(c).syncButtonTitle isEqualToString:@"Publish Branch"];
    });
    MCGuiStep(@"Push publishes it", ^{ [Git(c) push]; });
    MCGuiWait(@"published", 30, ^BOOL { return [Git(c).lastSync isEqualToString:@"push ok"]; });
    MCGuiAsync(@"origin has it, and it tracks origin/feature", 20, ^(MCGuiDone done) {
        GCHECK_EQ_STR(Git(c).errorLine, @"Published feature to origin.");
        NSString *sh = [NSString stringWithFormat:
            @"cd '%@' && %@ rev-parse --abbrev-ref @{upstream} && cd ../remote.git && "
             "%@ rev-parse --verify -q refs/heads/feature", work, G, G];
        MCGuiRunProcess(@[@"/bin/sh", @"-c", sh], nil, ^(int s, NSString *out, NSString *) {
            GCHECK(s == 0 && [out hasPrefix:@"origin/feature\n"]);
            done();
        });
    });

    // ------------------------------------- a credential it cannot ask for
    __block NSDate *pushed = nil;
    MCGuiStep(@"a push URL over ssh whose key needs a passphrase", ^{
        NSString *ssh = [base stringByAppendingPathComponent:@"fake-ssh"];
        // Asks the terminal, as ssh does, which only works where there is
        // one; then says what ssh says without a key.
        MCGuiWriteFile(ssh, @"#!/bin/sh\n(read -r answer < /dev/tty) 2>/dev/null\n"
                            @"echo 'git@example.invalid: Permission denied (publickey).' >&2\n"
                            @"exit 255\n");
        chmod(ssh.fileSystemRepresentation, 0755);
        MCGuiWriteFile([work stringByAppendingPathComponent:@"f.txt"], @"changed\n");
    });
    Shell(@"configured", [NSString stringWithFormat:
              @"cd '%@' && %@ config core.sshCommand '%@/fake-ssh' && "
               "%@ remote set-url --push origin ssh://git@example.invalid/x.git && "
               "%@ commit -qam again", work, G, base, G, G]);
    MCGuiStep(@"Push", ^{
        pushed = [NSDate date];
        [Git(c) push];
    });
    MCGuiWait(@"refused", 30, ^BOOL { return [Git(c).lastSync isEqualToString:@"push failed"]; });
    MCGuiStep(@"at once, with ssh's words and what to do", ^{
        MCGuiNote(@"push failed after %.1f s: %@", -pushed.timeIntervalSinceNow, Git(c).errorLine);
        GCHECK(-pushed.timeIntervalSinceNow < 15);
        GCHECK([Git(c).errorLine containsString:@"Permission denied (publickey)"]);
        GCHECK([Git(c).errorLine containsString:@"ssh-add --apple-use-keychain"]);
        GCHECK(MCGuiAlertsShown().count == alertsBefore);
    });
    Shell(@"the push URL put back", [NSString stringWithFormat:
              @"cd '%@' && %@ config --unset remote.origin.pushurl && %@ config --unset core.sshCommand",
              work, G, G]);

    // ------------------------------------------------------- autofetch
    Shell(@"another commit on the remote's main", pushFromOther(@"later"));
    MCGuiStep(@"autofetch off: nothing", ^{
        Git(c).isActive = ^BOOL { return YES; };
        t0 = NSDate.timeIntervalSinceReferenceDate + 100000;
        GCHECK(![Git(c) autoFetchTickAt:t0]);
        GCHECK(Git(c).autoFetches == 0);
        MCGuiSetSettings(@"lsp.enabled = false\ngit.autofetch = true\n");
        __block BOOL inactive = YES;
        Git(c).isActive = ^BOOL { return !inactive; };
        GCHECK(![Git(c) autoFetchTickAt:t0]);   // the window in the background
        inactive = NO;
        // The window has the keyboard: due, as nothing was fetched by it yet
        // at this (later) time.
        GCHECK([Git(c) autoFetchTickAt:t0]);
    });
    MCGuiWait(@"fetched in the background", 30, ^BOOL {
        return Git(c).autoFetches == 1 && !Git(c).syncing;
    });
    MCGuiStep(@"quietly; not again within three minutes", ^{
        GCHECK_EQ_STR(Git(c).lastSync, @"fetch ok");
        GCHECK(![Git(c) autoFetchTickAt:t0 + 60]);
        GCHECK(![Git(c) autoFetchTickAt:t0 + 179]);
        GCHECK([Git(c) autoFetchTickAt:t0 + 181]);
    });
    MCGuiWait(@"the second", 30, ^BOOL { return Git(c).autoFetches == 2 && !Git(c).syncing; });
    MCGuiStep(@"the panel again; a period of ten minutes", ^{
        [c toggleSourceControl:nil];   // the tree again
        [c toggleSourceControl:nil];   // and the panel, refreshed
        // A period of ten minutes waits ten minutes.
        MCGuiSetSettings(@"lsp.enabled = false\ngit.autofetch = true\ngit.autofetch-period = 600\n");
        GCHECK(![Git(c) autoFetchTickAt:t0 + 181 + 400]);
        GCHECK([Git(c) autoFetchTickAt:t0 + 181 + 600]);
    });
    MCGuiWait(@"the third", 30, ^BOOL { return Git(c).autoFetches == 3 && !Git(c).syncing; });

    // ------------------------------------------- no remote: never fetched
    MCGuiStep(@"a window on a repository with no remote", ^{
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:lonelyRepo]]];
        lonely = MCGuiController();
        GCHECK(lonely != c && Git(lonely) != nil);
        Git(lonely).isActive = ^BOOL { return YES; };
        GCHECK([Git(lonely) autoFetchTickAt:t0]);   // looks, once the wait is over
    });
    MCGuiWait(@"looked", 20, ^BOOL { return Git(lonely).lastSync != nil && !Git(lonely).syncing; });
    MCGuiStep(@"nothing fetched, nothing said, and not looked at again soon", ^{
        GCHECK_EQ_STR(Git(lonely).lastSync, @"fetch refused");
        GCHECK(Git(lonely).autoFetches == 0);
        GCHECK(Git(lonely).errorLine.length == 0);
        GCHECK(![Git(lonely) autoFetchTickAt:t0 + 60]);
        // Asked by hand, it says why.
        [Git(lonely) fetch];
    });
    MCGuiWait(@"refused by hand", 20, ^BOOL { return !Git(lonely).syncing; });
    MCGuiStep(@"with the reason", ^{
        GCHECK_EQ_STR(Git(lonely).errorLine, @"This repository has no remote to fetch from.");
        MCGuiSetSettings(@"lsp.enabled = false\n");
        GCHECK(![Git(c) autoFetchTickAt:t0 + 100000]);   // off again
        [lonely.window close];
        [c.window close];
    });
    MCGuiWait(@"both windows gone", 5, ^BOOL {
        return ![MCGuiControllers() containsObject:c] && ![MCGuiControllers() containsObject:lonely];
    });
}
