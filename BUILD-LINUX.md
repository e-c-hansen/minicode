# Building MiniCode on Linux (GTK4)

MiniCode began as a native macOS editor. This is a port of the same editor to
Ubuntu Linux using GTK4. It reuses the exact same portable C++ core (the syntax
highlighter, the Markdown parser, the settings file parser and the comment
toggle) that the macOS build uses, so highlighting, Markdown rendering,
settings and Ctrl+/ behave identically. Only the GUI layer is rewritten, from
AppKit to GTK4.

Everything for the Linux port lives under `linux/`. The macOS build is
untouched.

## What is verified, and what is not

The port now builds clean and runs, with every panel enabled. It was brought up
on Ubuntu 26.04 with GTK 4.22.4, VTE 0.84.0 and WebKitGTK 2.52.3, compiling
without a single warning at `warning_level=2`.

Verified by running it:

- The portable C++ core in `src/` compiles cleanly under `g++` 15 and passes
  all 1,218 checks of the top-level `make test`, the same suite the macOS
  build runs. This code is shared verbatim with the macOS build. CI now runs
  it in the Linux job too.
- The port's own pure-C++ piece, the byte-to-character offset conversion in
  `linux/src/Utf8Offsets.h`, passes 47 checks (`cd linux && make test`).
- The window, file tree, editor, syntax highlighting, Markdown preview, the
  binary-file guard, the VTE terminal and the WebKit browser panel were all
  confirmed on screen. Highlighting was checked against source containing
  accented Latin, CJK and 4-byte emoji, and the colors land on the correct
  spans.
- The browser panel loads and renders real pages over the network, its URL
  entry navigates, plain words are sent to a web search rather than mangled
  into a URL, the URL bar follows redirects and back/forward navigation, and
  the back and forward buttons grey out correctly. Showing or hiding the panel
  leaves the editor filling the right area with the terminal still docked
  underneath.
- The terminal panel's top edge is a real `GtkPaned` divider. Moving it resizes
  the panel, the size survives closing and reopening it, and the clamps hold at
  both ends: the terminal will not go below 80px and the editor above it will
  not go below 120px, the same limits the macOS drag handler enforces.
- Collapsing the editor (Ctrl Shift E) gives the terminal the entire window, and
  the split comes back exactly where it was. The window can never be left empty:
  collapsing the editor opens the terminal if it was closed, and closing the
  terminal while the editor is collapsed brings the editor back. Opening a file
  or the browser while collapsed restores the editor too.
- The shortcut hints panel (Ctrl Shift H) opens and closes, reports which panes
  are currently up, and refreshes when they change. The status bar advertises it.
- The settings file, Ctrl+/, and the divider drags (September 2026). These were
  checked in an Ubuntu 26.04 Docker container on the Mac, on a virtual X display
  with a compositor, driven with real key presses and mouse drags from
  `xdotool` and checked from screenshots. `linux/dev/` has that setup
  (`linux/dev/gtk-dev.sh`, see its README):
  - Ctrl+, creates `~/.config/minicode/settings.conf` with every setting
    commented out and opens it, with each color shown as a swatch. Clicking a
    swatch opens the GTK color dialog, and the choice is written into the line,
    uncommented, and saved.
  - Edits to the file apply while the app runs. Per-panel opacity was measured
    in screenshot pixels against a known backdrop and matches the setting for
    the editor, file tree, terminal, status bar and menu bar. Text colors, the
    title bar (with client-side decorations), the browser toolbar and Markdown
    heading colors change live. A bad line shows in the status bar.
  - Ctrl+/ comments and uncomments the caret's line and a selected block, one
    Ctrl+Z undoes it, and it does nothing to the file while the terminal has
    focus. Ctrl+Shift+T toggles the terminal from any focus.
  - Dragging the terminal divider to the top snaps the editor shut and dragging
    it down brings it back; dragging the sidebar divider to the right edge
    leaves only the file tree, and opening a file restores the right side at
    its earlier width.
- The shell restarts in place when it exits. Confirmed by sending `exit` to the
  child and reading the terminal buffer back: the notice line, a fresh prompt,
  and a command run successfully in the new shell.
- Find in Folder (Ctrl+Shift+F, September 2026). Checked on Ubuntu 26.04
  under GNOME on Wayland with a temporary test hook that drove the real code
  paths inside the running app, then was removed: the action opens the window
  scoped to the open folder; one character is not searched; a search started
  and replaced at once has its results dropped, so only the newer query's
  matches appear; folders the Mac skips (`node_modules`, dotfolders) are left
  out; the status line counts matches and files; activating a match opens the
  file with the caret on the right line and the match selected, including
  after accented letters and an emoji, and a Markdown file switches from the
  preview to its source first; a folder selected in the tree becomes the
  scope and the query re-runs there; a path typed into the folder field that
  is not a folder is refused; closing the window hides it and keeps the
  results. The search itself is the shared `src/FolderSearch.cpp`, with its
  own tests in the core suite.
- All 13 window actions are registered with the intended accelerators, and the
  sidebar, dotfile, terminal, browser and preview toggles were confirmed to
  change the state they claim to.

Not verified:

