// SyntaxHighlighter.cpp — hand-rolled lexer. Pure C++ standard library only.
//
// The lexer runs one line at a time (lexLine). A line is its text up to and
// including its break ('\n', a lone '\r' or U+2029, see endsLineAfter); the
// last line has none. Nothing in a line's lexing looks past its own break,
// so a line's tokens depend only on its text and the state
// the line above ended in. SyntaxHighlighter::highlight lexes every line and
// joins the pieces of tokens that cross lines; IncrementalHighlighter keeps
// the per-line states and re-lexes only what an edit can have changed.
#include "SyntaxHighlighter.h"
#include <algorithm>
#include <cctype>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

struct SyntaxGrammar {
    std::vector<std::string> lineComments;   // e.g. {"//"} or {"#"}
    std::string blockStart;                  // e.g. "/*"  ("" = none)
    std::string blockEnd;                    // e.g. "*/"
    std::string stringDelims;                // characters that open strings
    bool tripleQuotes = false;               // Python-style """ / '''
    bool preprocHash = false;                // '#' at line start is preprocessor (C)
    bool decorators = false;                 // '@' at line start is a decorator (Python)
    bool fStrings = false;                   // Python f-strings (and t-strings): {fields} are code
    bool tex = false;                        // TeX/LaTeX: lexed by lexTexLine
    bool bibEntries = false;                 // TeX grammar, plus @article etc. (.bib)
    bool texAtLetter = false;                // .sty/.cls: '@' is always a letter
    bool jsCode = false;                     // JavaScript/TypeScript: regex literals and
                                             // template literals (lexJsLine)
    bool cppRaw = false;                     // C++ raw strings, R"d( ... )d"
    bool rust = false;                       // Rust: r#"..."#, "..." over lines, 'a lifetimes
    bool goRaw = false;                      // Go: `raw strings`, over lines
    std::string tripleChars;                 // a tripled quote opens a string over lines
    bool tripleEscapes = false;              // ... in which a backslash escapes
    bool csStrings = false;                  // C#: @"verbatim" and """raw""" strings
    bool digitQuote = false;                 // 1'000'000 is one number (C++14, C23)
    bool numUnderscore = false;              // 1_000_000 is one number
    bool longOpeners[128] = {};              // characters lexLongString may start at
    std::unordered_set<std::string> keywords;
    std::unordered_set<std::string> types;
};

