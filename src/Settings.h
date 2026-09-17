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

enum class MarkdownColor { Heading, Link, Code, Quote };
constexpr int kMarkdownColorCount = 4;

// A color value on one line of the settings file, in UTF-16 offsets.
struct ColorSpan {
    size_t start, length;
    Rgba color;
};

struct SettingsError {
    int line;                    // 1-based
    std::string message;
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
    Rgba terminalInputBackground() const;     // follows the terminal's opacity
    Rgba terminalInputText() const;

    bool blur() const { return blur_; }
    const std::string& material() const { return material_; }
    // False when anything behind the window can show through.
    bool windowIsOpaque() const;
    // The title bar is drawn by MiniCode rather than the system: when any
    // titlebar key is set, or the window isn't opaque (a system title bar
    // would sit there as a solid strip).
    bool customTitlebar() const;

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

    bool apply(const std::string& key, const std::string& value,
               std::string& error);
};
