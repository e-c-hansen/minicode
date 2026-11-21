// MarkdownAlertsSuite.cpp — GitHub's alerts in the preview
// (`./build/mutible-guitest markdown_alerts`): a note, a warning and a plain
// quote, the alerts' text upright and their titles carrying the alert's tag
// and GitHub's color, the plain quote neither.
#include "Harness.h"

#include "DocumentView.h"
#include "Editor.h"

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

GtkTextBuffer* shown() { return gtk_text_view_get_buffer(GTK_TEXT_VIEW(ed()->textView())); }

std::string shownText() {
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(shown(), &s, &e);
    char* raw = gtk_text_buffer_get_text(shown(), &s, &e, TRUE);
    std::string t = raw;
    g_free(raw);
    return t;
}

// Whether the text of `word` (its first occurrence) carries the tag `name`.
bool taggedAs(const char* name, const char* word) {
    const std::string t = shownText();
    const size_t at = t.find(word);
    GtkTextTag* tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(shown()), name);
    if (at == std::string::npos || !tag) return false;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(shown(), &it, (int)g_utf8_strlen(t.c_str(), (gssize)at));
    return gtk_text_iter_has_tag(&it, tag);
}

// The tag's foreground as 0xRRGGBB.
unsigned tagRgb(const char* name) {
    GtkTextTag* tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(shown()), name);
    GdkRGBA* c = nullptr;
    if (tag) g_object_get(tag, "foreground-rgba", &c, NULL);
    if (!c) return 0;
    const unsigned v = ((unsigned)(c->red * 255 + 0.5) << 16) | ((unsigned)(c->green * 255 + 0.5) << 8) |
                       (unsigned)(c->blue * 255 + 0.5);
    gdk_rgba_free(c);
    return v;
}

}  // namespace

MC_GUI_SUITE(markdown_alerts) {
    MCGuiStep("a note, a warning and a plain quote", [] {
        MCGuiWriteFile(F("alerts.md"),
                       "# Page\n\n> [!NOTE]\n> Remember zebra\n\n> [!WARNING]\n> Careful yak\n\n"
                       "> Plain walrus\n");
        MCGuiOpenFileKept(F("alerts.md"));
    });
    MCGuiWait("the file open", 10, [] { return ed()->active() && ed()->active()->path() == F("alerts.md"); });
    MCGuiStep("shown as a preview", [] {
        if (!ed()->inPreview()) ed()->togglePreview();
    });
    MCGuiWait("the preview", 10, [] { return shownText().find("Remember zebra") != std::string::npos; });
    MCGuiStep("titles in their tag, the plain quote in neither", [] {
        MCGuiNote("text: %s", shownText().c_str());
        GCHECK(taggedAs("md_alerttitle1", "Note"));
        GCHECK(taggedAs("md_alerttitle4", "Warning"));
        GCHECK(taggedAs("md_alert1", "Remember zebra") && !taggedAs("md_alerttitle1", "Remember zebra"));
        GCHECK(taggedAs("md_alert4", "Careful yak"));
        GCHECK(!taggedAs("md_alert1", "Plain walrus") && !taggedAs("md_alert4", "Plain walrus") &&
               !taggedAs("md_alerttitle1", "Plain walrus") && !taggedAs("md_alerttitle4", "Plain walrus"));
        GCHECK(taggedAs("md_quote", "Plain walrus") && taggedAs("md_quote", "Careful yak"));
        const unsigned note = tagRgb("md_alerttitle1"), warn = tagRgb("md_alerttitle4");
        MCGuiNote("note %06x warning %06x", note, warn);
        GCHECK(note == 0x0969DA || note == 0x4493F8);
        GCHECK(warn == 0x9A6700 || warn == 0xD29922);
        ed()->closeAllTabs();
    });
}
