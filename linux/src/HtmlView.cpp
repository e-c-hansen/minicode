// HtmlView.cpp — see HtmlView.h.
#include "HtmlView.h"

#ifdef MINICODE_ENABLE_BROWSER

#include "ConsentBar.h"
#include "FileSniff.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace {

const char* const kWorld = "minicode-html";

// What each file was answered, until minicode quits (the Mac's ScriptAnswers):
// 1 run, 2 do not run.
std::map<std::string, int>& answers() {
    static std::map<std::string, int>* all = new std::map<std::string, int>();
    return *all;
}

std::string canonical(const std::string& path) {
    char* c = g_canonicalize_filename(path.c_str(), nullptr);
    std::string out = c ? c : path;
    g_free(c);
    return out;
}

std::string uriForPath(const std::string& path) {
    char* u = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    std::string out = u ? u : "";
    g_free(u);
    return out;
}

HtmlPage::Navigation::Kind kindOf(WebKitNavigationType t) {
    switch (t) {
    case WEBKIT_NAVIGATION_TYPE_LINK_CLICKED: return HtmlPage::Navigation::Kind::Link;
    case WEBKIT_NAVIGATION_TYPE_FORM_SUBMITTED: return HtmlPage::Navigation::Kind::Form;
    case WEBKIT_NAVIGATION_TYPE_RELOAD: return HtmlPage::Navigation::Kind::Reload;
    case WEBKIT_NAVIGATION_TYPE_BACK_FORWARD: return HtmlPage::Navigation::Kind::BackForward;
    default: return HtmlPage::Navigation::Kind::Other;
    }
}

}  // namespace

HtmlView::Stamp HtmlView::stampOf(const std::string& path) {
    Stamp s;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return s;
    s.valid = true;
    s.mtime = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
    s.size = st.st_size;
    s.inode = st.st_ino;
    return s;
}

HtmlView::HtmlView(Host host) : host_(std::move(host)) {
    ConsentBar::ensureCss();
    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(box_, TRUE);
    gtk_widget_set_vexpand(box_, TRUE);
    makeBar();
    gtk_box_append(GTK_BOX(box_), bar_);
}

HtmlView::~HtmlView() {
    *alive_ = false;
    unwatch();
    if (placeTimer_) g_source_remove(placeTimer_);
    if (checkTimer_) g_source_remove(checkTimer_);
    // The widgets belong to the window's tree; their handlers name this.
    for (GtkWidget* w : {run_, dont_, close_})
        if (w) g_signal_handlers_disconnect_by_data(w, this);
    if (web_) g_signal_handlers_disconnect_by_data(web_, this);
}

void HtmlView::makeBar() {
    bar_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(bar_, "minicode-consent-bar");
    label_ = gtk_label_new(HtmlPage::kQuestion);
    gtk_label_set_xalign(GTK_LABEL(label_), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label_), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label_, TRUE);
    run_ = gtk_button_new_with_label(HtmlPage::kRun);
    dont_ = gtk_button_new_with_label(HtmlPage::kDontRun);
    remember_ = gtk_check_button_new_with_label(HtmlPage::kRemember);
    gtk_widget_set_tooltip_text(remember_, HtmlPage::kRememberTip);
    close_ = gtk_button_new_with_label("\xC3\x97");
    gtk_widget_add_css_class(close_, "flat");
    gtk_widget_set_tooltip_text(close_, HtmlPage::kCloseTip);
    for (GtkWidget* w : {label_, run_, dont_, remember_, close_}) gtk_box_append(GTK_BOX(bar_), w);
    for (GtkWidget* w : {run_, dont_, remember_, close_}) gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
    g_signal_connect_swapped(run_, "clicked", G_CALLBACK(+[](HtmlView* v) { v->pressRun(); }), this);
    g_signal_connect_swapped(dont_, "clicked",
                             G_CALLBACK(+[](HtmlView* v) { v->pressDontRun(); }), this);
    g_signal_connect_swapped(close_, "clicked", G_CALLBACK(+[](HtmlView* v) { v->endNote(); }), this);
    gtk_widget_set_visible(bar_, FALSE);
}

bool HtmlView::rememberChecked() const {
    return gtk_check_button_get_active(GTK_CHECK_BUTTON(remember_));
}
void HtmlView::setRemember(bool on) { gtk_check_button_set_active(GTK_CHECK_BUTTON(remember_), on); }

