// SidebarIconsSuite.mm — `make guitest SUITES=sidebar_icons`: the row of
// panel icons at the top of the sidebar (SidebarIcons.h). In a window on a
// repository with two changes: the row is the split view's first subview's
// top, four buttons named with their keys, the tree's marked; a click on
// each shows its panel in the sidebar (the tree back from any), and a click
// on the one on show gives it the keyboard; Source Control's badge counts
// the changes and follows a new file; Agents gets a dot while a terminal of
// the window waits; Cmd+B hides the row with the sidebar; and
// workbench.sidebar-icons = off hides it, the panel taking its height.
// Real git (skipped without it).
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "GitPanel.h"
#import "Keymap.h"
#import "SidebarIcons.h"
#import "Terminal.h"
#import "TerminalAttention.h"

namespace {

MCSidebarIconBar *Bar(EditorController *c) { return c.sidebarHost.iconBar; }

NSString *Tip(NSString *name, NSString *command) {
    return [NSString stringWithFormat:@"%@ %@", name, MCKeymapGlyphs(command)];
}

BOOL Click(EditorController *c, MCSidebarPanel panel) {
    NSArray<NSButton *> *buttons = Bar(c).buttons;
    if ((NSUInteger)panel >= buttons.count) return NO;
    [buttons[(NSUInteger)panel] performClick:nil];
    return YES;
}

}  // namespace

