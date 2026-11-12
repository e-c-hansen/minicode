// ControlSuite.cpp — the control channel, part one (`./build/minicode-guitest
// control`, linux/PARITY.md item 3.1; the Mac's tests/gui/ControlSuite.mm).
//
// The client is the real build/minicode (`minicode open|window|state|
// selection`), run as a subprocess the main loop keeps turning for, since the
// server is this very process: a client run on the loop's thread would wait
// for an answer the loop cannot give. What is checked:
//   - the socket is <pid>.sock in a 0700 folder, 0600;
//   - `open path:line:col` shows the file and puts the caret there, and
//     `--to` selects up to a line's end;
//   - `state` names the window, its tabs and the selection, `selection`
//     answers the selected text;
//   - an unknown command, a misspelled field and a value of the wrong type
//     are error answers over the raw socket, with the request's id kept;
//   - a missing file and the commands of later items answer a
//     clear error and exit 1, never a crash; a request line cut short and
//     a peer that goes away leave the app answering;
//   - with no minicode running `state` exits 3 at once;
//   - sockets of dead processes, and of live pids where nothing listens, are
//     removed at startup, and one with a listener behind it is kept.
// The refusal of another user's peer (SO_PEERCRED) is one comparison in the
// accept handler and cannot be tested without a second user.
#include "Harness.h"

#include "ControlPaths.h"
#include "ControlServer.h"
#include "CompareView.h"
#include "Document.h"
#include "Editor.h"
#include "GitRun.h"
#include "FileTree.h"
#include "Json.h"

#include <gio/gio.h>
#include <glib/gstdio.h>

#include <cstring>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

std::string F(const std::string& name) { return MCGuiProjectFile("control/" + name); }
Editor* ed() { return MCGuiApp().editor; }
size_t gTabs = 0;

std::string folder() {
    return ControlChannel::socketFolder(getenv("MINICODE_CONTROL_DIR"), getenv("XDG_RUNTIME_DIR"),
                                        getenv("TMPDIR"), (unsigned)getuid());
}

// One client run, finished when `done`.
struct Run {
    bool done = false;
    int exitCode = -1;
    std::string out, err;
};

std::shared_ptr<Run> runClient(const std::vector<std::string>& args, const char* runtimeDir = nullptr) {
    auto run = std::make_shared<Run>();
    GSubprocessLauncher* l = g_subprocess_launcher_new(
        (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE));
    if (runtimeDir) {
        g_subprocess_launcher_setenv(l, "XDG_RUNTIME_DIR", runtimeDir, TRUE);
        g_subprocess_launcher_unsetenv(l, "MINICODE_CONTROL_DIR");
    }
    std::vector<const char*> argv = {nullptr};
    const std::string exe = MCGuiBuildDir() + "/minicode";
    argv[0] = exe.c_str();
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    GError* error = nullptr;
    GSubprocess* p = g_subprocess_launcher_spawnv(l, argv.data(), &error);
    g_object_unref(l);
    if (!p) {
        run->err = error ? error->message : "spawn failed";
        run->done = true;
        g_clear_error(&error);
        return run;
    }
    g_subprocess_communicate_utf8_async(
        p, nullptr, nullptr,
        +[](GObject* source, GAsyncResult* res, gpointer data) {
            auto* holder = static_cast<std::shared_ptr<Run>*>(data);
            Run& r = **holder;
            char *out = nullptr, *err = nullptr;
            g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), res, &out, &err, nullptr);
            r.out = out ? out : "";
            r.err = err ? err : "";
            g_free(out);
            g_free(err);
            r.exitCode = g_subprocess_get_if_exited(G_SUBPROCESS(source))
                             ? g_subprocess_get_exit_status(G_SUBPROCESS(source))
                             : -1;
            r.done = true;
            delete holder;
            g_object_unref(source);
        },
        new std::shared_ptr<Run>(run));
    return run;
}

// One raw exchange over the socket: `request` written, one line read back
// (or the connection closed: `answer` stays empty, `closed` set).
struct Raw {
    bool done = false, closed = false;
    std::string answer;
    GSocketConnection* conn = nullptr;
    GDataInputStream* in = nullptr;
    std::string request;
};

