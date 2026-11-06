// AgentCommandsSuite.mm — `make guitest SUITES=agentcommands`: what an
// agent's commands may and may not do to the user's window (the third
// review's P2). An agent's `open`, `diff` and `tour` never move the user's
// typing into another document; a note waits in its tab, and comparisons
// have a preview tab of their own; notes never move the caret in a tab of
// the user's and survive the reload of a file the agent just wrote; only
// plain files are read, up to 16 MB; the browser keeps a page the user
// loaded; the socket takes one request at a time and never blocks on a
// slow reader; the command line skips untrusted folders and refused
// sockets, and reads never launch the app; Compare with Main shows main at
// the branch point; old prompt files go; a restart record cannot choose
// the command that runs.
//
// It works in a window of its own on a scratch folder, closed at the end,
// so the window the other suites use keeps its tabs, terminals and browser.
#import "Harness.h"
#import "EditorController.h"
#import "AgentModel.h"
#import "AgentNotes.h"
#import "Browser.h"
#import "ChangePanel.h"
#import "CompareView.h"
#import "ControlCommands.h"
#import "ControlServer.h"
#import "GitPanel.h"
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

@interface EditorController (AgentCommandsSuite)
- (MCChangeSession *)changeSession;
- (void)selectTabAtIndex:(NSInteger)i;
@end

