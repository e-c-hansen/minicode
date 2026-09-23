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
- the editor with incremental syntax highlighting (item 3 below);
- the Markdown preview;
- a VTE terminal and a WebKitGTK browser;
- the settings file with live colors and per-panel opacity;
- Ctrl+/ comment toggling, pane hiding and divider drags;
- the shortcut hints panel;
- opening a file named on the command line;
- Find in Folder (Ctrl+Shift+F), over the shared `FolderSearch` core.

It shares from `../src` only `SyntaxHighlighter`, `MarkdownParser`, `Settings`,
`LineComments` and `FolderSearch`. The rest of the core (`LatexDoc`, `SyncTex`, `Json`,
`LspClient`, `TerminalScreen`) is portable and tested, and has not been added
to `meson.build` yet.

## What to do, in order

Each item says where the Mac does it, what to use in GTK, and when it is
done. Keep the rules from `../CLAUDE.md`: no third-party dependencies beyond
the system libraries a distribution ships, logic in `../src` with tests, and
the GUI kept thin.

### 1. Check data safety first

Before adding features, confirm the basics that were never verified on Linux
(`BUILD-LINUX.md`, "Not verified"): saving, the Open Folder dialog, creating
files and folders, and what happens to an edited buffer when another file is
opened. The Mac asks "Save changes?" and Android now does too. If Linux
silently drops the edit, fix that before anything else.

Done when: an edited file cannot be lost by opening another file, opening a
folder or closing the window, and a failed save leaves the buffer marked
unsaved.

### 2. File tree actions

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

### 3. Incremental highlighting (done, September 2026)

Done and checked in the running app on the ThinkPad; `BUILD-LINUX.md` has
the test and the timings. How it works, in `Editor.cpp`:

- `insert-text` and `delete-range` handlers that run before the buffer
  changes turn each edit into UTF-8 byte offsets (the highlighter's line
  start plus `gtk_text_iter_get_line_index`), apply it to `mirror_`, a copy
  of the buffer that the highlighter reads, and feed it to
  `IncrementalHighlighter<char>`. The `_after` handlers, or `end-user-action`
  when the edit is inside one, retag the lines it reported.
- A retag removes only the highlight tags (`hlTags_`: one per style plus the
  settings swatches) and only over those lines, so find matches and future
  diagnostic tags are never touched. An `apply-tag` handler refuses highlight
  tags from anyone else, because a paste from a GtkTextView brings the
  source's tags along.
- Retagging costs GTK a few microseconds per tag, so over 1,000 lines the
  lines on screen are retagged at once and the rest in idle slices of 5 ms.
- A user action with more than 64 edits in it (a large paste arrives as one
  insert per run of tags) stops being tracked edit by edit; at its end the
  old and new text are compared once for a common prefix and suffix, and
  that becomes the edit.
- GTK also ends lines at a lone `\r` and U+2029, the lexer only at `\n`.
  When the line counts differ, offsets are counted through `mirror_`
  instead, which is slower but correct.
- `Editor::setPath` is the hook for rename and save-as: it re-lexes only if
  the grammar changed. Nothing calls it yet; the file tree work should.

Left over: turning off highlighting (a rename to `.txt`) removes the tags
from the whole buffer at once, about 0.2 s for 50,000 lines. The debug build
is slow here (see `BUILD-LINUX.md`).

What the item said:

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

### 5. Find in folder (done, September 2026)

Mac: `src/Search.mm`. The search is now `../src/FolderSearch.{h,cpp}`, a pure
C++17 function (a folder, a query, an `std::atomic<bool>` cancel flag, and
matches out with the line, the column in characters and in bytes, and the
cleaned display text) with the Mac's rules, tested in the core suite. Android
can call it through JNI as it is.

The GTK side is `src/Search.{h,cpp}`: a separate window like the Mac's, with
a folder field and Choose button, a `GtkSearchEntry` (0.35 s typing pause, as
on the Mac), a status line and a `GtkListView` over a `GtkStringList` of
markup rows. Each search runs on a `GTask` thread; a generation counter plus
the previous search's cancel flag make sure a superseded search stops and its
results are dropped. Activating a row goes through `openFileCb` in `main.cpp`
(the tree's open path, so any unsaved-changes prompt added there applies) and
then `Editor::revealLine`, which selects the match and switches a Markdown
preview to source. The scope is the folder selected in the tree, else the
open folder, and it resets after Open Folder. `FileTree::selectedDir()` reads
the selection; the tree's selection no longer autoselects its first row.

Verified by driving it inside the running app on the ThinkPad (see
`../BUILD-LINUX.md`); real key presses, the double-click and the look of the
window still need a person.

If opening a file ever becomes asynchronous (a save prompt that returns
later), `openSearchMatch` in `main.cpp` has to go to the line after the open
completes; today it checks `currentPath()` straight after `openFileCb`.

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
- Done: the core tests run on Linux in CI. The top-level Makefile picks
  clang++ only on the Mac, so `make test` builds with g++ here, cleanly, and
  the `linux` job in `.github/workflows/ci.yml` runs it.

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
