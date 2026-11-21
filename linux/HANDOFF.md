# Linux port: handoff, September 2026

The work from here is in `PARITY.md`: its stages, rules, and decisions come
first. The items below are done, and stay for how each was built and what it
cost.

The GTK4 port under `linux/` was last worked on in August 2026. Since then the
macOS app has gained its biggest features: the LaTeX preview, the LSP client,
incremental highlighting, image and PDF viewing, and several file tree
actions. The Android port has caught up on most of them. This document
listed what Linux lacked then, in the order to do it, with where each thing
lives on the Mac and what the GTK equivalent is.

Read these first:
- `../BUILD-LINUX.md`: build, install, what was verified, and what still
  needs a person at the desktop.
- `dev/README.md`: the Docker container with a virtual display for driving
  the app from a Mac.
- `../CLAUDE.md` and its area notes in `../docs/notes/`, especially
  `latex.md`, `lsp.md` and `viewers.md` ("Images"), with each file's "Traps".
  The design decisions there apply here too.
- `../android/README.md`: the Android port solved the same problems more
  recently, without AppKit, so it is often the closer model.

## Where it stands

Items 1 to 14 below are done (September and October 2026), so the port
has what the Mac had then, apart from what the list at the end of item 8
leaves out. Stage 0 and Stage 1 of `PARITY.md`, and the owner's reports R1
to R3, came in November 2026 (items 15 to 27). Almost everything else the
Mac gained from 1.5.0 on (rename and the other language server commands,
formatting, the problems list, the split editor, several terminals, the
viewers, the agent features and the command line control) is not here
yet; `PARITY.md`'s Stages 2 to 4 are the plan, and `../ROADMAP.md`, "Linux
parity", lists the core files that already hold its logic. It builds
without warnings at `warning_level=2` (CI: Ubuntu, GTK 4, VTE, WebKitGTK,
poppler, and the GUI suites on Xvfb). The work up to October was checked by
hand and with temporary hooks, much of it under GNOME on Wayland on Ubuntu
26.04; the November work is checked by the GUI suites, and what they leave
to a person at the desktop is in `../BUILD-LINUX.md`, "What needs a
person". The port has:
- data safety: "Save changes?" before anything replaces edits, failed saves
  reported, atomic saves (item 1);
- the file tree, live-refreshing, with New File, New Folder, Rename, Move to
  Trash, Open Containing Folder and Copy Path (item 2), rows that drag onto
  folders (item 13), and listings that no longer hold the window while a
  folder churns (`PARITY.md` R1);
- the editor with incremental syntax highlighting (item 3), Ctrl+/, the
  Markdown preview, and reloading when the file changes on disk (item 8);
- the menu bar and keys from the command registry, changed by keys.conf
  (`PARITY.md` item 0.3);
- tabs, with preview tabs, Reopen Closed Tab and Save All (item 15, which
  replaced Previous File), a folder's tabs coming back, backups of unsaved
  tabs, and auto-save (item 17);
- line numbers and the gutter (item 16), and folding, with at most 5,000
  lines hidden in a tab (item 27);
- the command palette, with Quick Open, Go to Line and the open file's
  symbols from minicode's own outline (item 19);
- Markdown task lists: boxes to click in the preview, Ctrl+Alt+L, Return
  continuing a list, the count in the title, and the TODOs window on
  Ctrl+Shift+D (item 14);
- images and PDFs in the editor's slot (item 4), with zoom for PDFs and the
  LaTeX preview;
- Find in Folder (item 5) with Replace in Folder, and the find and replace
  bar in the file with VS Code's options (item 20);
- brackets boxed beside the caret and colored by depth, Go to Bracket, and
  brackets and quotes that close as they are typed (item 21);
- several carets and the line commands (item 26);
- the editor's and the terminals' zoom, word wrap, Go Back and Forward, and
  Maximize Pane (item 25);
- large-file mode, files over 4 MB read without holding the window, and
  lines over 10,000 characters shown a part at a time (item 22);
- language servers: squiggles, completion, hover and go to definition
  (item 6), every tab's file open on its server, and rename, references and
  quick fixes (item 28);
- Format Document, by the project's formatter or the file's server, and a
  folder's own tools (its `.venv`, `node_modules` and code a server would
  run) used only once the folder's answer allows them (item 29);
- the LaTeX preview with double-click editing and Export PDF (item 7);
- several windows, each with its own tree, editor, terminal, browser, search
  panel and language servers; Focus Editor, Close Window and Quit (item 8);
- a VTE terminal that gets its own Ctrl keys while it has the keyboard, with
  Ctrl+click on a file:line or URL in its output (item 9), and a WebKitGTK
  browser, beside the editor with the terminal below both, as the Mac lays
  them out, each hidden on its own and its bars dragged (item 18);
- the Source Control panel with the commit graph, in the tree's place on
  Ctrl+Shift+G (item 10), which pushes, pulls, fetches in the background,
  stages and discards whole files, hunks and lines, switches branches, and
  amends, undoes and stashes (item 30);
- video and audio in the editor's slot through GTK's GStreamer backend,
  with Play/Pause on Ctrl+Shift+Space (item 11);
- the settings file with live colors, per-panel opacity and a live color
  picker (item 8);
- the shortcut hints panel, and opening a file or folder named on the
  command line, where `minicode` gives the terminal back once its window is
  up (item 23); the terminal and the browser are made when first shown, so
  the window comes sooner (item 24).

It shares from `../src` `SyntaxHighlighter`, `MarkdownParser`, `MarkdownEdit`,
`MarkdownTasks`, `Settings`, `LineComments`, `AutoIndent`, `KeepSorted`,
`FolderSearch`, `TextFind`, `LatexDoc`, `SyncTex`, `Json`, `LspClient`,
`TermLinks`, `GitStatus`, `GitGraph`, `GitModel`, `BracketMatch`, `BracketPairs`,
`AutoClose` and `MultiCaret`, and since November the command registry
(`Commands`, `CommandsLinux`), `EditorTabs`, `TabSessions`, `Backups`,
`LineIndex`, `CommandPalette`, `SymbolOutline`, `FindRules`, `EditSpans`,
`EditBatch`, `Folding`, `NavHistory`, `NavRecorder`, `ViewZoom`,
`RightAreaLayout` and others; `meson.build`'s `core_sources` is the list. It
and the plain Makefile's `CORE` must name the same files: CI builds only with
Meson, so the Makefile once fell three files behind and could not link, and
`make test`'s `linux:build-lists` check now compares the two.
The rest of the core (`TerminalScreen`) is portable and tested, and has not
been added to `meson.build`, since VTE does that job here.

What each item verified at run time is in `../BUILD-LINUX.md`, "What is
verified", and what still needs a person (mostly real key presses, input
methods, and how things look) in its "What needs a person".

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
in the tree's right-click menu and the File menu (`mac/src/EditorController.mm`).
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
"Images" bullet of `../docs/notes/viewers.md`. Linux shows "cannot display" (the
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

Mac: `mac/src/Search.mm`. The search is now `../src/FolderSearch.{h,cpp}`, a pure
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

Mac: `mac/src/Lsp.mm` over `../src/LspClient.cpp`, described in the "LSP"
notes, `../docs/notes/lsp.md`. The GTK side is `src/Lsp.{h,cpp}` over the same
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

Mac: `mac/src/Latex.mm`; read the whole of `../docs/notes/latex.md`
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
as Command+click does on the Mac (`mac/src/Terminal.mm`, `TerminalGridView.mm`,
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

Mac: `mac/src/GitPanel.mm` and "Git panel" in `../docs/notes/git.md`. GTK:
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

**Followed since (2026-11-10, item 30 below): everything in the next two
paragraphs, and push, pull, autofetch and the commit verbs.** As written
before then: **Not followed yet (2026-11-07): Stage All, Discard, the file
menu and the branch picker.** The Mac's panel gained them (round 4, W19); the rules are
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
- **The title** says "minicode — TODO.md * — 3 of 7 done" while a Markdown
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
  `GtkTextBuffer` over the editor's one tag table, its undo history (the
  core's `UndoHistory` since item 72; GTK's undo until then), the
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
  (`settleOnPlace`; its two seconds count from the first frame with the
  view on screen: counted from the call, a session's tab in a window slow
  to open, or a tab shown with the editor's pane hidden, came back at its
  top); `gtk_text_view_scroll_to_mark` lands short on a fresh
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
- **Trap (the nightly soak, 2026-11-18): the scroll offset outlives a
  buffer too.** `gtk_text_view_set_buffer` keeps the view's offset and the
  adjustments' values. Far down one file, GTK set the adjustment to 0 for
  the next before laying it out, took the unlaid buffer's last line (lines
  have no height until laid out) for the view's first, and once laid out
  moved the offset after it, which the adjustment clamped back to the 0 it
  had, so no value-changed put the two back in step: a short file showed
  its empty last line or nothing, the gutter numbering that line. GTK keeps
  the view's offset at value minus the top margin; a visible rect that
  disagrees is this. `CodeView::setBuffer` now puts both adjustments at 0
  while the old text is still laid out, then changes the buffer; tab places
  come back after it as before. `gutter_switch` checks it (all four rounds
  failed without it). The scroll goes before the cache is emptied, never
  after: scrolling asks for the caret's place, which lays the caret's line
  out into the cache again, and a line with no height yet (the text just
  replaced) kept its display through set_buffer (#165 did it after, and
  `semantic_tokens` crashed main's GTK job twice; item 122). The cache is
  now emptied again after set_buffer as well.

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
- **Trap (November 2026): a GTK question does not stop the main loop.**
  The Mac's alerts run modally, and its timers wait; `gtk_alert_dialog_choose`
  returns at once, and every timer keeps firing under the question. Keep
  Mine or Reload counts the version asked about as the file's when it is
  shown (so it is not asked about twice), and after-delay's auto-save then
  wrote the tab's edits over that very change before the click; Reload read
  them back. The tab being asked about is now `Editor::askingAboutDisk`
  until the shell calls `diskQuestionAnswered`, and auto-save skips it.
  Anything else a timer does to a document while a question about it is
  up needs the same care.
- **Trap (November 2026): an idle at the default priority runs after the
  input already queued.** Focus-change's save ran from `g_idle_add`, and
  every key waiting in GDK's queue (X events are dispatched at the default
  priority) went first: a key typed right after Ctrl+PageUp was written
  with the tab it went into, as if that tab had lost the keyboard too.
  Work that must see the state of the moment it was asked for goes at
  `G_PRIORITY_HIGH`.

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
  widgets. A pick list of a caller's own rows (the Mac's `MCPalettePick`)
  is `showPick` (item 30), a mode beside the four, which the branch picker
  uses.
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
  - GTK replayed an undo step edit by edit with no user action around it,
    so the highlighter followed each of 40,000 edits alone (19 s), and even
    taken as one action the undo of Replace All of 20,000 matches cost
    about 1.6 s, GTK placing the caret after each edit it put back. Since
    item 72 an undo is the history's plan put in as one `MultiEdit::apply`,
    as the Replace All itself was.
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
  composing. The warning's stack (`G_DEBUG=fatal-criticals`, 2026-11-11) is
  `gtk_text_view_get_cursor_locations` from GtkTextView's own idle, none of
  ours, and only `auto_close`'s dead-key step raises it (green runs log it
  too); it is not the palette's, whose `git_actions` flake did not repeat in
  a whole-order run or under load. A tab closed before the view has drawn it (open a file, then
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

### 28. The language server session's parts, every tab open, and rename, references and quick fixes (done, November 2026; `PARITY.md` items 2.0 and 2.1)

What later items build on:
- **The session is split by concern**, as the Mac's `LspSession` is split
  into `Lsp+<Concern>.mm` categories. `Lsp.h` is the one class, its methods
  grouped by the file that has them; `LspSessionPrivate.h` holds what the
  parts share (`lspui::` helpers: `realPath`, `uriOf`, `canonicalUri`,
  `lineOf`, `byteColumn`, `bufferText`) and **each part's state in a struct
  of its own** (`SyncState`, `DiagState`, `CompletionState`, `NavState`),
  held by the class as a `unique_ptr`, which no other part touches: a part
  reaches another's through a method (`detachDocument` asks each to forget
  what it holds for the file leaving the view). State two parts use stays
  in the class. The files: `Lsp.cpp` (made and destroyed, the buffer on
  show, positions, the view's key controller, the popovers' teardown),
  `LspProcess.{h,cpp}` (one server process, `lspgtk::Server`, moved
  unchanged), `LspSessionServers.cpp` (which server, starting, exits,
  stopping, the status bar; `launchServer` is the one place a program is
  chosen and started, where item 2.3's consent hooks in),
  `LspSessionSync.cpp` (documents), `LspSessionDiagnostics.cpp`,
  `LspSessionCompletion.cpp`, `LspSessionNavigation.cpp` (hover and
  definition). **A later item adds a file of its own** (2.2's Problems and
  second server, 2.5's completion edits in `LspSessionCompletion.cpp`, 2.6
  semantic tokens, 2.7 inlay hints, 2.12 symbols in
  `LspSessionNavigation.cpp`, 2.13), a struct for its state in
  `LspSessionPrivate.h`, a `unique_ptr` and its methods in `Lsp.h`, and its
  file to both build lists. The names carry `LspSession` so they never
  read as the core's `../src/Lsp*.cpp`.
- **Every tab's file is open on its server** (`syncOpenDocuments`, the
  Mac's): every tab that is read text with a path (not a tab still waiting
  to be read, nor a large file with its features off), whose language has
  a server running, is opened there with its own buffer's text, once;
  files whose tabs went, or whose path or server changed, are closed.
  Only a running server takes a tab: the file on show is what starts one
  (`documentChanged`), and a server just started is given every tab of its
  language. It runs on show, when the tabs change (`EditorObserver::
  tabsChanged`, from `Editor::refreshStrip`, in an idle), and should run
  before any request about other files (rename and references do).
  Leaving a tab no longer closes it on the server, and its diagnostics stay
  (`DiagState::byUri`, with the text they were computed on), drawn when it
  is back on show, placed on its text as it is then
  (`Lsp::diagnosticSpans`: one about text typed over since is left out).
- **Edits follow each buffer**, not only the one on show: the session
  watches each open document's buffer (`insert-text` and `delete-range`,
  before the change) into a `Lsp::ChangeTracker` per document and server,
  and 0.3 s after the last edit sends `didChange` for each document edited,
  ranged where the server takes ranges (the core decides; a change whose
  ranges cost as much as the text goes whole, so small files always do).
  The buffer counts characters and the server UTF-16 units: the tracker
  keeps characters and the client turns them into units through its copy
  of the text (`Model::Text`'s code points, 2026-11-16; until then a
  character outside the Basic Multilingual Plane made that document send
  its whole text until one was sent without any). So a
  background tab's edit (Replace in Folder, the applier) reaches the
  server too. `EditorObserver::documentSaved(Document*)` now comes for
  every tab saved (Save All, auto-save), with `didSave`.
- **For checks**: `LspSession::openDocuments()` ("<server key> <path>"),
  `rangedChangesSent()`, `wholeChangesSent()`; `tests/gui/FakeLspLog.h`
  reads the scripted server's log: every message, a file's text as the
  server has it after the client's didOpen, didChange and didClose
  (`MCGuiFakeLspText`), and its ranged changes. The scripted server's
  default capabilities are full sync; a check of ranged sync says
  `"change": 2` itself.
- **Traps**: a file shorter than about 100 characters per change is always
  sent whole (each ranged entry costs 96 in `Client::didChange`'s
  reckoning), so a check of ranges needs longer files. In a whole GUI run
  the window may not be active when a suite starts (the suite before it
  started another program): `MCGuiFocusKeyWindow()` before checking
  `gtk_widget_has_focus`.

Item 2.1 (`LspSessionActions.cpp`, `LspSessionEdits.cpp`, `LspPicker.{h,cpp}`):
- **The workspace edit applier** is `LspSession::applyWorkspaceEdit(we,
  &failure)` (public, for Format Document's server fallback in 2.4 and
  anything else with a `Lsp::WorkspaceEdit`), over the private `applyEdit(we,
  server, asked, &failure)`, which also refuses a file that moved on since
  the server computed the edit (`Lsp::staleEditFile` against the
  versions when it was asked). Every file or none: each read from its tab
  (on show or not) or the disk; refused when another window's tab has
  unsaved changes to it; checked against the text the server's positions
  are for (`Client::textFor`, else the disk's) with the core's
  `Lsp::editsHold`; then every file without a tab opened in a kept tab
  (`OpenFrom::Other`), each checked to be text to edit (not a preview: a
  Markdown file opens in its page, which the Mac also refuses) holding that
  text; then each file's edits as one `MultiEdit::apply` on its own buffer
  (`Lsp::pointEdits` turns the UTF-16 edits into code points), one undo
  step per file, background tabs included without showing them. A failure
  part way undoes the files edited (`Document::undo`) and closes the
  tabs it opened; the tab on show is put back on show; nothing is saved.
  `applyingEdit_` is set meanwhile, so the tabs it opens do not close the
  lists or forget the rename, and `MultiEdit::applying()` keeps completion
  from taking the edits for typing.
- **Rename** (F2): `prepareRename` when the server offers it, its range and
  placeholder; the box (`lspgtk::RenameBox`, an autohide popover with an
  entry, which takes the keyboard and gives it back to the view when it
  closes); the request after every tab's text is flushed and synced, with
  the versions as asked; the answer applied in an idle (`later`, as the
  Mac's dispatch_async: applying switches tabs, which talks to the server
  mid-answer otherwise), with "Renamed N occurrences in M files".
- **References** (Shift+F12) and **Quick Fix** (Ctrl+.) share
  `lspgtk::Picker`, a popover that never takes the keyboard, as the
  completion list: the view's capture key controller (`onKey`) hands it Up,
  Down, Page Up, Page Down, Enter and Escape (`pickerKey`); a click on a row
  takes it; the caret moving, typing, or the view losing the keyboard closes
  it. A reference in another file opens through `onReveal` (a Go Back jump,
  as the definition), in the same file through the editor's reveal. Quick
  Fix asks with `Lsp::quickFixRange` (the selection, or the caret's line's
  first diagnostic when none touches the caret) and the stored diagnostics
  there as the context; an action without its edit is resolved first, then
  its edit applied, then its command run. `workspace/applyEdit` from a
  server (`Client::onApplyEdit`, set in `serverForKey`) goes through the
  same applier in an idle and is answered whether it went in. Its steps
  are named "Edit" (item 72 gave the port named steps; the applier takes
  no name yet).
- **For checks**: `lastNote()`, `pickerKind()`, `pickerRows()`,
  `pickerSelectedRow()`, `pickerRowWidget(row)`, `renameBoxVisible()`,
  `renameEntry()`; the scripted server's `${version:name-1}` makes an edit
  for an older version, and `${uri:/absolute/path}` names a file it does not
  have open.
- **Not done here**: Organize Imports and the second server's merged code
  actions (item 2.2).
- **Traps**: the scripted server answers `codeAction/resolve` by its first
  matching rule, so a rule for one action needs a `when`, or it resolves
  every action without an edit (a command action too) into that edit.

### 29. The folder's own tools and Format Document (done, November 2026; `PARITY.md` items 2.3 and 2.4)

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
- **The servers** (`src/LspSessionFolderTools.cpp`, LspSession's part, its
  state in `FolderToolsState`, item 28's shape): the one hook is in
  `launchServer` (`LspSessionServers.cpp`), where a server's program is
  chosen and started. A Python server is
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
- **The server fallback** (`src/LspSessionFormatting.cpp`): only when
  nothing is planned (`Formatter::serverMayFormat`), the file's server if
  it offers formatting (`Lsp::routeFormatting`), 5 s, its edits through
  item 28's workspace edit applier (`applyEdit`, with the server's
  document versions as asked), as the Mac's go through its applier.
  For it to ever run here, `languageOf` (`LspSessionServers.cpp`, which the
  file on show and every tab go through) now maps a file with
  `Settings::lspExtensions` as the Mac does: servers of the user's own
  (`lsp.<name>` with `lsp.<name>.extensions`) and an optional built-in one
  the settings name a command for (`lsp.markdown = ...`). The Mac also
  starts an optional one it finds installed, which this port does not yet.
- **`LspSearchDirs` and `LspFindProgram`** (Lsp.h, in `LspProcess.cpp`)
  are where programs are looked for after PATH; FormatRunner uses them too.
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
  waits for the layout to settle first.

### 30. The Source Control panel's verbs (done, November 2026; `PARITY.md` item 2.9)

The Mac's View > Source Control in the GTK panel, rule for rule and word
for word from the core's `GitUi` plans (`../src/GitModel.h`): Fetch, Pull,
Push and Sync with the Sync Changes or Publish Branch button under Commit,
and autofetch; Stage All and Unstage All; Discard Changes with the file as
it was in the Trash, and Undo Discard (the last 20); the hunk and line
actions in a file's diff; the branch picker; Commit and Amend, Undo Last
Commit, Stash and Pop Stash; several rows at once. What later items build
on:
- **The palette's pick list** (`PalettePanel::showPick(const Pick&)`, the
  Mac's `MCPalettePick`): a list of the caller's own rows in place of the
  modes. `Pick` holds `placeholder` (the field's hint), `labels` (matched
  by the palette's own rules, the whole text the query, no prefixes),
  `details` (muted at the right, may be fewer), `choose(index)`, an
  optional `typedRow(text)` that makes a last row from what is typed ("+
  Create branch “x” from main", "" for none) with `chooseTyped(text)`, and
  `emptyText`. It always opens afresh (an open palette or pick list closes
  first, putting the keyboard back), and a later `show()` closes an open
  pick list; the keys, the click, Escape and putting the keyboard back are
  the palette's, and `choose` runs on the turn after it closed, so a
  command it starts acts where the keyboard was. `isPick()` says one is
  up. Another chooser (an interpreter, a server) is another `Pick`.
- **The panel is made with its window** (`makeGitPanel` in `main.cpp`), as
  the Mac's `ensureGitPanel`, so autofetch's 30-second tick and the
  commands work before it is first shown; its widget joins the sidebar's
  stack only at the first Ctrl+Shift+G, which keeps the sidebar's minimum
  as it was at launch. The window's commands are `win.gitfetch` and the
  rest, added by `GitPanel::installActions`; `updateActions` (from the
  tab switch and focus hooks) keeps Undo Discard enabled only with a
  discard to undo and Discard Changes only with a file on show or a row
  chosen in the list while it has the keyboard, so the palette offers them
  only then. Each command reads the status anew on the git thread, so it
  acts on the repository as it is; asks with a `GtkAlertDialog` (the plan's
  button first and the default, Cancel the Escape); and says what happened
  under the button and, while the panel is hidden, in the status bar
  (`onNote`).
- **Files**: `GitRun.{h,cpp}` runs git (moved from `GitPanel.cpp`), with
  an input on stdin for `git apply` and, for fetch, pull and push
  (`Git::talksToRemote`), `setsid` in the child instead of `setpgid`, as
  the Mac's `POSIX_SPAWN_SETSID`: ssh then has no terminal to ask on and
  fails at once, as git does for a password with `GIT_TERMINAL_PROMPT=0`.
  A GUI askpass (`SSH_ASKPASS`) is left as the desktop sets it.
  `GitTrash.{h,cpp}` moves a file to the Trash with `g_file_trash` and then
  finds where it went (GIO does not say): the newest `.trashinfo` naming
  it in the home Trash or the `.Trash/<uid>`, `.Trash-<uid>` at the top of
  its filesystem; `trashCopy` (Discard Hunk) copies the file into a folder
  of ours beside it and trashes that copy; `putBack` moves it back and
  drops its `.trashinfo`. A volume with no Trash (a tmpfs) refuses, and
  nothing is discarded. `GitDiffView.{h,cpp}` is the diff view (moved from
  `GitPanel.cpp`) with the hunk links (a tag over the words of
  `GitUi::diffTextWithActions`, a pointer cursor and a tooltip over them)
  and its own right click, Menu key and Shift+F10 menu
  (`GitUi::hunkMenu`) above Copy and Select All; a diff without actions
  keeps the text view's own menu. `Editor::onDiffViewMade` lets the window
  hand the new view to the panel (`attachDiffView`), which decorates only
  the diff it showed last (`hunksFor`, by its bytes). `GitVerbs.cpp` has
  the commands and `GitMenus.cpp` the row menu, the panel's menu and the
  window's actions.
- **The menus are built in one place**, `GitMenus.cpp`, and every one goes
  up through `GitPanel::popupMenu`, which ends it with item 2.11's Adjust
  Settings… as a last section. Part by part as the Mac's: a file row has
  its menu, the rest of the change list (headings, the empty part) and the
  graph only Adjust Settings…, the rest of the panel its own menu (only the
  item outside a repository), and the message box its text's menu, which
  `buildActions` ends with the item (`Appearance::addToTextMenus`). A disabled
  Discard item's reason is its tooltip, set on the item's `GtkModelButton`
  found by its label, since a popover menu's model has no tooltips.
- **Several rows**: the change lists are a `GtkMultiSelection` (GTK's own
  Ctrl and Shift clicks); headings never stay chosen (`onSelectionChanged`
  takes them off); the row the keyboard is on is the panel's `cursor_`,
  Shift with the arrows chooses from `anchor_`; a refresh keeps them chosen
  by `GitUi::keepChosen`. A right click on a row among several chosen acts
  on all of them, as in VS Code.
- **Words that differ from the Mac's**: `networkFailureText(..., true)`
  names git-credential-libsecret and plain ssh-add; Undo Discard in the
  menu bar and the palette keeps that title (a GMenu's labels are fixed),
  while the panel's own menu names the file; a Fetch, Pull, Push or Sync
  asked for with the panel hidden says its result in the status bar too
  (the Mac only puts it under the button). The core's discard texts still
  say Put Back in the Trash, the Finder's words.
- **For checks**: `GitPanel`'s `lastSync()` ("fetch ok", "push failed",
  "fetch refused"), `autoFetches()`, `autoFetchTickAt(now)` with a time of
  the check's own (the monotonic clock in seconds otherwise) and
  `isActive`, `actionsDone()`, `chosenRows()`, `rowWidget(row)`, `menu()`,
  `syncButtonTitle()`, `undoDiscardTitle()`, `message()`; `GitDiffView`'s
  `links()` and `menu()`; `MCGuiOpenWindow(folder)` (a new window on a
  folder, as a second `minicode <folder>`); `GitTrash::useFolderForChecks`
  (a folder of the scratch world stands in for the Trash). The suites'
  helpers are `tests/gui/GitChecks.h`; `git_actions` also makes one round
  trip through GIO's real Trash, after checking that the scratch world's
  home and data folder hold it.

Traps:
- **A row's tooltip took a click.** GTK shows a list row's tooltip below
  the pointer; an XTest click sent at once to the row underneath went to
  the tooltip's surface (GTK hides it on the next motion, after the click
  had reached it), so a Ctrl click on b.txt right below a.txt reached no
  window at all. The suites' `ClickRow` rests the pointer on the message
  box first. A person's pointer moves there and the tooltip goes first.
- **A wait on the panel's data ends before GTK has laid the widget out.**
  The message box and the Sync button turn visible with the status that
  fills the rows, and GTK places them on its next frame, so `git_actions`'
  first `ClickRow`, in the step after its rows wait, pointed at a 0x0 text
  view (in every run after `tabs` and `palette`, and in whole GUI runs at
  load 13 to 14); the branch line `git_sync` once could not right-click
  was most likely the same (that click, too, followed a wait on data only).
  A diff just shown is the same in its text: GTK gives its lines heights in
  idle slices, and a point worked out before then for "+k2" selected the
  line below under load. Every click in the git suites now waits for its
  widget on screen first (`WaitOnScreen`, `ClickBranch`, `ClickEditor` in
  `GitChecks.h`), `DiffOnShow` also wants the diff's last line to have a
  height, and a failed point says why (`a GtkTextView, 0x0`).
- **A refresh made every row again.** `applySnapshot` reset the lists'
  model on every status, so a refresh landing between a click's press and
  its release, or between finding a row and clicking it, lost the click (a
  whole GUI run at load 14 lost two). A status with the same rows now
  leaves the list, what is chosen and the keyboard's row as they are; most
  refreshes (a save, the window coming back, git's own files) are such.
- **`gtk_widget_has_focus` wants an active window**, which the harness's
  window is only after it gives it the X focus (before a key). Check the
  root's focus (`GitChecks.h`'s `Focused`) after a click.
- **Two clicks on one place within GTK's double-click time are one double
  click** (n_press 2), however much happened between: a third click on the
  branch label soon after the second opened nothing. The suites pause
  between clicks on the same widget.
- **`Git` is a namespace** (`GitStatus.h`): a suite helper called `Git()`
  is ambiguous under `using namespace`; the suites call the panel
  `Panel()`.
- **A popover child of a widget that places its children by hand** is
  presented by that widget (`GitPanel::layout` calls
  `gtk_popover_present`), as GTK asks of a custom `size_allocate`.
- **A patch on stdin to a git that stops reading** ends in GIO's
  `G_IO_ERROR_BROKEN_PIPE` from `g_subprocess_communicate`, not a SIGPIPE
  (a throwaway program showed it); `runGitWithin` keeps git's own answer
  then.

### 31. Themes from a menu and Adjust Settings (done, November 2026; `PARITY.md` item 2.11)

What later items build on:
- **A new part of the window adds Adjust Settings… with one call**
  (`src/Appearance.h`), where its menu is made, with its id from
  `Settings::appearanceSurfaces()`: `Appearance::addItem(menu, "git")` at
  the end of a `GMenu` the part builds (the tree's `g.treeMenu`, a tab's
  `Editor::menuForTab`); `Appearance::attachMenu(widget, "statusbar")` for
  a widget with no menu of its own (a right click there that no child takes
  pops one holding only the item, and every text field and text view under
  it gets the item in its editing menu, as `addToTextMenus` does alone);
  `Appearance::attachEditorMenu` for the editor's view ("editor", or
  "markdown" while a page shows, followed on `notify::buffer` and as a
  right click starts); `attachTerminalMenu` for VTE (Copy, Paste, Select
  All, the item, through `vte_terminal_set_context_menu_model`); and
  `addToWebMenu` from a `WebKitWebView`'s `context-menu` signal. The items
  name the window's `win.adjustsettings` with the part's id as target;
  main.cpp's `act_adjust_settings` opens the settings file and
  `Appearance::showSurfaceKeys` does the rest: the part's missing keys'
  commented defaults appended (`Settings::adjustSettingsAddition`, one
  user action, saved through main.cpp's `saveDocument` only when the tab
  was clean), the first key's value selected by `Editor::revealLine`, and
  the keys' lines (`Settings::surfaceKeyLines`) tinted with the
  `minicode-adjust-tint` tag at priority 0 (a swatch keeps its color) for
  3.3 s, fading as the Mac's. Item 2.9's row and file menus in the Source
  Control panel should end with `addItem(menu, "git")`; the panel itself
  and its message box have it already (`act_toggle_git`).
- **View > Theme and View > Adjust Settings** are no registry commands, as
  on the Mac, where main.mm adds them by hand: `Keys::menuBar` takes an
  `extend` callback, `Appearance::extendMenu`, which appends both submenus
  to View. The theme rows name the application's stateful `app.theme`
  (the theme's id; `Appearance::settingsChanged` moves the check after
  every reload of the file), whose activation is `Appearance::chooseTheme`:
  refused before asking while a tab holds settings.conf with unsaved
  changes (`AppSettings::unsavedTextCheck`, set in `initShared`), else
  the Mac's question when `Settings::colorOverrides` finds colors of the
  file's own (a `GtkAlertDialog`: Use the Theme's Colors, Keep My Colors,
  Cancel), then `AppSettings::writeTheme`. The keys suite leaves those two
  submenus out of its count of the registry's items. The palette does not
  list them (the Mac's reads its menu bar; the port's reads the registry).
- **minicode's own writes** of settings.conf and keys.conf go through
  `writeConfigFile` (AppSettings.h, the Mac's `MCWriteConfigFile` over
  `FolderSearch::writeFileAtomically`): `AppSettings::ensureFileExists`,
  `Keys::ensureFileExists` and the theme. `g_file_set_contents` replaced a
  symlinked dotfile with a plain file.
- **GTK's own widgets follow the theme's darkness**:
  `Appearance::applyColorScheme` sets `gtk-interface-color-scheme` (GTK
  4.20 and later) from `Settings::isDark()` on every settings change. A
  throwaway program on the ThinkPad showed GTK 4.22's own theme ignoring
  `gtk-application-prefer-dark-theme` and following the color scheme. A
  desktop theme whose name fixes its look (the owner's `Yaru-dark`) may
  keep it: not tried on the desktop. The stylesheet's selection, scroll
  bar sliders and the tab close button's light follow too
  (`theme:light`).
- **The settings file's opacity values** are underlined
  (`Document::kOpacityTag`, applied by `decorateColors` beside the
  swatches) and a click opens a slider (`OpacityPicker.cpp`, Editor's
  members, in the color picker's popover slot, the same 150 ms save).
- **For checks**: `MCGuiApp().statusBar`; `Editor::pickerPopover()` (the
  color chooser's or the slider's popover); `Appearance::tintedLines`,
  `editorMenuSurface`. The suites are `adjust_settings`, `themes`,
  `swatches` and `config_writes`. A suite that picks from the menu bar
  finds the `GtkPopoverMenuBar` under the window and clicks labels in the
  mapped `GtkPopoverMenu`s; a submenu slides in, so wait for one of its
  rows to be on screen before clicking.
- **Traps**: GTK 4's menu bar left the keyboard on its item after a choice
  from it, so after View > Theme typing went nowhere until a click;
  `MenuBarFocus.{h,cpp}` puts it back where it was once a menu of the bar
  has closed and the command did not move it. Two more ways it was lost
  (2026-11-10): a refresh while the menu was open made the Source Control
  panel's rows again, so the row remembered was gone and the keyboard
  stayed on the bar (it now goes to the nearest ancestor still on show,
  the list, which puts it on its row); and GTK's model button opens its
  submenu by a timer 80 ms after the pointer last moved over it, which
  the menu closing does not stop, so after a quick click it grabbed the
  keyboard for a row of the closed menu (a row of a menu not on show
  taking it now counts as the menu closing). The `themes` check had
  compared the exact widget, and failed when git's first answer moved
  the keyboard from the panel's list onto a row after the menu: it
  waits for that answer now and compares panes. One `GtkPopoverMenu` shown a
  second time from a right click kept the keyboard on its item after a
  click and never ran it (one run in two): `attachMenu` makes a new one
  each time, as the tab strip does. `gtk_text_view_get_iter_at_location`
  answers the next character over a character's right half (GTK 4 adds
  the trailing offset), so a one-digit opacity, and a swatch's last digit,
  missed half their clicks; `swatchAt` and `opacityAt` use
  `gtk_text_view_get_iter_at_position`. A right click at a fixed point near
  the top of a Markdown page opened no menu in two runs of four (the
  heading and its rule are there); aimed at the paragraph's text
  (`pointAtText` in the suite) it never failed.

### 32. Problems, the second server, watched files and Organize Imports (done, November 2026; `PARITY.md` item 2.2)

What later items build on:
- **Diagnostics are kept per file and per server** (`Lsp::DiagnosticStore`
  in `DiagState`, keyed by the server's settings key, "python" or
  "python.also"), drawn merged (`Lsp::mergedDiagnosticSpans`), each
  message tagged with its source and code ("(Ruff F401)"). A server
  going erases all it said (`forgetServerDiagnostics`), a file closing
  on one server what that server said about it. Files no tab has keep
  what their server published, for the list.
- **The Problems list** (`src/ProblemsPanel.{h,cpp}`, rows from
  `LspSessionProblems.cpp`): `LspSession::problemRows()` places each open
  tab's problems on its text as it is now and caches each file's rows by
  the store's generation and the tab's edit count (`documentEdits`, from
  the sync part); `onProblemsChanged` comes 50 ms after a change. The
  panel sits between the top and the terminal (`WindowLayout::
  setProblemsPane` and `setProblemsShown`, the Mac's 30% rule; the
  terminal's bar is dragged in what the list leaves), its counts in the
  status bar left of the servers' line. F8's message uses the hover box
  without autohide (`showProblemNote`), so the keyboard stays in the
  editor and the next F8 works; the caret leaving closes it.
- **The second server** (`LspSessionAlso.cpp`): `alsoKey_` and
  `alsoServer_` beside `server_` for the file on show; `uri_` is set when
  either runs. One-server requests go through `serverFor(feature)`
  (`Lsp::routeRequest`), code actions through `askCodeActions` (every
  server offering them, each with its own diagnostics, merged, 3 s at
  most), which Organize Imports shares (`LspSessionImports.cpp`). The
  status line is `Lsp::statusLine`; with the main server missing its note
  comes first.
- **Watched files** (`LspSessionWatch.cpp`, the watcher in
  `src/LspWatcher.{h,cpp}`): every client offers dynamic registration
  (`setFileWatching(true)` in `serverForKey`). inotify watches one folder
  at a time, so the watcher keeps a `GFileMonitor` per folder below the
  roots (`Lsp::watchRoots`, `Lsp::watchDescends`: never .git or a
  package's folders), at most 16,384, on a thread with its own main
  context; a folder that appears is read and its files reported as
  created. Batches are classified, coalesced and matched there
  (`Lsp::prepareWatched`); the main thread only writes the message.
  Trap: the registration is known before the thread has walked the
  tree (39 monitors quiet, 5 loaded); a file written meanwhile is found
  by the walk, never reported as created, so a check waits for
  `watchedFolderCount` to stop growing before it writes.
- **Organize on save**: `Formatting::beforeSaving(organize, format)` from
  `act_save`, the same `saveTab_` marker as the formatter's wait.
- **For checks**: `MCGuiApp().problems` (`rowTexts`, `selectedRow`,
  `rowWidget`, `statusText`), `LspSession::fileServerNames`,
  `hoverVisible`, `watchedFolders`, `watchedFolderCount`,
  `watchedBatches`, `injectWatchedPaths`. A stand-in `ruff` (a script
  exec'ing build/fakelsp) gives a server the name ruff.
- **A request goes only to a server that has the file's text**:
  `serversFor` leaves out a server with no open entry for the file on
  show, and `askCodeActions` and Organize Imports run `syncOpenDocuments`
  first (it flushes edits too), so a second server that came up after the
  file was shown is given didOpen before it is asked, never asked for
  edits on nothing.
- **Traps**: replacing the list's model takes the keyboard from the row
  that had it (the focused GtkListItemWidget goes); `setRows` does
  nothing for the same rows and gives the list the keyboard back after a
  real change. An autohide popover takes the keyboard, so a window
  accelerator that checks the editor has it (F8) then rings the bell.
- **Not done**: the Mac's Language Servers popover and its cards, the
  Problems list's right-click menu (Copy, Copy Message), and restarting a
  crashed server.

### 33. Completion's edits, snippets and the signature hint (done, November 2026; `PARITY.md` item 2.5)

The Mac's `Lsp+Completion.mm` and `SnippetSession.mm` on the GTK side
(`LspSessionCompletion.cpp`, `LspSessionSignature.cpp`,
`SnippetSession.{h,cpp}`). What later items build on:
- **The completion list** now opens by itself as on the Mac: after ".",
  "->" or "::" (the server's trigger characters), on the first letter
  after one of those, and on the second letter of any other word (not a
  number); a list the server called incomplete is asked again at each
  letter, the old one staying up until the answer. A request goes only to
  a server that offers completion, or one still starting. Tab and Return
  take the selected item whenever the list shows
  (`Lsp::completionKeyAccepts`, VS Code's defaults; `../docs/notes/lsp.md`'s
  bullet still describes the rule before 2026-11-08, when Return took an
  item only in some cases); Shift+Tab and Shift+Return close it and are
  the editor's; a key pressed before the list was filtered for the last
  letter has it filtered first. The highlight is the best match unless the
  arrows chose an item still listed. Not done: the documentation panel
  beside the list (`completionItem/resolve` is asked only for late edits),
  parsing a big list off the main thread, and the list following the zoom.
- **The keys' order** (`Editor::onViewKey`): a list showing by the caret
  first (`EditorObserver::listKey`, which the session answers for the
  completion list, the references and fixes and the signature hint; its
  own capture controller calls the same, so the order of the two
  controllers does not matter), then the snippet's Tab, Shift+Tab and
  Escape, then the several carets, folds, auto-closing and indentation.
  `takesReturn` is asked after the list had its chance, so Shift+Return
  closing the list is an indented new line.
- **Accepting** is one `MultiEdit::apply` inside one user action: the
  word and its `additionalTextEdits` (`Lsp::completionEdits`, then the new
  `Lsp::pointEdits(text, OffsetEdits)` for code points). **Trap:**
  GtkTextHistory joins an action that only inserts to the typing just
  before it (a throwaway program showed it: "os." typed, "path" put in,
  one Ctrl+Z took "os.path" away), and no call breaks that chain (a cursor
  move, an empty user action, set_modified, max-undo-levels: tried). An
  action that also deletes is a step of its own, so an accept that only
  inserts takes its last character out and puts it back in the same
  action (`applyAcceptEdits`). `applyingEdit_` is set meanwhile, so the
  session's own edits open no list or hint (`textEdited`).
- **Late edits** (an item whose server sends its import only from
  resolve): the word goes in at once, and the answer has a second
  (`CompletionState::Pending`, its limit moved back by every edit before
  it, as the Mac's). GtkTextHistory cannot add to a step once made, so
  "the same undo step when nothing changed" is the accept undone and put in
  again with the edits as one action, after checking the undo gave back the
  very text the accept was made on (else it is redone, and the edits go as
  a step of their own). The snippet session is held meanwhile and moved
  by the edits (`SnippetSession::hold`, `release`), and the carets are put
  back mapped through them. An undo of the accept before the answer
  (the buffer's `undo` signal, and the word no longer between its marks)
  drops the import; an edit before it leaves it out with a note; no answer
  in a second, a note.
- **Snippets**: `Snippet::expand` with the Mac's context (the line's
  indentation, `AutoIndent::detect`'s unit, the file's line break, the
  variables); `$CLIPBOARD` is minicode's own clipboard only, since GTK
  reads another program's asynchronously. `SnippetSession` (one per
  editor, `Editor::snippets()`) keeps the core's `Snippet::Session` in
  characters (the new `Snippet::inCodePoints` and the session's `length`),
  follows the buffer's `insert-text` and `delete-range` before they
  happen, ends on `undo` and `redo`, on the view showing another buffer,
  and when a mark-set leaves the selection outside the stop (looked at in
  an idle). Mirrors are carets through `Carets::set`; a choice's option
  goes in at every caret through the new `Carets::insertText`. The stops
  are drawn by a `BelowText` pass at order 6.
- **The signature hint** (`LspSessionSignature.cpp`): a popover above the
  caret (no autohide, never focusable), opened by the server's trigger
  characters, asked again after a retrigger character, and 0.15 s after
  any other edit or caret move while it shows; Escape, the view losing the
  keyboard, an empty answer or the file leaving the view close it.
- **For checks**: `LspSession::completionSelectedLabel()`,
  `signatureText()`, `pendingAccepts()`; `SnippetSession::active()`,
  `currentStopIndex()`, `currentRanges()`, `allRanges()`, `drawnBoxes()`;
  `tests/gui/CompletionChecks.h` (the text and caret, a file shown with the
  keyboard, the scripted server's JSON). The suites: `completion_keys`,
  `snippets`, `completion_edits`, `signature_help`.
- **Traps**: the scripted server keeps an unknown `${...}` as written, so
  snippet text goes in its script as is; an item with no
  `additionalTextEdits` field is resolved after it is accepted when the
  server resolves (give it `[]` in a check that is not about that).

### 34. Change bars and the tree's git colors (done, November 2026; `PARITY.md` item 2.10)

- **The change bars** are `GitGutter` (`src/GitGutter.{h,cpp}`), the Mac's
  `MCGitGutter` step for step over the core's `QuickDiff`: the original is
  the index entry (`ls-files --stage`, `cat-file blob`), read again only
  when the file looked at or the index file's stat changed; the diff runs
  on a thread of its own 0.3 s after typing pauses (one timer) and is
  dropped when an edit, a tab switch or another look came meanwhile. It
  takes two hops to the worker: the source first, then the text, copied
  only for a file git has (`Document::text()`, a copy of the whole file).
  Each buffer's `BufferLines` keeps its bars, so a tab shown again has them
  and a look follows. `editor.git-gutter` turns the gutter's lane on and
  off (`Gutter::setChangeLane`) and `setChangeColors` takes
  `editor.git-*`; while it is off nothing looks and nothing touches the
  bars, which is why the `gutter` suite, which sets bars by hand, runs
  with it off. The bar's popover (a child of the text view, unparented on
  unrealize; shown on the button's release, since one popped up on the press
  was closed again by GTK before a slower release, the first time; and the old lines' scrolled window has a fixed height, since one sized by the text view's lazily validated lines queued resizes during the popover's own allocation and GTK closed the popover, about 1 run in 6 pinned to two cores) has Revert Change, a `MultiEdit::apply` of
  `QuickDiff::revertEdit` (UTF-16 offsets through `utf16::toCharOffset`),
  and Stage Change, refused when the entry no longer names the bars'
  object. The window's actions are `nextchange`, `previouschange`,
  `revertchange` and `stagechange`, enabled as the Mac validates them (bars
  there, a bar on the caret's line). The panel's refresh calls
  `GitPanel::onRefreshed`, and the window coming to the front looks again.
- **The tree's colors** are `TreeGitColors` (`src/TreeGitColors.{h,cpp}`),
  the Mac's `MCTreeGitColors`: one `Git::decorationArgs()` status per look,
  0.5 s after the first change, nothing for a change inside an ignored
  folder or `.git/objects` and `logs`. `FileTree::setDecorator` gives each
  row's name a class (`minicode-git-modified`, ...) and a tooltip as it is
  bound, `redecorate()` asks the rows on show again, and `ThemeCss.cpp`
  colors the classes from `tree.git-*`. The tree's activity callback now
  carries the changed path.
- **Both watch the git folder** (`src/GitDirWatch.{h,cpp}`, the Mac's
  `MCGitDirWatch`): a directory monitor on `rev-parse --absolute-git-dir`,
  calling only when the index's or HEAD's stat changed. The tree needs it
  more than the Mac's does: GIO's monitors see one folder each and the tree
  watches only the folders it has listed (a closed folder has no monitor,
  whatever item 16 assumed), so a commit, which writes only in `.git`, was
  invisible to it. A file another program changes in a folder the tree
  never listed is colored at the next look (the window back in front, a
  save, the index, any change in a listed folder); the Mac sees it at once.
- **For checks**: `MCGuiAppWindow::changeBars` and `treeColors`;
  `GitGutter::holdLooksForChecks` makes the index move on under bars
  already drawn (Stage Change's refusal); `barDescriptions()` spells bars
  as the Mac's suite does. The scratch world sets `GIT_CEILING_DIRECTORIES`
  to its root (`Wrapper.cpp`): it lives under `build/`, inside minicode's
  own checkout, so every window's project was in that repository (ignored,
  the whole tree gray) and every look ran git on it.
- **Trap: ccache kept an object made from a header edited during the
  build.** `AppSeam.h` gained two fields while a build was compiling the
  suites; ccache stored those objects under the new header's hash, and
  later builds took them from the cache, so `GitActionsSuite.o` and three
  others destroyed `MCGuiAppWindow`s 184 bytes apart where `AppSeam.inc`
  made them 200 apart: `free(): invalid size` in `OpenRepoWindow`, in the
  normal build only (the ASan build compiled afresh). `objdump` of the
  lambda showed the old stride. Do not edit headers while a build runs; if
  one did, delete the objects and build with `CCACHE_DISABLE=1`.
- **Trap: `Editor::buffer()` lags the view in `notify::buffer`.** The
  editor sets its own pointer after `CodeView::setBuffer`, so a handler of
  the view's `notify::buffer` that asked `editor->buffer()` read the buffer
  just replaced, freed when a large file's loading buffer gave way
  (`large_file` crashed in `BufferLines::of`). `GitGutter` reads
  `gtk_text_view_get_buffer` instead.

### 35. Symbols and folding ranges from the servers (done, November 2026; `PARITY.md` items 2.12 and 2.13)

- **Symbols** are `LspSessionSymbols.cpp`, the symbol half of the Mac's
  `Lsp+Navigation`. Go to Symbol in File ("@") asks the file's server
  (`Feature::DocumentSymbols`, the core's tree flattened parent first); the
  palette's own outline stands in when no server lists symbols, the one
  asked refuses, or it exits, so the palette never waits on a dead server.
  Go to Symbol in Workspace ("#", Ctrl+T, a registry row in
  `CommandsLinux.cpp` and a GAction in `Keys.cpp`) asks every running
  server offering `workspace/symbol`, cancels the question before and
  merges answers as they come (`Lsp::mergeSymbols`). A pick in another
  file opens through `onReveal`, as a jump for Go Back.
- **Folding ranges** are `LspSessionFolding.cpp`: `textDocument/foldingRange`
  0.1 s after a file comes on show and 0.6 s after typing, parsed and
  sanitized by the core, handed to `DocumentFolds::setServerRanges` with
  the generation asked about; `regions()` and `freshRegions()` prefer them
  only while the text is that one, else `Fold::compute`'s.
- **Checked by** the `server_symbols` and `server_folding` GUI suites
  (scripted `fakelsp`); each fails with its fix taken out. The command
  registry checks in `tests/commands_linux_tests.inc` now use the inline
  agent (`inlineChat.start`) as the command Linux still lacks.
- **Needs the owner:** real clangd and basedpyright, Ctrl+T under GNOME and
  in the terminal, and the folds' look from a real server.

### 36. Semantic tokens (done, November 2026; `PARITY.md` item 2.6)

- **What it does**: the servers' `textDocument/semanticTokens/full` (then
  `/full/delta`), painted for the lines on screen only, as the Mac's
  `editor.semantic-highlighting` does. `LspSessionSemantic.cpp` asks on
  show, 0.3 s after typing and on the server's refresh, one request at a
  time, and hands `Lsp::SemanticSpan`s (decoded in the core by
  `Lsp::decodeSemanticTokens`, then `semanticSpansInUtf8` for GTK's bytes) to
  `SemanticColors` (`src/SemanticColors.{h,cpp}`, one per `Document`) only
  while the text is the one the server answered for.
- **Painting**: six tags in the shared table (class, function, variable,
  parameter, property, constant), placed above the lexer's tags and below
  the bracket colors. `paintVisible` runs when tokens arrive and as lines
  scroll in (`BracketView`'s scroll hook), each line once; spans are cut
  where the lexer has a string, escape or comment
  (`Lsp::semanticSpansOverCode`), so those keep their color. A character
  edit ends the tokens (`edited`), their tags move with the text, and the
  lines the lexer retags (`Document::retagLines`) lose theirs at once.
  None in large-file mode or with the setting off.
- **Core**: `lsp:semantic-utf8` and `-over-code` (`tests/lsp_semantic_tests.inc`).
- **Checked by** the `semantic_tokens` suite (scripted `fakelsp` with a
  legend, a 400-line file): styles and colors of class, function,
  parameter and variable, the string keeping its color, nothing painted far
  below until scrolled to, an edit ending the tokens, the lines painted
  bounded by the lines on screen and the CPU time of painting.
- **Trap**: the suite's span count is the tokens kept (8), not the ones
  painted; the string token is kept and painted around.
- **Trap**: Ctrl+End (and Page Down) left the view short of the caret under
  load: GTK scrolls by estimated heights and stopped where the estimate was,
  so the lines painted were right for what was on screen, which was the
  wrong place (`semantic_tokens` timed out only with the machine busy).
  Painting was never at fault. The view's `move-cursor` for buffer ends,
  pages and paragraph ends now starts `Editor::settleOnCaret`, as a go to
  line does; any jump by the view's own keys must be watched this way.
- **Needs the owner**: how the colors look with real basedpyright, clangd
  or rust-analyzer, and scrolling a long file.

### 37. Install hints (done, November 2026; `PARITY.md` item 2.8)

- **What it does**: a language server that is not installed is offered
  as an install, as the Mac's install bar does. `ConsentBar` (the one bar
  under the tabs) shows "No Python language server is installed...", Install...
  (a menu button whose popover holds an `InstallView`), Don't Show Again and
  x. The commands are the core's (`src/InstallHints.{h,cpp}`, Linux's ways:
  uv, pipx, npm, apt, go, cargo, rustup, snap, the first whose installer is
  here, else how to get the installer). minicode never runs one: Copy puts it
  on the clipboard, Run in Terminal types it with `Terminal::typeText` (a paste
  in brackets, never Return) and shows the terminal.
- **When**: `LspSession::missingServer` (a key none of whose programs was
  found, `notInstalled_`), once per language per launch across windows, until
  x, Don't Show Again (`$XDG_STATE_HOME/minicode/install-hints.json`) or the
  server appearing. `LspSession::lookAgainForMissingServer` runs when the
  window becomes active (`onIsActive`) and when a terminal command finishes
  (`Terminal::onCommandFinished`, a look four times a second after each Return
  until the shell has the terminal), so no restart is needed.
- **Fake installers**: `MINICODE_INSTALL_HAVE=uv,npm` replaces the search for
  installers, as on the Mac.
- **Checked by** the `install_hints` suite: the bar with the right command
  (uv), Copy to the clipboard, Run in Terminal typed and not run, the server
  appearing taking the bar away, x for this launch, Don't Show Again kept.
  It fails without the wiring in main.cpp.
- **Format Document's note** (`Formatting::showNote`): a Failed decision with
  `installPrograms` (pyink and yapf, prettier...) puts an
  `InstallView::forFormatters` under the message in the note's popover (Copy,
  Run in Terminal through `Formatting::Host::runInTerminal`, the same typing
  as the bar's); `Formatting::installView()` is for checks, and the
  `install_hints` suite's last case clears PATH and presses Format Document.
- **Needs the owner**: the commands on a real Ubuntu (apt, snap), and a real
  install finishing in the terminal bringing the server up.

### 38. The control channel, part one (done, November 2026; `PARITY.md` item 3.1)

- **What it does**: the Mac's control channel for the commands that need
  only windows, tabs and a selection: `ping`, `window`, `open`
  (`path[:line[:col]]`, `--to`, `--keep`, `--window`, `--folder`), `state`
  (`--pretty`) and `selection`. One JSON object a line, validated by the
  core's `ControlProtocol` table (`src/ControlProtocol.cpp` and
  `PageCheck.cpp` joined the Linux build lists; nothing copied). Every other
  command in the table is registered and answers "<cmd> is not on Linux
  yet"; `open --note` and `--from` answer that notes are item 3.4's.
- **The pieces**: `linux/src/ControlServer.{h,cpp}` is the socket on the
  GLib main loop (`g_unix_fd_add`, no thread): `<pid>.sock`, 0600, in a 0700
  folder; `SO_PEERCRED` refuses another uid; one request at a time per
  connection (the reader is taken off the loop while a request is out, so a
  client cannot queue work); non-blocking `send` with a write source, and a
  peer that takes nothing for 10 s dropped; lines over 1 MB close the
  connection; stale sockets (dead pid, or a live pid where `connect` is
  refused) removed at start (`removeStale`); the socket unlinked at exit.
  `linux/src/ControlCommands.{h,cpp}` are the handlers; they see the app
  only through `ControlHost` (windows, front id, open window, present, show
  file), which `main.cpp` fills in (`startControlChannel`, called once from
  the first command line; each `App` has a `controlId`). The client is the
  Mac's `tools/minicode-cli.cpp`, with `#ifdef __APPLE__` around what is the
  Mac's (libproc, bundle paths, `open -a`, hooks, `Procs`) and
  `minicodeCliMain` in place of `main`; it is a source of the `minicode`
  target, and `main()` hands argv to it when argv[1] is a protocol
  subcommand (`ControlChannel::isSubcommand`), before Detach or GTK start.
  So `minicode open|window|state|selection|term|...` is the client, and a
  file named like a subcommand needs `./open`, as on the Mac's launcher.
- **Where the socket is**: `src/ControlPaths.h` (header only, tested as
  `control:linux-socket-folder`): `$MINICODE_CONTROL_DIR` when set (tests),
  else `$XDG_RUNTIME_DIR/minicode` (`/run/user/<uid>/minicode`, which
  systemd makes 0700), else `<$TMPDIR or /tmp>/minicode-<uid>`, made 0700 by
  the server and used by either side only when it is a real folder of the
  user's own with no group or other access. The harness's scratch
  `XDG_RUNTIME_DIR` keeps a run's sockets apart from the user's.
- **The keyboard**: `userIsInDocument` is the window active and the focus
  in the editor's text view. Then a file that is not the one on show gets a
  tab (`Editor::addTab`, or its own) with the caret or range given as the
  tab's place (`placeTab`, offsets worked out from the file by
  `ControlCommands::offsetOfLineColumn`), `"background": true` in the answer
  and a line on the client's stderr, and the tab on show stays. Otherwise
  the file shows as a double-click in the tree shows it, and
  `revealLineColumn` places the caret (`--to` selects up to the range's end
  instead).
- **What differs from the Mac**: `state` has `terminals: []` and `browser:
  null`; columns count characters (the Mac's, UTF-16 units); the agent
  preview tab does not exist (it comes with notes, 3.4), so `--keep` changes
  nothing: every open is a kept tab; a launch with no minicode running
  spawns this binary on the folder (it detaches from the terminal) and waits
  10 s for the socket.
- **Checked by** the `control` GUI suite (the real client as a subprocess
  of the harness, since the server is the harness's own process), which
  fails with the server taken out, and `control:linux-socket-folder`.
- **Left for later bites of 3.1**: `term`, `term-output`, `diff`,
  `browse`, `page`, `open-url` and BROWSER, `notify` and `hooks`, `procs`,
  `tour` and `install-skill`, and the skill file. Another uid's refusal is
  untested (needs a second user).

### 39. The compare view (done, November 2026; `PARITY.md` item 3.2)

- **`CompareView.{h,cpp}`** is the Mac's `MCCompareView`: two panes, each a
  `GtkDrawingArea` in a `GtkScrolledWindow`, drawn by hand with cairo and
  Pango, never laid out as text. Every row is one fixed height
  (`rowHeight()`), so a row's place is arithmetic and only the rows on screen
  are shaped (the Mac's trap was 1.5 s laying out 50,000 lines a side; here
  50,000 a side costs the main thread well under half a second of CPU). The
  two vertical adjustments follow each other (`sync`, guarded against the
  echo); sideways each pane scrolls on its own, and the line numbers stay put.
  Tints are the git panel's red and green, changed words stronger, a filler
  row hatched.
- **`ComparePanes.{h,cpp}`** (no GTK) is `MCComparison prepare`: `align`,
  `hunks`, each pane's text from `paneText`, the syntax colors from
  `IncrementalHighlighter<char16_t>` moved onto the pane by `spansOnPane`
  (none past 2 MB a side), and `wordChanges` under the Mac's 4 MB budget and
  2,000-character lines, all as UTF-8 byte offsets. `SideBySide.cpp` joined the
  Linux core lists for it. `Editor::showComparison(spec, done)` runs it on a
  thread and opens the tab from an idle (a flag in `compareAlive_` covers a
  window closed meanwhile).
- **The tab** is a diff tab with `Document::compare()` set (`setCompare`), so
  it has no path, is read-only, is skipped by sessions and Quick Open, closes
  like any tab and is titled by `spec.title`; `mutible diff` makes the
  agents' preview tab, which the next one replaces (item 149). F7 and
  Shift+F7 are handled by the panes while one has the keyboard
  (`SideBySide::stepHunk`, wrapping), with three rows of context above.
- **Checked by** the `compare` GUI suite: rows and fillers, the words, syntax
  styles, scrolling either pane, real F7 and Shift+F7 with wrapping, and
  50,000 lines a side under a CPU bound. It fails with `showComparison`
  taken out.
- **Decisions** (part two). `CompareSpec::decisionPrompt` (with `onDecision`,
  `lastChoiceGrace`, `quiet`) makes `Editor::showComparison` give the tab a
  `CompareDecision` (`Document::decision()`; the Mac's `MCComparison`
  decision, GTK-free but for a GLib timeout): choices per hunk, `decide`,
  `decideRest`, `decideHunk`, `sendNow`, `tabClosed`, `endDecision`, and
  `finish` telling `onDecision(accepted, text)` once, the text the right one
  when all are accepted, else `SideBySide::assemble`. `CompareView` shows a
  bar (prompt, Accept and Reject, which turn into Accept the Rest and Reject
  the Rest once a hunk is decided, and Send Now in the grace) and a gutter
  between the panes with a check and a cross at each hunk's first row, drawn
  from the shared adjustment so it scrolls with them. A press on an
  undecided hunk's button decides it, a press on a decided hunk's own button
  undecides it. The last choice sends after the grace (3 s); the bar's
  buttons send at once, and `Editor::destroy` calls `tabClosed`, so closing
  the tab in the grace sends what was decided and before it rejects. A
  rejected hunk's rows are tinted as the Mac's (item 149). Trap: two clicks in one check step are
  one gesture to GTK's click recognizer; click once per step and wait.
- **`minicode diff`** (`ControlCommands.cpp`, the client allows it): `path`,
  `--base` (`isSafeRef`: never starting with "-"), `other`, `--window` and
  `--folder`; files are plain, UTF-8, up to 16 MB, opened `O_NONBLOCK` and
  checked again once open; git (`rev-parse`, `show ref:./name`, through
  `runGit`) and the reads run on a thread, the lining up on
  `showComparison`'s own, and the answer waits for the tab. While the user
  types in the editor the tab opens quietly beside theirs (`quiet`, and
  `"background"`, which the client already says). Since item 84 a call
  without `--keep` opens in the agents' preview tab for comparisons
  (`CompareSpec::preview`, `Model::Preview::AgentDiff`), which the next
  such call replaces, as on the Mac. Titles and labels are the Mac's.
- **The corner** (November 2026): the cell of the pane's grid under the
  shared vertical bar, beside the right pane's horizontal bar, had no
  widget, so the window behind it showed (white under a light desktop
  theme). The grids carry `minicode-compare` CSS now, `background` from the
  editor's color in `applySettings`. The check reads the pixel through
  `MCGuiPixel` (Harness.h: red, green, blue and alpha of a widget's
  rendering) and wants it painted and dark; it fails without the CSS.
- **Still open:** a tab's scroll place when another tab shows.
- **Needs the owner:** the look (tints, hatching, colors in a light theme),
  the wheel and a touchpad in the panes, F7 under GNOME.

### 40. Processes and ports (done, November 2026; `PARITY.md` item 3.8)

- **The source** is `src/ProcFs.{h,cpp}` (core, POSIX only, in both build
  lists; it compiles on the Mac and nothing there uses it): `ProcFs::Source`
  is `Procs::Source` over a `/proc`-shaped folder, `/proc` by default, the
  tests' own otherwise. The parsers (`parseStat`, `parseBootTime`,
  `joinCommandLine`, `parsePss`, `parseListening`, `socketInode`) take text
  already read. Start time is field 22 of `stat` in clock ticks, made
  microseconds since 1970 with `btime` read once when the source is made
  (`btime` moves when the clock is stepped, and a key (pid, start) must not
  change under a process). Footprint is Pss from `smaps_rollup`, else RSS from
  `statm`: the Mac's physical footprint charges a process its own dirty pages
  and shared ones to nobody in particular, and Pss shares them out, where RSS
  counts a shared library in full in every process. Only the caller's own
  processes are listed (the owner of `/proc/<pid>`). Listening ports are
  `net/tcp` and `tcp6` rows in state 0A matched to the `socket:[inode]` links
  under `fd`, read once per look.
- **The sampler** is `src/ProcessMonitor.{h,cpp}` (GTK): a thread of its own
  owns the source and the core's `Tracker`; a 2 s main-loop timer starts a
  look when 10 s (2 s once `setPanelShown(true)`) have passed. The main thread
  gathers the windows' folders (`realpath`) and terminals first (a window has
  one terminal, so its id stands for both; `Terminal::shellPid()` is VTE's
  child), the thread samples and the copied rows come back through
  `g_idle_add`. `stop` is `planStop` plus SIGTERM, then SIGKILL 3 s later on
  the thread's own queue, each only while the pid still has its planned start
  time; minicode, its group, the terminals' shells and pid 1 are refused by
  the core.
- **`procs`** (`--json`, `--folder`, `--window`) is registered in
  `ControlCommands.cpp` and answers from a look taken at once, as the Mac's;
  `tools/minicode-cli.cpp` let it through its "not on Linux yet" gate.
  `minicode procs` prints the core's table.
- **Checked by** the `procfs:*` core groups and the `processes` GUI suite; the
  suite starts its servers in the app's terminal and, for "not started here",
  with a shell that leaves its sleeps to the harness's subreaper (an ancestor
  of the app, so outside its tree). It failed while `procs` answered "not on
  Linux yet".
- **The panel** is `src/ProcessesPanel.{h,cpp}` (GTK): a GtkListBox in the
  sidebar stack beside the tree and the Source Control panel
  (`act_toggle_processes` in main.cpp; opening one closes the other; Focus
  File Tree, reveal and Go to Folder bring the tree back). View > Toggle
  Processes is `win.toggleprocesses` (`Keys.cpp`); the Mac gives the command
  no key, so neither does Linux. Rows are `ProcessMonitor::view(window,
  folder)` flattened (a header per group with its summary, then each
  process indented by depth: name, `:ports`, footprint, CPU, and a line of
  command and folder; the tooltip has the pid and whole command). A 0.5 s
  poll of `samples()` rebuilds only when a look arrived, keeping the
  selected pid. Stop is the row's menu (`procs.stop`, "Stop…" or, for an
  orphan, "Stop") and Delete; `planStop` is the monitor's, and the panel
  asks first (GtkAlertDialog) for all but orphans, as the Mac does. Adjust
  Settings… ends the menu (surface `tree`, as the Mac's). Colors are
  `.minicode-procs` in `ThemeCss.cpp`, the sidebar's. `setActive` calls
  `ProcessMonitor::setPanelShown`, so looks come every 2 s only while it
  shows. Trap met: none yet; an orphan needs its window closed, so the suite
  opens a second window on the folder and runs `(trap '' HUP; exec sleep
  612)` in its terminal (VTE hangs the shell up on close).
- **Session labels** (November 2026): the gatherer in main.cpp gives each
  terminal's session and notify time from the board (`TerminalAttention::
  sessionOf`) to the core's `Tracker`, so a process under the program that
  ran when a notify named a session has it, in `procs --json` ("session")
  and the row's tooltip ("Started by Claude Code session ..."); the
  `processes` suite notifies and reads both.
- **The menu's other items** (November 2026): one "Open localhost:<port>"
  per listening port (`procs.openport`, the port as the action's int32
  target; `onOpenUrl` is `openLink`, so the page loads in the user's page of
  the browser pane as a link clicked in a terminal does) and "Reveal Origin"
  (`procs.origin`, disabled for a process outside minicode's tree or an
  orphan, as the Mac's): `onRevealTerminal` shows the terminal whose id the
  row carries (`showTerminalOf`), else the status bar says the terminal was
  closed; a process started outside minicode says so. The row's `RowInfo`
  keeps `portNumbers`; the `processes` suite checks the labels, the URL and
  the three answers, and the Stop labels now end in `|Reveal Origin`.
- **Remains:** a terminal's title and id from the Terminal object (rows say
  "terminal"), and the owner's eyes on the look.
- **Needs the owner:** the sampling on a busy desktop (smaps_rollup's cost
  with a browser running), and other users' processes, which are left out.

### 41. Notes and tours (done, November 2026; `PARITY.md` item 3.4)

- **What it does**: the Mac's `AgentNotes`. `minicode open file:10 --to 12
  --note TEXT [--from NAME] [--keep]` highlights the lines (a text tag with
  a paragraph background, so no buffer text and no undo step), scrolls the
  first line to the upper third and shows a box over the editor's slot above
  it: the writer's name (Claude when none), the text (cleaned by the core's
  `cleanNoteText`, 500 characters) and an x. `minicode tour FILE` (the Mac's
  file format, parsed by the shared client) shows the first stop; Alt+] and
  Alt+[ (`win.tournext` and `win.tourprevious`, rows of `CommandsLinux.cpp`
  and `Keys.cpp`) step, enabled only while a tour is on, so the keys type
  their character otherwise; the label reads "Claude · 2 of 3".
- **The pieces**: `linux/src/AgentNotes.{h,cpp}`, one a window, made by the
  `Editor` (`Editor::notes()`), which tells it before and after the tab on
  show changes (`activeWillChange`, `activeChanged`) and when a tab goes
  (`forget`). A note is a `Note` kept per `Document*` until its tab shows;
  `place` puts it on the buffer (the tag, two marks on the first line's start
  and the last line's end, "insert-text" and "delete-range" handlers that
  close it when an edit reaches `[start, end]`, so edits above only move the
  marks). Leaving the tab takes the note off the screen and keeps it with
  its lines as the marks have them; closing (Escape through `onViewKey`, the
  x, an edit, the tab closing) forgets it, and closing a tour's note ends the
  tour. The box is an overlay child (`Editor::addOverlay`), positioned from
  the first line's window coordinates above it (below it with no room),
  again on scrolling and 60 ms after placing.
- **The agent's preview tab**: `Document::isAgentPreview`, its title italic
  like the preview tab, replaced by the next `Editor::openForAgent(path,
  preview, quiet)` in its place (never a dirty one, which is kept, nor the
  tab the user types in, which is left a preview), kept by an edit, a save
  or a double-click on its title (`keepPreviewTab`). A file already in a
  tab uses it as it is (item 84). `--keep` opens a kept tab. Without
  `--note` `open` is as before. A diff has a preview tab of its own.
- **The keyboard**: `open --note` and `tour` open beside the user's tab,
  unshown, while the keyboard is in the editor (`userIsInDocument`; the
  client says so, `"background": true`), the file added unread; the note
  waits (`pending_`) and is placed when the tab is shown, after the disk
  check, so a reload cannot close it. A note moves the caret only in the
  agent preview tab. `state` has `note` (text, label, lines) and `tour`
  (`stop`, `of`) per window, as the Mac's.
- **Checked by** the `notes` GUI suite (`BUILD-LINUX.md`); 30 of its checks
  fail with `place` switched off.
- **Not done**: a note over a Markdown preview or LaTeX page (it waits until
  the source shows), over a tab in the split editor (there is none here),
  columns of a range are highlighted but the box is placed by the first
  line, the hints panel's rows for the tour keys, and a note over a tab
  still reading in the background when the keyboard leaves it (it waits for
  the load, then places). **Needs the owner**: the box's look on a real
  theme and its place with word wrap and a long first line.

### 42. Waiting for you (done, November 2026; `PARITY.md` item 3.6)

- **What it does**: the Mac's feature 7 for a Claude Code session in the
  app's terminal. Every terminal has an id when made (a window has one
  terminal, so it is the window's control id, what `minicode state` reports
  under `terminals`), and its shell gets `MINICODE_TERMINAL_ID` and
  `MINICODE_PID` (`Terminal::spawnShell` builds the environment from the
  app's, less any copies it inherited from another minicode). `minicode
  notify --from-hook` (stdin: the hook's JSON) marks that terminal on the
  core's `AttentionBoard`: Notification is waiting, Stop is done,
  UserPromptSubmit and PostToolUse clear. It never starts the app, and with
  `--from-hook` prints nothing and exits 0 whatever happens, outside minicode
  too. `minicode hooks --print` and `--install` are the Mac's code, no longer
  `#ifdef __APPLE__` in `tools/minicode-cli.cpp`: `~/.claude/settings.json`
  (or `$CLAUDE_CONFIG_DIR`), a timestamped backup first, a merge that only
  adds, a second run changing nothing, a symlinked file staying a link
  (`ClaudeHooks::writeSettingsFile`).
- **Where it shows**: there are no terminal tabs, so the mark is a dot in the
  terminal panel's top right corner (a `GtkLabel` in the terminal's overlay;
  orange for waiting, green for done), with Claude's message as its tooltip;
  the count of waiting terminals in every window leads each window's title
  ("(2 waiting) MiniCode - ..."), in place of the Mac's Dock badge (GTK4 has no
  urgency hint to set on a window). Next Waiting Terminal
  (`minicode.terminal.nextWaiting`, **Ctrl+Alt+W**, Global so it works from
  the terminal; the Mac's Ctrl+Cmd+W) presents the next window whose terminal
  waits, in the windows' order, wraps, shows a hidden terminal and gives it
  the keyboard; with none waiting it beeps.
- **Clearing**: a key press in the terminal (a controller on the VTE widget,
  capture phase, modifiers alone ignored) or the terminal going away
  (`~Terminal`). Output arriving, pastes from Run in Terminal and the
  terminal's own replies do not clear it.
- **The pieces**: `linux/src/TerminalAttention.{h,cpp}` is the one board for
  every window (no GTK in it; `main.cpp` sets its changed handler in
  `initShared`, which redraws the dots and titles), `notify` is in
  `ControlCommands.cpp` (it answers "there is no terminal N" for an id no
  window has), and `ClaudeHooks.cpp`, `AttentionBoard.cpp`, `AgentIndex.cpp`
  and `AgentTranscript.cpp` joined the build lists (the board's session
  helper uses `AgentIndex`; nothing of the Agents panel, item 3.5, is built).
- **Checked by** the `attention` GUI suite (`BUILD-LINUX.md`): the shell's two
  variables read back from a file, canned hook JSON through the real client,
  silence and exit 0 with no app or no variables, the dot, tooltip and title,
  a real key (XTest) clearing the mark, Ctrl+Alt+W showing a hidden terminal,
  and `hooks --install` twice into the scratch HOME (and onto a symlink); the
  key-clearing checks fail with the controller switched off. Nothing writes
  the real `~/.claude`.
- **Not done**: a `term --run`-style way to type into
  the terminal from the client, and a per-tab mark (there are no terminal
  tabs). **Needs the owner**: the dot's look on a real theme, and the title
  count in GNOME's overview.

### 43. The Claude Code IDE link (done, November 2026; `PARITY.md` item 3.9)

- **What it does**: the Mac's `/ide` answer, per window, off by default
  (`agent.ide-link`; View > Claude Code IDE Link, the registry's
  `minicode.toggleIdeLink`, `app.idelink` here, a check-box action whose state
  follows the settings file and which writes it through
  `AppSettings::writeValue`, refused like a theme while a tab holds the file
  with unsaved changes). Core `IdeLink.{h,cpp}` joined the Linux build lists;
  nothing copied.
- **The pieces**: `linux/src/IdeLinkServer.{h,cpp}`, one a window, made by
  `newWindow` (`App::ide`) and followed by `applyWindowSettings`
  (`setEnabled(st.ideLink())`), deleted before the editor in
  `onWindowRemoved`. Raw non-blocking sockets on 127.0.0.1 with
  `g_unix_fd_add` (no thread), a write source only while a peer is slow (30 s
  without progress drops it), 8 connections, 10 s to finish the handshake;
  a 48-hex-digit secret from /dev/urandom; the lock file
  (`$CLAUDE_CONFIG_DIR/ide`, else `~/.claude/ide`; mode 0600 by temp file and
  rename; the folder must be the user's own and the config folder must exist)
  with stale own locks removed once per process and an `atexit` removal; the
  selection is polled every 300 ms while a client is initialized and sent
  when its JSON changed (0-based lines, UTF-16 columns). The tools are the
  Mac's: openFile (`Editor::openForAgent`, beside the user's tab while the
  keyboard is in the editor, `AgentNotes::show` with an empty note and the
  label "Claude Code"), openDiff (`Editor::showComparison` with
  `decisionPrompt` and `onDecision`, `quiet` while the user types),
  close_tab and closeAllDiffTabs (only diffs this link offered; closing a
  tab answers DIFF_REJECTED through `CompareDecision::tabClosed`),
  getDiagnostics (the Problems rows, the uri spelling kept), the selection
  tools, getOpenEditors, getWorkspaceFolders, checkDocumentDirty. Not offered:
  saveDocument, executeCode. A connection that closes, or the link going
  off, ends the decisions of its diffs (`endDecision`).
- **Terminals**: `Terminal::extraEnvironment` is asked each time a shell is
  spawned; the window gives `CLAUDE_CODE_SSE_PORT=<port>` while its link
  runs, so a terminal started before the link was turned on needs its shell
  restarted.
- **The harness**: `Wrapper.cpp` sets `CLAUDE_CONFIG_DIR` to the scratch
  `claude` folder (and clears the user's), so no suite can reach the real
  `~/.claude`; `MCGuiAppWindow::ide`.
- **Checked by** the `idelink` GUI suite (`BUILD-LINUX.md`).
- **The View menu is taller now**: with the item it no longer fits an
  800-pixel screen (the popover is cut to the screen and Adjust Settings,
  its last item, was off it for `adjust_settings`), so the harness's Xvfb is
  1280x1024. The menu scrolls on a real small screen.
- **Not done**: a run against a real Claude Code; a lock file's workspace
  folders when the window's folder changes are rechecked on each tool call and
  selection tick (no window re-rooting exists here); the lock file is not
  removed on SIGKILL (the next start removes it, once its pid is gone).

### 44. The Agents panel and Evidence (done, November 2026; `PARITY.md` item 3.5)

- **The model** is `src/AgentModel.{h,cpp}` (GTK, glib only): the core's
  `AgentIndex` lives on a worker thread of its own (it is not thread-safe);
  each refresh copies the rows into an immutable `AgentModelData::Snapshot`
  (sessions newest first, subagents nested by `Row::depth`, each session's
  `AgentView::groupFiles` groups with `folderText` and a stat for "exists",
  the scratchpads, `loaded` and `projectsMissing`) and hands it to the main
  loop with `g_idle_add` only when a signature of what is shown changed.
  A window on `/` or the home folder indexes nothing
  (`AgentView::indexesFolder` on the realpath). The files are
  `$MINICODE_CLAUDE_PROJECTS` or `~/.claude/projects` and
  `$MINICODE_CLAUDE_TMP` or `/tmp/claude-<uid>`.
- **Following changes**: GFileMonitor on the transcripts folder, the
  scratchpads' folder and each project folder below the first (a missing one
  through the nearest folder above it that exists), events coalesced 0.3 s,
  the set re-made at each refresh. Monitors are not recursive and a
  subagent's transcript is two folders down, so while the panel shows a 2 s
  poll asks for a refresh too (`Index::refresh` only stats and reads what
  changed). The transcripts folder going gives `projectsMissing`, and the
  panel's message is the core's `AgentView::emptyText`.
- **The panel** is `src/AgentsPanel.{h,cpp}` (GTK): a GtkListBox in the
  sidebar stack as the Processes panel is (`act_toggle_agents`,
  `win.toggleagents`, Ctrl+Shift+A, the Mac's Shift+Cmd+A; opening it closes
  Source Control and Processes, and Focus File Tree, reveal and Go to Folder
  close it). A click on a session or group opens or closes it (the Mac's
  click on a session shows its Evidence, which is part two); a file opens
  in the editor, a missing one (struck through, `<s>`) only bells. The row
  menu has Open and Copy Path for a file, Copy Transcript Path for a
  session, Adjust Settings… (surface `tree`) last. Colors are the Processes
  panel's `.minicode-procs` classes. Nothing here writes, moves or deletes
  Claude Code's files. The checks reach it as `MCGuiApp().agents`
  (`AppSeam`).
- **Build lists**: `AgentEvidence.cpp` and `AgentView.cpp` joined both lists.
- **Trap met**: the new View item made the View menu taller than the
  harness's 800 px Xvfb screen, so GTK scrolled the popover and Adjust
  Settings (last) was not on screen for `adjust_settings` and `themes`; the
  harness's screen is 1280x1024 now (`Wrapper.cpp`). A real 720 px screen
  would scroll that menu too.
- **Show Terminal** (November 2026): a session row's menu has Show Terminal
  when `TerminalAttention::terminalFor` (the board of item 42, by session id
  else transcript) names a terminal; it presents that window, shows the
  terminal and gives it the keyboard (`showTerminalOf`, also Next Waiting
  Terminal's). A link clicked in a terminal that has a session looks, when
  the path is relative and not in the shell's folder, in that session's
  scratchpad and worktree first (`Terminal::setSessionRoots`, read from the
  window's Agents model by session id; the core's resolution is plain
  `TermLinkPath`, not the Mac's `AgentIndex::resolveTarget`). Not checked by
  a suite: the link part (needs text in VTE and a session in the model).
- **The Worktrees node appears at once**: the top folder's watch also
  watches `.claude` and `.claude/worktrees` once they exist
  (`watchWorktreeFolders`, FileTree.cpp, made again when one appears), and
  any event there relists the top folder. `agents` no longer asks the tree
  to look again; it fails without the monitors.
- **Trap (suite order):** the Worktrees node is a row added above a tree an
  earlier suite left scrolled down, so it exists but is not mapped; the
  `agents` suite scrolls the tree up on each look (a full run only; alone
  the tree sits at the top). Not the watches: they fire and relist.
- **Trap (Evidence):** the page updates itself every 2 s and each change
  publishes the list without outputs, then with them. `outputsLoaded()` was
  set by the first and never cleared, so a tick in between left it true over
  a list with no output ends (the `agents` suite failed one run in three,
  and the steps after a failed check then failed too). It now follows each
  publish.
- **Trap (menus in checks):** running a row menu's action by name
  (`activate_action`) leaves its popover open over the rows, and a later
  `clickRow` lands on the popover (a menu item, or a dismissing click), not
  the row; `agents` calls `closeMenu()` after Show Terminal. Choosing an item
  with the pointer closes it.
- **Part three** (November 2026): a session's menu has Open Worktree in New
  Window (`agents.worktree`, `onOpenFolderInNewWindow` is `newWindow`) and
  Reveal Scratchpad in File Manager (`agents.scratchpad`, `onRevealFolder` is
  `revealFolder` in main.cpp), each only when the session's `worktreePath` or
  `scratchpad` is a folder that still exists, as the Mac's. Reveal opens the
  folder itself (`GtkFileLauncher` launch, not the containing folder, which
  would show the scratchpad's parent); `$MINICODE_FILE_MANAGER` names a
  program that gets the folder as its argument instead, which the `agents`
  suite records. The worktree node's Open in New Window already existed.
- **Remains:** the owner's eyes on the look and on a real `~/.claude`.

### 45. Agents checking pages (done, November 2026; `PARITY.md` item 3.7)

- **What it does**: the Mac's `browse` and `page`, on WebKitGTK. `browse`
  loads an address in a second web view in the browser pane, the agent page;
  `page --url`, `--text`, `--console`, `--screenshot` (`--full`,
  `--overwrite`) and `--reload` read or reload it, `--yours` the user's page.
  The rules are the core's `PageCheck` (addresses, the origin limit, the
  console record, the screenshot path), unchanged and already in the build
  lists; nothing copied. Text, console and screenshots only for local pages
  unless `control.page-any-origin`, checked again when the read comes back
  (a page that moved on, or was closed, while it was read gives an error and
  a finished screenshot is deleted).
- **The pieces**: `linux/src/BrowserPages.{h,cpp}`, `PageProbe`: one a web
  view, tracking the address, title, HTTP status (the main resource's
  response at COMMITTED), whether it loads, and waiters for `--wait` (15 s;
  a load error from `load-failed`, a cancelled load is no error). The agent
  view has a `WebKitUserContentManager` of its own: a script message handler
  and a listener in the script world `minicode-agent` (uncaught errors,
  rejections, relayed console calls) and, in the page's world, the Mac's
  wrapper of `console.*` that relays each call as a DOM event with a name
  made per view, so a page can only forge console calls (`fromPage`).
  `readText` evaluates in our world (`innerText`, 1 MB); `snapshotTo` uses
  `webkit_web_view_get_snapshot` (visible region, or the full document cut
  to `snapshotPlan`'s height), written with `O_EXCL | O_NOFOLLOW`, or with
  `--overwrite` a `mkstemp` file in the same folder renamed over a plain file.
  `Browser` holds a `GtkStack` ("yours" slot, "agent") under a Yours | Agent |
  x bar shown only while an agent page exists; `Browser::load` (an address
  typed in the bar, a link from the terminal) shows the user's page first.
  The user's web view is still made when the pane is first shown; `browse`
  with the pane never shown does not make it.
- **The keyboard**: `browse` shows the agent page and the pane
  (`ControlHost::showBrowser`, which keeps the keyboard where it was) unless
  the keyboard is in the pane (`pane_busy`, the CLI says so on stderr); the
  page still loads, and shows when the user picks Agent.
- **`open-url` and BROWSER**: the command takes an http, https or file
  address and puts it in the user's page of the window of the terminal
  (the window's id, as `notify`). `minicode open-url` in the client is no
  longer Mac-only: a page that is not local, or no minicode answering, goes
  to `xdg-open` (`$MINICODE_OPEN_FALLBACK` replaces it in the checks).
  Terminals get `BROWSER=<script>` and `MINICODE_EXE=<the app's binary>`
  (`Terminal::spawnShell`): `linux/resources/minicode-browser.js` is sh and
  JavaScript at once like the Mac's, and is copied beside the binary in the
  build folder and installed in `share/minicode`; `spawnShell` looks beside
  the binary, then in `../share/minicode`. In the guitest the binary beside
  `minicode-guitest` is `minicode`.
- **Traps**: WebKit opens a connection to a server with no request on it
  (preconnect), so a test server that reads on the main thread hangs the
  app: use `GThreadedSocketService`. An uncaught error from a `file://`
  script reaches `window.onerror` as "Script error." with no place (an
  opaque origin); a served page gives the message. The agent view must be
  added to the stack before it is shown, and loading works while it is
  hidden.
- **Checked by** the `browser_pages` GUI suite (`BUILD-LINUX.md`).
- **Not done**: the Mac's per-site page zoom is not kept for the agent page;
  a screenshot of a page taller than the cut is only checked through
  `snapshotPlan`'s unit tests, not on a real tall page; Vite's `--open` and
  Claude Code's links through BROWSER were not run.

### 46. The inline agent (done, November 2026; `PARITY.md` item 3.3)

- **What it does**: Ctrl+I (the registry's `inlineChat.start`, `win.inlineagent`,
  a row in `CommandsLinux.cpp`, Edit menu, the hints panel) opens the Mac's
  bar over the editor, below the caret's line (only that line is asked for,
  so the file is not laid out; it follows the scroll). A request goes to
  `agent.command` as the Mac runs it: `$SHELL -l -c 'exec <command>
  <InlineAgent::kPrintFlags>'` (`/bin/sh` without `$SHELL`; this machine has
  no zsh) in the folder, `PATH` widened with `LspSearchDirs`, `NO_COLOR=1`,
  the prompt from `InlineAgent::editPrompt` on stdin. Exit 127 or "command
  not found" says Claude Code was not found; five minutes at most.
  Core `InlineAgent.{h,cpp}` joined the Linux build lists; nothing copied.
- **The pieces**: `linux/src/InlineAgentBar.{h,cpp}`, one a window
  (`Editor::inlineAgent()`, made and deleted with the editor like the notes;
  `Editor` has it as a friend for `sourceText`). A worker thread (detached,
  checked against a generation and a live-set when it posts back) spawns
  with `g_spawn_async_with_pipes_and_fds`, polls the three pipes (stdin
  written as it takes bytes), feeds `InlineAgent::Stream`, and keeps a
  preview (`InlineAgent::preview`) for the main thread, which a 80 ms timer
  shows; the result comes back with `g_idle_add`. Esc while it works kills it
  ("Stopped."); a second Esc closes.
- **Pending, not made**: the blocks are applied to a copy
  (`InlineAgent::applyEdits`), the bar shows `pendingDiff` (removed and added
  lines in markup, 18 lines at most) and a text tag tints the replaced lines
  in the buffer (never text, never undo). Return with an empty field keeps:
  the rounds are made again on the buffer as it is (`moveOffset` for the
  selection), and when `sameChange` says that is not what was shown the new
  diff is shown and Keep waits for another Return; else the text goes in as
  the line runs that changed (`FormatRun::Replacement`, `MultiEdit::apply`,
  as Format Document), one Ctrl+Z. Esc drops it. A request typed while an
  edit waits is asked about the pending text and builds on it, its earlier
  requests in the prompt. If the buffer changed while Claude thought, the
  blocks are made again on it before the diff shows. A tab switch
  (`onActiveWillChange`) or the window closing closes the bar, and an edit
  waiting says in the status bar that it was not made.
- **Checked by** the `agent_panel` GUI suite (`BUILD-LINUX.md`): the real key,
  a stand-in script over `tests/fixtures/claude/stream`, no real `claude`.
- **Part two, the kinds** (`main.cpp` `act_inline_agent` decides by what has
  the keyboard, as the Mac): a text file or a Markdown preview in the editor is
  an `Edit` (on the preview the bar sits at the top and reads and changes the
  document's source buffer, `readSource`); an idle terminal a `Command`
  (`commandPrompt`, `commandFrom`, the line typed with `Terminal::typeText`,
  never run, the status bar says so); anything else (tree, panels, browser,
  picture, LaTeX preview, a busy terminal) a `Task`. Ctrl+Enter in the bar
  (its key controller, no registry row) makes any request a task. A task
  writes `InlineAgent::taskPrompt` to `$TMPDIR/MiniCode-agent/task-<pid>-<n>.txt`
  (0600; files of a process that quit or a day old removed as the next is
  written) and types `cd <root> && <agent.command> "$(cat <file>)"` into the
  window's terminal (shown and focused), for the user to read and run. The
  Linux port has one terminal, not tabs, so a busy terminal cannot take it:
  the status bar names the prompt file then.
- **Ctrl+I is Global** in `CommandsLinux.cpp`: plain Ctrl keys are the
  terminal program's, and Ctrl+I there is a Tab to vim's jump list; the
  inline agent in an idle terminal needs the key, so it takes it (Tab itself
  still goes to the shell). Say so if it is missed; a Ctrl+Shift key would
  be the alternative.
- **Closed from elsewhere**: `InlineAgentBar::focusMoved`, from the window's
  `notify::focus-widget`, closes the bar when the keyboard leaves the editor
  widget (tree, terminal, browser); an edit waiting says in the status bar
  that it was not made. A bar of a Command or Task kind does not give the
  keyboard back to the editor.
- **The tint follows edits**: while an edit waits the buffer's `changed`
  signal re-runs the blocks on the buffer as it is (120 ms after typing
  stops) and tints the lines they now replace (`retint`).
- Not done: a command's request does not send the terminal's screen
  (`CommandRequest::screen` is empty; only the folder).

### 47. The empty editor and hiding panes (done, November 2026; `PARITY.md` R4 and R5)

R4: `Editor::Editor` no longer sets the welcome text; the buffer shown with
no tab (`welcome_`, the name is the old one) is empty and read-only, so the
window is the Mac's empty editor. R5: only one path brought out a hidden
pane, `WindowLayout::collapseRightAreaIfEmpty` showing a hidden sidebar when
the right side was empty; it now returns, leaving a bare area (Toggle Editor,
Toggle Sidebar, or opening a file brings things back). The Mac's rule
(`RightArea::empty`) is unchanged and was right. `OwnerReportsSuite.cpp`
(`owner_reports`) checks both with real keys and a real bar drag.

### 48. The shell integration (done, November 2026; D10, `PARITY.md` item 3.1)

(The termprop route below was replaced by the host's reading of OSC 133 from the pty, item 90.)

- **What it does**: the app's terminal knows when its shell is at a prompt,
  when a command runs and its text, and the last command's status, so part
  two (`term`, `term-output`, Run File's busy check) can stop guessing from
  the foreground process group. VTE 0.84 reads no OSC 133 or 633, so the
  hooks set a string termprop of the app's own, `vte.ext.minicode.mark`,
  installed once with `vte_install_termprop` before the first terminal is
  made (`Terminal::installTermprops`), and read on its
  `termprop-changed::vte.ext.minicode.mark` signal.
- **The value**: `ESC ] 666 ; vte.ext.minicode.mark=p=<n>\sx=<n>\sd=<n>\ss=<status>\sc=<command>\smc=<nonce> ESC \`.
  D10's recipe wrote the fields apart with `;`, but VTE splits OSC 666 at
  `;` (the value stopped at `p=1`), so the hooks escape them as `\s` (and
  `\` as `\\`), which VTE unescapes. A value over 1,024 characters, or a
  bad escape, leaves the termprop unset, so the command's text is cut to
  500 characters. `ShellIntegration::parseVteMark` (core, header only)
  reads the value only when it ends in `;mc=<this shell's nonce>`, and
  `ShellState` keeps the newest: busy when x (lines started) is past d
  (finished; set to x at the next prompt, so a line of three commands
  counts once), at a prompt otherwise, the text while busy, the status.
  VTE keeps only the last value of a read, so a command's output in the
  same read as its start mark hides the start (seen in the suite: a
  `printf` at once after it); the counters show the jump, and the state is
  right again at the next mark.
- **bash** (`linux/resources/minicode-bash-integration.bash`): the shell
  is started `bash --rcfile <copy>`, interactive and non-login, as the port
  and GNOME Terminal started it before, so the desktop session's login ran
  `~/.profile` already. bash reads `/etc/bash.bashrc` itself (Debian's
  SYS_BASHRC, which `--rcfile` does not replace); the script then takes
  `MINICODE_NONCE` out of the environment and reads `~/.bashrc`, which is
  all a non-login bash reads, so no profile branch is needed. The hooks,
  installed after `~/.bashrc` and only with a nonce and bash 4.4 or later:
  PROMPT_COMMAND gets a first entry that keeps `$?` for the user's entries
  and ours last (an array from bash 5.1, else joined by newlines); PS0
  (bash 4.4) gets `${BASH_VERSION:0:$((__minicode_x+=1,0))}` (an
  arithmetic expansion that moves the counter in the shell itself and
  expands to nothing) and a command substitution that prints the mark. The
  text is the history's newest line when `HISTCMD` moved past the prompt's
  (a line the history did not keep, under ignorespace or ignoredups or
  with history off, has none). No DEBUG trap. The user's prompt, history,
  options, PROMPT_COMMAND and PS0 are kept (the suite checks a .bashrc's
  PROMPT_COMMAND and alias).
- **zsh**: the Mac's script (`resources/minicode-zsh-integration.zsh`,
  copied by Meson beside the binary and installed, never forked) with
  `__minicode_term=vte` in the .zshenv stub (`zshenvStub(grid, vte)`):
  `__minicode_mark` keeps counts at D and E and prints the termprop at A
  and C; B prints nothing; OSC 7 goes out without the nonce, since VTE
  takes everything after `7;` as the address. ZDOTDIR is the folder,
  `MINICODE_USER_ZDOTDIR` the user's, and the exec rules are the Mac's
  (an `exec zsh` gets the nonce and starts its counts again; the app reads
  only the newest value, so counts going back are harmless). zsh is not
  installed on this machine or the ThinkPad: this half has not run here.
- **The half-typed line**: before every command the app types (`term
  --run`, Run File, Open in Terminal, the inline agent's commands and
  tasks: `Terminal::runLine` and `typePending`) it sends the private key
  `ESC [9999~` (`Terminal::setLineAside`), only to a shell with the
  integration whose marks say it is at a prompt and with no program in the
  foreground, so a half-typed `rm ` never joins `python3 file.py`. zsh:
  the Mac's `__minicode_push` (`zle push-input`) brings it back by
  itself. bash: `bind -x` on `\e[9999~` in emacs, vi-insert and vi-command
  runs `__minicode_push`, which puts `READLINE_LINE` on a stack and empties
  the line; the app then owes a pop and, at the first prompt mark after
  (`giveLineBack`), sends `ESC [9998~` (`bind -x` `__minicode_pop`: the set-aside
  text goes in front of the line, the caret after it). Chosen over
  readline's kill ring because a user's own yank, yank-pop or kill between
  would change or lose the text. A Ctrl+C at a prompt draws no new prompt
  mark, so the text waits for the next command's prompt. Never sent into a
  program, a busy shell, or a shell without the nonce. Checked by
  `shell_integration` (`echo LEFT` half-typed, `term --run 'echo RIGHT'`
  and Run File, a user's own `bind` in the .bashrc); the suite fails
  without the key. zsh's half has not run here (no zsh).
- **Trap: PS0 printed as text (CI, 2026-11-12)**: PS0 was
  `${BASH_VERSION:0:$((x+=1,0))}$(...)`; on CI's bash its leftover
  (`BASH_VERSION:0:0}`) showed before every prompt and command output,
  breaking `term-output` and the half-typed-line checks (the ThinkPad never
  showed it; the exact CI cause was not reproduced: `shopt -u promptvars`
  prints the whole string, a long path and `set -o posix` do nothing).
  The text now holds no `${...}`: an OSC 7777 (ignored by VTE, ended by BEL,
  since `\` + `$` in a prompt is an escape) wraps the counter's `$((...))`,
  and `shopt -s promptvars` is put back after `~/.bashrc`. The suite reads
  `vte_terminal_get_text_format` (soft wraps joined) and
  `MC_SHELL_TEST_BASHRC='shopt -u promptvars'` adds lines to its .bashrc.
- **Other shells** (fish, sh, dash, anything else in `$SHELL`) start as
  before, with no nonce, and stay unknown. `exec` into another shell, or
  ssh, leaves the state as the last mark said: busy with that command.
- **The files**: `Terminal::integrationFolder` copies both scripts and the
  stub once a process into `$XDG_RUNTIME_DIR/MiniCode-zsh-grid-<pid>-<start>`
  (`ShellIntegration::folderName`, the start in seconds since boot as
  `BackupStore::processStart` gives it), never the shared `/tmp`; a folder
  that is not a 0700 directory of the user's is refused. The first look
  removes the folders of processes gone; `atexit` removes this one's.
- **The nonce**: 16 bytes from `getrandom` as hex, a new one for each start
  of the shell (the state goes back to unknown with it). `MINICODE_NONCE`,
  `MINICODE_USER_ZDOTDIR` and `MINICODE_EXEC` inherited from a minicode
  terminal are dropped first. As D10 says, the user's own programs can read
  it from `/proc/<pid>/environ`; it refuses marks that are only printed.
  `/etc/bash.bashrc` runs before the script takes it out of the environment.
- **`state`**: each terminal's entry has `busy` (null while unknown),
  `command` (the running line's text, else null) and `status` (the last
  finished line's, null before one). `Terminal::shellState()` and
  `onShellStateChanged` are part two's.
- **Checked by** the `shell_integration` GUI suite and `make test`'s
  `shell:vte-mark`, `shell:vte-state`, `shell:zshenv-vte`,
  `bash:integration` and `zsh:vte` groups (`BUILD-LINUX.md`).
- **Trap: bash's screen after a width change.** VTE rewraps lines when the
  terminal's columns change (the terminal's zoom), and bash redraws its
  line for the old wrapping: with a prompt or line that wraps, the prompt
  lands over earlier output (`echo cols=eric@...`) or everything below the
  old prompt is erased. CI's 93-column prompt on 104 columns, after `zoom`,
  left `processes` waiting 30 s for typed text that never showed on the
  screen (an `MC_GUI_ROOT` 44 characters long gives CI's prompt width here
  and shows the garbled screen, though not that failure), with
  the lines typed there since `folding` gone; f2efeaf passed only because
  "true" was elsewhere on the screen. A
  check after another suite has used the terminal runs its line with
  `runLine` and reads the marks, not the screen. The same look found
  `recentText` (`term-output`) leaving out the newest row: VTE's end
  column is exclusive on the last row, so -1 read nothing of it.
- **Not done (part two)**: `term` and `term-output`, Run File's busy check
  and `onCommandFinished` from the marks instead of the foreground group's
  poll, an exec into another shell (the Mac's `_integrated` NO), and the
  owner's own shell setup on a real desktop.

### 49. Files by kind and the CSV table (part one of `PARITY.md` item 4.1)

- **Routing**: `Editor::loadInto` (Tabs.cpp) reads the first 8 KB and asks
  `FileSniff::classify` after the media check. A SQLite database and a
  binary file or Office-type document (the Mac's Quick Look kinds, which
  have no counterpart here) become a `Document::Kind::Message` saying so,
  never text. UTF-16 with a BOM is still refused by `readText` (not valid
  UTF-8), as before. JSON and notebooks are text until their viewers come.
- **The table** (`CsvTable.{h,cpp}`): a `GtkColumnView` over a
  `GtkStringList` of row numbers, one `GtkEditableLabel` per cell, the
  cells read from a `Csv::Document` parsed from the tab's text. A cell
  edit serializes the table, takes the bytes between the old and new text's
  common start and end (whole characters), and `Editor::showTable`'s
  `onReplace` makes that one user action on the tab's own buffer, so undo,
  dirty, backups, saving and the servers work unchanged; one cell changes
  one line, an unedited file is never rewritten, and BOM, CRLF, quoting and
  a missing last newline come from Csv. Text changed any other way (undo,
  Ctrl+Z on the table calls `gtk_text_buffer_undo`, a reload) reaches the
  table by the buffer's `changed` signal (`csvApplying_` skips its own).
- **Choosing**: `Document::sniffCsv` (from `loadText` and `restoreText`)
  asks `FileSniff::classify` on the text; `isCsv()`, `showsTable()`. A file
  over 50 MB (`CsvTable::kMaxTableBytes`) or empty opens as text; the Mac's
  read-only 100,000-row table for those is not done. Files over the
  large-file background threshold are read in pieces and so open as text
  too. The preview key (`Editor::togglePreview`) flips table and text.
- **Trap**: `Editor::dropTable` (called on leaving a tab and before every
  show) commits a cell being typed into the tab it was typed in, then
  disconnects; the table is one widget for every tab, as the Mac's.
- **Not done**: inserting and deleting rows and columns (Csv has them), the
  first row as a header, multi-line cells typed in a cell, column widths
  kept.

### 50. Browser tabs (tabs done, November 2026; `PARITY.md` item 4.5, first part)

- **What it does**: the browser pane has the Mac's tabs. A `TabStrip` (the
  editor's, now with an opt-in `onMove` for dragging, `EditorTabs::dropIndex`)
  and a + sit above the toolbar. `Browser` keeps a `Tab` per page (address,
  title, WebKit's serialized session state, and a web view and `PageProbe`
  only while live). `BrowserTabs::livePages` (kLivePages 2) decides which
  keep a web view: the tab on show and the one used before it; the others
  are suspended in `applyLive` (`webkit_web_view_get_session_state`, then
  the view is removed from its box, which ends its WebKit processes) and
  made again on show (`restore_session_state`, then
  `go_to_back_forward_list_item` on the current item; no record loads the
  address). Placing, stepping, numbering, closing and dragging are
  `EditorTabs`' rules.
- **Probes follow the tab**: `userProbe()` and `webView()` are the tab on
  show's, null while it has no view (before the pane was first shown, or a
  restored tab). The control channel's `page --yours` reads the same.
- **Links**: `decide-policy` ignores a Ctrl-clicked or middle-clicked link
  and opens its address in a suspended tab behind (`newTab(url, false)`);
  `create` (target=_blank, window.open) makes the tab right of its opener
  with a view whose `related-view` is the opener's (WebKitGTK 6 has no
  `new_with_related_view`) and shows it.
- **Keys** (D1; the Mac's Cmd is Ctrl, no new letter): New Browser Tab is
  `minicode.browser.newTab`, Ctrl+T, a `Page` command (Go to Symbol in
  Workspace is `NotPage`, so the key is Safari's in a page only); View > New
  Browser Tab from anywhere. Close Tab (Ctrl+W) closes the agent page when it
  shows, else the browser's tab, and the last one calls `Browser::onEmpty`
  (the window hides the pane; showing it again makes a home tab). Next,
  previous and numbered tab (Ctrl+PageDown, Ctrl+PageUp, Alt+1 to 9) act on
  the browser's tabs while the pane has the keyboard and has more than one;
  with one they go to the editor's, as on the Mac.
- **Session**: `TabSessions::Session` gained `browser` (url, title, state in
  base64) and `browserActive`; old files read as before and a folder with a
  browser but no file tab is kept. `Persistence::Host::browserTabs` fills it
  (`Browser::records`, `BrowserTabs::keptRecords` limits 2 MB a tab and 4 MB
  in all), `Browser::onChanged` schedules the save, and `main.cpp` restores
  it after the files with `Browser::restore`: every tab suspended, the one on
  show loads when the pane is shown. Not kept yet: the pane's visibility and
  width, and page zoom per site.
- **Checked by** `browser_tabs` (`BrowserTabsSuite.cpp`, real clicks, a real
  drag and real keys; pages are files in the project folder): the points in
  its opening comment. The session check goes through the JSON and into a
  second, unmapped `Browser`; a quit and relaunch is not run.
- **Traps**: the `create` handler must make the view (with its related view)
  before `selectTab`, whose `applyLive` would otherwise make a plain one for
  the active tab. A signal's user data is the `Tab`, disconnected before the
  view is removed.

### 51. LaTeX problems, warnings and scratch records (done, November 2026; `PARITY.md` item 4.4, first part)

- **What it does**: a typeset that fails lists tectonic's problems
  (`LatexLog::collect`: the summary's `error: file:line: message`, TeX's
  `! ...` blocks only when the summary placed none) as rows above the log, in
  `Latex.cpp`; a row is a flat button that calls `LatexPreview::setOpenProblem`'s
  function with the file `LatexLog::sourcePath` finds (the sibling's name is
  the document, an `\input` name is read against the document's folder) and
  the line, and `Editor::makeLatex` opens that file in a tab and `revealLine`s
  it (a row with no line or no file is insensitive). The failure page is
  now `logPage_` (rows over the log), which `showingLog` compares against.
- **Warnings**: every run passes `--keep-logs`; tectonic's own `warning:`
  lines and TeX's transcript (`<stem>.log` beside the PDF, read and removed
  at once, whatever came of the run) go through `LatexLog::collect`. A typeset
  that worked shows a "N warnings" flat button in the preview's bar (not the
  status bar, which this port keeps for the page count) that opens a popover
  of the same rows. Underfull boxes are left out, as on the Mac.
- **Gutter marks**: the lines are tagged `latex-error` and `latex-warning` in
  the document's own buffer (an underline like a server's; the gutter's
  default `diagTags_` names them beside `lsp-error` and `lsp-warning`), so a
  language server's publish, which clears only its own tags, never replaces
  them. Marks are laid only when the buffer still equals the text typeset,
  only on lines of the document itself (not an `\input` part), and come off
  at the first change to the buffer (`onBufferChanged`) or the next typeset
  (a good one lays only its warnings). GTK tags follow edits, but the
  rule here is the Mac's: they are the typeset text's lines, so they go.
- **LatexScratch**: each preview's output folder is
  `LatexScratch::folderName(<16 hex of the path's SHA-256>, owner)` under
  `$XDG_RUNTIME_DIR/minicode-latex` (so two windows on one file share
  nothing), removed with the preview (destructor, `close`). A
  `sibling.record` (pid, start time from `/proc/<pid>/stat`, size, SHA-256)
  is written before the hidden copy and removed with it; the first typeset of
  a launch (`sweepOldFolders`) removes the folders of owners that have gone
  and, by `LatexScratch::decide`, a copy a crashed process left only while it
  is still exactly the recorded bytes. Folders named by the hash alone
  (builds before this) are left alone. The sibling's own name stays
  `.<file name>.<preview number>.minicode.tex` (which `isPreviewCopyName`
  accepts); that is the port's, not the core's `siblingName`.
- **Checks**: `latex_problems` (`tests/gui/LatexProblemsSuite.cpp`) stands a
  shell script in for tectonic (`MINICODE_TECTONIC`), giving it canned
  output made of tectonic 0.17.0's real lines (`tests/latex/logs`): the
  error row, its mark and click, marks gone after typing, the five warnings
  (tectonic's boxes with an underfull one dropped, the transcript's, one in
  an `\input` part whose row opens that file at line 3), the transcript, the
  record and the copy gone, and a dead process's folders swept (its copy
  removed when unchanged, kept when the user had changed it).
- **Not done**: forward search (Option+Ctrl+J), the rest of 4.4. The marks
  do not follow edits (they leave), a part's own problems are not marked in
  the part's buffer when it is open, and a folder left by a rename of the
  document (`setPath` keeping the pages) waits for the next launch's sweep.

### 52. The JSON outline and notebooks (done, November 2026; `PARITY.md` item 4.1, part two)

- **The outline** (`JsonOutlineView.{h,cpp}` over the core's `JsonOutline`):
  a `GtkListView` over a `GtkTreeListModel` (one `GObject` a node, carrying
  its index; children made when a row opens), right of the text behind a
  `GtkPaned` divider that wraps the slot's overlay (`Editor::outlinePaned_`, hidden rather than removed when a tab has none,
  so the find bar stays over the text). `Editor::syncOutline` (end of
  `showActive`, and the preview key) shows it for a `Document::showsOutline()`
  tab and `dropOutline` (in `stash`) disconnects it; the preview key
  (`togglePreview`) hides and shows it. A row chosen (click, arrows, Enter)
  selects its text in the buffer with the keyboard staying in the outline;
  a member selects from its key's opening quote. `notify::cursor-position`
  on the buffer chooses the deepest node under the caret (`nodeAt16`). The
  core's offsets are UTF-16, the buffer's are characters: the view keeps the
  parsed text as UTF-16 and converts with `utf16::toCharOffset` and
  `fromCharOffset`.
- **Parsing**: `schedule()` marks the outline stale at once (no row chooses
  anything, either way, while stale) and parses 0.3 s after the last change.
  A good parse rebuilds the tree and the editor looks for the caret's node
  (`onParsed`); a bad one keeps the last good outline, dimmed (opacity 0.5)
  under an error line ("Line 3, column 5: ..."), still stale. A file that
  fails at its first parse shows the nodes read so far, dimmed. `.jsonc`
  allowances come from `JsonOutline::allowsComments` (`Document::jsonc`).
  Files over 16 MB (`Document::kMaxOutlineBytes`) and trees over 200,000
  nodes are not outlined; `Document::sniffCsv` (now also the JSON and
  notebook kind, from `FileSniff::classify`) decides.
- **Notebooks**: a .ipynb is `Document::isNotebook()` and starts with
  `showsPreview()` on: `renderPreview` renders `Notebook::markdownForFile` of
  the buffer through the Markdown page, read-only (its source lines are not the
  buffer's, so a double-click edit and a task box do nothing). The preview key
  flips to the raw JSON with the outline and back. Pictures are `data:image/`
  addresses, now decoded by `Markdown.cpp`'s `loadPicture` (png, jpeg, gif and
  whatever gdk-pixbuf reads; an SVG shows its alt text if no loader). Nothing
  runs.
- **Checked by** `json_outline` (`JsonOutlineSuite.cpp`): the points in its
  opening comment, with a real click on a row.
- **Not done**: the outline's width is not kept, a notebook's scroll place is
  not kept across tabs, a row click on the row already chosen selects nothing
  again (Enter does), and SQLite (the rest of 4.1).

### 53. Find in the page and page zoom per site (done, November 2026; `PARITY.md` item 4.5, second part)

- **Find**: `Browser::showFind` opens a small bar over the page's top right
  (an overlay in the pane: a `GtkSearchEntry`, the count, previous, next, x)
  on `WebKitFindController`: typing searches (case-insensitive, wrapping)
  and also asks `count_matches`, because `counted-matches` fires only for
  that, never for `search`; the label is "N matches" or "No results".
  Enter and Shift+Enter (the Mac's) step, Esc or x closes it and the page
  gets the keyboard back. It closes on a tab switch.
- **Keys** (D1, no new command): Ctrl+F, F3 and Shift+F3 are the editor's
  Find actions; `act_find`, `act_find_next` and `act_find_previous` in
  `main.cpp` hand them to the browser when `browserHasKeyboard` (page or
  address bar) and it has a page, else the editor's bar as before.
- **Zoom per site**: the page zoom keys already worked; now the level is kept
  by site (`Browser::siteOf`: the host, "file" for local files, nothing for
  about:blank) in `$XDG_STATE_HOME/minicode/browser-zoom.txt` (host, a tab character,
  level; 100% forgets the site), applied when a page commits, so any tab and
  any launch opens a site at its zoom, and the site's other live tabs follow.
  Zoom to Fit Width is not ported (the registry has the Mac's command; the
  GTK port has no row for it yet).
- **Checked by** `browser_find` (`BrowserFindSuite.cpp`, real keys): Ctrl+F
  in the page and from the address bar opens the page's bar and not the
  editor's, the count, stepping, Esc returning the keyboard, the editor's
  Ctrl+F unchanged, zoom following a new tab and a tab switch, and the state
  file. A relaunch is not run.
- **Traps**: `counted-matches` never fires without `count_matches`;
  zoom set by the user must be written before the commit handler reapplies
  the site's.

### 54. The Markdown page to the side, in pieces (done, November 2026; `PARITY.md` item 4.2, first part)

- **Files**: `src/MarkdownSide.{h,cpp}` (the pane), `Markdown::renderRuns` in
  `Markdown.cpp` (the page builder over a run range, inserting at a character
  offset; `render` is it over every run), `MarkdownPage.cpp` joined the core
  lists (`meson.build`, `Makefile`; Android's CMake has not got it, since it
  has no side pane). `Editor::syncSide`/`dropSide`/`toggleSidePreview`, and
  `Document::sideWanted`.
- **The key** is Ctrl+Alt+V (`markdown.showPreviewToSide`, a `markdown`
  language command in the registry): VS Code's Linux key is the chord Ctrl+K V
  and the port has no chords, so it is the Mac's Ctrl+Cmd+V with Ctrl+Alt for
  Cmd, as the task key's Ctrl+Alt+L is; GNOME takes no Ctrl+Alt+V.
- **Where it sits**: the end child of `outlinePaned_`, the JSON outline's
  place (a Markdown tab has no outline, a JSON tab no side page; both
  widgets are ref'd so the paned can swap them). A GtkTextView of its own,
  read-only, with a buffer of its own on the editor's tag table. It shows only
  while the tab shows its source: the full preview (a Markdown file opens as
  the page here, so the pane comes with Ctrl+Shift+V to the source) and
  another tab drop it and clear its page; it is built again when it returns.
  The wish is per tab (`Document::sideWanted`) and not kept in the session:
  the port keeps no per-tab modes there.
- **Pieces**: a buffer "changed" schedules a render 0.3 s later. The text is
  copied on the main thread; `MarkdownParser::parse`, `MarkdownPage::split`
  and `match` run on a GTask worker (generation counted; one job at a time,
  another asked for while it runs follows it). On the main thread
  `MarkdownSide::apply` walks the new pieces: a kept one keeps its text, its
  widgets and its `Markdown::Page` (offsets are the piece's own, so only the
  source lines move, by the change of the piece's base line); the old pieces
  between kept ones are taken off the view (widgets first, or GTK 4.22
  crashes in the buffer's deletion, as `clearPage` says) and deleted from the
  buffer in one range; an unmatched piece is built at its place by
  `renderRuns`. A piece holding a web picture still being fetched is not
  matchable (`keep` false), and `placeArrived` puts a picture in, as the full
  preview does, per piece. A new context (the file's folder, web pictures
  on or off) is built whole (`setContext`, `invalidate`).
- **Measured** (the main thread's CPU for a one-letter edit, from the
  worker's answer to the splice done, `Stats::mainCpuMs`): 0.7 ms at 1,200
  lines (607 pieces, 1 built), 4.5 ms at 20,000 lines (10,101 pieces, 1
  built); the worker's 5 and 93 ms are off the main thread. The Mac bounds
  10 and 30 ms. The first page is the full cost of every piece built, and a
  README's tables (4.2's R1 figures) are in it: not measured here.
- **Checked by** `markdown_side` (`MarkdownSideSuite.cpp`): the key opens and
  closes the pane, the page against the source, an edit building one piece
  and leaving every table widget the same pointer, a line added above, an
  edit inside the table building a new widget, the full preview hiding the
  pane and bringing it back, a second tab with no pane, a text file where the
  key does nothing, and the CPU bounds (15 ms and 40 ms, with headroom for a
  loaded machine) at 1,200 and 20,000 lines.
- **Scroll sync** (`MarkdownSide::setSource`, `markdown_side_sync`): the
  Mac's rules over the core's `PreviewScroll`. Marks are (source line,
  character) from the pieces' spans, rebuilt lazily after a render; a mark's
  top is asked of the pane's layout (`gtk_text_view_get_iter_location`) only
  for the two marks around the place (`PreviewScroll::around`), and the page
  to source direction finds its mark by the character at the pane's top
  (`get_iter_at_location`), so no question walks the page. GTK lays a view
  out lazily and revises line heights, so a target is held: re-applied every
  80 ms while the target adjustment's upper bound still moves, until it has
  been still for four ticks, and dropped on any user move. Our own scrolls
  set `holding_`, and a value-changed that leaves a side as last seen, or
  came with its upper bound moving (GTK keeping a line still), is no move of
  the user's. The pane leading sets `pageLeads_` (a render then leaves the
  page alone); scrolling the source or typing in it clears it. The caret
  (`notify::cursor-position` on the tab's buffer) brings its block into the
  pane, a third down, only when it is not in view. Trap for checks:
  `gtk_text_buffer_get_text` leaves out child anchors, so offsets from the
  page's text are off by the tables before them; search the buffer with
  iters. Measured: 0.3 to 0.7 ms of main-thread CPU for a sync at 20,000
  lines.
- **Editing from the pane** (`markdown_side_edit`): a double-click on text in
  the pane (a capture-phase gesture on the pane's view, claimed so the view
  does not take the keyboard from the editor once the button is up; a click
  that lands on a table or picture child is left to its own gesture) or on a
  table cell (the `cellDoubleClick` hook) calls `Editor::editMarkdownBlock`
  with `inPane`: the same popover as the full preview's, now parented to
  whichever view it is over (`showMdPopover`; dropped and made again when the
  parent changes; it grabs the keyboard when mapped, since a grab before
  that is lost). A span's line is the source line, so the block is found with
  `MarkdownEdit::blockAt` on the source; the commit is
  `applySourceFromPane`: `Document::spliceText` (one undo step), the caret
  kept on its line and the source's scroll kept, and the pane renders by its
  own 0.3 s path. A click while the page is older than the buffer
  (`MarkdownSide::idle()` false) renders first and edits nothing; a block
  edited while the buffer changed underneath is not applied, and its text goes
  to the clipboard (with an error bell; the editor has no status line to
  say it). A kept piece whose lines moved updates the lines its table cells
  and task boxes carry (`Markdown::shiftLines`). A task box in the pane
  toggles the task in the source (`toggleSideTask`). Trap for checks: the
  paned's handle takes the pointer in the pane's first ten pixels, so click a
  little in from the edge. The task box has no check of its own yet.
- **Not done**: heading anchors are per piece (`#section` links in the pane do
  not resolve); the pane's width is the paned's default and is not kept;
  the page is not kept across a switch to another tab.
  Trap: "upper changed since the last value-changed" alone made a later user
  scroll look like layout (the last seen upper went stale under lazy layout, so a
  full run never followed a 20,000-line jump); it counts only within 50 ms of
  a `notify::upper`.

Table cost (R1, 2026-11-12; `markdown_tables`): opening a copy of README.md
(10 tables, about 540 cells) held the main thread 439 ms of CPU in the full
preview and 617 ms in the side pane (the longest stall between two fires of a
5 ms timer; total CPU 754 and 1,159 ms). Building the tables was only 39 ms of
it; the rest was GTK measuring wrapping labels, because `PANGO_WRAP_WORD_CHAR`
with `gtk_label_set_max_width_chars(1)` makes each label measured at many
widths. A cell now wraps at words (`PANGO_WRAP_WORD`) with no max width, and
only a cell holding a word over 18 characters keeps `WORD_CHAR`; `fit()` still
gives each label its width. Now 80 to 100 ms of stall (bounded at 150 in the
suite), total 360 to 500 ms spread over several idle slices. Looks, links,
the cells' double-click and the labels' selection are unchanged. Tried and
worse or no better: wrapping only once `fit()` has run. What remains if it
must go lower: one custom widget per table, or building a table when it
scrolls near.

### 55. LaTeX forward search (done, November 2026; `PARITY.md` item 4.4, second part)

Ctrl+Alt+Shift+J (`latex-workshop.synctex`, "Show Line in PDF"; LaTeX
Workshop's Ctrl+Alt+J is Unfold All's here, an Editor command, so Shift joins
it) lights the caret's line in the PDF. `Editor::showLineInPdf` finds the
document to search: the caret file's `% !TEX root` comment
(`LatexLog::rootComment`, realpath), else, for a file without
`\documentclass`, the open tab whose preview's typeset read it
(`LatexPreview::readsFile`, `SyncTexIndex::tagForFile` with realpath on both
sides); that tab is shown. `LatexPreview::forwardSearch(file, line)` goes
through `pdfForBuffer`, so a stale PDF is typeset first and the light comes
when it arrives; `boxesForLine` gives the boxes (the nearest line below, then
above, within 40, when the line put nothing), `PdfView::setHighlights` draws
them as translucent rectangles over the page (in `snapshotPage`), the page is
scrolled to the first box with 80 points above it, and the light goes after
1.5 s. Check: `latex_forward`, a stand-in tectonic copying the committed
`tests/latex/fixtures/main.*` typeset (Input paths rewritten by `sed`).
Not done: the Mac's menu item (the port has no View entry for it; the hints
panel has a row).

### 56. Export as HTML and Mermaid diagrams (done, November 2026; `PARITY.md` item 4.3, first part)

- **Export as HTML…** is the action `win.exporthtml` (registry id
  `minicode.markdown.exportHtml`, no key; the palette lists it), enabled for a
  Markdown tab (a notebook excepted). `Editor::exportHtml` asks a GtkFileDialog
  (the file's folder, `<name>.html`) and `exportHtmlTo` writes the core's
  `MarkdownHtml::page` of the buffer (unsaved changes included) through
  `FolderSearch::writeFileAtomically`. The harness sets
  `MINICODE_HTML_EXPORT_PATH` to skip the dialog (the Mac's
  `MCHtmlExportPathChooser`). The link and picture rules are the core's.
- **Mermaid**: `src/Mermaid.{h,cpp}` runs mmdc (`$MINICODE_MMDC`, else
  `lspgtk::findProgram("mmdc")`) from one worker thread, a job at a time,
  each in a session of its own (`setsid`, killed by group) with 90 s, with
  `MermaidDiagram::arguments` (transparent PNG, scale 2, `dark` or `default`
  by `Settings::isDark`). Pictures are kept as GdkTextures in memory and in
  `$MINICODE_MERMAID_CACHE` or `$XDG_CACHE_HOME/minicode/mermaid` (50 MB,
  trimmed to 40); a failure is remembered 30 s and its complaint
  (`errorSummary`) is the code's tooltip. `want()` is the one entry: kept,
  waiting (queued on first ask) or failed; nothing runs for a page with no
  mermaid block, with `markdown.mermaid = false` or without mmdc.
- **In the page**: `Hooks::mermaid`/`mermaidDark`; `renderRuns` finds the
  blocks of its run range (`MermaidDiagram::blocks`), and a kept diagram is a
  picture widget (half its pixels, scale 2) anchored for the block's runs. A
  waiting one stays code and counts in `Page::waiting`; a failed one stays
  code under an anonymous-by-text tag `md_tip:<complaint>` that
  `Markdown::installTooltips` (connected to the editor's view and the side
  view) shows. On an arrival (`Mermaid::subscribe`) the full preview is
  rendered again keeping its scroll (`rerenderPreview`), and the side pane
  treats a piece with `waiting` as unmatchable like one waiting for a web
  picture, so only those pieces are built again. A change of theme or of
  `markdown.mermaid` re-renders both.
- **Checked by** `mermaid` and `export_html` (`MermaidSuite.cpp`; a stand-in
  mmdc script, no real one): code before the picture, the picture after, the
  dark and default themes reaching mmdc, a reopen answered by the disk cache
  with no run, a failure's tooltip without the stack trace, the setting off
  and a page without a diagram running nothing, the side pane, and the
  exported page's key lines.
- **Not done**: math (4.3's second part); Mermaid on Android (no pane there).

### 57. The SQLite viewer (done, November 2026; `PARITY.md` item 4.1, last part)

- **Routing**: `FileSniff::Kind::Sqlite` makes a `Document::Kind::Database`
  (`Document::setDatabase`, no text in the buffer), shown by `SqliteView` in
  the slot's `sqlite` page; `stash` clears it, a rename calls `setPath`.
  `csv_table` used to check the old "no database viewer" message and now
  checks the kind.
- **The view** (`SqliteView.{h,cpp}`, libsqlite3, `dependency('sqlite3')`,
  `libsqlite3-dev` in debian/control, the GTK CI job and BUILD-LINUX.md): a
  `GtkPaned` with the tables and views (tables first) in a `GtkListView`, and
  a query entry, a status line and a `GtkColumnView` of the answer. Picking a
  table runs `SELECT * FROM "name"` (quotes doubled); Enter in the entry runs
  the query. NULL is a dimmed italic "NULL", a BLOB "(BLOB, N bytes)", a cell
  is cut at 4,000 bytes. A fresh file shows its first table.
- **Read only, as the Mac's**: `SQLITE_OPEN_READONLY | SQLITE_OPEN_URI` on
  `file:<percent-encoded path>?mode=ro`, plus `&immutable=1` for a WAL
  database (bytes 18 and 19 are 2) with no `-wal` beside it; the defensive
  flag; `PRAGMA query_only=ON`; an authorizer denying BEGIN/COMMIT/ROLLBACK,
  SAVEPOINT, ATTACH, DETACH and a pragma given an argument unless it is a
  reading one (`table_info` and friends); a statement with
  `sqlite3_stmt_readonly` false is refused after it is prepared and never
  stepped; only the first statement runs (the tail is ignored); a progress
  handler (every 1,000 VM steps) ends a query at 5 s ("Stopped: ...") or when
  a newer request arrives; 1,000 rows at most, with a note.
- **Threads**: one `std::thread` owns the connection and does everything,
  opening included. `submit` sets `cancel_` under the queue's mutex (the
  worker clears it as it takes a job, so it ends only a query already
  running); results go back by `g_idle_add` holding a `shared_ptr<Link>`
  that the destructor empties, and carry a generation: an answer for a
  request a newer one replaced is dropped, and `busy()` counts requests not
  yet delivered. The destructor joins (a running query ends at the next
  progress step).
- **A reopen**: the file is watched (`GFileMonitor`, a change settled for
  400 ms); `show` of the same path opens again and runs the query that was on
  show, and the table list keeps its pick. The editor calls `show` whenever
  the tab is shown.
- **Checked by** `sqlite_view` (`SqliteViewSuite.cpp`, databases made with the
  C API): tables, rows and NULLs, a query, the 1,000-row cap, the first
  statement only, INSERT, UPDATE, DELETE, CREATE, BEGIN, SAVEPOINT, ATTACH and
  `PRAGMA user_version = 5` refused with the file's bytes unchanged and no
  journal or second file made, a reading pragma answered, a WAL file without
  -wal opened, a file that is not a database, a reopen keeping the query, and
  an endless recursive CTE stopped at about 5 s with the main thread's CPU
  bounded.
- **Not done**: sorting by a column, a cell's full text (a tooltip or copy),
  column widths kept, the table list's width kept, and a query history.

### 58. Math in the Markdown preview (done, November 2026; `PARITY.md` item 4.3, second part)

- **`src/Math.{h,cpp}`** is the Mac's `MarkdownMath.mm` over the core's
  `MathTex` (the checks, the batch document, the nonce, `readLog`,
  `firstError`; nothing of that is re-done). `Math::want(tex, display,
  color)` is the one entry, like `Mermaid::want`: kept, waiting (queued on
  first ask) or failed. Requests gather for 50 ms into one batch; one run at
  a time on a worker thread: tectonic (`LatexPreview::tectonicPath()`, so
  `MINICODE_TECTONIC`, the downloaded copy, PATH) with `-Z continue-on-errors
  --untrusted --print --keep-logs --reruns 0` in a `g_dir_make_tmp` folder
  of its own, in a session of its own (killed by group), 15 s without output
  once the log shows `BEGIN:` and 60 s before (the formula TeX was in fails,
  the rest go round again); a run with no PDF fails the formula TeX began and
  never finished and reruns the rest; a page must be the size the log gave
  its box. Compiled only with poppler (`MINICODE_ENABLE_PDF`); without it
  `Math::built()` is false and a formula stays its TeX.
- **Cache**: `$MINICODE_MATH_CACHE` or `$XDG_CACHE_HOME/minicode/math`, one
  `<batch>.pdf` as tectonic wrote it and a `<batch>.idx` (`key page descent`,
  header `minicode-math-index 1`) per run, 50 MB trimmed to 40. The key is
  SHA-256 of `MathTex::kVersion`, display or inline, and the TeX; only
  typeset formulas are indexed, so a failed one asks again after a reopen.
- **Drawing**: the page is rendered by poppler at 2x onto white, its
  luminance becomes alpha, and the mask is filled with the text color asked
  for (`Editor::mathColor()`, the editor's), so a formula takes the color
  around it (cairo's ARGB32, premultiplied, as a `GdkMemoryTexture`). 1 bp is
  1.4 logical pixels (`kPxPerBp`, 10 pt TeX next to 13 px text). The picture is
  an `MdPicture` anchored in the text; `MdPicture` now reports a baseline
  (`height - depth`), so a formula's depth hangs below the line's baseline.
  A display formula's anchor carries `md_mathcenter` (centered, 4 px above
  and below); `Markdown::fit` scales a wider-than-the-pane formula with its
  depth. `Page::waiting` counts formulas with Mermaid's diagrams (so the
  re-render on an arrival and the side pane's rebuilding of such pieces are
  the same code), `mathWaiting` the formulas, `mathMissing` those left as TeX
  for want of tectonic. A failed formula stays code with `md_tip:` (TeX's
  complaint) as its tooltip.
- **The line**: `Editor::mathNote()`, a label over the preview's top right
  (`updateMathNote`, from `notifyDocument`): "Typesetting N formulas…" while
  any is out, else, with `markdown.math` on and no tectonic, "Math is shown
  as TeX: tectonic is not installed." with a Download tectonic link, which
  runs the LaTeX preview's download (`LatexPreview::downloadTectonicThen`, the
  same function body as its button, the same checksum) and renders again.
  The side pane has no such line (its pieces just rebuild as formulas
  arrive).
- **Checked by** `markdown_math` (`MathSuite.cpp`): a stand-in tectonic and
  PDFs made with cairo; see the file's opening comment. A real tectonic
  would confirm the batch document compiles under XeTeX, the log's
  `MC:` lines in tectonic 0.17's `--print` output, the PDF page sizes against
  the boxes, and the look and size of a real formula next to the text.
- **Not done**: the line in the side pane; copying a formula copies an
  anchor character (as on the Mac); the picture size does not follow the
  preview's font size.

### 59. Terminal tabs (done, November 2026; `PARITY.md` item 4.7, first part)

- **The dock**: `TerminalDock`
  (`src/TerminalDock.{h,cpp}`) is the terminal pane: a `TabStrip` (with its
  opt-in drag reorder) and a + above a `GtkStack` of `Terminal`s, each with
  a shell of its own. `app->terminal` is **always the active one** (the
  dock's `onActiveChanged` sets it, as the Mac's `terminal` getter), so Run
  File, `term-output`, the inline agent, Run in Terminal, the focus and key
  places all kept working untouched; `app->termDock` is for what means
  every terminal.
- **Every terminal**: settings, the font and the terminals' zoom
  (`applyWindowSettings`, `applyTerminalZoom`), the project root on Open
  Folder, the waiting dot (the board's mark is the window's, so all its tabs
  show it), the IDE link's variables (`makeTerminal`, the dock's factory,
  sets them and the current settings and zoom on a new one).
- **Keys** (D1; the Mac's, with Ctrl for Cmd): New Terminal Ctrl+Shift+`
  (VS Code's Linux key, Global), Next and Previous Terminal Ctrl+Alt+PageDown and
  PageUp (the Mac's Option+Cmd+] and [, whose Ctrl+Alt keys are the folds; Global), Close Terminal
  Ctrl+Alt+Shift+W (the Mac's Ctrl+Shift+W is Close Window here, VS Code's),
  Rename Terminal… and Move Terminal Left and Right without keys, as on the
  Mac. Next and Previous wrap. The Terminal menu has them.
- **Titles**: a user title (Rename… is a popover on the tab; an empty name
  gives the program's back) wins over the title a program sets (OSC 0/2,
  VTE's `window-title-changed`), over the shell's name.
- **Closing**: a terminal with a program in the foreground
  (`Terminal::busy`) asks (`TerminalDock::confirmClose`; main.cpp's is a
  `GtkAlertDialog`, the checks replace it with a recorder). The port asked
  nothing before, for its one terminal or for the window closing: closing
  the **window** still does not count busy terminals (the Mac's quit asks
  once for all; not done). Closing the last tab deletes it and leaves a
  fresh `Terminal` that has made no shell yet (they start when first
  mapped), then `onEmpty` hides the dock, so `app->terminal` is never null
  and showing the dock starts a shell.
- **Run File** never types into a busy terminal: it opens a tab and runs
  there (`runInTerminal(..., newTabIfBusy)`, using `runBlocker`). **`minicode
  term`** now always makes a tab (`ControlHost::newTerminal`; `--title` is
  its user title, `reused` is false, the old "busy, left alone" refusal is
  gone because the new tab is idle); a host without the hook keeps the old
  path (the core's tests).
- **Ids**: each terminal has its own id (item 63; this bullet once said
  they shared the window's, with extra tabs under `window id + 100000 * n`).
  Not done: the terminals in the session, hide-and-restore of tabs across
  launches.
- **Checked by** `terminal_tabs` (`TerminalTabsSuite.cpp`, real keys and
  clicks, bash): the cases above, each shell known by the integration's
  marks and pids, a command in a hidden tab, rename/move/drag, the close
  question, Run File, `term` and `term-output`, the last tab.
- **shell_integration part two follows the tabs**: `term --run` is checked in
  the tab it made (that shell's own marks: one command, status 0, the
  finished hook once, `term-output` reads it) and, beside a half-typed line,
  a busy `sleep 3` and an exec'd `sh`, the first terminal is checked to be
  left alone (same line, same started count, still busy); `term` is never
  refused now and Run File into the busy shell runs in a new tab. The suite
  closes its extra tabs (`backToOriginal`). `terminal_tabs` replaces a
  terminal an earlier suite left with a shell of another kind.
- **Trap (found by `G_DEBUG=fatal-criticals`)**: `gtk_stack_remove` held the
  terminal widget's last reference, so `~Terminal` disconnected handlers on
  freed widgets (heap corruption that showed as a crash suites later, e.g.
  `lsp_actions` once). `TerminalDock::remove` now holds a reference across
  the delete. In a long combined run `terminal_tabs` can still fail on a
  loaded machine (see the coordinator's notes); alone and after
  `shell_integration` it passes with no GLib criticals.
  Two more causes found with an ASan build (`-Db_sanitize=address
  -Db_lundef=false`, the terminal suites): `TerminalDock::remove` deleted the
  closing terminal while `app->terminal` still named it, and removing its
  widget moved the focus, whose handler (`updateKeyPlace`, `Terminal::owns`)
  read the freed object; the dock now hands the host the surviving terminal
  first (the last tab's fresh replacement joins the stack before the old one
  goes, the dock hidden before the widget leaves). And the failure in the
  combined run ("minicode did not start within 10 seconds") was
  `latex_problems` leaving `XDG_RUNTIME_DIR` changed, so a later suite's CLI
  looked for the control socket in the wrong folder; it puts it back.

### 60. Terminal splits (done, November 2026; `PARITY.md` item 4.7, second part)

- **Panes**: a tab is a `TerminalGroup` (`src/TerminalGroup.{h,cpp}`), a box
  with a `GtkCustomLayout` that places columns of stacked `Terminal`s from
  the core's `TerminalLayout` (shares, `frames`, `split`, `nudge`,
  `dragBoundary`, `removePane`; its minimums are 20 cell widths and 5 cell
  heights from `Terminal::cellSize`, the gap 4 px). `TerminalDock` holds
  groups: `count/at/activeIndex` are still tabs (`at` is the pane last
  used), `all()` is every pane (now a vector by value), `split(down)`,
  `closePane`, `resizeActive`, `focusPane`. `TerminalLayout.cpp` joined both
  core lists.
- **Keys** (D1): Split Terminal Ctrl+Shift+5 (VS Code's Linux key, Global);
  Split Terminal Down and the four Resize Terminal Pane commands have no
  key: the Mac's Ctrl+Cmd+arrows would be Ctrl+Alt+arrows (GNOME's
  workspaces) or Ctrl+Alt+Shift+arrows (moving the window between them).
  They are in the Terminal menu and the palette. Close Terminal
  (Ctrl+Alt+Shift+W) closes the active pane, the tab with its last; the tab's
  x closes the whole tab (asking for the first busy pane). Next and Previous
  Terminal walk panes then tabs.
- **Behaviour**: a split halves the active pane, evens all when a half
  would be under the minimum, else beeps and adds nothing. The new pane has
  the keyboard (a control-channel split leaves it where it was). The active
  terminal follows the keyboard (`Terminal::onFocusIn`, a focus controller
  on the terminal's root). A drag is one gesture on the group (capture
  phase) that finds the bar near the press: a gesture on a bar measures from
  the bar's moving origin. Bars are reused per boundary, last in the box.
  Before the dock was ever allocated a split plans against 1000x300.
- **`minicode split`**: `--right term` splits (title, cwd and run as `term`),
  `--below term` is a new tab (as the Mac's `beside:NO` is on a dock),
  `--right browse` is `browse`, and `open` is refused with the core's message.
  `ControlHost::splitTerminal`.
- **Not done**: Up and Down on a pane alone in its column do not resize the
  dock (they beep); no ◫ button; per-terminal ids (all panes share the
  window's id; the process monitor lists every pane); find.
- **Checked by** `terminal_splits` (real keys, clicks and drags, bash): the
  shares, frames, distinct shells and ptys' columns, the half/even/refuse
  rule, bar drags, keyboard resize, closing, focus by click, and the four
  `split` forms. `control` expected "not on Linux yet" for `split`; it now
  expects the open refusal.
- **Trap: the key place follows `app->terminal`.** `updateKeyPlace` reads
  `a->terminal->owns(focus)`, and the dock sets `app->terminal` in
  `onActiveChanged`, which can come after the focus move that already ran it,
  so a new or replaced terminal left `place.terminal` false with the keyboard
  in it and every Global terminal key (Ctrl+Shift+5, Ctrl+Shift+`) unbound.
  `onActiveChanged` now runs `updateKeyPlace`; `terminal_splits` checks the
  accelerators are bound before its first key.
- **Trap: a pane that looked dead was a view left above the end (CI,
  2026-11-12).** `terminal_find` failed "cleared: no old line on the screen"
  after a split and its close, only at CI's prompt width (an `MC_GUI_ROOT`
  44 characters long here). The shell was fine: probes after each step
  (a `touch` fed to the pane, the slave's `FIONREAD`, the adjustment) showed
  every line ran. The find had left the view on its last match, which the
  long prompt's wrapped command line put three rows above the end; with
  scroll-on-output off, and the app's text fed to VTE (no key press, so no
  scroll-on-keystroke), Clear, `runLine` and Run in Terminal worked below a
  view that never moved, and the split's rewrap kept it there. They now
  bring the view to the end first (`Terminal::scrollToEnd`, the Mac's
  snap-back for app-typed text); at the end VTE follows the output. The
  suite checks it from a find far up the scrollback, at any prompt width.

### 61. The split editor (done, November 2026; `PARITY.md` item 4.6)

- **Two groups, one view with everything**: `src/Groups.cpp` (Editor's
  methods, as Tabs.cpp is). The view and all that hangs on it (gutter,
  carets, folds, the find bar, the viewers, notes, the inline agent) stays
  in the focused group, whose tabs are still `tabs_` and `active_`, so the
  strip, `tabCount`, `document(i)`, `indexOf`, the tab keys and closing all
  act on the focused group unchanged. The other group is `other_` and
  `otherActive_`, shown by `otherBox_`: its own `TabStrip` over a plain
  read-only, never-focusable `GtkTextView` (`mirrorView_`) on the **same
  buffer** (D2 made that free; the Mac needs `MCEditorMirror`'s second
  layout manager), so typing shows in both as it happens, with one undo
  history, one dirty state and one backup. Focusing the other group
  (`swapGroups`) trades the two: the view takes the other group's tab at
  that group's place, and the other group keeps the view's tab at the
  view's place. Sides are fixed (0 left); `groups_` is a box with a custom
  layout putting `box_` (the focused group) on whichever side `focused_`
  says, with `EditorTabs::leftGroupWidth` (150 px a side, the Mac's) and a
  `.minicode-bar` the drag moves.
- **`documents()` is every document once** (by value now): the focused
  group's in tab order, then the other's. Backups, sessions, auto-save,
  the disk checks and the language servers read it, so a file in both
  groups is opened once on its server. `documentFor`, `documentById`,
  `dirtyDocuments`, `pathMoved` and the settings loops read it too; a tab
  menu's Close Others, Close to the Right and Close All, and File > Close
  All Tabs, act on the focused group (`groupDocuments`).
- **A buffer has one selection**, so the other group's caret and first
  line are marks of its own (`mirrorIns_`, `mirrorBound_`, `mirrorTop_`,
  refs we hold, since a mark outlives a buffer that goes) and its scroll is
  its own view's; the mirror hides the selection the buffer shows for the
  focused view (`.minicode-mirror`). `gtk_text_view_scroll_to_mark` waits
  for the layout, so until three frames after one, `mirrorPlace` reads the
  mark, not the view.
- **Rules**: a file opens in the group you are in (`activate` adds the
  other group's document to this group as a second tab of the same
  document); Split Editor when split puts the tab in the other group; Ctrl+2
  with no split splits (VS Code's, the Mac's); Move Editor alone in the only
  group rings the bell (it would leave an empty group); closing a group's
  last tab closes the group (`closeFocusedGroup`, `dropOther`); closing a
  tab whose document the other group still has asks nothing
  (`inBothGroups`, main.cpp's `closeTabAsking`) and is not remembered for
  Reopen Closed Tab. A press anywhere in the other group is claimed in the
  capture phase (the view would move the shared caret), and once over the
  group is focused with the caret where it landed; its strip's press
  selects that tab there; its close button closes it in that group; it has
  no right click menu.
- **Viewers stay in the focused group**: a picture, PDF, video, the CSV
  table, SQLite, a diff, a page, the LaTeX preview and the rendered
  Markdown page show only in the view's slot (a second `MediaView`, LaTeX
  preview or page buffer would cost as much again); the other group names
  the file, "Click to show it here". A text tab or a message shows as
  text. The other group draws no gutter, squiggles, bracket colors past
  what the highlighter tags, or inlay labels (the Mac's mirror leaves the
  same out).
- **Keys** (D1): Split Editor Ctrl+Backslash (VS Code's; an editor command, so the
  terminal's program keeps it as SIGQUIT), Focus First and Second Editor
  Group Ctrl+1 and Ctrl+2 (VS Code's, Global; Ctrl+1 was Focus Editor and
  takes the registry's title now), Move Editor into Next and Previous Group
  Ctrl+Alt+Shift+Page Down and Page Up: VS Code's Ctrl+Alt+Right and Left
  are GNOME's, so the terminal tabs' Ctrl+Alt+Page Down and Up with Shift.
- **Sessions**: `TabSessions::Session` gains `split` and `groups`
  (`EditorTabs::GroupsSession`, written as "groups" only when split; an
  older or damaged record is one group, `tab-sessions:groups`).
  `Persistence::restoreSession` lays the split over the tabs that came back
  (`EditorTabs::restoreGroups`, `Editor::restoreGroups`), reading each
  group's tab on show.
- **Checked**: `split_editor` (real keys, clicks and a drag): Ctrl+Backslash shows
  one document in two views on one buffer (three documents for three
  files), typing in one shows in the other and marks both tabs, Ctrl+Z in
  the other group undoes it, each group's caret and scroll come back as
  its own across Ctrl+1 and Ctrl+2, a click in the other group focuses it
  with the caret on the line clicked, Move into Next and Previous Group
  move the tab and the keyboard, Ctrl+Page Up and Alt+2 step through the
  focused group only, the bar drags 100 px and stops at 150 px a side, a
  new window on the folder comes back split as it was, and closing the
  right group's only tab closes the group. Not checked: the look (the
  selection hidden in the other group, the note for a viewer).
Trap: split_editor needs the editor's full width; a browser pane an earlier suite left shown halves it and the groups' 150 px minimum stops the bar's 100 px drag, so the suite hides the browser and terminal at its start.

### 62. Find in the terminal, Clear, dock resize (done, November 2026; `PARITY.md` item 4.7, third part)

- **Find** is `actions.find` routed by the keyboard: with a terminal (or its
  bar) holding it, Find… opens that terminal's bar (`Terminal::showFind`, an
  overlay at the pane's top right: an entry and a label), else the editor's
  or the browser's. **No new key, by D1**: Ctrl+F is readline's
  forward-char and a plain Ctrl key is the program's in a terminal, and
  Ctrl+Shift+F is Find in Folder, so it is the Edit menu or the palette
  (a new registry row would have changed the Mac). Inside the bar Enter and
  F3 step on, Shift+Enter and Shift+F3 back, Escape closes and gives the
  terminal the keyboard (a capture-phase key controller on the entry, so it
  does not depend on the key place). It searches as you type with
  `vte_terminal_search_set_regex` over `vte_regex_new_for_search` (the query
  escaped by hand, PCRE2 CASELESS|MULTILINE), wrapping; case-insensitive
  only, no toggles. **No count**: VTE cannot give one, and scanning text was
  ruled out; a miss says "No results" and outlines the entry. The dock's
  `onActiveChanged` (main.cpp) closes every other terminal's bar.
- **Clear Terminal** (`workbench.action.terminal.clear`, keyless; Ctrl+K is
  kill-line): `Terminal::clearScreen` sends Ctrl+L when the integration says
  the shell is at a prompt and nothing else has the terminal; refused (bell)
  otherwise. The screen is cleared and the scrollback is kept (the Mac drops
  both).
- **Dock resize**: Up and Down on a pane alone in its column
  (`TerminalGroup::aloneInColumn`) call `WindowLayout::nudgeTerminalBar`, a
  drag of the terminal's bar by four rows (Up grows), so the right-area
  limits apply.
- **Checked by** `terminal_find`: bar opens on a focused terminal only,
  match in the scrollback selected, miss, case, stepping, Escape and the
  keyboard, the bar closing on a split, Clear, refusal while `sleep` runs,
  and the dock height by exactly four rows.

### 63. A terminal's own id (done, November 2026; `PARITY.md` item 4.7, last part)

- **The id** is the terminal's, made with it: `Terminal`'s constructor takes
  the next number from `kFirstId` (1001, so a window's small control id is
  never taken for one) and enters it in a registry; `Terminal::byId(id)`
  finds a live terminal, and the destructor leaves the registry and
  `TerminalAttention::forget`s the id, so a closed tab or pane takes its mark
  with it and nothing else's (the old `setControlId(0)` dance in
  `TerminalDock` is gone, as is `setControlId`). The shell gets it as
  `MINICODE_TERMINAL_ID` beside `MINICODE_PID` (unchanged: `spawnShell`).
  The last tab closing leaves a fresh terminal, with a new id.
- **The window keeps its own id** for what is the window's (`window`, `open`,
  `browse`, `--window`). `main.cpp` maps a terminal to its window with
  `appOfTerminal` (a walk of the docks).
- **Control channel**: `state` lists every terminal of every window
  (`ControlWindow::terminals`, filled in `controlWindowOf`) with `id`,
  `title`, `active`, `attention`, `busy`, `command`, `status`; `notify
  --terminal`, `term-output ID` and `open-url`'s terminal (BROWSER) resolve
  through the terminals; `term` answers with the id of the terminal it used or
  made (`ControlHost::newTerminal` and `splitTerminal` return it). **A
  window's id is no terminal's**: `notify` and `term-output` with one say
  "there is no terminal N (terminal ids are in `minicode state`)", where they
  used to act on the window's active terminal. A terminal's `open-url` for
  an unknown id is the same error.
- **The board and the strip**: the changed handler sets each terminal's dot
  from its own id and asks the dock to redraw its tabs; a tab holding a
  waiting pane has " ●" after its title, a done one " ✓"
  (`TerminalDock::refresh`). Next Waiting Terminal walks every terminal and
  shows (and focuses) the tab and pane it lands on (`showTerminalOf`).
  The Agents panel's Show Terminal and the Processes panel's reveal do the
  same by id.
- **The process monitor** lists every terminal's shell under its id and the
  window's id as `window`, the session the board has for that terminal
  (`sessionRootsOf(app, terminalId)` for the pane's link roots).
- **Checked by** `terminal_ids` (in `AttentionSuite.cpp`): two tabs and a
  split with each shell writing its `$MINICODE_TERMINAL_ID` to a file, `state`,
  a notify marking the second tab only and a closed tab clearing it,
  term-output by id and the window's id refused, the monitor's rows.
- Left out: the Mac's `cwd`, `message` and `session` fields in `state`.

### 64. Web bookmarks: opening them, and the browser window (done, November 2026; `PARITY.md` item 4.8, first part)

- **Core in the build.** `src/WebBookmark.cpp` is in `meson.build` and the
  `Makefile` (`linux:build-lists` holds them together). `BookmarkOpen.{h,cpp}`
  is the files side, with no GTK: the bookmark folder from
  `browser.bookmarks` (`~` expanded, a relative path from the home folder,
  `~/Bookmarks` when unset), `ensureFolder`, `sameFolder` (by realpath), a
  file's address (`WebBookmark::addressIn`, so only what `isWebAddress`
  accepts; a binary `.webloc` gives none) and `addressesIn` (the bookmarks
  directly in a folder, in the tree's name order, dotfiles left out).
- **Opening.** `openFileThen` (every open in `main.cpp`: the tree, the
  command line, the palette, links) hands a bookmark to
  `openBookmarkFile` first. A bookmark with an address that may open goes to
  `showWebAddresses`: `Browser::openAddresses` (new: a tab already at the
  address, compared without a trailing slash, is shown again; the first new
  address takes the tab on show when that is empty or still the home page;
  the others are tabs right of it), then the pane is shown. The pages are
  loaded before the pane is shown, so a pane never shown before opens them
  and not its home page. A preview open (`EditorTabs::opensInPreview`, a
  single click) keeps the keyboard where it was, a kept one (double click,
  Return) gives it to the page (`Browser::focusPage`); the keyboard also
  goes to the page when what had it is hidden now. Any other address, an
  unreadable file or a binary `.webloc` falls through to the text path.
- **The tree's right click menu** has a section "Open as Text" and "Open All
  in Browser Tabs" (`win.openbookmarkastext`, `win.openbookmarkstabs`). The
  model is static, so `FileTree::onContextMenu` (called after the click
  selected its row) enables them: the first for a selected bookmark file,
  the second for a folder with a bookmark file directly in it. Open as Text
  sets `gOpenBookmarkAsText` for one open and gives the editor the keyboard;
  Open All opens `addressesIn(folder)` as tabs and gives the page the
  keyboard.
- **The browser window.** `handleCommandLine` (split out of `onCommandLine`
  so the checks can call it) takes each argument for which
  `WebBookmark::isWebAddress` is true as an address and the first other one
  as the path, handled as before; with only addresses no window on the
  current folder is made. The addresses go to the window rooted at the
  bookmark folder (`sameFolder`), made, with the folder, when missing, else
  reused, so later addresses join it. While no file is open there the editor
  is hidden and the page has the right side. A folder that cannot be made is
  said on the terminal's side of the command line. Detach and
  `LaunchOptions` pass an address through as any word that does not start
  with `-`, in the first process and in a second one handing over through
  GApplication, so `Detach` needed no change; the help says what an address
  does.
- **The desktop file** offers `application/x-mswinurl`, as the Mac's
  Info.plist offers `.url` to Finder: no default is taken.
- **Checked by** `bookmarks` (`tests/gui/BookmarksSuite.cpp`, a server in the
  test process): a tree click loads a tab with the keyboard left in the
  tree, the same address from another file shows that tab and a double
  click gives the page the keyboard; `file:`, `javascript:` and a binary
  `.webloc` open as text; the menu items' enabled state by row, Open as
  Text and Open All (three tabs in order, the bad one skipped); the command
  line through `MCGuiCommandLine` (two addresses, two tabs, the folder made,
  a later address in the same window, a path still a path).
- Left out: a real second process handing over through GApplication (the
  `detach` suite is the place; the argument handling is the same function);
  `.webloc` binary property lists (opened as text); the Bookmarks menu, the
  toolbar star and saving (the other half of 4.8).
- Needs a person: the right click menu on a real desktop, a real
  `minicode https://...` from a terminal, a double-clicked `.url` in the
  file manager once the user picks minicode for the type.

### 65. Web bookmarks: saving pages (done, November 2026; `PARITY.md` item 4.8, second part)

- **The core** `WebBookmark.cpp` is in both build lists. `src/BookmarkSave.{h,cpp}`
  (no GTK) is the file half of the Mac's `Bookmarks.mm`: `folderFor`
  (`browser.bookmarks`, `~` expanded, a relative path from home, else
  `~/Bookmarks`), `ensureFolder`, `write` (the core's `fileName` and
  `uniqueName`, then `O_CREAT|O_EXCL` so a name taken meanwhile gets the
  next number and a file is never replaced), `fileFor` (the bookmark for an
  address anywhere under the folder by `sameAddress`, hidden entries
  skipped, 10,000 entries and symlink loops cut), `writeTabs`.
- **Save Page as Bookmark** is `minicode.browser.saveBookmark`, **Ctrl+D as a
  Page command** (`PARITY.md` D14), with its row in `CommandsLinux.cpp` and
  `Keys.cpp` (`win.savebookmark`, `act_save_bookmark` in `main.cpp`). The
  editor keeps Ctrl+D for Add Next Occurrence: `Keys.cpp`'s `bound` and
  `rank` give a Page command its keys only while `place.page` holds (the
  browser's page or toolbar, or a PDF), as for Ctrl+T. File > Save Page as
  Bookmark is the same action from anywhere. A page already bookmarked is not
  saved again; the status bar says "Already bookmarked as ~/Bookmarks/x.url".
  The file goes in the bookmark folder (made when missing), or, in the
  window rooted at it, in the tree's selected folder (a selected file's
  folder, else the root).
- **Save All Tabs as Bookmarks** (`win.savebrowsertabs`, no key) makes
  `Tabs YYYY-MM-DD HH.MM` (a number when taken) and writes a bookmark for each
  tab with a web address, once each (`tabAddresses`' rule), named by the tab's
  title; the status bar says how many.
- **Checked by** `bookmark_save` (`BookmarkSaveSuite.cpp`): a server in the
  process, a scratch `browser.bookmarks`; real Ctrl+D with the page focused,
  a second one that writes nothing and says where, a taken title numbered, Ctrl+D
  in the editor still Add Next Occurrence (carets) and saving nothing, the File
  menu's action with the editor focused, Save All Tabs. Core: `commands:linux-*`
  and the README's Linux column.
- Left out: the toolbar's star (filled while bookmarked; a click moves the
  bookmark to the Trash), the book and the Bookmarks menu, the palette's
  bookmarks (the next bite); the actions are not disabled with no page (they
  do nothing).
- **Needs a person**: Ctrl+D in a real WebKit page on a desktop where the
  page might take it (no page we know binds it); where the status bar's note
  reads on a narrow window.

### 66. Web bookmarks: the star, the book, the Bookmarks menu and the palette (done, November 2026; `PARITY.md` item 4.8, last part)

- **One module.** `BookmarkOpen` and `BookmarkSave` each had a folder
  resolver and an `ensureFolder`; they are `src/Bookmarks.{h,cpp}` now
  (namespace `Bookmarks`, both build lists): `folder` (the setting, `~`
  expanded, canonicalized), `ensureFolder`, `sameFolder`, `addressOf`,
  `addressesIn`, `hasBookmarkIn`, `write`, `fileFor`, `shortPath`,
  `writeTabs`, and for this bite `list` (a folder's entries, folders first,
  the tree's name order), `all` (every bookmark under a folder with its
  address and "Folder/Name", for the palette; 3,000 entries at most, loops
  cut) and `menu` (a `GMenu` of the folder: a submenu a folder opening with
  "Open All in Tabs", then its rows; 2,000 entries and six levels at most;
  an underscore in a title is doubled, since GTK reads one as a mnemonic).
  Titles are `WebBookmark::menuTitle`.
- **The star** (`Browser`, a button after the address bar): `isBookmarked`,
  `onStar` and `bookMenu` are set by the window, so `Browser` knows nothing
  of files. `refreshStar` runs in `syncUrlBar` (every navigation and tab
  switch) and from the window when the folder changed; the icon is
  `starred-symbolic` while the address on show is bookmarked anywhere in the
  folder (`fileFor`, a subfolder counts) and `non-starred-symbolic` else,
  and the tooltip says what a click does; it is insensitive for a page that
  may not be bookmarked. A click on the empty star runs `win.savebookmark`
  (so the same once-only rule and the same folder choice); on the filled one
  it moves the file to the Trash through `GitTrash::trash` (GIO's
  `g_file_trash`, and the checks' scratch Trash) and the status bar says
  "Moved “x.url” to the Trash".
- **The book** is a `GtkMenuButton` beside it. Its model is made by
  `set_create_popup_func`, which GTK calls each time before the popover
  opens, so the folder is read then and never kept. An item runs
  `win.openbookmark` (the path as a string target): it loads in the tab on
  show, **with Ctrl held in a new tab**. A menu item's activation does not
  carry the event, so the action asks the keyboard device
  (`gdk_device_get_modifier_state`) for the modifier; the check holds Ctrl
  through XTest. A folder's "Open All in Tabs" is `win.openbookmarkfolder`.
  The page gets the keyboard.
- **The Bookmarks menu** in the bar (after Navigate; "Bookmark This Page",
  then the same listing). A `GMenu` has no "about to open" signal, so it
  is refilled from the folder: when the pointer enters or presses the menu
  bar (the press in the capture phase, before the menu opens;
  `MenuBarFocus::bar` finds the widget), when a window becomes active,
  when the settings change, after a save or the star's trash, and when
  watches on the folder and the folders in it (three levels, a hundred
  at most; `GFileMonitor`, made again when that set changes) report a
  change, coalesced over 100 ms. The same refresh redraws every window's
  star, so a bookmark made or deleted by hand outside minicode shows.
- **The palette**: `PalettePanel::Context::bookmarks` is read when the
  palette opens, and each bookmark is a command row "Bookmarks > Name" (or
  "Bookmarks > Folder/Name") that runs `win.openbookmark` on its path, so
  it ranks, matches and is remembered like a command.
- **Checked by** `bookmark_menu` (`BookmarkMenuSuite.cpp`, a server in the
  test process, scratch folder and Trash): the star filled for a bookmarked
  page and one in a subfolder, empty for another, following a tab switch
  and a bookmark made and deleted outside; a click saving, a click on the
  filled star trashing with the note; the book's contents (a folder first,
  titles, a file made just before) and real clicks loading in the tab on
  show and, with Ctrl, in a new one; Open All in Tabs; the Bookmarks menu's
  contents and its following the folder; the palette row. Core:
  `commands:*`, `linux:build-lists`.
- Left out: a bookmark folder created while no folder existed is not
  watched until the next refresh (a save, a window coming to the front);
  folders deeper than three levels are not watched (the bar's press still
  reads them); no "Show Bookmarks Folder".
- **Needs a person**: the look of the star and the book in the toolbar,
  Ctrl+click on a real desktop, the Bookmarks menu with many entries.

### 67. The memory benchmark (done, November 2026)

`scripts/membench-linux.sh` (`make membench-linux`) runs the Mac's workload
(the repo copied as the project, tests/run_tests.cpp open with clangd, a bash
terminal, demo/paper/notes.tex typeset by tectonic with the PDF on screen,
one web page) on a release build of the GTK port (`linux/build-bench`,
`MINICODE_BENCH`, `linux/src/Bench.cpp`) and on the VS Code snap with the
clangd extension, Chrome and Papers. Memory is PSS from
`/proc/<pid>/smaps_rollup` (`tools/procmem-linux.c`), since shared libraries
(WebKitGTK, Chrome's) would be counted per process by RSS. A side is its root
pid, its descendants and any process in the same `snap.*` scope. Each side
runs under its own `dbus-run-session` in a private Xvfb with scratch profiles
under `/tmp/membench-linux`; nothing appears on the desktop.

Result, 2026-11-14, Ubuntu 26.04, i7-8550U, 8 cores, 23 GB, minicode 1.5.24,
VS Code 1.145.2, Chrome 157, Papers 50.2, clangd 21.1.8; settled 60 s, median
of 5 samples:

- minicode 957 MB: clangd 504, minicode 271, WebKit (web page and PDF) 177,
  dbus 2, shell 1, bwrap 1.
- VS Code + Chrome + Papers 2,081 MB: VS Code 908 (main, renderer 413,
  four node utility processes including the extension host, GPU, network),
  clangd 503, Chrome 468 (browser, GPU, network, renderers), Papers 196,
  cat 4, shell and dbus 2.

Caveat: Xvfb has no GPU, so Chrome, Electron and WebKit draw in software;
their numbers differ on a real desktop. The "bus can't be made" and "Failed to
open display" lines in the logs of an earlier attempt were teardown noise:
each app was stopped while others still ran. The script started all three
apps under one bus without trouble, and the process lists
(`build/membench-linux-*.procs`) show every expected process present.

### 68. The tabs' font size (done, November 2026)

The owner found the tab titles much too small: `.minicode-tab` fixed
`font-size: 12px`, about 9 pt, against GNOME's 11 pt interface font.

- **`tabs.font-size`** is a core setting (`Settings::tabsFontSizeSet`,
  `tabsFontSize`, points, 6 to 72, as `editor.font-size`), in the Tabs
  surface's keys, so Adjust Settings on a tab opens the file at it. The
  Mac's own side is its session's.
- **Unset, no size is written**: `ThemeCss.cpp` leaves `.minicode-tab`
  without a `font-size`, so the labels take GTK's own font
  (`gtk-font-name`, the desktop's interface font). Set, it writes
  `font-size: Npt`. The editor, terminal and browser strips share the
  class, so all follow. The padding is in pixels and the height follows the
  label, so a large size is not clipped.
- **Checked** in the `themes` suite: the title's size read from its Pango
  context equals `gtk-font-name`'s with the key unset (the old pixel rule
  fails this under any desktop font but 9 pt), is 16 pt after
  `tabs.font-size = 16` with no restart, and goes back when the line goes.

### 69. Recent folders, and the dock's menu (done, November 2026)

The owner asked to right click minicode's icon in the dock and see the
folders opened lately, with a way to choose another through the system's
folder chooser; the Mac half is File > Open Recent (`src/RecentFolders.mm`).

- **The list** (`RecentList.{h,cpp}`, linux/src, PARITY.md D11): a JSON array
  in `$XDG_STATE_HOME/minicode/recent-folders.json`, written atomically
  (0600). Every window's folder is noted (`newWindow`, and `openFolder` for
  File > Open Folder): newest first, 20 at most, one entry a folder by its
  realpath, folders that are gone left out when listed but kept in the file.
  Off under `MINICODE_BENCH` or `MINICODE_DEMO` (the port has no demo; the
  switch is there for the day). The port keeps it apart from the core since
  the Mac's lives in its defaults; the rules are the Mac's.
- **File > Open Recent**: the registry's `workbench.action.openRecent`
  ("Open Recent…", `win.openrecent`) lists the folders as a pick list in the
  palette's box (`PalettePanel::showPick`), and a submenu "Open Recent" right
  after it lists them with Clear Recently Opened at the end (`app.openrecentfolder`
  with the path as its target, `app.clearrecent`). Choosing one is
  `openRecentFolder` (item 71): the window in front moves to the folder, as the
  Mac's does.
- **The key is Ctrl+R**, VS Code's Linux key and the Mac's. It is a plain
  Ctrl key and not Global, so `linuxTerminalTakes` leaves it to the terminal:
  with the terminal's keyboard, Ctrl+R is still the shell's history search.
- **The dock's menu** (`DockMenu.{h,cpp}`): Ubuntu Dock and GNOME's dash show a
  desktop file's `[Desktop Action]` groups in the icon's menu, running or
  not (the dock's live quicklist over D-Bus works only while the app runs).
  minicode writes its own copy of the installed `org.minicode.Editor.desktop`
  (found in `$XDG_DATA_DIRS`, never `$XDG_DATA_HOME`) to
  `$XDG_DATA_HOME/applications/`, which shadows the installed one under the
  same id, so a pinned icon stays pinned. It is the installed
  `[Desktop Entry]` verbatim plus `Actions=open-folder;new-window;recent-N;`
  and `X-Minicode-Generated=true`, then the actions: Open Folder…
  (`Exec=<binary> --open-folder`), New Window (`Exec=<binary> --new-window`,
  item 71; first it ran the binary with no path, which only raised the window
  on the launcher's folder), and one action for
  each of the first 10 existing folders, named with `~` for home, with the
  folder quoted as the Desktop Entry spec says (quoted, `"` `` ` `` `$` `\`
  escaped, `%` doubled, then each `\` doubled for the file's own escapes). A
  folder whose name holds a line break is left out. Written on startup and
  whenever the list changes, only when the text differs. **A file there
  without the marker is the user's own and is never written or removed**
  (logged once). With no installed file (a run from a checkout) nothing is
  written. GNOME watches the folder, so `update-desktop-database` is not
  needed (it indexes only `MimeType`, which the copy repeats unchanged).
- **`window.dock-menu`** (default true, in the `window.*` keys): false removes
  the copy, if it carries the marker.
- **`minicode --open-folder`** (`LaunchOptions`, left in the arguments so a
  running minicode gets it over GApplication): the system's folder chooser
  (`chooseFolder`, the one File > Open Folder uses, `GtkFileDialog`
  `select_folder`, which goes through the desktop portal), over the front
  window if there is one, starting in the home folder; the folder chosen
  gets a window or is brought forward. The application is held while the
  chooser is up, so a cancel with no window open quits.
- **Checked** in the `recent_folders` suite in a scratch `XDG_STATE_HOME`,
  `XDG_DATA_HOME` and `XDG_DATA_DIRS` (a fake installed file): the list's
  order, dedupe through a symlink, cap and missing folders, `MINICODE_BENCH`;
  the generated file's exact text with a folder with a space in it, the
  spec's quoting, following the list, a file without the marker left alone,
  the setting, no installed file; the submenu and the registry item in the
  menu bar, an item opening and then raising a window (before item 71),
  Ctrl+R's pick list,
  Clear; `--open-folder` through `handleCommandLine` with the chooser
  answered by the check (`MCGuiAnswerFolderChooser`) and a cancel.
- **Needs a person**: the real dock on GNOME with the icon pinned (the
  menu shows the actions, an item opens its folder, a pinned icon stays
  pinned after the copy is written, and the first right click after a
  new folder), and the portal's own folder chooser from the dock's Open
  Folder… (with the app not running, and running).

### 70. Double-clicks counted by event time, and idles that wait for layout (done, November 2026)

- **The bug class**: GtkGestureClick counts presses with a timer on the main
  loop. When the first click's handler is slow (it opens a file, shows a
  pane) the second press is handled after GTK reset its count and arrives as
  n = 1, so the double-click's action never happens (the tree, 7abeb99; CI
  lost bookmarks' double-click so).
- **`src/ClickCount.{h,cpp}`**: one counter per widget; `press(widget, time,
  target, gtkN)` answers 2 when the press before was on the same target and
  the event times (`gtk_event_controller_get_current_event_time`) are within
  `gtk-double-click-time`, with GTK's count as a floor. Used by the file tree
  (replacing its own copy), `SearchPanel::onRowReleased` (the first click
  previews a result, the second keeps it) and `TabStrip::onPressed` (a double
  click keeps the preview tab). Left alone: `Carets` (already counts by event
  time and place), the Markdown page's double-click in `Editor::onPressed`
  and the LaTeX PDF's (`PdfView`, `Latex.cpp`): their first click does
  nothing slow; `GitDiffView` and `FoldUi` only act on n == 1.
- **Idles that read layout**: `Editor::documentReloaded` and
  `rerenderPreview` restored the scroll in a low-priority idle, which can run
  before GtkTextView has laid out the new text, so `set_value` clamped to the
  old range. `holdScrollOffset` (Editor.cpp) sets the value now and again on
  each adjustment `changed` until the range reaches it, at most 3 s.
  `TabStrip::keepActiveInSight` read the tab's bounds and the scroller's
  range in an idle; it is a tick callback now that acts once the tab has a
  width and the range holds the row, or after 3 s.
- **Checks**: `preview_tabs` has a slow first click (`stall()`: the first
  click's action holds the loop 700 ms, the second press waits in the queue)
  on a Find in Folder result and on a tab's title; both fail without
  ClickCount (checked by making `press` return GTK's n). The idles have no
  deterministic check: a runner slow enough to run the idle first cannot be
  made on demand, so they are unchecked beyond the existing suites.

### 71. Open Recent in this window, and `minicode --new-window` (done, November 2026)

The owner's choice, 2026-11-14: Open Recent follows the Mac, and the dock's
New Window makes a window.

- **Open Recent** (`openRecentFolder`, main.cpp): a window already on the
  folder (resolved) comes forward; else the window in front moves to it
  through `confirmAllUnsaved` and `openFolder`, the path File > Open Folder
  takes, so a Cancel leaves the window alone; with no window one is opened
  (`showFolderWindow`); a folder that is gone rings
  `gtk_widget_error_bell` and changes nothing. The palette's pick (Ctrl+R)
  uses the window it was opened in, the app action `openrecentfolder` (the
  File > Open Recent submenu) the active window. `handleCommandLine` is
  unchanged for paths: the dock's recent entries run `minicode <folder>`,
  which still means "the window on that folder, or a new one", as the Mac's
  Dock picks do.
- **`minicode --new-window`** (`LaunchOptions`, left in the arguments, any
  order): always a new window, never raising one that exists: on the path's
  folder (a named file opens in it), else the active window's folder as File
  > New Window, else the home folder when none is open. The generated
  desktop file's New Window action is `Exec=<binary> --new-window`, and an
  installed file's own `new-window` action is replaced by ours, so the option
  is there in every case (the others of its actions are kept as before).
- **Checked** in `recent_folders`: Open Recent, by the action and by the
  palette's pick, re-roots the one window (no new window, title and tree on
  the new folder); a dirty tab asks, and Cancel leaves the window and the
  tab; a folder another window has is raised; a missing folder changes
  nothing; `--new-window` adds a window on a folder a window already has, on
  a path, and with the active window's folder; the generated New Window line
  and an installed one's both end in `--new-window`; `launch:` groups in
  `make test` cover the option and the help text. Each fails without its
  change.

## How to work on it

- **Trap: a key or click right after a popover menu closes can be lost**
  (2026-11-11, `git_actions`, only in full runs, a few in ten). The menu is
  unmapped and Escape was delivered, yet the next Ctrl+Shift+P or click is
  still gone (the palette never opened; a diff row's click showed nothing),
  and every later step then typed into the void, so the reports named the
  branch picker, far from the cause. It was not the View menu's length (the
  checks run the palette, never the menu). `RunFromPalette` presses the key
  again every 2 s until the palette is open, and a click that must show
  something is repeated after 3 s inside its wait; use the same where a step
  follows a menu's close. Look for the first FAIL in the log, not the one a
  report names.
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

Two ways to run the app by hand, both described in `../docs/notes/verification.md`
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

- GtkGestureClick counts a double-click with a timer on the main loop, not
  by the events' times: a slow first-click handler makes the second press
  arrive as n = 1. Count with `ClickCount` (item 70) wherever the first
  click does real work. Likewise a low-priority idle can run before GTK's
  idle layout: wait for the adjustment's `changed` or a tick, bounded.
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
- A double-click sent as two clicks in one go (2026-11-10, CI's GTK job,
  `editor_words`): the app handled both within one millisecond of the X
  server's time, the first giving up PRIMARY (the selection emptied) and
  the second taking it back with a word; X's late SelectionClear for the
  first had the same time, which GDK ignores only when older, and
  GtkTextBuffer then collapsed the word to a caret at its end. A hand's
  clicks are tens of milliseconds apart. `MCGuiClicks` (and
  `MCGuiDoubleClick`, `MCGuiDoubleClickDrag`) now has the server hold each
  later click 60 ms (XTest's delay, on a connection of the harness's own),
  so the app's main loop runs between them and a stall cannot stretch the
  gap past the double-click time, and raises the clicked window again
  first: there is no window manager to keep Find in Folder's window, a
  transient, over the editor's, which a result's first click presents.
  Never send the clicks of a double-click by hand in one step.
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
  `Keys::label`), and its row in `docs/REFERENCE.md` (`commands:linux-readme` fails
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
- **CPU time grows with the load on the ThinkPad** (2026-11-10). Its
  i7-8550U (four cores, eight threads) runs a thread three to four times
  slower in CPU time with every core busy: Replace All of 20,000 matches
  took 170 ms quiet and 0.5 to 1.6 s loaded, its undo 1.6 s and up to 4.6
  s. `MALLOC_PERTURB_` (meson and CI set it) made no difference measured.
  A bound with room for that and still below the regression it guards is
  the larger of a fixed bound and a multiple of a baseline measured on the
  same thread in the same step or suite (`find_bar`'s plain-buffer edits,
  `large_file`'s read into a plain buffer), as the Mac's `large_file` does.
- **Quiet is not arrival** (2026-11-10). `MCGuiWaitQuiet` on a scroll value
  or a row count passed under load before anything had moved: GTK's first
  scroll, a worker's first rows, or the rest of the wheel's clicks came
  after 0.3 to 0.5 s of stillness, and the checks after read the old
  state (`tabs`, `gutter`, `tree_stall`, `harness`, whose double-click then
  landed above a text scrolled on by the late clicks). Wait for the value
  the check reads; keep `MCGuiWaitQuiet` for "and nothing more arrives",
  after that.
- **A condition an earlier state already meets** (2026-11-10).
  `find_bar` waited for "200 matches, highlighted" with
  `highlighted() > 0`, which the seed's one tagged match already made
  true; the new matches are painted from an idle, which a loaded run
  reached after the check. Wait for something only the new state has (the
  selected match's own tag).
- **A scroll to a mark in a big buffer waits for GTK's whole layout**
  (2026-11-10). `gtk_text_view_scroll_to_mark` acts at once only over a
  complete layout; while GTK is still laying a new buffer out in idle
  slices (seconds for 100,000 lines, longer under load) it is only kept
  pending, so asking again every poll changes nothing, and a wait of 10 or
  20 s for the line to come on screen timed out in loaded runs
  (`bracket_pairs`, `gutter_typing`). Wait for `MCGuiTextLaidOut` first,
  with a limit about the machine. A scroll that GTK does carry out over
  lines it has not laid out (Ctrl+End in a file just opened) lands short
  and stays, since GTK keeps the top line still as heights come; the
  `gutter` suite waits for every line to have its height before its second
  Ctrl+End, where a quiet view for 0.3 s had let it go too early.
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
- **Part two: pages.** `EditorPage.h` is a read-only page in a tab: a
  diff-kind `Document` with `setEditorPage` (so sessions, backups, saving and
  the servers leave it alone), its widget added to the editor's slot stack on
  first show and removed in `Editor::destroy`; `Editor::showPage` finds the
  tab by `key()` (reuse) and `currentPage()` is for the checks.
- **Evidence** (`EvidencePage.{h,cpp}`, the Mac's `MCEvidencePage`): the core's
  `AgentEvidence::Reader` on a worker thread (one at a time, joined in the
  destructor), published twice (the list, then the outputs' ends, read lazily
  with `readOutput`; Show all up to 1 MB), a 2 s poll for a growing
  transcript. The final message and the last test run sit side by side above a
  GtkTextView of the commands; its links are text tags named `ev:<verb>:<n>`
  found by a click gesture. Open in Terminal types `cd <dir> && <command>` when
  the command's folder is not the window's (the Linux window has one terminal,
  not a new one per folder) and never presses Return. A click on a session now
  opens its Evidence and also opens or closes its files; the row menu has
  Show Evidence.
- **Worktrees** (`AgentWorktrees.{h,cpp}`): `git worktree list --porcelain`,
  `rev-list --count main..head` and `status --porcelain` on a thread, named
  from the AgentModel's sessions by worktree path (else the folder name);
  made by `ensureAgents` in main.cpp (with the Agents panel, or at startup when
  `.claude/worktrees` exists), refreshed 2 s after the model changes. The tree's
  node is a synthetic row named "Worktrees" in the top folder's listing whose
  `standard::file` is the real `.claude/worktrees` folder, sorted first (an
  empty key), present only when that folder holds a non-hidden folder, hidden
  files or not; `.claude`'s own listing leaves `worktrees` out
  (`FileTree.cpp` `ListJob`). Worktree rows are named and badged in
  `FileTree::decorate`, which returns before the git colors. A right click on
  one pops its own two-item menu (`win.compareworktree`,
  `win.worktreewindow`). Compare with Main was a list page here
  (`WorktreeChangesPage`, gone: item 88) and is the review page in its
  worktree mode, the changed files against the merge base
  (`AgentView::compareBase`).
- **Left**: Show Terminal for a session waiting in a terminal (the attention
  board knows the session; the Agents panel does not read it yet); the node
  does not appear until the top folder is listed again after
  `.claude/worktrees` is first made. The `agents` suite checks all of the
  above against a real git repository (fails without the feature, checked).

**Part two (November 2026, `linux-31-term`)**: `term`, `term-output`, Run
File's busy check and the finishing mark.

- **`Terminal::runLine`** (and `runBlocker`) types one line as a paste
  (bracketed when the shell asked) and Return, so it is visible and in the
  shell's history (never `exec`'d). It types only when the integration says
  the shell is at a prompt and `tcgetpgrp` agrees; the refusals name why: a
  running command ("busy running: <text>"), a shell with no nonce (fish,
  sh, anything but bash and zsh: "no integration"), no mark after 15 s, a
  shell not at a prompt. Waited for, not refused: the shell starting (15
  s), a line typed whose start mark has not come (2 s: a second `--run` in
  quick succession), and a foreground other than the shell's while the
  marks say prompt (0.5 s, "longer than a moment", the Mac's `tcgetpgrp`
  rule). One wait at a time (a second asker is refused). A half-typed line
  at the prompt is not set aside as the Mac's push-input does: it runs with
  ours (bash has no such widget bound here).
- **`exec` into another shell or ssh** needs no extra code: the last mark
  said busy with that command and no further mark comes, so it stays busy
  and nothing is typed. `exec zsh` from bash is not re-integrated: bash has
  no hook that runs in the shell before a line other than PS0's
  arithmetic, and a function named `exec` would scope the redirections of
  `exec >log` to its own call, which breaks them. The terminal is busy
  with `exec zsh` until the user restarts it. Run `zsh` itself in the
  terminal and its marks (nonce-less, so unknown) are ignored.
- **`term`** (`ControlCommands.cpp`, `ControlHost::terminalRun`): the
  window's one terminal (no tabs, item 4.7), shown with the keyboard left
  where it was; the answer is `{terminal, reused: true, typed}`, with
  `title_ignored` for `--title`. `--cwd` types `cd DIR` (with `--run`, `cd
  DIR && COMMAND`, one line). `--split` still says it is not on Linux yet.
  An error answer begins "the terminal was left alone:" and the reason.
  `term-output <id> [--lines N]` is `Terminal::recentText` (VTE's
  `get_text_range_format` over the scrollback, trailing blank rows cut).
- **Run File** did not exist on Linux (the note said to find it in
  `linux/src`): `act_run_file` in `main.cpp`, `win.runfile` and Ctrl+Shift+
  Return (Global, the Mac's Shift+Cmd+Return), with the Mac's rules:
  `Settings::runTemplate`, `RunFile::expand`, the nearest virtualenv's
  python, saved first, never into a busy terminal (the status bar says "Not
  run: <why>"). `run.terminal = new` and the busy case's new terminal wait
  for tabs. `RunFile.cpp` joined both core lists.
- **`onCommandFinished`** is called from the finishing mark (the done count
  went up; counters, not the busy flag, since a short command's start and
  finish can arrive in one VTE read). The 0.25 s polling after Return
  stays only for a shell with no nonce (fish, sh), where there are no marks.
- **install-skill** (November 2026): `minicode install-skill [--dir DIR]`
  is `minicodeInstallSkill` in `tools/minicode-cli.cpp` (Linux only; the
  Mac's is the launcher script), called by `main` before the control
  channel's words since it is not one. It copies `skills/minicode/SKILL.md`
  to `$CLAUDE_CONFIG_DIR/skills/minicode` or `~/.claude/skills/minicode`,
  says "up to date" when the bytes agree, exits 3 with no skill found and 2
  on a stray argument. The skill is found at `$MINICODE_SKILL_FILE`, else
  `SKILL.md` beside the binary (a build folder: `configure_file` copy), else
  `../share/minicode/skills/minicode/SKILL.md` (`install_data` in
  `meson.build`, so the package has it), else the checkout's own. The
  `control` suite installs with `--dir` into its scratch folder.
- **Left of 3.1**: nothing of `split` (done with item 60).

### 72. Undo: the core's history (done, November 2026; SESSION-DESIGN.md stage 2)

GtkTextBuffer's undo (GtkTextHistory) is off for every document, and each
`Document` records its edits into the core's `Model::UndoHistory`
(`src/UndoHistory.{h,cpp}`, in code points), the history the Mac's editor
moves onto next. What a user meets: the Mac's undo rules.
- **Steps.** A user action (`gtk_text_buffer_begin_user_action`) is one
  step. One nobody named is typing, which joins the step before while the
  caret stays where that step left it, as NSTextView's typing does: a run
  typed with spaces and Returns is one Ctrl+Z, where GtkTextHistory broke
  it at words. A click, an arrow, an undo or redo (`mark-set` on the
  insert mark, `noteSelection`) or `breakUndoCoalescing()` starts a new
  one. Anything else names its step first and is never joined:
  `MultiEdit::apply(..., stepName)` ("Edit" when none: Replace All,
  Replace, Format Document, Revert Change, Replace in Folder, Snippet,
  Completion, Typing at several carets), `Document::nameStep(buffer,
  name)` before a user action (auto-close's pair, Preview Edit, Toggle
  Line Comment, Toggle Task, Edit Cell, Change Color, Change Opacity,
  Adjust Settings, Paste and Cut from the view's signals, Reload from
  Disk). The first name a step is given stands, so a MultiEdit inside a
  caller's named action keeps the caller's. Edits outside any user action
  (code or a test calling `gtk_text_buffer_insert`, `set_text`) are one
  step a turn of the main loop ("Edit"), closed in a high-priority idle
  or when the next action, undo or `canUndo()` comes.
- **Recording** is in `onInsertText`/`onDeleteRange`, before the buffer
  changes (the deleted text is read then with `get_slice`), skipped while
  filling, loading (`load_`), or putting an undo back (`replaying_`).
  `fill()` (a file read, a message) and `finishLoading` start the history
  over. A multi-range edit arrives last first (a delete and an insert per
  range) and the core takes such a sequence whole in one pass.
- **Undo** (`Document::undo`/`redo`, `UndoResult`): the core checks every
  range still holds what the step left there, else refuses, changes
  nothing, forgets the history and the editor rings the bell (logged on
  stderr). Done, it emits the buffer's own "undo" or "redo" signal (the
  snippet session and the completion waiting for its import listen for
  them), puts the plan in as one `MultiEdit::apply` that is not recorded,
  and selects what was selected before the step (after it, for a redo).
  Reload from Disk (a clean tab following its file, and Reload chosen in
  the question) is a step that keeps the ones before it.
- **Keys and menu.** `Editor::onViewKey` takes Ctrl+Z, Ctrl+Shift+Z and
  Ctrl+Y in the source and in a Markdown page (`undoInSource`) before the
  view's bindings, which reach GTK's undo and do nothing now; the CSV
  table and the LaTeX preview call the document's too. GtkTextView's own
  right click Undo and Redo name "text.undo" and "text.redo", class
  actions that ask GTK's undo; `Editor::adoptViewMenu` inserts a "text"
  group of ours on the menu's popover (asked before the view's class
  actions) once the view has made it, and relabels the items in the
  view's own sections ("Undo Typing", `refreshUndoMenu`, run as a right
  click or the menu key starts).
- **Checks**: the core's `undo:*` (each rule, a fuzz against a plain
  string in both units, the speed of a 20,000-range step) and the
  `undo_history` suite (typing as one step and its caret, the run broken
  by Left and Right, Ctrl+Y, a three-range MultiEdit, Reload from Disk
  undone and redone with the step before it kept, the right click menu's
  Undo Typing taking the run back). Suites that read GTK's undo
  (`gtk_text_buffer_get_can_undo`) ask the `Document` now.
- **Not done**: the workspace edit applier and the inline agent's Keep are
  named "Edit"; drag and drop of text is typing; carets beyond the first
  are not restored by an undo.

### 73. Tabs: the core's window (done, November 2026; SESSION-DESIGN.md stage 3a)

The editor's tabs, their order, the two editor groups, the tab on show in
each, the preview tabs and Reopen Closed Tab's list are a `Model::Window`
of the core's (`src/EditorTabs.{h,cpp}`, beside the rules it was already
asking), one of a process-wide `Model::Session` that every window's
`Editor` joins as it is made and leaves as it goes (`Editor::session()`,
`Editor::window()`). The Mac's EditorController moves onto the same
window in stage 3c; sessions.json and the backup planner follow in 3b.
- **What moved.** `tabs_`, `other_`, `split_`, `focused_`,
  `groupFraction_`, `closed_`, the use stamps and `Document`'s
  `previewTab_` and `agentPreview_` are gone. The window names documents
  by `Document::id()`; the editor keeps the documents (`docs_`, made by
  `newDocument()`, let go in `destroy()`) and shows what the window says.
  `active_` and `otherActive_` stay as what the view and the mirror show,
  set from `window().active()` and `active(other())` at the same points as
  before. `Document::isPreviewTab()`, `isAgentPreview()` and `lastUsed()`
  ask the window through `DocumentHost`; a session's preview tab comes
  back through `Editor::setPreview`.
- **Paths.** The window keeps each tab's file ("" for a diff, a
  comparison or a page: `Editor::fileOf`) for labels, Reopen Closed Tab
  and the groups' session; `syncPaths()` gives it every document's in
  `refreshStrip` and before an open, so a rename or Save As needs no hook
  of its own.
- **The rules** are the window's now: `prepareOpen` (in preview or not;
  a code navigation keeping the preview tab it starts from; a kept open
  keeping a preview), `add` (the preview tab's place and replacement, the
  agents' preview tab, a dirty or typed-in one kept instead, the replaced
  tab out of both groups and never remembered), `activate` (a tab only
  the other group has joins this one), `close` (the tab used most
  recently, a group closing with its last tab, a tab the other group
  still has not remembered), `putInOther`, `focus`, `canMove`,
  `restoreGroups` and `groupsSession`. Groups.cpp keeps the views: the
  mirror's places, the swap, the bar.
- **New for a user**: a tab can be dragged along either group's row
  (`TabStrip::onMove`, `Window::move`), as the browser's could; the
  order is kept in the session. (A preview tab replaced while the other
  group also showed it left both groups here; item 84 put back the Mac's
  rule, under which it stays in the other.)
- **Checks**: the core's `session:*` (each rule of "Tabs, sessions and
  the disk", "The preview tab" and "Two editor groups" in docs/notes/sessions.md, and
  a random walk holding the window's bookkeeping), the `tabs` suite (the
  strip checked against the window, a tab dragged to the end and back)
  and `split_editor` (the groups checked against the window after each
  move, a drag in the left group's row, kept by a new window's session).

### 74. File tree icons by kind (done, November 2026)

A file row's icon follows `FileIcon::kindOf` (what a click on the file opens), as the Mac's does: `fileIconName` in `linux/src/FileTree.cpp` maps each kind to a symbolic name that Adwaita has (checked against `/usr/share/icons/Adwaita/symbolic`, since GTK draws the missing-icon picture for a name the theme lacks): image, video and audio `*-x-generic`, `x-office-document` for a PDF, `x-office-spreadsheet` for a table, `accessories-dictionary` for a notebook, `drive-harddisk` for a database, `package-x-generic` for an archive, `web-browser` for a web page, `user-bookmarks` for a bookmark, `utilities-terminal` for source code (Adwaita has no script icon), `text-x-generic` for the rest. Folders keep `folder-symbolic`. The `tree_icons` suite writes one file of each kind and reads the names off the rows. Other themes may draw these names differently; the look needs the owner's eyes.

### 75. A notebook's In and Out prompts (done, November 2026)

A fenced block with a `MdRun::label` (the core's Notebook gives a run cell "In [n]" and its result "Out [n]") gets that text on a line of its own above the block, at the block's first run only, in the new `md_codelabel` tag: monospace 11 points in the Markdown quote color, as the Mac's `markdownLabelFor:`. The tag is made with the others in `Editor.cpp` and recolored in `applySettings`. The notebook check in `json_outline` now reads "In [3]" and "Out [3]" from the shown text, in order before their blocks, and the tag at the label. The Mac's extra indent for a block inside a list is not copied, since notebook cells are never in lists. The look needs the owner's eyes.

### 76. Sessions and the save planner in the core (done, November 2026; SESSION-DESIGN.md stage 3b)

What a window keeps between runs is one core record, and when its unsaved
tabs are written is decided by the core.
- **The record.** `TabSessions::Window` (W1's, `src/TabSessions.h`: folder,
  frame, its own tabs with both groups, the tab on show, carets and
  scroll, the sidebar, the editor, Problems, the terminal dock, the tree)
  is what the Mac keeps in `MCOpenWindows` and now what sessions.json
  keeps under the key `"windows"`, beside the folders' sessions
  (`TabSessions::Store`, `encodeStore`/`decodeStore`). Folder keys are
  absolute paths, so the key never clashes; a sessions.json from before
  has no windows and reads as before, and a minicode from before skips the
  key and keeps the folders (it drops the windows when it writes).
  `Persistence::putSession` and `putWindows` both go through
  `changeStore`, under the one lock.
- **Windows back on a desktop launch.** `Persistence::windowRecord()` is
  the window's folder and own tabs plus what `Host::windowState` adds:
  its size (`gtk_window_get_default_size`; Wayland gives no place, so the
  frame's x and y stay 0), and whether the sidebar, the editor, Problems
  and the terminal showed. `saveOpenWindows()` (main.cpp) writes every
  window, front first, with each session save (`Host::windowsChanged`)
  and at Quit; `onWindowRemoved` writes the others, or the closing window
  alone when it was the last (`TabSessions::afterClosing`'s rule). A
  launch with no path, from the home folder or `/` (the dock, the desktop
  entry), with no window open yet, runs `restoreOpenWindows()`: each
  window whose folder is still there, back to front, through
  `newWindow(root, "", &record)`, which sets its size, restores its own
  tabs (`restoreSession(&record.tabs)`, backups included) and, on the
  next idle, hides its sidebar, shows its terminal and Problems. Not yet:
  the tree's open folders and selection, the terminal's tabs and splits
  (a single fresh shell comes back), the sidebar's width, a hidden editor
  (recorded, not restored), maximized. `minicode` typed in a project
  folder still opens that folder.
- **The planner.** `SavePlan::Planner` (`src/SavePlan.{h,cpp}`) holds the
  Mac's timing rules: a dirty tab's backup 1 s after typing stops and at
  most 5 s after the first change not yet written, a clean tab's backup
  removed at once, after-delay auto-save restarted by every edit, a
  focus-change save due at once and never cancelled by an edit. The port
  gives it the tabs (`SavePlan::Tab`: id, dirty, generation, path, and
  what the backup holds, from `Document::Backup`) and the monotonic time;
  `nextDue()` says when to tick and `tick(now)` what to write.
  Persistence's three timers (backup, auto-save delay, focus idle) are one,
  `planTimer_`, set by `arm()`; due now is a `G_PRIORITY_HIGH` idle as the
  focus save was. The Mac's EditorController+Persistence.mm keeps its own
  timers until 3c; `saveplan:*` pins its rules.
- **Checks**: the core's `saveplan:*` and `tab-sessions:store`; the
  `sessions` suite's new windows part (a child opens two windows on two
  folders, places a caret, notes a size, hides one sidebar and quits; a
  child launched with no path from the home folder gets both back, each
  with its own tabs, caret, size and sidebar; `MCGuiRunChild` takes an
  empty folder for that); the `backups` suite's new timing part (no backup
  the moment after a key, one once typing pauses, one within 5 s of
  typing that never pauses, removed at once on save).

### 77. The session save on the planner, flushed before another window reads (done, November 2026; SESSION-DESIGN.md stage 3c)

A window's tabs changing wrote its session a second later on a timer of
its own (`sessionTimer_`), so a second window opened on the same folder
within that second read the folder's session as it had been (the
`split_editor` flake, #81). The second is now the planner's
(`SavePlan::Planner::sessionChanged`, due 1 s after the first change not
yet written; `Plan::saveSession` from `tick`), on the one `planTimer_`.
`restoreSession` for a folder (a new window, Open Folder) first has every
other window on it write a pending save (`Persistence::flushSession`,
`Planner::takeSession`), and any save takes the pending one, so it is not
written twice. Core checks: `saveplan:session` and
`saveplan:session-before-another-window-reads`. The Mac moved onto the
planner for backups and auto-save in the same stage; it writes its
session at once, to the same sessions.json format in its own folder.

### 78. Replace in Folder names files over 1 MB as too large (done, November 2026)

The core's `planReplace` leaves files over `FolderSearchOptions::maxFileSize`
alone and reports them in `FolderReplacePlan::tooLarge` and `tooLargeCount`;
`SearchPanel::planned` now says what the Mac's does: "Nothing replaced: the
matches are in files larger than 1 MB, which Replace in Folder leaves alone
(big.txt)" when nothing else is left, " Left alone, since they are larger
than 1 MB: big.txt." in the question, "and N more" past the listed names, and
"big.txt is larger than 1 MB, which Replace in Folder leaves alone" where the
write job cannot read a file. `replace_in_folder` writes a file just over
1 MB with a match at its end: Find in Folder lists it, Replace All names it
as above, and it is byte for byte as it was. Checked: that suite (20 passed;
3 fail with the fix taken out).

### 79. The workspace-edit applier's and the inline agent's undo steps are named (done, November 2026)

`LspSession::applyEdit` takes the step's name and gives it to
`MultiEdit::apply`: "Rename" for F2, the code action's title for a quick fix,
the server's `label` (else "Edit") for workspace/applyEdit, and "Format
Document" for the server fallback, as the Mac's `applyWorkspaceEdit:...
actionName:`. The inline agent's Keep names its step "Inline Agent", as the
Mac's `applyText`. Checks: `lsp_actions` reads `undoName()` of the renamed
files (c.py, a.py) and `agent_panel` that of the kept edit; all three new
checks fail ("Edit") with the src change taken out. The "Not done" note in
item 72 about these two is now done.

### 80. The offer of format on save and Edit > Format on Save (done, November 2026)

The Mac's offer bar (`FormatOnSave.mm`) and menu item. After Ctrl+S saves a
file whose project's own configuration chose its formatter and that
formatter is installed (`FormatRun::onSaveOffer`, over
`Formatter::offersOnSave` and the same formatter lookup Format Document
uses, with a core check `format-run:on-save-offer`), while format.on-save is
off and the offer is not muted, a bar under the tabs (`FormatOffer.{h,cpp}`,
the consent bar's look) asks "This project formats Python files with ruff,
set up in pyproject.toml. Format them each time you save?": Turn On writes
`format.on-save = project` (through `AppSettings::writeValue`, so the same
refusal while the settings file has unsaved changes) and says "Format on
save is on for formatters a project sets up. Edit > Format on Save turns it
off." in the status bar; Don't Ask Again is kept in install-hints.json under
"format-on-save", with the install bar's; × closes it until minicode starts
again. Once per launch across windows; the look runs off the main thread
(`FormatRunner::offerOnSave`) and Save All and auto-save never offer. The
settings turning format on some other way closes the bar. Edit > Format on
Save (`minicode.toggleFormatOnSave`, no key, `app.formatonsave`) is checked
while format.on-save is on and writes `false` or `project`. Not offered
after a Ctrl+S that went through organize imports first, which saves
through Formatting's own path. Checked by the `format_offer` suite.

### 81. A double-click in the LaTeX preview on text from an \input file edits that file (done, November 2026)

The Mac's change (30eddf9, aec3869, 03ce0c5) in the GTK preview. The core
had the pieces (`SyncTexIndex::tagsAtPoint`, `inputs()`,
`LatexDoc::parseInput`); `LatexPreview::doubleClick` now tries the inputs
the boxes under the point name, likeliest first, then the document, each
parsed on its own from its tab's buffer or else the disk
(`docOfInputFile`, parsed again only when the text changed). The box is
titled "Editing text in chapter.tex"; its text and lists are that file's.
Save is a splice of that file's text, made by the editor
(`LatexPreview::InputHost`, `Editor::editLatexInput`): into the file's tab,
or a tab made beside the active one and not shown (`openForAgent` quiet,
then read), unsaved, one "Edit in Preview" step; nothing is written. It is
refused with "chapter.tex changed while the box was open, so the edit was
not made" when the file's text moved, and "chapter.tex could not be
changed" when its tab cannot take it. The bar says "chapter.tex has
unsaved changes: save to see them in the PDF" (or "N files it reads have
unsaved changes: save to see them in the PDF"), since tectonic reads the
disk; Ctrl+S with the preview on show saves those files too
(`Editor::saveLatexInputs`, from `act_save`), and any save of a file a
preview read typesets that preview again (`Editor::latexFileSaved`). The
document's own edit is now named "Edit in Preview" as well. The Mac's
"clicked word selected" fix (03ce0c5) has no counterpart: the GTK box
selects the whole span for every click. Checked by `latex_inputs`, over the
committed three-file fixture and a stand-in tectonic.

### 82. Show in Processes in the terminal's close question (done, November 2026)

The close question of a terminal with a program running (`askCloseBusyTerminal`
in main.cpp, still an async `GtkAlertDialog` answered in one callback) has the
Mac's third button, Show in Processes. It shows the Processes panel (View >
Toggle Processes when hidden) and calls `ProcessesPanel::revealTerminal(id,
job)`, the Mac's `revealTerminal:job:`: the row of the foreground process
group's leader (`Terminal::foregroundJob`, read when the question is asked)
is selected and scrolled to; a job not seen yet waits up to 3 s for a look,
then the terminal's first running process stands in (15 s in all). Nothing is
closed. `processes` closes the busy terminal, presses the button and checks
the panel, the selected row (`selectedPid`) and that the terminal is still
there. The question's wording is Linux's own ("Close this terminal?"); the Mac
names the command, which the GTK terminal does not know.

### 83. The Claude Code hooks offer (done, November 2026)

The Mac's `ClaudeHooksOffer` on GTK: when Claude Code starts in a terminal a
bar between the terminal tabs and the terminals (`TerminalDock::setNotice`)
offers Turn On…, Not Now and ×, once per launch across windows, unless the
hooks are in already or Not Now was pressed (kept as `claude-hooks` in the
install bar's muted list, `ConsentBar::installMuted`, in
`$XDG_STATE_HOME/minicode/install-hints.json`). The rule is the core's
`ClaudeHooks::shouldOffer` (`hooks:offer`). "Started" is
`Terminal::onClaudeStarted`: on the shell integration's mark that a command
began, `ClaudeHooks::startsClaude` on its text at once, else one second later
`isClaudeProgramPath` on `/proc/<foreground group>/exe`; a shell without the
integration (fish, sh) offers nothing, as on the Mac without 633;E.
Turn On… (`linux/src/ClaudeHooksOffer.{h,cpp}`) asks with the JSON that would
be added in the alert's detail (Add the Hooks, Cancel), writes through
`ClaudeHooks::install` (backup first) and says where. Not ported: minicode >
Install Claude Code Hooks… and the Agents panel's header line.
`claude_hooks_offer` checks it against the scratch `CLAUDE_CONFIG_DIR`.

### 84. One set of tab rules with the Mac (done, November 2026; SESSION-DESIGN.md stage 3d)

Stage 3c left the Mac on its own rules where `Model::Window`'s differed;
the window now follows the Mac's (docs/notes/sessions.md, "The preview tab" and "Two
editor groups") and the Mac's copies are gone, so both ports run the same
code. What changes on Linux:
- **A preview tab per group.** A tree click (or a search result's) replaces
  the focused group's preview tab only; one the other group has is left
  alone (before, the window's one preview tab was replaced wherever it
  was).
- **A replaced preview tab the other group also shows stays there**, still
  a preview; only the focused group's tab of it closes (before, it left
  both groups).
- **The agents have two preview tabs**: `minicode open --note` replaces
  the files' one, `minicode diff` (without `--keep`) the comparisons'
  (`Model::Preview::AgentDiff`), so a diff after a note leaves the note's
  tab. Before, every `diff` opened a kept tab of its own.
- **The agents' new preview tab takes the old one's place** in the row,
  not a place beside the tab on show.
- **While the user types**, an agent's open never replaces the agents'
  preview tab they are in, and that tab stays a preview (`passOver`);
  before, it was made an ordinary tab.
- **A kept open of a file in the agents' preview tab keeps it** (a
  double-click in the tree, Return, a menu), as the user's preview tab is
  kept; before, only the user's was.
- **An agent's open of a file already in a tab shows that tab as it is**
  (`Editor::openForAgent`): a preview stays a preview. Before, it went
  through `openFile` as a kept open, which kept it.

The rules are `Window::add` (its new `passOver`, the agents' place, a
User preview closed in the focused group only), `previewTab` (the focused
group's for User) and `prepareOpen` (any preview kept by a kept open).
Checked by the core's `session:preview` and `session:groups` groups. The
GTK files changed: `linux/src/Tabs.cpp`, `Editor.h`, `Document.h`,
`ComparePanes.h` and `ControlCommands.cpp`; no GTK suite needed changing.

### 85. The review page of uncommitted changes (done, November 2026)

The Mac's change review (A1, `mac/src/ChangeReviewPage.mm`) in a tab:
`ChangeReviewPage` (`linux/src/ChangeReviewPage.{h,cpp}`, an `EditorPage`
with the key `review:<folder>`, one a window, shown again when open). Every
rule is the core's. `ChangeReview` (rows, folding, what a block makes of the
file) was already tested but not built on Linux; `ChangeReview.cpp` and the
new `ChangeReviewRead.{h,cpp}` are in both build lists. `ChangeReviewRead`
holds what the Mac kept in its `ReadReview`, `writeIndex` and `discardText`,
over a `GitRunner` the caller gives (`review-read:git` runs it against a real
git): one status, ls-files, ls-tree and cat-file --batch, then the work
tree's files; `stageText` (hash-object and update-index after checking the
entry is still the object read) and `discardText` (checks the file and the
entry, has the caller put a copy in the Trash, writes atomically). The Mac's
file still has its own copy; moving it onto the core is a Mac change left
for someone who can run the Mac's suite.

The page is a read-only `GtkTextView`: a row a line, a tint a paragraph
background (red, green, grey for headers, folded stretches), the changed words
and the syntax colors tags over them (`syntaxColor` from the settings), and
each row's buttons a `GtkBox` in a child anchor at its end. A click on a
folded row opens it (`Opened`, laid out again with no git). Reading and each
action run on the Source Control panel's git thread (`GitPanel::runOnGitThread`),
so they stay in order with the panel's commands; Discard asks through the
panel's question (`askQuestion`), copies to the Trash with `GitTrash::trashCopy`
and tells the panel (`rememberDiscard`), so the panel's Undo Discard and the
page's button are one list. The panel reads while the page is open
(`setReviewOpen`: `refresh()` no longer needs the panel showing, and main.cpp's
three `gitShown` tests are the panel's own now) and calls
`onReviewRefresh` after each status, which is how a change made elsewhere
reaches the page; a read whose fingerprint and text are the last one's
changes nothing on screen, so the eye and a selection stay. Not done:
F7 and Shift+F7, the right click on selected lines, a conflict's Keep
buttons (a conflicted file shows its blocks and has Open), the commit and
worktree pages, and a click on a file in the panel still shows its diff.
Wording is the Mac's (`ChangeReview::plan`'s questions and done texts, the
buttons' titles). `git_review` (after `git_verbs` in suites.txt) presses
the panel's button, a block's Stage, Discard with Cancel and with the answer,
Undo Discard, Stage File, and a change made with the panel hidden.

### 86. An .html file opens as its page (done, November 2026)

The Mac's HTML page (mac/src/HtmlPage.mm, 1.5.27 and the fixes after it) on
GTK: an .html, .htm or .xhtml file (FileSniff::Kind::Html, `Document::isHtml`,
browser builds only) opens as its page in the editor's slot, a viewer over
the tab's text like the CSV table (`Editor::showHtml`, beside `showTable`),
and Ctrl+Shift+V flips to the source and back, giving the keyboard to the
page when it is asked for (`togglePreview`). An empty file opens in its
source. The rules are the core's (`src/HtmlPage.{h,cpp}`, `htmlpage:*`):
what `html.scripts` and the file's answer make of a page (`scripts`), the
bar's and the answered line's wording, the folder a page may read, which
navigations load and which go to the link handler or are dropped as stale
(`decide`: a reload, a back or forward, or a load this page asked for
earlier arrives after the tab moved on and is never taken for a link). The
Mac keeps its own copy of those rules in the .mm; it was not changed here.
The view is `HtmlView.{h,cpp}`: a box with the bar (the consent bar's look,
`minicode-consent-bar`) over a WebKitWebView on an ephemeral network
session, JavaScript off in its settings until allowed (`html.scripts` on, or
Run Scripts, or an answer held for the file until quit; the setting is the
page's and takes effect on the load that follows, so an answer loads the page
again). The bar says "This page has scripts. Run them?" with Run Scripts,
Don't Run and "Remember for every HTML file", which writes `html.scripts =
on` or `off` through `Editor::htmlRemember` (main.cpp's `writeValue`); an
answer leaves a line in the bar's place ("This page's scripts run, until
minicode quits.") with an ×, so the page does not move. A save, or a write
from outside, loads the page again (its own `GFileMonitor`, 200 ms after the
last event, and a stamp of mtime, size and inode), reading the scroll first
and putting it back every 100 ms until it holds (the Mac's), with a
generation count so an older answer is dropped. A link or `window.open`
leaving the page goes to `Editor::openMarkdownLink` (a local file in a tab, a
web address in the browser pane). The web view is not focusable until a click
or the toggle key, so an autofocus field cannot take the keyboard.
Differences: WebKitGTK has no read-access folder for a file page (the Mac's
`loadFileURL:allowingReadAccessToURL:`), so subresources load from wherever
the page names them (scripts still cannot read other files, as file access
from file URLs is off). The frame's name is not used to tell a frame from the
main frame (most iframes have none): `decide` lets a navigation leave the page
only when the user made it (a clicked link, a submitted form, a gesture), so
an iframe loads; and a script's navigation of the main frame (`location =`) is
caught at `WEBKIT_LOAD_COMMITTED` (at STARTED the view still names the old
address), where `HtmlPage::started` stops the load, the page is loaded again in
its place and the address goes to the link handler (or only the restore, for a
stale load). `html_page` checks the page's text and style, the
script not running and then running after Run Scripts, the keyboard, the
toggle, a write from outside keeping the scroll, a link to another file, the
remembered answer's setting and `off`, an unnamed iframe to a local file
(its text shows, nothing opens), and a script's `location =` (opened as a
link, the page kept). Item 121 changed the last: a navigation the page
starts by itself no longer opens a file, nor another app.
### 87. The session's host owns each document's text and undo (done, November 2026; SESSION-DESIGN.md stage 4a)

What a user meets: nothing, by design. Who owns the text changed under
every edit, undo and language server sync.
- **The host.** One `Shared::SessionHost` for the process, on a thread of
  its own, and one `Shared::Client` in code points
  (`linux/src/SessionLink.{h,cpp}`; the core's `SessionHost.h`,
  `SessionClient.h`, `SessionProtocol.h`, `Ot.h`). The host owns every
  open document's `Model::Document` (text, version) and its
  `Model::UndoHistory`; the client's messages from the host are
  dispatched in a main-loop idle as they arrive.
- **The buffer is a mirror.** A `Document` opens on the host when it is
  made (an empty text), `fill` and `finishLoading` reset the host's copy
  (its history gone), a kind other than Text closes it, and so does the
  destructor. Each buffer edit is reported from `onInsertText` and
  `onDeleteRange` as before (no longer reading the deleted text) with
  `Client::record`, inside the step the user action (or a main-loop turn's
  edits outside one) opened, and sent when the step closes; one edit is in
  flight at a time, the rest wait and join while they are one step.
  Coalescing is decided here, as before: a moved caret (`mark-set`) or
  `breakUndoCoalescing()` marks the next step fresh.
- **Undo goes to the host.** `Document::undo`/`redo` ask and wait for the
  answer (in one process a round trip through the host's queue); the edit
  comes back through `Document::hostEdited`, which emits the buffer's
  "undo" or "redo" signal first, puts it in as one `MultiEdit::apply` that
  is not reported back (one operation at a time for any other shape), and
  selects what the host says. `canUndo`, `undoName` and the rest settle
  with the host first.
- **The language server sync reads the host's text**
  (`LspSessionSync.cpp`): each document a server has is watched through
  the client (`Client::watch`, a copy of the host's text kept from the
  host's own edits, in UTF-16), `didOpen` sends the copy, the host's edits
  feed the `ChangeTracker`, and `flushChanges` settles first, so the copy
  holds every edit made so far. The buffer's insert-text and delete-range
  handlers there are gone.
- **Tabs.** `refreshStrip` reports the window's tabs
  (`Shared::windowJson`, sent when they changed); the host keeps them and
  answers `session.state` with them. The rules stay `Model::Window`'s in
  the editor: with one screen per window there is nothing to decide
  between, and stage 6 makes the host the one that decides.
- **Checks.** At every suite's end the harness settles with the host and
  fails the suite unless the host's text of every document a window has
  read is its buffer's, the host's tabs are the window's, and the host
  reported nothing wrong (`checkSessionMirrors`). `MINICODE_HOST=loopback`
  puts every message through the protocol's encoding and back;
  `MINICODE_HOST_DELAY_US` delays each one on the host's thread (the GUI
  harness's wrapper passes both through). CI's GTK job runs a named set of
  editing suites that way after the two halves ("GUI checks over the
  session host's wire format", `GUI_LOOPBACK` in ci.yml, 200 us a
  message). The core's
  `ot:*` and `host:*` groups hold the transform and the host to a fuzz of
  renderers in both units.
- **Not moved yet** (stage 7): saving, the disk and its stamps, dirty,
  backups, auto-save, the file watch and git stay in GTK and read the
  buffer. The host's own saved text is set at open, reset and `doc.saved`,
  so its dirty flag is wrong for a tab restored from a backup; nothing
  reads it yet. A large file costs the host a UTF-16 copy and its saved
  text (about 100 MB for 25 MB).
- **Trap for stage 6**: a wait (undo, `canUndo`, a flush) dispatches every
  message in order, so another screen's edit could reach the buffer
  inside the signal handler that asked. With one screen nothing but an
  undo's own edit arrives during a wait.

GTK files changed: `SessionLink.{h,cpp}` (new), `Document.{h,cpp}`,
`LargeFileView.cpp`, `Editor.{h,cpp}`, `Tabs.cpp`, `Lsp.{h,cpp}`,
`LspSessionPrivate.h`, `LspSessionSync.cpp`, `meson.build`, `Makefile`,
`tests/gui/Harness.cpp` and `tests/gui/Wrapper.cpp`.


### 88. Compare with Main in one scroll, and Apply to Main (done, November 2026)

The Mac's worktree page (`ae95033`, `f714354`) on the review page of item 85.
The tree's Worktrees node menu, `win.compareworktree`, opens a
`ChangeReviewPage` made with its second constructor (the worktree's folder,
its name, its branch, the main branch): the same page over what the worktree
changed since its branch left main, committed or not, plus its untracked
files, against main at the merge base (`AgentView::compareBase`, so main's
later work is never shown as the agent deleting it), every file plain (Side
by Side and Open, no Stage), the header "against main at the branch point
(1a2b3c4)  .  branch  .  3 files, +4 -1". Key `worktree:<folder>`, title "agent-wt
vs main", one page a worktree, shown again (and read again) when open.
`WorktreeChangesPage` is gone; `AgentWorktrees::changes` and `comparison` have
no caller now and can go with the next change there.

The rules are the core's, in `ChangeReviewRead.{h,cpp}` beside the review's:
`readWorktree` (merge base, `diff --raw`, untracked files, the old blobs in one
cat-file --batch), `planApplyToMain` (main's status, then
`GitUi::applyToMainPlan`: refused naming the files when main has changes of
its own in them, else the question), and `applyToMain` (the worktree's
`diff --binary` against the merge base plus a `--no-index` patch for each
untracked file, main's `apply`, and when it fails `apply --3way`, whose
cleanly applied files are unstaged; conflicts listed; never a commit). All run
over the `GitRunner` the caller gives, which now may report the exit status
(`GitOut::status`: `diff --no-index` exits 1 when files differ). The Mac's
own copy in `ChangeReviewPage.mm` is untouched; moving it onto these is a Mac
change. `review-worktree:git` (tests/change_review_read_tests.inc) runs them on
a scratch repository with a main and a `git worktree add`: committed and
uncommitted changes, main's later work left out, an untracked file, the refusal
while main is dirty in a file, the apply (files in main, nothing staged or
committed), the three-way clash, and a worktree with no changes.

The page's Apply to Main... runs on the Source Control panel's git thread
(`runOnGitThread`, so in order with its commands) and asks through the panel's
question (`askQuestion`: Apply to Main, Cancel). Afterwards the result is on a
line of its own under the bar with Show in Source Control beside it
(`onShowSourceControl`: main.cpp shows the Source Control panel and the
uncommitted review when this window is on the main checkout and not on the
worktree, else opens a window on the main checkout), and the button reads
"Applied to Main", unavailable, until the worktree's fingerprint differs from
the one brought in. Wording is the Mac's (`applyDoneText`, `applyFailureText`).
The worktree page does not follow the panel's reads (the Mac's does not
either): it reads when opened, on Compare with Main again, and after an apply.
Not done: a refresh while the agent works (the Mac's watches the folder), and
the commit page. The `agents` suite's Worktrees part presses it: the page
shows the agent's three files and none of main's later work, Side by Side on
base.txt, Apply to Main with Cancel (nothing changes) and with the answer
(files in main's checkout, nothing staged or committed, "Brought 3 files into
main as uncommitted changes."), Show in Source Control, and the button coming
back when the agent adds a file.

### 89. A heap corruption once in CI's second half, not yet placed (open, November 2026)

CI's GTK job on PR #108 (Mac files only) aborted once in the second half
with `realloc(): invalid next size` just as `recent_folders` began, on a
thread other than the main one (the main thread went on through ten more
steps, then hung opening a window), and printed no backtrace, most likely
because the crash handler's first `backtrace()` allocates and the aborting
thread held the malloc lock.
No such abort is in any failed run from November 16 on.

What was tried, on this machine (`linux/build-asan`, `linux/build-tsan`,
neither committed):
- **ASan** (`meson setup build-asan -Db_sanitize=address -Db_lundef=false
  -Dterminal=enabled -Dbrowser=enabled -Dpdf=enabled`; run with
  `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1
  MC_SKIP_TIMING=1 MC_GUI_ROOT=/tmp MC_GUI_SUITES="..."`): the sixteen
  loopback suites with `recent_folders`, plain and with
  `MINICODE_HOST=loopback MINICODE_HOST_DELAY_US=200`, and every suite the
  second half ran up to the crash, in CI's order, and 27 of them again
  beside other agents' builds. Clean but for one read
  in a check's own helper (`GitText` in `tests/gui/GitChecks.h` passed a
  dead temporary's `c_str()` as git's path; fixed).
- **TSan on the session's host** (`src/SessionHost.*`, `SessionClient.*`,
  `Ot.*`): a stand-alone hammer of the GTK pattern (host thread, one client
  dispatched from a "main loop", documents opened, edited, undone, reset,
  watched and closed at random: 150,000 rounds, and 30,000 more over the
  wire format with a delay of 50 us a message) reported nothing, and
  every text matched.
- **TSan on the GTK app** (clang, `-Dbrowser=disabled`; the browser suites
  do not compile without it, so the guitest was linked without them),
  save, tabs, preview_tabs, sessions, backups and the apps they start: of
  250 reports none has a frame of `SessionLink`, `Shared::` or
  `Document::hostEdited`.
  GLib's GMutex and GCond are futexes TSan does not see, so every hand-off
  through a GTask, a GThreadPool or `g_idle_add` shows as a race (the
  tree's listing, Find in Folder, the tree's git colors); those reports
  are noise, not findings.
- By reading: the host's state is touched only on its thread, the client's
  only on the main thread but for `deliver` (under its mutex), messages are
  values, and no GLib call is made from the host thread but
  `g_idle_add_full`.

The harness's crash handler now calls `backtrace()` once at startup, so
the unwinder is loaded before any crash and the handler allocates nothing,
and its FATAL line says whether the main thread crashed and the thread's
id: the next occurrence prints its frames. Still open: which thread
aborted and what wrote past its block; a rerun of CI's two halves at once
(CI's load) under ASan is the next step. Seen once under ASan with the
wire format and a 200 us delay on a loaded machine: two of preview_tabs'
waits timed out at 30 s, and in the loaded second pass control failed
11 checks (no memory error either time); neither was seen otherwise.

### 90. The heap corruption of item 89 was setenv racing a spawn (done, November 2026)

CI's GTK job aborted twice in its second half with `realloc(): invalid
next size` during `recent_folders`' first step, and the harness's crash
handler (item 89) printed the stack:

```
realloc <- g_realloc <- g_environ_setenv <- g_subprocess_launcher_setenv
  <- runGitWithin <- runGit <- (a GThread)
```

`runGitWithin` runs on the git panel's worker thread and makes a
`GSubprocessLauncher`, whose init copies the process environment
(`g_get_environ`). The suite's first step called `g_setenv` on the main
thread at the same moment; glibc's setenv reallocates `environ` and frees
strings while the other thread reads them, so the copy was torn and the
heap corrupted. The product changes its environment only at startup
(`MINICODE_FOREGROUND`), but the GUI suites did it 93 times while the app's
workers started processes.

The fix is `src/EnvLock.{h,cpp}`: a `std::shared_mutex` with an `EnvRead`
guard and `envSet`/`envUnset`, which take it exclusively. The rule: any
spawn or environment read off the main thread holds `EnvRead` while it
reads, never across the wait for a child. Wrapped: the launchers of
`GitRun.cpp`, `Latex.cpp` (`newLauncher`, the tool lookups) and
`LspProcess.cpp`, the `g_spawn_*` with no environment in `Math.cpp` and
`Mermaid.cpp`, `g_find_program_in_path` and `g_getenv` of PATH and the
MINICODE_* paths in those files, and the main thread's own reads
(`Terminal.cpp`, `FormatRunner.cpp`, `InlineAgentBar.cpp`, `main.cpp`,
`ClaudeHooksOffer.cpp`). The GUI suites call `EnvLock::envSet` and
`envUnset`. The test `env lock: launchers while the environment grows` in
`tests/run_tests.cpp` runs a worker making launchers against the main
thread setting new variables.

A sturdier design, not done: runners never read `environ` off the main
thread at all, using an environment snapshot built on the main thread and
rebuilt under the lock when the environment changes. On GTK that also
needs the spawns moved to `g_spawn_async_with_pipes_and_fds` with an
explicit envp and `G_SPAWN_SEARCH_PATH_FROM_ENVP`, because
`GSubprocessLauncher`'s init copies `environ` itself.

### 91. The note's box placed by the overlay (done, November 2026)

`notes` failed in full runs (and in `install_hints processes notes`, where the
terminal dock leaves the editor's view 418 px tall). The cause: the box was
placed by `halign START` and a 56 px `margin_start`, and `reposition` set its
`margin_top` from a measurement. GTK allocated the box less than its own
minimum width (its warning "Trying to measure GtkBox ... for width of 127, but
it needs at least 201": the minimum included the 56 px margin the overlay did
not leave), so the wrapping body label measured 89 to 279 tall at that width,
`margin_top` changed the layout, the next layout measured again, and the box
flipped between two places every frame (top 24 and 39); when a measure came
out tall, `top < 2` put the box below the last line. The fix: no margins or
alignment on the box; `AgentNotes::childPosition` answers the overlay's
`get-child-position` with the whole rectangle (x 56; the natural width held
between the minimum and what the overlay has past the indent; the height
measured at that very width; above the first line, else below the last), so
nothing the box does feeds back into what it is measured at. `reposition`
only asks for a new allocation. The `notes` suite now opens the terminal dock
first, then checks the box is above the first line by its bounds, holds one
place over a quiet half second, and is allocated at least its minimum width;
without the fix it fails 3 of 3 alone, with it 3 of 3 pass, alone and in
`install_hints processes notes`. (The earlier step here, measuring at the
natural width, stays: it is what childPosition does.)

### 92. The session's host owns the terminals' ptys (done, November 2026; SESSION-DESIGN.md stage 5)

What a user meets: the terminal's commands are marked, as on the Mac.
Ctrl+Up and Ctrl+Down (VS Code's Linux keys) move to the previous or next
command that ran, its prompt at the top of the view and the command
outlined; past the newest, back to the end. The waiting dot goes when a key
the user types reaches the shell, not with any key pressed in the terminal
(Ctrl+Shift+C copied and cleared it).
- **The pty is the host's.** `Terminal::spawnShell` builds a
  `Shared::TermSpec` (the shell, the environment as before plus TERM,
  COLORTERM and VTE_VERSION, which VTE's own spawn added, the folder, the
  size VTE has, its 10,000 lines of scrollback, the integration's nonce)
  and opens it through `SessionLink::client()`; VTE has no pty. What the
  shell writes comes back as `termOutput`, fed to VTE with
  `vte_terminal_feed` and acknowledged at low priority once the main loop
  is idle (the host stops reading a pty past 1 MB unacknowledged, so
  `yes` cannot fill the main loop's queue). Everything VTE would write to a
  pty (keys, pastes, `feed_child`, `paste_text`, its answers to DA and
  DSR) comes through its "commit" signal, which VTE emits with no pty too
  (VTE issue 222), and goes to the host's pty in order. VTE's rows and
  columns go to the host when they change (`sendSize`, from the underline
  drawing area's "resize" and VTE's "char-size-changed"). The shell's end
  is the host's `termExited` (the restart rules are as before), the
  destructor closes the terminal on the host, which hangs it up and reaps
  it.
- **The marks are the host's.** VTE never parsed OSC 133, so D10's
  termprop (`vte.ext.minicode.mark`, `parseVteMark`) is gone: the bash
  script and the zsh script (the Mac's, its VTE branch removed) print OSC
  133 A, C and D, 633;E and 7 with the nonce, which the host reads from the
  pty's bytes before VTE sees them (`TermShell`, the core's
  `applyTermEvent`) and `termShell` copies into `ShellState`, firing
  `onShellStateChanged`, `onCommandFinished` and the Claude look as the
  termprop did. `busy()` and `foregroundJob()` read the host's
  `TermShell::foreground` (tcgetpgrp after each read and every 200 ms),
  `shellDirectory()` its believed OSC 7 first (VTE takes the nonce for
  part of a marked OSC 7's address).
- **Moving between commands** (`moveToCommand`, `rowOfLine`): the host
  sends every marked command as lines counted from the first that went
  into its scrollback, and with each output its cursor's line; VTE counts
  its rows the same way, so a command's row is its line plus VTE's cursor
  row minus the host's cursor line. The outline is drawn on the underline
  drawing area. Shift's selection (the Mac's Select to the Previous
  Command) is left out: VTE takes no selection from an app.
- **Checked by** `terminal_marks` (new: marks and statuses, real Ctrl+Up
  and Ctrl+Down, the outline forgotten on typing, the alternate screen
  keeping the keys, `stty size` after the font grows, the waiting dot with
  Ctrl+Shift+C and a typed key; also in the wire-format run) and
  `shell_integration` (forged marks printed by a command, without the
  nonce or with another), and `make test`'s `bash:integration` (the
  script's marks read by a TerminalStream and `applyTermEvent`). Not run on
  the ThinkPad yet: the outline's look, Ctrl+Up with a real keyboard, and a
  resize by dragging.

### 93. The palette lists View > Theme (done, November 2026)

The palette's rows come from `Keys::actions()` (the registry's commands), and
View > Theme's items are not registry commands: they are `app.theme` with a
string target, built from `Settings::themes()` in `Appearance.cpp`. So typing
"theme" found nothing. `PalettePanel::collectCommands` now adds one row per
core theme, "View > Theme > Light Modern" as the Mac's palette reads its menu,
which runs `app.theme` with the id: the same action as the menu item, so the
same `chooseTheme` (unsaved-settings refusal, the question about colors the
file sets, `Settings::withTheme`, the config writer). Checked by the palette
suite's last block. The rows do not mark the current theme (the palette has
no checked rows on either port).

### 94. Focus on Folder (done, November 2026)

The Mac's feature of 2026-11-17 (`../docs/notes/tree.md`, "Focus on
Folder"; `mac/src/EditorController+TreeFocus.mm`), over the same core:
`FileDetails.h`'s `TreeFocus` (normalize, relative, contains, parent,
breadcrumb, widened, restored, fit) and `TabSessions::Session.treeFocus`.
A folder's right click, File > Focus on Folder and the palette
(`minicode.tree.focusFolder`, `win.focusfolder`: the selected folder, a
selected file's folder, else the open file's) root the tree at that
folder; Show Whole Folder in Tree (`minicode.tree.showWholeFolder`,
`win.showwholefolder`) and the breadcrumb row's first part or × give the
whole folder back. No keys, as on the Mac (`CommandsLinux.cpp` has both
rows without one).

- **The model starts lower.** `FileTree::focus_` is a path from `root_`,
  which never changes; `top()` is the folder the rows start at.
  `applyFocus` reads the folders open now, keeps those outside the new top
  in `focusExpanded_` (the Mac's `_treeFocusExpanded`), replaces the model
  (`replaceModel`: a fresh `makeTreeModel()` from `top()` put on the same
  `GtkSingleSelection`, which owns the tree model, so the old one goes
  with its folders' watches and the list view and its handlers stay), and
  opens the folders again through `pendingExpand_` as the listings come.
  Widening opens the old root and the folders down to it and selects its
  row; the selected row stays selected where it still shows. The
  Worktrees node is listed only from the window's folder (`makeDirModel`'s
  `root`), and neither it nor a folder inside a worktree can be focused
  (`inTree`: .claude lists no "worktrees"; a dotfolder only with hidden
  files on). New File, Open Containing Folder and Copy Path with nothing
  selected, and a drop on the empty space, start at `top()`.
- **The breadcrumb** is a box over the scroller in `container()`, which
  the sidebar's stack now holds (`widget()` stays the scrolled window,
  which several suites read): one label per part, a "›" each, an ellipsis
  and a flat × button, laid out by a `GtkCustomLayout` whose allocate
  function measures the labels and shows what `TreeFocus::fit` chooses
  (the rest `gtk_widget_set_child_visible(FALSE)`, the last part cut
  short with its ellipsized label). A press picks the part's label with
  `gtk_widget_pick`; the last part, the root already, does nothing. Its
  right click has Show Whole Folder in Tree. Colors in `ThemeCss.cpp`
  (`.minicode-tree-focus-*`): the sidebar's background, the parts at 60%
  of its text color and the root in full and bold, as the Mac draws them.
- **Gone from the disk.** The tree's own watches start at the focused
  folder, so `watchFocusPath` adds a `GFileMonitor` on each folder from
  the window's folder down to the focused folder's parent; a delete or
  rename there checks `inTree(focus_)` 100 ms later and falls back to the
  whole folder with the Mac's status note (`dropLostFocus`). Turning hidden
  files off with a dotfolder focused does the same. Those monitors also
  pass their events to the activity callback, as the root's watch would.
- **Reveals widen, opens do not.** `revealPath(path, widen)` asks
  `showsPath` first: outside the focus with `widen` it widens to
  `TreeFocus::widened`, without it leaves the tree alone. `widen` is set
  by `revealInTree` (Reveal Active File in File Tree, the tab's Reveal in
  File Tree, source control's), a folder link, New File, New Folder,
  Rename and drops; the tree following the tab on show
  (`revealActiveInTree`), a file link and the opens behind it never widen.
- **Sessions.** `Persistence::Host` has `treeFocus` (asked by `session()`,
  also as the window closes, so it is not behind `app->dead`) and
  `restoreTreeFocus`, which `restoreSession` calls first with the saved
  focus (the window's record or the folder's session), before any tab, so
  a focus alone comes back; `FileTree::restoreFocus` takes it through
  `TreeFocus::restored`, so a folder gone since gives the whole folder. A
  change of focus saves the session at once. The port keeps no open
  folders in a window's record (the Mac does), so a window brought back
  focused opens none.
- **Menus.** The tree's right click has a section of its own with
  `win.focusfolderrow` and `win.showwholefolder`, both
  `hidden-when="action-disabled"`; `updateTreeFocusActions` enables the
  first for a folder row the tree can focus and the second while focused
  (on every right click, change of focus and restore), so the File menu's
  Show Whole Folder in Tree is greyed while the whole folder shows.

Checked by the new `tree_focus` suite (64 checks; real right clicks on a
folder's and a file's row, the menus read; the action; the root and the
breadcrumb, the bar above the rows; a file made inside; real clicks on
the breadcrumb's parts, the × and the bar's own menu, widening with the
folders kept open; Show Whole Folder; a focus too long for the bar, its
first part, ellipsis
and last part; an open outside the focus leaving it, reveals widening,
Reveal Active File in File Tree; a window brought back from its record,
also focused on a folder gone since; the folder's session after closing
and opening again, also with the folder renamed meanwhile; the focused
folder deleted on disk, with the status note). The bar's pictures were
looked at (`MC_GUI_SNAPSHOTS`). Needs a person at the ThinkPad: the bar's
look under GNOME, hover and the pointer (`BUILD-LINUX.md`, "How it
looks").

### 95. Merge conflicts in the editor and on the review page (done, November 2026)

The Mac's feature of 2026-11-16 (`../docs/notes/git.md`, "Merge
conflicts"; `mac/src/EditorConflicts.mm`, the conflict parts of
`mac/src/ChangeReviewPage.mm`), over the same core: `MergeConflicts.h`
(the blocks, each choice as one splice, `followEdit`, `step`, the words)
and `ChangeReview::conflictFile`, `conflictResult`, `canMarkResolved`.

- **The editor** (`EditorConflicts.{h,cpp}`, owned by the `Editor` beside
  the folds): two drawing passes of the `CodeView`, the tints below the
  text (order 2, under the carets' selection and the bracket boxes) in
  `editor.merge-current` and `editor.merge-incoming`, the markers' lines at
  two and a half times the alpha, a diff3 ancestor gray, each a rect
  across the visible width from its first line's top to its last line's
  bottom (`gtk_text_view_get_line_yrange`, so a folded line takes
  nothing); the buttons above the text (order 30), drawn rounded rects
  with Pango text in the view's font made Sans, medium, 85%, placed after
  the `<<<<<<<` line's end (its iter location) or pinned left of the
  visible right edge, then "Conflict 2 of 3" muted. A click gesture in the
  capture phase claims a press on a button (no modifiers), as the folds'
  placeholders do; the rects are worked out again from the visible rect
  for the press, so nothing drawn is trusted stale. A choice reads the
  text again, resolves the block in UTF-8 (`MergeConflicts::resolve` on
  the buffer's slice) and puts it in through `MultiEdit::apply` named
  "Keep Current Change" (one undo step; the highlighter, the server and
  the change bars follow it as any edit); `onResolved` has main.cpp
  flash `resolvedNote` in the status bar.
- **Characters, not bytes.** The blocks are parsed from the buffer's
  UTF-8 slice (`gtk_text_buffer_get_slice`, so a child anchor is one
  character) and their offsets, all line starts, made characters in one
  pass (`toCharacters`). The buffer's `insert-text` and `delete-range`,
  connected before the default handlers, give `followEdit` the edit in
  the text as it was; an edit touching a block drops it at once, and the
  text is read again 0.2 s after the last edit when a block was there,
  something was removed, or the text typed holds `<|=>`. `notify::buffer`
  and `notify::editable` read it on idle. Read only while `reads()`: the
  active document's editable source with a path, not loading, not
  large-file mode with its features off (the change bars' conditions,
  without their setting), at most 20 MB.
- **Next and Previous Change**: the change bars have none for a conflicted
  file (`ls-files --stage` has no stage-0 entry, so `parseIndexEntry`
  fails and the original is never read), so `GitGutter::goToNextChange`
  asks the conflicts first, and `updateActions` enables `nextchange` and
  `previouschange` while there are any (`onBlocksChanged`), with
  `editor.git-gutter` off too.
- **The review page** already laid conflicted files out with
  `conflictFile` and showed Open on them. Now a block's first row has Keep
  Current, Keep Incoming and Keep Both (its buttons go on their own row
  above it, as every row's do on this page), each part has a tag of its
  own (`conflict-current-marker` ... `conflict-incoming-marker`, colored
  in `applySettings` from the `mergeColor` hook; the separator untinted,
  as on the Mac), and the header has Side by Side (`resolvedAll` current
  against incoming, no path, so neither pane is the file on disk), Open
  and, with no block left, Mark Resolved (`stageFile` with "Marked x
  resolved.", no question). A choice runs on the panel's git thread:
  `readFileStable` must give the bytes the page read (`Extra::workBytes`),
  else `staleText`; then `writeFileAtomically` with the file's encoding
  and mark; a tab with unsaved text for the file refuses first. A tab open
  on the file follows the disk.
- **Left to the Mac**: the review page's right click (and the Keep
  choices in it). The buttons in the editor are drawn, not widgets, so
  they have no hover light or tooltip (the Mac's have tooltips from
  `MergeConflicts::tooltip`); the pointer over them is the text cursor.

Checked by `git_conflicts` (`tests/gui/GitConflictsSuite.cpp`): a real
diff3 merge in a scratch repository; the blocks and labels, the buttons
in a row after the marker's text on its line, the four tints read as
pixels against the plain background, Alt+F5 and Shift+Alt+F5 stepping
and wrapping with no bars, a letter typed in a block and the block found
again, the separator deleted (dropped at once, still gone once read
again), a real click on Keep Current (one step, "Keep Current Change",
the status bar's "2 conflicts left in c.txt."), undone, Keep Incoming,
Keep Both, the save; on the review page the rows, their tints and
buttons, Side by Side's two texts and labels, Keep Incoming and Keep
Both writing the file, and Mark Resolved staging r.txt and c.txt until
`ls-files -u` is empty. The editor's and the page's pictures were looked
at (`MC_GUI_SNAPSHOTS`). Needs a person at the ThinkPad: the look of the
tints and buttons under GNOME and at 2x, a real merge from the terminal.
### 96. Run one test: the ▶ beside each test (done, November 2026)

The Mac's feature of 2026-11-16 (C5; `../docs/notes/terminal.md`, "Running
one test"; `mac/src/TestGutter.mm`), over the same core, `Runnables.h`,
which finds pytest, go test, cargo test, Jest and Vitest tests without a
server and builds the command for one test or the whole file from
test.<language> or its built-in template. Nothing of its rules is in the
port.

- **The marks live with the buffer.** `BufferLines` keeps them beside the
  change bars (`setTests(lane, marks)`, `GutterTest`, `TestState`): whether
  the buffer is a test file (the lane) and a mark a test, moved by each
  insert and delete as the Mac's gutter moves them (`shiftTests`: a mark
  below the edit moves with its line, its own line too when the edit began
  at that line's start; one in lines the edit took stays on its first
  line), cleared by a whole new text. So a tab shown again has its lane and
  marks at once, and the gutter never shows one file's marks beside
  another's text.
- **The lane** (`Gutter`, `kTestLane` 14 px, leftmost): drawn while the
  shown buffer's `testLane()`, the dots and the change bars (their rects
  and the click's range) moved right by it, the width measured again when
  it comes or goes (`updateTestLane`). A ▶ in the numbers' color, a ring in
  the active color while running, VS Code's check (#73C991) and cross
  (#F14C4C), as the Mac draws them; `drawnRows()`'s `test` says which. A
  press on a mark calls `onTest` instead of selecting lines, after giving
  the view the keyboard; the tooltip comes from `query-tooltip`, looked up
  by the line under the pointer (`testTipAt`).
- **`TestGutter`** (one per window, made beside `GitGutter`) follows the
  view's buffer and its edits as GitGutter does: a look 50 ms after a tab
  switch or a settings change, 0.3 s after typing pauses, `Runnables::find`
  on a one-thread pool, the answer dropped when anything happened
  meanwhile. Results are kept by file and test label until the file on
  show is edited. Run Test at Cursor is `win.runtestatcursor`, enabled
  only while the file on show is a test file with tests, so the palette
  and the Terminal menu offer it only there.
- **Running** goes through `runFileCommand` in main.cpp, now Run File's
  too (the Mac's `runFileCommand:inDirectory:busyNote:`): save first (a
  failed save runs nothing), a new tab when the terminal on show is busy
  (the test's note in the status bar), the keyboard left where it was, and
  `cd <project> && ` first unless the shell's OSC 7 folder is already the
  project's. `TestGutter` knows nothing of `Terminal` (a build may have
  none): the window hands back where the line went (`Typed`: the
  terminal's control id, its shell's host terminal id, and the
  integration's count of commands started when it was typed) and, from
  each terminal's `onCommandFinished`, what finished (`Finished`: the
  same ids, the count of commands done, 133;D's status, and the line).
- **Matching a finished command to its run.** The host clears a command's
  line when it ends, and a short command's start and end reach the window
  in one report, so the line is seldom known by then: a run is the
  command its shell started next after it was typed, `started + 1`, in
  that terminal and that shell (a restarted shell is another host
  terminal: the run is forgotten). When the shell's done count reaches it
  and it was the last to end, its status decides; when the window also
  saw its line (`Terminal::finishedCommand`, new) that line must hold the
  run's. Anything less certain (several ended in one report, another
  line) puts the marks back to ▶, never a wrong check or cross. A
  terminal closed with a run in it does the same (the dock's `onChanged`,
  now set, calls `terminalsChanged`).

Checked by the new `test_gutter` suite, a bash terminal of its own with a
stand-in `pytest` first on its PATH (`EnvLock::envSet`, put back after)
and `test.python = pytest {tests}`: the lane and a mark on each test's
line, tooltips, real clicks on the marks, the command typed and run (read
from what the stand-in wrote down and from the marks' counts, never off
the screen), the check and the cross by exit status, the keyboard left in
the editor, Run Test at Cursor in a test's body and above the first test
(the whole file, saved first), an edit moving the marks and clearing the
results, a busy terminal never typed into (a new tab took the run, with
the note), a file that is no test file without a lane, and the results
kept across a tab switch. Pictures of the lane were looked at
(`MC_GUI_SNAPSHOTS`). Needs a person at the ThinkPad: the marks' look and
the tooltip under GNOME, and real pytest, go, cargo and vitest projects.

### 97. minicode is renamed mutible (done, November 2026; 1.6.0)

The items above keep the names they were written with. From 1.6.0 the
binary is `mutible`, with `mbl` and `minicode` installed beside it as
links (run as `minicode`, it says it is now mutible), the app id is
`ai.ible.Mutible` (desktop file and icons named for it), the shared files
are under `$datadir/mutible`, the Meson project is `mutible-gtk` and the
checks are `mutible-guitest`, `mutible-linux-tests` and
`mutible-latex-sweep`. Settings, state and data move to the XDG folders
named mutible: the first launch copies minicode's when mutible's are
missing and leaves the old ones (`src/Rename.h`, `linuxItems`), and
`MINICODE_*` variables are still read where the `MUTIBLE_*` name is unset.

The Debian source package is `mutible` (`debian/`), still uploaded to
`ppa:echansen/minicode`, since Launchpad cannot rename a PPA. It builds
two binaries: `mutible`, with `Breaks` and `Replaces` on `minicode (<<
1.6.0~)` so it takes over `/usr/bin/minicode` from an installed one, and
`minicode`, a transitional package (all, oldlibs) that depends on mutible
and holds only its docs, so `apt upgrade` moves PPA users over.
`scripts/ppa.sh` refuses releases before 1.6.0 and adds the committed
changelog's 1.6.0 entry to that upload's; `scripts/ppa-wait.sh` polls
source `mutible`, or `minicode` for an older upload; the key id file is
`~/.config/mutible/ppa-key-id`, the old one still read. BUILD-LINUX.md,
"Renamed from minicode in 1.6.0", says what a user sees.

Needs a person at the ThinkPad: `apt upgrade` from an installed minicode
1.5.x once 1.6.0 is on the PPA (minicode becomes the transitional package,
mutible comes in, `/usr/bin/minicode` stays), the first launch's copy of
real settings and sessions, and pinning mutible to the dock in place of
the old icon.

### 98. Add to Focus: several folders, each a root of the tree (done, November 2026)

The Mac's feature of 2026-11-18 (`../docs/notes/tree.md`, "Add to Focus,
several folders"; PRs 157 and 169; `mac/src/EditorController+TreeFocus.mm`
and `TreeFocusBar.mm`), over the same core: `FileDetails.h`'s
`TreeFocus` set rules (`added`, `removed`, `merged`, `rootOf`,
`revealed`, `restoredSet`, `shortLabels`, `shared`) and the list form of
`TabSessions::Session.treeFocus`. Item 94's single focus is the set of
one, breadcrumb and widening unchanged. PARITY.md 4.11.

- **The set.** `FileTree::foci_` is the focused folders in the order
  added; `setFocusFolders` takes a list through `TreeFocus::merged` and
  refuses one that is not `inTree`; `setFocus` (one folder or none),
  `addFocus` and `removeFocus` go through it. `focusPath()` joins the set
  with ", " (the Mac's `treeFocus`), `top()` is the first folder (New
  File, Copy Path, Open Containing Folder with nothing selected).
- **Roots.** With several, `makeTreeModel` puts a plain `GListStore` of
  one `GFileInfo` per folder (type directory, its `standard::file`, a
  `minicode-root-label` with its path) at the top (`makeRootsModel`); each
  row's children are the usual watched folder model, so listings, the
  dotfile filter, git colors and drops into a root need nothing new.
  `onBind` shows the label and the `minicode-tree-root` class (a little
  heavier). A drop below the roots is refused (`dropDirAt` gives "").
  `applyFocus(set, lost)` keeps the Mac's rules for what is open: memory
  outside the old tops kept, rows open now added, widening opens the old
  root and the folders down to it (and selects it when one folder was
  focused), and a root new to several, or the folder that was the single
  focus, opens. `showsPath` asks `rootOf` with several and reveals
  through `TreeFocus::revealed`, so a reveal outside every root adds its
  folder. `watchFocusPath` watches the folders from the window's down to
  every root's parent; `dropLostFocus` keeps the roots still `inTree` and
  says one of the Mac's three notes ("A focused folder is gone: the tree
  shows the others", or the whole folder's two).
- **The label row** reuses the breadcrumb's bar and `GtkCustomLayout`:
  one chip per folder (`minicode-tree-focus-chip`: a label, a flat ×),
  laid out with the full paths when their widths (measured with Pango for
  either text, whatever the label shows) fit, else `shortLabels`, each
  capped by `shared`. The texts change from an idle (`setBarShort`),
  never inside the allocate function. A press on a label selects its root
  row (`selectRoot`); a chip's × removes its folder from an idle, since
  the bar is made again without that button. A + (`onAddFolder`) sits
  before the last × on the breadcrumb and the label row alike.
- **The window** (`main.cpp`, "Focus on Folder"): `win.addtofocus` (the
  File menu and the palette, `mutible.tree.addToFocus`: the selected
  row's folder, else the open file's, when not shown already; else the
  chooser, as the Mac opens its picker), `win.addtofocusrow` and
  `win.removefromfocusrow` in the tree's right click (hidden while they
  cannot act, `updateTreeFocusActions`), and `win.addfoldertofocus`
  (`mutible.tree.addFolderToFocus`, Add Folder to Focus…, the bar's +).
  That one is the system's folder chooser (`chooseFolder`, titled "Add
  Folder to Focus"), started in the window's folder: the GTK port has no
  palette folder picker like the Mac's `MCFolderPick`, so the open tabs'
  folders are not offered first and nothing completes a typed path. The
  answer is held to the window's folder by the core's
  `FolderPicker::inScope` (strictly inside: the folder itself is refused
  too), with the Mac's "Only folders inside …" (`FolderPicker::
  abbreviate`); `FolderPicker.cpp` joins the Linux build lists for it.
  Persistence's `treeFocus` and `restoreTreeFocus` hooks carry the list.

Checked: the `tree_focus` suite's second half (`./build/mutible-guitest
tree_focus`), run three times: Add to Focus in a folder's right click
(not Remove from Focus) and from the menu bar, the + asking the chooser
(answered through `MCGuiAnswerFolderChooser`, started in the window's
folder) and giving two roots in the order added, both open, labelled with
their paths, the label row and the session saying both; a file made in a
root; a folder already shown adding nothing; a reveal adding its folder
as a third root; the labels fitting the bar (short ones at the sidebar's
width); Remove from Focus on a root row's right click; a label's click
selecting its root row and its × taking it out, back to the breadcrumb;
the chooser's answer outside the window's folder and the folder itself
refused with the note; Add to Focus with nothing new asking the chooser;
the last ×; a window's record with a folder gone since between two;
the folder's session bringing both back; one of two deleted on disk
dropping out alone. Pictures of the label row with two and three
folders were looked at (`MC_GUI_SNAPSHOTS`). Needs a person at the
ThinkPad: the label row's look under GNOME, real clicks on the chips'
× at the sidebar's narrowest, and the portal's folder chooser opening in
the window's folder.

### 99. Windows attach to the folder's headless session host (stage 6, step 1; November 2026)

The first step of stage 6's GTK half (`../docs/notes/session-host.md`, "GTK
attaches (stage 6, step 1)", which has the details and step 2's list).
`SessionLink` is one `SessionLink::Link` per folder now, each with its own
client, instead of one process-wide client and in-process host.

- **One link per folder** (`SessionLink::forFolder`, by resolved path),
  shared by the windows on it. The window holds it (`App::link`, set in
  `newWindow` before the editor and again in `openFolder`), the editor
  opens new documents on it (`Editor(link)`, `setSessionLink`), and each
  `Document` and `Terminal` keeps the link it was made with; the LSP sync
  follows each document's host text through its own link. The
  process-wide `SessionLink::client()` is gone.
- **Where the host is**: `MUTIBLE_HOST=socket|inprocess|loopback`, else
  `editor.session-host = headless` (a third value of the key; off and on
  both mean in-process here) for the headless host, which is found by its
  socket or started as `/proc/self/exe host --folder DIR`, detached
  (setsid, a second fork, `/dev/null` and a log in the state folder), and
  waited for up to 5 s. Anything failing: the in-process host for that
  folder, with one line on stderr. The default stays in-process until
  step 2.
- **The host going** is said once on stderr, in the status bar and in each
  of its terminals; the client then waits for nothing
  (`Client::hostLost`), so undo and the like neither hang nor time out.
  Files stay open and save; their undo is gone; terminals end. No
  reconnect yet.
- **Quitting** detaches (`SessionLink::detachAll`); closing a window is as
  before (documents closed on the host, terminals asked about and ended).
- **Checks**: the harness pins `MUTIBLE_HOST=inprocess` (Wrapper.cpp) and
  takes the CLI subcommands in its own `main`, since the host a window
  starts is `mutible-guitest host`. `host_link` (the notes say what it
  checks); `hostserver:host-gone` and the settings checks in `make test`.
### 100. The mutible desktop: mutible as the whole desktop under cage (done, November 2026)

`desktop/README.md` is the guide; `desktop/mutible-desktop-setup.sh` turns
a minimal Ubuntu 26.04 into it and undoes it; `src/DesktopMode.{h,cpp}` is
the app's side (`MUTIBLE_DESKTOP=1`), with hooks in `main.cpp`'s
initShared, newWindow, onCloseRequest, onWindowRemoved and the shutdown
handler; the `desktop_mode` GUI suite checks it with logind replaced.

How it runs: a system service, not getty autologin plus `~/.profile`.
`mutible-desktop.service` has `User=`, `PAMName=mutible-desktop` and
`TTYPath=/dev/tty1`, which make pam_systemd register a logind session on
seat0, vt 1, type wayland: the same active local session a console login
makes, so devices come through logind (cage uses libseat's logind backend)
and polkit's allow_active applies. It conflicts with getty@tty1, which the
setup disables. Checked in a VM (QEMU with KVM, std VGA, `-display none`,
screendumps over QMP): the window full screen with no title bar; Ctrl+Alt+F2
to a login prompt and Ctrl+Alt+F1 back (cage's `-s`); GTK's own file chooser
(no portal installed; `GDK_DEBUG=no-portals` keeps it so); Suspend, Restart,
Power Off and Log Out from the menu (the actions activated over D-Bus with
`busctl --user call ai.ible.Mutible ... org.gtk.Actions Activate`, the
questions answered with QMP sendkey), `systemctl suspend` typed in
mutible's terminal with no sudo (polkit allowed it with the same user also
logged in over SSH), PipeWire listing the emulated sound card, and `--undo`
leaving tty1's login after a reboot.

Traps:
- `OnFailure=getty@tty1.service` fired on every exit of a restarting unit
  (systemd 259), and the getty it started, which conflicts with the unit,
  reset the start limit: seven kills in 30 s and it kept restarting. The
  session script counts exits within 30 s of starting itself (in
  `$RUNTIME_DIRECTORY`, kept over restarts) and after three gives tty1 a
  text shell; the start limit is only a backstop, with no OnFailure.
- `mutible` must run with `--foreground` under cage: the service gives it
  tty1 as its controlling terminal, so it would detach (Detach.h), its
  first process would exit, and cage would end the session.
- `apt-cache policy mutible | grep -q` under `set -o pipefail` failed when
  grep stopped reading early: read the output whole first.
- Ubuntu's brightnessctl has neither logind support nor a udev rule; the
  setup adds brightnessctl's upstream rule and the video group.
- QEMU's `-vga virtio` with `-display none` gave screendumps of the boot
  console while GNOME was up; `-vga std` showed the screen.

Measured in VMs (the table in the guide): about 500 MB used against 1,010
with GNOME, and mutible's first frame 9 s after power on against 19.

Left: volume, brightness and media keys (cage binds none), a battery,
Wi-Fi and clock line in the status bar, screen locking and blanking, a way
to switch between several windows (cage shows the newest), and everything
on real hardware: Wi-Fi with nmtui, the lid, the power key, the backlight
rule, suspend and resume with a real GPU, and how long a cheap laptop takes.


### 101. Closing a window detaches, and the next window reattaches (stage 6, step 2; November 2026)

The second step of stage 6's GTK half; `../docs/notes/session-host.md`,
"GTK reattaches (stage 6, step 2)", has the details and what is left.

- **What a user meets.** With `editor.session-host = headless`, closing a
  window (or quitting) leaves its terminals running on the folder's host,
  and the close question has a fourth answer, Keep Unsaved, which leaves
  unsaved files there; the folder's next window, in this app or after a
  relaunch, gets its tabs back (the unsaved text, unsaved; the host's undo
  history) and its terminals attached (the same shell, what it printed,
  live output). In process (the default) the same happens for saved files
  and terminals while the app runs. When the host goes, Reconnect in the
  status bar (or the next window on the folder) finds or starts it again
  and reopens every window's files on it from their buffers.
- **Code.** Core: `DocPath`, `DocDetach`, `TermDetach` (SessionProtocol.h,
  additive), the state's `connected` and `seen` for windows,
  `Client::setPath`, `detach`, `detachTerminal`, `clientId`,
  `showsTerminal`, `Shared::termLooksLikeAnswer`. GTK: `SessionLink`
  (`takeForWindow`, `windowDetached`, `windowTerminals`,
  `keepWhileHolding`, `endDetached`, `folderLost`), `Document`
  (`attachHeld`, `detachFromHost`, `rehost`, the path), `Editor`
  (`attachTab`, `detachFromHost`, `rehost`), `Terminal` (`attachHeld`,
  `detachFromHost`, `rehost`, answers, `followPtySize`), `Persistence`
  (a held path's backup removed), main.cpp (`detachWindow`, the question,
  Reconnect, `rehostWindows`).
- **Checks**: `host_reattach` (and `_host_reattach_child`), `host:detach`,
  `host:window-records`, `host:term-answers`. The harness ends what closed
  windows left on their hosts between suites (`SessionLink::endDetached`).


### 102. Latin-1, Windows-1252 and UTF-16 files open as text (C3b; November 2026)

The Mac's #176 on GTK; `../docs/notes/sessions.md` has the rules.

- **What a user meets.** A file that is not UTF-8 and holds no zero byte,
  such as an old Latin-1 or Windows-1252 file, opens as text, read as
  Windows-1252 by the core's rule (`FolderSearch::readsAsWindows1252`, the
  one Find in Folder searches by), where it had said "Cannot display";
  UTF-16 with its byte order mark opens as text too, and a UTF-8 file's
  mark is kept apart instead of sitting in the buffer as U+FEFF. Each is
  saved in its own encoding, byte for byte when nothing changed. The
  status bar names any encoding but plain UTF-8 ("Windows-1252", "UTF-16
  LE", "UTF-16 BE", "UTF-8 with BOM"), left of the problem counts. A
  character Windows-1252 cannot store (an arrow, an emoji) has the file
  saved as UTF-8, and the status bar says so ("Saved notes.txt as UTF-8:
  Windows-1252 cannot store “→”"); auto-save says it through its note. A
  search hit in such a file opens it at its line; Replace in Folder still
  leaves it alone, and now says why when a row's Replace reaches one.
- **Code.** `Document::readText` decodes through `decodeFileText` and
  gives the encoding (`Document::FileEncoding`); the document keeps it
  from `loadText`, a reload, the background load (LargeFileView.cpp),
  `attachHeld` and a restored backup (Persistence, from the disk, else the
  backup's `file-encoding`, which `writeBackup` now records). `save`
  writes `encodeFileText`, or UTF-8 when `canEncode` says no, with
  `takeSaveNote`; `encodingLabel` feeds main.cpp's label. The git gutter
  reads the index's blob in the file's encoding (`readOriginal`).
- **Checks**: `latin1_files` (and its `_latin1_restore` child, which
  restores a backup of a file gone since and saves it in Windows-1252).
- **Left**: the GTK port has no Save As or encoding picker (neither has
  the Mac); a file changed on disk to the same text in another encoding
  is followed only while its buffer is clean.

### 103. The plan usage forecast in the Agents panel (done, November 2026)

The Mac's A13 of 2026-11-18 (PR 178; `../docs/notes/agents.md`, "The plan
usage forecast"; `mac/src/UsageView.mm` and `StatusLineOffer.mm`), over
the same core: `ClaudeUsage` (the transcripts' tokens, prices, blocks,
burn rate, the status line's samples, the fit, `describe`'s lines) and
`ClaudeStatusLine` (`planInstall`, `install`, `planUninstall`,
`uninstall`, the record, the log). The CLI's `mutible statusline` was
already in the Linux build; this adds the panel's side.

- **What a user meets.** Under the Agents panel's heading, PLAN USAGE:
  the 5-hour window, the week, the burn rate, any unseen usage (the fit's
  line is the text's tooltip), and "Log plan usage from Claude Code's
  status line…", which becomes "Stop logging plan usage…" once it is in.
  Before the transcripts are read it says "Reading Claude Code's
  transcripts…"; without readings each window says "no reading yet".
- **The model** (`UsageView.{h,cpp}`, `UsageModel`): one per app, never
  freed. A `GThreadPool` of one thread keeps the `Scanner` and `Tally`
  and reads the transcripts incrementally (`AgentModel::
  defaultProjectsDir`, 8 days back) and the log (`StatusLineOffer::
  usageDir`: `$MUTIBLE_USAGE_DIR`, else `$XDG_STATE_HOME/mutible/usage`);
  the lines come back with `g_idle_add`. Where to look is read on the
  main loop. `refresh` skips a look within 5 s of the last;
  `refreshThen` forces one (the checks, and after an install or undo).
  The panel's `rebuild` refreshes it while the panel shows (its model's
  changes and the 30 s timer), as the Mac's `reload` does.
- **The view** (`UsageView`): a box in `AgentsPanel` between the heading
  and the notice or sessions; the text a wrapping, selectable label (not
  a Tab stop) in the sidebar's text at 0.8 (`minicode-usage-text`), the
  action a flat button whose label is the accent, ellipsized
  (`minicode-usage-action`, ThemeCss.cpp). Its click goes through a
  shared handle the view clears when it goes, so a question still up
  when a window closes answers into nothing.
- **Install and undo** (`StatusLineOffer.{h,cpp}`): the Mac's wording and
  steps through `Question`, as the hooks offer (item 83) asks: the
  question names the settings file, the command and the usage folder,
  with the `statusLine` JSON in the preview box; Log Plan Usage or
  Cancel; the core reads and plans again, backs the file up and writes
  it, then a notice says where (and where the backup is). Undo asks (Put
  It Back, Cancel) and says whether the file is exactly as before. The
  command is `mutible statusline` when `mutible` is on the PATH, else
  this program by its full path (`/proc/self/exe`), as the hooks' is;
  but the core knows the wrapper by its program's name, so a copy named
  otherwise (`mutible-guitest`) names the `mutible` beside it.
- **Checks**: the `plan_usage` suite (a scratch CLAUDE_CONFIG_DIR,
  transcripts folder and usage folder of its own, checked to be under the
  harness's scratch root before anything is written): "no reading yet",
  then the fitted forecast's lines and the fit's tooltip, the section
  above what follows it, Cancel writing nothing, Log Plan Usage over an
  existing `statusLine` (the rest kept, one backup, the record in the
  usage folder), the built `mutible statusline` logging a reading and
  running the old command with the same stdin, Put It Back restoring the
  file byte for byte, and a settings file that is a symbolic link staying
  one through install and undo, its target written and restored.
- **Left**: the look of the section in a narrow sidebar on a real screen,
  and a real Claude Code session with the wrapper installed (the readings
  arrive after a reply), as on the Mac.

### 104. The headless host keeps backups, untitled buffers come back, one path twice, VTE pans (stage 6, step 3; November 2026)

The host-side items left before `editor.session-host = headless` can be
the default (`../docs/notes/session-host.md`, "The host's own backups,
untitled buffers, one path twice, panning (stage 6, step 3)").

- **What a user meets.** A file left unsaved on the folder's headless
  host (Keep Unsaved) is in the backups folder at once, held by the
  host's process; a crash, `kill -9` or a reboot of the host loses
  nothing: the folder's next window restores it as an unsaved tab. While
  the host lives, the next window gets the host's document as before and
  nothing is restored twice. A buffer with no file that another renderer
  left on the host comes back as the tab of `Untitled-1` (not on disk
  until saved). Two windows on one file, both closed unsaved: the next
  window shows the last one's text, the window after it the other's. A
  terminal another renderer made larger follows the cursor.
- **Code.** Core: `Host::keepBackups`, `writeBackupsNow`, `backupId`,
  `backupPath`, `SessionHost::keepBackups`, `HostOptions::backupDir`
  (`mutible host` sets it from `Backups::defaultDir`), the Hello's flag 4
  (`Client::setKeepsBackups`), `busyReason` counting an untitled buffer
  with text, `Backups::defaultDir`, `processStart`, `newId`,
  `untitledPath`, `Shared::reattachFrom`. GTK: `SessionLink` (the flag,
  `free` over `reattachFrom`, its Held types now the core's), main.cpp
  (`attachHeldDocuments` naming untitled buffers, `backUpHeldDocuments`),
  `Editor::attachTab(..., hostNamed)`, `Editor::saveDocument` telling the
  host a new path, `Document::attachHeld(..., hostNamed)`,
  `hostSavedFromDisk` for a file not on disk, `Terminal::panToCursor`.
  The top-level Makefile's CLI links Backups.cpp and SavePlan.cpp.
- **Checks**: `host_backups`, `host_pan`, and the `hostbackup:*` and
  `reattach:*` groups of `make test`.

### 105. VTE in a frame of its own: no warnings while it pans, and a smaller pty's wrap (stage 6, step 3; November 2026)

Two of item 104's leftovers (`../docs/notes/session-host.md`, "VTE as a
renderer among several" and "VTE pans").

- **What a user meets.** A terminal another renderer (the browser's
  view, a guest) made smaller shows the program's screen at the pty's
  own width and height, at the top left of the panel, the rest of the
  panel the terminal's background: a long line with no cursor
  positioning wraps where it wraps for the renderer that typed, not at
  the panel's width. Typing in the panel takes the pty back to the
  panel's size, as before. A larger one pans as in item 104, and GTK no
  longer prints a warning on every pan.
- **Code.** `src/TerminalFrame.{h,cpp}` (new): a GtkWidget implementing
  GtkScrollable, the scrolled window's child in VTE's place, passing the
  adjustments and scroll policies through to VTE (the scroll bar, the
  wheel and the scrollback stay VTE's); it lays VTE out at its own size,
  or at the size and offset it is held at, clips it, paints the
  terminal's background where VTE does not reach, and gives VTE the
  keyboard on a press there. `Terminal`: `followPtySize` holds VTE at
  any size of another renderer's (larger or smaller), `measureCells`
  (only while mapped), `viewCells` from the frame, `placeGrid` (was
  `applyPan` and its negative margins), `releasePtySize`; `sendSize`
  sends the frame's cells whenever VTE's cell is known (VTE's grid is
  still the held one for a layout after a release, and sent then it took
  the pty back to the other renderer's size); `char-size-changed` keeps
  the cell and lays a held grid out in the new cells; a resize of
  the view pans again; `sentSizes_` keeps the sizes still on their way
  (up to 32, those before the latest dropped once it comes back);
  `clipped()` is `atPtySize()`. The harness: `MCGuiLogWatchStart` and
  `MCGuiLogWatchStop`, GLib's one log writer, keeping every warning and
  critical while on.
- **Checks**: `host_pan` (VTE laid out up in its frame by the rows
  panned, no margins, the scroll bar VTE's adjustment, no Gtk-WARNING or
  Gtk-CRITICAL from the other renderer's attach to the pty coming back)
  and `host_wrap` (new; its header says what it checks).
- **Left for a real screen**: the empty part's look beside a translucent
  terminal color, trackpad scrolling over it, and a hidden terminal
  shown while another renderer's size is in force (it follows at the
  host's next TermShell, as before).

### 106. Terminal splits come back as splits (stage 6; November 2026)

The last terminal item of "Left before the default flips"
(`../docs/notes/session-host.md`, "Terminal splits in the record").

- **What a user meets.** A window closed with split terminals (Split
  Terminal, Split Terminal Down) and the folder's next window, in this app
  or after a relaunch, has the same terminal tabs with the same panes in
  the same columns and rows, the boundaries where they were, each tab's
  pane with the keyboard and the tab on show as they were. A shell that
  ended while no window showed it leaves its pane out, its neighbours
  where they were. A record an older app left (or a terminal no record
  names) still comes back as a tab each.
- **Code.** Core: `src/TermDockRecord.{h,cpp}` (`Shared::TermDock`,
  `termLayoutJson`, `termDockFrom`, `prune`): the record's `termLayout`
  beside `terms`, written and read by both apps; built into the top-level
  Makefile's app and tests, linux/Makefile and meson.build (not the Mac's
  CLI: nothing of the host reads it). GTK: `reportTerminals` (main.cpp)
  writes it from the dock's groups; `Link::windowTerminals` takes it;
  `takeHeldTerminals` orders the held terminals as the dock makes them
  back, `buildHeldTerminals` makes the tabs (`TerminalDock::restoreTab`,
  `TerminalGroup::append` and `setShares`, no size rule); the dock tells a
  moved boundary (`TerminalGroup::onSharesChanged`, the dock's
  `onSharesChanged` 300 ms after the last move); `focusPane(t, false)`
  shows a tab's pane without the keyboard and reports a pane made active.
  **Fixed on the way:** with the keyboard in the dock, a tab switch gave
  it to the tab's first pane (GtkStack's `gtk_widget_child_focus`), whose
  focus-in made it the tab's active pane; `TerminalDock::activate` now
  gives it to the tab's active pane and ignores focus-ins meanwhile.
- **Checks**: `host_splits` (HostSplitsSuite.cpp; its header says what it
  checks) and the `termdock:*` groups of `make test`.

### 107. The tree follows the active tab, centered (explorer.auto-reveal; November 2026)

The owner's request ("I have to hunt for the file that I am already
using"), the Mac's #197 (`../docs/notes/tree.md`, "The tree follows the
active tab").

- **What a user meets.** Whenever another tab comes on show (a click on
  its tab, Alt+1 to 9, the next or previous tab, a click into the other
  editor group, closing the tab before it, Quick Open, a definition, a
  search result), the tree opens the file's folders, selects its row and
  scrolls it to the middle; a row already wholly in view is selected and
  the tree stays where it is. The editor keeps the keyboard. A file
  outside the focused folders leaves the tree alone (only Reveal Active
  File in File Tree widens). Pages, comparisons, untitled tabs and files
  outside the window's folder are not revealed. With the sidebar closed or
  Source Control, Processes or Agents in the tree's place nothing moves,
  and the file on show is revealed once when the tree comes back.
  `explorer.auto-reveal = off` stops it (the row is still selected when it
  is listed already); turned on again, the file on show is revealed at
  once, as on the Mac. Reveal Active File and Quick Open center the same
  way, Quick Open whatever the setting. A window brought back at launch
  has its tree's open folders, selected row and scroll as it was left,
  with no jump to its file (new: the record kept none before), and
  opening a folder leaves its tree as it opens, as the Mac holds it.
- **Code.** `src/TreeFollow.{h,cpp}` (new): the Mac's
  `EditorController+TreeReveal.mm` rules over three hooks (the active
  tab's file the tree can show, whether the tree is on show, the setting)
  and the core's `TreeReveal::Follower`; `activeChanged`,
  `treeMaybeShown`, `hold`, `settingsApplied`. `FileTree`: `revealPath`
  selects without `gtk_list_view_scroll_to` and `centerRow` sets the
  scroll from `TreeReveal::scrollTop` in the frame clock's after-paint
  (`scheduleFrame`, `afterLayout`; a row the list has not made is placed
  from one it has, the rows being of one height); `reveal(path, widen,
  center)` keeps the edge scroll for the tree's own reveals;
  `selectIfListed`; `opening()` around the open callback in `actOnRow`;
  `openFolders`, `scrolled` and `restoreView` (the record's folders
  opened as their listings come, those outside the focus remembered, the
  selection and scroll put back once the rows stop coming for 150 ms,
  3 s at most); `showsPath` through the core's `TreeFocus::shows`.
  `main.cpp`: `treeFollowablePath`, `treeOnShow` (the sidebar shown, the
  tree the stack's child, the divider above 0), `onActiveChanged` calls
  the follower (`revealActiveInTree` is gone), `layout->onChanged`, the
  stack's `notify::visible-child` and the divider's `notify::position`
  call `treeMaybeShown`, `applyWindowSettings` calls `settingsApplied`,
  the hold is around `attachHeldDocuments` to `showHeldActive` in
  `newWindow` and `openFolder`, the record's `windowState` writes
  `expanded`, `selected` and `treeScroll`, and `newWindow` calls
  `restoreView` with them. Settings text: the setting's comment no longer
  says macOS. REFERENCE.md: the paragraph is the Mac's and Linux's
  (Alt+1 to 9 on Linux), and a window brought back on Linux has its tree.
- **Differs from the Mac**: GtkListView brings the row a click focuses
  wholly into view by itself, so a click on a row cut by the edge nudges
  it in by the part cut off (the follower adds nothing; never centered).
- **Checks**: the `tree_focus` suite's last part (its header says what it
  checks), in a folder of 6,000 files. Its auto-reveal-off step was flaky
  (1 run in 5 beside `hints_panel`, 2026-11-19): it closed m_deep once
  Show Whole Folder's top rows were in, before the idle that opens the
  folders open before the focus (`FileTree::tryExpand`) had opened it, so
  the close did nothing and m_deep opened after it, down to near.txt. The
  step now waits for those folders to be open and checks the close; the
  product was right (`../docs/notes/tree.md`, the trap). `tree_clicks` went
  FLAKY in both CI runs from #232 on (2026-11-19, "timed out: closed",
  passing alone; 5 of 5 local runs in CI's order failed): its folder closed and stayed closed (a probe on the row's
  `expanded` saw one change, no reveal ran), but its wait found b.txt by
  name, and the tree, following Find in Folder's results, had opened
  `replace_in_folder`'s new rif4, whose b.txt the closing moved into view.
  The suite finds every row by its path now (`MCGuiTreeRow`); the product
  was right (the trap in `../docs/notes/tree.md`).
- **Left for a real screen**: the look of the centering and of the scroll
  jumping (no animation), and a sidebar dragged shut and open by hand.

### 108. The hints panel's search box, and the file's path in the title (November 2026)

The owner's two asks on the ThinkPad: "I cannot search within the help
panel like I can on mac mutible", and the file's path in the title while
the editor is selected.

- **What a user meets.** With the shortcut hints up (Ctrl+Shift+H), a box
  at the panel's top, "Type to search the shortcuts". Every key without
  Ctrl or Super, save a function key and the Menu key, types into it
  wherever the keyboard was (the editor, the tree, the terminal, a field),
  an Alt+letter as its letter, and no command or view sees it: Alt+Z
  types "z", it does not toggle Word Wrap. A shortcut matches when every
  word is found in its key or its words; the places found are marked (the
  Mac's orange behind the text) and the rest dimmed, a heading with
  nothing found under it too, never hidden, so the list keeps its layout
  and scrolls to the first mark. Keys go by name ("control" finds Ctrl,
  "opt" Alt, "return" Enter) and as combinations in any order
  ("shift+ctrl+h" finds Ctrl+Shift+H). Esc empties the box, then closes the
  panel, the keyboard back where it was; Return and Tab do nothing; a
  click puts the keyboard in the box; Ctrl keys run their commands (Ctrl+B,
  Ctrl+Shift+H closing the panel and emptying it), F1 still opens the
  palette, and the palette's typing stays its own. The keys are drawn as
  the core's `Commands::keyLabel(key, Platform::Linux)` draws them
  ("Ctrl+Shift+P", "Ctrl+⏎", "⎋"; `Keys::hintLabel`), the Mac's work
  (12d7a32c). The title follows the core's rule, the Mac's too
  (`src/WindowTitle.h`, #200): "src/main.cpp — mutible" (the path from the
  window's folder, "~/notes/todo.md" for a file in the home folder outside
  it, the full path outside both, "● " in front while unsaved, the folder's
  name with no tab) while a document pane (the editor, the tree, any pane
  but the terminal and the browser) has the keyboard, and the terminal's
  or the page's own title while one of those has it; it follows the
  keyboard between panes and the tab on show. The palette and the hints'
  box only borrow the keyboard, so the title stays as it was while they
  have it.
- **Code.** Core: none of its own; the search is `HintsSearch` with
  `Spelling::Linux` and the title `WindowTitle::title`, both the Mac
  session's. GTK:
  `linux/src/HintsSearchBox.{h,cpp}` (the GtkSearchEntry, `takeKey`,
  `giveBackKeyboard`, `attributes`: Pango background and foreground-alpha
  attributes on the label); main.cpp's `hintsRows` (was `hintsText`, the
  rows unchanged), `hintsText` (the same layout, each `HintRow` told where
  it landed), `showHintsText`, `hintsQueryChanged`, `setHintsShown`,
  `onHintsKey` (a key controller in the window's capture phase);
  `setWindowTitle` and `titleFocus`, run from `updateTitle` and
  `onFocusWidget`; `Keys::hintLabel`. The panel's columns are as they were
  (PageMeasure::layoutHints on GTK came next: item 111).
- **Traps.** An input method types nothing for an Alt+letter
  (`GDK_MODIFIER_INTENT_NO_TEXT_INPUT`): forwarded, Alt+Z was lost and the
  box held "s" (hints_panel failed so), so the box inserts the letter
  itself; other keys are forwarded to the entry's GtkText
  (`gtk_event_controller_key_forward`) after the box takes the keyboard,
  so input methods and Backspace work as in any field. `onHintsKey`, added
  to the window after GTK's own controllers, runs before the application's
  accelerators on GTK 4.22: with no accelerator unbound, Alt+Z still
  reached the box and Word Wrap stayed as it was (an unbinding like
  `updateShellKeys`'s was tried and taken out as needless); a GTK that ran
  them first would let Alt keys' commands run while the panel shows. The
  keyboard is given back before the panel hides, or GTK puts it wherever
  it likes.
- **Checks**: `hints_panel` (HintsPanelSuite.cpp) and `window_title`
  (WindowTitleSuite.cpp), their headers saying what they check; the
  core's `hints:*` and `window-title:*` groups of `make test`.

### 109. A row going keeps the tree's place; file icons by type (November 2026)

The owner, on this ThinkPad: "If I delete a file in the filetree panel,
rather than simply removing it from my current view, the view resets and I
am brought to the top of the filetree panel, which is annoying", and
"Almost none of the different file types I have encountered in the
filetree panel have non-default icons, which seems like a missed
opportunity." (`../docs/notes/tree.md`, "A row going keeps the tree's
place (GTK)" and "Icons by kind".)

- **What a user meets.** Move to Trash (the row's menu, or Delete with the
  tree's keyboard) takes out just that row: the tree stays scrolled where
  it was, every folder stays open, and the row after it (else the one
  before) is selected, with the keyboard when the tree had it, so Delete
  again trashes that one, as in VS Code. The file's tab closes and the tab
  before it comes on show without the tree going to that file. A selected
  file deleted or renamed on disk by something else does the same; files
  deleted or made above the view move nothing on show; Rename to a name
  that sorts in place keeps the scroll and selects the renamed row. File
  rows show the icon theme's picture for their type: a Python, C, C++,
  Rust, Go, Java, Kotlin, shell or Ruby file, Markdown, HTML, CSS, JSON,
  YAML, TOML, XML, CSV and SQL, LaTeX, a notebook, pictures, audio, video,
  PDF, archives, fonts, programs and libraries, office documents, a
  Makefile, CMakeLists.txt, meson.build, a Dockerfile, LICENSE, README,
  AUTHORS, lockfiles, git's files, dotfiles and config, patches, logs,
  keys and translations each have their own on Yaru (77 of the core's 78
  types draw apart from plain text, in 65 pictures). Changing the icon
  theme changes the rows on show at once.
- **Why it jumped.** Two causes. (1) `trashNow` closed the file's tab, the
  editor showed the tab before it, and the tree followed that tab
  (item 107's `TreeFollow`; before it `revealActiveInTree`), scrolling to
  its file: for a file near the top, the top of the tree. It also cleared
  the selection. (2) GtkListView (4.22) itself put the list at its top a
  frame or two after the row it was anchored on went (the row last
  clicked or focused): `gtk_list_base`'s `update_adjustments` takes 0 for
  an anchor it cannot place. A selected file deleted on disk did it with
  no tab involved. Found with a backtrace on the scroll's value-changed:
  the 0 came from `gtk_adjustment_configure` in the list's size-allocate.
- **Code.** `FileTree`: `removing(path)` (the next row selected without
  moving the keyboard; `goingRow_`, and `afterRowsGone` moves the keyboard
  once the row is out, else the list would anchor on that row and push the
  rows above down by one); `onItemsChanged` (the tree model's first
  handler: the selection and the list's row widgets are still as they
  were) notes a selected row going (`lostAt_`, `lostPath_` from
  `onSelectionChanged`'s `selPath_`, `lostKeys_`), and `afterRowsGone`
  selects the row in its place when its file is gone from the disk;
  `keepRows`, `applyKeep` and `stopKeeping` keep the first row on show
  where it was drawn when a scroll goes to 0 within 300 ms of rows going
  (`applyKeep` on the frame clock's "layout", connected after the window's,
  and in `afterLayout`); a press, a key, the wheel (a scroll controller), a
  drag's edge scroll, a reveal's scroll and a window record's scroll end
  it. `TreeFollow::quiet`: the tabs `trashNow` closes are neither revealed
  nor selected. `main.cpp`'s `trashNow`: `removing` before the tabs close,
  inside `quiet`, and no `clearSelection`. Icons: the core's
  `FileIcon::Type`, `typeOf`, `typeName`, `allTypes` (FileDetails,
  `tree:icon-types`); `src/TreeIcons.{h,cpp}` (new): each type's names
  from the system theme, the first `gtk_icon_theme_has_icon` finds, kept
  per theme and forgotten on its "changed"; `FileTree::setIcon` and
  `refreshIcons` (connected after the theme's "changed"); the listing's
  worker sets `minicode-exec` on a regular file with no extension that
  `access(X_OK)` allows. Folders keep the tinted folder-symbolic.
- **Differs from the Mac**: the Mac still draws `Kind`'s SF Symbols;
  `typeOf` is in the core for it. Its tree reloads only the folders that
  changed and never had the jump.
- **Checks**: the `tree_edits` suite (new: a folder of about 120 rows,
  Move to Trash twice by the key, into the scratch world's
  `$XDG_DATA_HOME/Trash`, checked before anything is trashed; a selected
  file deleted on disk; rows going and coming above the view; Rename; the
  last row) and the `tree_icons` suite (rewritten: one file of each type,
  each row's name in its type's family and the theme's answer, then Yaru
  where installed, the main names checked, the pictures counted, three
  snapshots). Before the fix `tree_edits` showed the tree at 0 (was 745)
  with nothing selected after the first trash.
- **Left for a real screen**: the icons' look at the owner's scale and in
  Papirus-Gruvbox (the snapshots are Yaru at 1x), and whether a one-frame
  flash of the top can show before `applyKeep` runs (it runs after the
  layout and before the paint, so none is expected).

### 110. The terminal's cursor shows, and readline's Ctrl keys reach the shell (done, November 2026)

The owner's two reports from the ThinkPad (Ubuntu, GNOME, Toshy): "the
cursor in the terminal for dark mode does not appear to exist", and
"Ctrl+W does not delete the word to the left of the cursor".

- **The cursor.** `vte_terminal_set_colors` unsets VTE's cursor, cursor
  text, highlight and bold colors. `applyTheme` set the cursor (#AEAFAD)
  and the selection once, then `applyColors` ran `set_colors` for the
  app's settings and set only the cursor's text (the background). With a
  cursor text and no cursor color, VTE takes the cell's own background for
  the block (`determine_colors`: no swap once either is set), so the block
  and the outline it draws without the keyboard were the background, and
  the character under the cursor was drawn in the background too: no
  cursor on any theme, dark or light, and a character under it vanished.
  The terminal's selection had fallen back to VTE's reversed colors the
  same way. Now `applyColors` sets, after `set_colors`, every time: the
  cursor in the terminal's text color and the character under a block in
  its background, both opaque (core `Settings::terminalCursor` and
  `terminalCursorText`, as the Mac's grid draws it: no built-in theme
  has a cursor color of its own), and the selection in the editor's
  (`theme::editorSelection`, #264F78 dark, #ADD6FF light). `pal::TermCursor`
  is gone.
- **The keys, before and now.** Nothing changed in mutible's key model,
  which already had what was asked: while the terminal has the keyboard,
  every plain Ctrl key that is not Global is no accelerator
  (`Commands::linuxTerminalTakes`, Keys.cpp's `bound`), the Linux form of
  the Mac's rule (keys on Command, Control to the shell). The harness,
  with no remapper, found Ctrl+W, A, E, U, K, R, L, D, C, Z, Y, P, N, B
  and F all reaching bash as their control bytes before this item, as the
  `keys` suite had for Ctrl+W, B, N, L, H, G, O, Q and F; Ctrl+W at a
  prompt deleted the word. The Global plain Ctrl keys, the only ones the
  terminal does not get, are Ctrl+`, 0, 1, 2, Tab, Page Up, Page Down
  (ways out) and Ctrl+I (the inline agent, as VS Code's terminal chat).
  VS Code's terminal on Linux sends keys to the shell too, apart from its
  `terminal.integrated.commandsToSkipShell` list, which keeps Quick
  Open's Ctrl+P and Ctrl+E and its terminal find's Ctrl+F (and chords
  such as Ctrl+K, while `terminal.integrated.allowChords` is on);
  mutible gives those to the shell, with Find… in the Edit menu.
- **Toshy** (read from `~/.config/toshy/toshy_config.py` and its
  preferences, never written). The ThinkPad's keyboard ("AT Translated
  Set 2 keyboard") is Toshy's Windows type, with Caps Lock left as Caps.
  mutible's window class, `ai.ible.Mutible`, is not in Toshy's
  `terminals` list, so the "Cond modmap - GUI - Win kbd" modmap applies:
  physical Left Ctrl becomes Left Super (and Left Alt, the Mac's Cmd,
  Right Ctrl). A physical Ctrl+W therefore reaches mutible as Super+W,
  which no keymap of Toshy's changes (General GUI remaps only Super+A, E,
  B, F, N, P, K and D, to Home, End, the arrows, Shift+End Backspace and
  Delete); VTE gets Super+W and types a plain w (a probe in the harness:
  `echo one two` and Super+W left `echo one twow`), which is what the
  owner saw. The fix is Toshy's: a
  keymap in the `user_apps` slice for mutible's window that turns Super
  and the shell's letters back into Ctrl (Super+W to C-w, and U, R, L, C,
  Z, Y, K, D, and A, E, B, F, N, P if the shell's own meaning is wanted
  over General GUI's), as Toshy's VS Code keymap does for Super+C and
  Super+X. Toshy cannot tell the terminal from the editor, so in the
  editor those keys then do what Ctrl does (Ctrl+W closes the tab, as
  Cmd+W does). The owner's existing "User overrides MiniCode" keymap
  (Super+Space to C-Space, completion) matches `clas="^org\\.minicode\\."`,
  the class from before 1.6.0's rename, so it no longer applies to
  mutible at all: its `clas` wants `^ai\\.ible\\.Mutible$`. Adding mutible
  to Toshy's `terminals` list instead would make Cmd keys Ctrl+Shift in
  the whole window (Cmd+S as Save As), so it is not the fix.
- **The `agents` suite's "the same key closes the panel" (fixed, November
  19).** It failed 2 of 3 runs of `agents` alone on main's build (3 of 5 on
  another branch), the panel left open and the Worktrees steps after it
  failing in turn. A probe (`fprintf` in `act_toggle_agents`, in the
  window's capture-phase key controller `onHintsKey`, and in the step)
  found the action ran no times: as the key went, the window's focus was a
  `GtkListBoxRow` with no parent and no root, and the key reached none of
  the window's controllers. `AgentsPanel::rebuild` removes every row and
  makes them again; the last session's row had the keyboard (the step's
  click on it), and GTK moves the keyboard off a removed widget only at
  the window's next frame. Until then `gtk_root_get_focus` names the
  removed row, a key goes to that row alone (its event's path runs up its
  parents, and it has none), and no accelerator runs. The step pressed the
  key as soon as the wait saw the rows gone: in passing runs 33 to 47 ms
  after the rebuild, after GTK's frame had put the keyboard in the tab
  strip's GtkScrolledWindow (where the failing logs found it, 5 s later);
  in failing runs before it. A person met it too: a key pressed in the
  frame after any remake of the rows, on a model change while Claude Code
  writes a transcript or on the 30 s tick, was lost while a session row
  had the keyboard; the Processes panel remakes its rows every half
  second the same way. Now `linux/src/ListKeyboard.h` (header only) puts
  the keyboard on the panel itself before the old rows go, and on the
  selected new row (else the first) once they are in, or leaves it on the
  panel, focusable while it has it, when there are none, so the window's
  focus is never a removed widget; both panels use it. Opening the Agents
  panel with no sessions now gives the panel the keyboard (its list is
  hidden and could not take it), as the Problems panel does. The step
  checks the keyboard is on the panel before the key, which failed in
  every run without the fix (on the removed row, or in the tab strip once
  GTK had moved it), where the key itself failed only when the race was
  lost. Then 15 runs of `agents` alone passed in a row.
- **Docs.** `docs/REFERENCE.md`: the Linux column says "in the terminal
  the program has it" for Ctrl+D, Q, C, X, V, F, N, O, S, L, B and Z, the
  terminal's word and line deletion row has Ctrl+W and Ctrl+U, and the
  keys.conf paragraph names Ctrl+2 and Ctrl+I among the Global keys, says
  what VS Code does, and says what a Mac-style remapper does to Ctrl.
  `docs/notes/terminal.md` has the trap.
- **Checks.** `terminal_keys` (TerminalKeysSuite.cpp): no accelerator for
  any of the fifteen with the terminal focused, each read back as its
  byte from a raw `head` and from VTE's commit signal, no command run;
  Ctrl+W at a bash prompt (`echo one two`, Ctrl+W, `three`); and Close
  Tab by Ctrl+W with the editor focused. `terminal_draw`
  (TerminalDrawSuite.cpp): the cursor cell's pixels on all nine themes,
  the block on a character on Dark Modern and Light Modern, and the
  outline without the keyboard. Its cell is 8x17 in the harness, at half a
  pixel, so a block's edges and an outline are blended over two pixels;
  the checks count over the cell and a pixel around it. Run against the
  old `applyColors`, it found no pixel of the cursor's color on any theme,
  the cell the background's, and the x under the cursor gone too. Core: `settings:terminal-cursor` (every
  theme's cursor against its background, 3:1 or more, and its text
  against the cursor), `commands:linux-terminal` (readline's fifteen are
  the shell's whatever has them).

### 111. The shortcut hints in as many columns as fit the window (November 2026)

The owner's ask: "I don't know why the help window is only two columns wide
with a scroll bar. It should be wider to show everything on one screen,
ideally." The Mac's answer (12d7a32c, #203) in the core,
`PageMeasure::layoutHints`, now lays out the GTK panel too.

- **What a user meets.** Ctrl+Shift+H shows the shortcuts in as many
  columns as the window's width holds (16 at most), their heights
  balanced, so on a wide window every shortcut is on one screen: five
  columns and nothing to scroll on a 2000x1000 window at the desktop's 10
  points, three at 1400 (the list scrolls when even those are taller than
  the window, as at 1400x900), one on a 640 pixel window. The columns
  follow the window as it is resized and the editor's zoom (Ctrl+= and
  Ctrl+-: three steps in, 12.5 points, two columns at 1400). Keys are blue and in
  Monospace, what they do in the desktop's font, a pane's state ("Terminal
  hidden") muted at a stop of its own, headings small, semibold and in
  capitals, as on the Mac; a heading or the terminal's note is never a
  column's last line, and every column's lines line up. The search box
  marks and dims as before, the columns, their text and the panel's size
  unchanged while it searches, and the highest mark is scrolled to. With
  no room for one column (a 440 pixel window) the one column's labels wrap
  under themselves, at the label's stop.
- **Code.** Core: none; `PageMeasure::layoutHints` is the Mac's. GTK:
  `linux/src/HintsColumns.{h,cpp}` (new; both build lists): the rows'
  widths measured with Pango in the fonts drawn (a cache a size), the
  core's items made from the GTK rows (a line of text kept with what
  follows it as a heading is, a row with no key a second line of the row
  before it, the blank line before "Hide these hints" a gap), each column
  one PangoLayout with its own tab stops, drawn a line at a time into one
  GtkDrawingArea in the panel's scrolled window; `HintRow` gained `state`,
  `column` and `stateAt`; `HintsSearchBox::attributes` adds the marks to
  each column's attribute list (the state is a third field searched, as
  the Mac's); main.cpp's `hintsRows` (pane rows by `pane()`, the state no
  longer spaces in the label), `hintsRoom`, `showHintsText`,
  `hintsQueryChanged`, and `onHintsPlaced` on the overlay's
  `get-child-position`, which lays the list out again, from an idle, when
  the overlay's width is not the one it was laid out for. The harness's
  Xvfb screen is 2560x1600 (`Wrapper.cpp`).
- **Traps.** Pango's absolute line height
  (`pango_attr_line_height_new_absolute`) does not make lines that mix
  fonts as tall as asked: each run's box is centered on its own font's
  metrics and the line is their union, so a key in Monospace beside a
  label in Cantarell came out half a pixel taller, 586 pixels for 26
  lines of 22 (hints_panel's first run), and the columns drifted apart;
  the lines are drawn one by one at n line heights down instead
  (`pango_cairo_show_layout_line`, `lineTop` for the scroll). GTK keeps a
  window inside its monitor: on the harness's old 1280x1024 screen a
  window asked to be 2000 wide stayed 1280, hence the wider screen. The
  scrolled window's viewport puts its child at whole pixels, so a
  scroll value that is not whole moved the text a pixel in hints_panel's
  geometry check; the scroll to a mark is rounded and the check allows a
  pixel. After a relayout the drawing area is allocated its new width a
  frame later: the checks wait for it (`settled`) before measuring.
- **Not done.** The search box's own font does not follow the zoom (the
  Mac's does). Pango has no per-paragraph indent, so in the wrapped one
  column a heading or the terminal's note that wraps also continues at
  the label's stop.
- **Checks**: `hints_panel` (HintsPanelSuite.cpp, its header saying what
  it checks: 2000x1000 five columns and nothing to scroll, 1400x900 three,
  640x800 one, 440x700 one that wraps, zoomed in two, every column ending
  on a shortcut and clear of the next, the search's marks and dims with
  the columns and the panel's size unchanged), and `keys` (the hints'
  keys, read from the columns' text). The whole GUI set passed on the
  wider screen (5,770 checks).

### 112. Browser bookmarks per workspace (November 2026)

The owner's request, the Mac's #206 brought to GTK with the same core
calls (`WebBookmark::workspaceFolder`, `places`, `lookupOrder`, `entries`,
`mergedEntries`, `moveDestination`; none of their logic copied).

- **What a user meets.** In a window on a folder, Bookmark This Page
  (Ctrl+D with the page focused, the star, File > Save Page as Bookmark)
  writes to `<folder>/.mutible/bookmarks/`, made on the first save, no
  .gitignore written; the status bar says "Saved “Alpha.url” in
  .mutible/bookmarks". The window's folder is the workspace whatever the
  tree's focus. The book and the menu bar's Bookmarks menu list that
  folder's bookmarks first ("No bookmarks" when it has none), then a
  "Shared" submenu with ~/Bookmarks (`browser.bookmarks`) when it has
  any; the shared folder and each folder in it end with "Move to This
  Workspace", a submenu of its bookmarks, which moves the file to the
  workspace's top level, a number added when the name is taken. "Already
  bookmarked" and the star look in the workspace, then Shared ("Already
  bookmarked in Shared as ~/Bookmarks/..."); the filled star trashes the
  workspace's file before a shared one. The palette's rows are
  "Bookmarks > Name" and "Bookmarks > Shared > Name". Another folder's
  window lists its own. The window on ~/Bookmarks (`mutible https://...`)
  is as before: that folder alone, saving in its tree's folder, no Shared
  part; opening a .url file (the tree, the file manager) is unchanged. The
  tree hides `.mutible` as every dot name until Show Hidden Files.
- **Code.** `linux/src/Bookmarks.{h,cpp}`: `places` (the root taken as
  the shared folder when `sameFolder`, as the Mac's `MCPlaces`), `names`
  (a folder's names in the tree's order, for `WebBookmark::entries`;
  `list` and the menus now go through the core), `inside`,
  `menu(Places)`, `fileFor(Places, url)` (`lookupOrder`) and
  `moveToWorkspace` (`g_file_move` without overwrite, so never over a
  file, the next free name on `G_IO_ERROR_EXISTS`; GIO copies across file
  systems). main.cpp: `bookmarkPlaces`, `bookmarksMenuWindow`,
  `bookmarkPlace` (the status bar's ".mutible/bookmarks"),
  `act_move_bookmark` (`win.movebookmark`), the star, the book, the save
  actions and the palette rows on the window's places, the watches on
  each window's folder too, and a refresh when a window is re-rooted.
- **Traps.** GtkPopoverMenu names each submenu's page by its label: a
  second submenu of the same label in one menu is refused ("While adding
  page: duplicate child name in GtkStack") and its button opens the
  first, so Shared > Reading > Move to This Workspace showed Shared's own
  list (the check fails so with the fix taken out). `Bookmarks::menu`
  puts zero-width spaces after a label met again in the menu (`Builder::
  appendSubmenu`), which also covers two folders of one name, a case the
  one-folder menu had too; the checks read labels with them dropped. The
  menu bar's Bookmarks menu is one GMenu for the whole application (GTK's
  menubar model), not one a window: it is filled for
  `gtk_application_get_active_window` and again on every window's
  `notify::is-active`, so the check waits for it to follow the window
  given the X focus.
- **Checks**: `bookmark_workspace` (BookmarkWorkspaceSuite.cpp: saving
  in A, the tree hiding `.mutible`, the book and the menu bar's menu with
  Shared and its Move to This Workspace items, "already bookmarked"
  preferring the workspace, a move by clicks through the book's pages and
  one onto a taken name, B listing
  none of A's, the menu following the window in front, and the window on
  ~/Bookmarks keeping the one folder; the shared folder is the scratch
  home's ~/Bookmarks, checked to be under `MCGuiScratch()` before
  anything is written). `bookmark_save` and `bookmark_menu` now save in
  and list the scratch project's `.mutible/bookmarks` and remove it at
  their end; `bookmark_menu` checks the palette's Shared row. The core's
  `bookmarks:workspace-*` groups of `make test` are the Mac's.

### 113. Mac-style keys in the terminal, for Toshy and Kinto (terminal.mac-style-keys; November 2026)

The owner types on a PC keyboard through Toshy, which in mutible's window
(not in its `terminals` list) sends the physical Cmd-position key, beside
the space bar, as Ctrl, and the physical Ctrl as Super (item 110). With
#209's rule, every plain Ctrl key that is not Global goes to the shell
while the terminal has the keyboard, so there the owner's Cmd+W deleted a
word, Cmd+C and Cmd+V went to the shell, and their Ctrl+W typed a w. The
owner approved an opt-in setting that makes the physical keys do what
they do in Terminal.app.

- **The setting.** `terminal.mac-style-keys` (Settings.cpp's keyTable, its
  commented default in `defaultFileText`, `Settings::terminalMacStyleKeys`),
  Linux only, false by default. The Mac ignores it (its keys are on
  Command already).
- **Ctrl is the Mac's Cmd.** `Commands::linuxTerminalTakes(c, k, macStyle)`
  with `macStyle` gives no key with Ctrl to the terminal but VTE's own
  Ctrl+Shift+C and V. Keys.cpp keeps the flag (`Keys::setTerminalMacStyle`,
  set at startup before `Keys::init` and in `onSettingsChanged`, which
  applies the accelerators again), so in the terminal every Ctrl key of a
  command that applies there is an accelerator: Ctrl+W, F, S, P, N, O, B,
  Q, R, T, comma, and Ctrl+Shift+G and Space too. Close Tab
  (`act_close_tab`) with the terminal's keyboard and the setting on closes
  that pane, as the Mac's `closeFocused:` does (`closeFocusedTerminal`): a
  running program asks first, and the last tab leaves a fresh one and
  hides the dock (TerminalDock::remove). With it off the menu's Close Tab
  is as before. A key no accelerator took goes to the terminal's capture
  controller (`Terminal::macStyleKey`, core `Commands::linuxMacStyleKey`):
  Ctrl+C copies the selection (nothing without one), Ctrl+V pastes as
  Ctrl+Shift+V does (`Terminal::pasteClipboard`), Ctrl+A selects all, as
  the Mac's grid does for Cmd+C, V and A, and any other Ctrl key that
  types a character, with Shift or Alt too, is dropped: it never reaches
  the program. Ctrl with a key that types nothing (Ctrl+Left,
  Ctrl+Backspace) goes on to VTE, and Ctrl+Up and Ctrl+Down still move
  between commands.
- **Super is the program's Ctrl.** Super with a character sends what VTE
  sends for Ctrl with it (`Commands::terminalControlByte`) through
  `vte_terminal_feed_child`, so it reaches the host by VTE's commit as
  typing: every letter, Shift or not (a terminal sends one byte for Ctrl
  and Ctrl+Shift with a letter), and readline's punctuation (Super+[ Esc,
  Super+] ^], Super+\ ^\, Super+Shift+- ^_, Super+Space and Super+Shift+2
  NUL, Super+Shift+6 ^^, Super+/ ^_, Super+3 to 8). Super with Alt and a
  letter sends the same (the coordinator's addition): Toshy's general
  keymap rewrites Super+K and Super+D (and Super+A, E, B, F, N and P, to
  Home, End and the arrows, which readline takes the same way, so the
  physical Ctrl+A, E, B, F, N and P never arrive as Super), and GNOME
  grabs Super+L to lock the screen, so Toshy's keymap for VS Code's
  terminal sends the physical Ctrl+K, D and L as Super+Alt+K, D and L; a
  keymap of Toshy's for `ai.ible.Mutible` can do the same. Super with
  Ctrl, or with a key that types nothing, goes on to VTE.
- **Unchanged:** everything with the setting off, the editor and every
  pane but the terminal with it on (the controller is on VTE's widget),
  and the hints search box, which takes no key with Super. The hints
  panel's "In the terminal" part says what Ctrl and Super do while it is
  on.
- **Docs.** `docs/REFERENCE.md`'s keys.conf paragraph (the plain Ctrl keys'
  rule names the setting) and a new paragraph for remappers, Toshy's
  Super+Alt keys among them; `docs/notes/terminal.md`.
- **Checks.** `terminal_keys` keeps #209's half as it was (the setting
  off) and adds: VTE's own bytes for Ctrl+[, ], \, @, _, ^, Space, /, 3
  to 8, read from a raw `head` and VTE's commit, equal to
  `terminalControlByte`'s (they were on the first run, so the table is
  VTE's, not only xterm's); then with the setting on the hints panel's
  terminal part, Ctrl+W, F and S as win.closetab, find and save there and
  Ctrl+K, U, L, Z, X and D as nothing; Ctrl+K, U, L, Z, X and D pressed,
  then Super with W, A, E, U, K, R, L, C, Z, Y, Shift+W, Alt+K, Alt+D and
  Alt+L and with the fourteen punctuation keys, the program reading only
  Super's bytes; Super+W at a bash prompt (`echo one two`, Super+W,
  `three`), Super+Alt+K killing " gone", Super+C ending a `sleep 100`;
  Ctrl+A then Ctrl+C putting the terminal's text on the clipboard, read
  back, and Ctrl+V pasting a command that Return runs; Ctrl+W closing a
  second terminal tab with no editor tab closed, and in the editor closing
  its tab; the setting off again, Ctrl+W no accelerator and deleting a
  word. The suite, `terminal_draw`, `terminal_tabs`, `terminal_splits`,
  `keys`, `agents` and `hints_panel` pass together in one harness run
  (`agents` printed a LEFT warning: the X focus left on a window it
  closed, given back). Core:
  `commands:linux-terminal` (`linuxTerminalTakes` with macStyle for the
  Linux table's Ctrl keys, `linuxMacStyleKey` for all 26 letters with
  Super, Shift and Alt, the punctuation, Super with Ctrl, Ctrl's Copy,
  Paste, Select All, VTE's own Ctrl+Shift+C and V, the dropped keys and
  the keys that pass), `settings:terminal` and `settings:default-file`
  (the new key and its default).
- **Needs the owner's keyboard:** the real Toshy on the ThinkPad (whether
  its keymaps leave Super+W, C, R, U and the rest alone in mutible's
  window, as item 110 read them, and whether its Super+Alt keymap for
  mutible is wanted), and GNOME on Wayland, where the harness's X11 Super
  (Mod4) cannot speak for GDK's Super mask.

### 114. Home and End as VS Code's, JSON errors in Problems, and wrapped lines keeping their indentation (November 2026)

The Mac's #213 and #214 brought to GTK with the same core calls
(`MultiCaret::homeTarget`, `endTarget`, `homeCarets`, `endCarets`;
`JsonOutline::checksAsJson`, `problems`, `allowsComments`;
`WrapIndent::leadingColumns`, `continuationIndent`; none of their logic
copied).

- **Home and End.** Before, GTK's own: Home and End went to the display
  line's start and end, so Home never stopped at the first non-blank
  character. Now `Carets::onMoveCursor` takes
  `GTK_MOVEMENT_DISPLAY_LINE_ENDS` (Home, End, the keypad's, with Shift)
  in source text at one caret or several (`Carets::homeOrEnd`): Home to
  the first non-blank character, then the line's start, then back; End to
  the line's end; on a wrapped line End goes to the row's end first
  (before the space it broke at, so the caret is drawn on that row) and
  Home on a later row to its start first. A row is GTK's display line
  (`Carets::rowAt`: `gtk_text_view_backward_display_line_start`, then
  `forward_display_line` for the next one's start); not wrapping, the
  whole line. GtkTextBuffer keeps the anchor itself (the selection bound),
  so Shift needs none of the Mac's head-finding: Shift+Home then
  Shift+Right gives a character back. A Markdown page, a message or the
  welcome text keep GTK's. The registry's `cursorHome`, `cursorHomeSelect`,
  `cursorEnd` and `cursorEndSelect` are Linux rows (home, shift+home, end,
  shift+end), listed, not actions, like Ctrl+Home; the hints panel has
  "Start of the line" and "End of the line (Shift selects)", and
  REFERENCE.md's Linux column the keys.
- **Trap: a long line read in blocks.** The core reads the text through
  `MultiCaret::Text`, which asked `CaretText::text()` for 1,024-character
  blocks; each block's `gtk_text_buffer_get_iter_at_offset` found its place
  in a 5 MB line from the line's start, so End there took 49 s of CPU
  (large_file's stall check). `CaretText::textNear(offsets)` reads the
  carets' lines whole, once each, and serves the core's blocks from them:
  92 ms for that End now (the bound is 250). `moveAll` (the arrows, and End
  at several carets before this item) still reads through `text()`.
- **JSON in Problems** (`LspSessionJsonCheck.cpp`, the Mac's
  Lsp+JsonCheck.mm): a file `JsonOutline::checksAsJson` names that no
  server has (`server_` and `alsoServer_` null, which servers off
  includes) is parsed on a detached thread when it comes on show
  (`documentChanged`) and 0.3 s after typing (`textEdited`); the answer
  comes back through `lspui::afterMs` under `life_` and a generation, and
  goes into the diagnostics store as the list of a server of its own,
  `mutible.json`, source "json". Painting and F8 use `shownUri()` (the
  server's URI, else the check's), `problemsKnown` holds while a file is
  checked (so a valid one says "No problems"), `tabsChanged` drops the
  lists of files no tab has, and a server taking the file erases the
  editor's list. Over 8 MB: no check. JSON with comments (tsconfig.json,
  .jsonc, .vscode's files) allows comments and trailing commas.
- **Wrapped lines keep their indentation** (`WrapIndentTags.{h,cpp}`,
  `editor.wrapping-indent`): GtkTextView has nothing like TextKit's line
  fragment rects, but a paragraph's `indent` taken negative is a hanging
  indent. Each source line with leading spaces or tabs and something after
  them carries one tag of the shared tag table, `mutible-wrap-<columns>`
  (columns by `WrapIndent::leadingColumns`, tabs to Pango's 8-column
  stops), over the whole line and its break, so alike neighbours join up.
  Each tag's pixels are `-continuationIndent(columns × space, mode, 4 ×
  space, room)`, set by `WrapIndentTable::configure` from
  `Editor::updateWrapIndent` (the setting through `applySettings`; a space
  measured as the ruler measures, in the view's font, only while it shows
  source; the room the text window less its margins), run in an idle after
  each allocation (`CodeView::onAllocated`) and after a zoom; only tags
  whose pixels change are set, so a zoom or a resize past the half-room
  fall back re-lays lines out, and typing never does. `WrapIndentTags` (one
  a document) retags the lines an edit touched, in the buffer's
  `insert-text` and `delete-range` after-handlers: a space typed into the
  indentation took 0.02 to 0.03 ms of CPU there in a file of 20,000
  wrapped lines, and nothing changes when the leading width stays (the
  line's first character's tags are read with `gtk_text_iter_get_tags`;
  only they count, as GTK takes a paragraph's attributes from there). More
  than 1,000 lines (a file opening, a big paste) get the lines on screen
  at once and the rest in idle slices of 5 ms, the waiting range carried
  through edits (`noteLines`). A paste from another buffer brings its
  tags; `apply-tag` refuses wrap tags from anyone else, as the
  highlighter's. On only for text, not in large-file mode (no wrapping
  there) and not with none, decided in `Document::wrapIndentSettingChanged`
  (from `startHighlighting`, so each load, rename and large-file switch,
  and from `Editor::applySettings`); tags left from before keep 0 px with
  none. Nothing reaches the text, its undo, a copy or a server. Left: the
  first source file shown after the welcome text may draw one frame with
  0 px (the space is measured in an idle once the view's font is the
  source's).
- **The CSV grid's menu keys** (#214's other half): the Mac's grid
  swallowed every menu key; GTK's never did (its key controller takes only
  Ctrl+Z and Ctrl+Y, and the window's accelerators run), now checked.
- **Checks**: `editor_keys` (new, EditorKeysSuite.cpp: Home and End
  first and second presses, End Left Left, Shift with Shift+Right and
  Shift+End past the anchor, two carets plain and with Shift, and a
  wrapped line's rows: End to the first row's end, drawn on that row, then
  the line's end; Home on the second row to its start, then the line's;
  Shift+End to the row's end; two carets on two rows); `problems` (a
  package.json with a trailing comma: one error drawn on the comma, the
  list's rows "# package.json — 1 error | E 3:21 Trailing comma [json]",
  the status bar's "1 error"; Delete takes it away, "No problems"; typed
  back, back; tsconfig.json's comment and trailing comma none; its tab
  closed, gone); `word_wrap` (the continuations of an 8-space line and a
  tab line at the first non-blank character's x by
  `gtk_text_view_get_iter_location`, none at the edge, indent 4 spaces
  further in, a space typed into the indentation moving them, a line
  indented past half the pane at the edge, and 20,000 such lines settled
  in idle time with a typed space's retag bounded at 2 ms of CPU);
  `csv_table` (Ctrl+Shift+P in the grid opens the palette). The core's
  `commands:linux-defaults` checks the four rows.

### 115. Tabs: whole names and a scrolling row, as VS Code's tabSizing "fit" (November 2026)

The Mac's #211 brought to GTK with the same core calls
(`EditorTabs::tabNames` through `Model::Window::tabNames`, `layoutStrip`,
`clampScroll`, `revealScroll`, `wheelScroll`, `dragAutoScroll`,
`clipped`, `fitName` and its `fitMiddle`; none of their logic copied).

- **What a user meets.** A tab keeps its natural width, its whole title,
  80 to 320 pixels, and is never squeezed: before this, every title had a
  30-character cap and Pango's middle ellipsis. Only a title wider than
  the widest tab is cut, in the middle ("a_rather_long_modul…_their_
  summaries.py"). A row wider than the strip scrolls sideways by the wheel,
  vertical too, or a touchpad, with a thin bar along the bottom while the
  pointer is over it and fades over the cut-off ends; the tab on show
  comes into view whenever it is another tab (a new active index, a row of
  another length, its title or path changed by a preview replaced) and
  when the strip gets a new width, never for an unsaved-changes dot. Files
  of one name show the name and, after it, smaller and dimmed, the
  folders that tell them apart ("index.ts app", "index.ts lib"), where
  "one/README.md" was one label. A tab still drags along its row, and
  held near either end the row scrolls under it each frame. The terminal
  dock's and the browser's strips are the same class and behave the same;
  their titles have no hints.
- **Code.** `linux/src/TabStrip.{h,cpp}`: the scrolled window and its
  inner box are gone. The tabs are children of `row_`, a GtkBox whose
  layout is a `gtk_custom_layout_new` (as `Groups.cpp`'s), clipped
  (`GTK_OVERFLOW_HIDDEN`), asking no width and only its natural height as
  the scroller did. `rowAllocate` places each tab at `layoutStrip`'s x
  less the scroll, at its width, then the two fades and the bar, which
  never take a click (`gtk_widget_set_child_visible` by `clipped` and the
  pointer). A tab's natural width is GTK's measure of its box less its
  names box (`chrome`: padding, the gap, the close button) plus its whole
  title and hint measured with Pango in each label's own font (the CSS's
  italics and sizes), so it never depends on what the labels show cut, and
  `fitTitles` puts `fitName`'s result in the labels in `setItems`, and
  again from an idle when a layout finds the fonts changed since
  (`tabs.font-size`, a strip not yet on screen). The tabs are measured
  only when GTK asked the row's size again (`rowMeasure` marks it), never
  for a scroll. The wheel is a `GtkEventControllerScroll` on the row,
  both axes, its deltas negated (GTK's point the way the view goes, the
  core's the way the content goes), a notch 48 pixels, a touchpad's
  pixels as they are where GDK says they are pixels
  (`GDK_SCROLL_UNIT_SURFACE`, Wayland; X11 gives a touchpad fractional
  notches); over a row that fits it lets the event go.
  A capture-phase motion controller on the row sees a held tab's pointer
  (the tab's click gesture keeps the drag) and starts a tick callback
  scrolling by `dragAutoScroll` (28 pixels from an end, 14 a frame). The
  press and release places are the tab's left edge in the whole row plus
  the pointer's x in the tab, so a row scrolled meanwhile drops right
  (`dropIndex` over the core's widths, not the allocations). Each tab is
  a names box (the title, then the hint with a 5 pixel margin, on one
  baseline) and the close label; the names box expands, so at 80 pixels
  the close button stays at the right end. `Tabs.cpp`'s `refreshStrip`
  and `Groups.cpp`'s `refreshMirror` take `tabNames` (a diff keeps its
  title); `ThemeCss.cpp` adds `.minicode-tab-hint` (92%, opacity 0.55),
  the fades in the strip's color and the bar in the sidebar text's at
  0.45. Checks read `displayedTitle`, `displayedHint`, `hintWidget`,
  `rowWidget`, `tabLeft`, `tabWidth`, `contentWidth` and `scrollOffset`
  (as last laid out: read right after a title changed, it can be the old
  layout's, even past the end).
- **Trap: a reveal waiting for its frame undid a wheel.** `setItems`
  only marks the reveal; the layout does it. In `browser_tabs` (two runs
  in eight) the page's title came in, so the active tab's title changed,
  and the wheel clicks arrived before the next frame: they scrolled the
  row, and the layout after them revealed the active tab again, back at
  the end. `settle()` now does the layout's pending work (measuring anew
  if GTK has not, the reveal) before a wheel, a drag's press and each
  auto-scroll frame, so the user's scroll goes on from where the row is
  about to be. `tab_strip` sends Alt+1 and two wheel clicks in one step
  and wants the row two notches from its start; with `settle` emptied it
  failed three runs in three, and it passed eight browser_tabs runs in
  eight after.
- **The old strip squeezed.** The scrolled window gave its row the
  view's width whenever the tabs' smallest widths (an ellipsis each) fit,
  so its tabs shrank instead of scrolling, and `browser_tabs` clicked the
  first tab and dragged it onto the last with five tabs in a 378 pixel
  pane. They are 88 pixels each now, a row of 440 scrolled to the last;
  the suite wheels the row back to its start before clicking the first,
  and drags it by holding it at the strip's right end until the row has
  scrolled to its end, then letting go.
- **Checks.** `tab_strip` (TabStripSuite.cpp, 130 checks): seventeen
  files (the Mac's fifteen, a.c and a 75-character name) in an 851 pixel
  strip, a row of 2116; every title whole but the long one, which keeps
  its first and last eight bytes around "…", and no label ellipsized by
  Pango; widths 80 (a.c) to 320 (the long one), end to end, each where
  GTK allocated it; the last tab in view at the row's end with the left
  fade only; Alt+1 back to 0 with the right fade only, Ctrl+Page Up to
  the end again; two wheel clicks up 96 pixels with the bar and both fades
  and nothing chosen, one down 48; typing in a tab scrolled half out of
  view put its dot there without moving the row, undone; the index.ts
  tabs' hints, after the title, 12.3 pt to its 13.3; summary.py dragged
  past config.json; a tab held at the strip's right end scrolling the row
  to its end and dropped last; Alt+1 with two wheel clicks at once (the
  trap above). With the reveal taken out 14 checks failed; with a reveal
  on every `setItems`, the dot's. `tabs` now also checks the README.md
  tabs' name and hint, and `browser_tabs` scrolls its row (above). With
  `tabs`, `preview_tabs`, `tab_viewers`,
  `split_editor`, `sessions`, `window_title`, `go_back`, `terminal_tabs`,
  `terminal_splits`, `layout`, `adjust_settings`, `backups`,
  `browser_tabs`, `themes`, `compare` and `owner_reports` passing. The
  core's `tabs:names`, `tabs:strip-widths`, `tabs:strip-scroll` and
  `tabs:fit-title` groups of `make test` are the Mac's.
- **Needs a person:** a real touchpad's feel (its smooth deltas go in as
  pixels, with no kinetic glide after the fingers lift, as the Mac's
  strip) and a real wheel's notch size on the owner's mouse.

### 116. Find in Folder: Enter never replaces, the panes take the keyboard, dismissed rows stay out of Replace All (the Mac's #210; November 2026)

On the Mac (1.6.3) a walker's `python3 -m pytest -q` and Enter landed in
the Search window's replace field, where Enter was Replace All, and a file
was rewritten; a row's Dismiss took it out of the list, but Replace All
re-planned the folder and replaced it anyway. #210 fixed both there and
added the core's `FolderReplaceExclusions`, `excludeDismissed`,
`dismissedLines`, skip lines in `replaceInText` and
`TextFind::dropEditsOnLines` (`search:dismissed`). GTK had the same
Enter, no Dismiss at all, and a search window no accelerator reached.
`linux/src/Search.{h,cpp}` and `main.cpp`, with the core's functions only:

- **Enter in the replacement** (`fieldReturn`) does what Enter in the
  query does: it searches, or, when the list already answers the query,
  goes into the results on the first row. It never replaces. (The Mac's
  Enter always searches again; GTK's query had gone into the list since
  September 2026, and kept that, so Enter after a dismiss keeps the
  dismissals while rows are left.) Replace All is its button or
  Ctrl+Alt+Enter, only while the replace row shows (else the bell, the
  Mac's `replaceAllFromKey`); the button's tooltip no longer says Enter.
- **The results take the keyboard.** A single click on a row selects it,
  gives the list the keyboard and shows the match in the preview tab
  without presenting the editor window (`openMatch(..., present)`; it
  had presented it, so the keyboard left the results at every click),
  as the Mac's `previewHandler` and VS Code's search view do; the arrows
  then move between rows and Enter opens one kept, as a double-click
  does, both presenting the editor window.
- **Pane keys from the search window.** The Find in Folder and TODOs
  windows are no application windows, so Ctrl+`, Ctrl+Shift+P and the
  rest did nothing there and what was typed next stayed in the fields.
  `runPaneKey` matches the keys of `kPaneCommands` (the commands the
  Mac's `currentForPane:` serves: terminal, browser, hints, sidebar
  panels, problems, palette, focus commands) against the key with
  `gtk_shortcut_trigger_trigger` over `Commands::linuxAccelerators`, so
  keys.conf moves them, and main.cpp's `runPaneCommandFromSearch`
  presents the editor window and runs the action; Toggle Terminal and
  Toggle Browser show and focus their pane, never hide it.
- **Dismiss.** GTK's results are lines without file rows, so: a row's ×
  (shown on `:hover` and `:selected`, `installRowCss`; not focusable),
  Delete in the list (VS Code's Linux key for Dismiss; the Mac has none)
  and the right click menu's Dismiss take the line, and the menu's Dismiss
  File every line of its file (the Mac's Dismiss on a file's row). The
  rows leave the list a run at a time (`removeRows`, the scroll kept), the
  keyboard held on the window's box meanwhile (`ListKeyboard::hold`; a
  scroller would pass the focus to the list's removed row) and given to
  the row now in the first one's place; the status says what is left
  ("3 matches in 2 files (dismissed rows are left alone)"). Replace All
  copies the dismissals into its plan job (`excludeDismissed` on the
  worker, so a search while it counts cannot change them), the question
  says how many matches they keep, and `replaceInPaths` skips dismissed
  files and lines in tabs (`dropEditsOnLines`) and on disk
  (`replaceInText` with skip lines); a file whose dismissed line changed
  since the search is left alone whole and named. A new search clears
  them. In the TODOs window the same keys and menu take rows out of the
  list (and out of its filter's source).
- **Left out:** the Mac's file rows (Replace All in This File, a file
  row's own ×), which GTK's flat list never had; Cmd+G stepping through
  the results; and #210's discarding of keys queued before the question,
  which GTK does not need: a key the X server sent while the search window
  had the focus goes to that window, where Enter now only searches.
- **Checks.** `replace_in_folder` (46 checks): its four Enter-as-Replace-All
  presses became Ctrl+Alt+Enter and the button; new steps on four files
  (seven matches, six rows) check Enter in the replacement moving into the
  results with no question and every file unchanged after a second's
  wait, Down and Down moving the selection, a click on a row from the
  replace field previewing it with the list holding the keyboard and the
  search window still active, Up from there, a line dismissed by its ×,
  a file by the menu's Dismiss File, a line by Delete (the keyboard still
  in the list, Up working after), the status each time, Ctrl+Alt+Enter
  asking "Replace 3 matches in 2 files?" with "4 matches in dismissed rows
  are left alone.", the dismissed line, file and row unchanged on disk and
  in the preview tab while the rest is replaced, every row back after;
  then Ctrl+` from the replace field making the editor window active with
  its terminal shown and holding the keyboard, and Ctrl+` again from the
  search window leaving it shown. The suite now gives the X focus back
  when it closes the window (it had printed a LEFT line). Seen failing
  without the feature: Enter as Replace All failed at the first new wait
  (the question up); presenting on a single click failed the active
  window checks; Replace All without the dismissals failed the question
  and the files (a.txt's line 2 and d.txt replaced, b.txt's tab changed).
- **Needs the owner's eyes:** the × on hover, the right click menu's look,
  and the click-then-arrows feel with a real window manager, which keeps
  the transient search window over the editor's.
- **A FLAKY line, then a fix (2026-11-19).** CI printed `FLAKY
  [replace_in_folder] ... the second row`: a search remade the rows while
  the keyboard was in them, and a frame before its results left the search
  window no focus at all, so the first Down only put the keyboard back on
  row 0 (`docs/notes/find.md`'s trap). Alt+C from the results lost it the
  same way on most runs. `runSearch` now holds the keyboard on the
  window's box while the list is empty and `giveKeyboardBack` gives it to
  the new first row. The suite empties the query before `setScope`, whose
  search of the same query had passed the wait for six rows while the
  typed one came 0.35 s later, after Enter; it waits for
  `searchedQuery()`, and checks a search and Alt+C from the third row
  (the keyboard on the new first row, Down to the second). Before: 3 of 30
  runs failed on one core shared with two busy loops, and every run with a
  50 ms sleep in the worker; the new checks failed without the fix with or
  without the sleep. After: 30 of 30 under the same load.

### 117. `exit` closes the terminal; a detached job is its terminal's (November 2026)

The Mac's #224 brought to GTK with the same core calls
(`TerminalExit::fromWaitStatus` and `decide`, `Settings::
terminalCloseOnExit`; none of their logic copied). The Processes half
needed no GTK code: the panel's `Procs::Tracker` samples `ProcFs::Source`,
whose stat parse now carries the session id, so it follows the core.

- **What a user meets.** `exit`, or Ctrl+D at an empty prompt, closes
  the terminal, as Terminal.app, VS Code, Zed and the Mac do: a split's
  pane alone, the tab with its last pane, and the dock with the last tab
  (a fresh terminal then waits, its shell started when the dock shows
  again), nobody asked. Any exit code closes; `exit 1` too, as on the
  Mac. terminal.close-on-exit = off keeps the terminal, saying "Process
  exited. Press Return to start a new shell." (with "(code 3)" when not
  0). A shell killed by a signal never closes its terminal: "The shell
  ended (signal 9). Press Return to start a new one.", in red. A shell
  that ends before its first prompt (a ~/.bashrc that exits) says "The
  shell exited as it started (code 4). Press Return to try again." and
  waits, where it used to restart three times in a second and then stop.
  While a terminal waits, only Return reaches anything (it starts the
  new shell); Run File, `mutible term --run` and Run in Terminal start
  one and run their line in it. Before this a shell that exited printed
  "[shell exited — restarting]" and started again, so `exit` closed
  nothing. With terminal.mac-style-keys on, Close Tab (Ctrl+W) closes a
  waiting terminal unasked (nothing runs in it), and Super+D at an empty
  prompt sends ^D, which ends the shell and closes the terminal as
  Ctrl+D does with the setting off.
- **Processes.** A job detached with `( server & )`, or nohup and disown
  just before `exit`, is the subreaper's (systemd's, or the GUI harness's)
  before any look sees it under the shell. It now shows under the
  terminal that started it, by its session (the host's child calls
  `setsid`, so the shell leads the pty's session and the job keeps it),
  and as an orphan of the window once `exit` closes that terminal. Before
  it was "not started here".
- **Code.** `Terminal::shellExited(status, error)` (linux/src/Terminal.cpp)
  takes the host's TermExited status (`Client::TermListener::termExited`'s
  `status`, ignored until now) after `vteCaughtUp`, as before, so the
  host's last word on the shell, and its prompt count, are applied first.
  `prompted` is `seenPrompt_` (a TermShell with prompts, or a held shell
  attached, which ran before), the host's prompt count, or 2 s since the
  spawn (a shell without the integration). On Close, `onShellExit` is
  called from an idle (`exitIdle_`, removed by the destructor), not from
  inside the host listener's call, and only while no shell started again
  meanwhile; the idle calls a copy of the callback, since the call
  deletes the terminal. `TerminalDock::hook` sets it to `closePane(t,
  false)`, so the close is the same code as Close Terminal's and Close
  Tab's: `removePane` gives the keyboard to the neighbour, `remove`
  leaves a fresh terminal and calls `onEmpty`, and `onChanged` reports
  the session record. The dock clears `onShellExit` wherever it clears
  `onFocusIn`. Waiting (`awaitReturn_`, `waitingForReturn()` for the
  checks), `onCommit` sends nothing to a host (there is no pty) and a
  Return calls `startAgain`; `typeText` and `runLine` call it too. A
  line `runLine` or `typeText` still holds when the shell ends gets a new
  shell, not a close, as the Mac's waiting `runCommand:`. Before the note
  VTE is fed the alternate screen's end, a soft reset and bracketed paste
  and mouse reports off, as the Mac's grid is. `rapidExits_` is gone.
  `feedNotice` takes an `error` flag (bright red, ANSI 91, the Mac's
  0xF48771 in this palette).
- **The session host.** Every GTK terminal is on it: the host reaps the
  shell and sends TermExited with its wait status after the output before
  it, and has ended its terminal by then, so `hostTerm_` goes to 0 and
  the destructor closes nothing. A held terminal attached by a later
  window closes on `exit` too (`seenPrompt_` set in `attachShell`). The
  host going away is not a shell's exit: the lost listener's note, and
  no Return restart, as before.
- **Checks.** A new suite, `terminal_exit` (TerminalExitSuite.cpp), with
  real keys: `exit` in the second of two tabs closes it, nobody asked,
  the first active with the keyboard and the dock shown; Ctrl+D in a
  third does the same; in a split, `exit 1` closes only that pane, the
  other keeping its shell and the keyboard; `exit` in the last terminal
  hides the dock, and showing it starts a new shell; `kill -9 $$` keeps
  the terminal with the signal's line, and Return starts a new shell; a
  ~/.bashrc with `exit 4` says so, stays 1.5 s with no shell (a negative
  check), and Return with the file mended starts one; with
  close-on-exit off, `exit 3` and `exit` each say so and stay, Return
  starts a shell, and `runLine` on a waiting terminal starts a shell and
  runs the line; with mac-style keys on, Ctrl+W closes a waiting
  terminal unasked and Super+D at an empty prompt closes the last
  terminal and the dock. `processes` adds the detached job:
  `( (trap '' HUP; exec sleep 623) & echo $! > file )` in a terminal of
  its own, whose parent is then not the shell but whose session is,
  shows under that terminal in the panel (its row's terminal id), `exit`
  closes the terminal, and the job is in the panel's orphans; the suite
  then kills it. `host_reattach`'s in-process part ends the reattached
  (held) shell with ^C and `exit` instead of closing its pane by hand: the
  terminal closes and a fresh one waits. The crash is SIGKILL, not
  SIGSEGV: a segfaulting bash
  would leave a core or an apport report outside the scratch world, and
  `bash -c 'kill -SEGV $$'` typed at the prompt kills a child, not the
  terminal's shell, which only prints "Segmentation fault".
- **Numbers** (2026-11-19, this ThinkPad, cores 0 to 3): `terminal_exit`
  42 checks in 3.8 s alone; with `terminal_tabs`, `terminal_splits`,
  `terminal_keys`, `terminal_marks`, `processes`, `host_reattach`,
  `host_splits` and `shell_integration`, 454 checks, none failed, in
  53 s. Seen failing without the feature: the dock's `onShellExit` made
  a no-op, `terminal_exit` failed 35 checks (nothing closed); the
  session id blanked in `ProcFs::Source::list`, `processes` failed 2
  (the job neither under its terminal nor an orphan). The detached job's
  parent was the harness's subreaper, its session the shell's pid.
- **Not this change: `processes` and a long scratch path.** Run with
  MC_GUI_ROOT inside this worktree's `linux/build`, the second window's
  "the command is at the prompt" failed every time (and passed with
  /tmp, and with an equally long root under /tmp): bash's prompt there
  (`user@host:` and the 110-character project path) plus `(trap '' HUP;
  exec sleep` is exactly 160 characters, two full rows of the hidden
  terminal's 80 columns, and VTE's screen text ends at "sleep", the rest
  of the line never drawn on a third row. A line that fills its rows
  exactly is worth a look in the port (readline's wrap, VTE fed by the
  host while hidden); the check is unchanged.
- **Trap: suites restarted the shell with `exit`.** `shell_integration`
  (its `startShell`, for a new `$SHELL`, and its last step) and
  `terminal_marks` (a bash with no earlier suite's commands) typed `exit`
  and waited for the restarted shell; with `exit` closing the terminal
  they waited 30 s for a prompt that never came and failed 31 and 19
  checks. Each now makes a new terminal and closes the dock's others
  (`newTerminal`, then `close(0, false)` until one is left), as
  `terminal_tabs` and `terminal_splits` start; shell_integration carries
  its `onShellStateChanged` counter over to the new one. A new suite
  wanting a fresh shell does the same.
- **Not the product: a Ctrl+W that missed the terminal.** With the dock's
  close turned off to see `terminal_exit` fail (35 failed checks: no tab,
  pane or dock closed), the keyboard never reached the ended terminal and
  Ctrl+W, mac-style, closed the window; the next wait read a null dock and
  the run crashed. The step now presses Ctrl+W only while the terminal
  has the keyboard.

### 118. Source control: a click opens, the review keeps its place, pictures show (the Mac's #217; November 2026)

The Mac's #217 brought to GTK with the same core calls
(`EditorTabs::treeClick`, `ChangeReview::actedPlace`, `rowAfterAction`,
`showAsImages`, `imageLabel`; none of their logic copied). The one core
change: `ChangeReviewRead.h` takes an `ImageReading` (the GUI's
header-only size reader and the row height), and `seeImages` does in
`readUncommitted` and `readWorktree` what the Mac's `SeeImages` does in
its own reading, the bytes kept in `Extra::oldImage`/`newImage`
(`review-read:images`, with a fake reader).

- **What a user meets.** A single click on a changed file in Source
  Control opens the review of every change at that file in the preview
  tab (its name in italics, replaced by the next file or page opened in
  preview), and the keyboard stays in the list; a click on another file
  shows that file in the same tab. A double-click on the same row, Return,
  or Open Changes in the row's menu keeps the tab and gives the page the
  keyboard. A Ctrl or Shift click chooses rows and opens nothing, and the
  arrows only move the selection, as before. The row menu has Open as git
  Diff (the Mac's), which shows git's own diff with its hunk links, which
  is what a click used to show. Review Changes opens the page at its top.
  After Stage, Unstage or Discard on a block (or Stage File, Unstage File,
  Discard File, Mark Resolved on a header), the next block of that file
  takes the place on screen of the one acted on, or the next file when
  that was the file's last, even with a staged list appearing above. A
  binary picture (png, jpg, gif, webp, tiff, bmp, heic, ico, icns, as far
  as GTK's image loaders read them; SVG stays text) shows its versions:
  a new or a deleted one alone, a changed one Before and After side by
  side, each labelled with its size in pixels and bytes, at its pixel
  size, never taller than 320 pixels nor wider than its half of the pane
  (as the pane was when read). Worktree pages (Compare with Main) show
  them too.
- **Code.** `GitPanel::clickedRow(row, graph, count, sameRow, mods)` (the
  Mac's `clickedRow:graph:clickCount:sameRow:modifiers:`) takes every click
  released on the row it was pressed on; the count comes from a
  `ClickCount` per list (the tree's, by event times), `sameRow` from the
  last released row. `onReview` is now `(path, staged, preview)`;
  `reviewRow(row, preview)` calls it (Return, Open Changes: kept) and
  `openRawDiff` is Open as git Diff. main.cpp's `showChangeReview(app,
  rel, staged, preview)` shows the page with `Editor::showPage(page,
  preview)`, which puts a new page in the preview tab through
  `Model::Window::add(..., Preview::User)` and `letGo`s the tab it
  replaced (its page with it, as the Mac's `letGoOfReplacedTab:` closes
  it), and keeps a preview tab a kept open finds; then
  `focusActiveDocument` for a kept open, and `ChangeReviewPage::
  revealFile`. The page: reads carry a serial (`readsStarted_`); a press
  records `actedPlace` and the row's height on screen (`rememberActedRow`),
  `actionPosted()` marks where the work went to the git thread, and the
  first read started after that puts `rowAfterAction`'s row there; a
  refusal drops it. Scrolls go through a text mark (`scrollRowTo`): GTK
  scrolls to a mark once the lines around it are laid out, where a scroll
  to an iter just after the text was replaced used heights not measured
  yet. The pictures are sized on the git thread by a `GdkPixbufLoader` fed
  4 KB at a time until `size-prepared` (no pixel decoded), decoded at the
  size drawn by another loader (`set_size`), made textures there
  (`PixbufTexture.h`, MediaView's helper moved out), and put in a child
  anchor on the file's first `RowKind::Image` row, a box of a label and
  a framed `GtkPicture` per version; the other Image rows are tagged
  invisible.
- **Other reads keep the top row through the same settling scroll.** The
  page's old `scrollToRow` right after `fill()` scrolled to an iter whose
  lines GTK had not measured yet, and the page went to its top: with the
  acted place turned off, the check below saw the first row on show 0 and
  z2.txt's next block 5170 pixels down. The top row (`anchorAt`/`rowFor`)
  now goes through `scrollRowTo` too, with its offset above the pane's top.
  `scrollRowTo` scrolls to a mark at once and a tick callback (`onSettle`)
  puts the row right on each frame it is not, until it has held three
  frames (five seconds at most); GTK then keeps the lines on screen in
  place as it measures those above. `scrollToRow` (the checks' scroll)
  ends a settling scroll.
- **Checks.** A new suite, `git_panel` (GitPanelSuite.cpp), named for the
  Mac's: a Ctrl click and Down open nothing (negative checks, 0.8 s each);
  a click on pic.png opens the review in the preview tab (one review tab,
  `previewTab()` that tab) with pic.png's header on show and a.txt's (150
  changed lines above it) not, the keyboard still in the list on that row;
  four pictures, labelled "2 × 2 pixels, 106 bytes" (gone.png, deleted and
  staged), "Before: 4 × 3 pixels, 107 bytes", "After: 8 × 6 pixels, 108
  bytes" and "5 × 5 pixels, 107 bytes" (new.png, untracked), drawn at those
  sizes, After to the right of Before on the same line, no "A binary file"
  note (PNGs written by `gdk_texture_save_to_png`, sizes read from the
  files); a click on b.txt shows it in the same preview tab; a
  double-click on a.txt keeps the tab (no preview tab left) with a.txt on
  show and the page's text view holding the keyboard; the row menu reads
  Open File, Open Changes, Open as git Diff, and Open as git Diff shows
  git's diff with its two hunk links, the review still open. `git_review`
  adds the Mac's place check: m1.txt (150 lines changed), z2.txt (lines 2
  and 30) and zz3.txt (100 lines) after the earlier steps are put back;
  once laid out (the page taller than twice the pane), z2.txt's first
  block is put in the middle of the pane (`scrollRowTo`), Stage on it is
  clicked, and z2.txt's next block ("z 30") is within 4 pixels of where
  the pressed one was, the first row on show past m1.txt's header.
  `git_hunks` and `git_actions` open the raw diff with `RawDiff`
  (GitChecks.h, `GitPanel::openRawDiff`, as the Mac's suites call
  `showDiffAtRow:`) where they clicked a row for it, and `git_actions`'s
  row menu has Open as git Diff. `review-read:images` in the core: a
  changed, a new staged and an unreadable picture with a fake size reader;
  none without one.
- **Numbers** (2026-11-19, this ThinkPad, cores 0 to 3, another agent on
  4 to 7): `git_panel` 37 checks in 6.4 s; `git_review` 41 (the place:
  pressed at 301 or 302 pixels down a 604 pixel pane, the next block at
  the same pixel, every run); with git_sync, git_actions, git_hunks,
  git_verbs, git_conflicts, git_gutter, tree_git_colors, preview_tabs,
  tabs, tab_viewers and compare, 803 checks, none failed, in 90 s;
  git_panel, git_review, git_hunks and git_actions again with MC_GUI_ROOT
  inside `linux/build`, 198, none failed. Seen failing without the
  feature: the acted place turned off, `git_review` failed 3 (the next
  block at 5170, the page at its top).
- **Needs the owner's eyes.** The preview tab's italics and replacement
  with real clicks, the pictures' look (tiny ones at pixel size, large and
  transparent ones, the frame), and the place after Stage or Discard on a
  real repository.
- **Left out.** The commit page (`initForCommitsWithRoot:`, per-commit
  keys, commits in the preview tab): GTK has none yet, so a click on a
  commit still shows `git show` in the one diff tab, which the next commit
  replaces; a Ctrl or Shift click on the graph now opens nothing, as on
  the Mac. The right click on selected lines (where #217 also records the
  place) and F7 are not on GTK's page. An animated GIF shows its
  first frame (the Mac animates). The pictures are sized to the pane as
  it was at the read, not again on a resize (the Mac lays them out again;
  GTK scrolls sideways). heic, webp and ico need their gdk-pixbuf loaders
  installed; without one the file keeps "A binary file: its bytes are not
  shown."

### 119. The hints' keys in colour, and what the search finds first (November 2026)

The owner's two decisions about the shortcut hints (2026-11-19). The
first changed twice on the way: Apple's symbols for every modifier
(⌃⌥⇧❖) were asked for, then ISO/IEC 9995-7's PC symbols (⎈ ⎇ ⇱ ⇲), and
then, as the owner had never seen ⎈ or ⎇, the words a PC keyboard prints
for Ctrl, Alt and Super, each in a colour of its own, with ⇧ for Shift.

- **What a user meets.** Ctrl+Shift+H shows its keys as "Ctrl ⇧ P",
  "Ctrl ⏎", "Alt Z", "⇧ ⇥", "⎋", "Ctrl Alt ⇧ Page Down": Ctrl, Alt and
  Super are words, each in a colour of its own the same all through the
  panel (on Dark Modern Ctrl orange #CE9178, Alt pink #C586C0, Super
  yellow #DCDCAA), the rest of a key in the keys' blue; Shift is ⇧, Enter,
  Tab, Esc, Backspace, Delete and the arrows ⏎ ⇥ ⎋ ⌫ ⌦ ↑ ↓ ← →; Home,
  End, Page Up, Page Down, Space and Menu are words; a thin space parts
  the pieces of a key, half a monospace cell (4 pixels at 10 points), and
  no "+". The
  colours come from the theme's syntax colours and follow a theme change
  at once (View > Theme, or the settings file). The palette, the menus,
  tooltips and the status bar keep "Ctrl+Shift+P". With a query in the
  search box, what it finds moves to the top of the first column, under
  its sections' headings (only those with a match, in their order), still
  marked, and everything else follows, dimmed: a heading whose rows all
  moved is not repeated below, "Hide these hints" comes with the blank
  line above it rather than under the last heading. The panel keeps its
  columns and never shrinks while searching, the list scrolls to its top,
  and emptying the box (Backspace or Esc) puts the list back exactly as it
  was. A query that finds nothing dims everything in place. Searching by
  name: "ctrl", "control", "cmd" and "command" find Ctrl; "alt", "opt" and
  "option" Alt; "super", "win" and "meta" Super; "shift" ⇧; "return" and
  "enter" ⏎; and "shift+ctrl+h" finds Ctrl ⇧ H in any order.
- **Code.** Core: `Commands::keyLabel(k, Platform::Linux)` (the new
  spelling, the Mac's untouched), `Commands::modifiersLabel` and
  `Commands::kLinuxKeyJoin` (new); `HintsSearch`'s Linux spelling
  (`linuxKeyNames`, `keyNames(Spelling)`, `glyphsNamed(w, Spelling)`,
  combinations joined by thin spaces) and the new
  `HintsSearch::bubbleMatches(items)` with `Item` and `Bubbled`, written
  for the Mac to adopt (its header says how). GTK: `ThemeCss.h`'s
  `hintsModifierColors(settings)`, `contrast`, `distinct`,
  `kHintsBackground` and `kHintsKeyColor`; `HintsColumns` keeps the rows
  it was given and lays them out in `orderFor(query)`'s order (`layOut`),
  `mark(query)` laying out again only when that order changed,
  `keyAttributes` (the spaces at half the keys' size, the modifier words
  in their colours), `setModifierColors`, `layouts()`; `HintRow::rest`;
  `HintsSearchBox::found` and `attributes` (the rest dimmed whole);
  main.cpp's `showHintsText` (the colours on every layout),
  `hintsQueryChanged` (the top, always), and `hintsRows`' keys written by
  hand (`keyed`, `with`: "Alt click", "Ctrl or ⇧ click", "⇥, ⇧ ⇥").
- **Traps.** A monospace font gives a thin space (U+2009) a whole cell, 8
  pixels in DejaVu Sans Mono at 10 points, where Ubuntu Sans gives it 2,
  and drawn in the desktop's font the first snapshot ran "Ctrl Alt S"
  together; a key's spaces are drawn, and measured, at half the keys'
  size, 4 pixels. The
  panel's background is #252526 on every theme, so a light theme's syntax
  colours (keyword #0000FF) are lightened toward white to 4.5:1 there.
  DejaVu Sans Mono lacks ⎇ (Pango fell back to DejaVu Sans), one more
  reason the words won.
- **Not done.** The Mac still dims in place; it adopts `bubbleMatches`
  in its own change, with CLAUDE.md's hints sentence. The panel itself
  stays dark on a light theme, as the Mac's.
- **Checks.** `hints_panel` (HintsPanelSuite.cpp, its header saying what
  it checks): the keys' spelling (no "Shift" and no "+" in 113 keys, "Ctrl
  ⇧ P", "Alt Z", "⇧ ⇥"), no glyph missing from the fonts
  (`pango_layout_get_unknown_glyphs_count`), Ctrl, Alt and ⇧ in their
  colours in every key, Light Modern's colours after a theme change and
  Dark Modern's back; "save" putting Save at the top of the first column
  under KEYBOARD SHORTCUTS, then TABS with Save all, then 124 rows of the
  rest, every one dimmed; "shift+ctrl+h", "control" and "qui" likewise
  (their first parts' headings those of the sections found in, in
  order); a space typed laying nothing out again; the list at its top
  after each query; nothing found dimming in place; Esc restoring the
  list exactly (text, columns, sizes, place). `keys` reads the new
  spellings. The core's `hints:key-labels`, `hints:search` and the new
  `hints:bubble`; linux's `make test` adds `theme:hints-modifiers` (every
  theme's three colours at 4.5:1 or better, apart from the blue and each
  other).
- **Numbers** (2026-11-19, this ThinkPad, cores 4 to 7, other agents
  building): `hints_panel`, `keys`, `terminal_keys`, `window_title` and
  `palette` 1,507 checks, none failed, in 11.9 s. A keystroke that finds
  other rows lays the list out again in 10.4 to 11.5 ms of CPU (bound 25),
  one that finds the same rows marks them in 5.2 to 5.8 ms (bound 8),
  each under a frame. `make test` 196,374 passed and 2 failed that touch
  none of this, under a load of 12: `usage:scanner`'s CPU bound (464 of
  400 ms) and `sessionweb:files`' video body; linux's `make test` 435
  passed; `meson test --no-suite gui` passed.

### 120. Tree drags reach the terminal and the editor; the server's popups in the theme's colors; edit boxes keep their typing through an app switch (the Mac's #212 and #223; November 2026)

The Mac fixed four things in #212 and #223 with no core parts. Each was
first checked on GTK with a new GUI check, run against the code before the
change; GTK had all four.

- **Tree rows dropped on the terminal and the editor** (`tree_drags`, real
  XTest drags in the harness's Xvfb, so GTK's drag source and the XDND
  exchange both run). Before: nothing reached the terminal (VTE's GTK 4
  build has no drop target of its own), and a file dropped on the editor
  typed its path into the text (GtkTextView's own drop target, as item 13
  said). Now `Terminal::dropped` (a GtkDropTarget on the VTE widget, a
  GdkFileList or a string, Copy, Move or Link) types each path quoted as
  the Mac's terminal does (the core's `TermDrop::paths`: backslashes, a
  space after each) as a paste, and the terminal takes the keyboard; the
  check reads `.../drags/a\ b\'s.txt X` back after typing X. On the
  editor, a capture-phase drop target ahead of the text view's
  (`Editor::openDropped`) opens each file through the link handler (a tab
  of its own, as VS Code does) and shows a dropped folder in the tree;
  dropped text is still the view's to insert.
- **The language server's popups** (`lsp_popups`, dark-modern and
  light-modern, each popup's drawn background against the theme's).
  Before: the hover box, the completion list, the signature hint and the
  references list were #181818 on both themes (the themes want #202020
  and #F8F8F8), from fixed CSS in `Lsp.cpp` and `LspPicker.cpp`. Now
  `ThemeCss.cpp` styles them from `FindRules::palette` (what the find bar
  reads): editorWidget.background, its border and foreground, the
  selection for a list's selected row, input.* and focusBorder for the
  rename field; muted text in their markup is the label's own color at
  70% (`fgalpha`), and the signature's active parameter is VS Code's list
  highlight by the theme's darkness. `Lsp.cpp` and `LspPicker.cpp` keep
  only the layout.
- **The rename field over the name** (`lsp_popups`). Before: under the
  name's line, over the next. Now no arrow, the box as wide as the field
  (a popover is centered on the rectangle it points at, so the box starts
  where the name does, less the field's 5-pixel inset), its top just above
  the line, in the editor's font, the old name selected.
- **Edit boxes through an app switch** (`lsp_popups` for the rename field,
  `edit_boxes` for the Markdown page's block editor and the tree's name
  box). Before: switching apps closed each box and its typing went, and
  the block editor said "Enter saves" with a Save button that only
  applied the edit. Trap: an autohide popover closes when another program
  takes the keyboard: on X11 GDK grabs the pointer and keyboard for it, so
  the click on the other program's window is its outside click (and under
  the grab the window does not even go inactive), and on Wayland the
  compositor dismisses a grabbing popup. `PopoverKeep::install`
  (`PopoverKeep.h`) makes the three not autohide and watches presses in
  their window while they show: a press outside calls the box's own answer
  (the rename fields cancel, the block editor applies its draft) from an
  idle, and the press goes on. Escape still closes them. The block editor
  now says "Enter applies, Shift+Enter for a new line", its button is
  Apply, Escape and Cancel are the only ways to drop a draft, and
  `Editor::commitPageEdit` applies one before a tab leaves (`stash`), the
  page turns to its source, Ctrl+S, Save All, closing a tab or the window
  (before the unsaved question) and quitting. Closed with the keyboard in
  it, it gives the keyboard back to the page, as on the Mac.
- **Trap: a key typed in a popover never reaches the window's
  accelerators.** Ctrl+S in the block editor ran no Save at all (before
  this change, too): GTK hands a key in a popover to the popover's own
  shortcut handling. The box's key controller now matches Save's keys
  (`gtk_application_get_accels_for_action("win.save")`, so keys.conf's
  too), applies the draft and activates `win.save`; `edit_boxes` checks
  the file then holds the draft.
- **Trap (the checks): a double-click soon after the last.** The page
  opens the block editor on a double-click only (`n == 2`); a second
  double-click on the same paragraph within GTK's double-click time is
  its third and fourth clicks, which select the line and open nothing.
  `edit_boxes` typed a word, pressed Escape and double-clicked again in
  under 0.4 s; it now pauses 0.5 s before each double-click, as
  `tree_clicks` does. Not a product problem: a hand is never that quick
  after Escape.
- **Trap (the checks): a popup's picture right after a theme change.**
  `lsp_popups` read a pixel inside the rename field on light-modern just
  as the field showed; in the long run (a busier machine) the box was not
  yet laid out at the new theme's sizes, the field's right end past the
  box's, and the read missed. It now waits until the field lies inside
  the box and the box is drawn.
- **The harness**: `MCGuiFocusElsewhere()` switches apps as a hand does
  with the mouse: a click on a 10-pixel window of the harness's own, on a
  second X connection, in the screen's far corner, then the X focus given
  to it. `XSetInputFocus(None)` alone left the window active here.
- **Mac-only, not ported**: ClickOutline's drag detection (GTK's click
  acts on release and its drag starts past the threshold, item 13),
  pictures with no file of their own dropped on the terminal (saved as a
  PNG in Mutible-drop), promised files, the paste question for dropped
  text (GTK's terminal asks none for a paste either), and the two-group
  double-click (the other group shows no rendered page on GTK).
- **Not checked**: a row dragged onto a folder row of the tree itself. The
  harness's drag, two rows up the same list, never landed (neither moved
  nor copied), before this change as after it; item 13 checked it with
  xdotool in Docker. A real drag, Alt+Tab under GNOME (X11 and Wayland)
  with each box open, and the popups' look need the owner's eyes.
- **Numbers** (2026-11-19, this ThinkPad, cores 4 to 7): before the
  change `tree_drags` failed 8 checks, `lsp_popups` 19 (the colors on
  both themes, the field's place, the field lost in the switch) and
  `edit_boxes` 16. After it, alone: `edit_boxes` 16 passed, `lsp_popups`
  43, `tree_drags` 9, `themes` 136 (five runs, each 136, none failed);
  the four together 204 passed, none failed, twice, and once more with
  MC_GUI_ROOT under linux/build. With tree_clicks, tree_focus,
  tree_edits, terminal_tabs, notes, lsp_actions, signature_help,
  completion_keys, second_server and markdown_side_edit as well: 683
  passed, none failed. `meson test --no-suite gui` and `make test` (349)
  pass.
- **Trap (the checks): `themes` read "the pane before" off the menu
  bar.** Its "the keyboard back where it was" failed 1 run in 8 alone on
  this branch (main: 5 of 5 passed): the check before View read the
  keyboard while it was still on an earlier menu's bar item, which
  MenuBarFocus gives back a moment after that menu closes, so the pane
  "before" was the bar's item and the keyboard went back to the editor.
  This change touches neither the menus nor MenuBarFocus; a longer
  stylesheet for each theme change is the likeliest reason the race shows
  more here. `openThemeMenu` now waits for the keyboard to be off the bar.

### 121. An HTML page opens nothing by itself (the Mac's #249; round 10's documents defect 2; November 2026)

On the Mac since #249, a navigation an HTML page starts by itself (a
`<meta http-equiv="refresh">`, a script setting `location`, `window.open`
without a click) is followed only to a web address while the page's
scripts run; anything else is refused and the status bar names it. A meta
refresh to `zoommtg://` had opened Zoom with no click, and `html.scripts`
never applied, a refresh not being a script. GTK now follows the same rule.

- **The core** (`src/HtmlPage.{h,cpp}`, `htmlpage:follow`):
  `HtmlPage::mayFollow(nav, scriptsRun)` is the Mac's `MCHtmlMayFollow`
  (the user's navigation, `byUser`: a clicked link, a submitted form, a
  gesture, always; the page's own only to http or https, and only while
  its scripts run). `loadsInFrame(scheme)` (http, https, file, data, blob,
  about) and `policy(nav, newWindow, scriptsRun, own, requested)`, GTK's
  answer at the policy check, are built on `decide`, which is unchanged,
  as are `started` and the rest; `refusedNote(url)` is the status bar's
  note in the Mac's words (cut to 80 characters). The Mac uses none of
  these: its .mm keeps its own copy.
- **Every path on GTK** (`linux/src/HtmlView.cpp`), and what it does now:
  - The policy check (`onPolicy`, `HtmlPage::policy`). The user's leaves
    the page for the link handler as before. One the page started by
    itself to a scheme no frame loads (zoommtg:, smb:, mailto:) is
    refused there, in any frame, before anything loads: WebKitGTK cannot
    tell the main frame at this check, and no frame could load it. One a
    frame could load is let through, as before (an iframe loads).
  - A new window (a NEW_WINDOW_ACTION, which a target=_blank meets, and
    the `create` signal, which `window.open` reaches): the user's opens
    as before; the page's own is opened only as `mayFollow` says, else
    refused, and a blank one is dropped (it used to go to the link
    handler, and so `about:blank` to the desktop's). WebKit's own popup
    blocker already stops `window.open` without a click
    (`javascript-can-open-windows-automatically` is off), as the check
    found: the script gets no window and `create` is never called.
  - The main frame's answer (`onResponse`, the response check, the one
    place WebKitGTK tells the main frame:
    `webkit_response_policy_decision_is_main_frame_main_resource`). A
    main-frame load away from the page's file that is not one it asked
    for is the page's own doing; it is ignored there, so the page never
    leaves and is never loaded again, and it goes to `followOrRefuse`.
    And nothing the page loads by itself is downloaded (a response
    WebKit cannot show is ignored; WebKitGTK's default is to download it).
  - A main-frame load that fails before any answer (`onFailed`: nothing
    listening, no such host, a missing file, a scheme WebKit cannot
    show): when the page started it, away from its file, it goes to
    `followOrRefuse` and no error page is shown. This is how a custom
    scheme ended before: allowed at the policy check, `zoommtg://example`
    failed with `WEBKIT_POLICY_ERROR_CANNOT_SHOW_URI` ("The URL can't be
    shown"), the default `load-failed` handler committed its error page
    at `zoommtg://example`, and that reached the COMMITTED path below,
    which gave it to the link handler, so to `GtkUriLauncher`. Seen in a
    trace (2026-11-19) with the policy check let through and `onFailed`
    left to the default: `load-changed` COMMITTED with the view at
    `zoommtg://example`, then, the page loaded again, the refresh asked
    for `zoommtg://example` once more.
  - `WEBKIT_LOAD_COMMITTED` (the old path): a load this page asked for
    earlier still puts the page back. A load the page made by itself no
    longer gets here; should one, it is followed or refused by the same
    rule (it used to go to the link handler every time, and loading the
    file again fired its refresh again, round after round).
  - WebKitGTK itself hands no address to another app: none of its
    library or helper processes import `g_app_info_launch*`,
    `gtk_show_uri` or `gtk_uri_launcher_*` (`nm -D`, 2.52.6). The one way
    out is the link handler, `Editor::openMarkdownLink`, which gives any
    scheme but http(s) and file to `GtkUriLauncher`.
- **Refused** ones go to `HtmlView::Host::refused`, wired in
  `Editor::showHtml` to `Editor::htmlRefused`, which main.cpp points at
  `flashStatus` with `HtmlPage::refusedNote`: "The page tried to open
  zoommtg://example by itself; mutible did not open it", or, for a web
  address with scripts off, "...; with its scripts off, it was not
  followed".
- **The harness**: `Editor::launchUriHook`, when set, takes what
  `openMarkdownLink` would give the desktop's handler; the harness sets it
  at startup to a recorder (`MCGuiLaunchedUris()`, the Mac's
  `MCGuiOpenedURLs`), so no check starts another app. `HtmlView::linkSpy`
  sees every address the page hands the link handler; html_page records
  them and lets only `file:` ones through, so a followed web address
  never loads in the browser pane.
- **The check** (`html_page`, its new steps): with scripts off, a meta
  refresh to `zoommtg://example` and one to `https://127.0.0.1:2/refused`
  (nothing listens there; no network is needed) open nothing, leave the
  page on show, and the status bar says the Mac's words; with scripts on,
  `location = 'target.html'` (a file: the Mac's rule, the old check said
  it opened) and `location = 'smb://x/share'` are refused and named, a
  meta refresh to `https://127.0.0.1:2/followed` reaches the link handler
  and nothing else does, and `window.open('zoommtg://y/meeting')` without
  a click opens nothing; a real click (XTest) on a link, and on a
  target=_blank link, opens its file in a tab through the link handler,
  and the desktop's handler got nothing all along.
  What the trace showed for each: the zoommtg refresh and the smb
  `location =` were refused at the policy check; `location =
  'target.html'` at the response check (`is_main_frame_main_resource`
  true for a file); both https refreshes failed with "Connection
  refused" and went through `onFailed`; `window.open` reached neither
  the policy check nor `create` (the script got no window).
- **Numbers** (2026-11-19, this ThinkPad, cores 4 to 7, MC_GUI_ROOT=/tmp):
  `html_page` 81 passed, none failed (78 passed and 5 failed while the
  web address was on port 9, below). With markdown_side,
  markdown_side_edit, markdown_side_sync, markdown_tables, tab_viewers,
  browser_pages, browser_tabs and browser_find: 299 passed, none failed.
  The core's `make test` 196,924 passed (`htmlpage:follow` new);
  `meson test --no-suite gui` and linux's `make test` (435) pass.
- **Trap (the check): a bad port does nothing.** The web address was
  first `https://127.0.0.1:9/`: WebKit never navigates to a port on the
  fetch standard's bad-ports list (9, discard, is one), so the refresh
  made no navigation at all, and nothing was followed or named. Port 2
  is not on the list.
- **Not changed**: a script's synthetic click on a link (`a.click()`), or
  its `form.submit()`, counts as the user's on both platforms (WebKit
  reports a link click or a form submission), so a page whose scripts run
  can still open another app; scripts run only once the user says so.
  (Item 123 closes it: such a click is refused on GTK, asked about on
  the Mac.) A real meta refresh, an app's scheme and the look of the status bar's
  note need the owner's eyes on the ThinkPad; a real Zoom was never
  started (no handler for zoommtg: or smb: is installed here, and the
  checks record the desktop's handler instead).

### 122. GtkTextView's line cache emptied after set_buffer too: semantic_tokens' signal 11 on main (November 2026)

- **What happened**: main's GTK job died with signal 11 in
  `semantic_tokens` at "the servers off again; the tab closed unasked" on
  two runs in a row, 44258729827 (ff0dcf0a, #165's merge) and 44260237099
  (2d0e5332, #166 on top of it). No GLib critical came before it. Against
  Ubuntu's debug symbols for CI's GTK (4.22.4+ds-0ubuntu0.2, amd64v3, from
  debuginfod.ubuntu.com) the stack is `_gtk_text_line_get_number` under
  `gtk_text_line_display_compare`, `g_sequence_insert_sorted`,
  `gtk_text_line_display_cache_get`, `gtk_text_layout_snapshot` and
  `gtk_text_view_snapshot`, from the frame clock's paint, with no frame of
  ours: not a language server's callback after its server or document
  went, but item 16's first trap again. A display of the closed tab's
  buffer stayed in the view's line cache through set_buffer, and the next
  frame sorted a display in beside it and read its freed line. #165 had
  put the scroll to the top after the cache was emptied; the step's
  `setText` had just replaced every line (none with a height yet) and left
  the caret at the end, the scroll asked for the caret's place
  (GtkTextView's `value_changed` updates the input method's spot), and that
  line's display, of a line the view had never validated, was one
  set_buffer does not drop. #168 (4f5a13de, merged an hour later) moved
  the scroll before the emptying, and no run since has crashed there: no
  main run's GTK job, no nightly soak, and no failed CI run of any branch
  from November 17 on (76 runs, every failed GTK job's log read) failed in
  `semantic_tokens` after 4f5a13de. #165's own PR run passed the suite,
  so on CI the old order crashed two runs of three.
- **Reproduced** with #165's order put back: `semantic_tokens` alone
  crashed 4 runs of 5 under `MALLOC_PERTURB_=165`, the same frames
  (symbolized against this machine's GTK). On main's order it passed 30
  of 30 (ten perturb values).
- **Change**: `CodeView::setViewBuffer` now empties the cache again once
  the view shows the new buffer (`emptyLineCaches`, the same never-applied
  tag, on the new buffer's table), so no display of the old buffer can
  stay, whatever laid its lines out after the first emptying. Emptying a
  cache whole reads none of its lines (GTK removes each display by
  pointer, gtktextlinedisplaycache.c), so it is safe even with a line
  already freed. With #165's order and this change, `semantic_tokens`
  passed 5 of 5 and `tab_close_now` 5 of 5.
- **Checked by** `tab_close_now`'s new rounds, which need no server: a
  2,000-line file scrolled 400 px, its text set again whole, the caret at
  its end and every tab closed in one step, then two frames drawn. With
  #165's order they crashed 5 runs of 5 under `MALLOC_PERTURB_` (which
  `meson test`, so CI, sets), and 3 of 3 in an AddressSanitizer build with
  `free_fill_byte` (a plain segfault: GTK is not instrumented), where
  `semantic_tokens` passed 3 of 3; with #168's order or this change they
  pass. With this change, `semantic_tokens` passed 30 runs alone (5 of
  them on one core under load), 3 under ASan with no report, and CI's
  first half in its order up to it twice (`MC_GUI_IN_ORDER=1`, once on
  two cores beside an ASan build); `tab_close_now` passed 18. That
  two-core run failed `large_file`'s 60 MB stall bound (529 ms of CPU for
  361); main's code failed the same bound beside the same build (435 for
  346), and three runs of each, alternated, measured alike (main 238, 435,
  345 ms; this change 165, 436, 301).
- **Trap**: anything that lays out a line of the buffer a view is
  leaving, after the cache is emptied (a scroll, a caret's or an iter's
  place), leaves a display that set_buffer keeps unless the line has a
  height. The second emptying covers it now; keep it after
  `gtk_text_view_set_buffer`.

### 123. A link to another app asks first, and a script's click to one is refused (the Mac's #254; November 2026)

Item 121 left one way out: a page whose scripts run can click its own
link (`a.click()`) or send its own form (`form.submit()`), WebKit reports
those as a link clicked and a form sent, like the user's, and so such a
page could still hand `zoommtg://` (any app's scheme) to the desktop with
no click. The Mac (#254) cannot tell a script's click from the user's, so
there every followed link to another app asks first, in a sheet naming the
app and the address. WebKitGTK can tell
(`webkit_navigation_action_is_user_gesture`), so GTK, agreed with the Mac
session, asks only about the user's own and refuses the script's unasked.

- **The core** (`src/HtmlPage.{h,cpp}`, `htmlpage:app-gesture`):
  `HtmlPage::follow(nav, gestureKnown)` says what becomes of a navigation
  the port has decided to follow: Open for a web address or a file (as
  before), and for another app's address (`opensAnotherApp`: any scheme
  but http, https and file) Ask when `nav.userGesture`, or always when the
  port cannot tell (`gestureKnown` false, the Mac's case), else Refuse.
  The question's words are #254's: `openAppQuestion(app)` ("Open in Zoom?",
  "Open in another app?" with no app known), `openAppDetail(url)` ("The
  link goes to zoommtg://example, outside mutible.", the address cut at
  80 characters) and `kOpenApp` ("Open").
- **GTK** (`linux/src/HtmlView.cpp`): the policy check's Open (`onPolicy`,
  a link, a form, a target=_blank) and `create`'s (`onNewWindow`) go
  through `follow(nav, true)`; `hand` now takes a `Follow`: Open to the
  link handler as before, Refuse to `Host::refused` (the status bar's
  `refusedNote`, "The page tried to open smb://x by itself; mutible did
  not open it"), Ask to `askToOpen`. That puts up `Question::ask` over the
  window (modal, transient for it) with the core's message and detail and
  the Mac's buttons in its order, Open then Cancel; Return is Open, Esc
  and closing it Cancel. Open hands the address to the link handler
  (`Editor::openMarkdownLink`, so `GtkUriLauncher`, or the harness's
  recorder); Cancel opens nothing. One question at a time: another the
  page asks for meanwhile is refused, as on the Mac. The app is the
  scheme's default handler (`HtmlView::appFor`:
  `g_app_info_get_default_for_uri_scheme`, its display name), a lookup
  that starts nothing. `followOrRefuse` (a page's own jump) goes through
  `follow` too, though `mayFollow` already lets only web addresses by.
- **Not ported: the Mac's box**, Refuse any more from this page. It is
  there so a page clicking its own link in a loop cannot hold the window
  with sheets; on GTK a script's click never asks, so only the user's own
  clicks can.
- **Unchanged**: http, https and file links (#253's rules, no question);
  a Markdown preview's links, on both platforms (`openMarkdownLink` gives
  zoommtg: to the desktop unasked, the Mac's `openURL:` the same).
- **The check** (`html_page`, its new steps; the spy now lets every
  address but a web one through, since the desktop's handler is the
  recorder): with scripts on, a page whose script runs
  `document.querySelector('a').click()` on its `smb://x` link at load,
  and one whose script sends its form to `mailto:someone@example.com`,
  are refused with no question (no window over the app's, `askingToOpen`
  false), nothing reaches the link handler or the recorder, and the
  status bar names each in the Mac's words. A desktop file for
  `x-scheme-handler/zoommtg` named "Zoom Check" (Exec `/bin/false`) goes
  in the scratch world's `XDG_DATA_HOME/applications`, the default in its
  `XDG_CONFIG_HOME/mimeapps.list`, both removed at the end, and the suite
  waits until `appFor` sees it. A real click (XTest) on a `zoommtg://example`
  link puts up "Open in Zoom Check?" over the window, with "The link goes
  to zoommtg://example, outside mutible." under it, Open left of Cancel,
  and nothing opened; Cancel opens nothing; a second click answered Open
  hands `zoommtg://example` to the link handler and the recorder once,
  the page still the file. Real clicks on an https link (the link
  handler, no question) and on a file link (its tab, no question) behave
  as before, and a Markdown file's `[join](zoommtg://example/md)`, clicked
  in its preview, reaches the recorder with no question, as before.
  What each showed: the script's click and form reached the policy check
  as a link and a form with `is_user_gesture` false, and the XTest click
  as a link with it true (the question came up), so the two are told
  apart as WebKitGTK documents. The form's address is
  `mailto:someone@example.com?subject=hello`; the status bar names it.
- **Numbers** (2026-11-19, this ThinkPad, cores 4 to 7): `html_page` 129
  passed, none failed, with MC_GUI_ROOT=/tmp and with a long root inside
  `linux/build` (removed after); markdown_side, markdown_side_edit,
  markdown_side_sync, markdown_tables, tab_viewers, browser_pages,
  browser_tabs and browser_find 218 passed, none failed. The core's
  `make test` 197,060 passed (`htmlpage:app-gesture` new); `meson test
  --no-suite gui` and linux's `make test` (435) pass.
- **Trap (the check): the X focus after a question.** With no window
  manager the X focus stays on the question once it closes, and the
  harness says so (a LEFT line, the app's window inactive); the steps
  after Cancel and Open give it back (`MCGuiFocusKeyWindow`).
- **Needs the owner's eyes**: the question's look over a real window, a
  real handler's name (Zoom, a mail app) from the desktop's defaults, and
  Return and Esc on it by hand. No real app was started: the checks
  record the desktop's handler.

### 124. Backups by window, and UTF-8 with stray bytes kept (the Mac's #247; November 2026)

The Mac's #247 (round 10's workspace items 1 and 2) on GTK, through the
core's rules: `Backups::plan` with a window and the reserved keys,
`Backups::draftPath`, `TabSessions::Window::backupKey`, and
`FolderSearch::decodeFileText`'s `Utf8Raw`. `../docs/notes/sessions.md`
has the rules ("Backups by window", "UTF-8 with stray bytes").

- **Before** (the new checks run against the GTK code without this
  change). Two windows on one folder, each with different unsaved text
  in a.txt, then killed: every GTK window planned with no key, so the
  first window back took the newest a.txt and every other backup of the
  folder, whichever window's they were; which text a window got followed
  their ages and the order the windows came back in, not the window that
  wrote it (the backups named no window). Until #247 the older a.txt was
  deleted unseen (`r.discard`); with #247's stopgap in `BackupStore` it
  waited on disk and the next window took it. A launch with one window on
  the folder got the newest a.txt and left the other on disk unshown:
  Don't Save asked about two files and the third backup stayed behind
  (and later suites' launches opened a window for it). A held document from the session's
  host removed any backup of its file a process that went had left,
  unread. A mostly-UTF-8 file with a stray byte read whole as
  Windows-1252 (`readsAsWindows1252`, item 102) until #247's core change;
  GTK took that change through `Document::readText` (the new
  `latin1_files` part passed against the old GTK code too), and this item
  adds its tooltip and Replace in Folder's note.
- **Now.** Every backup names its window (`Persistence::backupKey`, a
  `BackupStore::newId` kept in the window's record and taken back from it
  by `setBackupKey` in `newWindow`; `writeBackup` sets `Entry::window`).
  `restoreOpenWindows` reserves every record's key
  (`BackupStore::reserveWindowKeys`) before the first window opens, and
  `planForRoot(root, files, window)` hands the window and the reserved
  keys to `Backups::plan`: each window gets its own back, and nothing of
  another's. A second backup of a file the window brings back (a
  `Restore::conflicts` entry, or a backup of a file it holds from the
  session's host whose text differs from the held document's) opens after
  the session's tabs as "a (unsaved copy).txt" (`Backups::draftPath`):
  `Persistence::restoreConflicts`, the Mac's `restoreConflictingBackups:`,
  rewrites the backup under that name first (`BackupStore::move`, on the
  store's thread, `diskExisted` false, held by this process for the
  window), so it is never without a copy on disk, then makes the tab
  (`addTab`, `takeBackup`): unsaved, not on disk until Ctrl+S, never
  auto-saved (`autoSaveNow` now skips a buffer that was never on disk, as
  the Mac's untitled tabs). Its note when shown is "Unsaved changes to
  a.txt from another window, kept apart in a (unsaved copy).txt", and the
  window says the same once its tab on show is up. The only backup removed
  is one whose text is already the file's tab's.
  A UTF-8 file with stray bytes opens as UTF-8, its label the core's
  ("UTF-8, bad bytes kept", with a tooltip saying the bytes are saved back
  as they were), a backup records `file-encoding: utf8raw`, and a save,
  edited or not, writes the stray bytes back unchanged. Replace in Folder
  leaves such a file alone and now says "has bytes that are not UTF-8"
  (`notUtf8Reason`, Search.cpp).
- **Checks**: `backups` (its new part and three children:
  `_backups_two_writer` opens two windows on one folder, types different
  text into a.txt in both and b.txt in the second, and is SIGKILLed;
  `_backups_two_restore`, launched with no path, finds both windows back,
  each with its own tabs and text and no copy, all three backups still on
  disk, and is SIGKILLed in turn; `_backups_two_copy`, opened on the
  folder, gets a.txt with one text and "a (unsaved copy).txt" with the
  other, not on disk, its backup moved under that name, the note, and
  Don't Save leaves nothing); `latin1_files` (a UTF-8 file with 0xFF and
  0x80 opens as UTF-8 with the label, an unchanged save and an edit
  elsewhere both keep the file's bytes, the edit's arrow written once).
- **Left**: the copy tab is a plain unsaved tab (the Mac marks it
  `draftOf`); Find in Folder's streamed read of a large file still
  switches to Windows-1252 at the first piece that is not UTF-8 (round 10
  item 3, open on both ports).

### 125. A row of panel icons over the sidebar, and a link hint that shows (the Mac's #225; November 2026)

The Mac's #225 (round 9) on GTK: Source Control, Agents and Processes had
no visible way in, only their keys and the View menu; and a terminal link
said nothing about Ctrl+click until it was tried. The core already had
#225's parts (`Git::Decorations::changeCount`, `Procs::countServers`,
`workbench.sidebar-icons`).

- **What a user meets.** A row of four icons at the top of the sidebar,
  28 px tall in the sidebar's colours: File Tree, Source Control, Agents,
  Processes, each the icon theme's symbolic icon in the sidebar's text
  colour, dimmed but for the panel on show and the one under the pointer.
  Each tooltip names its panel and key as keys.conf leaves it ("Source
  Control (Ctrl+Shift+G)", "Agents (Ctrl+Shift+A)"; "Processes" alone,
  since Toggle Processes has no key on Linux; "File Tree" alone, as on the
  Mac, Ctrl+0 focusing whichever panel shows). A click shows that panel in
  the sidebar; a click on the icon of the panel on show gives that panel
  the keyboard; the tree's icon gives the tree back. The panel on show is
  underlined in VS Code's activity bar blue (#0078D4, the Mac's). Source
  Control's icon carries the count of changed paths (a pill in the same
  blue, "99+" past 99), Agents' an orange dot (#E8A33D, the terminal's
  waiting dot) while a terminal of this window waits, Processes' the count
  of servers listening for the window's folder, read at each of the
  process monitor's looks (every 10 s). The row hides with the sidebar
  (Ctrl+B) and with `workbench.sidebar-icons = off`, the panels then taking
  the sidebar's whole height. In the terminal, resting the pointer on a
  link for half a second shows "Ctrl+click to open" beside it (below the
  pointer, else above, always inside the terminal), in the language
  server's popups' colours; a move, a press, a key, a scroll or leaving
  the terminal takes it away, and a plain click still selects. OSC 8
  hyperlinks, which GTK did not open before (item 9 left them out), now
  open on Ctrl+click as the Mac's do (a web address in the browser pane, a
  file here that exists in the editor; javascript:, another host's file
  and the like are no link, whatever the text says); the rest's hint names
  the address on a second line, and with Ctrl held over one the address
  shows alone at once.
- **Code.** Core: `src/SidebarPanels.h` (new, header only: the panels'
  order, names and toggles' command ids, `badgeText`, `tip` in each port's
  spelling, `onShow`, `click`), `TermLinks::restHint` (the hint's words,
  "⌘-click to open" or "Ctrl+click to open", with an address). GTK:
  `SidebarIcons.{h,cpp}` (new: the row; each icon the first of a few names
  the display's theme has, as `TreeIcons` picks, re-resolved when the theme
  changes), main.cpp's `makeSidebar` (the row in a scroller with no bar
  over the stack of panels, in `sidebarHost`, which `WindowLayout` now
  holds as the sidebar), `updateSidebarIcons` (on the stack's
  `notify::visible-child`, keys.conf's change, the tree's git colours'
  `onChange`), `updateSidebarAgentsDot` (the attention board's handler),
  `countSidebarServers` (`ProcessMonitor::setSampledHandler`, new; and
  when the sidebar shows again), `showSidebarPanel`,
  `applySidebarIconsSetting`; `TreeGitColors::changeCount`;
  `ThemeCss.cpp`'s row, badge, dot and `.minicode-link-hint` rules.
  `Terminal`: `Link::address`, `hyperlinkAt` (VTE's
  `vte_terminal_check_hyperlink_at`, then `TermLinks::hyperlink`, then
  `TermLinkPath::resolve` for a file), first in `findLink`; the hint
  (`linkHint_`, a label in the terminal's overlay that clicks pass,
  placed by the overlay's `get-child-position`; `showLinkHint`,
  `hideLinkHint`, `armRestHint`, `showRestHint`, `kRestHintMs`); a
  capture-phase press of any button and any key on the window take it
  away; `hoverLink_` (the hand and the recheck now cover a hyperlink,
  which has no spans of ours: VTE underlines a hovered one itself); for
  the checks `linkHintText`, `linkHintWidget`, `linkUnderlined`,
  `cellCenter`. The harness: `MCGuiHoldModifier` (a modifier left down
  while the pointer moves); AppSeam's `sidebarIcons`, `sidebarIconsRow`,
  `sidebarStack`, `gitShown`.
- **Differences from the Mac.** The Mac gives no rest hint while a program
  takes the mouse; VTE does not say when one does, so GTK shows it there
  too, where Ctrl+click still opens the link (the capture-phase gesture
  claims it before the program sees it), so the hint stays true. The Mac
  draws its own symbols (SF Symbols); GTK takes the icon theme's, and
  Adwaita has no branch or processor picture: there Source Control is
  `media-playlist-shuffle-symbolic` and Processes `system-run-symbolic`
  (Yaru gives `emblem-shared-symbolic` and
  `utilities-system-monitor-symbolic`, Papirus `object-merge-symbolic`
  and `cpu-symbolic`); Agents is `starred-symbolic` everywhere.
- **Checks.** `sidebar_icons` (new, a window of its own on a scratch
  repository with real git): the four icons on screen in order, none
  taking the keyboard, each a name the theme has, each tooltip the
  registry's key ("Source Control (Ctrl+Shift+G)"), keys.conf giving
  Toggle Processes Ctrl+Alt+J and taking Source Control's key and the tips
  following, and back; the tree marked alone; the row above the stack, 28
  px; a new file making Source Control's badge 1, a second 2 (the pill
  mapped, its text); a real click on each icon showing its panel (the
  stack's child, the flags, the mark alone on its icon), a click on the
  icon on show giving the Source Control panel the keyboard back from the
  editor, the tree's icon giving the tree back, Ctrl+Shift+G marking and
  unmarking it; a notify marking the window's terminal waiting putting the
  dot on Agents, not on the first window's, and `TerminalAttention::typed`
  taking it away; a `python3 -m http.server` started in the folder outside
  the app making Processes' badge 1 at a look, and 0 once it is stopped (by
  its pid); Ctrl+B hiding and showing the row with the sidebar;
  `workbench.sidebar-icons = off` hiding the row, the stack at the
  sidebar's top, and on bringing it back with its mark and count.
  `terminal_links` (new on GTK; the Mac's suite of that name), real XTest
  pointer over a real shell's printf output: the rest's hint on a path,
  inside the terminal, no underline without Ctrl; a move off the link, a
  plain press (which opens nothing), Shift, and leaving for the tree each
  take it away, and a rest off a link shows none; Ctrl held underlines the
  path with no hint; on an http OSC 8 hyperlink the hint with the address
  on a second line, the address alone with Ctrl held, gone when Ctrl is
  let go, and Ctrl+click loading its page (from a server in the process)
  in the browser pane; Ctrl+click on a file:// hyperlink opening the
  file; a javascript: hyperlink over a URL's
  text neither underlined nor hinted, with or without Ctrl. The core's new
  `sidebar-panels:*` groups and `hyperlinks:rest-hint`.
- **Trap: a scroller with nothing focusable inside takes the focus
  itself.** The row sits in a `GtkScrolledWindow` (no bar) so a narrow
  sidebar clips it; with only unfocusable buttons inside, the scroller
  became focusable, and a tree row trashed under the keyboard sent the
  focus to it rather than to the next row (`tree_edits` failed nine checks
  until `gtk_widget_set_can_focus(row, FALSE)`). The same goes for any new
  scroller that is never the keyboard's.
- **Not ours: `tree_edits`' rename step.** Its "the tree kept its place;
  b_dir/f050b.txt selected" fails two checks on this ThinkPad on main
  (3136b81c, built apart from this change) exactly as with it: after F2
  the tree scrolls from 722 to 894. Left for the tree's owner.
- **Numbers** (2026-11-20, this ThinkPad): `sidebar_icons` 56 passed and
  `terminal_links` 27 passed, none failed, with MC_GUI_ROOT=/tmp and with
  a long root inside `linux/build` (8.6 s together); shuffled with
  neighbours, MC_GUI_SHUFFLE=1 over tree_clicks, tree_focus, git_panel,
  agents, agent_panel, processes, sidebar_icons, terminal_links, tabs,
  layout, owner_reports, maximize, attention, terminal_tabs,
  terminal_splits, terminal_draw, hints_panel, keys and sessions 2,364
  passed, none failed (119 s); MC_GUI_SHUFFLE=7 over sidebar_icons,
  terminal_links, browser_tabs, browser_pages, terminal_keys,
  terminal_find, terminal_marks, tree_git_colors, git_review, agents,
  processes, startup, themes, zoom and tree_edits 773 passed, 2 failed
  (`tree_edits`' rename, above). The first shuffled run found two of this
  item's own: `sidebar_icons` left an empty keys.conf where there was
  none, which `keys` (written first there) failed on, so it now removes
  the file it made; and `terminal_links` loaded a page that refused its
  connection, which WebKit then took back, so its page now comes from a
  server in the process. The core's `make test` 203,258 passed, none
  failed; linux's `make test` 435 passed; `meson test --no-suite gui`
  passed.
- **Needs the owner's eyes**: the row's look on the real desktop (Yaru's
  icons there, their size and dimming, the underline and the pill against
  each theme, a narrow sidebar clipping the row), the hint's look and
  place beside a real pointer, its half second, and Ctrl+click on
  `ls --hyperlink` and gcc's links by hand. Snapshots of the row (with a
  count of 2 and the dot) and of the hint over printf's output were looked
  at under the harness's Adwaita.

### 126. The session browser, first part: Show Session in Browser… and Copy Session Browser Link (the Mac's SessionBrowser; November 2026)

The Mac's View > Show Session in Browser… on GTK, without the LaTeX
typesets, the review pages and the page's Source Control, which are the
next bites. The core's `Shared::Web::Server` (SessionWeb.h) was already
built into the GTK app; nothing started one.

- **What a user meets.** View > Session Browser has Show Session in
  Browser…, Copy Session Browser Link and Remembered Browsers…, no keys (a
  submenu, since three more rows in View itself would pass a 768-pixel
  screen). Show asks Editable or Read-Only, the last answer first, then
  opens a fresh link with the system's handler; Copy puts one on the
  clipboard. A browser the app does not remember waits ("Waiting for
  approval in mutible") while a dialog on the window asks Allow, Deny or
  Always Allow, naming the browser and where it connects from (this
  computer with the connection's port, or the far end of an SSH tunnel).
  An editable page saves through the window's Ctrl+S, opens a file from
  its tree as a tree click does, closes a tab (never one with unsaved
  changes) and opens http and https addresses in the window's browser
  pane. The page shows only the window's tabs, its own terminals, its
  browser pane's tabs and which panes it shows.
- **Code.** `linux/src/SessionBrowser.{h,cpp}` (new): one server per
  window, its callbacks carried to the main loop with a generation number,
  the half-second report, the dialog, the remembered browsers and the
  Editable answer in `$XDG_STATE_HOME/mutible/session-browser.json`, and
  the SSH peer read from /proc/net/tcp{,6} and the user's /proc/PID/fd.
  SessionLink gained `connectViewer` (a browser's view as one more
  renderer of the in-process host, its messages routed by client id),
  `windowPanes` (the "browser" and "panes" records), `hostWindowId`, and a
  public `dispatch`. main.cpp: the three actions, the Host callbacks, and
  the server deleted before the window detaches. CommandsLinux and Keys
  have the three rows.
- **Differences from the Mac.** No question about editor.session-host:
  GTK always has a host. Nothing asks for attention when the app is in
  the background.
- **Checks.** Bite 2 adds the `session_browser` suite. This bite was
  smoke-checked in a private Xvfb with scratch XDG folders and a
  temporary hook (removed) that ran Copy: curl got the page (200, the
  cookies), the spent link 403, the cookie 200, a wrong Host 403; a
  socket client got "waiting", the dialog showed (looked at), Return
  allowed it, and the page's open, edit, save and close reached the
  window (the file on disk changed), on the in-process host and on a
  headless one (`MUTIBLE_HOST=socket`).
- **Needs the owner's eyes**: the dialogs' look on the real desktop, Show
  opening the default browser, and a page from a real SSH tunnel.

### 127. The session browser, second part: typesets, pages as pictures and Source Control refresh (the Mac's SessionBrowser; November 2026)

The three things section 126 left out of the page that need no new core
code: the LaTeX typesets, the pages drawn as pictures, and the page's
Source Control reading again when git changes. The review pages are still
to come (section 128).

- **What a user meets.** A page showing a `.tex` file shows its typeset
  PDF: from the window's own LaTeX preview when a tab has one (its last PDF
  at once, then "working" or the failure, and each later typeset as the
  preview makes it), else from a typeset the server makes for it in the
  background from the tab's text or the file (at most four kept, the oldest
  stopped first). With poppler the pages come as one picture each, so a new
  typeset keeps the reader's place; without it (the `pdf` option off) the
  page frames the PDF in the browser's viewer, and with no PDF viewer in
  the build the page says the window's preview makes it. A page showing
  Source Control reads again when a stage, commit or checkout happens in
  the window or anywhere in the folder.
- **Code.** `LatexPreview` (Latex.h) gained `setOnTypeset` (a callback for
  began, made a PDF, failed, with the preview's path), `typesetPdf`,
  `typesetFailure`, `typesetting` and `path`; Editor has a `typesetHook`
  that every tab's preview reports to, set in main.cpp after the editor is
  made (before that it crashed at startup: the editor does not exist while
  the tree's callbacks are set). `SessionBrowser` sets `Options::typeset`
  (carried to the main loop with the generation number, as every callback),
  holds the hidden previews (`Hidden`, a text buffer and a LatexPreview
  that is never shown) and has `typesetOf` and `gitChanged`; the Host has
  `previewOf` and `source`. `Options::typesetPageSizes` and
  `typesetPageRender` are poppler-glib functions in SessionBrowser.cpp
  (guarded by `MUTIBLE_ENABLE_PDF`): each call opens its own
  PopplerDocument from the bytes and draws with cairo on white to a PNG in
  memory, no GTK, sizes in points as the page's crop box shows it, the
  width and pixel limits the core's. `sessionGitChanged` in main.cpp is
  called from the Source Control panel's refresh (it learns of its own
  verbs there), the tree's activity callback and the tree's git colors
  changing (a commit or stage made elsewhere).
- **Differences from the Mac.** `Options::git` is left unset: GTK's
  GitRun has no git-finding step the panel uses that the core's runGit (the
  first git on PATH) lacks. The panel itself refreshes only while it or the
  review shows, so the page's refresh comes from the tree's watchers and
  the panel's refresh together, not from the panel alone. Pages are drawn
  with poppler where the Mac uses CoreGraphics, so anti-aliasing differs by
  a few pixels. A tab's preview that was made but never typeset is
  typeset when a page asks (the Mac's is made already typesetting).
- **Checks.** `session_browser` (SessionBrowserSuite.cpp) now covers this
  part with a stand-in tectonic (`MUTIBLE_TECTONIC`, a script that copies
  `tests/latex/fixtures/main.pdf`, counts its runs in a file, and fails on
  a marker in the .tex; the setting is put back). A `latex` for a file no tab
  holds reaches "ready" with two 612 by 792 pages, its PDF is 200 and begins
  with `%PDF-`, and `p=2&w=400` is a 200 PNG whose IHDR says 400 wide; a
  file the engine fails on reaches the page as "failed" with the engine's
  text; five untabbed files keep four typesets (`hiddenTypesets()`, added for
  checks), a kept one asked again runs nothing and the oldest runs again; with
  the .tex open in a tab the page's typeset is the tab's (no second run, no
  hidden one) and `compileNow` on the tab's preview reaches the page as
  "working" then a newer "ready"; and a commit made in the folder reaches a
  page watching git as a newer `git` message. A page follows the one
  document it last asked for, so the check asks for one file at a time (two
  asked together left the first at "working"). Open a tab through window 0's
  editor, not `MCGuiOpenFileKept`, while a second window exists. Not
  checked: real tectonic, and a commit made in a terminal.
- **Needs the owner's eyes**: a LaTeX document's page in a real browser
  with real tectonic, and the page's Source Control after a commit.

### 128. The session browser, last part: the window's commit and worktree pages (the Mac's SessionBrowser; November 2026)

The review pages section 127 left out. The core already reads and serves
them (`reviews` in the state, `review` with a `page`, SessionWeb.h and
SessionWebGit.h); the window only had to say which it has open.

- **What a user meets.** A page lists the window's worktree review pages
  (Compare with Main) beside its tabs, the one on show in each editor
  group marked, and shows one read-only: its header, its files and their
  changes as the window's page has them. Closing the tab takes it from the
  page's list at the next report.
- **Code.** `reportSessionWindow` (main.cpp) walks both editor groups
  (`groupDocuments`, `groupActive`), takes each tab whose `editorPage()` is
  a `ChangeReviewPage` in worktree mode (kind "worktree", its folder, its
  `title()`, whether it is the group's tab on show) and hands
  `Shared::Web::reviewsReportJson` of them to `Link::windowPanes`, which
  gained a third argument and keeps it beside the browser and panes
  (`Link::Panes`); `tabsJson` sets `reviews` when there are any, as the
  Mac's `w.reviews` does, so the half-second report sends it only when it
  changes.
- **Differences from the Mac.** GTK had no commit page yet, so only worktree
  pages were reported; the commit page (section 129) now goes in the same
  loop (kind "commit", the hash). The uncommitted changes' page is left out
  on both.
- **Checks.** `session_browser` makes a worktree one commit ahead of main in
  its scratch folder, opens its `ChangeReviewPage` with `Editor::showPage`,
  sees the state list it (kind "worktree", "sbt-wt vs main", on show, and
  the only entry), asks for it with `review` and `page` and gets the file
  the worktree added, closes the tab and sees `reviews` leave the state;
  the worktree folder and the repository are removed. Not checked: a
  commit's page, a page in the second group, and a page opened through the
  tree's menu (the suite makes the page itself).
- **Needs the owner's eyes**: a worktree page in a real browser.

### 129. The commit page (the Mac's initForCommitsWithRoot: and showCommit:title:; November 2026)

PARITY 2.9's last big piece: a click on a commit in the Source Control graph
opens that commit as a review page, as on the Mac, instead of git's text in
the diff tab.

- **What a user meets.** A single click on a commit shows its page in the
  preview tab, and each further commit clicked replaces it there; a
  double-click, or Return on the selected row, keeps it in a tab of its own
  (the Mac's Return keeps it too, so Return no longer shows the raw diff).
  The page has the subject, the byline under it (author, when in words, the
  short hash, "against its first parent" for a merge, with the file counts),
  the rest of the message, then every file the commit changed against its
  first parent (against nothing for a root commit), colored with the changed
  words marked, in one scroll. Plain files: Side by Side and Open on each,
  no Stage or Discard, no Undo Discard. A commit that changes no files says
  so. The tab reads "a1b2c3d Subject" and its key is
  `commit:<root>:<hash>`, so a commit already kept shows that tab.
- **Code.** `ChangeReviewPage` has a third constructor (panel, root, hash,
  subject) and `isCommit()`/`commit()`. `reload()` reads on the git thread
  with the core's `ChangeReview::readCommit` (header, diff-tree, one
  cat-file batch) into `Data::commit`; `updateSummary` fills the subject,
  the byline (`ChangeReview::commitByline`) and the body label. A commit page
  neither asks for the panel's reads nor follows them (a commit never
  changes), so its destructor leaves `onReviewRefresh` alone, and
  `reviewPageOf` (the uncommitted changes' page) skips it. `GitPanel` has
  `onReviewCommit(hash, subject, preview)` set in `makeGitPanel`;
  `showCommit(row, preview)` calls it (the old raw-diff path, `onShowCommit`,
  stays for a panel without the hook). `clickedRow` passes `preview` from
  `EditorTabs::treeClick`, so the second click of a pair keeps the page.
  main.cpp's `showCommitReview` builds the page and hands it to
  `Editor::showPage`, which finds a page by its key and replaces the preview
  tab. `reportSessionWindow` reports a commit page as kind "commit" with its
  hash.
- **Not done.** GTK restores no review pages with the session (it restores
  none at all), so a kept commit page is not restored. GTK has no line
  history, so its show-commit has nothing to point at.
- **Checks.** `git_review` (GitReviewSuite.cpp, last section): in a
  repository of a root commit, a commit with a body that changes one file
  and adds another, and an empty one, real clicks on the graph's rows
  (`GitPanel::graphRowWidget`): a single click shows the page in the preview
  tab (title "<short hash> <subject>", the subject, the byline with the
  author and the short hash, the body, both files, no Stage or Discard); a
  click on the root commit replaces it (same tab count, 1 file); a
  double-click keeps a page and a click on another row then leaves two tabs;
  a click on a kept commit shows its tab, no new one; Return keeps the page;
  an empty commit says "This commit changes no files."; closing the pages
  and the repository leaves nothing. `session_browser` (section 7b): an open
  commit page is in the state's `reviews` (kind commit, its title, onShow;
  the hash is in the id, `commit:<hash>`), `review` with that page answers
  pending, reviewread, then its files, and closing it empties `reviews`.
  The page's `headerTitle()` and `bodyText()` are for these checks.

### 130. The review page's F7 and the right click on selected lines (the Mac's stepBlock: and menuForRow:; November 2026)

The last two items of PARITY 2.9's review page.

- **What a user meets.** F7 and Shift+F7, with the page's text view focused,
  go to the next or previous change block across files, wrapping at the ends,
  the block's row a quarter of the way down the pane and the caret on it (the
  window's bell when the page has no block). A right click on selected
  changed lines of one file adds, above the text view's own Copy, Stage
  Selected Lines and Discard Selected Lines... in the Changes list, Unstage
  Selected Lines in the staged list; a right click on a conflict's rows in
  Changes adds Keep Current, Keep Incoming and Keep Both. A click with
  nothing selected, and every row of a commit's or a worktree's page (plain
  sections), keep the text view's menu as it was.
- **Code.** `ChangeReviewPage` has its own key controller (F7 is not a window
  action on GTK: the compare view takes it the same way) calling
  `stepBlock(forward)`, which asks the core's `ChangeReview::stepBlock` from
  the current block when that is on screen, else from the top row (the
  Mac's rule). A capture-phase right-click gesture (never claimed) runs
  `menuItems(row)`, which reads the selection's changed rows
  (`RowKind::Removed`/`Added`, their `oldLine`/`newLine`, the first file's
  only) and `installMenu()`, which sets the text view's extra menu (a
  section of `review.itemN` actions, the window's own extra menu kept below).
  `runOp` became `runOpOn(data, op, file, block, oldLines, newLines)`: a
  block uses `blockResult`, lines (`block < 0`) `ChangeReview::plan(.., true)`,
  `anyChanged` and `linesResult`; both then take the block buttons' road
  (the same question for Discard, the Trash copy and Undo Discard,
  `writeIndex`, the checks that the file and index are as read). The menu
  holds the read it was made from. `menuItems` and `runMenuItem` are public
  for the checks.
- **Not done.** The Mac's menu also has Stage Change, Discard Change, Open
  File and Show Side by Side for the clicked row; GTK has the buttons for
  those, so they are not repeated. Keep acts on the clicked row's conflict,
  as the Mac's does.
- **Checks.** In `git_review` (a section of its own, real keys and real
  right clicks, no fallback): F7 three times and around the end, Shift+F7
  back, each block's row on screen with the caret on it (check-only
  `caretRow()` and `selectRowText()`); a right click on one selected added
  line of a two-line change offers Stage and Discard Selected Lines above
  Copy, Stage puts only that line in the index ("Staged the selected lines
  of g.txt."), the staged list offers Unstage Selected Lines alone and the
  index is empty after it, Discard asks (Cancel changes nothing, Discard
  Lines takes only that line) and Undo Discard brings it back, and a right
  click with nothing selected has none of the items; on a merge conflict's
  rows Keep Current, Keep Incoming and Keep Both are offered and Keep
  Current resolves the file to the current side. Trap: a right click soon
  after another near it is a double-click to GTK and opens no menu, so the
  check waits 0.6 s before each (time is the condition).

### 131. Animated GIFs on the review page (the last of PARITY 2.9; November 2026)

- **What a user meets.** A GIF of more than one frame on the review page, in
  Before or After, plays at its own delays, as on the Mac and in the
  Markdown preview. It plays only while its picture is mapped and inside the
  text view's window; scrolled away, or the tab hidden or closed, it stops.
  A one-frame GIF and every other picture are drawn as before.
- **Code.** The git thread still only decodes the first frame's texture and
  keeps the bytes (`Data::read.extras`). On the main thread `pictureBox`
  calls `animatePicture`, which asks `animationOf` (a `GIF8` header, then
  `GdkPixbufLoader`'s animation, not `is_static_image`) and, for an animated
  one, hangs a `PicAnim` (animation, iterator, timer, frame count) on the
  `GtkPicture` with `g_object_set_data_full`: it is freed with the widget
  when the page lays out again or closes. The picture's `map` starts a
  `g_timeout_add` at the frame's delay (at least 20 ms) and `unmap` removes
  it, as MdPicture does; each tick advances the iterator, and when the
  picture is inside the view (`gtk_widget_compute_bounds` against the text
  view) sets the new frame's texture, scaled to the size the still was drawn
  at, with `gtk_picture_set_paintable`; off screen it waits 100 ms and steps
  nothing. A last frame with no delay ends the timer. The deprecated
  gdk-pixbuf animation calls are silenced as in MediaView.
- **For the checks.** `imageFrames()` (frames stepped after the first per
  picture, -1 for a still) and the static `runningAnimations()` (armed frame
  timers in every page).
- **Checks.** `git_review` has a section for it: a repository with a
  two-frame GIF and a one-frame GIF committed and changed to others, the
  GIFs written by the suite from bytes (GdkPixbuf cannot write GIFs; the
  loader's own word that they do and do not animate is checked), the page
  with four pictures, After of the animated one stepping to frame 1 and then
  three more, the still ones at -1 with exactly two timers, and after the
  tab closes no timer (one short pause, a negative check).
- **Not done.** Pictures scrolled away keep a 100 ms wake-up timer while
  the page is mapped, which does no drawing.

### 132. Tabs save through the host (stage 7; the Mac's writeTextThroughHost:, tab:writtenByHost: and tab:dirtyFromHost:; November 2026)

The Mac's "tabs save through the host" (stage 7's second write, its #294)
on GTK: docs/notes/session-host.md, "GTK's tabs save through the host
(stage 7's third write)".

- **What a user meets.** Nothing new on screen: Ctrl+S, Save All, the
  close question's Save and auto-save write as before, with the same
  questions, the same failure dialog ("Could not save “x”") and the same
  note when a Windows-1252 file is saved as UTF-8, but the session's host
  writes the file. What changes: an editable page of a headless host
  (`mutible host`, `attach --web --edit`) may now save a file a GTK
  window shows, and the window's tab takes that save as its own (clean,
  no question at the next Ctrl+S, the file watch not taking it for a
  change outside); and the tab's dot follows the host's count while the
  tab is the host's text.
- **Code.** `SessionLink::Link::makeClient` says flag 16 in every Hello
  (`setSavesThroughHost`) and hears DocDirty and the host's writes,
  handed to the document by its id (`Link::listen`, `DocNotices`, which
  `Document` implements; it enrols when made and on `rehost`).
  `Document::save` keeps its two refusals (a link to nothing, a file not
  writable), then, when `hostSavesFile()` (a host whose Welcome has flag
  2, not lost, not detached), `saveThroughHost`: the turn's step closed,
  an untitled buffer's path told first (put back on failure),
  `requestSave` with overwrite and the encoding's name, `settle(10000)`
  (the client's wait, no main-loop turn), then the encoding written, the
  host's note, the text the file holds (the buffer's when in step, else
  the file's) and `rememberDisk` with the answer's stamp; no DocSaved
  after it. Refused or unanswered: false with the host's sentence. A host
  lost meanwhile: the window's own write. `hostWrote` and `hostDirty`
  keep what they heard and take it at the top of the next turn
  (`takeHostWrite`: the path and the stamp must still be the ones
  written; `takeHostDirty`: `edits_` unmoved, not untitled, gone, unread
  or loading). `DocumentHost::documentWrittenByHost` runs the editor's
  save tail, now `Editor::tookSave` (a preview tab kept, the LaTeX
  previews and the language server told, the strip and the title).
  Where the saved text moves without a host write the host is told
  (`tellHostSaved`): the window's own write, a clean tab following the
  disk and Reload (`applyDiskText`), a change on disk asked about, a held
  tab read (the last two with the file's text, DocSaved flag 1).
- **Traps.** The host acknowledges an edit before it sends the DocDirty
  that edit caused, so a reload's edit followed by its DocSaved reaches
  the window as "unsaved" with nothing unconfirmed, before the DocSaved's
  "saved" (found by reading, then seen on the Mac): the client now
  fences it (#299, `Client::saved`), and `hostDirty` and `hostWrote`
  judge by the document's own unconfirmed messages
  (`Client::unconfirmed(doc)`); the stopgap `toldSavedAt_` went. GTK's own
  compare runs on every edit, so the host's count only matters where
  the two disagree; the tells above are what keep them agreeing.
- **Not done.** Save As from a page, and the host following the disk,
  as on the Mac (session-host.md's "Still to do").
- **Checks.** `host_reattach`'s stage 7 part, on a fresh headless host (its header):
  a page's guest over the socket saves a file the window shows with typing
  in it, the window's tab and its strip dot clean and in step with the
  file's stamp, the host counting it saved; the window types and saves
  (Ctrl+S) with no question and the guest hears the host's write with the
  file's stamp; a UTF-16 file saved from the window byte for byte with its
  mark; an agent's write, then Keep Mine and Ctrl+S, the window's text
  written over it; a directory where the file was, which the host refuses
  ("is not a regular file on disk"), the save dialog showing that sentence
  and the tab still unsaved; the host killed, then Ctrl+S written by the
  window itself. Not checked: an untitled buffer's Save As refused (its
  path put back), which needs the file chooser.

### 133. The in-process host watches the disk for the page (stage 7; November 2026)

- **What.** `Link::startInProcess` (linux/src/SessionLink.cpp) calls
  `SessionHost::watchDisk(true)` before the first renderer connects, as
  the Mac's `SessionLink.mm` does, so View > Session Browser's page sees
  its tree and Source Control follow the disk (FsChanged) with no refresh.
- **What it starts.** Nothing for a window with no page served: the
  watcher's roots are a headless host's folder and the roots renderers
  name in a `watch`, and the in-process host has no folder. The window
  keeps its own tree and LspWatcher monitors and follows its tabs itself;
  its client asks for no Hello flag 32. A window on a headless host is
  unchanged (that host decides its own watching).
- **Checks.** `session_browser`, section 6b: the page's `watch` of the
  root and a `list`; a file made in the folder arrives as a new `tree`
  listing, the file removed as another, a tracked file changed in the
  repository as a newer `git` message, none asked for. The made file is
  made only after `diskWatchSettled` reports the watcher on more than
  no folder (`Link::inProcessHost`, check-only), since its first walk is
  asynchronous. The page's notice may also come from the window's own
  monitors (since #283 they call `SessionBrowser::gitChanged`), so the
  check proves the host's watcher is live, not that its notice was the
  one heard. A probe that read the wrong window's link (the second
  window, not the page's) once showed "0 folders".

### 134. Sessions bring back the review and commit pages (November 2026)

- **What.** The folder's session and a window's record carry `pages`
  (`TabSessions::PageTab`, kinds "review" and "commit") and `activePage`,
  as the Mac's `pageTabOf:into:` and `tabForRestoredPage:` do.
  `Persistence::session()` asks the host's `pageRecord` hook about every
  tab (main.cpp's `reviewPageRecord`: the review, or a commit with its
  hash and the tab's title, as the Mac keeps it, from which
  `restoreReviewPage` takes the short hash off again for the subject;
  never a worktree's page) and sets `at` to the tab's
  place counting files and pages. `restoreSession` makes the pages after
  the files through `restorePage` (`restoreReviewPage`: the same
  `wireReviewPage` hooks a click gets, `Editor::addPageAt` inserting at
  `at` without taking focus), and the page that was on show is shown
  instead of a file. A session with only pages is no longer dropped by
  restoreSession's early return.
- **Left out.** Comparisons (kind "compare"; another bite) and worktree
  pages. A page already in the window (one review, one page per commit) is
  left out before it is made: a second review page would clear the first's
  `onReviewRefresh` as it is destroyed.
- **Checks.** `sessions`, the last section and the children
  `_pages_first` and `_pages_again`: a git folder with a commit and an
  uncommitted change, a file, a kept commit page, a file and the review
  page on show are saved with `at` 1 and 3 and the review as `activePage`;
  with both pages written twice in the record the next launch has the four
  tabs in the same order, the review on show and read, and the commit's
  page read when shown.

### 135. Comparisons come back with the session, made again from their recipe (the Mac's MCComparison leftPath/leftRev/rightPath/rightRev; November 2026)

A comparison is kept in the session as `TabSessions::PageTab` kind
"compare" (docs/notes/sessions.md, "Tabs that show no file come back made
again"), beside item 134's review and commit pages.
`Persistence::session()` asks `Editor::comparisonPage` about each tab
before the host's `pageRecord`; `restoreSession` hands a "compare" page to
`Editor::remakeComparison(page, at, shown)`, whose tab comes in at `at`
once a thread has read the two sides (on show then when it was the page on
show; the file shown last is shown meanwhile).

- **The recipe.** `CompareSpec::left` and `right`, each a
  `TabSessions::ComparedSide {path, rev, label}`: rev "" the file on disk,
  ":" the index, else a revision. An empty path is no recipe. Set where the
  Mac sets it: the review page's `sideBySide` (index against disk for an
  unstaged file, HEAD against the index for a staged one, a commit's parent
  against the commit, "<hash>^" for a root commit) and `mutible diff` (a
  revision against the disk, a file against a file). Left out as on the
  Mac: a worktree's page, a conflict's two sides, IdeLinkServer's and
  AgentWorktrees' comparisons.
- **Reading it.** `Editor::comparisonPage(const Document*, PageTab*)` (static)
  gives the "compare" record (title, the file name the colors go by, both
  sides with their labels; `at` is for the caller), or false for a tab with
  no recipe, a proposed change (`decisionPrompt`, a decision), the agents'
  preview tab (`mutible diff` without `--keep`), or a revision
  `TabSessions::isComparedRev` refuses.
- **Making it again.** `Editor::remakeComparison(page, at, show, done)`:
  the two sides are read on the worker thread (`sideText` in Tabs.cpp: the
  repository's `<rev>:<rel>` from the file's realpath's top level, or the
  file; a side it lacks, not text, or over 16 MB reads empty), lined up
  there as `showComparison` does, and the tab is inserted at `at` (clamped),
  put on show only when `show`. `showComparison` and it share
  `openComparison`; the window gone meanwhile drops the tab, as before. A
  tab made again has `spec.path` set to the right side's path when that
  side is the disk.
- **Checks.** `git_review`: Side by Side on a staged file, an unstaged file,
  a commit's file and the root commit's carries the recipe, and
  `remakeComparison` from `comparisonPage` gives the same texts, labels,
  title and row counts, not on show. `control`: `mutible diff` carries
  HEAD and the disk (none for the preview tab, which `--keep` turns into
  one that remakes) and the file against file. `idelink`: an agent's
  decision comparison has no recipe.

### 136. Checks for the page's Quick Open and Find in Folder (stage 7; November 2026)

- **What.** The Mac's #303 gave the session's browser page Quick Open and
  Find in Folder as host reads on workers (`src/SessionSearch`,
  `ReadWorkers`). GTK needed no code: its in-process host serves them as
  they stand, the worker answers reaching the page through the web
  server's `takeStreams`, run by the loop that serves the page.
- **Checks.** `session_browser`, section 4b, on the read-only page's
  socket (the GTK suite speaks the page's websocket and runs no
  JavaScript, so the page's own keys and clicks stay the Mac's checks): a
  `files` message for "sbq" lists both of its files, the nested one too,
  once the listing is no longer under way (not an empty query: the page
  shows only its first rows, and CI's folder held 235 files by then, so
  sbq/ fell past them; that was a FLAKY line on the PR); a query ranks them with the matched
  letters (`pos`) and opens nothing in the window; a `search` answers in
  `found` rows from both files with their highlights and the host's
  counts (2 files, 3 matches), the window opening nothing; a one-letter
  query is answered `short`. The files are made in a folder of their own
  and removed at the end.

### 137. bash's prompt without vte.sh's marks: a line typed after a resize is drawn whole (November 2026)

- **What failed.** The soak's `agent_panel` timed out in "the shell is up"
  in 2 rounds of 3 (run 44898401650, and its `agents` failures after it
  in round 2 followed from it). The pictures show the prompt with the
  cursor on the "e" of "project$" and no `true` anywhere: the line typed
  had been echoed in the wrong place and written over. CI's prompt is 92
  columns; the terminal had changed width under a prompt that wrapped at
  one width and fit at the other.
- **Cause.** Debian's and Ubuntu's `/etc/bash.bashrc` reads VTE's
  `/etc/profile.d/vte-2.91.sh` for an xterm TERM, and vte.sh wraps PS1 in
  OSC 133 marks of its own (`\[\e]133;D;$?\e\\\e]133;A\e\\\]` before,
  `\[\e]133;B\e\\\]` after) when nothing sets PS1 after it: a `.bashrc`
  without a PS1, or the GUI checks' empty home. readline counts a wrapped
  prompt's invisible bytes by row as it drew it, so after a width change
  every redraw put the cursor as many columns off as the marks' bytes.
  #285 kept a new view's first shell from drawing at 80 columns; this was
  the same miscount at any later width change.
- **Fix.** `linux/resources/mutible-bash-integration.bash` takes those two
  marks out of PS1 after `~/.bashrc`, in vte.sh's exact text and only when
  the integration is on (with the nonce). Ours print from PROMPT_COMMAND
  and PS0 and were never in PS1, and the host believes no mark without
  the nonce, so vte.sh's did nothing for mutible.
- **Checks.** `make test`'s `bash:integration`: a `.bashrc` that leaves
  vte.sh's PS1 comes out as the prompt alone, and without a nonce it is
  left as it was. `shell_integration`: PS1 has no `133;`, and a prompt as
  long as the line less six, drawn at font scale 1.5 (two rows) and then
  at 1.0 (one row), takes `echo typed-intact` whole after its `$`. With
  the old script that section fails both ways: PS1 has the marks, and the
  last line reads `.../guitest-XXXX/echo typed-intact`, the typed text
  over the prompt's end.

### 138. tree_clicks brings its folder on show through the tree, not over a keep (November 2026)

- **What failed.** The soak's `tree_clicks` (run 44898401650, round 3,
  seed 3448309052, right after `session_browser`) timed out in "its row on
  show": the picture shows the tree scrolled near its end, though the step
  before had scrolled the list to row 0.
- **Cause.** `session_browser`'s last step removes four files at the
  folder's top level; their rows go a moment later and `FileTree::keepRows`
  keeps the tree's place (300 ms and four layouts, up to 5 s under load),
  undoing any scroll to the top it did not ask for. Only the tree's own
  scrolls and the user's wheel and keys end a keep; the suite's
  `gtk_list_view_scroll_to` was neither.
- **Fix.** `tree_clicks` reveals its folder with `FileTree::revealPath`
  (which ends the keep), at its start and at its end. A check-only
  `FileTree::keeping()` says whether a keep runs.
- **Checks.** `tree_clicks` now makes a keep on purpose: eighty files at
  the top level, the tree scrolled to its end, four removed, and once
  `keeping()` is true the folder is revealed and must come on show. With
  the old raw scroll there it fails every run ("its row on show"), with
  the reveal it passes. It removes its files at the end and waits until
  the tree keeps no place. Ran alone three times, after `session_browser`,
  and with the tree suites in order and shuffled (`MC_GUI_SHUFFLE=59`).
  The in-order list `session_browser tree_clicks tree_icons tree_stall
  tree_focus tree_drags ...` fails `tree_drags` "plain.txt opens in the
  editor" with main's `tree_clicks` too (no one suite before it does it
  alone); that is a separate order dependence, not this change's.

### 139. Alerts in their colors and the CSV header row (November 2026)

- **What.** The Mac's a85f1a85 put GitHub's alerts, emoji shortcodes, front
  matter and the CSV header row in the core. Emoji and front matter come
  from the core parser and needed nothing here. Two pieces were drawn on the
  Mac only.
- **Alerts.** `Editor::ensureTags` makes `md_alertN` (the text, upright, in
  the editor's text color) and `md_alerttitleN` (bold, GitHub's color for
  kind N = 1..5, note to caution) after `md_quote`, so they win over it;
  `applySettings` colors them, light or dark by `isDark()`. `Markdown.cpp`
  applies them from `MdRun::alert` and `alertTitle`. Not done: the left bar
  (this port's quote has none) and the icon before the title (an icon in a
  GtkTextView is a child widget on an anchor, and none of the symbolic
  icons was checked to be in every build environment). Suite
  `markdown_alerts`.
- **CSV header.** `CsvTable::show(text, delimiter, path)` takes the file's
  choice from `$XDG_STATE_HOME/mutible/csv-headers.txt` (one `1 /path` or
  `0 /path` line a file, the 500 most recent), else
  `Csv::looksLikeHeader`. The grid's model starts at row 1 under a header,
  and `rowOf` is the file's row, so `rows()`, `cell()` and `setCell()` stay
  the file's. The names are the column titles, bold by a CSS class on the
  view (the letters of a column with an empty field are bold too, a
  GtkColumnView title has no font of its own). A right click opens a
  popover with a check, First Row Is Header; `headerMenuItem()` is it, for
  the checks. A reload from an edit keeps the choice. The header's names
  have no letter tooltip. The guess leaves a first row with an empty field
  alone (the core's rule), so the reader turns it on. Suite `csv_header`.

### 140. The status bar's branch (the Mac's 03e1c61d; November 2026)

- **What.** The left end of the status bar shows the branch, as VS Code's
  bottom left: `StatusBranchItem.{h,cpp}`, a box of two flat buttons, the
  name (the short hash when detached, `*` with uncommitted changes) and the
  counts (`↑2 ↓1`, hidden without an upstream). The words are the core's
  `Git::statusBranch`; the tooltip is its `tip`; the item hides outside a
  repository. No glyph before the name (the Mac draws an SF Symbol; the
  icon themes have no branch name to count on).
- **Where it reads.** `TreeGitColors` already runs `git status
  --branch` for the tree's colors (`decorationArgs`), so it now also keeps
  `Git::statusBranch` of that status and calls `onBranchChange` on the main
  loop when it changed (to not shown too, when the folder leaves a
  repository). The item runs no git of its own.
- **After a sync.** A fetch, pull or push moves only refs, which the
  tree's watches miss (and the panel does not refresh while hidden), so
  `GitPanel::onSynced` (end of `finishSync`) makes `TreeGitColors` look
  again; without it the counts stayed `↑1 ↓0` after the item's own Sync.
- **The clicks.** The name calls `GitPanel::chooseBranch()` (the branch
  picker in the palette), the counts `GitPanel::sync()`, the panel's own
  Switch Branch and Sync Changes.
- **Checks.** `status_branch` (`StatusBranchSuite.cpp`): hidden in a
  folder outside git; `main` and `↑0 ↓0` in a clone of a bare remote; `*`
  after an edit once the status is read again; `↑1 ↓0` after a local
  commit; a click on the counts syncs (level again, the bare remote has the
  commit); a click on the name opens the picker; detached HEAD shows the
  short hash.

### 141. New File and Save As from the session browser's page (stage 7; November 2026)

- **What.** The Mac's #307 let an editable page make files and name them.
  GTK's page now has both: `SessionBrowser::Host` gains `newFile` and
  `saveAs`, set in `main.cpp` (`newSessionFile`, `saveSessionDocumentAs`),
  and `SessionBrowser.cpp` fills `Options::newFile` and `Options::saveAs`
  from them, so the hello says `canNew` and `canSaveAs`. Both options come
  on the server's thread and hop to the main loop (the `later` helper,
  which drops them for a server stopped meanwhile); the answer is
  `Server::savedAs(ask, ok, why)` called there.
- **New File** is `Editor::addUntitled`: a document with no path on the
  host, a tab named Untitled-N (`Backups::untitledPath`) and on show; the
  page follows the window's tab, so it is told no `newfile`.
- **Save As** is `Editor::saveDocumentAs`: `Document::setPath(path, false)`
  (a new argument, `tellHost`) names the tab, `saveDocument` writes it
  through the host as Ctrl+S does (`saveThroughHost` tells the host the new
  path first, and puts the old one back if the write fails), and a failure
  puts the tab's name back. The core has asked about a file there already
  and held the path inside the folder; a file open in another tab is
  refused, as on the Mac.
- **Checks.** `session_browser`, section 5b, on the editable page's
  socket: the hello's `canNew` and `canSaveAs` (and neither on the
  read-only page's); `newfile` makes an untitled tab on show, listed in the
  page's state; text typed through the page, then `saveas` over a file
  there answers `exists` and leaves both alone, `../x` is `name` and a
  dotfile `dotfile`, a new name writes the typed text, the tab names the
  path and is clean, and the page's state lists it under the new path; with
  `overwrite` another file is replaced and the first left as it was.

### 142. tree_drags after tree_focus: a drag's target above its row (November 2026)

- **What failed.** `MC_GUI_IN_ORDER=1 ... MC_GUI_SUITES="tree_stall tree_focus tree_drags"`: plain.txt never opened after its drop on the editor (the second drag; the first, onto the terminal, worked). Each suite alone, and tree_drags after either one, passed.
- **Cause.** A test-harness bug, not the product. `pointOnRoot` (tests/gui/Input.cpp) took any negative coordinate as "the widget's middle". tree_focus leaves the tree scrolled, so plain.txt's row sat at root y 618, below the editor's middle (y 266); the target's point in the row's coordinates was y -352, became the row's own middle, and the pointer went across at y 619 and let go over the tree and the terminal's edge. A drop target on the window saw every motion at y 575 to 621 and the editor's target never an enter or an accept. In a fresh tree the row is above the editor and the point is positive.
- **Fix.** Only exactly -1 means the middle (the click calls pass -1). Checked: the failing order three times, the seven tree suites in order and with `MC_GUI_SHUFFLE=67`, and the longer order with session_browser, tree_clicks and tree_icons before.

### 143. The find bar in a large text, in stages (November 2026)

- **What.** The Mac's 96a45e14 and 11d4221f: a text of 1M characters or
  more is searched in stages through the core's `TextScan`, marks are drawn
  only for the query and text as they are, and a word seeded in a file over
  20 MB waits for a key. GTK had none of it: `FindRules::findAll` matched
  every line on a worker, the old query's marks stayed until the new
  results came, and the seed was always searched.
- **Stages** (`FindBar::stagedWork`, `stagedArrived`). The lines on screen
  (ASCII text only, where a character is a byte), the first match from the
  caret (selected at once, "1 of …" when found from the top, else "? of
  …", and "N of …" once a part of the count reaches it, with the Mac's
  tooltip "N matches counted so far"), then the matches from the top, at most `kMatchLimit` kept and sent
  every 100 ms. Each is a `Stage` message through `g_idle_add`; it applies
  only when its job's cancel flag is clear and the query generation and text
  version are those it was made for. An all-ASCII text (`Text::prepare`) is
  scanned as bytes, else as UTF-16 in blocks with the matches turned to
  characters once. A scope (Find in Selection) keeps the old whole-text
  search. Find Next while counting (`navigate`) scans from the selection on
  its own worker; typed before the first match it runs after it.
  `resultsCurrent()` is false while `counting()`; `marksCurrent()` is what
  painting reads. **Different from the Mac:** the count stops at 20,000
  ("3 of 20000+", GTK's rule for every search), where the Mac counts all.
  The buffer is still copied once per version on the main thread
  (`currentText`); GTK has no held file bytes to search in place.
- **Marks and the seed.** A staged search empties the held matches and
  repaints at once; a small text keeps its marks until results come. The
  seed (`holdSeed`, `seedPending()`, `releaseSeed`) puts the word in the
  box, searches nothing, shows an empty count with a tooltip, and Return
  (or a key, or an option) searches it, starting at the word's own
  occurrence so Return lands on the next.
- **Checked.** `large_file` gains a 22 MB log (the seed waits, Return lands
  on line 3's "2026", "3 of 20000+", the old marks gone after a new query,
  NEEDLE-4242 on the last line selected with its first match under
  `GCHECK_TIME(max(150, 8 x a memchr pass))` of the worker's CPU, "1 of 1",
  a match on every line with Find Next typed at once, main-thread stalls)
  and a 1.3 MB text with accents (characters, not bytes). First match on the
  last line 7 ms of worker CPU against a 1.3 ms memchr pass; 20,000 matches
  counted 5 ms; longest main-thread stall 58 ms (the seed's search, which
  copies the buffer), 12 to 14 ms after. `find_bar` 102 passed,
  `large_file` 115 passed. Not checked: a file of 250 MB (the harness's
  22 MB is the smallest over the seed's 20 MB), real keys and the look.

### 144. Keyboard and accessibility: Quick Open, Replace in Folder, graph rows (November 2026)

- **What.** The Mac's 3c02cbe0, from the task benchmark, in three parts.
- **Quick Open.** Not already right on GTK: with the tree holding the
  keyboard before Ctrl+P, the pick opened and typing still went to the tree
  (the palette closes first, giving the keyboard back to what had it).
  `showPalette`'s `openFile` now calls `Editor::focusActiveDocument` once
  the file is the active one (path or its realpath; a web bookmark's page
  takes the keyboard itself). Go to Symbol (`goToSymbol`) and opening a Find
  in Folder result kept (double-click, Enter) already left the text with the
  keyboard; `symbols` and `preview_tabs` now start from the tree (waiting
  for it to have the keyboard first) and say so. `palette` checks Quick
  Open from the tree.
- **Replace in Folder.** `Ctrl+Alt+Shift+F` put the keyboard in the
  replacement always. Now as VS Code's findOrReplaceInFiles:
  `seedSearchFromSelection` (main.cpp) makes the editor's one-line
  selection (at most 1,000 characters, the text view holding the keyboard)
  the query (`SearchPanel::seedQuery`, escaped by the core's `FindRules::escapeRegex` while the
  regex option is on; Find in Folder seeds the same way, which GTK did not
  do before), and `showReplace(focusReplace)` gives the replacement the
  keyboard only then; otherwise the query, its text selected. Tab in the
  query now goes straight to the replacement while its row shows (it went
  through the option toggles). `replace_in_folder` checks all three cases.
- **Graph rows.** Each row's list item has `GitUi::graphRowLabel` as its
  accessible label (`gtk_list_item_set_accessible_label`, set on bind;
  `GitPanel::graphRowAccessibleLabel` reads it back). Items are now
  activatable, for an assistive technology's activate: the list view's
  `activate` keeps the commit's full hash, and a turn later
  `pressedGraphCommit` finds it in the graph then (a refresh may have moved
  the rows), selects it and shows it as a click does, ringing the bell when
  it is gone ("Show more" loads more). Return is still the key controller's.
  `git_review` checks two labels and an activate (the signal the item's
  accessible action emits).
- **Trap.** An activatable list item takes the second press of a
  double-click for itself and emits `activate`, so the view's click gesture
  never sees its release: the `activate` handler in the constructor
  tells a pointer's activate from an assistive one by a capture-phase press
  stamp within 600 ms and runs `clickedRow(row, true, 2, true, 0)` for the
  pointer's. Another trap in the checks: `MCGuiClick` is a real X click that
  lands after the step, so a selection made in the same step is collapsed by
  it; click, wait for the text to have the keyboard, then select.
- **Not verified.** Orca reading the label or pressing the row: only the
  label property and the `activate` signal are checked, not a screen reader.

### 145. Accessibility: review buttons and Processes rows (November 2026)

- **What.** The Mac's f112b53d and the note of 331b3acf. Each review-page
  button (`ChangeReviewPage`) gets the core's `ChangeReview::buttonLabel`
  ("Stage change 1 in app.py", "Keep Current for conflict 2 in a.py",
  "Stage File app.py") as its accessible label
  (`gtk_accessible_update_property`, LABEL); its visible title and tooltip
  are unchanged. Each Processes row (`ProcessesPanel`) gets
  `Procs::rowLabel(p, orphan)` as its label and, while alive,
  `Procs::stopLabel(p)` ("Stop python3 on port 8123") as its accessible
  description: the Stop is a GMenu item with no widget to label, so the
  description is where a screen reader finds it. `stopPid` on a process
  that has gone still rings the bell and now says "<name> (process N) has
  exited; nothing was stopped" through `onNote` (the panel's note; "The
  process" when the row is gone from the list, since GTK keeps no captured
  item as the Mac does).
- **Why.** The audit found a screen reader heard only "Stage" for every
  change, and no row-level text in Processes.
- **Checks.** GTK has no getter for the label property and the checks run
  with `GTK_A11Y=none`, so each label is also kept on the widget with
  `g_object_set_data` ("mutible-a11y-label", "mutible-a11y-description"),
  read by `ChangeReviewPage::accessibleLabelOf` and
  `ProcessesPanel::rowAccessibleLabel/Description`, used only by the
  checks. `git_review` reads four buttons (two files, an Unstage, a Discard,
  a Stage File); `processes` reads the server row's label and its Stop.
- **Not verified.** Orca reading either; the exited-process note has no
  committed check.

### 146. Edit boxes keep an agent's rewrite; Replace in Folder checked from the page (November 2026)

- **What.** The Mac's 8efec157 (`MarkdownEdit::rebaseSpan`) on GTK. The
  Markdown block box (`Editor::commitMdEdit`) and the LaTeX preview's box
  (`LatexPreview::commitEdit`, and `commitInputEdit` for an `\input` file)
  used to refuse when the buffer changed at all; the Markdown box put the
  draft on the clipboard with a bell. They now make the edit on the text as
  it is then, the block, span or list (adding an entry) found again by
  `rebaseSpan`: the same offsets when the change is after, shifted when
  before, else the one place holding its exact bytes. Only when the span's
  own text changed (or is there twice) is the edit refused as before:
  clipboard and bell for Markdown, clipboard and the bar's note for LaTeX.
  The LaTeX box's refusal text changed to say the text it edited changed
  too. Part 2: `session_browser` checks Replace in Folder from the page: the
  hello says `canReplace`, a search then a replace of its rows writes the
  closed file and edits the open tab (unsaved, `edited: 1`). It passed
  through the core server and host unchanged, so no GTK fix was needed.
- **Checked.** `edit_boxes` (21 passed): a box opened on the first
  paragraph, the second rewritten with `Document::spliceText`, the draft
  applied at its block with the agent's change kept; then the first
  paragraph rewritten, nothing applied. `latex_inputs` (37 passed): the
  same two cases on the document's own text, the second paragraph's span
  shifted by a longer first rewrite; the refusal's bar text. `session_browser`
  (201 passed). `meson test` and `make test` pass.
- **Traps.** `Document::spliceText` alone leaves a Markdown page showing the
  old render (only the box's own apply and Ctrl+Z re-render it), so the
  check undoes with Ctrl+Z instead of splicing back. A `session_browser`
  step that overwrites `gMsg` breaks the next section's first check, which
  reads the last `closing` answer: keep a step's answer in its own Json.
  A tab for the replace check is opened with the page's own `open` message.
- **Not verified.** The clipboard's content after a refusal is not read back
  (GDK's clipboard read is asynchronous): the buffer and the status text are.
  The `\input` file path's rebase has no committed check. The checks were
  not run against the old code to see them fail.

### 147. <details> folds in the Markdown preview (November 2026)

- **What.** The page and the side preview fold `<details>` as the Mac does,
  over the core's `MarkdownParser::fold`: the summary behind a triangle
  (the core's glyphs), the body hidden until a click, `open` honoured.
  `Markdown::foldedRuns` folds with `Hooks::detailsOpen`; the side's worker
  folds before `MarkdownPage::split`, with `MarkdownSide::getDetails`. State is
  `Document::detailsOpen()`, by tag line like the Mac's, so it survives
  re-renders while editing and lives as long as the tab. A summary's span
  carries `mutible-details:<tag line>:<open>`, which `Editor::openMarkdownLink`
  turns into `toggleDetails` (the page re-rendered at its scroll, the side
  scheduled). Click only, as on the Mac.
- **Checked.** `markdown_details` (new): closed body hidden, open shown, real
  clicks open and close, triangles follow, raw tags never show; the markdown_*
  suites and `meson test` pass.
- **Not verified.** The look (triangle and cursor); the side preview's click
  has no check of its own (same link path, folded in its worker).
- **Trap.** A check that clicks the same summary twice must wait out GTK's
  double-click time between the clicks: `Editor::onPressed` follows a page
  link only on `n == 1`, so a quick second press on the same spot is a
  double-click and toggles nothing. The suite passed under load and failed
  on a quiet machine until it paused for `gtk-double-click-time` plus 0.1 s.
  The Mac's summary also has a "Click to open" / "Click to close" tooltip;
  GTK's page spans have none.

### 148. Memory: the find bar's copy, malloc_trim, and what is left (November 2026)

GTK held a big file at about 13 times its size. Three shares were the GTK
side's; two are fixed, one left alone on purpose.

- **The find bar's copy.** `FindBar::close()` let go of `text_` but not
  `foundText_`, which holds the same `Text`, so the copy (UTF-8, the file's
  size) stayed. Both go now (`holdsText()` reports it); a running search
  holds its own `shared_ptr<Text>` in its Job or Stage, so what it reads
  stays until it ends. A big text (3 MB or more) also gets a
  `malloc_trim(0)` 300 ms after the close: the copy is under glibc's 32 MB
  mmap ceiling, so freeing it only fed the heap's free list.
- **After a big tab closes.** `Editor::destroy` runs `malloc_trim(0)` in a
  low-priority idle after `delete d` for a buffer of 3 million characters
  or more (glibc only; `Editor::trimsDone()` counts the runs).
- **Left alone: `Document::saved_`.** It is the disk's text as last read or
  saved and is what dirty means (`matchesSaved`, `rememberDisk`,
  `checkDirty`), what an external change is compared with
  (`checkExternalChange`, `externalChangeCheck`) and what the host is reset
  to after a reload or restore (`hostSavedFromDisk`). It cannot go without
  changing those, so it stays (1 times the file).
- **Numbers** (`large_memory`, a 30 MB ASCII log of 80-byte lines, Pss from
  /proc/self/smaps_rollup, MB; the core's host and mirror shares are not
  touched here): before opening 126, laid out 529 (+403), after a search for
  a word on the last line 563 (+34, the bar's copy). Bar closed: 563 before
  this change (nothing was returned), 530 after. Tab closed: 383 without the
  trim (+257, 8.6 times the file), 203 to 216 with it (+77 to +90, 2.6 to 3
  times). What stays is the heap's holes (about 130 MB of heap, small
  allocations of the buffer and index interleaved), which a trim cannot give
  back.
- **Checked.** `large_memory` (new, LargeMemorySuite.cpp): the bar holds a
  text while it shows and none after `closeFromEditor`, a search after
  reopening works, the Pss drops by the copy's size after the close, and
  after the tab's close it is under 5 times the file above its start (the
  trim ran). find_bar, tabs, backups, save, sessions, preview_tabs pass,
  `meson test`, root `make test`.

### 149. The compare view's last three pieces (PARITY 3.2 done; November 2026)

- **The diff preview tab** was already in place (`CompareSpec::preview` makes
  the tab `Model::Preview::AgentDiff`, which `Window::add` replaces, `--keep`
  turning it off; PARITY's "a tab of its own each call" was stale). What was
  missing was a check: `compare` now opens three comparisons: the second
  replaces the first, `keepPreviewTab` (a double-click, Keep Open) keeps the
  second, the third is a tab of its own. A decision tab (the IDE link's
  openDiff) is never a preview, as on the Mac.
- **Tints.** `CompareView::rowTint(left, row)` (also for the checks): 1 the
  pane's own, 2 the trace of tint (grey, alpha 0.10: the Mac's "dropped") a
  rejected hunk keeps on the right; an accepted hunk keeps its colours, as
  on the Mac. `hunkOfRow` is a binary search over the hunks.
- **A tab's place.** `Document::compareScroll()` (the vertical place and each
  pane's sideways one) is saved in `Editor::stash` while the compare view
  shows that tab's data and put back after `CompareView::show`. As the
  Mac's `diffScroll`, it is not in the session (a remade comparison opens at
  the top).
- **Checked.** `compare` (the tints on a rejected, an accepted and an
  undecided hunk; replace, keep, and the scroll back after another tab).
- **Not verified.** The look of the tint.
