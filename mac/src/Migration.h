// Migration.h — the first launch after minicode became mutible (1.6.0).
// Copies what the old name kept (Application Support, ~/.config, the
// browser's site data and cookies, the defaults domain of the old bundle
// id) to the new places, leaves the old copies, and writes a marker so it
// runs once. The paths and rules are the core's (src/Rename.h).
#import <Foundation/Foundation.h>

// Runs the migration when it has not run yet. Called first thing in main,
// before anything reads the defaults; only in the real bundle (its id is
// the new one), never in the GUI checks, a demo or a benchmark.
void MCMigrateFromOldName(void);
