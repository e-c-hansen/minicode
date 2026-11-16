// TestGutterSuite.mm — `make guitest SUITES=test_gutter`: the ▶ beside each
// test (TestGutter.h). A pytest file shows one by each test, in a lane of
// the gutter, with a tooltip naming it; a click saves nothing it need not,
// types that one test's command into the terminal (the setting's template,
// here a stand-in for pytest that fails the TestBroken class) and leaves
// the keyboard in the editor; the mark turns into a pass or a failure when
// the command ends, by its exit status. Run Test at Cursor runs the test
// the caret is in; an edit to the file clears what the runs said and moves
// the marks with their lines.
#import "Harness.h"
#import "EditorController.h"
#import "Gutter.h"
#import "Terminal.h"
#import "TerminalPanel.h"
#import "TestGutter.h"
#include <sys/stat.h>

namespace {
NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
MCTerminalPanel *Panel(EditorController *c) { return [c valueForKey:@"terminalPanel"]; }
MCTestGutter *Tests(EditorController *c) { return [c valueForKey:@"testGutter"]; }
MCLineGutter *GutterOf(EditorController *c) {
    return (MCLineGutter *)Text(c).enclosingScrollView.verticalRulerView;
}
NSEvent *Click(NSWindow *w, NSPoint at) {
    return [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:at modifierFlags:0
                             timestamp:0 windowNumber:w.windowNumber context:nil
                           eventNumber:0 clickCount:1 pressure:1];
}
// A press on test `i`'s mark, as the pointer would make it.
void ClickTest(EditorController *c, NSInteger i) {
    MCLineGutter *g = GutterOf(c);
    const NSRect r = [g rectOfTest:i];
    GCHECK(!NSIsEmptyRect(r));
    [g mouseDown:Click(c.window, [g convertPoint:NSMakePoint(NSMidX(r), NSMidY(r)) toView:nil])];
}
NSArray<NSNumber *> *States(EditorController *c) { return [Tests(c) testStates]; }
BOOL Idle(TerminalView *t) { return t && t.atPrompt && !t.busy; }
}  // namespace

