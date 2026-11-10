// FormatSuite.cpp — Format Document (`./build/minicode-guitest format
// format_on_save`, linux/PARITY.md item 2.4; VS Code's Linux Ctrl+Shift+I),
// the counterpart of the Mac's `format` suite in tests/gui/FormatSuite.mm,
// with real keys through XTest and stand-in formatters (scripts) on PATH.
//
// format: 300 changed lines spread over a 3,000-line file go in as one user
// action (one Ctrl+Z, Ctrl+Shift+Z puts them back), unsaved, the caret on
// its text and on screen; the default plan finds Google's pyink on PATH and
// gives it its options and the file; selected lines pass --line-ranges; a
// formatter that fails leaves the buffer alone and shows its complaint; so
// does one writing what is not UTF-8; one past the 10 s limit is stopped
// (SIGTERM ignored, so SIGKILL) and its process is gone; a result for text
// typed into meanwhile is dropped; formatted text says so.
//
// format_on_save: Ctrl+S formats, then writes; Save All and auto-save write
// as typed; auto-save leaves a tab whose Ctrl+S waits for its formatter;
// a Ctrl+S is never lost: another tab shown meanwhile saves it as it was.
#include "Harness.h"

#include "Editor.h"
#include "FormatRunner.h"
#include "Formatting.h"

#include <glib/gstdio.h>
#include <signal.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdlib>
#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
Formatting* fmt() { return MCGuiApp().format; }

std::string F(const std::string& name) { return MCGuiProjectFile("fmt/" + name); }
std::string bin() { return MCGuiScratch() + "/fmtbin"; }
std::string argsFile() { return MCGuiScratch() + "/fmt-args"; }
std::string pidFile() { return MCGuiScratch() + "/fmt-slow-pid"; }

void script(const std::string& path, const std::string& body) {
    MCGuiWriteFile(path, body);
    chmod(path.c_str(), 0755);
}

// The stand-ins come first on PATH while the suites run.
std::string gPath;
void standInsOnPath() {
    const char* p = g_getenv("PATH");
    gPath = p ? p : "";
    g_setenv("PATH", (bin() + ":" + gPath).c_str(), TRUE);
}
void pathBack() { g_setenv("PATH", gPath.c_str(), TRUE); }

std::string sourceText() {
    GtkTextBuffer* b = ed()->sourceBuffer();
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* t = gtk_text_buffer_get_text(b, &s, &e, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

GtkTextIter caretIter() {
    GtkTextIter it;
    GtkTextBuffer* b = ed()->sourceBuffer();
    gtk_text_buffer_get_iter_at_mark(b, &it, gtk_text_buffer_get_insert(b));
    return it;
}

bool caretOnScreen() {
    GtkTextIter it = caretIter();
    GdkRectangle at, vis;
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(ed()->textView()), &it, &at);
    gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(ed()->textView()), &vis);
    return at.y >= vis.y && at.y + at.height <= vis.y + vis.height;
}

int topLine() {
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(ed()->textView()), &vis);
    GtkTextIter it;
    gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(ed()->textView()), &it, vis.x, vis.y + 1);
    return gtk_text_iter_get_line(&it);
}

bool dirty() { return ed()->active() && ed()->active()->dirty(); }
bool editorHasKeyboard() { return gtk_widget_has_focus(ed()->textView()); }

std::string gOriginal, gExpected;
int gUserActions = 0;
gulong gActionsHandler = 0;
int gTop = -1;
int gSlowPid = 0;

void countUserActions(bool on) {
    GtkTextBuffer* b = ed()->sourceBuffer();
    if (on) {
        gUserActions = 0;
        gActionsHandler = g_signal_connect(b, "end-user-action",
                                           G_CALLBACK(+[](GtkTextBuffer*, gpointer) { gUserActions++; }),
                                           nullptr);
    } else if (gActionsHandler) {
        g_signal_handler_disconnect(b, gActionsHandler);
        gActionsHandler = 0;
    }
}

}  // namespace

