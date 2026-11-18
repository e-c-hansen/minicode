// TestGutter.mm — see TestGutter.h.
#import "TestGutter.h"
#import "AppSettings.h"
#import "Gutter.h"
#import "Lsp.h"
#import "Terminal.h"
#import <QuartzCore/QuartzCore.h>
#include "Runnables.h"
#include <memory>
#include <string>
#include <vector>

// A command typed for some tests, waiting for its terminal to say it ended.
@interface MCTestRun : NSObject
@property(nonatomic, weak) TerminalView *terminal;
@property(nonatomic, copy) NSString *path;
@property(nonatomic, copy) NSString *line;   // the test command, without a cd before it
@property(nonatomic, copy) NSArray<NSString *> *labels;
@end
@implementation MCTestRun
@end

namespace {

NSString *NS(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

std::string Std(NSString *s) {
    const char *p = s.UTF8String;
    return p ? std::string(p) : std::string();
}

std::string Fs(NSString *s) {
    const char *p = s.length ? s.fileSystemRepresentation : nullptr;
    return p ? std::string(p) : std::string();
}

NSString *TipFor(NSString *label, MCTestState state) {
    switch (state) {
        case MCTestIdle: return [NSString stringWithFormat:@"Run %@", label];
        case MCTestRunning: return [NSString stringWithFormat:@"%@ is running", label];
        case MCTestPassed:
            return [NSString stringWithFormat:@"%@ passed. Click to run it again.", label];
        case MCTestFailed:
            return [NSString stringWithFormat:@"%@ failed. Click to run it again.", label];
    }
    return label;
}

}  // namespace

@implementation MCTestGutter {
    __weak id<MCTestGutterHost> _host;
    dispatch_queue_t _queue;
    NSUInteger _generation;            // every edit and every look
    BOOL _scheduled;
    CFTimeInterval _lastEdit;
    BOOL _pauseWanted;
    NSString *_shownPath;              // the file the tests are of
    Runnables::Kind _kind;
    std::vector<Runnables::Test> _tests;
    // What the last runs said, by file and test, until the file changes.
    NSMutableDictionary<NSString *, NSMutableDictionary<NSString *, NSNumber *> *> *_results;
    NSMutableArray<MCTestRun *> *_runs;
}

- (instancetype)initWithHost:(id<MCTestGutterHost>)host {
    if ((self = [super init])) {
        _host = host;
        _queue = dispatch_queue_create("mutible.test-gutter", DISPATCH_QUEUE_SERIAL);
        _kind = Runnables::Kind::None;
        _results = [NSMutableDictionary dictionary];
        _runs = [NSMutableArray array];
        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
        [nc addObserver:self selector:@selector(storageEdited:)
                   name:NSTextStorageDidProcessEditingNotification
                 object:host.testGutterTextView.textStorage];
        [nc addObserver:self selector:@selector(settingsChanged:)
                   name:MCSettingsDidChangeNotification object:nil];
        [nc addObserver:self selector:@selector(commandFinished:)
                   name:MCTerminalCommandDidFinishNotification object:nil];
        [nc addObserver:self selector:@selector(terminalClosed:)
                   name:MCTerminalDidCloseNotification object:nil];
        __weak MCTestGutter *weakSelf = self;
        host.testGutterView.onTest = ^(NSInteger index) { [weakSelf runTestAtIndex:index]; };
        [self scheduleAfter:0.05];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)settingsChanged:(NSNotification *)note {
    (void)note;
    [self refreshSoon];
}

- (void)storageEdited:(NSNotification *)note {
    NSTextStorage *ts = note.object;
    if (!(ts.editedMask & NSTextStorageEditedCharacters)) return;
    _generation++;
    _lastEdit = CACurrentMediaTime();
    NSString *path = _host.testGutterPath;
    // The whole text replaced is another tab put in the view (or the same
    // one shown again); anything less is the file changing, and what its
    // tests' last runs said no longer holds.
    const BOOL whole = ts.editedRange.location == 0 && ts.editedRange.length == ts.length;
    const BOOL same = path && [path isEqualToString:_shownPath];
    if (same && !whole) {
        [_results removeObjectForKey:path];
        NSIndexSet *gone = [_runs indexesOfObjectsPassingTest:^BOOL(MCTestRun *r, NSUInteger i, BOOL *stop) {
            (void)i; (void)stop;
            return [r.path isEqualToString:path];
        }];
        [_runs removeObjectsAtIndexes:gone];
        _pauseWanted = YES;
    }
    [self scheduleAfter:same && !whole ? 0.3 : 0.05];
}

- (void)refreshSoon { [self scheduleAfter:0.05]; }

- (void)scheduleAfter:(NSTimeInterval)delay {
    if (_scheduled) return;
    _scheduled = YES;
    __weak MCTestGutter *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        MCTestGutter *s = weakSelf;
        if (!s) return;
        s->_scheduled = NO;
        // Still typing: wait for the pause (one timer, not one a key).
        const CFTimeInterval since = CACurrentMediaTime() - s->_lastEdit;
        if (s->_pauseWanted && since < 0.3) {
            [s scheduleAfter:0.3 - since + 0.01];
            return;
        }
        s->_pauseWanted = NO;
        [s look];
    });
}

