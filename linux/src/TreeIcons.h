// TreeIcons.h — the file tree's icon for a file, from the system icon theme
// (the GTK port's half of the core's FileIcon::Type, FileDetails.h).
//
// Each type has a list of icon names, best first: the freedesktop mimetype
// names (text-x-python, application-json, text-x-makefile) that Yaru,
// Papirus and other full themes draw, then broader ones (text-x-script),
// then text-x-generic, which every theme has. The first the display's theme
// has (gtk_icon_theme_has_icon, its parents and hicolor included) is the
// row's: GTK draws its missing-image picture for a name the theme lacks,
// and Adwaita, GTK's default, lacks most of the languages. Answers are kept
// per theme and asked again when the theme changes (a user switching icon
// themes, or the checks switching to Yaru). No pictures of mutible's own:
// the names are the theme's.
#pragma once

#include <gtk/gtk.h>

#include <string>
#include <vector>

#include "FileDetails.h"

namespace TreeIcons {

// The names tried for `t`, best first; the last is always text-x-generic.
const std::vector<const char*>& candidates(FileIcon::Type t);
// The first of them `theme` has (text-x-generic-symbolic when it has none).
const char* resolve(GtkIconTheme* theme, FileIcon::Type t);
// The icon for the file at `path` as `widget`'s display's theme draws it;
// `executable`: a file with no extension the listing found executable.
const char* nameFor(GtkWidget* widget, const std::string& path, bool executable);
// What a row with no type of its own shows: the Text type's icon.
const char* fallback(GtkIconTheme* theme);

}  // namespace TreeIcons
