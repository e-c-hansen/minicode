// MarkdownSideSuite.mm — `make guitest SUITES=markdown_side`: Open Preview
// to the Side (Control+Command+V). From a Markdown file's full preview the
// key brings the source back with the rendered page at its right, the
// source keeping the keyboard; typing re-renders the page a moment after
// it stops, each run still carrying its source line; a task box ticked in
// the page ticks the source in one undo step; the pane steps aside for
// another file and for Shift+Cmd+V and comes back with the tab; the bar
// resizes it and closes it at the right edge; the key closes it from the
// pane, the keyboard going back to the source; the session remembers it;
// and closing the tab closes it.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "EditorPanes.h"
#import "MarkdownSidePreview.h"

namespace {

NSTextView *Source(EditorController *c) { return [c valueForKey:@"textView"]; }
NSScrollView *SourceScroll(EditorController *c) { return [c valueForKey:@"editorScroll"]; }
MCTab *Active(EditorController *c) { return [c valueForKey:@"activeTab"]; }

// The pane, made or not.
MCMarkdownSidePreview *Side(EditorController *c) { return [c valueForKey:@"sidePreview"]; }
NSString *Page(EditorController *c) { return Side(c).textView.string; }

const unsigned short kKeyV = 9;
const NSEventModifierFlags kCtrlCmd = NSEventModifierFlagControl | NSEventModifierFlagCommand;

// The pane and the source side by side, both on show, not overlapping.
BOOL SideBySide(EditorController *c) {
    MCMarkdownSidePreview *side = Side(c);
    NSScrollView *src = SourceScroll(c);
    return side.shown && !side.scrollView.hidden && !src.hidden &&
           NSWidth(side.scrollView.frame) > 100 && NSWidth(src.frame) > 100 &&
           NSMaxX(src.frame) <= NSMinX(side.scrollView.frame) + 0.5 &&
           fabs(NSMinY(src.frame) - NSMinY(side.scrollView.frame)) < 0.5;
}

void Type(NSTextView *tv, NSString *text) {
    MCGuiUndoGroup(tv.undoManager, ^{
        [tv insertText:text replacementRange:NSMakeRange(NSNotFound, 0)];
    });
}

// The first character of the page carrying link `href`, or NSNotFound.
NSUInteger LinkAt(NSTextView *tv, NSString *href) {
    NSTextStorage *st = tv.textStorage;
    __block NSUInteger found = NSNotFound;
    [st enumerateAttribute:NSLinkAttributeName inRange:NSMakeRange(0, st.length) options:0
                usingBlock:^(id v, NSRange r, BOOL *stop) {
        if ([v isEqual:href]) { found = r.location; *stop = YES; }
    }];
    return found;
}

}  // namespace