std::string HtmlView::barText() const { return gtk_label_get_text(GTK_LABEL(label_)); }

void HtmlView::makeWeb() {
    WebKitNetworkSession* session = webkit_network_session_new_ephemeral();
    web_ = GTK_WIDGET(g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session", session, nullptr));
    g_object_unref(session);
    gtk_widget_set_hexpand(web_, TRUE);
    gtk_widget_set_vexpand(web_, TRUE);
    gtk_box_append(GTK_BOX(box_), web_);
    WebKitSettings* st = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(web_));
    webkit_settings_set_enable_javascript(st, FALSE);
    // The page never takes the keyboard by itself: a click, or the toggle
    // key, lets it, and it lets go again when the keyboard moves on.
    gtk_widget_set_focusable(web_, FALSE);
    GtkGesture* click = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick*, int, double, double, gpointer p) {
        static_cast<HtmlView*>(p)->takeKeyboard();
    }), this);
    gtk_widget_add_controller(web_, GTK_EVENT_CONTROLLER(click));
    g_signal_connect(web_, "notify::has-focus", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
        if (!gtk_widget_has_focus(GTK_WIDGET(o))) gtk_widget_set_focusable(GTK_WIDGET(o), FALSE);
    }), this);
    g_signal_connect(web_, "load-changed", G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent ev, gpointer p) {
        static_cast<HtmlView*>(p)->onLoad(ev);
    }), this);
    g_signal_connect(web_, "load-failed", G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent, char*, GError*, gpointer p) -> gboolean {
        HtmlView* v = static_cast<HtmlView*>(p);
        if (!webkit_web_view_is_loading(WEBKIT_WEB_VIEW(v->web_))) v->restoring_ = false;
        return FALSE;
    }), this);
    g_signal_connect(web_, "decide-policy", G_CALLBACK(+[](WebKitWebView*, WebKitPolicyDecision* d, WebKitPolicyDecisionType t, gpointer p) -> gboolean {
        return static_cast<HtmlView*>(p)->onPolicy(d, t);
    }), this);
    // window.open and target=_blank: the link handler, never a window.
    g_signal_connect(web_, "create", G_CALLBACK(+[](WebKitWebView*, WebKitNavigationAction* a, gpointer p) -> GtkWidget* {
        HtmlView* v = static_cast<HtmlView*>(p);
        WebKitURIRequest* r = webkit_navigation_action_get_request(a);
        const char* uri = r ? webkit_uri_request_get_uri(r) : nullptr;
        if (uri && *uri && v->host_.openLink) {
            std::string u = uri;
            auto alive = v->alive_;
            g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, +[](gpointer d) -> gboolean {
                auto* job = static_cast<std::pair<std::function<void()>, std::shared_ptr<bool>>*>(d);
                if (*job->second) job->first();
                delete job;
                return G_SOURCE_REMOVE;
            }, new std::pair<std::function<void()>, std::shared_ptr<bool>>(
                   [v, u] { v->host_.openLink(u); }, alive), nullptr);
        }
        return nullptr;
    }), this);
}

// The scheme, file path and query of an address, for the core's rules. The
// frame is not told apart: its name is no sign of a main frame.
static HtmlPage::Navigation navigationFor(const char* uri) {
    HtmlPage::Navigation nav;
    char* scheme = g_uri_parse_scheme(uri);
    if (scheme) {
        nav.scheme = scheme;
        std::transform(nav.scheme.begin(), nav.scheme.end(), nav.scheme.begin(),
                       [](unsigned char c) { return (char)g_ascii_tolower(c); });
    }
    g_free(scheme);
    const std::string u = HtmlPage::withoutFragment(uri);
    nav.hasQuery = u.find('?') != std::string::npos;
    if (nav.scheme == "file") {
        const std::string noQuery = u.substr(0, u.find('?'));
        char* f = g_filename_from_uri(noQuery.c_str(), nullptr, nullptr);
        nav.path = f ? canonical(f) : "";
        g_free(f);
    }
    return nav;
}

