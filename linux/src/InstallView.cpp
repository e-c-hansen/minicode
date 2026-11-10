// InstallView.cpp — see InstallView.h. The words are the Mac's
// (InstallPanel.mm), with "this computer" for "this Mac".
#include "InstallView.h"

#include "Lsp.h"
#include "LspClient.h"

#include <cstdlib>
#include <sstream>

namespace {

void installCss() {
    static bool done = false;
    if (done || !gdk_display_get_default()) return;
    done = true;
    // The popover's own colors, whatever the theme; the Mac's muted lines
    // are its text dimmed.
    const char* css =
        ".minicode-install { padding: 12px 14px; }"
        ".minicode-install .minicode-install-title { font-weight: bold; }"
        ".minicode-install .minicode-install-command {"
        " font-family: monospace; padding: 3px 6px; border-radius: 4px;"
        " background-color: alpha(currentColor, 0.08); }"
        ".minicode-install .minicode-install-quiet { color: alpha(currentColor, 0.7); }";
    GtkCssProvider* p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

GtkWidget* wrappingLabel(const std::string& text) {
    GtkWidget* l = gtk_label_new(text.c_str());
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 70);
    return l;
}

// Copy: the command on the clipboard, and the button says so for a moment.
void copyClicked(GtkButton* b, gpointer) {
    const char* command = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "minicode-command"));
    if (!command) return;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(b)), command);
    gtk_button_set_label(b, "Copied");
    g_timeout_add(1500, [](gpointer p) -> gboolean {
        gtk_button_set_label(GTK_BUTTON(p), "Copy");
        g_object_unref(p);
        return G_SOURCE_REMOVE;
    }, g_object_ref(b));
}

}  // namespace

bool InstallerAvailable(const std::string& program) {
    if (const char* fake = getenv("MINICODE_INSTALL_HAVE")) {
        std::stringstream ss(fake);
        std::string p;
        while (std::getline(ss, p, ','))
            if (p == program) return true;
        return false;
    }
    return !LspFindProgram(program).empty();
}

InstallView* InstallView::forServer(const std::string& key, const std::string& message, Run run) {
    InstallView* v = new InstallView();
    v->runner_ = std::move(run);
    v->build(Install::server(key), {}, "No " + Lsp::serverDisplayName(key) + " language server",
             message);
    return v;
}

InstallView* InstallView::forFormatters(const std::vector<std::string>& programs,
                                        const std::string& message, Run run) {
    const Install::Tool* first = nullptr;
    std::vector<const Install::Tool*> extra;
    for (const std::string& p : programs) {
        const Install::Tool* t = Install::formatter(p);
        if (!t) continue;
        if (!first) first = t;
        else extra.push_back(t);
    }
    InstallView* v = new InstallView();
    v->runner_ = std::move(run);
    v->build(first, extra, first ? first->name + " is not installed" : "No formatter found",
             message);
    return v;
}

std::string InstallView::shownText() const {
    std::string out;
    for (const std::string& t : texts_) out += t + "\n";
    return out;
}

void InstallView::addText(const std::string& text, const char* cssClass) {
    GtkWidget* l = wrappingLabel(text);
    if (cssClass) gtk_widget_add_css_class(l, cssClass);
    gtk_box_append(GTK_BOX(box_), l);
    texts_.push_back(text);
}

