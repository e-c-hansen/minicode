// Bench.mm — see Bench.h.
#import "Bench.h"
#import "EditorController.h"

static void After(double seconds, dispatch_block_t block) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), block);
}

static NSString *Env(const char *name) {
    const char *v = getenv(name);
    return v && *v ? @(v) : nil;
}

BOOL MCBenchActive(void) { return Env("MINICODE_BENCH") != nil; }

void MCBenchStart(EditorController *c) {
    if (!MCBenchActive()) return;
    // Resolved the way main.mm resolves the folder it was given, or the tree
    // (rooted at /tmp/...) could not find a file named as /private/tmp/....
    NSString *cpp = Env("MINICODE_BENCH_CPP").stringByResolvingSymlinksInPath;
    NSString *tex = Env("MINICODE_BENCH_TEX").stringByResolvingSymlinksInPath;
    NSString *url = Env("MINICODE_BENCH_URL");
    // A step every couple of seconds, as a person would click through. The
    // C++ file, terminal and browser share the first window; the LaTeX
    // preview gets a second one, the way the comparison shows its PDF in
    // Preview beside the editor.
    After(1, ^{ if (cpp) [c revealPath:cpp andOpen:YES]; });
    After(3, ^{ [c toggleTerminal:nil]; });
    After(5, ^{
        if (!url) return;
        [c toggleBrowser:nil];
        id browser = [(id)c valueForKey:@"browser"];
        SEL go = NSSelectorFromString(@"navigateToString:");
        if ([browser respondsToSelector:go]) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
            [browser performSelector:go withObject:url];
#pragma clang diagnostic pop
        }
    });
    After(7, ^{
        if (!tex) return;
        SEL open = NSSelectorFromString(@"openWindowAtPath:");
        id app = NSApp.delegate;
        if (![app respondsToSelector:open]) return;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
        EditorController *second = [app performSelector:open withObject:c.rootPath];
#pragma clang diagnostic pop
        After(1, ^{ [second revealPath:tex andOpen:YES]; });
    });
    After(10, ^{ fprintf(stderr, "minicode bench: ready\n"); });
}
