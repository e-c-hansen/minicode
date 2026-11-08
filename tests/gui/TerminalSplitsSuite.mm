// TerminalSplitsSuite.mm — `make guitest SUITES=terminal_splits`: the
// terminal dock's tabs and splits (TerminalPanel.h, TerminalLayout.h), in
// real zsh shells.
//   - Split Terminal puts a terminal beside the active one, Split Terminal
//     Down one below it in its column; frames fill the dock with a bar on
//     every boundary, and `stty size` in each shell says its own pane's size.
//   - Dragging a bar (the DragBar's own mouse methods, as the harness drags
//     NSSplitView) resizes the panes either side, stopping at 20 columns;
//     the shell hears the new width.
//   - Ctrl+Cmd+arrows through the menu: Right widens the left pane by 4
//     cells, Up in a stack moves its boundary, Up for a pane alone in its
//     column grows the dock; with the keyboard in the editor they are off.
//   - Splitting until no room: refused once a pane would be under 20
//     columns (or 5 rows stacked), every pane at least that, a status note.
//   - Option+Cmd+] and [ move the keyboard between panes.
//   - Rename Terminal… wins over a program's OSC title and `term --title`,
//     an empty name gives the title back, Cancel changes nothing.
//   - A running command in a pane is asked about before it closes; Cancel
//     keeps it, Close closes it and its neighbour takes its space.
//   - Tabs drag to a new place along the strip, and Move Terminal Left and
//     Right move them, each terminal keeping its shell (`echo $$`).
#import "Harness.h"
#import "EditorController.h"
#import "EditorPanes.h"
#import "TabStrip.h"
#import "Terminal.h"
#import "TerminalGridView.h"
#import "TerminalPanel.h"
#import <Carbon/Carbon.h>   // kVK_*

@interface EditorController (SplitsSuite)
- (void)relayoutRightArea;   // EditorController+Layout.mm
@end

@interface MCSplitsState : NSObject
@property(nonatomic, strong) EditorController *c;
@property(nonatomic, strong) MCTerminalPanel *panel;
@property(nonatomic, strong) TerminalView *t1, *t2, *t3, *other;
@property(nonatomic) NSInteger tab;              // the tab of t1, t2, t3
@property(nonatomic) CGFloat before;             // a width or height, before a change
@property(nonatomic, strong) NSMutableArray<TerminalView *> *extra;
@property(nonatomic) pid_t pid1, pidOther;
@end
@implementation MCSplitsState
@end