// The kind of test file on show, None when its language's tests are off.
- (Runnables::Kind)kindOf:(NSString *)path {
    if (!path) return Runnables::Kind::None;
    const Runnables::Kind kind = Runnables::kindFor(Fs(path));
    if (kind == Runnables::Kind::None) return kind;
    std::string v = [AppSettings shared].settings.testTemplate(Runnables::languageOf(kind));
    for (char &c : v) c = (char)std::tolower((unsigned char)c);
    if (v == "off" || v == "none" || v == "false") return Runnables::Kind::None;
    return kind;
}

// On the main thread: what to read; on the queue: the tests; back here,
// the marks, if nothing was typed meanwhile.
- (void)look {
    id<MCTestGutterHost> host = _host;
    MCLineGutter *g = host.testGutterView;
    CodeTextView *tv = host.testGutterTextView;
    NSString *path = host.testGutterPath;
    const Runnables::Kind kind = [self kindOf:path];
    const NSUInteger gen = ++_generation;
    if (kind == Runnables::Kind::None || !tv || !g) {
        _shownPath = path;
        _kind = Runnables::Kind::None;
        _tests.clear();
        g.testLane = NO;
        _looks++;
        return;
    }
    NSString *text = [tv.textStorage.string copy];
    __weak MCTestGutter *weakSelf = self;
    dispatch_async(_queue, ^{
        auto found = std::make_shared<std::vector<Runnables::Test>>(Runnables::find(Std(text), kind));
        dispatch_async(dispatch_get_main_queue(), ^{
            MCTestGutter *s = weakSelf;
            if (!s) return;
            if (gen != s->_generation) return;   // typed meanwhile: the next look
            if (![path isEqualToString:s->_host.testGutterPath]) return;
            s->_shownPath = path;
            s->_kind = kind;
            s->_tests = *found;
            s->_host.testGutterView.testLane = YES;
            [s pushMarks];
            s->_looks++;
        });
    });
}

- (NSString *)labelOf:(const Runnables::Test &)t {
    return NS(t.label(_kind));
}

- (void)pushMarks {
    NSDictionary<NSString *, NSNumber *> *said = _shownPath ? _results[_shownPath] : nil;
    std::vector<MCGutterTest> marks;
    for (const Runnables::Test &t : _tests) {
        NSString *label = [self labelOf:t];
        MCGutterTest m;
        m.line = t.line;
        m.state = (MCTestState)[said[label] integerValue];
        m.tip = TipFor(label, m.state);
        marks.push_back(m);
    }
    [_host.testGutterView setTests:marks];
}

// ---- running
- (BOOL)runTestAtIndex:(NSInteger)index {
    if (index < 0 || (size_t)index >= _tests.size()) {
        NSBeep();
        return NO;
    }
    return [self run:std::vector<Runnables::Test>{_tests[(size_t)index]} wholeFile:NO];
}

- (BOOL)runTestAtCaret {
    CodeTextView *tv = _host.testGutterTextView;
    MCLineGutter *g = _host.testGutterView;
    if (!self.canRunTestAtCaret || !tv || !g) {
        NSBeep();
        if (self.onNote) self.onNote(@"No tests here to run");
        return NO;
    }
    const int at = Runnables::testAt(_tests, [g lineOfCharacter:tv.selectedRange.location]);
    if (at < 0) return [self run:_tests wholeFile:YES];
    return [self run:std::vector<Runnables::Test>{_tests[(size_t)at]} wholeFile:NO];
}

