// PageCheck.cpp — see PageCheck.h.
#include "PageCheck.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace PageCheck {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

bool hasControl(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7F) return true;
    return false;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// The scheme of an address ("http"), lowercased, or "" when it has none.
std::string schemeOf(const std::string& s) {
    if (s.empty() || !std::isalpha((unsigned char)s[0])) return "";
    for (size_t i = 1; i < s.size(); i++) {
        char c = s[i];
        if (c == ':') return lower(s.substr(0, i));
        if (!std::isalnum((unsigned char)c) && c != '+' && c != '-' && c != '.') return "";
    }
    return "";
}

// host[:port][/...] with a host an agent's server has: localhost, an IPv4
// address, [an IPv6 one], or a name; the port required for a name, so
// "example.com" stays a site. The host and port are checked by the caller.
bool isHostPort(const std::string& s, bool& numericOrLocal) {
    size_t end = s.find_first_of("/?#");
    std::string auth = s.substr(0, end);
    std::string host = auth, port;
    if (!auth.empty() && auth[0] == '[') {
        size_t close = auth.find(']');
        if (close == std::string::npos) return false;
        host = auth.substr(0, close + 1);
        std::string rest = auth.substr(close + 1);
        if (!rest.empty()) {
            if (rest[0] != ':') return false;
            port = rest.substr(1);
            if (!allDigits(port)) return false;
        }
        numericOrLocal = true;
        return host.size() > 2;
    }
    size_t colon = auth.find(':');
    if (colon != std::string::npos) {
        host = auth.substr(0, colon);
        port = auth.substr(colon + 1);
        if (!allDigits(port)) return false;
    }
    if (host.empty()) return false;
    bool digitsAndDots = true;
    for (char c : host) {
        if (!std::isalnum((unsigned char)c) && c != '.' && c != '-') return false;
        if (!(std::isdigit((unsigned char)c) || c == '.')) digitsAndDots = false;
    }
    const std::string h = lower(host);
    numericOrLocal = digitsAndDots || h == "localhost" ||
                     (h.size() > 10 && h.compare(h.size() - 10, 10, ".localhost") == 0);
    return numericOrLocal || !port.empty();
}

}  // namespace

bool isLocalAddress(const std::string& url) {
    if (url.empty() || hasControl(url) || url.find(' ') != std::string::npos) return false;
    const std::string scheme = schemeOf(url);
    if (scheme == "file") return url.size() >= 7 && lower(url.substr(0, 7)) == "file://";
    if (scheme != "http" && scheme != "https") return false;
    std::string rest = url.substr(scheme.size() + 1);
    if (rest.size() < 2 || rest[0] != '/' || rest[1] != '/') return false;
    rest = rest.substr(2);
    // The authority ends where a browser ends it; a backslash counts as a
    // slash in http and https addresses.
    std::string auth = rest.substr(0, rest.find_first_of("/?#\\"));
    size_t at = auth.rfind('@');
    if (at != std::string::npos) auth = auth.substr(at + 1);
    std::string host = auth, port;
    if (!auth.empty() && auth[0] == '[') {
        size_t close = auth.find(']');
        if (close == std::string::npos) return false;
        host = auth.substr(0, close + 1);
        std::string after = auth.substr(close + 1);
        if (!after.empty()) {
            if (after[0] != ':') return false;
            port = after.substr(1);
        }
    } else {
        size_t colon = auth.find(':');
        if (colon != std::string::npos) {
            host = auth.substr(0, colon);
            port = auth.substr(colon + 1);
        }
    }
    if (!port.empty() && !allDigits(port)) return false;
    host = lower(host);
    if (host.size() > 1 && host.back() == '.') host.pop_back();
    if (host == "localhost" || host == "127.0.0.1" || host == "[::1]" ||
        host == "[0:0:0:0:0:0:0:1]")
        return true;
    // A name under .localhost, which resolves to this Mac.
    const std::string tail = ".localhost";
    if (host.size() > tail.size() && host.compare(host.size() - tail.size(), tail.size(), tail) == 0) {
        const std::string name = host.substr(0, host.size() - tail.size());
        if (name.empty() || name.front() == '.' || name.back() == '.') return false;
        for (char c : name)
            if (!std::isalnum((unsigned char)c) && c != '-' && c != '.') return false;
        return true;
    }
    return false;
}

