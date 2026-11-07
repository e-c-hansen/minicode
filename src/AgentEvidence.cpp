// AgentEvidence.cpp — see AgentEvidence.h.
#include "AgentEvidence.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace AgentEvidence {

using AgentTranscript::Block;
using AgentTranscript::Line;

const char *testToolName(TestTool t) {
    switch (t) {
    case TestTool::None: return "";
    case TestTool::Pytest: return "pytest";
    case TestTool::Make: return "make";
    case TestTool::Ctest: return "ctest";
    case TestTool::Npm: return "npm";
    case TestTool::Cargo: return "cargo";
    case TestTool::Go: return "go";
    case TestTool::Unittest: return "unittest";
    case TestTool::Tox: return "tox";
    case TestTool::Nox: return "nox";
    case TestTool::Jest: return "jest";
    case TestTool::Vitest: return "vitest";
    }
    return "";
}

// ============================================================ recognising
namespace {

using Tokens = std::vector<std::string>;

// Splits a shell command into simple commands (at ; & | && || newlines,
// parentheses and backquotes outside quotes), each a list of words with
// the quotes taken off. Not a shell parser: enough to find what runs.
std::vector<Tokens> simpleCommands(const std::string &cmd) {
    std::vector<Tokens> out(1);
    std::string word;
    bool inWord = false;
    char quote = 0;
    auto endWord = [&]() {
        if (inWord) out.back().push_back(word);
        word.clear();
        inWord = false;
    };
    auto endCommand = [&]() {
        endWord();
        if (!out.back().empty()) out.emplace_back();
    };
    for (size_t i = 0; i < cmd.size(); i++) {
        char c = cmd[i];
        if (quote) {
            if (c == quote) quote = 0;
            else if (c == '\\' && quote == '"' && i + 1 < cmd.size()) word += cmd[++i];
            else word += c;
            continue;
        }
        switch (c) {
        case '\'': case '"': quote = c; inWord = true; break;
        case '\\':
            if (i + 1 < cmd.size()) {
                if (cmd[i + 1] == '\n') { i++; break; }
                word += cmd[++i];
                inWord = true;
            }
            break;
        case ' ': case '\t': endWord(); break;
        case ';': case '&': case '|': case '\n': case '(': case ')': case '`': case '{': case '}':
            endCommand();
            break;
        default: word += c; inWord = true; break;
        }
    }
    endWord();
    if (out.back().empty()) out.pop_back();
    return out;
}

std::string base(const std::string &w) {
    size_t s = w.rfind('/');
    return s == std::string::npos ? w : w.substr(s + 1);
}

bool isAssignment(const std::string &w) {
    size_t eq = w.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    for (size_t i = 0; i < eq; i++) {
        char c = w[i];
        if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (i > 0 && c >= '0' && c <= '9')))
            return false;
    }
    return true;
}

bool isNumber(const std::string &w) {
    if (w.empty()) return false;
    for (char c : w)
        if (!((c >= '0' && c <= '9') || c == '.' || c == 's' || c == 'm' || c == 'h')) return false;
    return w[0] >= '0' && w[0] <= '9';
}

TestTool match(const Tokens &t, size_t i, int depth);

// The words that run something, past assignments and wrappers.
size_t commandStart(const Tokens &t, size_t i) {
    for (;;) {
        while (i < t.size() && isAssignment(t[i])) i++;
        if (i >= t.size()) return i;
        const std::string w = base(t[i]);
        if (w == "env" || w == "time" || w == "command" || w == "exec" || w == "nice" ||
            w == "sudo" || w == "nohup" || w == "caffeinate" || w == "stdbuf" || w == "xvfb-run") {
            i++;
            while (i < t.size() && (t[i][0] == '-' || isNumber(t[i]))) i++;
            continue;
        }
        if (w == "timeout" || w == "gtimeout") {
            i++;
            while (i < t.size() && t[i][0] == '-') i++;
            if (i < t.size()) i++;   // the duration
            continue;
        }
        return i;
    }
}

size_t nextWord(const Tokens &t, size_t i) {
    while (i < t.size() && !t[i].empty() && (t[i][0] == '-' || t[i][0] == '+')) i++;
    return i;
}

