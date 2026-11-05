// CommandPalette.cpp — see CommandPalette.h.
#include "CommandPalette.h"

#include <algorithm>
#include <climits>

namespace CommandPalette {

namespace {

char16_t lower(char16_t c) { return (c >= 'A' && c <= 'Z') ? char16_t(c + 32) : c; }
bool isUpper(char16_t c) { return c >= 'A' && c <= 'Z'; }
bool isLower(char16_t c) { return c >= 'a' && c <= 'z'; }
bool isDigit(char16_t c) { return c >= '0' && c <= '9'; }
bool isWordChar(char16_t c) { return isUpper(c) || isLower(c) || isDigit(c) || c >= 0x80; }

std::u16string lowered(const std::u16string& s) {
    std::u16string out = s;
    for (char16_t& c : out) c = lower(c);
    return out;
}

std::u16string trimmed(const std::u16string& s) {
    size_t a = 0, b = s.size();
    while (a < b && s[a] == ' ') a++;
    while (b > a && s[b - 1] == ' ') b--;
    return s.substr(a, b - a);
}

// Scores of one alignment step.
constexpr int kMatch = 1;        // any matched character
constexpr int kWordStart = 10;   // it begins a word
constexpr int kFirst = 2;        // it is the label's first character
constexpr int kConsecutive = 6;  // it follows the previous match directly
constexpr int kNeg = INT_MIN / 4;

}  // namespace

bool wordStart(const std::u16string& text, size_t i) {
    if (i == 0) return true;
    if (i >= text.size()) return false;
    char16_t c = text[i], p = text[i - 1];
    if (!isWordChar(c)) return false;
    if (!isWordChar(p)) return true;
    return isUpper(c) && isLower(p);
}

bool match(const std::u16string& query, const std::u16string& text, int* score,
           std::vector<size_t>* positions) {
    std::u16string q;
    for (char16_t c : query)
        if (c != ' ') q.push_back(lower(c));
    const size_t n = q.size(), m = text.size();
    if (score) *score = 0;
    if (positions) positions->clear();
    if (n == 0) return true;
    if (n > m) return false;
    const std::u16string t = lowered(text);

    // best[i][j]: the best score of the first i+1 query characters with the
    // last one at text[j]; from[i][j] the previous one's place. A gap costs
    // one point a character, so the best predecessor more than one back is
    // a running maximum of best[i-1][k] + k (minus j-1 at use).
    std::vector<int> best(n * m, kNeg);
    std::vector<int> from(n * m, -1);
    auto bonus = [&](size_t j) {
        int b = kMatch;
        if (wordStart(text, j)) b += kWordStart;
        if (j == 0) b += kFirst;
        return b;
    };
    for (size_t j = 0; j < m; j++)
        if (t[j] == q[0]) best[j] = bonus(j);
    for (size_t i = 1; i < n; i++) {
        int runMax = kNeg, runArg = -1;
        for (size_t j = i; j < m; j++) {
            // k = j-2 joins the gapped predecessors.
            if (j >= 2) {
                int k = int(j) - 2, v = best[(i - 1) * m + k];
                if (v > kNeg && v + k > runMax) { runMax = v + k; runArg = k; }
            }
            if (t[j] != q[i]) continue;
            int b = bonus(j), cell = kNeg, arg = -1;
            int adj = best[(i - 1) * m + j - 1];
            if (adj > kNeg) { cell = adj + kConsecutive + b; arg = int(j) - 1; }
            if (runMax > kNeg) {
                int v = runMax - int(j - 1) + b;
                if (v > cell) { cell = v; arg = runArg; }
            }
            best[i * m + j] = cell;
            from[i * m + j] = arg;
        }
    }
    int top = kNeg, end = -1;
    for (size_t j = 0; j < m; j++) {
        int v = best[(n - 1) * m + j];
        if (v > top) { top = v; end = int(j); }
    }
    if (end < 0) return false;
    if (score) *score = top;
    if (positions) {
        std::vector<size_t> pos(n);
        int j = end;
        for (size_t i = n; i-- > 0;) {
            pos[i] = size_t(j);
            j = from[i * m + size_t(j)];
        }
        *positions = pos;
    }
    return true;
}

Tier tierOf(const std::u16string& text, const std::vector<size_t>& positions) {
    if (positions.empty()) return Tier::Prefix;
    bool runsAtWords = true;
    size_t runs = 0;
    for (size_t i = 0; i < positions.size(); i++) {
        if (i > 0 && positions[i] == positions[i - 1] + 1) continue;
        runs++;
        if (!wordStart(text, positions[i])) runsAtWords = false;
    }
    if (runsAtWords) return Tier::WordStarts;
    return runs == 1 ? Tier::Substring : Tier::Scattered;
}

std::vector<Match> rank(const std::vector<Entry>& entries, const std::u16string& query,
                        const std::vector<std::string>& recent) {
    auto recency = [&](const std::string& id) {
        auto it = std::find(recent.begin(), recent.end(), id);
        return it == recent.end() ? recent.size() : size_t(it - recent.begin());
    };
    const std::u16string q = lowered(trimmed(query));
    std::vector<Match> out;
    std::vector<size_t> rec;
    for (size_t i = 0; i < entries.size(); i++) {
        const Entry& e = entries[i];
        Match mt;
        mt.index = i;
        if (q.empty()) {
            mt.tier = Tier::Prefix;
        } else {
            const std::u16string label = lowered(e.label), title = lowered(e.title);
            // The title starts the label's last part when the label ends with it.
            const size_t titleAt =
                (label.size() >= title.size() &&
                 label.compare(label.size() - title.size(), title.size(), title) == 0)
                    ? label.size() - title.size()
                    : std::u16string::npos;
            if (!title.empty() && titleAt != std::u16string::npos &&
                title.compare(0, q.size(), q) == 0 && q.size() <= title.size()) {
                mt.tier = Tier::Prefix;
                mt.score = 1000 - int(title.size());   // the shorter title first
                for (size_t k = 0; k < q.size(); k++)
                    if (q[k] != ' ') mt.positions.push_back(titleAt + k);
            } else if (label.compare(0, q.size(), q) == 0 && q.size() <= label.size()) {
                mt.tier = Tier::Prefix;
                mt.score = 1000 - int(label.size());
                for (size_t k = 0; k < q.size(); k++)
                    if (q[k] != ' ') mt.positions.push_back(k);
            } else {
                if (!match(q, e.label, &mt.score, &mt.positions)) continue;
                mt.tier = tierOf(e.label, mt.positions);
            }
        }
        out.push_back(mt);
        rec.push_back(recency(e.id));
    }
    std::vector<size_t> order(out.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const Match &x = out[a], &y = out[b];
        if (x.tier != y.tier) return x.tier < y.tier;
        if (rec[a] != rec[b]) return rec[a] < rec[b];
        if (x.score != y.score) return x.score > y.score;
        return x.index < y.index;
    });
    std::vector<Match> sorted;
    sorted.reserve(out.size());
    for (size_t i : order) sorted.push_back(std::move(out[i]));
    return sorted;
}

