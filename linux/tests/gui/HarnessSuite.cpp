// HarnessSuite.cpp — the harness checking itself (`./build/minicode-guitest
// harness`): the world it gave the app is the scratch one, characters off the
// keyboard layout type, a second app in the same world runs and can be
// killed, and a picture of a widget is written. Every later suite leans on
// these, so a change to the harness shows up here first.
#include "Harness.h"

#include "Editor.h"

#ifdef MINICODE_ENABLE_BROWSER
#include "Browser.h"
#include <webkit/webkit.h>
#endif

#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <map>

static std::string notesPath() { return MCGuiProjectFile("harness.txt"); }

#ifdef MINICODE_ENABLE_BROWSER
static WebKitWebView* findWebView(GtkWidget* root) {
    if (!root) return nullptr;
    if (WEBKIT_IS_WEB_VIEW(root)) return WEBKIT_WEB_VIEW(root);
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (WebKitWebView* hit = findWebView(c)) return hit;
    return nullptr;
}

static WebKitWebView* browserWebView() {
    Browser* browser = MCGuiApp().browser;
    return browser ? findWebView(browser->widget()) : nullptr;
}

// Whether a WebKit web process runs below this one with a bwrap between, from
// /proc: WebKit starts each page's process inside bubblewrap, and with
// WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS set it starts it directly instead.
static bool webProcessInSandbox() {
    std::map<pid_t, std::vector<pid_t>> kids;
    std::map<pid_t, std::string> comm;
    GDir* proc = g_dir_open("/proc", 0, nullptr);
    if (!proc) return false;
    while (const char* name = g_dir_read_name(proc)) {
        if (!g_ascii_isdigit(name[0])) continue;
        gchar* stat = nullptr;
        if (!g_file_get_contents((std::string("/proc/") + name + "/stat").c_str(), &stat, nullptr,
                                 nullptr))
            continue;
        // pid (comm) state ppid ...; comm may hold spaces and parentheses.
        const char* open = strchr(stat, '(');
        const char* close = strrchr(stat, ')');
        int ppid = 0;
        char state = 0;
        if (open && close && close > open && sscanf(close + 1, " %c %d", &state, &ppid) == 2) {
            const pid_t pid = (pid_t)atoi(name);
            kids[ppid].push_back(pid);
            comm[pid] = std::string(open + 1, (size_t)(close - open - 1));
        }
        g_free(stat);
    }
    g_dir_close(proc);
    // Depth first from here, carrying whether a bwrap lies on the way down.
    std::vector<std::pair<pid_t, bool>> todo = {{getpid(), false}};
    while (!todo.empty()) {
        const auto [pid, underBwrap] = todo.back();
        todo.pop_back();
        for (pid_t child : kids[pid]) {
            const std::string& name = comm[child];
            // The kernel cuts names at 15 characters: "WebKitWebProces".
            if (name.rfind("WebKitWebProc", 0) == 0 && underBwrap) return true;
            todo.push_back({child, underBwrap || name == "bwrap"});
        }
    }
    return false;
}
#endif

// The second app, for the "a second app" steps below. Runs when named only.
MC_GUI_SUITE(_harness_child) {
    MCGuiStep("leaves a file behind", [] {
        GCHECK(MCGuiApp().window != nullptr);
        MCGuiWriteFile(MCGuiProjectFile("from-child.txt"), "child ran\n");
    });
}

MC_GUI_SUITE(_harness_child_killed) {
    MCGuiStep("dies at once", [] { kill(getpid(), SIGKILL); });
}

