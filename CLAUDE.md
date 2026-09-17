# CLAUDE.md — working notes for MiniCode

MiniCode is a small native macOS code editor built from scratch, no Electron,
no third-party dependencies. It links only Apple system frameworks (Cocoa,
WebKit, CoreServices) plus the C++ standard library. This file is the handoff
for future sessions: architecture, workflow, and the hard-won gotchas.

There is also a GTK4 Linux port under `linux/`, sharing the portable C++ core
verbatim. It has its own handoff notes in `BUILD-LINUX.md` and `linux/README.md`;
this file covers the macOS app except where it says otherwise.

## Build / test / run / release

- `make` — build `MiniCode.app` (ad-hoc signed; that signature is required to
  run on Apple Silicon and to keep granted permissions stable).
- `make test` — build and run the pure-C++ unit tests (`tests/run_tests.cpp`).
  376 checks over the tokenizer, Markdown parser, terminal output stream,
  settings parser, and comment toggling. Exits non-zero on failure.
- `make run [DIR=~/path]` — build and launch.
- `make icon` — regenerate `resources/AppIcon.icns` from `tools/makeicon.m`.
- `make dist-zip` / `make dmg` — package for distribution.
- `scripts/release.sh 1.2.0` — cut a release: builds the zip, publishes it on
  the Homebrew tap repo, and bumps the cask's version + sha256 in one shot.

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
- `src/TerminalStream.{h,cpp}` — pty byte stream -> styled lines (SGR colors,
  in-line cursor movement) + shell-integration events (OSC 133 marks, OSC 7
  cwd). A line model, not a screen: handles sequences and UTF-8 split across
  reads.

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
- `src/Terminal.{h,mm}` — shell panel: zsh on a pty via forkpty, log-style view.
- `src/Browser.{h,mm}` — WKWebView panel.
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
  forwarding method on `AppDelegate`), `hintsText` in EditorController, and the
  README shortcut table.
- Fonts: system font for Markdown/UI, `monospacedSystemFontOfSize:` 12-13 for
  code and terminal.

## Current state (handoff, 2026-09-17)

- Everything is merged to `main` and pushed (`linux-port` points at the same
  commit). macOS has the settings file (Cmd+,) with per-panel opacity, blur,
  title bar and text colors and clickable color swatches, Cmd+/, and the
  terminal on Shift+Cmd+T. Tests: 376 core, 110 Linux port; builds are
  warning-free on both.
- The user has confirmed in the running app: tables, the pty terminal
  (Ctrl+C, sudo, aliases), colors, the terminal bar, Shift+Cmd+E, and both
  divider drags.
- Linux port has caught up: settings file with transparency and swatches,
  Ctrl+/, Ctrl+Shift+T, divider drags, built and checked in Docker (see
  Verification). Blur and live color picking remain macOS-only.
- Next up per the user: UI changes.

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
on the Mac. An `ubuntu:26.04` image with the CI packages plus `xvfb xcompmgr
x11-xserver-utils xdotool imagemagick dbus-x11` builds the port with meson and
runs it for real:

- Mount the repo (`-v ~/MiniCode:/work`) and build into a container-local
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
  color; pagers stay `cat` because there is no screen model. The last line of
  the output view is "live": the stream re-sends it whole on every change and
  the view replaces text from `_liveStart`. Panel-written lines (headers,
  exit statuses) must go through `ensureNewline`, which ends the live line and
  calls `breakLine()` on the stream, or the next update overwrites them. When testing headless, give the shell a scratch
  `HOME` and `ZDOTDIR`: `/etc/zshrc` sets `HISTFILE` from them, and test
  commands otherwise land in the user's real `~/.zsh_history`.
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

- Source repo: `e-c-hansen/minicode` (PRIVATE).
- Homebrew tap: `e-c-hansen/homebrew-tap` (PUBLIC). The compiled zip is hosted
  on that tap's GitHub Releases (keeps source private, binary installable).
- Install: `brew install --cask e-c-hansen/tap/minicode`. The cask clears the
  quarantine flag in a postflight, so the unsigned app opens cleanly. A
  reference copy of the cask is `packaging/minicode.rb`.
- Not notarized (that needs the paid Apple Developer Program). The full sign +
  notarize flow is scripted in `scripts/sign-and-notarize.sh` for when/if paid.

## Conventions

- No third-party dependencies, ever. New capabilities use system frameworks
  (e.g. planned media rendering uses NSImageView + AVKit, not VLC).
- End commit messages with the Co-Authored-By trailer.
- README / user-facing prose: plain human voice, real sentences, commas, no
  emoji, no LLM-ish phrasing. The user cares about this.

## What's next

See `ROADMAP.md`. Biggest items: image/video rendering (native, easy), then an
LSP client (the feature that would make it a daily driver), then real
incremental highlighting.
