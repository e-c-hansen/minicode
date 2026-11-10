// format_run_tests.inc — the `format-run:*` groups (FormatRun.h: which copy
// of which formatter runs, or why none does, what its exit means, the
// question, and the output as line runs in code points) and the
// `folder-tools:*` groups (FolderTools.h: the servers' question about the
// folder's Python environment, its words, and the answers file). Included
// by run_tests.cpp after format_consent_tests.inc, whose fakeFs it uses.
#include "FolderTools.h"
#include "FormatRun.h"

namespace {

// A machine: project files, runnable files, and what the program search
// finds by name.
struct FakeMachine {
    std::map<std::string, std::string> files;
    std::set<std::string> runnable;
    std::map<std::string, std::string> onPath;   // name -> path
    std::map<std::string, std::string> links;    // path -> resolved
    std::string virtualEnv;

    FormatRun::Env env() const {
        FormatRun::Env e;
        e.fs = fakeFs(files);
        const std::set<std::string> run = runnable;
        e.executable = [run](const std::string &p) { return run.count(p) > 0; };
        const std::map<std::string, std::string> path = onPath;
        e.find = [path, run](const std::string &name) -> std::string {
            if (name.find('/') != std::string::npos) return run.count(name) ? name : "";
            auto it = path.find(name);
            return it == path.end() ? std::string() : it->second;
        };
        const std::map<std::string, std::string> l = links;
        e.realPath = [l](const std::string &p) {
            auto it = l.find(p);
            return it == l.end() ? p : it->second;
        };
        e.virtualEnv = virtualEnv;
        e.home = "/home/me";
        e.askAgainMenu = "Edit";
        return e;
    }
};

}  // namespace

