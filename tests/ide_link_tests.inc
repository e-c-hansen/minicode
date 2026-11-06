// ide_link_tests.inc — the `ide:*` groups (IdeLink.h, the Claude Code IDE
// link's protocol) and `mermaid:*` (MermaidDiagram.h), included by
// run_tests.cpp.
#include "IdeLink.h"
#include "MermaidDiagram.h"

static std::string ideRequest(const std::string &extra, const std::string &host = "127.0.0.1:45123") {
    return "GET / HTTP/1.1\r\nHost: " + host +
           "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" +
           extra + "\r\n";
}

static void testIdeLink() {
    using namespace IdeLink;

    GROUP("ide:sha1");
    {
        CHECK(toHex(sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
        CHECK(toHex(sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
        CHECK(toHex(sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
              "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
        CHECK(toHex(sha1(std::string(1000000, 'a'))) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
        // Lengths around the 55/56/64-byte padding boundaries.
        CHECK(toHex(sha1(std::string(55, 'x'))).size() == 40);
        CHECK(sha1(std::string(56, 'x')) != sha1(std::string(57, 'x')));
        CHECK(base64("") == "" && base64("f") == "Zg==" && base64("fo") == "Zm8=" &&
              base64("foo") == "Zm9v" && base64("foobar") == "Zm9vYmFy");
        std::string out;
        CHECK(fromBase64("Zm9vYmFy", out) && out == "foobar");
        CHECK(fromBase64("Zg==", out) && out == "f");
        CHECK(!fromBase64("Zg=", out) && !fromBase64("Z=g=", out) && !fromBase64("Zm9*", out));
        // RFC 6455's own example.
        CHECK(acceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
        CHECK(sameSecret("abc", "abc") && !sameSecret("abc", "abd") && !sameSecret("abc", "ab"));
    }

    GROUP("ide:handshake");
    {
        const std::string token = "0f8c2a5e-token";
        const std::string auth = std::string(kAuthHeader) + ": " + token + "\r\n";
        Handshake h = readHandshake(ideRequest(auth + "Sec-WebSocket-Protocol: mcp\r\n"), token);
        CHECK(h.state == Handshake::Accepted);
        CHECK(h.response.find("HTTP/1.1 101 ") == 0);
        CHECK(h.response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);
        CHECK(h.response.find("Sec-WebSocket-Protocol: mcp\r\n") != std::string::npos);
        CHECK(h.consumed == ideRequest(auth + "Sec-WebSocket-Protocol: mcp\r\n").size());
        // Bytes after the request (a frame sent at once) are not consumed.
        Handshake more = readHandshake(ideRequest(auth) + "XYZ", token);
        CHECK(more.state == Handshake::Accepted && more.consumed + 3 == ideRequest(auth).size() + 3);
        CHECK(more.response.find("Sec-WebSocket-Protocol") == std::string::npos);
        // Header names in any case; the header split over two reads.
        std::string lowerAuth = "x-claude-code-ide-authorization: " + token + "\r\n";
        CHECK(readHandshake(ideRequest(lowerAuth), token).state == Handshake::Accepted);
        const std::string whole = ideRequest(auth);
        CHECK(readHandshake(whole.substr(0, 40), token).state == Handshake::Incomplete);
        // No token, the wrong one, or a token of another length: 401, said nothing else.
        Handshake none = readHandshake(ideRequest(""), token);
        CHECK(none.state == Handshake::Refused && none.response.find("401") != std::string::npos);
        CHECK(readHandshake(ideRequest(std::string(kAuthHeader) + ": 0f8c2a5e-tokeN\r\n"), token).state ==
              Handshake::Refused);
        CHECK(readHandshake(ideRequest(std::string(kAuthHeader) + ": x\r\n"), token).state ==
              Handshake::Refused);
        CHECK(readHandshake(ideRequest(auth), "").state == Handshake::Refused);
        // A page that rebinds a name to 127.0.0.1 still sends its own name.
        Handshake evil = readHandshake(ideRequest(auth, "evil.example:45123"), token);
        CHECK(evil.state == Handshake::Refused && evil.response.find("403") != std::string::npos);
        CHECK(readHandshake(ideRequest(auth, "localhost:45123"), token).state == Handshake::Accepted);
        CHECK(readHandshake(ideRequest(auth, "[::1]:45123"), token).state == Handshake::Accepted);
        CHECK(readHandshake(ideRequest(auth, "127.0.0.1"), token).state == Handshake::Accepted);
        CHECK(readHandshake(ideRequest(auth, "127.0.0.1:x"), token).state == Handshake::Refused);
        CHECK(readHandshake(ideRequest(auth, "127.0.0.1.evil.example"), token).state == Handshake::Refused);
        // Not an upgrade, not version 13, a bad key, not GET.
        std::string noUpgrade = whole;
        noUpgrade.replace(noUpgrade.find("Upgrade: websocket"), 18, "Upgrade: h2c");
        CHECK(readHandshake(noUpgrade, token).state == Handshake::Refused);
        std::string v8 = whole;
        v8.replace(v8.find("Version: 13"), 11, "Version: 8");
        CHECK(readHandshake(v8, token).state == Handshake::Refused);
        std::string badKey = whole;
        badKey.replace(badKey.find("dGhlIHNhbXBsZSBub25jZQ=="), 24, "c2hvcnQ=");
        CHECK(readHandshake(badKey, token).state == Handshake::Refused);
        std::string post = whole;
        post.replace(0, 3, "PUT");
        CHECK(readHandshake(post, token).state == Handshake::Refused);
        // A request that never ends is refused once past the limit.
        CHECK(readHandshake("GET / HTTP/1.1\r\n" + std::string(kMaxHandshakeBytes + 10, 'a'), token).state ==
              Handshake::Refused);
        // Garbage never crashes.
        const char *junk[] = {"\r\n\r\n", "GET\r\n\r\n", ":\r\n\r\n", "GET / HTTP/1.1\r\n:x\r\n\r\n"};
        for (const char *j : junk) CHECK(readHandshake(j, token).state == Handshake::Refused);
    }

    GROUP("ide:frames");
    {
        FrameReader r;
        Message m;
        r.feed(encodeClientFrame(Opcode::Text, "hello", 0x37fa213d));
        CHECK(r.next(m) == FrameReader::Ready && m.op == Opcode::Text && m.payload == "hello");
        CHECK(r.next(m) == FrameReader::NeedMore);
        // RFC 6455's masked "Hello".
        const unsigned char rfc[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58};
        r.feed((const char *)rfc, sizeof rfc);
        CHECK(r.next(m) == FrameReader::Ready && m.payload == "Hello");
        // Byte by byte, every length form.
        for (size_t len : {(size_t)0, (size_t)125, (size_t)126, (size_t)65535, (size_t)65536, (size_t)200000}) {
            std::string payload(len, 'q');
            for (size_t i = 0; i < len; i += 97) payload[i] = (char)('a' + i % 26);
            const std::string f = encodeClientFrame(Opcode::Text, payload, 0x01020304);
            FrameReader one;
            bool got = false;
            for (size_t i = 0; i < f.size(); i++) {
                one.feed(f.data() + i, 1);
                Message x;
                FrameReader::Result res = one.next(x);
                if (res == FrameReader::Ready) { got = x.payload == payload && i + 1 == f.size(); break; }
                if (res == FrameReader::Error) break;
                if (len > 1000 && i < f.size() - 2) {   // skip ahead: feeding all of a big one byte by byte is slow
                    one.feed(f.data() + i + 1, f.size() - i - 2);
                    i = f.size() - 2;
                }
            }
            CHECK(got);
        }
        // Fragments, with a ping between them.
        FrameReader fr;
        fr.feed(encodeClientFrame(Opcode::Text, "{\"a\":", 7, false));
        fr.feed(encodeClientFrame(Opcode::Ping, "p", 9));
        fr.feed(encodeClientFrame(Opcode::Continuation, "1}", 11, true));
        CHECK(fr.next(m) == FrameReader::Ready && m.op == Opcode::Ping && m.payload == "p");
        CHECK(fr.next(m) == FrameReader::Ready && m.op == Opcode::Text && m.payload == "{\"a\":1}");
        // Close with a code and reason; and with none.
        FrameReader cr;
        cr.feed(encodeClientFrame(Opcode::Close, std::string("\x03\xe8", 2) + "bye", 5));
        CHECK(cr.next(m) == FrameReader::Ready && m.op == Opcode::Close && m.closeCode == 1000 &&
              m.payload == "bye");
        FrameReader cr2;
        cr2.feed(encodeClientFrame(Opcode::Close, "", 5));
        CHECK(cr2.next(m) == FrameReader::Ready && m.closeCode == 1005);
        // Broken streams: each an error with its close code.
        auto errorOf = [](const std::string &bytes) {
            FrameReader e;
            e.feed(bytes);
            Message x;
            while (true) {
                FrameReader::Result res = e.next(x);
                if (res == FrameReader::Error) return (int)e.errorCode();
                if (res == FrameReader::NeedMore) return 0;
            }
        };
        CHECK(errorOf(encodeFrame(Opcode::Text, "unmasked")) == 1002);
        CHECK(errorOf(encodeClientFrame(Opcode::Continuation, "x", 1)) == 1002);
        CHECK(errorOf(encodeClientFrame(Opcode::Ping, "x", 1, false)) == 1002);
        CHECK(errorOf(encodeClientFrame(Opcode::Ping, std::string(126, 'x'), 1)) == 1002);
        CHECK(errorOf(encodeClientFrame((Opcode)3, "x", 1)) == 1002);
        CHECK(errorOf(encodeClientFrame((Opcode)11, "x", 1)) == 1002);
        CHECK(errorOf(encodeClientFrame(Opcode::Text, "\xff\xfe", 1)) == 1007);
        CHECK(errorOf(encodeClientFrame(Opcode::Text, "a", 1, false) +
                      encodeClientFrame(Opcode::Text, "b", 1)) == 1002);
        std::string rsv = encodeClientFrame(Opcode::Text, "x", 1);
        rsv[0] = (char)(rsv[0] | 0x40);
        CHECK(errorOf(rsv) == 1002);
        CHECK(errorOf(encodeClientFrame(Opcode::Close, "\x03", 1)) == 1002);
        FrameReader small(10);
        small.feed(encodeClientFrame(Opcode::Text, std::string(11, 'x'), 1));
        CHECK(small.next(m) == FrameReader::Error && small.errorCode() == 1009);
        FrameReader small2(10);
        small2.feed(encodeClientFrame(Opcode::Text, std::string(6, 'x'), 1, false) +
                    encodeClientFrame(Opcode::Continuation, std::string(6, 'x'), 1));
        CHECK(small2.next(m) == FrameReader::Error && small2.errorCode() == 1009);
        // A huge declared length is refused before it is waited for.
        std::string huge = encodeClientFrame(Opcode::Text, "", 1);
        huge = std::string("\x81\xff\x00\x00\x01\x00\x00\x00\x00\x00", 10) + std::string("\0\0\0\0", 4);
        CHECK(errorOf(huge) == 1009);
        // Server frames: unmasked, in each length form, and a close.
        CHECK(encodeFrame(Opcode::Text, "hi") == std::string("\x81\x02hi", 4));
        CHECK(encodeFrame(Opcode::Text, std::string(126, 'x')).substr(0, 4) == std::string("\x81\x7e\x00\x7e", 4));
        CHECK(encodeFrame(Opcode::Text, std::string(70000, 'x')).size() == 70000 + 10);
        CHECK(encodeClose(1000, "ok") == std::string("\x88\x04\x03\xe8ok", 6));
        CHECK(validUtf8("é ✓ 😀") && !validUtf8("\xc0\xaf") && !validUtf8("\xed\xa0\x80") &&
              !validUtf8("\xe2\x82"));
        // Random bytes never crash.
        uint32_t seed = 12345;
        for (int round = 0; round < 2000; round++) {
            std::string junk;
            const int n = (int)(seed % 40);
            for (int i = 0; i < n; i++) {
                seed = seed * 1103515245 + 12345;
                junk += (char)(seed >> 16);
            }
            FrameReader j(1000);
            j.feed(junk);
            Message x;
            for (int k = 0; k < 10 && j.next(x) == FrameReader::Ready; k++) {}
        }
    }

    GROUP("ide:mcp");
    {
        McpServer server("minicode", "1.5.7");
        std::vector<std::string> sent;
        std::vector<std::string> notes;
        server.send = [&](const std::string &t) { sent.push_back(t); };
        server.onNotification = [&](const std::string &m, const Json &) { notes.push_back(m); };
        std::function<void(ToolResult)> later;
        Tool echo;
        echo.name = "echo";
        echo.description = "says it back";
        echo.inputSchema = objectSchema({{"text", schemaProperty("string", "what to say")}}, {"text"});
        echo.run = [](const Json &args, std::function<void(ToolResult)> respond) {
            respond(ToolResult::text(args["text"].asString()));
            respond(ToolResult::text("twice"));   // only the first counts
        };
        server.addTool(echo);
        Tool wait;
        wait.name = "wait";
        wait.inputSchema = objectSchema({});
        wait.run = [&](const Json &, std::function<void(ToolResult)> respond) { later = respond; };
        server.addTool(wait);
        auto last = [&]() {
            Json j;
            CHECK(!sent.empty() && Json::parse(sent.back(), j));
            return j;
        };
        // Before initialize, only ping works.
        server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
        CHECK(last()["error"]["code"].asInt() == -32002);
        server.handle(R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"claude-code","version":"2.1.319"}}})");
        Json init = last();
        CHECK(init["id"].asInt() == 2 && init["result"]["protocolVersion"].asString() == "2025-06-18");
        CHECK(init["result"]["serverInfo"]["name"].asString() == "minicode");
        CHECK(init["result"]["capabilities"]["tools"].isObject());
        server.handle(R"({"jsonrpc":"2.0","id":3,"method":"initialize","params":{"protocolVersion":"1999-01-01"}})");
        CHECK(last()["result"]["protocolVersion"].asString() == kProtocolVersion);
        // Notifications get no answer and reach onNotification.
        const size_t before = sent.size();
        server.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        server.handle(R"({"jsonrpc":"2.0","method":"ide_connected","params":{"pid":42}})");
        CHECK(sent.size() == before && notes.size() == 2 && notes[1] == "ide_connected");
        server.handle(R"({"jsonrpc":"2.0","id":"s","method":"ping"})");
        CHECK(last()["id"].asString() == "s" && last()["result"].isObject());
        server.handle(R"({"jsonrpc":"2.0","id":4,"method":"tools/list"})");
        Json list = last()["result"]["tools"];
        CHECK(list.size() == 2 && list[0]["name"].asString() == "echo" &&
              list[0]["inputSchema"]["required"][0].asString() == "text");
        server.handle(R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hi"}}})");
        Json call = last();
        CHECK(call["id"].asInt() == 5 && call["result"]["content"][0]["text"].asString() == "hi" &&
              !call["result"].has("isError"));
        CHECK(sent.size() == before + 3);   // ping, list, one answer (not two)
        // A tool that answers later; a second request meanwhile is answered first.
        server.handle(R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"wait","arguments":{}}})");
        const size_t pending = sent.size();
        server.handle(R"({"jsonrpc":"2.0","id":7,"method":"ping"})");
        CHECK(sent.size() == pending + 1 && later);
        later(diffAccepted("new text"));
        Json done = last();
        CHECK(done["id"].asInt() == 6 && done["result"]["content"][0]["text"].asString() == "FILE_SAVED" &&
              done["result"]["content"][1]["text"].asString() == "new text");
        // Errors: an unknown tool or method, garbage, a response, a bad id.
        server.handle(R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"rm"}})");
        CHECK(last()["error"]["code"].asInt() == -32602);
        server.handle(R"({"jsonrpc":"2.0","id":9,"method":"sampling/createMessage"})");
        CHECK(last()["error"]["code"].asInt() == -32601);
        server.handle("{");
        CHECK(last()["error"]["code"].asInt() == -32700);
        const size_t quiet = sent.size();
        server.handle(R"({"jsonrpc":"2.0","id":1,"result":{}})");
        CHECK(sent.size() == quiet);
        server.handle(R"({"jsonrpc":"2.0","id":{},"method":"ping"})");
        CHECK(last()["error"]["code"].asInt() == -32600);
        CHECK(ToolResult::failure("no").toJson()["isError"].asBool());
        Json note;
        CHECK(Json::parse(McpServer::notification("selection_changed", Json::object({{"a", 1}})), note) &&
              note["method"].asString() == "selection_changed" && !note.has("id"));
        // An answer that comes after its server is gone goes nowhere.
        std::function<void(ToolResult)> orphan;
        {
            McpServer gone("minicode", "1");
            gone.send = [&](const std::string &t) { sent.push_back(t); };
            gone.addTool(wait);
            gone.handle(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
            gone.handle(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"wait"}})");
            orphan = later;
        }
        const size_t afterGone = sent.size();
        orphan(ToolResult::text("late"));
        CHECK(sent.size() == afterGone);
    }

    GROUP("ide:lock");
    {
        LockInfo info;
        info.pid = 4242;
        info.workspaceFolders = {"/Users/me/Code/x", "/tmp/a \"b\""};
        info.ideName = kIdeName;
        info.authToken = "tok";
        const std::string text = lockFileText(info);
        Json j;
        CHECK(Json::parse(text, j));
        CHECK(j["pid"].asInt() == 4242 && j["transport"].asString() == "ws" &&
              j["ideName"].asString() == "minicode" && j["authToken"].asString() == "tok" &&
              j["runningInWindows"].isBool() && !j["runningInWindows"].asBool());
        LockInfo back;
        CHECK(parseLockFile(text, back) && back.workspaceFolders.size() == 2 &&
              back.workspaceFolders[1] == "/tmp/a \"b\"" && back.pid == 4242);
        CHECK(!parseLockFile("not json", back) && !parseLockFile("[]", back));
        CHECK(lockFileName(51234) == "51234.lock");
        CHECK(portFromLockName("51234.lock") == 51234 && portFromLockName("x.lock") == 0 &&
              portFromLockName(".lock") == 0 && portFromLockName("70000.lock") == 0 &&
              portFromLockName("12.lock.tmp") == 0 && portFromLockName("0.lock") == 0);
        // Only minicode's own, of a dead process that is not this one.
        CHECK(isStaleOwnLock(info, 1, false));
        CHECK(!isStaleOwnLock(info, 1, true));
        CHECK(!isStaleOwnLock(info, 4242, false));
        LockInfo vscode = info;
        vscode.ideName = "Visual Studio Code";
        CHECK(!isStaleOwnLock(vscode, 1, false));
        LockInfo noPid = info;
        noPid.pid = 0;
        CHECK(!isStaleOwnLock(noPid, 1, false));
    }

    GROUP("ide:shapes");
    {
        CHECK(fileUri("/a/b c.py") == "file:///a/b c.py" && fileUri("").empty());
        CHECK(pathFromUri("file:///a/b c.py") == "/a/b c.py");
        CHECK(pathFromUri("file:///a/b%20c.py") == "/a/b c.py");
        CHECK(pathFromUri("file://localhost/a") == "/a");
        CHECK(pathFromUri("/plain/path") == "/plain/path");
        CHECK(pathFromUri("_claude_fs_right:/x").empty() && pathFromUri("file://host/x").empty());
        Selection s;
        s.filePath = "/p/a.py";
        s.text = "foo";
        s.startLine = 2; s.startCharacter = 4; s.endLine = 2; s.endCharacter = 7;
        Json sj = selectionJson(s);
        CHECK(sj["text"].asString() == "foo" && sj["filePath"].asString() == "/p/a.py" &&
              sj["fileUrl"].asString() == "file:///p/a.py" &&
              sj["selection"]["start"]["line"].asInt() == 2 &&
              sj["selection"]["end"]["character"].asInt() == 7 && !sj["selection"]["isEmpty"].asBool());
        s.endCharacter = 4;
        s.text.clear();
        CHECK(selectionJson(s)["selection"]["isEmpty"].asBool());
        FileDiagnostics fd;
        fd.path = "/p/a.py";
        Diagnostic d;
        d.line = 3; d.character = 1; d.endLine = 3; d.endCharacter = 5;
        d.severity = 2; d.message = "unused"; d.source = "ruff"; d.code = "F401";
        fd.diagnostics.push_back(d);
        Diagnostic e;
        e.message = "bad";
        fd.diagnostics.push_back(e);
        Json dj = diagnosticsJson({fd});
        CHECK(dj.size() == 1 && dj[0]["uri"].asString() == "file:///p/a.py");
        CHECK(dj[0]["diagnostics"][0]["severity"].asString() == "Warning" &&
              dj[0]["diagnostics"][0]["range"]["start"]["line"].asInt() == 3 &&
              dj[0]["diagnostics"][0]["code"].asString() == "F401" &&
              dj[0]["diagnostics"][1]["severity"].asString() == "Error" &&
              !dj[0]["diagnostics"][1].has("source"));
        CHECK(std::string(severityName(3)) == "Information" && std::string(severityName(4)) == "Hint");
        CHECK(diagnosticsJson({}).size() == 0 && diagnosticsJson({}).isArray());
        // openFile's startText / endText.
        const std::u16string text = u"def f():\n    return 1\n\ndef g():\r\n    pass\n";
        TextRange r = findTextRange(text, u"return", u"", false);
        CHECK(r.found && r.endFound && r.start == 13 && r.end == 19);
        r = findTextRange(text, u"def f", u"1", false);
        CHECK(r.found && r.endFound && r.start == 0 && r.end == 21);
        r = findTextRange(text, u"def g", u"()", true);
        CHECK(r.found && r.endFound && text.substr(r.start, r.end - r.start) == u"def g():");
        r = findTextRange(text, u"return", u"nowhere", false);
        CHECK(r.found && !r.endFound && r.start == r.end && r.start == 13);
        CHECK(!findTextRange(text, u"absent", u"", false).found);
        CHECK(!findTextRange(text, u"", u"", false).found);
        // The end is looked for after the start, not before it.
        r = findTextRange(u"b a b", u"a", u"b", false);
        CHECK(r.found && r.start == 2 && r.end == 5);
        int line = -1, col = -1;
        lineAndColumn(text, 13, line, col);
        CHECK(line == 1 && col == 4);
        lineAndColumn(text, 0, line, col);
        CHECK(line == 0 && col == 0);
        lineAndColumn(text, 9999, line, col);
        CHECK(line == 5 && col == 0);
        CHECK(diffRejected("✻ [Claude Code] a.py (abc123) ⧉").texts[0] == "DIFF_REJECTED");
        CHECK(diffAccepted("x").texts.size() == 2);
        CHECK(resolvePath("src/a.py", "/root") == "/root/src/a.py" &&
              resolvePath("./a.py", "/root/") == "/root/a.py" &&
              resolvePath("/abs", "/root") == "/abs" && resolvePath("a", "") == "a");
    }
}