namespace {

const std::unordered_set<std::string> kCLike = {
    "if","else","for","while","do","switch","case","default","break","continue",
    "return","goto","struct","class","enum","union","namespace","template",
    "typename","public","private","protected","virtual","override","static",
    "const","constexpr","inline","new","delete","this","using","typedef",
    "sizeof","operator","friend","explicit","volatile","mutable","try","catch",
    "throw","nullptr","true","false","auto","extern","register"};
const std::unordered_set<std::string> kCTypes = {
    "int","char","bool","void","float","double","long","short","unsigned",
    "signed","size_t","wchar_t","string","vector","map","uint8_t","uint32_t",
    "int64_t","std"};

const std::unordered_set<std::string> kPy = {
    "def","class","return","if","elif","else","for","while","break","continue",
    "import","from","as","with","try","except","finally","raise","pass","yield",
    "lambda","global","nonlocal","assert","del","in","is","not","and","or",
    "None","True","False","async","await","self"};
const std::unordered_set<std::string> kPyTypes = {
    "int","str","float","bool","list","dict","set","tuple","bytes","object"};

const std::unordered_set<std::string> kJs = {
    "function","return","if","else","for","while","do","switch","case","break",
    "continue","var","let","const","new","delete","typeof","instanceof","this",
    "class","extends","super","import","export","from","default","try","catch",
    "finally","throw","async","await","yield","null","undefined","true","false",
    "of","in","void"};

// Which grammar each extension gets. Several are near enough that one
// grammar serves them all: C's serves the brace languages, JavaScript's the
// JSON family (JSON Lines included), and the '#' one shell and config files.
enum class Family { C, Python, Js, Css, Hash, Ini, Lisp, Dash, Tex };

const std::unordered_map<std::string, Family>& families() {
    static const std::unordered_map<std::string, Family> m = [] {
        std::unordered_map<std::string, Family> f;
        auto add = [&f](Family fam, std::initializer_list<const char*> exts) {
            for (const char* e : exts) f[e] = fam;
        };
        add(Family::Tex, {"tex", "ltx", "latex", "sty", "cls", "bib"});
        add(Family::Python, {"py", "pyi", "pyw", "pyx", "pxd", "bzl", "star",
                             "gyp", "gypi", "sconstruct", "sconscript"});
        add(Family::Js, {"js", "mjs", "cjs", "jsx", "ts", "mts", "cts", "tsx",
                         "json", "jsonl", "ndjson", "jsonc", "json5",
                         "geojson", "topojson", "webmanifest", "har",
                         "ipynb", "babelrc", "eslintrc", "prettierrc",
                         "jshintrc", "swcrc", "as"});
        add(Family::C, {"c", "cpp", "cc", "cxx", "c++", "h", "hpp", "hxx",
                        "hh", "h++", "inl", "ipp", "tpp", "cu", "cuh", "m",
                        "mm", "java", "go", "rs", "swift", "kt", "kts",
                        "scala", "sc", "cs", "fs", "dart", "groovy",
                        "gradle", "proto", "zig", "d", "v", "sv", "svh",
                        "sol", "php", "glsl", "vert", "frag", "geom", "comp",
                        "tesc", "tese", "hlsl", "metal", "wgsl", "ino",
                        "pde", "jsonnet", "prisma",
                        "libsonnet", "thrift", "capnp", "hx", "nut", "vala",
                        "cg", "fx", "y", "yy", "l", "ll", "gn", "gni"});
        add(Family::Hash, {"sh", "bash", "zsh", "fish", "ksh", "csh", "tcsh",
                           "command", "yml", "yaml", "toml", "conf", "cfg",
                           "cnf", "env", "properties", "rb", "rake",
                           "gemspec", "ru", "pl", "pm", "t", "r", "jl", "ex",
                           "exs", "nim", "cr", "tcl", "awk", "sed", "ps1",
                           "psm1", "psd1", "nix", "tf", "tfvars", "hcl",
                           "mk", "mak", "make", "cmake", "dockerfile",
                           "containerfile", "service", "socket", "timer",
                           "mount", "target", "desktop", "gitignore",
                           "gitattributes", "gitmodules", "gitconfig",
                           "dockerignore", "npmignore", "editorconfig",
                           "zshrc", "zshenv", "zprofile", "zlogin",
                           "zlogout", "bashrc", "bash_profile",
                           "bash_logout", "profile", "inputrc", "tmux",
                           "vimrc", "pkgbuild", "ebuild", "spec", "pp",
                           "sls", "bats", "envrc", "flake8", "pylintrc",
                           "condarc", "npmrc", "yarnrc", "nanorc", "mailmap",
                           "pri", "bb", "bbappend", "graphql", "gql"});
        add(Family::Css, {"css", "scss", "less"});
        add(Family::Ini, {"ini", "reg", "inf", "iss"});
        add(Family::Lisp, {"asm", "s", "nasm", "el", "lisp", "lsp", "scm",
                           "ss", "rkt", "clj", "cljs", "cljc", "edn", "fnl"});
        add(Family::Dash, {"sql", "psql", "mysql", "pgsql", "plsql", "lua",
                           "hs", "lhs", "elm", "purs", "ada", "adb", "ads",
                           "vhd", "vhdl", "applescript", "scpt"});
        return f;
    }();
    return m;
}

SyntaxGrammar makeGrammar(const std::string& ext) {
    SyntaxGrammar d;
    auto it = families().find(ext);
    Family fam = it == families().end() ? Family::C : it->second;
    switch (fam) {
    case Family::Tex:
        d.tex = true;
        d.bibEntries = ext == "bib";
        d.texAtLetter = ext == "sty" || ext == "cls";
        break;
    case Family::Python:
        d.lineComments = {"#"}; d.stringDelims = "\"'"; d.tripleQuotes = true;
        d.decorators = true; d.fStrings = true; d.numUnderscore = true;
        d.keywords = kPy; d.types = kPyTypes;
        break;
    case Family::Js:
        d.lineComments = {"//"}; d.blockStart = "/*"; d.blockEnd = "*/";
        d.stringDelims = "\"'`"; d.keywords = kJs; d.types = kCTypes;
        d.numUnderscore = true;
        // The JSON family shares the grammar but has no regexes or templates.
        d.jsCode = ext == "js" || ext == "mjs" || ext == "cjs" || ext == "jsx" ||
                   ext == "ts" || ext == "mts" || ext == "cts" || ext == "tsx" ||
                   ext == "as";
        break;
    case Family::Css:   // plain CSS has no line comments; a URL would end in one
        if (ext != "css") d.lineComments = {"//"};
        d.blockStart = "/*"; d.blockEnd = "*/"; d.stringDelims = "\"'";
        break;
    case Family::Hash:
        d.lineComments = {"#"}; d.stringDelims = "\"'";
        break;
    case Family::Ini:
        d.lineComments = {";", "#"}; d.stringDelims = "\"";
        break;
    case Family::Lisp:
        d.lineComments = {";"}; d.stringDelims = "\"";
        break;
    case Family::Dash:
        d.lineComments = {"--"}; d.blockStart = "/*"; d.blockEnd = "*/";
        d.stringDelims = "\"'";
        break;
    case Family::C:
        d.lineComments = {"//"}; d.blockStart = "/*"; d.blockEnd = "*/";
        d.stringDelims = "\"'"; d.preprocHash = true;
        d.keywords = kCLike; d.types = kCTypes;
        if (it == families().end()) break;   // an unknown extension: plain C
        d.numUnderscore = true;
        d.rust = ext == "rs";
        d.digitQuote = !d.rust;
        d.goRaw = ext == "go";
        d.csStrings = ext == "cs";
        d.cppRaw = ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "c++" ||
                   ext == "h" || ext == "hpp" || ext == "hxx" || ext == "hh" ||
                   ext == "h++" || ext == "inl" || ext == "ipp" || ext == "tpp" ||
                   ext == "cu" || ext == "cuh" || ext == "mm" || ext == "ino";
        if (ext == "java" || ext == "swift" || ext == "dart" || ext == "groovy" ||
            ext == "gradle") {
            d.tripleChars = ext == "java" || ext == "swift" ? "\"" : "\"'";
            d.tripleEscapes = true;
        } else if (ext == "kt" || ext == "kts" || ext == "scala" || ext == "sc") {
            d.tripleChars = "\"";   // raw: a backslash is just a backslash
        }
        auto opens = [&d](const char* cs) { for (; *cs; ++cs) d.longOpeners[(int)*cs] = true; };
        if (d.cppRaw) opens("RuUL");
        if (d.rust) opens("rbc\"'");
        if (d.goRaw) opens("`");
        if (d.csStrings) opens("@\"");
        opens(d.tripleChars.c_str());
        break;
    }
    return d;
}

// Character classes, ASCII only: a byte of a UTF-8 sequence or a non-ASCII
// UTF-16 unit is never a letter or digit here, which is what the C locale's
// isalpha said of those bytes all along.
template <class Ch> inline unsigned code(Ch c) {
    return (unsigned)(typename std::make_unsigned<Ch>::type)c;
}
template <class Ch> inline bool isAlpha(Ch c) { unsigned u = code(c); return u < 128 && std::isalpha((int)u); }
template <class Ch> inline bool isDigit(Ch c) { unsigned u = code(c); return u < 128 && std::isdigit((int)u); }
template <class Ch> inline bool isAlnum(Ch c) { unsigned u = code(c); return u < 128 && std::isalnum((int)u); }
template <class Ch> inline bool isIdentStart(Ch c) { return isAlpha(c) || c == Ch('_'); }
template <class Ch> inline bool isIdentChar(Ch c)  { return isAlnum(c) || c == Ch('_'); }

// ---- line breaks
// A line ends after '\n', after a '\r' not followed by '\n' (an old Mac
// file's breaks), and after U+2029 (in UTF-8 its three bytes, E2 80 A9), as
// LineIndex, TextFind and NSTextView count lines; "\r\n" is one break. The
// lexers are written for '\n', so lexAnyLine shows them that instead.
//
// How many units before a line's start decide whether it is one: a break
// is at most this long.
template <class Ch> constexpr size_t breakReach() { return sizeof(Ch) == 1 ? 3 : 1; }

// Whether a line ends just after s[i], where s holds n units and i + 1 == n
// is the end of the text.
template <class Ch> inline bool endsLineAfter(const Ch* s, size_t n, size_t i) {
    const unsigned c = code(s[i]);
    if (c == '\n') return true;
    if (c == '\r') return i + 1 >= n || s[i + 1] != Ch('\n');
    if (sizeof(Ch) == 1)
        return c == 0xA9 && i >= 2 && code(s[i - 1]) == 0x80 && code(s[i - 2]) == 0xE2;
    return c == 0x2029;
}

// The break a line s[0, n) ends with: its length in units (2 for "\r\n"),
// and whether it is one the lexers do not know ('\r' or U+2029). The last
// line has none.
template <class Ch> inline size_t breakAtEnd(const Ch* s, size_t n, bool* foreign) {
    *foreign = false;
    if (!n) return 0;
    const unsigned c = code(s[n - 1]);
    if (c == '\n') return n >= 2 && s[n - 2] == Ch('\r') ? 2 : 1;
    *foreign = true;
    if (c == '\r') return 1;
    if (sizeof(Ch) == 1 && c == 0xA9 && n >= 3 && code(s[n - 2]) == 0x80 &&
        code(s[n - 3]) == 0xE2)
        return 3;
    if (sizeof(Ch) == 2 && c == 0x2029) return 1;
    *foreign = false;
    return 0;
}

template <class Ch>
bool matchesAt(const Ch* s, size_t n, size_t i, const std::string& tok) {
    if (tok.empty() || i + tok.size() > n) return false;
    for (size_t k = 0; k < tok.size(); ++k)
        if (code(s[i + k]) != (unsigned char)tok[k]) return false;
    return true;
}

template <class Ch>
bool isDelim(const SyntaxGrammar& d, Ch c) {
    unsigned u = code(c);
    return u < 128 && u != 0 && d.stringDelims.find((char)u) != std::string::npos;
}

// Scan a single-quoted string body from i: up to the closing quote or the end
// of the line. A backslash skips the next unit, so an escaped newline carries
// the string onto the next line. Returns the end; *continues says whether it
// ran on past this line.
template <class Ch>
size_t scanString(const Ch* s, size_t n, size_t i, Ch q, bool* continues) {
    while (i < n && s[i] != q && s[i] != Ch('\n')) {
        if (s[i] == Ch('\\') && i + 1 < n) i++;   // skip escape
        i++;
    }
    // Stopping at n with a '\n' last means the escape swallowed it.
    *continues = i >= n && n > 0 && s[n - 1] == Ch('\n');
    if (i < n && s[i] == q) i++;
    return i;
}

// Scan for the end of a triple-quoted string from i. Returns the end, or n if
// the string is still open at the end of the line.
template <class Ch>
size_t scanTriple(const Ch* s, size_t n, size_t i, Ch q, bool* closed) {
    while (i < n && !(s[i] == q && i + 2 < n && s[i+1] == q && s[i+2] == q)) i++;
    *closed = i < n;
    return *closed ? i + 3 : n;
}

// ------------------------------------------------------------------- TeX
//
// TeX has its own line lexer, since almost nothing of the C-like one fits:
//
//   \command         Keyword; preamble and definition commands (\usepackage,
//                    \newcommand, ...) Preprocessor; sectioning (\section, ...)
//                    Function. '@' counts as a letter, as in .sty files.
//   \begin{name}     \begin and \end Keyword, the name Type.
//   % to line end    Comment, but \% is a control symbol, not a comment.
//   math             $...$, $$...$$, \(...\), \[...\] and the bodies of the
//                    math environments in kTexEnvs are Number, delimiters
//                    included, with commands inside still Keyword and %
//                    comments still Comment. Math carries across lines
//                    (TexMath / TexEnv), and a blank line ends it: TeX never
//                    allows a paragraph break in math, so an unclosed $ colors
//                    only to the end of its paragraph.
//   \verb|...|       \verb Keyword, the rest String; a \verb never spans lines.
//   verbatim         The bodies of verbatim and listing environments are
//                    String, of the comment environment Comment, up to the
//                    exact \end{name}; nothing inside is read as TeX.
//   @article         In .bib files, an entry type is Preprocessor.
//
// A token only ever starts or ends at an ASCII character, so UTF-8 and UTF-16
// text give the same tokens.

enum class TexBody { Verbatim, Comment, Math };
// What may follow \begin{name} on its own line before the body starts:
// options in [...] (fancyvrb's Verbatim, lstlisting), and for minted the
// language in {...} as well. TeX reads those as arguments, not as body.
enum class TexArgs { None, Optional, OptionalThenRequired };
struct TexSpecialEnv { const char* name; TexBody body; TexArgs args = TexArgs::None; };
const TexSpecialEnv kTexEnvs[] = {
    {"verbatim", TexBody::Verbatim},   {"verbatim*", TexBody::Verbatim},
    {"Verbatim", TexBody::Verbatim, TexArgs::Optional},
    {"Verbatim*", TexBody::Verbatim, TexArgs::Optional},
    {"BVerbatim", TexBody::Verbatim, TexArgs::Optional},
    {"lstlisting", TexBody::Verbatim, TexArgs::Optional},
    {"minted", TexBody::Verbatim, TexArgs::OptionalThenRequired},
    {"comment", TexBody::Comment},
    {"equation", TexBody::Math},       {"equation*", TexBody::Math},
    {"align", TexBody::Math},          {"align*", TexBody::Math},
    {"alignat", TexBody::Math},        {"alignat*", TexBody::Math},
    {"flalign", TexBody::Math},        {"flalign*", TexBody::Math},
    {"gather", TexBody::Math},         {"gather*", TexBody::Math},
    {"multline", TexBody::Math},       {"multline*", TexBody::Math},
    {"eqnarray", TexBody::Math},       {"eqnarray*", TexBody::Math},
    {"displaymath", TexBody::Math},    {"math", TexBody::Math},
};
const size_t kTexEnvCount = sizeof(kTexEnvs) / sizeof(kTexEnvs[0]);

const std::unordered_set<std::string> kTexPreamble = {
    "documentclass","usepackage","RequirePackage","ProvidesPackage",
    "ProvidesClass","ProvidesFile","NeedsTeXFormat","LoadClass",
    "PassOptionsToPackage","PassOptionsToClass","input","include",
    "includeonly","newcommand","renewcommand","providecommand",
    "newenvironment","renewenvironment","NewDocumentCommand",
    "RenewDocumentCommand","ProvideDocumentCommand","DeclareDocumentCommand",
    "NewDocumentEnvironment","RenewDocumentEnvironment","DeclareRobustCommand",
    "DeclareMathOperator","newtheorem","newcounter","newlength","newif",
    "def","gdef","edef","xdef","let","makeatletter","makeatother",
    "bibliography","bibliographystyle","addbibresource"};
const std::unordered_set<std::string> kTexSections = {
    "part","chapter","section","subsection","subsubsection","paragraph",
    "subparagraph"};

template <class Ch> inline bool isTexLetter(Ch c) { return isAlpha(c) || c == Ch('@'); }

// The end of the control sequence at s[i] (a backslash): a run of letters for
// a control word, one ASCII character for a control symbol (\%, \\, \$), and
// just the backslash before a newline, a non-ASCII character or the end.
template <class Ch>
size_t texCommandEnd(const Ch* s, size_t n, size_t i) {
    size_t j = i + 1;
    if (j < n && isTexLetter(s[j])) {
        while (j < n && isTexLetter(s[j])) j++;
        return j;
    }
    if (j < n && code(s[j]) < 128 && s[j] != Ch('\n')) return j + 1;
    return j;
}

// True when s reads "\end{name}" at i. With `spaces`, "\end {name}" too, as
// LaTeX reads it for an ordinary environment; a verbatim body ends only at
// the exact "\end{name}", since that is all verbatim looks for.
template <class Ch>
bool texEndsEnv(const Ch* s, size_t n, size_t i, const char* name, bool spaces) {
    static const std::string head = "\\end";
    if (!matchesAt(s, n, i, head)) return false;
    size_t j = i + head.size();
    if (spaces)
        while (j < n && (s[j] == Ch(' ') || s[j] == Ch('\t'))) j++;
    if (j >= n || s[j] != Ch('{')) return false;
    j++;
    for (const char* p = name; *p; ++p, ++j)
        if (j >= n || code(s[j]) != (unsigned char)*p) return false;
    return j < n && s[j] == Ch('}');
}

// The end of a group that opens at s[i] ('[' or '{') and closes on the same
// line, nested groups included: the index just past its closing character,
// or 0 when it does not close before the line ends.
template <class Ch>
size_t texGroupEnd(const Ch* s, size_t n, size_t i, Ch open, Ch close) {
    int depth = 0;
    for (size_t k = i; k < n && s[k] != Ch('\n'); ++k) {
        if (s[k] == Ch('\\') && k + 1 < n && s[k + 1] != Ch('\n')) { ++k; continue; }
        if (s[k] == open) depth++;
        else if (s[k] == close && --depth == 0) return k + 1;
    }
    return 0;
}

template <class Ch>
LexState lexTexLine(const SyntaxGrammar& d, const Ch* s, size_t n, LexState st,
                    size_t base, std::vector<Token>& out, std::string& word) {
    auto push = [&](size_t start, size_t len, TokenStyle sty) {
        if (len) out.push_back({base + start, len, sty});
    };
    auto body = [&] { return kTexEnvs[st.quote - 1].body; };
    auto inMath = [&] {
        return st.kind == LexState::TexMath ||
               (st.kind == LexState::TexEnv && body() == TexBody::Math);
    };

    // A blank line inside math ends it (see above).
    if (inMath()) {
        size_t k = 0;
        while (k < n && (s[k] == Ch(' ') || s[k] == Ch('\t') || s[k] == Ch('\r') ||
                         s[k] == Ch('\n'))) k++;
        if (k == n) return LexState{};
    }

    size_t i = 0;
    size_t run = 0;   // where the current stretch of math began
    while (i < n) {
        // Inside a .bib entry: fields, not TeX. A '%' is text there (DOIs and
        // URLs are full of percent escapes; BibTeX has no comments inside an
        // entry), braces count until the entry closes, commands are still
        // Keyword, and $...$ on one line is still math.
        if (st.kind == LexState::BibEntry) {
            const bool paren = (st.quote & 0x8000) != 0;
            unsigned depth = st.quote & 0x7FFF;
            Ch c = s[i];
            if (c == Ch('{')) {
                if (depth < 0x7FFF) depth++;
                st.quote = (char16_t)(depth | (paren ? 0x8000 : 0));
                i++;
                continue;
            }
            if (c == Ch('}') || (paren && c == Ch(')') && depth == 1)) {
                if (depth > 0) depth--;
                st = depth == 0 ? LexState{}
                                : LexState{LexState::BibEntry,
                                           (char16_t)(depth | (paren ? 0x8000 : 0))};
                i++;
                continue;
            }
            if (c == Ch('\\')) {
                size_t e = texCommandEnd(s, n, i);
                if (e == i + 1) { i++; continue; }
                push(i, e - i, TokenStyle::Keyword);
                i = e;
                continue;
            }
            if (c == Ch('$')) {
                size_t k = i + 1;
                while (k < n && s[k] != Ch('$') && s[k] != Ch('\n')) {
                    if (s[k] == Ch('\\') && k + 1 < n && s[k + 1] != Ch('\n')) k++;
                    k++;
                }
                if (k < n && s[k] == Ch('$')) {
                    push(i, k + 1 - i, TokenStyle::Number);
                    i = k + 1;
                    continue;
                }
            }
            i++;
            continue;
        }

        // Verbatim and comment environments: everything up to \end{name}.
        if (st.kind == LexState::TexEnv && body() != TexBody::Math) {
            const char* name = kTexEnvs[st.quote - 1].name;
            size_t j = i;
            while (j < n && !texEndsEnv(s, n, j, name, false)) j++;
            push(i, j - i, body() == TexBody::Comment ? TokenStyle::Comment
                                                      : TokenStyle::String);
            if (j >= n) return st;
            i = j;
            st = LexState{};   // the \end{name} itself is lexed as usual
            continue;
        }

        Ch c = s[i];

        if (inMath()) {
            if (c == Ch('%')) {
                push(run, i - run, TokenStyle::Number);
                size_t j = i;
                while (j < n && s[j] != Ch('\n')) j++;
                push(i, j - i, TokenStyle::Comment);
                return st;
            }
            if (c == Ch('\\')) {
                if (st.kind == LexState::TexMath &&
                    (st.quote == u')' || st.quote == u']') && i + 1 < n &&
                    s[i + 1] == Ch(st.quote)) {
                    i += 2;
                    push(run, i - run, TokenStyle::Number);
                    st = LexState{};
                    continue;
                }
                if (st.kind == LexState::TexEnv &&
                    texEndsEnv(s, n, i, kTexEnvs[st.quote - 1].name, true)) {
                    push(run, i - run, TokenStyle::Number);
                    st = LexState{};
                    continue;
                }
                size_t e = texCommandEnd(s, n, i);
                if (e == i + 1) { i++; continue; }
                push(run, i - run, TokenStyle::Number);
                push(i, e - i, TokenStyle::Keyword);
                i = run = e;
                continue;
            }
            if (c == Ch('$') && st.kind == LexState::TexMath) {
                size_t close = st.quote == u'$' ? 1
                             : (st.quote == u'D' && i + 1 < n && s[i + 1] == Ch('$')) ? 2
                             : 0;
                if (close) {
                    i += close;
                    push(run, i - run, TokenStyle::Number);
                    st = LexState{};
                    continue;
                }
            }
            i++;
            continue;
        }

        // Ordinary text.
        if (c == Ch('%')) {
            size_t j = i;
            while (j < n && s[j] != Ch('\n')) j++;
            push(i, j - i, TokenStyle::Comment);
            i = j;
            continue;
        }
        if (c == Ch('$')) {
            bool dbl = i + 1 < n && s[i + 1] == Ch('$');
            st = LexState{LexState::TexMath, dbl ? u'D' : u'$'};
            run = i;
            i += dbl ? 2 : 1;
            continue;
        }
        if (c == Ch('\\')) {
            if (i + 1 < n && (s[i + 1] == Ch('(') || s[i + 1] == Ch('['))) {
                st = LexState{LexState::TexMath, s[i + 1] == Ch('(') ? u')' : u']'};
                run = i;
                i += 2;
                continue;
            }
            size_t e = texCommandEnd(s, n, i);
            if (e == i + 1) { i++; continue; }
            word.assign(e - i - 1, ' ');
            for (size_t k = i + 1; k < e; ++k) word[k - i - 1] = (char)code(s[k]);
            // \verb@x@: '@' is a letter in a command name, but not here.
            // In a package or class it is a letter everywhere, so \verb@egroup
            // is one internal command, not \verb with '@' as its delimiter.
            if (!d.texAtLetter && word.size() > 4 && word.compare(0, 5, "verb@") == 0) {
                e = i + 5;
                word = "verb";
            }
            if (word == "verb") {
                size_t j = e;
                if (j < n && s[j] == Ch('*')) j++;
                push(i, j - i, TokenStyle::Keyword);
                if (j < n && code(s[j]) < 128 && s[j] != Ch('\n') &&
                    s[j] != Ch('\r') && s[j] != Ch(' ') && s[j] != Ch('\t')) {
                    Ch dl = s[j];
                    size_t k = j + 1;
                    while (k < n && s[k] != dl && s[k] != Ch('\n')) k++;
                    if (k < n && s[k] == dl) k++;
                    push(j, k - j, TokenStyle::String);
                    j = k;
                }
                i = j;
                continue;
            }
            // A URL is read almost verbatim, so a '%' in it (a percent
            // escape) is not a comment: \url{...} and \href's first argument
            // are String, when they close on this line.
            if (word == "url" || word == "href") {
                push(i, e - i, TokenStyle::Keyword);
                i = e;
                size_t j = e;
                while (j < n && (s[j] == Ch(' ') || s[j] == Ch('\t'))) j++;
                if (j < n && s[j] == Ch('{')) {
                    size_t k = texGroupEnd(s, n, j, Ch('{'), Ch('}'));
                    if (k) {
                        push(j + 1, k - 1 - (j + 1), TokenStyle::String);
                        i = k;
                    }
                }
                continue;
            }
            if (word == "begin" || word == "end") {
                push(i, e - i, TokenStyle::Keyword);
                i = e;
                size_t j = e;
                while (j < n && (s[j] == Ch(' ') || s[j] == Ch('\t'))) j++;
                if (j < n && s[j] == Ch('{')) {
                    size_t k = j + 1;
                    bool ascii = true;
                    while (k < n && s[k] != Ch('}') && s[k] != Ch('{') &&
                           s[k] != Ch('\\') && s[k] != Ch('\n')) {
                        ascii = ascii && code(s[k]) < 128;
                        k++;
                    }
                    if (k < n && s[k] == Ch('}')) {
                        push(j + 1, k - j - 1, TokenStyle::Type);
                        if (word == "begin" && ascii) {
                            std::string name(k - j - 1, ' ');
                            for (size_t m = j + 1; m < k; ++m)
                                name[m - j - 1] = (char)code(s[m]);
                            for (size_t t = 0; t < kTexEnvCount; ++t)
                                if (name == kTexEnvs[t].name) {
                                    st = LexState{LexState::TexEnv, (char16_t)(t + 1)};
                                    break;
                                }
                        }
                        i = run = k + 1;
                        // Options and minted's language on the \begin line
                        // are arguments, not the start of the body.
                        if (st.kind == LexState::TexEnv && body() != TexBody::Math &&
                            kTexEnvs[st.quote - 1].args != TexArgs::None) {
                            auto group = [&](Ch open, Ch close) {
                                size_t p = i;
                                while (p < n && (s[p] == Ch(' ') || s[p] == Ch('\t'))) p++;
                                if (p >= n || s[p] != open) return;
                                size_t q = texGroupEnd(s, n, p, open, close);
                                if (q) i = run = q;
                            };
                            group(Ch('['), Ch(']'));
                            if (kTexEnvs[st.quote - 1].args == TexArgs::OptionalThenRequired)
                                group(Ch('{'), Ch('}'));
                        }
                    }
                }
                continue;
            }
            TokenStyle sty = kTexPreamble.count(word)  ? TokenStyle::Preprocessor
                           : kTexSections.count(word) ? TokenStyle::Function
                                                      : TokenStyle::Keyword;
            push(i, e - i, sty);
            i = e;
            continue;
        }
        if (d.bibEntries && c == Ch('@') && i + 1 < n && isAlpha(s[i + 1])) {
            size_t j = i + 1;
            while (j < n && isAlpha(s[j])) j++;
            push(i, j - i, TokenStyle::Preprocessor);
            i = j;
            // The entry's body, in braces or parentheses, is fields.
            while (j < n && (s[j] == Ch(' ') || s[j] == Ch('\t'))) j++;
            if (j < n && (s[j] == Ch('{') || s[j] == Ch('('))) {
                st = LexState{LexState::BibEntry,
                              (char16_t)(s[j] == Ch('(') ? 0x8001 : 1)};
                i = j + 1;
            }
            continue;
        }
        i++;
    }
    if (inMath()) push(run, n - run, TokenStyle::Number);
    return st;
}

// ------------------------------------------------------------ code pieces
// An identifier at s[i]: a keyword, a type (a known one, or capitalized), a
// function (followed by '('), or plain, which gets no token. Returns its end.
template <class Ch, class Push>
size_t lexIdent(const SyntaxGrammar& d, const Ch* s, size_t n, size_t i,
                std::string& word, Push& push) {
    size_t st = i;
    while (i < n && isIdentChar(s[i])) i++;
    word.assign(i - st, ' ');
    for (size_t k = st; k < i; ++k) word[k - st] = (char)code(s[k]);
    TokenStyle sty = TokenStyle::Plain;
    if (d.keywords.count(word)) sty = TokenStyle::Keyword;
    else if (d.types.count(word)) sty = TokenStyle::Type;
    else if (std::isupper((unsigned char)word[0]))
        sty = TokenStyle::Type;
    else {
        size_t j = i;
        while (j < n && (s[j] == Ch(' ') || s[j] == Ch('\t'))) j++;
        if (j < n && s[j] == Ch('(')) sty = TokenStyle::Function;
    }
    if (sty != TokenStyle::Plain) push(st, i - st, sty);
    return i;
}

template <class Ch>
bool startsNumber(const Ch* s, size_t n, size_t i) {
    return isDigit(s[i]) || (s[i] == Ch('.') && i + 1 < n && isDigit(s[i + 1]));
}

// A number: letters, digits and dots (0x1F, 3.14f, 10n), and digit
// separators where the language has them, an '_' or '\'' between two of
// those (1_000, 1'000'000, Verilog's 4'b1010), so the quote never opens a
// character literal.
template <class Ch, class Push>
size_t lexNumber(const SyntaxGrammar& d, const Ch* s, size_t n, size_t i, Push& push) {
    size_t st = i;
    while (i < n) {
        if (isAlnum(s[i]) || s[i] == Ch('.')) { i++; continue; }
        if (((d.numUnderscore && s[i] == Ch('_')) || (d.digitQuote && s[i] == Ch('\''))) &&
            i + 1 < n && isAlnum(s[i + 1])) {
            i += 2;
            continue;
        }
        break;
    }
    push(st, i - st, TokenStyle::Number);
    return i;
}

// ------------------------------------------------------------ f-strings
//
// Python's f-strings (and 3.14's t-strings) hold code: f"total {n + 1:>8}".
// The text is String, the braces of a replacement field, its !r conversion
// and its :format spec Keyword (as VS Code's Dark+ draws them), and the
// expression inside is lexed as code. {{ and }} are literal braces. An
// expression may hold strings, nested f-strings among them (3.12 lets those
// reuse the outer quote). A triple-quoted or backslash-continued f-string
// carries across lines with kFString set in its LexState's quote; a field
// left open at a line's end is not carried (the next line is string again),
// so a line's tokens still depend only on its text and the state above.
const char16_t kFString = 0x100;
const int kMaxFDepth = 8;   // nested f-strings past this are plain strings

// Is the quote at s[i] preceded by a string prefix holding f or t (f, rf,
// Fr, t, ...), with nothing of a longer word before it?
template <class Ch>
bool fPrefixBefore(const Ch* s, size_t i) {
    size_t j = i;
    while (j > 0 && i - j < 3 && isAlpha(s[j - 1])) j--;
    if (j == i || i - j > 2 || (j > 0 && isIdentChar(s[j - 1]))) return false;
    bool f = false;
    for (size_t k = j; k < i; ++k) {
        char c = (char)std::tolower((int)code(s[k]));
        if (c != 'r' && c != 'b' && c != 'u' && c != 'f' && c != 't') return false;
        f = f || c == 'f' || c == 't';
    }
    return f;
}

// Does the string opened by q (tripled or not) close at s[i]?
template <class Ch>
bool closesAt(const Ch* s, size_t n, size_t i, Ch q, bool triple) {
    if (s[i] != q) return false;
    return !triple || (i + 2 < n && s[i + 1] == q && s[i + 2] == q);
}

template <class Ch, class Push>
size_t lexStringAt(const SyntaxGrammar& d, const Ch* s, size_t n, size_t i, int depth,
                   std::string& word, Push& push, LexState* state);

// A replacement field's inside, from just after its '{'. Returns the end:
// just past its '}', or where it stopped (a line end, or the string's close
// inside a format spec).
template <class Ch, class Push>
size_t lexField(const SyntaxGrammar& d, const Ch* s, size_t n, size_t i, Ch q,
                bool triple, int depth, std::string& word, Push& push) {
    int brackets = 0;
    while (i < n) {
        Ch c = s[i];
        if (c == Ch('\n')) return i;
        if (brackets == 0 && c == Ch('}')) { push(i, 1, TokenStyle::Keyword); return i + 1; }
        if (c == Ch('(') || c == Ch('[') || c == Ch('{')) { brackets++; i++; continue; }
        if (c == Ch(')') || c == Ch(']') || c == Ch('}')) {
            if (brackets > 0) brackets--;
            i++;
            continue;
        }
        if (brackets == 0 && c == Ch('!') && !(i + 1 < n && s[i + 1] == Ch('='))) {
            size_t st = i++;   // the conversion: !r, !s, !a
            while (i < n && isIdentChar(s[i])) i++;
            push(st, i - st, TokenStyle::Keyword);
            continue;
        }
        if (brackets == 0 && c == Ch(':')) {
            // The format spec, to the field's '}', with fields of its own.
            size_t st = i;
            while (i < n && s[i] != Ch('}') && s[i] != Ch('\n') &&
                   !closesAt(s, n, i, q, triple)) {
                if (s[i] == Ch('{')) {
                    push(st, i - st, TokenStyle::Keyword);
                    push(i, 1, TokenStyle::Keyword);
                    i = lexField(d, s, n, i + 1, q, triple, depth, word, push);
                    st = i;
                    continue;
                }
                i++;
            }
            if (i < n && s[i] == Ch('}')) i++;
            push(st, i - st, TokenStyle::Keyword);
            return i;
        }
        if (isDelim(d, c)) {
            LexState ignored;
            i = lexStringAt(d, s, n, i, depth + 1, word, push, &ignored);
            continue;
        }
        if (startsNumber(s, n, i)) { i = lexNumber(d, s, n, i, push); continue; }
        if (isIdentStart(c)) { i = lexIdent(d, s, n, i, word, push); continue; }
        i++;
    }
    return i;
}

// An f-string's text from i to its close, or to the line's end. `run` is
// where the current String piece began (the prefix's quote, or the line's
// start). *open says whether it carries on to the next line.
template <class Ch, class Push>
size_t lexFBody(const SyntaxGrammar& d, const Ch* s, size_t n, size_t i, size_t run,
                Ch q, bool triple, int depth, std::string& word, Push& push, bool* open) {
    *open = false;
    while (i < n) {
        Ch c = s[i];
        if (closesAt(s, n, i, q, triple)) {
            i += triple ? 3 : 1;
            push(run, i - run, TokenStyle::String);
            return i;
        }
        if (c == Ch('\n') && !triple) {   // unterminated: it ends with the line
            push(run, i - run, TokenStyle::String);
            return i;
        }
        if (c == Ch('\\') && i + 1 < n) { i += 2; continue; }
        if (c == Ch('{')) {
            if (i + 1 < n && s[i + 1] == Ch('{')) { i += 2; continue; }
            push(run, i - run, TokenStyle::String);
            push(i, 1, TokenStyle::Keyword);
            i = lexField(d, s, n, i + 1, q, triple, depth, word, push);
            run = i;
            continue;
        }
        i++;
    }
    push(run, n - run, TokenStyle::String);
    // Past the end: a triple string is still open; a single-quoted one only
    // when a backslash swallowed the '\n' (as scanString decides).
    *open = triple || (n > 0 && s[n - 1] == Ch('\n'));
    return n;
}

// A string opening at s[i], whose character is a delimiter. Pushes its
// tokens and returns its end; *state is what the line ends in if the string
// is still open at the line's end (Normal otherwise).
template <class Ch, class Push>
size_t lexStringAt(const SyntaxGrammar& d, const Ch* s, size_t n, size_t i, int depth,
                   std::string& word, Push& push, LexState* state) {
    *state = LexState{};
    const size_t st = i;
    const Ch q = s[i];
    const bool triple = d.tripleQuotes && i + 2 < n && s[i + 1] == q && s[i + 2] == q;
    if (d.fStrings && depth < kMaxFDepth && fPrefixBefore(s, i)) {
        bool open;
        i = lexFBody(d, s, n, i + (triple ? 3 : 1), st, q, triple, depth, word, push, &open);
        if (open)
            *state = LexState{triple ? LexState::TripleString : LexState::StringCont,
                              (char16_t)(code(q) | kFString)};
        return i;
    }
    if (triple) {
        bool closed;
        i = scanTriple(s, n, i + 3, q, &closed);
        push(st, i - st, TokenStyle::String);
        if (!closed) *state = LexState{LexState::TripleString, (char16_t)q};
    } else {
        bool continues;
        i = scanString(s, n, i + 1, q, &continues);
        push(st, i - st, TokenStyle::String);
        if (continues) *state = LexState{LexState::StringCont, (char16_t)q};
    }
    return i;
}

// ------------------------------------------------------------ state texts
//
// The process-wide table behind the RawString and Template states (see
// LexState in SyntaxHighlighter.h): the same text always gets the same id.
// Ids start at 1; 0 means the table is full.
struct StateTexts {
    std::mutex lock;
    std::unordered_map<std::string, char16_t> ids;
    std::vector<std::string> texts{std::string()};
};
StateTexts& stateTexts() {
    static StateTexts* t = new StateTexts;   // never destroyed, so usable at exit too
    return *t;
}
const size_t kMaxStateTexts = 0x7FFF;   // ids fit in 15 bits (RawString uses the 16th)

char16_t stateTextId(const std::string& text) {
    StateTexts& t = stateTexts();
    std::lock_guard<std::mutex> guard(t.lock);
    auto it = t.ids.find(text);
    if (it != t.ids.end()) return it->second;
    if (t.texts.size() > kMaxStateTexts) return 0;
    char16_t id = (char16_t)t.texts.size();
    t.texts.push_back(text);
    t.ids.emplace(text, id);
    return id;
}

std::string stateText(char16_t id) {
    StateTexts& t = stateTexts();
    std::lock_guard<std::mutex> guard(t.lock);
    return id < t.texts.size() ? t.texts[id] : std::string();
}

// ------------------------------------------------------------ long strings
//
// Strings whose end is a fixed text and which may run over lines, so that
// text is all the next line needs to know (lexLongString):
//
//   C++     R"d( ... )d", also u8R, uR, UR and LR: quotes and backslashes in
//           the body are just text. d is at most 16 characters, none of them
//           a space, parenthesis or backslash; otherwise it is no raw string.
//   Rust    r"...", r#"..."# with any number of #, br and cr too; and plain
//           "...", which Rust lets run over lines. A ' opens a character
//           literal only when one closes it right after ('x', '\n',
//           '\u{1F600}', 'é'); otherwise it is a lifetime or loop label
//           ('a, 'static), Keyword, and never a string to the line's end.
//   Go      `...`, raw, over lines.
//   Java, Swift, Dart, Groovy   """...""" (Dart and Groovy ''' too), where
//           a backslash escapes; Kotlin and Scala """...""", raw.
//   C#      @"..." verbatim (a doubled "" inside reads as two strings side
//           by side, which looks the same), and """...""" raw strings,
//           closed by as many quotes as opened them.
//
// One still open at the line's end carries on as a RawString state whose
// text is the closing text, with kRawEscapes when backslashes escape.
const char16_t kRawEscapes = 0x8000;

template <class Ch>
size_t scanRaw(const Ch* s, size_t n, size_t i, const std::string& close,
               bool escapes, bool* closed) {
    while (i < n) {
        if (escapes && s[i] == Ch('\\') && i + 1 < n) { i += 2; continue; }
        if (matchesAt(s, n, i, close)) { *closed = true; return i + close.size(); }
        i++;
    }
    *closed = false;
    return n;
}

template <class Ch> inline bool isRawDelimChar(Ch c) {
    unsigned u = code(c);
    return u > 0x20 && u < 0x7F && u != '(' && u != ')' && u != '\\';
}

// A unit that continues a character: a UTF-8 byte 10xxxxxx, or a UTF-16 low
// surrogate. Skipping them reads one whole character in either encoding.
template <class Ch> inline bool isContinuation(Ch c) {
    unsigned u = code(c);
    return sizeof(Ch) == 1 ? (u & 0xC0) == 0x80 : (u >= 0xDC00 && u < 0xE000);
}

// A long string, or a Rust character literal or lifetime, at s[i]: pushes
// its token and returns its end, or returns 0 when none starts there.
// *open is the state to end the line in (Normal unless the string is open).
template <class Ch, class Push>
size_t lexLongString(const SyntaxGrammar& d, const Ch* s, size_t n, size_t i,
                     Push& push, LexState* open) {
    *open = LexState{};
    const Ch c = s[i];
    const bool wordStart = i == 0 || !isIdentChar(s[i - 1]);
    std::string close;
    bool escapes = false;
    size_t body = 0;   // where the string's text starts; 0 = no string here
    if (d.cppRaw && wordStart &&
        (c == Ch('R') || c == Ch('u') || c == Ch('U') || c == Ch('L'))) {
        size_t j = i;
        if (c == Ch('u') && j + 1 < n && s[j + 1] == Ch('8')) j += 2;
        else if (c != Ch('R')) j += 1;
        if (j + 1 < n && s[j] == Ch('R') && s[j + 1] == Ch('"')) {
            size_t k = j + 2;
            while (k < n && k - (j + 2) < 16 && isRawDelimChar(s[k])) k++;
            if (k < n && s[k] == Ch('(')) {
                close = ")";
                for (size_t m = j + 2; m < k; ++m) close += (char)code(s[m]);
                close += '"';
                body = k + 1;
            }
        }
    }
    if (d.rust) {
        size_t j = i;
        if (wordStart && (c == Ch('b') || c == Ch('c')) && j + 1 < n && s[j + 1] == Ch('r'))
            j++;
        if (wordStart && s[j] == Ch('r')) {
            size_t k = j + 1;
            while (k < n && s[k] == Ch('#') && k - j <= 255) k++;
            if (k < n && s[k] == Ch('"')) {
                close = "\"" + std::string(k - j - 1, '#');
                body = k + 1;
            }
        } else if (c == Ch('"')) {
            close = "\"";
            escapes = true;
            body = i + 1;
        } else if (c == Ch('\'')) {
            size_t k = i + 1;
            if (k < n && s[k] == Ch('\\')) {
                k += 2;   // the backslash and the character it escapes
                while (k < n && s[k] != Ch('\'') && s[k] != Ch('\n') && k - i < 14) k++;
            } else if (k < n && s[k] != Ch('\'') && s[k] != Ch('\n')) {
                k++;
                while (k < n && isContinuation(s[k])) k++;
            }
            if (k < n && k > i + 1 && s[k] == Ch('\'')) {
                push(i, k + 1 - i, TokenStyle::String);
                return k + 1;
            }
            if (i + 1 < n && isIdentStart(s[i + 1])) {
                size_t e = i + 1;
                while (e < n && isIdentChar(s[e])) e++;
                push(i, e - i, TokenStyle::Keyword);
                return e;
            }
            return i + 1;   // a stray quote
        }
    }
    if (!body && d.goRaw && c == Ch('`')) {
        close = "`";
        body = i + 1;
    }
    if (!body && d.csStrings) {
        if (c == Ch('@') && i + 1 < n && s[i + 1] == Ch('"')) {
            close = "\"";
            body = i + 2;
        } else if (c == Ch('@') && i + 2 < n && s[i + 1] == Ch('$') && s[i + 2] == Ch('"')) {
            close = "\"";
            body = i + 3;
        } else if (c == Ch('"') && i + 2 < n && s[i + 1] == c && s[i + 2] == c) {
            size_t k = i;
            while (k < n && s[k] == c && k - i < 32) k++;
            close.assign(k - i, '"');
            body = k;
        }
    }
    if (!body && !d.tripleChars.empty() && code(c) < 128 && code(c) != 0 &&
        d.tripleChars.find((char)code(c)) != std::string::npos &&
        i + 2 < n && s[i + 1] == c && s[i + 2] == c) {
        close.assign(3, (char)code(c));
        escapes = d.tripleEscapes;
        body = i + 3;
    }
    if (!body) return 0;
    bool closed;
    size_t e = scanRaw(s, n, body, close, escapes, &closed);
    push(i, e - i, TokenStyle::String);
    if (!closed) {
        char16_t id = stateTextId(close);
        if (id) *open = LexState{LexState::RawString, (char16_t)(id | (escapes ? kRawEscapes : 0))};
    }
    return e;
}

// ------------------------------------------------------------ JavaScript
//
// JavaScript and TypeScript have their own line lexer (lexJsLine), for two
// things the shared one cannot do:
//
//   /regex/flags  String, as VS Code colors them. A '/' starts one only
//                 where an expression can start: at the line's start, after
//                 an operator or ( , = : [ ! & | ? { ; =>, and after return,
//                 typeof, case and the other words in kJsBeforeRegex; never
//                 after a name, a number, a string, ) ] or } (the last also
//                 keeps JSX's {...props} /> out), nor after < (JSX's </a>).
//                 It must close on its line (after one '/' that does not,
//                 the line looks no more). Inside, a backslash escapes and
//                 a '/' in [...] does not close it. "//" and "/*" are always
//                 comments, as in JavaScript.
//   `template`    String, over lines, with ${...} fields whose ${ and } are
//                 Keyword and whose inside is code: names, nested templates,
//                 strings with braces in them, regexes, objects in braces.
//
// Templates and fields nest, so what a line ends inside is a stack: 'T' for
// a template, and for a field a character counting the braces open inside
// it (1 + that count). A line that ends with any of it open ends in a
// Template state whose text is that stack. Inside a field, only templates
// and braces are carried that way; a block comment or backslash-continued
// string open there at the line's end ends with the line.
const std::unordered_set<std::string> kJsBeforeRegex = {
    "return","typeof","instanceof","case","do","else","in","of","new","delete",
    "void","throw","yield","await"};
const size_t kMaxTemplateStack = 16;   // deeper templates and fields are plain text
const char kFieldMaxBraces = 80;       // a field's count stops here, short of 'T'

// A regex literal at s[i] ('/'): its end, flags included, or 0 when it does
// not close on this line.
template <class Ch>
size_t scanRegex(const Ch* s, size_t n, size_t i) {
    bool cls = false;
    for (size_t j = i + 1; j < n && s[j] != Ch('\n'); ++j) {
        Ch c = s[j];
        if (c == Ch('\\')) {
            if (j + 1 >= n || s[j + 1] == Ch('\n')) return 0;
            ++j;
        } else if (cls) {
            if (c == Ch(']')) cls = false;
        } else if (c == Ch('[')) {
            cls = true;
        } else if (c == Ch('/')) {
            ++j;
            while (j < n && isIdentChar(s[j])) ++j;
            return j;
        }
    }
    return 0;
}

template <class Ch>
LexState lexJsLine(const SyntaxGrammar& d, const Ch* s, size_t n, LexState in,
                   size_t base, std::vector<Token>& out, std::string& word) {
    auto push = [&](size_t start, size_t len, TokenStyle st) {
        if (len) out.push_back({base + start, len, st});
    };
    std::string stack;      // see above; the innermost is last
    size_t i = 0;
    size_t text = 0;        // where the current piece of template text began
    bool regexOk = true;    // could an expression start here?
    bool regexFailed = false;   // a '/' found no close on this line

    switch (in.kind) {
    case LexState::BlockComment:
        while (i < n && !matchesAt(s, n, i, d.blockEnd)) i++;
        if (i >= n) { push(0, n, TokenStyle::Comment); return in; }
        i += d.blockEnd.size();
        push(0, i, TokenStyle::Comment);
        break;
    case LexState::StringCont: {
        bool continues;
        i = scanString(s, n, 0, (Ch)in.quote, &continues);
        push(0, i, TokenStyle::String);
        if (continues) return in;
        regexOk = false;
        break;
    }
    case LexState::Template:
        stack = stateText(in.quote);
        break;
    default:
        break;
    }

    while (i < n) {
        if (!stack.empty() && stack.back() == 'T') {
            // Template text, up to its closing backtick or a field.
            bool left = false;
            while (i < n && !left) {
                Ch c = s[i];
                if (c == Ch('`')) {
                    i++;
                    push(text, i - text, TokenStyle::String);
                    stack.pop_back();
                    regexOk = false;
                    left = true;
                } else if (c == Ch('\\') && i + 1 < n) {
                    i += 2;
                } else if (c == Ch('$') && i + 1 < n && s[i + 1] == Ch('{') &&
                           stack.size() < kMaxTemplateStack) {
                    push(text, i - text, TokenStyle::String);
                    push(i, 2, TokenStyle::Keyword);
                    i += 2;
                    stack.push_back(1);
                    regexOk = true;
                    left = true;
                } else {
                    i++;
                }
            }
            if (!left) push(text, n - text, TokenStyle::String);
            continue;
        }

        // Code, at the top level or in a field.
        Ch c = s[i];
        if (c == Ch('\n') || c == Ch(' ') || c == Ch('\t') || c == Ch('\r')) { i++; continue; }
        if (c == Ch('/') && i + 1 < n && s[i + 1] == Ch('/')) {
            size_t st = i;
            while (i < n && s[i] != Ch('\n')) i++;
            push(st, i - st, TokenStyle::Comment);
            continue;
        }
        if (c == Ch('/') && i + 1 < n && s[i + 1] == Ch('*')) {
            size_t st = i;
            i += 2;
            while (i < n && !matchesAt(s, n, i, d.blockEnd)) i++;
            if (i >= n) {
                push(st, n - st, TokenStyle::Comment);
                if (stack.empty()) return LexState{LexState::BlockComment, 0};
                break;
            }
            i += d.blockEnd.size();
            push(st, i - st, TokenStyle::Comment);
            continue;
        }
        if (c == Ch('`') && stack.size() < kMaxTemplateStack) {
            stack.push_back('T');
            text = i++;
            continue;
        }
        if (c == Ch('"') || c == Ch('\'') || c == Ch('`')) {
            bool continues;
            size_t e = scanString(s, n, i + 1, c, &continues);
            push(i, e - i, TokenStyle::String);
            i = e;
            regexOk = false;
            if (continues && stack.empty() && c != Ch('`'))
                return LexState{LexState::StringCont, (char16_t)c};
            continue;
        }
        if (c == Ch('/') && regexOk && !regexFailed) {
            size_t e = scanRegex(s, n, i);
            if (e) {
                push(i, e - i, TokenStyle::String);
                i = e;
                regexOk = false;
                continue;
            }
            // Nothing closed it before the line's end. Stop looking on this
            // line, or a long line of (/(/(/ would be scanned once per slash.
            regexFailed = true;
        }
        if (startsNumber(s, n, i)) {
            i = lexNumber(d, s, n, i, push);
            regexOk = false;
            continue;
        }
        if (isIdentStart(c)) {
            i = lexIdent(d, s, n, i, word, push);
            regexOk = kJsBeforeRegex.count(word) > 0;
            continue;
        }
        i++;
        if (!stack.empty()) {   // in a field: count its braces
            char& top = stack.back();
            if (c == Ch('{')) {
                if (top < kFieldMaxBraces) top++;
            } else if (c == Ch('}')) {
                if (top == 1) {   // the field's own '}'
                    push(i - 1, 1, TokenStyle::Keyword);
                    stack.pop_back();
                    text = i;
                    continue;
                }
                top--;
            }
        }
        switch (code(c)) {
        case ')': case ']': case '}': case '<': case '$': case '.':
            regexOk = false;
            break;
        case '>':   // only the arrow: a > /x/ means nothing
            regexOk = i >= 2 && s[i - 2] == Ch('=');
            break;
        case '+': case '-':   // x++ / 2 is a division
            if (i < n && s[i] == c) { i++; regexOk = false; }
            else regexOk = true;
            break;
        default:
            regexOk = code(c) < 128;   // other punctuation; not a letter elsewhere
            break;
        }
    }
    if (stack.empty()) return LexState{};
    char16_t id = stateTextId(stack);
    return id ? LexState{LexState::Template, id} : LexState{};
}

// Lex one line: s[0, n), ending in '\n' unless it is the last line. `in` is
// the state the line above ended in. Tokens go to out at offset `base`.
// Returns the state this line ends in.
template <class Ch>
LexState lexLine(const SyntaxGrammar& d, const Ch* s, size_t n, LexState in,
                 size_t base, std::vector<Token>& out, std::string& word) {
    if (d.tex) return lexTexLine(d, s, n, in, base, out, word);
    if (d.jsCode) return lexJsLine(d, s, n, in, base, out, word);
    size_t i = 0;
    bool atLineStart = true;
    auto push = [&](size_t start, size_t len, TokenStyle st) {
        if (len) out.push_back({base + start, len, st});
    };

    // Finish whatever the line above left open.
    switch (in.kind) {
    case LexState::BlockComment: {
        while (i < n && !matchesAt(s, n, i, d.blockEnd)) i++;
        if (i >= n) { push(0, n, TokenStyle::Comment); return in; }
        i += d.blockEnd.size();
        push(0, i, TokenStyle::Comment);
        atLineStart = false;
        break;
    }
    case LexState::TripleString:
    case LexState::StringCont:
        if (in.quote & kFString) {
            bool open;
            i = lexFBody(d, s, n, 0, 0, (Ch)(in.quote & 0xFF),
                         in.kind == LexState::TripleString, 0, word, push, &open);
            if (open) return in;
            atLineStart = false;
            break;
        }
        if (in.kind == LexState::StringCont) {
            bool continues;
            i = scanString(s, n, 0, (Ch)in.quote, &continues);
            push(0, i, TokenStyle::String);
            if (continues) return in;
        } else {
            bool closed;
            i = scanTriple(s, n, 0, (Ch)in.quote, &closed);
            push(0, i, TokenStyle::String);
            if (!closed) return in;
        }
        atLineStart = false;
        break;
    case LexState::RawString: {
        bool closed;
        i = scanRaw(s, n, 0, stateText((char16_t)(in.quote & ~kRawEscapes)), (in.quote & kRawEscapes) != 0,
                    &closed);
        push(0, i, TokenStyle::String);
        if (!closed) return in;
        atLineStart = false;
        break;
    }
    default:
        break;
    }

    while (i < n) {
        Ch c = s[i];

        if (c == Ch('\n')) { i++; atLineStart = true; continue; }
        if (c == Ch(' ') || c == Ch('\t') || c == Ch('\r')) { i++; continue; }

        // Preprocessor / decorators at line start.
        if (atLineStart && ((d.preprocHash && c == Ch('#')) ||
                            (d.decorators && c == Ch('@')))) {
            size_t st = i;
            while (i < n && s[i] != Ch('\n')) i++;
            push(st, i - st, TokenStyle::Preprocessor);
            atLineStart = false;
            continue;
        }

        // Line comments.
        bool handled = false;
        for (const auto& lc : d.lineComments) {
            if (matchesAt(s, n, i, lc)) {
                size_t st = i;
                while (i < n && s[i] != Ch('\n')) i++;
                push(st, i - st, TokenStyle::Comment);
                handled = true; break;
            }
        }
        if (handled) { atLineStart = false; continue; }

        // Block comments.
        if (matchesAt(s, n, i, d.blockStart)) {
            size_t st = i;
            i += d.blockStart.size();
            while (i < n && !matchesAt(s, n, i, d.blockEnd)) i++;
            if (i >= n) {
                push(st, n - st, TokenStyle::Comment);
                return LexState{LexState::BlockComment, 0};
            }
            i += d.blockEnd.size();
            push(st, i - st, TokenStyle::Comment);
            atLineStart = false;
            continue;
        }

        // Raw strings and the other kinds in lexLongString.
        if (code(c) < 128 && d.longOpeners[code(c)]) {
            LexState open;
            size_t e = lexLongString(d, s, n, i, push, &open);
            if (e) {
                if (open.kind != LexState::Normal) return open;
                i = e;
                atLineStart = false;
                continue;
            }
        }

        // Strings.
        if (isDelim(d, c)) {
            LexState open;
            i = lexStringAt(d, s, n, i, 0, word, push, &open);
            if (open.kind != LexState::Normal) return open;
            atLineStart = false;
            continue;
        }

        // Numbers.
        if (startsNumber(s, n, i)) {
            i = lexNumber(d, s, n, i, push);
            atLineStart = false;
            continue;
        }

        // Identifiers / keywords.
        if (isIdentStart(c)) {
            i = lexIdent(d, s, n, i, word, push);
            atLineStart = false;
            continue;
        }

        i++;
        atLineStart = false;
    }
    return LexState{};
}

// lexLine for any line of a text: s[0, n) is the line with its break, which
// may be changed while it is read (the caller owns it; it is put back).
// A '\r' or U+2029 break is read as '\n', and U+2029's other two UTF-8
// bytes go to the token ending there, so the lexers see the line end they
// were written for and tokens still cover the whole line. A line longer than
// kMaxTokenizationLineLength (its break not counted) is not lexed: no tokens,
// and it ends in the state it started in, as VS Code does past
// editor.maxTokenizationLineLength.
template <class Ch>
LexState lexAnyLine(const SyntaxGrammar& d, Ch* s, size_t n, LexState in, size_t base,
                    std::vector<Token>& out, std::string& word) {
    bool foreign;
    const size_t br = breakAtEnd(s, n, &foreign);
    if (n - br > kMaxTokenizationLineLength) return in;
    if (!foreign) return lexLine(d, (const Ch*)s, n, in, base, out, word);
    const size_t at = n - br;          // where the break starts
    const Ch was = s[at];
    s[at] = Ch('\n');
    const size_t before = out.size();
    LexState st = lexLine(d, (const Ch*)s, at + 1, in, base, out, word);
    s[at] = was;
    if (br > 1)
        for (size_t k = before; k < out.size(); ++k)
            if (out[k].start + out[k].length == base + at + 1) out[k].length += br - 1;
    return st;
}

} // namespace

