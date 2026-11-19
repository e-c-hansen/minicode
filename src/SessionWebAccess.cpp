// SessionWebAccess.cpp — see SessionWebAccess.h.
#include "SessionWebAccess.h"

#include <algorithm>
#include <cctype>

#include "IdeLink.h"
#include "SessionWeb.h"   // newToken

namespace Shared {
namespace Web {

const char* const kBrowserCookie = "mutible-browser";

std::string credentialCookie(int port) { return "mutible-view-" + std::to_string(port); }

namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    return s.substr(a, b - a);
}

// What a cookie or an id may hold: hex digits only, so nothing a browser
// sends can reach a header or a question as anything but itself.
bool hexOnly(const std::string& s, std::size_t size) {
    if (s.size() != size) return false;
    for (unsigned char c : s)
        if (!std::isxdigit(c) || std::isupper(c)) return false;
    return true;
}

}  // namespace

std::string cookieValue(const std::string& header, const std::string& name) {
    std::size_t at = 0;
    while (at <= header.size()) {
        std::size_t semi = header.find(';', at);
        if (semi == std::string::npos) semi = header.size();
        const std::string pair = header.substr(at, semi - at);
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos && trim(pair.substr(0, eq)) == name) {
            std::string v = trim(pair.substr(eq + 1));
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            return v;
        }
        at = semi + 1;
    }
    return "";
}

std::string setCookieHeader(const std::string& name, const std::string& value,
                            const std::string& path, const std::string& sameSite,
                            long long maxAge) {
    std::string h = "Set-Cookie: " + name + "=" + value + "; Path=" + path +
                    "; HttpOnly; SameSite=" + sameSite;
    if (maxAge > 0) h += "; Max-Age=" + std::to_string(maxAge);
    return h + "\r\n";
}

std::string browserSummary(const std::string& ua) {
    auto has = [&](const char* s) { return ua.find(s) != std::string::npos; };
    std::string browser;
    if (has("Edg/") || has("EdgA/") || has("EdgiOS/")) browser = "Edge";
    else if (has("OPR/") || has("Opera")) browser = "Opera";
    else if (has("Firefox/") || has("FxiOS/")) browser = "Firefox";
    else if (has("CriOS/") || has("Chrome/")) browser = "Chrome";
    else if (has("Chromium/")) browser = "Chromium";
    else if (has("Safari/") && has("Version/")) browser = "Safari";
    else if (has("AppleWebKit")) browser = "a WebKit browser";
    else if (has("curl/")) browser = "curl";
    std::string os;
    if (has("iPhone")) os = "iPhone";
    else if (has("iPad")) os = "iPad";
    else if (has("Android")) os = "Android";
    else if (has("CrOS")) os = "ChromeOS";
    else if (has("Macintosh") || has("Mac OS X")) os = "macOS";
    else if (has("Windows")) os = "Windows";
    else if (has("Linux")) os = "Linux";
    if (browser.empty()) browser = os.empty() ? "an unknown browser" : "a browser";
    return os.empty() ? browser : browser + " on " + os;
}

// ---------------------------------------------------------------- Access

Access::Access() : base_("/v/" + newToken().substr(0, 16) + "/") {}

void Access::setIdle(std::chrono::milliseconds idle) {
    std::lock_guard<std::mutex> lk(mu_);
    idle_ = std::max(std::chrono::milliseconds(0), idle);
}

std::chrono::milliseconds Access::idle() const {
    std::lock_guard<std::mutex> lk(mu_);
    return idle_;
}

void Access::setAsking(bool asking) {
    std::lock_guard<std::mutex> lk(mu_);
    asking_ = asking;
}

void Access::setRemembered(std::function<bool(const std::string&)> f) {
    std::lock_guard<std::mutex> lk(mu_);
    remembered_ = std::move(f);
}

std::string Access::newLink() {
    const std::string t = newToken();
    addLink(t);
    return t;
}

void Access::addLink(const std::string& token) {
    std::lock_guard<std::mutex> lk(mu_);
    links_.push_back(token);
    while (links_.size() > kMaxLinks) links_.pop_front();
}

std::string Access::liveLink() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!links_.empty()) return links_.back();
    }
    return newLink();
}

bool Access::live(const Cred& c, Clock::time_point now) const {
    return idle_.count() == 0 || now - c.last < idle_;
}

// Every entry compared, each in constant time, so how long a look takes
// says nothing of how much of a guess was right.
Access::Cred* Access::find(const std::string& credential) {
    Cred* found = nullptr;
    if (credential.empty()) return nullptr;
    for (Cred& c : creds_)
        if (IdeLink::sameSecret(c.value, credential)) found = &c;
    return found;
}

const Access::Cred* Access::find(const std::string& credential) const {
    return const_cast<Access*>(this)->find(credential);
}

