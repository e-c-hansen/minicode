// IdeLinkSuite.cpp — `./build/minicode-guitest idelink`: the Claude Code IDE
// link (src/IdeLinkServer.h, linux/PARITY.md item 3.9; the Mac's
// tests/gui/IdeLinkSuite.mm) driven as Claude Code drives it. A WebSocket
// client (non-blocking, read by the waits so the main loop keeps turning,
// built on the core's frame code) dials the window's port from its lock file
// under the scratch CLAUDE_CONFIG_DIR, never ~/.claude. What is checked:
//   - the lock file: mode 0600, the token, the folder, the pid; a stale one
//     of minicode's own (a dead pid) removed at start, another IDE's left;
//   - a wrong token, no token and a Host off the loopback refused, and
//     nothing said before the token;
//   - ide_connected, initialize, tools/list (saveDocument and executeCode
//     not offered), selection_changed on a real selection in UTF-16 units;
//   - openFile with startText and endText: the agent's preview tab and a
//     highlight with an empty note;
//   - openDiff accepted with the bar's real Accept click (FILE_SAVED and the
//     text) and rejected by closing the tab (DIFF_REJECTED); close_tab and
//     closeAllDiffTabs touching only diffs the link offered;
//   - getDiagnostics from the scripted language server, under the uri
//     spelling it was asked with, and for a file with none;
//   - the View menu's action writes agent.ide-link and the link follows it,
//     removing the lock file and dropping the client when off;
//   - terminals get CLAUDE_CODE_SSE_PORT while it is on.
#include "Harness.h"

#include "CompareView.h"
#include "Document.h"
#include "Editor.h"
#include "FileTree.h"
#include "FakeLsp.h"
#include "IdeLink.h"
#include "IdeLinkServer.h"
#include "Json.h"
#include "AgentNotes.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <climits>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
IdeLinkServer* ide() { return MCGuiApp().ide; }
CompareView* cv() { return ed()->compareView(); }
std::string F(const std::string& name) { return MCGuiProjectFile("ide/" + name); }
std::string claudeDir() { return MCGuiScratch() + "/claude"; }
std::string lockDir() { return claudeDir() + "/ide"; }

std::string real(const std::string& p) {
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}

// ------------------------------------------------------- a test client
struct WS {
    int fd = -1;
    std::string in, head;
    bool upgraded = false, closed = false, gotClose = false;
    std::vector<std::string> texts;
    uint32_t mask = 7;

    bool connectTo(int port, const char* token, const std::string& host = "127.0.0.1") {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (struct sockaddr*)&a, sizeof a) != 0) return false;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        std::string req = "GET / HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
                          "Sec-WebSocket-Protocol: mcp\r\nUser-Agent: claude-code/test\r\n";
        if (token) req += std::string("X-Claude-Code-Ide-Authorization: ") + token + "\r\n";
        req += "\r\n";
        return sendRaw(req);
    }
    bool sendRaw(const std::string& bytes) {
        size_t done = 0;
        while (done < bytes.size()) {
            ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
            if (n < 0 && errno == EAGAIN) continue;
            if (n <= 0) return false;
            done += (size_t)n;
        }
        return true;
    }
    void sendText(const std::string& text) {
        sendRaw(IdeLink::encodeClientFrame(IdeLink::Opcode::Text, text, ++mask * 2654435761u));
    }
    // Reads what has arrived; call from a wait.
    void pump() {
        if (fd < 0) return;
        char buf[65536];
        for (;;) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n > 0) { in.append(buf, (size_t)n); continue; }
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { closed = true; }
            break;
        }
        if (!upgraded) {
            const size_t end = in.find("\r\n\r\n");
            if (end == std::string::npos) return;
            head = in.substr(0, end + 4);
            in.erase(0, end + 4);
            upgraded = head.rfind("HTTP/1.1 101 ", 0) == 0;
            if (!upgraded) return;
        }
        for (;;) {   // unmasked server frames
            if (in.size() < 2) return;
            const unsigned char* p = (const unsigned char*)in.data();
            const int op = p[0] & 0x0F;
            uint64_t len = p[1] & 0x7F;
            size_t at = 2;
            if (len == 126) { if (in.size() < 4) return; len = (uint64_t)p[2] << 8 | p[3]; at = 4; }
            else if (len == 127) {
                if (in.size() < 10) return;
                len = 0;
                for (int i = 0; i < 8; i++) len = len << 8 | p[2 + i];
                at = 10;
            }
            if (in.size() < at + len) return;
            const std::string payload = in.substr(at, (size_t)len);
            in.erase(0, at + (size_t)len);
            if (op == 1) texts.push_back(payload);
            else if (op == 8) gotClose = true;
        }
    }
    // The JSON-RPC message with this id, or null.
    Json answer(int id) {
        pump();
        for (const std::string& t : texts) {
            Json j;
            if (Json::parse(t, j) && j.has("id") && j["id"].asInt(-1) == id) return j;
        }
        return Json();
    }
    // The last notification with this method, or null.
    Json notification(const std::string& method) {
        pump();
        Json found;
        for (const std::string& t : texts) {
            Json j;
            if (Json::parse(t, j) && !j.has("id") && j["method"].asString() == method) found = j;
        }
        return found;
    }
    void call(int id, const std::string& method, const Json& params) {
        Json m = Json::object();
        m.set("jsonrpc", "2.0");
        m.set("id", id);
        m.set("method", method);
        m.set("params", params);
        sendText(m.dump());
    }
    void tool(int id, const std::string& name, const Json& args) {
        Json p = Json::object();
        p.set("name", name);
        p.set("arguments", args);
        call(id, "tools/call", p);
    }
    void notify(const std::string& method, const Json& params) {
        Json m = Json::object();
        m.set("jsonrpc", "2.0");
        m.set("method", method);
        m.set("params", params);
        sendText(m.dump());
    }
    void closeFd() { if (fd >= 0) close(fd); fd = -1; }
};

