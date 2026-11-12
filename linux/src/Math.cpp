// Math.cpp — see Math.h.
#include "Math.h"

#ifdef MINICODE_ENABLE_PDF

#include "Latex.h"
#include "MathTex.h"

#include <cairo.h>
#include <glib/gstdio.h>
#include <poll.h>
#include <poppler.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>

namespace Math {
namespace {

constexpr double kBpPerPt = 72.0 / 72.27;
constexpr double kPxPerBp = 1.4;        // a formula's size on screen: 10 pt TeX next to 13 px text
constexpr int kPictureScale = 2;        // pixels per logical pixel
constexpr gint64 kStallUs = 15 * G_USEC_PER_SEC, kQuietUs = 60 * G_USEC_PER_SEC,
                 kRunLimitUs = 600 * G_USEC_PER_SEC;
constexpr gint64 kRetryUs = 60 * G_USEC_PER_SEC;
constexpr gint64 kCacheMax = 50ll * 1024 * 1024, kCacheKeep = 40ll * 1024 * 1024;
constexpr guint kGatherMs = 50;
const char* kIndexHeader = "minicode-math-index 1";

struct Item {
    std::string key, tex;
    bool display = false;
};
struct Placed {
    std::string key, batch;
    int page = 0;
    double descent = 0;   // bp below the baseline, pad included
};
struct Result {
    std::vector<Placed> placed;
    std::map<std::string, std::string> failed;      // key -> complaint, for good
    std::map<std::string, std::string> transient;   // key -> complaint, retried later
};
struct Entry {
    std::string batch;
    int page = 0;
    double descent = 0;
};
struct Kept {
    GdkTexture* texture = nullptr;
    int w = 0, h = 0, depth = 0;
};
struct Failure {
    std::string why;
    gint64 at = 0;
    bool permanent = false;
};

// Main-thread state.
std::map<std::string, Kept> g_kept;            // key + color
std::map<std::string, Failure> g_failed;       // key
std::map<std::string, Entry> g_index;          // key -> where the cache has it
bool g_indexLoaded = false;
std::map<std::string, PopplerDocument*> g_docs;   // batch -> its PDF
std::vector<Item> g_requests;
std::set<std::string> g_requested;             // queued or out
bool g_running = false;
guint g_timer = 0;
std::vector<std::pair<int, std::function<void()>>> g_listeners;
int g_nextListener = 1;
std::atomic<int> g_runs{0};

std::string cacheDir() {
    if (const char* e = g_getenv("MINICODE_MATH_CACHE"); e && *e) return e;
    return std::string(g_get_user_cache_dir()) + "/minicode/math";
}

bool readFile(const std::string& path, std::string* out) {
    gchar* data = nullptr;
    gsize n = 0;
    if (!g_file_get_contents(path.c_str(), &data, &n, nullptr)) return false;
    out->assign(data, n);
    g_free(data);
    return true;
}

std::string keyOf(const std::string& tex, bool display) {
    const std::string src = std::to_string(MathTex::kVersion) + (display ? "\nD\n" : "\nI\n") + tex;
    char* h = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                          reinterpret_cast<const guchar*>(src.data()), src.size());
    std::string out = h;
    g_free(h);
    return out;
}

void trimCache(const std::string& dir) {
    struct Ent { std::string path; gint64 mtime; gint64 size; };
    std::vector<Ent> ents;
    gint64 total = 0;
    GDir* d = g_dir_open(dir.c_str(), 0, nullptr);
    if (!d) return;
    while (const char* name = g_dir_read_name(d)) {
        const std::string p = dir + "/" + name;
        struct stat st;
        if (g_stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        ents.push_back({p, (gint64)st.st_mtime, (gint64)st.st_size});
        total += st.st_size;
    }
    g_dir_close(d);
    if (total <= kCacheMax) return;
    std::sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) {
        return a.mtime != b.mtime ? a.mtime < b.mtime : a.path < b.path;
    });
    for (const Ent& e : ents) {
        if (total <= kCacheKeep) break;
        if (g_unlink(e.path.c_str()) == 0) total -= e.size;
    }
}

// ------------------------------------------------------------ the worker

std::string batchName() {
    static std::atomic<unsigned> counter{0};
    char buf[64];
    g_snprintf(buf, sizeof buf, "%015lld-%04u-%08x", (long long)(g_get_real_time() / 1000),
               ++counter % 10000, g_random_int());
    return buf;
}

