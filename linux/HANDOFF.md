# Linux port: handoff, September 2026

The GTK4 port under `linux/` was last worked on in August 2026. Since then the
macOS app has gained its biggest features: the LaTeX preview, the LSP client,
incremental highlighting, image and PDF viewing, and several file tree
actions. The Android port has caught up on most of them. This document lists
what Linux lacks, in the order to do it, with where each thing lives on the
Mac and what the GTK equivalent is.

Read these first:
- `../BUILD-LINUX.md`: build, install and what was verified in August.
- `dev/README.md`: the Docker container with a virtual display for driving
  the app from a Mac.
- `../CLAUDE.md`, especially "LaTeX preview", "LSP", "Images" and "Gotchas".
  The design decisions there apply here too.
- `../android/README.md`: the Android port solved the same problems more
  recently, without AppKit, so it is often the closer model.

## Where it stands

The port builds without warnings in CI (Ubuntu, GTK 4, VTE, WebKitGTK) and has
been run for real on Ubuntu 26.04. It has:
- the file tree, live-refreshing;
- the editor with syntax highlighting (a debounced full re-lex);
- the Markdown preview;
- a VTE terminal and a WebKitGTK browser;
- the settings file with live colors and per-panel opacity;
- Ctrl+/ comment toggling, pane hiding and divider drags;
- the shortcut hints panel;
- opening a file named on the command line.

It shares from `../src` only `SyntaxHighlighter`, `MarkdownParser`, `Settings`
and `LineComments`. The rest of the core (`LatexDoc`, `SyncTex`, `Json`,
`LspClient`, `TerminalScreen`) is portable and tested, and has not been added
to `meson.build` yet.

## What to do, in order

Each item says where the Mac does it, what to use in GTK, and when it is
done. Keep the rules from `../CLAUDE.md`: no third-party dependencies beyond
the system libraries a distribution ships, logic in `../src` with tests, and
the GUI kept thin.

### 1. Check data safety first (done, September 2026)

Before adding features, confirm the basics that were never verified on Linux
(`BUILD-LINUX.md`, "Not verified"): saving, the Open Folder dialog, creating
files and folders, and what happens to an edited buffer when another file is
opened. The Mac asks "Save changes?" and Android now does too. If Linux
silently drops the edit, fix that before anything else.

Done when: an edited file cannot be lost by opening another file, opening a
folder or closing the window, and a failed save leaves the buffer marked
unsaved.

What was found, each confirmed by running the old code before fixing it:
opening another file dropped the edits without a word; clicking the open file
in the tree reloaded it from disk over its edits; Ctrl+S on a binary file
wrote the "Cannot display" message over it (12 bytes became 74); a failed
save reported success and cleared the unsaved mark; and New File opened its
new file over an edited buffer. Now `confirmUnsaved` in `main.cpp` asks
"Save changes?" (GtkAlertDialog: Save, Don't Save, Cancel) before opening
another file, before Ctrl+O, and on the window's `close-request`; the Mac's
`-confirmProceedPastUnsavedChanges` is the model. `Editor::save` reports
failure with a reason, which is shown in an alert, and writes atomically.
Open Folder closes the old file, as the Mac does.

Runtime-verified with a temporary hook (see `BUILD-LINUX.md`). Not seen by a
person yet: the alerts themselves, and the real Open Folder dialog, which the
test never opened.

### 2. File tree actions (done, September 2026)

The Mac has New File, New Folder, Rename, Move to Trash, Reveal and Copy Path
in the tree's right-click menu and the File menu (`src/EditorController.mm`).
Linux creates `untitled.txt` in the root and has no rename (`main.cpp`,
`act_new_file`).

- A right-click `GtkPopoverMenu` on the list rows.
- Rename and new-file names through a small entry popover.
- `g_file_trash` for Move to Trash, and `gdk_clipboard_set_text` for Copy
  Path.
- "Reveal" becomes opening the containing folder through
  `GtkFileLauncher`.

Done when: every item works from the menu and the tree, a renamed open file
stays open under its new name, and trashing the open file resets the editor.

All six are in the File menu and the tree's right-click menu, built from one
`GMenuModel` (`treeActionsMenu` in `main.cpp`); the actions are `act_new_file`,
`act_new_folder`, `act_rename`, `act_trash`, `act_reveal` and
`act_copy_path`. `FileTree` gained `selectedPath`, `revealPath` (selects a
row, expanding folders and retrying while rows load), `setContextMenu` and
`askName` (the entry popover). F2 and Delete rename and trash, but only while
the tree has focus, through a shortcut controller on the tree rather than
window accelerators. Renaming a folder above the open file moves the open
file's path too, which the Mac does not do.

Runtime-verified with the same hook, except as follows. Driven by signals,
not by a real mouse or keyboard: the right-click, F2 and Delete. Not run at
all: Open Containing Folder, which would have opened a file manager window.
Needs the user's eyes: where the popovers sit and how they look.

The tree's live refresh was broken all along on GTK 4.22 (see "Traps"), and
is fixed as part of this item.

### 3. Incremental highlighting

Mac: `IncrementalHighlighter` in `../src/SyntaxHighlighter.h`, driven from
`textStorage:willProcessEditing:` and `flushHighlighting` in
`EditorController.mm`. Android: `Highlighter.kt` feeding a native mirror.

GTK: connect to the buffer's `insert-text` and `delete-range` (use the
`_after` variants, or record the edit and apply it in `changed`), feed each
edit to `IncrementalHighlighter<char>` in byte offsets (the buffer is UTF-8,
and `Utf8Offsets.h` already converts to character offsets), then retag only
the lines it reports. Remove the 120 ms debounce in `Editor.cpp`.

