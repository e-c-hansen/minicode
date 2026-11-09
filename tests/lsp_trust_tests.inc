// lsp_trust_tests.inc — the `lsp:project-code` group of run_tests.cpp:
// which servers would run a folder's own code, and what they get until the
// folder's answer is "allow" (Lsp::checkProjectCode): rust-analyzer with
// build scripts, proc macros and cargo check off, or not started at all
// with a Cargo or toolchain file that names programs; the TypeScript
// server pointed at the user's own TypeScript, or not started. Included
// there, after lsp_sync_tests.inc.

static void testLspProjectCode() {
    GROUP("lsp:project-code");
    // A made-up file system: the paths that exist, and folders' names.
    std::set<std::string> files;
    Lsp::ProjectFs fs;
    fs.exists = [&](const std::string &p) { return files.count(p) != 0; };
    fs.subfolders = [&](const std::string &d) {
        std::set<std::string> names;
        const std::string prefix = d + "/";
        for (const std::string &f : files)
            if (f.compare(0, prefix.size(), prefix) == 0) {
                const std::string rest = f.substr(prefix.size());
                const size_t slash = rest.find('/');
                if (slash != std::string::npos) names.insert(rest.substr(0, slash));
            }
        return std::vector<std::string>(names.begin(), names.end());
    };
    const std::string home = "/Users/me";

    CHECK(Lsp::serverMayRunProjectCode("rust") && Lsp::serverMayRunProjectCode("typescript"));
    CHECK(!Lsp::serverMayRunProjectCode("cpp") && !Lsp::serverMayRunProjectCode("go") &&
          !Lsp::serverMayRunProjectCode("python") && !Lsp::serverMayRunProjectCode("rust.also"));

    // The folders read: the file's up to the window's folder, then on up,
    // stopping before home and the folders holding it ("/" among them).
    CHECK((Lsp::projectFolders("/Users/me/code/repo/crate/src", "/Users/me/code/repo/crate", home) ==
           std::vector<std::string>{"/Users/me/code/repo/crate/src", "/Users/me/code/repo/crate",
                                    "/Users/me/code/repo", "/Users/me/code"}));
    CHECK((Lsp::projectFolders("/tmp/x/src", "/tmp/x/", home) ==
           std::vector<std::string>{"/tmp/x/src", "/tmp/x", "/tmp"}));
    CHECK((Lsp::projectFolders("/elsewhere", "/Users/me/p", home) ==
           std::vector<std::string>{"/elsewhere", "/Users/me/p"}));   // the file's, then the window's
    CHECK(Lsp::projectFolders("/Users/me", "/Users/me", home).empty());   // home is the user's
    CHECK(Lsp::projectFolders("/Users", "/Users", home).empty());

    // rust: nothing of the folder's names a program, not allowed yet: it
    // starts with the three off, by initializationOptions and the settings
    // workspace/configuration is answered from.
    const std::string root = "/Users/me/code/repo";
    files = {root + "/Cargo.toml", root + "/src/main.rs", root + "/build.rs"};
    Lsp::ProjectCodeCheck r = Lsp::checkProjectCode("rust", false, root + "/src", root, home, "", fs);
    CHECK(r.verdict == Lsp::ProjectCode::Restricted && r.file.empty());
    CHECK(r.initializationOptions["cargo"]["buildScripts"]["enable"].isBool() &&
          !r.initializationOptions["cargo"]["buildScripts"]["enable"].asBool());
    CHECK(r.initializationOptions["procMacro"]["enable"].isBool() &&
          !r.initializationOptions["procMacro"]["enable"].asBool());
    CHECK(r.initializationOptions["checkOnSave"].isBool() &&
          !r.initializationOptions["checkOnSave"].asBool());
    CHECK(r.settings["rust-analyzer"].dump() == r.initializationOptions.dump());
    CHECK(Lsp::configurationSection(r.settings, "rust-analyzer.procMacro.enable").isBool());
    // Allowed: its defaults, nothing sent.
    Lsp::ProjectCodeCheck allowed = Lsp::checkProjectCode("rust", true, root + "/src", root, home,
                                                          "", fs);
    CHECK(allowed.verdict == Lsp::ProjectCode::Free && allowed.initializationOptions.isNull() &&
          allowed.settings.isNull());
    // Each file that could name a program, in the crate, above the folder
    // (a repository opened at a subfolder), or in a crate below it: waits.
    for (const std::string &f :
         {root + "/.cargo/config.toml", root + "/.cargo/config", root + "/rust-toolchain.toml",
          root + "/rust-toolchain", root + "/rust-analyzer.toml", root + "/rust-project.json",
          root + "/src/.cargo/config.toml", std::string("/Users/me/code/.cargo/config.toml")}) {
        files = {root + "/Cargo.toml", root + "/src/main.rs", f};
        Lsp::ProjectCodeCheck w = Lsp::checkProjectCode("rust", false, root + "/src", root, home,
                                                        "", fs);
        CHECK(w.verdict == Lsp::ProjectCode::Waits && w.file == f);
        if (w.file != f) std::printf("  %s: %s\n", f.c_str(), w.file.c_str());
        CHECK(Lsp::checkProjectCode("rust", true, root + "/src", root, home, "", fs).verdict ==
              Lsp::ProjectCode::Free);
    }
    files = {root + "/Cargo.toml", root + "/member/Cargo.toml", root + "/member/rust-toolchain.toml"};
    CHECK(Lsp::rustProjectCodeFile(root, root, home, fs) == root + "/member/rust-toolchain.toml");
    files = {root + "/Cargo.toml", root + "/notes/rust-toolchain.toml"};   // no crate there
    CHECK(Lsp::rustProjectCodeFile(root, root, home, fs).empty());
    // The user's own ~/.cargo/config.toml and toolchain are theirs.
    files = {root + "/Cargo.toml", home + "/.cargo/config.toml", home + "/rust-toolchain.toml"};
    CHECK(Lsp::checkProjectCode("rust", false, root, root, home, "", fs).verdict ==
          Lsp::ProjectCode::Restricted);

    // typescript: no TypeScript of the folder's, nothing to hold back.
    const std::string web = "/Users/me/code/web";
    files = {web + "/package.json", web + "/src/a.ts"};
    CHECK(Lsp::checkProjectCode("typescript", false, web + "/src", web, home, "/opt/ts/tsserver.js",
                                fs).verdict == Lsp::ProjectCode::Free);
    // Its own (here or above it, as the server looks): the user's instead,
    // or with none, it waits.
    for (const std::string &lib :
         {web + "/node_modules/typescript/lib", std::string("/Users/me/code/node_modules/typescript/lib")}) {
        files = {web + "/package.json", lib + "/tsserver.js"};
        CHECK(Lsp::workspaceTypescript(web, home, fs) == lib);
        Lsp::ProjectCodeCheck t = Lsp::checkProjectCode("typescript", false, web, web, home,
                                                        "/opt/ts/lib/tsserver.js", fs);
        CHECK(t.verdict == Lsp::ProjectCode::Restricted && t.file == lib &&
              t.initializationOptions["tsserver"]["path"].asString() == "/opt/ts/lib/tsserver.js");
        CHECK(Lsp::checkProjectCode("typescript", false, web, web, home, "", fs).verdict ==
              Lsp::ProjectCode::Waits);
        CHECK(Lsp::checkProjectCode("typescript", true, web, web, home, "", fs).verdict ==
              Lsp::ProjectCode::Free);
    }
    files = {home + "/node_modules/typescript/lib/tsserver.js"};   // the user's own
    CHECK(Lsp::workspaceTypescript(web, home, fs).empty());

    // The user's own TypeScript: in the server's package, beside it, or
    // the tsserver on PATH's; never one inside the folder.
    const std::string pkg = "/opt/homebrew/lib/node_modules/typescript-language-server";
    files = {pkg + "/node_modules/typescript/lib/tsserver.js"};
    CHECK(Lsp::ownTypescript(pkg + "/lib/cli.mjs", "", web, fs) ==
          pkg + "/node_modules/typescript/lib/tsserver.js");
    files = {"/opt/homebrew/lib/node_modules/typescript/lib/tsserver.js"};
    CHECK(Lsp::ownTypescript(pkg + "/lib/cli.mjs", "", web, fs) ==
          "/opt/homebrew/lib/node_modules/typescript/lib/tsserver.js");
    files = {"/usr/local/lib/node_modules/typescript/lib/tsserver.js"};
    CHECK(Lsp::ownTypescript("", "/usr/local/lib/node_modules/typescript/bin/tsserver", web, fs) ==
          "/usr/local/lib/node_modules/typescript/lib/tsserver.js");
    files = {web + "/node_modules/typescript/lib/tsserver.js"};
    CHECK(Lsp::ownTypescript("", web + "/node_modules/typescript/bin/tsserver", web, fs).empty());

    // Other servers are never held back.
    files = {root + "/.cargo/config.toml", web + "/node_modules/typescript/lib/tsserver.js"};
    CHECK(Lsp::checkProjectCode("go", false, root, root, home, "", fs).verdict ==
          Lsp::ProjectCode::Free);
    CHECK(Lsp::checkProjectCode("cpp", false, web, web, home, "", fs).verdict ==
          Lsp::ProjectCode::Free);

    // The status bar says a waiting server waits.
    Lsp::ServerStatus waiting;
    waiting.name = "rust-analyzer";
    waiting.main = true;
    waiting.state = Lsp::ServerState::Waiting;
    const Lsp::StatusLine line = Lsp::statusLine("Rust", {waiting}, 0, 0);
    CHECK(line.text == "rust-analyzer waits for your answer about this folder");
}
