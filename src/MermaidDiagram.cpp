// MermaidDiagram.cpp — see MermaidDiagram.h.
#include "MermaidDiagram.h"
#include "IdeLink.h"   // sha1, toHex

namespace MermaidDiagram {

const int kVersion = 3;   // 2: the drawer is in the key; 3: mermaid.js drawn by WebKit
const int kScale = 2;

const char *const kDrawerMmdc = "mmdc";
const char *const kDrawerMermaidJs = "mermaid.js 12.3.0";
const char *const kMermaidJsVersion = "12.3.0";
const char *const kMermaidJsUrl = "https://cdn.jsdelivr.net/npm/mermaid@12.3.0/dist/mermaid.min.js";
const char *const kMermaidJsSha256 =
    "336df803d87a38b70a01e736e666c8f7601d2ce42f5e47c8fafe5d721074b9a3";
const long kMermaidJsBytes = 5537838;

std::string renderScript() {
    return
        "mermaid.initialize({startOnLoad: false, theme: theme, securityLevel: 'strict'});\n"
        "await document.fonts.ready;\n"
        "document.documentElement.style.background = 'transparent';\n"
        "document.body.style.cssText = 'margin:0;background:transparent';\n"
        "document.body.replaceChildren();\n"
        "const id = 'mc' + Math.random().toString(36).slice(2);\n"
        "const out = await mermaid.render(id, source);\n"
        "const holder = document.createElement('div');\n"
        "holder.style.cssText = 'position:absolute;left:0;top:0';\n"
        "holder.innerHTML = out.svg;\n"
        "document.body.appendChild(holder);\n"
        "const svg = holder.querySelector('svg');\n"
        "if (!svg) throw new Error('mermaid.js drew nothing');\n"
        "const box = svg.viewBox && svg.viewBox.baseVal;\n"
        "svg.removeAttribute('style');\n"
        "svg.style.display = 'block';\n"
        "if (box && box.width > 0 && box.height > 0) {\n"
        "  svg.setAttribute('width', String(Math.ceil(box.width)));\n"
        "  svg.setAttribute('height', String(Math.ceil(box.height)));\n"
        "}\n"
        "const r = svg.getBoundingClientRect();\n"
        "return {x: r.left, y: r.top, width: Math.ceil(r.width), height: Math.ceil(r.height)};\n";
}

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

std::string key(const std::string &source, const std::string &theme, const std::string &drawer) {
    return IdeLink::toHex(IdeLink::sha1(std::to_string(kVersion) + "\n" + drawer + "\n" + theme +
                                        "\n" + source));
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