void removeTree(const std::string& dir) {
    if (GDir* d = g_dir_open(dir.c_str(), 0, nullptr)) {
        while (const char* name = g_dir_read_name(d)) g_unlink((dir + "/" + name).c_str());
        g_dir_close(d);
    }
    g_rmdir(dir.c_str());
}

struct Once {
    Result r;
    std::vector<size_t> again;   // go round again
    std::string whole;           // nothing reached TeX
};

// The last "BEGIN:k" in what TeX printed: the formula it was in.
long lastBegun(const std::string& output, const std::string& begin) {
    const size_t at = output.rfind(begin);
    if (at == std::string::npos) return -1;
    return std::strtol(output.c_str() + at + begin.size(), nullptr, 10);
}

// Runs tectonic once on items[which], on the worker.
Once runOnce(const std::string& tool, const std::vector<Item>& items, const std::vector<size_t>& which) {
    Once run;
    g_runs++;
    std::vector<MathTex::Formula> formulas;
    for (size_t i : which) formulas.push_back({items[i].tex, items[i].display});
    gchar* tmpl = g_dir_make_tmp("minicode-math-XXXXXX", nullptr);
    if (!tmpl) { run.whole = "Could not make a scratch folder for the formulas."; return run; }
    const std::string dir = tmpl;
    g_free(tmpl);
    char nonceBuf[24];
    g_snprintf(nonceBuf, sizeof nonceBuf, "%08x%08x", g_random_int(), g_random_int());
    const std::string nonce = nonceBuf;
    const std::string texPath = dir + "/math.tex";
    const std::string source = MathTex::document(formulas, nonce);
    if (!g_file_set_contents(texPath.c_str(), source.c_str(), (gssize)source.size(), nullptr)) {
        removeTree(dir);
        run.whole = "The formulas could not be written out.";
        return run;
    }
    // --print streams TeX's own output, so a run that hangs still says which
    // formula it hung in. One pass is enough: nothing refers to anything.
    std::vector<std::string> args = {tool, "-Z", "continue-on-errors", "--untrusted", "--print",
                                     "--keep-logs", "--reruns", "0", "--color", "never",
                                     "--outdir", dir, texPath};
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    GPid pid = 0;
    gint outFd = -1, errFd = -1;
    GError* err = nullptr;
    if (!g_spawn_async_with_pipes(
            dir.c_str(), argv.data(), nullptr,
            (GSpawnFlags)(G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDIN_FROM_DEV_NULL),
            [](gpointer) { setsid(); }, nullptr, &pid, nullptr, &outFd, &errFd, &err)) {
        run.whole = std::string("tectonic did not start: ") + (err ? err->message : "unknown");
        g_clear_error(&err);
        removeTree(dir);
        return run;
    }
    std::string output;
    const std::string begin = MathTex::logMark(nonce) + "BEGIN:";
    const gint64 started = g_get_monotonic_time();
    gint64 lastWord = started;
    bool inFormulas = false, killed = false;
    int open = 2;
    struct pollfd fds[2] = {{outFd, POLLIN, 0}, {errFd, POLLIN, 0}};
    while (open > 0) {
        const int r = poll(fds, 2, 250);
        const gint64 now = g_get_monotonic_time();
        if (r > 0) {
            for (auto& f : fds) {
                if (f.fd < 0 || !(f.revents & (POLLIN | POLLHUP | POLLERR))) continue;
                char buf[8192];
                const ssize_t n = read(f.fd, buf, sizeof buf);
                if (n > 0) {
                    if (output.size() < (64u << 20)) output.append(buf, (size_t)n);
                    lastWord = now;
                    if (!inFormulas) inFormulas = output.find(begin) != std::string::npos;
                } else {
                    close(f.fd);
                    f.fd = -1;
                    open--;
                }
            }
        }
        if (!killed && (now - started > kRunLimitUs ||
                        now - lastWord > (inFormulas ? kStallUs : kQuietUs))) {
            killed = true;
            kill(-pid, SIGKILL);
        }
    }
    int status = 0;
    waitpid(pid, &status, 0);
    kill(-pid, SIGKILL);   // anything of the group left
    g_spawn_close_pid(pid);

    std::string log, pdf;
    if (!readFile(dir + "/math.log", &log)) log.clear();
    if (!killed) readFile(dir + "/math.pdf", &pdf);
    removeTree(dir);
    if (log.empty()) log = output;

    if (killed) {
        const long culprit = lastBegun(output, begin);
        if (culprit < 0 || (size_t)culprit >= which.size()) {
            run.whole = "tectonic did not finish in time.";
            return run;
        }
        for (size_t k = 0; k < which.size(); k++) {
            if ((long)k == culprit) run.r.failed[items[which[k]].key] = "TeX did not finish this formula.";
            else run.again.push_back(which[k]);
        }
        return run;
    }

    const std::vector<MathTex::Box> boxes = MathTex::readLog(log, which.size(), nonce);
    bool anyReached = false;
    for (const MathTex::Box& b : boxes) anyReached = anyReached || b.reached;
    if (!anyReached) {
        const std::string why = MathTex::firstError(output);
        run.whole = why.empty() ? "tectonic did not typeset the formulas." : why;
        return run;
    }
    PopplerDocument* doc = nullptr;
    if (!pdf.empty()) {
        GBytes* bytes = g_bytes_new(pdf.data(), pdf.size());
        doc = poppler_document_new_from_bytes(bytes, nullptr, nullptr);
        g_bytes_unref(bytes);
    }
    const int pageCount = doc ? poppler_document_get_n_pages(doc) : 0;
    const std::string batch = batchName();
    std::string index = std::string(kIndexHeader) + "\n";
    for (size_t k = 0; k < which.size(); k++) {
        const MathTex::Box& b = boxes[k];
        const Item& item = items[which[k]];
        if (!b.error.empty()) { run.r.failed[item.key] = b.error; continue; }
        if (!doc) {
            // tectonic stopped with no PDF: in this formula if TeX began it
            // and never finished, else it is only waiting its turn again.
            if (b.reached && !b.measured) {
                const std::string why = MathTex::firstError(output);
                run.r.failed[item.key] = why.empty() ? "TeX stopped in this formula." : why;
            } else {
                run.again.push_back(which[k]);
            }
            continue;
        }
        if (!b.reached) { run.again.push_back(which[k]); continue; }   // the run ended early
        // The page must be the size the log gave the box, or it is not this
        // formula's page.
        double pw = 0, ph = 0;
        bool ok = b.measured && b.page >= 1 && b.page <= pageCount;
        if (ok) {
            PopplerPage* page = poppler_document_get_page(doc, b.page - 1);
            if (page) { poppler_page_get_size(page, &pw, &ph); g_object_unref(page); }
            else ok = false;
        }
        const double pad = MathTex::kPadPt;
        if (!ok || std::fabs(pw - (b.width + 2 * pad) * kBpPerPt) > 0.1 ||
            std::fabs(ph - (b.height + b.depth + 2 * pad) * kBpPerPt) > 0.1 || b.width <= 0) {
            run.r.failed[item.key] = (ok && b.width <= 0) ? "The formula came out empty."
                                                          : "The formula could not be measured.";
            continue;
        }
        const double descent = (b.depth + pad) * kBpPerPt;
        run.r.placed.push_back({item.key, batch, b.page, descent});
        char line[256];
        g_snprintf(line, sizeof line, "%s %d %.4f\n", item.key.c_str(), b.page, descent);
        index += line;
    }
    if (doc) g_object_unref(doc);
    if (!run.r.placed.empty()) {
        const std::string cache = cacheDir();
        g_mkdir_with_parents(cache.c_str(), 0700);
        auto put = [&](const std::string& ext, const std::string& data) {
            const std::string path = cache + "/" + batch + ext, tmp = path + ".tmp";
            if (!g_file_set_contents(tmp.c_str(), data.data(), (gssize)data.size(), nullptr)) return false;
            return g_rename(tmp.c_str(), path.c_str()) == 0;
        };
        if (put(".pdf", pdf)) put(".idx", index);
        else run.r.placed.clear();
        trimCache(cache);
    }
    return run;
}

