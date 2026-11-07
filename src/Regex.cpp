// Regex.cpp — see Regex.h.
#include "Regex.h"

#include "TextFind.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace Regex {

namespace {

constexpr char32_t kMaxCodePoint = 0x10FFFF;
// TextFind::foldCase changes nothing at or past this (a core test checks).
constexpr char32_t kFoldEnd = 0x0500;
constexpr std::size_t kMaxProgram = 50000;
constexpr int kMaxRepeat = 1000;
constexpr int kMaxNesting = 200;
constexpr std::size_t kMaxGroups = 1000;

using Ranges = std::vector<std::pair<char32_t, char32_t>>;

void normalize(Ranges& r) {
    std::sort(r.begin(), r.end());
    Ranges out;
    for (const auto& x : r) {
        if (!out.empty() && x.first <= out.back().second + 1)
            out.back().second = std::max(out.back().second, x.second);
        else
            out.push_back(x);
    }
    r.swap(out);
}

Ranges complement(const Ranges& r) {   // r normalized
    Ranges out;
    char32_t next = 0;
    for (const auto& x : r) {
        if (x.first > next) out.push_back({next, x.first - 1});
        next = x.second + 1;
    }
    if (next <= kMaxCodePoint) out.push_back({next, kMaxCodePoint});
    return out;
}

const Ranges& digits() {
    static const Ranges r{{'0', '9'}};
    return r;
}
const Ranges& wordChars() {
    static const Ranges r{{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}};
    return r;
}
// JavaScript's WhiteSpace and LineTerminator.
const Ranges& spaces() {
    static const Ranges r{{0x09, 0x0D},     {0x20, 0x20},     {0xA0, 0xA0},
                          {0x1680, 0x1680}, {0x2000, 0x200A}, {0x2028, 0x2029},
                          {0x202F, 0x202F}, {0x205F, 0x205F}, {0x3000, 0x3000},
                          {0xFEFF, 0xFEFF}};
    return r;
}

bool isAsciiWord(char32_t c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '_';
}

struct CharSet {
    std::uint64_t ascii[2] = {0, 0};
    Ranges ranges;   // normalized; the ASCII part too

    explicit CharSet(Ranges r = {}) : ranges(std::move(r)) {
        for (const auto& x : ranges)
            for (char32_t c = x.first; c <= x.second && c < 128; ++c)
                ascii[c >> 6] |= std::uint64_t(1) << (c & 63);
    }
    bool contains(char32_t c) const {
        if (c < 128) return (ascii[c >> 6] >> (c & 63)) & 1;
        auto it = std::upper_bound(ranges.begin(), ranges.end(), c,
                                   [](char32_t v, const std::pair<char32_t, char32_t>& x) {
                                       return v < x.first;
                                   });
        if (it == ranges.begin()) return false;
        --it;
        return c <= it->second;
    }
};

// ------------------------------------------------------------------ the tree
struct Node {
    enum Kind { Empty, Lit, Set, Bol, Eol, WordB, NotWordB, Group, Concat, Alt, Repeat };
    Kind kind = Empty;
    char32_t c = 0;        // Lit
    int set = -1;          // Set
    int group = -1;        // Group: the capture's number, or -1 for (?:...)
    int min = 0, max = 0;  // Repeat; max -1 is no limit
    bool greedy = true;
    // Repeat: the capturing groups inside, [firstGroup, endGroup).
    int firstGroup = 0, endGroup = 0;
    std::vector<int> kids;
};

class Parser {
public:
    Parser(const std::u32string& p, bool icase) : p_(p), icase_(icase) {}

    std::vector<Node> nodes;
    std::vector<Ranges> sets;
    int groups = 0;
    std::string why;

    int parse() {
        const int root = alternation(0);
        if (root < 0) return -1;
        if (i_ != p_.size()) return fail("an unmatched )");
        return root;
    }

private:
    const std::u32string& p_;
    bool icase_;
    std::size_t i_ = 0;

