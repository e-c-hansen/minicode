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
// screen as replay() bytes, then the live output. The pty's size is one
// renderer's (tmux's window-size latest): that of the one that last typed
// into it and has said its size, else the opener's, else the first
// attached one's; TermShell tells every renderer the size (see Term).
//
// Stage 6: renderers come and go (a headless host, SessionServer.h).
// Detaching leaves documents, windows and terminals as they are; a
// terminal nobody shows keeps running, its output going only into the
// host's screen. busyReason() says whether anything would be lost if the
// host went now.
//
// Backups (a headless host, Host::keepBackups): a document the host holds
// with unsaved changes (or with no file and some text) that no app's window
// has open (no seat of a renderer whose Hello said it keeps backups) is
// backed up by the host itself, in the apps' store and format (Backups.h),
// owned by the host's process, so no app takes it while the host lives and
// any app restores it once the host is gone (a crash, a kill, a reboot).
// Written at once when the last such window lets go of it, then again after
// edits from other renderers (a browser's view that edits), as the apps'
// planner times them (SavePlan.h: a second after typing stops, at most five
// after the first change not yet written); removed when the document is
// saved, closed, or opened by an app's window again (whose own backup takes
// over). A document with no file is named for its backup only
// (Backups::untitledPath); the host's document keeps no path.
//
// Stage 7's first reads (SessionFiles.h): FsRead lists one folder of the
// session's (the headless host's own folder, else the folder its window's
// renderer reported), confined to it, for a browser's tree, or reads a
// piece of one file in it for the browser's viewers. They run on the host's
// thread, so a listing is capped (kMaxListed) and a read too
// (kMaxReadBytes).
//
// Stage 7's first write, saving: DocSave has the host write a document's
// text to its file (SessionFiles.h's saveDocument: atomic, the file's own
// encoding, refused when the file changed on disk since the host's saved
// text unless the renderer says to overwrite), then count it saved with
// the stamp it wrote, so its next save knows the file without reading it.
// A guest's save (a browser's page) is held to the session's folder, and is
// refused while an app's window that saves by itself has the document open
// (GTK's): that app saves it, by its own rules and questions, and tells the
// host (DocSaved), as before. An app that saves through the host (the
// Mac's, Hello flag 16) sends DocSave with its tab's encoding after asking
// its own questions, so the host is the file's one writer; when the host
// writes for anyone, the other renderers hear it (DocDirty flag 2) and
// take the stamp written, so a window follows a page's save.
// Whoever makes a document's unsaved state flip (an edit, an undo, a save,
// an app's DocSaved), renderers that said they hear it get DocDirty. The
// write is synchronous on the host's thread, as the host's backups are.
//
// The disk (stage 7, DiskWatch.h): a host told to watch (SessionHost::
// watchDisk; `mutible host`, and the Mac's own host) watches its folder (a
// headless host's) and the folders renderers' trees have open (FsRead's
// "watch": each renderer's list replaces its last; an ignored folder or a
// node_modules is watched only while a tree has it open), coalesces what
// changed, and then (diskChanged):
//   - sends each renderer that hears the disk (Hello flag 32) FsChanged with
//     the listing again of every folder it has open that changed, and
//     whether Source Control may have changed (a change outside .git's
//     insides and outside what the ignore files ignore);
//   - looks again at each document whose file's folder changed and that no
//     app's window has open (an app follows its own tabs and tells the host,
//     DocSaved): a clean one follows its file (the host's edit, one step,
//     "Reload from Disk", sent to every renderer showing it), one with
//     unsaved changes is marked changed on disk (never written over: its
//     save is refused until a person says to overwrite), a deleted file's
//     document is unsaved and gone until the file returns (SessionFiles.h's
//     diskAction). Renderers that hear the disk get DocDisk when that state
//     changes; the state's documents carry it ("disk").
// DocReload (a guest may send it) puts the file's text in place of a
// document no app's window has open, the same step. The reads are on the
// host's thread, as the listings are (capped); the watching itself is on
// the watcher's own thread or queue.
//
// The heavier reads (stage 7, SessionSearch.h): FsRead's "files" (Quick
// Open's list) and "search" (Find in Folder) run on worker threads of the
// host's own (ReadWorkers), never on its thread, and answer as they go (a
// search in batches, the last "done"); "cancel" stops one. The loop polls
// readsFd() and calls takeReads(), which sends what the workers answered.
// The Welcome says the host takes them (flag 8).
//
// A page's writes (stage 7, Welcome flag 16): a guest's New File is an
// untitled document the host keeps (DocOpen flag 2, Doc::fromPage, dropped
// while empty once nobody has it); its Save As names the document's file
// anew inside the session's folder (onSaveAs, SessionFiles.h's
// newFilePath, then saveDocument), refused while an app's window has it
// open; its Replace in Folder (FsRead's "replace", onReplace) edits each
// clean document the host holds as one step of the host's ("Replace in
// Folder"), leaves one with unsaved changes alone, and hands the other
// files to a worker (SessionSearch.h). A reader's is refused.
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