static void testIdeLinkSettings() {
    GROUP("settings:ide-link");
    {
        CHECK(!parseSettings("").ideLink());
        CHECK(parseSettings("agent.ide-link = on\n").ideLink());
        CHECK(parseSettings("agent.ide-link = true # yes\n").ideLink());
        CHECK(!parseSettings("agent.ide-link = off\n").ideLink());
        std::vector<SettingsError> e;
        Settings bad = parseSettings("agent.ide-link = sometimes\n", &e);
        CHECK(!bad.ideLink() && e.size() == 1);
        CHECK(parseSettings("").mermaid());
        CHECK(!parseSettings("markdown.mermaid = false\n").mermaid());
        CHECK(std::string(Settings::defaultFileText()).find("# agent.ide-link = off\n") != std::string::npos);
        CHECK(std::string(Settings::defaultFileText()).find("# markdown.mermaid = true\n") != std::string::npos);
    }
    GROUP("settings:with-value");
    {
        // The active line changed, keeping its comment; else the commented
        // one uncommented; else a line at the end. CRLF kept.
        CHECK(Settings::withValue("a = 1\nagent.ide-link = off  # mine\n", "agent.ide-link", "on") ==
              "a = 1\nagent.ide-link = on  # mine\n");
        CHECK(Settings::withValue("# x\n# agent.ide-link = off\nb = 2\n", "agent.ide-link", "on") ==
              "# x\nagent.ide-link = on\nb = 2\n");
        CHECK(Settings::withValue("b = 2", "agent.ide-link", "on") == "b = 2\nagent.ide-link = on\n");
        CHECK(Settings::withValue("", "agent.ide-link", "on") == "agent.ide-link = on\n");
        CHECK(Settings::withValue("  agent.ide-link = on\r\nc = 3\r\n", "agent.ide-link", "off") ==
              "  agent.ide-link = off\r\nc = 3\r\n");
        CHECK(parseSettings(Settings::withValue(Settings::defaultFileText(), "agent.ide-link", "on")).ideLink());
    }
}

