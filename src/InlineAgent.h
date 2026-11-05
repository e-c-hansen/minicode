// InlineAgent.h — pure C++. The inline agent (Cmd+I, as in VS Code): what
// minicode asks Claude Code, and what it does with the answer. No I/O; the
// Mac's AgentPanel.mm runs Claude Code and shows the bar.
//
// What has the keyboard decides the kind of request:
//
// - A file in the editor (or its Markdown preview): an edit. The prompt
//   holds the request, the buffer as it is (unsaved changes too) and the
//   cursor or selection; Claude answers with SEARCH/REPLACE blocks, which
//   are applied to the buffer by their text, so edits elsewhere in the file
//   while it thought do not get in the way. A question gets a plain answer.
// - A terminal: a shell command, typed at the prompt and never run.
// - Anything else, or Cmd+Return from either of the above: a task, handed
//   to Claude Code in a terminal tab with a note of where it was asked from.
//
// Text is UTF-8 and offsets are bytes.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace InlineAgent {

// The flags added to agent.command for an answer in the bar: print mode, the
// prompt read from stdin, tools that read the project but change nothing,
// and no session kept on disk.
extern const char* const kPrintFlags;

struct EditRequest {
    std::string request;               // what the user typed
    std::string path;                  // the file, as the prompt names it
    std::string text;                  // the buffer
    size_t selStart = 0, selEnd = 0;   // the selection (equal for a caret)
    std::vector<std::string> earlier;  // earlier requests in this bar, already applied
};
// Past this many bytes only the lines around the cursor go into the prompt.
constexpr size_t kMaxFileBytes = 120 * 1024;
std::string editPrompt(const EditRequest& r);

struct Edit {
    std::string search, replace;   // whole lines, joined with "\n", no final "\n"
    bool hasSearch = true;         // false for an empty SEARCH part (insert at the cursor)
    bool hasReplace = true;        // false for an empty REPLACE part (delete the lines)
};
struct Reply {
    std::vector<Edit> edits;
    std::string answer;   // the text, trimmed, when there are no edit blocks
};
Reply parseReply(const std::string& output);

struct Applied {
    bool ok = false;
    std::string text;    // the new buffer
    std::string error;   // why not, when !ok
};
// Applies every edit in order, each to the result of the last, or none:
// a SEARCH part is found as whole lines first, then as text anywhere, then
// as lines ignoring trailing and then all surrounding whitespace; among
// several matches the one nearest `near` (the cursor) wins. An empty SEARCH
// inserts at `near`. Line endings follow the buffer's ("\r\n" stays).
Applied applyEdits(const std::string& text, const std::vector<Edit>& edits, size_t near);

struct CommandRequest {
    std::string request;
    std::string directory;              // the shell's folder
    std::string screen;                 // the last lines the terminal shows
    std::vector<std::string> earlier;
};
std::string commandPrompt(const CommandRequest& r);
// The command in Claude's answer: a fenced block's content when there is
// one, "$ " prompts taken off, blank lines at the ends dropped. "" with
// *note set when the answer is a "#" line saying no command can do it.
std::string commandFrom(const std::string& output, std::string* note);

struct TaskRequest {
    std::string request;
    std::string where;       // "the file tree, with src/a.cpp selected"
    std::string path;        // the open file, "" for none
    std::string selection;   // the selected text, "" for none
    int firstLine = 0, lastLine = 0;   // the selection's lines, 1-based
    std::string screen;      // a terminal's last lines, "" for none
};
// The prompt for Claude Code in a terminal: the request, then where it was
// asked from and what was selected. "" for a request that is only blank.
std::string taskPrompt(const TaskRequest& r);

// 1-based line of a byte offset.
int lineOf(const std::string& text, size_t offset);

}  // namespace InlineAgent