MC_GUI_SUITE(test_gutter) {
    __block EditorController *c = nil;
    __block TerminalView *term = nil;
    __block NSUInteger looks = 0;
    NSString *dir = MCGuiProjectFile(@"tested");
    NSString *py = [dir stringByAppendingPathComponent:@"test_calc.py"];
    NSString *fake = [dir stringByAppendingPathComponent:@"fake-pytest"];

    MCGuiStep(@"a pytest file and a stand-in for pytest", ^{
        MCGuiWriteFile(py, @"def test_adds():\n"
                           @"    assert 1 + 1 == 2\n"
                           @"\n"
                           @"\n"
                           @"class TestBroken:\n"
                           @"    def test_fails(self):\n"
                           @"        assert False\n");
        // Says what it was asked to run; TestBroken fails.
        MCGuiWriteFile(fake, @"#!/bin/sh\n"
                             @"echo \"fake-pytest ran: $*\"\n"
                             @"case \"$*\" in *TestBroken*) echo 'fake: 1 failed'; exit 1;; esac\n"
                             @"echo 'fake: 1 passed'\n");
        chmod(fake.fileSystemRepresentation, 0755);
        MCGuiSetSettings([NSString stringWithFormat:@"lsp.enabled = false\n"
                                                    @"test.python = '%@' {tests}\n", fake]);
        c = MCGuiController();
        // A terminal of its own, at a prompt, so what earlier suites left
        // in theirs plays no part.
        if (!Panel(c)) [c toggleTerminal:nil];
        term = [Panel(c) addTabInDirectory:MCGuiProject()];
        [c focusEditor:nil];
        GCHECK([c showFile:py]);
        [c.window makeFirstResponder:Text(c)];
    });
    MCGuiWait(@"its shell at the prompt", 20, ^BOOL { return Idle(term); });
    MCGuiWait(@"a mark by each test", 10, ^BOOL {
        return [[Tests(c) testLabels] isEqualToArray:@[@"test_adds", @"TestBroken::test_fails"]] &&
               GutterOf(c).tests.size() == 2;
    });
    MCGuiStep(@"in the test lane, on the tests' lines, each with its tooltip", ^{
        MCLineGutter *g = GutterOf(c);
        GCHECK(g.testLane);
        GCHECK(g.tests[0].line == 0 && g.tests[1].line == 5);
        GCHECK(([States(c) isEqualToArray:@[@(MCTestIdle), @(MCTestIdle)]]));
        const NSRect r = [g rectOfTest:1];
        GCHECK_EQ_STR([g testTipAtPoint:NSMakePoint(NSMidX(r), NSMidY(r))],
                      @"Run TestBroken::test_fails");
        // The lane is left of the numbers, inside the gutter's width.
        GCHECK(NSMaxX(r) <= g.laneWidth && g.laneWidth <= NSWidth(g.bounds));
        MCGuiSnapshot(Text(c).enclosingScrollView, @"test-gutter-idle");
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Run Test at Cursor"
                                                      action:@selector(runTestAtCursor:)
                                               keyEquivalent:@""];
        GCHECK([c validateMenuItem:item]);
        [c.window makeFirstResponder:Text(c)];
        GCHECK(Panel(c).activeTerminal == term);
        ClickTest(c, 0);
        GCHECK(c.window.firstResponder == Text(c));
        GCHECK([Tests(c).lastCommand containsString:@"tested/test_calc.py::test_adds"]);
        MCGuiNote(@"typed: %@", Tests(c).lastCommand);
    });
    MCGuiWait(@"the test ran in the terminal and passed", 20, ^BOOL {
        return [[term recentText:8] containsString:@"fake-pytest ran: tested/test_calc.py::test_adds"] &&
               [States(c)[0] integerValue] == MCTestPassed;
    });
    MCGuiStep(@"a check by the passed test; the other untouched", ^{
        GCHECK([States(c)[1] integerValue] == MCTestIdle);
        MCLineGutter *g = GutterOf(c);
        const NSRect r = [g rectOfTest:0];
        GCHECK_EQ_STR([g testTipAtPoint:NSMakePoint(NSMidX(r), NSMidY(r))],
                      @"test_adds passed. Click to run it again.");
        GCHECK(c.window.firstResponder == Text(c));
    });
    MCGuiWait(@"back at the prompt", 10, ^BOOL { return Idle(term); });
    MCGuiStep(@"the failing test's mark", ^{ ClickTest(c, 1); });
    MCGuiWait(@"it ran and failed", 20, ^BOOL {
        return [[term recentText:8] containsString:@"fake-pytest ran: tested/test_calc.py::TestBroken::test_fails"] &&
               [States(c)[1] integerValue] == MCTestFailed;
    });
    MCGuiStep(@"a cross by the failed test; the pass still shows", ^{
        GCHECK([States(c)[0] integerValue] == MCTestPassed);
        MCLineGutter *g = GutterOf(c);
        const NSRect r = [g rectOfTest:1];
        GCHECK_EQ_STR([g testTipAtPoint:NSMakePoint(NSMidX(r), NSMidY(r))],
                      @"TestBroken::test_fails failed. Click to run it again.");
        [g display];
        MCGuiSnapshot(Text(c).enclosingScrollView, @"test-gutter-results");
    });
    MCGuiWait(@"the prompt again", 10, ^BOOL { return Idle(term); });
    MCGuiStep(@"Run Test at Cursor, the caret in test_fails's body", ^{
        NSString *s = Text(c).string;
        const NSRange body = [s rangeOfString:@"assert False"];
        Text(c).selectedRange = NSMakeRange(body.location, 0);
        [c runTestAtCursor:nil];
        GCHECK([Tests(c).lastCommand hasSuffix:@"tested/test_calc.py::TestBroken::test_fails"]);
        GCHECK([States(c)[1] integerValue] == MCTestRunning);
    });
    MCGuiWait(@"it failed again", 20, ^BOOL {
        return [States(c)[1] integerValue] == MCTestFailed && Idle(term);
    });
    MCGuiStep(@"an edit: the marks move with their lines and the results go", ^{
        looks = Tests(c).looks;
        MCGuiUndoGroup(Text(c).undoManager, ^{
            Text(c).selectedRange = NSMakeRange(0, 0);
            [Text(c) insertText:@"import os\n" replacementRange:Text(c).selectedRange];
        });
        GCHECK(GutterOf(c).tests[0].line == 1);
    });
    MCGuiWait(@"the tests found again", 10, ^BOOL { return Tests(c).looks > looks; });
    MCGuiStep(@"each test's ▶ again, a line down", ^{
        GCHECK(([States(c) isEqualToArray:@[@(MCTestIdle), @(MCTestIdle)]]));
        GCHECK(GutterOf(c).tests[0].line == 1 && GutterOf(c).tests[1].line == 6);
        [c saveCurrentFile:nil];
    });
    MCGuiStep(@"a file that is not a test file has no lane", ^{
        NSString *plain = [dir stringByAppendingPathComponent:@"calc.py"];
        MCGuiWriteFile(plain, @"def test_like():\n    pass\n");
        GCHECK([c showFile:plain]);
    });
    MCGuiWait(@"no lane", 10, ^BOOL { return !GutterOf(c).testLane; });
    MCGuiStep(@"nothing to run there", ^{
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Run Test at Cursor"
                                                      action:@selector(runTestAtCursor:)
                                               keyEquivalent:@""];
        GCHECK(![c validateMenuItem:item]);
        GCHECK([Panel(c) closeTerminal:term]);
    });
}
