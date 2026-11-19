// SessionWebAccess.h — who may use a browser's view of a session
// (SessionWeb.h): the links a person opens, the credentials the page trades
// them for, the cookies that carry those, the idle expiry, and the
// questions the app asks before a browser it does not know connects.
//
// A link (http://127.0.0.1:PORT/v/ID/?t=TOKEN) is good for one exchange:
// the page's first load trades its token for a credential, an HttpOnly,
// SameSite=Strict cookie scoped to the view's own path (/v/ID/, ID made at
// the server's start, so no other server on the machine gets it: cookies
// are not kept apart by port), and the page's script takes the token out
// of the address bar and the history entry (history.replaceState to
// /v/ID/). Every later request, the socket, a file for a viewer, a reload,
// carries the credential instead; the token is spent and refused if it
// comes again. The app makes a fresh link each time one is asked for (the
// Mac's Show and Copy), and a headless host prints the next one after each
// exchange (Options::nextLink).
//
// A credential ends after a time with nothing from its browser (no request,
// no message on its socket; what the server sends does not count), set by
// editor.session-web-idle-minutes (60, 0 for never). Its page is then told
// it expired and its socket closed.
//
// With someone to ask (the Mac: Options::ask), a browser not remembered
// gets a credential that waits: its page says so until the app's question
// is answered, Allow (the page goes on) or Deny (it is refused, for good).
// A browser is known by a random id in a cookie of its own
// (kBrowserCookie, set at its first exchange); remembering one is the
// app's (Options::remembered). The app is asked one question at a time,
// the others waiting their turn, at most kMaxWaiting in all, and at most
// kMaxQuestions are put in kQuestionWindow: past either, an exchange is
// turned away (and its link kept), so no page can make the app ask in a
// loop. With no one to ask (`mutible host --web`), the link alone admits.
//
// Pure C++17; every method may be called from any thread. The clock is
// passed in, so the checks run it on a timeline of their own.
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Shared {
namespace Web {

// ---- cookies
// The credential's cookie for the server listening on `port`.
std::string credentialCookie(int port);
// The browser's own id, shared by every view on the machine (Path=/v/), so
// a browser the app remembered is known again on another port.
extern const char* const kBrowserCookie;
// The value of cookie `name` in a Cookie header ("" for none; the first
// when it is there twice).
std::string cookieValue(const std::string& header, const std::string& name);
// A Set-Cookie header line, CRLF included: HttpOnly, the SameSite given
// ("Strict" or "Lax"), Path=`path`, and Max-Age when `maxAge` > 0 (0: the
// browser drops it when it quits).
std::string setCookieHeader(const std::string& name, const std::string& value,
                            const std::string& path, const std::string& sameSite,
                            long long maxAge);
// How long a browser's id is kept: 400 days, the most browsers keep.
constexpr long long kBrowserCookieSeconds = 400LL * 24 * 3600;

// A short name for a browser from its User-Agent, for the app's question:
// "Safari on macOS", "Chrome on Android", "Firefox on Linux", "a WebKit
// browser on macOS", and so on; "an unknown browser" when nothing is known.
std::string browserSummary(const std::string& userAgent);

class Access {
public:
    using Clock = std::chrono::steady_clock;
    // A credential's state. Unknown: none such (never made, expired and
    // let go, or from a server that has since stopped).
    enum class State { Unknown, Pending, Allowed, Denied };

    // A question for the app: may this browser connect?
    struct Question {
        std::uint64_t id = 0;
        std::string browserId;   // its cookie's id, for remembering it
        std::string browser;     // browserSummary of its User-Agent
        std::string peer;        // the connection's address, "127.0.0.1:PORT"
        int peerPort = 0;        // that port (the app may look up who holds it)
    };

    // What an exchange of a link's token came to.
    struct Exchange {
        enum Kind {
            Ok,      // a credential made (Allowed, or Pending with a question)
            Spent,   // the link was used already
            Wrong,   // no such link
            Busy,    // too many questions wait, or were asked lately: try later
        };
        Kind kind = Wrong;
        std::string credential;
        State state = State::Unknown;
        std::string browserId;    // the browser's id (made when it had none)
        bool newBrowser = false;  // it had none: set its cookie
        std::uint64_t question = 0;
        std::string why;          // for a refusal
    };

    static constexpr std::size_t kMaxLinks = 8;          // live, unspent
    static constexpr std::size_t kMaxSpent = 64;         // remembered as used
    static constexpr std::size_t kMaxCredentials = 64;
    static constexpr std::size_t kMaxWaiting = 3;
    static constexpr int kMaxQuestions = 10;
    static constexpr std::chrono::minutes kQuestionWindow{10};

    Access();
    // The view's own path, "/v/ID/": every address it serves is under it.
    const std::string& base() const { return base_; }

    // ---- settings
    // How long a credential lasts with nothing from its browser; 0 for ever.
    void setIdle(std::chrono::milliseconds idle);
    std::chrono::milliseconds idle() const;
    // Whether a browser not remembered waits for the app's answer.
    void setAsking(bool asking);
    // Whether the app remembered a browser id as allowed (any thread).
    void setRemembered(std::function<bool(const std::string& browserId)> f);

    // ---- links
    // A fresh token, good until it is exchanged once. The oldest of more
    // than kMaxLinks unspent ones is dropped.
    std::string newLink();
    // The newest unspent token, made when there is none.
    std::string liveLink();
    // Adds a token of the caller's (checks).
    void addLink(const std::string& token);

    // ---- exchanging and checking
    // Trades a link's token for a credential: spent on Ok only.
    Exchange exchange(const std::string& token, const std::string& browserId,
                      const std::string& userAgent, const std::string& peer, int peerPort,
                      Clock::time_point now);
    // A credential's state now (Unknown once its idle time has passed).
    State state(const std::string& credential, Clock::time_point now) const;
    // Something came from the credential's browser: its idle time starts
    // again. False when it is not a live credential.
    bool touch(const std::string& credential, Clock::time_point now);
    // The app's answer to question `id`: its credential Allowed or Denied.
    // False when nothing waits for it (answered, or its credential gone).
    bool answer(std::uint64_t id, bool allow);
    // The question to put to the app now, the oldest unanswered (id 0 for
    // none); the next comes once it is answered.
    Question waiting() const;
    // How many wait, that one included.
    std::size_t questions() const;
    // Lets go of credentials whose idle time passed; returns them, so their
    // pages can be told.
    std::vector<std::string> expire(Clock::time_point now);

private:
    struct Cred {
        std::string value, browserId;
        State state = State::Pending;
        Clock::time_point last;
        std::uint64_t question = 0;
    };
    mutable std::mutex mu_;
    std::string base_;
    std::chrono::milliseconds idle_{60 * 60 * 1000};
    bool asking_ = false;
    std::function<bool(const std::string&)> remembered_;
    std::deque<std::string> links_, spent_;
    std::deque<Cred> creds_;
    std::deque<Clock::time_point> asked_;   // when each question was put
    std::deque<Question> waiting_;   // unanswered, oldest first
    // Drops questions whose credential has gone or passed its idle time.
    void dropStale(Clock::time_point now);
    std::uint64_t nextQuestion_ = 0;
    Cred* find(const std::string& credential);
    const Cred* find(const std::string& credential) const;
    bool live(const Cred& c, Clock::time_point now) const;
};

}  // namespace Web
}  // namespace Shared
