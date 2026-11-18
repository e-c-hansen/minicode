// MarkdownSideSuite.mm — `make guitest SUITES=markdown_side`: Open Preview
// to the Side (Control+Command+V). From a Markdown file's full preview the
// key brings the source back with the rendered page at its right, the
// source keeping the keyboard; typing re-renders the page a moment after
// it stops, each run still carrying its source line; a task box ticked in
// the page ticks the source in one undo step; the pane steps aside for
// another file and for Shift+Cmd+V and comes back with the tab; the bar
// resizes it and closes it at the right edge; the key closes it from the
// pane, the keyboard going back to the source; the session remembers it;
// and closing the tab closes it. Then the owner's report (2026-11-16): a
// settings file and a Markdown page in one window, switched between many
// times and at different turns, always show the active tab's text, with
// the page's pane drawn again in full where it ended up, margins too (the
// settings file's text had stayed in them beside the page).
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "EditorPanes.h"
#import "MarkdownSidePreview.h"
#import "MarkdownEditPanel.h"
#import "Gutter.h"
#import "ControlServer.h"
#include <mach/mach.h>
#include <objc/runtime.h>

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

// `range` replaced by `text` as typing would (the delegate told, the
// buffer following).
void Replace(NSTextView *tv, NSRange range, NSString *text) {
    MCGuiUndoGroup(tv.undoManager, ^{
        [tv insertText:text replacementRange:range];
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

// What asks the editor's text view to draw again, recorded while
// gRecordMarks is set: every rect given to setNeedsDisplayInRect: (in both
// forms) of gMarkedView, in its own coordinates.
BOOL gRecordMarks = NO;
__unsafe_unretained NSView *gMarkedView = nil;
NSMutableArray<NSValue *> *gMarks = nil;

void RecordMarksFor(SEL sel, BOOL withAvoid) {
    Class cls = NSClassFromString(@"CodeTextView");
    Method m = class_getInstanceMethod(cls, sel);
    if (!m) return;
    IMP original = method_getImplementation(m);
    IMP imp = withAvoid
        ? imp_implementationWithBlock(^(id me, NSRect r, BOOL avoid) {
              if (gRecordMarks && me == gMarkedView) [gMarks addObject:[NSValue valueWithRect:r]];
              ((void (*)(id, SEL, NSRect, BOOL))original)(me, sel, r, avoid);
          })
        : imp_implementationWithBlock(^(id me, NSRect r) {
              if (gRecordMarks && me == gMarkedView) [gMarks addObject:[NSValue valueWithRect:r]];
              ((void (*)(id, SEL, NSRect))original)(me, sel, r);
          });
    // Added to CodeTextView when only a superclass has it (calling that one
    // is calling super), else in place of its own.
    if (!class_addMethod(cls, sel, imp, method_getTypeEncoding(m))) method_setImplementation(m, imp);
}

void StartMarks(NSView *view) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        RecordMarksFor(@selector(setNeedsDisplayInRect:avoidAdditionalLayout:), YES);
        RecordMarksFor(@selector(setNeedsDisplayInRect:), NO);
    });
    gMarks = [NSMutableArray array];
    gMarkedView = view;
    gRecordMarks = YES;
}

// Some one mark covers all of `rect`.
BOOL MarkCovers(NSRect rect) {
    for (NSValue *v in gMarks)
        if (NSContainsRect(v.rectValue, NSInsetRect(rect, 1, 1))) return YES;
    return NO;
}

