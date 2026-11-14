// IdeLinkServer.h — the Claude Code IDE link on macOS (agent.ide-link):
// each editor window answers Claude Code's `/ide` as VS Code's extension
// does, over the protocol in IdeLink.h.
//
// While the setting is on, every window listens on 127.0.0.1 (a port the
// system picks) and writes ~/.claude/ide/<port>.lock ($CLAUDE_CONFIG_DIR/ide
// when that is set; mode 0600, written whole and renamed into place) naming
// its folder, minicode's pid and a fresh secret. Claude Code started in that
// folder offers the window under /ide; started in one of the window's
// terminals (which get CLAUDE_CODE_SSE_PORT), it joins by itself. A peer
// without the secret is turned away before anything is said. The lock file
// goes when the window closes, the setting goes off or minicode quits
// (atexit too); lock files minicode left behind when it died are removed at
// the next start, and every other one (VS Code's, a running minicode's) is
// left alone. Nothing else is written under ~/.claude.
//
// What Claude Code gets (each tool keeps to the control channel's rules:
// nothing closes, replaces or saves the user's tabs, and the keyboard stays
// where the user has it):
//   - the selection: selection_changed as it moves (0.3 s after it settles),
//     getCurrentSelection and getLatestSelection;
//   - getDiagnostics: the language servers' problems (the Problems list);
//   - openFile: the file in a tab (the agent preview tab with preview),
//     beside the user's while they type, its startText..endText highlighted;
//   - openDiff: the proposed text against the file on disk, side by side,
//     with Accept and Reject over it; Claude Code writes the file itself
//     when accepted. close_tab and closeAllDiffTabs close only these tabs;
//   - getOpenEditors, getWorkspaceFolders, checkDocumentDirty.
// Not offered: saveDocument (an agent does not save the user's buffer) and
// executeCode (nothing runs by itself).
#import <Cocoa/Cocoa.h>

@class EditorController;

// One window's link.
@interface MCIdeLink : NSObject
@property(nonatomic, readonly) int port;
@property(nonatomic, readonly, copy) NSString *lockPath;
@property(nonatomic, readonly, copy) NSString *authToken;     // tests
@property(nonatomic, readonly) NSUInteger connectionCount;    // peers past the handshake
@property(nonatomic, readonly) NSUInteger clientCount;        // those that said initialize
@end

// The folder lock files go in: $CLAUDE_CONFIG_DIR/ide, else ~/.claude/ide.
NSString *MCIdeLinkFolder(void);
// Removes lock files in `folder` that minicode left behind (its ideName, a
// pid that is not this process and no longer runs, owned by this user);
// returns how many.
NSUInteger MCIdeLinkRemoveStaleLocks(NSString *folder);

// A window was made (EditorController's init): it gets a link while the
// setting is on, follows the setting from then on, and loses it as it closes.
void MCIdeLinkWindowOpened(EditorController *controller);
// The window's link, or nil while it has none.
MCIdeLink *MCIdeLinkFor(EditorController *controller);
// Why the link is not running when it should be (no ~/.claude, a socket
// error), or nil.
NSString *MCIdeLinkProblem(void);

// For a shell started in `dir`: CLAUDE_CODE_SSE_PORT=<port> of the link of
// the window whose folder holds `dir` (the innermost), so Claude Code
// started there joins that window; empty when there is none.
NSArray<NSString *> *MCIdeLinkTerminalEnvironment(NSString *dir);

// View > Claude Code IDE Link: turns the setting on and off, with its check
// mark (its own target, as the Theme menu has).
NSMenuItem *MCIdeLinkMenuItem(void);
