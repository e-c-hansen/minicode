// ClaudeStatusLine.cpp — see ClaudeStatusLine.h.
#include "ClaudeStatusLine.h"
#include "ClaudeHooks.h"
#include "ControlProtocol.h"
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ClaudeStatusLine {

const char *const kCommand = "mutible statusline";

namespace {

bool plainWord(const std::string &s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '/' || c == '.' || c == '_' || c == '-' || c == '+' || c == ',' ||
                        c == ':' || c == '@' || c == '%';
        if (!ok) return false;
    }
    return true;
}

std::string baseName(const std::string &word) {
    const size_t slash = word.rfind('/');
    return slash == std::string::npos ? word : word.substr(slash + 1);
}

bool blank(const std::string &text) {
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
    return true;
}

bool parseRoot(const std::string &text, Json &root, std::string &error) {
    root = Json::object();
    if (blank(text)) return true;
    std::string err;
    if (!Json::parse(text, root, &err)) {
        error = "it is not valid JSON (" + err + ")";
        return false;
    }
    if (!root.isObject()) {
        error = "it is not a JSON object";
        return false;
    }
    return true;
}

// The root with statusLine set to `value`, in its place (or last), or
// removed when `value` is null.
std::string withStatusLine(const Json &root, const Json &value) {
    Json out = Json::object();
    bool placed = false;
    for (const auto &kv : root.items()) {
        if (kv.first == "statusLine") {
            if (!value.isNull()) out.set("statusLine", value);
            placed = true;
        } else {
            out.set(kv.first, kv.second);
        }
    }
    if (!placed && !value.isNull()) out.set("statusLine", value);
    return ControlChannel::prettyJson(out) + "\n";
}

bool readFile(const std::string &path, std::string &text) {
    bool exists = false;
    std::string error;
    return ClaudeHooks::readSettingsFile(path, text, exists, error) && exists;
}

bool makeFolders(const std::string &dir) {
    for (size_t i = 1; i <= dir.size(); i++)
        if (i == dir.size() || dir[i] == '/') {
            const std::string part = dir.substr(0, i);
            if (mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) return false;
        }
    return true;
}

std::string folderOf(const std::string &path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos || slash == 0 ? std::string() : path.substr(0, slash);
}

}  // namespace

std::string commandFor(const std::string &launcher) {
    if (launcher.empty()) return kCommand;
    return shellQuote(launcher) + " statusline";
}