MC_GUI_SUITE(format) {
    MCGuiStep("a 3,000-line file with 300 lines to change, and pyink on PATH", [] {
        gOriginal.clear();
        gExpected.clear();
        for (int i = 0; i < 3000; i++) {
            if (i % 10 == 5) {
                gOriginal += "x" + std::to_string(i) + "=" + std::to_string(i) + "\n";
                gExpected += "x" + std::to_string(i) + " = " + std::to_string(i) + "\n";
            } else {
                gOriginal += "# line " + std::to_string(i) + " stays as it is\n";
                gExpected += "# line " + std::to_string(i) + " stays as it is\n";
            }
        }
        MCGuiWriteFile(F("spaced.py"), gOriginal);
        // Reads the buffer on stdin, as every formatter here does, and
        // keeps the arguments it was given.
        script(bin() + "/pyink", "#!/bin/sh\nprintf '%s\\n' \"$@\" > '" + argsFile() +
                                     "'\nexec /usr/bin/sed -E 's/^(x[0-9]+)=([0-9]+)$/\\1 = \\2/'\n");
        standInsOnPath();
        MCGuiSetSettings("lsp.enabled = false\n");
        MCGuiOpenFileKept(F("spaced.py"));
    });
    MCGuiWait("it shows", 10, [] { return ed()->currentPath() == F("spaced.py") && ed()->isSource(); });
    MCGuiStep("the caret on line 1506, after \"x1505=\"", [] {
        GCHECK(ed()->revealLine(1506, 6, 0));
    });
    MCGuiWait("on screen, with the keyboard", 10, [] { return caretOnScreen() && editorHasKeyboard(); });
    // GTK lays the lines out in idle slices after a buffer comes on show,
    // which moves what is at the top as estimated heights become real.
    MCGuiWaitQuiet("the layout settles", 0.5, 20, [] {
        GtkAdjustment* v = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(ed()->textView()));
        return (long)gtk_adjustment_get_upper(v) * 100000 + topLine();
    });
    MCGuiStep("Ctrl+Shift+I", [] {
        gTop = topLine();
        countUserActions(true);
        MCGuiKey("ctrl+shift+i");
    });
    MCGuiWait("the formatted text", 15, [] { return sourceText() == gExpected; });
    MCGuiStep("one user action, the caret on its text and on screen, unsaved", [] {
        countUserActions(false);
        MCGuiNote("user actions: %d; top line %d, then %d", gUserActions, gTop, topLine());
        GCHECK(gUserActions == 1);
        GtkTextIter it = caretIter();
        GCHECK(gtk_text_iter_get_line(&it) == 1505);
        // After "x1505=", the caret maps into the changed line's "= "
        // (counted by non-blank characters within the run).
        const int col = gtk_text_iter_get_line_offset(&it);
        MCGuiNote("the caret's column: %d", col);
        GCHECK(col >= 5 && col <= 8);
        GCHECK(caretOnScreen());
        GCHECK(std::abs(topLine() - gTop) <= 1);
        GCHECK(dirty());
        GCHECK_EQ_STR(MCGuiReadFile(F("spaced.py")), gOriginal);
        // Google's pyink, told the file.
        const std::string args = MCGuiReadFile(argsFile());
        GCHECK(args.find("--line-length\n80\n") != std::string::npos);
        GCHECK(args.find("--stdin-filename\n" + F("spaced.py") + "\n") != std::string::npos);
        GCHECK(args.find("--line-ranges") == std::string::npos);
    });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("all of it undone at once, clean again", 10,
              [] { return sourceText() == gOriginal && !dirty(); });
    MCGuiStep("Ctrl+Shift+Z", [] { MCGuiKey("ctrl+shift+z"); });
    MCGuiWait("all of it again", 10, [] { return sourceText() == gExpected; });
    MCGuiStep("Ctrl+Shift+I on formatted text", [] { MCGuiKey("ctrl+shift+i"); });
    MCGuiWait("a note says so", 15, [] {
        return fmt()->lastNote() == "Already formatted (pyink, Google style).";
    });
    MCGuiStep("the note by the caret; Ctrl+Z", [] {
        GCHECK(fmt()->noteShown());
        GCHECK(editorHasKeyboard());   // a note never takes the keyboard
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("undone", 10, [] { return sourceText() == gOriginal; });

    // Lines 3 to 5 selected with the keyboard: only those, where the
    // formatter can.
    MCGuiStep("Ctrl+Home, Down, Down, Shift+Down three times", [] {
        MCGuiKey("ctrl+home");
        MCGuiKey("down");
        MCGuiKey("down");
        MCGuiKey("shift+down");
        MCGuiKey("shift+down");
        MCGuiKey("shift+down");
    });
    MCGuiWait("three lines selected", 10, [] {
        return gtk_text_buffer_get_has_selection(ed()->sourceBuffer());
    });
    MCGuiStep("Ctrl+Shift+I", [] {
        g_remove(argsFile().c_str());
        MCGuiKey("ctrl+shift+i");
    });
    MCGuiWait("pyink ran", 15, [] { return sourceText() == gExpected; });
    MCGuiStep("it was given lines 3 to 5", [] {
        const std::string args = MCGuiReadFile(argsFile());
        MCGuiNote("pyink's arguments: %s", args.c_str());
        GCHECK(args.find("--line-ranges=3-5\n-\n") != std::string::npos);
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("undone", 10, [] { return sourceText() == gOriginal; });

    MCGuiStep("a formatter that fails", [] {
        script(bin() + "/failing-formatter",
               "#!/bin/sh\n/bin/cat >/dev/null\necho 'spaced.py:3: cannot parse' >&2\nexit 1\n");
        MCGuiSetSettings("lsp.enabled = false\nformat.python = failing-formatter\n");
        countUserActions(true);
        MCGuiKey("ctrl+shift+i");
    });
    MCGuiWait("its note", 15, [] { return fmt()->lastNote().find("cannot parse") != std::string::npos; });
    MCGuiStep("the buffer untouched, the formatter's complaint shown", [] {
        countUserActions(false);
        MCGuiNote("the note: %s", fmt()->lastNote().c_str());
        GCHECK(fmt()->lastNote().find("failing-formatter could not format spaced.py:") == 0);
        GCHECK(fmt()->noteShown());
        GCHECK(gUserActions == 0);
        GCHECK(sourceText() == gOriginal);
        GCHECK(!dirty());
    });
    MCGuiStep("one that writes what is not UTF-8", [] {
        script(bin() + "/latin1-formatter", "#!/bin/sh\n/bin/cat >/dev/null\nprintf 'x\\377\\n'\n");
        MCGuiSetSettings("lsp.enabled = false\nformat.python = latin1-formatter\n");
        MCGuiKey("ctrl+shift+i");
    });
    MCGuiWait("its note", 15, [] { return fmt()->lastNote().find("not UTF-8") != std::string::npos; });
    MCGuiStep("the buffer untouched", [] { GCHECK(sourceText() == gOriginal); });

    MCGuiStep("one that takes a second, and a letter typed meanwhile", [] {
        script(bin() + "/slow-formatter",
               "#!/bin/sh\n/usr/bin/sleep 1\nexec /usr/bin/sed -E 's/^(x[0-9]+)=([0-9]+)$/\\1 = \\2/'\n");
        MCGuiSetSettings("lsp.enabled = false\nformat.python = slow-formatter\n");
        MCGuiKey("ctrl+home");
        MCGuiKey("ctrl+shift+i");
        MCGuiType("z");
    });
    MCGuiWait("the formatter is done", 15, [] { return FormatRunner::running() == 0; });
    MCGuiStep("its output dropped: the text as typed", [] {
        GCHECK(sourceText() == "z" + gOriginal);
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("undone", 10, [] { return sourceText() == gOriginal; });

    MCGuiStep("one past the 10 s limit, which ignores SIGTERM", [] {
        g_remove(pidFile().c_str());
        script(bin() + "/stuck-formatter", "#!/bin/sh\necho $$ > '" + pidFile() +
                                               "'\ntrap '' TERM\nexec /usr/bin/sleep 60\n");
        MCGuiSetSettings("lsp.enabled = false\nformat.python = stuck-formatter\n");
        MCGuiKey("ctrl+shift+i");
    });
    MCGuiWait("it runs", 10, [] {
        gSlowPid = atoi(MCGuiReadFile(pidFile()).c_str());
        return gSlowPid > 0;
    });
    MCGuiWait("stopped at the limit, with a note", 20, [] {
        return fmt()->lastNote().find("took longer than 10 seconds") != std::string::npos;
    });
    MCGuiStep("its process is gone; the buffer untouched", [] {
        MCGuiNote("the note: %s", fmt()->lastNote().c_str());
        GCHECK_EQ_STR(fmt()->lastNote(),
                      "stuck-formatter took longer than 10 seconds and was stopped. The file "
                      "was not changed.");
        GCHECK(kill(gSlowPid, 0) == -1 && errno == ESRCH);
        GCHECK(FormatRunner::running() == 0);
        GCHECK(sourceText() == gOriginal);
    });
    MCGuiStep("tidy up", [] {
        MCGuiSetSettings("lsp.enabled = false\n");
        if (Document* d = ed()->documentFor(F("spaced.py"))) ed()->closeTab(d);
        pathBack();
    });
}

// -------------------------------------------------------------- on save

namespace {
std::string G(const std::string& name) { return MCGuiProjectFile("fos/" + name); }
}  // namespace

MC_GUI_SUITE(format_on_save) {
    MCGuiStep("format.on-save, pyink on PATH, two files", [] {
        script(bin() + "/pyink", "#!/bin/sh\nexec /usr/bin/sed -E 's/^(x[0-9]+)=([0-9]+)$/\\1 = \\2/'\n");
        standInsOnPath();
        MCGuiWriteFile(G("a.py"), "x1=1\n");
        MCGuiWriteFile(G("b.py"), "b = 1\n");
        MCGuiSetSettings("lsp.enabled = false\nformat.on-save = true\n");
        MCGuiOpenFileKept(G("b.py"));
        MCGuiOpenFileKept(G("a.py"));
    });
    MCGuiWait("a.py on show, with the keyboard", 10, [] {
        if (ed()->currentPath() == G("a.py") && ed()->isSource() && !editorHasKeyboard())
            ed()->focusActiveDocument();
        return ed()->currentPath() == G("a.py") && ed()->isSource() && editorHasKeyboard();
    });
    MCGuiStep("type a line, Ctrl+S", [] {
        MCGuiKey("ctrl+end");
        MCGuiType("x2=2\n");
    });
    MCGuiWait("typed", 10, [] { return sourceText() == "x1=1\nx2=2\n"; });
    MCGuiStep("Ctrl+S", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("formatted, then saved", 15, [] {
        return MCGuiReadFile(G("a.py")) == "x1 = 1\nx2 = 2\n" && !dirty();
    });
    MCGuiStep("formatted in the buffer too; Save All writes as typed", [] {
        GCHECK(sourceText() == "x1 = 1\nx2 = 2\n");
        MCGuiType("x3=3\n");
    });
    MCGuiWait("typed", 10, [] { return dirty(); });
    MCGuiStep("Ctrl+Alt+S", [] { MCGuiKey("ctrl+alt+s"); });
    MCGuiWait("saved as typed", 10, [] {
        return MCGuiReadFile(G("a.py")) == "x1 = 1\nx2 = 2\nx3=3\n" && !dirty();
    });
    MCGuiPause(0.5);   // negative check: no format comes after it
    MCGuiStep("not formatted; auto-save after a pause writes as typed", [] {
        GCHECK(sourceText() == "x1 = 1\nx2 = 2\nx3=3\n");
        MCGuiSetSettings("lsp.enabled = false\nformat.on-save = true\n"
                         "editor.auto-save = after-delay\neditor.auto-save-delay = 200\n");
        MCGuiType("x4=4\n");
    });
    MCGuiWait("auto-saved as typed", 10, [] {
        return MCGuiReadFile(G("a.py")) == "x1 = 1\nx2 = 2\nx3=3\nx4=4\n";
    });
    MCGuiPause(0.5);   // negative check: no format comes after it
    MCGuiStep("not formatted; a formatter that takes two seconds, a line, Ctrl+S at once", [] {
        GCHECK(sourceText() == "x1 = 1\nx2 = 2\nx3=3\nx4=4\n");
        script(bin() + "/pyink", "#!/bin/sh\n/usr/bin/sleep 2\n"
                                 "exec /usr/bin/sed -E 's/^(x[0-9]+)=([0-9]+)$/\\1 = \\2/'\n");
        MCGuiType("x5=5\n");
        MCGuiKey("ctrl+s");
    });
    MCGuiPause(1);   // negative check: auto-save's 200 ms pass, and it leaves the tab
    MCGuiStep("auto-save left the tab whose Ctrl+S waits", [] {
        GCHECK(fmt()->saveWaits(ed()->active()));
        GCHECK(MCGuiReadFile(G("a.py")) == "x1 = 1\nx2 = 2\nx3=3\nx4=4\n");
    });
    MCGuiWait("formatted, then saved by Ctrl+S", 15, [] {
        return MCGuiReadFile(G("a.py")) == "x1 = 1\nx2 = 2\nx3 = 3\nx4 = 4\nx5 = 5\n" && !dirty();
    });
    MCGuiStep("a line, Ctrl+S, and at once another tab (Ctrl+Page Up)", [] {
        MCGuiSetSettings("lsp.enabled = false\nformat.on-save = true\n");
        MCGuiType("x6=6\n");
        MCGuiKey("ctrl+s");
        MCGuiKey("ctrl+pageup");
    });
    MCGuiWait("b.py on show, and a.py saved as it was at once", 10, [] {
        return ed()->currentPath() == G("b.py") &&
               MCGuiReadFile(G("a.py")) == "x1 = 1\nx2 = 2\nx3 = 3\nx4 = 4\nx5 = 5\nx6=6\n";
    });
    MCGuiWait("the formatter is done", 15, [] { return FormatRunner::running() == 0; });
    MCGuiStep("its output dropped: a.py stays as saved, and clean", [] {
        Document* a = ed()->documentFor(G("a.py"));
        GCHECK(a && !a->dirty());
        GCHECK(MCGuiReadFile(G("a.py")) == "x1 = 1\nx2 = 2\nx3 = 3\nx4 = 4\nx5 = 5\nx6=6\n");
        GCHECK(a && a->text() == "x1 = 1\nx2 = 2\nx3 = 3\nx4 = 4\nx5 = 5\nx6=6\n");
        GCHECK(!fmt()->saveWaits(a));
    });
    MCGuiStep("tidy up", [] {
        MCGuiSetSettings("lsp.enabled = false\n");
        for (const char* name : {"a.py", "b.py"})
            if (Document* d = ed()->documentFor(G(name))) ed()->closeTab(d);
        pathBack();
    });
}