// The page's margins in the pane as it shows now: left and right of its
// column, where no glyph of it is.
NSRect LeftMargin(NSTextView *tv) {
    const NSRect vis = tv.visibleRect;
    return NSMakeRect(NSMinX(vis), NSMinY(vis), tv.textContainerOrigin.x - NSMinX(vis), NSHeight(vis));
}
NSRect RightMargin(NSTextView *tv) {
    const NSRect vis = tv.visibleRect;
    const CGFloat right = tv.textContainerOrigin.x + tv.textContainer.size.width;
    return NSMakeRect(right, NSMinY(vis), NSMaxX(vis) - right, NSHeight(vis));
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
        const NSUInteger box = LinkAt(page, @"mutible-task:4");
        GCHECK(box != NSNotFound);
        if (box == NSNotFound) return;
        MCGuiUndoGroup(Source(c).undoManager, ^{
            [page.delegate textView:page clickedOnLink:@"mutible-task:4" atIndex:box];
        });
        GCHECK([Source(c).string containsString:@"- [x] a task to tick"]);
        GCHECK(Active(c).dirty);
        [Source(c).undoManager undo];
        GCHECK([Source(c).string isEqualToString:before]);
    });

    MCGuiStep(@"a click on a page behind the typing acts on nothing: the page catches up first", ^{
        NSTextView *page = Side(c).textView;
        [c renderSidePreviewKeepingPlace];   // after the undo
        GCHECK([c sidePageIsCurrent]);
        NSString *start = [Source(c).string copy];
        // Lines typed above the task: the page still shows the old lines.
        NSTextView *tv = Source(c);
        tv.selectedRange = NSMakeRange(0, 0);
        Type(tv, @"Two new lines\n\n");
        GCHECK(Side(c).renderPending && ![c sidePageIsCurrent]);
        NSString *before = [tv.string copy];
        const NSUInteger box = LinkAt(page, @"mutible-task:4");   // the box, as the page has it
        GCHECK(box != NSNotFound);
        if (box == NSNotFound) return;
        MCGuiUndoGroup(tv.undoManager, ^{
            GCHECK([page.delegate textView:page clickedOnLink:@"mutible-task:4" atIndex:box]);
        });
        // Nothing ticked (line 4 is now the heading's blank line, the task on 6) ...
        GCHECK([tv.string isEqualToString:before]);
        // ... the page is up to date, and the status bar says why nothing happened.
        GCHECK([c sidePageIsCurrent] && !Side(c).renderPending);
        GCHECK([Page(c) containsString:@"Two new lines"]);
        NSTextField *note = [c valueForKey:@"settingsLabel"];
        GCHECK([note.stringValue containsString:@"behind your typing"]);
        // The same for a double-click's block editor.
        tv.selectedRange = NSMakeRange(0, 0);
        Type(tv, @"x");
        GCHECK(![c sidePageIsCurrent]);
        const NSUInteger at = [Page(c) rangeOfString:@"A first paragraph"].location;
        GCHECK(at != NSNotFound);
        const NSRect r = MCTextViewRectForCharacter(page, at);
        GCHECK([c editSidePreviewAtCharacter:at point:NSMakePoint(NSMidX(r), NSMidY(r))]);
        GCHECK([c sidePageIsCurrent] && [Page(c) containsString:@"xTwo new lines"]);
        // Now current: the box the page shows ticks its own line, the
        // number in its link moved with it.
        const NSUInteger moved = [Page(c) rangeOfString:@"a task to tick"].location;
        GCHECK(moved != NSNotFound && moved > 2);
        NSUInteger tick = NSNotFound;
        for (NSUInteger k = moved; k > 0 && k + 3 > moved; k--)
            if ([page.textStorage attribute:NSAttachmentAttributeName atIndex:k - 1 effectiveRange:NULL])
                tick = k - 1;
        GCHECK(tick != NSNotFound);
        if (tick != NSNotFound) {
            id link = [page.textStorage attribute:NSLinkAttributeName atIndex:tick effectiveRange:NULL];
            MCGuiUndoGroup(tv.undoManager, ^{
                GCHECK([page.delegate textView:page clickedOnLink:link atIndex:tick]);
            });
            GCHECK([tv.string containsString:@"- [x] a task to tick"]);
            [tv.undoManager undo];
        }
        // Back as it was.
        Replace(tv, NSMakeRange(0, [@"xTwo new lines\n\n" length]), @"");
        GCHECK([tv.string isEqualToString:start]);
        [c renderSidePreviewKeepingPlace];
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
        NSDictionary *entry = [c storedSession];   // as sessions.json holds it
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

    // ------------------------------------------------ the owner's report
    // A settings file and JOBS.md, the page beside its source or in place
    // of it, switched between: the source pane held the settings file's
    // text, swatches and all, beside JOBS.md's page. A window of its own, as
    // wide as a laptop's, so the full page has margins.
    __block EditorController *w = nil;
    NSString *dir = MCGuiProjectFile(@"side_switch");
    NSString *jobs = [dir stringByAppendingPathComponent:@"JOBS.md"];
    NSString *settingsPath = NSProcessInfo.processInfo.environment[@"MUTIBLE_SETTINGS"];
    __block NSString *settingsText = nil, *original = nil;
    NSMutableString *jobsText = [NSMutableString stringWithString:
        @"# Jobs mutible has to do better\n\nTwo layers.\n\n"];
    for (int i = 1; i <= 30; i++)
        [jobsText appendFormat:@"- [%@] Job %d: a line long enough to wrap in a narrow pane, "
                                @"said plainly.\n", i % 3 ? @" " : @"x", i];
    [jobsText appendString:@"\n## The end\n\nDone.\n"];
    // The source pane shows `text` and nothing else; the page beside it
    // shows `side` (nil: no page beside it).
    BOOL (^shows)(NSString *, NSString *) = ^BOOL(NSString *text, NSString *side) {
        const BOOL source = Source(w).editable && [Source(w).string isEqualToString:text];
        const BOOL page = side ? [w sidePreviewShowing] && SideBySide(w) && [Page(w) containsString:side]
                               : ![w sidePreviewShowing] && Side(w).scrollView.hidden;
        if (!source || !page)
            MCGuiNote(@"active %@, source editable %d starts \"%@\", side %d, page starts \"%@\"",
                      Active(w).currentPath.lastPathComponent, Source(w).editable,
                      [Source(w).string substringToIndex:MIN((NSUInteger)40, Source(w).string.length)],
                      [w sidePreviewShowing],
                      [Page(w) substringToIndex:MIN((NSUInteger)40, Page(w).length)]);
        return source && page;
    };

    MCGuiStep(@"a settings file and JOBS.md in a wide window of their own", ^{
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                                   attributes:nil error:nil];
        MCGuiWriteFile(jobs, jobsText);
        original = [NSString stringWithContentsOfFile:settingsPath encoding:NSUTF8StringEncoding
                                                error:nil] ?: @"";
        settingsText = original;
        // A color, so the file has a swatch, as the owner's had, and
        // comments enough to scroll.
        NSMutableString *more = [NSMutableString stringWithString:settingsText];
        for (int i = 1; i <= 80; i++)
            [more appendFormat:@"# A comment line %d, as the default file has many.\n", i];
        settingsText = [more copy];
        MCGuiSetSettings([settingsText stringByAppendingString:@"editor.ruler-color = #5A5A5A\n"]);
        w = [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:dir];
        GCHECK(w != nil);
        if (!w) return;
        MCGuiSetKeyWindow(w.window);
        NSRect fr = w.window.frame;
        fr.size = NSMakeSize(1800, 900);
        [w.window setFrame:fr display:NO];
        [w.window.contentView layoutSubtreeIfNeeded];
        GCHECK([w showFile:settingsPath]);
        GCHECK([Source(w).string containsString:@"editor.ruler-color = #5A5A5A"]);
        GCHECK([w showFile:jobs]);
        GCHECK(w.previewMode);   // a Markdown file opens rendered
        MCGuiNote(@"view %.0f wide, page inset %.0f", NSWidth(Source(w).frame),
                  Source(w).textContainerInset.width);
        GCHECK(Source(w).textContainerInset.width > 100);   // margins either side of the page
        GCHECK([w showFile:settingsPath]);
        GCHECK(Source(w).editable && Source(w).textContainerInset.width < 20);
    });

    // The full page in place of the source: after the switch, as the turn
    // ends, the pane is asked to draw again where it ended up, its margins
    // too. Before, only the page's own column was, and the settings file's
    // text stayed in the margins. Three ways round: one switch in a step,
    // the settings file scrolled down first, and there and back and there
    // again in one turn.
    for (int round = 0; round < 3; round++) {
        MCGuiStep([NSString stringWithFormat:@"round %d: the settings file, then the page", round], ^{
            NSTextView *tv = Source(w);
            if (round == 1)   // the settings file scrolled to its end
                [tv scrollRangeToVisible:NSMakeRange(tv.string.length, 0)];
            if (round == 2) {   // there and back in the same turn first
                GCHECK([w showFile:jobs]);
                GCHECK([w showFile:settingsPath]);
                GCHECK(Source(w).editable && [Source(w).string containsString:@"ruler-color"]);
            }
            GCHECK([w showFile:jobs]);
            // What asks the pane to draw from here to the end of the turn.
            StartMarks(tv);
        });
        MCGuiStep([NSString stringWithFormat:@"round %d: all of the pane is drawn again, margins too",
                                             round], ^{
            gRecordMarks = NO;
            NSTextView *tv = Source(w);
            GCHECK(w.previewMode && !tv.editable);
            GCHECK([tv.string hasPrefix:@"Jobs mutible has to do better"]);
            GCHECK(![tv.string containsString:@"ruler-color"]);
            const NSRect left = LeftMargin(tv), right = RightMargin(tv);
            MCGuiNote(@"visible %@, margins %@ and %@, %lu marks after the switch: %@",
                      NSStringFromRect(tv.visibleRect), NSStringFromRect(left),
                      NSStringFromRect(right), (unsigned long)gMarks.count,
                      gMarks.count ? NSStringFromRect(gMarks.lastObject.rectValue) : @"-");
            GCHECK(NSWidth(left) > 50 && NSWidth(right) > 50);
            GCHECK(MarkCovers(left));
            GCHECK(MarkCovers(right));
            GCHECK([w showFile:settingsPath]);
            GCHECK(Source(w).editable && [Source(w).string containsString:@"ruler-color"]);
        });
    }

    MCGuiStep(@"the page beside its source (Control+Command+V)", ^{
        GCHECK([w showFile:jobs]);
        GCHECK(w.previewMode);
        [w toggleSidePreview:nil];
        GCHECK(!w.previewMode);
        GCHECK(shows(jobsText, @"Two layers."));
    });
    MCGuiStep(@"switched ten times in one turn, the source is always the active tab's", ^{
        for (int i = 0; i < 10; i++) {
            GCHECK([w showFile:settingsPath]);
            GCHECK(!w.previewMode);
            GCHECK([Source(w).string containsString:@"editor.ruler-color = #5A5A5A"]);
            GCHECK(shows(Source(w).string, nil));
            GCHECK([w showFile:jobs]);
            GCHECK(shows(jobsText, @"Two layers."));
        }
    });
    // A turn of its own for each switch.
    for (int i = 0; i < 4; i++) {
        MCGuiStep([NSString stringWithFormat:@"switch %d, one a turn", i], ^{
            if (i % 2 == 0) {
                GCHECK([w showFile:settingsPath]);
                GCHECK([Source(w).string containsString:@"editor.ruler-color = #5A5A5A"]);
                GCHECK(shows(Source(w).string, nil));
            } else {
                GCHECK([w showFile:jobs]);
                GCHECK(shows(jobsText, @"Two layers."));
            }
        });
    }
    MCGuiStep(@"typed in the source, then the settings file before the page follows", ^{
        GCHECK(shows(jobsText, @"Two layers."));
        NSTextView *tv = Source(w);
        tv.selectedRange = NSMakeRange(tv.string.length, 0);
        Type(tv, @"\nTyped before the switch.\n");
        GCHECK(Side(w).renderPending);
        GCHECK([w showFile:settingsPath]);
        GCHECK([Source(w).string containsString:@"ruler-color"] && shows(Source(w).string, nil));
    });
    // A negative check: the page's render was due 0.3 s after the typing,
    // and must not land in the settings file's tab.
    MCGuiPause(0.8);
    MCGuiStep(@"the render that was due left the settings file alone", ^{
        GCHECK(![Source(w).string containsString:@"Typed before the switch"]);
        GCHECK([Source(w).string containsString:@"ruler-color"] && shows(Source(w).string, nil));
        GCHECK(Page(w).length == 0);
        GCHECK([w showFile:jobs]);
        NSString *typed = [jobsText stringByAppendingString:@"\nTyped before the switch.\n"];
        GCHECK(shows(typed, @"Typed before the switch."));
        [Source(w).undoManager undo];
        GCHECK([Source(w).string isEqualToString:jobsText] && !Active(w).dirty);
    });
    MCGuiStep(@"the settings file changed on disk while the page shows", ^{
        MCGuiSetSettings([settingsText stringByAppendingString:@"editor.ruler-color = #6B6B6B\n"]);
        GCHECK(shows(jobsText, @"Two layers."));
    });
    MCGuiWait(@"its tab to follow, shown", 5, ^BOOL {
        if (Active(w) && [Active(w).currentPath isEqualToString:jobs]) [w showFile:settingsPath];
        if ([Source(w).string containsString:@"#6B6B6B"]) return YES;
        [w showFile:jobs];   // shown again, it is checked against the disk again
        return NO;
    });
    MCGuiStep(@"and back: JOBS.md's source beside its page", ^{
        GCHECK(shows(Source(w).string, nil));
        GCHECK([w showFile:jobs]);
        GCHECK(shows(jobsText, @"Two layers."));
    });
    MCGuiStep(@"the window goes", ^{
        MCGuiSetSettings(original);
        if ([w sidePreviewShowing]) [w toggleSidePreview:nil];
        for (MCTab *t in [(NSArray *)[w valueForKey:@"tabs"] copy]) [w closeTab:t prompt:NO];
        MCGuiSetKeyWindow(nil);
        [w.window performClose:nil];
    });
}

