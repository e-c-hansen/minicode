// format_coverage_tests.inc — the `format:prettier-extensions`,
// `format:latexindent`, `format:fallback-rule` and `lsp:formatting` groups:
// Format Document for Markdown, JSON, YAML, CSS and LaTeX (Formatter.h),
// and a language server's formatting for files no formatter is planned
// for (LspClient.h). Included by run_tests.cpp, which has fakeFs,
// FakeServer and reply.
#include "Formatter.h"
#include "InstallHints.h"
#include "LspClient.h"

static void testFormatCoverage() {
    using namespace Formatter;
    auto none = fakeFs({});

    GROUP("format:prettier-extensions");
    {
        CHECK(languageFor("/w/README.md") == "markdown");
        CHECK(languageFor("/w/a.markdown") == "markdown");
        CHECK(languageFor("/w/package.json") == "json");
        CHECK(languageFor("/w/.vscode/settings.jsonc") == "json");
        CHECK(languageFor("/w/ci.yml") == "yaml" && languageFor("/w/ci.YAML") == "yaml");
        CHECK(languageFor("/w/a.css") == "css" && languageFor("/w/a.scss") == "css" &&
              languageFor("/w/a.less") == "css");
        CHECK(displayName("json") == "JSON" && displayName("latex") == "LaTeX");
        // No configuration: nothing planned, and nothing to install.
        for (const char *f : {"/w/README.md", "/w/a.json", "/w/ci.yml", "/w/a.css"}) {
            Plan p = plan(languageFor(f), "", f, "/w", none);
            CHECK(p.candidates.empty() && p.installHint.empty() && !p.off);
        }
        // A .prettierrc above the file: Prettier, with the project's style.
        auto pr = fakeFs({{"/w/.prettierrc", "{}"}});
        for (const char *f : {"/w/docs/README.md", "/w/a.json", "/w/a.jsonc", "/w/ci.yaml",
                              "/w/web/a.scss", "/w/a.less"}) {
            Plan p = plan(languageFor(f), "", f, "/w", pr);
            CHECK(p.candidates.size() == 1 && p.candidates[0].program == "prettier" &&
                  p.candidates[0].command == "prettier --stdin-filepath {file}" &&
                  p.candidates[0].projectStyle && p.configFile == "/w/.prettierrc" &&
                  p.installHint == "npm install -g prettier");
        }
        // Prettier as a dependency, or in node_modules, as for JavaScript.
        auto dep = fakeFs({{"/w/package.json", "{\"devDependencies\": {\"prettier\": \"3\"}}"}});
        CHECK(plan("markdown", "", "/w/README.md", "/w", dep).candidates.size() == 1);
        auto nm = fakeFs({{"/w/node_modules/.bin/prettier", ""}});
        CHECK(plan("yaml", "", "/w/a.yml", "/w", nm).candidates.size() == 1);
        // Biome formats the JSON and CSS of a project set up for it, not its
        // Markdown, YAML or JSON5.
        auto bio = fakeFs({{"/w/biome.json", "{}"}});
        Plan bj = plan("json", "", "/w/tsconfig.json", "/w", bio);
        CHECK(bj.candidates.size() == 1 && bj.candidates[0].program == "biome" &&
              bj.candidates[0].command == "biome format --stdin-file-path={file}" &&
              bj.configFile == "/w/biome.json");
        CHECK(plan("css", "", "/w/a.css", "/w", bio).candidates.size() == 1);
        CHECK(plan("css", "", "/w/a.scss", "/w", bio).candidates.empty());
        CHECK(plan("json", "", "/w/a.json5", "/w", bio).candidates.empty());
        CHECK(plan("markdown", "", "/w/README.md", "/w", bio).candidates.empty());
        CHECK(plan("yaml", "", "/w/a.yml", "/w", bio).candidates.empty());
        // Both in one folder: Prettier, which comes first, as for scripts.
        auto both = fakeFs({{"/w/biome.json", "{}"}, {"/w/.prettierrc", "{}"}});
        CHECK(plan("json", "", "/w/a.json", "/w", both).candidates[0].program == "prettier");
        // A command in the settings formats them anywhere.
        Plan s = plan("markdown", "prettier --stdin-filepath {file}", "/w/a.md", "/w", none);
        CHECK(s.fromSettings && s.candidates.size() == 1 && s.candidates[0].program == "prettier");
        CHECK(plan("yaml", "off", "/w/a.yml", "/w", pr).off);
        // The config search stops at the open folder: a .prettierrc above it
        // does not count.
        auto above = fakeFs({{"/.prettierrc", "{}"}});
        CHECK(plan("markdown", "", "/w/a.md", "/w", above).candidates.empty());
        // The JavaScript rule is unchanged: C++ beside a .prettierrc keeps
        // clang-format.
        CHECK(plan("cpp", "", "/w/a.cc", "/w", pr).candidates[0].program == "clang-format");
        // Install hints name the new uses.
        CHECK(Install::formatter("prettier")->gives.find("Markdown") != std::string::npos);
        CHECK(Install::formatter("biome")->gives.find("JSON") != std::string::npos);
        // The settings know the new keys.
        CHECK(parseSettings("format.markdown = prettier --stdin-filepath {file}\n")
                  .formatCommand("markdown") == "prettier --stdin-filepath {file}");
        Settings bad = parseSettings("format.toml = taplo fmt -\n");
        CHECK(bad.formatCommand("toml").empty());
    }

    GROUP("format:latexindent");
    {
        CHECK(languageFor("/w/paper.tex") == "latex" && languageFor("/w/my.sty") == "latex" &&
              languageFor("/w/my.cls") == "latex");
        CHECK(languageFor("/w/refs.bib").empty());
        CHECK(hasDefault("latex") && defaultCommand("latex") == "latexindent -g /dev/null");
        Plan d = plan("latex", "", "/w/paper/main.tex", "/w", none);
        CHECK(d.candidates.size() == 1 && d.candidates[0].program == "latexindent" &&
              !d.candidates[0].projectStyle && d.configFile.empty() &&
              d.installHint == "brew install latexindent");
        CHECK(arguments(d.candidates[0].command, "/w/paper/main.tex", 0, 0, nullptr,
                        d.configFile) ==
              (std::vector<std::string>{"latexindent", "-g", "/dev/null"}));
        // The project's settings, in the file's folder or above it, by path.
        for (const char *name : {".latexindent.yaml", "latexindent.yaml", "localSettings.yaml",
                                 ".localSettings.yaml"}) {
            std::string cfg = std::string("/w/my paper/") + name;
            Plan p = plan("latex", "", "/w/my paper/ch/one.tex", "/w", fakeFs({{cfg, "x: 1\n"}}));
            CHECK(p.candidates.size() == 1 && p.candidates[0].projectStyle &&
                  p.configFile == cfg);
            CHECK(arguments(p.candidates[0].command, "/w/my paper/ch/one.tex", 0, 0, nullptr,
                            p.configFile) ==
                  (std::vector<std::string>{"latexindent", "-g", "/dev/null", "-l=" + cfg}));
        }
        // No line ranges for it: the whole file.
        bool ranged = true;
        arguments("latexindent -g /dev/null", "/w/a.tex", 3, 9, &ranged);
        CHECK(!ranged);
        CHECK(Install::formatter("latexindent") &&
              Install::formatter("latexindent")->ways.front().command ==
                  "brew install latexindent");
        // {config} with no project file is empty, and {file} still works.
        CHECK(arguments("x {file} {config}", "/a b.tex") ==
              (std::vector<std::string>{"x", "/a b.tex", ""}));
    }

    GROUP("format:fallback-rule");
    {
        // A kind of file minicode formats nothing of: the server may.
        CHECK(serverMayFormat("", Plan()));
        // Markdown with no Prettier set up: the server may.
        CHECK(serverMayFormat("markdown", plan("markdown", "", "/w/a.md", "/w", none)));
        // The project configures Prettier (installed or not): never.
        auto pr = fakeFs({{"/w/.prettierrc", "{}"}});
        CHECK(!serverMayFormat("markdown", plan("markdown", "", "/w/a.md", "/w", pr)));
        CHECK(!serverMayFormat("json", plan("json", "", "/w/a.json", "/w",
                                            fakeFs({{"/w/biome.json", "{}"}}))));
        // A command in the settings, or the language turned off: never.
        CHECK(!serverMayFormat("markdown", plan("markdown", "mdformat -", "/w/a.md", "/w", none)));
        CHECK(!serverMayFormat("markdown", plan("markdown", "off", "/w/a.md", "/w", none)));
        CHECK(!serverMayFormat("yaml", plan("yaml", "off", "/w/a.yml", "/w", none)));
        // A language with a default formatter, installed or not: never (the
        // server would format Python, C++ or LaTeX in another style).
        for (const std::string &lang : {std::string("python"), std::string("cpp"),
                                        std::string("go"), std::string("rust"),
                                        std::string("shell"), std::string("latex")})
            CHECK(!serverMayFormat(lang, plan(lang, "", "/w/a", "/w", none)));
        // The notes.
        CHECK(unplannedMessage("", "toml") == "minicode has no formatter for .toml files.");
        CHECK(unplannedMessage("markdown", "md") ==
              "minicode formats Markdown files only in projects set up for Prettier, or when "
              "the settings say format.markdown = prettier (your own Prettier) or give a "
              "command of your own.");
        CHECK(unplannedMessage("css", "css").find("Prettier or Biome") != std::string::npos);
    }

    GROUP("lsp:formatting");
    {
        Json caps;
        CHECK(Json::parse("{\"documentFormattingProvider\": true,"
                          "\"documentRangeFormattingProvider\": {}}", caps));
        CHECK(Lsp::offers(caps, Lsp::Feature::Formatting) &&
              Lsp::offers(caps, Lsp::Feature::RangeFormatting));
        Json whole, nothing;
        CHECK(Json::parse("{\"documentFormattingProvider\": {\"workDoneProgress\": true}}", whole));
        CHECK(Json::parse("{\"documentFormattingProvider\": false}", nothing));
        CHECK(Lsp::offers(whole, Lsp::Feature::Formatting) &&
              !Lsp::offers(whole, Lsp::Feature::RangeFormatting));
        CHECK(!Lsp::offers(nothing, Lsp::Feature::Formatting));

        // Routing: running servers only, the main one first; a range goes
        // to a server that formats ranges when there is one.
        auto view = [](bool main, bool ready, const Json *c) {
            Lsp::ServerView v;
            v.main = main;
            v.ready = ready;
            v.capabilities = c;
            return v;
        };
        bool useRange = true;
        CHECK(Lsp::routeFormatting({}, false, &useRange) == -1 && !useRange);
        CHECK(Lsp::routeFormatting({view(true, true, &nothing)}, false, nullptr) == -1);
        CHECK(Lsp::routeFormatting({view(true, false, &caps)}, false, nullptr) == -1);   // starting
        CHECK(Lsp::routeFormatting({view(true, true, &whole), view(false, true, &caps)}, false,
                                   &useRange) == 0 && !useRange);
        CHECK(Lsp::routeFormatting({view(true, true, &whole), view(false, true, &caps)}, true,
                                   &useRange) == 1 && useRange);
        CHECK(Lsp::routeFormatting({view(false, true, &whole), view(true, true, &caps)}, false,
                                   &useRange) == 1 && !useRange);
        CHECK(Lsp::routeFormatting({view(true, true, &whole)}, true, &useRange) == 0 && !useRange);
        CHECK(Lsp::routeFormatting({view(true, true, &nothing), view(false, true, &whole)}, false,
                                   nullptr) == 1);

        // The answer as an edit for the applier: sorted, null as none,
        // overlaps refused.
        Json edits;
        CHECK(Json::parse("[{\"range\":{\"start\":{\"line\":2,\"character\":0},"
                          "\"end\":{\"line\":2,\"character\":1}},\"newText\":\"b\"},"
                          "{\"range\":{\"start\":{\"line\":0,\"character\":1},"
                          "\"end\":{\"line\":0,\"character\":1}},\"newText\":\" \"}]",
                          edits));
        Lsp::WorkspaceEdit we = Lsp::formattingEdit("file:///w/a.toml", edits);
        CHECK(we.problem.empty() && we.files.size() == 1 && we.editCount() == 2 &&
              we.files[0].uri == "file:///w/a.toml" && we.files[0].edits[0].range.start.line == 0);
        CHECK(Lsp::applyTextEdits(u"a=1\n\nx\n", we.files[0].edits) == u"a =1\n\nb\n");
        CHECK(Lsp::formattingEdit("file:///w/a.toml", Json()).editCount() == 0);
        CHECK(Lsp::formattingEdit("file:///w/a.toml", Json::array()).editCount() == 0);
        Json overlap;
        CHECK(Json::parse("[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
                          "\"end\":{\"line\":0,\"character\":3}},\"newText\":\"\"},"
                          "{\"range\":{\"start\":{\"line\":0,\"character\":1},"
                          "\"end\":{\"line\":0,\"character\":2}},\"newText\":\"y\"}]",
                          overlap));
        CHECK(!Lsp::formattingEdit("file:///w/a.toml", overlap).problem.empty());

        // The requests, with the file's indentation.
        FakeServer srv;
        Lsp::Client client([&](const std::string &b) { srv.take(b); });
        client.initialize("/w", 1);
        const Json &td = srv.sent[0]["params"]["capabilities"]["textDocument"];
        CHECK(td["formatting"].isObject() && !td["formatting"]["dynamicRegistration"].asBool(true));
        CHECK(td["rangeFormatting"].isObject());
        client.receive(reply(1, "{\"capabilities\":{\"documentFormattingProvider\":true,"
                                "\"documentRangeFormattingProvider\":true}}"));
        CHECK(client.offers(Lsp::Feature::Formatting) &&
              client.offers(Lsp::Feature::RangeFormatting));
        client.didOpen("file:///w/a.toml", "toml", "a=1\n");
        Json got;
        int id = client.formatting("file:///w/a.toml", 2, true,
                                   [&](const Json &r, const Json &) { got = r; });
        const Json &f = srv.sent.back();
        CHECK(f["method"].asString() == "textDocument/formatting" &&
              f["params"]["textDocument"]["uri"].asString() == "file:///w/a.toml" &&
              f["params"]["options"]["tabSize"].asInt() == 2 &&
              f["params"]["options"]["insertSpaces"].asBool());
        client.receive(reply(id, "[]"));
        CHECK(got.isArray());
        client.rangeFormatting("file:///w/a.toml", Lsp::Range{{1, 0}, {3, 0}}, 8, false,
                               [](const Json &, const Json &) {});
        const Json &rf = srv.sent.back();
        CHECK(rf["method"].asString() == "textDocument/rangeFormatting" &&
              rf["params"]["range"]["start"]["line"].asInt() == 1 &&
              rf["params"]["range"]["end"]["line"].asInt() == 3 &&
              rf["params"]["options"]["tabSize"].asInt() == 8 &&
              !rf["params"]["options"]["insertSpaces"].asBool(true));
        // A tab size of 0 (never expected) goes out as 4.
        client.formatting("file:///w/a.toml", 0, true, [](const Json &, const Json &) {});
        CHECK(srv.sent.back()["params"]["options"]["tabSize"].asInt() == 4);
    }
}
