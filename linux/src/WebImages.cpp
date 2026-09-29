// WebImages.cpp — see WebImages.h.
#include "WebImages.h"
#include "WebImageRules.h"

#include <map>
#include <utility>
#include <vector>

#ifdef MINICODE_ENABLE_WEB_IMAGES
#include <libsoup/soup.h>
#endif

#ifndef MINICODE_VERSION
#define MINICODE_VERSION "dev"
#endif

#define WEB_LOG_DOMAIN "minicode-web"

WebImages::Picture::~Picture() {
    if (texture) g_object_unref(texture);
    if (animation) g_object_unref(animation);
}

bool WebImages::isFetchable(const std::string& src) { return WebImageRules::isFetchable(src); }

#ifndef MINICODE_ENABLE_WEB_IMAGES

bool WebImages::available() { return false; }
WebImages::PicturePtr WebImages::cached(const std::string&) { return nullptr; }
bool WebImages::recentlyFailed(const std::string&) { return false; }
void WebImages::fetch(const std::string&, std::function<void(PicturePtr)>) {}

#else

namespace {

using WebImages::Picture;
using WebImages::PicturePtr;
namespace R = WebImageRules;

// One session for the whole process. libsoup 3 starts a session with no
// cookie jar and no cache (both are features that have to be added), so
// nothing a picture's server sets is kept or sent back, and nothing is
// written to disk. TLS certificates are checked against the system's
// store, as for any libsoup client; a picture from a server that fails
// the check is a failed picture. The proxy is the desktop's.
SoupSession* session() {
    static SoupSession* s = nullptr;
    if (!s)
        s = soup_session_new_with_options("user-agent", "MiniCode/" MINICODE_VERSION,
                                          "timeout", (guint)R::kTimeoutSeconds, nullptr);
    return s;
}

R::ByteLru<PicturePtr>& cache() {
    static R::ByteLru<PicturePtr> c(R::kCacheBytes);
    return c;
}

R::FailureMemory& failures() {
    static R::FailureMemory f(R::kFailureSeconds);
    return f;
}

double now() { return (double)g_get_monotonic_time() / G_USEC_PER_SEC; }

// A fetch in progress, and everyone waiting for it.
struct Fetch {
    std::string key;   // the address as written, which the cache is keyed on
    std::string url;   // where it is being fetched from now, after redirects
    std::vector<std::function<void(PicturePtr)>> waiters;
    GCancellable* cancel = nullptr;
    SoupMessage* msg = nullptr;
    GInputStream* body = nullptr;
    GByteArray* data = nullptr;
    std::size_t rawBytes = 0;   // the body's size, once it is all here
    int redirects = 0;
    guint deadline = 0;
    bool timedOut = false;
};

std::map<std::string, Fetch*>& inflight() {
    static std::map<std::string, Fetch*> m;
    return m;
}

// The end of a fetch: the picture is kept (or the failure remembered) and
// every waiter is told.
void finish(Fetch* f, PicturePtr pic, const std::string& why) {
    if (pic) {
        const std::size_t cost = R::decodedCost(pic->width, pic->height, pic->animation != nullptr,
                                                f->rawBytes);
        const bool kept = cache().put(f->key, pic, cost);
        g_log(WEB_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "done %s: %dx%d%s, %zu bytes decoded%s; "
              "cache %zu pictures, %zu bytes", f->key.c_str(), pic->width, pic->height,
              pic->animation ? " animated" : "", cost, kept ? "" : ", too big to keep",
              cache().size(), cache().bytes());
    } else {
        failures().remember(f->key, now());
        g_log(WEB_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "failed %s: %s", f->key.c_str(), why.c_str());
    }
    inflight().erase(f->key);
    if (f->deadline) g_source_remove(f->deadline);
    auto waiters = std::move(f->waiters);
    g_clear_object(&f->body);
    g_clear_object(&f->msg);
    g_clear_object(&f->cancel);
    if (f->data) g_byte_array_unref(f->data);
    delete f;
    for (auto& w : waiters) w(pic);
}

void finishError(Fetch* f, GError* err, const char* what) {
    std::string why = f->timedOut ? std::string("no answer within ") +
                                        std::to_string(R::kDeadlineSeconds) + " s"
                                  : std::string(what) + ": " + (err ? err->message : "failed");
    g_clear_error(&err);
    finish(f, nullptr, why);
}

// Runs on a worker thread: the bytes as a picture, the way Markdown.cpp
// decodes a local one (a GIF as an animation when it has several frames,
// anything else as a texture), or null. The size the header claims is
// checked before anything is decoded.
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
Picture* decode(GBytes* bytes) {
    gsize len = 0;
    const auto* d = static_cast<const unsigned char*>(g_bytes_get_data(bytes, &len));
    int w = 0, h = 0;
    if (R::headerSize(d, len, &w, &h) && !R::sizeAllowed(w, h)) return nullptr;
    auto* p = new Picture;
    if (R::isGif(d, len)) {
        GInputStream* in = g_memory_input_stream_new_from_bytes(bytes);
        GdkPixbufAnimation* anim = gdk_pixbuf_animation_new_from_stream(in, nullptr, nullptr);
        g_object_unref(in);
        if (anim && !gdk_pixbuf_animation_is_static_image(anim)) {
            p->animation = anim;
            p->width = gdk_pixbuf_animation_get_width(anim);
            p->height = gdk_pixbuf_animation_get_height(anim);
        } else if (anim) {
            g_object_unref(anim);
        }
    }
    if (!p->animation) {
        p->texture = gdk_texture_new_from_bytes(bytes, nullptr);
        if (p->texture) {
            p->width = gdk_texture_get_width(p->texture);
            p->height = gdk_texture_get_height(p->texture);
        }
    }
    if ((!p->texture && !p->animation) || !R::sizeAllowed(p->width, p->height)) {
        delete p;
        return nullptr;
    }
    return p;
}
G_GNUC_END_IGNORE_DEPRECATIONS

void onDecoded(GObject*, GAsyncResult* res, gpointer data) {
    Fetch* f = static_cast<Fetch*>(data);
    auto* p = static_cast<Picture*>(g_task_propagate_pointer(G_TASK(res), nullptr));
    finish(f, PicturePtr(p), "not a picture this system can decode");
}

void startDecode(Fetch* f) {
    if (f->deadline) g_source_remove(f->deadline);
    f->deadline = 0;
    g_clear_object(&f->body);
    f->rawBytes = f->data->len;
    GBytes* bytes = g_byte_array_free_to_bytes(f->data);
    f->data = nullptr;
    GTask* task = g_task_new(nullptr, nullptr, onDecoded, f);
    g_task_set_task_data(task, bytes, (GDestroyNotify)g_bytes_unref);
    g_task_run_in_thread(task, [](GTask* t, gpointer, gpointer data, GCancellable*) {
        g_task_return_pointer(t, decode(static_cast<GBytes*>(data)), [](gpointer p) {
            delete static_cast<Picture*>(p);
        });
    });
    g_object_unref(task);
}

void readMore(Fetch* f);

void onRead(GObject* src, GAsyncResult* res, gpointer data) {
    Fetch* f = static_cast<Fetch*>(data);
    GError* err = nullptr;
    GBytes* chunk = g_input_stream_read_bytes_finish(G_INPUT_STREAM(src), res, &err);
    if (!chunk) return finishError(f, err, "reading");
    const gsize n = g_bytes_get_size(chunk);
    if (n == 0) {
        g_bytes_unref(chunk);
        startDecode(f);
        return;
    }
    if (f->data->len + n > R::kMaxBytes) {
        g_bytes_unref(chunk);
        g_cancellable_cancel(f->cancel);   // the connection goes with the rest
        finish(f, nullptr, "larger than 20 MB, cancelled after " +
                               std::to_string(f->data->len) + " bytes");
        return;
    }
    g_byte_array_append(f->data, static_cast<const guint8*>(g_bytes_get_data(chunk, nullptr)),
                        (guint)n);
    g_bytes_unref(chunk);
    readMore(f);
}

void readMore(Fetch* f) {
    g_input_stream_read_bytes_async(f->body, 64 * 1024, G_PRIORITY_LOW, f->cancel, onRead, f);
}

void send(Fetch* f);

void onSent(GObject* src, GAsyncResult* res, gpointer data) {
    Fetch* f = static_cast<Fetch*>(data);
    GError* err = nullptr;
    GInputStream* in = soup_session_send_finish(SOUP_SESSION(src), res, &err);
    if (!in) return finishError(f, err, "request");
    const guint status = soup_message_get_status(f->msg);
    SoupMessageHeaders* headers = soup_message_get_response_headers(f->msg);
    g_log(WEB_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "answer %s: %u", f->url.c_str(), status);

    // Redirects are followed here rather than by libsoup, so that one to a
    // plain http:// address is refused like an http:// picture is.
    if (SOUP_STATUS_IS_REDIRECTION(status)) {
        g_cancellable_cancel(f->cancel);   // the body is not wanted
        g_object_unref(in);
        const char* loc = soup_message_headers_get_one(headers, "Location");
        char* next = loc ? g_uri_resolve_relative(f->url.c_str(), loc, G_URI_FLAGS_NONE, nullptr)
                         : nullptr;
        const std::string to = next ? next : "";
        g_free(next);
        if (to.empty() || !R::isFetchable(to)) {
            finish(f, nullptr, "redirected to " + (to.empty() ? std::string("nowhere") : to) +
                                   ", which is not https");
            return;
        }
        if (++f->redirects > R::kMaxRedirects) {
            finish(f, nullptr, "too many redirects");
            return;
        }
        g_clear_object(&f->cancel);
        f->cancel = g_cancellable_new();
        f->url = to;
        send(f);
        return;
    }
    if (!SOUP_STATUS_IS_SUCCESSFUL(status)) {
        g_cancellable_cancel(f->cancel);
        g_object_unref(in);
        finish(f, nullptr, "HTTP " + std::to_string(status));
        return;
    }
    if (soup_message_headers_get_encoding(headers) == SOUP_ENCODING_CONTENT_LENGTH &&
        soup_message_headers_get_content_length(headers) > (goffset)R::kMaxBytes) {
        g_cancellable_cancel(f->cancel);   // nothing of the body is read
        g_object_unref(in);
        finish(f, nullptr, "Content-Length " +
                               std::to_string(soup_message_headers_get_content_length(headers)) +
                               " is over 20 MB");
        return;
    }
    f->body = in;
    f->data = g_byte_array_new();
    readMore(f);
}

void send(Fetch* f) {
    g_clear_object(&f->msg);
    f->msg = soup_message_new(SOUP_METHOD_GET, f->url.c_str());
    if (!f->msg) {
        finish(f, nullptr, "not a valid address");
        return;
    }
    soup_message_add_flags(f->msg, SOUP_MESSAGE_NO_REDIRECT);
    soup_message_headers_replace(soup_message_get_request_headers(f->msg), "Accept",
                                 "image/*,*/*;q=0.8");
    g_log(WEB_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "request %s", f->url.c_str());
    soup_session_send_async(session(), f->msg, G_PRIORITY_LOW, f->cancel, onSent, f);
}

gboolean onDeadline(gpointer data) {
    Fetch* f = static_cast<Fetch*>(data);
    f->deadline = 0;
    f->timedOut = true;
    g_cancellable_cancel(f->cancel);   // the pending send or read fails, and finishes it
    return G_SOURCE_REMOVE;
}

}  // namespace