namespace {

NSString *gDir;   // the suite's folder
NSString *gWid;   // its window's id, for --window

NSString *CLIPath() { return [MCGuiBuildDir() stringByAppendingPathComponent:@"minicode-cli"]; }

// The command line against this process and the suite's window.
void CLI(NSString *name, NSArray<NSString *> *args, void (^check)(int, NSString *, NSString *)) {
    MCGuiAsync(name, 60, ^(MCGuiDone done) {
        NSMutableArray *argv = [NSMutableArray arrayWithObject:CLIPath()];
        [argv addObjectsFromArray:args];
        [argv addObjectsFromArray:@[@"--window", gWid, @"--pid",
                                    [NSString stringWithFormat:@"%d", getpid()]]];
        MCGuiRunProcess(argv, nil, ^(int status, NSString *out, NSString *err) {
            check(status, out, err);
            done();
        });
    });
}

NSString *F(NSString *name) { return [gDir stringByAppendingPathComponent:name]; }

NSString *Lines(int n, NSString *prefix) {
    NSMutableString *s = [NSMutableString string];
    for (int i = 1; i <= n; i++) [s appendFormat:@"%@ line %d\n", prefix, i];
    return s;
}

NSRange LinesRange(NSString *s, int from, int to) {
    NSUInteger start = 0;
    for (int l = 1; l < from; l++) start = NSMaxRange([s lineRangeForRange:NSMakeRange(start, 0)]);
    NSUInteger end = start;
    for (int l = from; l <= to; l++) end = NSMaxRange([s lineRangeForRange:NSMakeRange(end, 0)]);
    if (end > start && [s characterAtIndex:end - 1] == '\n') end--;
    return NSMakeRange(start, end - start);
}

NSDictionary *ActiveTab(EditorController *c) {
    for (NSDictionary *t in [c controlTabs])
        if ([t[@"active"] boolValue]) return t;
    return nil;
}

NSInteger TabIndex(EditorController *c, NSString *name, NSString *kind) {
    NSArray<NSDictionary *> *tabs = [c controlTabs];
    for (NSUInteger i = 0; i < tabs.count; i++) {
        id p = tabs[i][@"path"];
        if (kind && ![tabs[i][@"kind"] isEqualToString:kind]) continue;
        if (!name || ([p isKindOfClass:NSString.class] && [[p lastPathComponent] isEqualToString:name]))
            return (NSInteger)i;
    }
    return -1;
}

NSString *ActiveName(EditorController *c) {
    id p = ActiveTab(c)[@"path"];
    return [p isKindOfClass:NSString.class] ? [p lastPathComponent] : @"";
}

// What the user would type, into whatever has the keyboard.
void Type(EditorController *c, NSString *text) {
    NSResponder *fr = c.window.firstResponder;
    if ([fr isKindOfClass:NSTextView.class]) {
        NSTextView *tv = (NSTextView *)fr;
        MCGuiUndoGroup(tv.undoManager, ^{ [tv insertText:text replacementRange:tv.selectedRange]; });
    }
}

BOOL Git(NSString *dir, NSArray<NSString *> *args) {
    MCGitResult *r = MCGitRunSync(dir, args);
    if (!r.ok) MCGuiNote(@"git %@ failed: %@", [args componentsJoinedByString:@" "], r.errorText);
    return r.ok;
}

// A socket file bound at `path` that nothing listens on.
BOOL BindDeadSocket(NSString *path) {
    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    if (strlen(path.fileSystemRepresentation) >= sizeof addr.sun_path) return NO;
    strlcpy(addr.sun_path, path.fileSystemRepresentation, sizeof addr.sun_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    BOOL ok = bind(fd, (struct sockaddr *)&addr, sizeof addr) == 0;
    close(fd);
    return ok;
}

int Connect(NSString *path) {
    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strlcpy(addr.sun_path, path.fileSystemRepresentation, sizeof addr.sun_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Reads answer lines until `count` arrive or `seconds` pass.
NSArray<NSString *> *ReadLines(int fd, NSUInteger count, double seconds) {
    struct timeval tv = {(time_t)seconds, (suseconds_t)((seconds - (time_t)seconds) * 1e6)};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    NSMutableString *got = [NSMutableString string];
    char buf[65536];
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while ([got componentsSeparatedByString:@"\n"].count <= count && [until timeIntervalSinceNow] > 0) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) break;
        [got appendString:[[NSString alloc] initWithBytes:buf length:(NSUInteger)n
                                                 encoding:NSUTF8StringEncoding] ?: @""];
    }
    NSMutableArray *lines = [[got componentsSeparatedByString:@"\n"] mutableCopy];
    [lines removeLastObject];
    return lines;
}

}  // namespace

MC_GUI_SUITE(agentcommands) {
    __block EditorController *c = nil;
    __block NSTextView *tv = nil;
    __block MCAgentNotes *notes = nil;
    __block NSString *git = nil;
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *pid = [NSString stringWithFormat:@"%d", getpid()];

    // Set now: the command lines below are made as the steps are queued.
    gDir = [MCGuiScratch() stringByAppendingPathComponent:@"cmds"];
    MCGuiStep(@"a folder, a repository, and a window of the suite's own", ^{
        MCGuiWriteFile(F(@"mine.txt"), Lines(10, @"mine"));
        MCGuiWriteFile(F(@"theirs.py"), Lines(20, @"theirs"));
        MCGuiWriteFile(F(@"other.py"), Lines(5, @"other"));
        MCGuiWriteFile(F(@"fresh.txt"), Lines(5, @"old"));
        git = MCGitExecutable();
        if (git) {
            Git(gDir, @[@"init", @"-q", @"-b", @"main"]);
            Git(gDir, @[@"add", @"-A"]);
            Git(gDir, @[@"commit", @"-q", @"-m", @"one"]);
        }
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:gDir];
        c = MCGuiController();
        GCHECK([[c.rootPath stringByResolvingSymlinksInPath]
                   isEqualToString:gDir.stringByResolvingSymlinksInPath]);
        MCGuiSetKeyWindow(c.window);
        gWid = [NSString stringWithFormat:@"%ld", (long)MCControlWindowID(c)];
        tv = [c valueForKey:@"textView"];
        notes = c.agentNotes;
        GCHECK(c.window.isKeyWindow);
    });

