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
// Terminals (stage 5): the host owns each terminal's pty (HostPty.h) and a
// headless TerminalScreen with scrollback, fed every byte the shell writes,
// with the shell integration's events read from the same bytes
// (TerminalStream) into a TermShell. The bytes go on to every renderer
// showing the terminal (the one that opened it, and each that attached
// since), whose own emulator (VTE on GTK) draws them; input comes back as
// bytes. The host's screen answers no queries (DA, DSR): the renderer's
// emulator does, through its input. A renderer that attaches gets the
// screen as replay() bytes, then the live output.
//
// Stage 6: renderers come and go (a headless host, SessionServer.h).
// Detaching leaves documents, windows and terminals as they are; a
// terminal nobody shows keeps running, its output going only into the
// host's screen. busyReason() says whether anything would be lost if the
// host went now.
//
// Pure C++17 and POSIX: SessionHost's thread polls the ptys (poll(2)) and a
// pipe that wakes it for messages; Host does the I/O only when the loop
// calls it (readable, writable, tick).
#pragma once

#include <chrono>
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
#include "HostPty.h"
#include "SessionProtocol.h"
#include "TerminalScreen.h"
#include "TerminalStream.h"
#include "UndoHistory.h"

namespace Shared {

// One of the shell integration's events (a nonce-checked mark the
// terminal's TerminalStream read) applied to what is known of the shell:
// A a prompt, B the line editor reading, C a command started (with 633;E's
// text), D it finished with its status, 7 the folder, 0/2 the title. A
// prompt with a command still running counts that command finished.
void applyTermEvent(TermShell& s, const TermEvent& e);

class Host {
public:
    // Where the host's messages go: to renderer `to`.
    using Sink = std::function<void(ClientId to, Message&& m)>;
    explicit Host(Sink sink) : sink_(std::move(sink)) {}

    // A renderer joins: its id, to be named in every handle(). Nothing is
    // sent until its Hello.
    ClientId connect();
    // It goes: its documents, windows and terminals stay with the host for
    // the next renderer; it hears nothing more.
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
    // As if id bases up to `slot` had been given (SessionProtocol.h,
    // kIdSlots): checks of what follows two million connections.
    void setLastIdSlot(std::uint64_t slot) { lastSlot_ = slot; }
    // The session as StateQuery answers it: the folder, windows, documents
    // (id, path, version, length, dirty) and terminals (id, size, pid, the
    // shell's state, how many renderers show it).
    Json state() const;
    // The folder a headless host serves ("" in an app's own host), named in
    // state() and Welcome.
    void setFolder(std::string folder) { folder_ = std::move(folder); }
    const std::string& folder() const { return folder_; }
    // Why the session must not end now, "" when nothing would be lost: a
    // document with unsaved changes, or a terminal running a program (a
    // command the integration marked running, or a foreground process group
    // other than the shell's).
    std::string busyReason() const;

    // ---- terminals: what the loop polls, and what it calls when a pty is
    // ready. A terminal's renderers are the one that opened it and every
    // one that attached to it since; all of them hear of it.
    struct PollFd {
        int fd = -1;
        bool read = false, write = false;
        TermId term = 0;
    };
    void pollSet(std::vector<PollFd>* out) const;
    void readable(TermId t);
    void writable(TermId t);
    // Reaps hung-up shells, notices a shell that exited while something it
    // started still holds the pty, and looks at foreground groups and
    // terminal modes: the milliseconds until it wants to run again (-1:
    // only when something happens).
    int tick();
    // Starts a terminal's shell (TermOpen): false, with `error`, when it
    // could not be.
    bool openPty(ClientId c, TermId t, const TermSpec& spec, std::string* error);
    // For checks.
    std::size_t terminalCount() const { return terms_.size(); }
    std::size_t dyingCount() const { return dying_.size(); }
    const TerminalScreen* screen(TermId t) const;
    TermShell shell(TermId t) const;
    int ptyFd(TermId t) const;
    int shellPid(TermId t) const;

private:
    struct Renderer {
        Model::Unit unit = Model::Unit::Utf16;
        std::string name;
        bool hello = false;
        bool readOnly = false;   // Hello's flag 1: a view, which changes nothing
        bool guest = false;      // flag 2: a view that may edit, undo and type
        std::uint64_t idBase = 0;   // its Welcome's: the ids it makes are above it
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
            bool viewer = false;   // a renderer that only reads: keeps nothing open
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

