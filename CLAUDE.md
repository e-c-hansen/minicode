# CLAUDE.md — working notes for MiniCode

MiniCode is a small native macOS code editor built from scratch, no Electron,
no third-party dependencies. It links only Apple system frameworks (Cocoa,
WebKit, CoreServices) plus the C++ standard library. This file is the handoff
for future sessions: architecture, workflow, and the hard-won gotchas.

## Build / test / run / release

- `make` — build `MiniCode.app` (ad-hoc signed; that signature is required to
  run on Apple Silicon and to keep granted permissions stable).
- `make test` — build and run the pure-C++ unit tests (`tests/run_tests.cpp`).
  38 checks over the tokenizer and Markdown parser. Exits non-zero on failure.
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

The GUI is Objective-C++ (`.mm`), the normal way to drive AppKit from C++.

- `src/main.mm` — `AppDelegate`, the menu bar, multi-window bookkeeping, and one
  local key monitor (only for Ctrl+`).
- `src/EditorController.{h,mm}` — the window: file tree, editor, data-safety,
  Markdown rendering, find, scope of most features. This is the big file.
- `src/Terminal.{h,mm}` — persistent-shell command runner (NSTask).
- `src/Browser.{h,mm}` — WKWebView panel.
- `src/Search.{h,mm}` — scoped, project-wide text search window.

## Verification reality (important)

This environment cannot exercise the GUI: no screen capture, no reliable
synthetic clicks/keys. So:

- Test the pure-C++ core with `make test`.
- For GUI logic, add a temporary `getenv("MINICODE_*TEST")` block in `main.mm`
  that drives the code path and NSLogs the result, run it headless, then remove
  it. This pattern found the zero-width sidebar, the search data path, etc.
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
- **Cmd+B collapse** fought the split delegate's 160px min; a `sidebarCollapsed`
  flag lets the minimum drop to 0.
- **Custom hotkeys**: prefer real MENU items with key equivalents. They work in
  every pane (like Cmd+C) and for shift-variants that share a letter (Cmd+G vs
  Cmd+Shift+G both work). A local key monitor was flaky; it now handles only
  Ctrl+` (which is awkward as a clean menu item alongside Cmd+T).
- **Window tabbing**: `setAllowsAutomaticWindowTabbing:NO`, otherwise macOS
  merges windows into tabs and Cmd+W closes the whole group.
- **Two app instances**: launching the `.app` twice just activates the existing
  one. The CLI launcher (`Contents/Resources/minicode`) runs the raw binary
  detached, which does start a separate process.
- **Terminal**: one long-lived `zsh -l` fed over a pipe. A sentinel line after
  each command delimits output and carries back `$?` and `$PWD`. Commands run as
  `{ cmd ; } </dev/null` so they can't swallow the control stream. ANSI is
  stripped for display.
- **Markdown**: block elements call `ensureLineStart` so they aren't glued to
  the previous paragraph; headings get `paragraphSpacingBefore`; tables render
  as aligned monospace.
- **Search**: scoped to a folder (default = open folder or selected folder),
  min 2 chars, generation bumped up front + per-file cancellation, ANSI stripped
  from result lines.
- **Highlighting** is a debounced full re-lex, not incremental.

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