    // ------------------------------------------- typing goes on undisturbed
    MCGuiStep(@"the user is typing in mine.txt", ^{
        GCHECK([c openFileForAgent:F(@"mine.txt") preview:NO]);
        [c.window makeFirstResponder:tv];
        tv.selectedRange = NSMakeRange(0, 0);
        Type(c, @"a");
        GCHECK([tv.string hasPrefix:@"amine line 1"]);
        GCHECK([c userIsInDocument]);
    });
    CLI(@"an agent opens theirs.py with a note meanwhile",
        @[@"open", F(@"theirs.py:5"), @"--to", @"6", @"--note", @"Look here"],
        ^(int s, NSString *, NSString *err) {
        GCHECK(s == 0);
        GCHECK([err containsString:@"beside theirs"]);
        // Still the user's file on show, with the keyboard.
        GCHECK_EQ_STR(ActiveName(c), @"mine.txt");
        GCHECK(c.window.firstResponder == tv);
        GCHECK(!notes.noteShown);
        // The agent's file waits in the preview tab beside it.
        NSInteger i = TabIndex(c, @"theirs.py", nil);
        GCHECK(i >= 0 && [[c controlTabs][(NSUInteger)i][@"preview"] boolValue]);
    });
    MCGuiStep(@"the next keystroke lands in mine.txt, and theirs.py is untouched", ^{
        Type(c, @"b");
        GCHECK([tv.string hasPrefix:@"abmine line 1"]);
        NSInteger i = TabIndex(c, @"theirs.py", nil);
        GCHECK(i >= 0 && ![[c controlTabs][(NSUInteger)i][@"dirty"] boolValue]);
        GCHECK([[c controlTabs][(NSUInteger)i][@"preview"] boolValue]);
        GCHECK([ActiveTab(c)[@"dirty"] boolValue]);
    });
    MCGuiStep(@"switching to the agent's tab shows the note at its lines", ^{
        [c selectTabAtIndex:TabIndex(c, @"theirs.py", nil)];
        GCHECK_EQ_STR(ActiveName(c), @"theirs.py");
        GCHECK(notes.noteShown);
        GCHECK_EQ_STR(notes.noteText, @"Look here");
        GCHECK(NSEqualRanges(notes.noteRange, LinesRange(tv.string, 5, 6)));
        // In the agent's own preview tab the caret goes to the lines.
        GCHECK(tv.selectedRange.location == LinesRange(tv.string, 5, 5).location);
    });
    MCGuiStep(@"back in mine.txt, typing", ^{
        [c selectTabAtIndex:TabIndex(c, @"mine.txt", nil)];
        GCHECK(c.window.firstResponder == tv && [c userIsInDocument]);
    });
    CLI(@"a diff while the user types waits beside them too", @[@"diff", F(@"other.py"), F(@"theirs.py")],
        ^(int s, NSString *, NSString *err) {
        GCHECK(s == 0 && [err containsString:@"beside theirs"]);
        GCHECK_EQ_STR(ActiveName(c), @"mine.txt");
        GCHECK(c.window.firstResponder == tv);
        // Its own preview tab: the note's tab is still there.
        GCHECK(TabIndex(c, nil, @"diff") >= 0);
        GCHECK(TabIndex(c, @"theirs.py", @"text") >= 0);
        Type(c, @"c");
        GCHECK([tv.string hasPrefix:@"abcmine line 1"]);
    });
    MCGuiStep(@"the note's tab kept its note through the diff", ^{
        [c selectTabAtIndex:TabIndex(c, @"theirs.py", @"text")];
        GCHECK(notes.noteShown && [notes.noteText isEqualToString:@"Look here"]);
        [notes closeNote];
    });

    // ------------------------------------------------ the skill's example
    MCGuiStep(@"the keyboard in the tree, as when an agent reports from a terminal", ^{
        [c.window makeFirstResponder:[c valueForKey:@"outline"]];
        GCHECK(![c userIsInDocument]);
    });
    CLI(@"the example's open --note", @[@"open", F(@"other.py:2"), @"--note", @"The fix"],
        ^(int s, NSString *, NSString *) {
        GCHECK(s == 0 && notes.noteShown && [ActiveName(c) isEqualToString:@"other.py"]);
    });
    CLI(@"the example's diff", @[@"diff", F(@"mine.txt"), F(@"other.py")], ^(int s, NSString *, NSString *) {
        GCHECK(s == 0);
        GCHECK([ActiveTab(c)[@"kind"] isEqualToString:@"diff"]);
        // The diff replaced the earlier diff, not the note's file.
        NSUInteger diffs = 0;
        for (NSDictionary *t in [c controlTabs]) diffs += [t[@"kind"] isEqualToString:@"diff"];
        GCHECK(diffs == 1);
        GCHECK(TabIndex(c, @"other.py", @"text") >= 0);
    });
    MCGuiStep(@"and the note is still there to see", ^{
        [c selectTabAtIndex:TabIndex(c, @"other.py", @"text")];
        GCHECK(notes.noteShown && [notes.noteText isEqualToString:@"The fix"]);
        [notes closeNote];
        [c.window makeFirstResponder:[c valueForKey:@"outline"]];
    });

