// LspWatchSuite.cpp — `./build/minicode-guitest lsp_watch`: files changed on
// disk reach the language servers (workspace/didChangeWatchedFiles;
// linux/PARITY.md item 2.2, the Mac's tests/gui/LspWatchSuite.mm), against
// the scripted server (build/fakelsp). It registers watchers once
// initialized, as basedpyright and Ruff do: `**/*.py`, and a
// RelativePattern on a folder outside the project. A file no tab holds,
// in a folder made after the watching began, is created, changed in place
// and deleted, and each reaches the server with its URI and type; a .txt
// and a file inside .git do not; a file below the outside folder does.
// When the server unregisters, nothing is watched and nothing more is
// sent. Then a branch switch's batch: 20,000 changes against twelve
// watchers, matched on the watcher's thread, the main thread only writing
// the message.
#include "Harness.h"

#include "Editor.h"
#include "FakeLspLog.h"
#include "Lsp.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
LspSession* lsp() { return MCGuiApp().lsp; }
std::string F(const std::string& name) { return MCGuiProjectFile("watch/" + name); }
const char* kServer = "watch";
const char* kBig = "watchbig";

std::string real(const std::string& p) {
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}

std::string lib() { return MCGuiScratch() + "/watchlib"; }
std::string gRoot, gLib;   // as the kernel spells them

// Every change the server named `name` was sent, in order, as "type uri".
std::vector<std::string> changes(const std::string& name) {
    std::vector<std::string> out;
    for (const Json& m : MCGuiFakeLspMessages(name)) {
        if (m["method"].asString() != "workspace/didChangeWatchedFiles") continue;
        for (const Json& c : m["params"]["changes"].elements())
            out.push_back(std::to_string(c["type"].asInt()) + " " + c["uri"].asString());
    }
    return out;
}

bool sent(const std::string& name, int type, const std::string& path) {
    const std::string want = std::to_string(type) + " " + Lsp::uriFromPath(path);
    for (const std::string& c : changes(name))
        if (c == want) return true;
    return false;
}

bool mentions(const std::string& name, const std::string& part) {
    for (const std::string& c : changes(name))
        if (c.find(part) != std::string::npos) return true;
    return false;
}

void append(const std::string& path, const std::string& text) {
    if (FILE* f = fopen(path.c_str(), "a")) {
        fputs(text.c_str(), f);
        fclose(f);
    }
}

std::string str(const std::string& s) { return Json(s).dump(); }

std::string script() {
    const std::string libUri = Lsp::uriFromPath(gLib);
    const std::string watchers =
        "{\"registrations\": ["
        "{\"id\": \"py\", \"method\": \"workspace/didChangeWatchedFiles\", \"registerOptions\":"
        " {\"watchers\": [{\"globPattern\": \"**/*.py\"}]}},"
        "{\"id\": \"lib\", \"method\": \"workspace/didChangeWatchedFiles\", \"registerOptions\":"
        " {\"watchers\": [{\"globPattern\": {\"baseUri\": " + str(libUri) +
        ", \"pattern\": \"**/*.so\"}}]}}]}";
    const std::string drop =
        "{\"unregisterations\": ["
        "{\"id\": \"py\", \"method\": \"workspace/didChangeWatchedFiles\"},"
        "{\"id\": \"lib\", \"method\": \"workspace/didChangeWatchedFiles\"}]}";
    return "{\"name\": \"fakelsp\", \"rules\": ["
           "{\"method\": \"initialized\", \"request\": [{\"method\": \"client/registerCapability\","
           " \"params\": " + watchers + "}]},"
           "{\"method\": \"textDocument/didOpen\", \"when\": {\"uriEndsWith\": \"b.py\"},"
           " \"request\": [{\"method\": \"client/unregisterCapability\", \"params\": " + drop +
           "}]}]}";
}

