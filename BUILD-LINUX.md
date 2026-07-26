# Building MiniCode on Linux (GTK4)

MiniCode began as a native macOS editor. This is a port of the same editor to
Ubuntu Linux using GTK4. It reuses the exact same portable C++ core (the syntax
highlighter and the Markdown parser) that the macOS build uses, so highlighting
and Markdown rendering behave identically. Only the GUI layer is rewritten, from
AppKit to GTK4.

Everything for the Linux port lives under `linux/`. The macOS build is
untouched.

## What is verified, and what is not

The port now builds clean and runs. It was brought up on Ubuntu 26.04 with GTK
4.22.4 and VTE 0.84.0, compiling without a single warning at `warning_level=2`.

Verified by running it:

- The portable C++ core (`src/SyntaxHighlighter.{h,cpp}` and
  `src/MarkdownParser.{h,cpp}`) compiles and passes all 38 checks under `g++`.
  This code is shared verbatim with the macOS build.
- The port's own pure-C++ piece, the byte-to-character offset conversion in
  `linux/src/Utf8Offsets.h`, passes 47 checks (`cd linux && make test`).
- The window, file tree, editor, syntax highlighting, Markdown preview, the
  binary-file guard and the VTE terminal panel were all confirmed on screen.
  Highlighting was checked against source containing accented Latin, CJK and
  4-byte emoji, and the colors land on the correct spans.
- All 11 window actions are registered with the intended accelerators, and the
  sidebar, dotfile, terminal and preview toggles were confirmed to change the
  state they claim to.

Not verified:

- **The browser panel has never been compiled.** `libwebkitgtk-6.0-dev` was not
  installed on the machine used to bring this up, so `webkitgtk-6.0` was absent
  and the panel compiled out. `linux/src/Browser.cpp` is still a first draft.
  Its five WebKit entry points do exist in the installed `libwebkitgtk-6.0.so.4`,
  so it is likely close, but expect to fix something.
- Everything driven by real keyboard and mouse input. Actions were activated
  programmatically, which proves the wiring but not the key handling.
- Saving, creating files and folders, and the Open Folder dialog.

### Notes on the things that were most at risk

The four concerns below were the ones flagged before the port had ever been
compiled. All four turned out fine, and the details are recorded here so nobody
re-investigates them.

1. **Byte vs. character offsets.** This was the real one, though not in the way
   expected. `SyntaxHighlighter` returns byte offsets into UTF-8 text and
   `GtkTextBuffer` iterators are indexed by character. The original conversion
   used `g_utf8_pointer_to_offset`, which counts from the start of the string on
   every call, making a full re-lex O(n^2) and stalling the editor on large
   files. It is now a single forward-only pass (`Utf8OffsetCursor`), which is
   valid because the lexer is one left-to-right scan and its tokens are
   therefore strictly ascending. That invariant is asserted by the tests, so it
   cannot silently rot.

2. **The file tree model API.** `"standard::file"` is correct: GTK really does
   set that attribute on every `GFileInfo` a `GtkDirectoryList` produces. Lazy
   expansion through `create_child` works, and the filter and factory callback
   signatures are unchanged.

3. **VTE.** `vte_terminal_spawn_async` works as written. The panel spawns
   `$SHELL` in the opened folder.

4. **GtkTextTag setup and CSS.** The tag properties and the
   `.minicode-editor text { ... }` node selector are all correct; the editor and
   Markdown preview render with the intended VS Code dark palette.

## Install the dependencies

On Ubuntu 24.04 and 26.04:

    sudo apt install build-essential meson libgtk-4-dev \
        libvte-2.91-gtk4-dev libwebkitgtk-6.0-dev pkg-config

Check afterwards that all three actually landed, since apt will happily install
the runtime library while leaving the `-dev` package out:

    pkg-config --modversion gtk4 vte-2.91-gtk4 webkitgtk-6.0

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
    meson test -C build
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
    make test                 # pure-C++ tests; needs no GTK and no display

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
