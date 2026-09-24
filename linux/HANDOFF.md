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

Items 1 to 8 below are done (September 2026), so the port now has what the
Mac has, apart from what the list at the end of item 8 leaves out. It builds
without warnings at `warning_level=2` (CI: Ubuntu, GTK 4, VTE, WebKitGTK,
poppler) and has been run for real on Ubuntu 26.04 under GNOME on Wayland:
- data safety: "Save changes?" before anything replaces edits, failed saves
  reported, atomic saves (item 1);
- the file tree, live-refreshing, with New File, New Folder, Rename, Move to
  Trash, Open Containing Folder and Copy Path (item 2);
- the editor with incremental syntax highlighting (item 3), Ctrl+/, the
  Markdown preview, and reloading when the file changes on disk (item 8);
- images and PDFs in the editor's slot (item 4);
- Find in Folder (item 5) and find in the file with Find Next and Find
  Previous (item 8);
- language servers: squiggles, completion, hover and go to definition
  (item 6);
- the LaTeX preview with double-click editing and Export PDF (item 7);
- several windows, each with its own tree, editor, terminal, browser, search
  panel and language servers; Previous File, Focus Editor, Close Window and
  Quit (item 8);
- a VTE terminal that gets its own Ctrl keys while it has the keyboard, and
  a WebKitGTK browser;
- the settings file with live colors, per-panel opacity and a live color
  picker (item 8);
- pane hiding and divider drags, the shortcut hints panel, and opening a
  file or folder named on the command line.

It shares from `../src` `SyntaxHighlighter`, `MarkdownParser`, `Settings`,
`LineComments`, `FolderSearch`, `Json`, `LspClient`, `LatexDoc` and `SyncTex`.
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
not by a real mouse or keyboard: the right-click, F2 and Delete. Not run at
all: Open Containing Folder, which would have opened a file manager window.
Needs the user's eyes: where the popovers sit and how they look.

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
  the grammar changed. Rename in the file tree calls it.

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
Not done: zoom, on Linux as on the Mac, and a person looking at it.

`src/PdfView.{h,cpp}` is the piece item 7 reuses. It shows every page in a
scrolling column fitted to the width, rendered lazily (visible pages first,
one per idle pass, at the surface's scale factor, bitmaps dropped for pages
far from view). Its API, with 0-based pages and points from the page's
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

A trap paid for: page sizes set from the adjustment's `changed` signal land
in the middle of an allocation and are never laid out; the box kept its old
width after a resize. The relayout now runs from an idle.

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
- Typeset from the hidden sibling `.<name>.minicode.tex` into
  `$XDG_RUNTIME_DIR` or `/tmp`, debounced and generation-counted.
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
- **Output** goes to `$XDG_RUNTIME_DIR/minicode-latex/<hash of the path>/`
  (or `/tmp/minicode-latex-<user>`), so two `notes.tex` in different folders
  never share a PDF.
- **Nothing outlives the app.** tectonic and the download tools are started
  with `PR_SET_PDEATHSIG`, so they die with MiniCode even when it is killed;
  on a normal quit the application's `shutdown` signal stops tectonic and
  removes the hidden sibling. Opening another file, or Open Folder
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

Not done: zoom (as with PDFs), click-to-line in the source view, and a person
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
  key in text views.
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

What the Mac has that Linux still lacks: Command-click on a file or URL in
the terminal's output (the core's `TermLinks` is portable; VTE would need a
regex match or its hyperlink hover), Refresh File Tree (not needed, the tree
is live), zoom for images and PDFs (missing on the Mac too), and blur.

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
  then quietly builds without that panel. CI checks `pkg-config` for this,
  poppler included.
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
- A new shortcut goes in `kBinds` in `main.cpp`, the menu in `buildMenu`,
  `hintsText`, and the README table. A plain Ctrl key is given to the shell
  while the terminal has the keyboard without anything more; if it is a key
  for moving between panes, add it to the exceptions in `shellOwns`.
- A window's objects are deleted while its widgets still exist, from
  `window-removed`. Anything new that connects a signal to a window's object
  with the object as data is covered for widgets and their controllers by
  `disconnectOwners`, once the object is in the owners list in
  `onWindowRemoved`; a connection to anything else (a buffer, a model, a
  monitor, the application) must be undone in the object's destructor.
