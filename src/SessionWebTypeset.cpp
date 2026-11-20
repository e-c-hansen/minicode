// SessionWebTypeset.cpp — the LaTeX preview's typeset in the session's web
// page (R3's third slice, SessionWeb.h): which files are LaTeX documents,
// the address their PDF is served at, the app's typesets kept by the
// document's real path, what the page is told of one, and its pages drawn
// as pictures (PageRenders) when the app gives a renderer. Nothing here
// runs tectonic: the app's preview publishes what it typeset.
#include "SessionWeb.h"

#include "SqliteRead.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <iterator>

namespace Shared {
namespace Web {

namespace {

std::string lowerExtension(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string e = path.substr(dot + 1);
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

std::string realOf(const std::string& path) {
    char buf[PATH_MAX];
    if (!path.empty() && ::realpath(path.c_str(), buf)) return buf;
    return "";
}

// `path` under `dir` (both spelled the same way): the rest, or false.
bool under(const std::string& dir, const std::string& path, std::string* rest) {
    if (dir.empty()) return false;
    const std::string d = dir.back() == '/' ? dir : dir + "/";
    if (path.compare(0, d.size(), d) != 0) return false;
    *rest = path.substr(d.size());
    return true;
}

}  // namespace

bool typesetSource(const std::string& path) {
    const std::string e = lowerExtension(path);
    return e == "tex" || e == "ltx" || e == "latex";
}

std::string typesetTarget(const std::string& base, const std::string& rel) {
    // fileTarget's encoding, under typeset/ instead of file/.
    const std::string file = fileTarget(base, rel);
    return base + "typeset/" + file.substr(base.size() + 5);
}

std::string typesetPageTarget(const std::string& base, const std::string& rel) {
    const std::string file = fileTarget(base, rel);
    return base + "typeset-page/" + file.substr(base.size() + 5);
}

namespace {

// Digits alone, at most 15 of them: false for anything else.
bool digits(const std::map<std::string, std::string>& query, const char* name, std::uint64_t* n) {
    auto it = query.find(name);
    if (it == query.end() || it->second.empty() || it->second.size() > 15) return false;
    std::uint64_t v = 0;
    for (char c : it->second) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (std::uint64_t)(c - '0');
    }
    *n = v;
    return true;
}

}  // namespace

bool typesetPageAsk(const std::map<std::string, std::string>& query, PageAsk* ask) {
    std::uint64_t v = 0, p = 0, w = 0;
    if (!digits(query, "v", &v) || !digits(query, "p", &p) || !digits(query, "w", &w)) return false;
    if (p < 1 || p > (std::uint64_t)INT_MAX) return false;
    ask->version = v;
    ask->page = (int)p;
    ask->width = (int)std::min<std::uint64_t>(std::max<std::uint64_t>(w, kMinPageWidth), kMaxPageWidth);
    return true;
}

int typesetPageWidth(const PageSize& size, int width) {
    int w = std::min(std::max(width, kMinPageWidth), kMaxPageWidth);
    if (size.width > 0 && size.height > 0) {
        // w * (w * h / width) pixels at most kMaxPagePixels.
        const double most = std::floor(std::sqrt(kMaxPagePixels * size.width / size.height));
        if (most < w) w = (int)std::max<double>(most, kMinPageWidth);
    }
    return w;
}

bool relativeInFolder(const std::string& folder, const std::string& path, std::string* rel) {
    if (path.empty() || path[0] != '/') {
        *rel = path;
        return true;
    }
    if (under(folder, path, rel)) return true;
    const std::string realFolder = realOf(folder), realPath = realOf(path);
    return !realFolder.empty() && !realPath.empty() && under(realFolder, realPath, rel);
}

std::string Typesets::key(const std::string& path) {
    const std::string r = realOf(path);
    return r.empty() ? path : r;
}

Typesets::Kept& Typesets::at(const std::string& key) {
    auto it = kept_.find(key);
    if (it != kept_.end()) return it->second;
    if (kept_.size() >= kMaxEntries) {
        auto oldest = kept_.begin();
        for (auto o = kept_.begin(); o != kept_.end(); ++o)
            if (o->second.changed < oldest->second.changed) oldest = o;
        kept_.erase(oldest);
    }
    return kept_[key];
}

bool Typesets::working(const std::string& path) {
    const std::string k = key(path);
    std::lock_guard<std::mutex> lock(mutex_);
    Kept& e = at(k);
    if (e.entry.state == State::Working) return false;
    e.entry.state = State::Working;
    e.entry.version++;
    e.changed = ++changes_;
    return true;
}

bool Typesets::ready(const std::string& path, std::shared_ptr<const std::string> pdf,
                     std::shared_ptr<const std::vector<PageSize>> pages) {
    if (!pdf || pdf->size() > kMaxPdfBytes)
        return failed(path, pdf ? "The typeset PDF is over " + std::to_string(kMaxPdfBytes >> 20) +
                                      " MB, too large to show on this page."
                                : "The typeset made no PDF.");
    const std::string k = key(path);
    std::lock_guard<std::mutex> lock(mutex_);
    Kept& e = at(k);
    const bool same = e.entry.pdf && *e.entry.pdf == *pdf;
    if (same && e.entry.state == State::Ready) return false;
    if (!same) {
        e.entry.pdf = std::move(pdf);
        e.entry.pages = std::move(pages);
        e.entry.pdfVersion = ++pdfVersions_;
    }
    e.entry.state = State::Ready;
    e.entry.error.clear();
    e.entry.version++;
    e.changed = ++changes_;
    return true;
}

bool Typesets::failed(const std::string& path, const std::string& error) {
    std::string text = SqliteRead::validUtf8(SqliteRead::cutUtf8(error, kMaxErrorBytes));
    if (text.empty()) text = "The document did not typeset.";
    const std::string k = key(path);
    std::lock_guard<std::mutex> lock(mutex_);
    Kept& e = at(k);
    if (e.entry.state == State::Failed && e.entry.error == text) return false;
    e.entry.state = State::Failed;
    e.entry.error = std::move(text);
    e.entry.version++;
    e.changed = ++changes_;
    return true;
}

Typesets::Entry Typesets::get(const std::string& path) const {
    const std::string k = key(path);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = kept_.find(k);
    return it == kept_.end() ? Entry() : it->second.entry;
}

std::uint64_t Typesets::changes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return changes_;
}

std::size_t Typesets::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kept_.size();
}

