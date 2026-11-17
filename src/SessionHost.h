// SessionHost.h — the session's host (SESSION-DESIGN.md, stage 4): the one
// owner of every open document's text (a Model::Document), its version and
// its undo (a Model::UndoHistory, one per document, not per renderer), and
// of the windows' tabs as their renderers report them. Renderers
// (SessionClient.h) only send it messages (SessionProtocol.h) and keep
// mirrors.
//
// Host is the logic, with no thread: handle() takes one renderer's message
// and sends what follows through the Sink, synchronously, so a test can
// deliver messages in any order it likes. SessionHost runs a Host on a
// thread of its own, as the GTK port does: post() queues a message from any
// thread and returns at once.
//
// An edit from a renderer names the host version it was made on (`base`).
// Everything the host applied since was concurrent with it (a renderer has
// one edit in flight at a time), so the edit is moved past those
// (Ot::transform, the host's winning ties), applied, recorded in the
// document's undo history as its renderer grouped it (Step), and sent to
// every renderer with the document open: to its sender as an
// acknowledgement, to the others as the edit to apply. The log of applied
// edits is kept back to the oldest base a renderer might still send.
//
// Unit: the host's text, history and logs count UTF-16 units; each
// renderer's offsets are converted at the edge, using the text as it is at
// that moment, and every logged edit is kept in both units.
//
// Pure C++17 (std::thread for SessionHost); no I/O.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Document.h"
#include "SessionProtocol.h"
#include "UndoHistory.h"

namespace Shared {

class Host {
public:
    // Where the host's messages go: to renderer `to`.
    using Sink = std::function<void(ClientId to, Message&& m)>;
    explicit Host(Sink sink) : sink_(std::move(sink)) {}

    // A renderer joins: its id, to be named in every handle(). Nothing is
    // sent until its Hello.
    ClientId connect();
    // It goes: its documents and windows stay with the host (stage 6 keeps
    // them for the next renderer); it hears nothing more.
    void disconnect(ClientId c);
    void handle(ClientId from, Message&& m);

    // For checks: a document's text, version, history and dirtiness as the
    // host has them, and the windows' tabs.
    bool hasDocument(DocId d) const { return docs_.count(d) > 0; }
    std::u16string text(DocId d) const;
    std::uint64_t version(DocId d) const;
    bool dirty(DocId d) const;
    UndoInfo undoInfo(DocId d) const;
    std::size_t logSize(DocId d) const;
    Json state() const;

private:
    struct Renderer {
        Model::Unit unit = Model::Unit::Utf16;
        std::string name;
        bool hello = false;
    };
    // One applied edit, as every renderer may need it again: by whom, in
    // both units.
    struct Entry {
        ClientId by = 0;
        Ot::Ops ops16, opsCp;
    };
    struct Doc {
        std::string path;
        Model::Document doc;
        Model::UndoHistory history{Model::Unit::Utf16};
        std::uint64_t v = 0;
        std::deque<Entry> log;          // versions logStart + 1 ... v
        std::uint64_t logStart = 0;
        // Renderers with it open: the base each last sent (the log is kept
        // past the smallest), whether it watches the text, and the undo
        // state it last heard.
        struct Seat {
            std::uint64_t base = 0;
            bool watch = false;
            UndoInfo heard;
        };
        std::map<ClientId, Seat> seats;
        // The step open in the history: whose, and the selection it last
        // reported after its edits (UTF-16).
        bool stepOpen = false;
        ClientId stepBy = 0;
        std::uint64_t stepId = 0;
        Model::UndoHistory::StepInfo stepInfo;
        Selection stepAfter;
    };

    Sink sink_;
    std::map<ClientId, Renderer> renderers_;
    ClientId nextClient_ = 0;
    std::map<DocId, Doc> docs_;
    std::map<std::uint64_t, std::pair<ClientId, Json>> windows_;

    void send(ClientId to, Message&& m);
    void error(ClientId to, DocId d, const std::string& what);
    Model::Unit unitOf(ClientId c) const;
    // The log after `base` in `u`, as one list; false when `base` is not
    // in it (newer than the document, or older than the log's start).
    bool concurrent(const Doc& d, std::uint64_t base, Model::Unit u, Ot::Ops* out) const;
    void trimLog(Doc& d);
    // Offsets of `s` (in `u`, in the text as it is) as UTF-16, and back.
    Selection toUnits(const Doc& d, const Selection& s, Model::Unit u) const;
    Selection fromUnits(const Doc& d, const Selection& s, Model::Unit u) const;
    Selection mapSelection(const Selection& s, const Ot::Ops& ops, Model::Unit u) const;
    // Primitives in `u`, applied one at a time to the text: logged in both
    // units, and each recorded in the open step when `record`.
    // With `outside` (and not `record`), each moves the history past it
    // instead (UndoHistory::foreign).
    Entry applyOps(Doc& d, const Ot::Ops& ops, Model::Unit u, bool record, bool outside = false);
    void closeStep(Doc& d);
    void broadcast(DocId id, Doc& d, const Entry& e, ClientId by, std::uint64_t seq, int flag,
                   ClientId selectFor, const Selection* select16);
    void tellUndoState(DocId id, Doc& d);

    void onHello(ClientId c, Message& m);
    void onOpen(ClientId c, Message& m);
    void onReset(ClientId c, Message& m);
    void onClose(ClientId c, Message& m);
    void onEdit(ClientId c, Message& m);
    void onUndo(ClientId c, Message& m);
    void onSaved(ClientId c, Message& m);
    void onForget(ClientId c, Message& m);
    void onWatch(ClientId c, Message& m);
    void onQuery(ClientId c, Message& m);
};

// A Host on a thread of its own. Messages from renderers go in with post()
// from any thread, in order per renderer; the host's go out through the
// Sink on the host's thread. MINICODE_HOST_DELAY_US-style delays are the
// caller's: `delayUs` makes the thread wait that long before each message,
// as a slow link would.
class SessionHost {
public:
    explicit SessionHost(Host::Sink sink, unsigned delayUs = 0);
    ~SessionHost();   // finishes what was posted, then stops
    SessionHost(const SessionHost&) = delete;
    SessionHost& operator=(const SessionHost&) = delete;

    ClientId connect();
    void post(ClientId from, Message&& m);
    void disconnect(ClientId c);
    // Runs `f` on the host's thread with the Host, after everything posted
    // before it, and waits for it (checks only).
    void inspect(const std::function<void(const Host&)>& f);

private:
    Host host_;
    unsigned delayUs_;
    std::mutex mu_;
    std::condition_variable cv_;
    struct Item {
        ClientId from = 0;
        Message m;
        std::function<void()> run;   // instead of a message
    };
    std::deque<Item> queue_;
    bool stop_ = false;
    std::thread thread_;
    void loop();
};

}  // namespace Shared
