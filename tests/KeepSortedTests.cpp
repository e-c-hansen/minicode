// KeepSortedTests.cpp — included by run_tests.cpp (uses its CHECK, toU16 and
// toU8). tests/keepsorted/ holds keep-sorted's own golden files, copied from
// https://github.com/google/keep-sorted (goldens/, Apache License 2.0, see
// tests/keepsorted/LICENSE): each .in, sorted with the marker name
// "keep-sorted-test", must give its .out exactly.
#include "KeepSorted.h"

namespace {

std::string ksAll(const std::string &text) { return KeepSorted::fixAll(text).text; }

#define KS_EQ(got, want)                                                       \
    do {                                                                       \
        std::string g_ = (got), w_ = (want);                                   \
        CHECK(g_ == w_);                                                       \
        if (g_ != w_)                                                          \
            std::printf("      got:\n%s\n      want:\n%s\n", g_.c_str(), w_.c_str()); \
    } while (0)

std::string ksRead(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "\x01missing";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void testKeepSortedGoldens() {
    GROUP("keepsorted:goldens");
    const char *names[] = {
        "block", "by_regex", "case_insensitive", "comments", "duplicates",
        "duplicates_keep_first_comment", "duplicates_merge_comments", "group",
        "group_regex", "ignore_prefixes", "numeric", "prefixes", "separator",
        "simple", "skip_lines", "sticky_prefixes", "trailing_commas",
    };
    int compared = 0;
    for (const char *n : names) {
        std::string in = ksRead(std::string("tests/keepsorted/") + n + ".in");
        std::string out = ksRead(std::string("tests/keepsorted/") + n + ".out");
        if (in == "\x01missing" || out == "\x01missing") continue;
        ++compared;
        KeepSorted::Result r = KeepSorted::fixAll(in, "keep-sorted-test");
        CHECK(r.text == out);
        if (r.text != out) std::printf("    golden %s differs\n", n);
        CHECK(r.errors.empty());
        // Sorting again changes nothing.
        KeepSorted::Result again = KeepSorted::fixAll(r.text, "keep-sorted-test");
        CHECK(!again.changed && again.text == out);
        // Every line of the result comes from the input or is a blank added
        // between items.
        CHECK(r.origin.size() == (size_t)std::count(r.text.begin(), r.text.end(), '\n') + 1);
        // keep-sorted's warnings (the .err files), by line.
        if (std::string(n) == "simple") {
            CHECK(r.warnings.size() == 3);
            CHECK(r.warnings.size() == 3 && r.warnings[0].line == 112 &&
                  r.warnings[1].line == 112 && r.warnings[2].line == 119);
        }
        if (std::string(n) == "skip_lines") {
            CHECK(r.warnings.size() == 3);
            CHECK(r.warnings.size() == 3 && r.warnings[0].line == 5 &&
                  r.warnings[1].line == 9 && r.warnings[2].line == 22);
        }
        if (std::string(n) == "by_regex") {
            CHECK(r.warnings.size() == 2);   // the bad regex "*", and ignore_prefixes
        }
    }
    if (compared == 0)
        std::printf("  (tests/keepsorted not found here; golden checks skipped)\n");
    else
        CHECK(compared == 17);
}

void testKeepSortedReadme() {
    GROUP("keepsorted:readme");
    // The examples in keep-sorted's README, before and after.
    KS_EQ(ksAll("@Component(\n"
                "    modules = {\n"
                "      // keep-sorted start\n"
                "      UtilsModule.class,\n"
                "      GetRequestModule.class,\n"
                "      PostRequestModule.class,\n"
                "      AuthModule.class,\n"
                "      MonitoringModule.class,\n"
                "      LoggingModule.class,\n"
                "      // keep-sorted end\n"
                "    })\n"),
          "@Component(\n"
          "    modules = {\n"
          "      // keep-sorted start\n"
          "      AuthModule.class,\n"
          "      GetRequestModule.class,\n"
          "      LoggingModule.class,\n"
          "      MonitoringModule.class,\n"
          "      PostRequestModule.class,\n"
          "      UtilsModule.class,\n"
          "      // keep-sorted end\n"
          "    })\n");
    // Nested blocks.
    KS_EQ(ksAll("# keep-sorted start block=yes\n"
                "foo = [\n"
                "  # keep-sorted start\n"
                "  'y',\n"
                "  'x',\n"
                "  'z',\n"
                "  # keep-sorted end\n"
                "]\n"
                "bar = [\n"
                "  # keep-sorted start\n"
                "  '1',\n"
                "  '3',\n"
                "  '2',\n"
                "  # keep-sorted end\n"
                "]\n"
                "# keep-sorted end\n"),
          "# keep-sorted start block=yes\n"
          "bar = [\n"
          "  # keep-sorted start\n"
          "  '1',\n"
          "  '2',\n"
          "  '3',\n"
          "  # keep-sorted end\n"
          "]\n"
          "foo = [\n"
          "  # keep-sorted start\n"
          "  'x',\n"
          "  'y',\n"
          "  'z',\n"
          "  # keep-sorted end\n"
          "]\n"
          "# keep-sorted end\n");
    // Line continuations: indented lines stay with their item...
    KS_EQ(ksAll("// keep-sorted start\n"
                "private final Foo foo;\n"
                "private final Baz baz =\n"
                "    new Baz()\n"
                "private final Bar bar;\n"
                "// keep-sorted end\n"),
          "// keep-sorted start\n"
          "private final Bar bar;\n"
          "private final Baz baz =\n"
          "    new Baz()\n"
          "private final Foo foo;\n"
          "// keep-sorted end\n");
    // ...unless group=no.
    KS_EQ(ksAll("// keep-sorted start group=no\n"
                "private final Bar bar;\n"
                "private final Baz baz =\n"
                "    new Baz()\n"
                "private final Foo foo;\n"
                "// keep-sorted end\n"),
          "// keep-sorted start group=no\n"
          "    new Baz()\n"
          "private final Bar bar;\n"
          "private final Baz baz =\n"
          "private final Foo foo;\n"
          "// keep-sorted end\n");
    // block=yes.
    KS_EQ(ksAll("  widgets := []widget{\n"
                "    // keep-sorted start block=yes\n"
                "    {\n"
                "      Name: \"def\",\n"
                "    },\n"
                "    {\n"
                "      Name: \"abc\",\n"
                "    },\n"
                "    // keep-sorted end\n"
                "  }\n"),
          "  widgets := []widget{\n"
          "    // keep-sorted start block=yes\n"
          "    {\n"
          "      Name: \"abc\",\n"
          "    },\n"
          "    {\n"
          "      Name: \"def\",\n"
          "    },\n"
          "    // keep-sorted end\n"
          "  }\n");
    // group_prefixes.
    KS_EQ(ksAll("// keep-sorted start group_prefixes=and,with\n"
                "spaghetti\n"
                "with meatballs\n"
                "peanut butter\n"
                "and jelly\n"
                "hamburger\n"
                "with lettuce\n"
                "and tomatoes\n"
                "// keep-sorted end\n"),
          "// keep-sorted start group_prefixes=and,with\n"
          "hamburger\n"
          "with lettuce\n"
          "and tomatoes\n"
          "peanut butter\n"
          "and jelly\n"
          "spaghetti\n"
          "with meatballs\n"
          "// keep-sorted end\n");
    // group_start_regex with newline_separated.
    KS_EQ(ksAll("// keep-sorted start group_start_regex=[\"^define\\\\b\"] newline_separated=yes\n"
                "// Some comment for foo\n"
                "define foo =\n"
                "abc + def;\n"
                "\n"
                "// Some other comment for bar\n"
                "define bar =\n"
                "ghi + jkl;\n"
                "// keep-sorted end\n"),
          "// keep-sorted start group_start_regex=[\"^define\\\\b\"] newline_separated=yes\n"
          "// Some other comment for bar\n"
          "define bar =\n"
          "ghi + jkl;\n"
          "\n"
          "// Some comment for foo\n"
          "define foo =\n"
          "abc + def;\n"
          "// keep-sorted end\n");
    // Sticky comments, and turning them off.
    const std::string users = "# alice\nusername: al1\n# bob\nusername: bo2\n"
                              "# charlie\nusername: ch3\n# keep-sorted end\n";
    KS_EQ(ksAll("# keep-sorted start\n# charlie\nusername: ch3\n# alice\nusername: al1\n"
                "# bob\nusername: bo2\n# keep-sorted end\n"),
          "# keep-sorted start\n" + users);
    KS_EQ(ksAll("# keep-sorted start sticky_comments=no\n" + users),
          "# keep-sorted start sticky_comments=no\n# alice\n# bob\n# charlie\n"
          "username: al1\nusername: bo2\nusername: ch3\n# keep-sorted end\n");
    // sticky_prefixes.
    KS_EQ(ksAll("// keep-sorted start sticky_prefixes=/*,@Annotation\n"
                "/* Foo */\n@Annotation\nFoo foo;\nBaz baz;\n// keep-sorted end\n"),
          "// keep-sorted start sticky_prefixes=/*,@Annotation\n"
          "Baz baz;\n/* Foo */\n@Annotation\nFoo foo;\n// keep-sorted end\n");
    // skip_lines, at the start and both ends.
    KS_EQ(ksAll("<!-- keep-sorted start skip_lines=2 -->\n"
                "Name    | Value\n------- | -----\n"
                "Charlie | Baz\nDelta   | Qux\nBravo   | Bar\nAlpha   | Foo\n"
                "<!-- keep-sorted end -->\n"),
          "<!-- keep-sorted start skip_lines=2 -->\n"
          "Name    | Value\n------- | -----\n"
          "Alpha   | Foo\nBravo   | Bar\nCharlie | Baz\nDelta   | Qux\n"
          "<!-- keep-sorted end -->\n");
    KS_EQ(ksAll("<!-- keep-sorted start skip_lines=2,-2 -->\n"
                "Item  | Cost\n----- | -----\nLemon | $1.50\nPear  | $2.10\nApple | $1.09\n"
                "----- | -----\nTotal | $4.69\n<!-- keep-sorted end -->\n"),
          "<!-- keep-sorted start skip_lines=2,-2 -->\n"
          "Item  | Cost\n----- | -----\nApple | $1.09\nLemon | $1.50\nPear  | $2.10\n"
          "----- | -----\nTotal | $4.69\n<!-- keep-sorted end -->\n");
    // case.
    KS_EQ(ksAll("# keep-sorted start\nalpha\nBravo\ncharlie\nDelta\necho\nFoxtrot\n# keep-sorted end\n"),
          "# keep-sorted start\nBravo\nDelta\nFoxtrot\nalpha\ncharlie\necho\n# keep-sorted end\n");
    KS_EQ(ksAll("# keep-sorted start case=no\nBravo\nDelta\nFoxtrot\nalpha\ncharlie\necho\n# keep-sorted end\n"),
          "# keep-sorted start case=no\nalpha\nBravo\ncharlie\nDelta\necho\nFoxtrot\n# keep-sorted end\n");
    // numeric.
    KS_EQ(ksAll("progress = (\n  # keep-sorted start numeric=yes\n"
                "  'PROGRESS_100_PERCENT',\n  'PROGRESS_10_PERCENT',\n  'PROGRESS_1_PERCENT',\n"
                "  'PROGRESS_50_PERCENT',\n  'PROGRESS_5_PERCENT',\n  # keep-sorted end\n)\n"),
          "progress = (\n  # keep-sorted start numeric=yes\n"
          "  'PROGRESS_1_PERCENT',\n  'PROGRESS_5_PERCENT',\n  'PROGRESS_10_PERCENT',\n"
          "  'PROGRESS_50_PERCENT',\n  'PROGRESS_100_PERCENT',\n  # keep-sorted end\n)\n");
    // by_regex, with prefix_order.
    const std::string javaFields = "List<String> foo;\nObject baz;\nString bar;\n// keep-sorted end\n";
    KS_EQ(ksAll("// keep-sorted start by_regex=\\w+;\n" + javaFields),
          "// keep-sorted start by_regex=\\w+;\nString bar;\nObject baz;\nList<String> foo;\n"
          "// keep-sorted end\n");
    KS_EQ(ksAll("// keep-sorted start by_regex=\\w+; prefix_order=foo\n" + javaFields),
          "// keep-sorted start by_regex=\\w+; prefix_order=foo\nList<String> foo;\n"
          "String bar;\nObject baz;\n// keep-sorted end\n");
    // by_regex over blocks.
    KS_EQ(ksAll("// keep-sorted start block=yes newline_separated=yes case=no by_regex=(\\w+)\\(\\)\\s+{ numeric=yes\n"
                "bool func2() {\n  return true;\n}\n\n"
                "int func1() {\n  return 1;\n}\n\n"
                "List<SomeReallyLongTypeParameterThatWouldForceTheFunctionNameOnlyTheNextLine>\n"
                "    func0() {\n  return List.of(whatever);\n}\n"
                "// keep-sorted end\n"),
          "// keep-sorted start block=yes newline_separated=yes case=no by_regex=(\\w+)\\(\\)\\s+{ numeric=yes\n"
          "List<SomeReallyLongTypeParameterThatWouldForceTheFunctionNameOnlyTheNextLine>\n"
          "    func0() {\n  return List.of(whatever);\n}\n\n"
          "int func1() {\n  return 1;\n}\n\n"
          "bool func2() {\n  return true;\n}\n"
          "// keep-sorted end\n");
    // by_regex with a template: last name, then first.
    KS_EQ(ksAll("keep-sorted start skip_lines=1 by_regex=['^(?<first_name>\\w+) (?<last_name>\\w+)$': '${last_name} ${first_name}']\n"
                "\nDaniel Bernoulli\nEmmy Noether\nJacob Bernoulli\nJohann Bernoulli\nMax Noether\n"
                "Nicolaus Bernoulli\n\nkeep-sorted end\n"),
          "keep-sorted start skip_lines=1 by_regex=['^(?<first_name>\\w+) (?<last_name>\\w+)$': '${last_name} ${first_name}']\n"
          "\nDaniel Bernoulli\nJacob Bernoulli\nJohann Bernoulli\nNicolaus Bernoulli\nEmmy Noether\n"
          "Max Noether\n\nkeep-sorted end\n");
    // by_regex with alternatives and (?i).
    KS_EQ(ksAll("// keep-sorted start numeric=yes by_regex=(?i)(.*?)(?:(\\d+)b|(\\d+)m|(\\d+)k)\n"
                "Data Size A 20M\nData Size A 50K\nData Size A 250M\n"
                "Data Size B 1B\nData Size B 80M\nData Size B 250K\n// keep-sorted end\n"),
          "// keep-sorted start numeric=yes by_regex=(?i)(.*?)(?:(\\d+)b|(\\d+)m|(\\d+)k)\n"
          "Data Size A 50K\nData Size A 20M\nData Size A 250M\n"
          "Data Size B 250K\nData Size B 80M\nData Size B 1B\n// keep-sorted end\n");
    // prefix_order, with numeric.
    KS_EQ(ksAll("// keep-sorted start prefix_order=INIT_,,FINAL_\n"
                "DO_SOMETHING_WITH_BAR,\nDO_SOMETHING_WITH_FOO,\nFINAL_BAR,\nFINAL_FOO,\n"
                "INIT_BAR,\nINIT_FOO\n// keep-sorted end\n"),
          "// keep-sorted start prefix_order=INIT_,,FINAL_\n"
          "INIT_BAR,\nINIT_FOO,\nDO_SOMETHING_WITH_BAR,\nDO_SOMETHING_WITH_FOO,\nFINAL_BAR,\n"
          "FINAL_FOO\n// keep-sorted end\n");
    KS_EQ(ksAll("droid_components = [\n  # keep-sorted start numeric=yes prefix_order=R2,C3\n"
                "  C3PO_ARM_L,\n  C3PO_ARM_R,\n  C3PO_HEAD,\n  R2D2_BOLTS_10_MM,\n"
                "  R2D2_BOLTS_5_MM,\n  R2D2_PROJECTOR,\n  R4_MOTIVATOR,\n  # keep-sorted end\n]\n"),
          "droid_components = [\n  # keep-sorted start numeric=yes prefix_order=R2,C3\n"
          "  R2D2_BOLTS_5_MM,\n  R2D2_BOLTS_10_MM,\n  R2D2_PROJECTOR,\n  C3PO_ARM_L,\n"
          "  C3PO_ARM_R,\n  C3PO_HEAD,\n  R4_MOTIVATOR,\n  # keep-sorted end\n]\n");
    // ignore_prefixes, with numeric.
    KS_EQ(ksAll("// keep-sorted start ignore_prefixes=fs.setBoolFlag,fs.setIntFlag\n"
                "fs.setBoolFlag(\"paws_with_cute_toebeans\", true)\n"
                "fs.setBoolFlag(\"whiskered_adorable_dog\", true)\n"
                "fs.setIntFlag(\"pretty_whiskered_kitten\", 6)\n// keep-sorted end\n"),
          "// keep-sorted start ignore_prefixes=fs.setBoolFlag,fs.setIntFlag\n"
          "fs.setBoolFlag(\"paws_with_cute_toebeans\", true)\n"
          "fs.setIntFlag(\"pretty_whiskered_kitten\", 6)\n"
          "fs.setBoolFlag(\"whiskered_adorable_dog\", true)\n// keep-sorted end\n");
    KS_EQ(ksAll(" droid_components = [\n   # keep-sorted start numeric=yes ignore_prefixes=R2D2,C3PO,R4\n"
                "   R4_MOTIVATOR,\n   C3PO_HEAD,\n   R2D2_BOLTS_10_MM,\n   C3PO_ARM_L,\n"
                "   R2D2_PROJECTOR,\n   R2D2_BOLTS_5_MM,\n   C3PO_ARM_R,\n   # keep-sorted end\n ]\n"),
          " droid_components = [\n   # keep-sorted start numeric=yes ignore_prefixes=R2D2,C3PO,R4\n"
          "   C3PO_ARM_L,\n   C3PO_ARM_R,\n   R2D2_BOLTS_5_MM,\n   R2D2_BOLTS_10_MM,\n"
          "   C3PO_HEAD,\n   R4_MOTIVATOR,\n   R2D2_PROJECTOR,\n   # keep-sorted end\n ]\n");
    // order=desc.
    KS_EQ(ksAll("# keep-sorted start order=desc\na\nb\nc\n# keep-sorted end\n"),
          "# keep-sorted start order=desc\nc\nb\na\n# keep-sorted end\n");
    // Duplicates go, unless they carry different comments, or remove_duplicates=no.
    KS_EQ(ksAll("# keep-sorted start\nrotation: foo\nrotation: bar\nrotation: baz\nrotation: bar\n"
                "# Add bar twice!\nrotation: bar\n# keep-sorted end\n"),
          "# keep-sorted start\nrotation: bar\n# Add bar twice!\nrotation: bar\nrotation: baz\n"
          "rotation: foo\n# keep-sorted end\n");
    KS_EQ(ksAll("# keep-sorted start remove_duplicates=no\nrotation: baz\nrotation: bar\n"
                "rotation: foo\nrotation: baz\nrotation: bar\nrotation: baz\n# keep-sorted end\n"),
          "# keep-sorted start remove_duplicates=no\nrotation: bar\nrotation: bar\n"
          "rotation: baz\nrotation: baz\nrotation: baz\nrotation: foo\n# keep-sorted end\n");
    // newline_separated, yes and 2.
    KS_EQ(ksAll("# keep-sorted start newline_separated=yes\nPineapples\nApples\nOranges\nBananas\n"
                "# keep-sorted end\n"),
          "# keep-sorted start newline_separated=yes\nApples\n\nBananas\n\nOranges\n\nPineapples\n"
          "# keep-sorted end\n");
    KS_EQ(ksAll("# keep-sorted start newline_separated=2\nBananas\nApples\n# keep-sorted end\n"),
          "# keep-sorted start newline_separated=2\nApples\n\n\nBananas\n# keep-sorted end\n");
    // YAML lists of prefixes.
    KS_EQ(ksAll("<!-- keep-sorted start prefix_order=[\"* \", \"* [\"] -->\n"
                "  * [baz](path/to/baz)\n  * foo\n  * bar\n<!-- keep-sorted end -->\n"),
          "<!-- keep-sorted start prefix_order=[\"* \", \"* [\"] -->\n"
          "  * bar\n  * foo\n  * [baz](path/to/baz)\n<!-- keep-sorted end -->\n");
    // FAQ: blank lines and trailing commas.
    KS_EQ(ksAll("keep-sorted start\n3\n\n1\n\n2\n\n\nkeep-sorted end\n"),
          "keep-sorted start\n\n1\n2\n3\n\n\nkeep-sorted end\n");
    KS_EQ(ksAll("keep-sorted start\n3,\n1,\n2\nkeep-sorted end\n"),
          "keep-sorted start\n1,\n2,\n3\nkeep-sorted end\n");
}

void testKeepSortedEdges() {
    GROUP("keepsorted:errors");
    // A start without an end, or an end without a start: nothing anywhere is
    // sorted, and the line is named.
    const std::string open = "// keep-sorted start\nb\na\n// keep-sorted end\n// keep-sorted start\nd\nc\n";
    KeepSorted::Result r = KeepSorted::fixAll(open);
    CHECK(!r.changed && r.text == open);
    CHECK(r.errors.size() == 1 && r.errors[0].line == 4);
    r = KeepSorted::fixAll("b\na\n# keep-sorted end\n# keep-sorted start\nz\ny\n# keep-sorted end\n");
    CHECK(!r.changed && r.errors.size() == 1 && r.errors[0].line == 2);
    r = KeepSorted::fixAll("# keep-sorted end\n# keep-sorted start\n");
    CHECK(!r.changed && r.errors.size() == 2);
    // A nested block that sorting would split: the outer block is left alone
    // and named, the inner one still sorted.
    const std::string split = "# keep-sorted start group=no\n"
                              "z\n"
                              "# keep-sorted start\n"
                              "b\n"
                              "a\n"
                              "# keep-sorted end\n"
                              "m\n"
                              "# keep-sorted end\n";
    r = KeepSorted::fixAll(split);
    CHECK(r.errors.size() == 1 && r.errors[0].line == 0);
    CHECK(r.text == "# keep-sorted start group=no\nz\n# keep-sorted start\na\nb\n"
                    "# keep-sorted end\nm\n# keep-sorted end\n");
    // With the default group=yes the nested block travels as one item (its
    // start marker is a comment, so it sticks to the lines after it, as in
    // keep-sorted).
    r = KeepSorted::fixAll("# keep-sorted start\nz\n# keep-sorted start\nb\na\n# keep-sorted end\n"
                           "m\n# keep-sorted end\n");
    CHECK(r.errors.empty());
    CHECK(r.text == "# keep-sorted start\n# keep-sorted start\na\nb\n# keep-sorted end\nm\nz\n"
                    "# keep-sorted end\n");
    // skip_lines past the end: a warning, nothing sorted.
    r = KeepSorted::fixAll("# keep-sorted start skip_lines=5\nb\na\n# keep-sorted end\n");
    CHECK(!r.changed && r.warnings.size() == 1);
    // An unknown option is reported and the block still sorted.
    r = KeepSorted::fixAll("# keep-sorted start colour=blue\nb\na\n# keep-sorted end\n");
    CHECK(r.changed && r.warnings.size() == 1 && r.warnings[0].text.find("colour") != std::string::npos);
    // A bad value keeps the default.
    r = KeepSorted::fixAll("# keep-sorted start case=maybe\nb\nA\na\n# keep-sorted end\n");
    CHECK(r.text == "# keep-sorted start case=maybe\nA\na\nb\n# keep-sorted end\n");
    CHECK(r.warnings.size() == 1);
    // RE2 syntax std::regex lacks: the block is left as it is.
    r = KeepSorted::fixAll("# keep-sorted start by_regex=\\pL+\nb\na\n# keep-sorted end\n");
    CHECK(!r.changed && r.errors.size() == 1);

    GROUP("keepsorted:text");
    CHECK(ksAll("") == "");
    CHECK(!KeepSorted::fixAll("").changed && KeepSorted::fixAll("").blocks == 0);
    CHECK(ksAll("no markers\nhere\n") == "no markers\nhere\n");
    // CRLF is kept.
    CHECK(ksAll("# keep-sorted start\r\nb\r\na\r\n# keep-sorted end\r\n") ==
          "# keep-sorted start\r\na\r\nb\r\n# keep-sorted end\r\n");
    // No newline at the end of the file.
    CHECK(ksAll("// keep-sorted start\nb\na\n// keep-sorted end") ==
          "// keep-sorted start\na\nb\n// keep-sorted end");
    // Leading spaces do not count, and UTF-8 sorts by code point.
    CHECK(ksAll("// keep-sorted start\n  zeta\nalpha\nÉcole\nécole\n\xF0\x9F\x98\x80\n// keep-sorted end\n") ==
          "// keep-sorted start\nalpha\n  zeta\nÉcole\nécole\n\xF0\x9F\x98\x80\n// keep-sorted end\n");
    // case=no folds accented capitals too.
    CHECK(ksAll("// keep-sorted start case=no\nécole\nÉcole\nÉtude\n// keep-sorted end\n") ==
          "// keep-sorted start case=no\nÉcole\nécole\nÉtude\n// keep-sorted end\n");
    // The markers work in any comment syntax and anywhere in the line.
    CHECK(ksAll("x = [  # keep-sorted start\n  3,\n  1,\n  2,\n]  # keep-sorted end\n") ==
          "x = [  # keep-sorted start\n  1,\n  2,\n  3,\n]  # keep-sorted end\n");
    CHECK(ksAll("-- keep-sorted start\nb\na\n-- keep-sorted end\n") ==
          "-- keep-sorted start\na\nb\n-- keep-sorted end\n");
    // Numbers: leading zeros, and big ones.
    CHECK(ksAll("# keep-sorted start numeric=yes\nv010\nv9\nv0011\n"
                "x123456789012345678901234567890\nx99\n# keep-sorted end\n") ==
          "# keep-sorted start numeric=yes\nv9\nv010\nv0011\nx99\n"
          "x123456789012345678901234567890\n# keep-sorted end\n");
    // A block sorted down to nothing keeps one empty line, as keep-sorted
    // writes it; a nested one keeps none.
    CHECK(ksAll("# keep-sorted start newline_separated=yes skip_lines=-1\n  \nx\n# keep-sorted end\n") ==
          "# keep-sorted start newline_separated=yes skip_lines=-1\n\nx\n# keep-sorted end\n");
    CHECK(ksAll("# keep-sorted start\n  # keep-sorted start newline_separated=2 skip_lines=-1\n"
                "  \n  x\n  # keep-sorted end\n# keep-sorted end\n") ==
          "# keep-sorted start\n  # keep-sorted start newline_separated=2 skip_lines=-1\n"
          "  x\n  # keep-sorted end\n# keep-sorted end\n");
    // Origins follow the lines.
    r = KeepSorted::fixAll("# keep-sorted start\nb\na\n# keep-sorted end\n");
    CHECK((r.origin == std::vector<int>{0, 2, 1, 3, 4}));
    r = KeepSorted::fixAll("# keep-sorted start newline_separated=yes\nb\na\n# keep-sorted end");
    CHECK((r.origin == std::vector<int>{0, 2, -1, 1, 3}));

    GROUP("keepsorted:at");
    const std::string two = "# keep-sorted start\nb\na\n# keep-sorted end\nx\n"
                            "# keep-sorted start\nd\nc\n# keep-sorted end\n";
    r = KeepSorted::fixAt(two, 1);
    CHECK(r.text == "# keep-sorted start\na\nb\n# keep-sorted end\nx\n"
                    "# keep-sorted start\nd\nc\n# keep-sorted end\n");
    r = KeepSorted::fixAt(two, 8);   // on the end marker
    CHECK(r.text == "# keep-sorted start\nb\na\n# keep-sorted end\nx\n"
                    "# keep-sorted start\nc\nd\n# keep-sorted end\n");
    r = KeepSorted::fixAt(two, 4);   // between the blocks
    CHECK(!r.changed && r.errors.size() == 1);
    // The innermost block only.
    const std::string nested = "# keep-sorted start block=yes\nz = [\n  # keep-sorted start\n"
                               "  2,\n  1,\n  # keep-sorted end\n]\na = 1\n# keep-sorted end\n";
    r = KeepSorted::fixAt(nested, 3);
    CHECK(r.text == "# keep-sorted start block=yes\nz = [\n  # keep-sorted start\n"
                    "  1,\n  2,\n  # keep-sorted end\n]\na = 1\n# keep-sorted end\n");
    r = KeepSorted::fixAt(nested, 7);
    CHECK(r.text == "# keep-sorted start block=yes\na = 1\nz = [\n  # keep-sorted start\n"
                    "  1,\n  2,\n  # keep-sorted end\n]\n# keep-sorted end\n");

    GROUP("keepsorted:sort lines");
    r = KeepSorted::sortLines("c\nb\na\nz\n", 0, 2);
    CHECK(r.text == "a\nb\nc\nz\n");
    r = KeepSorted::sortLines("x\n  b\na\nb\na\n", 1, 4);   // stable, duplicates kept
    CHECK(r.text == "x\na\na\n  b\nb\n");
    r = KeepSorted::sortLines("b\r\na", 0, 1);
    CHECK(r.text == "a\r\nb");
    CHECK(!KeepSorted::sortLines("a\nb\n", 0, 1).changed);
}

std::string ksShow(const std::u16string &text, const KeepSorted::Edit &e) {
    std::u16string t = text;
    if (e.changed) t.replace(e.replaceStart, e.replaceLength, e.replacement);
    if (e.selStart == e.selEnd) {
        t.insert(e.selStart, u"|");
    } else {
        t.insert(e.selEnd, u"\u203A");
        t.insert(e.selStart, u"\u2039");
    }
    return toU8(t, nullptr);
}

void testKeepSortedCommands() {
    GROUP("keepsorted:commands");
    auto sortCmd = [](const std::string &s, size_t a, size_t b) {
        std::u16string t = toU16(s);
        return ksShow(t, KeepSorted::sortCommand(t, a, b));
    };
    auto fixCmd = [](const std::string &s, size_t a, size_t b) {
        std::u16string t = toU16(s);
        return ksShow(t, KeepSorted::fixCommand(t, a, b));
    };
    // A selection over lines sorts them and selects them.
    KS_EQ(sortCmd("x\nc\nb\na\ny", 2, 7), "x\n\u2039a\nb\nc\u203A\ny");
    // Ending at the start of a line leaves that line out.
    KS_EQ(sortCmd("c\nb\na\n", 0, 4), "\u2039b\nc\n\u203Aa\n");
    // A selection inside one line: the block around it.
    KS_EQ(sortCmd("// keep-sorted start\nbeta\nalpha\n// keep-sorted end\n", 22, 24),
          "// keep-sorted start\nalpha\nb\u2039et\u203Aa\n// keep-sorted end\n");
    // The caret follows its line, keeping its column.
    KS_EQ(sortCmd("// keep-sorted start\nbeta\nalpha\n// keep-sorted end\n", 23, 23),
          "// keep-sorted start\nalpha\nbe|ta\n// keep-sorted end\n");
    // On a line that lost its comma the column is cut to its end.
    KS_EQ(fixCmd("# keep-sorted start\nc,\na,\nb\n# keep-sorted end", 22, 22),
          "# keep-sorted start\na,\nb,\nc|\n# keep-sorted end");
    // The edit is only the lines that changed.
    {
        std::u16string t = toU16("head\n# keep-sorted start\nb\na\n# keep-sorted end\ntail\n");
        KeepSorted::Edit e = KeepSorted::fixCommand(t, 0, 0);
        CHECK(e.changed && e.replaceStart == 25 && e.replaceLength == 3 && e.replacement == u"a\nb");
        CHECK(e.selStart == 0 && !e.error && e.message.empty());
    }
    // Not in a block, no selection: a message and nothing changed.
    {
        std::u16string t = toU16("b\na\n");
        KeepSorted::Edit e = KeepSorted::sortCommand(t, 1, 1);
        CHECK(!e.changed && e.error && e.message.find("not in a keep-sorted block") != std::string::npos);
        CHECK(e.message.rfind("Line 1:", 0) == 0);
        e = KeepSorted::fixCommand(t, 0, 0);
        CHECK(!e.changed && !e.error && e.message == "No keep-sorted blocks in this file");
    }
    // An unmatched marker is named.
    {
        std::u16string t = toU16("a\n# keep-sorted start\nb\n");
        KeepSorted::Edit e = KeepSorted::fixCommand(t, 0, 0);
        CHECK(!e.changed && e.error && e.message.rfind("Line 2:", 0) == 0);
    }
    // UTF-16: an emoji before the caret.
    KS_EQ(sortCmd("# keep-sorted start\n\U0001F600 b\n\U0001F600 a\n# keep-sorted end", 24, 24),
          "# keep-sorted start\n\U0001F600 a\n\U0001F600 b|\n# keep-sorted end");
    // CRLF.
    KS_EQ(fixCmd("# keep-sorted start\r\nb\r\na\r\n# keep-sorted end\r\n", 22, 22),
          "# keep-sorted start\r\na\r\nb|\r\n# keep-sorted end\r\n");
    // A selection kept by fixCommand, each end on its own line.
    KS_EQ(fixCmd("# keep-sorted start\nbb\naa\n# keep-sorted end\n", 21, 24),
          "# keep-sorted start\na\u2039a\nb\u203Ab\n# keep-sorted end\n");
    CHECK(KeepSorted::hasBlocks(u"// keep-sorted start"));
    CHECK(!KeepSorted::hasBlocks(u"keep-sorted end"));
}

void testKeepSorted() {
    testKeepSortedGoldens();
    testKeepSortedReadme();
    testKeepSortedEdges();
    testKeepSortedCommands();
}

}  // namespace
