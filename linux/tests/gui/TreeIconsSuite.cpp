// TreeIconsSuite.cpp — the file tree's icons by type (`./build/mutible-guitest
// tree_icons`): a row's icon is the first of its type's names
// (FileIcon::typeOf, TreeIcons::candidates) the icon theme has. A folder
// holds one file of each type (the window's tree shows dotfiles for the
// suite: .gitignore, .editorconfig); each row's icon name, read off its image, is
// checked to be in its type's family and to be the theme's answer, and not
// every row may show the plain text icon. Then, where Ubuntu's Yaru theme is
// installed, the app is switched to it (as a user switching themes): the
// rows on show take its names, the main ones are checked by name, how many
// types get a picture of their own is counted (by the image file each name
// draws), and the tree is snapshotted (MC_GUI_SNAPSHOTS). The theme goes
// back afterwards.
#include "Harness.h"

#include "Editor.h"
#include "FileDetails.h"
#include "FileTree.h"
#include "TreeIcons.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <sys/stat.h>

namespace {

std::string dir() { return MCGuiProjectFile("0-tree-icons"); }
std::string F(const std::string& name) { return dir() + "/" + name; }

// One file of each type, and what Yaru draws for the main ones ("" for no
// name checked).
struct Want { const char* file; const char* type; const char* yaru; };
const Want kWant[] = {
    {"notes.txt", "text", "text-x-generic"},      {"schema.proto", "code", "text-x-script"},
    {"main.c", "c", "text-x-csrc"},               {"main.h", "c-header", "text-x-chdr"},
    {"main.cpp", "cpp", "text-x-c++src"},         {"main.hpp", "cpp-header", "text-x-c++hdr"},
    {"app.mm", "objc", "text-x-csrc"},            {"app.cs", "csharp", "text-x-csharp"},
    {"tool.py", "python", "text-x-python"},       {"app.js", "javascript", "text-x-javascript"},
    {"app.ts", "typescript", "text-x-typescript"}, {"lib.rs", "rust", "text-rust"},
    {"main.go", "go", "text-x-go"},               {"App.java", "java", "text-x-java"},
    {"App.kt", "kotlin", "text-x-kotlin"},        {"App.swift", "swift", "text-x-script"},
    {"app.rb", "ruby", "text-x-ruby"},            {"index.php", "php", "application-x-php"},
    {"tool.pl", "perl", "application-x-perl"},    {"init.lua", "lua", "text-x-lua"},
    {"run.sh", "shell", "application-x-shellscript"}, {"Main.hs", "haskell", "text-x-haskell"},
    {"App.scala", "scala", "text-x-scala"},       {"main.dart", "dart", "text-x-dart"},
    {"plot.r", "r", "text-x-r"},                  {"fit.jl", "julia", ""},
    {"app.ex", "elixir", ""},                     {"app.erl", "erlang", ""},
    {"build.zig", "zig", ""},                     {"app.nim", "nim", "text-x-nim"},
    {"app.vala", "vala", "text-x-vala"},          {"solve.f90", "fortran", "text-x-fortran"},
    {"main.ml", "ocaml", ""},                     {"core.clj", "lisp", ""},
    {"Main.qml", "qml", "text-x-qml"},            {"App.vue", "vue", "text-html"},
    {"notes.md", "markdown", "text-markdown"},    {"index.html", "html", "text-html"},
    {"site.css", "css", "text-css"},              {"site.scss", "sass", "text-x-scss"},
    {"site.less", "less", "text-less"},           {"conf.json", "json", "application-json"},
    {"ci.yml", "yaml", "application-yaml"},       {"Cargo.toml", "toml", "application-toml"},
    {"pom.xml", "xml", "text-xml"},               {"data.csv", "csv", "text-csv"},
    {"q.sql", "sql", "text-x-sql"},               {"paper.tex", "latex", "text-x-tex"},
    {"run.ipynb", "notebook", "application-x-ipynb+json"},
    {"shot.png", "image", "image-x-generic"},     {"logo.svg", "svg", "image-svg+xml"},
    {"song.mp3", "audio", "audio-x-generic"},     {"clip.mp4", "video", "video-x-generic"},
    {"paper.pdf", "pdf", "application-pdf"},      {"src.zip", "archive", "package-x-generic"},
    {"Inter.ttf", "font", "font-x-generic"},      {"setup.exe", "executable", "application-x-executable"},
    {"libz.so", "library", "application-x-sharedlib"}, {"app.sqlite", "database", "application-x-sqlite3"},
    {"memo.docx", "document", "x-office-document"}, {"sheet.xlsx", "spreadsheet", "x-office-spreadsheet"},
    {"deck.pptx", "presentation", "x-office-presentation"}, {"book.epub", "ebook", "application-epub+zip"},
    {"Makefile", "makefile", "text-x-makefile"},  {"CMakeLists.txt", "cmake", "text-x-cmake"},
    {"meson.build", "meson", "text-x-meson"},     {"Dockerfile", "dockerfile", "text-dockerfile"},
    {"LICENSE", "license", "text-x-copying"},     {"README.md", "readme", "text-x-readme"},
    {"AUTHORS", "authors", "text-x-authors"},     {"Cargo.lock", "lockfile", "object-locked"},
    {".gitignore", "git", "text-x-patch"},        {".editorconfig", "config", "configure"},
    {"fix.patch", "patch", "text-x-patch"},       {"build.log", "log", "text-x-log"},
    {"cert.pem", "key", "application-pgp-keys"},  {"docs.url", "bookmark", "application-x-mswinurl"},
    {"fr.po", "translation", "text-x-gettext-translation"},
    {"tool", "executable", "application-x-executable"},   // no extension, executable
};

FileTree* Tree() { return MCGuiApp().tree; }
GtkIconTheme* IconTheme() { return gtk_icon_theme_get_for_display(gdk_display_get_default()); }

FileIcon::Type TypeNamed(const std::string& name) {
    for (FileIcon::Type t : FileIcon::allTypes())
        if (name == FileIcon::typeName(t)) return t;
    return FileIcon::Type::Text;
}

// The row's name label for `path`, on screen.
GtkWidget* RowLabel(const std::string& path, GtkWidget* root = nullptr) {
    if (!root) root = Tree()->widget();
    if (GTK_IS_LABEL(root) && gtk_widget_has_css_class(root, "minicode-tree-label")) {
        const char* p = static_cast<const char*>(g_object_get_data(G_OBJECT(root), "minicode-path"));
        return p && path == p && MCGuiOnScreen(root) ? root : nullptr;
    }
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = RowLabel(path, c)) return hit;
    return nullptr;
}

