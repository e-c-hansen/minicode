# Building MiniCode on Linux (GTK4)

MiniCode began as a native macOS editor. This is a port of the same editor to
Ubuntu Linux using GTK4. It reuses the exact same portable C++ core (the syntax
highlighter, the Markdown parser, the settings file parser, the comment
toggle, the folder search and the language server client) that the macOS
build uses, so highlighting, Markdown rendering, settings, Ctrl+/ and the
language server features behave identically. Since November 2026 it also
shares the command registry, and the rules behind tabs, sessions and
backups, the palette, find and replace, brackets, several cursors, folding,
and the window's layout (`linux/PARITY.md` is the plan for the rest). Only
the GUI layer is rewritten, from AppKit to GTK4.

Everything for the Linux port lives under `linux/`. The macOS build is
untouched.

## Install on Ubuntu

On Ubuntu 26.04 there is no need to build it: a Launchpad PPA has it, and
apt keeps it up to date.

    sudo add-apt-repository ppa:echansen/minicode
    sudo apt install minicode

The package is built from `debian/` at the top of the repository with every
panel on, and installs the same launcher and icons as `meson install` below,
under `/usr`. If you installed a build of your own into `~/.local` before,
remove that first, or it shadows the package. `packaging/PPA.md` has the
commands for that, and how the package is made and uploaded. The rest of
this file is for building MiniCode yourself.

## What is verified, and what is not

The port now builds clean and runs, with every panel enabled. It was brought up
on Ubuntu 26.04 with GTK 4.22.4, VTE 0.84.0 and WebKitGTK 2.52.3, compiling
without a single warning at `warning_level=2`.

Until October 2026 the checks below were temporary hooks inside the running
app, removed afterwards, real X input in a container, and runs by hand, and
the keys they name are the keys of the time (item 0.3 moved several; the
Linux column of `README.md`'s table has them as they are). Since November
(`linux/PARITY.md`) each feature has GUI suites in `linux/tests/gui` that
run the whole app on a private Xvfb, X11 with no window manager, and press
real keys and clicks through XTest; CI's GTK job runs them on every push.
They put nothing on a screen, so GNOME on Wayland, input methods, how
things look, and real hardware are left for a person: "What needs a
person", after this section, is that list.

Verified by running it:

- The control channel, part one (item 3.1): the `control` GUI suite runs the
  real `build/minicode open|window|state|selection` against the app. The
  socket is `<pid>.sock` (0600) in a 0700 folder; `open a.txt:3:6` shows the
  file with the caret there and `--to` selects a range; `state` and
  `selection` answer what is on show; with the keyboard in the editor an
  open lands in a tab beside the user's and the client says so; an unknown
  command, a misspelled field, a wrong type and a line that is not JSON are
  error answers over the raw socket, a cut-short request leaves the app
  answering, `--note`, a missing file and `term` exit 1 with the reason,
  `state` with no minicode exits 3, and stale sockets (a dead pid, a live
  pid with no listener) are removed while one with a listener is kept. The
  check fails with the server taken out. Not tested: another user's peer
  refused (one `SO_PEERCRED` comparison; it needs a second user).
- Loose ends of 3.2, 3.5 and 3.8 (November 2026): `compare` reads the pixel
  at the scroll bars' corner (`MCGuiPixel`), `agents` checks Show Terminal
  and the Worktrees node appearing with no relisting, `processes` notifies a
  session and reads it in `procs --json` and the row's tooltip.
- The compare view (item 3.2): the `compare` GUI suite, which opens a
  comparison through `Editor::showComparison`, checks the rows and fillers, the
  marked words, the syntax styles, the panes scrolling together, real F7 and
  Shift+F7 and 50,000 lines a side under a main-thread CPU bound. With a
  decision it clicks the gutter's check and cross for real, takes a choice
  back, watches the 3 s grace (shortened in the check) end and send, and
  checks Send Now, the bar's Accept and Reject and closing the tab, in the
  grace and before it, and the accepted text. The `control` suite runs
  `minicode diff` as a real client (two files, a file against HEAD in a real
  repository, the beside-the-user note, a bad `--base`, a missing file, a
  folder); both fail with the feature taken out. Not tested: a 16 MB file
  (the limit is a `stat` comparison), a window closed during the read.
- Processes and ports (item 3.8): the `procfs:*` core groups feed
  canned `/proc` text (a stat line with parentheses in the name, `btime`,
  `cmdline`, `smaps_rollup`, `net/tcp` and `tcp6` in LISTEN state) and a
  folder shaped like `/proc` to `ProcFs::Source`, and read the real `/proc`
  where there is one, so every CI job runs them. The `processes` GUI suite
  types `python3 -m http.server 0` into the app's terminal and has
  `build/minicode procs --json --folder` report it under the terminal with
  its port, cwd, command line and Pss; a sleep whose parent is gone and whose
  cwd is in the folder is "not started here"; Stop ends one by SIGTERM, the
  shell's child too, and a sleep ignoring SIGTERM by the SIGKILL 3 s later,
  and refuses minicode; a look's CPU time is bounded on the sampling thread's
  clock; a folder no window shows is answered for. It failed with the client's
  "procs is not on Linux yet" and passes. Part two, the panel: the same suite
  toggles View > Toggle Processes (the `toggleprocesses` window action, no
  key: the Mac has none), finds the server by its port under its terminal's
  group and the sleeps under "Not started here", reads a row's name, command,
  folder, footprint and tooltip, bounds the panel's rebuild by thread CPU
  time, opens a second window on the folder, runs a sleep that ignores SIGHUP
  in its terminal, closes the window and sees the sleep as an orphan, which
  its menu's Stop (no question) ends; a non-orphan's item reads "Stop…". It
  fails (checked once) with the toggle made a no-op. Not tested: the question
  dialog's buttons, Delete on a row, a real right click, the look on screen,
  other users' processes.
- Waiting for you (item 3.6): the `attention` GUI suite runs the real client
  with canned hook JSON against the app's terminal (the shell's
  `MINICODE_TERMINAL_ID` and `MINICODE_PID`, the dot and title count, a real
  key clearing the mark, Ctrl+Alt+W, `hooks --install` twice and onto a
  symlink in the scratch HOME). Not seen on a real desktop.
- The Agents panel and Evidence (item 3.5; the same suite also checks part
  two: a click on a session opens its Evidence page with the final message,
  the last test's summary, Show all and Open in Terminal typing without
  running; a real git repository with a worktree shows in the tree's
  Worktrees node with its ahead count, and Compare with Main lists its files
  and opens the compare view with main at the branch point; the fixtures are
  listed before and after). Part one: the `agents` GUI suite copies
  `tests/fixtures/claude` into a scratch transcripts folder
  (`MINICODE_CLAUDE_PROJECTS`, `MINICODE_CLAUDE_TMP`), presses Ctrl+Shift+A for
  real, and checks the sessions newest first with the running one marked,
  the subagent nested, the file groups, a click opening a file in the editor,
  a transcript appended to while shown adding a file, a file deleted
  struck through, the transcripts folder removed emptying the panel with its
  message, and a listing with sizes and dates of the whole scratch Claude
  folder unchanged by the panel's reading. It fails (checked once) with the
  toggle a no-op. Not tested: the look on a real desktop, a real
  `~/.claude/projects`, a right click.
