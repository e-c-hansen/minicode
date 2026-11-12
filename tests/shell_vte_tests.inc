// shell_vte_tests.inc — the `shell:*`, `bash:*` and `zsh:vte` groups of
// run_tests.cpp: the Linux port's shell integration (ShellIntegration.h's
// VTE half, linux/PARITY.md D10). The termprop value's parser and the state
// it keeps; then the scripts in a real shell reading a pipe, as
// zsh_integration_tests.inc runs zsh (its runZsh, which this file follows
// into run_tests.cpp): bash with linux/resources/minicode-bash-integration.bash
// (skipped where /bin/bash is older than 4.4, as on the Mac), and zsh with
// the .zshenv's VTE line where /bin/zsh exists.

namespace {

// The termprop's values in `out` as VTE would hold them: each
// ESC ] 666 ; vte.ext.minicode.mark=... ESC \ with \s, \\ and \n unescaped,
// a bare ';' ending the value (VTE splits the OSC there).
std::vector<std::string> vteValuesIn(const std::string &out) {
    std::vector<std::string> values;
    const std::string head = "\x1b]666;vte.ext.minicode.mark=";
    for (size_t at = out.find(head); at != std::string::npos; at = out.find(head, at + 1)) {
        const size_t from = at + head.size();
        const size_t end = out.find("\x1b\\", from);
        if (end == std::string::npos) break;
        std::string v;
        for (size_t i = from; i < end; i++) {
            if (out[i] == ';') break;
            if (out[i] == '\\' && i + 1 < end) {
                const char c = out[++i];
                v += c == 's' ? ';' : c == 'n' ? '\n' : c;
            } else {
                v += out[i];
            }
        }
        values.push_back(v);
    }
    return values;
}

// Each value as the app reads it: "p x d status command" (status "-" before
// the first command finishes, the command only while busy), "?" when the
// value is refused; joined with " | ".
std::string vteStates(const std::vector<std::string> &values, const std::string &nonce) {
    std::string s;
    for (const std::string &v : values) {
        if (!s.empty()) s += " | ";
        ShellIntegration::VteMark m;
        if (!ShellIntegration::parseVteMark(v, nonce, &m)) {
            s += "?";
            continue;
        }
        ShellIntegration::ShellState st;
        st.apply(m);
        s += std::to_string(st.prompts) + " " + std::to_string(st.started) + " " +
             std::to_string(st.done) + " " + (st.status < 0 ? std::string("-") : std::to_string(st.status));
        if (st.busy) s += " " + st.command;
    }
    return s;
}

// /bin/bash's version as major * 100 + minor, 0 when there is none.
int bashVersion(const zfs::path &root) {
    ZshRun r = runZsh(root, {"-c", "echo $((BASH_VERSINFO[0] * 100 + BASH_VERSINFO[1]))"},
                      {"PATH=/usr/bin:/bin"}, "", "/bin/bash");
    return r.status == 0 ? std::atoi(r.out.c_str()) : 0;
}

}  // namespace

