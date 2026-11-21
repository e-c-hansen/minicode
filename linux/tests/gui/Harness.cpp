// Harness.cpp — see Harness.h: the checks' stage. The wrapper is Wrapper.cpp,
// real input is Input.cpp, and the app's parts come from AppSeam.inc.
#include "Harness.h"

#include "Browser.h"
#include "DocumentView.h"
#include "Editor.h"
#include "FileTree.h"
#include "ControlProtocol.h"
#include "Search.h"
#include "SessionLink.h"
#include "TerminalAttention.h"
#include "TerminalDock.h"
#include "WindowLayout.h"
#include "../../../tests/GuiRetry.h"

#include <execinfo.h>
#include <limits.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

int mutible_main(int argc, char** argv);    // src/main.cpp, renamed
int MCGuiWrapperMain(int argc, char** argv); // Wrapper.cpp
bool MCGuiIsDbusDaemonShim(int argc, char** argv);
int MCGuiDbusDaemonShim(char** argv);

// -------------------------------------------------------------- registry
namespace {
struct Suite {
    std::string name;
    void (*body)();
};
std::vector<Suite>& suites() {
    static std::vector<Suite> s;
    return s;
}
}  // namespace

void MCGuiRegisterSuite(const char* name, void (*body)()) { suites().push_back({name, body}); }

// ----------------------------------------------------------------- items
namespace {
enum class Kind { Step, Wait, Async, Suite };
struct Item {
    Kind kind = Kind::Step;
    std::string name;
    std::function<void()> block;
    std::function<bool()> until;
    std::function<void(MCGuiDone)> async;
    double timeout = 0;
};

std::vector<std::unique_ptr<Item>> gItems;
size_t gIndex = 0;
Item* gWaiting = nullptr;
double gDeadlineMs = 0;
bool gAsyncDone = false;
int gPass = 0, gFail = 0, gSuitePass = 0, gSuiteFail = 0, gSlow = 0, gLeaks = 0;
std::string gSuite, gStep;
// What failed, by suite, for the wrapper's retry (tests/GuiRetry.h);
// gOutside while the end's checks or the run's own limit fail.
GuiRetry::Results gResults;
bool gOutside = false;
double gStartedMs = 0;
double gStepSeconds = 60;

// Read by the watchdog thread. The names point into gItems, which lives as
// long as the process does.
std::atomic<double> gItemDeadlineMs{0};
std::atomic<const char*> gItemName{"starting"};
std::atomic<const char*> gSuiteName{""};

Item* enqueue(Kind kind, const std::string& name, double timeout) {
    gItems.push_back(std::make_unique<Item>());
    Item* i = gItems.back().get();
    i->kind = kind;
    i->name = name;
    i->timeout = timeout;
    return i;
}
// A factor of at least 1 from the environment (CI sets these; Harness.h).
double factor(const char* name) {
    const char* s = getenv(name);
    const double v = s ? atof(s) : 0;
    return v > 1 ? v : 1;
}
}  // namespace

double MCGuiWaitScale() { return factor("MC_GUI_WAIT_SCALE"); }
double MCGuiTimingScale() { return factor("MC_TIMING_SCALE"); }

void MCGuiStep(const std::string& name, std::function<void()> block) {
    enqueue(Kind::Step, name, 0)->block = std::move(block);
}

void MCGuiWait(const std::string& what, double timeout, std::function<bool()> until) {
    enqueue(Kind::Wait, what, timeout * MCGuiWaitScale())->until = std::move(until);
}

void MCGuiAsync(const std::string& name, double timeout, std::function<void(MCGuiDone)> block) {
    enqueue(Kind::Async, name, timeout * MCGuiWaitScale())->async = std::move(block);
}

void MCGuiPause(double seconds) {
    // Timed from when the pause is reached, not from when the suite queued it.
    auto until = std::make_shared<double>(0);
    char name[64];
    snprintf(name, sizeof name, "a pause of %.1f s", seconds);
    MCGuiWait(name, seconds + 5, [until, seconds] {
        if (*until == 0) *until = MCGuiNowMs() + seconds * 1000;
        return MCGuiNowMs() >= *until;
    });
}

void MCGuiWaitQuiet(const std::string& what, double quiet, double timeout,
                    std::function<long()> value) {
    auto last = std::make_shared<long>(0);
    auto since = std::make_shared<double>(0);
    MCGuiWait(what, timeout, [=] {
        const long v = value();
        if (*since == 0 || v != *last) {
            *last = v;
            *since = MCGuiNowMs();
            return false;
        }
        return MCGuiNowMs() - *since >= quiet * 1000;
    });
}

void MCGuiWaitCpu(const std::string& what, double cpuSeconds, double wallSeconds,
                  std::function<bool()> until) {
    auto cpu0 = std::make_shared<double>(-1);
    // A stretch of the clock and the CPU the thread spent in it: a minute in
    // which it spent almost none is a thread at rest, its work over and the
    // condition not come (large_file's place settled in the wrong spot), and
    // fails then, not at `wallSeconds` (900 s there, 22 minutes at CI's
    // wait scale, half the GTK job's limit). Work starved by a loaded machine
    // still gets far more than this: a ninth of a core is 6 s a minute.
    auto stretchAt = std::make_shared<double>(0), stretchCpu = std::make_shared<double>(0);
    const double budgetMs = cpuSeconds * 1000 * MCGuiWaitScale();
    const double stretchMs = 60 * 1000 * MCGuiWaitScale(), restMs = 300;
    MCGuiWait(what, wallSeconds, [=] {
        const double cpu = MCGuiThreadCpuMs(), now = MCGuiNowMs();
        if (*cpu0 < 0) *cpu0 = *stretchCpu = cpu, *stretchAt = now;
        if (until()) return true;
        char text[256];
        if (now - *stretchAt >= stretchMs) {
            if (cpu - *stretchCpu < restMs) {
                snprintf(text, sizeof text, "the main thread at rest for %.0f s (%.0f ms of CPU) without: %s",
                         stretchMs / 1000, cpu - *stretchCpu, what.c_str());
                MCGuiCheck(false, text, __FILE__, __LINE__);
                return true;
            }
            *stretchAt = now, *stretchCpu = cpu;
        }
        if (cpu - *cpu0 < budgetMs) return false;
        snprintf(text, sizeof text, "%.0f s of the main thread's CPU without: %s", budgetMs / 1000,
                 what.c_str());
        MCGuiCheck(false, text, __FILE__, __LINE__);
        return true;
    });
}

// ---------------------------------------------------------------- checks
bool MCGuiCheck(bool ok, const std::string& what, const char* file, int line) {
    if (ok) {
        gPass++;
        gSuitePass++;
        gResults.passed++;
    } else {
        gFail++;
        gSuiteFail++;
        const char* slash = strrchr(file, '/');
        printf("  FAIL [%s] %s:%d  %s  (in: %s)\n", gSuite.c_str(), slash ? slash + 1 : file, line,
               what.c_str(), gStep.c_str());
        GuiRetry::noteFailure(gResults, gOutside ? "" : gSuite,
                              std::string(slash ? slash + 1 : file) + ":" + std::to_string(line) + "  " +
                                  what + "  (in: " + gStep + ")");
        fflush(stdout);
        // When pictures are kept, one of the window on top as it was at the
        // failure (CI keeps them when the job fails). The first few only: a
        // run that goes wrong early can fail every check after.
        const char* dir = getenv("MC_GUI_SNAPSHOTS");
        if (dir && *dir && gFail <= 10) {
            const std::string name = "fail-" + std::to_string(gFail) + "-" + gSuite;
            MCGuiSnapshot(GTK_WIDGET(MCGuiTopWindow()), name);
            printf("    a picture of the window: %s/%s.png\n", dir, name.c_str());
            fflush(stdout);
        }
    }
    return ok;
}

