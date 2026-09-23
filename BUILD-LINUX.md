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
- Incremental highlighting (September 2026, on the ThinkPad, Ubuntu 26.04,
  GNOME on Wayland). A temporary test hook opened a generated 50,000-line C
  file in the running app, edited it through the GtkTextBuffer API, and
  compared every character's highlight tag with a full re-lex of the final
  text: 30 comparisons, no mismatches. The edits covered typing, `/*` near
  the top (in a file with no `*/` below, so the whole rest of the file turns
  into a comment) and removing it again, an opening quote, accented, CJK and
  emoji text, a 5,000-line paste, a 3,000-line `insert_range` carrying its
  tags (what a paste from a GtkTextView does), undo and redo, 400 random
  inserts and deletes, edits made while a background retag was still under
  way, renaming to `.txt` and back, and color swatches in the settings file.
  A tag standing in for find matches survived all of it. A second run did the
  same over CRLF, lone-CR and mixed line endings. Timings from a release
  build: a keystroke at line 25,000 takes 0.2 ms; typing `/*` takes 6 ms, the
  lines on screen recolor at once and the rest in the background, with no
  main loop turn longer than 11 ms. A real clipboard copy and paste passed in
  three runs and delivered nothing in two, while other MiniCode windows were
  running and writing the clipboard; Wayland only gives the clipboard to the
  focused window, so treat that one as unconfirmed.
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
- Images and PDFs (September 2026, poppler-glib 26.01, GTK 4.22.4, on the
  ThinkPad under GNOME Wayland at scale 2). A temporary test hook, since
  removed, opened files through the same path a click in the tree takes and
  made 27 checks, all passing:
  - png, jpg, bmp, ico, tiff and webp show in the editor's slot with the
    pixel size in the title (`MiniCode — icon.png  256 × 256`), and an
    animated GIF advances frames. A small image is drawn at its own size,
    not enlarged, which a snapshot of the widget confirmed.
  - A PDF shows in the slot with the page count in the title (`1 page`,
    `100 pages`). A generated 100-page PDF opened in about 10 ms, and the
    main loop never went more than 17 ms without running while its pages
    rendered or after jumping to page 51. Snapshots of the view, read back as images,
    show the pages drawn, and sharp at twice the scale.
  - With an image, a PDF or a binary file open, Save is disabled, calling
    save anyway leaves the file's bytes unchanged, and the buffer is never
    dirty. An image that will not decode falls through to the text path.
  - Replacing an image on disk by rename, rewriting a PDF in place, and
    replacing it with a 3-page PDF all reloaded within a second. The PDF
    kept its scroll position (page 51, 100 points down) through the rewrite,
    and the point at the top of the view stayed there through a window
    resize from 856 to 556 pixels wide.
  - Opening a text file afterwards gives the editor its slot back, editable
    and saveable, with no size or page count in the title.

- Data safety and the file tree actions (September 2026, on the ThinkPad).
  These were driven from inside the running app by a temporary
  `MINICODE_FILETEST` hook, since removed, which ran 50 checks against a
  scratch project, all passing. It activated the window actions, pressed the
  alert buttons by emitting their `clicked` signal, typed names into the name
  popover's entry, and fired the tree's right-click gesture at a row's
  coordinates:
  - Opening another file with unsaved edits asks "Save changes?". Cancel
    keeps the file open with the edits and puts the tree's selection back on
    it; Save writes it and then switches; Don't Save switches and leaves the
    file on disk as it was. Clicking the open file again no longer reloads it
    from disk over its edits. Ctrl+O asks the same question before the folder
    dialog opens, and closing the window asks too: Cancel keeps the window,
    Don't Save closes it and the file is unchanged on disk.
  - A failed save (a read-only file, a folder that cannot be written) keeps
    the buffer marked unsaved, keeps the `*` in the title, and shows an alert
    with the reason. Choosing Save in the "Save changes?" alert when the save
    fails does not switch files. Saves are atomic now; they also write through
    symlinks and keep the file's permissions, though those two were not
    tested. Ctrl+S on a binary file writes nothing,
    and neither a binary file nor an image ever asks "Save changes?". Find in
    Folder goes to its match after the question is answered, not before.
  - New File and New Folder ask for a name in a popover under the selected
    row and create it in the selected folder, or in the selected file's
    folder, or in the root when nothing is selected. The new item is selected
    afterwards and a new file is opened, as on the Mac. A name that already
    exists, or that contains `/`, gets an alert and touches nothing.
  - Rename offers the current name with the part before the extension
    selected. Renaming the open file, or a folder above it, keeps the buffer
    and its unsaved edits, and the next save goes to the new path. A name
    already taken is refused.
  - Move to Trash asks first, and says so when the open file has unsaved
    edits that would go with it. The file really lands in the Trash (checked
    in a scratch `XDG_DATA_HOME`), and trashing the open file resets the
    editor to the welcome text. On a tmpfs such as /tmp, GIO refuses to trash
    at all ("Trashing on system internal mounts is not supported"); that shows
    as an alert and the file stays put.
  - Copy Path puts the selected item's absolute path on the clipboard, read
    back from the clipboard. A right-click on a row selects it and opens the
    menu with all six items, whose actions resolve from inside the popover; a
    right-click on empty space clears the selection, so New File there goes
    to the root.
  - The tree refreshes by itself again. It turned out that on GTK 4.22
    `GtkDirectoryList` with monitoring on never reports a change, although a
    `GFileMonitor` on the same folder sees every one, so nothing created,
    renamed or deleted after a folder was first listed ever showed up. Each
    folder is now a list the tree keeps itself, from its own `GFileMonitor`.
    Creating, renaming and deleting files and folders from outside the app,
    including renaming an expanded folder, all show up.
