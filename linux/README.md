# linux/ — the GTK4 port of MiniCode

This directory holds the Linux port of MiniCode, built on GTK4. It is separate
from the macOS app under `src/`, which it does not touch. The portable C++ core
(`../src/SyntaxHighlighter.{h,cpp}` and `../src/MarkdownParser.{h,cpp}`) is
shared verbatim and compiled straight from `../src`; it is never copied here, so
there is one source of truth for highlighting and Markdown parsing.

For install, build, and run instructions, and an honest account of what is and
is not verified, see `../BUILD-LINUX.md`. For what the port still lacks
compared with the macOS app, and the order to do it in, see `HANDOFF.md`.

## Files

- `meson.build`, `meson_options.txt` — primary build (Meson). Auto-detects VTE,
  WebKit and poppler-glib and builds the panels only when present.
- `Makefile` — plain pkg-config fallback that builds at least the core viewer.
- `src/main.cpp` — the GtkApplication: window, menu, accelerators, the
  sidebar/editor split, status bar, and the collapsible panels.
- `src/FileTree.{h,cpp}` — the sidebar file browser (GtkDirectoryList +
  GtkTreeListModel + GtkListView), lazy-loaded and live-refreshing via the
  GFileMonitor that GtkDirectoryList runs internally.
- `src/Editor.{h,cpp}` — the GtkTextView editor: file loading, syntax
  highlighting via GtkTextTags, and the Markdown preview toggle.
- `src/MediaView.{h,cpp}` — images and PDFs in the editor's slot: a
  `GtkPicture` for images, a `PdfView` for PDFs, and a `GFileMonitor` that
  reloads them when the file changes on disk.
- `src/PdfView.{h,cpp}` — every page of a PDF in a scrolling column, drawn
  lazily with poppler-glib, behind `MINICODE_ENABLE_PDF`. Written to be reused
  by the LaTeX preview.
- `src/Markdown.{h,cpp}` — renders the MarkdownParser run list into a styled
  GtkTextBuffer.
- `src/Terminal.{h,cpp}` — VTE terminal panel, behind `MINICODE_ENABLE_TERMINAL`.
- `src/Browser.{h,cpp}` — WebKitGTK browser panel, behind
  `MINICODE_ENABLE_BROWSER`.
- `src/Palette.h` — the dark color palette, mirrored from the macOS build.
- `resources/org.minicode.Editor.desktop.in` — the desktop launcher, with the
  binary path filled in at configure time from the install prefix.
- `resources/icons/` — PNG icons at five sizes, extracted from the macOS
  `../resources/AppIcon.icns`, which stays the single source of truth for the
  artwork.

## Feature flags

The always-built target is the file tree, the editor with syntax highlighting,
the Markdown preview and the image viewer. The terminal (VTE), the browser
(WebKitGTK) and the PDF viewer (poppler-glib) are optional: Meson enables each
one when its library is found, and the code for a missing one is compiled out
entirely. Without poppler a PDF gets the same "Cannot display" message as any
other binary file.
