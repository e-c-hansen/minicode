// HostPty.cpp — see HostPty.h.
#include "HostPty.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace Shared {

namespace {

// The slave's name; ptsname is not thread-safe where ptsname_r is missing.
std::string slaveName(int master) {
#if defined(__linux__)
    char buf[128];
    if (ptsname_r(master, buf, sizeof buf) != 0) return "";
    return buf;
#else
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    const char* n = ptsname(master);
    return n ? n : "";
#endif
}

std::string envValue(const std::vector<std::string>& env, const char* key) {
    const std::string k = std::string(key) + "=";
    for (const std::string& e : env)
        if (e.compare(0, k.size(), k) == 0) return e.substr(k.size());
    return "";
}

// The program's path: as given when it holds a '/', else the first
// executable of that name in PATH (the shell's, then /usr/bin and /bin).
std::string findProgram(const std::string& program, const std::vector<std::string>& env) {
    if (program.find('/') != std::string::npos) return program;
    std::string path = envValue(env, "PATH");
    path += path.empty() ? "/usr/bin:/bin" : ":/usr/bin:/bin";
    std::size_t at = 0;
    while (at <= path.size()) {
        const std::size_t colon = path.find(':', at);
        const std::string dir = path.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
        if (!dir.empty()) {
            const std::string p = dir + "/" + program;
            struct stat st;
            if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(p.c_str(), X_OK) == 0) return p;
        }
        if (colon == std::string::npos) break;
        at = colon + 1;
    }
    return program;   // execve fails, and the child exits 127
}

void setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

}  // namespace

PtySpec ownShellSpec(const std::string& shell, const std::vector<std::string>& hostEnv,
                     const std::vector<std::string>& extra, const std::string& cwd, int cols,
                     int rows) {
    PtySpec p;
    p.program = shell;
    if (p.program.empty()) p.program = envValue(hostEnv, "SHELL");
    if (p.program.empty()) {
        struct passwd pw, *found = nullptr;
        char buf[4096];
        if (getpwuid_r(getuid(), &pw, buf, sizeof buf, &found) == 0 && found && found->pw_shell &&
            *found->pw_shell)
            p.program = found->pw_shell;
    }
    if (p.program.empty()) p.program = "/bin/sh";
    const std::size_t slash = p.program.rfind('/');
    p.argv = {"-" + (slash == std::string::npos ? p.program : p.program.substr(slash + 1))};
    auto nameOf = [](const std::string& e) { return e.substr(0, e.find('=')); };
    std::vector<std::string> names;
    for (const std::string& e : extra) names.push_back(nameOf(e));
    for (const std::string& e : hostEnv) {
        if (e.find('=') == std::string::npos) continue;
        const std::string n = nameOf(e);
        bool replaced = false;
        for (const std::string& x : names) replaced = replaced || x == n;
        if (!replaced) p.env.push_back(e);
    }
    for (const std::string& e : extra)
        if (e.find('=') != std::string::npos) p.env.push_back(e);
    p.cwd = cwd;
    p.cols = cols;
    p.rows = rows;
    return p;
}

