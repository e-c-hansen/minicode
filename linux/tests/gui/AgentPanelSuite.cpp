// AgentPanelSuite.cpp — the inline agent's bar (Ctrl+I, InlineAgentBar.cpp,
// PARITY.md 3.3 part one; the Mac's AgentPanelSuite.mm) with a stand-in for
// Claude Code: agent.command points at a script that prints a recorded
// stream (tests/fixtures/claude/stream) a line at a time, slowly, so the bar
// can be seen showing the reply before it ends. Claude Code itself never
// runs here.
//
//   ./build/minicode-guitest agent_panel
//
// Checked: the real key opens the bar at the caret's line; the run asks for
// stream-json with the request in the prompt; the reply shows while it is
// written; the edit waits as a diff in the bar with the lines it replaces
// tinted, the buffer untouched; Esc discards it; Return keeps it as one undo
// step; a user's edit elsewhere meanwhile still lets Keep place it; a second
// request builds on the edit waiting; a missing command says so; a bar
// closed by another tab says the edit was not made.
//
// Part two (the request kinds by what has the keyboard): Ctrl+I in an idle
// terminal asks for a command and types it, running nothing (a marker file
// the command would make stays absent); in the file tree it makes a task, a
// 0600 prompt file in $TMPDIR/MiniCode-agent and a `cd ... && claude "$(cat
// file)"` line typed in the terminal, not run; Ctrl+Enter makes an edit a
// task; in the Markdown preview Ctrl+I is an edit of its source; the tree
// taking the keyboard closes a bar with an edit waiting and says so; an edit
// of a tinted line redraws the tint.
#include "Harness.h"

#include "Document.h"
#include "AppSeam.h"
#include "Editor.h"
#include "FileTree.h"
#include "InlineAgentBar.h"
#include "Terminal.h"
#include "WindowLayout.h"

#include <glib/gstdio.h>
#include <vte/vte.h>
#include <sys/stat.h>

#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
InlineAgentBar* bar() { return ed()->inlineAgent(); }
std::string F(const std::string& name) { return MCGuiProjectFile("agent-panel/" + name); }
std::string Dir() { return MCGuiScratch() + "/agent-panel"; }

std::string bufferText() {
    GtkTextBuffer* b = ed()->sourceBuffer();
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    gchar* t = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    std::string out = t;
    g_free(t);
    return out;
}

VteTerminal* findVte(GtkWidget* root) {
    if (!root) return nullptr;
    if (VTE_IS_TERMINAL(root)) return VTE_TERMINAL(root);
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (VteTerminal* hit = findVte(c)) return hit;
    return nullptr;
}

std::string screenText() {
    VteTerminal* vte = findVte(MCGuiApp().terminal->widget());
    if (!vte) return "";
    char* t = vte_terminal_get_text_format(vte, VTE_FORMAT_TEXT);
    std::string s = t ? t : "";
    g_free(t);
    return s;
}

bool barHasKeyboard() {
    GtkRoot* root = gtk_widget_get_root(bar()->entry());
    GtkWidget* f = root ? gtk_window_get_focus(GTK_WINDOW(root)) : nullptr;
    return f && (f == bar()->entry() || gtk_widget_is_ancestor(f, bar()->entry()));
}

void focusText() { gtk_widget_grab_focus(ed()->textView()); }

// A stand-in agent: keeps its arguments and prompt, then runs `body`.
void writeAgent(const std::string& body) {
    const std::string script = Dir() + "/fake-claude.sh";
    MCGuiWriteFile(script, "#!/bin/sh\nprintf '%s\\n' \"$*\" > '" + Dir() + "/args.txt'\ncat > '" +
                               Dir() + "/prompt.txt'\n" + body + "\n");
    g_chmod(script.c_str(), 0755);
}

// Prints a stream file a line at a time: quickly, but pausing after the tool
// call and before the result, so each stage stays up long enough to be seen.
std::string slowStream(const std::string& file) {
    return "while IFS= read -r line; do\n"
           "  case \"$line\" in '{\"type\":\"result\"'*) sleep 1.2;; esac\n"
           "  printf '%s\\n' \"$line\"\n"
           "  case \"$line\" in '{\"type\":\"assistant\"'*tool_use*) sleep 1.0;; *) sleep 0.03;; esac\n"
           "done < '" + file + "'";
}

std::string plainReply(const std::string& reply) {
    std::string q;
    for (char c : reply) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return "printf '%s' '" + q + "'";
}

