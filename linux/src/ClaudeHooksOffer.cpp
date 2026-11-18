// ClaudeHooksOffer.cpp — see ClaudeHooksOffer.h.
#include "EnvLock.h"
#include "ClaudeHooksOffer.h"

#include <ctime>
#include <memory>

#include <unistd.h>

#include "ClaudeHooks.h"
#include "ConsentBar.h"
#include "ControlProtocol.h"

namespace ClaudeHooksOffer {

const char* const kKey = "claude-hooks";

namespace {

bool gOffered = false;   // once per launch, across windows (the Mac's MCInstallBarTakeOffer)

std::string tilde(const std::string& path) {
    const char* home = g_get_home_dir();
    const std::string h = home ? home : "";
    if (!h.empty() && path.compare(0, h.size(), h) == 0 && (path.size() == h.size() || path[h.size()] == '/'))
        return "~" + path.substr(h.size());
    return path;
}

bool readSettings(std::string& text, std::string* error) {
    bool exists = false;
    std::string err;
    if (ClaudeHooks::readSettingsFile(settingsPath(), text, exists, err)) return true;
    if (error) *error = err;
    return false;
}

// One alert, answered in one callback with the button's number.
void alert(GtkWindow* parent, const std::string& message, const std::string& detail,
           std::vector<std::string> buttons, std::function<void(int)> answered) {
    GtkAlertDialog* dlg = gtk_alert_dialog_new("%s", message.c_str());
    gtk_alert_dialog_set_detail(dlg, detail.c_str());
    std::vector<const char*> names;
    for (const std::string& b : buttons) names.push_back(b.c_str());
    names.push_back(nullptr);
    gtk_alert_dialog_set_buttons(dlg, names.data());
    gtk_alert_dialog_set_default_button(dlg, 0);
    gtk_alert_dialog_set_cancel_button(dlg, (int)buttons.size() - 1);
    auto* cb = new std::function<void(int)>(std::move(answered));
    gtk_alert_dialog_choose(dlg, parent, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer up) {
            auto* f = static_cast<std::function<void(int)>*>(up);
            GError* err = nullptr;
            const int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, &err);
            if (err) { g_error_free(err); (*f)(-1); }
            else (*f)(choice);
            delete f;
        }, cb);
    g_object_unref(dlg);
}

void notice(GtkWindow* parent, const std::string& message, const std::string& detail,
            std::function<void()> then) {
    alert(parent, message, detail, {"OK"}, [then](int) { if (then) then(); });
}

}  // namespace

std::string settingsPath() {
    const char* home = getenv("HOME");
    return ClaudeHooks::settingsPath(getenv("CLAUDE_CONFIG_DIR"), home && *home ? home : g_get_home_dir());
}

std::string command() {
    gchar* found;
    {
        EnvLock::EnvRead env;
        found = g_find_program_in_path("mutible");
    }
    if (found) {
        g_free(found);
        return ClaudeHooks::kCommand;
    }
    char path[4096];
    const ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return ClaudeHooks::kCommand;
    path[n] = 0;
    return ClaudeHooks::commandFor(path);
}

bool installed() {
    std::string text;
    return readSettings(text, nullptr) && ClaudeHooks::installed(text);
}