- Notes and tours (item 3.4): the `notes` GUI suite runs the real client.
  `open --note` puts the file in the agent's preview tab and shows the box
  (text cleaned, label, x) over lines 10 to 12 with the text tag on exactly
  those lines and the caret there, the file's text and undo untouched;
  Escape (a real key) and a click on the x close it; a line break above
  moves it along, a letter typed on its lines closes it and undo does not
  bring it back; the next note replaces the preview tab and an edited one is
  kept; a note sent while the keyboard is in the editor waits with its tab
  (their caret unmoved, the client says so) and is placed when the tab is
  shown; `minicode tour` steps with Alt+] and Alt+[ to "2 of 3" and "3 of 3",
  stops at the ends, goes back, and the keys do nothing before a tour and
  after its note is closed; `state` names the note. 30 of its checks fail
  with placing switched off. Not tested: the box's look beyond one
  snapshot, a note in a Markdown preview or a large file.
- Agents checking pages (item 3.7): the `browser_pages` GUI suite, the real
  client against the running app, pages from a small HTTP server inside the
  test process (127.0.0.1) and from files, nothing off the machine. `browse
  --wait` of a file and of served pages (status 200 and 404 from the
  response); `page --url`, `--text`, `--console` (a log with its object, an
  uncaught error masked as "Script error." on a file page and with its
  message on a served one, the record cleared by the next page); screenshots
  (a PNG at a new path; refused onto an existing file, onto a symlink even
  with `--overwrite`, which replaces a plain file; `--full`); the text of
  about:blank refused; other schemes refused; `pane_busy` with the keyboard
  in the address bar; the Yours | Agent buttons and the x with real clicks; a
  typed or linked address going to the user's page; `open-url` of a local
  page landing in the user's page and a remote one going to a recording
  stand-in for xdg-open; `$BROWSER` in the app's terminal naming the script,
  which run as a program opens a page. The suite fails (42 checks) with the
  commands switched off. Not tested: a very tall `--full` page being cut,
  `control.page-any-origin`, Vite's `--open` or Claude Code's links, the
  look of the switcher.
- The Claude Code IDE link (item 3.9): the `idelink` GUI suite, with a
  WebSocket client built on the core's frame code and a scratch
  `CLAUDE_CONFIG_DIR` (never `~/.claude`; the wrapper sets it). The lock
  file is mode 0600 with the token, the folder and our pid, a stale one of
  minicode's own (a dead pid) is removed at start and another IDE's stays; a
  wrong token, no token and a Host off the loopback are refused with
  nothing said first; initialize, ide_connected and tools/list (no
  saveDocument or executeCode); selection_changed on a real selection
  (0-based lines, UTF-16 columns after an astral character); openFile
  with startText and endText (added beside the user's tab while the
  keyboard is in the editor, the note waiting; shown in the agent's preview
  tab with the highlight and an empty note once it is not); getDiagnostics
  from the scripted server, under the uri it was asked with, and for a file
  with none; openDiff accepted by the bar's real Accept click (FILE_SAVED
  and the text, the file untouched) and rejected by closing the tab;
  close_tab and closeAllDiffTabs touching only the diffs the link offered;
  the View menu's action writing `agent.ide-link` and the link, its lock
  file and its client following it; `CLAUDE_CODE_SSE_PORT` for terminals.
  Fails (checked once) with the link never started. Not tested: a real
  Claude Code, the per-hunk clicks of a diff (item 3.2's suite has them),
  the lock file removed at exit.
- Install hints (item 2.8): the `install_hints` GUI suite, with `MINICODE_INSTALL_HAVE=uv`,
  shows the bar for a missing Python server with `uv tool install basedpyright`,
  Copy, Run in Terminal typing it without Return, the bar going when the
  server appears, x and Don't Show Again; and Format Document's note for a
  missing formatter with `uv tool install pyink`, Copy and Run in Terminal.
  No real install was run.

- The portable C++ core in `src/` compiles cleanly under `g++` 15 and passes
  the top-level `make test`, the same suite the macOS build runs. This code
  is shared verbatim with the macOS build. CI runs it in the Linux job too.
- The port's own pure-C++ pieces pass their checks (`cd linux && make test`,
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
  left the editor filling the right area with the terminal still docked
  underneath (since item 1.13 the browser sits beside the editor instead).
- The terminal panel's top edge is a real `GtkPaned` divider. Moving it resizes
  the panel, the size survives closing and reopening it, and the clamps hold at
  both ends: the terminal will not go below 80px and the editor above it will
  not go below 120px, the same limits the macOS drag handler enforces.
- Collapsing the editor (Ctrl Shift E) gives the terminal the entire window, and
  the split comes back exactly where it was. The window can never be left empty:
  collapsing the editor opens the terminal if it was closed, and closing the
  terminal while the editor is collapsed brings the editor back. Opening a file
  or the browser while collapsed restores the editor too. (This item and the
  one before it were replaced in November 2026 by the Mac's layout,
  `linux/PARITY.md` item 1.13: the browser beside the editor, the terminal
  below both with bars of the window's own instead of `GtkPaned`s, the
  smallest top the tab strip and 40 px, and each toggle hiding only its own
  pane; with all three hidden the file tree has the window.)
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
- All 13 window actions were registered with the intended accelerators, and the
  sidebar, dotfile, terminal, browser and preview toggles were confirmed to
  change the state they claim to. (Since item 0.3 the actions, the menu bar,
  and their keys come from the command registry; see below.)
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

- Video and audio (September 2026, in the Docker container: aarch64, GTK
  4.22.4, GStreamer 1.28.2, X11 under Xvfb with llvmpipe, no sound device).
  No test hook: every file was opened by a real xdotool click in the tree
  and every key was a real X key event, with screenshots looked at. Clips
  were made with gst-launch-1.0 from `videotestsrc` (SMPTE bars scrolling
  sideways, so frames differ) and `audiotestsrc`.
  - With `gstreamer1.0-libav` installed, all fourteen play: h264.mp4 (with
    AAC), h264.mov, mjpeg.avi, theora.ogv, vp8.webm, vp9.mkv, and aac,
    flac, m4a, mp3, ogg, opus and wav. A video opens paused on its first
    frame with GtkVideo's play button over it, and the title reads
    `MiniCode — vp8.webm  640 × 360, 0:06`; audio shows its name over
    always-visible controls, titled `MiniCode — tone.mp3  0:05`. Ctrl Shift
    Space from the tree played each one: about 71% of the video's pixels
    changed between captures 0.7 s apart, and the audio page's time and
    scale moved. With no sound device, GStreamer played silently rather
    than failing.
  - On the stock packages (plugins-base and plugins-good, libav and ugly
    removed), everything played except h264.mp4, h264.mov, tone.m4a and
    tone.aac, which show "Cannot play", the two packages to install, and
    GStreamer's own "Your GStreamer installation is missing a plug-in."
    Installing libav while MiniCode ran did not help until it was
    restarted, which the message says. A file of random bytes named .mp4
    says "Could not determine type of stream." With `GTK_MEDIA=none` (GTK
    with no media backend) a video shows the message about GTK's GStreamer
    support, and Ctrl Shift Space does nothing. Nothing crashed.
  - Switching from a playing video to a text file ends GStreamer's GstPlay
    thread at once; its streaming threads are pooled by GStreamer and gone
    within 20 s. Move to Trash on a playing file and Ctrl W on a window
    playing a video also end the GstPlay thread.
  - Replacing a playing 20 s video on disk (a copy renamed over it) reloaded
    it at 4.2 s, still playing; replacing it while paused reloaded it at
    6.95 s, still paused, which the controls showed as 0:06 of 0:20.
  - Ctrl Shift Space with the terminal focused left the video alone and
    reached the shell (`cat -v` printed `^@`); with the commit message box
    focused it left the video alone and typed nothing; from the tree it
    played. The hints list it under "Video and audio" while a file that can
    play is on screen.
  - The build is warning-free and `meson test` passes. Two faults found on
    the way are fixed: the new pages' minimum height made GTK warn "Trying
    to measure GtkBox for height of 46" (from launch, and every second a
    video played), and the audio controls were dark grey on the dark
    editor background.
  - Not fixed, and not ours: each video opened leaves one idle
    `gstglcontext` thread and one `gldisplay-event` thread behind. A
    standalone GtkVideo program that opens and releases the same way
    leaks the same, also when it reuses one GtkMediaFile, so it is in
    GTK 4.22 and GStreamer 1.28 under X11 with llvmpipe. Audio does not
    use GL.
  - Not seen: real sound, GNOME on Wayland with a real GPU (the ThinkPad),
    a HiDPI screen, whether the GL thread leak happens there, and Ubuntu
    24.04's `libgtk-4-media-gstreamer` package, whose name is Debian's and
    was not checked in a container.

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
    the tree, all three were bound again. (Since item 1.11 the same keys
    zoom the editor elsewhere and the terminals in the terminal.)
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
    forward to the image again. (Tabs replaced it in item 1.1.)
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
    where it stays bound like the other shifted keys. Ctrl+H worked too
    then; item 0.3 gave it to Replace.
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
    read. (256 MB since November 2026, files over 4 MB read on a worker
    thread: `PARITY.md` item 1.12.)
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
- The GUI checks themselves (November 2026, `linux/PARITY.md` items 0.1
  and 0.2). `meson test -C build --suite gui` runs the whole app, with
  `main` renamed, on an Xvfb it starts itself under `dbus-run-session`, with
  a scratch home, settings, and git config, and presses keys and clicks
  through XTest, so GTK's own accelerator matching and gestures run as they
  do for a keyboard and mouse. It refuses any display it did not start;
  four runs at once pass, and nothing outlives a run, a hung step, a crash,
  or a SIGKILL to its wrapper. Its first suite types into a file and saves
  it with Ctrl S, and fails with the save action taken out. CI's GTK job
  runs every suite: a check made to fail turned it red, with a picture of
  the window kept, and WebKit's sandbox starts on the runner.
