// IdeLink.cpp — see IdeLink.h.
#include "IdeLink.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>

namespace IdeLink {

const char *const kAuthHeader = "X-Claude-Code-Ide-Authorization";
const char *const kSubprotocol = "mcp";
const char *const kProtocolVersion = "2025-06-18";
const char *const kIdeName = "minicode";

// ------------------------------------------------------------------ SHA-1
// FIPS 180-4. Only the handshake uses it, as RFC 6455 says to; nothing here
// relies on it for secrecy.
static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

std::string sha1(const std::string &data) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m = data;
    const uint64_t bits = (uint64_t)data.size() * 8;
    m += '\x80';
    while (m.size() % 64 != 56) m += '\0';
    for (int i = 7; i >= 0; i--) m += (char)((bits >> (i * 8)) & 0xFF);
    for (size_t chunk = 0; chunk < m.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            const unsigned char *p = (const unsigned char *)m.data() + chunk + i * 4;
            w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        }
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::string out;
    for (uint32_t v : h)
        for (int i = 3; i >= 0; i--) out += (char)((v >> (i * 8)) & 0xFF);
    return out;
}

std::string toHex(const std::string &bytes) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) { out += hex[c >> 4]; out += hex[c & 15]; }
    return out;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const std::string &bytes) {
    std::string out;
    size_t i = 0;
    const unsigned char *p = (const unsigned char *)bytes.data();
    for (; i + 2 < bytes.size(); i += 3) {
        const uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
        out += kB64[v >> 18]; out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63]; out += kB64[v & 63];
    }
    if (i + 1 == bytes.size()) {
        const uint32_t v = (uint32_t)p[i] << 16;
        out += kB64[v >> 18]; out += kB64[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == bytes.size()) {
        const uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8;
        out += kB64[v >> 18]; out += kB64[(v >> 12) & 63]; out += kB64[(v >> 6) & 63]; out += '=';
    }
    return out;
}

bool fromBase64(const std::string &text, std::string &out) {
    out.clear();
    if (text.size() % 4 != 0) return false;
    uint32_t v = 0;
    int have = 0, pad = 0;
    for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        int d;
        if (c == '=') {
            if (i + 2 < text.size()) return false;   // only in the last two places
            pad++;
            d = 0;
        } else {
            if (pad) return false;
            const char *at = c ? std::strchr(kB64, c) : nullptr;
            if (!at) return false;
            d = (int)(at - kB64);
        }
        v = v << 6 | (uint32_t)d;
        if (++have == 4) {
            out += (char)((v >> 16) & 0xFF);
            if (pad < 2) out += (char)((v >> 8) & 0xFF);
            if (pad < 1) out += (char)(v & 0xFF);
            v = 0;
            have = 0;
        }
    }
    return true;
}

