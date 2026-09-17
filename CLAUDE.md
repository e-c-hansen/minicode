# CLAUDE.md — working notes for MiniCode

MiniCode is a small native macOS code editor built from scratch, no Electron,
no third-party dependencies. It links only Apple system frameworks (Cocoa,
WebKit, CoreServices) plus the C++ standard library. This file is the handoff
for future sessions: architecture, workflow, and the hard-won gotchas.

## Build / test / run / release

- `make` — build `MiniCode.app` (ad-hoc signed; that signature is required to
  run on Apple Silicon and to keep granted permissions stable).
- `make test` — build and run the pure-C++ unit tests (`tests/run_tests.cpp`).
  142 checks over the tokenizer, Markdown parser, and terminal output stream. Exits non-zero on failure.
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
- `src/TerminalStream.{h,cpp}` — pty byte stream -> styled lines (SGR colors,
  in-line cursor movement) + shell-integration events (OSC 133 marks, OSC 7
  cwd). A line model, not a screen: handles sequences and UTF-8 split across
  reads.

The GUI is Objective-C++ (`.mm`), the normal way to drive AppKit from C++.

- `src/main.mm` — `AppDelegate`, the menu bar, multi-window bookkeeping, and one
  local key monitor (only for Ctrl+`).
- `src/EditorController.{h,mm}` — the window: file tree, editor, data-safety,
  Markdown rendering, find, scope of most features. This is the big file.
- `src/Terminal.{h,mm}` — shell panel: zsh on a pty via forkpty, log-style view.
- `src/Browser.{h,mm}` — WKWebView panel.
- `src/Search.{h,mm}` — scoped, project-wide text search window.

## UI map (read this before UI work)

Window content view (`EditorController`, built in `init`):

```
container (NSView)
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

- Colors are VS Code Dark+ values, but there is **no shared palette**: each file
  has its own hex helper (`Hex` in EditorController, `THex` Terminal, `BHex`
  Browser, `SHex` Search) and literals are inline. Main ones: text #D4D4D4,
  editor bg #1E1E1E, sidebar #252526, terminal bg #181818, input #232323,
  accent/link #4EA1F7, muted #9CA3AF, divider line #333333. The terminal's ANSI
  palette lives in `TermColor::rgb()` (C++). A theme pass should start by
  centralizing these; `linux/src/Palette.h` mirrors the macOS values.
- Layout is manual frames, not Auto Layout (see gotchas for why). Pane state
  flags: `sidebarCollapsed`, `editorHidden`, `terminalVisible`,
  `browserVisible`, plus NSSplitView's own collapse of `rightArea`.
- A new shortcut goes in three places: the menu in `main.mm` (plus a
  forwarding method on `AppDelegate`), `hintsText` in EditorController, and the
  README shortcut table.
- Fonts: system font for Markdown/UI, `monospacedSystemFontOfSize:` 12-13 for
  code and terminal.

## Current state (handoff, 2026-09-17)

- Branch `linux-port`, 5 commits ahead of `main`, **not pushed**: the GTK port
  draft, Markdown tables, pty terminal, resize-bar fix, terminal colors, and
  the editor-hide/pane dividers. All tests pass (142), build is warning-free.
- The user has confirmed in the running app: tables, the pty terminal
  (Ctrl+C, sudo, aliases), colors, the terminal bar, Shift+Cmd+E, and both
  divider drags.
- Linux port has not caught up: it still has its own VTE terminal (fine), but
  none of the pane-hiding shortcuts. Only the table fix touched `linux/`.
- Next up per the user: UI changes.

## Verification reality (important)

This environment cannot take screenshots or send real input to the app. So:

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
- When a check fails, print the actual output before changing code: most
  failures in this session were wrong checks, not wrong code.
- Otherwise confirm the app builds clean and launches without crashing, and
  tell the user which behavior needs their eyes. Do not claim GUI behavior is
  verified when it was only compiled.

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
  Ctrl+` (which is awkward as a clean menu item alongside Cmd+T).
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