std::string originError(const std::string& url) {
    return "the page is not local (" + (url.empty() ? std::string("no page") : url) +
           "): agents read only localhost, 127.0.0.1, ::1, *.localhost and file:// pages, "
           "unless the settings file has " + kAnyOriginSetting + " = true";
}

std::string fileAddress(const std::string& path) {
    static const char* hex = "0123456789ABCDEF";
    std::string out = "file://";
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

bool agentAddress(const std::string& raw, const std::function<bool(const std::string&)>& exists,
                  std::string& url, std::string& error) {
    const std::string s = trim(raw);
    if (s.empty()) {
        error = "browse needs an address";
        return false;
    }
    if (hasControl(s)) {
        error = "an address has no control characters";
        return false;
    }
    if (s[0] == '/') {
        if (exists && exists(s)) {
            url = fileAddress(s);
            return true;
        }
        error = "no such file: " + s;
        return false;
    }
    bool local = false;
    if (s.find("://") == std::string::npos && isHostPort(s, local)) {
        url = "http://" + s;
        return true;
    }
    const std::string scheme = schemeOf(s);
    if (scheme == "http" || scheme == "https" || scheme == "file") {
        if (s.compare(scheme.size() + 1, 2, "//") != 0) {
            error = "not an address: " + s;
            return false;
        }
        url = s;
        return true;
    }
    if (lower(s) == "about:blank") {
        url = "about:blank";
        return true;
    }
    if (!scheme.empty() && s.find(' ') == std::string::npos) {
        error = "browse opens http, https and file addresses, not " + scheme + ":";
        return false;
    }
    // A site's name, as the address bar takes it.
    if (s.find('.') != std::string::npos && s.find(' ') == std::string::npos &&
        s[0] != '.') {
        url = "https://" + s;
        return true;
    }
    error = "not an address: " + s +
            " (browse takes a URL, a host:port or a file, and never searches)";
    return false;
}

// ------------------------------------------------------------ console log
int64_t ConsoleLog::add(ConsoleEntry e) {
    e.seq = next_++;
    entries_.push_back(std::move(e));
    while (entries_.size() > kMaxConsoleEntries) {
        entries_.pop_front();
        dropped_++;
    }
    return entries_.back().seq;
}

void ConsoleLog::newPage() {
    entries_.clear();
    dropped_ = 0;
}

std::vector<ConsoleEntry> ConsoleLog::since(int64_t since) const {
    std::vector<ConsoleEntry> out;
    for (const ConsoleEntry& e : entries_)
        if (e.seq > since) out.push_back(e);
    return out;
}

std::string clipUtf8(const std::string& text, size_t maxBytes) {
    if (text.size() <= maxBytes) return text;
    size_t n = maxBytes;
    while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) n--;
    return text.substr(0, n);
}

