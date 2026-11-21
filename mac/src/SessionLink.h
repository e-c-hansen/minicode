// SessionLink.h — Objective-C++. The Mac's side of the session's host
// (SESSION-DESIGN.md, stage 4b; the core's SessionHost.h, SessionClient.h
// and Ot.h). With editor.session-host on, each tab's text, version and undo
// history belong to a Shared::SessionHost running on a thread of its own in
// the app's process, and the tab's text (the editor's storage while it is
// on show, the tab's own string while it is not) is the host's mirror:
//
//   - every change of the mirror goes to the host as it is made, from the
//     storage (MCSourceStorage willReplaceCharacters, before the store
//     changes, so a surrogate pair is never split) or, for a text set
//     whole (a viewer's edit, a preview's, a reload), as the one range in
//     which the two differ (MCTab setSourceText:); a storage edit outside
//     a change of the text view's (CodeTextView sessionChanging) is in no
//     undo step, and the host's history moves past it;
//   - the edits of one run-loop turn are one undo step, as NSUndoManager's
//     grouping by event made them, named by the first name given
//     (replaceRanges:withStrings:actionName:, setActionName:), else
//     "Typing" (joining the step before while the caret stays put) for
//     what is typed and "Edit" for the rest;
//   - undo and redo ask the host (the tab's undo manager, MCTabUndoManager
//     in EditorTab.mm, and CodeTextView undo:, redo:), whose answer comes
//     back as an edit put into the storage through
//     replaceRanges:withStrings:actionName:, recording nothing (and with
//     the tab's undo registration off); what NSTextView and the editor
//     register still goes on NSUndoManager's stacks, never undone then;
//   - an edit the host sends while the storage is in the middle of an edit
//     of its own (inside processEditing, a storage notification, a wait
//     that dispatched it there) waits for the next run-loop turn, and an
//     edit made meanwhile is moved past it (Ot::transform).
//
// Links (stage 6, docs/notes/session-host.md, "The Mac reattaches"). A
// link is one client of one host, in UTF-16 units (what NSString counts),
// its messages dispatched on the main queue as they arrive:
//   - the process's link: a SessionHost on a thread of this process, one for
//     every window (editor.session-host = on, and the fallback);
//   - a folder's link (editor.session-host = headless, or MUTIBLE_HOST=
//     socket): the folder's `mutible host`, found by its socket or started
//     from Contents/MacOS/mutible-cli, detached, and attached to over the
//     socket; one per folder, shared by the windows on it;
//   - a remote link (stage 8, `mutible ssh://host/path`, forRemote): `mutible
//     host --stdio` on another machine through the user's own ssh, attached
//     to over ssh's pipes; its window's files are read (readText) and saved
//     there, its terminals run there, its tree, Quick Open and Find in
//     Folder read there (the window's MCRemoteWorkspace: the disk's notices
//     through addDiskListener, the heavier reads through stream), and
//     closing the window ends it.
// Each document and terminal keeps the link it was opened on. A window
// closing detaches: its documents, terminals and its record (its tabs)
// stay on the host, and a window opening on the folder takes the latest
// record a closed one left (takeForWindow). A headless host that goes is
// told to the link's lost listeners once; Reconnect (or the folder's next
// window) finds or starts it again, and every window on the old link moves
// to the new one, its buffers opened there anew.
//
// MUTIBLE_HOST=loopback sends every message of the process's link through
// the protocol's encoding and back; MUTIBLE_HOST_DELAY_US delays each one on
// the host's thread; MUTIBLE_SESSION_HOST=on turns the setting on for every
// tab (the GUI checks' second pass); MUTIBLE_HOST_IDLE_MINUTES is passed to
// a host started here as --idle-minutes (checks). A host started here gets
// MUTIBLE_BACKUP_DIR (MCSessionHostEnvironment), and every client of the
// app's says in its Hello that its windows keep backups (stage 6, step 3).
//
// Saving (stage 7, docs/notes/session-host.md, "The Mac's tabs save through
// the host"): on a host that saves for apps (Client::hostSavesForApps) a
// tab's save is the host's write (saveOnHostEncoding:bom:), after the
// window has asked every question it asks, and every client of the app's
// says so in its Hello (flag 16), so a page's save of a file a window shows
// is the host's to make too. The window hears when the host wrote the file
// for someone else (MCTabOwner's tab:writtenByHost:) and when the host's
// count of the tab as unsaved flips (tab:dirtyFromHost:).
#pragma once
#import <Cocoa/Cocoa.h>

