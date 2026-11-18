// LineHistorySuite.mm — `make guitest SUITES=line_history`: Show Line
// History (EditorLineHistory.h). A scratch repository holds hello.py,
// committed; a Claude Code session (a transcript in a scratch projects
// folder, beside copies of tests/fixtures/claude, named through
// MUTIBLE_CLAUDE_PROJECTS) then edited two places in it, and a line was
// typed by hand after. Resting on a line number shows the line's commit,
// the session (its title, the prompt the edit answered, when) or "Not
// committed"; View > Show Line History puts the words beside every line,
// and a click on them opens the commit page or the session's page for the
// file. An edit hides the words from the edited line on until the next
// look, which finds them on their new lines. A large file's look keeps the
// main thread's share small. The real ~/.claude is never read or written.
#import "Harness.h"
#import "EditorController.h"
#import "AgentModel.h"
#import "AgentWindow.h"
#import "ControlServer.h"
#import "EditorLineHistory.h"
#import "EditorPage.h"
#import "GitPanel.h"
#import "Gutter.h"
#import "Lsp.h"

namespace {

NSString *gBase, *gProj, *gReal, *gProjects, *gFile, *gBig;
NSString *const kSid = @"sess-line-history";

NSString *Json(id obj) {
    NSData *d = [NSJSONSerialization dataWithJSONObject:obj options:0 error:nil];
    return [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
}

NSString *Stamp(NSTimeInterval ago) {
    NSISO8601DateFormatter *f = [NSISO8601DateFormatter new];
    f.formatOptions = NSISO8601DateFormatWithInternetDateTime | NSISO8601DateFormatWithFractionalSeconds;
    return [f stringFromDate:[NSDate dateWithTimeIntervalSinceNow:-ago]];
}

NSMutableDictionary *Head(NSString *type, NSTimeInterval ago) {
    return [@{@"parentUuid": NSNull.null, @"isSidechain": @NO, @"type": type,
              @"timestamp": Stamp(ago), @"cwd": gReal, @"sessionId": kSid,
              @"version": @"9.9.9", @"gitBranch": @"main"} mutableCopy];
}

NSString *Prompt(NSTimeInterval ago, NSString *text) {
    NSMutableDictionary *d = Head(@"user", ago);
    d[@"message"] = @{@"role": @"user", @"content": text};
    return Json(d);
}

NSString *EditUse(NSTimeInterval ago, NSString *ident, NSString *oldText, NSString *newText) {
    NSMutableDictionary *d = Head(@"assistant", ago);
    NSDictionary *input = @{@"file_path": [gReal stringByAppendingPathComponent:@"hello.py"],
                            @"old_string": oldText, @"new_string": newText, @"replace_all": @NO};
    d[@"message"] = @{@"id": [@"msg_" stringByAppendingString:ident], @"role": @"assistant",
                      @"content": @[@{@"type": @"tool_use", @"id": ident, @"name": @"Edit",
                                      @"input": input}]};
    return Json(d);
}

NSString *Result(NSTimeInterval ago, NSString *ident) {
    NSMutableDictionary *d = Head(@"user", ago);
    d[@"message"] = @{@"role": @"user",
                      @"content": @[@{@"tool_use_id": ident, @"type": @"tool_result",
                                      @"content": @"The file has been updated successfully.",
                                      @"is_error": @NO}]};
    return Json(d);
}

BOOL Git(NSString *dir, NSArray<NSString *> *args) {
    MCGitResult *r = MCGitRunSync(dir, args);
    if (!r.ok) MCGuiNote(@"git %@ failed: %@", [args componentsJoinedByString:@" "], r.errorText);
    return r.ok;
}

NSString *const kCommitted = @"def area(r):\n"
                             @"    return 3 * r * r\n"
                             @"\n"
                             @"\n"
                             @"def main():\n"
                             @"    print(area(2))\n";
// Lines: 0 def area, 1 the session's return, 2-3 blank, 4-5 the session's
// perimeter, 6-7 blank, 8 def main, 9 print, 10 typed by hand.
NSString *const kNow = @"def area(r):\n"
                       @"    return 3.14159 * r * r\n"
                       @"\n"
                       @"\n"
                       @"def perimeter(r):\n"
                       @"    return 2 * 3.14159 * r\n"
                       @"\n"
                       @"\n"
                       @"def main():\n"
                       @"    print(area(2))\n"
                       @"# typed by hand\n";

void BuildWorld() {
    NSFileManager *fm = NSFileManager.defaultManager;
    gBase = [MCGuiScratch() stringByAppendingPathComponent:@"line-history"];
    gProj = [gBase stringByAppendingPathComponent:@"proj"];
    gProjects = [gBase stringByAppendingPathComponent:@"claude/projects"];
    [fm createDirectoryAtPath:gProj withIntermediateDirectories:YES attributes:nil error:nil];
    char buf[PATH_MAX];
    gReal = realpath(gProj.fileSystemRepresentation, buf) ? @(buf) : gProj;
    gFile = [gProj stringByAppendingPathComponent:@"hello.py"];
    gBig = [gProj stringByAppendingPathComponent:@"big.txt"];

    MCGuiWriteFile(gFile, kCommitted);
    NSMutableString *big = [NSMutableString string];
    for (int i = 0; i < 50000; i++) [big appendFormat:@"committed line %d of the big file\n", i];
    MCGuiWriteFile(gBig, big);
    Git(gProj, @[@"init", @"-q", @"-b", @"main"]);
    Git(gProj, @[@"add", @"-A"]);
    Git(gProj, @[@"commit", @"-q", @"-m", @"First version"]);

    // The transcripts: the fixtures (their paths moved here), and the
    // session that edited hello.py twice.
    NSString *fixtures = [MCGuiBuildDir().stringByDeletingLastPathComponent
        stringByAppendingPathComponent:@"tests/fixtures/claude/projects/-Users-user-Code-public-minicode"];
    NSString *sessDir = [gProjects stringByAppendingPathComponent:@"-line-history-proj"];
    for (NSString *rel in [fm enumeratorAtPath:fixtures]) {
        NSString *src = [fixtures stringByAppendingPathComponent:rel];
        BOOL isDir = NO;
        [fm fileExistsAtPath:src isDirectory:&isDir];
        if (isDir) continue;
        NSString *text = [NSString stringWithContentsOfFile:src encoding:NSUTF8StringEncoding error:nil];
        text = [text stringByReplacingOccurrencesOfString:@"/Users/user/Code/public/minicode"
                                               withString:[gBase stringByAppendingPathComponent:@"elsewhere"]];
        MCGuiWriteFile([sessDir stringByAppendingPathComponent:rel], text);
    }
    NSMutableString *t = [NSMutableString string];
    [t appendFormat:@"%@\n", Prompt(600, @"Use a better pi\nand nothing else yet")];
    [t appendFormat:@"%@\n", EditUse(598, @"e1", @"    return 3 * r * r\n", @"    return 3.14159 * r * r\n")];
    [t appendFormat:@"%@\n", Result(597, @"e1")];
    [t appendFormat:@"%@\n", Prompt(500, @"Now add a perimeter function")];
    [t appendFormat:@"%@\n", EditUse(498, @"e2", @"def main():\n",
                                     @"def perimeter(r):\n    return 2 * 3.14159 * r\n\n\ndef main():\n")];
    [t appendFormat:@"%@\n", Result(497, @"e2")];
    NSMutableDictionary *title = Head(@"ai-title", 496);
    title[@"aiTitle"] = @"Better pi and a perimeter";
    [t appendFormat:@"%@\n", Json(title)];
    MCGuiWriteFile([sessDir stringByAppendingFormat:@"/%@.jsonl", kSid], t);
    MCGuiWriteFile(gFile, kNow);
    MCSetEnv("MUTIBLE_CLAUDE_PROJECTS", gProjects.fileSystemRepresentation);
    MCSetEnv("MUTIBLE_CLAUDE_TMP", [gBase stringByAppendingPathComponent:@"claude-tmp"].fileSystemRepresentation);
}

EditorController *Win() {
    for (EditorController *c in MCGuiControllers())
        if ([c.rootPath isEqualToString:gProj]) return c;
    return nil;
}
MCLineHistory *History(EditorController *c) { return [c valueForKey:@"lineHistory"]; }
CodeTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
MCLineGutter *GutterOf(EditorController *c) {
    return (MCLineGutter *)Text(c).enclosingScrollView.verticalRulerView;
}
MCAgentModel *Model(EditorController *c) { return [(MCAgentWindow *)[c valueForKey:@"agents"] model]; }
id ActivePage(EditorController *c) { return [[c valueForKey:@"activeTab"] valueForKey:@"page"]; }

NSEvent *Mouse(NSEventType type, NSWindow *w, NSPoint inWindow) {
    return [NSEvent mouseEventWithType:type location:inWindow modifierFlags:0 timestamp:0
                          windowNumber:w.windowNumber context:nil eventNumber:0 clickCount:1
                              pressure:type == NSEventTypeMouseMoved ? 0 : 1];
}

}  // namespace

MC_GUI_SUITE(line_history) {
    __block EditorController *c = nil;
    __block NSUInteger looks = 0;

    MCGuiStep(@"a repository with hello.py committed, and a session that edited it", ^{
        BuildWorld();
        MCSetLineHistoryInline(NO);
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:gProj];
        c = Win();
        GCHECK(c != nil);
        MCGuiSetKeyWindow(c.window);
        GCHECK([c showFile:gFile]);
        GCHECK(History(c) != nil);
        GCHECK(GutterOf(c).onHoverLine != nil);
        (void)Model(c);   // the Agents model, which the line history reads
    });
    MCGuiWait(@"the session is indexed", 10, ^BOOL {
        for (MCAgentWork *w in Model(c).sessions)
            if ([w.title isEqualToString:@"Better pi and a perimeter"]) return YES;
        return NO;
    });