    int fail(const char* w) {
        if (why.empty()) why = w;
        return -1;
    }
    bool more() const { return i_ < p_.size(); }
    char32_t peek(std::size_t k = 0) const { return i_ + k < p_.size() ? p_[i_ + k] : 0; }

    int add(Node n) {
        nodes.push_back(std::move(n));
        return static_cast<int>(nodes.size()) - 1;
    }
    int lit(char32_t c) {
        Node n;
        n.kind = Node::Lit;
        n.c = icase_ ? TextFind::foldCase(c) : c;
        return add(n);
    }
    // A set of characters; with icase, every character that folds from
    // one in it too (the text is folded, so only folded ones meet it).
    int setNode(Ranges r, bool negate) {
        normalize(r);
        if (icase_) {
            Ranges extra;
            for (const auto& x : r)
                for (char32_t c = x.first; c <= x.second && c < kFoldEnd; ++c) {
                    const char32_t f = TextFind::foldCase(c);
                    if (f != c) extra.push_back({f, f});
                }
            r.insert(r.end(), extra.begin(), extra.end());
            normalize(r);
        }
        if (negate) r = complement(r);
        sets.push_back(std::move(r));
        Node n;
        n.kind = Node::Set;
        n.set = static_cast<int>(sets.size()) - 1;
        return add(n);
    }

    int alternation(int depth) {
        if (depth > kMaxNesting) return fail("groups nested too deep");
        std::vector<int> alts;
        int a = sequence(depth);
        if (a < 0) return -1;
        alts.push_back(a);
        while (more() && peek() == '|') {
            ++i_;
            a = sequence(depth);
            if (a < 0) return -1;
            alts.push_back(a);
        }
        if (alts.size() == 1) return alts[0];
        Node n;
        n.kind = Node::Alt;
        n.kids = std::move(alts);
        return add(n);
    }

    int sequence(int depth) {
        std::vector<int> items;
        while (more() && peek() != '|' && peek() != ')') {
            const int t = term(depth);
            if (t < 0) return -1;
            items.push_back(t);
        }
        if (items.size() == 1) return items[0];
        Node n;
        n.kind = items.empty() ? Node::Empty : Node::Concat;
        n.kids = std::move(items);
        return add(n);
    }

    static bool isDigit(char32_t c) { return c >= '0' && c <= '9'; }

    int term(int depth) {
        const char32_t c = peek();
        Node::Kind assertion = Node::Empty;
        if (c == '^') assertion = Node::Bol;
        else if (c == '$') assertion = Node::Eol;
        else if (c == '\\' && peek(1) == 'b') assertion = Node::WordB;
        else if (c == '\\' && peek(1) == 'B') assertion = Node::NotWordB;
        if (assertion != Node::Empty) {
            i_ += c == '\\' ? 2 : 1;
            const char32_t q = peek();
            if (q == '*' || q == '+' || q == '?' || q == '{')
                return fail("a repeated assertion");
            Node n;
            n.kind = assertion;
            return add(n);
        }
        const int before = groups;
        const int a = atom(depth);
        if (a < 0) return -1;
        int min = 0, max = 0;
        const char32_t q = peek();
        if (q == '*') { min = 0; max = -1; ++i_; }
        else if (q == '+') { min = 1; max = -1; ++i_; }
        else if (q == '?') { min = 0; max = 1; ++i_; }
        else if (q == '{') {
            std::size_t j = i_ + 1;
            auto number = [&](int* out) {
                const std::size_t start = j;
                long v = 0;
                while (j < p_.size() && isDigit(p_[j])) {
                    v = v * 10 + static_cast<long>(p_[j] - '0');
                    if (v > kMaxRepeat) v = kMaxRepeat + 1;
                    ++j;
                }
                *out = static_cast<int>(v);
                return j > start;
            };
            if (!number(&min)) return fail("a { that is not a count");
            max = min;
            if (j < p_.size() && p_[j] == ',') {
                ++j;
                if (j < p_.size() && p_[j] == '}') max = -1;
                else if (!number(&max)) return fail("a { that is not a count");
            }
            if (j >= p_.size() || p_[j] != '}') return fail("a { that is not a count");
            if (min > kMaxRepeat || max > kMaxRepeat) return fail("a count over 1,000");
            if (max >= 0 && max < min) return fail("a count out of order");
            i_ = j + 1;
        } else {
            return a;
        }
        bool greedy = true;
        if (peek() == '?') {
            greedy = false;
            ++i_;
        }
        const char32_t after = peek();
        if (after == '*' || after == '+' || after == '?' || after == '{')
            return fail("a repeat of a repeat");
        Node n;
        n.kind = Node::Repeat;
        n.min = min;
        n.max = max;
        n.greedy = greedy;
        n.kids = {a};
        n.firstGroup = before + 1;
        n.endGroup = groups + 1;
        return add(n);
    }

