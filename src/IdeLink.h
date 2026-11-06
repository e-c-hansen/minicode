// IdeLink.h — pure C++. The protocol of the Claude Code IDE link, with no
// I/O: what Claude Code's `/ide` speaks to VS Code and JetBrains, read from
// Claude Code 2.1.319 and its VS Code extension 2.1.318 (see "The Claude
// Code IDE link" in CLAUDE.md for what was read where).
//
// An IDE announces itself with a lock file, ~/.claude/ide/<port>.lock
// ($CLAUDE_CONFIG_DIR/ide when that is set), mode 0600, holding JSON: pid,
// workspaceFolders, ideName, transport "ws", runningInWindows and
// authToken. Claude Code started in one of those folders (or with
// CLAUDE_CODE_SSE_PORT set to the port) opens a WebSocket to
// ws://127.0.0.1:<port> with the subprotocol "mcp" and the token in an
// X-Claude-Code-Ide-Authorization header, and talks MCP (JSON-RPC 2.0, one
// message per text frame) to it: it calls the IDE's tools (openFile,
// openDiff, getDiagnostics, getCurrentSelection, close_tab, ...) and the
// IDE sends it selection_changed notifications.
//
// Here: SHA-1 and base64 for the handshake, the HTTP upgrade read and
// answered, RFC 6455 frames read (masked, fragmented, control frames) and
// written, the MCP requests answered over a table of tools, the lock file's
// text, and the JSON shapes each tool answers with. IdeLink.mm (macOS) does
// the sockets and maps the tools onto the editor.
#pragma once
#include "Json.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace IdeLink {

// ---- hashing and encoding (for Sec-WebSocket-Accept)
std::string sha1(const std::string &data);         // 20 raw bytes
std::string toHex(const std::string &bytes);
std::string base64(const std::string &bytes);
bool fromBase64(const std::string &text, std::string &out);   // false when malformed
// base64(sha1(key + the RFC's GUID)).
std::string acceptKey(const std::string &clientKey);
// Compares in time that depends only on the lengths.
bool sameSecret(const std::string &a, const std::string &b);

// ---- the opening handshake
// The header the token travels in, and the subprotocol Claude Code asks for.
extern const char *const kAuthHeader;     // "X-Claude-Code-Ide-Authorization"
extern const char *const kSubprotocol;    // "mcp"
const size_t kMaxHandshakeBytes = 16384;

struct Handshake {
    enum State { Incomplete, Accepted, Refused };
    State state = Incomplete;
    std::string response;   // what to write back (an error page when refused)
    std::string error;      // why it was refused, for logs and tests
    size_t consumed = 0;    // bytes of the buffer that were the request
};
// Reads an HTTP upgrade request from what arrived so far. Accepted only for
// GET with Upgrade: websocket, Connection: upgrade, Sec-WebSocket-Version 13,
// a key of 16 bytes, a Host on the loopback (127.0.0.1, localhost or [::1],
// so a page that rebinds a name to 127.0.0.1 is turned away), and the token
// in kAuthHeader. The answer names the "mcp" subprotocol when it was asked
// for.
Handshake readHandshake(const std::string &buffer, const std::string &token);

// ---- frames (RFC 6455 section 5)
enum class Opcode : uint8_t { Continuation = 0, Text = 1, Binary = 2, Close = 8, Ping = 9, Pong = 10 };

struct Message {
    Opcode op = Opcode::Text;   // Text, Binary, Close, Ping or Pong
    std::string payload;        // a close's reason, without its code
    uint16_t closeCode = 0;     // a close's code (1005 when it had none)
};

// Reads what a client sends: every frame must be masked, control frames
// short and whole, and a message (all its fragments) at most `maxMessage`
// bytes. A broken stream is an error with the close code to answer with
// (1002 protocol error, 1007 bad UTF-8 in text, 1009 too big).
class FrameReader {
public:
    explicit FrameReader(size_t maxMessage = 64u << 20) : max_(maxMessage) {}
    void feed(const char *data, size_t n) { buf_.append(data, n); }
    void feed(const std::string &data) { buf_ += data; }
    enum Result { Ready, NeedMore, Error };
    Result next(Message &out);
    uint16_t errorCode() const { return errorCode_; }
    const std::string &error() const { return error_; }
    size_t buffered() const { return buf_.size(); }

private:
    std::string buf_;
    std::string partial_;          // a fragmented message so far
    Opcode partialOp_ = Opcode::Text;
    bool inMessage_ = false;
    size_t max_;
    uint16_t errorCode_ = 0;
    std::string error_;
    Result fail(uint16_t code, const std::string &why);
};

// One whole frame from the server (never masked).
std::string encodeFrame(Opcode op, const std::string &payload);
std::string encodeClose(uint16_t code, const std::string &reason = "");
// A client's frame, masked with `mask` (tests, and anything that dials in).
std::string encodeClientFrame(Opcode op, const std::string &payload, uint32_t mask,
                              bool fin = true);
bool validUtf8(const std::string &s);

