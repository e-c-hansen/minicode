// JsonOutlineSuite.cpp — a JSON file's outline beside its text, and notebooks
// (`./build/minicode-guitest json_outline`, linux/PARITY.md item 4.1, part two):
// the nodes of a nested file, a row chosen selecting the right text with
// non-ASCII text before it (a real click on a row), the caret choosing its row,
// an edit parsed again 0.3 s later, a broken edit keeping the old outline
// dimmed under an error line and choosing nothing, the preview key hiding and
// showing the outline, a .jsonc with comments (an error in plain .json), and a
// notebook shown read-only as its cells and outputs, with its JSON a toggle away.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "JsonOutlineView.h"
#include "Markdown.h"

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }
JsonOutlineView* ol() { return ed()->jsonOutline(); }

std::string textOf(GtkTextBuffer* b) {
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* raw = gtk_text_buffer_get_text(b, &s, &e, TRUE);
    std::string t = raw;
    g_free(raw);
    return t;
}
std::string bufferText() { return textOf(ed()->active()->buffer()); }
std::string shownText() { return textOf(gtk_text_view_get_buffer(GTK_TEXT_VIEW(ed()->textView()))); }

std::string selectedText() {
    GtkTextBuffer* b = ed()->active()->buffer();
    GtkTextIter s, e;
    if (!gtk_text_buffer_get_selection_bounds(b, &s, &e)) return "";
    char* raw = gtk_text_buffer_get_text(b, &s, &e, TRUE);
    std::string t = raw;
    g_free(raw);
    return t;
}

long nodeNamed(const std::string& label) {
    const JsonOutline::Outline& o = ol()->outline();
    for (size_t i = 0; i < o.nodes.size(); ++i)
        if (JsonOutline::label(o.nodes[i]) == label) return (long)i;
    return -1;
}

void caretAt(const std::string& needle) {
    GtkTextBuffer* b = ed()->active()->buffer();
    const std::string t = bufferText();
    const size_t at = t.find(needle);
    const long chars = g_utf8_pointer_to_offset(t.c_str(), t.c_str() + at);
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(b, &it, (int)chars);
    gtk_text_buffer_place_cursor(b, &it);
}

void insertAt(int offset, const std::string& s) {
    GtkTextBuffer* b = ed()->active()->buffer();
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(b, &it, offset);
    gtk_text_buffer_insert(b, &it, s.c_str(), -1);
}

const char* kJson =
    "{\n  \"name\": \"Zo\xC3\xAB \xF0\x9F\x98\x80\",\n  \"list\": [1, 2, {\"deep\": true}],\n"
    "  \"\xC3\xA9\": \"after\"\n}\n";

const char* kPng =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

}  // namespace

