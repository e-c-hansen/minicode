// format_consent_tests.inc — the `format:inside-folder` and `format:consent`
// groups (Formatter.h: which formatters a folder supplies, and when they may
// run), included by run_tests.cpp.
#include "Formatter.h"

static void testFormatConsent() {
    using namespace Formatter;

    GROUP("format:inside-folder");
    {
        CHECK(insideFolder("/w/proj/.venv/bin/black", "/w/proj"));
        CHECK(insideFolder("/w/proj/.venv/bin/black", "/w/proj/"));
        CHECK(insideFolder("/w/proj", "/w/proj"));
        CHECK(insideFolder("/w/proj/node_modules/.bin/prettier", "/w"));
        CHECK(!insideFolder("/w/projx/.venv/bin/black", "/w/proj"));   // a sibling's prefix
        CHECK(!insideFolder("/w/pro", "/w/proj"));
        CHECK(!insideFolder("/opt/homebrew/bin/black", "/w/proj"));
        CHECK(insideFolder("/opt/homebrew/bin/black", "/"));           // a window on /
        CHECK(!insideFolder("/w/proj/.venv/bin/black", ""));           // no folder
        CHECK(!insideFolder("", "/w/proj"));
        CHECK(!insideFolder(".venv/bin/black", "/w/proj"));            // relative: never
    }

    GROUP("format:consent");
    {
        const std::string local = "/w/proj/.venv/bin/black", outside = "/opt/homebrew/bin/black";
        // Nothing in the folder: as before, the usual folders decide.
        CHECK(chooseProgram(false, "", outside, "") == Source::Outside);
        CHECK(chooseProgram(false, "", "", "") == Source::None);
        CHECK(chooseProgram(true, "", outside, "deny") == Source::Outside);
        // The project's own copy: asked about first, then as answered.
        CHECK(chooseProgram(false, local, outside, "") == Source::Ask);
        CHECK(chooseProgram(false, local, "", "") == Source::Ask);
        CHECK(chooseProgram(false, local, outside, "allow") == Source::Local);
        CHECK(chooseProgram(false, local, "", "allow") == Source::Local);
        CHECK(chooseProgram(false, local, outside, "deny") == Source::Outside);
        CHECK(chooseProgram(false, local, "", "deny") == Source::None);
        // format.<lang> in the settings: the user's own tool, never a
        // question while a copy outside the folder exists.
        CHECK(chooseProgram(true, local, outside, "") == Source::Outside);
        CHECK(chooseProgram(true, local, outside, "allow") == Source::Local);
        CHECK(chooseProgram(true, local, outside, "deny") == Source::Outside);
        CHECK(chooseProgram(true, local, "", "") == Source::Ask);
        CHECK(chooseProgram(true, local, "", "deny") == Source::None);
        // An answer this code does not know counts as none.
        CHECK(chooseProgram(false, local, outside, "maybe") == Source::Ask);
    }

    GROUP("format:config-runs-code");
    {
        auto readFrom = [](std::map<std::string, std::string> files) {
            return [files](const std::string &p) {
                auto it = files.find(p);
                return it == files.end() ? std::string() : it->second;
            };
        };
        auto none = readFrom({});
        // Every JavaScript and TypeScript name runs code, whatever is in it.
        for (const char *f : {"/w/.prettierrc.js", "/w/.prettierrc.cjs", "/w/.prettierrc.mjs",
                              "/w/.prettierrc.ts", "/w/.prettierrc.cts", "/w/.prettierrc.mts",
                              "/w/prettier.config.js", "/w/prettier.config.cjs",
                              "/w/prettier.config.mjs", "/w/prettier.config.ts",
                              "/w/prettier.config.cts", "/w/prettier.config.mts"}) {
            CHECK(configRunsCode("prettier", f, none));
            CHECK(configRunsCode("/opt/homebrew/bin/prettier", f, none));
        }
        // A JSON .prettierrc: data, unless it names plugins (also in an
        // override's options), a parser by path, or is a module's name.
        auto rc = [&](const std::string &text) {
            return configRunsCode("prettier", "/w/.prettierrc", readFrom({{"/w/.prettierrc", text}}));
        };
        CHECK(!rc("{}"));
        CHECK(!rc("{\"semi\": false, \"proseWrap\": \"always\", \"parser\": \"babel\"}"));
        CHECK(!rc("{\"plugins\": []}"));
        CHECK(rc("{\"plugins\": [\"./x.js\"]}"));
        CHECK(rc("{\"plugins\": [\"prettier-plugin-tailwindcss\"]}"));
        CHECK(rc("{\"overrides\": [{\"files\": \"*.md\", \"options\": {\"plugins\": [\"p\"]}}]}"));
        CHECK(rc("{\"parser\": \"./my-parser.js\"}"));
        CHECK(rc("\"@company/prettier-config\""));
        CHECK(!rc("{\n  // a comment\n  \"semi\": false,\n}\n"));   // comments, a trailing comma
        CHECK(rc("{\n  /* plugins: */\n  \"plugins\": [\"p\",],\n}\n"));
        // YAML, TOML and JSON5, read as text on the safe side.
        CHECK(!rc("semi: false\nsingleQuote: true\n"));
        CHECK(rc("semi: false\nplugins:\n  - ./x.js\n"));
        CHECK(rc("@company/prettier-config\n"));
        CHECK(rc("# shared\n\"@company/prettier-config\"\n"));
        CHECK(rc("parser: ./p.cjs\n"));
        CHECK(!configRunsCode("prettier", "/w/.prettierrc.toml",
                              readFrom({{"/w/.prettierrc.toml", "semi = false\ntabWidth = 4\n"}})));
        CHECK(configRunsCode("prettier", "/w/.prettierrc.toml",
                             readFrom({{"/w/.prettierrc.toml", "plugins = [\"./x.js\"]\n"}})));
        CHECK(!configRunsCode("prettier", "/w/.prettierrc.json5",
                              readFrom({{"/w/.prettierrc.json5", "{semi: false}\n"}})));
        // package.json's "prettier" key: an object as above, a string a module.
        auto pkg = [&](const std::string &text) {
            return configRunsCode("prettier", "/w/package.json", readFrom({{"/w/package.json", text}}));
        };
        CHECK(!pkg("{\"name\": \"x\", \"prettier\": {\"semi\": false}}"));
        CHECK(!pkg("{\"name\": \"x\", \"devDependencies\": {\"prettier\": \"3\"}}"));
        CHECK(pkg("{\"prettier\": {\"plugins\": [\"./x.js\"]}}"));
        CHECK(pkg("{\"prettier\": \"@company/prettier-config\"}"));
        // A package's own "plugins" key elsewhere is not Prettier's.
        CHECK(!pkg("{\"plugins\": [\"x\"], \"prettier\": {\"semi\": false}}"));
        auto yaml = [&](const std::string &text) {
            return configRunsCode("prettier", "/w/package.yaml", readFrom({{"/w/package.yaml", text}}));
        };
        CHECK(!yaml("name: x\nprettier:\n  semi: false\nplugins: [a]\n"));
        CHECK(yaml("name: x\nprettier:\n  plugins:\n    - ./x.js\n"));
        CHECK(yaml("name: x\nprettier: \"@company/prettier-config\"\n"));
        // Biome loads JavaScript and TypeScript plugins as well as GritQL
        // ones; extends is followed to files beside it, and a package's
        // configuration counts as code, being out of sight.
        auto biome = [&](std::map<std::string, std::string> files) {
            return configRunsCode("biome", "/w/biome.json", readFrom(files));
        };
        CHECK(!biome({{"/w/biome.json", "{\"formatter\": {\"indentStyle\": \"space\"}}"}}));
        CHECK(biome({{"/w/biome.json", "{\"plugins\": [\"./rule.grit\"]}"}}));
        CHECK(biome({{"/w/biome.json", "{\"plugins\": [\"./plugin.js\"]}"}}));
        CHECK(!biome({{"/w/biome.json", "{\"extends\": [\"./base.json\"]}"},
                      {"/w/base.json", "{\"formatter\": {}}"}}));
        CHECK(biome({{"/w/biome.json", "{\"extends\": [\"./base.json\"]}"},
                     {"/w/base.json", "{\"plugins\": [\"./p.js\"]}"}}));
        CHECK(biome({{"/w/biome.json", "{\"extends\": [\"./missing.json\"]}"}}));
        CHECK(biome({{"/w/biome.json", "{\"extends\": [\"@company/biome-config\"]}"}}));
        CHECK(!biome({{"/w/biome.json", "{\"extends\": \"//\"}"}, {"/biome.json", "{}"}}));
        CHECK(biome({{"/w/biome.json", "{\"extends\": \"//\"}"},
                     {"/biome.json", "{\"plugins\": [\"x.js\"]}"}}));
        CHECK(biome({{"/w/biome.json", "{\"extends\": [\"./a.json\"]}"},
                     {"/w/a.json", "{\"extends\": [\"./a.json\"]}"}}));   // a loop: refused
        CHECK(!configRunsCode("biome", "/w/biome.jsonc",
                              readFrom({{"/w/biome.jsonc", "// c\n{\"formatter\": {},}\n"}})));
        // latexindent: a substitution is Perl that latexindent evaluates.
        CHECK(!configRunsCode("latexindent", "/w/.latexindent.yaml",
                              readFrom({{"/w/.latexindent.yaml", "defaultIndent: \"  \"\n"}})));
        CHECK(configRunsCode("latexindent", "/w/localSettings.yaml",
                             readFrom({{"/w/localSettings.yaml",
                                        "switchesViaYaml:\n  rSwitch: 1\nreplacements:\n"
                                        "  - substitution: s/a/b/e\n"}})));
        // Everything else is data.
        CHECK(!configRunsCode("clang-format", "/w/.clang-format", none));
        CHECK(!configRunsCode("ruff", "/w/ruff.toml", none));
        CHECK(!configRunsCode("prettier", "", none));

        // Which file the tool reads: the nearest folder holding any of its
        // files, up to the open folder.
        auto fs = fakeFs({{"/w/prettier.config.js", ""}, {"/w/docs/README.md", "x"}});
        CHECK(codeConfigFile("prettier", "/w/docs/README.md", "/w", "", fs) ==
              "/w/prettier.config.js");
        auto nearer = fakeFs({{"/w/prettier.config.js", ""}, {"/w/docs/.prettierrc", "{}"}});
        CHECK(codeConfigFile("prettier", "/w/docs/README.md", "/w", "", nearer).empty());
        auto both = fakeFs({{"/w/.prettierrc", "{}"}, {"/w/.prettierrc.mjs", ""}});
        CHECK(codeConfigFile("prettier", "/w/a.md", "/w", "", both) == "/w/.prettierrc.mjs");
        // package.json without the key is not a configuration: the search goes on.
        auto dep = fakeFs({{"/w/docs/package.json", "{\"devDependencies\": {\"prettier\": \"3\"}}"},
                           {"/w/.prettierrc.cjs", ""}});
        CHECK(codeConfigFile("prettier", "/w/docs/a.md", "/w", "", dep) == "/w/.prettierrc.cjs");
        auto above = fakeFs({{"/prettier.config.js", ""}});
        CHECK(codeConfigFile("prettier", "/w/a.md", "/w", "", above).empty());   // the user's own
        auto biomeFs = fakeFs({{"/w/biome.json", "{\"plugins\": [\"./p.js\"]}"}});
        CHECK(codeConfigFile("biome", "/w/a.json", "/w", "", biomeFs) == "/w/biome.json");
        auto latex = fakeFs({{"/w/localSettings.yaml", "replacements:\n  - substitution: s/a/b/\n"}});
        CHECK(codeConfigFile("latexindent", "/w/a.tex", "/w", "/w/localSettings.yaml", latex) ==
              "/w/localSettings.yaml");
        CHECK(codeConfigFile("latexindent", "/w/a.tex", "/w", "", latex).empty());   // not given -l
        CHECK(codeConfigFile("clang-format", "/w/a.cc", "/w", "/w/.clang-format", fs).empty());
    }

    GROUP("format:consent-config");
    {
        const std::string local = "/w/node_modules/.bin/prettier",
                          outside = "/opt/homebrew/bin/prettier";
        // A configuration that runs code: asked about, even for a copy
        // outside the folder, and even for the user's own command.
        CHECK(chooseProgram(false, "", outside, "", true) == Source::Ask);
        CHECK(chooseProgram(true, "", outside, "", true) == Source::Ask);
        CHECK(chooseProgram(false, local, outside, "", true) == Source::Ask);
        // Allow runs as before; Don't Allow runs nothing, never the outside
        // copy reading the same file.
        CHECK(chooseProgram(false, "", outside, "allow", true) == Source::Outside);
        CHECK(chooseProgram(false, local, outside, "allow", true) == Source::Local);
        CHECK(chooseProgram(false, "", outside, "deny", true) == Source::Refused);
        CHECK(chooseProgram(false, local, outside, "deny", true) == Source::Refused);
        CHECK(chooseProgram(true, "", outside, "deny", true) == Source::Refused);
        // Nothing installed: nothing to ask about.
        CHECK(chooseProgram(false, "", "", "", true) == Source::None);
        // A plain .prettierrc with an outside Prettier still runs unasked.
        auto plain = fakeFs({{"/w/.prettierrc", "{\"semi\": false}"}});
        const bool code = !codeConfigFile("prettier", "/w/a.md", "/w", "/w/.prettierrc", plain).empty();
        CHECK(!code);
        CHECK(chooseProgram(false, "", outside, "", code) == Source::Outside);
    }

    GROUP("format:own-prettier");
    {
        // format.markdown = prettier: the user's own Prettier, as a choice
        // in the settings, with the prose left as it is.
        auto none = fakeFs({});
        Plan p = plan("markdown", "prettier", "/w/notes/a.md", "/w", none);
        CHECK(p.fromSettings && p.candidates.size() == 1 && p.configFile.empty());
        CHECK(p.candidates[0].program == "prettier" && !p.candidates[0].projectStyle);
        CHECK(p.candidates[0].command == "prettier --stdin-filepath {file} --prose-wrap preserve "
                                         "--config-precedence prefer-file");
        CHECK(p.installHint == "npm install -g prettier");
        CHECK(!serverMayFormat("markdown", p));
        CHECK(plan("markdown", " Prettier ", "/w/a.md", "/w", none).candidates.size() == 1);
        // A project set up for Prettier keeps its own style.
        auto pr = fakeFs({{"/w/.prettierrc", "{}"}});
        Plan q = plan("markdown", "prettier", "/w/a.md", "/w", pr);
        CHECK(q.fromSettings && q.candidates[0].projectStyle && q.configFile == "/w/.prettierrc" &&
              q.candidates[0].command == "prettier --stdin-filepath {file}");
        // JSON, YAML and CSS alike; C beside it is not Prettier's: the word
        // stays a command as written.
        for (const char *f : {"/w/a.json", "/w/a.yaml", "/w/a.scss", "/w/a.ts"})
            CHECK(plan(languageFor(f), "prettier", f, "/w", none).candidates[0].command.find(
                      "--prose-wrap preserve") != std::string::npos);
        Plan c = plan("cpp", "prettier", "/w/a.c", "/w", none);
        CHECK(c.fromSettings && c.candidates[0].command == "prettier");
        // A longer command is the user's own, run as written.
        CHECK(plan("markdown", "prettier --prose-wrap always --stdin-filepath {file}", "/w/a.md",
                   "/w", none).candidates[0].command ==
              "prettier --prose-wrap always --stdin-filepath {file}");
    }
}
