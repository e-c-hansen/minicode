// TerminalMarksSuite.cpp — terminal_marks: the terminal's commands, from
// marks the session's host reads off the pty (SESSION-DESIGN.md stage 5;
// Terminal.cpp, SessionHost.h). A real bash with the integration, its pty
// the host's and VTE fed what the shell writes:
//   - commands run at the prompt are marked, with their statuses; the
//     host's line for each lands on the VTE row its prompt shows on;
//   - Ctrl+Up (a real key) moves to the last command and outlines it,
//     again to the one before with its prompt at the top of the view,
//     Ctrl+Down back, and past the newest to the end with no outline;
//     with no command above it stays where it is;
//   - typing forgets the outline;
//   - while a program has the alternate screen the keys are the program's;
//   - the shell gets VTE's size (stty size) after its font grows;
//   - the waiting dot goes with a key typed into the shell, not with
//     Ctrl+Shift+C, which sends the shell nothing.
#include "Harness.h"
#include "AppSeam.h"
#include "Json.h"
#include "Terminal.h"
#include "TerminalAttention.h"
#include "WindowLayout.h"

#include <vte/vte.h>

#include <algorithm>
#include <string>

namespace {

Terminal* term() { return MCGuiApp().terminal; }

void typeLine(const std::string& line) {
    VteTerminal* vte = VTE_TERMINAL(term()->vteWidget());
    const std::string bytes = "\x15" + line + "\r";
    vte_terminal_feed_child(vte, bytes.c_str(), (gssize)bytes.size());
}

// A row and the two below it, a soft wrap joining them (the prompt holds the
// scratch folder's long path, so a command often wraps).
std::string rowText(long row) {
    VteTerminal* vte = VTE_TERMINAL(term()->vteWidget());
    gsize n = 0;
    char* t = vte_terminal_get_text_range_format(vte, VTE_FORMAT_TEXT, row, 0, row + 2,
                                                 vte_terminal_get_column_count(vte), &n);
    std::string s = t ? std::string(t, n) : std::string();
    g_free(t);
    return s;
}

// The first three rows on show, joined (a soft wrap may or may not end a
// line of VTE's text).
std::string topText() {
    VteTerminal* vte = VTE_TERMINAL(term()->vteWidget());
    char* t = vte_terminal_get_text_format(vte, VTE_FORMAT_TEXT);
    std::string s = t ? t : "";
    g_free(t);
    std::string out;
    int lines = 0;
    for (char c : s) {
        if (c == '\n' && ++lines == 3) break;
        if (c != '\n') out += c;
    }
    return out;
}

// The command whose prompt is at the top of the view: the rows on show begin
// with its prompt's rows.
bool atTop(long long prompt, const std::string& command) {
    std::string mine = rowText(term()->rowOfLine(prompt));
    mine.erase(std::remove(mine.begin(), mine.end(), '\n'), mine.end());
    const std::string top = topText();
    return top.find(command) != std::string::npos && mine.size() >= 10 && top.compare(0, 10, mine, 0, 10) == 0 &&
           top.find(command) == mine.find(command);
}

bool atEnd() {
    GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(term()->vteWidget()));
    return adj && gtk_adjustment_get_value(adj) + 0.5 >=
                      gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj);
}

// The rows a command was typed on: from its prompt's row up to the row its
// output starts on (a long prompt and command wrap over several). Reading a
// fixed number of rows from the prompt took in the next command's prompt
// when the output was short, so `seq 1 80` was found at the first command.
std::string commandText(const Shared::TermMark& m) {
    VteTerminal* vte = VTE_TERMINAL(term()->vteWidget());
    const long first = term()->rowOfLine(m.prompt);
    const long last = std::max(first, term()->rowOfLine(m.output) - 1);
    gsize n = 0;
    char* t = vte_terminal_get_text_range_format(vte, VTE_FORMAT_TEXT, first, 0, last,
                                                 vte_terminal_get_column_count(vte), &n);
    std::string s = t ? std::string(t, n) : std::string();
    g_free(t);
    s.erase(std::remove(s.begin(), s.end(), '\n'), s.end());
    return s;
}

