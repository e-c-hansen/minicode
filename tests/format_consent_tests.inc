// format_consent_tests.inc — the `format:inside-folder` and `format:consent`
// groups (Formatter.h: which formatters a folder supplies, and when they may
// run), included by run_tests.cpp.
#include "Formatter.h"

static void testFormatConsent() {
    using namespace Formatter;

    GROUP("format:inside-folder");
    {
        CHECK(insideFolder("/w/proj/.venv/bin/black", "/w/proj"));
        CHECK(insideFolder("/w/proj/.venv/bin/black", "/w/proj/"));
        CHECK(insideFolder("/w/proj", "/w/proj"));
        CHECK(insideFolder("/w/proj/node_modules/.bin/prettier", "/w"));
        CHECK(!insideFolder("/w/projx/.venv/bin/black", "/w/proj"));   // a sibling's prefix
        CHECK(!insideFolder("/w/pro", "/w/proj"));
        CHECK(!insideFolder("/opt/homebrew/bin/black", "/w/proj"));
        CHECK(insideFolder("/opt/homebrew/bin/black", "/"));           // a window on /
        CHECK(!insideFolder("/w/proj/.venv/bin/black", ""));           // no folder
        CHECK(!insideFolder("", "/w/proj"));
        CHECK(!insideFolder(".venv/bin/black", "/w/proj"));            // relative: never
    }

    GROUP("format:consent");
    {
        const std::string local = "/w/proj/.venv/bin/black", outside = "/opt/homebrew/bin/black";
        // Nothing in the folder: as before, the usual folders decide.
        CHECK(chooseProgram(false, "", outside, "") == Source::Outside);
        CHECK(chooseProgram(false, "", "", "") == Source::None);
        CHECK(chooseProgram(true, "", outside, "deny") == Source::Outside);
        // The project's own copy: asked about first, then as answered.
        CHECK(chooseProgram(false, local, outside, "") == Source::Ask);
        CHECK(chooseProgram(false, local, "", "") == Source::Ask);
        CHECK(chooseProgram(false, local, outside, "allow") == Source::Local);
        CHECK(chooseProgram(false, local, "", "allow") == Source::Local);
        CHECK(chooseProgram(false, local, outside, "deny") == Source::Outside);
        CHECK(chooseProgram(false, local, "", "deny") == Source::None);
        // format.<lang> in the settings: the user's own tool, never a
        // question while a copy outside the folder exists.
        CHECK(chooseProgram(true, local, outside, "") == Source::Outside);
        CHECK(chooseProgram(true, local, outside, "allow") == Source::Local);
        CHECK(chooseProgram(true, local, outside, "deny") == Source::Outside);
        CHECK(chooseProgram(true, local, "", "") == Source::Ask);
        CHECK(chooseProgram(true, local, "", "deny") == Source::None);
        // An answer this code does not know counts as none.
        CHECK(chooseProgram(false, local, outside, "maybe") == Source::Ask);
    }
}
