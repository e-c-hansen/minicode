// Mermaid.cpp — see Mermaid.h.
#include "Mermaid.h"

#include "LspProcess.h"
#include "MermaidDiagram.h"

#include <glib/gstdio.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace Mermaid {
namespace {

constexpr gint64 kRunLimitUs = 90 * G_USEC_PER_SEC;
constexpr gint64 kFailureUs = 30 * G_USEC_PER_SEC;
constexpr gint64 kCacheMax = 50ll * 1024 * 1024, kCacheKeep = 40ll * 1024 * 1024;

struct Job {
    std::string key, source, theme, mmdc;
};
struct Done {
    std::string key, why;
    std::string png;   // empty: failed
};
struct Kept {
    GdkTexture* texture = nullptr;
};
struct Failure {
    std::string why;
    gint64 at = 0;
};

// Main-thread state.
std::map<std::string, Kept> g_kept;
std::map<std::string, Failure> g_failed;
std::map<std::string, bool> g_inFlight;   // queued or running
std::vector<std::pair<int, std::function<void()>>> g_listeners;
int g_nextListener = 1;
std::atomic<int> g_runs{0};

// The worker.
std::mutex g_mutex;
std::condition_variable g_wake;
std::deque<Job> g_queue;
bool g_workerStarted = false;

std::string mmdcPath() {
    if (const char* e = g_getenv("MINICODE_MMDC"); e && *e)
        return g_file_test(e, G_FILE_TEST_IS_EXECUTABLE) ? e : "";
    return lspgtk::findProgram("mmdc");
}

std::string cacheDir() {
    if (const char* e = g_getenv("MINICODE_MERMAID_CACHE"); e && *e) return e;
    return std::string(g_get_user_cache_dir()) + "/minicode/mermaid";
}

bool readFile(const std::string& path, std::string* out) {
    gchar* data = nullptr;
    gsize n = 0;
    if (!g_file_get_contents(path.c_str(), &data, &n, nullptr)) return false;
    out->assign(data, n);
    g_free(data);
    return true;
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
    std::sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) { return a.mtime < b.mtime; });
    for (const Ent& e : ents) {
        if (total <= kCacheKeep) break;
        if (g_unlink(e.path.c_str()) == 0) total -= e.size;
    }
}