bool MCGuiCheckEqual(const std::string& got, const std::string& want, const char* a, const char* b,
                     const char* file, int line) {
    return MCGuiCheck(got == want,
                      std::string(a) + " == " + b + " (got \"" + got + "\", wanted \"" + want + "\")",
                      file, line);
}

void MCGuiNote(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    char* text = g_strdup_vprintf(format, ap);
    va_end(ap);
    printf("    %s\n", text);
    fflush(stdout);
    g_free(text);
}

// GLib's log, watched (Harness.h). A writer may run on any thread.
namespace {
std::mutex gLogMutex;
bool gLogWatching = false;
std::vector<std::string> gLogKept;

std::string logField(const GLogField& f) {
    if (!f.value) return "";
    return f.length < 0 ? std::string((const char*)f.value) : std::string((const char*)f.value, (size_t)f.length);
}

GLogWriterOutput logWriter(GLogLevelFlags level, const GLogField* fields, gsize n, gpointer data) {
    if (level & (G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL)) {
        std::lock_guard<std::mutex> lock(gLogMutex);
        if (gLogWatching) {
            std::string domain = "(no domain)", message;
            for (gsize i = 0; i < n; i++) {
                if (!strcmp(fields[i].key, "GLIB_DOMAIN")) domain = logField(fields[i]);
                else if (!strcmp(fields[i].key, "MESSAGE")) message = logField(fields[i]);
            }
            gLogKept.push_back(domain + ((level & G_LOG_LEVEL_CRITICAL) ? "-CRITICAL: " : "-WARNING: ") + message);
        }
    }
    return g_log_writer_default(level, fields, n, data);
}
}  // namespace

void MCGuiLogWatchStart() {
    static bool installed = false;
    if (!installed) {
        installed = true;
        g_log_set_writer_func(logWriter, nullptr, nullptr);
    }
    std::lock_guard<std::mutex> lock(gLogMutex);
    gLogKept.clear();
    gLogWatching = true;
}

std::vector<std::string> MCGuiLogWatchStop() {
    std::lock_guard<std::mutex> lock(gLogMutex);
    gLogWatching = false;
    std::vector<std::string> kept;
    kept.swap(gLogKept);
    return kept;
}

double MCGuiThreadCpuMs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

double MCGuiNowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

bool MCGuiTimingSkipped() {
    const char* s = getenv("MC_SKIP_TIMING");
    return s && *s && strcmp(s, "0") != 0;
}

// Every timing check's share of its bound, for the end of the run's list of
// those closest to failing.
namespace {
struct Timed {
    double ratio;
    std::string line;
};
std::vector<Timed> gTimed;

void printNearestBounds() {
    if (gTimed.empty()) return;
    std::sort(gTimed.begin(), gTimed.end(), [](const Timed& a, const Timed& b) { return a.ratio > b.ratio; });
    printf("Timing checks closest to their bounds (CPU time over the bound):\n");
    for (size_t i = 0; i < gTimed.size() && i < 10; i++) printf("%s\n", gTimed[i].line.c_str());
}
}  // namespace

bool MCGuiCheckTime(double ms, double bound, const std::string& what, const char* file, int line) {
    if (bound > 0) {
        const char* slash = strrchr(file, '/');
        char buf[512];
        snprintf(buf, sizeof buf, "  NEAR [%s] %s:%d  %s: %.0f%% (%.2f of %.0f ms)", gSuite.c_str(),
                 slash ? slash + 1 : file, line, what.c_str(), 100 * ms / bound, ms, bound);
        gTimed.push_back({ms / bound, buf});
    }
    const double scale = MCGuiTimingScale();
    if (!MCGuiTimingSkipped() && ms >= bound && ms < bound * scale) {
        // Over the bound, within the room this runner is given: a pass, said
        // loudly, so a bound being outgrown shows in the job's summary first.
        gSlow++;
        const char* slash = strrchr(file, '/');
        printf("  SLOW [%s] %s:%d  %s: %.2f ms of CPU, over its bound of %.0f (passed within "
               "this runner's x%.2g)\n",
               gSuite.c_str(), slash ? slash + 1 : file, line, what.c_str(), ms, bound, scale);
        fflush(stdout);
    }
    char text[96];
    if (scale > 1)
        snprintf(text, sizeof text, " under %.0f ms of CPU, x%.2g on this runner (%.2f)", bound, scale, ms);
    else
        snprintf(text, sizeof text, " under %.0f ms of CPU (%.2f)", bound, ms);
    return MCGuiCheck(MCGuiTimingSkipped() || ms < bound * scale, what + text, file, line);
}

// ----------------------------------------------------------------- world
static std::string envOr(const char* name, const std::string& fallback = "") {
    const char* v = getenv(name);
    return v && *v ? v : fallback;
}

std::string MCGuiScratch() { return envOr("MC_GUI_SCRATCH"); }
std::string MCGuiProject() { return MCGuiScratch() + "/project"; }
std::string MCGuiProjectFile(const std::string& name) { return MCGuiProject() + "/" + name; }

static std::string selfPath() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "";
    buf[n] = 0;
    return buf;
}

std::string MCGuiBuildDir() {
    const std::string self = selfPath();
    const size_t slash = self.rfind('/');
    return slash == std::string::npos ? "." : self.substr(0, slash);
}

void MCGuiWriteFile(const std::string& path, const std::string& text) {
    char* dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    g_file_set_contents(path.c_str(), text.c_str(), (gssize)text.size(), nullptr);
}

std::string MCGuiReadFile(const std::string& path) {
    gchar* contents = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(path.c_str(), &contents, &len, nullptr)) return "";
    std::string out(contents, len);
    g_free(contents);
    return out;
}

bool MCGuiFileExists(const std::string& path) {
    return g_file_test(path.c_str(), G_FILE_TEST_EXISTS);
}

static std::vector<std::string>& launchedUris() {
    static std::vector<std::string>* all = new std::vector<std::string>();
    return *all;
}
std::vector<std::string> MCGuiLaunchedUris() { return launchedUris(); }

void MCGuiSetSettings(const std::string& text) {
    MCGuiWriteFile(envOr("MUTIBLE_SETTINGS"), text);
    MCGuiReloadSettings();
}

void MCGuiSetKeys(const std::string& text) {
    MCGuiWriteFile(envOr("MUTIBLE_KEYS"), text);
    MCGuiReloadKeys();
}

std::string MCGuiEditorText() {
    Editor* editor = MCGuiApp().editor;
    if (!editor) return "";
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(editor->buffer(), &start, &end);
    gchar* text = gtk_text_buffer_get_text(editor->buffer(), &start, &end, TRUE);
    std::string out = text;
    g_free(text);
    return out;
}

void MCGuiSnapshot(GtkWidget* widget, const std::string& name) {
    const std::string dir = envOr("MC_GUI_SNAPSHOTS");
    if (dir.empty() || !widget) return;
    const int w = gtk_widget_get_width(widget), h = gtk_widget_get_height(widget);
    if (w <= 0 || h <= 0) return;
    g_mkdir_with_parents(dir.c_str(), 0755);
    GdkPaintable* paintable = gtk_widget_paintable_new(widget);
    GtkSnapshot* snap = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snap, w, h);
    GskRenderNode* node = gtk_snapshot_free_to_node(snap);
    GskRenderer* renderer = gtk_native_get_renderer(gtk_widget_get_native(widget));
    if (node && renderer) {
        GdkTexture* texture = gsk_renderer_render_texture(renderer, node, nullptr);
        gdk_texture_save_to_png(texture, (dir + "/" + name + ".png").c_str());
        g_object_unref(texture);
    }
    if (node) gsk_render_node_unref(node);
    g_object_unref(paintable);
}

