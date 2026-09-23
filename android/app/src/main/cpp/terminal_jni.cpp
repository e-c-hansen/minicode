// terminal_jni.cpp — a shell on a pseudo terminal, drawn by the shared
// TerminalScreen from ../../../src.
//
// Android has no Termux requirement for this: every device carries a shell at
// /system/bin/sh with the toybox utilities beside it, and an app may run it
// inside its own sandbox. What it cannot do is reach another app's files, so
// git, python and the rest arrive only if the user has Termux and points the
// terminal at its shell later.
//
// The parsing is not reimplemented here. Bytes from the pty go to the same
// TerminalScreen the macOS app uses for vim and less, and this file only
// carries them across the JNI boundary.
#include <jni.h>

#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include "TerminalScreen.h"

namespace {

struct Session {
    int fd = -1;
    pid_t pid = -1;
    TerminalScreen screen;
    Session(int cols, int rows) : screen(cols, rows) {}
};

Session *Get(jlong handle) { return reinterpret_cast<Session *>(handle); }

void SetSize(int fd, int cols, int rows) {
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    ioctl(fd, TIOCSWINSZ, &ws);
}

}  // namespace

extern "C" {

/**
 * Start a shell on a pty. `home` is where it runs and what HOME points at;
 * `shell` is the program, so a Termux shell can be used instead of the
 * system one when it is reachable. Returns 0 if it could not start.
 */
JNIEXPORT jlong JNICALL
Java_org_minicode_editor_Pty_nativeOpen(JNIEnv *env, jclass, jstring shell,
                                        jstring home, jint cols, jint rows) {
    const char *shellChars = env->GetStringUTFChars(shell, nullptr);
    const char *homeChars = env->GetStringUTFChars(home, nullptr);
    std::string shellPath(shellChars ? shellChars : "/system/bin/sh");
    std::string homePath(homeChars ? homeChars : "/");
    env->ReleaseStringUTFChars(shell, shellChars);
    env->ReleaseStringUTFChars(home, homeChars);

    auto session = std::make_unique<Session>(cols, rows);

    // Everything the child needs is built before the fork: only
    // async-signal-safe calls are allowed between fork and exec.
    const std::string term = "TERM=xterm-256color";
    const std::string homeEnv = "HOME=" + homePath;
    const std::string pathEnv = "PATH=/system/bin:/system/xbin:/vendor/bin";
    const std::string tmpEnv = "TMPDIR=" + homePath + "/tmp";
    char *argv[] = {const_cast<char *>(shellPath.c_str()), nullptr};
    char *envp[] = {const_cast<char *>(term.c_str()),
                    const_cast<char *>(homeEnv.c_str()),
                    const_cast<char *>(pathEnv.c_str()),
                    const_cast<char *>(tmpEnv.c_str()), nullptr};

    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);

    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) return 0;
    if (pid == 0) {
        chdir(homePath.c_str());
        execve(shellPath.c_str(), argv, envp);
        _exit(127);
    }
    fcntl(master, F_SETFD, FD_CLOEXEC);
    session->fd = master;
    session->pid = pid;
    return reinterpret_cast<jlong>(session.release());
}

/**
 * Wait up to `timeoutMs` for output, and give whatever arrives to the screen.
 * Returns 1 when the screen changed, 0 on a timeout, -1 when the shell has
 * exited. Called from a background thread; the view redraws from a snapshot.
 */
JNIEXPORT jint JNICALL
Java_org_minicode_editor_Pty_nativePump(JNIEnv *, jclass, jlong handle,
                                        jint timeoutMs) {
    Session *s = Get(handle);
    if (!s || s->fd < 0) return -1;

    fd_set reads;
    FD_ZERO(&reads);
    FD_SET(s->fd, &reads);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    const int ready = select(s->fd + 1, &reads, nullptr, nullptr, &tv);
    if (ready <= 0) return 0;

    char buffer[8192];
    const ssize_t n = read(s->fd, buffer, sizeof buffer);
    if (n <= 0) return -1;
    s->screen.feed(buffer, static_cast<size_t>(n));

    // Anything the program asked the terminal to answer (cursor position,
    // device attributes) goes straight back, as the Mac app does.
    const std::string replies = s->screen.takeReplies();
    if (!replies.empty()) write(s->fd, replies.data(), replies.size());
    return 1;
}

JNIEXPORT void JNICALL
Java_org_minicode_editor_Pty_nativeWrite(JNIEnv *env, jclass, jlong handle,
                                         jbyteArray data) {
    Session *s = Get(handle);
    if (!s || s->fd < 0) return;
    const jsize length = env->GetArrayLength(data);
    std::vector<jbyte> bytes(static_cast<size_t>(length));
    env->GetByteArrayRegion(data, 0, length, bytes.data());
    ssize_t written = 0;
    while (written < length) {
        const ssize_t n = write(s->fd, bytes.data() + written,
                                static_cast<size_t>(length - written));
        if (n <= 0) break;
        written += n;
    }
}

