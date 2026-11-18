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
// one active where mutible started ($VIRTUAL_ENV, trusted). `ctx` is the
// servers' context (PythonEnvs.h: fileDir, root, env).
std::string environmentToAsk(const PythonEnvs::Context& ctx, const std::string& consent,
                             bool userChose);

// The bar's words for the environment question, and its tooltip.
// `askAgainMenu`: where Ask Again About This Folder's Tools is ("mutible"
// on the Mac, "Edit" on Linux).
std::string environmentQuestion(const std::string& env);
std::string environmentTooltip(const std::string& env, const std::string& askAgainMenu);

// Whether the Python server `server` (Lsp::pythonServerName: "basedpyright",
// "pyright", "pylsp", "jedi-language-server") reads the packages of the
// folder's environment `env` by itself, running nothing from it:
// basedpyright finds a .venv at the project's root on its own and reads its
// site-packages as files. Completions and checks then see those packages
// before any answer, and the answer gates only what runs the environment's
// Python (the servers' exact search paths, a new terminal's activation).
bool serverReadsEnvironment(const std::string& server, const std::string& env);

// The bar over a Python file, when something a person would notice waits
// on the answer: "" when the server reads the packages itself (the choice
// is then in the Language Servers popover only); for a server that does
// not, what it is missing; for one not known yet (`server` "": none found,
// and one may be in the environment), environmentQuestion.
std::string environmentBarQuestion(const std::string& env, const std::string& server);
// The Language Servers popover's words beside its Allow and Don't Allow
// while the folder has no answer: what the answer changes. `terminal`:
// terminal.activate-venv is on.
std::string environmentPopoverText(const std::string& env, const std::string& server,
                                   bool terminal);

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