bool SyntaxHighlighter::supports(const std::string& ext) {
    return families().count(ext) > 0;
}

std::vector<Token> SyntaxHighlighter::highlight(const std::string& text,
                                                const std::string& ext) {
    std::vector<Token> out;
    SyntaxGrammar d = makeGrammar(ext);
    std::string word, line;
    LexState st;
    size_t ls = 0;
    const size_t n = text.size();
    const char* t = text.data();
    while (true) {
        size_t le = ls;
        while (le < n && !endsLineAfter(t, n, le)) le++;
        if (le < n) le++;
        size_t before = out.size();
        LexState in = st;
        line.assign(t + ls, le - ls);   // lexAnyLine may write to its line
        st = lexAnyLine(d, &line[0], line.size(), in, ls, out, word);
        // A line that starts inside a token continues the previous line's
        // last piece: join them, so a multi-line comment is one token. (In
        // TeX math the line may start with a command instead, not joined.)
        if (in.kind != LexState::Normal && out.size() > before && before > 0 &&
            out[before].start == ls && out[before].style == out[before - 1].style &&
            out[before - 1].start + out[before - 1].length == ls) {
            out[before - 1].length += out[before].length;
            out.erase(out.begin() + before);
        }
        if (le >= n) break;
        ls = le;
    }
    return out;
}

