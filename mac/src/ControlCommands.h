// ControlCommands.h — what each control channel command does in the app:
// the handlers registered with the protocol's Dispatcher (ControlProtocol.h),
// on the main thread, over the windows AppDelegate lists (ControlServer.h).
// Nothing here closes, replaces or reorders a tab or terminal the user has,
// or moves the keyboard: commands only add (the agent preview tab is the
// agent's own).
#import <Cocoa/Cocoa.h>
#include "ControlProtocol.h"

@class EditorController, TerminalView;
@protocol MCControlAppHost;

void MCControlRegisterCommands(ControlChannel::Dispatcher &dispatcher, id<MCControlAppHost> host);

// `browse` and `page`, in ControlPageCommands.mm (registered by the above).
void MCControlRegisterPageCommands(ControlChannel::Dispatcher &dispatcher, id<MCControlAppHost> host);
// `state`'s list of a window's browser pages: the user's, and the agent's.
Json MCControlBrowserPages(EditorController *controller);
// The window a command acts in: --window, else --folder (opened if no window
// shows it), else `fallback`, else the frontmost; nil with `error` set.
EditorController *MCControlTarget(id<MCControlAppHost> host, const Json &args,
                                  EditorController *fallback, std::string &error);

// Ids that stay the same for the life of a window or terminal, given out on
// first use (`minicode state` lists them).
NSInteger MCControlWindowID(EditorController *controller);
NSInteger MCControlTerminalID(TerminalView *terminal);
