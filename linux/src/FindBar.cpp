// FindBar.cpp — see FindBar.h. The steps follow the Mac's MCEditorFind
// (src/EditorFind.mm) one for one where GTK allows; the rules themselves
// are FindRules.h's.
#include "FindBar.h"

#include "Carets.h"
#include "Editor.h"
#include "EditSpans.h"
#include "Keys.h"
#include "MultiEdit.h"
#include "Settings.h"
#include "Utf8Offsets.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace {

double threadCpuMs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int offsetOf(const GtkTextIter& it) { return gtk_text_iter_get_offset(&it); }

// The selection, ordered; both the caret's offset when there is none.
void selectionOf(GtkTextBuffer* b, int* start, int* end) {
    GtkTextIter s, e;
    gtk_text_buffer_get_selection_bounds(b, &s, &e);
    *start = offsetOf(s);
    *end = offsetOf(e);
    if (*end < *start) std::swap(*start, *end);
}

// "rgba(234,92,0,0.329)" from RRGGBBAA, the alpha written by hand: GTK sets
// the C locale from the environment, and a German one would write "0,329".
std::string rgba(uint32_t c) {
    const unsigned a = (unsigned)(c & 0xFF) * 1000 / 255;
    char buf[64];
    std::snprintf(buf, sizeof buf, "rgba(%u,%u,%u,%u.%03u)", (unsigned)(c >> 24),
                  (unsigned)((c >> 16) & 0xFF), (unsigned)((c >> 8) & 0xFF), a / 1000, a % 1000);
    return buf;
}

GdkRGBA gdk(uint32_t c) {
    return GdkRGBA{(float)(c >> 24) / 255.0f, (float)((c >> 16) & 0xFF) / 255.0f,
                   (float)((c >> 8) & 0xFF) / 255.0f, (float)(c & 0xFF) / 255.0f};
}

// The bar's stylesheet: one for the display, in the theme's colors.
GtkCssProvider* gCss = nullptr;

std::string stylesheet(const FindRules::Palette& p, bool dark) {
    const std::string border = p.widgetBorder ? rgba(p.widgetBorder) : rgba(p.widgetBg);
    const std::string inputBorder = p.inputBorder ? rgba(p.inputBorder) : "transparent";
    const std::string hover = dark ? "rgba(90,93,94,0.314)" : "rgba(184,184,184,0.314)";
    std::string css;
    css += ".minicode-find-bar { background-color: " + rgba(p.widgetBg) +
           "; color: " + rgba(p.widgetFg) + "; border: 1px solid " + border +
           "; border-top-width: 0; border-radius: 0 0 6px 6px; padding: 4px 4px 4px 0;"
           " box-shadow: 0 0 8px " + rgba(p.shadow) + "; }\n";
    css += ".minicode-find-input { background-color: " + rgba(p.inputBg) +
           "; border: 1px solid " + inputBorder + "; border-radius: 2px; padding: 0 2px 0 4px;"
           " min-height: 22px; }\n";
    css += ".minicode-find-input:focus-within { border-color: " + rgba(p.focus) + "; }\n";
    css += ".minicode-find-input.error { border-color: #BE1100; }\n";
    css += ".minicode-find-input > text { background: none; color: " + rgba(p.inputFg) +
           "; caret-color: " + rgba(p.inputFg) + "; min-height: 20px; }\n";
    css += ".minicode-find-input > text > placeholder { color: " + rgba(p.placeholder) + "; }\n";
    css += ".minicode-find-bar button { background: none; box-shadow: none; border: 1px solid "
           "transparent; border-radius: 3px; min-height: 18px; min-width: 18px; padding: 1px 2px;"
           " color: " + rgba(p.widgetFg) + "; }\n";
    css += ".minicode-find-bar button:hover { background-color: " + hover + "; }\n";
    css += ".minicode-find-bar button:disabled { opacity: 0.5; }\n";
    css += ".minicode-find-bar button.minicode-find-option { font-family: monospace;"
           " font-size: 11px; padding: 0 3px; }\n";
    css += ".minicode-find-bar button.minicode-find-option:checked,"
           " .minicode-find-bar button.minicode-find-selection:checked { background-color: " +
           rgba(p.optBg) + "; border-color: " + rgba(p.optBorder) + "; color: " +
           rgba(p.optFg) + "; }\n";
    css += ".minicode-find-bar button.minicode-find-text { font-size: 12px; padding: 1px 6px; }\n";
    css += ".minicode-find-bar button.minicode-find-chevron { padding: 0; min-width: 16px; "
           "border-radius: 0; }\n";
    css += ".minicode-find-count { font-size: 12px; min-width: 69px; color: " + rgba(p.widgetFg) +
           "; }\n";
    css += ".minicode-find-count.error { color: " + rgba(p.errorFg) + "; }\n";
    return css;
}

GtkWidget* iconButton(const char* icon) {
    GtkWidget* b = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_focusable(b, FALSE);
    gtk_widget_set_focus_on_click(b, FALSE);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    return b;
}

GtkWidget* textButton(const char* label) {
    GtkWidget* b = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(b, "minicode-find-text");
    gtk_widget_set_focusable(b, FALSE);
    gtk_widget_set_focus_on_click(b, FALSE);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    return b;
}

GtkWidget* optionToggle(const char* label) {
    GtkWidget* b = gtk_toggle_button_new_with_label(label);
    gtk_widget_add_css_class(b, "minicode-find-option");
    gtk_widget_set_focusable(b, FALSE);
    gtk_widget_set_focus_on_click(b, FALSE);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    return b;
}

// An input as VS Code draws one: a box with the field and its toggles.
GtkWidget* inputBox(GtkWidget** text, const char* placeholder) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 1);
    gtk_widget_add_css_class(box, "minicode-find-input");
    gtk_widget_set_hexpand(box, TRUE);
    *text = gtk_text_new();
    gtk_text_set_placeholder_text(GTK_TEXT(*text), placeholder);
    gtk_editable_set_width_chars(GTK_EDITABLE(*text), 14);
    gtk_widget_set_hexpand(*text, TRUE);
    gtk_widget_set_valign(*text, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), *text);
    return box;
}

