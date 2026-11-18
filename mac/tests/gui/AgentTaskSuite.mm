// AgentTaskSuite.mm — `make guitest SUITES=agent_task`: New Agent Task
// (AgentTaskBar.h). A stand-in for Claude Code (agent.command names a
// scratch script, never the real claude) writes the argument it was given
// into the folder it runs in, and for a prompt holding SESSION also a
// transcript into the scratch transcripts folder, as Claude Code would.
//
// Checked: the bar says no to a folder outside a repository, to a
// repository with no commit, and to a missing agent command (with where to
// get Claude Code); it says uncommitted changes stay behind; the name
// follows the prompt; Return makes the worktree from HEAD on a branch of
// that name (the uncommitted change left out, the main checkout's status
// kept clear of it), opens a terminal running the stand-in in the worktree
// with the prompt as one argument, exactly as typed (quotes, $( ), `, !),
// and lists the task in the Agents panel; a long prompt over several lines
// arrives whole too; a name already taken gets -2; once a session for the
// task is in the transcripts, its row stands for the task and offers
// Compare with Main…, which opens the worktree's review.
#import "Harness.h"
#import "EditorController.h"
#import "AgentModel.h"
#import "AgentTaskBar.h"
#import "AgentWindow.h"
#import "AgentsPanel.h"
#import "ChangeReviewPage.h"
#import "Environment.h"
#import "GitPanel.h"
#import "Terminal.h"
#import "TerminalPanel.h"

#include "AgentTask.h"

#include <string>
#include <sys/stat.h>

namespace {

NSString *Read(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}

MCAgentWindow *Agents(EditorController *c) { return [c valueForKey:@"agents"]; }

void Shell(NSString *what, NSString *line) {
    MCGuiAsync(what, 60, ^(MCGuiDone done) {
        MCGuiRunProcess(@[@"/bin/sh", @"-c", line], nil, ^(int s, NSString *out, NSString *err) {
            if (s) MCGuiNote(@"%@: %@%@", what, out, err);
            GCHECK(s == 0);
            done();
        });
    });
}

void TypePrompt(MCAgentTaskBar *bar, NSString *text) {
    NSTextView *tv = bar.textView;
    [tv insertText:text replacementRange:NSMakeRange(0, tv.string.length)];
}

NSInteger RowWith(MCAgentsPanel *p, NSString *text) {
    NSArray<NSString *> *rows = [p rowDescriptions];
    for (NSUInteger i = 0; i < rows.count; i++)
        if ([rows[i] containsString:text]) return (NSInteger)i;
    return -1;
}

NSArray<NSString *> *MenuTitles(NSMenu *m) {
    NSMutableArray *out = [NSMutableArray array];
    for (NSMenuItem *it in m.itemArray)
        if (!it.isSeparatorItem) [out addObject:it.title];
    return out;
}

} // namespace

