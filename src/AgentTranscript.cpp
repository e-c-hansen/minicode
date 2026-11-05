// AgentTranscript.cpp — see AgentTranscript.h.
#include "AgentTranscript.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace AgentTranscript {

// ================================================================== files
Fs posixFs() {
    Fs fs;
    fs.list = [](const std::string &dir) {
        std::vector<std::string> names;
        DIR *d = opendir(dir.c_str());
        if (!d) return names;
        while (struct dirent *e = readdir(d)) {
            if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
            names.push_back(e->d_name);
        }
        closedir(d);
        std::sort(names.begin(), names.end());
        return names;
    };
    fs.stat = [](const std::string &path) {
        Stat s;
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) return s;
        s.exists = true;
        s.isDir = S_ISDIR(st.st_mode);
        s.size = (uint64_t)st.st_size;
#ifdef __APPLE__
        s.mtime = (double)st.st_mtimespec.tv_sec + st.st_mtimespec.tv_nsec / 1e9;
#else
        s.mtime = (double)st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9;
#endif
        s.inode = (uint64_t)st.st_ino;
        s.device = (uint64_t)st.st_dev;
        return s;
    };
    fs.read = [](const std::string &path, uint64_t offset, size_t maxBytes, std::string *out) {
        out->clear();
        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        out->resize(maxBytes);
        size_t got = 0;
        while (got < maxBytes) {
            ssize_t n = ::pread(fd, &(*out)[got], maxBytes - got, (off_t)(offset + got));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            got += (size_t)n;
        }
        ::close(fd);
        out->resize(got);
        return true;
    };
    fs.realpath = [](const std::string &path) {
        char buf[PATH_MAX];
        if (::realpath(path.c_str(), buf)) return std::string(buf);
        return path;
    };
    return fs;
}

// Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm).
static long long daysFromCivil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

double parseTime(const std::string &s) {
    auto num = [&](size_t at, size_t n, int *v) {
        if (at + n > s.size()) return false;
        int r = 0;
        for (size_t i = at; i < at + n; i++) {
            if (s[i] < '0' || s[i] > '9') return false;
            r = r * 10 + (s[i] - '0');
        }
        *v = r;
        return true;
    };
    int y, mo, d, h, mi, se;
    if (!num(0, 4, &y) || s.size() < 19 || s[4] != '-' || !num(5, 2, &mo) || s[7] != '-' ||
        !num(8, 2, &d) || (s[10] != 'T' && s[10] != ' ') || !num(11, 2, &h) || s[13] != ':' ||
        !num(14, 2, &mi) || s[16] != ':' || !num(17, 2, &se))
        return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return 0;
    double t = (double)daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400.0 + h * 3600 +
               mi * 60 + se;
    size_t i = 19;
    if (i < s.size() && s[i] == '.') {
        double f = 0.1;
        for (i++; i < s.size() && s[i] >= '0' && s[i] <= '9'; i++, f /= 10) t += (s[i] - '0') * f;
    }
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        int oh, om;
        if (num(i + 1, 2, &oh) && i + 3 < s.size() && s[i + 3] == ':' && num(i + 4, 2, &om))
            t -= (s[i] == '+' ? 1 : -1) * (oh * 3600 + om * 60);
    }
    return t;
}

// ================================================================ skimming
// A JSON skimmer: it checks a value is well formed and says where it ends,
// without building anything, so the parts of a line MiniCode does not use
// cost a memchr and nothing more.
namespace {

using SV = std::string_view;

inline const char *ws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    return p;
}

// p at the opening quote; returns just past the closing one.
const char *skipString(const char *p, const char *e) {
    ++p;
    const char *from = p;
    for (;;) {
        const char *q = (const char *)std::memchr(from, '"', (size_t)(e - from));
        if (!q) return nullptr;
        const char *b = q;
        while (b > p && b[-1] == '\\') --b;
        if (((q - b) & 1) == 0) return q + 1;
        from = q + 1;
    }
}

const char *skipValue(const char *p, const char *e, int depth);

