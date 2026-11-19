// ThemeCss.h — pure C++. The application stylesheet, built from the settings
// file. GTK draws panel backgrounds and text from CSS, so this is where
// per-panel colors and opacity turn into something GTK understands. Kept free
// of GTK so it can be unit-tested.
#pragma once
#include "Settings.h"
#include <string>

namespace theme {

// "rgba(30,30,30,0.500)". The alpha is written by hand rather than with
// printf: GTK sets the C locale from the environment, and a German locale
// would print "0,500", which CSS rejects.
std::string cssColor(const Rgba& c);

// The whole stylesheet: layout constants plus every configurable color.
std::string stylesheet(const Settings& s);

// The editor's selection, which GTK draws for the buffer's own selection
// from the stylesheet and Carets.cpp draws for the other carets' (item 1.9).
Rgba editorSelection(const Settings& s);

// The shortcut hints' panel (Ctrl+Shift+H): its background and its keys'
// blue, the same on every theme, as the Mac's.
constexpr uint32_t kHintsBackground = 0x252526;
constexpr uint32_t kHintsKeyColor = 0x4EA1F7;

// The colours of the modifier words in the hints' keys (Ctrl, Alt and
// Super, Commands::keyLabel's Linux spelling), each the same everywhere in
// the panel. They are the theme's syntax colours, taken in this order:
// keyword, string, preprocessor (VS Code's control keywords), function,
// type, number, escape, tag, attribute. Each is lightened toward white
// until it reads at 4.5:1 or better on the panel's background (a light
// theme's colours are made for a light page, and the panel stays dark),
// and taken only when it stands apart from the keys' blue and from the
// ones taken before it (`distinct`). Dark Modern's string, preprocessor
// and function colours fill in when a theme has too few. They follow a
// theme change as the panel is laid out again.
struct HintsModifierColors {
    Rgba ctrl, alt, super;
};
HintsModifierColors hintsModifierColors(const Settings& s);
// WCAG 2's contrast ratio of two opaque colours, from 1 to 21.
double contrast(const Rgba& a, const Rgba& b);
// Whether two colours read as different ones: hues 30 degrees apart or
// more when both have colour in them, else far apart by the "redmean"
// distance (100 or more of about 765).
bool distinct(const Rgba& a, const Rgba& b);

}  // namespace theme