// The color drawn at a point of a widget (red, green, blue, alpha; 0..255), rendered as the
// snapshot is; false when the widget has no size or renderer yet.
bool MCGuiPixel(GtkWidget* widget, int x, int y, int rgb[4]) {
    if (!widget) return false;
    const int w = gtk_widget_get_width(widget), h = gtk_widget_get_height(widget);
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x >= w || y >= h) return false;
    GskRenderer* renderer = gtk_native_get_renderer(gtk_widget_get_native(widget));
    if (!renderer) return false;
    GdkPaintable* paintable = gtk_widget_paintable_new(widget);
    GtkSnapshot* snap = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snap, w, h);
    GskRenderNode* node = gtk_snapshot_free_to_node(snap);
    bool ok = false;
    if (node) {
        GdkTexture* texture = gsk_renderer_render_texture(renderer, node, nullptr);
        if (texture) {
            const int tw = gdk_texture_get_width(texture), th = gdk_texture_get_height(texture);
            std::vector<guchar> data((size_t)tw * th * 4);
            gdk_texture_download(texture, data.data(), (size_t)tw * 4);  // B8G8R8A8 premultiplied
            const guchar* p = &data[((size_t)y * th / h * tw + (size_t)x * tw / w) * 4];
            rgb[0] = p[2];
            rgb[1] = p[1];
            rgb[2] = p[0];
            rgb[3] = p[3];
            ok = true;
            g_object_unref(texture);
        }
        gsk_render_node_unref(node);
    }
    g_object_unref(paintable);
    return ok;
}

// --------------------------------------------------------- finding widgets
bool MCGuiOnScreen(GtkWidget* widget) {
    if (!widget || !gtk_widget_get_mapped(widget) || gtk_widget_get_width(widget) <= 0 ||
        gtk_widget_get_height(widget) <= 0)
        return false;
    // What a click at its center would reach: GTK's own pick, which knows
    // what is clipped (a row scrolled just out of a list stays mapped) and
    // what is on top.
    GtkNative* native = gtk_widget_get_native(widget);
    if (!native) return false;
    graphene_point_t center = GRAPHENE_POINT_INIT(gtk_widget_get_width(widget) / 2.0f,
                                                  gtk_widget_get_height(widget) / 2.0f);
    graphene_point_t at;
    if (!gtk_widget_compute_point(widget, GTK_WIDGET(native), &center, &at)) return false;
    GtkWidget* hit = gtk_widget_pick(GTK_WIDGET(native), at.x, at.y, GTK_PICK_DEFAULT);
    for (GtkWidget* w = hit; w; w = gtk_widget_get_parent(w))
        if (w == widget) return true;   // the widget, or a part of it
    // A widget that takes no input itself is reached through what holds it.
    int up = 0;
    for (GtkWidget* w = widget; w && up <= 3; w = gtk_widget_get_parent(w), up++)
        if (w == hit) return true;
    return false;
}

GtkWidget* MCGuiFindLabel(GtkWidget* root, const std::string& text) {
    if (!root || !gtk_widget_get_mapped(root)) return nullptr;
    if (GTK_IS_LABEL(root) && text == gtk_label_get_text(GTK_LABEL(root)))
        return MCGuiOnScreen(root) ? root : nullptr;
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = MCGuiFindLabel(c, text)) return hit;
    return nullptr;
}

GtkWidget* MCGuiFindButton(GtkWidget* root, const std::string& text) {
    if (!root || !gtk_widget_get_mapped(root)) return nullptr;
    if (GTK_IS_BUTTON(root)) {
        const char* label = gtk_button_get_label(GTK_BUTTON(root));
        if ((label && text == label) || MCGuiFindLabel(root, text))
            return MCGuiOnScreen(root) ? root : nullptr;
    }
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = MCGuiFindButton(c, text)) return hit;
    return nullptr;
}

namespace {
GtkWidget* treeLabelOf(GtkWidget* root, const std::string& path) {
    if (!root || !gtk_widget_get_mapped(root)) return nullptr;
    if (GTK_IS_LABEL(root)) {
        const char* p = static_cast<const char*>(g_object_get_data(G_OBJECT(root), "minicode-path"));
        return p && path == p && MCGuiOnScreen(root) ? root : nullptr;
    }
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = treeLabelOf(c, path)) return hit;
    return nullptr;
}
}  // namespace

GtkWidget* MCGuiTreeRow(const std::string& path) { return MCGuiTreeRow(MCGuiApp().tree, path); }

GtkWidget* MCGuiTreeRow(FileTree* tree, const std::string& path) {
    return tree ? treeLabelOf(tree->widget(), path) : nullptr;
}

void MCGuiBringTreeRow(const std::string& path, double timeout) {
    const std::string name = path.substr(path.find_last_of('/') + 1);
    MCGuiStep(name + "'s row brought into view in the tree", [path] {
        MCGuiApp().tree->revealPath(path);
    });
    // When it was last asked again, where the row was, and since when.
    auto asked = std::make_shared<double>(MCGuiNowMs());
    auto lastY = std::make_shared<double>(-1);
    auto since = std::make_shared<double>(0);
    MCGuiWait(name + "'s row on screen, standing still", timeout, [path, asked, lastY, since] {
        GtkWidget* row = MCGuiTreeRow(path);
        graphene_point_t in = GRAPHENE_POINT_INIT(0, 0), at;
        if (!row || !gtk_widget_compute_point(row, MCGuiApp().tree->widget(), &in, &at)) {
            // revealPath gives up after a couple of seconds; a file written
            // in the step before may still be on its way from the monitor.
            if (MCGuiNowMs() - *asked > 1000) {
                *asked = MCGuiNowMs();
                MCGuiApp().tree->revealPath(path);
            }
            *since = 0;
            return false;
        }
        if (*since == 0 || at.y != *lastY) {
            *lastY = at.y;
            *since = MCGuiNowMs();
            return false;
        }
        return MCGuiNowMs() - *since >= 300;
    });
}

void MCGuiHideBrowserPane() {
    MCGuiStep("the browser pane hidden, if an earlier suite left it shown", [] {
        // The window action, as View > Toggle Browser: a key could go to the
        // page or the terminal, wherever the keyboard was left.
        if (MCGuiApp().layout && MCGuiApp().layout->browserShown())
            g_action_group_activate_action(G_ACTION_GROUP(MCGuiApp().window), "togglebrowser", nullptr);
    });
    MCGuiWait("the browser pane hidden", 10, [] {
        return !MCGuiApp().layout || !MCGuiApp().layout->browserShown();
    });
}

namespace {
const char* entryText(GtkWidget* entry) { return entry ? gtk_editable_get_text(GTK_EDITABLE(entry)) : ""; }
bool toggleOn(GtkWidget* t) { return t && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(t)); }

// What a window's Find in Folder holds that a new window's does not, or "".
std::string folderSearchChanges(const MCGuiAppWindow& a) {
    SearchPanel* s = a.search;
    if (!s) return "";
    std::string out;
    auto add = [&out](const std::string& part) { out += (out.empty() ? "" : ", ") + part; };
    if (s->scope() != a.rootDir) add("the scope " + s->scope());
    if (*entryText(s->includeEntry()))
        add(std::string("files to include \"") + entryText(s->includeEntry()) + "\"");
    if (*entryText(s->excludeEntry()))
        add(std::string("files to exclude \"") + entryText(s->excludeEntry()) + "\"");
    if (toggleOn(s->caseButton())) add("Match Case");
    if (toggleOn(s->wordButton())) add("Match Whole Word");
    if (toggleOn(s->regexButton())) add("Use Regular Expression");
    return out;
}
}  // namespace