namespace {

NSString *Scratch(NSString *name) {
    return [MCGuiScratch() stringByAppendingPathComponent:[@"splits-" stringByAppendingString:name]];
}

NSString *ReadFile(NSString *path) {
    NSString *s = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    return [s stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

BOOL Idle(TerminalView *t) { return t.atPrompt && !t.busy; }

// "rows cols", as `stty size` prints it, for the terminal's pane now
// (TerminalView updateWindowSize's arithmetic).
NSString *SizeFor(TerminalView *t) {
    NSSize cell = [TerminalGridView cellSize];
    CGFloat pad = [TerminalGridView padding];
    int cols = MAX(2, (int)floor((t.bounds.size.width - 2 * pad) / cell.width));
    int rows = MAX(2, (int)floor((t.bounds.size.height - 2 * pad) / cell.height));
    return [NSString stringWithFormat:@"%d %d", rows, cols];
}

// A mouse event at a point in the window.
NSEvent *Mouse(NSWindow *w, NSEventType type, NSPoint p, NSEventModifierFlags flags) {
    return [NSEvent mouseEventWithType:type location:p modifierFlags:flags timestamp:0
                          windowNumber:w.windowNumber context:nil eventNumber:0
                            clickCount:1 pressure:1];
}

// Drags a view from one point to another (its own coordinates for the
// start; `by` is added to it), through its own mouse methods.
void Drag(NSView *v, NSPoint start, NSSize by) {
    NSWindow *w = v.window;
    NSPoint a = [v convertPoint:start toView:nil];
    NSPoint b = NSMakePoint(a.x + by.width, a.y + by.height);
    [v mouseDown:Mouse(w, NSEventTypeLeftMouseDown, a, 0)];
    NSPoint mid = NSMakePoint((a.x + b.x) / 2, (a.y + b.y) / 2);
    [v mouseDragged:Mouse(w, NSEventTypeLeftMouseDragged, mid, 0)];
    [v mouseDragged:Mouse(w, NSEventTypeLeftMouseDragged, b, 0)];
    [v mouseUp:Mouse(w, NSEventTypeLeftMouseUp, b, 0)];
}

// The menu bar's Terminal menu (its item in the bar has no title of its
// own, so itemWithTitle: finds nothing).
NSMenu *TerminalMenu() {
    for (NSMenuItem *it in NSApp.mainMenu.itemArray)
        if ([it.submenu.title isEqualToString:@"Terminal"]) return it.submenu;
    return nil;
}

DragBar *Bar(MCTerminalPanel *panel, BOOL vertical, NSUInteger nth) {
    NSUInteger seen = 0;
    for (DragBar *bar in [panel splitBars])
        if (bar.vertical == vertical && seen++ == nth) return bar;
    return nil;
}

// Runs `stty size` into a file in each terminal; checked by CheckSizes.
void AskSizes(NSArray<TerminalView *> *terms, NSString *tag) {
    for (NSUInteger i = 0; i < terms.count; i++) {
        NSString *file = Scratch([NSString stringWithFormat:@"%@-%lu", tag, (unsigned long)i]);
        [[NSFileManager defaultManager] removeItemAtPath:file error:nil];
        [terms[i] runCommand:[NSString stringWithFormat:@"stty size > '%@'", file]];
    }
}

BOOL SizesArrived(NSUInteger n, NSString *tag) {
    for (NSUInteger i = 0; i < n; i++)
        if (!ReadFile(Scratch([NSString stringWithFormat:@"%@-%lu", tag, (unsigned long)i])).length)
            return NO;
    return YES;
}

void CheckSizes(NSArray<TerminalView *> *terms, NSString *tag) {
    for (NSUInteger i = 0; i < terms.count; i++) {
        NSString *got = ReadFile(Scratch([NSString stringWithFormat:@"%@-%lu", tag, (unsigned long)i]));
        GCHECK_EQ_STR(got, SizeFor(terms[i]));
    }
}

// Every terminal of a tab within the dock, none overlapping, side by side
// or stacked with a 1-point gap.
void CheckTiled(MCTerminalPanel *panel, NSInteger tab) {
    NSArray<NSArray<TerminalView *> *> *columns = [panel columnsInTab:(NSUInteger)tab];
    CGFloat W = panel.bounds.size.width, H = panel.bounds.size.height - kMCTabStripHeight;
    CGFloat x = 0;
    for (NSArray<TerminalView *> *column in columns) {
        CGFloat top = H;
        CGFloat w = column.firstObject.frame.size.width;
        GCHECK(fabs(NSMinX(column.firstObject.frame) - x) < 0.5);
        for (TerminalView *t in column) {
            GCHECK(fabs(NSMaxY(t.frame) - top) < 0.5);
            GCHECK(t.frame.size.width == w);
            GCHECK(!t.hidden);
            top = NSMinY(t.frame) - 1;
        }
        GCHECK(fabs(top + 1) < 0.5);   // the last reaches the bottom
        x += w + 1;
    }
    GCHECK(fabs(x - 1 - W) < 0.5);     // the last column reaches the right edge
}

}  // namespace

MC_GUI_SUITE(terminal_splits) {
    MCSplitsState *s = [MCSplitsState new];
    s.extra = [NSMutableArray array];

    MCGuiStep(@"a terminal tab of its own", ^{
        MCGuiSetSettings(@"lsp.enabled = false\n");
        s.c = MCGuiController();
        [s.c newTerminal:nil];
        s.panel = [s.c valueForKey:@"terminalPanel"];
        s.t1 = s.panel.activeTerminal;
        s.tab = s.panel.selectedTab;
        GCHECK(s.t1 != nil && s.tab >= 0);
        GCHECK([s.panel terminalsInTab:(NSUInteger)s.tab].count == 1);
        GCHECK([s.panel splitBars].count == 0);
        // A dock tall enough for a stack of two with room to move its bar.
        [s.c setValue:@(3 * [MCTerminalPanel minPaneHeight] + kMCTabStripHeight + 60)
               forKey:@"terminalHeight"];
        [s.c relayoutRightArea];
        MCGuiNote(@"window %@, dock %@", NSStringFromSize(s.c.window.frame.size),
                  NSStringFromSize(s.panel.frame.size));
        GCHECK(s.panel.frame.size.height - kMCTabStripHeight >= 3 * [MCTerminalPanel minPaneHeight]);
    });
    MCGuiWait(@"its prompt", 20, ^BOOL { return Idle(s.t1); });

    // ---- split beside, then down
    MCGuiStep(@"Split Terminal, then Split Terminal Down on the new one", ^{
        [NSApp sendAction:@selector(splitTerminal:) to:nil from:nil];
        s.t2 = s.panel.activeTerminal;
        GCHECK(s.t2 && s.t2 != s.t1);
        [NSApp sendAction:@selector(splitTerminalDown:) to:nil from:nil];
        s.t3 = s.panel.activeTerminal;
        GCHECK(s.t3 && s.t3 != s.t2);
        NSArray *cols = [s.panel columnsInTab:(NSUInteger)s.tab];
        GCHECK(cols.count == 2);
        if (cols.count == 2) {
            GCHECK([cols[0] isEqual:@[s.t1]]);
            GCHECK([cols[1] isEqual:(@[s.t2, s.t3])]);
        }
        // Beside: halves of the width; below: halves of the second column.
        GCHECK(NSMinX(s.t2.frame) > NSMaxX(s.t1.frame));
        GCHECK(NSMaxY(s.t3.frame) < NSMinY(s.t2.frame));
        GCHECK(fabs(s.t1.frame.size.width - s.t2.frame.size.width) <= 1);
        GCHECK(fabs(s.t2.frame.size.height - s.t3.frame.size.height) <= 1);
        CheckTiled(s.panel, s.tab);
        // One vertical bar between the columns, one horizontal in the stack,
        // above the terminals so they take the clicks.
        GCHECK(Bar(s.panel, YES, 0) && Bar(s.panel, NO, 0) && [s.panel splitBars].count == 2);
        NSArray *sub = s.panel.subviews;
        GCHECK([sub indexOfObject:Bar(s.panel, YES, 0)] > [sub indexOfObject:s.t2]);
        GCHECK(fabs(NSMidX(Bar(s.panel, YES, 0).frame) - (NSMaxX(s.t1.frame) + 0.5)) < 0.5);
        GCHECK(fabs(NSMidY(Bar(s.panel, NO, 0).frame) - (NSMaxY(s.t3.frame) + 0.5)) < 0.5);
    });
    MCGuiWait(@"three prompts", 20, ^BOOL { return Idle(s.t1) && Idle(s.t2) && Idle(s.t3); });
    MCGuiStep(@"each shell in the project folder, asked its size", ^{
        for (TerminalView *t in @[s.t1, s.t2, s.t3])
            [t runCommand:[NSString stringWithFormat:@"cd '%@'", MCGuiProject()]];
        AskSizes(@[s.t1, s.t2, s.t3], @"first");
    });
    MCGuiWait(@"stty size from each", 20, ^BOOL { return SizesArrived(3, @"first"); });
    MCGuiStep(@"each shell has its own pane's size", ^{ CheckSizes(@[s.t1, s.t2, s.t3], @"first"); });

    // ---- the bars
    MCGuiStep(@"dragging the bar 100 points left narrows the left column", ^{
        DragBar *bar = Bar(s.panel, YES, 0);
        s.before = s.t1.frame.size.width;
        Drag(bar, NSMakePoint(NSMidX(bar.bounds), NSMidY(bar.bounds)), NSMakeSize(-100, 0));
        GCHECK(fabs(s.t1.frame.size.width - (s.before - 100)) <= 1);
        if (fabs(s.t1.frame.size.width - (s.before - 100)) > 1)
            MCGuiNote(@"left column %.1f, was %.1f", s.t1.frame.size.width, s.before);
        CheckTiled(s.panel, s.tab);
        AskSizes(@[s.t1, s.t2], @"dragged");
    });
    MCGuiWait(@"stty size after the drag", 20, ^BOOL { return SizesArrived(2, @"dragged"); });
    MCGuiStep(@"the shells hear the new widths", ^{ CheckSizes(@[s.t1, s.t2], @"dragged"); });
    MCGuiStep(@"dragged far past the edge: the column stops at 20 columns", ^{
        DragBar *bar = Bar(s.panel, YES, 0);
        Drag(bar, NSMakePoint(NSMidX(bar.bounds), NSMidY(bar.bounds)), NSMakeSize(-2000, 0));
        GCHECK(s.t1.frame.size.width == ceil([MCTerminalPanel minPaneWidth]));
        GCHECK([SizeFor(s.t1) hasSuffix:@" 20"]);
        CheckTiled(s.panel, s.tab);
    });
    MCGuiStep(@"the stack's bar dragged down 30 points", ^{
        DragBar *bar = Bar(s.panel, NO, 0);
        s.before = s.t2.frame.size.height;
        Drag(bar, NSMakePoint(NSMidX(bar.bounds), NSMidY(bar.bounds)), NSMakeSize(0, -30));
        GCHECK(fabs(s.t2.frame.size.height - (s.before + 30)) <= 1);
        GCHECK(s.t3.frame.size.height >= floor([MCTerminalPanel minPaneHeight]));
        CheckTiled(s.panel, s.tab);
    });

    // ---- the keyboard
    MCGuiStep(@"Ctrl+Cmd+Right in the left pane widens it by 4 cells", ^{
        [s.t1 focusInput];
        s.before = s.t1.frame.size.width;
        GCHECK(MCGuiPerformMenuKey(s.c.window, kVK_RightArrow,
                                   NSEventModifierFlagControl | NSEventModifierFlagCommand, nil, nil));
        CGFloat want = s.before + 4 * [TerminalGridView cellSize].width;
        GCHECK(fabs(s.t1.frame.size.width - want) <= 1);
        if (fabs(s.t1.frame.size.width - want) > 1)
            MCGuiNote(@"width %.1f, wanted %.1f", s.t1.frame.size.width, want);
        // Left takes it back.
        GCHECK(MCGuiPerformMenuKey(s.c.window, kVK_LeftArrow,
                                   NSEventModifierFlagControl | NSEventModifierFlagCommand, nil, nil));
        GCHECK(fabs(s.t1.frame.size.width - s.before) <= 1);
    });
    MCGuiStep(@"Ctrl+Cmd+Up in the lower pane of the stack grows it", ^{
        [s.t3 focusInput];
        s.before = s.t3.frame.size.height;
        GCHECK(MCGuiPerformMenuKey(s.c.window, kVK_UpArrow,
                                   NSEventModifierFlagControl | NSEventModifierFlagCommand, nil, nil));
        CGFloat want = s.before + 4 * [TerminalGridView cellSize].height;
        GCHECK(fabs(s.t3.frame.size.height - want) <= 1);
        CheckTiled(s.panel, s.tab);
    });
    MCGuiStep(@"Ctrl+Cmd+Up in a pane alone in its column grows the dock", ^{
        [s.t1 focusInput];
        s.before = s.panel.frame.size.height;
        GCHECK(MCGuiPerformMenuKey(s.c.window, kVK_UpArrow,
                                   NSEventModifierFlagControl | NSEventModifierFlagCommand, nil, nil));
        CGFloat want = s.before + 4 * [TerminalGridView cellSize].height;
        GCHECK(fabs(s.panel.frame.size.height - want) <= 1);
        if (fabs(s.panel.frame.size.height - want) > 1)
            MCGuiNote(@"dock %.1f, wanted %.1f", s.panel.frame.size.height, want);
        GCHECK(MCGuiPerformMenuKey(s.c.window, kVK_DownArrow,
                                   NSEventModifierFlagControl | NSEventModifierFlagCommand, nil, nil));
        GCHECK(fabs(s.panel.frame.size.height - s.before) <= 1);
        CheckTiled(s.panel, s.tab);
    });
    MCGuiStep(@"with the keyboard in the editor the resize items are off", ^{
        [s.c focusEditor:nil];
        GCHECK(![s.panel hasFocus]);
        NSMenu *term = TerminalMenu();
        [term update];
        NSInteger i = [term indexOfItemWithTarget:nil andAction:@selector(resizeTerminalPane:)];
        GCHECK(i >= 0 && ![term itemAtIndex:i].enabled);
        CGFloat w = s.t1.frame.size.width;
        MCGuiPerformMenuKey(s.c.window, kVK_RightArrow,
                            NSEventModifierFlagControl | NSEventModifierFlagCommand, nil, nil);
        GCHECK(s.t1.frame.size.width == w);
        // And with it in a terminal they are on.
        [s.t2 focusInput];
        [term update];
        GCHECK([term itemAtIndex:i].enabled);
    });

    // ---- moving between panes
    MCGuiStep(@"Option+Cmd+] and [ move the keyboard between the panes", ^{
        [s.t1 focusInput];
        GCHECK(MCGuiPerformMenuKey(s.c.window, kVK_ANSI_RightBracket,
                                   NSEventModifierFlagOption | NSEventModifierFlagCommand, nil, nil));
        GCHECK([s.t2 hasFocus] && s.panel.activeTerminal == s.t2);
        GCHECK(MCGuiPerformMenuKey(s.c.window, kVK_ANSI_RightBracket,
                                   NSEventModifierFlagOption | NSEventModifierFlagCommand, nil, nil));
        GCHECK([s.t3 hasFocus] && s.panel.activeTerminal == s.t3);
        GCHECK(MCGuiPerformMenuKey(s.c.window, kVK_ANSI_LeftBracket,
                                   NSEventModifierFlagOption | NSEventModifierFlagCommand, nil, nil));
        GCHECK([s.t2 hasFocus] && s.panel.activeTerminal == s.t2);
    });

    // ---- Ctrl+Shift+W with the keyboard in a pane
    MCGuiStep(@"Ctrl+Shift+W in a pane closes that pane", ^{
        TerminalView *t4 = [s.panel splitActiveDown];   // below t2, above t3
        GCHECK(t4 && [t4 hasFocus]);
        NSUInteger before = [s.panel terminalsInTab:(NSUInteger)s.tab].count;
        // The panel answers Close Terminal's action itself (it has a method
        // of that name), so this once closed nothing.
        GCHECK(MCGuiPerformMenuKey(s.c.window, kVK_ANSI_W,
                                   NSEventModifierFlagControl | NSEventModifierFlagShift, nil, nil));
        NSArray *left = [s.panel terminalsInTab:(NSUInteger)s.tab];
        GCHECK(left.count == before - 1 && ![left containsObject:t4]);
        GCHECK([left containsObject:s.t2] && [left containsObject:s.t3]);
        CheckTiled(s.panel, s.tab);
        [s.t2 focusInput];
    });

    // ---- rename against a program's title
    MCGuiStep(@"a program sets its title", ^{
        [s.t2 runCommand:@"printf '\\033]2;osc-title\\007'; sleep 3"];
    });
    MCGuiWait(@"the tab shows it", 10, ^BOOL { return [s.t2.title isEqualToString:@"osc-title"]; });
    MCGuiStep(@"Rename Terminal… wins over it, and over --title", ^{
        s.panel.namePrompt = ^NSString *(TerminalView *t) {
            GCHECK_EQ_STR(t.title, @"osc-title");
            return @"  my server ";
        };
        [s.panel selectTerminal:s.t2];
        [NSApp sendAction:@selector(renameTerminal:) to:nil from:nil];
        GCHECK_EQ_STR(s.t2.title, @"my server");
        s.t2.fixedTitle = @"from the agent";
        GCHECK_EQ_STR(s.t2.title, @"my server");
        MCTabStrip *strip = [s.panel valueForKey:@"strip"];
        GCHECK([strip.items[(NSUInteger)s.tab].title containsString:@"my server"]);
        // An empty name gives back what was there.
        s.panel.namePrompt = ^NSString *(TerminalView *) { return @""; };
        GCHECK([s.panel renameTerminal:s.t2]);
        GCHECK_EQ_STR(s.t2.title, @"from the agent");
        s.t2.fixedTitle = nil;
        GCHECK_EQ_STR(s.t2.title, @"osc-title");
        // The question itself: Cancel changes nothing.
        s.panel.namePrompt = nil;
        NSUInteger shown = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);
        GCHECK(![s.panel renameTerminal:s.t2]);
        GCHECK(MCGuiAlertsShown().count == shown + 1);
        GCHECK_EQ_STR(MCGuiAlertsShown().lastObject, @"Rename Terminal");
        GCHECK_EQ_STR(s.t2.title, @"osc-title");
        // In the tab's menu too.
        NSMenu *m = strip.menuForTab(s.tab);
        GCHECK([m indexOfItemWithTitle:@"Rename Terminal…"] >= 0);
        GCHECK([m indexOfItemWithTitle:@"Split Terminal Down"] >= 0);
    });
    MCGuiWait(@"the program ends", 10, ^BOOL { return Idle(s.t2); });

    // ---- closing a pane with something running
    MCGuiStep(@"sleep in the lower pane", ^{ [s.t3 runCommand:@"sleep 60"]; });
    MCGuiWait(@"it runs", 10, ^BOOL { return s.t3.runningCommands.count > 0; });
    MCGuiStep(@"closing it asks; Cancel keeps it, Close closes it", ^{
        NSUInteger shown = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);
        GCHECK(![s.panel closeTerminal:s.t3]);
        GCHECK(MCGuiAlertsShown().count == shown + 1);
        GCHECK_EQ_STR(MCGuiAlertsShown().lastObject, @"Close the terminal?");
        GCHECK([[s.panel terminalsInTab:(NSUInteger)s.tab] containsObject:s.t3]);
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        GCHECK([s.panel closeTerminal:s.t3]);
        s.t3 = nil;
        NSArray *cols = [s.panel columnsInTab:(NSUInteger)s.tab];
        GCHECK(cols.count == 2 && [cols[1] isEqual:@[s.t2]]);
        GCHECK(Bar(s.panel, NO, 0) == nil && [s.panel splitBars].count == 1);
        CheckTiled(s.panel, s.tab);   // t2 takes the whole column
    });

