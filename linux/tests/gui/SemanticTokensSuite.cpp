// SemanticTokensSuite.cpp — `./build/minicode-guitest semantic_tokens`: a
// language server's semantic tokens painted over the lexer's colors
// (editor.semantic-highlighting, linux/PARITY.md item 2.6, SemanticColors.h
// and LspSessionSemantic.cpp), against the scripted server (build/fakelsp)
// with real keys. A Python file's class, function, parameter and variable
// take the server's colors as tags on the lines on screen; its token over a
// string leaves the string's color; a line far below has none until Ctrl+End
// brings it on screen. Typing on another line keeps the colors there (they
// move with their text) and asks again 0.3 s later, by /full/delta, whose
// edit recolors a name; typing inside a name takes its line's server colors
// off at once. The server's workspace/semanticTokens/refresh (sent here
// when the file is saved) asks again. Off by the setting: every server
// color goes and nothing more is asked.
#include "CompletionChecks.h"

#include "Document.h"
#include "SemanticColors.h"
#include "Settings.h"

#include <cmath>
#include <string>

using namespace cmpcheck;

namespace {

const char* kServer = "sem";
std::string F() { return MCGuiProjectFile("sem/main.py"); }
const int kFar = 390, kLines = 400;
std::string gSettings, gBase, gSaved;
size_t gRequests = 0, gAnswers = 0;

std::string fileText() {
    std::string s = "# semantic tokens\n"
                    "class Widget:\n"
                    "    def draw(self, size):\n"
                    "        label = \"Widget\"\n"
                    "        return size\n";
    for (int k = 5; k < kFar; k++) s += "value_" + std::to_string(k) + " = " + std::to_string(k) + "\n";
    s += "far_name = Widget()\n";
    for (int k = kFar + 1; k < kLines; k++) s += "tail_" + std::to_string(k) + " = 0\n";
    return s;
}

// The tokens, five numbers each (line and start relative to the one
// before, length, type, modifiers), in the legend below: Widget (class),
// draw (function), size (parameter), label (variable), "Widget" inside a
// string (class, which the string keeps its color under), size again, and
// far below far_name (variable) and Widget (class). label's type is
// number 18.
const char* kData = "[1,6,6,0,0, 1,8,4,3,0, 0,11,4,2,0, 1,8,5,1,0, 0,9,6,0,0, 1,15,4,2,0, "
                    "386,0,8,1,0, 0,11,6,0,0]";

std::string script() {
    return std::string(
               "{\"capabilities\": {\"textDocumentSync\": {\"openClose\": true, \"change\": 1,"
               " \"save\": {\"includeText\": false}},"
               " \"semanticTokensProvider\": {\"legend\": {\"tokenTypes\": [\"class\", \"variable\","
               " \"parameter\", \"function\", \"property\"], \"tokenModifiers\": [\"readonly\"]},"
               " \"full\": {\"delta\": true}}},"
               " \"rules\": ["
               // The refresh's request, after the name was typed in: nothing changed.
               "{\"method\": \"textDocument/semanticTokens/full/delta\","
               " \"when\": {\"paramsContain\": \"\\\"previousResultId\\\":\\\"3\\\"\"},"
               " \"result\": {\"resultId\": \"4\", \"edits\": []}},"
               // label typed in (labQel): its token gone, the string's after it
               // moved one along.
               "{\"method\": \"textDocument/semanticTokens/full/delta\","
               " \"when\": {\"textContains\": \"labQel\"}, \"delayMs\": 500,"
               " \"result\": {\"resultId\": \"3\", \"edits\": [{\"start\": 15, \"deleteCount\": 10,"
               " \"data\": [1,18,6,0,0]}]}},"
               // Typing on the first line: label is a class now.
               "{\"method\": \"textDocument/semanticTokens/full/delta\", \"delayMs\": 500,"
               " \"result\": {\"resultId\": \"2\", \"edits\": [{\"start\": 18, \"deleteCount\": 1,"
               " \"data\": [0]}]}},"
               "{\"method\": \"textDocument/semanticTokens/full\","
               " \"result\": {\"resultId\": \"1\", \"data\": ") +
           kData + "}},"
                   "{\"method\": \"textDocument/didSave\","
                   " \"request\": [{\"method\": \"workspace/semanticTokens/refresh\", \"params\": null}]}"
                   "]}";
}

Document* doc() { return ed()->active(); }
SemanticColors* sem() { return doc() ? doc()->semanticColors() : nullptr; }
GtkTextView* tv() { return GTK_TEXT_VIEW(ed()->textView()); }

size_t at(int line, int column) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line_offset(buf(), &it, line, column);
    return (size_t)gtk_text_iter_get_offset(&it);
}