void useAgent() {
    MCGuiSetSettings("lsp.enabled = false\nagent.command = " + Dir() + "/fake-claude.sh\n");
}

const char* kOriginal = "def double(x):\n    return x * 2\n";
const char* kTripled = "def double(x):\n    return x * 3\n";

struct State {
    std::vector<std::string> seen;   // messages shown while working
    std::vector<std::string> notes;  // the status bar lines
    std::function<void(const std::string&)> realNote;
    std::string prevTask;
};
State* gS = nullptr;

void ask(const std::string& request) {
    GCHECK(barHasKeyboard());
    MCGuiType(request + "\n");
}

void waitForReply(const std::string& what) {
    MCGuiWait(what, 25, [] {
        if (bar()->mode() == InlineAgentBar::Mode::Working) {
            const std::string m = bar()->message();
            if (!m.empty() && (gS->seen.empty() || gS->seen.back() != m)) gS->seen.push_back(m);
        }
        return bar()->mode() != InlineAgentBar::Mode::Working &&
               bar()->mode() != InlineAgentBar::Mode::Input;
    });
}

bool noteSaid(const std::string& part) {
    for (const std::string& n : gS->notes)
        if (n.find(part) != std::string::npos) return true;
    return false;
}

std::string readFile(const std::string& p) {
    gchar* t = nullptr;
    std::string s;
    if (g_file_get_contents(p.c_str(), &t, nullptr, nullptr)) s = t;
    g_free(t);
    return s;
}

}  // namespace