// The icon name of the image beside a row's label, "" when none.
std::string IconOf(const std::string& path) {
    GtkWidget* label = RowLabel(path);
    GtkWidget* image = label ? gtk_widget_get_prev_sibling(label) : nullptr;
    const char* n = image && GTK_IS_IMAGE(image) ? gtk_image_get_icon_name(GTK_IMAGE(image)) : nullptr;
    return n ? n : "";
}

// A full run leaves the project folder with a hundred entries and the tree
// scrolled anywhere, and its list view makes rows only for what is on screen,
// so each row is revealed (its folder expanded, the row scrolled to) before
// it is read. The folder may still be arriving from the file monitor, so the
// reveal is asked again until the row is there.
bool Shown(const std::string& path) {
    if (RowLabel(path)) return true;
    Tree()->revealPath(path);
    return RowLabel(path) != nullptr;
}

// The file each name draws at the tree's size in the theme, its links
// followed (Yaru's text-x-c is a link to text-x-csrc's picture).
std::string ImageOf(const std::string& name) {
    GtkIconPaintable* p = gtk_icon_theme_lookup_icon(IconTheme(), name.c_str(), nullptr, 16, 1,
                                                     GTK_TEXT_DIR_LTR, (GtkIconLookupFlags)0);
    GFile* f = p ? gtk_icon_paintable_get_file(p) : nullptr;
    char* path = f ? g_file_get_path(f) : nullptr;
    std::string out = path ? path : name;
    if (path) {
        if (char* real = realpath(path, nullptr)) {
            out = real;
            free(real);
        }
    }
    g_free(path);
    if (f) g_object_unref(f);
    if (p) g_object_unref(p);
    return out;
}

// Every row in its type's family and the theme's answer for it.
void CheckRows(const std::string& what) {
    for (const Want& w : kWant) {
        std::string file = w.file, type = w.type;
        MCGuiWait(file + "'s row is on show", 20, [file] { return Shown(F(file)); });
        MCGuiStep(what + ": " + file + " (" + type + ")", [file, type] {
            const FileIcon::Type t = TypeNamed(type);
            const std::string icon = IconOf(F(file));
            const std::vector<const char*>& family = TreeIcons::candidates(t);
            const bool inFamily = icon == "text-x-generic-symbolic" ||
                std::any_of(family.begin(), family.end(), [&](const char* n) { return icon == n; });
            if (!inFamily) MCGuiNote("%s: %s is not among %s's names", file.c_str(), icon.c_str(), type.c_str());
            GCHECK(inFamily);
            GCHECK_EQ_STR(icon, std::string(TreeIcons::resolve(IconTheme(), t)));
        });
    }
}

std::string gThemeBefore;
bool gYaru = false, gHiddenBefore = false;

}  // namespace

