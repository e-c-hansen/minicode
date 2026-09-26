# Building MiniCode on Linux (GTK4)

MiniCode began as a native macOS editor. This is a port of the same editor to
Ubuntu Linux using GTK4. It reuses the exact same portable C++ core (the syntax
highlighter, the Markdown parser, the settings file parser, the comment
toggle, the folder search and the language server client) that the macOS
build uses, so highlighting, Markdown rendering, settings, Ctrl+/ and the
language server features behave identically. Only the GUI layer is rewritten, from
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
- The port's own pure-C++ pieces pass 327 checks (`cd linux && make test`,
  or `meson test -C build`): the byte-to-character and UTF-16 offset
  conversions in `linux/src/Utf8Offsets.h`, the theme stylesheet, the page
  word finder behind the LaTeX click, terminal link paths, and the Source
  Control panel's model (`linux/src/GitModel.h`: rows, keys, selection after
  a refresh, argument vectors, the summary line, error text, diff and commit
  text styles in character offsets, and the graph's labels and tooltips). On a Mac two
  of the link-path checks fail because `/tmp` is a link to `/private/tmp`
  there; on Linux all pass.
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
    swatch opened the GTK color dialog then, and the choice was written into
    the line, uncommented, and saved. Since item 8 of `linux/HANDOFF.md` it
    opens a popover that applies picks live instead (checked below).
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
  preview to its source first (but see the correction below: whether the
  line was on screen was never checked, and it was not); a folder selected in the tree becomes the
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
    rendered or after jumping to page 51. That was a light PDF; rendering
    ran on the main thread then, so a heavy page could freeze it (see the
    next point). Snapshots of the view, read back as images,
    show the pages drawn, and sharp at twice the scale.
  - After the September 2026 review, in the Docker container (aarch64,
    poppler 26.01), with generated PDFs: a 100 by 60,000 point page between
    two letter pages made the build before the fix spin at 100% of a core
    with `gdk_memory_texture_new: assertion 'image size 0x0 is invalid'`
    repeating, because cairo cannot make a surface that tall; the fixed
    build renders it narrower and sits at 0% CPU, before and after
    scrolling to it, with no criticals. A page with a broken inline image
    also settles at 0%. On two pages of 400,000 strokes each, zoomed in four
    steps, the main thread used 245 ticks of CPU in 12 s before the fix and
    4 after, with the work (316 ticks) on the render thread. Switching
    documents three times mid-render and quitting exited cleanly, also under
    AddressSanitizer. The grey placeholder for a page poppler cannot render
    at all was not reached by any file tried (poppler repairs a zero-size
    MediaBox to letter), so that path is checked by reading.
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

