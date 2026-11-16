// EditorController+Tests.mm — EditorController: the ▶ beside each test in
// a test file (TestGutter.h), whose host it is, and Run Test at Cursor in
// the Terminal menu. A run goes the way Run File's does (+Panels'
// runFileCommand:inDirectory:busyNote:).
#import "EditorController+Private.h"
#import "EditorController+TestsIvars.h"
#import "Lsp.h"
#import "RunCommand.h"
#import "TestGutter.h"

@implementation EditorController (Tests)

- (MCTestGutter *)ensureTestGutter {
    if (!_testGutter && self.textView && self.gutter) {
        _testGutter = [[MCTestGutter alloc] initWithHost:self];
        __weak EditorController *weakSelf = self;
        _testGutter.onNote = ^(NSString *text) { [weakSelf showStatusNote:text]; };
    }
    return _testGutter;
}

- (MCTestGutter *)testGutter { return _testGutter; }   // tests, by KVC

// ---------------------------------------------------------------- the host
- (CodeTextView *)testGutterTextView { return self.textView; }
- (MCLineGutter *)testGutterView { return self.gutter; }
- (NSString *)testGutterPath {
    if (!self.currentPath || !self.textView.editable || self.previewMode || self.isDiff ||
        [self largeFileFeaturesOff])
        return nil;
    return self.currentPath;
}
- (NSString *)testGutterRoot { return self.rootPath; }

- (NSString *)testGutterPythonFor:(NSString *)path {
    // As Run File: the interpreter the Python server uses, else the
    // project's virtualenv, else python3.
    NSString *python = [self.lsp.currentLanguage isEqualToString:@"python"]
                           ? [self.lsp pythonInterpreterWithSource:nil] : nil;
    return python.length ? python : MCPythonInterpreterFor(path, self.rootPath);
}

- (TerminalView *)testGutterRun:(NSString *)line inDirectory:(NSString *)dir {
    return [self runFileCommand:line
                    inDirectory:dir
                       busyNote:@"The terminal was busy, so the test runs in a new one"];
}

// ------------------------------------------------------- Run Test at Cursor
- (void)runTestAtCursor:(id)sender {
    (void)sender;
    if (![[self ensureTestGutter] runTestAtCaret]) return;
}

- (BOOL)validateTestItem:(NSMenuItem *)item enabled:(BOOL *)enabled {
    if (item.action != @selector(runTestAtCursor:)) return NO;
    *enabled = _testGutter.canRunTestAtCaret;
    return YES;
}

@end