#include "Backups.h"
#include "DiskWatch.h"
#include "Document.h"
#include "HostPty.h"
#include "SavePlan.h"
#include "SessionProtocol.h"
#include "SessionSearch.h"
#include "TerminalScreen.h"
#include "TerminalStream.h"
#include "UndoHistory.h"

namespace Shared {

struct DiskText;   // SessionFiles.h

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
    ~Host();   // cancels the workers' reads and waits for them
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

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
    // The file `mutible host --stdio` was given (stage 8): its Welcome names
    // it ({"file": path}, with "new": true when it is not there yet), so the
    // window over SSH shows it.
    void setAskedFile(std::string file, bool isNew = false) {
        askedFile_ = std::move(file);
        askedFileNew_ = isNew;
    }
    // Why the session must not end now, "" when nothing would be lost: a
    // document with unsaved changes, or a terminal running a program (a
    // command the integration marked running, or a foreground process group
    // other than the shell's).
    std::string busyReason() const;

    // ---- the host's own shell (TermSpec's hostShell, stage 8): what a
    // terminal gets when its renderer, on another machine, asks for the
    // host's login shell and environment (HostPty's ownShellSpec). Set
    // once, before any renderer, by `mutible host`; unset, the shell comes
    // from the password database and the environment is the renderer's
    // `env` alone.
    void setOwnShell(std::vector<std::string> env, std::string shell) {
        ownEnv_ = std::move(env);
        ownShell_ = std::move(shell);
    }

    // ---- backups of what no app's window shows (see the top of the file).
    struct BackupKeeping {
        Backups::Store store;
        std::string root;              // Entry::root: the folder, resolved
        std::int64_t ownerPid = 0;     // the host's process, and its start
        double ownerStart = 0;         // (Backups::processStart)
        // Clocks: Entry::saved's (Unix seconds), and the planner's (any
        // that only goes forward, in seconds); the system's when unset.
        std::function<double()> wallNow, monoNow;
        std::function<std::string()> newId;   // Backups::newId when unset
    };
    // From now on (called once, before any renderer, by `mutible host`).
    void keepBackups(BackupKeeping k);
    bool keepsBackups() const { return keeping_; }
    // Every backup a document needs written now, not when planned: the host
    // is about to end (a signal), or a check wants it on disk.
    void writeBackupsNow();
    // For checks: the id of a document's backup on disk, "" for none; and
    // the path that backup names (the document's, or the untitled name).
    std::string backupId(DocId d) const;
    std::string backupPath(DocId d) const;

    // ---- the disk (see the top of the file; SessionHost runs the watcher)
    // Whether the host watches the disk: said in its Welcome (flag 4), so
    // set before any renderer's Hello.
    void setWatching(bool on);
    bool watching() const { return watching_; }
    // What to watch: the host's folder and the folders renderers' trees
    // have open in it, and in each window's folder they named; none when
    // not watching. The generation moves on whenever that changes.
    std::vector<DiskWatch::Root> watchRoots() const;
    std::uint64_t watchGeneration() const { return watchGen_; }
    // A coalesced batch of changes (DiskWatch's, spelled under the roots
    // watchRoots gave).
    void diskChanged(const std::vector<DiskWatch::Change>& changes);
    // For checks: a document's disk state (0 in step, 1 changed on disk,
    // 2 gone).
    int diskState(DocId d) const;