std::string tip(const char* what, const char* id, const char* also = nullptr) {
    std::string keys;
    if (also) keys = also;
    const std::string k = id ? Keys::label(id, "+") : std::string();
    if (!k.empty()) keys += (keys.empty() ? "" : ", ") + k;
    return keys.empty() ? std::string(what) : std::string(what) + " (" + keys + ")";
}

}  // namespace

// The text a search reads, as of one version of the buffer: copied on the
// main thread once per edit of it (never per keystroke in the query), and
// turned into UTF-16 by the first worker that reads it.
struct FindBar::Text {
    std::string utf8;
    std::once_flag once;
    std::u16string u16;
    const std::u16string& units() {
        std::call_once(once, [this] {
            u16 = utf16::fromUtf8(utf8);
            std::string().swap(utf8);
        });
        return u16;
    }
};

// One search, or one Replace All's changes, on a worker thread.
struct FindBar::Job {
    bool replace = false;
    FindBar* self = nullptr;
    std::weak_ptr<int> alive;
    unsigned long gen = 0, version = 0;
    std::shared_ptr<Text> text;
    TextFind::Query query;
    std::u16string replacement;
    std::size_t scopeStart = 0, scopeEnd = 0;
    std::shared_ptr<std::atomic<bool>> cancel;
    FindRules::Found found;
    std::vector<FindRules::Change> changes;
    double workerMs = 0;
};

// ================================================================ the bar

FindBar::FindBar(Editor* editor) : editor_(editor), view_(editor->textView()) {
    build();
    editor_->addOverlay(box_);
    vadj_ = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view_));
    if (vadj_) {
        g_object_ref(vadj_);
        g_signal_connect_swapped(vadj_, "value-changed", G_CALLBACK(+[](FindBar* self) {
            self->schedulePaint();
        }), this);
        g_signal_connect_swapped(vadj_, "notify::page-size", G_CALLBACK(+[](FindBar* self) {
            self->schedulePaint();
        }), this);
    }
    g_signal_connect_swapped(view_, "notify::buffer", G_CALLBACK(+[](FindBar* self) {
        self->attach(gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->view_)));
    }), this);
    g_signal_connect_swapped(view_, "map", G_CALLBACK(+[](FindBar* self) {
        self->updateVisibility();
    }), this);
    g_signal_connect_swapped(view_, "unmap", G_CALLBACK(+[](FindBar* self) {
        self->updateVisibility();
    }), this);
    g_signal_connect_swapped(view_, "notify::editable", G_CALLBACK(+[](FindBar* self) {
        if (!self->canReplace() && self->replaceShown_) self->setReplaceShown(false);
        self->showCount();
    }), this);
    // Escape in the editor closes the bar, after anything else that wanted
    // it (a completion list's own controller runs in the capture phase).
    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint key, guint,
                                                         GdkModifierType mods, gpointer p) -> gboolean {
        if (key != GDK_KEY_Escape || (mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK)))
            return FALSE;
        return static_cast<FindBar*>(p)->closeFromEditor();
    }), this);
    gtk_widget_add_controller(view_, keys);
    viewKeys_ = keys;
    attach(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view_)));
    refreshTips();
}

FindBar::~FindBar() {
    life_.reset();
    if (cancel_) cancel_->store(true);
    if (searchTimer_) g_source_remove(searchTimer_);
    if (paintIdle_) g_source_remove(paintIdle_);
    if (selectionIdle_) g_source_remove(selectionIdle_);
    searchTimer_ = paintIdle_ = selectionIdle_ = 0;
    detach();
    if (vadj_) {
        g_signal_handlers_disconnect_by_data(vadj_, this);
        g_object_unref(vadj_);
    }
    // The widgets' handlers (main.cpp's disconnectOwners takes them too).
    g_signal_handlers_disconnect_by_data(view_, this);
    for (GtkEventController* c : {viewKeys_, boxKeys_})
        if (c) g_signal_handlers_disconnect_by_data(c, this);
    for (GtkWidget* w : {box_, findText_, replaceText_, case_, word_, regex_, preserve_, selection_,
                         prev_, next_, close_, chevron_, replaceOne_, replaceAll_})
        if (w) g_signal_handlers_disconnect_by_data(w, this);
}

