// ClaudeHooks.cpp — see ClaudeHooks.h.
#include "ClaudeHooks.h"
#include "ControlProtocol.h"

namespace ClaudeHooks {

const char *const kCommand = "minicode notify --from-hook";

Action actionFor(const std::string &e) {
    if (e == "Notification") return Action::Waiting;
    if (e == "Stop") return Action::Done;
    if (e == "UserPromptSubmit" || e == "PostToolUse") return Action::Clear;
    return Action::None;
}

const char *actionName(Action a) {
    switch (a) {
    case Action::Waiting: return "waiting";
    case Action::Done: return "done";
    case Action::Clear: return "clear";
    case Action::None: break;
    }
    return "";
}

Action actionNamed(const std::string &w) {
    if (w == "waiting") return Action::Waiting;
    if (w == "done") return Action::Done;
    if (w == "clear") return Action::Clear;
    return Action::None;
}

bool parseHookInput(const std::string &text, HookInput &out) {
    out = HookInput();
    Json j;
    if (!Json::parse(text, j) || !j.isObject()) return false;
    out.event = j["hook_event_name"].asString();
    out.sessionId = j["session_id"].asString();
    out.transcriptPath = j["transcript_path"].asString();
    out.cwd = j["cwd"].asString();
    out.message = j["message"].asString();
    return true;
}

int parsePositive(const char *text) {
    if (!text || !*text) return 0;
    int n = 0, digits = 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9' || ++digits > 9) return 0;
        n = n * 10 + (*p - '0');
    }
    return n;
}

bool notifyArgsFromHook(const std::string &stdinText, const char *terminalVar,
                        const char *pidVar, Json &args, int &pid) {
    args = Json::object();
    pid = parsePositive(pidVar);
    const int terminal = parsePositive(terminalVar);
    if (!pid || !terminal) return false;
    HookInput in;
    if (!parseHookInput(stdinText, in)) return false;
    const Action a = actionFor(in.event);
    if (a == Action::None) return false;
    args.set("terminal", terminal);
    args.set("state", actionName(a));
    args.set("event", in.event);
    if (!in.message.empty()) args.set("message", ControlChannel::cleanNoteText(in.message));
    if (!in.sessionId.empty()) args.set("session_id", in.sessionId);
    if (!in.transcriptPath.empty()) args.set("transcript_path", in.transcriptPath);
    if (!in.cwd.empty()) args.set("cwd", in.cwd);
    return true;
}

// ------------------------------------------------------- the configuration
const std::vector<std::string> &events() {
    static const std::vector<std::string> e = {"Notification", "Stop", "UserPromptSubmit",
                                               "PostToolUse"};
    return e;
}

namespace {

Json entry() {
    Json hook = Json::object();
    hook.set("type", "command");
    hook.set("command", kCommand);
    Json list = Json::array();
    list.push(hook);
    Json e = Json::object();
    e.set("hooks", list);
    return e;
}

bool isOurCommand(const std::string &c) {
    // Any spelling that runs `minicode notify --from-hook` (a full path to
    // the launcher, extra flags after it) counts as installed.
    return c.find("minicode notify --from-hook") != std::string::npos ||
           (c.find("minicode") != std::string::npos &&
            c.find(" notify --from-hook") != std::string::npos);
}

}  // namespace

bool runsOurCommand(const Json &e) {
    const Json &hooks = e["hooks"];
    for (size_t i = 0; i < hooks.size(); i++)
        if (isOurCommand(hooks[i]["command"].asString())) return true;
    return false;
}

Json configuration() {
    Json hooks = Json::object();
    for (const std::string &ev : events()) {
        Json list = Json::array();
        list.push(entry());
        hooks.set(ev, list);
    }
    Json root = Json::object();
    root.set("hooks", hooks);
    return root;
}

MergeResult merge(const std::string &text) {
    MergeResult r;
    bool blank = true;
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') blank = false;
    Json root = Json::object();
    if (!blank) {
        std::string err;
        if (!Json::parse(text, root, &err)) {
            r.error = "it is not valid JSON (" + err + ")";
            return r;
        }
        if (!root.isObject()) {
            r.error = "it is not a JSON object";
            return r;
        }
    }
    const Json &oldHooks = root["hooks"];
    if (root.has("hooks") && !oldHooks.isObject()) {
        r.error = "its \"hooks\" is not an object";
        return r;
    }
    for (const auto &kv : oldHooks.items())
        if (!kv.second.isArray()) {
            for (const std::string &ev : events())
                if (kv.first == ev) {
                    r.error = "its \"hooks\" entry for " + ev + " is not a list";
                    return r;
                }
        }
    // The user's events in their order, each list as it was plus ours at
    // its end when missing; then the events it did not have yet.
    Json hooks = Json::object();
    for (const auto &kv : oldHooks.items()) {
        bool ours = false;
        for (const std::string &ev : events()) ours = ours || kv.first == ev;
        if (!ours) {
            hooks.set(kv.first, kv.second);
            continue;
        }
        bool have = false;
        for (size_t i = 0; i < kv.second.size(); i++) have = have || runsOurCommand(kv.second[i]);
        if (have) {
            hooks.set(kv.first, kv.second);
            continue;
        }
        Json list = Json::array();
        for (size_t i = 0; i < kv.second.size(); i++) list.push(kv.second[i]);
        list.push(entry());
        hooks.set(kv.first, list);
        r.added.push_back(kv.first);
    }
    for (const std::string &ev : events()) {
        if (oldHooks.has(ev)) continue;
        Json list = Json::array();
        list.push(entry());
        hooks.set(ev, list);
        r.added.push_back(ev);
    }
    r.ok = true;
    r.changed = !r.added.empty();
    if (!r.changed) return r;
    Json out = Json::object();
    bool placed = false;
    for (const auto &kv : root.items()) {
        if (kv.first == "hooks") {
            out.set("hooks", hooks);
            placed = true;
        } else {
            out.set(kv.first, kv.second);
        }
    }
    if (!placed) out.set("hooks", hooks);
    r.text = ControlChannel::prettyJson(out) + "\n";
    return r;
}

}  // namespace ClaudeHooks
