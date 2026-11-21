// shell_bash_tests.inc — the `shell:*` and `bash:*` groups of
// run_tests.cpp: the Linux port's bash integration
// (linux/resources/mutible-bash-integration.bash) in a real bash reading a
// pipe, as zsh_integration_tests.inc runs zsh (its runZsh, which this file
// follows into run_tests.cpp; skipped where /bin/bash is older than 4.4, as
// on the Mac). Its marks are the zsh script's (OSC 133, 633;E and 7 with
// the nonce), read as the session's host reads them: a TerminalStream with
// the nonce, its events applied to a TermShell (SessionHost.h's
// applyTermEvent). Included after zsh_integration_tests.inc.

namespace {

// The shell's state after each prompt and each command start in `out`, as
// the host would have it: "prompts started done status" (status "-" before
// the first command finishes) and, while a command runs, its text; joined
// with " | ".
std::string hostStates(const std::string &out, const std::string &nonce) {
    TerminalStream stream;
    stream.setEventsOnly(true);
    stream.setMarkNonce(nonce);
    Shared::TermShell s;
    std::string states;
    for (const TermEvent &e : stream.feed(out)) {
        Shared::applyTermEvent(s, e);
        if (e.kind != TermEvent::PromptStart && e.kind != TermEvent::CommandStart) continue;
        if (!states.empty()) states += " | ";
        states += std::to_string(s.prompts) + " " + std::to_string(s.started) + " " + std::to_string(s.done) +
                  " " + (s.status < 0 ? std::string("-") : std::to_string(s.status));
        if (s.busy) states += " " + s.command;
    }
    return states;
}

// /bin/bash's version as major * 100 + minor, 0 when there is none.
int bashVersion(const zfs::path &root) {
    ZshRun r = runZsh(root, {"-c", "echo $((BASH_VERSINFO[0] * 100 + BASH_VERSINFO[1]))"},
                      {"PATH=/usr/bin:/bin"}, "", "/bin/bash");
    return r.status == 0 ? std::atoi(r.out.c_str()) : 0;
}

}  // namespace

