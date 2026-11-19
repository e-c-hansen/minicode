// HtmlPage.cpp — see HtmlPage.h.
#include "HtmlPage.h"

#include <cctype>

namespace HtmlPage {

const char *const kQuestion = "This page has scripts. Run them?";
const char *const kRun = "Run Scripts";
const char *const kDontRun = "Don't Run";
const char *const kRemember = "Remember for every HTML file";
const char *const kRememberTip = "Writes html.scripts = on or off in the settings file";
const char *const kCloseTip = "Hide this line";

Scripts scripts(Settings::HtmlScripts setting, bool pageHasScripts, int answer) {
    Scripts s;
    switch (setting) {
    case Settings::HtmlScripts::On: s.run = true; return s;
    case Settings::HtmlScripts::Off: return s;
    case Settings::HtmlScripts::Ask: break;
    }
    if (!pageHasScripts) return s;
    if (answer == 1) s.run = true;
    else if (answer != 2) s.ask = true;
    return s;
}

const char *rememberedValue(bool run) { return run ? "on" : "off"; }

std::string answeredLine(bool run, bool settingAsks) {
    if (!settingAsks) return run ? "This page's scripts run." : "This page's scripts stay off.";
    return run ? "This page's scripts run, until mutible quits."
               : "This page's scripts stay off, until mutible quits.";
}

static std::string trimSlashes(std::string s) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

std::string readAccessFolder(const std::string &file, const std::string &root) {
    const size_t slash = file.rfind('/');
    std::string access = slash == std::string::npos || slash == 0 ? "/" : file.substr(0, slash);
    const std::string r = trimSlashes(root);
    if (!r.empty() && (r == "/" || file.compare(0, r.size() + 1, r + "/") == 0)) access = r;
    return access;
}

std::string withoutFragment(const std::string &uri) {
    const size_t hash = uri.find('#');
    return hash == std::string::npos ? uri : uri.substr(0, hash);
}

static bool isOwnOrBlank(const Navigation &nav, const std::string &own) {
    const bool ownPage = nav.scheme == "file" && !nav.hasQuery && !own.empty() && nav.path == own;
    return nav.scheme.empty() || ownPage || nav.scheme == "about";
}

Verdict decide(const Navigation &nav, const std::string &own,
               const std::set<std::string> &requested) {
    if (isOwnOrBlank(nav, own) || !nav.mainFrame) return Verdict::Allow;
    const bool isFile = nav.scheme == "file";
    const bool stale = nav.kind == Navigation::Kind::Reload ||
                       nav.kind == Navigation::Kind::BackForward ||
                       (nav.kind == Navigation::Kind::Other && isFile && requested.count(nav.path));
    if (stale) return Verdict::Drop;
    const bool byUser = nav.kind == Navigation::Kind::Link || nav.kind == Navigation::Kind::Form ||
                        nav.userGesture;
    return byUser ? Verdict::Open : Verdict::Allow;
}

Started started(const Navigation &nav, const std::string &own,
                const std::set<std::string> &requested) {
    if (isOwnOrBlank(nav, own)) return Started::Keep;
    if (nav.scheme == "file" && !nav.hasQuery && requested.count(nav.path)) return Started::Restore;
    return Started::RestoreAndOpen;
}

bool byUser(const Navigation &nav) {
    return nav.kind == Navigation::Kind::Link || nav.kind == Navigation::Kind::Form ||
           nav.userGesture;
}

static bool isWeb(const std::string &scheme) { return scheme == "http" || scheme == "https"; }

bool mayFollow(const Navigation &nav, bool scriptsRun) {
    return byUser(nav) || (scriptsRun && isWeb(nav.scheme));
}

bool loadsInFrame(const std::string &scheme) {
    return scheme.empty() || isWeb(scheme) || scheme == "file" || scheme == "data" ||
           scheme == "blob" || scheme == "about";
}

Policy policy(const Navigation &nav, bool newWindow, bool scriptsRun, const std::string &own,
              const std::set<std::string> &requested) {
    const Verdict v = decide(nav, own, requested);
    if (v == Verdict::Drop) return Policy::Drop;
    if (v == Verdict::Open) return Policy::Open;
    // Allowed: the page's own file, about:blank, a frame, or the page's own
    // doing.
    if (newWindow) {
        if (byUser(nav)) return Policy::Open;   // never a window of its own
        if (nav.scheme.empty() || nav.scheme == "about") return Policy::Drop;
        return mayFollow(nav, scriptsRun) ? Policy::Open : Policy::Refuse;
    }
    return byUser(nav) || loadsInFrame(nav.scheme) ? Policy::Load : Policy::Refuse;
}

std::string refusedNote(const std::string &url) {
    std::string where = url;
    // 79 characters and an ellipsis, cut on a character's first byte.
    size_t chars = 0, cut = std::string::npos;
    for (size_t i = 0; i < where.size(); i++) {
        if (((unsigned char)where[i] & 0xC0) == 0x80) continue;
        if (chars == 79) cut = i;
        if (++chars > 80) break;
    }
    if (chars > 80 && cut != std::string::npos) where = where.substr(0, cut) + "\xE2\x80\xA6";
    const size_t colon = url.find(':');
    std::string scheme = colon == std::string::npos ? "" : url.substr(0, colon);
    for (char &c : scheme) c = (char)tolower((unsigned char)c);
    return isWeb(scheme)
               ? "The page tried to open " + where + " by itself; with its scripts off, it was not followed"
               : "The page tried to open " + where + " by itself; mutible did not open it";
}

}  // namespace HtmlPage