    // ---- the heavier reads (SessionSearch.h), on the host's workers
    // Readable while the workers have answers waiting (-1 before the first
    // such read): the loop polls it, then calls takeReads, which sends them.
    int readsFd() const { return workers_ ? workers_->fd() : -1; }
    void takeReads();
    // For checks: waits (at most `timeoutMs`) for an answer and sends what
    // came; false when none came. And how many reads run.
    bool waitReads(int timeoutMs);
    std::size_t readsRunning() const { return workers_ ? workers_->running() : 0; }
    // For checks: the time a read may take, in seconds (< 0: the op's own),
    // and the workers themselves (made if need be).
    void setReadSeconds(double s) { readSeconds_ = s; }
    ReadWorkers& readWorkers() {
        if (!workers_) workers_ = std::make_unique<ReadWorkers>();
        return *workers_;
    }

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
        bool keepsBackups = false;   // flag 4: an app that backs up what it has open
        bool hearsDirty = false;     // flag 8: told DocDirty
        bool savesThroughHost = false;   // flag 16 (with 4): an app whose saves are DocSave
        bool hearsDisk = false;      // flag 32: told FsChanged and DocDisk
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
        // Its backup while the host keeps one (keepBackups): the file's id,
        // whether it is on disk, the version and path it holds; and the
        // name it was given for having no path (Backups::untitledPath).
        struct Kept {
            std::string id;
            bool written = false;
            std::uint64_t v = 0;
            std::string path;
            std::string untitled;
            bool wasShown = false;   // an app's window had it, as last looked
        } kept;
        // Its unsaved state as renderers that hear DocDirty were last told.
        bool toldDirty = false;
        // Its file as the host last looked at it after the folder changed
        // (diskChanged): 0 in step, 1 changed on disk under unsaved
        // changes, 2 gone; what renderers that hear the disk were told; and
        // the stamp then, so an event that changed nothing of it reads
        // nothing.
        int disk = 0, toldDisk = 0;
        FolderSearch::FileStamp diskSeen;
        bool diskLooked = false;
        // Its file's folder by its real place, for a path spelled otherwise
        // than the watched root (/tmp and /private/tmp); "" until needed.
        std::string realDir;
        // Made by a page's New File (DocOpen flag 2): let go when no
        // renderer has it and it is still empty and untitled.
        bool fromPage = false;
    };
    // The folders a renderer's trees have open (FsRead's "watch"): in whose
    // folder, and the listings' dotfile rule.
    struct WatchAsk {
        std::uint64_t window = 0;
        std::string root;
        std::vector<std::string> open;
        bool hidden = false;
    };
    std::map<ClientId, WatchAsk> watches_;
    bool watching_ = false;
    std::uint64_t watchGen_ = 0;
    std::vector<std::string> ownEnv_;   // setOwnShell's
    std::string ownShell_;
    std::string askedFile_;
    bool askedFileNew_ = false;
    // A watch op answered (FsData's json), the ask kept.
    Json onWatchAsk(ClientId c, std::uint64_t window, const Json& request);
    void onReload(ClientId c, Message& m);
    // The document's file looked at again: followed, marked, or left.
    void lookAtDisk(DocId id, Doc& d);
    // The file's text in place of the document's: one step of the host's
    // ("Reload from Disk"), every seat told, counted saved as read.
    void takeDiskText(DocId id, Doc& d, const DiskText& t);
    // `next` in place of the document's text as one step of the host's
    // named `name`: the one range in which the two differ, every seat told.
    void hostStep(DocId id, Doc& d, const std::u16string& next, const char* name);
    // Replace in Folder from a page (FsRead's "replace", SessionSearch.h):
    // the documents the host holds edited here, the other files on a worker.
    void onReplace(ClientId c, Message& m);
    // A page's Save As (DocSave with "saveAs"): the document's file named
    // anew, inside the session's folder, written, and its path changed.
    void onSaveAs(ClientId c, Message& m);
    // The document whose file is `abs` (by its spelling, or its real
    // place), other than `except`; 0 for none.
    DocId docAtPath(const std::string& abs, DocId except) const;
    // A page's new documents nobody has and nobody typed into, let go.
    void dropEmptyPageDocs();
    void setDisk(DocId id, Doc& d, int disk);

    struct Term {
        // Renderers showing it: the pty is read while every one of them has
        // credit left (output not yet acknowledged under kTermCredit).
        struct Viewer {
            long long unacked = 0;
            int cols = 0, rows = 0;      // its view's size (TermSpec, TermResize), 0 unsaid
            std::uint64_t typed = 0;     // when it last typed (order_), 0 never
            std::uint64_t joined = 0;    // when it opened or first attached (order_)
        };
        std::map<ClientId, Viewer> viewers;
        ClientId opener = 0;
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
    // Whose size the pty takes: the renderer showing it that typed last and
    // has a size, else the opener while it shows it, else the first that
    // joined with a size; nullptr when none has one (the pty keeps its own).
    const Term::Viewer* sizer(const Term& t) const;
    // The pty and the host's screen take the sizer's size, and every
    // renderer is told (TermShell's cols and rows), when it changed.
    void fit(TermId id, Term& t);
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
    // When each window's tabs were last reported, in the order they came
    // (state()'s "seen"): a renderer reattaching takes the latest record a
    // closing window left.
    std::map<std::uint64_t, std::uint64_t> windowSeen_;
    std::uint64_t windowOrder_ = 0;
    std::uint64_t order_ = 0;   // counts terminals' typing and joining (Term::Viewer)

    // Backups (keepBackups): the planner of when they are written, on the
    // monotonic clock.
    bool keeping_ = false;
    BackupKeeping keep_;
    SavePlan::Planner keepPlan_;
    double monoNow() const;
    // Whether the document has text a backup should hold (unsaved, or no
    // file and some text), and whether an app's window has it open (then
    // the host keeps no backup of it).
    static bool unsaved(const Doc& d);
    bool shownByApp(const Doc& d) const;
    // Whether an app's window that saves by itself (its Hello without flag
    // 16: GTK's, an older Mac's) has it open: then a guest's save is
    // refused, since that app writes the file with its own stamp.
    bool shownByAppSavingItself(const Doc& d) const;
    // The path its backup names: its own, or a name for one with none.
    std::string keptPath(DocId id, Doc& d);
    std::vector<SavePlan::Tab> keptTabs();
    // After anything that may change which documents need one: removed at
    // once where none is needed, written at once for a document the host has
    // just taken over, and planned for the rest.
    void backupsChanged();
    void writeBackup(DocId id, Doc& d);
    void discardBackup(Doc& d);

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
    // DocDirty to every renderer that hears it, when the document's unsaved
    // state changed since they were last told (compared only while one
    // hears: a same-length edit compares the whole text).
    void tellDirty(DocId id);
    // After the host wrote the document for `by`: DocDirty flag 2 (with the
    // version written and {path, encodingName, stamp}) to every other
    // renderer that hears it, whether or not its state flipped, and the
    // plain flip to `by` when it did.
    void tellWritten(DocId id, Doc& d, ClientId by, const Json& written);

    void onHello(ClientId c, Message& m);
    // Whether `m` from `c` would change the session while `c` only reads,
    // or go past what a guest may change.
    bool refusedFromViewer(ClientId c, const Message& m) const;
    void onOpen(ClientId c, Message& m);
    void onReset(ClientId c, Message& m);
    void onClose(ClientId c, Message& m);
    void onPath(ClientId c, Message& m);
    void onDetach(ClientId c, Message& m);
    void onEdit(ClientId c, Message& m);
    void onUndo(ClientId c, Message& m);
    void onSaved(ClientId c, Message& m);
    void onForget(ClientId c, Message& m);
    void onWatch(ClientId c, Message& m);
    void onQuery(ClientId c, Message& m);
    // DocSave: the document written to its file by the host
    // (SessionFiles.h's saveDocument), answered DocSaveDone.
    void onSave(ClientId c, Message& m);
    // The workspace (SessionFiles.h): a read in the folder of the session
    // (a headless host's own), else of the window the request names.
    void onRead(ClientId c, Message& m);
    std::string readRoot(std::uint64_t window) const;
    // The workers of the heavier reads, made at the first; their time limit
    // for checks.
    std::unique_ptr<ReadWorkers> workers_;
    double readSeconds_ = -1;
};

