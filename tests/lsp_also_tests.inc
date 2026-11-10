// lsp_also_tests.inc — the `lsp:also-*` groups of run_tests.cpp: a second
// language server per language (Ruff's beside basedpyright), its setting,
// routing requests between the servers, and merging what they say.
// Included there, after the fake server and the Json helpers.

static void testLspSecondServer() {
    GROUP("lsp:also-keys");
    CHECK(Lsp::alsoKey("python") == "python.also");
    CHECK(Lsp::isAlsoKey("python.also") && !Lsp::isAlsoKey("python") &&
          !Lsp::isAlsoKey(".also") && !Lsp::isAlsoKey("also"));
    CHECK(Lsp::languageOfKey("python.also") == "python" && Lsp::languageOfKey("cpp") == "cpp");
    CHECK(Lsp::defaultAlsoCommand("python", true) == "ruff server");
    CHECK(Lsp::defaultAlsoCommand("python", false).empty());
    CHECK(Lsp::defaultAlsoCommand("cpp", true).empty());
    CHECK(Lsp::programDisplayName("/x/bin/ruff") == "ruff");
    CHECK(Lsp::programDisplayName("/x/basedpyright-langserver") == "basedpyright");
    CHECK(Lsp::programDisplayName("clangd") == "clangd");

    GROUP("lsp:also-settings");
    {
        std::vector<SettingsError> errs;
        Settings s = parseSettings("lsp.python.also = ruff server --preview  # mine\n"
                                   "lsp.cpp.also = off\n"
                                   "lsp.go.also = auto\n"
                                   "lsp.cobol.also = x\n", &errs);
        CHECK(s.lspCommand("python.also") == "ruff server --preview");
        CHECK(!s.lspOff("python.also") && s.lspOff("cpp.also"));
        CHECK(s.lspCommand("go.also").empty() && !s.lspOff("go.also"));
        CHECK(s.lspCommand("python").empty() && !s.lspOff("python"));
        // cobol is not built in: a server of one's own needs its extensions.
        CHECK(errs.size() == 1 && errs[0].line == 4 &&
              errs[0].message.find("lsp.cobol.extensions") != std::string::npos);
        // auto after a command puts the built-in rule back.
        Settings t = parseSettings("lsp.python.also = ruff server\nlsp.python.also = auto\n");
        CHECK(t.lspCommand("python.also").empty());
        // The documented line is the default.
        Settings d;
        std::string doc = Settings::defaultFileText();
        CHECK(doc.find("# lsp.python.also = auto\n") != std::string::npos);
        CHECK(d.lspCommand("python.also").empty());
    }

    GROUP("lsp:also-ruff-config");
    {
        std::map<std::string, std::string> files;
        Formatter::Fs fs{[&](const std::string &p) { return files.count(p) > 0; },
                         [&](const std::string &p) {
                             auto it = files.find(p);
                             return it == files.end() ? std::string() : it->second;
                         }};
        const std::string root = "/w", file = "/w/pkg/sub/a.py";
        CHECK(Formatter::configFile("ruff", file, root, fs).empty());
        // Lint settings alone count, in the open folder.
        files["/w/pyproject.toml"] = "[project]\nname = \"x\"\n\n[tool.ruff.lint]\nselect = [\"I\"]\n";
        CHECK(Formatter::configFile("ruff", file, root, fs) == "/w/pyproject.toml");
        // A nearer folder that configures another formatter does not hide it.
        files["/w/pkg/pyproject.toml"] = "[tool.black]\nline-length = 88\n";
        CHECK(Formatter::configFile("ruff", file, root, fs) == "/w/pyproject.toml");
        CHECK(Formatter::configFile("black", file, root, fs) == "/w/pkg/pyproject.toml");
        // The nearest Ruff file wins.
        files["/w/pkg/sub/.ruff.toml"] = "";
        CHECK(Formatter::configFile("ruff", file, root, fs) == "/w/pkg/sub/.ruff.toml");
        files.erase("/w/pkg/sub/.ruff.toml");
        files["/w/pkg/ruff.toml"] = "line-length = 100\n";
        CHECK(Formatter::configFile("ruff", file, root, fs) == "/w/pkg/ruff.toml");
        // A repository's top ends the search, as for Format Document.
        files.erase("/w/pkg/ruff.toml");
        files["/w/pkg/.git"] = "";
        CHECK(Formatter::configFile("ruff", file, root, fs).empty());
        // A table that only mentions ruff in its name does not count.
        files.erase("/w/pkg/.git");
        files["/w/pyproject.toml"] = "[tool.ruffle]\n";
        CHECK(Formatter::configFile("ruff", file, root, fs).empty());
        CHECK(Formatter::configFile("nosuchtool", file, root, fs).empty());
    }

    GROUP("lsp:also-routing");
    {
        Json pyright = parsed("{\"completionProvider\":{},\"hoverProvider\":true,"
                              "\"definitionProvider\":true,\"referencesProvider\":true,"
                              "\"renameProvider\":{\"prepareProvider\":true},"
                              "\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\"]},"
                              "\"codeActionProvider\":{\"codeActionKinds\":[\"quickfix\"]}}");
        Json ruff = parsed("{\"hoverProvider\":true,\"codeActionProvider\":"
                           "{\"resolveProvider\":true},\"textDocumentSync\":2}");
        Json none = parsed("{}");
        using F = Lsp::Feature;
        CHECK(Lsp::offers(pyright, F::Completion) && Lsp::offers(pyright, F::SignatureHelp) &&
              Lsp::offers(pyright, F::Rename) && Lsp::offers(pyright, F::References));
        CHECK(!Lsp::offers(ruff, F::Completion) && Lsp::offers(ruff, F::Hover) &&
              Lsp::offers(ruff, F::CodeActions) && !Lsp::offers(ruff, F::Definition));
        CHECK(!Lsp::offers(parsed("{\"hoverProvider\":false}"), F::Hover));
        CHECK(!Lsp::offers(parsed("{\"signatureHelpProvider\":true}"), F::SignatureHelp));
        std::vector<Lsp::ServerView> both = {{true, true, &pyright}, {false, true, &ruff}};
        // One-server requests go to the main one, even where Ruff offers too.
        for (F f : {F::Completion, F::SignatureHelp, F::Hover, F::Definition, F::References,
                    F::Rename})
            CHECK(Lsp::routeRequest(f, both) == 0);
        // A main server that is still starting gets it (the request waits).
        std::vector<Lsp::ServerView> starting = {{true, false, &none}, {false, true, &ruff}};
        CHECK(Lsp::routeRequest(F::Hover, starting) == 0);
        // No main server (not installed): Ruff's hover, and no completion.
        std::vector<Lsp::ServerView> ruffOnly = {{false, true, &ruff}};
        CHECK(Lsp::routeRequest(F::Hover, ruffOnly) == 0);
        CHECK(Lsp::routeRequest(F::Completion, ruffOnly) == -1);
        // A main server without the feature: the other, when it has it.
        std::vector<Lsp::ServerView> bare = {{true, true, &none}, {false, true, &ruff}};
        CHECK(Lsp::routeRequest(F::Hover, bare) == 1);
        CHECK(Lsp::routeRequest(F::Rename, bare) == -1);
        // A second server still starting is not guessed at for one-server requests.
        std::vector<Lsp::ServerView> ruffStarting = {{true, true, &none}, {false, false, &none}};
        CHECK(Lsp::routeRequest(F::Hover, ruffStarting) == -1);
        CHECK(Lsp::routeRequest(F::Hover, {}) == -1);
        // Code actions: everyone who offers them, and anyone still starting.
        CHECK((Lsp::serversOffering(F::CodeActions, both) == std::vector<int>{0, 1}));
        CHECK((Lsp::serversOffering(F::CodeActions, bare) == std::vector<int>{1}));
        CHECK((Lsp::serversOffering(F::CodeActions, ruffStarting) == std::vector<int>{1}));
        CHECK(Lsp::serversOffering(F::Completion, ruffOnly).empty());
    }

    GROUP("lsp:also-status");
    {
        using S = Lsp::ServerState;
        auto line = [](std::vector<Lsp::ServerStatus> v, int e = 0, int w = 0) {
            return Lsp::statusLine("Python", v, e, w);
        };
        Lsp::StatusLine l = line({{"basedpyright", S::Running, true}, {"ruff", S::Running, false}},
                                 2, 1);
        CHECK(l.text == "basedpyright + ruff: 2 errors, 1 warning" && l.dot == Lsp::StatusDot::Green);
        l = line({{"basedpyright", S::Running, true}});
        CHECK(l.text == "basedpyright: no problems" && l.dot == Lsp::StatusDot::Green);
        l = line({{"basedpyright", S::Running, true}, {"ruff", S::Starting, false}}, 0, 3);
        CHECK(l.text == "basedpyright: 3 warnings \xC2\xB7 ruff starting\xE2\x80\xA6" &&
              l.dot == Lsp::StatusDot::Yellow);
        l = line({{"basedpyright", S::Running, true}, {"ruff", S::Crashed, false}}, 1);
        CHECK(l.text == "basedpyright: 1 error \xC2\xB7 ruff crashed" &&
              l.dot == Lsp::StatusDot::Red);
        l = line({{"basedpyright", S::Starting, true}, {"ruff", S::Running, false}});
        CHECK(l.text == "basedpyright starting\xE2\x80\xA6 \xC2\xB7 ruff: no problems" &&
              l.dot == Lsp::StatusDot::Yellow);
        // A main server that is missing keeps its install prompt, first.
        l = line({{"", S::NotInstalled, true}, {"ruff", S::Running, false}}, 0, 1);
        CHECK(l.text == "Install a Python language server\xE2\x80\xA6 \xC2\xB7 ruff: 1 warning");
        l = line({{"", S::NotInstalled, true}});
        CHECK(l.text == "Install a Python language server\xE2\x80\xA6" &&
              l.dot == Lsp::StatusDot::Gray);
        // Off says so alone; beside a running server it says nothing.
        CHECK(line({{"", S::Off, true}}).text == "Python language server off");
        CHECK(line({{"", S::Off, true}, {"ruff", S::Running, false}}).text == "ruff: no problems");
        // A second server that is off, idle or missing by default says nothing...
        CHECK(line({{"basedpyright", S::Running, true}, {"ruff", S::Off, false}}).text ==
              "basedpyright: no problems");
        CHECK(line({{"basedpyright", S::Running, true}, {"", S::Idle, false}}).text ==
              "basedpyright: no problems");
        // ...but one asked for and not installed, stopped or failed does.
        CHECK(line({{"basedpyright", S::Running, true}, {"ruff", S::NotInstalled, false}}).text ==
              "basedpyright: no problems \xC2\xB7 ruff not installed");
        CHECK(line({{"basedpyright", S::Stopped, true}, {"ruff", S::Stopped, false}}).text ==
              "basedpyright stopped \xC2\xB7 ruff stopped");
        l = line({{"basedpyright", S::Failed, true}, {"ruff", S::Restarting, false}});
        CHECK(l.text == "basedpyright failed to start \xC2\xB7 ruff crashed, restarting\xE2\x80\xA6" &&
              l.dot == Lsp::StatusDot::Red);
        l = line({{"", S::Idle, true}});
        CHECK(l.text.empty() && l.dot == Lsp::StatusDot::None);
    }

    GROUP("lsp:also-diagnostics");
    {
        auto diag = [](int line, int c0, int c1, Lsp::Severity sev, const char *msg,
                       const char *source = "") {
            Lsp::Diagnostic d;
            d.range = {{line, c0}, {line, c1}};
            d.severity = sev;
            d.message = msg;
            d.source = source;
            return d;
        };
        Lsp::DiagnosticStore store;
        CHECK(store.empty() && !store.has("file:///a.py"));
        // Ruff and basedpyright each publish for a.py: both stand.
        CHECK(store.publish("file:///a.py", "py#1",
                            {diag(0, 7, 9, Lsp::Severity::Error, "Import \"os\" is not accessed",
                                  "basedpyright")}, nullptr, "basedpyright"));
        CHECK(store.publish("file:///a.py", "ruff#2",
                            {diag(0, 7, 9, Lsp::Severity::Warning, "`os` imported but unused"),
                             diag(3, 0, 4, Lsp::Severity::Warning, "Line too long")},
                            nullptr, "ruff"));
        CHECK(store.forUri("file:///a.py").size() == 2 && store.has("file:///a.py"));
        std::vector<Lsp::Diagnostic> all = store.merged("file:///a.py");
        CHECK(all.size() == 3);
        // Tagged with their server: Ruff sent no source, so it is "ruff".
        CHECK(all.size() == 3 && all[0].source == "basedpyright" && all[1].source == "ruff" &&
              all[2].source == "ruff");
        // basedpyright publishing again replaces only its own list.
        CHECK(store.publish("file:///a.py", "py#1", {}, nullptr));
        CHECK(store.merged("file:///a.py").size() == 2 &&
              store.find("file:///a.py", "py#1") == nullptr &&
              store.find("file:///a.py", "ruff#2") != nullptr);
        // An empty list for a file a server had nothing on changes nothing.
        CHECK(!store.publish("file:///b.py", "py#1", {}, nullptr));
        CHECK(!store.has("file:///b.py"));
        store.publish("file:///b.py", "py#1", {diag(1, 0, 1, Lsp::Severity::Error, "x")}, nullptr);
        store.publish("file:///c.py", "ruff#2", {diag(1, 0, 1, Lsp::Severity::Hint, "y")}, nullptr);
        CHECK((store.uris() == std::vector<std::string>{"file:///a.py", "file:///b.py",
                                                         "file:///c.py"}));
        // A server going away takes its lists, and only its.
        CHECK(store.eraseServer("ruff#2"));
        CHECK((store.uris() == std::vector<std::string>{"file:///b.py"}));
        CHECK(!store.eraseServer("ruff#2"));
        CHECK(store.erase("file:///b.py", "py#1") && store.empty());
        CHECK(!store.erase("file:///b.py", "py#1"));

        // Drawn together, each against the text its server had: Ruff's from
        // before a line was typed above, basedpyright's from after.
        auto text = std::make_shared<const std::u16string>(u"import os\nx = 1\n");
        auto typed = std::make_shared<const std::u16string>(u"# hi\nimport os\nx = 1\n");
        store.publish("file:///a.py", "ruff#2",
                      {diag(1, 0, 1, Lsp::Severity::Warning, "ruff on x")}, text);
        store.publish("file:///a.py", "py#1",
                      {diag(1, 7, 9, Lsp::Severity::Hint, "pyright on os")}, typed);
        std::vector<Lsp::MergedSpan> spans =
            Lsp::mergedDiagnosticSpans(store.forUri("file:///a.py"), *typed);
        CHECK(spans.size() == 2);
        // In order of where they start: "os" (12) before "x" (15).
        CHECK(spans.size() == 2 && spans[0].start == 12 && spans[0].end == 14 &&
              spans[0].diag->message == "pyright on os");
        CHECK(spans.size() == 2 && spans[1].start == 15 && spans[1].end == 16 &&
              spans[1].diag->message == "ruff on x");
        CHECK(Lsp::mergedDiagnosticSpans({}, *typed).empty());
    }

    GROUP("lsp:also-code-actions");
    {
        auto action = [](const char *title, const char *kind, bool preferred = false) {
            Lsp::CodeAction a;
            a.title = title;
            a.kind = kind;
            a.preferred = preferred;
            a.edit = Json::object();
            return a;
        };
        CHECK(Lsp::kindUnder("source.organizeImports.ruff", "source.organizeImports"));
        CHECK(Lsp::kindUnder("source.organizeImports", "source.organizeImports"));
        CHECK(Lsp::kindUnder("source.fixAll.ruff", "source"));
        CHECK(!Lsp::kindUnder("source.organizeImportsX", "source.organizeImports"));
        CHECK(!Lsp::kindUnder("", "source") && !Lsp::kindUnder("source", ""));
        CHECK(!Lsp::kindUnder("quick", "quickfix"));
        std::vector<Lsp::CodeAction> pyright = {
            action("Add \"from os import path\"", "quickfix"),
            action("Organize Imports", "source.organizeImports"),
        };
        std::vector<Lsp::CodeAction> ruff = {
            action("Ruff (F401): Remove unused import: `os`", "quickfix", true),
            action("Ruff (F401): Disable for this line", "quickfix"),
            action("Ruff: Fix all auto-fixable problems", "source.fixAll.ruff"),
            action("Ruff: Organize imports", "source.organizeImports.ruff"),
            action("Extract", "refactor.extract"),
            action("Odd one", "x.custom"),
            action("Organize Imports", "source.organizeImports"),   // a duplicate
        };
        std::vector<Lsp::CodeAction> m = Lsp::mergeCodeActions({pyright, ruff});
        std::vector<std::string> titles;
        for (const Lsp::CodeAction &a : m) titles.push_back(a.title);
        CHECK((titles == std::vector<std::string>{
                   "Ruff (F401): Remove unused import: `os`", "Add \"from os import path\"",
                   "Ruff (F401): Disable for this line", "Extract", "Odd one",
                   "Organize Imports", "Ruff: Fix all auto-fixable problems",
                   "Ruff: Organize imports"}));
        CHECK(m.size() == 8 && m[0].origin == 1 && m[1].origin == 0 && m[5].origin == 0 &&
              m[7].origin == 1);
        CHECK(Lsp::mergeCodeActions({}).empty() && Lsp::mergeCodeActions({{}, {}}).empty());
        // Organize Imports: Ruff's own kind before the bare one, wherever it is.
        int i = Lsp::pickOrganizeImports(m);
        CHECK(i == 7 && m[(size_t)i].origin == 1);
        CHECK(Lsp::pickOrganizeImports(pyright) == 1);
        CHECK(Lsp::pickOrganizeImports({action("Fix", "quickfix")}) == -1);
        CHECK(Lsp::pickOrganizeImports({}) == -1);
    }

    GROUP("lsp:also-client");
    {
        FakeServer srv;
        Lsp::Client client([&](const std::string &b) { srv.take(b); });
        client.initialize("/p", 1);
        client.receive(reply(1, "{\"capabilities\":{\"codeActionProvider\":true}}"));
        CHECK(client.state() == Lsp::Client::State::Ready);
        CHECK(client.offers(Lsp::Feature::CodeActions) && !client.offers(Lsp::Feature::Hover));
        // The client says it handles source.fixAll (Ruff's Fix all).
        bool fixAll = false;
        for (const Json &k : srv.sent[0]["params"]["capabilities"]["textDocument"]["codeAction"]
                                ["codeActionLiteralSupport"]["codeActionKind"]["valueSet"]
                                    .elements())
            if (k.asString() == "source.fixAll") fixAll = true;
        CHECK(fixAll);
        client.codeActions("file:///p/a.py", {{0, 0}, {9, 0}}, Json::array(), nullptr,
                           {"source.organizeImports"});
        const Json &q = srv.sent.back();
        CHECK(q["method"].asString() == "textDocument/codeAction" &&
              q["params"]["context"]["only"].size() == 1 &&
              q["params"]["context"]["only"][0].asString() == "source.organizeImports");
        client.codeActions("file:///p/a.py", {{0, 0}, {0, 0}}, Json::array(), nullptr);
        CHECK(srv.sent.back()["params"]["context"]["only"].isNull());
    }

    GROUP("lsp:also-install");
    {
        const Install::Tool *ruff = Install::server("python.also");
        CHECK(ruff && ruff->name == "ruff");
        if (ruff) {
            Install::Suggestion s = Install::suggest(*ruff, [](const std::string &p) {
                return p == "uv";
            });
            CHECK(s.command == "uv tool install ruff");
            s = Install::suggest(
                *ruff, [](const std::string &p) { return p == "brew"; }, {}, Install::Os::Mac);
            CHECK(s.command == "brew install ruff" && s.before.empty());
            // Linux has no Homebrew way: uv's, with where uv comes from.
            s = Install::suggest(
                *ruff, [](const std::string &p) { return p == "brew"; }, {}, Install::Os::Linux);
            CHECK(s.command == "uv tool install ruff" && s.missing == "uv" &&
                  s.beforeUrl.find("astral.sh") != std::string::npos);
        }
        CHECK(Install::server("cpp.also") == nullptr);
        bool listed = false;
        for (const Install::Tool *t : Install::all()) if (t == ruff) listed = true;
        CHECK(listed);
    }
}