MC_GUI_SUITE(json_outline) {
    MCGuiStep("a nested JSON file with non-ASCII text before its last member", [] {
        ed()->closeAllTabs();
        MCGuiWriteFile(F("o.json"), kJson);
        MCGuiWriteFile(F("c.jsonc"), "// top\n{\"a\": 1, /* x */ \"b\": [1,],}\n");
        MCGuiWriteFile(F("c.json"), "// top\n{\"a\": 1, /* x */ \"b\": [1,],}\n");
        MCGuiWriteFile(F("plain.txt"), "no outline\n");
        MCGuiOpenFileKept(F("o.json"));
    });
    MCGuiWait("the outline on show", 10, [] {
        return ed()->active() && ed()->showsOutline() && ol() && ol()->nodeCount() > 0 &&
               gtk_widget_get_mapped(ol()->widget());
    });
    MCGuiStep("with a node for every value, and the text still editable", [] {
        GCHECK_EQ_STR(std::to_string(ol()->nodeCount()), "8");   // root, 3 members, list items, deep
        GCHECK(nodeNamed("name") > 0);
        GCHECK(nodeNamed("deep") > 0);
        GCHECK_EQ_STR(ol()->rowText(nodeNamed("name")), "name  \"Zo\xC3\xAB \xF0\x9F\x98\x80\"");
        GCHECK(!ol()->hasError() && !ol()->dimmed());
        GCHECK(gtk_text_view_get_editable(GTK_TEXT_VIEW(ed()->textView())));
        GCHECK(ed()->isSource());
    });
    MCGuiStep("a click on a row (the last member, after emoji) selects its text", [] {
        // The member's row is below the collapsed root's children only once opened.
        GCHECK(ol()->selectNode(nodeNamed("\xC3\xA9"), false));
    });
    MCGuiWait("the row on screen", 10, [] {
        return MCGuiFindLabel(ol()->widget(), "\xC3\xA9  \"after\"") != nullptr;
    });
    MCGuiStep("by a real click", [] {
        ol()->selectNode(nodeNamed("name"), false);
        GtkWidget* l = MCGuiFindLabel(ol()->widget(), "\xC3\xA9  \"after\"");
        GCHECK(l != nullptr);
        if (l) MCGuiClick(l);
    });
    MCGuiWait("the text selected", 10, [] { return selectedText() == "\"\xC3\xA9\": \"after\""; });
    MCGuiStep("with the keyboard not taken from the outline", [] {
        GCHECK_EQ_STR(selectedText(), "\"\xC3\xA9\": \"after\"");
        GCHECK(ol()->selectedNode() == nodeNamed("\xC3\xA9"));
    });
    MCGuiStep("a nested node selects its value's text", [] {
        GCHECK(ol()->selectNode(nodeNamed("deep"), true));
        GCHECK_EQ_STR(selectedText(), "\"deep\": true");
    });
    MCGuiStep("moving the caret chooses its node", [] {
        caretAt("Zo");
        GCHECK(ol()->selectedNode() == nodeNamed("name"));
        caretAt("true");
        GCHECK(ol()->selectedNode() == nodeNamed("deep"));
        caretAt("after");
        GCHECK(ol()->selectedNode() == nodeNamed("\xC3\xA9"));
    });
    MCGuiStep("an edit is parsed again 0.3 s later", [] {
        insertAt(2, "  \"added\": 1,\n");
        GCHECK(ol()->stale());
    });
    MCGuiWait("the new node", 10, [] { return !ol()->stale() && nodeNamed("added") > 0; });
    MCGuiStep("a broken edit keeps the old outline, dimmed, under an error line", [] {
        GCHECK_EQ_STR(std::to_string(ol()->nodeCount()), "9");
        insertAt(2, "  \"broken\" 2,\n");
    });
    MCGuiWait("the error", 10, [] { return ol()->hasError() && ol()->dimmed(); });
    MCGuiStep("with the old nodes and an error naming the place", [] {
        GCHECK_EQ_STR(std::to_string(ol()->nodeCount()), "9");
        GCHECK(nodeNamed("broken") < 0);
        GCHECK(ol()->error().find("Line 2") != std::string::npos);
        GCHECK(ol()->stale());
    });
    MCGuiStep("which select nothing, either way", [] {
        const std::string before = selectedText();
        GCHECK(!ol()->selectNode(0, true) || selectedText() == before);
        caretAt("true");
        ol()->selectNode(nodeNamed("added"), true);
        GCHECK_EQ_STR(selectedText().find("\"added\"") == std::string::npos ? "none" : "wrong", "none");
    });
    MCGuiStep("mending the text brings the new outline", [] {
        GtkTextBuffer* b = ed()->active()->buffer();
        GtkTextIter s, e;
        gtk_text_buffer_get_iter_at_offset(b, &s, 2);
        gtk_text_buffer_get_iter_at_offset(b, &e, 2 + 16);
        gtk_text_buffer_delete(b, &s, &e);
    });
    MCGuiWait("a clean outline", 10, [] { return !ol()->hasError() && !ol()->dimmed() && !ol()->stale(); });
    MCGuiStep("the preview key hides the outline, and shows it again", [] {
        GCHECK(ed()->showsOutline());
        ed()->togglePreview();
        GCHECK(!ed()->showsOutline());
        GCHECK(!gtk_widget_get_visible(ol()->widget()));
        ed()->togglePreview();
        GCHECK(ed()->showsOutline());
        GCHECK_EQ_STR(std::to_string(ol()->nodeCount()), "9");
    });
    MCGuiStep("a .jsonc file takes comments and trailing commas", [] { MCGuiOpenFileKept(F("c.jsonc")); });
    MCGuiWait("its outline", 10, [] {
        return ed()->active()->path() == F("c.jsonc") && ed()->showsOutline() && ol()->nodeCount() > 0;
    });
    MCGuiStep("without an error", [] {
        GCHECK(!ol()->hasError());
        GCHECK(nodeNamed("b") > 0);
        MCGuiOpenFileKept(F("c.json"));
    });
    MCGuiWait("the plain .json's", 10, [] { return ed()->active()->path() == F("c.json") && ed()->showsOutline(); });
    MCGuiStep("which does not allow them", [] {
        GCHECK(ol()->hasError());
        MCGuiOpenFileKept(F("plain.txt"));
    });
    MCGuiWait("a text file", 10, [] { return ed()->active()->path() == F("plain.txt"); });
    MCGuiStep("has no outline", [] {
        GCHECK(!ed()->showsOutline());
        GCHECK(!gtk_widget_get_visible(ol()->widget()));
    });

    MCGuiStep("a notebook: markdown, code, a stream, a result and a picture", [] {
        const std::string nb =
            "{\"nbformat\":4,\"nbformat_minor\":5,\"metadata\":{\"kernelspec\":{\"language\":\"python\","
            "\"name\":\"python3\"}},\"cells\":["
            "{\"cell_type\":\"markdown\",\"metadata\":{},\"source\":[\"# Caf\xC3\xA9 notes\\n\",\"Hello notebook\"]},"
            "{\"cell_type\":\"code\",\"execution_count\":3,\"metadata\":{},\"source\":[\"print('hi')\\n\",\"1 + 41\"],"
            "\"outputs\":["
            "{\"output_type\":\"stream\",\"name\":\"stdout\",\"text\":[\"hi out\\n\"]},"
            "{\"output_type\":\"execute_result\",\"execution_count\":3,\"metadata\":{},\"data\":{\"text/plain\":[\"42\"]}},"
            "{\"output_type\":\"display_data\",\"metadata\":{},\"data\":{\"image/png\":\"" + std::string(kPng) +
            "\",\"text/plain\":[\"<Figure>\"]}}]}]}";
        MCGuiWriteFile(F("n.ipynb"), nb);
        MCGuiOpenFileKept(F("n.ipynb"));
    });
    MCGuiWait("the rendered notebook", 10, [] {
        return ed()->active() && ed()->active()->path() == F("n.ipynb") && ed()->active()->showsPreview() &&
               shownText().find("Hello notebook") != std::string::npos;
    });
    MCGuiStep("read-only, with cells and outputs", [] {
        const std::string t = shownText();
        GCHECK(!gtk_text_view_get_editable(GTK_TEXT_VIEW(ed()->textView())));
        GCHECK(t.find("Caf\xC3\xA9 notes") != std::string::npos);
        GCHECK(t.find("print('hi')") != std::string::npos);
        GCHECK(t.find("hi out") != std::string::npos);
        GCHECK(t.find("42") != std::string::npos);
        GCHECK(!ed()->showsOutline());
        GCHECK(!ed()->active()->pageInfo().embeds.empty());   // the picture
        GCHECK(!ed()->active()->dirty());
    });
    MCGuiStep("the preview key shows its JSON, with the outline", [] { ed()->togglePreview(); });
    MCGuiWait("the source and its outline", 10, [] {
        return ed()->showsOutline() && ol() && ol()->nodeCount() > 5 && ed()->isSource();
    });
    MCGuiStep("editable, and the key goes back to the rendering", [] {
        GCHECK(gtk_text_view_get_editable(GTK_TEXT_VIEW(ed()->textView())));
        GCHECK(bufferText().find("\"cell_type\"") != std::string::npos);
        GCHECK(nodeNamed("cells") > 0);
        ed()->togglePreview();
        GCHECK(!ed()->showsOutline());
        GCHECK(shownText().find("Hello notebook") != std::string::npos);
        GCHECK(!gtk_text_view_get_editable(GTK_TEXT_VIEW(ed()->textView())));
        ed()->closeAllTabs();
    });
}
