// ZoomSuite.cpp — the editor's zoom and the terminals' (`./build/minicode-guitest
// zoom`, linux/PARITY.md item 1.11), the counterparts of the Mac's
// terminal_zoom (tests/gui/LooseEndsSuite.mm) and zoomfit's zoom steps
// (tests/gui/ZoomFitSuite.mm), with real keys through XTest: Ctrl+= and
// Ctrl+Shift+= zoom the editor in a point at a time, Ctrl+- out, Ctrl+Alt+0
// and Ctrl+keypad 0 back, the text and the gutter growing and shrinking
// with it and the Markdown preview's sizes scaled; the zoom lasts into a
// second app through $XDG_STATE_HOME/minicode/zoom.json and never touches
// the settings file; with a PDF on show the keys zoom it and Ctrl+0 still
// focuses the tree, and with the browser's keyboard they zoom its page;
// with the terminal's keyboard the same keys zoom only the terminals (the
// shell sees the new width), while plain Ctrl keys, Alt+Z and Ctrl+Shift+-
// still reach the program there.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "FileTree.h"
#include "Keys.h"
#include "MediaView.h"
#include "NavHistoryUi.h"
#include "ViewZoom.h"
#include "WindowLayout.h"
#include "Zoom.h"

#ifdef MINICODE_ENABLE_PDF
#include "PdfView.h"
#include <cairo-pdf.h>
#endif
#ifdef MINICODE_ENABLE_TERMINAL
#include "Terminal.h"
#include <vte/vte.h>
#endif
#ifdef MINICODE_ENABLE_BROWSER
#include "Browser.h"
#endif

#include <cmath>
#include <string>

namespace {

std::string F(const std::string& name) { return MCGuiProjectFile(name); }
Editor* ed() { return MCGuiApp().editor; }
GtkWidget* view() { return ed()->textView(); }
GtkWidget* focusWidget() { return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)); }
bool editorHasKeyboard() { return gtk_widget_has_focus(view()); }

// A widget's font size in pixels, as Pango was given it.
double fontPx(GtkWidget* w) {
    const PangoFontDescription* fd = pango_context_get_font_description(gtk_widget_get_pango_context(w));
    if (!fd) return 0;
    const double size = (double)pango_font_description_get_size(fd) / PANGO_SCALE;
    return pango_font_description_get_size_is_absolute(fd) ? size : size * 96.0 / 72.0;
}
double px(double points) { return points * 96.0 / 72.0; }
bool near(double a, double b, double within = 0.05) { return std::fabs(a - b) <= within; }

GtkWidget* gutter() { return gtk_text_view_get_gutter(GTK_TEXT_VIEW(view()), GTK_TEXT_WINDOW_LEFT); }
int gutterWidth() { return gutter() ? gtk_widget_get_width(gutter()) : 0; }

// The first character's box in the view.
int charWidth() {
    GtkTextIter it;
    gtk_text_buffer_get_start_iter(ed()->buffer(), &it);
    GdkRectangle r;
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(view()), &it, &r);
    return r.width;
}

double tagPoints(const char* name) {
    GtkTextTag* tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(ed()->buffer()), name);
    double pt = 0;
    if (tag) g_object_get(tag, "size-points", &pt, nullptr);
    return pt;
}

std::string settingsText() { return MCGuiReadFile(g_getenv("MINICODE_SETTINGS")); }

#ifdef MINICODE_ENABLE_TERMINAL
VteTerminal* findVte(GtkWidget* root) {
    if (!root) return nullptr;
    if (VTE_IS_TERMINAL(root)) return VTE_TERMINAL(root);
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (VteTerminal* hit = findVte(c)) return hit;
    return nullptr;
}
VteTerminal* vte() { return findVte(MCGuiApp().terminal->widget()); }
std::string terminalText() {
    char* text = vte() ? vte_terminal_get_text_format(vte(), VTE_FORMAT_TEXT) : nullptr;
    std::string out = text ? text : "";
    g_free(text);
    return out;
}
bool terminalHasKeyboard() { return MCGuiApp().terminal->owns(focusWidget()); }
long gCols = 0, gCellH = 0;
#endif

double gPx0 = 0;     // the editor's font, unzoomed
int gGutter0 = 0, gChar0 = 0;
double gPdfZoom = 0;
std::string gSettings;

}  // namespace

