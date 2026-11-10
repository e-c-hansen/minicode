// GitTrash.h — the Trash for the Source Control panel's Discard (PARITY.md
// 2.9): the file as it is goes to the Trash before git puts it back, and
// Undo Discard moves it back from there, as the Mac's NSFileManager
// trashItemAtURL:resultingItemURL: and moveItemAtURL: do.
//
// GIO's g_file_trash says nothing of where it put the file, so `trash` looks
// for it afterwards in the Trash that took it (the freedesktop.org Trash
// specification, which GIO follows): the home Trash, $XDG_DATA_HOME/Trash,
// for a file on the home folder's filesystem, else the .Trash/<uid> or
// .Trash-<uid> folder at the top of the file's own filesystem; the newest
// .trashinfo there whose Path names the file. A volume with no Trash (a
// tmpfs, as /tmp) refuses, and then nothing is discarded.
//
// Called on the panel's git thread; nothing here touches GTK.
#pragma once

#include <string>

namespace GitTrash {

// Moves `path` to the Trash. `where` gets the trashed copy's path.
bool trash(const std::string& path, std::string* where, std::string* error);
// Puts a copy of the file at `path` in the Trash under the file's own name
// (Discard Hunk keeps the file as it was, which stays where it is): copied
// into a folder of our own beside it, which is then trashed through.
bool trashCopy(const std::string& path, std::string* where, std::string* error);
// Moves the trashed copy at `where` back to `path`, making its folder, and
// drops what the Trash knew of it. Refuses to replace a file at `path`.
bool putBack(const std::string& where, const std::string& path, std::string* error);
// Removes a trashed copy (a Discard Hunk that git then refused).
void remove(const std::string& where);
// Whether the trashed copy is still there (the Trash may have been emptied).
bool stillThere(const std::string& where);

// For the GUI checks only: a folder of the scratch world stands in for the
// Trash, files moving into it as "<n>-<name>", as the Mac's suites swap
// NSFileManager's trashItemAtURL for one; "" puts the real Trash back. The
// checks never reach the owner's real Trash this way, whatever HOME says.
void useFolderForChecks(const std::string& dir);

}  // namespace GitTrash