WS gA;
int gNext = 100;

// The text of a tool's answer, "" when there is none.
std::string toolText(WS& c, int id, size_t block = 0) {
    Json a = c.answer(id);
    if (a.isNull()) return "";
    return a["result"]["content"][block]["text"].asString();
}
bool toolError(WS& c, int id) { return c.answer(id)["result"]["isError"].asBool(false); }

bool answered(int id) { return !gA.answer(id).isNull(); }

Json args(std::initializer_list<std::pair<std::string, Json>> kv) { return Json::object(kv); }

std::string readLock() { return MCGuiReadFile(ide()->lockPath()); }

int gPort = 0;
std::string gToken;

const std::string kA = "import os\nx = undefined_name\ns = \"\xF0\x9D\x84\x9Ex\"\nprint('hello world')\n";
const std::string kB = "alpha\nbeta gamma delta\nepsilon\n";

std::string script(const std::string& aPath) {
    (void)aPath;
    return "{\"name\": \"fakelsp\", \"rules\": ["
           "{\"method\": \"textDocument/didOpen\", \"when\": {\"uriEndsWith\": \"a.py\"},"
           " \"notify\": [{\"method\": \"textDocument/publishDiagnostics\", \"params\": {"
           "\"uri\": \"${uri}\", \"version\": \"${version}\", \"diagnostics\": ["
           "{\"range\": {\"start\": {\"line\": 1, \"character\": 4}, \"end\": {\"line\": 1, \"character\": 18}},"
           " \"severity\": 1, \"message\": \"undefined_name is not defined\", \"source\": \"fake\", \"code\": \"F821\"},"
           "{\"range\": {\"start\": {\"line\": 0, \"character\": 7}, \"end\": {\"line\": 0, \"character\": 9}},"
           " \"severity\": 2, \"message\": \"os unused\", \"source\": \"fake\", \"code\": \"F401\"}]}}]}]}";
}

std::string fileUri(const std::string& p) { return IdeLink::fileUri(p); }

void selectInA(int line, int c0, int c1) {
    GtkTextBuffer* b = ed()->sourceBuffer();
    GtkTextIter s, e;
    gtk_text_buffer_get_iter_at_line_offset(b, &s, line, c0);
    gtk_text_buffer_get_iter_at_line_offset(b, &e, line, c1);
    gtk_text_buffer_select_range(b, &s, &e);
}

// A diff tab by title, or null.
Document* diffTab(const std::string& title) {
    for (Document* d : ed()->documents())
        if (d->compare() && d->diffTitle() == title) return d;
    return nullptr;
}

