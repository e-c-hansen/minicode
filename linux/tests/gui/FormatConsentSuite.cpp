// FormatConsentSuite.cpp — Format Document and the folder's own programs
// (`./build/minicode-guitest format_consent`, linux/PARITY.md items 2.3 and
// 2.4), the counterpart of the Mac's `format_consent` suite in
// tests/gui/FormatSuite.mm, with real keys and clicks through XTest and
// stand-in formatters (scripts that record being run):
//   - format.python = mcfmt with a copy in the folder's .venv and one
//     outside it: the one outside runs, nobody is asked;
//   - only the folder's: asked (the question names the tool and the
//     folder); Don't Allow runs nothing and is kept; asked no more; Ask
//     Again (the palette) and Allow runs the folder's copy, and it is kept;
//   - the project's black ([tool.black]) in its .venv: asked, Cancel runs
//     nothing and keeps nothing; Don't Allow looks outside the folder, and
//     the copy there formats;
//   - format.on-save: Ctrl+S asks too, and Don't Allow saves as typed;
//   - a prettier.config.js (a configuration that runs code), with Prettier
//     only outside the folder: asked, Don't Allow runs nothing at all, Allow
//     runs it; a plain .prettierrc formats unasked; the user's own command
//     (format.markdown = prettier) is asked about too, and one above the
//     window's folder is named by its whole path;
//   - format.markdown = prettier with no configuration: the user's own
//     Prettier, prose kept as written;
//   - npx prettier with the folder's own node_modules/.bin/prettier: asked,
//     Don't Allow runs nothing, Allow runs npx; without the folder's copy,
//     npx runs unasked.
#include "Harness.h"

#include "Editor.h"
#include "FolderConsent.h"
#include "FormatRunner.h"
#include "Formatting.h"
#include "Lsp.h"
#include "PalettePanel.h"

#include <glib/gstdio.h>
#include <sys/stat.h>

#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
Formatting* fmt() { return MCGuiApp().format; }
PalettePanel* pal() { return MCGuiApp().palette; }

std::string F(const std::string& name) { return MCGuiProjectFile("fmtc/" + name); }
std::string J(const std::string& name) { return MCGuiProjectFile("fmtcjs/" + name); }
std::string U(const std::string& name) { return MCGuiProjectFile("fmtup/" + name); }
std::string bin() { return MCGuiScratch() + "/fmtcbin"; }
std::string ran(const std::string& what) { return MCGuiScratch() + "/fmtc-ran-" + what; }
std::string prettierArgs() { return MCGuiScratch() + "/fmtc-prettier-args"; }
std::string answer() { return FolderConsent::answer(MCGuiProject()); }
void forget() { FolderConsent::forget(MCGuiProject()); }

void script(const std::string& path, const std::string& body) {
    MCGuiWriteFile(path, body);
    chmod(path.c_str(), 0755);
}

// A formatter that records it ran and puts spaces round x=1's "=" (and
// makes "#Title" a heading).
std::string formatter(const std::string& marker) {
    return "#!/bin/sh\n/usr/bin/touch '" + ran(marker) +
           "'\nexec /usr/bin/sed -E -e 's/^x=1$/x = 1/' -e 's/^#Title$/# Title/'\n";
}

std::string gPath;
void standInsOnPath() {
    const char* p = g_getenv("PATH");
    gPath = p ? p : "";
    g_setenv("PATH", (bin() + ":" + gPath).c_str(), TRUE);
}

std::string sourceText() {
    GtkTextBuffer* b = ed()->sourceBuffer();
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* t = gtk_text_buffer_get_text(b, &s, &e, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

// The folder's question up and laid out.
bool questionUp() {
    GtkWindow* top = MCGuiTopWindow();
    if (top == MCGuiApp().window) return false;
    GtkWidget* cancel = MCGuiFindButton(GTK_WIDGET(top), "Cancel");
    return cancel && MCGuiFindButton(GTK_WIDGET(top), "Allow") &&
           gtk_widget_get_width(cancel) > 0 && gtk_widget_get_height(cancel) > 0;
}

std::string questionText() {
    GtkWindow* top = MCGuiTopWindow();
    if (top == MCGuiApp().window) return "";
    std::string all;
    std::vector<GtkWidget*> todo = {GTK_WIDGET(top)};
    while (!todo.empty()) {
        GtkWidget* w = todo.back();
        todo.pop_back();
        if (GTK_IS_LABEL(w)) all += std::string(gtk_label_get_text(GTK_LABEL(w))) + "\n";
        for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
            todo.push_back(c);
    }
    return all;
}

std::string gAsked;
int gQuestions = 0;
int gNotes = 0;
bool newNote() { return fmt()->notesShown() > gNotes; }

// Waits for the question, keeps its words, and presses `button`.
void answerQuestion(const char* button) {
    MCGuiWait("the folder's question", 15, questionUp);
    MCGuiStep(std::string("its words; ") + button, [button] {
        gAsked = questionText();
        MCGuiNote("asked: %s", gAsked.c_str());
        MCGuiClick(MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), button));
    });
    MCGuiWait("the question goes", 10, [] { return MCGuiTopWindow() == MCGuiApp().window; });
}