void rawFinish(Raw* r) {
    r->done = true;
    if (r->in) g_object_unref(r->in);
    if (r->conn) g_object_unref(r->conn);
    r->in = nullptr;
    r->conn = nullptr;
}

std::shared_ptr<Raw> rawRequest(const std::string& path, const std::string& request,
                                bool shutdownWrite = false) {
    auto raw = std::make_shared<Raw>();
    raw->request = request;
    GSocketClient* client = g_socket_client_new();
    GSocketAddress* addr = g_unix_socket_address_new(path.c_str());
    struct Ctx {
        std::shared_ptr<Raw> raw;
        bool shutdownWrite;
    };
    g_socket_client_connect_async(
        client, G_SOCKET_CONNECTABLE(addr), nullptr,
        +[](GObject* source, GAsyncResult* res, gpointer data) {
            auto* ctx = static_cast<Ctx*>(data);
            Raw* r = ctx->raw.get();
            r->conn = g_socket_client_connect_finish(G_SOCKET_CLIENT(source), res, nullptr);
            g_object_unref(source);
            if (!r->conn) {
                r->closed = true;
                rawFinish(r);
                delete ctx;
                return;
            }
            GOutputStream* out = g_io_stream_get_output_stream(G_IO_STREAM(r->conn));
            g_output_stream_write_all(out, r->request.data(), r->request.size(), nullptr, nullptr,
                                      nullptr);
            if (ctx->shutdownWrite)
                g_socket_shutdown(g_socket_connection_get_socket(r->conn), FALSE, TRUE, nullptr);
            r->in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(r->conn)));
            g_data_input_stream_read_line_async(
                r->in, G_PRIORITY_DEFAULT, nullptr,
                +[](GObject* s, GAsyncResult* rr, gpointer d) {
                    auto* c = static_cast<Ctx*>(d);
                    gsize len = 0;
                    char* line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(s), rr,
                                                                      &len, nullptr);
                    if (line) c->raw->answer.assign(line, len);
                    else c->raw->closed = true;
                    g_free(line);
                    rawFinish(c->raw.get());
                    delete c;
                },
                ctx);
        },
        new Ctx{raw, shutdownWrite});
    g_object_unref(addr);
    return raw;
}

Json parse(const std::string& s) {
    Json j;
    std::string err;
    if (!Json::parse(s, j, &err)) return Json();
    return j;
}

std::string socketPath() { return folder() + "/" + std::to_string((int)getpid()) + ".sock"; }

// A socket file at `path` with nothing listening (or with a listener when
// `listen`, whose descriptor is returned).
int makeSocketFile(const std::string& path, bool listening) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    unlink(path.c_str());
    if (bind(fd, (struct sockaddr*)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    if (listening) return listen(fd, 4) == 0 ? fd : -1;
    close(fd);
    return -2;
}

bool exists(const std::string& path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0;
}

std::shared_ptr<Run> gRun;
std::shared_ptr<Raw> gRaw;
Json gJson;
int gListener = -1;

}  // namespace

