// TreeIcons.cpp — see TreeIcons.h.
#include "TreeIcons.h"

#include <unordered_map>

namespace TreeIcons {

namespace {

using FileIcon::Type;

// Each type's names, best first, before the common tail. Where Yaru (Ubuntu's
// theme) has a name of its own it comes first or second: what each name
// draws in Yaru, Papirus and Adwaita is in docs/notes/tree.md.
const std::vector<std::vector<const char*>>& table() {
    static const std::vector<std::vector<const char*>> t = [] {
        std::vector<std::vector<const char*>> v(FileIcon::allTypes().size());
        auto set = [&v](Type type, std::initializer_list<const char*> names) {
            v[(size_t)type] = names;
        };
        // A source file of a language the theme does not draw is a script.
        auto lang = [&v](Type type, std::initializer_list<const char*> names) {
            v[(size_t)type] = names;
            v[(size_t)type].push_back("text-x-script");
        };
        set(Type::Text, {"text-x-generic"});
        set(Type::Code, {"text-x-script"});
        lang(Type::C, {"text-x-csrc", "text-x-c"});
        lang(Type::CHeader, {"text-x-chdr", "text-x-csrc"});
        lang(Type::Cpp, {"text-x-c++src", "text-x-cpp", "text-x-csrc"});
        lang(Type::CppHeader, {"text-x-c++hdr", "text-x-chdr"});
        lang(Type::ObjC, {"text-x-objcsrc", "text-x-csrc"});
        lang(Type::CSharp, {"text-x-csharp"});
        lang(Type::Python, {"text-x-python", "text-x-python3"});
        lang(Type::JavaScript, {"text-x-javascript", "application-javascript"});
        lang(Type::TypeScript, {"text-x-typescript", "application-typescript"});
        lang(Type::Rust, {"text-rust", "text-x-rust"});
        lang(Type::Go, {"text-x-go"});
        lang(Type::Java, {"text-x-java", "text-x-java-source", "application-x-java"});
        lang(Type::Kotlin, {"text-x-kotlin"});
        lang(Type::Swift, {"text-x-swift"});
        lang(Type::Ruby, {"text-x-ruby", "application-x-ruby"});
        lang(Type::Php, {"application-x-php", "text-x-php"});
        lang(Type::Perl, {"application-x-perl", "text-x-perl"});
        lang(Type::Lua, {"text-x-lua"});
        lang(Type::Shell, {"application-x-shellscript"});
        lang(Type::Haskell, {"text-x-haskell"});
        lang(Type::Scala, {"text-x-scala"});
        lang(Type::Dart, {"text-x-dart", "application-vnd-dart"});
        lang(Type::R, {"text-x-r", "text-x-r-source"});
        lang(Type::Julia, {"text-julia", "text-x-julia"});
        lang(Type::Elixir, {"text-x-elixir"});
        lang(Type::Erlang, {"text-x-erlang"});
        lang(Type::Zig, {"text-x-zig"});
        lang(Type::Nim, {"text-x-nim"});
        lang(Type::Vala, {"text-x-vala"});
        lang(Type::Fortran, {"text-x-fortran"});
        lang(Type::OCaml, {"text-x-ocaml"});
        lang(Type::Lisp, {"text-x-common-lisp", "text-x-scheme", "text-x-emacs-lisp"});
        lang(Type::Qml, {"text-x-qml"});
        lang(Type::Vue, {"text-x-vue", "text-html"});
        set(Type::Markdown, {"text-markdown", "text-x-markdown"});
        set(Type::Html, {"text-html"});
        set(Type::Css, {"text-css"});
        set(Type::Sass, {"text-x-scss", "text-x-sass", "text-css"});
        set(Type::Less, {"text-less", "text-x-less", "text-css"});
        set(Type::Json, {"application-json"});
        set(Type::Yaml, {"application-yaml", "application-x-yaml"});
        set(Type::Toml, {"application-toml"});
        set(Type::Xml, {"text-xml", "application-xml"});
        set(Type::Csv, {"text-csv", "x-office-spreadsheet"});
        lang(Type::Sql, {"text-x-sql", "application-sql"});
        set(Type::Latex, {"text-x-tex"});
        set(Type::Notebook, {"application-x-ipynb+json"});
        set(Type::Image, {"image-x-generic"});
        set(Type::Svg, {"image-svg+xml", "image-x-generic"});
        set(Type::Audio, {"audio-x-generic"});
        set(Type::Video, {"video-x-generic"});
        set(Type::Pdf, {"application-pdf", "x-office-document"});
        set(Type::Archive, {"package-x-generic", "application-x-archive"});
        set(Type::Font, {"font-x-generic"});
        set(Type::Executable, {"application-x-executable"});
        set(Type::Library, {"application-x-sharedlib", "application-x-executable"});
        set(Type::Database, {"application-x-sqlite3", "office-database", "text-x-sql"});
        set(Type::Document, {"x-office-document"});
        set(Type::Spreadsheet, {"x-office-spreadsheet"});
        set(Type::Presentation, {"x-office-presentation"});
        set(Type::Ebook, {"application-epub+zip", "x-office-document"});
        set(Type::Makefile, {"text-x-makefile"});
        set(Type::CMake, {"text-x-cmake", "text-x-makefile"});
        set(Type::Meson, {"text-x-meson", "text-x-makefile"});
        lang(Type::Dockerfile, {"text-dockerfile", "text-x-dockerfile"});
        set(Type::License, {"text-x-copying"});
        set(Type::Readme, {"text-x-readme"});
        set(Type::Authors, {"text-x-authors"});
        // No theme has a lockfile's or git's own mimetype: a lock, git's
        // logo (Papirus), a patch.
        set(Type::Lockfile, {"object-locked", "system-lock-screen-symbolic"});
        set(Type::Git, {"git", "text-x-patch", "text-x-diff"});
        set(Type::Config, {"configure", "preferences-system", "emblem-system-symbolic"});
        set(Type::Patch, {"text-x-patch", "text-x-diff"});
        set(Type::Log, {"text-x-log"});
        set(Type::Key, {"application-pgp-keys", "application-certificate"});
        set(Type::Bookmark, {"application-x-mswinurl", "user-bookmarks"});
        set(Type::Translation, {"text-x-gettext-translation", "text-x-po"});
        for (auto& names : v)
            if (names.empty() || std::string(names.back()) != "text-x-generic")
                names.push_back("text-x-generic");
        return v;
    }();
    return t;
}

// Per theme, each type's answer (null: not asked yet). Forgotten when the
// theme changes; the theme for a display lives as long as the display.
std::unordered_map<GtkIconTheme*, std::vector<const char*>>& answers() {
    static std::unordered_map<GtkIconTheme*, std::vector<const char*>> m;
    return m;
}

void forget(GtkIconTheme* theme, gpointer) { answers().erase(theme); }

}  // namespace

const std::vector<const char*>& candidates(FileIcon::Type t) { return table()[(size_t)t]; }

const char* resolve(GtkIconTheme* theme, FileIcon::Type t) {
    if (!theme) return candidates(t).front();
    auto& all = answers();
    auto it = all.find(theme);
    if (it == all.end()) {
        // The first time this theme is asked: its answers go when it changes.
        if (!g_object_get_data(G_OBJECT(theme), "minicode-tree-icons")) {
            g_object_set_data(G_OBJECT(theme), "minicode-tree-icons", GINT_TO_POINTER(1));
            g_signal_connect(theme, "changed", G_CALLBACK(forget), nullptr);
        }
        it = all.emplace(theme, std::vector<const char*>(FileIcon::allTypes().size(), nullptr)).first;
    }
    const char*& answer = it->second[(size_t)t];
    if (!answer) {
        answer = "text-x-generic-symbolic";
        for (const char* name : candidates(t))
            if (gtk_icon_theme_has_icon(theme, name)) {
                answer = name;
                break;
            }
    }
    return answer;
}

const char* nameFor(GtkWidget* widget, const std::string& path, bool executable) {
    GtkIconTheme* theme = widget ? gtk_icon_theme_get_for_display(gtk_widget_get_display(widget)) : nullptr;
    return resolve(theme, FileIcon::typeOf(path, executable));
}

const char* fallback(GtkIconTheme* theme) { return resolve(theme, FileIcon::Type::Text); }

}  // namespace TreeIcons
