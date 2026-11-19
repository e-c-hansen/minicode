// HtmlPage.h — the rules for an .html file shown as its page (the GTK port's
// HtmlPage; the Mac's MCHtmlPage in mac/src/HtmlPage.mm keeps its own copy of
// the same rules). No I/O and no view: the port asks, then shows.
//
// Scripts follow `html.scripts` (Settings.h): `on` and `off` apply unasked;
// `ask` (the default) lets a page that has nothing to run load as it is, and
// asks about one that does (FileSniff::htmlMayRunScripts), once for each file
// until mutible quits unless the answer is remembered in the settings file.
//
// A page loads from its own file only. Anything the page tries to open in
// its main frame (a link, window.open, a script's location change) is not
// loaded there: a link the user clicked or a form they sent goes to the
// port's link handler (a local file in a tab, a web address in the browser
// pane), so the page on show is always the tab's file. One the page started
// by itself (a meta refresh, a script) goes there only as `mayFollow` says:
// a web address while the page's scripts run; anything else is refused and
// named in the status bar (`refusedNote`). A load this page asked for
// earlier, a reload and a back or forward are dropped rather than taken for
// a link, since a policy check for them can arrive after the tab has moved
// to another file.
#pragma once

#include <set>
#include <string>

#include "Settings.h"

namespace HtmlPage {

// The wording of the bar (the Mac's).
extern const char *const kQuestion;       // "This page has scripts. Run them?"
extern const char *const kRun;            // "Run Scripts"
extern const char *const kDontRun;        // "Don't Run"
extern const char *const kRemember;       // "Remember for every HTML file"
extern const char *const kRememberTip;    // what the box writes
extern const char *const kCloseTip;       // "Hide this line"

struct Scripts {
    bool run = false;   // the page loads with its scripts on
    bool ask = false;   // the bar asks
};

// What the setting, the page's text and the answer so far (0 none, 1 run,
// 2 do not run) make of it.
Scripts scripts(Settings::HtmlScripts setting, bool pageHasScripts, int answer);

// The value a remembered answer writes to html.scripts.
const char *rememberedValue(bool run);

// The line left in the bar's place once a question is answered, so the page
// does not move under the pointer. `settingAsks` is html.scripts = ask (the
// answer then holds until quit).
std::string answeredLine(bool run, bool settingAsks);

// The folder the page may read: the window's when the file is inside it (or
// it is "/"), else the file's own folder. Both without a trailing slash.
std::string readAccessFolder(const std::string &file, const std::string &root);

// The address without its #fragment, for telling a reload of the page's own
// file from a link to a place in it.
std::string withoutFragment(const std::string &uri);

// What a navigation the page asks for is.
struct Navigation {
    std::string scheme;        // lower case, "" when none
    std::string path;          // a file: address's path, decoded
    bool hasQuery = false;
    // Whether the navigation is in the main frame. The GTK port cannot tell
    // (a frame's name is no sign), so it leaves this true and relies on the
    // user's gesture, and on `started` for the main frame.
    bool mainFrame = true;
    bool userGesture = false;   // a click or a key made it
    enum class Kind { Link, Form, Reload, BackForward, Other } kind = Kind::Other;
};

enum class Verdict {
    Allow,    // the page loads it (its own file, about:blank, a frame inside it)
    Drop,     // not loaded and not opened: a stale load
    Open,     // not loaded here; the link handler opens it
};

// `own` is the file on show; `requested` the files this page was asked to
// load (their paths).
// Only a navigation the user made (a clicked link, a submitted form, a
// gesture) leaves the page here; any other (a frame loading its address, a
// script's) is allowed, and a script's in the main frame is caught by
// `started` when the load begins.
Verdict decide(const Navigation &nav, const std::string &own,
               const std::set<std::string> &requested);

// The main frame has started loading `nav` (the scheme, path and query of
// its address): Keep what is the page's own file or about:blank; otherwise
// stop it and put the page back, and Open the address, or only restore it
// when it is a load this page asked for earlier (stale).
enum class Started { Keep, Restore, RestoreAndOpen };
Started started(const Navigation &nav, const std::string &own,
                const std::set<std::string> &requested);

// Whether the user made the navigation: a clicked link, a submitted form, or
// one a click or a key started (decide()'s test for leaving the page).
bool byUser(const Navigation &nav);

// Whether a navigation leaving the page may go to the link handler (the
// Mac's MCHtmlMayFollow): one the user made, always; one the page started
// by itself (a <meta http-equiv="refresh">, a script setting location,
// window.open) only to a web address (http, https), and only while the
// page's scripts run, since such a page acts on its own as a script would.
// A meta refresh to zoommtg:// opened that app with no click (round 10's
// documents defect 2): html.scripts never applied, a refresh not being a
// script.
bool mayFollow(const Navigation &nav, bool scriptsRun);

// Whether a frame can load an address of this scheme at all: http, https,
// file, data, blob, about (and none). Any other (zoommtg:, smb:, mailto:)
// could only be handed to another app.
bool loadsInFrame(const std::string &scheme);

// The GTK port's answer to a navigation's policy check, where a frame cannot
// be told from the main frame: decide()'s, except for one the page started
// by itself. In the page's view, one to an address no frame can load is
// Refused at once (nothing loads; the page stays as it is); one a frame
// could load is let through, and the main frame's is caught when its answer
// arrives (the port's response check, then `started`). In a new window
// (window.open), it is Opened as mayFollow says, else Refused, and a blank
// one is Dropped; the user's (a target=_blank link clicked) is Opened.
enum class Policy { Load, Drop, Open, Refuse };
Policy policy(const Navigation &nav, bool newWindow, bool scriptsRun, const std::string &own,
              const std::set<std::string> &requested);

// The status bar's note for a navigation the page started by itself that was
// not followed (the Mac's htmlPageRefused: wording), the address cut to 80
// characters.
std::string refusedNote(const std::string &url);

}  // namespace HtmlPage
