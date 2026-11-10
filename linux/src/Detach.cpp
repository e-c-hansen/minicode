// Detach.cpp — see Detach.h.
#include "Detach.h"

#include "LaunchOptions.h"

#include <gtk/gtk.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#ifndef MINICODE_VERSION
#define MINICODE_VERSION ""
#endif

namespace {

// How long the terminal is held at most while the app starts. A window is
// up in well under a second; past this, something is wrong, and holding the
// terminal longer would be the old behaviour again.
constexpr int kWaitSeconds = 30;

// The detached child's end of the pipe, until its first window is made.
int gReadyFd = -1;

double nowSeconds() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// This process's threads. A fork copies only the calling thread, so it is
// safe only while there is no other: a lock another thread held (GLib's,
// malloc's) would stay held in the child for good. Nothing has started one
// when main calls us; if a library's constructor ever does, the app stays
// in the foreground rather than risk it.
long threadCount() {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 1;
    char line[256];
    long threads = 1;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "Threads:", 8) == 0) {
            threads = strtol(line + 8, nullptr, 10);
            break;
        }
    fclose(f);
    return threads;
}

// Whether this process has a controlling terminal: /dev/tty opens only
// then. O_NONBLOCK, so a serial line waiting for its carrier cannot hold us.
bool hasControllingTerminal() {
    const int fd = open("/dev/tty", O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    close(fd);
    return true;
}

// /dev/null on each standard stream, except a regular file the user sent it
// to (`minicode 2>log` keeps its log). A terminal is what is let go of; a
// pipe goes too, since its reader in the shell would wait for the app to
// quit, and SIGPIPE would end the app once that reader went.
void letGoOfTerminal() {
    fflush(stdout);
    fflush(stderr);
    const int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null < 0) return;
    if (null <= 2) fcntl(null, F_SETFD, 0);   // a closed stream's place: keep it for children
    for (int fd = 0; fd <= 2; fd++) {
        struct stat st;
        if (fd == null) continue;
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) continue;
        dup2(null, fd);
    }
    if (null > 2) close(null);
}

void scopeAnswered(GObject* source, GAsyncResult* result, gpointer) {
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply) {
        g_variant_unref(reply);
    } else {
        // No systemd on the session bus, or it refused: the app runs as
        // well in the terminal's scope.
        g_debug("minicode: no systemd scope of its own: %s", error ? error->message : "?");
        g_clear_error(&error);
    }
}

// The request GNOME Shell (gnome-desktop's gnome_start_systemd_scope) and
// VTE make for the processes they start, for this process itself: mode
// "fail", a description, the PID, and CollectMode inactive-or-failed so
// systemd forgets the scope once minicode has gone.
void moveToOwnScope(GApplication* app) {
    GDBusConnection* bus = g_application_get_dbus_connection(app);
    const char* id = g_application_get_application_id(app);
    if (!bus || !id) return;
    const guint32 pid = (guint32)getpid();
    const std::string name = LaunchOptions::scopeName(id, (long)pid);
    GVariantBuilder* props = g_variant_builder_new(G_VARIANT_TYPE("a(sv)"));
    g_variant_builder_add(props, "(sv)", "Description",
                          g_variant_new_string("minicode, started from a terminal"));
    g_variant_builder_add(props, "(sv)", "PIDs",
                          g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32, &pid, 1, sizeof pid));
    g_variant_builder_add(props, "(sv)", "CollectMode", g_variant_new_string("inactive-or-failed"));
    GVariantBuilder* aux = g_variant_builder_new(G_VARIANT_TYPE("a(sa(sv))"));
    GVariant* args = g_variant_new("(ssa(sv)a(sa(sv)))", name.c_str(), "fail", props, aux);
    g_variant_builder_unref(props);
    g_variant_builder_unref(aux);
    g_dbus_connection_call(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
                           "org.freedesktop.systemd1.Manager", "StartTransientUnit", args,
                           G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, nullptr,
                           scopeAnswered, nullptr);
}

