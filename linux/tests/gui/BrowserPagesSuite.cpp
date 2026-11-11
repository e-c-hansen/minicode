// BrowserPagesSuite.cpp — agents checking pages on Linux (`./build/minicode-guitest
// browser_pages`, PARITY.md item 3.7; the Mac's BrowserPagesSuite). The real
// client (`build/minicode`) against the running app, pages from a small HTTP
// server inside this process and from files, nothing off this machine:
//   - browse loads a local page in the agent page; page --url, --text and
//     --console (a console.log with its object, an uncaught error) read it;
//   - the HTTP status comes from the response (a 404);
//   - a screenshot makes a PNG at a new path, is refused onto an existing
//     file, through a symlink, and onto a file without --overwrite, and
//     replaces one with it;
//   - a page that is not local (about:blank) gives no text;
//   - the keyboard in the pane keeps the agent page from being shown
//     (pane_busy); the switcher's buttons and the x work with real clicks;
//   - open-url puts a local page in the user's page, and anything else goes
//     to the fallback (a recorder here, xdg-open in life); BROWSER in the
//     app's terminal names the script that does it.
#include "Harness.h"

#include "AppSeam.h"
#include "Browser.h"
#include "BrowserPages.h"
#include "Editor.h"
#include "Terminal.h"
#include "WindowLayout.h"

#include <gio/gio.h>
#include <vte/vte.h>

#include <map>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct Run {
    bool done = false;
    int exitCode = -1;
    std::string out, err;
};

