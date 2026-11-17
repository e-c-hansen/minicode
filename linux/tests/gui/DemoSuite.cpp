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
//
// Captions (the Mac's key:caption: and caption:): a scene that calls
// captionsOn() has a pill drawn inside the window, over its content, for the
// scene's duration (a GtkOverlay child added here, never part of the app):
// each key chord pressed through key() shows its keys in Linux's names
// ("Ctrl+Shift+P") with an optional label saying what the step is for, and
// label() shows a label alone. The caption stays until the next one.
// MC_DEMO_STILLS=<folder> also saves a PNG of the harness's own Xvfb at each
// still("name") step, to look at a scene without ffmpeg.
#include "Harness.h"

#include <signal.h>
#include <algorithm>
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

// ------------------------------------------------------------ captions
GtkWidget* gCaption = nullptr;       // the pill, a child of the window's overlay
GtkWidget* gCaptionKeys = nullptr;
GtkWidget* gCaptionText = nullptr;
GtkCssProvider* gCaptionCss = nullptr;
bool gCaptions = false;
bool gCaptionTop = false;   // at the top, for a scene whose popovers fill the middle

// "ctrl+shift+p" as "Ctrl+Shift+P"; "enter" as "Enter".
std::string chordName(const std::string& chord) {
    std::string out;
    bool start = true;
    for (char c : chord) {
        out += start ? (char)toupper((unsigned char)c) : c;
        start = c == '+';
    }
    return out;
}

// The window's outermost GtkOverlay (the one over the panes, above the
// status bar), found from the window down.
GtkWidget* findOverlay(GtkWidget* root) {
    if (!root) return nullptr;
    if (GTK_IS_OVERLAY(root)) return root;
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = findOverlay(c)) return hit;
    return nullptr;
}

void showCaption(const std::string& keys, const std::string& text) {
    if (!gCaptions) return;
    GtkWidget* overlay = findOverlay(win());
    if (!overlay) return;
    if (!gCaption) {
        gCaptionCss = gtk_css_provider_new();
        gtk_css_provider_load_from_string(gCaptionCss,
            ".demo-caption { background: #101010; border-radius: 12px;"
            " padding: 8px 18px; border: 1px solid #5a5a5a; }"
            ".demo-keys { color: #ffffff; font-size: 22px; font-weight: bold;"
            " font-family: monospace; background: #3c3c3c;"
            " border-radius: 7px; padding: 2px 12px; }"
            ".demo-text { color: #f0f0f0; font-size: 21px; }");
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gtk_style_context_add_provider_for_display(gdk_display_get_default(),
            GTK_STYLE_PROVIDER(gCaptionCss), GTK_STYLE_PROVIDER_PRIORITY_USER + 10);
G_GNUC_END_IGNORE_DEPRECATIONS
        gCaption = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
        gtk_widget_add_css_class(gCaption, "demo-caption");
        gtk_widget_set_halign(gCaption, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(gCaption, gCaptionTop ? GTK_ALIGN_START : GTK_ALIGN_END);
        if (gCaptionTop) gtk_widget_set_margin_top(gCaption, 64);
        else gtk_widget_set_margin_bottom(gCaption, 40);
        gtk_widget_set_can_target(gCaption, FALSE);   // never takes a click or the pointer
        gtk_widget_set_can_focus(gCaption, FALSE);
        gCaptionKeys = gtk_label_new("");
        gtk_widget_add_css_class(gCaptionKeys, "demo-keys");
        gCaptionText = gtk_label_new("");
        gtk_widget_add_css_class(gCaptionText, "demo-text");
        gtk_box_append(GTK_BOX(gCaption), gCaptionKeys);
        gtk_box_append(GTK_BOX(gCaption), gCaptionText);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), gCaption);
    }
    gtk_label_set_text(GTK_LABEL(gCaptionKeys), keys.c_str());
    gtk_widget_set_visible(gCaptionKeys, !keys.empty());
    gtk_label_set_text(GTK_LABEL(gCaptionText), text.c_str());
    gtk_widget_set_visible(gCaptionText, !text.empty());
    gtk_widget_set_visible(gCaption, !keys.empty() || !text.empty());
}

void removeCaption() {
    if (gCaption) {
        GtkWidget* overlay = gtk_widget_get_parent(gCaption);
        if (GTK_IS_OVERLAY(overlay)) gtk_overlay_remove_overlay(GTK_OVERLAY(overlay), gCaption);
        gCaption = gCaptionKeys = gCaptionText = nullptr;
    }
    if (gCaptionCss) {
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gtk_style_context_remove_provider_for_display(gdk_display_get_default(),
                                                      GTK_STYLE_PROVIDER(gCaptionCss));
G_GNUC_END_IGNORE_DEPRECATIONS
        g_object_unref(gCaptionCss);
        gCaptionCss = nullptr;
    }
}