- Zoom for PDFs and the LaTeX preview (September 2026, on the ThinkPad under
  GNOME Wayland, display at 125%). A temporary `MINICODE_ZOOMTEST` hook,
  since removed, ran under its own application id (`org.minicode.ZoomTest`)
  with a scratch home, config and tectonic cache, and made 62 checks, all
  passing. It generated a 100-page PDF with a landscape page 50 and a red
  and a blue square at known points on every page, and also used
  `demo/paper/notes.pdf` and `notes.tex`:
  - The zoom actions were activated as the menu and keys do: from fit width
    (79% in an 856-pixel view) down to 50% and up to 100% and 300% by steps,
    each step keeping the point at the view's centre within a pixel.
    `setZoom` about points off the centre, the path Ctrl+wheel and a pinch
    take, kept the point under them too, sideways included once the page is
    wider than the view (a page narrower than the view is centred, so only
    the vertical position can hold then). 400% and 25% are the limits.
  - At fit, 25%, 50%, 100%, 300% and 400%, and on the landscape page and
    the page after it: the view was scrolled to the red square, a snapshot
    of the widget was read back, and the pixel where `pagePointIn` put the
    square's centre was red, with `pageAtPoint` at that point giving the
    square's PDF coordinates back to within a pixel. The snapshots were
    looked at: sharp text at 400% and 300%, the column of pages at 25%, and
    the badge in the corner ("Fit width, 79%").
  - `anchor()` after `scrollTo` at 300% gave back the page and y it was
    given. Rewriting the PDF on disk at 300% kept the zoom, the anchor and
    the sideways scroll exactly. Opening another PDF went back to fit width,
    and a text file disabled the three actions.
  - Memory at 400%: jumping through twelve places in the 100-page document
    ended with 173 MB in page bitmaps and the process at 376 MB. Before the
    pages were drawn as textures it ended at 1.7 GB, most of it copies of
    bitmaps already let go (see `linux/HANDOFF.md`, item 4).
  - With the terminal focused, `win.zoomin` and `win.zoomout` had no
    accelerators and `win.zoomfit` kept Ctrl+Alt+0; with the focus back in
    the tree, all three were bound again.
  - The LaTeX preview: `notes.tex` typeset, the actions were enabled, and a
    double-click through `pageAtPoint` and the preview's own click handler,
    at 150%, 300%, 50% and fit, opened the right item for "draft",
    "printer", "Friday" and "careful". An edit committed through the popover
    ("review" to "careful review") retypeset, and the zoom, the anchor and
    the sideways scroll were the same afterwards.

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
  - After the September 2026 review, in the Docker container (aarch64,
    poppler 26.01, tectonic 0.17.0, `XDG_RUNTIME_DIR` unset), driven with
    xdotool and checked from the folder listings and screenshots:
    - notes.tex in one window and notes.ltx in a second, plus notes.tex in
      that second window too, typeset together. A watcher listing the folder
      every 20 ms saw three different siblings (`.notes.tex.1…`,
      `.notes.tex.2…`, `.notes.ltx.2…`), and none was left afterwards. Both
      previews showed their own title page.
    - Renaming the document's folder while tectonic ran (a document made
      slow on purpose): the build before the fix left `.notes.minicode.tex`
      in the renamed folder, the fixed one left nothing.
    - With `/tmp/minicode-latex-root` made first by another user and open to
      all, the output went to a fresh 0700 `/tmp/minicode-latex-XXXXXX`
      instead.
    - A double-click on "review" still opened "Editing list item" with
      `Ask for a review`, so SyncTeX finds the renamed sibling.
    - Not checked by running: an Export PDF left waiting when its window
      closes now frees its file instead of leaking it (by reading).
