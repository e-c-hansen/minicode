// FakeLspLog.h — reading what the scripted language server (FakeLsp.h) was
// sent, beyond counting: every message, and a document's text as the server
// has it after the client's didOpen, didChange (whole or ranged) and
// didClose, which is how the language server checks see that every tab's
// file is open on its server with its own text (PARITY.md item 2.0).
#pragma once

#include "FakeLsp.h"

#include "LspClient.h"

#include <string>
#include <vector>

// Every message the client sent the server `name`, in order (the log is
// appended to across restarts of a server with the same name).
inline std::vector<Json> MCGuiFakeLspMessages(const std::string& name) {
    std::vector<Json> out;
    const std::string text = MCGuiReadFile(MCGuiFakeLspDir() + "/" + name + ".log");
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        Json m;
        if (!line.empty() && Json::parse(line, m)) out.push_back(m);
    }
    return out;
}

inline bool MCGuiUriEndsWith(const std::string& uri, const std::string& name) {
    const std::string tail = "/" + name;
    return uri.size() >= tail.size() && uri.compare(uri.size() - tail.size(), tail.size(), tail) == 0;
}

// The text the server `name` has for the document whose URI ends in
// "/<file>", as the client synced it; false when it is not open there.
inline bool MCGuiFakeLspText(const std::string& name, const std::string& file, std::string* out) {
    bool open = false;
    std::u16string text;
    for (const Json& m : MCGuiFakeLspMessages(name)) {
        const std::string method = m["method"].asString();
        const Json& p = m["params"];
        if (!MCGuiUriEndsWith(p["textDocument"]["uri"].asString(), file)) continue;
        if (method == "textDocument/didOpen") {
            open = true;
            text = Lsp::toUtf16(p["textDocument"]["text"].asString());
        } else if (method == "textDocument/didChange") {
            for (const Json& c : p["contentChanges"].elements()) {
                const std::u16string with = Lsp::toUtf16(c["text"].asString());
                if (!c.has("range")) {
                    text = with;
                    continue;
                }
                const Lsp::Range r = Lsp::parseRange(c["range"]);
                size_t a = Lsp::offsetForPosition(text, r.start);
                size_t b = Lsp::offsetForPosition(text, r.end);
                if (b < a) std::swap(a, b);
                text.replace(a, b - a, with);
            }
        } else if (method == "textDocument/didClose") {
            open = false;
            text.clear();
        }
    }
    if (out) *out = Lsp::toUtf8(text);
    return open;
}

// How many didChange notifications for "/<file>" carried a range.
inline int MCGuiFakeLspRangedChanges(const std::string& name, const std::string& file) {
    int n = 0;
    for (const Json& m : MCGuiFakeLspMessages(name)) {
        if (m["method"].asString() != "textDocument/didChange") continue;
        if (!MCGuiUriEndsWith(m["params"]["textDocument"]["uri"].asString(), file)) continue;
        for (const Json& c : m["params"]["contentChanges"].elements())
            if (c.has("range")) {
                n++;
                break;
            }
    }
    return n;
}
