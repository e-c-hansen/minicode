// StatusLineOffer.h — Objective-C++. Installing and undoing `mutible
// statusline` as Claude Code's status line (A13, ClaudeStatusLine.h), from
// the Agents panel's plan usage section (UsageView.h). It follows the
// waiting dots' hooks (ClaudeHooksOffer.h) exactly: a question that shows
// what goes into Claude Code's settings file, a write only on the second
// yes (a backup first, every other setting kept), then where it went. The
// user's own status line keeps running, chained from the wrapper, and undo
// puts it back as it was.
#import <Cocoa/Cocoa.h>

// mutible's folder for the usage log ($MUTIBLE_USAGE_DIR, else
// ~/Library/Application Support/Mutible/usage).
NSString *MCUsageDir(void);
// The command installed: `mutible statusline` when `mutible` is found
// (MCFindProgram), else the bundle's launcher by its full path.
NSString *MCClaudeStatusLineCommand(void);
// Is the wrapper Claude Code's status line now?
BOOL MCClaudeStatusLineInstalled(void);

// The question (Log Plan Usage, Cancel), the write, then what happened.
// Sheets on `window` when given; `done` gets whether it is installed after.
void MCInstallClaudeStatusLine(NSWindow *window, void (^done)(BOOL installed));
// Undo: the question (Put It Back, Cancel), the write, then what happened.
// `done` gets whether it is still installed after.
void MCUninstallClaudeStatusLine(NSWindow *window, void (^done)(BOOL installed));