MC_GUI_SUITE(control) {
    MCGuiStep("makes the files", [] {
        g_mkdir_with_parents(F("").c_str(), 0755);
        g_file_set_contents(F("a.txt").c_str(),
                            "one\ntwo\nthree four\nfive\nsix\nseven\n", -1, nullptr);
        g_file_set_contents(F("b.txt").c_str(), "alpha\nbeta\ngamma\ndelta\nepsilon\n", -1,
                            nullptr);
    });

    // ---- the socket
    MCGuiStep("the socket is in a private folder, itself private", [] {
        struct stat dir, sock;
        GCHECK(stat(folder().c_str(), &dir) == 0 && S_ISDIR(dir.st_mode));
        GCHECK((dir.st_mode & 0777) == 0700);
        GCHECK(dir.st_uid == getuid());
        GCHECK(stat(socketPath().c_str(), &sock) == 0 && S_ISSOCK(sock.st_mode));
        GCHECK((sock.st_mode & 0777) == 0600);
    });

    // ---- open, with a line and column
    MCGuiStep("minicode open a.txt:3:6", [] { gRun = runClient({"open", F("a.txt") + ":3:6"}); });
    MCGuiWait("the client returns", 20, [] { return gRun->done; });
    MCGuiStep("it exited 0 and printed no error", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(gRun->err.empty());
    });
    MCGuiWait("the file is on show", 10, [] { return ed()->currentPath() == F("a.txt"); });
    MCGuiWait("the caret is at line 3, column 6", 10, [] {
        GtkTextBuffer* b = ed()->sourceBuffer();
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_mark(b, &it, gtk_text_buffer_get_insert(b));
        return gtk_text_iter_get_line(&it) == 2 && gtk_text_iter_get_line_offset(&it) == 5;
    });

    // ---- state and selection
    MCGuiStep("minicode state", [] { gRun = runClient({"state"}); });
    MCGuiWait("state returns", 20, [] { return gRun->done; });
    MCGuiStep("state names the window, its tab and the caret", [] {
        GCHECK(gRun->exitCode == 0);
        gJson = parse(gRun->out);
        GCHECK(gJson["pid"].asInt() == (int)getpid());
        const Json& w = gJson["windows"][(size_t)0];
        GCHECK(w["id"].asInt() >= 1);
        GCHECK(w["folder"].asString() == MCGuiProject());
        bool tab = false;
        for (size_t i = 0; i < w["tabs"].size(); i++)
            if (w["tabs"][i]["path"].asString() == F("a.txt") && w["tabs"][i]["active"].asBool())
                tab = true;
        GCHECK(tab);
        GCHECK(w["selection"]["path"].asString() == F("a.txt"));
        GCHECK(w["selection"]["start"]["line"].asInt() == 3);
        GCHECK(w["selection"]["start"]["column"].asInt() == 6);
        GCHECK(w["selection"]["text"].asString().empty());
        GCHECK(w["terminals"].size() == 1);
    });
    MCGuiStep("minicode state --pretty is indented", [] { gRun = runClient({"state", "--pretty"}); });
    MCGuiWait("pretty state returns", 20, [] { return gRun->done; });
    MCGuiStep("pretty state has line breaks and parses", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(gRun->out.find("\n  \"pid\"") != std::string::npos);
        GCHECK(parse(gRun->out)["pid"].asInt() == (int)getpid());
    });
    MCGuiStep("selects 'four' in the editor and asks selection", [] {
        GtkTextBuffer* b = ed()->sourceBuffer();
        GtkTextIter a, e;
        gtk_text_buffer_get_iter_at_line_offset(b, &a, 2, 6);
        gtk_text_buffer_get_iter_at_line_offset(b, &e, 2, 10);
        gtk_text_buffer_select_range(b, &a, &e);
        gRun = runClient({"selection"});
    });
    MCGuiWait("selection returns", 20, [] { return gRun->done; });
    MCGuiStep("it is the selected text", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK_EQ_STR(gRun->out, "four");
    });

    // ---- while the user types in the editor, a file opens beside theirs
    MCGuiStep("puts the keyboard in the editor", [] {
        MCGuiFocusKeyWindow();
        gtk_widget_grab_focus(ed()->textView());
    });
    MCGuiWait("the window is active with the keyboard in the text", 10, [] {
        return gtk_window_is_active(MCGuiApp().window) && gtk_widget_has_focus(ed()->textView());
    });
    MCGuiStep("minicode open b.txt:2:2 --to 3:3 while the keyboard is in the editor", [] {
        GCHECK(gtk_widget_has_focus(ed()->textView()));
        gRun = runClient({"open", F("b.txt") + ":2:2", "--to", "3:3"});
    });
    MCGuiWait("the second open returns", 20, [] { return gRun->done; });
    MCGuiStep("a tab beside theirs, not shown, and the client says so", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(gRun->err.find("the user is typing in the editor") != std::string::npos);
        GCHECK(ed()->documentFor(F("b.txt")) != nullptr);
        GCHECK(ed()->documentFor(F("a.txt")) != nullptr);
        GCHECK(ed()->currentPath() == F("a.txt"));
        GCHECK(ed()->documentFor(F("a.txt")) == ed()->active());
    });


    // ---- minicode diff, as a real client
    MCGuiStep("minicode diff a b while the keyboard is in the editor", [] {
        MCGuiWriteFile(F("d1.txt"), "one\ntwo\nthree\n");
        MCGuiWriteFile(F("d2.txt"), "one\n2\nthree\nfour\n");
        gTabs = (size_t)ed()->tabCount();
        gtk_widget_grab_focus(ed()->textView());
        gRun = runClient({"diff", F("d1.txt"), F("d2.txt")});
    });
    MCGuiWait("the client returns", 30, [] { return gRun->done; });
    MCGuiStep("a comparison tab beside theirs, not shown, and the client says so", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(gRun->err.find("the user is typing in the editor") != std::string::npos);
        GCHECK((size_t)ed()->tabCount() == gTabs + 1);
        GCHECK(ed()->currentPath() == F("a.txt"));
        Document* d = nullptr;
        for (int i = 0; i < ed()->tabCount(); i++)
            if (ed()->document(i)->compare()) d = ed()->document(i);
        GCHECK(d && d->compare());
        if (d && d->compare()) {
            GCHECK_EQ_STR(d->compare()->spec.title, "d1.txt \xE2\x86\x94 d2.txt");
            GCHECK(d->compare()->changedRows == 2);   // one changed, one added
            GCHECK(d->compare()->hunks.size() == 2);
        }
    });
    MCGuiStep("minicode diff of a committed file against HEAD, with the keyboard in the tree", [] {
        gtk_widget_grab_focus(MCGuiApp().tree->widget());
        const std::string repo = F("repo");
        MCGuiWriteFile(repo + "/f.txt", "alpha\nbeta\n");
        runGit(repo, {"init", "-q"});
        runGit(repo, {"add", "f.txt"});
        runGit(repo, {"-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "x"});
        MCGuiWriteFile(repo + "/f.txt", "alpha\nBETA\n");
        gRun = runClient({"diff", repo + "/f.txt"});
    });
    MCGuiWait("the client returns", 30, [] { return gRun->done; });
    MCGuiWait("it is shown", 10, [] { return ed()->isComparison(); });
    MCGuiStep("left is HEAD's text, right the working copy", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK_EQ_STR(ed()->diffTitle(), "f.txt (HEAD \xE2\x86\x94 working copy)");
        const CompareData* d = ed()->active() ? ed()->active()->compare().get() : nullptr;
        GCHECK(d && d->spec.leftText == "alpha\nbeta\n" && d->spec.rightText == "alpha\nBETA\n");
        GCHECK(d && d->changedRows == 1);
    });
    MCGuiStep("--base names another commit; a bad one, a missing file and a folder are errors", [] {
        gRun = runClient({"diff", F("repo") + "/f.txt", "--base", "nosuchrev"});
    });
    MCGuiWait("returns", 30, [] { return gRun->done; });
    MCGuiStep("no commit named", [] {
        GCHECK(gRun->exitCode == 1);
        GCHECK(gRun->err.find("no commit named nosuchrev") != std::string::npos);
        gRun = runClient({"diff", F("repo") + "/f.txt", "--base", "--evil"});
    });
    MCGuiWait("returns", 30, [] { return gRun->done; });
    MCGuiStep("a base starting with a dash is refused", [] {
        GCHECK(gRun->exitCode != 0);
        gRun = runClient({"diff", F("nosuchfile.txt")});
    });
    MCGuiWait("returns", 30, [] { return gRun->done; });
    MCGuiStep("a missing file", [] {
        GCHECK(gRun->exitCode == 1);
        GCHECK(gRun->err.find("no such file") != std::string::npos);
        gRun = runClient({"diff", F("repo"), F("d1.txt")});
    });
    MCGuiWait("returns", 30, [] { return gRun->done; });
    MCGuiStep("a folder is not a plain file", [] {
        GCHECK(gRun->exitCode == 1);
        GCHECK(gRun->err.find("not a plain file") != std::string::npos);
    });

    // ---- open --to, with the keyboard elsewhere
    MCGuiStep("moves the keyboard to the tree", [] {
        gtk_widget_grab_focus(MCGuiApp().tree->widget());
    });
    MCGuiWait("the editor no longer has it", 5, [] { return !gtk_widget_has_focus(ed()->textView()); });
    MCGuiStep("minicode open b.txt:2:2 --to 3:3 again", [] {
        gRun = runClient({"open", F("b.txt") + ":2:2", "--to", "3:3"});
    });
    MCGuiWait("the third open returns", 20, [] { return gRun->done; });
    MCGuiWait("b.txt shows with the range selected", 10, [] {
        if (ed()->currentPath() != F("b.txt")) return false;
        GtkTextBuffer* b = ed()->sourceBuffer();
        GtkTextIter a, e;
        if (!gtk_text_buffer_get_selection_bounds(b, &a, &e)) return false;
        return gtk_text_iter_get_line(&a) == 1 && gtk_text_iter_get_line_offset(&a) == 1 &&
               gtk_text_iter_get_line(&e) == 2 && gtk_text_iter_get_line_offset(&e) == 2;
    });
    MCGuiStep("a.txt's tab is still there, the tabs are not reordered or replaced", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(gRun->err.empty());
        GCHECK(ed()->documentFor(F("a.txt")) != nullptr);
        GCHECK(ed()->tabCount() >= 2);
    });

    // ---- window
    MCGuiStep("minicode window", [] { gRun = runClient({"window"}); });
    MCGuiWait("window returns", 20, [] { return gRun->done; });
    MCGuiStep("it prints the window's id", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(atoi(gRun->out.c_str()) >= 1);
    });

    // ---- errors over the raw socket
    MCGuiStep("an unknown command", [] {
        gRaw = rawRequest(socketPath(), "{\"v\":1,\"id\":7,\"cmd\":\"nope\",\"args\":{}}\n");
    });
    MCGuiWait("it is answered", 10, [] { return gRaw->done; });
    MCGuiStep("an error answer that keeps the id", [] {
        Json j = parse(gRaw->answer);
        GCHECK(!j["ok"].asBool());
        GCHECK(j["id"].asInt() == 7);
        GCHECK(!j["error"].asString().empty());
    });
    MCGuiStep("a misspelled field", [] {
        gRaw = rawRequest(socketPath(),
                          "{\"v\":1,\"id\":8,\"cmd\":\"window\",\"args\":{\"flodre\":\"x\"}}\n");
    });
    MCGuiWait("answered", 10, [] { return gRaw->done; });
    MCGuiStep("an error answer", [] {
        Json j = parse(gRaw->answer);
        GCHECK(!j["ok"].asBool());
        GCHECK(j["id"].asInt() == 8);
    });
    MCGuiStep("a value of the wrong type", [] {
        gRaw = rawRequest(socketPath(),
                          "{\"v\":1,\"id\":9,\"cmd\":\"window\",\"args\":{\"new\":\"yes\"}}\n");
    });
    MCGuiWait("answered again", 10, [] { return gRaw->done; });
    MCGuiStep("an error answer too", [] {
        Json j = parse(gRaw->answer);
        GCHECK(!j["ok"].asBool());
        GCHECK(j["id"].asInt() == 9);
    });
    MCGuiStep("a line that is not JSON", [] { gRaw = rawRequest(socketPath(), "not json\n"); });
    MCGuiWait("answered a third time", 10, [] { return gRaw->done; });
    MCGuiStep("an error answer, not a crash", [] {
        Json j = parse(gRaw->answer);
        GCHECK(!j["ok"].asBool());
    });
    MCGuiStep("a part of a later item (term --split)", [] {
        gRaw = rawRequest(socketPath(),
                          "{\"v\":1,\"id\":10,\"cmd\":\"term\",\"args\":{\"split\":\"right\"}}\n");
    });
    MCGuiWait("answered a fourth time", 10, [] { return gRaw->done; });
    MCGuiStep("it says it is not on Linux yet", [] {
        Json j = parse(gRaw->answer);
        GCHECK(!j["ok"].asBool());
        GCHECK(j["error"].asString().find("not on Linux yet") != std::string::npos);
    });
    MCGuiStep("a request cut short by the peer closing", [] {
        gRaw = rawRequest(socketPath(), "{\"v\":1,\"id\":11,\"cmd\":\"sta", true);
    });
    MCGuiWait("the server closes that connection", 10, [] { return gRaw->done; });
    MCGuiStep("and still answers the next peer", [] {
        GCHECK(gRaw->closed);
        gRaw = rawRequest(socketPath(), "{\"v\":1,\"id\":12,\"cmd\":\"ping\",\"args\":{}}\n");
    });
    MCGuiWait("ping is answered", 10, [] { return gRaw->done; });
    MCGuiStep("with this process's id", [] {
        Json j = parse(gRaw->answer);
        GCHECK(j["ok"].asBool());
        GCHECK(j["result"]["pid"].asInt() == (int)getpid());
    });

    // ---- errors through the client
    MCGuiStep("minicode open of a file that is not there", [] {
        gRun = runClient({"open", F("missing.txt")});
    });
    MCGuiWait("that returns", 20, [] { return gRun->done; });
    MCGuiStep("exit 1 and the reason", [] {
        GCHECK(gRun->exitCode == 1);
        GCHECK(gRun->err.find("no such file") != std::string::npos);
    });
    MCGuiStep("minicode term --right (split)", [] { gRun = runClient({"split", "--right", "term"}); });
    MCGuiWait("term returns", 20, [] { return gRun->done; });
    MCGuiStep("exit 1, not on Linux yet", [] {
        GCHECK(gRun->exitCode == 1);
        GCHECK(gRun->err.find("not on Linux yet") != std::string::npos);
    });
    MCGuiStep("minicode open with no path is a usage error", [] { gRun = runClient({"open"}); });
    MCGuiWait("usage error returns", 20, [] { return gRun->done; });
    MCGuiStep("exit 2", [] { GCHECK(gRun->exitCode == 2); });

    // ---- no minicode running
    MCGuiStep("minicode state with no minicode running", [] {
        const std::string empty = MCGuiScratch() + "/control-empty";
        g_mkdir_with_parents(empty.c_str(), 0700);
        gRun = runClient({"state"}, empty.c_str());
    });
    MCGuiWait("it returns at once", 20, [] { return gRun->done; });
    MCGuiStep("exit 3 and not running", [] {
        GCHECK(gRun->exitCode == 3);
        GCHECK(gRun->err.find("not running") != std::string::npos);
    });

    // ---- stale sockets
    MCGuiStep("sockets of a dead pid and of a live pid with nothing behind it", [] {
        GCHECK(makeSocketFile(folder() + "/4194000.sock", false) == -2);   // pid beyond pid_max
        GCHECK(makeSocketFile(folder() + "/1.sock", false) == -2);         // init: alive
        gListener = makeSocketFile(folder() + "/" + std::to_string((int)getppid()) + ".sock", true);
        GCHECK(gListener >= 0);
        GCHECK(exists(folder() + "/4194000.sock") && exists(folder() + "/1.sock"));
    });
    MCGuiStep("a start removes the first two and keeps the one that listens", [] {
        const int removed = ControlServer::removeStale(folder(), (int)getpid());
        GCHECK(removed >= 2);   // others left by earlier suites' minicodes may go too
        GCHECK(!exists(folder() + "/4194000.sock"));
        GCHECK(!exists(folder() + "/1.sock"));
        GCHECK(exists(folder() + "/" + std::to_string((int)getppid()) + ".sock"));
        GCHECK(exists(socketPath()));   // its own is never touched
        if (gListener >= 0) close(gListener);
        unlink((folder() + "/" + std::to_string((int)getppid()) + ".sock").c_str());
    });
    // ---- minicode install-skill (PARITY.md 3.1): the skill, copied, never into the real ~/.claude
    MCGuiStep("minicode install-skill --dir copies the skill that ships", [] {
        gRun = runClient({"install-skill", "--dir", F("skill-home/skills/minicode")});
    });
    MCGuiWait("install-skill returns", 20, [] { return gRun->done; });
    MCGuiStep("it exited 0, said so, and the file is the shipped one", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(gRun->out.find("Installed the minicode skill in") != std::string::npos);
        const std::string shipped = MCGuiReadFile(MCGuiBuildDir() + "/../../skills/minicode/SKILL.md");
        GCHECK(!shipped.empty());
        GCHECK(MCGuiReadFile(F("skill-home/skills/minicode/SKILL.md")) == shipped);
        gRun = runClient({"install-skill", "--dir", F("skill-home/skills/minicode")});
    });
    MCGuiWait("the second run returns", 20, [] { return gRun->done; });
    MCGuiStep("a second run finds it up to date; a stray word is a usage error", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(gRun->out.find("is up to date") != std::string::npos);
        gRun = runClient({"install-skill", "--bogus"});
    });
    MCGuiWait("the third run returns", 20, [] { return gRun->done; });
    MCGuiStep("it exited 2", [] { GCHECK(gRun->exitCode == 2); });
}