template <class F>
const char *members(const char *p, const char *e, int depth, F &&f) {
    // p at '{'
    if (depth > 256) return nullptr;
    p = ws(p + 1, e);
    if (p < e && *p == '}') return p + 1;
    for (;;) {
        p = ws(p, e);
        if (p >= e || *p != '"') return nullptr;
        const char *ke = skipString(p, e);
        if (!ke) return nullptr;
        SV key(p + 1, (size_t)(ke - 1 - (p + 1)));
        p = ws(ke, e);
        if (p >= e || *p != ':') return nullptr;
        p = ws(p + 1, e);
        const char *vs = p;
        const char *ve = skipValue(p, e, depth + 1);
        if (!ve) return nullptr;
        f(key, vs, ve);
        p = ws(ve, e);
        if (p < e && *p == ',') { ++p; continue; }
        if (p < e && *p == '}') return p + 1;
        return nullptr;
    }
}

template <class F>
const char *elements(const char *p, const char *e, int depth, F &&f) {
    // p at '['
    if (depth > 256) return nullptr;
    p = ws(p + 1, e);
    if (p < e && *p == ']') return p + 1;
    for (;;) {
        p = ws(p, e);
        const char *vs = p;
        const char *ve = skipValue(p, e, depth + 1);
        if (!ve) return nullptr;
        f(vs, ve);
        p = ws(ve, e);
        if (p < e && *p == ',') { ++p; continue; }
        if (p < e && *p == ']') return p + 1;
        return nullptr;
    }
}

const char *skipValue(const char *p, const char *e, int depth) {
    p = ws(p, e);
    if (p >= e) return nullptr;
    auto lit = [&](const char *w) -> const char * {
        size_t n = std::strlen(w);
        if ((size_t)(e - p) >= n && !std::memcmp(p, w, n)) return p + n;
        return nullptr;
    };
    switch (*p) {
    case '"': return skipString(p, e);
    case '{': return members(p, e, depth, [](SV, const char *, const char *) {});
    case '[': return elements(p, e, depth, [](const char *, const char *) {});
    case 't': return lit("true");
    case 'f': return lit("false");
    case 'n': return lit("null");
    default: {
        const char *q = p;
        if (q < e && *q == '-') ++q;
        const char *digits = q;
        while (q < e && ((*q >= '0' && *q <= '9') || *q == '.' || *q == 'e' || *q == 'E' ||
                         *q == '+' || *q == '-'))
            ++q;
        return q > digits ? q : nullptr;
    }
    }
}

int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hex4(const char *p, const char *e, unsigned *v) {
    if (e - p < 4) return false;
    unsigned r = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexv(p[i]);
        if (h < 0) return false;
        r = r * 16 + (unsigned)h;
    }
    *v = r;
    return true;
}

void putUtf8(std::string &out, unsigned cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// Drops a UTF-8 sequence cut short at the end.
void trimUtf8(std::string &s) {
    size_t n = s.size(), i = n;
    while (i > 0 && n - i < 4 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) --i;
    if (i == 0) return;
    unsigned char lead = (unsigned char)s[i - 1];
    size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (lead >= 0xC0 && n - (i - 1) < want) s.resize(i - 1);
}

// Decodes a string's interior [p, e). Stops at `cap` bytes of output (*cut
// set). An escape cut off by `e` ends the text there (a window's edge).
void decodeInto(const char *p, const char *e, std::string &out, size_t cap, bool *cut) {
    out.clear();
    if (cut) *cut = false;
    while (p < e) {
        if (out.size() >= cap) {
            if (cut) *cut = true;
            break;
        }
        const char *bs = (const char *)std::memchr(p, '\\', (size_t)(e - p));
        const char *run = bs ? bs : e;
        size_t room = cap - out.size();
        if ((size_t)(run - p) > room) {
            out.append(p, room);
            if (cut) *cut = true;
            break;
        }
        out.append(p, (size_t)(run - p));
        p = run;
        if (p >= e) break;
        if (p + 1 >= e) break;
        char c = p[1];
        p += 2;
        switch (c) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
            unsigned v;
            if (!hex4(p, e, &v)) {
                if (e - p < 4) { p = e; break; }   // cut by a window's edge
                putUtf8(out, 0xFFFD);
                break;
            }
            p += 4;
            if (v >= 0xD800 && v < 0xDC00) {
                unsigned lo;
                if (e - p >= 6 && p[0] == '\\' && p[1] == 'u' && hex4(p + 2, e, &lo) &&
                    lo >= 0xDC00 && lo < 0xE000) {
                    v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                } else {
                    v = 0xFFFD;
                }
            } else if (v >= 0xDC00 && v < 0xE000) {
                v = 0xFFFD;
            }
            putUtf8(out, v);
            break;
        }
        default: out += c; break;   // \" \\ \/ and anything lenient
        }
    }
    trimUtf8(out);
}