    int atom(int depth) {
        const char32_t c = peek();
        switch (c) {
        case '.': {
            ++i_;
            return setNode({{'\n', '\n'}, {'\r', '\r'}, {0x2028, 0x2029}}, true);
        }
        case '(': {
            int group = -1;
            if (peek(1) == '?') {
                if (peek(2) != ':') return fail("lookaround or a named group");
                i_ += 3;
            } else {
                ++i_;
                if (static_cast<std::size_t>(groups) >= kMaxGroups) return fail("too many groups");
                group = ++groups;
            }
            const int kid = alternation(depth + 1);
            if (kid < 0) return -1;
            if (peek() != ')') return fail("an unclosed (");
            ++i_;
            Node n;
            n.kind = Node::Group;
            n.group = group;
            n.kids = {kid};
            return add(n);
        }
        case '[':
            return charClass();
        case '\\': {
            char32_t ch = 0;
            Ranges set;
            const int k = escape(false, &ch, &set);
            if (k < 0) return -1;
            return k == 1 ? lit(ch) : setNode(set, false);
        }
        case '*': case '+': case '?': case '{':
            return fail("nothing to repeat");
        case ']': case '}': case ')':
            return fail("a lone ], } or )");
        default:
            ++i_;
            return lit(c);
        }
    }

    static int hex(char32_t c) {
        if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<int>(c - 'a') + 10;
        if (c >= 'A' && c <= 'F') return static_cast<int>(c - 'A') + 10;
        return -1;
    }

