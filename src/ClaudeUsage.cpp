// ClaudeUsage.cpp — see ClaudeUsage.h. The transcript, dedupe, pricing,
// block and burn-rate rules are ported from ccusage
// (https://github.com/ccusage/ccusage, MIT License, Copyright (c) 2025
// ryoppippi); the header says which of its files each comes from.
#include "ClaudeUsage.h"
#include "Json.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace ClaudeUsage {

void Tokens::add(const Tokens &o) {
    input += o.input;
    write5m += o.write5m;
    write1h += o.write1h;
    cacheRead += o.cacheRead;
    output += o.output;
}

// ------------------------------------------------------------- skimming
namespace {

// A forward-only reader over one line of JSON that decodes only what it is
// asked for and steps over the rest (a reply's text, a Write's whole file).
struct Skim {
    const char *p, *end;
    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }
    bool eat(char c) {
        ws();
        if (p < end && *p == c) { p++; return true; }
        return false;
    }
    // At a '"': past the closing quote.
    bool skipString() {
        if (p >= end || *p != '"') return false;
        for (p++; p < end; p++) {
            if (*p == '\\') { p++; continue; }
            if (*p == '"') { p++; return true; }
        }
        return false;
    }
    // At a '"': the string, its common escapes decoded (\uXXXX as UTF-8,
    // surrogate pairs joined); `cap` bytes at most are kept.
    bool readString(std::string *out, size_t cap = 512) {
        out->clear();
        if (p >= end || *p != '"') return false;
        for (p++; p < end; p++) {
            char c = *p;
            if (c == '"') { p++; return true; }
            if (c != '\\') {
                if (out->size() < cap) out->push_back(c);
                continue;
            }
            if (++p >= end) return false;
            unsigned cp = 0;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                if (end - p < 5) return false;
                for (int i = 1; i <= 4; i++) {
                    const char h = p[i];
                    cp = cp * 16 + (unsigned)(h >= '0' && h <= '9' ? h - '0'
                                              : h >= 'a' && h <= 'f' ? h - 'a' + 10
                                              : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
                }
                p += 4;
                if (out->size() >= cap) continue;
                if (cp < 0x80) out->push_back((char)cp);
                else if (cp < 0x800) {
                    out->push_back((char)(0xC0 | (cp >> 6)));
                    out->push_back((char)(0x80 | (cp & 0x3F)));
                } else {
                    out->push_back((char)(0xE0 | (cp >> 12)));
                    out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                    out->push_back((char)(0x80 | (cp & 0x3F)));
                }
                continue;
            }
            default: c = *p; break;   // \" \\ \/ and anything else as itself
            }
            if (out->size() < cap) out->push_back(c);
        }
        return false;
    }
    // Past one value of any kind.
    bool skipValue() {
        ws();
        if (p >= end) return false;
        if (*p == '"') return skipString();
        if (*p == '{' || *p == '[') {
            int depth = 0;
            while (p < end) {
                const char c = *p;
                if (c == '"') {
                    if (!skipString()) return false;
                    continue;
                }
                if (c == '{' || c == '[') depth++;
                else if (c == '}' || c == ']') {
                    if (--depth == 0) { p++; return true; }
                }
                p++;
            }
            return false;
        }
        const char *start = p;
        while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' &&
               *p != '\n' && *p != '\r')
            p++;
        return p > start;
    }
    bool readBool(bool *out) {
        ws();
        if (end - p >= 4 && std::memcmp(p, "true", 4) == 0) { *out = true; p += 4; return true; }
        if (end - p >= 5 && std::memcmp(p, "false", 5) == 0) { *out = false; p += 5; return true; }
        return skipValue();
    }
    // An object's keys in turn: `onKey` must consume the value.
    template <class F> bool object(F onKey) {
        if (!eat('{')) return false;
        if (eat('}')) return true;
        std::string key;
        for (;;) {
            ws();
            if (!readString(&key, 64)) return false;
            if (!eat(':')) return false;
            ws();
            if (!onKey(key)) return false;
            if (eat(',')) continue;
            return eat('}');
        }
    }
};

uint64_t count(const Json &j) {
    if (!j.isNumber()) return 0;
    const double v = j.asNumber();
    return v > 0 && v < 1e15 ? (uint64_t)v : 0;
}

bool readUsage(const std::string &text, Entry *e) {
    Json u;
    if (!Json::parse(text, u) || !u.isObject()) return false;
    if (!u["input_tokens"].isNumber() && !u["output_tokens"].isNumber()) return false;
    Tokens &t = e->tokens;
    t.input = count(u["input_tokens"]);
    t.output = count(u["output_tokens"]);
    t.cacheRead = count(u["cache_read_input_tokens"]);
    uint64_t writes = count(u["cache_creation_input_tokens"]);
    const Json &split = u["cache_creation"];
    if (split.isObject()) {
        const uint64_t w5 = count(split["ephemeral_5m_input_tokens"]);
        const uint64_t w1 = count(split["ephemeral_1h_input_tokens"]);
        if (writes == 0) writes = w5 + w1;
        // The split may cover only a reply's first iteration (an advisor
        // turn) while the total covers them all: the hour's share is what
        // the split says, the rest is priced as 5-minute writes.
        t.write1h = std::min(writes, w1);
        e->detailed = true;
    }
    t.write5m = writes - t.write1h;
    e->fast = u["speed"].asString() == "fast";
    return true;
}