std::string strOf(const char *vs, const char *ve, size_t cap = kInputCap, bool *cut = nullptr) {
    std::string s;
    if (ve - vs >= 2 && *vs == '"') decodeInto(vs + 1, ve - 1, s, cap, cut);
    return s;
}

bool isTrue(const char *vs, const char *ve) { return ve - vs == 4 && !std::memcmp(vs, "true", 4); }

class ClaudeCodeFormat : public Format {
public:
    const char *name() const override { return "Claude Code"; }

    bool parseLine(const char *data, size_t length, uint64_t offset, Line *out) const override {
        out->clear();
        out->span = {offset, (uint64_t)length};
        const char *e = data + length;
        const char *p = ws(data, e);
        if (p >= e || *p != '{') return false;
        const char *msgS = nullptr, *msgE = nullptr, *turS = nullptr, *turE = nullptr;
        const char *origS = nullptr, *origE = nullptr;
        std::string timestamp;
        const char *end = members(p, e, 0, [&](SV k, const char *vs, const char *ve) {
            if (k == "type") out->type = strOf(vs, ve, 64);
            else if (k == "timestamp") timestamp = strOf(vs, ve, 64);
            else if (k == "sessionId") out->sessionId = strOf(vs, ve, 128);
            else if (k == "agentId") out->agentId = strOf(vs, ve, 128);
            else if (k == "cwd") out->cwd = strOf(vs, ve, 4096);
            else if (k == "gitBranch") out->gitBranch = strOf(vs, ve, 512);
            else if (k == "version") out->version = strOf(vs, ve, 64);
            else if (k == "aiTitle") out->aiTitle = strOf(vs, ve, 512);
            else if (k == "isSidechain") out->sidechain = isTrue(vs, ve);
            else if (k == "isMeta" || k == "isCompactSummary") out->meta = out->meta || isTrue(vs, ve);
            else if (k == "message") { msgS = vs; msgE = ve; }
            else if (k == "toolUseResult") { turS = vs; turE = ve; }
            else if (k == "origin") { origS = vs; origE = ve; }
        });
        if (!end || ws(end, e) != e) return false;
        out->time = parseTime(timestamp);

        std::string originKind;
        bool hasOrigin = false;
        if (origS && *origS == '{') {
            hasOrigin = true;
            members(origS, origE, 1, [&](SV k, const char *vs, const char *ve) {
                if (k == "kind") originKind = strOf(vs, ve, 64);
            });
        }
        if (turS && *turS == '{') {
            members(turS, turE, 1, [&](SV k, const char *vs, const char *ve) {
                if (k == "agentId") out->spawnedAgent = strOf(vs, ve, 128);
            });
        }
        // Only the conversation's own lines: Claude Code also logs copies of
        // the messages it sends (api-request-blob), which would count twice.
        if (msgS && *msgS == '{' && (out->type == "user" || out->type == "assistant"))
            readMessage(data, offset, msgS, msgE, out);

        if (out->type == "user" && !out->meta) {
            bool text = false, result = false;
            for (const Block &b : out->blocks) {
                if (b.kind == Block::Kind::Text) text = true;
                if (b.kind == Block::Kind::ToolResult) result = true;
            }
            if (text && !result) {
                if (hasOrigin) {
                    out->prompt = originKind == "human";
                } else {
                    // No origin (older versions, a subagent's task): leave out
                    // what the harness wraps in tags (notifications, a slash
                    // command's output).
                    const std::string &t = out->blocks.front().text;
                    size_t i = 0;
                    while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\t')) i++;
                    out->prompt = i < t.size() && t[i] != '<';
                }
            }
        }
        return true;
    }

private:
    static void readMessage(const char *base, uint64_t offset, const char *ms, const char *me,
                            Line *out) {
        const char *cs = nullptr, *ce = nullptr;
        members(ms, me, 1, [&](SV k, const char *vs, const char *ve) {
            if (k == "id") out->messageId = strOf(vs, ve, 128);
            else if (k == "content") { cs = vs; ce = ve; }
        });
        if (!cs) return;
        if (*cs == '"') {
            Block b;
            b.kind = Block::Kind::Text;
            b.text = strOf(cs, ce, kTextCap, &b.textCut);
            out->blocks.push_back(std::move(b));
            return;
        }
        if (*cs != '[') return;
        elements(cs, ce, 2, [&](const char *bs, const char *be) {
            if (*bs != '{') return;
            std::string type;
            const char *textS = nullptr, *textE = nullptr, *inS = nullptr, *inE = nullptr;
            const char *conS = nullptr, *conE = nullptr;
            Block b;
            members(bs, be, 3, [&](SV k, const char *vs, const char *ve) {
                if (k == "type") type = strOf(vs, ve, 64);
                else if (k == "text") { textS = vs; textE = ve; }
                else if (k == "id") b.id = strOf(vs, ve, 128);
                else if (k == "tool_use_id") b.id = strOf(vs, ve, 128);
                else if (k == "name") b.name = strOf(vs, ve, 128);
                else if (k == "input") { inS = vs; inE = ve; }
                else if (k == "content") { conS = vs; conE = ve; }
                else if (k == "is_error") b.isError = isTrue(vs, ve);
            });
            if (type == "text") {
                b.kind = Block::Kind::Text;
                if (textS) b.text = strOf(textS, textE, kTextCap, &b.textCut);
                b.id.clear();
                b.name.clear();
                out->blocks.push_back(std::move(b));
            } else if (type == "tool_use") {
                b.kind = Block::Kind::ToolUse;
                if (inS && *inS == '{') {
                    members(inS, inE, 4, [&](SV k, const char *vs, const char *ve) {
                        if (k == "file_path" || (k == "notebook_path" && b.filePath.empty()))
                            b.filePath = strOf(vs, ve, kInputCap);
                        else if (k == "command") b.command = strOf(vs, ve, kInputCap, &b.commandCut);
                        else if (k == "description") b.description = strOf(vs, ve, 1024);
                    });
                }
                out->blocks.push_back(std::move(b));
            } else if (type == "tool_result") {
                b.kind = Block::Kind::ToolResult;
                auto spanOf = [&](const char *vs, const char *ve) {
                    return Span{offset + (uint64_t)(vs - base), (uint64_t)(ve - vs)};
                };
                if (conS && *conS == '"') {
                    b.output = spanOf(conS, conE);
                } else if (conS && *conS == '[') {
                    elements(conS, conE, 4, [&](const char *xs, const char *xe) {
                        if (!b.output.empty() || *xs != '{') return;
                        std::string t;
                        const char *ts = nullptr, *te = nullptr;
                        members(xs, xe, 5, [&](SV k, const char *vs, const char *ve) {
                            if (k == "type") t = strOf(vs, ve, 32);
                            else if (k == "text") { ts = vs; te = ve; }
                        });
                        if (t == "text" && ts && *ts == '"') b.output = spanOf(ts, te);
                    });
                }
                out->blocks.push_back(std::move(b));
            }
        });
    }
};

}  // namespace