void FindBar::build() {
    box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_widget_add_css_class(box_, "minicode-find-bar");
    gtk_widget_set_halign(box_, GTK_ALIGN_END);
    gtk_widget_set_valign(box_, GTK_ALIGN_START);
    gtk_widget_set_margin_end(box_, 18);   // the scroll bar's room, as VS Code leaves it
    gtk_widget_set_visible(box_, FALSE);

    chevron_ = iconButton("pan-end-symbolic");
    gtk_widget_add_css_class(chevron_, "minicode-find-chevron");
    gtk_widget_set_valign(chevron_, GTK_ALIGN_FILL);
    gtk_box_append(GTK_BOX(box_), chevron_);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 3);
    gtk_box_append(GTK_BOX(box_), grid);

    findInput_ = inputBox(&findText_, "Find");
    case_ = optionToggle("Aa");
    word_ = optionToggle("ab");
    regex_ = optionToggle(".*");
    for (GtkWidget* t : {case_, word_, regex_}) gtk_box_append(GTK_BOX(findInput_), t);
    count_ = gtk_label_new("No results");
    gtk_widget_add_css_class(count_, "minicode-find-count");
    gtk_label_set_xalign(GTK_LABEL(count_), 0.5);
    prev_ = iconButton("go-up-symbolic");
    next_ = iconButton("go-down-symbolic");
    selection_ = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(selection_), "edit-select-all-symbolic");
    gtk_widget_add_css_class(selection_, "minicode-find-selection");
    gtk_widget_set_focusable(selection_, FALSE);
    gtk_widget_set_focus_on_click(selection_, FALSE);
    gtk_widget_set_valign(selection_, GTK_ALIGN_CENTER);
    close_ = iconButton("window-close-symbolic");
    gtk_grid_attach(GTK_GRID(grid), findInput_, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), count_, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), prev_, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), next_, 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), selection_, 4, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), close_, 5, 0, 1, 1);

    replaceInput_ = inputBox(&replaceText_, "Replace");
    preserve_ = optionToggle("AB");
    gtk_box_append(GTK_BOX(replaceInput_), preserve_);
    replaceButtons_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    replaceOne_ = textButton("Replace");
    replaceAll_ = textButton("All");
    gtk_box_append(GTK_BOX(replaceButtons_), replaceOne_);
    gtk_box_append(GTK_BOX(replaceButtons_), replaceAll_);
    gtk_grid_attach(GTK_GRID(grid), replaceInput_, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), replaceButtons_, 1, 1, 5, 1);
    setReplaceShown(false);

    g_signal_connect_swapped(findText_, "changed", G_CALLBACK(+[](FindBar* self) {
        if (!self->settingField_) self->queryChanged();
    }), this);
    for (GtkWidget* t : {case_, word_, regex_, preserve_})
        g_signal_connect_swapped(t, "toggled", G_CALLBACK(+[](FindBar* self) {
            self->queryChanged();
        }), this);
    g_signal_connect_swapped(selection_, "toggled", G_CALLBACK(+[](FindBar* self) {
        // The click flipped it; setFindInSelection sets it as it decides.
        self->toggleFindInSelection();
    }), this);
    g_signal_connect_swapped(prev_, "clicked", G_CALLBACK(+[](FindBar* self) { self->findNext(false); }), this);
    g_signal_connect_swapped(next_, "clicked", G_CALLBACK(+[](FindBar* self) { self->findNext(true); }), this);
    g_signal_connect_swapped(close_, "clicked", G_CALLBACK(+[](FindBar* self) { self->close(); }), this);
    g_signal_connect_swapped(chevron_, "clicked", G_CALLBACK(+[](FindBar* self) {
        self->toggleReplaceRow();
    }), this);
    g_signal_connect_swapped(replaceOne_, "clicked", G_CALLBACK(+[](FindBar* self) {
        self->replaceOne();
    }), this);
    g_signal_connect_swapped(replaceAll_, "clicked", G_CALLBACK(+[](FindBar* self) {
        self->replaceAll();
    }), this);

    // The keys of the fields, before the fields see them.
    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint key, guint,
                                                         GdkModifierType mods, gpointer p) -> gboolean {
        FindBar* self = static_cast<FindBar*>(p);
        if (!self->editing()) return FALSE;
        const bool inReplace = gtk_widget_has_focus(self->replaceText_);
        const GdkModifierType other = (GdkModifierType)(GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK);
        switch (key) {
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
        case GDK_KEY_ISO_Enter:
            if ((mods & other) == GDK_ALT_MASK && !inReplace) {
                if (!self->selectAllMatches()) gtk_widget_error_bell(self->box_);
                return TRUE;
            }
            if (mods & other) return FALSE;   // Ctrl+Alt+Enter is Replace All's
            if (inReplace) self->replaceOne();
            else self->findNext(!(mods & GDK_SHIFT_MASK));
            return TRUE;
        case GDK_KEY_Escape:
            self->close();
            return TRUE;
        case GDK_KEY_Tab:
        case GDK_KEY_ISO_Left_Tab:
        case GDK_KEY_KP_Tab:
            if (mods & other) return FALSE;
            // Between the two fields; with one, nowhere else to go.
            if (self->replaceShown_) self->focusField(inReplace ? self->findText_ : self->replaceText_);
            return TRUE;
        default:
            return FALSE;
        }
    }), this);
    gtk_widget_add_controller(box_, keys);
    boxKeys_ = keys;
}

void FindBar::refreshTips() {
    gtk_widget_set_tooltip_text(case_, tip("Match Case", "toggleFindCaseSensitive").c_str());
    gtk_widget_set_tooltip_text(word_, tip("Match Whole Word", "toggleFindWholeWord").c_str());
    gtk_widget_set_tooltip_text(regex_, tip("Use Regular Expression", "toggleFindRegex").c_str());
    gtk_widget_set_tooltip_text(preserve_, tip("Preserve Case", "togglePreserveCase").c_str());
    gtk_widget_set_tooltip_text(selection_, tip("Find in Selection", "toggleFindInSelection").c_str());
    gtk_widget_set_tooltip_text(prev_, tip("Previous Match", "editor.action.previousMatchFindAction",
                                           "Shift+Enter").c_str());
    gtk_widget_set_tooltip_text(next_, tip("Next Match", "editor.action.nextMatchFindAction",
                                           "Enter").c_str());
    gtk_widget_set_tooltip_text(close_, "Close (Escape)");
    gtk_widget_set_tooltip_text(chevron_,
                                tip("Toggle Replace", "editor.action.startFindReplaceAction").c_str());
    gtk_widget_set_tooltip_text(replaceOne_, tip("Replace", "editor.action.replaceOne", "Enter").c_str());
    gtk_widget_set_tooltip_text(replaceAll_, tip("Replace All", "editor.action.replaceAll").c_str());
}

void FindBar::applySettings(const Settings& s) {
    const bool dark = s.isDark();
    const FindRules::Palette p = FindRules::palette(s.theme().id, dark);
    if (!gCss) {
        gCss = gtk_css_provider_new();
        // Above the application's own stylesheet, whose rules for buttons,
        // labels and text would otherwise win.
        gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                                   GTK_STYLE_PROVIDER(gCss),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
    gtk_css_provider_load_from_string(gCss, stylesheet(p, dark).c_str());
    matchColor_ = gdk(FindRules::matchHighlight(dark));
    currentColor_ = gdk(FindRules::currentMatch(dark));
    scopeColor_ = gdk(FindRules::scopeHighlight(dark));
    if (matchTag_) g_object_set(matchTag_, "background-rgba", &matchColor_, nullptr);
    if (currentTag_) g_object_set(currentTag_, "background-rgba", &currentColor_, nullptr);
    if (scopeTag_) g_object_set(scopeTag_, "background-rgba", &scopeColor_, nullptr);
    refreshTips();
}

// ------------------------------------------------------------ state

bool FindBar::matchCase() const { return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(case_)); }
bool FindBar::wholeWord() const { return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(word_)); }
bool FindBar::regex() const { return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(regex_)); }
bool FindBar::preserveCase() const {
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(preserve_));
}
std::string FindBar::query() const { return gtk_editable_get_text(GTK_EDITABLE(findText_)); }
std::string FindBar::countText() const { return gtk_label_get_text(GTK_LABEL(count_)); }
bool FindBar::countInErrorColor() const { return gtk_widget_has_css_class(count_, "error"); }
bool FindBar::queryOutlined() const { return gtk_widget_has_css_class(findInput_, "error"); }

