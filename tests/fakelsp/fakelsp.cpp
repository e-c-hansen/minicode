// fakelsp.cpp — a scripted language server for the GUI checks (`make
// fakelsp`, build/fakelsp). It speaks LSP over stdin and stdout with the
// app's own framing (Lsp::Framer) and JSON, keeps every document's text as
// the client syncs it (full or incremental), and answers from a script, so
// the language server checks in tests/gui run anywhere, with no real server
// installed.
//
//   fakelsp --script rules.json [--log received.jsonl] [any other words]
//
// Other words are ignored, so a test can name a command line the app
// recognises ("ruff server"). The log gets every message the client sent,
// one JSON object per line, as it arrives. The script is read again
// whenever its modification time changes, so a test can change what the
// server does while it runs. It is a JSON object:
//
//   {
//     "name": "fakelsp",                 serverInfo.name (default "fakelsp")
//     "capabilities": { ... },           initialize's capabilities (a default
//                                        set when absent: full sync, save,
//                                        completion, hover, definition,
//                                        references, rename, code actions,
//                                        document and workspace symbols)
//     "rules": [                         the first that matches answers
//       { "method": "textDocument/hover",
//         "when": { "uriEndsWith": "a.py",        all given must hold
//                   "textContains": "import os",  the document's text now
//                   "paramsContain": "\"line\":3" },  params as compact JSON
//         "times": 1,                    used at most this often, then skipped
//         "delayMs": 200,                sleep before answering (a slow server)
//         "result": ...,                 or "error": {"code":..., "message":...}
//         "notify": [ {"method": ..., "params": ...} ],   sent after the answer
//         "request": [ {"method": ..., "params": ...} ],  server-to-client requests
//         "exit": 3 }                    exit with this status afterwards
//     ]
//   }
//
// A request no rule answers gets a null result; a notification none
// matches is only logged. In "result", "params" and "error" every string
// (and key) is expanded: ${uri} is the request's document (params.textDocument.uri),
// ${uri:name} an open document whose URI ends in /name (or the file URI of
// an absolute path), ${version} and ${version:name} their versions as last
// synced, ${version-1} one less, and ${text} the request's document's
// text. A string that is exactly ${version...} becomes a number.
#include "Json.h"
#include "LspClient.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct Doc {
    std::u16string text;
    int64_t version = 0;
};

std::string gScriptPath, gLogPath;
Json gScript;
struct timespec gScriptTime = {0, 0};
std::map<size_t, int> gUsed;              // rule index -> times used
std::map<std::string, Doc> gDocs;         // by URI
int gNextId = 1000;
FILE *gLog = nullptr;

void writeAll(const std::string &s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::write(1, s.data() + off, s.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            _exit(0);   // the client went away
        }
        off += (size_t)n;
    }
}

void send(const Json &msg) { writeAll(Lsp::Framer::frame(msg.dump())); }

void loadScript() {
    if (gScriptPath.empty()) return;
    struct stat st;
    if (stat(gScriptPath.c_str(), &st) != 0) return;
#ifdef __APPLE__
    const struct timespec mtime = st.st_mtimespec;
#else
    const struct timespec mtime = st.st_mtim;
#endif
    if (mtime.tv_sec == gScriptTime.tv_sec && mtime.tv_nsec == gScriptTime.tv_nsec) return;
    FILE *f = fopen(gScriptPath.c_str(), "rb");
    if (!f) return;
    std::string text;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    Json parsed;
    std::string error;
    if (!Json::parse(text, parsed, &error)) {
        fprintf(stderr, "fakelsp: %s: %s\n", gScriptPath.c_str(), error.c_str());
        return;
    }
    gScript = parsed;
    gScriptTime = mtime;
    gUsed.clear();
}

Json defaultCapabilities() {
    Json sync = Json::object({{"openClose", true}, {"change", 1},
                              {"save", Json::object({{"includeText", false}})}});
    return Json::object({
        {"textDocumentSync", sync},
        {"completionProvider", Json::object({{"triggerCharacters", Json::array().push(".")}})},
        {"hoverProvider", true},
        {"definitionProvider", true},
        {"referencesProvider", true},
        {"renameProvider", true},
        {"codeActionProvider", true},
        {"documentSymbolProvider", true},
        {"workspaceSymbolProvider", true},
    });
}

