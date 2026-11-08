// LatexScratch.cpp — see LatexScratch.h.
//
// The record is a few lines of text: a header naming the format, then pid,
// start time, size and hash, one to a line, and last the sibling's path,
// which runs to the end of the file (less its final newline), so a path
// holding a newline still reads back whole.
#include "LatexScratch.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace LatexScratch {

namespace {

constexpr const char* kHeader = "minicode-latex-sibling 1";

bool isHex(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isxdigit((unsigned char)c)) return false;
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// The next line of `text` from `at` (without its '\n'), or false at the end.
bool nextLine(const std::string& text, size_t& at, std::string& line) {
    const size_t nl = text.find('\n', at);
    if (nl == std::string::npos) return false;
    line = text.substr(at, nl - at);
    at = nl + 1;
    return true;
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

}  // namespace

std::string folderName(const std::string& pathSha256Hex) {
    if (pathSha256Hex.size() < 16 || !isHex(pathSha256Hex)) return "untitled";
    return lower(pathSha256Hex.substr(0, 16));
}

std::string folderName(const std::string& pathSha256Hex, const Owner& o) {
    return folderName(pathSha256Hex) + "-" + std::to_string(o.pid) + "-" +
           std::to_string(o.start) + "-" + std::to_string(o.view);
}

bool parseFolderName(const std::string& name, Owner* out) {
    // 16 hex digits (or "untitled"), then three numbers each after a '-'.
    const size_t first = name.find('-');
    if (first == std::string::npos) return false;
    const std::string head = name.substr(0, first);
    if (head != "untitled" && (head.size() != 16 || !isHex(head))) return false;
    std::string parts[3];
    size_t at = first + 1;
    for (int k = 0; k < 3; k++) {
        const size_t dash = k < 2 ? name.find('-', at) : name.size();
        if (dash == std::string::npos) return false;
        parts[k] = name.substr(at, dash - at);
        if (!allDigits(parts[k]) || parts[k].size() > 19) return false;
        at = dash + 1;
    }
    Owner o;
    o.pid = std::strtoll(parts[0].c_str(), nullptr, 10);
    o.start = std::strtoll(parts[1].c_str(), nullptr, 10);
    o.view = std::strtoull(parts[2].c_str(), nullptr, 10);
    if (o.pid <= 0 || o.view == 0) return false;
    *out = o;
    return true;
}

std::string siblingName(const std::string& stem, const Owner& o) {
    if (o.view == 0) return "." + stem + ".minicode.tex";
    return "." + stem + "." + std::to_string(o.pid) + "-" + std::to_string(o.view) +
           ".minicode.tex";
}

bool ownerGone(const Owner& o, double startNow) {
    if (startNow <= 0) return true;
    const double d = startNow - (double)o.start;
    return d < -1.0 || d > 1.0;
}

std::string format(const Record& r) {
    char nums[96];
    std::snprintf(nums, sizeof nums, "%lld\n%.3f\n%llu\n", r.pid, r.start, r.size);
    return std::string(kHeader) + "\n" + nums + lower(r.sha256) + "\n" + r.sibling + "\n";
}

bool parse(const std::string& text, Record* out) {
    size_t at = 0;
    std::string header, pid, start, size, sha;
    if (!nextLine(text, at, header) || header != kHeader) return false;
    if (!nextLine(text, at, pid) || !nextLine(text, at, start) || !nextLine(text, at, size) ||
        !nextLine(text, at, sha))
        return false;
    if (!allDigits(pid) || !allDigits(size) || sha.size() != 64 || !isHex(sha)) return false;
    char* end = nullptr;
    const double when = std::strtod(start.c_str(), &end);
    if (start.empty() || !end || *end != '\0') return false;
    std::string sibling = text.substr(at);
    if (!sibling.empty() && sibling.back() == '\n') sibling.pop_back();
    if (sibling.empty() || sibling[0] != '/') return false;
    Record r;
    r.sibling = sibling;
    r.pid = std::strtoll(pid.c_str(), nullptr, 10);
    r.start = when;
    r.size = std::strtoull(size.c_str(), nullptr, 10);
    r.sha256 = lower(sha);
    *out = r;
    return true;
}

bool isPreviewCopyName(const std::string& path) {
    const size_t slash = path.rfind('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::string tail = ".minicode.tex";
    if (!endsWith(name, tail)) return false;
    if (name == "untitled" + tail) return true;
    // ".<stem>.minicode.tex" with a stem of at least one character.
    return name.size() > tail.size() + 1 && name[0] == '.';
}

Action decide(const Record& r, bool writerAlive, bool siblingExists, unsigned long long size,
              const std::string& sha256) {
    if (writerAlive) return Action::Keep;
    if (!siblingExists || !isPreviewCopyName(r.sibling)) return Action::ForgetRecord;
    if (size != r.size || lower(sha256) != lower(r.sha256)) return Action::ForgetRecord;
    return Action::RemoveSibling;
}

}  // namespace LatexScratch