#include <sys/types.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "FolderSearch.h"
#include "Json.h"
#include "SessionClient.h"
#include "SessionServer.h"

@class MCTab, CodeTextView, MCDiskText;

// editor.session-host (on or headless), or MUTIBLE_SESSION_HOST=on.
BOOL MCSessionHostEnabled(void);
// The process's client, the host started with it (the process's link's).
Shared::Client &MCSessionClient(void);

namespace MCSession {

// What a host holds that a window opening on its folder takes
// (Link::takeForWindow), as GTK's SessionLink has it.
struct HeldDoc {
    std::uint64_t id = 0;
    std::string path;
    bool dirty = false;
};
struct HeldTerm {
    std::uint64_t id = 0;
    int pid = 0;
    int cols = 0, rows = 0;
    Shared::TermShell shell;
};
struct Held {
    std::uint64_t window = 0;   // the record taken, 0 for none
    Json tabs;                  // its tabs, null for none
    std::vector<HeldDoc> docs;
    std::vector<HeldTerm> terms;
    std::vector<std::uint64_t> records;   // allFree: every free record
};

class Link {
public:
    // InProcess: the process's host. Socket: a folder's headless host on
    // this Mac. Remote: `mutible host --stdio` on another machine, through
    // the user's ssh (stage 8, forRemote).
    enum class Kind { InProcess, Socket, Remote };
    ~Link();
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    Shared::Client& client() { return *client_; }
    Kind kind() const { return kind_; }
    // The folder, resolved; "" for the process's link, which serves every
    // window.
    const std::string& folder() const { return folder_; }
    const std::string& socketPath() const { return socketPath_; }
    // The pid of the host this link started (0 when it found one running,
    // or in process).
    pid_t startedHost() const { return startedHost_; }
    bool lost() const { return client_->lost(); }
    // A headless host, there: what a closing window leaves unsaved stays
    // after the app is gone (Keep Unsaved).
    bool keepsUnsaved() const { return kind_ == Kind::Socket && !lost(); }
    // Which host this is, for moving windows to its replacement: the folder
    // for a link on this Mac, the machine's ssh:// and the folder for a
    // remote one (a folder there may be spelled as one here is).
    std::string key() const;

    // ---- a remote link (Kind::Remote)
    // Its address as given (ssh://...), its title's "host:path" (the folder
    // as the address names it, ~ kept), and the file the address named
    // (spelled under folder(), as the host resolved it), "" for a folder.
    const std::string& address() const { return address_; }
    const std::string& label() const { return label_; }
    const std::string& askedFile() const { return askedFile_; }
    // Why the connection went (ssh's status and last words, in the
    // window's words), once lost.
    const std::string& failure() const { return failure_; }
    // A file's text read through the host (FsRead "read", piece by piece,
    // decoded as the Mac decodes a file): nil text when it cannot be read
    // or does not read as text (MCDiskText's rules), tooLarge past 64 MB.
    // Never a stamp: the file is on the other machine. Waits on the host.
    MCDiskText *readText(const std::string &path);
    // The address named a file not there yet (the Welcome's "new"): the
    // window opens it empty, and its first save makes it.
    bool askedFileNew() const { return askedFileNew_; }
    // What the remote host saw change on disk (FsChanged's json: {root,
    // listings, git}, for the folders Client::watchFolders named), told on
    // the main queue inside dispatch() (perhaps inside a wait: a listener
    // that reads the host again defers itself). The number takes it off.
    int addDiskListener(std::function<void(const Json &changed)> f);
    void removeDiskListener(int token);
    // A heavier read on the host's workers (SessionSearch.h: "files",
    // "search"): `answers` gets each batch of answers as they are
    // dispatched, `done` once the last came (and never again). 0, calling
    // nothing, when the host does not take them or is gone.
    std::uint64_t stream(Json request, std::function<void(std::vector<Json> &&answers, bool done)> answers);
    // Stops one: nothing more is told for it.
    void cancelStream(std::uint64_t req);

    // Told once, on the main queue, when the host goes; the number takes it
    // off again.
    int addLostListener(std::function<void()> f);
    void removeLostListener(int token);