    // After a backslash: 1 with a character in *ch, 2 with a set in *set,
    // -1 when this engine leaves it to std::regex.
    int escape(bool inClass, char32_t* ch, Ranges* set) {
        if (i_ + 1 >= p_.size()) return fail("a trailing \\");
        const char32_t d = p_[i_ + 1];
        i_ += 2;
        switch (d) {
        case 'd': *set = digits(); return 2;
        case 'D': *set = complement(digits()); return 2;
        case 'w': *set = wordChars(); return 2;
        case 'W': *set = complement(wordChars()); return 2;
        case 's': *set = spaces(); return 2;
        case 'S': *set = complement(spaces()); return 2;
        case 't': *ch = 0x09; return 1;
        case 'n': *ch = 0x0A; return 1;
        case 'v': *ch = 0x0B; return 1;
        case 'f': *ch = 0x0C; return 1;
        case 'r': *ch = 0x0D; return 1;
        case 'b':
            if (inClass) { *ch = 0x08; return 1; }
            return fail("\\b");
        case '0':
            if (isDigit(peek())) return fail("an octal escape");
            *ch = 0;
            return 1;
        case 'c':   // libc++ and libstdc++ read \cX differently: theirs to decide
            return fail("\\c");
        case 'x': {
            const int a = hex(peek()), b = hex(peek(1));
            if (a < 0 || b < 0) return fail("a bad \\x");
            i_ += 2;
            *ch = static_cast<char32_t>(a * 16 + b);
            return 1;
        }
        case 'u': {
            int v = 0;
            for (std::size_t k = 0; k < 4; ++k) {
                const int h = hex(peek(k));
                if (h < 0) return fail("a bad \\u");
                v = v * 16 + h;
            }
            i_ += 4;
            *ch = static_cast<char32_t>(v);
            return 1;
        }
        case '-':
            *ch = '-';
            return 1;
        default:
            if (isDigit(d)) return fail("a backreference");
            if ((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z'))
                return fail("an escape this engine does not know");
            *ch = d;   // \. \* \\ \/ and the like: the character itself
            return 1;
        }
    }

    int charClass() {
        ++i_;   // [
        bool negate = false;
        if (peek() == '^') {
            negate = true;
            ++i_;
        }
        if (peek() == ']') return fail("[] or []...]");
        Ranges r;
        // One item: 1 a character, 2 a set, -1 refused.
        auto item = [&](char32_t* ch, Ranges* set) -> int {
            if (!more()) return fail("an unclosed [");
            const char32_t c = peek();
            if (c == '[') return fail("[ inside a class");
            if (c == '\\') {
                if (peek(1) == 'B') return fail("\\B in a class");
                return escape(true, ch, set);
            }
            ++i_;
            *ch = c;
            return 1;
        };
        while (true) {
            if (!more()) return fail("an unclosed [");
            if (peek() == ']') {
                ++i_;
                break;
            }
            char32_t a = 0;
            Ranges as;
            const int k = item(&a, &as);
            if (k < 0) return -1;
            const bool range = peek() == '-' && i_ + 1 < p_.size() && p_[i_ + 1] != ']';
            if (k == 2) {
                if (range) return fail("a class escape in a range");
                r.insert(r.end(), as.begin(), as.end());
                continue;
            }
            if (!range) {
                r.push_back({a, a});
                continue;
            }
            ++i_;   // -
            char32_t b = 0;
            Ranges bs;
            const int kb = item(&b, &bs);
            if (kb < 0) return -1;
            if (kb == 2) return fail("a class escape in a range");
            if (b < a) return fail("a range out of order");
            r.push_back({a, b});
        }
        return setNode(std::move(r), negate);
    }
};

// --------------------------------------------------------------- the program
enum Op : std::uint8_t {
    kChar,    // x: the character
    kSet,     // x: the set
    kMatch,
    kSplit,   // try x, then y
    kJmp,     // x
    kSave,    // x: the slot
    kReset,   // slots [x, y) unset
    kEnter,   // a repetition past its minimum starts
    kCheck,   // ...and must have taken a character
    kBol,
    kEol,
    kWordB,
    kNotWordB,
};

struct Inst {
    Op op;
    std::uint32_t x = 0, y = 0;
};

}  // namespace

class Program {
public:
    std::vector<Inst> code;
    std::vector<CharSet> sets;
    std::size_t groups = 0;
    std::u32string prefix;     // every match starts with it
    std::u32string required;   // every match holds it
    bool anchoredStart = false;
    bool literal = false;      // the pattern is the prefix and nothing else
    bool hasFirst = false;     // every match starts with a character in first
    CharSet first;
};

namespace {

class Compiler {
public:
    Compiler(const std::vector<Node>& nodes, Program& prog) : nodes_(nodes), prog_(prog) {}

    // The size of a node's code, saturated past the limit.
    std::size_t size(int id) {
        const Node& n = nodes_[static_cast<std::size_t>(id)];
        auto sat = [](std::size_t a) { return std::min(a, kMaxProgram + 1); };
        switch (n.kind) {
        case Node::Empty: return 0;
        case Node::Lit: case Node::Set: case Node::Bol: case Node::Eol: case Node::WordB:
        case Node::NotWordB: return 1;
        case Node::Group: return sat(size(n.kids[0]) + (n.group > 0 ? 2 : 0));
        case Node::Concat: {
            std::size_t s = 0;
            for (int k : n.kids) s = sat(s + size(k));
            return s;
        }
        case Node::Alt: {
            std::size_t s = 0;
            for (int k : n.kids) s = sat(s + size(k) + 2);
            return s;
        }
        case Node::Repeat: {
            const std::size_t body = size(n.kids[0]) + 4;
            const std::size_t copies = n.max < 0 ? static_cast<std::size_t>(n.min) + 1
                                                 : static_cast<std::size_t>(n.max);
            if (copies && body > (kMaxProgram + 1) / copies) return kMaxProgram + 1;
            return sat(body * copies + 2);
        }
        }
        return 0;
    }

