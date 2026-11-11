// IdeLinkServer.h — the Claude Code IDE link for one window, the GTK port's
// counterpart of the Mac's MCIdeLink (src/IdeLinkServer.mm), PARITY.md item
// 3.9: Claude Code's `/ide` answered per window, as VS Code's extension does.
// The protocol is the core's IdeLink.h; this is the sockets on the GLib main
// loop (127.0.0.1 only, no thread), the lock file, and the tools mapped onto
// the editor: openFile (the agent's preview tab, a highlight), openDiff (a
// comparison tab offered for a decision), close_tab, closeAllDiffTabs,
// getDiagnostics, the selection and the open editors. saveDocument and
// executeCode are not offered.
//
// Off unless agent.ide-link is on (setEnabled follows the setting). Lock
// files go to $CLAUDE_CONFIG_DIR/ide, else ~/.claude/ide, and are removed
// when the link stops, the window closes or the process exits.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtk/gtk.h>

#include "Lsp.h"

class Editor;

struct IdeLinkHost {
    Editor* editor = nullptr;
    GtkWindow* window = nullptr;
    std::string root;
    // The Problems rows (headings included; they are skipped).
    std::function<std::vector<LspProblem>()> problems;
    // A line for the status bar.
    std::function<void(const std::string&)> note;
};

class IdeLinkServer {
public:
    explicit IdeLinkServer(IdeLinkHost host);
    ~IdeLinkServer();
    IdeLinkServer(const IdeLinkServer&) = delete;
    IdeLinkServer& operator=(const IdeLinkServer&) = delete;

    // Starts or stops the link (agent.ide-link). A start that fails leaves
    // the reason in problem() and the link off.
    void setEnabled(bool on);
    bool running() const { return listenFd_ >= 0; }
    int port() const { return port_; }
    const std::string& lockPath() const { return lockPath_; }
    const std::string& authToken() const { return token_; }
    size_t connectionCount() const;   // peers past the handshake
    size_t clientCount() const;       // those that said initialize
    // "CLAUDE_CODE_SSE_PORT=<port>" while the link is on, else nothing:
    // for the shell a terminal starts.
    std::vector<std::string> terminalEnvironment() const;

    static const std::string& problem();
    // $CLAUDE_CONFIG_DIR/ide, else ~/.claude/ide.
    static std::string folder();
    // Removes lock files minicode left behind (its own ideName, another pid,
    // that pid gone); returns how many.
    static unsigned removeStaleLocks(const std::string& folder);

private:
    struct Impl;
    Impl* d_;
    int listenFd_ = -1;
    int port_ = 0;
    std::string token_, lockPath_;
};
