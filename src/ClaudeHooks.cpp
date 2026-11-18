// ClaudeHooks.cpp — see ClaudeHooks.h.
#include "ClaudeHooks.h"
#include "Rename.h"
#include "ControlProtocol.h"
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace ClaudeHooks {

const char *const kCommand = "mutible notify --from-hook";

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

Json entry(const std::string &command) {
    Json hook = Json::object();
    hook.set("type", "command");
    hook.set("command", command);
    Json list = Json::array();
    list.push(hook);
    Json e = Json::object();
    e.set("hooks", list);
    return e;
}

bool isOurCommand(const std::string &c) {
    // Any spelling that runs `mutible notify --from-hook` (a full path to
    // the launcher, extra flags after it) counts as installed.
    // The command before the rename (src/Rename.h) counts too: Claude
    // Code's settings files keep it, and it still runs.
    for (const char *name : {"mutible", Rename::kOldName})
        if (c.find(std::string(name) + " notify --from-hook") != std::string::npos ||
            (c.find(name) != std::string::npos && c.find(" notify --from-hook") != std::string::npos))
            return true;
    return false;
}

}  // namespace

bool runsOurCommand(const Json &e) {
    const Json &hooks = e["hooks"];
    for (size_t i = 0; i < hooks.size(); i++)
        if (isOurCommand(hooks[i]["command"].asString())) return true;
    return false;
}

Json configuration() { return configuration(kCommand, {}); }

Json configuration(const std::string &command, const std::vector<std::string> &only) {
    Json hooks = Json::object();
    for (const std::string &ev : events()) {
        bool wanted = only.empty();
        for (const std::string &o : only) wanted = wanted || o == ev;
        if (!wanted) continue;
        Json list = Json::array();
        list.push(entry(command));
        hooks.set(ev, list);
    }
    Json root = Json::object();
    root.set("hooks", hooks);
    return root;
}

std::string commandFor(const std::string &launcher) {
    if (launcher.empty()) return kCommand;
    bool plain = true;
    for (unsigned char c : launcher) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '/' || c == '.' || c == '_' || c == '-' || c == '+' || c == ',' ||
                        c == ':' || c == '@' || c == '%';
        plain = plain && ok;
    }
    std::string quoted = launcher;
    if (!plain) {
        quoted = "'";
        for (char c : launcher) quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
        quoted += "'";
    }
    return quoted + " notify --from-hook";
}

bool installed(const std::string &text) {
    MergeResult r = merge(text);
    return r.ok && !r.changed;
}

std::string eventList(const std::vector<std::string> &ev) {
    std::string out;
    for (size_t i = 0; i < ev.size(); i++) {
        if (i) out += ev.size() == 2 ? " and " : (i + 1 == ev.size() ? ", and " : ", ");
        out += ev[i];
    }
    return out;
}

MergeResult merge(const std::string &text) { return merge(text, kCommand); }

MergeResult merge(const std::string &text, const std::string &command) {
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
        list.push(entry(command));
        hooks.set(kv.first, list);
        r.added.push_back(kv.first);
    }
    for (const std::string &ev : events()) {
        if (oldHooks.has(ev)) continue;
        Json list = Json::array();
        list.push(entry(command));
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

std::string writeTarget(const std::string &path) {
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved)) return resolved;
    std::string p = path;
    for (int hops = 0; hops < 40; hops++) {
        struct stat st;
        if (lstat(p.c_str(), &st) != 0 || !S_ISLNK(st.st_mode)) return p;
        char buf[PATH_MAX];
        const ssize_t n = readlink(p.c_str(), buf, sizeof buf - 1);
        if (n <= 0) return p;
        std::string next(buf, (size_t)n);
        if (next[0] != '/') {
            const size_t slash = p.rfind('/');
            next = (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + next;
        }
        p = next;
    }
    return std::string();
}

namespace {

// Overwrites the file itself: every name it has sees the new text.
bool writeInPlace(const std::string &target, const std::string &text, std::string &error) {
    const int fd = open(target.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        error = target + ": " + std::strerror(errno);
        return false;
    }
    size_t off = 0;
    int failed = 0;
    while (off < text.size() && !failed) {
        const ssize_t n = pwrite(fd, text.data() + off, text.size() - off, (off_t)off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) failed = n < 0 ? errno : EIO;
        else off += (size_t)n;
    }
    if (!failed && ftruncate(fd, (off_t)text.size()) != 0) failed = errno;
    if (!failed && fsync(fd) != 0) failed = errno;
    if (close(fd) != 0 && !failed) failed = errno;
    if (failed) error = target + ": " + std::strerror(failed);
    return !failed;
}

}  // namespace