void testShellBash() {
    using namespace ShellIntegration;
    GROUP("shell:stub");
    {
        const unsigned char raw[] = {0x00, 0x0f, 0xa5, 0xff};
        CHECK(nonceHex(raw, 4) == "000fa5ff");
        CHECK(zshenvStub(true).find("typeset -g __mutible_mode=grid\n") != std::string::npos);
        CHECK(zshenvStub(false).find("typeset -g __mutible_mode=line\n") != std::string::npos);
        CHECK(zshenvStub(true).find("__mutible_term") == std::string::npos);
    }

    std::random_device rd;
    const zfs::path root = zfs::temp_directory_path() / ("mutible-shell-" + std::to_string(rd()));
    zfs::create_directories(root / "home");
    struct Cleanup {
        zfs::path p;
        ~Cleanup() { std::error_code ec; zfs::remove_all(p, ec); }
    } cleanup{root};

    GROUP("bash:integration");
    const std::string bashScript = readFile("linux/resources/mutible-bash-integration.bash");
    const int bashV = bashScript.empty() ? 0 : bashVersion(root);
    if (bashV < 404) {
        std::printf("  (no bash 4.4 or later at /bin/bash, or no "
                    "linux/resources/mutible-bash-integration.bash here; bash checks skipped)\n");
    } else {
        // As Terminal.cpp starts it: bash --rcfile <the script>, the nonce in
        // the environment. The user's .bashrc is read (its PROMPT_COMMAND
        // still sees each line's status, its alias works), the nonce leaves
        // the environment, and a line printing a mark with another nonce is
        // refused. stderr joins stdout first, since PS0 goes to stderr.
        writeFile(root / "script.bash", bashScript);
        writeFile(root / "home" / ".bashrc",
                  "exec 2>&1\nPROMPT_COMMAND='echo \"user-pc:$?\"'\nalias hi='echo hi-alias'\n");
        const std::vector<std::string> env = {"HOME=" + (root / "home").string(), "PATH=/usr/bin:/bin",
                                              "TERM=dumb", "MUTIBLE_NONCE=n0nce"};
        ZshRun r = runZsh(root, {"--rcfile", (root / "script.bash").string(), "-i"}, env,
                          "hi\nfalse\ntrue; true; true\n"
                          "printf '\\e]133;A;mc=bad\\a\\e]133;D;9\\a'\n"
                          "echo \"nonce:${MUTIBLE_NONCE-unset}\"\n"
                          "echo 'a;b\\c'\n",
                          "/bin/bash");
        CHECK(printed(r.out, "user-pc") == "0");
        CHECK(r.out.find("user-pc:1") != std::string::npos);
        CHECK(r.out.find("hi-alias") != std::string::npos);
        CHECK(r.out.find("nonce:unset") != std::string::npos);
        const std::string want =
            "1 0 0 - | 1 1 0 - hi | 2 1 1 0 | 2 2 1 0 false | 3 2 2 1 | 3 3 2 1 true; true; true | "
            "4 3 3 0 | 4 4 3 0 printf '\\e]133;A;mc=bad\\a\\e]133;D;9\\a' | "
            "5 4 4 0 | 5 5 4 0 echo \"nonce:${MUTIBLE_NONCE-unset}\" | 6 5 5 0 | "
            "6 6 5 0 echo 'a;b\\c' | 7 6 6 0";
        const std::string got = hostStates(r.out, "n0nce");
        CHECK(got == want);
        if (got != want) std::printf("    bash states: %s\n    out: %s\n", got.c_str(), r.out.c_str());
        // The folder at each prompt, with the nonce (OSC 7).
        TerminalStream folders;
        folders.setEventsOnly(true);
        folders.setMarkNonce("n0nce");
        std::string cwd;
        for (const TermEvent &e : folders.feed(r.out))
            if (e.kind == TermEvent::Directory) cwd = e.text;
        CHECK(!cwd.empty() && cwd[0] == '/');

        // vte.sh's own OSC 133 marks around PS1 (as /etc/bash.bashrc leaves
        // them when no .bashrc sets PS1 after it) come out, the prompt's own
        // text stays; without a nonce they are left as they are.
        writeFile(root / "home" / ".bashrc",
                  "exec 2>&1\nPS1='\\[\\e]133;D;$?\\e\\\\\\e]133;A\\e\\\\\\]'"
                  "'my-prompt\\$ ''\\[\\e]133;B\\e\\\\\\]'\n");
        const std::string ps1Line = "printf 'ps1<%s>\\n' \"$PS1\"\n";
        ZshRun vte = runZsh(root, {"--rcfile", (root / "script.bash").string(), "-i"}, env, ps1Line,
                            "/bin/bash");
        CHECK(vte.out.find("ps1<my-prompt\\$ >") != std::string::npos);
        if (vte.out.find("ps1<my-prompt\\$ >") == std::string::npos)
            std::printf("    vte prompt: %s\n", vte.out.c_str());
        ZshRun vteNone = runZsh(root, {"--rcfile", (root / "script.bash").string(), "-i"},
                                {env[0], env[1], env[2]}, ps1Line, "/bin/bash");
        CHECK(vteNone.out.find("133;B") != std::string::npos);
        writeFile(root / "home" / ".bashrc",
                  "exec 2>&1\nPROMPT_COMMAND='echo \"user-pc:$?\"'\nalias hi='echo hi-alias'\n");

        // Without a nonce there are no hooks at all (and nothing printed).
        ZshRun none = runZsh(root, {"--rcfile", (root / "script.bash").string(), "-i"},
                             {env[0], env[1], env[2]}, "true\n", "/bin/bash");
        CHECK(none.out.find("\x1b]133;") == std::string::npos);
        CHECK(none.out.find("user-pc:0") != std::string::npos);
    }

}
