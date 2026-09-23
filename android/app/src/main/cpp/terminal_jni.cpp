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

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "TerminalScreen.h"

namespace {

/**
 * One shell and the screen its output is read into.
 *
 * Two threads use it. The reader thread (Pty.startReading) sits in nativePump
 * feeding output to the screen, while the UI thread types, resizes and takes
 * snapshots. `lock` guards the screen and is never held while waiting for
 * output, so typing is never stuck behind select(). `writeLock` keeps the
 * UI's keys and the reader's replies from interleaving on the pty.
 *
 * Neither thread may free it while the other can still reach it, so it is
 * counted: one reference for the Kotlin owner, dropped by nativeClose, and
 * one for the reader, dropped by nativeReaderDone when its loop ends. The
 * last one out closes the pty and deletes. Closing only raises `closing`;
 * the reader notices within one pump timeout and leaves by itself.
 */
struct Session {
    int fd = -1;
    pid_t pid = -1;
    bool reaped = false;   // set by the reader only, before it lets go
    std::mutex lock;
    std::mutex writeLock;
    std::atomic<bool> closing{false};
    std::atomic<int> refs{2};
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

void WriteAll(Session *s, const char *data, size_t length) {
    std::lock_guard<std::mutex> guard(s->writeLock);
    size_t written = 0;
    while (written < length) {
        const ssize_t n = write(s->fd, data + written, length - written);
        if (n <= 0) break;
        written += static_cast<size_t>(n);
    }
}

/**
 * Drops one reference and, for the last, ends the shell and frees the
 * session. Closing the master hangs up the terminal, which ends what the
 * shell started; the shell gets SIGHUP, then SIGKILL if it has not gone, so
 * it is always reaped rather than left a zombie.
 */
void Release(Session *s) {
    if (s->refs.fetch_sub(1) != 1) return;
    if (s->fd >= 0) close(s->fd);
    if (s->pid > 0 && !s->reaped) {
        kill(s->pid, SIGHUP);
        if (waitpid(s->pid, nullptr, WNOHANG) == 0) {
            kill(s->pid, SIGKILL);
            waitpid(s->pid, nullptr, 0);
        }
    }
    delete s;
}

/** The first code point of a cell's UTF-8; combining marks are not drawn. */
jint FirstCodePoint(const std::string &ch) {
    if (ch.empty()) return 0;
    const auto *b = reinterpret_cast<const unsigned char *>(ch.data());
    const size_t n = ch.size();
    if (b[0] < 0x80) return b[0];
    if ((b[0] & 0xE0) == 0xC0 && n >= 2)
        return ((b[0] & 0x1F) << 6) | (b[1] & 0x3F);
    if ((b[0] & 0xF0) == 0xE0 && n >= 3)
        return ((b[0] & 0x0F) << 12) | ((b[1] & 0x3F) << 6) | (b[2] & 0x3F);
    if ((b[0] & 0xF8) == 0xF0 && n >= 4)
        return ((b[0] & 0x07) << 18) | ((b[1] & 0x3F) << 12) |
               ((b[2] & 0x3F) << 6) | (b[3] & 0x3F);
    return 0xFFFD;
}

/** 0xAARRGGBB, or 0 for the terminal's default colour. */
jint Colour(const TermColor &c) {
    return c.kind == TermColor::Default
               ? 0
               : static_cast<jint>(0xFF000000u | c.rgb());
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
                                        jstring home, jstring cwd, jint cols,
                                        jint rows) {
    const char *shellChars = env->GetStringUTFChars(shell, nullptr);
    const char *homeChars = env->GetStringUTFChars(home, nullptr);
    const char *cwdChars = env->GetStringUTFChars(cwd, nullptr);
    std::string shellPath(shellChars ? shellChars : "/system/bin/sh");
    std::string homePath(homeChars ? homeChars : "/");
    std::string cwdPath(cwdChars ? cwdChars : homePath);
    env->ReleaseStringUTFChars(shell, shellChars);
    env->ReleaseStringUTFChars(home, homeChars);
    env->ReleaseStringUTFChars(cwd, cwdChars);

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
        // The open folder when the shell can reach it, else home.
        if (chdir(cwdPath.c_str()) != 0) chdir(homePath.c_str());
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
 * exited or the session is closing. Only the reader thread calls this.
 */
JNIEXPORT jint JNICALL
Java_org_minicode_editor_Pty_nativePump(JNIEnv *, jclass, jlong handle,
                                        jint timeoutMs) {
    Session *s = Get(handle);
    if (!s || s->closing) return -1;

    // The wait holds no lock, so the UI keeps typing and drawing meanwhile.
    fd_set reads;
    FD_ZERO(&reads);
    FD_SET(s->fd, &reads);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    const int ready = select(s->fd + 1, &reads, nullptr, nullptr, &tv);
    if (s->closing) return -1;
    if (ready <= 0) return 0;

    char buffer[8192];
    const ssize_t n = read(s->fd, buffer, sizeof buffer);
    if (n <= 0) {
        // The shell has gone. Reap it here, where blocking costs nothing.
        if (s->pid > 0 && waitpid(s->pid, nullptr, 0) == s->pid) s->reaped = true;
        return -1;
    }

    // Anything the program asked the terminal to answer (cursor position,
    // device attributes) goes straight back, as the Mac app does.
    std::string replies;
    {
        std::lock_guard<std::mutex> guard(s->lock);
        s->screen.feed(buffer, static_cast<size_t>(n));
        replies = s->screen.takeReplies();
    }
    if (!replies.empty()) WriteAll(s, replies.data(), replies.size());
    return 1;
}

/** The reader thread is done with the session; see Session. */
JNIEXPORT void JNICALL
Java_org_minicode_editor_Pty_nativeReaderDone(JNIEnv *, jclass, jlong handle) {
    if (Session *s = Get(handle)) Release(s);
}

JNIEXPORT void JNICALL
Java_org_minicode_editor_Pty_nativeWrite(JNIEnv *env, jclass, jlong handle,
                                         jbyteArray data) {
    Session *s = Get(handle);
    if (!s || s->fd < 0) return;
    const jsize length = env->GetArrayLength(data);
    std::vector<jbyte> bytes(static_cast<size_t>(length));
    env->GetByteArrayRegion(data, 0, length, bytes.data());
    WriteAll(s, reinterpret_cast<const char *>(bytes.data()),
             static_cast<size_t>(length));
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
    bool appCursor = false, appKeypad = false;
    if (s) {
        std::lock_guard<std::mutex> guard(s->lock);
        appCursor = s->screen.appCursorKeys();
        appKeypad = s->screen.appKeypad();
    }
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
    std::lock_guard<std::mutex> guard(s->lock);
    s->screen.resize(cols, rows);
    SetSize(s->fd, cols, rows);
}

/**
 * The whole screen in one copy, taken under the lock so a frame is never
 * half of one update and half of the next. A header, then four ints per
 * cell, row by row:
 *
 *   rows, cols, cursorRow, cursorCol, cursorVisible,
 *   then per cell: code point, foreground, background, flags
 *
 * The code point is 0 for the right half of a wide character, which the left
 * half already covers. Colours are 0xAARRGGBB, with 0 for a cell the program
 * left at the terminal's default: the view paints those in the panel's own
 * colours, as the macOS app does with its MCTerminalDefaultForeground marker.
 * Without that, "default" reads as black, and a dark panel shows black text
 * on grey. Flags are the CELL_* bits in Pty.kt: bold, inverse and wide.
 *
 * Inverse is sent as a flag rather than applied here, because swapping a
 * default colour needs the panel's colours, and those live in Kotlin.
 */
JNIEXPORT jintArray JNICALL
Java_org_minicode_editor_Pty_nativeSnapshot(JNIEnv *env, jclass, jlong handle) {
    Session *s = Get(handle);
    if (!s) return env->NewIntArray(0);
    std::vector<jint> out;
    {
        std::lock_guard<std::mutex> guard(s->lock);
        const TerminalScreen &screen = s->screen;
        const int rows = screen.rows(), cols = screen.cols();
        out.reserve(5 + static_cast<size_t>(rows * cols) * 4);
        out.push_back(rows);
        out.push_back(cols);
        out.push_back(screen.cursorRow());
        out.push_back(screen.cursorCol());
        out.push_back(screen.cursorVisible() ? 1 : 0);
        for (int row = 0; row < rows; row++) {
            for (int col = 0; col < cols; col++) {
                const TermCell &cell = screen.cell(row, col);
                jint flags = 0;
                if (cell.style.bold) flags |= 1;
                if (cell.style.inverse) flags |= 2;
                if (cell.width == 2) flags |= 4;
                out.push_back(cell.width == 0 ? 0 : FirstCodePoint(cell.ch));
                out.push_back(Colour(cell.style.fg));
                out.push_back(Colour(cell.style.bg));
                out.push_back(flags);
            }
        }
    }
    jintArray array = env->NewIntArray(static_cast<jsize>(out.size()));
    env->SetIntArrayRegion(array, 0, static_cast<jsize>(out.size()), out.data());
    return array;
}

/**
 * The Kotlin owner is done. The reader is told to stop, and whichever of the
 * two lets go last frees the session; nothing may use the handle after this.
 */
JNIEXPORT void JNICALL
Java_org_minicode_editor_Pty_nativeClose(JNIEnv *, jclass, jlong handle) {
    Session *s = Get(handle);
    if (!s) return;
    s->closing = true;
    Release(s);
}

}  // extern "C"
