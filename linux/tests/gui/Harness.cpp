// Harness.cpp — see Harness.h: the checks' stage. The wrapper is Wrapper.cpp,
// real input is Input.cpp, and the app's parts come from AppSeam.inc.
#include "Harness.h"

#include "Editor.h"

#include <execinfo.h>
#include <limits.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <set>
#include <sstream>
#include <thread>

int minicode_main(int argc, char** argv);    // src/main.cpp, renamed
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
int gPass = 0, gFail = 0, gSuitePass = 0, gSuiteFail = 0;
std::string gSuite, gStep;
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
}  // namespace

void MCGuiStep(const std::string& name, std::function<void()> block) {
    enqueue(Kind::Step, name, 0)->block = std::move(block);
}

void MCGuiWait(const std::string& what, double timeout, std::function<bool()> until) {
    enqueue(Kind::Wait, what, timeout)->until = std::move(until);
}

void MCGuiAsync(const std::string& name, double timeout, std::function<void(MCGuiDone)> block) {
    enqueue(Kind::Async, name, timeout)->async = std::move(block);
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

// ---------------------------------------------------------------- checks
bool MCGuiCheck(bool ok, const std::string& what, const char* file, int line) {
    if (ok) {
        gPass++;
        gSuitePass++;
    } else {
        gFail++;
        gSuiteFail++;
        const char* slash = strrchr(file, '/');
        printf("  FAIL [%s] %s:%d  %s  (in: %s)\n", gSuite.c_str(), slash ? slash + 1 : file, line,
               what.c_str(), gStep.c_str());
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

bool MCGuiCheckTime(double ms, double bound, const std::string& what, const char* file, int line) {
    char text[64];
    snprintf(text, sizeof text, " under %.0f ms of CPU (%.2f)", bound, ms);
    return MCGuiCheck(MCGuiTimingSkipped() || ms < bound, what + text, file, line);
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

void MCGuiSetSettings(const std::string& text) {
    MCGuiWriteFile(envOr("MINICODE_SETTINGS"), text);
    MCGuiReloadSettings();
}

void MCGuiSetKeys(const std::string& text) {
    MCGuiWriteFile(envOr("MINICODE_KEYS"), text);
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
                     std::function<void(int, std::string, std::string)> done) {
    GSubprocessLauncher* launcher = g_subprocess_launcher_new(
        (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE));
    g_subprocess_launcher_set_cwd(launcher, MCGuiProject().c_str());
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
    MCGuiRunProcess({runner, config, daemon, "--", selfPath(), folder}, {{"MC_GUI_SUITES", suite}},
                    [done](int status, std::string out, std::string err) {
                        done(status, out + err);
                    });
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
    MCGuiNote("state: the key window %s, the keyboard in a %s, %s on top",
              kw && gtk_window_is_active(kw) ? "active" : "not active",
              f ? G_OBJECT_TYPE_NAME(f) : "nothing",
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
        endSuite();
        finish();
        return G_SOURCE_REMOVE;
    }
    Item* item = gItems[gIndex++].get();
    gStep = item->name;
    startItem(item);
    switch (item->kind) {
    case Kind::Suite:
        endSuite();
        gSuite = item->name;
        gSuiteName = item->name.c_str();
        gSuitePass = gSuiteFail = 0;
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

void selectSuites() {
    std::set<std::string> wanted;
    std::istringstream in(envOr("MC_GUI_SUITES"));
    for (std::string w; in >> w;) wanted.insert(w);
    int ran = 0;
    for (const Suite& s : suites()) {
        // A suite named with a leading underscore is a part another suite
        // runs in a process of its own: only when named.
        if (wanted.empty() ? s.name[0] == '_' : !wanted.count(s.name)) continue;
        enqueue(Kind::Suite, s.name, 0);
        s.body();
        ran++;
    }
    if (!ran) {
        printf("No suite matched \"%s\".\n", envOr("MC_GUI_SUITES").c_str());
        gFail++;
    }
}

void finish() {
    if (gFinished) return;
    gFinished = true;
    gStep = "the end";
    gItemName = "closing the windows";
    gItemDeadlineMs = MCGuiNowMs() + 30000;
    printf("\nGUI checks: %d passed, %d failed (%.1f s)\n", gPass, gFail,
           (MCGuiNowMs() - gStartedMs) / 1000);
    fflush(stdout);
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

// A crash says where it was, before the signal does its usual work.
void reportCrash(int sig) {
    char head[512];
    int n = snprintf(head, sizeof head, "\nFATAL signal %d in [%s] (step: %s)\n", sig,
                     gSuiteName.load(), gItemName.load());
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
    if (!getenv("MC_GUI_CHILD")) return MCGuiWrapperMain(argc, argv);

    prctl(PR_SET_PDEATHSIG, SIGKILL);   // never outlive the run around it

    std::string why;
    if (!ourDisplay(&why)) {
        fprintf(stderr,
                "minicode-guitest: refusing to run: %s.\n"
                "Run it without MC_GUI_CHILD; it starts its own display.\n",
                why.c_str());
        return 2;
    }
    for (int s : {SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL}) signal(s, reportCrash);

    // minicode_main's main loop picks this up; it polls until the window it
    // opens is mapped, then the suites start.
    g_timeout_add(50, startWhenWindowIsUp, nullptr);
    return minicode_main(argc, argv);
}