// ------------------------------------------------------ IncrementalHighlighter

template <class Ch>
IncrementalHighlighter<Ch>::IncrementalHighlighter(const std::string& ext)
    : def_(new SyntaxGrammar(makeGrammar(ext))) {
    starts_.assign(1, 0);
    states_.assign(1, LexState{});
}

template <class Ch>
IncrementalHighlighter<Ch>::~IncrementalHighlighter() = default;

template <class Ch>
void IncrementalHighlighter<Ch>::reset(const TextSource<Ch>& src,
                                       std::vector<Token>* out) {
    length_ = src.length();
    std::vector<Ch> text(length_);
    if (length_) src.read(0, length_, text.data());
    starts_.assign(1, 0);
    for (size_t i = 0; i < length_; ++i)
        if (endsLineAfter(text.data(), length_, i)) starts_.push_back(i + 1);
    states_.assign(starts_.size(), LexState{});

    std::vector<Token> scratch;
    std::vector<Token>& sink = out ? *out : scratch;
    std::string word;
    LexState st;
    for (size_t k = 0; k < starts_.size(); ++k) {
        size_t a = starts_[k];
        st = lexAnyLine(*def_, text.data() + a, lineEnd(k) - a, st, a, sink, word);
        states_[k] = st;
        if (!out) scratch.clear();
    }
}