    bool canBeEmpty(int id) const {
        const Node& n = nodes_[static_cast<std::size_t>(id)];
        switch (n.kind) {
        case Node::Lit: case Node::Set: return false;
        case Node::Group: return canBeEmpty(n.kids[0]);
        case Node::Concat:
            for (int k : n.kids)
                if (!canBeEmpty(k)) return false;
            return true;
        case Node::Alt:
            for (int k : n.kids)
                if (canBeEmpty(k)) return true;
            return false;
        case Node::Repeat: return n.min == 0 || canBeEmpty(n.kids[0]);
        default: return true;
        }
    }

    std::uint32_t pc() const { return static_cast<std::uint32_t>(prog_.code.size()); }
    std::uint32_t emit(Op op, std::uint32_t x = 0, std::uint32_t y = 0) {
        prog_.code.push_back(Inst{op, x, y});
        return pc() - 1;
    }

    void gen(int id) {
        const Node& n = nodes_[static_cast<std::size_t>(id)];
        switch (n.kind) {
        case Node::Empty: return;
        case Node::Lit: emit(kChar, n.c); return;
        case Node::Set: emit(kSet, static_cast<std::uint32_t>(n.set)); return;
        case Node::Bol: emit(kBol); return;
        case Node::Eol: emit(kEol); return;
        case Node::WordB: emit(kWordB); return;
        case Node::NotWordB: emit(kNotWordB); return;
        case Node::Group:
            if (n.group > 0) emit(kSave, static_cast<std::uint32_t>(2 * n.group));
            gen(n.kids[0]);
            if (n.group > 0) emit(kSave, static_cast<std::uint32_t>(2 * n.group + 1));
            return;
        case Node::Concat:
            for (int k : n.kids) gen(k);
            return;
        case Node::Alt: {
            std::vector<std::uint32_t> jumps;
            for (std::size_t k = 0; k < n.kids.size(); ++k) {
                if (k + 1 < n.kids.size()) {
                    const std::uint32_t split = emit(kSplit);
                    prog_.code[split].x = pc();
                    gen(n.kids[k]);
                    jumps.push_back(emit(kJmp));
                    prog_.code[split].y = pc();
                } else {
                    gen(n.kids[k]);
                }
            }
            for (std::uint32_t j : jumps) prog_.code[j].x = pc();
            return;
        }
        case Node::Repeat: {
            const int body = n.kids[0];
            // Each repetition starts with the groups inside unset.
            const bool reset = n.endGroup > n.firstGroup;
            const std::uint32_t lo = static_cast<std::uint32_t>(2 * n.firstGroup);
            const std::uint32_t hi = static_cast<std::uint32_t>(2 * n.endGroup);
            const bool check = canBeEmpty(body);
            for (int k = 0; k < n.min; ++k) {
                if (reset) emit(kReset, lo, hi);
                gen(body);
            }
            auto optional = [&]() {
                if (reset) emit(kReset, lo, hi);
                if (check) emit(kEnter);
                gen(body);
                if (check) emit(kCheck);
            };
            if (n.max < 0) {
                const std::uint32_t loop = emit(kSplit);
                const std::uint32_t start = pc();
                optional();
                emit(kJmp, loop);
                const std::uint32_t exit = pc();
                prog_.code[loop].x = n.greedy ? start : exit;
                prog_.code[loop].y = n.greedy ? exit : start;
                return;
            }
            std::vector<std::uint32_t> splits;
            for (int k = n.min; k < n.max; ++k) {
                splits.push_back(emit(kSplit));
                optional();
            }
            const std::uint32_t exit = pc();
            for (std::uint32_t s : splits) {
                prog_.code[s].x = n.greedy ? s + 1 : exit;
                prog_.code[s].y = n.greedy ? exit : s + 1;
            }
            return;
        }
        }
    }

private:
    const std::vector<Node>& nodes_;
    Program& prog_;
};

// The nodes a match passes through in order, groups opened up.
void flatten(const std::vector<Node>& nodes, int id, std::vector<int>& out) {
    const Node& n = nodes[static_cast<std::size_t>(id)];
    if (n.kind == Node::Concat) {
        for (int k : n.kids) flatten(nodes, k, out);
    } else if (n.kind == Node::Group) {
        flatten(nodes, n.kids[0], out);
    } else if (n.kind != Node::Empty) {
        out.push_back(id);
    }
}

void findLiterals(const std::vector<Node>& nodes, int root, Program& prog) {
    std::vector<int> items;
    flatten(nodes, root, items);
    std::u32string run;
    bool atStart = true;
    bool allLiterals = !items.empty() && prog.groups == 0;
    auto endRun = [&] {
        if (atStart) prog.prefix = run;
        if (run.size() > prog.required.size()) prog.required = run;
        run.clear();
        atStart = false;
    };
    for (std::size_t k = 0; k < items.size(); ++k) {
        const Node& n = nodes[static_cast<std::size_t>(items[k])];
        if (n.kind == Node::Lit) {
            run.push_back(n.c);
        } else if (n.kind == Node::Bol || n.kind == Node::Eol || n.kind == Node::WordB ||
                   n.kind == Node::NotWordB) {
            // Takes no character: the literals either side stay together.
            if (n.kind == Node::Bol && k == 0) prog.anchoredStart = true;
        } else {
            endRun();
        }
        if (n.kind != Node::Lit) allLiterals = false;
    }
    endRun();
    prog.literal = allLiterals;
}

// The characters a match can start with, unless it can be empty.
void findFirst(Program& prog) {
    const std::size_t n = prog.code.size();
    std::vector<char> seen(n, 0);
    std::vector<std::uint32_t> stack{0};
    Ranges r;
    while (!stack.empty()) {
        const std::uint32_t pc = stack.back();
        stack.pop_back();
        if (seen[pc]) continue;
        seen[pc] = 1;
        const Inst& in = prog.code[pc];
        switch (in.op) {
        case kMatch: return;   // an empty match: anything goes
        case kChar: r.push_back({in.x, in.x}); break;
        case kSet: {
            const Ranges& s = prog.sets[in.x].ranges;
            r.insert(r.end(), s.begin(), s.end());
            break;
        }
        case kSplit:
            stack.push_back(in.y);
            stack.push_back(in.x);
            break;
        case kJmp: stack.push_back(in.x); break;
        default: stack.push_back(pc + 1); break;
        }
    }
    normalize(r);
    prog.first = CharSet(std::move(r));
    prog.hasFirst = true;
}

// --------------------------------------------------------------- the search
struct List {
    std::vector<std::uint32_t> mark;   // a state's stamp when it is in the list
    std::uint32_t stamp = 0;
    std::vector<std::uint32_t> pcs;
    std::vector<std::size_t> caps;   // slots per thread, in the order of pcs