static void testFormatRun() {
    using FormatRun::Kind;

    GROUP("format-run:nothing-planned");
    {
        FakeMachine m;
        FormatRun::Decision d = FormatRun::decide("/w/notes.txt", "/w", "", "", 0, 0, m.env());
        CHECK(d.kind == Kind::Unavailable && d.serverMayFormat);
        CHECK(d.message == "minicode has no formatter for .txt files.");
        d = FormatRun::decide("/w/Makefile", "/w", "", "", 0, 0, m.env());
        CHECK(d.kind == Kind::Unavailable && d.message.find(".Makefile") != std::string::npos);
        // Markdown in a project not set up for Prettier: a server may.
        d = FormatRun::decide("/w/README.md", "/w", "", "", 0, 0, m.env());
        CHECK(d.kind == Kind::Unavailable && d.serverMayFormat);
        CHECK(d.message.find("Markdown files only in projects set up for Prettier") !=
              std::string::npos);
        // format.python = off: nothing, and no server either.
        d = FormatRun::decide("/w/a.py", "/w", "off", "", 0, 0, m.env());
        CHECK(d.kind == Kind::Unavailable && !d.serverMayFormat);
        CHECK(d.message == "Formatting is off for Python files (format.python = off in the "
                           "settings).");
    }

    GROUP("format-run:found");
    {
        FakeMachine m;
        m.runnable = {"/usr/bin/pyink", "/opt/bin/mine"};
        m.onPath = {{"pyink", "/usr/bin/pyink"}};
        // Google's default, found on PATH; {file} is the path as the window
        // spells it, the folder the file's.
        FormatRun::Decision d = FormatRun::decide("/w/pkg/a.py", "/w", "", "", 0, 0, m.env());
        CHECK(d.kind == Kind::Run && d.tool == "pyink" && d.style == "Google style");
        CHECK(!d.argv.empty() && d.argv[0] == "/usr/bin/pyink");
        CHECK(std::find(d.argv.begin(), d.argv.end(), "/w/pkg/a.py") != d.argv.end());
        CHECK(d.argv.back() == "-" && d.cwd == "/w/pkg" && !d.ranged);
        // Lines: pyink's --line-ranges before the closing "-".
        d = FormatRun::decide("/w/pkg/a.py", "/w", "", "", 3, 5, m.env());
        CHECK(d.ranged && d.argv.size() >= 2 && d.argv[d.argv.size() - 2] == "--line-ranges=3-5");
        // A command from the settings, by path.
        d = FormatRun::decide("/w/a.py", "/w", "/opt/bin/mine --x {file}", "", 0, 0, m.env());
        CHECK(d.kind == Kind::Run && d.tool == "mine" &&
              d.style == "format.python in the settings");
        CHECK((d.argv == std::vector<std::string>{"/opt/bin/mine", "--x", "/w/a.py"}));
        // The project's configuration names the style.
        m.files = {{"/w/.style.yapf", "[style]\n"}};
        m.runnable.insert("/usr/bin/yapf");
        m.onPath["yapf"] = "/usr/bin/yapf";
        d = FormatRun::decide("/w/a.py", "/w", "", "", 0, 0, m.env());
        CHECK(d.kind == Kind::Run && d.tool == "yapf" && d.style == "the project's .style.yapf");
    }

    GROUP("format-run:missing");
    {
        FakeMachine m;
        FormatRun::Decision d = FormatRun::decide("/w/a.py", "/w", "", "", 0, 0, m.env());
        CHECK(d.kind == Kind::Failed);
        CHECK(d.message == "No formatter for Python files was found (looked for pyink and yapf).");
        CHECK((d.installPrograms == std::vector<std::string>{"pyink", "yapf"}));
        d = FormatRun::decide("/w/a.py", "/w", "mcfmt --q", "", 0, 0, m.env());
        CHECK(d.message == "format.python in the settings runs mcfmt, which was not found.");
        m.files = {{"/w/pyproject.toml", "[tool.black]\n"}};
        d = FormatRun::decide("/w/a.py", "/w", "", "", 0, 0, m.env());
        CHECK(d.message == "This project configures black (in pyproject.toml), which is not "
                           "installed.");
    }

    GROUP("format-run:folder-copy");
    {
        FakeMachine m;
        m.files = {{"/w/pyproject.toml", "[tool.black]\n"}};
        m.runnable = {"/w/.venv/bin/black", "/usr/bin/black"};
        m.onPath = {{"black", "/usr/bin/black"}};
        // Not asked yet: ask, naming the folder's copy as the folder names it.
        FormatRun::Decision d = FormatRun::decide("/w/pkg/a.py", "/w", "", "", 0, 0, m.env());
        CHECK(d.kind == Kind::NeedsConsent && d.tool == "black" && d.consentFolder == ".venv" &&
              d.consentConfig.empty() && d.argv.empty());
        FormatRun::Question q = FormatRun::consentQuestion(d);
        CHECK(q.message == "Run black from this folder's .venv?");
        CHECK(q.detail.find("Programs in the folder run with your rights") == 0);
        CHECK(q.detail.find("also covers its Python environment") != std::string::npos);
        // Allowed: the folder's copy. Denied: the one outside, never another tool.
        d = FormatRun::decide("/w/pkg/a.py", "/w", "", "allow", 0, 0, m.env());
        CHECK(d.kind == Kind::Run && d.argv[0] == "/w/.venv/bin/black");
        d = FormatRun::decide("/w/pkg/a.py", "/w", "", "deny", 0, 0, m.env());
        CHECK(d.kind == Kind::Run && d.argv[0] == "/usr/bin/black");
        // Denied, and only the folder has it.
        m.runnable.erase("/usr/bin/black");
        m.onPath.clear();
        d = FormatRun::decide("/w/pkg/a.py", "/w", "", "deny", 0, 0, m.env());
        CHECK(d.kind == Kind::Failed);
        CHECK(d.message == "black is installed only in this folder's .venv, and this folder's own "
                           "programs are not allowed to run. Install black yourself to use it "
                           "here.");
        CHECK((d.installPrograms == std::vector<std::string>{"black"}));
        // The user's own command never asks while a copy outside exists.
        m.runnable = {"/w/.venv/bin/mcfmt", "/home/me/.local/bin/mcfmt"};
        m.onPath = {{"mcfmt", "/home/me/.local/bin/mcfmt"}};
        m.files.clear();
        d = FormatRun::decide("/w/a.py", "/w", "mcfmt", "", 0, 0, m.env());
        CHECK(d.kind == Kind::Run && d.argv[0] == "/home/me/.local/bin/mcfmt");
        // ... and asks when the folder's is the only one.
        m.runnable.erase("/home/me/.local/bin/mcfmt");
        m.onPath.clear();
        d = FormatRun::decide("/w/a.py", "/w", "mcfmt", "", 0, 0, m.env());
        CHECK(d.kind == Kind::NeedsConsent && d.consentFolder == ".venv");
        // $VIRTUAL_ENV, even inside the folder, is the user's: trusted.
        m.virtualEnv = "/w/.venv";
        d = FormatRun::decide("/w/a.py", "/w", "mcfmt", "", 0, 0, m.env());
        CHECK(d.kind == Kind::Run && d.argv[0] == "/w/.venv/bin/mcfmt");
        // A copy in the folder of a file opened from outside the window's
        // folder is never used.
        m.virtualEnv.clear();
        m.runnable = {"/elsewhere/.venv/bin/mcfmt"};
        d = FormatRun::decide("/elsewhere/a.py", "/w", "mcfmt", "allow", 0, 0, m.env());
        CHECK(d.kind == Kind::Failed && d.message.find("which was not found") != std::string::npos);
        // A folder reached through a link is planned on its resolved spelling.
        m.runnable = {"/real/w/.venv/bin/mcfmt"};
        m.links = {{"/w", "/real/w"}, {"/w/a.py", "/real/w/a.py"}};
        d = FormatRun::decide("/w/a.py", "/w", "mcfmt", "", 0, 0, m.env());
        CHECK(d.kind == Kind::NeedsConsent && d.consentFolder == ".venv");
    }

    GROUP("format-run:node-modules");
    {
        FakeMachine m;
        m.files = {{"/w/web/.prettierrc", "{}"}};
        m.runnable = {"/w/web/node_modules/.bin/prettier"};
        FormatRun::Decision d = FormatRun::decide("/w/web/src/a.js", "/w", "", "", 0, 0, m.env());
        CHECK(d.kind == Kind::NeedsConsent && d.tool == "prettier" &&
              d.consentFolder == "web/node_modules");
        d = FormatRun::decide("/w/web/src/a.js", "/w", "", "allow", 0, 0, m.env());
        CHECK(d.kind == Kind::Run && d.argv[0] == "/w/web/node_modules/.bin/prettier" &&
              d.style == "the project's .prettierrc");
        // npx with the folder's own prettier is the folder's program.
        m.runnable.insert("/usr/bin/npx");
        m.onPath = {{"npx", "/usr/bin/npx"}};
        d = FormatRun::decide("/w/web/README.md", "/w", "npx prettier --stdin-filepath {file}", "",
                              0, 0, m.env());
        CHECK(d.kind == Kind::NeedsConsent && d.tool == "prettier" &&
              d.consentFolder == "web/node_modules");
        d = FormatRun::decide("/w/web/README.md", "/w", "npx prettier --stdin-filepath {file}",
                              "deny", 0, 0, m.env());
        CHECK(d.kind == Kind::Failed &&
              d.message == "format.markdown runs npx, which would run this folder's own "
                           "web/node_modules, and this folder's own programs are not allowed "
                           "to run.");
        // Without the folder's copy npx is the user's program.
        m.runnable.erase("/w/web/node_modules/.bin/prettier");
        d = FormatRun::decide("/w/web/README.md", "/w", "npx prettier --stdin-filepath {file}", "",
                              0, 0, m.env());
        CHECK(d.kind == Kind::Run && d.argv[0] == "/usr/bin/npx");
    }

    GROUP("format-run:config-runs-code");
    {
        FakeMachine m;
        m.files = {{"/w/prettier.config.js", "module.exports = {};"}};
        m.runnable = {"/home/me/.local/bin/prettier"};
        m.onPath = {{"prettier", "/home/me/.local/bin/prettier"}};
        // Prettier installed outside: still the folder's program to ask about.
        FormatRun::Decision d = FormatRun::decide("/w/a.js", "/w", "", "", 0, 0, m.env());
        CHECK(d.kind == Kind::NeedsConsent && d.consentFolder.empty() &&
              d.consentConfig == "prettier.config.js");
        FormatRun::Question q = FormatRun::consentQuestion(d);
        CHECK(q.message == "Let prettier run the code in this folder's prettier.config.js?");
        CHECK(q.detail.find("prettier.config.js can run code when prettier reads it") == 0);
        // The user's own command too.
        d = FormatRun::decide("/w/notes.md", "/w", "prettier", "", 0, 0, m.env());
        CHECK(d.kind == Kind::NeedsConsent && d.consentConfig == "prettier.config.js");
        // Don't Allow runs nothing at all.
        d = FormatRun::decide("/w/a.js", "/w", "", "deny", 0, 0, m.env());
        CHECK(d.kind == Kind::Failed && d.tool == "prettier");
        CHECK(d.message == "prettier.config.js can run code when prettier reads it, and this "
                           "folder's own programs are not allowed to run, so prettier did not "
                           "run. Ask Again About This Folder's Tools (Edit menu) asks again.");
        d = FormatRun::decide("/w/a.js", "/w", "", "allow", 0, 0, m.env());
        CHECK(d.kind == Kind::Run && d.argv[0] == "/home/me/.local/bin/prettier");
        // Above the window's folder: by its whole path from "~".
        m.files = {{"/home/me/repo/prettier.config.js", "module.exports = {};"},
                   {"/home/me/repo/docs/README.md", "#Title\n"}};
        d = FormatRun::decide("/home/me/repo/docs/README.md", "/home/me/repo/docs", "prettier", "",
                              0, 0, m.env());
        CHECK(d.kind == Kind::NeedsConsent && d.consentConfig == "~/repo/prettier.config.js");
        q = FormatRun::consentQuestion(d);
        CHECK(q.message == "Let prettier run the code in ~/repo/prettier.config.js, above this "
                           "folder?");
        // A folder's copy and a configuration that runs code, together.
        m.files = {{"/w/prettier.config.js", "module.exports = {};"}};
        m.runnable.insert("/w/node_modules/.bin/prettier");
        d = FormatRun::decide("/w/a.js", "/w", "", "", 0, 0, m.env());
        q = FormatRun::consentQuestion(d);
        CHECK(q.message ==
              "Run prettier from this folder's node_modules, with its prettier.config.js?");
        // A plain .prettierrc is data: no question for a copy outside.
        m.files = {{"/w/.prettierrc", "{\"semi\": false}"}};
        m.runnable.erase("/w/node_modules/.bin/prettier");
        d = FormatRun::decide("/w/a.js", "/w", "", "", 0, 0, m.env());
        CHECK(d.kind == Kind::Run && d.argv[0] == "/home/me/.local/bin/prettier");
    }

    GROUP("format-run:outcome");
    {
        FormatRun::Outcome o =
            FormatRun::outcome("pyink", "/w/a.py", true, 10, "", -1, "", "", 5);
        CHECK(!o.ok && o.message == "pyink took longer than 10 seconds and was stopped. The file "
                                    "was not changed.");
        o = FormatRun::outcome("pyink", "/w/a.py", false, 10, "could not be started", -1, "", "", 5);
        CHECK(!o.ok && o.message == "pyink could not be started.");
        o = FormatRun::outcome("pyink", "/w/a.py", false, 10, "", 1, "",
                               "error: cannot format /w/a.py: line 3\n", 5);
        CHECK(!o.ok && o.message == "pyink could not format a.py:\nerror: cannot format a.py: line 3");
        o = FormatRun::outcome("pyink", "/w/a.py", false, 10, "", 2, "", "", 5);
        CHECK(!o.ok && o.message == "pyink exited with status 2.");
        o = FormatRun::outcome("pyink", "/w/a.py", false, 10, "", 0, std::string("x\xff\n"), "", 5);
        CHECK(!o.ok && o.message == "pyink wrote something that is not UTF-8 text. The file was "
                                    "not changed.");
        o = FormatRun::outcome("pyink", "/w/a.py", false, 10, "", 0, "\xed\xa0\x80", "", 5);
        CHECK(!o.ok);   // a surrogate's encoding is not UTF-8
        o = FormatRun::outcome("pyink", "/w/a.py", false, 10, "", 0, "", "", 5);
        CHECK(!o.ok && o.message == "pyink returned no text. The file was not changed.");
        o = FormatRun::outcome("pyink", "/w/a.py", false, 10, "", 0, "", "", 0);
        CHECK(o.ok && o.text.empty());   // an empty file stays empty
        o = FormatRun::outcome("pyink", "/w/a.py", false, 10, "", 0, "x = \"é\"\n", "", 5);
        CHECK(o.ok && o.text == "x = \"é\"\n");
        CHECK(FormatRun::alreadyFormatted("pyink", "Google style") ==
              "Already formatted (pyink, Google style).");
    }

    GROUP("format-run:replacement");
    {
        // Lines the formatter left alone are never touched; offsets are
        // code points (é is one, 😀 one, though two UTF-16 units).
        const std::string before = "# é😀\nx=1\nkeep\ny=2\n";
        const std::string after = "# é😀\nx = 1\nkeep\ny = 2\n";
        FormatRun::Replacement r(before, after);
        CHECK(r.changes().size() == 2);
        if (r.changes().size() == 2) {
            CHECK(r.changes()[0].start == 5 && r.changes()[0].length == 4 &&
                  r.changes()[0].text == "x = 1\n");
            CHECK(r.changes()[1].start == 14 && r.changes()[1].length == 4 &&
                  r.changes()[1].text == "y = 2\n");
        }
        // Applied by hand, last first, it gives the new text.
        std::u32string text;
        for (char32_t c : std::u32string(U"# é😀\nx=1\nkeep\ny=2\n")) text += c;
        std::u32string want = U"# é😀\nx = 1\nkeep\ny = 2\n";
        for (size_t i = r.changes().size(); i-- > 0;) {
            const FormatRun::Replacement::Change& c = r.changes()[i];
            std::u32string t;
            const std::u16string u = Lsp::toUtf16(c.text);
            for (char16_t ch : u) t += (char32_t)ch;   // ASCII here
            text.replace(c.start, c.length, t);
        }
        CHECK(text == want);
        // The caret after "x=" stays after "x =" (non-blank characters
        // counted within the run); one on a line left alone moves with it.
        CHECK(r.map(7) >= 7 && r.map(7) <= 9);
        CHECK(r.map(2) == 2);                 // in the first line, before é
        CHECK(r.map(9) == 11 && r.map(10) == 12);   // "keep", two later
        CHECK(r.map(before.size()) > 0);
        FormatRun::Replacement same("a\nb\n", "a\nb\n");
        CHECK(same.empty() && same.map(3) == 3);
    }

    GROUP("format-run:selected-lines");
    {
        int a = -1, b = -1;
        const std::string t = "a\nb\nc\nd\n";
        FormatRun::selectedLines(t, 2, 4, &a, &b);   // "b\n": line 2 alone
        CHECK(a == 2 && b == 2);
        FormatRun::selectedLines(t, 2, 5, &a, &b);   // "b\nc"
        CHECK(a == 2 && b == 3);
        FormatRun::selectedLines(t, 0, 8, &a, &b);   // everything
        CHECK(a == 1 && b == 4);
        FormatRun::selectedLines(t, 3, 3, &a, &b);   // nothing selected
        CHECK(a == 0 && b == 0);
        FormatRun::selectedLines("é\nü\nx", 2, 4, &a, &b);   // code points, not bytes
        CHECK(a == 2 && b == 2);
    }
}