MC_GUI_SUITE(sidebar_icons) {
    __block EditorController *c = nil;
    __block NSString *git = nil;
    __block TerminalView *term = nil;
    NSString *repo = [MCGuiScratch() stringByAppendingPathComponent:@"iconsrepo"];

    MCGuiStep(@"a repository with one commit", ^{
        git = MCGitExecutable();
        if (!git) MCGuiNote(@"no git here: the badge checks are skipped");
        MCGuiWriteFile([repo stringByAppendingPathComponent:@"a.txt"], @"one\n");
        MCGuiWriteFile([repo stringByAppendingPathComponent:@"b.txt"], @"bee\n");
    });
    MCGuiAsync(@"git init, add, commit", 30, ^(MCGuiDone done) {
        if (!git) { done(); return; }
        NSString *sh = [NSString stringWithFormat:
            @"cd '%@' && '%@' init -q -b main && '%@' add -A && '%@' commit -qm first", repo, git, git, git];
        MCGuiRunProcess(@[@"/bin/sh", @"-c", sh], nil, ^(int s, NSString *, NSString *err) {
            GCHECK(s == 0);
            if (s) MCGuiNote(@"git: %@", err);
            done();
        });
    });
    MCGuiStep(@"two changes, and a window on it", ^{
        MCGuiWriteFile([repo stringByAppendingPathComponent:@"a.txt"], @"ONE\n");
        MCGuiWriteFile([repo stringByAppendingPathComponent:@"c.txt"], @"new\n");
        NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:repo]]];
        GCHECK(MCGuiControllers().count == before + 1);
        c = MCGuiController();
    });
    MCGuiStep(@"the row: at the sidebar's top, four icons named with their keys, the tree's marked", ^{
        MCSidebarHost *host = c.sidebarHost;
        GCHECK(host && c.splitView.subviews.firstObject == host);
        GCHECK(host.iconsShown && !Bar(c).hidden);
        GCHECK(Bar(c).buttons.count == 4);
        GCHECK(NSHeight(Bar(c).frame) == kMCSidebarIconBarHeight &&
               NSMaxY(Bar(c).frame) == NSHeight(host.bounds) &&
               NSWidth(Bar(c).frame) == NSWidth(host.bounds));
        // The tree under it, the rest of the height.
        GCHECK(host.content == c.sidebarScroll);
        GCHECK(NSMaxY(c.sidebarScroll.frame) == NSHeight(host.bounds) - kMCSidebarIconBarHeight);
        MCGuiNote(@"sidebar %@, row %@", NSStringFromRect(host.frame), NSStringFromRect(Bar(c).frame));
        GCHECK(Bar(c).current == MCSidebarPanelTree);
        NSArray<NSButton *> *b = Bar(c).buttons;
        if (b.count < 4) return;
        GCHECK_EQ_STR(b[0].toolTip, @"File Tree");
        GCHECK_EQ_STR(b[1].toolTip, Tip(@"Source Control", @"workbench.view.scm"));
        GCHECK_EQ_STR(b[2].toolTip, Tip(@"Agents", @"mutible.toggleAgents"));
        GCHECK_EQ_STR(b[3].toolTip, Tip(@"Processes", @"mutible.toggleProcesses"));
        GCHECK([b[1].toolTip containsString:@"⌘"]);
        // The icons never take the keyboard.
        for (NSButton *button in b) GCHECK(!button.acceptsFirstResponder);
    });
    MCGuiWait(@"Source Control's badge counts the two changes", 20, ^BOOL {
        return !git || [[Bar(c) badgeForPanel:MCSidebarPanelSourceControl] isEqualToString:@"2"];
    });
    MCGuiStep(@"no other badge yet; a picture of the row", ^{
        if (!git) return;
        GCHECK_EQ_STR([Bar(c) badgeForPanel:MCSidebarPanelSourceControl], @"2");
        GCHECK_EQ_STR([Bar(c) badgeForPanel:MCSidebarPanelTree], @"");
        GCHECK(![Bar(c) dotForPanel:MCSidebarPanelAgents]);
        MCGuiSnapshot(c.sidebarHost, @"sidebar_icons");
    });
    MCGuiStep(@"a click on Source Control shows it in the sidebar", ^{
        GCHECK(Click(c, MCSidebarPanelSourceControl));
        GCHECK(c.gitPanelVisible && !c.agentsVisible && !c.processesVisible);
        GCHECK(c.sidebarHost.content == c.gitPanel && c.gitPanel.superview == c.sidebarHost);
        GCHECK(Bar(c).current == MCSidebarPanelSourceControl);
        GCHECK(c.sidebarHost.iconsShown);   // the row stays over every panel
    });
    MCGuiStep(@"Agents, from Source Control", ^{
        GCHECK(Click(c, MCSidebarPanelAgents));
        GCHECK(c.agentsVisible && !c.gitPanelVisible && !c.processesVisible);
        GCHECK(c.sidebarHost.content != c.gitPanel && c.sidebarHost.content != c.sidebarScroll);
        GCHECK(c.gitPanel.superview == nil);
        GCHECK(Bar(c).current == MCSidebarPanelAgents);
    });
    MCGuiStep(@"Processes, from Agents", ^{
        GCHECK(Click(c, MCSidebarPanelProcesses));
        GCHECK(c.processesVisible && !c.agentsVisible && !c.gitPanelVisible);
        GCHECK(c.sidebarHost.content == (NSView *)[c processesPanelOnShow]);
        GCHECK(Bar(c).current == MCSidebarPanelProcesses);
    });
    MCGuiStep(@"the File Tree back, from Processes; a click on it again gives it the keyboard", ^{
        GCHECK(Click(c, MCSidebarPanelTree));
        GCHECK(!c.processesVisible && !c.agentsVisible && !c.gitPanelVisible);
        GCHECK(c.sidebarHost.content == c.sidebarScroll);
        GCHECK(Bar(c).current == MCSidebarPanelTree);
        [c.window makeFirstResponder:c.textView];
        GCHECK(Click(c, MCSidebarPanelTree));
        GCHECK(c.window.firstResponder == c.outline);
        GCHECK(c.sidebarHost.content == c.sidebarScroll);
    });
    MCGuiStep(@"the menu's toggle marks its icon too", ^{
        [c toggleSourceControl:nil];
        GCHECK(Bar(c).current == MCSidebarPanelSourceControl);
        [c toggleSourceControl:nil];
        GCHECK(Bar(c).current == MCSidebarPanelTree);
    });
    MCGuiStep(@"a new file", ^{
        if (git) MCGuiWriteFile([repo stringByAppendingPathComponent:@"d.txt"], @"dee\n");
    });
    MCGuiWait(@"the badge follows: three changes", 20, ^BOOL {
        return !git || [[Bar(c) badgeForPanel:MCSidebarPanelSourceControl] isEqualToString:@"3"];
    });
    MCGuiStep(@"a terminal in the window", ^{
        [c toggleTerminal:nil];
        term = c.controlTerminals.firstObject;
        GCHECK(term != nil);
    });
    MCGuiWait(@"its prompt", 20, ^BOOL { return term.atPrompt; });
    MCGuiStep(@"while it waits for you, Agents has a dot; cleared, none", ^{
        NSString *err = nil;
        MCTerminalAttention *att = [MCTerminalAttention shared];
        GCHECK([att applyNotify:Json::object({{"state", "waiting"}, {"message", "Allow?"}})
                     toTerminal:term error:&err]);
        GCHECK([Bar(c) dotForPanel:MCSidebarPanelAgents]);
        GCHECK([att applyNotify:Json::object({{"state", "clear"}}) toTerminal:term error:&err]);
        GCHECK(![Bar(c) dotForPanel:MCSidebarPanelAgents]);
        if (err) MCGuiNote(@"notify: %@", err);
    });
    MCGuiStep(@"Cmd+B hides the row with the sidebar, and brings it back", ^{
        [c toggleSidebarVisibility:nil];
        GCHECK(!c.sidebarHost.iconsShown && Bar(c).hidden);
        [c toggleSidebarVisibility:nil];
        GCHECK(c.sidebarHost.iconsShown && !Bar(c).hidden);
    });
    MCGuiStep(@"workbench.sidebar-icons = off hides it; the tree takes its height", ^{
        MCGuiSetSettings(@"lsp.enabled = false\nworkbench.sidebar-icons = off\n");
        MCSidebarHost *host = c.sidebarHost;
        GCHECK(!host.showsIcons && !host.iconsShown && Bar(c).hidden);
        GCHECK(NSEqualRects(c.sidebarScroll.frame, host.bounds));
        // The panels still come by their keys, the row still hidden.
        [c toggleSourceControl:nil];
        GCHECK(c.gitPanelVisible && NSEqualRects(c.gitPanel.frame, host.bounds) && Bar(c).hidden);
        [c toggleSourceControl:nil];
    });
    MCGuiStep(@"on again, it is back", ^{
        MCGuiSetSettings(@"lsp.enabled = false\n");
        MCSidebarHost *host = c.sidebarHost;
        GCHECK(host.showsIcons && host.iconsShown && !Bar(c).hidden);
        GCHECK(NSMaxY(c.sidebarScroll.frame) == NSHeight(host.bounds) - kMCSidebarIconBarHeight);
    });
    MCGuiStep(@"close the window", ^{
        [c.window performClose:nil];
    });
}
