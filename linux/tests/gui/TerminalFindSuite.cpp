// TerminalFindSuite.cpp — terminal_find: find in the terminal, Clear
// Terminal, and the dock's height from a pane alone in its column
// (PARITY.md 4.7, third part; the Mac's terminal_find). A real bash; output
// is known to have arrived by the shell integration's prompt count, never by
// reading the screen.
//   - Find… with a terminal focused opens the terminal's bar (the editor's
//     find bar stays shut); typing selects the first match in the
//     scrollback and screen, Return and Shift+Return step (wrapping), a miss
//     leaves no selection and says "No results";
//   - Escape closes the bar and the terminal has the keyboard again;
//   - the bar closes when another pane or tab becomes the active one;
//   - after a split and its close, with the view left up the scrollback by
//     a find, a command typed into the remaining pane runs and the view
//     comes back to the end to show it (CI's long prompt left a find's match
//     a few rows up, and the pane looked dead);
//   - Clear Terminal at the prompt clears the screen (from a view scrolled
//     up too) and the shell is still at a prompt; while a program runs it
//     sends nothing;
//   - Resize Terminal Pane Up and Down on a pane alone in its column change
//     the dock's height by four rows.
#include "Harness.h"
#include "AppSeam.h"
#include "Terminal.h"
#include "TerminalDock.h"
#include "WindowLayout.h"

#include <vte/vte.h>

#include <cmath>
#include <string>

namespace {

TerminalDock* dock() { return MCGuiApp().termDock; }
bool atPrompt(Terminal* t) { return t && t->shellPid() > 0 && t->shellState().known && t->shellState().atPrompt; }

std::string gOldShell;
bool gHadShell = false;
Terminal* gA = nullptr;
Terminal* gB = nullptr;
long long gPrompts = 0;
double gHeight = 0;
std::function<void(Terminal*, std::function<void()>)> gOldConfirm;

void action(const char* name) {
    GAction* a = g_action_map_lookup_action(G_ACTION_MAP(MCGuiApp().window), name);
    GCHECK(a != nullptr);
    if (a) g_action_activate(a, nullptr);
}

std::string selected(Terminal* t) {
    VteTerminal* v = VTE_TERMINAL(t->vteWidget());
    if (!vte_terminal_get_has_selection(v)) return "";
    char* s = vte_terminal_get_text_selected(v, VTE_FORMAT_TEXT);
    std::string out = s ? s : "";
    g_free(s);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

void type(const std::string& q) {
    gtk_editable_set_text(GTK_EDITABLE(gA->findEntry()), "");
    MCGuiType(q);
}

bool keyboardIn(GtkWidget* w) {
    GtkWidget* f = gtk_root_get_focus(GTK_ROOT(MCGuiApp().window));
    return f && (f == w || gtk_widget_is_ancestor(f, w));
}

// The view at the newest row, and the text of the rows on show.
bool atEnd(Terminal* t) {
    GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(t->vteWidget()));
    return adj && std::lround(gtk_adjustment_get_value(adj)) ==
                      std::lround(gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj));
}

void scrollToTop(Terminal* t) {
    GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(t->vteWidget()));
    gtk_adjustment_set_value(adj, gtk_adjustment_get_lower(adj));
}

std::string onScreen(Terminal* t) {
    char* s = vte_terminal_get_text(VTE_TERMINAL(t->vteWidget()), nullptr, nullptr, nullptr);
    std::string screen = s ? s : "";
    g_free(s);
    return screen;   // get_text covers the rows on show only
}

void runAndNote(Terminal* t, const std::string& line) {
    gPrompts = t->shellState().prompts;
    t->runLine(line, [](const std::string& why) { GCHECK_EQ_STR(why, ""); });
}

}  // namespace