- The keys from the command registry (November 2026, `linux/PARITY.md` item
  0.3), by the `keys` GUI suite on a private Xvfb with real key presses.
  The menu bar holds each command the port has once, in the registry's
  order and Linux's words, and each action's accelerators are its keys in
  `src/CommandsLinux.cpp`. Every key that moved does its job and its old key
  no longer does: Ctrl Shift V toggles the preview and Ctrl Shift P nothing,
  Ctrl Alt L and (in Markdown only) Alt C toggle a task and Ctrl L nothing,
  Ctrl Shift D opens the TODOs and Ctrl Shift L nothing, Ctrl N asks for a
  new file's name, Ctrl Alt N a folder's, Ctrl Shift N opens a window that
  Ctrl W leaves and Ctrl Shift W closes, Ctrl Shift . shows dotfiles and
  Ctrl H nothing, F3 opens the find bar and Ctrl G nothing, Ctrl Alt I runs
  the hover and Ctrl I nothing (since then Ctrl L, Ctrl Shift L, Ctrl H,
  and Ctrl G have taken new jobs: Select Line, Select All Occurrences,
  Replace, and Go to Line). With the terminal focused, a raw `head`
  received Ctrl N, L, H, G, W, I, B, O, Q, F, Ctrl Alt L, Ctrl Alt I and F3
  as their bytes, in order, while no command ran; Ctrl Shift V pasted into
  the shell; Ctrl Shift D, Ctrl Shift N, Ctrl Shift W and Ctrl Alt N acted,
  and Ctrl 0 moved the keyboard to the tree. A scratch keys.conf moved the
  preview to Ctrl Alt J and Rename in the tree from F2 to Ctrl Alt R, the
  hints showed the new keys, its Mac line (`cmd+k`) was named in the status
  bar, and emptying the file put every key back. Edit > Keyboard
  Shortcuts… wrote the file with only the commands the port has.
- Tabs (November 2026, `linux/PARITY.md` item 1.1), by the `tabs`,
  `preview_tabs`, and `tab_viewers` GUI suites on a private Xvfb with real
  key presses and clicks. Each file has a tab with its own buffer, caret,
  scroll, and undo; the strip, Ctrl Page Down and Up, Ctrl Tab and Ctrl Shift
  Tab (wrapping), and Alt 1 to 9 and Alt 0 switch them. A tab undone back to
  the disk's text loses its dot. Ctrl W on a dirty tab asks, and Cancel,
  Don't Save, and Save each did their part; a window with two dirty tabs
  asked once. Ctrl Alt Shift T reopened tabs newest first with their caret
  and scroll, and skipped a removed file; Ctrl Alt S saved every dirty tab;
  two README.md files were labelled by folder. The tab menu's Close Tabs to
  the Right, Close Other Tabs, and Copy Path did their part, and a middle
  click on a tab or its close button closed it. A single click in the tree or Find in
  Folder opens a preview tab (italic) that the next replaces; a double
  click, Ctrl click, typing, saving, a double click on its title, and Keep
  Open keep it. Pictures, PDFs, the LaTeX preview (one per tab, kept while
  another shows), and the shared diff tab show in tabs. The Markdown preview
  renders into a buffer of its own: typing in the source survived toggling
  it and switching tabs, a task box clicked in it was undone from it, and
  Ctrl Z went back to the clean file. By hand against clangd, squiggles
  followed the tab on show and a fix typed in a tab cleared them.
