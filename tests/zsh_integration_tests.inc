// zsh_integration_tests.inc — the `zsh:*` groups of run_tests.cpp: the
// terminals' shell integration (resources/minicode-zsh-integration.zsh)
// run in a real zsh, started as the app starts it (ShellIntegration.h's
// .zshenv in ZDOTDIR beside a copy of the script), with commands on stdin.
// An interactive zsh reading a pipe runs precmd and preexec for each line,
// so the marks, the nonce and the exec hook are the script's own, not a
// copy of its logic. Skipped where /bin/zsh or the script is missing (CI's
// Linux jobs have no zsh), as the corpus checks skip without their files.

#include "ShellIntegration.h"
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <ctime>

extern char **environ;

namespace {

namespace zfs = std::filesystem;

struct ZshRun {
    int status = -1;       // exit status, or -1 when it had to be killed
    std::string out, err;  // stdout (the marks and what commands print), stderr
};

// Runs /bin/zsh (or `program`: shell_vte_tests.inc runs bash) with `args`,
// `env` as its whole environment and `input` on stdin, in `dir`; killed
// after 20 s.
ZshRun runZsh(const zfs::path &dir, const std::vector<std::string> &args,
              const std::vector<std::string> &env, const std::string &input,
              const char *program = "/bin/zsh") {
    ZshRun r;
    const zfs::path in = dir / "stdin.txt", out = dir / "stdout.txt", err = dir / "stderr.txt";
    { std::ofstream(in, std::ios::binary) << input; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, in.c_str(), O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_addopen(&fa, 2, err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    std::vector<std::string> argStore = {zfs::path(program).filename().string()};
    argStore.insert(argStore.end(), args.begin(), args.end());
    std::vector<char *> argv, envp;
    for (auto &a : argStore) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    std::vector<std::string> envStore = env;
    for (auto &e : envStore) envp.push_back(&e[0]);
    envp.push_back(nullptr);
    // A session of its own, so no controlling terminal: an interactive zsh
    // with one reads /dev/tty, not the commands on stdin (the tests failed
    // when run from a terminal rather than with output redirected).
    posix_spawnattr_t sa;
    posix_spawnattr_init(&sa);
    posix_spawnattr_setflags(&sa, POSIX_SPAWN_SETSID);
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, program, &fa, &sa, argv.data(), envp.data());
    posix_spawnattr_destroy(&sa);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return r;
    int status = 0;
    bool done = false;
    for (int i = 0; i < 2000 && !done; i++) {   // 2,000 x 10 ms
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) { done = true; break; }
        if (w < 0 && errno != EINTR) break;
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    if (!done) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        std::printf("    %s did not finish in 20 s; killed\n", program);
    } else if (WIFEXITED(status)) {
        r.status = WEXITSTATUS(status);
    }
    r.out = readFile(out.c_str());
    r.err = readFile(err.c_str());
    return r;
}

// A ZDOTDIR as Terminal.mm's IntegrationDirectory writes it, and a HOME of
// its own (so no user file is read), under `root`.
void writeIntegration(const zfs::path &root, const std::string &script, bool grid) {
    const zfs::path zdot = root / (grid ? "zdot-grid" : "zdot-line");
    writeFile(zdot / ShellIntegration::kZshScriptName, script);
    writeFile(zdot / ".zshenv", ShellIntegration::zshenvStub(grid));
}

std::vector<std::string> zshEnv(const zfs::path &root, bool grid, const char *nonce) {
    std::vector<std::string> env = {
        "HOME=" + (root / "home").string(),
        "ZDOTDIR=" + (root / (grid ? "zdot-grid" : "zdot-line")).string(),
        "PATH=/usr/bin:/bin", "TERM=dumb", "LANG=en_US.UTF-8"};
    if (nonce) env.push_back(std::string("MINICODE_NONCE=") + nonce);
    return env;
}

// The marks in `out`, as "133;A" and the like, in order: only those
// carrying `nonce` (with any other nonce, or none, a mark is "?").
std::vector<std::string> marksIn(const std::string &out, const std::string &nonce) {
    std::vector<std::string> marks;
    for (size_t at = out.find("\x1b]"); at != std::string::npos; at = out.find("\x1b]", at + 2)) {
        const size_t end = out.find('\a', at);
        if (end == std::string::npos) break;
        std::string body = out.substr(at + 2, end - at - 2);
        const std::string tail = ";mc=" + nonce;
        if (body.size() > tail.size() && body.compare(body.size() - tail.size(), tail.size(), tail) == 0)
            marks.push_back(body.substr(0, body.size() - tail.size()));
        else
            marks.push_back("?");
    }
    return marks;
}

std::string joinMarks(const std::vector<std::string> &marks) {
    std::string s;
    for (const auto &m : marks) {
        if (!s.empty()) s += ' ';
        s += m.compare(0, 2, "7;") == 0 ? std::string("7") : m;   // OSC 7 names the folder
    }
    return s;
}

// The line `key:<value>` a command printed (at a line's start or right
// after a mark), or "" when none did.
std::string printed(const std::string &out, const std::string &key) {
    const std::string k = key + ":";
    for (size_t at = out.find(k); at != std::string::npos; at = out.find(k, at + 1)) {
        if (at > 0 && out[at - 1] != '\n' && out[at - 1] != '\a') continue;
        const size_t from = at + k.size();
        const size_t end = out.find('\n', from);
        return out.substr(from, end == std::string::npos ? std::string::npos : end - from);
    }
    return "";
}

}  // namespace

