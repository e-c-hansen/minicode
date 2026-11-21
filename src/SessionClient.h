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
    // Told whenever a document's unsaved state against the disk flips on
    // the host (DocDirty), whoever made it flip, for every document the
    // host holds, open here or not; said in its Hello (flag 8), so a host
    // sends it only to a renderer that knows the message. Called on this
    // renderer's thread, inside dispatch(). Before connect().
    void hearDirty(std::function<void(DocId doc, bool dirty)> f) {
        dirtyListener_ = std::move(f);
        hearsDirty_ = dirtyListener_ || writtenListener_;
    }
    // Told when the host wrote a document's file for another renderer (a
    // page's save, another app's): DocDirty flag 2, with the host version
    // written and {path, encodingName, stamp} (SessionFiles.h's stampJson).
    // Hears DocDirty too (Hello flag 8). Called inside dispatch(), before
    // the dirty listener. Before connect().
    void hearWrites(std::function<void(DocId doc, std::uint64_t version, const Json& json)> f) {
        writtenListener_ = std::move(f);
        hearsDirty_ = dirtyListener_ || writtenListener_;
    }
    // An app that saves its documents through the host (requestSave, not a
    // write of its own and saved()), said in its Hello (flag 16, with
    // setKeepsBackups): a guest's save of a document its window shows is
    // then the host's to make, as the app's are. Before connect().
    void setSavesThroughHost(bool on) { savesThroughHost_ = on; }
    // Whether the host saves documents itself (its Welcome said so; a host
    // from before saving does not, and would drop a DocSave). Known once
    // the Welcome is taken (settle after connect()).
    bool hostSaves() const { return (hostFlags_ & 1) != 0; }
    // Whether it saves for an app too (Welcome flag 2): keeps the encoding
    // the app's DocSave names, answers with the stamp written, and tells the
    // other renderers it wrote (hearWrites).
    bool hostSavesForApps() const { return (hostFlags_ & 3) == 3; }
    // ---- the disk (stage 7: the host watches it, DiskWatch.h)
    // Told what the host saw change on disk, said in its Hello (flag 32), so
    // a host sends it only to a renderer that knows the messages. `folders`:
    // FsChanged's window and json {root, listings: [each open folder's "list"
    // answer again, for the folders watchFolders named that changed], git:
    // whether Source Control may have changed}. `document`: DocDisk, a
    // document's file changed under unsaved changes (1), went (2), or is in
    // step with it again (0). Called inside dispatch(). Before connect().
    void hearDisk(std::function<void(std::uint64_t window, const Json& changed)> folders,
                  std::function<void(DocId doc, int disk)> document) {
        foldersListener_ = std::move(folders);
        diskListener_ = std::move(document);
        hearsDisk_ = foldersListener_ || diskListener_;
    }
    // Whether the host watches the disk (its Welcome's flag 4): it takes
    // watchFolders and requestReload. Known once the Welcome is taken.
    bool hostWatches() const { return (hostFlags_ & 4) != 0; }
    // The folders this renderer's trees have open in `window`'s folder (0 a
    // headless host's own), '/'-separated from it, "" the folder itself,
    // listed with dotfiles when `hidden`: the host watches them (an ignored
    // folder, a node_modules, only while one is open) and sends each one's
    // listing again when it changes. Replaces what was said before. The
    // answer comes as a read's (readAnswer); 0, sending nothing, to a host
    // that does not watch.
    std::uint64_t watchFolders(std::uint64_t window, const std::vector<std::string>& open, bool hidden);
    // (requestReload, below with the saves.)
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
    // A guest's New File (a browser's page): an untitled document, empty,
    // made on the host with this id (newId), this renderer's seat a view as
    // its others; the host lets it go when nobody has it and nobody typed
    // into it. False, sending nothing, to a host that does not write for
    // pages (hostWritesForPages).
    bool openUntitled(DocId doc, Listener* listener);
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
    //
    // Both this and savedAs fence the dirty listener (hearDirty) for the
    // document until the host has taken the DocSaved: a Ping goes behind
    // it, and DocDirty heard before its Pong is held, the latest of them
    // told at the Pong. The host acknowledges an edit before the DocDirty
    // it caused, so the ack lets the DocSaved out while that "unsaved" is
    // still on its way: a reload's edit and then its DocSaved would tell
    // the listener "unsaved" for a round trip after the renderer counted
    // the document clean (the save suite's reload, CI 2026-11-21).
    void saved(DocId doc);
    // The file holds `text`, which is not the mirror's (a Keep Mine kept the
    // buffer over a file changed on disk): the host counts the document
    // unsaved against it, as the window does (DocSaved flag 1). False,
    // sending nothing, to a host that would take it for saved() (no
    // hostSavesForApps).
    bool savedAs(DocId doc, std::u16string text);
    // Whether a DocSaved of the document's is still fenced (checks).
    bool dirtyFenced(DocId doc) const { return fenced_.count(doc) > 0; }
    // The host writes the document to its file itself (stage 7,
    // SessionFiles.h's saveDocument), after every edit this renderer sent
    // before: `window` names whose folder holds a guest's save (0 for a
    // headless host's own), `overwrite` writes over a file that changed on
    // disk or went (a person's answer to the first refusal). 0, sending
    // nothing, when the document is not open here or the host does not
    // save (hostSaves). The answer as saveAnswer(req) once dispatched.
    // `encoding`, from an app (hostSavesForApps), is the encoding its tab
    // writes (FolderSearch::encodingName), kept whatever the file holds now;
    // "" lets the host choose by the file.
    std::uint64_t requestSave(DocId doc, std::uint64_t window, bool overwrite,
                              const std::string& encoding = std::string());
    struct SaveAnswer {
        bool ok = false;
        std::uint64_t version = 0;   // the host version written
        // saveDoneJson's {path, ok, reason, why, note, encoding, encodingName,
        // stamp, created}
        Json json;
    };
    bool saveAnswer(std::uint64_t req, SaveAnswer* out);
    // Whether the host writes for a browser's page (its Welcome's flag 16):
    // Save As (requestSaveAs), a new untitled document (openUntitled), and
    // Replace in Folder (requestReplace).
    bool hostWritesForPages() const { return (hostFlags_ & 16) != 0; }
    // A guest's Save As: the document written to `rel` ('/'-separated from
    // `window`'s folder, which it must be held inside: SessionFiles.h's
    // newFilePath, with dotfiles only when `hidden`), after every edit this
    // renderer sent, and its path that file's for every view. A file there
    // already is written over only with `overwrite` (the first answer is
    // "exists"). Answered as a save (saveAnswer). 0, sending nothing, when
    // the document is not open here or the host does not write for pages.
    std::uint64_t requestSaveAs(DocId doc, std::uint64_t window, const std::string& rel, bool hidden,
                                bool overwrite);
    // The host puts the file's text in place of the document's (Reload from
    // Disk: one step, undoable, counted saved), after every edit this
    // renderer sent before. 0, sending nothing, when the document is not
    // open here or the host does not watch (hostWatches). The answer as
    // reloadAnswer(req) once dispatched (its json {path, ok, reason, why}).
    std::uint64_t requestReload(DocId doc);
    bool reloadAnswer(std::uint64_t req, SaveAnswer* out);

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
    // This renderer's messages about `doc` the host has not taken: whether
    // what the host says of the document (its unsaved state, a write) is of
    // the mirror's text. Another document's edits in flight do not count
    // (they kept the Mac's tab from taking the host's "saved" after a
    // reload, and its dot stuck: docs/notes/session-host.md).
    std::size_t unconfirmed(DocId doc) const;
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
    // The heavier reads (SessionSearch.h: "files", "search"), on the host's
    // workers, answered as they go: several answers for one request, the
    // last with "done". Whether the host takes them (its Welcome's flag 8),
    // known once the Welcome is taken.
    bool hostSearches() const { return (hostFlags_ & 8) != 0; }
    // Whether the host runs a terminal in its own login shell and
    // environment (its Welcome's flag 32, TermSpec's hostShell): what a
    // window on another machine needs (stage 8, the Mac over SSH).
    bool hostRunsOwnShell() const { return (hostFlags_ & 32) != 0; }
    // The folder the host serves (its Welcome's name: a headless host's,
    // resolved on its machine; "" for an app's own), and what else its
    // Welcome said (its json: `mutible host --stdio` given a file names it,
    // {"file": path}). Known once the Welcome is taken; main thread.
    const std::string& hostFolder() const { return hostFolder_; }
    const Json& welcomeJson() const { return welcome_; }
    // Sends one: its answers come as takeStream(req) once dispatched. 0,
    // sending nothing, to a host that does not take them.
    std::uint64_t requestStream(std::uint64_t window, Json request);
    // Replace in Folder from a page (SessionSearch.h's "replace"), sent
    // behind every edit this renderer sent, its one answer a stream's
    // (takeStream). 0, sending nothing, to a host that does not write for
    // pages.
    std::uint64_t requestReplace(std::uint64_t window, Json request);
    // The answers that came since the last take, appended to *out: true
    // once the last ("done") is among them, and the request is forgotten.
    bool takeStream(std::uint64_t req, std::vector<Json>* out);
    // Stops one (FsRead's "cancel", in `window`'s folder as it was asked):
    // what still comes for it is dropped, and the request forgotten at its
    // last answer.
    void cancelStream(std::uint64_t req, std::uint64_t window);
    // Requests whose answers are still to come or to be taken (checks).
    std::size_t streams() const { return streams_.size(); }

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
    bool readOnly_ = false, guest_ = false, keepsBackups_ = false, hearsDirty_ = false;
    bool savesThroughHost_ = false, hearsDisk_ = false;
    std::function<void(std::uint64_t, const Json&)> foldersListener_;
    std::function<void(DocId, int)> diskListener_;
    std::function<void(DocId, bool)> dirtyListener_;
    std::function<void(DocId, std::uint64_t, const Json&)> writtenListener_;
    int hostFlags_ = 0;   // the Welcome's
    std::string hostFolder_;   // the Welcome's name
    Json welcome_;             // the Welcome's json
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
    // The heavier reads (requestStream): the answers not taken yet, whether
    // the last came, and whether the renderer stopped it (cancelStream).
    struct Stream {
        std::vector<Json> answers;
        bool done = false, cancelled = false;
    };
    std::map<std::uint64_t, Stream> streams_;
    // saved()'s fences: the Ping sent behind each DocSaved, by its request,
    // and per document how many are out and the latest DocDirty held.
    struct Fence {
        int out = 0;
        bool held = false, dirty = false;
    };
    std::map<std::uint64_t, DocId> fencePings_;
    std::map<DocId, Fence> fenced_;
    void fenceDirty(DocId doc);
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