    void clear(std::size_t states) {
        if (mark.size() < states) mark.assign(states, 0), stamp = 0;
        if (++stamp == 0) {
            std::fill(mark.begin(), mark.end(), 0);
            stamp = 1;
        }
        pcs.clear();
        caps.clear();
    }
    bool empty() const { return pcs.empty(); }
};

struct Frame {
    std::uint32_t pc;
    std::uint32_t e;          // nothing taken since the last kEnter
    std::size_t slot;         // a restore when not npos
    std::size_t value;
};

struct Scratch {
    List a, b;
    std::vector<Frame> stack;
    std::vector<std::size_t> work, best, init;
};

Scratch& scratch() {
    static thread_local Scratch s;
    return s;
}

class Vm {
public:
    Vm(const Program& p, const char32_t* s, std::size_t n, Scratch& sc)
        : p_(p), s_(s), n_(n), slots_(2 * (p.groups + 1)), states_(2 * p.code.size()), sc_(sc) {}

    bool word(std::size_t i) const { return i < n_ && isAsciiWord(s_[i]); }

    // Every thread reachable from pc at pos without taking a character,
    // added to `list` in priority order, starting from the slots in `from`.
    void add(List& list, std::uint32_t pc0, std::uint32_t e0, std::size_t pos,
             const std::size_t* from) {
        std::vector<std::size_t>& w = sc_.work;
        w.assign(from, from + slots_);
        std::vector<Frame>& st = sc_.stack;
        st.clear();
        st.push_back({pc0, e0, npos, 0});
        while (!st.empty()) {
            const Frame f = st.back();
            st.pop_back();
            if (f.slot != npos) {
                w[f.slot] = f.value;
                continue;
            }
            const std::size_t state = 2 * static_cast<std::size_t>(f.pc) + f.e;
            if (list.mark[state] == list.stamp) continue;
            list.mark[state] = list.stamp;
            const Inst& in = p_.code[f.pc];
            switch (in.op) {
            case kChar: case kSet: case kMatch:
                list.pcs.push_back(f.pc);
                list.caps.insert(list.caps.end(), w.begin(), w.end());
                break;
            case kSplit:
                st.push_back({in.y, f.e, npos, 0});
                st.push_back({in.x, f.e, npos, 0});
                break;
            case kJmp:
                st.push_back({in.x, f.e, npos, 0});
                break;
            case kSave:
                st.push_back({0, 0, in.x, w[in.x]});
                w[in.x] = pos;
                st.push_back({f.pc + 1, f.e, npos, 0});
                break;
            case kReset:
                for (std::uint32_t k = in.x; k < in.y && k < slots_; ++k) {
                    if (w[k] == npos) continue;
                    st.push_back({0, 0, k, w[k]});
                    w[k] = npos;
                }
                st.push_back({f.pc + 1, f.e, npos, 0});
                break;
            case kEnter:
                st.push_back({f.pc + 1, 1, npos, 0});
                break;
            case kCheck:
                if (!f.e) st.push_back({f.pc + 1, 0, npos, 0});
                break;
            case kBol:
                if (pos == 0) st.push_back({f.pc + 1, f.e, npos, 0});
                break;
            case kEol:
                if (pos == n_) st.push_back({f.pc + 1, f.e, npos, 0});
                break;
            case kWordB:
            case kNotWordB: {
                const bool at = (pos > 0 && word(pos - 1)) != word(pos);
                if (at == (in.op == kWordB)) st.push_back({f.pc + 1, f.e, npos, 0});
                break;
            }
            }
        }
    }