- (BOOL)canRunTestAtCaret {
    NSString *path = _host.testGutterPath;
    return _kind != Runnables::Kind::None && path && [path isEqualToString:_shownPath] &&
           !_tests.empty();
}

- (BOOL)run:(const std::vector<Runnables::Test> &)tests wholeFile:(BOOL)whole {
    id<MCTestGutterHost> host = _host;
    NSString *path = host.testGutterPath;
    if (!path || ![path isEqualToString:_shownPath] || _kind == Runnables::Kind::None) {
        NSBeep();
        if (self.onNote) self.onNote(@"Open a test file to run its tests");
        return NO;
    }
    Runnables::Request r;
    r.file = Fs(path);
    r.root = Fs(host.testGutterRoot);
    if (_kind == Runnables::Kind::Js) r.text = Std(host.testGutterTextView.textStorage.string);
    if (_kind == Runnables::Kind::Pytest) r.python = Fs([host testGutterPythonFor:path]);
    r.tmpl = [AppSettings shared].settings.testTemplate(Runnables::languageOf(_kind));
    Runnables::Command c;
    std::string problem;
    if (!Runnables::command(r, _kind, tests, whole, &c, &problem)) {
        NSBeep();
        if (self.onNote) self.onNote(NS(problem));
        return NO;
    }
    NSString *line = NS(c.line);
    NSString *dir = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:c.dir.c_str()
                                                                               length:c.dir.size()];
    TerminalView *term = [host testGutterRun:line inDirectory:dir];
    if (!term) return NO;
    _lastCommand = line;
    NSMutableArray<NSString *> *labels = [NSMutableArray array];
    for (const Runnables::Test &t : tests) [labels addObject:[self labelOf:t]];
    MCTestRun *run = [MCTestRun new];
    run.terminal = term;
    run.path = path;
    run.line = line;
    run.labels = labels;
    [_runs addObject:run];
    NSMutableDictionary *said = _results[path] ?: [NSMutableDictionary dictionary];
    for (NSString *label in labels) said[label] = @(MCTestRunning);
    _results[path] = said;
    [self pushMarks];
    return YES;
}

// A command ended in some terminal: if it was a run of ours, its tests
// passed (exit status 0) or failed.
- (void)commandFinished:(NSNotification *)note {
    TerminalView *t = note.object;
    NSDictionary *info = note.userInfo;
    MCTestRun *done = nil;
    for (MCTestRun *run in _runs) {
        if (run.terminal != t) continue;
        NSString *command = info[MCTerminalCommandKey];
        // Another command in the same terminal (one before ours ran).
        if (info && command.length && ![command containsString:run.line]) continue;
        done = run;
        break;
    }
    if (!done) return;
    [_runs removeObject:done];
    NSMutableDictionary *said = _results[done.path];
    if (!said) return;
    const MCTestState state = !info ? MCTestIdle
                              : [info[MCTerminalExitStatusKey] intValue] == 0 ? MCTestPassed
                                                                                : MCTestFailed;
    for (NSString *label in done.labels) {
        if (state == MCTestIdle) [said removeObjectForKey:label];
        else said[label] = @(state);
    }
    if ([done.path isEqualToString:_shownPath]) [self pushMarks];
}

// A terminal closed with a run in it: nothing more will be said of it.
- (void)terminalClosed:(NSNotification *)note {
    BOOL changed = NO;
    for (MCTestRun *run in [_runs copy]) {
        if (run.terminal && run.terminal != note.object) continue;
        NSMutableDictionary *said = _results[run.path];
        for (NSString *label in run.labels)
            if ([said[label] integerValue] == MCTestRunning) [said removeObjectForKey:label];
        [_runs removeObject:run];
        changed = YES;
    }
    if (changed) [self pushMarks];
}

// ---- for tests
- (NSArray<NSString *> *)testLabels {
    NSMutableArray *a = [NSMutableArray array];
    for (const Runnables::Test &t : _tests) [a addObject:[self labelOf:t]];
    return a;
}

- (NSArray<NSNumber *> *)testStates {
    NSMutableArray *a = [NSMutableArray array];
    for (const MCGutterTest &m : _host.testGutterView.tests) [a addObject:@(m.state)];
    return a;
}

@end