- Sessions, backups, and auto-save (November 2026, `linux/PARITY.md` items
  1.2 and 1.3), by the `sessions`, `backups`, and `auto_save` GUI suites. A
  second app opened a folder's files (a Markdown page, a named pipe, a
  17 MB file, one deleted afterwards, the preview tab) and closed with Ctrl
  Shift W, and a third got them back in order with their caret and scroll,
  without the deleted one, reading only the tab on show; two windows on one
  folder share its session. A second app that typed into three tabs and was
  killed with a real `kill -9` came back in a third, which opened a window
  on that folder by itself, as unsaved tabs over the disk's text, with
  nothing to undo, the note in the status bar, the question about a file
  changed meanwhile, and a deleted file's text; Ctrl Q with Don't Save left
  no backup. Auto-save after a delay and on a change of focus (Ctrl 0, a
  tab switch) saved, Close Tab wrote instead of asking, and a file changed
  on disk, one deleted, and a dead key still composing were left alone. The
  browser's page is not in the session yet.
- Line numbers and the gutter (November 2026, `linux/PARITY.md` item 1.4),
  by the `gutter`, `ruler`, and `gutter_typing` GUI suites. The numbers
  drawn match an index made afresh and GTK's own line positions after
  typing that adds and removes lines and pastes with a lone CR, U+2029, and
  U+2028; the current line's color follows the caret; relative and off
  work; the colors, `editor.opacity`, and the rulers at their columns were
  read back as pixels; real clicks, drags, and Shift clicks select whole
  lines; a Markdown preview has no gutter; and typing in 100,000 lines
  costs the gutter 0.05 ms of CPU a key.
- The palette (November 2026, `linux/PARITY.md` item 1.5), by the `palette`
  and `symbols` GUI suites on a private Xvfb with real key presses and
  clicks. Ctrl Shift P and F1 list the commands with their keys, only those
  whose action is enabled; a fuzzy query ranks Toggle Terminal and Show
  Hidden Files first and Enter runs them, and the command run last comes
  first next time, read from `$XDG_STATE_HOME/minicode/palette.json`.
  Escape, a click outside, or running a row gives the keyboard back, to the
  editor or to the find bar with its selection. Ctrl P lists the folder's
  files off the main thread, dotfiles in and node_modules out, and opens
  one by Enter or a click, tabs and earlier picks first. Ctrl G goes to a
  line and column. Ctrl Shift O lists a Python file's classes and methods
  and a Markdown page's headings, shows each as the list moves, and
  Escape puts the selection and scroll back. In the terminal Ctrl P, Ctrl
  G and F1 reached a raw `head` as their bytes, and Ctrl Shift P opened the
  palette.
- Find and replace (November 2026, `linux/PARITY.md` item 1.6), by the
  `find_bar` and `replace_in_folder` GUI suites. Ctrl F seeds the query from
  a one-line selection or the word at the caret, the options change the
  count, a regular expression's groups go into the replacement, Preserve
  Case and Find in Selection work, Enter, F3, and their Shift forms wrap,
  Replace and Replace All undo in one step, and Escape closes the bar from
  the bar or the editor. Replace All of 20,000 matches in a 100,000-line
  file took about 170 ms of main-thread CPU and its undo 1.6 s. In the
  terminal the options' Alt keys reach the program. Replace in Folder
  counted and asked, changed a clean tab in place, left a tab with unsaved
  text alone and named it, replaced a symlinked file once, refused a stale
  row, and kept to the files to include and exclude.
- Brackets and auto-closing pairs (November 2026, `linux/PARITY.md` items
  1.7 and 1.8), by the `brackets`, `bracket_pairs`, and `auto_close` GUI
  suites. The bracket by the caret and its partner are boxed (read back as
  pixels), strings and comments skipped, nothing looked for past 20,000
  characters, and Ctrl Shift backslash goes out and back; the three depth
  colors and the unexpected closer were read as tags and pixels, painted
  only for the lines on screen, with typing in 100,000 lines at 7 to 9 ms a
  key either way. Every auto-closing case was typed through XTest: the
  pair with the caret inside, one Ctrl Z taking it, typing over the closer
  and Backspace between the two, Python's quotes, a selection wrapped,
  Markdown's own pairs, nothing while a dead key composes, and the setting
  off.
- Several carets and the line commands (November 2026, `linux/PARITY.md`
  item 1.9), by the `multicaret`, `multicaret_select`, `line_commands`,
  `editor_words` and `multicaret_perf` GUI suites on a private Xvfb with
  real key presses and clicks. Alt click adds and takes away carets, Shift
  Alt, Alt and middle-button drags make columns, Ctrl D, Ctrl Shift L and
  Ctrl Shift Up and Down add carets, and the find bar's Alt Enter selects
  its matches; typing, brackets closing, Backspace, Return, the arrows,
  Copy and Paste act at every caret, one Ctrl Z taking a key back; Ctrl L,
  Ctrl Shift K, Alt Up and Down and Shift Alt Up and Down work on the
  caret's lines, Ctrl C and Ctrl X with nothing selected take the whole
  line, pasted above; double-clicks and Ctrl Left and Right go by VS Code's
  words; and a key typed at 1,000 carets over 100,000 lines takes 35 to 55
  ms of the main thread in its edit.