std::string hex(const Rgba& c) {
    char s[8];
    g_snprintf(s, sizeof s, "#%02X%02X%02X", c.r, c.g, c.b);
    return s;
}

// The color the character at `i` is drawn in, from its tags: the one of
// highest priority that sets a foreground, as #RRGGBB.
std::string colorAt(size_t i) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)i);
    GSList* tags = gtk_text_iter_get_tags(&it);   // ascending priority
    std::string out = "plain";
    for (GSList* l = tags; l; l = l->next) {
        gboolean set = FALSE;
        GdkRGBA* c = nullptr;
        g_object_get(l->data, "foreground-set", &set, "foreground-rgba", &c, NULL);
        if (set && c) {
            char h[8];
            g_snprintf(h, sizeof h, "#%02X%02X%02X", (int)std::lround(c->red * 255),
                       (int)std::lround(c->green * 255), (int)std::lround(c->blue * 255));
            out = h;
        }
        if (c) gdk_rgba_free(c);
    }
    g_slist_free(tags);
    return out;
}

// The server's style on the character at `i`, None without one.
Lsp::SemanticStyle styleAt(size_t i) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)i);
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buf());
    for (int s = 1; s <= 6; s++)
        if (gtk_text_iter_has_tag(&it, SemanticColors::tag(table, (Lsp::SemanticStyle)s)))
            return (Lsp::SemanticStyle)s;
    return Lsp::SemanticStyle::None;
}

bool onScreen(size_t offset) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)offset);
    GdkRectangle r, vis;
    gtk_text_view_get_iter_location(tv(), &it, &r);
    gtk_text_view_get_visible_rect(tv(), &vis);
    return r.y >= vis.y && r.y + r.height <= vis.y + vis.height;
}

size_t visibleLines() {
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(tv(), &vis);
    GtkTextIter a, b;
    gtk_text_view_get_line_at_y(tv(), &a, vis.y, nullptr);
    gtk_text_view_get_line_at_y(tv(), &b, vis.y + vis.height - 1, nullptr);
    return (size_t)(gtk_text_iter_get_line(&b) - gtk_text_iter_get_line(&a) + 1);
}

const Settings kTheme;
const std::string kClass = hex(kTheme.semanticColor(SemanticColor::Class));
const std::string kVariable = hex(kTheme.semanticColor(SemanticColor::Variable));
const std::string kParameter = hex(kTheme.semanticColor(SemanticColor::Parameter));
const std::string kFunction = hex(kTheme.syntax(TokenStyle::Function));
const std::string kString = hex(kTheme.syntax(TokenStyle::String));

using S = Lsp::SemanticStyle;

}  // namespace

