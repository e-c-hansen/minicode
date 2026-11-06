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

enum class TestTool { None, Pytest, Make, Ctest, Npm, Cargo, Go };
const char *testToolName(TestTool t);   // "pytest", "make", ... ("" for None)

// The test runner a shell command runs, if any: pytest (also python -m
// pytest, uv run pytest, poetry run pytest), make test / make check (also
// with -C dir, -jN or VAR=x), ctest, npm/pnpm/yarn test and npm run test,
// cargo test, go test. Any part of a pipeline or && chain counts
// (`cd x && make test 2>&1 | tail`), and env, time, timeout and the like in
// front are looked through.
TestTool testCommand(const std::string &command);

struct Summary {
    bool found = false;
    std::string format;     // "pytest", "ctest", "cargo", "go", "count"
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
//   count   "4466 passed, 0 failed" (minicode's own `make test`)
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
};

struct Evidence {
    std::string transcript;
    std::string cwd;                    // the session's first cwd
    std::vector<Command> commands;      // in the order they were asked for
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
    std::string lastMessageId_;
    bool sideFirst_ = false, sawFirst_ = false;
    uint64_t bytesRead_ = 0;
};

// One-shot: the evidence for a transcript.
Evidence read(const Fs &fs, const std::string &transcript);

// Text with ANSI escape sequences (colors, cursor moves) removed.
std::string stripAnsi(const std::string &text);

}  // namespace AgentEvidence