bool fieldHasKeyboard() {
    GtkWidget* f = gtk_root_get_focus(GTK_ROOT(MCGuiApp().window));
    return f && (f == pal()->entry() || gtk_widget_is_ancestor(f, pal()->entry()));
}
std::string selectedLabel() {
    const std::vector<std::string> l = pal()->labels();
    const int i = pal()->selected();
    return i >= 0 && i < (int)l.size() ? l[(size_t)i] : "";
}

// Ask Again About This Folder's Tools from the palette, with real keys.
void askAgain() {
    MCGuiStep("Ctrl+Shift+P", [] { MCGuiKey("ctrl+shift+p"); });
    MCGuiWait("the palette", 10, [] { return pal()->isOpen() && fieldHasKeyboard(); });
    MCGuiStep("\"ask again\", Enter", [] { MCGuiType("ask again"); });
    MCGuiWait("ranked first", 10, [] {
        return selectedLabel() == "Edit > Ask Again About This Folder's Tools";
    });
    MCGuiStep("Enter", [] { MCGuiKey("enter"); });
    MCGuiWait("forgotten", 10, [] { return !pal()->isOpen() && answer().empty(); });
}

// Ctrl+Shift+I with the editor's keyboard.
void formatKey() {
    // The window's focus widget: after an alert there is no window manager
    // to give the window the X focus back, which the harness does before
    // the next key (MCGuiKey).
    MCGuiWait("the editor has the window's keyboard", 10, [] {
        if (MCGuiTopWindow() != MCGuiApp().window) return false;
        GtkRoot* root = GTK_ROOT(MCGuiApp().window);
        if (gtk_root_get_focus(root) != ed()->textView()) ed()->focusActiveDocument();
        return gtk_root_get_focus(root) == ed()->textView();
    });
    MCGuiStep("Ctrl+Shift+I", [] {
        gQuestions = fmt()->questionsAsked();
        gNotes = fmt()->notesShown();
        MCGuiKey("ctrl+shift+i");
    });
}

// Opens `path` on its source: a Markdown file opens on its page, and
// Ctrl+Shift+V shows the source.
void openSource(const std::string& path) {
    MCGuiStep("open " + path.substr(path.find_last_of('/') + 1), [path] { MCGuiOpenFileKept(path); });
    MCGuiWait("on show", 10, [path] {
        return ed()->currentPath() == path && (ed()->isSource() || ed()->inPreview());
    });
    MCGuiStep("its source (Ctrl+Shift+V on a page)", [] {
        if (ed()->inPreview()) MCGuiKey("ctrl+shift+v");
    });
    MCGuiWait("its source on show", 10, [path] { return ed()->currentPath() == path && ed()->isSource(); });
}

}  // namespace

