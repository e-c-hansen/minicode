// TextFind.cpp — see TextFind.h.
#include "TextFind.h"

#include "Regex.h"

#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <regex>

static_assert(sizeof(wchar_t) == 4, "TextFind runs std::wregex on UTF-32 code points");

namespace TextFind {

char32_t foldCase(char32_t c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x0100 && c <= 0x017F) {
        if ((c <= 0x012F) || (c >= 0x0132 && c <= 0x0137) || (c >= 0x014A && c <= 0x0177))
            return (c % 2 == 0) ? c + 1 : c;
        if ((c >= 0x0139 && c <= 0x0148) || (c >= 0x0179 && c <= 0x017E))
            return (c % 2 == 1) ? c + 1 : c;
        if (c == 0x0178) return 0x00FF;
        return c;
    }
    if (c >= 0x0391 && c <= 0x03A9 && c != 0x03A2) return c + 32;
    if (c == 0x0386) return 0x03AC;
    if (c >= 0x0388 && c <= 0x038A) return c + 37;
    if (c == 0x038C) return 0x03CC;
    if (c == 0x038E || c == 0x038F) return c + 63;
    if (c == 0x03C2) return 0x03C3;   // final sigma matches sigma
    if (c >= 0x0410 && c <= 0x042F) return c + 32;
    if (c >= 0x0400 && c <= 0x040F) return c + 80;
    return c;
}

char32_t upperCase(char32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x0178;
    if (c >= 0x0100 && c <= 0x017F) {
        if ((c <= 0x012F) || (c >= 0x0132 && c <= 0x0137) || (c >= 0x014A && c <= 0x0177))
            return (c % 2 == 1) ? c - 1 : c;
        if ((c >= 0x0139 && c <= 0x0148) || (c >= 0x0179 && c <= 0x017E))
            return (c % 2 == 0) ? c - 1 : c;
        return c;
    }
    if (c >= 0x03B1 && c <= 0x03C9 && c != 0x03C2) return c - 32;
    if (c == 0x03C2) return 0x03A3;
    if (c == 0x03AC) return 0x0386;
    if (c >= 0x03AD && c <= 0x03AF) return c - 37;
    if (c == 0x03CC) return 0x038C;
    if (c == 0x03CD || c == 0x03CE) return c - 63;
    if (c >= 0x0430 && c <= 0x044F) return c - 32;
    if (c >= 0x0450 && c <= 0x045F) return c - 80;
    return c;
}

bool isWordChar(char32_t c) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')
        return true;
    if (c < 0x80) return false;
    // Past ASCII everything is a word character but the spaces.
    return !(c == 0x00A0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
             c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF);
}

namespace {

// std::regex's case-insensitive matching asks the traits to fold; the
// standard ones ask the C locale, which folds ASCII only.
struct FoldTraits : std::regex_traits<wchar_t> {
    wchar_t translate_nocase(wchar_t c) const {
        return static_cast<wchar_t>(foldCase(static_cast<char32_t>(c)));
    }
};
using StdRegex = std::basic_regex<wchar_t, FoldTraits>;

// A line longer than this (code points) is searched one start position at
// a time, so a regex that backtracks over it can be stopped between them:
// after kRegexLineSeconds plus kRegexSecondsPerChar for each character
// (an ordinary regex takes well under that), or when the caller's cancel
// flag goes up. Shorter lines are searched in one call each.
constexpr std::size_t kSteppedLine = 2000;
constexpr double kRegexLineSeconds = 1.0;
constexpr double kRegexSecondsPerChar = 0.5e-6;
// How far past a start position std::regex reads on a stepped line: a
// match is at most this long. libstdc++ before GCC 16 recurses at least
// once per character it reads (GCC bug 86164), so the whole rest of a
// long line could overflow even the worker's 256 MB stack (it did under
// ThreadSanitizer on a 50,000-character line), and a thread overflowing
// its stack ends the app. Past the window $ and \b do not match at its
// end, which is not the line's.
std::atomic<std::size_t> gRegexReach{8192};

constexpr std::size_t npos = std::string::npos;

// ------------------------------------------------------------- decoding
// A line as code points, with each code point's offset in the source units
// (one more entry than code points: the line's end).
struct Line {
    std::u32string cps;
    std::vector<std::size_t> offs;
};

void decode16(const char16_t* s, std::size_t n, Line& out) {
    out.cps.clear();
    out.offs.clear();
    for (std::size_t i = 0; i < n;) {
        char32_t c = s[i];
        std::size_t len = 1;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            len = 2;
        }
        out.cps.push_back(c);
        out.offs.push_back(i);
        i += len;
    }
    out.offs.push_back(n);
}