void testShellVte() {
    using namespace ShellIntegration;
    GROUP("shell:vte-mark");
    {
        VteMark m;
        CHECK(parseVteMark("p=3;x=2;d=1;s=;c=ls -l;mc=n0nce", "n0nce", &m));
        CHECK(m.prompts == 3 && m.started == 2 && m.done == 1 && m.status == -1);
        CHECK(m.command == "ls -l");
        // The command may hold anything, ";mc=" too: the nonce is the end.
        CHECK(parseVteMark("p=1;x=1;d=0;s=0;c=echo ;mc=evil;s=7;mc=n0nce", "n0nce", &m));
        CHECK(m.command == "echo ;mc=evil;s=7" && m.status == 0);
        CHECK(parseVteMark("p=12;x=40;d=40;s=130;c=;mc=n0nce", "n0nce", &m));
        CHECK(m.prompts == 12 && m.started == 40 && m.status == 130 && m.command.empty());
        // Refused: another nonce, none, a nonce the shell does not have, a
        // value past the nonce, and anything malformed.
        CHECK(!parseVteMark("p=3;x=2;d=1;s=;c=x;mc=other", "n0nce", &m));
        CHECK(!parseVteMark("p=3;x=2;d=1;s=;c=x", "n0nce", &m));
        CHECK(!parseVteMark("p=3;x=2;d=1;s=;c=x;mc=", "", &m));
        CHECK(!parseVteMark("p=3;x=2;d=1;s=;c=x;mc=n0nce2", "n0nce", &m));
        CHECK(!parseVteMark("p=3;x=2;d=1;s=;c=x;mc=n0nce;", "n0nce", &m));
        CHECK(!parseVteMark("p=;x=2;d=1;s=;c=x;mc=n0nce", "n0nce", &m));
        CHECK(!parseVteMark("p=3;x=2a;d=1;s=;c=x;mc=n0nce", "n0nce", &m));
        CHECK(!parseVteMark("p=3;d=1;x=2;s=;c=x;mc=n0nce", "n0nce", &m));
        CHECK(!parseVteMark("p=3;x=2;d=1;s=256;c=x;mc=n0nce", "n0nce", &m));
        CHECK(!parseVteMark("p=3;x=2;d=1;s=;mc=n0nce", "n0nce", &m));
        CHECK(!parseVteMark("p=9999999999999999;x=2;d=1;s=;c=;mc=n0nce", "n0nce", &m));
        CHECK(!parseVteMark("", "n0nce", &m));
        CHECK(!parseVteMark(";mc=n0nce", "n0nce", &m));
        const unsigned char raw[] = {0x00, 0x0f, 0xa5, 0xff};
        CHECK(nonceHex(raw, 4) == "000fa5ff");
    }
    GROUP("shell:vte-state");
    {
        ShellState st;
        CHECK(!st.known && !st.busy && !st.atPrompt && st.status == -1);
        VteMark m;
        CHECK(parseVteMark("p=1;x=0;d=0;s=;c=;mc=k", "k", &m));
        st.apply(m);
        CHECK(st.known && st.atPrompt && !st.busy && st.command.empty() && st.status == -1);
        CHECK(parseVteMark("p=1;x=1;d=0;s=;c=sleep 2;mc=k", "k", &m));
        st.apply(m);
        CHECK(st.busy && !st.atPrompt && st.command == "sleep 2");
        // zsh's prompt marks still carry the last command's text: not running.
        CHECK(parseVteMark("p=2;x=1;d=1;s=0;c=sleep 2;mc=k", "k", &m));
        st.apply(m);
        CHECK(!st.busy && st.atPrompt && st.command.empty() && st.status == 0);
        // A lost prompt mark shows as a jump; the newest value decides.
        CHECK(parseVteMark("p=5;x=6;d=4;s=1;c=make;mc=k", "k", &m));
        st.apply(m);
        CHECK(st.busy && st.command == "make" && st.status == 1 && st.prompts == 5);
    }
    GROUP("shell:zshenv-vte");
    {
        CHECK(zshenvStub(true, true).find("typeset -g __minicode_term=vte\n") != std::string::npos);
        CHECK(zshenvStub(true).find("__minicode_term") == std::string::npos);
        CHECK(zshenvStub(true, true).find("typeset -g __minicode_mode=grid\n") != std::string::npos);
    }

    std::random_device rd;
    const zfs::path root = zfs::temp_directory_path() / ("minicode-shell-" + std::to_string(rd()));
    zfs::create_directories(root / "home");
    struct Cleanup {
        zfs::path p;
        ~Cleanup() { std::error_code ec; zfs::remove_all(p, ec); }
    } cleanup{root};

    GROUP("bash:integration");
    const std::string bashScript = readFile("linux/resources/minicode-bash-integration.bash");
    const int bashV = bashScript.empty() ? 0 : bashVersion(root);
    if (bashV < 404) {
        std::printf("  (no bash 4.4 or later at /bin/bash, or no "
                    "linux/resources/minicode-bash-integration.bash here; bash checks skipped)\n");
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
                                              "TERM=dumb", "MINICODE_NONCE=n0nce"};
        ZshRun r = runZsh(root, {"--rcfile", (root / "script.bash").string(), "-i"}, env,
                          "hi\nfalse\ntrue; true; true\n"
                          "printf '\\e]666;vte.ext.minicode.mark=p=9\\\\sx=9\\\\sd=0\\\\ss=\\\\sc=forged\\\\smc=bad\\e\\\\'\n"
                          "echo \"nonce:${MINICODE_NONCE-unset}\"\n"
                          "echo 'a;b\\c'\n",
                          "/bin/bash");
        CHECK(printed(r.out, "user-pc") == "0");
        CHECK(r.out.find("user-pc:1") != std::string::npos);
        CHECK(r.out.find("hi-alias") != std::string::npos);
        CHECK(r.out.find("nonce:unset") != std::string::npos);
        const std::string want =
            "1 0 0 - | 1 1 0 - hi | 2 1 1 0 | 2 2 1 0 false | 3 2 2 1 | 3 3 2 1 true; true; true | "
            "4 3 3 0 | "
            "4 4 3 0 printf '\\e]666;vte.ext.minicode.mark=p=9\\\\sx=9\\\\sd=0\\\\ss=\\\\sc=forged\\\\smc=bad\\e\\\\' | ? | "
            "5 4 4 0 | 5 5 4 0 echo \"nonce:${MINICODE_NONCE-unset}\" | 6 5 5 0 | "
            "6 6 5 0 echo 'a;b\\c' | 7 6 6 0";
        const std::string got = vteStates(vteValuesIn(r.out), "n0nce");
        CHECK(got == want);
        if (got != want) std::printf("    bash states: %s\n    out: %s\n", got.c_str(), r.out.c_str());

        // Without a nonce there are no hooks at all (and nothing printed).
        ZshRun none = runZsh(root, {"--rcfile", (root / "script.bash").string(), "-i"},
                             {env[0], env[1], env[2]}, "true\n", "/bin/bash");
        CHECK(vteValuesIn(none.out).empty());
        CHECK(none.out.find("user-pc:0") != std::string::npos);
    }

    GROUP("zsh:vte");
    const std::string zshScript = readFile("resources/minicode-zsh-integration.zsh");
    if (zshScript.empty() || access("/bin/zsh", X_OK) != 0) {
        std::printf("  (/bin/zsh not found here; zsh's VTE marks not checked)\n");
        return;
    }
    {
        // The Linux port's .zshenv (grid mode on VTE): one termprop at each
        // prompt and each command, never OSC 133 or 633, OSC 7 plain.
        const zfs::path zdot = root / "zdot-vte";
        writeFile(zdot / kZshScriptName, zshScript);
        writeFile(zdot / ".zshenv", zshenvStub(true, true));
        ZshRun r = runZsh(root, {"-d", "-i"},
                          {"HOME=" + (root / "home").string(), "ZDOTDIR=" + zdot.string(), "PATH=/usr/bin:/bin",
                           "TERM=dumb", "LANG=en_US.UTF-8", "MINICODE_NONCE=n0nce"},
                          "echo one\nfalse\nprint -r -- 'a;b\\c'\n");
        const std::string want = "1 0 0 - | 1 1 0 - echo one | 2 1 1 0 | 2 2 1 0 false | 3 2 2 1 | "
                                 "3 3 2 1 print -r -- 'a;b\\c' | 4 3 3 0";
        const std::string got = vteStates(vteValuesIn(r.out), "n0nce");
        CHECK(got == want);
        if (got != want) std::printf("    zsh states: %s\n    err: %s\n", got.c_str(), r.err.c_str());
        CHECK(r.out.find("\x1b]133;") == std::string::npos && r.out.find("\x1b]633;") == std::string::npos);
        const size_t osc7 = r.out.find("\x1b]7;file://");
        CHECK(osc7 != std::string::npos);
        if (osc7 != std::string::npos) {
            const size_t bel = r.out.find('\a', osc7);
            CHECK(bel != std::string::npos && r.out.substr(osc7, bel - osc7).find(";mc=") == std::string::npos);
        }
    }
}