bool FindBar::editing() const {
    return gtk_widget_has_focus(findText_) || gtk_widget_has_focus(replaceText_);
}

bool FindBar::scope(int* start, int* end) const {
    if (!scopeA_ || !buf_) return false;
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_mark(buf_, &a, scopeA_);
    gtk_text_buffer_get_iter_at_mark(buf_, &b, scopeB_);
    *start = offsetOf(a);
    *end = offsetOf(b);
    return true;
}

bool FindBar::resultsCurrent() const {
    return resultsGen_ == queryGen_ && resultsVersion_ == textVersion_;
}

long FindBar::currentIndex() const {
    if (!resultsCurrent() || !buf_) return -1;
    int s = 0, e = 0;
    selectionOf(buf_, &s, &e);
    return TextFind::indexOf(found_.points, (std::size_t)s, (std::size_t)(e - s));
}

bool FindBar::canReplace() const {
    return editor_->isSource() && gtk_text_view_get_editable(GTK_TEXT_VIEW(view_));
}

// Alt+Enter, VS Code's Select All Matches: a selection at every match (in
// the scope), a caret each (Carets.h, at most 10,000, those around the
// selected match), and the editor takes the keyboard. The bar stays.
bool FindBar::selectAllMatches() {
    Carets* carets = editor_->carets();
    if (!carets || !canReplace() || !resultsCurrent() || found_.points.empty()) return false;
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    ranges.reserve(found_.points.size());
    for (const TextFind::Match& m : found_.points) ranges.push_back({m.start, m.length});
    const long at = currentIndex();
    gtk_widget_grab_focus(view_);
    return carets->selectRanges(ranges, at < 0 ? 0 : (std::size_t)at);
}

void FindBar::setReplaceShown(bool on) {
    replaceShown_ = on;
    gtk_widget_set_visible(replaceInput_, on);
    gtk_widget_set_visible(replaceButtons_, on);
    gtk_button_set_icon_name(GTK_BUTTON(chevron_), on ? "pan-down-symbolic" : "pan-end-symbolic");
}

void FindBar::updateVisibility() {
    const bool visible = shown_ && gtk_widget_get_mapped(view_);
    if (gtk_widget_get_visible(box_) == visible) return;
    gtk_widget_set_visible(box_, visible);
}

// The primary selection, X's and Wayland's, has one owner: whoever selects
// last takes it, and the one it is taken from gives up its selection. A
// GtkText then collapses its own to its caret, which it had put at the
// start, so the query typed next went in before the seed ("alpha10");
// and a GtkTextBuffer collapses its selection to the insert mark, so
// focusing the query (all of it selected) dropped the editor's selection
// that Ctrl+F had just seeded from, and the match found after it was the
// one after the selection (both found by the find_bar suite). So while the
// bar's fields have the keyboard, a selection made in the text is made
// without offering it to the primary clipboard (the buffer offers it again
// from the next change of it), and the editor's selection is put back
// that way after a field takes the keyboard.
void FindBar::selectInText(int insert, int bound) {
    if (!buf_) return;
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(buf_, &a, insert);
    gtk_text_buffer_get_iter_at_offset(buf_, &b, bound);
    GdkClipboard* primary = editing() && gtk_widget_get_realized(view_)
                                ? gtk_widget_get_primary_clipboard(view_)
                                : nullptr;
    if (primary) gtk_text_buffer_remove_selection_clipboard(buf_, primary);
    gtk_text_buffer_select_range(buf_, &a, &b);
    if (primary) gtk_text_buffer_add_selection_clipboard(buf_, primary);
}

void FindBar::focusField(GtkWidget* field) {
    int ins = -1, bound = -1;
    if (buf_) {
        GtkTextIter i, b;
        gtk_text_buffer_get_iter_at_mark(buf_, &i, gtk_text_buffer_get_insert(buf_));
        gtk_text_buffer_get_iter_at_mark(buf_, &b, gtk_text_buffer_get_selection_bound(buf_));
        ins = offsetOf(i);
        bound = offsetOf(b);
    }
    gtk_widget_grab_focus(field);
    gtk_editable_select_region(GTK_EDITABLE(field), 0, -1);
    if (buf_ && ins != bound) {
        GtkTextIter i, b;
        gtk_text_buffer_get_iter_at_mark(buf_, &i, gtk_text_buffer_get_insert(buf_));
        gtk_text_buffer_get_iter_at_mark(buf_, &b, gtk_text_buffer_get_selection_bound(buf_));
        if (offsetOf(i) != ins || offsetOf(b) != bound) selectInText(ins, bound);
    }
}

// ------------------------------------------------------------ the buffer

void FindBar::attach(GtkTextBuffer* buffer) {
    if (buffer == buf_) return;
    detach();
    if (!buffer) return;
    buf_ = buffer;
    g_object_ref(buf_);
    g_signal_connect_swapped(buf_, "changed", G_CALLBACK(+[](FindBar* self) {
        self->textChanged();
    }), this);
    g_signal_connect(buf_, "mark-set", G_CALLBACK(+[](GtkTextBuffer* b, GtkTextIter*, GtkTextMark* m,
                                                      gpointer p) {
        if (m == gtk_text_buffer_get_insert(b) || m == gtk_text_buffer_get_selection_bound(b))
            static_cast<FindBar*>(p)->selectionChanged();
    }), this);
    textVersion_++;
    ensureTags();
    if (replaceShown_ && !canReplace()) setReplaceShown(false);
    if (shown_) {
        // Another tab: its matches, with nothing selected for the user.
        queryGen_++;
        selectOnArrival_ = false;
        search();
    }
    updateSelectionButton();
}