void decode8(const char* s, std::size_t n, Line& out) {
    out.cps.clear();
    out.offs.clear();
    for (std::size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        char32_t cp;
        if (len == 1 || i + len > n) {
            cp = c;   // ASCII, or a stray byte taken as it is
            len = 1;
        } else {
            cp = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
            for (std::size_t k = 1; k < len; ++k)
                cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        out.cps.push_back(cp);
        out.offs.push_back(i);
        i += len;
    }
    out.offs.push_back(n);
}

std::u32string toU32(const std::string& s) {
    Line l;
    decode8(s.data(), s.size(), l);
    return l.cps;
}

std::u32string toU32(const std::u16string& s) {
    Line l;
    decode16(s.data(), s.size(), l);
    return l.cps;
}

// The end of the line starting at `pos` and where the next one starts:
// \n, \r\n, \r and U+2029, as FolderSearch splits lines.
template <class Ch>
std::size_t lineEnd(const std::basic_string<Ch>& t, std::size_t pos, std::size_t* next);

template <>
std::size_t lineEnd(const std::u16string& t, std::size_t pos, std::size_t* next) {
    const std::size_t n = t.size();
    for (std::size_t e = pos; e < n; ++e) {
        const char16_t c = t[e];
        if (c == u'\n' || c == 0x2029) { *next = e + 1; return e; }
        if (c == u'\r') { *next = (e + 1 < n && t[e + 1] == u'\n') ? e + 2 : e + 1; return e; }
    }
    *next = n;
    return n;
}

template <>
std::size_t lineEnd(const std::string& t, std::size_t pos, std::size_t* next) {
    const std::size_t n = t.size();
    for (std::size_t e = pos; e < n; ++e) {
        const char c = t[e];
        if (c == '\n') { *next = e + 1; return e; }
        if (c == '\r') { *next = (e + 1 < n && t[e + 1] == '\n') ? e + 2 : e + 1; return e; }
        if (c == '\xE2' && e + 2 < n && t[e + 1] == '\x80' && t[e + 2] == '\xA9') {
            *next = e + 3;
            return e;
        }
    }
    *next = n;
    return n;
}

}  // namespace

// ------------------------------------------------------------------ the query
namespace {

// How long a search through std::regex may take before it is left behind.
std::atomic<double> gSlowSeconds{2.0};
// Threads left behind that are still running, and how many may be.
std::atomic<int> gLeftBehind{0};
constexpr int kMaxLeftBehind = 4;

// One thread that runs std::regex searches for a query, so the caller can
// stop waiting for one. It is shared with the thread, which outlives the
// query when it is left behind.
struct Worker {
    std::mutex m;
    std::condition_variable cv;
    std::function<void()> job;
    std::uint64_t posted = 0, finished = 0;
    bool quit = false;
    bool leftBehind = false;
};

void* workerMain(void* arg) {
    auto* hold = static_cast<std::shared_ptr<Worker>*>(arg);
    std::shared_ptr<Worker> w = std::move(*hold);
    delete hold;
    std::unique_lock<std::mutex> lock(w->m);
    while (true) {
        w->cv.wait(lock, [&] { return w->quit || w->finished < w->posted; });
        if (w->finished < w->posted) {
            std::function<void()> job = std::move(w->job);
            w->job = nullptr;
            lock.unlock();
            job();
            job = nullptr;   // what it held goes outside the lock
            lock.lock();
            ++w->finished;
            w->cv.notify_all();
            continue;
        }
        break;
    }
    if (w->leftBehind) gLeftBehind.fetch_sub(1);
    return nullptr;
}

std::shared_ptr<Worker> startWorker() {
    auto w = std::make_shared<Worker>();
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return nullptr;
    // libstdc++'s std::regex recurses per character; a secondary thread's
    // default stack (512 KB on macOS) is small for a long line. 256 MB is
    // only reserved, not used, and holds a long line's recursion even under
    // AddressSanitizer, whose frames are several times larger (16 MB
    // overflowed in CI's sanitizer job on find:regex-long-lines).
    pthread_attr_setstacksize(&attr, size_t(256) << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    auto* hold = new std::shared_ptr<Worker>(w);
    pthread_t thread;
    const int err = pthread_create(&thread, &attr, workerMain, hold);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        delete hold;
        return nullptr;
    }
    return w;
}

}  // namespace

// A pattern the linear engine refuses: std::regex, run on a worker.
struct Fallback {
    std::shared_ptr<const StdRegex> re;
    std::mutex callers;   // one search at a time
    std::shared_ptr<Worker> worker;
    std::atomic<bool> tooSlow{false};

    ~Fallback() {
        if (!worker) return;
        std::lock_guard<std::mutex> lock(worker->m);
        worker->quit = true;
        worker->cv.notify_all();
    }

    enum class Ran { Done, Cancelled, TooSlow };

