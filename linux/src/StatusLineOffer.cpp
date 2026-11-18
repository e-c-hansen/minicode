// StatusLineOffer.cpp — see StatusLineOffer.h.
#include "StatusLineOffer.h"

#include <ctime>

#include <unistd.h>

#include "ClaudeHooks.h"
#include "ClaudeHooksOffer.h"
#include "ClaudeStatusLine.h"
#include "ControlProtocol.h"
#include "EnvLock.h"
#include "Json.h"
#include "Question.h"

namespace StatusLineOffer {

namespace {

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
    if (ClaudeHooks::readSettingsFile(ClaudeHooksOffer::settingsPath(), text, exists, err)) return true;
    if (error) *error = err;
    return false;
}

void notice(GtkWindow* parent, const std::string& message, const std::string& detail,
            std::function<void()> then) {
    Question::tell(parent, message, detail, std::move(then));
}

void ask(GtkWindow* parent, const std::string& message, const std::string& detail,
         std::vector<std::string> buttons, const std::string& preview, std::function<void(int)> answered) {
    Question::Spec spec;
    spec.message = message;
    spec.detail = detail;
    spec.preview = preview;
    spec.buttons = std::move(buttons);
    spec.defaultButton = 0;
    spec.cancelButton = (int)spec.buttons.size() - 1;
    Question::ask(parent, spec, std::move(answered));
}

// The JSON that goes in, as the Mac's accessory box shows it.
std::string statusLineJson(const Json& value) {
    Json root = Json::object();
    root.set("statusLine", value);
    return ControlChannel::prettyJson(root);
}

}  // namespace

std::string usageDir() {
    EnvLock::EnvRead env;
    const char* home = getenv("HOME");
    return ClaudeStatusLine::usageDir(getenv("MUTIBLE_USAGE_DIR"), home && *home ? home : g_get_home_dir(),
                                      getenv("XDG_STATE_HOME"), false);
}

std::string command() {
    gchar* found;
    {
        EnvLock::EnvRead env;
        found = g_find_program_in_path("mutible");
    }
    if (found) {
        g_free(found);
        return ClaudeStatusLine::kCommand;
    }
    char path[4096];
    const ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return ClaudeStatusLine::kCommand;
    path[n] = 0;
    return ClaudeStatusLine::commandFor(path);
}

bool installed() {
    std::string text;
    return readSettings(text, nullptr) && ClaudeStatusLine::installed(text);
}

void install(GtkWindow* parent, std::function<void(bool)> done) {
    const std::string path = ClaudeHooksOffer::settingsPath(), cmd = command(), dir = usageDir();
    std::string text, error;
    if (!readSettings(text, &error)) {
        notice(parent, "Claude Code's settings could not be read", error, [done] { if (done) done(false); });
        return;
    }
    const ClaudeStatusLine::Plan plan = ClaudeStatusLine::planInstall(text, cmd);
    if (!plan.ok) {
        notice(parent, "Claude Code's settings were left alone",
               tilde(path) + " was not changed: " + plan.error + ". Fix the file, then try again.",
               [done] { if (done) done(false); });
        return;
    }
    if (!plan.changed) {
        notice(parent, "Plan usage is already logged",
               "Claude Code's status line in " + tilde(path) + " already runs " + cmd +
                   "; nothing was changed.",
               [done] { if (done) done(true); });
        return;
    }
    const std::string chain =
        plan.previous.empty()
            ? "You have no status line now, so it prints the model and both windows' usage."
            : "It then runs your own status line, `" + plan.previous +
                  "`, with the same input and shows what it prints, as before.";
    const std::string detail =
        "This changes the status line in " + tilde(path) + " to the lines below: Claude Code then runs `" +
        cmd + "`, which notes the 5-hour and weekly usage Claude Code reports in mutible's own folder (" +
        tilde(dir) + "). " + chain +
        " Your other settings stay as they are, the file is backed up first, and Stop Logging puts it back.";
    ask(parent, "Log Claude Code's plan usage for mutible's forecast?", detail, {"Log Plan Usage", "Cancel"},
        statusLineJson(plan.statusLine), [parent, path, cmd, dir, done](int answer) {
            if (answer != 0) {
                if (done) done(false);
                return;
            }
            // Read and planned again inside: the file may have changed while
            // the question was up.
            const ClaudeStatusLine::Result r =
                ClaudeStatusLine::install(path, cmd, ClaudeStatusLine::recordPath(dir), (long)time(nullptr));
            if (!r.ok) {
                notice(parent, "The status line was not changed", r.error, [done] { if (done) done(false); });
            } else if (!r.changed) {
                notice(parent, "Plan usage is already logged",
                       tilde(path) + " already has it; nothing was changed.", [done] { if (done) done(true); });
            } else {
                const std::string backup = r.backup.empty()
                    ? "It is a new file, so there was nothing to back up."
                    : "The file as it was is in " + tilde(r.backup) + ".";
                notice(parent, "Plan usage is logged",
                       "Written to " + tilde(path) + ". " + backup +
                           " The forecast fills in after Claude Code's next reply.",
                       [done] { if (done) done(true); });
            }
        });
}

void uninstall(GtkWindow* parent, std::function<void(bool)> done) {
    const std::string path = ClaudeHooksOffer::settingsPath(), dir = usageDir();
    std::string text, error;
    if (!readSettings(text, &error)) {
        notice(parent, "Claude Code's settings could not be read", error, [done] { if (done) done(true); });
        return;
    }
    const ClaudeStatusLine::Plan plan = ClaudeStatusLine::planUninstall(text);
    if (!plan.ok || !plan.changed) {
        notice(parent, "Plan usage is not logged",
               "Claude Code's status line in " + tilde(path) + " does not run mutible; nothing was changed.",
               [done] { if (done) done(false); });
        return;
    }
    const std::string back = plan.previous.empty()
        ? "You had no status line before, so it is removed."
        : "Your own status line, `" + plan.previous + "`, runs as it did.";
    const std::string detail = "This puts Claude Code's status line in " + tilde(path) +
                               " back as it was before mutible changed it. " + back +
                               " The readings logged so far stay in " + tilde(dir) + ".";
    ask(parent, "Stop logging plan usage?", detail, {"Put It Back", "Cancel"}, "",
        [parent, path, dir, done](int answer) {
            if (answer != 0) {
                if (done) done(true);
                return;
            }
            const ClaudeStatusLine::Result r =
                ClaudeStatusLine::uninstall(path, ClaudeStatusLine::recordPath(dir), (long)time(nullptr));
            const bool still = !r.ok;
            if (!r.ok) {
                notice(parent, "The status line was not changed", r.error, [done, still] { if (done) done(still); });
                return;
            }
            const std::string how = r.exact ? "The file is exactly as it was before."
                : r.backup.empty()          ? ""
                                            : "The file before this change is in " + tilde(r.backup) + ".";
            std::string told = "Written to " + tilde(path) + ".";
            if (!how.empty()) told += " " + how;
            notice(parent, "The status line is back as it was", told, [done, still] { if (done) done(still); });
        });
}

}  // namespace StatusLineOffer