void FindBar::detach() {
    if (!buf_) return;
    if (cancel_) cancel_->store(true);
    cancel_.reset();
    searching_ = false;
    whenCurrent_ = nullptr;
    dropScope();
    removeAllTags();
    if (paintA_) gtk_text_buffer_delete_mark(buf_, paintA_);
    if (paintB_) gtk_text_buffer_delete_mark(buf_, paintB_);
    paintA_ = paintB_ = nullptr;
    g_signal_handlers_disconnect_by_data(buf_, this);
    g_object_unref(buf_);
    buf_ = nullptr;
    found_ = FindRules::Found();
    foundText_.reset();
    text_.reset();
    highlighted_ = 0;
}

void FindBar::textChanged() {
    textVersion_++;
    if (!shown_) return;
    // Searched again 0.1 s after typing stops. Inside a multi-range edit
    // (thousands of changes) the timer is only started, not moved each time.
    if (searchTimer_ && MultiEdit::applying()) return;
    if (searchTimer_) g_source_remove(searchTimer_);
    searchTimer_ = g_timeout_add(100, [](gpointer p) -> gboolean {
        FindBar* self = static_cast<FindBar*>(p);
        self->searchTimer_ = 0;
        self->search();
        return G_SOURCE_REMOVE;
    }, this);
}

// Once per turn of the main loop, however often the marks move: GTK's undo
// of a Replace All moves the caret 80,000 times.
void FindBar::selectionChanged() {
    if (MultiEdit::applying() || selectionIdle_) return;
    selectionIdle_ = g_idle_add_full(G_PRIORITY_HIGH_IDLE + 10, [](gpointer p) -> gboolean {
        FindBar* self = static_cast<FindBar*>(p);
        self->selectionIdle_ = 0;
        self->updateSelectionButton();
        if (!self->shown_) return G_SOURCE_REMOVE;
        self->showCount();
        self->schedulePaint();
        return G_SOURCE_REMOVE;
    }, this, nullptr);
}

// ------------------------------------------------------------ open, close

void FindBar::showFind() {
    // From the bar's own field Ctrl+F only selects the query.
    if (shown_ && editing()) {
        focusField(findText_);
        return;
    }
    show(findText_, false);
}

void FindBar::showReplace() {
    if (shown_ && editing()) {
        if (!replaceShown_ && canReplace()) setReplaceShown(true);
        if (replaceShown_) focusField(replaceText_);
        return;
    }
    show(canReplace() ? replaceText_ : findText_, true);
}

// The Mac's showFocusing:replace: the query seeded as VS Code's
// getSelectionSearchString does (seedSearchStringFromSelection "always"),
// worked out on the selection's line alone; the bar shown, the replace row
// with it when asked for; `field` (or no field) given the keyboard; and the
// matches found again when the bar was closed or the query changed.
void FindBar::show(GtkWidget* field, bool replace) {
    refreshTips();
    bool seeded = false, word = false;
    std::size_t wordAt = 0;
    if (buf_) {
        GtkTextIter s, e;
        gtk_text_buffer_get_selection_bounds(buf_, &s, &e);
        if (gtk_text_iter_get_line(&s) == gtk_text_iter_get_line(&e)) {
            GtkTextIter ls = s, le = s;
            gtk_text_iter_set_line_offset(&ls, 0);
            if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
            char* raw = gtk_text_buffer_get_slice(buf_, &ls, &le, TRUE);
            const std::u16string line = utf16::fromUtf8(raw ? raw : "");
            g_free(raw);
            const std::size_t a =
                EditSpans::unitsAt(line, (std::size_t)gtk_text_iter_get_line_offset(&s));
            const std::size_t b =
                EditSpans::unitsAt(line, (std::size_t)gtk_text_iter_get_line_offset(&e));
            const FindRules::Seed seed = FindRules::seed(line, a, b, regex());
            if (seed.found) {
                const std::string q = utf16::toUtf8(seed.query);
                if (!(shown_ && q == query())) {
                    settingField_ = true;
                    gtk_editable_set_text(GTK_EDITABLE(findText_), q.c_str());
                    settingField_ = false;
                    seeded = true;
                    word = seed.word;
                    wordAt = (std::size_t)offsetOf(ls) + EditSpans::codePoints(line, {seed.start})[0];
                }
            }
        }
    }
    const bool wasShown = shown_;
    shown_ = true;
    if (replace && !replaceShown_ && canReplace()) setReplaceShown(true);
    updateSelectionButton();
    updateVisibility();
    if (field && gtk_widget_get_visible(box_)) focusField(field);
    if (!wasShown || seeded) {
        queryChanged();
        // A word at the caret: its own occurrence is the one to select.
        if (seeded && word) anchor_ = wordAt;
    }
    showCount();
}

void FindBar::toggleReplaceRow() {
    if (!canReplace()) return;
    setReplaceShown(!replaceShown_);
    if (replaceShown_ && editing()) focusField(replaceText_);
}

void FindBar::close() {
    if (!shown_) return;
    const bool hadFocus = editing();
    shown_ = false;
    updateVisibility();
    if (cancel_) cancel_->store(true);
    cancel_.reset();
    searching_ = false;
    if (searchTimer_) g_source_remove(searchTimer_);
    searchTimer_ = 0;
    whenCurrent_ = nullptr;
    text_.reset();   // the copy goes with the bar
    selectOnArrival_ = false;
    if (scopeA_) {   // VS Code drops the scope with the widget
        dropScope();
        updateSelectionButton();
        queryGen_++;
    }
    removeAllTags();
    if (hadFocus && gtk_widget_get_mapped(view_)) gtk_widget_grab_focus(view_);
}

bool FindBar::closeFromEditor() {
    if (!shown_) return false;
    close();
    return true;
}

// ------------------------------------------------------------ options

void FindBar::toggleMatchCase() {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(case_), !matchCase());
}
void FindBar::toggleWholeWord() {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(word_), !wholeWord());
}
void FindBar::toggleRegex() { gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(regex_), !regex()); }
void FindBar::togglePreserveCase() {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(preserve_), !preserveCase());
}