    // Runs job(stop) on the worker and waits for it, at most gSlowSeconds
    // (or until `cancel` goes up). A job not done by then is told to stop
    // and left to finish on its own; its results are never read, so it
    // must own everything it touches. Past the time limit every later
    // search with this query finds nothing (tooSlow).
    Ran run(const std::function<void(const std::atomic<bool>&)>& job,
            const std::atomic<bool>* cancel) {
        if (tooSlow.load()) return Ran::TooSlow;
        std::lock_guard<std::mutex> one(callers);
        if (tooSlow.load()) return Ran::TooSlow;
        auto stop = std::make_shared<std::atomic<bool>>(false);
        if (!worker) {
            if (gLeftBehind.load() >= kMaxLeftBehind) {
                tooSlow = true;
                return Ran::TooSlow;
            }
            worker = startWorker();
            if (!worker) {   // no thread to be had: run it here, unguarded
                job(*stop);
                return Ran::Done;
            }
        }
        using Clock = std::chrono::steady_clock;
        const auto deadline =
            Clock::now() + std::chrono::duration_cast<Clock::duration>(
                               std::chrono::duration<double>(gSlowSeconds.load()));
        std::unique_lock<std::mutex> lock(worker->m);
        worker->job = [job, stop] { job(*stop); };
        const std::uint64_t ticket = ++worker->posted;
        worker->cv.notify_all();
        bool cancelled = false;
        while (worker->finished < ticket) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                cancelled = true;
                break;
            }
            const auto now = Clock::now();
            if (now >= deadline) break;
            worker->cv.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(20)));
        }
        if (worker->finished >= ticket) return cancelled ? Ran::Cancelled : Ran::Done;
        stop->store(true);
        if (cancelled) {   // it polls `stop` between lines: give it a moment
            worker->cv.wait_for(lock, std::chrono::milliseconds(200),
                                [&] { return worker->finished >= ticket; });
            if (worker->finished >= ticket) return Ran::Cancelled;
        }
        worker->quit = true;
        worker->leftBehind = true;
        gLeftBehind.fetch_add(1);
        lock.unlock();
        worker.reset();
        if (!cancelled) tooSlow = true;
        return cancelled ? Ran::Cancelled : Ran::TooSlow;
    }
};

struct Query::Impl {
    Options opt;
    std::u32string needle;   // plain text, folded unless case-sensitive
    std::shared_ptr<const Regex::Program> vm;   // the linear engine
    std::shared_ptr<Fallback> slow;             // else std::regex
};

static std::shared_ptr<const Query::Impl> compile(const std::u32string& pattern,
                                                  const Options& options, bool linear,
                                                  std::string* error) {
    if (pattern.empty()) return nullptr;
    auto impl = std::make_shared<Query::Impl>();
    impl->opt = options;
    if (options.regex) {
        // std::regex decides what is a valid pattern, and what it says
        // about one that is not, as before the linear engine.
        auto flags = std::regex_constants::ECMAScript;
        if (!options.caseSensitive) flags |= std::regex_constants::icase;
        std::shared_ptr<const StdRegex> re;
        try {
            re = std::make_shared<const StdRegex>(std::wstring(pattern.begin(), pattern.end()),
                                                  flags);
        } catch (const std::regex_error& e) {
            *error = std::string("Invalid regular expression: ") + e.what();
            return nullptr;
        }
        if (linear) impl->vm = Regex::compile(pattern, !options.caseSensitive);
        if (!impl->vm) {
            impl->slow = std::make_shared<Fallback>();
            impl->slow->re = std::move(re);
        }
    } else {
        impl->needle.reserve(pattern.size());
        for (char32_t c : pattern) impl->needle.push_back(options.caseSensitive ? c : foldCase(c));
    }
    return impl;
}

Query Query::make(const std::string& pattern, const Options& options) {
    Query q;
    q.options_ = options;
    q.impl_ = compile(toU32(pattern), options, true, &q.error_);
    return q;
}

Query Query::make(const std::u16string& pattern, const Options& options) {
    Query q;
    q.options_ = options;
    q.impl_ = compile(toU32(pattern), options, true, &q.error_);
    return q;
}

Query Query::makeBacktracking(const std::u16string& pattern, const Options& options) {
    Query q;
    q.options_ = options;
    q.impl_ = compile(toU32(pattern), options, false, &q.error_);
    return q;
}

bool Query::linear() const { return impl_ && impl_->vm; }

std::u32string requiredText(const Query& query) {
    const Query::Impl* q = query.impl();
    if (!q) return std::u32string();
    if (!q->opt.regex) return q->needle;
    return q->vm ? Regex::requiredText(*q->vm) : std::u32string();
}

bool Query::tooSlow() const { return impl_ && impl_->slow && impl_->slow->tooSlow.load(); }

void setSlowPatternSeconds(double seconds) { gSlowSeconds = seconds; }
std::size_t setRegexReach(std::size_t codePoints) { return gRegexReach.exchange(codePoints); }

