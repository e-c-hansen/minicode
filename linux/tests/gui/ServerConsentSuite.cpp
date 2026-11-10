// ServerConsentSuite.cpp — a language server that would run the folder's
// own code waits for the folder's answer (`./build/minicode-guitest
// server_consent`, linux/PARITY.md item 2.3, Lsp::checkProjectCode), the
// counterpart of the Mac's tests/gui/ServerConsentSuite.mm, against the
// scripted server standing in for rust-analyzer (lsp.rust), with real
// clicks on the bar under the tabs:
//   - unanswered: it starts with build scripts, proc macros and cargo check
//     off, in initializationOptions and in what workspace/configuration
//     answers, and the bar over the Rust file asks, Allow and Don't Allow;
//   - Allow: it starts again with its defaults (nothing sent);
//   - the answer taken back: it starts again with the three off;
//   - a rust-toolchain.toml in the crate (rustup would run a toolchain it
//     names by path) and Don't Allow: it does not start at all, and the
//     status bar says it waits; taken back, the bar names the file; Allow
//     starts it once, with its defaults.
#include "Harness.h"

#include "ConsentBar.h"
#include "Editor.h"
#include "FakeLsp.h"
#include "FolderConsent.h"
#include "Lsp.h"

#include <glib/gstdio.h>

#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
LspSession* lsp() { return MCGuiApp().lsp; }
ConsentBar* bar() { return MCGuiApp().consentBar; }

std::string F(const std::string& name) { return MCGuiProjectFile("consent/" + name); }
std::string answer() { return FolderConsent::answer(MCGuiProject()); }
const char* kServer = "rust-consent";

// Every message the client sent the stand-in, as JSON.
std::vector<Json> received() {
    std::vector<Json> out;
    const std::string text = MCGuiReadFile(MCGuiFakeLspDir() + "/" + kServer + ".log");
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        Json m;
        if (Json::parse(text.substr(at, end - at), m)) out.push_back(m);
        at = end + 1;
    }
    return out;
}

std::vector<Json> initializes() {
    std::vector<Json> out;
    for (const Json& m : received())
        if (m["method"].asString() == "initialize") out.push_back(m);
    return out;
}

// Build scripts, proc macros and cargo check all off in `options`.
bool allOff(const Json& o) {
    return o.isObject() && o["cargo"]["buildScripts"]["enable"].isBool() &&
           !o["cargo"]["buildScripts"]["enable"].asBool() && o["procMacro"]["enable"].isBool() &&
           !o["procMacro"]["enable"].asBool() && o["checkOnSave"].isBool() &&
           !o["checkOnSave"].asBool();
}

bool barAsks(const std::string& part) {
    return bar()->asking() == "rust" && MCGuiOnScreen(bar()->allowButton()) &&
           bar()->text().find(part) != std::string::npos;
}

void showRust() {
    MCGuiStep("another tab, and the Rust file again", [] { MCGuiOpenFileKept(F("notes.txt")); });
    MCGuiWait("notes.txt on show", 10, [] { return ed()->currentPath() == F("notes.txt"); });
    MCGuiStep("the Rust file", [] { MCGuiOpenFileKept(F("src/main.rs")); });
    MCGuiWait("on show", 10, [] { return ed()->currentPath() == F("src/main.rs") && ed()->isSource(); });
}

}  // namespace