void Line::clear() {
    type.clear();
    time = 0;
    sessionId.clear();
    agentId.clear();
    cwd.clear();
    gitBranch.clear();
    version.clear();
    messageId.clear();
    aiTitle.clear();
    sidechain = meta = prompt = false;
    spawnedAgent.clear();
    blocks.clear();
    span = {};
}

const Format &claudeCode() {
    static const ClaudeCodeFormat f;
    return f;
}

// ============================================================= incremental
bool Tail::read(const Fs &fs, const Format &format,
                const std::function<bool(const Line &)> &onLine,
                const std::function<void()> &onReset, ReadStats *stats, size_t chunk) {
    ReadStats local;
    ReadStats &st = stats ? *stats : local;
    Stat s = fs.stat(path);
    if (!s.exists || s.isDir) return false;
    if ((inode && (s.inode != inode || s.device != device)) || s.size < offset) {
        offset = 0;
        st.reset = true;
        if (onReset) onReset();
    }
    inode = s.inode;
    device = s.device;
    if (chunk < 4096) chunk = 4096;
    std::string carry, got;
    uint64_t base = offset;     // file offset of carry[0]
    uint64_t pos = offset;
    Line line;
    bool stop = false;
    while (pos < s.size && !stop) {
        size_t want = (size_t)std::min<uint64_t>(chunk, s.size - pos);
        if (!fs.read(path, pos, want, &got) || got.empty()) break;
        pos += got.size();
        st.bytesRead += got.size();
        if (carry.empty()) carry.swap(got);
        else carry += got;
        size_t from = 0;
        for (;;) {
            const char *nl = (const char *)std::memchr(carry.data() + from, '\n', carry.size() - from);
            if (!nl) break;
            size_t to = (size_t)(nl - carry.data());
            size_t n = to - from;
            if (n && carry[to - 1] == '\r') n--;
            const char *d = carry.data() + from;
            bool blank = true;
            for (size_t i = 0; i < n && blank; i++)
                blank = d[i] == ' ' || d[i] == '\t' || d[i] == '\r';
            if (!blank) {
                st.lines++;
                if (format.parseLine(d, n, base + from, &line)) stop = !onLine(line);
                else st.badLines++;
            }
            from = to + 1;
            if (stop) break;
        }
        carry.erase(0, from);
        base += from;
        offset = base;
    }
    return true;
}