void MCGuiResetFolderSearch() {
    for (const MCGuiAppWindow& a : MCGuiAppWindows()) {
        SearchPanel* s = a.search;
        if (!s) continue;
        gtk_editable_set_text(GTK_EDITABLE(s->queryEntry()), "");
        // Only one that holds something: each change searches again 350 ms
        // later, with whatever query is in the box by then.
        for (GtkWidget* e : {s->includeEntry(), s->excludeEntry()})
            if (*entryText(e)) gtk_editable_set_text(GTK_EDITABLE(e), "");
        for (GtkWidget* t : {s->caseButton(), s->wordButton(), s->regexButton()})
            if (toggleOn(t)) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t), FALSE);
        s->setScope(a.rootDir);
    }
}

namespace {
// What a command printed, run synchronously with no shell; "" when it failed.
std::string commandOutput(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    gchar* out = nullptr;
    gint status = 0;
    const bool ran = g_spawn_sync(nullptr, argv.data(), nullptr,
                                  (GSpawnFlags)(G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL),
                                  nullptr, nullptr, &out, nullptr, &status, nullptr);
    std::string text = ran && g_spawn_check_wait_status(status, nullptr) && out ? out : "";
    g_free(out);
    return text;
}
}  // namespace

bool MCGuiProjectIsRepository() { return MCGuiFileExists(MCGuiProject() + "/.git"); }

void MCGuiRemoveProjectRepository() {
    char buf[PATH_MAX];
    const std::string proj = realpath(MCGuiProject().c_str(), buf) ? buf : MCGuiProject();
    std::istringstream in(commandOutput({"git", "-C", proj, "worktree", "list", "--porcelain"}));
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("worktree ", 0) != 0) continue;
        const std::string wt = line.substr(9);
        if (wt.rfind(proj + "/", 0) == 0) commandOutput({"rm", "-rf", "--", wt});
    }
    commandOutput({"rm", "-rf", "--", proj + "/.git"});
}

#ifdef MUTIBLE_ENABLE_TERMINAL
namespace {
// Ctrl+U to a terminal's shell, as a key typed there.
void sendCtrlU(Terminal* t) {
    std::vector<GtkWidget*> todo;
    if (t) todo.push_back(t->widget());
    while (!todo.empty()) {
        GtkWidget* w = todo.back();
        todo.pop_back();
        if (VTE_IS_TERMINAL(w)) {
            vte_terminal_feed_child(VTE_TERMINAL(w), "\x15", 1);
            return;
        }
        for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
            todo.push_back(c);
    }
}

// Every terminal of every app window.
std::vector<Terminal*> allTerminals() {
    std::vector<Terminal*> out;
    for (const MCGuiAppWindow& a : MCGuiAppWindows())
        for (int i = 0; a.termDock && i < a.termDock->count(); i++)
            if (TerminalGroup* g = a.termDock->group(i))
                for (Terminal* t : g->panes()) out.push_back(t);
    return out;
}
}  // namespace
#endif

void MCGuiClearTerminalLine(const std::string& typed) {
#ifdef MUTIBLE_ENABLE_TERMINAL
    MCGuiStep("the line typed at the shell's prompt taken back (Ctrl+U)", [] {
        sendCtrlU(MCGuiApp().terminal);
    });
    if (typed.empty()) return;
    MCGuiWait("\"" + typed + "\" gone from the shell's prompt", 10, [typed] {
        Terminal* t = MCGuiApp().terminal;
        return !t || t->recentText(1).find(typed) == std::string::npos;
    });
#else
    (void)typed;
#endif
}

bool MCGuiTextLaidOut(GtkWidget* textView) {
    if (!textView || !GTK_IS_TEXT_VIEW(textView)) return false;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(gtk_text_view_get_buffer(GTK_TEXT_VIEW(textView)), &end);
    int y = 0, h = 0;
    gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(textView), &end, &y, &h);
    return h > 0;
}

GtkWindow* MCGuiTopWindow() {
    std::set<GtkWindow*> apps;
    for (const MCGuiAppWindow& w : MCGuiAppWindows()) apps.insert(w.window);
    GList* all = gtk_window_list_toplevels();
    GtkWindow* found = nullptr;
    for (GList* l = all; l && !found; l = l->next) {
        GtkWindow* w = GTK_WINDOW(l->data);
        if (gtk_widget_get_mapped(GTK_WIDGET(w)) && !apps.count(w)) found = w;
    }
    g_list_free(all);
    return found ? found : MCGuiApp().window;
}

// ------------------------------------------------------------- processes
namespace {
struct ProcessRun {
    std::function<void(int, std::string, std::string)> done;
};

std::string bytesText(GBytes* b) {
    if (!b) return "";
    gsize n = 0;
    const char* p = static_cast<const char*>(g_bytes_get_data(b, &n));
    return std::string(p ? p : "", n);
}
}  // namespace

void MCGuiRunProcess(const std::vector<std::string>& argv,
                     const std::map<std::string, std::string>& env,
                     std::function<void(int, std::string, std::string)> done,
                     const std::string& cwd) {
    GSubprocessLauncher* launcher = g_subprocess_launcher_new(
        (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE));
    g_subprocess_launcher_set_cwd(launcher, cwd.empty() ? MCGuiProject().c_str() : cwd.c_str());
    for (const auto& kv : env) g_subprocess_launcher_setenv(launcher, kv.first.c_str(), kv.second.c_str(), TRUE);
    std::vector<const char*> c;
    for (const std::string& a : argv) c.push_back(a.c_str());
    c.push_back(nullptr);
    GError* error = nullptr;
    GSubprocess* proc = g_subprocess_launcher_spawnv(launcher, c.data(), &error);
    g_object_unref(launcher);
    if (!proc) {
        std::string why = error ? error->message : "could not start";
        g_clear_error(&error);
        done(-1, "", why);
        return;
    }
    auto* run = new ProcessRun{std::move(done)};
    g_subprocess_communicate_async(proc, nullptr, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer data) {
            auto* r = static_cast<ProcessRun*>(data);
            GSubprocess* p = G_SUBPROCESS(src);
            GBytes* out = nullptr;
            GBytes* err = nullptr;
            g_subprocess_communicate_finish(p, res, &out, &err, nullptr);
            int status = -1;
            if (g_subprocess_get_if_exited(p)) status = g_subprocess_get_exit_status(p);
            else if (g_subprocess_get_if_signaled(p)) status = 128 + g_subprocess_get_term_sig(p);
            r->done(status, bytesText(out), bytesText(err));
            if (out) g_bytes_unref(out);
            if (err) g_bytes_unref(err);
            delete r;
            g_object_unref(p);
        }, run);
}

void MCGuiRunChild(const std::string& suite, const std::string& folder,
                   std::function<void(int, std::string)> done) {
    gchar* dbusRun = g_find_program_in_path("dbus-run-session");
    const std::string runner = dbusRun ? dbusRun : "dbus-run-session";
    g_free(dbusRun);
    const std::string config = "--config-file=" + MCGuiScratch() + "/dbus.conf";
    const std::string daemon = "--dbus-daemon=" + selfPath();
    // No folder: launched as the dock launches it, with no path and the
    // home folder as its working folder.
    std::vector<std::string> argv = {runner, config, daemon, "--", selfPath()};
    if (!folder.empty()) argv.push_back(folder);
    MCGuiRunProcess(argv, {{"MC_GUI_SUITES", suite}, {"MC_GUI_RESULTS", ""}, {"MC_GUI_SHUFFLE", ""}},
                    [done](int status, std::string out, std::string err) {
                        // The child's windows have gone. With no window
                        // manager the X focus stays on the last of them, and
                        // no app window is active (the harness's LEFT line
                        // after startup and sessions); a desktop gives it to
                        // the window left, and so does this.
                        MCGuiFocusKeyWindow();
                        done(status, out + err);
                    },
                    folder.empty() ? std::string(g_get_home_dir()) : std::string());
}