namespace {

// A match in code points of one line.
struct CpMatch {
    std::size_t start, length;
    std::vector<std::pair<std::size_t, std::size_t>> groups;
};

bool wholeWordAt(const std::u32string& l, std::size_t s, std::size_t len) {
    const std::size_t e = s + len;
    auto word = [&](std::size_t i) { return isWordChar(l[i]); };
    if (s > 0 && word(s - 1) && (len == 0 || word(s))) return false;
    if (e < l.size() && word(e) && (len == 0 || word(e - 1))) return false;
    return true;
}

std::u32string folded(const std::u32string& line) {
    std::u32string f(line.size(), U'\0');
    for (std::size_t i = 0; i < line.size(); ++i) f[i] = foldCase(line[i]);
    return f;
}

// Plain text: every match of the needle.
void matchPlain(const Query::Impl& q, const std::u32string& line, std::vector<CpMatch>& out,
                std::size_t limit) {
    const std::size_t n = line.size();
    const std::u32string& needle = q.needle;
    const std::size_t m = needle.size();
    if (m == 0 || n < m) return;
    std::u32string f;
    const std::u32string* hay = &line;
    if (!q.opt.caseSensitive) {
        f = folded(line);
        hay = &f;
    }
    std::size_t pos = 0;
    while (pos + m <= n && !(limit && out.size() >= limit)) {
        const std::size_t at = hay->find(needle, pos);
        if (at == std::u32string::npos) break;
        if (q.opt.wholeWord && !wholeWordAt(line, at, m)) {
            pos = at + 1;
            continue;
        }
        out.push_back({at, m, {{at, m}}});
        pos = at + m;
    }
}

// Matches one after another, stepping past an empty match as
// std::regex_iterator and JavaScript do: the next search starts where a
// match ended, and after an empty one first asks for a non-empty match
// there, then searches from one further on. Ignoring case, the line is
// folded to lower case (one code point for one, so positions hold) and so
// is the pattern. Returns false when `cancel` stopped it.
bool matchLinear(const Query::Impl& q, const std::u32string& line, std::vector<CpMatch>& out,
                 std::size_t limit, const std::atomic<bool>* cancel) {
    const std::size_t n = line.size();
    std::u32string f;
    const std::u32string* subject = &line;
    if (!q.opt.caseSensitive) {
        f = folded(line);
        subject = &f;
    }
    const char32_t* s = subject->data();
    const Regex::Program& prog = *q.vm;
    std::vector<std::size_t> caps;
    std::size_t from = 0;
    bool notNull = false;
    while (from <= n && !(limit && out.size() >= limit)) {
        Regex::Result r;
        if (notNull) {
            Regex::SearchOptions o;
            o.anchored = o.notEmpty = true;
            r = Regex::search(prog, s, n, from, o, caps, cancel);
            if (r == Regex::Result::NotFound) {
                if (from >= n) break;
                r = Regex::search(prog, s, n, from + 1, {}, caps, cancel);
            }
        } else {
            r = Regex::search(prog, s, n, from, {}, caps, cancel);
        }
        if (r == Regex::Result::Stopped) return false;
        if (r == Regex::Result::NotFound) break;
        const std::size_t st = caps[0], len = caps[1] - caps[0];
        from = st + len;
        notNull = len == 0;
        if (q.opt.wholeWord && !wholeWordAt(line, st, len)) continue;
        CpMatch cm{st, len, {}};
        cm.groups.reserve(caps.size() / 2);
        for (std::size_t k = 0; k + 1 < caps.size(); k += 2) {
            if (caps[k] == Regex::npos || caps[k + 1] == Regex::npos)
                cm.groups.push_back({npos, 0});
            else
                cm.groups.push_back({caps[k], caps[k + 1] - caps[k]});
        }
        out.push_back(std::move(cm));
    }
    return true;
}

// The same through std::regex, for the patterns the linear engine refuses.
// Over pointers, not string iterators: libc++ copies the rest of the line
// for every search through any other iterator type, which made a line with
// many matches quadratic. A search from inside the line has the character
// before it (\b sees it) and is not at the line's start (^ does not match
// there). libstdc++ tests a bracket range such as [а-я] through the
// locale's ctype, not the traits, and the C locale folds only ASCII;
// folded text puts the character itself in the lower-case range. A line
// over kSteppedLine is searched a start position at a time, polling `cancel`
// and giving up after about a second. Returns false when `cancel` stopped it.
bool matchBacktracking(const Options& opt, const StdRegex& re, const std::u32string& line32,
                       std::vector<CpMatch>& out, std::size_t limit,
                       const std::atomic<bool>* cancel) {
    namespace rc = std::regex_constants;
    using Results = std::match_results<const wchar_t*>;
    const std::size_t n = line32.size();
    auto full = [&] { return limit && out.size() >= limit; };
    const std::u32string f = opt.caseSensitive ? std::u32string() : folded(line32);
    const std::u32string& src = opt.caseSensitive ? line32 : f;
    const std::wstring subject(src.begin(), src.end());
    const wchar_t* const b = subject.data();
    const wchar_t* const e = b + n;
    const bool stepped = n > kSteppedLine;
    const std::size_t reach = std::max<std::size_t>(gRegexReach.load(), 1);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(kRegexLineSeconds +
                                                            kRegexSecondsPerChar * (double)n));
    bool stop = false, stopped = false;
    // From inside the line: match_prev_avail (\b looks at the character
    // before), and ^ must not match. libstdc++ takes that from
    // match_prev_avail itself (or match_not_bol); libc++ ignores
    // match_not_bol beside it and lets ^ match at the start of any search,
    // except one flagged with its own __no_update_pos, which is how its
    // regex_iterator searches on from a match.
    auto flagsAt = [](std::size_t p) {
        if (p == 0) return rc::match_default;
#ifdef _LIBCPP_VERSION
        return rc::match_prev_avail | rc::match_not_bol | rc::__no_update_pos;
#else
        return rc::match_prev_avail | rc::match_not_bol;
#endif
    };
    // The first match starting at or after `from` (not an empty one at
    // `from` when notNull), into m.
    auto next = [&](std::size_t from, bool notNull, Results& m) -> bool {
        if (!stepped) {
            if (notNull) {
                if (std::regex_search(b + from, e, m, re,
                                      flagsAt(from) | rc::match_not_null | rc::match_continuous))
                    return true;
                if (from >= n) return false;
                ++from;
            }
            return std::regex_search(b + from, e, m, re, flagsAt(from));
        }
        for (std::size_t p = from; p <= n; ++p) {
            // Before every attempt: one can itself cost a pass over the line.
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                stop = stopped = true;
                return false;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                stop = true;
                return false;
            }
            auto fl = flagsAt(p) | rc::match_continuous;
            if (p == from && notNull) fl |= rc::match_not_null;
            const wchar_t* end = e;
            if (n - p > reach) {
                end = b + p + reach;
                fl |= rc::match_not_eol | rc::match_not_eow;
            }
            if (std::regex_search(b + p, end, m, re, fl)) return true;
        }
        return false;
    };
    try {
        Results mr;
        std::size_t from = 0;
        bool notNull = false;
        while (from <= n && !full() && !stop && next(from, notNull, mr)) {
            const std::size_t s = static_cast<std::size_t>(mr[0].first - b);
            const std::size_t len = static_cast<std::size_t>(mr[0].second - mr[0].first);
            from = s + len;
            notNull = len == 0;
            if (opt.wholeWord && !wholeWordAt(line32, s, len)) continue;
            CpMatch cm{s, len, {}};
            for (std::size_t k = 0; k < mr.size(); ++k) {
                if (mr[k].matched)
                    cm.groups.push_back({static_cast<std::size_t>(mr[k].first - b),
                                         static_cast<std::size_t>(mr[k].second - mr[k].first)});
                else
                    cm.groups.push_back({npos, 0});
            }
            out.push_back(std::move(cm));
        }
    } catch (const std::exception&) {
        // Too complex for std::regex: this line has no more.
    }
    return !stopped;
}