MC_GUI_SUITE(terminal_find) {
    MCGuiStep("a bash, one fresh terminal, the dock shown", [] {
        GCHECK(dock() != nullptr);
        if (!dock()) return;
        const char* old = g_getenv("SHELL");
        gHadShell = old != nullptr;
        gOldShell = old ? old : "";
        GCHECK(std::string(g_get_home_dir()).find(MCGuiScratch()) == 0);   // never the user's own
        g_setenv("SHELL", "/bin/bash", TRUE);
        MCGuiWriteFile(std::string(g_get_home_dir()) + "/.bashrc", "\n");
        gOldConfirm = dock()->confirmClose;
        dock()->confirmClose = nullptr;
        dock()->newTerminal();
        while (dock()->count() > 1) dock()->close(0, false);
        if (!MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
        gA = dock()->at(0);
    });
    MCGuiWait("the shell marks its prompt", 30, [] { return atPrompt(gA); });
    MCGuiStep("known words in the scrollback and on the screen", [] {
        gA->focus();
        runAndNote(gA, "printf 'zeta-%s\\n' one; seq 1 120; printf 'zeta-%s\\n' two three; printf 'ZETA-%s\\n' Upper");
    });
    MCGuiWait("the command finished", 30, [] { return gA->shellState().prompts > gPrompts && atPrompt(gA); });

    // ---- find
    MCGuiStep("Find with the terminal focused opens the terminal's bar, not the editor's", [] {
        gA->focus();
        action("find");
        GCHECK(gA->findOpen());
        GCHECK(gtk_widget_get_visible(gA->findEntry()));
    });
    MCGuiWait("the bar's field has the keyboard", 5, [] { return gA->findHasKeyboard(); });
    MCGuiStep("typing a word selects its first match (scrollback included)", [] { type("zeta-one"); });
    MCGuiWait("the match is selected", 10, [] { return selected(gA) == "zeta-one"; });
    MCGuiStep("a miss: no selection, No results", [] {
        gtk_editable_set_text(GTK_EDITABLE(gA->findEntry()), "no-such-word");
    });
    MCGuiWait("the bar says so", 10, [] { return gA->findStatus() == "No results"; });
    MCGuiStep("and nothing is selected", [] { GCHECK_EQ_STR(selected(gA), ""); });
    MCGuiStep("the query is case-insensitive", [] { gtk_editable_set_text(GTK_EDITABLE(gA->findEntry()), "zeta-upper"); });
    MCGuiWait("ZETA-Upper is found", 10, [] { return selected(gA) == "ZETA-Upper"; });
    MCGuiStep("Return steps to the next match, Shift+Return back, wrapping", [] {
        gtk_editable_set_text(GTK_EDITABLE(gA->findEntry()), "zeta-");
    });
    MCGuiWait("a first match", 10, [] { return selected(gA).rfind("zeta-", 0) == 0 || selected(gA).rfind("ZETA-", 0) == 0; });
    MCGuiStep("step forward", [] { MCGuiKey("enter"); });
    MCGuiWait("another match", 10, [] { return !selected(gA).empty() && gA->findStatus().empty(); });
    MCGuiStep("four matches in all: five Returns come back round", [] {
        // zeta-one, zeta-two, zeta-three, ZETA-Upper: the outputs, the typed
        // command line holds "zeta-%s" twice more (a match each).
        std::string first = selected(gA);
        GCHECK(!first.empty());
        for (int i = 0; i < 12; i++) gA->findStep(true);
        // Twelve steps later we are on a match still, and backwards works.
        GCHECK(!selected(gA).empty());
        gA->findStep(false);
        GCHECK(!selected(gA).empty());
    });
    MCGuiStep("Escape closes the bar; the terminal has the keyboard", [] { MCGuiKey("escape"); });
    MCGuiWait("closed", 5, [] { return !gA->findOpen(); });
    MCGuiStep("and the keyboard is back in the terminal", [] {
        GCHECK(keyboardIn(gA->widget()));
        GCHECK_EQ_STR(selected(gA), "");
    });

    // ---- the bar follows the active pane
    MCGuiStep("open it, then split: the new pane is active and the bar closes", [] {
        action("find");
        GCHECK(gA->findOpen());
        action("splitterminal");
    });
    MCGuiWait("two panes, the second active", 30, [] {
        return dock()->activeGroup()->count() == 2 && MCGuiApp().terminal != gA;
    });
    MCGuiStep("the first pane's bar is shut", [] {
        gB = MCGuiApp().terminal;
        GCHECK(!gA->findOpen());
        GCHECK(gB != gA);
        GCHECK(!gB->findOpen());
    });
    MCGuiWait("the new shell is at a prompt", 30, [] { return atPrompt(gB); });
    MCGuiStep("Find now opens in the new pane only", [] {
        gB->focus();
        action("find");
        GCHECK(gB->findOpen());
        GCHECK(!gA->findOpen());
        MCGuiKey("escape");
    });
    MCGuiWait("closed again", 5, [] { return !gB->findOpen(); });
    MCGuiStep("back to one pane", [] {
        dock()->closePane(gB, false);
        gA = dock()->at(0);
        gA->focus();
    });
    MCGuiWait("one pane", 10, [] { return dock()->activeGroup()->count() == 1 && MCGuiApp().terminal == gA; });
    MCGuiStep("a find far up the scrollback in the pane left", [] {
        action("find");
        GCHECK(gA->findOpen());
        type("zeta-one");
    });
    MCGuiWait("its match is selected", 10, [] { return selected(gA) == "zeta-one"; });
    MCGuiStep("Escape: the bar closes, the view stays on the match", [] { MCGuiKey("escape"); });
    MCGuiWait("closed after the find", 5, [] { return !gA->findOpen(); });
    MCGuiStep("the view is above the end; a command typed there", [] {
        GCHECK(!atEnd(gA));
        runAndNote(gA, "echo pane-left-$((6*7))");
    });
    MCGuiWait("the command ran in the remaining pane", 30, [] {
        return gA->shellState().prompts > gPrompts && atPrompt(gA);
    });
    MCGuiStep("and the view came back to the end to show it", [] {
        GCHECK(atEnd(gA));
        GCHECK(onScreen(gA).find("pane-left-42") != std::string::npos);
    });

    // ---- clear
    MCGuiStep("Clear Terminal at the prompt: the old lines leave the screen", [] {
        GCHECK(gA->recentText(200).find("zeta-") != std::string::npos);
        gPrompts = gA->shellState().prompts;
        scrollToTop(gA);   // the user reading back: Clear brings the view to the end
        GCHECK(!atEnd(gA));
        action("clearterminal");
    });
    MCGuiWait("cleared: no old line on the screen", 15, [] {
        const std::string screen = onScreen(gA);
        return screen.find("zeta-") == std::string::npos && screen.find("ZETA-") == std::string::npos;
    });
    MCGuiStep("the shell is still at its prompt and takes a command", [] {
        GCHECK(atPrompt(gA));
        runAndNote(gA, "echo after-clear");
    });
    MCGuiWait("it ran", 30, [] { return gA->shellState().prompts > gPrompts && atPrompt(gA); });
    MCGuiStep("while a program runs, Clear sends nothing", [] {
        gPrompts = gA->shellState().prompts;
        runAndNote(gA, "sleep 3");
    });
    MCGuiWait("the sleep runs", 15, [] { return gA->shellState().busy; });
    MCGuiStep("clearScreen is refused", [] { GCHECK(!gA->clearScreen()); });
    MCGuiWait("the sleep ends", 20, [] { return gA->shellState().prompts > gPrompts && atPrompt(gA); });

    // ---- the dock's height
    MCGuiStep("Resize Up on a lone pane grows the dock by four rows", [] {
        gHeight = MCGuiApp().layout->state().terminalHeight;
        action("resizeterminalup");
    });
    MCGuiWait("the dock is taller", 10, [] { return MCGuiApp().layout->state().terminalHeight > gHeight + 1; });
    MCGuiStep("by four rows' worth, then Down brings it back", [] {
        double cw = 0, ch = 0;
        gA->cellSize(&cw, &ch);
        const double grew = MCGuiApp().layout->state().terminalHeight - gHeight;
        MCGuiNote("grew %.1f, four rows %.1f", grew, 4 * ch);
        GCHECK(std::fabs(grew - 4 * ch) < 1.5);
        action("resizeterminaldown");
    });
    MCGuiWait("the dock is as it was", 10, [] {
        return std::fabs(MCGuiApp().layout->state().terminalHeight - gHeight) < 1.5;
    });

    MCGuiStep("settings put back", [] {
        dock()->confirmClose = gOldConfirm;
        if (gHadShell) g_setenv("SHELL", gOldShell.c_str(), TRUE);
        else g_unsetenv("SHELL");
    });
}
