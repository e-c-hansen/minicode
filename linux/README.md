# linux/ — the GTK4 port of minicode

This directory holds the Linux port of minicode, built on GTK4. It is separate
from the macOS app under `src/`, which it does not touch. The portable C++ core
in `../src` (the highlighter, the Markdown parser, the command registry, the
rules behind tabs, find, several cursors, and folding, and the rest that
`meson.build`'s `core_sources` names) is shared verbatim and compiled
straight from there; it is never copied here, so there is one source of
truth for each.

For install, build, and run instructions, an honest account of what is and
is not verified, and the list of what still needs a person at the desktop,
see `../BUILD-LINUX.md`. For what the port still lacks compared with the
macOS app, and the order to do it in, see `PARITY.md`; `HANDOFF.md` says
how each finished item was built and the traps it paid for.

## Files

- `meson.build`, `meson_options.txt` — primary build (Meson). Auto-detects VTE,
  WebKit and poppler-glib and builds the panels only when present.
- `Makefile` — plain pkg-config fallback that builds at least the core viewer.
- `src/main.cpp` — the GtkApplication: its windows (each an `App`), the
  actions the menu bar and the keys name, the status bar, and the shortcut
  hints.
- `src/WindowLayout.{h,cpp}` — a window's panes, as the Mac lays them out:
  the sidebar beside the right side, and there the editor with the browser
  to its right and the terminal below both, each hidden or shown by its own
  toggle, with bars to drag. The rules are `../src/RightAreaLayout`, tested
  in the core's `rightarea:*` groups.
- `src/FileTree.{h,cpp}` — the sidebar file browser (a list per folder +
  GtkTreeListModel + GtkListView), lazy-loaded and live-refreshing from a
  GFileMonitor per folder (GtkDirectoryList's own monitoring reports nothing
  on GTK 4.22; see `HANDOFF.md`, "Traps").
- `src/Editor.{h,cpp}` — the GtkTextView editor: file loading, syntax
  highlighting via GtkTextTags, the Markdown preview toggle, reloading when
  the file changes on disk, and the live color picker for the settings file.
- `src/ScrollSettle.h` — ends a scroll animation before the editor is hidden,
  working around a GTK 4.22 fault that logged two criticals.
- `src/Document.{h,cpp}`, `src/Tabs.cpp`, `src/TabStrip.{h,cpp}` — one open
  file each (its buffer, undo, highlighter, and place), the editor's tabs by
  the core's `EditorTabs` rules, and the strip above the editor.
- `src/Persistence.{h,cpp}`, `src/BackupStore.{h,cpp}` — a folder's tabs
  coming back, backups of unsaved tabs, and auto-save.
- `src/CodeView.{h,cpp}`, `src/Gutter.{h,cpp}`, `src/BufferLines.{h,cpp}` —
  the editor's GtkTextView subclass and its drawing passes, the line numbers
  and their lanes, and each buffer's line index.
- `src/FindBar.{h,cpp}`, `src/MultiEdit.{h,cpp}` — the find and replace bar,
  and edits with several ranges as one undo step.
- `src/BracketView.{h,cpp}`, `src/BracketColors.{h,cpp}`,
  `src/AutoCloseInput.{h,cpp}`, `src/BufferText.{h,cpp}` — bracket boxes and
  Go to Bracket, bracket-pair colors, auto-closing pairs, and the buffer as
  the core's editing rules read it.
- `src/Carets.{h,cpp}`, `src/CaretEdits.cpp`, `src/CaretText.h` — several
  carets and the line commands.
- `src/Folds.{h,cpp}` — folding, under an invisible tag.
- `src/PalettePanel.{h,cpp}` — the command palette, Quick Open, Go to Line,
  and the open file's symbols.
- `src/Zoom.{h,cpp}`, `src/NavHistoryUi.{h,cpp}` — the editor's and the
  terminals' zoom and the word wrap toggle, and Go Back and Forward.
- `src/LargeFileView.{h,cpp}` — large-file mode, big files read in the
  background, and long lines shown a part at a time.
- `src/Detach.{h,cpp}`, `src/LaunchOptions.{h,cpp}` — `minicode` giving its
  terminal back, and the command line's options.
- `src/StartupLog.{h,cpp}`, `src/WhenDrawn.{h,cpp}` — the startup log
  (`MINICODE_STARTUP_LOG=1`), and work put off until the first frame.
- `src/MediaView.{h,cpp}` — images, PDFs, video and audio in the editor's
  slot: a `GtkPicture` for images, a `PdfView` for PDFs, a `GtkVideo` or
  `GtkMediaControls` over GTK's GStreamer backend for video and audio (or a
  message naming what to install when it cannot play), and a `GFileMonitor`
  that reloads them when the file changes on disk.
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
  GtkTextBuffer, with tables (a grid of labels) and pictures (GIFs play) as
  embedded widgets, and a map from the rendered text back to source lines
  that the editor uses for links, the preview toggle keeping its place, and
  editing a block from the preview (`../src/MarkdownEdit`).