    // --------------------------------------- the user's caret stays put
    __block NSRange caret = NSMakeRange(0, 0);
    MCGuiStep(@"mine.txt on show, its caret on line 2", ^{
        [c selectTabAtIndex:TabIndex(c, @"mine.txt", nil)];
        [c.window makeFirstResponder:[c valueForKey:@"outline"]];
        caret = NSMakeRange(LinesRange(tv.string, 2, 2).location + 3, 0);
        tv.selectedRange = caret;
    });
    CLI(@"a note on the user's own tab", @[@"open", F(@"mine.txt:7"), @"--note", @"Yours"],
        ^(int s, NSString *, NSString *) {
        GCHECK(s == 0 && notes.noteShown);
        GCHECK(NSEqualRanges(notes.noteRange, LinesRange(tv.string, 7, 7)));
        GCHECK(NSEqualRanges(tv.selectedRange, caret));
        [notes closeNote];
    });

    // ------------------------------------------------------------- tours
    MCGuiStep(@"a tour file; the user types again", ^{
        MCGuiWriteFile(F(@"tour.json"),
                       [NSString stringWithFormat:@"[{\"path\":\"%@\",\"line\":3,\"note\":\"One\"},"
                                                  "{\"path\":\"%@\",\"line\":1,\"note\":\"Two\"}]",
                                                  F(@"theirs.py"), F(@"other.py")]);
        [c.window makeFirstResponder:tv];
        GCHECK([c userIsInDocument]);
    });
    CLI(@"the tour's first stop waits beside the user", @[@"tour", F(@"tour.json")],
        ^(int s, NSString *, NSString *err) {
        GCHECK(s == 0 && [err containsString:@"beside theirs"]);
        GCHECK_EQ_STR(ActiveName(c), @"mine.txt");
        GCHECK(c.window.firstResponder == tv);
        GCHECK(notes.tourActive && !notes.noteShown);
    });
    MCGuiStep(@"and shows when its tab is chosen", ^{
        [c selectTabAtIndex:TabIndex(c, @"theirs.py", @"text")];
        GCHECK_EQ_STR(notes.noteLabel, @"Claude · 1 of 2");
        GCHECK(NSEqualRanges(notes.noteRange, LinesRange(tv.string, 3, 3)));
        GCHECK([notes stepTour:1]);   // the user's own step shows its stop
        GCHECK_EQ_STR(ActiveName(c), @"other.py");
        GCHECK_EQ_STR(notes.noteLabel, @"Claude · 2 of 2");
        [notes closeNote];
        [c.window makeFirstResponder:[c valueForKey:@"outline"]];
    });

    // ------------------------------------- a file the agent has just written
    MCGuiStep(@"fresh.txt on show, clean", ^{
        GCHECK([c openFileForAgent:F(@"fresh.txt") preview:NO]);
        [c.window makeFirstResponder:[c valueForKey:@"outline"]];
        GCHECK(![ActiveTab(c)[@"dirty"] boolValue]);
        // The agent rewrites it, and points at a line of the new text at once.
        MCGuiWriteFile(F(@"fresh.txt"), [@"new 1\nnew 2\n" stringByAppendingString:Lines(5, @"old")]);
    });
    CLI(@"a note on the new text", @[@"open", F(@"fresh.txt:2"), @"--note", @"Added"],
        ^(int s, NSString *, NSString *) {
        GCHECK(s == 0 && notes.noteShown);
        GCHECK([tv.string hasPrefix:@"new 1\nnew 2\n"]);
        GCHECK(NSEqualRanges(notes.noteRange, LinesRange(tv.string, 2, 2)));
    });
    MCGuiPause(2.0);   // the file system's event for the write comes and goes
    MCGuiStep(@"the reload that followed left the note alone", ^{
        GCHECK(notes.noteShown && [notes.noteText isEqualToString:@"Added"]);
        GCHECK(NSEqualRanges(notes.noteRange, LinesRange(tv.string, 2, 2)));
        GCHECK(![ActiveTab(c)[@"dirty"] boolValue]);
        [notes closeNote];
    });

