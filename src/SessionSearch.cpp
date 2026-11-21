// SessionSearch.cpp — see SessionSearch.h.
#include "SessionSearch.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <set>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>

#include "CommandPalette.h"
#include "SessionFiles.h"
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

std::string nameOf(const std::string& rel) {
    const std::size_t slash = rel.rfind('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

Json skippedJson(const std::string& path, const char* reason, const std::string& why) {
    Json j = Json::object();
    j.set("path", Json(path));
    j.set("reason", Json(reason));
    j.set("why", Json(why));
    return j;
}

// The files no document holds, rewritten one by one with the save rules,
// after what the host did to the documents it holds ("edited").
void runReplace(const std::string& root, const Json& request, std::atomic<bool>& cancel,
                const std::function<void(Json&&)>& emit) {
    const std::string query = request["query"].asString();
    const std::string replacement = request["replacement"].asString();
    const bool hidden = request["hidden"].asBool();
    const FolderSearchOptions options = searchOptions(request);
    const Json& edited = request["edited"];
    Json skipped = Json::array();
    for (const Json& s : edited["skipped"].elements()) skipped.push(s);
    std::int64_t replaced = edited["editedMatches"].asInt(), files = 0;
    bool cancelled = false;
    for (const Json& f : request["files"].elements()) {
        if (cancel.load()) {
            cancelled = true;
            break;
        }
        const std::string rel = f["path"].asString();
        const std::string name = nameOf(rel);
        std::string abs, why;
        if (!hidden && replaceHidesPath(rel)) {
            skipped.push(skippedJson(rel, "dotfile", name + " is hidden on this page: it was left alone."));
            continue;
        }
        if (!confinedPath(root, rel, &abs, &why)) {
            skipped.push(skippedJson(rel, "outside", name + " is not one of the session's files: it was left alone."));
            continue;
        }
        std::vector<ReplaceRow> rows;
        if (!replaceRowsFrom(f["rows"], kMaxReplaceRows, &rows)) continue;
        std::string bytes;
        FolderSearch::FileStamp stamp;
        const FolderSearch::ReadStatus r = FolderSearch::readFileStable(abs, kMaxReplaceBytes, &bytes, &stamp);
        if (r != FolderSearch::ReadStatus::Ok) {
            const char* reason = r == FolderSearch::ReadStatus::Missing      ? "gone"
                                 : r == FolderSearch::ReadStatus::NotRegular ? "notfile"
                                 : r == FolderSearch::ReadStatus::TooLarge   ? "large"
                                                                             : "unreadable";
            const std::string says = r == FolderSearch::ReadStatus::Missing      ? " was deleted since the search"
                                     : r == FolderSearch::ReadStatus::NotRegular ? " is not a regular file"
                                     : r == FolderSearch::ReadStatus::TooLarge   ? " is too large to replace in here"
                                                                                 : " could not be read";
            skipped.push(skippedJson(rel, reason, name + says + ": it was left alone."));
            continue;
        }
        FolderSearch::TextEncoding enc = FolderSearch::TextEncoding::Utf8;
        bool bom = false;
        if (!FolderSearch::decodeFileText(&bytes, &enc, &bom)) {
            skipped.push(skippedJson(rel, "changed", name + " no longer reads as text: it was left alone."));
            continue;
        }
        std::string out;
        std::size_t n = 0;
        const ReplaceCheck c = replaceRows(bytes, rows, query, replacement, options, &out, &n);
        if (c == ReplaceCheck::Nothing) continue;
        if (c == ReplaceCheck::Changed) {
            skipped.push(skippedJson(rel, "changed", name + " changed since the search: it was left alone."));
            continue;
        }
        std::string unheld;
        if (!FolderSearch::canEncode(out, enc, &unheld)) {
            skipped.push(skippedJson(rel, "encoding",
                                     name + " is " + FolderSearch::encodingLabel(enc, bom) + ", which cannot store “" +
                                         unheld + "”: it was left alone."));
            continue;
        }
        // Written over while it was read: that change wins.
        if (FolderSearch::stampOf(abs) != stamp) {
            skipped.push(skippedJson(rel, "changed", name + " changed on disk while it was read: it was left alone."));
            continue;
        }
        if (!FolderSearch::writeFileAtomically(abs, FolderSearch::encodeFileText(out, enc, bom), &why)) {
            skipped.push(skippedJson(rel, "write", name + " could not be written: " + why + "."));
            continue;
        }
        replaced += (std::int64_t)n;
        files++;
    }
    Json a = answerFor("replace");
    a.set("done", Json(true));
    a.set("replaced", Json(replaced));
    a.set("files", Json(files));
    a.set("edited", Json(edited["edited"].asInt()));
    a.set("editedMatches", Json(edited["editedMatches"].asInt()));
    a.set("skipped", std::move(skipped));
    if (cancelled) a.set("cancelled", Json(true));
    emit(std::move(a));
}

}  // namespace

bool isWorkerRead(const std::string& op) {
    return op == "files" || op == "search" || op == "cancel" || op == "replace";
}

bool replaceHidesPath(const std::string& rel) {
    std::size_t at = 0;
    for (;;) {
        if (at < rel.size() && rel[at] == '.') return true;
        const std::size_t slash = rel.find('/', at);
        if (slash == std::string::npos) return false;
        at = slash + 1;
    }
}

ReplaceCheck replaceRows(const std::string& content, const std::vector<ReplaceRow>& rows, const std::string& query,
                         const std::string& replacement, const FolderSearchOptions& options, std::string* out,
                         std::size_t* replaced) {
    if (replaced) *replaced = 0;
    std::size_t counted = 0;
    std::set<int> skip;
    bool any = false;
    for (const ReplaceRow& r : rows) {
        std::string now;
        if (!FolderSearch::lineAt(content, r.line, &now) ||
            FolderSearch::displayText(now, options.maxLineChars) != r.text)
            return ReplaceCheck::Changed;
        counted += r.count;
        if (r.on) any = true;
        else skip.insert(r.line);
    }
    // As many matches as the rows counted: one added or gone on a line no
    // row shows is a change too.
    std::size_t all = 0;
    std::string every = FolderSearch::replaceInText(content, query, replacement, options, &all);
    if (all != counted) return ReplaceCheck::Changed;
    if (!any) return ReplaceCheck::Nothing;
    std::size_t n = all;
    if (skip.empty()) *out = std::move(every);
    else *out = FolderSearch::replaceInText(content, query, replacement, options, &n, &skip);
    if (replaced) *replaced = n;
    return n ? ReplaceCheck::Replaced : ReplaceCheck::Nothing;
}

bool replaceRowsFrom(const Json& rows, std::size_t most, std::vector<ReplaceRow>* out) {
    out->clear();
    if (!rows.isArray()) return false;
    for (const Json& r : rows.elements()) {
        if (out->size() >= most) break;
        if (!r.isObject() || !r["line"].isNumber() || !r["text"].isString() || r["line"].asInt() < 1) return false;
        ReplaceRow row;
        row.line = (int)std::min<std::int64_t>(r["line"].asInt(), INT32_MAX);
        row.text = r["text"].asString();
        row.count = (std::size_t)std::max<std::int64_t>(1, r["count"].asInt(1));
        row.on = !r.has("on") || r["on"].asBool();
        out->push_back(std::move(row));
    }
    return true;
}

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
    if (op == "replace") return runReplace(root, request, cancel, emit);
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