- Everything driven by real keyboard and mouse input. Actions were activated
  programmatically, which proves the wiring but not the key handling. For Find
  in Folder that includes pressing Ctrl+Shift+F, typing into the field (the
  test set its text, which fires the same signal), double-clicking a row, the
  Down arrow and Escape, the Choose button's folder dialog, and whether GNOME
  raises the main window when a match is opened (the editor is made the
  window's focus widget, but activating a window is up to the compositor).
- How the Find in Folder window looks. Nothing in it was seen on screen.
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

3. **VTE and WebKit.** `vte_terminal_spawn_async` and the `webkitgtk-6.0` entry
   points all work as written; neither needed a change. The terminal spawns
   `$SHELL` in the opened folder. What did need fixing was the browser panel's
   layout rather than its API: the web view sets `vexpand`, which propagates up
   through its `GtkRevealer`, so the panel both squashed the editor when shown
   and held onto half the space when hidden. The editor is now hidden while the
   browser is up, and the revealer only expands while revealed.

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

## Install it as a real desktop app

Building leaves the binary in `linux/build`. To get `minicode` on your PATH and
a launcher in the app grid (so it can be pinned to the dock), install into your
home prefix:

    cd linux
    meson setup build --prefix=$HOME/.local   # or: meson configure build --prefix=...
    meson install -C build

That installs three things:

- `~/.local/bin/minicode` — the binary. Ubuntu already puts `~/.local/bin` on
  the PATH for login shells.
- `~/.local/share/applications/org.minicode.Editor.desktop` — the launcher.
- `~/.local/share/icons/hicolor/*/apps/org.minicode.Editor.png` — the icon, at
  five sizes, extracted from the macOS `resources/AppIcon.icns`.

Then refresh the caches, or log out and back in:

    update-desktop-database ~/.local/share/applications
    gtk4-update-icon-cache -f -t ~/.local/share/icons/hicolor

The launcher, the icons, and the GTK application id are all named
`org.minicode.Editor` on purpose. A Wayland compositor matches a window back to
its launcher by application id, so if those names drift apart the dock shows a
second, generic icon for the running window instead of lighting up the pinned
one.

The `Exec=` line uses the absolute path to the binary rather than bare
`minicode`. GNOME starts desktop entries from the systemd user session, whose
PATH does not reliably include `~/.local/bin`.

To pin it: launch it once, then right-click its dock icon and choose "Pin to
Dash". The equivalent from a shell is

    gsettings set org.gnome.shell favorite-apps \
        "$(gsettings get org.gnome.shell favorite-apps \
           | sed "s/]$/, 'org.minicode.Editor.desktop']/")"

Installing to `/usr/local` with `sudo meson install` works the same way and puts
the app in every user's menu.

### Sudden second copy of the app?

`minicode` is a single-instance GApplication, so launching it again while it is
running does not start a second process. It does still do what you asked:
the argument is forwarded to the running instance, which re-roots its sidebar,
opens the file if you named one, and raises its window. There is only ever one
window, so a second invocation reuses it rather than opening another.

## Running

    ./build/minicode [path]

The path can be a directory or a single file. A directory becomes the root of
the sidebar. A file is opened in the editor, with the sidebar rooted at the
directory containing it, so `minicode notes/todo.md` shows the file and its
neighbours without you having to name the folder separately. Markdown opens
rendered, the same as clicking it in the sidebar. With no argument at all it
opens the current working directory.

Relative paths are resolved against the shell you typed them in, including when
an instance is already running. A path that does not exist reports
`no such file or directory` on the terminal you ran the command from, and the
sidebar opens on the parent directory so a mistyped filename still lands in the
right place.

The window shows the file tree on the left and the editor on the right, with a
status bar along the bottom.

## Keyboard shortcuts

These mirror the macOS set, with Ctrl standing in for Command.

The settings file (Ctrl+,) is the same `~/.config/minicode/settings.conf` the
macOS build reads (it follows `$XDG_CONFIG_HOME`). Every panel's background,
opacity and text color work the same way, with two differences: on Linux the
title bar settings also color the menu bar, and `window.blur` is ignored,
because Linux desktops don't give apps a way to blur what is behind a window.
Transparency needs a compositor, which GNOME always has. GTK's color dialog
applies a picked color when you press Select, rather than live while you drag.

| Shortcut          | Action                     |
| ----------------- | -------------------------- |
| Ctrl O            | Open folder                |
| Ctrl S            | Save                       |
| Ctrl Alt N        | New file                   |
| Ctrl Shift N      | New folder                 |
| Ctrl F            | Find in the current file   |
| Ctrl Shift F      | Find in the folder         |
| Ctrl Shift P      | Toggle Markdown preview    |
| Ctrl Shift H      | Show or hide the shortcut hints |
| Ctrl B            | Toggle the sidebar         |
| Ctrl Shift E      | Collapse or restore the editor |
| Ctrl Shift T      | Toggle the terminal panel  |
| Ctrl /            | Comment or uncomment the selected lines |
| Ctrl ,            | Open the settings file     |
| Ctrl Shift B      | Toggle the browser panel   |
| Ctrl H            | Show or hide dotfiles      |
| Ctrl 0            | Focus the file tree        |