std::string requestUri(const Json &params) { return params["textDocument"]["uri"].asString(); }

std::string uriFor(const std::string &name) {
    for (const auto &d : gDocs) {
        const std::string &u = d.first;
        if (u.size() > name.size() && u.compare(u.size() - name.size(), name.size(), name) == 0 &&
            u[u.size() - name.size() - 1] == '/')
            return u;
    }
    if (!name.empty() && name[0] == '/') return Lsp::uriFromPath(name);
    return name;
}

// One ${...} placeholder's value, as text; `number` says it is a version.
std::string expandOne(const std::string &key, const std::string &uri, bool &number) {
    number = false;
    if (key == "uri") return uri;
    if (key.compare(0, 4, "uri:") == 0) return uriFor(key.substr(4));
    if (key == "text") {
        auto it = gDocs.find(uri);
        return it == gDocs.end() ? "" : Lsp::toUtf8(it->second.text);
    }
    if (key.compare(0, 7, "version") == 0) {
        std::string rest = key.substr(7);
        int64_t delta = 0;
        if (rest.size() >= 2 && rest.compare(rest.size() - 2, 2, "-1") == 0) {
            delta = -1;
            rest.resize(rest.size() - 2);
        }
        std::string u = uri;
        if (!rest.empty() && rest[0] == ':') u = uriFor(rest.substr(1));
        auto it = gDocs.find(u);
        number = true;
        return std::to_string((it == gDocs.end() ? 0 : it->second.version) + delta);
    }
    return "${" + key + "}";
}

Json expand(const Json &j, const std::string &uri) {
    if (j.isString()) {
        const std::string &s = j.asString();
        if (s.find("${") == std::string::npos) return j;
        std::string out;
        size_t i = 0;
        bool onlyNumber = false;
        while (i < s.size()) {
            size_t open = s.find("${", i);
            size_t close = open == std::string::npos ? open : s.find('}', open);
            if (open == std::string::npos || close == std::string::npos) {
                out += s.substr(i);
                break;
            }
            out += s.substr(i, open - i);
            bool number = false;
            out += expandOne(s.substr(open + 2, close - open - 2), uri, number);
            onlyNumber = number && open == 0 && close == s.size() - 1;
            i = close + 1;
        }
        if (onlyNumber) return Json((int64_t)std::strtoll(out.c_str(), nullptr, 10));
        return Json(out);
    }
    if (j.isArray()) {
        Json a = Json::array();
        for (const Json &e : j.elements()) a.push(expand(e, uri));
        return a;
    }
    if (j.isObject()) {
        Json o = Json::object();
        // Keys too: a WorkspaceEdit's changes are keyed by URI.
        for (const auto &kv : j.items())
            o.set(expand(Json(kv.first), uri).asString(), expand(kv.second, uri));
        return o;
    }
    return j;
}

