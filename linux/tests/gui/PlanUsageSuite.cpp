// PlanUsageSuite.cpp — the Agents panel's plan usage section on Linux
// (`./build/mutible-guitest plan_usage`; A13, the Mac's steps at the end of
// mac/tests/gui/AgentsSuite.mm, over the core's ClaudeUsage and
// ClaudeStatusLine):
//   - with a transcript at $10 an hour and no status line readings, both
//     windows say there is no reading yet, beside the burn rate;
//   - with two readings, the section under the heading shows both windows,
//     the forecast, the burn rate and the fit (its tooltip), above the
//     sessions;
//   - "Log plan usage from Claude Code's status line…" asks first (the JSON
//     that goes in), Cancel writes nothing, Log Plan Usage wraps the user's
//     own status line, keeps the rest of the file, backs it up once and
//     keeps the undo's record in mutible's folder, not Claude Code's;
//   - the built `mutible statusline` logs a reading and runs the old command
//     with the same input;
//   - "Stop logging plan usage…" asks, then puts the file back byte for byte;
//   - a settings file that is a symbolic link (a dotfiles checkout) stays a
//     link through install and undo, its target written and put back.
// Nothing here reaches the real ~/.claude: CLAUDE_CONFIG_DIR, the
// transcripts folder and the usage folder are scratch folders of this
// suite's, checked to be under the harness's scratch root before anything
// is written.
#include "Harness.h"

#include "AgentsPanel.h"
#include "ClaudeHooksOffer.h"
#include "ClaudeStatusLine.h"
#include "Json.h"
#include "StatusLineOffer.h"
#include "UsageView.h"

#include <gio/gio.h>
#include <glib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string gBase, gUsageDir, gCfg, gSettings, gProjects, gReal;
std::string gOriginal;
bool gOpened = false;    // this suite showed the panel
bool gSafe = false;      // every path checked to be scratch
bool gRead = false;
long gNow = 0;

struct Saved {
    bool had = false;
    std::string value;
};
std::map<std::string, Saved> gSaved;

void setEnv(const char* name, const std::string& value) {
    if (!gSaved.count(name)) {
        const char* old = g_getenv(name);
        gSaved[name] = Saved{old != nullptr, old ? old : ""};
    }
    EnvLock::envSet(name, value.c_str(), TRUE);
}

void restoreEnv() {
    for (const auto& kv : gSaved) {
        if (kv.second.had) EnvLock::envSet(kv.first.c_str(), kv.second.value.c_str(), TRUE);
        else EnvLock::envUnset(kv.first.c_str());
    }
    gSaved.clear();
}

bool scratch(const std::string& path) {
    const std::string root = MCGuiScratch();
    return !root.empty() && path.compare(0, root.size() + 1, root + "/") == 0;
}

AgentsPanel* panel() { return MCGuiApp().agents; }
UsageView* usage() { return panel() ? panel()->usageView() : nullptr; }

std::string stamp(long ago) {
    time_t t = time(nullptr) - ago;
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S.000Z", &tm);
    return buf;
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void refresh() {
    gRead = false;
    UsageModel::shared().refreshThen([] { gRead = true; });
}

// An alert is up with this button showing.
bool questionWith(const char* text) {
    GtkWindow* top = MCGuiTopWindow();
    if (!top || top == MCGuiApp().window) return false;
    GtkWidget* b = MCGuiFindButton(GTK_WIDGET(top), text);
    return b && gtk_widget_get_width(b) > 0;
}

// The question's message is its window's title (Question.cpp).
std::string questionTitle() {
    GtkWindow* top = MCGuiTopWindow();
    if (!top || top == MCGuiApp().window) return "";
    const char* t = gtk_window_get_title(top);
    return t ? t : "";
}

void answer(const char* text) { MCGuiClick(MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), text)); }

bool noQuestion() { return MCGuiTopWindow() == MCGuiApp().window; }

GtkWidget* actionButton() {
    UsageView* u = usage();
    return u && MCGuiOnScreen(u->actionButton()) ? u->actionButton() : nullptr;
}

