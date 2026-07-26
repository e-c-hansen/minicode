# Building MiniCode on Linux (GTK4)

MiniCode began as a native macOS editor. This is a port of the same editor to
Ubuntu Linux using GTK4. It reuses the exact same portable C++ core (the syntax
highlighter and the Markdown parser) that the macOS build uses, so highlighting
and Markdown rendering behave identically. Only the GUI layer is rewritten, from
AppKit to GTK4.

Everything for the Linux port lives under `linux/`. The macOS build is
untouched.

## What is verified, and what is not

Be clear-eyed about this before you start.

- The portable C++ core (`src/SyntaxHighlighter.{h,cpp}` and
  `src/MarkdownParser.{h,cpp}`) was compiled and unit-tested with `g++` on the
  development machine. All 38 checks pass. This code is shared verbatim with the
  macOS build and is solid.
- The GTK4 GUI (everything under `linux/src/`) was written **without being
  compiled or run**. The development machine is a Mac with no GTK headers and no
  display, so none of the GTK code could be exercised there. Treat it as a
  careful first draft, not working software. It will almost certainly need fixes
  before it builds and runs on real hardware.

The intended workflow is: build it on your Ubuntu laptop, paste the compiler
errors back, and iterate. The structure and the wiring are all in place; what
remains is shaking out GTK API details that can only be checked against real
headers.

### The most likely things to fix first

1. **Byte vs. character offsets in the editor highlighter.**
   `SyntaxHighlighter` returns byte offsets into the UTF-8 text, but
   `GtkTextBuffer` iterators are indexed by character. `linux/src/Editor.cpp`
   converts byte offsets to character offsets with `g_utf8_pointer_to_offset`.
   The logic is marked with a comment. Test it with a file that contains
   multi-byte UTF-8 (accented letters, emoji, CJK) and confirm colors land on
   the right spans. This is the single most likely bug.

2. **The file tree model API.** `linux/src/FileTree.cpp` uses
   `GtkDirectoryList` + `GtkTreeListModel` + `GtkListView`. Getting the `GFile`
   back off a `GFileInfo` relies on the `"standard::file"` attribute, which is
   the documented idiom but is worth verifying against your installed GTK. The
   `create_child` / filter / factory callback signatures are also a place where
   GTK 4.x point releases have shifted slightly.

3. **VTE and WebKit API drift.** `vte_terminal_spawn_async` and the
   `webkitgtk-6.0` entry points change across releases. These panels are behind
   feature flags, so the core viewer builds without them; enable them once the
   core is up.

4. **GtkTextTag setup and CSS.** The tag properties (`size-points`,
   `pixels-above-lines`, and so on) and the CSS node selectors for theming the
   `GtkTextView` (`.minicode-editor text { ... }`) are the kind of thing that is
   easy to get subtly wrong. If highlighting or the dark theme looks off, start
   here.

5. **Menu, actions, accelerators.** Action names are `win.*`; if a shortcut or
   menu item does nothing, check that the action is registered on the window and
   that the accel string matches.

## Install the dependencies

On Ubuntu 24.04:

    sudo apt install build-essential meson libgtk-4-dev \
        libvte-2.91-gtk4-dev libwebkitgtk-6.0-dev pkg-config

Notes on package names, which drift between Ubuntu versions:

- `libgtk-4-dev` is required. Everything else is optional.
- `libvte-2.91-gtk4-dev` provides the GTK4 build of VTE. On some releases the
  package or its pkg-config module may be named slightly differently (the
  pkg-config module this build looks for is `vte-2.91-gtk4`). If the terminal
  panel is silently skipped, check `pkg-config --exists vte-2.91-gtk4`.
- `libwebkitgtk-6.0-dev` provides the GTK4 WebKit (pkg-config module
  `webkitgtk-6.0`). Older Ubuntu used `libwebkit2gtk-4.1-dev` with a different
  API; that will not work with this code as written.

If you only install `libgtk-4-dev`, the core viewer still builds. The terminal
and browser panels are compiled out automatically when their libraries are
absent.

## Build with Meson (recommended)

    cd linux
    meson setup build
    meson compile -C build
    ./build/minicode ~/some/project

Meson auto-detects VTE and WebKit. Watch the configure output: it prints whether
each panel is ENABLED or disabled. To force a panel on (and make configuration
fail loudly if the library is missing):

    meson setup build -Dterminal=enabled -Dbrowser=enabled

To force one off:

    meson setup build -Dterminal=disabled

## Build with the Makefile (fallback)

If you would rather not use Meson:

    cd linux
    make                      # core viewer only (needs just libgtk-4-dev)
    make TERMINAL=1           # add the terminal panel
    make BROWSER=1            # add the browser panel
    make TERMINAL=1 BROWSER=1
    make run DIR=~/some/project

Meson is the better-tested path; the Makefile exists so a minimal install can
get the core viewer up quickly.

## Running

    ./build/minicode [directory]

With no argument it opens the current working directory. The window shows the
file tree on the left and the editor on the right, with a status bar along the
bottom.

## Keyboard shortcuts

These mirror the macOS set, with Ctrl standing in for Command.

| Shortcut          | Action                     |
| ----------------- | -------------------------- |
| Ctrl O            | Open folder                |
| Ctrl S            | Save                       |
| Ctrl Alt N        | New file                   |
| Ctrl Shift N      | New folder                 |
| Ctrl F            | Find in the current file   |
| Ctrl Shift P      | Toggle Markdown preview    |
| Ctrl B            | Toggle the sidebar         |
| Ctrl T            | Toggle the terminal panel  |
| Ctrl Shift B      | Toggle the browser panel   |
| Ctrl H            | Show or hide dotfiles      |
| Ctrl 0            | Focus the file tree        |
