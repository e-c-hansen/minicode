// EditorController+Processes.mm — EditorController: the hooks of the
// Processes panel (ProcessesPanel.h), which takes the sidebar's place as
// Agents does. The panel and whether it shows are kept as associated
// objects, so the class gains no ivars.
#import "EditorController+Private.h"
#import "ControlCommands.h"
#import "ProcessesPanel.h"
#import "Terminal.h"
#import "TerminalPanel.h"
#include <objc/runtime.h>

static char kProcessesPanelKey, kProcessesShownKey;

@implementation EditorController (Processes)

- (MCProcessesPanel *)processesPanel {
    MCProcessesPanel *panel = objc_getAssociatedObject(self, &kProcessesPanelKey);
    if (panel) return panel;
    panel = [MCProcessesPanel new];
    __weak EditorController *weakSelf = self;
    panel.windowID = ^NSInteger { return MCControlWindowID(weakSelf); };
    panel.folder = ^NSString * { return weakSelf.rootPath; };
    panel.onOpenURL = ^(NSString *url) {
        MCTermLink *link = [MCTermLink new];
        link.target = url;
        link.isURL = YES;
        [weakSelf openTerminalLink:link];
    };
    panel.onRevealTerminal = ^BOOL(NSInteger terminal) {
        EditorController *s = weakSelf;
        for (TerminalView *t in [s controlTerminals])
            if (MCControlTerminalID(t) == terminal) {
                [s focusChangeTerminal:t];
                return YES;
            }
        return NO;
    };
    panel.onNote = ^(NSString *note) { [weakSelf showStatusNote:note]; };
    panel.onTab = ^{ [weakSelf focusEditor:nil]; };
    objc_setAssociatedObject(self, &kProcessesPanelKey, panel, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return panel;
}

- (BOOL)processesVisible {
    return [objc_getAssociatedObject(self, &kProcessesShownKey) boolValue];
}

- (void)setProcessesVisible:(BOOL)shown {
    objc_setAssociatedObject(self, &kProcessesShownKey, shown ? @YES : nil,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

- (MCProcessesPanel *)processesPanelOnShow {
    return self.processesVisible ? [self processesPanel] : nil;
}

// The panel takes the sidebar's place, and gives the tree back when toggled
// again; the sidebar's width is kept, and a collapsed sidebar opens for it.
- (void)toggleProcesses:(id)sender {
    (void)sender;
    MCProcessesPanel *panel = [self processesPanel];
    NSView *from = [self sidebarViewOnShow];
    const BOOL show = !self.processesVisible;
    NSView *to = show ? panel : self.sidebarScroll;
    to.frame = from.frame;
    if (from != to) [self.splitView replaceSubview:from with:to];
    self.processesVisible = show;
    self.gitPanelVisible = NO;
    self.agentsVisible = NO;
    if (self.sidebarCollapsed || from.frame.size.width < 1) {
        self.sidebarCollapsed = NO;
        [self.splitView setPosition:260 ofDividerAtIndex:0];
    }
    if (show) {
        [panel reload];
        [panel focusList];
    } else {
        [self.window makeFirstResponder:self.outline];
    }
    if (self.hintsVisible) [self updateHints];
}

@end