// A whole batch: runs until every formula is settled, or no run settles any.
Result runBatch(const std::string& tool, const std::vector<Item>& items) {
    Result all;
    std::vector<size_t> todo;
    for (size_t i = 0; i < items.size(); i++) todo.push_back(i);
    for (int round = 0; !todo.empty() && round < 16; round++) {
        Once run = runOnce(tool, items, todo);
        for (auto& f : run.r.failed) all.failed[f.first] = f.second;
        all.placed.insert(all.placed.end(), run.r.placed.begin(), run.r.placed.end());
        if (!run.whole.empty() || (run.r.failed.empty() && run.r.placed.empty())) {
            // Nothing settled: try these again in a while.
            const std::string why = run.whole.empty() ? "tectonic did not typeset the formulas." : run.whole;
            for (size_t i : todo) all.transient[items[i].key] = why;
            return all;
        }
        todo = run.again;
    }
    for (size_t i : todo) all.transient[items[i].key] = "tectonic did not typeset the formulas.";
    return all;
}

// ------------------------------------------------------------ the main thread

void loadIndex() {
    if (g_indexLoaded) return;
    g_indexLoaded = true;
    const std::string dir = cacheDir();
    GDir* d = g_dir_open(dir.c_str(), 0, nullptr);
    if (!d) return;
    std::vector<std::string> names;
    while (const char* name = g_dir_read_name(d))
        if (g_str_has_suffix(name, ".idx")) names.push_back(name);
    g_dir_close(d);
    std::sort(names.begin(), names.end());   // later batches win
    for (const std::string& n : names) {
        std::string text;
        if (!readFile(dir + "/" + n, &text)) continue;
        const std::string batch = n.substr(0, n.size() - 4);
        size_t a = 0;
        bool first = true;
        while (a < text.size()) {
            size_t e = text.find('\n', a);
            if (e == std::string::npos) e = text.size();
            const std::string line = text.substr(a, e - a);
            a = e + 1;
            if (first) { first = false; if (line != kIndexHeader) break; continue; }
            char key[80];
            int page = 0;
            double descent = 0;
            if (std::sscanf(line.c_str(), "%79s %d %lf", key, &page, &descent) == 3)
                g_index[key] = {batch, page, descent};
        }
    }
}