    // ---- splitting until there is no room
    MCGuiStep(@"split beside until refused", ^{
        [s.panel selectTerminal:s.t2];
        __block NSString *note = nil;
        void (^old)(NSString *) = s.panel.onNote;
        s.panel.onNote = ^(NSString *n) { note = n; };
        TerminalView *t = nil;
        int made = 0;
        while ((t = [s.panel splitActive]) && made < 30) {
            [s.extra addObject:t];
            made++;
        }
        s.panel.onNote = old;
        GCHECK(t == nil && made > 0 && made < 30);
        GCHECK([note containsString:@"20 columns"]);
        for (TerminalView *p in [s.panel terminalsInTab:(NSUInteger)s.tab])
            GCHECK(p.frame.size.width + 1e-6 >= [MCTerminalPanel minPaneWidth]);
        CheckTiled(s.panel, s.tab);
        GCHECK([s.panel splitBars].count == [s.panel columnsInTab:(NSUInteger)s.tab].count - 1);
    });
    MCGuiStep(@"split down until refused", ^{
        __block NSString *note = nil;
        void (^old)(NSString *) = s.panel.onNote;
        s.panel.onNote = ^(NSString *n) { note = n; };
        TerminalView *t = nil;
        int made = 0;
        while ((t = [s.panel splitActiveDown]) && made < 30) {
            [s.extra addObject:t];
            made++;
        }
        s.panel.onNote = old;
        GCHECK(t == nil && made < 30);
        GCHECK([note containsString:@"5 rows"]);
        for (TerminalView *p in [s.panel terminalsInTab:(NSUInteger)s.tab])
            GCHECK(p.frame.size.height + 1e-6 >= [MCTerminalPanel minPaneHeight]);
        CheckTiled(s.panel, s.tab);
    });
    MCGuiStep(@"the extra terminals close, unasked", ^{
        NSUInteger shown = MCGuiAlertsShown().count;
        for (TerminalView *t in s.extra) GCHECK([s.panel closeTerminal:t]);
        [s.extra removeAllObjects];
        GCHECK(MCGuiAlertsShown().count == shown);
        NSArray *cols = [s.panel columnsInTab:(NSUInteger)s.tab];
        GCHECK(cols.count == 2 && [cols[0] isEqual:@[s.t1]] && [cols[1] isEqual:@[s.t2]]);
        CheckTiled(s.panel, s.tab);
    });

