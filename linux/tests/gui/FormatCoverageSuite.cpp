// FormatCoverageSuite.cpp — which files Format Document formats, and with
// what (`./build/minicode-guitest format_coverage`, linux/PARITY.md item
// 2.4), the counterpart of the Mac's `format_coverage` suite in
// tests/gui/FormatSuite.mm, with real keys through XTest, a stand-in
// Prettier on PATH and the scripted language server (build/fakelsp):
//   - a README in a project set up for Prettier is formatted by Prettier,
//     told the file, and its language server is never asked;
//   - with Prettier configured but not installed, the note names the
//     project's configuration, and the server is still not asked: it would
//     format in a style the project never chose;
//   - with no Prettier set up, the file's server formats it
//     (textDocument/formatting, with the file's indentation) as one undo
//     step, unsaved;
//   - a kind of file minicode has no formatter for (lsp.notes with
//     lsp.notes.extensions = notes) is formatted by its server, also
//     before Ctrl+S writes it with format.on-save.
#include "Harness.h"

#include "Editor.h"
#include "FakeLsp.h"
#include "Formatting.h"
#include "Lsp.h"

#include <glib/gstdio.h>
#include <sys/stat.h>

#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
Formatting* fmt() { return MCGuiApp().format; }

std::string F(const std::string& name) { return MCGuiProjectFile("fmtcov/" + name); }
std::string bin() { return MCGuiScratch() + "/fmtcovbin"; }
std::string args() { return MCGuiScratch() + "/fmtcov-prettier-args"; }

void script(const std::string& path, const std::string& body) {
    MCGuiWriteFile(path, body);
    chmod(path.c_str(), 0755);
}

std::string gPath, gMd, gNotesServer;
int gNotes = 0;

std::string sourceText() {
    GtkTextBuffer* b = ed()->sourceBuffer();
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* t = gtk_text_buffer_get_text(b, &s, &e, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

bool dirty() { return ed()->active() && ed()->active()->dirty(); }
bool editorHasKeyboard() { return gtk_widget_has_focus(ed()->textView()); }

std::string edit(int line, int from, int to, const std::string& text) {
    auto pos = [](int l, int c) {
        return "{\"line\": " + std::to_string(l) + ", \"character\": " + std::to_string(c) + "}";
    };
    return "{\"range\": {\"start\": " + pos(line, from) + ", \"end\": " + pos(line, to) +
           "}, \"newText\": \"" + text + "\"}";
}

std::string server(const std::string& result) {
    return "{\"capabilities\": {\"textDocumentSync\": 1, \"documentFormattingProvider\": true},"
           " \"rules\": [{\"method\": \"textDocument/formatting\", \"result\": [" +
           result + "]}]}";
}

// The params of every textDocument/formatting the server was sent.
std::vector<Json> formattingAsked(const std::string& name) {
    std::vector<Json> out;
    const std::string text = MCGuiReadFile(MCGuiFakeLspDir() + "/" + name + ".log");
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        Json m;
        if (Json::parse(text.substr(at, end - at), m) &&
            m["method"].asString() == "textDocument/formatting")
            out.push_back(m["params"]);
        at = end + 1;
    }
    return out;
}

void openSource(const std::string& path) {
    MCGuiStep("open " + path.substr(path.find_last_of('/') + 1), [path] { MCGuiOpenFileKept(path); });
    MCGuiWait("on show", 10, [path] {
        return ed()->currentPath() == path && (ed()->isSource() || ed()->inPreview());
    });
    MCGuiStep("its source (Ctrl+Shift+V on a page)", [] {
        if (ed()->inPreview()) MCGuiKey("ctrl+shift+v");
    });
    MCGuiWait("its source on show, with the keyboard", 10, [path] {
        if (ed()->currentPath() == path && ed()->isSource() && !editorHasKeyboard())
            ed()->focusActiveDocument();
        return ed()->currentPath() == path && ed()->isSource() && editorHasKeyboard();
    });
}

void formatKey() {
    MCGuiStep("Ctrl+Shift+I", [] {
        gNotes = fmt()->notesShown();
        MCGuiKey("ctrl+shift+i");
    });
}

}  // namespace