- Language servers (September 2026, clangd 21 at /usr/bin/clangd, on the
  ThinkPad under GNOME Wayland). A temporary test hook, since removed, ran
  inside the real app under its own application id on a scratch copy of
  `demo/vec` with a `compile_flags.txt`, and made 32 checks, all passing:
  - clangd started (the status bar read "clangd starting…", then "clangd: no
    problems"), one server process for the window.
  - An undeclared name inserted through the buffer API came back as "1 error";
    the error tag covered exactly the name's characters, not one either side,
    and the tooltip text read "Error: Use of undeclared identifier
    'undefinedThing'". A space typed at the start of that line, which makes
    the highlighter retag it, left the squiggle in place, shifted by one.
    Deleting the line brought back "no problems" and the tag went.
  - Typing `p.` one character at a time opened the completion list with
    `lengthSquared() const`, `scaled(double k) const`, `x` and `y`, while the
    text view kept the keyboard focus. Typing `le` narrowed it, Down and Up
    moved, and Return inserted `lengthSquared`. The Ctrl+Space action on
    `p.sc` listed `scaled` first and Tab inserted it. Escape closed the list
    and left the text alone.
  - Hover (the Ctrl+I action) on `lengthSquared` gave clangd's text with the
    type (`→ double`) and the header's comment. The tooltip path asked the
    server once for that word and showed the answer on the next query.
  - Go to definition on `scaled` in main.cpp opened vec.h and selected
    `scaled` on line 11; on `Vec2` inside vec.h it stayed in the file and
    selected `struct Vec2`'s name on line 4. Saving first sent didSave.
  - A Python file said "No Python language server found" (none is
    installed); a PNG got no server and an empty status.
  - Closing the window sent shutdown and exit (both in the
    `MINICODE_LSP_LOG` traffic), the app exited on its own, the whole run
    took under 5 s, and afterwards `pgrep -a clangd` found nothing and the
    server's pid was gone. Every run of the hook ended the same way.
  - The first run found a real bug: the completion and hover popovers,
    parented to the text view, sent GtkTextView's dispose into an endless
    loop of "GtkPopover is not a child of GtkTextView" warnings, so the
    window never closed. They are now removed when the view is unrealized.
  - Seen in the full run and not explained then: opening the PNG, just after
    t.py, logged two GTK criticals (`g_signal_handler_disconnect: assertion
    'handler_id > 0'` and `gdk_frame_clock_idle_end_updating`), from inside
    GTK's unmap of the text view when the editor's stack switches to the
    image. Explained and fixed in item 8, below: the editor was still
    scrolling to the caret when its slot was switched.
- The smaller gaps, item 8 of `linux/HANDOFF.md` (September 2026, on the
  ThinkPad under GNOME Wayland, GTK 4.22.4, VTE 0.84, clangd 21, tectonic
  0.17). A temporary `MINICODE_ITEM8TEST` hook, since removed, ran inside the
  real app under its own application id, with scratch `HOME`, config, cache
  and settings, `GSETTINGS_BACKEND=memory` (the color chooser writes its
  custom colors to GSettings), and a scratch copy of the tectonic cache. It
  activated the window and application actions, set the find bar's text,
  emitted the swatch's click gesture, pressed alert buttons, and wrote files
  from outside. 81 checks, all passing, with no GTK criticals or warnings
  from MiniCode in the log and no clangd, tectonic or MiniCode left after.
  The one warning in the log is WebKit's web process failing to release
  its bus name as the app exits.
  - Previous File: with no history it opens nothing; main.cpp then vec.h,
    and Ctrl+Tab goes back and forth; from an image back to vec.h and
    forward to the image again.
  - Find Next and Previous: with no query, Ctrl+G opens the find bar. With
    "vec2", the matches on lines 4, 5 and 5 come in order, Ctrl+G wraps to
    the top, Ctrl+Shift+G wraps to the last and steps back, and the entry's
    own previous-match does the same. No match leaves the selection alone.
  - Show Hidden Files: dotfiles hidden by default, in the root and inside an
    expanded folder; Ctrl+H flips the action's state (the menu's check
    mark) and shows them in both places. It was also reported to keep the
    folder expanded, which held for that test's layout but not in general:
    see the correction below. A new
    window starts with the setting as it is, and toggling it again hides
    them in both windows.
  - New Window: a second window on the same folder, with its own editor,
    tree and language server session (two clangd processes, one per
    window) and its own file. Closing it with unsaved edits asks "Save
    changes?": Cancel keeps it, Don't Save closes it and leaves the file
    alone on disk, while the first window and the application carry on. Its
    clangd was stopped, and the LaTeX preview it had open (tectonic
    running) went with it: no tectonic afterwards and no hidden sibling.
    The terminal, browser and Find in Folder panel it had open were torn
    down without a warning.
  - A second `minicode sub` run from outside opened a new window on `sub`,
    and `minicode sub/plain.txt` opened the file in that window rather than
    a third. Ctrl+Q at the end closed every window and the process exited.
  - The terminal's keys: with the terminal focused, the plain Ctrl
    accelerators and F12 are all unbound (14 checked: Ctrl+B, F, H, I, O,
    Space, W, N, G, Q, S, /, comma and F12), while 12 Ctrl+Shift, Ctrl+Alt
    and pane keys stay bound, and Ctrl+Shift+T and Ctrl+` both stay for
    the terminal. Focusing the editor (Ctrl+1) or the tree (Ctrl+0) brings
    them all back, and so does closing the window whose terminal had them.
  - Reload on external change: a 300-line file changed on disk in its first
    and 290th lines reloaded by itself, stayed clean, and kept the caret
    (line 151, column 4) and the scroll position (1,500 pixels) exactly; so
    did a whole-file rewrite through an atomic rename. MiniCode's own save
    changed nothing. With unsaved edits, a change on disk asked Keep Mine or
    Reload and replaced nothing before the answer; Reload took the disk's
    text and left the buffer clean, Keep Mine kept the edits, the same
    version on disk was not asked about twice, and a rewrite with the same
    contents was no change. A Markdown preview re-rendered.
  - Live color picking: a click on the `# editor.background` swatch opens
    the popover with the chooser on the swatch's color (#1E1E1E). Four
    colors set in a row, like a drag, rewrote the line at once, uncommented,
    were not yet saved, then were saved 150 ms after the last one, and the
    settings applied: the editor's background, read back from the settings,
    was the picked #CC4080, and a snapshot of the window showed it. Closing
    the popover saves the last pick at once and takes the popover off the
    view. Snapshots of the popover and the window were looked at.
  - GTK criticals: a separate run scrolled the editor with an animation and
    then switched to a PNG, a GIF, a PDF, a `.tex`, the browser, or hid the
    editor, 0 to 100 ms later, 60 times. Before the fix it logged the two
    criticals from the language server run 20 times, with a backtrace (under
    `G_DEBUG=fatal-criticals` in gdb) inside GTK's unmap of the editor's
    scrolled window, called from `Editor::openFile`. After it, two runs
    logged none. The cause is in `linux/src/ScrollSettle.h`.