    // ------------------------------------------------------------- the box
    MCGuiStep(@"resting on the session's line number", ^{
        GCHECK(!History(c).popupVisible);
        [History(c) hoverLine:1];
        GCHECK(!History(c).popupVisible);   // not before the pointer rests
    });
    MCGuiWait(@"the box shows the session", 10, ^BOOL {
        return History(c).popupVisible &&
               [History(c).popupText containsString:@"Better pi and a perimeter"];
    });
    MCGuiStep(@"its title, the prompt the edit answered, and when", ^{
        NSString *t = History(c).popupText;
        MCGuiNote(@"box: %@", t);
        GCHECK([t hasPrefix:@"Written by a Claude Code session, not committed\n"]);
        GCHECK([t containsString:@"\nPrompt: Use a better pi\n"]);
        GCHECK([t containsString:@"\nEdited "] && [t containsString:@" minutes ago ("]);
        GCHECK([t hasSuffix:@"Click to see what the session changed in this file."]);
        GCHECK(History(c).popupLine == 1);
        // Moving down the gutter: the box follows at once.
        [History(c) hoverLine:5];
        GCHECK(History(c).popupLine == 5);
        GCHECK([History(c).popupText containsString:@"Prompt: Now add a perimeter function"]);
        [History(c) hoverLine:0];
        NSString *commit = History(c).popupText;
        MCGuiNote(@"commit: %@", commit);
        GCHECK([commit containsString:@"GUI Check"]);
        GCHECK([commit containsString:@"\nFirst version\n"]);
        GCHECK([commit hasSuffix:@"Click to open the commit."]);
        [History(c) hoverLine:10];
        GCHECK([History(c).popupText hasPrefix:@"Not committed\n"]);
        [History(c) hoverLine:9];
        GCHECK([History(c).popupText containsString:@"First version"]);
        [History(c) hoverLine:NSNotFound];
    });
    MCGuiWait(@"the box goes when the pointer leaves", 5, ^BOOL { return !History(c).popupVisible; });
    MCGuiStep(@"the pointer moving over the gutter itself", ^{
        MCLineGutter *g = GutterOf(c);
        const NSRect row = [g rowRectOfLine:4];
        GCHECK(!NSIsEmptyRect(row));
        // Over the line numbers, clear of the fold lane at the right.
        const NSPoint at = [g convertPoint:NSMakePoint(NSWidth(g.bounds) / 2, NSMidY(row)) toView:nil];
        [g mouseMoved:Mouse(NSEventTypeMouseMoved, c.window, at)];
    });
    MCGuiWait(@"the box for that line", 5, ^BOOL {
        return History(c).popupVisible && History(c).popupLine == 4;
    });
    MCGuiStep(@"leaving the gutter", ^{
        [GutterOf(c) mouseExited:Mouse(NSEventTypeMouseMoved, c.window, NSZeroPoint)];
    });
    MCGuiWait(@"closes the box", 5, ^BOOL { return !History(c).popupVisible; });