    // ---- windows (Client::windowTabs): an open window's id on the host,
    // its record sent when it differs from what was sent last, and on
    // closing either kept (marked "detached", the latest for the folder's
    // next window) or closed.
    std::uint64_t newWindow();
    void windowTabs(std::uint64_t window, Json tabs);
    void windowDetached(std::uint64_t window, Json tabs, bool keep);
    void windowClosed(std::uint64_t window);
    // What a window opening on `root` takes: the latest free record (one no
    // open window has, whose renderer is gone or is this app's) with its
    // documents and terminals, then held documents (with a path, under
    // `root` for the process's link) and, on a folder's host, terminals that
    // no record names. `window` becomes the record's id when one is taken.
    Held takeForWindow(std::uint64_t* window, const std::string& root);
    // Everything free (the checks' endDetached).
    Held allFree();

    // Lets go of a headless host: it forgets this renderer and keeps the
    // rest (quitting). Nothing is sent after it.
    void detach();
    void dispatch();

private:
    friend std::shared_ptr<Link> forFolder(const std::string& root);
    friend std::shared_ptr<Link> processLink();
    friend std::shared_ptr<Link> forRemote(const std::string& address, std::string* why);
    Link() = default;
    Held free(bool all, const std::string& root);
    bool startSocket(std::string* why);
    bool startRemote(std::weak_ptr<Link> self, std::string* why);
    // The client made and spoken through `link`, its messages dispatched on
    // the main queue through `self`; false (nothing kept) when the host's
    // Welcome does not come within `timeoutMs`.
    bool attachClient(std::unique_ptr<Shared::HostLink> link, std::weak_ptr<Link> self, int timeoutMs);

    Kind kind_ = Kind::InProcess;
    std::string folder_, socketPath_;
    std::string address_, label_, machine_, askedFile_, failure_;
    Shared::SshTarget target_;
    pid_t startedHost_ = 0;
    Shared::Client* client_ = nullptr;            // the process's, or owned_'s
    std::unique_ptr<Shared::Client> owned_;
    std::unique_ptr<Shared::HostLink> socket_;
    std::shared_ptr<std::atomic<bool>> queued_;
    bool detached_ = false, lostTold_ = false, askedFileNew_ = false;
    std::map<int, std::function<void()>> lostListeners_;
    std::map<int, std::function<void(const Json &)>> diskListeners_;
    std::map<std::uint64_t, std::function<void(std::vector<Json> &&, bool)>> streams_;
    int nextListener_ = 0;
    void pumpStreams();
    std::set<std::uint64_t> live_;                 // open windows' ids
    std::map<std::uint64_t, std::string> sent_;   // each one's record as sent
};

// The link for a window on `root`: the folder's headless host (found or
// started) with editor.session-host = headless or MUTIBLE_HOST=socket, else
// the process's. A folder's link whose host went is passed over: a new one
// is made (the windows on the old one move to it).
std::shared_ptr<Link> forFolder(const std::string& root);
std::shared_ptr<Link> processLink();
// A window over SSH (stage 8): `address` is ssh://[user@]host[:port]/path.
// The live link for that machine and path when there is one, else a new
// one: the user's ssh ($MUTIBLE_SSH names another program, for the checks)
// running `mutible host --stdio <path>` there, attached to as a renderer
// over its pipes. Null, with why in the window's words (ssh needed a
// password or a host key confirmed, mutible missing or too old there, ssh's
// own last line), when it cannot be had. Waits for the host's Welcome (at
// most 30 s: ssh's connection).
std::shared_ptr<Link> forRemote(const std::string& address, std::string* why);
// The live folder link whose folder holds `path`, else the process's: for a
// document or terminal made outside a window's own calls.
std::shared_ptr<Link> linkForPath(const std::string& path);
// Whether `root`'s folder link lost its host.
bool folderLost(const std::string& root);
// Every link there is (the process's first, when made).
std::vector<std::shared_ptr<Link>> all();
// Quitting: every headless host let go of; each runs on until its own
// lifetime ends.
void detachAll();
// For the GUI checks between suites: everything the links hold that no
// open window shows is closed on its host, so one suite's detached
// documents and terminals never come back in the next suite's window.
void endDetached();

}  // namespace MCSession

// A link carried by Objective-C objects (a tab's document, a terminal, a
// window).
@interface MCSessionLinkRef : NSObject
- (instancetype)initWithLink:(std::shared_ptr<MCSession::Link>)link;
@property(nonatomic, readonly) std::shared_ptr<MCSession::Link> link;
@end

