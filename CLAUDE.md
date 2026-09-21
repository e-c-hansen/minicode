# CLAUDE.md — working notes for MiniCode

MiniCode is a small native macOS code editor built from scratch, no Electron,
no third-party dependencies. It links only Apple system frameworks (Cocoa,
WebKit, CoreServices, Quartz for PDFKit) plus the C++ standard library and the
system zlib. The LaTeX preview runs tectonic, an external binary the user
installs or the app downloads on request, the way an LSP client would use
language servers. This file is the handoff
for future sessions: architecture, workflow, and the hard-won gotchas.

There is also a GTK4 Linux port under `linux/`, sharing the portable C++ core
verbatim. It has its own handoff notes in `BUILD-LINUX.md` and `linux/README.md`;
this file covers the macOS app except where it says otherwise.

## Build / test / run / release

- `make` — build `MiniCode.app` (ad-hoc signed; that signature is required to
  run on Apple Silicon and to keep granted permissions stable).
- `make test` — build and run the pure-C++ unit tests (`tests/run_tests.cpp`).
  655 checks over the tokenizer, Markdown parser, terminal output stream and
  screen grid, settings parser, comment toggling, and the LaTeX and SyncTeX
  readers.
  Exits non-zero on failure.
- `make run [DIR=~/path]` — build and launch.
- `make icon` — regenerate `resources/AppIcon.icns` from `tools/makeicon.m`.
- `make dist-zip` / `make dmg` — package for distribution.
- `scripts/release.sh 1.2.0` — cut a release from a clean `main`: runs the
  tests, stamps the version into `Info.plist` and `packaging/minicode.rb` and
  commits that, builds the zip and checks the built app reports the version,
  publishes it on the Homebrew tap repo, bumps the cask's version + sha256,
  then tags `v1.2.0` and pushes `main` with the tag. The version on the
  command line is the only place a version is typed; never edit it by hand.

## Layout of the code

The parts that don't need a GUI are plain C++17 and are unit-tested in
isolation. Keep them dependency-free.

- `src/SyntaxHighlighter.{h,cpp}` — hand-rolled lexer, grammar chosen by file
  extension. Emits `{start, length, style}` tokens.
- `src/MarkdownParser.{h,cpp}` — CommonMark subset -> flat list of styled runs.
- `src/Settings.{h,cpp}` — the settings file (`key = value`): parsing with
  per-line errors, defaults, and the resolved color of every surface
  (opacity applied, fallbacks from `window.*`). All color decisions live here
  so they are unit-tested; the GUI only converts `Rgba` to NSColor.
- `src/LatexDoc.{h,cpp}` — LaTeX source -> the byte ranges that hold editable
  text (fields, prose runs, list items, math), plus the two edits the preview
  can make (replace a span, add an `\item`). A structural reader, not a
  typesetter: it reports only what it understands, and an edit replaces exactly
  one byte range, so unknown macros can never be damaged.
- `src/SyncTex.{h,cpp}` — reads the `.synctex` file TeX writes beside the PDF:
  a point on a page -> candidate source lines, best first.
- `src/TerminalStream.{h,cpp}` — pty byte stream -> styled lines (SGR colors,
  in-line cursor movement) + shell-integration events (OSC 133 marks, OSC 7
  cwd). A line model, not a screen: handles sequences and UTF-8 split across
  reads, and leaves alternate-screen text out of the log. Also home to the
  shared pieces: `TermParser` (the DEC/ANSI state machine, with a Handler
  interface), `applySgr`, `termCharWidth`.
- `src/TerminalScreen.{h,cpp}` — the cell grid for full-screen programs
  (xterm subset: CUP/CUU.., ED/EL/ECH, IL/DL/ICH/DCH, DECSTBM + IND/RI/SU/SD,
  alt screen 47/1047/1049, DECSC/DECRC, pending wrap, tabs, wide chars, DEC
  line drawing, DECCKM/keypad/bracketed paste, DSR/DA replies, resize), plus
  key/paste encoding. Tests replay bytes recorded from vim and less.