- Folding (November 2026, `linux/PARITY.md` item 1.10, with D8's limit of
  5,000 hidden lines a tab), by the `folding`, `fold_budget`, and
  `foldperf` GUI suites. Ctrl Shift [ folds the innermost region and then
  the next one out; the gutter numbers only the lines on show; the arrows
  step over a fold; edits above and in a fold move or open it; a copy across
  one takes what it hides; Go to Line and Find open what hides their
  target; the chevrons and the placeholder answer clicks; Backspace,
  Delete, and Return beside a fold act as on the Mac; folds stay with their
  tab and go on a reload; large-file mode has none; and Fold All past the
  limit closes what fits and says how many stayed open. Fold All and Unfold
  All of 20,000 regions stay within a CPU bound.
- Zoom, word wrap, Go Back, and Maximize Pane (November 2026,
  `linux/PARITY.md` item 1.11), by the `zoom`, `word_wrap`, `go_back`, and
  `maximize` GUI suites. Ctrl equals, Ctrl minus, and Ctrl with keypad 0
  zoom the editor and its Markdown page a point at a time, kept across
  launches in `$XDG_STATE_HOME/minicode/zoom.json`; the same keys zoom a
  PDF while one shows, the browser's page while it has the keyboard, and
  only the terminals while a terminal has it (`tput cols` read fewer
  columns). Source wraps by default and Alt Z turns that over for the
  session; a 30,000-character line and a file in large-file mode do not
  wrap. Go Back and Forward follow the ten-line rule through tab switches,
  Go to Line, symbols, a scripted language server's definition, a Find in
  Folder result, a rename, and a deletion. Ctrl Alt M gives the pane with
  the keyboard the right side and puts the others back.
- Large files (November 2026, `linux/PARITY.md` item 1.12), by the
  `large_file` GUI suite. A 25 MB file is read in the background (0.33 to
  0.37 s of main-thread CPU in all, never more than 21 ms at a time),
  without colors or a language server and with the status bar's note, a
  real click on which turns them on; typing costs 0.07 to 0.18 ms a
  key. A
  300,001-line file is large too, a 70 MB file asks first, and a 5 MB line
  opens and types without holding the main loop (33 and 10 ms at most),
  shown 10,000 characters at a time, with End, the find bar, and Copy
  reaching its hidden part.
- The window's layout (November 2026, `linux/PARITY.md` item 1.13), by the
  `layout` GUI suite in a window of its own. The browser sits right of the
  editor and the terminal below both, across the whole width. The bar
  between the editor and the browser and the terminal's bar were dragged
  for real: within 16 px of an edge a side hides, and back out in the same
  drag it returns. The three panes hidden in all six orders stay hidden,
  leaving the tree the window; a click in the tree, the preview key, or
  Focus Editor brings a hidden editor back; and the sidebar's divider no
  longer takes presses inside the editor.
- The file tree under churn (November 2026, `linux/PARITY.md` R1), by the
  `tree_stall` and `tree_clicks` GUI suites. With 10,000 files deleted and
  made again in an open folder, real clicks in the tree met a stall of 30
  to 60 ms of CPU at most, against 18.2 s before, and a listing that lands
  while the button is down no longer opens the wrong file. The owner's
  freeze itself was not reproduced: none of their folders is that large.
- `minicode` from a terminal (November 2026, `linux/PARITY.md` R2), by the
  `detach` GUI suite, which runs the real `build/minicode` on
  pseudo-terminals with `/bin/sh` as the shell, on the checks' Xvfb and a
  session bus of its own. `minicode <folder>` returned 0 in under half a
  second while its window stayed up, in a session of its own with no
  controlling terminal and `/dev/null` on its standard streams; it asked
  systemd (answered by the check) for `app-minicode-org.minicode.Editor-<pid>.scope`
  and carried on when refused; closing the terminal's master and sending
  SIGHUP to its session left it running. A second `minicode notes.txt`
  returned 0 and the running window showed the file. Started with no
  terminal, as the desktop entry starts it, it did not fork.
  `minicode --foreground` held the terminal in its session until the
  hang-up closed it, and `--version` and `-h` printed and returned.
- The time to a window (November 2026, `linux/PARITY.md` item R3), by the
  `startup` GUI suite on a private Xvfb and by `MINICODE_STARTUP_LOG=1`
  runs on `~/minicode`: a window opening with the terminal and the browser
  hidden starts no shell and no WebKit process, the language server for a
  session's tab starts after the first frame, and both panes work once
  shown. The first frame comes about 190 ms after main with no session and
  220 ms with five tabs, 55 to 80 ms sooner than before, and the app with
  what it starts uses a seventh of the CPU it did in its first seconds.
- Format Document and the folder's own tools (November 2026,
  `linux/PARITY.md` items 2.3 and 2.4), by the `tools_consent`,
  `server_consent`, `format`, `format_on_save`, `format_consent` and
  `format_coverage` GUI suites on a
  private Xvfb, with real keys and clicks and stand-in formatters and
  servers (scripts that record being run, and the scripted language
  server): nothing in a folder's `.venv` or `node_modules`, and no
  configuration that runs code, runs before the folder's answer allows it,
  and a server standing in for rust-analyzer starts restricted or waits;
  Ctrl+Shift+I formats in one undo step, keeping the caret; failures,
  output that is not UTF-8 and the 10 s limit leave the buffer alone with a
  note; format.on-save formats before Ctrl+S writes; and a file nothing
  else formats goes to its language server.
- The Source Control panel's commands (November 2026, `linux/PARITY.md`
  item 2.9), by the `git_sync`, `git_actions`, `git_hunks` and `git_verbs`
  GUI suites on a private Xvfb with real key presses and clicks, over
  scratch repositories and a bare remote. Fetch, Pull, Push and Sync from
  the palette, the panel's right click and the Sync Changes button, a
  branch published, a push over ssh without a key refused at once with
  ssh's words and git in a session of its own; autofetch on its tick, its
  period and its backoff; Stage All and Unstage All; Discard Changes asking
  first, restoring from the index, moving an untracked file to the Trash
  (a scratch folder, and once the scratch world's own GIO Trash), and Undo
  Discard; Stage Hunk, Unstage Selected Lines and Discard Hunk from the
  diff's links and right click, a stale diff refused; the branch picker
  switching, tracking a remote branch, creating one and refusing a bad
  name; Commit, Commit and Amend, Undo Last Commit, Stash and Pop Stash;
  several rows by Ctrl and Shift clicks and Shift with the arrows; Discard
  refused for a submodule, a file added with git add -N, a conflict and a
  staged row, and Stage All asking first while a conflict is left.
- Completion's edits, snippets and the signature hint (November 2026,
  `linux/PARITY.md` item 2.5), by the `completion_keys`, `snippets`,
  `completion_edits` and `signature_help` GUI suites on a private Xvfb with
  real key presses, against the scripted language server: the list coming
  up by itself on a word's second letter; Tab and Return taking the item
  (a word typed out, the arrows, a letter and Tab pressed together, inside
  auto-closed brackets, at a snippet's stop), Shift+Return and Escape not;
  a snippet expanded with its line's indentation and the file's unit, its
  stops walked by Tab and Shift+Tab and drawn, ended by Escape, by the
  caret leaving and at its end; a mirror following typing at two carets; a
  choice's options listed and taken at both places; `$CLIPBOARD`; an import
  added with its word and taken back with it by one Ctrl+Z; a word that
  only inserts an undo step apart from the typing before it; an import
  that comes from resolve half a second later joining the word's undo
  step, or a step of its own after typing, dropped after an undo, left out
  with a note after an edit above, and noted when it takes over a second;
  and the signature hint on "(" and ",", closed by Escape.


Not verified, for the work before November (what the November work leaves
to a person is in "What needs a person", below):

- For the Source Control panel, everything that needs GNOME on Wayland: the
  look under the desktop's own theme (the button and check box are styled
  from the sidebar colors, the rest follows the theme), the selection and
  focus colors at 2x, tooltips on a real pointer, the window-focus refresh
  when switching from another application, and a real repository with many
  branches in All branches mode.