// ---------------------------------------------------------------- runner
namespace {

void finish();
gboolean tick(gpointer);

void schedule(guint ms) { g_timeout_add(ms, tick, nullptr); }

void endSuite() {
    if (!gSuite.empty()) printf("%s: %d passed, %d failed\n", gSuite.c_str(), gSuitePass, gSuiteFail);
    fflush(stdout);
}

void startItem(Item* item) {
    gItemName = item->name.c_str();
    gItemDeadlineMs = MCGuiNowMs() + (item->timeout + gStepSeconds) * 1000;
}

void runGuarded(const std::string& what, const std::function<void()>& body) {
    try {
        body();
    } catch (const std::exception& e) {
        MCGuiCheck(false, what + " threw: " + e.what(), __FILE__, __LINE__);
    }
}

// After a wait that timed out, what the window was doing, so that a failure
// on CI says more than the wait's name: whether the key window is active and
// where its keyboard is, whether another window is on top, and the editor's
// file, selection, scroll and first characters. (In November 2026 they showed
// a tab clean with its own edits after Reload, which led to auto-save writing
// while a question was up.)
void noteState() {
    GtkWindow* kw = MCGuiKeyWindow();
    GtkWidget* f = kw ? gtk_root_get_focus(GTK_ROOT(kw)) : nullptr;
    // The widget with the keyboard and the style classes of it and its
    // ancestors, which name the pane ("minicode-editor-scroller", ...).
    std::string where = f ? G_OBJECT_TYPE_NAME(f) : "nothing";
    for (GtkWidget* w = f; w; w = gtk_widget_get_parent(w)) {
        char** classes = gtk_widget_get_css_classes(w);
        for (char** c = classes; c && *c; c++)
            if (g_str_has_prefix(*c, "minicode-")) where += std::string(" .") + *c;   // the CSS classes keep their names (rename-mutible: keep)
        g_strfreev(classes);
    }
    MCGuiNote("state: the key window %s, the keyboard in a %s, %s on top",
              kw && gtk_window_is_active(kw) ? "active" : "not active", where.c_str(),
              MCGuiTopWindow() == MCGuiApp().window ? "the app's window" : "another window");
    Editor* ed = MCGuiApp().editor;
    if (!ed) return;
    GtkTextIter a, b;
    gtk_text_buffer_get_selection_bounds(ed->buffer(), &a, &b);
    GtkAdjustment* v = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(ed->textView()));
    const std::string text = MCGuiEditorText();
    MCGuiNote("state: the editor on %s%s, the selection %d to %d, scrolled to %.0f, text \"%.60s\"",
              ed->currentPath().empty() ? "(no file)" : ed->currentPath().c_str(),
              ed->dirty() ? " (unsaved)" : "", gtk_text_iter_get_offset(&a),
              gtk_text_iter_get_offset(&b), gtk_adjustment_get_value(v), text.c_str());
}

// ------------------------------------------------ what a suite leaves behind
// A suite starts from what the one before left, so what one leaves fails
// later suites that never touched it (HANDOFF.md: a terminal's width after
// zoom, the editor's font, a row menu left open; on the Mac, a key window
// left pinned failed five suites). The world is looked at as each suite
// starts and again as it ends; a difference is a LEAK line naming the suite,
// a failed check of that suite, and put back where the harness can, so the
// suites after it start clean. MC_GUI_LEAKS=note prints without failing or
// putting anything back.
struct World {
    size_t windows = 0;
    int terminals = 0;
    bool closeAsks = true;   // every window asks before closing a busy terminal
    bool otherOnTop = false;
    bool active = true;   // the app's window has the X focus
    int waiting = 0;   // terminals marked waiting for the user (TerminalAttention)
    int busy = 0;      // terminals where a program other than the shell has the foreground
    std::string settings, keys;
    std::string folderSearch;   // what Find in Folder keeps that a new window's has not
    bool projectRepository = false;   // the project folder has a .git
    int browsers = 0;   // windows with the browser pane on show
    int agentPages = 0;   // windows whose browser has an agent page open
};
World gWorldAtStart;

World lookAtWorld() {
    World w;
    const std::vector<MCGuiAppWindow> all = MCGuiAppWindows();
    w.windows = all.size();
#ifdef MUTIBLE_ENABLE_TERMINAL
    for (const MCGuiAppWindow& a : all)
        if (a.termDock) {
            w.terminals += a.termDock->count();
            if (!a.termDock->confirmClose) w.closeAsks = false;
        }
#endif
    w.otherOnTop = MCGuiTopWindow() != MCGuiApp().window;
    w.active = MCGuiApp().window && gtk_window_is_active(MCGuiApp().window);
    w.waiting = TerminalAttention::waitingCount();
#ifdef MUTIBLE_ENABLE_TERMINAL
    for (Terminal* t : allTerminals())
        if (t->busy()) w.busy++;
#endif
    w.settings = MCGuiReadFile(envOr("MUTIBLE_SETTINGS"));
    w.keys = MCGuiReadFile(envOr("MUTIBLE_KEYS"));
    for (const MCGuiAppWindow& a : all) {
        const std::string changes = folderSearchChanges(a);
        if (!changes.empty()) w.folderSearch += (w.folderSearch.empty() ? "" : "; ") + changes;
    }
    w.projectRepository = MCGuiProjectIsRepository();
    for (const MCGuiAppWindow& a : all)
        if (a.layout && a.layout->browserShown()) w.browsers++;
#ifdef MUTIBLE_ENABLE_BROWSER
    for (const MCGuiAppWindow& a : all)
        if (a.browser && a.browser->hasAgentPage()) w.agentPages++;
#endif
    return w;
}

bool leaksFail() { return envOr("MC_GUI_LEAKS") != "note"; }

std::string shortText(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (out.size() >= 120) return out + "...";
        out += c == '\n' ? std::string("\\n") : std::string(1, c);
    }
    return out;
}

// A LEAK fails the suite (what a later suite trips over: a pin, a window, a
// question left on top); a LEFT line only warns (a file the harness puts
// back itself, a terminal tab left open).
void leak(const std::string& what, bool fails = true) {
    if (fails) gLeaks++;
    printf("  %s [%s] %s\n", fails ? "LEAK" : "LEFT", gSuite.c_str(), what.c_str());
    fflush(stdout);
    if (fails && leaksFail()) {
        gFail++;
        gSuiteFail++;
        GuiRetry::noteFailure(gResults, gSuite, "LEAK " + what);
    }
}

// The session's host (SessionLink.h, SESSION-DESIGN.md stage 4a) owns the
// documents' text, and each buffer is its mirror: once everything sent has
// been answered, the host's text of every document a window has read is
// what the buffer holds, its tabs are the window's, and it reported nothing
// wrong. Checked as every suite ends, so a suite that left them apart
// fails by name (under MUTIBLE_HOST=loopback too, every message through
// the protocol's encoding).
void checkSessionMirrors() {
    for (const MCGuiAppWindow& a : MCGuiAppWindows()) {
        if (!a.editor) continue;
        // Each document against the host it was opened on (its window's
        // folder's, SessionLink.h), and the window's tabs against its own.
        std::map<SessionLink::Link*, std::vector<std::pair<uint64_t, std::string>>> docs;
        for (Document* d : a.editor->documents()) {
            if (d->kind() != Document::Kind::Text || d->loading() || d->unread()) continue;
            GtkTextIter s, e;
            gtk_text_buffer_get_bounds(d->buffer(), &s, &e);
            char* t = gtk_text_buffer_get_text(d->buffer(), &s, &e, TRUE);
            docs[d->sessionLink()].emplace_back(d->id(), t ? t : "");
            g_free(t);
        }
        std::string why;
        for (const auto& [link, list] : docs) why += link->checkDocuments(list);
        if (SessionLink::Link* link = a.editor->sessionLink())
            why += link->checkWindow(a.editor->sessionWindowId(), a.editor->window());
        if (!why.empty())
            MCGuiCheck(false, ("the session's host and the window it renders differ: " + why).c_str(),
                       __FILE__, __LINE__);
    }
}

