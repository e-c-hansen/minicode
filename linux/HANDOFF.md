# Linux port: handoff, September 2026

The work from here is in `PARITY.md`: its stages, rules, and decisions come
first. The items below are done, and stay for how each was built and what it
cost.

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

Items 1 to 11 below are done (September 2026), so the port has what the Mac
had then, apart from what the list at the end of item 8 leaves out. The
registry's keys, tabs, line numbers, sessions and backups, and the command
palette came in November 2026 (items 15 to 18, and `PARITY.md`). Almost
everything else the Mac gained from 1.5.0 on (folding, several cursors,
rename and the other language server commands, formatting, the viewers,
the agent features and the command line control) is not here yet;
`PARITY.md` is the plan, and
`../ROADMAP.md`, "Linux parity", lists the core files that already hold its
logic. It builds
without warnings at `warning_level=2` (CI: Ubuntu, GTK 4, VTE, WebKitGTK,
poppler) and has been run for real on Ubuntu 26.04 under GNOME on Wayland:
- data safety: "Save changes?" before anything replaces edits, failed saves
  reported, atomic saves (item 1);
- the file tree, live-refreshing, with New File, New Folder, Rename, Move to
  Trash, Open Containing Folder and Copy Path (item 2), and rows that drag
  onto folders (item 13);
- the editor with incremental syntax highlighting (item 3), Ctrl+/, the
  Markdown preview, and reloading when the file changes on disk (item 8);
- Markdown task lists: boxes to click in the preview, Ctrl+L, Return
  continuing a list, the count in the title, and the TODOs window on
  Ctrl+Shift+L (item 14);
- images and PDFs in the editor's slot (item 4), with zoom for PDFs and the
  LaTeX preview;
- Find in Folder (item 5) with Replace in Folder, and the find and replace
  bar in the file with VS Code's options (item 20);
- language servers: squiggles, completion, hover and go to definition
  (item 6);
- the LaTeX preview with double-click editing and Export PDF (item 7);
- several windows, each with its own tree, editor, terminal, browser, search
  panel and language servers; Focus Editor, Close Window and Quit (item 8);
- tabs, with preview tabs, Reopen Closed Tab and Save All (item 15, which
  replaced Previous File);
- brackets boxed beside the caret and colored by depth, Go to Bracket, and
  brackets and quotes that close as they are typed (item 21);
- large-file mode, files over 4 MB read without holding the window, and
  lines over 10,000 characters shown a part at a time (item 22);
- a VTE terminal that gets its own Ctrl keys while it has the keyboard, with
  Ctrl+click on a file:line or URL in its output (item 9), and a WebKitGTK
  browser;
- the Source Control panel with the commit graph, in the tree's place on
  Ctrl+Shift+G (item 10);
- video and audio in the editor's slot through GTK's GStreamer backend,
  with Play/Pause on Ctrl+Shift+Space (item 11);
- the settings file with live colors, per-panel opacity and a live color
  picker (item 8);
- pane hiding and divider drags, the shortcut hints panel, and opening a
  file or folder named on the command line.

It shares from `../src` `SyntaxHighlighter`, `MarkdownParser`, `MarkdownEdit`,
`MarkdownTasks`, `Settings`, `LineComments`, `AutoIndent`, `KeepSorted`,
`FolderSearch`, `TextFind`, `LatexDoc`, `SyncTex`, `Json`, `LspClient`,
`TermLinks`, `GitStatus`, `GitGraph`, `GitModel`, `BracketMatch`, `BracketPairs`,
`AutoClose` and `MultiCaret`. `meson.build`'s `core_sources` and
the plain Makefile's `CORE` must name the same files: CI builds only with
Meson, so the Makefile once fell three files behind and could not link, and
`make test`'s `linux:build-lists` check now compares the two.
The rest of the core (`TerminalScreen`) is portable and tested, and has not
been added to `meson.build`, since VTE does that job here.

What each item verified at run time, and what still needs a person (mostly
real key presses and how things look), is in `../BUILD-LINUX.md`.

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

Opening a file is therefore asynchronous: the alert's answer arrives later.
Anything to do once the file is open goes through `openFileThen(app, path,
then)`, whose `then` runs after the open and not at all on Cancel. Find in
Folder's go-to-match and Ctrl+, use it, and go-to-definition (item 6) should
too; calling `openFileCb` and then acting on the editor straight away acts on
the old file whenever the question is asked.

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
not by a real mouse or keyboard: F2 and Delete. Not run at all: Open
Containing Folder, which would have opened a file manager window. Needs the
user's eyes: where the popovers sit and how they look.

