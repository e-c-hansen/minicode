// EditorController+SessionBrowser.mm — EditorController: View > Show
// Session in Browser… and Copy Session Browser Link (SessionBrowser.h). The
// window's server is kept as an associated object, so the class gains no
// ivars, and it stops itself when the window closes.
#import "EditorController+Private.h"
#import "AppSettings.h"
#import "SessionBrowser.h"
#import "SessionLink.h"
#include <objc/runtime.h>

static char kSessionBrowserKey;

@implementation EditorController (SessionBrowser)

- (MCSessionBrowser *)sessionBrowser {
    return objc_getAssociatedObject(self, &kSessionBrowserKey);
}

// With the setting off, says what the page needs and offers to turn it on:
// YES once it is on.
- (BOOL)sessionHostReadyAsking {
    if (MCSessionHostEnabled()) return YES;
    NSAlert *a = [NSAlert new];
    a.messageText = @"The session host is off";
    a.informativeText = @"The browser's view shows the tabs the session host holds, and "
                        @"editor.session-host is off. Turn it on? Tabs opened from then on appear "
                        @"on the page; a tab open now appears once it is closed and opened again.";
    [a addButtonWithTitle:@"Turn On"];
    [a addButtonWithTitle:@"Cancel"];
    if ([a runModal] != NSAlertFirstButtonReturn) return NO;
    if (![[AppSettings shared] writeValue:@"on" forKey:@"editor.session-host"]) {
        NSBeep();   // the settings file has unsaved changes in a tab: said in the status bar
        return NO;
    }
    return MCSessionHostEnabled();
}

// The window's server, started now if it is not yet; nil, said in the
// status bar, when it cannot be.
- (MCSessionBrowser *)startedSessionBrowser {
    MCSessionBrowser *b = self.sessionBrowser;
    if (!b) {
        __weak EditorController *weakSelf = self;
        b = [[MCSessionBrowser alloc] initWithWindow:self.window
            folder:self.rootPath
            tabs:^NSArray<NSArray<MCTab *> *> * {
                EditorController *s = weakSelf;
                if (!s) return @[];
                NSArray<MCTab *> *parked = [s parkedTabs];
                return parked.count ? @[ [s groupTabs], parked ] : @[ [s groupTabs] ];
            }
            active:^NSArray<MCTab *> * {
                EditorController *s = weakSelf;
                if (!s) return @[];
                NSMutableArray<MCTab *> *on = [NSMutableArray array];
                [on addObject:s->_activeTab ?: (id)NSNull.null];
                if ([s parkedTabs].count) [on addObject:[s parkedActive] ?: (id)NSNull.null];
                return on;
            }];
        objc_setAssociatedObject(self, &kSessionBrowserKey, b, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    NSString *error = nil;
    if (![b start:&error]) {
        [self flashStatus:[NSString stringWithFormat:@"The session's page could not start: %@", error]];
        return nil;
    }
    return b;
}

- (void)showSessionInBrowser:(id)sender {
    if (![self sessionHostReadyAsking]) return;
    MCSessionBrowser *b = [self startedSessionBrowser];
    if (!b) return;
    NSURL *url = [NSURL URLWithString:b.url];
    if (MCSessionBrowserOpener) MCSessionBrowserOpener(url);
    else [NSWorkspace.sharedWorkspace openURL:url];
    [self flashStatus:@"The session's page opened in the browser; it is read-only and only on this Mac."];
}

- (void)copySessionBrowserLink:(id)sender {
    if (![self sessionHostReadyAsking]) return;
    MCSessionBrowser *b = [self startedSessionBrowser];
    if (!b) return;
    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    [pb clearContents];
    [pb setString:b.url forType:NSPasteboardTypeString];
    [self flashStatus:@"Copied the session's link: it opens only on this Mac, and holds its key."];
}

@end
