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

// A color theme: a color for everything the settings file can color. The
// settings file's `theme` key picks one (dark-plus, minicode's look since
// the start, when unset), and a color set in the file wins over the theme's,
// as workbench.colorCustomizations do in VS Code. The themes are VS Code's
// own built-in ones, read off their theme files and mapped onto minicode's
// fewer keys: a surface's background and text, the seven syntax styles,
// the Markdown preview's colors, its code background, the ruler, the
// terminal's input line, the gutter's line numbers and the bracket match.
struct Theme {
    const char* id;              // "dark-modern", the value of the theme key
    const char* title;           // "Dark Modern", for menus
    bool dark;                   // dark backgrounds: the window's controls go dark too
    uint32_t background[kSurfaceCount], text[kSurfaceCount];   // indexed by Surface
    uint32_t syntax[8];          // indexed by TokenStyle; [0] is unused (editor text)
    uint32_t markdown[kMarkdownColorCount];
    uint32_t codeBackground, ruler, terminalInput;
    uint32_t lineNumber, lineNumberActive, bracketMatch;
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

    // Every theme, in menu order (dark ones, then light ones), and one by id
    // (null for an unknown id).
    static const std::vector<Theme>& themes();
    static const Theme* findTheme(const std::string& id);
    // The settings text with `theme = id` in it: the line that sets the theme
    // changed, else the commented-out "# theme = ..." line uncommented, else a
    // new line after the file's opening comment. With commentColors, every
    // line that sets a color of its own (and so would cover the theme's) is
    // commented out too, so the theme shows whole; nothing else changes.
    static std::string withTheme(const std::string& text, const std::string& id,
                                 bool commentColors);
    // How many lines of the text set a color (uncommented), which a theme's
    // color would give way to.
    static int colorOverrides(const std::string& text);
    // Whether `key` (lowercase) takes a color: *.background, *.text,
    // syntax.*, markdown's colors, editor.ruler-color, the gutter's and the
    // bracket match's.
    static bool isColorKey(const std::string& key);

    // Every surface "Adjust Appearance…" knows, and one by id (or null).
    static const std::vector<AppearanceSurface>& appearanceSurfaces();
    static const AppearanceSurface* appearanceSurface(const std::string& id);
    // The 0-based line of the settings text that holds `key`: the last line
    // that sets it (the one that wins), else the first commented-out
    // "# key = ..." line. -1 when the key is nowhere in the text.
    static int findKeyLine(const std::string& text, const std::string& key);
    // The lines of defaultFileText for a surface's keys that `text` has
    // nowhere, not even commented out (a file written before those keys
    // existed), each with the comment lines just above it there, in the
    // default file's order and with a blank line between its sections. ""
    // when none are missing or the surface is unknown.
    static std::string missingSurfaceLines(const std::string& text, const std::string& surface);

    // Black or white text, whichever reads better on a swatch of `c` drawn
    // over the editor background.
    Rgba contrastText(const Rgba& c) const;

    // The theme the file picks (dark-plus when it names none).
    const Theme& theme() const { return *theme_; }
    // Whether the editor's background, as resolved, is dark: the window's
    // own controls (scroll bars, menus in it, text fields) follow this.
    bool isDark() const;

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

    // editor.keep-sorted-on-save: sort every keep-sorted block in a file
    // when it is saved (off by default).
    bool keepSortedOnSave() const { return keepSortedOnSave_; }
    // editor.auto-save: off (the default), after-delay (each file with
    // unsaved changes is written once typing has paused for
    // editor.auto-save-delay milliseconds, 1000 by default, as VS Code's
    // files.autoSave afterDelay), or focus-change (written when the editor
    // loses the keyboard: another pane, another tab, another window or app).
    // Auto-saves never format and never sort keep-sorted blocks, and never
    // write over a file changed on disk since it was read.
    enum class AutoSave { Off, AfterDelay, FocusChange };
    AutoSave autoSave() const { return autoSave_; }
    int autoSaveDelay() const { return autoSaveDelay_; }
    // editor.ruler: the columns a thin vertical line marks in the editor,
    // ascending ({100} by default; empty for off). editor.ruler-color is the
    // line's color.
    const std::vector<int>& rulers() const { return rulers_; }
    Rgba rulerColor() const;
    // editor.font-size: the editor's font in points (13 by default, 6 to
    // 72). Zooming (Cmd+= and Cmd+-) adds to it without touching the file.
    double editorFontSize() const { return editorFontSize_; }
    static constexpr double kMinFontSize = 6, kMaxFontSize = 72;
    // editor.line-numbers: the gutter beside the editor with each line's
    // number (on, the default), none (off), or numbers counted from the
    // caret's line, which shows its own (relative, as VS Code's).
    enum class LineNumbers { Off, On, Relative };
    LineNumbers lineNumbers() const { return lineNumbers_; }
    // The gutter's background: editor.gutter-background, else the editor's
    // own; editor.opacity applies to either.
    Rgba gutterBackground() const;
    // editor.line-number-color and editor.line-number-active-color (the
    // number of the caret's line), from the theme unless set.
    Rgba lineNumberColor() const;
    Rgba lineNumberActiveColor() const;
    // editor.bracket-match: the box drawn around the bracket beside the
    // caret and its partner (filled with the same color, faintly).
    Rgba bracketMatchColor() const;

