// BrowserPages.cpp — see BrowserPages.h.
#include "BrowserPages.h"

#ifdef MINICODE_ENABLE_BROWSER

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace {

const char* const kWorld = "minicode-agent";
const char* const kHandler = "minicodeConsole";

// The wrapper in the page's world: each console call becomes a DOM event
// with a name only this page load knows, read by the listener in our world.
std::string pageWorldShim(const std::string& event) {
    return std::string(
        "(function(){"
        "var c=window.console;if(!c)return;"
        "var name='") + event + "',D=document,CE=CustomEvent,send=EventTarget.prototype.dispatchEvent,"
        "S=String,J=JSON.stringify,E=Error,A=Array.prototype.join;"
        "function show(a){try{if(typeof a==='string')return a.slice(0,4096);"
        "if(a instanceof E)return S(a);"
        "if(typeof a==='object'&&a!==null){var j=J(a);if(typeof j==='string')return j.slice(0,4096);}"
        "return S(a).slice(0,4096);}catch(x){return '[object]';}}"
        "['log','info','warn','error','debug'].forEach(function(level){"
        "var orig=c[level];if(typeof orig!=='function')return;"
        "c[level]=function(){"
        "try{var parts=[];for(var i=0;i<arguments.length&&i<32;i++)parts.push(show(arguments[i]));"
        "var st='';try{st=S(new E().stack||'').split('\\n').slice(1,8).join('\\n');}catch(x){}"
        "send.call(D,new CE(name,{detail:J({kind:'console',level:level,"
        "text:A.call(parts,' '),stack:st})}));}catch(x){}"
        "return orig.apply(this,arguments);};});"
        "})();";
}

// The listener in our world: uncaught errors and rejections from the
// window's own events, console calls from the wrapper's event.
std::string isolatedListener(const std::string& event) {
    return std::string(
        "(function(){"
        "var h=window.webkit&&window.webkit.messageHandlers&&window.webkit.messageHandlers.") + kHandler + ";"
        "if(!h)return;"
        "function post(o){try{h.postMessage(JSON.stringify(o));}catch(x){}}"
        "function str(v){try{return String(v).slice(0,4096);}catch(x){return '';}}"
        "window.addEventListener('error',function(e){"
        "if(!(e instanceof ErrorEvent))return;"
        "post({kind:'uncaught',text:str(e.message),source:str(e.filename||''),"
        "line:e.lineno||0,column:e.colno||0});},true);"
        "window.addEventListener('unhandledrejection',function(e){"
        "var r=e.reason,t;try{t=(r&&r.stack)?str(r)+'':str(r);}catch(x){t='';}"
        "post({kind:'rejection',text:t});},true);"
        "document.addEventListener('" + event + "',function(e){"
        "var d=e.detail;if(typeof d==='string'&&d.length<65536)post({relay:d});},true);"
        "})();";
}

std::string uuidWord() {
    char* u = g_uuid_string_random();
    std::string s;
    for (const char* p = u; *p; p++)
        if (*p != '-') s += *p;
    g_free(u);
    return s;
}

}  // namespace

GtkWidget* PageProbe::makeAgentView(PageProbe** probeOut) {
    WebKitUserContentManager* ucm = webkit_user_content_manager_new();
    const std::string event = "minicode-console-" + uuidWord();
    webkit_user_content_manager_register_script_message_handler(ucm, kHandler, kWorld);
    WebKitUserScript* listener = webkit_user_script_new_for_world(
        isolatedListener(event).c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, kWorld, nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, listener);
    webkit_user_script_unref(listener);
    WebKitUserScript* shim = webkit_user_script_new(
        pageWorldShim(event).c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, shim);
    webkit_user_script_unref(shim);
    GtkWidget* view = GTK_WIDGET(g_object_new(WEBKIT_TYPE_WEB_VIEW, "user-content-manager", ucm, nullptr));
    PageProbe* probe = new PageProbe(view, true);
    probe->ucm_ = ucm;   // the probe's own reference
    g_signal_connect(ucm, "script-message-received::minicodeConsole",
                     G_CALLBACK(+[](WebKitUserContentManager*, JSCValue* v, gpointer p) {
                         if (!jsc_value_is_string(v)) return;
                         char* s = jsc_value_to_string(v);
                         static_cast<PageProbe*>(p)->addConsole(s ? s : "");
                         g_free(s);
                     }),
                     probe);
    *probeOut = probe;
    return view;
}

