// ControlServer.h — the control channel's socket. Each running minicode
// listens on $TMPDIR/MiniCode/<pid>.sock (mode 0600, in a 0700 folder) for
// the `minicode` command line (tools/minicode-cli.cpp), an agent, or a script
// the user asked for. Peers running as another user are turned away
// (getpeereid). The socket goes when the app quits, and sockets left by a
// minicode that died (kill -9) are removed when the next one starts.
//
// Requests are read off the main thread and handled on it, one JSON line in,
// one out (ControlProtocol.h for the protocol, ControlCommands.mm for what
// each command does).
#import <Cocoa/Cocoa.h>
#include <sys/types.h>

@class EditorController, TerminalView;

// What the commands need of the application: its windows, and opening one.
// AppDelegate (main.mm) answers.
@protocol MCControlAppHost <NSObject>
- (NSArray<EditorController *> *)controlControllers;   // every editor window
- (EditorController *)controlFrontController;          // the frontmost, or nil
// The window whose folder holds `file` (the innermost), with the file spelled
// from that window's root in *spelled; nil when none does.
- (EditorController *)controlControllerContaining:(NSString *)file
                                          spelled:(NSString **)spelled;
- (EditorController *)controlControllerRootedAt:(NSString *)dir;   // resolved path
- (EditorController *)controlOpenWindowAtPath:(NSString *)dir;
@end

// Starts listening (applicationWillFinishLaunching). Stale sockets of dead
// processes are removed first. Does nothing under MINICODE_CONTROL=off.
void MCControlServerStart(id<MCControlAppHost> host);
// Stops listening and removes the socket (applicationWillTerminate, and an
// atexit handler for any other way out).
void MCControlServerStop(void);
// The socket's path, or nil when not listening.
NSString *MCControlSocketPath(void);
// The folder the sockets live in: $TMPDIR/MiniCode.
NSString *MCControlSocketFolder(void);
// Removes sockets in `folder` whose process is gone; returns how many.
NSUInteger MCControlRemoveStaleSockets(NSString *folder);

// One request line handled in this process, as if it came over the socket
// (tests); `done` gets the answer line on the main thread.
void MCControlHandleLine(NSString *line, void (^done)(NSString *answer));

// Tests: the user id a peer must have (normally this process's own).
void MCControlServerSetPeerUIDForTesting(uid_t uid);
