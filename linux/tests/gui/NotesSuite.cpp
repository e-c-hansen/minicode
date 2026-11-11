// NotesSuite.cpp — notes and tours (`./build/minicode-guitest notes`,
// linux/PARITY.md item 3.4; the Mac's AgentNotes). The client is the real
// build/minicode, run as a subprocess the main loop keeps turning for. What
// is checked:
//   - `open --note` puts the file in the agent's preview tab, highlights the
//     lines, shows the box with its text and label, moves the caret there,
//     and leaves the file's text and undo alone;
//   - Escape and the x close it; an edit on its lines closes it and undo
//     does not bring it back; an edit above moves it along; an edit makes
//     the preview tab a kept one; the next such open replaces the preview;
//   - a note sent while the user types in the editor waits with its tab,
//     placed when the tab is shown, the caret of the user's tab unmoved;
//   - a tour steps forward and back with the real keys, shows "n of m",
//     and the keys do nothing outside a tour;
//   - the text is cleaned (control and bidi characters dropped, cut to 500).
#include "Harness.h"

#include "AgentNotes.h"
#include "Document.h"
#include "Editor.h"
#include "FileTree.h"

#include <gio/gio.h>
#include <glib/gstdio.h>

#include <memory>
#include <string>
#include <vector>

namespace {

std::string F(const std::string& name) { return MCGuiProjectFile("notes/" + name); }
Editor* ed() { return MCGuiApp().editor; }
AgentNotes* notes() { return ed()->notes(); }

// One client run, finished when `done`.
struct Run {
    bool done = false;
    int exitCode = -1;
    std::string out, err;
};

std::shared_ptr<Run> runClient(const std::vector<std::string>& args) {
    auto run = std::make_shared<Run>();
    GSubprocessLauncher* l = g_subprocess_launcher_new(
        (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE));
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

std::shared_ptr<Run> gRun;
long gCaretBefore = 0;

std::string numbered(const char* word, int n) {
    std::string s;
    for (int i = 1; i <= n; i++) s += std::string(word) + " " + std::to_string(i) + "\n";
    return s;
}

int caretLine() {
    GtkTextBuffer* b = ed()->sourceBuffer();
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(b, &it, gtk_text_buffer_get_insert(b));
    return gtk_text_iter_get_line(&it) + 1;
}

// Whether line `n` (1-based) of the source on show carries the note's tag.
bool tagged(int n) {
    GtkTextBuffer* b = ed()->sourceBuffer();
    GtkTextTag* t = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(b), "agent-note");
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line(b, &it, n - 1);
    return t && gtk_text_iter_has_tag(&it, t);
}

std::string headText() {
    GtkWidget* top = gtk_widget_get_first_child(notes()->box());
    return gtk_label_get_text(GTK_LABEL(gtk_widget_get_first_child(top)));
}
std::string bodyText() {
    GtkWidget* top = gtk_widget_get_first_child(notes()->box());
    return gtk_label_get_text(GTK_LABEL(gtk_widget_get_next_sibling(top)));
}

void keepRun(const char* what, std::vector<std::string> args) {
    MCGuiStep(what, [args] { gRun = runClient(args); });
    MCGuiWait("the client returns", 20, [] { return gRun->done; });
}

void focusEditor() {
    MCGuiFocusKeyWindow();
    gtk_widget_grab_focus(ed()->textView());
}

}  // namespace

