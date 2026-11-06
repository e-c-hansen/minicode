// ControlProtocol.h — pure C++. The control channel's protocol: what an
// agent, a script or the `minicode` command line can ask a running minicode
// to do, read from and written to its Unix socket (ControlServer.mm).
//
// One JSON object per line, request then response:
//   {"v":1,"id":7,"cmd":"open","args":{...}}
//   {"id":7,"ok":true,"result":...}   or   {"id":7,"ok":false,"error":"..."}
// Every command and every argument is in one table (commands()), which
// validates requests before any handler sees them: an unknown command, an
// unknown or misspelled field, or a value of the wrong type is an error
// answer, never a crash. The Dispatcher routes a valid request to the
// handler the app registered for its command, and makes sure exactly one
// answer goes back.
//
// Also here, so they are tested: the command line's parsing (parseCommandLine,
// argv to one request), path:line:col specs, tour files, note text, the
// socket folder's file names, and pretty-printed JSON for `state --pretty`.
#pragma once
#include "Json.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ControlChannel {

constexpr int kVersion = 1;
// Longest note shown, in characters (code points); longer text is cut.
constexpr size_t kMaxNoteChars = 500;
// The most of a selection `state` and `selection` return, in bytes.
constexpr size_t kMaxSelectionBytes = 64 * 1024;

// ------------------------------------------------------------ the table
enum class ArgType { String, Int, Bool, Objects };

struct ArgSpec {
    std::string name;
    ArgType type;
    bool required = false;
    std::vector<ArgSpec> fields;   // for Objects: each element's fields
};

struct CommandSpec {
    std::string name;
    std::string summary;           // one line, for errors and help
    std::vector<ArgSpec> args;
};

const std::vector<CommandSpec>& commands();
const CommandSpec* findCommand(const std::string& name);

// ------------------------------------------------------- requests, answers
struct Request {
    Json id;            // as sent (a number or a string)
    std::string cmd;
    Json args;          // an object, validated against the table
};

// Parses and validates one request line. On failure returns false with
// `error` set, and `out.id` holds the request's id when it could be read
// (so the error answer still names it), else null.
bool parseRequest(const std::string& line, Request& out, std::string& error);
// Checks an args object against a command's table entry.
bool validateArgs(const CommandSpec& spec, const Json& args, std::string& error);

// The answer lines, each ending in "\n".
std::string okLine(const Json& id, const Json& result);
std::string errorLine(const Json& id, const std::string& error);

// A request line as the command line sends it.
std::string requestLine(const Json& id, const std::string& cmd, const Json& args);

// What a handler hands back.
struct Result {
    bool ok = true;
    Json value;          // the result, when ok
    std::string error;   // when not
    static Result success(Json v = Json()) { Result r; r.value = std::move(v); return r; }
    static Result failure(std::string e) { Result r; r.ok = false; r.error = std::move(e); return r; }
};

class Dispatcher {
public:
    // Called exactly once per request; may be called later (after git ran).
    using Respond = std::function<void(const Result&)>;
    using Handler = std::function<void(const Request&, Respond)>;

    void on(const std::string& cmd, Handler handler);
    bool handles(const std::string& cmd) const;
    // One request line in; `send` gets the answer line once. A handler that
    // throws answers with an error; a second answer from a handler is dropped.
    void handle(const std::string& line, std::function<void(const std::string&)> send) const;

private:
    std::vector<std::pair<std::string, Handler>> handlers_;
};

// --------------------------------------------------------- the command line
// `path`, `path:12` or `path:12:5`. When the whole text names a file that
// exists (`exists`), it is the path, colons and all.
struct PathSpec {
    std::string path;
    int line = 0;     // 1-based, 0 when not given
    int column = 0;
};
PathSpec splitPathSpec(const std::string& spec,
                       const std::function<bool(const std::string&)>& exists);
// `path` made absolute against `cwd` and tidied ("a/./b/../c" -> "a/c"),
// "~/" expanded with `home`.
std::string absolutePath(const std::string& path, const std::string& cwd,
                         const std::string& home);

enum class Output { None, Id, Json, Text };

struct CliCommand {
    bool help = false;          // `minicode help`, or -h after a subcommand
    std::string helpTopic;      // the subcommand asked about, or "" for all
    std::string cmd;            // the protocol command
    Json args = Json::object();
    Output output = Output::None;
    std::string outputField;    // the result field printed (Id, Text)
    bool pretty = false;        // state --pretty
    int pid = 0;                // --pid: this process only
    std::string tourFile;       // tour: read by the caller, then tourStops()
    std::string launchFolder;   // where a launched minicode opens (R1.4)
    std::string error;          // a usage error (exit code 2)
};

// argv after the program name, the subcommand first. Paths are made
// absolute here; files are not read (see tourStops).
CliCommand parseCommandLine(const std::vector<std::string>& argv, const std::string& cwd,
                            const std::string& home,
                            const std::function<bool(const std::string&)>& exists);
// The subcommands, which scripts/minicode-launcher passes on to the client
// (its own list; control:launcher-words checks the two agree), and whether
// `word` is one of them.
const std::vector<std::string>& subcommands();
bool isSubcommand(const std::string& word);
// Whether the command line starts minicode when none is running. Reads
// (state, selection, term-output) do not: there would be nothing to read,
// and an agent should not reopen an app the user has just quit.
bool launchesApp(const std::string& cmd);
// Whether `ref` may be handed to git as a revision: not empty, and not
// starting with "-" (which git would read as an option).
bool isSafeRef(const std::string& ref);
// The help text for every subcommand, or for one.
std::string usageText(const std::string& topic = "");

// A tour file: a list of stops, or {"stops": [...]}, each
// {"path", "line", "to"?, "note", "column"?}. Relative paths are taken
// against `cwd`. Fills `stops` with protocol objects for the `tour` command.
bool tourStops(const std::string& text, const std::string& cwd, const std::string& home,
               Json& stops, std::string& error);

// Note text as shown: control characters (C0 and C1), line breaks and the
// line and paragraph separators become spaces, runs of spaces one; bidi
// controls (U+200E-200F, U+202A-202E, U+2066-2069) are dropped; trimmed, and
// cut to kMaxNoteChars characters with "…". The app cleans every note it is
// sent, not only the command line's.
std::string cleanNoteText(const std::string& text);
// Text cut to at most `maxBytes` bytes on a UTF-8 character boundary.
std::string clipUtf8(const std::string& text, size_t maxBytes);

// ------------------------------------------------------ the socket folder
// "<pid>.sock" -> pid, else 0.
int pidFromSocketName(const std::string& name);
std::string socketName(int pid);
// The socket to talk to among those found: `wantPid` when given and alive,
// else the newest (by start time) whose process is alive. 0 when none.
struct SocketEntry { int pid; int64_t started; };
int chooseSocket(const std::vector<SocketEntry>& found, int wantPid,
                 const std::function<bool(int)>& alive);
// Every candidate in the order to try them: the live ones, newest first,
// each pid once (the same one can be found in two folders), or only
// `wantPid`. The command line moves on to the next when one refuses the
// connection (a socket left by a process whose pid was reused).
std::vector<int> rankSockets(const std::vector<SocketEntry>& found, int wantPid,
                             const std::function<bool(int)>& alive);

// JSON with two-space indents, for `state --pretty`.
std::string prettyJson(const Json& value);

}  // namespace ControlChannel
