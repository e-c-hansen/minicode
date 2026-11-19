// window_title_tests.inc — the `window-title:*` groups of run_tests.cpp:
// the window's title (src/WindowTitle.h), the file's path from the
// window's folder, ~ for home outside it, untitled buffers, and what the
// terminal and the browser show while they have the keyboard. Included
// there.
#include "WindowTitle.h"

static void testWindowTitle() {
    GROUP("window-title:path");
    {
        using WindowTitle::displayPath;
        const std::string folder = "/Users/me/work", home = "/Users/me";
        CHECK(displayPath("/Users/me/work/prep/snippets/a.json", folder, home) ==
              "prep/snippets/a.json");
        CHECK(displayPath("/Users/me/work/a.txt", folder, home) == "a.txt");
        CHECK(displayPath("/Users/me/work/a.txt", folder + "/", home) == "a.txt");
        // A folder whose name starts the same is not the folder.
        CHECK(displayPath("/Users/me/workshop/a.txt", folder, home) == "~/workshop/a.txt");
        CHECK(displayPath("/Users/me/notes/todo.md", folder, home) == "~/notes/todo.md");
        CHECK(displayPath("/Users/me", folder, home) == "~");
        CHECK(displayPath("/etc/hosts", folder, home) == "/etc/hosts");
        CHECK(displayPath("/Users/me/work", folder, home) == "work");
        // No folder, no home: the whole path.
        CHECK(displayPath("/Users/me/a.txt", "", "") == "/Users/me/a.txt");
        CHECK(displayPath("/Users/me/a.txt", "", home) == "~/a.txt");
        // "/" as the folder or home is no prefix to take off.
        CHECK(displayPath("/etc/hosts", "/", "/") == "/etc/hosts");
    }

    GROUP("window-title:document");
    {
        using namespace WindowTitle;
        Input in;
        in.folder = "/Users/me/work";
        in.home = "/Users/me";
        CHECK(title(in) == "work — mutible");   // no tab: the folder's name
        in.path = "/Users/me/work/prep/snippets/agentic-system-design.json";
        CHECK(title(in) == "prep/snippets/agentic-system-design.json — mutible");
        CHECK(subject(in) == "prep/snippets/agentic-system-design.json");
        in.dirty = true;
        CHECK(title(in) == "● prep/snippets/agentic-system-design.json — mutible");
        in.dirty = false;
        in.gone = true;
        CHECK(title(in) == "prep/snippets/agentic-system-design.json (deleted) — mutible");
        in.gone = false;
        in.detail = "  2 pages";
        CHECK(title(in) == "prep/snippets/agentic-system-design.json — mutible  2 pages");
        in.detail.clear();
        in.path = "/Users/me/notes/todo.md";
        CHECK(title(in) == "~/notes/todo.md — mutible");
        // Untitled: its name, never a path.
        in.path = "/Users/me/work/Untitled-2";
        in.untitled = true;
        CHECK(title(in) == "Untitled-2 — mutible");
        in.path.clear();
        CHECK(title(in) == "Untitled-1 — mutible");
        in.untitled = false;
        // A tab that is no file: its name as given.
        in.name = "a.txt (diff)";
        CHECK(title(in) == "a.txt (diff) — mutible");
        // Nothing at all.
        Input none;
        CHECK(title(none) == "mutible");
    }

    GROUP("window-title:focus");
    {
        using namespace WindowTitle;
        Input in;
        in.folder = "/Users/me/work";
        in.home = "/Users/me";
        in.path = "/Users/me/work/src/main.cpp";
        in.dirty = true;
        in.detail = "  3 of 4 done";
        in.focus = Focus::Terminal;
        in.pane = "vim notes.md";
        // The document's dot and detail stay with the document.
        CHECK(title(in) == "vim notes.md — mutible");
        in.pane.clear();
        CHECK(title(in) == "work — mutible");
        in.focus = Focus::Browser;
        in.pane = "Example Domain";
        CHECK(title(in) == "Example Domain — mutible");
        in.pane.clear();
        in.folder.clear();
        CHECK(title(in) == "mutible");
        in.focus = Focus::Document;
        CHECK(title(in) == "● ~/work/src/main.cpp — mutible  3 of 4 done");
    }
}