PageProbe::PageProbe(GtkWidget* view, bool recordsConsole) : view_(view), records_(recordsConsole) {
    g_signal_connect(view_, "load-changed", G_CALLBACK(onLoadChanged), this);
    g_signal_connect(view_, "load-failed", G_CALLBACK(onLoadFailed), this);
}

PageProbe::~PageProbe() {
    *alive_ = false;
    if (view_) g_signal_handlers_disconnect_by_data(view_, this);
    if (ucm_) {
        g_signal_handlers_disconnect_by_data(ucm_, this);
        g_object_unref(ucm_);
    }
    for (Wait* w : waiters_) {
        if (w->timer) g_source_remove(w->timer);
        delete w;
    }
}

std::string PageProbe::url() const {
    const char* u = view_ ? webkit_web_view_get_uri(WEBKIT_WEB_VIEW(view_)) : nullptr;
    return u ? u : "";
}

std::string PageProbe::title() const {
    const char* t = view_ ? webkit_web_view_get_title(WEBKIT_WEB_VIEW(view_)) : nullptr;
    return t ? t : "";
}

Json PageProbe::stateJson() const {
    return PageCheck::pageState(url(), status_, title(), loading_);
}

bool PageProbe::load(const std::string& u) {
    if (!view_) return false;
    loading_ = true;
    error_.clear();
    status_ = 0;
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(view_), u.c_str());
    return true;
}

bool PageProbe::reload() {
    if (!view_ || url().empty()) return false;
    loading_ = true;
    error_.clear();
    webkit_web_view_reload(WEBKIT_WEB_VIEW(view_));
    return true;
}

void PageProbe::onLoadChanged(WebKitWebView* wv, WebKitLoadEvent ev, gpointer selfp) {
    PageProbe* self = static_cast<PageProbe*>(selfp);
    auto readStatus = [&] {
        WebKitWebResource* r = webkit_web_view_get_main_resource(wv);
        WebKitURIResponse* resp = r ? webkit_web_resource_get_response(r) : nullptr;
        if (resp) self->status_ = (int)webkit_uri_response_get_status_code(resp);
    };
    if (ev == WEBKIT_LOAD_STARTED || ev == WEBKIT_LOAD_REDIRECTED) {
        self->loading_ = true;
    } else if (ev == WEBKIT_LOAD_COMMITTED) {
        self->status_ = 0;
        readStatus();
        if (self->records_) self->console_.newPage();
    } else if (ev == WEBKIT_LOAD_FINISHED) {
        self->loading_ = false;
        if (self->status_ == 0) readStatus();
        self->finishWaiters();
    }
}

gboolean PageProbe::onLoadFailed(WebKitWebView*, WebKitLoadEvent, gchar*, GError* err, gpointer selfp) {
    PageProbe* self = static_cast<PageProbe*>(selfp);
    if (err && !g_error_matches(err, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED))
        self->error_ = err->message ? err->message : "the page did not load";
    return FALSE;
}

void PageProbe::finishWaiters() {
    std::vector<Wait*> ws;
    ws.swap(waiters_);
    const std::string error = error_;
    for (Wait* w : ws) {
        if (w->timer) g_source_remove(w->timer);
        Waiter done = std::move(w->done);
        delete w;
        done(error, false);
    }
}