PopplerDocument* docFor(const std::string& batch) {
    if (auto it = g_docs.find(batch); it != g_docs.end()) return it->second;
    std::string pdf;
    PopplerDocument* doc = nullptr;
    if (readFile(cacheDir() + "/" + batch + ".pdf", &pdf)) {
        GBytes* bytes = g_bytes_new(pdf.data(), pdf.size());
        doc = poppler_document_new_from_bytes(bytes, nullptr, nullptr);
        g_bytes_unref(bytes);
    }
    g_docs[batch] = doc;
    return doc;
}

// The page as a mask of its ink filled with `color`.
bool pictureFor(const Entry& e, const std::string& color, Kept* out) {
    PopplerDocument* doc = docFor(e.batch);
    if (!doc || e.page < 1 || e.page > poppler_document_get_n_pages(doc)) return false;
    PopplerPage* page = poppler_document_get_page(doc, e.page - 1);
    if (!page) return false;
    double pw = 0, ph = 0;
    poppler_page_get_size(page, &pw, &ph);
    const int lw = std::max(1, (int)std::lround(pw * kPxPerBp)), lh = std::max(1, (int)std::lround(ph * kPxPerBp));
    const int w = lw * kPictureScale, h = lh * kPictureScale;
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t* cr = cairo_create(surf);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_scale(cr, w / pw, h / ph);
    poppler_page_render(page, cr);
    cairo_destroy(cr);
    g_object_unref(page);
    cairo_surface_flush(surf);
    GdkRGBA rgba = {1, 1, 1, 1};
    gdk_rgba_parse(&rgba, color.c_str());
    const int cr8 = (int)std::lround(rgba.red * 255), cg8 = (int)std::lround(rgba.green * 255),
              cb8 = (int)std::lround(rgba.blue * 255);
    const int stride = cairo_image_surface_get_stride(surf);
    unsigned char* src = cairo_image_surface_get_data(surf);
    std::vector<unsigned char> px((size_t)stride * (size_t)h);
    for (int y = 0; y < h; y++) {
        const guint32* row = reinterpret_cast<const guint32*>(src + (size_t)y * (size_t)stride);
        guint32* dst = reinterpret_cast<guint32*>(px.data() + (size_t)y * (size_t)stride);
        for (int x = 0; x < w; x++) {
            const guint32 p = row[x];
            const int lum = (int)(((p >> 16) & 255) + ((p >> 8) & 255) + (p & 255)) / 3;
            const int a = 255 - lum;
            dst[x] = ((guint32)a << 24) | ((guint32)(cr8 * a / 255) << 16) |
                     ((guint32)(cg8 * a / 255) << 8) | (guint32)(cb8 * a / 255);
        }
    }
    cairo_surface_destroy(surf);
    GBytes* bytes = g_bytes_new(px.data(), px.size());
    GdkTexture* tex = gdk_memory_texture_new(
        w, h, G_BYTE_ORDER == G_LITTLE_ENDIAN ? GDK_MEMORY_B8G8R8A8_PREMULTIPLIED
                                              : GDK_MEMORY_A8R8G8B8_PREMULTIPLIED,
        bytes, (gsize)stride);
    g_bytes_unref(bytes);
    out->texture = tex;
    out->w = lw;
    out->h = lh;
    out->depth = (int)std::lround(e.descent * kPxPerBp);
    return true;
}

