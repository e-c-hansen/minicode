// EditorController+SidebarIcons.mm — EditorController: the sidebar's row of
// panel icons (SidebarIcons.h draws it). What each icon shows: the panel on
// show marked; Source Control's count, the paths the tree's git status
// found changed (MCTreeGitColors, so no git runs for it); Agents' dot while
// one of the window's terminals waits for you (MCTerminalAttention); and
// Processes' count of running servers for the window, read at each of the
// process monitor's looks (every 10 s) while the row shows. A click shows
// that panel through the panel's own toggle, or gives the panel on show
// the keyboard. No state of its own beyond the host's.
#import "EditorController+Private.h"
#import "ControlCommands.h"
#import "Keymap.h"
#import "ProcessMonitor.h"
#import "SidebarIcons.h"
#import "TerminalAttention.h"

namespace {
// "3", "99+", "" for none.
NSString *MCBadgeText(NSUInteger n) {
    if (n == 0) return @"";
    return n > 99 ? @"99+" : [NSString stringWithFormat:@"%lu", (unsigned long)n];
}
}  // namespace

@implementation EditorController (SidebarIcons)

- (void)sidebarIconsInstall {
    MCSidebarIconBar *bar = self.sidebarHost.iconBar;
    __weak EditorController *weakSelf = self;
    bar.onPick = ^(MCSidebarPanel panel) { [weakSelf showSidebarPanel:panel]; };
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserver:self selector:@selector(sidebarIconsAttentionChanged:)
               name:MCTerminalAttentionDidChangeNotification object:nil];
    [nc addObserver:self selector:@selector(sidebarIconsProcessesSampled:)
               name:MCProcessMonitorDidSampleNotification object:nil];
    [self sidebarIconsUpdate];
}

- (void)sidebarShowPanelView:(NSView *)view {
    self.sidebarHost.content = view;
}

- (MCSidebarPanel)sidebarPanelOnShow {
    if (self.processesVisible) return MCSidebarPanelProcesses;
    if (self.agentsVisible) return MCSidebarPanelAgents;
    if (self.gitPanelVisible) return MCSidebarPanelSourceControl;
    return MCSidebarPanelTree;
}

// "Source Control ⇧⌘G", from the keys as keys.conf has them.
static NSString *MCTipWithKey(NSString *name, NSString *commandId) {
    NSString *key = commandId ? MCKeymapGlyphs(commandId) : nil;
    return key.length ? [NSString stringWithFormat:@"%@ %@", name, key] : name;
}

- (void)sidebarIconsUpdate {
    MCSidebarIconBar *bar = self.sidebarHost.iconBar;
    if (!bar) return;
    bar.current = [self sidebarPanelOnShow];
    [bar setTip:MCTipWithKey(@"File Tree", nil) forPanel:MCSidebarPanelTree];
    [bar setTip:MCTipWithKey(@"Source Control", @"workbench.view.scm")
       forPanel:MCSidebarPanelSourceControl];
    [bar setTip:MCTipWithKey(@"Agents", @"mutible.toggleAgents") forPanel:MCSidebarPanelAgents];
    [bar setTip:MCTipWithKey(@"Processes", @"mutible.toggleProcesses")
       forPanel:MCSidebarPanelProcesses];
    [bar setBadge:MCBadgeText([self treeGitChangeCount]) forPanel:MCSidebarPanelSourceControl];
    [self sidebarIconsAttentionChanged:nil];
}

- (void)sidebarIconsAttentionChanged:(NSNotification *)note {
    (void)note;
    MCTerminalAttention *att = [MCTerminalAttention shared];
    BOOL waiting = NO;
    if (att.waitingCount > 0)
        for (TerminalView *t in [self controlTerminals])
            if ([att markForTerminal:t] == MCAttentionWaiting) { waiting = YES; break; }
    [self.sidebarHost.iconBar setDot:waiting forPanel:MCSidebarPanelAgents];
}

- (void)sidebarIconsProcessesSampled:(NSNotification *)note {
    (void)note;
    [self sidebarIconsCountServers];
}

// Only while the row shows: a hidden row is counted again at the next look.
- (void)sidebarIconsCountServers {
    MCSidebarHost *host = self.sidebarHost;
    if (!host.iconsShown || !self.window) return;
    NSString *folder = MCProcessRealPath(self.rootPath ?: @"");
    const Procs::View v = [[MCProcessMonitor shared] viewForWindow:MCControlWindowID(self)
                                                            folder:folder];
    [host.iconBar setBadge:MCBadgeText((NSUInteger)Procs::countServers(v))
                  forPanel:MCSidebarPanelProcesses];
}

- (void)sidebarIconsApplySettings {
    AppSettings *cfg = [AppSettings shared];
    MCSidebarHost *host = self.sidebarHost;
    [host.iconBar setBackground:[cfg background:Surface::Sidebar] text:[cfg text:Surface::Sidebar]];
    const BOOL was = host.showsIcons;
    host.showsIcons = cfg.settings.sidebarIcons();
    if (host.showsIcons && !was) [self sidebarIconsCountServers];
}

- (void)showSidebarPanel:(MCSidebarPanel)panel {
    const MCSidebarPanel now = [self sidebarPanelOnShow];
    if (panel == now) {   // on show already: it takes the keyboard
        [self focusTree:nil];
        return;
    }
    switch (panel) {
    case MCSidebarPanelTree:   // the toggle of the panel on show gives the tree back
        if (now == MCSidebarPanelSourceControl) [self toggleSourceControl:nil];
        else if (now == MCSidebarPanelAgents) [self toggleAgents:nil];
        else [self toggleProcesses:nil];
        break;
    case MCSidebarPanelSourceControl: [self toggleSourceControl:nil]; break;
    case MCSidebarPanelAgents: [self toggleAgents:nil]; break;
    case MCSidebarPanelProcesses: [self toggleProcesses:nil]; break;
    }
}

@end