Json typesetJson(const std::string& asked, const std::string& rel, const Typesets::Entry& e,
                 const std::string& base, bool canTypeset) {
    Json j = Json::object();
    j.set("t", Json("latex"));
    j.set("path", Json(asked));
    j.set("rel", Json(rel));
    j.set("version", Json((int64_t)e.version));
    const char* state = "none";
    switch (e.state) {
    case Typesets::State::Ready: state = "ready"; break;
    case Typesets::State::Working: state = "working"; break;
    case Typesets::State::Failed: state = "failed"; break;
    case Typesets::State::None: state = canTypeset ? "working" : "none"; break;
    }
    j.set("state", Json(state));
    if (e.pdf && !rel.empty())
        j.set("url", Json(typesetTarget(base, rel) + "?v=" + std::to_string(e.pdfVersion)));
    if (e.pdf && e.pages && !e.pages->empty() && !rel.empty()) {
        // Sizes to a hundredth of a point: the page lays its pictures out
        // by them before any is drawn.
        auto hundredths = [](double x) { return Json(std::round(x * 100) / 100); };
        Json pages = Json::array();
        for (const PageSize& p : *e.pages) {
            Json size = Json::array();
            size.push(hundredths(p.width));
            size.push(hundredths(p.height));
            pages.push(std::move(size));
        }
        j.set("pages", std::move(pages));
        j.set("pageUrl", Json(typesetPageTarget(base, rel) + "?v=" + std::to_string(e.pdfVersion)));
    }
    if (e.state == Typesets::State::Failed) {
        j.set("text", Json(e.error));
    } else if (e.state == Typesets::State::None && !canTypeset) {
        j.set("text", Json("This host does not typeset LaTeX. The mutible window's own page "
                           "(View > Show Session in Browser…) shows the pages its LaTeX preview "
                           "typesets; Show Source shows the text."));
    }
    return j;
}

// ---- pages drawn as pictures

bool PageRenders::Key::operator<(const Key& o) const {
    if (doc != o.doc) return doc < o.doc;
    if (version != o.version) return version < o.version;
    if (page != o.page) return page < o.page;
    return width < o.width;
}

