# linux/ — the GTK4 port of MiniCode

This directory holds the Linux port of MiniCode, built on GTK4. It is separate
from the macOS app under `src/`, which it does not touch. The portable C++ core
(`../src/SyntaxHighlighter.{h,cpp}` and `../src/MarkdownParser.{h,cpp}`) is
shared verbatim and compiled straight from `../src`; it is never copied here, so
there is one source of truth for highlighting and Markdown parsing.

For install, build, and run instructions, and an honest account of what is and
is not verified, see `../BUILD-LINUX.md`.

## Files

- `meson.build`, `meson_options.txt` — primary build (Meson). Auto-detects VTE
  and WebKit and builds the panels only when present.
- `Makefile` — plain pkg-config fallback that builds at least the core viewer.
- `src/main.cpp` — the GtkApplication: window, menu, accelerators, the
  sidebar/editor split, status bar, and the collapsible panels.
- `src/FileTree.{h,cpp}` — the sidebar file browser (GtkDirectoryList +
  GtkTreeListModel + GtkListView), lazy-loaded and live-refreshing via the
  GFileMonitor that GtkDirectoryList runs internally.
- `src/Editor.{h,cpp}` — the GtkTextView editor: file loading, syntax
  highlighting via GtkTextTags, and the Markdown preview toggle.
- `src/Markdown.{h,cpp}` — renders the MarkdownParser run list into a styled
  GtkTextBuffer.
- `src/Terminal.{h,cpp}` — VTE terminal panel, behind `MINICODE_ENABLE_TERMINAL`.
- `src/Browser.{h,cpp}` — WebKitGTK browser panel, behind
  `MINICODE_ENABLE_BROWSER`.
- `src/Palette.h` — the dark color palette, mirrored from the macOS build.

## Feature flags

The always-built target is the file tree, the editor with syntax highlighting,
and the Markdown preview. The terminal (VTE) and browser (WebKitGTK) panels are
optional: Meson enables each one when its library is found, and the code for a
missing panel is compiled out entirely.