// ---------------------------------------------------------------------
// `make guitest SUITES=markdown_side_sync`: the page and its source scroll
// together, either one leading (VS Code's scrollPreviewWithEditor and
// scrollEditorWithPreview), each landing the other within a line and
// neither moving back the side the user scrolled; the block of the
// source's caret line is marked in the page (markEditorSelection); a
// double-click in the page opens the block editor, whose edit goes into
// the source as one splice and one undo step; and following costs little
// main-thread CPU time on a long page.

namespace {

// The source line `line` starts on, put at the source's top.
void ScrollSourceToLine(EditorController *c, NSUInteger line) {
    MCLineGutter *g = [c valueForKey:@"gutter"];
    const NSRect r = MCTextViewRectForCharacter(Source(c), [g startOfLine:line]);
    MCScrollTextViewToY(SourceScroll(c), Source(c), NSMinY(r));
}

// The source line at the source's top, from the editor's own index.
NSUInteger SourceTop(EditorController *c) {
    NSTextView *tv = Source(c);
    const NSRect v = tv.visibleRect;
    const NSUInteger i =
        [tv characterIndexForInsertionAtPoint:NSMakePoint(NSMidX(v), NSMinY(v) + 2)];
    return [(MCLineGutter *)[c valueForKey:@"gutter"] lineOfCharacter:i];
}

// The source line of the page's text at the page's top.
NSInteger PageTop(EditorController *c) {
    NSTextView *tv = Side(c).textView;
    const NSRect v = tv.visibleRect;
    NSUInteger i =
        [tv characterIndexForInsertionAtPoint:NSMakePoint(NSMinX(v) + 30, NSMinY(v) + 2)];
    NSTextStorage *st = tv.textStorage;
    if (i >= st.length) i = st.length - 1;
    NSNumber *line = [st attribute:kMarkdownSourceLine atIndex:i effectiveRange:NULL];
    return line ? line.integerValue : -1;
}

NSUInteger PageAt(EditorController *c, NSString *text) {
    return [Side(c).textView.string rangeOfString:text].location;
}

// The main thread's CPU time in ms (wall time varies with the machine's load).
double CpuMs() {
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    const mach_port_t self = mach_thread_self();
    const kern_return_t kr = thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&info, &count);
    mach_port_deallocate(mach_task_self(), self);
    if (kr != KERN_SUCCESS) return 0;
    return (info.user_time.seconds + info.system_time.seconds) * 1e3 +
           (info.user_time.microseconds + info.system_time.microseconds) / 1e3;
}