// A terminal's place on a host: its link, and the held terminal it takes
// over when it opens (0: it starts a shell).
@interface MCSessionTermPlace : NSObject
@property(nonatomic, strong) MCSessionLinkRef *link;
@property(nonatomic, assign) uint64_t heldTerm;
@property(nonatomic, assign) int heldPid, heldCols, heldRows;
@property(nonatomic, assign) Shared::TermShell heldShell;
@end
BOOL MCSessionLoopback(void);
// The environment a `mutible host` started here gets: this process's, with
// MUTIBLE_BACKUP_DIR set to the app's backup store (MCBackupStore), so the
// host's backups of what no window shows are the ones the app restores.
NSDictionary<NSString *, NSString *> *MCSessionHostEnvironment(void);
// Another renderer of the process's host (a browser's view, SessionBrowser.h,
// on a thread of its own): `client` says Hello through it, and the host's
// messages for it go to Client::deliver from then on. What it returns lets
// it go: nothing is delivered to `client` after it returns, and the host
// forgets the renderer (any thread).
std::function<void()> MCSessionConnectViewer(Shared::Client &client);

// A host's save of a tab's document, as the window takes it.
struct MCHostSave {
    bool answered = false;   // the host answered in time
    bool ok = false;
    std::string reason, why, note;   // SessionFiles.h's SaveDone
    FolderSearch::TextEncoding encoding = FolderSearch::TextEncoding::Utf8;
    bool bom = false;
    FolderSearch::FileStamp written;
    bool created = false;
    // The text written is the tab's text now: the host wrote the version
    // the mirror is at, with nothing of anyone's since.
    bool inStep = false;
};

// The host wrote a tab's file for another renderer (a page's save): what is
// on disk now. `text` is the text written when the tab's text was the
// host's at that version (nil when it was not: the window reads the file).
@interface MCSessionWrite : NSObject
@property(nonatomic, copy) NSString *text;
@property(nonatomic, copy) NSString *path;
@property(nonatomic, assign) BOOL knowsEncoding;
@property(nonatomic, assign) FolderSearch::TextEncoding encoding;
@property(nonatomic, assign) BOOL bom;
@property(nonatomic, assign) FolderSearch::FileStamp stamp;
@end

// One tab's document on the host (made by MCTab when its text is first set
// with the setting on, closed with the tab). Its link is its window's
// (the tab's owner's sessionLinkForTab:), else the one for its path.
@interface MCSessionDoc : NSObject
- (instancetype)initWithTab:(MCTab *)tab text:(NSString *)text;
// A document the host holds already (a window opening takes what a closed
// one left), under its id: the host's text is heldText once this returns
// (nil when the host did not answer).
- (instancetype)initWithTab:(MCTab *)tab held:(Shared::DocId)docId link:(MCSessionLinkRef *)link;
@property(nonatomic, readonly) NSString *heldText;
@property(nonatomic, readonly) Shared::DocId docId;
@property(nonatomic, readonly) MCSessionLinkRef *link;
// Whether it is open on its host now (not detached, closed or lost).
@property(nonatomic, readonly) BOOL open;
// The window closes and detaches: the host keeps the document, its text and
// history (Client::detach); closing the tab then closes nothing there.
- (void)detach;
// The tab's file is now at its path (Save As, a rename): the host is told.
- (void)pathChanged;
// Its link's host went and `link` is the one found or started since: the
// document is opened on it anew under a new id from the tab's text, with
// `saved` (the file's text as last read or saved, nil when unknown) as the
// host's saved text and the difference an edit outside the history, so the
// host counts it unsaved (stage 6, step 2, as GTK's Document::rehost).
- (void)rehostOn:(MCSessionLinkRef *)link saved:(NSString *)saved;
// The host's saved text made `saved` when the tab's text differs from it:
// the host's copy is reset to it (its history gone) and the difference goes
// as an edit outside the history, so the host counts the buffer unsaved and
// a headless host's lifetime and backups keep it (GTK's
// Document::hostSavedFromDisk). `saved` is the file's text as last read or
// saved, "" for a file not on disk (all of the buffer unsaved); nil does
// nothing. For a backup restored, a held buffer whose file is gone, and
// rehostOn:saved:.
- (void)hostSaved:(NSString *)saved;
@property(nonatomic, weak, readonly) MCTab *tab;
// The host's edit is going into the mirror: nothing is recorded.
@property(nonatomic, readonly) BOOL applying;
// Edits from the host waiting for the storage to finish an edit of its own.
@property(nonatomic, readonly) NSUInteger deferredEdits;