static void testMermaidDiagram() {
    GROUP("mermaid:blocks");
    {
        const std::string md =
            "# Title\n\n```mermaid\nflowchart LR\n  A --> B\n```\n\ntext\n\n"
            "```mermaid\nsequenceDiagram\n  A->>B: hi\n```\n```Mermaid\ngraph TD\n```\n"
            "```python\nx = 1\n```\n\n```mermaid\n```\n";
        std::vector<MdRun> runs = MarkdownParser::parse(md);
        std::vector<MermaidDiagram::Block> b = MermaidDiagram::blocks(runs);
        CHECK(b.size() == 3);   // an empty block has no runs
        if (b.size() == 3) {
            CHECK(b[0].source == "flowchart LR\n  A --> B\n" && b[0].line == 3);
            CHECK(b[1].source == "sequenceDiagram\n  A->>B: hi\n" && b[1].line == 10);
            // The info string's first word, lowercased, as the parser gives it.
            CHECK(b[2].source == "graph TD\n");
            for (const auto &blk : b) {
                CHECK(blk.end > blk.first && blk.end <= runs.size());
                for (size_t i = blk.first; i < blk.end; i++) CHECK(MermaidDiagram::isMermaidRun(runs[i]));
                CHECK(blk.end == runs.size() || !MermaidDiagram::isMermaidRun(runs[blk.end]));
            }
        }
        CHECK(MermaidDiagram::blocks(MarkdownParser::parse("no diagrams\n")).empty());
    }

    GROUP("mermaid:command");
    {
        const std::string k1 = MermaidDiagram::key("graph TD\n", "dark");
        CHECK(k1.size() == 40 && k1 == MermaidDiagram::key("graph TD\n", "dark"));
        CHECK(k1 != MermaidDiagram::key("graph TD\n", "default"));
        CHECK(k1 != MermaidDiagram::key("graph LR\n", "dark"));
        CHECK(MermaidDiagram::theme(true) == "dark" && MermaidDiagram::theme(false) == "default");
        std::vector<std::string> a = MermaidDiagram::arguments("/t/in.mmd", "/t/out.png", "dark");
        const std::vector<std::string> want = {"-i", "/t/in.mmd", "-o", "/t/out.png", "-t", "dark",
                                               "-b", "transparent", "-s", "2", "-q"};
        CHECK(a == want);
    }

    GROUP("mermaid:errors");
    {
        // mermaid-cli 12.2's complaint about a broken flowchart, as printed.
        const std::string err =
            "\nError: Parse error on line 3:\n...] -->   B -->|Yes C[OK]\n----------------------^\n"
            "Expecting 'SQE', 'PIPE', got 'SQS'\n"
            "Parser.parseError (https://mermaid-cli-intercept.invalid/x/chunk.mjs:1636:21)\n"
            "    at #evaluate (file:///x/ExecutionContext.js:402:19)\n";
        CHECK(MermaidDiagram::errorSummary(err, "fallback") ==
              "Parse error on line 3:\n...] -->   B -->|Yes C[OK]\n----------------------^\n"
              "Expecting 'SQE', 'PIPE', got 'SQS'");
        CHECK(MermaidDiagram::errorSummary("", "mmdc failed") == "mmdc failed");
        CHECK(MermaidDiagram::errorSummary("env: node: No such file or directory\n", "x") ==
              "env: node: No such file or directory");
        CHECK(MermaidDiagram::errorSummary("    at a (file:///x:1:1)\n", "x") == "x");
        std::string many = "Error: one\n";
        for (int i = 0; i < 20; i++) many += "line\n";
        const std::string s = MermaidDiagram::errorSummary(many, "x");
        CHECK(std::count(s.begin(), s.end(), '\n') == 5);
    }
}