gboolean HtmlView::onPolicy(WebKitPolicyDecision* d, WebKitPolicyDecisionType type) {
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
        type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
        return FALSE;
    WebKitNavigationAction* a =
        webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(d));
    WebKitURIRequest* r = a ? webkit_navigation_action_get_request(a) : nullptr;
    const char* uri = r ? webkit_uri_request_get_uri(r) : nullptr;
    if (!uri) return FALSE;
    HtmlPage::Navigation nav = navigationFor(uri);
    nav.userGesture = webkit_navigation_action_is_user_gesture(a);
    nav.kind = kindOf(webkit_navigation_action_get_navigation_type(a));
    HtmlPage::Verdict v = HtmlPage::decide(nav, canonical(path_), requested_);
    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION && v == HtmlPage::Verdict::Allow)
        v = HtmlPage::Verdict::Open;   // never a window of its own
    if (v == HtmlPage::Verdict::Allow) {
        webkit_policy_decision_use(d);
        return TRUE;
    }
    webkit_policy_decision_ignore(d);
    if (v == HtmlPage::Verdict::Open && host_.openLink) {
        const std::string target = uri;
        auto alive = alive_;
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, +[](gpointer data) -> gboolean {
            auto* job = static_cast<std::pair<std::function<void()>, std::shared_ptr<bool>>*>(data);
            if (*job->second) job->first();
            delete job;
            return G_SOURCE_REMOVE;
        }, new std::pair<std::function<void()>, std::shared_ptr<bool>>(
               [this, target] { host_.openLink(target); }, alive), nullptr);
    }
    return TRUE;
}

HtmlPage::Scripts HtmlView::wanted() const {
    const auto& all = answers();
    auto it = all.find(path_);
    return HtmlPage::scripts(host_.setting ? host_.setting() : Settings::HtmlScripts::Ask,
                             hasScripts_, it == all.end() ? 0 : it->second);
}

void HtmlView::layoutBar() {
    const bool noting = noting_ && !asking_;
    const bool shown = asking_ || noting;
    gtk_widget_set_visible(bar_, shown);
    for (GtkWidget* w : {run_, dont_, remember_}) gtk_widget_set_visible(w, asking_);
    gtk_widget_set_visible(close_, noting);
    if (asking_) {
        gtk_label_set_text(GTK_LABEL(label_), HtmlPage::kQuestion);
    } else if (noting) {
        const bool settingAsks =
            (host_.setting ? host_.setting() : Settings::HtmlScripts::Ask) == Settings::HtmlScripts::Ask;
        gtk_label_set_text(GTK_LABEL(label_), HtmlPage::answeredLine(runs_, settingAsks).c_str());
    }
}

void HtmlView::endNote() {
    if (!noting_) return;
    noting_ = false;
    layoutBar();
}

void HtmlView::answer(bool run, bool remember) {
    if (!path_.empty()) answers()[path_] = run ? 1 : 2;
    if (asking_) noting_ = true;
    setRemember(false);
    if (remember && host_.remember) host_.remember(HtmlPage::rememberedValue(run));
    followScriptsRule();
}

void HtmlView::followScriptsRule() {
    if (path_.empty()) return;
    const HtmlPage::Scripts w = wanted();
    if (w.ask != asking_) {
        asking_ = w.ask;
        layoutBar();
    }
    if (w.run != runs_) loadKeepingScroll(true);
    else layoutBar();
}

void HtmlView::settingsChanged() { followScriptsRule(); }

void HtmlView::show(const std::string& path) {
    if (path.empty()) return;
    const bool same = web_ && path == path_;
    if (same && stampOf(path) == stamp_) {
        followScriptsRule();   // the rule may have changed meanwhile
        return;
    }
    if (!same) {
        restoreScroll_ = false;
        noting_ = false;
        unwatch();
    }
    path_ = path;
    watch();
    loadKeepingScroll(same);
}

void HtmlView::reloadIfChanged() {
    if (!path_.empty() && web_) show(path_);
}