// Every match of `q` in one line of code points, at most `limit` (0 = all).
// False when `cancel` stopped it within the line (what it found is kept).
// This runs std::regex where the caller stands: matchLineGuarded and
// findAll put it on the worker.
bool matchLine(const Query::Impl& q, const std::u32string& line, std::vector<CpMatch>& out,
               std::size_t limit, const std::atomic<bool>* cancel = nullptr) {
    if (q.vm) return matchLinear(q, line, out, limit, cancel);
    if (q.slow) return matchBacktracking(q.opt, *q.slow->re, line, out, limit, cancel);
    matchPlain(q, line, out, limit);
    return true;
}

// Every match in one line, std::regex's through the worker. False when it
// did not finish (too slow, or cancelled). The job holds copies of what it
// reads, since it may outlive the call.
bool matchLineGuarded(const Query::Impl& q, const std::u32string& line,
                      std::vector<CpMatch>& out, std::size_t limit,
                      const std::atomic<bool>* cancel = nullptr) {
    if (!q.slow) return matchLine(q, line, out, limit, cancel);
    auto result = std::make_shared<std::vector<CpMatch>>();
    auto lineCopy = std::make_shared<const std::u32string>(line);
    const Options opt = q.opt;
    const std::shared_ptr<const StdRegex> re = q.slow->re;
    const Fallback::Ran ran = q.slow->run(
        [opt, re, lineCopy, result, limit](const std::atomic<bool>& stop) {
            matchBacktracking(opt, *re, *lineCopy, *result, limit, &stop);
        },
        cancel);
    if (ran != Fallback::Ran::Done) return false;
    out.insert(out.end(), result->begin(), result->end());
    return true;
}

// Code point matches to matches in the line's units, offset by `base`.
void toUnits(const std::vector<CpMatch>& cms, const Line& l, std::size_t base,
             std::vector<Match>& out) {
    for (const CpMatch& c : cms) {
        Match m;
        m.start = base + l.offs[c.start];
        m.length = l.offs[c.start + c.length] - l.offs[c.start];
        for (const auto& g : c.groups) {
            if (g.first == npos) m.groups.push_back({npos, 0});
            else
                m.groups.push_back({base + l.offs[g.first],
                                    l.offs[g.first + g.second] - l.offs[g.first]});
        }
        out.push_back(std::move(m));
    }
}