// The detached child's first window: the command line has been handled
// (its messages, a path that is not there among them, went to the
// terminal), so the terminal is let go of and the parent told.
gboolean onFirstWindow(GSignalInvocationHint*, guint n, const GValue* values, gpointer) {
    letGoOfTerminal();
    if (gReadyFd >= 0) {
        const char ready = 1;
        ssize_t w;
        do w = write(gReadyFd, &ready, 1);
        while (w < 0 && errno == EINTR);
        close(gReadyFd);
        gReadyFd = -1;
    }
    if (n > 0 && G_VALUE_HOLDS_OBJECT(&values[0])) {
        GObject* object = static_cast<GObject*>(g_value_get_object(&values[0]));
        if (G_IS_APPLICATION(object)) moveToOwnScope(G_APPLICATION(object));
    }
    return FALSE;   // once: the first window only
}

// The parent, still in the terminal: until the child's first window, or
// the child's end, whose status becomes ours.
int waitForChild(pid_t child, int fd) {
    const double until = nowSeconds() + kWaitSeconds;
    for (;;) {
        const int ms = (int)((until - nowSeconds()) * 1000);
        if (ms <= 0) {
            fprintf(stderr,
                    "minicode: still starting after %d seconds; it goes on apart from this "
                    "terminal\n",
                    kWaitSeconds);
            return 0;
        }
        pollfd p{fd, POLLIN, 0};
        const int r = poll(&p, 1, ms);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return 0;   // cannot wait; the child goes on by itself
        if (r == 0) continue;
        char c;
        const ssize_t got = read(fd, &c, 1);
        if (got < 0 && errno == EINTR) continue;
        if (got == 1) return 0;   // its window is up
        break;                    // it ended without one
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return 1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "minicode: %s\n", strsignal(WTERMSIG(status)));
        return 128 + WTERMSIG(status);
    }
    return 1;
}

}  // namespace

namespace Detach {

int start(int* argc, char*** argv) {
    const std::vector<std::string> given(*argv, *argv + *argc);
    const LaunchOptions::Parsed parsed = LaunchOptions::parse(given);
    if (!parsed.error.empty()) {
        fputs(parsed.error.c_str(), stderr);
        return 2;
    }
    if (parsed.help) {
        fputs(LaunchOptions::helpText().c_str(), stdout);
        return 0;
    }
    if (parsed.version) {
        fputs(LaunchOptions::versionText(MINICODE_VERSION).c_str(), stdout);
        return 0;
    }

    // What GApplication gets (and hands to a running minicode), kept for
    // the life of the process as argv is.
    static std::vector<std::string> kept;
    static std::vector<char*> pointers;
    kept = parsed.args;
    pointers.clear();
    for (std::string& s : kept) pointers.push_back(&s[0]);
    pointers.push_back(nullptr);
    *argc = (int)kept.size();
    *argv = pointers.data();

    const bool fromTerminal = isatty(0) || isatty(1) || isatty(2) || hasControllingTerminal();
    if (!LaunchOptions::shouldDetach(parsed, fromTerminal, getenv("MINICODE_FOREGROUND"))) return -1;
    if (threadCount() != 1) return -1;

    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return -1;
    fflush(stdout);
    fflush(stderr);
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;   // no fork: run in the foreground, as before
    }
    if (pid > 0) {
        close(fds[1]);
        return waitForChild(pid, fds[0]);
    }
    close(fds[0]);
    setsid();
    gReadyFd = fds[1];
    // The first window is GtkApplication's window-added. Its class must
    // exist before the signal can be looked up; the app makes it a moment
    // later anyway, and its startup class handler (gtk_init, the display)
    // has run by the time a window is made.
    g_type_class_ref(GTK_TYPE_APPLICATION);
    g_signal_add_emission_hook(g_signal_lookup("window-added", GTK_TYPE_APPLICATION), 0,
                               onFirstWindow, nullptr, nullptr);
    return -1;
}

}  // namespace Detach