void HtmlView::watch() {
    if (monitor_ || path_.empty()) return;
    GFile* f = g_file_new_for_path(path_.c_str());
    monitor_ = g_file_monitor_file(f, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    g_object_unref(f);
    if (!monitor_) return;
    g_signal_connect(monitor_, "changed", G_CALLBACK(+[](GFileMonitor*, GFile*, GFile*, GFileMonitorEvent ev, gpointer p) {
        HtmlView* v = static_cast<HtmlView*>(p);
        switch (ev) {
        case G_FILE_MONITOR_EVENT_CHANGED:
        case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
        case G_FILE_MONITOR_EVENT_CREATED:
        case G_FILE_MONITOR_EVENT_RENAMED:
        case G_FILE_MONITOR_EVENT_MOVED_IN: break;
        default: return;
        }
        // A burst of events becomes one look, a moment after the last.
        if (v->checkTimer_) g_source_remove(v->checkTimer_);
        v->checkTimer_ = g_timeout_add(200, +[](gpointer d) -> gboolean {
            HtmlView* me = static_cast<HtmlView*>(d);
            me->checkTimer_ = 0;
            me->reloadIfChanged();
            return G_SOURCE_REMOVE;
        }, v);
    }), this);
}

void HtmlView::unwatch() {
    if (checkTimer_) g_source_remove(checkTimer_);
    checkTimer_ = 0;
    if (!monitor_) return;
    g_signal_handlers_disconnect_by_data(monitor_, this);
    g_file_monitor_cancel(monitor_);
    g_object_unref(monitor_);
    monitor_ = nullptr;
}

void HtmlView::loadKeepingScroll(bool keep) {
    if (path_.empty()) return;
    stamp_ = stampOf(path_);
    std::string text;
    gchar* data = nullptr;
    gsize len = 0;
    if (g_file_get_contents(path_.c_str(), &data, &len, nullptr)) {
        text.assign(data, len);
        g_free(data);
    }
    hasScripts_ = FileSniff::htmlMayRunScripts(text);
    const HtmlPage::Scripts w = wanted();
    runs_ = w.run;
    asking_ = w.ask;
    if (!web_) {
        makeWeb();
        keep = false;
    }
    layoutBar();
    const unsigned generation = ++generation_;
    WebKitWebView* wv = WEBKIT_WEB_VIEW(web_);
    if (!keep || !webkit_web_view_get_uri(wv)) {
        restoring_ = false;
        startLoad(generation);
        return;
    }
    if (restoreScroll_ || webkit_web_view_is_loading(wv)) {
        restoring_ = restoreScroll_;
        startLoad(generation);
        return;
    }
    restoring_ = true;
    auto alive = alive_;
    evaluate("[window.scrollX, window.scrollY].join(',')", [this, alive, generation](const std::string& xy) {
        if (!*alive || generation_ != generation) return;
        double x = 0, y = 0;
        if (sscanf(xy.c_str(), "%lf,%lf", &x, &y) == 2) {
            scrollX_ = x;
            scrollY_ = y;
            restoreScroll_ = true;
        } else {
            restoring_ = false;
        }
        startLoad(generation);
    });
}

void HtmlView::evaluate(const std::string& script, std::function<void(const std::string&)> done) {
    if (!web_) {
        done("");
        return;
    }
    using Cb = std::function<void(const std::string&)>;
    auto* job = new std::pair<Cb, std::shared_ptr<bool>>(std::move(done), alive_);
    webkit_web_view_evaluate_javascript(
        WEBKIT_WEB_VIEW(web_), script.c_str(), -1, kWorld, nullptr, nullptr,
        +[](GObject* src, GAsyncResult* res, gpointer p) {
            std::unique_ptr<std::pair<Cb, std::shared_ptr<bool>>> j(
                static_cast<std::pair<Cb, std::shared_ptr<bool>>*>(p));
            if (!*j->second) return;
            GError* err = nullptr;
            JSCValue* v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(src), res, &err);
            g_clear_error(&err);
            std::string out;
            if (v) {
                char* s = jsc_value_to_string(v);
                out = s ? s : "";
                g_free(s);
                g_object_unref(v);
            }
            j->first(out);
        },
        job);
}

void HtmlView::startLoad(unsigned generation) {
    if (generation != generation_ || !web_ || path_.empty()) return;
    const std::string uri = uriForPath(path_);
    WebKitWebView* wv = WEBKIT_WEB_VIEW(web_);
    // The setting is the page's: it takes effect on the load that follows.
    webkit_settings_set_enable_javascript(webkit_web_view_get_settings(wv), runs_ ? TRUE : FALSE);
    loads_++;
    requested_.insert(canonical(path_));
    const char* now = webkit_web_view_get_uri(wv);
    if (now && HtmlPage::withoutFragment(now) == uri) webkit_web_view_reload_bypass_cache(wv);
    else webkit_web_view_load_uri(wv, uri.c_str());
}

