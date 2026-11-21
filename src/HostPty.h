// HostPty.h — a shell on a pseudo-terminal, owned by the session's host
// (SESSION-DESIGN.md, stage 5; SessionHost.h runs its I/O on the host's
// thread). POSIX: Linux and macOS.
//
// The rules are the Mac's MCPty's (docs/notes/terminal.md, "The pty"):
//   - Everything the child needs (argv, the environment, the program's
//     path) is prepared before fork(): in a threaded process the child may
//     make only async-signal-safe calls until it execs. The terminal's modes
//     and size are set on the slave before the fork, so nothing races the
//     child's own tcsetattr.
//   - The master is non-blocking. Bytes it will not take yet wait in a
//     buffer, in order, until it has room (the owner polls for writing
//     while wantsWrite()); a write never blocks the thread, so a big paste
//     into a program that is not reading stalls nothing.
//   - Hanging up drops unwritten bytes, closes the master (which hangs the
//     shell up) and sends the shell SIGHUP; reap() then polls waitpid
//     without blocking, and a shell still there 2 s after the hang-up gets
//     SIGKILL. A shell is gone when kill(pid, 0) says ESRCH, not when
//     waitpid says so (docs/notes/terminal.md's trap).
// One thread uses a Pty at a time (the host's). Pure C++17 and POSIX.
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace Shared {

struct PtySpec {
    // A path, or a name looked up in the PATH `env` names (then /usr/bin
    // and /bin).
    std::string program;
    // argv[0] included (a leading '-' asks for a login shell); empty: the
    // program's own name.
    std::vector<std::string> argv;
    // "NAME=value", the whole environment the shell gets.
    std::vector<std::string> env;
    std::string cwd;   // "/" when it cannot be entered
    int cols = 80, rows = 24;
};

// TermSpec's hostShell (stage 8, a renderer on another machine): the
// host's own login shell as a login shell ("-zsh", "-bash"), in the host's
// environment `hostEnv` ("NAME=value") with `extra` laid over it (a name
// in `extra` replaces the host's). The shell is `shell` when given, else
// SHELL in `hostEnv`, else the user's from the password database, else
// /bin/sh. Pure but for that last look.
PtySpec ownShellSpec(const std::string& shell, const std::vector<std::string>& hostEnv,
                     const std::vector<std::string>& extra, const std::string& cwd, int cols,
                     int rows);

class Pty {
public:
    // The shell started on a fresh pty; null with `error` when it could
    // not be (no pty, no fork). A program that cannot be run still starts:
    // its child exits 127 at once, as a shell's would.
    static std::unique_ptr<Pty> spawn(const PtySpec& spec, std::string* error);
    // Hangs up if still open, and reaps: SIGKILL at once if the shell is
    // still there, then a blocking wait (only a host going away does this).
    ~Pty();
    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

    int fd() const { return fd_; }     // -1 once hung up
    int pid() const { return pid_; }

    // What the pty has now, appended to `out` (at most `max` bytes). False
    // once nothing more will come: every process holding the other end has
    // gone (EIO or end of file). Nothing to read is true with nothing added.
    bool read(std::string& out, std::size_t max = 65536);

    // Bytes for the program, written as far as the pty takes them now; the
    // rest waits, in order, for flush().
    void write(const char* data, std::size_t n);
    void write(const std::string& s) { write(s.data(), s.size()); }
    // Writes what waits as far as the pty takes it. True when nothing waits.
    bool flush();
    bool wantsWrite() const { return fd_ >= 0 && outStart_ < out_.size(); }
    std::size_t waiting() const { return out_.size() - outStart_; }

    // The size programs see (TIOCSWINSZ; the kernel sends SIGWINCH).
    void resize(int cols, int rows);
    // The size last set (spawn, resize) given back if a program has set
    // another; true when it had. Readline (bash's line editor) reads the
    // size and writes it back each time it takes the terminal (at every
    // prompt, and after a SIGWINCH): a resize between its read and its
    // write is undone, and the pty is left at the old size while the
    // screen is at the new one (term:size's failure, 2026-11-21).
    bool keepSize();
    // The foreground process group (tcgetpgrp), 0 when none can be read.
    int foreground() const;
    // The terminal's modes say a password is being read (ECHO off,
    // ICANON on), or the line editor is not in canonical mode (raw).
    bool echoOff() const;
    bool raw() const;
    // Signals off (ISIG): a raw program (cfmakeraw, fzf), not zsh's line
    // editor, which keeps ISIG (TerminalScreen::setRawTty's rule).
    bool signalsOff() const;

    // Whether the shell has exited, without hanging up (something it
    // started may still hold the pty): reaped then, with its status.
    bool exited(int* status);

    // Drops what waits, closes the master and sends the shell SIGHUP. A
    // second call does nothing.
    void hangUp();
    bool hungUp() const { return hungUp_; }
    // After the hang-up, or once read() said nothing more will come: the
    // shell's exit, polled without blocking. True once it was reaped, with
    // its wait status in `status` (-1 when someone else reaped it).
    // SIGKILL 2 s after the hang-up. Hangs up first if need be.
    bool reap(int* status);
    bool reaped() const { return reaped_; }
    int status() const { return status_; }

private:
    Pty() = default;
    int fd_ = -1;
    int pid_ = -1;
    unsigned short cols_ = 0, rows_ = 0;   // the size set last
    std::string out_;
    std::size_t outStart_ = 0;
    bool hungUp_ = false, reaped_ = false, killed_ = false;
    int status_ = -1;
    std::chrono::steady_clock::time_point hungUpAt_;
};

}  // namespace Shared