template <class S>
S expandRaw(const S& tmpl, const S& text, const Match& m, bool regex) {
    using Ch = typename S::value_type;
    if (!regex) return tmpl;
    S out;
    auto group = [&](std::size_t k) {
        if (k >= m.groups.size() || m.groups[k].first == npos) return;
        out.append(text, m.groups[k].first, m.groups[k].second);
    };
    const std::size_t n = tmpl.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Ch c = tmpl[i];
        if (c == Ch('$') && i + 1 < n) {
            const Ch d = tmpl[i + 1];
            if (d == Ch('$')) { out.push_back(Ch('$')); ++i; continue; }
            if (d == Ch('&')) { group(0); ++i; continue; }
            if (d >= Ch('0') && d <= Ch('9')) {
                std::size_t k = static_cast<std::size_t>(d - Ch('0'));
                std::size_t used = 1;
                if (i + 2 < n && tmpl[i + 2] >= Ch('0') && tmpl[i + 2] <= Ch('9')) {
                    const std::size_t two = k * 10 + static_cast<std::size_t>(tmpl[i + 2] - Ch('0'));
                    if (two < m.groups.size()) { k = two; used = 2; }
                }
                if (k < m.groups.size()) {
                    group(k);
                    i += used;
                    continue;
                }
            }
        } else if (c == Ch('\\') && i + 1 < n) {
            const Ch d = tmpl[i + 1];
            if (d == Ch('n')) { out.push_back(Ch('\n')); ++i; continue; }
            if (d == Ch('t')) { out.push_back(Ch('\t')); ++i; continue; }
            if (d == Ch('\\')) { out.push_back(Ch('\\')); ++i; continue; }
        }
        out.push_back(c);
    }
    return out;
}

// ------------------------------------------------------- preserve case
using U32 = std::u32string;

U32 to32(const std::u16string& s) {
    Line l;
    decode16(s.data(), s.size(), l);
    return U32(l.cps.begin(), l.cps.end());
}
U32 to32(const std::string& s) {
    Line l;
    decode8(s.data(), s.size(), l);
    return U32(l.cps.begin(), l.cps.end());
}
void from32(const U32& u, std::u16string* out) {
    out->clear();
    for (char32_t c : u) {
        if (c >= 0x10000) {
            out->push_back(static_cast<char16_t>(0xD800 + ((c - 0x10000) >> 10)));
            out->push_back(static_cast<char16_t>(0xDC00 + ((c - 0x10000) & 0x3FF)));
        } else {
            out->push_back(static_cast<char16_t>(c));
        }
    }
}
void from32(const U32& u, std::string* out) {
    out->clear();
    for (char32_t c : u) {
        if (c < 0x80) {
            out->push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (c >> 6)));
            out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (c >> 12)));
            out->push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (c >> 18)));
            out->push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
}

U32 upper32(U32 s) {
    for (char32_t& c : s) c = upperCase(c);
    return s;
}
U32 lower32(U32 s) {
    for (char32_t& c : s) c = foldCase(c);
    return s;
}

std::vector<U32> split32(const U32& s, char32_t sep) {
    std::vector<U32> parts(1);
    for (char32_t c : s) {
        if (c == sep) parts.emplace_back();
        else parts.back().push_back(c);
    }
    return parts;
}

U32 casePreserved(const U32& match, const U32& pattern);

// VS Code's validateSpecificSpecialCharacter: both hold the separator, in
// as many parts.
bool sameParts(const U32& match, const U32& pattern, char32_t sep) {
    return match.find(sep) != U32::npos && pattern.find(sep) != U32::npos &&
           split32(match, sep).size() == split32(pattern, sep).size();
}

U32 byParts(const U32& match, const U32& pattern, char32_t sep) {
    const std::vector<U32> ps = split32(pattern, sep), ms = split32(match, sep);
    U32 out;
    for (std::size_t i = 0; i < ps.size(); ++i) {
        if (i) out.push_back(sep);
        out += casePreserved(ms[i], ps[i]);
    }
    return out;
}

// VS Code's buildReplaceStringWithCasePreserved, step for step.
U32 casePreserved(const U32& match, const U32& pattern) {
    if (match.empty()) return pattern;
    const bool hyphens = sameParts(match, pattern, U'-');
    const bool underscores = sameParts(match, pattern, U'_');
    if (hyphens && !underscores) return byParts(match, pattern, U'-');
    if (!hyphens && underscores) return byParts(match, pattern, U'_');
    if (upper32(match) == match) return upper32(pattern);
    if (lower32(match) == match) return lower32(pattern);
    if (pattern.empty()) return pattern;
    const U32 first(1, match[0]);
    if (lower32(first) != first)   // a capital first
        return upper32(pattern.substr(0, 1)) + pattern.substr(1);
    if (upper32(first) != first)   // a small letter first
        return lower32(pattern.substr(0, 1)) + pattern.substr(1);
    return pattern;
}

template <class S>
S preserveCaseT(const S& matched, const S& replacement) {
    S out;
    from32(casePreserved(to32(matched), to32(replacement)), &out);
    return out;
}

