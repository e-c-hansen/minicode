// HtmlView.h — an .html file shown as its page in the editor's slot, the
// Mac's MCHtmlPage (mac/src/HtmlPage.mm) over the core's HtmlPage.h rules.
// The viewer over a tab's text the CSV table is: Ctrl+Shift+V flips it to
// the source and back.
//
// The page is the file on disk, not the buffer: unsaved source shows once it
// is saved. It loads from its own file (relative stylesheets, scripts,
// pictures and pages resolve beside it), in a web view with an ephemeral
// network session, so nothing it stores outlives it. When the file changes
// (a save, a tool rewriting it) it loads again keeping the scroll.
//
// Scripts follow html.scripts (HtmlPage.h): turned off in the web view's
// settings until allowed; a bar over the page asks, Run Scripts or Don't
// Run, with a box to remember the answer in the settings file. An answer not
// remembered holds for that file until mutible quits, and leaves a line in
// the bar's place saying what was chosen, with a close box. A link or
// window.open leaving the page goes to the link handler (a local file in a
// tab of its own, a web address in the browser pane); the page on show is
// always the tab's file. WebKitGTK has no read-access folder for a file
// page, so pictures and stylesheets are read from wherever the page names;
// scripts cannot read other files (file access from file URLs stays off).
// The page never takes the keyboard by itself: its web view takes focus only
// from a click or takeKeyboard().
#pragma once

#include "HtmlPage.h"

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <set>
#include <string>

#ifdef MUTIBLE_ENABLE_BROWSER
#include <webkit/webkit.h>

class HtmlView {
public:
    struct Host {
        std::function<Settings::HtmlScripts()> setting;   // html.scripts now
        std::function<std::string()> root;                // the window's folder
        // Writes html.scripts = on or off; false when it could not.
        std::function<bool(const std::string& value)> remember;
        std::function<void(const std::string& url)> openLink;
    };

    explicit HtmlView(Host host);
    ~HtmlView();
    HtmlView(const HtmlView&) = delete;
    HtmlView& operator=(const HtmlView&) = delete;

    GtkWidget* widget() const { return box_; }
    // Shows the file: loads it when it is another file or changed since it
    // was loaded (keeping the scroll), else leaves the page as it is.
    void show(const std::string& path);
    void reloadIfChanged();
    // Lets go of the page (the tab left the page), and of its web process.
    void drop();
    // The user asked for the keyboard (the toggle key).
    void takeKeyboard();
    // html.scripts changed.
    void settingsChanged();
    // The bar's answer.
    void answer(bool run, bool remember);
    void endNote();

    // For checks.
    const std::string& path() const { return path_; }
    bool runsScripts() const { return runs_; }
    bool asking() const { return asking_; }
    bool noting() const { return noting_ && !asking_; }
    unsigned loads() const { return loads_; }
    bool restoringPlace() const { return restoring_; }
    // The page finished a load of its file and nothing is being put back.
    bool settled() const;
    WebKitWebView* webView() const { return web_ ? WEBKIT_WEB_VIEW(web_) : nullptr; }
    GtkWidget* bar() const { return bar_; }
    GtkWidget* rememberBox() const { return remember_; }
    std::string barText() const;
    // Runs `script` in the page's own world (scripts of the page need not be
    // allowed to run) and hands back its string value, or "" on failure.
    void evaluate(const std::string& script, std::function<void(const std::string&)> done);
    // Clicks the bar's buttons the way a user does (checks).
    void pressRun() { answer(true, rememberChecked()); }
    void pressDontRun() { answer(false, rememberChecked()); }
    bool rememberChecked() const;
    void setRemember(bool on);

private:
    struct Stamp {
        long long mtime = 0, size = 0;
        unsigned long long inode = 0;
        bool valid = false;
        bool operator==(const Stamp& o) const {
            return valid == o.valid && mtime == o.mtime && size == o.size && inode == o.inode;
        }
    };
    static Stamp stampOf(const std::string& path);
    void makeWeb();
    void makeBar();
    void layoutBar();
    void followScriptsRule();
    HtmlPage::Scripts wanted() const;
    void loadKeepingScroll(bool keep);
    void startLoad(unsigned generation);
    void putBackPlace(unsigned generation, int tries, int held);
    void watch();
    void unwatch();
    void onLoad(WebKitLoadEvent ev);
    gboolean onPolicy(WebKitPolicyDecision* d, WebKitPolicyDecisionType type);

    Host host_;
    GtkWidget* box_ = nullptr;
    GtkWidget* bar_ = nullptr;
    GtkWidget* label_ = nullptr;
    GtkWidget* run_ = nullptr;
    GtkWidget* dont_ = nullptr;
    GtkWidget* remember_ = nullptr;
    GtkWidget* close_ = nullptr;
    GtkWidget* web_ = nullptr;
    std::string path_;
    Stamp stamp_;
    bool hasScripts_ = false;
    bool runs_ = false, asking_ = false, noting_ = false;
    unsigned generation_ = 0, loads_ = 0;
    bool restoreScroll_ = false, restoring_ = false;
    double scrollX_ = 0, scrollY_ = 0;
    std::set<std::string> requested_;
    GFileMonitor* monitor_ = nullptr;
    guint checkTimer_ = 0, placeTimer_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
#endif