// ================================================================== output
namespace {

void splitLines(const std::string &text, std::vector<std::string> &lines) {
    size_t i = 0;
    while (i <= text.size()) {
        size_t nl = text.find('\n', i);
        if (nl == std::string::npos) {
            if (i < text.size()) lines.push_back(text.substr(i));
            break;
        }
        size_t n = nl - i;
        if (n && text[nl - 1] == '\r') n--;
        lines.push_back(text.substr(i, n));
        i = nl + 1;
    }
}

void fillFromLines(std::vector<std::string> &&lines, int headLines, int tailLines, Output *o) {
    size_t h = std::min(lines.size(), (size_t)std::max(0, headLines));
    size_t t = std::min(lines.size() - h, (size_t)std::max(0, tailLines));
    o->head.assign(lines.begin(), lines.begin() + (long)h);
    o->tail.assign(lines.end() - (long)t, lines.end());
    o->omitted = (long long)(lines.size() - h - t);
    o->whole = o->omitted == 0;
}

}  // namespace

std::string decodeJsonFragment(const char *p, size_t n, bool fromStart, bool *ok) {
    const char *e = p + n;
    if (!fromStart) {
        // A backslash that does not follow a backslash always begins an
        // escape, so start at the first such "\n": the text before it is a
        // partial line anyway. With no line break in the window, start at
        // the first certain escape, or at the first whole UTF-8 character.
        const char *start = nullptr, *firstEsc = nullptr;
        for (const char *q = p + 1; q + 1 < e; q++) {
            if (*q == '\\' && q[-1] != '\\') {
                if (!firstEsc) firstEsc = q;
                if (q[1] == 'n') { start = q; break; }
            }
        }
        if (!start) start = firstEsc;
        if (!start) {
            start = p;
            while (start < e && ((unsigned char)*start & 0xC0) == 0x80) start++;
        }
        p = start;
    }
    std::string out;
    decodeInto(p, e, out, SIZE_MAX, nullptr);
    if (ok) *ok = true;
    return out;
}

std::string persistedOutputPath(const std::string &text) {
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\n')) i++;
    if (text.compare(i, 18, "<persisted-output>") != 0) return "";
    static const char kKey[] = "saved to: ";
    size_t k = text.find(kKey, i);
    if (k == std::string::npos) return "";
    k += sizeof kKey - 1;
    size_t end = text.find('\n', k);
    std::string path = text.substr(k, end == std::string::npos ? std::string::npos : end - k);
    while (!path.empty() && (path.back() == ' ' || path.back() == '\r')) path.pop_back();
    return path;
}

std::string backgroundOutputPath(const std::string &text) {
    static const char kKey[] = "Output is being written to: ";
    size_t k = text.find(kKey);
    if (k == std::string::npos || k > 4096) return "";
    k += sizeof kKey - 1;
    size_t end = k;
    while (end < text.size() && text[end] != '\n' && text[end] != ' ') end++;
    std::string path = text.substr(k, end - k);
    while (!path.empty() && (path.back() == '.' || path.back() == '\r')) path.pop_back();
    return path.empty() || path[0] != '/' ? "" : path;
}