void PageProbe::waitForLoad(Waiter done) {
    if (!loading_) {
        done(error_, false);
        return;
    }
    Wait* w = new Wait;
    w->done = std::move(done);
    w->probe = this;
    w->timer = g_timeout_add(
        (guint)(PageCheck::kWaitSeconds * 1000),
        +[](gpointer p) -> gboolean {
            Wait* w = static_cast<Wait*>(p);
            PageProbe* probe = w->probe;
            w->timer = 0;
            for (size_t i = 0; i < probe->waiters_.size(); i++)
                if (probe->waiters_[i] == w) {
                    probe->waiters_.erase(probe->waiters_.begin() + (long)i);
                    break;
                }
            Waiter d = std::move(w->done);
            delete w;
            d("", true);
            return G_SOURCE_REMOVE;
        },
        w);
    waiters_.push_back(w);
}

void PageProbe::addConsole(const std::string& body) {
    if (!records_) return;
    Json o;
    std::string error;
    std::string text = body;
    bool fromPage = false;
    // A relay from the page-world wrapper carries the call's own JSON.
    if (Json::parse(body, o, &error) && o.isObject() && o["relay"].isString()) {
        text = o["relay"].asString();
        fromPage = true;
    }
    PageCheck::ConsoleEntry e;
    if (PageCheck::parseConsoleMessage(text, e, fromPage)) console_.add(e);
}

void PageProbe::evaluate(const std::string& script, std::function<void(JSCValue*, const std::string&)> done) {
    if (!view_) {
        done(nullptr, "the page was closed");
        return;
    }
    using Cb = std::function<void(JSCValue*, const std::string&)>;
    struct Job {
        Cb cb;
        std::shared_ptr<bool> alive;
    };
    Job* job = new Job{std::move(done), alive_};
    webkit_web_view_evaluate_javascript(
        WEBKIT_WEB_VIEW(view_), script.c_str(), -1, kWorld, nullptr, nullptr,
        +[](GObject* src, GAsyncResult* res, gpointer p) {
            std::unique_ptr<Job> job(static_cast<Job*>(p));
            GError* err = nullptr;
            JSCValue* v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(src), res, &err);
            std::string msg = err ? err->message : "";
            g_clear_error(&err);
            job->cb(v, msg);
            if (v) g_object_unref(v);
        },
        job);
}

void PageProbe::readText(std::function<void(const std::string&, bool, const std::string&)> done) {
    const std::string script =
        "(function(){var b=document.body;var t=b?b.innerText:'';"
        "return typeof t==='string'?t.slice(0," + std::to_string(PageCheck::kMaxTextBytes) + "):'';})()";
    evaluate(script, [done](JSCValue* v, const std::string& error) {
        if (!v || !jsc_value_is_string(v)) {
            done("", false, error.empty() ? "the page has no text" : error);
            return;
        }
        char* s = jsc_value_to_string(v);
        std::string utf8 = s ? s : "";
        g_free(s);
        const bool cut = utf8.size() >= PageCheck::kMaxTextBytes;
        done(PageCheck::clipUtf8(utf8, PageCheck::kMaxTextBytes), cut, "");
    });
}

namespace {

// Writes `data` to `path`: a new file (never an existing one or a link) or,
// with overwrite, a temporary file beside it renamed over a plain file.
std::string writePng(const std::string& path, const void* data, size_t size, bool overwrite) {
    std::string tmp;
    int fd;
    if (overwrite) {
        std::string t = path + ".minicode-XXXXXX";
        std::vector<char> buf(t.begin(), t.end());
        buf.push_back(0);
        fd = mkstemp(buf.data());
        tmp = buf.data();
    } else {
        fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    }
    if (fd < 0) return "could not create " + path + ": " + std::strerror(errno);
    const char* p = static_cast<const char*>(data);
    size_t left = size;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            const std::string why = std::strerror(errno);
            close(fd);
            unlink(overwrite ? tmp.c_str() : path.c_str());
            return "could not write " + path + ": " + why;
        }
        p += n;
        left -= (size_t)n;
    }
    close(fd);
    if (overwrite && rename(tmp.c_str(), path.c_str()) != 0) {
        const std::string why = std::strerror(errno);
        unlink(tmp.c_str());
        return "could not replace " + path + ": " + why;
    }
    return "";
}