    // Where the next match could start, at or after pos; past n when none.
    std::size_t nextStart(std::size_t pos) const {
        if (!p_.prefix.empty()) {
            const std::u32string& pre = p_.prefix;
            const std::size_t m = pre.size();
            for (std::size_t i = pos; i + m <= n_; ++i) {
                if (s_[i] != pre[0]) continue;
                std::size_t k = 1;
                while (k < m && s_[i + k] == pre[k]) ++k;
                if (k == m) return i;
            }
            return n_ + 1;
        }
        if (p_.hasFirst) {
            for (std::size_t i = pos; i < n_; ++i)
                if (p_.first.contains(s_[i])) return i;
            return n_ + 1;
        }
        return pos;
    }

    bool holds(const std::u32string& lit, std::size_t from) const {
        const std::size_t m = lit.size();
        for (std::size_t i = from; i + m <= n_; ++i) {
            if (s_[i] != lit[0]) continue;
            std::size_t k = 1;
            while (k < m && s_[i + k] == lit[k]) ++k;
            if (k == m) return true;
        }
        return false;
    }

    // A pattern of plain characters: no program to run.
    Result literal(std::size_t from, bool anchored, std::vector<std::size_t>& caps) const {
        const std::u32string& lit = p_.prefix;
        const std::size_t m = lit.size();
        std::size_t at = from;
        if (anchored) {
            if (from + m > n_ || !std::equal(lit.begin(), lit.end(), s_ + from))
                return Result::NotFound;
        } else {
            at = nextStart(from);
            if (at > n_) return Result::NotFound;
        }
        caps.assign({at, at + m});
        return Result::Found;
    }

