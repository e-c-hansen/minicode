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

}  // namespace theme