- Terminal links (Ctrl+click), with a temporary hook under its own
  application id: 51 checks, all passing. The real shell ran
  `cd sub && clear && command cat ../links.txt` on a scratch project, and
  the hook worked out each reference's cell from the terminal's text, then
  asked VTE's own hit test (`vte_terminal_check_match_at` with a `\S+`
  regex) which word sits under that pixel, so the cell maths was checked
  against VTE and not against itself. Then it called the click path and
  read the editor's caret. Covered: a gcc `path:line:col` resolved against
  the project folder, Python's `File "x", line N` resolved against the
  shell's directory (which followed the `cd`, from OSC 7), TypeScript's
  `file(line,col)`, a reference after accented and arrow characters (the
  column counted in characters, not bytes: 5:7 put the caret after "é"), a
  reference wrapped across two rows clicked on either row, a missing file,
  plain words and the space past a line's end giving nothing, an https URL
  with a balanced parenthesis, a folder selected in the tree, a `file://`
  URL opening the browser panel, a file link putting the browser away, and
  the same after 60 more lines of output with the view scrolled back 30
  rows. The underline was checked as spans of cells (one span, and two for
  the wrapped reference) and seen in widget snapshots to sit under the
  link's text, and the pointer was read back as `pointer` over a link and
  `text` after. The run under AddressSanitizer was clean. This test is what
  found that VTE's scroll adjustment does not count rows the way its text
  calls do once `clear` has dropped the scrollback (see
  `linux/HANDOFF.md`, item 9).
