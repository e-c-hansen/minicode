// DemoSuite.cpp — the GTK port's demo scenes, recorded for the reference's
// Linux GIFs (docs/demos/linux) by .github/workflows/demos.yml, the
// counterpart of the Mac's mac/src/Demo.mm.
//
//   MC_DEMO_OUT=<folder> MC_DEMO_SOURCE=<repo>/demo ./build/minicode-guitest _demo_tour
//
// Each scene is a suite whose name starts with "_", so `meson test --suite
// gui` never runs one. It replaces the scratch project with a copy of the
// repo's demo/ folder made into a small git repository, then plays its steps
// with the harness's real X input (XTest), while ffmpeg records the window's
// corner of the private Xvfb (no window manager, so the window sits at the
// top left) into <scene>.mkv, without the X pointer. The workflow turns that
// into a GIF. Nothing is recorded without MC_DEMO_OUT, so a scene can be
// played and watched through its checks alone.
#include "Harness.h"

#include <signal.h>
#include <sys/wait.h>

#include "Editor.h"
#include "FileTree.h"
#include "GitPanel.h"
#include "Terminal.h"

namespace {

GPid gRecorder = 0;

Editor* ed() { return MCGuiApp().editor; }
GtkWidget* win() { return GTK_WIDGET(MCGuiApp().window); }

std::string envOr(const char* name, const std::string& otherwise = "") {
    const char* v = getenv(name);
    return v && *v ? v : otherwise;
}

// The demo folder copied over the scratch project, committed in a few
// steps with fixed dates, as scripts/demos.sh does for the Mac.
void prepareProject() {
    MCGuiAsync("the demo folder in the project", 60, [](MCGuiDone done) {
        const std::string source = envOr("MC_DEMO_SOURCE", MCGuiBuildDir() + "/../../demo");
        const std::string script =
            "set -e; cd \"$1\"; rm -rf ./* ./.git; cp -R \"$2\"/. .;"
            "export GIT_AUTHOR_NAME=MiniCode GIT_AUTHOR_EMAIL=demo@example.com;"
            "export GIT_COMMITTER_NAME=MiniCode GIT_COMMITTER_EMAIL=demo@example.com;"
            "n=0; c() { n=$((n+1)); export GIT_AUTHOR_DATE=2026-09-0${n}T10:00:00"
            " GIT_COMMITTER_DATE=2026-09-0${n}T10:00:00; m=$1; shift; git add -A \"$@\";"
            " git commit -q -m \"$m\"; };"
            "git init -q -b main;"
            "c 'Add a Python sample' hello.py; c 'Add a C++ sample' sample.cpp;"
            "c 'Write the demo README' README.md; c 'Add field notes in LaTeX' paper;"
            "c 'Add a small vector type for the LSP demo' vec; c 'Add the app icon' images;"
            "c 'Add a table of cities' data;"
            // A short prompt for the terminal scene: the folder, not the
            // runner's host name and the scratch path.
            "printf '%s\\n' \"PS1='\\\\W \\\\$ '\" > \"$HOME/.bashrc\";"
            "printf '%s\\n' '. ~/.bashrc' > \"$HOME/.bash_profile\"";
        MCGuiRunProcess({"sh", "-c", script, "sh", MCGuiProject(), source}, {},
                        [done](int status, std::string, std::string err) {
                            GCHECK(status == 0);
                            if (status) MCGuiNote("preparing the demo folder: %s", err.c_str());
                            done();
                        });
    });
    MCGuiWait("the tree lists the demo folder", 15, [] {
        return MCGuiFindLabel(win(), "hello.py") && MCGuiFindLabel(win(), "vec");
    });
}

void startRecording(const std::string& scene) {
    MCGuiStep("start recording", [scene] {
        const std::string out = envOr("MC_DEMO_OUT");
        if (out.empty()) {
            MCGuiNote("MC_DEMO_OUT is not set: playing %s without recording", scene.c_str());
            return;
        }
        g_mkdir_with_parents(out.c_str(), 0755);
        const int w = gtk_widget_get_width(win()) & ~1, h = gtk_widget_get_height(win()) & ~1;
        const std::string size = std::to_string(w) + "x" + std::to_string(h);
        const std::string input = envOr("DISPLAY", ":0") + "+0,0";
        const std::string file = out + "/" + scene + ".mkv";
        std::vector<std::string> args = {"ffmpeg", "-y", "-loglevel", "error", "-f", "x11grab",
                                         "-draw_mouse", "0", "-framerate", "10", "-video_size",
                                         size, "-i", input, "-c:v", "libx264rgb", "-preset",
                                         "ultrafast", "-crf", "0", file};
        std::vector<char*> argv;
        for (std::string& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        GError* error = nullptr;
        if (!g_spawn_async(nullptr, argv.data(), nullptr,
                           GSpawnFlags(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD), nullptr,
                           nullptr, &gRecorder, &error)) {
            MCGuiNote("ffmpeg did not start: %s", error ? error->message : "?");
            GCHECK(false);
            g_clear_error(&error);
            gRecorder = 0;
        }
    });
    MCGuiPause(0.8);   // time is the condition: ffmpeg's first frames
}

void stopRecording(const std::string& scene) {
    MCGuiPause(1.0);   // time is the condition: the last frame stays a moment
    MCGuiStep("stop recording", [scene] {
        MCGuiSnapshot(win(), "demo-" + scene);
        if (!gRecorder) return;
        kill(gRecorder, SIGINT);
        int status = 0;
        while (waitpid(gRecorder, &status, 0) < 0 && errno == EINTR) {}
        g_spawn_close_pid(gRecorder);
        gRecorder = 0;
    });
}

// A label in the tree, clicked: a folder opens, a file shows.
void clickTree(const std::string& name) {
    MCGuiWait("the tree shows " + name, 10, [name] {
        return MCGuiFindLabel(MCGuiApp().tree->widget(), name) != nullptr;
    });
    MCGuiStep("click " + name, [name] { MCGuiClick(MCGuiFindLabel(MCGuiApp().tree->widget(), name)); });
    MCGuiPause(0.6);   // time is the condition: a person looks
}

void openFile(const std::string& relative) {
    std::string rest = relative;
    for (size_t slash; (slash = rest.find('/')) != std::string::npos;) {
        clickTree(rest.substr(0, slash));
        rest = rest.substr(slash + 1);
    }
    clickTree(rest);
    MCGuiWait("the editor shows " + relative, 10, [relative] {
        return ed()->currentPath() == MCGuiProjectFile(relative);
    });
}

// Where `needle` is in the editor's text view, in its own coordinates.
bool textPoint(const std::string& needle, bool atEnd, double* x, double* y) {
    GtkTextView* tv = GTK_TEXT_VIEW(ed()->textView());
    GtkTextBuffer* b = gtk_text_view_get_buffer(tv);
    GtkTextIter start, ms, me;
    gtk_text_buffer_get_start_iter(b, &start);
    if (!gtk_text_iter_forward_search(&start, needle.c_str(), GTK_TEXT_SEARCH_TEXT_ONLY, &ms, &me,
                                      nullptr))
        return false;
    GdkRectangle r;
    gtk_text_view_get_iter_location(tv, atEnd ? &me : &ms, &r);
    int wx = 0, wy = 0;
    gtk_text_view_buffer_to_window_coords(tv, GTK_TEXT_WINDOW_WIDGET, r.x, r.y + r.height / 2,
                                          &wx, &wy);
    *x = wx + (atEnd ? 1 : 3);
    *y = wy;
    return true;
}

void clickText(const std::string& needle, bool atEnd) {
    MCGuiStep("click at " + needle, [needle, atEnd] {
        double x = 0, y = 0;
        GCHECK(textPoint(needle, atEnd, &x, &y));
        MCGuiClick(ed()->textView(), x, y);
    });
    MCGuiPause(0.3);   // time is the condition: the caret lands
}

void key(const std::string& chord, double after = 0.8) {
    MCGuiStep("press " + chord, [chord] { MCGuiKey(chord); });
    MCGuiPause(after);   // time is the condition: what the key does, on show
}

// Typed at about a person's pace, a few characters a step.
void type(const std::string& text, double after = 0.4) {
    for (size_t i = 0; i < text.size();) {
        size_t n = 1;
        while (i + n < text.size() && n < 3 && (unsigned char)text[i + n] >= 0x80) n++;
        const std::string piece = text.substr(i, n);
        MCGuiStep("type", [piece] { MCGuiType(piece); });
        MCGuiPause(piece == "\n" ? 0.25 : 0.05);   // time is the condition: typing speed
        i += n;
    }
    MCGuiPause(after);
}

void palette(const std::string& typed) {
    key("ctrl+shift+p", 0.5);
    type(typed, 0.7);
    key("enter", 1.6);
}

Terminal* term() { return MCGuiApp().terminal; }

void openTerminal() {
    key("ctrl+`", 0.2);
    MCGuiWait("the terminal is made", 15, [] { return term() && term()->made(); });
    MCGuiPause(2.0);   // time is the condition: the shell's first prompt
}

void run(const std::string& command, double after) {
    type(command + "\n", 0.2);
    MCGuiWait("the command ends", 20, [] { return term() && !term()->busy(); });
    MCGuiPause(after);   // time is the condition: a person reads the output
}

}  // namespace

MC_GUI_SUITE(_demo_tour) {
    prepareProject();
    startRecording("tour");
    openFile("README.md");
    MCGuiPause(1.2);
    key("ctrl+shift+v", 1.6);
    key("ctrl+shift+v", 1.2);
    openFile("hello.py");
    clickText("c.area():.2f}\")", true);
    type("\n    print(\"Diameter:\", 2 * c.radius)");
    key("ctrl+s", 1.0);
    key("ctrl+p", 0.5);
    type("vec", 0.8);
    key("enter", 1.8);
    stopRecording("tour");
}

MC_GUI_SUITE(_demo_terminal) {
    prepareProject();
    startRecording("terminal");
    openFile("hello.py");
    openTerminal();
    run("ls", 1.0);
    run("git log --oneline", 1.4);
    run("python3 hello.py", 1.6);
    stopRecording("terminal");
}

MC_GUI_SUITE(_demo_markdown) {
    prepareProject();
    startRecording("markdown");
    openFile("README.md");
    MCGuiPause(1.4);
    key("ctrl+shift+v", 1.0);
    key("ctrl+alt+v", 1.6);
    clickText("Add a perimeter to the circle", true);
    type("\nTry the page beside the source", 2.0);
    stopRecording("markdown");
}

MC_GUI_SUITE(_demo_git) {
    prepareProject();
    startRecording("git");
    openFile("hello.py");
    clickText("# A sample Python file", true);
    type(", with a circle");
    key("ctrl+s", 1.8);
    key("ctrl+shift+g", 0.2);
    MCGuiWait("the panel lists hello.py", 15, [] {
        GitPanel* g = MCGuiApp().git;
        if (!g) return false;
        for (const std::string& r : g->rowDescriptions())
            if (r.find("hello.py") != std::string::npos) return true;
        return false;
    });
    MCGuiPause(1.0);   // time is the condition: a person looks
    MCGuiStep("click hello.py in the panel", [] {
        // The rows are drawn, not labels: the row's own drawing area.
        GitPanel* g = MCGuiApp().git;
        const std::vector<std::string> rows = g->rowDescriptions();
        for (size_t i = 0; i < rows.size(); i++)
            if (rows[i].find("hello.py") != std::string::npos) {
                MCGuiClick(g->rowWidget((int)i));
                return;
            }
        GCHECK(false);
    });
    MCGuiPause(1.6);   // time is the condition: the diff, on show
    key("space", 1.4);
    MCGuiStep("click the message box", [] { MCGuiClick(MCGuiApp().git->messageView()); });
    type("Say what the sample shows");
    key("ctrl+enter", 2.4);
    stopRecording("git");
}

MC_GUI_SUITE(_demo_files) {
    prepareProject();
    startRecording("files");
    openFile("images/icon.png");
    MCGuiPause(2.0);
    openFile("paper/notes.pdf");
    MCGuiPause(2.4);
    openFile("data/cities.csv");
    MCGuiPause(2.4);
    stopRecording("files");
}

MC_GUI_SUITE(_demo_settings) {
    prepareProject();
    startRecording("settings");
    openFile("sample.cpp");
    // The palette here lists no themes; the settings file's theme line,
    // written and saved, as a person does it.
    key("ctrl+,", 1.2);
    key("ctrl+End", 0.3);
    type("\ntheme = light-modern", 0.3);
    key("ctrl+s", 2.0);
    key("ctrl+shift+k", 0.3);
    type("theme = solarized-dark", 0.3);
    key("ctrl+s", 2.0);
    key("ctrl+shift+k", 0.3);
    key("ctrl+s", 1.6);
    stopRecording("settings");
}