void testZshIntegration() {
    GROUP("zsh:script");
    const std::string script = readFile("resources/minicode-zsh-integration.zsh");
    if (script.empty() || access("/bin/zsh", X_OK) != 0) {
        std::printf("  (/bin/zsh or resources/minicode-zsh-integration.zsh not found here; "
                    "zsh integration checks skipped)\n");
        return;
    }
    std::random_device rd;
    const zfs::path root = zfs::temp_directory_path() / ("minicode-zsh-" + std::to_string(rd()));
    zfs::create_directories(root / "home");
    writeIntegration(root, script, true);
    writeIntegration(root, script, false);
    {
        // It parses (zsh -n reads without running).
        ZshRun r = runZsh(root, {"-n", (root / "zdot-grid" / ShellIntegration::kZshScriptName).string()},
                          {"PATH=/usr/bin:/bin"}, "");
        CHECK(r.status == 0);
        if (r.status != 0) std::printf("    zsh -n: %s\n", r.err.c_str());
    }
    {
        // Each private key Terminal.mm sends is bound in all three keymaps
        // (the loop over emacs, viins and vicmd).
        auto bound = [&](const char *key, const char *widget) {
            std::string k = key;
            CHECK(k.size() > 1 && k[0] == '\x1b');
            const std::string line = "bindkey -M $m $'\\e" + k.substr(1) + "' " + widget;
            const bool ok = script.find(line) != std::string::npos;
            if (!ok) std::printf("    not bound: %s\n", line.c_str());
            return ok;
        };
        {
        // Each process's own integration folder (round 6's terminal defect
        // 6: every minicode wrote its copy into one shared folder, so two
        // running at once sourced each other's script), and which folders
        // the sweep at startup may take: only ones naming a process that
        // has gone.
        CHECK(ShellIntegration::folderName(true, 812, 1793900000) == "MiniCode-zsh-grid-812-1793900000");
        CHECK(ShellIntegration::folderName(false, 812, 1793900000) == "MiniCode-zsh-812-1793900000");
        long long pid = 0, start = 0;
        CHECK(ShellIntegration::parseFolderName("MiniCode-zsh-grid-812-1793900000", &pid, &start) &&
              pid == 812 && start == 1793900000);
        CHECK(ShellIntegration::parseFolderName("MiniCode-zsh-77-5", &pid, &start) && pid == 77 &&
              start == 5);
        for (const char *other : {"MiniCode-zsh-grid", "MiniCode-zsh", "MiniCode-zsh-grid-12",
                                  "MiniCode-zsh-grid-12-", "MiniCode-zsh-grid--5", "MiniCode-zsh-a-5",
                                  "MiniCode-zsh-grid-1-2-3", "MiniCode-latex", "MiniCode-zsh-0-5"})
            CHECK(!ShellIntegration::parseFolderName(other, &pid, &start));
        CHECK(ShellIntegration::ownerGone(100, 0));          // no such process
        CHECK(ShellIntegration::ownerGone(100, 500));        // the pid is someone else's now
        CHECK(!ShellIntegration::ownerGone(100, 100.4));     // still running
    }
    CHECK(script.find("for m in emacs viins vicmd; do") != std::string::npos);
        CHECK(bound(ShellIntegration::kZlePushKey, "__minicode_push"));
        CHECK(bound(ShellIntegration::kZleLineStartKey, "beginning-of-line"));
        CHECK(bound(ShellIntegration::kZleLineEndKey, "end-of-line"));
        CHECK(bound(ShellIntegration::kZleKillToStartKey, "backward-kill-line"));
        CHECK(bound(ShellIntegration::kZleClearKey, "clear-screen"));
    }

    GROUP("zsh:marks");
    {
        // Grid mode: no D before the first command, then for each command
        // E (its line), C, and D with its status, each prompt with OSC 7 and
        // A, every mark with the nonce. The user's .zshenv is read, with
        // ZDOTDIR handed back (unset: theirs is $HOME), and the nonce is
        // out of the environment.
        writeFile(root / "home" / ".zshenv", "print -r -- \"user-zshenv:${ZDOTDIR-unset}\"\n");
        ZshRun r = runZsh(root, {"-d", "-i"}, zshEnv(root, true, "n0nce"),
                          "echo one\nfalse\nprint -r -- \"nonce:${MINICODE_NONCE-unset}\"\n");
        std::remove((root / "home" / ".zshenv").c_str());
        CHECK(r.status == 0);
        CHECK(printed(r.out, "user-zshenv") == "unset");
        CHECK(printed(r.out, "nonce") == "unset");
        CHECK(r.out.find("\none\n") != std::string::npos ||
              r.out.find("\aone\n") != std::string::npos);
        // B: from precmd when the hook widget cannot be added, else from
        // ZLE, which a pipe never starts; either way only after an A.
        std::vector<std::string> m = marksIn(r.out, "n0nce");
        m.erase(std::remove(m.begin(), m.end(), std::string("133;B")), m.end());
        const std::string want =
            "7 133;A "
            "633;E;echo one 133;C 133;D;0 7 133;A "
            "633;E;false 133;C 133;D;1 7 133;A "
            "633;E;print -r -- \"nonce:${MINICODE_NONCE-unset}\" 133;C 133;D;0 7 133;A";
        CHECK(joinMarks(m) == want);
        if (joinMarks(m) != want) std::printf("    marks: %s\n    err: %s\n", joinMarks(m).c_str(), r.err.c_str());
    }
    {
        // A command line's control characters are blanked in E.
        ZshRun r = runZsh(root, {"-d", "-i"}, zshEnv(root, true, "n0nce"),
                          "echo $'a\\tb' # \x07" "bell\n");
        std::vector<std::string> m = marksIn(r.out, "n0nce");
        CHECK(std::find(m.begin(), m.end(), "633;E;echo $'a\\tb' #  bell") != m.end());
        CHECK(std::find(m.begin(), m.end(), "?") == m.end());
    }
    {
        // OSC 7 percent-encodes %, ; and control characters in the folder.
        const zfs::path odd = root / "a%b;c\x01" "d";
        zfs::create_directories(odd);
        ZshRun r = runZsh(root, {"-d", "-i"}, zshEnv(root, true, "n0nce"),
                          "cd " + root.string() + "/a%b\\;c$'\\x01'd\n");
        const std::string enc = "7;file://", tail = "/a%25b%3Bc%01d";
        bool found = false;
        for (const auto &mark : marksIn(r.out, "n0nce")) {
            if (mark.compare(0, enc.size(), enc) == 0 && mark.size() >= tail.size() &&
                mark.compare(mark.size() - tail.size(), tail.size(), tail) == 0)
                found = true;
        }
        CHECK(found);
        if (!found) std::printf("    marks: %s\n", joinMarks(marksIn(r.out, "n0nce")).c_str());
    }
    {
        // Without a nonce in the environment the marks carry none, so the
        // terminal believes none of them.
        ZshRun r = runZsh(root, {"-d", "-i"}, zshEnv(root, true, nullptr), "echo x\n");
        std::vector<std::string> m = marksIn(r.out, "n0nce");
        CHECK(!m.empty() && std::count(m.begin(), m.end(), "?") == (long)m.size());
    }
    {
        // Line mode: D at every prompt, the first included, no B or E,
        // and the prompt blanked (nothing but marks before the first
        // command's output on stderr's prompt).
        ZshRun r = runZsh(root, {"-d", "-i", "+Z"}, zshEnv(root, false, "n0nce"),
                          "echo one\nfalse\ntrue\n");
        CHECK(r.status == 0);
        const std::string want = "133;D;0 7 133;A 133;C 133;D;0 7 133;A 133;C 133;D;1 7 133;A "
                                 "133;C 133;D;0 7 133;A";
        CHECK(joinMarks(marksIn(r.out, "n0nce")) == want);
        if (joinMarks(marksIn(r.out, "n0nce")) != want)
            std::printf("    marks: %s\n    err: %s\n", joinMarks(marksIn(r.out, "n0nce")).c_str(),
                        r.err.c_str());
        CHECK(r.err.find("%") == std::string::npos);   // zsh's default prompt ends in %
    }

    GROUP("zsh:exec-nonce");
    {
        // What an exec's program gets, case by case, in the real shell
        // (__minicode_exec_env with exec's arguments, as the function named
        // exec calls it): ZDOTDIR back at the integration's folder with
        // MINICODE_EXEC, and the nonce only for a zsh that reads our
        // startup file and so takes it out of its environment: not one
        // that reads none (TE-D1), nor one emulating sh or ksh, which
        // reads .profile instead (round 6's terminal defect 1: `exec -a sh
        // zsh` and `exec zsh --emulate sh` kept it).
        struct Case { const char *args; bool nonce; };
        const Case cases[] = {
            {"zsh", true},
            {"/bin/zsh -i", true},
            {"-l zsh", true},
            {"-a myzsh zsh", true},
            {"-la myzsh zsh", true},
            {"-a -zsh zsh", true},
            {"-a csh zsh", true},          // csh emulation still reads .zshenv
            {"-- zsh", true},
            {"zsh -o rcs", true},
            {"zsh -f +f", true},
            {"zsh --no-rcs --rcs", true},
            {"zsh -i script.zsh -f", true},
            {"zsh -c \"echo -f\"", true},
            {"bash", false},
            {"claude", false},
            {"zsh -f", false},
            {"zsh -fi", false},
            {"zsh -if", false},
            {"zsh -fc true", false},
            {"zsh --no-rcs", false},
            {"zsh --norcs", false},
            {"zsh --no_rcs", false},
            {"zsh -o norcs", false},
            {"zsh -o NO_RCS", false},
            {"zsh -onorcs", false},
            {"zsh +o rcs", false},
            {"-l zsh +o rcs", false},
            {"zsh -i -o norcs", false},
            {"-a sh zsh", false},
            {"-a -sh zsh", false},
            {"-la ksh zsh", false},
            {"-a bash /bin/zsh", false},
            {"-a rsh zsh", false},
            {"zsh --emulate sh", false},
            {"zsh --emulate ksh -i", false},
            {"zsh --emulate=sh", false},
            {"zsh -i --emulate sh", false},  // anywhere among the options
        };
        std::string input;
        int n = 0;
        for (const Case &c : cases) {
            input += "__minicode_exec_env " + std::string(c.args) + "; print -r -- \"case" +
                     std::to_string(n) +
                     ":${MINICODE_NONCE-none}:${MINICODE_EXEC-none}:${ZDOTDIR-none}\"; "
                     "__minicode_exec_restore; print -r -- \"after" + std::to_string(n) +
                     ":${MINICODE_NONCE-none}:${MINICODE_EXEC-none}:${ZDOTDIR-none}\"\n";
            n++;
        }
        ZshRun r = runZsh(root, {"-d", "-i"}, zshEnv(root, true, "n0nce"), input);
        const std::string zdot = (root / "zdot-grid").string();
        n = 0;
        for (const Case &c : cases) {
            const std::string got = printed(r.out, "case" + std::to_string(n));
            const std::string want = std::string(c.nonce ? "n0nce" : "none") + ":1:" + zdot;
            CHECK(got == want);
            if (got != want) std::printf("    exec %s: %s (wanted %s)\n", c.args, got.c_str(), want.c_str());
            CHECK(printed(r.out, "after" + std::to_string(n)) == "none:none:none");
            n++;
        }
    }
    {
        // Which lines the hook gives a function named exec (which sets the
        // environment just before exec runs), which it sets the
        // environment for at once (exec after `builtin` skips functions;
        // only as the line's first command), and which neither. Round 6's
        // terminal defect 2: the hook exported the nonce from preexec, so
        // in `make && exec zsh` make had it, and anything make ran.
        struct Case { const char *line; bool fn, direct; };
        const Case cases[] = {
            {"exec zsh", true, false},
            {"make && exec zsh", true, false},
            {"npm test; exec zsh -l", true, false},
            {"cd /tmp && exec bash", true, false},
            {"noglob exec zsh", true, false},
            {"if true; then exec zsh; fi", true, false},
            {"{ exec zsh }", true, false},
            {"builtin exec zsh", false, true},
            {"command exec zsh", false, true},
            {"cd /tmp && builtin exec zsh", false, false},
            {"exec >log; exec zsh", false, false},     // a redirection's exec: no function
            {"exec 2>/dev/null", false, false},
            {"exec", false, false},
            {"echo exec zsh", false, false},
            {"(exec zsh)", false, false},
        };
        std::string input;
        int n = 0;
        for (const Case &c : cases) {
            input += "__minicode_exec_hook '" + std::string(c.line) + "'; print -r -- \"case" +
                     std::to_string(n) + ":${+functions[exec]}:${MINICODE_NONCE-none}:${MINICODE_EXEC-none}\"; "
                     "__minicode_exec_undo; print -r -- \"undone" + std::to_string(n) +
                     ":${+functions[exec]}:${MINICODE_NONCE-none}:${MINICODE_EXEC-none}:${ZDOTDIR-none}\"\n";
            n++;
        }
        ZshRun r = runZsh(root, {"-d", "-i"}, zshEnv(root, true, "n0nce"), input);
        n = 0;
        for (const Case &c : cases) {
            const std::string got = printed(r.out, "case" + std::to_string(n));
            const std::string want = std::string(c.fn ? "1" : "0") + ":" +
                                     (c.direct ? "n0nce:1" : "none:none");
            CHECK(got == want);
            if (got != want) std::printf("    %s: %s (wanted %s)\n", c.line, got.c_str(), want.c_str());
            CHECK(printed(r.out, "undone" + std::to_string(n)) == "0:none:none:none");
            n++;
        }
    }
    {
        // End to end, defect 2: what ran before the exec on its line had
        // none of it in its environment; the zsh it started did get the
        // nonce (marks with it, D;0 for the exec) and took it out again.
        const zfs::path envFile = root / "make-env.txt";
        ZshRun r = runZsh(root, {"-d", "-i"}, zshEnv(root, true, "n0nce"),
                          "/usr/bin/env > '" + envFile.string() + "' && exec /bin/zsh -i\n"
                          "print -r -- \"after:${MINICODE_NONCE-unset}\"\n");
        const std::string env = readFile(envFile.c_str());
        CHECK(!env.empty());
        CHECK(env.find("n0nce") == std::string::npos);
        CHECK(env.find("MINICODE_EXEC") == std::string::npos);
        CHECK(env.find("zdot-grid") == std::string::npos);
        if (env.find("n0nce") != std::string::npos) std::printf("    env before exec:\n%s\n", env.c_str());
        CHECK(printed(r.out, "after") == "unset");
        const std::string marks = joinMarks(marksIn(r.out, "n0nce"));
        CHECK(marks.find("133;C 133;D;0 7 133;A") != std::string::npos);
        CHECK(marks.find("?") == std::string::npos);
        if (marks.find("133;C 133;D;0 7 133;A") == std::string::npos)
            std::printf("    marks: %s\n    err: %s\n", marks.c_str(), r.err.c_str());
        // An exec that fails leaves the line going on with the shell's own
        // environment, and the function gone at the next prompt (zsh 5.9
        // exits instead, so nothing runs after it at all).
        std::remove(envFile.c_str());
        ZshRun f = runZsh(root, {"-d", "-i"}, zshEnv(root, true, "n0nce"),
                          "exec /nonexistent/zsh; /usr/bin/env > '" + envFile.string() + "'\n"
                          "print -r -- \"fn:${+functions[exec]}\"\n");
        const std::string env2 = readFile(envFile.c_str());
        CHECK(env2.find("n0nce") == std::string::npos && env2.find("MINICODE_EXEC") == std::string::npos);
        CHECK(printed(f.out, "fn") == "0" || f.out.find("fn:") == std::string::npos);
    }
    {
        // End to end, defect 1: a zsh in sh emulation (by its name, or
        // --emulate) never gets the nonce, since ours never runs in it to
        // take it out again.
        for (const char *line : {"exec -a sh /bin/zsh -i", "exec /bin/zsh --emulate sh -i"}) {
            ZshRun r = runZsh(root, {"-d", "-i"}, zshEnv(root, true, "n0nce"),
                              std::string(line) + "\nprint -r -- \"sh:${MINICODE_NONCE-unset}\"\n");
            CHECK(printed(r.out, "sh") == "unset");
            if (printed(r.out, "sh") != "unset")
                std::printf("    %s: %s\n    err: %s\n", line, printed(r.out, "sh").c_str(), r.err.c_str());
        }
    }
    {
        // End to end, TE-D1: `exec zsh -f` reads no startup files, so ours
        // cannot take the nonce out of its environment; it must never get
        // it. Before the fix the rc-less zsh printed the nonce here.
        ZshRun r = runZsh(root, {"-d", "-i"}, zshEnv(root, true, "n0nce"),
                          "exec /bin/zsh -f -i\nprint -r -- \"after-f:${MINICODE_NONCE-unset}\"\n");
        CHECK(printed(r.out, "after-f") == "unset");
        // And a zsh that does read them (`exec zsh -i`) takes it, loads the
        // integration, says the exec finished (D;0 at its first prompt) and
        // marks with the nonce; it is out of the environment again.
        ZshRun s = runZsh(root, {"-d", "-i"}, zshEnv(root, true, "n0nce"),
                          "exec /bin/zsh -i\nprint -r -- \"after:${MINICODE_NONCE-unset}\"\n");
        CHECK(printed(s.out, "after") == "unset");
        const std::string marks = joinMarks(marksIn(s.out, "n0nce"));
        CHECK(marks.find("633;E;exec /bin/zsh -i 133;C 133;D;0 7 133;A") != std::string::npos);
        CHECK(marks.find("?") == std::string::npos);
        if (marks.find("633;E;exec /bin/zsh -i 133;C 133;D;0 7 133;A") == std::string::npos)
            std::printf("    marks: %s\n    err: %s\n", marks.c_str(), s.err.c_str());
    }
    std::error_code ec;
    zfs::remove_all(root, ec);
}