struct Done {
    Result r;
    std::vector<Item> items;
};

void schedule();

void fire() {
    g_timer = 0;
    if (g_running || g_requests.empty()) return;
    const std::string tool = LatexPreview::tectonicPath();
    auto* batch = new std::vector<Item>(std::move(g_requests));
    g_requests.clear();
    if (tool.empty()) {
        for (const Item& i : *batch) {
            g_requested.erase(i.key);
            g_failed[i.key] = {"tectonic is not installed.", g_get_monotonic_time(), false};
        }
        delete batch;
        auto ls = g_listeners;
        for (auto& l : ls) l.second();
        return;
    }
    g_running = true;
    std::thread([tool, batch] {
        auto* done = new Done();
        done->items = *batch;
        done->r = runBatch(tool, *batch);
        delete batch;
        g_idle_add([](gpointer p) -> gboolean {
            std::unique_ptr<Done> d(static_cast<Done*>(p));
            g_running = false;
            for (const Item& i : d->items) g_requested.erase(i.key);
            const gint64 now = g_get_monotonic_time();
            for (const Placed& pl : d->r.placed) g_index[pl.key] = {pl.batch, pl.page, pl.descent};
            for (auto& f : d->r.failed) g_failed[f.first] = {f.second, now, true};
            for (auto& f : d->r.transient) g_failed[f.first] = {f.second, now, false};
            auto ls = g_listeners;
            for (auto& l : ls) l.second();
            if (!g_requests.empty()) schedule();
            return G_SOURCE_REMOVE;
        }, done);
    }).detach();
}

void schedule() {
    if (g_timer || g_running) return;
    g_timer = g_timeout_add(kGatherMs, [](gpointer) -> gboolean { fire(); return G_SOURCE_REMOVE; }, nullptr);
}

}  // namespace

bool built() { return true; }
bool available() { return !LatexPreview::tectonicPath().empty(); }

State want(const std::string& tex, bool display, const std::string& color) {
    State s;
    const std::string key = keyOf(tex, display);
    const std::string mem = key + color;
    auto ready = [&](const Kept& k) {
        s.kind = State::Ready;
        s.texture = k.texture;
        s.width = k.w;
        s.height = k.h;
        s.depth = k.depth;
        return s;
    };
    if (auto it = g_kept.find(mem); it != g_kept.end()) return ready(it->second);
    if (auto it = g_failed.find(key); it != g_failed.end()) {
        if (it->second.permanent || g_get_monotonic_time() - it->second.at < kRetryUs) {
            s.kind = State::Failed;
            s.why = it->second.why;
            return s;
        }
        g_failed.erase(it);
    }
    loadIndex();
    if (auto it = g_index.find(key); it != g_index.end()) {
        Kept k;
        if (pictureFor(it->second, color, &k)) return ready(g_kept[mem] = k);
        g_index.erase(it);
    }
    s.kind = State::Waiting;
    if (g_requested.count(key)) return s;
    if (const std::string why = MathTex::check(tex); !why.empty()) {
        g_failed[key] = {why, g_get_monotonic_time(), true};
        s.kind = State::Failed;
        s.why = why;
        return s;
    }
    g_requested.insert(key);
    g_requests.push_back({key, tex, display});
    schedule();
    return s;
}

int outstanding() { return (int)g_requested.size(); }

int subscribe(std::function<void()> fn) {
    g_listeners.emplace_back(g_nextListener, std::move(fn));
    return g_nextListener++;
}

void unsubscribe(int id) {
    g_listeners.erase(std::remove_if(g_listeners.begin(), g_listeners.end(),
                                     [id](const auto& l) { return l.first == id; }),
                      g_listeners.end());
}

void forgetMemory() {
    for (auto& k : g_kept) g_object_unref(k.second.texture);
    g_kept.clear();
    g_failed.clear();
    g_index.clear();
    g_indexLoaded = false;
    for (auto& d : g_docs) if (d.second) g_object_unref(d.second);
    g_docs.clear();
}

int runs() { return g_runs; }

}  // namespace Math

#else  // no poppler: a formula stays its TeX

namespace Math {
bool built() { return false; }
bool available() { return false; }
State want(const std::string&, bool, const std::string&) { State s; s.kind = State::Failed; return s; }
int outstanding() { return 0; }
int subscribe(std::function<void()>) { return 0; }
void unsubscribe(int) {}
void forgetMemory() {}
int runs() { return 0; }
}  // namespace Math

#endif