// The page's text as a whole render (the full preview's builder) makes it
// for the source as it is now.
NSString *WholePage(EditorController *c) {
    return [c markdownPageFor:c.sourceText width:Side(c).pageWidth anchors:NULL].string;
}

// The last render built only what changed: under `bound` ms of CPU on a
// Mac as fast as the one these were set on (an M3, where a whole render of
// the same text took `wholeHere` ms), and in proportion on a slower one
// (CI's runners, an efficiency core under load), which a whole render of
// the same text, measured now on the same thread (the best of three), says
// how much slower it is. Like against like, so the bound is as tight
// everywhere: a render that built every piece again costs a whole one.
void CheckRenderCost(EditorController *c, double bound, double wholeHere, NSString *what,
                     const char *file, int line) {
    const double ms = Side(c).lastRenderCpuMilliseconds;
    double whole = INFINITY;
    for (int k = 0; k < 3; k++) {
        const double t0 = MCGuiThreadCpuMs();
        (void)WholePage(c);
        whole = MIN(whole, MCGuiThreadCpuMs() - t0);
    }
    const double slower = MAX(1.0, whole / wholeHere);
    MCGuiNote(@"%@: %.2f ms of CPU; a whole render %.2f, %.2f times the M3's", what, ms, whole, slower);
    MCGuiCheckTime(ms, bound * slower, what, file, line);
}
#define CHECK_RENDER_COST(c, bound, wholeHere, what) \
    CheckRenderCost((c), (bound), (wholeHere), (what), __FILE__, __LINE__)

// How the two strings differ: the one stretch of `a` that `b` replaces.
NSRange Changed(NSString *a, NSString *b, NSString **with) {
    NSUInteger p = 0, s = 0;
    while (p < a.length && p < b.length && [a characterAtIndex:p] == [b characterAtIndex:p]) p++;
    while (s < a.length - p && s < b.length - p &&
           [a characterAtIndex:a.length - 1 - s] == [b characterAtIndex:b.length - 1 - s])
        s++;
    if (with) *with = [b substringWithRange:NSMakeRange(p, b.length - p - s)];
    return NSMakeRange(p, a.length - p - s);
}

}  // namespace