void HtmlView::onLoad(WebKitLoadEvent ev) {
    if (ev == WEBKIT_LOAD_COMMITTED) {
        // Only the main frame reports a load here (at STARTED the view still
        // names the old address, so it is read once committed). One the
        // page's own script made, away from the file, is stopped, the page
        // put back and the address opened as a link.
        const char* uri = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(web_));
        if (!uri || path_.empty()) return;
        const HtmlPage::Started s =
            HtmlPage::started(navigationFor(uri), canonical(path_), requested_);
        if (s == HtmlPage::Started::Keep) return;
        webkit_web_view_stop_loading(WEBKIT_WEB_VIEW(web_));
        const std::string target = uri;
        const bool open = s == HtmlPage::Started::RestoreAndOpen;
        struct Job { HtmlView* v; std::shared_ptr<bool> alive; std::string target; bool open; unsigned g; };
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, +[](gpointer p) -> gboolean {
            std::unique_ptr<Job> j(static_cast<Job*>(p));
            if (!*j->alive || j->g != j->v->generation_) return G_SOURCE_REMOVE;
            j->v->loadKeepingScroll(true);   // the file's page again, in its place
            if (j->open && j->v->host_.openLink) j->v->host_.openLink(j->target);
            return G_SOURCE_REMOVE;
        }, new Job{this, alive_, target, open, generation_}, nullptr);
        return;
    }
    if (ev != WEBKIT_LOAD_FINISHED) return;
    if (!restoreScroll_) {
        restoring_ = false;
        return;
    }
    restoreScroll_ = false;
    putBackPlace(generation_, 0, 0);
}

// The place goes back until it holds: one scroll at the load's end can be
// lost to layout not yet done or to WebKit's own restore on a reload.
void HtmlView::putBackPlace(unsigned generation, int tries, int held) {
    if (generation != generation_ || !web_) return;
    char js[600];
    snprintf(js, sizeof js,
             "(function () {"
             "  var x = %f, y = %f;"
             "  var e = document.scrollingElement || document.documentElement;"
             "  var maxY = Math.max(0, e.scrollHeight - window.innerHeight);"
             "  var want = Math.min(y, maxY);"
             "  if (Math.abs(window.scrollY - want) > 1 || Math.abs(window.scrollX - x) > 1)"
             "    window.scrollTo(x, y);"
             "  return window.scrollY + ',' + want;"
             "})()", scrollX_, scrollY_);
    auto alive = alive_;
    evaluate(js, [this, alive, generation, tries, held](const std::string& r) {
        if (!*alive || generation != generation_) return;
        double a = 0, b = 1e9;
        const bool there = sscanf(r.c_str(), "%lf,%lf", &a, &b) == 2 && std::fabs(a - b) <= 1;
        const int nowHeld = there ? held + 1 : 0;
        if ((nowHeld >= 3 && tries >= 5) || tries >= 30) {
            restoring_ = false;
            return;
        }
        struct Again { HtmlView* v; std::shared_ptr<bool> alive; unsigned g; int tries, held; };
        placeTimer_ = g_timeout_add(100, +[](gpointer p) -> gboolean {
            std::unique_ptr<Again> a(static_cast<Again*>(p));
            if (*a->alive) {
                a->v->placeTimer_ = 0;
                a->v->putBackPlace(a->g, a->tries, a->held);
            }
            return G_SOURCE_REMOVE;
        }, new Again{this, alive, generation, tries + 1, nowHeld});
    });
}

bool HtmlView::settled() const {
    return web_ && webkit_web_view_get_uri(WEBKIT_WEB_VIEW(web_)) &&
           !webkit_web_view_is_loading(WEBKIT_WEB_VIEW(web_)) && !restoring_ && !restoreScroll_;
}

void HtmlView::drop() {
    generation_++;
    restoreScroll_ = restoring_ = false;
    if (placeTimer_) g_source_remove(placeTimer_);
    placeTimer_ = 0;
    unwatch();
    if (web_) {
        g_signal_handlers_disconnect_by_data(web_, this);
        webkit_web_view_terminate_web_process(WEBKIT_WEB_VIEW(web_));
        gtk_box_remove(GTK_BOX(box_), web_);
        web_ = nullptr;
    }
    path_.clear();
    stamp_ = Stamp();
    asking_ = noting_ = runs_ = false;
    layoutBar();
}

void HtmlView::takeKeyboard() {
    if (!web_ || !gtk_widget_get_mapped(web_)) return;
    gtk_widget_set_focusable(web_, TRUE);
    gtk_widget_grab_focus(web_);
}

#endif