// ---- MCP over JSON-RPC
// A tool's answer: MCP content blocks of text, or an error (isError).
struct ToolResult {
    std::vector<std::string> texts;
    bool error = false;
    static ToolResult text(std::string t) { ToolResult r; r.texts.push_back(std::move(t)); return r; }
    static ToolResult failure(std::string t) { ToolResult r = text(std::move(t)); r.error = true; return r; }
    Json toJson() const;   // {"content":[{"type":"text","text":...}],"isError":...}
};

struct Tool {
    std::string name, description;
    Json inputSchema;                                        // a JSON Schema object
    // Answers once, perhaps later (openDiff waits for the user).
    std::function<void(const Json &args, std::function<void(ToolResult)> respond)> run;
};

// A property of an input schema.
Json schemaProperty(const char *type, const char *description);
Json objectSchema(std::vector<std::pair<std::string, Json>> properties,
                  std::vector<std::string> required = {});

// The MCP side of one connection: initialize, ping, tools/list and
// tools/call; notifications from the client go to onNotification. `send`
// gets each message to write as a text frame.
class McpServer {
public:
    McpServer(std::string name, std::string version);
    void addTool(Tool tool);
    const std::vector<Tool> &tools() const { return tools_; }
    std::function<void(const std::string &text)> send;
    std::function<void(const std::string &method, const Json &params)> onNotification;
    void handle(const std::string &text);
    bool initialized() const { return initialized_; }
    // A notification to the client ({"jsonrpc":"2.0","method":...,"params":...}).
    static std::string notification(const std::string &method, const Json &params);

private:
    std::string name_, version_;
    std::vector<Tool> tools_;
    bool initialized_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    void reply(const Json &id, Json result);
    void replyError(const Json &id, int code, const std::string &message);
};

// The protocol versions minicode has seen Claude Code ask for answer with
// the one asked; anything else gets this.
extern const char *const kProtocolVersion;   // "2025-06-18"

// ---- the lock file
struct LockInfo {
    int pid = 0;
    std::vector<std::string> workspaceFolders;
    std::string ideName, transport, authToken;
};
std::string lockFileName(int port);              // "<port>.lock"
int portFromLockName(const std::string &name);   // 0 when not one
std::string lockFileText(const LockInfo &info);
bool parseLockFile(const std::string &text, LockInfo &out);
// The name minicode writes as ideName.
extern const char *const kIdeName;               // "minicode"
// Whether a lock file found at start is one minicode left behind and may
// remove: minicode's own name, another pid than ours, and that pid gone.
// Anyone else's lock file (VS Code's, a JetBrains IDE's, a running
// minicode's) is left alone.
bool isStaleOwnLock(const LockInfo &info, int ourPid, bool pidAlive);

// ---- what the tools answer with
// "file://" + path, as Claude Code writes it (no percent-encoding), and back
// (a percent-encoded one decoded too). "" when not a file URI.
std::string fileUri(const std::string &path);
std::string pathFromUri(const std::string &uri);

// A selection in VS Code's shape: lines 0-based, characters in UTF-16 units.
struct Selection {
    std::string filePath, text;
    int startLine = 0, startCharacter = 0, endLine = 0, endCharacter = 0;
};
// {"text","filePath","fileUrl","selection":{"start","end","isEmpty"}}: the
// selection_changed notification's params, and getCurrentSelection's
// answer with "success":true added.
Json selectionJson(const Selection &s);

struct Diagnostic {
    int line = 0, character = 0, endLine = 0, endCharacter = 0;
    int severity = 1;   // 1 error ... 4 hint
    std::string message, source, code;
};
struct FileDiagnostics {
    std::string path;
    std::vector<Diagnostic> diagnostics;
};
const char *severityName(int severity);   // "Error", "Warning", "Information", "Hint"
// [{"uri","diagnostics":[{"message","severity","range","source","code"}]}]
Json diagnosticsJson(const std::vector<FileDiagnostics> &files);

// openFile's startText / endText: the first match of `start`, through the
// first match of `end` after it (to its line's end with `toLineEnd`), in
// UTF-16 offsets. found is false when `start` is not there; endFound false
// when `end` was asked for and is not after it (the range is then empty, at
// start's match), as VS Code's extension does.
struct TextRange {
    bool found = false, endFound = false;
    size_t start = 0, end = 0;
};
TextRange findTextRange(const std::u16string &text, const std::u16string &start,
                        const std::u16string &end, bool toLineEnd);
// The 0-based line and UTF-16 column of an offset.
void lineAndColumn(const std::u16string &text, size_t offset, int &line, int &column);

// openDiff's answers: accepted (with the text to write, which Claude Code
// writes itself) and rejected, as Claude Code reads them.
ToolResult diffAccepted(const std::string &newText);
ToolResult diffRejected(const std::string &tabName);

// Path handling for tools that take a filePath: relative ones are taken
// from the window's folder, as VS Code takes them from its first folder.
std::string resolvePath(const std::string &path, const std::string &root);

}  // namespace IdeLink