    // ---- reordering tabs
    MCGuiStep(@"a second tab", ^{
        [s.c newTerminal:nil];
        s.other = s.panel.activeTerminal;
        GCHECK(s.other && s.panel.selectedTab == s.tab + 1);
    });
    MCGuiWait(@"its prompt", 20, ^BOOL { return Idle(s.other); });
    MCGuiStep(@"echo $$ in each", ^{
        [s.t1 runCommand:[NSString stringWithFormat:@"echo $$ > '%@'", Scratch(@"pid1")]];
        [s.other runCommand:[NSString stringWithFormat:@"echo $$ > '%@'", Scratch(@"pid2")]];
    });
    MCGuiWait(@"both", 10, ^BOOL {
        return ReadFile(Scratch(@"pid1")).length && ReadFile(Scratch(@"pid2")).length;
    });
    MCGuiStep(@"each is its shell", ^{
        s.pid1 = ReadFile(Scratch(@"pid1")).intValue;
        s.pidOther = ReadFile(Scratch(@"pid2")).intValue;
        GCHECK(s.pid1 == s.t1.shellPid && s.pidOther == s.other.shellPid && s.pid1 != s.pidOther);
    });
    MCGuiStep(@"the second tab dragged onto the first: it goes before it", ^{
        MCTabStrip *strip = [s.panel valueForKey:@"strip"];
        NSRect from = [strip rectForTab:s.tab + 1], to = [strip rectForTab:s.tab];
        GCHECK(!NSIsEmptyRect(from) && !NSIsEmptyRect(to));
        NSPoint start = NSMakePoint(NSMidX(from) - 20, NSMidY(from));
        NSWindow *w = strip.window;
        NSPoint a = [strip convertPoint:start toView:nil];
        [strip mouseDown:Mouse(w, NSEventTypeLeftMouseDown, a, 0)];
        // Under 4 points it is still a click.
        [strip mouseDragged:Mouse(w, NSEventTypeLeftMouseDragged, NSMakePoint(a.x - 3, a.y), 0)];
        GCHECK(strip.dropTarget == -1);
        NSPoint b = [strip convertPoint:NSMakePoint(NSMinX(to) + 5, NSMidY(to)) toView:nil];
        [strip mouseDragged:Mouse(w, NSEventTypeLeftMouseDragged, b, 0)];
        GCHECK(strip.dropTarget == s.tab);
        [strip mouseUp:Mouse(w, NSEventTypeLeftMouseUp, b, 0)];
        GCHECK(strip.dropTarget == -1);
        GCHECK([[s.panel terminalsInTab:(NSUInteger)s.tab] isEqual:@[s.other]]);
        GCHECK([[s.panel terminalsInTab:(NSUInteger)s.tab + 1] isEqual:(@[s.t1, s.t2])]);
        GCHECK(s.panel.selectedTab == s.tab);   // still the one on show
        GCHECK(!s.other.hidden && s.t1.hidden);
    });
    MCGuiStep(@"let go far below the strip, a drag moves nothing", ^{
        MCTabStrip *strip = [s.panel valueForKey:@"strip"];
        NSRect from = [strip rectForTab:s.tab];
        NSRect to = [strip rectForTab:s.tab + 1];
        NSWindow *w = strip.window;
        NSPoint a = [strip convertPoint:NSMakePoint(NSMidX(from), NSMidY(from)) toView:nil];
        NSPoint b = [strip convertPoint:NSMakePoint(NSMaxX(to) - 5, NSMidY(to) + 200) toView:nil];
        [strip mouseDown:Mouse(w, NSEventTypeLeftMouseDown, a, 0)];
        [strip mouseDragged:Mouse(w, NSEventTypeLeftMouseDragged, b, 0)];
        [strip mouseUp:Mouse(w, NSEventTypeLeftMouseUp, b, 0)];
        GCHECK([[s.panel terminalsInTab:(NSUInteger)s.tab] isEqual:@[s.other]]);
    });
    MCGuiStep(@"Move Terminal Right and Left from the menu", ^{
        [s.panel selectTab:(NSUInteger)s.tab];
        NSMenu *term = TerminalMenu();
        [term update];
        NSMenuItem *right = [term itemWithTitle:@"Move Terminal Right"];
        GCHECK(right.enabled);
        [NSApp sendAction:@selector(moveTerminalRight:) to:nil from:right];
        GCHECK([[s.panel terminalsInTab:(NSUInteger)s.tab + 1] isEqual:@[s.other]]);
        GCHECK(s.panel.selectedTab == s.tab + 1);
        [NSApp sendAction:@selector(moveTerminalLeft:) to:nil from:nil];
        GCHECK([[s.panel terminalsInTab:(NSUInteger)s.tab] isEqual:@[s.other]]);
    });
    MCGuiStep(@"echo $$ again", ^{
        [s.t1 runCommand:[NSString stringWithFormat:@"echo $$ > '%@'", Scratch(@"pid1b")]];
        [s.other runCommand:[NSString stringWithFormat:@"echo $$ > '%@'", Scratch(@"pid2b")]];
    });
    MCGuiWait(@"both again", 10, ^BOOL {
        return ReadFile(Scratch(@"pid1b")).length && ReadFile(Scratch(@"pid2b")).length;
    });
    MCGuiStep(@"each terminal kept its shell", ^{
        GCHECK(ReadFile(Scratch(@"pid1b")).intValue == s.pid1);
        GCHECK(ReadFile(Scratch(@"pid2b")).intValue == s.pidOther);
    });

    MCGuiStep(@"the tabs close", ^{
        GCHECK([s.panel closeTerminal:s.other]);
        GCHECK([s.panel closeTerminal:s.t2]);
        GCHECK([s.panel closeTerminal:s.t1]);
    });
}