MC_GUI_SUITE(format_consent) {
    MCGuiStep("mcfmt in fmtc/.venv and outside the folder; format.python = mcfmt", [] {
        forget();
        for (const char* m : {"local", "outside", "black", "blackout", "prettier", "npx"})
            g_remove(ran(m).c_str());
        MCGuiWriteFile(F("a.py"), "x=1\n");
        script(F(".venv/bin/mcfmt"), formatter("local"));
        script(bin() + "/mcfmt", formatter("outside"));
        standInsOnPath();
        MCGuiSetSettings("lsp.enabled = false\nformat.python = mcfmt\n");
    });
    openSource(F("a.py"));
    formatKey();
    MCGuiWait("formatted by the copy outside the folder", 15, [] { return sourceText() == "x = 1\n"; });
    MCGuiStep("no question, the folder's copy not run, nothing kept", [] {
        GCHECK(fmt()->questionsAsked() == gQuestions);
        GCHECK(MCGuiFileExists(ran("outside")));
        GCHECK(!MCGuiFileExists(ran("local")));
        GCHECK(answer().empty());
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("undone", 10, [] { return sourceText() == "x=1\n"; });
    MCGuiStep("only the folder has it", [] { g_remove((bin() + "/mcfmt").c_str()); });
    formatKey();
    answerQuestion("Don't Allow");
    MCGuiWait("a note says why", 15, [] {
        return newNote() && fmt()->lastNote().find("not allowed") != std::string::npos;
    });
    MCGuiStep("the question named the tool and its folder; nothing ran; deny kept", [] {
        MCGuiNote("the note: %s", fmt()->lastNote().c_str());
        GCHECK(gAsked.find("Run mcfmt from this folder's fmtc/.venv?") != std::string::npos);
        GCHECK(gAsked.find("Programs in the folder run with your rights") != std::string::npos);
        GCHECK(fmt()->questionsAsked() == gQuestions + 1);
        GCHECK_EQ_STR(fmt()->lastNote(),
                      "mcfmt is installed only in this folder's fmtc/.venv, and this folder's own "
                      "programs are not allowed to run. Install mcfmt yourself to use it here.");
        GCHECK(!MCGuiFileExists(ran("local")));
        GCHECK(sourceText() == "x=1\n");
        GCHECK_EQ_STR(answer(), "deny");
    });
    formatKey();
    MCGuiWait("the note again at once", 15, [] { return newNote() && fmt()->noteShown(); });
    MCGuiStep("not asked again; still nothing ran", [] {
        GCHECK(fmt()->questionsAsked() == gQuestions);
        GCHECK(MCGuiTopWindow() == MCGuiApp().window);
        GCHECK(!MCGuiFileExists(ran("local")));
    });
    askAgain();
    formatKey();
    answerQuestion("Allow");
    MCGuiWait("Allow: the folder's copy formats", 15, [] { return sourceText() == "x = 1\n"; });
    MCGuiStep("asked once, the folder's copy ran, allow kept", [] {
        GCHECK(fmt()->questionsAsked() == gQuestions + 1);
        GCHECK(MCGuiFileExists(ran("local")));
        GCHECK_EQ_STR(answer(), "allow");
        g_remove(ran("local").c_str());
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("undone", 10, [] { return sourceText() == "x=1\n"; });
    formatKey();
    MCGuiWait("a second format runs it without asking", 15, [] { return sourceText() == "x = 1\n"; });
    MCGuiStep("the answer stands", [] {
        GCHECK(fmt()->questionsAsked() == gQuestions);
        GCHECK(MCGuiFileExists(ran("local")));
        g_remove(ran("local").c_str());
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("undone", 10, [] { return sourceText() == "x=1\n"; });

    // The project chooses Black and has it in its .venv.
    MCGuiStep("[tool.black] and fmtc/.venv/bin/black; no answer", [] {
        forget();
        MCGuiWriteFile(F("pyproject.toml"), "[tool.black]\nline-length = 88\n");
        script(F(".venv/bin/black"), formatter("black"));
        MCGuiSetSettings("lsp.enabled = false\n");
    });
    formatKey();
    answerQuestion("Cancel");
    MCGuiPause(0.5);   // negative check: black must not start
    MCGuiStep("Cancel: black not run, nothing kept, the buffer as it was", [] {
        GCHECK(gAsked.find("Run black from this folder's fmtc/.venv?") != std::string::npos);
        GCHECK(!MCGuiFileExists(ran("black")));
        GCHECK(answer().empty());
        GCHECK(sourceText() == "x=1\n");
        // Black installed outside the folder too.
        script(bin() + "/black", formatter("blackout"));
    });
    formatKey();
    answerQuestion("Don't Allow");
    MCGuiWait("Don't Allow looks outside the folder: that black formats", 15,
              [] { return sourceText() == "x = 1\n"; });
    MCGuiStep("the folder's black never ran; deny kept", [] {
        GCHECK(MCGuiFileExists(ran("blackout")));
        GCHECK(!MCGuiFileExists(ran("black")));
        GCHECK_EQ_STR(answer(), "deny");
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("undone", 10, [] { return sourceText() == "x=1\n"; });

    // format.on-save: Ctrl+S asks too.
    MCGuiStep("format.python = mcfmt, format.on-save; no answer; a line typed", [] {
        forget();
        g_remove(F("pyproject.toml").c_str());
        MCGuiSetSettings("lsp.enabled = false\nformat.python = mcfmt\nformat.on-save = true\n");
        MCGuiKey("ctrl+end");
        MCGuiType("y=2\n");
    });
    MCGuiWait("typed", 10, [] { return sourceText() == "x=1\ny=2\n"; });
    MCGuiStep("Ctrl+S", [] {
        gQuestions = fmt()->questionsAsked();
        MCGuiKey("ctrl+s");
    });
    answerQuestion("Don't Allow");
    MCGuiWait("saved as typed", 15, [] { return MCGuiReadFile(F("a.py")) == "x=1\ny=2\n"; });
    MCGuiStep("nothing from the folder ran", [] {
        GCHECK(fmt()->questionsAsked() == gQuestions + 1);
        GCHECK(!MCGuiFileExists(ran("local")));
        GCHECK_EQ_STR(answer(), "deny");
        if (Document* d = ed()->documentFor(F("a.py"))) ed()->closeTab(d);
        MCGuiSetSettings("lsp.enabled = false\n");
    });

    // A configuration that runs code, with Prettier outside the folder.
    MCGuiStep("prettier.config.js in fmtcjs, prettier outside the folder; no answer", [] {
        forget();
        MCGuiWriteFile(J("a.js"), "x=1\n");
        MCGuiWriteFile(J("prettier.config.js"), "module.exports = {};\n");
        script(bin() + "/prettier", "#!/bin/sh\n/usr/bin/touch '" + ran("prettier") +
                                        "'\nprintf '%s\\n' \"$@\" > '" + prettierArgs() +
                                        "'\nexec /usr/bin/sed -E -e 's/^x=1$/x = 1/' "
                                        "-e 's/^#Title$/# Title/'\n");
    });
    openSource(J("a.js"));
    formatKey();
    answerQuestion("Don't Allow");
    MCGuiWait("a note: it can run code", 15, [] {
        return newNote() && fmt()->lastNote().find("can run code") != std::string::npos;
    });
    MCGuiStep("the question named the file; prettier never ran; deny kept", [] {
        MCGuiNote("the note: %s", fmt()->lastNote().c_str());
        GCHECK(gAsked.find("Let prettier run the code in this folder's fmtcjs/prettier.config.js?") !=
               std::string::npos);
        GCHECK(fmt()->lastNote().find("fmtcjs/prettier.config.js can run code when prettier reads "
                                      "it") == 0);
        GCHECK(fmt()->lastNote().find("(Edit menu)") != std::string::npos);
        GCHECK(!MCGuiFileExists(ran("prettier")));
        GCHECK(sourceText() == "x=1\n");
        GCHECK_EQ_STR(answer(), "deny");
    });
    formatKey();
    MCGuiWait("the note again at once", 15, [] { return newNote() && fmt()->noteShown(); });
    MCGuiStep("not asked again; still not run; then forgotten", [] {
        GCHECK(fmt()->questionsAsked() == gQuestions);
        GCHECK(!MCGuiFileExists(ran("prettier")));
        forget();
    });
    formatKey();
    answerQuestion("Allow");
    MCGuiWait("Allow: prettier formats", 15, [] { return sourceText() == "x = 1\n"; });
    MCGuiStep("allow kept; a plain .prettierrc instead, and no answer", [] {
        GCHECK(MCGuiFileExists(ran("prettier")));
        GCHECK_EQ_STR(answer(), "allow");
        MCGuiKey("ctrl+z");
        g_remove(ran("prettier").c_str());
        g_remove(J("prettier.config.js").c_str());
        MCGuiWriteFile(J(".prettierrc"), "{\"semi\": false}\n");
        forget();
    });
    MCGuiWait("undone", 10, [] { return sourceText() == "x=1\n"; });
    formatKey();
    MCGuiWait("a plain .prettierrc: formatted unasked", 15, [] { return sourceText() == "x = 1\n"; });
    MCGuiStep("no question, nothing kept", [] {
        GCHECK(fmt()->questionsAsked() == gQuestions);
        GCHECK(answer().empty());
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("undone", 10, [] { return sourceText() == "x=1\n"; });

    // The user's own command is asked about too, for a configuration that
    // runs code.
    MCGuiStep("format.markdown = prettier, and a prettier.config.js beside a README", [] {
        g_remove(J(".prettierrc").c_str());
        MCGuiWriteFile(J("prettier.config.js"), "module.exports = {};\n");
        MCGuiWriteFile(J("notes.md"), "#Title\n");
        g_remove(ran("prettier").c_str());
        MCGuiSetSettings("lsp.enabled = false\nformat.markdown = prettier\n");
    });
    openSource(J("notes.md"));
    formatKey();
    answerQuestion("Don't Allow");
    MCGuiWait("a note: it can run code", 15, [] {
        return newNote() && fmt()->lastNote().find("can run code") != std::string::npos;
    });
    MCGuiStep("prettier never ran; then the configuration goes, and no answer", [] {
        GCHECK(gAsked.find("fmtcjs/prettier.config.js") != std::string::npos);
        GCHECK(!MCGuiFileExists(ran("prettier")));
        GCHECK(sourceText() == "#Title\n");
        g_remove(J("prettier.config.js").c_str());
        forget();
    });
    formatKey();
    MCGuiWait("the user's own Prettier formats the README", 15, [] { return sourceText() == "# Title\n"; });
    MCGuiStep("unasked, prose kept as written", [] {
        const std::string given = MCGuiReadFile(prettierArgs());
        MCGuiNote("prettier's arguments: %s", given.c_str());
        GCHECK(given.find("--prose-wrap\npreserve\n") != std::string::npos);
        GCHECK(given.find("--stdin-filepath\n" + J("notes.md") + "\n") != std::string::npos);
        GCHECK(fmt()->questionsAsked() == gQuestions);
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("undone", 10, [] { return sourceText() == "#Title\n"; });
    MCGuiStep("close the two tabs", [] {
        for (const char* name : {"notes.md", "a.js"})
            if (Document* d = ed()->documentFor(J(name))) ed()->closeTab(d);
    });

    // Above the window's folder, and npx with the folder's own Prettier.
    MCGuiStep("a prettier.config.js above the window's folder", [] {
        forget();
        MCGuiWriteFile(U("README.md"), "#Title\n");
        MCGuiWriteFile(MCGuiScratch() + "/prettier.config.js", "module.exports = {};\n");
        g_remove(ran("prettier").c_str());
    });
    openSource(U("README.md"));
    formatKey();
    answerQuestion("Don't Allow");
    MCGuiWait("a note: it can run code", 15, [] {
        return newNote() && fmt()->lastNote().find("can run code") != std::string::npos;
    });
    MCGuiStep("named by its whole path, above this folder; prettier never ran", [] {
        GCHECK(gAsked.find("Let prettier run the code in " + MCGuiScratch() +
                           "/prettier.config.js, above this folder?") != std::string::npos);
        GCHECK(!MCGuiFileExists(ran("prettier")));
        GCHECK_EQ_STR(answer(), "deny");
        g_remove((MCGuiScratch() + "/prettier.config.js").c_str());
        forget();
        script(bin() + "/npx", formatter("npx"));
        script(U("node_modules/.bin/prettier"), formatter("prettier"));
        MCGuiSetSettings("lsp.enabled = false\n"
                         "format.markdown = npx prettier --stdin-filepath {file}\n");
    });
    formatKey();
    answerQuestion("Don't Allow");
    MCGuiWait("a note: npx would run the folder's own", 15, [] {
        return newNote() && fmt()->lastNote().find("would run this folder's own") != std::string::npos;
    });
    MCGuiStep("the question named prettier and node_modules; neither ran", [] {
        GCHECK(gAsked.find("Run prettier from this folder's fmtup/node_modules?") !=
               std::string::npos);
        GCHECK(!MCGuiFileExists(ran("npx")) && !MCGuiFileExists(ran("prettier")));
        GCHECK(sourceText() == "#Title\n");
        forget();
    });
    formatKey();
    answerQuestion("Allow");
    MCGuiWait("Allow: npx formats", 15, [] { return sourceText() == "# Title\n"; });
    MCGuiStep("npx ran; without the folder's copy it runs unasked", [] {
        GCHECK(MCGuiFileExists(ran("npx")));
        MCGuiKey("ctrl+z");
        g_remove(ran("npx").c_str());
        g_remove(U("node_modules/.bin/prettier").c_str());
        forget();
    });
    MCGuiWait("undone", 10, [] { return sourceText() == "#Title\n"; });
    formatKey();
    MCGuiWait("npx alone formats unasked", 15, [] { return sourceText() == "# Title\n"; });
    MCGuiStep("no question, nothing kept; put everything back", [] {
        GCHECK(fmt()->questionsAsked() == gQuestions);
        GCHECK(answer().empty());
        GCHECK(MCGuiFileExists(ran("npx")));
        MCGuiKey("ctrl+z");
        MCGuiSetSettings("lsp.enabled = false\n");
        if (Document* d = ed()->documentFor(U("README.md"))) ed()->closeTab(d);
        g_setenv("PATH", gPath.c_str(), TRUE);
        forget();
    });
}