template <class Ch>
size_t IncrementalHighlighter<Ch>::relex(const TextSource<Ch>& src, size_t first,
                                         size_t mustReach, std::vector<Token>& out) {
    std::string word;
    LexState st = first == 0 ? LexState{} : states_[first - 1];
    size_t k = first;
    for (; k < starts_.size(); ++k) {
        size_t a = starts_[k], len = lineEnd(k) - a;
        // A line too long to lex is not even read (lexAnyLine would give
        // back the state it started in).
        if (len <= kMaxTokenizationLineLength + breakReach<Ch>()) {
            buf_.resize(len);
            if (len) src.read(a, len, buf_.data());
            st = lexAnyLine(*def_, buf_.data(), len, st, a, out, word);
        }
        bool same = st == states_[k];
        states_[k] = st;
        // Past the edit, a line that ends the way it used to leaves every
        // line below it exactly as it was.
        if (k >= mustReach && same) { ++k; break; }
    }
    if (buf_.capacity() > 1 << 16) std::vector<Ch>().swap(buf_);
    return k;
}

template <class Ch>
void IncrementalHighlighter<Ch>::lineTokens(const TextSource<Ch>& src, size_t first,
                                            size_t end, std::vector<Token>& out) {
    std::string word;
    end = std::min(end, starts_.size());
    for (size_t k = first; k < end; ++k) {
        size_t a = starts_[k], len = lineEnd(k) - a;
        if (len > kMaxTokenizationLineLength + breakReach<Ch>()) continue;   // never lexed
        buf_.resize(len);
        if (len) src.read(a, len, buf_.data());
        lexAnyLine(*def_, buf_.data(), len, k == 0 ? LexState{} : states_[k - 1], a, out,
                   word);
    }
    if (buf_.capacity() > 1 << 16) std::vector<Ch>().swap(buf_);
}