// A Host on a thread of its own. Messages from renderers go in with post()
// from any thread, in order per renderer; the host's go out through the
// Sink on the host's thread. MUTIBLE_HOST_DELAY_US-style delays are the
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
    // Watches the disk from now on (Host::setWatching; DiskWatch.h): the
    // host's folder and the folders renderers' trees have open, through the
    // system's watcher, changes coalesced, on the host's thread. Before any
    // renderer connects (the Welcome says it). Where the system has no
    // watcher, the host still says it watches and takes DocReload, but no
    // change ever comes.
    void watchDisk(bool on);
    // For checks: waits (at most `timeoutMs`) until the watcher watches
    // what the host asked of it last; false when it does not, or there is
    // none. *folders: how many it watches (DiskWatch::Watcher::folders).
    bool diskWatchSettled(int timeoutMs, std::size_t* folders = nullptr);
    // Backups of what no app's window shows (Host::keepBackups), on its
    // thread; before any renderer connects.
    void keepBackups(Host::BackupKeeping k);
    // The host's own shell and environment (Host::setOwnShell), on its
    // thread; before any renderer connects.
    void setOwnShell(std::vector<std::string> env, std::string shell);
    // The file its Welcome names (Host::setAskedFile), on its thread; `isNew`
    // when it is not there yet.
    void setAskedFile(std::string file, bool isNew = false);
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
    // The disk, on the host's thread: the system's watcher (made once there
    // is something to watch), the batch it fills, and the roots it was
    // last given.
    bool watchDisk_ = false;
    std::unique_ptr<DiskWatch::Watcher> watcher_;
    DiskWatch::Coalescer coalesce_;
    std::uint64_t watchSeen_ = 0;
    void lookAtWatch();
    void wake();
    void loop();
};

}  // namespace Shared