std::string acceptKey(const std::string &clientKey) {
    return base64(sha1(clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
}

bool sameSecret(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

// -------------------------------------------------------------- handshake
static std::string lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    return s.substr(a, b - a);
}

// The comma-separated tokens of a header value, lowercased.
static std::vector<std::string> tokens(const std::string &value) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t comma = value.find(',', start);
        std::string t = lower(trim(value.substr(start, comma == std::string::npos ? std::string::npos
                                                                                 : comma - start)));
        if (!t.empty()) out.push_back(t);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

static bool loopbackHost(const std::string &host) {
    std::string h = lower(trim(host));
    if (h.empty()) return false;
    if (h[0] == '[') {
        const size_t close = h.find(']');
        if (close == std::string::npos) return false;
        const std::string rest = h.substr(close + 1);
        if (!rest.empty() && (rest[0] != ':' || rest.find_first_not_of("0123456789", 1) != std::string::npos))
            return false;
        return h.substr(1, close - 1) == "::1";
    }
    const size_t colon = h.find(':');
    if (colon != std::string::npos) {
        const std::string port = h.substr(colon + 1);
        if (port.empty() || port.find_first_not_of("0123456789") != std::string::npos) return false;
        h = h.substr(0, colon);
    }
    return h == "127.0.0.1" || h == "localhost";
}

static Handshake refuse(int status, const char *reason, const std::string &why, size_t consumed) {
    Handshake h;
    h.state = Handshake::Refused;
    h.error = why;
    h.consumed = consumed;
    h.response = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                 "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    return h;
}

Handshake readHandshake(const std::string &buffer, const std::string &token) {
    const size_t end = buffer.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (buffer.size() > kMaxHandshakeBytes)
            return refuse(431, "Request Header Fields Too Large", "the request is too long",
                          buffer.size());
        return Handshake();
    }
    const size_t consumed = end + 4;
    if (end > kMaxHandshakeBytes)
        return refuse(431, "Request Header Fields Too Large", "the request is too long", consumed);
    const std::string head = buffer.substr(0, end);
    std::vector<std::string> lines;
    for (size_t at = 0;;) {
        size_t nl = head.find("\r\n", at);
        lines.push_back(head.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
        if (nl == std::string::npos) break;
        at = nl + 2;
    }
    const std::string &request = lines[0];
    if (request.compare(0, 4, "GET ") != 0 || request.size() < 14 ||
        request.compare(request.size() - 9, 9, " HTTP/1.1") != 0)
        return refuse(400, "Bad Request", "not a GET over HTTP/1.1", consumed);
    std::map<std::string, std::string> headers;
    for (size_t i = 1; i < lines.size(); i++) {
        const size_t colon = lines[i].find(':');
        if (colon == std::string::npos || colon == 0)
            return refuse(400, "Bad Request", "a malformed header line", consumed);
        const std::string name = lower(lines[i].substr(0, colon));
        if (name.find_first_of(" \t") != std::string::npos)
            return refuse(400, "Bad Request", "a malformed header line", consumed);
        const std::string value = trim(lines[i].substr(colon + 1));
        auto it = headers.find(name);
        if (it == headers.end()) headers[name] = value;
        else it->second += ", " + value;   // repeated headers join, as HTTP says
    }
    auto get = [&](const char *name) {
        auto it = headers.find(name);
        return it == headers.end() ? std::string() : it->second;
    };
    if (!loopbackHost(get("host")))
        return refuse(403, "Forbidden", "the Host is not this machine's loopback", consumed);
    // The token first: nothing else is said to a peer without it.
    auto auth = headers.find(lower(kAuthHeader));
    if (auth == headers.end() || token.empty() || !sameSecret(auth->second, token))
        return refuse(401, "Unauthorized", "no token, or the wrong one", consumed);
    bool upgrade = false, connection = false;
    for (const std::string &t : tokens(get("upgrade"))) upgrade = upgrade || t == "websocket";
    for (const std::string &t : tokens(get("connection"))) connection = connection || t == "upgrade";
    if (!upgrade || !connection)
        return refuse(426, "Upgrade Required", "not a WebSocket upgrade", consumed);
    if (trim(get("sec-websocket-version")) != "13")
        return refuse(426, "Upgrade Required", "not WebSocket version 13", consumed);
    const std::string key = trim(get("sec-websocket-key"));
    std::string raw;
    if (!fromBase64(key, raw) || raw.size() != 16)
        return refuse(400, "Bad Request", "a malformed Sec-WebSocket-Key", consumed);
    bool mcp = false;
    for (const std::string &t : tokens(get("sec-websocket-protocol"))) mcp = mcp || t == kSubprotocol;
    Handshake h;
    h.state = Handshake::Accepted;
    h.consumed = consumed;
    h.response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: " + acceptKey(key) + "\r\n";
    if (mcp) h.response += std::string("Sec-WebSocket-Protocol: ") + kSubprotocol + "\r\n";
    h.response += "\r\n";
    return h;
}

// ----------------------------------------------------------------- frames
bool validUtf8(const std::string &s) {
    const unsigned char *p = (const unsigned char *)s.data();
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = p[i];
        if (c < 0x80) { i++; continue; }
        size_t len;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = cp << 6 | (p[i + k] & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += len;
    }
    return true;
}

FrameReader::Result FrameReader::fail(uint16_t code, const std::string &why) {
    errorCode_ = code;
    error_ = why;
    return Error;
}

FrameReader::Result FrameReader::next(Message &out) {
    if (errorCode_) return Error;
    for (;;) {
        if (buf_.size() < 2) return NeedMore;
        const unsigned char *p = (const unsigned char *)buf_.data();
        const bool fin = p[0] & 0x80;
        if (p[0] & 0x70) return fail(1002, "reserved bits set (no extension was agreed)");
        const uint8_t op = p[0] & 0x0F;
        const bool masked = p[1] & 0x80;
        uint64_t len = p[1] & 0x7F;
        size_t at = 2;
        if (len == 126) {
            if (buf_.size() < 4) return NeedMore;
            len = (uint64_t)p[2] << 8 | p[3];
            if (len < 126) return fail(1002, "a length not in its shortest form");
            at = 4;
        } else if (len == 127) {
            if (buf_.size() < 10) return NeedMore;
            len = 0;
            for (int i = 0; i < 8; i++) len = len << 8 | p[2 + i];
            if (len >> 63) return fail(1002, "a length with its top bit set");
            if (len < 65536) return fail(1002, "a length not in its shortest form");
            at = 10;
        }
        if (!masked) return fail(1002, "a client frame that is not masked");
        const bool control = op & 0x8;
        if (control) {
            if (op != 8 && op != 9 && op != 10) return fail(1002, "an unknown control opcode");
            if (!fin) return fail(1002, "a fragmented control frame");
            if (len > 125) return fail(1002, "a control frame over 125 bytes");
        } else if (op != 0 && op != 1 && op != 2) {
            return fail(1002, "an unknown opcode");
        }
        if (!control && (inMessage_ ? partial_.size() : 0) + len > max_)
            return fail(1009, "a message over the size limit");
        if (buf_.size() < at + 4 + len) return NeedMore;
        unsigned char mask[4];
        std::memcpy(mask, p + at, 4);
        at += 4;
        std::string payload = buf_.substr(at, (size_t)len);
        for (size_t i = 0; i < payload.size(); i++) payload[i] = (char)(payload[i] ^ mask[i % 4]);
        buf_.erase(0, at + (size_t)len);

        if (control) {
            out = Message();
            out.op = (Opcode)op;
            if (op == 8) {
                if (payload.size() == 1) return fail(1002, "a close frame of one byte");
                if (payload.size() >= 2) {
                    out.closeCode = (uint16_t)((unsigned char)payload[0] << 8 | (unsigned char)payload[1]);
                    out.payload = payload.substr(2);
                    if (!validUtf8(out.payload)) return fail(1007, "a close reason that is not UTF-8");
                } else {
                    out.closeCode = 1005;
                }
            } else {
                out.payload = payload;
            }
            return Ready;
        }
        if (op == 0) {
            if (!inMessage_) return fail(1002, "a continuation with no message to continue");
            partial_ += payload;
        } else {
            if (inMessage_) return fail(1002, "a new message inside a fragmented one");
            inMessage_ = true;
            partialOp_ = (Opcode)op;
            partial_ = payload;
        }
        if (!fin) continue;
        inMessage_ = false;
        out = Message();
        out.op = partialOp_;
        out.payload.swap(partial_);
        partial_.clear();
        if (out.op == Opcode::Text && !validUtf8(out.payload))
            return fail(1007, "a text message that is not UTF-8");
        return Ready;
    }
}

static std::string frameHeader(uint8_t first, size_t len, bool masked) {
    std::string h;
    h += (char)first;
    const unsigned char m = masked ? 0x80 : 0;
    if (len < 126) {
        h += (char)(m | len);
    } else if (len < 65536) {
        h += (char)(m | 126);
        h += (char)((len >> 8) & 0xFF);
        h += (char)(len & 0xFF);
    } else {
        h += (char)(m | 127);
        for (int i = 7; i >= 0; i--) h += (char)(((uint64_t)len >> (i * 8)) & 0xFF);
    }
    return h;
}

std::string encodeFrame(Opcode op, const std::string &payload) {
    return frameHeader((uint8_t)(0x80 | (uint8_t)op), payload.size(), false) + payload;
}

std::string encodeClose(uint16_t code, const std::string &reason) {
    std::string p;
    p += (char)(code >> 8);
    p += (char)(code & 0xFF);
    p += reason.substr(0, 123);
    return encodeFrame(Opcode::Close, p);
}

std::string encodeClientFrame(Opcode op, const std::string &payload, uint32_t mask, bool fin) {
    std::string out = frameHeader((uint8_t)((fin ? 0x80 : 0) | (uint8_t)op), payload.size(), true);
    unsigned char m[4] = {(unsigned char)(mask >> 24), (unsigned char)(mask >> 16),
                          (unsigned char)(mask >> 8), (unsigned char)mask};
    out.append((const char *)m, 4);
    for (size_t i = 0; i < payload.size(); i++) out += (char)(payload[i] ^ m[i % 4]);
    return out;
}

// -------------------------------------------------------------------- MCP
Json ToolResult::toJson() const {
    Json content = Json::array();
    for (const std::string &t : texts)
        content.push(Json::object({{"type", "text"}, {"text", t}}));
    Json r = Json::object({{"content", content}});
    if (error) r.set("isError", true);
    return r;
}

Json schemaProperty(const char *type, const char *description) {
    return Json::object({{"type", type}, {"description", description}});
}

Json objectSchema(std::vector<std::pair<std::string, Json>> properties,
                  std::vector<std::string> required) {
    Json props = Json::object();
    for (auto &p : properties) props.set(p.first, std::move(p.second));
    Json s = Json::object({{"type", "object"}, {"properties", props}});
    if (!required.empty()) {
        Json req = Json::array();
        for (const std::string &r : required) req.push(r);
        s.set("required", req);
    }
    s.set("additionalProperties", false);
    return s;
}

McpServer::McpServer(std::string name, std::string version)
    : name_(std::move(name)), version_(std::move(version)) {}

void McpServer::addTool(Tool tool) {
    for (Tool &t : tools_)
        if (t.name == tool.name) { t = std::move(tool); return; }
    tools_.push_back(std::move(tool));
}

void McpServer::reply(const Json &id, Json result) {
    if (!send) return;
    Json m = Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
    send(m.dump());
}

void McpServer::replyError(const Json &id, int code, const std::string &message) {
    if (!send) return;
    Json m = Json::object({{"jsonrpc", "2.0"}, {"id", id},
                           {"error", Json::object({{"code", code}, {"message", message}})}});
    send(m.dump());
}

std::string McpServer::notification(const std::string &method, const Json &params) {
    return Json::object({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}}).dump();
}

void McpServer::handle(const std::string &text) {
    Json m;
    if (!Json::parse(text, m) || !m.isObject()) {
        replyError(Json(), -32700, "not JSON");
        return;
    }
    // A batch is not something Claude Code sends; it is refused whole.
    const Json &method = m["method"];
    const bool hasId = m.has("id") && (m["id"].isNumber() || m["id"].isString());
    const Json id = hasId ? m["id"] : Json();
    if (!method.isString()) {
        // A response to something we asked: we ask nothing, so it is dropped.
        if (m.has("result") || m.has("error")) return;
        replyError(id, -32600, "not a JSON-RPC request");
        return;
    }
    const std::string name = method.asString();
    const Json &params = m["params"];
    if (!hasId) {
        if (m.has("id")) {   // an id of a kind JSON-RPC does not allow
            replyError(Json(), -32600, "the id must be a number or a string");
            return;
        }
        if (onNotification) onNotification(name, params);
        return;
    }
    if (name == "initialize") {
        std::string asked = params["protocolVersion"].asString();
        static const char *known[] = {"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"};
        bool ok = false;
        for (const char *k : known) ok = ok || asked == k;
        initialized_ = true;
        reply(id, Json::object({
            {"protocolVersion", ok ? asked : std::string(kProtocolVersion)},
            {"capabilities", Json::object({{"tools", Json::object({{"listChanged", false}})}})},
            {"serverInfo", Json::object({{"name", name_}, {"version", version_}})}}));
        return;
    }
    if (name == "ping") {
        reply(id, Json::object());
        return;
    }
    if (!initialized_) {
        replyError(id, -32002, "initialize first");
        return;
    }
    if (name == "tools/list") {
        Json list = Json::array();
        for (const Tool &t : tools_)
            list.push(Json::object({{"name", t.name}, {"description", t.description},
                                    {"inputSchema", t.inputSchema}}));
        reply(id, Json::object({{"tools", list}}));
        return;
    }
    if (name == "tools/call") {
        const std::string toolName = params["name"].asString();
        for (const Tool &t : tools_) {
            if (t.name != toolName) continue;
            const Json args = params["arguments"].isObject() ? params["arguments"] : Json::object();
            // Answered once, even by a tool that calls back twice, and
            // not at all once the server (its connection) is gone.
            auto answered = std::make_shared<bool>(false);
            std::weak_ptr<bool> alive = alive_;
            t.run(args, [this, id, answered, alive](ToolResult r) {
                if (*answered || alive.expired()) return;
                *answered = true;
                reply(id, r.toJson());
            });
            return;
        }
        replyError(id, -32602, "unknown tool \"" + toolName + "\"");
        return;
    }
    if (name == "resources/list") { reply(id, Json::object({{"resources", Json::array()}})); return; }
    if (name == "prompts/list") { reply(id, Json::object({{"prompts", Json::array()}})); return; }
    replyError(id, -32601, "method not found: " + name);
}

// -------------------------------------------------------------- lock file
std::string lockFileName(int port) { return std::to_string(port) + ".lock"; }

int portFromLockName(const std::string &name) {
    const std::string suffix = ".lock";
    if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
        return 0;
    const std::string digits = name.substr(0, name.size() - suffix.size());
    if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos)
        return 0;
    const int port = std::stoi(digits);
    return port > 0 && port < 65536 ? port : 0;
}

