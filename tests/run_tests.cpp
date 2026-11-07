// run_tests.cpp — unit tests for the pure-C++ core (no framework, no deps).
// Build/run with `make test`. Exits non-zero if any check fails.
#include "SyntaxHighlighter.h"
#include "TermLinks.h"
#include "MarkdownParser.h"
#include "MarkdownEdit.h"
#include "MarkdownTasks.h"
#include "MathTex.h"
#include "LatexScratch.h"
#include "TerminalStream.h"
#include "TerminalScreen.h"
#include "Settings.h"
#include "RunFile.h"
#include "LineComments.h"
#include "LatexDoc.h"
#include "SyncTex.h"
#include "Json.h"
#include "LspClient.h"
#include "PythonEnvs.h"
#include "FolderSearch.h"
#include "GitStatus.h"
#include "GitGraph.h"
#include "EditorTabs.h"
#include "Formatter.h"
#include "InstallHints.h"
#include "ChangeRequest.h"
#include "LineDiff.h"
#include "MultiCaret.h"
#include "CommandPalette.h"
#include "Backups.h"
#include "InlineAgent.h"
#include "SymbolOutline.h"
#include "NavHistory.h"
#include "LineIndex.h"
#include "BracketMatch.h"
#include "ControlProtocol.h"
#include "PageCheck.h"
#include "SideBySide.h"
#include "AgentTranscript.h"
#include "AgentIndex.h"
#include "AgentEvidence.h"
#include "AgentView.h"
#include "Folding.h"
#include "FileSniff.h"
#include "Csv.h"
#include "JsonOutline.h"
#include "Notebook.h"
#include "Commands.h"
#include "LegacyHighlighter.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>   // mkfifo, for the folder search test
#include <unistd.h>     // getpid, for scratch folder names
#endif
#ifdef __APPLE__
#include <sys/time.h>    // utimes, for the atomic write's creation date
#include <sys/xattr.h>   // setxattr, for the atomic write's extended attributes
#endif

// --------------------------------------------------------------- tiny harness
static int g_pass = 0, g_fail = 0;
static const char *g_group = "";

#define GROUP(name) g_group = name
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else {                                                                 \
            g_fail++;                                                          \
            std::printf("  FAIL [%s] %s:%d  %s\n", g_group, __FILE__,          \
                        __LINE__, #cond);                                      \
        }                                                                      \
    } while (0)

// --------------------------------------------------------- syntax highlighter
namespace {

std::string slice(const std::string &s, const Token &t) {
    return s.substr(t.start, t.length);
}

// Is there a token whose exact text is `word` with style `st`?
bool hasToken(const std::string &text, const std::string &ext,
              const std::string &word, TokenStyle st) {
    for (const Token &t : SyntaxHighlighter::highlight(text, ext))
        if (slice(text, t) == word && t.style == st) return true;
    return false;
}

// Is there any token with style `st`?
bool hasStyle(const std::string &text, const std::string &ext, TokenStyle st) {
    for (const Token &t : SyntaxHighlighter::highlight(text, ext))
        if (t.style == st) return true;
    return false;
}

void testSyntax() {
    GROUP("syntax:supports");
    CHECK(SyntaxHighlighter::supports("py"));
    CHECK(SyntaxHighlighter::supports("cpp"));
    CHECK(SyntaxHighlighter::supports("js"));
    CHECK(!SyntaxHighlighter::supports("xyz"));
    CHECK(!SyntaxHighlighter::supports(""));
    for (const char *e : {"jsonl", "ndjson", "jsonc", "mjs", "kt", "swift", "cs",
                          "rb", "lua", "sql", "ini", "css", "pyi", "fish"})
        CHECK(SyntaxHighlighter::supports(e));
    // JSON Lines gets the JSON grammar: strings, numbers, true/false/null.
    CHECK(hasToken("{\"a\": 1, \"b\": true}\n{\"c\": null}\n", "jsonl", "\"a\"",
                   TokenStyle::String));
    CHECK(hasToken("{\"c\": null}\n", "jsonl", "null", TokenStyle::Keyword));
    CHECK(hasToken("SELECT 1 -- note\n", "sql", "-- note", TokenStyle::Comment));
    CHECK(hasToken("[s]\n; note\n", "ini", "; note", TokenStyle::Comment));
    CHECK(hasToken("a { color: red; } /* c */\n", "css", "/* c */", TokenStyle::Comment));
    // A URL in plain CSS is not a comment.
    CHECK(!hasStyle("b { background: url(http://x/y.png); }\n", "css", TokenStyle::Comment));

    GROUP("syntax:python");
    std::string py = "def foo(x):\n    return 1  # note\n";
    CHECK(hasToken(py, "py", "def", TokenStyle::Keyword));
    CHECK(hasToken(py, "py", "return", TokenStyle::Keyword));
    CHECK(hasToken(py, "py", "1", TokenStyle::Number));
    CHECK(hasToken(py, "py", "# note", TokenStyle::Comment));
    CHECK(hasToken(py, "py", "foo", TokenStyle::Function));  // foo(
    CHECK(hasToken("s = \"hi\"", "py", "\"hi\"", TokenStyle::String));
    // A '#' comment must not be mistaken for anything else.
    CHECK(!hasStyle("# just a comment", "py", TokenStyle::Keyword));

    GROUP("syntax:python-fstrings");
    {
        // The style at each byte, '-' where no token is.
        auto styles = [](const std::string &text) {
            std::string m(text.size(), '-');
            const char *letters = "pktscnPf";   // TokenStyle order
            for (const Token &t : SyntaxHighlighter::highlight(text, "py"))
                for (size_t k = t.start; k < t.start + t.length && k < m.size(); ++k)
                    m[k] = letters[(int)t.style];
            return m;
        };
        // Text is String, the braces Keyword, the expression code.
        CHECK(styles("x = f'a {name} b'") == "-----sssk----ksss");
        CHECK(styles("f\"{len(x) + 1}\"") == "-skfff------nks");
        // {{ and }} are literal braces.
        CHECK(styles("f'{{x}}'") == "-sssssss");
        // A plain string with braces is all string.
        CHECK(styles("'{x}'") == "sssss");
        // Conversion and format spec, with a nested field in the spec.
        CHECK(styles("f'{v!r:>{w}}'") == "-sk-kkkkk-kks");
        // A string inside a field, and code after the f-string.
        CHECK(styles("f\"{d['k']}\" + len(x)") == "-sk--sss-ks---fff---");
        // Other prefixes; a longer word is not a prefix.
        CHECK(styles("rf'{x}'") == "--sk-ks");
        CHECK(styles("F'{x}'") == "tsk-ks"   /* F reads as a capitalized name */);
        CHECK(styles("t'{x}'") == "-sk-ks");
        CHECK(styles("elif'{x}'") == "kkkksssss");
        CHECK(styles("b'{x}'") == "-sssss");
        // A keyword inside a field.
        CHECK(styles("f'{x if y else z}'") == "-sk--kk---kkkk--ks");
        // Triple-quoted, over lines: fields on the next line too, and code
        // after the close.
        std::string tri = "s = f\"\"\"a\n{x} b\n\"\"\"\nreturn";
        CHECK(styles(tri) == "-----sssss" "k-ksss" "sss-" "kkkkkk");
        IncrementalHighlighter<char> h("py");
        h.reset(StringSource<char>(tri), nullptr);
        CHECK(h.endState(0).kind == LexState::TripleString && (h.endState(0).quote & 0xFF) == u'"' &&
              (h.endState(0).quote & 0x100));
        CHECK(h.endState(2).kind == LexState::Normal);
        // A backslash carries a single-quoted f-string over the line.
        CHECK(styles("f'a\\\n{x}'") == "-ssss" "k-ks");
        // An unclosed field ends with its line; the next line is code.
        CHECK(styles("f'{x\ny = 1") == "-sk--" "----n");
    }

    GROUP("syntax:cpp");
    std::string cpp = "int main() {\n  // c\n  return 0;\n}\n";
    CHECK(hasToken(cpp, "cpp", "int", TokenStyle::Type));
    CHECK(hasToken(cpp, "cpp", "return", TokenStyle::Keyword));
    CHECK(hasToken(cpp, "cpp", "0", TokenStyle::Number));
    CHECK(hasToken(cpp, "cpp", "// c", TokenStyle::Comment));
    CHECK(hasToken(cpp, "cpp", "main", TokenStyle::Function));
    CHECK(hasStyle("#include <cstdio>", "cpp", TokenStyle::Preprocessor));
    CHECK(hasToken("/* block */ x", "cpp", "/* block */", TokenStyle::Comment));

    GROUP("syntax:strings-escapes");
    // An escaped quote should not end the string early.
    CHECK(hasToken("\"a\\\"b\"", "cpp", "\"a\\\"b\"", TokenStyle::String));

    GROUP("syntax:plain");
    // Plain text / unknown extension yields no styled tokens.
    CHECK(SyntaxHighlighter::highlight("just words here", "txt").empty());

    GROUP("syntax:tex");
    for (const char *e : {"tex", "ltx", "latex", "sty", "cls", "bib"})
        CHECK(SyntaxHighlighter::supports(e));
    std::string tex = "\\documentclass{article}\n\\usepackage{amsmath}\n"
                      "\\section{Intro} Some \\emph{words}.\n";
    CHECK(hasToken(tex, "tex", "\\documentclass", TokenStyle::Preprocessor));
    CHECK(hasToken(tex, "tex", "\\usepackage", TokenStyle::Preprocessor));
    CHECK(hasToken(tex, "tex", "\\section", TokenStyle::Function));
    CHECK(hasToken(tex, "tex", "\\emph", TokenStyle::Keyword));
    CHECK(!hasStyle("Just prose, 42 of it.", "tex", TokenStyle::Number));
    // \% is a character, not a comment; \\% is a line break, then one.
    std::string pct = "50\\% off % real\n";
    CHECK(hasToken(pct, "tex", "\\%", TokenStyle::Keyword));
    CHECK(hasToken(pct, "tex", "% real", TokenStyle::Comment));
    CHECK(hasToken("a\\\\% c", "tex", "\\\\", TokenStyle::Keyword));
    CHECK(hasToken("a\\\\% c", "tex", "% c", TokenStyle::Comment));
    // Math of every kind, with commands inside still commands.
    CHECK(hasToken("see $x^2$ here", "tex", "$x^2$", TokenStyle::Number));
    CHECK(hasToken("$$y$$", "tex", "$$y$$", TokenStyle::Number));
    CHECK(hasToken("\\(a\\) b", "tex", "\\(a\\)", TokenStyle::Number));
    CHECK(hasToken("\\[ b \\]", "tex", "\\[ b \\]", TokenStyle::Number));
    CHECK(hasToken("$\\alpha + 1$", "tex", "\\alpha", TokenStyle::Keyword));
    CHECK(hasToken("$\\alpha + 1$", "tex", " + 1$", TokenStyle::Number));
    CHECK(hasToken("$a % c\nb$", "tex", "% c", TokenStyle::Comment));
    CHECK(hasToken("$a % c\nb$", "tex", "b$", TokenStyle::Number));
    CHECK(!hasStyle("\\$5 and \\$6", "tex", TokenStyle::Number));
    // Display math across lines is one token, cut only by commands.
    CHECK(hasToken("\\[\na\n\\]\nb", "tex", "\\[\na\n\\]", TokenStyle::Number));
    CHECK(hasToken("\\[\n\\alpha\n\\]", "tex", "\\[\n", TokenStyle::Number));
    CHECK(hasToken("\\[\n\\alpha\n\\]", "tex", "\n\\]", TokenStyle::Number));
    // An unclosed $ stops at the paragraph's end.
    std::string open = "$a\n\nb $c$ d";
    CHECK(hasToken(open, "tex", "$a\n", TokenStyle::Number));
    CHECK(hasToken(open, "tex", "$c$", TokenStyle::Number));
    // \begin / \end and their environment names.
    CHECK(hasToken("\\begin{itemize}", "tex", "\\begin", TokenStyle::Keyword));
    CHECK(hasToken("\\begin{itemize}", "tex", "itemize", TokenStyle::Type));
    std::string eq = "\\begin{equation}\nE = mc^2 % note\n\\end{equation}\nx";
    CHECK(hasToken(eq, "tex", "\nE = mc^2 ", TokenStyle::Number));
    CHECK(hasToken(eq, "tex", "% note", TokenStyle::Comment));
    CHECK(hasToken(eq, "tex", "\\end", TokenStyle::Keyword));
    CHECK(hasToken(eq, "tex", "equation", TokenStyle::Type));
    // Verbatim: nothing inside is TeX, and what follows is normal again.
    std::string verb = "\\begin{verbatim}\n$x % \\y\n\\end{verbatim}\nafter $z$\n";
    CHECK(hasToken(verb, "tex", "\n$x % \\y\n", TokenStyle::String));
    CHECK(hasToken(verb, "tex", "verbatim", TokenStyle::Type));
    CHECK(hasToken(verb, "tex", "$z$", TokenStyle::Number));
    CHECK(!hasStyle(verb, "tex", TokenStyle::Comment));
    CHECK(hasToken("\\begin{comment}\n\\foo $\n\\end{comment} $q$", "tex",
                   "\n\\foo $\n", TokenStyle::Comment));
    CHECK(hasToken("\\begin{comment}\n\\foo $\n\\end{comment} $q$", "tex",
                   "$q$", TokenStyle::Number));
    std::string vb = "\\verb|$%| then $m$ \\verb*+a+ \\verb@b@";
    CHECK(hasToken(vb, "tex", "\\verb", TokenStyle::Keyword));
    CHECK(hasToken(vb, "tex", "|$%|", TokenStyle::String));
    CHECK(hasToken(vb, "tex", "$m$", TokenStyle::Number));
    CHECK(hasToken(vb, "tex", "\\verb*", TokenStyle::Keyword));
    CHECK(hasToken(vb, "tex", "+a+", TokenStyle::String));
    CHECK(hasToken(vb, "tex", "@b@", TokenStyle::String));
    // An unclosed \verb ends with its line.
    CHECK(hasToken("\\verb|abc\n$d$", "tex", "|abc", TokenStyle::String));
    CHECK(hasToken("\\verb|abc\n$d$", "tex", "$d$", TokenStyle::Number));
    // '@' is a letter in command names (.sty); .bib entries.
    CHECK(hasToken("\\@ifnextchar[", "sty", "\\@ifnextchar", TokenStyle::Keyword));
    std::string bib = "@article{key,\n  title = {On $x$},\n}\n";
    CHECK(hasToken(bib, "bib", "@article", TokenStyle::Preprocessor));
    CHECK(hasToken(bib, "bib", "$x$", TokenStyle::Number));
    CHECK(!hasStyle(bib, "tex", TokenStyle::Preprocessor));
    // Inside a .bib entry a '%' is text (a percent-escaped DOI), across
    // lines and nested braces; outside an entry it is still a comment.
    std::string doi = "% my refs\n@article{k,\n  url = {https://doi.org/10.1000%2Fabc},\n"
                      "  note = {50% {off}},\n}\n% after\n@book(b, title = {x%y})\n% end\n";
    CHECK(hasToken(doi, "bib", "% my refs", TokenStyle::Comment));
    CHECK(!hasToken(doi, "bib", "%2Fabc},", TokenStyle::Comment));
    CHECK(!hasToken(doi, "bib", "% {off}},", TokenStyle::Comment));
    CHECK(hasToken(doi, "bib", "% after", TokenStyle::Comment));
    CHECK(!hasToken(doi, "bib", "%y})", TokenStyle::Comment));
    CHECK(hasToken(doi, "bib", "% end", TokenStyle::Comment));
    CHECK(hasToken(doi, "bib", "@book", TokenStyle::Preprocessor));
    CHECK(hasToken("@misc{m, title = {\\emph{A} % b}}", "bib", "\\emph", TokenStyle::Keyword));
    // \url and \href's first argument: a '%' is part of the address.
    std::string url = "See \\url{https://x.org/a%20b} and \\href{http://y.io/%7Ez}{text} % real\n";
    CHECK(hasToken(url, "tex", "https://x.org/a%20b", TokenStyle::String));
    CHECK(hasToken(url, "tex", "http://y.io/%7Ez", TokenStyle::String));
    CHECK(hasToken(url, "tex", "\\href", TokenStyle::Keyword));
    CHECK(hasToken(url, "tex", "% real", TokenStyle::Comment));
    CHECK(!hasToken(url, "tex", "%20b} and \\href{http://y.io/%7Ez}{text} % real", TokenStyle::Comment));
    // An unclosed \url on its line is ordinary text again.
    CHECK(hasToken("\\url{a%b\nc}", "tex", "%b", TokenStyle::Comment));
    // \end {equation}, with a space, ends math as TeX does.
    std::string sp = "\\begin{equation}\n  x^2\n\\end {equation}\nafter prose\n";
    CHECK(hasToken(sp, "tex", "equation", TokenStyle::Type));
    bool afterIsMath = false;
    for (const Token &t : SyntaxHighlighter::highlight(sp, "tex"))
        if (t.style == TokenStyle::Number && t.start + t.length > sp.find("after")) afterIsMath = true;
    CHECK(!afterIsMath);
    // ...but a verbatim body ends only at the exact \end{verbatim}.
    std::string vsp = "\\begin{verbatim}\n\\end {verbatim}\n\\end{verbatim}\n";
    CHECK(hasToken(vsp, "tex", "\n\\end {verbatim}\n", TokenStyle::String));
    // In a package or class '@' is a letter: \verb@egroup is one command.
    std::string sty = "\\def\\verb@egroup{\\endgroup}\n";
    CHECK(hasToken(sty, "sty", "\\verb@egroup", TokenStyle::Keyword));
    CHECK(!hasStyle(sty, "sty", TokenStyle::String));
    CHECK(hasToken(sty, "cls", "\\verb@egroup", TokenStyle::Keyword));
    CHECK(hasToken("\\verb@b@", "tex", "@b@", TokenStyle::String));   // .tex unchanged
    // Arguments on the \begin line of minted and lstlisting are not body.
    std::string mint = "\\begin{minted}[linenos]{python}\nx = 1 % no\n\\end{minted}\n";
    CHECK(!hasToken(mint, "tex", "[linenos]{python}\nx = 1 % no\n", TokenStyle::String));
    CHECK(hasToken(mint, "tex", "\nx = 1 % no\n", TokenStyle::String));
    CHECK(hasToken("\\begin{minted}{c}\nint x;\n\\end{minted}", "tex",
                   "\nint x;\n", TokenStyle::String));
    std::string lst = "\\begin{lstlisting}[language=C]\nint y;\n\\end{lstlisting}\n";
    CHECK(hasToken(lst, "tex", "\nint y;\n", TokenStyle::String));
    // Plain verbatim has no arguments: a bracket there is body.
    CHECK(hasToken("\\begin{verbatim}[x]\ny\n\\end{verbatim}", "tex", "[x]\ny\n",
                   TokenStyle::String));
}

// Literals the lexer once got wrong (2026-11-04): JavaScript regexes and
// templates, raw strings in C++, Rust and Go, triple quotes, Rust lifetimes,
// C# verbatim strings, digit separators.
void testSyntaxLiterals() {
    // The style at each unit, '-' where no token is (TokenStyle order).
    auto styles = [](const std::string &text, const std::string &ext) {
        std::string m(text.size(), '-');
        const char *letters = "pktscnPf";
        for (const Token &t : SyntaxHighlighter::highlight(text, ext))
            for (size_t k = t.start; k < t.start + t.length && k < m.size(); ++k)
                m[k] = letters[(int)t.style];
        return m;
    };
    auto endStates = [](const std::string &text, const std::string &ext) {
        IncrementalHighlighter<char> h(ext);
        h.reset(StringSource<char>(text), nullptr);
        std::vector<LexState> v;
        for (size_t k = 0; k < h.lineCount(); ++k) v.push_back(h.endState(k));
        return v;
    };

    GROUP("syntax:js-regex");
    // The review's case: /* inside a regex opened a comment to the file's end.
    CHECK(styles("x = s.replace(/\\/*$/, \"\"); y = 1;", "js") ==
          "------fffffff-ssssss--ss-------n-");
    CHECK(hasToken("a = /\\/*$/;\nb = 1;\n", "js", "1", TokenStyle::Number));
    CHECK(!hasStyle("a = /\\/*$/;\nb = 1;\n", "js", TokenStyle::Comment));
    CHECK(endStates("a = /\\/*$/;\nb\n", "js")[0].kind == LexState::Normal);
    // A '/' in a class does not close it; flags belong to it; keywords
    // before a regex.
    CHECK(styles("if (/[/]/.test(s)) return /x/g;", "js") ==
          "kk--sssss-ffff-----kkkkkk-ssss-");
    CHECK(hasToken("x => /re/.test(x)", "js", "/re/", TokenStyle::String));
    CHECK(hasToken("f(a, /b/i)", "ts", "/b/i", TokenStyle::String));
    CHECK(hasToken("x = typeof /a/", "tsx", "/a/", TokenStyle::String));
    // Division: after a name, a number, ) ] and x++.
    CHECK(!hasStyle("a = b / c / d", "js", TokenStyle::String));
    CHECK(!hasStyle("a = (b) / c / d", "js", TokenStyle::String));
    CHECK(!hasStyle("a = b[1] / 2 / 3", "js", TokenStyle::String));
    CHECK(!hasStyle("i++ / 2 / 3", "js", TokenStyle::String));
    // Never over lines.
    CHECK(!hasStyle("a = /b\nc / d", "js", TokenStyle::String));
    {
        // A long line of regexes that never close (a class stays open) is
        // read once, not once per slash, which would be quadratic.
        std::string slashes;
        for (int k = 0; k < 50000; ++k) slashes += "(/[";
        CHECK(!hasStyle(slashes, "js", TokenStyle::String));
    }
    // JSX's closing tags and self-closing elements are not regexes.
    CHECK(styles("<li><a href=\"#\">Home</a></li>", "jsx") ==
          "------------sss-tttt---------");
    CHECK(!hasStyle("<Foo {...p} /><Bar x={1} />", "jsx", TokenStyle::String));
    // JSON has none: there a '/' is just punctuation.
    CHECK(!hasStyle("[1, /x/]", "json", TokenStyle::String));
    // A '//' inside a string is not a comment.
    CHECK(styles("const u = \"http://x\"; // c", "js") ==
          "kkkkk-----ssssssssss--cccc");

    GROUP("syntax:js-templates");
    // Fields are code with Keyword braces; templates nest inside them.
    CHECK(styles("const t = `a ${b + `c ${d}`} e`;", "js") ==
          "kkkkk-----ssskk----ssskk-ksksss-");
    // A '}' in a string inside a field does not close it.
    CHECK(styles("f(`x ${ \"}\" } y`)", "js") == "f-ssskk-sss-ksss-");
    // Objects in a field; an escaped ${ is text.
    CHECK(styles("n = `\\${not} ${o({a: 1})}`", "js") ==
          "----ssssssssskkf-----n--ks");
    // Over lines, with fields on the later lines and code after the close.
    {
        std::string t = "s = `line1\n${x} line2\nend` + 1\nq";
        CHECK(styles(t, "js") == "----ssssss" "s" "kk-kssssss" "s" "ssss---n" "-" "-");
        std::vector<LexState> st = endStates(t, "js");
        CHECK(st[0].kind == LexState::Template && st[1] == st[0]);
        CHECK(st[2].kind == LexState::Normal);
    }
    // A field open at a line's end carries on: the next line is code.
    {
        std::string t = "const s = `${items.map(x => {\n  return `<li>${x}</li>`;\n"
                        "}).join('')}`;\nconst y = 1;\n";
        CHECK(hasToken(t, "js", "return", TokenStyle::Keyword));
        CHECK(hasToken(t, "js", "''", TokenStyle::String));
        CHECK(hasToken(t, "js", "1", TokenStyle::Number));
        std::vector<LexState> st = endStates(t, "js");
        CHECK(st[0].kind == LexState::Template && st[1] == st[0]);
        CHECK(st[2].kind == LexState::Normal && st[3].kind == LexState::Normal);
    }
    // A template inside a field, itself over lines (lit's html`...`).
    {
        std::string t = "h = html`<ul>${items.map(i => html`\n  <li>${i}</li>\n"
                        "`)}</ul>`;\nz = 1\n";
        CHECK(hasToken(t, "js", "1", TokenStyle::Number));
        std::vector<LexState> st = endStates(t, "js");
        CHECK(st[0].kind == LexState::Template && st[1] == st[0]);
        CHECK(st[2].kind == LexState::Normal);
        // Same text, same state, in another highlighter too.
        CHECK(endStates("x = html`<ul>${items.map(i => html`\n", "ts")[0] == st[0]);
        CHECK(endStates("x = `\n", "js")[0] != st[0]);
    }
    // JSON keeps reading a backtick as a one-line string.
    CHECK(endStates("`a\nb`", "json")[0].kind == LexState::Normal);

    GROUP("syntax:cpp-raw");
    CHECK(hasToken("auto s = R\"(a \"quoted\" \\ back)\" + x;", "cpp",
                   "R\"(a \"quoted\" \\ back)\"", TokenStyle::String));
    {
        std::string t = "auto j = R\"json({\n  \"k\": \"v)\"\n})json\"; int y = 1;\n";
        CHECK(hasToken(t, "cpp", "R\"json({\n  \"k\": \"v)\"\n})json\"", TokenStyle::String));
        CHECK(hasToken(t, "cpp", "int", TokenStyle::Type));
        CHECK(hasToken(t, "cpp", "1", TokenStyle::Number));
        std::vector<LexState> st = endStates(t, "cpp");
        CHECK(st[0].kind == LexState::RawString && st[1] == st[0]);
        CHECK(st[2].kind == LexState::Normal);
        // The delimiter is part of the state: )x" does not close R"y(.
        CHECK(endStates("R\"x(\n", "cpp")[0] != endStates("R\"y(\n", "cpp")[0]);
        CHECK(endStates("R\"x(\n", "hpp")[0] == endStates("u8R\"x(\n", "cc")[0]);
        CHECK(hasToken("R\"y(\n)x\"\n)y\" z", "cpp", "R\"y(\n)x\"\n)y\"", TokenStyle::String));
    }
    // Prefixes; a 16-character delimiter; 17 is no raw string.
    CHECK(styles("u8R\"x(a)x\" LR\"(b)\" uR\"(c)\" UR\"(d)\"", "cpp") ==
          "ssssssssss-sssssss-sssssss-sssssss");
    CHECK(hasToken("R\"1234567890123456(a)1234567890123456\"", "cpp",
                   "R\"1234567890123456(a)1234567890123456\"", TokenStyle::String));
    CHECK(hasToken("R\"12345678901234567(no)\"", "cpp", "R", TokenStyle::Type));
    CHECK(hasToken("FooR\"(x)\"", "cpp", "FooR", TokenStyle::Type));
    // Only C++: C, C# and Java read R as a name.
    for (const char *e : {"c", "cs", "java"})
        CHECK(hasToken("s = R\"(x)\";", e, "R", TokenStyle::Type));
    // Digit separators: one number, not a character literal.
    CHECK(styles("int n = 1'000'000; char c = 'a';", "cpp") ==
          "ttt-----nnnnnnnnn--tttt-----sss-");
    CHECK(hasToken("x = 0xFF'FF + 0b1010'1010;", "c", "0xFF'FF", TokenStyle::Number));
    CHECK(hasToken("assign a = 4'b1010;", "v", "4'b1010", TokenStyle::Number));
    CHECK(hasToken("auto d = 100_km;", "cpp", "100_km", TokenStyle::Number));

    GROUP("syntax:rust");
    {
        std::string t = "fn f<'a>(x: &'a str) -> &'static str { 'outer: loop { "
                        "let c = 'x'; let d = '\\n'; let e = '\\u{1F600}'; let q = '\\''; } }";
        CHECK(hasToken(t, "rs", "'a", TokenStyle::Keyword));
        CHECK(hasToken(t, "rs", "'static", TokenStyle::Keyword));
        CHECK(hasToken(t, "rs", "'outer", TokenStyle::Keyword));
        CHECK(hasToken(t, "rs", "'x'", TokenStyle::String));
        CHECK(hasToken(t, "rs", "'\\n'", TokenStyle::String));
        CHECK(hasToken(t, "rs", "'\\u{1F600}'", TokenStyle::String));
        CHECK(hasToken(t, "rs", "'\\''", TokenStyle::String));
        // No string runs on from a lifetime.
        CHECK(!hasToken(t, "rs", "'a>(x: &'", TokenStyle::String));
        CHECK(hasToken("let c = '\xC3\xA9';", "rs", "'\xC3\xA9'", TokenStyle::String));
    }
    CHECK(hasToken("let r = r#\"a \"b\" c\"#; x", "rs", "r#\"a \"b\" c\"#", TokenStyle::String));
    CHECK(hasToken("let z = r##\"has \"# inside\"## + 1;", "rs",
                   "r##\"has \"# inside\"##", TokenStyle::String));
    CHECK(hasToken("let s = br\"x\\\"; y", "rs", "br\"x\\\"", TokenStyle::String));
    {
        // Rust's plain strings run over lines.
        std::string t = "let t = \"multi\nline \\\" still\nend\"; let u = 1_000u32;";
        CHECK(hasToken(t, "rs", "\"multi\nline \\\" still\nend\"", TokenStyle::String));
        CHECK(hasToken(t, "rs", "1_000u32", TokenStyle::Number));
        std::vector<LexState> st = endStates(t, "rs");
        CHECK(st[0].kind == LexState::RawString && st[2].kind == LexState::Normal);
    }

    GROUP("syntax:go-raw");
    {
        std::string t = "s := `http://x \"y\"\nz` + \"a\"\nc := 'x'";
        CHECK(!hasStyle(t, "go", TokenStyle::Comment));
        CHECK(hasToken(t, "go", "`http://x \"y\"\nz`", TokenStyle::String));
        CHECK(hasToken(t, "go", "\"a\"", TokenStyle::String));
        CHECK(hasToken(t, "go", "'x'", TokenStyle::String));
        CHECK(endStates(t, "go")[0].kind == LexState::RawString);
    }

    GROUP("syntax:triple-quoted");
    {
        // Java's text blocks: escapes, over lines.
        std::string t = "String s = \"\"\"\n    hello \"x\" \\\"\"\" still\n    \"\"\"; int y = 1;";
        CHECK(hasToken(t, "java", "\"\"\"\n    hello \"x\" \\\"\"\" still\n    \"\"\"",
                       TokenStyle::String));
        CHECK(hasToken(t, "java", "1", TokenStyle::Number));
        CHECK(hasToken("let s = \"\"\"\na \"b\"\n\"\"\"\nlet x = 1", "swift", "1",
                       TokenStyle::Number));
        // Kotlin's are raw: a backslash does not escape.
        CHECK(hasToken("val s = \"\"\"a\\\"\"\" + 1", "kt", "\"\"\"a\\\"\"\"", TokenStyle::String));
        CHECK(hasToken("val s = \"\"\"a\\\"\"\" + 1", "kt", "1", TokenStyle::Number));
        CHECK(hasToken("var s = '''a\nb''' + 1", "dart", "'''a\nb'''", TokenStyle::String));
        // An empty string is still an empty string.
        CHECK(hasToken("String s = \"\" + 1;", "java", "\"\"", TokenStyle::String));
        CHECK(hasToken("String s = \"\" + 1;", "java", "1", TokenStyle::Number));
    }
    // C#: verbatim strings, where a backslash is text, and raw strings.
    CHECK(styles("var p = @\"C:\\path\\\"; var q = 1;", "cs") ==
          "--------sssssssssss----------n-");
    CHECK(hasToken("var v = @$\"x {y}\";", "cs", "@$\"x {y}\"", TokenStyle::String));
    CHECK(hasToken("var r = \"\"\"\n  {\"a\": 1}\n  \"\"\"; var z = 2;", "cs", "2",
                   TokenStyle::Number));
    CHECK(hasToken("var r = \"\"\"\"x\"\"\"y\"\"\"\" + 3;", "cs", "3", TokenStyle::Number));

    GROUP("syntax:literal-misc");
    // A '#' in an f-string's field is not a comment (Python 3.12 allows them
    // only in fields over lines); one in a string in a field is text.
    CHECK(!hasStyle("f'{x # y}' + f\"{'#'}\"", "py", TokenStyle::Comment));
    CHECK(hasToken("n = 1_000_000 + 0x_FF", "py", "1_000_000", TokenStyle::Number));
    CHECK(hasToken("n = 1_000n", "js", "1_000n", TokenStyle::Number));
    // Shell has no separators; a plain file no grammar at all.
    CHECK(hasToken("x=1_000", "sh", "1", TokenStyle::Number));
    CHECK(SyntaxHighlighter::highlight("it's 1'000 and 1_000", "txt").size() == 3);
}

// ------------------------------------------------ incremental highlighting
// The line-at-a-time lexer must give exactly the old whole-file tokens, and
// the incremental highlighter must, after any edit, leave every line with
// exactly the tokens a fresh full lex gives. Checked by applying thousands of
// random edits to real files and comparing after each one.

std::string readFile(const char *path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Cut tokens at line boundaries, the way IncrementalHighlighter reports them:
// a piece ends just after a '\n' it covers.
template <class Ch>
std::vector<Token> splitAtLines(const std::vector<Token> &toks,
                                const std::basic_string<Ch> &text) {
    std::vector<Token> out;
    for (const Token &t : toks) {
        size_t s = t.start, e = t.start + t.length;
        while (s < e) {
            size_t nl = text.find(Ch('\n'), s);
            size_t cut = (nl == std::basic_string<Ch>::npos || nl + 1 > e) ? e : nl + 1;
            out.push_back({s, cut - s, t.style});
            s = cut;
        }
    }
    return out;
}

// One style per code unit, Plain where no token is.
std::vector<TokenStyle> styleMap(const std::vector<Token> &toks, size_t n) {
    std::vector<TokenStyle> m(n, TokenStyle::Plain);
    for (const Token &t : toks)
        for (size_t i = t.start; i < t.start + t.length && i < n; ++i) m[i] = t.style;
    return m;
}

// Minimal UTF-8 <-> UTF-16 for the test (input is valid UTF-8).
std::u16string toU16(const std::string &s) {
    std::u16string u;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp; int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if (c < 0xE0) { cp = c & 0x1F; len = 2; }
        else if (c < 0xF0) { cp = c & 0x0F; len = 3; }
        else { cp = c & 0x07; len = 4; }
        for (int k = 1; k < len && i + k < s.size(); ++k)
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            u += (char16_t)(0xD800 + (cp >> 10));
            u += (char16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            u += (char16_t)cp;
        }
    }
    return u;
}
std::string toU8(const std::u16string &u, std::vector<size_t> *u16AtByte) {
    std::string s;
    if (u16AtByte) u16AtByte->clear();
    for (size_t i = 0; i < u.size();) {
        uint32_t cp = u[i];
        size_t units = 1;
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < u.size()) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            units = 2;
        }
        size_t before = s.size();
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
        } else {
            s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F));
            s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F));
        }
        if (u16AtByte) for (size_t k = before; k < s.size(); ++k) u16AtByte->push_back(i);
        i += units;
    }
    if (u16AtByte) u16AtByte->push_back(u.size());
    return s;
}

// Grammars the frozen lexer never had. For these the reference is a full lex
// by the new highlighter (every line in order from the top), so the random
// edits still check the incremental bookkeeping, and the UTF-16 run checks
// that UTF-16 gives the same tokens as UTF-8.
bool newGrammar(const std::string &ext) {
    return ext == "tex" || ext == "ltx" || ext == "latex" || ext == "sty" ||
           ext == "cls" || ext == "bib" ||
           // C-family languages whose strings the frozen lexer read as C's
           // (raw strings, triple quotes, Rust's lifetimes): see mayHoldNewLiteral.
           ext == "rs" || ext == "go" || ext == "cs" || ext == "swift" ||
           ext == "kt" || ext == "java";
}
// Text the frozen lexer read differently since literals were added
// (2026-11-04), for the grammars it is still compared against: a number with
// a digit separator (1_000 in Python, JavaScript and C++, 1'000 in C++), a
// C++ raw string (R"), and in JavaScript a template literal (any backtick)
// or a '/' that might start a regex (one that opens no comment and has
// another '/' after it on its line). Such text is checked against a full
// lex instead.
bool mayHoldNewLiteral(const std::string &text, const std::string &ext) {
    const bool js = ext == "js", cpp = ext == "cpp";
    if (!js && !cpp && ext != "py") return false;
    auto ident = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
    for (size_t i = 0; i + 1 < text.size(); ++i) {
        char c = text[i], d = text[i + 1];
        bool sep = d == '_' || (cpp && d == '\'');
        if (sep && (std::isalnum((unsigned char)c) || c == '.')) {
            // Might c be part of a number: does a digit start one in the run
            // of letters, digits, '_' and '.' that ends at c?
            size_t j = i;
            while (j > 0 && (ident(text[j - 1]) || text[j - 1] == '.')) j--;
            for (size_t k = j; k <= i; ++k)
                if (std::isdigit((unsigned char)text[k]) && (k == j || !ident(text[k - 1])))
                    return true;
        }
        if (cpp && c == 'R' && d == '"') return true;
        if (js && c == '`') return true;
        if (js && c == '/' && d != '/' && d != '*') {
            size_t eol = text.find('\n', i + 1);
            if (text.find('/', i + 1) < eol) return true;
        }
    }
    return js && !text.empty() && text.back() == '`';
}
// Python text that may hold an f-string (a quote after an f or t prefix),
// which the frozen lexer read as a plain string. Such text is checked
// against a full lex too.
bool mayHoldFString(const std::string &text, const std::string &ext) {
    if (ext != "py") return false;
    for (size_t i = 1; i < text.size(); ++i) {
        if (text[i] != '\'' && text[i] != '"') continue;
        size_t j = i;
        while (j > 0 && i - j < 3 && std::isalpha((unsigned char)text[j - 1])) j--;
        for (size_t k = j; k < i; ++k)
            if (std::tolower((unsigned char)text[k]) == 'f' ||
                std::tolower((unsigned char)text[k]) == 't') return true;
    }
    return false;
}
std::vector<Token> fullLex(const std::string &text, const std::string &ext) {
    return newGrammar(ext) || mayHoldFString(text, ext) || mayHoldNewLiteral(text, ext)
               ? SyntaxHighlighter::highlight(text, ext)
               : legacy::highlight(text, ext);
}

// The reference tokens for a document: the frozen whole-file lexer (or a full
// lex, for a grammar it lacks), cut at lines, in the document's own units.
std::vector<Token> reference(const std::string &text, const std::string &ext) {
    return splitAtLines(fullLex(text, ext), text);
}
std::vector<Token> reference(const std::u16string &text, const std::string &ext) {
    std::vector<size_t> map;
    std::string u8 = toU8(text, &map);
    std::vector<Token> toks;
    for (const Token &t : fullLex(u8, ext)) {
        size_t a = map[t.start], b = map[t.start + t.length];
        if (b > a) toks.push_back({a, b - a, t.style});
    }
    return splitAtLines(toks, text);
}

// Fragments that open and close every multi-line construct, plus ordinary
// code and non-ASCII text.
const char *const kFragments[] = {
    "/*", "*/", "\"", "'", "`", "\"\"\"", "'''", "\\", "\\\n", "\n", "\n\n",
    "#", "//", "@", "# ", "#include <x>\n", "x(", "foo (", "Bar", "0x1F", "3.14",
    ".5", " ", "\t", "\r\n", "if ", "return ", "def ", "self", "é", "日本",
    "\xF0\x9F\x98\x80", "/* c */", "\"s\"", "a = \"b\\\"c\"", "*/\n", "/*\n",
    "\"\"\"doc\n", "x", "}", "{\n",
    // Python f-strings: fields, literal braces, conversions, specs.
    "f'", "f\"", "rf'", "f\"\"\"", "{x}", "{{", "}}", "!r", ":>8", "{",
    // TeX: math, comments, verbatim and the environments that carry state.
    "$", "$$", "\\(", "\\)", "\\[", "\\]", "%", "\\%", "\\\\", "\\alpha",
    "\\section{A}", "\\begin{verbatim}", "\\end{verbatim}", "\\begin{equation}\n",
    "\\end{equation}", "\\begin{comment}", "\\end{comment}", "\\verb|x|",
    "\\verb", "|", "@article{", "\\begin{itemize}", "\n \n",
    "\\end {equation}", "\\verb@egroup", "\\verb@b@", "\\url{a%2F}", "\\url{",
    "\\href{h%7E}{t}", "@book(", ")", "(", "10.1000%2Fabc", "\\begin{minted}[n]{py}",
    "\\end{minted}", "\\begin{lstlisting}[language=C]", "\\end{lstlisting}", "{",
    "\\begin{align}", "\\end{align}",
    // Literals added 2026-11-04: JavaScript regexes and templates, C++ and
    // Rust raw strings, Rust lifetimes, Go raw strings, triple quotes, C#
    // verbatim strings, digit separators.
    "/a/g", "= /[/*]/", "/\\/*$/", "return /x/", ") / 2 /", "`a${", "${x}", "${",
    "}`", "`${`", "R\"(", ")\"", "R\"d(", ")d\"", "u8R\"x(\"", "r#\"", "\"#",
    "'a", "'x'", "'\\n'", "1'000", "1_000", "@\"", "\"\"\"\n", "\\\"",
};

template <class Ch>
std::basic_string<Ch> fromUtf8(const std::string &s);
template <> std::string fromUtf8<char>(const std::string &s) { return s; }
template <> std::u16string fromUtf8<char16_t>(const std::string &s) { return toU16(s); }

// Keep a split never landing inside a UTF-8 sequence or surrogate pair, so
// the UTF-16 reference stays convertible. (Edits in UTF-8 may split
// sequences; the lexer treats stray bytes as punctuation either way.)
size_t snap(const std::u16string &t, size_t p) {
    if (p > 0 && p < t.size() && t[p] >= 0xDC00 && t[p] < 0xE000) --p;
    return p;
}
size_t snap(const std::string &, size_t p) { return p; }

struct FuzzStats { size_t edits = 0, linesRelexed = 0, lineTotal = 0; };

// The line holding offset `off`.
template <class Ch>
size_t lineOf(const IncrementalHighlighter<Ch> &h, size_t off) {
    size_t lo = 0, hi = h.lineCount();   // first line starting after off
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (h.lineStart(mid) <= off) lo = mid + 1; else hi = mid;
    }
    return lo - 1;
}

// Apply `rounds` random edits to `text`, checking the incremental result
// against a full lex after each one. Returns false (and prints) on the first
// mismatch.
template <class Ch>
bool fuzzIncremental(std::basic_string<Ch> text, const std::string &ext,
                     unsigned seed, int rounds, FuzzStats &stats) {
    using Str = std::basic_string<Ch>;
    std::mt19937 rng(seed);
    auto rnd = [&](size_t n) { return n ? (size_t)(rng() % n) : 0; };

    IncrementalHighlighter<Ch> h(ext);
    std::vector<Token> all;
    h.reset(StringSource<Ch>(text), &all);
    if (all != reference(text, ext)) {
        std::printf("    reset differs from a full lex (%s)\n", ext.c_str());
        return false;
    }
    // The GUI's view: tokens per line, relative to the line start (they move
    // with the text), and a style per unit (attributes move with the text).
    std::vector<std::vector<Token>> perLine(h.lineCount());
    for (const Token &t : all) {
        size_t line = lineOf(h, t.start);
        perLine[line].push_back({t.start - h.lineStart(line), t.length, t.style});
    }
    std::vector<TokenStyle> styles = styleMap(all, text.size());

    const size_t nFrag = sizeof(kFragments) / sizeof(kFragments[0]);
    for (int round = 0; round < rounds; ++round) {
        // Build the edit: sometimes two separate edits reported as one range.
        size_t pos = snap(text, rnd(text.size() + 1));
        size_t maxDel = text.size() - pos;
        size_t oldLen = 0;
        switch (rnd(4)) {
        case 0: oldLen = 0; break;
        case 1: oldLen = std::min<size_t>(maxDel, 1 + rnd(3)); break;
        case 2: oldLen = std::min<size_t>(maxDel, rnd(40)); break;
        default: oldLen = std::min<size_t>(maxDel, rnd(400)); break;
        }
        oldLen = snap(text, pos + oldLen) - pos;
        Str ins;
        size_t kind = rnd(10);
        if (kind < 6) {
            for (size_t k = 0, m = 1 + rnd(3); k < m; ++k)
                ins += fromUtf8<Ch>(kFragments[rnd(nFrag)]);
        } else if (kind < 8 && !text.empty()) {          // paste a slice
            size_t a = snap(text, rnd(text.size()));
            size_t b = snap(text, std::min(text.size(), a + rnd(300)));
            if (b > a) ins = text.substr(a, b - a);
        }                                                 // else: pure delete
        size_t newLen = ins.size();
        Str before = text;
        text.replace(pos, oldLen, ins);

        size_t repPos = pos, repOld = oldLen, repNew = newLen;
        if (rnd(8) == 0 && pos + newLen < text.size()) {
            // A second edit further on, reported together with the first.
            size_t p2 = snap(text, pos + newLen + rnd(text.size() - pos - newLen));
            size_t d2 = snap(text, std::min(text.size(), p2 + rnd(5))) - p2;
            Str ins2 = fromUtf8<Ch>(kFragments[rnd(nFrag)]);
            text.replace(p2, d2, ins2);
            size_t endNew = p2 + ins2.size();
            repNew = endNew - pos;
            repOld = repNew + before.size() - text.size();
        }

        size_t oldLines = h.lineCount();
        std::vector<Token> got;
        auto r = h.edit(StringSource<Ch>(text), repPos, repOld, repNew, got);
        stats.edits++;
        stats.linesRelexed += r.endLine - r.firstLine;
        stats.lineTotal += h.lineCount();

        // Per-line model: replace the re-lexed lines, keep the rest.
        size_t delta = h.lineCount() - oldLines;   // wraps when lines were removed
        std::vector<std::vector<Token>> next;
        next.reserve(h.lineCount());
        for (size_t k = 0; k < r.firstLine; ++k) next.push_back(perLine[k]);
        for (size_t k = r.firstLine; k < r.endLine; ++k) next.emplace_back();
        for (const Token &t : got) {
            size_t line = lineOf(h, t.start);
            if (line < r.firstLine || line >= r.endLine) {
                std::printf("    token outside the re-lexed lines\n");
                return false;
            }
            next[line].push_back({t.start - h.lineStart(line), t.length, t.style});
        }
        for (size_t k = r.endLine; k < h.lineCount(); ++k)
            next.push_back(perLine[k - delta]);
        perLine.swap(next);

        // Asking again for the same lines, from the stored states, agrees.
        std::vector<Token> again;
        h.lineTokens(StringSource<Ch>(text), r.firstLine, r.endLine, again);
        if (again != got || lineOf(h, repPos) != h.lineOf(repPos)) {
            std::printf("    lineTokens/lineOf disagree with edit()\n");
            return false;
        }

        std::vector<Token> flat;
        for (size_t k = 0; k < perLine.size(); ++k)
            for (const Token &t : perLine[k])
                flat.push_back({t.start + h.lineStart(k), t.length, t.style});
        std::vector<Token> want = reference(text, ext);

        // Style-per-unit model, the way NSTextStorage keeps attributes.
        styles.erase(styles.begin() + repPos, styles.begin() + repPos + repOld);
        styles.insert(styles.begin() + repPos, repNew, TokenStyle::Plain);
        std::fill(styles.begin() + r.start, styles.begin() + r.end, TokenStyle::Plain);
        for (const Token &t : got)
            std::fill(styles.begin() + t.start, styles.begin() + t.start + t.length, t.style);

        IncrementalHighlighter<Ch> fresh(ext);
        fresh.reset(StringSource<Ch>(text), nullptr);
        bool statesOk = fresh.lineCount() == h.lineCount();
        for (size_t k = 0; statesOk && k < h.lineCount(); ++k)
            statesOk = fresh.endState(k) == h.endState(k) &&
                       fresh.lineStart(k) == h.lineStart(k);

        if (flat != want || styles != styleMap(want, text.size()) || !statesOk ||
            h.length() != text.size()) {
            std::printf("    %s: edit %d (pos %zu, -%zu +%zu) diverged: tokens %s, "
                        "styles %s, states %s\n", ext.c_str(), round, repPos, repOld,
                        repNew, flat == want ? "ok" : "DIFFER",
                        styles == styleMap(want, text.size()) ? "ok" : "DIFFER",
                        statesOk ? "ok" : "DIFFER");
            return false;
        }
    }
    return true;
}

void testIncrementalHighlight() {
    struct Sample { std::string name, text; };
    std::vector<Sample> samples;
    for (const char *p : {"demo/sample.cpp", "demo/hello.py", "demo/README.md",
                          "src/SyntaxHighlighter.cpp", "src/LatexDoc.cpp",
                          "scripts/release.sh", "tests/LegacyHighlighter.h",
                          "demo/paper/notes.tex", "tests/latex/torture.tex"}) {
        std::string t = readFile(p);
        GROUP("incremental:samples");
        CHECK(!t.empty());   // run from the repo root (make test does)
        if (!t.empty()) samples.push_back({p, t});
    }
    samples.push_back({"js", "// js\nconst a = `tpl\nline` + 'x';\n/* multi\n"
                             " line */ function f(x) { return x * 2; }\n"
                             "let s = \"esc \\\n continued\";\n"});
    samples.push_back({"py", "def f():\n    '''doc\n    more'''\n    s = \"\"\"a\n\"\"\"\n"
                             "@dec\nclass K: pass  # note\nx = 'a\\\nb'\n"});
    samples.push_back({"conf", "# settings\nwindow.background = #1E1E1E\n"
                               "key = \"quoted # not comment\"\n"});
    samples.push_back({"empty", ""});
    // The literals of 2026-11-04, every kind in one text (read as each
    // language in turn, so each grammar sees the others' as noise).
    samples.push_back({"literals",
        "s = x.replace(/\\/*$/, '') + `a ${b + `c ${d}`}\nline ${e({f: 1})}` / 2;\n"
        "h = html`<ul>${items.map(i => html`\n  <li>${i}</li>\n`)}</ul>`;\n"
        "auto j = R\"json({\n  \"k\": \"v)\" \xC3\xA9\n})json\" + u8R\"(x)\"; int n = 1'000;\n"
        "fn f<'a>(x: &'a str) { let c = '\xC3\xA9'; let r = r#\"a \"b\"\n\"#; }\n"
        "s := `raw // \"q\"\n` + \"\"\"\ntext \\\"\"\"\n\"\"\" + @\"C:\\x\\\" + 1_000\n"});
    samples.push_back({"unicode", "s = \"h\xC3\xA9llo \xF0\x9F\x98\x80\"; /* \xE6\x97\xA5\n"
                                  "\xE6\x9C\xAC */ int caf\xC3\xA9(x);\n"});
    samples.push_back({"tex",
        "\\documentclass{article} % class\n\\usepackage{amsmath}\n"
        "\\begin{document}\n\\section{Caf\xC3\xA9} Price: 50\\%, $x^2 + \\alpha$\n"
        "and $a\nb$ over lines, \\(c\\) and $$\nd\n$$ then\n\\[\n  e = \\frac{1}{2}\n\\]\n"
        "\\begin{equation}\n  E = mc^2 % energy\n\n  lost\n\\end{equation}\n"
        "\\begin{verbatim}\n$ not math % not comment \\[\n\\end{verbatim}\n"
        "\\begin{lstlisting}[language=C]\nint x; // \xE6\x97\xA5\n\\end{lstlisting}\n"
        "\\verb|$%| and \\verb*+\\[+ \\verb|open\n"
        "\\begin{comment}\nhidden $\n\\end{comment}\n"
        "@article{k, title = {On $x$}}\n\\end{document}\n"});
    // The old-lexer comparison covers only the grammars it had; the random
    // edits also run TeX, checked against a full lex (see newGrammar).
    const char *exts[] = {"cpp", "py", "js", "sh", "txt"};
    const char *fuzzExts[] = {"cpp", "py", "js", "sh", "txt", "tex", "bib", "sty",
                              "rs", "go", "cs", "swift", "kt"};

    GROUP("incremental:matches-old-lexer");
    // The line lexer changed no token anywhere, joined back into whole tokens.
    bool same = true;
    for (const Sample &s : samples)
        for (const char *e : exts)
            if (!mayHoldFString(s.text, e) && !mayHoldNewLiteral(s.text, e) &&
                SyntaxHighlighter::highlight(s.text, e) != legacy::highlight(s.text, e)) {
                std::printf("    %s as %s differs from the old lexer\n", s.name.c_str(), e);
                same = false;
            }
    CHECK(same);
    // Random soup of the tricky fragments, too.
    {
        std::mt19937 rng(7);
        bool soup = true;
        for (int k = 0; k < 400; ++k) {
            std::string t;
            for (int m = 0, len = 1 + (int)(rng() % 60); m < len; ++m)
                t += kFragments[rng() % (sizeof(kFragments) / sizeof(kFragments[0]))];
            for (const char *e : exts)
                soup = soup && (mayHoldFString(t, e) || mayHoldNewLiteral(t, e) ||
                                SyntaxHighlighter::highlight(t, e) == legacy::highlight(t, e));
        }
        CHECK(soup);
    }

    GROUP("incremental:line-states");
    {
        IncrementalHighlighter<char> h("cpp");
        std::string t = "a /* b\nc\nd */ e\n\"x\\\ny\"\n";
        h.reset(StringSource<char>(t), nullptr);
        CHECK(h.lineCount() == 6);
        CHECK(h.endState(0).kind == LexState::BlockComment);
        CHECK(h.endState(1).kind == LexState::BlockComment);
        CHECK(h.endState(2).kind == LexState::Normal);
        CHECK(h.endState(3).kind == LexState::StringCont && h.endState(3).quote == u'"');
        CHECK(h.endState(4).kind == LexState::Normal);
        IncrementalHighlighter<char> p("py");
        std::string py = "x = '''a\nb''' + \"\"\"c\n";
        p.reset(StringSource<char>(py), nullptr);
        CHECK(p.endState(0).kind == LexState::TripleString && p.endState(0).quote == u'\'');
        CHECK(p.endState(1).kind == LexState::TripleString && p.endState(1).quote == u'"');

        IncrementalHighlighter<char> x("tex");
        std::string tx = "$a\nb$ \\[\nc\n\\] \\begin{verbatim}\n\n$\n\\end{verbatim} "
                         "\\begin{align}\nx\n\n$$\n$$ \\end{align}\n\\(\n";
        x.reset(StringSource<char>(tx), nullptr);
        CHECK(x.lineCount() == 13);
        CHECK(x.endState(0).kind == LexState::TexMath && x.endState(0).quote == u'$');
        CHECK(x.endState(1).kind == LexState::TexMath && x.endState(1).quote == u']');
        CHECK(x.endState(2).kind == LexState::TexMath && x.endState(2).quote == u']');
        CHECK(x.endState(3).kind == LexState::TexEnv);           // verbatim
        CHECK(x.endState(4) == x.endState(3));                    // blank: still verbatim
        CHECK(x.endState(5) == x.endState(3));                    // $ is text there
        CHECK(x.endState(6).kind == LexState::TexEnv && x.endState(6) != x.endState(3));
        CHECK(x.endState(7) == x.endState(6));                    // align
        CHECK(x.endState(8).kind == LexState::Normal);            // blank ends math
        CHECK(x.endState(9).kind == LexState::TexMath && x.endState(9).quote == u'D');
        CHECK(x.endState(10).kind == LexState::Normal);
        CHECK(x.endState(11).kind == LexState::TexMath && x.endState(11).quote == u')');
        CHECK(x.endState(12).kind == LexState::Normal);           // empty last line
    }

    GROUP("incremental:edit-scope");
    {
        // Typing a letter re-lexes one line; opening a comment re-lexes to the
        // end; closing it again stops as soon as the states agree.
        std::string t;
        for (int k = 0; k < 200; ++k) t += "int x = 1; // line\n";
        IncrementalHighlighter<char> h("cpp");
        h.reset(StringSource<char>(t), nullptr);
        std::vector<Token> out;
        t.insert(100, "y");
        auto r = h.edit(StringSource<char>(t), 100, 0, 1, out);
        CHECK(r.endLine - r.firstLine == 1);
        CHECK(r.start == h.lineStart(r.firstLine) && r.end == h.lineStart(r.firstLine + 1));
        out.clear();
        t.insert(40, "/*");
        r = h.edit(StringSource<char>(t), 40, 0, 2, out);
        CHECK(r.firstLine == 2 && r.endLine == h.lineCount());
        CHECK(!out.empty() && out.back().style == TokenStyle::Comment);
        out.clear();
        t.insert(80, "*/");
        r = h.edit(StringSource<char>(t), 80, 0, 2, out);
        CHECK(r.endLine == h.lineCount());   // everything below uncomments
        out.clear();
        t.erase(80, 2);
        r = h.edit(StringSource<char>(t), 80, 2, 0, out);
        CHECK(r.endLine == h.lineCount());
        out.clear();
        t.erase(40, 2);
        r = h.edit(StringSource<char>(t), 40, 2, 0, out);
        CHECK(r.endLine == h.lineCount());
        // A report that does not add up falls back to a full lex.
        out.clear();
        r = h.edit(StringSource<char>(t), 0, 5, 0, out);
        CHECK(r.firstLine == 0 && r.endLine == h.lineCount() && r.end == t.size());
        CHECK(out == reference(t, "cpp"));
    }
    {
        // In TeX an unclosed $ runs only to the paragraph's end, so typing
        // one re-lexes the paragraph, not the rest of the document.
        std::string t;
        for (int k = 0; k < 50; ++k) t += "Some prose with \\emph{a} word.\nMore.\n\n";
        IncrementalHighlighter<char> h("tex");
        h.reset(StringSource<char>(t), nullptr);
        std::vector<Token> out;
        t.insert(5, "$");
        auto r = h.edit(StringSource<char>(t), 5, 0, 1, out);
        CHECK(r.firstLine == 0 && r.endLine == 3);
        CHECK(h.endState(1).kind == LexState::TexMath && h.endState(2).kind == LexState::Normal);
        out.clear();
        t.insert(t.find("word"), "$");
        r = h.edit(StringSource<char>(t), t.find("$word"), 0, 1, out);
        CHECK(r.firstLine == 0 && r.endLine == 3);
        CHECK(h.endState(0).kind == LexState::Normal && h.endState(1).kind == LexState::Normal);
    }

    GROUP("incremental:random-edits");
    FuzzStats stats;
    unsigned seed = 1;
    for (const Sample &s : samples) {
        for (const char *e : fuzzExts) {
            bool ok = fuzzIncremental<char>(s.text, e, seed++, 120, stats);
            if (!ok) std::printf("    (UTF-8, sample %s)\n", s.name.c_str());
            CHECK(ok);
            ok = fuzzIncremental<char16_t>(toU16(s.text), e, seed++, 120, stats);
            if (!ok) std::printf("    (UTF-16, sample %s)\n", s.name.c_str());
            CHECK(ok);
        }
    }
    std::printf("  incremental: %zu random edits matched a full lex; "
                "%.1f lines re-lexed per edit (of %.0f)\n", stats.edits,
                (double)stats.linesRelexed / stats.edits,
                (double)stats.lineTotal / stats.edits);
}

// Timing on a big file: a full lex against one keystroke. Printed, not
// checked, since machines differ; the edit-scope checks above are the
// deterministic part.
void benchIncrementalHighlight() {
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    };
    std::string unit = readFile("demo/sample.cpp");
    if (unit.empty()) return;
    std::u16string text;
    {
        std::string big;
        size_t lines = 0;
        while (lines < 100000) {
            big += unit;
            // No block comments, so a "/*" typed at the top runs to the end.
            big += "// note\nstatic const char *s = \"text\";\n";
            lines += 11;
        }
        text = toU16(big);
    }
    size_t nLines = std::count(text.begin(), text.end(), u'\n');

    IncrementalHighlighter<char16_t> h("cpp");
    auto t0 = Clock::now();
    std::vector<Token> all;
    h.reset(StringSource<char16_t>(text), &all);
    double full = ms(Clock::now() - t0);

    // One keystroke at a time in the middle, then deleted again.
    const int kEdits = 2000;
    std::vector<Token> out;
    size_t pos = text.size() / 2;
    t0 = Clock::now();
    for (int k = 0; k < kEdits; ++k) {
        out.clear();
        if (k % 2 == 0) { text.insert(pos, 1, u'x'); h.edit(StringSource<char16_t>(text), pos, 0, 1, out); }
        else { text.erase(pos, 1); h.edit(StringSource<char16_t>(text), pos, 1, 0, out); }
    }
    double perKey = ms(Clock::now() - t0) / kEdits;

    // Worst case: open a block comment near the top, then close it.
    size_t top = text.find(u'\n') + 1;   // line 2 (line 1 is an #include)
    out.clear();
    t0 = Clock::now();
    text.insert(top, u"/*");
    auto r = h.edit(StringSource<char16_t>(text), top, 0, 2, out);
    double openAll = ms(Clock::now() - t0);
    size_t openLines = r.endLine - r.firstLine;
    out.clear();
    t0 = Clock::now();
    text.erase(top, 2);
    h.edit(StringSource<char16_t>(text), top, 2, 0, out);
    double closeAll = ms(Clock::now() - t0);

    std::printf("  bench: %zu lines of C++ (%zu tokens): full lex %.1f ms, "
                "one keystroke %.4f ms; typing /* at the top %.1f ms (%zu lines), "
                "deleting it %.1f ms\n",
                nLines, all.size(), full, perKey, openAll, openLines, closeAll);
}

// --------------------------------------------------------------- markdown
bool anyRun(const std::vector<MdRun> &runs,
            bool (*pred)(const MdRun &)) {
    for (const MdRun &r : runs) if (pred(r)) return true;
    return false;
}

// Join the table runs into display lines (the text a user actually sees).
std::vector<std::string> tableLines(const std::vector<MdRun> &runs) {
    std::vector<std::string> lines;
    std::string cur;
    for (const MdRun &r : runs) {
        if (!r.table) continue;
        for (char c : r.text) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else cur += c;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// Display columns of every column divider (│ in body rows, ┼ in the rule).
// Counts code points, with CJK ideographs as two columns.
std::vector<size_t> dividerColumns(const std::string &line) {
    std::vector<size_t> cols;
    size_t col = 0;
    for (size_t i = 0; i < line.size();) {
        unsigned char c = (unsigned char)line[i];
        int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        std::string ch = line.substr(i, len);
        if (ch == "\xE2\x94\x82" || ch == "\xE2\x94\xBC") cols.push_back(col);
        col += (len == 3 && c >= 0xE3) ? 2 : 1;   // U+3000.. are wide here
        i += len;
    }
    return cols;
}

// Every row puts its dividers in the same screen columns.
bool columnsAligned(const std::vector<std::string> &lines) {
    if (lines.empty()) return false;
    auto first = dividerColumns(lines[0]);
    for (const auto &l : lines)
        if (dividerColumns(l) != first) return false;
    return true;
}

void testMarkdownEdit() {
    using namespace MarkdownEdit;
    const std::string doc =
        "# Title here\n"            // 0
        "\n"                        // 1
        "First line of a\n"         // 2
        "paragraph, two lines.\n"   // 3
        "\n"                        // 4
        "- apple\n"                 // 5
        "  - pear\n"                // 6
        "3. third\n"                // 7
        "\n"                        // 8
        "> quoted\n"                // 9
        "> more\n"                  // 10
        "\n"                        // 11
        "```\n"                     // 12
        "x = 1\n"                   // 13
        "y = 2\n"                   // 14
        "```\n"                     // 15
        "| Key | Mac |\n"            // 16
        "| --- | --- |\n"            // 17
        "| Save | Cmd S |\n"         // 18
        "|  | x |\n"                 // 19
        "\n"                        // 20
        "---\n";                    // 21
    auto text = [&](const Block &b) { return doc.substr(b.start, b.end - b.start); };

    GROUP("mdedit:blocks");
    Block h = blockAt(doc, 0);
    CHECK(h.kind == Block::Heading && text(h) == "Title here");
    Block p = blockAt(doc, 3);
    CHECK(p.kind == Block::Paragraph && text(p) == "First line of a\nparagraph, two lines.");
    CHECK(p.firstLine == 2 && p.lastLine == 3);
    Block li = blockAt(doc, 5);
    CHECK(li.kind == Block::ListItem && text(li) == "apple" && li.nextItemPrefix == "- ");
    Block nested = blockAt(doc, 6);
    CHECK(nested.kind == Block::ListItem && text(nested) == "pear" && nested.nextItemPrefix == "  - ");
    Block num = blockAt(doc, 7);
    CHECK(num.kind == Block::ListItem && text(num) == "third" && num.nextItemPrefix == "4. ");
    Block q = blockAt(doc, 10);
    CHECK(q.kind == Block::Quote && text(q) == "> quoted\n> more");
    Block code = blockAt(doc, 13);
    CHECK(code.kind == Block::Code && text(code) == "x = 1\ny = 2");
    Block cell = blockAt(doc, 18, 1);
    CHECK(cell.kind == Block::TableCell && text(cell) == "Cmd S");
    Block head = blockAt(doc, 16, 0);
    CHECK(head.kind == Block::TableCell && text(head) == "Key");
    Block row = blockAt(doc, 18);
    CHECK(row.kind == Block::TableRow && text(row) == "| Save | Cmd S |");
    CHECK(blockAt(doc, 18, 7).kind == Block::TableRow);
    for (int none : {1, 12, 15, 17, 21, 99, -1})
        CHECK(blockAt(doc, none).kind == Block::None);

    GROUP("mdedit:edits");
    CHECK(replace(doc, h, "New").rfind("# New\n\nFirst", 0) == 0);
    std::string r = replace(doc, cell, "Command S");
    CHECK(r.find("| Save | Command S |\n") != std::string::npos);
    // Newlines and bare pipes cannot go into a cell as they are.
    r = replace(doc, cell, "a|b\nc");
    CHECK(r.find("| Save | a\\|b c |\n") != std::string::npos);
    CHECK(replace(doc, cell, "a\\|b").find("| Save | a\\|b |") != std::string::npos);
    Block empty = blockAt(doc, 19, 0);
    CHECK(empty.kind == Block::TableCell && empty.start == empty.end);
    CHECK(replace(doc, empty, "new").find("| new| x |\n") != std::string::npos ||
          replace(doc, empty, "new").find("| new | x |\n") != std::string::npos);
    // Only the block's bytes change.
    std::string pr = replace(doc, p, "One line now.");
    CHECK(pr == doc.substr(0, p.start) + "One line now." + doc.substr(p.end));
    size_t at = 0;
    std::string added = addItem(doc, num, "fourth", &at);
    CHECK(added.find("3. third\n4. fourth\n\n> quoted") != std::string::npos);
    CHECK(added.substr(at, 6) == "fourth");
    CHECK(addItem(doc, nested, "plum").find("  - pear\n  - plum\n3.") != std::string::npos);
    CHECK(addItem(doc, p, "nope") == doc);

    GROUP("mdedit:agrees-with-parser");
    // Every run the parser stamped with a line maps back to a block (or to
    // nothing only for the lines that render as nothing editable).
    auto runs = MarkdownParser::parse(doc);
    bool ok = true;
    for (const MdRun &run : runs) {
        if (run.text.find_first_not_of(" \n") == std::string::npos || run.rule) continue;
        if (run.table && run.tableCol < 0) continue;
        int line = run.line;
        if (run.table) line = run.line + run.tableRow + (run.tableRow > 0 ? 1 : 0);
        Block b = blockAt(doc, line, run.table ? run.tableCol : -1);
        if (b.kind == Block::None) { ok = false; printf("  no block for run '%s' line %d\n", run.text.c_str(), line); }
    }
    CHECK(ok);

    GROUP("mdedit:blocks-more");
    const std::string more =
        "- item one\n"          // 0
        "  continues here\n"    // 1
        "- two\n"               // 2
        "\n"                    // 3
        "Setext title\n"        // 4
        "============\n"        // 5
        "\n"                    // 6
        "## Closed ##\n"        // 7
        "\n"                    // 8
        "> q1\n"                // 9
        ">\n"                   // 10
        "> q2\n"                // 11
        "\n"                    // 12
        "[ref]: /x\n"           // 13
        "\n"                    // 14
        "    indented\n"        // 15
        "    code\n"            // 16
        "\n"                    // 17
        "- [ ] task\n"          // 18
        "<!-- note -->\n";      // 19
    auto textOf = [&](const Block &b) { return more.substr(b.start, b.end - b.start); };
    // A list item takes its continuation lines with it.
    Block cont = blockAt(more, 1);
    CHECK(cont.kind == Block::ListItem && textOf(cont) == "item one\n  continues here");
    CHECK(cont.firstLine == 0 && cont.lastLine == 1);
    CHECK(blockAt(more, 0).end == cont.end);
    CHECK(addItem(more, cont, "new").find("continues here\n- new\n- two\n") != std::string::npos);
    Block setext = blockAt(more, 4);
    CHECK(setext.kind == Block::Heading && textOf(setext) == "Setext title");
    CHECK(blockAt(more, 5).kind == Block::None);
    Block closed = blockAt(more, 7);
    CHECK(closed.kind == Block::Heading && textOf(closed) == "Closed");
    CHECK(replace(more, closed, "Open").find("## Open ##\n") != std::string::npos);
    Block quote = blockAt(more, 10);
    CHECK(quote.kind == Block::Quote && textOf(quote) == "> q1\n>\n> q2");
    CHECK(blockAt(more, 13).kind == Block::None && blockAt(more, 19).kind == Block::None);
    Block indented = blockAt(more, 16);
    CHECK(indented.kind == Block::Code && textOf(indented) == "    indented\n    code");
    Block task = blockAt(more, 18);
    CHECK(task.kind == Block::ListItem && textOf(task) == "[ ] task");
    CHECK(task.nextItemPrefix == "- [ ] ");
    // Front matter is a code block.
    const std::string fm = "---\ntitle: x\n---\ntext\n";
    Block front = blockAt(fm, 1);
    CHECK(front.kind == Block::Code && fm.substr(front.start, front.end - front.start) == "title: x");
    CHECK(blockAt(fm, 0).kind == Block::None && blockAt(fm, 3).kind == Block::Paragraph);
}

void testMarkdown() {
    GROUP("md:headings");
    auto h = MarkdownParser::parse("# Title\n");
    CHECK(anyRun(h, [](const MdRun &r) {
        return r.heading == 1 && r.text.find("Title") != std::string::npos;
    }));
    auto h2 = MarkdownParser::parse("## Sub\n");
    CHECK(anyRun(h2, [](const MdRun &r) { return r.heading == 2; }));
    // '#' without a following space is not a heading.
    auto notH = MarkdownParser::parse("#nospace\n");
    CHECK(!anyRun(notH, [](const MdRun &r) { return r.heading > 0; }));

    GROUP("md:inline");
    CHECK(anyRun(MarkdownParser::parse("**bold**"),
                 [](const MdRun &r) { return r.bold && r.text == "bold"; }));
    CHECK(anyRun(MarkdownParser::parse("*ital*"),
                 [](const MdRun &r) { return r.italic && r.text == "ital"; }));
    CHECK(anyRun(MarkdownParser::parse("`code`"),
                 [](const MdRun &r) { return r.code && r.text == "code"; }));
    CHECK(anyRun(MarkdownParser::parse("[t](http://x)"), [](const MdRun &r) {
        return r.link && r.text == "t" && r.url == "http://x";
    }));

    GROUP("md:images");
    {
        auto img = MarkdownParser::parse("![A demo](docs/demos/tour.gif)\n");
        CHECK(anyRun(img, [](const MdRun &r) {
            return r.image && !r.link && r.text == "A demo" &&
                   r.src == "docs/demos/tour.gif";
        }));
        // No stray "!" or link run around it.
        CHECK(!anyRun(img, [](const MdRun &r) {
            return r.text.find('!') != std::string::npos || r.link;
        }));
        CHECK(anyRun(MarkdownParser::parse("![t](a.png \"Title\")"),
                     [](const MdRun &r) { return r.image && r.src == "a.png"; }));
        CHECK(anyRun(MarkdownParser::parse("![t](<my pic.png>)"),
                     [](const MdRun &r) { return r.image && r.src == "my pic.png"; }));
        auto badge = MarkdownParser::parse("[![CI](b.svg)](http://ci)");
        CHECK(anyRun(badge, [](const MdRun &r) {
            return r.image && r.link && r.src == "b.svg" && r.url == "http://ci" &&
                   r.text == "CI";
        }));
        CHECK(badge.size() == 1 || !anyRun(badge, [](const MdRun &r) {
            return r.text.find("](") != std::string::npos;
        }));
        // An exclamation mark that is not an image stays text.
        CHECK(anyRun(MarkdownParser::parse("Hi! [x](y)"), [](const MdRun &r) {
            return !r.image && r.text.find("Hi!") != std::string::npos;
        }));
        CHECK(!anyRun(MarkdownParser::parse("![]()"),
                      [](const MdRun &r) { return r.image; }));
    }

    GROUP("md:blocks");
    CHECK(anyRun(MarkdownParser::parse("```\nx=1\n```\n"), [](const MdRun &r) {
        return r.codeBlock && r.text.find("x=1") != std::string::npos;
    }));
    CHECK(anyRun(MarkdownParser::parse("- item\n"),
                 [](const MdRun &r) { return r.listDepth > 0; }));
    CHECK(anyRun(MarkdownParser::parse("> quote\n"),
                 [](const MdRun &r) { return r.quote; }));
    CHECK(anyRun(MarkdownParser::parse("---\n"),
                 [](const MdRun &r) { return r.rule; }));

    GROUP("md:tables");
    auto tbl = MarkdownParser::parse("| A | B |\n| --- | --- |\n| 1 | 2 |\n");
    CHECK(anyRun(tbl, [](const MdRun &r) { return r.table; }));
    CHECK(anyRun(tbl, [](const MdRun &r) {  // header row is bold
        return r.table && r.bold && r.text.find("A") != std::string::npos;
    }));
    CHECK(anyRun(tbl, [](const MdRun &r) {
        return r.table && r.text.find("1") != std::string::npos;
    }));
    // A pipe line without a separator row underneath is NOT a table.
    auto notTbl = MarkdownParser::parse("a | b is just text\n");
    CHECK(!anyRun(notTbl, [](const MdRun &r) { return r.table; }));

    GROUP("md:tables-layout");
    // Rows come out as: header, rule, body rows.
    auto lay = tableLines(MarkdownParser::parse(
        "| Name | Qty |\n|------|-----|\n| apple | 3 |\n| fig | 12 |\n"));
    CHECK(lay.size() == 4);
    CHECK(lay.size() == 4 && lay[0] == "Name  │ Qty");
    CHECK(lay.size() == 4 && lay[1] == "──────┼────");
    CHECK(lay.size() == 4 && lay[2] == "apple │ 3  ");
    CHECK(lay.size() == 4 && lay[3] == "fig   │ 12 ");
    CHECK(columnsAligned(lay));

    GROUP("md:tables-unicode");
    // Widths are measured in characters, not UTF-8 bytes: "café" and "✓ ok"
    // are longer in bytes than on screen, which used to push columns right.
    auto uni = tableLines(MarkdownParser::parse(
        "| Word | Mark |\n|---|---|\n| café | ✓ ok |\n| tea | x |\n"));
    CHECK(uni.size() == 4);
    CHECK(columnsAligned(uni));
    CHECK(uni.size() == 4 && uni[2] == "café │ ✓ ok");
    CHECK(uni.size() == 4 && uni[3] == "tea  │ x   ");
    // Wide (CJK) characters take two columns.
    auto cjk = tableLines(MarkdownParser::parse(
        "| k | v |\n|---|---|\n| 日本 | 1 |\n| ab | 2 |\n"));
    CHECK(cjk.size() == 4 && cjk[3] == "ab   │ 2");
    CHECK(columnsAligned(cjk));

    GROUP("md:tables-inline");
    // Inline markup in cells is rendered, not shown raw, and the hidden markup
    // characters don't count toward the column width.
    auto inl = MarkdownParser::parse(
        "| Kind | Example |\n|---|---|\n| **strong** | `x = 1` |\n"
        "| link | [docs](http://d) |\n| plain | abc |\n");
    auto inlLines = tableLines(inl);
    CHECK(columnsAligned(inlLines));
    CHECK(inlLines.size() == 5 && inlLines[2] == "strong │ x = 1  ");
    CHECK(inlLines.size() == 5 && inlLines[3] == "link   │ docs   ");
    CHECK(anyRun(inl, [](const MdRun &r) {
        return r.table && r.bold && r.text == "strong";
    }));
    CHECK(anyRun(inl, [](const MdRun &r) {
        return r.table && r.code && r.text == "x = 1";
    }));
    CHECK(anyRun(inl, [](const MdRun &r) {
        return r.table && r.link && r.text == "docs" && r.url == "http://d";
    }));
    CHECK(!anyRun(inl, [](const MdRun &r) {
        return r.text.find("**") != std::string::npos ||
               r.text.find('`') != std::string::npos;
    }));

    GROUP("md:tables-align");
    auto al = tableLines(MarkdownParser::parse(
        "| L | C | R |\n|:--|:-:|--:|\n| a | b | c |\n| long | wide | 12345 |\n"));
    CHECK(al.size() == 4 && al[0] == "L    │  C   │     R");
    CHECK(al.size() == 4 && al[2] == "a    │  b   │     c");
    CHECK(columnsAligned(al));

    GROUP("md:tables-cells");
    {
        // Each run knows its cell, so a GUI can build a real table; the
        // padding and rules are marked as not belonging to any cell.
        auto t = MarkdownParser::parse(
            "| L | R |\n|:--|--:|\n| **a** b | 2 |\n| | x |\n\nafter\n\n| Q |\n|---|\n| z |\n");
        CHECK(anyRun(t, [](const MdRun &r) {
            return r.tableId == 1 && r.tableRow == 1 && r.tableCol == 0 && r.bold &&
                   r.text == "a" && r.tableCols == 2 && r.tableAlign == 0;
        }));
        CHECK(anyRun(t, [](const MdRun &r) {
            return r.tableId == 1 && r.tableRow == 1 && r.tableCol == 0 && !r.bold &&
                   r.text == " b";
        }));
        CHECK(anyRun(t, [](const MdRun &r) {
            return r.tableId == 1 && r.tableRow == 2 && r.tableCol == 1 &&
                   r.text == "x" && r.tableAlign == 2;
        }));
        CHECK(anyRun(t, [](const MdRun &r) {
            return r.tableId == 1 && r.tableRow == 0 && r.tableCol == 1 && r.bold;
        }));
        // Every table run is numbered, and the second table is table 2.
        CHECK(!anyRun(t, [](const MdRun &r) { return r.table && r.tableId == 0; }));
        CHECK(anyRun(t, [](const MdRun &r) {
            return r.tableId == 2 && r.tableCol == 0 && r.tableRow == 1 && r.text == "z";
        }));
        // Separators and the header rule belong to no cell.
        CHECK(!anyRun(t, [](const MdRun &r) {
            return r.tableCol >= 0 && (r.text.find("\xE2\x94") != std::string::npos ||
                                       r.text == "\n");
        }));
        CHECK(!anyRun(t, [](const MdRun &r) { return !r.table && r.tableId != 0; }));
    }

    GROUP("md:anchors");
    CHECK(MarkdownParser::anchor("Next to VS Code and Zed") == "next-to-vs-code-and-zed");
    CHECK(MarkdownParser::anchor(" Installing, and building! ") == "installing-and-building");
    CHECK(MarkdownParser::anchor("C++ & you_2") == "c--you_2");
    CHECK(MarkdownParser::anchor("Caf\xC3\xA9") == "caf\xC3\xA9");

    GROUP("md:source-lines");
    {
        auto doc = MarkdownParser::parse(
            "# Title\n\nfirst para\nstill first\n\n| a | b |\n|---|---|\n| c | d |\n\n- item **x**\n");
        CHECK(anyRun(doc, [](const MdRun &r) { return r.heading == 1 && r.line == 0; }));
        CHECK(anyRun(doc, [](const MdRun &r) { return r.text == "first para" && r.line == 2; }));
        CHECK(anyRun(doc, [](const MdRun &r) { return r.text == "still first" && r.line == 3; }));
        // A table's runs all carry its first line.
        CHECK(anyRun(doc, [](const MdRun &r) { return r.text == "d" && r.line == 5; }));
        CHECK(anyRun(doc, [](const MdRun &r) { return r.bold && r.text == "x" && r.line == 9; }));
        CHECK(!anyRun(doc, [](const MdRun &r) { return r.line < 0; }));
    }

    GROUP("md:tables-shape");
    // No leading/trailing pipes is still a table.
    CHECK(tableLines(MarkdownParser::parse("A | B\n--- | ---\n1 | 2\n")).size() == 3);
    // Short rows are padded out to the header's column count, long rows are cut.
    auto ragged = tableLines(MarkdownParser::parse(
        "| a | b | c |\n|---|---|---|\n| 1 |\n| 1 | 2 | 3 | 4 |\n"));
    CHECK(ragged.size() == 4 && ragged[2] == "1 │   │  ");
    CHECK(ragged.size() == 4 && ragged[3] == "1 │ 2 │ 3");
    // An escaped pipe is cell content, not a column break.
    auto esc = tableLines(MarkdownParser::parse(
        "| op | means |\n|---|---|\n| a \\| b | or |\n"));
    CHECK(esc.size() == 3 && esc[2] == "a | b │ or   ");
    // Header and separator column counts must match.
    CHECK(!anyRun(MarkdownParser::parse("| a | b |\n| --- |\n"),
                  [](const MdRun &r) { return r.table; }));
    // A separator cell must be dashes (optionally colon-wrapped).
    CHECK(!anyRun(MarkdownParser::parse("| a | b |\n| -x- | --- |\n"),
                  [](const MdRun &r) { return r.table; }));
    // The table ends at a blank line or a line without pipes, and the text
    // around it stays ordinary paragraph text on its own lines.
    auto around = MarkdownParser::parse(
        "Before\n| a | b |\n|---|---|\n| 1 | 2 |\nAfter\n");
    CHECK(tableLines(around).size() == 3);
    CHECK(anyRun(around, [](const MdRun &r) {
        return !r.table && r.text == "After";
    }));
    CHECK(!anyRun(around, [](const MdRun &r) {
        return r.table && r.text.find("fter") != std::string::npos;
    }));

    GROUP("md:block-separation");
    // A heading directly after a paragraph (no blank line) must still be a
    // heading, preceded by a newline rather than glued onto the paragraph.
    auto glued = MarkdownParser::parse("paragraph text\n# Heading\n");
    CHECK(anyRun(glued, [](const MdRun &r) {
        return r.heading == 1 && r.text.find("Heading") != std::string::npos;
    }));
    bool sawNewlineBeforeHeading = false;
    {
        bool prevEndsNewline = true;  // start of doc counts as a line start
        for (const MdRun &r : glued) {
            if (r.heading == 1) { sawNewlineBeforeHeading = prevEndsNewline; break; }
            prevEndsNewline = !r.text.empty() && r.text.back() == '\n';
        }
    }
    CHECK(sawNewlineBeforeHeading);
}

// Every run's text joined: what a reader sees, block by block.
std::string mdText(const std::vector<MdRun> &runs) {
    std::string s;
    for (const MdRun &r : runs) s += r.text;
    return s;
}

template <class F> bool mdAny(const std::vector<MdRun> &runs, F f) {
    for (const MdRun &r : runs)
        if (f(r)) return true;
    return false;
}

template <class F> int mdCount(const std::vector<MdRun> &runs, F f) {
    int n = 0;
    for (const MdRun &r : runs)
        if (f(r)) n++;
    return n;
}

// The run whose text is exactly `text`, or a blank run.
MdRun mdRun(const std::vector<MdRun> &runs, const std::string &text) {
    for (const MdRun &r : runs)
        if (r.text == text) return r;
    return MdRun();
}

// What CommonMark and GitHub do that the first parser did not. Most of the
// cases come from files that rendered wrongly: identifiers with
// underscores, a bold label glued to its sentence, escapes, and the space
// between blocks.
void testMarkdownCommonMark() {
    using P = MarkdownParser;

    GROUP("md:spacing");
    {
        // One gap between blocks, however many blank lines part them.
        auto r = P::parse("one\ntwo\n\nthree\n\n\n\nfour\n");
        CHECK(mdText(r) == "one two\n\nthree\n\nfour\n");
        CHECK(mdCount(r, [](const MdRun &x) { return x.gap; }) == 2);
        // A heading's space is the same with or without a blank line after it.
        CHECK(mdText(P::parse("# H\n\ntext\n")) == mdText(P::parse("# H\ntext\n")));
        CHECK(mdText(P::parse("# H\ntext\n")) == "H\n\ntext\n");
        // A list or a table straight after a paragraph is set apart all the
        // same; the items of a tight list are not.
        CHECK(mdText(P::parse("text\n- a\n- b\n\nafter\n")) ==
              "text\n\n  \xE2\x80\xA2 a\n  \xE2\x80\xA2 b\n\nafter\n");
        auto t = P::parse("text\n| a |\n|---|\n| 1 |\nafter\n");
        CHECK(t.size() > 2 && t[0].text == "text" && t[1].text == "\n" && t[2].gap);
        CHECK(!t.empty() && t.back().text == "\n" && !t.back().gap);
        // A loose list has a gap between its items.
        CHECK(mdCount(P::parse("- a\n\n- b\n"), [](const MdRun &x) { return x.gap; }) == 1);
        // Never two gaps in a row, and none at either end.
        auto g = P::parse("\n\n# A\n\n\n<!-- c -->\n\n[x]: /y\n\npara\n\n\n");
        bool ok = !g.empty() && !g.front().gap && !g.back().gap;
        for (size_t k = 1; k < g.size(); k++)
            if (g[k].gap && g[k - 1].gap) ok = false;
        CHECK(ok);
        CHECK(mdText(g) == "A\n\npara\n");
    }

    GROUP("md:emphasis");
    {
        auto names = P::parse("snake_case_name, max_len, load_config and x_i^T\n");
        CHECK(mdText(names) == "snake_case_name, max_len, load_config and x_i^T\n");
        CHECK(!mdAny(names, [](const MdRun &x) { return x.italic || x.bold; }));
        auto maths = P::parse("2 * 3 * 4 and a * b\n");
        CHECK(mdText(maths) == "2 * 3 * 4 and a * b\n");
        CHECK(!mdAny(maths, [](const MdRun &x) { return x.italic; }));
        auto both = P::parse("***both***\n");
        CHECK(mdRun(both, "both").bold && mdRun(both, "both").italic);
        auto nest = P::parse("**bold *it* bold** and *em **strong** em*\n");
        CHECK(mdRun(nest, "it").bold && mdRun(nest, "it").italic);
        CHECK(mdRun(nest, "bold ").bold && !mdRun(nest, "bold ").italic);
        CHECK(mdRun(nest, "strong").bold && mdRun(nest, "strong").italic);
        CHECK(mdRun(nest, "em ").italic && !mdRun(nest, "em ").bold);
        // A bold label glued to its sentence ends where its ** close.
        auto label = P::parse("**Label**Text after\n");
        CHECK(mdRun(label, "Label").bold && !mdRun(label, "Text after").bold);
        // Stars inside a word emphasise; underscores inside one do not.
        CHECK(mdRun(P::parse("un*frig*able\n"), "frig").italic);
        CHECK(!mdAny(P::parse("un_frig_able\n"), [](const MdRun &x) { return x.italic; }));
        CHECK(mdRun(P::parse("__init__.py\n"), "init").bold);   // as on GitHub
        CHECK(!mdAny(P::parse("foo_bar_ and _foo_bar\n"),
                     [](const MdRun &x) { return x.italic; }));
        CHECK(mdText(P::parse("**unclosed and a lone *\n")) == "**unclosed and a lone *\n");
        auto strike = P::parse("~~gone~~, ~one~ and ~~~three~~~\n");
        CHECK(mdRun(strike, "gone").strike && mdRun(strike, "one").strike);
        CHECK(!mdAny(strike, [](const MdRun &x) { return x.strike && x.text == "three"; }));
        // A code span binds tighter than emphasis.
        auto code = P::parse("*foo`*`\n");
        CHECK(mdRun(code, "*").code && !mdAny(code, [](const MdRun &x) { return x.italic; }));
        auto inLink = P::parse("[*a*](u) and *[b](v)*\n");
        CHECK(mdRun(inLink, "a").italic && mdRun(inLink, "a").link);
        CHECK(mdRun(inLink, "b").italic && mdRun(inLink, "b").url == "v");
    }

    GROUP("md:escapes");
    {
        auto e = P::parse("\\*x\\* a\\_b \\[t\\](u) \\q \\\\ \\`c\\`\n");
        CHECK(mdText(e) == "*x* a_b [t](u) \\q \\ `c`\n");
        CHECK(!mdAny(e, [](const MdRun &x) { return x.italic || x.link || x.code; }));
        CHECK(!mdAny(P::parse("\\# not a heading\n"), [](const MdRun &x) { return x.heading; }));
        CHECK(mdText(P::parse("f\xE2\x80\xB2\\_n=42\n")) == "f\xE2\x80\xB2_n=42\n");
    }

    GROUP("md:code-spans");
    {
        CHECK(mdRun(P::parse("``a ` b``\n"), "a ` b").code);
        CHECK(mdRun(P::parse("` `` `\n"), "``").code);
        CHECK(mdRun(P::parse("`  x  `\n"), " x ").code);
        CHECK(mdRun(P::parse("`a\nb`\n"), "a b").code);
        CHECK(mdText(P::parse("`unclosed\n")) == "`unclosed\n");
        CHECK(mdRun(P::parse("`**not bold** _x_`\n"), "**not bold** _x_").code);
    }

    GROUP("md:links");
    {
        auto three = P::parse("[a](u1) [b](u2) [c](u3)\n");
        CHECK(mdCount(three, [](const MdRun &x) { return x.link; }) == 3);
        CHECK(mdRun(three, "c").url == "u3");
        CHECK(mdRun(P::parse("[t](http://x \"Title\")\n"), "t").url == "http://x");
        CHECK(mdRun(P::parse("[w](https://e.org/a_(b))\n"), "w").url == "https://e.org/a_(b)");
        CHECK(mdRun(P::parse("[t](<a b.md>)\n"), "t").url == "a b.md");
        auto styled = P::parse("[**b** c](u)\n");
        CHECK(mdRun(styled, "b").bold && mdRun(styled, "b").url == "u");
        CHECK(mdRun(styled, " c").link && !mdRun(styled, " c").bold);
        // Reference links: full, collapsed (any case) and shortcut; the
        // definitions themselves show nothing.
        auto refs = P::parse("[x][r], [R][] and [r].\n\n[r]: http://r \"T\"\n[s]: </s>\n");
        CHECK(mdRun(refs, "x").url == "http://r" && mdRun(refs, "R").url == "http://r");
        CHECK(mdRun(refs, "r").url == "http://r");
        CHECK(mdText(refs) == "x, R and r.\n");
        CHECK(mdText(P::parse("[missing][nope]\n")) == "[missing][nope]\n");
        // Autolinks, and web addresses written out.
        CHECK(mdRun(P::parse("<https://a.b/c>\n"), "https://a.b/c").url == "https://a.b/c");
        CHECK(mdRun(P::parse("<me@x.org>\n"), "me@x.org").url == "mailto:me@x.org");
        auto bare = P::parse("see https://a.b/p_q_r. or (https://a.b/c) or www.a.b,\n");
        CHECK(mdRun(bare, "https://a.b/p_q_r").link);
        CHECK(mdRun(bare, "https://a.b/c").link);
        CHECK(mdRun(bare, "www.a.b").url == "http://www.a.b");
        CHECK(!mdAny(bare, [](const MdRun &x) { return x.italic; }));
        CHECK(!mdAny(P::parse("nohttps://x and awww.b\n"), [](const MdRun &x) { return x.link; }));
        // A badge: a picture inside a link.
        auto badge = P::parse("[![CI](b.svg)](http://ci) [![x]][site]\n\n[x]: x.png\n[site]: /s\n");
        CHECK(mdAny(badge, [](const MdRun &x) { return x.image && x.src == "b.svg" && x.link; }));
        CHECK(mdAny(badge, [](const MdRun &x) { return x.image && x.src == "x.png" && x.url == "/s"; }));
    }

    GROUP("md:entities");
    {
        CHECK(mdText(P::parse("&amp; &lt;t&gt; &copy; &#65;&#x42; &nope; &alpha;&Omega; AT&T\n")) ==
              "& <t> \xC2\xA9 AB &nope; \xCE\xB1\xCE\xA9 AT&T\n");
        CHECK(mdText(P::parse("a&nbsp;b &mdash; &frac12; &#0;\n")) ==
              "a\xC2\xA0" "b \xE2\x80\x94 \xC2\xBD \xEF\xBF\xBD\n");
    }

    GROUP("md:hard-breaks");
    {
        auto two = P::parse("a  \nb\n");
        CHECK(two.size() >= 3 && two[0].text == "a" && two[1].hardBreak && two[1].text == "\n" &&
              two[2].text == "b" && two[2].line == 1);
        CHECK(mdAny(P::parse("a\\\nb\n"), [](const MdRun &x) { return x.hardBreak; }));
        CHECK(mdAny(P::parse("a<br>b and c<br/>d\n"), [](const MdRun &x) { return x.hardBreak; }));
        CHECK(mdText(P::parse("a\nb\n")) == "a b\n");
        CHECK(mdText(P::parse("last line  \n")) == "last line\n");
    }

    GROUP("md:lists");
    {
        auto nested = P::parse("- a\n  - b\n    - c\n");
        CHECK(mdRun(nested, "  \xE2\x80\xA2 ").marker && mdRun(nested, "a").listDepth == 1);
        CHECK(mdRun(nested, "    \xE2\x97\xA6 ").listDepth == 2);        // ◦
        CHECK(mdRun(nested, "      \xE2\x96\xAA ").listDepth == 3);      // ▪
        // Nesting follows the parent's text column, not two spaces a level.
        CHECK(mdRun(P::parse("- a\n    - b\n"), "b").listDepth == 2);
        CHECK(mdRun(P::parse("1. a\n   - b\n"), "b").listDepth == 2);
        CHECK(mdRun(P::parse("- a\n - b\n"), "b").listDepth == 1);
        auto ordered = P::parse("3. x\n4. y\n10) z\n");
        CHECK(mdRun(ordered, "  3. ").marker && mdRun(ordered, "  3. ").ordered);
        CHECK(mdRun(ordered, "y").ordered && mdRun(ordered, "  10) ").marker);
        auto tasks = P::parse("- [ ] to do\n- [x] done\n");
        CHECK(mdRun(tasks, "  \xE2\x98\x90 ").marker && mdRun(tasks, "  \xE2\x98\x91 ").marker);
        CHECK(mdRun(tasks, "to do").listDepth == 1 && mdRun(tasks, "done").listDepth == 1);
        // A line under an item continues it, indented or not.
        auto lazy = P::parse("- item\ncontinued\n  and more\n- next\n");
        CHECK(mdRun(lazy, "continued").listDepth == 1 && mdRun(lazy, "continued").line == 1);
        CHECK(mdRun(lazy, "and more").listDepth == 1);
        CHECK(mdCount(lazy, [](const MdRun &x) { return x.marker; }) == 2);
        // A paragraph indented under an item after a blank line belongs to it.
        auto para = P::parse("- item\n\n  second paragraph\n\nafter\n");
        CHECK(mdRun(para, "second paragraph").listDepth == 1);
        CHECK(!mdRun(para, "second paragraph").marker && mdRun(para, "after").listDepth == 0);
        // Only a list starting at 1 may interrupt a paragraph.
        CHECK(!mdAny(P::parse("The year\n2019. was long\n"),
                     [](const MdRun &x) { return x.listDepth > 0; }));
        CHECK(mdAny(P::parse("Steps:\n1. first\n"), [](const MdRun &x) { return x.marker; }));
        CHECK(!mdAny(P::parse("* * *\n"), [](const MdRun &x) { return x.marker; }));
        // A bold label at the start of an item.
        auto label = P::parse("- **Step one**: open the file\n");
        CHECK(mdRun(label, "Step one").bold && mdRun(label, "Step one").listDepth == 1);
        CHECK(mdRun(label, ": open the file").listDepth == 1);
    }

    GROUP("md:quotes");
    {
        auto q = P::parse("> a long\n> quote\n");
        CHECK(mdText(q) == "a long quote\n" && mdRun(q, "a long").quote);
        auto paras = P::parse("> one\n>\n> two\nlazy\n");
        CHECK(mdText(paras) == "one\n\ntwo lazy\n");
        CHECK(mdAny(paras, [](const MdRun &x) { return x.gap && x.quote; }));
        CHECK(mdRun(paras, "lazy").quote);
        auto apart = P::parse("> one\n\n> two\n");
        CHECK(mdAny(apart, [](const MdRun &x) { return x.gap && !x.quote; }));
        auto nested = P::parse("> outer\n>> inner\n");
        CHECK(mdText(nested) == "outer\n\ninner\n");
        auto alert = P::parse("> [!TIP]\n> Use it.\n");
        CHECK(mdRun(alert, "Tip").bold && mdRun(alert, "Tip").quote);
        CHECK(mdRun(alert, "Use it.").quote && !mdRun(alert, "Use it.").bold);
        auto list = P::parse("> - a\n> - b\n");
        CHECK(mdCount(list, [](const MdRun &x) { return x.marker && x.quote; }) == 2);
        CHECK(mdRun(P::parse("> # Title\n"), "Title").heading == 1);
    }

    GROUP("md:code-blocks");
    {
        CHECK(mdRun(P::parse("~~~\nx\n~~~\n"), "x\n").codeBlock);
        auto longer = P::parse("````md\n```\ninner\n```\n````\n");
        CHECK(mdText(longer) == "```\ninner\n```\n");
        CHECK(mdRun(P::parse("    code\n      more\n"), "code\n").codeBlock);
        CHECK(mdRun(P::parse("    code\n      more\n"), "  more\n").codeBlock);
        auto blankIn = P::parse("    a\n\n    b\n\nafter\n");
        CHECK(mdText(blankIn) == "a\n\nb\n\nafter\n");
        CHECK(mdRun(blankIn, "\n").codeBlock);   // the blank line inside the block
        CHECK(mdText(P::parse("para\n    not code\n")) == "para not code\n");
        CHECK(mdText(P::parse("```\nunclosed\n")) == "unclosed\n");
        CHECK(mdRun(P::parse("- x\n  ```\n  y\n  ```\n"), "y\n").codeBlock);
        CHECK(mdRun(P::parse("``` a`b ```\n"), "a`b").code);   // not a fence
    }

    GROUP("md:headings");
    {
        CHECK(mdText(P::parse("# T #\n")) == "T\n" && mdText(P::parse("# T#\n")) == "T#\n");
        CHECK(!mdAny(P::parse("#\n"), [](const MdRun &x) { return x.heading; }));
        CHECK(mdRun(P::parse("Title\n=====\n"), "Title").heading == 1);
        auto h2 = P::parse("Title\n---\n");
        CHECK(mdRun(h2, "Title").heading == 2 && !mdAny(h2, [](const MdRun &x) { return x.rule; }));
        CHECK(mdAny(P::parse("text\n\n---\n"), [](const MdRun &x) { return x.rule; }));
        CHECK(mdAny(P::parse("- item\n---\n"), [](const MdRun &x) { return x.rule; }));
        auto two = P::parse("One\nTwo\n===\n");
        CHECK(mdRun(two, "One").heading == 1 && mdRun(two, "Two").heading == 1);
        CHECK(mdRun(two, "Two").line == 0);   // one heading, one line
        CHECK(mdRun(P::parse("<h2 align=\"center\">Hi <b>there</b></h2>\n"), "there").heading == 2);
        // Front matter shows as code.
        auto fm = P::parse("---\ntitle: x\n---\n# Body\n");
        CHECK(mdRun(fm, "title: x\n").codeBlock && mdRun(fm, "Body").heading == 1);
        CHECK(!mdAny(fm, [](const MdRun &x) { return x.rule; }));
        auto notFm = P::parse("---\nplain words\n---\n");
        CHECK(mdAny(notFm, [](const MdRun &x) { return x.rule; }));
        CHECK(mdRun(notFm, "plain words").heading == 2);
    }

    GROUP("md:html");
    {
        CHECK(mdText(P::parse("<!-- c -->\ntext\n")) == "text\n");
        CHECK(mdText(P::parse("<!--\na\n\nb\n-->\ntext\n")) == "text\n");
        CHECK(!mdAny(P::parse("a <!-- c --> b\n"),
                     [](const MdRun &x) { return x.text.find("<!--") != std::string::npos; }));
        auto tags = P::parse("<kbd>K</kbd> <b>B</b> <i>I</i> <s>S</s> <a href=\"u\">L</a> "
                             "<span>s</span> Vec<T> x\n");
        CHECK(mdRun(tags, "K").code && mdRun(tags, "B").bold && mdRun(tags, "I").italic);
        CHECK(mdRun(tags, "S").strike && mdRun(tags, "L").url == "u");
        CHECK(mdText(tags) == "K B I S L s Vec x\n");
        auto img = P::parse("<p align=\"center\">\n  <img src=\"logo.png\" alt=\"Logo\" width=\"90\">\n</p>\n");
        CHECK(img.size() == 2 && img[0].image && img[0].src == "logo.png" && img[0].text == "Logo");
        CHECK(mdText(P::parse("a < b > c\n")) == "a < b > c\n");
    }

    GROUP("md:anchors-unicode");
    CHECK(P::anchor("\xC2\xA7" "2Setup") == "2setup");                       // §2Setup
    CHECK(P::anchor("\xF0\x9F\x9A\x80 Launch") == "-launch");                 // 🚀
    CHECK(P::anchor("\xC3\x9C" "ber Stra\xC3\x9F" "e") == "\xC3\xBC" "ber-stra\xC3\x9F" "e");
    CHECK(P::anchor("\xCE\x91\xCE\x92 \xD0\x9C\xD0\x98\xD0\xA0") ==
          "\xCE\xB1\xCE\xB2-\xD0\xBC\xD0\xB8\xD1\x80");                      // ΑΒ МИР
    CHECK(P::anchor("A \xE2\x80\x93 B") == "a--b");                           // en dash
    CHECK(P::anchor("x \xE2\x88\x88 \xE2\x84\x9D") == "x--\xE2\x84\x9D");     // ∈ goes, ℝ stays

    GROUP("md:speed");
    {
        std::string big;
        for (int k = 0; k < 20000; k++) {
            big += "Line " + std::to_string(k) + " has *emphasis*, `code`, a [link](u_" +
                   std::to_string(k) + "), snake_case and **bold**.\n";
            if (k % 5 == 4) big += "\n";
            if (k % 50 == 49) big += "- item\n- item\n\n";
        }
        auto t0 = std::chrono::steady_clock::now();
        auto runs = P::parse(big);
        double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
        CHECK(!runs.empty());
        // Inputs that make a naive parser quadratic.
        const std::string hard[] = {
            std::string(30000, '['), std::string(30000, '*') + "a",
            std::string(20000, '_') + " x", [] {
                std::string s;
                for (int k = 1; k < 200; k++) s += std::string((size_t)k, '`') + "a";
                return s;
            }(),
            [] {
                std::string s;
                for (int k = 0; k < 5000; k++) s += "![";
                return s + "x" + std::string(5000, ']');
            }(),
            [] {
                std::string s;
                for (int k = 0; k < 10000; k++) s += "*a **b ";
                return s;
            }(),
        };
        double worst = 0;
        for (const std::string &h : hard) {
            auto t1 = std::chrono::steady_clock::now();
            P::parse(h);
            worst = std::max(worst, std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - t1).count());
        }
        CHECK(worst < 2000);
        std::printf("  markdown: 20000 lines parsed in %.1f ms; worst pathological input %.1f ms\n",
                    ms, worst);
    }
}

// MarkdownParser::lines, which MarkdownEdit builds on.
// Math, read as GitHub and pandoc read it, before any other inline rule.
void testMarkdownMath() {
    using P = MarkdownParser;
    auto maths = [](const std::vector<MdRun> &runs) {
        std::vector<MdRun> m;
        for (const MdRun &r : runs)
            if (r.math) m.push_back(r);
        return m;
    };
    auto noMath = [&](const std::string &md) { return maths(P::parse(md)).empty(); };

    GROUP("md:math-inline");
    {
        auto one = maths(P::parse("$x^2$"));
        CHECK(one.size() == 1 && one[0].math == 1 && one[0].text == "x^2" && one[0].code);
        auto mid = P::parse("a $x_1$ b\n");
        CHECK(mdText(mid) == "a x_1 b\n");
        CHECK(mid.size() == 4 && mid[1].math == 1 && mid[1].text == "x_1" && mid[1].line == 0);
        // Nothing inside is Markdown: escapes stay, underscores and stars
        // do not pair, brackets are not links.
        auto esc = maths(P::parse("set $\\{a, b\\}$ here"));
        CHECK(esc.size() == 1 && esc[0].text == "\\{a, b\\}");
        auto under = P::parse("$a_i$ and $b_j$ and $c*d*e$\n");
        CHECK(maths(under).size() == 3 && !mdAny(under, [](const MdRun &r) { return r.italic; }));
        CHECK(maths(under).size() == 3 && maths(under)[2].text == "c*d*e");
        auto brackets = P::parse("$[a](b)$ and $\\mathbb{1}[i \\in S]$\n");
        CHECK(maths(brackets).size() == 2 && !mdAny(brackets, [](const MdRun &r) { return r.link; }));
        CHECK(maths(P::parse("$\\,x\\!y\\ z\\\\$")).size() == 1 &&
              maths(P::parse("$\\,x\\!y\\ z\\\\$"))[0].text == "\\,x\\!y\\ z\\\\");
        // Prices are not math: the closing $ may not follow a space, and may
        // not come before a digit.
        CHECK(noMath("It costs $5 and $10.\n"));
        CHECK(mdText(P::parse("It costs $5 and $10.\n")) == "It costs $5 and $10.\n");
        CHECK(noMath("from $5, or $6 with tax"));
        CHECK(noMath("between $5 and 10$20 today"));
        CHECK(noMath("$ x$"));      // the opener is followed by a space
        CHECK(noMath("$x $"));      // the closer follows a space
        CHECK(noMath("$x$5"));      // the closer is followed by a digit
        CHECK(noMath("a $ b"));
        CHECK(mdText(P::parse("a $ b\n")) == "a $ b\n");
        // \$ is a dollar sign; a $ in a code span is not math.
        CHECK(noMath("\\$x\\$"));
        CHECK(mdText(P::parse("\\$x\\$\n")) == "$x$\n");
        auto code = P::parse("`$x$` and `a$` $b$\n");
        CHECK(maths(code).size() == 1 && maths(code)[0].text == "b");
        CHECK(mdAny(code, [](const MdRun &r) { return !r.math && r.code && r.text == "$x$"; }));
        // An escaped dollar inside math does not close it.
        auto dollar = maths(P::parse("$\\$5 + x$"));
        CHECK(dollar.size() == 1 && dollar[0].text == "\\$5 + x");
        // A code span inside math is skipped whole.
        auto tick = maths(P::parse("$a `$` b$"));
        CHECK(tick.size() == 1 && tick[0].text == "a `$` b");
        // \(...\) is inline math too.
        auto paren = maths(P::parse("so \\(x + y\\) holds"));
        CHECK(paren.size() == 1 && paren[0].math == 1 && paren[0].text == "x + y");
        CHECK(noMath("an escaped \\( paren"));
        // A formula may cross a line break; it reads as one line.
        auto wrap = maths(P::parse("so $a +\nb$ holds\n"));
        CHECK(wrap.size() == 1 && wrap[0].text == "a + b" && wrap[0].line == 0);
        auto later = P::parse("one\ntwo $y$\n");
        CHECK(maths(later).size() == 1 && maths(later)[0].line == 1);
        // In links, headings, quotes, lists and emphasis.
        auto link = P::parse("[see $x$](u)\n");
        CHECK(maths(link).size() == 1 && maths(link)[0].link && maths(link)[0].url == "u");
        auto dest = P::parse("[a](b$c) $d$\n");
        CHECK(mdAny(dest, [](const MdRun &r) { return r.link && r.url == "b$c"; }) &&
              maths(dest).size() == 1 && maths(dest)[0].text == "d");
        auto head = maths(P::parse("# The $O(n)$ bound\n"));
        CHECK(head.size() == 1 && head[0].heading == 1 && head[0].text == "O(n)");
        auto quote = maths(P::parse("> so $q_t$ here\n"));
        CHECK(quote.size() == 1 && quote[0].quote);
        auto item = maths(P::parse("- item $i$\n"));
        CHECK(item.size() == 1 && item[0].listDepth == 1);
        auto em = maths(P::parse("*$x$*"));
        CHECK(em.size() == 1 && em[0].italic);
        // Dollars that close nothing are text.
        CHECK(noMath("a $$ b"));
        CHECK(mdText(P::parse("a $$ b\n")) == "a $$ b\n");
        CHECK(noMath("$$"));
        // Math in a table cell, with the table's fields.
        auto tbl = P::parse("| a | $x_1$ |\n|---|---|\n| $\\alpha$ | $5 |\n");
        auto tm = maths(tbl);
        CHECK(tm.size() == 2 && tm[0].table && tm[0].tableRow == 0 && tm[0].tableCol == 1 &&
              tm[0].text == "x_1");
        CHECK(tm.size() == 2 && tm[1].tableRow == 1 && tm[1].tableCol == 0 &&
              tm[1].text == "\\alpha");
        CHECK(mdAny(tbl, [](const MdRun &r) { return r.table && r.text == "$5"; }));
    }

    GROUP("md:math-display");
    {
        // $$ inside a paragraph stands on a line of its own.
        auto para = P::parse("see $$x^2$$ here\n");
        CHECK(mdText(para) == "see\nx^2\nhere\n");
        auto pm = maths(para);
        CHECK(pm.size() == 1 && pm[0].math == 2 && pm[0].text == "x^2");
        auto spaced = maths(P::parse("$$ a + b $$"));
        CHECK(spaced.size() == 1 && spaced[0].math == 2 && spaced[0].text == "a + b");
        auto two = P::parse("$$a$$ and $$b$$\n");
        CHECK(mdText(two) == "a\nand\nb\n");
        auto across = P::parse("text\nmore $$\nx\n$$ after\n");
        CHECK(mdText(across) == "text more\nx\nafter\n");
        CHECK(maths(across).size() == 1 && maths(across)[0].line == 1);
        // Not in a heading or a table cell, which stay one line.
        auto head = P::parse("# A $$x$$ b\n");
        CHECK(mdCount(head, [](const MdRun &r) { return r.text == "\n"; }) == 1);
        CHECK(maths(head).size() == 1 && maths(head)[0].math == 2);
    }

    GROUP("md:math-blocks");
    {
        const std::string src = "Intro\n\n$$\nx^2 + y^2\n$$\n\nAfter\n";
        auto b = P::parse(src);
        auto bm = maths(b);
        CHECK(bm.size() == 1 && bm[0].math == 2 && bm[0].text == "x^2 + y^2" && bm[0].line == 2);
        CHECK(mdText(b) == "Intro\n\nx^2 + y^2\n\nAfter\n");
        auto lines = P::lines(src);
        CHECK(lines.size() >= 5 && lines[2].kind == MdLine::Math && lines[3].kind == MdLine::Math &&
              lines[4].kind == MdLine::Math && lines[2].block == lines[4].block &&
              lines[2].block >= 0);
        // A double-click opens the whole block, delimiters and all.
        for (int line : {2, 3, 4}) {
            MarkdownEdit::Block e = MarkdownEdit::blockAt(src, line);
            CHECK(e.kind == MarkdownEdit::Block::Math &&
                  src.substr(e.start, e.end - e.start) == "$$\nx^2 + y^2\n$$" &&
                  e.firstLine == 2 && e.lastLine == 4);
        }
        MarkdownEdit::Block e = MarkdownEdit::blockAt(src, 3);
        CHECK(MarkdownEdit::replace(src, e, "$$\nz\n$$") == "Intro\n\n$$\nz\n$$\n\nAfter\n");

        // An aligned block keeps its lines exactly.
        const std::string aligned =
            "$$\n\\begin{aligned}\na &= b \\\\\nc &= d\n\\end{aligned}\n$$\n";
        auto am = maths(P::parse(aligned));
        CHECK(am.size() == 1 &&
              am[0].text == "\\begin{aligned}\na &= b \\\\\nc &= d\n\\end{aligned}");
        // One line, and content on the delimiter lines.
        auto single = maths(P::parse("$$ E = mc^2 $$\n"));
        CHECK(single.size() == 1 && single[0].text == "E = mc^2");
        CHECK(P::lines("$$ E = mc^2 $$\n")[0].kind == MdLine::Math);
        auto edges = maths(P::parse("$$ a = b\n+ c $$\n"));
        CHECK(edges.size() == 1 && edges[0].text == "a = b\n+ c");
        // A $$ line interrupts a paragraph, as a fence does.
        auto interrupt = P::parse("text\n$$\nx\n$$\nmore\n");
        CHECK(mdText(interrupt) == "text\n\nx\n\nmore\n");
        CHECK(P::lines("text\n$$\nx\n$$\nmore\n")[4].kind == MdLine::Text);
        // A blank line inside is no block, and no math at all.
        CHECK(noMath("$$\nx\n\ny\n$$\n"));
        CHECK(noMath("$$\nnever closed\n"));
        // Two formulas on one line are a paragraph.
        CHECK(P::lines("$$a$$ and $$b$$\n")[0].kind == MdLine::Text);
        // \[ ... \] on lines of its own.
        auto bracket = maths(P::parse("\\[\n\\int_0^1 f\n\\]\n"));
        CHECK(bracket.size() == 1 && bracket[0].math == 2 && bracket[0].text == "\\int_0^1 f");
        CHECK(maths(P::parse("\\[x\\]\n")).size() == 1);
        // GitHub's ```math fence.
        const std::string fence = "```math\nE = mc^2\n```\n";
        auto fm = maths(P::parse(fence));
        CHECK(fm.size() == 1 && fm[0].math == 2 && fm[0].text == "E = mc^2" && fm[0].line == 1);
        CHECK(MarkdownEdit::blockAt(fence, 1).kind == MarkdownEdit::Block::Code);
        CHECK(noMath("```python\nx = 1\n```\n"));
        // In a list item, and in a quote.
        auto inList = maths(P::parse("- item\n\n  $$\n  x\n  $$\n"));
        CHECK(inList.size() == 1 && inList[0].listDepth == 1 && inList[0].text == "x");
        auto inQuote = maths(P::parse("> $$\n> x\n> $$\n"));
        CHECK(inQuote.size() == 1 && inQuote[0].quote && inQuote[0].math == 2 &&
              inQuote[0].text == "x");
        // Indented four spaces it is code.
        CHECK(noMath("    $$\n    x\n    $$\n"));
    }

    GROUP("md:math-speed");
    {
        std::string dollars, doubles, parens;
        for (int k = 0; k < 20000; k++) {
            dollars += "$a ";
            doubles += "$$a ";
            parens += "\\(a ";
        }
        auto t0 = std::chrono::steady_clock::now();
        auto r1 = P::parse(dollars);
        auto r2 = P::parse(doubles);
        auto r3 = P::parse(parens);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        // Every $$ closes the one before it; the others close nothing.
        CHECK(maths(r1).empty() && maths(r2).size() == 10000 && maths(r3).empty());
        CHECK(ms < 2000);
        std::printf("  markdown math: 60,000 openers, 40,000 closing nothing, in %.1f ms\n", ms);
    }
}

// The TeX side of typesetting a page's math: what may go into a batch, how
// a formula is rewritten for LaTeX, the batch document, and the log.
void testMathTex() {
    using namespace MathTex;
    auto count = [](const std::string &s, const std::string &what) {
        int n = 0;
        for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) n++;
        return n;
    };

    GROUP("mathtex:check");
    CHECK(check("x^2").empty());
    CHECK(check("\\{a, b\\} \\cup \\{c\\}").empty());
    CHECK(check("\\text{costs \\$5 and $x$}").empty());
    CHECK(check("50\\% % a comment with a { in it\n+ 1").empty());
    CHECK(check("\\\\{x}").empty());
    CHECK(check("\\inputs_i").empty());               // not \input
    CHECK(!check("{x").empty());
    CHECK(!check("x}").empty());
    CHECK(check("x}{").find("}") != std::string::npos);
    CHECK(!check("a $ b").empty());
    CHECK(!check("\\input{/etc/passwd}").empty());
    CHECK(!check("x \\end{document}").empty());
    CHECK(!check("\\immediate\\write18{ls}").empty());

    GROUP("mathtex:prepare");
    CHECK(prepare("\\mathbb{1}[i \\in S]", false) == "\\mathds{1}[i \\in S]");
    CHECK(prepare("\\mathbb 1_A", false) == "\\mathds 1_A");
    CHECK(prepare("\\mathbb{R}^d \\mathbb{1}", false) == "\\mathbb{R}^d \\mathds{1}");
    CHECK(prepare("\\mathbbm{1}_A", false) == "\\mathds{1}_A");
    CHECK(prepare("\\begin{align*}a &= b\\\\ c &= d\\end{align*}", true) ==
          "\\begin{aligned}a &= b\\\\ c &= d\\end{aligned}");
    CHECK(prepare("\\begin{gather}a\\\\b\\end{gather}", true) ==
          "\\begin{gathered}a\\\\b\\end{gathered}");
    CHECK(prepare("\\begin{equation}E = mc^2\\end{equation}", true) == "E = mc^2");
    CHECK(prepare("\\begin{alignat}{2}a&=b\\end{alignat}", true) ==
          "\\begin{alignedat}{2}a&=b\\end{alignedat}");
    CHECK(prepare("E = mc^2 \\tag{3}", true) == "E = mc^2 \\qquad\\text{(3)}");
    CHECK(prepare("E = mc^2 \\tag*{A}", true) == "E = mc^2 \\qquad\\text{A}");
    CHECK(prepare("E \\tag{3}", false) == "E \\tag{3}");   // inline: left to TeX
    CHECK(prepare("\\label{eq:one} a = b \\nonumber", true) == " a = b ");
    CHECK(prepare("a\n\n   \nb\n", true) == "a\nb");
    // Unicode: a command in math, \ensuremath in text.
    CHECK(prepare("α + β ≤ γ", false) == "\\alpha  + \\beta  \\leq  \\gamma ");
    CHECK(prepare("x_α", false) == "x_\\alpha ");
    CHECK(prepare("\\text{for all α}", false) == "\\text{for all \\ensuremath{\\alpha}}");
    CHECK(prepare("x ∈ ℝ^{n×d}", false) == "x \\in  \\mathbb{R} ^{n\\times d}");
    CHECK(prepare("a − b", false) == "a - b");
    CHECK(prepare("日本", false) == "日本");   // left alone

    GROUP("mathtex:document");
    const std::string doc = document({{"x^2", false}, {"\\sum_i a_i", true}, {"y % note", false}});
    CHECK(count(doc, "MC:BEGIN:") == 3 && count(doc, "\\mcship{") == 3);
    CHECK(doc.find("\\setbox\\mcbox\\hbox{$\\displaystyle\n\\sum_i a_i\n$}") != std::string::npos);
    CHECK(doc.find("\\setbox\\mcbox\\hbox{$\nx^2\n$}") != std::string::npos);
    CHECK(doc.find("y % note\n$}") != std::string::npos);   // the comment ends before the $
    CHECK(doc.find("\\usepackage{amsmath,amssymb,amsfonts") != std::string::npos);
    CHECK(doc.find("\\typeout{MC:END}\n\\end{document}") != std::string::npos);

    GROUP("mathtex:log");
    const std::string log =
        "(math.tex LaTeX2e\n"
        "MC:BEGIN:0\n"
        "(umsa.fd) (umsb.fd)\n"
        "MC:0:1:47.89156pt:8.49002pt:4.11543pt\n"
        "[1]\n"
        "MC:BEGIN:1\n"
        "! Undefined control sequence.\n"
        "<recently read> \\notacommand \n"
        "                               \n"
        "l.29 \\notacommand\n"
        "! Missing } inserted.\n"
        "MC:1:2:5.71527pt:4.30554pt:0.0pt\n"
        "MC:BEGIN:2\n"
        "! LaTeX Error: \\begin{document} ended by \\end{cases}.\n"
        "MC:2:3:5.0pt:6.0pt:1.5pt\n"
        "MC:BEGIN:3\n"
        "MC:END\n"
        "! Emergency stop.\n";
    const std::vector<Box> boxes = readLog(log, 5);
    CHECK(boxes.size() == 5);
    CHECK(boxes[0].reached && boxes[0].measured && boxes[0].error.empty() && boxes[0].page == 1);
    CHECK(std::fabs(boxes[0].width - 47.89156) < 1e-6 && std::fabs(boxes[0].depth - 4.11543) < 1e-6);
    CHECK(boxes[1].measured && boxes[1].error == "Undefined control sequence: \\notacommand");
    CHECK(boxes[2].error == "\\begin{document} ended by \\end{cases}");
    CHECK(boxes[3].reached && !boxes[3].measured && boxes[3].error.empty());   // after MC:END
    CHECK(!boxes[4].reached && !boxes[4].measured);
    // The culprit named on the line where TeX read it.
    const std::vector<Box> l2 = readLog("MC:BEGIN:0\n! Undefined control sequence.\n"
                                        "l.12 $\\foo\n", 1);
    CHECK(l2[0].error == "Undefined control sequence: \\foo");
    CHECK(firstError("note: downloading\nerror: batch.tex:12: Undefined control sequence\n"
                     "error: another\n") == "batch.tex:12: Undefined control sequence");
    CHECK(firstError("all fine\n").empty());
    CHECK(firstError("thread 'main' (1) panicked at src/config.rs:154:18:\n"
                     "called `Result::unwrap()` on an `Err` value: this bundle isn't cached, "
                     "and we couldn't get it from the internet. Error: error sending request "
                     "for url (https://relay.example/bundle)\n"
                     "note: run with `RUST_BACKTRACE=1`\n") ==
          "this bundle isn't cached, and we couldn't get it from the internet.");
}

void testMarkdownLines() {
    GROUP("md:lines");
    const std::string doc =
        "# H\n"            // 0
        "\n"               // 1
        "para\n"           // 2
        "lazy\n"           // 3
        "- item\n"         // 4
        "cont\n"           // 5
        "\n"               // 6
        "[r]: /x\n"        // 7
        "<!-- c -->\n"     // 8
        "T\n"              // 9
        "===\n"            // 10
        "```\n"            // 11
        "code\n"           // 12
        "```\n";           // 13
    auto L = MarkdownParser::lines(doc);
    CHECK(L.size() == 15);
    if (L.size() < 15) return;
    CHECK(L[0].kind == MdLine::Heading && L[1].kind == MdLine::Blank);
    CHECK(L[2].kind == MdLine::Text && L[3].kind == MdLine::Text && L[2].block == L[3].block);
    CHECK(L[4].kind == MdLine::ListItem && L[5].kind == MdLine::Text && L[4].block == L[5].block);
    CHECK(L[2].block != L[4].block);
    CHECK(L[7].kind == MdLine::Hidden && L[8].kind == MdLine::Hidden && L[7].block < 0);
    CHECK(L[9].kind == MdLine::Heading && L[10].kind == MdLine::SetextUnderline);
    CHECK(L[11].kind == MdLine::Fence && L[12].kind == MdLine::Code && L[13].kind == MdLine::Fence);
    CHECK(L[14].kind == MdLine::Blank);
    // Offsets: the heading's text, the item's text after its marker.
    CHECK(doc.substr(L[0].start, L[0].end - L[0].start) == "H");
    CHECK(doc.substr(L[4].start, L[4].end - L[4].start) == "item");
    CHECK(doc.substr(L[12].lineStart, L[12].lineEnd - L[12].lineStart) == "code");
    // \r\n line ends are not part of a line.
    auto crlf = MarkdownParser::lines("a\r\nb\r\n");
    CHECK(crlf.size() == 3 && crlf[0].lineEnd - crlf[0].lineStart == 1 && crlf[1].lineStart == 3);
}

// The corpus in tests/markdown, read as a whole: the invariants every
// document must keep, and its own links landing on its own headings.
void testMarkdownCorpus() {
    GROUP("md:corpus");
    std::ifstream f("tests/markdown/corpus.md");
    if (!f) {
        std::printf("  (tests/markdown/corpus.md not found here; corpus checks skipped)\n");
        return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    auto runs = MarkdownParser::parse(src);
    CHECK(runs.size() > 200);
    CHECK(!mdAny(runs, [](const MdRun &r) { return r.line < 0; }));
    CHECK(!mdAny(runs, [](const MdRun &r) { return r.text.empty() && !r.image; }));
    bool gapsOk = !runs.empty() && !runs.front().gap && !runs.back().gap;
    for (size_t k = 1; k < runs.size(); k++)
        if (runs[k].gap && runs[k - 1].gap) gapsOk = false;
    CHECK(gapsOk);
    // Nothing that is markup shows as text.
    CHECK(!mdAny(runs, [](const MdRun &r) {
        return !r.code && (r.text.find("<!--") != std::string::npos ||
                           r.text.find("&amp;") != std::string::npos ||
                           r.text.find("[ref-one]") != std::string::npos ||
                           r.text.find("<kbd>") != std::string::npos);
    }));
    CHECK(mdCount(runs, [](const MdRun &r) { return r.tableId > 0 && r.tableRow == 0 &&
                                                    r.tableCol == 0; }) == 3);
    // Every run that shows text maps back to a block a double-click can edit.
    bool editable = true;
    for (const MdRun &r : runs) {
        if (r.gap || r.rule || r.text.find_first_not_of(" \n") == std::string::npos) continue;
        if (r.table && r.tableCol < 0) continue;
        int line = r.line;
        if (r.table) line = r.line + r.tableRow + (r.tableRow > 0 ? 1 : 0);
        if (MarkdownEdit::blockAt(src, line, r.table ? r.tableCol : -1).kind ==
            MarkdownEdit::Block::None) {
            editable = false;
            std::printf("  no block for run '%s' on line %d\n", r.text.c_str(), line);
        }
    }
    CHECK(editable);
    // The corpus's #links name its own headings.
    std::set<std::string> anchors;
    std::string title;
    int titleLine = -1;
    for (const MdRun &r : runs) {
        if (r.heading > 0 && r.line == titleLine) { title += r.text; continue; }
        if (titleLine >= 0) anchors.insert(MarkdownParser::anchor(title));
        titleLine = r.heading > 0 ? r.line : -1;
        title = r.heading > 0 ? r.text : "";
    }
    // Footnotes and their references are landing places too.
    for (const MdRun &r : runs)
        if (!r.anchor.empty()) anchors.insert(r.anchor);
    bool linked = true;
    int local = 0;
    for (const MdRun &r : runs) {
        if (!r.link || r.url.empty() || r.url[0] != '#') continue;
        local++;
        if (!anchors.count(r.url.substr(1))) {
            linked = false;
            std::printf("  no heading for link %s\n", r.url.c_str());
        }
    }
    CHECK(linked && local >= 5);
    // Its footnotes: three references to two notes, listed at the end, and
    // one to nothing, which stays as written.
    CHECK(mdCount(runs, [](const MdRun &r) { return r.footnote > 0; }) == 3);
    CHECK(mdCount(runs, [](const MdRun &r) { return r.footnotes && r.marker; }) == 2);
    CHECK(runs.back().footnotes);
    CHECK(mdAny(runs, [](const MdRun &r) {
        return r.text.find("[^missing]") != std::string::npos;
    }));
    // Its HTML block's text is not Markdown.
    CHECK(mdAny(runs, [](const MdRun &r) {
        return !r.italic && r.text.find("*Not emphasis*") != std::string::npos;
    }));
    // Its math section.
    CHECK(mdCount(runs, [](const MdRun &r) { return r.math == 1; }) == 3);
    CHECK(mdCount(runs, [](const MdRun &r) { return r.math == 2; }) == 3);

    GROUP("md:math-corpus");
    std::ifstream mf("tests/markdown/math.md");
    if (!mf) return;
    std::stringstream ms;
    ms << mf.rdbuf();
    const std::string math = ms.str();
    auto mruns = MarkdownParser::parse(math);
    const int inl = mdCount(mruns, [](const MdRun &r) { return r.math == 1; });
    const int disp = mdCount(mruns, [](const MdRun &r) { return r.math == 2; });
    CHECK(inl == 32 && disp == 11);
    if (inl != 32 || disp != 11) std::printf("  math.md: %d inline, %d display\n", inl, disp);
    // No delimiter is left in the text, besides the ones meant as text.
    CHECK(!mdAny(mruns, [](const MdRun &r) {
        return !r.code && (r.text.find("$$") != std::string::npos ||
                           r.text.find("\\(") != std::string::npos ||
                           r.text.find("\\[") != std::string::npos);
    }));
    CHECK(mdAny(mruns, [](const MdRun &r) {
        return !r.math && r.text.find("costs $5 and $10, a price of $20,000, $x$") !=
                              std::string::npos;
    }));
    // Every formula opens a block when double-clicked, and a display
    // formula of its own opens the whole $$ block.
    bool mathEditable = true;
    for (const MdRun &r : mruns) {
        if (!r.math) continue;
        int line = r.line;
        if (r.table) line = r.line + r.tableRow + (r.tableRow > 0 ? 1 : 0);
        MarkdownEdit::Block b = MarkdownEdit::blockAt(math, line, r.table ? r.tableCol : -1);
        if (b.kind == MarkdownEdit::Block::None) {
            mathEditable = false;
            std::printf("  no block for formula '%s' on line %d\n", r.text.c_str(), line);
        }
        if (b.kind == MarkdownEdit::Block::Math &&
            math.substr(b.start, b.end - b.start).find(r.text) == std::string::npos)
            mathEditable = false;
    }
    CHECK(mathEditable);
}

}  // namespace

// ---------------------------------------------------------- terminal stream
namespace {

// Plays events back the way the panel does: ended lines are committed, the
// live line is replaced by each new Line event.
struct Panel {
    std::vector<std::vector<TermRun>> lines;   // ended lines
    std::vector<TermRun> live;
    std::string events;                        // trace: L l A C D P
    int lastStatus = -1;
    std::string lastDir;

    void apply(const std::vector<TermEvent> &evs) {
        for (const TermEvent &e : evs) {
            switch (e.kind) {
            case TermEvent::Line:
                events += e.ended ? 'L' : 'l';
                if (e.ended) { lines.push_back(e.runs); live.clear(); }
                else live = e.runs;
                break;
            case TermEvent::PromptStart:  events += 'A'; break;
            case TermEvent::CommandStart: events += 'C'; break;
            case TermEvent::Title:        events += 'T'; break;
            case TermEvent::CommandEnd:
                events += 'D'; lastStatus = e.status; break;
            case TermEvent::Directory:
                events += 'P'; lastDir = e.text; break;
            case TermEvent::InputStart:   events += 'B'; break;
            }
        }
    }
    static std::string plain(const std::vector<TermRun> &runs) {
        std::string s;
        for (const TermRun &r : runs) s += r.text;
        return s;
    }
    // Everything on screen as plain text, lines joined with \n.
    std::string text() const {
        std::string s;
        for (const auto &l : lines) s += plain(l) + "\n";
        return s + plain(live);
    }
};

Panel run(const std::string &input) {
    TerminalStream ts;
    Panel p;
    p.apply(ts.feed(input));
    return p;
}

// Feed `input` one byte at a time, the worst case for split sequences.
Panel runBytewise(const std::string &input) {
    TerminalStream ts;
    Panel p;
    for (char c : input) p.apply(ts.feed(&c, 1));
    return p;
}

// The style of the run containing `needle` anywhere in the panel.
const TermStyle *styleOf(const Panel &p, const std::string &needle) {
    auto search = [&](const std::vector<TermRun> &runs) -> const TermStyle * {
        for (const TermRun &r : runs)
            if (r.text.find(needle) != std::string::npos) return &r.style;
        return nullptr;
    };
    for (const auto &l : p.lines)
        if (const TermStyle *s = search(l)) return s;
    return search(p.live);
}

bool isIndexed(const TermColor &c, int index) {
    return c.kind == TermColor::Indexed && c.index == index;
}

void testTerminalStream() {
    GROUP("term:text");
    {
        Panel p = run("hello\r\nworld\r\n");
        CHECK(p.text() == "hello\nworld\n");    // \r\n ends a line
        CHECK(p.events == "LL");
        Panel partial = run("prompt> ");       // no newline: a live line
        CHECK(partial.events == "l" && partial.text() == "prompt> ");
        CHECK(run("a\r\n\r\nb").text() == "a\n\nb");   // empty lines survive
    }

    GROUP("term:escapes-dropped");
    {
        // Cursor visibility, other-line movement, charset select: no text.
        CHECK(run("\x1b[?25lok\x1b[2A\x1b[10;5H\x1b(B!\x1b[?25h").text() == "ok!");
        // An OSC we don't use (window title) and a DCS string vanish too.
        CHECK(run("\x1b]0;my title\x07" "a\x1bPq#0;2\x1b\\b").text() == "ab");
    }

    GROUP("term:sgr-basic");
    {
        Panel p = run("\x1b[31mred\x1b[0m plain \x1b[1;4;32mbold\x1b[22m under\x1b[m\n");
        CHECK(p.text() == "red plain bold under\n");
        const TermStyle *red = styleOf(p, "red");
        CHECK(red && isIndexed(red->fg, 1) && !red->bold);
        const TermStyle *plain = styleOf(p, "plain");
        CHECK(plain && *plain == TermStyle());
        const TermStyle *bold = styleOf(p, "bold");
        CHECK(bold && bold->bold && bold->underline && isIndexed(bold->fg, 2));
        const TermStyle *under = styleOf(p, "under");   // 22 ends bold only
        CHECK(under && !under->bold && under->underline && isIndexed(under->fg, 2));
        CHECK(p.lines.size() == 1 && p.lines[0].size() == 4);  // one run per style
    }

    GROUP("term:sgr-colors");
    {
        // Bright, 256-color (both separators), truecolor, backgrounds, resets.
        Panel p = run("\x1b[91ma\x1b[38;5;213mb\x1b[38:5:81mc"
                      "\x1b[38;2;10;20;30md\x1b[38:2::1:2:3me"
                      "\x1b[44;103mf\x1b[39;49mg\x1b[2;3;7;9mh\x1b[23;27;29mi");
        CHECK(p.text() == "abcdefghi");
        CHECK(isIndexed(styleOf(p, "a")->fg, 9));
        CHECK(isIndexed(styleOf(p, "b")->fg, 213));
        CHECK(isIndexed(styleOf(p, "c")->fg, 81));
        const TermColor &d = styleOf(p, "d")->fg;
        CHECK(d.kind == TermColor::RGB && d.r == 10 && d.g == 20 && d.b == 30);
        const TermColor &e = styleOf(p, "e")->fg;
        CHECK(e.kind == TermColor::RGB && e.r == 1 && e.g == 2 && e.b == 3);
        const TermStyle *f = styleOf(p, "f");
        CHECK(isIndexed(f->bg, 11) && e.kind == TermColor::RGB);
        const TermStyle *g = styleOf(p, "g");
        CHECK(g->fg.kind == TermColor::Default && g->bg.kind == TermColor::Default);
        const TermStyle *h = styleOf(p, "h");
        CHECK(h->dim && h->italic && h->inverse && h->strike);
        const TermStyle *i = styleOf(p, "i");
        CHECK(i->dim && !i->italic && !i->inverse && !i->strike);
        // Style carries across lines, as in a real terminal.
        Panel carry = run("\x1b[35mone\ntwo\x1b[0m");
        CHECK(isIndexed(styleOf(carry, "two")->fg, 5));
        // A malformed extended color is ignored rather than misread.
        Panel bad = run("\x1b[38;5mx");
        CHECK(styleOf(bad, "x")->fg.kind == TermColor::Default);
    }

    GROUP("term:palette");
    {
        TermColor c; c.kind = TermColor::Indexed;
        c.index = 1;   CHECK(c.rgb() == 0xCD3131);
        c.index = 16;  CHECK(c.rgb() == 0x000000);   // cube corner
        c.index = 196; CHECK(c.rgb() == 0xFF0000);   // cube pure red
        c.index = 213; CHECK(c.rgb() == 0xFF87FF);
        c.index = 232; CHECK(c.rgb() == 0x080808);   // gray ramp ends
        c.index = 255; CHECK(c.rgb() == 0xEEEEEE);
        TermColor rgb; rgb.kind = TermColor::RGB; rgb.r = 1; rgb.g = 2; rgb.b = 3;
        CHECK(rgb.rgb() == 0x010203);
    }

    GROUP("term:line-editing");
    {
        // \r then text overwrites in place; leftovers stay unless erased.
        CHECK(run("10%\r20%\n").text() == "20%\n");
        CHECK(run("abcdef\rXY\n").text() == "XYcdef\n");
        CHECK(run("downloading 99%\r\x1b[Kdone\n").text() == "done\n");
        // Erase to start of line, whole line, and N characters.
        CHECK(run("abcdef\x1b[3D\x1b[1K\n").text() == "    ef\n");
        CHECK(run("abcdef\x1b[2Kxy\n").text() == "      xy\n");
        CHECK(run("abcdef\r\x1b[2C\x1b[2X\n").text() == "ab  ef\n");
        // Backspace moves left; the next character overwrites.
        CHECK(run("ab\bc\n").text() == "ac\n");
        CHECK(run("|\b/\b-\n").text() == "-\n");
        // Cursor to column (1-based), forward past the end pads with spaces.
        CHECK(run("hello\x1b[1GJ\n").text() == "Jello\n");
        CHECK(run("a\x1b[3Cb\n").text() == "a   b\n");
        // Tabs move to the next multiple of 8, filling only past the end.
        CHECK(run("ab\tc\n").text() == "ab      c\n");
        CHECK(run("abcdefghij\r\tX\n").text() == "abcdefghXj\n");
        // A spinner redraw keeps its color on the rewritten character.
        Panel spin = run("\x1b[36m|\x1b[0m\r\x1b[36m/\x1b[0m");
        CHECK(spin.text() == "/" && isIndexed(styleOf(spin, "/")->fg, 6));
        // Combining marks join the previous character's cell.
        CHECK(run("e\xCC\x81x\b!\n").text() == "e\xCC\x81!\n");
    }

    GROUP("term:live-line-updates");
    {
        // Across reads, the live line is re-sent whole and replaced.
        TerminalStream ts;
        Panel p;
        p.apply(ts.feed("50%"));
        p.apply(ts.feed("\r75%"));
        CHECK(p.events == "ll" && p.text() == "75%");
        p.apply(ts.feed("\r\n"));
        CHECK(p.events == "llL" && p.text() == "75%\n");
        // Moving the cursor alone changes nothing on screen: no event.
        CHECK(ts.feed("\r").empty());
        // The host breaking the line starts a fresh one.
        p.apply(ts.feed("typed"));
        ts.breakLine();
        Panel q;
        q.apply(ts.feed("next"));
        CHECK(q.text() == "next");
        // A huge unbroken line is ended automatically.
        Panel big = run(std::string(TerminalStream::kMaxLineCells + 10, 'x'));
        CHECK(big.lines.size() == 1 &&
              Panel::plain(big.lines[0]).size() == TerminalStream::kMaxLineCells);
        CHECK(Panel::plain(big.live).size() == 10);
    }

    GROUP("term:split-across-reads");
    {
        std::string input = "\x1b[38;5;213mpink\x1b[0m caf\xC3\xA9 \xE2\x9C\x93\r\n"
                            "50%\r\x1b[Kdone\r\n"
                            "\x1b]133;D;7\x07\x1b]7;file://h/tmp\x1b\\";
        Panel whole = run(input);
        Panel bytes = runBytewise(input);
        CHECK(whole.text() == "pink caf\xC3\xA9 \xE2\x9C\x93\ndone\n");
        CHECK(bytes.text() == whole.text());
        CHECK(isIndexed(styleOf(bytes, "pink")->fg, 213));
        CHECK(bytes.lastStatus == 7 && bytes.lastDir == "/tmp");
        // Half a UTF-8 character is held back, not shown as garbage.
        TerminalStream ts;
        Panel p;
        p.apply(ts.feed("x\xE2\x9C"));
        CHECK(p.text() == "x");
        p.apply(ts.feed("\x93"));
        CHECK(p.text() == "x\xE2\x9C\x93");
    }

    GROUP("term:invalid-utf8");
    {
        CHECK(run("a\xFF" "b\xC0\xAF" "c\n").text() ==
              "a\xEF\xBF\xBD" "b\xEF\xBF\xBD\xEF\xBF\xBD" "c\n");
        // A sequence cut short by ESC or ASCII becomes one replacement.
        CHECK(run("\xE2\x9C" "a\xE2\x1b[31mb\n").text() ==
              "\xEF\xBF\xBD" "a\xEF\xBF\xBD" "b\n");
        CHECK(run("\xED\xA0\x80!\n").text() == "\xEF\xBF\xBD!\n");  // surrogate
    }

    GROUP("term:shell-integration");
    {
        TerminalStream ts;
        Panel p;
        p.apply(ts.feed("\x1b]133;A\x07"));
        CHECK(p.events == "A");
        p.apply(ts.feed("\x1b]133;C\x07out\r\npartial\x1b]133;D;130\x07"
                        "\x1b]7;file://Host.local/Users/me/my%20dir%25\x07\x1b]133;A\x07"));
        CHECK(p.events == "ACLlDPA");    // the live line is sent before D
        CHECK(p.text() == "out\npartial");
        CHECK(p.lastStatus == 130);
        CHECK(p.lastDir == "/Users/me/my dir%");
        // D without a status, and ST (ESC \) instead of BEL as terminator.
        auto noStatus = ts.feed("\x1b]133;D\x1b\\");
        CHECK(noStatus.size() == 1 && noStatus[0].kind == TermEvent::CommandEnd &&
              noStatus[0].status == 0);
        // B: the line editor reads input (the grid mode's integration).
        auto b = ts.feed("\x1b]133;B\x07");
        CHECK(b.size() == 1 && b[0].kind == TermEvent::InputStart);
        // The command line from OSC 633;E travels with the next C, once.
        auto c = ts.feed("\x1b]633;E;ls -la ~/my dir;x\x07\x1b]133;C\x07");
        CHECK(c.size() == 1 && c[0].kind == TermEvent::CommandStart &&
              c[0].text == "ls -la ~/my dir;x");
        auto c2 = ts.feed("\x1b]133;C\x07");
        CHECK(c2.size() == 1 && c2[0].text.empty());
        CHECK(ts.feed("\x1b]7;file://nohostpath\x07").empty());
    }

    GROUP("term:robustness");
    {
        // An OSC that never terminates is abandoned; output resumes after its
        // eventual terminator instead of being swallowed forever.
        TerminalStream ts;
        Panel p;
        p.apply(ts.feed("\x1b]133;" + std::string(20000, 'x')));
        CHECK(p.text().empty());
        p.apply(ts.feed("\x07visible"));
        CHECK(p.text() == "visible");
        // ESC inside an OSC starts a new sequence.
        CHECK(run("\x1b]0;title\x1b]133;A\x07").events == "A");
        // An overlong CSI is ignored whole, not half-applied.
        std::string longCsi = "\x1b[" + std::string(600, '1') + "mz";
        Panel lc = run(longCsi);
        CHECK(lc.text() == "z" && *styleOf(lc, "z") == TermStyle());
        // Other control characters are dropped.
        CHECK(run("a\x07\x01" "b").text() == "ab");
    }
}

}  // namespace

// ------------------------------------------------------- terminal screen
namespace {

// Real output, recorded with `script -q` from programs on a 40x10 terminal
// with TERM=xterm-256color.
//
// vim -u NONE -N -i NONE f.txt (f.txt = "line one\nline two\n"), typed:
// "iHello <Esc>:wq<Enter>".
const std::string kVimEdit =
    "\033[\?1049h\033[>4;2m\033[\?1h\033=\033[\?2004h\033[\?1004h\033["
    "1;10r\033[\?12h\033[\?12l\033[22;2t\033[22;1t\033[27m\033[23m\033"
    "[29m\033[m\033[H\033[2J\033[\?25l\033[10;1H\"f.txt\" 2L, 18B\033["
    "2;1H\342\226\275\033[6n\033[2;1H  \033[3;1H\033Pzz\033\\\033[0%m\033"
    "[6n\033[3;1H           \033[1;1H\033[>c\033[1;1Hline one\r\n"
    "line two\033[2;9H\033[K\033[3;1H\033[94m~                        "
    "               \033[4;1H~                                       \033"
    "[5;1H~                                       \033[6;1H~          "
    "                             \033[7;1H~                          "
    "             \033[8;1H~                                       \033"
    "[9;1H~                                       \033[1;1H\033[\?25h\033"
    "[\?4m\033[\?25l\033[m\033[10;1H\033[1m-- INSERT --\033[m\033[10;1"
    "3H\033[K\033[10;1H\033[K\033[1;6H\rHello line one\033[10;1H\033[1"
    "m-- INSERT --\033[1;7H\033[m\033[10;30H^[\033[1;6H\033[\?25h\033["
    "\?25l\033[10;30H  \033[1;7H\033[10;1H\033[K\033[1;6H\033[\?25h\033"
    "[\?25l\033[10;1H:wq\r\033[\?2004l\033[>4;m\"f.txt\" 2L, 24B writt"
    "en\r\033[23;2t\033[23;1t\r\r\n"
    "\033[\?1004l\033[\?2004l\033[\?1l\033>\033[\?1049l\033[\?25h\033["
    ">4;m";

// vim on a 100-line file ("row 1".."row 100"), typed: Ctrl+E three times,
// "dd", "Onew<Esc>", ":q!<Enter>". Scrolls with a scroll region and redraws
// lines by overwriting only the characters that differ.
const std::string kVimScroll =
    "\033[\?1049h\033[>4;2m\033[\?1h\033=\033[\?2004h\033[\?1004h\033["
    "1;10r\033[\?12h\033[\?12l\033[22;2t\033[22;1t\033[27m\033[23m\033"
    "[29m\033[m\033[H\033[2J\033[\?25l\033[10;1H\"long2.txt\" 100L, 69"
    "2B\033[2;1H\342\226\275\033[6n\033[2;1H  \033[3;1H\033Pzz\033\\\033"
    "[0%m\033[6n\033[3;1H           \033[1;1H\033[>c\033[1;1Hrow 1\r\n"
    "row 2\033[2;6H\033[K\033[3;1Hrow 3\033[3;6H\033[K\033[4;1Hrow 4\r"
    "\n"
    "row 5\r\n"
    "row 6\r\n"
    "row 7\r\n"
    "row 8\r\n"
    "row 9\033[1;1H\033[\?25h\033[\?4m\033[\?25l\033[1;9r\033[9;1H\r\n"
    "\033[1;10r\033[9;1Hrow 10\033[10;1H\033[K\033[1;1H\033[\?25h\033["
    "\?25l\033[1;9r\033[9;1H\r\n"
    "\033[1;10r\033[9;1Hrow 11\033[1;1H\033[\?25h\033[\?25l\033[10;30H"
    "^E\033[1;1H\033[10;30H  \033[1;1H\033[1;9r\033[9;1H\r\n"
    "\033[1;10r\033[9;1Hrow 12\033[1;1H\033[\?25h\033[\?25l\033[10;30H"
    "dd\033[1;1H\033[10;30H  \033[1;1H\033[1;9r\033[9;1H\r\n"
    "\033[1;10r\033[9;1Hrow 13\033[1;1H\033[\?25h\033[\?25l\033[10;1H\033"
    "[1m-- INSERT --\033[m\033[10;1H\033[K\033[1;3H\rne\033[1;5H\033[K"
    "\033[2;5H5\r\n"
    "row 6\r\n"
    "row 7\r\n"
    "row 8\r\n"
    "row 9\033[6;6H\033[K\033[7;6H0\033[8;6H1\033[9;6H2\r\n"
    "\033[1m-- INSERT --\033[1;4H\033[m\033[10;30H^[\033[1;3H\033[\?25"
    "h\033[\?25l\033[10;30H  \033[1;4H\033[10;1H\033[K\033[1;3H\033[\?"
    "25h\033[\?25l\033[10;1H:q!\r\033[\?2004l\033[>4;m\033[23;2t\033[2"
    "3;1t\033[10;1H\033[K\033[10;1H\033[\?1004l\033[\?2004l\033[\?1l\033"
    ">\033[\?1049l\033[\?25h\033[>4;m";

// less on the same 100-line file, typed: Space, q.
const std::string kLess =
    "\033[\?1049h\033[10;1H\033[\?1h\033=\rrow 1\r\n"
    "row 2\r\nrow 3\r\nrow 4\r\nrow 5\r\nrow 6\r\nrow 7\r\nrow 8\r\nrow 9\r\n"
    "\033[7mlong.txt\033[27m\033[K\r\033[Krow 10\r\n"
    "row 11\r\nrow 12\r\nrow 13\r\nrow 14\r\nrow 15\r\nrow 16\r\nrow 17\r\n"
    "row 18\r\n"
    ":\033[K\r\033[K\033[\?1l\033>\033[\?1049l";

// Feed `data` up to (not including) the first `marker`.
void feedUntil(TerminalScreen &s, const std::string &data, const std::string &marker) {
    size_t at = data.find(marker);
    s.feed(data.substr(0, at));
}

// Rows [from, to) as text, one string per row.
std::vector<std::string> rowsOf(const TerminalScreen &s, int from, int to) {
    std::vector<std::string> out;
    for (int r = from; r < to; r++) out.push_back(s.rowText(r));
    return out;
}

std::vector<std::string> numberedRows(const std::string &prefix, int first, int last) {
    std::vector<std::string> out;
    for (int i = first; i <= last; i++) out.push_back(prefix + std::to_string(i));
    return out;
}

TerminalScreen screenAfter(const std::string &bytes, int cols = 10, int rows = 4) {
    TerminalScreen s(cols, rows);
    s.feed(bytes);
    return s;
}

void testTerminalScreen() {
    GROUP("screen:vim-capture");
    {
        TerminalScreen s(40, 10);
        feedUntil(s, kVimEdit, "\033[10;1H:wq");
        CHECK(s.altScreen());
        CHECK(s.appCursorKeys() && s.appKeypad() && s.bracketedPaste());
        CHECK(s.rowText(0) == "Hello line one");
        CHECK(s.rowText(1) == "line two");
        bool tildes = true;
        for (int r = 2; r <= 8; r++) tildes = tildes && s.rowText(r) == "~";
        CHECK(tildes);
        CHECK(s.rowText(9) == "");             // "-- INSERT --" was erased
        CHECK(isIndexed(s.cell(2, 0).style.fg, 12));   // ~ in bright blue
        CHECK(s.cell(0, 0).style == TermStyle());
        CHECK(s.cursorRow() == 0 && s.cursorCol() == 5);
        // vim asked where the cursor was (after an ambiguous-width probe).
        CHECK(s.takeReplies() == "\033[2;2R\033[3;1R");
        CHECK(s.takeReplies().empty());
        // "\e[0%m" and "\e[>4;2m" are not SGR: nothing turned bold or odd.
        CHECK(!s.cell(0, 0).style.bold && !s.cell(0, 0).style.underline);
        s.feed(kVimEdit.substr(kVimEdit.find("\033[10;1H:wq")));
        CHECK(!s.altScreen());
        CHECK(!s.appCursorKeys() && !s.appKeypad() && !s.bracketedPaste());
        CHECK(s.cursorVisible());
        CHECK(s.text().empty());               // the shell's screen, untouched
        CHECK(s.cursorRow() == 0 && s.cursorCol() == 0);
    }

    GROUP("screen:vim-scroll-capture");
    {
        TerminalScreen s(40, 10);
        feedUntil(s, kVimScroll, "\033[10;30Hdd");
        CHECK(rowsOf(s, 0, 9) == numberedRows("row ", 4, 12));   // three Ctrl+E
        CHECK(s.scrollTop() == 0 && s.scrollBottom() == 9);
        feedUntil(s, kVimScroll.substr(kVimScroll.find("\033[10;30Hdd")),
                  "\033[1m-- INSERT --");
        CHECK(rowsOf(s, 0, 9) == numberedRows("row ", 5, 13));   // dd
        TerminalScreen whole(40, 10);
        feedUntil(whole, kVimScroll, "\033[10;1H:q!");
        std::vector<std::string> want = {"new"};
        for (const std::string &r : numberedRows("row ", 5, 12)) want.push_back(r);
        CHECK(rowsOf(whole, 0, 9) == want);                      // Onew<Esc>
        CHECK(whole.cursorRow() == 0 && whole.cursorCol() == 2);
        // Byte at a time gives the same screen.
        TerminalScreen bytes(40, 10);
        std::string upTo = kVimScroll.substr(0, kVimScroll.find("\033[10;1H:q!"));
        for (char c : upTo) bytes.feed(&c, 1);
        CHECK(bytes.text() == whole.text());
        whole.feed(kVimScroll.substr(kVimScroll.find("\033[10;1H:q!")));
        CHECK(!whole.altScreen() && whole.text().empty());
    }

    GROUP("screen:less-capture");
    {
        TerminalScreen s(40, 10);
        s.feed("$ ls\r\nhello\r\n$ ");                 // on the main screen first
        feedUntil(s, kLess, "\033[\?1049l");
        CHECK(s.altScreen());
        CHECK(rowsOf(s, 0, 9) == numberedRows("row ", 10, 18));
        s.feed("\033[\?1049l");
        CHECK(!s.altScreen());
        CHECK(s.text() == "$ ls\nhello\n$");         // main screen restored
        CHECK(s.cursorRow() == 2 && s.cursorCol() == 2);
        // The log model keeps less's pages out of the scrollback.
        TerminalStream ts;
        std::string logged;
        for (const TermEvent &e : ts.feed("before\r\n" + kLess + "after\r\n"))
            if (e.kind == TermEvent::Line)
                for (const TermRun &r : e.runs) logged += r.text + (e.ended ? "|" : "");
        CHECK(logged.find("row") == std::string::npos);
        CHECK(logged.find("before") != std::string::npos &&
              logged.find("after") != std::string::npos);
        CHECK(!ts.altScreen());
    }

    GROUP("screen:cursor-addressing");
    {
        TerminalScreen s(10, 5);
        s.feed("\033[3;4HX");                  // CUP is 1-based
        CHECK(s.rowText(2) == "   X" && s.cursorRow() == 2 && s.cursorCol() == 4);
        s.feed("\033[2AY");                    // CUU
        CHECK(s.rowText(0) == "    Y");
        s.feed("\033[9BZ");                    // CUD clamps to the last row
        CHECK(s.rowText(4) == "     Z");
        s.feed("\033[3D!");                    // CUB
        CHECK(s.rowText(4) == "   ! Z");
        s.feed("\033[20C#");                   // CUF clamps to the last column
        CHECK(s.cell(4, 9).ch == "#");
        s.feed("\033[1G<\033[2d^");            // CHA, VPA
        CHECK(s.rowText(4)[0] == '<' && s.rowText(1) == " ^");
        s.feed("\033[H\033[2E.\033[F,");       // CNL, CPL move to column 0
        CHECK(s.rowText(2) == ".  X" && s.rowText(1) == ",^");
        s.feed("\033[;5H*");                   // empty parameter = default
        CHECK(s.rowText(0) == "    *");
        s.feed("\033[99;99H");
        CHECK(s.cursorRow() == 4 && s.cursorCol() == 9);
    }

    GROUP("screen:erase");
    {
        auto s = screenAfter("abcdefghij\r\nklmnopqrst\r\nuvwxyz\033[2;5H");
        TerminalScreen el0 = s; el0.feed("\033[K");
        CHECK(el0.rowText(1) == "klmn");
        TerminalScreen el1 = s; el1.feed("\033[1K");
        CHECK(el1.rowText(1) == "     pqrst");
        TerminalScreen el2 = s; el2.feed("\033[2K");
        CHECK(el2.rowText(1) == "" && el2.rowText(0) == "abcdefghij");
        TerminalScreen ed0 = s; ed0.feed("\033[J");
        CHECK(ed0.text() == "abcdefghij\nklmn");
        TerminalScreen ed1 = s; ed1.feed("\033[1J");
        CHECK(ed1.text() == "\n     pqrst\nuvwxyz");
        TerminalScreen ed2 = s; ed2.feed("\033[2J");
        CHECK(ed2.text().empty() && ed2.cursorRow() == 1 && ed2.cursorCol() == 4);
        TerminalScreen ech = s; ech.feed("\033[3X");
        CHECK(ech.rowText(1) == "klmn   rst" && ech.cursorCol() == 4);
        // Erase paints the current background (xterm's bce).
        TerminalScreen bce = s; bce.feed("\033[44m\033[K");
        CHECK(isIndexed(bce.cell(1, 7).style.bg, 4) && bce.cell(1, 7).ch == " ");
        CHECK(bce.cell(1, 3).style.bg.kind == TermColor::Default);
    }

    GROUP("screen:insert-delete");
    {
        auto s = screenAfter("abcdefghij\r\nklmnopqrst\r\nuvwxyz\033[1;3H");
        TerminalScreen ich = s; ich.feed("\033[2@");
        CHECK(ich.rowText(0) == "ab  cdefgh");
        TerminalScreen dch = s; dch.feed("\033[3P");
        CHECK(dch.rowText(0) == "abfghij");
        TerminalScreen il = s; il.feed("\033[2;5H\033[L");
        CHECK(il.text() == "abcdefghij\n\nklmnopqrst\nuvwxyz");
        CHECK(il.cursorCol() == 0);
        TerminalScreen dl = s; dl.feed("\033[M");
        CHECK(dl.text() == "klmnopqrst\nuvwxyz");
        TerminalScreen dl9 = s; dl9.feed("\033[2;1H\033[9M");
        CHECK(dl9.text() == "abcdefghij");
        // Insert mode (IRM) pushes the rest of the line right.
        TerminalScreen irm = s; irm.feed("\033[4hXY\033[4lZ");
        CHECK(irm.rowText(0) == "abXYZdefgh");
    }

    GROUP("screen:scroll-region");
    {
        // Rows 2-4 scroll; rows 1 and 5 stay put.
        auto s = screenAfter("top\r\na\r\nb\r\nc\r\nbottom\033[2;4r", 10, 5);
        CHECK(s.scrollTop() == 1 && s.scrollBottom() == 3);
        CHECK(s.cursorRow() == 0 && s.cursorCol() == 0);   // DECSTBM homes
        TerminalScreen lf = s; lf.feed("\033[4;1H\nd");
        CHECK(lf.text() == "top\nb\nc\nd\nbottom");
        TerminalScreen ind = s; ind.feed("\033[4;1H\033D");
        CHECK(ind.text() == "top\nb\nc\n\nbottom");
        TerminalScreen ri = s; ri.feed("\033[2;1H\033M");
        CHECK(ri.text() == "top\n\na\nb\nbottom");
        TerminalScreen su = s; su.feed("\033[2S");
        CHECK(su.text() == "top\nc\n\n\nbottom");
        TerminalScreen sd = s; sd.feed("\033[T");
        CHECK(sd.text() == "top\n\na\nb\nbottom");
        // IL/DL act inside the region only.
        TerminalScreen il = s; il.feed("\033[3;1H\033[L");
        CHECK(il.text() == "top\na\n\nb\nbottom");
        TerminalScreen outside = s; outside.feed("\033[5;1H\033[L");
        CHECK(outside.text() == "top\na\nb\nc\nbottom");
        // CUU/CUD stop at the margins when starting inside the region.
        TerminalScreen cuu = s; cuu.feed("\033[3;1H\033[9A");
        CHECK(cuu.cursorRow() == 1);
        TerminalScreen cud = s; cud.feed("\033[3;1H\033[9B");
        CHECK(cud.cursorRow() == 3);
        // An invalid region is ignored; no parameters resets it.
        TerminalScreen bad = s; bad.feed("\033[4;2r");
        CHECK(bad.scrollTop() == 1 && bad.scrollBottom() == 3);
        TerminalScreen reset = s; reset.feed("\033[r");
        CHECK(reset.scrollTop() == 0 && reset.scrollBottom() == 4);
        // Origin mode addresses rows from the top margin.
        TerminalScreen om = s; om.feed("\033[\?6h\033[1;1HX\033[9;1H");
        CHECK(om.rowText(1) == "X" && om.cursorRow() == 3);
    }

    GROUP("screen:alternate");
    {
        auto s = screenAfter("main\r\ntext\033[31m", 10, 4);
        TerminalScreen a = s;
        a.feed("\033[\?1049h");
        CHECK(a.altScreen() && a.text().empty());
        a.feed("\033[32m\033[3;3Halt");
        a.feed("\033[\?1049l");
        CHECK(!a.altScreen() && a.text() == "main\ntext");
        CHECK(a.cursorRow() == 1 && a.cursorCol() == 4);   // cursor restored
        a.feed("x");
        CHECK(isIndexed(a.cell(1, 4).style.fg, 1));          // and its style
        // 1049 always starts with a clear alternate screen.
        a.feed("\033[\?1049h");
        CHECK(a.text().empty());
        // 47 switches without saving the cursor or clearing.
        TerminalScreen b = s;
        b.feed("\033[\?47hAB\033[\?47l");
        CHECK(b.text() == "main\ntext" && b.cursorCol() == 6);
        b.feed("\033[\?47h");
        CHECK(b.text() == "\n    AB");                // kept from last time
        // 1047 clears the alternate screen when leaving it.
        TerminalScreen c = s;
        c.feed("\033[\?1047hAB\033[\?1047l\033[\?1047h");
        CHECK(c.text().empty());
    }

    GROUP("screen:save-restore");
    {
        TerminalScreen s(10, 4);
        s.feed("\033[2;3H\033[1;33m\0337\033[H\033[0mx\0338y");
        CHECK(s.rowText(1) == "  y" && s.cell(1, 2).style.bold &&
              isIndexed(s.cell(1, 2).style.fg, 3));
        CHECK(s.cell(0, 0).style == TermStyle());
        s.feed("\033[4;4H\033[s\033[H\033[uz");        // SCOSC / SCORC
        CHECK(s.rowText(3) == "   z");
        // Restoring with nothing saved goes home.
        TerminalScreen fresh(10, 4);
        fresh.feed("\033[3;3H\0338");
        CHECK(fresh.cursorRow() == 0 && fresh.cursorCol() == 0);
    }

    GROUP("screen:autowrap");
    {
        TerminalScreen s(5, 3);
        s.feed("abcde");
        // The cursor waits on the last column: the wrap is pending.
        CHECK(s.cursorRow() == 0 && s.cursorCol() == 4 && s.rowText(1) == "");
        TerminalScreen cr = s; cr.feed("\rX");
        CHECK(cr.rowText(0) == "Xbcde" && cr.rowText(1) == "");
        TerminalScreen more = s; more.feed("f");
        CHECK(more.rowText(0) == "abcde" && more.rowText(1) == "f");
        TerminalScreen lf = s; lf.feed("\r\nz");        // no blank line between
        CHECK(lf.text() == "abcde\nz");
        TerminalScreen el = s; el.feed("\033[K!");      // erase clears the pending wrap
        CHECK(el.rowText(0) == "abcd!" && el.rowText(1) == "");
        // Wrapping at the bottom scrolls.
        TerminalScreen bottom(5, 2);
        bottom.feed("12345678901234");
        CHECK(bottom.text() == "67890\n1234");
        // With autowrap off, the last column is overwritten.
        TerminalScreen off(5, 2);
        off.feed("\033[\?7labcdefg");
        CHECK(off.text() == "abcdg" && !off.autowrap());
    }

    GROUP("screen:tabs");
    {
        TerminalScreen s(20, 2);
        s.feed("a\tb\tc\td");
        CHECK(s.rowText(0) == "a       b       c  d");   // last stop clamps
        TerminalScreen h(20, 2);
        h.feed("\033[3g\033[4G\033H\033[1G\tx");        // clear all, set at col 4
        CHECK(h.rowText(0) == "   x");
        TerminalScreen cbt(20, 2);
        cbt.feed("\033[18G\033[Zy\033[2Zz");
        CHECK(cbt.rowText(0) == "        z       y");
        TerminalScreen cht(20, 2);
        cht.feed("\033[2Iq");
        CHECK(cht.cursorCol() == 17);
    }

    GROUP("screen:wide-chars");
    {
        TerminalScreen s(6, 2);
        s.feed("\xE4\xB8\xAD\xE6\x96\x87!");                  // 中文!
        CHECK(s.cell(0, 0).width == 2 && s.cell(0, 1).width == 0);
        CHECK(s.cell(0, 2).ch == "\xE6\x96\x87" && s.cell(0, 4).ch == "!");
        CHECK(s.cursorCol() == 5 && s.rowText(0) == "\xE4\xB8\xAD\xE6\x96\x87!");
        // A wide character that does not fit wraps whole.
        s.feed("\xE5\xAD\x97");                               // 字
        CHECK(s.rowText(0) == "\xE4\xB8\xAD\xE6\x96\x87!" &&
              s.rowText(1) == "\xE5\xAD\x97");
        // Overwriting half of a wide character blanks the other half.
        TerminalScreen o(6, 2);
        o.feed("\xE4\xB8\xAD\xE6\x96\x87\033[1;2Hx");
        CHECK(o.rowText(0) == " x\xE6\x96\x87");
        // Emoji are wide, combining marks join the previous cell.
        TerminalScreen e(6, 2);
        e.feed("\xF0\x9F\x98\x80" "e\xCC\x81.");
        CHECK(e.cell(0, 0).width == 2 && e.cell(0, 2).ch == "e\xCC\x81" &&
              e.cell(0, 3).ch == ".");
        CHECK(termCharWidth(0x4E2D) == 2 && termCharWidth('a') == 1 &&
              termCharWidth(0x0301) == 0 && termCharWidth(0x2713) == 1);
    }

    GROUP("screen:line-drawing");
    {
        TerminalScreen s(10, 2);
        s.feed("\033(0lqk\033(Bq\016x\017x");
        CHECK(s.rowText(0) == "\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90qxx");
        TerminalScreen g1(10, 2);
        g1.feed("\033)0a\016q\017q");
        CHECK(g1.rowText(0) == "a\xE2\x94\x80q");
        // REP repeats the last character.
        TerminalScreen rep(10, 2);
        rep.feed("-\033[4b|");
        CHECK(rep.rowText(0) == "-----|");
    }

    GROUP("screen:modes-and-replies");
    {
        TerminalScreen s(10, 4);
        CHECK(s.cursorVisible() && !s.appCursorKeys() && !s.bracketedPaste());
        s.feed("\033[\?25l\033[\?1h\033=\033[\?2004h");
        CHECK(!s.cursorVisible() && s.appCursorKeys() && s.appKeypad() &&
              s.bracketedPaste());
        s.feed("\033[\?1;2004l\033>\033[\?25h");        // several at once
        CHECK(s.cursorVisible() && !s.appCursorKeys() && !s.appKeypad() &&
              !s.bracketedPaste());
        s.feed("\033[3;7H\033[6n\033[5n\033[c\033[>c\033[\?6n");
        CHECK(s.takeReplies() == "\033[3;7R\033[0n\033[\?1;2c");
        s.feed("\033]0;my title\007");
        CHECK(s.title() == "my title");
        // Shell integration, which Android's terminal reads from the grid:
        // OSC 7 names the directory, OSC 133;D counts finished commands.
        CHECK(s.directory().empty() && s.commandsEnded() == 0);
        s.feed("\033]7;file://localhost/storage/emulated/0/my%20dir\007");
        CHECK(s.directory() == "/storage/emulated/0/my dir");
        s.feed("\033]7;/tmp\033\\\033]133;D;0\007\033]133;A\007\033]133;D\007");
        CHECK(s.directory() == "/tmp" && s.commandsEnded() == 2);
        s.feed("\033]7;file://host-without-path\007\033]7;\007");
        CHECK(s.directory() == "/tmp");
        CHECK(termOsc7Path("7;file://h/a%2Fb") == "/a/b" &&
              termOsc7Path("0;title").empty());
        // Private and intermediate sequences never reach SGR.
        s.feed("\033[>4;2m\033[\?4m\033[0%m\033[2 qA");
        CHECK(s.cell(2, 6).style == TermStyle() && s.cell(2, 6).ch == "A");
        // Full reset.
        s.feed("\033[31m\033[\?1049h\033c");
        CHECK(!s.altScreen() && s.text().empty() && s.cursorRow() == 0);
    }

    GROUP("screen:resize");
    {
        TerminalScreen s(10, 4);
        s.feed("one\r\ntwo\r\nthree\r\nfour");
        s.resize(10, 2);                     // the cursor's line stays on screen
        CHECK(s.text() == "three\nfour" && s.cursorRow() == 1 && s.cursorCol() == 4);
        // Narrower: the lines wrap again (the scrollback is off here, so the
        // rows pushed up are gone), the cursor staying after "four".
        s.resize(3, 3);
        CHECK(s.text() == "ee\nfou\nr" && s.rows() == 3);
        CHECK(s.cursorRow() == 2 && s.cursorCol() == 1);
        CHECK(!s.lineWraps(0) && s.lineWraps(1) && !s.lineWraps(2));
        s.resize(6, 3);                      // wider: joined again
        CHECK(s.cols() == 6 && s.text() == "ee\nfour");
        CHECK(s.cursorRow() == 1 && s.cursorCol() == 4);
        CHECK(s.scrollBottom() == 2);
        // A wide character that no longer fits goes whole to the next row.
        TerminalScreen w(4, 1);
        w.feed("a\xE4\xB8\xAD");
        w.resize(2, 1);
        CHECK(w.rowText(0) == "\xE4\xB8\xAD" && w.cell(0, 0).width == 2);
        // On the alternate screen it is cut: a blank.
        TerminalScreen wa(4, 1);
        wa.feed("\033[?1049ha\xE4\xB8\xAD");
        wa.resize(2, 1);
        CHECK(wa.rowText(0) == "a");
        // The alternate screen resizes too.
        TerminalScreen alt(10, 4);
        alt.feed("\033[\?1049h\033[4;1Hlast");
        alt.resize(8, 2);
        CHECK(alt.altScreen() && alt.rowText(1) == "last");
    }

    GROUP("screen:scrollback");
    {
        // Off until the host sets a limit: nothing kept, as before.
        TerminalScreen none(10, 3);
        none.feed("1\r\n2\r\n3\r\n4");
        CHECK(none.historySize() == 0 && none.historyBase() == 1);
        CHECK(none.text() == "2\n3\n4");

        TerminalScreen s(10, 3);
        s.setScrollbackLimit(5);
        s.feed("1\r\n2\r\n3\r\n4\r\n5");
        CHECK(s.historySize() == 2 && s.historyBase() == 2);
        CHECK(s.lineText(-2) == "1" && s.lineText(-1) == "2");
        CHECK(s.lineText(0) == "3" && s.text() == "3\n4\n5");
        CHECK(s.lineCell(-2, 0).ch == "1" && s.lineCell(-2, 9).ch == " ");
        CHECK(s.lineCell(-3, 0).ch == " " && s.lineCell(5, 0).ch == " ");
        // The limit drops the oldest lines, and line numbers keep counting.
        s.feed("\r\n6\r\n7\r\n8\r\n9");
        CHECK(s.historySize() == 5 && s.historyBase() == 6);
        CHECK(s.lineText(-5) == "2" && s.lineText(-1) == "6");
        s.setScrollbackLimit(2);
        CHECK(s.historySize() == 2 && s.lineText(-2) == "5");
        // Selection text runs from the scrollback into the screen.
        CHECK(s.textBetween(-2, 0, 1, 9) == "5\n6\n7\n8");
        CHECK(s.textBetween(1, 0, -1, 0) == "6\n7\n8");
        // Colors are kept.
        TerminalScreen col(10, 2);
        col.setScrollbackLimit(10);
        col.feed("\033[31mred\033[m\r\nx\r\ny");
        CHECK(col.lineText(-1) == "red" && col.lineCell(-1, 0).style.fg.index == 1);
        // Trailing default blanks are not stored; colored ones are.
        CHECK(col.lineWidth(-1) == 3);
        TerminalScreen bg(10, 1);
        bg.setScrollbackLimit(10);
        bg.feed("a\033[41m  \033[m\r\n");
        CHECK(bg.lineWidth(-1) == 3 && bg.lineCell(-1, 2).style.bg.index == 1);
        CHECK(bg.textBetween(-1, 0, -1, 6) == "a" && bg.lineCell(-1, 7).ch == " ");

        // A scroll region that does not start at the top keeps its lines out
        // (a status line program), and so does the alternate screen.
        TerminalScreen reg(10, 4);
        reg.setScrollbackLimit(10);
        reg.feed("\033[2;4r\033[4;1Ha\r\nb\r\nc");
        CHECK(reg.historySize() == 0);
        reg.feed("\033[r\033[4;1H\r\n");      // region reset: saved again
        CHECK(reg.historySize() == 1);
        TerminalScreen alt(10, 2);
        alt.setScrollbackLimit(10);
        alt.feed("\033[?1049h1\r\n2\r\n3\r\n4\033[?1049l");
        CHECK(alt.historySize() == 0);
        // Reverse index, inserting and deleting lines, and SU/SD never save.
        TerminalScreen ops(10, 3);
        ops.setScrollbackLimit(10);
        ops.feed("a\r\nb\r\nc\033[1;1H\033M\033[M\033[L\033[2S\033[T");
        CHECK(ops.historySize() == 0);

        // ED 3 (clear's "erase saved lines") empties the scrollback only;
        // ED 2 leaves it alone.
        TerminalScreen ed(10, 2);
        ed.setScrollbackLimit(10);
        ed.feed("1\r\n2\r\n3");
        ed.feed("\033[2J");
        CHECK(ed.historySize() == 1 && ed.text().empty());
        ed.feed("\033[H\033[2Jz\033[3J");
        CHECK(ed.historySize() == 0 && ed.text() == "z");
        // A full reset keeps it (it describes what was printed, not the screen).
        TerminalScreen ris(10, 2);
        ris.setScrollbackLimit(10);
        ris.feed("1\r\n2\r\n3\033c");
        CHECK(ris.historySize() == 1 && ris.text().empty());
    }

    GROUP("screen:scrollback-resize");
    {
        // Rows taken away go into the scrollback, and come back when the
        // screen grows again, the cursor moving with its line.
        TerminalScreen s(10, 4);
        s.setScrollbackLimit(100);
        s.feed("one\r\ntwo\r\nthree\r\nfour");
        s.resize(10, 2);
        CHECK(s.text() == "three\nfour" && s.historySize() == 2);
        CHECK(s.lineText(-2) == "one" && s.cursorRow() == 1 && s.historyBase() == 2);
        s.resize(10, 5);                      // two back down, one blank below
        CHECK(s.historySize() == 0 && s.historyBase() == 0);
        CHECK(s.text() == "one\ntwo\nthree\nfour" && s.cursorRow() == 3);
        CHECK(s.cursorCol() == 4);
        // Blank rows below the cursor are cut first: nothing to save.
        TerminalScreen top(10, 6);
        top.setScrollbackLimit(100);
        top.feed("$ ");
        top.resize(10, 3);
        CHECK(top.historySize() == 0 && top.rowText(0) == "$");
        // Scrollback lines are reflowed too: narrowing wraps a long line
        // over two rows, which copy as one line, and widening joins them.
        // (A line exactly as wide as the screen, ended by CR LF, is a hard
        // line: it never wrapped.)
        TerminalScreen w(8, 2);
        w.setScrollbackLimit(10);
        w.feed("abcdefgh\r\nx\r\ny");
        CHECK(w.lineWidth(-1) == 8 && !w.lineWraps(-1));
        w.resize(4, 2);
        CHECK(w.historySize() == 2 && w.lineText(-2) == "abcd" && w.lineText(-1) == "efgh");
        CHECK(w.lineWraps(-2) && !w.lineWraps(-1));
        CHECK(w.textBetween(-2, 0, -1, 3) == "abcdefgh");
        CHECK(w.textBetween(-2, 1, -1, 0) == "bcde");
        CHECK(w.rowText(0) == "x" && w.rowText(1) == "y" && w.cursorRow() == 1);
        // A row added comes back down from the scrollback.
        w.resize(4, 3);
        CHECK(w.rowText(0) == "efgh" && w.historySize() == 1 && w.cursorRow() == 2);
        w.resize(8, 3);
        CHECK(w.historySize() == 0 && w.text() == "abcdefgh\nx\ny");
        CHECK(w.cursorRow() == 2 && w.cursorCol() == 1);
        // On the alternate screen the main screen behind it keeps its
        // cursor's line too (the one saved on entering), and gets its rows
        // back after; the alternate screen itself never touches the
        // scrollback.
        TerminalScreen a(10, 4);
        a.setScrollbackLimit(10);
        a.feed("p1\r\np2\r\np3\r\n$ \033[?1049h\033[Hvim");
        a.resize(10, 2);
        CHECK(a.altScreen() && a.historySize() == 2 && a.rowText(0) == "vim");
        a.feed("\033[?1049l");
        CHECK(a.rowText(1) == "$" && a.cursorRow() == 1 && a.cursorCol() == 2);
        a.feed("\033[?1049h");
        a.resize(10, 4);
        CHECK(a.historySize() == 0);
        a.feed("\033[?1049l");
        CHECK(a.rowText(3) == "$" && a.cursorRow() == 3 && a.rowText(0) == "p1");
    }

    GROUP("screen:scrollback-packed");
    {
        // Scrollback lines are stored packed (text plus style runs); every
        // cell must read back as it was on the screen. Random lines of
        // ASCII, accents with combining marks, wide characters and colors:
        // before each line leaves the top, its row is copied, and after,
        // lineCell(-1, c) must give the same cells.
        std::mt19937 rng(7);
        const char *pieces[] = {"a", "Z", "0", " ", "é", "e\xCC\x81", "日", "本",
                                "\xF0\x9F\x98\x80", "─", "\t", "x"};
        const char *styles[] = {"\033[m", "\033[31m", "\033[1;44m", "\033[38;5;208m",
                                "\033[48;2;1;2;3m", "\033[4;7m", "\033[9;2m"};
        TerminalScreen s(23, 3);
        s.setScrollbackLimit(1000);
        s.feed("\033[?7l");   // no autowrap: a long line must not scroll by itself
        bool same = true, widths = true;
        int compared = 0;
        for (int line = 0; line < 400 && same; line++) {
            s.feed("\033[3;1H\033[m\033[2K");
            int n = (int)(rng() % 30);
            for (int i = 0; i < n; i++) {
                if (rng() % 4 == 0) s.feed(styles[rng() % 7]);
                s.feed(pieces[rng() % 12]);
            }
            s.feed("\033[1;1H");
            std::vector<TermCell> row;
            for (int c = 0; c < s.cols(); c++) row.push_back(s.cell(0, c));
            int keep = (int)row.size();
            while (keep > 0 && row[keep - 1].ch == " " && row[keep - 1].width == 1 &&
                   row[keep - 1].style == TermStyle())
                keep--;
            s.feed("\033[3;1H\n");
            if (s.lineWidth(-1) != keep) widths = false;
            for (int c = 0; c < s.cols() + 2; c++) {
                TermCell want = c < keep ? row[c] : TermCell();
                TermCell got = s.lineCell(-1, c);
                if (got.ch != want.ch || got.width != want.width || got.style != want.style) {
                    same = false;
                    std::printf("    line %d col %d: '%s' w%d, want '%s' w%d\n", line, c,
                                got.ch.c_str(), got.width, want.ch.c_str(), want.width);
                    break;
                }
                compared++;
            }
        }
        CHECK(same && widths && compared > 9000);
        // A line with wide and combining characters comes back down whole.
        TerminalScreen w(8, 2);
        w.setScrollbackLimit(10);
        w.feed("\033[32m日e\xCC\x81\033[m!\r\nx\r\ny");
        CHECK(w.historySize() == 1 && w.lineText(-1) == "日e\xCC\x81!");
        CHECK(w.lineCell(-1, 0).width == 2 && w.lineCell(-1, 1).width == 0 &&
              w.lineCell(-1, 1).ch.empty() && w.lineCell(-1, 2).ch == "e\xCC\x81");
        CHECK(w.lineCell(-1, 2).style.fg.index == 2 && w.lineCell(-1, 3).style == TermStyle());
        w.resize(8, 3);
        CHECK(w.historySize() == 0 && w.rowText(0) == "日e\xCC\x81!");
        CHECK(w.cell(0, 0).width == 2 && w.cell(0, 1).width == 0 &&
              w.cell(0, 0).style.fg.index == 2 && w.cell(0, 3).ch == "!");
    }

    GROUP("screen:scrollback-memory");
    {
        // 10,000 full-width lines in a background color (a status line, a
        // test runner's colored bar), and 10,000 lines of ls-like colored
        // words: a TermCell per cell took about 48 bytes, so the first
        // alone was some 53 MB.
        const int kCols = 111, kLines = 10000;
        TerminalScreen bar(kCols, 5);
        bar.setScrollbackLimit(kLines);
        std::string row = "\033[44m PASS  tests/run_tests.cpp\033[K";
        for (int i = 0; i < kLines + 5; i++) bar.feed(row + "\033[m\r\n");
        TerminalScreen ls(kCols, 5);
        ls.setScrollbackLimit(kLines);
        std::string words;
        for (int w = 0; w < 9; w++)
            words += (w % 3 ? "\033[1;34m" : "\033[32m") + std::string("name") +
                     std::to_string(w) + "\033[m      ";
        for (int i = 0; i < kLines + 5; i++) ls.feed(words + "\r\n");
        CHECK(bar.historySize() == kLines && ls.historySize() == kLines);
        CHECK(bar.lineWidth(-1) == kCols && bar.lineCell(-1, kCols - 1).style.bg.index == 4);
        size_t barOld = bar.historyCellBytes(), barNew = bar.historyBytes();
        size_t lsOld = ls.historyCellBytes(), lsNew = ls.historyBytes();
        CHECK(barNew * 20 < barOld && lsNew * 8 < lsOld);
        std::printf("  scrollback: %d colored %d-column lines take %.1f MB "
                    "(%.1f MB as cells); %d ls-like lines %.1f MB (%.1f MB)\n",
                    kLines, kCols, barNew / 1048576.0, barOld / 1048576.0, kLines,
                    lsNew / 1048576.0, lsOld / 1048576.0);
    }

    GROUP("screen:selection-text");
    {
        auto s = screenAfter("abc\r\ndefgh\r\nij", 10, 3);
        CHECK(s.textBetween(0, 1, 1, 2) == "bc\ndef");
        CHECK(s.textBetween(1, 2, 0, 1) == "bc\ndef");     // either direction
        CHECK(s.textBetween(2, 0, 2, 9) == "ij");
        CHECK(s.textBetween(0, 0, 2, 9) == "abc\ndefgh\nij");
    }

    GROUP("screen:find");
    {
        TerminalScreen s(20, 3);
        s.setScrollbackLimit(100);
        s.feed("Error one\r\nok\r\nerror two ERROR\r\nfine\r\nlast error");
        // Lines: -2 "Error one", -1 "ok", 0 "error two ERROR", 1 "fine", 2 "last error".
        auto m = s.find("error", false);
        CHECK(m.size() == 4);
        CHECK(m.size() == 4 && m[0] == (TermMatch{-2, 0, 5, -2, 0 + 5}) &&
              m[1] == (TermMatch{0, 0, 5, 0, 0 + 5}) && m[2] == (TermMatch{0, 10, 5, 0, 10 + 5}) &&
              m[3] == (TermMatch{2, 5, 5, 2, 5 + 5}));
        auto exact = s.find("error", true);
        CHECK(exact.size() == 2 && exact[0].line == 0 && exact[1].line == 2);
        CHECK(s.find("", false).empty() && s.find("nothing", false).empty());
        CHECK(s.find("error", false, 2).size() == 2);           // the limit
        // Overlapping text: matches do not overlap.
        auto aa = screenAfter("aaaa", 10, 2).find("aa", false);
        CHECK(aa.size() == 2 && aa[0].col == 0 && aa[1].col == 2);
        // Wide characters, a combining mark and accented capitals: columns are
        // cells, and the match covers both halves of a wide character.
        TerminalScreen w(20, 2);
        w.setScrollbackLimit(10);
        w.feed("x\xe4\xb8\xad\xe6\x96\x87y \xc3\x89T\xc3\x89\r\n");   // x中文y ÉTÉ
        auto wm = w.find("\xe4\xb8\xad\xe6\x96\x87y", false);
        CHECK(wm.size() == 1 && wm[0] == (TermMatch{0, 1, 5, 0, 1 + 5}));
        auto em = w.find("\xc3\xa9t\xc3\xa9", false);            // été
        CHECK(em.size() == 1 && em[0] == (TermMatch{0, 7, 3, 0, 7 + 3}));
        w.feed("cafe\xcc\x81 cafe\r\n");                         // café (e + U+0301), cafe
        auto cm = w.find("cafe", false);
        // "cafe" + a combining accent is not "cafe": only the plain one counts.
        CHECK(cm.size() == 1 && cm[0] == (TermMatch{0, 5, 4, 0, 5 + 4}));
        // In the scrollback too, packed lines with wide characters included.
        w.feed("\r\n\r\n");
        auto hm = w.find("\xe4\xb8\xad\xe6\x96\x87y", false);
        CHECK(hm.size() == 1 && hm[0].line < 0 && hm[0].col == 1 && hm[0].width == 5);
        CHECK(w.lineText(hm[0].line).find("\xe4\xb8\xad") != std::string::npos);
        // Greek and Cyrillic fold; other scripts are matched as written.
        CHECK(screenAfter("\xce\xa3\xce\x9f\xce\xa6 \xd0\x9c\xd0\x98\xd0\xa0", 20, 2)
                  .find("\xcf\x83\xce\xbf\xcf\x86 \xd0\xbc\xd0\xb8\xd1\x80", false).size() == 1);
        // The alternate screen is searched alone: its own rows, no scrollback.
        s.feed("\033[?1049h\033[Hvim error");
        auto am = s.find("error", false);
        CHECK(am.size() == 1 && am[0] == (TermMatch{0, 4, 5, 0, 4 + 5}));
        s.feed("\033[?1049l");
        CHECK(s.find("error", false).size() == 4);
        // A long scrollback stays quick enough to search as one types.
        TerminalScreen big(100, 30);
        big.setScrollbackLimit(10000);
        std::string chunk;
        for (int i = 0; i < 10000; i++)
            chunk += "line " + std::to_string(i) + " of the build output, warning here\r\n";
        big.feed(chunk);
        auto t0 = std::chrono::steady_clock::now();
        auto bm = big.find("WARNING", false);
        double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        CHECK(bm.size() == 10000);
        std::printf("  screen:find  10,000 matches in 10,000 lines: %.1f ms\n", ms);
    }

    GROUP("screen:sync-output");
    {
        TerminalScreen s(10, 3);
        CHECK(!s.synchronizedOutput());
        s.feed("\033[?2026$p");                         // DECRQM: supported, reset
        CHECK(s.takeReplies() == "\033[?2026;2$y");
        s.feed("\033[?2026h");
        CHECK(s.synchronizedOutput());
        s.feed("\033[?2026$p");
        CHECK(s.takeReplies() == "\033[?2026;1$y");
        s.feed("frame\033[?2026l");
        CHECK(!s.synchronizedOutput() && s.rowText(0) == "frame");
        CHECK(s.synchronizedFramesEnded() == 1);
        s.feed("\033[?2026l\033[?2026h\033[?2026h\033[?2026l\033[?2026h");
        CHECK(s.synchronizedFramesEnded() == 2 && s.synchronizedOutput());
        s.feed("\033[?2026l");
        // Split across reads, as a pty delivers it.
        s.feed("\033[?20");
        CHECK(!s.synchronizedOutput());
        s.feed("26h");
        CHECK(s.synchronizedOutput());
        // A full reset ends it, so a program that dies mid-frame cannot hold
        // the screen.
        s.feed("\033c");
        CHECK(!s.synchronizedOutput());
        // Other modes report their state, unknown ones 0, ANSI modes too.
        s.feed("\033[?2004h\033[?2004$p\033[?25$p\033[?1000$p\033[4$p\033[4h\033[4$p");
        CHECK(s.takeReplies() ==
              "\033[?2004;1$y\033[?25;1$y\033[?1000;2$y\033[4;2$y\033[4;1$y");
        // DECRQM never reaches the screen as text or a mode change.
        CHECK(s.rowText(0).empty() && s.bracketedPaste());
    }

    GROUP("screen:clear");
    {
        TerminalScreen s(10, 4);
        s.setScrollbackLimit(100);
        s.feed("1\r\n2\r\n3\r\n4\r\n5\r\n$ ls");
        CHECK(s.historySize() == 2 && s.cursorRow() == 3);
        TerminalScreen running = s;
        s.clearAll();
        CHECK(s.historySize() == 0 && s.text().empty());
        CHECK(s.cursorRow() == 0 && s.cursorCol() == 0);
        // A running program: the cursor's line goes to the top, with what is
        // below it; the scrollback goes.
        running.feed("\033[2;1H");                     // cursor on "4"
        running.feed("\033[4;1Hx\033[2;1H");
        running.clearToCursorLine();
        CHECK(running.historySize() == 0);
        CHECK(running.rowText(0) == "4" && running.rowText(1) == "5" &&
              running.rowText(2) == "x ls" && running.rowText(3).empty());
        CHECK(running.cursorRow() == 0);
        // Output after it continues from there.
        running.feed("\r\nnext");
        CHECK(running.rowText(1) == "next");
        // On the alternate screen only the scrollback goes.
        TerminalScreen alt(10, 3);
        alt.setScrollbackLimit(10);
        alt.feed("a\r\nb\r\nc\r\nd\033[?1049h\033[2;1Hvim");
        alt.clearAll();
        alt.clearToCursorLine();
        CHECK(alt.historySize() == 0 && alt.rowText(1) == "vim" && alt.cursorRow() == 1);
        alt.feed("\033[?1049l");
        CHECK(alt.text() == "b\nc\nd");
    }

    GROUP("screen:keys");
    {
        using K = TermKey;
        CHECK(TerminalScreen::encodeKey(K::Up, 0, false) == "\033[A");
        CHECK(TerminalScreen::encodeKey(K::Up, 0, true) == "\033OA");     // DECCKM
        CHECK(TerminalScreen::encodeKey(K::Left, 0, true) == "\033OD");
        CHECK(TerminalScreen::encodeKey(K::Home, 0, false) == "\033[H");
        CHECK(TerminalScreen::encodeKey(K::End, 0, true) == "\033OF");
        CHECK(TerminalScreen::encodeKey(K::Right, TermModCtrl, true) == "\033[1;5C");
        CHECK(TerminalScreen::encodeKey(K::Down, TermModShift | TermModAlt, false) ==
              "\033[1;4B");
        CHECK(TerminalScreen::encodeKey(K::PageUp, 0, false) == "\033[5~");
        CHECK(TerminalScreen::encodeKey(K::PageDown, TermModShift, false) == "\033[6;2~");
        CHECK(TerminalScreen::encodeKey(K::Delete, 0, false) == "\033[3~");
        CHECK(TerminalScreen::encodeKey(K::F1, 0, false) == "\033OP");
        CHECK(TerminalScreen::encodeKey(K::F4, TermModCtrl, false) == "\033[1;5S");
        CHECK(TerminalScreen::encodeKey(K::F5, 0, false) == "\033[15~");
        CHECK(TerminalScreen::encodeKey(K::F12, 0, false) == "\033[24~");
        CHECK(TerminalScreen::encodeKey(K::Enter, 0, false) == "\r");
        // Shift+Return: ESC CR, a new line in Claude Code rather than submit.
        CHECK(TerminalScreen::encodeKey(K::Enter, TermModShift, false) == "\033\r");
        CHECK(TerminalScreen::encodeKey(K::Enter, TermModShift | TermModAlt, true) == "\033\r");
        CHECK(TerminalScreen::encodeKey(K::Enter, TermModAlt, false) == "\033\r");
        CHECK(TerminalScreen::encodeKey(K::KeypadEnter, 0, false, true) == "\033OM");
        CHECK(TerminalScreen::encodeKey(K::KeypadEnter, 0, false, false) == "\r");
        CHECK(TerminalScreen::encodeKey(K::Backspace, 0, false) == "\x7f");
        CHECK(TerminalScreen::encodeKey(K::Backspace, TermModAlt, false) == "\033\x7f");
        CHECK(TerminalScreen::encodeKey(K::Tab, 0, false) == "\t");
        CHECK(TerminalScreen::encodeKey(K::Tab, TermModShift, false) == "\033[Z");
        CHECK(TerminalScreen::encodeKey(K::Escape, 0, false) == "\033");

        CHECK(TerminalScreen::encodeChar('c', TermModCtrl) == "\x03");
        CHECK(TerminalScreen::encodeChar('C', TermModCtrl) == "\x03");
        CHECK(TerminalScreen::encodeChar('[', TermModCtrl) == "\033");
        CHECK(TerminalScreen::encodeChar(' ', TermModCtrl) == std::string(1, '\0'));
        CHECK(TerminalScreen::encodeChar('/', TermModCtrl) == "\x1f");
        CHECK(TerminalScreen::encodeChar('b', TermModAlt) == "\033b");  // Meta
        CHECK(TerminalScreen::encodeChar('x', TermModAlt | TermModCtrl) == "\033\x18");
        CHECK(TerminalScreen::encodeChar(0xE9, 0) == "\xC3\xA9");
        CHECK(TerminalScreen::encodePaste("a\nb\r\nc", false) == "a\rb\rc");
        CHECK(TerminalScreen::encodePaste("x", true) == "\033[200~x\033[201~");
        // An ESC inside the brackets is shown, never sent (more in
        // terminal_paste_tests.inc).
        CHECK(TerminalScreen::encodePaste("x\033[201~y", true) ==
              "\033[200~x\xE2\x90\x9B[201~y\033[201~");
    }
}

}  // namespace

// --------------------------------------------------------------- settings
namespace {

bool near(double a, double b) { return a - b < 1e-9 && b - a < 1e-9; }

bool colorIs(const Rgba &c, uint32_t rgb, double alpha = 1.0) {
    return c.rgb() == rgb && near(c.a, alpha);
}

Settings parseSettings(const std::string &text,
                       std::vector<SettingsError> *errors = nullptr) {
    return Settings::parse(text, errors);
}

// Every line of `text` that reads "# key = value" with the "# " removed, so
// the documented keys in the default file can be checked against the parser.
std::string uncommentKeys(const std::string &text) {
    std::string out, line;
    for (size_t i = 0; i <= text.size(); i++) {
        if (i < text.size() && text[i] != '\n') { line += text[i]; continue; }
        // "# group.field = value", not prose that happens to contain " = ".
        size_t eq = line.find(" = ");
        std::string key = eq == std::string::npos ? "" : line.substr(2, eq - 2);
        bool isKey = line.rfind("# ", 0) == 0 &&
                     (key.find('.') != std::string::npos || key == "theme");
        for (char ch : key)
            if (!(ch == '.' || ch == '-' || (ch >= 'a' && ch <= 'z'))) isKey = false;
        if (isKey) out += line.substr(2) + "\n";
        line.clear();
    }
    return out;
}

void testSettings() {
    GROUP("settings:colors");
    Rgba c;
    CHECK(Settings::parseColor("#1E1E1E", c) && colorIs(c, 0x1E1E1E));
    CHECK(Settings::parseColor("#1e1e1e", c) && colorIs(c, 0x1E1E1E));
    CHECK(Settings::parseColor("#fff", c) && colorIs(c, 0xFFFFFF));
    CHECK(Settings::parseColor("#0A0", c) && colorIs(c, 0x00AA00));
    CHECK(Settings::parseColor("#00000080", c) && colorIs(c, 0x000000, 128 / 255.0));
    CHECK(Settings::parseColor("#FFFFFF00", c) && near(c.a, 0));
    CHECK(!Settings::parseColor("1E1E1E", c));      // no #
    CHECK(!Settings::parseColor("#12", c));
    CHECK(!Settings::parseColor("#12345", c));
    CHECK(!Settings::parseColor("#1234567", c));
    CHECK(!Settings::parseColor("#GGGGGG", c));
    CHECK(!Settings::parseColor("#", c));
    CHECK(!Settings::parseColor("", c));
    CHECK(!Settings::parseColor("red", c));

    GROUP("settings:opacity");
    double o = -1;
    CHECK(Settings::parseOpacity("0.5", o) && near(o, 0.5));
    CHECK(Settings::parseOpacity(".25", o) && near(o, 0.25));
    CHECK(Settings::parseOpacity("1", o) && near(o, 1));
    CHECK(Settings::parseOpacity("0", o) && near(o, 0));
    CHECK(Settings::parseOpacity("80%", o) && near(o, 0.8));
    CHECK(Settings::parseOpacity("100%", o) && near(o, 1));
    CHECK(Settings::parseOpacity("12.5%", o) && near(o, 0.125));
    CHECK(!Settings::parseOpacity("1.5", o));
    CHECK(!Settings::parseOpacity("80", o));        // a bare 80 is not 80%
    CHECK(!Settings::parseOpacity("-0.1", o));
    CHECK(!Settings::parseOpacity("101%", o));
    CHECK(!Settings::parseOpacity("%", o));
    CHECK(!Settings::parseOpacity("half", o));
    CHECK(!Settings::parseOpacity("0.5.0", o));
    CHECK(!Settings::parseOpacity("", o));

    GROUP("settings:defaults");
    Settings d = parseSettings("");
    CHECK(colorIs(d.background(Surface::Editor), 0x1E1E1E));
    CHECK(colorIs(d.background(Surface::Sidebar), 0x252526));
    CHECK(colorIs(d.background(Surface::Terminal), 0x181818));
    CHECK(colorIs(d.background(Surface::Statusbar), 0x007ACC));
    CHECK(colorIs(d.background(Surface::Browser), 0x2A2A2A));
    CHECK(colorIs(d.text(Surface::Editor), 0xD4D4D4));
    CHECK(colorIs(d.text(Surface::Sidebar), 0xCCCCCC));
    CHECK(colorIs(d.text(Surface::Statusbar), 0xFFFFFF));
    CHECK(colorIs(d.syntax(TokenStyle::Keyword), 0x569CD6));
    CHECK(colorIs(d.syntax(TokenStyle::Comment), 0x6A9955));
    CHECK(colorIs(d.syntax(TokenStyle::Plain), 0xD4D4D4));
    CHECK(colorIs(d.markdown(MarkdownColor::Link), 0x4EA1F7));
    CHECK(colorIs(d.markdownCodeBackground(), 0x2A2A2A));
    CHECK(colorIs(d.terminalInputText(), 0xEDEDED));
    CHECK(near(d.opacity(Surface::Editor), 1));
    CHECK(!d.blur());
    CHECK(d.material() == "under-window");
    CHECK(d.windowIsOpaque());
    CHECK(!d.customTitlebar());
    CHECK(!d.textIsSet(Surface::Browser));
    CHECK(std::string(Settings::surfaceName(Surface::Statusbar)) == "statusbar");

    GROUP("settings:panel-opacity");
    Settings p = parseSettings("editor.opacity = 0.6\nsidebar.opacity = 40%\n");
    CHECK(colorIs(p.background(Surface::Editor), 0x1E1E1E, 0.6));
    CHECK(colorIs(p.background(Surface::Sidebar), 0x252526, 0.4));
    CHECK(colorIs(p.background(Surface::Terminal), 0x181818, 1));  // untouched
    CHECK(colorIs(p.text(Surface::Editor), 0xD4D4D4, 1));          // text stays solid
    CHECK(near(p.markdownCodeBackground().a, 0.6));                // follows editor
    CHECK(!p.windowIsOpaque());
    CHECK(p.customTitlebar());   // a see-through window draws its own title bar
    CHECK(near(p.background(Surface::Titlebar).a, 1));

    GROUP("settings:window-opacity");
    Settings w = parseSettings("window.opacity = 0.5\nterminal.opacity = 0.9\n");
    CHECK(near(w.opacity(Surface::Editor), 0.5));
    CHECK(near(w.opacity(Surface::Titlebar), 0.5));
    CHECK(near(w.opacity(Surface::Statusbar), 0.5));
    CHECK(near(w.opacity(Surface::Terminal), 0.9));      // its own wins
    // Order doesn't matter: the panel's own opacity wins either way.
    Settings w2 = parseSettings("terminal.opacity = 0.9\nwindow.opacity = 0.5\n");
    CHECK(near(w2.opacity(Surface::Terminal), 0.9));
    // A panel can be fully opaque inside a see-through window.
    Settings w3 = parseSettings("window.opacity = 0.3\neditor.opacity = 1\n");
    CHECK(near(w3.background(Surface::Editor).a, 1));
    CHECK(near(w3.background(Surface::Sidebar).a, 0.3));

    GROUP("settings:background-alpha");
    // A color's own alpha multiplies with the opacity.
    Settings ba = parseSettings("editor.background = #10203080\n"
                                "editor.opacity = 0.5\n");
    CHECK(ba.background(Surface::Editor).rgb() == 0x102030);
    CHECK(near(ba.background(Surface::Editor).a, (128 / 255.0) * 0.5));
    Settings bb = parseSettings("statusbar.background = #333\n");
    CHECK(colorIs(bb.background(Surface::Statusbar), 0x333333));
    CHECK(bb.windowIsOpaque());
    Settings bc = parseSettings("sidebar.background = #25252600\n");
    CHECK(!bc.windowIsOpaque());

    GROUP("settings:text");
    Settings t = parseSettings("window.text = #EEEEEE\neditor.text = #00FF00\n"
                               "sidebar.text = #FF000080\n");
    CHECK(colorIs(t.text(Surface::Editor), 0x00FF00));
    CHECK(colorIs(t.text(Surface::Sidebar), 0xFF0000, 128 / 255.0));
    CHECK(colorIs(t.text(Surface::Terminal), 0xEEEEEE));   // window.text fallback
    CHECK(colorIs(t.text(Surface::Statusbar), 0xEEEEEE));
    CHECK(colorIs(t.syntax(TokenStyle::Plain), 0x00FF00)); // plain = editor.text
    CHECK(colorIs(t.terminalInputText(), 0xEEEEEE));
    CHECK(t.textIsSet(Surface::Browser));
    CHECK(t.windowIsOpaque());          // text alone never makes it see-through
    CHECK(!t.customTitlebar());
    Settings t2 = parseSettings("browser.text = #111111\n");
    CHECK(t2.textIsSet(Surface::Browser));
    CHECK(!t2.textIsSet(Surface::Editor));

    GROUP("settings:syntax-markdown");
    Settings sy = parseSettings("syntax.keyword = #FF0000\nsyntax.function = #00F\n"
                                "markdown.heading = #ABCDEF\nmarkdown.quote = #123\n");
    CHECK(colorIs(sy.syntax(TokenStyle::Keyword), 0xFF0000));
    CHECK(colorIs(sy.syntax(TokenStyle::Function), 0x0000FF));
    CHECK(colorIs(sy.syntax(TokenStyle::String), 0xCE9178));   // default kept
    CHECK(colorIs(sy.markdown(MarkdownColor::Heading), 0xABCDEF));
    CHECK(colorIs(sy.markdown(MarkdownColor::Quote), 0x112233));
    CHECK(colorIs(sy.markdown(MarkdownColor::Code), 0xCE9178));
    std::vector<SettingsError> se;
    parseSettings("syntax.plain = #FFFFFF\n", &se);   // plain is editor.text
    CHECK(se.size() == 1);

    GROUP("settings:window");
    Settings bl = parseSettings("window.blur = true\nwindow.material = HUD\n");
    CHECK(bl.blur());
    CHECK(bl.material() == "hud");
    CHECK(!bl.windowIsOpaque());
    CHECK(bl.customTitlebar());
    for (const char *v : {"on", "yes", "1", "TRUE"})
        CHECK(parseSettings(std::string("window.blur = ") + v).blur());
    for (const char *v : {"off", "no", "0", "false"})
        CHECK(!parseSettings(std::string("window.blur = true\nwindow.blur = ") + v).blur());
    Settings tb = parseSettings("titlebar.background = #000000\n");
    CHECK(tb.customTitlebar());          // any titlebar key takes it over
    CHECK(tb.windowIsOpaque());
    CHECK(colorIs(tb.background(Surface::Titlebar), 0x000000));
    Settings tb2 = parseSettings("titlebar.opacity = 0\n");
    CHECK(tb2.customTitlebar());
    CHECK(!tb2.windowIsOpaque());        // the title bar counts once it's ours

    GROUP("settings:syntax-of-file");
    std::vector<SettingsError> e1;
    Settings f = parseSettings(
        "# a comment\n"
        "   \n"
        "  # indented comment\n"
        "EDITOR.Opacity=0.5\n"                      // case, no spaces
        "sidebar.opacity =\t25%   # trailing comment\n"
        "statusbar.background = #FF0000#not-a-comment-space\n"
        "terminal.text = #00FF00\r\n"               // CRLF
        "editor.opacity = 0.7", &e1);               // no final newline
    CHECK(near(f.opacity(Surface::Editor), 0.7));   // last one wins
    CHECK(near(f.opacity(Surface::Sidebar), 0.25));
    CHECK(colorIs(f.text(Surface::Terminal), 0x00FF00));
    CHECK(e1.size() == 1);   // "#FF0000#not..." is not a color
    CHECK(e1.size() == 1 && e1[0].line == 6);
    CHECK(colorIs(f.background(Surface::Statusbar), 0x007ACC));  // default kept

    GROUP("settings:errors");
    std::vector<SettingsError> e2;
    Settings bad = parseSettings(
        "editor.opacity = 0.5\n"         // 1 ok
        "editor.opacity = 2\n"           // 2 out of range
        "editr.opacity = 0.5\n"          // 3 unknown surface
        "editor.color = #FFFFFF\n"       // 4 unknown field
        "just some words\n"              // 5 no '='
        "= #FFFFFF\n"                    // 6 no key
        "editor.text =\n"                // 7 no value
        "editor.text = #FFF extra\n"     // 8 junk after value
        "sidebar.text = white\n"         // 9 not a color
        "window.blur = maybe\n"          // 10 not a bool
        "window.material = glass\n"      // 11 unknown material
        "editor.opacity = 80\n"          // 12 bare percentage
        "editr.opacity = abc\n",         // 13 unknown beats bad value
        &e2);
    CHECK(e2.size() == 12);
    std::vector<int> lines;
    for (const SettingsError &e : e2) lines.push_back(e.line);
    CHECK((lines == std::vector<int>{2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13}));
    CHECK(near(bad.opacity(Surface::Editor), 0.5));      // bad lines ignored
    CHECK(colorIs(bad.text(Surface::Sidebar), 0xCCCCCC));
    CHECK(!bad.blur());
    CHECK(bad.material() == "under-window");
    auto has = [&](size_t i, const char *needle) {
        return i < e2.size() && e2[i].message.find(needle) != std::string::npos;
    };
    CHECK(has(0, "not an opacity"));
    CHECK(has(1, "unknown setting 'editr.opacity'"));
    CHECK(has(2, "unknown setting 'editor.color'"));
    CHECK(has(3, "expected key = value"));
    CHECK(has(7, "not a color"));
    CHECK(has(8, "not true or false"));
    CHECK(has(9, "unknown material"));
    CHECK(has(10, "80%"));                               // hints at the fix
    CHECK(has(11, "unknown setting"));
    parseSettings("editor.opacity = 2\n");              // null errors is fine

    GROUP("settings:default-file");
    std::vector<SettingsError> e3;
    Settings df = parseSettings(Settings::defaultFileText(), &e3);
    CHECK(e3.empty());
    CHECK(df.windowIsOpaque() && !df.customTitlebar() && !df.blur());
    // Every documented key, uncommented, is one the parser accepts, and its
    // documented value is the real default.
    std::string keys = uncommentKeys(Settings::defaultFileText());
    // Window, panel, syntax, markdown (colors, web images, math), lsp, run,
    // format, python, keep-sorted, auto-save, ruler, word wrap, terminal,
    // develop, git, control and agent keys (with markdown.mermaid and
    // agent.ide-link), terminal.activate-venv, search.use-ignore-files and
    // terminal.multiline-paste-warning.
    CHECK(std::count(keys.begin(), keys.end(), '\n') == 89);
    {
        GROUP("settings:key-table");
        // Every key of the table is documented in the default file (a new
        // row needs its commented default there), and is a key once.
        std::vector<std::string> table = Settings::tableKeys();
        CHECK(table.size() >= 29);
        std::set<std::string> seen;
        for (const std::string &k : table) {
            CHECK(seen.insert(k).second);
            const bool documented = ("\n" + keys).find("\n" + k + " =") != std::string::npos;
            if (!documented) std::printf("  defaultFileText lacks %s\n", k.c_str());
            CHECK(documented);
        }
        CHECK(Settings::isColorKey("editor.bracket-match") && Settings::isColorKey("window.text"));
        CHECK(!Settings::isColorKey("window.opacity") && !Settings::isColorKey("theme"));
        CHECK(Settings::isColorKey("editor.background") && Settings::isColorKey("syntax.keyword"));
        // A path keeps its spaces; a command line its quotes; a word refuses more.
        std::vector<SettingsError> ek;
        Settings p = parseSettings("python.interpreter = /a b/python3  # mine\n"
                                   "agent.command = claude --x \"a # b\" # note\n"
                                   "develop.source = auto\n", &ek);
        CHECK(ek.empty() && p.pythonInterpreter() == "/a b/python3");
        CHECK(p.agentCommand() == "claude --x \"a # b\"" && p.developSource().empty());
        parseSettings("terminal.mode = grid line\n", &ek);
        CHECK(ek.size() == 1 && ek[0].message.find("unexpected text") != std::string::npos);
        GROUP("settings:default-file");
    }
    std::vector<SettingsError> e4;
    Settings un = parseSettings(keys, &e4);
    CHECK(e4.empty());
    for (const SettingsError &e : e4)
        std::printf("    line %d: %s\n", e.line, e.message.c_str());
    for (int i = 0; i < kSurfaceCount; i++) {
        Surface s = (Surface)i;
        CHECK(un.background(s) == d.background(s));
        CHECK(un.text(s) == d.text(s));
    }
    for (int i = 1; i < 8; i++)
        CHECK(un.syntax((TokenStyle)i) == d.syntax((TokenStyle)i));
    for (int i = 0; i < kMarkdownColorCount; i++)
        CHECK(un.markdown((MarkdownColor)i) == d.markdown((MarkdownColor)i));
    CHECK(un.material() == d.material() && un.blur() == d.blur());
    CHECK(std::string(un.theme().id) == d.theme().id &&
          std::string(d.theme().id) == "dark-plus");
    // The documented servers are the first ones the built-in search tries.
    CHECK(un.lspEnabled() == d.lspEnabled());
    CHECK(un.webImages() == d.webImages());
    CHECK(un.math() == d.math());
    CHECK(un.runInNewTerminal() == d.runInNewTerminal());
    for (const char *ext : {"py", "sh", "go", "js", "rb", "pyw", "bash", "mjs", "txt", "c"})
        CHECK(un.runTemplate(ext) == d.runTemplate(ext));
    CHECK(un.pythonInterpreter().empty() && d.pythonInterpreter().empty());
    CHECK(un.keepSortedOnSave() == d.keepSortedOnSave());
    CHECK(un.organizeImportsOnSave() == d.organizeImportsOnSave() && !d.organizeImportsOnSave());
    CHECK(un.gitAutoFetch() == d.gitAutoFetch() && !d.gitAutoFetch());
    CHECK(un.gitAutoFetchPeriod() == d.gitAutoFetchPeriod() && d.gitAutoFetchPeriod() == 180);
    CHECK(un.autoSave() == d.autoSave() && d.autoSave() == Settings::AutoSave::Off);
    CHECK(un.autoSaveDelay() == d.autoSaveDelay() && d.autoSaveDelay() == 1000);
    CHECK(un.rulers() == d.rulers() && un.rulerColor() == d.rulerColor());
    CHECK(un.editorFontSize() == d.editorFontSize() && d.editorFontSize() == 13);
    CHECK(un.terminalFontSize() == d.terminalFontSize() && d.terminalFontSize() == 12);
    CHECK(un.lineNumbers() == d.lineNumbers() &&
          d.lineNumbers() == Settings::LineNumbers::On);
    CHECK(un.gutterBackground() == d.gutterBackground());
    CHECK(un.lineNumberColor() == d.lineNumberColor());
    CHECK(un.lineNumberActiveColor() == d.lineNumberActiveColor());
    CHECK(un.bracketMatchColor() == d.bracketMatchColor());
    CHECK(un.terminalLineMode() == d.terminalLineMode() && !d.terminalLineMode());
    CHECK(un.terminalScrollback() == d.terminalScrollback() &&
          d.terminalScrollback() == 10000);
    CHECK(un.terminalOptionAsMeta() == d.terminalOptionAsMeta() &&
          d.terminalOptionAsMeta());
    CHECK(un.developCommand() == d.developCommand() && d.developCommand() == "claude");
    CHECK(un.developSource().empty() && d.developSource().empty());
    CHECK(un.agentCommand() == d.agentCommand() && d.agentCommand() == "claude");
    CHECK(parseSettings("agent.command = claude --model sonnet  # quick\n").agentCommand() ==
          "claude --model sonnet");
    for (const std::string &server : Settings::lspServers()) {
        std::vector<std::string> cmds = Lsp::defaultCommands(server);
        CHECK(!cmds.empty() && un.lspCommand(server) == cmds[0]);
        CHECK(d.lspCommand(server).empty());
    }
    // The documented formatters are each language's first Google-style tool.
    CHECK(Settings::formatLanguages() == Formatter::languages());
    for (const std::string &lang : Settings::formatLanguages()) {
        CHECK(!Formatter::defaultCommand(lang).empty());
        CHECK(un.formatCommand(lang) == Formatter::defaultCommand(lang));
        CHECK(d.formatCommand(lang).empty());
    }
    CHECK(!d.formatOnSave());
    CHECK(!parseSettings(std::string(Settings::defaultFileText()) +
                         "format.on-save = false\n").formatOnSave());

    GROUP("settings:themes");
    {
        // Dark+ is the built-in look, value for value.
        const Theme *plus = Settings::findTheme("dark-plus");
        CHECK(plus && &d.theme() == plus && d.isDark());
        CHECK(d.background(Surface::Editor) == Rgba::hex(0x1E1E1E));
        CHECK(d.syntax(TokenStyle::Keyword) == Rgba::hex(0x569CD6));
        CHECK(Settings::findTheme(" Light-Plus ") == Settings::findTheme("light-plus"));
        CHECK(!Settings::findTheme("dracula"));
        // Every theme is whole: no color left at 0 by mistake (black is a
        // real color, so only the code background, ruler and the syntax
        // entries are checked against 0), dark ones dark, light ones light.
        std::set<std::string> ids;
        for (const Theme &t : Settings::themes()) {
            ids.insert(t.id);
            Settings s = parseSettings(std::string("theme = ") + t.id + "\n");
            CHECK(&s.theme() == &t);
            CHECK(s.isDark() == t.dark);
            for (int i = 1; i < 8; i++) CHECK(t.syntax[i] != 0);
            CHECK(t.codeBackground != 0 && t.ruler != 0);
            CHECK(s.background(Surface::Editor) == Rgba::hex(t.background[(int)Surface::Editor]));
            CHECK(s.text(Surface::Terminal) == Rgba::hex(t.text[(int)Surface::Terminal]));
            CHECK(s.markdown(MarkdownColor::Link) == Rgba::hex(t.markdown[(int)MarkdownColor::Link]));
            CHECK(s.syntax(TokenStyle::Plain) == s.text(Surface::Editor));
            // Every theme is named in the settings file's own comment.
            CHECK(std::string(Settings::defaultFileText()).find(t.id) != std::string::npos);
        }
        CHECK(ids.size() == Settings::themes().size());
        // A color of the file's own wins over the theme's, in either order.
        Settings m = parseSettings("syntax.string = #123456\ntheme = monokai\n");
        CHECK(m.syntax(TokenStyle::String) == Rgba::hex(0x123456));
        CHECK(m.syntax(TokenStyle::Keyword) == Rgba::hex(0xF92672));
        Settings o = parseSettings("theme = light-plus\neditor.background = #000000\n");
        CHECK(o.isDark() && !o.theme().dark);   // the editor as drawn decides
        // Opacity still applies to a theme's colors.
        Settings op = parseSettings("theme = light-modern\neditor.opacity = 50%\n");
        CHECK(op.background(Surface::Editor).a == 0.5);
        CHECK(op.markdownCodeBackground().a == 0.5);
        std::vector<SettingsError> te;
        Settings bad = parseSettings("theme = dracula\n", &te);
        CHECK(te.size() == 1 && &bad.theme() == plus);
        CHECK(te.size() == 1 && te[0].message.find("unknown theme 'dracula'") == 0 &&
              te[0].message.find("solarized-light") != std::string::npos);

        GROUP("settings:with-theme");
        CHECK(Settings::isColorKey("editor.background") && Settings::isColorKey("syntax.type"));
        CHECK(Settings::isColorKey("markdown.done") && Settings::isColorKey("window.text"));
        CHECK(Settings::isColorKey("editor.ruler-color"));
        CHECK(!Settings::isColorKey("editor.opacity") && !Settings::isColorKey("markdown.math"));
        CHECK(!Settings::isColorKey("theme") && !Settings::isColorKey("lsp.cpp"));
        // The default file: the commented line is uncommented in place.
        const std::string df2 = Settings::defaultFileText();
        std::string w = Settings::withTheme(df2, "monokai", false);
        CHECK(w.find("\ntheme = monokai\n") != std::string::npos);
        CHECK(w.find("# theme = ") == std::string::npos);
        CHECK(w.size() == df2.size() - 2 - std::string("dark-plus").size() + 7);
        CHECK(&parseSettings(w).theme() == Settings::findTheme("monokai"));
        // Set again: the active line changes, keeping its comment.
        std::string w2 = Settings::withTheme("theme = monokai  # mine\nx.y = 1\n",
                                             "light-plus", false);
        CHECK(w2 == "theme = light-plus  # mine\nx.y = 1\n");
        // Neither: a new line after the opening comment, or at the top.
        CHECK(Settings::withTheme("# my file\n# notes\n\neditor.opacity = 1\n",
                                  "monokai", false) ==
              "# my file\n# notes\n\ntheme = monokai\n\neditor.opacity = 1\n");
        CHECK(Settings::withTheme("editor.opacity = 1\n", "monokai", false) ==
              "theme = monokai\n\neditor.opacity = 1\n");
        CHECK(Settings::withTheme("", "monokai", false) == "theme = monokai\n");
        // CRLF stays CRLF.
        CHECK(Settings::withTheme("# theme = dark-plus\r\na = 1\r\n", "monokai", false) ==
              "theme = monokai\r\na = 1\r\n");
        // The file's own colors: counted, and commented out on request,
        // leaving opacity and everything else alone.
        const std::string mine = "theme = dark-plus\n  editor.background = #0d1117\n"
                                 "editor.opacity = 0.8\n# syntax.type = #FFFFFF\n"
                                 "markdown.code = #40CE2C  # green\nlsp.cpp = clangd\n";
        CHECK(Settings::colorOverrides(mine) == 2);
        std::string cleared = Settings::withTheme(mine, "solarized-light", true);
        CHECK(cleared == "theme = solarized-light\n  # editor.background = #0d1117\n"
                         "editor.opacity = 0.8\n# syntax.type = #FFFFFF\n"
                         "# markdown.code = #40CE2C  # green\nlsp.cpp = clangd\n");
        CHECK(Settings::colorOverrides(cleared) == 0);
        Settings cs = parseSettings(cleared);
        CHECK(cs.background(Surface::Editor) == Rgba::hex(0xFDF6E3, 0.8));
        CHECK(Settings::withTheme(mine, "monokai", false).find("  editor.background = #0d1117") !=
              std::string::npos);
    }

    GROUP("settings:ruler");
    {
        CHECK(d.rulers() == std::vector<int>{100});
        CHECK(d.rulerColor() == Rgba::hex(0x5A5A5A));
        CHECK(parseSettings("editor.ruler = 80\n").rulers() == std::vector<int>{80});
        CHECK(parseSettings("editor.ruler = 120,80,120\n").rulers() ==
              (std::vector<int>{80, 120}));
        CHECK(parseSettings("editor.ruler = off\n").rulers().empty());
        CHECK(parseSettings("editor.ruler = None  # no line\n").rulers().empty());
        for (const char *bad : {"editor.ruler = 0\n", "editor.ruler = 80,\n",
                                "editor.ruler = wide\n", "editor.ruler = 1001\n",
                                "editor.ruler = -5\n", "editor.ruler = 80, 120\n"}) {
            std::vector<SettingsError> re;
            Settings rs = parseSettings(bad, &re);
            CHECK(re.size() == 1 && rs.rulers() == std::vector<int>{100});
        }
        std::vector<SettingsError> re;
        parseSettings("editor.ruler = 9x\n", &re);
        CHECK(re.size() == 1 && re[0].message.find("not a column") != std::string::npos);
        Settings rc = parseSettings("editor.ruler-color = #FF000080\n");
        CHECK(rc.rulerColor().r == 255 && rc.rulerColor().a > 0.49 && rc.rulerColor().a < 0.51);
        std::vector<SettingsError> ce;
        parseSettings("editor.ruler-color = grey\n", &ce);
        CHECK(ce.size() == 1);
    }

    GROUP("settings:gutter");
    {
        // VS Code's Dark+ values: editorLineNumber.foreground and
        // activeForeground, editorBracketMatch.border.
        CHECK(d.lineNumberColor() == Rgba::hex(0x858585));
        CHECK(d.lineNumberActiveColor() == Rgba::hex(0xC6C6C6));
        CHECK(d.bracketMatchColor() == Rgba::hex(0x888888));
        // The gutter follows the editor's background, set or themed, and
        // its opacity.
        CHECK(d.gutterBackground() == d.background(Surface::Editor));
        Settings eb = parseSettings("editor.background = #000000\neditor.opacity = 50%\n");
        CHECK(eb.gutterBackground() == Rgba::hex(0x000000, 0.5));
        Settings gb = parseSettings("editor.gutter-background = #102030\n"
                                    "editor.opacity = 0.8\n");
        CHECK(gb.gutterBackground() == Rgba::hex(0x102030, 0.8));
        CHECK(gb.background(Surface::Editor) == Rgba::hex(0x1E1E1E, 0.8));
        Settings lm = parseSettings("theme = light-plus\n");
        CHECK(lm.lineNumberColor() == Rgba::hex(0x237893));
        CHECK(lm.lineNumberActiveColor() == Rgba::hex(0x0B216F));
        CHECK(lm.gutterBackground() == Rgba::hex(0xFFFFFF));
        // A color of the file's own wins over the theme's.
        Settings own = parseSettings("editor.line-number-color = #ABCDEF\ntheme = monokai\n"
                                     "editor.bracket-match = #FF000080\n"
                                     "editor.line-number-active-color = #fff\n");
        CHECK(own.lineNumberColor() == Rgba::hex(0xABCDEF));
        CHECK(own.lineNumberActiveColor() == Rgba::hex(0xFFFFFF));
        CHECK(own.bracketMatchColor().r == 255 && own.bracketMatchColor().a < 0.51);
        for (const Theme &t : Settings::themes())
            CHECK(t.lineNumber != 0 && t.lineNumberActive != 0 && t.bracketMatch != 0 &&
                  t.lineNumber != t.lineNumberActive);
        for (const char *key : {"editor.gutter-background", "editor.line-number-color",
                                "editor.line-number-active-color", "editor.bracket-match"})
            CHECK(Settings::isColorKey(key));
        CHECK(Settings::colorOverrides("editor.line-number-color = #ABCDEF\n") == 1);
        // Line numbers: on, off, relative.
        CHECK(parseSettings("editor.line-numbers = off\n").lineNumbers() ==
              Settings::LineNumbers::Off);
        CHECK(parseSettings("editor.line-numbers = Relative  # vim\n").lineNumbers() ==
              Settings::LineNumbers::Relative);
        CHECK(parseSettings("editor.line-numbers = true\n").lineNumbers() ==
              Settings::LineNumbers::On);
        std::vector<SettingsError> le;
        Settings ln = parseSettings("editor.line-numbers = interval\n", &le);
        CHECK(le.size() == 1 && ln.lineNumbers() == Settings::LineNumbers::On &&
              le[0].message.find("on, off or relative") != std::string::npos);
        // Word wrap: on by default; on, off, true or false.
        CHECK(parseSettings("").wordWrap());
        CHECK(!parseSettings("editor.word-wrap = off\n").wordWrap());
        CHECK(!parseSettings("editor.word-wrap = False\n").wordWrap());
        CHECK(parseSettings("editor.word-wrap = ON  # long lines\n").wordWrap());
        std::vector<SettingsError> we;
        Settings ww = parseSettings("editor.word-wrap = bounded\n", &we);
        CHECK(we.size() == 1 && ww.wordWrap() &&
              we[0].message.find("on or off") != std::string::npos);
        // The font size: points, decimals allowed, 6 to 72.
        CHECK(parseSettings("editor.font-size = 15\n").editorFontSize() == 15);
        CHECK(parseSettings("editor.font-size = 12.5\n").editorFontSize() == 12.5);
        CHECK(parseSettings("editor.font-size = 6\n").editorFontSize() == 6);
        CHECK(parseSettings("editor.font-size = 72\n").editorFontSize() == 72);
        for (const char *bad : {"editor.font-size = 5\n", "editor.font-size = 73\n",
                                "editor.font-size = big\n", "editor.font-size = -14\n",
                                "editor.font-size = 14pt\n"}) {
            std::vector<SettingsError> fe;
            Settings fs = parseSettings(bad, &fe);
            CHECK(fe.size() == 1 && fs.editorFontSize() == 13);
        }
        CHECK(std::string(Settings::defaultFileText()).find("# editor.font-size = 13\n") !=
              std::string::npos);
        // The terminal's font size, by the same rules.
        CHECK(parseSettings("terminal.font-size = 14\n").terminalFontSize() == 14);
        CHECK(parseSettings("terminal.font-size = 11.5\n").terminalFontSize() == 11.5);
        CHECK(parseSettings("terminal.font-size = 72\n").terminalFontSize() == 72);
        for (const char *bad : {"terminal.font-size = 5\n", "terminal.font-size = 80\n",
                                "terminal.font-size = small\n"}) {
            std::vector<SettingsError> fe;
            Settings fs = parseSettings(bad, &fe);
            CHECK(fe.size() == 1 && fs.terminalFontSize() == 12 &&
                  fs.editorFontSize() == 13);
        }
        CHECK(std::string(Settings::defaultFileText()).find("# terminal.font-size = 12\n") !=
              std::string::npos);
        // The editor's Adjust Settings marks the gutter's keys too.
        const AppearanceSurface *ed = Settings::appearanceSurface("editor");
        int gutterKeys = 0;
        for (const char *k : ed->keys)
            if (std::string(k).find("line-number") != std::string::npos ||
                std::string(k) == "editor.gutter-background" ||
                std::string(k) == "editor.bracket-match")
                gutterKeys++;
        CHECK(gutterKeys == 4);
    }

    GROUP("settings:terminal");
    {
        std::vector<SettingsError> te;
        Settings t = parseSettings("terminal.mode = line\nterminal.scrollback = 0\n"
                                   "terminal.option-as-meta = false\n", &te);
        CHECK(te.empty() && t.terminalLineMode() && t.terminalScrollback() == 0 &&
              !t.terminalOptionAsMeta());
        CHECK(!parseSettings("terminal.mode = GRID").terminalLineMode());
        CHECK(parseSettings("terminal.scrollback = 100000").terminalScrollback() == 100000);
        for (const char *bad : {"terminal.mode = tty", "terminal.scrollback = -1",
                                "terminal.scrollback = 100001", "terminal.scrollback = 1e3",
                                "terminal.option-as-meta = left", "terminal.colour = red"}) {
            std::vector<SettingsError> be;
            Settings b = parseSettings(bad, &be);
            CHECK(be.size() == 1);
            CHECK(!b.terminalLineMode() && b.terminalScrollback() == 10000 &&
                  b.terminalOptionAsMeta());
        }
        // The surface keys under terminal. still work beside them.
        CHECK(parseSettings("terminal.text = #FF0000\nterminal.mode = line")
                  .text(Surface::Terminal).rgb() == 0xFF0000);
    }

    GROUP("settings:run");
    {
        Settings rd = parseSettings("");
        CHECK(!rd.runInNewTerminal());
        CHECK(rd.runTemplate("py") == "{python} {file}");
        CHECK(rd.runTemplate("PY") == "{python} {file}");
        CHECK(rd.runTemplate("pyw") == "{python} {file}");
        CHECK(rd.runTemplate("bash") == "bash {file}");
        CHECK(rd.runTemplate("go") == "go run {file}");
        CHECK(rd.runTemplate("txt").empty());
        CHECK(rd.runTemplate("c").empty());
        CHECK(rd.runTemplate("").empty());
        CHECK(Settings::runLanguage("py") == "python");
        CHECK(Settings::runLanguage("lua") == "lua");
        std::vector<SettingsError> re;
        Settings rs = parseSettings(
            "run.terminal = new\n"                              // 1
            "run.python = uv run {file}  # with uv\n"           // 2 whole line
            "run.lua = lua5.4 {file}\n"                         // 3 a new language
            "run.go = off\n"                                    // 4
            "run.py = {python} -X dev {file}\n"                 // 5 extension wins
            "run.terminal = sometimes\n"                        // 6 bad
            "run.Bad Key = x\n"                                 // 7 bad key
            "run.sh =\n",                                       // 8 no value
            &re);
        CHECK(rs.runInNewTerminal());
        CHECK(rs.runTemplate("py") == "{python} -X dev {file}");
        CHECK(rs.runTemplate("pyw") == "uv run {file}");
        CHECK(rs.runTemplate("lua") == "lua5.4 {file}");
        CHECK(rs.runTemplate("go").empty());
        CHECK(rs.runTemplate("sh") == "bash {file}");
        CHECK(re.size() == 3);
        if (re.size() == 3) {
            CHECK(re[0].line == 6 && re[1].line == 7 && re[2].line == 8);
        }
        CHECK(parseSettings("run.terminal = Current\n").runInNewTerminal() == false);
        // A " #" inside quotes is the command's own, not a comment.
        std::vector<SettingsError> qe;
        Settings rq = parseSettings(
            "run.py = python3 \"{file}\" --tag \"a #1\"  # mine\n"
            "run.sh = echo 'b #2' {file}\n"
            "run.rb = ruby {file} \\# not a comment\n"
            "run.go = go run {file} \"# open\n"
            "lsp.cpp = \"/opt/x #y/clangd\" # mine\n"
            "format.go = gofmt -r 'a #b -> c' # mine\n"
            "run.js = node {file}#frag # mine\n",
            &qe);
        CHECK(qe.empty());
        CHECK(rq.runTemplate("py") == "python3 \"{file}\" --tag \"a #1\"");
        CHECK(rq.runTemplate("sh") == "echo 'b #2' {file}");
        CHECK(rq.runTemplate("rb") == "ruby {file} \\# not a comment");
        CHECK(rq.runTemplate("go") == "go run {file} \"# open");
        CHECK(rq.lspCommand("cpp") == "\"/opt/x #y/clangd\"");
        CHECK(rq.formatCommand("go") == "gofmt -r 'a #b -> c'");
        CHECK(rq.runTemplate("js") == "node {file}#frag");
    }

    GROUP("runfile:quote");
    CHECK(RunFile::shellQuote("/Users/me/proj/main.py") == "/Users/me/proj/main.py");
    CHECK(RunFile::shellQuote("a b.py") == "'a b.py'");
    CHECK(RunFile::shellQuote("it's.py") == "'it'\\''s.py'");
    CHECK(RunFile::shellQuote("$HOME") == "'$HOME'");
    CHECK(RunFile::shellQuote("x;rm -rf ~") == "'x;rm -rf ~'");
    CHECK(RunFile::shellQuote("~/x") == "'~/x'");
    CHECK(RunFile::shellQuote("") == "''");
    CHECK(RunFile::shellQuote("caf\xC3\xA9.py") == "caf\xC3\xA9.py");

    GROUP("runfile:expand");
    {
        std::map<std::string, std::string> v = {
            {"python", "/p/.venv/bin/python"}, {"file", "/p/my app/main.py"},
            {"dir", "/p/my app"}, {"root", "/p"}, {"name", "main.py"}};
        CHECK(RunFile::expand("{python} {file}", v) ==
              "/p/.venv/bin/python '/p/my app/main.py'");
        CHECK(RunFile::expand("cd {dir} && {python} {name}", v) ==
              "cd '/p/my app' && /p/.venv/bin/python main.py");
        CHECK(RunFile::expand("echo {a,b} {unknown} {file", v) == "echo {a,b} {unknown} {file");
        CHECK(RunFile::expand("", v).empty());
        // A placeholder the template already quotes is escaped for those
        // quotes instead of getting quotes of its own.
        CHECK(RunFile::expand("python3 \"{file}\"", v) == "python3 \"/p/my app/main.py\"");
        CHECK(RunFile::expand("python3 '{file}'", v) == "python3 '/p/my app/main.py'");
        CHECK(RunFile::expand("cd \"{dir}\" && python3 {name}", v) ==
              "cd \"/p/my app\" && python3 main.py");
        // After a closed quote the next placeholder is a word of its own again.
        CHECK(RunFile::expand("echo \"x\" {file} 'y' {file}", v) ==
              "echo \"x\" '/p/my app/main.py' 'y' '/p/my app/main.py'");
        // An escaped quote opens nothing; a quote inside the other kind is plain.
        CHECK(RunFile::expand("echo \\\" {file}", v) == "echo \\\" '/p/my app/main.py'");
        CHECK(RunFile::expand("echo \"it's\" {file}", v) ==
              "echo \"it's\" '/p/my app/main.py'");
        CHECK(RunFile::expand("echo \"a \\\" b\" {file}", v) ==
              "echo \"a \\\" b\" '/p/my app/main.py'");
        std::map<std::string, std::string> odd = {{"file", "/p/a\"b$c`d\\e!f'g.py"}};
        CHECK(RunFile::expand("run \"{file}\"", odd) ==
              "run \"/p/a\\\"b\\$c\\`d\\\\e\"'!'\"f'g.py\"");
        CHECK(RunFile::expand("run '{file}'", odd) ==
              "run '/p/a\"b$c`d\\e!f'\\''g.py'");
        CHECK(RunFile::expand("run {file}", odd) ==
              "run '/p/a\"b$c`d\\e!f'\\''g.py'");
        // An unclosed quote in the template still escapes for it.
        CHECK(RunFile::expand("echo \"{name}", odd).empty() == false);
        CHECK(RunFile::quoteInside("a'b", '\'') == "a'\\''b");
        CHECK(RunFile::quoteInside("$x", '"') == "\\$x");
    }

    GROUP("runfile:venv");
    {
        namespace fs = std::filesystem;
        fs::path base = fs::temp_directory_path() /
            ("minicode-venv-test-" + std::to_string(::getpid()));
        fs::remove_all(base);
        auto python = [&](const fs::path &venv) {
            fs::create_directories(venv / "bin");
            std::ofstream(venv / "bin" / "python") << "#!/bin/sh\n";
            fs::permissions(venv / "bin" / "python", fs::perms::owner_all);
        };
        fs::create_directories(base / "proj" / "pkg" / "sub");
        fs::create_directories(base / "other");
        std::string root = (base / "proj").string();
        std::string deep = (base / "proj" / "pkg" / "sub" / "x.py").string();
        CHECK(RunFile::findVirtualenvPython(deep, root).empty());
        python(base / ".venv");   // above the root: never looked at
        CHECK(RunFile::findVirtualenvPython(deep, root).empty());
        python(base / "proj" / "env");
        CHECK(RunFile::findVirtualenvPython(deep, root) == root + "/env/bin/python");
        CHECK(RunFile::findVirtualenvPython(deep, root + "/") == root + "/env/bin/python");
        python(base / "proj" / "pkg" / "venv");
        CHECK(RunFile::findVirtualenvPython(deep, root) == root + "/pkg/venv/bin/python");
        python(base / "proj" / "pkg" / ".venv");   // .venv before venv
        CHECK(RunFile::findVirtualenvPython(deep, root) == root + "/pkg/.venv/bin/python");
        // Not executable: not a python.
        fs::create_directories(base / "proj" / "pkg" / "sub" / "venv" / "bin");
        std::ofstream(base / "proj" / "pkg" / "sub" / "venv" / "bin" / "python") << "x";
        fs::permissions(base / "proj" / "pkg" / "sub" / "venv" / "bin" / "python",
                        fs::perms::owner_read | fs::perms::owner_write);
        CHECK(RunFile::findVirtualenvPython(deep, root) == root + "/pkg/.venv/bin/python");
        // A file outside the root looks only in its own folder.
        std::string outside = (base / "other" / "y.py").string();
        CHECK(RunFile::findVirtualenvPython(outside, root).empty());
        python(base / "other" / "venv");
        CHECK(RunFile::findVirtualenvPython(outside, root) ==
              (base / "other" / "venv" / "bin" / "python").string());
        // No root at all: the file's folder only.
        CHECK(RunFile::findVirtualenvPython(deep, "").empty());
        fs::remove_all(base);
    }

    GROUP("runfile:repl");
    {
        using V = std::vector<std::string>;
        bool extra = true;
        CHECK(RunFile::pythonReplLines("x = 1", false, &extra) == V{"x = 1"} && !extra);
        // A block picked out of a function loses its common indentation.
        CHECK(RunFile::pythonReplLines("    a = 1\n    b = 2\n", false, &extra) ==
              (V{"a = 1", "b = 2"}));
        // Blank lines would end the block at the prompt: dropped, unless
        // the REPL takes bracketed paste; the indented last line needs one
        // more Return.
        CHECK(RunFile::pythonReplLines("def f():\n    a = 1\n\n    return a\n\n", false, &extra) ==
              (V{"def f():", "    a = 1", "    return a"}));
        CHECK(extra);
        CHECK(RunFile::pythonReplLines("def f():\n    a = 1\n\n    return a\n", true, &extra) ==
              (V{"def f():", "    a = 1", "", "    return a"}));
        CHECK(RunFile::pythonReplLines("\n\n", false, &extra).empty() && !extra);
        CHECK(RunFile::pythonReplLines("a\r\nb", false, nullptr) == (V{"a", "b"}));
    }

    GROUP("runfile:activation");
    {
        namespace fs = std::filesystem;
        fs::path base = fs::temp_directory_path() /
            ("minicode-activate-test-" + std::to_string(::getpid()));
        fs::remove_all(base);
        fs::create_directories(base / "proj" / ".venv" / "bin");
        std::ofstream(base / "proj" / ".venv" / "bin" / "python") << "";
        RunFile::Activation a;
        const std::string py = (base / "proj" / ".venv" / "bin" / "python").string();
        CHECK(!RunFile::activationFor(py, &a));   // no pyvenv.cfg: not a venv
        std::ofstream(base / "proj" / ".venv" / "pyvenv.cfg") << "home = /usr/bin\n";
        CHECK(RunFile::activationFor(py, &a) && a.prefix == (base / "proj" / ".venv").string() &&
              a.prompt == ".venv");
        // uv names it after the project; a name with shell or prompt
        // syntax in it is cut to plain characters.
        std::ofstream(base / "proj" / ".venv" / "pyvenv.cfg")
            << "home = /usr/bin\nprompt = my-proj\n";
        CHECK(RunFile::activationFor(py, &a) && a.prompt == "my-proj");
        std::ofstream(base / "proj" / ".venv" / "pyvenv.cfg") << "prompt = '$(rm -rf ~)%n`x`'\n";
        CHECK(RunFile::activationFor(py, &a) && a.prompt == "rm -rf nx");
        CHECK(!RunFile::activationFor("/usr/bin/python3", &a));
        CHECK(!RunFile::activationFor("python3", &a));
        fs::remove_all(base);
    }

    GROUP("settings:lsp");
    std::vector<SettingsError> e5;
    Settings ls = parseSettings(
        "lsp.enabled = false\n"                             // 1
        "lsp.python = pylsp -v --log-file /tmp/x  # log\n"  // 2 words + comment
        "lsp.cpp = \"/opt/my tools/clangd\" --background-index\n"   // 3
        "lsp.go = off\n"                                    // 4
        "lsp.cobol = cobol-ls\n"                            // 5 unknown
        "lsp.rust =\n"                                      // 6 no value
        "lsp.enabled = sometimes\n",                        // 7 not a bool
        &e5);
    CHECK(!ls.lspEnabled());
    CHECK(ls.lspCommand("python") == "pylsp -v --log-file /tmp/x");
    CHECK(ls.lspCommand("cpp") == "\"/opt/my tools/clangd\" --background-index");
    CHECK(ls.lspOff("go") && !ls.lspOff("python") && !ls.lspOff("rust"));
    CHECK(ls.lspCommand("rust").empty());
    CHECK(e5.size() == 3);
    CHECK(e5.size() == 3 && e5[0].line == 5 && e5[1].line == 6 && e5[2].line == 7);
    CHECK(!e5.empty() && e5[0].message.find("lsp.cobol") != std::string::npos);
    CHECK(parseSettings("").lspEnabled());

    GROUP("settings:format");
    std::vector<SettingsError> e8;
    Settings fm = parseSettings(
        "format.python = yapf --style=google  # yapf instead\n"        // 1
        "format.cpp = \"/opt/my tools/clang-format\" --style=LLVM\n"   // 2
        "format.go = off\n"                                           // 3
        "format.cobol = cobfmt\n"                                     // 4 unknown
        "format.rust =\n"                                             // 5 no value
        "format.on-save = true\n"                                     // 6
        "format.on-save = maybe\n",                                   // 7 not a bool
        &e8);
    CHECK(fm.formatCommand("python") == "yapf --style=google");
    CHECK(fm.formatCommand("cpp") == "\"/opt/my tools/clang-format\" --style=LLVM");
    CHECK(fm.formatCommand("go") == "off");
    CHECK(fm.formatCommand("rust").empty() && fm.formatCommand("shell").empty());
    CHECK(fm.formatOnSave());
    CHECK(e8.size() == 3 && e8[0].line == 4 && e8[1].line == 5 && e8[2].line == 7);
    CHECK(!e8.empty() && e8[0].message.find("format.cobol") != std::string::npos);

    GROUP("settings:python");
    std::vector<SettingsError> py7;
    CHECK(parseSettings("").pythonInterpreter().empty());
    CHECK(parseSettings("python.interpreter = /my envs/x/bin/python  # work\n", &py7)
              .pythonInterpreter() == "/my envs/x/bin/python");
    CHECK(parseSettings("Python.Interpreter = ~/venv/bin/python3\n", &py7)
              .pythonInterpreter() == "~/venv/bin/python3");
    CHECK(parseSettings("python.interpreter = /a\npython.interpreter = auto\n", &py7)
              .pythonInterpreter().empty());   // the last one wins
    CHECK(parseSettings("python.interpreter =\n", &py7).pythonInterpreter().empty());
    CHECK(py7.empty());
    std::vector<SettingsError> py8;
    parseSettings("python.version = 3\n", &py8);
    CHECK(py8.size() == 1);

    GROUP("settings:web images");
    std::vector<SettingsError> e6;
    CHECK(parseSettings("").webImages());
    CHECK(!parseSettings("markdown.web-images = false\n").webImages());
    CHECK(!parseSettings("Markdown.Web-Images = off\n").webImages());
    Settings wi = parseSettings("markdown.web-images = maybe\n", &e6);
    CHECK(wi.webImages() && e6.size() == 1 && e6[0].line == 1);

    GROUP("settings:keep-sorted");
    std::vector<SettingsError> eks;
    CHECK(!parseSettings("").keepSortedOnSave());
    CHECK(!parseSettings("").organizeImportsOnSave());
    {
        std::vector<SettingsError> eg;
        Settings g = parseSettings("git.autofetch = true\ngit.autofetch-period = 600\n"
                                   "git.autofetch-period = 60\ngit.autofetch = sometimes\n", &eg);
        CHECK(g.gitAutoFetch() && g.gitAutoFetchPeriod() == 600);
        CHECK(eg.size() == 2 && eg[0].line == 3 && eg[1].line == 4);
        CHECK(eg.size() == 2 && eg[0].message.find("from 180 to 86400") != std::string::npos);
    }
    CHECK(parseSettings("editor.organize-imports-on-save = true\n").organizeImportsOnSave());
    {
        std::vector<SettingsError> eo;
        Settings o = parseSettings("editor.organize-imports-on-save = explicit\n", &eo);
        CHECK(!o.organizeImportsOnSave() && eo.size() == 1 && eo[0].line == 1);
    }
    CHECK(parseSettings("editor.keep-sorted-on-save = true\n").keepSortedOnSave());
    CHECK(parseSettings("Editor.Keep-Sorted-On-Save = yes  # sort\n").keepSortedOnSave());
    CHECK(!parseSettings("editor.keep-sorted-on-save = off\n").keepSortedOnSave());
    Settings ks = parseSettings("editor.keep-sorted-on-save = always\n", &eks);
    CHECK(!ks.keepSortedOnSave() && eks.size() == 1 && eks[0].line == 1);
    // The editor's colors still parse beside it.
    CHECK(parseSettings("editor.keep-sorted-on-save = true\neditor.text = #FF0000\n")
              .text(Surface::Editor) == Rgba::hex(0xFF0000));

    GROUP("settings:auto-save");
    {
        using AS = Settings::AutoSave;
        CHECK(parseSettings("").autoSave() == AS::Off);
        CHECK(parseSettings("editor.auto-save = after-delay\n").autoSave() == AS::AfterDelay);
        CHECK(parseSettings("Editor.Auto-Save = Focus-Change  # on leaving\n").autoSave() ==
              AS::FocusChange);
        CHECK(parseSettings("editor.auto-save = after-delay\neditor.auto-save = off\n")
                  .autoSave() == AS::Off);
        CHECK(parseSettings("editor.auto-save-delay = 250\n").autoSaveDelay() == 250);
        CHECK(parseSettings("editor.auto-save-delay = 600000\n").autoSaveDelay() == 600000);
        for (const char *bad : {"editor.auto-save = afterDelay\n", "editor.auto-save = on\n",
                                "editor.auto-save-delay = 99\n",
                                "editor.auto-save-delay = 600001\n",
                                "editor.auto-save-delay = 1s\n",
                                "editor.auto-save-delay = -5\n"}) {
            std::vector<SettingsError> ae;
            Settings a = parseSettings(bad, &ae);
            CHECK(ae.size() == 1 && ae[0].line == 1);
            CHECK(a.autoSave() == AS::Off && a.autoSaveDelay() == 1000);
        }
        std::vector<SettingsError> ae;
        parseSettings("editor.auto-save = sometimes\n", &ae);
        CHECK(ae.size() == 1 && ae[0].message.find("after-delay") != std::string::npos);
    }

    GROUP("settings:math");
    std::vector<SettingsError> e7;
    CHECK(parseSettings("").math());
    CHECK(!parseSettings("markdown.math = false\n").math());
    CHECK(!parseSettings("markdown.math = off\n").math());
    CHECK(parseSettings("markdown.math = on\n").math());
    Settings mt = parseSettings("\nmarkdown.math = sometimes\n", &e7);
    CHECK(mt.math() && e7.size() == 1 && e7[0].line == 2);
}

}  // namespace

// ------------------------------------------------------------ line comments
namespace {

std::string narrow(const std::u16string &s) {
    std::string out;
    for (char16_t c : s) out += c < 0x80 ? (char)c : '?';
    return out;
}

// Toggle with the selection written into the text as [ and ] (or a single |
// for a caret), and return the result written the same way.
std::string toggled(const std::string &marked, const std::string &marker) {
    std::u16string text;
    size_t a = std::string::npos, b = std::string::npos;
    for (char c : marked) {
        if (c == '|') { a = b = text.size(); continue; }
        if (c == '[') { a = text.size(); continue; }
        if (c == ']') { b = text.size(); continue; }
        text += (char16_t)(unsigned char)c;
    }
    LineComments::Result r = LineComments::toggle(text, a, b, marker);
    // The single-replacement form must describe the same edit.
    std::u16string viaReplace = text;
    if (r.changed)
        viaReplace.replace(r.replaceStart, r.replaceLength, r.replacement);
    if (viaReplace != r.text) return "replacement mismatch";
    std::string out = narrow(r.text);
    if (r.selStart == r.selEnd) return out.insert(r.selStart, "|");
    out.insert(r.selEnd, "]");
    return out.insert(r.selStart, "[");
}

void testLineComments() {
    using LineComments::markerFor;
    GROUP("comments:markers");
    CHECK(markerFor("a.py") == "#");
    CHECK(markerFor("settings.conf") == "#");
    CHECK(markerFor("/Users/x/.config/minicode/settings.conf") == "#");
    CHECK(markerFor("main.CPP") == "//");
    CHECK(markerFor("App.mm") == "//");
    CHECK(markerFor("index.tsx") == "//");
    CHECK(markerFor("query.sql") == "--");
    CHECK(markerFor("init.lua") == "--");
    CHECK(markerFor("setup.ini") == ";");
    CHECK(markerFor("Makefile") == "#");
    CHECK(markerFor("src/Dockerfile") == "#");
    CHECK(markerFor(".zshrc") == "#");
    CHECK(markerFor("paper.tex") == "%");
    CHECK(markerFor("data.json") == "");      // JSON has no comments
    CHECK(markerFor("README.md") == "");
    CHECK(markerFor("LICENSE") == "");
    CHECK(markerFor("") == "");

    GROUP("comments:single-line");
    CHECK(toggled("x = 1|", "#") == "# x = 1|");
    CHECK(toggled("x| = 1", "#") == "# x| = 1");
    CHECK(toggled("|x = 1", "#") == "# |x = 1");       // caret follows the text
    CHECK(toggled("# x = 1|", "#") == "x = 1|");
    CHECK(toggled("#x = 1|", "#") == "x = 1|");        // no space after marker
    CHECK(toggled("#  x|", "#") == " x|");             // only one space removed
    CHECK(toggled("    return 0;|", "//") == "    // return 0;|");
    CHECK(toggled("    // return 0;|", "//") == "    return 0;|");
    CHECK(toggled("  // |a", "//") == "  |a");         // caret inside removed marker
    CHECK(toggled("\tint a;|", "//") == "\t// int a;|");
    CHECK(toggled("a\nb|\nc", "#") == "a\n# b|\nc");   // only the caret's line
    CHECK(toggled("|", "#") == "# |");                 // empty line
    CHECK(toggled("a\n|", "#") == "a\n# |");
    CHECK(toggled("   |", "#") == "   # |");
    CHECK(toggled("SELECT 1|", "--") == "-- SELECT 1|");

    GROUP("comments:multi-line");
    CHECK(toggled("[a\nb\nc]", "#") == "[# a\n# b\n# c]");
    CHECK(toggled("[# a\n# b\n# c]", "#") == "[a\nb\nc]");
    // Mixed: anything uncommented means comment them all.
    CHECK(toggled("[# a\nb]", "#") == "[# # a\n# b]");
    // Inserted at the smallest indentation, keeping the block aligned.
    CHECK(toggled("[  if x:\n    y\n  z]", "#") == "[  # if x:\n  #   y\n  # z]");
    CHECK(toggled("[  # if x:\n  #   y\n  # z]", "#") == "[  if x:\n    y\n  z]");
    // Blank lines are skipped, in both directions.
    CHECK(toggled("[a\n\nb]", "//") == "[// a\n\n// b]");
    CHECK(toggled("[// a\n   \n// b]", "//") == "[a\n   \nb]");
    // A selection that ends at the start of a line leaves that line alone.
    CHECK(toggled("[a\nb\n]c", "#") == "[# a\n# b\n]c");
    // Selection starting mid-line keeps its place in the text.
    CHECK(toggled("ab[c\nd]e", "#") == "# ab[c\n# d]e");
    CHECK(toggled("# ab[c\n# d]e", "#") == "ab[c\nd]e");
    // Surrounding lines are untouched.
    CHECK(toggled("keep\n[a\nb]\nkeep", "//") == "keep\n[// a\n// b]\nkeep");
    // Backwards selection is fine.
    std::u16string t = u"a\nb";
    LineComments::Result r = LineComments::toggle(t, 3, 0, "#");
    CHECK(narrow(r.text) == "# a\n# b");

    GROUP("comments:non-ascii");
    // UTF-16 offsets: an emoji is two units, and the caret stays after it.
    std::u16string emoji = u"x = \U0001F600";
    LineComments::Result e = LineComments::toggle(emoji, emoji.size(), emoji.size(), "#");
    CHECK(e.text == u"# x = \U0001F600");
    CHECK(e.selStart == e.text.size());

    GROUP("comments:no-marker");
    LineComments::Result n = LineComments::toggle(u"abc", 1, 1, "");
    CHECK(!n.changed && n.text == u"abc" && n.selStart == 1);
    // Out-of-range selections are clamped rather than crashing.
    LineComments::Result o = LineComments::toggle(u"abc", 99, 99, "#");
    CHECK(o.changed && o.text == u"# abc");
}

void testSettingsColorEditing() {
    GROUP("settings:find-color");
    ColorSpan span;
    CHECK(Settings::findColor(u"editor.text = #D4D4D4", span) &&
          span.start == 14 && span.length == 7 && colorIs(span.color, 0xD4D4D4));
    CHECK(Settings::findColor(u"# editor.text = #D4D4D4", span) && span.start == 16);
    CHECK(Settings::findColor(u"sidebar.background=#12345680  # note", span) &&
          span.start == 19 && span.length == 9 && near(span.color.a, 128 / 255.0));
    CHECK(Settings::findColor(u"x = #abc\r", span) && span.length == 4);
    CHECK(!Settings::findColor(u"editor.opacity = 0.5", span));
    CHECK(!Settings::findColor(u"# Colors are #RRGGBB, or #RRGGBBAA", span));
    CHECK(!Settings::findColor(u"# Each line is key = value, and", span));
    CHECK(!Settings::findColor(u"x = #GGG", span));
    CHECK(!Settings::findColor(u"", span));

    GROUP("settings:format-color");
    CHECK(Settings::formatColor(Rgba::hex(0x0a0b0c)) == "#0A0B0C");
    CHECK(Settings::formatColor(Rgba::hex(0xFFFFFF, 0.5)) == "#FFFFFF80");
    CHECK(Settings::formatColor(Rgba::hex(0x000000, 0)) == "#00000000");
    CHECK(Settings::formatColor(Rgba::hex(0x123456, 0.999)) == "#123456");
    Rgba back;
    CHECK(Settings::parseColor(Settings::formatColor(Rgba::hex(0x336699, 0.25)), back) &&
          back.rgb() == 0x336699 && back.a > 0.24 && back.a < 0.26);

    GROUP("settings:contrast");
    Settings dark = Settings::parse("");
    CHECK(dark.contrastText(Rgba::hex(0xFFFFFF)).rgb() == 0x000000);
    CHECK(dark.contrastText(Rgba::hex(0xD4D4D4)).rgb() == 0x000000);
    CHECK(dark.contrastText(Rgba::hex(0x1E1E1E)).rgb() == 0xFFFFFF);
    CHECK(dark.contrastText(Rgba::hex(0x007ACC)).rgb() == 0xFFFFFF);
    // A see-through white swatch over the dark editor mostly shows the editor.
    CHECK(dark.contrastText(Rgba::hex(0xFFFFFF, 0.1)).rgb() == 0xFFFFFF);
    Settings light = Settings::parse("editor.background = #FFFFFF\n");
    CHECK(light.contrastText(Rgba::hex(0x000000, 0.1)).rgb() == 0x000000);

    GROUP("settings:set-color");
    Rgba red = Rgba::hex(0xFF0000);
    CHECK(Settings::setColor(u"editor.text = #D4D4D4", red) == u"editor.text = #FF0000");
    // A commented-out setting is switched on.
    CHECK(Settings::setColor(u"# editor.text = #D4D4D4", red) == u"editor.text = #FF0000");
    CHECK(Settings::setColor(u"#editor.text = #D4D4D4", red) == u"editor.text = #FF0000");
    CHECK(Settings::setColor(u"  # editor.text = #D4D4D4", red) == u"  editor.text = #FF0000");
    // Trailing comments and alpha survive.
    CHECK(Settings::setColor(u"sidebar.text = #FFF  # mine", Rgba::hex(0x00FF00, 0.5)) ==
          u"sidebar.text = #00FF0080  # mine");
    // Lines without a color, or prose, are left alone.
    CHECK(Settings::setColor(u"editor.opacity = 0.5", red) == u"editor.opacity = 0.5");
    CHECK(Settings::setColor(u"# Colors are #RRGGBB", red) == u"# Colors are #RRGGBB");
    CHECK(Settings::setColor(u"# a note, x y = #FFFFFF", red) == u"# a note, x y = #FF0000");
    // The result parses as the new setting.
    std::u16string line = Settings::setColor(u"# terminal.text = #D4D4D4", red);
    CHECK(colorIs(Settings::parse(narrow(line)).text(Surface::Terminal), 0xFF0000));

    GROUP("settings:find-value");
    ValueSpan vs;
    CHECK(Settings::findValue(u"editor.opacity = 0.5", vs) && vs.start == 17 && vs.length == 3);
    CHECK(Settings::findValue(u"# window.blur = false  # note", vs) &&
          vs.start == 16 && vs.length == 5);
    CHECK(Settings::findValue(u"lsp.cpp=clangd", vs) && vs.start == 8 && vs.length == 6);
    CHECK(!Settings::findValue(u"# Each line is key = value, and", vs));
    CHECK(!Settings::findValue(u"editor.opacity =", vs));
    CHECK(!Settings::findValue(u"", vs));

    GROUP("settings:opacity-editing");
    double op = -1;
    CHECK(Settings::findOpacity(u"# sidebar.opacity = 1", vs, op) && vs.start == 20 &&
          vs.length == 1 && near(op, 1));
    CHECK(Settings::findOpacity(u"window.opacity = 80%", vs, op) && near(op, 0.8));
    CHECK(!Settings::findOpacity(u"editor.text = #D4D4D4", vs, op));
    CHECK(!Settings::findOpacity(u"window.blur = true", vs, op));
    CHECK(!Settings::findOpacity(u"editor.opacity = lots", vs, op));
    CHECK(Settings::setOpacity(u"# sidebar.opacity = 1", 0.85) == u"sidebar.opacity = 0.85");
    CHECK(Settings::setOpacity(u"sidebar.opacity = 1  # mine", 0.5) ==
          u"sidebar.opacity = 0.5  # mine");
    CHECK(Settings::setOpacity(u"editor.opacity = 0.5", 1.0) == u"editor.opacity = 1");
    CHECK(Settings::setOpacity(u"editor.opacity = 0.5", 0.0) == u"editor.opacity = 0");
    CHECK(Settings::setOpacity(u"editor.opacity = 0.5", 0.07) == u"editor.opacity = 0.07");
    CHECK(Settings::setOpacity(u"# window.opacity = 80%", 0.425) == u"window.opacity = 43%");
    CHECK(Settings::setOpacity(u"editor.opacity = 0.5", 3) == u"editor.opacity = 1");
    CHECK(Settings::setOpacity(u"editor.text = #D4D4D4", 0.5) == u"editor.text = #D4D4D4");
    std::u16string opLine = Settings::setOpacity(u"# terminal.opacity = 1", 0.6);
    CHECK(near(Settings::parse(narrow(opLine)).opacity(Surface::Terminal), 0.6));

    GROUP("settings:key-line");
    const std::string conf = "# window.opacity = 1\n"
                             "# sidebar.background = #252526\n"
                             "sidebar.background = #000000\n"
                             "# Each line is key = value\n"
                             "  Sidebar.Opacity = 0.5\n"
                             "sidebar.background = #111111\n";
    CHECK(Settings::findKeyLine(conf, "window.opacity") == 0);        // commented only
    CHECK(Settings::findKeyLine(conf, "sidebar.background") == 5);    // the last one set
    CHECK(Settings::findKeyLine(conf, "sidebar.opacity") == 4);       // any case, indented
    CHECK(Settings::findKeyLine(conf, "editor.text") == -1);
    CHECK(Settings::findKeyLine(conf, "key") == -1);                  // prose is not a key
    CHECK(Settings::findKeyLine("", "window.opacity") == -1);

    GROUP("settings:appearance-surfaces");
    // Every key a surface lists is in the default file and parses there, so
    // the map cannot outlive a renamed key.
    const std::string defaults = Settings::defaultFileText();
    const std::string live = uncommentKeys(defaults);
    std::vector<SettingsError> liveErrors;
    Settings::parse(live, &liveErrors);
    CHECK(liveErrors.empty());
    int rotten = 0, misordered = 0, ownAfterFallback = 0;
    for (const AppearanceSurface &s : Settings::appearanceSurfaces()) {
        CHECK(!s.keys.empty());
        CHECK(Settings::appearanceSurface(s.id) == &s);
        std::string group = std::string(s.keys[0]).substr(0, std::string(s.keys[0]).find('.'));
        int lastOwn = -1, lastFallback = -1;
        bool inFallback = false;
        for (const char *key : s.keys) {
            int at = Settings::findKeyLine(defaults, key);
            if (at < 0 || Settings::findKeyLine(live, key) < 0) {
                rotten++;
                std::printf("    surface %s: %s is not in the default file\n", s.id, key);
            }
            bool fallback = std::string(key).rfind("window.", 0) == 0 &&
                            std::string(s.id) != "window";
            if (fallback) {
                inFallback = true;
                if (at < lastFallback) misordered++;
                lastFallback = at;
            } else {
                if (inFallback) ownAfterFallback++;
                if (std::string(key).rfind(group, 0) == 0 && at < lastOwn) misordered++;
                if (std::string(key).rfind(group, 0) == 0) lastOwn = at;
            }
        }
    }
    CHECK(rotten == 0);
    CHECK(misordered == 0);
    CHECK(ownAfterFallback == 0);
    for (const char *id : {"tree", "editor", "markdown", "syntax", "terminal", "browser",
                           "statusbar", "titlebar", "window", "git", "tabs",
                           "terminal-tabs"})
        CHECK(Settings::appearanceSurface(id) != nullptr);
    CHECK(Settings::appearanceSurface("nothing") == nullptr);
    CHECK(std::string(Settings::appearanceSurface("tree")->keys[0]) == "sidebar.background");
    CHECK(std::string(Settings::appearanceSurface("tree")->keys.back()) == "window.text");

    GROUP("settings:missing-surface-lines");
    {
        // A file with every key has nothing missing, and an unknown surface none.
        for (const AppearanceSurface &s : Settings::appearanceSurfaces())
            CHECK(Settings::missingSurfaceLines(defaults, s.id).empty());
        CHECK(Settings::missingSurfaceLines("", "nothing").empty());
        // An empty file is missing all of a surface's keys, each with its
        // section's heading.
        std::string t = Settings::missingSurfaceLines("", "terminal");
        CHECK(t == "# The whole window. window.opacity is the default for every panel below that\n"
                   "# doesn't set its own, and window.text is the default text color.\n"
                   "# window.opacity = 1\n"
                   "# window.text = #D4D4D4\n"
                   "\n"
                   "# The terminal panel.\n"
                   "# terminal.background = #181818\n"
                   "# terminal.opacity = 1\n"
                   "# terminal.text = #D4D4D4\n");
        // An old file with the window and editor keys, set or commented,
        // lacks only the syntax and Markdown ones; a key's own note comes
        // with it.
        const std::string old = "window.opacity = 0.9\n# window.text = #D4D4D4\n"
                                "editor.background = #000000\n# editor.opacity = 1\n"
                                "# editor.text = #D4D4D4\n"
                                "markdown.heading = #FFFFFF\nmarkdown.link = #4EA1F7\n"
                                "# markdown.code = #CE9178\n# markdown.quote = #9CA3AF\n";
        std::string m = Settings::missingSurfaceLines(old, "markdown");
        CHECK(m == "# A checked task's text (\"- [x] ...\"), which is also struck through.\n"
                   "# markdown.done = #858585\n");
        std::string e = Settings::missingSurfaceLines(old, "editor");
        // The gutter's and the bracket match's keys came later than the
        // file, so they are added too, with their notes.
        CHECK(e.rfind("# The gutter behind the numbers", 0) == 0);
        CHECK(e.find("# editor.gutter-background = #1E1E1E\n") != std::string::npos);
        CHECK(e.find("# The box around the bracket beside the caret and its partner.\n"
                     "# editor.bracket-match = #888888\n") != std::string::npos);
        CHECK(e.find("# Syntax highlighting. Plain text uses editor.text.\n"
                     "# syntax.keyword = #569CD6\n") != std::string::npos);
        CHECK(e.find("syntax.function") != std::string::npos);
        CHECK(e.find("# editor.background") == std::string::npos &&
              e.find("# editor.text") == std::string::npos &&
              e.find("# window.") == std::string::npos);
        // Appended, the lines are found, so nothing is missing a second time.
        CHECK(Settings::missingSurfaceLines(old + "\n" + e, "editor").empty());
        CHECK(Settings::findKeyLine(old + "\n" + e, "syntax.keyword") > 0);
        std::vector<SettingsError> pe;
        Settings::parse(old + "\n" + e + m, &pe);
        CHECK(pe.empty());
    }
}

// ------------------------------------------------------------------- latex

// The span covering `text`, or nullptr.
const LatexSpan *spanWithText(const LatexDoc &doc, const std::string &text) {
    for (const LatexSpan &sp : doc.spans)
        if (sp.display == text) return &sp;
    return nullptr;
}

bool hasSpanText(const LatexDoc &doc, const std::string &text) {
    return spanWithText(doc, text) != nullptr;
}

const char *kSampleTex =
    "\\documentclass{article}\n"                       // 1
    "\\usepackage{amsmath}\n"                          // 2
    "\\title{My Paper}\n"                              // 3
    "\\author{A. Writer}\n"                            // 4
    "\\begin{document}\n"                              // 5
    "\\maketitle\n"                                    // 6
    "\\section{Introduction}\\label{sec:intro}\n"       // 7
    "Some prose with \\textbf{bold} inside.\n"          // 8
    "\n"                                               // 9
    "Another paragraph, $x^2 + 1$ included.\n"          // 10
    "\\begin{itemize}\n"                               // 11
    "  \\item First thing\n"                           // 12
    "  \\item Second thing\n"                          // 13
    "\\end{itemize}\n"                                 // 14
    "\\end{document}\n";                               // 15

void testLatexDoc() {
    std::string src = kSampleTex;
    LatexDoc doc = LatexDoc::parse(src);

    GROUP("latex:fields");
    const LatexSpan *title = spanWithText(doc, "My Paper");
    CHECK(title != nullptr);
    CHECK(title && title->kind == LatexSpanKind::Field);
    CHECK(title && title->command == "title");
    CHECK(title && title->line == 3);
    CHECK(title && src.substr(title->start, title->end - title->start) == "My Paper");
    const LatexSpan *sec = spanWithText(doc, "Introduction");
    CHECK(sec && sec->command == "section" && sec->line == 7);
    CHECK(hasSpanText(doc, "A. Writer"));

    GROUP("latex:skips-machinery");
    // Package options, labels and class names are not prose, so are not offered.
    CHECK(!hasSpanText(doc, "amsmath"));
    CHECK(!hasSpanText(doc, "sec:intro"));
    CHECK(!hasSpanText(doc, "article"));

    GROUP("latex:text-runs");
    // A styling command splits the run; its content is editable on its own.
    CHECK(hasSpanText(doc, "Some prose with"));
    CHECK(hasSpanText(doc, "bold"));
    CHECK(hasSpanText(doc, "inside."));
    // A blank line ends a paragraph.
    const LatexSpan *para = spanWithText(doc, "Another paragraph,");
    CHECK(para != nullptr);
    CHECK(para && para->line == 10);
    // Nothing before \begin{document} is treated as body text.
    for (const LatexSpan &sp : doc.spans)
        if (sp.kind == LatexSpanKind::Text) CHECK(sp.line >= 6);

    GROUP("latex:math");
    const LatexSpan *math = nullptr;
    for (const LatexSpan &sp : doc.spans)
        if (sp.kind == LatexSpanKind::Math) math = &sp;
    CHECK(math != nullptr);
    CHECK(math && src.substr(math->start, math->end - math->start) == "x^2 + 1");
    LatexDoc envMath = LatexDoc::parse(
        "\\begin{document}\\begin{align}\na &= b\n\\end{align}\\end{document}");
    CHECK(envMath.spans.size() == 1);
    CHECK(envMath.spans[0].kind == LatexSpanKind::Math);
    CHECK(envMath.spans[0].display.find("a") != std::string::npos);

    GROUP("latex:lists");
    CHECK(doc.lists.size() == 1);
    const LatexList &list = doc.lists[0];
    CHECK(list.environment == "itemize");
    CHECK(list.itemBodyStart.size() == 2);
    CHECK(list.itemBodyEnd.size() == 2);
    CHECK(list.indent == "  ");
    const LatexSpan *first = spanWithText(doc, "First thing");
    CHECK(first != nullptr);
    CHECK(first && first->listIndex == 0 && first->itemIndex == 0);
    const LatexSpan *second = spanWithText(doc, "Second thing");
    CHECK(second && second->itemIndex == 1);

    GROUP("latex:lines");
    CHECK(LatexDoc::lineAt(src, 0) == 1);
    CHECK(doc.spansOnLine(12).size() == 1);
    CHECK(doc.spansOnLine(999).empty());

    GROUP("latex:display-text");
    CHECK(LatexDoc::displayText("a  b\n  c") == "a b c");
    CHECK(LatexDoc::displayText("50\\% off \\& more") == "50% off & more");
    CHECK(LatexDoc::displayText("a~b") == "a b");
    CHECK(LatexDoc::displayText("The \\emph{fun} part") == "The fun part");
    CHECK(LatexDoc::displayText("x % a comment\ny") == "x y");

    GROUP("latex:replace");
    LatexDoc::Edit e = LatexDoc::replaceSpan(src, *title, "A Better Title");
    CHECK(e.source.find("\\title{A Better Title}") != std::string::npos);
    CHECK(e.source.find("My Paper") == std::string::npos);
    CHECK(e.source.substr(e.start, e.end - e.start) == "A Better Title");
    // The rest of the file is untouched, byte for byte.
    CHECK(e.source.substr(0, title->start) == src.substr(0, title->start));
    CHECK(e.source.substr(e.end) == src.substr(title->end));
    // Re-parsing the result finds the new text where the old one was.
    LatexDoc after = LatexDoc::parse(e.source);
    CHECK(hasSpanText(after, "A Better Title"));

    GROUP("latex:add-item");
    LatexDoc::Edit add = LatexDoc::addItem(src, list, 1, "Third thing");
    CHECK(add.source.find("  \\item Second thing\n  \\item Third thing\n"
                          "\\end{itemize}") != std::string::npos);
    CHECK(add.source.substr(add.start, add.end - add.start) == "Third thing");
    LatexDoc addedDoc = LatexDoc::parse(add.source);
    CHECK(addedDoc.lists.size() == 1 && addedDoc.lists[0].itemBodyStart.size() == 3);
    CHECK(hasSpanText(addedDoc, "Third thing"));
    // Inserting in the middle keeps the order.
    LatexDoc::Edit mid = LatexDoc::addItem(src, list, 0, "Middle");
    CHECK(mid.source.find("First thing\n  \\item Middle\n  \\item Second") !=
          std::string::npos);
    // A list written on one line still gets a well-formed new item.
    std::string oneLine =
        "\\begin{document}\\begin{itemize}\\item one\\end{itemize}\\end{document}";
    LatexDoc oneDoc = LatexDoc::parse(oneLine);
    CHECK(oneDoc.lists.size() == 1);
    LatexDoc::Edit oneAdd = LatexDoc::addItem(oneLine, oneDoc.lists[0], 0, "two");
    CHECK(LatexDoc::parse(oneAdd.source).lists[0].itemBodyStart.size() == 2);
    CHECK(hasSpanText(LatexDoc::parse(oneAdd.source), "two"));

    GROUP("latex:click");
    // SyncTeX names the line a paragraph closed on, one past the prose here.
    const LatexSpan *hit = doc.spanForClick({9}, "prose");
    CHECK(hit && hit->display == "Some prose with");
    // The word decides between two items whose lines both look plausible.
    CHECK(doc.spanForClick({13, 14}, "First")->display == "First thing");
    CHECK(doc.spanForClick({13, 14}, "Second")->display == "Second thing");
    // A heading is found on its own line.
    CHECK(doc.spanForClick({7}, "Introduction")->display == "Introduction");
    // Without a usable word, the nearest span still wins.
    CHECK(doc.spanForClick({12}, "")->display == "First thing");
    CHECK(doc.spanForClick({12}, "nowhere") == nullptr);   // refuses to guess
    CHECK(doc.spanForClick({}, "First") == nullptr);
    // The PDF reads math back without its markup, so matching ignores it.
    CHECK(doc.spanForClick({10}, "x2")->kind == LatexSpanKind::Math);
    CHECK(doc.spanForClick({10}, "paragraph,")->display == "Another paragraph,");
    CHECK(doc.spanForClick({900}, "First") == nullptr);

    GROUP("latex:real-documents");
    // Real documents wrap their text in all sorts of commands, including the
    // author's own macros. Text the reader cannot see is text the preview
    // cannot edit, so unknown commands are looked through, not skipped.
    LatexDoc wild = LatexDoc::parse(
        "\\begin{document}\n"
        "\\normalfont{Post-training of Gemini}\n"
        "\\raisebox{0.5em}{\\textit{Senior Engineer} @ Google}\n"
        "\\makebox[8em][l]{\\includegraphics[scale=0.1]{GDM.png}}\n"
        "\\twemoji{globe} \\href{https://echansen.org}{echansen.org}\n"
        "\\todo{Bullet 2 -- scale and adoption}\n"
        "\\vspace*{-\\baselineskip}\n"
        "\\setlength{\\parskip}{1em}\n"
        "\\multicolumn{2}{c}{Total}\n"
        "\\end{document}\n");
    CHECK(hasSpanText(wild, "Post-training of Gemini"));
    CHECK(hasSpanText(wild, "Senior Engineer"));
    CHECK(hasSpanText(wild, "@ Google"));
    CHECK(hasSpanText(wild, "Bullet 2 -- scale and adoption"));   // a user macro
    CHECK(hasSpanText(wild, "Total"));
    // ... while arguments that are machinery stay out of reach.
    CHECK(!hasSpanText(wild, "GDM.png"));
    CHECK(!hasSpanText(wild, "https://echansen.org"));
    CHECK(hasSpanText(wild, "echansen.org"));     // the link text, not its URL
    CHECK(!hasSpanText(wild, "0.5em"));
    CHECK(!hasSpanText(wild, "1em"));
    CHECK(!hasSpanText(wild, "8em"));
    CHECK(!hasSpanText(wild, "l"));
    CHECK(!hasSpanText(wild, "c"));
    CHECK(!hasSpanText(wild, "2"));
    // A date is text even though it has no letters in it.
    LatexDoc dates = LatexDoc::parse(
        "\\begin{document}\\raisebox{0.5em}{\\textit{2014 - 2019}}"
        "\\normalfont{11/2025 - ...}\\end{document}");
    CHECK(hasSpanText(dates, "2014 - 2019"));
    CHECK(hasSpanText(dates, "11/2025 - ..."));
    CHECK(!hasSpanText(dates, "0.5em"));
    LatexDoc refs = LatexDoc::parse(
        "\\begin{document}\\label{sec:intro}\\cite{knuth1984}"
        "\\includegraphics{fig.pdf}\\input{chapter}Real text\\end{document}");
    CHECK(refs.spans.size() == 1 && refs.spans[0].display == "Real text");

    GROUP("latex:click-refuses");
    // A click on something the reader does not know must not open the nearest
    // text instead: editing the wrong line is worse than editing nothing.
    CHECK(doc.spanForClick({12, 13}, "Photograph") == nullptr);
    CHECK(doc.spanForClick({7}, "Introduction") != nullptr);
    // With nothing to match on (a logo, a glyph the PDF cannot name), the
    // nearest span is still offered.
    CHECK(doc.spanForClick({12}, "") != nullptr);
    // A click that reports a whole line finds the span inside it.
    CHECK(doc.spanForClick({12}, "First thing and more")->display == "First thing");

    GROUP("latex:verbatim");
    LatexDoc verb = LatexDoc::parse(
        "\\begin{document}\\begin{verbatim}\n\\section{no}\n"
        "\\end{verbatim}\nafter\\end{document}");
    CHECK(!hasSpanText(verb, "no"));
    CHECK(hasSpanText(verb, "after"));

    GROUP("latex:tabular");
    LatexDoc tab = LatexDoc::parse(
        "\\begin{document}\\begin{tabular}{|l|r|}\nApples & 3 \\\\\n"
        "\\end{tabular}\\end{document}");
    CHECK(!hasSpanText(tab, "|l|r|"));
    CHECK(hasSpanText(tab, "Apples"));   // each cell is editable on its own
    CHECK(hasSpanText(tab, "3"));

    GROUP("latex:description");
    LatexDoc desc = LatexDoc::parse(
        "\\begin{document}\\begin{description}\n\\item[Term] meaning\n"
        "\\end{description}\\end{document}");
    CHECK(hasSpanText(desc, "Term"));
    CHECK(hasSpanText(desc, "meaning"));
    CHECK(desc.lists.size() == 1 && desc.lists[0].itemBodyStart.size() == 1);

    GROUP("latex:robustness");
    // Unterminated input must not hang or read past the end.
    CHECK(LatexDoc::parse("\\section{unclosed").spans.size() == 1);
    CHECK(LatexDoc::parse("\\begin{itemize}\\item x").lists.size() == 1);
    CHECK(LatexDoc::parse("$x").spans.size() <= 1);
    CHECK(LatexDoc::parse("").spans.empty());
    CHECK(LatexDoc::parse("\\").spans.empty());
}

// ------------------------------------------------------------------ synctex

// Two boxes on page 1: source line 7 near the top, line 12 lower down.
// Values are scaled points (1pt = 65536sp); y is the baseline of the box.
const char *kSampleSyncTex =
    "SyncTeX Version:1\n"
    "Input:1:/tmp/doc.tex\n"
    "Input:2:/tmp/other.tex\n"
    "Output:pdf\n"
    "Magnification:1000\n"
    "Unit:1\n"
    "X Offset:0\n"
    "Y Offset:0\n"
    "Content:\n"
    "!753\n"
    "{1\n"
    "[1,7:4718592,6553600:19660800,655360,0\n"   // x=72 y=100 w=300 h=10
    "h1,7:4718592,6553600:19660800,655360,0\n"
    "[1,12:4718592,19660800:13107200,655360,0\n" // x=72 y=300 w=200 h=10
    "]\n"
    "]\n"
    "}1\n"
    "Postamble:\n";

void testSyncTex() {
    SyncTexIndex idx = SyncTexIndex::parse(kSampleSyncTex);

    GROUP("synctex:parse");
    CHECK(idx.valid());
    CHECK(idx.recordCount() == 3);
    CHECK(idx.pathForTag(1) == "/tmp/doc.tex");
    CHECK(idx.tagForPath("/tmp/doc.tex") == 1);
    CHECK(idx.tagForPath("doc.tex") == 1);          // matched by file name
    CHECK(idx.tagForPath("/elsewhere/other.tex") == 2);
    CHECK(idx.tagForPath("missing.tex") == 0);

    GROUP("synctex:hit");
    // A point inside the first box resolves to its line.
    std::vector<SyncTexHit> hits = idx.hitsAtPoint(1, 100, 95);
    CHECK(!hits.empty());
    CHECK(!hits.empty() && hits[0].line == 7);
    CHECK(!hits.empty() && hits[0].distance == 0.0);
    CHECK(!hits.empty() && hits[0].tag == 1);
    // The box is reported in points, measured from the top-left of the page.
    CHECK(!hits.empty() && std::abs(hits[0].x - 72.0) < 0.01);
    CHECK(!hits.empty() && std::abs(hits[0].width - 300.0) < 0.01);
    CHECK(!hits.empty() && std::abs(hits[0].y - 90.0) < 0.01);

    // Inside the second box.
    hits = idx.hitsAtPoint(1, 100, 295);
    CHECK(!hits.empty() && hits[0].line == 12);

    // Between them: nothing contains the point, so the nearest box wins, and
    // the other line is still offered as a candidate.
    hits = idx.hitsAtPoint(1, 100, 260);
    CHECK(hits.size() == 2);
    CHECK(!hits.empty() && hits[0].line == 12);
    CHECK(hits.size() > 1 && hits[1].line == 7);

    // One hit per source line, and never from another page.
    CHECK(idx.hitsAtPoint(1, 100, 95).size() == 2);
    CHECK(idx.hitsAtPoint(2, 100, 95).empty());

    GROUP("synctex:robustness");
    CHECK(!SyncTexIndex::parse("").valid());
    CHECK(!SyncTexIndex::parse("SyncTeX Version:1\nContent:\n{1\nnonsense\n").valid());
    CHECK(SyncTexIndex::parse(kSampleSyncTex).hitsAtPoint(1, 0, 0, 1).size() == 1);
}

// ------------------------------------------------------ latex click-to-source
// Each group is a failure the click sweep (tests/latex/sweep.sh) found in a
// real or torture document: a double-click that refused, or opened the wrong
// text.

// The span holding exactly `text` as its source bytes, or null.
const LatexSpan *spanWithSource(const LatexDoc &doc, const std::string &src,
                                const std::string &text) {
    for (const LatexSpan &sp : doc.spans)
        if (src.substr(sp.start, sp.end - sp.start) == text) return &sp;
    return nullptr;
}

std::string sourceOf(const std::string &src, const LatexSpan *sp) {
    return sp ? src.substr(sp->start, sp->end - sp->start) : std::string("(none)");
}

void testLatexClicks() {
    GROUP("latex:link-inside-wrapper");
    // A wrapper whose text holds a link used to be taken for a URL argument
    // and skipped whole, so nothing in an italic note or a bullet that cites a
    // paper could be clicked.
    std::string wrapped =
        "\\begin{document}\n"
        "\\it{Note: my notes live at \\href{https://notes.example}{notes.example}}.\n"
        "\\normalfont{Built a birdhouse (\\href{https://x.org/paper}{paper}) "
        "painted it blue.}\n"
        "\\normalfont{\\href{https://scholar.example/u}{Google Scholar}}\n"
        "\\end{document}\n";
    LatexDoc w = LatexDoc::parse(wrapped);
    CHECK(hasSpanText(w, "Note: my notes live at"));
    CHECK(hasSpanText(w, "notes.example"));
    CHECK(hasSpanText(w, "Built a birdhouse ("));
    CHECK(hasSpanText(w, "paper"));
    CHECK(hasSpanText(w, "Google Scholar"));
    CHECK(!hasSpanText(w, "https://notes.example"));   // the URLs stay machinery
    CHECK(!hasSpanText(w, "https://x.org/paper"));
    CHECK(w.spanForClick({2}, "Note") != nullptr);
    CHECK(sourceOf(wrapped, w.spanForClick({3}, "painted")) == ") painted it blue.");

    GROUP("latex:match-key");
    // The PDF gives ligatures as one character and accents as letters; the
    // source spells them in ASCII and TeX escapes. Both meet in the key.
    CHECK(LatexDoc::matchKey("e\xEF\xAC\x83" "cient") == "efficient");   // U+FB03
    CHECK(LatexDoc::matchKey("\xEF\xAC\x81nd") == "find");                // U+FB01
    CHECK(LatexDoc::matchKey("R\xC3\xA9sum\xC3\xA9") == "resume");
    CHECK(LatexDoc::matchKey("Stra\xC3\x9F" "e") == "strasse");
    CHECK(LatexDoc::matchKey("se\xC3\xB1or, co\xC3\xB6perate") == "senorcooperate");
    CHECK(LatexDoc::matchKey("\xE2\x80\x9Cquoted\xE2\x80\x9D \xE2\x80\x94 x") == "quotedx");
    CHECK(LatexDoc::matchKey("\xCE\xB1\xCE\xB2") == "\xCE\xB1\xCE\xB2");  // Greek kept
    CHECK(LatexDoc::matchKey(LatexDoc::displayText("r\\'esum\\'e")) == "resume");
    CHECK(LatexDoc::matchKey(LatexDoc::displayText("r\\'{e}sum\\'{e}")) == "resume");
    CHECK(LatexDoc::matchKey(LatexDoc::displayText("na\\\"ive")) == "naive");
    CHECK(LatexDoc::displayText("Stra\\ss e") == "Strasse");
    CHECK(LatexDoc::displayText("Fran\\c{c}ais") == "Francais");
    CHECK(LatexDoc::displayText("hy\\-phen") == "hyphen");
    CHECK(LatexDoc::displayText("the \\LaTeX{} way") == "the LaTeX way");
    CHECK(LatexDoc::matchKey(LatexDoc::displayText("$\\alpha + \\beta$")) ==
          "\xCE\xB1\xCE\xB2");

    GROUP("latex:accents-stay-in-the-run");
    // \ss, \c{c} and \'{e} are letters of a word, not markup that ends a run.
    std::string accents =
        "\\begin{document}\nCaf\\'{e} in der Stra\\ss e, Fran\\c{c}ais.\n"
        "\\end{document}\n";
    LatexDoc acc = LatexDoc::parse(accents);
    CHECK(acc.spans.size() == 1);
    CHECK(sourceOf(accents, acc.spanForClick({2}, "Stra\xC3\x9F" "e")) ==
          "Caf\\'{e} in der Stra\\ss e, Fran\\c{c}ais.");
    CHECK(acc.spanForClick({2}, "Fran\xC3\xA7" "ais") != nullptr);
    CHECK(acc.spanForClick({2}, "Caf\xC3\xA9") != nullptr);
    std::string ligs = "\\begin{document}\nAn efficient office.\n\\end{document}\n";
    CHECK(LatexDoc::parse(ligs).spanForClick({2}, "e\xEF\xAC\x83" "cient") != nullptr);
    CHECK(LatexDoc::parse(ligs).spanForClick({2}, "o\xEF\xAC\x83" "ce") != nullptr);

    GROUP("latex:row-break");
    // \\ ends a run like & does, so a cell or a resume line edits alone.
    std::string rows =
        "\\begin{document}\n\\textit{Principal Engineer} \\hfill 2014 -- 2019\\\\\n"
        "Next line\n\\end{document}\n";
    LatexDoc rw = LatexDoc::parse(rows);
    CHECK(spanWithSource(rw, rows, "2014 -- 2019") != nullptr);
    CHECK(spanWithSource(rw, rows, "Next line") != nullptr);

    GROUP("latex:url");
    // \url prints its argument, so the URL is the text to edit.
    std::string urls = "\\begin{document}\nSee \\url{https://example.org} now.\n"
                       "\\end{document}\n";
    LatexDoc ur = LatexDoc::parse(urls);
    const LatexSpan *url = spanWithSource(ur, urls, "https://example.org");
    CHECK(url && url->kind == LatexSpanKind::Field && url->command == "url");
    CHECK(ur.spanForClick({2}, "https://example.org") == url);

    GROUP("latex:maketitle");
    // \title sits in the preamble; SyncTeX places its text at \maketitle, or
    // the line after it.
    std::string titled =
        "\\documentclass{article}\n\\title{Torture Document}\n"
        "\\author{Quentin Placeholder}\n\\begin{document}\n\\maketitle\n\n"
        "Body text.\n\\end{document}\n";
    LatexDoc tt = LatexDoc::parse(titled);
    CHECK(tt.titleLines.size() == 1 && tt.titleLines[0] == 5);
    const LatexSpan *ttl = tt.spanForClick({6}, "Document");
    CHECK(ttl && ttl->command == "title");
    const LatexSpan *ath = tt.spanForClick({5}, "Placeholder");
    CHECK(ath && ath->command == "author");
    CHECK(tt.spanForClick({6}, "Unrelated") == nullptr);

    GROUP("latex:multi-line-span");
    // A span is found from any line it covers, not only its first.
    std::string longPara =
        "\\begin{document}\nOne\ntwo\nthree\nfour\nfive\nsix seven\n\n"
        "\\end{document}\n";
    LatexDoc lp = LatexDoc::parse(longPara);
    CHECK(lp.spans.size() == 1 && lp.spans[0].line == 2 && lp.spans[0].endLine == 7);
    CHECK(lp.spanForClick({7}, "One") != nullptr);

    GROUP("latex:word-split-by-font");
    // foo\emph{bar}baz is one word on the page and three spans in the
    // source; a join covers all three as one balanced range.
    std::string split = "\\begin{document}\nSplit: foo\\emph{bar}baz here.\n"
                        "\\end{document}\n";
    LatexDoc sp = LatexDoc::parse(split);
    CHECK(sp.joins.size() == 1);
    const LatexSpan *joined = sp.spanForClick({2}, "foobarbaz");
    CHECK(sourceOf(split, joined) == "Split: foo\\emph{bar}baz here.");
    // Clicking the italic part alone still opens just that part.
    CHECK(sourceOf(split, sp.spanForClick({2}, "bar")) == "bar");
    // A spaced font change is two words, so nothing is joined.
    CHECK(LatexDoc::parse("\\begin{document}\nfoo \\emph{bar} baz\n\\end{document}")
              .joins.empty());
    // A join whose range would unbalance the braces is not offered.
    CHECK(LatexDoc::parse("\\begin{document}\n\\textbf{\\emph{x}}y\n\\end{document}")
              .joins.empty());
    // Replacing a join is still one exact splice.
    LatexDoc::Edit je = LatexDoc::replaceSpan(split, *joined, "new");
    CHECK(je.source == "\\begin{document}\nnew\n\\end{document}\n");

    GROUP("latex:footnote-mark");
    // The PDF glues the footnote mark onto the word: "footnote1", "1The".
    std::string fn = "\\begin{document}\nA footnote.\\footnote{The note.}\n"
                     "\\end{document}\n";
    LatexDoc fd = LatexDoc::parse(fn);
    CHECK(sourceOf(fn, fd.spanForClick({2}, "footnote.1")) == "A footnote.");
    CHECK(sourceOf(fn, fd.spanForClick({2}, "1The")) == "The note.");

    GROUP("latex:short-words");
    // A short word must be a whole word of the span: "at" is not in "Math",
    // "is" is not in "Visit". Pieces split by punctuation still count.
    std::string shorts = "\\begin{document}\n\\section{Math}\nVisit\n\n\n\n\n"
                         "Go end-to-end at 5~pm.\n\\end{document}\n";
    LatexDoc sh = LatexDoc::parse(shorts);
    CHECK(sh.spanForClick({2}, "at") == nullptr);           // not in "Math"
    CHECK(sh.spanForClick({2, 3}, "is") == nullptr);        // not in "Visit"
    CHECK(sh.spanForClick({8}, "to") != nullptr);
    CHECK(sh.spanForClick({8}, "5 pm") != nullptr);
    CHECK(sh.spanForClick({8}, "at") != nullptr);
    // Math still matches inside, since the PDF reads x^2 back as "x2".
    CHECK(LatexDoc::parse("\\begin{document}\n$x^2$\n\\end{document}")
              .spanForClick({2}, "x2") != nullptr);

    GROUP("latex:context");
    // The same word in several nearby spans: the words around the click on
    // the page decide which one it was.
    std::string ctx =
        "\\begin{document}\n"
        "A paragraph with \\textit{italic walrus} words, \\emph{emphasized pelican}\n"
        "words, and \\underline{underlined badger} words and more.\n"
        "\\end{document}\n";
    LatexDoc cx = LatexDoc::parse(ctx);
    // Without context the nearest wins; with it, the right one.
    CHECK(sourceOf(ctx, cx.spanForClick({3, 2}, "words", "emphasized pelican",
                                        ", and underlined")) == "words, and");
    CHECK(sourceOf(ctx, cx.spanForClick({3, 2}, "words", "italic walrus",
                                        ", emphasized")) == "words,");
    CHECK(sourceOf(ctx, cx.spanForClick({3, 2}, "words", "underlined badger",
                                        "and more")) == "words and more.");

    GROUP("latex:single-characters");
    // A single letter or digit is refused, whatever the page shows around it.
    // It could be text TeX made up (a page, section or list number), which
    // has no place in the source, and the word beside it opens the same span.
    std::string pages =
        "\\documentclass{article}\n"                                 // 1
        "\\begin{document}\n"                                        // 2
        "\\section{Lists}\n"                                         // 3
        "See Fig.~3 here, a note I wrote in 2 minutes.\n"             // 4
        "\\begin{enumerate}\n"                                       // 5
        "  \\item First numbered hedgehog.\n"                        // 6
        "  \\item Second numbered porcupine.\n"                      // 7
        "\\end{enumerate}\n"                                         // 8
        "\\begin{enumerate}[(a)]\n"                                  // 9
        "  \\item Lettered item.\n"                                  // 10
        "\\end{enumerate}\n"                                         // 11
        "$x + y = z$ closes the page.\n"                              // 12
        "\\end{document}\n";
    LatexDoc pd = LatexDoc::parse(pages);
    // A page number at the foot of the page, below the last paragraph. It
    // used to fall through to the nearest span and open that paragraph.
    CHECK(pd.spanForClick({12, 11}, "2", "z closes the page.", "") == nullptr);
    CHECK(pd.spanForClick({12}, "2") == nullptr);
    // A section number, in front of its heading. It used to open the heading.
    CHECK(pd.spanForClick({3, 4}, "1", "", "Lists See Fig. 3 here") == nullptr);
    CHECK(pd.spanForClick({3}, "1") == nullptr);
    // A list number and a list label, in front of their items. They used to
    // open the item; that is the price of never opening the wrong text.
    CHECK(pd.spanForClick({6}, "1", "2 minutes.", ". First numbered") == nullptr);
    CHECK(pd.spanForClick({7}, "2.", "hedgehog.", "Second numbered") == nullptr);
    CHECK(pd.spanForClick({10}, "(a)", "porcupine.", "Lettered item.") == nullptr);
    // Real single characters in the source are refused too, even where the
    // text around them agrees: a digit, one-letter words, a math variable.
    CHECK(pd.spanForClick({4}, "3", "See Fig.", "here, a note") == nullptr);
    CHECK(pd.spanForClick({4}, "a", "Fig. 3 here,", "note I wrote") == nullptr);
    CHECK(pd.spanForClick({4}, "I", "here, a note", "wrote in 2") == nullptr);
    CHECK(pd.spanForClick({12}, "y", "x +", "= z closes") == nullptr);
    // One character is one character in any script: a Greek letter, an
    // accented letter.
    std::string greek = "\\begin{document}\n$\\alpha + \\beta$ and "
                        "\\'e t\\'e\n\\end{document}\n";
    LatexDoc gd = LatexDoc::parse(greek);
    CHECK(gd.spanForClick({2}, "\xCE\xB1") == nullptr);                 // α
    CHECK(gd.spanForClick({2}, "\xCE\xB1", "", "+ \xCE\xB2 and") == nullptr);
    CHECK(gd.spanForClick({2}, "\xC3\xA9", "and", "t\xC3\xA9") == nullptr);   // é
    CHECK(gd.spanForClick({2}, "t\xC3\xA9") != nullptr);                // "té"
    // Chinese and Japanese are the exception: the PDF often gives a single
    // ideograph as the word, and it is taken when the page's text around it
    // agrees with the source. Without context, or with context that does
    // not agree, it is still refused.
    std::string cjk =
        "\\begin{document}\n"
        "\\section{\xE7\xB4\xB9\xE4\xBB\x8B}\n"                           // 紹介
        "\xE4\xBB\x8A\xE6\x97\xA5\xE3\x81\xAF\xE9\x9B\xA8\xE3\x81\x8C"
        "\xE9\x99\x8D\xE3\x82\x8A\xE3\x81\xBE\xE3\x81\x99\xE3\x80\x82\n" // 今日は雨が降ります。
        "\\end{document}\n";
    LatexDoc cd = LatexDoc::parse(cjk);
    const char *rain = "\xE9\x9B\xA8";                                  // 雨
    CHECK(sourceOf(cjk, cd.spanForClick({3}, rain, "\xE4\xBB\x8A\xE6\x97\xA5\xE3\x81\xAF",
                                        "\xE3\x81\x8C\xE9\x99\x8D\xE3\x82\x8A")) ==
          "\xE4\xBB\x8A\xE6\x97\xA5\xE3\x81\xAF\xE9\x9B\xA8\xE3\x81\x8C"
          "\xE9\x99\x8D\xE3\x82\x8A\xE3\x81\xBE\xE3\x81\x99\xE3\x80\x82");
    CHECK(cd.spanForClick({3}, rain) == nullptr);                       // no context
    CHECK(cd.spanForClick({3}, rain, "\xE6\x98\x8E\xE6\x97\xA5",        // 明日
                          "\xE9\xA2\xA8") == nullptr);                  // 風: disagrees
    // Hangul too; a lone Latin letter beside CJK text is still refused.
    std::string ko = "\\begin{document}\n\xEB\x82\xA0\xEC\x94\xA8 \xEC\xA2\x8B\xEB\x8B\xA4 a\n\\end{document}\n";
    LatexDoc kd = LatexDoc::parse(ko);
    CHECK(kd.spanForClick({2}, "\xEC\x94\xA8", "\xEB\x82\xA0", "\xEC\xA2\x8B\xEB\x8B\xA4") != nullptr);
    CHECK(kd.spanForClick({2}, "a", "\xEC\xA2\x8B\xEB\x8B\xA4", "") == nullptr);
    // A line reported whole (for a glyph that is not a word) is the same:
    // one character is refused, and a line of words is still found.
    CHECK(pd.spanForClick({6}, "1.") == nullptr);
    CHECK(sourceOf(pages, pd.spanForClick({6}, "1. First numbered hedgehog.")) ==
          "First numbered hedgehog.");
    // The words beside them still open the right text.
    CHECK(sourceOf(pages, pd.spanForClick({6}, "First", "2 minutes. 1.",
                                          "numbered hedgehog.")) ==
          "First numbered hedgehog.");
    CHECK(sourceOf(pages, pd.spanForClick({4}, "note", "Fig. 3 here, a",
                                          "I wrote in 2")) ==
          "See Fig.~3 here, a note I wrote in 2 minutes.");
    CHECK(sourceOf(pages, pd.spanForClick({3, 4}, "Lists", "1", "See Fig.")) == "Lists");
    // Two characters are a word again ("at", "is" are checked above), and a
    // glyph with no letters or digits at all still offers the nearest span.
    CHECK(sourceOf(pages, pd.spanForClick({4}, "in", "a note I wrote", "2 minutes.")) ==
          "See Fig.~3 here, a note I wrote in 2 minutes.");
    CHECK(pd.spanForClick({6}, "\xE2\x80\xA2") != nullptr);   // a bullet
}

// A line box that SyncTeX files under line 23 (where its paragraph ended),
// with the glue between its words filed under the lines they were read on.
const char *kLineSyncTex =
    "SyncTeX Version:1\n"
    "Input:1:/tmp/doc.tex\n"
    "Unit:1\n"
    "X Offset:0\n"
    "Y Offset:0\n"
    "Content:\n"
    "{1\n"
    "[1,30:0,6553600:39321600,6553600,0\n"
    "(1,23:4718592,6553600:26214400,655360,131072\n"   // x=72 w=400, baseline 100
    "g1,20:9830400,6553600\n"                           // glue at x=150, line 20
    "g1,21:16384000,6553600\n"                          // glue at x=250, line 21
    "g1,24:30932992,6553600\n"                          // glue at the box's end
    ")\n"
    "]\n"
    "}1\n";

void testSyncTexText() {
    GROUP("synctex:text-lines");
    SyncTexIndex idx = SyncTexIndex::parse(kLineSyncTex);
    // The plain box lookup can only say "line 23".
    CHECK(idx.hitsAtPoint(1, 200, 95)[0].line == 23);
    // The glue beside the word knows better: right of it first, then left.
    std::vector<SyncTexHit> h = idx.textHitsAtPoint(1, 200, 95);
    CHECK(h.size() >= 3);
    CHECK(h.size() >= 3 && h[0].line == 21 && h[1].line == 20 && h[2].line == 23);
    CHECK(!h.empty() && std::abs(h[0].x - 72.0) < 0.01);   // anchored on the line
    CHECK(idx.textHitsAtPoint(1, 100, 95)[0].line == 20);
    // Glue on the box's end edge closes the box rather than following the
    // word, so the glue before the word comes first.
    std::vector<SyncTexHit> e = idx.textHitsAtPoint(1, 300, 95);
    CHECK(e.size() >= 2 && e[0].line == 21 && e[1].line == 24);
    // Off every line box, it is the plain lookup.
    CHECK(idx.textHitsAtPoint(1, 200, 300)[0].line ==
          idx.hitsAtPoint(1, 200, 300)[0].line);
    CHECK(idx.textHitsAtPoint(2, 200, 95).empty());
}

}  // namespace

// -------------------------------------------------------------------- JSON
namespace {

Json parsed(const std::string &text) {
    Json j;
    Json::parse(text, j);
    return j;
}

void testJson() {
    GROUP("json:parse");
    Json j;
    CHECK(Json::parse(" {\"a\": [1, 2.5, -3e2, true, false, null], \"b\": \"x\"} ", j));
    CHECK(j.isObject() && j.size() == 2);
    CHECK(j["a"].size() == 6);
    CHECK(j["a"][0].asInt() == 1);
    CHECK(j["a"][1].asNumber() == 2.5);
    CHECK(j["a"][2].asNumber() == -300);
    CHECK(j["a"][3].asBool() && !j["a"][4].asBool(true) && j["a"][5].isNull());
    CHECK(j["b"].asString() == "x");
    // Missing keys and out-of-range indexes read as null, never throw.
    CHECK(j["nope"].isNull() && j["nope"]["deeper"][3].isNull());
    CHECK(j["a"][99].isNull() && j["b"][0].isNull());
    CHECK(j["b"].asInt(7) == 7 && j["a"].asString().empty());

    GROUP("json:strings");
    CHECK(parsed("\"a\\\"b\\\\c\\/d\\n\\t\"").asString() == "a\"b\\c/d\n\t");
    CHECK(parsed("\"\\u00e9\"").asString() == "\xC3\xA9");                 // é
    CHECK(parsed("\"\\u20AC\"").asString() == "\xE2\x82\xAC");             // €
    CHECK(parsed("\"\\ud83d\\ude00\"").asString() == "\xF0\x9F\x98\x80");  // 😀 pair
    CHECK(parsed("\"\\ud83d\"").asString() == "\xEF\xBF\xBD");  // lone high -> U+FFFD
    CHECK(parsed("\"caf\xC3\xA9\"").asString() == "caf\xC3\xA9");  // raw UTF-8 kept

    GROUP("json:errors");
    std::string err;
    CHECK(!Json::parse("", j, &err) && !err.empty());
    CHECK(!Json::parse("{", j));
    CHECK(!Json::parse("[1,]", j));
    CHECK(!Json::parse("{\"a\" 1}", j));
    CHECK(!Json::parse("01", j));
    CHECK(!Json::parse("tru", j));
    CHECK(!Json::parse("\"unterminated", j));
    CHECK(!Json::parse("\"tab\there\"", j));   // raw control character
    CHECK(!Json::parse("1 2", j));
    CHECK(!Json::parse(std::string(300, '['), j));   // too deep, not a crash
    Json keep = Json(5);
    CHECK(!Json::parse("nope", keep) && keep.asInt() == 5);   // untouched on failure

    GROUP("json:dump");
    Json o = Json::object({{"id", 3}, {"name", "a\"b\n"}, {"ok", true}});
    o.set("list", Json::array().push(1).push(Json()).push(1.5));
    CHECK(o.dump() == "{\"id\":3,\"name\":\"a\\\"b\\n\",\"ok\":true,\"list\":[1,null,1.5]}");
    CHECK(Json(std::string("\x01")).dump() == "\"\\u0001\"");
    CHECK(Json(-42).dump() == "-42");
    CHECK(Json::object().dump() == "{}" && Json::array().dump() == "[]");
    o.set("id", 4);   // replaces in place, keeps order
    CHECK(o.dump().rfind("{\"id\":4,", 0) == 0);
    // Round trip.
    Json back;
    CHECK(Json::parse(o.dump(), back) && back == o);
    CHECK(parsed("{\"a\":1,\"b\":2}") == parsed("{\"b\":2,\"a\":1}"));
    CHECK(parsed("[1,2]") != parsed("[2,1]"));
}

// --------------------------------------------------------------------- LSP
void testLspFraming() {
    GROUP("lsp:framing");
    std::string one = "{\"a\":1}", two = "{\"b\":\"\xC3\xA9\"}";   // é is 2 bytes
    CHECK(Lsp::Framer::frame(one) == "Content-Length: 7\r\n\r\n{\"a\":1}");
    CHECK(Lsp::Framer::frame(two).find("Content-Length: 10\r\n") == 0);

    // Two messages in one read.
    Lsp::Framer f;
    f.feed(Lsp::Framer::frame(one) + Lsp::Framer::frame(two));
    std::string body;
    CHECK(f.next(body) && body == one);
    CHECK(f.next(body) && body == two);
    CHECK(!f.next(body));

    // One message dribbled in a byte at a time, header and body both split.
    std::string framed = Lsp::Framer::frame(two);
    Lsp::Framer g;
    int got = 0;
    for (char c : framed) {
        g.feed(&c, 1);
        while (g.next(body)) { got++; CHECK(body == two); }
    }
    CHECK(got == 1 && g.buffered() == 0);

    // Other headers, any case, and a message with an unusable header skipped.
    Lsp::Framer h;
    h.feed("content-type: application/vscode-jsonrpc; charset=utf-8\r\n"
           "CONTENT-LENGTH:7\r\n\r\n{\"a\":1}"
           "X-Nothing: 1\r\n\r\n"
           "Content-Length: 2\r\n\r\n[]");
    CHECK(h.next(body) && body == one);
    CHECK(h.next(body) && body == "[]");
    CHECK(!h.next(body));

    // Many messages: the consumed prefix is dropped, not kept forever.
    Lsp::Framer m;
    bool all = true;
    for (int i = 0; i < 1000; i++) {
        m.feed(Lsp::Framer::frame(one));
        if (!m.next(body) || body != one) all = false;
    }
    CHECK(all && m.buffered() == 0);
}

void testLspPositions() {
    GROUP("lsp:positions");
    using Lsp::Position;
    std::u16string t = Lsp::toUtf16("ab\ncd\r\nef\rgh");
    CHECK(Lsp::offsetForPosition(t, {0, 0}) == 0);
    CHECK(Lsp::offsetForPosition(t, {0, 2}) == 2);
    CHECK(Lsp::offsetForPosition(t, {0, 9}) == 2);    // clamps to the line end
    CHECK(Lsp::offsetForPosition(t, {1, 1}) == 4);
    CHECK(Lsp::offsetForPosition(t, {2, 0}) == 7);    // after \r\n
    CHECK(Lsp::offsetForPosition(t, {3, 2}) == 12);   // after a lone \r
    CHECK(Lsp::offsetForPosition(t, {9, 0}) == t.size());
    CHECK(Lsp::positionForOffset(t, 0) == (Position{0, 0}));
    CHECK(Lsp::positionForOffset(t, 4) == (Position{1, 1}));
    CHECK(Lsp::positionForOffset(t, 7) == (Position{2, 0}));
    CHECK(Lsp::positionForOffset(t, 11) == (Position{3, 1}));
    CHECK(Lsp::positionForOffset(t, 999) == (Position{3, 2}));
    // Round trip at every offset except inside \r\n.
    bool roundTrip = true;
    for (size_t i = 0; i <= t.size(); i++) {
        if (i == 6) continue;   // between \r and \n
        if (Lsp::offsetForPosition(t, Lsp::positionForOffset(t, i)) != i) roundTrip = false;
    }
    CHECK(roundTrip);

    // UTF-16 code units: é is one unit, 😀 is two (a surrogate pair), exactly
    // as NSString counts them. The server sees UTF-8, positions stay UTF-16.
    std::u16string u = Lsp::toUtf16("caf\xC3\xA9 \xF0\x9F\x98\x80 x\nnext");
    CHECK(u.size() == 14);
    CHECK(Lsp::positionForOffset(u, 8) == (Position{0, 8}));    // the x
    CHECK(u[8] == u'x');
    CHECK(Lsp::offsetForPosition(u, {1, 0}) == 10);
    CHECK(Lsp::toUtf8(u) == "caf\xC3\xA9 \xF0\x9F\x98\x80 x\nnext");
    CHECK(Lsp::toUtf16("\xFF").size() == 1 && Lsp::toUtf16("\xFF")[0] == 0xFFFD);

    GROUP("lsp:uri");
    CHECK(Lsp::uriFromPath("/Users/me/a b/c#.cpp") ==
          "file:///Users/me/a%20b/c%23.cpp");
    CHECK(Lsp::pathFromUri("file:///Users/me/a%20b/c%23.cpp") == "/Users/me/a b/c#.cpp");
    CHECK(Lsp::pathFromUri(Lsp::uriFromPath("/tmp/caf\xC3\xA9/x+y.h")) ==
          "/tmp/caf\xC3\xA9/x+y.h");
    CHECK(Lsp::uriFromPath("/tmp/caf\xC3\xA9") == "file:///tmp/caf%C3%A9");
    CHECK(Lsp::pathFromUri("file://localhost/etc/hosts") == "/etc/hosts");
    CHECK(Lsp::pathFromUri("FILE:///x%2") == "/x%2");   // bad escape kept literally
    CHECK(Lsp::pathFromUri("https://example.com/x").empty());
    CHECK(Lsp::pathFromUri("file:///a/b.cpp#L3") == "/a/b.cpp");
}

void testLspServers() {
    GROUP("lsp:servers");
    Lsp::Language l;
    CHECK(Lsp::languageForExtension("cpp", l) && l.server == "cpp" && l.languageId == "cpp");
    CHECK(Lsp::languageForExtension("C", l) && l.server == "cpp" && l.languageId == "c");
    CHECK(Lsp::languageForExtension("mm", l) && l.languageId == "objective-cpp");
    CHECK(Lsp::languageForExtension("h", l) && l.server == "cpp");
    CHECK(Lsp::languageForExtension("py", l) && l.server == "python");
    CHECK(Lsp::languageForExtension("go", l) && l.server == "go");
    CHECK(Lsp::languageForExtension("rs", l) && l.server == "rust");
    CHECK(Lsp::languageForExtension("tsx", l) && l.server == "typescript" &&
          l.languageId == "typescriptreact");
    CHECK(Lsp::languageForExtension("js", l) && l.languageId == "javascript");
    CHECK(!Lsp::languageForExtension("md", l));
    CHECK(!Lsp::languageForExtension("tex", l));
    CHECK(!Lsp::languageForExtension("png", l));
    CHECK(!Lsp::languageForExtension("", l));
    CHECK((Lsp::defaultCommands("python") ==
           std::vector<std::string>{"basedpyright-langserver --stdio",
                                    "pyright-langserver --stdio", "pylsp",
                                    "jedi-language-server"}));
    // Every server the settings know has install commands.
    for (const std::string &server : Settings::lspServers())
        CHECK(Install::server(server) && !Install::server(server)->ways.empty());
    CHECK(Install::server("cobol") == nullptr);
    CHECK(Lsp::defaultCommands("cobol").empty());
    CHECK(Lsp::serverDisplayName("cpp") == "C/C++");
    // Every server a file can map to is one the settings file knows.
    for (const char *ext : {"c", "py", "go", "rs", "ts"}) {
        Lsp::languageForExtension(ext, l);
        CHECK(std::find(Settings::lspServers().begin(), Settings::lspServers().end(),
                        l.server) != Settings::lspServers().end());
    }

    GROUP("lsp:servers-by-extension");
    {
        // The Mac's mapping: the optional built-in servers too.
        const std::map<std::string, std::vector<std::string>> none;
        CHECK(Lsp::languageForExtension("tex", l, none) && l.server == "latex" &&
              l.languageId == "latex");
        CHECK(Lsp::languageForExtension("bib", l, none) && l.server == "latex" &&
              l.languageId == "bibtex");
        CHECK(Lsp::languageForExtension("MD", l, none) && l.server == "markdown" &&
              l.languageId == "markdown");
        CHECK(Lsp::languageForExtension("toml", l, none) && l.server == "toml");
        CHECK(Lsp::languageForExtension("sh", l, none) && l.server == "shell" &&
              l.languageId == "shellscript");
        CHECK(Lsp::languageForExtension("py", l, none) && l.server == "python");
        CHECK(!Lsp::languageForExtension("yaml", l, none) && !Lsp::languageForExtension("", l, none));
        for (const std::string &s : Lsp::optionalServers()) {
            CHECK(Lsp::isOptionalServer(s) && !Lsp::defaultCommands(s).empty());
            CHECK(std::find(Settings::lspServers().begin(), Settings::lspServers().end(), s) !=
                  Settings::lspServers().end());
            CHECK(Lsp::serverDisplayName(s) != s);
        }
        CHECK(!Lsp::isOptionalServer("python") && !Lsp::isOptionalServer("yaml"));
        CHECK(Lsp::defaultCommands("markdown") == std::vector<std::string>{"marksman server"});
        // The settings' own: a new server, more files for a built-in one, and
        // a built-in extension taken over.
        std::vector<SettingsError> e;
        Settings st = parseSettings("lsp.yaml = yaml-language-server --stdio\n"
                                    "lsp.yaml.extensions = yaml, .YML  # both\n"
                                    "lsp.python.extensions = pyw sage\n"
                                    "lsp.mytex = my-tex-server\n"
                                    "lsp.mytex.extensions = tex\n", &e);
        CHECK(e.empty());
        CHECK(st.lspCommand("yaml") == "yaml-language-server --stdio");
        CHECK((st.lspExtensions().at("yaml") == std::vector<std::string>{"yaml", "yml"}));
        CHECK((st.lspCustomServers() == std::vector<std::string>{"mytex", "yaml"}));
        CHECK(Lsp::languageForExtension("yml", l, st.lspExtensions()) && l.server == "yaml" &&
              l.languageId == "yaml");
        CHECK(Lsp::languageForExtension("sage", l, st.lspExtensions()) && l.server == "python" &&
              l.languageId == "python");
        CHECK(Lsp::languageForExtension("py", l, st.lspExtensions()) && l.server == "python");
        CHECK(Lsp::languageForExtension("tex", l, st.lspExtensions()) && l.server == "mytex" &&
              l.languageId == "mytex");
        CHECK(Lsp::languageForExtension("bib", l, st.lspExtensions()) && l.server == "latex");
        // One line without the other, on the line that has it; a bad name.
        std::vector<SettingsError> e2;
        Settings bad = parseSettings("lsp.lua = lua-language-server\n"
                                     "lsp.zig.extensions = zig\n"
                                     "lsp.Bad Name = x\n"
                                     "lsp.json.extensions =  ,\n"
                                     "lsp.pyhton = pylsp\n", &e2);
        CHECK(e2.size() == 5);
        if (e2.size() == 5) {
            CHECK(e2[0].line == 1 && e2[0].message.find("lsp.lua.extensions") != std::string::npos);
            CHECK(e2[1].line == 2 && e2[1].message.find("add lsp.zig = ") != std::string::npos);
            CHECK(e2[2].line == 3);
            CHECK(e2[3].line == 4 && e2[3].message.find("missing extensions") != std::string::npos);
            CHECK(e2[4].line == 5 && e2[4].message.find("not a built-in server") != std::string::npos);
        }
        CHECK(bad.lspCommand("lua") == "lua-language-server");
        // The built-in ones need no extensions line.
        std::vector<SettingsError> e3;
        parseSettings("lsp.markdown = marksman server\nlsp.toml = off\n", &e3);
        CHECK(e3.empty());
    }

    GROUP("lsp:split-command");
    CHECK((Lsp::splitCommand("pyright-langserver --stdio") ==
           std::vector<std::string>{"pyright-langserver", "--stdio"}));
    CHECK((Lsp::splitCommand("  \"/opt/my tools/clangd\"  --log=error ") ==
           std::vector<std::string>{"/opt/my tools/clangd", "--log=error"}));
    CHECK((Lsp::splitCommand("a\\ b 'c \"d\"' \"e\\\"f\"") ==
           std::vector<std::string>{"a b", "c \"d\"", "e\"f"}));
    CHECK((Lsp::splitCommand("x ''") == std::vector<std::string>{"x", ""}));
    CHECK(Lsp::splitCommand("   ").empty());
}

void testLspParsing() {
    GROUP("lsp:diagnostics");
    std::string uri;
    std::vector<Lsp::Diagnostic> d = Lsp::parseDiagnostics(parsed(
        "{\"uri\":\"file:///a.cpp\",\"diagnostics\":["
        "{\"range\":{\"start\":{\"line\":2,\"character\":4},\"end\":{\"line\":2,\"character\":9}},"
        " \"severity\":1,\"message\":\"use of undeclared identifier 'x'\",\"source\":\"clang\","
        " \"code\":\"undeclared_var_use\"},"
        "{\"range\":{\"start\":{\"line\":5,\"character\":0},\"end\":{\"line\":5,\"character\":1}},"
        " \"severity\":2,\"message\":\"unused\",\"code\":42},"
        "{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":0}},"
        " \"message\":\"no severity\"}]}"), &uri);
    CHECK(uri == "file:///a.cpp");
    CHECK(d.size() == 3);
    CHECK(d.size() == 3 && d[0].range.start == (Lsp::Position{2, 4}) &&
          d[0].range.end == (Lsp::Position{2, 9}));
    CHECK(d.size() == 3 && d[0].severity == Lsp::Severity::Error && d[0].source == "clang");
    CHECK(d.size() == 3 && d[1].severity == Lsp::Severity::Warning && d[1].code == "42");
    CHECK(d.size() == 3 && d[2].severity == Lsp::Severity::Error);   // omitted -> error
    CHECK(Lsp::parseDiagnostics(parsed("{\"uri\":\"u\",\"diagnostics\":[]}")).empty());

    GROUP("lsp:completion");
    bool incomplete = false;
    std::vector<Lsp::CompletionItem> c = Lsp::parseCompletion(parsed(
        "{\"isIncomplete\":true,\"items\":["
        "{\"label\":\" size\",\"sortText\":\"2\",\"kind\":2,\"detail\":\"int\","
        " \"textEdit\":{\"range\":{\"start\":{\"line\":3,\"character\":6},"
        "\"end\":{\"line\":3,\"character\":6}},\"newText\":\"size()\"}},"
        "{\"label\":\"\xE2\x80\xA2" "push_back\",\"sortText\":\"1\",\"insertText\":\"push_back\"},"
        "{\"label\":\"empty\",\"filterText\":\"isEmpty\"},"
        "{\"label\":\"\"},"
        "{\"label\":\"at\",\"textEdit\":{\"insert\":{\"start\":{\"line\":1,\"character\":2},"
        "\"end\":{\"line\":1,\"character\":3}},\"replace\":{\"start\":{\"line\":1,\"character\":2},"
        "\"end\":{\"line\":1,\"character\":5}},\"newText\":\"at\"}}]}"), &incomplete);
    CHECK(incomplete);
    CHECK(c.size() == 4);   // the empty label is dropped
    // Sorted by sortText, falling back to the label: "1", "2", "at", "empty".
    CHECK(c.size() == 4 && c[0].label == "push_back" && c[1].label == "size");
    CHECK(c.size() == 4 && c[2].label == "at" && c[3].label == "empty");
    CHECK(c.size() == 4 && c[1].hasEdit && c[1].textToInsert() == "size()" &&
          c[1].editRange.start == (Lsp::Position{3, 6}));
    CHECK(c.size() == 4 && c[0].textToInsert() == "push_back" && !c[0].hasEdit);
    CHECK(c.size() == 4 && c[3].textToInsert() == "empty" && c[3].filterKey() == "isEmpty");
    CHECK(c.size() == 4 && c[2].hasEdit && c[2].editRange.end == (Lsp::Position{1, 3}));
    CHECK(c.size() == 4 && c[1].detail == "int" && c[1].kind == 2);
    // A bare array, and null.
    CHECK(Lsp::parseCompletion(parsed("[{\"label\":\"x\"}]"), &incomplete).size() == 1 &&
          !incomplete);
    CHECK(Lsp::parseCompletion(Json()).empty());

    GROUP("lsp:completion-filter");
    std::vector<Lsp::CompletionItem> all = Lsp::parseCompletion(parsed(
        "[{\"label\":\"push_back\"},{\"label\":\"pop_back\"},{\"label\":\"size\"},"
        "{\"label\":\"Capacity\"},{\"label\":\"reserve\"}]"));
    auto labels = [](const std::vector<Lsp::CompletionItem> &v) {
        std::string s;
        for (const auto &i : v) s += i.label + " ";
        return s;
    };
    CHECK(Lsp::filterCompletions(all, "").size() == 5);
    CHECK(labels(Lsp::filterCompletions(all, "p")) == "pop_back push_back Capacity ");
    CHECK(labels(Lsp::filterCompletions(all, "cap")) == "Capacity ");   // any case
    CHECK(labels(Lsp::filterCompletions(all, "pb")) == "pop_back push_back ");
    // Prefix matches come before subsequence matches.
    CHECK(labels(Lsp::filterCompletions(all, "s")) == "size push_back reserve ");
    CHECK(Lsp::filterCompletions(all, "zz").empty());

    GROUP("lsp:completion-rank");
    {
        // What pyright lists for "os." (sorted as it sends them: capitals first).
        std::vector<Lsp::CompletionItem> os = Lsp::parseCompletion(parsed(
            "[{\"label\":\"path\",\"sortText\":\"09.path\"},"
            "{\"label\":\"PathLike\",\"sortText\":\"09.PathLike\"},"
            "{\"label\":\"POSIX_SPAWN_CLOSE\",\"sortText\":\"09.POSIX_SPAWN_CLOSE\"},"
            "{\"label\":\"pardir\",\"sortText\":\"09.pardir\"},"
            "{\"label\":\"pathconf\",\"sortText\":\"09.pathconf\"},"
            "{\"label\":\"_path\",\"sortText\":\"10._path\"},"
            "{\"label\":\"getcwd\",\"sortText\":\"09.getcwd\"}]"));
        CHECK(labels(os) == "POSIX_SPAWN_CLOSE PathLike getcwd pardir path pathconf _path ");
        // The word typed out in full comes first, then same-case prefixes,
        // then other-case prefixes, then subsequences.
        CHECK(labels(Lsp::filterCompletions(os, "path")) == "path pathconf PathLike _path ");
        CHECK(labels(Lsp::filterCompletions(os, "pa")) ==
              "pardir path pathconf PathLike POSIX_SPAWN_CLOSE _path ");
        CHECK(labels(Lsp::filterCompletions(os, "Pa")) ==
              "PathLike pardir path pathconf POSIX_SPAWN_CLOSE _path ");
        CHECK(labels(Lsp::filterCompletions(os, "PathLike")) == "PathLike ");
        CHECK(labels(Lsp::filterCompletions(os, "pathlike")) == "PathLike ");
        // Within a group the server's sortText decides, not the label.
        std::vector<Lsp::CompletionItem> st = Lsp::parseCompletion(parsed(
            "[{\"label\":\"count\",\"sortText\":\"2\"},{\"label\":\"counter\",\"sortText\":\"1\"},"
            "{\"label\":\"Count\",\"sortText\":\"0\"}]"));
        CHECK(labels(Lsp::filterCompletions(st, "co")) == "counter count Count ");
        CHECK(labels(Lsp::filterCompletions(st, "count")) == "count counter Count ");
        CHECK(labels(Lsp::filterCompletions(st, "Count")) == "Count counter count ");
        // An exact match by insert text counts too (a label with a signature).
        std::vector<Lsp::CompletionItem> ins = Lsp::parseCompletion(parsed(
            "[{\"label\":\"append(x)\",\"insertText\":\"append\",\"filterText\":\"append\"},"
            "{\"label\":\"appendleft\"}]"));
        CHECK(Lsp::exactCompletion(Lsp::filterCompletions(os, "path"), "path") == 0);
        CHECK(Lsp::exactCompletion(Lsp::filterCompletions(os, "pa"), "pa") == -1);
        CHECK(Lsp::exactCompletion(Lsp::filterCompletions(os, "pathlike"), "pathlike") == -1);
        CHECK(Lsp::exactCompletion(Lsp::filterCompletions(ins, "append"), "append") == 0);
        CHECK(Lsp::exactCompletion(os, "") == -1);
    }

    GROUP("lsp:completion-keys");
    {
        using O = Lsp::CompletionOrigin;
        using K = Lsp::CompletionKey;
        // A list that came up by itself while typing: Return is a new line
        // ("name = nm" + Return stays "nm"), unless the arrows were used.
        CHECK(!Lsp::completionKeyAccepts(O::Typing, false, K::Return, false));
        CHECK(Lsp::completionKeyAccepts(O::Typing, true, K::Return, false));
        // Asked for, or after ".", "->", "::": Return takes the item.
        CHECK(Lsp::completionKeyAccepts(O::Explicit, false, K::Return, false));
        CHECK(Lsp::completionKeyAccepts(O::Trigger, false, K::Return, false));
        // The word typed out in full is a new line however the list came.
        CHECK(!Lsp::completionKeyAccepts(O::Explicit, false, K::Return, true));
        CHECK(!Lsp::completionKeyAccepts(O::Trigger, true, K::Return, true));
        CHECK(!Lsp::completionKeyAccepts(O::Typing, true, K::Return, true));
        // Tab takes the item always; Shift+Tab never.
        for (O o : {O::Typing, O::Trigger, O::Explicit})
            for (bool nav : {false, true}) {
                CHECK(Lsp::completionKeyAccepts(o, nav, K::Tab, false));
                CHECK(!Lsp::completionKeyAccepts(o, nav, K::ShiftTab, false));
            }
    }

    GROUP("lsp:hover");
    CHECK(Lsp::parseHover(parsed(
        "{\"contents\":{\"kind\":\"plaintext\",\"value\":\"int x\\n\\nA count.\"}}")) ==
          "int x\n\nA count.");
    CHECK(Lsp::parseHover(parsed(
        "{\"contents\":{\"kind\":\"markdown\",\"value\":\"### x\\n```cpp\\nint x\\n```\\n"
        "Uses \\\\_under\\\\_\"}}")) == "### x\nint x\nUses _under_");
    CHECK(Lsp::parseHover(parsed(
        "{\"contents\":[{\"language\":\"python\",\"value\":\"def f()\"},\"Docs.\",\"\"]}")) ==
          "def f()\n\nDocs.");
    CHECK(Lsp::parseHover(parsed("{\"contents\":\"plain\"}")) == "plain");
    // pyright's rule between signature and docs goes; one inside code stays.
    CHECK(Lsp::parseHover(parsed(
        "{\"contents\":{\"kind\":\"markdown\",\"value\":\"```python\\nx = 1\\n---\\n```\\n"
        "---\\nDocs.\"}}")) == "x = 1\n---\n\nDocs.");
    CHECK(Lsp::parseHover(Json()).empty());
    {
        // What basedpyright sends for a module: one Markdown part, kept whole.
        Lsp::HoverDoc d = Lsp::parseHoverDoc(parsed(
            "{\"contents\":{\"kind\":\"markdown\",\"value\":\"```python\\n(module) os\\n```\\n"
            "---\\nOS routines.\\n\"},\"range\":{\"start\":{\"line\":2,\"character\":7},"
            "\"end\":{\"line\":2,\"character\":9}}}"));
        CHECK(d.parts.size() == 1 && d.parts[0].markdown);
        CHECK(d.parts[0].text == "```python\n(module) os\n```\n---\nOS routines.");
        CHECK(d.hasRange && d.range.start.line == 2 && d.range.start.character == 7 &&
              d.range.end.character == 9);
        d = Lsp::parseHoverDoc(parsed(
            "{\"contents\":[{\"language\":\"python\",\"value\":\"def f()\"},\"Docs.\",\"\","
            "{\"language\":\"python\",\"value\":\"  \"}]}"));
        CHECK(d.parts.size() == 2 && !d.hasRange);
        CHECK(d.parts.size() == 2 && d.parts[0].text == "```python\ndef f()\n```" &&
              d.parts[0].markdown && d.parts[1].text == "Docs." && d.parts[1].markdown);
        d = Lsp::parseHoverDoc(parsed(
            "{\"contents\":{\"kind\":\"plaintext\",\"value\":\"int x *y\\n\"}}"));
        CHECK(d.parts.size() == 1 && !d.parts[0].markdown && d.parts[0].text == "int x *y");
        CHECK(Lsp::parseHoverDoc(Json()).parts.empty());
        CHECK(Lsp::parseHoverDoc(parsed("{\"contents\":[]}")).parts.empty());
    }

    GROUP("lsp:definition");
    std::vector<Lsp::Location> loc = Lsp::parseLocations(parsed(
        "{\"uri\":\"file:///a.h\",\"range\":{\"start\":{\"line\":4,\"character\":7},"
        "\"end\":{\"line\":4,\"character\":12}}}"));
    CHECK(loc.size() == 1 && loc[0].uri == "file:///a.h" &&
          loc[0].range.start == (Lsp::Position{4, 7}));
    loc = Lsp::parseLocations(parsed(
        "[{\"targetUri\":\"file:///b.h\","
        "\"targetRange\":{\"start\":{\"line\":1,\"character\":0},\"end\":{\"line\":9,\"character\":1}},"
        "\"targetSelectionRange\":{\"start\":{\"line\":2,\"character\":6},"
        "\"end\":{\"line\":2,\"character\":9}}},"
        "{\"uri\":\"file:///c.h\",\"range\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":0,\"character\":1}}}]"));
    CHECK(loc.size() == 2 && loc[0].uri == "file:///b.h" &&
          loc[0].range.start == (Lsp::Position{2, 6}));   // the name, not the body
    CHECK(loc.size() == 2 && loc[1].uri == "file:///c.h");
    CHECK(Lsp::parseLocations(Json()).empty());
    CHECK(Lsp::parseLocations(parsed("[]")).empty());
}

// A scripted exchange: everything the client writes is collected and parsed
// back, and the "server" answers by feeding framed replies to receive().
struct FakeServer {
    std::vector<Json> sent;
    Lsp::Framer framer;
    void take(const std::string &bytes) {
        framer.feed(bytes);
        std::string body;
        while (framer.next(body)) sent.push_back(parsed(body));
    }
    std::vector<std::string> methods() const {
        std::vector<std::string> m;
        for (const Json &j : sent) m.push_back(j["method"].asString());
        return m;
    }
};

std::string reply(int id, const std::string &resultJson) {
    return Lsp::Framer::frame("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
                              ",\"result\":" + resultJson + "}");
}

void testLspClient() {
    GROUP("lsp:client-handshake");
    FakeServer srv;
    Lsp::Client client([&](const std::string &b) { srv.take(b); });
    bool ready = false;
    client.onReady = [&] { ready = true; };
    CHECK(client.state() == Lsp::Client::State::Idle);

    client.initialize("/Users/me/my proj", 1234);
    CHECK(client.state() == Lsp::Client::State::Initializing);
    CHECK(srv.sent.size() == 1);
    const Json &init = srv.sent[0];
    CHECK(init["method"].asString() == "initialize" && init["id"].asInt() == 1);
    CHECK(init["jsonrpc"].asString() == "2.0");
    CHECK(init["params"]["processId"].asInt() == 1234);
    CHECK(init["params"]["rootUri"].asString() == "file:///Users/me/my%20proj");
    CHECK(init["params"]["workspaceFolders"][0]["name"].asString() == "my proj");
    CHECK(!init["params"]["capabilities"]["textDocument"]["completion"]
                ["completionItem"]["snippetSupport"].asBool(true));

    // Before the server answers, document traffic and requests wait.
    client.didOpen("file:///a.cpp", "cpp", "int x;");
    int hoverResult = 0;
    int hid = client.hover("file:///a.cpp", {0, 4}, [&](const Json &r, const Json &) {
        hoverResult = (int)r["n"].asInt();
    });
    CHECK(srv.sent.size() == 1 && client.queuedMessages() == 2);

    // A server request arriving mid-handshake is answered right away.
    client.receive(Lsp::Framer::frame(
        "{\"jsonrpc\":\"2.0\",\"id\":\"p1\",\"method\":\"window/workDoneProgress/create\","
        "\"params\":{}}"));
    CHECK(srv.sent.size() == 2 && srv.sent[1]["id"].asString() == "p1" &&
          srv.sent[1].has("result"));

    client.receive(reply(1, "{\"capabilities\":{\"textDocumentSync\":{\"save\":"
                            "{\"includeText\":true}},\"completionProvider\":"
                            "{\"triggerCharacters\":[\".\",\">\",\":\"]}},"
                            "\"serverInfo\":{\"name\":\"clangd\"}}"));
    CHECK(ready && client.state() == Lsp::Client::State::Ready);
    CHECK(client.serverName() == "clangd");
    CHECK((client.completionTriggers() == std::vector<std::string>{".", ">", ":"}));
    CHECK((srv.methods() == std::vector<std::string>{
        "initialize", "", "initialized", "textDocument/didOpen", "textDocument/hover"}));
    CHECK(client.queuedMessages() == 0);
    CHECK(srv.sent.size() == 5 &&
          srv.sent[3]["params"]["textDocument"]["text"].asString() == "int x;" &&
          srv.sent[3]["params"]["textDocument"]["version"].asInt() == 1 &&
          srv.sent[3]["params"]["textDocument"]["languageId"].asString() == "cpp");
    CHECK(srv.sent.size() == 5 && srv.sent[4]["id"].asInt() == hid);

    GROUP("lsp:client-sync");
    size_t before = srv.sent.size();
    client.didChange("file:///a.cpp", "int x;");   // unchanged: nothing sent
    CHECK(srv.sent.size() == before);
    client.didChange("file:///other.cpp", "x");    // not open: nothing sent
    CHECK(srv.sent.size() == before);
    client.didChange("file:///a.cpp", "int xy;");
    CHECK(srv.sent.size() == before + 1);
    const Json &ch = srv.sent.back();
    CHECK(ch["method"].asString() == "textDocument/didChange");
    CHECK(ch["params"]["textDocument"]["version"].asInt() == 2);
    CHECK(ch["params"]["contentChanges"][0]["text"].asString() == "int xy;");
    CHECK(client.version("file:///a.cpp") == 2);
    // Save sends pending text first, and includes it because the server asked.
    client.didSave("file:///a.cpp", "int xyz;");
    CHECK((srv.methods().back() == "textDocument/didSave"));
    CHECK(srv.sent.back()["params"]["text"].asString() == "int xyz;");
    CHECK(srv.sent[srv.sent.size() - 2]["method"].asString() == "textDocument/didChange");
    CHECK(client.version("file:///a.cpp") == 3);
    // Opening an open document again is a change, not a second didOpen.
    client.didOpen("file:///a.cpp", "cpp", "int q;");
    CHECK(srv.methods().back() == "textDocument/didChange");

    GROUP("lsp:client-responses");
    // Responses out of order each reach their own handler.
    std::string order;
    int a = client.definition("file:///a.cpp", {0, 4}, [&](const Json &r, const Json &) {
        order += "a" + std::to_string(r.size());
    });
    int b = client.completion("file:///a.cpp", {0, 5}, [&](const Json &r, const Json &e) {
        order += "b" + std::to_string(r.size()) + (e.isNull() ? "" : "E");
    }, ".");
    CHECK(srv.sent.back()["params"]["context"]["triggerKind"].asInt() == 2);
    CHECK(srv.sent.back()["params"]["context"]["triggerCharacter"].asString() == ".");
    CHECK(srv.sent.back()["params"]["position"]["character"].asInt() == 5);
    client.receive(Lsp::Framer::frame(
        "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(b) +
        ",\"error\":{\"code\":-32603,\"message\":\"boom\"}}") + reply(a, "[1,2]"));
    CHECK(order == "b0Ea2");
    client.receive(reply(hid, "{\"n\":7}"));   // the request queued before Ready
    CHECK(hoverResult == 7);
    // A reply nobody asked for is ignored.
    client.receive(reply(999, "null"));
    CHECK(client.pendingRequests() == 0);

    // Cancelled requests never reach their handler.
    bool ran = false;
    int c = client.hover("file:///a.cpp", {0, 0}, [&](const Json &, const Json &) { ran = true; });
    client.cancel(c);
    CHECK(srv.methods().back() == "$/cancelRequest" &&
          srv.sent.back()["params"]["id"].asInt() == c);
    client.receive(reply(c, "{}"));
    CHECK(!ran);

    GROUP("lsp:client-notifications");
    std::string diagUri;
    size_t diagCount = 0;
    client.onDiagnostics = [&](const std::string &u, const std::vector<Lsp::Diagnostic> &d) {
        diagUri = u;
        diagCount = d.size();
    };
    // A garbled message is reported, and the next one still gets through.
    std::string problem;
    client.onProtocolError = [&](const std::string &p) { problem = p; };
    client.receive(Lsp::Framer::frame("{not json") + Lsp::Framer::frame(
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":"
        "{\"uri\":\"file:///a.cpp\",\"diagnostics\":[{\"range\":{\"start\":{\"line\":0,"
        "\"character\":0},\"end\":{\"line\":0,\"character\":3}},\"message\":\"m\"}]}}"));
    CHECK(!problem.empty());
    CHECK(diagUri == "file:///a.cpp" && diagCount == 1);

    // Server requests: configuration gets one null per item; unknown methods
    // get MethodNotFound so the server isn't left waiting.
    client.receive(Lsp::Framer::frame(
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"workspace/configuration\","
        "\"params\":{\"items\":[{},{}]}}"));
    CHECK(srv.sent.back()["id"].asInt() == 5 && srv.sent.back()["result"].size() == 2);
    client.receive(Lsp::Framer::frame(
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"custom/somethingElse\",\"params\":{}}"));
    CHECK(srv.sent.back()["id"].asInt() == 6 &&
          srv.sent.back()["error"]["code"].asInt() == -32601);

    GROUP("lsp:client-close-shutdown");
    client.didClose("file:///a.cpp");
    CHECK(srv.methods().back() == "textDocument/didClose" && !client.isOpen("file:///a.cpp"));
    before = srv.sent.size();
    client.didClose("file:///a.cpp");   // twice: nothing
    CHECK(srv.sent.size() == before);

    bool done = false;
    client.shutdown([&] { done = true; });
    CHECK(client.state() == Lsp::Client::State::ShuttingDown);
    int sid = (int)srv.sent.back()["id"].asInt();
    CHECK(srv.methods().back() == "shutdown" && !done);
    // Nothing else goes out while shutting down.
    before = srv.sent.size();
    client.didOpen("file:///b.cpp", "cpp", "x");
    CHECK(client.hover("file:///b.cpp", {0, 0}, nullptr) == 0);
    CHECK(srv.sent.size() == before);
    client.receive(reply(sid, "null"));
    CHECK(done && client.state() == Lsp::Client::State::Exited);
    CHECK(srv.methods().back() == "exit");
    before = srv.sent.size();
    client.receive(Lsp::Framer::frame("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"x\"}"));
    CHECK(srv.sent.size() == before);   // an exited client answers nothing

    GROUP("lsp:client-exit-now");
    FakeServer s2;
    Lsp::Client quick([&](const std::string &b) { s2.take(b); });
    quick.initialize("/p", 1);
    quick.exitNow();   // before the handshake: exit only
    CHECK((s2.methods() == std::vector<std::string>{"initialize", "exit"}));
    FakeServer s3;
    Lsp::Client ready3([&](const std::string &b) { s3.take(b); });
    ready3.initialize("/p", 1);
    ready3.receive(reply(1, "{\"capabilities\":{}}"));
    ready3.exitNow();   // after it: shutdown and exit together, without waiting
    CHECK((s3.methods() == std::vector<std::string>{"initialize", "initialized",
                                                    "shutdown", "exit"}));
    bool d3 = false;
    ready3.shutdown([&] { d3 = true; });   // already exited: done at once
    CHECK(d3);

    // A failed initialize leaves the client exited, not waiting forever.
    FakeServer s4;
    Lsp::Client bad([&](const std::string &b) { s4.take(b); });
    std::string why, failed;
    bad.onProtocolError = [&](const std::string &p) { why = p; };
    bad.onInitializeFailed = [&](const std::string &m) { failed = m; };
    bad.initialize("/p", 1);
    bad.didOpen("file:///x.py", "python", "x");
    bad.receive(Lsp::Framer::frame(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-1,\"message\":\"no\"}}"));
    CHECK(bad.state() == Lsp::Client::State::Exited && why.find("no") != std::string::npos);
    CHECK(s4.sent.size() == 1 && bad.queuedMessages() == 0);
    CHECK(failed == "no");   // the owner hears it, to stop the process and say so
}

void testLspFeatures() {
    GROUP("lsp:completion-docs");
    std::vector<Lsp::CompletionItem> items = Lsp::parseCompletion(parsed(
        "[{\"label\":\"loads\",\"sortText\":\"2\",\"data\":{\"id\":7},"
        "  \"documentation\":{\"kind\":\"markdown\",\"value\":\"Read `s`.\"}},"
        " {\"label\":\"load\",\"sortText\":\"1\",\"documentation\":\"Read a file.\","
        "  \"detail\":\"  (function)  \"},"
        " {\"label\":\"loop\",\"sortText\":\"3\"}]"));
    CHECK(items.size() == 3);
    if (items.size() == 3) {
        CHECK(items[0].label == "load" && items[0].index == 0 && items[2].index == 2);
        CHECK(items[0].documentation == "Read a file." && !items[0].docIsMarkdown);
        CHECK(items[0].detail == "(function)");   // trimmed
        CHECK(items[1].documentation == "Read `s`." && items[1].docIsMarkdown);
        CHECK(items[1].raw && (*items[1].raw)["data"]["id"].asInt() == 7);
        CHECK(items[2].documentation.empty() && !items[2].resolved);
        // A filtered copy shares the raw item and keeps its index.
        std::vector<Lsp::CompletionItem> f = Lsp::filterCompletions(items, "loo");
        CHECK(f.size() == 1 && f[0].index == 2 && f[0].raw == items[2].raw);
        Lsp::applyResolved(items[2], parsed(
            "{\"label\":\"loop\",\"detail\":\"def loop() -> None\","
            "\"documentation\":{\"kind\":\"markdown\",\"value\":\"```python\\nloop()\\n```\"}}"));
        CHECK(items[2].resolved && items[2].detail == "def loop() -> None");
        CHECK(items[2].docIsMarkdown && items[2].documentation.find("loop()") != std::string::npos);
        // A resolve that adds nothing keeps what the item had.
        Lsp::applyResolved(items[0], parsed("{\"label\":\"load\",\"documentation\":\"  \"}"));
        CHECK(items[0].resolved && items[0].documentation == "Read a file." &&
              items[0].detail == "(function)");
        Lsp::applyResolved(items[1], Json());
        CHECK(items[1].resolved && items[1].documentation == "Read `s`.");
    }
    std::string text;
    bool md = true;
    CHECK(Lsp::parseMarkup(parsed("\"plain\""), text, md) && text == "plain" && !md);
    CHECK(Lsp::parseMarkup(parsed("{\"kind\":\"plaintext\",\"value\":\"p\"}"), text, md) &&
          text == "p" && !md);
    CHECK(!Lsp::parseMarkup(Json(), text, md) && !Lsp::parseMarkup(parsed("3"), text, md));

    GROUP("lsp:client-capabilities");
    FakeServer srv;
    Lsp::Client client([&](const std::string &b) { srv.take(b); });
    client.initialize("/p", 1);
    const Json &caps = srv.sent[0]["params"]["capabilities"];
    const Json &ci = caps["textDocument"]["completion"]["completionItem"];
    CHECK(ci["documentationFormat"][0].asString() == "markdown" &&
          ci["documentationFormat"][1].asString() == "plaintext");
    CHECK(caps["textDocument"]["hover"]["contentFormat"][0].asString() == "markdown");
    CHECK(ci["resolveSupport"]["properties"][0].asString() == "documentation" &&
          ci["resolveSupport"]["properties"][1].asString() == "detail");
    const Json &sh = caps["textDocument"]["signatureHelp"];
    CHECK(sh["signatureInformation"]["parameterInformation"]["labelOffsetSupport"].asBool());
    CHECK(sh["signatureInformation"]["documentationFormat"][0].asString() == "markdown");
    CHECK(sh["contextSupport"].asBool());
    CHECK(caps["workspace"]["configuration"].asBool());
    CHECK(!srv.sent[0]["params"].has("initializationOptions"));

    GROUP("lsp:client-resolve");
    client.receive(reply(1, "{\"capabilities\":{\"completionProvider\":"
                            "{\"resolveProvider\":true,\"triggerCharacters\":[\".\"]},"
                            "\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"],"
                            "\"retriggerCharacters\":[\")\"]}}}"));
    CHECK(client.canResolveCompletion());
    std::string resolvedDoc;
    Lsp::CompletionItem it = items.size() == 3 ? items[1] : Lsp::CompletionItem();
    int rid = client.resolveCompletion(it, [&](const Json &r, const Json &) {
        Lsp::applyResolved(it, r);
        resolvedDoc = it.documentation;
    });
    CHECK(rid > 0 && srv.methods().back() == "completionItem/resolve");
    // The item goes back exactly as it came, its data included.
    CHECK(srv.sent.back()["params"]["data"]["id"].asInt() == 7 &&
          srv.sent.back()["params"]["label"].asString() == "loads");
    client.receive(reply(rid, "{\"label\":\"loads\",\"documentation\":\"Parse s.\"}"));
    CHECK(resolvedDoc == "Parse s.");
    Lsp::CompletionItem bare;
    bare.label = "x";
    CHECK(client.resolveCompletion(bare, nullptr) == 0);   // no raw item: nothing sent
    FakeServer srv2;
    Lsp::Client noResolve([&](const std::string &b) { srv2.take(b); });
    noResolve.initialize("/p", 1);
    noResolve.receive(reply(1, "{\"capabilities\":{\"completionProvider\":{}}}"));
    CHECK(!noResolve.canResolveCompletion() && !noResolve.hasSignatureHelp());
    CHECK(noResolve.resolveCompletion(it, nullptr) == 0 &&
          srv2.methods().back() == "initialized");

    GROUP("lsp:signature-help");
    CHECK(client.hasSignatureHelp());
    CHECK((client.signatureTriggers() == std::vector<std::string>{"(", ","}));
    CHECK((client.signatureRetriggers() == std::vector<std::string>{")"}));
    int sid = client.signatureHelp("file:///a.py", {2, 9}, nullptr, "(");
    const Json &sctx = srv.sent.back()["params"]["context"];
    CHECK(sid > 0 && srv.methods().back() == "textDocument/signatureHelp");
    CHECK(sctx["triggerKind"].asInt() == 2 && sctx["triggerCharacter"].asString() == "(" &&
          !sctx["isRetrigger"].asBool(true));
    client.signatureHelp("file:///a.py", {2, 10}, nullptr, "", true);
    CHECK(srv.sent.back()["params"]["context"]["triggerKind"].asInt() == 3 &&
          srv.sent.back()["params"]["context"]["isRetrigger"].asBool());
    client.signatureHelp("file:///a.py", {2, 10}, nullptr);
    CHECK(srv.sent.back()["params"]["context"]["triggerKind"].asInt() == 1);

    Lsp::SignatureHelp help = Lsp::parseSignatureHelp(parsed(
        "{\"signatures\":[{\"label\":\"dumps(obj, *, indent=None)\","
        "\"documentation\":{\"kind\":\"markdown\",\"value\":\"Serialize.\"},"
        "\"parameters\":[{\"label\":\"obj\",\"documentation\":\" the object \"},"
        "{\"label\":\"*\"},{\"label\":\"indent=None\"},{\"label\":\"missing\"}]}],"
        "\"activeSignature\":0,\"activeParameter\":2}"));
    CHECK(help.signatures.size() == 1 && help.activeSignature == 0 && help.activeParameter == 2);
    if (help.signatures.size() == 1) {
        const Lsp::SignatureInfo &s = help.signatures[0];
        CHECK(s.label == "dumps(obj, *, indent=None)" && s.docIsMarkdown &&
              s.documentation == "Serialize.");
        CHECK(s.params.size() == 4 && s.params[0] == std::make_pair(6, 9) &&
              s.params[1] == std::make_pair(11, 12) && s.params[2] == std::make_pair(14, 25));
        CHECK(s.params.size() == 4 && s.params[3] == std::make_pair(-1, -1));
        CHECK(s.paramDocs.size() == 4 && s.paramDocs[0] == "the object");
    }
    // Offsets as given, in UTF-16; a string label after a non-ASCII name is
    // found in UTF-16 units too; a signature's own activeParameter wins.
    help = Lsp::parseSignatureHelp(parsed(
        "{\"signatures\":[{\"label\":\"f(a, b)\",\"parameters\":[{\"label\":[2,3]},"
        "{\"label\":[5,6]}]},{\"label\":\"caf\u00e9(x, y)\",\"activeParameter\":1,"
        "\"parameters\":[{\"label\":\"x\"},{\"label\":\"y\"}]}],"
        "\"activeSignature\":1,\"activeParameter\":0}"));
    CHECK(help.signatures.size() == 2 && help.activeSignature == 1 && help.activeParameter == 1);
    CHECK(help.signatures.size() == 2 && help.signatures[0].params[1] == std::make_pair(5, 6));
    CHECK(help.signatures.size() == 2 && help.signatures[1].params[0] == std::make_pair(5, 6) &&
          help.signatures[1].params[1] == std::make_pair(8, 9));
    // Out of range falls back; null and an empty list have no signatures.
    help = Lsp::parseSignatureHelp(parsed(
        "{\"signatures\":[{\"label\":\"g()\"}],\"activeSignature\":5,\"activeParameter\":3}"));
    CHECK(help.signatures.size() == 1 && help.activeSignature == 0 && help.activeParameter == -1);
    CHECK(Lsp::parseSignatureHelp(Json()).signatures.empty());
    CHECK(Lsp::parseSignatureHelp(parsed("{\"signatures\":[]}")).signatures.empty());

    GROUP("lsp:configuration");
    Json tree = parsed("{\"python\":{\"pythonPath\":\"/v/bin/python\","
                       "\"analysis\":{\"diagnosticMode\":\"openFilesOnly\"}}}");
    CHECK(Lsp::configurationSection(tree, "python")["pythonPath"].asString() == "/v/bin/python");
    CHECK(Lsp::configurationSection(tree, "python.analysis")["diagnosticMode"].asString() ==
          "openFilesOnly");
    CHECK(Lsp::configurationSection(tree, "python.analysis.diagnosticMode").asString() ==
          "openFilesOnly");
    CHECK(Lsp::configurationSection(tree, "pyright").isNull());
    CHECK(Lsp::configurationSection(tree, "python.nope.deeper").isNull());
    CHECK(Lsp::configurationSection(tree, "") == tree);
    CHECK(Lsp::configurationSection(Json(), "python").isNull());

    // Settings given before the handshake: initializationOptions go with
    // `initialize`, and the settings follow `initialized` before any queued
    // document traffic.
    FakeServer srv3;
    Lsp::Client py([&](const std::string &b) { srv3.take(b); });
    py.setInitializationOptions(parsed("{\"workspace\":{\"environmentPath\":\"/v\"}}"));
    py.setSettings(tree);
    py.initialize("/p", 1);
    CHECK(srv3.sent[0]["params"]["initializationOptions"]["workspace"]["environmentPath"]
              .asString() == "/v");
    py.didOpen("file:///p/a.py", "python", "import json");
    py.receive(reply(1, "{\"capabilities\":{}}"));
    CHECK((srv3.methods() == std::vector<std::string>{"initialize", "initialized",
                                                      "workspace/didChangeConfiguration",
                                                      "textDocument/didOpen"}));
    CHECK(srv3.sent.size() == 4 && srv3.sent[2]["params"]["settings"] == tree);
    // workspace/configuration is answered from them, item by item.
    py.receive(Lsp::Framer::frame(
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"workspace/configuration\","
        "\"params\":{\"items\":[{\"section\":\"python\"},{\"section\":\"python.analysis\"},"
        "{\"section\":\"pyright\"},{\"scopeUri\":\"file:///p\"}]}}"));
    const Json &answer = srv3.sent.back();
    CHECK(answer["id"].asInt() == 9 && answer["result"].size() == 4);
    CHECK(answer["result"][0]["pythonPath"].asString() == "/v/bin/python");
    CHECK(answer["result"][1]["diagnosticMode"].asString() == "openFilesOnly");
    CHECK(answer["result"][2].isNull() && answer["result"][3] == tree);
    // A change is stored and sent.
    Json tree2 = parsed("{\"python\":{\"pythonPath\":\"/w/bin/python\"}}");
    py.didChangeConfiguration(tree2);
    CHECK(srv3.methods().back() == "workspace/didChangeConfiguration" &&
          srv3.sent.back()["params"]["settings"] == tree2 && py.settings() == tree2);
    // No settings: no didChangeConfiguration after initialized.
    {
        const std::vector<std::string> ms = srv.methods();
        CHECK(std::count(ms.begin(), ms.end(),
                         std::string("workspace/didChangeConfiguration")) == 0);
    }

    GROUP("lsp:python-servers");
    using Lsp::PythonServer;
    auto kind = [](const std::string &cmd) { return Lsp::pythonServerKind(Lsp::splitCommand(cmd)); };
    CHECK(kind("pyright-langserver --stdio") == PythonServer::Pyright);
    CHECK(kind("/opt/homebrew/bin/pyright-langserver --stdio") == PythonServer::Pyright);
    CHECK(kind("node /x/node_modules/pyright/langserver.index.js --stdio") == PythonServer::Pyright);
    CHECK(kind("basedpyright-langserver --stdio") == PythonServer::BasedPyright);
    CHECK(kind("pylsp -v") == PythonServer::Pylsp);
    CHECK(kind("/v/bin/python -m pylsp") == PythonServer::Pylsp);
    CHECK(kind("jedi-language-server") == PythonServer::Jedi);
    CHECK(kind("ruff server") == PythonServer::Other);
    CHECK(std::string(Lsp::pythonServerName(PythonServer::BasedPyright)) == "basedpyright");

    Json ps = Lsp::pythonSettings(PythonServer::Pyright, "/v/bin/python");
    CHECK(ps["python"]["pythonPath"].asString() == "/v/bin/python");
    CHECK(ps["python"]["analysis"]["useLibraryCodeForTypes"].asBool() &&
          ps["python"]["analysis"]["autoSearchPaths"].asBool() &&
          ps["python"]["analysis"]["diagnosticMode"].asString() == "openFilesOnly");
    CHECK(!Lsp::pythonSettings(PythonServer::Pyright, "")["python"].has("pythonPath"));
    Json bs = Lsp::pythonSettings(PythonServer::BasedPyright, "/v/bin/python");
    CHECK(bs["python"]["pythonPath"].asString() == "/v/bin/python" &&
          !bs["python"].has("analysis"));
    CHECK(bs["basedpyright"]["analysis"]["typeCheckingMode"].asString() == "standard");
    CHECK(Lsp::pythonSettings(PythonServer::Pylsp, "/v/bin/python")["pylsp"]["plugins"]["jedi"]
              ["environment"].asString() == "/v/bin/python");
    CHECK(Lsp::pythonSettings(PythonServer::Pylsp, "").isNull());
    CHECK(Lsp::pythonSettings(PythonServer::Jedi, "/v/bin/python").isNull());
    CHECK(Lsp::pythonInitializationOptions(PythonServer::Jedi, "/v/bin/python")["workspace"]
              ["environmentPath"].asString() == "/v/bin/python");
    CHECK(Lsp::pythonInitializationOptions(PythonServer::Jedi, "").isNull());
    CHECK(Lsp::pythonInitializationOptions(PythonServer::Pyright, "/v/bin/python").isNull());
    CHECK(Lsp::pythonInterpreterNeedsRestart(PythonServer::Jedi) &&
          Lsp::pythonInterpreterNeedsRestart(PythonServer::Other));
    CHECK(!Lsp::pythonInterpreterNeedsRestart(PythonServer::Pyright) &&
          !Lsp::pythonInterpreterNeedsRestart(PythonServer::BasedPyright) &&
          !Lsp::pythonInterpreterNeedsRestart(PythonServer::Pylsp));
}


// Rename, references and code actions: the parsing, the WorkspaceEdit
// flattening, and the requests against the scripted server.
void testLspEditing() {
    auto range = [](int l1, int c1, int l2, int c2) {
        return "{\"start\":{\"line\":" + std::to_string(l1) + ",\"character\":" +
               std::to_string(c1) + "},\"end\":{\"line\":" + std::to_string(l2) +
               ",\"character\":" + std::to_string(c2) + "}}";
    };
    auto edit = [&](int l1, int c1, int l2, int c2, const std::string &text) {
        return "{\"range\":" + range(l1, c1, l2, c2) + ",\"newText\":\"" + text + "\"}";
    };

    GROUP("lsp:rename-edit");
    {
        // `changes`: one file, edits given out of order come back sorted.
        Lsp::WorkspaceEdit we = Lsp::parseWorkspaceEdit(parsed(
            "{\"changes\":{\"file:///p/a.py\":[" + edit(2, 4, 2, 7, "new") + "," +
            edit(0, 0, 0, 3, "new") + "],\"file:///p/b.py\":[" + edit(5, 1, 5, 4, "new") +
            "]}}"));
        CHECK(we.problem.empty() && !we.resourceOperations);
        CHECK(we.files.size() == 2 && we.editCount() == 3);
        CHECK(we.files.size() == 2 && we.files[0].uri == "file:///p/a.py" &&
              we.files[0].edits[0].range.start == (Lsp::Position{0, 0}) &&
              we.files[0].edits[1].range.start == (Lsp::Position{2, 4}));
        const std::u16string text = u"old = 1\nprint(old)\nx = old + old\n";
        Lsp::WorkspaceEdit w2 = Lsp::parseWorkspaceEdit(parsed(
            "{\"changes\":{\"file:///a\":[" + edit(2, 10, 2, 13, "renamed") + "," +
            edit(0, 0, 0, 3, "renamed") + "," + edit(1, 6, 1, 9, "renamed") + "," +
            edit(2, 4, 2, 7, "renamed") + "]}}"));
        CHECK(w2.files.size() == 1 &&
              Lsp::applyTextEdits(text, w2.files[0].edits) ==
                  u"renamed = 1\nprint(renamed)\nx = renamed + renamed\n");
        // Offsets come last first, in UTF-16 units (é is one, 😀 two).
        const std::u16string wide = u"é😀 ab ab";
        std::vector<Lsp::TextEdit> two = {{{{0, 4}, {0, 6}}, "X"}, {{{0, 7}, {0, 9}}, "Y"}};
        std::vector<Lsp::OffsetEdit> off = Lsp::offsetEdits(wide, two);
        CHECK(off.size() == 2 && off[0].start == 7 && off[0].length == 2 &&
              off[1].start == 4 && off[1].text == u"X");
        CHECK(Lsp::applyTextEdits(wide, two) == u"é😀 X Y");
        // Two insertions at one place keep the server's order.
        std::vector<Lsp::TextEdit> ins = {{{{0, 1}, {0, 1}}, "a"}, {{{0, 1}, {0, 1}}, "b"}};
        CHECK(Lsp::applyTextEdits(u"xy", ins) == u"xaby");
        // A position past the end of its line clamps there; CRLF lines count once.
        std::vector<Lsp::TextEdit> crlf = {{{{1, 0}, {1, 99}}, "Z"}};
        CHECK(Lsp::applyTextEdits(u"a\r\nbcd\r\ne", crlf) == u"a\r\nZ\r\ne");
    }

    GROUP("lsp:rename-document-changes");
    {
        // documentChanges wins over changes; two entries for one file merge;
        // an exact duplicate is dropped.
        Lsp::WorkspaceEdit we = Lsp::parseWorkspaceEdit(parsed(
            "{\"changes\":{\"file:///ignored\":[" + edit(0, 0, 0, 1, "q") + "]},"
            "\"documentChanges\":["
            "{\"textDocument\":{\"uri\":\"file:///p/b.py\",\"version\":3},\"edits\":[" +
            edit(1, 0, 1, 3, "zz") + "]},"
            "{\"textDocument\":{\"uri\":\"file:///p/a.py\",\"version\":null},\"edits\":[" +
            edit(0, 0, 0, 3, "zz") + "," + edit(0, 0, 0, 3, "zz") + "]},"
            "{\"textDocument\":{\"uri\":\"file:///p/b.py\",\"version\":3},\"edits\":[" +
            edit(0, 0, 0, 3, "zz") + "]}]}"));
        CHECK(we.problem.empty());
        CHECK(we.files.size() == 2 && we.files[0].uri == "file:///p/b.py" &&
              we.files[1].uri == "file:///p/a.py");
        CHECK(we.files.size() == 2 && we.files[0].edits.size() == 2 &&
              we.files[0].edits[0].range.start.line == 0 && we.files[1].edits.size() == 1);
        // A file operation refuses the whole edit.
        Lsp::WorkspaceEdit ops = Lsp::parseWorkspaceEdit(parsed(
            "{\"documentChanges\":[{\"kind\":\"rename\",\"oldUri\":\"file:///a\","
            "\"newUri\":\"file:///b\"},{\"textDocument\":{\"uri\":\"file:///c\"},\"edits\":[" +
            edit(0, 0, 0, 1, "x") + "]}]}"));
        CHECK(ops.resourceOperations && !ops.problem.empty() && ops.files.size() == 1);
        // Overlapping edits are a problem, not a guess.
        Lsp::WorkspaceEdit lap = Lsp::parseWorkspaceEdit(parsed(
            "{\"changes\":{\"file:///p/a.py\":[" + edit(0, 0, 0, 5, "x") + "," +
            edit(0, 3, 0, 8, "y") + "]}}"));
        CHECK(!lap.problem.empty() && lap.problem.find("/p/a.py") != std::string::npos);
        // Touching edits are fine; null and empty edits are nothing.
        Lsp::WorkspaceEdit touch = Lsp::parseWorkspaceEdit(parsed(
            "{\"changes\":{\"file:///a\":[" + edit(0, 0, 0, 2, "x") + "," +
            edit(0, 2, 0, 4, "y") + "],\"file:///empty\":[]}}"));
        CHECK(touch.problem.empty() && touch.files.size() == 1 && touch.editCount() == 2);
        CHECK(Lsp::parseWorkspaceEdit(Json()).editCount() == 0);
        CHECK(Lsp::parseWorkspaceEdit(parsed("{}")).files.empty());
        // The same edit to one file named two ways is made once.
        auto canon = [](const std::string &u) {
            return u.rfind("file:///tmp/", 0) == 0 ? "file:///private/tmp/" + u.substr(12) : u;
        };
        const std::string both = "{\"changes\":{\"file:///tmp/p/a.py\":[" + edit(0, 0, 0, 3, "new") +
                                 "],\"file:///private/tmp/p/a.py\":[" + edit(0, 0, 0, 3, "new") +
                                 "," + edit(2, 0, 2, 3, "new") + "]}}";
        Lsp::WorkspaceEdit merged = Lsp::parseWorkspaceEdit(parsed(both), canon);
        CHECK(merged.problem.empty() && merged.files.size() == 1 && merged.editCount() == 2 &&
              merged.files[0].uri == "file:///private/tmp/p/a.py");
        CHECK(Lsp::parseWorkspaceEdit(parsed(both)).files.size() == 2);
    }

    GROUP("lsp:rename-prepare");
    {
        Lsp::PrepareRename p = Lsp::parsePrepareRename(parsed(range(3, 4, 3, 9)));
        CHECK(p.ok && p.hasRange && p.range.start == (Lsp::Position{3, 4}) &&
              p.range.end == (Lsp::Position{3, 9}) && p.placeholder.empty());
        p = Lsp::parsePrepareRename(parsed("{\"range\":" + range(1, 0, 1, 3) +
                                           ",\"placeholder\":\"foo\"}"));
        CHECK(p.ok && p.hasRange && p.placeholder == "foo");
        p = Lsp::parsePrepareRename(parsed("{\"defaultBehavior\":true}"));
        CHECK(p.ok && !p.hasRange);
        CHECK(!Lsp::parsePrepareRename(Json()).ok);   // a keyword: not renameable
        CHECK(!Lsp::parsePrepareRename(parsed("{\"defaultBehavior\":false}")).ok);
        CHECK(!Lsp::parsePrepareRename(parsed("[]")).ok);
    }

    GROUP("lsp:references-group");
    {
        std::vector<Lsp::Location> locs = Lsp::parseLocations(parsed(
            "[{\"uri\":\"file:///p/z.py\",\"range\":" + range(9, 0, 9, 3) + "},"
            "{\"uri\":\"file:///p/b.py\",\"range\":" + range(4, 2, 4, 5) + "},"
            "{\"uri\":\"file:///p/main.py\",\"range\":" + range(7, 0, 7, 3) + "},"
            "{\"uri\":\"file:///p/b.py\",\"range\":" + range(1, 0, 1, 3) + "},"
            "{\"uri\":\"file:///p/main.py\",\"range\":" + range(2, 4, 2, 7) + "},"
            "{\"uri\":\"file:///p/b.py\",\"range\":" + range(1, 0, 1, 3) + "}]"));
        std::vector<Lsp::ReferenceFile> g = Lsp::groupReferences(locs, "file:///p/main.py");
        CHECK(g.size() == 3 && g[0].uri == "file:///p/main.py" && g[1].uri == "file:///p/b.py" &&
              g[2].uri == "file:///p/z.py");
        CHECK(g.size() == 3 && g[0].ranges.size() == 2 && g[0].ranges[0].start.line == 2);
        CHECK(g.size() == 3 && g[1].ranges.size() == 2);   // the duplicate went
        CHECK(Lsp::groupReferences({}, "x").empty());
        // One file named two ways (/tmp is /private/tmp) is one group.
        auto canon = [](const std::string &u) {
            return u.rfind("file:///tmp/", 0) == 0 ? "file:///private/tmp/" + u.substr(12) : u;
        };
        std::vector<Lsp::Location> twice = Lsp::parseLocations(parsed(
            "[{\"uri\":\"file:///tmp/p/a.py\",\"range\":" + range(1, 0, 1, 3) + "},"
            "{\"uri\":\"file:///private/tmp/p/a.py\",\"range\":" + range(1, 0, 1, 3) + "},"
            "{\"uri\":\"file:///private/tmp/p/a.py\",\"range\":" + range(5, 0, 5, 3) + "}]"));
        std::vector<Lsp::ReferenceFile> one = Lsp::groupReferences(twice, "file:///tmp/p/a.py", canon);
        CHECK(one.size() == 1 && one[0].uri == "file:///private/tmp/p/a.py" &&
              one[0].ranges.size() == 2);
        CHECK(Lsp::groupReferences(twice, "").size() == 2);   // without it, two
        CHECK(Lsp::lineOf(u"a\nbb\r\nccc", 1) == u"bb");
        CHECK(Lsp::lineOf(u"a\nbb\r\nccc", 2) == u"ccc");
        CHECK(Lsp::lineOf(u"a\nbb\r\nccc", 3).empty() && Lsp::lineOf(u"a", -1).empty());
        CHECK(Lsp::lineOf(u"a\n", 1).empty() && Lsp::lineOf(u"a\n", 0) == u"a");
    }

    GROUP("lsp:codeaction-parse");
    {
        std::string json =
            "[{\"title\":\"Run tests\",\"command\":\"test.run\",\"arguments\":[1]},"
            "{\"title\":\"Remove unused import\",\"kind\":\"quickfix\",\"edit\":{\"changes\":"
            "{\"file:///a.py\":[" + edit(0, 0, 1, 0, "") + "]}}},"
            "{\"title\":\"Add import os\",\"kind\":\"quickfix\",\"isPreferred\":true,"
            "\"command\":{\"title\":\"x\",\"command\":\"addImport\",\"arguments\":[\"os\"]}},"
            "{\"title\":\"Extract\",\"kind\":\"refactor.extract\",\"disabled\":{\"reason\":"
            "\"no selection\"}},"
            "{\"title\":\"Needs resolve\",\"kind\":\"quickfix\",\"data\":{\"id\":3}},"
            "{\"title\":\"\",\"command\":\"x\"}]";
        std::vector<Lsp::CodeAction> a = Lsp::parseCodeActions(parsed(json));
        CHECK(a.size() == 3);
        CHECK(a.size() == 3 && a[0].title == "Add import os" && a[0].preferred &&
              a[0].hasCommand() && !a[0].hasEdit());
        CHECK(a.size() == 3 && a[1].title == "Run tests" && a[1].hasCommand() &&
              a[1].command["arguments"][0].asInt() == 1 && !a[1].raw);
        CHECK(a.size() == 3 && a[2].title == "Remove unused import" && a[2].hasEdit() &&
              a[2].kind == "quickfix" &&
              Lsp::parseWorkspaceEdit(a[2].edit).editCount() == 1);
        // With resolve, an action with only data is offered too.
        std::vector<Lsp::CodeAction> r = Lsp::parseCodeActions(parsed(json), true);
        CHECK(r.size() == 4 && r[3].title == "Needs resolve" && r[3].raw &&
              (*r[3].raw)["data"]["id"].asInt() == 3);
        CHECK(Lsp::parseCodeActions(Json()).empty());
        Lsp::CodeAction one = Lsp::parseCodeAction(parsed(
            "{\"title\":\"T\",\"edit\":{\"changes\":{}},\"disabled\":{}}"));
        CHECK(one.title == "T" && one.hasEdit() && !one.disabledReason.empty());
    }

    GROUP("lsp:codeaction-diagnostics");
    {
        std::vector<Lsp::Diagnostic> d = Lsp::parseDiagnostics(parsed(
            "{\"uri\":\"file:///a.py\",\"diagnostics\":["
            "{\"range\":" + range(0, 7, 0, 9) + ",\"message\":\"\\\"os\\\" is not accessed\","
            "\"severity\":4,\"data\":{\"k\":1}},"
            "{\"range\":" + range(3, 0, 3, 5) + ",\"message\":\"other\"}]}"));
        CHECK(d.size() == 2 && d[0].raw && (*d[0].raw)["data"]["k"].asInt() == 1);
        // A caret inside, or at either end of, the first; not the second.
        Json in = Lsp::diagnosticsInRange(d, {{0, 8}, {0, 8}});
        CHECK(in.size() == 1 && in[0]["message"].asString() == "\"os\" is not accessed" &&
              in[0]["data"]["k"].asInt() == 1);
        CHECK(Lsp::diagnosticsInRange(d, {{0, 9}, {0, 9}}).size() == 1);
        CHECK(Lsp::diagnosticsInRange(d, {{0, 10}, {0, 10}}).size() == 0);
        CHECK(Lsp::diagnosticsInRange(d, {{0, 0}, {5, 0}}).size() == 2);
        Lsp::Diagnostic bare;
        bare.range = {{1, 1}, {1, 2}};
        bare.message = "m";
        bare.code = "E1";
        Json b = Lsp::diagnosticsInRange({bare}, {{1, 1}, {1, 1}});
        CHECK(b.size() == 1 && b[0]["code"].asString() == "E1" && b[0]["severity"].asInt() == 1);
    }

    GROUP("lsp:rename-client");
    FakeServer srv;
    Lsp::Client client([&](const std::string &b) { srv.take(b); });
    client.initialize("/p", 1);
    {
        const Json &caps = srv.sent[0]["params"]["capabilities"];
        CHECK(caps["textDocument"]["rename"]["prepareSupport"].asBool());
        CHECK(caps["textDocument"]["references"].isObject());
        const Json &ca = caps["textDocument"]["codeAction"];
        CHECK(ca["codeActionLiteralSupport"]["codeActionKind"]["valueSet"][1].asString() ==
              "quickfix");
        CHECK(ca["resolveSupport"]["properties"][0].asString() == "edit");
        CHECK(ca["isPreferredSupport"].asBool() && ca["dataSupport"].asBool());
        CHECK(caps["workspace"]["applyEdit"].asBool());
        CHECK(caps["workspace"]["workspaceEdit"]["documentChanges"].asBool());
        CHECK(caps["workspace"]["workspaceEdit"]["resourceOperations"].size() == 0);
    }
    client.receive(reply(1, "{\"capabilities\":{\"renameProvider\":{\"prepareProvider\":true},"
                            "\"referencesProvider\":true,\"codeActionProvider\":"
                            "{\"resolveProvider\":true}}}"));
    CHECK(client.hasRename() && client.canPrepareRename() && client.hasReferences() &&
          client.hasCodeActions() && client.canResolveCodeAction());
    {
        Lsp::PrepareRename prep;
        int pid = client.prepareRename("file:///p/a.py", {2, 5}, [&](const Json &r, const Json &) {
            prep = Lsp::parsePrepareRename(r);
        });
        CHECK(srv.sent.back()["method"].asString() == "textDocument/prepareRename" &&
              srv.sent.back()["params"]["position"]["line"].asInt() == 2);
        client.receive(reply(pid, range(2, 4, 2, 7)));
        CHECK(prep.ok && prep.range.end.character == 7);
        Lsp::WorkspaceEdit got;
        int rid = client.rename("file:///p/a.py", {2, 5}, "count", [&](const Json &r, const Json &) {
            got = Lsp::parseWorkspaceEdit(r);
        });
        CHECK(srv.sent.back()["method"].asString() == "textDocument/rename" &&
              srv.sent.back()["params"]["newName"].asString() == "count" &&
              srv.sent.back()["params"]["textDocument"]["uri"].asString() == "file:///p/a.py");
        client.receive(reply(rid, "{\"changes\":{\"file:///p/a.py\":[" +
                                      edit(2, 4, 2, 7, "count") + "]}}"));
        CHECK(got.editCount() == 1 && got.files[0].edits[0].newText == "count");
        // A keyword: prepareRename answers null, which refuses.
        bool refused = false;
        pid = client.prepareRename("file:///p/a.py", {0, 1}, [&](const Json &r, const Json &) {
            refused = !Lsp::parsePrepareRename(r).ok;
        });
        client.receive(reply(pid, "null"));
        CHECK(refused);
        FakeServer s2;
        Lsp::Client plain([&](const std::string &b) { s2.take(b); });
        plain.initialize("/p", 1);
        plain.receive(reply(1, "{\"capabilities\":{\"renameProvider\":true}}"));
        CHECK(plain.hasRename() && !plain.canPrepareRename() && !plain.hasReferences() &&
              !plain.hasCodeActions() && !plain.canResolveCodeAction());
    }

    GROUP("lsp:references-client");
    {
        size_t files = 0;
        int id = client.references("file:///p/a.py", {1, 2}, true, [&](const Json &r, const Json &) {
            files = Lsp::groupReferences(Lsp::parseLocations(r), "file:///p/a.py").size();
        });
        const Json &q = srv.sent.back();
        CHECK(q["method"].asString() == "textDocument/references" &&
              q["params"]["context"]["includeDeclaration"].asBool() &&
              q["params"]["position"]["character"].asInt() == 2);
        client.receive(reply(id, "[{\"uri\":\"file:///p/a.py\",\"range\":" + range(1, 0, 1, 3) +
                                     "},{\"uri\":\"file:///p/b.py\",\"range\":" +
                                     range(0, 0, 0, 3) + "}]"));
        CHECK(files == 2);
    }

    GROUP("lsp:codeaction-client");
    {
        std::vector<Lsp::CodeAction> actions;
        Json diags = parsed("[{\"range\":" + range(0, 7, 0, 9) + ",\"message\":\"m\"}]");
        int id = client.codeActions("file:///p/a.py", {{0, 7}, {0, 9}}, diags,
                                    [&](const Json &r, const Json &) {
                                        actions = Lsp::parseCodeActions(r, true);
                                    });
        const Json &q = srv.sent.back();
        CHECK(q["method"].asString() == "textDocument/codeAction" &&
              q["params"]["range"]["end"]["character"].asInt() == 9 &&
              q["params"]["context"]["diagnostics"][0]["message"].asString() == "m" &&
              q["params"]["context"]["triggerKind"].asInt() == 1);
        client.receive(reply(id, "[{\"title\":\"Remove import\",\"kind\":\"quickfix\","
                                 "\"data\":{\"n\":5}}]"));
        CHECK(actions.size() == 1 && !actions[0].hasEdit());
        // Resolve sends the action back as it came, and the answer has the edit.
        Lsp::CodeAction resolved;
        int rid = actions.empty() ? 0 : client.resolveCodeAction(actions[0],
            [&](const Json &r, const Json &) { resolved = Lsp::parseCodeAction(r); });
        CHECK(rid > 0 && srv.sent.back()["method"].asString() == "codeAction/resolve" &&
              srv.sent.back()["params"]["data"]["n"].asInt() == 5);
        client.receive(reply(rid, "{\"title\":\"Remove import\",\"edit\":{\"changes\":"
                                  "{\"file:///p/a.py\":[" + edit(0, 0, 1, 0, "") + "]}}}"));
        CHECK(resolved.hasEdit() && Lsp::parseWorkspaceEdit(resolved.edit).editCount() == 1);
        // A command goes out as workspace/executeCommand.
        int eid = client.executeCommand(parsed("{\"title\":\"t\",\"command\":\"pyright.organize\","
                                               "\"arguments\":[\"file:///p/a.py\"]}"),
                                        nullptr);
        CHECK(eid > 0 && srv.sent.back()["method"].asString() == "workspace/executeCommand" &&
              srv.sent.back()["params"]["command"].asString() == "pyright.organize" &&
              srv.sent.back()["params"]["arguments"][0].asString() == "file:///p/a.py");
    }

    GROUP("lsp:codeaction-apply-edit");
    {
        // workspace/applyEdit while the command runs: answered when the
        // owner says, once.
        Json gotEdit;
        Lsp::Client::ApplyEditReply later;
        client.onApplyEdit = [&](const Json &params, Lsp::Client::ApplyEditReply r) {
            gotEdit = params["edit"];
            later = r;
        };
        client.receive(Lsp::Framer::frame(
            "{\"jsonrpc\":\"2.0\",\"id\":41,\"method\":\"workspace/applyEdit\",\"params\":"
            "{\"label\":\"Organize\",\"edit\":{\"changes\":{\"file:///p/a.py\":[" +
            edit(0, 0, 0, 0, "import os\\n") + "]}}}}"));
        CHECK(Lsp::parseWorkspaceEdit(gotEdit).editCount() == 1);
        size_t before = srv.sent.size();
        CHECK(later != nullptr);
        if (later) later(true, "");
        CHECK(srv.sent.size() == before + 1 && srv.sent.back()["id"].asInt() == 41 &&
              srv.sent.back()["result"]["applied"].asBool());
        if (later) later(false, "again");   // a second answer is not sent
        CHECK(srv.sent.size() == before + 1);
        client.onApplyEdit = [&](const Json &, Lsp::Client::ApplyEditReply r) {
            r(false, "unsaved changes");
        };
        client.receive(Lsp::Framer::frame(
            "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"workspace/applyEdit\",\"params\":"
            "{\"edit\":{}}}"));
        CHECK(srv.sent.back()["id"].asInt() == 42 &&
              !srv.sent.back()["result"]["applied"].asBool(true) &&
              srv.sent.back()["result"]["failureReason"].asString() == "unsaved changes");
        // Nobody to apply it: refused, not left unanswered.
        client.onApplyEdit = nullptr;
        client.receive(Lsp::Framer::frame(
            "{\"jsonrpc\":\"2.0\",\"id\":43,\"method\":\"workspace/applyEdit\",\"params\":{}}"));
        CHECK(srv.sent.back()["id"].asInt() == 43 &&
              srv.sent.back()["result"].has("applied") &&
              !srv.sent.back()["result"]["applied"].asBool(true));
    }
}

// Accepting a completion with the edits that go with it (auto-import,
// #include), and diagnostics against the text they were computed on.
void testLspCompletionEditsAndVersions() {
    GROUP("lsp:completion-additional-edits");
    {
        std::vector<Lsp::CompletionItem> c = Lsp::parseCompletion(parsed(
            "[{\"label\":\"Path\",\"data\":{\"k\":1},\"additionalTextEdits\":["
            "{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,"
            "\"character\":0}},\"newText\":\"from pathlib import Path\\n\"}]},"
            "{\"label\":\"PurePath\",\"data\":{\"k\":2}},"
            "{\"label\":\"none\",\"additionalTextEdits\":[]},"
            "{\"label\":\"bad\",\"additionalTextEdits\":[{\"newText\":\"x\"},"
            "{\"range\":{\"start\":{\"line\":2,\"character\":1},\"end\":{\"line\":1,"
            "\"character\":0}},\"newText\":\"y\"}]}]"));
        CHECK(c.size() == 4);
        const Lsp::CompletionItem *path = nullptr, *pure = nullptr, *none = nullptr, *bad = nullptr;
        for (const auto &it : c) {
            if (it.label == "Path") path = &it;
            if (it.label == "PurePath") pure = &it;
            if (it.label == "none") none = &it;
            if (it.label == "bad") bad = &it;
        }
        CHECK(path && path->hasAdditionalEdits && path->additionalEdits.size() == 1 &&
              path->additionalEdits[0].newText == "from pathlib import Path\n" &&
              path->additionalEdits[0].range.start == (Lsp::Position{0, 0}));
        CHECK(pure && !pure->hasAdditionalEdits && pure->additionalEdits.empty());
        CHECK(none && none->hasAdditionalEdits && none->additionalEdits.empty());
        // A malformed edit is skipped; a backwards range is turned round.
        CHECK(bad && bad->additionalEdits.size() == 1 &&
              bad->additionalEdits[0].range.start == (Lsp::Position{1, 0}));
        // Waiting for resolve: only for an unresolved item that came without
        // the field, from a server that resolves.
        CHECK(pure && Lsp::completionWantsResolve(*pure, true));
        CHECK(pure && !Lsp::completionWantsResolve(*pure, false));
        CHECK(path && !Lsp::completionWantsResolve(*path, true));
        CHECK(none && !Lsp::completionWantsResolve(*none, true));
        Lsp::CompletionItem noRaw;
        noRaw.label = "x";
        CHECK(!Lsp::completionWantsResolve(noRaw, true));
        // The edit arriving on resolve (typescript-language-server's way).
        Lsp::CompletionItem later = pure ? *pure : Lsp::CompletionItem();
        Lsp::applyResolved(later, parsed(
            "{\"label\":\"PurePath\",\"additionalTextEdits\":[{\"range\":{\"start\":"
            "{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":0}},"
            "\"newText\":\"from pathlib import PurePath\\n\"}]}"));
        CHECK(later.resolved && later.hasAdditionalEdits && later.additionalEdits.size() == 1);
        CHECK(!Lsp::completionWantsResolve(later, true));
        Lsp::CompletionItem plain = pure ? *pure : Lsp::CompletionItem();
        Lsp::applyResolved(plain, parsed("{\"label\":\"PurePath\",\"detail\":\"class\"}"));
        CHECK(plain.resolved && !plain.hasAdditionalEdits && plain.detail == "class");
        CHECK(!Lsp::completionWantsResolve(plain, true));   // resolved: no second wait
    }

    GROUP("lsp:completion-edits");
    {
        auto u = [](const char *s) { return Lsp::toUtf16(s); };
        auto apply = [](std::u16string text, const Lsp::CompletionEdits &ce) {
            for (auto it = ce.edits.rbegin(); it != ce.edits.rend(); ++it)
                text.replace(it->start, it->length, it->text);
            return text;
        };
        auto te = [](int l0, int c0, int l1, int c1, const char *t) {
            return Lsp::TextEdit{Lsp::Range{{l0, c0}, {l1, c1}}, t};
        };
        // basedpyright's auto-import: "Pa" becomes Path, the import goes on
        // top, and the caret ends after the word, moved down by the import.
        std::u16string text = u("import os\n\nx = Pa");
        size_t start = text.size() - 2, end = text.size();
        Lsp::CompletionEdits ce = Lsp::completionEdits(
            text, start, end, u("Path"), {te(0, 0, 0, 0, "from pathlib import Path\n")});
        CHECK(!ce.droppedAdditional && ce.edits.size() == 2);
        CHECK(ce.edits.size() == 2 && ce.edits[0].start == 0 && ce.edits[1].start == start);
        CHECK(apply(text, ce) == u("from pathlib import Path\nimport os\n\nx = Path"));
        CHECK(ce.caret == apply(text, ce).size());
        // No additional edits: the word alone.
        ce = Lsp::completionEdits(text, start, end, u("Path"), {});
        CHECK(ce.edits.size() == 1 && apply(text, ce) == u("import os\n\nx = Path") &&
              ce.caret == text.size() + 2);
        // An edit after the word is placed after it, and the caret does not
        // move for it.
        std::u16string cpp = u("int f() { return sq");
        ce = Lsp::completionEdits(cpp, cpp.size() - 2, cpp.size(), u("sqrt"),
                                  {te(0, 19, 0, 19, "\n#include <cmath>")});
        CHECK(ce.edits.size() == 2 && ce.edits[0].text == u("sqrt"));
        CHECK(apply(cpp, ce) == u("int f() { return sqrt\n#include <cmath>"));
        CHECK(ce.caret == u("int f() { return sqrt").size());
        // An edit that replaces text before the word shifts the caret by
        // what it adds (here removes).
        std::u16string t2 = u("aaaa\nbb x");
        ce = Lsp::completionEdits(t2, t2.size() - 1, t2.size(), u("xyz"), {te(0, 0, 0, 4, "a")});
        CHECK(apply(t2, ce) == u("a\nbb xyz") && ce.caret == u("a\nbb xyz").size());
        // An edit inside the word being replaced is against the spec: all
        // the additional edits go, the word still goes in.
        ce = Lsp::completionEdits(t2, t2.size() - 1, t2.size(), u("xyz"),
                                  {te(0, 0, 0, 0, "import\n"), te(1, 3, 1, 4, "Q")});
        CHECK(ce.droppedAdditional && ce.edits.size() == 1 && apply(t2, ce) == u("aaaa\nbb xyz"));
        // Two additional edits that overlap each other: the same.
        ce = Lsp::completionEdits(t2, t2.size() - 1, t2.size(), u("xyz"),
                                  {te(0, 0, 0, 3, "1"), te(0, 2, 0, 4, "2")});
        CHECK(ce.droppedAdditional && apply(t2, ce) == u("aaaa\nbb xyz"));
        // Several imports, given out of order, all go in, in order.
        ce = Lsp::completionEdits(t2, t2.size() - 1, t2.size(), u("xyz"),
                                  {te(1, 0, 1, 0, "B"), te(0, 0, 0, 0, "A")});
        CHECK(!ce.droppedAdditional && apply(t2, ce) == u("Aaaaa\nBbb xyz") &&
              ce.caret == u("Aaaaa\nBbb xyz").size());
        // An insertion right where the word starts goes before it.
        ce = Lsp::completionEdits(t2, t2.size() - 1, t2.size(), u("xyz"), {te(1, 3, 1, 3, "ns.")});
        CHECK(!ce.droppedAdditional && apply(t2, ce) == u("aaaa\nbb ns.xyz") &&
              ce.caret == u("aaaa\nbb ns.xyz").size());
        // Out of range offsets are clamped, never past the text.
        ce = Lsp::completionEdits(t2, 100, 50, u("z"), {});
        CHECK(ce.edits.size() == 1 && ce.edits[0].start == t2.size() && ce.edits[0].length == 0);
    }

    GROUP("lsp:diagnostic-versions");
    {
        FakeServer srv;
        Lsp::Client client([&](const std::string &b) { srv.take(b); });
        client.initialize("/p", 1);
        client.receive(reply(1, "{\"capabilities\":{}}"));
        client.didOpen("file:///p/a.py", "python", "x = 1\n");
        client.didChange("file:///p/a.py", "x = 12\n");   // version 2
        int calls = 0;
        std::string base;
        client.onDiagnostics = [&](const std::string &uri, const std::vector<Lsp::Diagnostic> &) {
            calls++;
            const std::string *t = client.textFor(uri);
            base = t ? *t : "(none)";
        };
        auto publish = [&](const std::string &uri, const std::string &version) {
            client.receive(Lsp::Framer::frame(
                "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":"
                "{\"uri\":\"" + uri + "\"" + (version.empty() ? "" : ",\"version\":" + version) +
                ",\"diagnostics\":[]}}"));
        };
        publish("file:///p/a.py", "1");   // for the text before the change: dropped
        CHECK(calls == 0 && client.staleDiagnosticsDropped() == 1);
        publish("file:///p/a.py", "2");
        CHECK(calls == 1 && base == "x = 12\n");
        publish("file:///p/a.py", "");    // no version: taken as for the last text
        CHECK(calls == 2 && base == "x = 12\n");
        // The same file spelled another way (%61 is "a") is still found.
        publish("file:///p/%61.py", "1");
        CHECK(calls == 2 && client.staleDiagnosticsDropped() == 2);
        publish("file:///p/%61.py", "2");
        CHECK(calls == 3 && base == "x = 12\n");
        // A document not open: passed on, with no text to go by.
        publish("file:///p/b.py", "7");
        CHECK(calls == 4 && base == "(none)");
        CHECK((client.openUris() == std::vector<std::string>{"file:///p/a.py"}));
    }

    GROUP("lsp:diagnostic-spans");
    {
        auto u = [](const char *s) { return Lsp::toUtf16(s); };
        auto diag = [](int l0, int c0, int l1, int c1) {
            Lsp::Diagnostic d;
            d.range = Lsp::Range{{l0, c0}, {l1, c1}};
            return d;
        };
        // The server saw line 1 as "y = undefined_b"; since then "zz\n" was
        // typed at the start of line 1, and line 0 was left alone.
        std::u16string base = u("x = undefined_a\ny = undefined_b\nw = c\n");
        std::u16string now = u("x = undefined_a\nzz\ny = undefined_b\nw = c\n");
        std::vector<Lsp::Diagnostic> d = {diag(0, 4, 0, 15), diag(1, 4, 1, 15), diag(2, 4, 2, 5)};
        std::vector<Lsp::DiagnosticSpan> sp = Lsp::diagnosticSpans(&base, now, d);
        CHECK(sp.size() == 3);
        auto textAt = [&](const std::u16string &t, const Lsp::DiagnosticSpan &s) {
            return t.substr(s.start, s.end - s.start);
        };
        CHECK(sp.size() == 3 && textAt(now, sp[0]) == u("undefined_a") && sp[0].index == 0);
        CHECK(sp.size() == 3 && textAt(now, sp[1]) == u("undefined_b") && sp[1].index == 1);
        CHECK(sp.size() == 3 && textAt(now, sp[2]) == u("c") && sp[2].index == 2);
        // Typing at the end of the word a diagnostic is about: that one is
        // stale and left out, the others move or stay.
        std::u16string typed = u("x = undefined_aXY\ny = undefined_b\nw = c\n");
        sp = Lsp::diagnosticSpans(&base, typed, d);
        CHECK(sp.size() == 2 && sp[0].index == 1 && sp[1].index == 2);
        CHECK(sp.size() == 2 && textAt(typed, sp[0]) == u("undefined_b") &&
              textAt(typed, sp[1]) == u("c"));
        // Deleting text before them moves them back.
        std::u16string cut = u("x = 1\ny = undefined_b\nw = c\n");
        sp = Lsp::diagnosticSpans(&base, cut, d);
        CHECK(sp.size() == 2 && textAt(cut, sp[0]) == u("undefined_b"));
        // The text unchanged, or unknown: taken as given.
        sp = Lsp::diagnosticSpans(&base, base, d);
        CHECK(sp.size() == 3 && sp[1].start == 20 && sp[1].end == 31);
        sp = Lsp::diagnosticSpans(nullptr, base, d);
        CHECK(sp.size() == 3 && sp[1].start == 20);
        // An empty range (clang's "expected ';'") right where the text
        // changed touches it and goes; ones before and after stay.
        std::u16string b2 = u("int a = 1\nint b = 2");
        std::u16string n2 = u("int a = 1;\nint b = 2");
        sp = Lsp::diagnosticSpans(&b2, n2, {diag(0, 9, 0, 9), diag(1, 9, 1, 9), diag(0, 0, 0, 3)});
        CHECK(sp.size() == 2 && sp[0].index == 1 && sp[0].start == n2.size() &&
              sp[1].index == 2 && sp[1].start == 0 && sp[1].end == 3);
        // Positions past the end clamp.
        sp = Lsp::diagnosticSpans(nullptr, b2, {diag(9, 0, 9, 4)});
        CHECK(sp.size() == 1 && sp[0].start == b2.size() && sp[0].end == b2.size());
    }

    GROUP("lsp:problems-rows");
    {
        auto prob = [](const char *path, int line, int col, Lsp::Severity s, const char *msg) {
            Lsp::Problem p;
            p.path = path;
            p.line = line;
            p.column = col;
            p.severity = s;
            p.message = msg;
            return p;
        };
        using S = Lsp::Severity;
        std::vector<Lsp::Problem> ps = {
            prob("/p/b.py", 9, 0, S::Warning, "w1"),        // 0
            prob("/p/a.py", 5, 2, S::Hint, "h1"),           // 1
            prob("/p/c.py", 7, 4, S::Error, "e-late"),      // 2
            prob("/p/c.py", 2, 8, S::Warning, "w-early"),   // 3
            prob("/p/c.py", 2, 1, S::Error, "e-early"),     // 4
            prob("/p/b.py", 1, 0, S::Warning, "w0"),        // 5
            prob("/p/d.py", 0, 0, S::Information, "i"),     // 6
        };
        std::vector<Lsp::ProblemRow> rows = Lsp::problemRows(ps);
        // c.py (errors) first, then b.py (warnings), then a.py and d.py (the
        // rest, by path); within a file by severity, then position.
        std::vector<std::string> got;
        for (const Lsp::ProblemRow &r : rows)
            got.push_back(r.heading ? "# " + r.path : ps[r.problem].message);
        std::vector<std::string> want = {"# /p/c.py", "e-early", "e-late", "w-early",
                                         "# /p/b.py", "w0", "w1",
                                         "# /p/a.py", "h1", "# /p/d.py", "i"};
        CHECK(got == want);
        CHECK(rows[0].errors == 2 && rows[0].warnings == 1 && rows[0].others == 0);
        CHECK(rows[4].errors == 0 && rows[4].warnings == 2);
        CHECK(rows[7].others == 1 && rows[9].others == 1);
        CHECK(!rows[1].heading && rows[1].path == "/p/c.py" && rows[1].problem == 4);
        CHECK(Lsp::problemRows({}).empty());

        Lsp::ProblemCounts c = Lsp::countProblems(ps);
        CHECK(c.errors == 2 && c.warnings == 3 && c.infos == 1 && c.hints == 1 &&
              c.files == 4 && c.total() == 7);
        CHECK(Lsp::problemCountsText(c) == "2 errors, 3 warnings");
        CHECK(Lsp::problemCountsText(Lsp::countProblems({ps[0]})) == "1 warning");
        CHECK(Lsp::problemCountsText(Lsp::countProblems({ps[4]})) == "1 error");
        CHECK(Lsp::problemCountsText(Lsp::countProblems({ps[1], ps[6]})) == "1 info, 1 hint");
        CHECK(Lsp::problemCountsText(Lsp::countProblems({})) == "No problems");
        CHECK(Lsp::problemOneLine("  Argument of type \"int\"\n  is not\t\tassignable \r\n")
              == "Argument of type \"int\" is not assignable");
        CHECK(Lsp::problemOneLine("") == "" && Lsp::problemOneLine(" \n ") == "");
    }

    GROUP("lsp:problems-step");
    {
        // Out of order on purpose; two problems start at 20.
        std::vector<std::pair<size_t, size_t>> sp = {{40, 45}, {10, 12}, {20, 30}, {20, 22}};
        // Forward from the top: 10, then (as the caret sits at its start) on.
        CHECK(Lsp::stepProblem(sp, 0, -1, true) == 1);
        CHECK(Lsp::stepProblem(sp, 10, 1, true) == 3);    // {20,22} sorts before {20,30}
        CHECK(Lsp::stepProblem(sp, 20, 3, true) == 2);    // the other one at 20
        CHECK(Lsp::stepProblem(sp, 20, 2, true) == 0);
        CHECK(Lsp::stepProblem(sp, 40, 0, true) == 1);    // wraps
        // Backward the same way round.
        CHECK(Lsp::stepProblem(sp, 40, 0, false) == 2);
        CHECK(Lsp::stepProblem(sp, 20, 2, false) == 3);
        CHECK(Lsp::stepProblem(sp, 20, 3, false) == 1);
        CHECK(Lsp::stepProblem(sp, 10, 1, false) == 0);   // wraps to the last
        // The caret moved since the last step: from the caret, not `last`.
        CHECK(Lsp::stepProblem(sp, 25, 3, true) == 0);
        CHECK(Lsp::stepProblem(sp, 25, 3, false) == 2);   // the last of the two at 20
        CHECK(Lsp::stepProblem(sp, 50, -1, true) == 1);
        CHECK(Lsp::stepProblem(sp, 5, -1, false) == 0);
        // A caret on a problem's start with no step before goes past it.
        CHECK(Lsp::stepProblem(sp, 10, -1, true) == 3);
        CHECK(Lsp::stepProblem(sp, 10, -1, false) == 0);
        // `last` out of range is ignored; none at all is -1; one wraps to itself.
        CHECK(Lsp::stepProblem(sp, 0, 9, true) == 1);
        CHECK(Lsp::stepProblem({}, 0, -1, true) == -1);
        CHECK(Lsp::stepProblem({{3, 4}}, 3, 0, true) == 0);
        CHECK(Lsp::stepProblem({{3, 4}}, 3, 0, false) == 0);
    }
}
}  // namespace

// ------------------------------------------------------------- Python envs
namespace {

void makeExecutable(const std::filesystem::path &p, const std::string &bytes = "#!/bin/sh\n") {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
    std::filesystem::permissions(p, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add);
}

void testPythonEnvs() {
    namespace fs = std::filesystem;
    GROUP("python-envs");
    std::random_device rd;
    const fs::path base = fs::temp_directory_path() / ("minicode-pyenvs-" + std::to_string(rd()));
    // canonical, so paths compare with what realpath gives back
    fs::create_directories(base);
    const fs::path top = fs::canonical(base);
    const fs::path root = top / "proj", home = top / "home", bin = top / "bin";

    // The project: .venv at the top, venv in a subpackage, a stray file
    // named env that is no environment.
    makeExecutable(root / ".venv/bin/python3");
    { std::ofstream(root / ".venv/pyvenv.cfg") << "home = /x\nversion = 3.11.2\n"; }
    makeExecutable(root / "sub/pkg/venv/bin/python");
    { std::ofstream(root / "sub/pkg/venv/pyvenv.cfg") << "version_info = 3.12.4.final.0\n"; }
    { std::ofstream(root / "sub/env") << "not a venv"; }
    { std::ofstream(root / "pyproject.toml") << "[tool.poetry]\nname = \"My_Proj\"\n"; }
    // Above the root, never looked at.
    makeExecutable(top / ".venv/bin/python3");
    // conda, pyenv, poetry, pipenv, virtualenvwrapper, uv.
    makeExecutable(home / "miniconda3/bin/python3");
    { std::ofstream(home / "miniconda3/bin/../conda-meta-placeholder") << ""; }
    fs::create_directories(home / "miniconda3/conda-meta");
    { std::ofstream(home / "miniconda3/conda-meta/python-3.10.14-h_0_cpython.json") << "{}"; }
    makeExecutable(home / "miniconda3/envs/ml/bin/python");
    makeExecutable(home / ".pyenv/versions/3.9.18/bin/python3");
    makeExecutable(home / "Library/Caches/pypoetry/virtualenvs/my-proj-AbC12-py3.12/bin/python");
    makeExecutable(home / "Library/Caches/pypoetry/virtualenvs/other-XyZ98-py3.12/bin/python");
    makeExecutable(home / ".local/share/virtualenvs/proj-Q1w2E3/bin/python");
    makeExecutable(home / ".local/share/virtualenvs/elsewhere-Q1w2E3/bin/python");
    makeExecutable(home / ".virtualenvs/tools/bin/python3");
    makeExecutable(home / ".local/share/uv/python/cpython-3.13.1-macos-aarch64-none/bin/python3");
    // PATH: python3 -> python3.12, python3.11, python3-config (not a Python),
    // and a shim to skip.
    makeExecutable(bin / "python3.12");
    makeExecutable(bin / "python3.11");
    makeExecutable(bin / "python3-config");
    fs::create_symlink("python3.12", bin / "python3");
    makeExecutable(top / "shims/python3");

    using PythonEnvs::Interpreter;
    std::string R = root.string();
    CHECK(PythonEnvs::projectInterpreter(R, R) == R + "/.venv/bin/python3");
    CHECK(PythonEnvs::projectInterpreter(R + "/sub/pkg/mod", R) ==
          R + "/sub/pkg/venv/bin/python");
    CHECK(PythonEnvs::projectInterpreter(R + "/sub", R) == R + "/.venv/bin/python3");
    CHECK(PythonEnvs::projectInterpreter(top.string() + "/elsewhere", R) ==
          R + "/.venv/bin/python3");   // a file outside the root: the root's
    CHECK(PythonEnvs::projectInterpreter(top.string(), top.string() + "/nothing").empty());
    CHECK(PythonEnvs::interpreterIn(R + "/sub/env").empty());
    CHECK(PythonEnvs::versionOf(R + "/.venv/bin/python3") == "3.11.2");
    CHECK(PythonEnvs::versionOf(R + "/sub/pkg/venv/bin/python") == "3.12.4");
    CHECK(PythonEnvs::versionOf(home.string() + "/miniconda3/bin/python3") == "3.10.14");
    CHECK(PythonEnvs::versionOf((bin / "python3").string()) == "3.12");
    CHECK(PythonEnvs::versionOf(home.string() + "/.virtualenvs/tools/bin/python3").empty());

    PythonEnvs::Context ctx;
    ctx.fileDir = R + "/sub/pkg";
    ctx.root = R;
    ctx.home = home.string();
    ctx.env["VIRTUAL_ENV"] = R + "/.venv";   // the same as the project's: listed once
    ctx.pathDirs = {bin.string(), top.string() + "/shims", top.string() + "/missing"};
    ctx.skip = {top.string() + "/shims/python3"};
    std::vector<Interpreter> found = PythonEnvs::find(ctx);
    auto at = [&](const std::string &path) -> const Interpreter * {
        for (const Interpreter &i : found) if (i.path == path) return &i;
        return nullptr;
    };
    auto indexOf = [&](const std::string &path) {
        for (size_t i = 0; i < found.size(); i++) if (found[i].path == path) return (int)i;
        return -1;
    };
    const std::string H = home.string();
    CHECK(found.size() >= 2 && found[0].path == R + "/sub/pkg/venv/bin/python" &&
          found[0].kind == "project" && found[0].name == "sub/pkg/venv");
    CHECK(found.size() >= 2 && found[1].path == R + "/.venv/bin/python3" &&
          found[1].name == ".venv" && found[1].version == "3.11.2");
    CHECK(!at(top.string() + "/.venv/bin/python3"));
    int conda = indexOf(H + "/miniconda3/bin/python3");
    CHECK(conda > 1 && found[(size_t)conda].kind == "conda" && found[(size_t)conda].name == "base");
    CHECK(at(H + "/miniconda3/envs/ml/bin/python") &&
          at(H + "/miniconda3/envs/ml/bin/python")->name == "ml");
    CHECK(at(H + "/.pyenv/versions/3.9.18/bin/python3") &&
          at(H + "/.pyenv/versions/3.9.18/bin/python3")->version == "3.9.18");
    CHECK(at(H + "/Library/Caches/pypoetry/virtualenvs/my-proj-AbC12-py3.12/bin/python"));
    CHECK(!at(H + "/Library/Caches/pypoetry/virtualenvs/other-XyZ98-py3.12/bin/python"));
    CHECK(at(H + "/.local/share/virtualenvs/proj-Q1w2E3/bin/python") &&
          at(H + "/.local/share/virtualenvs/proj-Q1w2E3/bin/python")->kind == "pipenv");
    CHECK(!at(H + "/.local/share/virtualenvs/elsewhere-Q1w2E3/bin/python"));
    CHECK(at(H + "/.virtualenvs/tools/bin/python3"));
    const Interpreter *uv =
        at(H + "/.local/share/uv/python/cpython-3.13.1-macos-aarch64-none/bin/python3");
    CHECK(uv && uv->version == "3.13.1" && uv->kind == "uv");
    // PATH: python3 (the same file as python3.12, so 3.12 is not listed
    // again), then 3.11; never python3-config or the shim.
    int p3 = indexOf(bin.string() + "/python3");
    CHECK(p3 > conda && found[(size_t)p3].kind == "path" && found[(size_t)p3].version == "3.12");
    CHECK(!at(bin.string() + "/python3.12"));
    CHECK(indexOf(bin.string() + "/python3.11") == p3 + 1);
    CHECK(!at(bin.string() + "/python3-config") && !at(top.string() + "/shims/python3"));
    std::set<std::string> paths;
    for (const Interpreter &i : found) paths.insert(i.path);
    CHECK(paths.size() == found.size());

    // The automatic choice.
    CHECK(PythonEnvs::automatic(ctx) == R + "/sub/pkg/venv/bin/python");
    CHECK(PythonEnvs::automatic(ctx, true) == R + "/sub/pkg/venv/bin/python");
    // Without the user's consent the folder's own is left out: the active
    // environment, else nothing.
    {
        PythonEnvs::Context noProject = ctx;
        noProject.env.clear();
        CHECK(PythonEnvs::automatic(noProject, false).empty());
        noProject.env["VIRTUAL_ENV"] = H + "/.virtualenvs/tools";
        CHECK(PythonEnvs::automatic(noProject, false) == H + "/.virtualenvs/tools/bin/python3");
    }
    PythonEnvs::Context bare;
    bare.root = top.string() + "/nothing";
    bare.env["VIRTUAL_ENV"] = H + "/.virtualenvs/tools";
    CHECK(PythonEnvs::automatic(bare) == H + "/.virtualenvs/tools/bin/python3");
    bare.env.clear();
    bare.env["CONDA_PREFIX"] = H + "/miniconda3/envs/ml";
    CHECK(PythonEnvs::automatic(bare) == H + "/miniconda3/envs/ml/bin/python");
    bare.env.clear();
    CHECK(PythonEnvs::automatic(bare).empty());

    std::error_code ec;
    fs::remove_all(base, ec);
}

}  // namespace

// ---- TermLinks ---------------------------------------------------------------
static void testTermLinks() {
    using TermLinks::Link;
    auto one = [](const std::string& s) {
        auto v = TermLinks::find(s);
        return v.size() == 1 ? v[0] : Link{Link::Url, 0, 0, "<none>"};
    };
    // Compiler output: path, line, column, and the span stops before the message.
    {
        std::string s = "src/main.cpp:42:7: error: expected ';'";
        Link l = one(s);
        CHECK(l.kind == Link::File && l.target == "src/main.cpp");
        CHECK(l.line == 42 && l.column == 7);
        CHECK(s.substr(l.start, l.length) == "src/main.cpp:42:7");
    }
    { Link l = one("  at foo (lib/util.js:10:3)");
      CHECK(l.target == "lib/util.js" && l.line == 10 && l.column == 3); }
    { Link l = one("hello.py:3");
      CHECK(l.target == "hello.py" && l.line == 3 && l.column == 0); }
    // TypeScript and MSVC.
    { Link l = one("app.ts(12,5): error TS2322");
      CHECK(l.target == "app.ts" && l.line == 12 && l.column == 5); }
    // Python tracebacks.
    {
        std::string s = "  File \"/tmp/x/run.py\", line 17, in <module>";
        Link l = one(s);
        CHECK(l.target == "/tmp/x/run.py" && l.line == 17);
        CHECK(s.substr(l.start, l.length) == "/tmp/x/run.py");
    }
    // Plain paths, and punctuation that belongs to the sentence.
    { Link l = one("see ./docs/README.md.");  CHECK(l.target == "./docs/README.md"); }
    { Link l = one("~/code/notes.tex"); CHECK(l.target == "~/code/notes.tex"); }
    // URLs, with trailing punctuation and balanced parentheses.
    { Link l = one("docs at https://example.com/a?b=1.");
      CHECK(l.kind == Link::Url && l.target == "https://example.com/a?b=1"); }
    { Link l = one("(see https://en.wikipedia.org/wiki/Foo_(bar))");
      CHECK(l.target == "https://en.wikipedia.org/wiki/Foo_(bar)"); }
    { Link l = one("see https://en.wikipedia.org/wiki/Foo_(bar) for more");
      CHECK(l.target == "https://en.wikipedia.org/wiki/Foo_(bar)"); }
    { Link l = one("https://example.com/a_(b).");
      CHECK(l.target == "https://example.com/a_(b)"); }
    { Link l = one("(https://example.com/x)");
      CHECK(l.target == "https://example.com/x"); }
    // Not links: words, numbers, versions, a lone slash.
    CHECK(TermLinks::find("hello world").empty());
    CHECK(TermLinks::find("took 1.5 seconds").empty());
    CHECK(TermLinks::find("version 2.0.1").empty());
    CHECK(TermLinks::find("a / b").empty());
    // Several on a line, in order, and at().
    {
        std::string s = "a.cpp:1 b.h:2 https://x.io";
        auto v = TermLinks::find(s);
        CHECK(v.size() == 3);
        if (v.size() == 3) {
            CHECK(v[0].target == "a.cpp" && v[1].target == "b.h" &&
                  v[2].kind == Link::Url);
            CHECK(TermLinks::at(v, s.find("b.h") + 1) == &v[1]);
            CHECK(TermLinks::at(v, s.find(' ')) == nullptr);
        }
    }
    // Non-ASCII names are found whole.
    { Link l = one("données/résumé.tex:3"); CHECK(l.target == "données/résumé.tex" && l.line == 3); }
    // How Claude Code names files: in parentheses after a tool name, and
    // with a line in its summaries. The tool name itself is not a path.
    {
        std::string s = "⏺ Update(src/Terminal.mm)";
        Link l = one(s);
        CHECK(l.target == "src/Terminal.mm" && l.line == 0);
        CHECK(s.substr(l.start, l.length) == "src/Terminal.mm");
    }
    { Link l = one("  ⎿  Read src/main.mm:120 (40 lines)");
      CHECK(l.target == "src/main.mm" && l.line == 120); }
    // Inside backticks, as in Markdown output.
    { Link l = one("see `docs/demos/tour.gif` for it");
      CHECK(l.target == "docs/demos/tour.gif"); }
}

// ------------------------------------------------------------ folder search
namespace {

namespace fsys = std::filesystem;

void writeFile(const fsys::path &p, const std::string &bytes) {
    fsys::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
}

// Matches as "relative:line:column", in the order the search returned them.
std::vector<std::string> hitKeys(const FolderSearchResult &r) {
    std::vector<std::string> v;
    for (const auto &m : r.matches)
        v.push_back(m.relativePath + ":" + std::to_string(m.line) + ":" +
                    std::to_string(m.column));
    return v;
}

bool hasHit(const FolderSearchResult &r, const std::string &key) {
    auto v = hitKeys(r);
    return std::find(v.begin(), v.end(), key) != v.end();
}

bool anyHitIn(const FolderSearchResult &r, const std::string &relPrefix) {
    for (const auto &m : r.matches)
        if (m.relativePath.compare(0, relPrefix.size(), relPrefix) == 0) return true;
    return false;
}

void testFolderSearch() {
    using namespace FolderSearch;

    GROUP("folder-search:query");
    CHECK(normalizeQuery("  hello \t") == "hello");
    CHECK(normalizeQuery("\xC2\xA0x y\xE3\x80\x80") == "x y");   // NBSP, ideographic space
    CHECK(!isSearchable(""));
    CHECK(!isSearchable("a"));
    CHECK(!isSearchable("  a  "));
    CHECK(isSearchable("ab"));
    CHECK(isSearchable(" ab "));
    CHECK(!isSearchable("\xC3\xA9"));          // é: two bytes, one character
    CHECK(isSearchable("\xC3\xA9\xC3\xA9"));
    CHECK(!isSearchable("\xFF\xFE"));          // not UTF-8: never searched

    GROUP("folder-search:skip");
    CHECK(isSkippedDirectory("node_modules"));
    CHECK(isSkippedDirectory("build"));
    CHECK(isSkippedDirectory("__pycache__"));
    CHECK(isSkippedDirectory("DerivedData"));
    CHECK(isSkippedDirectory("venv"));
    CHECK(!isSkippedDirectory("src"));
    CHECK(!isSkippedDirectory("Build"));       // the Mac's list is case-sensitive

    GROUP("folder-search:utf8");
    CHECK(isValidUtf8("plain"));
    CHECK(isValidUtf8("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80"));
    CHECK(isValidUtf8(std::string("nul\0inside", 10)));   // NSString accepts NUL too
    CHECK(!isValidUtf8("\xC0\x80"));           // overlong NUL
    CHECK(!isValidUtf8("\xED\xA0\x80"));       // surrogate
    CHECK(!isValidUtf8("\xF4\x90\x80\x80"));   // past U+10FFFF
    CHECK(!isValidUtf8("\xE2\x82"));           // truncated
    CHECK(!isValidUtf8("\x80"));               // stray continuation byte

    GROUP("folder-search:clean");
    CHECK(cleanLine("\x1b[31mred\x1b[0m text") == "red text");
    CHECK(cleanLine("\x1b[1;32;40mbold\x1b[m") == "bold");
    CHECK(cleanLine("a\x07" "b\tc") == "abc");            // bell and tab are controls
    CHECK(cleanLine("\x1b[31") == "[31");                 // no final byte: only ESC goes
    CHECK(cleanLine("x\xE2\x80\xAEy") == "xy");           // RIGHT-TO-LEFT OVERRIDE
    CHECK(cleanLine("\xEF\xBB\xBFtop") == "top");         // byte-order mark
    CHECK(cleanLine("caf\xC3\xA9") == "caf\xC3\xA9");     // letters untouched
    CHECK(cleanLine("a\xC2\x85" "b") == "ab");            // C1 NEL
    CHECK(displayText("   \t  indented  ") == "indented");
    CHECK(displayText("\x1b[32m   green") == "green");     // cleaned, then trimmed
    {
        std::string longLine(300, 'x');
        CHECK(displayText(longLine).size() == 200);
        std::string wide;
        for (int i = 0; i < 250; ++i) wide += "\xC3\xA9";   // 250 two-byte chars
        const std::string cut = displayText(wide);
        CHECK(cut.size() == 400);                           // 200 whole characters
        CHECK(isValidUtf8(cut));
        CHECK(displayText("abcdef", 3) == "abc");
    }

    GROUP("folder-search:match");
    std::size_t col = 99, len = 99;
    CHECK(findInLine("Hello World", "world", false, &col, &len) && col == 6 && len == 5);
    CHECK(findInLine("HELLO", "hello", false, &col, &len) && col == 0);
    CHECK(!findInLine("Hello World", "world", true, &col, &len));
    CHECK(findInLine("Hello World", "World", true, &col, &len) && col == 6);
    CHECK(!findInLine("hel lo", "hello", false, &col, &len));
    CHECK(!findInLine("he", "hello", false, &col, &len));
    // ASCII query after multi-byte text: the byte column counts bytes.
    CHECK(findInLine("\xC3\xA9\xC3\xA9TODO", "todo", false, &col, &len) && col == 4 && len == 4);
    // Latin-1, Greek and Cyrillic fold; match length is in the line's bytes.
    CHECK(findInLine("\xC3\x89" "COLE", "\xC3\xA9" "cole", false, &col, &len) &&
          col == 0 && len == 6);
    CHECK(findInLine("x \xCE\xA3\xCE\x99\xCE\x93\xCE\x9C\xCE\x91", "\xCF\x83\xCE\xB9\xCE\xB3",
                     false, &col, &len) && col == 2 && len == 6);   // ΣΙΓΜΑ / σιγ
    CHECK(findInLine("\xD0\x9C\xD0\x98\xD0\xA0", "\xD0\xBC\xD0\xB8\xD1\x80",
                     false, &col, &len) && len == 6);               // МИР / мир
    CHECK(findInLine("\xC5\x81" "\xC3\xB3" "d\xC5\xBA", "\xC5\x82\xC3\xB3" "d\xC5\xBA",
                     false, &col, &len));                           // Łódź / łódź
    CHECK(!findInLine("\xC3\x89" "COLE", "\xC3\xA9" "cole", true, &col, &len));

    // ---- a real folder -------------------------------------------------
    std::random_device rd;
    const fsys::path root = fsys::temp_directory_path() /
        ("minicode-search-test-" + std::to_string(rd()));
    fsys::create_directories(root);
    const fsys::path outside = fsys::temp_directory_path() /
        ("minicode-search-outside-" + std::to_string(rd()));
    fsys::create_directories(outside);

    writeFile(root / "a.txt", "hello world\nnothing here\n  Second HELLO\n");
    writeFile(root / "sub" / "b.cpp", "int x; // hello\r\nfoo\rbar\xE2\x80\xA9" "and hello again");
    writeFile(root / "sub" / "deeper" / "c.md", "\xC3\xA9\xC3\xA9hello\n");
    writeFile(root / "ansi.log", "\x1b[32m  hello green\x1b[0m\n");
    writeFile(root / "nul.txt", std::string("hello\0there", 11));
    writeFile(root / "build.txt", "hello from a file named like a skipped folder\n");
    writeFile(root / "bom.txt", "\xEF\xBB\xBFhello bom\n");
    writeFile(root / ".git" / "config", "hello git\n");
    for (const char *d : {"node_modules", "build", "dist", "__pycache__", "venv",
                          "DerivedData"})
        writeFile(root / d / "x.txt", "hello skipped\n");
    writeFile(root / "binary.dat", std::string("hello\xFF\xFE\x00\x01", 9));
    writeFile(root / "latin1.txt", "hello caf\xE9\n");            // not UTF-8
    writeFile(root / "big.txt", "hello\n" + std::string(1024 * 1024, 'x'));
    writeFile(root / "exactly1mb.txt",
              "hello\n" + std::string(1024 * 1024 - 6, 'y'));     // at the limit: searched
    writeFile(root / "long.txt", std::string(50, ' ') + "hello" + std::string(400, 'z'));
    writeFile(outside / "linked.txt", "hello through a symlink\n");

    std::error_code ec;
    fsys::create_directory_symlink(root, root / "sub" / "loop", ec);        // back to root
    const bool loopMade = !ec;
    fsys::create_directory_symlink(outside, root / "zlink", ec);           // out of the tree
    const bool linkMade = !ec;
    fsys::create_symlink(root / "missing", root / "broken", ec);            // dangling
#ifndef _WIN32
    const bool fifoMade = mkfifo((root / "pipe.txt").c_str(), 0600) == 0;  // must not block
#else
    const bool fifoMade = false;
#endif

    GROUP("folder-search:tree");
    FolderSearchResult r = search(root.string(), "hello");
    CHECK(!r.cancelled);
    CHECK(!r.truncated);
    CHECK(hasHit(r, "a.txt:1:1"));
    CHECK(hasHit(r, "a.txt:3:10"));                      // case-insensitive, raw column
    CHECK(hasHit(r, "sub/b.cpp:1:11"));
    CHECK(hasHit(r, "sub/b.cpp:4:5"));                   // \r\n, \r and U+2029 end lines
    CHECK(hasHit(r, "sub/deeper/c.md:1:3"));             // column in characters
    CHECK(hasHit(r, "ansi.log:1:8"));                    // the raw line holds the escape
    CHECK(hasHit(r, "nul.txt:1:1"));
    CHECK(hasHit(r, "build.txt:1:1"));                   // only folders are skipped
    CHECK(hasHit(r, "bom.txt:1:2"));                     // the BOM is a character in line 1
    CHECK(hasHit(r, "exactly1mb.txt:1:1"));
    CHECK(hasHit(r, "long.txt:1:51"));
    CHECK(!anyHitIn(r, ".git"));
    CHECK(!anyHitIn(r, "node_modules"));
    CHECK(!anyHitIn(r, "build/"));
    CHECK(!anyHitIn(r, "dist"));
    CHECK(!anyHitIn(r, "__pycache__"));
    CHECK(!anyHitIn(r, "venv"));
    CHECK(!anyHitIn(r, "DerivedData"));
    CHECK(!anyHitIn(r, "binary.dat"));
    CHECK(!anyHitIn(r, "latin1.txt"));
    CHECK(!anyHitIn(r, "big.txt"));
    CHECK(!anyHitIn(r, "sub/loop"));                     // the loop back to root is not re-walked
    CHECK(!anyHitIn(r, "broken"));
    CHECK(!anyHitIn(r, "pipe.txt"));
    CHECK(loopMade);
    CHECK(fifoMade);
    if (linkMade) CHECK(hasHit(r, "zlink/linked.txt:1:1"));   // symlinked folders are followed
    CHECK(r.matches.size() == (linkMade ? 12u : 11u));
    CHECK(r.filesMatched == r.matches.size() - 2);       // a.txt and b.cpp have two
    CHECK(r.filesSearched >= r.filesMatched);

    // Stable order: files before folders, names in byte order, lines ascending.
    {
        auto keys = hitKeys(r);
        CHECK(!keys.empty() && keys.front() == "a.txt:1:1");
        auto pos = [&](const std::string &k) {
            return std::find(keys.begin(), keys.end(), k) - keys.begin();
        };
        CHECK(pos("a.txt:1:1") < pos("a.txt:3:10"));
        CHECK(pos("long.txt:1:51") < pos("sub/b.cpp:1:11"));    // files first
        CHECK(pos("sub/b.cpp:4:5") < pos("sub/deeper/c.md:1:3"));
    }

    // Match details: full path, byte offsets and the cleaned display text.
    for (const auto &m : r.matches) {
        if (m.relativePath == "ansi.log") {
            CHECK(m.text == "hello green");
            CHECK(m.byteColumn == 7 && m.byteLength == 5);
            CHECK(m.path == (root / "ansi.log").string());
        }
        if (m.relativePath == "sub/deeper/c.md") CHECK(m.byteColumn == 4);
        if (m.relativePath == "long.txt") {
            CHECK(m.text.size() == 200);
            CHECK(m.text.compare(0, 5, "hello") == 0);          // trimmed first
        }
        if (m.relativePath == "bom.txt") CHECK(m.text == "hello bom");
        if (m.relativePath == "nul.txt") CHECK(m.text == "hellothere");
    }

    GROUP("folder-search:options");
    CHECK(search(root.string(), "h").matches.empty());       // under two characters
    CHECK(search(root.string(), "   ").matches.empty());
    CHECK(search((root / "nope").string(), "hello").matches.empty());
    CHECK(search((root / "a.txt").string(), "hello").matches.empty());   // not a folder
    CHECK(search(root.string(), "  second hello ").matches.size() == 1); // trimmed query
    {
        FolderSearchOptions o;
        o.caseSensitive = true;
        auto cs = search(root.string(), "HELLO", nullptr, o);
        CHECK(cs.matches.size() == 1 && hasHit(cs, "a.txt:3:10"));
    }
    {
        FolderSearchOptions o;
        o.maxMatches = 3;
        auto capped = search(root.string(), "hello", nullptr, o);
        CHECK(capped.matches.size() == 3);
        CHECK(capped.truncated);
        CHECK(hitKeys(capped).front() == "a.txt:1:1");
    }
    {
        // The scope folder itself is searched even when its name is on the
        // skip list, as on the Mac.
        auto inBuild = search((root / "build").string(), "hello");
        CHECK(inBuild.matches.size() == 1 && inBuild.matches[0].relativePath == "x.txt");
        // And a root given with a trailing slash joins paths cleanly.
        auto slashed = search((root / "sub").string() + "/", "again");
        CHECK(slashed.matches.size() == 1 &&
              slashed.matches[0].path == (root / "sub" / "b.cpp").string());
    }

    GROUP("folder-search:cancel");
    {
        std::atomic<bool> stopNow{true};
        auto c = search(root.string(), "hello", &stopNow);
        CHECK(c.cancelled);
        CHECK(c.matches.empty());
        std::atomic<bool> keepGoing{false};
        auto k = search(root.string(), "hello", &keepGoing);
        CHECK(!k.cancelled && k.matches.size() == r.matches.size());
    }
    {
        // Cancelled from another thread while a large tree is being searched:
        // it stops early and says so.
        const fsys::path many = root / "many";
        for (int d = 0; d < 40; ++d)
            for (int f = 0; f < 25; ++f)
                writeFile(many / ("d" + std::to_string(d)) / ("f" + std::to_string(f) + ".txt"),
                          std::string(20000, 'q') + "\nneedle\n");
        std::atomic<bool> flag{false};
        std::thread t([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            flag = true;
        });
        auto c = search(many.string(), "needle", &flag);
        t.join();
        CHECK(c.cancelled);
        CHECK(c.matches.size() < 1000);
        auto whole = search(many.string(), "needle");
        CHECK(whole.matches.size() == 1000 && !whole.cancelled);
    }

    fsys::remove_all(root, ec);
    fsys::remove_all(outside, ec);
}

}  // namespace

// ---- Git status and diff -----------------------------------------------------
// Records as `git status --porcelain=v2 -z` writes them: each one ends in NUL.
static std::string nulRecords(std::initializer_list<std::string> recs) {
    std::string out;
    for (const std::string& r : recs) { out += r; out += '\0'; }
    return out;
}

static void testGitStatus() {
    using namespace Git;
    const std::string h40 = std::string(40, 'a'), z40 = std::string(40, '0');
    const std::string modes = " 100644 100644 100644 ";

    GROUP("git:status-empty-repo");
    {
        // A fresh `git init` with two untracked files, one with a space and
        // one with a non-ASCII name (-z never quotes paths).
        Status s = parseStatus(nulRecords({
            "# branch.oid (initial)", "# branch.head main",
            "? caf\xc3\xa9.txt", "? old name.txt"}));
        CHECK(s.initial && s.oid.empty());
        CHECK(s.branch == "main" && !s.detached);
        CHECK(s.upstream.empty() && !s.hasAheadBehind);
        CHECK(s.entries.size() == 2);
        CHECK(s.entries[0].untracked && s.entries[0].path == "caf\xc3\xa9.txt");
        CHECK(s.entries[1].path == "old name.txt");
        CHECK(!isStaged(s.entries[0]) && hasUnstaged(s.entries[0]));
        CHECK(unstagedLetter(s.entries[0]) == 'U');
        CHECK(branchLabel(s) == "main");
        // Nothing at all: no records.
        Status none = parseStatus("");
        CHECK(none.entries.empty() && none.branch.empty() && !none.initial);
    }

    GROUP("git:status-ordinary");
    {
        Status s = parseStatus(nulRecords({
            "# branch.oid " + h40, "# branch.head feature/x",
            "# branch.upstream origin/feature/x", "# branch.ab +2 -1",
            "1 .M N..." + modes + h40 + " " + h40 + " src/main.cpp",
            "1 M. N..." + modes + h40 + " " + h40 + " dir with spaces/a b.txt",
            "1 MM N..." + modes + h40 + " " + h40 + " both.txt",
            "1 A. N... 000000 100644 100644 " + z40 + " " + h40 + " added.txt",
            "1 D. N... 100644 000000 000000 " + h40 + " " + z40 + " gone.txt",
            "1 .D N..." + modes + h40 + " " + h40 + " deleted-here.txt",
            "1 .M SC.. 160000 160000 160000 " + h40 + " " + h40 + " vendor/lib",
            "! build/out.o"}));
        CHECK(s.oid == h40 && !s.initial);
        CHECK(s.branch == "feature/x");
        CHECK(s.upstream == "origin/feature/x");
        CHECK(s.hasAheadBehind && s.ahead == 2 && s.behind == 1);
        CHECK(branchLabel(s) == "feature/x \xe2\x86\x91" "2 \xe2\x86\x93" "1");
        CHECK(s.entries.size() == 7);   // the ignored file is left out
        const Entry& m = s.entries[0];
        CHECK(m.path == "src/main.cpp" && m.staged == '.' && m.unstaged == 'M');
        CHECK(!isStaged(m) && hasUnstaged(m) && unstagedLetter(m) == 'M');
        const Entry& sp = s.entries[1];
        CHECK(sp.path == "dir with spaces/a b.txt");
        CHECK(isStaged(sp) && !hasUnstaged(sp) && stagedLetter(sp) == 'M');
        const Entry& both = s.entries[2];
        CHECK(isStaged(both) && hasUnstaged(both));
        CHECK(stagedLetter(s.entries[3]) == 'A' && isStaged(s.entries[3]));
        CHECK(stagedLetter(s.entries[4]) == 'D' && !hasUnstaged(s.entries[4]));
        CHECK(unstagedLetter(s.entries[5]) == 'D' && !isStaged(s.entries[5]));
        CHECK(s.entries[6].submodule && s.entries[6].path == "vendor/lib");
        CHECK(!s.entries[0].submodule);
    }

    GROUP("git:status-ahead-behind");
    {
        Status s = parseStatus(nulRecords({"# branch.oid " + h40,
            "# branch.head main", "# branch.upstream origin/main", "# branch.ab +0 -0"}));
        CHECK(s.hasAheadBehind && s.ahead == 0 && s.behind == 0);
        CHECK(branchLabel(s) == "main");   // level with upstream: nothing added
        Status b = parseStatus(nulRecords({"# branch.head main", "# branch.ab +0 -12"}));
        CHECK(branchLabel(b) == "main \xe2\x86\x93" "12");
        Status bad = parseStatus(nulRecords({"# branch.head main", "# branch.ab junk"}));
        CHECK(!bad.hasAheadBehind);
    }

    GROUP("git:status-rename");
    {
        // A rename's original path is the record after it, with -z.
        Status s = parseStatus(nulRecords({
            "# branch.oid " + h40, "# branch.head main",
            "2 R. N..." + modes + h40 + " " + h40 + " R100 new name.txt",
            "old name.txt",
            "2 RM N..." + modes + h40 + " " + h40 + " R087 docs/caf\xc3\xa9.md",
            "docs/cafe.md",
            "2 C. N..." + modes + h40 + " " + h40 + " C75 copy.txt", "orig.txt",
            "? after.txt"}));
        CHECK(s.entries.size() == 4);
        CHECK(s.entries[0].path == "new name.txt");
        CHECK(s.entries[0].origPath == "old name.txt");
        CHECK(stagedLetter(s.entries[0]) == 'R' && !hasUnstaged(s.entries[0]));
        CHECK(s.entries[1].path == "docs/caf\xc3\xa9.md" &&
              s.entries[1].origPath == "docs/cafe.md");
        CHECK(isStaged(s.entries[1]) && unstagedLetter(s.entries[1]) == 'M');
        CHECK(stagedLetter(s.entries[2]) == 'C' && s.entries[2].origPath == "orig.txt");
        // The original path was not taken for a record of its own.
        CHECK(s.entries[3].untracked && s.entries[3].path == "after.txt");
    }

    GROUP("git:status-unmerged");
    {
        Status s = parseStatus(nulRecords({
            "# branch.oid " + h40, "# branch.head main",
            "u UU N... 100644 100644 100644 100644 " + h40 + " " + h40 + " " + h40 +
                " conflict file.txt",
            "u AA N... 000000 100644 100644 100644 " + z40 + " " + h40 + " " + h40 +
                " both-added.txt"}));
        CHECK(s.entries.size() == 2);
        const Entry& c = s.entries[0];
        CHECK(c.unmerged && c.path == "conflict file.txt");
        CHECK(!isStaged(c) && hasUnstaged(c));
        CHECK(unstagedLetter(c) == 'C');
        CHECK(s.entries[1].unmerged && s.entries[1].staged == 'A');
    }

    GROUP("git:status-detached");
    {
        Status s = parseStatus(nulRecords({
            "# branch.oid 1a2b3c4d5e6f" + std::string(28, '0'),
            "# branch.head (detached)"}));
        CHECK(s.detached && s.branch.empty());
        CHECK(branchLabel(s) == "HEAD (detached at 1a2b3c4)");
        Status s2 = parseStatus(nulRecords({"# branch.head (detached)"}));
        CHECK(branchLabel(s2) == "HEAD (detached)");
    }

    GROUP("git:status-robust");
    {
        // Short, unknown and truncated records are skipped, and output that
        // lacks the final NUL still gives its last record.
        std::string out = nulRecords({"", "x", "7 something new", "1 .M N... short",
                                      "# branch.head main"});
        out += "? no-terminator.txt";
        Status s = parseStatus(out);
        CHECK(s.branch == "main");
        CHECK(s.entries.size() == 1 && s.entries[0].path == "no-terminator.txt");
        // A rename at the very end with its original path missing.
        Status r = parseStatus(nulRecords({"2 R. N..." + modes + h40 + " " + h40 +
                                           " R100 new.txt"}));
        CHECK(r.entries.size() == 1 && r.entries[0].origPath.empty());
    }

    GROUP("git:time-limits");
    using V = std::vector<std::string>;
    // What the Mac's and the GTK port's panels run, and their limits.
    CHECK(subcommand({"-c", "core.quotePath=false", "diff", "--no-color"}) == "diff");
    CHECK(subcommand({"-C", "/x", "status"}) == "status");
    CHECK(subcommand({"--literal-pathspecs", "add", "--", "a"}) == "add");
    CHECK(subcommand({"-c"}).empty() && subcommand({}).empty());
    CHECK(timeoutFor(V{"status", "--porcelain=v2", "--branch", "-z"}) == 30);
    CHECK(timeoutFor(V{"-c", "log.showSignature=false", "log", "-z"}) == 30);
    CHECK(timeoutFor(V{"rev-parse", "--show-toplevel"}) == 30);
    CHECK(timeoutFor(V{"--literal-pathspecs", "add", "--", "a.txt"}) == 120);
    CHECK(timeoutFor(V{"--literal-pathspecs", "restore", "--staged", "--", "a"}) == 120);
    CHECK(timeoutFor(V{"--literal-pathspecs", "rm", "--cached", "--force", "--", "a"}) == 120);
    CHECK(timeoutFor(V{"commit", "-m", "x"}) == 600);
    CHECK(timeoutFor(V{"push"}) == 600 && timeoutFor(V{"push", "--set-upstream", "o", "b"}) == 600);
    CHECK(timeoutFor(V{"pull"}) == 300 && timeoutFor(V{"fetch", "origin"}) == 120);
    CHECK(talksToRemote(V{"fetch", "origin"}) && talksToRemote(V{"pull"}) &&
          talksToRemote(V{"push"}) && talksToRemote(V{"-c", "x=y", "ls-remote"}));
    CHECK(!talksToRemote(V{"status"}) && !talksToRemote(V{"remote"}) &&
          !talksToRemote(V{"-c", "push", "status"}));
    // A -c value that reads like a subcommand is skipped.
    CHECK(timeoutFor(V{"-c", "commit", "status"}) == 30);
    CHECK(durationText(30) == "30 seconds");
    CHECK(durationText(120) == "2 minutes");
    CHECK(durationText(600) == "10 minutes");
    CHECK(durationText(1) == "1 second");
    CHECK(durationText(0.5) == "0.5 seconds");
    CHECK(durationText(90) == "90 seconds");
    CHECK(timeoutMessage(V{"status", "--porcelain=v2", "--branch", "-z",
                           "--untracked-files=all"}, 30) ==
          "git did not answer within 30 seconds: git status --porcelain=v2 --branch -z "
          "--untracked-files=all");
    CHECK(heldOutputMessage(V{"add", "--", "a"}, 120) ==
          "git exited, but something it started kept its output open for 2 minutes: "
          "git add -- a");
    // A long commit message: one line, cut to 160 characters with "…".
    std::string msg = "Subject\n\nBody " + std::string(300, 'x');
    std::string line = commandLine(V{"commit", "-m", msg});
    CHECK(line.find('\n') == std::string::npos);
    CHECK(line.size() == 159 + 3 && line.substr(159) == "\xE2\x80\xA6");
    CHECK(line.rfind("git commit -m Subject  Body xx", 0) == 0);
    // Cut by characters, never inside one.
    std::string wide;
    for (int i = 0; i < 200; i++) wide += "\xC3\xA9";   // é
    std::string cut = commandLine(V{"commit", "-m", wide});
    CHECK(cut.size() == 14 + 145 * 2 + 3);   // "git commit -m " then 145 é
    std::string exact = commandLine(V{std::string(156, 'a')});   // 160 in all
    CHECK(exact.size() == 160 && exact.find("\xE2\x80\xA6") == std::string::npos);
}

static void testGitDiff() {
    using namespace Git;

    GROUP("git:unquote");
    CHECK(unquotePath("plain name.txt") == "plain name.txt");
    CHECK(unquotePath("\"a/caf\\303\\251.txt\"") == "a/caf\xc3\xa9.txt");
    CHECK(unquotePath("\"tab\\there\"") == "tab\there");
    CHECK(unquotePath("\"q\\\"uote\\\\s\"") == "q\"uote\\s");
    CHECK(unquotePath("\"") == "\"");
    CHECK(unquotePath("\"\"") == "");

    GROUP("git:diff-hunks");
    const std::string diff =
        "diff --git a/src/app.c b/src/app.c\n"
        "index 83db48f..bf269f4 100644\n"
        "--- a/src/app.c\n"
        "+++ b/src/app.c\n"
        "@@ -1,4 +1,5 @@ int main(void)\n"
        " #include <stdio.h>\n"
        "-int x;\n"
        "+int x = 1;\n"
        "+int y;\n"
        " \n"
        " int z;\n"
        "@@ -20,3 +21,2 @@\n"
        " a\n"
        "--- this line was removed, it is not a header\n"
        " b\n"
        "diff --git \"a/caf\\303\\251.txt\" \"b/caf\\303\\251.txt\"\n"
        "index c1b0730..722ed8c 100644\n"
        "--- \"a/caf\\303\\251.txt\"\n"
        "+++ \"b/caf\\303\\251.txt\"\n"
        "@@ -1 +1 @@\n"
        "-x\n"
        "\\ No newline at end of file\n"
        "+xd\n";
    {
        std::vector<DiffLine> ls = classifyDiff(diff);
        CHECK(ls.size() == 23);
        CHECK(ls[0].kind == LineKind::FileHeader);
        CHECK(ls[3].kind == LineKind::FileHeader);   // +++ before any hunk
        CHECK(ls[4].kind == LineKind::HunkHeader);
        CHECK(ls[5].kind == LineKind::Context && ls[5].oldLine == 1 && ls[5].newLine == 1);
        CHECK(ls[6].kind == LineKind::Removed && ls[6].oldLine == 2 && ls[6].newLine == 0);
        CHECK(ls[7].kind == LineKind::Added && ls[7].newLine == 2 && ls[7].oldLine == 0);
        CHECK(ls[8].kind == LineKind::Added && ls[8].newLine == 3);
        CHECK(ls[9].kind == LineKind::Context && ls[9].text == " ");
        CHECK(ls[10].kind == LineKind::Context && ls[10].oldLine == 4 && ls[10].newLine == 5);
        CHECK(ls[11].kind == LineKind::HunkHeader);
        CHECK(ls[12].kind == LineKind::Context && ls[12].oldLine == 20 && ls[12].newLine == 21);
        // Inside a hunk, "--- ..." is a removed line.
        CHECK(ls[13].kind == LineKind::Removed && ls[13].oldLine == 21);
        CHECK(ls[14].kind == LineKind::Context && ls[14].newLine == 22);
        CHECK(ls[15].kind == LineKind::FileHeader);
        CHECK(ls[17].kind == LineKind::FileHeader && ls[18].kind == LineKind::FileHeader);
        CHECK(ls[19].kind == LineKind::HunkHeader);
        CHECK(ls[20].kind == LineKind::Removed);
        CHECK(ls[21].kind == LineKind::NoNewline);
        CHECK(ls[21].text == "\\ No newline at end of file");
        CHECK(ls[22].kind == LineKind::Added && ls[22].text == "+xd");
    }
    {
        std::vector<FileDiff> fs = parseDiff(diff);
        CHECK(fs.size() == 2);
        CHECK(fs[0].oldPath == "src/app.c" && fs[0].newPath == "src/app.c");
        CHECK(fs[0].header.size() == 4 && fs[0].hunks.size() == 2);
        const Hunk& h = fs[0].hunks[0];
        CHECK(h.oldStart == 1 && h.oldCount == 4 && h.newStart == 1 && h.newCount == 5);
        CHECK(h.section == "int main(void)");
        CHECK(h.lines.size() == 7 && h.lines[0].kind == LineKind::HunkHeader);
        const Hunk& h2 = fs[0].hunks[1];
        CHECK(h2.oldStart == 20 && h2.oldCount == 3 && h2.newStart == 21 && h2.newCount == 2);
        CHECK(h2.section.empty() && h2.lines.size() == 4);
        CHECK(fs[1].oldPath == "caf\xc3\xa9.txt" && fs[1].newPath == "caf\xc3\xa9.txt");
        CHECK(fs[1].hunks.size() == 1);
        // "@@ -1 +1 @@": a count left out is 1.
        CHECK(fs[1].hunks[0].oldCount == 1 && fs[1].hunks[0].newCount == 1);
        CHECK(fs[1].hunks[0].lines.size() == 4);   // header, -, \, +
        CHECK(fs[1].hunks[0].lines[2].kind == LineKind::NoNewline);
        CHECK(fs[1].hunks[0].lines[3].kind == LineKind::Added);
    }

    GROUP("git:diff-new-deleted-binary");
    {
        const std::string d =
            "diff --git a/new file.txt b/new file.txt\n"
            "new file mode 100644\n"
            "index 0000000..3b18e51\n"
            "--- /dev/null\n"
            "+++ b/new file.txt\n"
            "@@ -0,0 +1,2 @@\n"
            "+hello\n"
            "+world\n"
            "diff --git a/old.txt b/old.txt\n"
            "deleted file mode 100644\n"
            "index 3b18e51..0000000\n"
            "--- a/old.txt\n"
            "+++ /dev/null\n"
            "@@ -1 +0,0 @@\n"
            "-bye\n"
            "diff --git a/logo.png b/logo.png\n"
            "index 1111111..2222222 100644\n"
            "Binary files a/logo.png and b/logo.png differ\n";
        std::vector<FileDiff> fs = parseDiff(d);
        CHECK(fs.size() == 3);
        CHECK(fs[0].isNew && !fs[0].isDeleted);
        CHECK(fs[0].oldPath.empty() && fs[0].newPath == "new file.txt");
        CHECK(fs[0].hunks.size() == 1 && fs[0].hunks[0].newCount == 2);
        CHECK(fs[0].hunks[0].lines[1].newLine == 1 && fs[0].hunks[0].lines[2].newLine == 2);
        CHECK(fs[1].isDeleted && fs[1].oldPath == "old.txt" && fs[1].newPath.empty());
        CHECK(fs[1].hunks[0].lines.size() == 2);
        CHECK(fs[2].binary && fs[2].hunks.empty() && fs[2].newPath == "logo.png");
        std::vector<DiffLine> ls = classifyDiff(d);
        CHECK(ls.back().kind == LineKind::Other);
        CHECK(ls[8].kind == LineKind::FileHeader);   // next file's diff line
    }

    GROUP("git:diff-rename");
    {
        const std::string d =
            "diff --git a/old name.txt b/new name.txt\n"
            "similarity index 100%\n"
            "rename from old name.txt\n"
            "rename to new name.txt\n"
            "diff --git a/a.txt b/b.txt\n"
            "similarity index 80%\n"
            "rename from a.txt\n"
            "rename to b.txt\n"
            "index 1..2 100644\n"
            "--- a/a.txt\n"
            "+++ b/b.txt\n"
            "@@ -1,2 +1,2 @@\n"
            " same\n"
            "-old\n"
            "+new\n";
        std::vector<FileDiff> fs = parseDiff(d);
        CHECK(fs.size() == 2);
        CHECK(fs[0].isRename && fs[0].oldPath == "old name.txt" &&
              fs[0].newPath == "new name.txt");
        CHECK(fs[0].hunks.empty() && fs[0].header.size() == 4);
        CHECK(fs[1].isRename && fs[1].oldPath == "a.txt" && fs[1].newPath == "b.txt");
        CHECK(fs[1].hunks.size() == 1 && fs[1].hunks[0].lines.size() == 4);
    }

    GROUP("git:diff-combined");
    {
        // A conflict, as `git diff` shows it during a merge.
        const std::string d =
            "diff --cc f\n"
            "index 351be5b,e45c9c2..0000000\n"
            "--- a/f\n"
            "+++ b/f\n"
            "@@@ -1,1 -1,1 +1,5 @@@\n"
            "++<<<<<<< HEAD\n"
            " +mine\n"
            "++=======\n"
            "+ other\n"
            "++>>>>>>> other\n";
        std::vector<DiffLine> ls = classifyDiff(d);
        CHECK(ls.size() == 10);
        CHECK(ls[4].kind == LineKind::HunkHeader);
        for (size_t i = 5; i < 10; i++) CHECK(ls[i].kind == LineKind::Added);
        CHECK(ls[5].newLine == 1 && ls[9].newLine == 5);
        CHECK(ls[6].oldLine == 1);   // "mine" is the first parent's line 1
        std::vector<FileDiff> fs = parseDiff(d);
        CHECK(fs.size() == 1 && fs[0].newPath == "f" && fs[0].oldPath == "f");
        CHECK(fs[0].hunks.size() == 1 && fs[0].hunks[0].newCount == 5);
        CHECK(fs[0].hunks[0].lines.size() == 6);
        // A combined removal.
        std::vector<DiffLine> r = classifyDiff(
            "diff --cc g\n@@@ -1,2 -1,1 +1,1 @@@\n- gone\n  kept\n");
        CHECK(r.size() == 4 && r[2].kind == LineKind::Removed && r[3].kind == LineKind::Context);
    }

    GROUP("git:diff-edges");
    {
        CHECK(classifyDiff("").empty());
        CHECK(parseDiff("").empty());
        // Text outside any file (a warning git printed) stays Other, and an
        // "@@" there is not a hunk.
        std::vector<DiffLine> ls = classifyDiff("warning: something\n@@ -1 +1 @@\n");
        CHECK(ls.size() == 2 && ls[0].kind == LineKind::Other && ls[1].kind == LineKind::Other);
        CHECK(parseDiff("warning: something\n").empty());
        // No final newline on the last line.
        ls = classifyDiff("diff --git a/x b/x\n@@ -1 +1 @@\n-a\n+b");
        CHECK(ls.size() == 4 && ls[3].kind == LineKind::Added && ls[3].text == "+b");
        // A malformed hunk header is not a hunk.
        ls = classifyDiff("diff --git a/x b/x\n@@ nonsense @@\n+a\n");
        CHECK(ls[1].kind == LineKind::FileHeader && ls[2].kind == LineKind::FileHeader);
        // A hunk shorter than its header says ends at the next file.
        ls = classifyDiff("diff --git a/x b/x\n@@ -1,5 +1,5 @@\n a\n"
                          "diff --git a/y b/y\n@@ -1 +1 @@\n-p\n+q\n");
        CHECK(ls[3].kind == LineKind::FileHeader && ls[4].kind == LineKind::HunkHeader);
        CHECK(ls[5].kind == LineKind::Removed && ls[6].kind == LineKind::Added);
        std::vector<FileDiff> fs = parseDiff("diff --git a/x b/x\n@@ -1,5 +1,5 @@\n a\n"
                                             "diff --git a/y b/y\n@@ -1 +1 @@\n-p\n+q\n");
        CHECK(fs.size() == 2 && fs[1].newPath == "y");
        // A header whose two names differ and hold " b/" is split at " b/".
        fs = parseDiff("diff --git a/one b/two\n");
        CHECK(fs.size() == 1 && fs[0].oldPath == "one" && fs[0].newPath == "two");
        // Git adds a tab after a name with a space in ---/+++ lines.
        fs = parseDiff("diff --git a/a b b/a b\n--- a/a b\t\n+++ b/a b\t\n");
        CHECK(fs[0].oldPath == "a b" && fs[0].newPath == "a b");
    }
}

// ---- Git commit graph --------------------------------------------------------
namespace {

Git::Commit gc(const std::string& hash, std::vector<std::string> parents) {
    Git::Commit c;
    c.hash = hash;
    c.parents = std::move(parents);
    c.subject = hash;
    return c;
}

// A row's lines, sorted, as "I1>0" (in from lane 1 to the dot at 0), "P2>1"
// (passing from 2 at the top to 1 at the bottom), "O0>1" (out to lane 1).
std::string edgeText(const Git::GraphRow& r) {
    std::vector<std::string> v;
    for (const Git::GraphEdge& e : r.edges) {
        const char* k = e.kind == Git::GraphEdge::In ? "I"
                        : e.kind == Git::GraphEdge::Pass ? "P" : "O";
        v.push_back(k + std::to_string(e.from) + ">" + std::to_string(e.to));
    }
    std::sort(v.begin(), v.end());
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : " ") + x;
    return s;
}

int edgeColor(const Git::GraphRow& r, Git::GraphEdge::Kind k, int from, int to) {
    for (const Git::GraphEdge& e : r.edges)
        if (e.kind == k && e.from == from && e.to == to) return e.color;
    return -1;
}

// Lines leaving a row at the bottom must be exactly the lines entering the
// next row at the top, in the same lanes and colors, and lanes must be packed
// from 0 at both edges. Returns false (and says where) when not.
bool graphConsistent(const std::vector<Git::GraphRow>& rows) {
    for (size_t i = 0; i < rows.size(); i++) {
        std::map<int, int> bottom, top;
        for (const Git::GraphEdge& e : rows[i].edges) {
            if (e.kind == Git::GraphEdge::In) continue;
            if (bottom.count(e.to) && bottom[e.to] != e.color) {
                std::printf("  row %zu: two colors leave lane %d\n", i, e.to);
                return false;
            }
            bottom[e.to] = e.color;
        }
        int n = 0;
        for (auto& kv : bottom)
            if (kv.first != n++) { std::printf("  row %zu: bottom not packed\n", i); return false; }
        if (i + 1 == rows.size()) break;
        for (const Git::GraphEdge& e : rows[i + 1].edges) {
            if (e.kind == Git::GraphEdge::Out) continue;
            if (top.count(e.from) && e.kind == Git::GraphEdge::Pass) {
                std::printf("  row %zu: lane %d enters twice\n", i + 1, e.from);
                return false;
            }
            top[e.from] = e.color;
        }
        if (top != bottom) {
            std::printf("  rows %zu/%zu: lanes differ at the seam\n", i, i + 1);
            return false;
        }
        const Git::GraphRow& r = rows[i + 1];
        if (r.lane < 0 || r.lane >= r.width) return false;
    }
    return true;
}

}  // namespace

static void testGitGraph() {
    using namespace Git;

    GROUP("git:log-parse");
    {
        // Bytes as `git log -z --topo-order --format=<logFormat()>` wrote them
        // (checked against git 2.54): fields NUL-separated, each commit ended
        // by a NUL, a root commit's parents empty.
        std::string h1(40, '1'), h2(40, '2'), h3(40, '3');
        std::string out = h3 + std::string("\0", 1) + h2 + " " + h1 + std::string("\0", 1) +
                          "Eric" + std::string("\0", 1) + "1790418642" +
                          std::string("\0", 1) + "Merge branch 'side'" +
                          std::string("\0", 1) + h1 + std::string("\0\0", 2) +
                          "T" + std::string("\0", 1) + "1790418600" +
                          std::string("\0", 1) + "first one" + std::string("\0", 1);
        std::vector<Commit> cs = parseLog(out);
        CHECK(cs.size() == 2);
        CHECK(cs[0].hash == h3 && cs[0].parents.size() == 2 && cs[0].parents[1] == h1);
        CHECK(cs[0].author == "Eric" && cs[0].time == 1790418642);
        CHECK(cs[0].subject == "Merge branch 'side'");
        CHECK(cs[1].parents.empty() && cs[1].subject == "first one");
        // An empty subject (--allow-empty-message) keeps the fields in step.
        out = h2 + std::string("\0", 1) + h1 + std::string("\0", 1) + "A" +
              std::string("\0", 1) + "5" + std::string("\0\0", 2) + h1 +
              std::string("\0\0", 2) + "B" + std::string("\0", 1) + "4" +
              std::string("\0", 1) + "root" + std::string("\0", 1);
        cs = parseLog(out);
        CHECK(cs.size() == 2 && cs[0].subject.empty() && cs[1].subject == "root");
        // Output cut off mid-record drops only that record.
        cs = parseLog(h2 + std::string("\0", 1) + h1 + std::string("\0", 1) + "A");
        CHECK(cs.empty());
        CHECK(parseLog("").empty());
    }

    GROUP("git:refs");
    {
        // `git for-each-ref --format=<refFormat()>` in a clone with an
        // annotated and a lightweight tag (git 2.54).
        auto line = [](const std::string& obj, const std::string& peeled,
                       const std::string& name, const std::string& sym) {
            return obj + std::string("\0", 1) + peeled + std::string("\0", 1) + name +
                   std::string("\0", 1) + sym + "\n";
        };
        std::string a(40, 'a'), b(40, 'b'), t(40, 't');
        std::string out = line(a, "", "refs/heads/main", "") +
                          line(b, "", "refs/heads/side", "") +
                          line(a, "", "refs/remotes/origin/HEAD", "refs/remotes/origin/main") +
                          line(b, "", "refs/remotes/origin/main", "") +
                          line(a, "", "refs/stash", "") +
                          line(a, "", "refs/tags/light", "") +
                          line(t, b, "refs/tags/v1", "");
        std::vector<Ref> refs = parseRefs(out);
        CHECK(refs.size() == 5);
        CHECK(refs[0].kind == RefKind::Branch && refs[0].name == "main" && refs[0].target == a);
        CHECK(refs[2].kind == RefKind::Remote && refs[2].name == "origin/main");
        CHECK(refs[4].kind == RefKind::Tag && refs[4].name == "v1" && refs[4].target == b);

        auto labels = labelsByCommit(refs, a, "main", false);
        CHECK(labels[a].size() == 2);
        CHECK(labels[a][0].name == "main" && labels[a][0].current);
        CHECK(labels[a][1].kind == RefKind::Tag && !labels[a][1].current);
        // On b: the local branch, then the remote one, then the tag.
        CHECK(labels[b].size() == 3);
        CHECK(labels[b][0].name == "side" && !labels[b][0].current);
        CHECK(labels[b][1].name == "origin/main" && labels[b][2].name == "v1");
        // Detached at b: a HEAD label first, and no branch is current.
        labels = labelsByCommit(refs, b, "", true);
        CHECK(labels[b].size() == 4 && labels[b][0].kind == RefKind::Head &&
              labels[b][0].current);
        CHECK(!labels[a][0].current);
        // Before the first commit there is nothing to label.
        CHECK(labelsByCommit({}, "", "main", false).empty());
    }

    GROUP("git:left-right");
    {
        // `git rev-list --left-right HEAD...@{u}` after one local commit and
        // one on the upstream.
        Divergence d = parseLeftRight("<" + std::string(40, 'a') + "\n>" +
                                      std::string(40, 'b') + "\n");
        CHECK(d.outgoing.size() == 1 && d.outgoing.count(std::string(40, 'a')));
        CHECK(d.incoming.size() == 1 && d.incoming.count(std::string(40, 'b')));
        CHECK(parseLeftRight("").outgoing.empty());
    }

    GROUP("git:graph-linear");
    {
        std::vector<GraphRow> g = layoutGraph({gc("c", {"b"}), gc("b", {"a"}), gc("a", {})});
        CHECK(g.size() == 3);
        CHECK(edgeText(g[0]) == "O0>0");
        CHECK(edgeText(g[1]) == "I0>0 O0>0");
        CHECK(edgeText(g[2]) == "I0>0");   // the root ends its lane
        for (const GraphRow& r : g) CHECK(r.lane == 0 && r.width == 1 && r.color == 0);
        CHECK(graphConsistent(g));
        CHECK(layoutGraph({}).empty());
    }

    GROUP("git:graph-branch-merge");
    {
        // git's own topo order for: A; B on main; S on side from A; M merging
        // side into main (M's parents B, S).
        std::vector<GraphRow> g = layoutGraph(
            {gc("M", {"B", "S"}), gc("S", {"A"}), gc("B", {"A"}), gc("A", {})});
        CHECK(edgeText(g[0]) == "O0>0 O0>1");
        CHECK(g[0].width == 2);
        CHECK(g[1].lane == 1 && edgeText(g[1]) == "I1>1 O1>1 P0>0");
        CHECK(g[2].lane == 0 && edgeText(g[2]) == "I0>0 O0>0 P1>1");
        // Both lanes meet at A, and the side lane's line keeps its color.
        CHECK(g[3].lane == 0 && edgeText(g[3]) == "I0>0 I1>0");
        CHECK(g[0].color == 0 && g[2].color == 0 && g[3].color == 0);
        CHECK(g[1].color != 0 && edgeColor(g[0], GraphEdge::Out, 0, 1) == g[1].color);
        CHECK(edgeColor(g[3], GraphEdge::In, 1, 0) == g[1].color);
        CHECK(g[3].width == 2);
        CHECK(graphConsistent(g));
    }

    GROUP("git:graph-octopus");
    {
        // `git merge A B C` from main at R: O's parents are A, B, C, and git
        // lists O C B A R.
        std::vector<GraphRow> g = layoutGraph({gc("O", {"A", "B", "C"}), gc("C", {"R"}),
                                               gc("B", {"R"}), gc("A", {"R"}), gc("R", {})});
        CHECK(edgeText(g[0]) == "O0>0 O0>1 O0>2" && g[0].width == 3);
        int c0 = edgeColor(g[0], GraphEdge::Out, 0, 0), c1 = edgeColor(g[0], GraphEdge::Out, 0, 1),
            c2 = edgeColor(g[0], GraphEdge::Out, 0, 2);
        CHECK(c0 != c1 && c1 != c2 && c0 != c2);
        CHECK(g[1].lane == 2 && g[1].color == c2);
        CHECK(edgeText(g[1]) == "I2>2 O2>2 P0>0 P1>1");
        CHECK(g[2].lane == 1 && edgeText(g[2]) == "I1>1 O1>1 P0>0 P2>2");
        CHECK(g[3].lane == 0 && edgeText(g[3]) == "I0>0 O0>0 P1>1 P2>2");
        CHECK(g[4].lane == 0 && edgeText(g[4]) == "I0>0 I1>0 I2>0");
        CHECK(graphConsistent(g));
    }

    GROUP("git:graph-several-tips");
    {
        // Two branches off X, neither merged: two tips, both meet at X.
        std::vector<GraphRow> g =
            layoutGraph({gc("T1", {"X"}), gc("T2", {"X"}), gc("X", {"W"}), gc("W", {})});
        CHECK(g[0].lane == 0 && edgeText(g[0]) == "O0>0");
        CHECK(g[1].lane == 1 && g[1].color != g[0].color);
        CHECK(edgeText(g[1]) == "O1>1 P0>0");   // a tip: no line in from above
        CHECK(edgeText(g[2]) == "I0>0 I1>0 O0>0" && g[2].width == 2);
        CHECK(edgeText(g[3]) == "I0>0" && g[3].width == 1);
        CHECK(graphConsistent(g));

        // Three lanes; the middle one merges into the left one's commit, and
        // the right lane moves left to close the gap in the same row.
        g = layoutGraph({gc("a", {"x"}), gc("b", {"x"}), gc("c", {"y"}), gc("x", {"y"}),
                         gc("y", {})});
        CHECK(edgeText(g[2]) == "O2>2 P0>0 P1>1");
        CHECK(edgeText(g[3]) == "I0>0 I1>0 O0>0 P2>1");
        CHECK(g[3].width == 3);
        CHECK(edgeColor(g[3], GraphEdge::Pass, 2, 1) == g[2].color);
        CHECK(edgeText(g[4]) == "I0>0 I1>0");
        CHECK(graphConsistent(g));
    }

    GROUP("git:graph-window");
    {
        // Parents past the loaded window keep their lines to the bottom.
        std::vector<GraphRow> g = layoutGraph({gc("M", {"p1", "p2"}), gc("c", {"p1"})});
        CHECK(edgeText(g[0]) == "O0>0 O0>1");
        CHECK(g[1].lane == 2 && edgeText(g[1]) == "O2>2 P0>0 P1>1");
        CHECK(graphConsistent(g));
        // A merge whose second parent is already some lane's: the line goes
        // into that lane rather than opening another.
        g = layoutGraph({gc("t", {"b"}), gc("M", {"a", "b"}), gc("b", {"a"}), gc("a", {})});
        CHECK(g[1].lane == 1 && edgeText(g[1]) == "O1>0 O1>1 P0>0");
        CHECK(graphConsistent(g));
    }

    GROUP("git:graph-roots");
    {
        // `git merge --allow-unrelated-histories`: git lists M B A, both roots.
        std::vector<GraphRow> g = layoutGraph({gc("M", {"A", "B"}), gc("B", {}), gc("A", {})});
        CHECK(edgeText(g[1]) == "I1>1 P0>0" && g[1].lane == 1);
        CHECK(edgeText(g[2]) == "I0>0" && g[2].width == 1);
        // A root in the left lane while a lane to its right carries on: that
        // lane moves left across the root's row.
        g = layoutGraph({gc("M", {"A", "B"}), gc("A", {}), gc("B", {})});
        CHECK(edgeText(g[1]) == "I0>0 P1>0" && g[1].width == 2);
        CHECK(g[2].lane == 0 && edgeText(g[2]) == "I0>0");
        CHECK(graphConsistent(g));
    }

    GROUP("git:graph-random");
    {
        // Random histories, created oldest first so reversing them gives a
        // topological order: the seams between rows must always match.
        std::mt19937 rng(7);
        bool ok = true;
        for (int round = 0; round < 200 && ok; round++) {
            int n = 1 + (int)(rng() % 60);
            std::vector<Commit> made;
            for (int i = 0; i < n; i++) {
                std::vector<std::string> ps;
                int np = i == 0 ? 0 : (int)(rng() % 10 < 1 ? 0 : rng() % 10 < 7 ? 1
                                            : rng() % 4 == 0 ? 3 : 2);
                for (int k = 0; k < np; k++) {
                    std::string p = "c" + std::to_string(rng() % i);
                    if (std::find(ps.begin(), ps.end(), p) == ps.end()) ps.push_back(p);
                }
                made.push_back(gc("c" + std::to_string(i), ps));
            }
            std::reverse(made.begin(), made.end());
            // Sometimes only a window of it, so parents are missing.
            if (rng() % 3 == 0) made.resize(1 + rng() % made.size());
            std::vector<GraphRow> g = layoutGraph(made);
            ok = g.size() == made.size() && graphConsistent(g);
            for (size_t i = 0; ok && i < made.size(); i++) {
                int outs = 0;
                for (const GraphEdge& e : g[i].edges) outs += e.kind == GraphEdge::Out;
                ok = outs == (int)made[i].parents.size();
            }
        }
        CHECK(ok);
    }

    GROUP("git:graph-speed");
    {
        // 100,000 commits with a merge every tenth: the layout is linear.
        std::vector<Commit> big;
        const int N = 100000;
        for (int i = N - 1; i >= 0; i--) {
            std::vector<std::string> ps;
            if (i > 0) ps.push_back("h" + std::to_string(i - 1));
            if (i > 10 && i % 10 == 0) ps.push_back("h" + std::to_string(i - 7));
            big.push_back(gc("h" + std::to_string(i), ps));
        }
        auto t0 = std::chrono::steady_clock::now();
        std::vector<GraphRow> g = layoutGraph(big);
        double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
        std::printf("  graph layout: %d commits in %.1f ms\n", N, ms);
        CHECK(g.size() == (size_t)N && graphConsistent(g));
    }

    GROUP("git:show-parse");
    {
        // `git show --format=<showFormat()> --stat --patch` (git 2.54): the
        // fields, a newline, "---" before the stat, then the diff.
        std::string z("\0", 1), h(40, 'a'), p(40, 'b');
        std::string out = h + z + p + z + "Eric" + z + "e@x" + z + "1790418654" + z + "Eric" +
                          z + "e@x" + z + "Subject line\n\nBody text.\n" + z +
                          "\n---\n a | 1 +\n 1 file changed, 1 insertion(+)\n\n"
                          "diff --git a/a b/a\n";
        CommitDetail d;
        CHECK(parseShow(out, d));
        CHECK(d.hash == h && d.parents.size() == 1 && d.parents[0] == p);
        CHECK(d.author == "Eric" && d.email == "e@x" && d.time == 1790418654);
        CHECK(d.committer.empty());
        CHECK(d.message == "Subject line\n\nBody text.");
        CHECK(d.patch.rfind(" a | 1 +", 0) == 0);
        CHECK(d.patch.find("diff --git a/a b/a\n") != std::string::npos);
        // A different committer is kept; a root merge-less commit has no
        // parents; a missing field fails.
        out = h + z + z + "A" + z + "a@x" + z + "5" + z + "C" + z + "c@x" + z + "m" + z + "\n";
        CHECK(parseShow(out, d) && d.parents.empty() && d.committer == "C <c@x>");
        CHECK(d.patch.empty());
        CHECK(!parseShow("fatal: bad object", d));
    }

    GROUP("git:relative-time");
    {
        const long long now = 1790418654, M = 60, H = 3600, D = 86400;
        CHECK(relativeTime(now, now) == "just now");
        CHECK(relativeTime(now + 500, now) == "just now");
        CHECK(relativeTime(now - 70, now) == "a minute ago");
        CHECK(relativeTime(now - 5 * M, now) == "5 minutes ago");
        CHECK(relativeTime(now - 70 * M, now) == "an hour ago");
        CHECK(relativeTime(now - 3 * H, now) == "3 hours ago");
        CHECK(relativeTime(now - 26 * H, now) == "yesterday");
        CHECK(relativeTime(now - 3 * D, now) == "3 days ago");
        CHECK(relativeTime(now - 8 * D, now) == "1 week ago");
        CHECK(relativeTime(now - 20 * D, now) == "3 weeks ago");
        CHECK(relativeTime(now - 65 * D, now) == "2 months ago");
        CHECK(relativeTime(now - 400 * D, now) == "1 year ago");
        CHECK(relativeTime(now - 3 * 365 * D, now) == "3 years ago");
        CHECK(shortHash(std::string(40, 'f')) == "fffffff" && shortHash("ab") == "ab");
    }
}

// ---------------------------------------------------------------- task lists

// `marked` uses '|' for a caret and '{' '}' for a selection; the result is
// written the same way, or "unchanged".
std::string taskEdit(const std::string &marked, bool isNewline) {
    std::u16string text;
    size_t a = std::string::npos, b = std::string::npos;
    for (char c : marked) {
        if (c == '|') { a = b = text.size(); continue; }
        if (c == '{') { a = text.size(); continue; }
        if (c == '}') { b = text.size(); continue; }
        text += (char16_t)(unsigned char)c;
    }
    MarkdownTasks::Edit e = isNewline ? MarkdownTasks::newline(text, a, b)
                                      : MarkdownTasks::toggle(text, a, b);
    if (!e.changed) return "unchanged";
    text.replace(e.replaceStart, e.replaceLength, e.replacement);
    std::string out = narrow(text);
    if (e.selStart == e.selEnd) return out.insert(e.selStart, "|");
    out.insert(e.selEnd, "}");
    return out.insert(e.selStart, "{");
}
std::string taskToggled(const std::string &m) { return taskEdit(m, false); }
// Toggle Bold or Italic on `marked`, written as taskEdit writes it.
std::string wrapped(const std::string &marked, const char16_t *marker) {
    std::u16string text;
    size_t a = std::string::npos, b = std::string::npos;
    for (char c : marked) {
        if (c == '|') { a = b = text.size(); continue; }
        if (c == '{') { a = text.size(); continue; }
        if (c == '}') { b = text.size(); continue; }
        text += (char16_t)(unsigned char)c;
    }
    MarkdownTasks::Edit e = MarkdownTasks::toggleWrap(text, a, b, marker);
    if (!e.changed) return "unchanged";
    text.replace(e.replaceStart, e.replaceLength, e.replacement);
    std::string out = narrow(text);
    if (e.selStart == e.selEnd) return out.insert(e.selStart, "|");
    out.insert(e.selEnd, "}");
    return out.insert(e.selStart, "{");
}
std::string taskReturn(const std::string &m) { return taskEdit(m, true); }

void testMarkdownTasks() {
    GROUP("tasks:toggle");
    CHECK(taskToggled("|buy milk") == "- [ ] |buy milk");
    CHECK(taskToggled("buy| milk") == "- [ ] buy| milk");
    CHECK(taskToggled("  indented|") == "  - [ ] indented|");
    CHECK(taskToggled("- buy|") == "- [ ] buy|");
    CHECK(taskToggled("* buy|") == "* [ ] buy|");
    CHECK(taskToggled("- [ ] buy|") == "- [x] buy|");
    CHECK(taskToggled("- [x] buy|") == "- [ ] buy|");
    CHECK(taskToggled("  * [X] nested|") == "  * [ ] nested|");
    CHECK(taskToggled("1. thing|") == "1. [ ] thing|");
    CHECK(taskToggled("> quoted|") == "> - [ ] quoted|");
    CHECK(taskToggled("> - [ ] quoted|") == "> - [x] quoted|");
    CHECK(taskToggled("|") == "- [ ] |");
    CHECK(taskToggled("a\n|\nb") == "a\n- [ ] |\nb");
    CHECK(taskToggled("-|") == "- [ ] |");
    CHECK(taskToggled("- [ ]|") == "- [x]|");
    CHECK(taskToggled("{a\n- [x] b}") == "{- [ ] a\n- [x] b}");
    CHECK(taskToggled("{- [ ] a\n- [x] b}") == "{- [x] a\n- [x] b}");
    CHECK(taskToggled("{- [x] a\n- [x] b}") == "{- [ ] a\n- [ ] b}");
    CHECK(taskToggled("{a\n\nb}") == "{- [ ] a\n\n- [ ] b}");
    // A selection ending at the start of a line leaves that line out.
    CHECK(taskToggled("{a\n}b") == "{- [ ] a\n}b");
    // A wrapped item's continuation line toggles its item.
    CHECK(taskToggled("- [ ] long\n  more|") == "- [x] long\n  more|");
    // A box written without a list marker gets the marker, not a second box,
    // and keeps its tick; then the next press checks as usual.
    CHECK(taskToggled("[ ] study|") == "- [ ] study|");
    CHECK(taskToggled("[x] done|") == "- [x] done|");
    CHECK(taskToggled("  [X] done|") == "  - [X] done|");
    CHECK(taskToggled("[ ]|") == "- [ ]|");
    CHECK(taskToggled("{[ ] a\n[x] b\nc}") == "{- [ ] a\n- [x] b\n- [ ] c}");
    CHECK(taskToggled("> [ ] quoted|") == "> - [ ] quoted|");
    // Only a real box: a link, "[]" and "[ ]x" are text.
    CHECK(taskToggled("[a] b|") == "- [ ] [a] b|");
    CHECK(taskToggled("[] b|") == "- [ ] [] b|");
    CHECK(taskToggled("[ ]x|") == "- [ ] [ ]x|");
    // Code, headings, tables and rules are not tasks.
    CHECK(taskToggled("```\n|x\n```") == "unchanged");
    CHECK(taskToggled("```\n- [ ] x|\n```") == "unchanged");
    CHECK(taskToggled("# Title|") == "unchanged");
    {
        const std::u16string table = u"| a | b |\n|---|---|\n| 1 | 2 |";
        CHECK(!MarkdownTasks::toggle(table, 25, 25).changed);
        CHECK(!MarkdownTasks::toggle(table, 0, 0).changed);
    }
    CHECK(taskToggled("text\n\n---|") == "unchanged");
    // Non-ASCII text keeps its UTF-16 offsets.
    CHECK(taskToggled("- [ ] caf\xC3\xA9|") != "unchanged");
    {
        std::u16string t = u"- [ ] café \U0001F600";
        MarkdownTasks::Edit e = MarkdownTasks::toggle(t, t.size(), t.size());
        CHECK(e.changed && e.replaceStart == 3 && e.replacement == u"x");
        CHECK(e.selStart == t.size());
    }

    GROUP("tasks:bold-italic");
    CHECK(wrapped("a {word} b", u"**") == "a **{word}** b");
    CHECK(wrapped("a **{word}** b", u"**") == "a {word} b");      // just outside
    CHECK(wrapped("a {**word**} b", u"**") == "a {word} b");      // inside the selection
    CHECK(wrapped("a wo|rd b", u"**") == "a **wo|rd** b");        // the word at the caret
    CHECK(wrapped("a **wo|rd** b", u"**") == "a wo|rd b");
    CHECK(wrapped("a | b", u"**") == "a **|** b");                // an empty pair
    CHECK(wrapped("a **|** b", u"**") == "a | b");
    CHECK(wrapped("x {word} y", u"*") == "x *{word}* y");
    CHECK(wrapped("x *{word}* y", u"*") == "x {word} y");
    CHECK(wrapped("x wo|rd y", u"*") == "x *wo|rd* y");
    // Italic never takes a star from bold; three stars are both.
    CHECK(wrapped("**{word}**", u"*") == "***{word}***");
    CHECK(wrapped("***{word}***", u"*") == "**{word}**");
    CHECK(wrapped("***{word}***", u"**") == "*{word}*");
    CHECK(wrapped("*{word}*", u"**") == "***{word}***");
    CHECK(wrapped("{two\nlines}", u"**") == "**{two\nlines}**");
    CHECK(wrapped("caf\xC3|", u"*") != "unchanged");             // past ASCII counts as a word

    GROUP("tasks:return");
    CHECK(taskReturn("- [ ] a|") == "- [ ] a\n- [ ] |");
    CHECK(taskReturn("- [x] a|") == "- [x] a\n- [ ] |");
    CHECK(taskReturn("- a|") == "- a\n- |");
    CHECK(taskReturn("  * a|") == "  * a\n  * |");
    CHECK(taskReturn("+ a|") == "+ a\n+ |");
    CHECK(taskReturn("9. a|") == "9. a\n10. |");
    CHECK(taskReturn("3) a|") == "3) a\n4) |");
    CHECK(taskReturn("2. [ ] a|") == "2. [ ] a\n3. [ ] |");
    CHECK(taskReturn("> - [ ] a|") == "> - [ ] a\n> - [ ] |");
    CHECK(taskReturn("-   a|") == "-   a\n-   |");
    CHECK(taskReturn("- [ ] fo|o") == "- [ ] fo\n- [ ] |o");
    CHECK(taskReturn("- {a}b") == "- \n- |b");
    CHECK(taskReturn("x\n- [ ] |") == "x\n|");
    CHECK(taskReturn("- a\n  - |") == "- a\n|");
    CHECK(taskReturn("- |") == "|");
    CHECK(taskReturn("|- a") == "unchanged");
    CHECK(taskReturn("- [ |] a") == "unchanged");
    CHECK(taskReturn("plain|") == "unchanged");
    CHECK(taskReturn("```\n- a|\n```") == "unchanged");
    CHECK(taskReturn("    - a|") == "unchanged");   // indented code
    CHECK(taskReturn("-a|") == "unchanged");
    CHECK(taskReturn("--- |") == "unchanged");
    CHECK(taskReturn("{- a\n- b}") == "unchanged");
    CHECK(taskReturn("- a|\r\n- b") == "- a\n- |\r\n- b");

    GROUP("tasks:preview");
    const std::string src =
        "# T\n- [ ] a\n- [x] b\n  - [ ] c\n```\n- [ ] no\n```\n> - [X] q\n- plain\n";
    MarkdownTasks::Counts c = MarkdownTasks::count(src);
    CHECK(c.total == 4 && c.done == 2);
    CHECK(MarkdownTasks::count("nothing here\n").total == 0);
    size_t mark = 0;
    CHECK(MarkdownTasks::toggleBox(src, 1, &mark) ==
          "# T\n- [x] a\n- [x] b\n  - [ ] c\n```\n- [ ] no\n```\n> - [X] q\n- plain\n");
    CHECK(mark == 7);
    CHECK(MarkdownTasks::toggleBox(src, 2).substr(12, 7) == "- [ ] b");
    CHECK(MarkdownTasks::toggleBox(src, 3).find("  - [x] c") != std::string::npos);
    CHECK(MarkdownTasks::toggleBox(src, 5) == src);   // in code
    CHECK(MarkdownTasks::toggleBox(src, 8) == src);   // no box
    CHECK(MarkdownTasks::toggleBox(src, 0) == src);
    CHECK(MarkdownTasks::toggleBox(src, 99) == src);
    CHECK(MarkdownTasks::toggleBox(src, -1) == src);
    CHECK(MarkdownTasks::toggleBox(src, 7).find("> - [ ] q") != std::string::npos);
    MarkdownTasks::Box box = MarkdownTasks::boxOnLine(src, 2);
    CHECK(box.found && box.checked);
    std::vector<MarkdownTasks::OpenTask> open = MarkdownTasks::openTasks(src);
    CHECK(open.size() == 2);
    if (open.size() == 2) {
        CHECK(open[0].line == 1 && open[0].column == 2);
        CHECK(open[1].line == 3 && open[1].column == 4);
    }
    // The parser marks a task item's marker and text, not its children's.
    std::vector<MdRun> runs = MarkdownParser::parse("- [x] done *now*\n  - [ ] sub\n- plain\n");
    int done = 0, open1 = 0, plain = 0;
    for (const MdRun &r : runs) {
        if (r.text == "\n") continue;
        if (r.task == 2) done++;
        else if (r.task == 1) open1++;
        else plain++;
    }
    CHECK(done == 3 && open1 == 2 && plain == 2);   // marker, "done ", "now"
    for (const MdRun &r : runs)
        if (r.marker && r.task) CHECK(r.text.find(r.task == 2 ? "\xE2\x98\x91" : "\xE2\x98\x90") != std::string::npos);

    GROUP("tasks:todo-lines");
    size_t col = 0, len = 0;
    CHECK(FolderSearch::findTodoInLine("// TODO: x", false, &col, &len) && col == 3 && len == 4);
    CHECK(FolderSearch::findTodoInLine("x = 1  # FIXME later", false, &col, &len) && len == 5);
    CHECK(FolderSearch::findTodoInLine("/* HACK */", false, &col, &len) && col == 3);
    CHECK(FolderSearch::findTodoInLine("/** XXX odd */", false, &col, &len));
    CHECK(FolderSearch::findTodoInLine("   * BUG: in the middle", false, &col, &len));
    CHECK(FolderSearch::findTodoInLine("-- TODO sql", false, &col, &len));
    CHECK(FolderSearch::findTodoInLine("; TODO lisp", false, &col, &len));
    CHECK(FolderSearch::findTodoInLine("% TODO tex", false, &col, &len));
    CHECK(FolderSearch::findTodoInLine("    TODO: in a docstring", false, &col, &len) && col == 4);
    CHECK(FolderSearch::findTodoInLine("call(); //TODO(eric) tidy", false, &col, &len) && col == 10);
    CHECK(!FolderSearch::findTodoInLine("int TODO = 1;", false, &col, &len));
    CHECK(!FolderSearch::findTodoInLine("// TODOS", false, &col, &len));
    CHECK(!FolderSearch::findTodoInLine("// todo: lower", false, &col, &len));
    CHECK(!FolderSearch::findTodoInLine("TODO write", false, &col, &len));
    CHECK(!FolderSearch::findTodoInLine("x = a * TODO", false, &col, &len));
    CHECK(!FolderSearch::findTodoInLine("s = \"TODO\"", false, &col, &len));
    CHECK(!FolderSearch::findTodoInLine("# TODO list", true, &col, &len));
    CHECK(FolderSearch::findTodoInLine("# TODO list", false, &col, &len));
    CHECK(FolderSearch::findTodoInLine("<!-- TODO: hidden -->", true, &col, &len) && col == 5);
    CHECK(FolderSearch::isMarkdownPath("/a/b/Notes.MD"));
    CHECK(FolderSearch::isMarkdownPath("x.markdown"));
    CHECK(!FolderSearch::isMarkdownPath("/a.md/readme"));
    CHECK(!FolderSearch::isMarkdownPath("x.mdx"));

    GROUP("tasks:todo-folder");
    std::random_device rd;
    const fsys::path root = fsys::temp_directory_path() /
        ("minicode-todo-test-" + std::to_string(rd()));
    fsys::create_directories(root / "sub");
    fsys::create_directories(root / "node_modules");
    writeFile(root / "a.cpp", "int x;\n// TODO: one\nint TODO = 2;\n");
    writeFile(root / "notes.md",
              "# TODO heading\n- [ ] task\n```\n- [ ] no\n```\n- [x] done\n<!-- FIXME: hidden -->\n");
    writeFile(root / "sub" / "b.py", "def f():\n    pass  # XXX slow\n");
    // A dot folder is listed, and what .gitignore names is not.
    writeFile(root / ".github" / "ci.yml", "# TODO: in a dot folder\n");
    writeFile(root / ".gitignore", "ignored.py\n");
    writeFile(root / "ignored.py", "# TODO: ignored\n");
    writeFile(root / "node_modules" / "m.js", "// TODO: skipped\n");
    FolderSearchResult r = FolderSearch::findTodos(root.string());
    CHECK(r.matches.size() == 5);
    if (r.matches.size() == 5) {
        CHECK(r.matches[0].relativePath == "a.cpp" && r.matches[0].line == 2 &&
              r.matches[0].text == "// TODO: one" && r.matches[0].column == 4);
        CHECK(r.matches[1].relativePath == "notes.md" && r.matches[1].line == 2 &&
              r.matches[1].byteColumn == 2 && r.matches[1].byteLength == 3);
        CHECK(r.matches[2].relativePath == "notes.md" && r.matches[2].line == 7 &&
              r.matches[2].byteColumn == 5);
        CHECK(r.matches[3].relativePath == ".github/ci.yml" && r.matches[3].line == 1);
        CHECK(r.matches[4].relativePath == "sub/b.py" && r.matches[4].line == 2);
    }
    CHECK(r.filesMatched == 4);
    std::atomic<bool> stop{true};
    CHECK(FolderSearch::findTodos(root.string(), &stop).cancelled);
    CHECK(FolderSearch::findTodos((root / "a.cpp").string()).matches.empty());
    std::error_code ec;
    fsys::remove_all(root, ec);
}

// ------------------------------------------------------------ editor tabs
static void testEditorTabs() {
    using namespace EditorTabs;
    GROUP("tabs:insert");
    CHECK(insertionIndex(-1, 0) == 0);
    CHECK(insertionIndex(0, 1) == 1);
    CHECK(insertionIndex(1, 4) == 2);
    CHECK(insertionIndex(3, 4) == 4);
    CHECK(insertionIndex(-1, 3) == 3);

    GROUP("tabs:number");
    CHECK(indexForNumber(1, 3) == 0);
    CHECK(indexForNumber(3, 3) == 2);
    CHECK(indexForNumber(4, 3) == -1);
    CHECK(indexForNumber(9, 3) == 2);    // 9 is the last, however many
    CHECK(indexForNumber(9, 12) == 11);
    CHECK(indexForNumber(8, 12) == 7);
    CHECK(indexForNumber(1, 0) == -1);
    CHECK(indexForNumber(9, 0) == -1);
    CHECK(indexForNumber(0, 5) == -1);
    CHECK(indexForNumber(10, 12) == -1);

    GROUP("tabs:step");
    CHECK(step(0, 3, 1) == 1);
    CHECK(step(2, 3, 1) == 0);           // wraps
    CHECK(step(0, 3, -1) == 2);
    CHECK(step(1, 3, -1) == 0);
    CHECK(step(0, 1, 1) == 0);
    CHECK(step(0, 0, 1) == -1);
    CHECK(step(-1, 3, 1) == 0);
    CHECK(step(-1, 3, -1) == 2);

    GROUP("tabs:close");
    // Another tab closed: the active one stays, its index shifting.
    CHECK(activeAfterClose({1, 2, 3}, 2, 0) == 1);
    CHECK(activeAfterClose({1, 2, 3}, 0, 2) == 0);
    CHECK(activeAfterClose({1, 2, 3}, 1, 2) == 1);
    // The active one closed: the most recently used of the rest.
    CHECK(activeAfterClose({5, 9, 7}, 1, 1) == 1);   // tab 2 (stamp 7), now index 1
    CHECK(activeAfterClose({8, 9, 7}, 1, 1) == 0);
    CHECK(activeAfterClose({1, 2, 3, 4}, 3, 3) == 2);
    CHECK(activeAfterClose({4, 1, 2, 3}, 0, 0) == 2);
    CHECK(activeAfterClose({1}, 0, 0) == -1);        // the last tab
    CHECK(activeAfterClose({}, -1, 0) == -1);
    CHECK(activeAfterClose({1, 2}, 0, 5) == -1);     // no such tab

    GROUP("tabs:session");
    auto has = [](const std::set<std::string> &files) {
        return [files](const std::string &f) { return files.count(f) > 0; };
    };
    {
        Session s{{"/p/a", "/p/b", "/p/c"}, 1};
        Session r = restorable(s, has({"/p/a", "/p/b", "/p/c"}));
        CHECK(r.files == s.files && r.active == 1);
    }
    {   // the active file is gone: the one before it
        Session r = restorable({{"/p/a", "/p/b", "/p/c"}, 2}, has({"/p/a", "/p/b"}));
        CHECK((r.files == std::vector<std::string>{"/p/a", "/p/b"}) && r.active == 1);
    }
    {   // missing ones dropped, the active one followed
        Session r = restorable({{"/p/a", "/p/b", "/p/c"}, 2}, has({"/p/c", "/p/b"}));
        CHECK((r.files == std::vector<std::string>{"/p/b", "/p/c"}) && r.active == 1);
    }
    {   // nothing before the active file is left: the first
        Session r = restorable({{"/p/a", "/p/b", "/p/c"}, 0}, has({"/p/c"}));
        CHECK((r.files == std::vector<std::string>{"/p/c"}) && r.active == 0);
    }
    {   // repeats and empty entries dropped; active on a repeat goes to its file
        Session r = restorable({{"/p/a", "", "/p/b", "/p/a"}, 3}, has({"/p/a", "/p/b"}));
        CHECK((r.files == std::vector<std::string>{"/p/a", "/p/b"}) && r.active == 0);
    }
    {
        Session r = restorable({{"/p/a"}, 0}, has({}));
        CHECK(r.files.empty() && r.active == -1);
    }
    {   // an active index out of range
        Session r = restorable({{"/p/a", "/p/b"}, 7}, has({"/p/a", "/p/b"}));
        CHECK(r.files.size() == 2 && r.active == 0);
    }

    GROUP("tabs:labels");
    CHECK((labels({"/p/a.txt", "/p/b.txt"}) == std::vector<std::string>{"a.txt", "b.txt"}));
    CHECK((labels({"/p/src/main.cpp", "/p/tests/main.cpp", "/p/x.h"}) ==
           std::vector<std::string>{"src/main.cpp", "tests/main.cpp", "x.h"}));
    CHECK((labels({"/a/x/m.c", "/b/x/m.c"}) == std::vector<std::string>{"a/x/m.c", "b/x/m.c"}));
    CHECK((labels({"/p/one/README.md", "", "/p/two/README.md"}) ==
           std::vector<std::string>{"one/README.md", "", "two/README.md"}));
    CHECK((labels({"/p/q.md", "/p/q.md"}) == std::vector<std::string>{"q.md", "q.md"}));
    CHECK((labels({"/m.c", "/x/m.c"}) == std::vector<std::string>{"m.c", "x/m.c"}));
    CHECK(labels({}).empty());
}

// ------------------------------------------------------------------ formatting
namespace {

std::u16string u16(const std::string &s) { return std::u16string(s.begin(), s.end()); }

// A fake file system: path -> contents.
Formatter::Fs fakeFs(const std::map<std::string, std::string> &files) {
    Formatter::Fs fs;
    fs.exists = [files](const std::string &p) { return files.count(p) > 0; };
    fs.read = [files](const std::string &p) {
        auto it = files.find(p);
        return it == files.end() ? std::string() : it->second;
    };
    return fs;
}

}  // namespace

void testFormatter() {
    using namespace Formatter;
    bool ranged0 = true;
    GROUP("format:languages");
    CHECK(languageFor("/a/b/x.py") == "python");
    CHECK(languageFor("/a/b/X.PYI") == "python");
    CHECK(languageFor("/a/main.cpp") == "cpp");
    CHECK(languageFor("/a/v.h") == "cpp");
    CHECK(languageFor("/a/App.java") == "cpp");
    CHECK(languageFor("/a/app.tsx") == "cpp");
    CHECK(languageFor("/a/x.proto") == "cpp");
    CHECK(languageFor("/a/x.mm") == "cpp");
    CHECK(languageFor("/a/main.go") == "go");
    CHECK(languageFor("/a/lib.rs") == "rust");
    CHECK(languageFor("/a/run.sh") == "shell");
    CHECK(languageFor("/a/notes.md").empty());
    CHECK(languageFor("/a/Makefile").empty());
    CHECK(languageFor("/a/.py").empty());       // a dotfile, not a .py
    CHECK(languageFor("/a.d/x").empty());
    CHECK(languages().size() == 5);
    CHECK(displayName("cpp") == "C-family");

    GROUP("format:plan defaults");
    Fs empty = fakeFs({});
    Plan p = plan("python", "", "/w/proj/pkg/a.py", "/w/proj", empty);
    CHECK(!p.off && !p.fromSettings && p.configFile.empty());
    CHECK(p.candidates.size() == 2);
    CHECK(p.candidates.size() == 2 && p.candidates[0].program == "pyink" &&
          p.candidates[1].program == "yapf");
    CHECK(p.candidates.size() == 2 &&
          p.candidates[0].command.find("--line-length 80") != std::string::npos &&
          p.candidates[1].command == "yapf --style=google");
    CHECK(p.installHint == "uv tool install pyink");
    Plan c = plan("cpp", "", "/w/proj/m.cc", "/w/proj", empty);
    CHECK(c.candidates.size() == 1 && c.candidates[0].program == "clang-format" &&
          c.candidates[0].command.find("--fallback-style=Google") != std::string::npos);
    CHECK(plan("go", "", "/w/m.go", "/w", empty).candidates.size() == 1);
    CHECK(plan("txt", "", "/w/m.txt", "/w", empty).candidates.empty());

    GROUP("format:plan settings");
    Plan s = plan("python", "  /opt/bin/yapf --style=pep8 ", "/w/a.py", "/w", empty);
    CHECK(s.fromSettings && s.candidates.size() == 1 &&
          s.candidates[0].program == "/opt/bin/yapf" &&
          s.candidates[0].command == "/opt/bin/yapf --style=pep8" &&
          s.installHint == "uv tool install yapf");
    CHECK(plan("python", "OFF", "/w/a.py", "/w", empty).off);
    CHECK(plan("cpp", "none", "/w/a.c", "/w", empty).off);
    Plan q = plan("cpp", "\"/opt/my tools/clang-format\" -style=LLVM", "/w/a.c", "/w", empty);
    CHECK(q.candidates.size() == 1 && q.candidates[0].program == "/opt/my tools/clang-format");
    // A project's configuration never overrides the settings.
    Plan s2 = plan("python", "yapf", "/w/a.py", "/w",
                   fakeFs({{"/w/pyproject.toml", "[tool.black]\n"}}));
    CHECK(s2.candidates.size() == 1 && s2.candidates[0].command == "yapf" &&
          s2.configFile.empty());

    GROUP("format:plan project");
    Plan pk = plan("python", "", "/w/proj/pkg/a.py", "/w/proj",
                   fakeFs({{"/w/proj/pyproject.toml",
                            "[project]\nname = 'x'\n\n[tool.pyink]\nline-length = 100\n"}}));
    CHECK(pk.candidates.size() == 1 && pk.candidates[0].program == "pyink" &&
          pk.candidates[0].projectStyle &&
          pk.candidates[0].command.find("--line-length") == std::string::npos);
    CHECK(pk.configFile == "/w/proj/pyproject.toml");
    Plan bl = plan("python", "", "/w/proj/a.py", "/w/proj",
                   fakeFs({{"/w/proj/pyproject.toml", "[tool.black]\nline-length = 88\n"}}));
    CHECK(bl.candidates.size() == 1 && bl.candidates[0].program == "black" &&
          bl.installHint == "uv tool install black");
    Plan yp = plan("python", "", "/w/proj/sub/a.py", "/w/proj",
                   fakeFs({{"/w/proj/sub/.style.yapf", "[style]\nbased_on_style = pep8\n"},
                           {"/w/proj/pyproject.toml", "[tool.pyink]\n"}}));
    CHECK(yp.candidates.size() == 1 && yp.candidates[0].command == "yapf" &&
          yp.configFile == "/w/proj/sub/.style.yapf");   // nearest folder wins
    Plan sc = plan("python", "", "/w/a.py", "/w",
                   fakeFs({{"/w/setup.cfg", "[metadata]\n[yapf]\nbased_on_style=google\n"}}));
    CHECK(sc.candidates.size() == 1 && sc.candidates[0].program == "yapf");
    Plan rf = plan("python", "", "/w/a.py", "/w",
                   fakeFs({{"/w/pyproject.toml", "[tool.ruff.format]\nquote-style='single'\n"}}));
    CHECK(rf.candidates.size() == 1 && rf.candidates[0].program == "ruff");
    // Any [tool.ruff] table means Ruff, lint settings alone included.
    Plan rt = plan("python", "", "/w/pkg/a.py", "/w",
                   fakeFs({{"/w/pyproject.toml",
                            "[project]\nname = 'x'\n\n[tool.ruff]\nline-length = 100\n"}}));
    CHECK(rt.candidates.size() == 1 && rt.candidates[0].program == "ruff" &&
          rt.candidates[0].projectStyle &&
          rt.candidates[0].command == "ruff format --stdin-filename {file} -" &&
          rt.configFile == "/w/pyproject.toml" && rt.installHint == "uv tool install ruff");
    Plan rl = plan("python", "", "/w/a.py", "/w",
                   fakeFs({{"/w/pyproject.toml", "[tool.ruff.lint]\nselect = ['E']\n"}}));
    CHECK(rl.candidates.size() == 1 && rl.candidates[0].program == "ruff");
    Plan rp = plan("python", "", "/w/a.py", "/w",
                   fakeFs({{"/w/pyproject.toml",
                            "[tool.ruff.lint.per-file-ignores]\n\"__init__.py\" = ['F401']\n"}}));
    CHECK(rp.candidates.size() == 1 && rp.candidates[0].program == "ruff");
    Plan rq = plan("python", "", "/w/a.py", "/w",
                   fakeFs({{"/w/pyproject.toml", "[ tool . \"ruff\" ]\nline-length = 90\n"}}));
    CHECK(rq.candidates.size() == 1 && rq.candidates[0].program == "ruff");
    Plan rd = plan("python", "", "/w/a.py", "/w", fakeFs({{"/w/.ruff.toml", "line-length = 88\n"}}));
    CHECK(rd.candidates.size() == 1 && rd.candidates[0].program == "ruff" &&
          rd.configFile == "/w/.ruff.toml");
    // A formatter named beside Ruff's lint settings is the one that formats.
    Plan rb = plan("python", "", "/w/a.py", "/w",
                   fakeFs({{"/w/pyproject.toml",
                            "[tool.ruff]\nselect = ['E']\n[tool.black]\nline-length = 88\n"}}));
    CHECK(rb.candidates.size() == 1 && rb.candidates[0].program == "black");
    Plan ri = plan("python", "", "/w/a.py", "/w",
                   fakeFs({{"/w/pyproject.toml", "[tool.ruff]\n[tool.pyink]\n"}}));
    CHECK(ri.candidates.size() == 1 && ri.candidates[0].program == "pyink");
    // The nearest folder still decides.
    Plan rn = plan("python", "", "/w/sub/a.py", "/w",
                   fakeFs({{"/w/sub/pyproject.toml", "[tool.ruff]\n"},
                           {"/w/pyproject.toml", "[tool.black]\n"}}));
    CHECK(rn.candidates.size() == 1 && rn.candidates[0].program == "ruff" &&
          rn.configFile == "/w/sub/pyproject.toml");
    // A name that only starts like Ruff's is not Ruff.
    Plan rx = plan("python", "", "/w/a.py", "/w",
                   fakeFs({{"/w/pyproject.toml", "[tool.ruffle]\n[tool.ruff-lsp]\n"}}));
    CHECK(rx.candidates.size() == 2 && rx.configFile.empty());
    // A pyproject.toml that configures no formatter changes nothing.
    Plan np = plan("python", "", "/w/a.py", "/w",
                   fakeFs({{"/w/pyproject.toml", "[tool.mypy]\nstrict = true\n"}}));
    CHECK(np.candidates.size() == 2 && np.configFile.empty());
    // The search stops at the open folder, and at a repository's top.
    Plan above = plan("cpp", "", "/w/proj/m.cc", "/w/proj",
                      fakeFs({{"/w/.clang-format", "BasedOnStyle: LLVM\n"}}));
    CHECK(above.configFile.empty() && !above.candidates[0].projectStyle);
    Plan repo = plan("cpp", "", "/w/proj/sub/m.cc", "",
                     fakeFs({{"/w/proj/sub/.git", ""}, {"/w/proj/.clang-format", ""}}));
    CHECK(repo.configFile.empty());
    Plan cf = plan("cpp", "", "/w/proj/src/m.cc", "/w/proj",
                   fakeFs({{"/w/proj/.clang-format", "BasedOnStyle: LLVM\n"}}));
    CHECK(cf.configFile == "/w/proj/.clang-format" && cf.candidates[0].projectStyle);
    Plan sh = plan("shell", "", "/w/x.sh", "/w",
                   fakeFs({{"/w/.editorconfig", "root = true\n[*.py]\nindent_size = 4\n"}}));
    CHECK(sh.configFile.empty() && sh.candidates[0].command.find("-i 2") != std::string::npos);
    Plan sh2 = plan("shell", "", "/w/x.sh", "/w",
                    fakeFs({{"/w/.editorconfig", "[*.{sh,bash}]\nindent_size = 4\n"}}));
    CHECK(sh2.configFile == "/w/.editorconfig" &&
          sh2.candidates[0].command == "shfmt --filename {file}");
    Plan rs = plan("rust", "", "/w/crate/src/lib.rs", "/w",
                   fakeFs({{"/w/crate/Cargo.toml",
                            "[package]\nname = \"x\"\nedition = \"2024\"\n"}}));
    CHECK(rs.candidates.size() == 1 && rs.candidates[0].command == "rustfmt --edition 2024");

    GROUP("format:javascript");
    // No configuration: clang-format in Google's style, as before.
    Plan js = plan("cpp", "", "/w/web/src/app.ts", "/w/web",
                   fakeFs({{"/w/web/package.json", "{\"name\": \"web\"}"}}));
    CHECK(js.candidates.size() == 1 && js.candidates[0].program == "clang-format" &&
          !js.candidates[0].projectStyle && js.configFile.empty());
    CHECK(defaultCommand("cpp").rfind("clang-format", 0) == 0);
    // A .prettierrc, in the file's folder or above it.
    Plan pr = plan("cpp", "", "/w/web/src/app.tsx", "/w/web",
                   fakeFs({{"/w/web/.prettierrc", "{}"}}));
    CHECK(pr.candidates.size() == 1 && pr.candidates[0].program == "prettier" &&
          pr.candidates[0].projectStyle &&
          pr.candidates[0].command == "prettier --stdin-filepath {file}" &&
          pr.configFile == "/w/web/.prettierrc" && pr.installHint == "npm install -g prettier");
    for (const char *name : {".prettierrc.json", ".prettierrc.yaml", ".prettierrc.mjs",
                             "prettier.config.js", "prettier.config.ts"}) {
        Plan each = plan("cpp", "", "/w/a.js", "/w", fakeFs({{std::string("/w/") + name, ""}}));
        CHECK(each.candidates.size() == 1 && each.candidates[0].program == "prettier");
    }
    // A "prettier" key in package.json counts, and so does Prettier as a
    // dependency (it needs no configuration file) or in node_modules/.bin;
    // a package.json with neither does not.
    Plan pj = plan("cpp", "", "/w/a.mjs", "/w",
                   fakeFs({{"/w/package.json",
                            "{\"name\": \"x\", \"prettier\": {\"semi\": false}}"}}));
    CHECK(pj.candidates.size() == 1 && pj.candidates[0].program == "prettier" &&
          pj.configFile == "/w/package.json");
    Plan pd = plan("cpp", "", "/w/a.js", "/w",
                   fakeFs({{"/w/package.json", "{\"devDependencies\": {\"prettier\": \"^3\"}}"}}));
    CHECK(pd.candidates.size() == 1 && pd.candidates[0].program == "prettier" &&
          pd.configFile == "/w/package.json");
    Plan pdd = plan("cpp", "", "/w/a.js", "/w",
                    fakeFs({{"/w/package.json", "{\"dependencies\": {\"prettier\": \"3\"}}"}}));
    CHECK(pdd.candidates.size() == 1 && pdd.candidates[0].program == "prettier");
    Plan pnm = plan("cpp", "", "/w/src/a.ts", "/w",
                    fakeFs({{"/w/node_modules/.bin/prettier", ""}}));
    CHECK(pnm.candidates.size() == 1 && pnm.candidates[0].program == "prettier");
    Plan pother = plan("cpp", "", "/w/a.js", "/w",
                       fakeFs({{"/w/package.json",
                                "{\"devDependencies\": {\"prettier-plugin-x\": \"1\", "
                                "\"eslint\": \"9\"}, \"scripts\": {\"prettier\": \"x\"}}"}}));
    CHECK(pother.candidates.size() == 1 && pother.candidates[0].program == "clang-format");
    CHECK(jsonHasDependency("{\"devDependencies\": {\"prettier\": \"^3\"}}", "prettier") &&
          !jsonHasDependency("{\"prettier\": {}}", "prettier") &&
          !jsonHasDependency("[", "prettier"));
    Plan pb = plan("cpp", "", "/w/a.js", "/w",
                   fakeFs({{"/w/package.json", "{\"prettier\": "}}));   // broken JSON
    CHECK(pb.candidates.size() == 1 && pb.candidates[0].program == "clang-format");
    // Biome.
    Plan bi = plan("cpp", "", "/w/pkg/a.cts", "/w", fakeFs({{"/w/biome.jsonc", "{}"}}));
    CHECK(bi.candidates.size() == 1 && bi.candidates[0].program == "biome" &&
          bi.candidates[0].command == "biome format --stdin-file-path={file}" &&
          bi.installHint == "npm install -g @biomejs/biome");
    CHECK(plan("cpp", "", "/w/a.js", "/w", fakeFs({{"/w/biome.json", "{}"}}))
              .candidates[0].program == "biome");
    // Only JavaScript and TypeScript: C++ and Java beside a .prettierrc keep
    // clang-format, and a .clang-format beside one loses for scripts only.
    Plan cc = plan("cpp", "", "/w/m.cc", "/w", fakeFs({{"/w/.prettierrc", "{}"}}));
    CHECK(cc.candidates.size() == 1 && cc.candidates[0].program == "clang-format" &&
          cc.configFile.empty());
    Plan jv = plan("cpp", "", "/w/A.java", "/w", fakeFs({{"/w/biome.json", "{}"}}));
    CHECK(jv.candidates[0].program == "clang-format");
    Fs both = fakeFs({{"/w/.prettierrc", "{}"}, {"/w/.clang-format", ""}});
    CHECK(plan("cpp", "", "/w/a.ts", "/w", both).candidates[0].program == "prettier");
    CHECK(plan("cpp", "", "/w/a.cpp", "/w", both).candidates[0].program == "clang-format");
    // The nearest folder still decides.
    Plan near = plan("cpp", "", "/w/web/a.ts", "/w",
                     fakeFs({{"/w/web/.clang-format", ""}, {"/w/.prettierrc", "{}"}}));
    CHECK(near.candidates[0].program == "clang-format" &&
          near.configFile == "/w/web/.clang-format");
    // The settings still win.
    Plan st = plan("cpp", "clang-format", "/w/a.ts", "/w", fakeFs({{"/w/.prettierrc", "{}"}}));
    CHECK(st.fromSettings && st.candidates[0].command == "clang-format");
    CHECK(jsonHasKey("{\"prettier\": \"@company/config\"}", "prettier"));
    CHECK(!jsonHasKey("[\"prettier\"]", "prettier"));
    CHECK(!jsonHasKey("", "prettier"));
    // The project's own copy, in node_modules/.bin up to the open folder.
    std::vector<std::string> nb = nodeBinDirs("/w/web/src/a.ts", "/w/web");
    CHECK(nb.size() == 2 && nb[0] == "/w/web/src/node_modules/.bin" &&
          nb[1] == "/w/web/node_modules/.bin");
    CHECK(nodeBinDirs("/elsewhere/a.ts", "/w") ==
          std::vector<std::string>{"/elsewhere/node_modules/.bin"});
    CHECK(fromNodeModules("prettier") && fromNodeModules("/x/node_modules/.bin/biome") &&
          !fromNodeModules("clang-format") && !fromNodeModules("pyink"));
    // No line ranges for them: the whole file is formatted.
    std::vector<std::string> pa =
        arguments("prettier --stdin-filepath {file}", "/w/a b.ts", 2, 3, &ranged0);
    CHECK(!ranged0 && pa.size() == 3 && pa[2] == "/w/a b.ts");

    GROUP("format:project files");
    CHECK(tomlHasTable("[tool.pyink]\n", "tool.pyink"));
    CHECK(tomlHasTable("  [ tool . pyink ]  # comment\n", "tool.pyink"));
    CHECK(tomlHasTable("[tool.\"pyink\"]\n", "tool.pyink"));
    CHECK(tomlHasTable("[tool.ruff.format]\n", "tool.ruff"));
    CHECK(!tomlHasTable("[tool.pyinkx]\n", "tool.pyink"));
    CHECK(!tomlHasTable("# [tool.pyink]\nx = '[tool.pyink]'\n", "tool.pyink"));
    CHECK(iniHasSection("[metadata]\n[YAPF]\n", "yapf"));
    CHECK(!iniHasSection("[yapf.x]\n", "yapf"));
    CHECK(cargoEdition("[package]\nname = \"a\"\nedition = \"2018\" # old\n") == "2018");
    CHECK(cargoEdition("[package]\nname = \"a\"\n") == "2021");
    CHECK(cargoEdition("[dependencies]\nedition = \"2015\"\n") == "2021");
    CHECK(cargoEdition("[package]\neditions = \"2015\"\n") == "2021");
    CHECK(editorconfigCoversShell("[*]\nindent_style = space\n"));
    CHECK(editorconfigCoversShell("[*.bash]\n"));
    CHECK(!editorconfigCoversShell("root = true\n[*.{js,py}]\n"));

    GROUP("format:venv");
    std::vector<std::string> v = venvBinDirs("/w/proj/pkg/a.py", "/w/proj", "/env/x");
    CHECK(v.size() == 5);
    CHECK(v.size() == 5 && v[0] == "/w/proj/pkg/.venv/bin" && v[1] == "/w/proj/pkg/venv/bin" &&
          v[2] == "/w/proj/.venv/bin" && v[3] == "/w/proj/venv/bin" && v[4] == "/env/x/bin");
    CHECK(venvBinDirs("/w/a.py", "/w/", "/w/.venv").size() == 2);   // no duplicate
    std::vector<std::string> out = venvBinDirs("/elsewhere/x/a.py", "/w", "");
    CHECK(out.size() == 2 && out[0] == "/elsewhere/x/.venv/bin");
    CHECK(venvBinDirs("/wx/a.py", "/w", "").size() == 2);   // /wx is not inside /w

    GROUP("format:arguments");
    std::vector<std::string> a = arguments(
        "clang-format --style=file --assume-filename={file}", "/my dir/it's \"x\".cc");
    CHECK(a.size() == 3 && a[2] == "--assume-filename=/my dir/it's \"x\".cc");
    std::vector<std::string> q2 = arguments("'/opt/my tools/yapf' --style=google", "/a.py");
    CHECK(q2.size() == 2 && q2[0] == "/opt/my tools/yapf");
    std::vector<std::string> two = arguments("tool {file}:{file}", "/x");
    CHECK(two.size() == 2 && two[1] == "/x:/x");
    bool ranged = true;
    std::vector<std::string> r1 = arguments("clang-format --assume-filename={file}", "/a.c",
                                            3, 7, &ranged);
    CHECK(ranged && r1.size() == 3 && r1[2] == "--lines=3:7");
    std::vector<std::string> r2 = arguments(
        "pyink --quiet --stdin-filename {file} -", "/a.py", 2, 2, &ranged);
    CHECK(ranged && r2.size() == 6 && r2[4] == "--line-ranges=2-2" && r2[5] == "-");
    std::vector<std::string> r3 = arguments("/v/bin/yapf --style=google", "/a.py", 1, 4, &ranged);
    CHECK(ranged && r3.back() == "--lines=1-4");
    std::vector<std::string> r4 = arguments("gofmt", "/a.go", 1, 4, &ranged);
    CHECK(!ranged && r4.size() == 1);
    std::vector<std::string> r5 = arguments("yapf", "/a.py", 0, 0, &ranged);
    CHECK(!ranged && r5.size() == 1);
    CHECK(programName("/opt/homebrew/bin/clang-format") == "clang-format");

    GROUP("format:errors");
    std::string e = errorSummary(
        "error: cannot format /w/p/a.py: Cannot parse: 1:6\n\n    def f(:\n         ^\n",
        "", "/w/p/a.py");
    CHECK(e == "error: cannot format a.py: Cannot parse: 1:6\n    def f(:\n         ^");
    CHECK(errorSummary("<standard input>:2:12: expected ')'\n", "", "/x/m.go") ==
          "m.go:2:12: expected ')'");
    CHECK(errorSummary("  \n", "out line\n", "/x/a") == "out line");
    CHECK(errorSummary("1\n2\n3\n4\n5\n6\n7\n8\n", "", "/x/a", 3) == "1\n2\n3\n…");
    CHECK(errorSummary("", "", "/x/a").empty());
}

void testLineDiff() {
    GROUP("linediff:hunks");
    std::u16string a = u16("a\nb\nc\nd\ne\n");
    CHECK(LineDiff::diff(a, a).empty());
    std::u16string b = u16("a\nB\nc\nd\nE\nf\n");
    std::vector<LineDiff::Hunk> h = LineDiff::diff(a, b);
    CHECK(h.size() == 2);
    CHECK(h.size() == 2 && h[0].oldLine == 1 && h[0].oldLines == 1 && h[0].newLines == 1 &&
          h[0].oldStart == 2 && h[0].oldLength == 2);
    CHECK(h.size() == 2 && h[1].oldLine == 4 && h[1].oldLines == 1 && h[1].newLines == 2);
    CHECK(LineDiff::apply(a, b, h) == b);
    // Insertions and deletions only.
    std::u16string ins = u16("a\nx\ny\nb\nc\nd\ne\n");
    h = LineDiff::diff(a, ins);
    CHECK(h.size() == 1 && h[0].oldLines == 0 && h[0].newLines == 2 && h[0].oldStart == 2);
    CHECK(LineDiff::apply(a, ins, h) == ins);
    h = LineDiff::diff(ins, a);
    CHECK(h.size() == 1 && h[0].oldLines == 2 && h[0].newLines == 0);
    // No final newline, an empty side.
    CHECK(LineDiff::apply(u16("x"), u16("x\n"), LineDiff::diff(u16("x"), u16("x\n"))) == u16("x\n"));
    CHECK(LineDiff::apply(u16(""), u16("q\n"), LineDiff::diff(u16(""), u16("q\n"))) == u16("q\n"));
    CHECK(LineDiff::apply(u16("q\n"), u16(""), LineDiff::diff(u16("q\n"), u16(""))) == u16(""));
    // Too many edits for the cap: one hunk, still right.
    std::u16string m1, m2;
    for (int i = 0; i < 300; i++) {
        m1 += u16("line " + std::to_string(i) + "\n");
        m2 += u16("LINE " + std::to_string(i) + "\n");
    }
    h = LineDiff::diff(m1, m2, 50);
    CHECK(h.size() == 1 && LineDiff::apply(m1, m2, h) == m2);
    CHECK(LineDiff::apply(m1, m2, LineDiff::diff(m1, m2)) == m2);

    GROUP("linediff:random");
    std::mt19937 rng(7);
    bool allOk = true, minimal = true;
    for (int round = 0; round < 400; round++) {
        std::vector<std::string> lines;
        int n = rng() % 30;
        for (int i = 0; i < n; i++) lines.push_back(std::string(1, char('a' + rng() % 5)));
        std::vector<std::string> edited = lines;
        int edits = rng() % 6;
        for (int e = 0; e < edits; e++) {
            int op = rng() % 3;
            size_t at = edited.empty() ? 0 : rng() % (edited.size() + 1);
            if (op == 0 || edited.empty())
                edited.insert(edited.begin() + at, std::string(1, char('a' + rng() % 7)));
            else if (at < edited.size() && op == 1) edited.erase(edited.begin() + at);
            else if (at < edited.size()) edited[at] = "z";
        }
        std::u16string x, y;
        for (auto &l : lines) x += u16(l + "\n");
        for (auto &l : edited) y += u16(l + "\n");
        if (rng() % 2 && !y.empty()) y.pop_back();   // sometimes no final newline
        std::vector<LineDiff::Hunk> hh = LineDiff::diff(x, y);
        if (LineDiff::apply(x, y, hh) != y) allOk = false;
        size_t changed = 0;
        for (auto &k : hh) changed += k.oldLines + k.newLines;
        // An edit costs at most 2 changed lines (a replacement), and losing
        // the final newline 2 more.
        if (changed > (size_t)(2 * edits + 2)) minimal = false;
        for (size_t i = 1; i < hh.size(); i++)
            if (hh[i].oldStart <= hh[i - 1].oldStart + hh[i - 1].oldLength) allOk = false;
    }
    CHECK(allOk);
    CHECK(minimal);

    GROUP("linediff:map");
    // Outside hunks: shifted by what the hunks before gained.
    std::u16string o = u16("def f( a,b ):\n    return a+b\nx = 1\n");
    std::u16string n2 = u16("def f(a, b):\n  return a + b\n\n\nx = 1\n");
    h = LineDiff::diff(o, n2);
    CHECK(LineDiff::apply(o, n2, h) == n2);
    size_t xo = o.find(u"x = 1"), xn = n2.find(u"x = 1");
    CHECK(LineDiff::mapOffset(o, n2, h, xo) == xn);
    CHECK(LineDiff::mapOffset(o, n2, h, xo + 4) == xn + 4);
    CHECK(LineDiff::mapOffset(o, n2, h, o.size()) == n2.size());
    // Inside: before the same character...
    CHECK(LineDiff::mapOffset(o, n2, h, o.find(u"b ):")) == n2.find(u"b):"));
    CHECK(LineDiff::mapOffset(o, n2, h, o.find(u"return")) == n2.find(u"return"));
    CHECK(LineDiff::mapOffset(o, n2, h, o.find(u"+b")) == n2.find(u"+ b"));
    // ...after it when the caret sat on whitespace...
    CHECK(LineDiff::mapOffset(o, n2, h, o.find(u" ):")) == n2.find(u"):"));
    // ...and in the indentation, at the same column of its line.
    CHECK(LineDiff::mapOffset(o, n2, h, o.find(u"    return") + 1) == n2.find(u"  return") + 1);
    // The end of a line stays the end of that line.
    CHECK(LineDiff::mapOffset(o, n2, h, o.find(u"\n")) == n2.find(u"\n"));
    CHECK(LineDiff::mapOffset(o, n2, h, 0) == 0);
    // Blank lines the formatter added can pair with ones it kept; the hunks
    // either side are joined, so the caret still finds its code.
    std::u16string bo = u16("w = 0\ndef h(x,y):\n    return x+y\n\nu = 1\n");
    std::u16string bn = u16("w = 0\n\n\ndef h(x, y):\n    return x + y\n\n\nu = 1\n");
    h = LineDiff::diff(bo, bn);
    CHECK(h.size() == 1 && LineDiff::apply(bo, bn, h) == bn);
    CHECK(LineDiff::mapOffset(bo, bn, h, bo.find(u"return")) == bn.find(u"return"));
    CHECK(LineDiff::mapOffset(bo, bn, h, bo.find(u"u = 1")) == bn.find(u"u = 1"));
}
#include "AutoIndentTests.cpp"
#include "KeepSortedTests.cpp"
#include "FileDetailsTests.cpp"

// How to install the servers and formatters: one table, the app's notes and
// the README.
static void testInstallHints() {
    GROUP("install:table");
    const Install::Tool *py = Install::server("python");
    CHECK(py && py->name == "basedpyright" && !py->homepage.empty());
    CHECK(py && py->ways.size() == 4 && py->ways[0].command == "uv tool install basedpyright" &&
          py->ways[0].needs == "uv");
    CHECK(Install::server("cpp") &&
          Install::server("cpp")->ways[0].command == "xcode-select --install" &&
          Install::server("cpp")->ways[0].needs.empty());
    CHECK(Install::server("go") &&
          Install::server("go")->ways[0].command == "go install golang.org/x/tools/gopls@latest");
    CHECK(Install::server("rust") &&
          Install::server("rust")->ways[0].command == "rustup component add rust-analyzer");
    CHECK(Install::server("typescript") &&
          Install::server("typescript")->ways[0].command ==
              "npm install -g typescript-language-server typescript");
    CHECK(Install::formatter("pyink") &&
          Install::formatter("/x/.venv/bin/pyink") == Install::formatter("pyink"));
    CHECK(Install::formatter("shfmt") &&
          Install::formatter("shfmt")->ways[0].command == "brew install shfmt");
    CHECK(Install::formatter("clang-format") &&
          Install::formatter("clang-format")->ways.size() == 2);
    CHECK(Install::formatter("rustfmt") && Install::formatter("gofmt"));
    CHECK(Install::formatter("dprint") == nullptr);
    CHECK(Install::formatter("node_modules/.bin/prettier") &&
          Install::formatter("prettier")->ways.front().command == "npm install -g prettier");
    CHECK(Install::formatter("biome") && Install::formatter("biome")->ways.size() == 2);
    // Every formatter Format Document can pick has a hint.
    for (const std::string &lang : Formatter::languages()) {
        Formatter::Fs none;
        none.exists = [](const std::string &) { return false; };
        none.read = [](const std::string &) { return std::string(); };
        Formatter::Plan pl = Formatter::plan(lang, "", "/w/a", "/w", none);
        for (const Formatter::Candidate &c : pl.candidates)
            CHECK(Install::formatter(c.program) != nullptr);
        CHECK(!pl.installHint.empty());
    }
    // Every command is one line, every tool says what it gives and where it
    // lives, and every prerequisite is one `prerequisite` knows.
    for (const Install::Tool *t : Install::all()) {
        std::string gives = t->gives;   // "Black formats ...": names it, any case
        std::transform(gives.begin(), gives.end(), gives.begin(), ::tolower);
        CHECK(!gives.empty() && gives.find(t->name) != std::string::npos);
        CHECK(t->homepage.rfind("https://", 0) == 0);
        for (const Install::Way &w : t->ways) {
            CHECK(w.command.find('\n') == std::string::npos && !w.command.empty());
            if (!w.needs.empty()) CHECK(!Install::prerequisite(w.needs).url.empty());
        }
    }
    CHECK(Install::prerequisite("uv").command == "brew install uv");
    CHECK(Install::prerequisite("brew").command.empty() &&
          Install::prerequisite("brew").url == "https://brew.sh");
    CHECK(Install::serverKeys().size() == Settings::lspServers().size());

    GROUP("install:suggest");
    auto haveOnly = [](std::set<std::string> progs) {
        return [progs](const std::string &p) { return progs.count(p) > 0; };
    };
    Install::Suggestion s = Install::suggest(*py, haveOnly({"uv", "brew"}));
    CHECK(s.command == "uv tool install basedpyright" && s.before.empty() &&
          s.beforeUrl.empty() && s.missing.empty());
    CHECK(s.alternatives.size() == 3 && s.alternatives[0] == "pipx install basedpyright" &&
          s.alternatives[1] == "npm install -g pyright");
    // No uv: the first that can run.
    s = Install::suggest(*py, haveOnly({"npm", "brew"}));
    CHECK(s.command == "npm install -g pyright" && s.before.empty());
    CHECK(s.alternatives.size() == 3 && s.alternatives[0] == "uv tool install basedpyright");
    s = Install::suggest(*py, haveOnly({"pipx"}));
    CHECK(s.command == "pipx install basedpyright");
    // Nothing that can run: the best, after getting uv with Homebrew...
    s = Install::suggest(*py, haveOnly({"brew"}));
    CHECK(s.command == "uv tool install basedpyright" && s.before == "brew install uv" &&
          s.missing == "uv" && s.beforeUrl.empty());
    // ...or from uv's page when Homebrew is missing too.
    s = Install::suggest(*py, haveOnly({}));
    CHECK(s.command == "uv tool install basedpyright" && s.before.empty() &&
          s.missing == "uv" && s.beforeUrl.find("astral.sh") != std::string::npos);
    // Needs nothing: always first.
    s = Install::suggest(*Install::server("cpp"), haveOnly({}));
    CHECK(s.command == "xcode-select --install" && s.missing.empty() &&
          s.alternatives == std::vector<std::string>{"brew install llvm"});
    // No go, but Homebrew: Homebrew's gopls.
    s = Install::suggest(*Install::server("go"), haveOnly({"brew"}));
    CHECK(s.command == "brew install gopls" && s.before.empty());
    // No rustup and no Homebrew: rustup's page.
    s = Install::suggest(*Install::server("rust"), haveOnly({}));
    CHECK(s.command == "rustup component add rust-analyzer" && s.before.empty() &&
          s.beforeUrl == "https://rustup.rs");
    // Other tools that would do come after the tool's own other ways.
    s = Install::suggest(*Install::formatter("pyink"), haveOnly({"pipx"}),
                         {Install::formatter("pyink"), Install::formatter("yapf")});
    CHECK(s.command == "pipx install pyink");
    CHECK((s.alternatives ==
           std::vector<std::string>{"uv tool install pyink", "pipx install yapf"}));
    Install::Tool none;
    CHECK(Install::suggest(none, haveOnly({})).command.empty());

    GROUP("install:readme");
    // The README's "Language servers and formatters" section lists every
    // command the app suggests, word for word.
    std::ifstream f("README.md");
    if (!f) {
        std::printf("  (README.md not found here; README checks skipped)\n");
        return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string readme = ss.str();
    for (const Install::Tool *t : Install::all())
        for (const Install::Way &w : t->ways) {
            bool found = readme.find("`" + w.command + "`") != std::string::npos;
            if (!found) std::printf("  README lacks `%s`\n", w.command.c_str());
            CHECK(found);
        }
}

// The core files a build file names: "../src/X.cpp" in the Makefile,
// "core_dir / 'X.cpp'" in core_sources in meson.build.
static std::set<std::string> coreFilesIn(const std::string &text, const std::string &before,
                                         const std::string &after) {
    std::set<std::string> out;
    size_t at = 0;
    while ((at = text.find(before, at)) != std::string::npos) {
        at += before.size();
        size_t end = text.find(after, at);
        if (end == std::string::npos) break;
        out.insert(text.substr(at, end - at));
        at = end;
    }
    return out;
}

// The instance variables a class extension block declares: the name before
// each ";" (a C array's size dropped), comments skipped.
static std::vector<std::string> ivarNamesIn(const std::string &block) {
    std::vector<std::string> out;
    std::istringstream lines(block);
    std::string line;
    auto ident = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
    while (std::getline(lines, line)) {
        size_t cm = line.find("//");
        if (cm != std::string::npos) line.erase(cm);
        size_t semi = line.find(';');
        if (semi == std::string::npos) continue;
        size_t e = semi;
        if (e > 0 && line[e - 1] == ']') e = line.rfind('[', e);
        if (e == std::string::npos) continue;
        size_t b = e;
        while (b > 0 && ident(line[b - 1])) b--;
        if (b < e && line[b] == '_') out.push_back(line.substr(b, e - b));
    }
    return out;
}

// Whether `name` appears in `text` as a whole identifier.
static bool mentionsIdent(const std::string &text, const std::string &name) {
    auto ident = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
    for (size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 1)) {
        bool before = at > 0 && ident(text[at - 1]);
        bool after = at + name.size() < text.size() && ident(text[at + name.size()]);
        if (!before && !after) return true;
    }
    return false;
}

static void testEditorControllerIvars() {
    GROUP("structure:ivars");
    // EditorController's shared class extension (EditorController+Private.h)
    // holds only the state more than one category uses; a variable of one
    // category's goes in its own EditorController+<Category>Ivars.h, which
    // EditorController.mm imports so the ivars are laid out. Every category
    // sees the shared block, and every one rebuilds when it changes.
    namespace fs = std::filesystem;
    std::ifstream ph("src/EditorController+Private.h");
    if (!ph) {
        std::printf("  (src/ not found here; ivar checks skipped)\n");
        return;
    }
    std::stringstream ps;
    ps << ph.rdbuf();
    const std::string priv = ps.str();
    size_t a = priv.find("@interface EditorController () <NSTextStorageDelegate> {");
    size_t b = a == std::string::npos ? a : priv.find("\n}\n", a);
    CHECK(a != std::string::npos && b != std::string::npos);
    if (a == std::string::npos || b == std::string::npos) return;
    std::vector<std::string> shared = ivarNamesIn(priv.substr(a, b - a));
    CHECK(shared.size() >= 10);

    std::map<std::string, std::string> categories;   // file name -> text
    std::vector<std::string> ivarHeaders;
    std::string core;
    for (const auto &e : fs::directory_iterator("src")) {
        const std::string name = e.path().filename().string();
        std::ifstream f(e.path());
        std::stringstream s;
        s << f.rdbuf();
        if (name == "EditorController.mm") core = s.str();
        else if (name.rfind("EditorController+", 0) == 0 && e.path().extension() == ".mm")
            categories[name] = s.str();
        else if (name.rfind("EditorController+", 0) == 0 && name.size() > 7 &&
                 name.compare(name.size() - 7, 7, "Ivars.h") == 0)
            ivarHeaders.push_back(name);
    }
    CHECK(categories.size() >= 10);
    for (const std::string &v : shared) {
        int users = 0;
        for (const auto &[file, text] : categories) users += mentionsIdent(text, v);
        if (users < 2)
            std::printf("  %s is in the shared ivar block but %s; move it to that category's "
                        "EditorController+<Category>Ivars.h\n",
                        v.c_str(), users ? "only one category uses it" : "no category uses it");
        CHECK(users >= 2);
    }
    CHECK(ivarHeaders.size() >= 5);
    for (const std::string &h : ivarHeaders) {
        const std::string imp = "#import \"" + h + "\"";
        bool laidOut = core.find(imp) != std::string::npos;
        if (!laidOut) std::printf("  EditorController.mm does not import %s\n", h.c_str());
        CHECK(laidOut);
        // No category the header is not imported by uses its variables (the
        // compiler says so too), and the shared block repeats none of them.
        std::ifstream f("src/" + h);
        std::stringstream s;
        s << f.rdbuf();
        for (const std::string &v : ivarNamesIn(s.str())) {
            CHECK(std::find(shared.begin(), shared.end(), v) == shared.end());
            for (const auto &[file, text] : categories)
                if (text.find(imp) == std::string::npos && mentionsIdent(text, v)) {
                    std::printf("  %s uses %s from %s\n", file.c_str(), v.c_str(), h.c_str());
                    CHECK(false);
                }
        }
    }
}

static void testLinuxBuildLists() {
    GROUP("linux:build-lists");
    // CI builds the GTK port with Meson only, so the plain Makefile's list of
    // core files fell behind (MarkdownTasks, AutoIndent and KeepSorted were
    // missing and it could not link). Both must name the same core files.
    std::ifstream mf("linux/Makefile"), mb("linux/meson.build");
    if (!mf || !mb) {
        std::printf("  (linux/ not found here; build list checks skipped)\n");
        return;
    }
    std::stringstream m, b;
    m << mf.rdbuf();
    b << mb.rdbuf();
    std::string make = m.str(), meson = b.str();
    // The Makefile's CORE variable, up to the first blank line.
    size_t c0 = make.find("\nCORE :=");
    size_t c1 = c0 == std::string::npos ? c0 : make.find("\n\n", c0);
    CHECK(c0 != std::string::npos && c1 != std::string::npos);
    if (c0 == std::string::npos || c1 == std::string::npos) return;
    std::set<std::string> fromMake = coreFilesIn(make.substr(c0, c1 - c0), "../src/", ".cpp");
    size_t s0 = meson.find("core_sources = [");
    size_t s1 = s0 == std::string::npos ? s0 : meson.find("]", s0);
    CHECK(s0 != std::string::npos && s1 != std::string::npos);
    if (s0 == std::string::npos || s1 == std::string::npos) return;
    std::set<std::string> fromMeson =
        coreFilesIn(meson.substr(s0, s1 - s0), "core_dir / '", ".cpp'");
    for (const std::string &f : fromMeson)
        if (!fromMake.count(f)) std::printf("  linux/Makefile lacks ../src/%s.cpp\n", f.c_str());
    for (const std::string &f : fromMake)
        if (!fromMeson.count(f)) std::printf("  meson.build lacks %s.cpp\n", f.c_str());
    CHECK(fromMeson.size() >= 16);
    CHECK(fromMake == fromMeson);

    // Android's CMakeLists names only the core files the app uses, so it
    // cannot equal the others; but every file it names must exist, and
    // every core file whose header those files (or the JNI glue) include,
    // directly or through other headers, must be in it too, or the APK
    // fails to link. CI does not build Android, so this is what catches a
    // core file that starts using another.
    std::ifstream ac("android/app/src/main/cpp/CMakeLists.txt");
    if (!ac) {
        std::printf("  (android/ not found here; its build list check skipped)\n");
        return;
    }
    std::stringstream a;
    a << ac.rdbuf();
    const std::string cmake = a.str();
    auto readFile = [](const std::string &path, std::string &out) {
        std::ifstream f(path);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    };
    auto exists = [](const std::string &path) {
        struct stat st;
        return stat(path.c_str(), &st) == 0;
    };
    std::set<std::string> core = coreFilesIn(cmake, "${CORE}/", ".cpp");
    std::set<std::string> fromLinux = coreFilesIn(cmake, "${LINUX}/", ".cpp");
    CHECK(core.size() >= 10);
    // The sources to scan: the core files and the JNI glue beside the list.
    std::vector<std::string> scan;
    for (const std::string &f : core) {
        bool ok = exists("src/" + f + ".cpp");
        if (!ok) std::printf("  CMakeLists.txt names src/%s.cpp, which is not there\n", f.c_str());
        CHECK(ok);
        scan.push_back("src/" + f + ".cpp");
        scan.push_back("src/" + f + ".h");
    }
    for (const std::string &f : fromLinux) {
        bool ok = exists("linux/src/" + f + ".cpp");
        if (!ok) std::printf("  CMakeLists.txt names linux/src/%s.cpp, which is not there\n", f.c_str());
        CHECK(ok);
        scan.push_back("linux/src/" + f + ".cpp");
        scan.push_back("linux/src/" + f + ".h");
    }
    {
        std::stringstream words(cmake);
        std::string w;
        while (words >> w) {
            while (!w.empty() && (w.back() == ')' || w.back() == '(')) w.pop_back();
            if (w.size() > 8 && w.find('/') == std::string::npos &&
                w.compare(w.size() - 8, 8, "_jni.cpp") == 0)
                scan.push_back("android/app/src/main/cpp/" + w);
        }
    }
    CHECK(scan.size() > core.size() * 2);   // the JNI files were found
    std::set<std::string> seen;
    for (size_t i = 0; i < scan.size(); i++) {
        std::string text;
        if (!readFile(scan[i], text)) continue;
        for (const std::string &inc : coreFilesIn(text, "#include \"", "\"")) {
            size_t slash = inc.find_last_of('/');
            std::string name = inc.substr(slash == std::string::npos ? 0 : slash + 1);
            if (name.size() < 3 || name.compare(name.size() - 2, 2, ".h") != 0) continue;
            name.resize(name.size() - 2);
            if (!seen.insert(name).second) continue;
            if (exists("src/" + name + ".h")) scan.push_back("src/" + name + ".h");
            if (!exists("src/" + name + ".cpp")) continue;
            bool listed = core.count(name) > 0;
            if (!listed)
                std::printf("  Android's CMakeLists.txt lacks ${CORE}/%s.cpp (%s includes it)\n",
                            name.c_str(), scan[i].c_str());
            CHECK(listed);
        }
    }
}

static void testChangeRequest() {
    using namespace ChangeRequest;
    GROUP("change:prompt");
    Context c;
    c.request = "  Make the tree's font bigger\n ";
    c.version = "1.5.2";
    c.appPath = "/Applications/MiniCode.app";
    std::string p = prompt(c);
    CHECK(p.rfind("Make the tree's font bigger\n\n(Sent from", 0) == 0);
    CHECK(p.find("minicode 1.5.2 is running from /Applications/MiniCode.app.") !=
          std::string::npos);
    CHECK(p.find("run `make`") != std::string::npos);
    CHECK(p.find("Do not commit, push, or release unless asked.") != std::string::npos);
    c.request = " \n\t ";
    CHECK(prompt(c).empty());
    c.request = "x";
    c.version.clear();
    c.appPath.clear();
    CHECK(prompt(c).find("(Sent from minicode's Request a Change bar. minicode is running. ") !=
          std::string::npos);

    GROUP("change:commands");
    CHECK(startCommand("claude", "/src/mini code", "/tmp/req 1.txt") ==
          "cd '/src/mini code' && claude \"$(cat '/tmp/req 1.txt')\"");
    CHECK(startCommand("  claude --model opus ", "/s", "/t/r.txt") ==
          "cd /s && claude --model opus \"$(cat /t/r.txt)\"");
    CHECK(resumeCommand("claude", "/a b") == "cd '/a b' && claude --continue");
    CHECK(std::string(kRepository) == "https://github.com/e-c-hansen/minicode.git");
    CHECK(cloneCommand("1.5.2", "/Users/x/Library/Application Support/MiniCode/source") ==
          "(git clone --branch v1.5.2 https://github.com/e-c-hansen/minicode.git "
          "'/Users/x/Library/Application Support/MiniCode/source' || git clone "
          "https://github.com/e-c-hansen/minicode.git "
          "'/Users/x/Library/Application Support/MiniCode/source')");
    CHECK(cloneCommand(" ", "/s") == "git clone https://github.com/e-c-hansen/minicode.git /s");
    CHECK(cloneThenStartCommand("1.6.0", "claude", "/s", "/t/r.txt") ==
          "(git clone --branch v1.6.0 https://github.com/e-c-hansen/minicode.git /s || "
          "git clone https://github.com/e-c-hansen/minicode.git /s) && "
          "cd /s && claude \"$(cat /t/r.txt)\"");

    GROUP("change:source");
    std::set<std::string> files = {"/x/mc/Makefile", "/x/mc/src/EditorController.mm",
                                   "/y/Makefile"};
    auto exists = [&](const std::string &f) { return files.count(f) > 0; };
    CHECK(looksLikeSource("/x/mc", exists));
    CHECK(looksLikeSource("/x/mc/", exists));
    CHECK(!looksLikeSource("/y", exists));
    CHECK(!looksLikeSource("", exists));
    CHECK(checkoutOfBundle("/x/mc/MiniCode.app") == "/x/mc");
    CHECK(checkoutOfBundle("/x/mc/MiniCode.app/") == "/x/mc");
    CHECK(checkoutOfBundle("/MiniCode.app").empty());
    CHECK(builtApp("/x/mc/") == "/x/mc/MiniCode.app");
    CHECK(buildStamp("/x/mc") == "/x/mc/MiniCode.app/Contents/_CodeSignature/CodeResources");

    GROUP("change:settings");
    Settings d = parseSettings("");
    CHECK(d.developCommand() == "claude" && d.developSource().empty());
    std::vector<SettingsError> e;
    Settings s = parseSettings("develop.command = claude --model opus  # mine\n"
                               "develop.source = ~/Code/my minicode # here\n", &e);
    CHECK(e.empty());
    CHECK(s.developCommand() == "claude --model opus");
    CHECK(s.developSource() == "~/Code/my minicode");
    CHECK(parseSettings("develop.source = auto\n").developSource().empty());
    std::vector<SettingsError> e2;
    parseSettings("develop.command =\n", &e2);
    CHECK(e2.size() == 1);
}

static void testMultiCaret() {
    using namespace MultiCaret;
    auto carets = [](std::initializer_list<size_t> at) {
        std::vector<Caret> v;
        for (size_t p : at) v.push_back({p, p});
        return v;
    };
    auto heads = [](const std::vector<Caret> &v) {
        std::vector<size_t> out;
        for (const Caret &c : v) out.push_back(c.head);
        return out;
    };

    GROUP("multicaret:normalize");
    {
        std::vector<Caret> v = normalize({{8, 8}, {2, 2}, {2, 2}, {20, 30}}, 10);
        CHECK(v.size() == 3);
        CHECK(v[0] == (Caret{2, 2}) && v[1] == (Caret{8, 8}) && v[2] == (Caret{10, 10}));
        v = normalize({{2, 6}, {4, 9}}, 20);   // overlapping selections merge
        CHECK(v.size() == 1 && v[0].lo() == 2 && v[0].hi() == 9);
        v = normalize({{2, 5}, {5, 8}}, 20);   // touching selections stay two
        CHECK(v.size() == 2);
        v = normalize({{2, 5}, {5, 5}}, 20);   // a caret at a selection's end joins it
        CHECK(v.size() == 1 && v[0].lo() == 2 && v[0].hi() == 5);
        CHECK(normalize({}, 5).size() == 1);
    }

    GROUP("multicaret:prefix");
    {
        // The request that started it: a common prefix on every line.
        const std::u16string t = u"alpha\nbeta\n\ngamma";
        std::vector<Caret> c = carets({0, 6, 11, 12});
        Edit e = insert(c, {u"// "});
        CHECK(applyEdit(t, e) == u"// alpha\n// beta\n// \n// gamma");
        CHECK(heads(e.carets) == (std::vector<size_t>{3, 12, 20, 24}));
        // Typing on: the carets follow.
        Edit e2 = insert(e.carets, {u"x"});
        CHECK(applyEdit(applyEdit(t, e), e2) == u"// xalpha\n// xbeta\n// x\n// xgamma");
    }

    GROUP("multicaret:column-replace");
    {
        const std::u16string t = u"int a = 1;\nint b = 2;\nint c = 3;";
        std::vector<Caret> c = {{0, 3}, {11, 14}, {22, 25}};
        Edit e = insert(c, {u"long"});
        CHECK(applyEdit(t, e) == u"long a = 1;\nlong b = 2;\nlong c = 3;");
        CHECK(heads(e.carets) == (std::vector<size_t>{4, 16, 28}));
        CHECK(copy(t, c) == u"int\nint\nint");
        CHECK(copy(t, carets({1, 2})).empty());
    }

    GROUP("multicaret:backspace");
    {
        const std::u16string t = u"// a\n// b\nc";
        Edit e = backspace(t, carets({3, 8}));
        CHECK(applyEdit(t, e) == u"//a\n//b\nc");
        CHECK(heads(e.carets) == (std::vector<size_t>{2, 6}));
        // At a line's start nothing goes, so the column stays.
        Edit e2 = backspace(t, carets({0, 5, 10}));
        CHECK(e2.changes.empty());
        CHECK(applyEdit(t, e2) == t);
        CHECK(heads(e2.carets) == (std::vector<size_t>{0, 5, 10}));
        // Mixed: one at a start, one not.
        Edit e3 = backspace(t, carets({5, 3}));
        CHECK(applyEdit(t, e3) == u"//a\n// b\nc");
        // A surrogate pair goes whole.
        const std::u16string emoji = u"a\U0001F600\nb\U0001F600";
        Edit e4 = backspace(emoji, carets({3, 7}));
        CHECK(applyEdit(emoji, e4) == u"a\nb");
        // Two carets side by side end up as one.
        Edit e5 = backspace(u"abc", carets({1, 2}));
        CHECK(applyEdit(u"abc", e5) == u"c" && e5.carets.size() == 1 && e5.carets[0].head == 0);
        // Selections are deleted whatever their size.
        Edit e6 = backspace(u"abcd\nefgh", {{1, 3}, {6, 8}});
        CHECK(applyEdit(u"abcd\nefgh", e6) == u"ad\neh");
    }

    GROUP("multicaret:delete-forward");
    {
        const std::u16string t = u"ab\ncd";
        Edit e = deleteForward(t, carets({0, 3}));
        CHECK(applyEdit(t, e) == u"b\nd");
        Edit e2 = deleteForward(t, carets({2, 5}));   // line end, text end
        CHECK(e2.changes.empty());
    }

    GROUP("multicaret:move");
    {
        const std::u16string t = u"    foo bar\n  baz.qux\nx";
        std::vector<Caret> c = carets({8, 16});
        CHECK(heads(move(t, c, Move::Left, false)) == (std::vector<size_t>{7, 15}));
        CHECK(heads(move(t, c, Move::Right, false)) == (std::vector<size_t>{9, 17}));
        CHECK(heads(move(t, c, Move::LineEnd, false)) == (std::vector<size_t>{11, 21}));
        // Home: first non-blank, then column 0.
        std::vector<Caret> h = move(t, c, Move::LineStart, false);
        CHECK(heads(h) == (std::vector<size_t>{4, 14}));
        CHECK(heads(move(t, h, Move::LineStart, false)) == (std::vector<size_t>{0, 12}));
        CHECK(heads(move(t, c, Move::WordRight, false)) == (std::vector<size_t>{11, 17}));
        CHECK(heads(move(t, carets({11, 21}), Move::WordLeft, false)) ==
              (std::vector<size_t>{8, 18}));
        // Extending keeps the anchors.
        std::vector<Caret> x = move(t, c, Move::LineEnd, true);
        CHECK(x[0] == (Caret{8, 11}) && x[1] == (Caret{16, 21}));
        // Without Shift, Left and Right collapse a selection to its edge.
        std::vector<Caret> s = {{8, 11}, {16, 21}};
        CHECK(heads(move(t, s, Move::Left, false)) == (std::vector<size_t>{8, 16}));
        CHECK(heads(move(t, s, Move::Right, false)) == (std::vector<size_t>{11, 21}));
        // Carets that meet merge.
        CHECK(move(u"ab", carets({0, 1}), Move::Left, false).size() == 1);
    }

    GROUP("multicaret:surround");
    {
        const std::u16string t = u"say hello now";
        Edit e = surround({{4, 9}}, u'(');
        CHECK(applyEdit(t, e) == u"say (hello) now");
        CHECK(e.carets.size() == 1 && e.carets[0] == (Caret{5, 10}));
        // Again: wraps the same text a second time.
        Edit e2 = surround(e.carets, u'"');
        CHECK(applyEdit(applyEdit(t, e), e2) == u"say (\"hello\") now");
        // A backwards selection stays backwards.
        Edit e3 = surround({{9, 4}}, u'[');
        CHECK(applyEdit(t, e3) == u"say [hello] now" && e3.carets[0] == (Caret{10, 5}));
        // Several selections, each wrapped, carets following.
        const std::u16string u = u"a b\nc d";
        Edit e4 = surround({{0, 1}, {4, 5}}, u'`');
        CHECK(applyEdit(u, e4) == u"`a` b\n`c` d");
        CHECK(e4.carets[0] == (Caret{1, 2}) && e4.carets[1] == (Caret{7, 8}));
        CHECK(applyEdit(u, surround({{0, 3}}, u'{')) == u"{a b}\nc d");
        CHECK(applyEdit(u, surround({{0, 1}}, u'\'')) == u"'a' b\nc d");
        CHECK(closerFor(u'x') == 0 && closerFor(u'<') == 0);
        CHECK(surround({{0, 1}}, u'x').changes.empty());
    }

    GROUP("multicaret:duplicate");
    {
        const std::u16string t = u"one\ntwo\nthree";
        // A caret alone copies its line.
        Edit d = duplicateLines(t, carets({5}), true);
        CHECK(applyEdit(t, d) == u"one\ntwo\ntwo\nthree");
        CHECK(heads(d.carets) == (std::vector<size_t>{9}));
        Edit u = duplicateLines(t, carets({5}), false);
        CHECK(applyEdit(t, u) == u"one\ntwo\ntwo\nthree");
        CHECK(heads(u.carets) == (std::vector<size_t>{5}));
        // A selection over two lines copies both, and stays selected on the copy.
        Edit s2 = duplicateLines(t, {{1, 6}}, true);
        CHECK(applyEdit(t, s2) == u"one\ntwo\none\ntwo\nthree");
        CHECK(s2.carets[0] == (Caret{9, 14}));
        // Ending at the start of a line leaves that line out.
        Edit s3 = duplicateLines(t, {{0, 4}}, true);
        CHECK(applyEdit(t, s3) == u"one\none\ntwo\nthree");
        CHECK(s3.carets[0] == (Caret{4, 8}));
        // The last line, with no newline after it.
        Edit s4 = duplicateLines(t, carets({10}), true);
        CHECK(applyEdit(t, s4) == u"one\ntwo\nthree\nthree");
        CHECK(heads(s4.carets) == (std::vector<size_t>{16}));
        // Several carets on different lines: each line copied, carets follow.
        Edit s5 = duplicateLines(t, carets({1, 9}), true);
        CHECK(applyEdit(t, s5) == u"one\none\ntwo\nthree\nthree");
        CHECK(heads(s5.carets) == (std::vector<size_t>{5, 19}));
        // Two carets on one line: one copy.
        Edit s6 = duplicateLines(t, carets({4, 6}), false);
        CHECK(applyEdit(t, s6) == u"one\ntwo\ntwo\nthree");
        CHECK(heads(s6.carets) == (std::vector<size_t>{4, 6}));
        // CRLF stays CRLF.
        const std::u16string w = u"a\r\nb";
        CHECK(applyEdit(w, duplicateLines(w, carets({0}), true)) == u"a\r\na\r\nb");
        // An empty file.
        CHECK(applyEdit(u"", duplicateLines(u"", carets({0}), true)) == u"\n");
    }

    GROUP("multicaret:move-lines");
    {
        const std::u16string t = u"one\ntwo\nthree";
        // A caret moves its line down and up, staying at the same column.
        Edit d = moveLines(t, carets({5}), true);
        CHECK(applyEdit(t, d) == u"one\nthree\ntwo");
        CHECK(heads(d.carets) == (std::vector<size_t>{11}));
        Edit u = moveLines(t, carets({5}), false);
        CHECK(applyEdit(t, u) == u"two\none\nthree");
        CHECK(heads(u.carets) == (std::vector<size_t>{1}));
        // At the edges nothing moves.
        CHECK(moveLines(t, carets({1}), false).changes.empty());
        CHECK(moveLines(t, carets({10}), true).changes.empty());
        CHECK(heads(moveLines(t, carets({10}), true).carets) == (std::vector<size_t>{10}));
        // A selection over two lines moves both and stays selected.
        Edit s2 = moveLines(t, {{1, 6}}, true);
        CHECK(applyEdit(t, s2) == u"three\none\ntwo");
        CHECK(s2.carets[0] == (Caret{7, 12}));
        // Ending at the start of a line leaves that line where it is.
        Edit s3 = moveLines(t, {{0, 4}}, true);
        CHECK(applyEdit(t, s3) == u"two\none\nthree");
        CHECK(s3.carets[0] == (Caret{4, 8}));
        // Carets on neighbouring lines move together; apart, each moves.
        const std::u16string f = u"a\nb\nc\nd\ne";
        Edit n = moveLines(f, carets({2, 4}), true);
        CHECK(applyEdit(f, n) == u"a\nd\nb\nc\ne");
        CHECK(heads(n.carets) == (std::vector<size_t>{4, 6}));
        Edit a = moveLines(f, carets({0, 4}), true);
        CHECK(applyEdit(f, a) == u"b\na\nd\nc\ne");
        CHECK(heads(a.carets) == (std::vector<size_t>{2, 6}));
        // One block at an edge holds them all.
        CHECK(moveLines(f, carets({4, 8}), true).changes.empty());
        // CRLF stays CRLF, and a trailing newline gives an empty last line.
        const std::u16string w = u"a\r\nb\r\nc";
        CHECK(applyEdit(w, moveLines(w, carets({3}), true)) == u"a\r\nc\r\nb");
        CHECK(applyEdit(w, moveLines(w, carets({3}), false)) == u"b\r\na\r\nc");
        CHECK(applyEdit(u"x\n", moveLines(u"x\n", carets({0}), true)) == u"\nx");
        CHECK(moveLines(u"", carets({0}), true).changes.empty());
    }

    GROUP("multicaret:select-line");
    {
        const std::u16string t = u"one\ntwo\nthree";
        // A caret selects its line with its break, from start to the next line.
        std::vector<Caret> s = expandLineSelection(t, carets({5}));
        CHECK(s.size() == 1 && s[0] == (Caret{4, 8}));
        // Again: the next line joins.
        s = expandLineSelection(t, s);
        CHECK(s.size() == 1 && s[0] == (Caret{4, 13}));
        // The last line has no break; once all of it is selected, nothing more.
        CHECK(expandLineSelection(t, s)[0] == (Caret{4, 13}));
        // A selection inside lines takes them whole.
        CHECK(expandLineSelection(t, {{2, 5}})[0] == (Caret{0, 8}));
        // A backwards selection too.
        CHECK(expandLineSelection(t, {{5, 2}})[0] == (Caret{0, 8}));
        // Several carets, each its line; neighbours merge into one selection.
        std::vector<Caret> m = expandLineSelection(t, carets({1, 10}));
        CHECK(m.size() == 2 && m[0] == (Caret{0, 4}) && m[1] == (Caret{8, 13}));
        std::vector<Caret> j = expandLineSelection(t, carets({1, 5}));
        CHECK(j.size() == 1 && j[0] == (Caret{0, 8}));
        // CRLF: the break is both characters.
        CHECK(expandLineSelection(u"a\r\nb", carets({0}))[0] == (Caret{0, 3}));
        // An empty file, and an empty last line.
        CHECK(expandLineSelection(u"", carets({0}))[0] == (Caret{0, 0}));
        CHECK(expandLineSelection(u"x\n", carets({2}))[0] == (Caret{2, 2}));
    }

    GROUP("multicaret:delete-lines");
    {
        const std::u16string t = u"one\ntwo\nthree";
        // The caret's line goes; the caret keeps its column on the line below.
        Edit d = deleteLines(t, carets({5}));
        CHECK(applyEdit(t, d) == u"one\nthree");
        CHECK(heads(d.carets) == (std::vector<size_t>{5}));
        // A long column clamps to the shorter line that takes its place.
        Edit c = deleteLines(t, carets({12}));
        CHECK(applyEdit(t, c) == u"one\ntwo");
        CHECK(heads(c.carets) == (std::vector<size_t>{7}));   // the end of "two"
        // The first line.
        Edit f = deleteLines(t, carets({2}));
        CHECK(applyEdit(t, f) == u"two\nthree");
        CHECK(heads(f.carets) == (std::vector<size_t>{2}));
        // A selection over two lines takes both; ending at a line start leaves it.
        CHECK(applyEdit(t, deleteLines(t, {{1, 6}})) == u"three");
        CHECK(applyEdit(t, deleteLines(t, {{0, 4}})) == u"two\nthree");
        // Several carets apart: each line goes, one caret each, in the new text.
        const std::u16string g = u"a1\nb2\nc3\nd4\ne5";
        Edit k = deleteLines(g, carets({1, 7}));
        CHECK(applyEdit(g, k) == u"b2\nd4\ne5");
        CHECK(heads(k.carets) == (std::vector<size_t>{1, 4}));
        // Neighbouring lines make one block and one caret.
        Edit nb = deleteLines(g, carets({4, 7}));
        CHECK(applyEdit(g, nb) == u"a1\nd4\ne5");
        CHECK(heads(nb.carets) == (std::vector<size_t>{4}));
        // The last line and a block reaching it.
        Edit l = deleteLines(g, carets({10, 13}));
        CHECK(applyEdit(g, l) == u"a1\nb2\nc3");
        CHECK(heads(l.carets) == (std::vector<size_t>{7}));
        // Everything.
        Edit all = deleteLines(t, {{0, 13}});
        CHECK(applyEdit(t, all) == u"" && heads(all.carets) == (std::vector<size_t>{0}));
        CHECK(applyEdit(u"x", deleteLines(u"x", carets({1}))) == u"");
        // CRLF, and a trailing newline (the empty last line goes with its break).
        const std::u16string w = u"a\r\nb\r\nc";
        CHECK(applyEdit(w, deleteLines(w, carets({3}))) == u"a\r\nc");
        CHECK(applyEdit(w, deleteLines(w, carets({6}))) == u"a\r\nb");
        CHECK(applyEdit(u"x\n", deleteLines(u"x\n", carets({0}))) == u"");
        CHECK(applyEdit(u"x\n", deleteLines(u"x\n", carets({2}))) == u"x");
    }

    GROUP("multicaret:line-copy");
    {
        const std::u16string t = u"one\ntwo\nthree";
        CHECK(lineCopy(t, carets({5})) == u"two\n");
        CHECK(lineCopy(t, carets({10})) == u"three\n");   // a break added
        CHECK(lineCopy(t, carets({1, 2, 10})) == u"one\nthree\n");   // each line once
        CHECK(lineCopy(u"a\r\nb", carets({0})) == u"a\r\n");
        CHECK(lineCopy(u"", carets({0})) == u"\n");
        // Pasted with nothing selected: above the caret's line, caret kept.
        Edit p = pasteLinesAbove(t, carets({6}), u"two\n");
        CHECK(applyEdit(t, p) == u"one\ntwo\ntwo\nthree");
        CHECK(heads(p.carets) == (std::vector<size_t>{10}));
        // A copy with no final break gets one.
        CHECK(applyEdit(t, pasteLinesAbove(t, carets({0}), u"x")) == u"x\none\ntwo\nthree");
        // One line per caret line: one each; otherwise all at each.
        Edit e = pasteLinesAbove(t, carets({1, 9}), u"A\nB\n");
        CHECK(applyEdit(t, e) == u"A\none\ntwo\nB\nthree");
        CHECK(heads(e.carets) == (std::vector<size_t>{3, 13}));
        Edit a = pasteLinesAbove(t, carets({1, 9}), u"Z\n");
        CHECK(applyEdit(t, a) == u"Z\none\ntwo\nZ\nthree");
        CHECK(heads(a.carets) == (std::vector<size_t>{3, 13}));
        // Two carets on one line: one paste.
        Edit s = pasteLinesAbove(t, carets({0, 2}), u"Z\n");
        CHECK(applyEdit(t, s) == u"Z\none\ntwo\nthree");
        CHECK(heads(s.carets) == (std::vector<size_t>{2, 4}));
        // The round trip: Cut a line, then Paste it back above another.
        const std::u16string cut = lineCopy(t, carets({5}));
        const std::u16string after = applyEdit(t, deleteLines(t, carets({5})));
        CHECK(applyEdit(after, pasteLinesAbove(after, carets({0}), cut)) == u"two\none\nthree");
    }

    GROUP("multicaret:paste");
    {
        CHECK(pieces(u"a\nb\nc\n", 3) == (std::vector<std::u16string>{u"a", u"b", u"c"}));
        CHECK(pieces(u"a\r\nb", 2) == (std::vector<std::u16string>{u"a", u"b"}));
        CHECK(pieces(u"a\nb", 3) == (std::vector<std::u16string>{u"a\nb"}));
        CHECK(pieces(u"x", 1) == (std::vector<std::u16string>{u"x"}));
        const std::u16string t = u"1\n2\n3";
        Edit e = insert(carets({0, 2, 4}), pieces(u"a\nb\nc", 3));
        CHECK(applyEdit(t, e) == u"a1\nb2\nc3");
    }
}

// ------------------------------------------------------------ command palette
static void testCommandPalette() {
    using namespace CommandPalette;
    auto entry = [](const std::u16string &menu, const std::u16string &title) {
        Entry e;
        e.label = menu + u" > " + title;
        e.title = title;
        std::string id;
        for (char16_t c : e.label) id.push_back(c < 0x80 ? char(c) : '?');
        e.id = id;
        return e;
    };
    const std::vector<Entry> menu = {
        entry(u"File", u"Save"),                      // 0
        entry(u"File", u"Export PDF…"),               // 1
        entry(u"Edit", u"Toggle Comment"),            // 2
        entry(u"Edit", u"Find in Folder…"),           // 3
        entry(u"View", u"Toggle Preview"),            // 4
        entry(u"View", u"Toggle Terminal"),           // 5
        entry(u"View", u"Toggle Sidebar"),            // 6
        entry(u"View", u"Show Sizes"),                // 7
        entry(u"Terminal", u"New Terminal"),          // 8
        entry(u"Navigate", u"Go to Definition"),      // 9
        entry(u"View > Adjust Settings", u"Editor"),  // 10
    };
    auto indices = [](const std::vector<Match> &v) {
        std::vector<size_t> out;
        for (const Match &m : v) out.push_back(m.index);
        return out;
    };
    auto first = [&](const std::u16string &q, const std::vector<std::string> &recent = {}) {
        std::vector<Match> v = rank(menu, q, recent);
        return v.empty() ? size_t(999) : v[0].index;
    };

    GROUP("palette:word-starts");
    {
        const std::u16string t = u"View > Toggle Terminal";
        CHECK(wordStart(t, 0));
        CHECK(!wordStart(t, 1));
        CHECK(!wordStart(t, 5));            // '>' is no word
        CHECK(wordStart(t, 7));             // T of Toggle
        CHECK(wordStart(t, 14));            // T of Terminal
        CHECK(wordStart(u"goToDefinition", 2));   // camelCase
        CHECK(!wordStart(u"goToDefinition", 3));
        CHECK(wordStart(u"keep-sorted", 5));
        CHECK(!wordStart(t, 99));
    }

    GROUP("palette:match");
    {
        int score = 0;
        std::vector<size_t> pos;
        CHECK(match(u"tt", u"View > Toggle Terminal", &score, &pos));
        CHECK(pos == (std::vector<size_t>{7, 14}));          // both word starts
        CHECK(tierOf(u"View > Toggle Terminal", pos) == Tier::WordStarts);
        CHECK(match(u"TERM", u"View > Toggle Terminal", &score, &pos));   // case ignored
        CHECK(pos == (std::vector<size_t>{14, 15, 16, 17}));
        CHECK(tierOf(u"View > Toggle Terminal", pos) == Tier::WordStarts);
        CHECK(!match(u"xyz", u"View > Toggle Terminal", &score, &pos));
        CHECK(!match(u"terminals", u"Toggle Terminal", &score, &pos));
        CHECK(match(u"", u"anything", &score, &pos) && pos.empty() && score == 0);
        // Spaces in the query are skipped.
        CHECK(match(u"tog term", u"View > Toggle Terminal", &score, &pos));
        CHECK(pos.size() == 7 && pos[0] == 7 && pos[3] == 14);
        // Inside a word, one run: a substring.
        CHECK(match(u"erm", u"Toggle Terminal", &score, &pos));
        CHECK(tierOf(u"Toggle Terminal", pos) == Tier::Substring);
        // Scattered letters.
        CHECK(match(u"oe", u"Toggle", &score, &pos));
        CHECK(tierOf(u"Toggle", pos) == Tier::Scattered);
        // A word start and a run beat the same letters scattered.
        int a = 0, b = 0;
        CHECK(match(u"sid", u"Toggle Sidebar", &a, nullptr));
        CHECK(match(u"sid", u"Show Hidden Files", &b, nullptr));
        CHECK(a > b);
        // The alignment prefers word starts to the first occurrence.
        CHECK(match(u"gd", u"Go to Definition", &score, &pos));
        CHECK(pos == (std::vector<size_t>{0, 6}));
        // Non-ASCII text survives (no crash, compared as is).
        CHECK(match(u"pdf…", u"Export PDF…", &score, &pos) && pos.back() == 10);
    }

    GROUP("palette:rank");
    {
        // Everything for an empty query, in menu order.
        std::vector<Match> all = rank(menu, u"", {});
        CHECK(all.size() == menu.size());
        CHECK(indices(all)[0] == 0 && indices(all)[10] == 10);
        CHECK(rank(menu, u"   ", {}).size() == menu.size());
        // A title prefix comes first, even against a word-start match.
        CHECK(first(u"save") == 0);
        CHECK(first(u"new") == 8);
        CHECK(first(u"toggle t") == 5);
        // Word starts beat a substring.
        std::vector<Match> v = rank(menu, u"tp", {});
        CHECK(!v.empty() && v[0].index == 4 && v[0].tier == Tier::WordStarts);
        v = rank(menu, u"term", {});
        CHECK(v.size() >= 2);
        CHECK(v[0].index == 8 || v[0].index == 5);
        // Only matches are listed.
        v = rank(menu, u"zzz", {});
        CHECK(v.empty());
        v = rank(menu, u"gtd", {});
        CHECK(v.size() == 1 && v[0].index == 9);
        // The menu part matches too: "Adjust Settings > Editor".
        CHECK(first(u"adjust ed") == 10);
        // A title prefix highlights the title, not the menu.
        v = rank(menu, u"Sav", {});
        CHECK(v[0].positions == (std::vector<size_t>{7, 8, 9}));
        // Shorter titles first among prefixes, then the menu's order.
        v = rank(menu, u"toggle", {});
        CHECK(v.size() == 4 && v[0].index == 2 && v[0].tier == Tier::Prefix);
        CHECK(v[3].index == 5);   // Toggle Terminal, the longest
    }

    GROUP("palette:recent");
    {
        // An empty query lists recently run commands first, newest first.
        std::vector<std::string> recent = {menu[5].id, menu[9].id};
        std::vector<size_t> order = indices(rank(menu, u"", recent));
        CHECK(order[0] == 5 && order[1] == 9 && order[2] == 0);
        CHECK(order.size() == menu.size());
        // Among equal matches (here, prefixes of "toggle") recent first.
        std::vector<Match> v = rank(menu, u"toggle", {menu[6].id});
        CHECK(v[0].index == 6);
        // A better tier still wins over recency.
        v = rank(menu, u"save", {menu[3].id});
        CHECK(v[0].index == 0);
        // An id no longer in the menu is harmless.
        CHECK(rank(menu, u"", {"Gone > Away"}).size() == menu.size());

        std::vector<std::string> r = noteRun({}, "a");
        CHECK(r == (std::vector<std::string>{"a"}));
        r = noteRun(r, "b");
        r = noteRun(r, "c");
        CHECK(r == (std::vector<std::string>{"c", "b", "a"}));
        r = noteRun(r, "a");   // moves to the front, once
        CHECK(r == (std::vector<std::string>{"a", "c", "b"}));
        r = noteRun(r, "d", 3);
        CHECK(r == (std::vector<std::string>{"d", "a", "c"}));
    }

    GROUP("palette:key-labels");
    {
        CHECK(keyLabel(u"p", Command | Shift) == u"⇧⌘P");
        CHECK(keyLabel(u"P", Command) == u"⇧⌘P");   // a capital implies Shift
        CHECK(keyLabel(u"v", Command | Shift) == u"⇧⌘V");
        CHECK(keyLabel(u"f", Option | Shift) == u"⌥⇧F");
        CHECK(keyLabel(u"i", Control | Option | Shift | Command) == u"⌃⌥⇧⌘I");
        CHECK(keyLabel(u",", Command) == u"⌘,");
        CHECK(keyLabel(u"\\", Command) == u"⌘\\");
        CHECK(keyLabel(u"`", Control | Shift) == u"⌃⇧`");
        CHECK(keyLabel(u" ", Control) == u"⌃Space");
        CHECK(keyLabel(u"\r", Command | Shift) == u"⇧⌘⏎");
        CHECK(keyLabel(u"\t", Control) == u"⌃⇥");
        CHECK(keyLabel(std::u16string(1, char16_t(0xF70C)), 0) == u"F9");    // NSF9FunctionKey
        CHECK(keyLabel(std::u16string(1, char16_t(0xF70D)), Shift) == u"⇧F10");
        CHECK(keyLabel(std::u16string(1, char16_t(0xF70F)), 0) == u"F12");
        CHECK(keyLabel(std::u16string(1, char16_t(0xF701)), Command) == u"⌘↓");
        CHECK(keyLabel(std::u16string(1, char16_t(0x7F)), Command) == u"⌘⌫");
        CHECK(keyLabel(u"", Command) == u"");
        CHECK(keyLabel(u"1", Control) == u"⌃1");
    }

    GROUP("palette:modes");
    {
        Parsed p = parse(u"");
        CHECK(p.mode == Mode::Files && p.query.empty());
        p = parse(u"  main.cpp ");
        CHECK(p.mode == Mode::Files && p.query == u"main.cpp");
        p = parse(u">");
        CHECK(p.mode == Mode::Commands && p.query.empty());
        p = parse(u"> toggle term");
        CHECK(p.mode == Mode::Commands && p.query == u"toggle term");
        p = parse(u":");
        CHECK(p.mode == Mode::Line && p.lineValid && p.line == 0);
        p = parse(u":42");
        CHECK(p.mode == Mode::Line && p.lineValid && p.line == 42 && p.column == 0);
        p = parse(u": 42:7");
        CHECK(p.lineValid && p.line == 42 && p.column == 7);
        p = parse(u":42,7");
        CHECK(p.lineValid && p.line == 42 && p.column == 7);
        p = parse(u":4x");
        CHECK(p.mode == Mode::Line && !p.lineValid && p.line == 0);
        p = parse(u"::3");
        CHECK(!p.lineValid);
        p = parse(u":99999999999999");   // capped, no overflow
        CHECK(p.lineValid && p.line > 0);
    }

    GROUP("palette:list-files");
    {
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() /
            ("minicode-palette-" + std::to_string(::getpid()));
        fs::remove_all(root);
        auto touch = [&](const std::string &rel) {
            fs::create_directories((root / rel).parent_path());
            std::ofstream(root / rel) << "x";
        };
        touch("b.txt");
        touch("a.py");
        touch("src/main.cpp");
        touch("src/deep/inner.h");
        touch(".hidden");
        touch(".git/config");
        touch("node_modules/pkg/index.js");
        touch("build/out.o");
        touch("venv/bin/python");
        touch("docs/.secret/x.md");
        std::error_code ec;
        fs::create_directory_symlink(root / "src", root / "loop", ec);       // a second way in
        fs::create_directory_symlink(root, root / "src" / "up", ec);          // a loop
        bool truncated = true;
        std::vector<std::string> files = listFiles(root.string(), 100, nullptr, &truncated);
        CHECK(!truncated);
        // Breadth first: the root's files, then the next level's, then
        // deeper. Dotfiles and dot folders are listed (not .git).
        CHECK(files.size() == 6);
        CHECK(files == (std::vector<std::string>{".hidden", "a.py", "b.txt", "loop/main.cpp",
                                                 "docs/.secret/x.md", "loop/deep/inner.h"}) ||
              files == (std::vector<std::string>{".hidden", "a.py", "b.txt", "src/main.cpp",
                                                 "docs/.secret/x.md", "src/deep/inner.h"}));
        for (const std::string &f : files) {
            CHECK(f.find(".git") == std::string::npos && f.find("node_modules") == std::string::npos);
            CHECK(f.find("build/") == std::string::npos && f.find("venv/") == std::string::npos);
        }
        files = listFiles(root.string(), 3, nullptr, &truncated);
        CHECK(files.size() == 3 && truncated);
        // A .gitignore leaves its files out, unless told to read none.
        std::ofstream(root / ".gitignore") << "*.txt\ndocs/\n";
        files = listFiles(root.string(), 100, nullptr, &truncated);
        CHECK(std::find(files.begin(), files.end(), "b.txt") == files.end());
        CHECK(std::find(files.begin(), files.end(), "docs/.secret/x.md") == files.end());
        CHECK(std::find(files.begin(), files.end(), ".gitignore") != files.end());
        files = listFiles(root.string(), 100, nullptr, &truncated, false);
        CHECK(std::find(files.begin(), files.end(), "b.txt") != files.end());
        std::atomic<bool> stop(true);
        CHECK(listFiles(root.string(), 100, &stop, nullptr).empty());
        CHECK(listFiles((root / "missing").string()).empty());
        fs::remove_all(root);
    }

    GROUP("palette:rank-files");
    {
        const std::vector<std::u16string> paths = {
            u"README.md",                  // 0
            u"src/main.mm",                // 1
            u"src/EditorController.mm",    // 2
            u"src/CommandPalette.cpp",     // 3
            u"tests/run_tests.cpp",        // 4
            u"linux/src/main.cpp",         // 5
            u"docs/maintenance.md",        // 6
        };
        auto order = [&](const std::u16string &q, const std::vector<std::u16string> &recent = {}) {
            std::vector<size_t> out;
            for (const Match &m : rankFiles(paths, q, recent)) out.push_back(m.index);
            return out;
        };
        std::vector<size_t> o = order(u"");
        CHECK(o.size() == paths.size() && o[0] == 0 && o[6] == 6);
        o = order(u"", {u"tests/run_tests.cpp", u"src/main.mm"});
        CHECK(o[0] == 4 && o[1] == 1 && o[2] == 0);
        // The name prefix first, the shorter path first among equals.
        o = order(u"main");
        CHECK(o.size() >= 3 && o[0] == 1 && o[1] == 5);
        // ...unless one was opened recently.
        o = order(u"main", {u"linux/src/main.cpp"});
        CHECK(o[0] == 5 && o[1] == 1);
        // Name matches beat matches that need the folders.
        o = order(u"ec");
        CHECK(!o.empty() && o[0] == 2);   // EditorController: word starts in the name
        o = order(u"srcmain");            // only through the path
        CHECK(o.size() == 2 && (o[0] == 1 || o[0] == 5));
        // A '/' in the query matches the path.
        o = order(u"linux/");
        CHECK(o.size() == 1 && o[0] == 5);
        o = order(u"zzz");
        CHECK(o.empty());
        // Positions are in the path, the name's offset added.
        std::vector<Match> m = rankFiles(paths, u"compal", {});
        CHECK(!m.empty() && m[0].index == 3 &&
              m[0].positions == (std::vector<size_t>{4, 5, 6, 11, 12, 13}));
        // The limit.
        CHECK(rankFiles(paths, u"", {}, 2).size() == 2);
    }
}

// Cmd+D and Shift+Cmd+L: the selection's text found again, as VS Code does.
static void testMultiCaretOccurrences() {
    using namespace MultiCaret;
    auto sel = [](const std::u16string &t, const std::vector<Caret> &v) {
        std::vector<std::u16string> out;
        for (const Caret &c : v) out.push_back(t.substr(c.lo(), c.hi() - c.lo()));
        return out;
    };
    auto los = [](const std::vector<Caret> &v) {
        std::vector<size_t> out;
        for (const Caret &c : v) out.push_back(c.lo());
        return out;
    };

    GROUP("multicaret:word-at");
    {
        const std::u16string t = u"foo(bar_1, baz) é_x";
        CHECK(wordAt(t, 0) == (Caret{0, 3}));
        CHECK(wordAt(t, 2) == (Caret{0, 3}));
        CHECK(wordAt(t, 3) == (Caret{0, 3}));    // touching on the left
        CHECK(wordAt(t, 4) == (Caret{4, 9}));    // touching on the right
        CHECK(wordAt(t, 10) == (Caret{10, 10})); // between ", " and nothing
        CHECK(wordAt(t, 11) == (Caret{11, 14}));
        CHECK(wordAt(t, 16) == (Caret{16, 19})); // past ASCII counts as a letter
        CHECK(wordAt(t, 99) == (Caret{16, 19})); // clamped to the end
        CHECK(wordAt(u"", 0) == (Caret{0, 0}));
        CHECK(wordAt(u"a  b", 2) == (Caret{2, 2}));
        CHECK(wordAt(u"x\ny", 1) == (Caret{0, 1}));
    }

    GROUP("multicaret:occurrences");
    {
        const std::u16string t = u"item items item\nItem item_2 (item)";
        CHECK(los(occurrences(t, u"item", false)) ==
              (std::vector<size_t>{0, 5, 11, 21, 29}));
        CHECK(los(occurrences(t, u"item", true)) == (std::vector<size_t>{0, 11, 29}));
        CHECK(occurrences(t, u"", false).empty());
        CHECK(occurrences(t, u"nothing", false).empty());
        CHECK(los(occurrences(u"aaaa", u"aa", false)) == (std::vector<size_t>{0, 2}));
        // Punctuation at the match's edge may sit against a word.
        CHECK(los(occurrences(u"f(x) g(x)", u"(x)", true)) == (std::vector<size_t>{1, 6}));
        // Across lines.
        CHECK(los(occurrences(u"a\nb a\nb", u"a\nb", false)) == (std::vector<size_t>{0, 4}));
    }

    GROUP("multicaret:next-occurrence");
    {
        const std::u16string t = u"count = count + counter;\nprint(count)";
        // From an empty caret: the word, and whole words from then on.
        Found f = addNextOccurrence(t, {{2, 2}}, {2, 2}, false);
        CHECK(f.changed && f.wholeWord);
        CHECK(f.carets.size() == 1 && f.carets[0] == (Caret{0, 5}));
        CHECK(f.added == (Caret{0, 5}));
        // Each press adds the next one, skipping "counter".
        f = addNextOccurrence(t, f.carets, f.added, f.wholeWord);
        CHECK(f.changed && los(f.carets) == (std::vector<size_t>{0, 8}));
        f = addNextOccurrence(t, f.carets, f.added, f.wholeWord);
        CHECK(f.changed && los(f.carets) == (std::vector<size_t>{0, 8, 31}));
        CHECK(f.added == (Caret{31, 36}));
        // All taken: nothing more.
        Found g = addNextOccurrence(t, f.carets, f.added, f.wholeWord);
        CHECK(!g.changed && g.carets == f.carets);
        // Without whole words (a selection made by hand), "counter" counts.
        Found h = addNextOccurrence(t, {{8, 13}}, {8, 13}, false);
        CHECK(h.changed && !h.wholeWord && los(h.carets) == (std::vector<size_t>{8, 16}));
        // Wrapping round to the start.
        Found w = addNextOccurrence(t, {{31, 36}}, {31, 36}, true);
        CHECK(w.changed && los(w.carets) == (std::vector<size_t>{0, 31}));
        CHECK(w.added == (Caret{0, 5}));
        // Case-sensitive.
        const std::u16string u = u"Name name NAME name";
        Found c = addNextOccurrence(u, {{5, 9}}, {5, 9}, true);
        CHECK(sel(u, c.carets) == (std::vector<std::u16string>{u"name", u"name"}));
        CHECK(los(c.carets) == (std::vector<size_t>{5, 15}));
        // A caret on no word does nothing.
        Found none = addNextOccurrence(u"a  b", {{2, 2}}, {2, 2}, false);
        CHECK(!none.changed && none.carets.size() == 1);
        // Several empty carets each take their word.
        Found two = addNextOccurrence(t, {{1, 1}, {27, 27}}, {27, 27}, false);
        CHECK(sel(t, two.carets) == (std::vector<std::u16string>{u"count", u"print"}));
        CHECK(two.added == (Caret{25, 30}));
        // A stale `last` past the text's end falls back to the last caret.
        Found stale = addNextOccurrence(t, {{0, 5}}, {500, 505}, true);
        CHECK(stale.changed && los(stale.carets) == (std::vector<size_t>{0, 8}));
        // Overlapping text: the next place not already selected.
        Found ov = addNextOccurrence(u"aaaa", {{0, 2}}, {0, 2}, false);
        CHECK(ov.changed && los(ov.carets) == (std::vector<size_t>{0, 2}));
    }

    GROUP("multicaret:select-all-occurrences");
    {
        const std::u16string t = u"x = foo(x) + foobar + foo\n";
        Found f = selectAllOccurrences(t, {5, 5}, false);   // the caret in "foo"
        CHECK(f.changed && f.wholeWord);
        CHECK(los(f.carets) == (std::vector<size_t>{4, 22}));
        Found s = selectAllOccurrences(t, {4, 7}, false);   // "foo" selected by hand
        CHECK(!s.wholeWord && los(s.carets) == (std::vector<size_t>{4, 13, 22}));
        Found x = selectAllOccurrences(t, {0, 0}, false);
        CHECK(los(x.carets) == (std::vector<size_t>{0, 8}));
        Found none = selectAllOccurrences(t, {2, 2}, false);  // on "=", between blanks
        CHECK(!none.changed && none.carets.size() == 1 && none.carets[0].empty());
        // Every one of them selected, and typing replaces all.
        Edit e = insert(f.carets, {u"bar"});
        CHECK(applyEdit(t, e) == u"x = bar(x) + foobar + bar\n");
    }
}
#include "backups_tests.inc"
#include "agent_tests.inc"
#include "lsp_also_tests.inc"
#include "lsp_fixes_tests.inc"
#include "find_tests.inc"
#include "symbols_tests.inc"
#include "gutter_tests.inc"
#include "terminal_reflow_tests.inc"
#include "terminal_fuzz.inc"
#include "control_tests.inc"
#include "gitmodel_tests.inc"
#include "agent_index_tests.inc"
#include "agent_evidence_tests.inc"
#include "agent_view_tests.inc"
#include "fold_tests.inc"
#include "previews_tests.inc"
#include "commands_tests.inc"
#include "procs_tests.inc"
#include "attention_tests.inc"
#include "ignore_tests.inc"
#include "page_check_tests.inc"
#include "terminal_links_tests.inc"
#include "ide_link_tests.inc"
#include "format_consent_tests.inc"
#include "terminal_paste_tests.inc"
#include "terminal_marks_tests.inc"
#include "latex_safety_tests.inc"
#include "markdown_gfm_tests.inc"
#include "lsp_watch_tests.inc"
#include "terminal_draw_tests.inc"

int main() {
    std::printf("Running MiniCode core tests...\n");
    testSyntax();
    testSyntaxLiterals();
    testIncrementalHighlight();
    testTermLinks();
    benchIncrementalHighlight();
    testMarkdown();
    testMarkdownCommonMark();
    testMarkdownMath();
    testMathTex();
    testMarkdownLines();
    testMarkdownCorpus();
    testMarkdownEdit();
    testMarkdownGfm();
    testTerminalStream();
    testTerminalScreen();
    testTerminalReflow();
    testTerminalMouse();
    testTerminalTrust();
    testTerminalFuzz();
    testTerminalHyperlinks();
    testTerminalPaste();
    testTerminalMarks();
    testTerminalDraw();
    testSettings();
    testSettingsColorEditing();
    testLineComments();
    testLatexDoc();
    testSyncTex();
    testJson();
    testLspFraming();
    testLspPositions();
    testLspServers();
    testLspParsing();
    testLspClient();
    testLspFeatures();
    testLspEditing();
    testLspCompletionEditsAndVersions();
    testPythonEnvs();
    testLatexClicks();
    testSyncTexText();
    testFolderSearch();
    testMarkdownTasks();
    testGitStatus();
    testGitDiff();
    testGitGraph();
    testGitModel();
    testEditorTabs();
    testFormatter();
    testFormatConsent();
    testInstallHints();
    testLinuxBuildLists();
    testEditorControllerIvars();
    testLineDiff();
    testAutoIndent();
    testKeepSorted();
    testFileDetails();
    testChangeRequest();
    testMultiCaret();
    testCommandPalette();
    testMultiCaretOccurrences();
    testBackups();
    testInlineAgent();
    testLspSecondServer();
    testLspReviewFixes();
    testTextFind();
    testIgnoreFiles();
    testFolderReplace();
    testSavingToDisk();
    testSymbols();
    testLineIndex();
    testBrackets();
    testControlProtocol();
    testProcs();
    testPageCheck();
    testSideBySide();
    testAgentTranscript();
    testAgentIndex();
    testAgentEvidence();
    testAgentView();
    testFolding();
    testFileSniff();
    testCsv();
    testJsonOutline();
    testNotebook();
    testCommandKeys();
    testCommandTable();
    testKeymapFile();
    testCommandLanguages();
    testCommandsReadme();
    testClaudeHooks();
    testNotifyCommandLine();
    testAttentionBoard();
    testIdeLink();
    testIdeLinkSettings();
    testMermaidDiagram();
    testLatexSafety();
    testLspWatchedFiles();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