MC_GUI_SUITE(notes) {
    MCGuiStep("makes the files", [] {
        g_mkdir_with_parents(F("").c_str(), 0755);
        MCGuiWriteFile(F("n.txt"), numbered("line", 40));
        MCGuiWriteFile(F("m.txt"), numbered("other", 30));
        MCGuiWriteFile(F("k.txt"), numbered("third", 30));
        MCGuiWriteFile(F("t1.txt"), numbered("one", 30));
        MCGuiWriteFile(F("t2.txt"), numbered("two", 30));
        MCGuiWriteFile(F("t3.txt"), numbered("three", 30));
        MCGuiWriteFile(F("tour.json"),
            "[{\"path\":\"" + F("t1.txt") + "\",\"line\":3,\"note\":\"first stop\"},"
            "{\"path\":\"" + F("t2.txt") + "\",\"line\":5,\"to\":7,\"note\":\"second stop\"},"
            "{\"path\":\"" + F("t3.txt") + "\",\"line\":9,\"note\":\"third stop\"}]");
    });

    // Earlier suites may leave the keyboard in the editor, where a note
    // would wait beside the user's tab.
    MCGuiStep("the keyboard leaves the editor", [] { gtk_widget_grab_focus(MCGuiApp().tree->widget()); });
    MCGuiWait("it is out of the text", 5, [] { return !gtk_widget_has_focus(ed()->textView()); });

    // ---- a note: where, what, and that it is display only
    keepRun("minicode open n.txt:10 --to 12 --note", {"open", F("n.txt") + ":10", "--to", "12",
                                                       "--note", "check this\x01 loop\xE2\x80\xAE",
                                                       "--from", "Claude"});
    MCGuiStep("it exited 0", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(gRun->err.empty());
    });
    MCGuiWait("the note shows", 10, [] { return notes()->shown(); });
    MCGuiStep("on lines 10 to 12 with its text, label and box", [] {
        GCHECK(ed()->currentPath() == F("n.txt"));
        GCHECK(notes()->firstLine() == 10);
        GCHECK(notes()->lastLine() == 12);
        GCHECK(!tagged(9));
        GCHECK(tagged(10) && tagged(11) && tagged(12));
        GCHECK(!tagged(13));
        GCHECK_EQ_STR(notes()->text(), "check this loop");
        GCHECK_EQ_STR(notes()->label(), "Claude");
        GCHECK(gtk_widget_get_visible(notes()->box()));
        GCHECK_EQ_STR(headText(), "Claude");
        GCHECK_EQ_STR(bodyText(), "check this loop");
        GCHECK(caretLine() == 10);   // the agent's preview tab: the caret goes there
    });
    MCGuiStep("it is the agent's preview tab, the file untouched, nothing to undo", [] {
        Document* d = ed()->documentFor(F("n.txt"));
        GCHECK(d && d->isAgentPreview());
        GCHECK(!d->dirty());
        GCHECK_EQ_STR(MCGuiEditorText(), numbered("line", 40));
        GCHECK(!gtk_text_buffer_get_can_undo(d->buffer()));
    });
    MCGuiWait("the box is placed above the first line", 5, [] {
        GtkTextView* v = GTK_TEXT_VIEW(ed()->textView());
        GdkRectangle r;
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(ed()->sourceBuffer(), &it, 9);
        gtk_text_view_get_iter_location(v, &it, &r);
        int x, y;
        gtk_text_view_buffer_to_window_coords(v, GTK_TEXT_WINDOW_WIDGET, r.x, r.y, &x, &y);
        const int h = gtk_widget_get_height(GTK_WIDGET(v));
        return h > 0 && y >= 0 && y < h && gtk_widget_get_margin_top(notes()->box()) <= y;
    });
    MCGuiStep("snapshot", [] {
        MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "note");
    });

    // ---- Escape closes it
    MCGuiStep("Escape with the editor's keyboard", [] {
        focusEditor();
    });
    MCGuiWait("the editor has the keyboard", 5, [] { return gtk_widget_has_focus(ed()->textView()); });
    MCGuiStep("presses Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("the note is gone", 5, [] { return !notes()->shown(); });
    MCGuiStep("with its highlight", [] {
        GCHECK(!tagged(10) && !tagged(11));
        GCHECK(!gtk_widget_get_visible(notes()->box()));
    });

    // ---- the x closes it
    keepRun("a note again", {"open", F("n.txt") + ":20", "--note", "second"});
    MCGuiWait("it shows", 10, [] { return notes()->shown() && notes()->firstLine() == 20; });
    MCGuiStep("clicks the x", [] {
        GtkWidget* top = gtk_widget_get_first_child(notes()->box());
        GtkWidget* x = gtk_widget_get_last_child(top);
        MCGuiClick(x, gtk_widget_get_width(x) / 2.0, gtk_widget_get_height(x) / 2.0, 1);
    });
    MCGuiWait("it closes", 5, [] { return !notes()->shown(); });

    // ---- an edit elsewhere moves it, an edit on its lines closes it for good
    keepRun("a note on 10 to 12 again", {"open", F("n.txt") + ":10", "--to", "12", "--note", "third"});
    MCGuiWait("it shows", 10, [] { return notes()->shown() && notes()->firstLine() == 10; });
    MCGuiStep("types a line break at the top of the file", [] {
        GtkTextBuffer* b = ed()->sourceBuffer();
        GtkTextIter it;
        gtk_text_buffer_get_start_iter(b, &it);
        gtk_text_buffer_place_cursor(b, &it);
        focusEditor();
    });
    MCGuiWait("the editor has the keyboard", 5, [] { return gtk_widget_has_focus(ed()->textView()); });
    MCGuiStep("Return", [] { MCGuiKey("enter"); });
    MCGuiWait("the note moved with its lines", 5, [] {
        return notes()->shown() && notes()->firstLine() == 11 && notes()->lastLine() == 13;
    });
    MCGuiStep("the highlight follows", [] {
        GCHECK(tagged(11) && tagged(13) && !tagged(10) && !tagged(14));
        Document* d = ed()->documentFor(F("n.txt"));
        GCHECK(!d->isAgentPreview());   // an edit keeps the preview tab
    });
    MCGuiStep("puts the caret on line 12 and types", [] {
        GtkTextBuffer* b = ed()->sourceBuffer();
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(b, &it, 11);
        gtk_text_buffer_place_cursor(b, &it);
        MCGuiType("x");
    });
    MCGuiWait("the note is gone", 5, [] { return !notes()->shown(); });
    MCGuiStep("undo", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("the typed letter is undone", 5, [] {
        return MCGuiEditorText().find("xline") == std::string::npos;
    });
    MCGuiStep("the note does not come back", [] {
        GCHECK(!notes()->shown());
        GCHECK(!notes()->pendingFor(ed()->active()));
        GCHECK(!tagged(11));
    });

    // ---- the agent's preview tab is replaced by each such open
    MCGuiStep("the keyboard leaves the editor", [] { gtk_widget_grab_focus(MCGuiApp().tree->widget()); });
    MCGuiWait("it is out of the text", 5, [] { return !gtk_widget_has_focus(ed()->textView()); });
    keepRun("a note in m.txt", {"open", F("m.txt") + ":4", "--note", "in m"});
    MCGuiWait("m.txt shows its note", 10, [] { return notes()->shown() && ed()->currentPath() == F("m.txt"); });
    keepRun("a note in k.txt", {"open", F("k.txt") + ":6", "--note", "in k"});
    MCGuiWait("k.txt shows its note", 10, [] { return notes()->shown() && ed()->currentPath() == F("k.txt"); });
    MCGuiStep("m.txt's tab is replaced, n.txt's, which was edited, kept", [] {
        GCHECK(ed()->documentFor(F("m.txt")) == nullptr);
        GCHECK(ed()->documentFor(F("n.txt")) != nullptr);
        GCHECK(ed()->documentFor(F("k.txt"))->isAgentPreview());
    });

    // ---- a note sent while the user types waits with its tab
    MCGuiStep("opens n.txt and puts the keyboard in it", [] {
        ed()->openFile(F("n.txt"));
        focusEditor();
        GtkTextBuffer* b = ed()->sourceBuffer();
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(b, &it, 30);
        gtk_text_buffer_place_cursor(b, &it);
        gCaretBefore = gtk_text_iter_get_offset(&it);
    });
    MCGuiWait("the window is active with the keyboard in the text", 10, [] {
        return gtk_window_is_active(MCGuiApp().window) && gtk_widget_has_focus(ed()->textView());
    });
    keepRun("a note in m.txt while typing", {"open", F("m.txt") + ":8", "--note", "waiting"});
    MCGuiStep("a tab beside theirs, not shown, the note waiting, the client says so", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(gRun->err.find("the user is typing in the editor") != std::string::npos);
        GCHECK(ed()->currentPath() == F("n.txt"));
        GCHECK(!notes()->shown());
        Document* d = ed()->documentFor(F("m.txt"));
        GCHECK(d && d->isAgentPreview());
        GCHECK(notes()->pendingFor(d));
        GtkTextBuffer* b = ed()->sourceBuffer();
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_mark(b, &it, gtk_text_buffer_get_insert(b));
        GCHECK(gtk_text_iter_get_offset(&it) == gCaretBefore);   // their caret did not move
    });
    MCGuiStep("shows m.txt", [] { ed()->selectTab(ed()->indexOf(ed()->documentFor(F("m.txt")))); });
    MCGuiWait("the note is placed on line 8", 10, [] {
        return notes()->shown() && notes()->firstLine() == 8 && notes()->text() == "waiting";
    });
    MCGuiStep("and shown tab is where the caret went", [] { GCHECK(caretLine() == 8); });
    MCGuiStep("switching away keeps it for the tab", [] {
        ed()->selectTab(ed()->indexOf(ed()->documentFor(F("n.txt"))));
    });
    MCGuiWait("hidden while another tab shows", 5, [] { return !notes()->shown(); });
    MCGuiStep("back again", [] { ed()->selectTab(ed()->indexOf(ed()->documentFor(F("m.txt")))); });
    MCGuiWait("it is back on the same line", 5, [] { return notes()->shown() && notes()->firstLine() == 8; });
    MCGuiStep("closes it", [] { notes()->close(); });

    // ---- the tour keys do nothing outside a tour
    MCGuiStep("Alt+] with no tour", [] {
        ed()->openFile(F("n.txt"));
        focusEditor();
    });
    MCGuiWait("the editor has the keyboard", 5, [] { return gtk_widget_has_focus(ed()->textView()); });
    MCGuiStep("presses Alt+] and Alt+[", [] {
        MCGuiKey("alt+]");
        MCGuiKey("alt+[");
    });
    MCGuiPause(0.4);   // a negative check: nothing may happen
    MCGuiStep("no note, no tour, the file still on show", [] {
        GCHECK(!notes()->shown());
        GCHECK(!notes()->tourActive());
        GCHECK(ed()->currentPath() == F("n.txt"));
    });

    // ---- a tour
    keepRun("minicode tour", {"tour", F("tour.json")});
    MCGuiStep("exit 0", [] {
        if (gRun->exitCode != 0) MCGuiNote("tour said: %s", gRun->err.c_str());
        GCHECK(gRun->exitCode == 0);
        GCHECK(gRun->err.find("the user is typing in the editor") != std::string::npos);
    });
    MCGuiStep("typing in the editor: the first stop waits beside their tab", [] {
        GCHECK(notes()->tourActive());
        GCHECK(notes()->tourCount() == 3 && notes()->tourIndex() == 0);
        GCHECK(ed()->currentPath() == F("n.txt"));
        GCHECK(!notes()->shown());
        GCHECK(ed()->documentFor(F("t1.txt")) != nullptr);
    });
    MCGuiStep("Alt+] shows the second stop", [] { MCGuiKey("alt+]"); });
    MCGuiWait("stop 2 of 3", 10, [] {
        return notes()->shown() && notes()->label() == "Claude \xC2\xB7 2 of 3" &&
               ed()->currentPath() == F("t2.txt");
    });
    MCGuiStep("on lines 5 to 7 with its text", [] {
        GCHECK(notes()->firstLine() == 5 && notes()->lastLine() == 7);
        GCHECK_EQ_STR(bodyText(), "second stop");
        GCHECK_EQ_STR(headText(), "Claude \xC2\xB7 2 of 3");
        GCHECK(ed()->documentFor(F("t1.txt")) == nullptr);   // the preview tab was replaced
    });
    MCGuiStep("Alt+] again", [] { MCGuiKey("alt+]"); });
    MCGuiWait("stop 3 of 3", 10, [] {
        return notes()->shown() && notes()->label() == "Claude \xC2\xB7 3 of 3" &&
               ed()->currentPath() == F("t3.txt");
    });
    MCGuiStep("Alt+] at the end does nothing", [] { MCGuiKey("alt+]"); });
    MCGuiPause(0.3);   // a negative check
    MCGuiStep("still stop 3", [] { GCHECK(notes()->tourIndex() == 2 && notes()->shown()); });
    MCGuiStep("Alt+[ goes back", [] { MCGuiKey("alt+["); });
    MCGuiWait("stop 2 of 3 again", 10, [] {
        return notes()->shown() && notes()->label() == "Claude \xC2\xB7 2 of 3" &&
               ed()->currentPath() == F("t2.txt");
    });
    MCGuiStep("closing the note ends the tour", [] { MCGuiKey("escape"); });
    MCGuiWait("no note", 5, [] { return !notes()->shown(); });
    MCGuiStep("and the keys are dead again", [] {
        GCHECK(!notes()->tourActive());
        MCGuiKey("alt+]");
    });
    MCGuiPause(0.4);   // a negative check
    MCGuiStep("nothing happened", [] {
        GCHECK(!notes()->shown());
        GCHECK(ed()->currentPath() == F("t2.txt"));
    });

    // ---- state names the note and the tour
    keepRun("a note, then state", {"open", F("t2.txt") + ":2", "--note", "in state"});
    MCGuiWait("it shows", 10, [] { return notes()->shown(); });
    keepRun("minicode state", {"state"});
    MCGuiStep("the window's note", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK(gRun->out.find("\"note\":{") != std::string::npos);
        GCHECK(gRun->out.find("in state") != std::string::npos);
    });
    MCGuiStep("closes it", [] { notes()->close(); });
}
