// FileTypeIcons.h — Objective-C++. The Mac's pictures for the core's
// FileIcon::Type (FileDetails.h), the way linux/src/TreeIcons.cpp gives the
// GTK port the icon theme's: a file's icon in the tree and in Find in
// Folder's results says what it is (Python, JSON, a Dockerfile, a picture).
//
// Each type is an SF Symbol (a list tried in order, for symbols newer than
// macOS 11, the app's minimum), or, for a language SF Symbols has no
// picture of, its short name drawn in a bold rounded face ("py", "JS",
// "C++", "M↓"), as Seti does in VS Code. Every image is a template, so the
// view's contentTintColor colors it, with one restrained color per family
// (blue for Python, C and TypeScript, yellow for JavaScript and JSON,
// green for shells and tables, and so on), a lighter shade on a dark tree
// and a deeper one on a light tree. The images and colors are made once
// per type and kept: no row makes one. Each image's accessibilityDescription
// is its type's name (FileIcon::typeName: "python", "json"), which the
// checks compare.
#import <Cocoa/Cocoa.h>
#include "FileDetails.h"

// The type of the file at `path` (its name alone; `executable`: a regular
// file the listing found executable, which matters for one with no
// extension).
FileIcon::Type MCFileTypeOf(NSString *path, BOOL executable);
// The type's picture, made once.
NSImage *MCFileTypeImage(FileIcon::Type type);
// The type's color on a dark background (`dark`) or a light one.
NSColor *MCFileTypeTint(FileIcon::Type type, BOOL dark);
// Whether a view whose text is `text` is dark: its text light.
BOOL MCFileTypeTextIsLight(NSColor *text);
