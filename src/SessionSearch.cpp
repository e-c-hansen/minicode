// SessionSearch.cpp — see SessionSearch.h.
#include "SessionSearch.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>

#include "CommandPalette.h"
#include "Text.h"

namespace Shared {

namespace {

using Clock = std::chrono::steady_clock;

Json answerFor(const std::string& op) {
    Json a = Json::object();
    a.set("op", Json(op));
    return a;
}

Json strings(const std::vector<std::string>& v) {
    Json a = Json::array();
    for (const std::string& s : v) a.push(Json(s));
    return a;
}

// UTF-16 units in the UTF-8 bytes [0, n) of `s`.
std::size_t units16(const std::string& s, std::size_t n) {
    std::size_t u = 0;
    for (std::size_t i = 0; i < n && i < s.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) == 0x80) continue;   // a continuation byte
        u += c >= 0xF0 ? 2 : 1;             // four bytes: a surrogate pair
    }
    return u;
}

// Raises `stop` when `cancel` goes up or `seconds` pass (0: never), until
// told the read ended; and, every few milliseconds meanwhile, runs `tick`
// (a batch that waited long enough goes out).
class Guard {
public:
    Guard(std::atomic<bool>& cancel, double seconds, std::function<void()> tick)
        : cancel_(cancel), tick_(std::move(tick)) {
        const auto start = Clock::now();
        thread_ = std::thread([this, start, seconds] {
            std::unique_lock<std::mutex> lk(mu_);
            while (!done_) {
                cv_.wait_for(lk, std::chrono::milliseconds(20));
                if (done_) break;
                if (cancel_.load()) stop.store(true);
                if (seconds > 0 && std::chrono::duration<double>(Clock::now() - start).count() >= seconds) {
                    timedOut.store(true);
                    stop.store(true);
                }
                if (tick_) {
                    lk.unlock();
                    tick_();
                    lk.lock();
                }
            }
        });
    }
    ~Guard() { end(); }
    void end() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            done_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    std::atomic<bool> stop{false};
    std::atomic<bool> timedOut{false};

private:
    std::atomic<bool>& cancel_;
    std::function<void()> tick_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread thread_;
};

void runFiles(const std::string& root, const Json& request, std::atomic<bool>& cancel,
              const std::function<void(Json&&)>& emit, double seconds) {
    Guard guard(cancel, seconds, nullptr);
    bool truncated = false;
    const std::vector<std::string> files =
        CommandPalette::listFiles(root, kMaxListedFiles, &guard.stop, &truncated, true,
                                  request["hidden"].asBool(), true);
    guard.end();
    Json a = answerFor("files");
    a.set("files", strings(files));
    a.set("truncated", Json(truncated || guard.timedOut.load()));
    if (guard.timedOut.load()) a.set("timedOut", Json(true));
    else if (cancel.load()) a.set("cancelled", Json(true));
    a.set("done", Json(true));
    emit(std::move(a));
}

void runSearch(const std::string& root, const Json& request, std::atomic<bool>& cancel,
               const std::function<void(Json&&)>& emit, double seconds, const std::function<void()>& eachFile) {
    const std::string query = request["query"].asString();
    FolderSearchOptions options = searchOptions(request);
    if (eachFile) options.onFileRead = eachFile;
    const TextFind::Query marker = FolderSearch::makeQuery(query, options);
    // The rows not sent yet, and when a batch last went: the walk's thread
    // adds to them, the guard's sends a batch that waited kBatchMs.
    std::mutex mu;
    Json batch = Json::array();
    auto last = Clock::now();
    auto flush = [&](bool always) {
        std::lock_guard<std::mutex> lk(mu);
        if (!batch.size()) return;
        if (!always && batch.size() < kBatchRows &&
            Clock::now() - last < std::chrono::milliseconds(kBatchMs))
            return;
        Json a = answerFor("search");
        a.set("rows", std::move(batch));
        a.set("done", Json(false));
        batch = Json::array();
        last = Clock::now();
        emit(std::move(a));
    };
    options.onRows = [&](const std::vector<FolderSearchMatch>& rows) {
        {
            std::lock_guard<std::mutex> lk(mu);
            for (const FolderSearchMatch& m : rows) batch.push(searchRowJson(m, marker));
        }
        flush(false);
    };
    Guard guard(cancel, seconds, [&] { flush(false); });
    const FolderSearchResult r = FolderSearch::search(root, query, &guard.stop, options);
    guard.end();
    flush(true);
    Json a = answerFor("search");
    a.set("rows", Json::array());
    a.set("done", Json(true));
    a.set("filesSearched", Json((int64_t)r.filesSearched));
    a.set("filesMatched", Json((int64_t)r.filesMatched));
    a.set("totalMatches", Json((int64_t)r.totalMatches));
    a.set("rowCount", Json((int64_t)r.matches.size()));
    a.set("truncated", Json(r.truncated));
    a.set("tooSlow", Json(r.tooSlow));
    const bool timedOut = guard.timedOut.load();
    a.set("timedOut", Json(timedOut));
    a.set("cancelled", Json(r.cancelled && !timedOut));
    if (!r.error.empty()) a.set("error", Json(r.error));
    Json skipped = Json::array();
    for (std::size_t i = 0; i < r.skipped.size() && i < 20; i++) skipped.push(Json(r.skipped[i].relativePath));
    a.set("skipped", std::move(skipped));
    a.set("skippedCount", Json((int64_t)(r.skippedTooLarge + r.skippedNotUtf8)));
    emit(std::move(a));
}

}  // namespace

