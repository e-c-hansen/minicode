// TerminalKeysSuite.cpp — terminal_keys: readline's Control keys reach the
// shell while the terminal has the keyboard (linux/HANDOFF.md item 108), as
// on the Mac, where mutible's keys are on Command and Control is the
// shell's. On Linux mutible's keys are on Ctrl, so while the terminal has
// it every plain Ctrl key that is not Global gives way
// (Commands::linuxTerminalTakes, Keys.cpp's bound), as VS Code's terminal
// sends keys to the shell unless they are in its commandsToSkipShell. A
// real bash, started as the app starts it:
//   - none of Ctrl+W, A, E, U, K, R, L, D, C, Z, Y, P, N, B and F (Close
//     Tab, Open Recent, Select Line, Quick Open, the sidebar...) is an
//     accelerator then, and each reaches the program as its control byte,
//     read back from what a raw `head` wrote, with no command run;
//   - Ctrl+W at a bash prompt deletes the word before the cursor;
//   - with the editor focused, Ctrl+W is Close Tab again;
//   - what VTE itself sends for Ctrl with readline's punctuation (Ctrl+[,
//     ], \, @, _, ^, Space, /, 3 to 8) is Commands::terminalControlByte's.
// Then with terminal.mac-style-keys on (linux/HANDOFF.md item 113: a
// Mac-style remapper sends the Mac's Cmd as Ctrl and the physical Ctrl as
// Super):
//   - Ctrl+W, F and S are accelerators in the terminal, and Ctrl+K, U, L,
//     Z, X and D, which no command there has, reach nothing;
//   - Super with those punctuation keys sends what Ctrl did with the
//     setting off, and Super with letters (with Shift, and Toshy's
//     Super+Alt+K, D and L) their control bytes, read back from a raw
//     `head` and from VTE's commit signal;
//   - Super+W at a prompt deletes a word, Super+Alt+K kills to the end of
//     the line, and Super+C interrupts a `sleep 100`;
//   - Ctrl+A selects all and Ctrl+C copies it (the clipboard read back),
//     and Ctrl+V pastes into the prompt;
//   - Ctrl+W closes the terminal tab with the keyboard, as Cmd+W on the
//     Mac, and in the editor still closes the editor's tab;
//   - with the setting off again, Ctrl+W is the shell's once more.
#include "Harness.h"
#include "AppSeam.h"
#include "Commands.h"
#include "Editor.h"
#include "EnvLock.h"
#include "Keys.h"
#include "Terminal.h"
#include "TerminalDock.h"
#include "WindowLayout.h"

#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <vte/vte.h>

#include <cstdio>
#include <iterator>
#include <string>

namespace {

TerminalDock* dock() { return MCGuiApp().termDock; }
bool ready(Terminal* t) { return t && t->shellPid() > 0 && t->shellState().known && t->shellState().atPrompt; }
GtkWidget* focusWidget() { return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)); }
bool terminalHasKeyboard() {
    Terminal* t = MCGuiApp().terminal;
    return t && t->owns(focusWidget()) && gtk_window_is_active(MCGuiApp().window);
}

// readline's keys that are also, or were, mutible's on Linux.
const char kLetters[] = "waeukrldczypnbf";
std::string gHome, gOldShell;
bool gHadShell = false;
Terminal* gT = nullptr;
std::string gCommitted;   // what VTE sent to the shell since the last reset
gulong gCommitId = 0;
int gTabs = 0;
bool gSidebar = false;

std::string accelsFor(const std::string& accel) {
    gchar** names = gtk_application_get_actions_for_accel(MCGuiApplication(), accel.c_str());
    std::string out;
    for (gchar** n = names; n && *n; n++) out += (out.empty() ? "" : " ") + std::string(*n);
    g_strfreev(names);
    return out;
}

std::string hex(const std::string& bytes) {
    std::string out;
    char b[4];
    for (unsigned char c : bytes) {
        std::snprintf(b, sizeof b, "%02x ", c);
        out += b;
    }
    return out;
}