// A replacement that takes nothing from the match: VS Code preserves case
// only for those.
template <class S>
bool isStatic(const S& tmpl, bool regex) {
    using Ch = typename S::value_type;
    if (!regex) return true;
    for (std::size_t i = 0; i + 1 < tmpl.size(); ++i) {
        if (tmpl[i] != Ch('$')) continue;
        const Ch d = tmpl[i + 1];
        if (d == Ch('$')) { ++i; continue; }
        if (d == Ch('&') || (d >= Ch('0') && d <= Ch('9'))) return false;
    }
    return true;
}

template <class S>
S expandT(const S& tmpl, const S& text, const Match& m, const Options& o) {
    S out = expandRaw(tmpl, text, m, o.regex);
    if (o.preserveCase && isStatic(tmpl, o.regex))
        out = preserveCaseT(text.substr(m.start, m.length), out);
    return out;
}

}  // namespace

std::u16string preserveCase(const std::u16string& matched, const std::u16string& replacement) {
    return preserveCaseT(matched, replacement);
}
std::string preserveCase(const std::string& matched, const std::string& replacement) {
    return preserveCaseT(matched, replacement);
}

// ----------------------------------------------------------------- UTF-16
namespace {

// findAll over a query's own copy of what it needs, so std::regex's can run
// on the worker. Returns false when cancelled.
bool findAllIn(const std::u16string& text, const Query::Impl& q, std::size_t limit,
               bool* truncated, const std::atomic<bool>* cancel, std::vector<Match>& out) {
    Line l;
    std::vector<CpMatch> cms;
    std::size_t pos = 0, lines = 0;
    const std::size_t n = text.size();
    while (true) {
        if (cancel && (++lines & 255) == 0 && cancel->load(std::memory_order_relaxed))
            return false;
        std::size_t next;
        const std::size_t end = lineEnd(text, pos, &next);
        decode16(text.data() + pos, end - pos, l);
        cms.clear();
        const bool finished = matchLine(q, l.cps, cms, limit ? limit - out.size() : 0, cancel);
        toUnits(cms, l, pos, out);
        if (!finished) return false;
        if (limit && out.size() >= limit) {
            if (truncated) *truncated = true;
            break;
        }
        if (next >= n) {
            // A text ending in a line break has an empty last line (where ^
            // and $ still match).
            if (end < n) {
                pos = n;
                continue;
            }
            break;
        }
        pos = next;
    }
    return true;
}

}  // namespace

std::vector<Match> findAll(const std::u16string& text, const Query& query, std::size_t limit,
                           bool* truncated, const std::atomic<bool>* cancel, bool* cancelled) {
    std::vector<Match> out;
    if (truncated) *truncated = false;
    if (cancelled) *cancelled = false;
    const Query::Impl* q = query.impl();
    if (!q) return out;
    if (!q->slow) {
        if (!findAllIn(text, *q, limit, truncated, cancel, out) && cancelled) *cancelled = true;
        return out;
    }
    // std::regex: the whole search on the worker, over a copy of the text.
    struct Job {
        std::u16string text;
        Query::Impl q;
        std::vector<Match> out;
        bool truncated = false, finished = false;
    };
    auto job = std::make_shared<Job>();
    job->text = text;
    job->q.opt = q->opt;
    job->q.slow = std::make_shared<Fallback>();
    job->q.slow->re = q->slow->re;
    const Fallback::Ran ran = q->slow->run(
        [job, limit](const std::atomic<bool>& stop) {
            job->finished = findAllIn(job->text, job->q, limit, &job->truncated, &stop, job->out);
        },
        cancel);
    if (ran == Fallback::Ran::Cancelled || (ran == Fallback::Ran::Done && !job->finished)) {
        if (cancelled) *cancelled = true;
        return out;
    }
    if (ran != Fallback::Ran::Done) return out;   // too slow: nothing
    if (truncated) *truncated = job->truncated;
    return std::move(job->out);
}

std::u16string expand(const std::u16string& replacement, const std::u16string& text,
                      const Match& match, const Query& query) {
    return expandT(replacement, text, match, query.options());
}

std::vector<Edit16> replaceAll(const std::u16string& text, const Query& query,
                               const std::u16string& replacement) {
    std::vector<Edit16> edits;
    for (const Match& m : findAll(text, query))
        edits.push_back({m.start, m.length, expand(replacement, text, m, query)});
    return edits;
}

std::u16string applyEdits(const std::u16string& text, const std::vector<Edit16>& edits) {
    std::u16string out;
    out.reserve(text.size());
    std::size_t at = 0;
    for (const Edit16& e : edits) {
        if (e.start < at || e.start + e.length > text.size()) continue;   // overlapping
        out.append(text, at, e.start - at);
        out += e.text;
        at = e.start + e.length;
    }
    out.append(text, at, std::u16string::npos);
    return out;
}