MC_GUI_SUITE(tree_icons) {
    MCGuiStep("a folder with one file of each type", [] {
        for (const Want& w : kWant) MCGuiWriteFile(F(w.file), "x\n");
        chmod(F("tool").c_str(), 0755);
        // .gitignore and .editorconfig are dotfiles: this window's tree shows
        // them for the suite (the app's own switch is left alone).
        gHiddenBefore = Tree()->showHidden();
        Tree()->setShowHidden(true);
        char* name = nullptr;
        g_object_get(gtk_settings_get_default(), "gtk-icon-theme-name", &name, nullptr);
        gThemeBefore = name ? name : "";
        g_free(name);
        MCGuiNote("the icon theme: %s", gThemeBefore.c_str());
    });
    CheckRows("the harness's theme");
    MCGuiStep("not every row shows the plain text icon", [] {
        std::set<std::string> icons;
        for (const Want& w : kWant) icons.insert(TreeIcons::resolve(IconTheme(), TypeNamed(w.type)));
        MCGuiNote("%zu names among %zu types", icons.size(), sizeof kWant / sizeof kWant[0]);
        GCHECK(icons.size() > 3);
    });
    MCGuiWait("the folder's row is on show", 20, [] { return Shown(dir()); });
    MCGuiStep("a folder keeps the folder icon", [] {
        GCHECK_EQ_STR(IconOf(dir()), std::string("folder-symbolic"));
    });

    // ---- Yaru, Ubuntu's theme, where it is installed
    MCGuiStep("Yaru, where installed", [] {
        // Installed where a folder GTK looks in holds Yaru/index.theme.
        char** dirs = gtk_icon_theme_get_search_path(IconTheme());
        for (char** d = dirs; d && *d; d++)
            if (MCGuiFileExists(std::string(*d) + "/Yaru/index.theme")) gYaru = true;
        g_strfreev(dirs);
        MCGuiNote(gYaru ? "Yaru is installed: the rows switch to it" : "no Yaru here: its checks are skipped");
        if (gYaru) g_object_set(gtk_settings_get_default(), "gtk-icon-theme-name", "Yaru", nullptr);
    });
    MCGuiWait("the rows take Yaru's names", 10, [] {
        return !gYaru || (Shown(F("tool.py")) && IconOf(F("tool.py")) == "text-x-python");
    });
    for (const Want& w : kWant) {
        std::string file = w.file, yaru = w.yaru;
        MCGuiWait(file + "'s row is on show", 20, [file] { return !gYaru || Shown(F(file)); });
        MCGuiStep("Yaru: " + file, [file, yaru] {
            if (!gYaru) return;
            if (!yaru.empty()) GCHECK_EQ_STR(IconOf(F(file)), yaru);
        });
    }
    MCGuiStep("Yaru: the types with a picture of their own", [] {
        if (!gYaru) return;
        std::set<std::string> images;
        int own = 0;
        const std::string plain = ImageOf("text-x-generic");
        std::string list;
        for (FileIcon::Type t : FileIcon::allTypes()) {
            const std::string name = TreeIcons::resolve(IconTheme(), t);
            const std::string image = ImageOf(name);
            images.insert(image);
            if (image != plain) own++;
            else list += std::string(list.empty() ? "" : ", ") + FileIcon::typeName(t);
        }
        MCGuiNote("Yaru: %d of %zu types drawn apart from plain text, %zu pictures in all; plain: %s", own,
                  FileIcon::allTypes().size(), images.size(), list.c_str());
        GCHECK(own >= 60);
        GCHECK(images.size() >= 50);
    });
    MCGuiStep("snapshots of the tree in Yaru", [] {
        if (!gYaru) return;
        Tree()->revealPath(F("App.java"));
    });
    MCGuiWait("App.java on show", 10, [] { return !gYaru || RowLabel(F("App.java")) != nullptr; });
    MCGuiStep("the first", [] { if (gYaru) MCGuiSnapshot(Tree()->widget(), "tree-icons-yaru-1"); });
    MCGuiStep("further down", [] { if (gYaru) Tree()->revealPath(F("main.go")); });
    MCGuiWait("main.go on show", 10, [] { return !gYaru || RowLabel(F("main.go")) != nullptr; });
    MCGuiStep("the second", [] { if (gYaru) MCGuiSnapshot(Tree()->widget(), "tree-icons-yaru-2"); });
    MCGuiStep("further down", [] { if (gYaru) Tree()->revealPath(F("tool.py")); });
    MCGuiWait("tool.py on show", 10, [] { return !gYaru || RowLabel(F("tool.py")) != nullptr; });
    MCGuiStep("the third", [] { if (gYaru) MCGuiSnapshot(Tree()->widget(), "tree-icons-yaru-3"); });
    MCGuiStep("the theme back", [] {
        if (gYaru)
            g_object_set(gtk_settings_get_default(), "gtk-icon-theme-name",
                         gThemeBefore.empty() ? nullptr : gThemeBefore.c_str(), nullptr);
    });
    MCGuiWait("the rows take its names again", 10, [] {
        return !gYaru || (Shown(F("tool.py")) &&
                          IconOf(F("tool.py")) == TreeIcons::resolve(IconTheme(), FileIcon::Type::Python));
    });
    // The folder goes again, so later suites find the tree as they would
    // have without it.
    MCGuiStep("the folder removed; hidden files as they were", [] {
        std::filesystem::remove_all(dir());
        Tree()->setShowHidden(gHiddenBefore);
    });
    MCGuiWait("its row gone from the tree", 20, [] { return RowLabel(dir()) == nullptr; });
}
