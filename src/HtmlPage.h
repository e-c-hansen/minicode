// HtmlPage.h — the rules for an .html file shown as its page (the GTK port's
// HtmlPage; the Mac's MCHtmlPage in mac/src/HtmlPage.mm keeps its own copy of
// the same rules). No I/O and no view: the port asks, then shows.
//
// Scripts follow `html.scripts` (Settings.h): `on` and `off` apply unasked;
// `ask` (the default) lets a page that has nothing to run load as it is, and
// asks about one that does (FileSniff::htmlMayRunScripts), once for each file
// until minicode quits unless the answer is remembered in the settings file.
//
// A page loads from its own file only. Anything the page tries to open in
// its main frame (a link, window.open, a script's location change) is not
// loaded there: a local file or a web address goes to the port's link
// handler, so the page on show is always the tab's file. A load this page
// asked for earlier, a reload and a back or forward are dropped rather than
// taken for a link, since a policy check for them can arrive after the tab
// has moved to another file.
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

}  // namespace HtmlPage
