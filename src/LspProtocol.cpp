// LspProtocol.cpp — see LspClient.h. The wire: Content-Length framing
// (Framer), the Reader that parses framed bodies into JSON off the main
// thread, and the text a $/progress report shows in the status line.
#include "LspClient.h"
#include "LspInternal.h"

namespace Lsp {

// ------------------------------------------------------------------ framing
std::string Framer::frame(const std::string& body) {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

void Framer::feed(const char* data, size_t n) {
    // Drop what has been consumed before growing, so a long session doesn't
    // keep every message it ever read.
    if (pos_ > 0 && pos_ >= buf_.size() / 2) {
        buf_.erase(0, pos_);
        pos_ = 0;
    }
    buf_.append(data, n);
}

namespace {
const long long kMaxMessage = 1LL << 30;
}

bool Framer::next(std::string& body) {
    while (true) {
        size_t headerEnd = buf_.find("\r\n\r\n", pos_);
        if (headerEnd == std::string::npos) return false;
        // Parse the header lines for Content-Length.
        long long length = -1;
        size_t line = pos_;
        while (line < headerEnd) {
            size_t eol = buf_.find("\r\n", line);
            if (eol == std::string::npos || eol > headerEnd) eol = headerEnd;
            std::string h = buf_.substr(line, eol - line);
            size_t colon = h.find(':');
            if (colon != std::string::npos &&
                lower(trim(h.substr(0, colon))) == "content-length") {
                // Digits only, and at most 1 GiB: a longer number (std::stoll
                // threw past 19 digits) is a broken header, skipped below.
                std::string v = trim(h.substr(colon + 1));
                long long n = v.empty() ? -1 : 0;
                for (char c : v) {
                    if (c < '0' || c > '9' || n > kMaxMessage) { n = -1; break; }
                    n = n * 10 + (c - '0');
                }
                length = n > kMaxMessage ? -1 : n;
            }
            line = eol + 2;
        }
        size_t bodyStart = headerEnd + 4;
        if (length < 0) {
            // A header block with no usable length: skip it rather than stall.
            pos_ = bodyStart;
            continue;
        }
        if (buf_.size() - bodyStart < (size_t)length) return false;
        body = buf_.substr(bodyStart, (size_t)length);
        pos_ = bodyStart + (size_t)length;
        return true;
    }
}

bool Reader::next(Incoming& out) {
    std::string body;
    if (!framer_.next(body)) return false;
    out.message = Json();
    out.error.clear();
    if (!Json::parse(body, out.message, &out.error)) {
        out.message = Json();
        if (out.error.empty()) out.error = "not JSON";
    }
    return true;
}

// ------------------------------------------------------------- progress
std::string progressText(const Progress& p) {
    std::string s = p.title;
    if (!p.message.empty()) s += (s.empty() ? "" : ": ") + p.message;
    if (p.percentage >= 0) s += (s.empty() ? "" : " ") + std::to_string(p.percentage) + "%";
    if (s.empty()) return "working\xE2\x80\xA6";
    // Line breaks and tabs would break the status bar's one line.
    for (char& c : s)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    const size_t kMax = 60;
    if (s.size() > kMax) {
        size_t cut = kMax;
        while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
        s = s.substr(0, cut) + "\xE2\x80\xA6";
    }
    return s;
}

}  // namespace Lsp