std::string lockFileText(const LockInfo &info) {
    Json folders = Json::array();
    for (const std::string &f : info.workspaceFolders) folders.push(f);
    return Json::object({{"pid", info.pid},
                         {"workspaceFolders", folders},
                         {"ideName", info.ideName},
                         {"transport", info.transport.empty() ? std::string("ws") : info.transport},
                         {"runningInWindows", false},
                         {"authToken", info.authToken}}).dump();
}

bool parseLockFile(const std::string &text, LockInfo &out) {
    Json j;
    if (!Json::parse(text, j) || !j.isObject()) return false;
    out = LockInfo();
    out.pid = (int)j["pid"].asInt(0);
    for (const Json &f : j["workspaceFolders"].elements())
        if (f.isString()) out.workspaceFolders.push_back(f.asString());
    out.ideName = j["ideName"].asString();
    out.transport = j["transport"].asString();
    out.authToken = j["authToken"].asString();
    return true;
}

bool isStaleOwnLock(const LockInfo &info, int ourPid, bool pidAlive) {
    return info.ideName == kIdeName && info.pid > 0 && info.pid != ourPid && !pidAlive;
}

// ------------------------------------------------------------ tool shapes
std::string fileUri(const std::string &path) { return path.empty() ? "" : "file://" + path; }

