// Environment.h — the process's environment for the Mac app, read from the
// core's ProcessEnv snapshot (src/ProcessEnv.h), never from `environ`.
//
// Any thread that starts a program (git on the panel's queue, a formatter,
// a language server, mmdc, tectonic, the inline agent, a shell) builds the
// child's environment from MCEnvironment() or a ProcessEnv::Block. Reading
// NSProcessInfo.environment or walking `environ` off the main thread raced
// with setenv in the GUI checks: libc reallocates the array under the
// reader. Variables change after startup only through MCSetEnv, on the main
// thread (docs/notes/build.md, "The environment").
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// The environment as it is now, as a dictionary. Any thread. The same
// object is returned until the environment changes.
NSDictionary<NSString *, NSString *> *MCEnvironment(void);

// One variable's value, or nil when it is not set. Any thread.
NSString *_Nullable MCEnv(NSString *name);

// Sets (or with nil unsets) a variable and refreshes the snapshot. Main
// thread only; the GUI checks change variables only through this.
void MCSetEnv(const char *name, const char *_Nullable value);

NS_ASSUME_NONNULL_END