// Turns the scene's captions on (at queueing, so the pauses know); the pill
// goes with the scene (stopRecording).
void captionsOn(bool top = false) {
    gCaptions = true;
    gCaptionTop = top;
}

// A step's label alone, held until the next caption, for about `hold` seconds.
void label(const std::string& text, double hold = 1.6) {
    MCGuiStep("label: " + text, [text] { showCaption("", text); });
    if (hold > 0) MCGuiPause(hold);   // time is the condition: a person reads it
}

// A PNG of the harness's own Xvfb, cropped to the window, when
// MC_DEMO_STILLS names a folder.
void still(const std::string& name) {
    MCGuiStep("still " + name, [name] {
        const std::string dir = envOr("MC_DEMO_STILLS");
        const std::string display = envOr("DISPLAY");
        if (dir.empty() || display.empty()) return;
        g_mkdir_with_parents(dir.c_str(), 0755);
        const std::string geometry = std::to_string(gtk_widget_get_width(win())) + "x" +
                                     std::to_string(gtk_widget_get_height(win())) + "+0+0";
        const std::string file = dir + "/" + name + ".png";
        gchar* out = nullptr;
        gint status = 0;
        gchar* argv[] = {(gchar*)"import", (gchar*)"-display", (gchar*)display.c_str(),
                         (gchar*)"-window", (gchar*)"root", (gchar*)"-crop",
                         (gchar*)geometry.c_str(), (gchar*)"+repage", (gchar*)file.c_str(), nullptr};
        g_spawn_sync(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, &out, nullptr,
                     &status, nullptr);
        g_free(out);
    });
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
        removeCaption();
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

// A key chord. With captions on, its keys show beside `caption` (what the
// step is for) and stay up long enough to read.
void key(const std::string& chord, double after = 0.8, const std::string& caption = "") {
    MCGuiStep("press " + chord, [chord, caption] {
        showCaption(chordName(chord), caption);
        MCGuiKey(chord);
    });
    MCGuiPause(gCaptions ? std::max(after, caption.empty() ? 0.9 : 1.5) : after);   // time is the condition: what the key does, on show
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
    captionsOn();
    startRecording("tour");
    label("Open a Markdown file from the tree", 0.2);
    openFile("README.md");
    MCGuiPause(1.2);
    still("tour-1-readme");
    key("ctrl+shift+v", 1.6, "Show the rendered page");
    still("tour-2-preview");
    key("ctrl+shift+v", 1.2, "Back to the source");
    label("Open a Python file and add a line", 0.2);
    openFile("hello.py");
    clickText("c.area():.2f}\")", true);
    // Typed as a person types it: Return indents the line and ( and " close
    // themselves, so no spaces here (I1: four more made eight).
    type("\nprint(\"Diameter:\", 2 * c.radius)");
    still("tour-3-typed");
    key("ctrl+s", 1.0, "Save the file");
    key("ctrl+p", 0.5, "Go to a file by name");
    type("vec", 0.8);
    still("tour-4-quick-open");
    key("enter", 1.8, "Open it");
    still("tour-5-vec");
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
    captionsOn();
    startRecording("markdown");
    label("Open a Markdown file from the tree", 0.2);
    openFile("README.md");
    MCGuiPause(1.4);
    still("markdown-1-source");
    key("ctrl+shift+v", 1.0, "Show the rendered page");
    still("markdown-2-page");
    key("ctrl+alt+v", 1.6, "Show the page beside the source");
    still("markdown-3-side");
    clickText("Add a perimeter to the circle", true);
    label("Edit the source and the page follows", 0.2);
    type("\nTry the page beside the source", 2.0);
    still("markdown-4-typed");
    stopRecording("markdown");
}

MC_GUI_SUITE(_demo_git) {
    prepareProject();
    captionsOn();
    startRecording("git");
    label("Open a file and make a change", 0.2);
    openFile("hello.py");
    clickText("# A sample Python file", true);
    type(", with a circle");
    key("ctrl+s", 1.8, "Save the file");
    still("git-1-saved");
    key("ctrl+shift+g", 0.2, "Open Source Control");
    MCGuiWait("the panel lists hello.py", 15, [] {
        GitPanel* g = MCGuiApp().git;
        if (!g) return false;
        for (const std::string& r : g->rowDescriptions())
            if (r.find("hello.py") != std::string::npos) return true;
        return false;
    });
    MCGuiPause(1.0);   // time is the condition: a person looks
    still("git-2-panel");
    label("Click a changed file to see its diff", 0.2);
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
    still("git-3-diff");
    key("space", 1.4, "Stage the file");
    still("git-4-staged");
    label("Write a commit message", 0.2);
    MCGuiStep("click the message box", [] { MCGuiClick(MCGuiApp().git->messageView()); });
    type("Say what the sample shows");
    key("ctrl+enter", 2.4, "Commit");
    still("git-5-committed");
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

// The settings scene's helpers: the color chooser's popover and what is in it.
GtkWidget* pickerPopover() { return ed()->pickerPopover(); }

GtkWidget* findIn(GtkWidget* root, bool (*is)(GtkWidget*)) {
    if (!root) return nullptr;
    if (is(root)) return root;
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = findIn(c, is)) return hit;
    return nullptr;
}
bool isEntryShown(GtkWidget* w) { return GTK_IS_ENTRY(w) && gtk_widget_get_mapped(w); }

// A mapped popover menu under the window that shows `text`.
GtkWidget* menuShowing(GtkWidget* root, const std::string& text) {
    if (!root || !gtk_widget_get_mapped(root)) return nullptr;
    if (GTK_IS_POPOVER_MENU(root) && MCGuiFindLabel(root, text)) return root;
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = menuShowing(c, text)) return hit;
    return nullptr;
}

