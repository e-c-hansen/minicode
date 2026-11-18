// SessionClient.h — a renderer's side of the session (SESSION-DESIGN.md,
// stage 4): what the GTK editor (and in 4b the Mac's) uses instead of
// owning its documents' text and undo. The renderer keeps a mirror of each
// document (its GtkTextBuffer, its NSTextStorage), edits it at once as the
// user types, and reports each edit here as it makes it; the host
// (SessionHost.h) owns the text and the undo history.
//
// What the client keeps:
//   - an outbox, sent in order, with one edit in flight at a time (as
//     ot.js's client): an edit is sent with the last host version the
//     renderer applied (`base`), and the next waits for its acknowledgement,
//     so the host needs to move it past nothing of this renderer's own.
//     Edits waiting meanwhile are joined while they belong to one undo step.
//   - for each document the host version it has heard of, the step being
//     recorded, and what typing may join (coalescing is the renderer's call:
//     it knows the keys and the caret).
//   - edits from elsewhere (an undo, another renderer's typing) moved past
//     its own unconfirmed ones (Ot::transform) and handed to the document's
//     listener to apply to the mirror; its unconfirmed ones moved past them.
//   - on request (watch), a copy of the host's text kept from the host's own
//     edits: what the GTK port's language server sync reads.
//
// Threads: deliver() may be called from any thread (the host's); every other
// call, and every listener and watcher, on the renderer's own thread, inside
// dispatch() or a call that waits (settle, undo, the queries). In one
// process those waits are a round trip through the host's queue: a few
// microseconds. Pure C++17, no I/O.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "SessionProtocol.h"
#include "Text.h"

namespace Shared {

class Client {
public:
    using Send = std::function<void(Message&& m)>;
    // A renderer that only reads says how far it has read every this many
    // versions, so the host's log of edits it may still have to move a
    // renderer's edit past stays short.
    static constexpr std::uint64_t kReportEvery = 32;

    // What applies the host's edits to a document's mirror.
    class Listener {
    public:
        virtual ~Listener() = default;
        // An edit made elsewhere (flag 0), or an undo (1) or redo (2) this
        // renderer asked for, in its unit, already moved past its own
        // unconfirmed edits: apply it now, recording nothing. `select`, for
        // an undo, is the selection to show.
        virtual void hostEdited(DocId doc, const Ot::Ops& ops, int flag, const Selection* select) = 0;
        // A view's document closed on the host, or was never there
        // (view()): the client has forgotten it.
        virtual void hostGone(DocId /*doc*/) {}
    };
    // Each edit the host applied, in UTF-16 and in the host's order, once
    // the copy (watch()) has taken it.
    using Watcher = std::function<void(DocId doc, const Ot::Ops& ops16)>;