std::vector<std::string> backups() {
    std::vector<std::string> out;
    if (GDir* d = g_dir_open(gCfg.c_str(), 0, nullptr)) {
        while (const char* n = g_dir_read_name(d))
            if (std::string(n).rfind("settings.json.mutible-backup-", 0) == 0) out.push_back(gCfg + "/" + n);
        g_dir_close(d);
    }
    return out;
}

// The two presses through the panel: the question, its answer, then the
// notice saying what happened (its title checked), then OK.
void pressAndAnswer(const std::string& what, const char* yes, const std::string& asked,
                    const std::string& told) {
    MCGuiClickWhenReady(what, [] { return actionButton(); });
    MCGuiWait("the question is up", 10, [yes] { return questionWith(yes) && questionWith("Cancel"); });
    MCGuiStep("it asks first; " + std::string(yes), [yes, asked] {
        GCHECK_EQ_STR(questionTitle(), asked);
        answer(yes);
    });
    MCGuiWait("it says what it did", 10, [] { return questionWith("OK"); });
    MCGuiStep("\"" + told + "\"; OK", [told] {
        GCHECK_EQ_STR(questionTitle(), told);
        answer("OK");
    });
    MCGuiWait("the notice is gone", 10, [] { return noQuestion(); });
}

struct Run {
    bool done = false;
    int exitCode = -1;
    std::string out, err;
};
std::shared_ptr<Run> gRun;

// The built mutible with `env` added and `input` on its stdin.
std::shared_ptr<Run> client(const std::vector<std::string>& args, const std::map<std::string, std::string>& env,
                            const std::string& input) {
    auto run = std::make_shared<Run>();
    GSubprocessLauncher* l = g_subprocess_launcher_new(
        (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                           G_SUBPROCESS_FLAGS_STDERR_PIPE));
    for (const auto& kv : env) g_subprocess_launcher_setenv(l, kv.first.c_str(), kv.second.c_str(), TRUE);
    std::vector<const char*> argv;
    const std::string exe = MCGuiBuildDir() + "/mutible";
    argv.push_back(exe.c_str());
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    GError* error = nullptr;
    GSubprocess* p = g_subprocess_launcher_spawnv(l, argv.data(), &error);
    g_object_unref(l);
    if (!p) {
        run->err = error ? error->message : "spawn failed";
        run->done = true;
        g_clear_error(&error);
        return run;
    }
    GBytes* in = g_bytes_new(input.data(), input.size());
    g_subprocess_communicate_async(
        p, in, nullptr,
        +[](GObject* source, GAsyncResult* res, gpointer data) {
            auto* holder = static_cast<std::shared_ptr<Run>*>(data);
            Run& r = **holder;
            GBytes *out = nullptr, *err = nullptr;
            g_subprocess_communicate_finish(G_SUBPROCESS(source), res, &out, &err, nullptr);
            if (out) r.out.assign((const char*)g_bytes_get_data(out, nullptr), g_bytes_get_size(out));
            if (err) r.err.assign((const char*)g_bytes_get_data(err, nullptr), g_bytes_get_size(err));
            if (out) g_bytes_unref(out);
            if (err) g_bytes_unref(err);
            r.exitCode = g_subprocess_get_if_exited(G_SUBPROCESS(source))
                             ? g_subprocess_get_exit_status(G_SUBPROCESS(source))
                             : -1;
            r.done = true;
            delete holder;
            g_object_unref(source);
        },
        new std::shared_ptr<Run>(run));
    g_bytes_unref(in);
    return run;
}

bool isLink(const std::string& path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

}  // namespace

