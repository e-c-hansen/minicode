// FolderTools.h — pure C++. The folder's answer about running its own
// tools: its Python environment for the language servers, its .venv and
// node_modules for Format Document (FormatRun.h), and the code
// rust-analyzer and the TypeScript server would run (Lsp::checkProjectCode).
// One answer per folder, "allow" or "deny", none until the user is asked.
// Here: what to ask the servers' question about, its words, and the file
// the answers are kept in on Linux. The Mac keeps the answers in its
// defaults (MCPythonEnvironmentConsent, Lsp+Python.mm) and asks in
// Objective-C++ with the same words.
#pragma once

#include "PythonEnvs.h"

#include <map>
#include <string>

namespace FolderTools {

// The folder's own Python environment to ask about for the language
// servers, as the window's folder names it (".venv", "sub/env"), or "":
// none in the file's folders up to the window's, the folder has an answer
// (`consent` "allow" or "deny"), the user chose an interpreter of their own
// (`userChose`: python.interpreter, runnable), or the environment is the
// one active where minicode started ($VIRTUAL_ENV, trusted). `ctx` is the
// servers' context (PythonEnvs.h: fileDir, root, env).
std::string environmentToAsk(const PythonEnvs::Context& ctx, const std::string& consent,
                             bool userChose);

// The bar's words for the environment question, and its tooltip.
// `askAgainMenu`: where Ask Again About This Folder's Tools is ("minicode"
// on the Mac, "Edit" on Linux).
std::string environmentQuestion(const std::string& env);
std::string environmentTooltip(const std::string& env, const std::string& askAgainMenu);

// The bar's words for a server that would run the folder's own code
// (`key` "rust" or "typescript"): it waits (Lsp::ProjectCode::Waits),
// because of `file` as the folder names it, or runs without that part.
std::string projectCodeQuestion(const std::string& key, bool waits, const std::string& file);
std::string projectCodeTooltip(const std::string& question, const std::string& askAgainMenu);

// The answers file: {"folders": {"<resolved folder>": "allow" | "deny"}}.
// decode skips anything else (a value that is neither, a file that is not
// JSON gives no answers); encode writes them sorted, one a line.
std::map<std::string, std::string> decode(const std::string& text);
std::string encode(const std::map<std::string, std::string>& answers);

}  // namespace FolderTools
