// ControlPaths.h — header only, plain C++. Where the control channel's
// sockets live on Linux, for the app (linux/src/ControlServer.cpp) and the
// `minicode` client (tools/minicode-cli.cpp) alike. The Mac keeps its own
// rule (a "MiniCode" folder in $TMPDIR, socketFolders() below). A headless
// session host (SessionServer.h) puts its socket in the same folder.
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
//
// After the rename (src/Rename.h) the client also looks in the folder a
// copy from before 1.6.0 uses, after the new one, so `mutible` reaches an
// old copy still running; a server only ever uses the first folder.
#pragma once
#include "Rename.h"
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

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

// Every folder a socket of this user's may be in, in the order looked at;
// a server makes its socket in the first. On the Mac the app makes its
// socket in $TMPDIR/MiniCode, and Launch Services starts it with TMPDIR set
// to the user's own temporary folder, which confstr gives whatever the
// shell's TMPDIR says (a nix shell, a dotfile's TMPDIR=/tmp). So that folder
// is always looked in, after the shell's TMPDIR when that is another one.
// MINICODE_CONTROL_DIR names the one folder to use instead (tests).
inline std::vector<std::string> currentSocketFolders() {
#ifndef __APPLE__
    return {socketFolder(Rename::getenvOrOld("MINICODE_CONTROL_DIR"), getenv("XDG_RUNTIME_DIR"),
                         getenv("TMPDIR"), (unsigned)getuid())};
#else
    const char* forced = Rename::getenvOrOld("MINICODE_CONTROL_DIR");
    if (forced && *forced) return {trimTrailingSlashes(forced)};
    std::vector<std::string> out;
    const char* t = getenv("TMPDIR");
    if (t && *t) out.push_back(trimTrailingSlashes(t) + "/MiniCode");
    char buf[PATH_MAX];
    size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof buf);
    std::string user = n > 0 && n <= sizeof buf ? trimTrailingSlashes(buf) + "/MiniCode" : "";
    if (!user.empty()) {
        char a[PATH_MAX], b[PATH_MAX];
        bool same = false;
        for (const std::string& o : out)
            same = same || o == user ||
                   (realpath(o.c_str(), a) && realpath(user.c_str(), b) && strcmp(a, b) == 0);
        if (!same) out.push_back(user);
    }
    if (out.empty()) out.push_back("/tmp/MiniCode");
    return out;
#endif
}

// The folders above, then the ones a copy from before the rename would use
// (Rename::oldSocketFolder); a forced folder alone.
inline std::vector<std::string> socketFolders() {
    std::vector<std::string> out = currentSocketFolders();
    const char* forced = Rename::getenvOrOld("MINICODE_CONTROL_DIR");
    if (forced && *forced) return out;
    const size_t now = out.size();
    for (size_t i = 0; i < now; i++) {
        const std::string old = Rename::oldSocketFolder(out[i]);
        bool seen = old.empty();
        for (const std::string& o : out) seen = seen || o == old;
        if (!seen) out.push_back(old);
    }
    return out;
}

}  // namespace ControlChannel
