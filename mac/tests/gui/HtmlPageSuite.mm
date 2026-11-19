// HtmlPageSuite.mm — `make guitest SUITES=html_page`: an .html file opens
// as its page (HtmlPageView.h, FileViewers.h's MCViewerHtml): its stylesheet
// and picture load from its folder; with html.scripts unset the page shows
// with its scripts off under a question, and Run Scripts runs them (for
// that file until quit, or for every file once remembered, which writes
// html.scripts = on); the page never takes the keyboard by itself, even
// from an autofocus field and a script's focus(); Shift+Cmd+V flips to the
// source and back; a save, and a write from outside, load the page again
// with its scroll kept; a link opens its file in a tab of its own; a meta
// refresh to zoommtg:// never reaches openURL:, nor one to https:// the
// browser pane while scripts are off; a page with no scripts is never
// asked about. Open Preview to the Side puts the
// page beside the editable source, follows the typing before any save
// (its stylesheet and picture still the folder's), and closes again.
//
// Only local pages (file://), in a window of the suite's own on a scratch
// folder. Page state is read in an isolated world, which runs whether or
// not the page's own scripts may.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "AppSettings.h"
#import "Browser.h"
#import "ControlServer.h"
#import "FileViewers.h"
#import "HtmlPageView.h"
#import <WebKit/WebKit.h>

namespace {

NSString *gDir;
NSString *F(NSString *name) { return [gDir stringByAppendingPathComponent:name]; }

MCFileViewers *Viewers(EditorController *c) { return [c valueForKey:@"viewers"]; }
MCHtmlPage *Page(EditorController *c) { return Viewers(c).html; }
WKWebView *Web(EditorController *c) { return Page(c).webView; }
NSTextView *EditorText(EditorController *c) { return [c valueForKey:@"textView"]; }

// Whether the page on show is the file, loaded at least `loads` times.
BOOL Loaded(EditorController *c, NSString *name, NSUInteger loads) {
    WKWebView *w = Web(c);
    return w && !w.isLoading && Page(c).loads >= loads && !Page(c).isHiddenOrHasHiddenAncestor &&
           [w.URL.path.lastPathComponent isEqualToString:name];
}

// A script's answer from the page, in an isolated world.
void Ask(EditorController *c, NSString *js, void (^answer)(id value)) {
    [Web(c) evaluateJavaScript:js inFrame:nil inContentWorld:WKContentWorld.defaultClientWorld
             completionHandler:^(id value, NSError *error) {
                 if (error) MCGuiNote(@"script error: %@", error.localizedDescription);
                 answer(value);
             }];
}

// The page beside the source (Open Preview to the Side), and a script's
// answer from it.
MCHtmlPage *SidePage(EditorController *c) { return [c valueForKey:@"sideHtml"]; }
void AskSide(EditorController *c, NSString *js, void (^answer)(id value)) {
    [SidePage(c).webView evaluateJavaScript:js inFrame:nil
                             inContentWorld:WKContentWorld.defaultClientWorld
                          completionHandler:^(id value, NSError *error) {
                              if (error) MCGuiNote(@"script error: %@", error.localizedDescription);
                              answer(value);
                          }];
}
BOOL SideLoaded(EditorController *c, NSUInteger loads) {
    MCHtmlPage *p = SidePage(c);
    return p.webView && !p.webView.isLoading && p.loads >= loads && !p.restoringPlace &&
           !p.isHiddenOrHasHiddenAncestor;
}

BOOL KeyboardInPage(EditorController *c) {
    NSResponder *r = c.window.firstResponder;
    return Web(c) && [r isKindOfClass:NSView.class] && [(NSView *)r isDescendantOf:Web(c)];
}

NSString *ScriptedPage(NSString *title, NSString *marker) {
    return [NSString stringWithFormat:
        @"<!doctype html><html><head><title>%@</title>"
        @"<link rel=stylesheet href='style.css'></head><body>"
        @"<input id=field autofocus>"
        @"<p id=marker>%@</p><img id=pic src='pic.svg'>"
        @"<p><a id=link href='other.html'>other</a> <a id=top href='#end'>end</a></p>"
        @"<div style='height:4000px'></div><p id=end>end</p>"
        @"<script>document.body.dataset.ran = 'yes';"
        @"document.getElementById('field').focus();</script>"
        @"</body></html>", title, marker];
}

}  // namespace