PageRenders::PageRenders(Draw draw, std::function<void()> wake, std::size_t maxBytes)
    : draw_(std::move(draw)), wake_(std::move(wake)), maxBytes_(maxBytes) {}

PageRenders::~PageRenders() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (std::thread& t : threads_) t.join();
}

PageRenders::Got PageRenders::get(const Key& key, const std::shared_ptr<const std::string>& pdf,
                                  std::shared_ptr<const std::string>* png) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t newest = newest_[key.doc];
    if (key.version > newest) {
        newest = newest_[key.doc] = key.version;
        trim();   // the document's older pictures go
    }
    auto it = items_.find(key);
    if (it != items_.end()) {
        it->second.used = ++uses_;
        switch (it->second.state) {
        case Item::Done: *png = it->second.png; return Got::Ready;
        case Item::Failed: return Got::Failed;
        default: return Got::Pending;
        }
    }
    if (!pdf || key.version < newest) return Got::Failed;
    if (queue_.size() >= kMaxWaiting) return Got::Busy;
    Item& item = items_[key];
    item.pdf = pdf;
    item.used = ++uses_;
    queue_.push_back(key);
    // A thread more while fewer than kThreads are running and work waits.
    if ((int)threads_.size() < kThreads && (std::size_t)drawing_ + queue_.size() > threads_.size())
        threads_.emplace_back([this] { work(); });
    cv_.notify_one();
    return Got::Pending;
}

void PageRenders::work() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (stop_) return;
        const Key key = queue_.front();
        queue_.pop_front();
        auto it = items_.find(key);
        if (it == items_.end() || it->second.state != Item::Waiting) continue;
        it->second.state = Item::Drawing;
        const std::shared_ptr<const std::string> pdf = std::move(it->second.pdf);
        drawing_++;
        mostAtOnce_ = std::max(mostAtOnce_, drawing_);
        lock.unlock();
        std::string png;
        const bool ok = pdf && draw_(*pdf, key.page, key.width, &png) && !png.empty();
        lock.lock();
        drawing_--;
        drawn_++;
        it = items_.find(key);   // trim() may have dropped it meanwhile
        if (it != items_.end()) {
            if (ok) {
                it->second.state = Item::Done;
                it->second.png = std::make_shared<const std::string>(std::move(png));
                bytes_ += it->second.png->size();
            } else {
                it->second.state = Item::Failed;
            }
            trim();
        }
        if (stop_) return;
        lock.unlock();
        if (wake_) wake_();
        lock.lock();
    }
}

void PageRenders::trim() {
    // Only each document's newest version's pictures (one being drawn is
    // let finish, then dropped here).
    for (auto it = items_.begin(); it != items_.end();) {
        auto n = newest_.find(it->first.doc);
        if (n != newest_.end() && it->first.version < n->second && it->second.state != Item::Drawing) {
            if (it->second.png) bytes_ -= it->second.png->size();
            it = items_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto q = queue_.begin(); q != queue_.end();)
        q = items_.count(*q) ? q + 1 : queue_.erase(q);
    // Then within the cap, the picture used longest ago first.
    while (bytes_ > maxBytes_) {
        auto oldest = items_.end();
        for (auto it = items_.begin(); it != items_.end(); ++it)
            if (it->second.png && (oldest == items_.end() || it->second.used < oldest->second.used)) oldest = it;
        if (oldest == items_.end()) break;
        bytes_ -= oldest->second.png->size();
        items_.erase(oldest);
    }
    // The documents no picture is kept for are forgotten past a few.
    if (newest_.size() > 64)
        for (auto n = newest_.begin(); n != newest_.end() && newest_.size() > 64;) {
            bool any = false;
            for (const auto& i : items_)
                if (i.first.doc == n->first) any = true;
            n = any ? std::next(n) : newest_.erase(n);
        }
}

std::size_t PageRenders::bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
}

std::size_t PageRenders::kept() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t n = 0;
    for (const auto& i : items_)
        if (i.second.state == Item::Done || i.second.state == Item::Failed) n++;
    return n;
}

std::uint64_t PageRenders::drawn() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return drawn_;
}

int PageRenders::mostAtOnce() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mostAtOnce_;
}

}  // namespace Web
}  // namespace Shared