// ---- the mirror's changes
// The storage is about to replace `range` with `string` (the store still
// holds the old text). `view` shows it.
- (void)storageWillReplace:(NSRange)range with:(NSString *)string view:(CodeTextView *)view;
// The tab's text is being set whole, from `old` to `now`.
- (void)textWillChangeFrom:(NSString *)old to:(NSString *)now;
// The step being recorded takes this name (when it has none yet), or a new
// step opens with it.
- (void)nameStep:(NSString *)name;
// replaceRanges:withStrings:actionName:'s edit named `name` (nil: by what
// is going on) begins and ends: a step of its own, as its undo group was,
// unless it is inside a group or follows this turn's typing.
- (void)beginNamed:(NSString *)name view:(CodeTextView *)view;
- (void)endNamed;
- (void)breakCoalescing;
- (void)noteSelection:(NSArray<NSValue *> *)ranges;
// The text was read afresh: nothing to undo, the host's copy starts again
// from `text`; a text set whole in this run-loop turn starts it again too.
- (void)forgetHistoryWithText:(NSString *)text;
// The step of this turn, closed now (before an undo, or asking about one).
- (void)closeStep;
// The undo history forgotten, the text kept (the tab's undo manager's
// removeAllActions).
- (void)forgetHistory;
// The tab's text was written to its file.
- (void)saved;
// The file holds `text`, not the tab's text (Keep Mine over a file changed
// on disk, a held tab's file read): the host counts the tab unsaved against
// it, as the window does. NO, telling nothing, when the host would not
// take it (Client::savedAs).
- (BOOL)savedAs:(NSString *)text;
// Whether the host has yet to take a saved or savedAs: until it has, what
// it says of the tab's unsaved state is held (Client::saved's fence).
@property(nonatomic, readonly) BOOL savePending;
// Whether the host writes this document's file: open on a host that saves
// for apps (its Welcome's flag 2), not lost.
@property(nonatomic, readonly) BOOL hostSavesFile;
// The host writes the document's text as the tab has it (the turn's step
// closed and every edit sent first) to its file, in `encoding` with `bom`,
// over whatever is on disk: the window asked its questions about the disk
// first. Waits for the answer (in one process a round trip through the
// host's thread; a headless host's over its socket), at most 10 s.
- (MCHostSave)saveOnHostEncoding:(FolderSearch::TextEncoding)encoding bom:(BOOL)bom;

// ---- undo
- (BOOL)canUndo:(BOOL)redo name:(NSString **)name;
// An undo (1) or redo (2) of the host's is going into the text, else 0.
@property(nonatomic, readonly) int undoing;
// How many undos and redos the host has made of this document.
@property(nonatomic, readonly) NSUInteger undoCount;
// The host takes its last step back (or puts it back); the edit is in the
// view before this returns. NO when there was nothing to do or it was
// refused.
- (BOOL)undo:(BOOL)redo inView:(CodeTextView *)view;

// ---- the host's text, for the language server sync
// A copy of the host's text kept from the host's own edits, everything
// sent so far applied; null when the host does not answer. `owner` lets
// it go with releaseHostTextFor:.
- (const Model::Text *)hostTextFor:(const void *)owner;
- (void)releaseHostTextFor:(const void *)owner;

- (void)close;
@end

// For the GUI checks: whether the host's text of each of `tabs` with a
// document on the host is the tab's text, and the host reported nothing
// wrong. Empty when so, else what differs.
NSString *MCSessionCheckTabs(NSArray<MCTab *> *tabs);

// For the GUI checks: what the host has of terminal `term` now (on its
// thread, after everything posted before): its pty master (-1 once hung
// up or gone), shell, whether bytes wait to be written to it and whether
// it is read, how many hung-up shells wait to be reaped, and its screen's
// scrollback and rows down to the last with text.
struct MCSessionTerminalState {
    int fd = -1;
    int pid = 0;
    bool writeWaiting = false, reading = false;
    size_t dying = 0;
    std::string screen;
};
MCSessionTerminalState MCSessionInspectTerminal(unsigned long long term);

