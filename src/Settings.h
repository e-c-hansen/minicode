// Settings.h — pure C++. MiniCode's settings file: parsing it, and resolving
// the colors each part of the window is drawn with.
//
// The file is plain `key = value` lines. Blank lines and lines starting with
// `#` are comments, and a value may be followed by a `# comment`. Keys are
// case-insensitive; if one appears twice the last wins. A bad line is
// reported with its line number and otherwise ignored, so one typo never
// throws away the rest of the file.
//
// Colors are #RGB, #RRGGBB or #RRGGBBAA. Opacity is a number from 0 to 1 or a
// percentage (80%). A surface's background color is drawn at its opacity
// (falling back to window.opacity), while text keeps its own alpha, so text
// stays solid over a see-through panel.
#pragma once
#include "SyntaxHighlighter.h"   // TokenStyle
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct Rgba {
    uint8_t r = 0, g = 0, b = 0;
    double a = 1.0;              // 0 transparent … 1 opaque

    static Rgba hex(uint32_t rgb, double alpha = 1.0) {
        Rgba c;
        c.r = (rgb >> 16) & 0xFF; c.g = (rgb >> 8) & 0xFF; c.b = rgb & 0xFF;
        c.a = alpha;
        return c;
    }
    uint32_t rgb() const { return (uint32_t(r) << 16) | (uint32_t(g) << 8) | b; }
    bool operator==(const Rgba& o) const {
        return r == o.r && g == o.g && b == o.b && a == o.a;
    }
    bool operator!=(const Rgba& o) const { return !(*this == o); }
};

// The parts of the window with their own background and text color.
enum class Surface { Titlebar, Sidebar, Editor, Terminal, Browser, Statusbar };
constexpr int kSurfaceCount = 6;

enum class MarkdownColor { Heading, Link, Code, Quote, Done };
constexpr int kMarkdownColorCount = 5;

// A color value on one line of the settings file, in UTF-16 offsets.
struct ColorSpan {
    size_t start, length;
    Rgba color;
};

struct SettingsError {
    int line;                    // 1-based
    std::string message;
};

// A part of the window as a right click names it, and the settings keys that
// color it, for "Adjust Appearance…": the settings file opens at the first
// key and all of them are marked. Keys of its own come first, in the order
// they appear in defaultFileText, then the window.* keys it falls back to.
struct AppearanceSurface {
    const char* id;                  // "tree", "editor", "terminal", ...
    const char* title;               // "File Tree", for menus
    std::vector<const char*> keys;
    bool placeholder;                // for views not built yet; kept out of menus
};

// A value on one line of the settings file, in UTF-16 offsets.
struct ValueSpan {
    size_t start = 0, length = 0;
};

class Settings {
public:
    // Parse the file's text. Problems are appended to `errors` if given.
    static Settings parse(const std::string& text,
                          std::vector<SettingsError>* errors = nullptr);

    // The file written the first time settings are opened: every key, with
    // its default, commented out.
    static const char* defaultFileText();

    static bool parseColor(const std::string& s, Rgba& out);
    static bool parseOpacity(const std::string& s, double& out);
    static const char* surfaceName(Surface s);

    // Editing support for the settings file in the editor. findColor finds
    // the color value of a "key = #color" line, commented out or not.
    static bool findColor(const std::u16string& line, ColorSpan& out);
    // #RRGGBB, or #RRGGBBAA when not fully opaque. Uppercase.
    static std::string formatColor(const Rgba& c);
    // The line with its color replaced, and uncommented if it was a
    // commented-out setting ("# editor.text = #D4D4D4"), so a picked color
    // takes effect. Lines without a color come back unchanged.
    static std::u16string setColor(const std::u16string& line, const Rgba& c);
    // The value of a "key = value" line, commented out or not: the first word
    // after the '='. False for a line with no key or no value.
    static bool findValue(const std::u16string& line, ValueSpan& out);
    // The value of a "*.opacity = 0.8" line (or 80%), commented out or not.
    static bool findOpacity(const std::u16string& line, ValueSpan& out, double& value);
    // The line with its opacity replaced (as a percentage if it was written
    // as one, else a decimal with at most two places) and uncommented, as
    // setColor does. Lines without an opacity come back unchanged.
    static std::u16string setOpacity(const std::u16string& line, double value);

    // Every surface "Adjust Appearance…" knows, and one by id (or null).
    static const std::vector<AppearanceSurface>& appearanceSurfaces();
    static const AppearanceSurface* appearanceSurface(const std::string& id);
    // The 0-based line of the settings text that holds `key`: the last line
    // that sets it (the one that wins), else the first commented-out
    // "# key = ..." line. -1 when the key is nowhere in the text.
    static int findKeyLine(const std::string& text, const std::string& key);