TestTool match(const Tokens &t, size_t i, int depth) {
    i = commandStart(t, i);
    if (i >= t.size() || depth > 3) return TestTool::None;
    const std::string w = base(t[i]);
    if (w == "pytest" || w == "py.test") return TestTool::Pytest;
    if (w == "tox") return TestTool::Tox;
    if (w == "nox") return TestTool::Nox;
    if (w == "jest") return TestTool::Jest;
    if (w == "vitest") return TestTool::Vitest;
    if (w.compare(0, 6, "python") == 0) {
        for (size_t j = i + 1; j + 1 < t.size(); j++) {
            if (t[j] == "-m") {
                const std::string &m = t[j + 1];
                if (m == "pytest") return TestTool::Pytest;
                if (m == "unittest") return TestTool::Unittest;
                if (m == "tox") return TestTool::Tox;
                if (m == "nox") return TestTool::Nox;
                return TestTool::None;
            }
            if (t[j][0] != '-') break;
        }
        return TestTool::None;
    }
    // A runner fetched or found by a package tool: `uvx tox`, `npx jest`,
    // `pnpm exec vitest`, `pipx run nox`, `bunx vitest`.
    if (w == "uvx" || w == "npx" || w == "bunx" || w == "pnpx") {
        size_t r = nextWord(t, i + 1);
        return r < t.size() ? match(t, r, depth + 1) : TestTool::None;
    }
    if (w == "uv" || w == "poetry" || w == "pdm" || w == "pipenv" || w == "hatch" || w == "rye" ||
        w == "pipx") {
        size_t r = nextWord(t, i + 1);
        if (r >= t.size() || t[r] != "run") return TestTool::None;
        for (size_t j = r + 1; j < t.size(); j++) {
            TestTool m = match(t, j, depth + 1);
            if (m != TestTool::None) return m;
        }
        return TestTool::None;
    }
    if (w == "make" || w == "gmake") {
        for (size_t j = i + 1; j < t.size(); j++) {
            const std::string &a = t[j];
            if (a == "-C" || a == "-f" || a == "-I" || a == "-o" || a == "-W" ||
                a == "--directory" || a == "--file") {
                j++;
                continue;
            }
            if ((a == "-j" || a == "-l") && j + 1 < t.size() && isNumber(t[j + 1])) {
                j++;
                continue;
            }
            if (a[0] == '-' || isAssignment(a)) continue;
            if (a == "test" || a == "check" || a == "tests") return TestTool::Make;
            if (a[0] == '>' || a[0] == '<' || (a.size() > 1 && a[1] == '>')) break;   // redirections
        }
        return TestTool::None;
    }
    if (w == "ctest") return TestTool::Ctest;
    if (w == "npm" || w == "pnpm" || w == "yarn" || w == "bun") {
        size_t a = nextWord(t, i + 1);
        if (a >= t.size()) return TestTool::None;
        if (t[a] == "test" || t[a] == "t" || t[a] == "tests") return TestTool::Npm;
        // `yarn jest`, `pnpm vitest run`, `pnpm exec jest`, `npm exec vitest`.
        if (t[a] == "jest" || t[a] == "vitest") return match(t, a, depth + 1);
        if (t[a] == "exec" || t[a] == "dlx" || t[a] == "x") {
            size_t s = nextWord(t, a + 1);
            return s < t.size() ? match(t, s, depth + 1) : TestTool::None;
        }
        if (t[a] == "run" || t[a] == "run-script") {
            size_t s = nextWord(t, a + 1);
            if (s < t.size() && t[s].compare(0, 4, "test") == 0) return TestTool::Npm;
        }
        return TestTool::None;
    }
    if (w == "cargo") {
        size_t a = nextWord(t, i + 1);
        if (a < t.size() && (t[a] == "test" || t[a] == "nextest")) return TestTool::Cargo;
        return TestTool::None;
    }
    if (w == "go") {
        size_t a = nextWord(t, i + 1);
        if (a < t.size() && t[a] == "test") return TestTool::Go;
    }
    return TestTool::None;
}

}  // namespace

TestTool testCommand(const std::string &command) {
    for (const Tokens &t : simpleCommands(command)) {
        TestTool m = match(t, 0, 0);
        if (m != TestTool::None) return m;
    }
    return TestTool::None;
}

