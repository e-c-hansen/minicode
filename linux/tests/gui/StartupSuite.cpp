// StartupSuite.cpp — the time to a usable window (`./build/minicode-guitest
// startup`, linux/PARITY.md R3). The owner found minicode taking seconds to
// open on the ThinkPad where the Mac takes under one. A second app opens a
// folder whose session brings back five tabs (a C++ file on show, four of
// 3 MB behind it) with a language server set for C++, and the startup log
// on (MINICODE_STARTUP_LOG, src/StartupLog.h). It checks, in the app:
//
//   - the first frame within a bound of main-thread CPU from main, and a
//     wall-time bound with room for a loaded machine;
//   - the terminal and the browser, hidden, not made: no shell, no WebKit
//     process, no bwrap or xdg-dbus-proxy below the app, as the Mac makes
//     its terminal dock and its browser when first shown;
//   - the language server started after the first frame (WhenDrawn.h);
//   - the terminal and the browser made once shown, with a shell and a
//     page, so what the user sees once the window is up is as before.
//
// and here, from the second app's log, that the first frame came with
// nothing started and the server after it.
//
// The bound: on the ThinkPad (Xvfb, software drawing, load 4 to 10) the
// first frame costs 165 to 300 ms of main-thread CPU from main, against 285
// to 390 before R3 and 140 to 175 for a bare GtkApplication window. 1500 ms
// leaves five times that for a slower runner, and is still under what
// reading the four 3 MB tabs at launch would add (about 350 ms of CPU each
// here), the regression the session's lazy tabs keep away. What R3 changed
// is checked by the processes and objects, which do not depend on speed.
#include "Harness.h"

#include "FakeLsp.h"
#include "Editor.h"
#include "Persistence.h"
#include "StartupLog.h"
#include "TabSessions.h"
#include "WindowLayout.h"

#ifdef MINICODE_ENABLE_TERMINAL
#include "Terminal.h"
#endif
#ifdef MINICODE_ENABLE_BROWSER
#include "Browser.h"
#include <webkit/webkit.h>
#endif

#include <unistd.h>

#include <string>
#include <vector>

namespace {

std::string Folder() { return MCGuiScratch() + "/startup"; }
std::string In(const std::string& name) { return Folder() + "/" + name; }
// The second app's own settings and state, so this app's are left alone.
std::string ChildSettings() { return MCGuiScratch() + "/startup-settings.conf"; }
std::string ChildState() { return MCGuiScratch() + "/startup-state"; }

const char* kBigTabs[] = {"big1.cpp", "big2.cpp", "big3.cpp", "big4.cpp"};

std::string CppLines(int lines) {
    std::string s;
    for (int i = 0; i < lines; i++)
        s += "int f" + std::to_string(i) + "(int a, int b) { return a * " + std::to_string(i) +
             " + b; }  // \"line\" " + std::to_string(i) + "\n";
    return s;
}

std::string ReadSmall(const std::string& path) {
    gchar* text = nullptr;
    if (!g_file_get_contents(path.c_str(), &text, nullptr, nullptr)) return "";
    std::string out = text;
    g_free(text);
    return out;
}

// The names of the processes below this one, from
// /proc/<pid>/task/<tid>/children.
void Below(pid_t pid, std::vector<std::string>& out) {
    const std::string tasks = "/proc/" + std::to_string(pid) + "/task";
    GDir* dir = g_dir_open(tasks.c_str(), 0, nullptr);
    if (!dir) return;
    while (const char* tid = g_dir_read_name(dir)) {
        const std::string kids = ReadSmall(tasks + "/" + tid + "/children");
        size_t at = 0;
        while (at < kids.size()) {
            size_t end = kids.find_first_of(" \n", at);
            if (end == std::string::npos) end = kids.size();
            const std::string child = kids.substr(at, end - at);
            at = end + 1;
            if (child.empty()) continue;
            std::string comm = ReadSmall("/proc/" + child + "/comm");
            while (!comm.empty() && comm.back() == '\n') comm.pop_back();
            out.push_back(comm);
            Below((pid_t)atoi(child.c_str()), out);
        }
    }
    g_dir_close(dir);
}

std::vector<std::string> Started() {
    std::vector<std::string> out;
    Below(getpid(), out);
    return out;
}

std::string Joined(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) out += (out.empty() ? "" : ", ") + s;
    return out.empty() ? "none" : out;
}

// The terminal's shell, as Terminal::spawnShell names it.
std::string ShellName() {
    const char* shell = g_getenv("SHELL");
    std::string s = shell && *shell ? shell : "/bin/bash";
    return s.substr(s.find_last_of('/') + 1);
}