bool writeSettingsFile(const std::string &path, const std::string &text, unsigned newMode,
                       std::string &error) {
    const std::string target = writeTarget(path);
    if (target.empty()) {
        error = path + ": the symbolic link loops";
        return false;
    }
    mode_t mode = (mode_t)(newMode & 07777);
    struct stat st;
    if (stat(target.c_str(), &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            error = path + " is not a plain file";
            return false;
        }
        mode = st.st_mode & 07777;
        // A file with other names (a dotfile manager's hard link): a rename
        // would leave them on the old text, so it is written in place, as
        // FolderSearch::writeFileAtomically does.
        if (st.st_nlink > 1) return writeInPlace(target, text, error);
    }
    std::string tmp = target + ".mutible-XXXXXX";
    std::vector<char> name(tmp.begin(), tmp.end());
    name.push_back('\0');
    const int fd = mkstemp(name.data());
    if (fd < 0) {
        error = target + ": " + std::strerror(errno);
        return false;
    }
    fchmod(fd, mode);
    size_t off = 0;
    bool ok = true;
    while (off < text.size()) {
        const ssize_t n = write(fd, text.data() + off, text.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        off += (size_t)n;
    }
    ok = ok && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (!ok || rename(name.data(), target.c_str()) != 0) {
        error = target + ": " + std::strerror(errno);
        unlink(name.data());
        return false;
    }
    return true;
}

// ------------------------------------------------------- installing
std::string settingsPath(const char *configDir, const char *home) {
    if (configDir && *configDir) {
        std::string dir = configDir;
        while (!dir.empty() && (dir.back() == ' ' || dir.back() == '\n' || dir.back() == '\t'))
            dir.pop_back();
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        return dir + "/settings.json";
    }
    return std::string(home && *home ? home : "") + "/.claude/settings.json";
}

bool readSettingsFile(const std::string &path, std::string &text, bool &exists,
                      std::string &error) {
    text.clear();
    exists = false;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) return true;
        error = path + ": " + std::strerror(errno);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        error = path + " is not a plain file";
        return false;
    }
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = path + ": " + std::strerror(errno);
        return false;
    }
    char buf[65536];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            error = path + ": " + std::strerror(errno);
            close(fd);
            return false;
        }
        if (n == 0) break;
        text.append(buf, (size_t)n);
    }
    close(fd);
    exists = true;
    return true;
}

bool backupSettings(const std::string &path, const std::string &text, unsigned mode, long now,
                    std::string &backup, std::string &error) {
    // Named for the time, never over an earlier one.
    char stamp[32];
    const time_t t = (time_t)now;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    backup = path + ".mutible-backup-" + stamp;
    for (int n = 2; access(backup.c_str(), F_OK) == 0; n++)
        backup = path + ".mutible-backup-" + stamp + "-" + std::to_string(n);
    if (writeSettingsFile(backup, text, mode, error)) return true;
    backup.clear();
    return false;
}

InstallResult install(const std::string &path, const std::string &command, long now) {
    InstallResult out;
    std::string text, error;
    bool exists = false;
    if (!readSettingsFile(path, text, exists, error)) {
        out.error = error;
        return out;
    }
    MergeResult r = merge(text, command);
    if (!r.ok) {
        out.refused = true;
        out.error = path + " was left alone: " + r.error;
        return out;
    }
    out.added = r.added;
    if (!r.changed) {
        out.ok = true;
        return out;
    }
    mode_t mode = 0600;
    if (exists) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) mode = st.st_mode & 07777;
        if (!backupSettings(path, text, mode, now, out.backup, error)) {
            out.error = "could not back up " + path + ", so it was left alone: " + error;
            return out;
        }
    } else {
        const size_t slash = path.rfind('/');
        if (slash != std::string::npos && slash > 0) mkdir(path.substr(0, slash).c_str(), 0700);
    }
    if (!writeSettingsFile(path, r.text, mode, error)) {
        out.error = error;
        return out;
    }
    out.ok = true;
    out.changed = true;
    return out;
}

// ------------------------------------------------------- seeing Claude Code
namespace {

std::string baseName(const std::string &word) {
    const size_t slash = word.rfind('/');
    return slash == std::string::npos ? word : word.substr(slash + 1);
}

bool isAssignment(const std::string &w) {
    const size_t eq = w.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    for (size_t i = 0; i < eq; i++) {
        const char c = w[i];
        const bool ok = c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (i > 0 && c >= '0' && c <= '9');
        if (!ok) return false;
    }
    return true;
}

// The line's words, quotes taken off; stops at the first ; & | or newline,
// since only the line's first command is looked at.
std::vector<std::string> firstCommandWords(const std::string &line) {
    std::vector<std::string> words;
    std::string w;
    bool in = false;
    char quote = 0;
    for (size_t i = 0; i < line.size(); i++) {
        const char c = line[i];
        if (quote) {
            if (c == quote) quote = 0;
            else w += c;
            continue;
        }
        if (c == '\'' || c == '"') { quote = c; in = true; continue; }
        if (c == '\\' && i + 1 < line.size()) { w += line[++i]; in = true; continue; }
        if (c == ';' || c == '&' || c == '|' || c == '\n' || c == '(' || c == ')') break;
        if (c == ' ' || c == '\t') {
            if (in) words.push_back(w);
            w.clear();
            in = false;
            continue;
        }
        w += c;
        in = true;
    }
    if (in) words.push_back(w);
    return words;
}

}  // namespace

bool startsClaude(const std::string &line) {
    const std::vector<std::string> words = firstCommandWords(line);
    size_t i = 0;
    while (i < words.size()) {
        const std::string &w = words[i];
        if (isAssignment(w)) { i++; continue; }
        if (w == "exec" || w == "command" || w == "builtin" || w == "noglob" || w == "nohup" ||
            w == "time" || w == "env" || w == "caffeinate") {
            i++;
            // Their own options (env -i, exec -a name, caffeinate -i).
            while (i < words.size() && !words[i].empty() && words[i][0] == '-') {
                if ((w == "exec" && words[i] == "-a") || (w == "env" && words[i] == "-u")) i++;
                i++;
            }
            continue;
        }
        return baseName(w) == "claude";
    }
    return false;
}

bool isClaudeProgramPath(const std::string &path) {
    if (baseName(path) == "claude") return true;
    return path.find("/claude/versions/") != std::string::npos;
}

bool shouldOffer(bool muted, bool installed, bool offeredThisLaunch, bool barShown) {
    return !muted && !installed && !offeredThisLaunch && !barShown;
}

}  // namespace ClaudeHooks