template <class Ch>
TokenStyle IncrementalHighlighter<Ch>::styleIfInserted(const TextSource<Ch>& src, size_t pos,
                                                       Ch c) {
    if (pos > length_) return TokenStyle::Plain;
    const size_t k = lineOf(pos);
    const size_t a = starts_[k], len = lineEnd(k) - a;
    if (len + 1 > kMaxTokenizationLineLength + breakReach<Ch>()) return TokenStyle::Plain;
    buf_.resize(len + 1);
    if (pos > a) src.read(a, pos - a, buf_.data());
    buf_[pos - a] = c;
    if (a + len > pos) src.read(pos, a + len - pos, buf_.data() + (pos - a) + 1);
    std::vector<Token> tokens;
    std::string word;
    lexAnyLine(*def_, buf_.data(), len + 1, k == 0 ? LexState{} : states_[k - 1], a, tokens,
               word);
    for (const Token& t : tokens)
        if (t.start <= pos && pos < t.start + t.length) return t.style;
    return TokenStyle::Plain;
}

template <class Ch>
size_t IncrementalHighlighter<Ch>::lineOf(size_t pos) const {
    return (size_t)(std::upper_bound(starts_.begin(), starts_.end(), pos) -
                    starts_.begin()) - 1;
}

template <class Ch>
typename IncrementalHighlighter<Ch>::Range
IncrementalHighlighter<Ch>::edit(const TextSource<Ch>& src, size_t pos,
                                 size_t oldLen, size_t newLen,
                                 std::vector<Token>& out) {
    const size_t total = src.length();
    if (pos > length_ || oldLen > length_ - pos ||
        total != length_ - oldLen + newLen) {
        // The edit does not describe how we got here; start over.
        reset(src, &out);
        Range r;
        r.firstLine = 0; r.endLine = starts_.size();
        r.start = 0; r.end = length_;
        return r;
    }

    // Whether a line starts at s depends on the units [s - reach, s] (a
    // '\r' ends a line only when no '\n' follows it; U+2029 is three bytes
    // in UTF-8). So the starts an edit can change are those in
    // [pos, pos + oldLen + reach - 1] before it, which go, and those in
    // [pos, pos + newLen + reach - 1] after it, found again (line 0 always
    // starts at 0). Line a, the one before the first that goes, holds the
    // edit's start.
    const size_t reach = breakReach<Ch>();
    const size_t from = std::max<size_t>(pos, 1);
    auto firstGone = std::lower_bound(starts_.begin(), starts_.end(), from);
    auto keep = std::upper_bound(firstGone, starts_.end(), pos + oldLen + reach - 1);
    const size_t a = (size_t)(firstGone - starts_.begin()) - 1;
    const size_t removed = (size_t)(keep - firstGone);
    // The last edited line ends where the last touched old line ended, so it
    // is compared against that line's old end state.
    const LexState tailState = states_[a + removed];

    // Line starts in the new text's window.
    std::vector<size_t> added;
    {
        const size_t last = std::min(pos + newLen + reach - 1, total);   // the last start that may change
        if (from <= last) {
            const size_t lo = from > reach ? from - reach : 0;
            const size_t hi = std::min(total, last + 1);
            // The unit after each one looked at is in the window, so the
            // window's end is taken for the text's only where it is.
            std::vector<Ch> win(hi - lo);
            if (hi > lo) src.read(lo, hi - lo, win.data());
            for (size_t st = from; st <= last; ++st)
                if (endsLineAfter(win.data(), win.size(), st - 1 - lo)) added.push_back(st);
        }
    }

    // Shift the untouched lines below, then splice the edited ones in.
    for (auto it = keep; it != starts_.end(); ++it) *it = *it - oldLen + newLen;
    size_t gi = (size_t)(firstGone - starts_.begin());
    if (added.size() < removed)
        starts_.erase(starts_.begin() + gi, starts_.begin() + gi + (removed - added.size()));
    else if (added.size() > removed)
        starts_.insert(starts_.begin() + gi, added.size() - removed, 0);
    std::copy(added.begin(), added.end(), starts_.begin() + gi);

    // States [a, a + removed] become added.size() + 1 fresh ones, the last
    // the old tail's. In place where the counts allow: typing on one line
    // (none removed, none added) moved the whole table twice per edit, which
    // an edit at each of thousands of carets paid thousands of times.
    const size_t oldCount = removed + 1, newCount = added.size() + 1;
    if (newCount < oldCount)
        states_.erase(states_.begin() + a, states_.begin() + a + (oldCount - newCount));
    else if (newCount > oldCount)
        states_.insert(states_.begin() + a, newCount - oldCount, LexState{LexState::Unknown, 0});
    for (size_t k = 0; k + 1 < newCount; ++k) states_[a + k] = LexState{LexState::Unknown, 0};
    states_[a + newCount - 1] = tailState;
    length_ = total;

    Range r;
    r.firstLine = a;
    r.endLine = relex(src, a, a + added.size(), out);
    r.start = starts_[a];
    r.end = lineEnd(r.endLine - 1);
    return r;
}

template class IncrementalHighlighter<char>;
template class IncrementalHighlighter<char16_t>;

template <class Ch>
bool lineIsLexed(const TextSource<Ch>& src, size_t start, size_t end) {
    if (end <= start) return true;
    const size_t len = end - start;
    if (len <= kMaxTokenizationLineLength) return true;
    if (len > kMaxTokenizationLineLength + 3) return false;
    Ch tail[3];
    const size_t k = std::min<size_t>(3, len);
    src.read(end - k, k, tail);
    bool foreign;
    return len - breakAtEnd(tail, k, &foreign) <= kMaxTokenizationLineLength;
}

template bool lineIsLexed<char>(const TextSource<char>&, size_t, size_t);
template bool lineIsLexed<char16_t>(const TextSource<char16_t>&, size_t, size_t);
