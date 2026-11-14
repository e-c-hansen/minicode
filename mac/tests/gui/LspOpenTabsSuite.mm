// LspOpenTabsSuite.mm — `make guitest SUITES=lsp_open_tabs`: every tab's
// file stays open on its server (LspSession syncOpenDocuments), and a tab
// whose text has not changed since the server was given it costs nothing
// to sync again, against the scripted server (lsp.go).
//
// syncOpenDocuments runs on every tab switch, tabsChanged, rename,
// references and workspace-symbol query. It used to convert every open
// tab's text whole and compare it (about 17 ms of the main thread per
// 2.5 MB tab, each time). Now:
//   - tabsChanged with three 1.5 MB tabs open hands none over whole and
//     costs well under a millisecond of the main thread;
//   - a tab switch hands over at most the two tabs involved;
//   - a background tab whose file changed on disk (a clean tab follows the
//     disk) reaches the server with its new text;
//   - typing in the tab on show reaches the server by didChange, and a
//     sync after it hands nothing over whole.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"   // closeTab:prompt:
#import "Lsp.h"

namespace {

LspSession *Session(EditorController *c) { return [c valueForKey:@"lsp"]; }
NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }

NSString *BigGo(NSString *name) {
    NSMutableString *s = [NSMutableString stringWithFormat:@"package %@\n\n", name];
    for (int i = 0; s.length < 1500000; i++)
        [s appendFormat:@"func f%d(x int) int { return x + %d } // é padding\n", i, i];
    return s;
}

// The last text the server was given for a file, whole (didOpen, or a
// didChange with the whole text: the stand-in syncs whole).
NSString *ServerText(NSString *name) {
    NSString *last = nil;
    for (NSDictionary *m in MCGuiFakeLspReceived(@"open-tabs", nil)) {
        NSString *method = m[@"method"];
        NSDictionary *p = m[@"params"];
        if (![p[@"textDocument"][@"uri"] hasSuffix:name]) continue;
        if ([method isEqualToString:@"textDocument/didOpen"]) last = p[@"textDocument"][@"text"];
        if ([method isEqualToString:@"textDocument/didChange"])
            last = [p[@"contentChanges"] lastObject][@"text"];
    }
    return last;
}

}  // namespace

MC_GUI_SUITE(lsp_open_tabs) {
    __block EditorController *c = nil;
    __block NSUInteger before = 0;
    NSString *a = MCGuiProjectFile(@"opentabs/a.go");
    NSString *b = MCGuiProjectFile(@"opentabs/b.go");
    NSString *d = MCGuiProjectFile(@"opentabs/d.go");

    MCGuiStep(@"three 1.5 MB Go files in tabs, the stand-in server for Go", ^{
        c = MCGuiController();
        MCGuiWriteFile(a, BigGo(@"a"));
        MCGuiWriteFile(b, BigGo(@"b"));
        MCGuiWriteFile(d, BigGo(@"d"));
        NSString *cmd = MCGuiFakeLsp(@"open-tabs", @{@"rules": @[]}, nil, nil);
        MCGuiFakeLspClearLog(@"open-tabs");
        MCGuiSetSettings([NSString stringWithFormat:@"lsp.enabled = true\nlsp.go = %@\n", cmd]);
        GCHECK([c showFile:a]);
        GCHECK([c showFile:b]);
        GCHECK([c showFile:d]);
    });
    MCGuiWait(@"all three open on the server", 30, ^BOOL {
        return MCGuiFakeLspReceived(@"open-tabs", @"textDocument/didOpen").count == 3;
    });
    MCGuiStep(@"tabsChanged five times: nothing handed over whole", ^{
        before = Session(c).documentsSentWhole;
        double worst = 0;
        for (int i = 0; i < 5; i++) {
            [Session(c) tabsChanged];
            worst = MAX(worst, Session(c).lastSyncCpuMs);
        }
        MCGuiNote(@"handed over whole: %lu; the slowest sync %.2f ms of CPU",
                  (unsigned long)(Session(c).documentsSentWhole - before), worst);
        GCHECK(Session(c).documentsSentWhole == before);
        GCHECK_TIME(worst, 5, @"a sync of three unchanged 1.5 MB tabs");
    });
    MCGuiStep(@"a tab switch: at most the two tabs involved", ^{
        before = Session(c).documentsSentWhole;
        GCHECK([c showFile:a]);
        const NSUInteger n = Session(c).documentsSentWhole - before;
        MCGuiNote(@"handed over whole: %lu", (unsigned long)n);
        GCHECK(n <= 2);
        before = Session(c).documentsSentWhole;
        [Session(c) tabsChanged];
        GCHECK(Session(c).documentsSentWhole == before);
    });
    MCGuiStep(@"b.go changes on disk while its tab is in the background", ^{
        MCGuiWriteFile(b, [BigGo(@"b") stringByAppendingString:@"// changed outside\n"]);
    });
    MCGuiWait(@"the server has b.go's new text", 20, ^BOOL {
        return [ServerText(@"/b.go") hasSuffix:@"// changed outside\n"];
    });
    MCGuiStep(@"typing in a.go, then a sync", ^{
        NSTextView *tv = Text(c);
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:@"// typed\n" replacementRange:NSMakeRange(0, 0)];
        });
        before = Session(c).documentsSentWhole;
        [Session(c) tabsChanged];   // flushes the edit first
        GCHECK(Session(c).documentsSentWhole == before);
    });
    MCGuiWait(@"the server has what was typed", 20, ^BOOL {
        return [ServerText(@"/a.go") isEqualToString:Text(c).string];
    });
    MCGuiStep(@"tidy up", ^{
        MCGuiSetSettings(@"lsp.enabled = false\n");
        for (NSString *p in @[a, b, d]) {
            GCHECK([c showFile:p]);
            GCHECK([c closeTab:[c valueForKey:@"activeTab"] prompt:NO]);
        }
        [[NSFileManager defaultManager] removeItemAtPath:MCGuiProjectFile(@"opentabs") error:nil];
    });
    MCGuiWait(@"the servers are gone", 10, ^BOOL { return !Session(c).hasServer; });
}
