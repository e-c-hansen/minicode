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

// Does this hook entry (one element of an event's list) run minicode's
// command?
bool runsOurCommand(const Json &entry);

}  // namespace ClaudeHooks