std::vector<std::string> noteRun(std::vector<std::string> recent, const std::string& id,
                                 size_t cap) {
    recent.erase(std::remove(recent.begin(), recent.end(), id), recent.end());
    recent.insert(recent.begin(), id);
    if (recent.size() > cap) recent.resize(cap);
    return recent;
}

std::u16string keyLabel(const std::u16string& key, unsigned mods) {
    if (key.empty()) return u"";
    char16_t k = key[0];
    if (isUpper(k)) mods |= Shift;
    std::u16string out;
    if (mods & Control) out += u"⌃";
    if (mods & Option) out += u"⌥";
    if (mods & Shift) out += u"⇧";
    if (mods & Command) out += u"⌘";
    // AppKit's function-key characters (NSF1FunctionKey and friends).
    if (k >= 0xF704 && k <= 0xF726) {
        int n = int(k - 0xF704) + 1;
        out += u"F";
        if (n >= 10) out.push_back(char16_t('0' + n / 10));
        out.push_back(char16_t('0' + n % 10));
        return out;
    }
    switch (k) {
        case 0xF700: out += u"↑"; return out;
        case 0xF701: out += u"↓"; return out;
        case 0xF702: out += u"←"; return out;
        case 0xF703: out += u"→"; return out;
        case 0xF728: out += u"⌦"; return out;
        case 0xF729: out += u"↖"; return out;
        case 0xF72B: out += u"↘"; return out;
        case 0xF72C: out += u"⇞"; return out;
        case 0xF72D: out += u"⇟"; return out;
        case '\r': case 3: case '\n': out += u"⏎"; return out;
        case '\t': out += u"⇥"; return out;
        case 0x1B: out += u"⎋"; return out;
        case 0x08: case 0x7F: out += u"⌫"; return out;
        case ' ': out += u"Space"; return out;
        default: break;
    }
    out.push_back(isLower(k) ? char16_t(k - 32) : k);
    return out;
}

}  // namespace CommandPalette
