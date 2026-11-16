// runnables_tests.inc — the `runnables:*` groups of run_tests.cpp: which
// files are test files, the tests found in them (pytest, go test, cargo
// test, Jest and Vitest; nothing in comments or strings), and the commands
// that run one test or a whole file (Runnables.h). Included there.
#include "Runnables.h"

namespace {

std::string rnLabels(const std::vector<Runnables::Test>& tests, Runnables::Kind k) {
    std::string out;
    for (const Runnables::Test& t : tests) {
        if (!out.empty()) out += " | ";
        out += std::to_string(t.line + 1) + ":" + t.label(k);
    }
    return out;
}

// A request whose filesystem is the paths listed.
Runnables::Request rnRequest(const std::string& file, const std::string& root,
                             std::vector<std::string> present) {
    Runnables::Request r;
    r.file = file;
    r.root = root;
    r.exists = [present](const std::string& p) {
        return std::find(present.begin(), present.end(), p) != present.end();
    };
    return r;
}

std::string rnCommand(const Runnables::Request& r, Runnables::Kind k,
                      const std::vector<Runnables::Test>& tests, bool whole,
                      std::string* dir = nullptr) {
    Runnables::Command c;
    std::string problem;
    if (!Runnables::command(r, k, tests, whole, &c, &problem)) return "ERROR " + problem;
    if (dir) *dir = c.dir;
    return c.line;
}

}  // namespace

