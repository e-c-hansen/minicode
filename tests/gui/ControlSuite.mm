// ControlSuite.mm — `make guitest SUITES=control`: the control channel
// (ControlServer, ControlCommands, the minicode-cli client), agent notes and
// tours, the agent preview tab and the side-by-side diff, driven through the
// real command line against this process's own socket (--pid), as an agent
// would drive it.
#import "Harness.h"
#import "EditorController.h"
#import "ControlServer.h"
#import "AgentNotes.h"
#import "CompareView.h"
#import "GitPanel.h"
#import "TabStrip.h"
#import "Terminal.h"
#import "TerminalPanel.h"
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static BOOL Env_Verbose() { return NSProcessInfo.processInfo.environment[@"MC_GUI_VERBOSE"].length > 0; }

namespace {

NSString *CLIPath() { return [MCGuiBuildDir() stringByAppendingPathComponent:@"minicode-cli"]; }

// The command line against this process, as a step: `check` gets the exit
// status, stdout and stderr.
void CLI(NSString *name, NSArray<NSString *> *args, void (^check)(int, NSString *, NSString *)) {
    MCGuiAsync(name, 60, ^(MCGuiDone done) {
        NSMutableArray *argv = [NSMutableArray arrayWithObject:CLIPath()];
        [argv addObjectsFromArray:args];
        if (![args.firstObject isEqualToString:@"help"])
            [argv addObjectsFromArray:@[@"--pid", [NSString stringWithFormat:@"%d", getpid()]]];
        MCGuiRunProcess(argv, nil, ^(int status, NSString *out, NSString *err) {
            if (status != 0 && Env_Verbose()) MCGuiNote(@"%@: exit %d, %@", name, status, err);
            check(status, out, err);
            done();
        });
    });
}

}  // namespace

static NSTextView *EditorText(EditorController *c) { return [c valueForKey:@"textView"]; }

static NSArray<NSDictionary *> *Tabs(EditorController *c) { return [c controlTabs]; }

static NSDictionary *ActiveTab(EditorController *c) {
    for (NSDictionary *t in Tabs(c))
        if ([t[@"active"] boolValue]) return t;
    return nil;
}

// The last part of a tab's path ("" for none).
static NSString *PathName(NSDictionary *t) {
    id p = t[@"path"];
    return [p isKindOfClass:NSString.class] ? [p lastPathComponent] : @"";
}

static NSUInteger CountTabsWithPath(EditorController *c, NSString *name) {
    NSUInteger n = 0;
    for (NSDictionary *t in Tabs(c))
        if ([t[@"path"] isKindOfClass:NSString.class] && [PathName(t) isEqualToString:name])
            n++;
    return n;
}

// The UTF-16 range of lines `from` to `to` (1-based, whole lines without the
// last line break).
static NSRange LinesRange(NSString *s, int from, int to) {
    NSUInteger start = 0;
    for (int l = 1; l < from; l++) start = NSMaxRange([s lineRangeForRange:NSMakeRange(start, 0)]);
    NSUInteger end = start;
    for (int l = from; l <= to; l++) end = NSMaxRange([s lineRangeForRange:NSMakeRange(end, 0)]);
    if (end > start && [s characterAtIndex:end - 1] == '\n') end--;
    return NSMakeRange(start, end - start);
}

static NSRect LineRect(NSTextView *tv, NSUInteger index) {
    NSLayoutManager *lm = tv.layoutManager;
    [lm ensureLayoutForCharacterRange:NSMakeRange(0, MIN(index + 1, tv.string.length))];
    NSRect r = [lm lineFragmentRectForGlyphAtIndex:[lm glyphIndexForCharacterAtIndex:index]
                                    effectiveRange:nil];
    NSPoint o = tv.textContainerOrigin;
    return NSOffsetRect(r, o.x, o.y);
}

static id ParseJSON(NSString *s) {
    NSData *d = [s dataUsingEncoding:NSUTF8StringEncoding];
    return d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
}

// Every menu item with a key equivalent, as "mask:key" -> titles.
static void CollectKeys(NSMenu *menu, NSMutableDictionary<NSString *, NSMutableArray *> *out) {
    for (NSMenuItem *item in menu.itemArray) {
        if (item.submenu) CollectKeys(item.submenu, out);
        if (!item.keyEquivalent.length || item.isAlternate) continue;
        // AppKit's own Edit items: once a Control+Command key has reached the
        // menu it shows them in their Globe-key form (fn E), beside a hidden
        // twin with the same key. Not minicode's keys to clash.
        if (item.action == @selector(orderFrontCharacterPalette:) ||
            item.action == @selector(startDictation:))
            continue;
        NSEventModifierFlags m = item.keyEquivalentModifierMask &
            (NSEventModifierFlagCommand | NSEventModifierFlagShift | NSEventModifierFlagOption |
             NSEventModifierFlagControl);
        NSString *key = item.keyEquivalent;
        if (![key.lowercaseString isEqualToString:key]) {   // "S" means Shift+S
            m |= NSEventModifierFlagShift;
            key = key.lowercaseString;
        }
        NSString *k = [NSString stringWithFormat:@"%lu:%@", (unsigned long)m, key];
        if (!out[k]) out[k] = [NSMutableArray array];
        [out[k] addObject:item.title];
    }
}

static NSMenuItem *FindItem(NSMenu *menu, NSString *title) {
    for (NSMenuItem *item in menu.itemArray) {
        if ([item.title isEqualToString:title]) return item;
        if (item.submenu)
            if (NSMenuItem *found = FindItem(item.submenu, title)) return found;
    }
    return nil;
}

