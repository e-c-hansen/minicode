// ClaudeHooks.h — pure C++. Claude Code's hooks, as `minicode notify` and
// `minicode hooks` use them (spec feature 7): what a hook hands its command
// on stdin, what each event means for the terminal the session runs in, the
// hook configuration minicode offers, and merging it into a Claude Code
// settings file without touching anything else in it.
//
// A hook's command gets one JSON object on stdin: hook_event_name,
// session_id, transcript_path, cwd, and for a Notification its message.
// The terminal it came from is named by the environment minicode gives
// every terminal: MINICODE_TERMINAL_ID and MINICODE_PID (Terminal.mm).
//
// The format is Claude Code's and may change, so it is read defensively:
// unknown fields are ignored, and anything unreadable is "nothing to do",
// never an error a hook would report.
#pragma once
#include "Json.h"
#include <string>
#include <vector>

namespace ClaudeHooks {

// What a hook event does to its terminal's mark.
enum class Action { None, Waiting, Done, Clear };

// Notification -> Waiting (a permission prompt, or idle at the prompt),
// Stop -> Done, UserPromptSubmit and PostToolUse -> Clear, else None.
Action actionFor(const std::string &eventName);
// "waiting", "done", "clear" (the protocol's and --state's words), "" for None.
const char *actionName(Action a);
Action actionNamed(const std::string &word);   // None for anything else

struct HookInput {
    std::string event, sessionId, transcriptPath, cwd, message;
};

// The JSON a hook reads on stdin. False when it is not a JSON object.
bool parseHookInput(const std::string &text, HookInput &out);

// The `notify` request's arguments for a hook's stdin and the two variables
// (as found in the environment, null when unset). False, with nothing to
// send, when the variables are missing or malformed, the input is not a
// hook's, or its event means nothing here: the command then exits 0 and
// prints nothing, so a hook can never break Claude Code.
bool notifyArgsFromHook(const std::string &stdinText, const char *terminalIdVar,
                        const char *pidVar, Json &args, int &pid);

// A positive decimal number of at most 9 digits, else 0.
int parsePositive(const char *text);

// ------------------------------------------------------- the configuration
// The command each hook runs.
extern const char *const kCommand;   // "minicode notify --from-hook"
// The events it is installed for, in order.
const std::vector<std::string> &events();
// {"hooks": {"Notification": [{"hooks": [{"type": "command", "command": ...}]}], ...}}
Json configuration();
// The same with another spelling of the command (commandFor's), for only
// these events (every event when empty): what an install adds, to show.
Json configuration(const std::string &command, const std::vector<std::string> &onlyEvents);

// The command to install: kCommand when `minicode` is on the PATH Claude
// Code's hooks see (`launcher` empty), else the launcher's full path,
// quoted for the shell, then " notify --from-hook" (an app copied by hand,
// with no `minicode` on the PATH).
std::string commandFor(const std::string &launcher);

struct MergeResult {
    bool ok = false;
    std::string error;                   // when not ok: why the file was left alone
    bool changed = false;                // something was added
    std::vector<std::string> added;      // the events given the hook
    std::string text;                    // the whole file, when changed
};

// The settings file's text (empty for a file that is not there yet) with
// the hook added to every event that does not already run it. Every other
// key, hook and matcher stays where it was; a second run changes nothing.
// A file that is not a JSON object, or whose "hooks" (or an event's list)
// is the wrong type, is refused, so nothing of the user's is lost.
// The text is written with two-space indents, as Claude Code writes it.
MergeResult merge(const std::string &settingsText);
// The same, adding `command` (commandFor's) rather than kCommand.
MergeResult merge(const std::string &settingsText, const std::string &command);

// Are the hooks in this settings text already (every event runs our
// command)? False for a file merge would refuse.
bool installed(const std::string &settingsText);

// "Notification, Stop, and PostToolUse": the events in prose, with an
// Oxford comma, for the messages that name what was added.
std::string eventList(const std::vector<std::string> &events);

// Does this hook entry (one element of an event's list) run minicode's
// command?
bool runsOurCommand(const Json &entry);

// ------------------------------------------------------- writing the file
// The file a write to `path` lands in: the end of any chain of symlinks at
// it, even a file not there yet (a dangling link, as a dotfiles checkout
// leaves before its first run), a relative link read against its own
// folder; `path` when it is no link, "" for a loop.
std::string writeTarget(const std::string &path);

// Writes `text` to the settings file at `path` so a reader sees the old
// file or the new one, never half of each: a temporary in the folder of
// the file `path` names (writeTarget: a settings.json that is a symlink
// into a dotfiles folder stays a link, and the dotfiles copy changes),
// given that file's mode (`newMode` for a new file), flushed, then renamed
// over it. A file with other hard links is written in place instead, so its
// other names see the new text too. False, with `error` saying why, when
// nothing was written.
bool writeSettingsFile(const std::string &path, const std::string &text, unsigned newMode,
                       std::string &error);

// ------------------------------------------------------- installing
// Claude Code's settings file: $CLAUDE_CONFIG_DIR/settings.json when that
// variable is set (either argument may be null), else ~/.claude/settings.json.
std::string settingsPath(const char *configDir, const char *home);

// Reads the settings file. A file that is not there is no error: true,
// `exists` false, `text` empty. False with `error` for one that cannot be
// read or is not a plain file.
bool readSettingsFile(const std::string &path, std::string &text, bool &exists,
                      std::string &error);

struct InstallResult {
    bool ok = false;                 // false: nothing was written, `error` says why
    std::string error;
    bool refused = false;            // the file is not one merge can change (left alone)
    bool changed = false;            // false with ok: the hooks were there already
    std::vector<std::string> added;  // the events given the hook
    std::string backup;              // where the old file was copied, "" for a new file
};

// `minicode hooks --install`, and the app's Install Claude Code Hooks…
// after the user said yes to what it adds: reads the file at `path`,
// merges `command` in (merge), and when that changes it, first copies the
// file as it was to `<path>.minicode-backup-<YYYYmmdd-HHMMSS>` (local time
// of `now`, never over an earlier backup), then writes the new text
// (writeSettingsFile; a new file gets mode 0600 in a folder made 0700).
InstallResult install(const std::string &path, const std::string &command, long now);

// ------------------------------------------------------- seeing Claude Code
// Does a command line typed at the shell (633;E's text) start Claude Code?
// Its program, past variable assignments and exec, command, builtin,
// noglob, nohup, time and env (with env's own assignments), is `claude` or
// a path ending in /claude.
bool startsClaude(const std::string &commandLine);
// Is this executable's path Claude Code's (proc_pidpath of the foreground
// process)? Its name is `claude`, or it sits in a `claude/versions` folder,
// where the native installer keeps each version under its number.
bool isClaudeProgramPath(const std::string &path);

}  // namespace ClaudeHooks