MC_GUI_SUITE(markdown_side_sync) {
    __block EditorController *c = nil;
    // A window of its own on a folder of its own, so the panes are as a new
    // window has them whatever earlier suites left in theirs: with CI's GUI
    // checks in two halves, a browser pane an earlier suite left showing in
    // the project's window narrowed the source until its lines wrapped, and
    // the source settled 644 lines from where the checks expected it.
    NSString *folder = MCGuiProjectFile(@"side-sync");
    NSString *md = [folder stringByAppendingPathComponent:@"sync.md"];
    // Ten lines a section: heading (10k), blank, a paragraph of two lines
    // (10k+2, 10k+3), blank, a fence, two code lines (10k+6, 10k+7), the
    // closing fence, blank.
    NSMutableString *text = [NSMutableString string];
    for (int k = 0; k < 120; k++)
        [text appendFormat:@"## Section %d\n\nParagraph %d line one\ncontinues on line two of %d.\n\n"
                           @"```\ncode %d a\ncode %d b\n```\n\n", k, k, k, k, k];

    MCGuiStep(@"a window of its own", ^{
        [[NSFileManager defaultManager] createDirectoryAtPath:folder withIntermediateDirectories:YES
                                                   attributes:nil error:nil];
        id app = NSApp.delegate;
        c = [app performSelector:@selector(openWindowAtPath:) withObject:folder.stringByStandardizingPath];
        MCGuiSetKeyWindow(nil);
        GCHECK(c != nil && MCGuiController() == c);
        GCHECK(![[c valueForKey:@"browserVisible"] boolValue]);
    });

    MCGuiStep(@"a long file with the page beside it", ^{
        MCGuiWriteFile(md, text);
        GCHECK([c showFile:md]);
        if (c.previewMode) [c togglePreview:nil];
        [c focusEditor:nil];
        if (![c sidePreviewShowing]) [c toggleSidePreview:nil];
        GCHECK([c sidePreviewShowing]);
        GCHECK([Page(c) containsString:@"Section 119"]);
        [Side(c).textView.layoutManager ensureLayoutForTextContainer:Side(c).textView.textContainer];
        GCHECK(NSHeight(Side(c).textView.frame) > 4 * NSHeight(Side(c).scrollView.frame));
    });

    MCGuiStep(@"scrolling the source moves the page to the same line", ^{
        for (NSUInteger line : {500u, 503u, 806u, 1000u}) {
            ScrollSourceToLine(c, line);
            const NSInteger page = PageTop(c);
            MCGuiNote(@"source at %lu (top line %.2f): page at %ld (%.2f)",
                      (unsigned long)SourceTop(c), Side(c).sourceTopLine, (long)page,
                      Side(c).pageTopLine);
            GCHECK(SourceTop(c) == line);
            GCHECK(labs(page - (NSInteger)line) <= 1);
            GCHECK(fabs(Side(c).pageTopLine - Side(c).sourceTopLine) < 1);
            GCHECK(!Side(c).pageLeads);
        }
        // Back to the top: the page's top too.
        MCScrollTextViewToY(SourceScroll(c), Source(c), 0);
        GCHECK(Side(c).scrollY < 1);
    });

    MCGuiStep(@"scrolling the page moves the source, and the page stays where it was put", ^{
        NSTextView *page = Side(c).textView;
        for (NSString *at in @[@"code 80 b", @"Section 30", @"continues on line two of 95."]) {
            const NSUInteger i = PageAt(c, at);
            GCHECK(i != NSNotFound);
            if (i == NSNotFound) continue;
            const CGFloat y = NSMinY(MCTextViewRectForCharacter(page, i));
            MCScrollTextViewToY(Side(c).scrollView, page, y);
            const NSInteger want = [[page.textStorage attribute:kMarkdownSourceLine atIndex:i
                                                 effectiveRange:NULL] integerValue];
            MCGuiNote(@"page at %@ (line %ld, %.2f): source at %lu (%.2f)", at, (long)want,
                      Side(c).pageTopLine, (unsigned long)SourceTop(c), Side(c).sourceTopLine);
            GCHECK(labs((NSInteger)SourceTop(c) - want) <= 1);
            GCHECK(fabs(Side(c).sourceTopLine - Side(c).pageTopLine) < 1);
            GCHECK(fabs(Side(c).scrollY - y) < 0.5);   // the source following did not move it back
            GCHECK(Side(c).pageLeads);
        }
        // The source scrolled again leads again.
        ScrollSourceToLine(c, 300);
        GCHECK(!Side(c).pageLeads && labs(PageTop(c) - 300) <= 1);
    });

    MCGuiStep(@"typing re-renders the page at the source's line", ^{
        NSTextView *tv = Source(c);
        MCLineGutter *g = [c valueForKey:@"gutter"];
        ScrollSourceToLine(c, 600);
        tv.selectedRange = NSMakeRange([g startOfLine:612], 0);
        Type(tv, @"Typed ");
        ScrollSourceToLine(c, 600);
    });
    MCGuiWait(@"the page to follow the typing", 3, ^BOOL {
        return [Page(c) containsString:@"Typed Paragraph 61"];
    });
    MCGuiStep(@"the page is still at the source's line", ^{
        MCGuiNote(@"source %lu, page %ld", (unsigned long)SourceTop(c), (long)PageTop(c));
        GCHECK(SourceTop(c) == 600 && labs(PageTop(c) - 600) <= 1);
        // Only the edited piece was built: the page is what a whole render
        // makes, and it cost little of the main thread.
        GCHECK_EQ_STR(Page(c), WholePage(c));
        CHECK_RENDER_COST(c, 10, 14, @"a pause's render at 1,200 lines");
    });

    __block NSTextBlock *codeBox = nil;
    __block NSUInteger codeAt = NSNotFound;
    MCGuiStep(@"a line added at the top: the pieces below are kept, their lines moved", ^{
        codeAt = PageAt(c, @"code 110 a");
        GCHECK(codeAt != NSNotFound);
        NSParagraphStyle *ps = [Side(c).textView.textStorage attribute:NSParagraphStyleAttributeName
                                                               atIndex:codeAt effectiveRange:NULL];
        codeBox = ps.textBlocks.lastObject;
        GCHECK(codeBox.backgroundColor != nil);
        NSTextView *tv = Source(c);
        ScrollSourceToLine(c, 0);   // typing where it is on show, as the caret is
        tv.selectedRange = NSMakeRange(0, 0);
        Type(tv, @"A line on top.\n\n");
    });
    MCGuiWait(@"the page to show the new line", 3, ^BOOL {
        return [Page(c) hasPrefix:@"A line on top."];
    });
    MCGuiStep(@"the lines below follow the source, the code box is the same object", ^{
        GCHECK_EQ_STR(Page(c), WholePage(c));
        const NSUInteger at = PageAt(c, @"code 110 a");
        NSParagraphStyle *ps = [Side(c).textView.textStorage attribute:NSParagraphStyleAttributeName
                                                               atIndex:at effectiveRange:NULL];
        GCHECK(ps.textBlocks.lastObject == codeBox);   // kept, not built again
        const NSInteger line = MCMarkdownLineOf([Side(c).textView.textStorage
            attribute:kMarkdownSourceLine atIndex:at effectiveRange:NULL]);
        const NSInteger want = (NSInteger)[[Source(c).string componentsSeparatedByString:@"\n"]
            indexOfObject:@"code 110 a"];
        MCGuiNote(@"code 110 a: page line %ld, source line %ld", (long)line, (long)want);
        GCHECK(line == want && want == 1108);
        // The anchors moved with the text.
        const NSUInteger heading = [Side(c).anchors[@"section-110"] unsignedIntegerValue];
        GCHECK([[Page(c) substringFromIndex:heading] hasPrefix:@"Section 110"]);
        CHECK_RENDER_COST(c, 10, 14, @"a render after a line on top");
        // Taken out again.
        NSTextView *tv = Source(c);
        Replace(tv, NSMakeRange(0, [@"A line on top.\n\n" length]), @"");
    });
    MCGuiWait(@"the page to lose it", 3, ^BOOL {
        return [Page(c) hasPrefix:@"Section 0"];
    });
    MCGuiStep(@"and the lines go back", ^{
        GCHECK_EQ_STR(Page(c), WholePage(c));
        const NSUInteger at = PageAt(c, @"code 110 a");
        GCHECK(MCMarkdownLineOf([Side(c).textView.textStorage attribute:kMarkdownSourceLine
                                                                atIndex:at effectiveRange:NULL]) == 1106);
    });

    MCGuiStep(@"the caret's block is marked in the page", ^{
        NSTextView *tv = Source(c);
        MCLineGutter *g = [c valueForKey:@"gutter"];
        NSString *page = Page(c);
        NSString * (^marked)(void) = ^NSString * {
            const NSRange r = Side(c).markedRange;
            return r.location == NSNotFound ? @"" : [page substringWithRange:r];
        };
        // A paragraph's second line: the whole paragraph.
        tv.selectedRange = NSMakeRange([g startOfLine:503] + 3, 0);
        MCGuiNote(@"marked: \"%@\" at %@", marked(), NSStringFromRect(Side(c).markRect));
        GCHECK([marked() containsString:@"Paragraph 50 line one"] &&
               [marked() containsString:@"line two of 50."] &&
               ![marked() containsString:@"Section"]);
        const NSRect bar = Side(c).markRect;
        const NSRect first =
            MCTextViewRectForCharacter(Side(c).textView, PageAt(c, @"Paragraph 50 line one"));
        const NSRect last =
            MCTextViewRectForCharacter(Side(c).textView, PageAt(c, @"line two of 50."));
        GCHECK(!NSIsEmptyRect(bar) && NSWidth(bar) == 3 && NSMaxX(bar) < NSMinX(first));
        GCHECK(NSMinY(bar) <= NSMinY(first) + 1 && NSMaxY(bar) >= NSMaxY(last) - 1);
        // A code block's line: the whole block.
        tv.selectedRange = NSMakeRange([g startOfLine:806], 0);
        GCHECK([marked() containsString:@"code 80 a"] && [marked() containsString:@"code 80 b"]);
        // A blank line: the block above it.
        tv.selectedRange = NSMakeRange([g startOfLine:801], 0);
        MCGuiNote(@"on a blank line: \"%@\"", marked());
        GCHECK([marked() containsString:@"Section 80"]);
        // The heading itself.
        tv.selectedRange = NSMakeRange([g startOfLine:900] + 4, 0);
        GCHECK([marked() hasPrefix:@"Section 90"]);
        tv.selectedRange = NSMakeRange([g startOfLine:903] + 4, 0);
        ScrollSourceToLine(c, 896);
        MCGuiSnapshot(c.window.contentView, @"markdown_side_mark");
    });

    MCGuiStep(@"a double-click in the page edits its block in the source: one splice, one undo step", ^{
        NSTextView *page = Side(c).textView;
        NSTextView *tv = Source(c);
        const NSUInteger i = PageAt(c, @"Paragraph 70 line one");
        GCHECK(i != NSNotFound);
        if (i == NSNotFound) return;
        const NSRect r = MCTextViewRectForCharacter(page, i + 3);
        const NSPoint at = [page convertPoint:NSMakePoint(NSMidX(r), NSMidY(r)) toView:nil];
        NSEvent *down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:at
                                      modifierFlags:0 timestamp:0
                                       windowNumber:c.window.windowNumber context:nil
                                        eventNumber:0 clickCount:2 pressure:1];
        [page mouseDown:down];
        MCMarkdownEditPanel *panel = [c valueForKey:@"markdownEditPanel"];
        NSTextView *editor = [panel valueForKey:@"_editor"];
        GCHECK_EQ_STR(editor.string, @"Paragraph 70 line one\ncontinues on line two of 70.");
        // It opens on the word that was double-clicked, and no more of it.
        GCHECK(NSEqualRanges(editor.selectedRange, NSMakeRange(0, 9)));
        MCGuiNote(@"selected: %@", NSStringFromRange(editor.selectedRange));
        // As wide as the page's lines (460 points at least).
        NSPopover *pop = [panel valueForKey:@"_popover"];
        const CGFloat lineWidth = page.textContainer.size.width - 2 * page.textContainer.lineFragmentPadding;
        MCGuiNote(@"box %.0f wide for lines %.0f wide", NSWidth(pop.contentViewController.view.frame), lineWidth);
        GCHECK(NSWidth(pop.contentViewController.view.frame) >= MAX(460, lineWidth) - 1);
        NSString *before = [tv.string copy];   // the view's string is its live storage
        // The caret, on line 904 since the step before, after the edit.
        const NSRange caret = tv.selectedRange;
        GCHECK(caret.location > NSMaxRange([before rangeOfString:@"two of 70."]));
        editor.string = @"Paragraph seventy, rewritten from the page.";
        MCGuiUndoGroup(tv.undoManager, ^{
            [panel performSelector:@selector(save:) withObject:nil];
        });
        NSString *with = nil;
        const NSRange changed = Changed(before, tv.string, &with);
        GCHECK_EQ_STR([before substringWithRange:changed], @"70 line one\ncontinues on line two of 70");
        GCHECK_EQ_STR(with, @"seventy, rewritten from the page");
        GCHECK([c.sourceText isEqualToString:tv.string]);
        GCHECK(Active(c).dirty);
        // The caret keeps its place in the text, moved by what the edit took out.
        GCHECK(tv.selectedRange.location == caret.location - (changed.length - with.length));
        GCHECK([Page(c) containsString:@"seventy, rewritten from the page."]);   // shown at once
        GCHECK_EQ_STR(tv.undoManager.undoActionName, @"Preview Edit");
        [tv.undoManager undo];
        GCHECK([tv.string isEqualToString:before]);
        [panel close];
    });

    MCGuiStep(@"following a long page costs little main-thread CPU time", ^{
        // The map is made once a render (the render's last step, its
        // page following the source); each scroll after it is a search of
        // it and a few rects. The render alone, for scale:
        double c0 = CpuMs();
        [Side(c) holdFollowing:^{ [c renderSidePreview]; }];
        const double render = CpuMs() - c0;
        c0 = CpuMs();
        [Side(c) pageRendered];   // the map made, the block marked, the page followed
        const double first = CpuMs() - c0;
        c0 = CpuMs();
        for (int k = 0; k < 200; k++) ScrollSourceToLine(c, (NSUInteger)((k * 37) % 1190));
        const double sourceLed = CpuMs() - c0;
        NSTextView *page = Side(c).textView;
        const CGFloat height = NSHeight(page.frame);
        c0 = CpuMs();
        for (int k = 0; k < 200; k++)
            MCScrollTextViewToY(Side(c).scrollView, page, fmod(k * 997.0, height));
        const double pageLed = CpuMs() - c0;
        MCGuiNote(@"CPU: render %.1f ms; map %.1f ms; 200 source-led %.1f ms; 200 page-led %.1f ms",
                  render, first, sourceLed, pageLed);
        GCHECK(first < MAX(400, render));
        GCHECK(sourceLed < 400 && pageLed < 400);
    });

    // 20,000 lines: a pause in typing still costs only its own piece, and
    // the page is laid out only as far as it is followed.
    NSString *big = [folder stringByAppendingPathComponent:@"big.md"];
    MCGuiStep(@"20,000 lines with the page beside them", ^{
        NSMutableString *t = [NSMutableString string];
        for (int k = 0; k < 2000; k++)
            [t appendFormat:@"## Part %d\n\nParagraph %d line one\ncontinues on line two of %d.\n\n"
                            @"```\ncode %d a\ncode %d b\n```\n\n", k, k, k, k, k];
        MCGuiWriteFile(big, t);
        double c0 = MCGuiThreadCpuMs();
        GCHECK([c showFile:big]);
        if (c.previewMode) [c togglePreview:nil];
        [c focusEditor:nil];
        if (![c sidePreviewShowing]) [c toggleSidePreview:nil];
        const double opened = MCGuiThreadCpuMs() - c0;
        GCHECK([c sidePreviewShowing] && [Page(c) containsString:@"Part 1999"]);
        c0 = MCGuiThreadCpuMs();
        ScrollSourceToLine(c, 10000);
        MCGuiNote(@"CPU: opened with the page %.0f ms, followed to line 10,000 %.0f ms", opened,
                  MCGuiThreadCpuMs() - c0);
        GCHECK(labs(PageTop(c) - 10000) <= 1);
        NSTextView *tv = Source(c);
        MCLineGutter *g = [c valueForKey:@"gutter"];
        tv.selectedRange = NSMakeRange([g startOfLine:10012], 0);
        Type(tv, @"Typed ");
        ScrollSourceToLine(c, 10000);
    });
    MCGuiWait(@"the long page to follow the typing", 10, ^BOOL {
        return [Page(c) containsString:@"Typed Paragraph 1001"];
    });
    MCGuiStep(@"the pause cost under 30 ms of the main thread, and the page is right", ^{
        CHECK_RENDER_COST(c, 30, 205, @"a pause's render at 20,000 lines");
        // The page at the source's line (the source's view, its own height
        // not yet back after the edit, may stop a few lines short of 10,000).
        MCGuiNote(@"source %lu, page %ld", (unsigned long)SourceTop(c), (long)PageTop(c));
        // With scroll bars that take room the source settles two lines
        // further (9,995 against the page's 9,997).
        GCHECK(labs((NSInteger)SourceTop(c) - 10000) <= 8 &&
               labs(PageTop(c) - (NSInteger)SourceTop(c)) <= 3 && !Side(c).pageLeads);
        GCHECK_EQ_STR(Page(c), WholePage(c));
        // Once more, at the very end of the page, which has been followed there.
        ScrollSourceToLine(c, 19990);
        NSTextView *tv = Source(c);
        tv.selectedRange = NSMakeRange(tv.string.length, 0);
        Type(tv, @"The last words.\n");
    });
    MCGuiWait(@"the end of the long page to follow", 10, ^BOOL {
        return [Page(c) hasSuffix:@"The last words.\n"];
    });
    MCGuiStep(@"at the end too", ^{
        CHECK_RENDER_COST(c, 30, 205, @"a render at the end of 20,000 lines");
        [Source(c).undoManager removeAllActions];
        for (MCTab *t in [(NSArray *)[c valueForKey:@"tabs"] copy])
            if ([t.currentPath isEqualToString:big]) [c closeTab:t prompt:NO];
    });

    NSString *folded = [folder stringByAppendingPathComponent:@"details.md"];
    MCGuiStep(@"a closed <details> block: its body is not on the page, lines below still sync", ^{
        NSMutableString *t = [NSMutableString string];
        for (int k = 0; k < 20; k++) [t appendFormat:@"Above %d.\n\n", k];    // lines 0 to 39
        [t appendString:@"<details>\n<summary>Folded notes</summary>\n\n"];  // 40, 41, 42
        for (int k = 0; k < 40; k++) [t appendFormat:@"Hidden %d.\n\n", k];   // 43 to 122
        [t appendString:@"</details>\n\n"];                                   // 123, 124
        for (int k = 0; k < 60; k++) [t appendFormat:@"Below %d.\n\n", k];    // 125 + 2k
        MCGuiWriteFile(folded, t);
        GCHECK([c showFile:folded]);
        if (c.previewMode) [c togglePreview:nil];
        [c focusEditor:nil];
        if (![c sidePreviewShowing]) [c toggleSidePreview:nil];
        GCHECK([c sidePreviewShowing]);
        GCHECK([Page(c) containsString:@"Folded notes"] && ![Page(c) containsString:@"Hidden"]);
        for (NSUInteger line : {165u, 205u}) {
            ScrollSourceToLine(c, line);
            MCGuiNote(@"source at %lu: page at %ld", (unsigned long)SourceTop(c), (long)PageTop(c));
            GCHECK(labs(PageTop(c) - (NSInteger)line) <= 1);
        }
        MCScrollTextViewToY(SourceScroll(c), Source(c), 0);
    });
    MCGuiStep(@"a click on its summary opens it, built as a whole render builds it", ^{
        NSTextView *page = Side(c).textView;
        NSString *link = @"mutible-details:40:41:0";
        const NSUInteger at = LinkAt(page, link);
        GCHECK(at != NSNotFound);
        if (at == NSNotFound) return;
        GCHECK([c sidePreviewClickedLink:link atIndex:at]);
        GCHECK([Page(c) containsString:@"Hidden 39."]);
        GCHECK(LinkAt(page, @"mutible-details:40:41:1") != NSNotFound);
        GCHECK_EQ_STR(Page(c), WholePage(c));
        ScrollSourceToLine(c, 63);
        MCGuiNote(@"open: source at 63, page at %ld", (long)PageTop(c));
        GCHECK(labs(PageTop(c) - 63) <= 1);
        // The full preview of the same tab shows it open too.
        [c togglePreview:nil];
        GCHECK(c.previewMode && [Source(c).string containsString:@"Hidden 39."]);
        [c togglePreview:nil];
        for (MCTab *tab in [(NSArray *)[c valueForKey:@"tabs"] copy])
            if ([tab.currentPath isEqualToString:folded]) [c closeTab:tab prompt:NO];
    });

    // The window goes with the suite (close, not performClose:, so no
    // question about anything the steps left unsaved).
    __block EditorController *closing = nil;
    MCGuiStep(@"done", ^{
        [Source(c).undoManager removeAllActions];
        for (MCTab *t in [(NSArray *)[c valueForKey:@"tabs"] copy])
            if ([t.currentPath isEqualToString:md]) [c closeTab:t prompt:NO];
        closing = c;
        c = nil;
        [closing.window close];
    });
    MCGuiWait(@"its window is gone", 10, ^BOOL {
        if ([MCGuiControllers() containsObject:closing]) return NO;
        closing = nil;
        return YES;
    });
}