Access::Exchange Access::exchange(const std::string& token, const std::string& browserId,
                                  const std::string& userAgent, const std::string& peer,
                                  int peerPort, Clock::time_point now) {
    Exchange x;
    std::unique_lock<std::mutex> lk(mu_);
    auto link = links_.end();
    if (!token.empty())
        for (auto it = links_.begin(); it != links_.end(); ++it)
            if (IdeLink::sameSecret(*it, token)) link = it;
    if (link == links_.end()) {
        bool spent = false;
        if (!token.empty())
            for (const std::string& s : spent_) spent = spent || IdeLink::sameSecret(s, token);
        x.kind = spent ? Exchange::Spent : Exchange::Wrong;
        x.why = spent ? "this link was used already: open a new link from mutible"
                      : "wrong token: open the address mutible printed";
        return x;
    }
    x.browserId = hexOnly(browserId, 32) ? browserId : std::string();
    if (x.browserId.empty()) {
        x.browserId = newToken().substr(0, 32);
        x.newBrowser = true;
    }
    // Whether the app asks: a browser it remembered never waits.
    bool ask = asking_;
    if (ask && !x.newBrowser && remembered_) {
        auto remembered = remembered_;
        const std::string id = x.browserId;
        lk.unlock();   // the app's own lookup, without our lock
        const bool known = remembered(id);
        lk.lock();
        ask = !known;
        // The link may have gone meanwhile (another exchange of it).
        link = links_.end();
        for (auto it = links_.begin(); it != links_.end(); ++it)
            if (IdeLink::sameSecret(*it, token)) link = it;
        if (link == links_.end()) {
            x.kind = Exchange::Spent;
            x.why = "this link was used already: open a new link from mutible";
            return x;
        }
    }
    if (ask) {
        // Only so many waiting, and only so many in a while.
        dropStale(now);
        while (!asked_.empty() && now - asked_.front() >= kQuestionWindow) asked_.pop_front();
        if (waiting_.size() >= kMaxWaiting) {
            x.kind = Exchange::Busy;
            x.why = "mutible is asking about other browsers now: try again once they are answered";
            return x;
        }
        if ((int)asked_.size() >= kMaxQuestions) {
            x.kind = Exchange::Busy;
            x.why = "too many browsers asked to connect: try again in a few minutes";
            return x;
        }
    }
    links_.erase(link);
    spent_.push_back(token);
    while (spent_.size() > kMaxSpent) spent_.pop_front();
    Cred c;
    c.value = newToken();
    c.browserId = x.browserId;
    c.last = now;
    c.state = ask ? State::Pending : State::Allowed;
    if (ask) {
        asked_.push_back(now);
        Question q;
        q.id = ++nextQuestion_;
        q.browserId = x.browserId;
        q.browser = browserSummary(userAgent);
        q.peer = peer;
        q.peerPort = peerPort;
        waiting_.push_back(q);
        c.question = q.id;
        x.question = q.id;
    }
    creds_.push_back(c);
    while (creds_.size() > kMaxCredentials) creds_.pop_front();
    x.kind = Exchange::Ok;
    x.credential = c.value;
    x.state = c.state;
    return x;
}

Access::State Access::state(const std::string& credential, Clock::time_point now) const {
    std::lock_guard<std::mutex> lk(mu_);
    const Cred* c = find(credential);
    if (!c || !live(*c, now)) return State::Unknown;
    return c->state;
}

bool Access::touch(const std::string& credential, Clock::time_point now) {
    std::lock_guard<std::mutex> lk(mu_);
    Cred* c = find(credential);
    if (!c || !live(*c, now)) return false;
    c->last = now;
    return true;
}

bool Access::answer(std::uint64_t id, bool allow) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!id) return false;
    waiting_.erase(std::remove_if(waiting_.begin(), waiting_.end(),
                                  [&](const Question& q) { return q.id == id; }),
                   waiting_.end());
    for (Cred& c : creds_)
        if (c.question == id && c.state == State::Pending) {
            c.state = allow ? State::Allowed : State::Denied;
            c.question = 0;
            return true;
        }
    return false;
}

Access::Question Access::waiting() const {
    std::lock_guard<std::mutex> lk(mu_);
    return waiting_.empty() ? Question() : waiting_.front();
}

std::size_t Access::questions() const {
    std::lock_guard<std::mutex> lk(mu_);
    return waiting_.size();
}

void Access::dropStale(Clock::time_point now) {
    for (auto q = waiting_.begin(); q != waiting_.end();) {
        Cred* w = nullptr;
        for (Cred& c : creds_)
            if (c.question == q->id) w = &c;
        if (w && live(*w, now)) {
            ++q;
            continue;
        }
        // Its page went quiet past its idle time: an answer to it comes
        // too late.
        if (w) w->question = 0;
        q = waiting_.erase(q);
    }
}

std::vector<std::string> Access::expire(Clock::time_point now) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> gone;
    for (auto it = creds_.begin(); it != creds_.end();) {
        if (live(*it, now)) {
            ++it;
            continue;
        }
        const std::uint64_t q = it->question;
        if (q)
            waiting_.erase(std::remove_if(waiting_.begin(), waiting_.end(),
                                          [&](const Question& w) { return w.id == q; }),
                           waiting_.end());
        gone.push_back(it->value);
        it = creds_.erase(it);
    }
    return gone;
}

}  // namespace Web
}  // namespace Shared
