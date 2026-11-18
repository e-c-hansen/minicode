// ClaudeStatusLine.h — pure C++17. `mutible statusline` (A13): a wrapper
// around the user's own Claude Code status line that notes the plan usage
// Claude Code hands every status line (ClaudeUsage.h's Sample) and then runs
// the user's command as before, so the forecast has readings to fit.
//
// Installing it changes one value in Claude Code's settings file, and only
// after the user said yes to what it changes (the Agents panel; the same
// two presses as the waiting dots' hooks, ClaudeHooksOffer.h):
//
//   {"statusLine": {"type": "command", "command": "~/bin/status.sh", "padding": 2}}
// becomes
//   {"statusLine": {"type": "command",
//                   "command": "mutible statusline --then '~/bin/status.sh'",
//                   "padding": 2}}
//
// so the old command travels inside the new one and the wrapper needs no
// file of its own to find it. With no status line before, the new value is
// {"type": "command", "command": "mutible statusline"}, which prints a short
// line of its own. Every other key of the file and of statusLine stays.
//
// Undo puts it back exactly: install keeps a record (the file as it was and
// as it was written) in mutible's own folder, never under ~/.claude, and
// while the settings file is still what install wrote, undo writes the old
// file back byte for byte (or removes it, when there was none). When the
// file changed since, only statusLine is put back: its old command, or no
// statusLine at all.
//
// The samples go to a log in mutible's own folder (`usageDir`): on the Mac
// ~/Library/Application Support/Mutible/usage, elsewhere
// $XDG_STATE_HOME/mutible/usage; $MUTIBLE_USAGE_DIR for tests.
#pragma once
#include "ClaudeUsage.h"
#include "Json.h"
#include <string>
#include <vector>

namespace ClaudeStatusLine {

// The command installed: "mutible statusline" when `mutible` is on the
// PATH Claude Code sees (`launcher` empty), else the launcher's full path,
// quoted for the shell, then " statusline".
extern const char *const kCommand;
std::string commandFor(const std::string &launcher);

// `s` as one shell word: as it is when only safe characters, else in
// single quotes.
std::string shellQuote(const std::string &s);
// A command line's words as sh would split and unquote them (single and
// double quotes, backslashes); false when a quote is left open.
bool shellWords(const std::string &line, std::vector<std::string> *words);

// Is this statusLine command mutible's wrapper (its program named mutible,
// then `statusline`)? `previous` gets the command it chains to ("" none).
bool isWrapper(const std::string &command, std::string *previous = nullptr);
// The wrapper's command line around `previous` ("" for none).
std::string wrapperCommand(const std::string &base, const std::string &previous);

struct Plan {
    bool ok = false;
    std::string error;          // when not ok: why the file is left alone
    bool changed = false;
    bool hadStatusLine = false;
    std::string previous;       // the command chained to, or put back
    Json statusLine;            // the new value (null when removed)
    std::string text;           // the whole new file, when changed
};

// What installing would make of the settings text: `base` (commandFor's)
// wrapped around the present command. Unchanged when the wrapper is in
// already. Refused for a file that is not a JSON object, a statusLine that
// is not an object, or one whose command is missing or not a command type.
Plan planInstall(const std::string &settingsText, const std::string &base);
// The other way: the chained command put back, or statusLine removed when
// the wrapper chained to nothing. Unchanged when the wrapper is not in.
Plan planUninstall(const std::string &settingsText);
// Is the wrapper Claude Code's status line now?
bool installed(const std::string &settingsText);

// The record install keeps for undo.
struct Record {
    std::string path;           // the settings file
    bool hadFile = false;       // false: install made it
    std::string original;       // its text before
    std::string written;        // its text as install wrote it
};
std::string recordText(const Record &r);
bool parseRecord(const std::string &text, Record *out);

struct Result {
    bool ok = false;
    std::string error;
    bool refused = false;       // the file is not one this can change
    bool changed = false;       // false with ok: nothing to do
    bool exact = false;         // undo wrote back the file as it was before
    std::string backup;         // where the file before this change went
    std::string previous;       // the command chained to (install) or put back (undo)
};

// Reads the settings file at `path`, changes it (a backup first, as the
// hooks' install makes), and keeps the record at `recordPath`.
Result install(const std::string &path, const std::string &base, const std::string &recordPath,
               long now);
// Undoes it, from the record when the file is still as install left it.
Result uninstall(const std::string &path, const std::string &recordPath, long now);

// ------------------------------------------------------------- the log
// mutible's folder for the usage log: `overrideDir` when set, else the
// Mac's or XDG's (see the top of this file).
std::string usageDir(const char *overrideDir, const char *home, const char *xdgState, bool mac);
std::string logPath(const std::string &usageDir);      // <dir>/statusline.log
std::string recordPath(const std::string &usageDir);   // <dir>/statusline-install.json

// Appends `s` unless the log's last line says the same and is under a
// minute old (ClaudeUsage::worthLogging). A log over 1 MB moves to
// `<log>.1` first. The folder is made (0700). False when nothing could be
// written; never more than that.
bool appendSample(const std::string &logPath, const ClaudeUsage::Sample &s);
// Both logs' text, the older first.
std::string readLog(const std::string &logPath);

// The line printed when there is no command to chain to: the model and
// the two windows ("Opus 5.5 · 5h 42% · week 18%").
std::string defaultLine(const std::string &statusJson);

}  // namespace ClaudeStatusLine