std::string bigScript() {
    std::string list;
    for (const char* g : {"**/*.py", "**/*.pyi", "**/pyrightconfig.json", "**/pyproject.toml",
                          "**/setup.cfg", "**/setup.py", "**/requirements*.txt", "ruff.toml",
                          "**/.ruff.toml", "**/*.ipynb"})
        list += std::string(list.empty() ? "" : ", ") + "{\"globPattern\": " + str(g) + "}";
    for (const char* g : {"**/*.so", "**/*.pth"})
        list += ", {\"globPattern\": {\"baseUri\": " + str(Lsp::uriFromPath(gLib)) +
                ", \"pattern\": " + str(g) + "}}";
    return "{\"name\": \"fakelsp\", \"rules\": [{\"method\": \"initialized\", \"request\": [{"
           "\"method\": \"client/registerCapability\", \"params\": {\"registrations\": [{\"id\":"
           " \"all\", \"method\": \"workspace/didChangeWatchedFiles\", \"registerOptions\":"
           " {\"watchers\": [" + list + "]}}]}}]}]}";
}

void showTab(const std::string& path) {
    MCGuiOpenFileKept(path);
    MCGuiFocusKeyWindow();
    ed()->focusActiveDocument();
}

bool hasFolder(const std::string& path) {
    for (const std::string& f : lsp()->watchedFolders())
        if (f == path) return true;
    return false;
}

std::string gSettings;
std::size_t gMatchedHere = 0;
const int kBigCount = 20000;

}  // namespace