void FindBar::toggleFindInSelection() {
    if (!shown_ && !findInSelection()) {
        // From the menu with the bar closed: open it, as VS Code's command does.
        shown_ = true;
        updateVisibility();
    }
    setFindInSelection(!findInSelection());
}

void FindBar::setFindInSelection(bool on) {
    if (!buf_ || on == findInSelection()) {
        updateSelectionButton();
        return;
    }
    if (on) {
        GtkTextIter s, e;
        if (!gtk_text_buffer_get_selection_bounds(buf_, &s, &e)) {
            gtk_widget_error_bell(view_);
            updateSelectionButton();
            return;
        }
        GtkTextIter last = e;
        gtk_text_iter_backward_char(&last);
        const int a = offsetOf(s);
        const int b = (int)FindRules::scopeEnd((std::size_t)a, (std::size_t)offsetOf(e),
                                               gtk_text_iter_get_char(&last));
        if (b <= a) {
            gtk_widget_error_bell(view_);
            updateSelectionButton();
            return;
        }
        setScope(a, b);
    } else {
        dropScope();
    }
    updateSelectionButton();
    queryChanged();
    if (on) {
        int a = 0, b = 0;
        if (scope(&a, &b)) anchor_ = (std::size_t)a;
    }
}

void FindBar::setScope(int start, int end) {
    dropScope();
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(buf_, &a, start);
    gtk_text_buffer_get_iter_at_offset(buf_, &b, end);
    // The start stays before text typed at it; the end follows text typed
    // at it, so the scope grows as the Mac's does.
    scopeA_ = gtk_text_buffer_create_mark(buf_, nullptr, &a, TRUE);
    scopeB_ = gtk_text_buffer_create_mark(buf_, nullptr, &b, FALSE);
    ensureTags();
    gtk_text_buffer_apply_tag(buf_, scopeTag_, &a, &b);
    scopeTagged_ = true;
}

void FindBar::dropScope() {
    if (!scopeA_ || !buf_) return;
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_mark(buf_, &a, scopeA_);
    gtk_text_buffer_get_iter_at_mark(buf_, &b, scopeB_);
    if (scopeTagged_ && scopeTag_) gtk_text_buffer_remove_tag(buf_, scopeTag_, &a, &b);
    scopeTagged_ = false;
    gtk_text_buffer_delete_mark(buf_, scopeA_);
    gtk_text_buffer_delete_mark(buf_, scopeB_);
    scopeA_ = scopeB_ = nullptr;
}

// VS Code's _updateToggleSelectionFindButton: enabled while it is on or
// there is a selection.
void FindBar::updateSelectionButton() {
    const bool on = findInSelection();
    bool any = false;
    if (buf_) {
        GtkTextIter s, e;
        any = gtk_text_buffer_get_selection_bounds(buf_, &s, &e);
    }
    g_signal_handlers_block_matched(selection_, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(selection_), on);
    g_signal_handlers_unblock_matched(selection_, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
    gtk_widget_set_sensitive(selection_, on || any);
}

// ------------------------------------------------------------ searching

void FindBar::queryChanged() {
    TextFind::Options o;
    o.caseSensitive = matchCase();
    o.wholeWord = wholeWord();
    o.regex = regex();
    o.preserveCase = preserveCase();
    query_ = TextFind::Query::make(query(), o);
    queryGen_++;
    if (buf_) {
        int s = 0, e = 0;
        selectionOf(buf_, &s, &e);
        anchor_ = (std::size_t)s;
    }
    selectOnArrival_ = true;
    search();
}

std::shared_ptr<FindBar::Text> FindBar::currentText() {
    if (!text_ || textAt_ != textVersion_) {
        text_ = std::make_shared<Text>();
        if (editor_->isSource() && buf_ == editor_->sourceBuffer()) {
            text_->utf8 = editor_->text();   // the highlighter's copy when it has one
        } else {
            // A slice, so a page's pictures (one character each) keep the
            // offsets the buffer's.
            GtkTextIter a, b;
            gtk_text_buffer_get_bounds(buf_, &a, &b);
            char* raw = gtk_text_buffer_get_slice(buf_, &a, &b, TRUE);
            text_->utf8 = raw ? raw : "";
            g_free(raw);
        }
        textAt_ = textVersion_;
        textCopies_++;
    }
    return text_;
}

void FindBar::search() {
    if (cancel_) cancel_->store(true);
    cancel_.reset();
    if (searchTimer_) g_source_remove(searchTimer_);
    searchTimer_ = 0;
    if (!shown_ || !buf_) return;
    if (!query_.usable()) {
        searching_ = false;
        found_ = FindRules::Found();
        foundText_.reset();
        resultsGen_ = queryGen_;
        resultsVersion_ = textVersion_;
        selectOnArrival_ = false;
        showCount();
        schedulePaint();
        auto then = std::move(whenCurrent_);
        whenCurrent_ = nullptr;
        if (then) then();
        return;
    }
    auto* job = new Job;
    job->self = this;
    job->alive = life_;
    job->gen = queryGen_;
    job->version = textVersion_;
    job->text = currentText();
    job->query = query_;
    int a = 0, b = 0;
    if (scope(&a, &b) && b > a) {
        job->scopeStart = (std::size_t)a;
        job->scopeEnd = (std::size_t)b;
    }
    job->cancel = std::make_shared<std::atomic<bool>>(false);
    cancel_ = job->cancel;
    searching_ = true;
    GTask* task = g_task_new(nullptr, nullptr, onDone, nullptr);
    g_task_set_task_data(task, job, [](gpointer p) { delete static_cast<Job*>(p); });
    g_task_run_in_thread(task, worker);
    g_object_unref(task);
    showCount();
}

void FindBar::worker(GTask* task, gpointer, gpointer data, GCancellable*) {
    Job* job = static_cast<Job*>(data);
    const double t0 = threadCpuMs();
    if (!job->cancel || !job->cancel->load()) {
        const std::u16string& text = job->text->units();
        if (job->replace)
            job->changes = FindRules::replaceAll(text, job->query, job->replacement,
                                                 job->scopeStart, job->scopeEnd);
        else
            job->found = FindRules::findAll(text, job->query, job->scopeStart, job->scopeEnd,
                                            job->cancel.get());
    }
    job->workerMs = threadCpuMs() - t0;
    g_task_return_boolean(task, TRUE);
}

void FindBar::onDone(GObject*, GAsyncResult* res, gpointer) {
    Job* job = static_cast<Job*>(g_task_get_task_data(G_TASK(res)));
    if (job->alive.expired()) return;   // the bar went with its window
    if (job->replace) job->self->replaced(job);
    else job->self->arrived(job);
}

void FindBar::arrived(Job* job) {
    if (job->cancel && job->cancel->load()) return;
    if (job->found.cancelled) return;
    // A newer query or text is on its way.
    if (job->gen != queryGen_ || job->version != textVersion_) return;
    searching_ = false;
    cancel_.reset();
    found_ = std::move(job->found);
    foundText_ = job->text;
    resultsGen_ = job->gen;
    resultsVersion_ = job->version;
    if (selectOnArrival_) {
        selectOnArrival_ = false;
        const long i = TextFind::nextIndex(found_.points, anchor_, anchor_, true);
        if (i >= 0) selectMatch((std::size_t)i);
    }
    showCount();
    schedulePaint();
    auto then = std::move(whenCurrent_);
    whenCurrent_ = nullptr;
    if (then) then();
}

// Run `block` once there are results for the query and the text as they are.
void FindBar::whenCurrent(std::function<void()> block) {
    if (resultsCurrent()) {
        block();
        return;
    }
    whenCurrent_ = std::move(block);
    search();
}

void FindBar::selectMatch(std::size_t i) {
    if (!buf_ || i >= found_.points.size()) return;
    const TextFind::Match& m = found_.points[i];
    if ((int)(m.start + m.length) > gtk_text_buffer_get_char_count(buf_)) return;
    // The caret at its end.
    selectInText((int)(m.start + m.length), (int)m.start);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(view_), gtk_text_buffer_get_insert(buf_), 0.05,
                                 FALSE, 0, 0);
    showCount();
    schedulePaint();
}