// =============================================================== summaries
std::string stripAnsi(const std::string &s) {
    if (s.find('\x1b') == std::string::npos) return s;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\x1b') {
            out += s[i];
            continue;
        }
        if (i + 1 < s.size() && s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < s.size() && !(s[j] >= '@' && s[j] <= '~')) j++;
            i = j;
        } else if (i + 1 < s.size() && s[i + 1] == ']') {
            size_t j = i + 2;
            while (j < s.size() && s[j] != '\x07' && !(s[j] == '\x1b' && j + 1 < s.size() && s[j + 1] == '\\'))
                j++;
            i = (j < s.size() && s[j] == '\x1b') ? j + 1 : j;
        } else {
            i++;
        }
    }
    return out;
}

namespace {

std::string trim(const std::string &s, const char *chars = " \t\r") {
    size_t a = s.find_first_not_of(chars);
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(chars);
    return s.substr(a, b - a + 1);
}

// "3 failed, 10 passed, 1 skipped" -> {3,"failed"}, ... ; false unless all of
// it reads that way.
bool items(const std::string &s, char sep, std::vector<std::pair<long, std::string>> *out) {
    out->clear();
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') i++;
        size_t d = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        if (i == d || i - d > 9) return false;
        long n = std::strtol(s.substr(d, i - d).c_str(), nullptr, 10);
        if (i >= s.size() || s[i] != ' ') return false;
        i++;
        size_t w = i;
        while (i < s.size() && s[i] != sep) i++;
        std::string word = trim(s.substr(w, i - w));
        if (word.empty()) return false;
        out->emplace_back(n, word);
        if (i < s.size()) i++;   // the separator
    }
    return !out->empty();
}

bool pytestLine(const std::string &raw, Summary *sum) {
    std::string t = trim(raw, " \t\r=");
    if (t.compare(0, 15, "no tests ran in") == 0) {
        *sum = Summary();
        sum->found = true;
        sum->format = "pytest";
        sum->line = trim(raw);
        return true;
    }
    size_t in = t.rfind(" in ");
    if (in == std::string::npos || in + 4 >= t.size() || !(t[in + 4] >= '0' && t[in + 4] <= '9'))
        return false;
    std::vector<std::pair<long, std::string>> v;
    if (!items(t.substr(0, in), ',', &v)) return false;
    Summary s;
    for (auto &p : v) {
        const std::string &w = p.second;
        if (w == "passed") s.passed += (int)p.first;
        else if (w == "failed") s.failed += (int)p.first;
        else if (w == "error" || w == "errors") s.errors += (int)p.first;
        else if (w == "skipped") s.skipped += (int)p.first;
        else if (w == "xfailed" || w == "xpassed" || w == "deselected" || w == "warning" ||
                 w == "warnings" || w == "rerun" || w == "subtests passed")
            ;
        else return false;
    }
    s.found = true;
    s.format = "pytest";
    s.line = trim(raw);
    *sum = s;
    return true;
}

bool countLine(const std::string &raw, Summary *sum) {
    std::vector<std::pair<long, std::string>> v;
    std::string t = trim(raw);
    if (!items(t, ',', &v) || v.size() != 2 || v[0].second != "passed" || v[1].second != "failed")
        return false;
    *sum = Summary();
    sum->found = true;
    sum->format = "count";
    sum->passed = (int)v[0].first;
    sum->failed = (int)v[1].first;
    sum->line = t;
    return true;
}

bool ctestLine(const std::string &raw, Summary *sum) {
    std::string t = trim(raw);
    size_t a = t.find("% tests passed, ");
    if (a == std::string::npos) return false;
    size_t b = t.find(" tests failed out of ", a);
    if (b == std::string::npos) return false;
    const char *fs = t.c_str() + a + 16;
    char *end = nullptr;
    long failed = std::strtol(fs, &end, 10);
    if (end != t.c_str() + b) return false;
    long total = std::strtol(t.c_str() + b + 21, &end, 10);
    if (end == t.c_str() + b + 21) return false;
    *sum = Summary();
    sum->found = true;
    sum->format = "ctest";
    sum->failed = (int)failed;
    sum->passed = (int)std::max(0L, total - failed);
    sum->line = t;
    return true;
}