Done when: typing in a 50,000-line file does not stall and typing `/*`
recolors the rest of the file. The logic itself is already covered by the
core's 14,000-edit test, so the GTK side needs no new unit tests.

### 4. Images and PDFs

Mac: `showImageAtPath:` and `showPDFAtPath:` in `EditorController.mm`, and the
"Images" section of `../CLAUDE.md`. Linux shows "cannot display" (the
`g_utf8_validate` branch in `Editor.cpp`).

- Images: `GtkPicture` with `gtk_picture_set_can_shrink` and content fit
  `SCALE_DOWN`, so small images are not enlarged. Show the pixel size in the
  title.
- PDFs: `poppler-glib` rendering into a scrolling list of pages. Make it an
  optional dependency like VTE (`-Dpdf=auto`), because the LaTeX preview
  needs it too.

Done when: png, jpg, gif, webp and PDF open in the editor's slot, nothing is
ever saved over them, and they reload when the file changes on disk.

### 5. Find in folder

Mac: `src/Search.mm` (scoped to a folder, at least 2 characters, a generation
counter cancels stale searches, ANSI stripped). Linux has only the in-file
find bar.

GTK: a window or side panel with a `GtkSearchEntry` and a `GtkListView` of
results, searching on a `GTask` thread. The search itself should go into
`../src` as a pure function (a folder, a query, a cancel flag, and matches
out) with tests, which Android can then use too.

### 6. Language servers

Mac: `src/Lsp.mm` over `../src/LspClient.cpp`, described in the "LSP"
section of `../CLAUDE.md`. Android: `LspSession.kt` and `lsp_jni.cpp`, which
show how little glue the core needs.

- Processes: `GSubprocess` with stdin and stdout pipes. Send stderr to
  `/dev/null` unless `MINICODE_LSP_LOG` is set, because clangd fills a pipe
  and stalls. Find servers on PATH plus `~/.cargo/bin`, `~/go/bin` and
  `~/.local/bin`.
- Diagnostics are easier than on the Mac: a `GtkTextTag` with
  `underline = PANGO_UNDERLINE_ERROR` and an `underline-rgba` draws the
  squiggle. Keep those tags separate from the highlighting tags, so a retag
  never erases them.
- Hover: the text view's `query-tooltip`. Completion: a `GtkPopover` with a
  list, opened on the server's trigger characters and on Ctrl+Space.
  Definition: F12 and Ctrl+click, through the same open-and-reveal path the
  tree uses.
- Settings keys are the Mac's (`lsp.enabled`, `lsp.cpp` and the rest), which
  `Settings.cpp` already parses.

Done when: clangd on a scratch project shows squiggles, completion, hover
and definition, and no server process outlives the app.

### 7. The LaTeX preview

Mac: `src/Latex.mm`; read the whole "LaTeX preview" section of `../CLAUDE.md`
before starting. Android: `LatexPreview.kt` and `latex_jni.cpp`.

- tectonic comes from `MINICODE_TECTONIC`, then
  `~/.local/share/minicode/bin`, then PATH. Offer the same download of the
  pinned release as the Mac.