void FindBar::findNext(bool forward) {
    if (query().empty()) {   // nothing to look for: as Ctrl+F
        showFind();
        return;
    }
    if (!shown_) {   // the last query again, the keyboard left where it is
        shown_ = true;
        updateSelectionButton();
        updateVisibility();
        queryChanged();
        selectOnArrival_ = false;
    }
    if (!query_.usable()) {
        gtk_widget_error_bell(view_);
        return;
    }
    whenCurrent([this, forward] {
        if (!buf_) return;
        int s = 0, e = 0;
        selectionOf(buf_, &s, &e);
        const long i = TextFind::nextIndex(found_.points, (std::size_t)s, (std::size_t)e, forward);
        if (i < 0) {
            gtk_widget_error_bell(view_);
            return;
        }
        selectMatch((std::size_t)i);
    });
}

void FindBar::showCount() {
    FindRules::CountState st;
    st.hasQuery = !query().empty();
    st.queryError = query_.error();
    st.searched = resultsGen_ == queryGen_;
    st.current = resultsCurrent();
    st.matches = found_.points.size();
    st.truncated = found_.truncated;
    st.currentIndex = currentIndex();
    st.tooSlow = query_.tooSlow();
    const FindRules::Count c = FindRules::count(st);
    const bool can = canReplace() && st.hasQuery;
    gtk_widget_set_sensitive(replaceOne_, can);
    gtk_widget_set_sensitive(replaceAll_, can);
    if (c.keep) return;
    gtk_label_set_text(GTK_LABEL(count_), c.text.c_str());
    if (c.error || c.noResults) gtk_widget_add_css_class(count_, "error");
    else gtk_widget_remove_css_class(count_, "error");
    if (c.error || c.noResults) gtk_widget_add_css_class(findInput_, "error");
    else gtk_widget_remove_css_class(findInput_, "error");
    gtk_widget_set_tooltip_text(count_, c.tip.empty() ? nullptr : c.tip.c_str());
    gtk_widget_set_tooltip_text(findInput_, c.error && !c.tip.empty() ? c.tip.c_str() : nullptr);
    gtk_widget_set_sensitive(prev_, c.navigable);
    gtk_widget_set_sensitive(next_, c.navigable);
}

// ------------------------------------------------------------ replacing

// VS Code's Replace: the selected match is replaced and the next one
// selected; with no match selected it only goes to the next.
void FindBar::replaceOne() {
    if (!canReplace() || query().empty()) {
        gtk_widget_error_bell(view_);
        return;
    }
    if (!shown_) show(nullptr, true);
    if (!query_.usable()) {
        gtk_widget_error_bell(view_);
        return;
    }
    whenCurrent([this] {
        if (!buf_ || !foundText_) return;
        const long i = currentIndex();
        if (i < 0) {
            findNext(true);
            return;
        }
        FindRules::Change c;
        const std::u16string with =
            utf16::fromUtf8(gtk_editable_get_text(GTK_EDITABLE(replaceText_)));
        if (!FindRules::replaceOne(foundText_->units(), found_, (std::size_t)i, query_, with, &c))
            return;
        if (!MultiEdit::apply(buf_, {{c.start, c.length, std::move(c.text)}}, false)) {
            gtk_widget_error_bell(view_);
            return;
        }
        // On to the next match after the replacement.
        anchor_ = c.start + c.newLength;
        selectOnArrival_ = true;
        GtkTextIter at;
        gtk_text_buffer_get_iter_at_offset(buf_, &at, (int)anchor_);
        gtk_text_buffer_place_cursor(buf_, &at);
        search();
    });
}

void FindBar::replaceAll() {
    if (!canReplace() || query().empty()) {
        gtk_widget_error_bell(view_);
        return;
    }
    if (!shown_) show(nullptr, true);
    if (!query_.usable() || !buf_) {
        gtk_widget_error_bell(view_);
        return;
    }
    auto* job = new Job;
    job->replace = true;
    job->self = this;
    job->alive = life_;
    job->version = textVersion_;
    job->text = currentText();
    job->query = query_;
    job->replacement = utf16::fromUtf8(gtk_editable_get_text(GTK_EDITABLE(replaceText_)));
    int a = 0, b = 0;
    if (scope(&a, &b) && b > a) {
        job->scopeStart = (std::size_t)a;
        job->scopeEnd = (std::size_t)b;
    }
    GTask* task = g_task_new(nullptr, nullptr, onDone, nullptr);
    g_task_set_task_data(task, job, [](gpointer p) { delete static_cast<Job*>(p); });
    g_task_run_in_thread(task, worker);
    g_object_unref(task);
}