MC_GUI_SUITE(semantic_tokens) {
    MCGuiStep("a Python file of 400 lines and a server with semantic tokens", [] {
        MCGuiSetKeyWindow(nullptr);
        ed()->closeAllTabs();
        MCGuiWriteFile(F(), fileText());
        gSettings = MCGuiReadFile(g_getenv("MINICODE_SETTINGS"));
        gBase = "lsp.enabled = true\nlsp.python = " + MCGuiFakeLsp(kServer, script()) +
                "\neditor.auto-close = off\n";
        MCGuiSetSettings(gBase);
        showFile(F());
    });
    MCGuiWait("the server's colors on the names on screen", 20, [] {
        return showing(F()) && sem() && sem()->current() && styleAt(at(3, 8)) == S::Variable;
    });
    MCGuiStep("class, function, parameter and variable; the string keeps its color", [] {
        GCHECK(styleAt(at(1, 6)) == S::Class && styleAt(at(1, 11)) == S::Class);
        GCHECK(styleAt(at(1, 5)) == S::None && styleAt(at(1, 12)) == S::None);
        GCHECK(styleAt(at(2, 8)) == S::Function);
        GCHECK(styleAt(at(2, 19)) == S::Parameter && styleAt(at(4, 15)) == S::Parameter);
        GCHECK(styleAt(at(2, 13)) == S::None);   // self: no token for it
        GCHECK_EQ_STR(colorAt(at(3, 8)), kVariable);
        GCHECK_EQ_STR(colorAt(at(2, 19)), kParameter);
        GCHECK_EQ_STR(colorAt(at(2, 8)), kFunction);
        GCHECK_EQ_STR(colorAt(at(1, 6)), kClass);
        // The server's token over "Widget" in the string: the string's color.
        GCHECK(styleAt(at(3, 17)) == S::None);
        GCHECK_EQ_STR(colorAt(at(3, 17)), kString);
        // Far below, off screen: not painted until it is seen.
        GCHECK(!onScreen(at(kFar, 0)));
        GCHECK(styleAt(at(kFar, 0)) == S::None && !sem()->painted(kFar));
        GCHECK(sem()->spanCount() == 8);   // the string's token kept, painted around
        MCGuiNote("%zu lines painted (%zu on screen), %.2f ms of CPU; %zu request(s)",
                  sem()->linesPainted(), visibleLines(), SemanticColors::cpuMs(),
                  lsp()->semanticRequests());
        GCHECK(sem()->linesPainted() <= visibleLines() + 2);
        GCHECK(lsp()->semanticRequests() >= 1 && lsp()->semanticDeltaRequests() == 0);
        GCHECK(MCGuiFakeLspReceived(kServer, "textDocument/semanticTokens/full", "main.py") >= 1);
    });
    MCGuiStep("Ctrl+End", [] { MCGuiKey("ctrl+end"); });
    MCGuiWait("the last lines on screen, and the far line painted as it came", 10, [] {
        return onScreen(at(kFar, 0)) && styleAt(at(kFar, 0)) == S::Variable &&
               styleAt(at(kFar, 11)) == S::Class;
    });
    MCGuiStep("Ctrl+Home", [] { MCGuiKey("ctrl+home"); });
    MCGuiWait("back at the top", 10, [] { return onScreen(0) && !onScreen(at(kFar, 0)); });

    MCGuiStep("typing at the end of the first line", [] {
        gRequests = lsp()->semanticRequests();
        gAnswers = lsp()->semanticAnswers();
        MCGuiKey("end");
        MCGuiType(" x");
    });
    MCGuiWait("typed", 5, [] { return text().rfind("# semantic tokens x\n", 0) == 0; });
    MCGuiStep("the tokens end; the colors on the other lines stay", [] {
        GCHECK(!sem()->current());
        GCHECK(styleAt(at(3, 8)) == S::Variable && styleAt(at(2, 19)) == S::Parameter);
        GCHECK_EQ_STR(colorAt(at(3, 8)), kVariable);
        GCHECK(lsp()->semanticAnswers() == gAnswers);
    });
    MCGuiWait("0.3 s later asked by /full/delta, and its edit applied", 10, [] {
        return lsp()->semanticDeltaRequests() == 1 && lsp()->semanticAnswers() == gAnswers + 1 &&
               styleAt(at(3, 8)) == S::Class;
    });
    MCGuiStep("label a class now; the rest as it was", [] {
        GCHECK(sem()->current());
        GCHECK_EQ_STR(colorAt(at(3, 8)), kClass);
        GCHECK(styleAt(at(2, 19)) == S::Parameter && styleAt(at(3, 17)) == S::None);
        GCHECK(MCGuiFakeLspReceived(kServer, "textDocument/semanticTokens/full/delta") == 1);
        GCHECK(lsp()->semanticRequests() == gRequests + 1);
    });

    MCGuiStep("typing inside label", [] {
        gAnswers = lsp()->semanticAnswers();
        caretAfter("lab");
        MCGuiType("Q");
    });
    MCGuiWait("typed", 5, [] { return text().find("labQel") != std::string::npos; });
    MCGuiStep("its line has the lexer's colors at once; the others keep the server's", [] {
        GCHECK(!sem()->current());
        GCHECK(styleAt(at(3, 8)) == S::None && styleAt(at(3, 11)) == S::None);
        GCHECK(colorAt(at(3, 8)) != kClass && colorAt(at(3, 8)) != kVariable);
        GCHECK_EQ_STR(colorAt(at(3, 18)), kString);
        GCHECK(styleAt(at(2, 19)) == S::Parameter && styleAt(at(1, 6)) == S::Class);
        GCHECK(lsp()->semanticAnswers() == gAnswers);
    });
    MCGuiWait("the next answer, without label's token", 10, [] {
        return lsp()->semanticDeltaRequests() == 2 && lsp()->semanticAnswers() == gAnswers + 1;
    });
    MCGuiStep("labQel plain, the string a string, size still a parameter", [] {
        GCHECK(sem()->current());
        GCHECK(styleAt(at(3, 8)) == S::None && styleAt(at(3, 18)) == S::None);
        GCHECK_EQ_STR(colorAt(at(3, 18)), kString);
        GCHECK(styleAt(at(2, 19)) == S::Parameter && styleAt(at(4, 15)) == S::Parameter);
    });

    MCGuiStep("Ctrl+S: the server says its tokens changed", [] {
        gRequests = lsp()->semanticRequests();
        gAnswers = lsp()->semanticAnswers();
        MCGuiKey("ctrl+s");
    });
    MCGuiWait("asked again on the server's refresh, and answered", 10, [] {
        return MCGuiFakeLspReceived(kServer, "textDocument/didSave", "main.py") >= 1 &&
               lsp()->semanticRequests() == gRequests + 1 &&
               lsp()->semanticAnswers() == gAnswers + 1;
    });
    MCGuiStep("nothing changed by it", [] {
        gSaved = text();
        GCHECK(styleAt(at(2, 19)) == S::Parameter && styleAt(at(3, 8)) == S::None);
    });

    MCGuiStep("editor.semantic-highlighting = off", [] {
        MCGuiSetSettings(gBase + "editor.semantic-highlighting = off\n");
    });
    MCGuiWait("every server color gone", 10, [] {
        return styleAt(at(2, 19)) == S::None && styleAt(at(1, 6)) == S::None &&
               styleAt(at(2, 8)) == S::None;
    });
    MCGuiStep("the lexer's colors; typing asks for nothing", [] {
        GCHECK(!sem()->current() && sem()->spanCount() == 0);
        GCHECK(colorAt(at(2, 19)) != kParameter);
        gRequests = lsp()->semanticRequests();
        MCGuiKey("ctrl+home");
        MCGuiType("y");
    });
    MCGuiWait("typed", 5, [] { return text().rfind("y# semantic", 0) == 0; });
    MCGuiPause(0.8);   // negative: no request comes of the edit
    MCGuiStep("no request", [] {
        GCHECK(lsp()->semanticRequests() == gRequests);
        GCHECK(styleAt(at(2, 19)) == S::None);
    });

    MCGuiStep("the servers off again; the tab closed unasked", [] {
        setText(gSaved);
        MCGuiSetSettings(gSettings);
        ed()->closeAllTabs();
    });
    MCGuiWait("no server left", 10, [] { return LspRunningServerCount() == 0; });
}
