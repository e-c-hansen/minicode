// AgentTranscript.h — pure C++17, no dependencies. Reading an agent's
// transcript: the files Claude Code writes as it works, one JSON object per
// line (`~/.claude/projects/<encoded-cwd>/<sessionId>.jsonl`, and a
// subagent's `<sessionId>/subagents/agent-<id>.jsonl`). The Agents panel's
// index (AgentIndex.h) and the Evidence view (AgentEvidence.h) are built on it.
//
// Three rules shape it:
//
// - Read only. Nothing here writes, moves or deletes anything; the Fs has no
//   way to.
// - The format is Claude Code's own and will change. A `Format` turns one line
//   into a `Line` of the few things MiniCode uses (who wrote it, when, in
//   which folder, and its text, tool calls and tool results); anything else in
//   the line is ignored, and a line that is not one well-formed JSON object is
//   skipped and counted, never fatal. `claudeCode()` is the only format so
//   far; another agent's transcripts would be a second Format, and the index
//   and evidence code would not change.
// - Nothing large is kept. A line is skimmed, not parsed into a tree: values
//   MiniCode does not need (a Write's whole file, a Bash command's output, the
//   copies of each request Claude Code logs) are stepped over without being
//   decoded. A tool result's output is remembered as a `Span`, its place in
//   the file, and read back on demand by `readOutput`, which reads only the
//   head and the tail of it.
//
// Reading is incremental (`Tail`): from the offset the last read stopped at,
// whole lines only; a last line still being written is left for the next read.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace AgentTranscript {

// ------------------------------------------------------------------ files
struct Stat {
    bool exists = false;
    bool isDir = false;
    uint64_t size = 0;
    double mtime = 0;       // Unix seconds
    uint64_t inode = 0;
    uint64_t device = 0;
};

// What the readers may do to the disk: list, stat and read, nothing else.
// Tests give a scratch tree through posixFs(), or a wrapper that counts bytes.
struct Fs {
    // Names in a folder ("." and ".." left out); empty when it cannot be read.
    std::function<std::vector<std::string>(const std::string &dir)> list;
    // Follows symlinks; exists == false when the path is not there.
    std::function<Stat(const std::string &path)> stat;
    // Up to maxBytes from `offset` into *out (replacing it). False when the
    // file cannot be opened; reading past the end gives an empty string.
    std::function<bool(const std::string &path, uint64_t offset, size_t maxBytes,
                       std::string *out)> read;
    // Optional: the path with symlinks resolved ("" or the path itself when
    // it cannot be). Used to compare folders spelled /tmp and /private/tmp.
    std::function<std::string(const std::string &path)> realpath;
};

Fs posixFs();

// "2026-11-05T19:39:56.507Z" (or with an offset, "+02:00") -> Unix seconds;
// 0 when it does not read.
double parseTime(const std::string &iso);

// ------------------------------------------------------------------ lines
// A place in a transcript file: `length` bytes from `offset`.
struct Span {
    uint64_t offset = 0;
    uint64_t length = 0;
    bool empty() const { return length == 0; }
};

struct Block {
    enum class Kind { Text, ToolUse, ToolResult };
    Kind kind = Kind::Text;
    // Text: the text, cut at kTextCap bytes (textCut set).
    std::string text;
    bool textCut = false;
    // ToolUse: the call's id and tool name. ToolResult: the id it answers.
    std::string id;
    std::string name;
    // ToolUse input, only the fields MiniCode uses, each cut at kInputCap:
    // file_path (or notebook_path), command, description.
    std::string filePath;
    std::string command;
    bool commandCut = false;
    std::string description;
    // ToolResult: is_error, and where the output's text is in the file: a
    // JSON string, quotes included (the content itself when it is a string,
    // else the first text block's text). Empty when there is none.
    bool isError = false;
    Span output;
};

