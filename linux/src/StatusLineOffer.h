// StatusLineOffer.h — the Mac's StatusLineOffer.{h,mm} (mac/src). Installing
// and undoing `mutible statusline` as Claude Code's status line (A13, the
// core's ClaudeStatusLine.h), from the Agents panel's plan usage section
// (UsageView.h). It follows the waiting dots' hooks (ClaudeHooksOffer.h)
// exactly: a question that shows what goes into Claude Code's settings file,
// a write only on the second yes (a backup first, every other setting kept),
// then where it went. The user's own status line keeps running, chained from
// the wrapper, and undo puts it back as it was: byte for byte while the file
// is still what install wrote, else only statusLine. Every rule is the
// core's (planInstall, install, planUninstall, uninstall); this file shows
// them and asks.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>

namespace StatusLineOffer {

// mutible's folder for the usage log: $MUTIBLE_USAGE_DIR, else
// $XDG_STATE_HOME/mutible/usage (~/.local/state/mutible/usage). Never under
// ~/.claude.
std::string usageDir();
// The command installed: `mutible statusline` when `mutible` is on the
// PATH, else this program by its full path, then ` statusline`. The
// wrapper is known by its program's name (ClaudeStatusLine::isWrapper), so
// a copy of another name (the GUI checks' mutible-guitest) names the
// mutible beside it.
std::string command();
// Is the wrapper Claude Code's status line now?
bool installed();

// The question (Log Plan Usage, Cancel), the write, then what happened.
// `done` gets whether it is installed after.
void install(GtkWindow* parent, std::function<void(bool installed)> done);
// Undo: the question (Put It Back, Cancel), the write, then what happened.
// `done` gets whether it is still installed after.
void uninstall(GtkWindow* parent, std::function<void(bool installed)> done);

}  // namespace StatusLineOffer
