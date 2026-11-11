// ControlServer.h — the control channel's socket, on the GLib main loop
// (PARITY.md 3.1; the Mac's is src/ControlServer.mm). What an agent, a script
// or `minicode open|window|state|selection` asks arrives here as one JSON
// object a line (ControlProtocol.h) and goes to the Dispatcher, whose
// handlers run on the main loop; ControlCommands.cpp registers them.
//
// The socket is <folder>/<pid>.sock, 0600, in a 0700 folder of the user's own
// (ControlPaths.h: $MINICODE_CONTROL_DIR, else $XDG_RUNTIME_DIR/minicode,
// else /tmp/minicode-<uid>, which is refused when it is not the user's own
// and private). Peers of another user are refused (SO_PEERCRED). A
// connection takes one request at a time: the next line is read once the
// answer is out, so a client cannot queue work faster than it is answered.
// Writes never block: an answer the peer is slow to take waits for the main
// loop, and a peer that takes nothing for ten seconds is dropped. The
// socket goes at exit, and sockets of dead processes (and of live pids where
// nothing listens) are removed when the server starts.
#pragma once

#include <string>

#include "ControlProtocol.h"

class ControlServer {
public:
    ControlServer();
    ~ControlServer();   // stops it
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    ControlChannel::Dispatcher& dispatcher() { return dispatcher_; }

    // Makes the folder, removes stale sockets, listens. False, with the
    // reason, when it cannot (the app then runs without the channel).
    bool start(std::string* error);
    void stop();
    const std::string& path() const { return path_; }

    // Removes the sockets in `folder` that belong to dead processes, or to a
    // live pid (not `self`) where nothing listens. Public for the checks.
    static int removeStale(const std::string& folder, int self);

    struct Impl;

private:
    ControlChannel::Dispatcher dispatcher_;
    std::string path_;
    Impl* impl_ = nullptr;
};