- The file tree, Find in Folder and Show Hidden Files, fixed after the user
  tried the installed app by hand (September 2026). Three of the four bugs
  were in things the earlier hooks had reported as working, because the
  hooks called the code paths rather than clicking, and never looked at the
  screen. This round used real input instead: a private rootful Xwayland
  (`Xwayland :87 -geometry 1300x850`, a window of its own on the user's
  desktop) with the app on GTK's X11 backend inside it, and clicks and keys
  sent with XTest from a Python script through `ctypes` (libXtst has no
  headers installed, and none are needed). Those are real X events, so
  GTK's gestures, their claiming, double-click counting and the accelerator
  matching all ran as they do for a mouse and keyboard. A temporary
  `MINICODE_TREETEST` hook, under the application id
  `org.minicode.TreeTest` and since removed, answered over a FIFO with the
  state (caret, selection, the lines in the visible rect, the tree's rows
  and selection, the focus) and each widget's position, and rendered the
  tree to PNG; `xwd` gave whole-screen captures. 38 checks, all passing
  after the fixes:
  - **No visible selection in the tree.** Confirmed first on screen: a
    clicked row looked like every other. The stylesheet made the rows'
    background transparent, and an application stylesheet outranks the
    desktop theme whatever the selectors, so the theme's selection color
    went too. The selected row is now blue while the tree has the keyboard
    and a tint of the sidebar's text color when it does not, with a focus
    ring after keyboard use and a faint hover. Checked in renders of the
    tree with the default dark sidebar and with a light one from a scratch
    settings file: focused selection (18,79,121) on (37,37,38), unfocused
    (64,64,65). The folder arrows now take the sidebar's text color too.
  - **One click acts.** A click on a folder opens or closes it, a click on
    a file opens it, as on the Mac. A click on the arrow toggles once; a
    double-click toggles once; the file stays selected and the tree keeps
    the keyboard. A right-click selects without opening. Clicking another
    file with unsaved edits asks "Save changes?"; Cancel keeps the file and
    puts the selection back on it, Don't Save opens the other and leaves
    the first as it was on disk. Clicking the open, edited file keeps its
    edits and caret. Up and Down move the selection without opening, Enter
    opens and moves the keyboard to the editor, Left goes up to the folder
    and then closes it, Right opens a folder and then goes to its first
    entry.
  - **Find in Folder opened at the top.** Reproduced with the old code: a
    double-click on a match at line 250 of a 400-line file selected the
    match, but lines 166 to 208 were on screen, and they stayed there. The
    text view had estimated the heights of lines it had not laid out and
    animated towards that point; the real heights differed. The editor now
    watches the caret for up to two seconds and scrolls again once the view
    has stopped with the caret off screen. After the fix the view passes
    through lines 112 to 154 and settles on 236 to 278 within 0.4 s, and
    stays there. Also checked: the file already open, a Markdown match
    (preview switched to source, match on screen), and the editor having
    the keyboard afterwards. A match now opens on one click, as the user
    expected. Down from the query field, and Enter there once the results
    are in, did not move the keyboard into the list at all, which nobody had
    pressed for real before; they do now, and Enter on a result opens it.
  - **Show Hidden Files on Ctrl+Shift+.** Written first as
    `<Ctrl><Shift>period`, which a real key press showed never matches: GTK
    compares the keyval the key produced (`greater` on a US layout) and
    treats Shift as used up by it. It is bound as `<Ctrl>greater`, and a
    real Ctrl+Shift+. toggles the dotfiles, also with the terminal focused,
    where it stays bound like the other shifted keys. Ctrl+H still works.
    The same run found that showing or hiding dotfiles closed every open
    folder and dropped the selection whenever a dotfile sorted into the root
    (the sorted lists report every row as removed and added again). The
    tree now reopens those folders and reselects the row.
  X11 is not the user's Wayland session, and gestures and accelerators do
  not depend on the backend, but window activation does: whether GNOME on
  Wayland raises the editor window when a match is opened still needs the
  user. A plain launch on Wayland under the hook opened the window and the
  search panel and found the match.
- Fixes from the September 25 review, each run in the Docker container with
  real X clicks and keys (xdotool), not hooks:
  - A 5 GB sparse file opens as "Cannot display … (Larger than 16 MB, too
    large to edit here.)" at once, and a named pipe as "(Not a regular
    file.)" without hanging; the next click on a text file opens it. Text
    files are read only if they are regular files of at most 16 MB, checked
    before the file is opened, and the external-change reload uses the same
    read.
  - Two windows with the same file, the first with unsaved edits. Renaming
    it from the second window (File > Rename) renamed it in both titles;
    Ctrl+S in the first wrote the edits to the new name and the old name did
    not come back. Renaming `x.txt` to `x.md` let Ctrl+Shift+P render it,
    and renaming it on to `y.txt` while the first window showed the preview
    took that window back to the source.
  - Saving a file with a second hard link wrote through both names (same
    inode, link count still 2); saving a group-writable file owned by
    another user kept its owner and inode; saving through a link whose
    target had been deleted was refused with "The file is a link to
    something that does not exist.", the edits left unsaved and the link
    left alone. Ordinary files still get the atomic rename.
  - 300 files created, renamed and deleted as fast as a shell loop can go
    left no row behind in the tree.
  - Closing a window that never opened a `.tex` logged no GTK warnings (the
    teardown no longer disconnects handlers by null data).