// Started below this app: the shell, or anything of WebKit's (its two
// processes, the bwrap of its sandbox, the D-Bus proxy it starts with it).
bool AnyShellOrWebKit(const std::vector<std::string>& names) {
    const std::string shell = ShellName();
    for (const std::string& n : names)
        if (n == shell || n.rfind("WebKit", 0) == 0 || n == "bwrap" || n == "xdg-dbus-proxy")
            return true;
    return false;
}

bool Has(const std::vector<std::string>& names, const std::string& name) {
    for (const std::string& n : names)
        if (n == name) return true;
    return false;
}

guint Controllers(GtkWidget* w) {
    GListModel* list = gtk_widget_observe_controllers(w);
    const guint n = g_list_model_get_n_items(list);
    g_object_unref(list);
    return n;
}

guint gWindowControllers = 0;
int gStatus = -1;
std::string gOut;
std::vector<std::string> gLog;   // the second app's startup lines, in order

// Where the first line holding `text` is in the log, or -1.
int LogLine(const std::string& text) {
    for (size_t i = 0; i < gLog.size(); i++)
        if (gLog[i].find(text) != std::string::npos) return (int)i;
    return -1;
}

void SetEnv(const char* name, const std::string& value, std::string* old, bool* had) {
    const char* v = g_getenv(name);
    *had = v != nullptr;
    *old = v ? v : "";
    g_setenv(name, value.c_str(), TRUE);
}

void PutBack(const char* name, const std::string& old, bool had) {
    if (had) g_setenv(name, old.c_str(), TRUE);
    else g_unsetenv(name);
}

}  // namespace

MC_GUI_SUITE(startup) {
    MCGuiStep("a folder, its session of five tabs, and a server for C++", [] {
        MCGuiWriteFile(In("main.cpp"), CppLines(2000));
        const std::string big = CppLines(40000);   // about 3 MB
        for (const char* f : kBigTabs) MCGuiWriteFile(In(f), big);
        MCGuiWriteFile(In("README.md"), "# Startup\n\nA folder for the startup check.\n");
        for (int i = 0; i < 40; i++)
            MCGuiWriteFile(In("src/part" + std::to_string(i) + ".txt"), "part\n");
        for (const char* d : {"docs", "tests", "tools"})
            MCGuiWriteFile(In(std::string(d) + "/notes.txt"), "notes\n");

        TabSessions::Session s;
        for (const char* f : kBigTabs) {
            TabSessions::Tab t;
            t.path = In(f);
            s.tabs.push_back(t);
        }
        TabSessions::Tab shown;
        shown.path = In("main.cpp");
        s.tabs.insert(s.tabs.begin() + 2, shown);
        s.active = 2;
        TabSessions::Sessions all;
        TabSessions::put(all, Persistence::keyFor(Folder()), s, 1762700000.0);
        MCGuiWriteFile(ChildState() + "/minicode/sessions.json", TabSessions::encode(all));

        const std::string server = MCGuiFakeLsp("startup", "{}");
        MCGuiWriteFile(ChildSettings(), "lsp.enabled = true\nlsp.cpp = " + server + "\n");
    });
    MCGuiAsync("a second app opens it, with the startup log on", 120, [](MCGuiDone done) {
        // The second app has this process's environment, as it is now.
        std::string oldSettings, oldState, oldLog;
        bool hadSettings = false, hadState = false, hadLog = false;
        SetEnv("MINICODE_SETTINGS", ChildSettings(), &oldSettings, &hadSettings);
        SetEnv("XDG_STATE_HOME", ChildState(), &oldState, &hadState);
        SetEnv("MINICODE_STARTUP_LOG", "1", &oldLog, &hadLog);
        MCGuiRunChild("_startup", Folder(), [done](int status, std::string out) {
            gStatus = status;
            gOut = out;
            size_t at = 0;
            while (at < out.size()) {
                size_t end = out.find('\n', at);
                if (end == std::string::npos) end = out.size();
                const std::string line = out.substr(at, end - at);
                at = end + 1;
                if (line.find("minicode startup:") != std::string::npos) gLog.push_back(line);
                // The second app's notes (its numbers), for the run's log.
                else if (line.rfind("    ", 0) == 0) MCGuiNote("second app:%s", line.c_str() + 3);
            }
            done();
        });
        PutBack("MINICODE_SETTINGS", oldSettings, hadSettings);
        PutBack("XDG_STATE_HOME", oldState, hadState);
        PutBack("MINICODE_STARTUP_LOG", oldLog, hadLog);
    });
    MCGuiStep("it passed its own checks", [] {
        GCHECK(gStatus == 0);
        if (gStatus != 0) MCGuiNote("the second app (%d):\n%s", gStatus, gOut.c_str());
    });
    MCGuiStep("its log: the first frame with nothing started, the server after it", [] {
        const int frame = LogLine("first frame drawn");
        GCHECK(frame >= 0);
        if (frame < 0) return;
        MCGuiNote("%s", gLog[(size_t)frame].c_str());
        GCHECK(gLog[(size_t)frame].find("started: none)") != std::string::npos);
        const int server = LogLine("language server started: fakelsp");
        GCHECK(server > frame);
        // The panes' parts, made when the second app showed them.
        GCHECK(LogLine("terminal: made, on first show") > frame);
        GCHECK(LogLine("browser: web view made, on first show") > frame);
    });
}

