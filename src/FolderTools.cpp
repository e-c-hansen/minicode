// FolderTools.cpp — see FolderTools.h. The words are the Mac's
// (InstallPanel.mm's bar, Lsp+Servers.mm's projectCodeQuestionForKey:).
#include "FolderTools.h"

#include "Json.h"

namespace FolderTools {

std::string environmentToAsk(const PythonEnvs::Context& ctx, const std::string& consent,
                             bool userChose) {
    if (consent == "allow" || consent == "deny" || userChose) return "";
    const std::string project = PythonEnvs::projectInterpreter(ctx.fileDir, ctx.root);
    // The same one active where minicode started ($VIRTUAL_ENV) is trusted.
    if (project.empty() || PythonEnvs::automatic(ctx, false) == project) return "";
    // bin/python's environment folder, as the window's folder names it.
    std::string env = project;
    for (int i = 0; i < 2; i++) {
        const size_t slash = env.rfind('/');
        if (slash == std::string::npos || slash == 0) break;
        env.resize(slash);
    }
    std::string root = ctx.root;
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    const std::string prefix = root + "/";
    if (!root.empty() && env.compare(0, prefix.size(), prefix) == 0) env = env.substr(prefix.size());
    return env;
}

std::string environmentQuestion(const std::string& env) {
    return "This folder has its own Python environment, " + env +
           ". The language servers run programs from it only if you allow it.";
}

std::string environmentTooltip(const std::string& env, const std::string& askAgainMenu) {
    return "Without it, completions and checks do not see the packages installed in " + env +
           ". The answer is kept for this folder; Ask Again About This Folder's Tools in the " +
           askAgainMenu + " menu takes it back.";
}

std::string projectCodeQuestion(const std::string& key, bool waits, const std::string& file) {
    if (key == "rust")
        return waits ? "This folder's " + file +
                           " can name programs for cargo or rustup to run, so the Rust language "
                           "server starts only if you allow it."
                     : "The Rust language server runs this project's build scripts and "
                       "procedural macros only if you allow it.";
    return waits ? "This folder has its own TypeScript (" + file +
                       ") and there is no other to use, so the language server starts only if "
                       "you allow it to run it."
                 : "This folder has its own TypeScript (" + file +
                       "). The language server uses yours instead unless you allow it.";
}

std::string projectCodeTooltip(const std::string& question, const std::string& askAgainMenu) {
    return question + " Until then it runs without what would run the folder's code, or waits. "
                      "The answer is kept for this folder, as for its .venv and node_modules; "
                      "Ask Again About This Folder's Tools in the " +
           askAgainMenu + " menu takes it back.";
}

std::map<std::string, std::string> decode(const std::string& text) {
    std::map<std::string, std::string> out;
    Json j;
    if (!Json::parse(text, j) || !j.isObject()) return out;
    const Json& folders = j["folders"];
    if (!folders.isObject()) return out;
    for (const auto& kv : folders.items()) {
        if (kv.first.empty() || !kv.second.isString()) continue;
        const std::string& v = kv.second.asString();
        if (v == "allow" || v == "deny") out[kv.first] = v;
    }
    return out;
}

std::string encode(const std::map<std::string, std::string>& answers) {
    std::string out = "{\n  \"folders\": {";
    bool first = true;
    for (const auto& kv : answers) {
        if (kv.second != "allow" && kv.second != "deny") continue;
        out += first ? "\n" : ",\n";
        first = false;
        out += "    " + Json(kv.first).dump() + ": " + Json(kv.second).dump();
    }
    out += first ? "}\n}\n" : "\n  }\n}\n";
    return out;
}

}  // namespace FolderTools