std::string pathFromUri(const std::string &uri) {
    if (uri.compare(0, 7, "file://") != 0) return uri.empty() || uri[0] != '/' ? "" : uri;
    std::string rest = uri.substr(7);
    if (rest.compare(0, 9, "localhost") == 0) rest = rest.substr(9);
    if (rest.empty() || rest[0] != '/') return "";
    std::string out;
    for (size_t i = 0; i < rest.size(); i++) {
        if (rest[i] == '%' && i + 2 < rest.size() && std::isxdigit((unsigned char)rest[i + 1]) &&
            std::isxdigit((unsigned char)rest[i + 2])) {
            out += (char)std::stoi(rest.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else {
            out += rest[i];
        }
    }
    return out;
}

static Json position(int line, int character) {
    return Json::object({{"line", line}, {"character", character}});
}

Json selectionJson(const Selection &s) {
    const bool empty = s.startLine == s.endLine && s.startCharacter == s.endCharacter;
    return Json::object({
        {"text", s.text},
        {"filePath", s.filePath},
        {"fileUrl", fileUri(s.filePath)},
        {"selection", Json::object({{"start", position(s.startLine, s.startCharacter)},
                                    {"end", position(s.endLine, s.endCharacter)},
                                    {"isEmpty", empty}})}});
}

const char *severityName(int severity) {
    switch (severity) {
        case 2: return "Warning";
        case 3: return "Information";
        case 4: return "Hint";
        default: return "Error";
    }
}

Json diagnosticsJson(const std::vector<FileDiagnostics> &files) {
    Json out = Json::array();
    for (const FileDiagnostics &f : files) {
        Json list = Json::array();
        for (const Diagnostic &d : f.diagnostics) {
            Json o = Json::object({
                {"message", d.message},
                {"severity", severityName(d.severity)},
                {"range", Json::object({{"start", position(d.line, d.character)},
                                        {"end", position(d.endLine, d.endCharacter)}})}});
            if (!d.source.empty()) o.set("source", d.source);
            if (!d.code.empty()) o.set("code", d.code);
            list.push(o);
        }
        out.push(Json::object({{"uri", fileUri(f.path)}, {"diagnostics", list}}));
    }
    return out;
}

TextRange findTextRange(const std::u16string &text, const std::u16string &start,
                        const std::u16string &end, bool toLineEnd) {
    TextRange r;
    if (start.empty()) return r;
    const size_t at = text.find(start);
    if (at == std::u16string::npos) return r;
    r.found = true;
    r.start = at;
    if (end.empty()) {
        r.endFound = true;
        r.end = at + start.size();
    } else {
        const size_t e = text.find(end, at + start.size());
        if (e == std::u16string::npos) {
            r.end = at;
            return r;
        }
        r.endFound = true;
        r.end = e + end.size();
    }
    if (toLineEnd) {
        size_t nl = text.find(u'\n', r.end);
        r.end = nl == std::u16string::npos ? text.size() : nl;
        if (r.end > r.start && text[r.end - 1] == u'\r') r.end--;
    }
    return r;
}

void lineAndColumn(const std::u16string &text, size_t offset, int &line, int &column) {
    offset = std::min(offset, text.size());
    line = 0;
    size_t start = 0;
    for (size_t i = 0; i < offset; i++)
        if (text[i] == u'\n') { line++; start = i + 1; }
    column = (int)(offset - start);
}

ToolResult diffAccepted(const std::string &newText) {
    ToolResult r;
    r.texts = {"FILE_SAVED", newText};
    return r;
}

ToolResult diffRejected(const std::string &tabName) {
    ToolResult r;
    r.texts = {"DIFF_REJECTED", tabName};
    return r;
}

std::string resolvePath(const std::string &path, const std::string &root) {
    if (path.empty() || path[0] == '/' || root.empty()) return path;
    std::string p = path;
    while (p.compare(0, 2, "./") == 0) p = p.substr(2);
    return root.back() == '/' ? root + p : root + "/" + p;
}

}  // namespace IdeLink
