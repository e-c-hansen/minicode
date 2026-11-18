// AgentTask.cpp — see AgentTask.h.
#include "AgentTask.h"

#include "RunFile.h"

#include <algorithm>
#include <set>

namespace AgentTask {

namespace {

bool isAlnum(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

// Words that say nothing about the task on their own.
const std::set<std::string> &stopWords() {
    static const std::set<std::string> words = {
        "a",    "an",    "the",  "to",     "of",   "and",  "or",   "in",    "on",
        "at",   "for",   "with", "from",   "by",   "into", "is",   "are",   "be",
        "it",   "its",   "this", "that",   "these", "those", "please", "can",
        "could", "would", "you", "we",     "i",    "me",   "my",   "our",   "your",
        "so",   "then",  "just", "some",   "all",  "as",   "up",   "lets", "let"};
    return words;
}

}  // namespace

std::string nameFromPrompt(const std::string &prompt) {
    std::vector<std::string> words;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty() && !stopWords().count(cur)) words.push_back(cur);
        cur.clear();
    };
    for (size_t i = 0; i < prompt.size(); i++) {
        const unsigned char c = (unsigned char)prompt[i];
        if (isAlnum(c)) {
            cur += lower((char)c);
            continue;
        }
        // An apostrophe inside a word joins it: "don't" is "dont".
        if (c == '\'' && !cur.empty()) continue;
        if (c == 0xE2 && i + 2 < prompt.size() && (unsigned char)prompt[i + 1] == 0x80 &&
            (unsigned char)prompt[i + 2] == 0x99 && !cur.empty()) {
            i += 2;
            continue;
        }
        flush();
    }
    flush();
    const size_t kMax = 32, kWords = 4;
    std::string out;
    for (const std::string &w : words) {
        if (out.empty()) {
            out = w.substr(0, kMax);
        } else {
            if (out.size() + 1 + w.size() > kMax) break;
            out += "-" + w;
        }
        if (std::count(out.begin(), out.end(), '-') + 1 >= (long)kWords) break;
    }
    return out.empty() ? "task" : out;
}

std::string sanitizeName(const std::string &typed) {
    std::string s;
    for (unsigned char c : typed) {
        const bool keep = isAlnum(c) || c == '.' || c == '_' || c == '-';
        char ch = keep ? (char)c : '-';
        // Runs of '-' are one, and so are runs of '.' (git refuses "..").
        if ((ch == '-' || ch == '.') && !s.empty() && s.back() == ch) continue;
        s += ch;
    }
    auto trimEnds = [&] {
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == '-' || s[a] == '.')) a++;
        while (b > a && (s[b - 1] == '-' || s[b - 1] == '.')) b--;
        s = s.substr(a, b - a);
    };
    trimEnds();
    if (s.size() > 60) {
        s.resize(60);
        trimEnds();
    }
    // git refuses a ref ending in ".lock".
    while (s.size() >= 5 && s.compare(s.size() - 5, 5, ".lock") == 0) {
        s.resize(s.size() - 5);
        trimEnds();
    }
    return s;
}

std::string uniqueName(const std::string &base,
                       const std::function<bool(const std::string &)> &taken) {
    if (!taken(base)) return base;
    for (int n = 2; n < 10000; n++) {
        const std::string candidate = base + "-" + std::to_string(n);
        if (!taken(candidate)) return candidate;
    }
    return base + "-" + std::to_string(10000);
}

std::string worktreePath(const std::string &folder, const std::string &name) {
    std::string f = folder;
    while (f.size() > 1 && f.back() == '/') f.pop_back();
    return f + "/.claude/worktrees/" + name;
}

bool insideClaudeHome(const std::string &path, const std::string &home) {
    if (home.empty()) return false;
    std::string h = home;
    while (h.size() > 1 && h.back() == '/') h.pop_back();
    const std::string dir = h + "/.claude";
    return path == dir || path.compare(0, dir.size() + 1, dir + "/") == 0;
}

std::vector<std::string> worktreeAddArgs(const std::string &branch, const std::string &path) {
    return {"worktree", "add", "-b", branch, path, "HEAD"};
}

bool promptFitsOnLine(const std::string &prompt) {
    if (prompt.empty() || prompt.size() > 160) return false;
    for (unsigned char c : prompt)
        if (c < 0x20 || c == 0x7F) return false;
    return true;
}

static std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r'))
        b--;
    return s.substr(a, b - a);
}

std::string startCommand(const std::string &command, const std::string &dir,
                         const std::string &prompt, const std::string &promptFile) {
    const std::string head = "cd " + RunFile::shellQuote(dir) + " && " + trim(command) + " ";
    // Always in quotes, even a plain word, so the line reads the same way
    // whatever was typed.
    if (promptFitsOnLine(prompt))
        return head + "'" + RunFile::quoteInside(prompt, '\'') + "'";
    return head + "\"$(cat " + RunFile::shellQuote(promptFile) + ")\"";
}

std::string startsFromText(const std::string &branch, int changed) {
    std::string s = branch.empty() ? "From the last commit (HEAD)"
                                   : "From the last commit on " + branch;
    if (changed > 0)
        s += "; your " + std::to_string(changed) + " uncommitted " +
             (changed == 1 ? "change stays" : "changes stay") +
             " here and " + (changed == 1 ? "is" : "are") + " not in the task's worktree";
    return s;
}

std::string trimPrompt(const std::string &prompt) { return trim(prompt); }

}  // namespace AgentTask