bool endsWith(const std::string &s, const std::string &tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// The first rule for this method whose conditions hold, or -1.
int findRule(const std::string &method, const Json &params) {
    const Json &rules = gScript["rules"];
    for (size_t i = 0; i < rules.size(); i++) {
        const Json &r = rules[i];
        if (r["method"].asString() != method) continue;
        if (r.has("times") && gUsed[i] >= r["times"].asInt()) continue;
        const Json &when = r["when"];
        std::string uri = requestUri(params);
        if (when.has("uriEndsWith") && !endsWith(uri, when["uriEndsWith"].asString())) continue;
        if (when.has("textContains")) {
            auto it = gDocs.find(uri);
            if (it == gDocs.end() ||
                Lsp::toUtf8(it->second.text).find(when["textContains"].asString()) ==
                    std::string::npos)
                continue;
        }
        if (when.has("paramsContain") &&
            params.dump().find(when["paramsContain"].asString()) == std::string::npos)
            continue;
        gUsed[i]++;
        return (int)i;
    }
    return -1;
}

void applyChange(Doc &doc, const Json &change) {
    std::u16string text = Lsp::toUtf16(change["text"].asString());
    if (!change.has("range")) {
        doc.text = text;
        return;
    }
    const Json &r = change["range"];
    Lsp::Position a{(int)r["start"]["line"].asInt(), (int)r["start"]["character"].asInt()};
    Lsp::Position b{(int)r["end"]["line"].asInt(), (int)r["end"]["character"].asInt()};
    size_t s = Lsp::offsetForPosition(doc.text, a), e = Lsp::offsetForPosition(doc.text, b);
    if (e < s) std::swap(s, e);
    doc.text.replace(s, e - s, text);
}

// Keeps the documents as the client syncs them.
void track(const std::string &method, const Json &params) {
    std::string uri = requestUri(params);
    if (method == "textDocument/didOpen") {
        Doc &d = gDocs[uri];
        d.text = Lsp::toUtf16(params["textDocument"]["text"].asString());
        d.version = params["textDocument"]["version"].asInt();
    } else if (method == "textDocument/didChange") {
        Doc &d = gDocs[uri];
        for (const Json &c : params["contentChanges"].elements()) applyChange(d, c);
        d.version = params["textDocument"]["version"].asInt(d.version);
    } else if (method == "textDocument/didClose") {
        gDocs.erase(uri);
    }
}

void runRule(int index, const Json &msg) {
    const Json &params = msg["params"];
    const bool isRequest = msg.has("id");
    std::string uri = requestUri(params);
    if (index < 0) {
        if (isRequest) send(Json::object({{"jsonrpc", "2.0"}, {"id", msg["id"]}, {"result", nullptr}}));
        return;
    }
    const Json &r = gScript["rules"][(size_t)index];
    if (r.has("delayMs")) usleep((useconds_t)(r["delayMs"].asInt() * 1000));
    if (isRequest) {
        Json reply = Json::object({{"jsonrpc", "2.0"}, {"id", msg["id"]}});
        if (r.has("error")) reply.set("error", expand(r["error"], uri));
        else reply.set("result", expand(r["result"], uri));
        send(reply);
    }
    for (const Json &n : r["notify"].elements())
        send(Json::object({{"jsonrpc", "2.0"}, {"method", n["method"]},
                           {"params", expand(n["params"], uri)}}));
    for (const Json &q : r["request"].elements())
        send(Json::object({{"jsonrpc", "2.0"}, {"id", gNextId++}, {"method", q["method"]},
                           {"params", expand(q["params"], uri)}}));
    if (r.has("exit")) {
        if (gLog) fflush(gLog);
        _exit((int)r["exit"].asInt());
    }
}

void handle(const std::string &body) {
    Json msg;
    if (!Json::parse(body, msg)) return;
    if (gLog) {
        fprintf(gLog, "%s\n", body.c_str());
        fflush(gLog);
    }
    loadScript();
    if (!msg.has("method")) return;   // a reply to one of our requests
    const std::string method = msg["method"].asString();
    const Json &params = msg["params"];
    track(method, params);
    if (method == "initialize") {
        int rule = findRule(method, params);
        if (rule >= 0) {
            runRule(rule, msg);
            return;
        }
        Json caps = gScript.has("capabilities") ? gScript["capabilities"] : defaultCapabilities();
        Json result = Json::object(
            {{"capabilities", caps},
             {"serverInfo", Json::object({{"name", gScript["name"].asString("fakelsp")},
                                          {"version", "1"}})}});
        send(Json::object({{"jsonrpc", "2.0"}, {"id", msg["id"]}, {"result", result}}));
        return;
    }
    if (method == "shutdown") {
        send(Json::object({{"jsonrpc", "2.0"}, {"id", msg["id"]}, {"result", nullptr}}));
        return;
    }
    if (method == "exit") {
        if (gLog) fflush(gLog);
        _exit(0);
    }
    if (method == "$/cancelRequest") return;
    runRule(findRule(method, params), msg);
}

}  // namespace

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--script" && i + 1 < argc) gScriptPath = argv[++i];
        else if (a == "--log" && i + 1 < argc) gLogPath = argv[++i];
    }
    if (!gLogPath.empty()) gLog = fopen(gLogPath.c_str(), "a");
    loadScript();
    Lsp::Framer framer;
    char buf[65536];
    while (true) {
        ssize_t n = ::read(0, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;   // the client closed our input: go
        framer.feed(buf, (size_t)n);
        std::string body;
        while (framer.next(body)) handle(body);
    }
    if (gLog) fclose(gLog);
    return 0;
}