// One mmdc run, on the worker. Fills png on success, else why.
Done runOne(const Job& job) {
    Done done;
    done.key = job.key;
    g_runs++;
    gchar* tmpl = g_dir_make_tmp("minicode-mermaid-XXXXXX", nullptr);
    if (!tmpl) { done.why = "could not make a scratch folder"; return done; }
    const std::string tmp = tmpl;
    g_free(tmpl);
    const std::string in = tmp + "/diagram.mmd", out = tmp + "/diagram.png";
    auto cleanup = [&] {
        g_unlink(in.c_str());
        g_unlink(out.c_str());
        g_rmdir(tmp.c_str());
    };
    if (!g_file_set_contents(in.c_str(), job.source.c_str(), (gssize)job.source.size(), nullptr)) {
        done.why = "could not write the diagram";
        cleanup();
        return done;
    }
    std::vector<std::string> args = {job.mmdc};
    for (const std::string& a : MermaidDiagram::arguments(in, out, job.theme)) args.push_back(a);
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    GPid pid = 0;
    gint errFd = -1;
    GError* err = nullptr;
    if (!g_spawn_async_with_pipes(
            tmp.c_str(), argv.data(), nullptr,
            (GSpawnFlags)(G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL |
                          G_SPAWN_STDIN_FROM_DEV_NULL),
            [](gpointer) { setsid(); }, nullptr, &pid, nullptr, nullptr, &errFd, &err)) {
        done.why = std::string("could not start mmdc: ") + (err ? err->message : "unknown");
        g_clear_error(&err);
        cleanup();
        return done;
    }
    std::string errText;
    const gint64 deadline = g_get_monotonic_time() + kRunLimitUs;
    bool timedOut = false;
    for (;;) {
        const gint64 left = deadline - g_get_monotonic_time();
        if (left <= 0) { timedOut = true; break; }
        struct pollfd p = {errFd, POLLIN, 0};
        const int r = poll(&p, 1, (int)std::min<gint64>(left / 1000 + 1, 1000));
        if (r > 0) {
            char buf[4096];
            const ssize_t n = read(errFd, buf, sizeof buf);
            if (n > 0) {
                if (errText.size() < (1u << 20)) errText.append(buf, (size_t)n);
                continue;
            }
            break;   // EOF: mmdc and its children closed it
        }
    }
    close(errFd);
    if (timedOut) kill(-pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    kill(-pid, SIGKILL);   // anything of the group left
    g_spawn_close_pid(pid);
    std::string png;
    if (timedOut) {
        done.why = "mmdc did not finish within 90 seconds";
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        done.why = MermaidDiagram::errorSummary(errText, "mmdc failed");
    } else if (!readFile(out, &png) || png.empty()) {
        done.why = MermaidDiagram::errorSummary(errText, "mmdc made no picture");
    } else {
        done.png = png;
    }
    cleanup();
    return done;
}

void worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_wake.wait(lock, [] { return !g_queue.empty(); });
            job = g_queue.front();
            g_queue.pop_front();
        }
        auto* done = new Done(runOne(job));
        if (!done->png.empty()) {
            const std::string dir = cacheDir();
            g_mkdir_with_parents(dir.c_str(), 0700);
            const std::string path = dir + "/" + job.key + ".png";
            const std::string tmp = path + ".tmp" + std::to_string((long)getpid());
            if (g_file_set_contents(tmp.c_str(), done->png.data(), (gssize)done->png.size(), nullptr))
                g_rename(tmp.c_str(), path.c_str());
            trimCache(dir);
        }
        g_idle_add([](gpointer p) -> gboolean {
            std::unique_ptr<Done> d(static_cast<Done*>(p));
            g_inFlight.erase(d->key);
            if (!d->png.empty()) {
                GBytes* bytes = g_bytes_new(d->png.data(), d->png.size());
                GdkTexture* tex = gdk_texture_new_from_bytes(bytes, nullptr);
                g_bytes_unref(bytes);
                if (tex) g_kept[d->key].texture = tex;
                else g_failed[d->key] = {"mmdc made a picture that cannot be read", g_get_monotonic_time()};
            } else {
                g_failed[d->key] = {d->why, g_get_monotonic_time()};
            }
            auto listeners = g_listeners;
            for (auto& l : listeners) l.second();
            return G_SOURCE_REMOVE;
        }, done);
    }
}

}  // namespace

bool available() { return !mmdcPath().empty(); }

State want(const std::string& source, bool dark) {
    State s;
    const std::string theme = MermaidDiagram::theme(dark);
    const std::string key = MermaidDiagram::key(source, theme);
    if (auto it = g_kept.find(key); it != g_kept.end()) {
        s.kind = State::Ready;
        s.texture = it->second.texture;
        return s;
    }
    if (auto it = g_failed.find(key); it != g_failed.end()) {
        if (g_get_monotonic_time() - it->second.at < kFailureUs) {
            s.kind = State::Failed;
            s.why = it->second.why;
            return s;
        }
        g_failed.erase(it);
    }
    std::string png;
    if (readFile(cacheDir() + "/" + key + ".png", &png) && !png.empty()) {
        GBytes* bytes = g_bytes_new(png.data(), png.size());
        GdkTexture* tex = gdk_texture_new_from_bytes(bytes, nullptr);
        g_bytes_unref(bytes);
        if (tex) {
            g_kept[key].texture = tex;
            s.kind = State::Ready;
            s.texture = tex;
            return s;
        }
    }
    s.kind = State::Waiting;
    if (g_inFlight.count(key)) return s;
    const std::string mmdc = mmdcPath();
    if (mmdc.empty()) {
        s.kind = State::Failed;
        s.why = "mmdc (mermaid-cli) is not installed";
        return s;
    }
    g_inFlight[key] = true;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.push_back({key, source, theme, mmdc});
        if (!g_workerStarted) {
            g_workerStarted = true;
            std::thread(worker).detach();
        }
    }
    g_wake.notify_one();
    return s;
}

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
}

int runs() { return g_runs; }

}  // namespace Mermaid