MC_GUI_SUITE(markdown_side) {
    __block EditorController *c = nil;
    NSString *md = MCGuiProjectFile(@"side.md");
    NSString *other = MCGuiProjectFile(@"side_other.py");
    NSString *text = @"# Side heading\n\nA first paragraph with [a link](#second-heading).\n\n"
                     @"- [ ] a task to tick\n\n| a | b |\n|---|---|\n| one | two |\n\n"
                     @"## Second heading\n\nThe end.\n";

    MCGuiStep(@"the key opens the page beside the source, from the full preview", ^{
        MCGuiWriteFile(md, text);
        MCGuiWriteFile(other, @"print('other')\n");
        c = MCGuiController();
        GCHECK([c showFile:md]);
        GCHECK(c.previewMode);   // a Markdown file opens rendered
        [c focusEditor:nil];
        GCHECK(c.canToggleSidePreview && ![c sidePreviewShowing]);
        MCGuiSendKeyToApp(c.window, kKeyV, kCtrlCmd, @"v", @"v");
        MCGuiNote(@"preview %d, side %d, source %@, pane %@", c.previewMode,
                  [c sidePreviewShowing], NSStringFromRect(SourceScroll(c).frame),
                  NSStringFromRect(Side(c).scrollView.frame));
        GCHECK(!c.previewMode && Source(c).editable);
        GCHECK([c sidePreviewShowing] && SideBySide(c));
        GCHECK([Page(c) containsString:@"Side heading"] && [Page(c) containsString:@"one"]);
        GCHECK(![Page(c) containsString:@"# Side"]);   // rendered, not source
        GCHECK(c.window.firstResponder == Source(c));
        // Roughly half each, and the bar on the boundary.
        GCHECK(fabs(NSWidth(SourceScroll(c).frame) - NSWidth(Side(c).scrollView.frame)) < 2);
        NSView *bar = Side(c).divider;
        GCHECK(!bar.hidden && fabs(NSMidX(bar.frame) - NSMinX(Side(c).scrollView.frame)) < 1);
        // The menu item is checked while it shows.
        NSMenuItem *item = nil;
        for (NSMenuItem *top in NSApp.mainMenu.itemArray)
            for (NSMenuItem *i in top.submenu.itemArray)
                if (i.action == @selector(toggleSidePreview:)) item = i;
        GCHECK(item && [item.keyEquivalent isEqualToString:@"v"] &&
               item.keyEquivalentModifierMask == kCtrlCmd);
        GCHECK([c validateMenuItem:item] && item.state == NSControlStateValueOn);
        MCGuiSnapshot(c.window.contentView, @"markdown_side");
    });

    MCGuiStep(@"typing re-renders the page once typing pauses, the source keeping the keyboard", ^{
        NSTextView *tv = Source(c);
        tv.selectedRange = NSMakeRange(tv.string.length, 0);
        Type(tv, @"\nA paragraph typed in the source.\n");
        GCHECK(![Page(c) containsString:@"typed in the source"]);   // not on every keystroke
        GCHECK(Side(c).renderPending);
    });
    MCGuiWait(@"the page to follow the typing", 3, ^BOOL {
        return [Page(c) containsString:@"typed in the source"];
    });
    MCGuiStep(@"the new run carries its source line", ^{
        GCHECK(c.window.firstResponder == Source(c));
        NSString *page = Page(c);
        const NSUInteger at = [page rangeOfString:@"typed in the source"].location;
        GCHECK(at != NSNotFound);
        NSNumber *line = at == NSNotFound ? nil
            : [Side(c).textView.textStorage attribute:kMarkdownSourceLine atIndex:at effectiveRange:NULL];
        const NSInteger want = (NSInteger)[[Source(c).string componentsSeparatedByString:@"\n"]
            indexOfObject:@"A paragraph typed in the source."];
        MCGuiNote(@"source line %@, wanted %ld", line, (long)want);
        GCHECK(line && line.integerValue == want);
        // A #link scrolls the pane to its heading.
        GCHECK(Side(c).anchors[@"second-heading"] != nil);
    });

    MCGuiStep(@"a task box ticked in the page ticks the source, one undo step", ^{
        NSTextView *page = Side(c).textView;
        NSString *before = Source(c).string;
        const NSUInteger box = LinkAt(page, @"minicode-task:4");
        GCHECK(box != NSNotFound);
        if (box == NSNotFound) return;
        MCGuiUndoGroup(Source(c).undoManager, ^{
            [page.delegate textView:page clickedOnLink:@"minicode-task:4" atIndex:box];
        });
        GCHECK([Source(c).string containsString:@"- [x] a task to tick"]);
        GCHECK(Active(c).dirty);
        [Source(c).undoManager undo];
        GCHECK([Source(c).string isEqualToString:before]);
    });

    MCGuiStep(@"another file: the pane steps aside; back: it returns with the tab", ^{
        GCHECK([c showFile:other]);
        GCHECK(![c sidePreviewShowing] && Side(c).scrollView.hidden && Side(c).divider.hidden);
        GCHECK(Page(c).length == 0);   // the page went with its tab
        GCHECK(![c canToggleSidePreview]);   // not a Markdown file
        GCHECK([c showFile:md]);
        GCHECK([c sidePreviewShowing] && SideBySide(c));
        GCHECK([Page(c) containsString:@"Side heading"]);
    });

    MCGuiStep(@"Shift+Cmd+V: the full preview takes the slot, the pane steps aside", ^{
        [c togglePreview:nil];
        GCHECK(c.previewMode && ![c sidePreviewShowing] && Side(c).scrollView.hidden);
        [c togglePreview:nil];
        GCHECK(!c.previewMode && [c sidePreviewShowing] && SideBySide(c));
    });

    MCGuiStep(@"the bar resizes the pane, and at the right edge closes it", ^{
        NSRect pane = Side(c).scrollView.frame;
        const CGFloat right = NSMaxX(pane);
        [c sidePreviewBarDraggedTo:right - 200];
        MCGuiNote(@"pane after the drag: %@", NSStringFromRect(Side(c).scrollView.frame));
        GCHECK(fabs(NSWidth(Side(c).scrollView.frame) - 200) < 1.5 && SideBySide(c));
        [c sidePreviewBarDraggedTo:right - 4];
        GCHECK(![c sidePreviewShowing] && !Active(c).sidePreview);
        [c toggleSidePreview:nil];
        GCHECK([c sidePreviewShowing]);
        GCHECK(fabs(NSWidth(Side(c).scrollView.frame) - 200) < 1.5);   // its width is kept
    });

    MCGuiStep(@"the key closes it from the pane, the keyboard back in the source", ^{
        [c.window makeFirstResponder:Side(c).textView];
        GCHECK(c.window.firstResponder == Side(c).textView);
        GCHECK(MCGuiPerformMenuKey(c.window, kKeyV, kCtrlCmd, nil, nil));
        GCHECK(![c sidePreviewShowing] && Side(c).scrollView.hidden);
        GCHECK(c.window.firstResponder == Source(c));
        GCHECK(NSWidth(SourceScroll(c).frame) > 400);   // the source has the slot again
        MCGuiSendKeyToApp(c.window, kKeyV, kCtrlCmd, @"v", @"v");
        GCHECK([c sidePreviewShowing]);
    });

    MCGuiStep(@"the session remembers it", ^{
        [c saveSession];
        NSString *suite = NSProcessInfo.processInfo.environment[@"MINICODE_DEFAULTS_SUITE"];
        NSUserDefaults *d = suite.length ? [[NSUserDefaults alloc] initWithSuiteName:suite]
                                         : [NSUserDefaults standardUserDefaults];
        NSDictionary *all = [d dictionaryForKey:@"MCTabSessions"];
        NSDictionary *entry = all[[c sessionKey]];
        MCGuiNote(@"session: %@", entry[@"sidePreview"]);
        GCHECK([entry[@"sidePreview"] containsObject:md]);
        GCHECK(![entry[@"sidePreview"] containsObject:other]);
    });

    MCGuiStep(@"closing the tab closes it", ^{
        [Source(c).undoManager removeAllActions];
        GCHECK([c closeTab:Active(c) prompt:NO]);
        GCHECK(![c sidePreviewShowing] && Side(c).scrollView.hidden);
        GCHECK(![Page(c) containsString:@"Side heading"]);
        for (MCTab *t in [(NSArray *)[c valueForKey:@"tabs"] copy])
            if ([t.currentPath isEqualToString:other] || [t.currentPath isEqualToString:md])
                [c closeTab:t prompt:NO];
    });
}
