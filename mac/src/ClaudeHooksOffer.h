// ClaudeHooksOffer.h — Objective-C++. The waiting dots (TerminalAttention.h)
// need Claude Code's hooks to run `mutible notify --from-hook`; this is
// where the app offers them, so nobody has to know `mutible hooks
// --install`:
//   - a bar over the terminals the first time Claude Code starts in one
//     (MCTerminalClaudeDidStartNotification), once per launch, unless the
//     hooks are in already or Not Now was pressed once (kept in the app's
//     defaults, as the install bar's Don't Show Again);
//   - mutible > Install Claude Code Hooks… (mutible.installClaudeHooks),
//     and a line in the Agents panel's header while they are missing.
// Each shows exactly what goes into Claude Code's settings file and writes
// only on a second yes, through the core's ClaudeHooks::install (a backup
// of the file first, every key and hook of the user's kept), then says
// where it wrote. A press when they are in says so and writes nothing.
// This is the one place mutible writes under ~/.claude, and only after
// those two presses (the owner's decision, 2026-11-15).
#import <Cocoa/Cocoa.h>

@class TerminalView;

// Claude Code's settings file: $CLAUDE_CONFIG_DIR/settings.json, else
// ~/.claude/settings.json (HOME's, as Claude Code reads it).
NSString *MCClaudeSettingsPath(void);
// The command the hooks run: `mutible notify --from-hook` when `mutible`
// is found (MCFindProgram), else the bundle's launcher by its full path.
NSString *MCClaudeHooksCommand(void);
// Every event's hook is in the settings file already.
BOOL MCClaudeHooksInstalled(void);
// The text the first question shows: what would be added, as JSON; nil when
// nothing would (installed, or a file merge refuses).
NSString *MCClaudeHooksPreview(void);

// Install Claude Code Hooks…: the question with what would be added
// (Add the Hooks, Cancel), then the write, then where it went (or why the
// file was left alone). Sheets on `window` when given. `done` gets YES when
// the hooks are in afterwards (written now or already there).
void MCInstallClaudeHooks(NSWindow *window, void (^done)(BOOL installed));

// The bar over `terminal`'s panel, if it is to be offered now (not muted,
// not installed, not offered yet this launch). Returns whether it showed.
// The notification calls it; tests may too.
BOOL MCOfferClaudeHooksForTerminal(TerminalView *terminal);
// The bar's key in the install bar's muted list ("claude-hooks").
extern NSString *const MCClaudeHooksOfferKey;
