// MermaidDiagram.cpp — see MermaidDiagram.h.
#include "MermaidDiagram.h"
#include "IdeLink.h"   // sha1, toHex

namespace MermaidDiagram {

const int kVersion = 1;
const int kScale = 2;

bool isMermaidRun(const MdRun &r) { return r.codeBlock && r.lang == "mermaid"; }

std::vector<Block> blocks(const std::vector<MdRun> &runs) {
    std::vector<Block> out;
    for (size_t i = 0; i < runs.size();) {
        if (!isMermaidRun(runs[i])) { i++; continue; }
        Block b;
        b.first = i;
        b.line = runs[i].line;
        // One run per line of the block; two blocks always have a run that
        // is not code between them (the gap after the first).
        while (i < runs.size() && isMermaidRun(runs[i])) b.source += runs[i++].text;
        b.end = i;
        out.push_back(std::move(b));
    }
    return out;
}

std::string theme(bool dark) { return dark ? "dark" : "default"; }

std::string key(const std::string &source, const std::string &theme) {
    return IdeLink::toHex(IdeLink::sha1(std::to_string(kVersion) + "\n" + theme + "\n" + source));
}

std::vector<std::string> arguments(const std::string &input, const std::string &output,
                                   const std::string &theme) {
    return {"-i", input, "-o", output, "-t", theme, "-b", "transparent",
            "-s", std::to_string(kScale), "-q"};
}

static std::string trimmed(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// A line of a JavaScript stack trace: "    at f (file:...)" or, from the
// page mmdc runs Mermaid in, "Parser.parse (https://...:12:3)".
static bool stackLine(const std::string &line) {
    const std::string t = trimmed(line);
    if (t.compare(0, 3, "at ") == 0) return true;
    return t.size() > 2 && t.back() == ')' &&
           (t.find(" (http") != std::string::npos || t.find(" (file:") != std::string::npos ||
            t.find(" (node:") != std::string::npos);
}

std::string errorSummary(const std::string &stderrText, const std::string &fallback) {
    std::vector<std::string> lines;
    for (size_t at = 0; at <= stderrText.size();) {
        size_t nl = stderrText.find('\n', at);
        if (nl == std::string::npos) nl = stderrText.size();
        std::string line = stderrText.substr(at, nl - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        at = nl + 1;
    }
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string t = trimmed(lines[i]);
        if (t.compare(0, 7, "Error: ") != 0 && t != "Error:") continue;
        std::string out = trimmed(t.substr(6));
        int kept = out.empty() ? 0 : 1;
        for (size_t k = i + 1; k < lines.size() && kept < 6; k++) {
            if (stackLine(lines[k]) || trimmed(lines[k]).empty()) break;
            out += (out.empty() ? "" : "\n") + lines[k];
            kept++;
        }
        if (!out.empty()) return out;
    }
    for (size_t i = lines.size(); i-- > 0;) {
        const std::string t = trimmed(lines[i]);
        if (!t.empty() && !stackLine(lines[i])) return t;
    }
    return fallback;
}

}  // namespace MermaidDiagram
