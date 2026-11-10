// StartupLog.cpp — see StartupLog.h.
#include "StartupLog.h"

#include <glib.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace StartupLog {
namespace {

bool gOn = false;
bool gBegun = false;
double gWall0 = 0;            // CLOCK_MONOTONIC at begin(), ms
double gCpu0 = 0;             // the main thread's CPU at begin(), ms
clockid_t gMainClock = CLOCK_THREAD_CPUTIME_ID;
std::set<std::string> gSeen;   // once()'s keys
std::mutex gLock;              // once() and the lines may come from other threads
FirstFrame gFirst;

double clockMs(clockid_t id) {
    timespec ts{};
    if (clock_gettime(id, &ts) != 0) return 0;
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// How long the process ran before main, from its start in /proc (clock
// ticks since boot, so 10 ms steps): the dynamic loader and the libraries'
// constructors, which grow with every library linked.
double msBeforeMain() {
    gchar* stat = nullptr;
    if (!g_file_get_contents("/proc/self/stat", &stat, nullptr, nullptr)) return -1;
    const char* p = strrchr(stat, ')');   // the name may hold spaces
    unsigned long long start = 0;
    if (p) {
        // Fields after the name: state is the 3rd, starttime the 22nd.
        int field = 2;
        for (const char* q = p + 1; *q; ++q) {
            if (*q != ' ') continue;
            if (++field == 22) {
                start = strtoull(q + 1, nullptr, 10);
                break;
            }
        }
    }
    g_free(stat);
    const long ticks = sysconf(_SC_CLK_TCK);
    if (!start || ticks <= 0) return -1;
    return clockMs(CLOCK_BOOTTIME) - (double)start * 1000.0 / (double)ticks;
}

void vprint(const char* format, va_list ap) {
    char what[512];
    vsnprintf(what, sizeof what, format, ap);
    std::lock_guard<std::mutex> hold(gLock);
    fprintf(stderr, "minicode startup: %8.1f ms  cpu %7.1f ms  %s\n", wallMs(), cpuMs(), what);
    fflush(stderr);
}

void print(const char* format, ...) G_GNUC_PRINTF(1, 2);
void print(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    vprint(format, ap);
    va_end(ap);
}

std::string readSmall(const std::string& path) {
    gchar* text = nullptr;
    if (!g_file_get_contents(path.c_str(), &text, nullptr, nullptr)) return "";
    std::string out = text;
    g_free(text);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

// The names of every process below this one (/proc/<pid>/task/*/children),
// for the first frame's line: what the launch started.
void descendants(const std::string& pid, std::vector<std::string>& out, int depth) {
    if (depth > 6) return;
    const std::string tasks = "/proc/" + pid + "/task";
    GDir* dir = g_dir_open(tasks.c_str(), 0, nullptr);
    if (!dir) return;
    while (const char* tid = g_dir_read_name(dir)) {
        const std::string kids = readSmall(tasks + "/" + tid + "/children");
        size_t at = 0;
        while (at < kids.size()) {
            size_t end = kids.find(' ', at);
            if (end == std::string::npos) end = kids.size();
            const std::string child = kids.substr(at, end - at);
            at = end + 1;
            if (child.empty()) continue;
            out.push_back(readSmall("/proc/" + child + "/comm"));
            descendants(child, out, depth + 1);
        }
    }
    g_dir_close(dir);
}

std::string descendantNames() {
    std::vector<std::string> names;
    descendants(std::to_string(getpid()), names, 0);
    std::string out;
    for (const std::string& n : names) out += (out.empty() ? "" : ", ") + n;
    return out.empty() ? "none" : out;
}

long threadCount() {
    long n = 0;
    GDir* dir = g_dir_open("/proc/self/task", 0, nullptr);
    if (!dir) return 0;
    while (g_dir_read_name(dir)) n++;
    g_dir_close(dir);
    return n;
}

// The frames: when each began (before-paint), when GTK's layout and paint
// handlers were done (ours run after theirs), and its end.
struct Frames {
    GdkFrameClock* clock = nullptr;
    int count = 0;
    double begin = 0, layout = 0, paint = 0, cpuBegin = 0;
};
constexpr int kFramesLogged = 3;

void stopWatching(Frames* f) {
    g_signal_handlers_disconnect_by_data(f->clock, f);
    g_object_unref(f->clock);
    delete f;
}

void onBeforePaint(GdkFrameClock*, gpointer fp) {
    auto* f = static_cast<Frames*>(fp);
    f->begin = wallMs();
    f->layout = f->paint = f->begin;
    f->cpuBegin = cpuMs();
}
void onLayout(GdkFrameClock*, gpointer fp) { static_cast<Frames*>(fp)->layout = wallMs(); }
void onPaint(GdkFrameClock*, gpointer fp) { static_cast<Frames*>(fp)->paint = wallMs(); }

void onAfterPaint(GdkFrameClock*, gpointer fp) {
    auto* f = static_cast<Frames*>(fp);
    const double end = wallMs();
    f->count++;
    if (f->count == 1) {
        gFirst.wallMs = end;
        gFirst.cpuMs = cpuMs();
        gFirst.paintMs = end - f->begin;
    }
    if (gOn) {
        print("frame %d: update and layout %.1f ms, paint %.1f ms, after %.1f ms (cpu %.1f ms)",
              f->count, f->layout - f->begin, f->paint - f->layout, end - f->paint,
              cpuMs() - f->cpuBegin);
        if (f->count == 1) {
            print("first frame drawn (process cpu %.1f ms, %ld threads; started: %s)",
                  clockMs(CLOCK_PROCESS_CPUTIME_ID), threadCount(), descendantNames().c_str());
            // Below everything GTK and the app queue in idle time, so this
            // runs once the start's own work has drained.
            g_idle_add_full(G_PRIORITY_LOW, [](gpointer) -> gboolean {
                print("settled: the main loop is idle (process cpu %.1f ms; started: %s)",
                      clockMs(CLOCK_PROCESS_CPUTIME_ID), descendantNames().c_str());
                return G_SOURCE_REMOVE;
            }, nullptr, nullptr);
        }
    }
    if (f->count >= (gOn ? kFramesLogged : 1)) stopWatching(f);
}

}  // namespace

void begin() {
    if (gBegun) return;
    gBegun = true;
    gWall0 = clockMs(CLOCK_MONOTONIC);
    pthread_getcpuclockid(pthread_self(), &gMainClock);
    gCpu0 = clockMs(gMainClock);
    const char* v = g_getenv("MINICODE_STARTUP_LOG");
    gOn = v && *v && strcmp(v, "0") != 0;
    if (gOn) {
        const char* renderer = g_getenv("GSK_RENDERER");
        print("main (the process ran %.0f ms before it; GSK_RENDERER=%s, GDK_BACKEND=%s)",
              msBeforeMain(), renderer ? renderer : "unset",
              g_getenv("GDK_BACKEND") ? g_getenv("GDK_BACKEND") : "unset");
    }
}

bool on() { return gOn; }

double wallMs() { return gBegun ? clockMs(CLOCK_MONOTONIC) - gWall0 : 0; }
double cpuMs() { return gBegun ? clockMs(gMainClock) - gCpu0 : 0; }

void mark(const char* what) {
    if (gOn) print("%s", what);
}

void markf(const char* format, ...) {
    if (!gOn) return;
    va_list ap;
    va_start(ap, format);
    vprint(format, ap);
    va_end(ap);
}

void once(const char* key, const char* format, ...) {
    if (!gOn) return;
    {
        std::lock_guard<std::mutex> hold(gLock);
        if (!gSeen.insert(key).second) return;
    }
    va_list ap;
    va_start(ap, format);
    vprint(format, ap);
    va_end(ap);
}

void watchFrames(GtkWidget* window) {
    static bool watching = false;
    static gulong realized = 0;
    if (watching) return;   // the first window only
    watching = true;
    auto start = +[](GtkWidget* w, gpointer) {
        if (realized) g_signal_handler_disconnect(w, realized);
        realized = 0;
        GdkFrameClock* clock = gtk_widget_get_frame_clock(w);
        if (!clock) return;
        auto* f = new Frames;
        f->clock = GDK_FRAME_CLOCK(g_object_ref(clock));
        g_signal_connect(clock, "before-paint", G_CALLBACK(onBeforePaint), f);
        g_signal_connect(clock, "layout", G_CALLBACK(onLayout), f);
        g_signal_connect(clock, "paint", G_CALLBACK(onPaint), f);
        g_signal_connect(clock, "after-paint", G_CALLBACK(onAfterPaint), f);
        mark("window: realized (its surface and frame clock made)");
    };
    if (gtk_widget_get_realized(window)) start(window, nullptr);
    else realized = g_signal_connect_after(window, "realize", G_CALLBACK(start), nullptr);
}

FirstFrame firstFrame() { return gFirst; }

}  // namespace StartupLog