- The LaTeX preview (September 2026, on the ThinkPad under GNOME Wayland,
  poppler-glib 26.01, tectonic 0.17.0). A temporary test hook, since
  removed, drove the real code paths in the running app, launched under its
  own application id with a scratch settings folder and a scratch tectonic
  cache, on scratch copies of the documents. 55 checks passed, plus 3 and 2
  in separate runs:
  - With no tectonic anywhere, opening `notes.tex` shows the message and a
    Download… button. Pressing it (its real "clicked" signal) downloaded the
    pinned release into `~/.local/share/minicode/bin` (that copy is now
    installed on the ThinkPad), byte for byte the published build, left no
    archive behind, and typeset the document. With a stand-in `curl` that
    writes junk, the download is refused for its checksum, nothing is
    installed, and Download… is offered again.
  - `demo/paper/notes.tex` (1 page) and `tests/latex/torture.tex` (2 pages)
    typeset; the pages, rendered with `PdfView::renderPage`, and snapshots
    of the widget were looked at. The hidden sibling is gone after each run
    and the file on disk never changed.
  - Every word of both documents was double-clicked at its first, middle
    and last character by emitting the page view's click gesture with a
    press count of 2, and the popover's text judged against the word:
    notes.tex 185 right, 0 refused, 0 wrong; torture.tex 1,083 right,
    8 refused (text from `\newcommand` bodies, refused on purpose), 0 wrong.
    Single characters (49 and 5) are counted apart, as on the Mac; see
    `linux/HANDOFF.md` item 7 for what happens to them. The harness,
    `tests/latex/sweep-linux.sh`, gives the same counts without the GUI.
  - A double-click on "Summary" opens a popover titled "Editing text" holding
    `Summary.`; on "draft", its list item with Add item. A click in the
    margin opens nothing and says so.
  - Saving an edit replaces exactly the span's bytes in the buffer, marks it
    unsaved, can be undone in one step, and typesets at once; the new text
    is on the page and opens again when clicked. Add item writes a new
    `\item` after the clicked one, indented like it. A click while the PDF
    is behind the buffer opens nothing. An undefined command shows the log
    with "Did not typeset — see the log", and undoing it brings the pages
    back.
  - The scroll position (page 2, 150 points down) stayed put through a
    retypeset. Ctrl+Shift+P switches to the editable source and back. The
    PDF for export comes back at once when current, and after a typeset
    when the source view has unsaved typing, containing it.
  - A rename keeps the pages clickable, and the next typeset uses the new
    sibling name. Closing the file (what Open Folder does) and opening a text
    file both take the preview away; with unsaved preview edits, opening
    another file asks "Save changes?" first.
  - Quitting while tectonic runs stops it and removes the hidden sibling;
    killing the app with SIGKILL takes tectonic with it (checked with a
    stand-in that sleeps 31 seconds). Only a killed app leaves the sibling
    behind.

Not verified:

- Everything driven by real keyboard and mouse input. Actions were activated
  programmatically, which proves the wiring but not the key handling. That
  includes scrolling a PDF with the wheel and clicking a page (the mapping
  from a point to a page and PDF coordinates was checked, the click was not).
  For Find in Folder it includes pressing Ctrl+Shift+F, typing into the field (the
  test set its text, which fires the same signal), double-clicking a row, the
  Down arrow and Escape, the Choose button's folder dialog, and whether GNOME
  raises the main window when a match is opened (the editor is made the
  window's focus widget, but activating a window is up to the compositor).
  For the file tree it includes F2 and Delete, a real right-click, and what
  the popovers and alerts look like and where they sit. For the LaTeX
  preview it includes a real double-click, Return, Shift+Return and Escape
  in the popover, Ctrl+Z in the preview, Ctrl+Shift+S, and the Export PDF
  save dialog and the file it writes (the bytes handed to it were checked,
  the dialog was never opened).
- Where the LaTeX popover sits and how it looks on screen. A snapshot of the
  popover widget looked right, but its placement over the clicked text was
  not seen.
- How the Find in Folder window looks. Nothing in it was seen on screen.
- How images and PDFs look to a person: the checks above read pixels back
  from the widget, not from the screen. A build without poppler was compiled
  and launched on a PDF, but what it shows was not looked at.
- How highlighting looks while real keys are typed. The test above edited the
  buffer through its API; nobody has watched the colors catch up by eye.
- Without optimization the lexer is several times slower: typing `/*` over a
  50,000-line file took 116 ms and closing it with `*/` 281 ms, against 6 and
  27 ms in a release build. `meson.build` therefore defaults to
  `debugoptimized`; a build directory set up before that change keeps
  `debug` until `meson configure build --buildtype=debugoptimized`.
- The Open Folder dialog itself, which was not opened (only the question
  asked before it, and the re-rooting done after it).
- Open Containing Folder, which was not run, because it opens a file manager
  window.

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
   signatures are unchanged. What did not hold up, found later, was
   `GtkDirectoryList`'s own monitoring, which reports no changes on GTK 4.22;
   the tree now fills each folder's list itself (see above) and sets the same
   attribute.

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
        libvte-2.91-gtk4-dev libwebkitgtk-6.0-dev libpoppler-glib-dev pkg-config

Check afterwards that all four actually landed, since apt will happily install
the runtime library while leaving the `-dev` package out:

    pkg-config --modversion gtk4 vte-2.91-gtk4 webkitgtk-6.0 poppler-glib

Notes on package names, which drift between Ubuntu versions:

- `libgtk-4-dev` is required. Everything else is optional.
- `libvte-2.91-gtk4-dev` provides the GTK4 build of VTE. On some releases the
  package or its pkg-config module may be named slightly differently (the
  pkg-config module this build looks for is `vte-2.91-gtk4`). If the terminal
  panel is silently skipped, check `pkg-config --exists vte-2.91-gtk4`.
- `libwebkitgtk-6.0-dev` provides the GTK4 WebKit (pkg-config module
  `webkitgtk-6.0`). Older Ubuntu used `libwebkit2gtk-4.1-dev` with a different
  API; that will not work with this code as written.
- `libpoppler-glib-dev` provides the PDF viewer (pkg-config module
  `poppler-glib`). Without it a PDF shows the "Cannot display" message, and
  a `.tex` file opens as plain source, with no LaTeX preview.
- The LaTeX preview typesets with [tectonic](https://tectonic-typesetting.github.io/),
  a separate program, not a build dependency. MiniCode uses `MINICODE_TECTONIC`,
  then `~/.local/share/minicode/bin/tectonic`, then one on your PATH, and if
  there is none the preview offers to download the official 0.17.0 build
  (about 10 MB, checked against its published checksum) into
  `~/.local/share/minicode/bin`. It needs `curl` or `wget` and `tar` for that.

If you only install `libgtk-4-dev`, the core viewer still builds, images
included. The terminal, the browser and the PDF viewer are compiled out
automatically when their libraries are absent.

## Build with Meson (recommended)

    cd linux
    meson setup build
    meson compile -C build
    meson test -C build
    ./build/minicode ~/some/project

Meson auto-detects VTE, WebKit and poppler. Watch the configure output: it
prints whether each panel is ENABLED or disabled. To force a panel on (and make
configuration fail loudly if the library is missing):

    meson setup build -Dterminal=enabled -Dbrowser=enabled -Dpdf=enabled

To force one off:

    meson setup build -Dterminal=disabled

## Build with the Makefile (fallback)

If you would rather not use Meson:

    cd linux
    make                      # core viewer only (needs just libgtk-4-dev)
    make TERMINAL=1           # add the terminal panel
    make BROWSER=1            # add the browser panel
    make PDF=1                # add the PDF viewer
    make TERMINAL=1 BROWSER=1 PDF=1
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
| Ctrl Shift P      | Toggle the Markdown or LaTeX preview |
| Ctrl Shift S      | Export the typeset PDF of a LaTeX file |
| Ctrl Shift H      | Show or hide the shortcut hints |
| Ctrl B            | Toggle the sidebar         |
| Ctrl Shift E      | Collapse or restore the editor |
| Ctrl Shift T      | Toggle the terminal panel  |
| Ctrl /            | Comment or uncomment the selected lines |
| Ctrl ,            | Open the settings file     |
| Ctrl Shift B      | Toggle the browser panel   |
| Ctrl H            | Show or hide dotfiles      |
| Ctrl 0            | Focus the file tree        |
| F2                | Rename the selected item, in the tree |
| Delete            | Move the selected item to the Trash, in the tree |

F2 and Delete work only while the file tree has the keyboard, so Delete in the
editor still deletes text. The tree's right-click menu and the File menu both
have New File, New Folder, Rename, Move to Trash, Open Containing Folder and
Copy Path.
