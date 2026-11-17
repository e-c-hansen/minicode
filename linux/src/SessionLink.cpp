// SessionLink.cpp — see SessionLink.h.
#include "SessionLink.h"

#include <glib.h>

#include <atomic>
#include <cstdlib>
#include <map>
#include <memory>

#include "EditorTabs.h"
#include "SessionHost.h"
#include "SessionProtocol.h"

namespace SessionLink {

namespace {

// Both live as long as the process: the host's thread waits for messages
// until the app exits, and no destructor runs at exit to join it.
Shared::SessionHost* gHost = nullptr;
Shared::Client* gClient = nullptr;
Shared::ClientId gId = 0;
bool gLoopback = false;
std::atomic<bool> gDispatchQueued{false};
// What each window's tabs were last sent as.
std::map<std::uint64_t, std::string> gSentTabs;

Shared::Message wire(Shared::Message&& m) { return gLoopback ? Shared::roundTrip(m) : std::move(m); }

gboolean dispatchOnMain(gpointer) {
    gDispatchQueued = false;
    gClient->dispatch();
    for (const std::string& e : gClient->takeErrors())
        g_printerr("minicode: the session's host: %s\n", e.c_str());
    return G_SOURCE_REMOVE;
}

void start() {
    const char* mode = std::getenv("MINICODE_HOST");
    gLoopback = mode && std::string(mode) == "loopback";
    const char* delay = std::getenv("MINICODE_HOST_DELAY_US");
    const unsigned delayUs = delay ? (unsigned)std::strtoul(delay, nullptr, 10) : 0;
    if (gLoopback)
        g_printerr("minicode: the session's host: every message through the protocol's encoding\n");
    if (delayUs) g_printerr("minicode: the session's host: each message %u us late\n", delayUs);
    gClient = new Shared::Client(Model::Unit::CodePoint, "gtk");
    // The host's messages arrive on its thread; the main loop dispatches
    // them, once per batch.
    gClient->setWake([] {
        if (!gDispatchQueued.exchange(true)) g_idle_add_full(G_PRIORITY_DEFAULT, dispatchOnMain, nullptr, nullptr);
    });
    gHost = new Shared::SessionHost(
        [](Shared::ClientId, Shared::Message&& m) { gClient->deliver(wire(std::move(m))); }, delayUs);
    gId = gHost->connect();
    gClient->connect([](Shared::Message&& m) { gHost->post(gId, wire(std::move(m))); });
}

}  // namespace

Shared::Client& client() {
    if (!gClient) start();
    return *gClient;
}

bool loopback() {
    client();
    return gLoopback;
}

void windowTabs(std::uint64_t window, const Model::Window& w) {
    Json tabs = Shared::windowJson(w);
    std::string dump = tabs.dump();
    auto it = gSentTabs.find(window);
    if (it != gSentTabs.end() && it->second == dump) return;
    gSentTabs[window] = std::move(dump);
    client().windowTabs(window, std::move(tabs));
}

void windowClosed(std::uint64_t window) {
    gSentTabs.erase(window);
    client().windowClosed(window);
}

std::string check(std::uint64_t window, const Model::Window& w,
                  const std::vector<std::pair<std::uint64_t, std::string>>& docs) {
    Shared::Client& c = client();
    std::string out;
    if (!c.settle()) return "the session's host did not answer";
    for (const auto& [id, text] : docs) {
        if (!c.isOpen(id)) {
            out += "document " + std::to_string(id) + " is not open on the host; ";
            continue;
        }
        const Shared::Client::HostDoc h = c.hostDocument(id);
        if (!h.open) out += "the host has no document " + std::to_string(id) + "; ";
        else if (Model::utf8FromUtf16(h.text) != text)
            out += "the host's text of document " + std::to_string(id) + " differs from its buffer (" +
                   std::to_string(h.text.size()) + " units against " + std::to_string(text.size()) +
                   " bytes); ";
    }
    // The tabs as the host has them, once the window's are sent (the strip
    // sends them as it is drawn; a change that drew nothing is sent here).
    windowTabs(window, w);
    const Json want = Shared::windowJson(w);
    const Json state = c.sessionState();
    bool found = false;
    for (const Json& win : state["windows"].elements())
        if ((std::uint64_t)win["window"].asInt() == window) {
            found = true;
            if (win["tabs"] != want)
                out += "the host's tabs of window " + std::to_string(window) + " are " + win["tabs"].dump() +
                       ", the window's " + want.dump() + "; ";
        }
    if (!found && w.count() > 0) out += "the host has no tabs for window " + std::to_string(window) + "; ";
    for (const std::string& e : c.takeErrors()) out += "the host said: " + e + "; ";
    return out;
}

}  // namespace SessionLink