The GUI is Objective-C++ (`.mm`), the normal way to drive AppKit from C++.

- `src/main.mm` — `AppDelegate`, the menu bar, multi-window bookkeeping, and one
  local key monitor (only for Ctrl+`).
- `src/EditorController.{h,mm}` — the window: file tree, editor, data-safety,
  Markdown rendering, find, scope of most features. This is the big file.
- `src/LineComments.{h,cpp}` — Cmd+/: comment marker per file name, and the
  toggle itself, in UTF-16 offsets so results map straight onto NSRange.
- `src/AppSettings.{h,mm}` — singleton that loads
  `~/.config/minicode/settings.conf` (or `$MINICODE_SETTINGS`), watches it,
  and posts `MCSettingsDidChangeNotification`.
- `src/Terminal.{h,mm}` — shell panel: zsh on a pty via forkpty; a log view
  with a line input, swapped for the grid while a program needs a screen.
- `src/TerminalGridView.{h,mm}` — draws a `TerminalScreen` and encodes keys
  (via `TerminalScreen::encodeKey`/`encodeChar`) straight to the pty.
- `src/Browser.{h,mm}` — WKWebView panel.
- `src/Latex.{h,mm}` — LaTeX preview: runs tectonic, shows the PDF with PDFKit,
  and turns a double-click into a popover editing the source behind that text.
- `src/Search.{h,mm}` — scoped, project-wide text search window.

## UI map (read this before UI work)

Window content view (`EditorController`, built in `init`):

```
container (PanelHost, laid out by hand in layoutContainer)
├── blurView (NSVisualEffectView)                 window.blur, whole window
├── titlebarView + titleLabel                     drawn when customTitlebar
├── NSSplitView (vertical, thin divider)          sidebar | right pane
│   ├── treeScroll → ClickOutline (NSOutlineView)  file tree, bg #252526
│   └── rightArea (PanelHost, laid out by hand in relayoutRightArea)
│       ├── editorScroll → textView (NSTextView)   top slot, bg #1E1E1E
│       ├── browser (BrowserView)                  top slot when open
│       ├── latex (LatexView)                     editor's slot, .tex preview
│       ├── terminal (TerminalView)                docked bottom, bg #181818
│       └── termDivider (DragBar)                  kept topmost
├── status bar (buildStatusBarInContainer)        bottom strip
└── hintsPanel (buildHintsPanelInContainer)       Shift+Cmd+H overlay
```

- Panel backgrounds, panel text, syntax and Markdown colors come from
  `Settings` (defaults are the VS Code Dark+ values in `Settings.cpp`). Each
  view has an `applySettings` method that runs at startup and on
  `MCSettingsDidChangeNotification`; it must recolor what is already on
  screen (editor re-highlights, Markdown re-renders keeping scroll, terminal
  output marked with `MCTerminalDefaultForeground` gets the new color). A new
  setting goes in `Settings.cpp` (parser + default + the commented
  `defaultFileText`, which a unit test checks key by key) and the view's
  `applySettings`.
- Not yet configurable, still inline hex helpers (`Hex`, `THex`, `SHex`):
  accents (#4EA1F7), muted #9CA3AF, divider line #333333, hints overlay,
  terminal panel messages, the Search window, and the ANSI palette in
  `TermColor::rgb()`. `linux/src/Palette.h` mirrors the macOS defaults.
- The window uses `NSWindowStyleMaskFullSizeContentView` always, so content
  runs under the title bar. `layoutContainer` keeps the split view below it
  using `contentLayoutRect` (empty before the window is shown, so
  `titlebarHeight` falls back to frame math, or the split comes out 0px tall).
  With a custom title bar (any `titlebar.*` key, or a non-opaque window) the
  system title is hidden and `titleLabel` mirrors `window.title` via KVO.
- Translucency: a translucent color must be painted once, and as a plain
  layer color. The tree and editor use `PanelScrollView`, which draws no
  scroll/clip background and sizes a layer-backed backdrop view in `tile`.
  Two traps paid for here: a clip view's translucent `backgroundColor` still
  renders opaque, and NSScrollView never resizes extra subviews (a backdrop
  added with autoresizing stays 0x0). Property checks passed both times; only
  a pixel capture caught it (see verification below).
- Settings file in the editor: `applyHighlighting` gives each color value a
  swatch (background = the color, `NSLinkAttributeName` = `minicode-color`).
  `textView:clickedOnLink:` opens NSColorPanel; `colorPicked:` rewrites the
  line with `Settings::setColor` (which uncomments it) and saves after
  150 ms. `linkTextAttributes` is only a pointing-hand cursor so swatches
  keep their colors.
- Layout is manual frames, not Auto Layout (see gotchas for why). Pane state
  flags: `sidebarCollapsed`, `editorHidden`, `terminalVisible`,
  `browserVisible`, plus NSSplitView's own collapse of `rightArea`.
- A new shortcut goes in three places: the menu in `main.mm` (plus a
  forwarding method on `AppDelegate`), `hintsText` in EditorController (one
  `appendHintsRow:` call), and the README shortcut table. The hints panel is an
  attributed string on tab stops; never line it up with spaces, because the key
  glyphs (⌘ ⇧ ⌃ ⌫ ⏎) differ in width even in a monospaced font.
- Fonts: system font for Markdown/UI, `monospacedSystemFontOfSize:` 12-13 for
  code and terminal.

## LaTeX preview (read before touching it)

Opening a `.tex`/`.ltx`/`.latex` file sets `isLatex` and opens in preview, the
same way Markdown does; `LatexView` takes the editor's slot in
`relayoutRightArea` (the text view hides, the split logic is unchanged).

- **tectonic does the typesetting.** Found via `MINICODE_TECTONIC`, then
  `~/Library/Application Support/MiniCode/bin`, then PATH and the Homebrew
  directories (a Finder-launched app has a short PATH). If it is missing the
  panel offers to download the pinned release into Application Support.
- **The buffer is typeset from a hidden sibling** `.<name>.minicode.tex` in the
  document's own folder, so relative `\input`/`\includegraphics` still resolve
  and the user's file is never written. It is deleted when tectonic exits.
  Output (PDF + `.synctex.gz`) goes to `NSTemporaryDirectory()/MiniCode-latex`.
- **Compiles are debounced (0.6s) and generation-counted**; a result from a
  stale run is dropped. The scroll position and zoom are restored by page index
  and point, since the `PDFDestination` belongs to the old document.
- **Unknown commands are looked through, not skipped.** Real documents (and
  custom classes like an Overleaf resume) wrap their text in `\normalfont{}`,
  `\raisebox{}{}`, `\href{}{}` and the author's own macros; text the reader
  cannot see is text the preview cannot edit, and the click then lands on the
  wrong line. So `LatexDoc` skips only a list of known machinery commands
  (`\label`, `\includegraphics`, `\setlength`, ...) and otherwise enters the
  first braced argument that `looksLikeProse` accepts. That heuristic is what
  keeps `{0.5em}`, `{l}`, `{sec:intro}`, `{GDM.png}` and URLs out, while
  letting `{2014 - 2019}` (digits plus spaces) in.
- **A click that matches nothing refuses.** `spanForClick` returns null rather
  than the nearest span whenever there was a real word to match on, because
  opening the wrong text for editing is worse than opening none.
- **Click to source**: PDFKit gives the page point and the word under it,
  SyncTeX gives candidate lines, `LatexDoc::spanForClick` picks the span.
  **TeX reports the line a paragraph *closed* on**, usually one or two past the
  text, so nearby lines are searched and the word decides between them. The
  word match is normalized to letters and digits, because the PDF reads math
  back as `x2` where the source says `x^2`.
- **Edits are byte splices.** The popover holds the span's own LaTeX source
  (not plain text), so no escaping is invented and nothing is re-serialized.
  Edits arrive back at `EditorController` through `onSourceEdited`, mark the
  buffer dirty, and are saved only by Cmd+S. Preview edits have their own undo
  stack inside `LatexView` (source snapshots, capped at 50), reached through
  `undo:`/`redo:` on the responder chain, with `EditorController` forwarding
  when focus never got as far as the preview. The text view keeps its own undo
  for typing; the two stacks are separate.
- **Packages missing from tectonic's bundle.** tectonic's bundle is an
  older TeX Live than Overleaf's. The user's resume needed `twemojis`, which the
  bundle lacks; the fix was to put `twemojis.sty` and `all-twemojis.pdf` in the
  document's own folder (TeX looks there first). tectonic cannot generate a
  `.sty` from a `.dtx`/`.ins` itself: its virtual filesystem throws away files
  docstrip writes. A short script that keeps the `.dtx`'s code lines (not
  starting with `%`, outside other guards) produced a working `.sty`.
- **Spacing differs from Overleaf by design.** Overleaf defaults to pdfLaTeX,
  tectonic is XeTeX; fonts and line breaking differ, and `\hfill`-built lines
  show it most. The user was told to switch Overleaf to XeLaTeX to compare.
- Headless testing worked well here: drive `openEditorForPage:point:`,
  `startAddItem:` and `commitEdit:` directly from a `MINICODE_LATEXTEST` block,
  finding page points with `[PDFDocument findString:]`. Point
  `MINICODE_TECTONIC` and `TECTONIC_CACHE_DIR` at scratch copies so the test
  neither needs the network nor touches the user's cache.

## Current state (handoff, 2026-09-18)

- Everything lives on `main`; there are no other branches any more. The merged
  ones (`linux-port`, `feat/open-file-from-cli`,
  `fix/markdown-preview-large-files`) were deleted on 2026-09-20 once their
  work was in `main`. Released as **1.2.1** to the Homebrew tap that day, the
  first release since 1.1.0 in July. 501 core checks pass; the build is
  warning-free.
- The LaTeX preview is macOS only; `linux/` has not been touched for it. The
  user has used it on an Overleaf resume at
  `~/Documents/Resume-September-2026` (their personal document: do not edit
  it or commit anything from it) and reported the wrong-text clicks that led to
  `9a92379`. Launch: `make run DIR=~/Documents/Resume-September-2026`.
- tectonic 0.17.0 lives at
  `~/Library/Application Support/MiniCode/bin/tectonic` (installed by the
  app's own Download button) with its package cache in
  `~/Library/Caches/Tectonic`.
- Fixed on the way out: opening a file that is not UTF-8 (every PNG) left the
  previous file's mode in place. After a `.tex` the stale PDF stayed on screen;
  after any text file the view stayed editable, and **Cmd+S wrote the "Cannot
  display" message over the binary file**. The binary branch of
  `openFileAtPath:` now resets `isMarkdown`/`isLatex`/`previewMode`/
  `sourceText`/`dirty`, makes the text view read-only and relays out, and
  `saveCurrentFile:` returns early while `showingMessage` is set. A headless
  test (8 checks) failed 5 before the fix and passed 8 after.
- Next up per the user: **rendering PNG files** (see the next section). After
  that, the Linux port of the LaTeX preview, tested in Docker.

## Next task: render images when they are opened (PNG first)

Clicking a `.png` in the tree today shows "Cannot display ... (Binary file or
unsupported encoding.)". The goal is to show the image instead. PNGs *inside*
a LaTeX document already render (tectonic embeds them in the PDF, verified on
the resume's logos); this is about opening an image file directly.

The plan in `ROADMAP.md` (Media rendering) holds: an `NSImageView`, which is
AppKit, so no new dependency. How it fits what exists:

- **Route by extension before reading the file as text.** `openFileAtPath:`
  currently tries `stringWithContentsOfFile:` first and falls into the binary
  branch when that fails. Check the extension (png, jpg, jpeg, gif, tiff, bmp,
  heic, webp; `NSImage imageTypes` lists what the system decodes) before that
  read, so an image never reaches the text path.
- **Reset the previous file's mode**, exactly as the binary branch now does,
  or the old preview and an editable text view survive into the image. Several
  places set these flags by hand now; a single `resetViewMode` helper called at
  the top of `openFileAtPath:` would be the tidy way.
- **Take the editor's slot the way `LatexView` does.** Add the view to
  `rightArea` lazily, and in `relayoutRightArea` give it `topRect` and hide
  `editorScroll` while it shows (see `showLatex` there). Terminal, browser,
  Shift+Cmd+E and the divider drags then keep working unchanged.
- **Nothing to save.** Set `showingMessage` (or an `isImage` flag checked the
  same way in `saveCurrentFile:`) so Cmd+S cannot write text over the image,
  and keep the text view read-only.
- Scale to fit, never up past 100% for small images (`NSImageScaleProportionallyDown`),
  on the editor background from `Settings` (`[cfg background:Surface::Editor]`,
  and re-apply in `applySettings`). A status line with pixel size is cheap and
  useful. Scrolling/zoom for large images can come later.
- `canTogglePreview` stays NO for images; hints and README need a line.
- **Verify** with a temporary `MINICODE_*TEST` block in `main.mm` (see below):
  open a `.tex`, then a `.png`, then a `.txt`, and check which view is visible,
  that the image view's `image.size` matches the file, that Cmd+S leaves the
  PNG's bytes unchanged, and that returning to a text file restores an
  editable editor. Capture the window with `screencapture -l` to confirm the
  image actually draws, then leave the visual judgment to the user.
- **Linux**: the same gap exists in `linux/src/Editor.cpp` (the
  `g_utf8_validate` branch). GTK4's `GtkPicture` is the equivalent there. Do the
  macOS side first; the user tests Linux in Docker afterwards.

## Verification reality (important)

How much of the GUI you can actually check depends on which port you are
working on, and on whether the session is running on a machine with a display.
Check first; do not assume either answer.

**The macOS app, in a session on the user's Mac: more than it looks.** The
app really launches (a window appears on the user's screen, so keep runs short
and never leave strays). Synthetic key and mouse events sent from inside the
app work, and so does capturing the app's own window as pixels. What stays
out of reach is judging the look: a pixel value can be read, but whether it
looks right is the user's call. A session on a machine without a display (CI,
the Linux box) gets none of this, only a compile.


- Test the pure-C++ core with `make test`.
- For GUI logic, add a temporary `getenv("MINICODE_*TEST")` block in `main.mm`
  that drives the code path and NSLogs PASS/FAIL, run the binary headless in
  the background with a poll-and-kill loop (there is no `timeout` on macOS),
  then remove the block. Techniques that proved reliable:
  - Read state via KVC (`[controller valueForKey:@"rightArea"]`).
  - Key shortcuts: build an `NSEvent keyEventWithType:` and send it through
    `[window performKeyEquivalent:]` then `[NSApp.mainMenu performKeyEquivalent:]`.
  - Mouse drags on views with their own tracking loop (NSSplitView): post
    LeftMouseDragged/LeftMouseUp with `[NSApp postEvent:atStart:NO]`, then call
    `mouseDown:`. For simple views (DragBar) call mouseDown/Dragged/Up directly.
  - Use `hitTest:` at pixel offsets to measure real grab areas.
  - Terminal tests must set `HOME` and `ZDOTDIR` to scratch dirs, or test
    commands land in the user's real `~/.zsh_history`.
- Pixels: `/usr/sbin/screencapture -x -o -l<windowNumber> out.png` run from
  inside the app captures the window with its alpha channel, and
  `NSBitmapImageRep colorAtX:y:` reads it (y from the top, scale for Retina).
  This is the only way to verify transparency; view properties can look right
  while nothing is drawn. `CGWindowListCreateImage` is gone in the macOS 15 SDK.
- Undo in tests: everything run from one callback lands in one undo group
  (`groupingLevel` stays 1). Wrap each simulated key press in
  begin/endUndoGrouping with `groupsByEvent` off. Programmatic edits call
  `breakUndoCoalescing` so each Cmd+/ is its own step for real users.
- Cleaning up temp test blocks: remove them by exact text, not by searching
  for a marker comment that also appears on the `getenv` line.
- When a check fails, print the actual output before changing code: most
  failures in this session were wrong checks, not wrong code.
- Otherwise confirm the app builds clean and launches without crashing, and
  tell the user which behavior needs their eyes. Do not claim GUI behavior is
  verified when it was only compiled.

**The GTK port from the Mac: a Docker container.** Docker Desktop is installed
on the Mac (start it with `open -a Docker`). `linux/dev/gtk-dev.sh` builds the
image (`linux/dev/Dockerfile`: ubuntu:26.04, the CI packages, Xvfb, xcompmgr,
xdotool, ImageMagick), starts a container with the repo mounted, and builds and
tests the port; `/tools/launch.sh` inside it runs the app. `linux/dev/README.md`
has the workflow. The details behind it:

- Mount the repo (`-v ~/Code/public/minicode:/work`) and build into a container-local
  directory (`meson setup /build`), not `linux/build`.
- `Xvfb :99 -screen 0 1280x800x24` plus `xcompmgr` gives a composited display,
  so translucency works. Launch with `GDK_BACKEND=x11 GTK_A11Y=none` under
  `dbus-run-session`, a scratch `HOME`/`XDG_CONFIG_HOME`, and
  `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` (WebKit's bubblewrap sandbox
  cannot start in a container and aborts the whole app). `GTK_CSD=1` draws
  GTK's own title bar, since there is no window manager.
- Drive it with `xdotool key ctrl+shift+t`, `xdotool mousemove X Y click 1`,
  and drags as `mousedown`, a few `mousemove`s, `mouseup`. Capture with
  `import -window root` and read pixels with
  `convert img -format '%[pixel:p{X,Y}]' info:`. The compositor's backdrop is
  gray 128, so a see-through panel's alpha can be computed from the blend.
- `docker cp` screenshots out and look at them. Stray dotted glyph debris in
  root captures is xcompmgr damage, not the app: it vanishes after a resize
  and never appears in `import -window <id>`.
- In helper scripts use `pkill -x minicode`, never `pkill -f` with a path: it
  matches the `docker exec bash -c` command line and kills the script.
- Disk on the Mac is tight (it had 730 MB free); the image is about 1.8 GB.

**The GTK port on the user's Ubuntu machine: a live session.** As of August
2026 the primary Linux box is Ubuntu 26.04 on a ThinkPad T480, GNOME on
Wayland, and a session there has `$WAYLAND_DISPLAY` set and can really launch
the app. Confirm with `echo $WAYLAND_DISPLAY` before relying on this. When it
holds, these give real runtime evidence rather than compile-only evidence:

- `meson test -C build` for the shared core.
- Launch detached (`setsid gtk-launch org.minicode.Editor &`, or run the binary
  directly) and check with `pgrep -a minicode` that it is still alive a few
  seconds later. This is not free: a window really does appear on the user's
  screen, so say what you are doing and do not leave strays running.
- `busctl --user list | grep minicode` shows the GApplication registered its id
  on the session bus.
- `systemctl --user list-units --type=scope | grep minicode` is the useful one.
  A launch through the desktop entry lands in
  `app-gnome-org.minicode.Editor-*.scope`, and that scope name is precisely how
  GNOME Shell ties the window back to its launcher, so seeing it proves the
  dock association works. A nested `vte-spawn-*.scope` proves the VTE terminal
  panel actually came up.
- `journalctl --user --since "-15 min" | grep -i minicode` catches a process
  that started and then died.
- Do not bother with `org.gnome.Shell.Introspect.GetWindows`; it answers
  `AccessDenied` for callers that are not whitelisted.

## Gotchas already paid for (don't rediscover these)

- **Single-click open in the file tree**: `NSOutlineView` selection
  notifications / target-action did not fire reliably. `ClickOutline` overrides
  `mouseDown:` to compute the row from the click point and open directly.
- **Sidebar collapsed to zero width** on launch: the split divider must be
  positioned AFTER the window is on screen and laid out. The editor / terminal /
  browser are laid out by hand in a `PanelHost` view, not nested split views —
  that was far more predictable.
- **Terminal resize bar**: the `DragBar` is a 12px grab area drawing a 1px
  line, and `relayoutRightArea` re-raises it to the top of `rightArea`.
  Otherwise a panel added later (the browser) covers half of it. Its cursor
  comes from a tracking area, because cursor rects lose to the overlapping
  text views.
- **Cmd+B collapse** fought the split delegate's 160px min; a `sidebarCollapsed`
  flag lets the minimum drop to 0.
- **Hiding panes**: two separate mechanisms. (1) Dragging the sidebar divider
  far right collapses the whole right pane via NSSplitView
  (`canCollapseSubview:` YES for `rightArea`; `effectiveRect:` widens the 1px
  divider's grab area to 5px each side; min pane width 150 so the drag doesn't
  visibly stall). (2) Shift+Cmd+E, or dragging the terminal bar to the top,
  sets `editorHidden`: only the file editor goes, and `relayoutRightArea`
  gives the terminal the full height (bar pinned at the top edge so it can be
  dragged back down). If that leaves the pane empty, `collapseRightAreaIfEmpty`
  collapses it. Opening a file, Cmd+1 and the preview toggle call
  `revealEditor`; showing the terminal/browser calls `showRightArea`. Headless,
  a real split-view drag can be tested by posting LeftMouseDragged/Up events
  with `postEvent:` and then calling the split view's `mouseDown:` (its
  tracking loop dequeues them).
- **Custom hotkeys**: prefer real MENU items with key equivalents. They work in
  every pane (like Cmd+C) and for shift-variants that share a letter (Cmd+G vs
  Cmd+Shift+G both work). A local key monitor was flaky; it now handles only
  Ctrl+` (which is awkward as a clean menu item alongside Shift+Cmd+T).
- **Window tabbing**: `setAllowsAutomaticWindowTabbing:NO`, otherwise macOS
  merges windows into tabs and Cmd+W closes the whole group.
- **Two app instances**: launching the `.app` twice just activates the existing
  one. The CLI launcher (`Contents/Resources/minicode`) runs the raw binary
  detached, which does start a separate process.
- **Terminal**: `zsh -l -i +Z` (interactive, line editor off) on a pty from
  `forkpty`. `ZDOTDIR` points at a generated `.zshenv` (in
  `NSTemporaryDirectory()/MiniCode-zsh`) that hands `ZDOTDIR` straight back to
  the user, loads their files, and on the first `precmd` installs hooks that
  emit OSC 133 A/C/D + OSC 7 and blank `PS1`. Installing at first precmd is
  what keeps the hook last, after .zshrc. Echo is off while zsh reads a
  command (precmd `stty -echo`) and on while it runs (preexec), so the pty's
  ECHO flag, read from the master with `tcgetattr`, tells us when a program
  wants hidden input. Everything the child needs is built before `fork`: only
  async-signal-safe calls between fork and exec. `PROMPT_SP` prints before
  precmd, so it is unset in .zshenv. `TERM=xterm-256color` so tools emit
  color. Pagers are no longer forced to `cat` (the grid runs them). The last line of
  the output view is "live": the stream re-sends it whole on every change and
  the view replaces text from `_liveStart`. Panel-written lines (headers,
  exit statuses) must go through `ensureNewline`, which ends the live line and
  calls `breakLine()` on the stream, or the next update overwrites them. When testing headless, give the shell a scratch
  `HOME` and `ZDOTDIR`: `/etc/zshrc` sets `HISTFILE` from them, and test
  commands otherwise land in the user's real `~/.zsh_history`.
- **Terminal grid mode** (`feat/terminal-screen`): every pty chunk goes to
  BOTH `TerminalStream` (log) and `TerminalScreen` (grid), so either is
  current when shown and no mid-chunk handoff is needed. `updateMode` shows
  the grid while `_screen.altScreen()`, or while `_stickyGrid`: a 0.1 s timer
  (running only between OSC 133 C and A) reads termios from the master, and a
  command that stays out of ICANON for two polls without the alt screen
  (git's `less -FRX`, python REPL, ssh) keeps the grid until its command
  ends. The two-poll debounce is what stops `less -F` on short output from
  flashing the grid. The pty size comes from the panel bounds and the grid's
  cell metrics in both modes (the log's `lineFragmentPadding` is 0 so its
  wrap column matches), so switching modes never sends SIGWINCH. Tiny or
  collapsed bounds keep the old size, or the screen would be cut to 20x5.
  Screen replies (`\e[6n` -> CPR) are written back after each chunk; vim
  sends two DSRs at startup. `TerminalScreen` must stay copyable: an earlier
  version kept a `Grid*` into itself and every copy drew the original's grid.
- **Synthetic key events in tests**: Shift+Cmd+H only matches the menu with
  `charactersIgnoringModifiers:@"h"` (lowercase). An arrow key event needs
  `characters` = the function-key character (U+F701 etc.); an empty string
  makes `-[NSMenu performKeyEquivalent:]` throw and abort the app. Drive
  keys with `[NSApp sendEvent:]` from a background thread via dispatch_sync
  to main; that exercises the real menu-then-first-responder routing.
- **Markdown**: block elements call `ensureLineStart` so they aren't glued to
  the previous paragraph; headings get `paragraphSpacingBefore`; tables render
  as aligned monospace. Table cells are inline-parsed and padded by *display*
  width (code points, CJK/emoji = 2), never UTF-8 byte length, or any
  non-ASCII cell knocks the columns out of line.
- **Search**: scoped to a folder (default = open folder or selected folder),
  min 2 chars, generation bumped up front + per-file cancellation, ANSI stripped
  from result lines.
- **Highlighting** is a debounced full re-lex, not incremental.
- **Crash on window close**: `NSWindow` defaults to `releasedWhenClosed = YES`,
  a legacy manual release. With ARC also owning the window through a `strong`
  property, closing double-frees it ("MiniCode quit unexpectedly"). Set
  `window.releasedWhenClosed = NO` so ARC is the sole owner.

## Distribution

- Source repo: `e-c-hansen/minicode` (PUBLIC). The release zip is published on
  this repo's own Releases, attached to the `vX.Y.Z` tag it was built from.
- Homebrew tap: `e-c-hansen/homebrew-tap` (PUBLIC), which holds nothing but
  `Casks/minicode.rb`. That file is *written* by `scripts/release.sh` from
  `packaging/minicode.rb`, which is the only copy anyone edits; the tap exists
  because `brew install --cask e-c-hansen/tap/minicode` needs a repo called
  `homebrew-<something>`, and nothing more.
- The cask clears the quarantine flag in `postflight_steps` (Homebrew 7
  deprecated the `postflight` block; `{{appdir}}` is the template token in step
  arguments), so the unsigned app opens cleanly. After editing the cask, check
  it with `brew style` from a tap checkout
  (`/opt/homebrew/Library/Taps/e-c-hansen/homebrew-tap`).
- Not notarized (that needs the paid Apple Developer Program). The full sign +
  notarize flow is scripted in `scripts/sign-and-notarize.sh` for when/if paid.
- Homebrew is the only way MiniCode updates: the app has no updater, so the
  cask must NOT declare `auto_updates`, or a plain `brew upgrade` would skip it.

## Conventions

- No third-party dependencies, ever. New capabilities use system frameworks
  (e.g. planned media rendering uses NSImageView + AVKit, not VLC).
- End commit messages with the Co-Authored-By trailer.
- README / user-facing prose: plain human voice, real sentences, commas, no
  emoji, no LLM-ish phrasing. The user cares about this.

## What's next

See `ROADMAP.md`. Immediate: image rendering (the section above), then the
LaTeX preview on Linux. After that: video/audio, an LSP client (the feature
that would make it a daily driver), and real incremental highlighting.