// The second app (MCGuiRunChild): its window is on screen when this runs.
MC_GUI_SUITE(_startup) {
    MCGuiStep("the first frame within its bounds, the session's tab on show", [] {
        const StartupLog::FirstFrame f = StartupLog::firstFrame();
        MCGuiNote("first frame %.0f ms after main, %.0f ms of main-thread CPU (the frame itself "
                  "%.0f ms)", f.wallMs, f.cpuMs, f.paintMs);
        GCHECK(f.wallMs > 0 && f.cpuMs > 0);
        GCHECK_TIME(f.cpuMs, 1500, "main-thread CPU from main to the first frame");
        // Wall time only with room: a loaded machine ran the GUI checks'
        // wall times three to ten times over.
        GCHECK_TIME(f.wallMs, 20000, "wall time from main to the first frame");
        GCHECK(MCGuiApp().editor->tabCount() == 5);
        GCHECK_EQ_STR(MCGuiApp().editor->currentPath(), In("main.cpp"));
    });
    MCGuiStep("the hidden terminal and browser not made, nothing of theirs started", [] {
        const std::vector<std::string> started = Started();
        MCGuiNote("started below the app: %s", Joined(started).c_str());
        GCHECK(!AnyShellOrWebKit(started));
#ifdef MINICODE_ENABLE_TERMINAL
        GCHECK(MCGuiApp().terminal && !MCGuiApp().terminal->made());
        GCHECK(!MCGuiApp().layout->terminalShown());
#endif
#ifdef MINICODE_ENABLE_BROWSER
        GCHECK(MCGuiApp().browser && !MCGuiApp().browser->webView());
        GCHECK(!MCGuiApp().layout->browserShown());
#endif
    });
    MCGuiWait("the language server, after the first frame", 20, [] {
        return Has(Started(), "fakelsp") && MCGuiFakeLspReceived("startup", "textDocument/didOpen",
                                                                "main.cpp") > 0;
    });
#ifdef MINICODE_ENABLE_TERMINAL
    MCGuiStep("the terminal shown", [] {
        gWindowControllers = Controllers(GTK_WIDGET(MCGuiApp().window));
        MCGuiSetKeyWindow(nullptr);
        MCGuiKey("ctrl+shift+t");
    });
    MCGuiWait("made, with its shell", 20, [] {
        return MCGuiApp().layout->terminalShown() && MCGuiApp().terminal->made() &&
               Has(Started(), ShellName());
    });
    // Made while the window is on show, the terminal is realized as it goes
    // into the panel: its handler that puts a key controller on the window
    // (Ctrl pressed over a link, Terminal.cpp) must have run all the same.
    MCGuiStep("its watch for Ctrl on the window", [] {
        GCHECK(Controllers(GTK_WIDGET(MCGuiApp().window)) == gWindowControllers + 1);
    });
#endif
#ifdef MINICODE_ENABLE_BROWSER
    MCGuiStep("the browser shown", [] { MCGuiKey("ctrl+shift+b"); });
    MCGuiWait("made, on its home page, its web process running", 30, [] {
        GtkWidget* web = MCGuiApp().browser->webView();
        if (!web || !MCGuiApp().layout->browserShown()) return false;
        const char* uri = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(web));
        bool process = false;
        for (const std::string& n : Started())
            if (n.rfind("WebKitWebProc", 0) == 0) process = true;
        return uri && std::string(uri) == "about:blank" && process;
    });
#endif
    MCGuiStep("closed", [] { MCGuiCloseAllWindows(); });
}