// readline's punctuation control keys and the digits xterm gives control
// codes, as the character each types (Shift applied) and the key to press
// after the modifier. None is an accelerator in the terminal.
struct Punct { char32_t ch; const char* key; };
const Punct kPunct[] = {
    {'[', "["}, {']', "]"}, {'\\', "\\"}, {'@', "@"}, {'_', "_"}, {'^', "^"},
    {' ', "space"}, {'/', "/"}, {'3', "3"}, {'4', "4"}, {'5', "5"},
    {'6', "6"}, {'7', "7"}, {'8', "8"},
};
std::string punctBytes() {
    std::string want;
    for (const Punct& p : kPunct) want += (char)Commands::terminalControlByte(p.ch);
    return want;
}

// With the setting on: Super with letters (Shift too, and Toshy's
// Super+Alt), each sending that letter's control byte.
const char* const kSuperLetters[] = {
    "super+w", "super+a", "super+e", "super+u", "super+k", "super+r", "super+l",
    "super+c", "super+z", "super+y", "super+shift+w", "super+alt+k", "super+alt+d",
    "super+alt+l",
};
// Keys with Ctrl that no command has in the terminal, which then reach nothing.
const char* const kDropped[] = {"ctrl+k", "ctrl+u", "ctrl+l", "ctrl+z", "ctrl+x", "ctrl+d"};
std::string superBytes() {
    std::string want;
    for (const char* k : kSuperLetters) want += (char)(k[std::string(k).size() - 1] - 'a' + 1);
    return want + punctBytes();
}

std::string gSettings;   // the settings file as the harness left it
std::string settingsPath() {
    const char* p = g_getenv("MUTIBLE_SETTINGS");
    return p ? p : "";
}

std::string gClip;
bool gClipReading = false;
// Asks for the clipboard's text, unless an ask is on its way; gClip has the
// last answer.
void readClipboard() {
    if (gClipReading) return;
    gClipReading = true;
    GdkClipboard* clip = gtk_widget_get_clipboard(GTK_WIDGET(MCGuiApp().window));
    gdk_clipboard_read_text_async(clip, nullptr, [](GObject* src, GAsyncResult* res, gpointer) {
        char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, nullptr);
        gClip = text ? text : "";
        g_free(text);
        gClipReading = false;
    }, nullptr);
}

void recordCommits() {
    if (gCommitId) return;
    gCommitId = g_signal_connect(gT->vteWidget(), "commit",
                                 G_CALLBACK(+[](GtkWidget*, char* text, guint size, gpointer) {
                                     gCommitted.append(text, size);
                                 }),
                                 nullptr);
}
void stopRecording() {
    if (gCommitId) g_signal_handler_disconnect(gT->vteWidget(), gCommitId);
    gCommitId = 0;
}

Terminal* gSecond = nullptr;

}  // namespace

