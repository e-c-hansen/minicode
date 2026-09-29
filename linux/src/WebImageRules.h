// WebImageRules.h — the plain C++ half of web pictures in the Markdown
// preview (WebImages.h is the libsoup half): which addresses are fetched,
// the limits, the pixel size a picture's header claims, and the
// bookkeeping of the in-memory cache and of recent failures. No GTK, so
// tests/run_tests.cpp covers all of it.
#pragma once

#include <cstddef>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>

namespace WebImageRules {

// One picture's download: past this it is cancelled, and a Content-Length
// that says more is not read at all.
constexpr std::size_t kMaxBytes = 20u * 1024 * 1024;
// Decoded pictures kept for the next render, all addresses together.
constexpr std::size_t kCacheBytes = 64u * 1024 * 1024;
// A fetch that makes no progress for this long fails (libsoup's I/O
// timeout, connecting included), and so does one that takes longer than
// the deadline in all, so a server trickling bytes cannot hold it open.
constexpr int kTimeoutSeconds = 15;
constexpr int kDeadlineSeconds = 60;
// A failed address is not asked for again for this long.
constexpr double kFailureSeconds = 60;
constexpr int kMaxRedirects = 5;
// The most pixels a picture may claim before it is decoded (a small file
// can claim a huge canvas). 64 megapixels is 256 MB decoded.
constexpr long long kMaxPixels = 64ll * 1024 * 1024;

// An https:// address with a host: the only kind the preview fetches. Plain
// http://, protocol-relative //host/x, data: and paths are not fetched. A
// redirect is followed only to an address that passes this too.
bool isFetchable(const std::string& url);

// The width and height a PNG, GIF, JPEG or WebP header claims, read from
// the first bytes without decoding anything. False for other formats (SVG,
// for one) and for a header too short or malformed to say.
bool headerSize(const unsigned char* data, std::size_t len, int* width, int* height);

// "GIF87a" or "GIF89a": decoded as an animation, the way a local .gif is.
bool isGif(const unsigned char* data, std::size_t len);

// A size small enough to decode and keep: both sides positive and at most
// kMaxPixels in all.
bool sizeAllowed(long long width, long long height);

// What a decoded picture costs the cache: its pixels at four bytes each,
// and for an animation a second frame (the one being composed) plus the
// compressed frames it keeps.
std::size_t decodedCost(int width, int height, bool animated, std::size_t rawBytes);

// A least-recently-used store with a limit in bytes: each value is put with
// its cost, and putting one drops the least recently used until the total
// fits again. A value costing more than the whole limit is not stored.
template <class V>
class ByteLru {
public:
    explicit ByteLru(std::size_t limit) : limit_(limit) {}

    // Stores `value` under `key` (replacing what was there). False, and
    // nothing stored, when `cost` alone is over the limit.
    bool put(const std::string& key, V value, std::size_t cost) {
        erase(key);
        if (cost > limit_) return false;
        order_.push_front(Entry{key, std::move(value), cost});
        index_[key] = order_.begin();
        bytes_ += cost;
        while (bytes_ > limit_ && order_.size() > 1) {
            index_.erase(order_.back().key);
            bytes_ -= order_.back().cost;
            order_.pop_back();
        }
        return true;
    }
    // The value under `key`, now the most recently used, or null.
    V* get(const std::string& key) {
        auto it = index_.find(key);
        if (it == index_.end()) return nullptr;
        order_.splice(order_.begin(), order_, it->second);
        return &order_.front().value;
    }
    void erase(const std::string& key) {
        auto it = index_.find(key);
        if (it == index_.end()) return;
        bytes_ -= it->second->cost;
        order_.erase(it->second);
        index_.erase(it);
    }
    std::size_t bytes() const { return bytes_; }
    std::size_t size() const { return order_.size(); }

private:
    struct Entry {
        std::string key;
        V value;
        std::size_t cost;
    };
    std::size_t limit_;
    std::size_t bytes_ = 0;
    std::list<Entry> order_;   // most recently used first
    std::unordered_map<std::string, typename std::list<Entry>::iterator> index_;
};

// Addresses that failed lately (a 404, a timeout, bytes that do not
// decode), each remembered for `seconds` from when it failed. Times are
// the caller's clock in seconds, so the tests can move it.
class FailureMemory {
public:
    explicit FailureMemory(double seconds) : seconds_(seconds) {}
    void remember(const std::string& key, double now);
    // Failed less than `seconds` before `now`; an older failure is forgotten.
    bool recent(const std::string& key, double now);
    std::size_t size() const { return until_.size(); }

private:
    double seconds_;
    std::unordered_map<std::string, double> until_;
};

}  // namespace WebImageRules