static void testRunnables() {
    using Runnables::Kind;
    GROUP("runnables:kinds");
    CHECK(Runnables::kindFor("/p/test_math.py") == Kind::Pytest);
    CHECK(Runnables::kindFor("/p/math_test.py") == Kind::Pytest);
    CHECK(Runnables::kindFor("/p/app/tests.py") == Kind::Pytest);
    CHECK(Runnables::kindFor("/p/math.py") == Kind::None);
    CHECK(Runnables::kindFor("/p/testing.py") == Kind::None);
    CHECK(Runnables::kindFor("/p/pkg/x_test.go") == Kind::Go);
    CHECK(Runnables::kindFor("/p/pkg/x.go") == Kind::None);
    CHECK(Runnables::kindFor("/p/src/lib.rs") == Kind::Rust);
    CHECK(Runnables::kindFor("/p/sum.test.ts") == Kind::Js);
    CHECK(Runnables::kindFor("/p/sum.spec.jsx") == Kind::Js);
    CHECK(Runnables::kindFor("/p/__tests__/sum.js") == Kind::Js);
    CHECK(Runnables::kindFor("/p/sum.ts") == Kind::None);
    CHECK(Runnables::kindFor("/p/notes.txt") == Kind::None);
    CHECK(Runnables::languageOf(Kind::Js) == "js");

    GROUP("runnables:pytest");
    {
        const std::string py =
            "import pytest\n"                                   // 1
            "\n"                                                // 2
            "def test_adds():\n"                                // 3
            "    assert 1 + 1 == 2\n"                           // 4
            "\n"                                                // 5
            "async def test_waits():\n"                         // 6
            "    def test_inner():  # not collected\n"          // 7
            "        pass\n"                                    // 8
            "\n"                                                // 9
            "# def test_commented():\n"                         // 10
            "TEXT = '''\n"                                      // 11
            "def test_in_a_string():\n"                         // 12
            "'''\n"                                             // 13
            "\n"                                                // 14
            "class TestParser:\n"                               // 15
            "    @pytest.mark.parametrize('x', [\n"             // 16
            "1, 2])\n"                                          // 17
            "    def test_parse(self, x):\n"                    // 18
            "        pass\n"                                    // 19
            "\n"                                                // 20
            "    class TestNested:\n"                           // 21
            "        def test_deep(self):\n"                    // 22
            "            pass\n"                                // 23
            "\n"                                                // 24
            "    def helper(self):\n"                           // 25
            "        pass\n"                                    // 26
            "\n"                                                // 27
            "class Helper:\n"                                   // 28
            "    def test_not_a_test_class(self):\n"            // 29
            "        pass\n"                                    // 30
            "\n"                                                // 31
            "class ParserTests(unittest.TestCase):\n"           // 32
            "    def test_case(self):\n"                        // 33
            "        pass\n"                                    // 34
            "def test_last(): pass\n";                          // 35
        const auto tests = Runnables::find(py, Kind::Pytest);
        CHECK(rnLabels(tests, Kind::Pytest) ==
              "3:test_adds | 6:test_waits | 18:TestParser::test_parse | "
              "22:TestParser::TestNested::test_deep | 33:ParserTests::test_case | 35:test_last");
        if (tests.size() != 6) return;
        // Line breaks as the editor counts them: \r\n, a lone \r, U+2029.
        const auto crlf = Runnables::find("x = 1\r\ndef test_a():\r  pass\xE2\x80\xA9" "def test_b(): pass\n",
                                          Kind::Pytest);
        CHECK(rnLabels(crlf, Kind::Pytest) == "2:test_a | 4:test_b");

        // One test: a node id from the project's folder, run there.
        auto r = rnRequest("/w/proj/tests/test_math.py", "/w", {"/w/proj/pyproject.toml"});
        r.python = "/w/proj/.venv/bin/python";
        std::string dir;
        CHECK(rnCommand(r, Kind::Pytest, {tests[2]}, false, &dir) ==
              "/w/proj/.venv/bin/python -m pytest tests/test_math.py::TestParser::test_parse");
        CHECK(dir == "/w/proj");
        CHECK(rnCommand(r, Kind::Pytest, tests, true) ==
              "/w/proj/.venv/bin/python -m pytest tests/test_math.py");
        // No marker: the window's folder.
        auto bare = rnRequest("/w/a b/test_x.py", "/w", {});
        CHECK(rnCommand(bare, Kind::Pytest, {tests[0]}, false, &dir) ==
              "python3 -m pytest 'a b/test_x.py::test_adds'");
        CHECK(dir == "/w");
        // The setting's template, quoted where the template quotes.
        bare.tmpl = "uv run pytest -q \"{tests}\" --rootdir {root}";
        CHECK(rnCommand(bare, Kind::Pytest, {tests[0]}, false) ==
              "uv run pytest -q \"a b/test_x.py::test_adds\" --rootdir /w");
        bare.tmpl = "Off";
        CHECK(rnCommand(bare, Kind::Pytest, {tests[0]}, false) ==
              "ERROR Running tests is off for this kind of file (test.python = off)");
        bare.tmpl = "";
        CHECK(rnCommand(bare, Kind::Pytest, {}, false).rfind("ERROR ", 0) == 0);
    }

    GROUP("runnables:go");
    {
        const std::string go =
            "package calc\n"                                        // 1
            "\n"                                                    // 2
            "import \"testing\"\n"                                  // 3
            "\n"                                                    // 4
            "func TestAdd(t *testing.T) {\n"                        // 5
            "\ts := `\n"                                            // 6
            "func TestInString(t *testing.T) {`\n"                  // 7
            "}\n"                                                   // 8
            "// func TestCommented(t *testing.T) {}\n"              // 9
            "func Testlower(t *testing.T) {}\n"                     // 10
            "func Test_under(t *testing.T) {}\n"                    // 11
            "func TestBench(b *testing.B) {}\n"                     // 12
            "func helper(t *testing.T) {}\n"                        // 13
            "func Test(t  *  testing.T) {}\n";                      // 14
        const auto tests = Runnables::find(go, Kind::Go);
        CHECK(rnLabels(tests, Kind::Go) == "5:TestAdd | 11:Test_under | 14:Test");
        if (tests.size() != 3) return;
        auto r = rnRequest("/w/mod/calc/add_test.go", "/w", {"/w/mod/go.mod"});
        std::string dir;
        CHECK(rnCommand(r, Kind::Go, {tests[0]}, false, &dir) == "go test -run '^TestAdd$' ./calc");
        CHECK(dir == "/w/mod");
        CHECK(rnCommand(r, Kind::Go, tests, true) == "go test -run '^(TestAdd|Test_under|Test)$' ./calc");
        auto top = rnRequest("/w/mod/add_test.go", "/w", {"/w/mod/go.mod"});
        CHECK(rnCommand(top, Kind::Go, {tests[0]}, false) == "go test -run '^TestAdd$' .");
    }

    GROUP("runnables:rust");
    {
        const std::string rs =
            "pub fn add(a: i32, b: i32) -> i32 { a + b }\n"     // 1
            "\n"                                                // 2
            "#[cfg(test)]\n"                                    // 3
            "mod tests {\n"                                     // 4
            "    use super::*;\n"                               // 5
            "\n"                                                // 6
            "    #[test]\n"                                     // 7
            "    fn it_works() {\n"                             // 8
            "        let s = \"#[test] fn in_string() {}\";\n"  // 9
            "        assert_eq!(add(2, 2), 4);\n"               // 10
            "    }\n"                                           // 11
            "\n"                                                // 12
            "    // #[test]\n"                                  // 13
            "    fn helper() {}\n"                              // 14
            "\n"                                                // 15
            "    #[tokio::test(flavor = \"multi_thread\")]\n"   // 16
            "    #[ignore]\n"                                   // 17
            "    async fn waits() {}\n"                         // 18
            "\n"                                                // 19
            "    mod nested {\n"                                // 20
            "        #[test] fn deep() {}\n"                    // 21
            "    }\n"                                           // 22
            "}\n"                                               // 23
            "\n"                                                // 24
            "#[test]\n"                                         // 25
            "fn top_level() {}\n";                              // 26
        const auto tests = Runnables::find(rs, Kind::Rust);
        CHECK(rnLabels(tests, Kind::Rust) ==
              "8:tests::it_works | 18:tests::waits | 21:tests::nested::deep | 26:top_level");
        if (tests.size() != 4) return;
        auto lib = rnRequest("/w/crate/src/lib.rs", "/w", {"/w/crate/Cargo.toml"});
        std::string dir;
        CHECK(rnCommand(lib, Kind::Rust, {tests[0]}, false, &dir) ==
              "cargo test -- --exact tests::it_works");
        CHECK(dir == "/w/crate");
        auto mod = rnRequest("/w/crate/src/parse/mod.rs", "/w", {"/w/crate/Cargo.toml"});
        CHECK(rnCommand(mod, Kind::Rust, {tests[2]}, false) ==
              "cargo test -- --exact parse::tests::nested::deep");
        auto sub = rnRequest("/w/crate/src/a/b.rs", "/w", {"/w/crate/Cargo.toml"});
        CHECK(rnCommand(sub, Kind::Rust, {tests[0], tests[3]}, true) ==
              "cargo test -- --exact a::b::tests::it_works a::b::top_level");
        auto integ = rnRequest("/w/crate/tests/api.rs", "/w", {"/w/crate/Cargo.toml"});
        CHECK(rnCommand(integ, Kind::Rust, {tests[3]}, false) ==
              "cargo test --test api -- --exact top_level");
        auto bin = rnRequest("/w/crate/src/bin/tool.rs", "/w", {"/w/crate/Cargo.toml"});
        CHECK(rnCommand(bin, Kind::Rust, {tests[3]}, false) ==
              "cargo test --bin tool -- --exact top_level");
    }

    GROUP("runnables:js");
    {
        const std::string js =
            "import { describe, it, expect } from 'vitest'\n"         // 1
            "\n"                                                      // 2
            "describe('math', () => {\n"                              // 3
            "  it('adds (two) numbers', () => {\n"                    // 4
            "    expect(1 + 1).toBe(2)\n"                             // 5
            "  })\n"                                                  // 6
            "  // it('commented', () => {})\n"                        // 7
            "  const s = \"test('in a string', () => {})\"\n"         // 8
            "  describe.skip(\"nested\", () => {\n"                   // 9
            "    test.only(`deep`, async () => {})\n"                 // 10
            "  })\n"                                                  // 11
            "  test(`with ${x}`, () => {})\n"                         // 12
            "  test.each([1, 2])('each %i', () => {})\n"              // 13
            "  obj.it('a method', () => {})\n"                        // 14
            "})\n"                                                    // 15
            "\n"                                                      // 16
            "test(\n"                                                 // 17
            "  'outside \\'quoted\\'', () => {})\n";                  // 18
        const auto tests = Runnables::find(js, Kind::Js);
        CHECK(rnLabels(tests, Kind::Js) ==
              "4:math > adds (two) numbers | 10:math > nested > deep | 17:outside 'quoted'");
        if (tests.size() != 3) return;
        // The project's own Vitest: its full name as an anchored pattern.
        auto r = rnRequest("/w/web/src/sum.test.ts", "/w",
                           {"/w/web/package.json", "/w/web/node_modules/.bin/vitest"});
        r.text = js;
        std::string dir;
        CHECK(rnCommand(r, Kind::Js, {tests[0]}, false, &dir) ==
              "node_modules/.bin/vitest run src/sum.test.ts -t '^math adds \\(two\\) numbers$'");
        CHECK(dir == "/w/web");
        CHECK(rnCommand(r, Kind::Js, tests, true) == "node_modules/.bin/vitest run src/sum.test.ts");
        // Jest when the project has Jest; npx when it has neither.
        auto jest = rnRequest("/w/web/sum.test.js", "/w",
                              {"/w/web/package.json", "/w/web/node_modules/.bin/jest"});
        CHECK(rnCommand(jest, Kind::Js, {tests[2]}, false) ==
              "node_modules/.bin/jest sum.test.js -t '^outside '\\''quoted'\\''$'");
        auto none = rnRequest("/w/sum.test.js", "/w", {});
        CHECK(rnCommand(none, Kind::Js, {tests[1]}, false) ==
              "npx jest sum.test.js -t '^math nested deep$'");
        none.text = "import { test } from \"vitest\"\n";
        CHECK(rnCommand(none, Kind::Js, {tests[1]}, false) ==
              "npx vitest run sum.test.js -t '^math nested deep$'");
    }

    GROUP("runnables:expand");
    {
        CHECK(Runnables::expand("a {x} {l} b", {{"x", "it's"}}, {{"l", {}}}) == "a 'it'\\''s' b");
        CHECK(Runnables::expand("{l} b", {}, {{"l", {}}}) == " b");
        CHECK(Runnables::expand("a {l}", {}, {{"l", {"1 2", "3"}}}) == "a '1 2' 3");
        CHECK(Runnables::expand("a {unknown}", {}, {}) == "a {unknown}");
        CHECK(Runnables::regexEscape("a.b(c)") == "a\\.b\\(c\\)");
        std::vector<Runnables::Test> t(2);
        t[0].line = 3;
        t[1].line = 9;
        CHECK(Runnables::testAt(t, 2) == -1);
        CHECK(Runnables::testAt(t, 3) == 0);
        CHECK(Runnables::testAt(t, 8) == 0);
        CHECK(Runnables::testAt(t, 40) == 1);
    }

    GROUP("runnables:settings");
    {
        std::vector<SettingsError> errors;
        Settings st = parseSettings(
            "test.python = uv run pytest -x {tests}  # quiet\n"   // 1
            "test.JS = off\n"                                      // 2
            "test.ruby = rspec {file}\n",                          // 3 not a language
            &errors);
        CHECK(st.testTemplate("python") == "uv run pytest -x {tests}");
        CHECK(st.testTemplate("js") == "off");
        CHECK(st.testTemplate("go").empty());
        CHECK(errors.size() == 1 && errors[0].line == 3);
    }

    GROUP("runnables:speed");
    {
        std::string big;
        for (int i = 0; i < 5000; i++)
            big += "def test_" + std::to_string(i) + "():\n    assert f('x') == 1  # note\n\n";
        const double t0 = cpuMs();
        const auto tests = Runnables::find(big, Kind::Pytest);
        CHECK(tests.size() == 5000);
        CHECK_TIME(cpuMs() - t0, 400);
    }
}