std::unique_ptr<Pty> Pty::spawn(const PtySpec& spec, std::string* error) {
    auto fail = [&](const char* what) -> std::unique_ptr<Pty> {
        if (error) *error = std::string(what) + ": " + std::strerror(errno);
        return nullptr;
    };
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) return fail("no pseudo-terminal");
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        const int e = errno;
        close(master);
        errno = e;
        return fail("no pseudo-terminal");
    }
    const std::string name = slaveName(master);
    const int slave = name.empty() ? -1 : open(name.c_str(), O_RDWR | O_NOCTTY);
    if (slave < 0) {
        const int e = errno;
        close(master);
        errno = e;
        return fail("could not open the pseudo-terminal");
    }
    // The modes the child starts with: the kernel's defaults plus IUTF8
    // (Backspace over a character of several bytes), and its size.
    struct termios tio;
    if (tcgetattr(slave, &tio) == 0) {
#ifdef IUTF8
        tio.c_iflag |= IUTF8;
#endif
        tcsetattr(slave, TCSANOW, &tio);
    }
    struct winsize ws = {};
    ws.ws_col = (unsigned short)(spec.cols < 2 ? 2 : spec.cols);
    ws.ws_row = (unsigned short)(spec.rows < 2 ? 2 : spec.rows);
    ioctl(slave, TIOCSWINSZ, &ws);

    // Everything the child needs, before the fork.
    const std::string path = findProgram(spec.program, spec.env);
    std::vector<std::string> args = spec.argv;
    if (args.empty()) {
        const std::size_t slash = spec.program.rfind('/');
        args.push_back(slash == std::string::npos ? spec.program : spec.program.substr(slash + 1));
    }
    std::vector<char*> argv, envp;
    for (std::string& a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    std::vector<std::string> env = spec.env;
    for (std::string& e : env) envp.push_back(&e[0]);
    envp.push_back(nullptr);
    const std::string cwd = spec.cwd.empty() ? "/" : spec.cwd;
    long maxFd = sysconf(_SC_OPEN_MAX);
    if (maxFd < 0 || maxFd > 65536) maxFd = 65536;

    const pid_t pid = fork();
    if (pid == 0) {
        // The child: default signals, nothing blocked, a session of its own
        // with the pty as its terminal, no descriptor of the parent's.
        const int sigs[] = {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGPIPE,
                            SIGCHLD, SIGHUP, SIGTERM, SIGALRM, SIGWINCH};
        for (int s : sigs) signal(s, SIG_DFL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        setsid();
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        for (long fd = 3; fd < maxFd; fd++) close((int)fd);
        if (chdir(cwd.c_str()) != 0 && chdir("/") != 0) _exit(126);
        execve(path.c_str(), argv.data(), envp.data());
        _exit(127);
    }
    const int forkErr = errno;
    close(slave);
    if (pid < 0) {
        close(master);
        errno = forkErr;
        return fail("could not start the shell");
    }
    fcntl(master, F_SETFD, FD_CLOEXEC);
    setNonBlocking(master);
    std::unique_ptr<Pty> p(new Pty());
    p->fd_ = master;
    p->pid_ = pid;
    p->cols_ = ws.ws_col;
    p->rows_ = ws.ws_row;
    return p;
}

Pty::~Pty() {
    hangUp();
    if (reaped_ || pid_ <= 0) return;
    int st = 0;
    if (waitpid(pid_, &st, WNOHANG) == 0) {
        kill(pid_, SIGKILL);
        while (waitpid(pid_, &st, 0) < 0 && errno == EINTR) {}
    }
}

bool Pty::read(std::string& out, std::size_t max) {
    if (fd_ < 0) return false;
    char buf[65536];
    if (max > sizeof buf) max = sizeof buf;
    for (;;) {
        const ssize_t n = ::read(fd_, buf, max);
        if (n > 0) {
            out.append(buf, (std::size_t)n);
            return true;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        return false;   // EIO: the other end has gone
    }
}

void Pty::write(const char* data, std::size_t n) {
    if (fd_ < 0 || n == 0) return;
    out_.append(data, n);
    flush();
}

bool Pty::flush() {
    while (fd_ >= 0 && outStart_ < out_.size()) {
        const ssize_t n = ::write(fd_, out_.data() + outStart_, out_.size() - outStart_);
        if (n > 0) {
            outStart_ += (std::size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // What is written goes from the front only once it is half the
            // buffer, so a big paste's tail is not moved at every write.
            if (outStart_ > (64u << 10) && outStart_ > out_.size() / 2) {
                out_.erase(0, outStart_);
                outStart_ = 0;
            }
            return false;
        }
        break;   // the pty is gone: what is left goes nowhere
    }
    std::string().swap(out_);
    outStart_ = 0;
    return true;
}

void Pty::resize(int cols, int rows) {
    if (fd_ < 0) return;
    struct winsize ws = {};
    ws.ws_col = (unsigned short)(cols < 2 ? 2 : cols);
    ws.ws_row = (unsigned short)(rows < 2 ? 2 : rows);
    cols_ = ws.ws_col;
    rows_ = ws.ws_row;
    ioctl(fd_, TIOCSWINSZ, &ws);
}

bool Pty::keepSize() {
    struct winsize ws = {};
    if (fd_ < 0 || !cols_ || ioctl(fd_, TIOCGWINSZ, &ws) != 0) return false;
    if (ws.ws_col == cols_ && ws.ws_row == rows_) return false;
    resize(cols_, rows_);
    return true;
}

int Pty::foreground() const {
    if (fd_ < 0) return 0;
    const pid_t g = tcgetpgrp(fd_);
    return g > 0 ? (int)g : 0;
}

bool Pty::echoOff() const {
    struct termios t;
    if (fd_ < 0 || tcgetattr(fd_, &t) != 0) return false;
    return !(t.c_lflag & ECHO) && (t.c_lflag & ICANON);
}

bool Pty::raw() const {
    struct termios t;
    if (fd_ < 0 || tcgetattr(fd_, &t) != 0) return false;
    return !(t.c_lflag & ICANON);
}

bool Pty::signalsOff() const {
    struct termios t;
    if (fd_ < 0 || tcgetattr(fd_, &t) != 0) return false;
    return !(t.c_lflag & ISIG);
}

void Pty::hangUp() {
    if (hungUp_) return;
    hungUp_ = true;
    hungUpAt_ = std::chrono::steady_clock::now();
    std::string().swap(out_);
    outStart_ = 0;
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    if (pid_ > 0 && !reaped_) kill(pid_, SIGHUP);
}

bool Pty::exited(int* status) {
    if (!reaped_ && pid_ > 0) {
        int st = 0;
        pid_t r;
        do r = waitpid(pid_, &st, WNOHANG);
        while (r < 0 && errno == EINTR);
        if (r == pid_) {
            reaped_ = true;
            status_ = st;
        } else if (r < 0 && kill(pid_, 0) != 0 && errno == ESRCH) {
            reaped_ = true;
        }
    }
    if (reaped_ && status) *status = status_;
    return reaped_;
}

bool Pty::reap(int* status) {
    if (!hungUp_) hangUp();
    if (!reaped_ && pid_ > 0) {
        int st = 0;
        pid_t r;
        do r = waitpid(pid_, &st, WNOHANG);
        while (r < 0 && errno == EINTR);
        if (r == pid_) {
            reaped_ = true;
            status_ = st;
        } else if (r < 0) {
            // ECHILD: SIGCHLD ignored, or reaped elsewhere. Gone once the
            // kernel says so.
            if (kill(pid_, 0) != 0 && errno == ESRCH) reaped_ = true;
        } else if (!killed_ &&
                   std::chrono::steady_clock::now() - hungUpAt_ >= std::chrono::seconds(2)) {
            killed_ = true;   // it ignored the hang-up
            kill(pid_, SIGKILL);
        }
    } else if (pid_ <= 0) {
        reaped_ = true;
    }
    if (reaped_ && status) *status = status_;
    return reaped_;
}

}  // namespace Shared
