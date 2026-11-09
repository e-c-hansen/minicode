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
//   while it thought do not get in the way. The result is shown as a
//   pending diff (pendingDiff) and reaches the buffer only on Keep. A
//   question gets a plain answer.
// - A terminal: a shell command, typed at the prompt and never run.
// - Anything else, or Cmd+Return from either of the above: a task, handed
//   to Claude Code in a terminal tab with a note of where it was asked from.
//
// Text is UTF-8 and offsets are bytes.
#pragma once

#include "Json.h"

#include <cstddef>
#include <string>
#include <vector>

namespace InlineAgent {

// The flags added to agent.command for an answer in the bar: print mode, the
// prompt read from stdin, tools that read the project but change nothing,
// no session kept on disk, and the answer as a stream of JSON lines with the
// text as it is written (Stream reads it).
extern const char* const kPrintFlags;

// Claude Code's `--output-format stream-json --verbose
// --include-partial-messages`, read as it arrives: one JSON object a line,
// "system" first, then for each turn "stream_event" lines (message_start,
// content_block_start, content_block_delta with text_delta pieces, ...),
// the whole "assistant" message, a "user" line with tool results, and last
// a "result" line with the final text. Lines that are not JSON are left
// out; when no line is (an agent.command that prints plain text, or the
// test's stand-ins), the output itself is the reply. Not thread-safe.
class Stream {
public:
    // Bytes as they arrive, cut anywhere. True when text() or activity()
    // changed.
    bool feed(const std::string& bytes);
    // The output ended: reads a last line that had no "\n".
    bool finish();
    // The reply being written: the text of the turn under way (a turn that
    // goes on to use a tool is replaced by the next one).
    const std::string& text() const { return text_; }
    // What Claude is doing while no text arrives: "Reading a.cpp",
    // "Searching for parseReply", "" otherwise.
    const std::string& activity() const { return activity_; }
    bool sawJson() const { return sawJson_; }
    bool done() const { return done_; }          // the result line came
    bool failed() const { return failed_; }      // ...and says is_error
    // The answer: the result line's text, else the last turn's text, else
    // (no JSON at all) the output as it came.
    std::string reply() const;
    // For failed(), or a run that ended without a result: why, in a line.
    std::string error() const;
    // Bytes held for the reply: the output as it came, kept only while no
    // line has been JSON, and an unfinished last line.
    size_t heldBytes() const { return raw_.size() + pending_.size(); }

private:
    void line(const std::string& l);
    std::string pending_, raw_, text_, activity_, lastText_, result_, subtype_, messageId_;
    bool sawJson_ = false, done_ = false, failed_ = false, partial_ = false, changed_ = false;
};

// What the bar shows while a reply is written (Stream::text so far): text
// outside edit blocks as it is, and for each edit block "Edit 1:" (with
// "in place of 3 lines" once its SEARCH part is over) and the REPLACE part's
// lines as they come; an unfinished marker line at the end is left out.
// Only the last `maxLines` lines, the newest.
std::string preview(const std::string& partial, size_t maxLines = 12);

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
    size_t selStart = 0, selEnd = 0;   // the selection, moved with the edits
};
// Applies every edit in order, each to the result of the last, or none:
// a SEARCH part is found as whole lines first, then as text anywhere, then
// as lines ignoring trailing and then all surrounding whitespace. A SEARCH
// part found in several places is taken only where exactly one of them
// touches the selection [selStart, selEnd) (for a caret, holds it); else
// nothing is changed and the error says how many places it matched. After
// a match that ignored indentation (or text found after a line's
// indentation, the REPLACE part running over several lines), the REPLACE
// lines are moved by the
// difference between the file's indentation and the SEARCH part's (tabs
// for spaces too, when the file indents with tabs). An empty SEARCH inserts
// at selStart. Line endings follow the buffer's ("\r\n" stays).
Applied applyEdits(const std::string& text, const std::vector<Edit>& edits, size_t selStart,
                   size_t selEnd);
inline Applied applyEdits(const std::string& text, const std::vector<Edit>& edits, size_t caret) {
    return applyEdits(text, edits, caret, caret);
}

// The pending edit as the bar shows it: the changed runs of `before` ->
// `after` (UTF-16, as NSString) with `context` equal lines around each,
// unified: a run's old lines, then its new ones, a Gap between runs that
// are apart. Words that changed inside a changed pair of lines are marked
// (SideBySide::wordChanges). Lines are split at "\n", a "\r" before it
// left out. Past `maxLines` lines the rest is one Gap whose `more` says how
// many lines it hides.
struct DiffLine {
    enum Kind { Context, Removed, Added, Gap } kind = Context;
    int oldLine = 0, newLine = 0;   // 1-based, 0 for the side it is not on
    std::u16string text;
    std::vector<std::pair<size_t, size_t>> words;   // (start, length) in text
    size_t more = 0;                                // Gap: lines left out
};
std::vector<DiffLine> pendingDiff(const std::u16string& before, const std::u16string& after,
                                  size_t context = 1, size_t maxLines = 400);
// How many lines the change touches: per run, the larger of its old and new
// line counts.
size_t changedLines(const std::u16string& before, const std::u16string& after);

// Keep, after the user changed the buffer while an edit waited (UTF-16,
// as NSString). The blocks are made again on the buffer as it is now, with
// the selection they were first placed by moved to where its text is now
// (moveOffset: through a line diff of `base` to `now`, LineDiff::mapOffset),
// so a SEARCH part found in several places picks the same one. Whether
// what that made (`now` to `next`) is the change the bar showed (`base` to
// `pending`): the same runs of lines taken out and put in, each where the
// user's own edits moved it. When it is not, the bar shows the new diff
// and Keep waits for another press, never putting the edit somewhere the
// user was not shown.
size_t moveOffset(const std::u16string& base, const std::u16string& now, size_t pos);
bool sameChange(const std::u16string& base, const std::u16string& pending,
                const std::u16string& now, const std::u16string& next);

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