MC_GUI_SUITE(html_page) {
    __block EditorController *c = nil;
    gDir = [MCGuiScratch() stringByAppendingPathComponent:@"html-page"];

    MCGuiStep(@"a folder with pages, a stylesheet and a picture, in a window of its own", ^{
        MCGuiSetSettings(@"lsp.enabled = false\n");
        MCGuiWriteFile(F(@"style.css"), @"body { color: rgb(1, 2, 3); }\n");
        MCGuiWriteFile(F(@"pic.svg"),
                       @"<svg xmlns='http://www.w3.org/2000/svg' width='37' height='5'></svg>");
        MCGuiWriteFile(F(@"index.html"), ScriptedPage(@"Index", @"First"));
        MCGuiWriteFile(F(@"second.html"), ScriptedPage(@"Second", @"Two"));
        MCGuiWriteFile(F(@"third.html"), ScriptedPage(@"Third", @"Three"));
        MCGuiWriteFile(F(@"other.html"), @"<html><head><title>Other</title></head>"
                                         @"<body><p>plain, no scripts</p></body></html>");
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:gDir];
        c = MCGuiController();
        MCGuiSetKeyWindow(c.window);
        GCHECK([c showFile:F(@"index.html")]);
    });
    MCGuiStep(@"index.html opens as its page, asking before its scripts run", ^{
        GCHECK(c.previewMode);
        GCHECK(c.viewerKind == MCViewerHtml);
        GCHECK(Page(c) != nil && !Page(c).hidden);
        GCHECK(EditorText(c).enclosingScrollView.hidden || !EditorText(c).editable);
        GCHECK(Page(c).asking && !Page(c).runsScripts);
        GCHECK(!Page(c).questionBar.hidden);
        GCHECK(NSMaxY(Page(c).questionBar.frame) == NSMaxY(Page(c).bounds));
        GCHECK(NSMaxY(Web(c).frame) <= NSMinY(Page(c).questionBar.frame));
    });
    MCGuiWait(@"the page loads", 20, ^BOOL { return Loaded(c, @"index.html", 1); });
    MCGuiAsync(@"its stylesheet and picture load; its script has not run", 10, ^(MCGuiDone done) {
        Ask(c, @"[getComputedStyle(document.body).color, document.getElementById('pic').naturalWidth,"
               @" document.styleSheets.length, document.body.dataset.ran || 'no']",
            ^(id v) {
                NSArray *a = [v isKindOfClass:NSArray.class] ? v : nil;
                GCHECK(a.count == 4);
                GCHECK_EQ_STR(a[0], @"rgb(1, 2, 3)");
                GCHECK([a[1] intValue] == 37);
                GCHECK([a[2] intValue] == 1);
                GCHECK_EQ_STR(a[3], @"no");
                MCGuiSnapshot(c.window.contentView, @"html-page-asking");
                done();
            });
    });
    MCGuiStep(@"Run Scripts, not remembered: the page loads again with them, in the same place", ^{
        GCHECK(!KeyboardInPage(c));
        const NSRect pageFrame = Web(c).frame;
        [Page(c) answerScripts:YES remember:NO];
        GCHECK(!Page(c).asking && Page(c).runsScripts);
        // The bar keeps its place as a line saying what was chosen, so the
        // page does not jump up under the pointer.
        GCHECK(Page(c).noting && !Page(c).questionBar.hidden);
        GCHECK(NSEqualRects(Web(c).frame, pageFrame));
        GCHECK([[Page(c).questionBar viewWithTag:1] isHidden]);
        NSString *line = nil;
        for (NSView *v in Page(c).questionBar.subviews)
            if ([v isKindOfClass:NSTextField.class] && !v.hidden) line = ((NSTextField *)v).stringValue;
        MCGuiNote(@"the bar now says: %@", line);
        GCHECK([line hasPrefix:@"This page's scripts run"]);
        GCHECK(![[AppSettings shared].fileText containsString:@"\nhtml.scripts"]);
    });
    MCGuiWait(@"the page runs its scripts", 20, ^BOOL {
        return Loaded(c, @"index.html", 2) && !Page(c).restoringPlace;
    });
    MCGuiAsync(@"the script ran", 10, ^(MCGuiDone done) {
        Ask(c, @"document.body.dataset.ran || 'no'", ^(id v) {
            GCHECK_EQ_STR(v, @"yes");
            done();
        });
    });
    // Time is the condition: the autofocus field and the script's focus()
    // have had their chance to take the keyboard.
    MCGuiPause(0.5);
    MCGuiStep(@"the page never took the keyboard by itself", ^{
        GCHECK(!KeyboardInPage(c));
    });
    MCGuiAsync(@"scrolled down", 10, ^(MCGuiDone done) {
        Ask(c, @"window.scrollTo(0, 600); window.scrollY", ^(id v) {
            GCHECK([v doubleValue] == 600);
            done();
        });
    });

    // ------------------------------------------- the source, and saving
    __block NSUInteger loadsBefore = 0;
    MCGuiStep(@"Shift+Cmd+V shows the source; an edit saved there shows on the page", ^{
        [c togglePreview:nil];
        GCHECK(!c.previewMode);
        GCHECK(Page(c).hidden);
        NSTextView *tv = EditorText(c);
        GCHECK(tv.editable && [tv.string containsString:@"<p id=marker>First</p>"]);
        const NSRange r = [tv.string rangeOfString:@"First"];
        MCGuiUndoGroup(tv.undoManager, ^{ [tv insertText:@"Edited" replacementRange:r]; });
        [c saveCurrentFile:nil];
        GCHECK([[NSString stringWithContentsOfFile:F(@"index.html") encoding:NSUTF8StringEncoding
                                             error:nil] containsString:@">Edited<"]);
        loadsBefore = Page(c).loads;
        [c togglePreview:nil];
        GCHECK(c.previewMode && !Page(c).hidden);
        GCHECK(c.window.firstResponder == Web(c) || KeyboardInPage(c));   // asked for: given
    });
    // Until the page has loaded the saved file and finished putting its
    // place back (HtmlPage's restoringPlace), then read once.
    MCGuiWait(@"the page loads the saved file and puts its place back", 20, ^BOOL {
        return Loaded(c, @"index.html", loadsBefore + 1) && !Page(c).restoringPlace;
    });
    __block NSArray *seen = nil;
    MCGuiAsync(@"its new text, scrolled where it was", 10, ^(MCGuiDone done) {
        Ask(c, @"[document.getElementById('marker').textContent, window.scrollY]", ^(id v) {
            seen = [v isKindOfClass:NSArray.class] ? v : nil;
            MCGuiNote(@"after the save: %@", [seen componentsJoinedByString:@", "]);
            GCHECK(seen.count == 2);
            GCHECK_EQ_STR(seen.firstObject, @"Edited");
            GCHECK(fabs([seen.lastObject doubleValue] - 600) < 2);
            done();
        });
    });
    MCGuiStep(@"a write from outside", ^{
        loadsBefore = Page(c).loads;
        MCGuiWriteFile(F(@"index.html"), ScriptedPage(@"Index", @"Outside"));
    });
    MCGuiWait(@"the page follows the file and puts its place back", 20, ^BOOL {
        return Loaded(c, @"index.html", loadsBefore + 1) && !Page(c).restoringPlace;
    });
    MCGuiAsync(@"the new text, the scroll kept", 10, ^(MCGuiDone done) {
        Ask(c, @"[document.getElementById('marker').textContent, window.scrollY,"
               @" document.body.dataset.ran || 'no']", ^(id v) {
            seen = [v isKindOfClass:NSArray.class] ? v : nil;
            MCGuiNote(@"after the write from outside: %@", [seen componentsJoinedByString:@", "]);
            GCHECK(seen.count == 3);
            GCHECK_EQ_STR(seen.firstObject, @"Outside");
            GCHECK(fabs([seen[1] doubleValue] - 600) < 2);
            GCHECK_EQ_STR(seen.lastObject, @"yes");   // the answer holds for the file
            done();
        });
    });
    MCGuiAsync(@"a link to a place in the page scrolls there", 10, ^(MCGuiDone done) {
        Ask(c, @"document.getElementById('top').click(); true", ^(id v) {
            (void)v;
            done();
        });
    });
    __block double scrollY = 0;
    MCGuiWait(@"scrolled to the end, on the same page", 10, ^BOOL {
        Ask(c, @"window.scrollY", ^(id v) { scrollY = [v doubleValue]; });
        return scrollY > 2000 && [c.currentPath hasSuffix:@"/index.html"] &&
               [Web(c).URL.path hasSuffix:@"/index.html"];
    });

    // ------------------------------------------------------------ links
    MCGuiAsync(@"a link to another page", 10, ^(MCGuiDone done) {
        Ask(c, @"document.getElementById('link').click(); true", ^(id v) {
            (void)v;
            done();
        });
    });
    MCGuiWait(@"opens that file in a tab of its own, as a page", 20, ^BOOL {
        return [c.currentPath hasSuffix:@"/other.html"] && Loaded(c, @"other.html", 1);
    });
    MCGuiStep(@"a page with no scripts is not asked about; the answered line went with the open", ^{
        GCHECK(c.viewerKind == MCViewerHtml && c.previewMode);
        GCHECK(!Page(c).asking && !Page(c).noting && Page(c).questionBar.hidden);
    });

    // A page that goes somewhere by itself (round 10's documents defect
    // 2): a meta refresh to another app's scheme opened that app with no
    // click. Only a link the user clicks, or a form they send, is followed;
    // a web address the page goes to by itself only while its scripts run.
    __block NSUInteger openedBefore = 0;
    MCGuiStep(@"a page whose meta refresh goes to zoommtg://", ^{
        openedBefore = MCGuiOpenedURLs().count;
        MCGuiWriteFile(F(@"refresh.html"),
                       @"<html><head><title>Refresh</title>"
                       @"<meta http-equiv=\"refresh\" content=\"0; url=zoommtg://zoom.us/join?confno=1\">"
                       @"</head><body><p id=marker>refreshing</p></body></html>");
        GCHECK([c showFile:F(@"refresh.html")]);
    });
    MCGuiWait(@"loads", 20, ^BOOL { return Loaded(c, @"refresh.html", 1); });
    MCGuiWait(@"and its refresh is refused, said in the status bar", 10, ^BOOL {
        return [[[c valueForKey:@"settingsLabel"] stringValue] containsString:@"zoommtg://"];
    });
    MCGuiStep(@"nothing reached openURL:, and the page is still the file", ^{
        NSArray<NSURL *> *opened = MCGuiOpenedURLs();
        MCGuiNote(@"opened %@; status \"%@\"", opened,
                  [[c valueForKey:@"settingsLabel"] stringValue]);
        GCHECK(opened.count == openedBefore);
        for (NSURL *u in opened) GCHECK(![u.scheme.lowercaseString isEqualToString:@"zoommtg"]);
        GCHECK([c.currentPath hasSuffix:@"/refresh.html"] && c.viewerKind == MCViewerHtml);
        GCHECK([[[c valueForKey:@"settingsLabel"] stringValue]
            containsString:@"mutible did not open it"]);
    });
    MCGuiStep(@"one to a web address, with no scripts running", ^{
        MCGuiWriteFile(F(@"refresh-web.html"),
                       @"<html><head><title>Web</title>"
                       @"<meta http-equiv=\"refresh\" content=\"0; url=https://example.invalid/\">"
                       @"</head><body><p>refreshing</p></body></html>");
        GCHECK([c showFile:F(@"refresh-web.html")]);
    });
    MCGuiWait(@"is not followed either", 20, ^BOOL {
        return Loaded(c, @"refresh-web.html", 1) &&
               [[[c valueForKey:@"settingsLabel"] stringValue] containsString:@"example.invalid"];
    });
    MCGuiStep(@"(nothing opened, the page still the file)", ^{
        GCHECK(MCGuiOpenedURLs().count == openedBefore);
        GCHECK([c.currentPath hasSuffix:@"/refresh-web.html"]);
        GCHECK([[[c valueForKey:@"settingsLabel"] stringValue] containsString:@"was not followed"]);
    });

    // ------------------------------------------------- remembered answers
    MCGuiStep(@"another scripted page asks; Run Scripts remembered writes the setting", ^{
        GCHECK([c showFile:F(@"second.html")]);
        GCHECK(Page(c).asking);
        [Page(c) answerScripts:YES remember:YES];
        GCHECK([[AppSettings shared].fileText containsString:@"html.scripts = on"]);
        GCHECK([AppSettings shared].settings.htmlScripts() == Settings::HtmlScripts::On);
        GCHECK(!Page(c).asking && Page(c).runsScripts);
        GCHECK(Page(c).noting && !Page(c).questionBar.hidden);
        // Its close box takes the line away, and the page has the room.
        [Page(c) endNote];
        GCHECK(!Page(c).noting && Page(c).questionBar.hidden);
        GCHECK(NSMaxY(Web(c).frame) == NSMaxY(Page(c).bounds));
    });
    MCGuiStep(@"from then on pages run their scripts unasked", ^{
        GCHECK([c showFile:F(@"third.html")]);
        GCHECK(!Page(c).asking && Page(c).runsScripts);
    });
    MCGuiWait(@"the third page loads", 20, ^BOOL { return Loaded(c, @"third.html", 1); });
    MCGuiAsync(@"its script ran", 10, ^(MCGuiDone done) {
        Ask(c, @"document.body.dataset.ran || 'no'", ^(id v) {
            GCHECK_EQ_STR(v, @"yes");
            done();
        });
    });
    MCGuiStep(@"html.scripts = off: never run, never asked", ^{
        MCGuiSetSettings(@"lsp.enabled = false\nhtml.scripts = off\n");
        [[AppSettings shared] reload];
        GCHECK(!Page(c).asking && !Page(c).runsScripts);
    });

    // ------------------------------------------- the page beside the source
    __block NSUInteger sideLoads = 0;
    MCGuiStep(@"Open Preview to the Side (Control+Command+V) on index.html", ^{
        GCHECK([c showFile:F(@"index.html")]);
        GCHECK(c.previewMode && c.canToggleSidePreview);
        GCHECK(MCGuiPerformMenuKey(c.window, 9 /* V */,
                                   NSEventModifierFlagControl | NSEventModifierFlagCommand, nil, nil));
        GCHECK(!c.previewMode && [c pageSideShowing]);
    });
    MCGuiStep(@"its source, editable and with the keyboard, its page beside it", ^{
        NSView *src = c.editorScroll;
        MCHtmlPage *side = SidePage(c);
        GCHECK(side != nil && !side.hidden && Page(c).hidden);
        MCGuiNote(@"source %@, page %@", NSStringFromRect(src.frame), NSStringFromRect(side.frame));
        GCHECK(!src.hidden && NSMinX(side.frame) >= NSMaxX(src.frame) - 1);
        GCHECK(NSWidth(src.frame) >= 150 && NSWidth(side.frame) >= 150);
        GCHECK(EditorText(c).editable && c.window.firstResponder == EditorText(c));
    });
    MCGuiWait(@"the page beside it loads", 20, ^BOOL { return SideLoaded(c, 1); });
    MCGuiAsync(@"the file's page, its stylesheet loaded", 10, ^(MCGuiDone done) {
        AskSide(c, @"[document.getElementById('marker').textContent,"
                   @" getComputedStyle(document.body).color]", ^(id v) {
            NSArray *a = [v isKindOfClass:NSArray.class] ? v : nil;
            MCGuiNote(@"beside: %@", [a componentsJoinedByString:@", "]);
            GCHECK(a.count == 2);
            GCHECK_EQ_STR(a.firstObject, @"Outside");
            GCHECK_EQ_STR(a.lastObject, @"rgb(1, 2, 3)");
            done();
        });
    });
    MCGuiStep(@"typed in the source, not saved", ^{
        sideLoads = SidePage(c).loads;
        NSTextView *tv = EditorText(c);
        const NSRange r = [tv.string rangeOfString:@"<p id=marker>"];
        GCHECK(r.location != NSNotFound);
        if (r.location == NSNotFound) return;
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:@"Typed " replacementRange:NSMakeRange(NSMaxRange(r), 0)];
        });
        GCHECK(c.dirty);
    });
    MCGuiWait(@"the page beside it follows the typing", 20, ^BOOL {
        return SideLoaded(c, sideLoads + 1);
    });
    MCGuiAsync(@"showing the typed text, relative addresses still the folder's", 10,
               ^(MCGuiDone done) {
        AskSide(c, @"[document.getElementById('marker').textContent,"
                   @" getComputedStyle(document.body).color,"
                   @" document.getElementById('pic').naturalWidth]", ^(id v) {
            NSArray *a = [v isKindOfClass:NSArray.class] ? v : nil;
            MCGuiNote(@"after typing: %@", [a componentsJoinedByString:@", "]);
            GCHECK(a.count == 3);
            GCHECK_EQ_STR(a.firstObject, @"Typed Outside");
            GCHECK_EQ_STR(a[1], @"rgb(1, 2, 3)");
            GCHECK([a[2] intValue] == 37);
            GCHECK(![[NSString stringWithContentsOfFile:F(@"index.html")
                                                encoding:NSUTF8StringEncoding
                                                   error:nil] containsString:@"Typed"]);
            done();
        });
    });
    MCGuiStep(@"Control+Command+V again closes it, the source keeping the slot", ^{
        [c toggleSidePreview:nil];
        GCHECK(![c pageSideShowing] && SidePage(c).hidden);
        GCHECK(!c.previewMode && !c.editorScroll.hidden);
        GCHECK(c.window.firstResponder == EditorText(c));
        [EditorText(c).undoManager undo];
        GCHECK(![EditorText(c).string containsString:@"Typed"]);
    });

    MCGuiStep(@"the suite's window closes", ^{
        [c.window close];
        c = nil;
        MCGuiSetKeyWindow(nil);
    });
}
