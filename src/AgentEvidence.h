// AgentEvidence.h — pure C++17, no dependencies. The Evidence view's model:
// for one agent session, every shell command it ran, in order, with what the
// agent said it was for, whether it failed, and where its output is; which of
// them ran tests, and the tests' own summary lines; and the session's final
// message, so a claim and the evidence for it sit side by side (R3.1-R3.3).
//
// It reports facts and draws no conclusions (R3.4): a test summary is the
// numbers the test runner printed, `isError` is what the agent's harness
// recorded, and nothing compares them with what the final message says.
//
// Outputs stay on disk. A Command carries its output's place in the
// transcript (`output`, a Span), and the view reads it with
// AgentTranscript::readOutput(fs, transcript, cmd.output, 20, 20), which
// reads only the first and last 64 KB however large it is; a persisted
// output (Claude Code moves big ones to a file of its own) is
// `persistedPath`, read with AgentTranscript::readTextFile. Only a test
// command's tail is read while indexing, to find its summary.
//
// Use from a GUI: an `Evidence::Reader` per open Evidence view, on a
// background queue; `update()` on open and whenever the index says the
// session's transcript grew, then copy `evidence()` to the main thread.
#pragma once
#include "AgentTranscript.h"

#include <map>
#include <string>
#include <vector>