    // ------------------------------------------------ only plain files
    MCGuiStep(@"a FIFO and a file over 16 MB", ^{
        unlink(F(@"pipe").fileSystemRepresentation);
        GCHECK(mkfifo(F(@"pipe").fileSystemRepresentation, 0600) == 0);
        NSMutableData *big = [NSMutableData dataWithLength:17 * 1024 * 1024];
        memset(big.mutableBytes, 'x', big.length);
        GCHECK([big writeToFile:F(@"big.log") atomically:NO]);
    });
    CLI(@"open refuses the FIFO, at once", @[@"open", F(@"pipe")], ^(int s, NSString *, NSString *err) {
        GCHECK(s == 1 && [err containsString:@"not a plain file"]);
    });
    CLI(@"diff refuses it", @[@"diff", F(@"pipe"), F(@"mine.txt")], ^(int s, NSString *, NSString *err) {
        GCHECK(s == 1 && [err containsString:@"not a plain file"]);
    });
    CLI(@"and /dev/zero", @[@"diff", F(@"mine.txt"), @"/dev/zero"], ^(int s, NSString *, NSString *err) {
        GCHECK(s == 1 && [err containsString:@"not a plain file"]);
    });
    CLI(@"and a file over 16 MB", @[@"diff", F(@"big.log"), F(@"mine.txt")], ^(int s, NSString *, NSString *err) {
        GCHECK(s == 1 && [err containsString:@"larger than 16 MB"]);
    });
    MCGuiStep(@"a tour stop that is a FIFO", ^{
        MCGuiWriteFile(F(@"bad-tour.json"),
                       [NSString stringWithFormat:@"[{\"path\":\"%@\",\"line\":1,\"note\":\"x\"}]", F(@"pipe")]);
    });
    CLI(@"is refused", @[@"tour", F(@"bad-tour.json")], ^(int s, NSString *, NSString *err) {
        GCHECK(s == 1 && [err containsString:@"no such file"]);
    });

    // ------------------------------------------- notes from the socket
    MCGuiStep(@"a note sent straight to the socket is cleaned as the command line's", ^{
        NSMutableString *note = [NSMutableString stringWithString:@"line one\\nline two \\u202Eevil\\u202C"];
        for (int i = 0; i < 100; i++) [note appendString:@" more words"];
        NSString *line = [NSString stringWithFormat:
            @"{\"v\":1,\"id\":9,\"cmd\":\"open\",\"args\":{\"window\":%@,\"files\":[{\"path\":\"%@\",\"line\":2}],"
             "\"note\":\"%@\"}}", gWid, F(@"other.py"), note];
        __block NSString *answer = nil;
        MCControlHandleLine(line, ^(NSString *a) { answer = a; });
        GCHECK([answer containsString:@"\"ok\":true"]);
        GCHECK(notes.noteShown);
        GCHECK(notes.noteText.length == 500 && [notes.noteText hasPrefix:@"line one line two evil more"]);
        GCHECK([notes.noteText rangeOfString:@"\n"].location == NSNotFound);
        GCHECK([notes.noteText rangeOfString:@"‮"].location == NSNotFound);
        [notes closeNote];
    });

    // ------------------------------------------------------- --base
    CLI(@"a --base that git would read as an option", @[@"diff", F(@"mine.txt"), @"--base", @"-p"],
        ^(int s, NSString *, NSString *err) {
        GCHECK(s == 2 && [err containsString:@"revision"]);
    });
    MCGuiStep(@"and the same over the socket", ^{
        NSString *line = [NSString stringWithFormat:
            @"{\"v\":1,\"id\":1,\"cmd\":\"diff\",\"args\":{\"path\":\"%@\",\"base\":\"--output=/tmp/x\"}}",
            F(@"mine.txt")];
        __block NSString *answer = nil;
        MCControlHandleLine(line, ^(NSString *a) { answer = a; });
        GCHECK([answer containsString:@"\"ok\":false"] && [answer containsString:@"revision"]);
    });