- Typeset from the hidden sibling `.<name>.minicode.tex` into
  `$XDG_RUNTIME_DIR` or `/tmp`, debounced and generation-counted.
- Show every page with poppler-glib (from item 4), keeping the scroll
  position across typesets.
- Double-click to edit: `SyncTexIndex::textHitsAtPoint` for the lines, the
  word and 40 characters either side from `poppler_page_get_text` and
  `poppler_page_get_text_layout`, then `LatexDoc::spanForClick`. The edit
  UI is a `GtkPopover` holding the span's LaTeX; commit splices bytes.
- Export PDF (Ctrl+Shift+S) copies the PDF tectonic wrote.
- Measure with `tests/latex/sweep.sh`, which drives the Mac click path. A
  Linux equivalent is worth building if the word finding differs from
  PDFKit's.

Done when: `demo/paper/notes.tex` and `tests/latex/torture.tex` typeset, and
double-clicking words opens the right text or refuses; it never opens the
wrong text.

### 8. The smaller gaps

- New Window, Previous File, Find Next and Find Previous. Ctrl+Shift+N is
  already New Folder on Linux, so give New Window a key that doesn't clash.
- Show Hidden Files exists as Ctrl+H; check it against the Mac's behaviour.
- Blur and live color picking, as `../ROADMAP.md` describes ("Linux port
  parity").
- Run the core tests on Linux in CI: add `make test` (or a Meson test target
  built from `tests/run_tests.cpp`) to the `linux` job in
  `.github/workflows/ci.yml`. Today only the Mac job runs them.

Not needed on Linux: the demo recorder and the memory benchmark, which are
Mac tools.

## How to work on it

Two ways to run the app, both described in `../CLAUDE.md` ("Verification
reality"):

- **From the Mac:** `linux/dev/gtk-dev.sh` builds a Docker image with Xvfb
  and a compositor, mounts the repo, and builds and tests the port. Drive it
  with `xdotool`, capture with `import`, and look at the screenshots.
  - Build into a directory inside the container (`meson setup /build`).
  - Use `pkill -x minicode`, never `pkill -f`.
  - WebKit needs `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` in a container.
  - The Mac's disk is tight, and the image is about 1.8 GB.
- **On the user's ThinkPad** (Ubuntu 26.04, GNOME on Wayland): a session
  there can launch the real app. Check `echo $WAYLAND_DISPLAY` first. A
  window appears on the user's screen, so say so and never leave one
  running.

Per feature:
1. Put the logic in `../src` with tests, and run `make test`.
2. Build with `meson compile -C build` at `warning_level=2`, with zero
   warnings.
3. Drive the feature for real, in the container or on the ThinkPad, and
   check a screenshot.
4. Update `BUILD-LINUX.md` ("What is verified") and this file.

Do not claim a GUI behaviour works when it was only compiled.

## Traps already known

- Byte offsets versus character offsets: the core speaks UTF-8 bytes and
  `GtkTextIter` speaks characters. `Utf8Offsets.h` converts, with 47 tests;
  use it for every range that crosses over.
- A `.desktop` file and icons must be named after the application id
  (`org.minicode.Editor`), or the dock shows a second, generic icon.
- `apt` can install a runtime library without its `-dev` package, and Meson
  then quietly builds without that panel. CI checks `pkg-config` for this;
  do the same for poppler when it is added.
- `GtkDirectoryList` with monitoring on reports no changes on GTK 4.22.4: a
  standalone program showed a `GFileMonitor` on the same folder receiving
  every create, rename and delete while the list emitted nothing. The tree
  therefore keeps each folder's list itself (`makeDirModel` in
  `FileTree.cpp`). Do not switch back without testing a create in an
  already-listed folder.
- `g_file_trash` refuses anything on a tmpfs such as /tmp ("Trashing on system
  internal mounts is not supported"). A test of Move to Trash needs its
  scratch project on the home filesystem, with `XDG_DATA_HOME` pointed at a
  scratch folder there so nothing reaches the user's real Trash.
- A test launch on the ThinkPad should run under `dbus-run-session`. The app
  is a single-instance GApplication, so a plain launch would hand its
  arguments to whatever MiniCode is already running, the user's or another
  session's, and exit.
- An alert from `gtk_alert_dialog_show` (no buttons of our own) did not close
  when its Close button was activated from a test hook; the error alerts now
  set an OK button and use `gtk_alert_dialog_choose`, the same path as
  "Save changes?".
