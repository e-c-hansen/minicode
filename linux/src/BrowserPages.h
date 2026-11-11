// BrowserPages.h — what `minicode browse` and `minicode page` need of a page
// in the browser pane (PARITY.md 3.7; the Mac's BrowserPages.mm): its address,
// title, HTTP status and whether it is loading, the console record of the
// agent page, a wait for a load, the page's text and a screenshot. The rules
// (addresses, the origin limit, the record) are PageCheck's, in the core.
//
// The agent page's web view has a user content manager of its own: a script
// message handler and a listener in a script world of ours (uncaught errors,
// rejections and console calls), and in the page's own world a wrapper of
// console.* that relays each call through a DOM event, so the page cannot
// forge anything but a console call.
//
// Compiled only with MINICODE_ENABLE_BROWSER.
#pragma once

#ifdef MINICODE_ENABLE_BROWSER

#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Json.h"
#include "PageCheck.h"

class PageProbe {
public:
    // Watches `view`; recordsConsole is for the agent page's view.
    PageProbe(GtkWidget* view, bool recordsConsole);
    ~PageProbe();
    PageProbe(const PageProbe&) = delete;
    PageProbe& operator=(const PageProbe&) = delete;

    // The agent page's web view (the capture scripts installed) and the
    // probe watching it.
    static GtkWidget* makeAgentView(PageProbe** probeOut);

    GtkWidget* view() const { return view_; }
    std::string url() const;
    std::string title() const;
    int status() const { return status_; }
    bool loading() const { return loading_; }
    PageCheck::ConsoleLog& console() { return console_; }
    Json stateJson() const;

    // Loads an address as it is.
    bool load(const std::string& url);
    bool reload();

    // Calls done when the load on its way ends (at once when none is): with
    // an error text, or timed out after PageCheck::kWaitSeconds.
    using Waiter = std::function<void(const std::string& error, bool timedOut)>;
    void waitForLoad(Waiter done);

    // The visible text, cut to PageCheck::kMaxTextBytes.
    void readText(std::function<void(const std::string& text, bool cut, const std::string& error)> done);
    // A PNG of the page at `path` (which PageCheck::screenshotPath has
    // approved): created with O_EXCL and never through a symlink, or with
    // `overwrite` a temporary file renamed over it.
    void snapshotTo(const std::string& path, bool full, bool overwrite,
                    std::function<void(int width, int height, bool cut, const std::string& error)> done);

private:
    static void onLoadChanged(WebKitWebView* wv, WebKitLoadEvent ev, gpointer self);
    static gboolean onLoadFailed(WebKitWebView* wv, WebKitLoadEvent ev, gchar* uri, GError* err, gpointer self);
    void finishWaiters();
    void addConsole(const std::string& body);
    void evaluate(const std::string& script, std::function<void(JSCValue*, const std::string&)> done);

    GtkWidget* view_ = nullptr;
    bool records_ = false;
    int status_ = 0;
    bool loading_ = false;
    std::string error_;
    PageCheck::ConsoleLog console_;
    struct Wait {
        Waiter done;
        guint timer = 0;
        PageProbe* probe = nullptr;
    };
    std::vector<Wait*> waiters_;
    WebKitUserContentManager* ucm_ = nullptr;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

#endif  // MINICODE_ENABLE_BROWSER