void ask(GtkWindow* parent, std::function<void(bool)> done) {
    const std::string path = settingsPath(), cmd = command();
    std::string text, error;
    if (!readSettings(text, &error)) {
        notice(parent, "Claude Code's settings could not be read", error, [done] { if (done) done(false); });
        return;
    }
    ClaudeHooks::MergeResult plan = ClaudeHooks::merge(text, cmd);
    if (!plan.ok) {
        notice(parent, "Claude Code's settings were left alone",
               tilde(path) + " was not changed: " + plan.error + ". Fix the file, then try again.",
               [done] { if (done) done(false); });
        return;
    }
    if (!plan.changed) {
        notice(parent, "The hooks are already on",
               tilde(path) + " already runs " + cmd + " for every hook mutible needs; nothing was changed.",
               [done] { if (done) done(true); });
        return;
    }
    const std::string detail =
        "This adds the lines below to " + tilde(path) + ": Claude Code's " +
        ClaudeHooks::eventList(plan.added) + " hooks then run `" + cmd +
        "`, which marks the mutible terminal a session runs in when it waits for you or has "
        "finished. Your own settings and hooks stay as they are, and the file is backed up first.\n\n" +
        ControlChannel::prettyJson(ClaudeHooks::configuration(cmd, plan.added));
    alert(parent, "Add mutible's hooks to Claude Code's settings?", detail, {"Add the Hooks", "Cancel"},
          [parent, path, cmd, done](int answer) {
              if (answer != 0) {
                  if (done) done(false);
                  return;
              }
              // Read and merged again: the file may have changed while the
              // question was up.
              ClaudeHooks::InstallResult r = ClaudeHooks::install(path, cmd, (long)time(nullptr));
              if (!r.ok) {
                  notice(parent, "The hooks were not added", r.error, [done] { if (done) done(false); });
              } else if (!r.changed) {
                  notice(parent, "The hooks are already on",
                         tilde(path) + " already has them; nothing was changed.",
                         [done] { if (done) done(true); });
              } else {
                  const std::string backup = r.backup.empty()
                      ? "It is a new file, so there was nothing to back up."
                      : "The file as it was is in " + tilde(r.backup) + ".";
                  notice(parent, "The hooks are on",
                         "Written to " + tilde(path) + ". " + backup +
                             " Claude Code sessions started from now on in mutible's terminals mark "
                             "their tabs: orange while one waits for you, green when it has finished.",
                         [done] { if (done) done(true); });
              }
          });
}

GtkWidget* takeBar(GtkWindow* parent, bool shown, std::function<void()> gone) {
    if (!ClaudeHooks::shouldOffer(ConsentBar::installMuted(kKey), installed(), gOffered, shown))
        return nullptr;
    gOffered = true;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(box, "minicode-consent-bar");
    GtkWidget* label = gtk_label_new("mutible can show which Claude Code session is waiting on you.");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_tooltip_text(
        label,
        "Claude Code's hooks then put an orange dot on a terminal while its session waits for you and "
        "a green one when it has finished. Turn On shows what goes into Claude Code's settings before "
        "anything is written. Not Now stops this offer; Install Claude Code Hooks… keeps it.");
    GtkWidget* on = gtk_button_new_with_label("Turn On…");
    GtkWidget* later = gtk_button_new_with_label("Not Now");
    GtkWidget* close = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(close, "flat");
    gtk_widget_set_tooltip_text(close, "Close (it stays closed until mutible starts again)");
    for (GtkWidget* w : {label, on, later, close}) gtk_box_append(GTK_BOX(box), w);
    for (GtkWidget* b : {on, later, close}) gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    auto leave = std::make_shared<std::function<void()>>(std::move(gone));
    // The window's widgets may outlive what the callbacks reach: a weak
    // pointer to the window keeps the question's parent honest.
    auto* win = new GtkWindow*(parent);
    g_object_add_weak_pointer(G_OBJECT(parent), (gpointer*)win);
    g_object_set_data_full(G_OBJECT(box), "mc-win", win, [](gpointer p) {
        GtkWindow** w = static_cast<GtkWindow**>(p);
        if (*w) g_object_remove_weak_pointer(G_OBJECT(*w), (gpointer*)w);
        delete w;
    });
    struct Ctx { GtkWindow** win; std::shared_ptr<std::function<void()>> leave; };
    auto bind = [&](GtkWidget* b, void (*fn)(GtkButton*, gpointer)) {
        g_signal_connect_data(b, "clicked", G_CALLBACK(fn), new Ctx{win, leave},
                              [](gpointer d, GClosure*) { delete static_cast<Ctx*>(d); }, (GConnectFlags)0);
    };
    bind(on, [](GtkButton*, gpointer d) {
        Ctx* c = static_cast<Ctx*>(d);
        if (!*c->win) return;
        auto leave = c->leave;
        ask(*c->win, [leave](bool in) { if (in && *leave) (*leave)(); });
    });
    bind(later, [](GtkButton*, gpointer d) {
        ConsentBar::setInstallMuted(kKey, true);
        auto leave = static_cast<Ctx*>(d)->leave;
        if (*leave) (*leave)();
    });
    bind(close, [](GtkButton*, gpointer d) {
        auto leave = static_cast<Ctx*>(d)->leave;
        if (*leave) (*leave)();
    });
    return box;
}

void forgetOffer() { gOffered = false; }

}  // namespace ClaudeHooksOffer