    struct Term {
        // Renderers showing it, each with its output not yet acknowledged:
        // the pty is read while every one of them has credit left.
        std::map<ClientId, long long> viewers;
        std::unique_ptr<Pty> pty;
        TerminalScreen screen;
        TerminalStream stream;
        std::string nonce;
        TermShell shell, sent;
        bool told = false;            // TermShell was sent once
        std::chrono::steady_clock::time_point nextLook;
    };
    // Shells hung up and not yet reaped; `tell` get TermExited once reaped
    // (every renderer of a shell that ended by itself; those but the one
    // that closed it otherwise).
    struct Dying {
        TermId term = 0;
        std::vector<ClientId> tell;
        std::unique_ptr<Pty> pty;
    };
    std::map<TermId, Term> terms_;
    std::vector<Dying> dying_;
    void onTermOpen(ClientId c, Message& m);
    void onTermAttach(ClientId c, Message& m);
    void sendShell(TermId id, Term& t, bool always = false);
    void sendMarks(TermId id, Term& t);
    void look(Term& t);   // foreground group and modes
    // The terminal's shell is gone or going: what is left is read (when
    // `drain`), the pty hung up, and the shell reaped later (tick), telling
    // its renderers but `closer`.
    void endTerminal(TermId id, bool drain, ClientId closer = 0);
    void sendToViewers(const Term& t, const Message& m);
    // Output read from a terminal's pty: into the screen and the stream,
    // then to its renderer.
    void output(TermId id, Term& t, std::string&& bytes);

    Sink sink_;
    std::string folder_;
    std::map<ClientId, Renderer> renderers_;
    ClientId nextClient_ = 0;
    // Id bases (SessionProtocol.h, kIdRange): the last slot given, and the
    // base for a renderer saying Hello: the next slot never given, then,
    // once all have been, the first that no renderer has and in whose range
    // the host holds no id; 0 when none is left.
    std::uint64_t lastSlot_ = 0;
    std::uint64_t chooseIdBase();
    bool slotFree(std::uint64_t slot) const;
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
    // Whether `m` from `c` would change the session while `c` only reads,
    // or go past what a guest may change.
    bool refusedFromViewer(ClientId c, const Message& m) const;
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
// as a slow link would. The thread waits in poll(2) on the terminals' ptys
// and a pipe post() writes to, so a terminal's output and a message are
// taken in turn, neither waiting for the other.
class SessionHost {
public:
    explicit SessionHost(Host::Sink sink, unsigned delayUs = 0);
    ~SessionHost();   // finishes what was posted, then stops
    SessionHost(const SessionHost&) = delete;
    SessionHost& operator=(const SessionHost&) = delete;

    ClientId connect();
    void post(ClientId from, Message&& m);
    void disconnect(ClientId c);
    // The folder a headless host serves (Host::setFolder), on its thread.
    void setFolder(std::string folder);
    // Runs `f` on the host's thread with the Host, after everything posted
    // before it, and waits for it, at most `timeoutMs`: false, and `f` never
    // run, when the thread did not come to it in time (a hang there fails
    // the caller instead of hanging it too). Once `f` has started it is
    // waited for.
    bool inspect(const std::function<void(const Host&)>& f, int timeoutMs = 10000);

private:
    Host host_;
    unsigned delayUs_;
    std::mutex mu_;
    struct Item {
        ClientId from = 0;
        Message m;
        std::function<void()> run;   // instead of a message
    };
    std::deque<Item> queue_;
    bool stop_ = false;
    int wake_[2] = {-1, -1};          // post() writes, the loop polls
    bool wakePending_ = false;        // under mu_: a byte is in the pipe
    std::thread thread_;
    void wake();
    void loop();
};

}  // namespace Shared