MC_GUI_SUITE(harness) {
    MCGuiStep("the app's world is the scratch one", [] {
        const std::string scratch = MCGuiScratch();
        GCHECK(g_get_home_dir() == scratch + "/home");
        GCHECK(g_get_user_config_dir() == scratch + "/config");
        GCHECK(g_getenv("WAYLAND_DISPLAY") == nullptr);
        GCHECK_EQ_STR(g_getenv("GDK_BACKEND"), "x11");
        GCHECK_EQ_STR(g_getenv("GTK_A11Y"), "none");
        // Set by dbus-run-session, never the desktop's.
        GCHECK(g_getenv("DBUS_SESSION_BUS_ADDRESS") != nullptr);
        GCHECK(gdk_display_get_name(gdk_display_get_default()) == std::string(g_getenv("DISPLAY")));
    });
    MCGuiAsync("a shell run from a check sees the same world", 20, [](MCGuiDone done) {
        MCGuiRunProcess({"/bin/sh", "-c", "printf %s \"$HOME|$XDG_RUNTIME_DIR|$GIT_CONFIG_GLOBAL\""},
                        {}, [done](int status, std::string out, std::string) {
                            GCHECK(status == 0);
                            const std::string want = MCGuiScratch() + "/home|";
                            GCHECK(out.rfind(want, 0) == 0);
                            GCHECK(out.find("|/dev/null") != std::string::npos);
                            done();
                        });
    });
    MCGuiAsync("a process's exit status is reported", 20, [](MCGuiDone done) {
        MCGuiRunProcess({"/bin/sh", "-c", "kill -9 $$"}, {},
                        [done](int status, std::string, std::string) {
                            GCHECK(status == 128 + SIGKILL);
                            done();
                        });
    });

#ifdef MINICODE_ENABLE_BROWSER
    // Every window makes a web view, and WebKit runs its page in a process
    // inside bubblewrap, which needs an unprivileged user namespace. Where
    // those are refused (Ubuntu's AppArmor restriction without its bwrap
    // profile, a container) the page's process dies as it starts and the
    // browser pane stays blank; nothing else in the window notices. A page
    // that answers JavaScript has a live process, and the process tree says
    // whether it is in the sandbox.
    MCGuiAsync("the browser pane's page runs JavaScript", 30, [](MCGuiDone done) {
        WebKitWebView* web = browserWebView();
        GCHECK(web != nullptr);
        if (!web) {
            done();
            return;
        }
        // The checks' home page (MINICODE_BROWSER_HOME), so no window
        // opening in a check reaches the network.
        const char* uri = webkit_web_view_get_uri(web);
        const std::string page = uri ? uri : "";
        GCHECK_EQ_STR(page, "about:blank");
        webkit_web_view_evaluate_javascript(
            web, "6 * 7", -1, nullptr, nullptr, nullptr,
            [](GObject* src, GAsyncResult* res, gpointer data) {
                auto* finished = static_cast<MCGuiDone*>(data);
                GError* error = nullptr;
                JSCValue* value =
                    webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(src), res, &error);
                GCHECK(value && jsc_value_is_number(value) && jsc_value_to_int32(value) == 42);
                if (error) MCGuiNote("WebKit: %s", error->message);
                g_clear_error(&error);
                if (value) g_object_unref(value);
                (*finished)();
                delete finished;
            },
            new MCGuiDone(done));
    });
    MCGuiStep("the page's process is in WebKit's sandbox", [] {
        GCHECK(g_getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS") == nullptr);
        GCHECK(webProcessInSandbox());
    });
#endif

    MCGuiStep("a file to type into", [] {
        MCGuiWriteFile(notesPath(), "");
        MCGuiOpenFile(notesPath());
    });
    MCGuiWait("it opened", 10, [] { return MCGuiApp().editor->currentPath() == notesPath(); });
    MCGuiStep("a click in the text gives it the keyboard", [] {
        MCGuiClick(MCGuiApp().editor->textView());
    });
    MCGuiWait("the text view has the keyboard", 5, [] {
        return gtk_widget_has_focus(MCGuiApp().editor->textView());
    });
    MCGuiStep("types characters off the layout, and Shift's and plain ones", [] {
        MCGuiType("caf\xC3\xA9 \xE2\x86\x92 \xCE\xBB ?!\n");
    });
    MCGuiWait("they arrive whole and in order", 10, [] {
        return MCGuiEditorText() == "caf\xC3\xA9 \xE2\x86\x92 \xCE\xBB ?!\n";
    });
    MCGuiStep("undo, through Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    // GTK undoes a run of typing a piece at a time, so only part of it goes.
    MCGuiWait("the last of the typing goes", 10, [] {
        const std::string now = MCGuiEditorText();
        return now.size() < strlen("caf\xC3\xA9 \xE2\x86\x92 \xCE\xBB ?!\n") &&
               std::string("caf\xC3\xA9 \xE2\x86\x92 \xCE\xBB ?!\n").rfind(now, 0) == 0;
    });
    MCGuiStep("a picture of the window", [] { MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "harness"); });
    MCGuiStep("saved, so the next open asks nothing", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("saved", 10, [] { return !MCGuiApp().editor->dirty(); });

    MCGuiStep("a timing bound reads this thread's CPU", [] {
        const double t0 = MCGuiThreadCpuMs();
        (void)MCGuiEditorText();
        GCHECK_TIME(MCGuiThreadCpuMs() - t0, 1000, "reading the buffer");
    });

    MCGuiStep("a file long enough to scroll", [] { MCGuiOpenFile(MCGuiProjectFile("big.txt")); });
    MCGuiWait("it shows", 10, [] {
        return MCGuiEditorText().rfind("line 1 of big.txt", 0) == 0;
    });
    MCGuiStep("the wheel, on the text", [] {
        MCGuiClick(MCGuiApp().editor->textView());
        MCGuiScroll(MCGuiApp().editor->textView(), 5);
    });
    MCGuiWait("the text scrolls", 10, [] {
        GtkAdjustment* v = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(MCGuiApp().editor->textView()));
        return gtk_adjustment_get_value(v) > 0;
    });
    MCGuiStep("back to the top", [] {
        GtkAdjustment* v = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(MCGuiApp().editor->textView()));
        gtk_adjustment_set_value(v, 0);
    });
    MCGuiWait("the first line is back on screen", 10, [] {
        GtkAdjustment* v = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(MCGuiApp().editor->textView()));
        return gtk_adjustment_get_value(v) == 0;
    });
    MCGuiStep("two quick clicks on the word \"line\"", [] {
        GtkWidget* view = MCGuiApp().editor->textView();
        GtkTextIter iter;
        GdkRectangle where;
        gtk_text_buffer_get_start_iter(MCGuiApp().editor->buffer(), &iter);
        gtk_text_view_get_iter_location(GTK_TEXT_VIEW(view), &iter, &where);
        int wx = 0, wy = 0;
        gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(view), GTK_TEXT_WINDOW_WIDGET,
                                              where.x + 12, where.y + where.height / 2, &wx, &wy);
        MCGuiDoubleClick(view, wx, wy);
    });
    MCGuiWait("the word is selected", 10, [] {
        GtkTextIter a, b;
        gtk_text_buffer_get_selection_bounds(MCGuiApp().editor->buffer(), &a, &b);
        gchar* text = gtk_text_buffer_get_text(MCGuiApp().editor->buffer(), &a, &b, TRUE);
        const std::string got = text;
        g_free(text);
        return got == "line";
    });
    MCGuiStep("a click elsewhere drops the selection", [] {
        MCGuiClick(MCGuiApp().editor->textView(), 300, 100);
    });
    MCGuiWait("nothing is selected", 10, [] {
        return !gtk_text_buffer_get_has_selection(MCGuiApp().editor->buffer());
    });
    MCGuiStep("a drag along a line", [] { MCGuiDrag(MCGuiApp().editor->textView(), 60, 60, 180, 60); });
    MCGuiWait("selects part of that line", 10, [] {
        GtkTextIter a, b;
        if (!gtk_text_buffer_get_selection_bounds(MCGuiApp().editor->buffer(), &a, &b)) return false;
        return gtk_text_iter_get_line(&a) == gtk_text_iter_get_line(&b) &&
               gtk_text_iter_get_offset(&b) - gtk_text_iter_get_offset(&a) > 4;
    });

    MCGuiStep("an edit, then a question about it", [] {
        MCGuiType("x");
    });
    MCGuiWait("the edit arrives", 10, [] { return MCGuiApp().editor->dirty(); });
    MCGuiStep("Close Tab asks about the unsaved edit", [] { MCGuiKey("ctrl+w"); });
    MCGuiWait("the question is up, a window of its own", 10, [] {
        return MCGuiTopWindow() != MCGuiApp().window &&
               MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), "Cancel") != nullptr;
    });
    MCGuiStep("Cancel, as a real click on its button", [] {
        MCGuiClick(MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), "Cancel"));
    });
    MCGuiWait("the question closes", 10, [] { return MCGuiTopWindow() == MCGuiApp().window; });
    MCGuiStep("the edit is still there", [] {
        GCHECK(MCGuiApp().editor->dirty());
        GCHECK(MCGuiApp().editor->currentPath() == MCGuiProjectFile("big.txt"));
    });
    MCGuiStep("a question answered from the keyboard", [] { MCGuiKey("ctrl+w"); });
    MCGuiWait("up again", 10, [] { return MCGuiTopWindow() != MCGuiApp().window; });
    MCGuiStep("Escape, in the question's own window", [] {
        MCGuiSetKeyWindow(MCGuiTopWindow());
        MCGuiKey("escape");
    });
    MCGuiWait("it closes", 10, [] { return MCGuiTopWindow() == MCGuiApp().window; });
    MCGuiStep("the keyboard back in the app's window", [] { MCGuiSetKeyWindow(nullptr); });

    MCGuiAsync("a second app, in the same world", 60, [](MCGuiDone done) {
        MCGuiRunChild("_harness_child", MCGuiProject(), [done](int status, std::string out) {
            GCHECK(status == 0);
            if (status != 0) MCGuiNote("%s", out.c_str());
            GCHECK_EQ_STR(MCGuiReadFile(MCGuiProjectFile("from-child.txt")), "child ran\n");
            done();
        });
    });
    MCGuiAsync("a real SIGKILL in a second app", 60, [](MCGuiDone done) {
        MCGuiRunChild("_harness_child_killed", MCGuiProject(), [done](int status, std::string out) {
            GCHECK(status != 0);
            if (status == 0) MCGuiNote("%s", out.c_str());
            done();
        });
    });
    MCGuiStep("this app still has the keyboard's window", [] {
        GCHECK(MCGuiApp().window != nullptr);
        MCGuiFocusKeyWindow();
    });
    MCGuiWait("the window is active again", 10, [] { return gtk_window_is_active(MCGuiApp().window); });
}