MC_GUI_SUITE(agent_task) {
    NSString *git = MCGitExecutable();
    if (!git) {
        MCGuiStep(@"no git", ^{ MCGuiNote(@"no git here: the agent task checks are skipped"); });
        return;
    }
    NSString *G = [NSString stringWithFormat:@"'%@'", git];
    NSString *base = [MCGuiScratch() stringByAppendingPathComponent:@"agenttask"];
    NSString *repo = [base stringByAppendingPathComponent:@"repo"];
    NSString *projects = [base stringByAppendingPathComponent:@"projects"];
    NSString *fake = [base stringByAppendingPathComponent:@"bin/claude"];
    NSString *const sid = @"5e55a0de-0000-4000-8000-00000000f00d";
    // Quotes of both kinds, a command substitution, backticks and a
    // history bang: all of it must reach the program as typed.
    NSString *const prompt1 = @"Fix the login bug: it's \"broken\" when $(whoami) runs `id` !";
    NSString *const prompt2 = @"SESSION check: two lines\nwith 'quotes' and $(echo no) and a\ttab";
    NSString *const name1 = @(AgentTask::nameFromPrompt(prompt1.UTF8String).c_str());
    NSString *wt1 = @(AgentTask::worktreePath(repo.UTF8String, name1.UTF8String).c_str());
    NSString *wt2 = [wt1 stringByAppendingString:@"-2"];
    __block EditorController *c = nil;
    MCAgentTaskSession *(^T)(void) = ^MCAgentTaskSession * { return Agents(c).tasks; };
    void (^settings)(NSString *) = ^(NSString *command) {
        MCGuiSetSettings([NSString stringWithFormat:@"lsp.enabled = false\nagent.command = %@\n", command]);
    };

    MCGuiStep(@"a folder (no repository yet), a stand-in claude, scratch transcripts", ^{
        GCHECK_EQ_STR(name1, @"fix-login-bug-broken");
        [NSFileManager.defaultManager createDirectoryAtPath:projects withIntermediateDirectories:YES
                                                 attributes:nil error:nil];
        [NSFileManager.defaultManager createDirectoryAtPath:[base stringByAppendingPathComponent:@"tmp"]
                                withIntermediateDirectories:YES attributes:nil error:nil];
        MCGuiWriteFile([repo stringByAppendingPathComponent:@"a.txt"], @"one\n");
        MCGuiWriteFile(fake, [NSString stringWithFormat:
            @"#!/bin/sh\n"
            @"printf '%%s' \"$1\" > .fake-claude-args\n"
            @"case \"$1\" in *SESSION*)\n"
            @"  d='%@/fake-task'; mkdir -p \"$d\"\n"
            @"  ts=$(date -u +%%Y-%%m-%%dT%%H:%%M:%%S.000Z)\n"
            @"  printf '{\"parentUuid\":null,\"isSidechain\":false,\"type\":\"user\",\"timestamp\":\"%%s\","
            @"\"cwd\":\"%%s\",\"sessionId\":\"%@\",\"version\":\"9.9.9\",\"gitBranch\":\"x\","
            @"\"message\":{\"role\":\"user\",\"content\":\"SESSION check from the stand-in\"}}\\n' "
            @"\"$ts\" \"$(pwd -P)\" > \"$d/%@.jsonl\"\n"
            @"esac\n", projects, sid, sid]);
        chmod(fake.fileSystemRepresentation, 0755);
        MCSetEnv("MUTIBLE_CLAUDE_PROJECTS", projects.fileSystemRepresentation);
        MCSetEnv("MUTIBLE_CLAUDE_TMP", [base stringByAppendingPathComponent:@"tmp"].fileSystemRepresentation);
        settings(fake);
        NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:repo]]];
        GCHECK(MCGuiControllers().count == before + 1);
        c = MCGuiController();
        [c newAgentTask:nil];
        GCHECK(T().barShown);
        GCHECK(T().bar.superview != nil && !T().bar.hidden);
        GCHECK(c.window.firstResponder == T().bar.textView);
    });
    MCGuiWait(@"the look at the folder", 20, ^BOOL { return T().repositoryChecked; });
    MCGuiStep(@"outside a repository: said, and Return makes nothing", ^{
        MCGuiNote(@"hint: %@", T().bar.hint);
        GCHECK(T().bar.hintIsProblem);
        GCHECK([T().bar.hint containsString:@"not in a git repository"]);
        TypePrompt(T().bar, prompt1);
        GCHECK_EQ_STR(T().bar.nameField.stringValue, name1);   // the name follows the prompt
        [T() start];
    });
    MCGuiWait(@"refused", 20, ^BOOL { return !T().busy && T().lastProblem != nil; });
    MCGuiStep(@"nothing made", ^{
        GCHECK([T().lastProblem containsString:@"not in a git repository"]);
        GCHECK(![NSFileManager.defaultManager fileExistsAtPath:[repo stringByAppendingPathComponent:@".claude"]]);
        GCHECK(T().lastTask == nil);
    });
    Shell(@"git init, no commit yet",
          [NSString stringWithFormat:@"cd '%@' && %@ init -q -b main", repo, G]);
    MCGuiStep(@"look again", ^{ [T() begin]; });
    MCGuiWait(@"the look", 20, ^BOOL { return T().repositoryChecked; });
    MCGuiStep(@"no commit yet: said", ^{
        MCGuiNote(@"hint: %@", T().bar.hint);
        GCHECK(T().bar.hintIsProblem);
        GCHECK([T().bar.hint containsString:@"no commit yet"]);
    });
    Shell(@"a commit, then an uncommitted change to a.txt",
          [NSString stringWithFormat:@"cd '%@' && %@ add -A && %@ commit -qm first && echo two >> a.txt",
                                     repo, G, G]);
    MCGuiStep(@"look again", ^{ [T() begin]; });
    MCGuiWait(@"the look", 20, ^BOOL { return T().repositoryChecked; });
    MCGuiStep(@"from main; the uncommitted change stays behind", ^{
        MCGuiNote(@"hint: %@", T().bar.hint);
        GCHECK(!T().bar.hintIsProblem);
        GCHECK_EQ_STR(T().bar.hint, @(AgentTask::startsFromText("main", 1).c_str()));
        // The agent's command missing: where to get Claude Code.
        settings(@"/nonexistent/bin/claude");
        [T() begin];
        GCHECK(T().bar.hintIsProblem);
        GCHECK([T().bar.hint containsString:@"was not found"]);
        GCHECK([T().bar.hint containsString:@"claude.com/claude-code"]);
        [T() start];
        GCHECK([T().lastProblem containsString:@"was not found"]);
        GCHECK(T().lastTask == nil);
        settings(fake);
        [T() begin];
    });
    MCGuiWait(@"the look", 20, ^BOOL { return T().repositoryChecked && !T().bar.hintIsProblem; });
    MCGuiStep(@"Return: the worktree, the terminal, the row", ^{
        GCHECK_EQ_STR(T().bar.textView.string, prompt1);   // kept through the refusals
        [T() start];
        GCHECK(T().busy);
    });
    MCGuiWait(@"the task started", 60, ^BOOL { return !T().busy; });
    MCGuiStep(@"made where Claude Code's worktrees go, on its own branch; the bar closed", ^{
        MCGuiNote(@"problem: %@", T().lastProblem);
        GCHECK(T().lastProblem == nil);
        GCHECK_EQ_STR(T().lastTask.name, name1);
        GCHECK_EQ_STR(T().lastTask.path, wt1);
        GCHECK_EQ_STR(T().lastTask.prompt, prompt1);
        GCHECK(!T().barShown);
        GCHECK([NSFileManager.defaultManager fileExistsAtPath:[wt1 stringByAppendingPathComponent:@".git"]]);
        // From HEAD: the uncommitted line is not there.
        GCHECK_EQ_STR(Read([wt1 stringByAppendingPathComponent:@"a.txt"]), @"one\n");
        TerminalView *t = T().lastTask.terminal;
        GCHECK(t != nil);
        MCTerminalPanel *panel = [c valueForKey:@"terminalPanel"];
        GCHECK([panel.terminals containsObject:t]);
    });
    Shell(@"the branch is the name; the main checkout's status leaves the worktrees out",
          [NSString stringWithFormat:
               @"test \"$(%@ -C '%@' rev-parse --abbrev-ref HEAD)\" = '%@' && "
               @"cd '%@' && test -z \"$(%@ status --porcelain --untracked-files=normal | grep claude)\"",
               G, wt1, name1, repo, G]);
    MCGuiWait(@"the stand-in ran in the worktree", 30, ^BOOL {
        return [NSFileManager.defaultManager fileExistsAtPath:[wt1 stringByAppendingPathComponent:@".fake-claude-args"]];
    });
    MCGuiStep(@"it got the prompt as one argument, exactly; the panel lists the task", ^{
        NSString *got = Read([wt1 stringByAppendingPathComponent:@".fake-claude-args"]);
        MCGuiNote(@"argument: %@", got);
        GCHECK_EQ_STR(got, prompt1);
        MCAgentsPanel *p = Agents(c).panel;
        [p reload];
        MCGuiNote(@"rows: %@", p.rowDescriptions);
        const NSInteger row = RowWith(p, [NSString stringWithFormat:@"T %@ | starting · %@", name1, name1]);
        GCHECK(row >= 0);
        if (row >= 0) {
            NSArray *titles = MenuTitles([p menuForRow:row]);
            GCHECK([titles containsObject:@"Show Terminal"]);
            GCHECK([titles containsObject:@"Compare with Main…"]);
        }
        GCHECK([p.summaryText hasPrefix:@"1 starting"]);
    });
    MCGuiStep(@"a second task: several lines, the same name typed", ^{
        [c newAgentTask:nil];
        GCHECK(T().barShown);
        GCHECK_EQ_STR(T().bar.textView.string, @"");   // the last prompt went with its task
        TypePrompt(T().bar, prompt2);
        T().bar.nameField.stringValue = name1;
        [T().bar controlTextDidChange:[NSNotification notificationWithName:NSControlTextDidChangeNotification
                                                                    object:T().bar.nameField]];
        GCHECK(T().bar.nameEdited);
        [T() start];
    });
    MCGuiWait(@"the second task started", 60, ^BOOL { return !T().busy; });
    MCGuiWait(@"its stand-in ran", 30, ^BOOL {
        return [NSFileManager.defaultManager fileExistsAtPath:[wt2 stringByAppendingPathComponent:@".fake-claude-args"]];
    });
    MCGuiStep(@"-2 on the name; the whole prompt arrived", ^{
        GCHECK(T().lastProblem == nil);
        GCHECK_EQ_STR(T().lastTask.name, [name1 stringByAppendingString:@"-2"]);
        GCHECK_EQ_STR(T().lastTask.path, wt2);
        GCHECK_EQ_STR(Read([wt2 stringByAppendingPathComponent:@".fake-claude-args"]), prompt2);
        [Agents(c).model refresh];
    });
    MCGuiWait(@"its session in the panel", 30, ^BOOL {
        MCAgentsPanel *p = Agents(c).panel;
        [p reload];
        return RowWith(p, @"SESSION check from the stand-in") >= 0;
    });
    MCGuiStep(@"the session stands for the second task; the first still waits", ^{
        MCAgentsPanel *p = Agents(c).panel;
        MCGuiNote(@"rows: %@", p.rowDescriptions);
        GCHECK(RowWith(p, [NSString stringWithFormat:@"T %@-2 |", name1]) < 0);
        GCHECK(RowWith(p, [NSString stringWithFormat:@"T %@ |", name1]) >= 0);
        const NSInteger row = RowWith(p, @"SESSION check from the stand-in");
        NSArray *titles = MenuTitles([p menuForRow:row]);
        MCGuiNote(@"session menu: %@", titles);
        GCHECK([titles containsObject:@"Compare with Main…"]);
        [Agents(c).model refreshWorktrees];
    });
    MCGuiWait(@"git lists the worktree", 20, ^BOOL { return [Agents(c) worktreeAtPath:wt2] != nil; });
    MCGuiStep(@"Compare with Main from the task's row: the worktree's review", ^{
        [Agents(c) compareWorktreeAt:wt2];
    });
    MCGuiWait(@"the review", 20, ^BOOL {
        id page = [[c valueForKey:@"activeTab"] valueForKey:@"page"];
        return [page isKindOfClass:MCChangeReviewPage.class] && ((MCChangeReviewPage *)page).loaded;
    });
    MCGuiStep(@"it lists what the stand-in left; the window closes", ^{
        MCChangeReviewPage *page = [[c valueForKey:@"activeTab"] valueForKey:@"page"];
        MCGuiNote(@"review rows: %@", page.rowDescriptions);
        GCHECK_EQ_STR(page.pageKind, @"worktree-changes");
        BOOL found = NO;
        for (NSString *r in page.rowDescriptions)
            if ([r containsString:@".fake-claude-args"]) found = YES;
        GCHECK(found);
        [c.window close];
        c = nil;
        MCGuiSetKeyWindow(nil);
        MCSetEnv("MUTIBLE_CLAUDE_PROJECTS", nullptr);
        MCSetEnv("MUTIBLE_CLAUDE_TMP", nullptr);
        MCGuiSetSettings(@"lsp.enabled = false\n");
    });
}