MC_GUI_SUITE(zoom) {
    MCGuiStep("files, both zooms at none, and the editor with the keyboard", [] {
        std::string text;
        for (int i = 1; i <= 120; i++) text += "zoom line " + std::to_string(i) + "\n";
        MCGuiWriteFile(F("zoom.txt"), text);
        MCGuiWriteFile(F("zoom.md"), "# Title\n\nBody text here.\n\nSome `code` too.\n");
#ifdef MINICODE_ENABLE_PDF
        cairo_surface_t* pdf = cairo_pdf_surface_create(F("zoom.pdf").c_str(), 300, 400);
        cairo_t* pc = cairo_create(pdf);
        cairo_show_page(pc);
        cairo_destroy(pc);
        cairo_surface_destroy(pdf);
#endif
        Zoom::zoomEditor(0);
        Zoom::zoomTerminal(0);
        gSettings = settingsText();
        MCGuiOpenFileKept(F("zoom.txt"));
    });
    MCGuiWait("it shows", 10, [] { return ed()->currentPath() == F("zoom.txt") && gutterWidth() > 0; });
    MCGuiStep("a click in it", [] { MCGuiClick(view()); });
    MCGuiWait("the editor has the keyboard", 10, editorHasKeyboard);
    MCGuiStep("at the desktop's size with no zoom", [] {
        gPx0 = fontPx(view());
        gGutter0 = gutterWidth();
        gChar0 = charWidth();
        MCGuiNote("unzoomed: %.2f px (the desktop's %.1f pt), gutter %d px, a character %d px",
                  gPx0, Zoom::desktopPoints(), gGutter0, gChar0);
        GCHECK(near(gPx0, px(Zoom::desktopPoints())));
        GCHECK(Zoom::editorZoom() == 0 && Zoom::terminalZoom() == 0);
        // Outside a page and the terminal the keys are the editor's zoom's.
        GCHECK(!Keys::keymap().keysOf("editor.action.fontZoomIn").empty());
    });

    // ---- the editor's zoom
    MCGuiStep("Ctrl+=", [] { MCGuiKey("ctrl+="); });
    MCGuiWait("a point larger", 10, [] {
        return Zoom::editorZoom() == 1 && near(fontPx(view()), px(Zoom::desktopPoints() + 1));
    });
    MCGuiStep("Ctrl+Shift+= (Ctrl+plus)", [] { MCGuiKey("ctrl+shift+="); });
    MCGuiWait("two points larger, the characters and the gutter wider", 10, [] {
        return Zoom::editorZoom() == 2 && near(fontPx(view()), px(Zoom::desktopPoints() + 2)) &&
               charWidth() > gChar0 && gutterWidth() > gGutter0;
    });
    MCGuiStep("kept in the state file, not the settings file; the keyboard where it was", [] {
        MCGuiNote("zoomed 2: %.2f px, gutter %d px, a character %d px", fontPx(view()),
                  gutterWidth(), charWidth());
        const ViewZoom::State s = ViewZoom::decode(MCGuiReadFile(Zoom::statePath()));
        GCHECK(s.editor == 2 && s.terminal == 0);
        GCHECK(Zoom::statePath() == MCGuiScratch() + "/state/minicode/zoom.json");
        GCHECK_EQ_STR(settingsText(), gSettings);
        GCHECK(editorHasKeyboard());
    });
    MCGuiStep("a Markdown page", [] { MCGuiOpenFileKept(F("zoom.md")); });
    MCGuiWait("rendered", 10, [] { return ed()->currentPath() == F("zoom.md") && ed()->inPreview(); });
    MCGuiStep("its sizes scaled by (13 + 2) / 13", [] {
        const double scale = 15.0 / 13.0;
        GCHECK(near(Zoom::previewScale(), scale, 1e-9));
        GCHECK(near(fontPx(view()), px(Zoom::desktopPoints() * scale)));
        GCHECK(tagPoints("h1") == ViewZoom::zoomed(26, 2) && tagPoints("h1") == 30);
        GCHECK(tagPoints("md_code") == 15 && tagPoints("md_gap") == ViewZoom::zoomed(8, 2));
        MCGuiNote("the page at zoom 2: body %.2f px, h1 %.1f pt, code %.1f pt", fontPx(view()),
                  tagPoints("h1"), tagPoints("md_code"));
    });
    MCGuiStep("back to the text, the keyboard in it", [] {
        MCGuiOpenFileKept(F("zoom.txt"));
        MCGuiClick(view());
    });
    MCGuiWait("the text, zoomed", 10, [] {
        return ed()->currentPath() == F("zoom.txt") && editorHasKeyboard() &&
               near(fontPx(view()), px(Zoom::desktopPoints() + 2));
    });
    MCGuiStep("Ctrl+-", [] { MCGuiKey("ctrl+-"); });
    MCGuiWait("a point smaller", 10, [] {
        return Zoom::editorZoom() == 1 && near(fontPx(view()), px(Zoom::desktopPoints() + 1));
    });
    MCGuiStep("Ctrl+Alt+0", [] { MCGuiKey("ctrl+alt+0"); });
    MCGuiWait("back as it was: the text, the gutter, the page's sizes", 10, [] {
        return Zoom::editorZoom() == 0 && near(fontPx(view()), gPx0) && charWidth() == gChar0 &&
               gutterWidth() == gGutter0 && tagPoints("h1") == 26 && tagPoints("md_code") == 13;
    });
    // The keypad's 0 as it comes with Num Lock off, as the checks' display
    // has it: KP_Insert (with Num Lock on it is KP_0, the same command).
    MCGuiStep("Ctrl+= then Ctrl+keypad 0", [] {
        MCGuiKey("ctrl+=");
        MCGuiKey("ctrl+KP_Insert");
    });
    MCGuiWait("back at none", 10, [] {
        return Zoom::editorZoom() == 0 && near(fontPx(view()), gPx0);
    });
    MCGuiStep("Ctrl+- six times from the desktop's size", [] {
        for (int i = 0; i < 6; i++) MCGuiKey("ctrl+-");
    });
    MCGuiWait("never under 6 points", 10, [] {
        return Zoom::editorZoom() == 6 - Zoom::desktopPoints() && near(fontPx(view()), px(6));
    });
    MCGuiStep("back, and zoomed in two for the next app", [] {
        MCGuiKey("ctrl+alt+0");
        MCGuiKey("ctrl+=");
        MCGuiKey("ctrl+=");
    });
    MCGuiWait("zoomed 2", 10, [] { return Zoom::editorZoom() == 2; });
    MCGuiAsync("a second app opens with the zoom from the state file", 90, [](MCGuiDone done) {
        MCGuiWriteFile(MCGuiScratch() + "/zoomchild/child.txt", "a child's file\n");
        MCGuiRunChild("_zoom_child", MCGuiScratch() + "/zoomchild", [done](int status, std::string out) {
            GCHECK(status == 0);
            if (status != 0) MCGuiNote("the second app (%d):\n%s", status, out.c_str());
            done();
        });
    });
    MCGuiStep("the zoom back to none", [] { MCGuiKey("ctrl+alt+0"); });
    MCGuiWait("none", 10, [] { return Zoom::editorZoom() == 0 && near(fontPx(view()), gPx0); });

#ifdef MINICODE_ENABLE_PDF
    // ---- a page: a PDF on show
    MCGuiStep("a PDF", [] { MCGuiOpenFileKept(F("zoom.pdf")); });
    MCGuiWait("on show", 10, [] { return ed()->showingPdf(); });
    MCGuiStep("Ctrl+= zooms the PDF, not the editor", [] {
        gPdfZoom = ed()->media()->pdfView()->effectiveZoom();
        MCGuiKey("ctrl+=");
    });
    MCGuiWait("the PDF zoomed in", 10, [] {
        return ed()->media()->pdfView()->zoom() > 0 &&
               ed()->media()->pdfView()->effectiveZoom() > gPdfZoom;
    });
    MCGuiStep("the editor's zoom as it was; Ctrl+Alt+0 fits the width again", [] {
        GCHECK(Zoom::editorZoom() == 0);
        MCGuiKey("ctrl+alt+0");
    });
    MCGuiWait("fit width", 10, [] { return ed()->media()->pdfView()->zoom() == 0; });
    MCGuiStep("and Ctrl+0 still focuses the tree", [] {
        GCHECK(Zoom::editorZoom() == 0);
        MCGuiKey("ctrl+0");
    });
    MCGuiWait("the tree has the keyboard", 10, [] {
        return Keys::within(focusWidget(), MCGuiApp().tree->widget());
    });
    MCGuiStep("back to the text", [] {
        MCGuiOpenFileKept(F("zoom.txt"));
        MCGuiClick(view());
    });
    MCGuiWait("the editor has the keyboard", 10, editorHasKeyboard);
#endif

#ifdef MINICODE_ENABLE_BROWSER
    // ---- a page: the browser with the keyboard
    MCGuiStep("the browser, by its key", [] { MCGuiKey("ctrl+shift+b"); });
    MCGuiWait("it has the keyboard", 10, [] {
        return Keys::within(focusWidget(), MCGuiApp().browser->widget());
    });
    MCGuiStep("Ctrl+= and Ctrl+Shift+= zoom its page on Safari's steps", [] {
        GCHECK(near(MCGuiApp().browser->zoomLevel(), 1, 1e-6));
        MCGuiKey("ctrl+=");
        MCGuiKey("ctrl+shift+=");
    });
    MCGuiWait("125%", 10, [] { return near(MCGuiApp().browser->zoomLevel(), 1.25, 1e-6); });
    MCGuiStep("Ctrl+- three times", [] {
        GCHECK(Zoom::editorZoom() == 0);
        for (int i = 0; i < 3; i++) MCGuiKey("ctrl+-");
    });
    MCGuiWait("85%", 10, [] { return near(MCGuiApp().browser->zoomLevel(), 0.85, 1e-6); });
    MCGuiStep("Ctrl+Alt+0", [] { MCGuiKey("ctrl+alt+0"); });
    MCGuiWait("100%", 10, [] { return near(MCGuiApp().browser->zoomLevel(), 1, 1e-6); });
    MCGuiStep("the editor's zoom never moved; the browser hidden again", [] {
        GCHECK(Zoom::editorZoom() == 0 && near(fontPx(view()), gPx0));
        GCHECK(Keys::within(focusWidget(), MCGuiApp().browser->widget()));
        MCGuiKey("ctrl+shift+b");
    });
    MCGuiWait("hidden, the keyboard back in the editor", 10, editorHasKeyboard);
#endif

#ifdef MINICODE_ENABLE_TERMINAL
    // ---- the terminals' zoom
    MCGuiStep("the terminal, by its key or (left open by another suite) a click", [] {
        if (MCGuiApp().layout->terminalShown()) MCGuiClick(MCGuiApp().terminal->widget());
        else MCGuiKey("ctrl+`");
    });
    MCGuiWait("it has the keyboard", 10, terminalHasKeyboard);
    MCGuiWait("the shell is up", 20, [] {
        return terminalText().find_first_not_of(" \n") != std::string::npos;
    });
    MCGuiStep("at its size, unzoomed", [] {
        gCols = vte_terminal_get_column_count(vte());
        gCellH = vte_terminal_get_char_height(vte());
        MCGuiNote("unzoomed: %ld columns, cells %ld px high, scale %.3f", gCols, gCellH,
                  vte_terminal_get_font_scale(vte()));
        GCHECK(near(vte_terminal_get_font_scale(vte()), 1, 1e-9));
        GCHECK(gCols > 20);
    });
    MCGuiStep("Ctrl+= and Ctrl+Shift+= with the terminal's keyboard", [] {
        MCGuiKey("ctrl+=");
        MCGuiKey("ctrl+shift+=");
    });
    MCGuiWait("the terminal two points larger, with fewer columns", 10, [] {
        const double want = (Zoom::terminalBasePoints() + 2) / Zoom::terminalBasePoints();
        return Zoom::terminalZoom() == 2 && near(vte_terminal_get_font_scale(vte()), want, 1e-6) &&
               vte_terminal_get_char_height(vte()) > gCellH &&
               vte_terminal_get_column_count(vte()) < gCols;
    });
    MCGuiStep("the editor as it was; the shell asked for its width", [] {
        GCHECK(Zoom::editorZoom() == 0 && near(fontPx(view()), gPx0));
        MCGuiNote("zoomed 2: %ld columns, cells %ld px high", vte_terminal_get_column_count(vte()),
                  vte_terminal_get_char_height(vte()));
        MCGuiType("echo cols=$(tput cols) > zcols.txt\n");
    });
    MCGuiWait("the program sees the new width", 20, [] {
        return MCGuiReadFile(F("zcols.txt")) ==
               "cols=" + std::to_string(vte_terminal_get_column_count(vte())) + "\n";
    });
    MCGuiStep("Ctrl+- then Ctrl+Alt+0", [] {
        MCGuiKey("ctrl+-");
        GCHECK(terminalHasKeyboard());
    });
    MCGuiWait("a point smaller", 10, [] { return Zoom::terminalZoom() == 1; });
    MCGuiStep("Ctrl+Alt+0", [] { MCGuiKey("ctrl+alt+0"); });
    MCGuiWait("back: the columns as they were", 10, [] {
        return Zoom::terminalZoom() == 0 && near(vte_terminal_get_font_scale(vte()), 1, 1e-9) &&
               vte_terminal_get_column_count(vte()) == gCols;
    });
    MCGuiStep("a program that writes the bytes it reads", [] {
        MCGuiType("stty raw -echo; head -c 6 > zkeys.bin; stty sane\n");
    });
    MCGuiWait("it is reading", 20, [] { return MCGuiFileExists(F("zkeys.bin")); });
    MCGuiStep("plain Ctrl keys, Alt+Z and Ctrl+Shift+- among the zoom keys", [] {
        const bool wraps = Zoom::wordWrap();
        GCHECK(wraps);
        for (const char* k : {"ctrl+w", "ctrl+=", "ctrl+b", "ctrl+=", "alt+z", "ctrl+-",
                              "ctrl+shift+-", "ctrl+e"})
            MCGuiKey(k);
    });
    MCGuiWait("the program got the six bytes", 20, [] {
        return MCGuiReadFile(F("zkeys.bin")).size() == 6;
    });
    MCGuiStep("as the program's: Ctrl+W, Ctrl+B, Meta Z, Ctrl+_ and Ctrl+E; the zoom keys zoomed", [] {
        const std::string got = MCGuiReadFile(F("zkeys.bin"));
        GCHECK(got == std::string("\x17\x02\x1bz\x1f\x05"));
        if (got != std::string("\x17\x02\x1bz\x1f\x05"))
            for (unsigned char c : got) MCGuiNote("byte %02x", c);
        GCHECK(Zoom::terminalZoom() == 1);
        GCHECK(Zoom::editorZoom() == 0);
        GCHECK(Zoom::wordWrap());   // Alt+Z was the program's
        GCHECK(terminalHasKeyboard());
    });
    MCGuiStep("the terminal's zoom back, and the terminal hidden", [] {
        MCGuiKey("ctrl+alt+0");
        MCGuiKey("ctrl+shift+t");
    });
    MCGuiWait("hidden, unzoomed", 10, [] {
        return Zoom::terminalZoom() == 0 && !terminalHasKeyboard();
    });
#endif
    MCGuiStep("the editor with the keyboard, both zooms at none", [] {
        MCGuiClick(view());
        Zoom::zoomEditor(0);
        Zoom::zoomTerminal(0);
    });
    MCGuiWait("as at the start", 10, [] {
        return editorHasKeyboard() && near(fontPx(view()), gPx0) && gutterWidth() == gGutter0;
    });
}

// The second app: the zoom it reads from the state file at its start.
MC_GUI_SUITE(_zoom_child) {
    MCGuiStep("a file of its own", [] { MCGuiOpenFileKept(MCGuiScratch() + "/zoomchild/child.txt"); });
    MCGuiWait("it shows", 10, [] { return ed() && ed()->currentPath().find("child.txt") != std::string::npos; });
    MCGuiWait("its text two points over the desktop's size", 10, [] {
        return near(fontPx(view()), px(Zoom::desktopPoints() + 2));
    });
    MCGuiStep("the zoom as the first app left it", [] {
        GCHECK(Zoom::editorZoom() == 2);
        GCHECK(Zoom::terminalZoom() == 0);
        MCGuiNote("the second app: %.2f px", fontPx(view()));
    });
}