MC_GUI_SUITE(server_consent) {
    MCGuiStep("a Rust file, a stand-in for rust-analyzer, no answer for the folder", [] {
        FolderConsent::forget(MCGuiProject());
        g_remove(F("rust-toolchain.toml").c_str());
        MCGuiWriteFile(F("src/main.rs"), "fn main() {}\n");
        MCGuiWriteFile(F("notes.txt"), "notes\n");
        // Asks for its settings once initialized, as rust-analyzer does.
        const std::string cmd = MCGuiFakeLsp(
            kServer, "{\"name\": \"rust-analyzer\", \"rules\": [{\"method\": \"initialized\", "
                     "\"request\": [{\"method\": \"workspace/configuration\", \"params\": "
                     "{\"items\": [{\"section\": \"rust-analyzer\"}]}}]}]}");
        MCGuiFakeLspClearLog(kServer);
        MCGuiSetSettings("lsp.enabled = true\nlsp.rust = " + cmd + "\n");
        MCGuiOpenFileKept(F("src/main.rs"));
    });
    MCGuiWait("it starts and is asked about its settings' answer", 20, [] {
        for (const Json& m : received())
            if (m["method"].isNull() && m["result"].isArray()) return true;
        return false;
    });
    MCGuiWait("the bar asks about build scripts", 10, [] { return barAsks("build scripts"); });
    MCGuiStep("build scripts, proc macros and cargo check off, every way it reads them", [] {
        const std::vector<Json> inits = initializes();
        GCHECK(inits.size() == 1);
        if (!inits.empty()) {
            const Json& options = inits.back()["params"]["initializationOptions"];
            MCGuiNote("initializationOptions: %s", options.dump().c_str());
            GCHECK(allOff(options));
        }
        bool answered = false;
        for (const Json& m : received())
            if (m["method"].isNull() && m["result"].isArray() && m["result"].size() > 0)
                answered = allOff(m["result"][0]);
        GCHECK(answered);
        GCHECK_EQ_STR(lsp()->folderCodeAwaitingConsent(), "rust");
        GCHECK(answer().empty());
        GCHECK(MCGuiFindButton(bar()->widget(), "Allow") != nullptr);
        GCHECK(MCGuiFindButton(bar()->widget(), "Don't Allow") != nullptr);
    });
    MCGuiStep("Allow, a real click", [] {
        MCGuiFakeLspClearLog(kServer);
        MCGuiClick(bar()->allowButton());
    });
    MCGuiWait("it starts again", 20, [] { return initializes().size() == 1 && answer() == "allow"; });
    MCGuiStep("with its own defaults, and no question", [] {
        const std::vector<Json> inits = initializes();
        const Json options = inits.back()["params"]["initializationOptions"];
        MCGuiNote("initializationOptions: %s", options.dump().c_str());
        GCHECK(options.isNull());
        GCHECK(lsp()->folderCodeAwaitingConsent().empty());
        GCHECK(bar()->asking().empty());
        MCGuiFakeLspClearLog(kServer);
        FolderConsent::forget(MCGuiProject());
    });
    MCGuiWait("taken back: it starts again with the three off", 20, [] {
        const std::vector<Json> inits = initializes();
        return inits.size() == 1 && allOff(inits.back()["params"]["initializationOptions"]);
    });
    MCGuiWait("and the bar asks again", 10, [] { return barAsks("build scripts"); });

    MCGuiStep("a rust-toolchain.toml in the crate, and Don't Allow, a real click", [] {
        MCGuiWriteFile(F("rust-toolchain.toml"), "[toolchain]\npath = \"/nowhere\"\n");
        MCGuiFakeLspClearLog(kServer);
        MCGuiClick(bar()->denyButton());
    });
    MCGuiWait("it waits, not running", 10, [] {
        return answer() == "deny" &&
               lsp()->statusText().find("waits for your answer") != std::string::npos;
    });
    // Negative check: a server that would start again would have sent
    // initialize by now.
    MCGuiPause(1.0);
    MCGuiStep("nothing started; no question (answered); then taken back", [] {
        GCHECK(initializes().empty());
        MCGuiNote("status: %s", lsp()->statusText().c_str());
        GCHECK(lsp()->folderCodeAwaitingConsent().empty());
        GCHECK(bar()->asking().empty());
        FolderConsent::forget(MCGuiProject());
    });
    showRust();
    MCGuiWait("the bar names the file", 10, [] { return barAsks("rust-toolchain.toml"); });
    MCGuiStep("still not started; Allow, a real click", [] {
        MCGuiNote("the bar: %s", bar()->text().c_str());
        GCHECK(initializes().empty());
        MCGuiClick(bar()->allowButton());
    });
    MCGuiWait("it starts", 20, [] { return !initializes().empty(); });
    MCGuiPause(0.5);   // negative check: it must not start twice
    MCGuiStep("once, with its defaults", [] {
        const std::vector<Json> inits = initializes();
        GCHECK(inits.size() == 1);
        GCHECK(!inits.empty() && inits.back()["params"]["initializationOptions"].isNull());
        GCHECK(bar()->asking().empty());
    });
    MCGuiStep("tidy up: servers off, no answer, no tabs or files", [] {
        MCGuiSetSettings("lsp.enabled = false\n");
        FolderConsent::forget(MCGuiProject());
        for (const char* name : {"src/main.rs", "notes.txt"})
            if (Document* d = ed()->documentFor(F(name))) ed()->closeTab(d);
        g_remove(F("rust-toolchain.toml").c_str());
        g_remove(F("src/main.rs").c_str());
        g_remove(F("src").c_str());
        g_remove(F("notes.txt").c_str());
        g_remove(MCGuiProjectFile("consent").c_str());
    });
}