**Clicks and selection (September 2026, from the user's own testing).** The
tree showed no selection at all (the stylesheet's transparent row background
had wiped out the theme's selection color), and needed a double-click where
the Mac takes one. Now one click acts: a primary-button `GtkGestureClick` on
the list view, in the bubble phase so the row's own handler has selected it
first, acts on release when press and release are on the same row, and only
for the first click of a double-click. The rows are not activatable, so GTK's
own double-click activation cannot act a second time; a press on the
expander's arrow is left to the arrow. Enter, Left and Right come from a
capture-phase key controller: Enter opens and moves the keyboard to the
editor (the Mac's `-activateRow:`), Left goes up and closes, Right opens and
steps in. `gtk_list_view_set_single_click_activate` was not used because it
also selects whatever row the pointer rests on. `openFileThen` no longer
reopens the file already open, as the Mac's `openFileAtPath:` does not, so a
click on it keeps the caret, the scroll and a preview switched to source;
changes made elsewhere come in through the editor's file watch. The selected
row is blue while the tree has the keyboard and a tint of the sidebar's text
color when not, with a focus ring after keyboard use. Checked with real X
input (see "How to work on it" and `../BUILD-LINUX.md`).

The tree's live refresh was broken all along on GTK 4.22 (see "Traps"), and
is fixed as part of this item.

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
  the grammar changed. Rename in the file tree calls it, in every window
  that has the file (or a file under a renamed folder) open, since a window
  left on the old name would save its edits there and bring the old file
  back. It also re-decides the mode from the name (`.txt` to `.md` gains the
  preview; a preview whose file stops being Markdown or LaTeX goes back to
  the source) and tells a picture or PDF viewer its new name, so it keeps
  reloading on change.

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

### 4. Images and PDFs (done, September 2026)

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

**Done, September 2026.** `src/MediaView.{h,cpp}` holds the picture and the
PDF view and watches the shown file with a `GFileMonitor`; `Editor::widget()`
is now a `GtkStack` with the text view and the media view, so an image or PDF
takes the editor's slot and the next text file takes it back. The extension
list is the Mac's (SVG stays text, on purpose). Still images load through
`gdk_texture_new_from_filename`; animated GIFs go through gdk-pixbuf's
animation API, which 2.44 deprecated, so that one function silences the
warning. While an image, a PDF or a binary file's message is shown,
`Editor::canSave()` is false, `win.save` is disabled and `save()` writes
nothing. poppler is optional (`-Dpdf=auto`, `make PDF=1`,
`MINICODE_ENABLE_PDF`). What was checked, and how, is in `BUILD-LINUX.md`.
Zoom came later (below); still not done: a person looking at it. Video and
audio joined the same view later still (item 11).

`src/PdfView.{h,cpp}` is the piece item 7 reuses. It shows every page in a
scrolling column fitted to the width, rendered lazily (visible pages first,
one at a time on a worker thread, at the surface's scale factor, bitmaps
dropped for pages far from view). Its API, with 0-based pages and points from the page's
top-left as SyncTeX uses them:
- `load(path, keepPosition)`: reads the bytes first, so a PDF being
  rewritten cannot pull data out from under poppler; on failure the old
  document stays on screen.
- `pageCount()`, `pageSize(i, &w, &h)`, `document()` for poppler calls such
  as `poppler_page_get_text_layout`.
- `pageAtPoint(x, y, &page, &px, &py)` from widget coordinates, and
  `setClickCallback(cb, user)`, which reports page, point and press count
  (2 for a double-click).
- `anchor()` and `scrollTo(anchor)`: the page and y at the top of the view,
  held through the relayout after a load or resize.
- `renderPage(page, pixelWidth)`: one page as a cairo image, for tests.
- `zoom()`, `effectiveZoom()`, `setZoom(z, x, y)`, `zoomIn()`, `zoomOut()`,
  `zoomToFit()`, and `setShownCallback` (see Zoom below).
- `widget()` is now a `GtkOverlay` holding the scrolled window and the zoom
  badge. Both have the same origin, so widget coordinates did not change.

A trap paid for: page sizes set from the adjustment's `changed` signal land
in the middle of an allocation and are never laid out; the box kept its old
width after a resize. The relayout now runs from an idle.

**Zoom, September 2026.** The Mac has no zoom keys, so this was designed
plainly, for the PDF viewer and the LaTeX preview alike (both are a
`PdfView`):

- **Scale.** `zoom_` is 0 for fit width, the default, or a factor where 1.0
  is 100%: a page at its printed size on a nominal 96 dpi display, 4/3
  logical pixels per point, as browsers and pdf.js have it. The range is
  25% to 400%. Zoomed, every page is its size in points times the zoom, and
  the view scrolls sideways when the widest page is wider than it; a window
  resize keeps the size. Fit width still fits the widest page.
- **Keys.** Ctrl+Plus, Ctrl+Equal and keypad Plus zoom in, Ctrl+Minus and
  keypad Minus zoom out, through the steps 25, 33, 50, 67, 75, 90, 100, 110,
  125, 150, 175, 200, 250, 300 and 400%, about the view's centre. Fit width
  is **Ctrl+Alt+0**: Ctrl+0 is Focus the File Tree (the Mac's Command 0), and
  one of the pane keys that stay bound while the terminal has the keyboard, so
  it is not borrowed. The actions (`win.zoomin`, `win.zoomout`,
  `win.zoomfit`, in the View menu) are enabled only while a `PdfView` is
  mapped, which `Editor::showingPdf()` reports and `updateTitle` applies;
  `PdfView` calls its shown callback on map and unmap, and the editor passes
  that on as a title update. Disabled, their keys go on to the focused
  widget. Ctrl+Plus and Ctrl+Minus are plain Ctrl keys, so `shellOwns` gives
  them to the shell while the terminal has the keyboard, with no new code;
  Ctrl+Alt+0 stays bound there, as everything with Alt does.
- **Wheel and pinch.** A scroll controller in the capture phase (so it runs
  before the scrolled window's own) takes the wheel when Ctrl is held: a
  notch is 10%, a touchpad's smooth scroll `exp(-dy / 100)`. A motion
  controller keeps the pointer's position, and the zoom is about it. A
  `GtkGestureZoom` zooms about the point between the fingers, relative to the
  zoom when the pinch began.
- **Holding a point.** `setZoom` finds the document point (page, and x and y
  in points, allowed off the page's edge) under the given viewport point,
  lays out at the new scale, and scrolls so the point is back under it. It
  works from the layout it just made (`pageTop`, `pageLeft`, `contentSize`,
  which reproduce the box's spacing and GTK's rounding in centring), not
  from allocations, which lag. A page narrower than the view is centred, so
  only the vertical position holds then. Successive steps of a pinch start
  from where the last one meant to be (`scrollValues`), not from where GTK
  has got to.
- **Keeping and resetting.** `load(path, true)` keeps the zoom and now also
  the sideways scroll, so a LaTeX retypeset and a reload from disk keep
  both. `clear()` and a load without `keepPosition` go back to fit width,
  which is what opening another file does in the viewer and the preview.
- **The badge.** A label in the overlay, bottom right, shows "150%" or "Fit
  width, 79%" for 1.2 s after each change. It is not in the title because
  the LaTeX preview has none of its own and the level changes too often for
  a title.
- **Rendering.** Still lazy, one piece at a time, at the surface's scale,
  and since the review of September 2026 off the main thread: the idle picks
  the piece, a `GTask` thread draws it with its own `PopplerDocument` opened
  from the same bytes (a document is not safe to share between threads, two
  are), and the bitmap comes back to the main loop, dropped if the document,
  the page's size or the scale changed meanwhile. A page cairo cannot hold
  (past 32,767 pixels on a side, a 100 by 60,000 point page at fit width) is
  rendered narrower, whole, instead of failing; one poppler cannot render at
  all is marked failed and drawn grey with a cross, and not asked for again
  until the size changes. Before, both kinds were asked for on every idle
  pass forever, at 100% CPU.
  A page whose bitmap would pass 8 million device pixels (32 MB) is rendered
  whole at the resolution that fits, soft up close, and the part in view plus
  half a view around it is rendered again at full resolution on top (a
  "detail", at most 12 million pixels, or just the visible part for a view
  bigger than that), redone when the view moves past it. Pages either side
  keep bitmaps only while the total stays near 192 MB (at most three either
  side, as before), and the out-of-range ones are dropped before anything
  new is rendered. While a zoom or resize is moving, the old bitmaps are
  stretched; they are redone 120 ms after it stops. At fit width a Letter
  page stays under the cap up to about 1240 logical pixels wide at 2x, so
  the default view rarely needs a detail.
- **Pages are textures.** Each page is a small widget (`MinicodePdfPage`,
  in `PdfView.cpp`) whose snapshot is a white rectangle, the page texture
  and the detail texture, drawn with `gtk_snapshot_append_scaled_texture`.
  It used to be a `GtkDrawingArea`; see the trap below for why it is not.
- **Coordinates.** `pageAtPoint`, `pagePointIn` and the click callback go
  through `gtk_widget_compute_point` on the page widgets, as before, so they
  hold at any zoom; `anchor()` and `scrollTo` use each page's own pixel size.
  The zoom test checked all of them at six zoom levels against where the
  page's pixels actually are.

### 5. Find in folder (done, September 2026)

Mac: `src/Search.mm`. The search is now `../src/FolderSearch.{h,cpp}`, a pure
C++17 function (a folder, a query, an `std::atomic<bool>` cancel flag, and
matches out with the line, the column in characters and in bytes, and the
cleaned display text) with the Mac's rules, tested in the core suite. Android
can call it through JNI as it is. Since November 2026 it also takes whole
word and regex options and plans Replace in Folder, matching through
`../src/TextFind.cpp` (which is why that file is in both build lists); the
call this port makes is unchanged and still searches plain text, ignoring
case. The Mac's find bar options and Replace in Folder would build on the
same functions here, and since item 20 they do.

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
`../BUILD-LINUX.md`); the look of the window still needs a person.

That verification was wrong in one respect, found by the user: it checked
the caret and the selection after opening a match, not whether the line was
on screen, and it was not. GtkTextView scrolled towards where it estimated
the line to be, from heights guessed for the lines above it that it had not
laid out yet, animating there while the real heights came in, so a match on
line 250 of a new file left lines 166 to 208 on screen. `Editor::revealLine`
now calls `settleOnCaret`: a tick callback checks the caret every frame for
up to two seconds and scrolls again once the view has stopped with the
caret off screen, and ends when the caret has been on screen, the view
still, for three frames, or when the caret moves. A result now opens on one
click (a gesture like the tree's; the rows are not activatable, so a
double-click opens it once), and Enter on a result opens it. Down from the
query, and Enter there once the list answers the query, had never moved the
keyboard into the list (`GTK_LIST_SCROLL_FOCUS` does not take the focus from
another widget); `focusFirstResult` grabs it now.

Opening a file became asynchronous with item 1 (the "Save changes?"
question), and `openSearchMatch` goes to the line from `openFileThen`'s
continuation, after the open.

### 6. Language servers (done, September 2026)

Mac: `src/Lsp.mm` over `../src/LspClient.cpp`, described in the "LSP"
section of `../CLAUDE.md`. The GTK side is `src/Lsp.{h,cpp}` over the same
`LspClient` and `Json`, now in `meson.build` and the Makefile. It follows the
Mac's behaviour; what differs is how.

- **Processes.** `GSubprocessLauncher` with pipes MiniCode creates itself
  (`take_stdin_fd`/`take_stdout_fd`), so both ends are plain non-blocking
  descriptors watched with `g_unix_fd_add`: no gio-unix headers, and a server
  that is slow to read never blocks typing (unwritten bytes wait for
  `G_IO_OUT`). stderr goes to `/dev/null`, or with the traffic to
  `$MINICODE_LSP_LOG` (O_APPEND). Servers are found on PATH, then
  `~/.cargo/bin`, `~/go/bin`, `~/.local/bin`, `/usr/local/bin`, `/usr/bin`,
  and get that wider PATH. A child-setup `prctl(PR_SET_PDEATHSIG, SIGTERM)`
  and stdin EOF are the backstops if MiniCode dies. SIGPIPE gets a do-nothing
  handler rather than `SIG_IGN`, because an ignored signal is inherited by
  every program the app starts (the terminal's shell included) and a handler
  is not.
- **Stopping.** Closing the window (`onCloseRequest`, once the close is
  really going ahead) sends shutdown, exit on the reply, then SIGTERM after
  2 s and SIGKILL after 3, as on the Mac. The application's `shutdown`
  signal calls `LspTerminateAllServers`: exit at once, written out before
  returning, then a wait of up to 0.5 s on a pidfd per server (it turns
  readable when the process exits, without reaping it, so GLib's child
  watch is left alone), then SIGTERM, then SIGKILL. The main loop has
  stopped by then, which is why it does not wait on GLib.
- **Sync.** `Editor` has an `EditorObserver` (three calls:
  `documentChanged(path or "")`, `documentSaved`, `textEdited`). The edit
  call comes from the same `insert-text`/`delete-range` `_after` handlers
  the highlighter uses, only while the buffer holds source, so a refill never
  counts as typing. Everything that changes what the buffer holds ends in
  `notifyDocument()`: loading source, a message, a Markdown preview, an
  image or PDF (they show an empty message, so they get no server),
  `closeFile()` (Open Folder, trashing the open file) and `setPath()` (a
  rename: didClose, then didOpen under the new name). Full-document sync
  like the Mac: didChange 0.3 s after typing stops, flushed before every
  request, sending `Editor::text()` (the highlighter's UTF-8 mirror when it
  has one). Open Folder calls `LspSession::setRoot` after `closeFile`, so
  the old file is not reopened under the new root.
- **Positions.** Per line: a GTK line's text to UTF-16 (`Utf8Offsets.h`)
  gives LSP's column; a column back through `utf16::toCharOffset` gives
  GTK's. Both count lines at `\n`, `\r\n` and `\r`; only U+2029 differs.
- **Squiggles.** Four tags (`lsp-error`, `-warning`, `-information`,
  `-hint`, the Mac's colors) with `underline = PANGO_UNDERLINE_ERROR` and
  `underline-rgba`, created from hint up so an error wins where they
  overlap. The highlighter removes only the tags in its own `hlTags_`, so a
  retag leaves them alone (checked at run time). Each diagnostic also keeps
  two `GtkTextMark`s, for the tooltip lookup; the tags themselves follow
  edits because they are in the buffer.
- **Hover.** `query-tooltip` shows the diagnostic under the pointer at once
  and asks the server about the word; the answer is cached per word and
  `gtk_widget_trigger_tooltip_query` shows it. Ctrl+I shows the same in a
  popover at the caret, like the Mac's Command I.
- **Completion.** A `GtkPopover` holding a `GtkListBox`, parented to the
  text view, not autohide and not focusable, so typing stays in the view.
  A capture-phase key controller on the view takes Up, Down, Return, Tab
  and Escape while the list is up. Opens on `.`, `->`, `::` when the
  server lists them, and on Ctrl+Space; narrows as you type and closes when
  the caret leaves the word, the view loses focus or the text is selected.
  Accepting replaces from the textEdit's start (or the word start) to the
  caret, in one user action.
- **Definition.** F12 and Ctrl+click (a capture-phase click gesture that
  claims the press only with Ctrl held and a server running). In the same
  file it computes the byte column from the buffer and calls
  `Editor::revealLine`; in another file it maps the path into the tree
  root's spelling and goes through `openSearchMatch`, so the "Save
  changes?" prompt and cancelling it behave as for a Find in Folder match.
- **The trap paid for.** A popover added to a `GtkTextView` with
  `gtk_widget_set_parent` is unknown to the view's own child list, and
  GtkTextView's dispose tried to remove it, warned "GtkPopover is not a
  child of GtkTextView" and tried again, forever: the first test run's
  window never closed (24 million warnings in 90 s). The popovers are now
  unparented on the view's `unrealize`, which comes before dispose, and
  built again on next use.

Core changes that reach this port as they are (November 2026): with
`lsp.python` unset, basedpyright is tried before pyright; diagnostics a
server published for a version older than one since sent are dropped in
`Client`, before `onDiagnostics`; completion items now carry
`additionalTextEdits` (an auto-import, an `#include`), which the Mac applies
with the word through `Lsp::completionEdits` and this port does not apply
yet. Everything else the Mac's client gained (rename, references, code
actions, symbols, folding ranges, a second server per language, the
Problems rows) is in `LspClient` for this port to use.

Verified at run time on the ThinkPad with a temporary test hook, 32 checks
against real clangd 21 (see `../BUILD-LINUX.md`), then the hook was removed.
Still needs a person: how the squiggles, the list and the tooltips look,
and the real keys and mouse (Ctrl+Space, F12, Ctrl+I, Ctrl+click, clicking
a row), since the hook called the same functions directly.

### 7. The LaTeX preview (done, September 2026)

Mac: `src/Latex.mm`; read the whole "LaTeX preview" section of `../CLAUDE.md`
before starting. Android: `LatexPreview.kt` and `latex_jni.cpp`.

- tectonic comes from `MINICODE_TECTONIC`, then
  `~/.local/share/minicode/bin`, then PATH. Offer the same download of the
  pinned release as the Mac.
- Typeset from the hidden sibling `.<file name>.<preview>.minicode.tex`
  into `$XDG_RUNTIME_DIR` or a private folder in `/tmp`, debounced and
  generation-counted. The Mac's `.<stem>.minicode.tex` made notes.tex and
  notes.ltx in one folder, and one file in two windows, share a copy, so one
  run's cleanup deleted another's input; the full name and a per-preview
  number keep them apart.
- Show every page with `PdfView` (from item 4): `load(pdf, true)` after
  each typeset keeps the scroll position, and its click callback gives the
  page and point for SyncTeX (add one to the page).
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

**Done, September 2026.** Both documents typeset, and every word of both
was double-clicked in the running app with none opening the wrong text (the
counts are below and in `BUILD-LINUX.md`). The pieces:

- `src/PageWords.{h,cpp}`, pure C++ and tested in `tests/run_tests.cpp`: a
  page's text plus one box per character, the word under a point with 40
  characters of context each way, the hyphenation join (the Mac's
  `MCJoinHyphenation`), and `latexSpanAtPoint`, the whole click path over
  `SyncTexIndex` and `LatexDoc`. A word is a run of characters that
  `LatexDoc::matchKey` keeps, so the page and the matcher agree on what a
  letter is, with an apostrophe allowed inside.
- `src/LatexClick.{h,cpp}`: the poppler and GIO half, `pageTextOf` (from
  `poppler_page_get_text` and `poppler_page_get_text_layout`, which give a
  rectangle per character, newlines included, in the same top-left frame as
  SyncTeX) and `readGzipFile` (a `GZlibDecompressor`, so no zlib dependency).
- `src/Latex.{h,cpp}`, `LatexPreview`: the status line (spinner, message,
  Recompile or Download…), a `GtkStack` of the `PdfView` and the log, the
  edit popover, tectonic, the download, and export.
- `Editor` gains `isLatex()`, `latex()` and `exportPdf()`, and puts the
  preview in its slot stack as `"latex"`. `main.cpp` gains the Export PDF
  action (Ctrl+Shift+S, File menu, enabled only for LaTeX) and two lines in
  the hints panel.

How it differs from the Mac, on purpose:

- **The buffer keeps the source.** The Mac swaps the text view out and keeps
  its own copy of the source in the preview. Here the GtkTextBuffer holds the
  file's text, editable and highlighted, while the preview sits in front of
  it; the preview reads the buffer to typeset, and a popover commit is a
  splice into the buffer (only the bytes that differ, as one user action).
  So dirty tracking, "Save changes?", the incremental highlighter (which
  has a TeX grammar now, so a preview edit recolors the source like typing
  does) and undo all see preview edits as typing. Ctrl+Z and Ctrl+Shift+Z (or Ctrl+Y) in the preview are the
  buffer's undo, through a shortcut controller on the preview, and retypeset
  at once. `save()` reads the buffer for LaTeX even in preview.
- **A click is traced only against the PDF of the current buffer.** SyncTeX's
  lines belong to the source that was typeset; with the buffer ahead of it
  they could name the wrong text. So a double-click while the PDF is behind
  refuses ("catching up") and typesets now, and a commit checks the buffer
  still holds the source the span indexes before splicing.
- **A click on no text refuses.** The Mac hands an empty word to
  `spanForClick`, which then offers the nearest span; here a click in a
  margin or on a picture opens nothing. A glyph that is not a word (a
  bullet) still sends its line, as on the Mac.
- **tectonic**: `MINICODE_TECTONIC`, then `$XDG_DATA_HOME/minicode/bin`
  (`~/.local/share/minicode/bin`), then PATH. The download is 0.17.0, the
  Mac's version, as the static musl build for x86_64 or aarch64 (about
  10 MB; the glibc build wants libgraphite2), fetched with curl or else wget
  (GLib has no HTTPS client), and checked against the SHA-256 GitHub
  publishes for the asset, which is pinned in `Latex.cpp`. A mismatch is
  thrown away. Then `tar`, a move into place, and an executable check, as on
  the Mac.
- **Output** goes to `$XDG_RUNTIME_DIR/minicode-latex/<hash of the path>/`,
  so two `notes.tex` in different folders never share a PDF. Without a
  runtime folder it is `/tmp/minicode-latex-<user>`, but only if that is a
  real folder owned by the user and closed to everyone else (it is made 0700
  if missing); one someone else made first is passed over for a fresh
  `g_dir_make_tmp` folder.
- **Nothing outlives the app.** tectonic and the download tools are started
  with `PR_SET_PDEATHSIG`, so they die with MiniCode even when it is killed;
  on a normal quit the application's `shutdown` signal stops tectonic and
  removes the hidden sibling. The sibling is removed with `unlinkat` on a
  descriptor of the folder it was written in, opened when the run starts,
  so it goes even if that folder was renamed or moved to the trash while
  tectonic ran (by path, it was left behind in the renamed folder). Opening another file, or Open Folder
  (`closeFile`), closes the preview and kills its typeset. A rename
  (`Editor::setPath`) keeps the pages, and the SyncTeX tag is looked up by
  the sibling that was actually typeset, so clicks keep working before the
  next typeset.

`tests/latex/sweep-linux.sh doc.tex [-v]` is the Linux `sweep.sh`: it
typesets the way the app does and runs `build/minicode-latex-sweep`
(`tests/latex_sweep.cpp`, built when poppler is found), which clicks the
first, middle and last character of every word through `latexSpanAtPoint`
with the Mac harness's verdicts, and exits non-zero on any wrong click. On
2026-09-23: torture.tex 1,091 clicks, 1,080 found, 3 found but not placed,
8 refused (text from `\newcommand` bodies, refused on purpose), 0 wrong;
notes.tex 185 of 185 found. The in-app run through the real gesture and
popover gave the same numbers. poppler has no word breaker, so the harness
enumerates words with `PageText::words()`, the click's own segmentation,
where the Mac uses NSString's; the verdicts still judge the span against the
page's text.

The sweep also found that a single letter or digit with no agreeing context
fell through to `spanForClick`'s last resort, the nearest span, so a page
number opened the last paragraph near it and a section number its heading.
The shared matcher now refuses every single character before matching, on
every port, since a single character cannot say which span it came from and
the word beside it opens the same span. A list item's number or label, a
one-letter word and a one-letter math variable no longer open anything; the
user accepted that. Both harnesses count single characters apart from the
verdicts above and report how many still offered a span: after the change,
0 of 49 on torture.tex and 0 of 5 on notes.tex (before, all 54 did), with
the other counts unchanged.

Zoom came later, with the PDF view's (item 4); a retypeset keeps it, and a
double-click opens the right text at every zoom tried. Not done:
click-to-line in the source view, and a person
using it: the real double-click, the popover's look and placement, Return and
Escape in it, Ctrl+Z in the preview, and the Export PDF dialog were not
driven by real input (see `BUILD-LINUX.md`).

### 8. The smaller gaps

- New Window, Previous File, Find Next and Find Previous. Ctrl+Shift+N is
  already New Folder on Linux, so give New Window a key that doesn't clash.
- Show Hidden Files exists as Ctrl+H; check it against the Mac's behaviour.
- Blur and live color picking, as `../ROADMAP.md` describes ("Linux port
  parity").
- Done: the core tests run on Linux in CI. The top-level Makefile picks
  clang++ only on the Mac, so `make test` builds with g++ here, cleanly, and
  the `linux` job in `.github/workflows/ci.yml` runs it.

**Done, September 2026**, with four things found in review. How each was
checked is in `../BUILD-LINUX.md` (81 checks in the running app, all
passing, plus the criticals run); what needs a person is listed there too.

- **Windows.** `App` in `main.cpp` is now one window, and `g` holds what they
  share: the settings file and stylesheet, the menu bar (application-wide,
  with a Navigate menu like the Mac's), the accelerators, the tree's
  context menu and the dotfile setting. Ctrl+N opens a window on the same
  folder (Ctrl+Shift+N stays New Folder; Ctrl+N was free and is the Mac's
  Command N). Ctrl+W closes one, Ctrl+Q all, each asking "Save changes?",
  and GtkApplication quits with the last window. A closing window is taken
  apart in `onWindowRemoved`, from GtkApplication's `window-removed`, which
  comes before GTK disposes a single widget: `disconnectOwners` takes every
  handler that names the window's objects off every widget and controller
  in it, then the search panel, language servers, terminal, browser, tree
  and editor are deleted, each with a destructor that removes its own
  sources and handlers on non-widgets (FileTree drops its models, whose
  filter and child functions call back into it; Terminal cancels a shell
  still starting; SearchPanel drops a search still running through a life
  token). The `App` struct itself is kept, marked `dead`, because an alert
  or file dialog may still answer later and must find out.
- **The command line.** The app id and `HANDLES_COMMAND_LINE` are as they
  were, so a second `minicode <path>` still hands off to the running
  process. It now opens a new window on that folder (what the Mac's
  command-line launcher gives, since it starts a separate process), or
  raises the window already on it and opens the named file there. Before,
  it re-rooted the only window.
- **Previous File, Find Next, Find Previous, Focus Editor.** Ctrl+Tab goes
  to the file before this one (a list per window, newest first, as the Mac's
  `_recent`; renames and trashing keep it right). Ctrl+G and Ctrl+Shift+G
  step through the find bar's matches from the selection, wrapping, with the
  bar open or closed; with no query they open the bar. Shift+Enter in the
  bar goes back. Ctrl+1 focuses the editor, and Ctrl+` toggles the terminal,
  as on the Mac.
- **Show Hidden Files.** The Mac's is one global flag for every window,
  under View as "Show Hidden Files", and hides names starting with a dot.
  Linux had one per tree and a menu item that did not show its state. It is
  now an application action with a check mark (`app.showhidden`, Ctrl+H):
  every window's tree follows it, a new window starts with it, and expanded
  folders stay expanded. The key stays Ctrl+H, the GTK file chooser's and
  Nautilus's, rather than the Mac's Shift+Command+.; Ctrl+. is GTK's emoji
  key in text views. Later (at the user's request, since Toshy's Mac-style
  remapping never lets Ctrl+H through) Ctrl+Shift+. was added as a second
  key, bound as `<Ctrl>greater` (see "Traps"). The claim that expanded
  folders stay expanded was not true in general: with a dotfile sorting into
  the root, refiltering closed every folder and dropped the selection. It
  holds now, because `setShowHidden` reopens them and reselects the row.
- **The terminal's keys** (found in review). Application accelerators run in
  the window's capture phase, before the focused widget, so Ctrl+B, Ctrl+F,
  Ctrl+H, Ctrl+W and the rest never reached bash or vim; a controller on the
  terminal could not win, since it runs after the window's. On the Mac the
  question does not arise: shortcuts are on Command, and Control goes to
  the terminal, in the log and the grid alike. So while the active window's
  terminal has the focus, `updateShellKeys` unbinds every accelerator that
  is a plain Ctrl key or a bare function key (`shellOwns` decides from the
  accelerator string, so a new one is covered without a list), and binds
  them again when the focus leaves. Anything with Shift or Alt stays, and so
  do the pane keys Ctrl+`, Ctrl+0, Ctrl+1 and Ctrl+Tab. Ctrl+Shift+C and
  Ctrl+Shift+V copy and paste in the terminal. The hints panel has an "In
  the terminal" section.
- **Reload on external change** (found in review). The Mac's
  `checkExternalChange`, in `Editor`: the open text file is watched with a
  `GFileMonitor`, and checked again when the window becomes active, as the
  Mac checks on becoming key. Contents are compared, not dates, with what
  was last loaded or saved, so MiniCode's own atomic saves are no change. A
  clean buffer takes the new text in place: only the bytes between the
  common start and end are replaced, as one user action, so highlighting
  and the language server see an edit, undo can take it back, and the caret
  and scroll stay (a caret inside the replaced part keeps its line and
  column). A Markdown preview re-renders. With unsaved edits the Mac's
  question comes up, Keep Mine or Reload, and that disk version is not
  asked about again. MediaView still reloads images and PDFs.
- **Live color picking.** A click on a swatch opens a popover on the text
  view holding `GtkColorChooserWidget`, on its editor, at the swatch's
  color. Each change rewrites the line at once and the file is saved 150 ms
  after the last, as on the Mac, so the window follows a drag; closing the
  popover saves at once. The widget is deprecated since GTK 4.10 with no
  embeddable replacement (GtkColorDialog is a separate window and answers
  once), so its calls silence the warning, as MediaView does for GIFs. The
  popover is unparented on the view's `unrealize`, the LSP popovers' trap.
  Each pick is its own undo step, which a drag makes many of.
- **Blur: not possible on GNOME.** GTK 4 has no API for it, and Mutter 50.1
  on the ThinkPad offers no Wayland protocol for it: a small client listing
  the compositor's globals found neither KDE's blur protocol nor
  `ext-background-effect-v1` from wayland-protocols' staging set. KDE
  Plasma's compositor has its own blur protocol, so a Plasma-only blur
  through GDK's Wayland surface looks possible; it was not tried.
  `window.blur` stays ignored, and the settings file says so.
- **The GTK criticals** (found in review). Reproduced every time by
  scrolling the editor with an animation (a Find in Folder match, a go to
  definition, a find step) and switching the slot to an image, a PDF, the
  LaTeX preview or the browser within about 100 ms. GtkAdjustment ends an
  animation by jumping to its target first; the resulting value-changed
  lets the text view end the animation already, and the adjustment then
  disconnects its frame-clock handler and releases the clock a second
  time. `ScrollSettle.h` ends the animation from the text view's unmap,
  which comes before the scrolled window's check. 20 pairs of criticals in
  60 switches before, none in two runs after.

What the Mac has that Linux still lacks: Refresh File Tree (not needed, the
tree is live), zoom for images (missing on the Mac too; PDFs and the LaTeX
preview have zoom now, item 4), and blur. Terminal links came after this
list was written, as item 9.

Not needed on Linux: the demo recorder and the memory benchmark, which are
Mac tools.

### 9. Terminal links (done, September 2026)

Ctrl+click on a file reference or a URL in the terminal's output opens it,
as Command+click does on the Mac (`src/Terminal.mm`, `TerminalGridView.mm`,
`EditorController.mm -openTerminalLink:`). A file opens at the line and
column the reference names, a folder is selected in the tree (the sidebar
is shown first; a folder outside the project only beeps, as on the Mac), and
a URL opens in the browser panel, or with `GtkUriLauncher` in a build
without WebKitGTK. Holding Ctrl over a link underlines it and shows the
pointing hand.

- **One source of truth.** The shared `TermLinks` core finds the links in a
  line; nothing on the GTK side knows a pattern. VTE's own regex matching
  (`vte_terminal_match_add_regex`) was not used, because it would have meant
  a second copy of those patterns as PCRE2, and a regex cannot know which
  paths exist.
- **From a click to a line.** `Terminal::findLink` turns the point into a
  cell (VTE's CSS padding, read through the deprecated style context, then
  whole cells), reads the cell's whole logical line with
  `vte_terminal_get_text_range_format` (rows that wrapped because they were
  full come back joined, a real line end is a newline, so a reference
  wrapped across rows is found from either row), and takes the byte offset
  of the cell as the length of the text before it. The underline maps the
  link's bytes back to cells, two for a wide character.
- **Which row is at the top.** VTE's vertical adjustment counts rows from the
  oldest one it still keeps, while the text calls number them from the start
  of the buffer. The two agree until the scrollback is dropped, which
  `clear` does (ESC[3J) and so does passing the 10,000 kept lines; after a
  `clear` a click landed five rows off. VTE has no call for the offset, so
  `Terminal::topRow` finds it by comparing the screen's text with the
  buffer's rows at each place the screen could start (the cursor is on the
  last screenful), and keeps it until the contents change. VTE's "visible"
  text includes one row past the screen, so both readings are compared.
- **Resolving a path.** `linux/src/TermLinkPath.cpp`, plain C++ with its own
  tests in `linux/tests/run_tests.cpp`: `~` expanded, an absolute path as
  is, a relative one against the shell's directory and then the project
  folder, symlinks resolved, and a path inside the project put back in the
  root's own spelling so the tree can select it. The shell's directory is
  VTE's OSC 7 termprop (Ubuntu's `/etc/bash.bashrc` sources
  `/etc/profile.d/vte-2.91.sh`, which prints it at every prompt), then
  `/proc/<shell pid>/cwd` for a shell without that hook, then the folder the
  shell started in. An OSC 7 from another host (ssh) is ignored. No cache:
  a `stat` per candidate is cheap, and only happens while Ctrl is down.
- **Input.** A `GtkGestureClick` on the terminal in the capture phase,
  claimed only when Ctrl is held over a link, so VTE never starts a
  selection or passes that click to a program that tracks the mouse, and any
  other click is left alone. Item 8's unbinding of plain Ctrl shortcuts is
  about keys, so it does not get in the way. The hover follows a motion
  controller, and a capture-phase key controller on the window catches Ctrl
  pressed or released without moving the mouse, whichever widget has the
  keyboard. The underline is a `GtkDrawingArea` over the terminal in a
  `GtkOverlay` (not targetable), since VTE underlines only its own regex
  matches. VTE sets its own pointer as the mouse moves, so the hand is put
  back from an idle callback after each event.
- **Opening.** `openTerminalLink` in `main.cpp` goes through `openFileThen`,
  so "Save changes?" is asked first, then `Editor::revealLineColumn`, which
  counts the column in characters like the Mac's `goToLine:column:` and
  switches a Markdown or LaTeX preview to the source first. The browser is
  put away when a file opens, so the file is actually seen.
- **Also fixed, in the shared core:** a URL ending in a balanced parenthesis,
  such as a Wikipedia link, lost its closing parenthesis unless another,
  unmatched one followed it. `make test` has the cases.

Checked with a temporary hook (51 checks); `../BUILD-LINUX.md` has what they
covered and what still needs a person. Not done: OSC 8 hyperlinks that
programs print themselves (`ls --hyperlink`, GCC's diagnostic URLs). VTE
already parses them (`vte_terminal_set_allow_hyperlink` is on) and
`vte_terminal_check_hyperlink_at` would give the URI, but the Mac does not
have them either.

### 10. The Source Control panel (done, September 2026)

Mac: `src/GitPanel.mm` and "Git panel" in `../CLAUDE.md`. GTK:
`src/GitPanel.{h,cpp}`. Both, and Android, over the core's
`../src/GitModel.{h,cpp}` (it lived here until 2026-11-06), which is plain
C++ and holds everything that can be decided without a GUI (rows, keys, the
selection after a refresh, argument vectors, the summary line, error text,
diff and commit text with styles in character offsets, graph tooltips). It
is tested in the core's `tests/gitmodel_tests.inc`.

- **Toggling.** The sidebar is a `GtkStack` (`App::sidebar` in `main.cpp`)
  holding the tree and, from the first Ctrl+Shift+G, the panel, so the
  divider and the width stay put. A hidden sidebar opens for it, Ctrl+0
  focuses it while it is shown, and a folder link from the terminal puts
  the tree back. Find Previous moved to Shift+F3 (and F3 joined Ctrl+G for
  Find Next): Ctrl+Shift+G is the panel, as in VS Code on Linux. While the
  terminal has the keyboard, `shellOwns` gives Ctrl+Shift+G to the shell.
- **Running git.** `GSubprocess` with an argument vector, run synchronously
  on a `GThreadPool` of one thread per panel, so commands keep their order
  (an add, then the status after it). Results come back through
  `g_idle_add` and are dropped when the panel is gone (a shared flag), the
  folder changed, or a newer diff or graph was asked for (generation
  counts). The environment and every argument vector are the Mac's.
  So are the time limits (2026-11-05; `Git::timeoutFor` in the core, one
  table for both): 30 s for reads, 2 minutes for add, restore and rm, 10
  for commit. git leads its own process group (`setpgid` in the
  launcher's child setup); at the limit the group gets SIGTERM and SIGKILL
  2 s later, and the panel shows "git did not answer within 30 seconds:
  git status ...". `runGitWithin` waits through `g_subprocess_wait_async`
  and `communicate_async` on a main context of the git thread's own, so a
  git that exits while something it started holds its pipes is caught too
  ("git exited, but something it started kept its output open ..."). A
  timed-out for-each-ref skips the log, as on the Mac. The one thread
  stays: order matters, and a hung git now holds it only that long.
  Checked on the Mac by building the running-git part against Homebrew's
  GLib with fake gits (a hang, one ignoring SIGTERM, one holding its
  pipes, 3 MB of output, a failure, a signal) and real git; the GTK file
  itself has not been compiled since.
- **Refreshing.** 50 ms debounce, only while the panel is shown. Triggers:
  showing it, every action, the window becoming active, a save, the file
  tree's folder monitors (`FileTree::setActivityCallback`, which reports
  content changes too), and the panel's own monitors on the top level,
  `.git`, `.git/refs/heads` and `.git/refs/tags`. A change in a folder the
  tree never opened is only seen on the next focus or save; GIO has no
  recursive monitor and watching every folder of a large repository costs
  inotify watches. Nothing the panel runs writes to `.git`
  (`GIT_OPTIONAL_LOCKS=0`), so a refresh cannot set off another.
- **Drawing.** The two lists are `GtkListView`s over a `GtkStringList` that
  only holds a count; each row is a `GtkDrawingArea` drawn with cairo and
  Pango from the panel's vectors by position, the way the Mac's table cells
  are drawn by hand. Up, Down, Space, Enter and Tab are a capture-phase key
  controller on each list; headings are neither selectable nor focusable.
  The panel's children are placed by a `GtkCustomLayout` that is the Mac's
  `layoutPanel`: the change lists take their rows' height up to about half,
  the graph the rest.
- **The diff** is a `GitDiffView` (read-only `GtkTextView` with the
  editor's CSS class, so its colors follow the settings) that `Editor` adds
  to its slot stack on first use; `Editor::showDiff` closes the file first,
  after main.cpp's "Save changes?", and any file opening puts the editor
  back.
- **Debug log.** `G_MESSAGES_DEBUG=minicode-git` prints each refresh's
  branch line, summary, rows, graph rows and every action. The Docker
  checks read state from it.

Checked in the Docker container with real X input; `../BUILD-LINUX.md` has
what was covered and what still needs the ThinkPad.

**Not followed yet (2026-11-07): Stage All, Discard, the file menu and the
branch picker.** The Mac's panel gained them (round 4, W19); the rules are
all in the core for this port to use: `GitUi::stageAllPlan` and
`unstageAllPlan` (Stage All asks first when a file has a conflict),
`discardPlan` (always asks; the file goes to the trash first, then `restore
--worktree` from the index; refused for staged rows, conflicts and `git add
-N`), `changesRowFor` (Discard from a menu acts on the open file),
`branchListArgs`, `branchItems`, `switchArgs`, `createBranchLabel`,
`newBranchName`, `branchNameProblem` and their texts, tested in
`git-ui:stage-all`, `git-ui:discard` and `git-ui:switch`. GTK would use
`g_file_trash` (which may fail on volumes without a trash: then refuse, as
the Mac does), a `GtkPopoverMenu` on a row's right click, and a filtered
list for the branches (the palette, item 19, has no pick lists of a
caller's rows yet; the Mac's branch picker is one). The Mac's Undo Discard
moves the trashed copy back; keep that, since a discard without it is the
one panel action that loses work.

**Not followed yet (2026-11-07): hunks and lines.** The Mac's diff stages,
unstages and discards a hunk, or the changed lines selected (round 4, W26).
The core does the work: `GitUi::diffTextWithActions` (the action words on
each hunk header line, with their character offsets, for GtkTextTag links),
`diffHunks` and `hunkAt` (lines are the diff text's lines), `hunkPlan` (the
patch, the `apply` arguments and the texts; Discard asks) and
`hunkFailureText`, tested in `git-ui:hunk-patch` and `git-ui:line-patch`.
The patch goes to git's stdin, so `runGitWithin` needs an input; keep a
copy of the file in the trash before a discard, as the Mac does.

### 11. Video and audio (done, September 2026)

Mac: AVKit, done in parallel by another session. GTK: a third kind in
`src/MediaView.{h,cpp}` beside images and PDFs, over `GtkMediaFile`, which
is GTK's GStreamer backend. No new build dependency.

- **Routing.** Video: mp4, m4v, mov, webm, mkv, ogv, avi. Audio: mp3, wav,
  m4a, aac, flac, ogg, opus. `MediaView::handles` covers all four kinds and
  `Editor::openFile` routes by it before reading anything as text. The page
  then follows what the file holds, not its name: once prepared, an .ogg
  with Theora in it moves to the video page and an .mp4 of sound alone to
  the audio page.
- **Pages.** Video is a `GtkVideo` (autoplay off, so it opens paused on its
  first frame, with its own overlay controls). Audio is the file's name
  over a `GtkMediaControls` that is always shown, since a `GtkVideo` of
  audio is an empty pane whose controls hide until the pointer moves. The
  controls carry the `osd` class, as GtkVideo's do. A failure is a
  selectable label in the player's place, with GStreamer's whole message as
  its tooltip. Each page sits in a scrolled window that keeps its minimum
  size to itself, and the media stack is not homogeneous (see Traps).
- **Title.** `  640 × 360, 0:06` for video, `  0:06` for audio, once the
  stream is prepared and the first frame has given the size
  (`invalidate-size`); nothing after a failure.
- **Stopping.** `releaseStream` pauses, takes the stream off the video and
  the controls, calls `gtk_media_file_clear` and drops the last reference,
  which ends GStreamer's pipeline. `clear()` calls it, and `clear()` runs on
  every other file, a diff, closing the file (a new folder, Move to Trash)
  and the window going (`~MediaView`). Nothing plays in the background.
- **Reloading.** The existing `GFileMonitor` path: the new stream is told
  the old one's timestamp and play state, and seeks and plays once
  prepared. A reload that arrives before the previous one was prepared
  passes the same place on.
- **Failures.** GTK built without a media backend returns a
  `GtkNoMediaFile`, recognised by type name, and the note names GTK's
  GStreamer support (in `libgtk-4-1` on Ubuntu 26.04,
  `libgtk-4-media-gstreamer` before). A missing decoder is recognised from
  the error text (see Traps) and the note names `gstreamer1.0-libav` and
  `gstreamer1.0-plugins-good` and says to restart, since GStreamer reads its
  plugin list once. Anything else shows GStreamer's plain message, without
  its debug detail.
- **Play/Pause** is `win.playpause` on Ctrl+Shift+Space, in the View menu.
  `updateMediaKey` in main.cpp enables it only while a stream that can play
  is shown and the keyboard is not in the terminal or any editable text
  (the commit message, the find bar); disabled, the key goes to the focused
  widget. `shellOwns` also gives it to the shell, which reads it as
  Ctrl+Space. It runs from `updateTitle` (the media view calls the title
  callback when a stream is prepared, sized or failed) and on every focus
  change. At the end of a file it starts over. The Mac session's key was
  not known when this was chosen; align them if they differ.
- **Debug log.** `G_MESSAGES_DEBUG=minicode-media` prints each stream's
  kind, size, length and restored place, and every error in full.

Checked in the Docker container with real X clicks and keys, on stock
plugins and with libav; `../BUILD-LINUX.md` has the list. Still needs the
ThinkPad: real sound through PipeWire, GNOME on Wayland with a real GPU,
HiDPI, and whether the GL thread leak below happens there.

### 12. Pictures from the web in the Markdown preview (done, September 2026)

The Mac has them too (`MarkdownImage.mm`). Here the rules and limits are
plain C++ in `src/WebImageRules.{h,cpp}` (tested in `tests/run_tests.cpp`)
and the fetching is `src/WebImages.{h,cpp}`, over libsoup 3.

- **Why libsoup.** WebKitGTK 6 is built on it (`webkitgtk-6.0.pc` requires
  `libsoup-3.0`, and `libwebkitgtk-6.0-dev` pulls in `libsoup-3.0-dev`),
  so wherever the browser panel builds, this costs no new package. It is
  its own meson option, `webimages` (auto), and `make WEBIMAGES=1`, so a
  build without WebKit can still have it. Without it, web pictures show
  their alt text as before. GIO's `GFile` on an https URI would need gvfs,
  which is not always there.
- **Rules.** Only `https://` with a host is fetched; a redirect is followed
  only to another https address, at most 5. The session has no cookie jar
  and no cache, and sends `User-Agent: MiniCode/<version>` (the version is
  read from `../Info.plist` at configure time). 15 s without progress or
  60 s in all ends a fetch. A Content-Length over 20 MB is not read, and a
  body is cancelled once it passes 20 MB. The header's claimed size is read
  before decoding, and more than 64 megapixels is refused. Decoded pictures
  are kept in a 64 MB least-recently-used store; a failed address is not
  asked for again for a minute. Nothing reaches the disk.
- **Placing.** The render shows the alt text for every web picture not in
  memory and lists it in `Page::pending`. The editor fetches each address
  once, collects arrivals and places them 100 ms after the first, each in
  place of its alt text in the buffer as it stands, not by a new render
  (`Markdown::placeWebPicture`). A result for a page that is gone finds
  nothing pending and is dropped. `markdown.web-images = false` renders
  alt text only; turning it back on re-renders and fetches.
- **Keeping the place.** GtkTextView keeps the line at the top of the view
  still when lines above it grow, but with two pictures laid out in one
  pass it moved for the second before the adjustment's upper bound had
  grown, and near the end of a page the move was cut short. `holdTopLine`
  puts the top line back for a few frames after placing, and a scroll by
  the user ends it at once.
- **SVG.** Decoded by glycin's SVG loader where GTK uses glycin (Ubuntu
  26.04), so shields.io badges show.
- **Debug log.** `G_MESSAGES_DEBUG=minicode-web` logs every request,
  answer, failure, the cache and each placing.

Checked in the Docker container (2026-09-29) against a local https server
whose certificate only the container trusted: a png, an animated gif, a
badge and an SVG arrived; a 404, bytes that do not decode, 25 MB with and
without a length (cancelled at 20 MB), plain http and a redirect to http
kept their alt text, and the http port saw no request; a redirect to https
arrived; two 300-pixel pictures arriving above a scrolled page left the
same paragraph at the top; the setting off made no request, and turning
it on fetched. Still needs the ThinkPad: real sites (GitHub, shields.io)
and scrolling by hand while pictures arrive.

### 13. Drag and drop in the file tree (done, October 2026)

The user had to use the terminal to move a new file into a folder. Rows
now drag, as in the Mac's outline, which gained the same at the same time.
`FileTree` decides where a drop goes and whether it may; `main.cpp`
(`dropOnTree`) does the work.

- **Where.** Onto a folder row: into it. Onto a file row: into that file's
  folder. Onto the space below the rows: into the root. The folder's row is
  lit (`minicode-drop-target`, accent #4EA1F7, in `ThemeCss.cpp`), or the
  whole tree is outlined for the root. A closed folder held under a drag
  for 0.7 s opens, and the list scrolls while the pointer is within 24
  pixels of its top or bottom.
- **Refused** while hovering (nothing lit, no drop): a folder into itself
  or anything below it, and anything into the folder it is already in.
- **Move or copy.** What a drag carries is a `GdkFileList`. A drag that
  began in this process (a tree in any window) moves, with `g_file_move`
  and no OVERWRITE, then `followMove`, the code Rename already used, so the
  open file and Previous File follow in every window, a file inside a moved
  folder too. Files from another program (a file manager) are copied, on a
  worker thread (`CopyJob`, `copyTree`: folders recursively, links as
  links), never over anything. A name already taken gives "“x” already
  exists in “folder”." and nothing is replaced. The folder monitors bring
  the rows in, and `revealPath` selects what arrived.
- **Clicks.** A click already acted on release, and only when press and
  release were on one row; the drag starts past GTK's drag threshold, after
  the click has given up, so a drag never opens its row.
- **Trap.** GtkTreeExpander has a drop controller of its own that opens a
  folder after 0.5 s under any drag, the folder being dragged included.
  `onSetup` removes it, so only `hoverFolder`, which knows what is
  refused, opens folders.

Checked in the Docker container (2026-10-01) with real xdotool drags: a
file onto a folder, a file onto a file row (into its folder), a folder
into a sibling, a file onto the empty space (root), the refusals (a folder
onto itself, into its child, a folder into its own parent) leaving the
disk untouched, a name collision showing the alert and leaving both files,
the open file following its own move and its folder's move (then Ctrl+S
writing to the new place and nothing at the old one), hover-open (and no
opening over a refused folder), autoscroll down a 70-row list, and single
clicks still opening files and toggling folders, a click that wobbles 3
pixels included. Files from outside came from a small GTK program run as a
separate X client (a real XDND drag, so the copy path and text/uri-list
conversion ran): a file and a folder copied with the originals left, and a
second drop of both reporting "2 items already exist". Screenshots of the
row highlight and the root outline were looked at. Not checked: a real file
manager (Nautilus) on the ThinkPad, a drag between two MiniCode windows,
dragging a row out to a file manager, and the look under GNOME's theme.
Dropping a row on the editor inserts its path as text, which is
GtkTextView's own handling of a file list.

### 14. Task lists and the TODOs window (done, October 2026)

Markdown task lists ("- [ ] buy milk", "- [x] done"), the TODO format
GitHub, Obsidian and VS Code read, built on the core's `MarkdownTasks` and
`FolderSearch::findTodos` (the Mac and Android do the same over the same
core). Nothing here decides what a task is: that is `MarkdownParser`, so a
"- [ ]" in a code block is never one.

- **The box in the preview.** `Markdown::render` puts a `GtkCheckButton`
  (`taskBox`, class `minicode-md-task`) in a child anchor where the parser
  wrote ☐ or ☑, with the indent before it and the space after it still
  text. It never takes the keyboard. A capture-phase click gesture claims
  the press, so the button never flips itself and the text view starts no
  selection, and calls `Hooks::taskToggle` from an idle. The editor's
  `toggleTaskBox` flips that one byte (`MarkdownTasks::toggleBox`) and
  hands the new source to `applyMarkdownSource`, the path the double-click
  popover's edits take: dirty until Ctrl+S, Ctrl+Z and Ctrl+Shift+Z in the
  preview, rendered again at the same scroll. The box is an embed, so the
  editor's own click handler leaves it alone and a double-click elsewhere
  still opens the popover. Styled in `ThemeCss.cpp`: an outline in the
  `markdown.done` color, filled with the accent #4EA1F7 and a white tick
  when checked.
- **Done items** get the `md_done` tag on their own text runs
  (`MdRun::task == 2`, not the marker): the `markdown.done` color, struck
  through, recolored by `applySettings`. It is made before `md_code` and
  `md_link`, so a code span or link in a done item keeps its color.
- **Ctrl+L** (Edit > Toggle Task) in a Markdown file's source runs
  `MarkdownTasks::toggle` on the lines the selection touches, as one user
  action, and moves the selection as it says. Anywhere else, the preview
  included, it rings the bell. In the terminal it is the shell's (clear
  screen): `shellOwns` takes every plain Ctrl key off while the terminal
  has the keyboard, so nothing special was needed.
- **Return** in a Markdown source asks `MarkdownTasks::newline` first, from
  `onViewKey`, and takes the key only when that changed something (the next
  item, "4." after "3.", or ending the list on an empty item); otherwise the
  key goes on to the usual keep-the-indent Return. It is never taken while a
  completion list is open: `EditorObserver::takesReturn`, which the LSP
  session answers with `completionVisible()`. Shift+Return is a plain new
  line, for a second line in the same item.
- **The title** says "MiniCode — TODO.md * — 3 of 7 done" while a Markdown
  file with at least one task is open. The counts live in the editor
  (`tasksDone`, `tasksTotal`), recounted 0.3 s after typing stops, at once
  after a toggle, a preview edit, an undo of one, a reload from disk and a
  rename, and the title callback runs only when they change.
- **TODOs** (Ctrl+Shift+L, Edit > TODOs…) is the Find in Folder window made
  with `SearchPanel::Mode::Todos`: titled "TODOs", scoped the same way (the
  folder selected in the tree, else the open one), `findTodos` on the same
  worker thread, scanned again every time it is shown (a hidden window does
  not scan when the scope changes). The field filters what was found, by
  "path:line  text" in any case, without scanning; the status line says
  "9 TODOs in 4 files, 2 shown". A row opens like a match: a Markdown file
  switches to its source with the box selected.
- **Trap: take a page's widgets off before replacing its text.** Clicking a
  box re-renders the page while the pointer is on the box and its press is
  still in progress. `gtk_text_buffer_set_text` then unmaps the box from
  inside the deletion, the view's state flags change, and GtkTextView's
  handler reads the selection from a half-deleted buffer: a segfault in
  `gtk_text_buffer_get_selection_bounds` (GTK 4.22). It happened every
  time once the edit popover had been open, and not before, which is why
  it took a backtrace (gdb in the container) to find. `Editor::clearPage`
  now removes every embed with `gtk_text_view_remove` before any render or
  refill, tables and pictures included.

Checked in the Docker container (2026-10-07) with xdotool: clicking boxes
(top level, nested, ordered, in a quote) changed the title to "* — 3 of 7
done", and Ctrl+S then differed from the original by one byte (`cmp -l`);
Ctrl+Z and Ctrl+Shift+Z in the preview; a double-click on an item's text
still opening the popover, and boxes clicked after it (the crash above,
found this way, then fixed and clicked through again under gdb); Ctrl+L
on a plain line, again to check it, on a task, on a heading (no change),
on two selected tasks (both cleared) and one Ctrl+Z restoring both;
Return in "- [ ] a" giving "- [ ] ", in "3. x" giving "4. ", in an empty
"- [ ] " ending the list (one Ctrl+Z bringing it back), Shift+Return a
plain line; Ctrl+L in the terminal clearing the screen and leaving the file
alone; the TODOs window on a folder with TODO and FIXME comments, Markdown
tasks and a task in a code fence (not listed), filtering, Down and Return
opening `main.c` with TODO selected, a click opening a Markdown row in its
source, a new file's FIXME appearing on the next Ctrl+Shift+L; and
`markdown.done` at startup and changed while running, read back from the
pixels. Screenshots of the boxes and the done items were looked at. Not
checked: Return with a completion list open (no Markdown language server
in the container; the code path is `takesReturn`), the look under GNOME's
theme and at 2x, and the box's height against the text on the ThinkPad
(it sits a pixel or two above the text's middle here).

### 15. Tabs (done, November 2026; `PARITY.md` item 1.1)

What later items build on:
- **`Document`** (`src/Document.{h,cpp}`) is one open file: its own
  `GtkTextBuffer` over the editor's one tag table, GTK's undo, the
  highlighter and its mirror, `dirty()` (the text differs from the text last
  read or saved), its file watch, a page buffer for the Markdown preview
  (`page()`), its LaTeX preview, and `place()`, where its view was. State
  that belongs to a file (folds, carets, change bars, backups) goes here;
  state of the view stays in `Editor`. `kind()` is Text, Message (a file
  that cannot show as text), Media (pictures, PDFs, video) or Diff.
- **`Editor`** keeps the documents in tab order (`src/Tabs.cpp`):
  `openFile(path, EditorTabs::OpenFrom)`, `active()`, `document(i)`,
  `documentFor(path)`, `activate`, `closeTab` (asks nothing; main.cpp's
  `closeTabAsking` asks), `reopenClosedTab`, `dirtyDocuments`. Switching is
  `stash()` then `showActive()`. `buffer()` is what the view shows (the page
  in a preview) and `sourceBuffer()` the active document's source;
  `onActiveChanged` fires on every switch (main.cpp reveals the file in the
  tree there), and the observer's `documentChanged` too, where the language
  server session re-points to the new buffer (`LspSession::useBuffer`). The tab rules are the core's
  `EditorTabs`, with `ClosedTabs` for Reopen.
- **The harness**: `MCGuiApp().editor->active()` is the document on show.
  `MCGuiOpenFile` opens as a single click in the tree does, in the preview
  tab, which the next one replaces; `MCGuiOpenFileKept` opens a kept tab.
  `MCGuiClickWith("ctrl", w)` clicks with modifiers.
- **Traps**: GTK starts its double-click timer again from a double-click,
  so a second double-click within the double-click time counts as one
  click; the tabs suite pauses 0.5 s between them. A place comes back by
  setting the adjustment once the view is mapped and still
  (`settleOnPlace`); `gtk_text_view_scroll_to_mark` lands short on a fresh
  layout and its pending scroll can undo the user's next move, and a tab
  left while its place is still coming back keeps that place, not the
  half-done scroll. The strip's scroller has an external vertical policy:
  any minimum height there made GTK warn about measuring the editor's box.
  A key-driven switch can print GTK's "Trying to snapshot GtkGizmo trough
  without a current allocation" (a scroll bar mid-relayout); it was not
  reproduced outside the app and is harmless.

### 16. Line numbers, the gutter and the drawing seam (done, November 2026; `PARITY.md` item 1.4)

What later items build on:
- **The view is a `CodeView`** (`src/CodeView.{h,cpp}`): a `GtkTextView`
  subclass made by `CodeView::create`, owned by its widget (deleted first
  thing in the view's dispose); `Editor::codeView()` reaches it. Its
  `snapshot_layer` runs **drawing passes**: `addPass(Layer, order, fn)`
  with `fn(GtkSnapshot*, visible)` in buffer coordinates (GTK translates
  the snapshot; x = 0 is left of `gtk_text_view_get_left_margin`), in
  `BelowText` (over the text's background, under the text and the
  selection) or `AboveText`, in ascending order; `removePass(id)`,
  `queueDraw()`. The ruler is `BelowText` order 0. Suggested places: bracket
  boxes (1.7) `BelowText` 10; carets (1.9) `AboveText` 10, fold
  placeholders (1.10) `AboveText` 20, inlay labels (2.7) `AboveText` 30. A
  pass draws only what is in `visible`, keeps its own state, and calls
  `queueDraw` when that changes; it never lays the whole buffer out.
- **Each buffer's lines are a `BufferLines`** (`src/BufferLines.{h,cpp}`),
  attached to every document's source buffer in `Document`'s constructor
  and owned by the buffer (`BufferLines::of(buffer)`): the core's
  `LineIndex` in characters, GtkTextIter's offsets, fed from `insert-text`
  and `delete-range` through `LineIndex::editUtf8` before the buffer
  changes, so an edit costs the text it inserts. It is also where per-file
  gutter state goes: `setChangeBars` (`QuickDiff::Bar`, moved by
  `QuickDiff::shift` with each edit, cleared by a whole new text),
  `setFolds(GutterFolds*)` (the fold lane's source: `hasClosedFolds`,
  `startsRegion`, `startsClosedFold`, `toggle`), `extraCarets` (offsets of
  carets besides the insert mark), and `changed()` to have every gutter
  showing the buffer draw again.
- **The gutter** (`src/Gutter.{h,cpp}`, the widget `MinicodeGutter` set with
  `gtk_text_view_set_gutter(LEFT)`) follows the view: `notify::buffer` (a
  tab switch), `notify::editable` (shown only for editable source with
  `BufferLines` and `editor.line-numbers` not off), the buffer's `changed`
  and the insert mark, the diagnostic tags, the adjustment, and the view's
  reallocation (`CodeView::allocated`; a scroll only reallocates the
  gutter, it does not draw it). Lanes left to right: dots (`kDotLane` 12;
  the tags `lsp-error` and `lsp-warning` by default, `setDiagnosticTags`
  for more, colored by each tag's underline color; a dot on the line a
  tag range starts on), change bars (`kChangeLane` 5, only while
  `setChangeLane(true)`; `onChangeBar(index, rect)` on a click,
  `rectOfChangeBar`), the numbers (digits of the line count, three at
  least, in the view's font with the monospace family set explicitly), and
  the fold lane (`kFoldLane` 14). It visits only the display lines on
  screen (`gtk_text_view_get_line_at_y`, `forward_display_line`) and numbers
  each logical line on the display line it starts on
  (`BufferLines::isLineStart`). Item 2.10 sets the bars and colors from
  `editor.git-*`; item 1.10 gives `setFolds` a source.
- **For checks**: `Gutter::drawnRows()` is what the last snapshot drew
  (line, label, y in the gutter's coordinates, active, dot, chevron);
  `CodeView::rulerXs()` the rulers' x; `gutterCpuMs()` the CPU time of the
  gutter's handlers and drawing. `GutterSuite` holds rows to an index made
  afresh and to GTK's own line positions.
- **Traps**: GtkTextBuffer's lines are not always the Mac's: a `\n` typed
  right after a lone `\r` stays a line of its own (an empty display line,
  unnumbered here), and text typed between `\r\n` keeps the `\r` inside its
  GTK line; the index and the numbers follow the Mac's rule (`\r\n` one
  break). The view's style lags behind `gtk_text_view_set_monospace` until
  the next frame, so a measure from the view's font as it was came out two
  pixels narrow beside the welcome text's font; the numbers' font is set
  explicitly, and a draw in another font size than the measure's measures
  again from an idle. The window's divider (Adwaita's 8 px of padding
  around the `GtkPaned` handle, plus GtkPaned's 6) took presses up to
  14 px into the editor, over the dot lane and the change lane's left part;
  item 18 (the layout) took that away, and the `gutter` suite now clicks a
  change bar 13 px from the edge. Seen while writing `gutter_typing`, with the
  gutter on or off: `Editor::revealLine(50000, ...)` in a 100,000-line file
  left the view near line 100 or 900, not 50,000 (GTK's height guesses
  outlast `settleOnCaret`'s two seconds); the suite scrolls by asking again
  until the line is on screen. (Item 22: `settleOnCaret` now keeps at it
  while GTK is still laying the buffer out.)
- **Trap (CI's GTK job, 2026-11-10): GtkTextLayout's display cache outlives
  a buffer.** The layout keeps a display for each line anything asked about
  (`gtk_text_view_get_iter_location`, `forward_display_line`, a scroll to a
  far line), and on `gtk_text_view_set_buffer` drops only the displays of
  lines it had validated. A display of any other line stayed, its line
  freed with the tab's buffer, and the next display the cache sorted in
  read it: a crash in `g_sequence_insert_sorted` under
  `gtk_text_view_get_cursor_locations`, one run in four, at random places
  in `tabs`. So the view's buffer changes only through
  `CodeView::setBuffer`, which empties the cache first (a tag reported
  changed with no change of size, the one public way), and code drawing
  beside the text asks only about lines with a height
  (`gtk_text_view_get_line_yrange`, which lays nothing out). The
  `gutter_switch` suite crashes in its first round without the first, under
  `MALLOC_PERTURB_`; plain runs may pass without it.

### 17. Sessions, backups and auto-save (done, November 2026; `PARITY.md` items 1.2 and 1.3)

What later items build on:
- **`Persistence`** (`src/Persistence.{h,cpp}`, the Mac's
  `EditorController+Persistence.mm`) is one per window, `App::persist`. It
  hears everything through four hooks: `Editor::onDocumentsChanged` (an
  edit, `d` set, or a change to the tabs, null, fired from `refreshStrip`),
  `onActiveChanged` (`tabShown`: a restored tab's note, focus-change),
  `notify::focus-widget` (`focusMoved`) and `notify::is-active`
  (`windowActive`). main.cpp calls `restoreSession` when a window opens and
  after Open Folder, `saveSessionClosing` from `onWindowRemoved`, and
  `autoSaveNow` before every close question (`closeTabAsking`,
  `confirmAllUnsaved`). The sessions file is the core's `TabSessions`;
  backups are the core's `Backups` through `src/BackupStore.{h,cpp}`, one
  per process, writing on a thread of its own.
- **When the session is written**: a second after the tabs change, when a
  window closes (or, if another window stays on the folder, by that one),
  before Open Folder, and for every window at the start of a quit
  (`g.quitting` then stops the closing windows from writing their own). A
  new window on a folder already open gets the folder's tabs, as on the
  Mac; a check that wants a new window with no tab closes them first
  (`keys`, `tabs`).
- **Tabs not read yet**: a session's tabs are `Document::setUnread`
  documents (a Message with an empty buffer, never dirty or saved) made by
  `Editor::addTab`; `Editor::showActive` reads one when it is first shown
  and gives it the place it came back with (`Place::restore`,
  `Editor::placeTab`). Code that walks the tabs must not take an unread
  one's empty buffer for its text. `Editor::placeOf` is what a tab keeps
  of its place (Reopen Closed Tab uses the same).
- **Per file**: `Document::backup()` (id, what was written, the note),
  `edits()`, `diskStamp()` (taken in `rememberDisk`), `restoreText`,
  `askAboutDiskChange`, `goneFromDisk`, `changedOnDisk`. A document's
  destructor removes its backup, so anything that destroys a dirty
  document must have asked first (or be a window closing after its
  question). The port still has no "(deleted)" state for a tab whose file
  went; auto-save leaves such a tab alone.
- **A restored file that changed since** asks Keep Mine or Reload when its
  window becomes active, or when its tab is shown; the harness's first
  wait now accepts a window that opens with such a question up.
- **Input methods**: `Editor::composing()` follows GtkTextView's
  `preedit-changed`. The checks now run with `GTK_IM_MODULE=
  gtk-im-context-simple` (`Wrapper.cpp`): IBus is installed on the
  ThinkPad and not on CI, and with no daemon on the private bus it showed
  no preedit for a dead key, which GTK's own input method does.
- **Not done**: the browser's page in the session (the Mac keeps its
  address, history and whether it showed); it waits for item 1.13's
  layout.
- **Traps**: `Ctrl+End` in a file GtkTextView has not laid out yet scrolls
  by guessed line heights and can leave the caret below the view; the
  `sessions` suite waits for the view to settle and steps up and down
  again. Three suites start second apps (`MCGuiRunChild`); a child that
  fails leaves backups nobody holds, and every later child then opens a
  window on that folder at launch (`reopenFoldersWithBackups`).

### 18. The Mac's layout (done, November 2026; `PARITY.md` item 1.13)

The browser sits right of the editor with a bar between them and the
terminal below both, across the whole width, as on the Mac; before, the
browser took the editor's place and closing the last pane brought the
editor back. What later items build on:
- **`WindowLayout`** (`src/WindowLayout.{h,cpp}`, `app->layout`) owns the
  window's panes: the sidebar and the right side in a `GtkPaned` (the
  sidebar's divider and its snap to the right edge moved here from
  `main.cpp`), and on the right the editor's side (the find bar over the
  `Editor`: its tab strip and slot), the browser, and the terminal. Each
  toggle (`toggleEditor`, `toggleBrowser`, `toggleTerminal`,
  `toggleSidebar`) hides or shows its own pane; with the editor, the browser
  and the terminal hidden the sidebar's divider goes to the right edge and
  nothing comes back until asked (`collapseRightAreaIfEmpty`). Anything
  that puts something in the editor's slot calls `main.cpp`'s
  `showEditorArea`, which is `revealEditor` (opening a file, Focus Editor,
  the tabs' keys, the preview toggle, a diff, Find); a URL opened from the
  terminal or a preview calls `showBrowser`, which leaves the keyboard where
  it was. Where the keyboard goes when a pane hides is the window's
  (`focusEditor`, `focusBrowser`, `focusTerminal`, `focusTree`), by the
  Mac's `focusAfterClosingPanel`.
- **The rules are the core's** (`../src/RightAreaLayout.h`, `rightarea:*`):
  the browser's share of the width, 150 px a side while both fit, the
  terminal's height, the smallest top (the tab strip and 40 px), a bar
  dragged within 16 px of an edge hiding that side and dragging back out in
  the same drag bringing it back, the terminal's bar to the top hiding the
  editor and the browser. The Mac keeps its own code
  (`EditorController+Layout.mm`), which the core follows: change both.
- **Why not nested `GtkPaned`s:** a paned draws no handle once a child is
  hidden, so the terminal could not be dragged back down from the top (the
  Mac keeps that bar at the top edge), and a side a drag hid could not come
  back in the same drag. The right side is a `GtkBox` with a custom layout
  manager that places the panes from `RightArea::layout`, a 4 px gap for
  each bar. One `GtkGestureDrag` on it, in the capture phase as GtkPaned's
  own, claims a press inside a bar's grab area (4 px past its line on
  either side, over the panes' edges) and denies every other press, so the
  drag outlives its bar hiding mid-drag; the bars are empty boxes that only
  draw (`.minicode-bar` in `ThemeCss.cpp`) and set the cursor. A probe
  showed that `GtkPaned` gives proportional shares when both children
  resize, and allocates a child shrunk past its minimum at that minimum and
  clips it, with no warning.
- **The sidebar's divider takes no presses over the editor.** GTK grows a
  thin paned handle's press area by 6 px each way, and the theme pads the
  handle 8 px into the end child, so it took presses 14 px into the editor,
  over the gutter's lanes (found by item 1.4). The handle is now a wide one
  (no 6 px) and its box is the stylesheet's own: 5 px painted as the
  sidebar, then the 4 px line, nothing past it. A negative margin is no way
  to widen it: GtkPaned measures a drag from the handle's box, and the
  divider jumped by the margin at the press.
- **Minimums:** the layout measures each pane and gives the core the larger
  of its own limits and what the pane needs, and the right side asks GTK
  for what the panes on show need, so no pane is allocated below its
  minimum (the sidebar's paned may still push the right side past the
  window's edge, as before). Each pane also clips to its rectangle.
- **The checks:** `MCGuiApp().layout` and its widgets (`area`,
  `editorPane`, `browserPane`, `terminalPane`, the bars, `sidebar`);
  `MCGuiDragThrough` drags through several points in one drag. The
  `layout` suite opens its own window (Ctrl+Shift+N) and closes it.
- **Changed for the user:** showing the browser no longer hides the
  editor, and hiding the terminal no longer brings back a hidden editor;
  Toggle Editor with nothing else open leaves the tree the window instead of
  opening the terminal; a URL opened from the terminal keeps the keyboard
  in the terminal; the find bar is above the editor only, not the browser;
  Toggle Sidebar moves the keyboard out of a sidebar it hides.

### 19. The palette (done, November 2026; `PARITY.md` item 1.5)

What later items build on:
- **A new command shows in the palette by itself**: it is offered once it
  has a row in `Keys.cpp`'s `actions()` (and so in `CommandsLinux.cpp`),
  labelled "Menu > Title" from the registry with its first key in effect,
  and only while its GAction is enabled when the palette opens. So an
  action that cannot act should be disabled (`g_simple_action_set_enabled`,
  as `win.save`, `win.exportpdf`, and the zoom keys are) rather than ring
  the bell, or the palette offers it. Recent commands are kept by registry
  id. The palette leaves out only itself.
- **`linux/src/PalettePanel.{h,cpp}`** is two children of the panes'
  `GtkOverlay` in `main.cpp` (a clear sheet under the box, and the box,
  placed by the overlay's `get-child-position`), so no window or popover
  takes the keyboard state; `show(text, Context)` opens it, and main.cpp's
  `showPalette` gives the folder and how Quick Open opens a file
  (`OpenFrom::QuickOpen`). The rows are a `GtkListView` over a
  `GtkStringList` of indexes into its items, so only the rows on show have
  widgets. A pick list of a caller's own rows (the Mac's `MCPalettePick`,
  for 2.9's branch picker) is not built yet; it would be another mode
  beside the four.
- **Symbols**: `askSymbols` is where 2.12 asks the server first. A preview
  calls `Editor::stopSettling` before it moves anything: for a moment after
  a tab is shown the editor keeps putting its place back whenever the view
  is still (`settleOnPlace`), which undid previews and the putting back when
  frames were slow.
- **State**: `$XDG_STATE_HOME/minicode/palette.json` holds
  `{"commands": [...], "files": [...]}`, most recent first (20 and 50).
- **Traps in the checks**: keys typed into the find bar while it is still
  sliding in were lost now and then, so wait for its revealer
  (`gtk_revealer_get_child_revealed`) before typing. A `GtkListView`
  rebinds its rows at once after its model changes but lays them out on
  the next frame, with any new rows all at the top and no height until
  then; a click in between went to another row (the palette suite waits
  for the row's label to have a height).

### 20. Find and replace (done, November 2026; `PARITY.md` item 1.6)

What later items build on:
- **`MultiEdit::apply(buffer, changes, keepCaret)`** (`src/MultiEdit.{h,cpp}`,
  D5) is the one way to make an edit with several ranges, the counterpart
  of the Mac's `replaceRanges:withStrings:actionName:`. Changes are
  `{start, length, text}` in characters (what `GtkTextIter` counts) with
  UTF-8 text, ascending and apart (`EditSpans::valid`, in the core); anything
  else is refused whole. They go in as one user action (one Ctrl+Z), last
  first, each range its own delete and insert, so the highlighter and the
  language server follow each one as they follow typing. With `keepCaret`
  the caret keeps its place in the text around it
  (`EditSpans::mapThrough`) and the selection goes; without, the selection
  is carried through the edits and the caller places carets itself.
  `MultiEdit::applying()` is true meanwhile, for anything that reacts to
  one typed character. Replace All and Replace in Folder's tabs use it;
  several carets (1.9), rename, formatting (Stage 2) and agent edits are
  to. A UTF-16 edit from the core (`TextFind::Edit16`, `MultiCaret::Change`)
  becomes one with `EditSpans::codePoints` over the text it was made from.
- **`FindBar`** (`src/FindBar.{h,cpp}`) hangs over the editor's slot
  (`Editor::addOverlay`, a `GtkOverlay` round the stack), follows the
  view's buffer (`notify::buffer`), and is hidden while the slot shows
  something other than text. Its rules are the core's `FindRules` (the
  seed, the count, the search in UTF-16 and code points, Replace and
  Replace All's changes, the theme colors, held to the Mac's table by a
  test). Matches near the lines on screen carry text tags
  (`minicode-find-match`, `-current`, `-scope`), never a snapshot pass.
  Searching runs on a `GTask` over a copy of the text made once per edit.
  Keys are the registry's; in the bar itself Enter, Shift+Enter, Escape and
  Tab. `MCGuiApp().findBar` reaches it from a suite.
- **Replace in Folder** is `SearchPanel`'s (`setEditors` gives it every
  window's editors and its own): it counts and asks, edits a clean tab of
  this window through `MultiEdit`, leaves a file with unsaved text in any
  window alone and names it, writes the rest atomically, matches tabs to
  files by device and inode, and replaces a row's match alone with
  Ctrl+Shift+1 only while its line reads as searched. Files to include and
  exclude are behind "…". The panel's window is no application window, so
  it reads the registry's option keys itself (`onWindowKey`). There is no
  right click menu on a row yet, and no replace preview in the rows.
- **Keys**: Alt with a letter alone now goes to the program while the
  terminal has the keyboard (`linuxTerminalTakes`), as Meta, so the
  options' Alt keys never take readline's word keys.
- **Traps paid for** (each found by the suites):
  - The primary selection has one owner, and the one it is taken from
    drops its selection: a `GtkText` collapses to its caret, which it had
    put at the start (the query typed after Ctrl+F went in before the seed,
    "alpha10"), and a `GtkTextBuffer` collapses to its insert mark (focusing
    the query, all of it selected, dropped the editor's selection Ctrl+F had
    seeded from). While the bar's fields have the keyboard, a selection in
    the text is made with the view's primary clipboard taken off the buffer
    for the moment (`FindBar::selectInText`), and the editor's selection is
    put back that way after a field takes the keyboard.
  - A `GtkTextBuffer` with a selection offers it to the primary clipboard
    again after every delete, an X round trip each: 40,000 deletes took
    58 s. `MultiEdit` takes the selection away first and carries it
    through.
  - GTK replays an undo step edit by edit with no user action around it, so
    the highlighter followed each of 40,000 edits alone (19 s).
    `Document` takes "undo" and "redo" as one action of its own (its flags
    only: a user action on the buffer would clear GTK's redo queue). The
    undo of Replace All of 20,000 matches still costs about 1.6 s: GTK
    places the caret after each edit it puts back, and the view lays out
    that line for the input method. Not done: undoing such a step faster.
  - The checks run with `GTK_A11Y=none`. Whether GTK's accessibility
    (AT-SPI on the owner's desktop) costs something per edit of a large
    Replace All has not been measured.

### 21. Brackets and auto-closing pairs (done, November 2026; `PARITY.md` items 1.7 and 1.8)

What later items build on:
- **The buffer a unit a character** (`src/BufferText.{h,cpp}`): the core's
  editing rules (`MultiCaret`, `AutoClose`, `BracketMatch`) count UTF-16
  units on the Mac; here they get one unit a code point, GtkTextIter's
  offsets, with U+FFFD for a character outside the BMP (the rules only ask
  whether a character is a bracket, a quote, a blank, or a break).
  `BufferText::text(buffer)` is a `MultiCaret::Text` reading the buffer in
  blocks (`gtk_text_buffer_get_slice`, so an embedded object still counts);
  `BufferText::context(doc, buffer)` is the document's strings and comments
  in characters, `AutoClose::Utf8HighlighterContext` over its UTF-8
  highlighter and `BufferLines`' index (the core, `autoclose:utf8-context`),
  or the plain context when the highlighter is not in step.
  `Document::highlighter()` and `highlighterText()` give the highlighter
  and its mirror only while they are in step (not during a large user
  action).
- **Bracket boxes and Go to Bracket** (`src/BracketView.{h,cpp}`, one per
  editor, `Editor::brackets()`): a `CodeView` pass at `BelowText` order 10,
  worked out in an idle after the caret or the text moves, over a window of
  20,001 characters either side of the caret; `boxed()`, `drawnBoxes()`,
  `updates()` and `lookups()` are for checks. Go to Bracket is
  `win.gotobracket`, VS Code's Ctrl+Shift+\ (`<Ctrl>bar`), an editor
  command, so the program in the terminal gets the key there.
- **Bracket-pair colors** (`src/BracketColors.{h,cpp}`, one per document,
  `Document::bracketColors()`): `BracketPairs::Depths` set from every
  line's tokens when the highlighter lexes the whole file and from each
  edit's re-lexed tokens in `Document::noteEdit` (`BracketPairs`' UTF-8
  overloads, `bracketpairs:utf8`); four tags made after the highlighter's
  in the shared table, painted only for the lines on screen: after each
  flush (`Document::paintBracketsOnScreen`), and from `BracketView` on the
  vertical adjustment's `value-changed` and `changed` and when another
  buffer comes on show. Painting is synchronous in the scroll handler, so
  a line never shows a frame in the old colors. The current lines are a
  set of line ranges that follows edits (an edit that changes the depth
  below it empties the set from its line on). Off with no highlighter, so
  item 1.12's large-file mode turns them off by dropping the highlighter;
  it should also make `BracketView::applies` false, as the Mac's
  `bracketMatchingApplies`.
- **Auto-closing pairs** (`src/AutoCloseInput.{h,cpp}`, one per editor,
  `Editor::autoClose()`): called first in `Editor::onViewKey`, the
  capture-phase key handler, so it acts before the text view and its input
  method see the key; never while `Editor::composing()`. Its public calls
  (`typed`, `deletePair`, `remember`, `caretsAre`) take carets, for item 1.9
  to call with every caret and apply through D5's `MultiEdit` (item 1.6);
  until then `apply` puts each change in at a caret placed there, last
  first, in one user action, so the typed character goes in last with the
  caret after it (Lsp.cpp's trigger check reads the character before the
  caret) and one Ctrl+Z takes a close. The Tracker follows `insert-text`
  and `delete-range` of the buffer on show and is validated on `mark-set`
  and `notify::cursor-position`; it starts empty for each buffer.
- **Traps**: emptying a selection for a moment and setting it again in one
  key press gave up the PRIMARY selection, and X's late word that it was
  lost then took the new selection away, now and then: a surround's edit
  must not place the caret while a selection exists (`auto_close` checks the
  selection is still there half a second later). GTK's simple input method
  keeps a dead key after a key it cannot compose with (dead acute then
  `(`), and Pango then warns "pango_layout_get_cursor_pos: assertion
  'index >= 0 ...' failed" with auto-closing or without; Escape ends the
  composing. A tab closed before the view has drawn it (open a file, then
  `Editor::closeAllTabs()` in the same step) aborts with "Gtk-ERROR:
  gtk_text_btree_line_number couldn't find line" in GtkTextView's own
  snapshot; that is on main, reported, and `bracket_pairs` lets the view
  draw a frame before each close. A text with a `\n` typed inside an open
  bracket in Python is indented by AutoIndent, so a check that selects a
  line with Shift+Home must close the bracket first.

### 22. Big files and long lines (done, November 2026; `PARITY.md` item 1.12)

What later items build on (`src/LargeFileView.{h,cpp}`, the core's
`../src/LargeFile.h`):
- **Large-file mode**: `Document::largeFileFeaturesOff()` is true for a text
  over 20 MB or 300,000 lines, read with `editor.large-file-optimizations`
  on, until the status bar's note turns the features on for its tab. Every
  feature VS Code turns off there asks it: the highlighter does
  (`startHighlighting`), and folding (1.10) and bracket boxes and colors
  (1.7) must. `Editor::notifyDocument` gives a server no such document
  (`languageServerMayRead`, which also keeps files past VS Code's 50 MB sync
  limit from any server). `Editor::turnOnLargeFileFeatures` (the note's
  click) restarts the highlighter and tells the server; a feature that
  works its state out when a file loads should start it there too.
- **Reading in the background**: a file over 4 MB is read, checked,
  measured and lexed on a worker thread (`Document::beginLoading`), then put
  in its buffer a piece at a time, 8 ms a turn (`LargeFile::pieceEnd` says
  where to cut). Meanwhile `Document::loading()` is true: the tab is Text
  but not editable or saved, the view shows `loadingBuffer()` ("Opening…",
  in the source's font, since a font change made GTK invalidate every line
  of the text coming on show), and no server has it. Anything that acts on
  the text once it opens must wait: `revealLine`, `revealLineColumn` and
  `placeTab` do, through `whenLoaded` and the place a session or Reopen
  Closed Tab gives (`place().haveRestore`, as for a tab not read yet). The
  host hears `documentLoaded`. Files over 50 MB opened by hand ask first
  (`LargeFileUi::mayOpen`, from `openFile`), and over 256 MB are refused.
- **Long lines**: `LongLines::of(buffer)` hides what a line over 10,000
  characters does not show, under the invisible tag
  `minicode-long-line-rest` (never in the file, the undo history, a copy, or
  a server's text, D6). `partOf(line)` is the part shown; the caret's line's
  part follows the caret a turn after it moves (a search, Go to Line, End);
  `LargeFileUi` draws "… N more characters" after each part (a `CodeView`
  pass, AboveText order 40), and puts End, Home and the arrows right where
  GtkTextView would skip the hidden text. Folding's invisible tag (D8) must
  leave this tag alone, and the other way round.
- **Copy and cut of hidden text**: GTK's copy leaves out what an invisible
  tag hides (Select All and Copy on a 300,000-character line handed over
  10,001 characters). `LargeFileUi` takes the view's `copy-clipboard` and
  `cut-clipboard` when the selection holds any invisible text, folds
  included, and puts all of it on the clipboard as plain text. Not done:
  the X11 primary selection (a middle click pastes the visible part) and
  dragging a selection, both GTK's.
- **AutoIndent on big buffers** reads the 2,000 lines above the caret, not
  the whole buffer (`LargeFileView::indentSlice`, from `sourceText` and
  `electric`): Return and Tab copied all 25 MB each time.
- **For checks**: `tests/gui/FakeLsp.h` is the Mac's scripted language
  server (`tests/fakelsp`, built by meson.build beside minicode-guitest);
  `LongLines::cpuMs`, `LargeFileUi::questionsAsked` and `notesDrawn`.

Measured on the ThinkPad (load 1 to 5 unless said), main-thread CPU:
- **A 5 MB line**: GtkTextView lays a line out as one Pango layout. The
  open held the loop for 4 to 7 s at once (6.7 s at load 25) and each
  keystroke 3 to 9 s; wrap CHAR or WORD_CHAR made no difference (4.3 and
  4.5 s), and past about 260,000 characters a line without wrapping is not
  drawn right anyway (Pango's widths overflow; pixman warns of an invalid
  rectangle). Shown 10,000 characters at a time: the longest stall opening
  it 33 ms, a keystroke 2.5 to 3 ms, mostly GTK laying out the part shown.
  Setting the tags again on every key cost 20 ms a key (GTK joins and
  splits the line's text), so a part may grow to twice its size first; and
  the highlighter copied the whole line on every key though it never lexes
  it (`retagLines` now skips a line with no tokens).
- **A 25 MB file of 470,000 lines**: 0.33 to 0.37 s to open in all, against
  0.33 s to read it and put it in a buffer in one go, never more than 13 to
  21 ms at a time (0.6 s at once before); a keystroke 0.07 to 0.18 ms;
  Return and Tab never more than 25 ms (0.45 to 0.6 s before, AutoIndent's
  copy); switching to and from its tab 8 to 60 ms (100 to 130 ms of it was
  the language server session taking its squiggles off the whole buffer,
  see the traps). A 60 MB file of 10 MB lines after Open Anyway:
  never more than 75 ms.
- **GTK lays out every line once** after a buffer comes on show, in idle
  slices: 10.7 s for a bare GtkTextView with 25 MB, 13 to 17 s in the
  editor, never more than 60 ms at a time, and again each time the tab is
  shown again. GtkTextView has no way round it.
- The find bar's case-insensitive search through the 5 MB line holds the
  loop 0.3 to 0.6 s (GtkTextIter folds the whole line); item 1.6 replaces
  that search.

Traps:
- **GTK gives a line no height until it has laid it out.** Right after a big
  buffer comes on show, line 300,000 sits at a small y with thousands of
  lines of no height at the same pixel; `gtk_text_view_get_line_at_y` at
  that pixel answers the first of them, GTK keeps that line at the top as
  heights arrive, and the view drifted from line 300,000 to line 2,000 in a
  few seconds. `settleOnCaret` and `settleOnPlace` now keep at it while the
  height grows (up to three minutes), `settleOnCaret` with the caret's line
  one pixel past the top margin and following the caret as it is typed
  into, and ask for the top line a pixel into the view. A check of "on
  screen" in a big file must also check the top line (the suite's
  `caretShown`). Holding the view by setting the adjustment each frame did
  not work: GTK clamped it back to the bottom of what it had laid out.
- `gtk_text_buffer_remove_tag` over the whole buffer queues a redisplay of
  all of it whether the tag was there or not: about 25 ms a tag at 470,000
  lines while the buffer is on show. `LspSession::clearMarks` took its four
  tags off the whole buffer at every tab switch; it now takes each off its
  own mark's range.
- GtkTextView's own scroll to the caret after a key measures a line with
  hidden text as it was before the key, and left the caret past the right
  edge; `LargeFileUi` scrolls again a turn later.
- Text typed at either end of a tagged range is outside it (a toggle-on
  segment has right gravity, a toggle-off left).
- A wait that searches the buffer on every poll holds the main thread
  itself (100 ms a poll in a 5 MB line): work offsets out from what was
  written.
- After Ctrl+F the find field has the focus, but keys typed in the checks
  went nowhere until the field was clicked.
- `g_format_size` puts a no-break space before the unit.

### 23. Leaving the terminal (done, November 2026; `PARITY.md` R2)

`minicode` typed in a terminal used to be the app in the foreground: the
shell waited, GNOME Terminal warned that closing it would kill a process,
and closing it did. Now `main` calls `Detach::start` first
(`src/Detach.{h,cpp}`, the rules in `src/LaunchOptions.{h,cpp}`), which
reads `-h`, `-v`, `--foreground` and `--` itself, and with a terminal on a
standard stream forks: the child calls `setsid` and runs the app, the
parent waits on a pipe until the child's first window (an emission hook on
GtkApplication's `window-added`, so the command line's own messages, such
as a path that is not there, still reach the terminal), then exits 0; or it
exits with the child's status when the child ends first (a second
`minicode` handing its path over, `--help`, no display). At the window the
child puts `/dev/null` on every standard stream that is not a regular file
and asks the session bus's systemd for `app-minicode-org.minicode.Editor-<pid>.scope`.
- **Fork before anything starts a thread.** Nothing before `Detach::start`
  may touch GLib or GTK, and it checks `/proc/self/status` and stays in the
  foreground if a library's constructor ever started a thread.
- GNOME Terminal never stops a shell's `vte-spawn-*.scope` (libvte 0.84 and
  gnome-terminal-server have no StopUnit or KillUnit; the scope's drop-in
  is `KillMode=process`), and the journal showed the owner's terminal
  scopes ending when the processes in them did. Only the hang-up's SIGHUP
  closed minicode, which a process in another session never gets. The
  scope of its own is for `systemctl --user` and systemd-oomd, which judge a
  scope as one; a refusal changes nothing.
- The GUI checks' build leaves the hook out (`MINICODE_GUITEST`): the
  harness's app must stay its child. The `detach` suite runs the real
  `build/minicode` on pseudo-terminals instead, with three traps of its own.
  On the harness's session bus this app owns `org.minicode.Editor`, and the
  real binary handed it its command line and exited, so the suite starts a
  dbus-daemon of its own from the harness's `dbus.conf`. VTE in the real
  binary asks that bus's systemd for a `vte-spawn-*.scope` per shell, so
  the suite's stand-in systemd hears those too. And the real binary's
  browser pane loads DuckDuckGo (only the checks' build has about:blank),
  so it runs with `http_proxy` and `https_proxy` at a listener in the suite
  that answers nothing; WebKitGTK 2.52 honours them (the listener hears
  `CONNECT duckduckgo.com:443`).
- A shell started for a check from inside the harness inherits its signal
  dispositions and mask unless they are reset before `execve`: a SIGHUP
  ignored there would let the hang-up check pass without the fix.

### 24. The time to a usable window (done, November 2026; `PARITY.md` item R3)

What later items build on:
- **`MINICODE_STARTUP_LOG=1`** (`src/StartupLog.{h,cpp}`) prints a line to
  stderr for each phase of the start, with the time since main and the
  main thread's CPU time: how long the process ran before main, GTK's
  startup, the command line, the settings, keys and menu bar, each pane of
  the first window, the session's tabs, the first file read and
  highlighted, the window realized and presented, the first three frames
  (update and layout, paint), the first frame with the process's CPU and
  what it had started, the tree's first listing, the first idle after it
  ("settled"), and the first time each thing starting on its own does
  (the shell, the web view, the browser's first page, each language
  server, git). A new thing a window starts at launch, or a new pane,
  gets a `StartupLog::mark` or `once`, so it shows. Without the variable
  each call tests one flag; `StartupLog::firstFrame()` is kept either way
  for the checks. A run with the log stays in the foreground (main.cpp sets
  `MINICODE_FOREGROUND` for `Detach::start`, item 23): a detached app puts
  `/dev/null` on its stderr once its window is made, which would cut the
  log off before the first frame. `begin()` runs after the fork, so its
  clock is the main thread's of the process that runs the app.
- **Nothing hidden is made at launch.** `Terminal` and `Browser` make their
  widget at once, and the terminal and its shell, or the WebKitWebView,
  when the panel first maps, as the Mac makes its terminal dock and its
  browser when first shown (`makeTerminalPanel`, `makeBrowserWithHome:`).
  The terminal is also made by `focus()`, and keeps settings told to it
  before; the browser's `load()` makes the view and loads that page
  instead of the home page, which is why `openLink` loads before it shows
  the pane. `made()` and `webView()` say whether they exist yet. So the
  real binary's browser loads DuckDuckGo only once shown. A future
  browser page in the session (the Mac's `restoreBrowserFromSession`) makes
  the view at launch only when the session had it.
- **Work after the first frame:** `whenDrawn(widget, fn)` (`src/WhenDrawn.h`)
  calls `fn` from the idle after the widget's window has drawn its first
  frame. `LspSession` holds a document that comes before that (the tab a
  session brings back) and starts its server then.
- **For the owner's desktop**, where the checks cannot go:
  `linux/dev/startup-times.sh [-n runs] [-r renderers] BINARY [FOLDER]`
  opens the app with the log on, three times for each of GTK's default
  renderer, `gl`, `vulkan` and `cairo`, waits until it settles, ends it by
  its PID and prints the phases. Every other minicode must be quit first,
  or the run hands its folder to the running one and logs nothing. For
  main before R3, build commit 3a5016d (the log without the fix) in a
  worktree of its own.

Measured on the ThinkPad's Xvfb (cairo drawing, `~/minicode` read only,
medians of six warm runs with main and the fix interleaved, load 3 to 6;
milliseconds after main, wall / main-thread CPU):

| phase | main | R3 |
|---|---|---|
| GTK's own start (`gtk: started`) | 100 / 93 | 96 / 93 |
| the browser's part of the window | 39 / 23 | 0 / 0 |
| the window built, no session | 164 / 142 | 115 / 110 |
| realized, then presented | 185 / 162, 217 / 182 | 139 / 132, 166 / 150 |
| first frame, no session | 244 / 203 | 189 / 170 |
| first frame, five session tabs | 299 / 248 | 220 / 197 |
| clangd started (five tabs) | 185, before the frame | 307, after it |
| CPU of the app and all it started, first 3 s | 1.87 s | 0.26 s |

The process runs 47 to 50 ms before main (GTK alone is about 25 ms of it,
WebKit about 17, VTE and poppler about 13: what they map and construct).
A bare GtkApplication window reaches its first frame at about 170 ms of
CPU here, so GTK is most of what is left. The PPA's 1.4.14, timed from exec
by an LD_PRELOAD probe that wraps `gtk_window_present`, drew its first
frame at 268 ms with 1.91 s of CPU in all: on Xvfb nothing takes seconds.
The renderer is the likely difference on the desktop: on Xvfb `gl` runs
on llvmpipe, and its first frame's paint took 410 to 560 ms with an empty
Mesa shader cache and 58 ms with a warm one, against 17 to 30 ms for
cairo; the desktop's GPU, Vulkan by default on Wayland, was not measured.

Traps:
- WebKit waits on the main thread for each sandboxed process it starts
  (a poll on the bwrap's info pipe, 40 to 140 ms each on Xvfb), so a
  WebKitWebView costs the window even hidden.
- A widget made from a parent's `map` handler is realized as it is added,
  since the parent is mapped by then: a `realize` handler connected after
  `gtk_*_set_child` never runs. `Terminal::makeTerminal` puts the terminal
  into its panel last; the `startup` suite checks that its key controller
  on the window was added.
- `LspSession::documentChanged(path_)` passes a reference to the member
  it clears first; take a copy.
- `perf` is closed to users here (`perf_event_paranoid` 4) and the owner
  wants no debugger, so the log, `strace -f -tt -T` and a bare GTK program
  timed the same way are what there is.

### 25. Zoom, word wrap, Go Back and Maximize Pane (done, November 2026; `PARITY.md` item 1.11)

What later items build on:
- **`Zoom`** (`src/Zoom.{h,cpp}`, over the core's `../src/ViewZoom.h`)
  holds what every window shares: the editor's zoom and the terminals'
  (points, in `$XDG_STATE_HOME/minicode/zoom.json`, read once at the first
  window, written on each change, never the settings file) and the word
  wrap toggle (for the session). The editor's size is one rule of a
  display-wide stylesheet on the view `attachEditor` marked (class
  `minicode-zoom`): its source at editor.font-size points when the file
  sets it (`Settings::editorFontSizeSet`), else the desktop's size as
  before, plus the zoom; its prose (a Markdown page, a message) at the
  desktop's size times `previewScale`. With no zoom and no size set there
  is no rule at all. Each window listens (`Zoom::listen`): "editor" scales
  its Markdown tags (`scaleTags`, which keeps each tag's first size as its
  base), "terminal" sets the terminal's font (`Terminal::setFontSize`: the
  setting's or the desktop's size, and VTE's font scale for the zoom),
  "wrap" calls `Editor::applyWordWrap`. The gutter follows the view's font
  by itself. The LSP popups, the find bar and the palette do not follow the
  zoom yet (the Mac's completion list and hover do).
- **Word wrap**: `Editor::setProseFont` wraps source as `Zoom::wordWrap()`
  says (editor.word-wrap, on by default, as on the Mac; before this item
  source never wrapped), and prose always. Two exceptions keep item 22's
  paths as they were. In large-file mode wrapping is one of the features
  off (as in VS Code; the Mac keeps it): GTK laid 470,000 lines out half as
  long again wrapped (47 s against 34 to keep line 300,000 on screen), and
  the note's click turns it back on with the rest
  (`turnOnLargeFileFeatures` calls `applyWordWrap`). And a long line
  (`LongLines`) never wraps: GTK takes a paragraph's wrap mode from the
  tags on its first character, so `LongLines::keepUnwrapped` puts
  `minicode-long-line-nowrap` (wrap-mode none) on that one character after
  every edit of the line; wrapped, End went to the end of the part's first
  row instead of on into the hidden rest, and opening a 300,000-character
  line held the loop 93 ms against 16.
- **`NavHistoryUi`** (`src/NavHistoryUi.{h,cpp}`, one per window,
  `app->nav`) is the host of the core's `Nav::Recorder`
  (`../src/NavRecorder.h`, the Mac's `MCNavigator` over `NavHistory.h`).
  It watches the source buffer on show (`mark-set` for the ten-line rule,
  `insert-text` and `delete-range` so places follow edits; replacing the
  whole text, a reload, moves none), and hears every tab switch through
  `Editor::onActiveWillChange` and `onActiveChanged`. main.cpp runs
  `openMatch` (Find in Folder, TODOs, the server's definition in another
  file) and `openLink` (terminal and Markdown links) inside `nav->jump`,
  calls `moved` from `followMove` and `removed` from `trashNow`, and
  `clear` from Open Folder. The palette tells it when it opens and closes
  (`PalettePanel::onOpenChanged`: nothing previewed is a jump) and runs Go
  to Line's and a symbol's move through `aroundMove`, a jump from where it
  opened. A definition in the same file is no `onReveal`: the ten-line rule
  records it. A new feature that moves somewhere far calls `nav->jump`
  around the move; one that previews calls `paletteOpened`/`Closed` or the
  recorder's `beginQuiet`/`endQuiet`.
- **Maximize Pane** is the core's (`RightArea::State::maximized`,
  `toggleMaximized`, `leaveMaximizedFor`, `onScreen`; `rightarea:maximize`)
  shown by `WindowLayout::toggleMaximized`. Each toggle of a pane hidden
  only by it shows that pane (as the Mac's), and `revealEditor` and
  `showBrowser` end it; `apply` drops a maximized pane hidden some other
  way (Close Tab on the browser). The panes' `browserShown()` and
  `terminalShown()` still say what the user asked for; `maximized()` says
  whether one pane has the right side.
- **Keys**: `Keys::Place::page` is the browser with the keyboard, or
  outside the terminal a PDF or the LaTeX preview on show (the PDF view
  takes no keyboard, and the port's PDF zoom always worked so): a `Page`
  command is bound only there, a `NotPageOrTerminal` one (the editor's
  zoom) only elsewhere, and Focus File Tree keeps Ctrl+0 in both. A
  `Terminal` command keeps its plain Ctrl keys in the terminal
  (`Commands::linuxTerminalTakes`). The Terminal menu is new, with the
  terminals' zoom in it. A keypad key is bound with Num Lock on and off
  (`<Ctrl>KP_0` and `<Ctrl>KP_Insert`), since GTK matches the keyval. The
  editor's zoom reset is Ctrl+keypad 0 alone: Ctrl+Alt+0 is Fold All in the
  editor, and the terminals' reset and a page's fit width elsewhere
  (`PARITY.md` D1, the decision after 1.11's choices).
- **For checks**: `MCGuiApp().nav` (entries "path:line" and `index`);
  `MCGuiKey` now takes a GDK name with capitals ("ctrl+KP_Insert", the
  keypad's 0 as the checks' display gives it with Num Lock off). The four
  suites are `zoom` (with `_zoom_child`), `word_wrap`, `go_back` and
  `maximize`.
- **Traps**: a `font-size` on a text view's `text` node changes nothing:
  GtkTextView takes its font from the view's own node (a throwaway program
  showed it), so `ThemeCss.cpp`'s `.minicode-prose text { font-size: 15pt }`
  never applied and the Markdown body is the desktop's size; the zoom's
  rule is on the view. Clicking the middle of a short file's view moves the
  caret far (to its end), which the ten-line rule records: a check that
  wants an empty history clears it after the click.

### 26. Several carets and the line commands (done, November 2026; `PARITY.md` item 1.9)

What later items build on (`src/Carets.{h,cpp}`, `src/CaretEdits.cpp`,
`src/CaretText.h`; the core's `MultiCaret.h` and `EditBatch.h`):
- **`Carets`** (one per editor, `Editor::carets()`) holds the carets as
  character offsets, two or more while `active()`, the first being the
  buffer's own selection (GTK draws and blinks it), the others drawn by two
  `CodeView` passes, selections at `BelowText` 5 and carets at `AboveText`
  10, only those on screen, and given to the gutter through
  `BufferLines::extraCarets`. `current()` is every caret, or the buffer's
  selection as one; `set()` shows carets (one goes back to being the
  buffer's selection). Any edit or caret move it did not make (a click,
  Page Down, Select All, an undo, a reload, Find, a tab switch) ends them;
  `applying()` is true while it edits or moves the selection itself.
- **Every edit is one `MultiEdit::apply`** (D5), one user action and one
  Ctrl+Z, the carets then where the core's `MultiCaret::Edit` says. Text a
  command copies from the buffer (Move Line, Copy Line, Paste) goes
  through `CaretText`, which reads a character outside the BMP as a unit
  of the Private Use Area from a table and writes it back, where
  `BufferText` would give U+FFFD. Rename, formatting and agent edits that
  move carets can call `Carets::set` afterwards.
- **Keys**: typing, Return, Tab, Backspace and Escape at several carets
  come first in `Editor::onViewKey` (`keyPressed`); everything else is
  GTK's own key bindings caught by their signals on the view (`move-cursor`
  for the arrows, Home, End, words, Ctrl+Home and Ctrl+Up;
  `delete-from-cursor`; `copy-clipboard`, `cut-clipboard`,
  `paste-clipboard`), so they hold whatever keys GTK gives them. With one
  caret only the word moves and deletes (VS Code's words) and Ctrl+Up and
  Ctrl+Down (a line's scroll) are taken; a Markdown page or a message keeps
  GTK's. Auto-closing pairs go through `AutoCloseInput::typed` and
  `deletePair` with every caret; its `carets()` answers every caret while
  several are in play, and it ignores the mark moves of a carets' edit.
- **The mouse** is a click and a drag gesture grouped, in the capture
  phase, claiming only what is theirs: Alt (a caret, or a column dragged),
  Shift+Alt (a column from the caret), the middle button (a column dragged;
  a click pastes the primary selection, as GTK would), a plain double and
  triple click in source text (VS Code's word, the line with its break, and
  a drag after either by words or lines). Presses over the gutter are left
  alone (`gtk_widget_pick`).
- **Commands**: `Carets::addActions` adds the window's ten actions
  (`win.addnextoccurrence` ... `win.copylinesdown`), each acting while the
  view has the keyboard. Ctrl+Home and Ctrl+End are rows of the Linux table
  without an action (the registry's Fixed), and Keys.cpp's table check
  passes them. The find bar's Alt+Enter is `FindBar::selectAllMatches`.
- **The highlighter's batch** (`Document::startBatch`, `finishBatch`): a
  `MultiEdit` of 8 ranges or more on a document's buffer is taken in bytes
  at begin-user-action (`MultiEdit::applyingChanges`), and at
  end-user-action given to `IncrementalHighlighter::editMany` together;
  only the lines it re-lexed are retagged, those on screen at once, the
  rest from `Document::later_` in idle time (byte ranges carried through
  later edits). Replace All uses it too. If anything else edits the buffer
  in the same user action, the old resync runs instead. Lsp.cpp gives no
  trigger character to completion during a `MultiEdit`.
- **For checks**: `drawnCarets()`, `drawnSelections()`, `lastEditMs()`;
  `tests/gui/CaretSuites.h` (where a character is on screen, the carets as
  text, the clipboard); `MCGuiDragWith` (a drag with modifiers held, any
  button).

Traps:
- Taking the view's primary clipboard off a buffer that holds the primary
  selection makes GtkTextBuffer give up its selection (a mark set, which
  ended the carets in the middle of setting them, and indexed an emptied
  vector). `Carets` takes it off around its own selection changes and
  edits, as one of its own moves, and puts it back after; several carets'
  selections never reach the primary clipboard.
- The caret's box (`gtk_text_view_get_cursor_locations`) is a pixel
  shorter than its line: Up and Down at several carets look for the first
  y past it that lands on another display line.
- A click computed in the same step as an edit that moves the text lands
  on the old layout: in the checks, wait for the text before clicking
  (`waitCarets`, the suites' `queueReset`).
- A Shift+Alt press is VS Code's box selection from the caret, wherever
  the press is: click first where the column should start.
- While the caret blinks, GTK 4.22 draws the view every frame for seconds
  after any key (Shift alone included): 1.7 ms a frame, 100 ms of CPU a
  second at 100,000 lines. A CPU measure that waits for the main thread to
  go quiet catches it at random; `multicaret_perf` turns the blink off.
- A double-click in a Markdown page edits the block (Editor.cpp), so a
  page has no word selection to keep; the carets never claim a press
  there.

Not done: an input method's composed text goes in at the first caret and
ends the others; the extra carets do not blink; a column drag does not
scroll the view when the pointer leaves it; the Mac's Cmd+Shift click
pairs (Ctrl+click is Go to Definition here).

### 27. Folding (done, November 2026; `PARITY.md` item 1.10)

What later items build on (`src/Folds.{h,cpp}`, the core's
`../src/Folding.h`):
- **`DocumentFolds`**, one per `Document` (`Document::folds()`, made in its
  constructor right after `BufferLines`, and found from a buffer with
  `DocumentFolds::of`): the regions (`Fold::compute` on a worker thread
  0.3 s after the text last changed, generation counted; `freshRegions()`
  works them out on the spot for a command), the closed folds
  (`Fold::Collapsed` in characters, which follows edits and opens a fold an
  edit touches), the invisible tag `minicode-fold` over the whole lines
  each outermost fold hides, and the gutter's fold lane (`GutterFolds`,
  with `lastHiddenLine` for the gutter's jump). Nothing for anything but a
  text file's source, and nothing in large-file mode
  (`applies()`); `forget()` on a reload from disk, `sourceChanged()` when
  the name or large-file mode changes.
- **`FoldUi`**, one per `Editor` (`Editor::folds()`): the placeholder pass
  (`AboveText` order 20) and its click, the keys beside a fold (asked first
  in `Editor::onViewKey`), and View > Folding's fifteen actions, added to the
  window by `main.cpp` (`addActions`), with notes to the status bar
  (`onNote`). Commands run only while the editor has the keyboard.
- **The limit** (D8): `DocumentFolds::kMaxHiddenLines`, 5,000 lines hidden
  by a tab's folds; `closeWithinLimit` asks `Fold::pickWithinBudget` which
  candidates fit. Opening a fold never adds hidden lines, and an edit never
  closes one, so only closing checks it.
- **Several carets** (item 1.9): the commands already take
  `BufferLines::extraCarets` as more carets; the keys beside a fold leave
  them alone. An edit with several ranges through `MultiEdit` opens what it
  touches by itself.
- **For checks**: `FoldUi::drawnPlaceholders()`, `Gutter::linesWalked()`,
  `DocumentFolds::computes()`, `hiddenLines()`, `outermost()`.

Traps:
- **Order of the buffer's handlers.** A tag changed in a handler
  invalidates every iterator, and the host's handler after an edit makes
  edits of its own (an electric dedent from `documentEdited`). The folds'
  after-handlers come first of all (made before Document connects its own),
  fix the tag around the edit, and set the signal's iterators again from
  their offsets; `Collapsed::edit` runs in the before-handlers, which see
  the edits in the order they happen.
- **Never change a tag inside `mark-set`.** `gtk_text_buffer_select_range`
  emits it for the insert mark and then for the selection bound with an
  iterator of its own; a reveal there would leave that iterator stale. A
  caret landing in hidden text opens its folds from an idle at
  `G_PRIORITY_HIGH_IDLE`, before GTK lays out, draws, or carries out a
  scroll it was asked for.
- **Text typed inside a tagged range is tagged**, and at either end it is
  not (the toggles' gravities): a line break put in at the end of a fold's
  last line would be hidden, and one typed at the start of the line after
  it would not end the fold. `retag` makes the tag right over a few
  characters around every edit, from the folds as they are.
- **There is no caret place after the placeholder.** GTK puts the caret at
  the end of the folded line (before the placeholder) or at the start of
  the line after the fold. So Return at the end of a folded line moves the
  caret to the end of the fold's last line for the key (no reveal for that
  move) and the editor's own Return (AutoIndent, a Markdown list) makes the
  line there, below the fold; Backspace "after the placeholder" is
  Backspace at the start of the line after a fold. GTK's undo of that
  Return puts the caret back at the end of the fold's last line, in hidden
  text: an empty selection exactly there goes to the end of the folded line
  instead of opening the fold.
- **The copy** of a selection across a fold is `LargeFileUi::copyWhole`'s
  (item 22), which takes any selection holding invisible text.
- **Read the buffer with its hidden text** (`gtk_text_buffer_get_text` and
  `get_slice` with `TRUE`). Toggle Comment, the settings file's swatches
  and the LaTeX preview read it without, so with a fold above the caret
  Ctrl+/ commented the wrong line and the preview would have typeset the
  document without its folded sections; they read it whole now, and the
  `folding` suite presses Ctrl+/ below a fold.
- **A gutter draw outside an edit** (the regions arriving), with a
  5,000,000-character line on screen, left GTK drawing the view every frame
  for over ten seconds in `large_file` (its "drawn" wait timed out). The
  regions now draw the gutter only when they differ. Hovering the gutter
  there may do the same; not looked into (item 22's ground).
- **Frames.** GTK still visits every hidden line between the first and the
  last on screen when it draws (D8); the limit keeps that under about 15 ms
  a frame here. After Fold All or Unfold All of thousands of lines GTK
  measures them again in idle slices (0.5 to 0.7 s of CPU for 5,000 folds),
  never long at a time.
- **Not done**: the language server's foldingRange (`PARITY.md` 2.13); a
  fold whose first line is a long line's hidden part draws its placeholder
  where GTK puts that hidden position.

### 28. The folder's own tools and Format Document (done, November 2026; `PARITY.md` items 2.3 and 2.4)

What later items build on:
- **The folder's answer** (`src/FolderConsent.{h,cpp}`): "allow", "deny"
  or none for a window's folder (its resolved path), kept in
  `$XDG_STATE_HOME/minicode/folder-tools.json` (the core's
  `FolderTools::decode` and `encode`), the Mac's `MCFolderToolsConsent`.
  One answer covers the folder's Python environment for the servers, its
  `.venv` and `node_modules` for the formatters, a configuration that runs
  code, and rust-analyzer's and the TypeScript server's project code.
  `listen` tells every window of a change; the bar's × is `putOff`, for
  this launch only, and `forget` (Ask Again About This Folder's Tools,
  `win.forgetfoldertools`, on only while there is an answer) brings it back.
- **The servers** (`src/LspFolderTools.cpp`, LspSession's part): the one
  hook is in `serverForKey`, where a server is chosen. A Python server is
  looked for first beside the interpreter it gets
  (`serverPythonInterpreter`: python.interpreter, else
  `PythonEnvs::automatic` with the folder's own environment only once
  allowed) and is given it as on the Mac (`Lsp::pythonSettings` and
  `pythonInitializationOptions`; before this item the port gave none).
  rust-analyzer and the TypeScript server get `Lsp::checkProjectCode`'s
  options, or wait (no server, a note in the status bar). An answer given
  or taken back stops those three servers and attaches the file again.
  Not done: re-pointing a running Python server when another file's
  environment differs (the Mac's `applyPythonInterpreterForSwitch`), and
  Select Python Interpreter.
- **The bar** (`src/ConsentBar.{h,cpp}`, under the tab strip by
  `Editor::addBar`) asks what `folderEnvironmentAwaitingConsent` and
  `folderCodeAwaitingConsent` say, refreshed from LspSession's `onStatus`
  and the answer's listeners. Item 2.8's install offer belongs in the same
  bar, as the Mac's MCInstallBar does both.
- **Format Document** (`src/Formatting.{h,cpp}`, one per window,
  `app->format`): `win.formatdocument`, VS Code's Ctrl+Shift+I, enabled
  while the view shows a file's editable source (`updateTitle`). The
  decisions are the core's `FormatRun` (`src/FormatRun.{h,cpp}`, the Mac's
  MCFormatText in plain C++ with its words; the Mac keeps its own copy
  until it uses this one), run with the program on a thread of its own by
  `src/FormatRunner.{h,cpp}` (`g_spawn_async_with_pipes_and_fds`, a poll
  loop writing stdin and draining both outputs, SIGTERM at the limit and
  SIGKILL a second later; SIGPIPE is blocked on that thread and unblocked
  in the child). The output goes in as `FormatRun::Replacement`'s line runs
  through `MultiEdit::apply`, one user action. Folder questions are a
  GtkAlertDialog (Allow, Don't Allow, Cancel). Notes are a popover on the
  text view that never takes the keyboard (autohide off), closed by the
  caret moving, the next format or a tab switch; `lastNote`,
  `notesShown` and `questionsAsked` are for checks.
- **On save**: `act_save` asks `formatBeforeSaving` when format.on-save
  is on; Save All, auto-save and the close question's Save never format.
  Persistence's Host has `saveWaits` (auto-save leaves a tab whose Ctrl+S
  waits) and `onActiveWillChange` calls `tabLeaving` (a waiting Ctrl+S
  saves the tab as it is). editor.organize-imports-on-save is item 2.2's,
  with code actions.
- **The server fallback** (`src/LspFormatting.cpp`): only when nothing is
  planned (`Formatter::serverMayFormat`), the file's server if it offers
  formatting (`Lsp::routeFormatting`), 5 s, its edits through
  `MultiEdit::apply` on the file's buffer. **Item 2.1's workspace edit
  applier should take this over** (the Mac's goes through its applier).
  For it to ever run here, `documentChanged` now maps a file with
  `Settings::lspExtensions` as the Mac does: servers of the user's own
  (`lsp.<name>` with `lsp.<name>.extensions`) and an optional built-in one
  the settings name a command for (`lsp.markdown = ...`). The Mac also
  starts an optional one it finds installed, which this port does not yet.
- **`LspSearchDirs` and `LspFindProgram`** (Lsp.h) are where programs are
  looked for after PATH; FormatRunner uses them too.
- **For checks**: `MCGuiApp().consentBar` and `MCGuiApp().format`; the
  suites are `tools_consent`, `server_consent`, `format`,
  `format_on_save`, `format_consent` and `format_coverage`. The
  suites put their stand-in formatters in a folder of the scratch world
  first on PATH (`g_setenv`, put back at the end), so a formatter the
  machine has is never run.
- **Traps**: an alert closing leaves no window with the X focus (there is
  no window manager in the checks), so `gtk_widget_has_focus` on the text
  view stays false until the next key gives the window the focus; wait on
  the window's focus widget (`gtk_root_get_focus`) instead. GTK lays a
  freshly shown buffer's lines out in idle slices, which moves the line at
  the top as estimated heights turn real: a check of "the top line kept"
  waits for the layout to settle first. `lspgtk::Server` is defined in
  Lsp.cpp only, so code in another file reaches a server through
  LspSession members defined there (`clientOnShow`, `stopServers`).

## How to work on it

- **The GUI checks, first** (`linux/tests/gui`, `PARITY.md` item 0.1).
  From `linux/`, `meson test -C build --suite gui` runs every suite (about
  37 seconds in November 2026: 4 for the `keys` suite, 15 for the tab
  suites, most of it waiting out GTK's double-click time between clicks,
  and 15 for `sessions`, `backups` and `auto_save`, which start second apps); `./build/minicode-guitest save harness` runs
  some with their output as it comes, and `SUITES="save" meson test -C build
  --suite gui -v` does the same through Meson. Plain `meson test -C build`
  leaves them out. The program starts its own Xvfb on a display number the
  server picks, so agents can run it at the same moment, with a private
  session bus and a scratch home under `build/`; it refuses any other
  display, so nothing reaches the owner's screen and no permission is
  needed. Input is real: XTest from inside the process, so accelerators,
  gestures, and double-click counting run as for a person. It needs `xvfb`
  and `libxtst-dev`; without the second, Meson builds no checks and says
  so. `tests/gui/Harness.h` describes the API, which follows the Mac's
  `tests/gui/Harness.h` name for name. A new suite is a file,
  `tests/gui/<Name>Suite.cpp`, and a line in `tests/gui/suites.txt`; the
  app's windows, editor, tree, and terminal are reached through
  `tests/gui/AppSeam.h`, and a suite needing more of the app adds a field
  there. `MC_GUI_KEEP=1` keeps the scratch folders, `MC_GUI_VERBOSE=1` names
  each step, and `MC_GUI_SNAPSHOTS=<folder>` keeps pictures from
  `MCGuiSnapshot` and one of the window on top at each failed check. Two
  lines of noise are expected on every run, a libEGL "DRI3" warning and
  WebKit's "Can't connect to a11y bus". CI's GTK job runs every suite
  (`.github/workflows/ci.yml`, about 7 seconds there before the tab suites); when they fail, the
  run keeps those pictures and Meson's log as its `gui-checks` artifact
  (`gh run download <id> -n gui-checks`).

Two ways to run the app by hand, both described in `../CLAUDE.md`
("Verification reality"):

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
- **Real clicks and keys on the ThinkPad.** GNOME's Wayland session takes
  no synthetic input, but a private rootful Xwayland does:
  `Xwayland :87 -geometry 1300x850 -noreset` opens one window on the user's
  desktop holding its own X screen; run the app inside it with
  `DISPLAY=:87 GDK_BACKEND=x11` under `dbus-run-session`, and send input with
  XTest (`XTestFakeMotionEvent`, `XTestFakeButtonEvent`,
  `XTestFakeKeyEvent`, called from Python through `ctypes` on
  `libXtst.so.6`; no headers or xdotool needed). These are real X events,
  so gestures, double-click counting and accelerator matching run as for a
  person. Find widget positions from a hook (bounds in the surface plus
  `gdk_x11_surface_get_xid`, then `XTranslateCoordinates` to the root), and
  capture with `xwd -root` (the XWD format is simple to read with PIL; the
  capture shows stripes over translucent areas, so read colors from a
  widget render instead). There is no window manager, so every window opens
  at the top left, and a window presented later covers the others. Kill the
  Xwayland and the app's process group afterwards. This is what found three
  of the bugs the September hooks had missed.
  - **Kill by PID, never by name.** The user runs the packaged MiniCode
    (`/usr/bin/minicode`), so `pkill -x minicode` closes their own windows
    too, unsaved work included. That happened in October 2026. And never
    `pkill -f` with a pattern that appears in your own command line.
  - **Prefer `Xephyr :87 -screen 1150x760 -noreset -ac`** (with `DISPLAY=:0`)
    to the rootful Xwayland. Xwayland stopped delivering XTest pointer
    events to the app whenever the user's real mouse was outside its window
    (the X pointer moved, the app saw nothing), which makes a scroll or a
    click test pass or fail at random. Xephyr does not.
  - **Read the XWD's bits per pixel** from the header (offset 44). These
    servers write 24-bit pixels; decoding them as 32-bit squeezes the image
    to three quarters of its width and adds the stripes, so a point in the
    picture is not the point to click.

Per feature:
1. Put the logic in `../src` with tests, and run `make test`.
2. Build with `meson compile -C build` at `warning_level=2`, with zero
   warnings.
3. Drive the feature for real with a GUI suite in `tests/gui`, pressing
   real keys, and see it fail without the feature. Look at a snapshot where
   the look matters.
4. Update `BUILD-LINUX.md` ("What is verified") and this file.

Do not claim a GUI behaviour works when it was only compiled.

## Traps already known

- `gtk_list_view_scroll_to` with `GTK_LIST_SCROLL_FOCUS` moves the list's
  cursor, not the keyboard: Tab from the commit message into the list left
  the caret in the message until the list view was also given
  `gtk_widget_grab_focus`. A refresh selects without either flag, so it
  never pulls the keyboard out of the message box.
- The Docker image has no git; `apt-get install git` in the running
  container (not the image) is enough for the panel's checks.
- In a worktree-isolated agent session, shell commands whose text names git
  (even a file called GitModel.cpp, or `docker exec ... git`) are refused.
  Put the command in a script under the scratchpad, `docker cp` it into the
  container and run it by path.

- Selecting text takes the primary selection, and whoever had it drops its
  own selection (a `GtkText` and a `GtkTextBuffer` both do), and a buffer
  with a selection offers it again after every delete, an X round trip
  each. See item 20 before selecting from code or editing many ranges.
- Byte offsets versus character offsets: the core speaks UTF-8 bytes and
  `GtkTextIter` speaks characters. `Utf8Offsets.h` converts, with 47 tests;
  use it for every range that crosses over.
- A `.desktop` file and icons must be named after the application id
  (`org.minicode.Editor`), or the dock shows a second, generic icon.
- `apt` can install a runtime library without its `-dev` package, and Meson
  then quietly builds without that panel. CI checks `pkg-config` for this,
  poppler included.
- `GtkDirectoryList` with monitoring on reports no changes on GTK 4.22.4: a
  standalone program showed a `GFileMonitor` on the same folder receiving
  every create, rename and delete while the list emitted nothing. The tree
  therefore keeps each folder's list itself (`makeDirModel` in
  `FileTree.cpp`). Do not switch back without testing a create in an
  already-listed folder.
- Never patch a list model once per `GFileMonitor` event (R1 in
  `PARITY.md`, November 2026). GIO hands a burst of events over in one
  dispatch, and a `GtkSortListModel` costs about 0.25 ms a change in a
  folder of 30,000 entries (0.01 ms without it), so deleting the files of an
  open folder held the main loop for two minutes and GNOME asked whether to
  force quit. Each folder's store is now kept sorted by the tree itself and
  an event only asks for a listing on a worker thread, which goes in as
  splices a few milliseconds a turn. GtkTreeExpander asks every folder row
  on screen whether it can expand, which makes its child model, so every
  folder row shown has a monitor and a listing, open or not. The
  `tree_stall` suite times the main thread's longest gap per phase.
- Rows move under the pointer when a listing comes in, so the tree keeps
  the row a click was pressed on (and the last click's row) as its
  `GtkTreeListRow`, never a position; a row that stays keeps its object.
- A list view maps a new row's widgets when the row is added and places
  them at the next frame. In between, a GUI check that finds the row's
  label and clicks it hits the window's top left corner. A row scrolled
  just out of the view also stays mapped and placed. Find click targets
  with `MCGuiFindLabel` or `MCGuiOnScreen` (mapped, with a size, and what
  GTK picks at its center), never `gtk_widget_get_mapped` alone;
  `tree_clicks` checks it. A row put in above the top row on show stays
  above the view (GTK keeps the rows on show still), so scroll a list only
  once the row is in it.
- `g_file_trash` refuses anything on a tmpfs such as /tmp ("Trashing on system
  internal mounts is not supported"). A test of Move to Trash needs its
  scratch project on the home filesystem, with `XDG_DATA_HOME` pointed at a
  scratch folder there so nothing reaches the user's real Trash.
- A test launch on the ThinkPad should run under `dbus-run-session`. The app
  is a single-instance GApplication, so a plain launch would hand its
  arguments to whatever MiniCode is already running, the user's or another
  session's, and exit. Since R2 (item 23), add `--foreground` when it is
  typed in a terminal: otherwise the command returns once the window is up,
  `dbus-run-session` then stops its bus, and the app, its session bus gone,
  quits within a second (seen in the `detach` suite's world).
- Pages of a `PdfView` that have never been on screen may not be allocated
  yet, and then `gtk_widget_compute_point` maps every page to the same place.
  A real click only reaches pages that are drawn, but a test that clicks
  page 2 must scroll it into view and wait until `pagePointIn` and
  `pageAtPoint` round-trip, or it clicks page 1. That produced 137 "wrong"
  clicks in the first in-app sweep, none of them real.
- A test launch that is not the user's instance can also change the
  application id for the run, since a second `org.minicode.Editor` hands off
  to the first. The LaTeX test used a temporary `MINICODE_APP_ID` read in
  `main()`, removed with the rest of the hook.
- An alert from `gtk_alert_dialog_show` (no buttons of our own) did not close
  when its Close button was activated from a test hook; the error alerts now
  set an OK button and use `gtk_alert_dialog_choose`, the same path as
  "Save changes?".
- `timeout -s KILL 60 dbus-run-session -- minicode` kills only
  `dbus-run-session`: MiniCode and the private `dbus-daemon` keep running.
  End a test run from inside (the hook activates `app.quit`), or find and
  kill those two PIDs yourself.
- The color chooser saves the colors it has seen as custom colors in
  GSettings (`org.gtk.gtk4.Settings.ColorChooser`), and the old color dialog
  did the same. A test that picks colors should run with
  `GSETTINGS_BACKEND=memory`, or it writes to the user's dconf.
- A new command (November 2026, `PARITY.md` item 0.3, which replaced
  `kBinds`, `buildMenu`'s hand-written menus and `shellOwns`; the older
  items below still name them): a row in `../src/CommandsLinux.cpp` (the
  registry's id, the Linux keys by D1's rule, a Linux title or menu where
  the Mac's do not fit), a row in `Keys.cpp`'s `actions()` naming its
  GAction, the action itself (`addAction` in `main.cpp`), a row in
  `hintsText` if the hints should list it (its key comes from
  `Keys::label`), and its README row (`commands:linux-readme` fails
  without it). The menu bar and the accelerators follow from the tables,
  and the `keys` GUI suite fails when the two disagree. A plain Ctrl key or
  a key with no modifier goes to the program while the terminal has the
  keyboard without anything more; a key for moving between panes gets the
  `Global` flag in its row (`Commands::linuxTerminalTakes`).
- VTE numbers rows two ways. `vte_terminal_get_text_range_format` and
  `vte_terminal_get_cursor_position` count from the start of the buffer;
  the vertical adjustment counts from the oldest row still kept. They part
  after `clear` or 10,000 lines of output. Use `Terminal::topRow`, and check
  anything that maps pixels to rows against `vte_terminal_check_match_at`,
  which is VTE's own hit test. Rows VTE does not have read back as empty
  lines, not as nothing, so they cannot be used to find the first kept row.
- In a test on the ThinkPad, `cat` may be the user's alias for `bat`, which
  adds a gutter and wraps lines itself. Use `command cat`.
- A window's objects are deleted while its widgets still exist, from
  `window-removed`. Anything new that connects a signal to a window's object
  with the object as data is covered for widgets and their controllers by
  `disconnectOwners`, once the object is in the owners list in
  `onWindowRemoved`; a connection to anything else (a buffer, a model, a
  monitor, the application) must be undone in the object's destructor.
- Do not draw big bitmaps with cairo in a `GtkDrawingArea`. Its drawing is
  recorded into a cairo recording surface, which keeps a copy-on-write
  snapshot of any image painted from; every widget keeps its last render
  node, so when the image is destroyed, cairo copies it into that node, and
  the copy lives as long as the node. Scrolling a 400% PDF grew the heap to
  1.5 GB while the view held 173 MB of bitmaps (`malloc_trim` gave nothing
  back, since it was all in use). `PdfView` now draws `GdkTexture`s, which are
  shared by reference, and queues a redraw of a page whose bitmap it drops,
  so the node lets go too.
- GtkAdjustment emits `changed` when notifications are thawed, and
  GtkViewport thaws them after it has placed its child. A value set from a
  `changed` handler therefore misses the allocation in progress, and the
  re-allocation it asks for can be lost: a zoom from 40% to 260% left the
  pages 485 pixels to one side of where the adjustment said, found by
  comparing a page's real position with the scroll value. `PdfView` raises
  the adjustments' upper bound to the new layout's size itself before
  setting the value, and queues the viewport's allocation from an idle when
  it has to set one from `changed`.
- An application stylesheet outranks the desktop theme whatever the
  selectors, so a rule that clears a widget's background also clears the
  theme's `:selected` and `:hover` colors on it. The tree's rows lost their
  selection color this way; state the colors for those states yourself.
- An accelerator with Shift on a punctuation key, such as
  `<Ctrl><Shift>period`, never fires: GTK matches the keyval the key
  produced (`greater`) and counts Shift as used up by it. Write the
  character instead (`<Ctrl>greater`); it then depends on the layout.
  `Commands::linuxAccelerators` does this for every key with Shift and a
  digit or punctuation, and the terminal's rule reads the registry's key
  (`ctrl+shift+.`), which has its Shift.
- `gtk_list_view_scroll_to` with `GTK_LIST_SCROLL_FOCUS` moves the list's
  own focus item but does not take the keyboard from another widget; call
  `gtk_widget_grab_focus` on the list as well.
- A scroll to a line of a freshly loaded buffer can end in the wrong place
  (see item 5, `settleOnCaret`). Check that the line is inside
  `gtk_text_view_get_visible_rect`, not just where the caret is.
- A hook that calls the open path directly proves the wiring, not the
  click. The tree's missing selection, the Find in Folder scroll and the
  dead Down key all passed such hooks and failed for the user.
- A test of a click on a PDF page must check `pageAtPoint`'s return value:
  at a low zoom a point that was on the page before can be in the margin
  after, and the outputs are then left as they were.
- The editor reads text only from a regular file of at most 256 MB (16 MB
  until item 1.12), and checks both before opening it
  (`Document::readText`): opening a named pipe blocks until a writer comes,
  and every window shares one process, so either a pipe or a multi-gigabyte
  file used to freeze or kill all of them, unsaved edits included. Files
  over 4 MB are read on a worker thread (item 22). Android stops at 4 MB.
- Saves are an atomic rename, except for a file with another hard link, one
  owned by someone else, or one in a folder we cannot write: those are
  written in place (`writeInPlace`, space reserved first), since the rename
  would cut the link or take the file from its owner. A link to nothing is
  refused rather than replaced by a regular file.
- A widget with a real minimum height in the editor's slot makes GTK warn
  "Trying to measure GtkBox ... for height of 46, but it needs at least 64":
  the vertical split may shrink the editor, so it measures the editor's side
  at small heights, and the box above it (`upperBox`) is then asked for less
  than its children need. The text view never showed this because its
  scroller asks for nothing. The media pages sit in scrolled windows for
  this reason, and the media stack is not homogeneous, or a hidden page's
  minimum counts too. The culprit was found by running under gdb with
  `G_DEBUG=fatal-warnings`, reading the widget's address from the warning
  and walking `gtk_widget_get_parent` with `g_type_name_from_instance`.
  Since item 18 the vertical split is `WindowLayout`'s, which never gives
  the editor's side less than its minimum; a slot page with a large minimum
  would now raise the window's smallest height instead, so keep them in
  scrolled windows still.
- GTK 4.22 plays through GstPlay, which wraps every GStreamer error in its
  own domain (`gst-play-error-quark`, code 1) with the text "Error from
  element <path>: <message>\n<message>\n<debug>". The original code
  (codec not found, missing plugin) is lost, so a missing decoder can only
  be told from the text; the debug line, "No suitable plugins found", is
  never translated.
- Each video opened leaves one idle `gstglcontext` and one
  `gldisplay-event` thread in the process, in the container (X11,
  llvmpipe). A standalone GtkVideo program leaks the same, even reusing one
  GtkMediaFile, so it is upstream; GStreamer's streaming threads, which also
  stay after a stream ends, are only its thread pool and go within 20 s.
  Check the ThinkPad before chasing it further.
- GStreamer reads its plugin registry once per process: a decoder installed
  while MiniCode runs is not used until it restarts.
- The dev image has none of the tools for making test clips.
  `apt-get install gstreamer1.0-tools gstreamer1.0-plugins-ugly
  gstreamer1.0-libav` in the running container (x264enc is in ugly, the AAC
  encoder in libav), then `apt-get remove` the last two to see what a stock
  desktop plays. `h264parse` and `timeoverlay` are in packages the image
  lacks; mp4mux takes x264enc's output directly, and `videotestsrc
  pattern=smpte horizontal-speed=8` gives frames that visibly differ.

- `disconnectOwners` must never be given a null owner: disconnecting by null
  data strips handlers GTK connected itself. A window that never opened a
  `.tex` has no LaTeX preview, and its pointer is null.

GUI checks (`tests/gui`):

- There is no window manager on the checks' display. A window becomes
  active (`gtk_window_is_active`) only when it has the X focus, which the
  harness gives the key window before every key (`MCGuiSetKeyWindow`, the
  newest app window by default). Every window opens at the top left, and an
  alert covers the window under it; `MCGuiTopWindow` finds it.
- The private session bus activates nothing, so no portal runs in the
  checks. Anything that can go through `xdg-desktop-portal` on the owner's
  desktop (a file dialog, a link opened outside the app) behaves there as on
  a desktop without one; check it on the desktop as well. Under the
  standard bus the first run started xdg-desktop-portal and at-spi, with a
  page of warnings.
- A drag sent as one burst (press, motion, release) selected nothing in the
  text view in 3 runs of 8 when it came right after a double-click; GDK
  merges motion events that arrive together. `MCGuiDrag` moves over several
  turns of the main loop, and the runner waits for it.
- GtkTextView undoes typing a piece at a time: one Ctrl+Z after typing a
  line takes back its last word or so, not the line.
- **Input that gives up the PRIMARY selection and takes it again, sent in
  one burst** (2026-11-10, `large_file` on CI one run in two): a click in a
  field whose text is selected, then Ctrl+A. GTK handles both within the
  same millisecond of the X server's time, and X's notice that the first
  was given up (a SelectionClear and an XFixes notice for owner None) comes
  after the second claim with the same time on it. GDK ignores only notices
  older than its claim, so it takes this one for another program taking the
  selection, and the `GtkText` drops Ctrl+A's selection to its start: the
  query typed next went in before the old one ("NEEDLEneedle"). Give up and
  take again in one step from code and the selection was lost in 4 rounds
  of 12; with a wait between, in none of 50. Wait for the click to land (no
  selection in the field) before Ctrl+A. A person cannot press keys that
  close together.
- GTK 4.18 deprecated its X11 API. `tests/gui/Input.cpp` is the only file
  that uses it and defines `GDK_DISABLE_DEPRECATION_WARNINGS`.
- In the checks the browser pane's home page is `about:blank`
  (`MINICODE_BROWSER_HOME`), so a window opening never reaches the network.
  `Browser`'s constructor loads it as an address: through `Browser::load`,
  which takes what is typed into the bar, it became a web search for
  "about:blank" until November 2026. The `harness` suite checks the page's
  address, that it answers JavaScript, and that its process runs inside
  WebKit's sandbox (a `bwrap` between it and the app in `/proc`).
- WebKit's sandbox starts on the ThinkPad and on CI's runner: both restrict
  unprivileged user namespaces through AppArmor, and both load the apparmor
  package's `bwrap-userns-restrict` profile, which lets `bwrap` make one.
  Where it cannot start (a Docker container, which needs
  `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1`), the `harness` suite fails
  its two sandbox checks rather than passing with a blank browser pane.