// The prompt line of the command typed as `text`, -1 for none.
long long promptOf(const std::string& text) {
    for (const Shared::TermMark& m : term()->commandMarks())
        if (m.output >= 0 && commandText(m).find(text) != std::string::npos) return m.prompt;
    return -1;
}

bool idle() {
    const auto& s = term()->shellState();
    return term()->shellPid() > 0 && s.known && s.atPrompt && !s.busy;
}

bool keyboardInTerminal() {
    GtkWidget* f = gtk_window_get_focus(MCGuiApp().window);
    return f && term()->owns(f);
}

std::string gOldShell;
bool gHadShell = false;
long long gPrompts = 0;
Shared::TermId gOldTerm = 0;
long long gSeq = -1, gThree = -1;
double gScale = 1;
int gCols = 0;

}  // namespace

MC_GUI_SUITE(terminal_marks) {
    MCGuiStep("a bash terminal", [] {
        const char* old = g_getenv("SHELL");
        gHadShell = old != nullptr;
        gOldShell = old ? old : "";
        g_setenv("SHELL", "/bin/bash", TRUE);
        if (!MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
        gOldTerm = term()->made() ? term()->hostTerminal() : 0;
        if (gOldTerm) typeLine("exit");   // restarted as bash, with no commands of earlier suites
        term()->focus();
    });
    MCGuiWait("a new bash at its prompt", 30, [] {
        return term()->hostTerminal() != 0 && term()->hostTerminal() != gOldTerm && idle() &&
               term()->shellState().prompts >= 1;
    });
    MCGuiStep("four commands, one failing, one with a page of output", [] {
        gPrompts = term()->shellState().prompts;
        typeLine("echo one; false");
    });
    MCGuiWait("the first done", 10, [] { return idle() && term()->shellState().prompts > gPrompts; });
    MCGuiStep("seq", [] {
        GCHECK(term()->shellState().status == 1);
        gPrompts = term()->shellState().prompts;
        typeLine("seq 1 80");
    });
    MCGuiWait("seq done", 10, [] { return idle() && term()->shellState().prompts > gPrompts; });
    MCGuiStep("echo three", [] {
        gPrompts = term()->shellState().prompts;
        typeLine("echo three");
    });
    MCGuiWait("three done and marked", 10, [] {
        return idle() && term()->shellState().prompts > gPrompts && term()->commandsRun() >= 3;
    });
    MCGuiWait("each command's prompt row shows it", 10, [] {
        gSeq = promptOf("seq 1 80");
        gThree = promptOf("echo three");
        return gSeq >= 0 && gThree > gSeq && promptOf("echo one; false") >= 0;
    });
    MCGuiStep("their statuses", [] {
        const auto& marks = term()->commandMarks();
        int failed = 0, ok = 0;
        for (const Shared::TermMark& m : marks)
            if (m.end >= 0) (m.status == 1 ? failed : ok)++;
        GCHECK(failed == 1 && ok >= 2);
        GCHECK(term()->outlinedCommand() == -1 && atEnd());
        GCHECK(keyboardInTerminal());
        MCGuiKey("ctrl+up");
    });
    MCGuiWait("Ctrl+Up: the last command, outlined", 5, [] { return term()->outlinedCommand() == gThree; });
    MCGuiStep("its prompt shows; again", [] {
        GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(term()->vteWidget()));
        GCHECK(adj != nullptr);
        MCGuiKey("ctrl+up");
    });
    MCGuiWait("Ctrl+Up again: seq, its prompt at the top", 5, [] {
        return term()->outlinedCommand() == gSeq && atTop(gSeq, "seq 1 80");
    });
    MCGuiStep("Ctrl+Down", [] { MCGuiKey("ctrl+down"); });
    MCGuiWait("echo three again", 5, [] { return term()->outlinedCommand() == gThree; });
    MCGuiStep("Ctrl+Down past the newest", [] { MCGuiKey("ctrl+down"); });
    MCGuiWait("the end, no outline", 5, [] { return term()->outlinedCommand() == -1 && atEnd(); });
    MCGuiStep("to the first command", [] {
        for (int i = 0; i < 3; i++) term()->moveToCommand(-1);
    });
    MCGuiWait("at the first", 5, [] { return term()->outlinedCommand() == promptOf("echo one; false"); });
    MCGuiStep("nothing above it: it stays", [] {
        const long long first = term()->outlinedCommand();
        GCHECK(term()->moveToCommand(-1));
        GCHECK(term()->outlinedCommand() == first);
        MCGuiType("x");
    });
    MCGuiWait("typing forgets it", 5, [] { return term()->outlinedCommand() == -1; });

    // The waiting dot: Ctrl+Shift+C sends the shell nothing and leaves it;
    // a key the shell gets clears it.
    MCGuiStep("the terminal marked waiting", [] {
        VteTerminal* vte = VTE_TERMINAL(term()->vteWidget());
        vte_terminal_feed_child(vte, "\x15", 1);
        Json args = Json::object();
        args.set("state", Json("waiting"));
        args.set("message", Json("Claude needs your permission"));
        std::string error;
        GCHECK(TerminalAttention::apply(term()->controlId(), args, &error));
        GCHECK_EQ_STR(TerminalAttention::markName(term()->controlId()), "waiting");
        MCGuiKey("ctrl+shift+c");
    });
    MCGuiPause(0.4);   // negative check: the copy must not clear it
    MCGuiStep("a copy leaves the mark", [] {
        GCHECK_EQ_STR(TerminalAttention::markName(term()->controlId()), "waiting");
        MCGuiType("y");
    });
    MCGuiWait("a typed key clears it", 5, [] { return std::string(TerminalAttention::markName(term()->controlId())).empty(); });

    // A program on the alternate screen keeps the keys.
    MCGuiStep("a program takes the alternate screen", [] {
        gPrompts = term()->shellState().prompts;
        typeLine("printf '\\e[?1049h'; sleep 2; printf '\\e[?1049l'");
    });
    MCGuiWait("it has it", 10, [] { return term()->onAlternateScreen(); });
    MCGuiStep("Ctrl+Up is the program's", [] {
        GCHECK(!term()->moveToCommand(-1));
        GCHECK(term()->outlinedCommand() == -1);
    });
    MCGuiWait("it ends", 15, [] { return idle() && term()->shellState().prompts > gPrompts; });

    // The shell's size is VTE's, after its font changes (as the
    // terminals' zoom does).
    MCGuiStep("a bigger font", [] {
        VteTerminal* vte = VTE_TERMINAL(term()->vteWidget());
        gScale = vte_terminal_get_font_scale(vte);
        gCols = term()->columns();
        vte_terminal_set_font_scale(vte, gScale * 1.5);
    });
    MCGuiWait("VTE has fewer columns", 5, [] { return term()->columns() < gCols; });
    MCGuiStep("stty size", [] {
        gPrompts = term()->shellState().prompts;
        typeLine("stty size");
    });
    MCGuiWait("it answers VTE's size", 10, [] {
        if (!idle() || term()->shellState().prompts <= gPrompts) return false;
        const std::string want = std::to_string(term()->rows()) + " " + std::to_string(term()->columns());
        VteTerminal* vte = VTE_TERMINAL(term()->vteWidget());
        long col = 0, row = 0;
        vte_terminal_get_cursor_position(vte, &col, &row);
        for (long r = row; r >= 0 && r > row - 6; r--)
            if (rowText(r).find("\n" + want + "\n") != std::string::npos || rowText(r).find(want + "\n") == 0)
                return true;
        return false;
    });
    MCGuiStep("the font as it was", [] {
        vte_terminal_set_font_scale(VTE_TERMINAL(term()->vteWidget()), gScale);
    });
    MCGuiStep("SHELL as it was", [] {
        if (gHadShell) g_setenv("SHELL", gOldShell.c_str(), TRUE);
        else g_unsetenv("SHELL");
    });
}