MC_GUI_SUITE(terminal_keys) {
    MCGuiStep("a bash in the only terminal, the dock shown", [] {
        GCHECK(dock() != nullptr);
        if (!dock()) return;
        const char* old = g_getenv("SHELL");
        gHadShell = old != nullptr;
        gOldShell = old ? old : "";
        gHome = g_get_home_dir();
        GCHECK(gHome.find(MCGuiScratch()) == 0);   // never the user's own
        EnvLock::envSet("SHELL", "/bin/bash", TRUE);
        MCGuiWriteFile(gHome + "/.bashrc", "PS1='$ '\n");
        while (dock()->count() > 1) dock()->close(dock()->count() - 1, false);
        if (dock()->at(0)->made()) {   // another suite's shell: a fresh bash
            dock()->newTerminal();
            dock()->close(0, false);
        }
        if (!MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
        gT = dock()->at(0);
        gT->focus();
    });
    MCGuiWait("bash marks its prompt", 30, [] { return ready(gT); });
    MCGuiStep("the keyboard in the terminal; what VTE sends the shell recorded", [] {
        gT->focus();
        gCommitId = g_signal_connect(gT->vteWidget(), "commit",
                                     G_CALLBACK(+[](GtkWidget*, char* text, guint size, gpointer) {
                                         gCommitted.append(text, size);
                                     }),
                                     nullptr);
    });
    MCGuiWait("the terminal has the keyboard", 10, terminalHasKeyboard);
    MCGuiStep("none of readline's Ctrl keys is an accelerator there", [] {
        for (const char* l = kLetters; *l; l++) {
            const std::string accel = std::string("<Control>") + *l;
            const std::string bound = accelsFor(accel);
            if (!bound.empty()) MCGuiNote("%s runs %s in the terminal", accel.c_str(), bound.c_str());
            GCHECK(bound.empty());
        }
        // mutible's ways out of the terminal, Global, still are.
        GCHECK(!accelsFor("<Control>grave").empty());
        GCHECK(!accelsFor("<Control>0").empty());
        gTabs = MCGuiApp().editor->tabCount();
        gSidebar = MCGuiApp().layout->sidebarShown();
    });
    MCGuiStep("a program that writes the bytes it reads, raw", [] {
        g_remove(MCGuiProjectFile("ctrl-keys.bin").c_str());
        MCGuiType("stty raw -echo -iexten; head -c 15 > ctrl-keys.bin; stty sane\n");
    });
    MCGuiWait("it is reading", 20, [] {
        return MCGuiFileExists(MCGuiProjectFile("ctrl-keys.bin")) && gT->shellState().busy;
    });
    MCGuiStep("Ctrl+W, A, E, U, K, R, L, D, C, Z, Y, P, N, B, F pressed", [] {
        gCommitted.clear();
        for (const char* l = kLetters; *l; l++) MCGuiKey(std::string("ctrl+") + *l);
    });
    MCGuiWait("VTE sent all fifteen", 20, [] { return gCommitted.size() >= 15; });
    MCGuiWait("the program read them", 20, [] {
        return MCGuiReadFile(MCGuiProjectFile("ctrl-keys.bin")).size() == 15;
    });
    MCGuiStep("each as its control byte, in order, and no command ran", [] {
        std::string want;
        for (const char* l = kLetters; *l; l++) want += (char)(*l - 'a' + 1);
        const std::string got = MCGuiReadFile(MCGuiProjectFile("ctrl-keys.bin"));
        if (got != want) MCGuiNote("read %s, wanted %s", hex(got).c_str(), hex(want).c_str());
        GCHECK(got == want);
        GCHECK(gCommitted == want);
        GCHECK(MCGuiApp().editor->tabCount() == gTabs);          // Ctrl+W, Ctrl+N
        GCHECK(MCGuiApp().layout->sidebarShown() == gSidebar);   // Ctrl+B
        GCHECK(terminalHasKeyboard());                           // Ctrl+P, Ctrl+F, Ctrl+R
    });
    MCGuiWait("the prompt is back", 20, [] { return ready(gT); });
    MCGuiStep("at the prompt: echo one two, Ctrl+W, three", [] {
        g_remove(MCGuiProjectFile("ctrl-w.txt").c_str());
        MCGuiType("echo one two");
        MCGuiKey("ctrl+w");
        MCGuiType("three > ctrl-w.txt\n");
    });
    MCGuiWait("Ctrl+W took \"two\" back", 20, [] {
        return MCGuiReadFile(MCGuiProjectFile("ctrl-w.txt")) == "one three\n";
    });
    MCGuiStep("a file open, the editor with the keyboard", [] {
        g_signal_handler_disconnect(gT->vteWidget(), gCommitId);
        gCommitId = 0;
        MCGuiWriteFile(MCGuiProjectFile("close-me.txt"), "close me\n");
        MCGuiOpenFileKept(MCGuiProjectFile("close-me.txt"));
    });
    MCGuiWait("it is on show", 10, [] {
        return MCGuiApp().editor->currentPath() == MCGuiProjectFile("close-me.txt");
    });
    MCGuiStep("the editor takes the keyboard", [] {
        gtk_widget_grab_focus(MCGuiApp().editor->textView());
    });
    MCGuiWait("it has it", 10, [] {
        return focusWidget() == MCGuiApp().editor->textView() && !terminalHasKeyboard();
    });
    MCGuiStep("there Ctrl+W is Close Tab", [] {
        GCHECK(!accelsFor("<Control>w").empty());
        gTabs = MCGuiApp().editor->tabCount();
        MCGuiKey("ctrl+w");
    });
    MCGuiWait("the tab closed", 10, [] {
        return MCGuiApp().editor->tabCount() == gTabs - 1 &&
               MCGuiApp().editor->currentPath() != MCGuiProjectFile("close-me.txt");
    });

    // ---- what VTE sends for Ctrl with punctuation, the setting off
    MCGuiStep("back in the terminal, VTE's commits recorded", [] {
        gT->focus();
        recordCommits();
    });
    MCGuiWait("the terminal has the keyboard again", 10, terminalHasKeyboard);
    MCGuiWait("at the prompt", 20, [] { return ready(gT); });
    MCGuiStep("a raw reader for Ctrl with readline's punctuation", [] {
        g_remove(MCGuiProjectFile("vte-punct.bin").c_str());
        MCGuiType("stty raw -echo -iexten; head -c " + std::to_string(std::size(kPunct)) +
                  " > vte-punct.bin; stty sane\n");
    });
    MCGuiWait("it is reading", 20, [] {
        return MCGuiFileExists(MCGuiProjectFile("vte-punct.bin")) && gT->shellState().busy;
    });
    MCGuiStep("Ctrl+[, ], \\, @, _, ^, Space, /, 3 to 8 pressed", [] {
        gCommitted.clear();
        for (const Punct& p : kPunct) MCGuiKey(std::string("ctrl+") + p.key);
    });
    MCGuiWait("the program read them all", 20, [] {
        return MCGuiReadFile(MCGuiProjectFile("vte-punct.bin")).size() == std::size(kPunct);
    });
    MCGuiStep("VTE sent terminalControlByte's bytes", [] {
        const std::string got = MCGuiReadFile(MCGuiProjectFile("vte-punct.bin"));
        if (got != punctBytes())
            MCGuiNote("VTE sent %s, the core says %s", hex(got).c_str(), hex(punctBytes()).c_str());
        GCHECK(got == punctBytes());
        GCHECK(gCommitted == punctBytes());
    });

    // ---- terminal.mac-style-keys on
    MCGuiWait("the prompt is back", 20, [] { return ready(gT); });
    MCGuiStep("terminal.mac-style-keys = true", [] {
        gSettings = MCGuiReadFile(settingsPath());
        GCHECK(!Keys::terminalMacStyle());
        MCGuiSetSettings(gSettings + "\nterminal.mac-style-keys = true\n");
    });
    MCGuiWait("the app took it", 10, [] { return Keys::terminalMacStyle(); });
    MCGuiStep("the shortcut hints, from the terminal", [] { MCGuiKey("ctrl+shift+h"); });
    MCGuiWait("they say what Ctrl and Super do there", 10, [] {
        return gtk_widget_get_visible(MCGuiApp().hintsPanel) &&
               std::string(gtk_label_get_text(GTK_LABEL(MCGuiApp().hintsLabel)))
                       .find("Super keys go to the program as Ctrl") != std::string::npos;
    });
    MCGuiStep("hidden again", [] { MCGuiKey("ctrl+shift+h"); });
    MCGuiWait("they are", 10, [] { return !gtk_widget_get_visible(MCGuiApp().hintsPanel); });
    MCGuiStep("Ctrl keys are mutible's in the terminal", [] {
        GCHECK(terminalHasKeyboard());
        const std::string w = accelsFor("<Control>w"), f = accelsFor("<Control>f"),
                          s = accelsFor("<Control>s");
        MCGuiNote("Ctrl+W runs %s, Ctrl+F %s, Ctrl+S %s", w.c_str(), f.c_str(), s.c_str());
        GCHECK(w == "win.closetab");
        GCHECK(f == "win.find");
        GCHECK(s == "win.save");
        for (const char* k : kDropped) {
            const std::string accel = std::string("<Control>") + k[std::string(k).size() - 1];
            GCHECK(accelsFor(accel).empty());
        }
        g_remove(MCGuiProjectFile("super-keys.bin").c_str());
        const size_t n = std::size(kSuperLetters) + std::size(kPunct);
        MCGuiType("stty raw -echo -iexten; head -c " + std::to_string(n) +
                  " > super-keys.bin; stty sane\n");
    });
    MCGuiWait("a raw reader is reading", 20, [] {
        return MCGuiFileExists(MCGuiProjectFile("super-keys.bin")) && gT->shellState().busy;
    });
    MCGuiStep("Ctrl+K, U, L, Z, X, D, then Super with letters and the punctuation", [] {
        gCommitted.clear();
        for (const char* k : kDropped) MCGuiKey(k);
        for (const char* k : kSuperLetters) MCGuiKey(k);
        for (const Punct& p : kPunct) MCGuiKey(std::string("super+") + p.key);
    });
    MCGuiWait("the program read them", 20, [] {
        return MCGuiReadFile(MCGuiProjectFile("super-keys.bin")).size() == superBytes().size();
    });
    MCGuiStep("only Super's, each as its Ctrl key's byte", [] {
        const std::string got = MCGuiReadFile(MCGuiProjectFile("super-keys.bin"));
        if (got != superBytes())
            MCGuiNote("read %s, wanted %s", hex(got).c_str(), hex(superBytes()).c_str());
        GCHECK(got == superBytes());
        GCHECK(gCommitted == superBytes());
        GCHECK(MCGuiApp().editor->tabCount() == gTabs - 1);
        GCHECK(terminalHasKeyboard());
    });
    MCGuiWait("the prompt again", 20, [] { return ready(gT); });
    MCGuiStep("at the prompt: echo one two, Super+W, three", [] {
        g_remove(MCGuiProjectFile("super-w.txt").c_str());
        MCGuiType("echo one two");
        MCGuiKey("super+w");
        MCGuiType("three > super-w.txt\n");
    });
    MCGuiWait("Super+W took \"two\" back", 20, [] {
        return MCGuiReadFile(MCGuiProjectFile("super-w.txt")) == "one three\n";
    });
    MCGuiWait("the prompt", 20, [] { return ready(gT); });
    MCGuiStep("echo keep gone, five to the left, Super+Alt+K", [] {
        g_remove(MCGuiProjectFile("super-k.txt").c_str());
        MCGuiType("echo keep gone");
        for (int i = 0; i < 5; i++) MCGuiKey("left");
        MCGuiKey("super+alt+k");
        MCGuiType(" > super-k.txt\n");
    });
    MCGuiWait("Super+Alt+K killed \" gone\"", 20, [] {
        return MCGuiReadFile(MCGuiProjectFile("super-k.txt")) == "keep\n";
    });
    MCGuiWait("the prompt after it", 20, [] { return ready(gT); });
    MCGuiStep("sleep 100", [] { MCGuiType("sleep 100\n"); });
    MCGuiWait("it runs", 20, [] { return gT->shellState().busy; });
    MCGuiStep("Super+C", [] { MCGuiKey("super+c"); });
    MCGuiWait("interrupted: the prompt is back long before 100 s", 15, [] { return ready(gT); });
    MCGuiStep("echo a word to copy", [] { MCGuiType("echo copy-$((6*7))-me\n"); });
    MCGuiWait("it shows", 20, [] {
        return gT->recentText(5).find("copy-42-me") != std::string::npos && ready(gT);
    });
    MCGuiStep("the clipboard holds something else; Ctrl+A", [] {
        gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(MCGuiApp().window)), "before");
        MCGuiKey("ctrl+a");
    });
    MCGuiWait("the terminal's text is selected", 10, [] {
        return vte_terminal_get_has_selection(VTE_TERMINAL(gT->vteWidget()));
    });
    MCGuiStep("Ctrl+C", [] {
        gClip.clear();
        MCGuiKey("ctrl+c");
    });
    MCGuiWait("the clipboard has the terminal's text", 10, [] {
        readClipboard();
        return gClip.find("copy-42-me") != std::string::npos;
    });
    MCGuiStep("a command on the clipboard; Ctrl+V, then Return", [] {
        g_remove(MCGuiProjectFile("pasted.txt").c_str());
        gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(MCGuiApp().window)),
                               "echo pasted > pasted.txt");
        MCGuiKey("ctrl+v");
    });
    MCGuiWait("the paste is at the prompt", 10, [] {
        return gT->recentText(3).find("echo pasted > pasted.txt") != std::string::npos;
    });
    MCGuiStep("Return", [] { MCGuiKey("enter"); });
    MCGuiWait("the pasted command ran", 20, [] {
        return MCGuiReadFile(MCGuiProjectFile("pasted.txt")) == "pasted\n";
    });
    MCGuiStep("a second terminal tab, with the keyboard", [] {
        stopRecording();
        gTabs = MCGuiApp().editor->tabCount();
        gSecond = dock()->newTerminal();
        GCHECK(dock()->count() == 2);
    });
    MCGuiWait("its bash is at its prompt", 30, [] { return ready(gSecond); });
    MCGuiStep("it takes the keyboard", [] { gSecond->focus(); });
    MCGuiWait("it has it", 10, [] {
        return MCGuiApp().terminal == gSecond && terminalHasKeyboard();
    });
    MCGuiStep("Ctrl+W there", [] { MCGuiKey("ctrl+w"); });
    MCGuiWait("the terminal tab closed, as Cmd+W on the Mac", 10, [] {
        return dock()->count() == 1 && dock()->at(0) == gT;
    });
    MCGuiStep("no editor tab closed", [] {
        GCHECK(MCGuiApp().editor->tabCount() == gTabs);
        GCHECK(MCGuiApp().layout->terminalShown());
        gSecond = nullptr;
        MCGuiWriteFile(MCGuiProjectFile("close-me-too.txt"), "close me too\n");
        MCGuiOpenFileKept(MCGuiProjectFile("close-me-too.txt"));
    });
    MCGuiWait("a file on show", 10, [] {
        return MCGuiApp().editor->currentPath() == MCGuiProjectFile("close-me-too.txt");
    });
    MCGuiStep("the editor takes the keyboard, the setting still on", [] {
        gtk_widget_grab_focus(MCGuiApp().editor->textView());
    });
    MCGuiWait("the editor has it", 10, [] {
        return focusWidget() == MCGuiApp().editor->textView() && !terminalHasKeyboard();
    });
    MCGuiStep("Ctrl+W in the editor", [] {
        gTabs = MCGuiApp().editor->tabCount();
        MCGuiKey("ctrl+w");
    });
    MCGuiWait("the editor's tab closed", 10, [] {
        return MCGuiApp().editor->tabCount() == gTabs - 1 &&
               MCGuiApp().editor->currentPath() != MCGuiProjectFile("close-me-too.txt");
    });

    // ---- off again: today's keys
    MCGuiStep("terminal.mac-style-keys off again", [] { MCGuiSetSettings(gSettings); });
    MCGuiWait("the app took that", 10, [] { return !Keys::terminalMacStyle(); });
    MCGuiStep("the terminal takes the keyboard", [] { gT->focus(); });
    MCGuiWait("it has the keyboard", 10, terminalHasKeyboard);
    MCGuiWait("bash is at its prompt", 20, [] { return ready(gT); });
    MCGuiStep("Ctrl+W is no accelerator there; echo one two, Ctrl+W, three", [] {
        GCHECK(accelsFor("<Control>w").empty());
        g_remove(MCGuiProjectFile("off-w.txt").c_str());
        MCGuiType("echo one two");
        MCGuiKey("ctrl+w");
        MCGuiType("three > off-w.txt\n");
    });
    MCGuiWait("Ctrl+W took \"two\" back again", 20, [] {
        return MCGuiReadFile(MCGuiProjectFile("off-w.txt")) == "one three\n";
    });
    MCGuiStep("SHELL as it was", [] {
        if (gHadShell) EnvLock::envSet("SHELL", gOldShell.c_str(), TRUE);
        else EnvLock::envUnset("SHELL");
    });
}
