// ClaudeHooksOffer.h — the Mac's ClaudeHooksOffer.{h,mm} (mac/src). The
// waiting dots (TerminalAttention.h) need Claude Code's hooks to run
// `mutible notify --from-hook`; this is where the app offers them, so nobody
// has to know `mutible hooks --install`:
//   - a bar over the terminals the first time Claude Code starts in one
//     (Terminal::onClaudeStarted), once per launch, unless the hooks are in
//     already or Not Now was pressed once (kept as the install bar's Don't
//     Show Again, ConsentBar::installMuted, under the key "claude-hooks").
// Turn On… shows exactly what goes into Claude Code's settings file and
// writes only on a second yes, through the core's ClaudeHooks::install (a
// backup of the file first, every key and hook of the user's kept), then says
// where it wrote. A press when they are in says so and writes nothing. This
// is the one place mutible writes under ~/.claude besides `mutible hooks
// --install`, and only after those two presses. Every rule is the core's
// (ClaudeHooks.h: merge, install, shouldOffer); this file shows them.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>

namespace ClaudeHooksOffer {

// The key in the install bar's muted list.
extern const char* const kKey;

// Claude Code's settings file: $CLAUDE_CONFIG_DIR/settings.json, else
// ~/.claude/settings.json.
std::string settingsPath();
// The command the hooks run: `mutible notify --from-hook` when `mutible`
// is on the PATH, else this program by its full path.
std::string command();
// Every event's hook is in the settings file already.
bool installed();

// The question (what would be added, as JSON; Add the Hooks, Cancel), then the
// write, then where it went (or why the file was left alone). `done` gets true
// when the hooks are in afterwards (written now or already there).
void ask(GtkWindow* parent, std::function<void(bool installed)> done);

// The bar, when it is to be offered now (not muted, not installed, not
// offered yet this launch); nullptr otherwise. `gone` is called when the bar
// should be taken out (Turn On… finished with the hooks in, Not Now, ×);
// `shown` says one is up in this window already (checked before the offer is
// taken, so a second terminal does not use it up).
GtkWidget* takeBar(GtkWindow* parent, bool shown, std::function<void()> gone);

// For checks: the offer may be made again this launch.
void forgetOffer();

}  // namespace ClaudeHooksOffer
