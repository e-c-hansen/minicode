// EditorController+WindowTitle.mm — the window's title, by the core's rule
// (src/WindowTitle.h): with the document on show and the keyboard in the
// editor (or the tree, the git panel, any pane but the terminal's and the
// browser's), the file's path from the window's folder, ~ for home outside
// it, then " — mutible" ("prep/snippets/a.json — mutible"); an untitled tab
// its name; a diff, a page or a comparison the name it is given. The
// terminal with the keyboard shows its title, the browser its page's
// title. "● " in front for unsaved changes in the tab on show, and after
// the name a picture's size, a PDF's pages, a player's length, or a
// Markdown file's tasks done, as before. The window's representedURL is
// the file while its path is the title, so the proxy icon and Cmd+click on
// the title work; none otherwise.
//
// updateTitle (EditorController+Layout.mm) sets it after a change to the
// tab; a change of who has the keyboard (the window's firstResponder,
// observed here) and the terminal's or the browser's titles changing
// (their strips' onTitlesChanged) set it again, and nothing else.
#import "EditorController+Private.h"
#import "Browser.h"
#import "Terminal.h"
#import "TerminalPanel.h"
#include "WindowTitle.h"

@implementation EditorController (WindowTitle)

static void *kWindowTitleFocusContext = &kWindowTitleFocusContext;

- (void)windowTitleStartObservingFocus {
    [self.window addObserver:self forKeyPath:@"firstResponder" options:0
                     context:kWindowTitleFocusContext];
}

- (void)windowTitleStopObservingFocus {
    [self.window removeObserver:self forKeyPath:@"firstResponder"
                        context:kWindowTitleFocusContext];
}

- (BOOL)windowTitleObserved:(NSString *)keyPath of:(id)object context:(void *)context {
    if (context != kWindowTitleFocusContext) return NO;
    if (object == self.window) [self refreshWindowTitle];
    return YES;
}

// Who has the keyboard, as the title tells it.
- (WindowTitle::Focus)windowTitleFocus {
    NSResponder *fr = self.window.firstResponder;
    NSView *v = [fr isKindOfClass:NSView.class] ? (NSView *)fr : nil;
    if (v && self.terminalPanel && self.terminalVisible && [v isDescendantOf:self.terminalPanel])
        return WindowTitle::Focus::Terminal;
    if (v && self.browser && self.browserVisible && [v isDescendantOf:self.browser])
        return WindowTitle::Focus::Browser;
    return WindowTitle::Focus::Document;
}

- (void)windowTitlePaneChanged {
    if ([self windowTitleFocus] != WindowTitle::Focus::Document) [self refreshWindowTitle];
}

// The terminal pane holding the keyboard, found from the first responder:
// never MCTerminalPanel's activeTerminal, whose getter makes the focused
// pane its tab's active one (here, mid-switch, the old tab's pane became
// the new tab's: host_reattach).
- (TerminalView *)windowTitleFocusedTerminal {
    NSResponder *fr = self.window.firstResponder;
    for (NSView *v = [fr isKindOfClass:NSView.class] ? (NSView *)fr : nil; v; v = v.superview)
        if ([v isKindOfClass:TerminalView.class]) return (TerminalView *)v;
    return nil;
}

// A path spelled as the folder and home are: /private/tmp/x and /tmp/x
// alike. Its folder standardized, so a file already deleted keeps it too.
static std::string TitlePath(NSString *p) {
    if (!p.length) return "";
    NSString *dir = p.stringByDeletingLastPathComponent.stringByStandardizingPath;
    return Utf8String([dir stringByAppendingPathComponent:p.lastPathComponent]);
}

// m:ss, or h:mm:ss from an hour up.
static NSString *MCFormatDuration(double seconds) {
    long t = (long)llround(seconds);
    long h = t / 3600, m = (t / 60) % 60, s = t % 60;
    return h > 0 ? [NSString stringWithFormat:@"%ld:%02ld:%02ld", h, m, s]
                 : [NSString stringWithFormat:@"%ld:%02ld", m, s];
}

// What follows " — mutible" for the document on show.
- (NSString *)windowTitleDetail {
    NSString *mode = (self.canTogglePreview && self.previewMode) ? @"  [Preview]" : @"";
    if (self.isImage)
        mode = [NSString stringWithFormat:@"  %.0f × %.0f",
                self.imagePixels.width, self.imagePixels.height];
    if (self.isPDF) {
        NSUInteger n = self.pdfView.document.pageCount;
        mode = n == 1 ? @"  1 page"
                      : [NSString stringWithFormat:@"  %lu pages", (unsigned long)n];
    }
    if (self.isMedia) {
        // Video: pixel size as shown (rotation applied), then the duration.
        // Audio has no picture, so only the duration.
        NSMutableString *m = [NSMutableString string];
        NSSize px = self.mediaPixels;
        if (px.width > 0 && px.height > 0)
            [m appendFormat:@"  %.0f × %.0f", px.width, px.height];
        if (self.mediaSeconds >= 0)
            [m appendFormat:@"  %@", MCFormatDuration(self.mediaSeconds)];
        mode = m;
    }
    // A Markdown file with tasks: how many are done.
    if (self.isMarkdown && self.currentPath && !self.isDiff) {
        NSString *src = self.textView.editable ? self.textView.string : self.sourceText;
        MarkdownTasks::Counts c = MarkdownTasks::count(Utf8String(src ?: @""));
        if (c.total > 0)
            mode = [mode stringByAppendingFormat:@"  %d of %d done", c.done, c.total];
    }
    return mode;
}

- (void)refreshWindowTitle {
    NSWindow *w = self.window;
    if (!w) return;
    WindowTitle::Input in;
    in.focus = [self windowTitleFocus];
    in.folder = TitlePath(_root.path);
    in.home = TitlePath(NSHomeDirectory());
    const BOOL document = in.focus == WindowTitle::Focus::Document;
    if (in.focus == WindowTitle::Focus::Terminal)
        in.pane = Utf8String([self windowTitleFocusedTerminal].title);
    else if (in.focus == WindowTitle::Focus::Browser)
        in.pane = Utf8String(self.browser.pageTitle);
    NSURL *represented = nil;
    if (document) {
        if (self.isDiff) {
            NSString *name = _activeTab.page ? (_activeTab.page.pageTitle ?: @"")
                           : _activeTab.comparison ? (_activeTab.comparison.title ?: @"Diff")
                           : self.diffIsCommit ? self.diffName
                           : [NSString stringWithFormat:@"%@ (diff)", self.diffName];
            in.name = Utf8String(name);   // none yet: the folder's name
        } else if (self.currentPath) {
            in.path = TitlePath(self.currentPath);
            in.untitled = _activeTab.untitled;
            in.gone = _activeTab.gone;
            if (!in.untitled && !in.gone) represented = [NSURL fileURLWithPath:self.currentPath];
        }
        in.dirty = self.dirty;
        in.detail = Utf8String([self windowTitleDetail]);
    }
    NSString *title = [NSString stringWithUTF8String:WindowTitle::title(in).c_str()] ?: @"mutible";
    if (![w.title isEqualToString:title]) w.title = title;
    if (!(w.representedURL == represented || [w.representedURL isEqual:represented]))
        w.representedURL = represented;
}

@end