bool cargoLine(const std::string &raw, Summary *sum) {
    std::string t = trim(raw);
    if (t.compare(0, 13, "test result: ") != 0) return false;
    size_t dot = t.find(". ", 13);
    if (dot == std::string::npos) return false;
    std::string rest = t.substr(dot + 2);
    bool any = false;
    size_t i = 0;
    while (i < rest.size()) {
        size_t e = rest.find(';', i);
        if (e == std::string::npos) e = rest.size();
        std::string part = trim(rest.substr(i, e - i));
        i = e + 1;
        char *end = nullptr;
        long n = std::strtol(part.c_str(), &end, 10);
        if (end == part.c_str()) continue;
        std::string w = trim(std::string(end));
        if (w == "passed") sum->passed += (int)n, any = true;
        else if (w == "failed") sum->failed += (int)n, any = true;
        else if (w == "ignored") sum->skipped += (int)n;
    }
    if (any) {
        sum->found = true;
        sum->format = "cargo";
        sum->line = t;
    }
    return any;
}

// jest's "Tests:       1 failed, 2 skipped, 10 passed, 13 total" and
// vitest's "Tests  1 failed | 10 passed | 1 skipped (12)". Not "Test Suites:"
// or "Test Files": those count files.
bool jestLine(const std::string &raw, Summary *sum) {
    std::string t = trim(raw);
    const bool jest = t.compare(0, 6, "Tests:") == 0;
    const bool vitest = !jest && t.compare(0, 6, "Tests ") == 0;
    if (!jest && !vitest) return false;
    std::string rest = trim(t.substr(6));
    if (vitest) {
        // The total in parentheses at the end.
        size_t paren = rest.rfind(" (");
        if (paren == std::string::npos || rest.back() != ')') return false;
        rest = trim(rest.substr(0, paren));
    }
    std::vector<std::pair<long, std::string>> v;
    if (!items(rest, vitest ? '|' : ',', &v)) return false;
    Summary s;
    for (auto &p : v) {
        const std::string &w = p.second;
        if (w == "passed") s.passed += (int)p.first;
        else if (w == "failed") s.failed += (int)p.first;
        else if (w == "skipped" || w == "todo" || w == "pending") s.skipped += (int)p.first;
        else if (w == "total" || w == "expected fail") ;
        else return false;
    }
    s.found = true;
    s.format = jest ? "jest" : "vitest";
    s.line = t;
    *sum = s;
    return true;
}

// "Ran 12 tests in 0.004s" (also "Ran 1 test in"), then, after a blank line,
// "OK", "OK (skipped=1, expected failures=1)", "FAILED (failures=2, errors=1)"
// or "NO TESTS RAN". Passed is what ran less everything named.
bool unittestAt(const std::vector<std::string> &lines, size_t k, Summary *sum) {
    std::string ran = trim(lines[k]);
    if (ran.compare(0, 4, "Ran ") != 0) return false;
    char *end = nullptr;
    long n = std::strtol(ran.c_str() + 4, &end, 10);
    if (end == ran.c_str() + 4) return false;
    std::string after(end);
    if (after.compare(0, 10, " tests in ") != 0 && after.compare(0, 9, " test in ") != 0)
        return false;
    size_t j = k + 1;
    while (j < lines.size() && trim(lines[j]).empty()) j++;
    if (j >= lines.size()) return false;
    std::string verdict = trim(lines[j]);
    Summary s;
    std::string counts;
    if (verdict == "OK" || verdict == "NO TESTS RAN") {
    } else if (verdict.compare(0, 4, "OK (") == 0 && verdict.back() == ')') {
        counts = verdict.substr(4, verdict.size() - 5);
    } else if (verdict.compare(0, 8, "FAILED (") == 0 && verdict.back() == ')') {
        counts = verdict.substr(8, verdict.size() - 9);
    } else {
        return false;
    }
    long named = 0;
    size_t i = 0;
    while (i < counts.size()) {
        size_t e = counts.find(',', i);
        if (e == std::string::npos) e = counts.size();
        std::string part = trim(counts.substr(i, e - i));
        i = e + 1;
        size_t eq = part.find('=');
        if (eq == std::string::npos) return false;
        std::string key = trim(part.substr(0, eq));
        long v = std::strtol(part.c_str() + eq + 1, nullptr, 10);
        if (key == "failures") s.failed += (int)v;
        else if (key == "errors") s.errors += (int)v;
        else if (key == "skipped") s.skipped += (int)v;
        else if (key != "expected failures" && key != "unexpected successes") return false;
        named += v;
    }
    s.passed = (int)std::max(0L, n - named);
    s.found = true;
    s.format = "unittest";
    s.line = ran + "; " + verdict;
    *sum = s;
    return true;
}