MC_GUI_SUITE(plan_usage) {
    MCGuiStep("a scratch Claude config and a transcript at $10 an hour, no readings yet", [] {
        gBase = MCGuiScratch() + "/plan-usage";
        gUsageDir = gBase + "/usage";
        gCfg = gBase + "/claude";
        gSettings = gCfg + "/settings.json";
        gProjects = gBase + "/projects";
        gReal = gBase + "/dotfiles/claude-settings.json";
        // Every path this suite writes, and the home the app sees, are the
        // harness's scratch ones; otherwise nothing is written at all.
        const char* home = g_get_home_dir();
        gSafe = scratch(gBase) && scratch(gSettings) && scratch(gUsageDir) && home && scratch(home);
        GCHECK(gSafe);
        if (!gSafe) return;
        setEnv("CLAUDE_CONFIG_DIR", gCfg);
        setEnv("MUTIBLE_USAGE_DIR", gUsageDir);
        setEnv("MUTIBLE_CLAUDE_PROJECTS", gProjects);
        gSafe = ClaudeHooksOffer::settingsPath() == gSettings && StatusLineOffer::usageDir() == gUsageDir;
        GCHECK_EQ_STR(ClaudeHooksOffer::settingsPath(), gSettings);
        GCHECK_EQ_STR(StatusLineOffer::usageDir(), gUsageDir);
        if (!gSafe) return;
        // The user's own file, in their own spacing, with a status line.
        gOriginal = "{\n  \"model\": \"sonnet\",\n  \"statusLine\": {\"type\": \"command\", "
                    "\"command\": \"~/bin/status.sh --short\", \"padding\": 1}\n}\n";
        MCGuiWriteFile(gSettings, gOriginal);
        GCHECK(!StatusLineOffer::installed());
        // What goes in names a program the core takes for the wrapper: the
        // built mutible beside mutible-guitest, which the steps below run.
        printf("    command: %s\n", StatusLineOffer::command().c_str());
        GCHECK(ClaudeStatusLine::isWrapper(StatusLineOffer::command()));
        // $1 every 6 minutes for 3 hours (250,000 input tokens of Opus 5.5),
        // in a folder no window shows.
        std::string lines;
        for (int i = 0; i < 30; i++) {
            const std::string n = std::to_string(i);
            lines += "{\"type\":\"assistant\",\"isSidechain\":false,\"cwd\":\"/elsewhere/usage\","
                     "\"sessionId\":\"usage-session\",\"timestamp\":\"" + stamp(3 * 3600 - 360 * i - 30) +
                     "\",\"requestId\":\"req_u" + n + "\",\"message\":{\"id\":\"msg_u" + n +
                     "\",\"model\":\"claude-opus-5-5\",\"role\":\"assistant\",\"content\":[{\"type\":"
                     "\"text\",\"text\":\"ok\"}],\"usage\":{\"input_tokens\":250000,\"output_tokens\":0}}}\n";
        }
        MCGuiWriteFile(gProjects + "/-elsewhere-usage/usage-session.jsonl", lines);
        gOpened = !MCGuiApp().agentsShown;
        if (gOpened) MCGuiKey("ctrl+shift+a");
        refresh();
    });
    MCGuiWait("the panel shows and the transcript is read", 20, [] {
        return !gSafe || (usage() && MCGuiApp().agentsShown && gRead && MCGuiOnScreen(usage()->widget()));
    });
    MCGuiStep("no reading yet: both windows say so, beside the burn rate", [] {
        if (!gSafe || !usage()) return;
        const std::string text = usage()->forecastText();
        printf("    forecast: %s\n", text.c_str());
        GCHECK(has(text, "5-hour window: no reading yet. Claude Code reports it to the status line after "
                         "its first reply."));
        GCHECK(has(text, "Week: no reading yet."));
        GCHECK(has(text, "Burn rate: $") && has(text, " an hour at API prices; 1 session active"));
        GCHECK_EQ_STR(usage()->fitText(), "");
        GCHECK_EQ_STR(usage()->actionTitle(), "Log plan usage from Claude Code's status line…");
        // 20% an hour ago and 30% now; the week at 2% and 3%.
        gNow = (long)time(nullptr);
        char log[256];
        std::snprintf(log, sizeof log, "v1 %ld 20 %ld 2 %ld\nv1 %ld 30 %ld 3 %ld\n", gNow - 3600, gNow + 7200,
                      gNow + 72 * 3600, gNow, gNow + 7200, gNow + 72 * 3600);
        MCGuiWriteFile(ClaudeStatusLine::logPath(gUsageDir), log);
        refresh();
    });
    MCGuiWait("the readings are read", 20, [] { return !gSafe || gRead; });
    MCGuiStep("the section shows both windows, the forecast, the burn rate and the fit", [] {
        if (!gSafe || !usage()) return;
        UsageView* u = usage();
        const std::string text = u->forecastText();
        printf("    forecast: %s\n    fit: %s\n", text.c_str(), u->fitText().c_str());
        GCHECK(has(text, "5-hour window: 30% used. At this rate it is about "));
        GCHECK(has(text, "when it resets in "));
        GCHECK(has(text, "Week: 3.0% used."));
        GCHECK(has(text, "Burn rate: $") && has(text, " an hour at API prices; 1 session active"));
        GCHECK(!has(text, "Unseen usage"));
        GCHECK(u->fitText().rfind("Fit: 1% of the 5-hour window is about $1.00 at API prices (2 readings)", 0) == 0);
        // Under the heading, above the sessions (or the panel's notice).
        graphene_rect_t box, next;
        GtkWidget* after = gtk_widget_get_next_sibling(u->widget());
        while (after && !gtk_widget_get_visible(after)) after = gtk_widget_get_next_sibling(after);
        GCHECK(gtk_widget_compute_bounds(u->widget(), panel()->widget(), &box));
        GCHECK(after && gtk_widget_compute_bounds(after, panel()->widget(), &next));
        printf("    the section is %.0fx%.0f at y %.0f; what follows starts at y %.0f\n", box.size.width,
               box.size.height, box.origin.y, after ? next.origin.y : -1.0);
        GCHECK(box.size.height > 60 && box.origin.y > 10);
        GCHECK(after && next.origin.y >= box.origin.y + box.size.height);
        GCHECK_EQ_STR(u->actionTitle(), "Log plan usage from Claude Code's status line…");
        MCGuiSnapshot(panel()->widget(), "plan-usage");
    });

    // ---- the two presses: Cancel first, then Log Plan Usage
    MCGuiClickWhenReady("Log plan usage…", [] { return gSafe ? actionButton() : nullptr; });
    MCGuiWait("it asks first", 10, [] { return questionWith("Log Plan Usage") && questionWith("Cancel"); });
    MCGuiStep("the question shows the change; Cancel", [] {
        GCHECK_EQ_STR(questionTitle(), "Log Claude Code's plan usage for mutible's forecast?");
        MCGuiSnapshot(GTK_WIDGET(MCGuiTopWindow()), "plan-usage-question");
        answer("Cancel");
    });
    MCGuiWait("the question is gone", 10, [] { return noQuestion(); });
    MCGuiStep("Cancel wrote nothing", [] {
        GCHECK_EQ_STR(MCGuiReadFile(gSettings), gOriginal);
        GCHECK(!StatusLineOffer::installed());
        GCHECK(backups().empty());
        GCHECK(!MCGuiFileExists(ClaudeStatusLine::recordPath(gUsageDir)));
        GCHECK_EQ_STR(usage() ? usage()->actionTitle() : "", "Log plan usage from Claude Code's status line…");
    });
    pressAndAnswer("Log plan usage…, again", "Log Plan Usage",
                   "Log Claude Code's plan usage for mutible's forecast?", "Plan usage is logged");
    MCGuiWait("the action says Stop", 10, [] {
        return usage() && usage()->actionTitle() == "Stop logging plan usage…";
    });
    MCGuiStep("the wrapper chains the old status line; the rest of the file kept, one backup", [] {
        Json j;
        GCHECK(Json::parse(MCGuiReadFile(gSettings), j));
        GCHECK_EQ_STR(j["model"].asString(), "sonnet");
        GCHECK(j["statusLine"]["padding"].asInt() == 1);
        GCHECK_EQ_STR(j["statusLine"]["type"].asString(), "command");
        GCHECK_EQ_STR(j["statusLine"]["command"].asString(),
                      StatusLineOffer::command() + " --then '~/bin/status.sh --short'");
        GCHECK(StatusLineOffer::installed());
        const std::vector<std::string> b = backups();
        GCHECK(b.size() == 1);
        if (b.size() == 1) GCHECK_EQ_STR(MCGuiReadFile(b[0]), gOriginal);
        // The record for undo is in mutible's folder, not Claude Code's.
        GCHECK(MCGuiFileExists(ClaudeStatusLine::recordPath(gUsageDir)));
        GCHECK(!MCGuiFileExists(gCfg + "/statusline-install.json"));
    });

    // ---- the wrapper itself, as Claude Code runs it
    MCGuiStep("the built mutible statusline, with the old command chained", [] {
        const std::string seen = gUsageDir + "/seen.json";
        gNow = (long)time(nullptr);
        const std::string in = "{\"model\":{\"display_name\":\"Opus 5.5\"},\"rate_limits\":{\"five_hour\":"
                               "{\"used_percentage\":31,\"resets_at\":" + std::to_string(gNow + 7200) + "}}}";
        MCGuiWriteFile(gUsageDir + "/in.json", in);
        gRun = client({"statusline", "--then", "cat > \"$SEEN\"; echo mine"},
                      {{"SEEN", seen}, {"MUTIBLE_USAGE_DIR", gUsageDir}}, in);
    });
    MCGuiWait("it exits", 20, [] { return gRun && gRun->done; });
    MCGuiStep("it logged a reading and ran the old command with the same input", [] {
        GCHECK(gRun->exitCode == 0);
        GCHECK_EQ_STR(gRun->out, "mine\n");
        GCHECK_EQ_STR(gRun->err, "");
        GCHECK_EQ_STR(MCGuiReadFile(gUsageDir + "/seen.json"), MCGuiReadFile(gUsageDir + "/in.json"));
        const std::string want = " 31 " + std::to_string(gNow + 7200) + " - -\n";
        GCHECK(has(MCGuiReadFile(ClaudeStatusLine::logPath(gUsageDir)), want));
    });

    // ---- undo
    pressAndAnswer("Stop logging plan usage…", "Put It Back", "Stop logging plan usage?",
                   "The status line is back as it was");
    MCGuiWait("the action says Log again", 10, [] {
        return usage() && usage()->actionTitle() == "Log plan usage from Claude Code's status line…";
    });
    MCGuiStep("undo restored the user's status line exactly", [] {
        GCHECK_EQ_STR(MCGuiReadFile(gSettings), gOriginal);
        GCHECK(!StatusLineOffer::installed());
        GCHECK(!MCGuiFileExists(ClaudeStatusLine::recordPath(gUsageDir)));
        GCHECK(backups().size() == 1);
    });

    // ---- a settings file that is a link stays one
    MCGuiStep("the settings file a link into a dotfiles folder", [] {
        if (!gSafe || !scratch(gReal)) return;
        MCGuiWriteFile(gReal, gOriginal);
        unlink(gSettings.c_str());
        GCHECK(symlink(gReal.c_str(), gSettings.c_str()) == 0);
        GCHECK(isLink(gSettings) && !StatusLineOffer::installed());
    });
    pressAndAnswer("Log plan usage… over the link", "Log Plan Usage",
                   "Log Claude Code's plan usage for mutible's forecast?", "Plan usage is logged");
    MCGuiWait("the action says Stop", 10, [] {
        return usage() && usage()->actionTitle() == "Stop logging plan usage…";
    });
    MCGuiStep("still a link, its target holds the wrapper", [] {
        GCHECK(isLink(gSettings));
        char to[4096];
        const ssize_t n = readlink(gSettings.c_str(), to, sizeof to - 1);
        GCHECK(n > 0 && std::string(to, (size_t)std::max<ssize_t>(n, 0)) == gReal);
        GCHECK(has(MCGuiReadFile(gReal), "--then '~/bin/status.sh --short'"));
        GCHECK(StatusLineOffer::installed());
    });
    pressAndAnswer("Stop logging plan usage… over the link", "Put It Back", "Stop logging plan usage?",
                   "The status line is back as it was");
    MCGuiWait("the action says Log again", 10, [] {
        return usage() && usage()->actionTitle() == "Log plan usage from Claude Code's status line…";
    });
    MCGuiStep("still a link, its target back byte for byte", [] {
        GCHECK(isLink(gSettings));
        GCHECK_EQ_STR(MCGuiReadFile(gReal), gOriginal);
        GCHECK(!StatusLineOffer::installed());
    });

    MCGuiStep("(cleanup: the environment as it was, the panel hidden if this suite showed it)", [] {
        restoreEnv();
        unlink((gProjects + "/-elsewhere-usage/usage-session.jsonl").c_str());
        if (gOpened && MCGuiApp().agentsShown) MCGuiKey("ctrl+shift+a");
    });
    MCGuiWait("the panel is as it was", 10, [] { return !gOpened || !MCGuiApp().agentsShown; });
}