std::string baseName(const std::string &path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool endsWith(const std::string &s, const char *suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

}  // namespace

bool parseUsageLine(const char *data, size_t length, const std::string &fallbackSession,
                    Entry *out) {
    *out = Entry();
    Skim s{data, data + length};
    std::string type, stamp, usageText;
    bool haveUsage = false;
    const bool ok = s.object([&](const std::string &key) {
        if (key == "type") return s.readString(&type, 32);
        if (key == "timestamp") return s.readString(&stamp, 64);
        if (key == "requestId") return s.readString(&out->requestId, 128);
        if (key == "sessionId") return s.readString(&out->sessionId, 128);
        if (key == "isSidechain") return s.readBool(&out->sidechain);
        if (key != "message") return s.skipValue();
        if (s.p >= s.end || *s.p != '{') return s.skipValue();
        return s.object([&](const std::string &mk) {
            if (mk == "id") return s.readString(&out->messageId, 128);
            if (mk == "model") return s.readString(&out->model, 128);
            if (mk != "usage") return s.skipValue();
            const char *from = s.p;
            if (!s.skipValue()) return false;
            usageText.assign(from, (size_t)(s.p - from));
            haveUsage = true;
            return true;
        });
    });
    if (!ok || type != "assistant" || !haveUsage) return false;
    if (!readUsage(usageText, out)) return false;
    if (out->tokens.total() == 0) return false;
    out->time = AgentTranscript::parseTime(stamp);
    if (out->time <= 0) return false;
    if (out->sessionId.empty()) out->sessionId = fallbackSession;
    return true;
}

// ------------------------------------------------------------- prices
namespace {

struct PriceRow {
    const char *model;
    double input, output, cacheRead;
    uint64_t longContext;
    double longInput, longOutput, longCacheRead;
    double fast;
};

// Anthropic's API list prices, US dollars per million tokens (November
// 2026). Newest first within each family: the first row of a family is
// what an unknown model of that family is priced as.
const PriceRow kPrices[] = {
    {"claude-fable-5-2", 8, 40, 0.20, 0, 0, 0, 0, 2},
    {"claude-fable-5-1", 10, 50, 0.25, 0, 0, 0, 0, 2},
    {"claude-fable-5", 10, 50, 1.0, 0, 0, 0, 0, 2},
    {"claude-mythos-5-2", 8, 40, 0.20, 0, 0, 0, 0, 2},
    {"claude-mythos-5-1", 10, 50, 0.25, 0, 0, 0, 0, 2},
    {"claude-mythos-5", 10, 50, 1.0, 0, 0, 0, 0, 2},
    {"claude-opus-5-5", 4, 20, 0.20, 0, 0, 0, 0, 2},
    {"claude-opus-5", 5, 25, 0.5, 0, 0, 0, 0, 2},
    {"claude-opus-4-8", 5, 25, 0.5, 0, 0, 0, 0, 2},
    {"claude-opus-4-7", 5, 25, 0.5, 0, 0, 0, 0, 2},
    {"claude-opus-4-6", 5, 25, 0.5, 0, 0, 0, 0, 6},
    {"claude-opus-4-5", 5, 25, 0.5, 0, 0, 0, 0, 2},
    {"claude-opus-4-1", 15, 75, 1.5, 0, 0, 0, 0, 2},
    {"claude-opus-4", 15, 75, 1.5, 0, 0, 0, 0, 2},
    {"claude-3-opus", 15, 75, 1.5, 0, 0, 0, 0, 2},
    {"claude-sonnet-5-5", 2, 10, 0.10, 0, 0, 0, 0, 2},
    {"claude-sonnet-5", 2, 10, 0.20, 0, 0, 0, 0, 2},
    {"claude-sonnet-4-6", 3, 15, 0.3, 0, 0, 0, 0, 2},
    {"claude-sonnet-4-5", 3, 15, 0.3, 200000, 6, 22.5, 0.6, 2},
    {"claude-sonnet-4", 3, 15, 0.3, 200000, 6, 22.5, 0.6, 2},
    {"claude-3-7-sonnet", 3, 15, 0.3, 0, 0, 0, 0, 2},
    {"claude-3-5-sonnet", 3, 15, 0.3, 0, 0, 0, 0, 2},
    {"claude-haiku-5-5", 0.10, 0.50, 0.01, 100000, 0.5, 2.5, 0.05, 2},
    {"claude-haiku-4-5", 1, 5, 0.1, 0, 0, 0, 0, 2},
    {"claude-3-5-haiku", 0.8, 4, 0.08, 0, 0, 0, 0, 2},
    {"claude-3-haiku", 0.25, 1.25, 0.03, 0, 0, 0, 0, 2},
};

Price fromRow(const PriceRow &r) {
    Price p;
    p.input = r.input;
    p.output = r.output;
    p.cacheRead = r.cacheRead;
    p.longContext = r.longContext;
    p.longInput = r.longInput;
    p.longOutput = r.longOutput;
    p.longCacheRead = r.longCacheRead;
    p.fastMultiplier = r.fast;
    return p;
}

}  // namespace

std::string normalizeModel(const std::string &model) {
    std::string m;
    for (char c : model) m += (char)std::tolower((unsigned char)c);
    // "claude-opus-5-5[1m]": the context window, not another model.
    const size_t bracket = m.find('[');
    if (bracket != std::string::npos) m.erase(bracket);
    // "us.anthropic.claude-opus-4-8-v1:0", "anthropic/claude-...".
    const size_t vendor = m.rfind("anthropic.");
    if (vendor != std::string::npos) m.erase(0, vendor + 10);
    const size_t slash = m.rfind('/');
    if (slash != std::string::npos) m.erase(0, slash + 1);
    for (char cut : {'@', ':'}) {
        const size_t at = m.find(cut);
        if (at != std::string::npos) m.erase(at);
    }
    if (endsWith(m, "-v1") || endsWith(m, "-v2")) m.erase(m.size() - 3);
    // A dated snapshot: "-20251101".
    if (m.size() > 9 && m[m.size() - 9] == '-') {
        bool digits = true;
        for (size_t i = m.size() - 8; i < m.size(); i++) digits = digits && std::isdigit((unsigned char)m[i]);
        if (digits) m.erase(m.size() - 9);
    }
    while (!m.empty() && (m.back() == ' ' || m.back() == '-')) m.pop_back();
    return m;
}

Price priceFor(const std::string &model, bool *known) {
    const std::string m = normalizeModel(model);
    const PriceRow *best = nullptr;
    size_t bestLen = 0;
    for (const PriceRow &r : kPrices) {
        const size_t n = std::strlen(r.model);
        if (m.compare(0, n, r.model) != 0) continue;
        if (m.size() != n && m[n] != '-') continue;
        if (n > bestLen) { best = &r; bestLen = n; }
    }
    if (known) *known = best != nullptr;
    if (best) return fromRow(*best);
    for (const char *family : {"fable", "mythos", "opus", "sonnet", "haiku"})
        if (m.find(family) != std::string::npos)
            for (const PriceRow &r : kPrices)
                if (std::strstr(r.model, family)) return fromRow(r);
    for (const PriceRow &r : kPrices)
        if (std::strstr(r.model, "opus")) return fromRow(r);
    return Price();
}

double costOf(const Tokens &t, const Price &p, bool fast) {
    const uint64_t context = t.input + t.write5m + t.write1h + t.cacheRead;
    const bool wide = p.longContext && context > p.longContext;
    const double in = wide ? p.longInput : p.input;
    const double out = wide ? p.longOutput : p.output;
    const double read = wide ? p.longCacheRead : p.cacheRead;
    double c = (double)t.input * in + (double)t.write5m * in * 1.25 + (double)t.write1h * in * 2.0 +
               (double)t.cacheRead * read + (double)t.output * out;
    c /= 1e6;
    return fast ? c * p.fastMultiplier : c;
}

// ------------------------------------------------------------- the tally
namespace {

std::string dedupeKey(const Entry &e) {
    if (e.messageId.empty()) return std::string();
    std::string key = e.messageId;
    key += '\x1f';
    key += e.requestId;
    if (e.requestId.empty()) {
        key += '\x1f';
        key += e.sessionId;
        const bool anthropic = e.messageId.compare(0, 4, "msg_") == 0 ||
                               e.messageId.compare(0, 5, "resp_") == 0;
        if (!anthropic) {
            char t[32];
            std::snprintf(t, sizeof t, "\x1f%.3f", e.time);
            key += t;
        }
    }
    return key;
}

bool better(const Entry &candidate, const Entry &existing) {
    if (candidate.sidechain != existing.sidechain) return existing.sidechain;
    const uint64_t a = candidate.tokens.total(), b = existing.tokens.total();
    if (a != b) return a > b;
    const int da = (candidate.fast ? 2 : 0) + (candidate.detailed ? 1 : 0);
    const int db = (existing.fast ? 2 : 0) + (existing.detailed ? 1 : 0);
    return da > db;
}

}  // namespace

bool Tally::add(Entry e) {
    e.cost = costOf(e.tokens, priceFor(e.model), e.fast);
    const std::string key = dedupeKey(e);
    if (!key.empty()) {
        auto it = index_.find(key);
        if (it != index_.end()) {
            if (!better(e, entries_[it->second])) return false;
            entries_[it->second] = std::move(e);
            return true;
        }
        index_[key] = entries_.size();
    }
    entries_.push_back(std::move(e));
    return true;
}

std::vector<Entry> Tally::sorted() const {
    std::vector<Entry> out = entries_;
    std::stable_sort(out.begin(), out.end(),
                     [](const Entry &a, const Entry &b) { return a.time < b.time; });
    return out;
}

std::map<std::string, std::map<std::string, SessionModel>> Tally::bySession() const {
    std::map<std::string, std::map<std::string, SessionModel>> out;
    for (const Entry &e : entries_) {
        SessionModel &sm = out[e.sessionId][normalizeModel(e.model)];
        sm.tokens.add(e.tokens);
        sm.cost += e.cost;
        sm.responses++;
    }
    return out;
}

std::map<std::string, SessionModel> Tally::byModel() const {
    std::map<std::string, SessionModel> out;
    for (const Entry &e : entries_) {
        SessionModel &sm = out[normalizeModel(e.model)];
        sm.tokens.add(e.tokens);
        sm.cost += e.cost;
        sm.responses++;
    }
    return out;
}

void Tally::clear() {
    entries_.clear();
    index_.clear();
}

// ------------------------------------------------------------- scanning
namespace {

std::string join(const std::string &dir, const std::string &name) {
    return dir.empty() || dir.back() == '/' ? dir + name : dir + "/" + name;
}

}  // namespace

Scanner::Stats Scanner::scan(const AgentTranscript::Fs &fs, const std::string &projectsDir,
                             double since, Tally &tally) {
    Stats stats;
    if (projectsDir.empty()) return stats;
    auto consider = [&](const std::string &path) {
        const AgentTranscript::Stat st = fs.stat(path);
        if (!st.exists || st.isDir || st.mtime < since) return;
        stats.files++;
        readFile(fs, path, tally, stats);
    };
    for (const std::string &project : fs.list(projectsDir)) {
        const std::string dir = join(projectsDir, project);
        if (!fs.stat(dir).isDir) continue;
        for (const std::string &name : fs.list(dir)) {
            const std::string path = join(dir, name);
            if (endsWith(name, ".jsonl")) {
                consider(path);
                continue;
            }
            // A session's folder: its subagents' transcripts.
            const std::string sub = join(path, "subagents");
            const AgentTranscript::Stat sst = fs.stat(sub);
            if (!sst.exists || !sst.isDir) continue;
            for (const std::string &agent : fs.list(sub))
                if (endsWith(agent, ".jsonl")) consider(join(sub, agent));
        }
    }
    return stats;
}

void Scanner::readFile(const AgentTranscript::Fs &fs, const std::string &path, Tally &tally,
                       Stats &stats) {
    const AgentTranscript::Stat st = fs.stat(path);
    FileState &f = files_[path];
    if (f.inode != st.inode || f.device != st.device || st.size < f.offset) {
        f = FileState();
        f.inode = st.inode;
        f.device = st.device;
    }
    std::string session = baseName(path);
    if (endsWith(session, ".jsonl")) session.erase(session.size() - 6);
    const size_t kChunk = 4u << 20, kMaxLine = 64u << 20;
    size_t want = kChunk;
    std::string buf;
    while (f.offset < st.size) {
        if (!fs.read(path, f.offset, want, &buf) || buf.empty()) return;
        stats.bytesRead += buf.size();
        size_t lineStart = 0;
        for (;;) {
            const char *nl = (const char *)std::memchr(buf.data() + lineStart, '\n',
                                                       buf.size() - lineStart);
            if (!nl) break;
            const size_t len = (size_t)(nl - (buf.data() + lineStart));
            const char *line = buf.data() + lineStart;
            // Most lines are not a reply's usage: look before skimming.
            static const char kUsage[] = "\"usage\"", kAssistant[] = "\"assistant\"";
            Entry e;
            if (len > 20 && memmem(line, len, kUsage, sizeof kUsage - 1) &&
                memmem(line, len, kAssistant, sizeof kAssistant - 1) &&
                parseUsageLine(line, len, session, &e) && tally.add(std::move(e)))
                stats.entries++;
            lineStart += len + 1;
        }
        if (lineStart == 0) {
            // No whole line in the window: a line still being written, or one
            // longer than the window.
            if (f.offset + buf.size() >= st.size) return;   // being written: next time
            if (want < kMaxLine) {
                want *= 2;
                continue;
            }
            // Longer than any reply's usage line could need: step over it.
            stats.skipped++;
            f.offset += buf.size();
            std::string rest;
            while (fs.read(path, f.offset, kChunk, &rest) && !rest.empty()) {
                const char *nl = (const char *)std::memchr(rest.data(), '\n', rest.size());
                if (nl) {
                    f.offset += (uint64_t)(nl - rest.data()) + 1;
                    break;
                }
                f.offset += rest.size();
            }
            want = kChunk;
            continue;
        }
        f.offset += lineStart;
        want = kChunk;
    }
}

// ------------------------------------------------------------- blocks
namespace {
constexpr double kHour = 3600, kFiveHours = 5 * 3600, kWeek = 7 * 24 * 3600;
}

std::vector<Block> blocks(const std::vector<Entry> &entries, double now) {
    std::vector<Block> out;
    Block cur;
    bool open = false;
    for (const Entry &e : entries) {
        if (open && (e.time - cur.start > kFiveHours || e.time - cur.last > kFiveHours)) {
            cur.active = now - cur.last < kFiveHours && now < cur.end;
            out.push_back(cur);
            open = false;
        }
        if (!open) {
            cur = Block();
            cur.start = std::floor(e.time / kHour) * kHour;
            cur.end = cur.start + kFiveHours;
            cur.first = e.time;
            open = true;
        }
        cur.last = e.time;
        cur.cost += e.cost;
        cur.tokens.add(e.tokens);
        cur.entries++;
    }
    if (open) {
        cur.active = now - cur.last < kFiveHours && now < cur.end;
        out.push_back(cur);
    }
    return out;
}

double blockCostPerHour(const Block &b) {
    const double minutes = (b.last - b.first) / 60;
    return minutes > 0 ? b.cost / minutes * 60 : 0;
}

double costBetween(const std::vector<Entry> &entries, double from, double to) {
    if (to <= from) return 0;
    auto it = std::upper_bound(entries.begin(), entries.end(), from,
                               [](double t, const Entry &e) { return t < e.time; });
    double c = 0;
    for (; it != entries.end() && it->time <= to; ++it) c += it->cost;
    return c;
}

std::vector<SessionRate> sessionRates(const std::vector<Entry> &entries, double now,
                                      double window) {
    std::map<std::string, SessionRate> by;
    auto it = std::upper_bound(entries.begin(), entries.end(), now - window,
                               [](double t, const Entry &e) { return t < e.time; });
    for (; it != entries.end() && it->time <= now; ++it) {
        SessionRate &r = by[it->sessionId];
        r.sessionId = it->sessionId;
        r.costPerHour += it->cost;
        r.last = std::max(r.last, it->time);
    }
    std::vector<SessionRate> out;
    for (auto &kv : by) {
        kv.second.costPerHour /= window / kHour;
        out.push_back(kv.second);
    }
    std::sort(out.begin(), out.end(), [](const SessionRate &a, const SessionRate &b) {
        return a.costPerHour != b.costPerHour ? a.costPerHour > b.costPerHour
                                              : a.sessionId < b.sessionId;
    });
    return out;
}

// ------------------------------------------------------------- samples
namespace {

double epochOf(const Json &j) {
    if (j.isNumber()) {
        double v = j.asNumber();
        if (v > 1e12) v /= 1000;   // milliseconds
        return v > 0 ? v : 0;
    }
    if (j.isString()) return AgentTranscript::parseTime(j.asString());
    return 0;
}

bool readWindow(const Json &w, double *pct, double *reset) {
    if (!w.isObject() || !w["used_percentage"].isNumber()) return false;
    const double p = w["used_percentage"].asNumber();
    const double r = epochOf(w["resets_at"]);
    if (!(p >= 0) || p > 1000 || r <= 0) return false;
    *pct = p;
    *reset = r;
    return true;
}

std::string pctText(double p) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", p);
    std::string s = b;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

bool number(const std::string &w, double *out) {
    if (w.empty()) return false;
    char *e = nullptr;
    const double v = std::strtod(w.c_str(), &e);
    if (!e || *e || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

}  // namespace

bool sampleFromStatusJson(const std::string &json, double now, Sample *out) {
    *out = Sample();
    Json j;
    if (!Json::parse(json, j) || !j.isObject()) return false;
    const Json &rl = j["rate_limits"];
    if (!rl.isObject()) return false;
    out->time = now;
    readWindow(rl["five_hour"], &out->fivePct, &out->fiveReset);
    readWindow(rl["seven_day"], &out->weekPct, &out->weekReset);
    if (!out->hasFive()) out->fivePct = -1, out->fiveReset = 0;
    if (!out->hasWeek()) out->weekPct = -1, out->weekReset = 0;
    return out->hasFive() || out->hasWeek();
}

std::string formatSample(const Sample &s) {
    char b[160];
    auto win = [](bool has, double pct, double reset) {
        if (!has) return std::string("- -");
        char r[32];
        std::snprintf(r, sizeof r, "%.0f", std::floor(reset));
        return pctText(pct) + " " + r;
    };
    std::snprintf(b, sizeof b, "v1 %.0f %s %s", std::floor(s.time),
                  win(s.hasFive(), s.fivePct, s.fiveReset).c_str(),
                  win(s.hasWeek(), s.weekPct, s.weekReset).c_str());
    return b;
}

bool parseSample(const std::string &line, Sample *out) {
    *out = Sample();
    std::istringstream in(line);
    std::string w[7];
    int n = 0;
    while (n < 7 && in >> w[n]) n++;
    if (n != 6 || w[0] != "v1") return false;
    if (!number(w[1], &out->time) || out->time <= 0) return false;
    auto win = [](const std::string &p, const std::string &r, double *pct, double *reset) {
        if (p == "-" && r == "-") return true;
        return number(p, pct) && number(r, reset) && *pct >= 0 && *reset > 0;
    };
    if (!win(w[2], w[3], &out->fivePct, &out->fiveReset)) return false;
    if (!win(w[4], w[5], &out->weekPct, &out->weekReset)) return false;
    return out->hasFive() || out->hasWeek();
}

std::vector<Sample> parseLog(const std::string &text) {
    std::vector<Sample> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        Sample s;
        if (parseSample(text.substr(start, nl - start), &s)) out.push_back(s);
        start = nl + 1;
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Sample &a, const Sample &b) { return a.time < b.time; });
    return out;
}

bool worthLogging(const Sample &last, const Sample &next, double minGap) {
    if (last.time <= 0) return true;
    if (std::fabs(last.fivePct - next.fivePct) > 1e-6 || std::fabs(last.weekPct - next.weekPct) > 1e-6)
        return true;
    if (std::fabs(last.fiveReset - next.fiveReset) > 1 || std::fabs(last.weekReset - next.weekReset) > 1)
        return true;
    return next.time - last.time >= minGap;
}

// ------------------------------------------------------------- the fit
double windowLength(Window w) { return w == Window::FiveHour ? kFiveHours : kWeek; }

namespace {

struct Reading {
    double dp = 0, dc = 0;
};

struct Group {
    double reset = 0;
    std::vector<std::pair<double, double>> points;   // time, percent
    std::vector<Reading> readings;
};

// A move this large in cost (dollars at API prices) or percent is worth a
// reading; smaller ones are added to the next.
constexpr double kReadingCost = 0.25, kReadingPct = 2.0;

std::vector<Group> groups(const std::vector<Sample> &samples, const std::vector<Entry> &entries,
                          Window w, double coveredFrom) {
    std::vector<Group> out;
    for (const Sample &s : samples) {
        const bool has = w == Window::FiveHour ? s.hasFive() : s.hasWeek();
        if (!has) continue;
        const double pct = w == Window::FiveHour ? s.fivePct : s.weekPct;
        const double reset = w == Window::FiveHour ? s.fiveReset : s.weekReset;
        if (s.time >= reset) continue;   // taken after its window ended
        if (out.empty() || std::fabs(out.back().reset - reset) > 90) {
            Group g;
            g.reset = reset;
            out.push_back(g);
        }
        out.back().points.push_back({s.time, pct});
    }
    const double length = windowLength(w);
    for (Group &g : out) {
        const double start = g.reset - length;
        double prevT = start, prevP = 0;
        if (start < coveredFrom && !g.points.empty()) {
            prevT = g.points.front().first;
            prevP = g.points.front().second;
        }
        for (size_t i = 0; i < g.points.size(); i++) {
            const double t = g.points[i].first, p = g.points[i].second;
            if (t <= prevT) continue;
            const double dc = costBetween(entries, prevT, t);
            const double dp = p - prevP;
            if (dp < -0.5) {   // the percent fell: start over from here
                prevT = t;
                prevP = p;
                continue;
            }
            const bool last = i + 1 == g.points.size();
            if (dc >= kReadingCost || dp >= kReadingPct || (last && (dc >= 0.05 || dp >= 0.5))) {
                g.readings.push_back({std::max(0.0, dp), dc});
                prevT = t;
                prevP = p;
            }
        }
    }
    return out;
}

Fit fitFrom(const std::vector<Group> &gs) {
    std::vector<std::pair<double, double>> ratios;   // ratio, weight
    double weight = 0;
    for (const Group &g : gs)
        for (const Reading &r : g.readings)
            if (r.dc > 0) {
                ratios.push_back({r.dp / r.dc, r.dc});
                weight += r.dc;
            }
    Fit f;
    f.readings = (int)ratios.size();
    if (ratios.empty() || weight <= 0) return f;
    std::sort(ratios.begin(), ratios.end());
    double acc = 0;
    for (const auto &r : ratios) {
        acc += r.second;
        if (acc >= weight / 2) {
            f.scale = r.first;
            break;
        }
    }
    f.ok = f.scale > 0;
    return f;
}

}  // namespace

Fit fitScale(const std::vector<Sample> &samples, const std::vector<Entry> &entries, Window w) {
    return fitFrom(groups(samples, entries, w, 0));
}

// ------------------------------------------------------------- the forecast
namespace {

WindowForecast windowForecast(const std::vector<Sample> &samples, const std::vector<Entry> &entries,
                              double now, Window w, double coveredFrom) {
    WindowForecast out;
    const double length = windowLength(w);
    const std::vector<Group> gs = groups(samples, entries, w, coveredFrom);
    out.fit = fitFrom(gs);
    const Sample *last = nullptr;
    for (const Sample &s : samples)
        if (s.time <= now && (w == Window::FiveHour ? s.hasFive() : s.hasWeek()) &&
            (!last || s.time >= last->time))
            last = &s;
    if (!last) return out;
    out.known = true;
    out.sampledAt = last->time;
    const double pct = w == Window::FiveHour ? last->fivePct : last->weekPct;
    const double reset = w == Window::FiveHour ? last->fiveReset : last->weekReset;
    const double scale = out.fit.ok ? out.fit.scale : 0;
    double windowStart;
    if (now < reset) {
        out.resetsAt = reset;
        windowStart = reset - length;
        out.pctNow = pct + scale * costBetween(entries, last->time, now);
        // What the current window's readings do not explain.
        if (out.fit.ok)
            for (const Group &g : gs)
                if (std::fabs(g.reset - reset) <= 90)
                    for (const Reading &r : g.readings)
                        out.unseenPct += std::max(0.0, r.dp - scale * r.dc);
    } else {
        // It reset since the last reading: the new window is worked out.
        out.estimated = true;
        if (w == Window::FiveHour) {
            // A 5-hour window starts with the first request after the last.
            auto it = std::upper_bound(entries.begin(), entries.end(), reset,
                                       [](double t, const Entry &e) { return t < e.time; });
            windowStart = it != entries.end() && it->time <= now ? it->time : now;
            out.resetsAt = windowStart + length;
            out.pctNow = scale * costBetween(entries, windowStart - 1e-3, now);
        } else {
            double r = reset;
            while (r <= now) r += length;
            out.resetsAt = r;
            windowStart = r - length;
            out.pctNow = scale * costBetween(entries, windowStart, now);
        }
    }
    // The rate, over a few spans back from now (never before the window).
    const std::vector<double> spans = w == Window::FiveHour
                                          ? std::vector<double>{15 * 60, kHour, length}
                                          : std::vector<double>{kHour, kFiveHours, length};
    const size_t central = w == Window::FiveHour ? 1 : 0;
    std::vector<double> rates;
    for (double span : spans) {
        const double from = std::max(now - span, windowStart);
        const double hours = std::max(now - from, 60.0) / kHour;
        rates.push_back(scale * costBetween(entries, from, now) / hours);
    }
    if (out.fit.ok) {
        out.pctPerHour = rates[std::min(central, rates.size() - 1)];
        out.pctPerHourLow = *std::min_element(rates.begin(), rates.end());
        out.pctPerHourHigh = *std::max_element(rates.begin(), rates.end());
    } else if (!out.estimated) {
        // No fit yet: the readings' own slope over the last hour.
        for (const Sample &s : samples) {
            const bool has = w == Window::FiveHour ? s.hasFive() : s.hasWeek();
            const double r = w == Window::FiveHour ? s.fiveReset : s.weekReset;
            if (!has || std::fabs(r - reset) > 90 || s.time < last->time - kHour) continue;
            const double dt = last->time - s.time;
            if (dt >= 600) {
                const double p0 = w == Window::FiveHour ? s.fivePct : s.weekPct;
                out.pctPerHour = std::max(0.0, (pct - p0) / (dt / kHour));
            }
            break;
        }
        out.pctPerHourLow = out.pctPerHourHigh = out.pctPerHour;
    }
    const double remaining = 100 - out.pctNow;
    auto hit = [&](double rate) -> double {
        if (remaining <= 0) return now;
        if (rate <= 0) return 0;
        const double at = now + remaining / rate * kHour;
        return at < out.resetsAt ? at : 0;
    };
    out.hitAt = hit(out.pctPerHour);
    out.hitEarliest = hit(out.pctPerHourHigh);
    out.hitLatest = hit(out.pctPerHourLow);
    out.pctAtReset = out.pctNow + out.pctPerHour * std::max(0.0, out.resetsAt - now) / kHour;
    return out;
}

std::string pctWord(double p) {
    char b[32];
    if (p < 0) p = 0;
    std::snprintf(b, sizeof b, p >= 10 || p == 0 ? "%.0f%%" : "%.1f%%", std::min(p, 999.0));
    return b;
}

std::string dollars(double d) {
    char b[32];
    std::snprintf(b, sizeof b, d >= 100 ? "$%.0f" : "$%.2f", d);
    return b;
}

std::string describeWindow(const char *name, const WindowForecast &w, double now) {
    std::string s = name;
    if (!w.known)
        return s + ": no reading yet. Claude Code reports it to the status line after its first reply.";
    s += ": ";
    s += w.estimated ? "about " + pctWord(w.pctNow) + " used (it reset since the last reading)"
                     : pctWord(w.pctNow) + " used";
    const std::string resets = "it resets in " + formatDuration(w.resetsAt - now);
    if (w.pctNow >= 100) return s + "; " + resets + ".";
    if (w.pctPerHour <= 0) {
        if (w.estimated && !w.fit.ok) return s + "; " + resets + ".";
        return s + ", and nothing is using it now; " + resets + ".";
    }
    if (w.hitAt > 0) {
        s += ". At this rate it reaches 100% in about " + formatDuration(w.hitAt - now);
        if (w.hitEarliest > 0 && w.hitLatest > 0 &&
            formatDuration(w.hitEarliest - now) != formatDuration(w.hitLatest - now))
            s += " (" + formatDuration(w.hitEarliest - now) + " to " +
                 formatDuration(w.hitLatest - now) + ")";
        else if (w.hitEarliest > 0 && w.hitLatest == 0)
            s += " (" + formatDuration(w.hitEarliest - now) + ", or not before it resets)";
        return s + "; " + resets + ".";
    }
    s += ". At this rate it is about " + pctWord(w.pctAtReset) + " when " + resets;
    if (w.hitEarliest > 0) s += " (at the fastest recent rate, 100% in " + formatDuration(w.hitEarliest - now) + ")";
    return s + ".";
}

}  // namespace

Forecast forecast(const std::vector<Sample> &samples, const std::vector<Entry> &entries,
                  double now) {
    Forecast f;
    f.entries = (int)entries.size();
    // Transcripts are read back kHorizon: a window that started before
    // that has no cost to measure from its start.
    const double coveredFrom = now - kHorizon;
    f.five = windowForecast(samples, entries, now, Window::FiveHour, coveredFrom);
    f.week = windowForecast(samples, entries, now, Window::Week, coveredFrom);
    f.costPerHour = costBetween(entries, now - kHour, now);
    for (const Block &b : blocks(entries, now))
        if (b.active) f.costInBlock = b.cost;
    f.active = sessionRates(entries, now, 15 * 60);
    return f;
}

std::string formatDuration(double seconds) {
    if (seconds < 60) return "under a minute";
    const long minutes = (long)std::lround(seconds / 60);
    const long days = minutes / (24 * 60), hours = (minutes / 60) % 24, mins = minutes % 60;
    char b[48];
    if (days > 0) std::snprintf(b, sizeof b, hours ? "%ld d %ld h" : "%ld d", days, hours);
    else if (hours > 0) std::snprintf(b, sizeof b, mins ? "%ld h %ld m" : "%ld h", hours, mins);
    else std::snprintf(b, sizeof b, "%ld m", mins);
    return b;
}

std::vector<std::string> describe(const Forecast &f, double now) {
    std::vector<std::string> out;
    out.push_back(describeWindow("5-hour window", f.five, now));
    out.push_back(describeWindow("Week", f.week, now));
    std::string burn = "Burn rate: ";
    if (f.costPerHour <= 0) {
        burn += "nothing in the last hour";
    } else {
        burn += dollars(f.costPerHour) + " an hour at API prices";
    }
    const size_t n = f.active.size();
    if (n == 1) burn += "; 1 session active in the last 15 minutes";
    else if (n > 1) burn += "; " + std::to_string(n) + " sessions active in the last 15 minutes";
    out.push_back(burn + ".");
    const bool fiveGap = f.five.unseenPct >= 0.5, weekGap = f.week.unseenPct >= 0.5;
    if (fiveGap || weekGap) {
        std::string u = "Unseen usage: ";
        char b[64];
        if (fiveGap) {
            std::snprintf(b, sizeof b, "%.0f points of the 5-hour window", f.five.unseenPct);
            u += b;
        }
        if (fiveGap && weekGap) u += " and ";
        if (weekGap) {
            std::snprintf(b, sizeof b, "%.0f points of the week", f.week.unseenPct);
            u += b;
        }
        u += " that this computer's transcripts do not explain (another computer, claude.ai, or "
             "the cloud).";
        out.push_back(u);
    }
    std::vector<std::string> fits;
    if (f.five.fit.ok)
        fits.push_back("1% of the 5-hour window is about " + dollars(1 / f.five.fit.scale) +
                       " at API prices (" + std::to_string(f.five.fit.readings) +
                       (f.five.fit.readings == 1 ? " reading)" : " readings)"));
    if (f.week.fit.ok)
        fits.push_back("1% of the week about " + dollars(1 / f.week.fit.scale) + " (" +
                       std::to_string(f.week.fit.readings) +
                       (f.week.fit.readings == 1 ? " reading)" : " readings)"));
    if (!fits.empty()) {
        std::string s = "Fit: " + fits[0];
        if (fits.size() > 1) s += "; " + fits[1];
        out.push_back(s + ".");
    }
    return out;
}

}  // namespace ClaudeUsage