namespace AgentEvidence {

using AgentTranscript::Fs;
using AgentTranscript::Span;

enum class TestTool { None, Pytest, Make, Ctest, Npm, Cargo, Go, Unittest, Tox, Nox, Jest, Vitest };
const char *testToolName(TestTool t);   // "pytest", "make", ... ("" for None)

// The test runner a shell command runs, if any: pytest (also python -m
// pytest, uv run pytest, poetry run pytest), make test / make check (also
// with -C dir, -jN or VAR=x), ctest, npm/pnpm/yarn test and npm run test,
// cargo test, go test, python -m unittest, tox, nox, jest and vitest (also
// through npx, bunx, uvx, pipx run, yarn, pnpm exec and the like). Any part of a pipeline or && chain counts
// (`cd x && make test 2>&1 | tail`), and env, time, timeout and the like in
// front are looked through.
TestTool testCommand(const std::string &command);

struct Summary {
    bool found = false;
    std::string format;     // "pytest", "ctest", "cargo", "go", "count", "unittest",
                            // "jest", "vitest", "tox", "nox"
    int passed = 0;
    int failed = 0;
    int errors = 0;
    int skipped = 0;
    std::string line;       // the summary line as printed (the last, for cargo and go)
};

// The summary in a test run's output, read from its last lines. Formats:
//   pytest  "=== 3 failed, 10 passed, 1 skipped in 1.2s ===", "5 passed in 0.03s"
//   ctest   "83% tests passed, 2 tests failed out of 12"
//   cargo   "test result: ok. 12 passed; 0 failed; 1 ignored; ..." (summed over binaries)
//   go      "ok  \tpkg\t0.1s" and "FAIL\tpkg\t0.2s" lines (counted as packages; only with
//           hint Go)
//   count   "4466 passed, 0 failed" (mutible's own `make test`)
//   unittest "Ran 12 tests in 0.004s" and the verdict after it, "OK (skipped=1)"
//           or "FAILED (failures=2, errors=1)"; `line` is both, joined by "; "
//   jest    "Tests:       1 failed, 2 skipped, 10 passed, 13 total"
//   vitest  "Tests  1 failed | 10 passed | 1 skipped (12)" (not "Test Files")
//   tox     "  py311: OK (1.2 seconds)", "  py312: FAIL code 1 (...)" (tox 4) and
//           "  py311: commands succeeded", "ERROR:   py312: commands failed" (tox 3),
//           counted as environments; only with hint Tox, else what ran inside
//   nox     "nox > Session tests-3.12 was successful in 2 seconds.", "... failed.",
//           "... skipped: ...", counted as sessions; only with hint Nox
// ANSI color codes are ignored. `hint` is what testCommand said.
Summary parseSummary(const std::vector<std::string> &lines, TestTool hint = TestTool::None);
Summary parseSummary(const std::string &text, TestTool hint = TestTool::None);

struct Command {
    std::string toolUseId;
    std::string description;    // what the agent said it was for ("" when it gave none)
    std::string command;        // cut at 8 KB (commandCut)
    bool commandCut = false;
    double time = 0;            // when it was asked for
    std::string cwd;            // the session's cwd when it was asked for
    bool answered = false;      // its result has arrived
    bool isError = false;       // the result was an error (a non-zero exit, a refusal)
    Span output;                // where the result's text is in the transcript
    // For a test command whose output is in a file of its own: Claude Code
    // moved a large one out of the transcript, or the command went on in the
    // background (inBackground) and writes there. The summary is read from
    // that file's tail when the result arrives; a background run still
    // going has none yet (read the file again for it).
    std::string persistedPath;
    bool inBackground = false;
    TestTool test = TestTool::None;
    Summary summary;            // for a test command, once answered
    // What it wrote to stderr, as Claude Code kept it apart (cut at 8 KB;
    // "" for none). Claude Code also appends it to the output, so its
    // lines are the output's last ones (stderrLineCount).
    std::string stderrText;
};

// A file the session changed with Edit, MultiEdit, Write or NotebookEdit:
// the Evidence page lists these beside the commands, each leading to that
// file's page (AgentEdits.h reads what the call changed).
struct FileEdit {
    std::string toolUseId;
    std::string tool;           // "Edit", "MultiEdit", "Write", "NotebookEdit"
    std::string path;           // file_path (or notebook_path) as written
    double time = 0;
    bool answered = false;
    bool isError = false;       // refused: Claude Code did not apply it
};

// The session item by item (A6), so the page can give each kind its own
// look: what was asked, what Claude said, each tool call, and what came in
// from elsewhere. Commands and file edits keep their own lists above; their
// entries point into them.
enum class EntryKind {
    Prompt,           // what the person asked (or the parent agent, in a subagent): Markdown
    Message,          // Claude's text (a reply's text blocks joined): Markdown
    Command,          // a Bash call: commands[index]
    Edit,             // an Edit, MultiEdit, Write or NotebookEdit: edits[index]
    Tool,             // another tool call, shown on one line: Read, Grep, Glob, WebFetch, ...
    Subagent,         // an Agent (or Task) call: a subagent started
    SentMessage,      // SendMessage: to another session or a subagent, Markdown
    ReceivedMessage,  // a message from another Claude Code session: Markdown
    Notification,     // a background task's end, a slash command run
    Hook,             // what a hook said
};
const char *entryKindName(EntryKind k);   // "prompt", "message", ... for checks and logs

struct Entry {
    EntryKind kind = EntryKind::Message;
    double time = 0;
    size_t index = 0;           // Command: commands[index]; Edit: edits[index]
    std::string toolUseId;
    // Tool, Subagent, SentMessage: the tool's name; Hook: the hook's.
    std::string tool;
    // Prompt, Message, SentMessage, ReceivedMessage: the Markdown (cut at
    // kEntryTextCap, textCut set). Tool: what it was asked (a pattern, an
    // address, a query; toolDetail). Subagent: the task's description.
    // Notification and Hook: what they said.
    std::string text;
    bool textCut = false;
    std::string path;           // Tool: the file it names (Read's file_path), "" for none
    // SentMessage: whom to, ReceivedMessage: whom from: a session's name
    // when one is known (a message from it named it, or the subagent's
    // task), else as the transcript spells it.
    std::string peer;
    std::string subagentType;   // Subagent: its type ("Explore", ...)
    bool answered = false;
    bool isError = false;       // a tool's error result, a failed task or hook
    Span output;                // Tool, Subagent, SentMessage: the result's text
};

constexpr size_t kEntryTextCap = 16 * 1024;

struct Evidence {
    std::string transcript;
    std::string cwd;                    // the session's first cwd
    std::vector<Command> commands;      // in the order they were asked for
    std::vector<FileEdit> edits;        // likewise
    std::vector<Entry> entries;         // everything, in order (A6)
    bool subagent = false;              // a subagent's transcript: its prompt is its task
    // The last assistant message that had text: its text blocks joined.
    std::string finalMessage;
    bool finalCut = false;
    double finalTime = 0;
    int lastTest = -1;                  // index of the last test command, -1 for none
    std::vector<std::string> versions;  // agent versions seen
    int badLines = 0;
    const Command *lastTestCommand() const {
        return lastTest < 0 ? nullptr : &commands[(size_t)lastTest];
    }
};

// Reads one transcript incrementally.
class Reader {
public:
    explicit Reader(std::string transcript,
                    const AgentTranscript::Format &format = AgentTranscript::claudeCode());
    // Reads what was added. True when the evidence changed. A transcript
    // replaced or cut shorter is read again from the start.
    bool update(const Fs &fs);
    const Evidence &evidence() const { return ev_; }
    uint64_t bytesRead() const { return bytesRead_; }

private:
    const AgentTranscript::Format &format_;
    AgentTranscript::Tail tail_;
    Evidence ev_;
    std::map<std::string, size_t> pending_;   // Bash tool_use id -> commands[]
    std::map<std::string, size_t> pendingEdits_;   // edit tool_use id -> edits[]
    std::map<std::string, size_t> pendingEntries_; // other tool_use ids -> entries[]
    std::map<std::string, std::string> names_;     // session or subagent id -> its name
    std::string entryMessageId_;                   // the Message entry last added, if last
    std::string lastMessageId_;
    bool sideFirst_ = false, sawFirst_ = false;
    uint64_t bytesRead_ = 0;
};

// One-shot: the evidence for a transcript.
Evidence read(const Fs &fs, const std::string &transcript);

// Text with ANSI escape sequences (colors, cursor moves) removed.
std::string stripAnsi(const std::string &text);

// A tool call's one line on the page, after its name and path: Grep's
// pattern and folder ("TODO in src"), Glob's pattern, WebFetch's address,
// WebSearch's query, a Skill's name, else its description or command.
std::string toolDetail(const AgentTranscript::Block &b);

// How many of `lines` (an output's last lines, as read) are the command's
// stderr: Claude Code puts stderr after stdout, so these are the trailing
// lines that end the stderr text, compared with trailing spaces ignored.
size_t stderrLineCount(const std::vector<std::string> &lines, const std::string &stderrText);

// A Claude Code background task's notification ("<task-notification> ...
// <status>completed</status><summary>Background command "x" completed
// (exit code 0)</summary>"): its summary, and whether its status says it
// failed. False when the text is not one.
bool taskNotification(const std::string &text, std::string *summary, bool *failed);

}  // namespace AgentEvidence