void checkWhatSuiteLeft() {
    // A "_" suite runs alone in a process of its own, which it may end.
    if (gSuite.empty() || gSuite[0] == '_') return;
    checkSessionMirrors();
    const bool putBack = leaksFail();
    // What windows that closed left on their folders' hosts (stage 6, step
    // 2: closing a window detaches) is the harness's to end between suites,
    // as the two files are, so a suite's detached terminals and documents
    // never come back in a later suite's window on the same folder.
    if (putBack) SessionLink::endDetached();
    if (MCGuiPinnedKeyWindow() && MCGuiPinnedKeyWindow() != MCGuiApp().window) {
        leak("left the key window pinned to a window still open (MCGuiSetKeyWindow(nullptr) at its end)");
        if (putBack) MCGuiSetKeyWindow(nullptr);
    }
    const World now = lookAtWorld(), was = gWorldAtStart;
    if (now.windows != was.windows)
        leak("left " + std::to_string(now.windows) + " app window(s) where it found " +
             std::to_string(was.windows));
    if (now.otherOnTop && !was.otherOnTop) {
        GtkWindow* top = MCGuiTopWindow();
        const char* title = gtk_window_get_title(top);
        leak(std::string("left another window on top of the app's: \"") + (title ? title : "") + "\"");
        // Closed, as Esc would: a modal question left up holds the app's
        // keys, so every suite after it failed (claude_hooks_offer's, before
        // idelink, browser_pages and agents, 2026-11-18).
        if (putBack && top && top != MCGuiApp().window) {
            gtk_window_close(top);
            MCGuiFocusKeyWindow();
        }
    }
    // A suite may turn the question off while it closes terminals, but must
    // put it back: terminal_ids did not, and a busy terminal closed unasked in
    // processes, after it in the soak's shuffled order.
    if (was.closeAsks && !now.closeAsks)
        leak("left a window's question before closing a busy terminal off (TerminalDock::confirmClose)");
    // A drop from one of the app's own drags (tree_drags') leaves GDK's user
    // time days ahead of the X server's with GTK 4.22 (Input.cpp), and every
    // window the app presents after it comes up without the keyboard:
    // preview_tabs' and replace_in_folder's, after tree_drags. GTK's, not
    // the suite's; said, not failed.
    if (MCGuiUserTimeAhead()) {
        leak("left GDK's user time ahead of the X server's (a drop from the app's own drag), put back",
             false);
        if (putBack) MCGuiPutBackUserTime();
    }
    // With no window manager the X focus stays on a window that closed (a
    // question, a second window) and no app window is active, so a click
    // gives the keyboard nothing: tabs' first click in the text, after such
    // a suite in a shuffled order. A desktop gives the focus to the window
    // left, and the harness does too; said, not failed.
    if (!now.active && !now.otherOnTop) {
        if (was.active)
            leak("left the app's window inactive (the X focus on a window it closed), given back",
                 false);
        if (putBack) MCGuiFocusKeyWindow();
    }
    // A terminal marked waiting puts "(1 waiting)" before every window's
    // title: agents marked one and never cleared it, and tabs, after it in
    // the soak's shuffled order, read the title.
#ifdef MUTIBLE_ENABLE_TERMINAL
    // A program left running in a terminal takes what later suites type
    // there, and Run in Terminal waits for it (agents' Open in Terminal).
    if (now.busy > was.busy)
        leak("left a program running in " + std::to_string(now.busy - was.busy) +
             " terminal(s) (stop it before the suite ends)");
    // A line typed at a shell's prompt and left there joins the next suite's
    // typing: find_bar's and folding's "stty raw ..." after install_hints'
    // "uv tool install ...", never run. Nothing tells a typed line from the
    // prompt, so every idle shell gets Ctrl+U, which an empty prompt ignores.
    if (putBack)
        for (Terminal* t : allTerminals())
            if (t->runBlocker(nullptr).empty()) sendCtrlU(t);
#endif
    if (now.waiting > was.waiting) {
        leak("left " + std::to_string(now.waiting) + " terminal(s) marked waiting where it found " +
             std::to_string(was.waiting) + " (a notify's mark; TerminalAttention::typed clears it)");
#ifdef MUTIBLE_ENABLE_TERMINAL
        if (putBack)
            for (Terminal* t : allTerminals())
                if (TerminalAttention::markName(t->controlId()) == std::string("waiting"))
                    TerminalAttention::typed(t->controlId());
#endif
    }
    if (now.terminals > was.terminals)
        leak("left " + std::to_string(now.terminals) + " terminal tab(s) where it found " +
                 std::to_string(was.terminals),
             false);
    // The two files are the harness's to put back between suites, so a suite
    // that changes them needs no step of its own to undo it; said, not failed.
    if (now.settings != was.settings) {
        leak("left settings.conf changed, put back: \"" + shortText(now.settings) + "\" (was \"" +
                 shortText(was.settings) + "\")",
             false);
        if (putBack) MCGuiSetSettings(was.settings);
    }
    if (now.keys != was.keys) {
        leak("left keys.conf changed, put back: \"" + shortText(now.keys) + "\" (was \"" +
                 shortText(was.keys) + "\")",
             false);
        if (putBack) MCGuiSetKeys(was.keys);
    }
    // Find in Folder keeps its scope, its files to include and exclude and
    // its options while its window is closed, and the app's Ctrl+Shift+F
    // changes the scope only for a folder selected in the tree: preview_tabs
    // searched replace_in_folder's rif with files to include "three.txt"
    // and found nothing, in the soak's shuffled orders. Said, not failed,
    // and put back as a new window has it.
    if (!now.folderSearch.empty() && now.folderSearch != was.folderSearch) {
        leak("left Find in Folder changed, put back: " + now.folderSearch +
                 (was.folderSearch.empty() ? "" : " (was " + was.folderSearch + ")"),
             false);
        if (putBack) MCGuiResetFolderSearch();
    }
    // The browser pane left on show takes the right side's width from the
    // editor: browser_tabs left it, and tab_strip, after it in the soak's
    // order, found a strip half as wide that widened under its drag. Said,
    // not failed, and hidden as View > Toggle Browser hides it.
    if (now.browsers > was.browsers) {
        leak("left the browser pane on show in " + std::to_string(now.browsers) + " window(s) where it found " +
                 std::to_string(was.browsers) + ", hidden",
             false);
        if (putBack)
            for (const MCGuiAppWindow& a : MCGuiAppWindows())
                if (a.layout && a.layout->browserShown())
                    g_action_group_activate_action(G_ACTION_GROUP(a.window), "togglebrowser", nullptr);
    }
#ifdef MUTIBLE_ENABLE_BROWSER
    // An agent's page (`mutible browse`) left open: browser_pages, after
    // terminal_splits' `split --right browse`, found one where it checks
    // there is none. Said, not failed, and closed.
    if (now.agentPages > was.agentPages) {
        leak("left an agent page open in " + std::to_string(now.agentPages) + " window(s) where it found " +
                 std::to_string(was.agentPages) + ", closed",
             false);
        if (putBack)
            for (const MCGuiAppWindow& a : MCGuiAppWindows())
                if (a.browser && a.browser->hasAgentPage()) a.browser->closeAgentPage();
    }
#endif
    // The project folder made a git repository and left one: every row of
    // the tree is then untracked, with a tooltip ("Untracked (git)"), the
    // Source Control panel and the gutters have a repository a later suite
    // does not expect, and a worktree under .claude/worktrees is a node of
    // its own at the tree's top. agents left one (git init, a worktree and
    // its branch), and preview_tabs' clicks, after it in the soak's
    // shuffled orders, landed on the tooltips. Said, not failed; removed
    // with the worktrees git lists inside the project.
    if (now.projectRepository && !was.projectRepository) {
        leak("left the project folder a git repository, removed (its .git and the worktrees in it)", false);
        if (putBack) MCGuiRemoveProjectRepository();
    }
}