void clickBar(const char* button) {
    GtkWidget* b = MCGuiFindButton(cv()->bar(), button);
    GCHECK(b != nullptr);
    if (b) MCGuiClick(b);
}

}  // namespace

MC_GUI_SUITE(idelink) {
    MCGuiStep("files, a stale lock file of a dead pid and another IDE's", [] {
        MCGuiWriteFile(F("a.py"), kA);
        MCGuiWriteFile(F("b.py"), kB);
        MCGuiWriteFile(F("c.txt"), "one\ntwo\nthree\n");
        mkdir(claudeDir().c_str(), 0700);
        mkdir(lockDir().c_str(), 0700);
        pid_t dead = fork();
        if (dead == 0) _exit(0);
        int status = 0;
        waitpid(dead, &status, 0);
        IdeLink::LockInfo own, other;
        own.pid = (int)dead;
        own.ideName = IdeLink::kIdeName;
        own.transport = "ws";
        own.authToken = "stale";
        other = own;
        other.ideName = "Visual Studio Code";
        MCGuiWriteFile(lockDir() + "/40001.lock", IdeLink::lockFileText(own));
        MCGuiWriteFile(lockDir() + "/40002.lock", IdeLink::lockFileText(other));
        GCHECK(ide() != nullptr && !ide()->running());   // off by default
        MCGuiOpenFileKept(F("a.py"));
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + MCGuiFakeLsp("idelink", script(F("a.py"))) +
                         "\nlsp.python.also = off\nagent.ide-link = on\n");
    });
    MCGuiWait("the link is listening", 10, [] { return ide()->running(); });
    MCGuiStep("the lock file: mode 0600, the token, the folder, our pid", [] {
        gPort = ide()->port();
        gToken = ide()->authToken();
        GCHECK(gPort > 0 && gToken.size() == 48);
        struct stat st;
        GCHECK(stat(ide()->lockPath().c_str(), &st) == 0);
        GCHECK((st.st_mode & 0777) == 0600);
        GCHECK_EQ_STR(ide()->lockPath(), lockDir() + "/" + std::to_string(gPort) + ".lock");
        IdeLink::LockInfo info;
        GCHECK(IdeLink::parseLockFile(readLock(), info));
        GCHECK(info.pid == (int)getpid() && info.ideName == "minicode" && info.transport == "ws");
        GCHECK_EQ_STR(info.authToken, gToken);
        bool folder = false;
        for (const std::string& f : info.workspaceFolders) folder = folder || real(f) == real(MCGuiProject());
        GCHECK(folder);
        GCHECK(!MCGuiFileExists(lockDir() + "/40001.lock"));   // minicode's own, pid gone
        GCHECK(MCGuiFileExists(lockDir() + "/40002.lock"));    // another IDE's stays
        GCHECK(getenv("HOME") && std::string(getenv("HOME")).find("/home/") != std::string::npos);
    });
    MCGuiStep("a wrong token, no token and a Host off the loopback are refused, nothing said first", [] {
        auto refused = [](const char* token, const std::string& host) {
            auto c = std::make_shared<WS>();
            GCHECK(c->connectTo(gPort, token, host));
            // Read what comes back, for up to 3 s.
            const gint64 end = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
            c->pump();
            while (c->head.empty() && !c->closed && g_get_monotonic_time() < end) {
                g_main_context_iteration(nullptr, FALSE);
                g_usleep(5000);
                c->pump();
            }
            const bool ok = !c->upgraded && (c->head.rfind("HTTP/1.1 4", 0) == 0 || c->closed);
            c->closeFd();
            return ok;
        };
        GCHECK(refused("0123456789abcdef0123456789abcdef0123456789abcdef", "127.0.0.1"));
        GCHECK(refused(nullptr, "127.0.0.1"));
        GCHECK(refused(gToken.c_str(), "rebind.example"));
        GCHECK(ide()->connectionCount() == 0);
    });
    MCGuiStep("a text file on show, then Claude Code connects with the token", [] {
        MCGuiOpenFileKept(F("a.py"));
        MCGuiFocusKeyWindow();
        ed()->focusActiveDocument();
        GCHECK(gA.connectTo(gPort, gToken.c_str()));
    });
    MCGuiWait("the upgrade is accepted", 5, [] { gA.pump(); return gA.upgraded; });
    MCGuiStep("initialize, initialized and ide_connected", [] {
        Json p = Json::object();
        p.set("protocolVersion", "2025-06-18");
        p.set("capabilities", Json::object());
        p.set("clientInfo", args({{"name", "claude-code"}, {"version", "2.1.319"}}));
        gA.call(1, "initialize", p);
    });
    MCGuiWait("initialize is answered", 5, [] { return answered(1); });
    MCGuiStep("it names itself and the protocol", [] {
        Json r = gA.answer(1)["result"];
        GCHECK_EQ_STR(r["serverInfo"]["name"].asString(), "minicode");
        GCHECK(!r["protocolVersion"].asString().empty());
        gA.notify("notifications/initialized", Json::object());
        gA.notify("ide_connected", args({{"pid", (int)getpid()}}));
    });
    MCGuiWait("the link counts a client", 5, [] { gA.pump(); return ide()->clientCount() == 1; });
    MCGuiWait("the selection on show is sent on connecting", 5, [] {
        return !gA.notification("selection_changed").isNull();
    });
    MCGuiStep("tools/list offers the tools, not saveDocument or executeCode", [] {
        gA.call(2, "tools/list", Json::object());
    });
    MCGuiWait("listed", 5, [] { return answered(2); });
    MCGuiStep("the list", [] {
        const Json tools = gA.answer(2)["result"]["tools"];
        std::string names;
        for (size_t i = 0; i < tools.size(); i++) names += tools[i]["name"].asString() + " ";
        for (const char* n : {"openFile", "openDiff", "close_tab", "closeAllDiffTabs", "getDiagnostics"})
            GCHECK(names.find(std::string(n) + " ") != std::string::npos);
        GCHECK(names.find("saveDocument") == std::string::npos);
        GCHECK(names.find("executeCode") == std::string::npos);
    });

    // ---- the selection
    MCGuiStep("a real selection in a line with an astral character", [] {
        gA.texts.clear();
        selectInA(2, 6, 7);   // the "x" after U+1D11E: character 6, UTF-16 unit 7
    });
    MCGuiWait("selection_changed arrives", 5, [] {
        Json n = gA.notification("selection_changed");
        return !n.isNull() && n["params"]["text"].asString() == "x";
    });
    MCGuiStep("0-based lines, UTF-16 characters, the file's path and uri", [] {
        Json p = gA.notification("selection_changed")["params"];
        GCHECK_EQ_STR(p["filePath"].asString(), F("a.py"));
        GCHECK_EQ_STR(p["fileUrl"].asString(), fileUri(F("a.py")));
        GCHECK(p["selection"]["start"]["line"].asInt(-1) == 2);
        GCHECK(p["selection"]["start"]["character"].asInt(-1) == 7);
        GCHECK(p["selection"]["end"]["character"].asInt(-1) == 8);
        GCHECK(!p["selection"]["isEmpty"].asBool(true));
    });

    // ---- openFile
    MCGuiStep("openFile selects from startText to endText", [] {
        gA.tool(10, "openFile", args({{"filePath", "ide/b.py"},
                                      {"preview", true}, {"startText", "beta"}, {"endText", "delta"}}));
    });
    MCGuiWait("answered", 5, [] { return answered(10); });
    MCGuiStep("while the user types, the file is added beside their tab, unshown, the note waiting", [] {
        GCHECK(toolText(gA, 10).find("selected text from \"beta\" to \"delta\"") != std::string::npos);
        GCHECK_EQ_STR(ed()->currentPath(), F("a.py"));
        Document* b = nullptr;
        for (Document* d : ed()->documents()) if (d->path() == F("b.py")) b = d;
        GCHECK(b != nullptr && b->isAgentPreview());
        GCHECK(b && ed()->notes()->pendingFor(b));
        GCHECK(!ed()->notes()->shown());
        // The user leaves the editor: the next open shows.
        gtk_widget_grab_focus(MCGuiApp().tree->widget());
        gA.tool(13, "openFile", args({{"filePath", "ide/b.py"}, {"preview", true}, {"startText", "beta"}, {"endText", "delta"}}));
    });
    MCGuiWait("answered", 5, [] { return answered(13); });
    MCGuiWait("the note shows", 5, [] { return ed()->notes()->shown(); });
    MCGuiStep("the file shows in the agent's preview tab with a highlight and no note text", [] {
        GCHECK_EQ_STR(ed()->currentPath(), F("b.py"));
        Document* d = ed()->active();
        GCHECK(d && d->isAgentPreview());
        AgentNotes* n = ed()->notes();
        GCHECK(n->firstLine() == 2 && n->lastLine() == 2);
        GCHECK(n->text().empty());
        GCHECK(n->label().find("Claude Code") != std::string::npos);
    });
    MCGuiStep("openFile with text that is not there says so; a missing file is an error", [] {
        gA.tool(11, "openFile", args({{"filePath", F("b.py")}, {"startText", "zzz"}}));
        gA.tool(12, "openFile", args({{"filePath", F("nope.py")}}));
    });
    MCGuiWait("answered", 5, [] { return answered(11) && answered(12); });
    MCGuiStep("both", [] {
        GCHECK(toolText(gA, 11).find("not found") != std::string::npos);
        GCHECK(toolError(gA, 12));
    });

    // ---- getDiagnostics
    MCGuiStep("getDiagnostics for a file with problems, under the uri it was asked with", [] {
        MCGuiOpenFileKept(F("a.py"));
        gNext = 20;
    });
    MCGuiWait("the server's diagnostics reach the window", 15, [] {
        return !MCGuiApp().lsp->problemRows().empty();
    });
    MCGuiStep("asked", [] { gA.tool(20, "getDiagnostics", args({{"uri", fileUri(F("a.py"))}})); });
    MCGuiWait("answered", 5, [] { return answered(20); });
    MCGuiStep("one file, its two problems, as asked", [] {
        Json j;
        GCHECK(Json::parse(toolText(gA, 20), j));
        GCHECK(j.size() == 1);
        GCHECK_EQ_STR(j[0]["uri"].asString(), fileUri(F("a.py")));
        GCHECK(j[0]["diagnostics"].size() == 2);
        bool found = false;
        for (size_t i = 0; i < j[0]["diagnostics"].size(); i++)
            found = found || j[0]["diagnostics"][i]["message"].asString().find("undefined_name") != std::string::npos;
        GCHECK(found);
        gA.tool(21, "getDiagnostics", args({{"uri", fileUri(F("b.py"))}}));
    });
    MCGuiWait("answered", 5, [] { return answered(21); });
    MCGuiStep("a file with none still answers under its uri", [] {
        Json j;
        GCHECK(Json::parse(toolText(gA, 21), j));
        GCHECK(j.size() == 1 && j[0]["diagnostics"].size() == 0);
        GCHECK_EQ_STR(j[0]["uri"].asString(), fileUri(F("b.py")));
    });

    // ---- openDiff
    MCGuiStep("openDiff opens a comparison tab with the decision bar", [] {
        gA.tool(30, "openDiff", args({{"old_file_path", F("c.txt")}, {"new_file_path", F("c.txt")},
                                      {"new_file_contents", "one\n2\nthree\nfour\n"},
                                      {"tab_name", "c.txt (Claude Code)"}}));
    });
    MCGuiWait("the diff tab shows", 10, [] { return diffTab("c.txt (Claude Code)") != nullptr && ed()->isComparison() && cv(); });
    MCGuiWaitQuiet("it is laid out", 0.3, 5, [] { return (long)gtk_widget_get_height(cv()->widget()); });
    MCGuiStep("Accept is clicked for real", [] {
        Document* d = diffTab("c.txt (Claude Code)");
        GCHECK(d && d->decision() && d->decision()->decision() == 0);
        GCHECK(cv()->barText().find("Claude Code wants to change c.txt.") != std::string::npos);
        GCHECK(!answered(30));   // nothing is said until the user decides
        clickBar("Accept");
    });
    MCGuiWait("the answer comes", 10, [] { return answered(30); });
    MCGuiStep("FILE_SAVED and the new text; the file itself untouched", [] {
        GCHECK_EQ_STR(toolText(gA, 30, 0), "FILE_SAVED");
        GCHECK_EQ_STR(toolText(gA, 30, 1), "one\n2\nthree\nfour\n");
        GCHECK_EQ_STR(MCGuiReadFile(F("c.txt")), "one\ntwo\nthree\n");
        gA.tool(31, "openDiff", args({{"new_file_path", F("c.txt")}, {"new_file_contents", "uno\n"},
                                      {"tab_name", "reject me"}}));
    });
    MCGuiWait("the second diff shows", 10, [] { return diffTab("reject me") != nullptr; });
    MCGuiStep("closing the tab rejects", [] {
        ed()->closeTab(diffTab("reject me"));
    });
    MCGuiWait("answered", 5, [] { return answered(31); });
    MCGuiStep("DIFF_REJECTED names the tab", [] {
        GCHECK_EQ_STR(toolText(gA, 31, 0), "DIFF_REJECTED");
        GCHECK_EQ_STR(toolText(gA, 31, 1), "reject me");
        gA.tool(32, "openDiff", args({{"new_file_path", F("c.txt")}, {"new_file_contents", "x\n"},
                                      {"tab_name", "t32"}}));
        gA.tool(33, "openDiff", args({{"new_file_path", F("c.txt")}, {"new_file_contents", "y\n"},
                                      {"tab_name", "t33"}}));
    });
    MCGuiWait("both show", 10, [] { return diffTab("t32") && diffTab("t33"); });
    MCGuiStep("close_tab closes one by name; a diff of the user's stays through closeAllDiffTabs", [] {
        CompareSpec mine;
        mine.title = "mine";
        mine.leftText = "a\n";
        mine.rightText = "b\n";
        mine.quiet = true;
        ed()->showComparison(mine);
        gA.tool(34, "close_tab", args({{"tab_name", "t32"}}));
    });
    MCGuiWait("closed", 5, [] { return answered(34) && diffTab("mine") != nullptr; });
    MCGuiStep("only t32 went", [] {
        GCHECK_EQ_STR(toolText(gA, 34), "TAB_CLOSED");
        GCHECK(!diffTab("t32") && diffTab("t33") && diffTab("mine"));
        gA.tool(35, "closeAllDiffTabs", Json::object());
    });
    MCGuiWait("closed all", 5, [] { return answered(35); });
    MCGuiStep("t33 went, the user's own comparison stayed", [] {
        GCHECK_EQ_STR(toolText(gA, 35), "CLOSED_1_DIFF_TABS");
        GCHECK(!diffTab("t33") && diffTab("mine"));
        ed()->closeTab(diffTab("mine"));
    });

    // ---- the terminal's environment, the menu item and the lock file
    MCGuiStep("terminals get the port while the link is on", [] {
        const std::vector<std::string> env = ide()->terminalEnvironment();
        GCHECK(env.size() == 1 && env[0] == "CLAUDE_CODE_SSE_PORT=" + std::to_string(gPort));
    });
    MCGuiStep("View > Claude Code IDE Link turns it off through the settings file", [] {
        g_action_group_activate_action(G_ACTION_GROUP(MCGuiApplication()), "idelink", nullptr);
    });
    MCGuiWait("the link stops and the lock file goes", 10, [] { return !ide()->running(); });
    MCGuiStep("the file says off, the lock file is gone, the client was dropped", [] {
        GCHECK(MCGuiReadFile(getenv("MINICODE_SETTINGS")).find("agent.ide-link = off") != std::string::npos);
        GCHECK(!MCGuiFileExists(lockDir() + "/" + std::to_string(gPort) + ".lock"));
        GCHECK(ide()->terminalEnvironment().empty());
    });
    MCGuiWait("the client sees it close", 5, [] { gA.pump(); return gA.closed; });
    MCGuiStep("and on again, a new port and token", [] {
        gA.closeFd();
        g_action_group_activate_action(G_ACTION_GROUP(MCGuiApplication()), "idelink", nullptr);
    });
    MCGuiWait("listening again", 10, [] { return ide()->running(); });
    MCGuiStep("a lock file again, 0600", [] {
        GCHECK(MCGuiReadFile(getenv("MINICODE_SETTINGS")).find("agent.ide-link = on") != std::string::npos);
        struct stat st;
        GCHECK(stat(ide()->lockPath().c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
        GCHECK(ide()->authToken() != gToken);
        MCGuiSetSettings("lsp.enabled = false\nagent.ide-link = off\n");
    });
    MCGuiWait("off for good", 10, [] { return !ide()->running(); });
    MCGuiStep("no lock file of ours is left", [] {
        for (const char* n : {"40001.lock"}) GCHECK(!MCGuiFileExists(lockDir() + "/" + n));
        GCHECK(ide()->lockPath().empty());
    });
}
