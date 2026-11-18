// HtmlPage.cpp — see HtmlPage.h.
#include "HtmlPage.h"

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

}  // namespace HtmlPage