std::string shellQuote(const std::string &s) {
    if (plainWord(s)) return s;
    std::string q = "'";
    for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

bool shellWords(const std::string &line, std::vector<std::string> *words) {
    words->clear();
    std::string w;
    bool in = false;
    for (size_t i = 0; i < line.size(); i++) {
        const char c = line[i];
        if (c == '\'') {
            const size_t close = line.find('\'', i + 1);
            if (close == std::string::npos) return false;
            w += line.substr(i + 1, close - i - 1);
            i = close;
            in = true;
        } else if (c == '"') {
            size_t j = i + 1;
            for (; j < line.size() && line[j] != '"'; j++) {
                if (line[j] == '\\' && j + 1 < line.size() &&
                    std::strchr("\"\\$`\n", line[j + 1])) {
                    w += line[++j];
                    continue;
                }
                w += line[j];
            }
            if (j >= line.size()) return false;
            i = j;
            in = true;
        } else if (c == '\\' && i + 1 < line.size()) {
            w += line[++i];
            in = true;
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (in) words->push_back(w);
            w.clear();
            in = false;
        } else {
            w += c;
            in = true;
        }
    }
    if (in) words->push_back(w);
    return true;
}

bool isWrapper(const std::string &command, std::string *previous) {
    if (previous) previous->clear();
    std::vector<std::string> words;
    if (!shellWords(command, &words) || words.size() < 2) return false;
    if (baseName(words[0]) != "mutible" || words[1] != "statusline") return false;
    for (size_t i = 2; i + 1 < words.size(); i++)
        if (words[i] == "--then") {
            if (previous) *previous = words[i + 1];
            break;
        }
    return true;
}

std::string wrapperCommand(const std::string &base, const std::string &previous) {
    if (previous.empty()) return base;
    // Always quoted, so the whole command is one word whatever it holds.
    std::string q = "'";
    for (char c : previous) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return base + " --then " + q + "'";
}

Plan planInstall(const std::string &text, const std::string &base) {
    Plan p;
    Json root;
    if (!parseRoot(text, root, p.error)) return p;
    p.hadStatusLine = root.has("statusLine");
    Json value = Json::object();
    if (p.hadStatusLine) {
        const Json &old = root["statusLine"];
        if (!old.isObject()) {
            p.error = "its \"statusLine\" is not an object";
            return p;
        }
        if (old.has("type") && old["type"].asString() != "command") {
            p.error = "its \"statusLine\" is not of type \"command\"";
            return p;
        }
        const Json &cmd = old["command"];
        if (!cmd.isString() || cmd.asString().empty()) {
            p.error = "its \"statusLine\" has no command";
            return p;
        }
        if (isWrapper(cmd.asString(), &p.previous)) {
            p.ok = true;
            p.statusLine = old;
            return p;
        }
        p.previous = cmd.asString();
        value = old;
        value.set("command", wrapperCommand(base, p.previous));
    } else {
        value.set("type", "command");
        value.set("command", base);
    }
    p.ok = true;
    p.changed = true;
    p.statusLine = value;
    p.text = withStatusLine(root, value);
    return p;
}

Plan planUninstall(const std::string &text) {
    Plan p;
    Json root;
    if (!parseRoot(text, root, p.error)) return p;
    p.hadStatusLine = root.has("statusLine");
    const Json &old = root["statusLine"];
    p.ok = true;
    if (!old.isObject() || !isWrapper(old["command"].asString(), &p.previous)) return p;
    Json value;   // null: removed
    if (!p.previous.empty()) {
        value = old;
        value.set("command", p.previous);
    }
    p.changed = true;
    p.statusLine = value;
    p.text = withStatusLine(root, value);
    return p;
}

bool installed(const std::string &text) {
    Json root;
    std::string error;
    if (!parseRoot(text, root, error)) return false;
    const Json &sl = root["statusLine"];
    return sl.isObject() && isWrapper(sl["command"].asString());
}

std::string recordText(const Record &r) {
    Json j = Json::object();
    j.set("version", 1.0);
    j.set("path", r.path);
    j.set("hadFile", r.hadFile);
    j.set("original", r.original);
    j.set("written", r.written);
    return ControlChannel::prettyJson(j) + "\n";
}

bool parseRecord(const std::string &text, Record *out) {
    *out = Record();
    Json j;
    if (!Json::parse(text, j) || !j.isObject() || j["version"].asNumber() != 1) return false;
    if (!j["path"].isString() || !j["written"].isString() || !j["original"].isString()) return false;
    out->path = j["path"].asString();
    out->hadFile = j["hadFile"].asBool();
    out->original = j["original"].asString();
    out->written = j["written"].asString();
    return !out->path.empty();
}

Result install(const std::string &path, const std::string &base, const std::string &recordPath,
               long now) {
    Result out;
    std::string text, error;
    bool exists = false;
    if (!ClaudeHooks::readSettingsFile(path, text, exists, error)) {
        out.error = error;
        return out;
    }
    Plan plan = planInstall(text, base);
    if (!plan.ok) {
        out.refused = true;
        out.error = path + " was left alone: " + plan.error;
        return out;
    }
    out.previous = plan.previous;
    if (!plan.changed) {
        out.ok = true;
        return out;
    }
    unsigned mode = 0600;
    if (exists) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) mode = st.st_mode & 07777;
        if (!ClaudeHooks::backupSettings(path, text, mode, now, out.backup, error)) {
            out.error = "could not back up " + path + ", so it was left alone: " + error;
            return out;
        }
    } else {
        const std::string dir = folderOf(path);
        if (!dir.empty()) mkdir(dir.c_str(), 0700);
    }
    if (!ClaudeHooks::writeSettingsFile(path, plan.text, mode, error)) {
        out.error = error;
        return out;
    }
    // The record for undo. Without it undo still puts statusLine back.
    Record r;
    r.path = path;
    r.hadFile = exists;
    r.original = text;
    r.written = plan.text;
    if (!recordPath.empty() && makeFolders(folderOf(recordPath))) {
        std::string ignored;
        ClaudeHooks::writeSettingsFile(recordPath, recordText(r), 0600, ignored);
    }
    out.ok = true;
    out.changed = true;
    return out;
}