// tox's line for each environment it ran, tox 4's and tox 3's.
bool toxCount(const std::vector<std::string> &lines, Summary *sum) {
    Summary g;
    for (const std::string &l0 : lines) {
        std::string l = trim(l0);
        if (l.compare(0, 6, "ERROR:") == 0) l = trim(l.substr(6));
        size_t colon = l.find(": ");
        if (colon == std::string::npos || colon == 0 || l.find(' ') < colon) continue;
        std::string rest = l.substr(colon + 2);
        if (rest.compare(0, 4, "OK (") == 0 || rest == "commands succeeded") {
            g.passed++;
            g.line = l;
        } else if (rest.compare(0, 5, "FAIL ") == 0 || rest == "commands failed") {
            g.failed++;
            g.line = l;
        } else if (rest.compare(0, 6, "SKIP (") == 0 || rest == "skipped tests") {
            g.skipped++;
        }
    }
    if (g.passed + g.failed == 0) return false;
    g.found = true;
    g.format = "tox";
    *sum = g;
    return true;
}

// nox's "nox > Session NAME was successful in 2 seconds.", "... failed.",
// "... skipped: why".
bool noxCount(const std::vector<std::string> &lines, Summary *sum) {
    Summary g;
    auto endsWith = [](const std::string &l, const char *tail) {
        const size_t n = std::strlen(tail);
        return l.size() >= n && l.compare(l.size() - n, n, tail) == 0;
    };
    for (const std::string &l0 : lines) {
        std::string l = trim(l0);
        if (l.compare(0, 14, "nox > Session ") != 0) continue;
        if (l.find(" was successful") != std::string::npos) g.passed++, g.line = l;
        else if (endsWith(l, " failed.") || endsWith(l, " failed")) g.failed++, g.line = l;
        else if (l.find(" skipped") != std::string::npos) g.skipped++;
    }
    if (g.passed + g.failed == 0) return false;
    g.found = true;
    g.format = "nox";
    *sum = g;
    return true;
}

}  // namespace

Summary parseSummary(const std::vector<std::string> &input, TestTool hint) {
    std::vector<std::string> lines;
    lines.reserve(input.size());
    for (const std::string &l : input) lines.push_back(stripAnsi(l));
    Summary s;
    Summary cargo;
    for (const std::string &l : lines) cargoLine(l, &cargo);
    if (cargo.found) return cargo;
    if (hint == TestTool::Go) {
        Summary g;
        for (const std::string &l0 : lines) {
            std::string l = trim(l0, "\r");
            if (l.size() > 3 && l.compare(0, 2, "ok") == 0 && (l[2] == ' ' || l[2] == '\t')) {
                g.passed++;
                g.line = trim(l);
            } else if (l.size() > 5 && l.compare(0, 4, "FAIL") == 0 && (l[4] == '\t' || l[4] == ' ') &&
                       !trim(l.substr(5)).empty()) {
                g.failed++;
                g.line = trim(l);
            }
        }
        if (g.passed + g.failed > 0) {
            g.found = true;
            g.format = "go";
            return g;
        }
    }
    if (hint == TestTool::Tox && toxCount(lines, &s)) return s;
    if (hint == TestTool::Nox && noxCount(lines, &s)) return s;
    for (size_t k = lines.size(); k-- > 0;) {
        if (pytestLine(lines[k], &s) || ctestLine(lines[k], &s) || countLine(lines[k], &s) ||
            jestLine(lines[k], &s) || unittestAt(lines, k, &s))
            return s;
    }
    return Summary();
}

Summary parseSummary(const std::string &text, TestTool hint) {
    std::vector<std::string> lines;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        lines.push_back(text.substr(i, e - i));
        i = e + 1;
    }
    return parseSummary(lines, hint);
}

