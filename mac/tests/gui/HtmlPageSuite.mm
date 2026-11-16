// HtmlPageSuite.mm — `make guitest SUITES=html_page`: an .html file opens
// as its page (HtmlPage.h, FileViewers.h's MCViewerHtml): its stylesheet
// and picture load from its folder; with html.scripts unset the page shows
// with its scripts off under a question, and Run Scripts runs them (for
// that file until quit, or for every file once remembered, which writes
// html.scripts = on); the page never takes the keyboard by itself, even
// from an autofocus field and a script's focus(); Shift+Cmd+V flips to the
// source and back; a save, and a write from outside, load the page again
// with its scroll kept; a link opens its file in a tab of its own; a page
// with no scripts is never asked about.
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
#import "HtmlPage.h"
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
    MCGuiStep(@"Run Scripts, not remembered: the page loads again with them", ^{
        GCHECK(!KeyboardInPage(c));
        [Page(c) answerScripts:YES remember:NO];
        GCHECK(!Page(c).asking && Page(c).questionBar.hidden);
        GCHECK(Page(c).runsScripts);
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
    MCGuiStep(@"a page with no scripts is not asked about", ^{
        GCHECK(c.viewerKind == MCViewerHtml && c.previewMode);
        GCHECK(!Page(c).asking && Page(c).questionBar.hidden);
    });

    // ------------------------------------------------- remembered answers
    MCGuiStep(@"another scripted page asks; Run Scripts remembered writes the setting", ^{
        GCHECK([c showFile:F(@"second.html")]);
        GCHECK(Page(c).asking);
        [Page(c) answerScripts:YES remember:YES];
        GCHECK([[AppSettings shared].fileText containsString:@"html.scripts = on"]);
        GCHECK([AppSettings shared].settings.htmlScripts() == Settings::HtmlScripts::On);
        GCHECK(!Page(c).asking && Page(c).runsScripts);
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

    MCGuiStep(@"the suite's window closes", ^{
        [c.window close];
        c = nil;
        MCGuiSetKeyWindow(nil);
    });
}
