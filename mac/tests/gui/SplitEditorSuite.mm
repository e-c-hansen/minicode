// SplitEditorSuite.mm — `make guitest SUITES=split_editor`: two editor
// groups side by side (EditorController+Groups.mm, EditorMirror.h). Cmd+\
// in the editor splits it, the tab on show in a new group to the right
// that takes the keyboard; the same file in both groups is one document,
// its text storage laid out twice, so typing in one shows in the other and
// undo from either group takes it back; different files show in each; a
// click in the other group moves the editor there at the character
// clicked; Cmd+1 and Cmd+2 focus the groups; Ctrl+Cmd+Left and Right move
// the tab on show to the other group; closing a group's last tab closes the
// group, asking nothing when the other group still holds the file; the
// session remembers the split; and the terminal keeps Cmd+\ while it has
// the keyboard.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "EditorMirror.h"
#import "TabStrip.h"
#import "TerminalPanel.h"
#import <Carbon/Carbon.h>

namespace {

NSString *Dir(void) { return [MCGuiScratch() stringByAppendingPathComponent:@"split-editor"]; }
NSString *F(NSString *rel) { return [Dir() stringByAppendingPathComponent:rel]; }

NSArray<MCTab *> *TabsOf(EditorController *c) { return [c valueForKey:@"tabs"]; }
MCTab *Active(EditorController *c) { return [c valueForKey:@"activeTab"]; }
MCEditorMirror *Mirror(EditorController *c) { return [c valueForKey:@"mirror"]; }
NSArray<MCTab *> *Parked(EditorController *c) { return [c valueForKey:@"parkedTabs"]; }
MCTab *ParkedActive(EditorController *c) { return [c valueForKey:@"parkedActive"]; }
CodeTextView *Editor(EditorController *c) { return [c valueForKey:@"textView"]; }
MCTabStrip *Strip(EditorController *c) { return [c valueForKey:@"tabStrip"]; }
MCTabStrip *ParkedStrip(EditorController *c) { return [c valueForKey:@"parkedStrip"]; }

NSString *Names(NSArray<MCTab *> *tabs) {
    NSMutableArray *names = [NSMutableArray array];
    for (MCTab *t in tabs) [names addObject:t.currentPath.lastPathComponent ?: @"?"];
    return [names componentsJoinedByString:@" "];
}

// Both groups as "left | right", the one with the keyboard starred.
NSString *Layout(EditorController *c) {
    if (![c editorSplit]) return Names(TabsOf(c));
    NSString *shown = Names([c groupTabs]), *parked = Names(Parked(c));
    const BOOL left = [c focusedGroup] == 0;
    return left ? [NSString stringWithFormat:@"*%@ | %@", shown, parked]
                : [NSString stringWithFormat:@"%@ | *%@", parked, shown];
}

NSString *Text(NSString *rel) {
    return [NSString stringWithContentsOfFile:F(rel) encoding:NSUTF8StringEncoding error:nil];
}

void Type(EditorController *c, NSString *text, NSUInteger at) {
    CodeTextView *tv = Editor(c);
    MCGuiUndoGroup(tv.undoManager, ^{
        tv.selectedRange = NSMakeRange(at, 0);
        [tv insertText:text replacementRange:NSMakeRange(at, 0)];
    });
}

// Cmd+\ as the key monitor gets it.
void SplitKey(EditorController *c) {
    MCGuiSendKeyToApp(c.window, kVK_ANSI_Backslash, NSEventModifierFlagCommand, @"\\", @"\\");
}

// A press on a character of the group without the keyboard (a window
// never on screen gets no mouse-down through sendEvent:).
void PressOtherGroup(EditorController *c, NSUInteger character) {
    MCEditorMirror *m = Mirror(c);
    const NSRect box = [m windowRectOfCharacter:character];
    const NSPoint at = NSMakePoint(NSMinX(box) + 1, NSMidY(box));
    const NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
    NSEvent *down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:at
                                  modifierFlags:0 timestamp:now
                                   windowNumber:c.window.windowNumber context:nil
                                    eventNumber:0 clickCount:1 pressure:1];
    [m.textView mouseDown:down];
}