    Client(Model::Unit unit, std::string name);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Says Hello through `send` (which runs on this thread and must not call
    // back into the client).
    void connect(Send send);
    // A renderer that only reads (a browser's view, SessionWeb.h): said in
    // its Hello, so the host refuses anything from it that would change the
    // session. Before connect().
    void setReadOnly(bool on) { readOnly_ = on; }
    // A guest (a browser's view that may edit, SessionWeb.h): its documents
    // are views (view()), as a reader's, but it may edit and undo them,
    // type into the terminals it attached to and say its view's size for
    // them (resizeTerminal: the pty takes it only while the guest is the
    // one that typed last). Said in its Hello. Before connect().
    void setGuest(bool on) { guest_ = on; }
    // An app whose windows keep backups of their unsaved files (Backups.h:
    // GTK's Persistence, the Mac's EditorController+Persistence): said in
    // its Hello (flag 4), so a headless host that keeps backups of what no
    // window shows (Host::keepBackups) lets go of a document's backup while
    // this renderer has it open, and takes it up again once it detaches.
    // Not for a renderer that only reads, nor a guest. Before connect().
    void setKeepsBackups(bool on) { keepsBackups_ = on; }
    Model::Unit unit() const { return unit_; }

    // ---- ids: what this renderer opens on the host (documents, terminals,
    // windows) takes an id from here, never one it numbered itself, so a
    // second renderer on the host never makes the same id (SessionProtocol.h,
    // kIdRange). One counter for all three. The base comes in the Welcome,
    // the answer to connect()'s Hello: an app settles after connect(), and
    // newId() before the Welcome was taken settles first (at most
    // `timeoutMs`). 0, which is never an id, when there is no base (no host
    // answered, or it had none left) or this connection used up its range.
    std::uint64_t newId(int timeoutMs = 10000);
    std::uint64_t idBase() const { return idBase_; }
    // This renderer's id on the host (the Welcome's), 0 before it: what the
    // state's windows name as the renderer that reported them.
    ClientId clientId() const { return id_; }
    // Documents and terminals the host holds already (StateQuery) keep their
    // ids: a renderer attaching opens them under those.

    // ---- the host's messages
    // Any thread: queued, and `wake` called when the queue was empty.
    void deliver(Message&& m);
    void setWake(std::function<void()> wake);
    // What was queued when it was called, in order, and no more: a listener
    // that acknowledges terminal output as it takes it lets the host send
    // more at once, and a dispatch() that ran until the queue was empty
    // never returned while a program printed as fast as it was taken
    // (term:credit hung CI under TSan, 2026-11-18). What came meanwhile
    // waits for the next call, and `wake` is called for it.
    void dispatch();
    // Everything sent before now done by the host and its answers taken
    // (dispatch()ed). False when the host did not answer in time, even
    // while other messages keep coming.
    bool settle(int timeoutMs = 10000);
    // The host is gone (its socket closed, HostLink's `closed`): nothing is
    // sent from now on, and every wait (settle, undo, the queries, newId)
    // returns at once once what had arrived is taken, so a renderer whose
    // host went neither hangs nor waits out a timeout per call. Any thread;
    // `wake` is called, so the renderer dispatches and can see it (lost()).
    void hostLost();
    bool lost() const { return lost_.load(); }

    // ---- documents
    // `text8` is the mirror's text as it starts; the host's copy starts as
    // it, saved.
    void open(DocId doc, const std::string& path, std::string text8, Listener* listener);
    // The mirror's text set whole anew (a file read again): the host's too,
    // its history forgotten.
    void reset(DocId doc, std::string text8);
    void close(DocId doc);
    // The document's file is now `path` (Save As, a rename): the host's
    // state names it so, and a renderer reattaching finds it by it.
    void setPath(DocId doc, const std::string& path);
    // Lets go of the document and keeps it on the host (a window closing
    // detaches, stage 6): what this renderer recorded for it is sent first,
    // in order, then its seat goes; the listener hears nothing from now on.
    // The host keeps its text and history for a renderer to open again
    // (open() with an empty text gets the host's text).
    void detach(DocId doc);
    // A read-only renderer's view of a document the host has: its text comes
    // to `listener` as one insertion, then every edit. The host neither makes
    // the document nor keeps it open for this; hostGone when it goes.
    void view(DocId doc, Listener* listener);
    bool isOpen(DocId doc) const { return docs_.count(doc) > 0; }
    // The host version this renderer has applied.
    std::uint64_t version(DocId doc) const;

    // ---- the renderer's edits, in its unit, as it makes them
    // A step opens and closes around edits; nested pairs are one step.
    // Typing (`coalesce`) joins the step before unless the selection moved
    // since, or coalescing was broken.
    void openStep(DocId doc, const Model::UndoHistory::StepInfo& info, const Selection& before);
    void renameStep(DocId doc, const Model::UndoHistory::StepInfo& info);
    void closeStep(DocId doc, const Selection& after);
    bool stepOpen(DocId doc) const;
    // One operation, in the text as the operations before it left it. With
    // no step open it is a step of its own ("Edit").
    void record(DocId doc, Ot::Op op);
    // One operation the mirror made outside its undo history (an edit
    // nothing registered for undo, as a storage edit outside the text
    // view's): the host applies it in no step and moves its history past
    // it, dropping the steps it touched and those older (UndoHistory's
    // foreign). Made between steps, never inside one.
    void recordOutside(DocId doc, Ot::Op op);
    // The undo history forgotten, the text kept (NSUndoManager's
    // removeAllActions).
    void forgetHistory(DocId doc);
    void breakCoalescing(DocId doc);
    // The selection now, outside a step: moved, typing starts a new step.
    void noteSelection(DocId doc, const Selection& now);
    // The text as the mirror has it now was written to its file.
    void saved(DocId doc);

    // ---- undo (the host's)
    UndoInfo undoInfo(DocId doc);   // settles first
    // The host undoes its document's last step (whoever made it); the edit
    // comes back through the listener, with the selection, before this
    // returns.
    Outcome undo(DocId doc, bool redo);
    // The same without waiting: the edit comes through the listener and the
    // outcome as outcome(req) once the host's answers are dispatched.
    std::uint64_t requestUndo(DocId doc, bool redo);
    bool outcome(std::uint64_t req, Outcome* out);

    // ---- the host's text, copied
    // A copy of the host's text kept from the host's edits (UTF-16), and
    // `w` told of each; null when the document is not open, or when the
    // host did not answer within `timeoutMs` (then `w` is not kept either,
    // so an owner that goes after a stall is never called). `owner` names
    // the watcher for unwatch().
    const Model::Text* watch(DocId doc, const void* owner, Watcher w, int timeoutMs = 10000);
    // Whether `owner` watches `doc` (checks).
    bool watching(DocId doc, const void* owner) const;
    void unwatch(DocId doc, const void* owner);
    const Model::Text* hostCopy(DocId doc) const;

    // ---- checks
    // The host's text, version, dirtiness and undo state now (settles).
    struct HostDoc {
        bool open = false;
        std::u16string text;
        std::uint64_t version = 0;
        bool dirty = false;
        UndoInfo undo;
    };
    HostDoc hostDocument(DocId doc);
    std::size_t unconfirmed() const { return outbox_.size() + (inFlight_ ? 1 : 0); }
    // What the host said went wrong (an edit that did not fit), oldest
    // first; takeErrors() empties the list.
    std::vector<std::string> takeErrors();

    // ---- terminals (stage 5): the host owns the pty; the renderer's
    // emulator is fed what it forwards and sends back what the user types.
    // Sent at once, never behind an edit waiting for its acknowledgement.
    class TermListener {
    public:
        virtual ~TermListener() = default;
        virtual void termOpened(TermId, int /*pid*/) {}
        // Output to feed the emulator, in order; `replay` for the bytes that
        // rebuild a terminal attached to. `cursorLine` is the host screen's
        // cursor line after these bytes (TermMark's count), `alt` whether a
        // program has the alternate screen. Acknowledge with ackTerminal
        // once the emulator has taken them.
        virtual void termOutput(TermId, const std::string& bytes, long long cursorLine, bool alt,
                                bool replay) = 0;
        virtual void termShell(TermId, const TermShell&) {}
        virtual void termMarks(TermId, const std::vector<TermMark>&) {}
        // The shell's wait status (-1 unknown), or why it never started.
        virtual void termExited(TermId, int /*status*/, const std::string& /*error*/) {}
    };
    void openTerminal(TermId t, const TermSpec& spec, TermListener* l);
    void attachTerminal(TermId t, TermListener* l);
    // `answer`: the emulator's reply to a program's query (DA, DSR, OSC
    // colors), not the user's typing, so it does not give this renderer the
    // pty's size.
    void terminalInput(TermId t, std::string bytes, bool answer = false);
    // The view's size: the pty takes it while this renderer is the one that
    // last typed (or opened it, before anyone has typed); TermShell's cols
    // and rows say the pty's size.
    void resizeTerminal(TermId t, int cols, int rows);
    void ackTerminal(TermId t, std::size_t bytes);
    // Hangs the shell up; the listener hears nothing more.
    void closeTerminal(TermId t);
    // No longer shows it: the listener hears nothing more, and the shell
    // runs on for a renderer to attach to (attachTerminal).
    void detachTerminal(TermId t);
    // Whether this renderer shows the terminal (opened or attached here).
    bool showsTerminal(TermId t) const { return terms_.count(t) > 0; }

    // ---- the workspace (SessionFiles.h): reads in the session's folder,
    // or in `window`'s when the host serves no one folder. Sent at once,
    // never behind an edit in flight.
    // The answer as readAnswer(req) once the host's answers are dispatched.
    std::uint64_t requestRead(std::uint64_t window, Json request);
    // `bytes`: a "read"'s bytes (FsData's).
    bool readAnswer(std::uint64_t req, Json* out, std::string* bytes = nullptr);
    // A read whose answer nobody will take: dropped now, or when it comes.
    void forgetRead(std::uint64_t req);
    // The same, waiting: null when the host did not answer in time.
    Json read(std::uint64_t window, Json request, int timeoutMs = 10000);

    // ---- windows
    void windowTabs(std::uint64_t window, Json tabs);
    void windowClosed(std::uint64_t window);
    Json sessionState();   // settles

private:
    struct Doc {
        Listener* listener = nullptr;
        std::uint64_t known = 0;   // the last host version applied
        std::uint64_t told = 0;    // the last base the host heard
        // The step being recorded.
        int depth = 0;
        std::uint64_t stepId = 0;
        Model::UndoHistory::StepInfo info;
        bool announced = false;    // a message of it went in the outbox
        Selection before;
        // Coalescing: where the last step left the selection, and whether it
        // moved since or was broken.
        Selection lastAfter;
        bool moved = true, broken = true;
        bool fresh = true;   // the step being recorded starts anew
        bool detaching = false;   // detach(): forgotten once its DocDetach is sent
        UndoInfo undo;
        std::unique_ptr<Model::Text> copy;
        std::vector<std::pair<const void*, Watcher>> watchers;
    };

    Model::Unit unit_;
    std::string name_;
    bool readOnly_ = false, guest_ = false, keepsBackups_ = false;
    Send send_;
    ClientId id_ = 0;
    std::uint64_t idBase_ = 0, idsUsed_ = 0;
    std::map<DocId, Doc> docs_;
    std::deque<Message> outbox_;
    bool inFlight_ = false;
    Message head_;   // the edit in flight
    std::uint64_t seq_ = 0, nextStep_ = 0, nextReq_ = 0;
    std::map<std::uint64_t, Message> replies_;
    std::map<TermId, TermListener*> terms_;
    void sendNow(Message&& m);
    void onTerminal(Message& m);
    // Requests whose wait timed out: their answers are dropped on arrival.
    std::set<std::uint64_t> abandoned_;
    std::vector<std::string> errors_;
    std::atomic<bool> lost_{false};

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Message> inbox_;
    std::function<void()> wake_;

    Doc* doc(DocId id);
    void queue(Message&& m);
    void pump();
    // The next message waiting, taken under the lock; false for none.
    bool take(Message* out);
    void process(Message&& m);
    void onEdited(Message& m);
    bool waitFor(const std::function<bool()>& done, int timeoutMs);
    // Sends `m` (given a fresh req) and waits for the reply to it.
    bool ask(Message&& m, Message* reply, int timeoutMs = 10000);
    Selection mapSelection(const Selection& s, const Ot::Ops& ops) const;
};

// What a window opening on a host's folder takes of what the host holds
// (stage 6, step 2; both apps): the latest record a closed window left (its
// tabs, windowJson's shape) with the documents and terminals it names, then
// those held documents and terminals no record names. A record is free
// when no window of this renderer reports under it (`live`) and its
// renderer is gone or is this one; what a record of a window still open
// names is that window's, and left alone; one that names nothing is
// never the one taken.
//   - A document is taken when it has a path, or has none and holds text or
//     unsaved changes (an untitled buffer: the window shows it under a name
//     of its own, Backups::untitledPath); one with neither (a renderer's
//     document before it had a file) never is.
//   - Never two documents of one path (two windows on one file, both
//     closed): the record's own first, then those no record names, oldest
//     id first; the others stay on the host for the folder's next window.
//   - `all` (a check's cleanup): every free record and what each names,
//     and every document no record names, seconds of a path too.
// The terminals' tabs and split panes: Shared::termDockFrom(tabs, the
// terms' ids), TermDockRecord.h.
struct Reattach {
    struct Doc {
        DocId id = 0;
        std::string path;   // "" for an untitled buffer
        bool dirty = false;
    };
    struct Term {
        TermId id = 0;
        int pid = 0;
        TermShell shell;
    };
    std::uint64_t window = 0;   // the record taken, 0 for none
    Json tabs;                  // its tabs, null for none
    std::vector<Doc> docs;
    std::vector<Term> terms;
    std::vector<std::uint64_t> records;   // `all`: every free record
    bool empty() const { return !window && docs.empty() && terms.empty(); }
};
// `state` is the host's StateQuery answer; `me` this renderer's id;
// `openHere` and `showsHere` what this renderer has open already.
Reattach reattachFrom(const Json& state, ClientId me, const std::set<std::uint64_t>& live,
                      const std::function<bool(DocId)>& openHere,
                      const std::function<bool(TermId)>& showsHere, bool all);

}  // namespace Shared