MC_GUI_SUITE(_demo_settings) {
    prepareProject();
    captionsOn(true);
    startRecording("settings");
    openFile("sample.cpp");
    still("settings-1-start");
    label("Right-click the file tree", 0.2);
    MCGuiStep("right click on the tree", [] {
        MCGuiClick(MCGuiFindLabel(MCGuiApp().tree->widget(), "vec"), -1, -1, 3);
    });
    MCGuiWait("the tree's menu", 10, [] {
        return menuShowing(win(), "Adjust Settings…") != nullptr;
    });
    MCGuiPause(1.2);   // time is the condition: a person reads the menu
    still("settings-2-menu");
    label("Choose Adjust Settings", 0.2);
    MCGuiStep("choose Adjust Settings…", [] {
        GtkWidget* menu = menuShowing(win(), "Adjust Settings…");
        GCHECK(menu != nullptr);
        if (menu) MCGuiClick(MCGuiFindLabel(menu, "Adjust Settings…"));
    });
    MCGuiWait("the settings file at the tree's keys", 15, [] {
        double x, y;
        return ed()->active() && ed()->active()->isSettingsFile() && ed()->isSource() &&
               textPoint("sidebar.background = #181818", true, &x, &y);
    });
    MCGuiPause(0.6);   // time is the condition: the lines' tint
    label("minicode opens the settings at the keys that color the tree", 2.2);
    still("settings-3-file");
    label("Click the color's swatch", 0.2);
    MCGuiStep("click the sidebar color's swatch", [] {
        double x = 0, y = 0;
        // On its last character, where a swatch ends.
        GCHECK(textPoint("sidebar.background = #181818", true, &x, &y));
        MCGuiClick(ed()->textView(), x - 4, y);
    });
    MCGuiWait("the color chooser", 10, [] {
        GtkWidget* p = pickerPopover();
        return p && gtk_widget_get_mapped(p) && findIn(p, isEntryShown);
    });
    MCGuiPause(1.2);   // time is the condition: a person looks at the picker
    still("settings-4-picker");
    label("Pick a new color", 0.2);
    MCGuiStep("click the hex field", [] { MCGuiClick(findIn(pickerPopover(), isEntryShown)); });
    MCGuiPause(0.4);   // time is the condition: the field takes the keyboard
    MCGuiStep("a dark blue", [] {
        MCGuiKey("ctrl+a");
        MCGuiType("#1B3A5C");
    });
    MCGuiPause(1.0);   // time is the condition: the hex shows
    still("settings-5-typed");
    MCGuiStep("Return", [] { MCGuiType("\n"); });
    MCGuiPause(2.2);   // time is the condition: the sidebar recolors, the file saves
    still("settings-6-recolored");
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiPause(0.8);   // time is the condition: the chooser closes
    label("The sidebar has the new color, and the file is saved", 2.4);
    still("settings-7-done");
    stopRecording("settings");
}