    // ------------------------------------------- the user's browser page
    __block NSString *page1 = nil, *page2 = nil, *page3 = nil;
    MCGuiStep(@"three local pages", ^{
        for (NSString *n in @[@"one", @"two", @"three"])
            MCGuiWriteFile(F([n stringByAppendingString:@".html"]),
                           [NSString stringWithFormat:@"<html><head><title>Page %@</title></head></html>", n]);
        page1 = [NSURL fileURLWithPath:F(@"one.html")].absoluteString;
        page2 = [NSURL fileURLWithPath:F(@"two.html")].absoluteString;
        page3 = [NSURL fileURLWithPath:F(@"three.html")].absoluteString;
    });
    MCGuiAsync(@"browse page two", 30, ^(MCGuiDone done) {
        MCGuiRunProcess(@[CLIPath(), @"browse", page2, @"--window", gWid, @"--pid", pid], nil,
                        ^(int s, NSString *, NSString *) {
            GCHECK(s == 0);
            done();
        });
    });
    MCGuiWait(@"page two loads", 20, ^BOOL { return [[c controlBrowser][@"title"] isEqualToString:@"Page two"]; });
    MCGuiAsync(@"the agent may replace its own page", 30, ^(MCGuiDone done) {
        MCGuiRunProcess(@[CLIPath(), @"browse", page3, @"--window", gWid, @"--pid", pid], nil,
                        ^(int s, NSString *, NSString *) {
            GCHECK(s == 0);
            done();
        });
    });
    MCGuiWait(@"page three loads", 20, ^BOOL { return [[c controlBrowser][@"title"] isEqualToString:@"Page three"]; });
    MCGuiStep(@"the user goes to page one", ^{
        BrowserView *b = [c valueForKey:@"browser"];
        GCHECK(!b.showsUserPage);
        [b navigateToString:page1];
    });
    MCGuiWait(@"page one loads", 20, ^BOOL { return [[c controlBrowser][@"title"] isEqualToString:@"Page one"]; });
    MCGuiAsync(@"and the agent may not take it from them", 30, ^(MCGuiDone done) {
        MCGuiRunProcess(@[CLIPath(), @"browse", page2, @"--window", gWid, @"--pid", pid], nil,
                        ^(int s, NSString *, NSString *err) {
            GCHECK(s == 1 && [err containsString:@"the user opened"]);
            done();
        });
    });
    MCGuiPause(0.5);
    MCGuiStep(@"page one is still on show", ^{
        GCHECK([[c controlBrowser][@"title"] isEqualToString:@"Page one"]);
        GCHECK(((BrowserView *)[c valueForKey:@"browser"]).showsUserPage);
    });

    // ------------------------------------------------- the command line
    MCGuiAsync(@"a read never launches minicode", 30, ^(MCGuiDone done) {
        NSString *empty = [MCGuiScratch() stringByAppendingPathComponent:@"no-app-2"];
        [fm createDirectoryAtPath:empty withIntermediateDirectories:YES attributes:nil error:nil];
        MCGuiRunProcess(@[CLIPath(), @"state"],
                        @{@"TMPDIR": empty, @"MINICODE_CONTROL_DIR": [empty stringByAppendingPathComponent:@"MiniCode"],
                          @"MINICODE_LAUNCHER_DRYRUN": @"1", @"MINICODE_CLI_WAIT": @"0.4"},
                        ^(int s, NSString *out, NSString *err) {
            GCHECK(s == 3);
            if (![err containsString:@"does not answer"]) {   // no older minicode on this Mac
                GCHECK(out.length == 0);   // no "open -a" printed: nothing launched
                GCHECK([err containsString:@"not running"]);
            }
            done();
        });
    });
    __block NSString *loose = nil;
    MCGuiStep(@"a folder anyone may enter, with a socket in it", ^{
        loose = [MCGuiScratch() stringByAppendingPathComponent:@"loose"];
        [fm createDirectoryAtPath:loose withIntermediateDirectories:YES attributes:nil error:nil];
        chmod(loose.fileSystemRepresentation, 0755);
        GCHECK(BindDeadSocket([loose stringByAppendingFormat:@"/%d.sock", getpid()]));
    });
    MCGuiAsync(@"is not looked in", 30, ^(MCGuiDone done) {
        MCGuiRunProcess(@[CLIPath(), @"state"], @{@"MINICODE_CONTROL_DIR": loose},
                        ^(int s, NSString *, NSString *err) {
            GCHECK(s == 3);
            if (![err containsString:@"does not answer"]) GCHECK([err containsString:@"not running"]);
            done();
        });
    });
    __block NSTask *sleeper = nil;
    __block NSString *leftover = nil;
    MCGuiStep(@"a newer socket left behind, its pid now another live process's", ^{
        sleeper = [NSTask launchedTaskWithExecutableURL:[NSURL fileURLWithPath:@"/bin/sleep"]
                                              arguments:@[@"60"] error:nil terminationHandler:nil];
        GCHECK(sleeper.processIdentifier > 0);
        leftover = [MCControlSocketFolder() stringByAppendingFormat:@"/%d.sock", sleeper.processIdentifier];
        GCHECK(BindDeadSocket(leftover));
    });
    MCGuiAsync(@"the command line moves on to the live minicode", 30, ^(MCGuiDone done) {
        MCGuiRunProcess(@[CLIPath(), @"state"], @{@"MINICODE_CONTROL_DIR": MCControlSocketFolder()},
                        ^(int s, NSString *out, NSString *err) {
            GCHECK(s == 0);
            if (s) MCGuiNote(@"%@", err);
            NSData *d = [out dataUsingEncoding:NSUTF8StringEncoding];
            NSDictionary *st = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
            GCHECK([st[@"pid"] intValue] == getpid());
            done();
        });
    });
    MCGuiStep(@"and the next start's cleanup takes the leftover away", ^{
        GCHECK(MCControlRemoveStaleSockets(MCControlSocketFolder()) == 1);
        GCHECK(![fm fileExistsAtPath:leftover]);
        GCHECK([fm fileExistsAtPath:MCControlSocketPath()]);
        [sleeper terminate];
    });