// ================================================================= reading
Reader::Reader(std::string transcript, const AgentTranscript::Format &format) : format_(format) {
    tail_.path = transcript;
    ev_.transcript = std::move(transcript);
}

bool Reader::update(const Fs &fs) {
    AgentTranscript::ReadStats st;
    bool changed = false;
    const std::string &path = tail_.path;
    auto onLine = [&](const Line &l) {
        changed = true;
        // A subagent's transcript is all side chain; a main one's own turns
        // are not. Its first message says which this is.
        if (!sawFirst_ && (l.type == "user" || l.type == "assistant")) {
            sawFirst_ = true;
            sideFirst_ = l.sidechain;
        }
        if (!l.version.empty() &&
            std::find(ev_.versions.begin(), ev_.versions.end(), l.version) == ev_.versions.end() &&
            ev_.versions.size() < 16)
            ev_.versions.push_back(l.version);
        if (ev_.cwd.empty() && !l.cwd.empty()) ev_.cwd = l.cwd;
        // An older main transcript also holds its subagents' lines.
        if (l.sidechain && !sideFirst_) return true;
        if (l.type == "assistant") {
            for (const Block &b : l.blocks) {
                if (b.kind == Block::Kind::ToolUse && b.name == "Bash") {
                    Command c;
                    c.toolUseId = b.id;
                    c.description = b.description;
                    c.command = b.command;
                    c.commandCut = b.commandCut;
                    c.time = l.time;
                    c.cwd = l.cwd.empty() ? ev_.cwd : l.cwd;
                    c.test = testCommand(b.command);
                    if (c.test != TestTool::None) ev_.lastTest = (int)ev_.commands.size();
                    if (!b.id.empty()) pending_[b.id] = ev_.commands.size();
                    ev_.commands.push_back(std::move(c));
                } else if (b.kind == Block::Kind::Text && !b.text.empty()) {
                    if (!l.messageId.empty() && l.messageId == lastMessageId_ &&
                        !ev_.finalMessage.empty()) {
                        ev_.finalMessage += "\n\n";
                        ev_.finalMessage += b.text;
                    } else {
                        ev_.finalMessage = b.text;
                        ev_.finalCut = false;
                    }
                    ev_.finalCut = ev_.finalCut || b.textCut;
                    ev_.finalTime = l.time;
                    lastMessageId_ = l.messageId;
                }
            }
        } else {
            for (const Block &b : l.blocks) {
                if (b.kind != Block::Kind::ToolResult) continue;
                auto it = pending_.find(b.id);
                if (it == pending_.end()) continue;
                Command &c = ev_.commands[it->second];
                pending_.erase(it);
                c.answered = true;
                c.isError = b.isError;
                c.output = b.output;
                if (c.test == TestTool::None || b.output.empty()) continue;
                AgentTranscript::Output o =
                    AgentTranscript::readOutput(fs, path, b.output, 4, 120, 32 * 1024);
                std::vector<std::string> lines = o.head;
                std::string head;
                for (const std::string &h : o.head) head += h + "\n";
                const std::string bg = AgentTranscript::backgroundOutputPath(head);
                if (!o.persistedPath.empty() || !bg.empty()) {
                    c.persistedPath = o.persistedPath.empty() ? bg : o.persistedPath;
                    c.inBackground = !bg.empty();
                    lines = AgentTranscript::readTextFile(fs, c.persistedPath, 0, 120, 32 * 1024).tail;
                } else {
                    lines.insert(lines.end(), o.tail.begin(), o.tail.end());
                }
                c.summary = parseSummary(lines, c.test);
            }
        }
        return true;
    };
    auto onReset = [&]() {
        std::string t = ev_.transcript;
        ev_ = Evidence();
        ev_.transcript = t;
        pending_.clear();
        lastMessageId_.clear();
        sawFirst_ = sideFirst_ = false;
        changed = true;
    };
    tail_.read(fs, format_, onLine, onReset, &st);
    bytesRead_ += st.bytesRead;
    if (st.badLines) {
        ev_.badLines += st.badLines;
        changed = true;
    }
    return changed;
}

Evidence read(const Fs &fs, const std::string &transcript) {
    Reader r(transcript);
    r.update(fs);
    return r.evidence();
}

}  // namespace AgentEvidence