/** The bytes a typed character sends, Ctrl and Alt included. */
JNIEXPORT jbyteArray JNICALL
Java_org_minicode_editor_Pty_nativeEncodeChar(JNIEnv *env, jclass,
                                              jint codePoint, jint mods) {
    const std::string bytes = TerminalScreen::encodeChar(
        static_cast<uint32_t>(codePoint), mods);
    jbyteArray out = env->NewByteArray(static_cast<jsize>(bytes.size()));
    env->SetByteArrayRegion(out, 0, static_cast<jsize>(bytes.size()),
                            reinterpret_cast<const jbyte *>(bytes.data()));
    return out;
}

/** The bytes a special key sends; `key` is the ordinal of TermKey. */
JNIEXPORT jbyteArray JNICALL
Java_org_minicode_editor_Pty_nativeEncodeKey(JNIEnv *env, jclass, jlong handle,
                                             jint key, jint mods) {
    Session *s = Get(handle);
    const bool appCursor = s && s->screen.appCursorKeys();
    const bool appKeypad = s && s->screen.appKeypad();
    const std::string bytes = TerminalScreen::encodeKey(
        static_cast<TermKey>(key), mods, appCursor, appKeypad);
    jbyteArray out = env->NewByteArray(static_cast<jsize>(bytes.size()));
    env->SetByteArrayRegion(out, 0, static_cast<jsize>(bytes.size()),
                            reinterpret_cast<const jbyte *>(bytes.data()));
    return out;
}

JNIEXPORT void JNICALL
Java_org_minicode_editor_Pty_nativeResize(JNIEnv *, jclass, jlong handle,
                                          jint cols, jint rows) {
    Session *s = Get(handle);
    if (!s || s->fd < 0) return;
    s->screen.resize(cols, rows);
    SetSize(s->fd, cols, rows);
}

/** The whole screen as text, rows joined with newlines. */
JNIEXPORT jstring JNICALL
Java_org_minicode_editor_Pty_nativeText(JNIEnv *env, jclass, jlong handle) {
    Session *s = Get(handle);
    if (!s) return env->NewStringUTF("");
    std::string out;
    for (int row = 0; row < s->screen.rows(); row++) {
        if (row) out += '\n';
        out += s->screen.rowText(row);
    }
    return env->NewStringUTF(out.c_str());
}

/**
 * The foreground colour of every cell, row by row, as 0xAARRGGBB, with 0 for
 * a cell the program left at the terminal's default colour: the view paints
 * those in the panel's own text colour, as the macOS app does with its
 * MCTerminalDefaultForeground marker. Without that, "default" reads as black
 * and a dark panel shows black text on grey.
 *
 * The text comes from nativeText, so the two line up cell for cell.
 */
JNIEXPORT jintArray JNICALL
Java_org_minicode_editor_Pty_nativeColors(JNIEnv *env, jclass, jlong handle) {
    Session *s = Get(handle);
    if (!s) return env->NewIntArray(0);
    const int rows = s->screen.rows(), cols = s->screen.cols();
    std::vector<jint> colors(static_cast<size_t>(rows * cols));
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            const TermCell &cell = s->screen.cell(row, col);
            const TermColor &colour =
                cell.style.inverse ? cell.style.bg : cell.style.fg;
            colors[static_cast<size_t>(row * cols + col)] =
                colour.kind == TermColor::Default
                    ? 0
                    : static_cast<jint>(0xFF000000u | colour.rgb());
        }
    }
    jintArray out = env->NewIntArray(static_cast<jsize>(colors.size()));
    env->SetIntArrayRegion(out, 0, static_cast<jsize>(colors.size()),
                           colors.data());
    return out;
}

JNIEXPORT jint JNICALL
Java_org_minicode_editor_Pty_nativeCursorRow(JNIEnv *, jclass, jlong handle) {
    Session *s = Get(handle);
    return s ? s->screen.cursorRow() : 0;
}

JNIEXPORT jint JNICALL
Java_org_minicode_editor_Pty_nativeCursorCol(JNIEnv *, jclass, jlong handle) {
    Session *s = Get(handle);
    return s ? s->screen.cursorCol() : 0;
}

JNIEXPORT void JNICALL
Java_org_minicode_editor_Pty_nativeClose(JNIEnv *, jclass, jlong handle) {
    Session *s = Get(handle);
    if (!s) return;
    if (s->pid > 0) {
        kill(s->pid, SIGHUP);
        waitpid(s->pid, nullptr, WNOHANG);
    }
    if (s->fd >= 0) close(s->fd);
    delete s;
}

}  // extern "C"