bool WebImages::available() { return true; }

WebImages::PicturePtr WebImages::cached(const std::string& url) {
    PicturePtr* p = cache().get(url);
    return p ? *p : nullptr;
}

bool WebImages::recentlyFailed(const std::string& url) {
    return failures().recent(url, now());
}

void WebImages::fetch(const std::string& url, std::function<void(PicturePtr)> done) {
    auto& live = inflight();
    auto it = live.find(url);
    if (it != live.end()) {
        it->second->waiters.push_back(std::move(done));
        return;
    }
    // Kept, or failed lately (a render and a fetch in one turn cannot see
    // this, but a caller that did not ask first can): answered from an idle.
    PicturePtr kept = cached(url);
    if (kept || !isFetchable(url) || recentlyFailed(url)) {
        auto* job = new std::pair<std::function<void(PicturePtr)>, PicturePtr>(std::move(done), kept);
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, [](gpointer d) -> gboolean {
            auto* j = static_cast<std::pair<std::function<void(PicturePtr)>, PicturePtr>*>(d);
            j->first(j->second);
            return G_SOURCE_REMOVE;
        }, job, [](gpointer d) {
            delete static_cast<std::pair<std::function<void(PicturePtr)>, PicturePtr>*>(d);
        });
        return;
    }
    Fetch* f = new Fetch;
    f->key = f->url = url;
    f->waiters.push_back(std::move(done));
    f->cancel = g_cancellable_new();
    live[url] = f;
    f->deadline = g_timeout_add_seconds(R::kDeadlineSeconds, onDeadline, f);
    // Sent from an idle, so the render that asked is over first.
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, [](gpointer d) -> gboolean {
        send(static_cast<Fetch*>(d));
        return G_SOURCE_REMOVE;
    }, f, nullptr);
}

#endif  // MINICODE_ENABLE_WEB_IMAGES