MC_GUI_SUITE(lsp_watch) {
    MCGuiStep("a server that registers watchers once initialized", [] {
        MCGuiSetKeyWindow(nullptr);
        ed()->closeAllTabs();
        MCGuiWriteFile(F("a.py"), "print('a')\n");
        MCGuiWriteFile(F("b.py"), "print('b')\n");
        MCGuiWriteFile(F(".git/hooks/README"), "hooks\n");
        MCGuiWriteFile(lib() + "/ext/README", "a library\n");
        gRoot = real(MCGuiProject());
        gLib = real(lib());
        gSettings = MCGuiReadFile(g_getenv("MINICODE_SETTINGS"));
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + MCGuiFakeLsp(kServer, script()) +
                         "\nlsp.python.also = off\n");
        showTab(F("a.py"));
    });
    MCGuiWait("the folders are watched", 20, [] { return lsp()->watchedFolders().size() == 2; });
    MCGuiStep("the project and the outside folder, resolved; the reply was null", [] {
        GCHECK(hasFolder(gRoot) && hasFolder(gLib));
        bool answered = false;
        for (const Json& m : MCGuiFakeLspMessages(kServer))
            if (!m.has("method") && m["id"].asInt() == 1000 && m["result"].isNull() &&
                !m.has("error"))
                answered = true;
        GCHECK(answered);
        GCHECK(changes(kServer).empty());
        // Every folder below, .git's left out.
        MCGuiNote("folders with a monitor: %zu", lsp()->watchedFolderCount());
        GCHECK(lsp()->watchedFolderCount() >= 3);
        MCGuiWriteFile(F("pkg/fresh.py"), "x = 1\n");
    });
    MCGuiWait("a new file no tab holds, in a new folder, is reported as created", 10, [] {
        return sent(kServer, 1, gRoot + "/watch/pkg/fresh.py");
    });
    MCGuiStep("changed in place", [] { append(F("pkg/fresh.py"), "y = 2\n"); });
    MCGuiWait("reported as changed", 10, [] {
        return sent(kServer, 2, gRoot + "/watch/pkg/fresh.py");
    });
    MCGuiStep("deleted, with a .txt and a file in .git written meanwhile", [] {
        MCGuiWriteFile(F("notes.txt"), "not python\n");
        MCGuiWriteFile(F(".git/hooks/x.py"), "# a hook\n");
        remove(F("pkg/fresh.py").c_str());
    });
    MCGuiWait("reported as deleted", 10, [] {
        return sent(kServer, 3, gRoot + "/watch/pkg/fresh.py");
    });
    MCGuiStep("a file below the outside folder, and a README beside it", [] {
        MCGuiWriteFile(lib() + "/ext/native.so", "elf");
        MCGuiWriteFile(lib() + "/ext/readme.md", "no");
    });
    MCGuiWait("the outside folder's file arrives", 10, [] {
        return sent(kServer, 1, gLib + "/ext/native.so");
    });
    MCGuiStep("a marker", [] { MCGuiWriteFile(F("marker.py"), "pass\n"); });
    MCGuiWait("the marker arrives", 10, [] { return sent(kServer, 1, gRoot + "/watch/marker.py"); });
    MCGuiStep("what no watcher wants, and what is inside .git, never went", [] {
        GCHECK(!mentions(kServer, "notes.txt"));
        GCHECK(!mentions(kServer, "/.git/"));
        GCHECK(!mentions(kServer, "readme.md"));
        // One notification a batch, never an empty one.
        for (const Json& m : MCGuiFakeLspMessages(kServer))
            if (m["method"].asString() == "workspace/didChangeWatchedFiles")
                GCHECK(m["params"]["changes"].size() > 0);
        GCHECK(lsp()->watchedBatches() >= 4);
        showTab(F("b.py"));   // the server unregisters when b.py opens
    });
    MCGuiWait("unregistered, nothing is watched", 10, [] {
        return lsp()->watchedFolders().empty() && lsp()->watchedFolderCount() == 0;
    });
    MCGuiStep("and nothing more is sent", [] {
        MCGuiFakeLspClearLog(kServer);
        MCGuiWriteFile(F("pkg/fresh.py"), "z = 3\n");
    });
    MCGuiPause(1.0);   // negative: no didChangeWatchedFiles may follow
    MCGuiStep("the new file went unreported", [] {
        GCHECK(changes(kServer).empty());
        MCGuiSetSettings("lsp.enabled = false\n");
        ed()->closeAllTabs();
    });
    MCGuiWait("the server is gone", 10, [] { return LspRunningServerCount() == 0; });

    // ------------------------------------------------ a branch switch's batch
    MCGuiStep("a server with twelve watchers", [] {
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + MCGuiFakeLsp(kBig, bigScript()) +
                         "\nlsp.python.also = off\n");
        showTab(F("a.py"));
    });
    MCGuiWait("its folders are watched", 20, [] { return lsp()->watchedFolders().size() == 2; });
    MCGuiStep("20,000 changes in one batch, half of them wanted", [] {
        gMatchedHere = lsp()->watchedBatchesMatchedHere();
        std::vector<std::string> paths;
        paths.reserve(kBigCount);
        for (int i = 0; i < kBigCount / 2; i++) {
            const std::string dir = gRoot + "/gen/d" + std::to_string(i % 97) + "/m" + std::to_string(i);
            paths.push_back(dir + ".py");
            paths.push_back(dir + ".dat");
        }
        lsp()->injectWatchedPaths(paths, 2);
    });
    MCGuiWait("the server gets the 10,000 it wants", 30, [] {
        size_t n = 0;
        for (const Json& m : MCGuiFakeLspMessages(kBig))
            if (m["method"].asString() == "workspace/didChangeWatchedFiles")
                n += m["params"]["changes"].size();
        return n == (size_t)kBigCount / 2;
    });
    MCGuiStep("matched off the main thread, which only wrote the message", [] {
        GCHECK(lsp()->watchedBatchesMatchedHere() == gMatchedHere);
        MCGuiNote("20,000 changes against 12 watchers: %.2f ms of the main thread's CPU",
                  lsp()->lastWatchedBatchCpuMs());
        GCHECK_TIME(lsp()->lastWatchedBatchCpuMs(), 15, "the batch on the main thread");
        GCHECK(!mentions(kBig, ".dat"));
        MCGuiSetSettings(gSettings);
        ed()->closeAllTabs();
    });
    MCGuiWait("that server is gone, and nothing is watched", 10, [] {
        return LspRunningServerCount() == 0 && lsp()->watchedFolders().empty();
    });
}