void FindBar::replaced(Job* job) {
    if (!buf_) return;
    if (job->version != textVersion_) {   // typed meanwhile: again, on the new text
        replaceAll();
        return;
    }
    lastReplaceAllWorkerMs_ = job->workerMs;
    if (job->changes.empty()) {
        gtk_widget_error_bell(view_);
        if (onNote) onNote("Nothing to replace");
        return;
    }
    const double t0 = threadCpuMs();
    std::vector<MultiEdit::Change> changes;
    changes.reserve(job->changes.size());
    for (FindRules::Change& c : job->changes) changes.push_back({c.start, c.length, std::move(c.text)});
    const bool ok = MultiEdit::apply(buf_, changes, true);
    lastReplaceAllMs_ = threadCpuMs() - t0;
    if (!ok) {
        gtk_widget_error_bell(view_);
        return;
    }
    const std::size_t n = changes.size();
    if (onNote) onNote("Replaced " + std::to_string(n) + (n == 1 ? " match" : " matches"));
}

// ------------------------------------------------------------ highlights

void FindBar::ensureTags() {
    if (!buf_) return;
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buf_);
    auto make = [&](const char* name, const GdkRGBA& color) {
        GtkTextTag* t = gtk_text_tag_table_lookup(table, name);
        if (!t) {
            t = gtk_text_tag_new(name);
            g_object_set(t, "background-rgba", &color, nullptr);
            gtk_text_tag_table_add(table, t);
            g_object_unref(t);   // the table holds it
        }
        return t;
    };
    scopeTag_ = make("minicode-find-scope", scopeColor_);
    matchTag_ = make("minicode-find-match", matchColor_);
    currentTag_ = make("minicode-find-current", currentColor_);
}

void FindBar::schedulePaint() {
    if (paintIdle_ || !shown_) return;
    // Before GTK draws the frame (GDK_PRIORITY_REDRAW is HIGH_IDLE + 20).
    paintIdle_ = g_idle_add_full(G_PRIORITY_HIGH_IDLE + 10, [](gpointer p) -> gboolean {
        FindBar* self = static_cast<FindBar*>(p);
        self->paintIdle_ = 0;
        self->paint();
        return G_SOURCE_REMOVE;
    }, this, nullptr);
}

void FindBar::clearMatchTags() {
    if (!buf_ || !paintA_ || !matchTag_) return;
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_mark(buf_, &a, paintA_);
    gtk_text_buffer_get_iter_at_mark(buf_, &b, paintB_);
    gtk_text_buffer_remove_tag(buf_, matchTag_, &a, &b);
    gtk_text_buffer_remove_tag(buf_, currentTag_, &a, &b);
    highlighted_ = 0;
}

void FindBar::removeAllTags() {
    if (!buf_ || !matchTag_) return;
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buf_, &a, &b);
    gtk_text_buffer_remove_tag(buf_, matchTag_, &a, &b);
    gtk_text_buffer_remove_tag(buf_, currentTag_, &a, &b);
    highlighted_ = 0;
}

// Every match on the lines on screen, and 50 either side, tagged; the
// selected one with its own tag. Tags elsewhere from the last paint come off.
void FindBar::paint() {
    if (!buf_ || MultiEdit::applying()) return;
    ensureTags();
    clearMatchTags();
    if (!shown_ || !resultsCurrent() || found_.points.empty()) return;
    // Mine above every other tag's background, the selected match on top.
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buf_);
    const int top = gtk_text_tag_table_get_size(table) - 1;
    if (gtk_text_tag_get_priority(currentTag_) != top) {
        gtk_text_tag_set_priority(scopeTag_, top);
        gtk_text_tag_set_priority(matchTag_, top);
        gtk_text_tag_set_priority(currentTag_, top);
    }
    GdkRectangle vis{0, 0, 0, 0};
    gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(view_), &vis);
    GtkTextIter first, last;
    if (vis.height > 0) {
        gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(view_), &first, vis.y, nullptr);
        gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(view_), &last, vis.y + vis.height, nullptr);
    } else {
        gtk_text_buffer_get_iter_at_mark(buf_, &first, gtk_text_buffer_get_insert(buf_));
        last = first;
    }
    const int margin = 50;
    gtk_text_buffer_get_iter_at_line(buf_, &first, std::max(0, gtk_text_iter_get_line(&first) - margin));
    if (!gtk_text_iter_forward_lines(&last, margin + 1)) gtk_text_buffer_get_end_iter(buf_, &last);
    const std::size_t lo = (std::size_t)offsetOf(first), hi = (std::size_t)offsetOf(last);
    const std::vector<TextFind::Match>& ms = found_.points;
    auto it = std::lower_bound(ms.begin(), ms.end(), lo, [](const TextFind::Match& m, std::size_t at) {
        return m.start + m.length < at;
    });
    const long cur = currentIndex();
    GtkTextIter a = first, b = first;
    for (; it != ms.end() && it->start <= hi; ++it) {
        if (it->length == 0) continue;
        gtk_text_iter_set_offset(&a, (int)it->start);
        gtk_text_iter_set_offset(&b, (int)(it->start + it->length));
        gtk_text_buffer_apply_tag(buf_, (it - ms.begin()) == cur ? currentTag_ : matchTag_, &a, &b);
        highlighted_++;
    }
    if (!paintA_) {
        paintA_ = gtk_text_buffer_create_mark(buf_, nullptr, &first, TRUE);
        paintB_ = gtk_text_buffer_create_mark(buf_, nullptr, &last, FALSE);
    } else {
        gtk_text_buffer_move_mark(buf_, paintA_, &first);
        gtk_text_buffer_move_mark(buf_, paintB_, &last);
    }
}