    // ------------------------------------------------------- beside every line
    MCGuiStep(@"View > Show Line History, no key, puts the words beside each line", ^{
        NSMenuItem *item = nil;
        for (NSMenuItem *top in NSApp.mainMenu.itemArray)
            for (NSMenuItem *i in top.submenu.itemArray)
                if (i.action == @selector(toggleLineHistory:)) item = i;
        GCHECK(item != nil && item.keyEquivalent.length == 0);
        GCHECK([item.title isEqualToString:@"Show Line History"]);
        GCHECK([c validateMenuItem:item] && item.state == NSControlStateValueOff);
        looks = History(c).looks;
        [c toggleLineHistory:item];
        GCHECK(MCLineHistoryInline());
        GCHECK([c validateMenuItem:item] && item.state == NSControlStateValueOn);
    });
    MCGuiWait(@"the words are drawn", 10, ^BOOL {
        return !NSIsEmptyRect([History(c) labelRectForLine:1]) &&
               !NSIsEmptyRect([History(c) labelRectForLine:0]);
    });
    MCGuiStep(@"a commit, a session, or not committed, in words", ^{
        MCLineHistory *h = History(c);
        NSString *commit = [h labelForLine:0], *session = [h labelForLine:1];
        MCGuiNote(@"line 0: %@ | line 1: %@", commit, session);
        GCHECK([commit containsString:@" GUI Check, "] && [commit hasSuffix:@" · First version"]);
        GCHECK([commit rangeOfString:@"^[0-9a-f]{7} " options:NSRegularExpressionSearch].location == 0);
        // The edits were about ten and eight minutes ago.
        GCHECK([session hasPrefix:@"Claude Code: Better pi and a perimeter, "] &&
               [session hasSuffix:@" minutes ago"]);
        GCHECK([[h labelForLine:4] hasPrefix:@"Claude Code: Better pi and a perimeter, "]);
        GCHECK(![[h labelForLine:4] isEqualToString:session]);   // the other, later edit
        GCHECK([[h labelForLine:9] hasSuffix:@"First version"]);
        GCHECK_EQ_STR([h labelForLine:10], @"Not committed");
        // Past the line's text, on its row.
        const NSRect r = [h labelRectForLine:1];
        const NSRect line = MCGuiLineTextRect(Text(c), [Text(c).string rangeOfString:@"3.14159 * r * r"].location);
        GCHECK(NSMinX(r) > NSMaxX(line) && NSMinY(r) <= NSMidY(line) && NSMaxY(r) >= NSMidY(line));
        [Text(c) display];
        MCGuiSnapshot(Text(c).enclosingScrollView, @"line-history-inline");
    });
    MCGuiStep(@"a click on the session's words opens its page for the file", ^{
        const NSRect r = [History(c) labelRectForLine:1];
        const NSPoint at = [Text(c) convertPoint:NSMakePoint(NSMidX(r), NSMidY(r)) toView:nil];
        [Text(c) mouseDown:Mouse(NSEventTypeLeftMouseDown, c.window, at)];
        id<MCEditorPage> page = ActivePage(c);
        GCHECK(page != nil && [page.pageKind isEqualToString:@"session-file"]);
        GCHECK([page.pageTitle hasPrefix:@"hello.py, changed by Better pi and a perimeter"]);
        GCHECK([c showFile:gFile]);
    });
    MCGuiWait(@"hello.py's words again", 10, ^BOOL {
        return !NSIsEmptyRect([History(c) labelRectForLine:0]);
    });
    MCGuiStep(@"a click on a commit's words opens the commit page", ^{
        const NSRect r = [History(c) labelRectForLine:0];
        const NSPoint at = [Text(c) convertPoint:NSMakePoint(NSMidX(r), NSMidY(r)) toView:nil];
        [Text(c) mouseDown:Mouse(NSEventTypeLeftMouseDown, c.window, at)];
        id<MCEditorPage> page = ActivePage(c);
        GCHECK(page != nil && [page.pageKey hasPrefix:@"commit:"]);
        MCGuiNote(@"commit page: %@", page.pageTitle);
        GCHECK([c showFile:gFile]);
    });
    MCGuiWait(@"the kept answer shows at once", 5, ^BOOL {
        return [[History(c) labelForLine:10] isEqualToString:@"Not committed"];
    });

