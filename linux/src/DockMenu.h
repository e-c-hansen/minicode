// DockMenu.h — the dock icon's right click menu: Open Folder, New Window and
// the folders opened lately, as desktop actions. Ubuntu Dock and GNOME's
// dash show a desktop file's [Desktop Action] groups in an icon's menu,
// running or not (the dock's own live "quicklist" over D-Bus needs the app
// running), so minicode writes its own copy of its desktop file with the
// actions in $XDG_DATA_HOME/applications/org.minicode.Editor.desktop. That
// file shadows the installed one under the same id, so a pinned icon stays
// pinned. GNOME watches the folder; no update-desktop-database is needed
// (it only indexes MimeType, which the copy repeats unchanged).
//
// The copy is ours only while it carries kMarker. A file there without it is
// the user's own override and is never written or removed. With no installed
// desktop file in $XDG_DATA_DIRS (a run from a checkout) nothing is written.
// The setting window.dock-menu = false removes the copy.
#pragma once

#include <string>
#include <vector>

namespace DockMenu {

constexpr const char* kMarker = "X-Minicode-Generated=true";
constexpr size_t kMaxRecent = 10;

// Pure. A Desktop Entry Exec argument for `arg`: quoted, with ", `, $ and a backslash
// escaped for the quoting rule, % doubled, then each \ doubled again for
// the file's own string escapes (the spec's rules, in that order).
std::string execArg(const std::string& arg);
// Pure. The generated file's text from the installed one's, for the first
// kMaxRecent of `folders` (a path with a control character is left out,
// since a line cannot hold it). Empty when the installed text has no
// [Desktop Entry] Exec to take the binary from.
std::string render(const std::string& installed, const std::vector<std::string>& folders,
                   const std::string& home);

// The installed org.minicode.Editor.desktop in $XDG_DATA_DIRS/applications
// (never $XDG_DATA_HOME's), or "".
std::string installedFile();
// $XDG_DATA_HOME (else ~/.local/share)/applications/org.minicode.Editor.desktop.
std::string generatedFile();

// Brings the generated file up to date with RecentList::folders() when
// `enabled`, else removes it. Writes only when the content changes. Reads
// the environment each call.
void update(bool enabled);

}  // namespace DockMenu