- The Source Control panel (Ctrl+Shift+G, September 26), in the Docker
  container with real X keys and clicks (xdotool) and screenshots looked at,
  against scratch repositories in the container's `/tmp`, identity given
  through `GIT_AUTHOR_*` and `GIT_COMMITTER_*` with `GIT_CONFIG_GLOBAL` set to
  `/dev/null`. The panel logs what it shows with
  `G_MESSAGES_DEBUG=minicode-git`, which is how the rows and graph were
  read back.
  - The clone was 2 ahead and 1 behind its upstream, with a merged feature
    branch, an annotated tag, a remote-only branch and a local branch. The
    branch line read "main ↑2 ↓1", the lists held two staged files (a
    modification and a rename) and four changes, and the graph's 8 rows had
    the right lanes, labels (main filled, local-only outlined, origin/*
    purple, the tag amber), hollow tinted dots with up arrows on the two
    local commits and a green down arrow on the upstream one.
  - Space unstaged the rename (both halves came back as a deletion and an
    untracked file), staged `sp*ecial.txt` without touching `spXecial.txt`,
    and in an empty repository staged a file and unstaged it again with
    `rm --cached`, the file left on disk. The selection stayed at the same
    place in its list each time.
  - Return and a click showed a modification, an untracked file and a staged
    file in the editor's slot, colored, titled "a.txt (diff)". With unsaved
    edits in the open file it asked "Save changes?" first; Cancel kept the
    file, Don't Save showed the diff and left the file alone on disk.
  - Ctrl+Return with an empty message showed git's "Aborting commit due to
    empty commit message." in red; with a two-line message it committed and
    showed "[main 807914c] ..." muted, and the graph gained the commit.
  - Return and a click on commits showed the header, message, stat and diff
    under "6a9014e More feature work"; a merge said it was against its first
    parent and showed what the feature brought in; the root commit had no
    parent line.
  - All branches added a side branch's commit in its own lane (10 rows
    instead of 9). On 3,200 commits made with fast-import the first 200
    arrived 208 ms after the key press, Show more (End, Return) loaded 400
    rows in about 70 ms with the selection on the first new one, and again
    600.
  - Tab went changes, graph, message and round; Shift+Tab the other way;
    Down from the last file entered the graph and Up from its first row went
    back to the list. With no changes, opening the panel put the keyboard in
    the graph.
  - With the terminal focused, Ctrl+Shift+G reached the shell (`cat -v`
    printed `^G`) and the panel stayed; Ctrl+0 then focused the panel and
    Ctrl+Shift+G put the tree back.
  - A new file at the top level and a commit typed in the terminal each
    refreshed the panel within a second; a change in a folder the tree had
    never opened was picked up when the window came back to the front. Six
    idle seconds after a refresh ran no git at all, and `.git/index` kept its
    time. Open Folder moved the panel to an empty repository, a detached
    HEAD ("HEAD is detached, so there is no upstream to compare with."), a
    clone level with its upstream and a folder outside any repository.
  - Through a logging wrapper first on PATH, only rev-parse, status,
    for-each-ref, log, rev-list, diff, add, commit and show ran; nine
    refreshes ran log four times, since an unchanged repository keeps its
    graph. Closing a window with the panel open, and with a Show more still
    loading, logged no GTK warnings.

Not verified:

- For the Source Control panel, everything that needs GNOME on Wayland: the
  look under the desktop's own theme (the button and check box are styled
  from the sidebar colors, the rest follows the theme), the selection and
  focus colors at 2x, tooltips on a real pointer, the window-focus refresh
  when switching from another application, and a real repository with many
  branches in All branches mode.
- Everything driven by real keyboard and mouse input. Actions were activated
  programmatically, which proves the wiring but not the key handling. That
  includes scrolling a PDF with the wheel and clicking a page (the mapping
  from a point to a page and PDF coordinates was checked, the click was not).
  For Find in Folder it includes pressing Ctrl+Shift+F, typing into the field (the
  test set its text, which fires the same signal), Escape, the Choose
  button's folder dialog, and whether GNOME on Wayland raises the main
  window when a match is opened (the editor is made the window's focus
  widget, but activating a window is up to the compositor); clicking a
  result, Down and Enter were done with real X events in the round above.
  For the file tree it includes F2 and Delete and what the popovers and
  alerts look like and where they sit; clicks, a right-click and the arrow
  keys were real X events in the round above, though on X11, not Wayland. For the LaTeX
  preview it includes a real double-click, Return, Shift+Return and Escape
  in the popover, Ctrl+Z in the preview, Ctrl+Shift+S, and the Export PDF
  save dialog and the file it writes (the bytes handed to it were checked,
  the dialog was never opened).
- Where the LaTeX popover sits and how it looks on screen. A snapshot of the
  popover widget looked right, but its placement over the clicked text was
  not seen.
- How the Find in Folder window looks. Nothing in it was seen on screen.
- For language servers: how the squiggles, the completion list, the hover
  popover and the tooltips look and where they sit, and the real keys and
  mouse. The test called the actions and the list's key handler directly, so
  Ctrl+Space, F12, Ctrl+I, a Ctrl+click (its hit test in particular),
  clicking a row of the list and resting the pointer on a word were not
  pressed or done for real. Whether an input method takes Ctrl+Space first
  is also open. Only clangd was tried.
- PDF zoom by real input. Ctrl+Plus, Ctrl+Equal, Ctrl+Minus and Ctrl+Alt+0
  were not pressed; the accelerators were read back from GTK and the actions
  activated. Ctrl+wheel and a touchpad pinch were not done at all: their
  handlers are compiled, and the zoom-about-a-point they call was tested.
  How big a step a touchpad's smooth Ctrl+scroll makes, how the view looks
  mid-pinch (the old bitmaps are stretched until 120 ms after the last
  change), and the badge's look and place need a person.
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
- For item 8: every new shortcut as a real key press. The accelerator table
  was read back from GTK and the actions were activated, so Ctrl+N, Ctrl+W,
  Ctrl+Q, Ctrl+Tab, Ctrl+G, Ctrl+Shift+G, Shift+Enter in the find bar,
  Ctrl+1, Ctrl+` and Ctrl+H were never pressed. Nor was a key typed into
  the terminal: that bash really receives Ctrl+W, Ctrl+H and the rest rests
  on GTK not binding them any more, which was checked, not on a keystroke.
  Ctrl+Shift+C and Ctrl+Shift+V in the terminal were compiled, not tried.
  Whether F10 (GTK's menu bar key) still reaches the terminal was not
  looked at. How the color popover looks in place (it follows the system's
  GTK theme, light on a stock Ubuntu, over the dark editor), dragging in it
  with a mouse, the Keep Mine / Reload alert on screen, where GNOME puts a
  second window, and GNOME's "New Window" dock item all need a person.
- For terminal links: a real Ctrl+click and a real Ctrl held over a link.
  The test called the click path and the hover code directly, so whether
  the capture-phase click gesture beats VTE's own selection, whether the
  window's key controller sees Ctrl on its own, and whether the pointer
  shape really changes on screen (VTE sets its own pointer as the mouse
  moves; ours is put back from an idle callback) all need someone at the
  machine. So does the look of the underline, one pixel in the text color
  near the bottom of the cell.

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
  a `.tex` file opens as highlighted source, with no LaTeX preview.
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
    meson install -C build --strip

`--strip` leaves out the debugging information. The default build type,
`debugoptimized`, is optimized but also carries about 16 MB of symbols for a
debugger, which would make the installed binary 17 MB instead of about 1 MB.
Keep them (drop `--strip`) only if you want readable crash traces.

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

Installing to `/usr/local` with `sudo meson install --strip` works the same way and puts
the app in every user's menu.

### Sudden second copy of the app?

`minicode` is a single-instance GApplication, so launching it again while it is
running does not start a second process. It does still do what you asked:
the argument is forwarded to the running instance, which opens a new window on
that folder, or raises the window that already has it open and shows the file
there if you named one. GNOME's "New Window" on the dock icon runs the
launcher again, so it opens a window too. Inside the app, Ctrl+N opens a
window on the current folder. The application quits when its last window
closes.

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
title bar settings also color the menu bar, and `window.blur` is ignored.
GTK has no way to ask for it, and GNOME's compositor offers no protocol for
it either: Mutter 50.1 on the ThinkPad advertises neither the older KDE blur
protocol nor `ext-background-effect-v1`, the one in wayland-protocols'
staging set (checked by listing the Wayland globals it offers). KDE
Plasma's compositor has a blur protocol of its own, so a Plasma-only blur
through GDK's Wayland surface looks possible; it was not tried or built. Transparency needs a compositor, which
GNOME always has. A click on a color swatch opens a color picker under it,
and the color applies live while you drag, as on the Mac.

| Shortcut          | Action                     |
| ----------------- | -------------------------- |
| Ctrl N            | New window on the same folder |
| Ctrl W            | Close the window           |
| Ctrl Q            | Quit (every window)        |
| Ctrl O            | Open folder                |
| Ctrl S            | Save                       |
| Ctrl Alt N        | New file                   |
| Ctrl Shift N      | New folder                 |
| Ctrl F            | Find in the current file   |
| Ctrl G or F3, Shift F3 | Find next, find previous (also Enter and Shift Enter in the find bar) |
| Ctrl Shift F      | Find in the folder         |
| Ctrl Shift P      | Toggle the Markdown or LaTeX preview |
| Ctrl Shift S      | Export the typeset PDF of a LaTeX file |
| Ctrl Shift H      | Show or hide the shortcut hints |
| Ctrl B            | Toggle the sidebar         |
| Ctrl Shift E      | Collapse or restore the editor |
| Ctrl Shift T, Ctrl ` | Toggle the terminal panel |
| Ctrl /            | Comment or uncomment the selected lines |
| Ctrl ,            | Open the settings file     |
| Ctrl Shift B      | Toggle the browser panel   |
| Ctrl Shift G      | Toggle the Source Control panel in place of the file tree |
| Ctrl H, Ctrl Shift . | Show or hide dotfiles, in every window |
| Ctrl 0            | Focus the file tree        |
| Ctrl 1            | Focus the editor           |
| Ctrl Tab          | Back to the previous file  |
| Ctrl Space        | Complete, with a language server |
| F12, Ctrl click   | Go to definition           |
| Ctrl I            | Show the type and documentation under the cursor |
| Ctrl +, Ctrl =, Ctrl - | Zoom a PDF or the LaTeX preview in and out (also Ctrl with the wheel, or a pinch) |
| Ctrl Alt 0        | Back to fit width          |
| F2                | Rename the selected item, in the tree |
| Delete            | Move the selected item to the Trash, in the tree |
| Ctrl Shift C, Ctrl Shift V | Copy and paste, in the terminal |
| Ctrl click, in the terminal | Open a file:line or URL from the output |

F2 and Delete work only while the file tree has the keyboard, so Delete in the
editor still deletes text.

In the tree, one click opens a file or opens and closes a folder, as on the
Mac. Up and Down move the selection without opening anything, Enter opens
the selected file and moves the keyboard to the editor, Left goes up to the
folder and closes it, and Right opens a folder and steps into it.

In the Source Control panel, Up and Down move through the files, Enter shows
the selected file's diff in the editor's place, Space stages or unstages it,
Tab moves between the changes, the graph and the message box, and Ctrl Enter
in the message box commits. Enter or a click on a commit in the graph shows
it. Find Previous is Shift F3 rather than the Mac's Shift Command G, because
Ctrl Shift G is the panel, as in VS Code on Linux.

Ctrl Shift . is the Mac's Shift Command . with Ctrl for Command, for a
keyboard remapped the Mac way (Toshy, for one, turns Command H into Super H,
which GNOME uses to hide the window, so Ctrl H never arrives). It is bound
as Ctrl and the `>` character, so it works on layouts where Shift . types
`>`, such as US and UK English.

The zoom keys act only while a PDF or the LaTeX preview is on screen, and
otherwise pass through to whatever has the keyboard. Fit width is Ctrl+Alt+0,
not the usual Ctrl+0, because Ctrl+0 is Focus the File Tree, as Command 0 is
on the Mac, and one of the ways out of the terminal.

While the terminal has the keyboard, the plain Ctrl shortcuts (Ctrl B, F, G,
H, I, N, O, Q, S, W, Space, slash, comma, plus and minus) and F12 belong to the shell, so
readline, vim and emacs get them: Ctrl W deletes a word, Ctrl H is backspace.
Everything with Shift or Alt in it still works there, except Ctrl Shift G,
which the shell reads as Ctrl G, and so do the keys that move between panes:
Ctrl `, Ctrl 0, Ctrl 1 and Ctrl Tab. Ctrl 0 reaches the Source Control panel
from the terminal while it is open. Ctrl click on a
link in the output is a mouse action and is not affected. The Mac has no such
split, since its shortcuts use Command and the terminal gets Control. The tree's right-click menu and the File menu both
have New File, New Folder, Rename, Move to Trash, Open Containing Folder and
Copy Path.
