// SessionBrowser.h — the GTK port's View > Show Session in Browser… and Copy
// Session Browser Link (the Mac's mac/src/SessionBrowser.h): the browser's
// view of a session (the core's SessionWeb.h) served for one window's tabs
// from the window's own host (SessionLink.h), so the page works on this
// computer without `mutible host`. Editable (the choice Show asks for), the
// page types into the window's documents through the host, undoes through
// its history, and saves through the window's own Ctrl+S; read-only, it
// only looks.
//
// One server per window, made the first time it is asked for and kept while
// the window lives: 127.0.0.1 on a free port, with Web::answer's Host and
// Origin checks. Each Show and Copy makes a fresh link, good for one browser
// (SessionWebAccess.h): the page trades its token for a cookie and takes it
// out of the address bar, and the credential ends after
// editor.session-web-idle-minutes with nothing from its browser. A browser
// this computer does not remember waits while a dialog on the window asks
// (Allow, Deny, Always Allow), naming the browser and where it connects
// from (this computer, or the far end of an SSH tunnel into it, read from
// /proc); Always Allow remembers its id in
// $XDG_STATE_HOME/mutible/session-browser.json, which View > Remembered
// Browsers… lists and forgets. The view connects to the window's host as a
// renderer that only reads, or as a guest that may edit, while a browser is
// connected: to an in-process host directly (Link::connectViewer), to a
// headless one over its socket.
//
// While the server runs the window reports its tabs, its own terminals, its
// browser pane's tabs and which panes it shows to the host every half
// second (Host::report, which sends only what changed), and the page shows
// only those; until the window has a terminal on the host the page says so
// in one muted line (kTermNote). On an editable page a click on a file in
// the page's tree opens it in the window as a click in the window's tree
// does, a tab's close button closes the window's tab (never one with unsaved
// changes), and an http or https address opens in the window's browser pane
// (nothing in a build without the pane). The server's callbacks come on its
// own thread and are carried to the main loop; an answer reaches only the
// server that asked (a generation number). The server stops when the window
// closes, before the window's tabs leave the host's state (main.cpp's
// detachWindow), or when the page's editable choice changes.
//
// A page showing a LaTeX document gets its typeset from the window: from the
// open tab's preview when there is one (its last PDF, then working or
// failed, and every later typeset), else from a preview of the server's own
// made without a tab, at most four kept (the oldest stopped first). The
// pages are drawn as pictures with poppler on the server's threads. The
// window calls gitChanged when its git or the folder's changed, and the
// page's Source Control reads again.
//
// Left for later: the review pages, which the Mac's has.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

typedef struct _GtkWindow GtkWindow;
typedef struct _GBytes GBytes;
class LatexPreview;

namespace SessionLink {
class Link;
}
namespace Shared {
namespace Web {
class Server;
}
}

class SessionBrowser {
public:
    // What the window does for the page, each called on the main loop.
    struct Host {
        // Saves the window's document with this id on the host as Ctrl+S
        // does (an editable page's save).
        std::function<void(std::uint64_t docId)> save;
        // Opens a file of the window's folder as a click in its tree does
        // (`keep` for a kept tab rather than the preview tab); the core held
        // the path inside the folder.
        std::function<void(const std::string& path, bool keep)> open;
        // Closes the window's tab for a document on the host; the core has
        // refused a tab with unsaved changes, and the window checks again.
        std::function<void(std::uint64_t docId)> close;
        // Opens an http or https address in the window's browser pane; unset
        // in a build without the pane (the page then offers no address).
        std::function<void(const std::string& url)> browse;
        // Sends the window's record to the host now (only what changed):
        // its tabs, terminals, browser tabs and panes.
        std::function<void()> report;
        // The window's LaTeX preview of the file at `path`, or null when no
        // tab has one. Unset in a build without the PDF viewer.
        std::function<LatexPreview*(const std::string& path)> previewOf;
        // The text a typeset of `path` reads: the open tab's, else the
        // file's; false when it cannot be read.
        std::function<bool(const std::string& path, std::string* text)> source;
    };

