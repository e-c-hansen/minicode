// lsp_clangd_tests.inc — the `lsp:clangd-flags` group of run_tests.cpp:
// whether clangd finds compile flags for a file (Lsp::clangdHasCompileFlags:
// a compile_commands.json or compile_flags.txt in the file's folder or one
// above it, or in a build/ folder of either, a .clangd file with a
// CompileFlags section, --compile-commands-dir), which program names are
// clangd, and which diagnostics are drawn on the text (Lsp::drawsOnText:
// not clang's fatal_too_many_errors). Included there, after
// lsp_trust_tests.inc.

static void testLspClangdFlags() {
    GROUP("lsp:clangd-flags");
    std::set<std::string> files;
    std::map<std::string, std::string> texts;
    auto exists = [&](const std::string &p) { return files.count(p) != 0; };
    auto read = [&](const std::string &p) {
        auto it = texts.find(p);
        return it == texts.end() ? std::string() : it->second;
    };
    const std::vector<std::string> clangd = {"clangd"};
    std::string found;

    CHECK(Lsp::isClangd("clangd") && Lsp::isClangd("/opt/homebrew/opt/llvm/bin/clangd"));
    CHECK(Lsp::isClangd("clangd-18") && Lsp::isClangd("/usr/bin/clangd-17.0"));
    CHECK(!Lsp::isClangd("clangd-") && !Lsp::isClangd("clangd-wrapper") &&
          !Lsp::isClangd("ccls") && !Lsp::isClangd("/x/clangd/fakelsp") && !Lsp::isClangd(""));

    // Nothing anywhere: clangd guesses.
    const std::string dir = "/Users/me/code/repo/mac/src";
    CHECK(!Lsp::clangdHasCompileFlags(dir, clangd, exists, read, &found));
    // At the repository's top, two folders up (mutible's own `make`).
    files = {"/Users/me/code/repo/compile_commands.json"};
    CHECK(Lsp::clangdHasCompileFlags(dir, clangd, exists, read, &found));
    CHECK(found == "/Users/me/code/repo/compile_commands.json");
    // In a build/ folder of a folder above (CMake's usual place).
    files = {"/Users/me/code/repo/build/compile_commands.json"};
    CHECK(Lsp::clangdHasCompileFlags(dir + "/", clangd, exists, read, &found));
    CHECK(found == "/Users/me/code/repo/build/compile_commands.json");
    // compile_flags.txt, in the file's own folder, and at the root.
    files = {dir + "/compile_flags.txt"};
    CHECK(Lsp::clangdHasCompileFlags(dir, clangd, exists, read, &found) &&
          found == dir + "/compile_flags.txt");
    files = {"/compile_flags.txt"};
    CHECK(Lsp::clangdHasCompileFlags(dir, clangd, exists, read, &found) &&
          found == "/compile_flags.txt");
    CHECK(Lsp::clangdHasCompileFlags("/", clangd, exists, read, &found));
    // A build/ folder below the file's folder is not looked in, nor is a
    // sibling's.
    files = {dir + "/sub/compile_commands.json", "/Users/me/code/other/compile_commands.json"};
    CHECK(!Lsp::clangdHasCompileFlags(dir, clangd, exists, read, &found));
    // A .clangd with a CompileFlags section counts; one with the word only
    // in a comment, or with other sections, does not.
    files = {"/Users/me/code/repo/.clangd"};
    texts["/Users/me/code/repo/.clangd"] = "CompileFlags:\n  Add: [-Isrc]\n";
    CHECK(Lsp::clangdHasCompileFlags(dir, clangd, exists, read, &found) &&
          found == "/Users/me/code/repo/.clangd");
    texts["/Users/me/code/repo/.clangd"] = "If:\n  PathMatch: .*\\.mm\n  CompileFlags:\n    Add: -x\n";
    CHECK(Lsp::clangdHasCompileFlags(dir, clangd, exists, read, &found));
    texts["/Users/me/code/repo/.clangd"] = "# no CompileFlags here\nDiagnostics:\n  Suppress: x\n";
    CHECK(!Lsp::clangdHasCompileFlags(dir, clangd, exists, read, &found));
    texts["/Users/me/code/repo/.clangd"] = "";
    CHECK(!Lsp::clangdHasCompileFlags(dir, clangd, exists, read, &found));
    // The command line names the folder, in either spelling.
    files.clear();
    CHECK(Lsp::clangdHasCompileFlags(dir, {"clangd", "--compile-commands-dir=/x/build"}, exists,
                                     read, &found) &&
          found == "--compile-commands-dir=/x/build");
    CHECK(Lsp::clangdHasCompileFlags(dir, {"clangd", "--compile-commands-dir", "/x"}, exists, read,
                                     nullptr));
    CHECK(!Lsp::clangdHasCompileFlags(dir, {"--compile-commands-dir=/x"}, exists, read, nullptr));
    // A relative or empty folder is never searched from the current one.
    files = {"compile_commands.json", "/compile_commands.json"};
    CHECK(!Lsp::clangdHasCompileFlags("", clangd, exists, read, nullptr));
    CHECK(!Lsp::clangdHasCompileFlags("src", clangd, exists, read, nullptr));
    // The words: what to do about it, plainly.
    const std::string words = Lsp::kClangdNoFlagsText;
    CHECK(words.find("compile_commands.json") != std::string::npos &&
          words.find("CMAKE_EXPORT_COMPILE_COMMANDS") != std::string::npos &&
          words.find("Bear") != std::string::npos);

    // Drawn on the text: everything but clang's "too many errors".
    Lsp::Diagnostic d;
    d.code = "pp_file_not_found";
    d.source = "clang";
    CHECK(Lsp::drawsOnText(d));
    d.code = "fatal_too_many_errors";
    CHECK(!Lsp::drawsOnText(d));
    d.code = "";
    CHECK(Lsp::drawsOnText(d));
}