    // Black or white text, whichever reads better on a swatch of `c` drawn
    // over the editor background.
    Rgba contrastText(const Rgba& c) const;

    // A surface's background color with its opacity applied.
    Rgba background(Surface s) const;
    // surface.opacity, else window.opacity, else 1.
    double opacity(Surface s) const;
    // surface.text, else window.text, else the built-in default.
    Rgba text(Surface s) const;
    // Whether surface.text or window.text was given. The browser only
    // recolors its URL bar when asked, since that field follows the system.
    bool textIsSet(Surface s) const;

    Rgba syntax(TokenStyle s) const;          // Plain is the editor's text color
    Rgba markdown(MarkdownColor c) const;
    Rgba markdownCodeBackground() const;      // follows the editor's opacity
    // The terminal's input line draws no background of its own (it is part of
    // the panel), only its text, a little brighter than the output's.
    Rgba terminalInputText() const;

    bool blur() const { return blur_; }
    const std::string& material() const { return material_; }
    // False when anything behind the window can show through.
    bool windowIsOpaque() const;
    // The title bar is drawn by MiniCode rather than the system: when any
    // titlebar key is set, or the window isn't opaque (a system title bar
    // would sit there as a solid strip).
    bool customTitlebar() const;

    // markdown.web-images: whether the Markdown preview fetches pictures
    // from https addresses (on by default). Off, they show their alt text.
    bool webImages() const { return webImages_; }
    // markdown.math: whether the Markdown preview typesets math (on by
    // default, where tectonic is found). Off, formulas show as their TeX.
    bool math() const { return math_; }

    // Language servers. lsp.enabled turns the whole client on or off (on by
    // default). lsp.<server> names the command for one server key (cpp,
    // python, go, rust, typescript): a whole command line, arguments and all,
    // unlike every other value. "" when the file names none, so the built-in
    // search applies.
    bool lspEnabled() const { return lspEnabled_; }
    std::string lspCommand(const std::string& server) const;
    // lsp.<server> = off (or none, false): leave that language alone.
    bool lspOff(const std::string& server) const;
    static const std::vector<std::string>& lspServers();
    // python.interpreter: the Python the Python language server analyses
    // with, for folders that have not chosen their own (Select Python
    // Interpreter). A path, which may hold spaces; "" (or auto) lets
    // MiniCode pick: the folder's .venv, venv or env, then $VIRTUAL_ENV,
    // then the server's own choice.
    const std::string& pythonInterpreter() const { return pythonInterpreter_; }

    // Run File (Shift+Cmd+Return). run.terminal = new runs each file in a new
    // terminal; current (the default) types the command into the terminal on
    // show. run.<language> is a command template for a language, a whole
    // command line like lsp.<server> (see RunFile.h for its placeholders);
    // off leaves a language without one.
    bool runInNewTerminal() const { return runInNewTerminal_; }
    // The template for a file extension (lowercase, no dot): run.<extension>,
    // else run.<its language>, else the built-in default. "" when there is
    // none or it is off.
    std::string runTemplate(const std::string& extension) const;
    // The language an extension's run command is listed under: python for
    // py, sh for bash, and so on; the extension itself otherwise.
    static std::string runLanguage(const std::string& extension);
    // Format Document. format.<lang> (python, cpp, go, rust, shell; see
    // Formatter.h) is a whole command line like lsp.*, "" when the file
    // names none, so the project's configuration and then Google's style
    // apply. format.on-save formats before Cmd+S writes the file (off by
    // default).
    std::string formatCommand(const std::string& lang) const;
    bool formatOnSave() const { return formatOnSave_; }
    static const std::vector<std::string>& formatLanguages();

private:
    struct SurfaceSettings {
        std::optional<Rgba> background, text;
        std::optional<double> opacity;
    };
    SurfaceSettings surfaces_[kSurfaceCount];
    std::optional<double> windowOpacity_;
    std::optional<Rgba> windowText_;
    bool blur_ = false;
    std::string material_ = "under-window";
    std::optional<Rgba> syntax_[8];
    std::optional<Rgba> markdown_[kMarkdownColorCount];
    bool titlebarKeySet_ = false;
    bool lspEnabled_ = true;
    bool webImages_ = true;
    bool math_ = true;
    std::map<std::string, std::string> lspCommands_;
    bool runInNewTerminal_ = false;
    std::map<std::string, std::string> runCommands_;
    std::map<std::string, std::string> formatCommands_;
    bool formatOnSave_ = false;
    std::string pythonInterpreter_;

    bool apply(const std::string& key, const std::string& value,
               std::string& error);
};