// A command in a box the user can select from, with Copy and, for the one
// to run, Run in Terminal.
GtkWidget* InstallView::row(const std::string& command, bool withRun, GtkWidget** copyOut,
                            GtkWidget** runOut) {
    GtkWidget* r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* text = gtk_label_new(command.c_str());
    gtk_label_set_selectable(GTK_LABEL(text), TRUE);
    gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_label_set_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(text), PANGO_WRAP_CHAR);
    gtk_widget_set_hexpand(text, TRUE);
    gtk_widget_add_css_class(text, "minicode-install-command");
    gtk_box_append(GTK_BOX(r), text);
    GtkWidget* copy = gtk_button_new_with_label("Copy");
    g_object_set_data_full(G_OBJECT(copy), "minicode-command", g_strdup(command.c_str()), g_free);
    g_signal_connect(copy, "clicked", G_CALLBACK(copyClicked), nullptr);
    gtk_widget_set_valign(copy, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(r), copy);
    if (copyOut) *copyOut = copy;
    if (withRun) {
        GtkWidget* run = gtk_button_new_with_label("Run in Terminal");
        gtk_widget_set_tooltip_text(run, "Types the command into the terminal; press Return there "
                                         "to run it");
        g_object_set_data_full(G_OBJECT(run), "minicode-command", g_strdup(command.c_str()),
                               g_free);
        // The view lives as long as its box, and the button is in it.
        g_signal_connect(run, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer selfp) {
                             InstallView* self = static_cast<InstallView*>(selfp);
                             const char* c = static_cast<const char*>(
                                 g_object_get_data(G_OBJECT(b), "minicode-command"));
                             if (!c || !self->runner_) return;
                             const std::string command = c;
                             // onDone first: it may close the popover this is in,
                             // and the command goes to the terminal after.
                             Run run = self->runner_;
                             if (self->onDone) self->onDone();
                             run(command);
                         }),
                         this);
        gtk_widget_set_valign(run, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(r), run);
        if (runOut) *runOut = run;
    }
    gtk_box_append(GTK_BOX(box_), r);
    texts_.push_back(command);
    return r;
}

void InstallView::build(const Install::Tool* tool, const std::vector<const Install::Tool*>& extra,
                        const std::string& title, const std::string& message) {
    installCss();
    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(box_, "minicode-install");
    gtk_widget_set_size_request(box_, 460, -1);
    // This object goes with its box.
    g_object_set_data_full(G_OBJECT(box_), "minicode-install-view", this,
                           [](gpointer p) { delete static_cast<InstallView*>(p); });
    addText(title, "minicode-install-title");
    if (!message.empty()) addText(message, nullptr);
    if (!tool) return;
    const Install::Suggestion s = Install::suggest(*tool, InstallerAvailable, extra);
    if (s.command.empty()) return;
    addText(tool->gives, nullptr);
    const std::string computer = Install::thisComputer();
    std::string command = s.command;
    if (!s.before.empty()) {
        // The installer is missing too, and one line can get it: both.
        command = s.before + " && " + s.command;
        addText("It installs with " + s.missing + ", which " + computer +
                    " does not have yet, so this gets " + s.missing + " first:",
                nullptr);
    } else if (!s.beforeUrl.empty()) {
        const std::string lead =
            "It installs with " + s.missing + ", which " + computer + " does not have yet: ";
        const std::string link = "how to get " + s.missing;
        GtkWidget* l = wrappingLabel("");
        gchar* markup = g_markup_printf_escaped("%s<a href=\"%s\">%s</a>", lead.c_str(),
                                                s.beforeUrl.c_str(), link.c_str());
        gtk_label_set_markup(GTK_LABEL(l), markup);
        g_free(markup);
        gtk_box_append(GTK_BOX(box_), l);
        texts_.push_back(lead + link);
    } else {
        addText("Install it with:", nullptr);
    }
    command_ = command;
    row(command, (bool)runner_, &copy_, &run_);
    alternatives_ = s.alternatives;
    if (!alternatives_.empty()) {
        addText(alternatives_.size() == 1 ? "Or:" : "Or one of these:", "minicode-install-quiet");
        for (const std::string& a : alternatives_) row(a, false, nullptr, nullptr);
    }
    addText("Run in Terminal types the command into the terminal; press Return there to run "
            "it. minicode finds the program by itself once it is installed, with no restart.",
            "minicode-install-quiet");
    const std::string site = tool->name + "'s website";
    GtkWidget* l = wrappingLabel("");
    gchar* markup =
        g_markup_printf_escaped("<a href=\"%s\">%s</a>", tool->homepage.c_str(), site.c_str());
    gtk_label_set_markup(GTK_LABEL(l), markup);
    g_free(markup);
    gtk_box_append(GTK_BOX(box_), l);
    texts_.push_back(site);
}
