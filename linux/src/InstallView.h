// InstallView.h — how to install a missing language server or formatter
// (PARITY.md 2.8), the Mac's MCInstallView (InstallPanel.mm): what the tool
// gives, the command to run with Copy and Run in Terminal, how to get the
// installer first when it is missing too, the other ways (each with Copy),
// and a note that minicode finds the tool once it is installed. The
// commands are the core's (InstallHints.h), Linux's ways, the first this
// computer can run first. minicode never runs them: Copy puts one on the
// clipboard, and Run in Terminal hands it to the window, which types it
// into the terminal for the user to press Return.
//
// Shown in a popover from the install bar's Install… button (ConsentBar.h)
// and the status bar's server line, and in Format Document's note when the
// formatter is missing (Formatting.h).
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

#include "InstallHints.h"

class InstallView {
public:
    using Run = std::function<void(const std::string& command)>;
    // For a language server key ("python"): "No Python language server",
    // then `message` when given ("lsp.python in the settings runs foo,
    // which was not found."). `run` null leaves out Run in Terminal.
    static InstallView* forServer(const std::string& key, const std::string& message, Run run);
    // For formatters, best first ("pyink", "yapf"): the first one's ways,
    // then the others' suggested commands as alternatives.
    static InstallView* forFormatters(const std::vector<std::string>& programs,
                                      const std::string& message, Run run);

    // The view, owned by whatever it is put in: this object goes with it.
    GtkWidget* widget() const { return box_; }
    // "" when InstallHints knows no such tool (then there is nothing to show).
    const std::string& command() const { return command_; }
    const std::vector<std::string>& alternatives() const { return alternatives_; }
    // After Run in Terminal (a popover closes).
    std::function<void()> onDone;

    // For checks: every text shown, and the command's buttons.
    std::string shownText() const;
    GtkWidget* copyButton() const { return copy_; }
    GtkWidget* runButton() const { return run_; }

private:
    InstallView() = default;
    void build(const Install::Tool* tool, const std::vector<const Install::Tool*>& extra,
               const std::string& title, const std::string& message);
    GtkWidget* row(const std::string& command, bool withRun, GtkWidget** copyOut,
                   GtkWidget** runOut);
    void addText(const std::string& text, const char* cssClass);

    GtkWidget* box_ = nullptr;
    GtkWidget* copy_ = nullptr;
    GtkWidget* run_ = nullptr;
    Run runner_;
    std::string command_;
    std::vector<std::string> alternatives_;
    std::vector<std::string> texts_;
};

// Is an installer (uv, pipx, npm, apt, go, cargo, rustup, snap) here?
// LspFindProgram, or, for checks, membership in $MINICODE_INSTALL_HAVE
// (comma-separated), as on the Mac.
bool InstallerAvailable(const std::string& program);
