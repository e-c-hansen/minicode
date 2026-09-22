// Bench.h — the MiniCode side of scripts/membench.sh.
//
// Compiled into every build but inert: nothing runs unless MINICODE_BENCH is
// set. It puts the window into the benchmark's workload the way a person
// would leave it: a C++ file open (so its language server starts), a LaTeX
// file typeset in the preview, the terminal open, and a page in the browser.
// Then it prints "minicode bench: ready" to stderr and leaves the app running
// for the script to measure and stop.
#import <Cocoa/Cocoa.h>

@class EditorController;

// MINICODE_BENCH_CPP, MINICODE_BENCH_TEX: files to open (absolute paths).
// MINICODE_BENCH_URL: the page for the browser panel.
void MCBenchStart(EditorController *controller);
BOOL MCBenchActive(void);
