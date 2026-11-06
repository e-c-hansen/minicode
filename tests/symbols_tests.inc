// symbols_tests.inc — Go to Symbol (Shift+Cmd+O, Cmd+T) and Go Back / Go
// Forward (Ctrl+-, Ctrl+Shift+-): the `lsp:symbols-*`, `palette:symbols*`,
// `outline:*` and `nav:*` groups of run_tests.cpp. Included there, after the
// fake server and the Json helpers.

namespace {
const Lsp::Symbol* findSym(const std::vector<Lsp::Symbol>& s, const std::string& name) {
    for (const Lsp::Symbol& x : s)
        if (x.name == name) return &x;
    return nullptr;
}
std::string symNames(const std::vector<Lsp::Symbol>& s) {
    std::string out;
    for (const Lsp::Symbol& x : s) {
        if (!out.empty()) out += ",";
        out += std::string((size_t)x.depth, '.') + x.name;
    }
    return out;
}
}  // namespace

static void testSymbols() {
    GROUP("lsp:symbols-parse");
    {
        // A tree (DocumentSymbol), children out of order.
        Json tree = parsed(
            "[{\"name\":\"B\",\"kind\":5,\"detail\":\"class B\","
            "\"range\":{\"start\":{\"line\":10,\"character\":0},\"end\":{\"line\":20,\"character\":0}},"
            "\"selectionRange\":{\"start\":{\"line\":10,\"character\":6},\"end\":{\"line\":10,\"character\":7}},"
            "\"children\":["
            "{\"name\":\"two\",\"kind\":6,\"range\":{\"start\":{\"line\":15,\"character\":4},"
            "\"end\":{\"line\":18,\"character\":0}},\"selectionRange\":{\"start\":{\"line\":15,"
            "\"character\":8},\"end\":{\"line\":15,\"character\":11}}},"
            "{\"name\":\"one\",\"kind\":6,\"range\":{\"start\":{\"line\":11,\"character\":4},"
            "\"end\":{\"line\":14,\"character\":0}},\"selectionRange\":{\"start\":{\"line\":11,"
            "\"character\":8},\"end\":{\"line\":11,\"character\":11}},"
            "\"children\":[{\"name\":\"inner\",\"kind\":12,\"range\":{\"start\":{\"line\":12,"
            "\"character\":8},\"end\":{\"line\":13,\"character\":0}}}]}]},"
            "{\"name\":\"A\",\"kind\":12,"
            "\"range\":{\"start\":{\"line\":1,\"character\":0},\"end\":{\"line\":5,\"character\":0}},"
            "\"selectionRange\":{\"start\":{\"line\":1,\"character\":4},\"end\":{\"line\":1,\"character\":5}}}]");
        std::vector<Lsp::Symbol> s = Lsp::parseDocumentSymbols(tree);
        CHECK(symNames(s) == "A,B,.one,..inner,.two");
        CHECK(s.size() == 5 && s[1].detail == "class B" && s[1].kind == Lsp::kSymClass);
        CHECK(s.size() == 5 && s[2].container == "B" && s[3].container == "one" &&
              s[0].container.empty());
        CHECK(s.size() == 5 && s[2].selectionRange.start == (Lsp::Position{11, 8}) &&
              s[2].range.start == (Lsp::Position{11, 4}));
        // No selectionRange: the range stands in.
        CHECK(s.size() == 5 && s[3].selectionRange.start == (Lsp::Position{12, 8}));
        CHECK(s.size() == 5 && s[0].uri.empty() && s[0].hasRange);

        // The innermost symbol at a place.
        CHECK(Lsp::symbolAt(s, {12, 9}) == 3);
        CHECK(Lsp::symbolAt(s, {16, 0}) == 4);
        CHECK(Lsp::symbolAt(s, {19, 0}) == 1);
        CHECK(Lsp::symbolAt(s, {3, 2}) == 0);
        CHECK(Lsp::symbolAt(s, {7, 0}) == -1);
        CHECK(Lsp::symbolAt({}, {0, 0}) == -1);

        // A flat list (SymbolInformation), sorted into the file's order.
        Json flat = parsed(
            "[{\"name\":\"g\",\"kind\":12,\"containerName\":\"N\",\"location\":{\"uri\":\"file:///a.c\","
            "\"range\":{\"start\":{\"line\":9,\"character\":0},\"end\":{\"line\":12,\"character\":1}}}},"
            "{\"name\":\"N\",\"kind\":3,\"location\":{\"uri\":\"file:///a.c\","
            "\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":20,\"character\":1}}}},"
            "{\"kind\":12},"
            "{\"name\":\"f\",\"kind\":12,\"containerName\":\"N\",\"location\":{\"uri\":\"file:///a.c\","
            "\"range\":{\"start\":{\"line\":2,\"character\":0},\"end\":{\"line\":5,\"character\":1}}}}]");
        s = Lsp::parseDocumentSymbols(flat);
        CHECK(symNames(s) == "N,f,g");
        CHECK(s.size() == 3 && s[1].container == "N" && s[1].uri.empty() &&
              s[1].selectionRange.start == (Lsp::Position{2, 0}));
        // At one depth, the one inside the other wins.
        CHECK(Lsp::symbolAt(s, {3, 0}) == 1);
        CHECK(Lsp::symbolAt(s, {15, 0}) == 0);
        CHECK(Lsp::parseDocumentSymbols(Json()).empty());
        CHECK(Lsp::parseDocumentSymbols(parsed("{}")).empty());
        CHECK(Lsp::parseDocumentSymbols(parsed("[]")).empty());

        // Workspace symbols: SymbolInformation and WorkspaceSymbol, whose
        // location may be only a file.
        Json ws = parsed(
            "[{\"name\":\"Server\",\"kind\":5,\"containerName\":\"net\",\"location\":{\"uri\":"
            "\"file:///p/net.py\",\"range\":{\"start\":{\"line\":3,\"character\":6},"
            "\"end\":{\"line\":3,\"character\":12}}}},"
            "{\"name\":\"serve\",\"kind\":12,\"location\":{\"uri\":\"file:///p/main.py\"}},"
            "{\"name\":\"nowhere\",\"kind\":12,\"location\":{}}]");
        s = Lsp::parseWorkspaceSymbols(ws);
        CHECK(s.size() == 2 && s[0].name == "Server" && s[0].uri == "file:///p/net.py" &&
              s[0].container == "net" && s[0].hasRange &&
              s[0].range.start == (Lsp::Position{3, 6}));
        CHECK(s.size() == 2 && s[1].name == "serve" && !s[1].hasRange);
        CHECK(Lsp::parseWorkspaceSymbols(Json()).empty());

        // Several servers: the first one's wins for the same name and place.
        Json other = parsed(
            "[{\"name\":\"Server\",\"kind\":5,\"location\":{\"uri\":\"file:///p/./net.py\","
            "\"range\":{\"start\":{\"line\":3,\"character\":6},\"end\":{\"line\":3,\"character\":12}}}},"
            "{\"name\":\"Client\",\"kind\":5,\"location\":{\"uri\":\"file:///p/net.py\","
            "\"range\":{\"start\":{\"line\":30,\"character\":6},\"end\":{\"line\":30,\"character\":12}}}}]");
        std::vector<Lsp::Symbol> m = Lsp::mergeSymbols(
            {s, Lsp::parseWorkspaceSymbols(other)}, [](const std::string &u) {
                std::string out = u;
                size_t at = out.find("/./");
                if (at != std::string::npos) out.erase(at, 2);
                return out;
            });
        CHECK(symNames(m) == "Server,serve,Client");
        CHECK(m.size() == 3 && m[0].container == "net");
        CHECK(Lsp::mergeSymbols({}).empty());

        // Kinds by name, plural, glyph and color.
        CHECK(Lsp::symbolKindName(Lsp::kSymFunction) == "function");
        CHECK(Lsp::symbolKindPlural(Lsp::kSymClass) == "classes");
        CHECK(Lsp::symbolKindPlural(Lsp::kSymProperty) == "properties");
        CHECK(Lsp::symbolKindName(0) == "symbol" && Lsp::symbolKindName(99) == "symbol");
        CHECK(Lsp::symbolKindGlyph(Lsp::kSymMethod) == u"\u0192");
        CHECK(Lsp::symbolKindColor(Lsp::kSymFunction) == 0xB180D7);
        bool all = true;
        for (int k = 0; k <= 27; k++)
            all = all && !Lsp::symbolKindGlyph(k).empty() && !Lsp::symbolKindName(k).empty();
        CHECK(all);
    }

    GROUP("lsp:symbols-client");
    {
        FakeServer srv;
        Lsp::Client client([&](const std::string &b) { srv.take(b); });
        client.initialize("/w", 1);
        const Json &caps = srv.sent[0]["params"]["capabilities"];
        CHECK(caps["textDocument"]["documentSymbol"]["hierarchicalDocumentSymbolSupport"].asBool());
        CHECK(caps["textDocument"]["documentSymbol"]["symbolKind"]["valueSet"].size() == 26);
        CHECK(caps["workspace"]["symbol"]["symbolKind"]["valueSet"][25].asInt() == 26);
        client.receive(reply(1, "{\"capabilities\":{\"documentSymbolProvider\":true,"
                                "\"workspaceSymbolProvider\":{\"resolveProvider\":false}}}"));
        CHECK(client.offers(Lsp::Feature::DocumentSymbols));
        CHECK(client.offers(Lsp::Feature::WorkspaceSymbols));
        CHECK(!Lsp::offers(parsed("{}"), Lsp::Feature::DocumentSymbols));
        CHECK(!Lsp::offers(parsed("{\"workspaceSymbolProvider\":false}"),
                           Lsp::Feature::WorkspaceSymbols));

        std::vector<Lsp::Symbol> got;
        int did = client.documentSymbols("file:///w/a.py", [&](const Json &r, const Json &) {
            got = Lsp::parseDocumentSymbols(r);
        });
        const Json &req = srv.sent.back();
        CHECK(req["method"].asString() == "textDocument/documentSymbol" &&
              req["params"]["textDocument"]["uri"].asString() == "file:///w/a.py");
        client.receive(reply(did, "[{\"name\":\"main\",\"kind\":12,\"range\":{\"start\":"
                                  "{\"line\":0,\"character\":0},\"end\":{\"line\":2,\"character\":0}}}]"));
        CHECK(got.size() == 1 && got[0].name == "main");

        std::vector<Lsp::Symbol> ws;
        int wid = client.workspaceSymbols("Serv", [&](const Json &r, const Json &) {
            ws = Lsp::parseWorkspaceSymbols(r);
        });
        CHECK(srv.sent.back()["method"].asString() == "workspace/symbol" &&
              srv.sent.back()["params"]["query"].asString() == "Serv");
        // A query grown since: the old request is cancelled, its answer dropped.
        int wid2 = client.workspaceSymbols("Server", [&](const Json &r, const Json &) {
            ws = Lsp::parseWorkspaceSymbols(r);
        });
        client.cancel(wid);
        CHECK(srv.sent.back()["method"].asString() == "$/cancelRequest" &&
              srv.sent.back()["params"]["id"].asInt() == wid);
        client.receive(reply(wid, "[{\"name\":\"Old\",\"kind\":5,\"location\":{\"uri\":\"file:///w/x.py\"}}]"));
        CHECK(ws.empty());
        client.receive(reply(wid2, "[{\"name\":\"Server\",\"kind\":5,\"location\":{\"uri\":"
                                   "\"file:///w/net.py\",\"range\":{\"start\":{\"line\":1,"
                                   "\"character\":6},\"end\":{\"line\":1,\"character\":12}}}}]"));
        CHECK(ws.size() == 1 && ws[0].name == "Server" && ws[0].uri == "file:///w/net.py");

        // Routing: the main server answers; a second one only when the main
        // one cannot.
        Json main = parsed("{\"documentSymbolProvider\":true}");
        Json ruff = parsed("{\"codeActionProvider\":true}");
        Lsp::ServerView a{true, true, &main}, b{false, true, &ruff};
        CHECK(Lsp::routeRequest(Lsp::Feature::DocumentSymbols, {a, b}) == 0);
        Lsp::ServerView c{true, true, &ruff}, d{false, true, &main};
        CHECK(Lsp::routeRequest(Lsp::Feature::DocumentSymbols, {c, d}) == 1);
        CHECK(Lsp::routeRequest(Lsp::Feature::WorkspaceSymbols, {c}) == -1);
    }

    GROUP("palette:symbol-modes");
    {
        using namespace CommandPalette;
        Parsed p = parse(u"@");
        CHECK(p.mode == Mode::Symbols && p.query.empty() && !p.byKind);
        p = parse(u"@ main ");
        CHECK(p.mode == Mode::Symbols && p.query == u"main" && !p.byKind);
        p = parse(u"@:");
        CHECK(p.mode == Mode::Symbols && p.byKind && p.query.empty());
        p = parse(u"@:ser");
        CHECK(p.mode == Mode::Symbols && p.byKind && p.query == u"ser");
        p = parse(u"#");
        CHECK(p.mode == Mode::WorkspaceSymbols && p.query.empty());
        p = parse(u"# Server");
        CHECK(p.mode == Mode::WorkspaceSymbols && p.query == u"Server");
        p = parse(u"a@b");
        CHECK(p.mode == Mode::Files && p.query == u"a@b");
    }

    GROUP("palette:symbol-rows");
    {
        using namespace CommandPalette;
        auto sym = [](const char *name, int kind, int line, int endLine, int depth = 0) {
            Lsp::Symbol s;
            s.name = name;
            s.kind = kind;
            s.depth = depth;
            s.range = {{line, 0}, {endLine, 0}};
            s.selectionRange = {{line, 4}, {line, 4 + (int)strlen(name)}};
            return s;
        };
        std::vector<Lsp::Symbol> s = {
            sym("Server", Lsp::kSymClass, 0, 30),
            sym("__init__", Lsp::kSymMethod, 1, 5, 1),
            sym("serve", Lsp::kSymMethod, 6, 20, 1),
            sym("stop", Lsp::kSymMethod, 21, 29, 1),
            sym("main", Lsp::kSymFunction, 32, 40),
            sym("PORT", Lsp::kSymConstant, 41, 41),
        };
        int pre = -2;
        std::vector<SymbolRow> rows = symbolRows(s, u"", false, 2, &pre);
        CHECK(rows.size() == 6 && pre == 2 && rows[2].symbol == 2 && !rows[0].heading);
        // The file's order with nothing typed.
        bool ordered = rows.size() == 6;
        for (size_t i = 0; ordered && i < rows.size(); i++) ordered = rows[i].symbol == i;
        CHECK(ordered);
        rows = symbolRows(s, u"", false, -1, &pre);
        CHECK(pre == 0);
        // Typing ranks by name, the caret's symbol forgotten.
        rows = symbolRows(s, u"se", false, 4, &pre);
        CHECK(rows.size() >= 2 && rows[0].symbol == 2 && rows[1].symbol == 0 && pre == 0);
        CHECK(!rows.empty() && rows[0].match.positions == (std::vector<size_t>{0, 1}));
        rows = symbolRows(s, u"zzz", false, 0, &pre);
        CHECK(rows.empty() && pre == -1);
        // Grouped by kind, the kinds in the order they first come.
        rows = symbolRows(s, u"", true, 4, &pre);
        CHECK(rows.size() == 10);
        CHECK(rows.size() == 10 && rows[0].heading && rows[0].text == u"classes (1)" &&
              rows[1].symbol == 0 && rows[2].heading && rows[2].text == u"methods (3)" &&
              rows[3].symbol == 1 && rows[6].text == u"functions (1)" && rows[7].symbol == 4 &&
              rows[8].text == u"constants (1)");
        CHECK(pre == 7);
        rows = symbolRows(s, u"s", true, -1, &pre);
        CHECK(!rows.empty() && rows[0].heading && pre == 1);
        // At most `limit`.
        rows = symbolRows(s, u"", false, -1, &pre, 3);
        CHECK(rows.size() == 3);
    }

    GROUP("outline:languages");
    CHECK(Outline::languageFor("a/README.md") == Outline::Language::Markdown);
    CHECK(Outline::languageFor("x.TEX") == Outline::Language::Latex);
    CHECK(Outline::languageFor("x.py") == Outline::Language::Python);
    CHECK(Outline::languageFor("x.hpp") == Outline::Language::CLike);
    CHECK(Outline::languageFor("x.rs") == Outline::Language::CLike);
    CHECK(Outline::languageFor("x.txt") == Outline::Language::None);
    CHECK(Outline::languageFor("Makefile") == Outline::Language::None);
    CHECK(Outline::languageFor(".md") == Outline::Language::None);
    CHECK(Outline::outline("notes.txt", u"# no").empty());

    GROUP("outline:markdown");
    {
        const std::u16string md =
            u"# Title\n"
            u"intro\n"
            u"## First part\n"
            u"```\n"
            u"# not a heading\n"
            u"```\n"
            u"### Deeper ###\n"
            u"Setext\n"
            u"------\n"
            u"text\n"
            u"# Second é\n"
            u"    # indented code\n"
            u"#nospace\n";
        std::vector<Lsp::Symbol> s = Outline::outline("x.md", md);
        CHECK(symNames(s) == "Title,.First part,..Deeper,.Setext,Second é");
        const Lsp::Symbol* first = findSym(s, "First part");
        CHECK(first && first->container == "Title" && first->kind == Lsp::kSymString &&
              first->range.start == (Lsp::Position{2, 0}) &&
              first->selectionRange.start == (Lsp::Position{2, 3}) &&
              first->selectionRange.end == (Lsp::Position{2, 13}));
        // A heading runs to the line before the next one of its level or higher.
        CHECK(first && first->range.end == (Lsp::Position{6, 14}));
        const Lsp::Symbol* title = findSym(s, "Title");
        CHECK(title && title->range.end == (Lsp::Position{9, 4}));
        const Lsp::Symbol* setext = findSym(s, "Setext");
        CHECK(setext && setext->range.start.line == 7 && setext->depth == 1);
        const Lsp::Symbol* second = findSym(s, "Second é");
        CHECK(second && second->range.end.line == 13 &&
              second->selectionRange.end == (Lsp::Position{10, 10}));
        CHECK(Lsp::symbolAt(s, {5, 0}) == 1);
        CHECK(Outline::markdown(u"").empty());
        CHECK(Outline::markdown(u"Para\n===\n").size() == 1);
    }

    GROUP("outline:latex");
    {
        const std::u16string tex =
            u"\\documentclass{article}\n"
            u"\\begin{document}\n"
            u"\\section{Intro \\emph{now}} % \\section{commented}\n"
            u"text \\subsection*[short]{Long one}\n"
            u"% \\section{All commented}\n"
            u"\\begin{verbatim}\n"
            u"\\section{Verbatim}\n"
            u"\\end{verbatim}\n"
            u"\\sectionmark{x}\n"
            u"\\section{Next}\n"
            u"\\paragraph{Para}\n"
            u"100\\% \\section{After percent}\n"
            u"\\end{document}\n";
        std::vector<Lsp::Symbol> s = Outline::outline("p.tex", tex);
        CHECK(symNames(s) == "Intro \\emph{now},.Long one,Next,.Para,After percent");
        const Lsp::Symbol* longOne = findSym(s, "Long one");
        CHECK(longOne && longOne->range.start == (Lsp::Position{3, 5}) &&
              longOne->selectionRange.start == (Lsp::Position{3, 25}) &&
              longOne->container == "Intro \\emph{now}");
        const Lsp::Symbol* intro = findSym(s, "Intro \\emph{now}");
        CHECK(intro && intro->range.end.line == 8);
    }

    GROUP("outline:python");
    {
        const std::u16string py =
            u"import os\n"
            u"\n"
            u"class Server(Base):\n"
            u"    \"\"\"A server.\n"
            u"def not_this(): pass\n"
            u"    \"\"\"\n"
            u"    def __init__(self, port,\n"
            u"  host):\n"
            u"        self.port = port  # def nope():\n"
            u"\n"
            u"    async def serve(self):\n"
            u"        def helper():\n"
            u"            return 1\n"
            u"        s = 'def nope2():'\n"
            u"# a comment at the left\n"
            u"        return helper()\n"
            u"\n"
            u"def main():\n"
            u"    x = (1,\n"
            u"def broken(): 1)\n"
            u"    return x\n"
            u"\n"
            u"if True:\n"
            u"    def inner_fn(): pass\n";
        std::vector<Lsp::Symbol> s = Outline::outline("srv.py", py);
        CHECK(symNames(s) == "Server,.__init__,.serve,..helper,main,inner_fn");
        const Lsp::Symbol* srv = findSym(s, "Server");
        CHECK(srv && srv->kind == Lsp::kSymClass && srv->range.start == (Lsp::Position{2, 0}) &&
              srv->selectionRange.start == (Lsp::Position{2, 6}) &&
              srv->selectionRange.end == (Lsp::Position{2, 12}));
        CHECK(srv && srv->range.end == (Lsp::Position{15, 23}));
        const Lsp::Symbol* init = findSym(s, "__init__");
        CHECK(init && init->kind == Lsp::kSymMethod && init->container == "Server" &&
              init->range.end.line == 8);
        const Lsp::Symbol* serve = findSym(s, "serve");
        CHECK(serve && serve->kind == Lsp::kSymMethod &&
              serve->selectionRange.start == (Lsp::Position{10, 14}));
        const Lsp::Symbol* helper = findSym(s, "helper");
        CHECK(helper && helper->kind == Lsp::kSymFunction && helper->container == "serve" &&
              helper->range.end.line == 12);
        const Lsp::Symbol* mainFn = findSym(s, "main");
        CHECK(mainFn && mainFn->depth == 0 && mainFn->range.end.line == 20);
        CHECK(Lsp::symbolAt(s, {12, 4}) == 3);
        CHECK(Outline::python(u"").empty());
        CHECK(Outline::python(u"def\n").empty());
        CHECK(Outline::python(u"def f(): pass").size() == 1);
    }

    GROUP("outline:cpp");
    {
        const std::u16string cpp =
            u"#include <vector>\n"
            u"#define MAX(a, b) { (a) > (b) }\n"
            u"namespace geo::shapes {\n"
            u"// void commented() {}\n"
            u"template <class T = int>\n"
            u"class Vec : public Base {\n"
            u"public:\n"
            u"    Vec(int n) : n_(n) {}\n"
            u"    ~Vec() { /* } */ }\n"
            u"    int size() const { return n_; }\n"
            u"    bool operator==(const Vec& o) const { return n_ == o.n_; }\n"
            u"    struct Inner { int x; };\n"
            u"private:\n"
            u"    int n_ = 0;\n"
            u"};\n"
            u"static int\n"
            u"length(const char *s,\n"
            u"       int max)\n"
            u"{\n"
            u"    if (s) { return 1; }\n"
            u"    const char *q = \"}{\";\n"
            u"    char c = '}';\n"
            u"    return 0;\n"
            u"}\n"
            u"void Vec::grow() {\n"
            u"}\n"
            u"}  // namespace\n"
            u"extern \"C\" {\n"
            u"int c_api(void) { return 0; }\n"
            u"}\n"
            u"struct Point *make_point(int x) { return 0; }\n"
            u"static const struct ops my_ops = { .open = 0 };\n"
            u"enum class Color : int { Red, Green };\n"
            u"auto lambda = [](int x) { return x; };\n"
            u"int main(int argc, char **argv) {\n"
            u"    return 0;\n"
            u"}\n";
        std::vector<Lsp::Symbol> s = Outline::outline("v.cpp", cpp);
        CHECK(symNames(s) == "geo::shapes,.Vec,..Vec,..~Vec,..size,..operator==,..Inner,"
                             ".length,.grow,c_api,make_point,Color,main");
        const Lsp::Symbol* vec = findSym(s, "Vec");
        CHECK(vec && vec->kind == Lsp::kSymClass && vec->container == "geo::shapes" &&
              vec->range.start == (Lsp::Position{5, 0}) &&
              vec->selectionRange.start == (Lsp::Position{5, 6}) &&
              vec->range.end == (Lsp::Position{14, 1}));
        CHECK(s.size() > 2 && s[2].kind == Lsp::kSymConstructor && s[2].container == "Vec");
        const Lsp::Symbol* dtor = findSym(s, "~Vec");
        CHECK(dtor && dtor->kind == Lsp::kSymMethod && dtor->range.end.line == 8);
        const Lsp::Symbol* len = findSym(s, "length");
        CHECK(len && len->kind == Lsp::kSymFunction && len->range.start == (Lsp::Position{16, 0}) &&
              len->selectionRange.start == (Lsp::Position{16, 0}) &&
              len->range.end == (Lsp::Position{23, 1}));
        const Lsp::Symbol* grow = findSym(s, "grow");
        CHECK(grow && grow->kind == Lsp::kSymMethod && grow->container == "Vec");
        const Lsp::Symbol* inner = findSym(s, "Inner");
        CHECK(inner && inner->kind == Lsp::kSymStruct && inner->depth == 2);
        const Lsp::Symbol* color = findSym(s, "Color");
        CHECK(color && color->kind == Lsp::kSymEnum);
        const Lsp::Symbol* cApi = findSym(s, "c_api");
        CHECK(cApi && cApi->depth == 0 && cApi->container.empty());
        CHECK(Lsp::symbolAt(s, {9, 20}) == 4);
    }

    GROUP("outline:other-c-family");
    {
        std::vector<Lsp::Symbol> s = Outline::outline("a.go",
            u"package main\n"
            u"import (\n"
            u"    \"fmt\"\n"
            u")\n"
            u"type Server struct {\n"
            u"    port int\n"
            u"}\n"
            u"func (s *Server) Start(ctx context.Context) error {\n"
            u"    go func() { fmt.Println(`}`) }()\n"
            u"    return nil\n"
            u"}\n"
            u"func Map[T any](xs []T) []T {\n"
            u"    return xs\n"
            u"}\n"
            u"func main() {\n"
            u"    if x := f(); x { }\n"
            u"}\n");
        CHECK(symNames(s) == "Server,Start,Map,main");
        const Lsp::Symbol* start = findSym(s, "Start");
        CHECK(start && start->kind == Lsp::kSymMethod && start->container == "Server" &&
              start->range.end.line == 10);
        CHECK(s.size() == 4 && s[0].kind == Lsp::kSymStruct);

        s = Outline::outline("a.rs",
            u"use std::fmt;\n"
            u"#[derive(Debug, Clone)]\n"
            u"pub struct Point<'a> { name: &'a str }\n"
            u"impl<'a> fmt::Display for Point<'a> {\n"
            u"    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {\n"
            u"        let c = '{';\n"
            u"        write!(f, \"{}\", self.name)\n"
            u"    }\n"
            u"}\n"
            u"mod tests {\n"
            u"    fn helper<T: Clone>(x: T) -> T { x }\n"
            u"}\n"
            u"pub fn main() {}\n");
        CHECK(symNames(s) == "Point,impl fmt::Display for Point<'a>,.fmt,tests,.helper,main");
        const Lsp::Symbol* fmt = findSym(s, "fmt");
        CHECK(fmt && fmt->kind == Lsp::kSymMethod && fmt->range.end.line == 7);
        const Lsp::Symbol* helper = findSym(s, "helper");
        CHECK(helper && helper->kind == Lsp::kSymFunction && helper->container == "tests");

        s = Outline::outline("A.java",
            u"package a.b;\n"
            u"import java.util.*;\n"
            u"@Entity(name = \"x\")\n"
            u"public class Account extends Base implements Thing {\n"
            u"    private Runnable r = new Runnable() { public void run() {} };\n"
            u"    static { init(); }\n"
            u"    public Account(String id) { this.id = id; }\n"
            u"    @Override\n"
            u"    public <T> List<T> items(Map<String, T> m) throws IOException {\n"
            u"        for (int i = 0; i < 3; i++) { }\n"
            u"        return null;\n"
            u"    }\n"
            u"    enum Kind { A, B; void f() {} }\n"
            u"}\n");
        CHECK(symNames(s) == "Account,.Account,.items,.Kind,..f");
        CHECK(s.size() == 5 && s[1].kind == Lsp::kSymConstructor && s[2].kind == Lsp::kSymMethod);

        s = Outline::outline("app.ts",
            u"import { x } from './x';\n"
            u"export const handler = async (req) => {\n"
            u"    return 1;\n"
            u"};\n"
            u"describe('suite', () => {\n"
            u"    it('works', () => { expect(1).toBe(1); });\n"
            u"});\n"
            u"export default class Store<T> {\n"
            u"    items: T[] = [];\n"
            u"    constructor(private name: string) {}\n"
            u"    async load(id: string): Promise<T> {\n"
            u"        const s = `template ${id} }`;\n"
            u"        return fetch(s);\n"
            u"    }\n"
            u"    get count() { return this.items.length; }\n"
            u"    handle = () => { };\n"
            u"}\n"
            u"function helper(a, b) { return a / b; }\n"
            u"interface Shape { area(): number; }\n"
            u"if (window.x) { }\n");
        CHECK(symNames(s) == "Store,.constructor,.load,.count,helper,Shape");
        CHECK(s.size() == 6 && s[1].kind == Lsp::kSymConstructor && s[5].kind == Lsp::kSymInterface);

        s = Outline::outline("v.swift",
            u"import Foundation\n"
            u"let x = 5\n"
            u"struct Point {\n"
            u"    var x: Int { return 1 }\n"
            u"    init(x: Int) { }\n"
            u"    func moved(by d: Int) -> Point { return self }\n"
            u"}\n"
            u"extension Point {\n"
            u"    func twice() -> Int { 2 }\n"
            u"}\n"
            u"func free() {}\n");
        CHECK(symNames(s) == "Point,.init,.moved,Point,.twice,free");
        CHECK(s.size() == 6 && s[1].kind == Lsp::kSymConstructor && s[3].kind == Lsp::kSymObject);

        s = Outline::outline("k.kt",
            u"package x\n"
            u"class Foo(\n"
            u"    val x: Int = 5\n"
            u") : Base() {\n"
            u"    fun bar(): Int {\n"
            u"        return x\n"
            u"    }\n"
            u"}\n"
            u"fun String.shout(): String { return uppercase() }\n"
            u"object Registry { fun all() = listOf<Int>() }\n");
        CHECK(symNames(s) == "Foo,.bar,shout,Registry");

        s = Outline::outline("v.m",
            u"#import <Foundation/Foundation.h>\n"
            u"@implementation View\n"
            u"- (void)drawRect:(NSRect)r {\n"
            u"    [super drawRect:r];\n"
            u"}\n"
            u"- (BOOL)tableView:(NSTableView *)t shouldSelectRow:(NSInteger)row {\n"
            u"    return YES;\n"
            u"}\n"
            u"+ (instancetype)shared { return nil; }\n"
            u"@end\n"
            u"static void helper(int x) {}\n");
        CHECK(symNames(s) == "drawRect:,tableView:shouldSelectRow:,shared,helper");
        CHECK(s.size() == 4 && s[0].kind == Lsp::kSymMethod && s[3].kind == Lsp::kSymFunction &&
              s[1].selectionRange.start == (Lsp::Position{5, 8}));

        CHECK(Outline::clike(u"").empty());
        CHECK(Outline::clike(u"}}}{{{").empty());
        CHECK(Outline::clike(u"int f() {").size() == 1);
        CHECK(Outline::clike(u"\"unterminated").empty());
    }

    GROUP("nav:history");
    {
        auto at = [](const char *path, int line, size_t loc = 0) {
            Nav::Place p;
            p.path = path;
            p.line = line;
            p.location = loc ? loc : size_t(line) * 10;
            return p;
        };
        Nav::History h;
        Nav::Place out;
        CHECK(!h.canGoBack(at("a", 0)) && !h.canGoForward());
        CHECK(!h.back(at("a", 0), &out));
        // a:5 -> b:40 (definition), then b:40 -> b:100 (a far caret move).
        h.jump(at("a", 5), at("b", 40));
        h.jump(at("b", 40), at("b", 100));
        CHECK(h.entries().size() == 3 && h.index() == 2);
        // Back from near the current entry: it is refreshed, then b:40.
        CHECK(h.canGoBack(at("b", 103)));
        CHECK(h.back(at("b", 103, 1031), &out) && out.path == "b" && out.line == 40);
        CHECK(h.entries()[2].location == 1031);   // where the caret really was
        CHECK(h.back(at("b", 41), &out) && out.path == "a" && out.line == 5);
        CHECK(!h.back(at("a", 5), &out) && h.index() == 0);
        CHECK(h.canGoForward());
        CHECK(h.forward(at("a", 6), &out) && out.path == "b" && out.line == 41);
        CHECK(h.forward(at("b", 41), &out) && out.line == 103 && out.location == 1031);
        CHECK(!h.canGoForward() && !h.forward(at("b", 103), &out));

        // A new jump from the middle drops what lay ahead.
        h.back(at("b", 103), &out);   // b:41
        h.jump(at("b", 41), at("c", 0));
        CHECK(h.entries().size() == 3 && h.entries()[2].path == "c" && !h.canGoForward());

        // Close places are one entry, the later kept.
        Nav::History m;
        m.jump(at("a", 10), at("a", 30));
        m.jump(at("a", 35), at("a", 80));   // a:35 merges into a:30
        CHECK(m.entries().size() == 3 && m.entries()[1].line == 35 && m.entries()[2].line == 80);
        m.jump(at("a", 80), at("a", 85));   // within ten lines: one entry
        CHECK(m.entries().size() == 3 && m.entries()[2].line == 85);
        CHECK(Nav::near(at("a", 0), at("a", 10)) && !Nav::near(at("a", 0), at("a", 11)) &&
              !Nav::near(at("a", 0), at("b", 0)));
        Nav::Place pic = at("p.png", 0);
        pic.hasText = false;
        CHECK(Nav::near(pic, at("p.png", 500)));

        // Back after wandering off: the place left is kept for Forward.
        Nav::History w;
        w.jump(at("a", 0), at("b", 0));
        CHECK(w.back(at("b", 200), &out) && out.path == "b" && out.line == 0);
        CHECK(w.forward(at("b", 0), &out) && out.line == 200);

        // The cap drops the oldest.
        Nav::History c(5);
        for (int i = 0; i < 20; i++) c.jump(at("f", i * 100), at("f", i * 100 + 50));
        CHECK(c.entries().size() == 5 && c.index() == 4 && c.entries()[4].line == 1950);

        // Files that move are followed, folders too; files that go are dropped.
        Nav::History r;
        r.jump(at("/p/src/a.c", 0), at("/p/b.c", 0));
        r.jump(at("/p/b.c", 0), at("/p/src/a.c", 200));
        r.jump(at("/p/src/a.c", 200), at("/p/srcx/c.c", 0));
        r.moved("/p/src", "/p/lib");
        CHECK(r.entries()[0].path == "/p/lib/a.c" && r.entries()[2].path == "/p/lib/a.c" &&
              r.entries()[3].path == "/p/srcx/c.c");
        r.moved("/p/b.c", "/p/bb.c");
        CHECK(r.entries()[1].path == "/p/bb.c");
        // b goes: a:0 and a:200 are not near, so both stay; current was c.
        r.removeIf([](const std::string &p) { return p == "/p/bb.c"; });
        CHECK(r.entries().size() == 3 && r.index() == 2 && r.entries()[2].path == "/p/srcx/c.c");
        r.removeIf([](const std::string &p) { return p == "/p/srcx/c.c"; });
        CHECK(r.entries().size() == 2 && r.index() == 1 && r.entries()[1].line == 200);
        Nav::History q;
        q.jump(at("x", 0), at("y", 0));
        q.jump(at("y", 0), at("x", 5));
        q.removeIf([](const std::string &p) { return p == "y"; });   // x:0 and x:5 merge
        CHECK(q.entries().size() == 1 && q.entries()[0].line == 5 && q.index() == 0);
        q.removeIf([](const std::string &) { return true; });
        CHECK(q.entries().empty() && q.index() == 0 && !q.canGoBack(at("x", 0)));
    }
}
