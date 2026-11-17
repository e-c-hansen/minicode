// SessionLink.h — the GTK app's session host and its one client
// (SESSION-DESIGN.md, stage 4a; the core's SessionHost.h and
// SessionClient.h). The host runs on a thread of its own and owns every
// open document's text and undo history; the editor's documents are its
// mirrors (Document.h records each buffer edit into the client, applies
// what the host sends back, and asks the host for undo), and the language
// server sync reads the host's text through the client's copy of it
// (LspSessionSync.cpp). The windows' tabs are reported as they change.
//
// One client for the whole process, in code points (what GtkTextIter
// counts), its messages dispatched on the main loop as they arrive.
//
// MINICODE_HOST=loopback puts every message through the protocol's
// encoding and back, both ways, so the GUI checks exercise the wire format
// before any network carries it; MINICODE_HOST_DELAY_US delays each
// message on the host's thread, as a slow link would.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "SessionClient.h"

namespace Model {
class Window;
}

namespace SessionLink {

// The client, with the host started on first use.
Shared::Client& client();
bool loopback();

// A window's tabs, sent when they differ from what was sent last.
void windowTabs(std::uint64_t window, const Model::Window& w);
void windowClosed(std::uint64_t window);

// For the GUI checks: whether the host's text of each of `docs` (a
// document's id and its buffer's text) is the buffer's, the window's tabs
// as the host has them are the window's own, and the host reported nothing
// wrong. Empty when so, else what differs.
std::string check(std::uint64_t window, const Model::Window& w,
                  const std::vector<std::pair<std::uint64_t, std::string>>& docs);

}  // namespace SessionLink
