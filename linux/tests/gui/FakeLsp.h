// FakeLsp.h — the scripted language server for the GUI checks, as on the
// Mac (tests/gui/Harness.h's MCGuiFakeLsp): build/fakelsp is the Mac's own
// tests/fakelsp/fakelsp.cpp, built by meson.build beside minicode-guitest.
// It answers from a JSON script and logs every message the client sent, one
// JSON object a line, so a check can see what reached a server without a
// real one installed. The file's header in tests/fakelsp describes the
// script; "{}" gives the defaults (full sync, the usual capabilities).
//
//   const std::string cmd = MCGuiFakeLsp("big", "{}");
//   MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + cmd + "\n");
//   ... MCGuiFakeLspReceived("big", "textDocument/didOpen", "a.py") ...
#pragma once

#include "Harness.h"

#include "Json.h"

#include <cstdio>
#include <string>
#include <unistd.h>

inline std::string MCGuiFakeLspDir() { return MCGuiScratch() + "/fakelsp"; }

// The command line for lsp.<language>: the server with its script and log.
inline std::string MCGuiFakeLsp(const std::string& name, const std::string& scriptJson) {
    const std::string dir = MCGuiFakeLspDir();
    const std::string script = dir + "/" + name + ".json", log = dir + "/" + name + ".log";
    // Written beside and renamed, so the server never reads half a script.
    MCGuiWriteFile(script + ".tmp", scriptJson);
    rename((script + ".tmp").c_str(), script.c_str());
    const std::string exe = MCGuiBuildDir() + "/fakelsp";
    MCGuiCheck(access(exe.c_str(), X_OK) == 0, "build/fakelsp is built", __FILE__, __LINE__);
    return exe + " --script " + script + " --log " + log;
}

// How many messages the client sent with `method`, whose params' document
// URI ends in `uriEnd` (any, when empty).
inline int MCGuiFakeLspReceived(const std::string& name, const std::string& method,
                                const std::string& uriEnd = "") {
    const std::string text = MCGuiReadFile(MCGuiFakeLspDir() + "/" + name + ".log");
    int n = 0;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (line.empty()) continue;
        Json m;
        if (!Json::parse(line, m) || m["method"].asString() != method) continue;
        const std::string uri = m["params"]["textDocument"]["uri"].asString();
        if (!uriEnd.empty() &&
            (uri.size() < uriEnd.size() || uri.compare(uri.size() - uriEnd.size(), uriEnd.size(), uriEnd) != 0))
            continue;
        n++;
    }
    return n;
}

inline void MCGuiFakeLspClearLog(const std::string& name) {
    if (FILE* f = fopen((MCGuiFakeLspDir() + "/" + name + ".log").c_str(), "w")) fclose(f);
}
