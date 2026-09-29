// WebImageRules.cpp — see WebImageRules.h.
#include "WebImageRules.h"

#include <cctype>
#include <cstring>
#include <iterator>

namespace WebImageRules {

bool isFetchable(const std::string& url) {
    static const char kScheme[] = "https://";
    const std::size_t n = sizeof kScheme - 1;
    if (url.size() <= n) return false;
    for (std::size_t i = 0; i < n; i++)
        if (std::tolower((unsigned char)url[i]) != kScheme[i]) return false;
    // A host must follow: "https:///x" and "https://?x" name none.
    const char c = url[n];
    if (c == '/' || c == '?' || c == '#') return false;
    // No spaces or control characters anywhere; Markdown would have ended
    // the address at a space, so one here is not a real address.
    for (unsigned char ch : url)
        if (ch <= 0x20 || ch == 0x7F) return false;
    return true;
}

namespace {
unsigned be16(const unsigned char* p) { return (unsigned)p[0] << 8 | p[1]; }
unsigned le16(const unsigned char* p) { return (unsigned)p[1] << 8 | p[0]; }
unsigned long be32(const unsigned char* p) {
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3];
}
unsigned long le24(const unsigned char* p) {
    return (unsigned long)p[2] << 16 | (unsigned long)p[1] << 8 | p[0];
}

bool jpegSize(const unsigned char* d, std::size_t len, int* w, int* h) {
    std::size_t i = 2;   // past FF D8
    while (i + 1 < len) {
        if (d[i] != 0xFF) return false;
        while (i < len && d[i] == 0xFF) i++;   // fill bytes
        if (i >= len) return false;
        const unsigned char m = d[i++];
        // Markers that stand alone, with no length after them.
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) continue;
        if (m == 0xD9 || m == 0xDA) return false;   // end of image, or scan data
        if (i + 2 > len) return false;
        const unsigned seg = be16(d + i);
        if (seg < 2) return false;
        // Start of frame, any kind but DHT (C4), JPG (C8) and DAC (CC).
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (i + 7 > len) return false;
            *h = (int)be16(d + i + 3);
            *w = (int)be16(d + i + 5);
            return true;
        }
        i += seg;
    }
    return false;
}
}  // namespace

bool isGif(const unsigned char* d, std::size_t len) {
    return len >= 6 && (std::memcmp(d, "GIF87a", 6) == 0 || std::memcmp(d, "GIF89a", 6) == 0);
}

bool headerSize(const unsigned char* d, std::size_t len, int* width, int* height) {
    int w = -1, h = -1;
    static const unsigned char kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (len >= 24 && std::memcmp(d, kPng, 8) == 0 && std::memcmp(d + 12, "IHDR", 4) == 0) {
        const unsigned long pw = be32(d + 16), ph = be32(d + 20);
        if (pw > 0x7FFFFFFF || ph > 0x7FFFFFFF) return false;
        w = (int)pw;
        h = (int)ph;
    } else if (len >= 10 && isGif(d, len)) {
        w = (int)le16(d + 6);
        h = (int)le16(d + 8);
    } else if (len >= 4 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) {
        if (!jpegSize(d, len, &w, &h)) return false;
    } else if (len >= 30 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0) {
        const unsigned char* c = d + 12;
        if (std::memcmp(c, "VP8X", 4) == 0) {
            w = (int)le24(d + 24) + 1;
            h = (int)le24(d + 27) + 1;
        } else if (std::memcmp(c, "VP8L", 4) == 0) {
            if (d[20] != 0x2F) return false;   // the lossless signature byte
            const unsigned long bits = (unsigned long)d[21] | (unsigned long)d[22] << 8 |
                                       (unsigned long)d[23] << 16 | (unsigned long)d[24] << 24;
            w = (int)(bits & 0x3FFF) + 1;
            h = (int)((bits >> 14) & 0x3FFF) + 1;
        } else if (std::memcmp(c, "VP8 ", 4) == 0) {
            if (d[23] != 0x9D || d[24] != 0x01 || d[25] != 0x2A) return false;
            w = (int)(le16(d + 26) & 0x3FFF);
            h = (int)(le16(d + 28) & 0x3FFF);
        } else {
            return false;
        }
    } else {
        return false;
    }
    *width = w;
    *height = h;
    return true;
}

bool sizeAllowed(long long width, long long height) {
    return width > 0 && height > 0 && width <= kMaxPixels && height <= kMaxPixels &&
           width * height <= kMaxPixels;
}

std::size_t decodedCost(int width, int height, bool animated, std::size_t rawBytes) {
    const std::size_t frame = (std::size_t)(width > 0 ? width : 0) *
                              (std::size_t)(height > 0 ? height : 0) * 4;
    return animated ? 2 * frame + rawBytes : frame;
}

void FailureMemory::remember(const std::string& key, double now) {
    // Kept small: forget what has expired once there are many.
    if (until_.size() >= 512)
        for (auto it = until_.begin(); it != until_.end();)
            it = it->second <= now ? until_.erase(it) : std::next(it);
    until_[key] = now + seconds_;
}

bool FailureMemory::recent(const std::string& key, double now) {
    auto it = until_.find(key);
    if (it == until_.end()) return false;
    if (it->second > now) return true;
    until_.erase(it);
    return false;
}

}  // namespace WebImageRules
