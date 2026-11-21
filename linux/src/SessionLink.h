// SessionLink.h — the GTK app's links to the session's hosts (SESSION-DESIGN.md,
// stages 4a and 6; the core's SessionHost.h, SessionClient.h and
// SessionServer.h). A host owns every open document's text and undo
// history and every terminal's shell; the editor's documents are its
// mirrors (Document.h records each buffer edit into the client, applies
// what the host sends back, and asks the host for undo), the terminals feed
// VTE what the host forwards, and the language server sync reads the host's
// text through the client's copy of it (LspSessionSync.cpp). The windows'
// tabs are reported as they change.
//
// A host serves one folder, so there is one Link per folder the windows
// show (resolved), shared by every window on it: a window takes its
// folder's link when it is made and again when it opens another folder
// (Editor::setSessionLink), and each document and terminal keeps the link
// it was opened on for its life (so a terminal started before Open Folder
// stays on the old folder's host). The link goes when the last of them lets
// it go. Each link has its own client, in code points (what GtkTextIter
// counts), its messages dispatched on the main loop as they arrive, and its
// ids from that client (Client::newId), so ids of two hosts never meet in
// one client.
//
// Where the host is, read when a link is made (MUTIBLE_HOST wins over the
// setting, for the checks and for forcing one):
//   - MUTIBLE_HOST=inprocess, or unset with editor.session-host off or on:
//     a SessionHost on a thread of this process, one per folder; kept
//     while it holds what windows that closed left (keepWhileHolding). The
//     default until the default flips on both apps together.
//   - MUTIBLE_HOST=loopback: the same, every message through the
//     protocol's encoding and back, both ways (MUTIBLE_HOST_DELAY_US delays
//     each on the host's thread, as a slow link would).
//   - MUTIBLE_HOST=socket, or unset with editor.session-host = headless: the
//     folder's headless host (`mutible host --folder`, SessionServer.h),
//     found by its socket or started as this program, detached, and
//     attached to over the socket. MUTIBLE_HOST_IDLE_MINUTES is passed on
//     as --idle-minutes (checks). When it cannot be had, the in-process host
//     for that folder, with one line on stderr saying why.
//
// A headless host that goes (killed, or its socket closed) is reported once
// on stderr and to the link's lost listeners (the windows' status bars, the
// terminals); its client then sends nothing and waits for nothing
// (Client::hostLost). The next window on the folder, or Reconnect in the
// status bar, finds or starts the host again (forFolder passes a lost link
// over) and moves the windows to it, their buffers opened on it anew.
// Quitting detaches from every headless host (detachAll), which keeps
// running until its lifetime ends.
//
// Windows detach and reattach (stage 6, step 2): a window closing leaves its
// documents and terminals on the host and its record (windowDetached); a
// window opening takes the latest record a closed window left, with what it
// names, and what no record names (takeForWindow).
//
// Saving (stage 7, docs/notes/session-host.md, "GTK's tabs save through the
// host"): every client says in its Hello that the windows save through the
// host (flag 16), and on a host that saves for apps
// (Client::hostSavesForApps) a tab's save is the host's write
// (Document::save), after everything the window asks. The host's notices
// about a document go to the document by its id (listen): its count of it
// as unsaved flipping (DocDirty), and its writes for another renderer (a
// page's save: DocDirty flag 2, Client::hearWrites).
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "SessionClient.h"

namespace Model {
class Window;
}
namespace Shared {
class SessionHost;
class HostLink;
}

namespace SessionLink {

class Link {
public:
    enum class Kind { InProcess, Loopback, Socket };
    ~Link();
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    Shared::Client& client() { return *client_; }
    Kind kind() const { return kind_; }
    // The folder, resolved (as given when it is not one).
    const std::string& folder() const { return folder_; }
    // A headless host's socket, and the pid of the host this link started
    // (0 when it found one running, or for an in-process host).
    const std::string& socketPath() const { return socketPath_; }
    pid_t startedHost() const { return startedHost_; }
    // The headless host went away.
    bool lost() const { return client_->lost(); }
    // For checks: the in-process host, null on a headless one
    // (SessionHost::diskWatchSettled).
    Shared::SessionHost* inProcessHost() const { return host_.get(); }