MC_GUI_SUITE(control) {
    __block EditorController *c = nil;
    __block NSTextView *tv = nil;
    __block NSString *socketPath = nil;
    NSString *pid = [NSString stringWithFormat:@"%d", getpid()];
    NSFileManager *fm = [NSFileManager defaultManager];

    // ------------------------------------------------------------ the socket
    MCGuiStep(@"the socket is in place, private, and the stale one is gone", ^{
        c = MCGuiController();
        tv = EditorText(c);
        GCHECK(c != nil && tv != nil);
        socketPath = MCControlSocketPath();
        GCHECK(socketPath != nil);
        NSString *scratchT = [[MCGuiScratch() stringByAppendingPathComponent:@"t"]
                                 stringByResolvingSymlinksInPath];
        GCHECK([socketPath.stringByResolvingSymlinksInPath hasPrefix:scratchT]);
        GCHECK_EQ_STR(socketPath.lastPathComponent, ([NSString stringWithFormat:@"%@.sock", pid]));
        struct stat st;
        GCHECK(lstat(socketPath.fileSystemRepresentation, &st) == 0 && S_ISSOCK(st.st_mode));
        GCHECK((st.st_mode & 0777) == 0600 && st.st_uid == geteuid());
        struct stat dir;
        GCHECK(lstat(socketPath.stringByDeletingLastPathComponent.fileSystemRepresentation, &dir) == 0 &&
               S_ISDIR(dir.st_mode) && (dir.st_mode & 0777) == 0700);
        NSString *stale = NSProcessInfo.processInfo.environment[@"MC_GUI_STALE_SOCKET"];
        GCHECK(stale.length && ![fm fileExistsAtPath:stale]);
        // The cleanup itself: a dead process's socket goes, a live one's
        // and anything else stay.
        NSString *folder = socketPath.stringByDeletingLastPathComponent;
        pid_t dead = fork();
        if (dead == 0) _exit(0);
        waitpid(dead, nullptr, 0);
        NSString *deadSock = [folder stringByAppendingFormat:@"/%d.sock", dead];
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr = {};
        addr.sun_family = AF_UNIX;
        strlcpy(addr.sun_path, deadSock.fileSystemRepresentation, sizeof addr.sun_path);
        bind(fd, (struct sockaddr *)&addr, sizeof addr);
        close(fd);
        NSString *other = [folder stringByAppendingPathComponent:@"notes.txt"];
        MCGuiWriteFile(other, @"x");
        GCHECK(MCControlRemoveStaleSockets(folder) == 1);
        GCHECK(![fm fileExistsAtPath:deadSock] && [fm fileExistsAtPath:socketPath] &&
               [fm fileExistsAtPath:other]);
        [fm removeItemAtPath:other error:nil];
    });

    MCGuiAsync(@"garbage on the socket gets error answers, in order, and the server lives", 20,
               ^(MCGuiDone done) {
        NSString *path = socketPath;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            struct sockaddr_un addr = {};
            addr.sun_family = AF_UNIX;
            strlcpy(addr.sun_path, path.fileSystemRepresentation, sizeof addr.sun_path);
            struct timeval tv5 = {10, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv5, sizeof tv5);
            BOOL connected = connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0;
            const char *lines = "garbage\n"
                                "{\"v\":1,\"id\":2,\"cmd\":\"rm -rf\"}\n"
                                "{\"v\":1,\"id\":3,\"cmd\":\"state\",\"extra\":true}\n"
                                "\n"
                                "{\"v\":1,\"id\":4,\"cmd\":\"ping\"}\n";
            if (connected) write(fd, lines, strlen(lines));
            NSMutableString *got = [NSMutableString string];
            char buf[4096];
            while (connected) {
                ssize_t n = read(fd, buf, sizeof buf);
                if (n <= 0) break;
                [got appendString:[[NSString alloc] initWithBytes:buf length:(NSUInteger)n
                                                         encoding:NSUTF8StringEncoding] ?: @""];
                if ([got componentsSeparatedByString:@"\n"].count > 4) break;
            }
            close(fd);
            dispatch_async(dispatch_get_main_queue(), ^{
                GCHECK(connected);
                NSArray<NSString *> *answers = [got componentsSeparatedByString:@"\n"];
                GCHECK(answers.count >= 4);
                if (answers.count >= 4) {
                    NSDictionary *a1 = ParseJSON(answers[0]), *a2 = ParseJSON(answers[1]),
                                 *a3 = ParseJSON(answers[2]), *a4 = ParseJSON(answers[3]);
                    GCHECK([a1[@"ok"] isEqual:@NO] && [a1[@"error"] hasPrefix:@"not JSON"]);
                    GCHECK([a2[@"id"] isEqual:@2] && [a2[@"error"] containsString:@"unknown command"]);
                    GCHECK([a3[@"id"] isEqual:@3] && [a3[@"error"] containsString:@"unknown field"]);
                    GCHECK([a4[@"id"] isEqual:@4] && [a4[@"ok"] isEqual:@YES] &&
                           [a4[@"result"][@"pid"] intValue] == getpid());
                }
                done();
            });
        });
    });

    MCGuiStep(@"commands answer well inside 100 ms", ^{
        for (NSString *line in @[@"{\"v\":1,\"id\":1,\"cmd\":\"ping\"}",
                                 @"{\"v\":1,\"id\":2,\"cmd\":\"state\"}",
                                 @"{\"v\":1,\"id\":3,\"cmd\":\"selection\"}"]) {
            __block NSString *answer = nil;
            NSDate *t0 = [NSDate date];
            MCControlHandleLine(line, ^(NSString *a) { answer = a; });
            NSTimeInterval took = -[t0 timeIntervalSinceNow];
            GCHECK(answer != nil && [answer containsString:@"\"ok\":true"]);
            GCHECK(took < 0.1);
        }
    });

    __block uid_t me = getuid();
    MCGuiStep(@"a peer of another user is turned away", ^{
        MCControlServerSetPeerUIDForTesting(me + 1);
    });
    CLI(@"state as another user", @[@"state"], ^(int s, NSString *out, NSString *err) {
        GCHECK(s == 3);
        GCHECK(out.length == 0);
        GCHECK([err containsString:@"without answering"]);
    });
    MCGuiStep(@"back to this user", ^{ MCControlServerSetPeerUIDForTesting(me); });
    CLI(@"state as this user", @[@"state"], ^(int s, NSString *out, NSString *) {
        GCHECK(s == 0 && [ParseJSON(out)[@"pid"] intValue] == getpid());
    });

    MCGuiAsync(@"with no minicode running, the command line starts one and waits", 30,
               ^(MCGuiDone done) {
        NSString *empty = [MCGuiScratch() stringByAppendingPathComponent:@"no-app"];
        [fm createDirectoryAtPath:empty withIntermediateDirectories:YES attributes:nil error:nil];
        MCGuiRunProcess(@[CLIPath(), @"browse", @"https://example.com"],
                        @{@"TMPDIR": empty, @"MINICODE_LAUNCHER_DRYRUN": @"1",
                          // Only there: the command line also looks in the
                          // user's own temporary folder, where a real
                          // minicode may be listening.
                          @"MINICODE_CONTROL_DIR": [empty stringByAppendingPathComponent:@"MiniCode"],
                          @"MINICODE_CLI_WAIT": @"0.4"},
                        ^(int s, NSString *out, NSString *err) {
            GCHECK(s == 3);
            if ([err containsString:@"does not answer"]) {
                // An older minicode is open on this Mac: it is told so, and
                // nothing is launched.
                GCHECK(out.length == 0);
            } else {
                GCHECK([out hasPrefix:@"open -a "] &&
                       [out containsString:MCGuiProject().lastPathComponent]);
                GCHECK([err containsString:@"did not start"]);
            }
            done();
        });
    });

    MCGuiAsync(@"the launcher in the bundle hands subcommands to the client", 30, ^(MCGuiDone done) {
        NSString *launcher = [[MCGuiBuildDir().stringByDeletingLastPathComponent
            stringByAppendingPathComponent:@"MiniCode.app"]
            stringByAppendingPathComponent:@"Contents/Resources/minicode"];
        if (![fm isExecutableFileAtPath:launcher]) {
            MCGuiNote(@"no MiniCode.app built here (make app): launcher check skipped");
            done();
            return;
        }
        MCGuiRunProcess(@[launcher, @"state", @"--pid", pid], nil, ^(int s, NSString *out, NSString *) {
            GCHECK(s == 0 && [ParseJSON(out)[@"pid"] intValue] == getpid());
            done();
        });
    });

    // ---------------------------------------------------- usage and errors
    CLI(@"usage error", @[@"open"], ^(int s, NSString *, NSString *err) {
        GCHECK(s == 2 && [err containsString:@"open needs a path"]);
    });
    CLI(@"state takes no --window", @[@"state", @"--window", @"1"], ^(int s, NSString *, NSString *) {
        GCHECK(s == 2);
    });
    CLI(@"help", @[@"help"], ^(int s, NSString *out, NSString *) {
        GCHECK(s == 0 && [out containsString:@"term-output"] && [out containsString:@"Exit codes"]);
    });
    CLI(@"a missing file", @[@"open", @"nowhere.txt"], ^(int s, NSString *, NSString *err) {
        GCHECK(s == 1 && [err containsString:@"no such file"]);
    });
    CLI(@"a missing window", @[@"browse", @"about:blank", @"--window", @"999"],
        ^(int s, NSString *, NSString *err) {
        GCHECK(s == 1 && [err containsString:@"no window 999"]);
    });
    CLI(@"a missing terminal", @[@"term-output", @"999"], ^(int s, NSString *, NSString *err) {
        GCHECK(s == 1 && [err containsString:@"no terminal 999"]);
    });
    CLI(@"open in a split is refused", @[@"split", @"--right", @"open", @"big.txt"],
        ^(int s, NSString *, NSString *err) {
        GCHECK(s == 1 && [err containsString:@"one pane"]);
    });

    // ------------------------------------------------- open, at a line
    __block NSResponder *keyboard = nil;
    MCGuiStep(@"the keyboard is in the tree before the agent acts", ^{
        [c.window makeFirstResponder:[c valueForKey:@"outline"]];
        keyboard = c.window.firstResponder;
        GCHECK(keyboard == [c valueForKey:@"outline"]);
    });
    CLI(@"open big.txt:100", @[@"open", @"big.txt:100"], ^(int s, NSString *, NSString *) {
        GCHECK(s == 0);
        NSDictionary *t = ActiveTab(c);
        GCHECK([PathName(t) isEqualToString:@"big.txt"]);
        GCHECK([t[@"preview"] isEqual:@NO]);
        NSRange line100 = LinesRange(tv.string, 100, 100);
        GCHECK(tv.selectedRange.location == line100.location);
        // Scrolled so line 100 is about a third of the way down.
        NSClipView *clip = tv.enclosingScrollView.contentView;
        CGFloat offset = NSMinY(LineRect(tv, line100.location)) - NSMinY(clip.bounds);
        CGFloat H = NSHeight(clip.bounds);
        if (!GCHECK(H > 100 && offset > H * 0.2 && offset < H * 0.45))
            MCGuiNote(@"line 100 is %.0f points down a pane %.0f tall", offset, H);
        GCHECK(c.window.firstResponder == keyboard);
    });

    // --------------------------------------------- the terminal (acceptance)
    __block NSString *firstTerm = nil;
    MCGuiStep(@"the keyboard is in the editor", ^{
        [c.window makeFirstResponder:tv];
        keyboard = tv;
        GCHECK([c controlTerminals].count == 0);
    });
    CLI(@"term --run types the command and runs it",
        @[@"term", @"--title", @"Tests", @"--run", @"echo hel\"\"lo-$((6*7))"],
        ^(int s, NSString *out, NSString *) {
        GCHECK(s == 0);
        firstTerm = [out stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        GCHECK(firstTerm.integerValue > 0);
        NSArray<TerminalView *> *terms = [c controlTerminals];
        GCHECK(terms.count == 1);
        GCHECK_EQ_STR(terms.firstObject.title, @"Tests");
        GCHECK(c.window.firstResponder == keyboard);   // typing in the editor goes on
    });
    MCGuiWait(@"the command runs in the terminal", 30, ^BOOL {
        return [[[c controlTerminals].firstObject recentText:50] containsString:@"hello-42"];
    });
    MCGuiAsync(@"term-output reads the terminal back", 30, ^(MCGuiDone done) {
        MCGuiRunProcess(@[CLIPath(), @"term-output", firstTerm, @"--lines", @"30", @"--pid", pid], nil,
                        ^(int s, NSString *out, NSString *) {
            GCHECK(s == 0 && [out containsString:@"hello-42"]);
            done();
        });
    });
    __block NSString *srcDir = nil;
    MCGuiStep(@"a folder for --cwd", ^{
        srcDir = MCGuiProjectFile(@"src");
        MCGuiWriteFile([srcDir stringByAppendingPathComponent:@"a.py"],
                       @"def a():\n    return 1\n\n\ndef b():\n    return 2\n");
    });
    MCGuiAsync(@"term --cwd starts the shell there", 30, ^(MCGuiDone done) {
        MCGuiRunProcess(@[CLIPath(), @"term", @"--cwd", @"src", @"--pid", pid], nil,
                        ^(int s, NSString *, NSString *) {
            GCHECK(s == 0 && [c controlTerminals].count == 2);
            GCHECK(c.window.firstResponder == keyboard);
            done();
        });
    });
    MCGuiWait(@"the second shell is in src", 30, ^BOOL {
        TerminalView *t = [c controlTerminals].lastObject;
        return [t.directory.stringByResolvingSymlinksInPath
                   isEqualToString:srcDir.stringByResolvingSymlinksInPath];
    });
    CLI(@"split --right term splits the active terminal", @[@"split", @"--right", @"term"],
        ^(int s, NSString *, NSString *) {
        GCHECK(s == 0);
        MCTerminalPanel *panel = [c valueForKey:@"terminalPanel"];
        GCHECK(panel.tabCount == 2 && [panel terminalsInTab:(NSUInteger)panel.selectedTab].count == 2);
        GCHECK([c controlTerminals].count == 3);
    });

    // ------------------------------------------------------------- state
    MCGuiStep(@"a selection in the editor", ^{
        [c openFileForAgent:MCGuiProjectFile(@"big.txt") preview:NO];
        NSRange r = LinesRange(tv.string, 7, 7);
        tv.selectedRange = r;
    });
    CLI(@"selection prints the selected text", @[@"selection"], ^(int s, NSString *out, NSString *) {
        GCHECK(s == 0);
        GCHECK_EQ_STR(out, @"line 7 of big.txt");
    });
    CLI(@"state lists windows, tabs, the selection and the terminals", @[@"state"],
        ^(int s, NSString *out, NSString *) {
        GCHECK(s == 0 && ![out containsString:@"\n  "]);
        NSDictionary *st = ParseJSON(out);
        NSArray *windows = st[@"windows"];
        GCHECK(windows.count == 1);
        NSDictionary *w = windows.firstObject;
        GCHECK([w[@"front"] isEqual:@YES] && [w[@"id"] intValue] > 0);
        GCHECK([[w[@"folder"] stringByResolvingSymlinksInPath]
                   isEqualToString:MCGuiProject().stringByResolvingSymlinksInPath]);
        NSArray *tabs = w[@"tabs"];
        GCHECK(tabs.count >= 1);
        BOOL sawBig = NO;
        for (NSDictionary *t in tabs)
            if ([[t[@"path"] description] hasSuffix:@"big.txt"]) {
                sawBig = YES;
                GCHECK([t[@"active"] isEqual:@YES] && [t[@"dirty"] isEqual:@NO]);
            }
        GCHECK(sawBig);
        NSDictionary *sel = w[@"selection"];
        GCHECK([sel[@"text"] isEqualToString:@"line 7 of big.txt"]);
        GCHECK([sel[@"start"][@"line"] intValue] == 7 && [sel[@"start"][@"column"] intValue] == 1);
        GCHECK([sel[@"end"][@"line"] intValue] == 7 && [sel[@"end"][@"column"] intValue] == 18);
        NSArray *terms = w[@"terminals"];
        GCHECK(terms.count == 3);
        GCHECK([[terms.firstObject[@"id"] description] isEqualToString:firstTerm]);
        GCHECK([terms.firstObject[@"title"] isEqualToString:@"Tests"]);
        GCHECK(terms.firstObject[@"cwd"] != nil && terms.firstObject[@"busy"] != nil);
    });
    CLI(@"state --pretty", @[@"state", @"--pretty"], ^(int s, NSString *out, NSString *) {
        GCHECK(s == 0 && [out containsString:@"\n  \"windows\": ["] && ParseJSON(out) != nil);
    });

    // ------------------------------------------------------------ browse
    MCGuiStep(@"the keyboard is in the editor again; a local page", ^{
        [c.window makeFirstResponder:tv];
        keyboard = tv;
        MCGuiWriteFile(MCGuiProjectFile(@"page.html"),
                       @"<html><head><title>Local page</title></head><body>hi</body></html>");
    });
    CLI(@"browse opens the address beside the editor",
        @[@"browse", [NSURL fileURLWithPath:MCGuiProjectFile(@"page.html")].absoluteString],
        ^(int s, NSString *, NSString *) {
        GCHECK(s == 0);
        GCHECK([[c valueForKey:@"browserVisible"] boolValue]);
        GCHECK(c.window.firstResponder == keyboard);
    });
    MCGuiWait(@"the page loads", 20, ^BOOL {
        return [[c controlBrowser][@"title"] isEqualToString:@"Local page"];
    });
    CLI(@"the browser is never below", @[@"split", @"--below", @"browse", @"localhost:9"],
        ^(int s, NSString *, NSString *err) {
        GCHECK(s == 1 && [err containsString:@"--right"]);
    });

    // -------------------------------------------- notes and the preview tab
    __block NSUInteger tabsBefore = 0;
    __block MCAgentNotes *notes = nil;
    MCGuiStep(@"before the notes", ^{
        notes = c.agentNotes;
        [c.window makeFirstResponder:[c valueForKey:@"outline"]];
        keyboard = c.window.firstResponder;
        // big.txt is open as an ordinary tab; close it so the note opens it
        // in the preview tab.
        tabsBefore = Tabs(c).count;
    });
    CLI(@"open --note puts the file in the preview tab with the note",
        @[@"open", @"src/a.py:5", @"--to", @"6", @"--note", @"Applies rename edits\nto every file"],
        ^(int s, NSString *, NSString *) {
        GCHECK(s == 0);
        GCHECK(Tabs(c).count == tabsBefore + 1);
        NSDictionary *t = ActiveTab(c);
        GCHECK([PathName(t) isEqualToString:@"a.py"] && [t[@"preview"] isEqual:@YES]);
        GCHECK(notes.noteShown);
        GCHECK_EQ_STR(notes.noteText, @"Applies rename edits to every file");
        GCHECK_EQ_STR(notes.noteLabel, @"Claude");
        NSRange want = LinesRange(tv.string, 5, 6);
        GCHECK(NSEqualRanges(notes.noteRange, want));
        // Above its lines, inside the text view, and never the keyboard's.
        NSView *v = notes.noteView;
        GCHECK(v != nil && v.superview == tv);
        GCHECK(NSMaxY(v.frame) <= NSMinY(LineRect(tv, want.location)) + 0.5);
        GCHECK(!v.acceptsFirstResponder);
        GCHECK(c.window.firstResponder == keyboard);
        // The highlight is a layout-manager attribute, not in the text.
        NSRange eff;
        id bg = [tv.layoutManager temporaryAttribute:NSBackgroundColorAttributeName
                                    atCharacterIndex:want.location effectiveRange:&eff];
        GCHECK(bg != nil);
        GCHECK([tv.textStorage attribute:NSBackgroundColorAttributeName atIndex:want.location
                          effectiveRange:nil] == nil);
    });
    CLI(@"another note replaces the preview tab's file; the tab count stays",
        @[@"open", @"big.txt:300", @"--to", @"310", @"--note", @"Second", @"--from", @"Reviewer"],
        ^(int s, NSString *, NSString *) {
        GCHECK(s == 0);
        // big.txt was open already as an ordinary tab: it is shown there,
        // and the preview tab keeps a.py.
        GCHECK(Tabs(c).count == tabsBefore + 1);
        GCHECK_EQ_STR(notes.noteLabel, @"Reviewer");
        GCHECK([PathName(ActiveTab(c)) isEqualToString:@"big.txt"]);
        // Its first line a third of the way down, the note above it.
        NSRange want = LinesRange(tv.string, 300, 310);
        GCHECK(NSEqualRanges(notes.noteRange, want));
        NSClipView *clip = tv.enclosingScrollView.contentView;
        const CGFloat top = NSMinY(LineRect(tv, want.location)) - NSMinY(clip.bounds);
        const CGFloat H = NSHeight(clip.bounds);
        if (!GCHECK(top > H * 0.25 && top < H * 0.45))
            MCGuiNote(@"line 300 is %.0f points down a pane %.0f tall", top, H);
        NSView *v = notes.noteView;
        GCHECK(v && NSMinY(v.frame) >= NSMinY(clip.bounds) && NSMaxY(v.frame) <= NSMinY(LineRect(tv, want.location)) + 0.5);
        MCGuiSnapshot(c.window.contentView, @"control-note");
    });
    MCGuiStep(@"a file of its own for the preview tab", ^{
        MCGuiWriteFile([srcDir stringByAppendingPathComponent:@"b.py"], @"x = 1\ny = 2\nz = 3\n");
    });
    CLI(@"a note on another file replaces a.py in the preview tab",
        @[@"open", @"src/b.py:2", @"--note", @"Second file"], ^(int s, NSString *, NSString *) {
        GCHECK(s == 0);
        GCHECK(Tabs(c).count == tabsBefore + 1);
        GCHECK(CountTabsWithPath(c, @"a.py") == 0 && CountTabsWithPath(c, @"b.py") == 1);
        GCHECK([ActiveTab(c)[@"preview"] isEqual:@YES]);
        GCHECK(NSEqualRanges(notes.noteRange, LinesRange(tv.string, 2, 2)));
        // The folder's session never lists the preview tab.
        GCHECK(c.window.firstResponder == keyboard);
    });
    MCGuiStep(@"an edit before the note moves it along", ^{
        NSRange before = notes.noteRange;
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:@"# top\n" replacementRange:NSMakeRange(0, 0)];
        });
        GCHECK(notes.noteShown);
        GCHECK(notes.noteRange.location == before.location + 6 && notes.noteRange.length == before.length);
        // An edit makes the preview tab an ordinary one.
        GCHECK([ActiveTab(c)[@"preview"] isEqual:@NO]);
        [tv.undoManager undo];
        GCHECK(notes.noteShown && notes.noteRange.location == before.location);
    });
    MCGuiStep(@"typing inside the note's lines closes it, and undo does not bring it back", ^{
        NSRange r = notes.noteRange;
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:@"Q" replacementRange:NSMakeRange(r.location + 1, 0)];
        });
        GCHECK(!notes.noteShown);
        GCHECK(notes.noteView == nil);
        [tv.undoManager undo];
        GCHECK(!notes.noteShown);
        GCHECK([tv.string isEqualToString:@"x = 1\ny = 2\nz = 3\n"]);
    });
    CLI(@"a third file: a new preview tab, since b.py became the user's",
        @[@"open", @"README.md:1", @"--note", @"Read me first"], ^(int s, NSString *, NSString *) {
        GCHECK(s == 0);
        GCHECK(CountTabsWithPath(c, @"b.py") == 1 && CountTabsWithPath(c, @"README.md") == 1);
        GCHECK([ActiveTab(c)[@"preview"] isEqual:@YES]);
        // A Markdown file shows its source for a note, which sits under the
        // first line when there is no room above it.
        GCHECK(tv.editable && [tv.string hasPrefix:@"# Project"]);
        NSView *v = notes.noteView;
        GCHECK(v && NSMinY(v.frame) >= NSMaxY(LineRect(tv, 0)) - 0.5);
    });
    MCGuiStep(@"Escape closes the note", ^{
        [tv doCommandBySelector:@selector(cancelOperation:)];
        GCHECK(!notes.noteShown);
    });
    CLI(@"note again", @[@"open", @"README.md:3", @"--note", @"Again"], ^(int s, NSString *, NSString *) {
        GCHECK(s == 0 && notes.noteShown);
    });
    MCGuiStep(@"its x closes it", ^{
        NSButton *x = [notes.noteView valueForKey:@"closeButton"];
        GCHECK(x != nil && x.refusesFirstResponder);
        [x performClick:nil];
        GCHECK(!notes.noteShown);
    });
    MCGuiStep(@"a double-click on the preview tab's title keeps it", ^{
        MCTabStrip *strip = [c valueForKey:@"tabStrip"];
        NSInteger i = strip.activeIndex;
        GCHECK(i >= 0 && strip.items[(NSUInteger)i].preview);
        NSRect r = [strip rectForTab:i];
        NSPoint p = [strip convertPoint:NSMakePoint(NSMidX(r) - 10, NSMidY(r)) toView:nil];
        NSEvent *e = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:p
                                   modifierFlags:0 timestamp:0 windowNumber:c.window.windowNumber
                                         context:nil eventNumber:0 clickCount:2 pressure:1];
        [strip mouseDown:e];
        GCHECK([ActiveTab(c)[@"preview"] isEqual:@NO]);
        GCHECK(!strip.items[(NSUInteger)strip.activeIndex].preview);
        // The click gave the editor the keyboard; the agent's next files
        // would wait beside it (AgentCommandsSuite), so it goes back to
        // the tree for the checks below.
        [c.window makeFirstResponder:[c valueForKey:@"outline"]];
    });
    CLI(@"--keep opens an ordinary tab, note and all",
        @[@"open", @"src/a.py:2", @"--note", @"Kept", @"--keep"], ^(int s, NSString *, NSString *) {
        GCHECK(s == 0 && notes.noteShown);
        GCHECK([ActiveTab(c)[@"preview"] isEqual:@NO]);
    });
    CLI(@"a long note is cut to 500 characters", @[@"open", @"src/a.py:1", @"--note",
                        [@"" stringByPaddingToLength:700 withString:@"word " startingAtIndex:0]],
        ^(int s, NSString *, NSString *) {
        GCHECK(s == 0 && notes.noteText.length == 500 && [notes.noteText hasSuffix:@"…"]);
    });

    // ------------------------------------------------------------- tours
    MCGuiStep(@"a tour file", ^{
        [tv doCommandBySelector:@selector(cancelOperation:)];
        NSString *json = @"[{\"path\":\"big.txt\",\"line\":10,\"note\":\"Stop one\"},"
                          "{\"path\":\"big.txt\",\"line\":50,\"to\":55,\"note\":\"Stop two\"},"
                          "{\"path\":\"src/b.py\",\"line\":3,\"note\":\"Stop three\"}]";
        MCGuiWriteFile(MCGuiProjectFile(@"tour.json"), json);
        NSMenuItem *next = FindItem(NSApp.mainMenu, @"Next Tour Stop");
        GCHECK(next != nil && ![(id)NSApp.delegate validateMenuItem:next]);
    });
    CLI(@"tour opens the first stop", @[@"tour", @"tour.json"], ^(int s, NSString *, NSString *) {
        GCHECK(s == 0);
        GCHECK(notes.tourActive && notes.tourIndex == 0 && notes.tourCount == 3);
        GCHECK_EQ_STR(notes.noteLabel, @"Claude · 1 of 3");
        GCHECK_EQ_STR(notes.noteText, @"Stop one");
        GCHECK(NSEqualRanges(notes.noteRange, LinesRange(tv.string, 10, 10)));
        NSMenuItem *next = FindItem(NSApp.mainMenu, @"Next Tour Stop");
        GCHECK([(id)NSApp.delegate validateMenuItem:next]);
    });
    MCGuiStep(@"Option+] and Option+[ step through it", ^{
        MCGuiSendKeyToApp(c.window, 30, NSEventModifierFlagOption, @"‘", @"]");
        GCHECK_EQ_STR(notes.noteLabel, @"Claude · 2 of 3");
        GCHECK(NSEqualRanges(notes.noteRange, LinesRange(tv.string, 50, 55)));
        MCGuiSnapshot(c.window.contentView, @"control-tour");
        MCGuiSendKeyToApp(c.window, 30, NSEventModifierFlagOption, @"‘", @"]");
        GCHECK_EQ_STR(notes.noteLabel, @"Claude · 3 of 3");
        GCHECK([PathName(ActiveTab(c)) isEqualToString:@"b.py"]);
        MCGuiSendKeyToApp(c.window, 30, NSEventModifierFlagOption, @"‘", @"]");   // the end
        GCHECK_EQ_STR(notes.noteLabel, @"Claude · 3 of 3");
        MCGuiSendKeyToApp(c.window, 33, NSEventModifierFlagOption, @"“", @"[");
        GCHECK_EQ_STR(notes.noteLabel, @"Claude · 2 of 3");
        GCHECK([PathName(ActiveTab(c)) isEqualToString:@"big.txt"]);
    });
    CLI(@"state shows the tour", @[@"state"], ^(int s, NSString *out, NSString *) {
        NSDictionary *w = [ParseJSON(out)[@"windows"] firstObject];
        GCHECK(s == 0 && [w[@"tour"][@"stop"] intValue] == 2 && [w[@"tour"][@"of"] intValue] == 3);
        GCHECK([w[@"note"][@"text"] isEqualToString:@"Stop two"]);
    });
    MCGuiStep(@"closing its note ends the tour", ^{
        [notes closeNote];
        GCHECK(!notes.tourActive);
    });

    // --------------------------------------------------- side-by-side diff
    __block NSString *git = nil;
    MCGuiStep(@"a git repository", ^{ git = MCGitExecutable(); if (!git) MCGuiNote(@"no git: skipping the diff checks"); });
    MCGuiAsync(@"git init and a commit", 60, ^(MCGuiDone done) {
        if (!git) { done(); return; }
        MCGuiRunProcess(@[@"/bin/sh", @"-c",
                          [NSString stringWithFormat:@"'%@' init -q && '%@' add -A && '%@' commit -qm one",
                           git, git, git]], nil, ^(int s, NSString *, NSString *err) {
            GCHECK(s == 0);
            if (s) MCGuiNote(@"git: %@", err);
            done();
        });
    });
    MCGuiStep(@"a change to the committed file", ^{
        MCGuiWriteFile([srcDir stringByAppendingPathComponent:@"a.py"],
                       @"def a():\n    return 10\n\n\ndef b():\n    return 2\n\ndef c():\n    return 3\n");
        MCGuiWriteFile([srcDir stringByAppendingPathComponent:@"new.py"], @"fresh = True\n");
        MCGuiWriteFile([MCGuiScratch() stringByAppendingPathComponent:@"outside.txt"], @"o\n");
        tabsBefore = Tabs(c).count;
    });
    CLI(@"diff against HEAD, side by side", @[@"diff", @"src/a.py"], ^(int s, NSString *, NSString *err) {
        if (!git) return;
        GCHECK(s == 0);
        if (s) MCGuiNote(@"%@", err);
        MCCompareView *cv = [c valueForKey:@"compareView"];
        GCHECK(cv != nil && !cv.hidden);
        NSDictionary *t = ActiveTab(c);
        GCHECK([t[@"kind"] isEqualToString:@"diff"] && [t[@"preview"] isEqual:@YES]);
        GCHECK([t[@"title"] containsString:@"HEAD ↔ working copy"]);
        NSArray *left = [cv.leftView.string componentsSeparatedByString:@"\n"];
        NSArray *right = [cv.rightView.string componentsSeparatedByString:@"\n"];
        GCHECK(left.count == right.count && left.count == 9);
        GCHECK([left[1] isEqualToString:@"    return 1"] && [right[1] isEqualToString:@"    return 10"]);
        GCHECK([left[7] isEqualToString:@""] && [right[7] isEqualToString:@"def c():"]);
        GCHECK(cv.comparison.changedRows == 4);
        GCHECK(cv.leftView.textContainerInset.width > 20);   // line numbers
        MCGuiSnapshot(c.window.contentView, @"control-diff");
    });
    MCGuiStep(@"the two panes scroll together", ^{
        if (!git) return;
        MCCompareView *cv = [c valueForKey:@"compareView"];
        NSMutableString *many = [NSMutableString string];
        for (int i = 0; i < 300; i++) [many appendFormat:@"row %d\n", i];
        MCComparison *big = [MCComparison new];
        big.leftText = many;
        big.rightText = [many stringByAppendingString:@"extra\n"];
        big.title = @"big";
        [c showComparison:big preview:YES];
        [cv.leftScroll.contentView scrollToPoint:NSMakePoint(0, 900)];
        [cv.leftScroll reflectScrolledClipView:cv.leftScroll.contentView];
        GCHECK(fabs(cv.rightScroll.contentView.bounds.origin.y - 900) < 0.5);
        GCHECK(Tabs(c).count == tabsBefore + 1);   // it replaced the diff
    });
    CLI(@"a file git does not know yet", @[@"diff", @"src/new.py"], ^(int s, NSString *, NSString *) {
        if (!git) return;
        MCCompareView *cv = [c valueForKey:@"compareView"];
        GCHECK(s == 0 && [cv.comparison.leftLabel containsString:@"not in HEAD"]);
        GCHECK([cv.rightView.string isEqualToString:@"fresh = True"]);
    });
    CLI(@"no such commit", @[@"diff", @"src/a.py", @"--base", @"nope"], ^(int s, NSString *, NSString *err) {
        if (!git) return;
        GCHECK(s == 1 && [err containsString:@"no commit named nope"]);
    });
    CLI(@"outside a repository", @[@"diff", @"../outside.txt"], ^(int s, NSString *, NSString *err) {
        GCHECK(s == 1 && ([err containsString:@"not in a git repository"] ||
                          [err containsString:@"git is not installed"]));
    });
    CLI(@"two files", @[@"diff", @"src/a.py", @"src/b.py", @"--keep"], ^(int s, NSString *, NSString *) {
        GCHECK(s == 0);
        NSDictionary *t = ActiveTab(c);
        GCHECK([t[@"title"] isEqualToString:@"a.py ↔ b.py"] && [t[@"preview"] isEqual:@NO]);
    });

    // ------------------------------------------------------------- menus
    MCGuiStep(@"no two menu items share a key; Option+] is the tour's alone", ^{
        NSMutableDictionary<NSString *, NSMutableArray *> *keys = [NSMutableDictionary dictionary];
        CollectKeys(NSApp.mainMenu, keys);
        for (NSString *k in keys)
            if (keys[k].count > 1) {
                MCGuiNote(@"shared key %@: %@", k, [keys[k] componentsJoinedByString:@", "]);
                GCHECK(keys[k].count == 1);
            }
        NSString *optBracket = [NSString stringWithFormat:@"%lu:]", (unsigned long)NSEventModifierFlagOption];
        GCHECK([keys[optBracket] isEqualToArray:@[@"Next Tour Stop"]]);
    });

    // ------------------------------------------------------------ windows
    __block NSString *firstID = nil;
    CLI(@"window prints the front window's id", @[@"window"], ^(int s, NSString *out, NSString *) {
        firstID = [out stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        GCHECK(s == 0 && firstID.integerValue > 0);
    });
    MCGuiStep(@"another folder", ^{
        [fm createDirectoryAtPath:[MCGuiScratch() stringByAppendingPathComponent:@"other"]
      withIntermediateDirectories:YES attributes:nil error:nil];
    });
    __block NSString *secondID = nil;
    CLI(@"window --folder opens a window on it", @[@"window", @"--folder", @"../other"],
        ^(int s, NSString *out, NSString *) {
        secondID = [out stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        GCHECK(s == 0 && MCGuiControllers().count == 2 && ![secondID isEqualToString:firstID]);
    });
    CLI(@"and finds it again", @[@"window", @"--folder", @"../other"], ^(int s, NSString *out, NSString *) {
        GCHECK(s == 0 && MCGuiControllers().count == 2);
        GCHECK([[out stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
                   isEqualToString:secondID]);
    });
    MCGuiAsync(@"--window sends a command to the first window", 30, ^(MCGuiDone done) {
        MCGuiRunProcess(@[CLIPath(), @"open", @"big.txt:20", @"--window", firstID, @"--pid", pid], nil,
                        ^(int s, NSString *, NSString *) {
            GCHECK(s == 0);
            GCHECK([PathName(ActiveTab(c)) isEqualToString:@"big.txt"]);
            done();
        });
    });
    // The suites after this one work in the project's window, which is the
    // newest again once the other folder's goes.
    MCGuiStep(@"the other folder's window closes", ^{
        for (EditorController *e in [MCGuiControllers() copy])
            if (e != c && [e.rootPath.lastPathComponent isEqualToString:@"other"]) [e.window close];
        GCHECK(MCGuiControllers().count == 1 && MCGuiController() == c);
    });

    // ------------------------------------------------------- the socket goes
    MCGuiStep(@"stopping removes the socket; starting again puts it back", ^{
        MCControlServerStop();
        GCHECK(![fm fileExistsAtPath:socketPath]);
        GCHECK(MCControlSocketPath() == nil);
        MCControlServerStart((id<MCControlAppHost>)NSApp.delegate);
        GCHECK([fm fileExistsAtPath:socketPath]);
    });
}
