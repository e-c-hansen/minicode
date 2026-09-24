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
- `src/main.cpp` — the GtkApplication: its windows (each an `App`), the
  menu, the accelerators and which of them the terminal gets, the
  sidebar/editor split, status bar, and the collapsible panels.
- `src/FileTree.{h,cpp}` — the sidebar file browser (a list per folder +
  GtkTreeListModel + GtkListView), lazy-loaded and live-refreshing from a
  GFileMonitor per folder (GtkDirectoryList's own monitoring reports nothing
  on GTK 4.22; see `HANDOFF.md`, "Traps").
- `src/Editor.{h,cpp}` — the GtkTextView editor: file loading, syntax
  highlighting via GtkTextTags, the Markdown preview toggle, reloading when
  the file changes on disk, and the live color picker for the settings file.
- `src/ScrollSettle.h` — ends a scroll animation before the editor is hidden,
  working around a GTK 4.22 fault that logged two criticals.
- `src/MediaView.{h,cpp}` — images and PDFs in the editor's slot: a
  `GtkPicture` for images, a `PdfView` for PDFs, and a `GFileMonitor` that
  reloads them when the file changes on disk.
- `src/PdfView.{h,cpp}` — every page of a PDF in a scrolling column, drawn
  lazily with poppler-glib, behind `MINICODE_ENABLE_PDF`. The LaTeX preview
  shows its pages with it too.
- `src/Latex.{h,cpp}` — the LaTeX preview in the editor's slot: tectonic
  (found, or downloaded on request), the pages, the log when it does not
  typeset, the double-click edit popover and Export PDF (Ctrl+Shift+S).
  Behind `MINICODE_ENABLE_PDF`.
- `src/PageWords.{h,cpp}` — pure C++: the word under a point on a page, from
  its text and one box per character, and the whole click-to-source path
  over `../src/LatexDoc` and `../src/SyncTex`. Tested in `tests/run_tests.cpp`.
- `src/LatexClick.{h,cpp}` — the poppler and GIO half of that path: a page's
  text and boxes, and the gzipped SyncTeX file.
- `src/Markdown.{h,cpp}` — renders the MarkdownParser run list into a styled
  GtkTextBuffer.
- `src/Terminal.{h,cpp}` — VTE terminal panel, behind `MINICODE_ENABLE_TERMINAL`,
  with Ctrl+click on file references and URLs found by `../src/TermLinks`.
- `src/TermLinkPath.{h,cpp}` — pure C++: resolves a file reference from the
  terminal against the shell's folder and the project folder. Tested in
  `tests/run_tests.cpp`.
- `src/Browser.{h,cpp}` — WebKitGTK browser panel, behind
  `MINICODE_ENABLE_BROWSER`.
- `src/Search.{h,cpp}` — Find in Folder (Ctrl+Shift+F): a window with a
  search entry and a list of matches. The search itself is
  `../src/FolderSearch.{h,cpp}`, run on a GTask worker thread.
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
other binary file, and a `.tex` file opens as plain source.