// Steps are played on a timer at the loop's default priority, as X events are,
// so the events a step sends arrive before the next step reads anything.
bool gFinished = false;

gboolean tick(gpointer) {
    if (gFinished) return G_SOURCE_REMOVE;   // the whole run's limit ended it
    if (MCGuiInputPending()) {   // a drag still under way belongs to the step before
        schedule(10);
        return G_SOURCE_REMOVE;
    }
    if (gWaiting) {
        bool done = false;
        const bool timedOut = MCGuiNowMs() > gDeadlineMs;
        if (gWaiting->kind == Kind::Wait) {
            Item* w = gWaiting;
            runGuarded(w->name, [&] { done = w->until(); });
        } else {
            done = gAsyncDone;
        }
        if (!done && !timedOut) {
            schedule(50);
            return G_SOURCE_REMOVE;
        }
        if (!done) {
            char text[256];
            snprintf(text, sizeof text, "timed out after %.0f s: %s", gWaiting->timeout,
                     gWaiting->name.c_str());
            MCGuiCheck(false, text, __FILE__, __LINE__);
            noteState();
        }
        gWaiting = nullptr;
    }
    if (gIndex >= gItems.size()) {
        gStep = "what the suite left";
        runGuarded(gStep, checkWhatSuiteLeft);
        endSuite();
        finish();
        return G_SOURCE_REMOVE;
    }
    Item* item = gItems[gIndex++].get();
    gStep = item->name;
    startItem(item);
    switch (item->kind) {
    case Kind::Suite:
        gStep = "what the suite left";
        runGuarded(gStep, checkWhatSuiteLeft);
        gStep = item->name;
        endSuite();
        gSuite = item->name;
        gSuiteName = item->name.c_str();
        gSuitePass = gSuiteFail = 0;
        runGuarded("looking at the world", [] { gWorldAtStart = lookAtWorld(); });
        printf("Suite %s\n", item->name.c_str());
        fflush(stdout);
        break;
    case Kind::Step:
        if (!envOr("MC_GUI_VERBOSE").empty()) MCGuiNote("%s", item->name.c_str());
        runGuarded(item->name, item->block);
        break;
    case Kind::Wait:
        gWaiting = item;
        gDeadlineMs = MCGuiNowMs() + item->timeout * 1000;
        break;
    case Kind::Async: {
        gWaiting = item;
        gDeadlineMs = MCGuiNowMs() + item->timeout * 1000;
        gAsyncDone = false;
        Item* mine = item;
        runGuarded(item->name, [mine] {
            mine->async([mine] {
                if (gWaiting == mine) gAsyncDone = true;
            });
        });
        break;
    }
    }
    schedule(10);
    return G_SOURCE_REMOVE;
}

std::set<std::string> suiteNames(const std::string& list) {
    std::set<std::string> out;
    std::istringstream in(list);
    for (std::string w; in >> w;) out.insert(w);
    return out;
}

void selectSuites() {
    const std::set<std::string> wanted = suiteNames(envOr("MC_GUI_SUITES"));
    // MC_GUI_SKIP_SUITES leaves suites out: CI runs the list as two
    // processes at once, one naming its half and the other skipping it, so
    // a new suite lands in the second without anyone listing it.
    const std::set<std::string> skipped = suiteNames(envOr("MC_GUI_SKIP_SUITES"));
    // A name in either list that no suite has is a failure, not a silent
    // skip: a suite renamed after CI's list named it would otherwise run in
    // neither half.
    std::set<std::string> known;
    for (const Suite& s : suites()) known.insert(s.name);
    for (const std::set<std::string>* list : {&wanted, &skipped})
        for (const std::string& w : *list)
            if (!known.count(w)) MCGuiCheck(false, "no suite is named \"" + w + "\"", __FILE__, __LINE__);
    std::vector<const Suite*> chosen;
    for (const Suite& s : suites()) {
        // A suite named with a leading underscore is a part another suite
        // runs in a process of its own: only when named.
        if (wanted.empty() ? s.name[0] == '_' : !wanted.count(s.name)) continue;
        if (skipped.count(s.name)) continue;
        chosen.push_back(&s);
    }
    // MC_GUI_IN_ORDER=1: the suites in the order MC_GUI_SUITES names them,
    // to replay part of a shuffled run (a soak round's log names its order)
    // up to the suite that failed.
    if (!envOr("MC_GUI_IN_ORDER").empty() && !wanted.empty()) {
        std::vector<std::string> named;
        std::istringstream in(envOr("MC_GUI_SUITES"));
        for (std::string w; in >> w;) named.push_back(w);
        auto place = [&named](const Suite* s) {
            return std::find(named.begin(), named.end(), s->name) - named.begin();
        };
        std::stable_sort(chosen.begin(), chosen.end(),
                         [&place](const Suite* a, const Suite* b) { return place(a) < place(b); });
    }
    // MC_GUI_SHUFFLE=<seed>: the chosen suites in the order the seed gives
    // (tests/GuiRetry.h), the same on every machine, so the soak
    // workflow's rounds can be replayed. Otherwise suites.txt's order.
    const std::string shuffle = envOr("MC_GUI_SHUFFLE");
    if (!shuffle.empty()) {
        uint64_t seed = 0;
        if (!GuiRetry::parseSeed(shuffle.c_str(), &seed)) {
            MCGuiCheck(false, "MC_GUI_SHUFFLE is \"" + shuffle + "\", not a number", __FILE__, __LINE__);
        } else {
            std::vector<const Suite*> order;
            for (size_t i : GuiRetry::shuffledOrder(chosen.size(), seed)) order.push_back(chosen[i]);
            chosen = order;
            printf("Suite order shuffled with MC_GUI_SHUFFLE=%llu (the same seed and suites replay it)\n",
                   (unsigned long long)seed);
            fflush(stdout);
        }
    }
    int ran = 0;
    for (const Suite* s : chosen) {
        enqueue(Kind::Suite, s->name, 0);
        s->body();
        ran++;
    }
    if (!ran) {
        printf("No suite matched \"%s\".\n", envOr("MC_GUI_SUITES").c_str());
        gFail++;
        GuiRetry::noteFailure(gResults, "", "no suite matched");
    }
}

void finish() {
    if (gFinished) return;
    gFinished = true;
    gStep = "the end";
    gOutside = true;
    gItemName = "closing the windows";
    gItemDeadlineMs = MCGuiNowMs() + 30000;
    printf("\nGUI checks: %d passed, %d failed (%.1f s)\n", gPass, gFail,
           (MCGuiNowMs() - gStartedMs) / 1000);
    if (gSlow || gLeaks)
        printf("Also: %d timing check(s) over their bound within this runner's room, %d leak(s) "
               "between suites\n", gSlow, gLeaks);
    printNearestBounds();
    fflush(stdout);
    // What failed, for the wrapper, which may retry a failed suite
    // (MC_GUI_RETRIES; "_" children are given none to write).
    gResults.complete = true;
    if (!envOr("MC_GUI_RESULTS").empty()) GuiRetry::writeResults(envOr("MC_GUI_RESULTS"), gResults);
    // The windows go the way a user's do (the shells hung up, the servers
    // told to stop), then a short turn of the loop for what that starts. The
    // wrapper stops anything that still runs.
    MCGuiCloseAllWindows();
    g_timeout_add(300, [](gpointer) -> gboolean {
        _exit(gFail ? 1 : 0);
        return G_SOURCE_REMOVE;
    }, nullptr);
}