bool isWorkerRead(const std::string& op) { return op == "files" || op == "search" || op == "cancel"; }

FolderSearchOptions searchOptions(const Json& request) {
    FolderSearchOptions o;
    o.maxMatches = kMaxSearchRows;
    o.contextLines = 1;
    o.caseSensitive = request["caseSensitive"].asBool();
    o.wholeWord = request["wholeWord"].asBool();
    o.regex = request["regex"].asBool();
    o.include = request["include"].asString();
    o.exclude = request["exclude"].asString();
    o.skipDotfiles = !request["hidden"].asBool();
    o.confine = true;
    return o;
}

Json searchRowJson(const FolderSearchMatch& m, const TextFind::Query& query) {
    Json row = Json::object();
    row.set("path", Json(m.relativePath));
    row.set("line", Json((int64_t)m.line));
    row.set("column", Json((int64_t)m.column));
    row.set("text", Json(m.text));
    Json hl = Json::array();
    if (query.usable())
        for (const TextFind::Match& x : TextFind::findAllInLine(m.text, query)) {
            Json r = Json::array();
            const std::size_t start = units16(m.text, x.start);
            r.push(Json((int64_t)start));
            r.push(Json((int64_t)(units16(m.text, x.start + x.length) - start)));
            hl.push(std::move(r));
        }
    row.set("hl", std::move(hl));
    row.set("before", strings(m.before));
    row.set("after", strings(m.after));
    row.set("count", Json((int64_t)m.count));
    return row;
}

void runWorkerRead(const std::string& root, const Json& request, std::atomic<bool>& cancel,
                   const std::function<void(Json&&)>& emit, double seconds,
                   const std::function<void()>& eachFile) {
    const std::string op = request["op"].asString();
    if (op == "files") return runFiles(root, request, cancel, emit, seconds);
    if (op == "search") return runSearch(root, request, cancel, emit, seconds, eachFile);
    Json a = answerFor(op);
    a.set("error", Json("no read named " + op));
    a.set("done", Json(true));
    emit(std::move(a));
}

// ---------------------------------------------------------------- workers

ReadWorkers::ReadWorkers() {
    if (pipe(wake_) == 0)
        for (int fd : wake_) {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
}

ReadWorkers::~ReadWorkers() {
    for (auto& j : jobs_) j->cancel.store(true);
    for (auto& j : jobs_)
        if (j->thread.joinable()) j->thread.join();
    for (int fd : wake_)
        if (fd >= 0) close(fd);
}

void ReadWorkers::push(Answer&& a) {
    std::lock_guard<std::mutex> lk(mu_);
    queue_.push_back(std::move(a));
    if (!wakePending_ && wake_[1] >= 0) {
        const char b = 1;
        if (write(wake_[1], &b, 1) == 1) wakePending_ = true;
    }
    cv_.notify_all();
}

void ReadWorkers::reap() {
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (!(*it)->finished.load()) {
            ++it;
            continue;
        }
        if ((*it)->thread.joinable()) (*it)->thread.join();
        it = jobs_.erase(it);
    }
}

bool ReadWorkers::start(ClientId c, std::uint64_t req, std::uint64_t window, const std::string& root,
                        const Json& request, std::string* why, double seconds) {
    reap();
    if (jobs_.size() >= kMaxReadJobs) {
        if (why) *why = "too many searches at once on this host; try again in a moment";
        return false;
    }
    const std::string op = request["op"].asString();
    if (seconds < 0) seconds = op == "files" ? kListSeconds : kSearchSeconds;
    auto job = std::make_unique<Job>();
    job->to = c;
    job->req = req;
    job->window = window;
    Job* j = job.get();
    try {
        j->thread = std::thread([this, j, root, request, seconds, pace = eachFile] {
            auto emit = [this, j](Json&& json) { push(Answer{j->to, j->req, j->window, std::move(json)}); };
            try {
                runWorkerRead(root, request, j->cancel, emit, seconds, pace);
            } catch (...) {
                Json a = Json::object();
                a.set("op", request["op"]);
                a.set("error", Json("the read failed"));
                a.set("done", Json(true));
                emit(std::move(a));
            }
            j->finished.store(true);
            std::lock_guard<std::mutex> lk(mu_);
            cv_.notify_all();
        });
    } catch (...) {
        if (why) *why = "no thread to read with";
        return false;
    }
    jobs_.push_back(std::move(job));
    return true;
}

void ReadWorkers::cancel(ClientId c, std::uint64_t req) {
    for (auto& j : jobs_)
        if (j->to == c && j->req == req) j->cancel.store(true);
}

void ReadWorkers::cancelAll(ClientId c) {
    for (auto& j : jobs_)
        if (j->to == c) j->cancel.store(true);
}

std::vector<ReadWorkers::Answer> ReadWorkers::take() {
    std::vector<Answer> out;
    {
        std::lock_guard<std::mutex> lk(mu_);
        char buf[64];
        while (wake_[0] >= 0 && read(wake_[0], buf, sizeof buf) > 0) {}
        wakePending_ = false;
        out.swap(queue_);
    }
    reap();
    return out;
}

std::size_t ReadWorkers::running() const {
    std::size_t n = 0;
    for (const auto& j : jobs_) n += j->finished.load() ? 0 : 1;
    return n;
}

bool ReadWorkers::wait(int timeoutMs) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_for(lk, std::chrono::milliseconds(std::max(0, timeoutMs)), [this] {
        if (!queue_.empty()) return true;
        for (const auto& j : jobs_)
            if (!j->finished.load()) return false;
        return true;
    });
    return !queue_.empty();
}

}  // namespace Shared