    // The host's notices about one document of this link's (stage 7): its
    // count of it as unsaved flipped (DocDirty), and it wrote the file for
    // another renderer (DocDirty flag 2, with the host version written and
    // {path, encodingName, stamp}). Called on the main loop inside a
    // dispatch, which a wait (settle) can run in the middle of anything.
    class DocNotices {
    public:
        virtual void hostDirty(bool dirty) = 0;
        virtual void hostWrote(std::uint64_t version, const Json& json) = 0;

    protected:
        ~DocNotices() = default;
    };
    // `n` hears the notices about `doc` (its id on this link's host) until
    // unlisten; unlisten takes it off only while it is `n`'s.
    void listen(Shared::DocId doc, DocNotices* n) { notices_[doc] = n; }
    void unlisten(Shared::DocId doc, DocNotices* n);

    // Told once, on the main loop, when the host goes, with a sentence
    // saying so; the number takes it off again.
    int addLostListener(std::function<void(const std::string& message)> f);
    void removeLostListener(int token);

    // A window's tabs, sent when they differ from what was sent last, with
    // the host terminals its dock shows ("terms", as the Mac's window has)
    // and how they sit in its tabs and panes ("termLayout", the core's
    // Shared::termLayoutJson, TermDockRecord.h; null leaves it out).
    void windowTabs(std::uint64_t window, const Model::Window& w);
    void windowTerminals(std::uint64_t window, const Model::Window& w, std::vector<std::uint64_t> terms,
                         Json layout = Json());
    void windowClosed(std::uint64_t window);
    // What the window's browser pane holds ("browser", the core's
    // Shared::Web::browserReportJson) and which panes it shows ("panes",
    // panesReportJson), sent with its tabs from now on (when they differ)
    // for the session browser's page (SessionBrowser.h); null leaves either
    // out; and its commit and worktree pages ("reviews", reviewsReportJson).
    void windowPanes(std::uint64_t window, const Model::Window& w, Json browser, Json panes, Json reviews = Json());
    // The window's id on the host, the one its tabs are reported under,
    // made now when it has none (the session browser serves only its tabs).
    std::uint64_t hostWindowId(std::uint64_t window) { return hostWindow(window); }
    // A browser's view (the session browser's server, SessionWeb.h's
    // Options::connect) connected to this link's in-process host as one
    // more renderer, the host's messages for it delivered to `client` from
    // then on; returns what lets it go again. Any thread. Null for a
    // headless host, which the view reaches over its socket instead.
    std::function<void()> connectViewer(Shared::Client& client);
    // What the host sent, taken now (dispatched on the main loop otherwise):
    // a page's save waits for every edit it made to be in the tab.
    void dispatch();
    // The window closes and detaches (stage 6, step 2): its record stays on
    // the host, sent once more so it is the latest a closing window left,
    // for the folder's next window to take; none is kept for a window with
    // no tabs and no terminals.
    void windowDetached(std::uint64_t window, const Model::Window& w);

    // What the host holds that a window opening on the folder takes
    // (takeForWindow): the core's rules (Shared::reattachFrom, SessionClient.h:
    // the latest record a closed window left with what it names, then what
    // no record names; never two documents of one path; an untitled buffer
    // too, which the window names, Backups::untitledPath).
    using HeldDoc = Shared::Reattach::Doc;
    using HeldTerm = Shared::Reattach::Term;
    using Held = Shared::Reattach;
    // `window` takes the record over: its tabs are reported under the
    // record's id from now on.
    Held takeForWindow(std::uint64_t window);
    // Everything free (every free record, and what no window's record
    // names), for endDetached.
    Held allFree();