    // ------------------------------------------------------- the socket
    MCGuiAsync(@"lines sent at once are answered one at a time, in order", 20, ^(MCGuiDone done) {
        NSString *path = MCControlSocketPath();
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            int fd = Connect(path);
            NSMutableString *req = [NSMutableString string];
            for (int i = 1; i <= 20; i++) [req appendFormat:@"{\"v\":1,\"id\":%d,\"cmd\":\"ping\"}\n", i];
            if (fd >= 0) write(fd, req.UTF8String, strlen(req.UTF8String));
            NSArray<NSString *> *answers = fd >= 0 ? ReadLines(fd, 20, 10) : @[];
            if (fd >= 0) close(fd);
            dispatch_async(dispatch_get_main_queue(), ^{
                GCHECK(answers.count == 20);
                BOOL inOrder = YES;
                for (NSUInteger i = 0; i < answers.count; i++)
                    inOrder = inOrder && [answers[i] hasPrefix:[NSString stringWithFormat:@"{\"id\":%lu,", i + 1]];
                GCHECK(inOrder);
                done();
            });
        });
    });
    MCGuiAsync(@"a client that sends a flood and never reads holds up no one", 30, ^(MCGuiDone done) {
        NSString *path = MCControlSocketPath();
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            int hog = Connect(path);
            if (hog >= 0) {
                fcntl(hog, F_SETFL, fcntl(hog, F_GETFL) | O_NONBLOCK);
                std::string flood;
                for (int i = 0; i < 4000; i++)
                    flood += "{\"v\":1,\"id\":1,\"cmd\":\"term-output\",\"args\":{\"terminal\":1}}\n";
                flood += "{\"v\":1,\"id\":2,\"cmd\":\"state\"}\n";
                size_t off = 0;
                for (int tries = 0; tries < 200 && off < flood.size(); tries++) {
                    ssize_t n = write(hog, flood.data() + off, flood.size() - off);
                    if (n > 0) off += (size_t)n;
                    else usleep(2000);
                }
            }
            NSDate *t0 = [NSDate date];
            int other = Connect(path);
            const char *ping = "{\"v\":1,\"id\":5,\"cmd\":\"ping\"}\n";
            if (other >= 0) write(other, ping, strlen(ping));
            NSArray<NSString *> *answer = other >= 0 ? ReadLines(other, 1, 10) : @[];
            const double took = -[t0 timeIntervalSinceNow];
            if (other >= 0) close(other);
            if (hog >= 0) close(hog);
            dispatch_async(dispatch_get_main_queue(), ^{
                GCHECK(hog >= 0 && other >= 0);
                GCHECK(answer.count == 1 && [answer.firstObject containsString:@"\"ok\":true"]);
                if (!GCHECK(took < 3)) MCGuiNote(@"the other client waited %.2f s", took);
                done();
            });
        });
    });

    // --------------------------------------------- compare with main
    __block MCAgentModel *model = nil;
    __block NSString *repo = nil;
    MCGuiStep(@"a worktree whose main has moved on since it branched", ^{
        if (!git) { MCGuiNote(@"no git: skipping"); return; }
        repo = [MCGuiScratch() stringByAppendingPathComponent:@"mb"];
        MCGuiWriteFile([repo stringByAppendingPathComponent:@"f.txt"], @"base\n");
        MCGuiWriteFile([repo stringByAppendingPathComponent:@".gitignore"], @".claude/\n");
        Git(repo, @[@"init", @"-q", @"-b", @"main"]);
        Git(repo, @[@"add", @"-A"]);
        Git(repo, @[@"commit", @"-q", @"-m", @"base"]);
        NSString *wt = [repo stringByAppendingPathComponent:@".claude/worktrees/agent-x"];
        Git(repo, @[@"worktree", @"add", @"-q", @"-b", @"worktree-agent-x", wt]);
        MCGuiWriteFile([wt stringByAppendingPathComponent:@"f.txt"], @"agent\n");
        Git(wt, @[@"commit", @"-q", @"-am", @"agent"]);
        MCGuiWriteFile([repo stringByAppendingPathComponent:@"f.txt"], @"main later\n");
        Git(repo, @[@"commit", @"-q", @"-am", @"later"]);
        model = [[MCAgentModel alloc] initWithFolder:repo];
    });
    MCGuiWait(@"git has answered about it", 20, ^BOOL {
        return !git || (model.worktreesLoaded && model.worktrees.count == 1 && model.mainBranch);
    });
    MCGuiAsync(@"the left pane is main at the branch point, not main now", 20, ^(MCGuiDone done) {
        if (!git) { done(); return; }
        MCAgentWorktree *w = model.worktrees.firstObject;
        [model changesInWorktree:w completion:^(NSArray<MCWorktreeChange *> *changes, NSString *error) {
            GCHECK(error == nil && changes.count == 1 && [changes.firstObject.path isEqualToString:@"f.txt"]);
            if (!changes.count) { done(); return; }
            [model comparisonForChange:changes.firstObject inWorktree:w completion:^(MCComparison *cmp) {
                GCHECK_EQ_STR(cmp.leftText, @"base\n");
                GCHECK_EQ_STR(cmp.rightText, @"agent\n");
                GCHECK([cmp.leftLabel hasPrefix:@"main at the branch point ("]);
                GCHECK(cmp.changedRows == 1);
                [model stop];
                done();
            }];
        }];
    });

    // --------------------------------------------------- prompt files
    MCGuiStep(@"prompt files of a minicode that has quit go; this one's stay", ^{
        NSString *dir = [MCGuiScratch() stringByAppendingPathComponent:@"MiniCode-agent"];
        [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        pid_t dead = fork();
        if (dead == 0) _exit(0);
        waitpid(dead, nullptr, 0);
        NSString *gone = [dir stringByAppendingFormat:@"/task-%d-1.txt", dead];
        NSString *ours = [dir stringByAppendingFormat:@"/task-%d-99.txt", getpid()];
        NSString *other = [dir stringByAppendingPathComponent:@"keep.me"];
        MCGuiWriteFile(gone, @"x");
        MCGuiWriteFile(ours, @"x");
        MCGuiWriteFile(other, @"x");
        MCRemoveStalePromptFiles(dir);
        GCHECK(![fm fileExistsAtPath:gone] && [fm fileExistsAtPath:ours] && [fm fileExistsAtPath:other]);
        [fm removeItemAtPath:ours error:nil];
        [fm removeItemAtPath:other error:nil];
    });

    // ------------------------------------------- the restart record
    MCGuiStep(@"a restart record naming a folder that is not minicode's source runs nothing", ^{
        const NSUInteger before = [c controlTerminals].count;
        [[c changeSession] resumeWithSource:gDir command:@"touch /tmp/should-not-run &&"];
        GCHECK([c controlTerminals].count == before);
    });

    MCGuiStep(@"the suite's window closes", ^{
        unlink(F(@"pipe").fileSystemRepresentation);
        [c.window close];
        c = nil;
        tv = nil;
        notes = nil;
        MCGuiSetKeyWindow(nil);
    });
}