// A step stuck on the main thread never reaches the loop's own timeouts.
void watchdog() {
    for (;;) {
        sleep(1);
        const double deadline = gItemDeadlineMs.load();
        if (deadline == 0 || MCGuiNowMs() < deadline) continue;
        fprintf(stdout, "\nFATAL: stuck in [%s] (step: %s); the main thread did not come back\n",
                gSuiteName.load(), gItemName.load());
        fflush(stdout);
        _exit(124);
    }
}

// The main thread's id, so a crash can say whether it was elsewhere.
long gMainTid = 0;

// A crash says where it was, before the signal does its usual work.
void reportCrash(int sig) {
    char head[512];
    // Which thread: a heap check's abort can come on any of them.
    const long tid = (long)syscall(SYS_gettid);
    int n = snprintf(head, sizeof head, "\nFATAL signal %d in [%s] (step: %s) on %s thread %ld\n", sig,
                     gSuiteName.load(), gItemName.load(), tid == gMainTid ? "the main" : "another",
                     tid);
    if (n > 0 && write(1, head, (size_t)n) < 0) {}
    void* frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, 1);
    signal(sig, SIG_DFL);
    raise(sig);
}

// Runs once the window is on screen, from the main loop.
gboolean startWhenWindowIsUp(gpointer) {
    static double giveUpMs = 0;
    if (giveUpMs == 0) giveUpMs = MCGuiNowMs() + 30000;
    GtkWindow* window = MCGuiApp().window;
    if (!window || !gtk_widget_get_mapped(GTK_WIDGET(window))) {
        if (MCGuiNowMs() < giveUpMs) return G_SOURCE_CONTINUE;
        printf("FATAL: no window on the display after 30 s\n");
        fflush(stdout);
        _exit(2);
    }
    // Windows closed by a suite must not end the application under the runner.
    g_application_hold(G_APPLICATION(MCGuiApplication()));

    gStartedMs = MCGuiNowMs();
    if (const char* s = getenv("MC_GUI_STEP_SECONDS")) gStepSeconds = atof(s);
    std::thread(watchdog).detach();

    MCGuiFocusKeyWindow();
    // X focus is what makes the window active with no window manager.
    MCGuiWait("the window takes the keyboard", 10, [] {
        GtkWindow* w = MCGuiKeyWindow();
        if (!w) return false;
        if (gtk_window_is_active(w)) return true;
        // Or a question it opened with (a tab back from a backup whose file
        // changed since), which has the keyboard then.
        GtkWindow* top = MCGuiTopWindow();
        return top != w && gtk_window_get_transient_for(top) == w;
    });
    selectSuites();

    const double total = getenv("MC_GUI_TOTAL_SECONDS") ? atof(getenv("MC_GUI_TOTAL_SECONDS")) : 1200;
    g_timeout_add_seconds((guint)(total > 0 ? total : 1200), [](gpointer) -> gboolean {
        gOutside = true;
        MCGuiCheck(false, "the whole run finished within its limit", __FILE__, __LINE__);
        finish();
        return G_SOURCE_REMOVE;
    }, nullptr);
    schedule(10);
    return G_SOURCE_REMOVE;
}

// ------------------------------------------------------- the display check
// The peer of DISPLAY's socket must be the Xvfb the wrapper started, checked
// through the kernel (SO_PEERCRED) rather than by the name in DISPLAY. The
// connection is closed at once, and nothing has spoken X on it, so a display
// that is refused sees no client and no window.
bool ourDisplay(std::string* why) {
    const char* display = getenv("DISPLAY");
    const char* pidText = getenv("MC_GUI_XVFB_PID");
    if (getenv("WAYLAND_DISPLAY") || getenv("WAYLAND_SOCKET")) {
        *why = "WAYLAND_DISPLAY or WAYLAND_SOCKET is set";
        return false;
    }
    if (envOr("GDK_BACKEND") != "x11") {
        *why = "GDK_BACKEND is not x11";
        return false;
    }
    if (!pidText) {
        *why = "no Xvfb of this run is named (MC_GUI_XVFB_PID)";
        return false;
    }
    if (!display || display[0] != ':') {
        *why = std::string("DISPLAY is ") + (display ? display : "unset") + ", not a local display";
        return false;
    }
    for (const char* p = display + 1; *p; p++)
        if (!g_ascii_isdigit(*p)) {
            *why = "DISPLAY is not a plain local display";
            return false;
        }
    const pid_t expected = (pid_t)atoi(pidText);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *why = "no socket";
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "/tmp/.X11-unix/X%s", display + 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        *why = std::string("cannot reach ") + addr.sun_path;
        close(fd);
        return false;
    }
    ucred peer{};
    socklen_t len = sizeof peer;
    const bool got = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &len) == 0;
    close(fd);
    if (!got || peer.pid != expected) {
        *why = std::string("the server on ") + display + " is not the Xvfb this run started";
        return false;
    }
    char exe[PATH_MAX];
    const std::string link = "/proc/" + std::to_string(expected) + "/exe";
    ssize_t n = readlink(link.c_str(), exe, sizeof exe - 1);
    if (n <= 0) {
        *why = "cannot read the server's program";
        return false;
    }
    exe[n] = 0;
    const char* base = strrchr(exe, '/');
    if (strcmp(base ? base + 1 : exe, "Xvfb") != 0) {
        *why = std::string("the server is ") + exe + ", not Xvfb";
        return false;
    }
    return true;
}

}  // namespace

// ------------------------------------------------------------------ main
int main(int argc, char** argv) {
    if (MCGuiIsDbusDaemonShim(argc, argv)) return MCGuiDbusDaemonShim(argv);
    // The app's command line subcommands, as the real binary takes them:
    // the headless session host a window starts as this program
    // (`/proc/self/exe host --folder ...`, SessionLink.cpp), never a run of
    // the checks, nor one that dies with the app (PR_SET_PDEATHSIG below).
    if (argc > 1 && ControlChannel::isSubcommand(argv[1])) return mutible_main(argc, argv);
    if (!getenv("MC_GUI_CHILD")) return MCGuiWrapperMain(argc, argv);

    prctl(PR_SET_PDEATHSIG, SIGKILL);   // never outlive the run around it

    std::string why;
    if (!ourDisplay(&why)) {
        fprintf(stderr,
                "mutible-guitest: refusing to run: %s.\n"
                "Run it without MC_GUI_CHILD; it starts its own display.\n",
                why.c_str());
        return 2;
    }
    // backtrace() loads libgcc's unwinder on its first call, which allocates;
    // in the handler, after glibc's heap checks abort while holding an
    // arena's lock, that first call can wait for the lock for ever and print
    // nothing (CI, linux/HANDOFF.md item 88). Called once here, it does not.
    void* warm[1];
    if (backtrace(warm, 1) < 0) {}
    gMainTid = (long)syscall(SYS_gettid);
    for (int s : {SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL}) signal(s, reportCrash);
    // A link to mailto: or an app's scheme is recorded, never handed to the
    // desktop: nothing a check does starts another app.
    Editor::launchUriHook = [](const std::string& url) { launchedUris().push_back(url); };

    // mutible_main's main loop picks this up; it polls until the window it
    // opens is mapped, then the suites start.
    g_timeout_add(50, startWhenWindowIsUp, nullptr);
    return mutible_main(argc, argv);
}