    // For the GUI checks: whether the host's text of each of `docs` (a
    // document's id and its buffer's text) is the buffer's, and the host
    // reported nothing wrong; and whether the window's tabs as the host has
    // them are the window's own. Empty when so, else what differs.
    std::string checkDocuments(const std::vector<std::pair<std::uint64_t, std::string>>& docs);
    std::string checkWindow(std::uint64_t window, const Model::Window& w);

    // Lets go of a headless host: the host forgets this renderer and keeps
    // the rest (quitting). Nothing is sent after it.
    void detach();

private:
    friend std::shared_ptr<Link> forFolder(const std::string& root, bool headless);
    Held free(bool all);
    Json tabsJson(std::uint64_t window, const Model::Window& w) const;
    Link() = default;
    struct Wake;
    void startInProcess(bool loopback, unsigned delayUs);
    bool startSocket(std::string* why);
    void makeClient();
    static int dispatchOnMain(void* wake);   // a GSourceFunc
    std::uint64_t hostWindow(std::uint64_t window);

    Kind kind_ = Kind::InProcess;
    std::string folder_, socketPath_;
    pid_t startedHost_ = 0;
    std::shared_ptr<Wake> wake_;
    // Destroyed in this order (~Link): the transport first, whose thread
    // delivers to the client.
    std::unique_ptr<Shared::Client> client_;
    std::unique_ptr<Shared::SessionHost> host_;   // in process
    std::unique_ptr<Shared::HostLink> socket_;    // headless
    Shared::ClientId hostId_ = 0;
    bool detached_ = false, lostTold_ = false;
    std::map<Shared::DocId, DocNotices*> notices_;   // listen
    std::map<int, std::function<void(const std::string&)>> lostListeners_;
    int nextListener_ = 0;
    // What each window's tabs were last sent as, and each window's id on
    // the host by its id in the app's Model::Session (which numbers windows
    // from 1 for itself): the client's (newId), so a second renderer's
    // windows never take it.
    std::map<std::uint64_t, std::string> sentTabs_;
    std::map<std::uint64_t, std::uint64_t> hostWindow_;
    std::map<std::uint64_t, std::vector<std::uint64_t>> windowTerms_;
    std::map<std::uint64_t, Json> windowLayout_;
    struct Panes {
        Json browser, panes, reviews;
    };
    std::map<std::uint64_t, Panes> windowPanes_;
    // The browsers' views on the in-process host, by their id there: each
    // message the host sends one of them goes to its client, under the
    // lock, so letting one go stops its deliveries at once.
    struct Viewers;
    std::shared_ptr<Viewers> viewers_;
};

// The link to the host of `root`'s folder: the one already made for it,
// else a new one, its host found or started as the environment and
// `headless` (editor.session-host = headless) say. A link whose host went
// is passed over: the folder gets a new one (stage 6, step 2: the windows
// on the old one move to it, Editor::rehost), found or started again.
std::shared_ptr<Link> forFolder(const std::string& root, bool headless);
// Whether the folder's link now has lost its host (a new window, or
// Reconnect, finds or starts it again).
bool folderLost(const std::string& root);

// A window let its link go, detached: an in-process host holding anything
// (documents, terminals, a window's record) is kept, with its thread, while
// the app runs, for the folder's next window; one holding nothing goes. A
// headless host needs nothing kept (it runs on by itself).
void keepWhileHolding(const std::shared_ptr<Link>& link);
// For the GUI checks between suites: everything the links hold that no
// open window shows (free records, their documents and terminals) is
// closed on its host, and kept links holding nothing go, so one suite's
// detached terminals never come back in the next suite's window.
void endDetached();

// Every link there is now (checks).
std::vector<std::shared_ptr<Link>> all();

// Quitting: every headless host let go of; each keeps running until its
// own lifetime ends. In-process hosts kept for the next window go (their
// shells hung up).
void detachAll();

}  // namespace SessionLink