    // The terminal. terminal.mode: grid (the default) makes each new
    // terminal a real one, the cell grid with zsh's own line editor, prompt
    // and key bindings; line is the older log with an input line below it,
    // kept for a release as a fallback. A change applies to terminals opened
    // after it. terminal.scrollback: lines kept above a grid terminal's
    // screen (10000 by default, 0 for none, at most 100000).
    // terminal.option-as-meta: Option sends ESC before the key, for the
    // shell's Meta bindings (true by default); false lets Option type the
    // characters the keyboard layout puts on it (@ { | on many European
    // layouts) and dead keys.
    bool terminalLineMode() const { return terminalLineMode_; }
    int terminalScrollback() const { return terminalScrollback_; }
    bool terminalOptionAsMeta() const { return terminalOptionAsMeta_; }

    // Language servers. lsp.enabled turns the whole client on or off (on by
    // default). lsp.<server> names the command for one server key (cpp,
    // python, go, rust, typescript): a whole command line, arguments and all,
    // unlike every other value. "" when the file names none, so the built-in
    // search applies.
    bool lspEnabled() const { return lspEnabled_; }
    std::string lspCommand(const std::string& server) const;
    // lsp.<server> = off (or none, false): leave that language alone.
    // lsp.<server>.also is a second server beside the main one, read the
    // same way with "<server>.also" ("" for unset or auto: the built-in
    // rule, Lsp::defaultAlsoCommand).
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
    // Request a Change to minicode (macOS). develop.command is the Claude
    // Code command line the request is handed to, a whole command line like
    // lsp.* ("claude" when unset). develop.source is minicode's source
    // folder, a path that may hold spaces (a leading ~ is the GUI's to
    // expand); "" lets minicode find it (the folder its own build is in,
    // else its own clone in Application Support, made on first use).
    std::string developCommand() const {
        return developCommand_.empty() ? "claude" : developCommand_;
    }
    const std::string& developSource() const { return developSource_; }
    // The inline agent (Cmd+I, macOS). agent.command is the Claude Code
    // command line it runs, a whole command line like lsp.* ("claude" when
    // unset): print mode and the read-only tools are added to it for an
    // answer in the bar, and it runs as written, with the request, for a
    // task handed to a terminal.
    std::string agentCommand() const {
        return agentCommand_.empty() ? "claude" : agentCommand_;
    }
    static const std::vector<std::string>& formatLanguages();

private:
    const Theme* theme_ = findTheme("dark-plus");
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
    bool keepSortedOnSave_ = false;
    AutoSave autoSave_ = AutoSave::Off;
    int autoSaveDelay_ = 1000;
    std::vector<int> rulers_ = {100};
    std::optional<Rgba> rulerColor_;
    double editorFontSize_ = 13;
    LineNumbers lineNumbers_ = LineNumbers::On;
    std::optional<Rgba> gutterBackground_, lineNumberColor_, lineNumberActiveColor_,
        bracketMatchColor_;
    bool terminalLineMode_ = false;
    int terminalScrollback_ = 10000;
    bool terminalOptionAsMeta_ = true;
    std::map<std::string, std::string> lspCommands_;
    bool runInNewTerminal_ = false;
    std::map<std::string, std::string> runCommands_;
    std::map<std::string, std::string> formatCommands_;
    bool formatOnSave_ = false;
    std::string pythonInterpreter_;
    std::string developCommand_;
    std::string developSource_;
    std::string agentCommand_;

    bool apply(const std::string& key, const std::string& value,
               std::string& error);
};