struct Line {
    // The line's `type`: "user", "assistant", "system", ... (as written).
    std::string type;
    double time = 0;            // timestamp, Unix seconds (0 when none)
    std::string sessionId;
    std::string agentId;        // a subagent's lines carry its id
    std::string cwd;
    std::string gitBranch;
    std::string version;        // the agent's version, for tracing breakage
    std::string messageId;      // message.id (an assistant reply's lines share it)
    std::string aiTitle;        // the title the agent gave the session, if any
    bool sidechain = false;
    bool meta = false;          // isMeta or isCompactSummary: not typed by anyone
    // A prompt someone (a person, or a parent agent for a subagent) wrote:
    // a user message of text, not a tool result, a notification or a
    // command's output.
    bool prompt = false;
    // On a tool result line, the subagent an Agent call started
    // (toolUseResult.agentId), "" otherwise.
    std::string spawnedAgent;
    std::vector<Block> blocks;
    Span span;                  // the whole line in the file, newline excluded

    void clear();
};

constexpr size_t kTextCap = 64 * 1024;
constexpr size_t kInputCap = 8 * 1024;

// One agent's transcript format.
class Format {
public:
    virtual ~Format() = default;
    virtual const char *name() const = 0;
    // Reads one line (no newline) found at `offset` in its file. False when
    // the line is not a well-formed record of this format; the caller counts
    // it and goes on. `out` is cleared first.
    virtual bool parseLine(const char *data, size_t length, uint64_t offset,
                           Line *out) const = 0;
};

// Claude Code's JSONL.
const Format &claudeCode();

// ------------------------------------------------------------- incremental
struct ReadStats {
    uint64_t bytesRead = 0;     // bytes asked of the Fs
    int lines = 0;              // complete lines read
    int badLines = 0;           // lines the format refused
    bool reset = false;         // the file was replaced or shrank: read from 0
};

// Reads one file in whole lines from where the last read stopped.
struct Tail {
    std::string path;
    uint64_t offset = 0;        // just past the last complete line read
    uint64_t inode = 0, device = 0;
    // Read what was added since. When the file was replaced (another inode)
    // or is shorter than `offset`, stats->reset is set and reading starts from
    // 0, so the caller must throw away what it built from the old file first:
    // `onReset` is called before any line in that case. A last line with no
    // newline yet is left for the next call. `onLine` returns false to stop
    // reading after that line (the next call goes on from there). False when
    // the file is gone.
    bool read(const Fs &fs, const Format &format,
              const std::function<bool(const Line &)> &onLine,
              const std::function<void()> &onReset, ReadStats *stats,
              size_t chunk = 1 << 20);
};

// ------------------------------------------------------------- output
// The head and tail of a tool result's output, read without loading the rest.
struct Output {
    std::vector<std::string> head;   // the first lines
    std::vector<std::string> tail;   // the last lines, not overlapping head
    // Lines between head and tail; -1 when unknown (only the ends were read).
    long long omitted = 0;
    bool whole = false;              // every line is in head + tail
    uint64_t bytes = 0;              // the output's size as stored (encoded)
    // Claude Code keeps an output too large for the transcript in a file of
    // its own and stores a preview: "<persisted-output> ... saved to: <path>".
    // That path, when this is such a preview ("" otherwise).
    std::string persistedPath;
};

// Reads the output at `span` of `path` (a JSON string, as Block::output
// gives). At most about 2 x `window` bytes are read: an output larger than
// that has only its ends read, and `omitted` is -1.
Output readOutput(const Fs &fs, const std::string &path, const Span &span,
                  int headLines = 20, int tailLines = 20, size_t window = 64 * 1024);

// The whole output, decoded, up to maxBytes of text (*cut set when it was cut).
std::string readOutputText(const Fs &fs, const std::string &path, const Span &span,
                           size_t maxBytes, bool *cut = nullptr);

// The same for a plain text file (a persisted output): its first and last lines.
Output readTextFile(const Fs &fs, const std::string &path, int headLines = 20,
                    int tailLines = 20, size_t window = 64 * 1024);

// The path in a "<persisted-output>" preview, or "".
std::string persistedOutputPath(const std::string &text);
// A command that went on in the background ("Command running in background
// with ID: x. Output is being written to: <path>", or "moved to the
// background") says where its output goes: that path, or "".
std::string backgroundOutputPath(const std::string &text);

// Decodes a JSON string's contents from raw bytes that may start anywhere
// inside it (a tail window): the bytes up to the first escape that can be
// told apart for certain are dropped. Exposed for tests.
std::string decodeJsonFragment(const char *p, size_t n, bool fromStart, bool *ok = nullptr);

}  // namespace AgentTranscript