- `src/Terminal.{h,cpp}` — VTE terminal panel, behind `MINICODE_ENABLE_TERMINAL`,
  with Ctrl+click on file references and URLs found by `../src/TermLinks`.
- `src/TermLinkPath.{h,cpp}` — pure C++: resolves a file reference from the
  terminal against the shell's folder and the project folder. Tested in
  `tests/run_tests.cpp`.
- `src/Browser.{h,cpp}` — WebKitGTK browser panel, behind
  `MINICODE_ENABLE_BROWSER`.
- `src/Search.{h,cpp}` — Find and Replace in Folder (Ctrl+Shift+F and
  Ctrl+Alt+Shift+F): a window with a search entry and a list of matches.
  The search itself is `../src/FolderSearch.{h,cpp}`, run on a GTask worker
  thread. The same
  window in a TODOs mode (Ctrl+Shift+D) lists `FolderSearch::findTodos`.
- `src/GitPanel.{h,cpp}` — the Source Control panel (Ctrl+Shift+G), which
  takes the file tree's place in the sidebar: the branch, staged and
  unstaged changes, the commit message, and the commit graph drawn with
  cairo, with git run off the main thread one command at a time. Also
  `GitDiffView`, the colored diff or commit that the editor shows in its
  slot.
- `src/GitModel.{h,cpp}` — pure C++: what the panel shows and every git
  command it runs, worked out from `../src/GitStatus` and `../src/GitGraph`.
  Tested in `tests/run_tests.cpp`.
- `src/Keys.{h,cpp}` — the command registry in the port: the table of
  GActions by command id, and from it and the Linux keys
  (`../src/CommandsLinux.cpp`, with `keys.conf` applied) the menu bar, the
  accelerators, the file tree's own keys and the hints' key text. Which
  keys are the shell's while the terminal has the keyboard is decided here.
- `src/Palette.h` — the dark color palette, mirrored from the macOS build.
- `tests/gui/` — the GUI checks (`meson test -C build --suite gui`): the
  whole app on a private Xvfb, driven by real key presses and clicks, a
  suite a file, listed in `suites.txt`.
- `resources/org.minicode.Editor.desktop.in` — the desktop launcher, with the
  binary path filled in at configure time from the install prefix.
- `resources/icons/` — PNG icons at five sizes, extracted from the macOS
  `../resources/AppIcon.icns`, which stays the single source of truth for the
  artwork.

## Feature flags

The always-built target is the file tree, the editor with syntax highlighting,
the Markdown preview, the image viewer, and video and audio. Video and audio
use GTK's own GStreamer backend, so they need no build option; what plays
depends on the GStreamer plugins installed at run time (plugins-good for
WebM, Ogg, MP3 and FLAC, `gstreamer1.0-libav` for H.264 and AAC; see
`../BUILD-LINUX.md`). The terminal (VTE), the browser
(WebKitGTK) and the PDF viewer (poppler-glib) are optional: Meson enables each
one when its library is found, and the code for a missing one is compiled out
entirely. Without poppler a PDF gets the same "Cannot display" message as any
other binary file, and a `.tex` file opens as highlighted source.
