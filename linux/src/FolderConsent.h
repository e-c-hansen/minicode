// FolderConsent.h — the folder's answer about running its own tools
// (linux/PARITY.md 2.3): its .venv for the Python language servers, its
// .venv and node_modules for Format Document, a configuration that runs
// code when a formatter reads it, and the code rust-analyzer and the
// TypeScript server would run. One answer per folder (its path with links
// resolved), "allow" or "deny", none until the user is asked: the Mac's
// MCFolderToolsConsent, MCSetFolderToolsConsent and MCForgetFolderToolsConsent
// (Lsp+Python.mm), there in the defaults, here in
// $XDG_STATE_HOME/minicode/folder-tools.json (D11; FolderTools::decode and
// encode). Every window on a folder follows a change at once (listen), its
// servers and its bar alike.
//
// A question about the folder's own tools put off with the bar's × stays
// away until minicode starts again, or until the folder's answer is taken
// back (Ask Again About This Folder's Tools, forget).
#pragma once

#include <functional>
#include <string>

namespace FolderConsent {

// A folder's key: its path with symlinks resolved.
std::string keyFor(const std::string& root);
// "allow", "deny", or "" when the folder has no answer (or no folder).
std::string answer(const std::string& root);
void set(const std::string& root, bool allow);
// The answer taken back; a question put off with × comes back too.
void forget(const std::string& root);

// The bar's × for this launch.
void putOff(const std::string& root);
bool isPutOff(const std::string& root);

// Called with the folder's key after its answer changes or a question is
// put off. Returns an id for unlisten.
int listen(std::function<void(const std::string& key)> fn);
void unlisten(int id);

// $XDG_STATE_HOME/minicode/folder-tools.json.
std::string file();

}  // namespace FolderConsent