- Much of it by real keyboard and mouse input. The September hooks
  activated actions programmatically, which proves the wiring but not the
  key handling, and the GUI suites since press only some of those keys, on
  X11. That includes scrolling a PDF with the wheel and clicking a page (the mapping
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
  Ctrl+Space, F12, Ctrl+I (now Ctrl+Alt+I), a Ctrl+click (its hit test in particular),
  clicking a row of the list and resting the pointer on a word were not
  pressed or done for real. Whether an input method takes Ctrl+Space first
  is also open. Only clangd was tried.
- PDF zoom by the wheel and the touchpad. Since item 1.11 the `zoom` suite
  presses the zoom keys with a PDF on show, but Ctrl+wheel and a touchpad
  pinch were not done at all: their
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
- For item 8: some shortcuts as real key presses. The `keys` suite now
  presses the keys item 0.3 moved, and the plain Ctrl keys typed into the
  terminal (see above), and the tabs and find bar suites press Ctrl+Tab and
  Shift+Enter, but Ctrl+1, Ctrl+Shift+G from the terminal and Ctrl+Shift+C
  in the terminal are still not pressed by any check, and whether F10
  (GTK's menu bar key) still reaches the terminal was not looked at. None
  of the keys was pressed under GNOME on Wayland with an input method,
  where GNOME or IBus could take one first. How the color popover looks in place (it follows the system's
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
   and held onto half the space when hidden. The editor was then hidden while
   the browser was up, and the revealer expanded only while revealed. Since
   item 1.13 the window lays its panes out itself, the browser beside the
   editor (`linux/HANDOFF.md`, item 18).

4. **GtkTextTag setup and CSS.** The tag properties and the
   `.minicode-editor text { ... }` node selector are all correct; the editor and
   Markdown preview render with the intended VS Code dark palette.

## What needs a person

The GUI checks run on Xvfb, an X11 display with no window manager, no IBus,
and no GPU, so what follows is left for someone at the ThinkPad under GNOME
on Wayland. It gathers every "Needs the owner" note of the items done in
`linux/PARITY.md` (Stage 0, Stage 1, and R1 to R3), merged, and should take
about an hour; what the work before November left unchecked is under "Not
verified" above. Write down anything that does not hold, with what you
saw.

First quit every minicode, the packaged one too (a second `minicode` hands
its folder to the one already running), build main (`meson compile -C
build` in `linux/`, set up as in "Build with Meson" below), and keep
scratch files in a folder of their own, such as `~/mc-check`.

### Starting it (R2, R3)

- [ ] In GNOME Terminal, from `linux/`, run `./build/minicode ~/minicode`.
  The prompt comes back at once, and the window is up in well under a
  second.
- [ ] While it runs, `systemctl --user list-units 'app-minicode-*'` lists
  one `app-minicode-org.minicode.Editor-<pid>.scope`, loaded and active.
- [ ] Close that GNOME Terminal window: it asks nothing about a running
  process, and minicode stays open.
- [ ] Quit minicode, run `dev/startup-times.sh build/minicode ~/minicode`
  from `linux/` (it opens and closes the window three times for each of
  GTK's four renderers), and keep what it prints: what the GPU adds to the
  first frame (`linux/HANDOFF.md`, item 24).

### Keys under GNOME on Wayland with IBus

Ubuntu runs IBus by default; leave it on. Each key should do its own job,
not something of GNOME's or the input method's.

- [ ] In a Markdown file, Ctrl Alt L makes the caret's line a task and,
  pressed again, ticks it, without locking the screen; Alt C does the same.
- [ ] With three files open, Alt 1, Alt 2, and Alt 3 switch between them,
  Ctrl W closes one, and Ctrl Alt Shift T brings it back at its place.
- [ ] F1 opens the command palette, and Ctrl Shift O the open file's
  symbols.
- [ ] With the find bar open (Ctrl H), Alt C, Alt W, Alt R, Alt P, and Alt
  L each turn their button on and off.
- [ ] Ctrl Alt M gives the editor the whole right side, and again puts the
  panes back; Alt Z turns wrapping off and on; in a long file, Ctrl End and
  then Ctrl Alt minus goes back to where you were.
- [ ] With Num Lock on, Ctrl equals twice and then Ctrl with keypad 0 puts
  the editor's text back to its size.
- [ ] In a source file, Ctrl Shift [ folds the block at the caret and Ctrl
  Shift ] unfolds it. If you use a layout where Shift [ types no brace, say
  which keys fold there, if any.
- [ ] Alt click adds a caret, and a Shift Alt drag makes a column of them.
- [ ] Read through the menu bar: each command once, in a sensible menu,
  with Move Line and Copy Line at the end of Edit.

### Input methods

- [ ] Add the layout English (US, intl., with dead keys), and in a Python
  file type ' and then e: one é, and no closing quote. Type ( and get ()
  with the caret between.
- [ ] If you use an input method that composes words (Pinyin, say), type
  with it: the text goes in once, at the caret. (At several carets it goes
  in at the first only; that is known.)
- [ ] If you use a layout where brackets need AltGr (German, say), type (
  with it: it closes as () too.
- [ ] Set `editor.auto-save = focus-change` in the settings (Ctrl comma),
  type in a file, and Alt Tab to another app: the tab's dot goes and the
  file on disk has the change. Type again and click the file tree: saved
  again. Put the setting back afterwards.

### How it looks

Go through these once as you have it, once with `theme = light-modern` in
the settings, and, if you can, once with the display at 200% in Settings >
Displays.

- [ ] The tab strip: the names, the preview tab in italics, the dot on an
  unsaved tab, the ×.
- [ ] The gutter: each number level with its line, the caret's line's
  number brighter, the ruler at column 100, and a red dot beside a line
  with an error (an undeclared name in a C++ file, with clangd). Press Ctrl
  equals a few times: the text and the numbers grow together, nothing cut
  off.
- [ ] Brackets: a box around the bracket by the caret and its partner,
  nested brackets in three colors, and a stray ) in red.
- [ ] Several carets (Ctrl Shift L on a word): every caret and selection
  drawn (the extra carets do not blink).
- [ ] Folding: arrows in the gutter while the pointer is over it, and the ⋯
  after a folded line.
- [ ] The find bar with its replace row (Ctrl H), the palette (Ctrl Shift
  P), and Replace in Folder's window (Ctrl Alt Shift F): readable and
  aligned, and "No results" in red.
- [ ] The bars between the editor, the browser (Ctrl Shift B), and the
  terminal (Ctrl Shift T), and the pointer over them.
- [ ] A large file (below): "Opening…" while it reads, the status bar's
  "Large file: features off", and "… N more characters" after the shown
  part of a long line.

### Mouse and touchpad

- [ ] Drag the bar between the editor and the browser, and the terminal's
  top bar, with the mouse and with the touchpad. Each grabs within about 4
  px of its line, follows smoothly, hides a side when let go within 16 px
  of an edge, and brings it back when dragged out again in the same drag.
- [ ] A middle-button drag in the editor makes a column of carets, and so
  does a Shift Alt drag on the touchpad; a middle click still pastes the
  primary selection.
- [ ] Fold All (Ctrl Alt 0) in a long source file and scroll through it
  with two fingers on the touchpad: smooth, with no jumps at the folds.
  Then Unfold All (Ctrl Alt J), and say whether GTK's settling afterwards
  is noticeable.

### Real folders, files, and programs

- [ ] Ctrl P in a big real folder (your home folder, or a large checkout):
  the list fills as you type, and the window never stalls.
- [ ] Open a real log over 20 MB and a minified JavaScript file: no "not
  responding", quick typing, and End reaching the end of the long line.
  Right after the log opens, Ctrl G to a line far down: say how long the
  view jumps about before it settles. Select all of the long line, Ctrl C,
  and paste it elsewhere: all of it comes. A middle click pastes only the
  shown part (GTK's; say whether that matters).
- [ ] The tree while a build runs: open `~/minicode`, run `make test` in
  its terminal, and click around the tree meanwhile, `build/` included. If
  "not responding" ever shows, note the folder, whether hidden files were
  shown, and what the click opened (R1).
- [ ] Go Back with clangd and terminal links: in `demo/vec/main.cpp`, F12
  on `scaled` opens vec.h, and Ctrl Alt minus comes back. In the terminal,
  run `grep -Hn scaled demo/vec/vec.h`, Ctrl click the result, and Ctrl Alt
  minus comes back to main.cpp.
- [ ] Replace All with the desktop's accessibility running: make
  `~/mc-check/big.txt` with `seq 100000 | sed 's/$/ foo/'`, open it, Ctrl
  H, foo for bar, Ctrl Alt Enter. Say how long it takes, and how long Ctrl
  Z takes (the checks, without accessibility, measured about 0.2 s and
  1.6 s of CPU).
- [ ] The primary selection on Wayland: select a word in the editor and
  press Ctrl F. The query is the word and the word stays selected in the
  text; a middle click in another app pastes it.

### Formatting and a folder's own tools (items 2.3 and 2.4)

- [ ] In a Python project of yours with Ruff configured, Ctrl Shift I
  formats the file with your real ruff in one undo step, the caret staying
  on its line; with three lines selected, only those change.
- [ ] Open a folder with its own `.venv`: the bar under the tabs asks before
  the language server uses it. Choose Use It, and hover or completion then
  knows the packages installed there; Edit > Ask Again About This Folder's
  Tools brings the question back.
- [ ] How the bar under the tabs, the question, and a formatter's note by
  the caret look, and that Ctrl Shift I reaches minicode with IBus running.

### Completion and snippets (item 2.5)

- [ ] With real clangd (C++) and basedpyright (Python): the list coming up
  as you type, Tab and Return taking an item, an auto-import or `#include`
  going in with its word (one Ctrl Z takes both back), clangd's snippets
  (a function's arguments as stops that Tab walks), and the signature hint
  after `(` and `,`.
- [ ] How the stops' boxes, the list and the hint look, and Tab, Shift Tab
  and Escape under GNOME on Wayland with IBus running.

### Semantic tokens (item 2.6)

- [x] The `semantic_tokens` suite (scripted server, real painting over a
  400-line file) and the core's `lsp:semantic-*` groups.
- [ ] With real basedpyright, clangd and rust-analyzer: the colors of
  classes, functions, parameters and properties, strings and comments keeping
  theirs, and scrolling a long file painting lines as they come in.
### The control channel (item 3.1, part one)

- [ ] From a real terminal under GNOME: `minicode open file:12:3` while the
  app runs raises nothing it should not and puts the caret there; typing in
  the editor meanwhile keeps the keyboard. `minicode state --pretty` and
  `minicode selection` with a real selection. `minicode open file` with no
  minicode running starts the app, waits for its socket and opens the file.
  The socket is under `/run/user/<uid>/minicode`.

### Symbols and folding ranges from the servers (items 2.12 and 2.13)

- [ ] With real clangd and basedpyright: Ctrl Shift O listing the file's
  symbols from the server, Ctrl T finding symbols across the folder and
  opening one (Go Back returns), and the gutter's folds following the
  server's ranges after a pause in typing. Ctrl T in the terminal stays the
  program's.

### Source Control (item 2.9)

- [ ] Push, pull and autofetch
  against a real remote with ssh keys, an agent, a GUI askpass or a
  credential helper; the desktop's real Trash and Restore From Trash in
  GNOME Files after a discard; how the menus, the hunk links and the Sync
  button look; Ctrl and Shift clicks and Shift F10 under GNOME on Wayland.

### Sessions and losing work

- [ ] The Claude Code IDE link (item 3.9): turn on View > Claude Code IDE
  Link, start the real Claude Code in the window's folder, run /ide and
  pick minicode; select some lines and see them in Claude's footer; ask it
  to open a file and to change one (with Diff tool set to auto in /config)
  and accept and reject a diff in the tab. Needs the owner's Claude Code
  login; no agent runs it.
- [ ] Logout: with tabs open on a folder, one with unsaved typing, log out
  of GNOME and back in, and start minicode on that folder. The tabs come
  back at their places, and the unsaved text comes back unsaved, with the
  file on disk as it was. Note whether the logout waited on minicode or
  asked anything.
- [ ] A crash: type in a tab without saving, wait two seconds, find
  minicode's pid with `pgrep -a minicode`, and `kill -SEGV` it. Start
  minicode again: a window on that folder opens with the text back unsaved.
- [ ] Last, a power loss: the same, but hold the power button until the
  machine is off instead of killing minicode, then boot and start it.

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
- Pictures with an https address in the Markdown preview are fetched with
  libsoup 3 (pkg-config module `libsoup-3.0`), which `libwebkitgtk-6.0-dev`
  already pulls in. Without it they show their alt text.
- `libpoppler-glib-dev` provides the PDF viewer (pkg-config module
  `poppler-glib`). Without it a PDF shows the "Cannot display" message, and
  a `.tex` file opens as highlighted source, with no LaTeX preview.
- The LaTeX preview typesets with [tectonic](https://tectonic-typesetting.github.io/),
  a separate program, not a build dependency. MiniCode uses `MINICODE_TECTONIC`,
  then `~/.local/share/minicode/bin/tectonic`, then one on your PATH, and if
  there is none the preview offers to download the official 0.17.0 build
  (about 10 MB, checked against its published checksum) into
  `~/.local/share/minicode/bin`. It needs `curl` or `wget` and `tar` for that.

- Video and audio play through GTK's own media support, which is GStreamer,
  so they add no build dependency. On Ubuntu 26.04 GTK's GStreamer backend
  is part of `libgtk-4-1` itself; on Ubuntu 24.04 and older Debian it is the
  separate package `libgtk-4-media-gstreamer`. What plays then depends on the
  GStreamer plugins installed. A stock Ubuntu desktop has
  `gstreamer1.0-plugins-base` and `gstreamer1.0-plugins-good`, which play
  WebM, Matroska (VP8, VP9), Ogg (Theora, Vorbis, Opus), MP3, FLAC, WAV and
  MJPEG AVI. H.264 video (most .mp4 and .mov files) and AAC audio (.m4a,
  .aac) need `gstreamer1.0-libav`:

      sudo apt install gstreamer1.0-libav

  A file GStreamer has no decoder for shows a message naming these packages
  in place of the player. GStreamer reads its plugin list when MiniCode
  starts, so restart MiniCode after installing one.

If you only install `libgtk-4-dev`, the core viewer still builds, images
included. The terminal, the browser and the PDF viewer are compiled out
automatically when their libraries are absent.

## Build with Meson (recommended)

    cd linux
    meson setup build
    meson compile -C build
    meson test -C build
    ./build/minicode ~/some/project

The GUI checks are a suite of their own, which plain `meson test` leaves out:
`meson test -C build --suite gui` runs the whole app on a private Xvfb with
real key presses and clicks (`linux/tests/gui/Harness.h`). They need `xvfb`,
`libxtst-dev`, and `dbus-run-session` (in Ubuntu's `dbus-daemon`), and are
not built without `libxtst-dev`. CI's GTK job runs them on every push to main
and every pull request.

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
launcher again, so it opens a window too. Inside the app, Ctrl+Shift+N opens a
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

Typed in a terminal, `minicode` gives the terminal back once its window is
up, in a session of its own, so closing the terminal leaves it open, as on
the Mac and as VS Code's `code` does. Its output then goes nowhere, unless
you sent it to a file. `minicode --foreground` keeps it in the terminal,
with its output there, until it quits; `-h` lists the options and `-v`
prints the version. The Mac's `-n` and `-r` are refused, since the port has
neither. Started from the desktop's launcher, it does not fork at all.

The window shows the file tree on the left and the editor on the right, its
tabs above it, with a status bar along the bottom. When shown, the browser
sits right of the editor and the terminal below both, across the whole
width, as on the Mac.

To see where the time to a window goes, run it with `MINICODE_STARTUP_LOG=1`
(such a run stays in the terminal):
each phase of the start is printed to the terminal with the time since the
program began and the CPU time it took, from GTK's own start to the first
frame drawn. `linux/dev/startup-times.sh BINARY [FOLDER]` does this three
times for each of GTK's renderers and prints the phases (quit every other
minicode first; `linux/HANDOFF.md`, item 24).

## Keyboard shortcuts

The keys are the Linux column of the table in `README.md` ("Keyboard
shortcuts"). They come from one table, `src/CommandsLinux.cpp`, beside the
Mac's in `src/Commands.cpp`, which builds the menu bar, the accelerators, the
file tree's keys and the shortcut hints, and a core test holds the README's
column to it. They follow the Mac's set by `linux/PARITY.md`'s rule D1: VS
Code's Linux key for a feature VS Code has, minicode's own Shift Command keys
as Ctrl Shift with the same letter, and its Command keys as Ctrl unless VS
Code's Linux keys use them for something else. `~/.config/minicode/keys.conf`
(or `$MINICODE_KEYS`) changes them as on the Mac, in the same notation but
with ctrl, alt, and shift only; Edit > Keyboard Shortcuts… writes it the
first time with every command the port has, commented out, and the menus,
the hints and the status bar follow it as it is saved.

The paragraph above that table says which keys go to the program while the
terminal has the keyboard, and `linux/PARITY.md`'s D1 and the choices after
it say why each key is the one it is. Three things the table leaves out: F2
and Delete act only while the file tree has the keyboard, so Delete in the
editor still deletes text; Show Hidden Files, Fold, and Unfold are bound to
Ctrl and a character (`>`, `{`, and `}`), not a key, so they are Ctrl Shift
., Ctrl Shift [, and Ctrl Shift ] where Shift types those characters, as on
a US layout, and not on a layout where it does not; and the tree's right-click menu and the File menu both have New
File, New Folder, Rename, Move to Trash, Open Containing Folder, and Copy
Path.

## The settings file

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
and the color applies live while you drag, as on the Mac; a click on an
opacity value opens a slider. View > Theme picks one of the nine themes
(the settings file's `theme` line), and Adjust Settings… at the end of
every part's right click menu (the file tree, the editor, a Markdown page,
the tabs, the status bar, the source control panel, the terminal, the
browser) opens the settings file at that part's keys; View > Adjust
Settings lists the same parts. GTK's own menus, dialogs and fields go dark
or light with the theme.

Besides the table, Enter and Shift Enter in the find bar find the next and
previous match (Enter in its replacement replaces one), Tab moves between
its two fields and Escape closes it, from the bar or the editor; Enter in
Replace in Folder's replacement replaces all after asking, and Ctrl Shift 1
on a result replaces that match alone; Ctrl Shift C and Ctrl Shift V copy
and paste in the terminal, where Alt with a letter goes to the program as
Meta; and a Ctrl click on a file:line or URL in the terminal's output opens
it.

F2 and Delete work only while the file tree has the keyboard, so Delete in the
editor still deletes text.

In the tree, one click opens a file in the preview tab, which the next
single click replaces, and opens and closes a folder, as on the Mac; a
double click or Ctrl click opens a file in a tab that stays. Up and Down
move the selection without opening anything, Enter opens the selected file
in a tab that stays and moves the keyboard to the editor, Left goes up to the
folder and closes it, and Right opens a folder and steps into it.

In the Source Control panel, Up and Down move through the files, Enter shows
the selected file's diff in the editor's place, Space stages or unstages it,
Tab moves between the changes, the graph and the message box, and Ctrl Enter
in the message box commits. Enter or a click on a commit in the graph shows
it. Find Next and Find Previous are F3 and Shift F3, VS Code's on Linux,
rather than the Mac's Command G and Shift Command G: VS Code's Ctrl G on
Linux is Go to Line, and Ctrl Shift G is the panel.

The command palette is Ctrl Shift P or F1, VS Code's keys: type part of a
command's name and press Enter. The same box opens empty for Ctrl P, which
finds a file of the folder by part of its name, with ":" for Ctrl G, a line
to go to (42, or 42:7 for a column too), and with "@" for Ctrl Shift O, a
symbol of the open file (its classes, functions, or headings; "@:" groups
them by kind). Up and Down choose, Escape closes it, and typing ">", ":"
or "@" first switches between them. In the terminal Ctrl P, Ctrl G and F1
are the program's; Ctrl Shift P and Ctrl Shift O still open the palette.

Show Hidden Files is Ctrl Shift ., the Mac's Shift Command . with Ctrl for
Command. It is bound as Ctrl and the `>` character, so it works on layouts
where Shift . types `>`, such as US and UK English. Ctrl H, the file
managers' key, is Replace's once the port has it, as in VS Code.

The zoom keys act only while a PDF or the LaTeX preview is on screen, and
otherwise pass through to whatever has the keyboard. Fit width is Ctrl+Alt+0,
not the usual Ctrl+0, because Ctrl+0 is Focus the File Tree, as Command 0 is
on the Mac, and one of the ways out of the terminal.

While the terminal has the keyboard, the plain Ctrl shortcuts (Ctrl B, F, N,
O, Q, S, Space, slash, comma, plus and minus) and the keys with no modifier
(F3, F9, F12) belong to the program in it, so readline, vim and emacs get
them: Ctrl W deletes a word, Ctrl H is backspace. The keys that move between
panes and tabs stay the app's: Ctrl `, Ctrl 0, Ctrl 1, Ctrl Tab, Ctrl Shift
Tab, Ctrl Page Down, and Ctrl Page Up (Global in `src/CommandsLinux.cpp`). Ctrl 0 reaches the Source Control panel from the
terminal while it is open. Everything with Shift or Alt in it still works
there, except Ctrl Shift C and Ctrl Shift V, the terminal's copy and paste
(elsewhere Ctrl Shift V is the preview), Ctrl Shift G (Source Control),
which the shell reads as Ctrl G, and Ctrl Shift Space (Play or Pause),
which it reads as Ctrl Space; the editor's own keys (Ctrl Alt L, Ctrl Alt I,
Shift F9) are not bound there at all. Ctrl click on a link in the output is
a mouse action and is not affected. The Mac has no such split, since its
shortcuts use Command and the terminal gets Control. The tree's right-click
menu and the File menu both have New File, New Folder, Rename, Move to
Trash, Open Containing Folder and Copy Path.