    // ------------------------------------------------------------- an edit
    MCGuiStep(@"a line typed at the top: the words below wait for the next look", ^{
        looks = History(c).looks;
        MCGuiUndoGroup(Text(c).undoManager, ^{
            Text(c).selectedRange = NSMakeRange(0, 0);
            [Text(c) insertText:@"import math\n" replacementRange:Text(c).selectedRange];
        });
        GCHECK([History(c) labelForLine:1] == nil && [History(c) labelForLine:2] == nil);
    });
    MCGuiWait(@"looked again after the pause", 10, ^BOOL { return History(c).looks > looks; });
    MCGuiStep(@"each line's words on its new line; the new line not committed", ^{
        MCLineHistory *h = History(c);
        GCHECK_EQ_STR([h labelForLine:0], @"Not committed");
        GCHECK([[h labelForLine:1] hasSuffix:@"First version"]);
        GCHECK([[h labelForLine:2] hasPrefix:@"Claude Code: Better pi and a perimeter"]);
        GCHECK_EQ_STR([h labelForLine:11], @"Not committed");
        [Text(c).undoManager undo];
    });

    // ------------------------------------------------------------- a large file
    MCGuiStep(@"a 50,000-line file", ^{
        looks = History(c).looks;
        GCHECK([c showFile:gBig]);
    });
    MCGuiWait(@"its look", 30, ^BOOL {
        return History(c).looks > looks && [History(c).lookedPath isEqualToString:gBig];
    });
    MCGuiStep(@"its words, with little of the main thread", ^{
        GCHECK([[History(c) labelForLine:0] hasSuffix:@"First version"]);
        GCHECK([[History(c) labelForLine:49999] hasSuffix:@"First version"]);
        MCGuiNote(@"main thread for the look: %.1f ms", History(c).lastLookMainCpuMs);
        GCHECK_TIME(History(c).lastLookMainCpuMs, 60, @"the main thread's share of a 50,000-line look");
    });

    MCGuiStep(@"the switch put back and the suite's window closed", ^{
        [c toggleLineHistory:nil];
        GCHECK(!MCLineHistoryInline());
        [c.window close];
        c = nil;
        MCGuiSetKeyWindow(nil);
        MCSetEnv("MUTIBLE_CLAUDE_PROJECTS", nullptr);
        MCSetEnv("MUTIBLE_CLAUDE_TMP", nullptr);
        GCHECK(Win() == nil);
    });
}
