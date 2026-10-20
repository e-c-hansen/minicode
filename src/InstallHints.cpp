// InstallHints.cpp — pure C++. See InstallHints.h.
#include "InstallHints.h"

namespace Install {

namespace {

const char* kServerGives =
    " gives you completions, documentation on hover, go to definition, and errors "
    "as you type.";

// uv first for Python: `uv tool install basedpyright` brings its own Node,
// where pyright from npm needs a working one (Homebrew's node has broken
// under it before). uv and pipx put their tools in ~/.local/bin, npm -g in
// Homebrew's bin or the user's prefix, go install in ~/go/bin and rustup in
// ~/.cargo/bin; MiniCode's search (MCSearchDirs) covers every one.
const std::vector<std::pair<std::string, Tool>>& servers() {
    static const std::vector<std::pair<std::string, Tool>> t = {
        {"python",
         {"basedpyright", std::string("basedpyright") + kServerGives,
          "https://docs.basedpyright.com",
          {{"uv tool install basedpyright", "uv"},
           {"pipx install basedpyright", "pipx"},
           {"npm install -g pyright", "npm"},
           {"pipx install python-lsp-server", "pipx"}}}},
        {"cpp",
         {"clangd", std::string("clangd") + kServerGives, "https://clangd.llvm.org",
          // The command line tools bring clangd (and clang-format); Homebrew's
          // llvm is keg-only, and its bin folder is searched too.
          {{"xcode-select --install", ""}, {"brew install llvm", "brew"}}}},
        {"go",
         {"gopls", std::string("gopls") + kServerGives, "https://go.dev/gopls",
          {{"go install golang.org/x/tools/gopls@latest", "go"},
           {"brew install gopls", "brew"}}}},
        {"rust",
         {"rust-analyzer", std::string("rust-analyzer") + kServerGives,
          "https://rust-analyzer.github.io",
          {{"rustup component add rust-analyzer", "rustup"},
           {"brew install rust-analyzer", "brew"}}}},
        {"typescript",
         {"typescript-language-server",
          std::string("typescript-language-server") + kServerGives,
          "https://github.com/typescript-language-server/typescript-language-server",
          {{"npm install -g typescript-language-server typescript", "npm"},
           {"brew install typescript-language-server", "brew"}}}},
    };
    return t;
}

const std::vector<Tool>& formatters() {
    static const std::vector<Tool> t = {
        {"pyink",
         "pyink formats Python in Google's style with Format Document (Shift+Option+F).",
         "https://github.com/google/pyink",
         {{"uv tool install pyink", "uv"}, {"pipx install pyink", "pipx"}}},
        {"yapf",
         "yapf formats Python in Google's style with Format Document (Shift+Option+F).",
         "https://github.com/google/yapf",
         {{"uv tool install yapf", "uv"}, {"pipx install yapf", "pipx"}}},
        {"black", "Black formats Python with Format Document (Shift+Option+F).",
         "https://black.readthedocs.io",
         {{"uv tool install black", "uv"},
          {"pipx install black", "pipx"},
          {"brew install black", "brew"}}},
        {"ruff", "Ruff formats Python with Format Document (Shift+Option+F).",
         "https://docs.astral.sh/ruff",
         {{"uv tool install ruff", "uv"},
          {"pipx install ruff", "pipx"},
          {"brew install ruff", "brew"}}},
        {"clang-format",
         "clang-format formats C, C++, Objective-C, Java, JavaScript, TypeScript, "
         "Protocol Buffers, and C# with Format Document (Shift+Option+F).",
         "https://clang.llvm.org/docs/ClangFormat.html",
         {{"xcode-select --install", ""}, {"brew install clang-format", "brew"}}},
        {"gofmt", "gofmt, which comes with Go, formats Go with Format Document "
                  "(Shift+Option+F).",
         "https://go.dev/doc/install", {{"brew install go", "brew"}}},
        {"rustfmt", "rustfmt formats Rust with Format Document (Shift+Option+F).",
         "https://github.com/rust-lang/rustfmt",
         {{"rustup component add rustfmt", "rustup"}}},
        {"shfmt", "shfmt formats shell scripts with Format Document (Shift+Option+F).",
         "https://github.com/mvdan/sh",
         {{"brew install shfmt", "brew"},
          {"go install mvdan.cc/sh/v3/cmd/shfmt@latest", "go"}}},
    };
    return t;
}

std::string baseName(const std::string& program) {
    size_t slash = program.find_last_of('/');
    return slash == std::string::npos ? program : program.substr(slash + 1);
}

}  // namespace

const Tool* server(const std::string& key) {
    for (const auto& s : servers())
        if (s.first == key) return &s.second;
    return nullptr;
}

const Tool* formatter(const std::string& program) {
    std::string name = baseName(program);
    for (const Tool& t : formatters())
        if (t.name == name) return &t;
    return nullptr;
}

std::vector<const Tool*> all() {
    std::vector<const Tool*> out;
    for (const auto& s : servers()) out.push_back(&s.second);
    for (const Tool& t : formatters()) out.push_back(&t);
    return out;
}

const std::vector<std::string>& serverKeys() {
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> k;
        for (const auto& s : servers()) k.push_back(s.first);
        return k;
    }();
    return keys;
}

Prerequisite prerequisite(const std::string& program) {
    if (program == "uv")
        return {"uv", "brew install uv", "brew",
                "https://docs.astral.sh/uv/getting-started/installation/"};
    if (program == "pipx")
        return {"pipx", "brew install pipx", "brew", "https://pipx.pypa.io"};
    if (program == "npm")
        return {"npm", "brew install node", "brew", "https://nodejs.org"};
    if (program == "go") return {"go", "brew install go", "brew", "https://go.dev/doc/install"};
    // rustup's own installer is a script piped to sh; better read on its page.
    if (program == "rustup") return {"rustup", "", "", "https://rustup.rs"};
    if (program == "brew") return {"brew", "", "", "https://brew.sh"};
    return {program, "", "", ""};
}

Suggestion suggest(const Tool& tool, const std::function<bool(const std::string&)>& have,
                   const std::vector<const Tool*>& extra) {
    Suggestion s;
    if (tool.ways.empty()) return s;
    auto canRun = [&](const Way& w) { return w.needs.empty() || (have && have(w.needs)); };
    size_t chosen = tool.ways.size();
    for (size_t i = 0; i < tool.ways.size(); i++)
        if (canRun(tool.ways[i])) { chosen = i; break; }
    if (chosen == tool.ways.size()) {
        chosen = 0;
        Prerequisite p = prerequisite(tool.ways[0].needs);
        s.missing = p.program;
        if (!p.command.empty() && (p.needs.empty() || (have && have(p.needs))))
            s.before = p.command;
        else   // Homebrew is missing too, or there is no one-liner: its page
            s.beforeUrl = p.url;
    }
    s.command = tool.ways[chosen].command;
    for (size_t i = 0; i < tool.ways.size(); i++)
        if (i != chosen) s.alternatives.push_back(tool.ways[i].command);
    for (const Tool* t : extra) {
        if (!t || t == &tool) continue;
        Suggestion other = suggest(*t, have);
        if (!other.command.empty()) s.alternatives.push_back(other.command);
    }
    return s;
}

}  // namespace Install