static void testFolderTools() {
    GROUP("folder-tools:environment");
    {
        char tmpl[] = "/tmp/mc-folder-tools-XXXXXX";
        const char* made = mkdtemp(tmpl);
        CHECK(made != nullptr);
        if (made) {
            const std::string root = made;
            const std::string bin = root + "/.venv/bin";
            CHECK(std::system(("mkdir -p '" + bin + "' && touch '" + bin +
                               "/python' && chmod +x '" + bin + "/python' && mkdir -p '" + root +
                               "/pkg'").c_str()) == 0);
            PythonEnvs::Context ctx;
            ctx.fileDir = root + "/pkg";
            ctx.root = root;
            ctx.mac = false;
            CHECK(FolderTools::environmentToAsk(ctx, "", false) == ".venv");
            CHECK(FolderTools::environmentToAsk(ctx, "allow", false).empty());
            CHECK(FolderTools::environmentToAsk(ctx, "deny", false).empty());
            CHECK(FolderTools::environmentToAsk(ctx, "", true).empty());
            // Active where minicode started: trusted, not asked about.
            ctx.env["VIRTUAL_ENV"] = root + "/.venv";
            CHECK(FolderTools::environmentToAsk(ctx, "", false).empty());
            ctx.env.clear();
            // An answer this code does not know counts as none.
            CHECK(FolderTools::environmentToAsk(ctx, "maybe", false) == ".venv");
            CHECK(std::system(("rm -rf '" + root + "'").c_str()) == 0);
        }
        PythonEnvs::Context none;
        none.fileDir = "/nonexistent/x";
        none.root = "/nonexistent";
        CHECK(FolderTools::environmentToAsk(none, "", false).empty());
    }

    GROUP("folder-tools:words");
    {
        CHECK(FolderTools::environmentQuestion(".venv") ==
              "This folder has its own Python environment, .venv. The language servers run "
              "programs from it only if you allow it.");
        CHECK(FolderTools::environmentTooltip(".venv", "Edit").find("in the Edit menu") !=
              std::string::npos);
        CHECK(FolderTools::projectCodeQuestion("rust", true, ".cargo/config.toml") ==
              "This folder's .cargo/config.toml can name programs for cargo or rustup to run, so "
              "the Rust language server starts only if you allow it.");
        CHECK(FolderTools::projectCodeQuestion("rust", false, "").find("build scripts") !=
              std::string::npos);
        CHECK(FolderTools::projectCodeQuestion("typescript", true, "node_modules/typescript/lib")
                  .find("starts only if you allow it") != std::string::npos);
        CHECK(FolderTools::projectCodeQuestion("typescript", false, "node_modules/typescript/lib")
                  .find("uses yours instead") != std::string::npos);
        CHECK(FolderTools::projectCodeTooltip("Q.", "Edit").find("Q. Until then") == 0);
    }

    GROUP("folder-tools:file");
    {
        std::map<std::string, std::string> answers = {{"/w/a", "allow"}, {"/w/b \"q\"", "deny"}};
        const std::string text = FolderTools::encode(answers);
        CHECK(FolderTools::decode(text) == answers);
        CHECK(FolderTools::decode(FolderTools::encode({})).empty());
        // Only the two answers, under "folders"; anything else is no answer.
        CHECK((FolderTools::decode("{\"folders\": {\"/a\": \"allow\", \"/b\": \"maybe\", "
                                   "\"/c\": 1, \"\": \"deny\"}}") ==
               std::map<std::string, std::string>{{"/a", "allow"}}));
        CHECK(FolderTools::decode("not json").empty());
        CHECK(FolderTools::decode("[]").empty());
        CHECK(FolderTools::decode("{\"folders\": []}").empty());
        // A value encode would not write is left out.
        CHECK(FolderTools::decode(FolderTools::encode({{"/x", "maybe"}})).empty());
    }
}
