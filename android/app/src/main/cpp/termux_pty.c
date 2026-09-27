// termux_pty.c: a pseudo terminal inside Termux, for MiniCode's terminal pane.
//
// MiniCode's own shell is /system/bin/sh in the app's sandbox, where Termux's
// python, git and compilers cannot be reached. Termux can run programs for
// MiniCode (RUN_COMMAND, see Termux.kt), but what it starts gets a socket for
// stdin and stdout, not a terminal: no line editing, no job control, and no
// vim or less. Termux ships nothing that turns a socket into a terminal by
// default (`script` is in util-linux, python may not be installed), so this
// small program does it. MiniCode carries it in the APK and hands it to
// Termux over the same socket the first time (TermuxShell.kt), and Termux
// runs it from its own folder, where it may run programs.
//
//     minicode-pty COLS ROWS PROGRAM [ARGS...]
//
// It starts PROGRAM on a new pty of COLS by ROWS and relays between that pty
// and its own stdin and stdout, which are the socket to MiniCode. Output goes
// back as it comes. Input arrives in frames, so the window size can travel on
// the same connection as the keys:
//
//     'd' LEN_HI LEN_LO BYTES...    keys and pastes for the program
//     'w' 0 4 COLS_HI COLS_LO ROWS_HI ROWS_LO    the pane changed size
//
// When the program exits the rest of its output is sent and the socket
// closes, which MiniCode reads as the shell having ended. When MiniCode goes
// away (the socket reaches end of file), the program is hung up, as a
// terminal window closing would.
//
// Plain C against bionic only, so it runs wherever Termux's own programs run.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int write_all(int fd, const unsigned char *p, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void set_size(int fd, int cols, int rows) {
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    ioctl(fd, TIOCSWINSZ, &ws);
}

// Sends whatever the pty still holds, without waiting for more.
static void drain(int master) {
    unsigned char buf[8192];
    int flags = fcntl(master, F_GETFL);
    fcntl(master, F_SETFL, flags | O_NONBLOCK);
    for (;;) {
        ssize_t n = read(master, buf, sizeof buf);
        if (n <= 0) break;
        if (write_all(1, buf, (size_t)n) < 0) break;
    }
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s COLS ROWS PROGRAM [ARGS...]\n", argv[0]);
        return 2;
    }
    int cols = atoi(argv[1]), rows = atoi(argv[2]);
    if (cols < 2) cols = 80;
    if (rows < 2) rows = 24;
    signal(SIGPIPE, SIG_IGN);

    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    int master = -1;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) {
        perror("forkpty");
        return 1;
    }
    if (pid == 0) {
        // The pty is stdin, stdout and stderr now; nothing else of the
        // parent's (the socket above all) may reach the shell.
        for (int fd = 3; fd < 1024; fd++) close(fd);
        signal(SIGPIPE, SIG_DFL);
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        execvp(argv[3], argv + 3);
        fprintf(stderr, "minicode-pty: cannot run %s: %s\r\n", argv[3], strerror(errno));
        _exit(127);
    }

    unsigned char in[4096 + 3];   // one frame's header and body, as it arrives
    size_t have = 0;
    unsigned char out[8192];
    int status = 0;
    for (;;) {
        struct pollfd fds[2] = {{0, POLLIN, 0}, {master, POLLIN, 0}};
        int ready = poll(fds, 2, 250);
        if (ready < 0 && errno != EINTR) break;

        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(master, out, sizeof out);
            if (n > 0) {
                if (write_all(1, out, (size_t)n) < 0) break;   // MiniCode is gone
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                break;   // every holder of the pty has closed it
            }
        }

        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(0, in + have, sizeof in - have);
            if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
                // MiniCode closed the pane or went away: hang up, as closing
                // a terminal window does.
                kill(-pid, SIGHUP);
                kill(pid, SIGHUP);
                break;
            }
            if (n > 0) have += (size_t)n;
            // Act on every whole frame; keep a partial one for next time.
            size_t at = 0;
            while (have - at >= 3) {
                size_t len = ((size_t)in[at + 1] << 8) | in[at + 2];
                if (len > sizeof in - 3) { at = have; break; }   // not our framing
                if (have - at < 3 + len) break;
                const unsigned char *body = in + at + 3;
                if (in[at] == 'd') {
                    write_all(master, body, len);
                } else if (in[at] == 'w' && len == 4) {
                    set_size(master, (body[0] << 8) | body[1], (body[2] << 8) | body[3]);
                }
                at += 3 + len;
            }
            memmove(in, in + at, have - at);
            have -= at;
        }

        // The shell has ended: send what it left on the pty, then stop,
        // even if a background job still holds the pty open.
        if (waitpid(pid, &status, WNOHANG) == pid) {
            drain(master);
            close(master);
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
    }
    close(master);
    // Give the shell a moment to leave after the hangup, then make sure.
    for (int i = 0; i < 20; i++) {
        if (waitpid(pid, &status, WNOHANG) == pid) return 0;
        usleep(50 * 1000);
    }
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return 0;
}