Result uninstall(const std::string &path, const std::string &recordPath, long now) {
    Result out;
    std::string text, error;
    bool exists = false;
    if (!ClaudeHooks::readSettingsFile(path, text, exists, error)) {
        out.error = error;
        return out;
    }
    Record r;
    std::string recordData;
    const bool haveRecord = !recordPath.empty() && readFile(recordPath, recordData) &&
                            parseRecord(recordData, &r) && r.path == path;
    Plan plan = planUninstall(text);
    if (!plan.ok) {
        out.refused = true;
        out.error = path + " was left alone: " + plan.error;
        return out;
    }
    out.previous = plan.previous;
    if (!plan.changed) {
        out.ok = true;
        if (haveRecord) unlink(recordPath.c_str());
        return out;
    }
    if (haveRecord && exists && text == r.written) {
        // Still what install wrote: the file as it was, byte for byte.
        if (!r.hadFile) {
            if (unlink(path.c_str()) != 0) {
                out.error = path + ": " + std::strerror(errno);
                return out;
            }
        } else if (!ClaudeHooks::writeSettingsFile(path, r.original, 0600, error)) {
            out.error = error;
            return out;
        }
        unlink(recordPath.c_str());
        out.ok = out.changed = out.exact = true;
        return out;
    }
    unsigned mode = 0600;
    struct stat st;
    if (stat(path.c_str(), &st) == 0) mode = st.st_mode & 07777;
    if (!ClaudeHooks::backupSettings(path, text, mode, now, out.backup, error)) {
        out.error = "could not back up " + path + ", so it was left alone: " + error;
        return out;
    }
    if (!ClaudeHooks::writeSettingsFile(path, plan.text, mode, error)) {
        out.error = error;
        return out;
    }
    if (haveRecord) unlink(recordPath.c_str());
    out.ok = out.changed = true;
    return out;
}

// ------------------------------------------------------------- the log
std::string usageDir(const char *overrideDir, const char *home, const char *xdgState, bool mac) {
    if (overrideDir && *overrideDir) return overrideDir;
    const std::string h = home && *home ? home : "";
    if (mac) return h + "/Library/Application Support/Mutible/usage";
    if (xdgState && *xdgState == '/') return std::string(xdgState) + "/mutible/usage";
    return h + "/.local/state/mutible/usage";
}

std::string logPath(const std::string &dir) { return dir + "/statusline.log"; }
std::string recordPath(const std::string &dir) { return dir + "/statusline-install.json"; }

bool appendSample(const std::string &log, const ClaudeUsage::Sample &s) {
    // The last line, to skip a reading that says nothing new.
    int fd = open(log.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size > 0) {
            char buf[512];
            const off_t from = st.st_size > (off_t)sizeof buf ? st.st_size - (off_t)sizeof buf : 0;
            const ssize_t n = pread(fd, buf, sizeof buf, from);
            if (n > 0) {
                std::string tail(buf, (size_t)n);
                while (!tail.empty() && tail.back() == '\n') tail.pop_back();
                const size_t nl = tail.rfind('\n');
                ClaudeUsage::Sample last;
                if (ClaudeUsage::parseSample(nl == std::string::npos ? tail : tail.substr(nl + 1), &last) &&
                    !ClaudeUsage::worthLogging(last, s)) {
                    close(fd);
                    return true;
                }
            }
        }
        const bool big = st.st_size > (1 << 20);
        close(fd);
        if (big) rename(log.c_str(), (log + ".1").c_str());
    } else if (!makeFolders(folderOf(log))) {
        return false;
    }
    fd = open(log.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const std::string line = ClaudeUsage::formatSample(s) + "\n";
    // One write: lines from several sessions at once never interleave.
    const bool ok = write(fd, line.data(), line.size()) == (ssize_t)line.size();
    close(fd);
    return ok;
}

std::string readLog(const std::string &log) {
    std::string out, text;
    for (const std::string &p : {log + ".1", log})
        if (readFile(p, text)) {
            out += text;
            if (!out.empty() && out.back() != '\n') out += '\n';
        }
    return out;
}

std::string defaultLine(const std::string &statusJson) {
    Json j;
    if (!Json::parse(statusJson, j) || !j.isObject()) return "Claude Code";
    std::string model = j["model"]["display_name"].asString();
    if (model.empty()) model = j["model"]["id"].asString();
    if (model.empty()) model = "Claude Code";
    std::string out = model;
    ClaudeUsage::Sample s;
    if (ClaudeUsage::sampleFromStatusJson(statusJson, 0, &s)) {
        char b[48];
        if (s.hasFive()) {
            std::snprintf(b, sizeof b, " \xC2\xB7 5h %.0f%%", s.fivePct);
            out += b;
        }
        if (s.hasWeek()) {
            std::snprintf(b, sizeof b, " \xC2\xB7 week %.0f%%", s.weekPct);
            out += b;
        }
    }
    return out;
}

}  // namespace ClaudeStatusLine