std::shared_ptr<Run> client(const std::vector<std::string>& args,
                            const std::map<std::string, std::string>& env = {},
                            const std::string& program = "") {
    auto run = std::make_shared<Run>();
    GSubprocessLauncher* l = g_subprocess_launcher_new(
        (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE));
    for (const auto& kv : env) g_subprocess_launcher_setenv(l, kv.first.c_str(), kv.second.c_str(), TRUE);
    std::vector<const char*> argv;
    const std::string exe = program.empty() ? MCGuiBuildDir() + "/minicode" : program;
    argv.push_back(exe.c_str());
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
    g_subprocess_communicate_async(
        p, nullptr, nullptr,
        +[](GObject* source, GAsyncResult* res, gpointer data) {
            auto* holder = static_cast<std::shared_ptr<Run>*>(data);
            Run& r = **holder;
            GBytes *out = nullptr, *err = nullptr;
            g_subprocess_communicate_finish(G_SUBPROCESS(source), res, &out, &err, nullptr);
            if (out) r.out.assign((const char*)g_bytes_get_data(out, nullptr), g_bytes_get_size(out));
            if (err) r.err.assign((const char*)g_bytes_get_data(err, nullptr), g_bytes_get_size(err));
            if (out) g_bytes_unref(out);
            if (err) g_bytes_unref(err);
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

// ---- a tiny HTTP server on 127.0.0.1: /ok, anything else a 404.
GSocketService* gServer = nullptr;
int gPort = 0;

gboolean onIncoming(GThreadedSocketService*, GSocketConnection* conn, GObject*, gpointer) {
    GInputStream* in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    char buf[4096];
    gssize n = g_input_stream_read(in, buf, sizeof buf - 1, nullptr, nullptr);
    if (n <= 0) return TRUE;
    buf[n] = 0;
    const bool ok = std::string(buf).rfind("GET /ok", 0) == 0;
    const bool boom = std::string(buf).rfind("GET /boom", 0) == 0;
    const std::string body = boom ? "<p>boom page</p><script>throw new Error('boom from http');</script>"
                             : ok ? "<title>fine</title><p>served fine</p>" : "<p>nothing here</p>";
    const std::string head = std::string("HTTP/1.1 ") + (ok || boom ? "200 OK" : "404 Not Found") +
                             "\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: " +
                             std::to_string(body.size()) + "\r\n\r\n";
    GOutputStream* out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    const std::string all = head + body;
    g_output_stream_write_all(out, all.data(), all.size(), nullptr, nullptr, nullptr);
    g_io_stream_close(G_IO_STREAM(conn), nullptr, nullptr);
    return TRUE;
}

std::shared_ptr<Run> gRun;
std::string gPage, gShot, gRecorder, gRecorded, gBrowserVar;

std::string idStr() { return std::to_string(MCGuiApp().terminal->controlId()); }
std::map<std::string, std::string> appEnv() {
    return {{"MINICODE_TERMINAL_ID", idStr()}, {"MINICODE_PID", std::to_string((int)getpid())}};
}
Browser* browser() { return MCGuiApp().browser; }
bool has(const std::string& s, const std::string& w) { return s.find(w) != std::string::npos; }

VteTerminal* findVte(GtkWidget* root) {
    if (!root) return nullptr;
    if (VTE_IS_TERMINAL(root)) return VTE_TERMINAL(root);
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (VteTerminal* hit = findVte(c)) return hit;
    return nullptr;
}

bool isPng(const std::string& path) {
    const std::string d = MCGuiReadFile(path);
    return d.size() > 8 && d.compare(1, 3, "PNG") == 0;
}

}  // namespace

MC_GUI_SUITE(browser_pages) {
    MCGuiStep("the pane is shown and a server and pages are made", [] {
        GCHECK(browser() != nullptr);
        if (!browser()) return;
        MCGuiApp().layout->showBrowser();
        GError* error = nullptr;
        gServer = g_threaded_socket_service_new(4);
        gPort = g_socket_listener_add_any_inet_port(G_SOCKET_LISTENER(gServer), nullptr, &error);
        GCHECK(gPort > 0);
        g_signal_connect(gServer, "run", G_CALLBACK(onIncoming), nullptr);
        g_socket_service_start(gServer);
        gPage = MCGuiProjectFile("agent-page.html");
        MCGuiWriteFile(gPage,
                       "<!doctype html><title>Agent page</title><body><p>hello agent</p>"
                       "<script>console.log('from-log', {a: 1});</script>"
                       "<script>throw new Error('boom');</script></body>");
        gShot = MCGuiProjectFile("shot.png");
        gRecorded = MCGuiProjectFile("recorded.txt");
        gRecorder = MCGuiProjectFile("recorder.sh");
        MCGuiWriteFile(gRecorder, "#!/bin/sh\necho \"$1\" >> '" + gRecorded + "'\n");
        chmod(gRecorder.c_str(), 0755);
    });
    MCGuiWait("the user's page is made", 30, [] { return browser()->userProbe() != nullptr; });

    // ---- page before browse
    MCGuiStep("page --text with no agent page says so", [] { gRun = client({"page", "--text"}); });
    MCGuiWait("answered", 15, [] { return gRun->done; });
    MCGuiStep("it failed naming browse", [] {
        GCHECK(gRun->exitCode != 0);
        GCHECK(has(gRun->err, "minicode browse"));
        GCHECK(!browser()->hasAgentPage());
    });

    // ---- browse a local file, read it
    MCGuiStep("browse --wait loads the file", [] { gRun = client({"browse", "--wait", gPage}, appEnv()); });
    MCGuiWait("browse answered", 30, [] { return gRun->done; });
    MCGuiStep("the agent page is there and shown, with the switcher", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(has(gRun->out, "agent-page.html"));
        GCHECK(browser()->hasAgentPage());
        GCHECK(browser()->agentPageShown());
        GCHECK(gtk_widget_get_visible(browser()->switcher()));
        GCHECK(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(browser()->agentButton())));
        gRun = client({"page", "--text"});
    });
    MCGuiWait("text answered", 15, [] { return gRun->done; });
    MCGuiStep("the text is the page's", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(has(gRun->out, "hello agent"));
        gRun = client({"page", "--console"});
    });
    MCGuiWait("console answered", 15, [] { return gRun->done; });
    MCGuiStep("the console has the log with its object and the uncaught error", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(has(gRun->out, "from-log"));
        GCHECK(has(gRun->out, "a\\\":1"));
        GCHECK(has(gRun->out, "\"uncaught\":true"));   // a file page's message is masked: "Script error."
        gRun = client({"page", "--url"});
    });
    MCGuiWait("url answered", 15, [] { return gRun->done; });
    MCGuiStep("url names the file and the title", [] {
        GCHECK(has(gRun->out, "agent-page.html"));
        GCHECK(has(gRun->out, "Agent page"));
    });

    // ---- the status from the response
    MCGuiStep("browse a missing page on the server", [] {
        gRun = client({"browse", "--wait", "127.0.0.1:" + std::to_string(gPort) + "/nope"}, appEnv());
    });
    MCGuiWait("missing answered", 30, [] { return gRun->done; });
    MCGuiStep("its status is 404", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(has(gRun->out, "404"));
        gRun = client({"browse", "--wait", "http://127.0.0.1:" + std::to_string(gPort) + "/ok"}, appEnv());
    });
    MCGuiWait("ok answered", 30, [] { return gRun->done; });
    MCGuiStep("its status is 200 and its text is served", [] {
        GCHECK(has(gRun->out, "200"));
        gRun = client({"page", "--text"});
    });
    MCGuiWait("served text answered", 15, [] { return gRun->done; });
    MCGuiStep("it says served fine", [] { GCHECK(has(gRun->out, "served fine")); });

    MCGuiStep("an uncaught error on a served page keeps its message", [] {
        gRun = client({"browse", "--wait", "http://127.0.0.1:" + std::to_string(gPort) + "/boom"}, appEnv());
    });
    MCGuiWait("boom answered", 30, [] { return gRun->done; });
    MCGuiStep("the console has it", [] {
        GCHECK(gRun->exitCode == 0);
        gRun = client({"page", "--console"});
    });
    MCGuiWait("boom console answered", 15, [] { return gRun->done; });
    MCGuiStep("with the message and its place", [] {
        GCHECK(has(gRun->out, "boom from http"));
        GCHECK(!has(gRun->out, "from-log"));   // the earlier page's entries went with it
        gRun = client({"browse", "--wait", "http://127.0.0.1:" + std::to_string(gPort) + "/ok"}, appEnv());
    });
    MCGuiWait("ok again answered", 30, [] { return gRun->done; });

    // ---- screenshots
    MCGuiStep("a screenshot to a new path", [] { gRun = client({"page", "--screenshot", gShot}); });
    MCGuiWait("shot answered", 30, [] { return gRun->done; });
    MCGuiStep("it is a PNG; a second without --overwrite is refused", [] {
        GCHECK(gRun->exitCode == 0);
        if (gRun->exitCode != 0) fprintf(stderr, "    shot: %s\n", gRun->err.c_str());
        GCHECK(isPng(gShot));
        gRun = client({"page", "--screenshot", gShot});
    });
    MCGuiWait("refusal answered", 30, [] { return gRun->done; });
    MCGuiStep("it failed and the file is as it was; --full with --overwrite works", [] {
        GCHECK(gRun->exitCode != 0);
        GCHECK(isPng(gShot));
        gRun = client({"page", "--screenshot", gShot, "--full", "--overwrite"});
    });
    MCGuiWait("overwrite answered", 30, [] { return gRun->done; });
    MCGuiStep("it replaced the file; a symlink is refused even with --overwrite", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(isPng(gShot));
        const std::string victim = MCGuiProjectFile("victim.txt"), link = MCGuiProjectFile("link.png");
        MCGuiWriteFile(victim, "keep me");
        unlink(link.c_str());
        GCHECK(symlink(victim.c_str(), link.c_str()) == 0);
        gRun = client({"page", "--screenshot", link, "--overwrite"});
    });
    MCGuiWait("symlink answered", 30, [] { return gRun->done; });
    MCGuiStep("it failed and the target is untouched", [] {
        GCHECK(gRun->exitCode != 0);
        GCHECK_EQ_STR(MCGuiReadFile(MCGuiProjectFile("victim.txt")), "keep me");
    });

    // ---- a page that is not local
    MCGuiStep("browse about:blank, then ask for its text", [] {
        gRun = client({"browse", "--wait", "about:blank"}, appEnv());
    });
    MCGuiWait("blank answered", 30, [] { return gRun->done; });
    MCGuiStep("text of a page that is not local is refused", [] {
        GCHECK(gRun->exitCode == 0);
        gRun = client({"page", "--text"});
    });
    MCGuiWait("refusal 2 answered", 15, [] { return gRun->done; });
    MCGuiStep("the error is the origin limit's", [] {
        GCHECK(gRun->exitCode != 0);
        GCHECK(has(gRun->err, "local") || has(gRun->err, "origin"));
        GCHECK(!has(gRun->out, "hello"));
        gRun = client({"browse", "javascript:alert(1)"}, appEnv());
    });
    MCGuiWait("javascript answered", 15, [] { return gRun->done; });
    MCGuiStep("another scheme is refused", [] { GCHECK(gRun->exitCode != 0); });

    // ---- the keyboard in the pane
    MCGuiStep("the user's page is shown and the keyboard goes to the address bar", [] {
        MCGuiSetKeyWindow(MCGuiApp().window);
        browser()->showAgentPage(false);
        browser()->focusUrlBar();
        gRun = client({"browse", "--wait", gPage}, appEnv());
    });
    MCGuiWait("busy answered", 30, [] { return gRun->done; });
    MCGuiStep("the agent page loaded but was not shown: pane_busy", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(!browser()->agentPageShown());
        GCHECK(has(gRun->err, "using the browser pane"));
        gtk_widget_grab_focus(MCGuiApp().editor->textView());
        gRun = client({"browse", "--wait", gPage}, appEnv());
    });
    MCGuiWait("not busy answered", 30, [] { return gRun->done; });
    MCGuiStep("with the keyboard in the editor it is shown", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(browser()->agentPageShown());
    });

    // ---- the switcher and the x
    MCGuiStep("Yours is clicked", [] { MCGuiClick(browser()->yoursButton()); });
    MCGuiWait("the user's page shows", 5, [] { return !browser()->agentPageShown(); });
    MCGuiStep("Agent is clicked", [] { MCGuiClick(browser()->agentButton()); });
    MCGuiWait("the agent page shows", 5, [] { return browser()->agentPageShown(); });
    MCGuiStep("a link from elsewhere goes to the user's page", [] {
        browser()->load("file://" + MCGuiProjectFile("agent-page.html"));
        GCHECK(!browser()->agentPageShown());
        GCHECK(browser()->hasAgentPage());
    });
    MCGuiStep("the x is clicked", [] { MCGuiClick(browser()->closeButton()); });
    MCGuiWait("the agent page is gone", 5, [] { return !browser()->hasAgentPage(); });
    MCGuiStep("the user's page shows and the switcher is hidden", [] {
        GCHECK(!gtk_widget_get_visible(browser()->switcher()));
        GCHECK(browser()->userProbe() != nullptr);
        gRun = client({"page", "--text"});
    });
    MCGuiWait("no agent page answered", 15, [] { return gRun->done; });
    MCGuiStep("page --text now has no agent page", [] { GCHECK(gRun->exitCode != 0); });

    // ---- open-url and BROWSER
    MCGuiStep("open-url of a local page", [] {
        gRun = client({"open-url", "file://" + MCGuiProjectFile("agent-page.html")},
                      {{"MINICODE_TERMINAL_ID", idStr()},
                       {"MINICODE_PID", std::to_string((int)getpid())},
                       {"MINICODE_OPEN_FALLBACK", gRecorder}});
    });
    MCGuiWait("open-url answered", 30, [] { return gRun->done; });
    MCGuiWait("it lands in the user's page", 30, [] {
        return has(browser()->userProbe()->url(), "agent-page.html");
    });
    MCGuiStep("nothing went to the fallback; a remote address does", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(!MCGuiFileExists(gRecorded));
        gRun = client({"open-url", "https://example.com/x"},
                      {{"MINICODE_TERMINAL_ID", idStr()},
                       {"MINICODE_PID", std::to_string((int)getpid())},
                       {"MINICODE_OPEN_FALLBACK", gRecorder}});
    });
    MCGuiWait("fallback answered", 30, [] { return gRun->done && MCGuiFileExists(gRecorded); });
    MCGuiStep("the recorder got the address and the page did not move", [] {
        GCHECK_EQ_STR(MCGuiReadFile(gRecorded), "https://example.com/x\n");
        GCHECK(has(browser()->userProbe()->url(), "agent-page.html"));
    });
    MCGuiStep("the app's terminal shell writes $BROWSER to a file", [] {
        if (!MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
        MCGuiApp().terminal->typeText("true");
    });
    MCGuiWait("the terminal has its shell", 30, [] {
        VteTerminal* vte = findVte(MCGuiApp().terminal->widget());
        if (!vte || MCGuiApp().terminal->shellPid() <= 0) return false;
        char* text = vte_terminal_get_text_format(vte, VTE_FORMAT_TEXT);
        const bool there = text && std::string(text).find("true") != std::string::npos;
        g_free(text);
        return there;
    });
    MCGuiStep("it echoes BROWSER", [] {
        VteTerminal* vte = findVte(MCGuiApp().terminal->widget());
        vte_terminal_feed_child(vte, "\x15", 1);
        const std::string cmd = "echo \"$BROWSER\" > '" + MCGuiProjectFile("browser-var.txt") + "'\n";
        vte_terminal_feed_child(vte, cmd.c_str(), cmd.size());
    });
    MCGuiWait("the file is written", 15, [] {
        return !MCGuiReadFile(MCGuiProjectFile("browser-var.txt")).empty();
    });
    MCGuiStep("BROWSER names the script; run as a program it opens a local page", [] {
        gBrowserVar = MCGuiReadFile(MCGuiProjectFile("browser-var.txt"));
        while (!gBrowserVar.empty() && (gBrowserVar.back() == '\n' || gBrowserVar.back() == '\r'))
            gBrowserVar.pop_back();
        GCHECK(has(gBrowserVar, "minicode-browser.js"));
        GCHECK(access(gBrowserVar.c_str(), X_OK) == 0);
        MCGuiWriteFile(MCGuiProjectFile("viaenv.html"), "<title>via BROWSER</title><p>x</p>");
        gRun = client({"file://" + MCGuiProjectFile("viaenv.html")},
                      {{"MINICODE_TERMINAL_ID", idStr()},
                       {"MINICODE_PID", std::to_string((int)getpid())},
                       {"MINICODE_EXE", MCGuiBuildDir() + "/minicode"}},
                      gBrowserVar);
    });
    MCGuiWait("the script ran", 30, [] { return gRun->done; });
    MCGuiWait("the page is in the user's pane", 30, [] {
        return has(browser()->userProbe()->url(), "viaenv.html");
    });
    MCGuiStep("done", [] {
        GCHECK(gRun->exitCode == 0);
        if (gServer) g_socket_service_stop(gServer);
        if (gServer) g_object_unref(gServer);
        gServer = nullptr;
    });
}