struct SnapJob {
    std::shared_ptr<bool> alive;
    std::string path;
    bool full, overwrite, cutWanted;
    double cutHeight;
    double scale;
    std::function<void(int, int, bool, const std::string&)> done;
};

}  // namespace

void PageProbe::snapshotTo(const std::string& path, bool full, bool overwrite,
                           std::function<void(int, int, bool, const std::string&)> done) {
    if (!view_) {
        done(0, 0, false, "the page was closed");
        return;
    }
    const double scale = gtk_widget_get_scale_factor(view_);
    const double visible = gtk_widget_get_height(view_);
    auto start = [this, path, full, overwrite, done, scale](double pageHeight) {
        if (!view_) {
            done(0, 0, false, "the page was closed");
            return;
        }
        PageCheck::SnapshotPlan plan = PageCheck::snapshotPlan(
            gtk_widget_get_height(view_), pageHeight, scale, full);
        auto* job = new SnapJob{alive_, path, full, overwrite, plan.cut, plan.height, scale, done};
        webkit_web_view_get_snapshot(
            WEBKIT_WEB_VIEW(view_),
            full ? WEBKIT_SNAPSHOT_REGION_FULL_DOCUMENT : WEBKIT_SNAPSHOT_REGION_VISIBLE,
            WEBKIT_SNAPSHOT_OPTIONS_NONE, nullptr,
            +[](GObject* src, GAsyncResult* res, gpointer p) {
                std::unique_ptr<SnapJob> job(static_cast<SnapJob*>(p));
                GError* err = nullptr;
                GdkTexture* tex = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(src), res, &err);
                if (!tex) {
                    std::string m = err ? err->message : "WebKit made no picture";
                    g_clear_error(&err);
                    job->done(0, 0, false, m);
                    return;
                }
                int w = gdk_texture_get_width(tex), h = gdk_texture_get_height(tex);
                GdkTexture* out = tex;
                bool cut = false;
                const int maxH = (int)(job->cutHeight * job->scale);
                if (job->full && job->cutWanted && h > maxH && maxH > 0) {
                    // Cut to the plan's height: the top rows of the picture.
                    const size_t stride = (size_t)w * 4;
                    std::vector<guchar> data(stride * (size_t)h);
                    gdk_texture_download(tex, data.data(), stride);
                    GBytes* bytes = g_bytes_new(data.data(), stride * (size_t)maxH);
                    out = gdk_memory_texture_new(w, maxH, GDK_MEMORY_DEFAULT, bytes, stride);
                    g_bytes_unref(bytes);
                    h = maxH;
                    cut = true;
                }
                GBytes* png = gdk_texture_save_to_png_bytes(out);
                gsize size = 0;
                const void* data = g_bytes_get_data(png, &size);
                const std::string error = writePng(job->path, data, size, job->overwrite);
                g_bytes_unref(png);
                if (out != tex) g_object_unref(out);
                g_object_unref(tex);
                job->done(error.empty() ? w : 0, error.empty() ? h : 0, cut, error);
            },
            job);
    };
    if (!full) {
        start(visible);
        return;
    }
    // The page's height, to cut a very tall one.
    evaluate("Math.max(document.documentElement?document.documentElement.scrollHeight:0,"
             "document.body?document.body.scrollHeight:0)",
             [start, visible](JSCValue* v, const std::string&) {
                 double h = v && jsc_value_is_number(v) ? jsc_value_to_double(v) : visible;
                 start(h);
             });
}

#endif  // MINICODE_ENABLE_BROWSER