Output readOutput(const Fs &fs, const std::string &path, const Span &span, int headLines,
                  int tailLines, size_t window) {
    Output o;
    o.bytes = span.length;
    if (span.length < 2) {
        o.whole = true;
        return o;
    }
    const uint64_t in0 = span.offset + 1, in1 = span.offset + span.length - 1;   // interior
    if (window < 1024) window = 1024;
    std::string raw;
    if (in1 - in0 <= 2 * (uint64_t)window) {
        if (!fs.read(path, in0, (size_t)(in1 - in0), &raw)) return o;
        std::string text;
        decodeInto(raw.data(), raw.data() + raw.size(), text, SIZE_MAX, nullptr);
        o.persistedPath = persistedOutputPath(text);
        std::vector<std::string> lines;
        splitLines(text, lines);
        fillFromLines(std::move(lines), headLines, tailLines, &o);
        return o;
    }
    // Only the two ends.
    o.omitted = -1;
    if (fs.read(path, in0, window, &raw)) {
        std::string text;
        decodeInto(raw.data(), raw.data() + raw.size(), text, SIZE_MAX, nullptr);
        o.persistedPath = persistedOutputPath(text);
        std::vector<std::string> lines;
        splitLines(text, lines);
        if (lines.size() > 1) lines.pop_back();   // cut by the window
        if ((int)lines.size() > headLines) lines.resize((size_t)std::max(0, headLines));
        o.head = std::move(lines);
    }
    if (fs.read(path, in1 - window, window, &raw)) {
        std::string text = decodeJsonFragment(raw.data(), raw.size(), false);
        if (!text.empty() && text[0] == '\n') text.erase(0, 1);
        std::vector<std::string> lines;
        splitLines(text, lines);
        if ((int)lines.size() > tailLines)
            lines.erase(lines.begin(), lines.end() - std::max(0, tailLines));
        o.tail = std::move(lines);
    }
    return o;
}

std::string readOutputText(const Fs &fs, const std::string &path, const Span &span,
                           size_t maxBytes, bool *cut) {
    if (cut) *cut = false;
    if (span.length < 2) return "";
    const uint64_t n = span.length - 2;
    // Escapes make the encoded text longer than the decoded, never shorter,
    // so reading maxBytes + a little more is always enough.
    size_t want = (size_t)std::min<uint64_t>(n, (uint64_t)maxBytes + 8);
    std::string raw;
    if (!fs.read(path, span.offset + 1, want, &raw)) return "";
    std::string text;
    bool c = false;
    decodeInto(raw.data(), raw.data() + raw.size(), text, maxBytes, &c);
    if (cut) *cut = c || want < n;
    return text;
}

Output readTextFile(const Fs &fs, const std::string &path, int headLines, int tailLines,
                    size_t window) {
    Output o;
    Stat s = fs.stat(path);
    if (!s.exists) return o;
    o.bytes = s.size;
    if (window < 1024) window = 1024;
    std::string raw;
    if (s.size <= 2 * (uint64_t)window) {
        if (!fs.read(path, 0, (size_t)s.size, &raw)) return o;
        trimUtf8(raw);
        std::vector<std::string> lines;
        splitLines(raw, lines);
        fillFromLines(std::move(lines), headLines, tailLines, &o);
        return o;
    }
    o.omitted = -1;
    if (fs.read(path, 0, window, &raw)) {
        std::vector<std::string> lines;
        splitLines(raw, lines);
        if (lines.size() > 1) lines.pop_back();
        if ((int)lines.size() > headLines) lines.resize((size_t)std::max(0, headLines));
        o.head = std::move(lines);
    }
    if (fs.read(path, s.size - window, window, &raw)) {
        size_t nl = raw.find('\n');
        if (nl != std::string::npos) raw.erase(0, nl + 1);
        std::vector<std::string> lines;
        splitLines(raw, lines);
        if ((int)lines.size() > tailLines)
            lines.erase(lines.begin(), lines.end() - std::max(0, tailLines));
        o.tail = std::move(lines);
    }
    return o;
}

}  // namespace AgentTranscript