MC_GUI_SUITE(format_coverage) {
    MCGuiStep("a README with a .prettierrc, Prettier on PATH, servers for .md and .notes", [] {
        MCGuiWriteFile(F("README.md"), "#  Title\n");
        MCGuiWriteFile(F(".prettierrc"), "{}\n");
        script(bin() + "/prettier", "#!/bin/sh\nprintf '%s\\n' \"$@\" > '" + args() +
                                        "'\nexec /usr/bin/sed -E 's/^#  /# /'\n");
        const char* p = g_getenv("PATH");
        gPath = p ? p : "";
        g_setenv("PATH", (bin() + ":" + gPath).c_str(), TRUE);
        gMd = MCGuiFakeLsp("fmtmd", server(edit(0, 1, 2, "")));
        gNotesServer = MCGuiFakeLsp("fmtnotes", server(edit(0, 1, 1, " ")));
        MCGuiFakeLspClearLog("fmtmd");
        MCGuiFakeLspClearLog("fmtnotes");
        MCGuiSetSettings("lsp.enabled = true\nlsp.markdown = " + gMd + "\nlsp.notes = " +
                         gNotesServer + "\nlsp.notes.extensions = notes\n");
    });
    openSource(F("README.md"));
    MCGuiWait("the Markdown server has the file", 20, [] {
        return MCGuiFakeLspReceived("fmtmd", "textDocument/didOpen", "README.md") > 0 &&
               MCGuiApp().lsp->canFormatDocument();
    });
    formatKey();
    MCGuiWait("formatted by the project's Prettier", 15, [] { return sourceText() == "# Title\n"; });
    MCGuiStep("Prettier was told the file; the server was not asked", [] {
        const std::string given = MCGuiReadFile(args());
        MCGuiNote("prettier's arguments: %s", given.c_str());
        GCHECK(given.find("--stdin-filepath\n" + F("README.md") + "\n") != std::string::npos);
        GCHECK(formattingAsked("fmtmd").empty());
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("undone", 10, [] { return sourceText() == "#  Title\n"; });
    MCGuiStep("Prettier configured but not installed", [] {
        g_remove((bin() + "/prettier").c_str());
    });
    formatKey();
    MCGuiWait("the note names the project's Prettier", 15, [] {
        return fmt()->notesShown() > gNotes &&
               fmt()->lastNote().find("not installed") != std::string::npos;
    });
    MCGuiStep("the server never stands in for it", [] {
        MCGuiNote("the note: %s", fmt()->lastNote().c_str());
        GCHECK_EQ_STR(fmt()->lastNote(),
                      "This project configures prettier (in .prettierrc), which is not installed.");
        GCHECK(formattingAsked("fmtmd").empty());
        GCHECK(sourceText() == "#  Title\n");
        g_remove(F(".prettierrc").c_str());
    });
    formatKey();
    MCGuiWait("no Prettier set up: formatted by the Markdown server", 15,
              [] { return sourceText() == "# Title\n"; });
    MCGuiStep("asked once with the file's indentation; one undo step, unsaved", [] {
        const std::vector<Json> asked = formattingAsked("fmtmd");
        GCHECK(asked.size() == 1);
        if (asked.size() == 1) {
            const Json& p = asked[0];
            const std::string uri = p["textDocument"]["uri"].asString();
            GCHECK(uri.size() > 9 && uri.compare(uri.size() - 9, 9, "README.md") == 0);
            GCHECK(p["options"]["tabSize"].asNumber() > 0);
            GCHECK(p["options"]["insertSpaces"].isBool());
        }
        GCHECK_EQ_STR(MCGuiReadFile(F("README.md")), "#  Title\n");
        GCHECK(dirty());
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("one Ctrl+Z takes it back, clean again", 10,
              [] { return sourceText() == "#  Title\n" && !dirty(); });

    // A kind of file minicode has no formatter for.
    MCGuiStep("a.notes, its server named in the settings", [] {
        MCGuiWriteFile(F("a.notes"), "a=1\n");
    });
    openSource(F("a.notes"));
    MCGuiWait("its server has it", 20, [] {
        return MCGuiFakeLspReceived("fmtnotes", "textDocument/didOpen", "a.notes") > 0 &&
               MCGuiApp().lsp->canFormatDocument();
    });
    formatKey();
    MCGuiWait("formatted by its server", 15, [] { return sourceText() == "a =1\n"; });
    MCGuiStep("one undo; then format.on-save, a line, Ctrl+S", [] {
        GCHECK(formattingAsked("fmtnotes").size() == 1);
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("undone", 10, [] { return sourceText() == "a=1\n"; });
    MCGuiStep("format.on-save, a line typed", [] {
        MCGuiSetSettings("lsp.enabled = true\nlsp.markdown = " + gMd + "\nlsp.notes = " +
                         gNotesServer + "\nlsp.notes.extensions = notes\nformat.on-save = true\n");
        MCGuiKey("ctrl+end");
        MCGuiType("b=2\n");
    });
    MCGuiWait("typed, and the server has it again", 20, [] {
        return sourceText() == "a=1\nb=2\n" && MCGuiApp().lsp->canFormatDocument();
    });
    MCGuiStep("Ctrl+S", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("formatted by its server, then saved", 15, [] {
        return MCGuiReadFile(F("a.notes")) == "a =1\nb=2\n" && !dirty();
    });
    MCGuiStep("put everything back", [] {
        GCHECK(sourceText() == "a =1\nb=2\n");
        MCGuiSetSettings("lsp.enabled = false\n");
        for (const char* name : {"a.notes", "README.md"})
            if (Document* d = ed()->documentFor(F(name))) ed()->closeTab(d);
        g_setenv("PATH", gPath.c_str(), TRUE);
    });
}