MC_GUI_SUITE(agent_panel) {
    gS = new State();
    const std::string fixture = MCGuiBuildDir() + "/../../tests/fixtures/claude/stream/edit.jsonl";
    MCGuiStep("a file and a stand-in agent", [fixture] {
        MCGuiWriteFile(F("util.py"), kOriginal);
        MCGuiWriteFile(F("other.txt"), "other\n");
        writeAgent(slowStream(fixture));
        useAgent();
        gS->realNote = bar()->note;
        bar()->note = [](const std::string& t) { gS->notes.push_back(t); };
        GCHECK(ed()->openFile(F("util.py")));
        gS->seen.clear();
    });
    MCGuiWait("the file shows", 10, [] { return bufferText() == kOriginal; });
    MCGuiStep("Ctrl+I opens the bar at the caret", [] {
        focusText();
        GtkTextBuffer* b = ed()->sourceBuffer();
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(b, &it, 1);
        gtk_text_buffer_place_cursor(b, &it);
        GCHECK(bar()->mode() == InlineAgentBar::Mode::Closed);
        MCGuiKey("ctrl+i");
    });
    MCGuiWait("the bar is up with the keyboard", 5, [] {
        return bar()->mode() == InlineAgentBar::Mode::Input && barHasKeyboard() &&
               gtk_widget_get_mapped(bar()->widget());
    });
    MCGuiStep("it sits below the caret's line", [] {
        GtkTextView* v = GTK_TEXT_VIEW(ed()->textView());
        GtkTextBuffer* b = gtk_text_view_get_buffer(v);
        GtkTextIter caret;
        gtk_text_buffer_get_iter_at_mark(b, &caret, gtk_text_buffer_get_insert(b));
        GdkRectangle r;
        gtk_text_view_get_iter_location(v, &caret, &r);
        int x, y;
        gtk_text_view_buffer_to_window_coords(v, GTK_TEXT_WINDOW_WIDGET, r.x, r.y, &x, &y);
        const int top = gtk_widget_get_margin_top(bar()->widget());
        MCGuiNote("caret line at %d, bar at %d", y, top);
        GCHECK(top >= y + r.height);
        GCHECK(top <= y + r.height + 8);
        ask("triple it");
    });
    waitForReply("the streamed edit");
    MCGuiStep("the run, the preview and the pending diff", [fixture] {
        const std::string args = MCGuiReadFile(Dir() + "/args.txt");
        GCHECK(args.find("-p") != std::string::npos);
        GCHECK(args.find("--output-format stream-json") != std::string::npos);
        GCHECK(args.find("--include-partial-messages") != std::string::npos);
        GCHECK(args.find("--no-session-persistence") != std::string::npos);
        GCHECK(MCGuiReadFile(Dir() + "/prompt.txt").find("triple it") != std::string::npos);
        GCHECK(bar()->lastCommand().compare(0, 5, "exec ") == 0);
        GCHECK(bar()->previews() > 0);
        bool sawEdit = false;
        bool sawActivity = false;
        for (const std::string& m : bar()->previewLog()) {
            if (m.find("Edit 1") != std::string::npos) sawEdit = true;
            if (m.find("Reading") != std::string::npos || m.find("Searching") != std::string::npos)
                sawActivity = true;
        }
        GCHECK(sawActivity);
        MCGuiNote("%zu states while working", gS->seen.size());
        GCHECK(sawEdit);
        GCHECK(bar()->mode() == InlineAgentBar::Mode::Pending);
        // Not in the buffer yet, nor on disk; the tab is clean.
        GCHECK_EQ_STR(bufferText(), kOriginal);
        GCHECK_EQ_STR(MCGuiReadFile(F("util.py")), kOriginal);
        GCHECK(!ed()->dirty());
        GCHECK_EQ_STR(bar()->message(), "The agent's change, 1 line, not made yet:");
        GCHECK(bar()->diffText().find("-     return x * 2") != std::string::npos);
        GCHECK(bar()->diffText().find("+     return x * 3") != std::string::npos);
        GCHECK(bar()->tintedLines() == 1);
        MCGuiSnapshot(gtk_widget_get_parent(bar()->widget()), "inline");
        MCGuiSnapshot(ed()->textView(), "inline-editor");
        MCGuiKey("escape");
    });
    MCGuiWait("Esc closes it", 5, [] { return bar()->mode() == InlineAgentBar::Mode::Closed; });
    MCGuiStep("nothing to put back", [] {
        GCHECK_EQ_STR(bufferText(), kOriginal);
        GCHECK(!ed()->dirty());
        GCHECK(bar()->tintedLines() == 0);
        GCHECK(!gtk_widget_get_visible(bar()->widget()));
        focusText();
        MCGuiKey("ctrl+i");
    });
    MCGuiWait("again", 5, [] { return bar()->mode() == InlineAgentBar::Mode::Input && barHasKeyboard(); });
    MCGuiStep("ask", [] { ask("triple it"); });
    waitForReply("the second streamed edit");
    MCGuiStep("Return keeps it", [] {
        GCHECK(bar()->mode() == InlineAgentBar::Mode::Pending);
        MCGuiKey("enter");
    });
    MCGuiWait("the bar closes", 5, [] { return bar()->mode() == InlineAgentBar::Mode::Closed; });
    MCGuiStep("the buffer has it, one undo step takes it back", [] {
        GCHECK_EQ_STR(bufferText(), kTripled);
        GCHECK(ed()->dirty());
        GCHECK(bar()->tintedLines() == 0);
        focusText();
        MCGuiKey("ctrl+z");
    });
    MCGuiStep("undone", [] {
        GCHECK_EQ_STR(bufferText(), kOriginal);
        focusText();
        MCGuiKey("ctrl+i");
    });

    // The user types elsewhere while the edit waits: Keep still places it.
    MCGuiWait("a bar for the third request", 5,
              [] { return bar()->mode() == InlineAgentBar::Mode::Input && barHasKeyboard(); });
    MCGuiStep("ask", [] { ask("triple it"); });
    waitForReply("the third edit");
    MCGuiStep("the user adds a line at the top; the bar keeps waiting", [] {
        GCHECK(bar()->mode() == InlineAgentBar::Mode::Pending);
        GtkTextBuffer* b = ed()->sourceBuffer();
        GtkTextIter it;
        gtk_text_buffer_get_start_iter(b, &it);
        gtk_text_buffer_insert(b, &it, "# mine\n", -1);
        GCHECK(bar()->mode() == InlineAgentBar::Mode::Pending);
        GCHECK(bar()->tintedLines() == 1);   // the tint moved with its line
        MCGuiKey("enter");
    });
    MCGuiWait("Keep closes the bar", 5, [] { return bar()->mode() == InlineAgentBar::Mode::Closed; });
    MCGuiStep("both changes are in", [] {
        GCHECK_EQ_STR(bufferText(), std::string("# mine\n") + kTripled);
        focusText();
        MCGuiKey("ctrl+z");
        MCGuiKey("ctrl+z");
    });

    // A second request builds on the edit waiting; Keep makes both.
    MCGuiStep("a bar, a plain two-block reply", [] {
        GtkTextBuffer* b = ed()->sourceBuffer();
        gtk_text_buffer_set_text(b, kOriginal, -1);
        writeAgent(plainReply("<<<<<<< SEARCH\ndef double(x):\n=======\ndef double(x: int) -> int:\n>>>>>>> REPLACE\n"));
        focusText();
        MCGuiKey("ctrl+i");
    });
    MCGuiWait("bar", 5, [] { return bar()->mode() == InlineAgentBar::Mode::Input && barHasKeyboard(); });
    MCGuiStep("add types", [] { ask("add types"); });
    waitForReply("the first edit");
    MCGuiStep("it waits; a second request", [] {
        GCHECK(bar()->mode() == InlineAgentBar::Mode::Pending);
        GCHECK_EQ_STR(bufferText(), kOriginal);
        writeAgent(plainReply("<<<<<<< SEARCH\n    return x * 2\n=======\n    return x * 4\n>>>>>>> REPLACE\n"));
        GCHECK(barHasKeyboard());
        MCGuiType("quadruple\n");
    });
    waitForReply("the second edit");
    MCGuiStep("the prompt named the first request; Return keeps both", [] {
        const std::string prompt = MCGuiReadFile(Dir() + "/prompt.txt");
        GCHECK(prompt.find("add types") != std::string::npos);
        GCHECK(prompt.find("def double(x: int) -> int:") != std::string::npos);
        GCHECK(bar()->mode() == InlineAgentBar::Mode::Pending);
        MCGuiKey("enter");
    });
    MCGuiWait("closed", 5, [] { return bar()->mode() == InlineAgentBar::Mode::Closed; });
    MCGuiStep("both edits", [] {
        GCHECK_EQ_STR(bufferText(), "def double(x: int) -> int:\n    return x * 4\n");
    });

    // A reply that is only text; a SEARCH that matches nothing.
    MCGuiStep("an answer", [] {
        gtk_text_buffer_set_text(ed()->sourceBuffer(), kOriginal, -1);
        writeAgent(plainReply("It doubles x."));
        focusText();
        MCGuiKey("ctrl+i");
    });
    MCGuiWait("bar", 5, [] { return bar()->mode() == InlineAgentBar::Mode::Input && barHasKeyboard(); });
    MCGuiStep("what does it do", [] { ask("what does it do"); });
    waitForReply("the answer");
    MCGuiStep("shown as text, not an edit", [] {
        GCHECK(bar()->mode() == InlineAgentBar::Mode::Message);
        GCHECK_EQ_STR(bar()->message(), "It doubles x.");
        GCHECK(!bar()->messageIsError());
        GCHECK_EQ_STR(bufferText(), kOriginal);
        writeAgent(plainReply("<<<<<<< SEARCH\nno such line\n=======\nx\n>>>>>>> REPLACE\n"));
        MCGuiType("change it\n");
    });
    waitForReply("the refused edit");
    MCGuiStep("it says it could not be applied and changes nothing", [] {
        GCHECK(bar()->messageIsError());
        GCHECK(bar()->message().find("could not be applied") != std::string::npos);
        GCHECK_EQ_STR(bufferText(), kOriginal);
        MCGuiKey("escape");
    });

    // A command that is not there.
    MCGuiStep("a missing command", [] {
        MCGuiSetSettings("lsp.enabled = false\nagent.command = /nonexistent/claude-nowhere\n");
        focusText();
        MCGuiKey("ctrl+i");
    });
    MCGuiWait("bar", 5, [] { return bar()->mode() == InlineAgentBar::Mode::Input && barHasKeyboard(); });
    MCGuiStep("ask", [] { ask("anything"); });
    waitForReply("the failure");
    MCGuiStep("it says Claude Code was not found", [] {
        GCHECK(bar()->messageIsError());
        MCGuiNote("%s", bar()->message().c_str());
        GCHECK(bar()->message().find("was not found") != std::string::npos);
        GCHECK(bar()->message().find("agent.command") != std::string::npos);
        GCHECK_EQ_STR(bufferText(), kOriginal);
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 5, [] { return bar()->mode() == InlineAgentBar::Mode::Closed; });
    MCGuiStep("the stand-in again", [] {
        useAgent();
        writeAgent(plainReply("<<<<<<< SEARCH\n    return x * 2\n=======\n    return x * 5\n>>>>>>> REPLACE\n"));
        focusText();
        MCGuiKey("ctrl+i");
    });

    // The bar closed from elsewhere with an edit waiting.
    MCGuiWait("bar", 5, [] { return bar()->mode() == InlineAgentBar::Mode::Input && barHasKeyboard(); });
    MCGuiStep("ask", [] { ask("five"); });
    waitForReply("the edit");
    MCGuiStep("another tab closes the bar and says so", [] {
        GCHECK(bar()->mode() == InlineAgentBar::Mode::Pending);
        gS->notes.clear();
        GCHECK(ed()->openFile(F("other.txt")));
    });
    MCGuiWait("closed", 5, [] { return bar()->mode() == InlineAgentBar::Mode::Closed; });
    MCGuiStep("the status bar said it was not made", [] {
        bool said = false;
        for (const std::string& n : gS->notes)
            if (n.find("edit was not made") != std::string::npos) said = true;
        GCHECK(said);
        GCHECK(bar()->tintedLines() == 0);
        GCHECK(ed()->openFile(F("util.py")));
        bar()->note = gS->realNote;
    });

    // ---- Part two: the kinds of request.
    MCGuiStep("a terminal, an agent that answers with a command", [] {
        useAgent();
        writeAgent(plainReply("```\ntouch " + Dir() + "/ran.txt\n```"));
        g_unlink((Dir() + "/ran.txt").c_str());
        bar()->note = [](const std::string& t) { gS->notes.push_back(t); };
        gS->notes.clear();
        GCHECK(MCGuiApp().terminal != nullptr);
        if (!MCGuiApp().terminal) return;
        if (!MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
        MCGuiApp().terminal->typeText("true");
    });
    MCGuiWait("the shell is up", 30, [] {
        return MCGuiApp().terminal->shellPid() > 0 && screenText().find("true") != std::string::npos;
    });
    MCGuiStep("clear the line, give the terminal the keyboard", [] {
        MCGuiApp().terminal->focus();
        MCGuiKey("ctrl+u");
    });
    MCGuiStep("Ctrl+I there asks for a command", [] {
        MCGuiKey("ctrl+i");
    });
    MCGuiWait("a command bar", 5, [] {
        return bar()->mode() == InlineAgentBar::Mode::Input &&
               bar()->kind() == InlineAgentBar::Kind::Command && barHasKeyboard();
    });
    MCGuiStep("ask", [] { ask("make the marker file"); });
    MCGuiWait("typed at the prompt", 25, [] {
        return bar()->mode() == InlineAgentBar::Mode::Closed &&
               screenText().find("ran.txt") != std::string::npos;
    });
    MCGuiPause(1.5);   // negative check: time is the condition, a run would make the marker
    MCGuiStep("typed, not run", [] {
        GCHECK(bar()->lastTypedLine().find("touch ") == 0);
        GCHECK(!g_file_test((Dir() + "/ran.txt").c_str(), G_FILE_TEST_EXISTS));
        GCHECK(noteSaid("not run"));
        MCGuiKey("ctrl+u");
    });

    MCGuiStep("the file tree has the keyboard", [] {
        gS->notes.clear();
        GCHECK(MCGuiApp().tree != nullptr);
        MCGuiApp().tree->focus();
        MCGuiKey("ctrl+i");
    });
    MCGuiWait("a task bar", 5, [] {
        return bar()->mode() == InlineAgentBar::Mode::Input &&
               bar()->kind() == InlineAgentBar::Kind::Task && barHasKeyboard();
    });
    MCGuiStep("ask", [] { ask("tidy the tests"); });
    MCGuiWait("the task is typed", 15, [] {
        return bar()->mode() == InlineAgentBar::Mode::Closed && screenText().find("$(cat") != std::string::npos;
    });
    MCGuiStep("a 0600 prompt file and a typed line", [] {
        const std::string f = bar()->lastTaskFile();
        GCHECK(!f.empty());
        struct stat st;
        GCHECK(stat(f.c_str(), &st) == 0);
        GCHECK((int)(st.st_mode & 0777) == 0600);
        const std::string body = readFile(f);
        GCHECK(body.find("tidy the tests") != std::string::npos);
        GCHECK(body.find("file tree") != std::string::npos);
        const std::string line = bar()->lastTypedLine();
        GCHECK(line.find("cd ") == 0);
        GCHECK(line.find("fake-claude.sh \"$(cat '" + f + "')\"") != std::string::npos);
        GCHECK(noteSaid("not run"));
        MCGuiKey("ctrl+u");
    });

    MCGuiStep("Ctrl+Enter makes an edit request a task", [] {
        gS->notes.clear();
        GCHECK(ed()->openFile(F("util.py")));
        focusText();
        MCGuiKey("ctrl+i");
    });
    MCGuiWait("an edit bar", 5, [] {
        return bar()->mode() == InlineAgentBar::Mode::Input &&
               bar()->kind() == InlineAgentBar::Kind::Edit && barHasKeyboard();
    });
    MCGuiStep("type a request and press Ctrl+Enter", [] {
        gS->prevTask = bar()->lastTaskFile();
        MCGuiType("rename double");
        MCGuiKey("ctrl+enter");
    });
    MCGuiWait("a new task", 10, [] {
        return bar()->mode() == InlineAgentBar::Mode::Closed && bar()->lastTaskFile() != gS->prevTask;
    });
    MCGuiStep("the prompt names the file; the buffer is untouched", [] {
        const std::string body = readFile(bar()->lastTaskFile());
        GCHECK(body.find("rename double") != std::string::npos);
        GCHECK(body.find("util.py") != std::string::npos);
        GCHECK_EQ_STR(bufferText(), kOriginal);
        MCGuiKey("ctrl+u");
    });

    MCGuiStep("the Markdown preview: Ctrl+I is an edit of its source", [] {
        MCGuiWriteFile(F("notes.md"), "# Notes\n\nhello world\n");
        writeAgent(plainReply("<<<<<<< SEARCH\nhello world\n=======\nhello there\n>>>>>>> REPLACE\n"));
        GCHECK(ed()->openFile(F("notes.md")));
    });
    MCGuiWait("the page shows", 10, [] { return ed()->inPreview(); });
    MCGuiStep("Ctrl+I", [] {
        focusText();
        MCGuiKey("ctrl+i");
    });
    MCGuiWait("an edit bar", 5, [] {
        return bar()->mode() == InlineAgentBar::Mode::Input &&
               bar()->kind() == InlineAgentBar::Kind::Edit && barHasKeyboard();
    });
    MCGuiStep("ask", [] { ask("friendlier"); });
    waitForReply("the edit");
    MCGuiStep("it waits; Return keeps it in the source", [] {
        GCHECK(bar()->mode() == InlineAgentBar::Mode::Pending);
        GCHECK_EQ_STR(bufferText(), "# Notes\n\nhello world\n");
        MCGuiKey("enter");
    });
    MCGuiWait("closed", 5, [] { return bar()->mode() == InlineAgentBar::Mode::Closed; });
    MCGuiStep("the source has it", [] {
        GCHECK_EQ_STR(bufferText(), "# Notes\n\nhello there\n");
        GCHECK(ed()->openFile(F("util.py")));
        gtk_text_buffer_set_text(ed()->sourceBuffer(), kOriginal, -1);
        writeAgent(plainReply("<<<<<<< SEARCH\n    return x * 2\n=======\n    return x * 5\n>>>>>>> REPLACE\n"));
        focusText();
        MCGuiKey("ctrl+i");
    });
    MCGuiWait("bar", 5, [] { return bar()->mode() == InlineAgentBar::Mode::Input && barHasKeyboard(); });
    MCGuiStep("ask", [] { ask("five"); });
    waitForReply("the edit");

    // An edit of a tinted line redraws the tint.
    MCGuiStep("the user retypes the tinted line as it was", [] {
        GCHECK(bar()->mode() == InlineAgentBar::Mode::Pending);
        GCHECK(bar()->tintedLines() == 1);
        GtkTextBuffer* b = ed()->sourceBuffer();
        GtkTextIter s, e;
        gtk_text_buffer_get_iter_at_line(b, &s, 1);
        e = s;
        gtk_text_iter_forward_to_line_end(&e);
        gtk_text_buffer_delete(b, &s, &e);
        gtk_text_buffer_insert(b, &s, "    return x * 2", -1);
    });
    MCGuiWait("the tint is back", 5, [] { return bar()->tintedLines() == 1; });

    // The tree taking the keyboard closes the bar and says so.
    MCGuiStep("the tree takes the keyboard", [] {
        gS->notes.clear();
        MCGuiApp().tree->focus();
    });
    MCGuiWait("closed", 5, [] { return bar()->mode() == InlineAgentBar::Mode::Closed; });
    MCGuiStep("the status bar said it was not made", [] {
        GCHECK(noteSaid("edit was not made"));
        GCHECK_EQ_STR(bufferText(), kOriginal);
        bar()->note = gS->realNote;
    });
}