long nextIndex(const std::vector<Match>& matches, std::size_t selStart, std::size_t selEnd,
               bool forward) {
    const long n = static_cast<long>(matches.size());
    if (n == 0) return -1;
    const long here = indexOf(matches, selStart, selEnd - selStart);
    if (here >= 0) return forward ? (here + 1) % n : (here - 1 + n) % n;
    if (forward) {
        for (long i = 0; i < n; ++i)
            if (matches[static_cast<std::size_t>(i)].start >= selEnd) return i;
        return 0;
    }
    for (long i = n - 1; i >= 0; --i)
        if (matches[static_cast<std::size_t>(i)].start < selStart) return i;
    return n - 1;
}

long indexOf(const std::vector<Match>& matches, std::size_t start, std::size_t length) {
    auto it = std::lower_bound(matches.begin(), matches.end(), start,
                               [](const Match& m, std::size_t s) { return m.start < s; });
    for (; it != matches.end() && it->start == start; ++it)
        if (it->length == length) return static_cast<long>(it - matches.begin());
    return -1;
}

bool replaceOneAt(const std::u16string& text, const Query& query,
                  const std::u16string& replacement, int line, std::size_t byteColumn,
                  Edit16* out) {
    if (!query.usable() || line < 1) return false;
    std::size_t pos = 0, next = 0, end = 0;
    for (int k = 1;; ++k) {
        end = lineEnd(text, pos, &next);
        if (k == line) break;
        if (next >= text.size()) {
            if (end < text.size() && k + 1 == line) {
                pos = end = next = text.size();
                break;
            }
            return false;
        }
        pos = next;
    }
    // The byte column in UTF-16 units: UTF-8 lengths summed along the line.
    std::size_t at = pos, bytes = 0;
    while (bytes < byteColumn && at < end) {
        const char16_t c = text[at];
        if (c >= 0xD800 && c <= 0xDBFF && at + 1 < end) { bytes += 4; at += 2; }
        else { bytes += c < 0x80 ? 1 : c < 0x800 ? 2 : 3; at += 1; }
    }
    if (bytes != byteColumn) return false;
    const std::u16string lineText = text.substr(pos, end - pos);
    for (const Match& m : findAll(lineText, query)) {
        if (m.start + pos != at) continue;
        out->start = at;
        out->length = m.length;
        out->text = expand(replacement, lineText, m, query);
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ UTF-8
std::vector<Match> findAllInLine(const std::string& line, const Query& query) {
    std::vector<Match> out;
    const Query::Impl* q = query.impl();
    if (!q) return out;
    Line l;
    decode8(line.data(), line.size(), l);
    std::vector<CpMatch> cms;
    matchLineGuarded(*q, l.cps, cms, 0);
    toUnits(cms, l, 0, out);
    return out;
}

bool findInLine(const std::string& line, const Query& query, std::size_t* byteColumn,
                std::size_t* byteLength) {
    const Query::Impl* q = query.impl();
    if (!q) return false;
    Line l;
    decode8(line.data(), line.size(), l);
    std::vector<CpMatch> cms;
    matchLineGuarded(*q, l.cps, cms, 1);
    if (cms.empty()) return false;
    *byteColumn = l.offs[cms[0].start];
    *byteLength = l.offs[cms[0].start + cms[0].length] - *byteColumn;
    return true;
}

std::string replaceInText(const std::string& content, const Query& query,
                          const std::string& replacement, std::size_t* count) {
    std::size_t total = 0;
    std::string out;
    if (!query.usable()) {
        if (count) *count = 0;
        return content;
    }
    out.reserve(content.size());
    const std::size_t n = content.size();
    std::size_t pos = 0;
    while (true) {
        std::size_t next;
        const std::size_t end = lineEnd(content, pos, &next);
        const std::string line = content.substr(pos, end - pos);
        std::size_t at = 0;
        for (const Match& m : findAllInLine(line, query)) {
            out.append(line, at, m.start - at);
            out += expandT(replacement, line, m, query.options());
            at = m.start + m.length;
            ++total;
        }
        out.append(line, at, std::string::npos);
        out.append(content, end, next - end);   // the line break as it was
        if (next >= n) {
            if (end < n) {   // the empty line after a final line break
                pos = n;
                continue;
            }
            break;
        }
        pos = next;
    }
    if (count) *count = total;
    return out;
}

bool replaceOneInText(const std::string& content, const Query& query,
                      const std::string& replacement, int line, std::size_t byteColumn,
                      std::string* out) {
    if (!query.usable() || line < 1) return false;
    std::size_t pos = 0, next = 0, end = 0;
    for (int k = 1;; ++k) {
        end = lineEnd(content, pos, &next);
        if (k == line) break;
        if (next >= content.size()) {
            if (end < content.size() && k + 1 == line) {   // the empty last line
                pos = end = next = content.size();
                break;
            }
            return false;
        }
        pos = next;
    }
    const std::string text = content.substr(pos, end - pos);
    for (const Match& m : findAllInLine(text, query)) {
        if (m.start != byteColumn) continue;
        std::string r = content.substr(0, pos + m.start);
        r += expandT(replacement, text, m, query.options());
        r.append(content, pos + m.start + m.length, std::string::npos);
        *out = std::move(r);
        return true;
    }
    return false;
}

}  // namespace TextFind