    SessionBrowser(GtkWindow* window, Host host);
    ~SessionBrowser();   // stops
    SessionBrowser(const SessionBrowser&) = delete;
    SessionBrowser& operator=(const SessionBrowser&) = delete;

    // Whether the page may edit. Changing it stops a server that runs; the
    // next start serves anew, at a new address.
    bool editable() const { return editable_; }
    void setEditable(bool editable);

    // Starts serving the window's tabs on `link`'s host if it is not yet
    // (`window` the window's id in the app's session, Editor::sessionWindowId):
    // false, with why, when it cannot. A server on another link than
    // `link` (the window moved to a new host) stops first.
    bool start(const std::shared_ptr<SessionLink::Link>& link, std::uint64_t window, bool showHidden,
               const std::string& settingsPath, std::string* error);
    bool running() const { return server_ != nullptr; }
    // A fresh link, http://127.0.0.1:PORT/v/ID/?t=TOKEN, good for one
    // browser; "" until started.
    std::string newLink();
    // The last link newLink made, "" until one was.
    const std::string& url() const { return url_; }
    int port() const;
    int browsers() const;      // connected now
    std::size_t asking() const;   // browsers waiting for the window's answer
    std::uint64_t hostWindow() const { return hostWindow_; }
    // The window's record sent now, and the page's view looks at once.
    void reportTabs();
    // The settings file changed: the page's colors follow now.
    void reloadSettings();
    // The window's git, or the folder's, changed: the page's Source Control
    // reads again (at most once a second).
    void gitChanged();
    // A preview's typeset state, for the page: began (no `pdf`, no `error`),
    // made `pdf`, or failed with `error`.
    void typesetOf(const std::string& path, GBytes* pdf, const std::string& error);
    void stop();
    // For checks: how many typesets the server keeps for pages with no tab.
    std::size_t hiddenTypesets() const { return hidden_.size(); }

    // The line the page shows where terminals would be.
    static const char* const kTermNote;
    // Checks: when set, a browser's dialog is shown only once this calls
    // what it is given, so a check can see the page wait first; and what
    // opens the page in Show (the system's URI launcher otherwise).
    static std::function<void(std::function<void()> ask)> askDelay;
    static std::function<void(const std::string& url)> opener;

    // Opens `url` with the system's handler (or `opener`).
    static void openInBrowser(GtkWindow* parent, const std::string& url);

    // The browsers Always Allow remembered (any thread), forgetting them,
    // and View > Remembered Browsers…, which lists them with Forget All.
    struct Remembered {
        std::string id, browser, address;
        std::int64_t since = 0;   // seconds since the epoch
    };
    static std::vector<Remembered> remembered();
    static void forgetAll();
    static void showRemembered(GtkWindow* parent);
    // Show's last answer: editable until one is given.
    static bool editableChoice();
    static void setEditableChoice(bool editable);
    // Where the remembered browsers and the choice are kept.
    static std::string stateFile();

    // Where a connection to the server on `serverPort` from loopback port
    // `peerPort` comes from: "this computer", or "ADDRESS, through an SSH
    // tunnel" when an sshd of the user's holds the other end (any thread).
    static std::string peerOf(int peerPort, int serverPort);

private:
    struct Guard;
    struct Hidden;
    // The page asked for a typeset of `path` (on the main loop).
    void typesetFor(const std::string& path);
    void tellPreview(const std::string& path, LatexPreview* preview);
    void stopHidden();
    void askAbout(std::uint64_t question, const std::string& browser, const std::string& browserId,
                  const std::string& from, const std::string& peer, unsigned generation);

    GtkWindow* window_;
    Host host_;
    bool editable_ = true;
    std::unique_ptr<Shared::Web::Server> server_;
    std::thread thread_;
    std::shared_ptr<SessionLink::Link> link_;
    std::shared_ptr<Guard> guard_;
    unsigned generation_ = 0;   // one per server started
    unsigned timer_ = 0;        // the half-second report
    std::uint64_t windowId_ = 0, hostWindow_ = 0;
    std::string url_;
    // Typesets for pages with no tab's preview, oldest first.
    std::vector<std::shared_ptr<Hidden>> hidden_;
};