bool callerFrame(const std::string& stack, Frame& out) {
    size_t i = 0;
    while (i <= stack.size()) {
        size_t j = stack.find('\n', i);
        if (j == std::string::npos) j = stack.size();
        std::string line = trim(stack.substr(i, j - i));
        i = j + 1;
        // "fn@where" or "where": the place is after the last "@" that comes
        // before the address (a function name has no "@"; an address may).
        size_t at = line.find('@');
        std::string where = at == std::string::npos ? line : line.substr(at + 1);
        if (where.empty() || where == "[native code]" || where.find(':') == std::string::npos)
            continue;
        // where:line:column, or where:line.
        size_t c2 = where.rfind(':');
        std::string last = where.substr(c2 + 1);
        if (!allDigits(last)) continue;
        size_t c1 = where.rfind(':', c2 - 1);
        Frame f;
        if (c1 != std::string::npos && allDigits(where.substr(c1 + 1, c2 - c1 - 1))) {
            f.source = where.substr(0, c1);
            f.line = std::atoi(where.substr(c1 + 1, c2 - c1 - 1).c_str());
            f.column = std::atoi(last.c_str());
        } else {
            f.source = where.substr(0, c2);
            f.line = std::atoi(last.c_str());
        }
        // Not an address, or one of WebKit's own (a user script's).
        const std::string sch = schemeOf(f.source);
        if (sch.empty() || sch == "user-script" || sch == "webkit-masked-url") continue;
        out = f;
        return true;
    }
    return false;
}

bool parseConsoleMessage(const std::string& json, ConsoleEntry& out, bool fromPage) {
    Json j;
    if (!Json::parse(json, j) || !j.isObject()) return false;
    const std::string kind = j["kind"].asString();
    if (kind != "console" && kind != "uncaught" && kind != "rejection") return false;
    if (fromPage && kind != "console") return false;
    ConsoleEntry e;
    e.uncaught = kind != "console";
    e.level = e.uncaught ? "error" : j["level"].asString();
    static const char* levels[] = {"log", "info", "warn", "error", "debug"};
    bool known = false;
    for (const char* l : levels) known = known || e.level == l;
    if (!known) return false;
    std::string text = j["text"].asString();
    if (kind == "rejection") text = "Unhandled promise rejection: " + text;
    e.text = clipUtf8(text, kMaxConsoleText);
    e.source = clipUtf8(j["source"].asString(), 2048);
    e.line = (int)std::max<int64_t>(0, j["line"].asInt());
    e.column = (int)std::max<int64_t>(0, j["column"].asInt());
    if (e.source.empty() && j["stack"].isString()) {
        Frame f;
        if (callerFrame(j["stack"].asString(), f)) {
            e.source = clipUtf8(f.source, 2048);
            e.line = f.line;
            e.column = f.column;
        }
    }
    out = e;
    return true;
}

Json entryJson(const ConsoleEntry& e) {
    Json o = Json::object();
    o.set("seq", e.seq);
    o.set("level", e.level);
    o.set("text", e.text);
    o.set("source", e.source.empty() ? Json() : Json(e.source));
    o.set("line", e.line > 0 ? Json(e.line) : Json());
    o.set("column", e.column > 0 ? Json(e.column) : Json());
    if (e.uncaught) o.set("uncaught", true);
    return o;
}

Json pageState(const std::string& url, int status, const std::string& title, bool loading) {
    Json o = Json::object();
    o.set("url", url.empty() ? Json() : Json(url));
    o.set("status", status > 0 ? Json(status) : Json());
    o.set("title", title);
    o.set("loading", loading);
    return o;
}

SnapshotPlan snapshotPlan(double visibleHeight, double pageHeight, double scale, bool full) {
    SnapshotPlan p;
    if (scale <= 0) scale = 1;
    p.height = std::max(1.0, full ? std::max(visibleHeight, pageHeight) : visibleHeight);
    const double most = kMaxSnapshotPixels / scale;
    if (p.height > most) {
        p.height = most;
        p.cut = true;
    }
    return p;
}

bool screenshotPath(const std::string& path, bool isFolder, std::string& error) {
    if (path.empty() || path[0] != '/') {
        error = "the screenshot needs a path";
        return false;
    }
    if (isFolder) {
        error = path + " is a folder: give a file name ending in .png";
        return false;
    }
    if (path.size() < 5 || lower(path.substr(path.size() - 4)) != ".png" || path.back() == '/') {
        error = "the screenshot is a PNG: give a path ending in .png";
        return false;
    }
    return true;
}

}  // namespace PageCheck