NSDictionary *SavedSession(EditorController *c) {
    NSUserDefaults *d = [[NSUserDefaults alloc]
        initWithSuiteName:NSProcessInfo.processInfo.environment[@"MINICODE_DEFAULTS_SUITE"]];
    return [d dictionaryForKey:@"MCTabSessions"][c.rootPath.stringByResolvingSymlinksInPath];
}

NSString *const kA = @"def one():\n    return 1\n\n\ndef two():\n    return 2\n";
NSString *const kB = @"bravo\nline 2\n";
NSString *const kC = @"charlie\n";
NSString *const kM = @"# Title\n\n> a quote\n\n```\ncode\n```\n\n| a | b |\n|---|---|\n| 1 | 2 |\n";

}  // namespace

MC_GUI_SUITE(split_editor) {
    __block EditorController *c = nil;
    __block TerminalView *term = nil;

    MCGuiStep(@"a folder of files, a window of its own, two tabs", ^{
        MCGuiWriteFile(F(@"a.py"), kA);
        MCGuiWriteFile(F(@"b.txt"), kB);
        MCGuiWriteFile(F(@"c.txt"), kC);
        const NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:Dir()]]];
        c = MCGuiController();
        GCHECK(MCGuiControllers().count == before + 1);
        MCGuiSetKeyWindow(c.window);
        [c openFileAtPath:F(@"b.txt") from:EditorTabs::OpenFrom::Other];
        [c openFileAtPath:F(@"a.py") from:EditorTabs::OpenFrom::Other];
        GCHECK_EQ_STR(Layout(c), @"b.txt a.py");
        GCHECK(![c editorSplit]);
        [c.window makeFirstResponder:Editor(c)];
    });

    MCGuiStep(@"Cmd+\\ in the editor: the file in a group to the right, with the keyboard", ^{
        SplitKey(c);
        GCHECK([c editorSplit]);
        GCHECK_EQ_STR(Layout(c), @"b.txt a.py | *a.py");
        GCHECK([Active(c).currentPath isEqualToString:F(@"a.py")]);
        GCHECK(ParkedActive(c) == Active(c));   // one document, shown twice
        GCHECK(TabsOf(c).count == 2);
        GCHECK(c.window.firstResponder == Editor(c));
        // The other group lays out the editor's own storage.
        MCEditorMirror *m = Mirror(c);
        GCHECK(m.sharedStorage == Editor(c).textStorage);
        GCHECK_EQ_STR(m.textView.string, kA);
        GCHECK(!m.scrollView.hidden && !ParkedStrip(c).hidden && !Strip(c).hidden);
        // Side by side: the other group on the left, the editor on the right.
        const NSRect left = [m.scrollView convertRect:m.scrollView.bounds toView:nil];
        NSScrollView *es = Editor(c).enclosingScrollView;
        const NSRect right = [es convertRect:es.bounds toView:nil];
        MCGuiNote(@"left %@, right %@", NSStringFromRect(left), NSStringFromRect(right));
        GCHECK(NSMaxX(left) <= NSMinX(right) + 1 && NSWidth(left) > 100 && NSWidth(right) > 100);
        GCHECK(ParkedStrip(c).items.count == 2 && Strip(c).items.count == 1);
        MCGuiSnapshot(c.window.contentView, @"split-editor-same-file");
    });

    MCGuiStep(@"typing in one group shows in the other at once", ^{
        Type(c, @"# hello\n", 0);
        GCHECK([Editor(c).string hasPrefix:@"# hello\n"]);
        GCHECK_EQ_STR(Mirror(c).textView.string, Editor(c).string);
        GCHECK(Active(c).dirty);
        // Both strips show the unsaved dot.
        GCHECK(Strip(c).items.firstObject.dirty);
        GCHECK(ParkedStrip(c).items[1].dirty);
    });

    MCGuiStep(@"Cmd+1: the left group takes the keyboard, the same document", ^{
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_ANSI_1, NSEventModifierFlagCommand, nil, nil));
        GCHECK([c focusedGroup] == 0);
        GCHECK_EQ_STR(Layout(c), @"*b.txt a.py | a.py");
        GCHECK([Active(c).currentPath isEqualToString:F(@"a.py")]);
        GCHECK(Mirror(c).sharedStorage == Editor(c).textStorage);
        GCHECK(c.window.firstResponder == Editor(c));
        GCHECK(Strip(c).items.count == 2 && ParkedStrip(c).items.count == 1);
    });

    MCGuiStep(@"undo from the other group takes the typing back in both", ^{
        [Editor(c).undoManager undo];
        GCHECK_EQ_STR(Editor(c).string, kA);
        GCHECK_EQ_STR(Mirror(c).textView.string, kA);
        GCHECK(!Active(c).dirty);
        [Editor(c).undoManager redo];
        GCHECK([Mirror(c).textView.string hasPrefix:@"# hello\n"]);
        [Editor(c).undoManager undo];
        GCHECK(!Active(c).dirty);
    });

    MCGuiStep(@"different files: b.txt here, a.py still in the other group", ^{
        [c selectTabAtIndex:0];
        GCHECK([Active(c).currentPath isEqualToString:F(@"b.txt")]);
        GCHECK_EQ_STR(Editor(c).string, kB);
        MCEditorMirror *m = Mirror(c);
        GCHECK(m.sharedStorage == nil);   // its own copy now
        GCHECK_EQ_STR(m.textView.string, kA);
        // Its copy keeps the editor's colors: `def` is not plain.
        NSColor *plain = [[AppSettings shared] text:Surface::Editor];
        NSColor *def = [m.textView.textStorage attribute:NSForegroundColorAttributeName
                                                 atIndex:0 effectiveRange:NULL];
        GCHECK(def && ![def isEqual:plain]);
        Type(c, @"edited ", 0);
        GCHECK_EQ_STR(m.textView.string, kA);   // the other file's group is untouched
        GCHECK_EQ_STR(ParkedActive(c).currentPath.lastPathComponent, @"a.py");
        MCGuiSnapshot(c.window.contentView, @"split-editor-two-files");
    });

    MCGuiStep(@"a click in the other group: the editor goes there, caret where clicked", ^{
        const NSUInteger at = [kA rangeOfString:@"two"].location;
        PressOtherGroup(c, at);
        GCHECK([c focusedGroup] == 1);
        GCHECK_EQ_STR(Layout(c), @"b.txt a.py | *a.py");
        GCHECK([Active(c).currentPath isEqualToString:F(@"a.py")]);
        GCHECK_EQ_STR(Editor(c).string, kA);
        MCGuiNote(@"selection %@, wanted %lu", NSStringFromRange(Editor(c).selectedRange),
                  (unsigned long)at);
        GCHECK(Editor(c).selectedRange.location == at && Editor(c).selectedRange.length == 0);
        GCHECK(c.window.firstResponder == Editor(c));
        // The group it left shows b.txt, edits and all.
        GCHECK_EQ_STR(Mirror(c).textView.string, [@"edited " stringByAppendingString:kB]);
        GCHECK([ParkedActive(c).currentPath isEqualToString:F(@"b.txt")]);
    });

    MCGuiStep(@"Cmd+1 and Cmd+2 go back and forth, each group its own file", ^{
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_ANSI_1, NSEventModifierFlagCommand, nil, nil));
        GCHECK([c focusedGroup] == 0 && [Active(c).currentPath isEqualToString:F(@"b.txt")]);
        GCHECK_EQ_STR(Editor(c).string, [@"edited " stringByAppendingString:kB]);
        GCHECK(Active(c).dirty);
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_ANSI_2, NSEventModifierFlagCommand, nil, nil));
        GCHECK([c focusedGroup] == 1 && [Active(c).currentPath isEqualToString:F(@"a.py")]);
        GCHECK(c.window.firstResponder == Editor(c));
    });

    MCGuiStep(@"a file opened goes in the group with the keyboard", ^{
        [c openFileAtPath:F(@"c.txt") from:EditorTabs::OpenFrom::Other];
        GCHECK_EQ_STR(Layout(c), @"b.txt a.py | *a.py c.txt");
        GCHECK(TabsOf(c).count == 3);
        // A file of the other group's opened here joins this group too.
        [c openFileAtPath:F(@"b.txt") from:EditorTabs::OpenFrom::Other];
        GCHECK_EQ_STR(Layout(c), @"b.txt a.py | *a.py c.txt b.txt");
        GCHECK(TabsOf(c).count == 3);
        GCHECK(Active(c).dirty);   // the same document, unsaved edit and all
        GCHECK([Editor(c).string hasPrefix:@"edited "]);
    });

    MCGuiStep(@"closing a tab the other group also holds asks nothing", ^{
        const NSUInteger alerts = MCGuiAlertsShown().count;
        [c.window makeFirstResponder:Editor(c)];
        [c closeFocused:nil];   // Cmd+W
        GCHECK(MCGuiAlertsShown().count == alerts);
        GCHECK_EQ_STR(Layout(c), @"b.txt a.py | *a.py c.txt");
        MCTab *b = [c tabForPath:F(@"b.txt")];
        GCHECK(b && b.dirty);   // still open, unsaved, in the left group
    });

    MCGuiStep(@"Ctrl+Cmd+Left moves the tab on show to the left group", ^{
        [c selectTabAtIndex:1];   // c.txt
        GCHECK([Active(c).currentPath isEqualToString:F(@"c.txt")]);
        [c.window makeFirstResponder:Editor(c)];
        MCGuiSendKeyToApp(c.window, kVK_LeftArrow,
                          NSEventModifierFlagCommand | NSEventModifierFlagControl |
                              NSEventModifierFlagFunction | NSEventModifierFlagNumericPad,
                          [NSString stringWithFormat:@"%C", (unichar)NSLeftArrowFunctionKey],
                          [NSString stringWithFormat:@"%C", (unichar)NSLeftArrowFunctionKey]);
        GCHECK([c focusedGroup] == 0);
        // After the left group's tab on show, as VS Code moves an editor.
        GCHECK_EQ_STR(Layout(c), @"*b.txt c.txt a.py | a.py");
        GCHECK([Active(c).currentPath isEqualToString:F(@"c.txt")]);
        GCHECK_EQ_STR(Editor(c).string, kC);
    });

    MCGuiStep(@"closing the right group's last tab closes the group", ^{
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_ANSI_2, NSEventModifierFlagCommand, nil, nil));
        GCHECK([c focusedGroup] == 1 && [Active(c).currentPath isEqualToString:F(@"a.py")]);
        const NSUInteger alerts = MCGuiAlertsShown().count;
        [c closeFocused:nil];
        GCHECK(MCGuiAlertsShown().count == alerts);
        GCHECK(![c editorSplit]);
        GCHECK_EQ_STR(Layout(c), @"b.txt c.txt a.py");
        GCHECK(Mirror(c).sharedStorage == nil);   // off the editor's storage
        GCHECK(Mirror(c).scrollView.hidden && ParkedStrip(c).hidden);
        GCHECK([Active(c).currentPath isEqualToString:F(@"c.txt")]);
        GCHECK_EQ_STR(Editor(c).string, kC);
        // The editor has the whole slot again.
        NSScrollView *es = Editor(c).enclosingScrollView;
        GCHECK(NSWidth(es.frame) > NSWidth(c.rightArea.bounds) - 2);
    });

    MCGuiStep(@"Ctrl+Cmd+Right with one group: the tab moves to a new group on the right", ^{
        [c.window makeFirstResponder:Editor(c)];
        MCGuiSendKeyToApp(c.window, kVK_RightArrow,
                          NSEventModifierFlagCommand | NSEventModifierFlagControl |
                              NSEventModifierFlagFunction | NSEventModifierFlagNumericPad,
                          [NSString stringWithFormat:@"%C", (unichar)NSRightArrowFunctionKey],
                          [NSString stringWithFormat:@"%C", (unichar)NSRightArrowFunctionKey]);
        GCHECK([c editorSplit]);
        GCHECK_EQ_STR(Layout(c), @"b.txt a.py | *c.txt");
        GCHECK_EQ_STR(Editor(c).string, kC);
        GCHECK([ParkedActive(c).currentPath isEqualToString:F(@"a.py")] ||
               [ParkedActive(c).currentPath isEqualToString:F(@"b.txt")]);
        GCHECK(Mirror(c).textView.string.length > 0);
    });

    MCGuiStep(@"a Markdown file in both groups: its preview in one, its source in the other", ^{
        MCGuiWriteFile(F(@"m.md"), kM);
        [c openFileAtPath:F(@"m.md") from:EditorTabs::OpenFrom::Other];
        GCHECK_EQ_STR(Layout(c), @"b.txt a.py | *c.txt m.md");
        if (Active(c).previewMode) [c togglePreview:nil];   // a Markdown file opens rendered
        GCHECK(!Active(c).previewMode && Editor(c).editable);
        [c splitEditor:nil];   // already split: shown in the other group too
        GCHECK_EQ_STR(Layout(c), @"*b.txt a.py m.md | c.txt m.md");
        GCHECK(Mirror(c).sharedStorage == Editor(c).textStorage);
        [c togglePreview:nil];
        GCHECK(Active(c).previewMode && !Editor(c).editable);
        // The other group let go of the storage before the page went in,
        // and still shows the source (never the page's text blocks).
        GCHECK(Mirror(c).sharedStorage == nil);
        GCHECK_EQ_STR(Mirror(c).textView.string, kM);
        [c togglePreview:nil];
        GCHECK(!Active(c).previewMode && Editor(c).editable);
    });
    MCGuiWait(@"the source shared again", 5, ^BOOL {
        return Mirror(c).sharedStorage == Editor(c).textStorage;
    });
    MCGuiStep(@"a preview left behind shows as it was, live again once clicked", ^{
        [c togglePreview:nil];
        GCHECK(Active(c).previewMode);
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_ANSI_2, NSEventModifierFlagCommand, nil, nil));
        GCHECK([c focusedGroup] == 1 && [Active(c).currentPath isEqualToString:F(@"m.md")]);
        // One document in both: the preview comes along, and the left group
        // shows what the editor showed there, as a picture.
        GCHECK(Mirror(c).showsPicture);
        GCHECK(Active(c).previewMode && !Editor(c).editable);
        [c selectTabAtIndex:0];   // c.txt here
        GCHECK([Active(c).currentPath isEqualToString:F(@"c.txt")]);
        GCHECK(Mirror(c).showsPicture);   // m.md's page, as a picture
        MCGuiSnapshot(c.window.contentView, @"split-editor-preview-picture");
        // Back in the left group, m.md's page is live, in preview.
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_ANSI_1, NSEventModifierFlagCommand, nil, nil));
        GCHECK([c focusedGroup] == 0 && [Active(c).currentPath isEqualToString:F(@"m.md")]);
        GCHECK(Active(c).previewMode && !Editor(c).editable);
        [c togglePreview:nil];
        // Closed in the left group: the right keeps it, so nothing is asked.
        [c closeTab:Active(c) prompt:YES];
        GCHECK_EQ_STR(Layout(c), @"*b.txt a.py | c.txt m.md");
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_ANSI_2, NSEventModifierFlagCommand, nil, nil));
        [c closeTab:Active(c) prompt:YES];   // c.txt
        [c selectTabAtIndex:0];
        GCHECK_EQ_STR(Layout(c), @"b.txt a.py | *m.md");
        [c closeTab:Active(c) prompt:NO];    // m.md, now only here: the group closes
        GCHECK(![c editorSplit]);
        [c openFileAtPath:F(@"c.txt") from:EditorTabs::OpenFrom::Other];
        [c moveEditorToNextGroup:nil];
        GCHECK_EQ_STR(Layout(c), @"b.txt a.py | *c.txt");
    });

    MCGuiStep(@"a document closed for good (its file trashed, a server done with it) goes from both groups", ^{
        MCGuiWriteFile(F(@"gone.txt"), @"gone\n");
        [c openFileAtPath:F(@"gone.txt") from:EditorTabs::OpenFrom::Other];
        // In the left group too, after its tab on show, with the keyboard.
        const BOOL afterA = [ParkedActive(c).currentPath isEqualToString:F(@"a.py")];
        [c splitEditor:nil];
        GCHECK_EQ_STR(Layout(c), afterA ? @"*b.txt a.py gone.txt | c.txt gone.txt"
                                        : @"*b.txt gone.txt a.py | c.txt gone.txt");
        [c closeTabEverywhere:[c tabForPath:F(@"gone.txt")] prompt:NO];
        GCHECK_EQ_STR(Layout(c), @"*b.txt a.py | c.txt");
        GCHECK([c tabForPath:F(@"gone.txt")] == nil);
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_ANSI_2, NSEventModifierFlagCommand, nil, nil));
        GCHECK_EQ_STR(Layout(c), @"b.txt a.py | *c.txt");
    });

    MCGuiStep(@"the session remembers both groups and which had the keyboard", ^{
        [c saveSession];
        NSDictionary *s = SavedSession(c);
        MCGuiNote(@"session: %@", s);
        NSDictionary *g = s[@"groups"];
        GCHECK([g isKindOfClass:NSDictionary.class]);
        GCHECK(([g[@"groups"] isEqual:@[@[F(@"b.txt"), F(@"a.py")], @[F(@"c.txt")]]]));
        GCHECK([g[@"focused"] intValue] == 1);
        GCHECK([s[@"files"] count] == 3);
        NSString *leftShown = ParkedActive(c).currentPath;
        // b.txt's edit is kept from the backup; the rest come back from disk.
        [c dropAllTabs];
        GCHECK(![c editorSplit] && TabsOf(c).count == 0);
        [c restoreSession];
        GCHECK([c editorSplit]);
        GCHECK_EQ_STR(Layout(c), @"b.txt a.py | *c.txt");
        GCHECK([Active(c).currentPath isEqualToString:F(@"c.txt")]);
        GCHECK_EQ_STR(ParkedActive(c).currentPath, leftShown);
        GCHECK(Mirror(c).textView.string.length > 0);
        GCHECK_EQ_STR(Mirror(c).textView.string,
                      [leftShown isEqualToString:F(@"a.py")] ? kA : Text(@"b.txt"));
    });

    MCGuiStep(@"the terminal keeps Cmd+\\ while it has the keyboard", ^{
        [c newTerminal:nil];
        MCTerminalPanel *panel = [c valueForKey:@"terminalPanel"];
        term = panel.activeTerminal;
        GCHECK(term != nil);
        [term focusInput];
        const NSUInteger before = [panel terminalsInTab:(NSUInteger)panel.selectedTab].count;
        NSString *layout = Layout(c);
        SplitKey(c);
        GCHECK([panel terminalsInTab:(NSUInteger)panel.selectedTab].count == before + 1);
        GCHECK_EQ_STR(Layout(c), layout);   // the editor as it was
    });

    MCGuiStep(@"tidy up: the window closes", ^{
        MCTerminalPanel *panel = [c valueForKey:@"terminalPanel"];
        [panel closeAll];
        for (MCTab *t in [TabsOf(c) copy]) t.dirty = NO;
        for (MCTab *t in [TabsOf(c) copy]) [c closeTab:t prompt:NO];
        GCHECK(![c editorSplit] && TabsOf(c).count == 0);
        [c.window performClose:nil];
        MCGuiSetKeyWindow(nil);
    });
    MCGuiWait(@"gone", 10, ^BOOL { return ![MCGuiControllers() containsObject:c]; });
}