    Result run(std::size_t from, const SearchOptions& opt, std::vector<std::size_t>& caps,
               const std::atomic<bool>* cancel) {
        bool anchored = opt.anchored;
        if (p_.anchoredStart) {
            if (from != 0) return Result::NotFound;
            anchored = true;
        }
        if (p_.literal) return literal(from, anchored, caps);
        if (!p_.required.empty() && !holds(p_.required, from)) return Result::NotFound;
        List* cl = &sc_.a;
        List* nl = &sc_.b;
        cl->clear(states_);
        std::vector<std::size_t>& init = sc_.init;
        init.assign(slots_, npos);
        bool matched = false;
        std::size_t pos = from, steps = 0;
        while (true) {
            if (!matched && (pos == from || !anchored)) {
                // With no thread running, skip to where a match could start.
                if (cl->empty() && !anchored) {
                    const std::size_t next = nextStart(pos);
                    if (next > n_) break;
                    if (next != pos) {
                        pos = next;
                        cl->clear(states_);
                    }
                }
                add(*cl, 0, 0, pos, init.data());   // the lowest priority
            }
            if (cl->empty()) {
                if (matched || anchored || pos >= n_) break;
                ++pos;
                cl->clear(states_);
                continue;
            }
            nl->clear(states_);
            const std::size_t threads = cl->pcs.size();
            for (std::size_t t = 0; t < threads; ++t) {
                const Inst& in = p_.code[cl->pcs[t]];
                const std::size_t* tc = cl->caps.data() + t * slots_;
                if (in.op == kMatch) {
                    if (opt.notEmpty && tc[0] == pos) continue;
                    sc_.best.assign(tc, tc + slots_);
                    sc_.best[1] = pos;
                    matched = true;
                    break;   // threads after this one have lower priority
                }
                if (pos >= n_) continue;
                const char32_t c = s_[pos];
                const bool takes = in.op == kChar ? c == in.x : p_.sets[in.x].contains(c);
                if (takes) add(*nl, cl->pcs[t] + 1, 0, pos + 1, tc);
            }
            if (pos >= n_) break;
            std::swap(cl, nl);
            ++pos;
            if (cancel && (++steps & 4095) == 0 && cancel->load(std::memory_order_relaxed))
                return Result::Stopped;
            if (matched && cl->empty()) break;
        }
        if (!matched) return Result::NotFound;
        caps = sc_.best;
        return Result::Found;
    }

private:
    const Program& p_;
    const char32_t* s_;
    std::size_t n_;
    std::size_t slots_;
    std::size_t states_;
    Scratch& sc_;
};

}  // namespace

std::shared_ptr<const Program> compile(const std::u32string& pattern, bool ignoreCase,
                                       std::string* why) {
    Parser parser(pattern, ignoreCase);
    const int root = parser.parse();
    if (root < 0) {
        if (why) *why = parser.why;
        return nullptr;
    }
    auto prog = std::make_shared<Program>();
    Compiler c(parser.nodes, *prog);
    if (c.size(root) + 2 > kMaxProgram) {
        if (why) *why = "a program over 50,000 steps";
        return nullptr;
    }
    prog->groups = static_cast<std::size_t>(parser.groups);
    for (Ranges& r : parser.sets) prog->sets.emplace_back(std::move(r));
    c.emit(kSave, 0);
    c.gen(root);
    c.emit(kMatch);
    findLiterals(parser.nodes, root, *prog);
    findFirst(*prog);
    return prog;
}

std::size_t groupCount(const Program& program) { return program.groups; }

std::size_t programSize(const Program& program) { return program.code.size(); }

const std::u32string& requiredText(const Program& program) { return program.required; }

Result search(const Program& program, const char32_t* s, std::size_t n, std::size_t from,
              const SearchOptions& options, std::vector<std::size_t>& caps,
              const std::atomic<bool>* cancel) {
    if (from > n) return Result::NotFound;
    Vm vm(program, s, n, scratch());
    return vm.run(from, options, caps, cancel);
}

}  // namespace Regex
