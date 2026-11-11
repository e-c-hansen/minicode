// ControlPaths.h — header only, plain C++. Where the control channel's
// sockets live on Linux, for the app (linux/src/ControlServer.cpp) and the
// `minicode` client (tools/minicode-cli.cpp) alike. The Mac keeps its own
// rule (a "MiniCode" folder in $TMPDIR, see the CLI).
//
//   $MINICODE_CONTROL_DIR            when set (tests), that folder
//   $XDG_RUNTIME_DIR/minicode        the normal place: /run/user/<uid>, which
//                                    systemd makes 0700 and removes at logout
//   <tmp>/minicode-<uid>             when no runtime folder is set: a folder
//                                    of the user's own in /tmp (or $TMPDIR)
//
// Whichever it is, the server makes the folder 0700 and uses it only when it
// is a real folder of the user's own that nobody else can enter; the client
// looks only in such a folder.
#pragma once
#include <string>

namespace ControlChannel {

inline std::string trimTrailingSlashes(std::string dir) {
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    return dir;
}

// The folder for the sockets; the arguments are the environment's values
// (null when unset) and the user id, so the choice can be tested.
inline std::string socketFolder(const char* forced, const char* xdgRuntime, const char* tmpDir,
                                unsigned uid) {
    if (forced && *forced) return trimTrailingSlashes(forced);
    if (xdgRuntime && xdgRuntime[0] == '/')
        return trimTrailingSlashes(xdgRuntime) + "/minicode";
    return trimTrailingSlashes(tmpDir && tmpDir[0] == '/' ? tmpDir : "/tmp") + "/minicode-" +
           std::to_string(uid);
}

}  // namespace ControlChannel
