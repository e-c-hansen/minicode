# CLAUDE.md — working notes for minicode

minicode is a small native macOS code editor built from scratch, no Electron,
no third-party dependencies. It links only Apple system frameworks (Cocoa,
WebKit, CoreServices, Quartz for PDFKit and Quick Look, AVKit, AVFoundation and
CoreMedia) plus the C++ standard library, the system zlib and the system
libsqlite3. The LaTeX preview and the Markdown math run tectonic, which the
user installs or the app downloads on request; the language servers, formatters
and Claude Code it talks to are the user's own.

There is a GTK4 Linux port under `linux/` and an Android port under `android/`,
both sharing the portable C++ core verbatim. Their handoff notes are
`BUILD-LINUX.md`, `linux/HANDOFF.md`, `linux/README.md` and
`android/README.md`; this file covers the macOS app except where it says
otherwise. Almost everything added since 1.5.0 is Mac-only, with its logic in
the core so the ports can follow (`ROADMAP.md`).

This file is architecture and the traps already paid for. What a commit
checked, and its timings, is in its message (`git log`); each source file's
opening comment says what it does in more detail than the lines below.

## Build, test, run, release

- `make` builds `MiniCode.app` (with `build/minicode-cli` copied into
  `Contents/MacOS`), signed with "MiniCode Local Signing" when the login
  keychain has it, else ad hoc (Apple Silicon needs a signature). **Why the
  certificate** (2026-11-05): macOS keys what an app was allowed (a protected
  folder, the microphone a terminal program asks for) to its designated
  requirement, and an ad-hoc one is `cdhash H"..."`, the exact binary, so every
  build, every restart into one and every release asked again. With the
  certificate it is `identifier "com.minicode.editor" and certificate leaf =
  H"..."`, the same from build to build. It is self-signed and untrusted, which
  neither codesign nor TCC minds. `scripts/make-signing-identity.sh` makes it
  once (openssl, then `security import -T /usr/bin/codesign`, so codesign uses
  it with no prompt); the auto-mode classifier refused to run it for the user,
  who runs it themselves. Releases built on that Mac are signed with it too
  (1.5.6 was the first), so installs keep their answers across releases.
  Info.plist carries the usage descriptions (microphone, camera, Apple events,
  Desktop, Documents, Downloads, removable and network volumes): programs in
  the terminal count as minicode to macOS, and without
  `NSMicrophoneUsageDescription` Claude Code's voice input was refused without
  a prompt.
- Built for macOS 11 and up: `-mmacosx-version-min` comes from Info.plist's
  LSMinimumSystemVersion, a newer API without an `@available` check is a
  compile error, and `scripts/check-minos.sh` (CI, release.sh) checks the
  binaries' minos and the cask's `depends_on`.
- Each source file is its own object in `build/obj` with a `-MMD -MP`
  dependency file; `build/obj/flags.txt` holds the compile command, so new
  flags rebuild everything. The Mac's GNU make 3.81 cannot tell a makefile
  whether `-j` was given, so the top-level make adds `-j$(JOBS)` itself (one
  job per core, never with `clean` among the goals); `make JOBS=1` builds
  serially.
- `make test` builds and runs the core's tests (`tests/run_tests.cpp` with the
  `tests/*.inc` groups and the `*Tests.cpp` files) in `build/test-obj`, without
  -O2, and exits non-zero on failure. Groups are named `area:case`. Counts grow
  with every feature; do not quote them (8,490 at the end of 2026-11-05). Run
  it from the repo root: corpus and README checks read files there and skip
  themselves when missing. clang++ on the Mac, make's default (g++) elsewhere,
  so CI runs it on Linux; `make test CXX=g++-16` reproduces CI's libstdc++ job
  on the Mac. Checks that read the docs: `commands:readme` (every default key
  is in the README's macOS column), `install:readme` (every install command is
  in the README in backticks), `linux:build-lists` (linux/Makefile and
  meson.build name the same core files, and Android's CMakeLists names every
  core file its own files include), and the `settings:*` count of keys in
  the default settings file.
- `make guitest [SUITES="control agents"]`: the offscreen GUI checks in
  `tests/gui` (see "Verification").
- `make run [DIR=~/path]`, `make icon` (from `tools/makeicon.m`), `make demos
  [SCENES="tour latex"] [POSTERS=1]`, `make membench`, `make install` (runs `minicode
  install-skill` from the checkout: `skills/minicode/SKILL.md` to
  `~/.claude/skills/minicode`; the bundle carries the skill in
  `Contents/Resources/skills`, so a Homebrew copy can install it too), `make dist-zip`,
  `make dmg`.
- `scripts/release.sh 1.2.0` cuts a release from a clean `main`: runs the
  core tests, and `make guitest` unless CI passed on that very commit, builds
  the signed APK before anything is stamped, stamps the version into `Info.plist` and `packaging/minicode.rb` and
  commits that, builds the zip as a release (`make dist-zip RELEASE=1`: no
  `Demo.mm` or `Bench.mm`, main.mm gets their stubs, checked with `nm`) and
  checks the built app reports the version,
  publishes it on the Homebrew tap repo, bumps the cask's version and sha256,
  then tags `v1.2.0` and pushes `main` with the tag, and runs the PPA upload
  last (skipped when no key is configured). The version on the command line is
  the only place a version is typed; never edit it by hand. Then `brew update
  && brew upgrade --cask e-c-hansen/tap/minicode` so the user's own copy has
  it; they run minicode from Homebrew and expect a finished, user-visible Mac
  change to reach it.
- `scripts/release.sh --linux 1.2.1`: a Linux-only release, which runs on the
  ThinkPad too: tests, stamps `Info.plist` (the Linux version comes from it),
  pushes `main` with the tag, uploads to the PPA. No GitHub Release, zip, APK
  or cask, so Homebrew and Obtainium stay on the last full release. Ubuntu
  users get Linux changes only through a release of one kind or the other.
- `scripts/ppa.sh VERSION [N]` builds the Ubuntu source package (`debian/`,
  versions like `1.4.12-1~ppa1~resolute1`) in an ubuntu:26.04 container, signs
  it on the Mac with the gpg key named in `~/.config/minicode/ppa-key-id`, and
  uploads it to `ppa:echansen/minicode` (`--dry-run` and `--simulate` send
  nothing). Setup is in `packaging/PPA.md`. Account `echansen`, key
  340F7652...1F40 in the Mac's gpg with its passphrase in the Keychain;
  Launchpad builds amd64 about 35 minutes after an upload. The ThinkPad has its
  own key (AEE321CE...DB7F9335) and no Docker, so there ppa.sh runs the
  packaging tools directly (`packaging/PPA.md`, step 9). On the Mac it needs
  Docker, which must not be started unattended (see "Verification").

## Layout of the code

### The core (`src/*.cpp`, pure C++17, tested by `make test`)

No dependencies and no I/O unless noted, so the ports can use it. New logic
goes here first, with tests; the GUI only displays it.

- `SyntaxHighlighter` (about 200 grammars, `IncrementalHighlighter`),
  `MarkdownParser` (CommonMark plus GitHub, math, `lines()`), `MarkdownEdit`
  (the block behind a preview click), `MarkdownTasks` (task lists), `MathTex`
  (the Markdown math batch), `MermaidDiagram` (```mermaid blocks for mmdc),
  `LatexDoc` (editable ranges of LaTeX source),
  `SyncTex` (page point to source lines), `Json`, `IdeLink` (the Claude Code
  IDE link: WebSocket, MCP, lock file).
- `Settings` (the settings file, themes, every resolved color), `Commands` (the
  command registry and keys.conf), `EditorTabs` (tab rules), `Backups` (backups
  of unsaved tabs, with a POSIX `Fs`), `FileDetails` (tree sorts, ages, folder
  totals), `RunFile` (Run File's quoting).
- Editing: `LineDiff` (Myers, UTF-16, `mapOffset`), `LineComments`,
  `AutoIndent` (with `ScanCache`), `KeepSorted`, `MultiCaret` (carets and line
  commands), `LineIndex` (line starts, a gap buffer), `BracketMatch`,
  `Folding`, `TextFind` (find and replace, UTF-16 and UTF-8), `Formatter`
  (Format Document's plan), `InstallHints`.
- Language servers: `LspClient` (protocol, results, WorkspaceEdits, diagnostics
  with versions, completion edits, symbols, folding ranges, the second server's
  routing and `DiagnosticStore`, the Problems rows), `PythonEnvs` (interpreters
  found without running any).
- Terminal: `TerminalStream` (bytes to lines and shell events; `TermParser`,
  `applySgr`, `termCharWidth`), `TerminalScreen` (the grid, scrollback, reflow,
  find, mouse encoding), `TermLinks`.
- `FolderSearch` (Find and Replace in Folder, atomic writes, TODOs),
  `GitStatus` (status, diffs, git time limits), `GitGraph` (log, refs, lanes),
  `GitModel` (what every port's Source Control panel shows and runs).
- `CommandPalette` (matching, modes, Quick Open, symbol rows), `SymbolOutline`
  (symbols without a server), `NavHistory` (Back and Forward), `InlineAgent`,
  `ChangeRequest`, `ControlProtocol` (the control channel and the command
  line's parsing), `SideBySide` (diff rows), `PageCheck` (agents checking
  pages: addresses, the origin limit, the console record).
- Agent work: `AgentTranscript`, `AgentIndex`, `AgentEvidence`, `AgentView`;
  `ProcessTable` (processes and ports per folder: tracking, groups, Stop's plan).
- Files: `FileSniff` (how a file is shown), `Csv`, `JsonOutline`, `Notebook`.

### The Mac app (Objective-C++)

- `main.mm`: `AppDelegate`, the menus, windows, the open handler, and the one
  local key monitor (which asks `MCKeymapHandleKeyDown` first).
- `EditorController.{h,mm}` and its categories (below); `EditorTab` (`MCTab`),
  `EditorPanes` (`PanelHost`, `DragBar`, `PanelScrollView`), `FileTree`
  (`FileItem`, `ClickOutline`, `gShowHidden`), `EditorPage.h`.
- The editor: `SourceStorage` (`MCSourceStorage`), `CodeTextView.mm`, `Gutter`,
  `FoldView`, `EditorFont`, `EditorFind`, `MultiCaretView`, `EditingCommands`
  (AutoIndent and KeepSorted on an NSTextView), `MarkdownCode` (code colors in
  previews).
- Language servers: `Lsp.mm` (`LspSession`, one per window), `LspServer`
  (processes), `LspPopups` (completion, hover, signature), `LspShared.h` (only
  the Lsp files import it), `LspPanel` (status item, popover, interpreter
  sheet), `LspPicker` (references and fixes list, rename box), `ProblemsPanel`,
  `InstallPanel`.
- `AppSettings`, `Appearance` (Adjust Settings, Theme menu, opacity popover),
  `Keymap`.
- Terminal: `Terminal` (`TerminalView`, `MCPty`, the shell integration),
  `TerminalGridView`, `TerminalPanel`, `TerminalFindBar`, `RunCommand`.
- Viewers: `Latex`, `MarkdownMath`, `MarkdownMermaid`, `MarkdownImage`, `MarkdownEditPanel`,
  `FileViewers`, `QuickLook`, `CsvTable`, `JsonOutlineView`, `SqliteView`,
  `CompareView`.
- `Browser` (with `MCKeyboardWindow`), `TabStrip`, `Search` (Find in Folder and
  TODOs window), `GitPanel`, `TreeDetails`, `FormatRunner`, `BackupStore`,
  `PalettePanel`, `Navigation`.
- Agents: `AgentPanel` (Cmd+I), `ChangePanel` (Request a Change),
  `ControlServer`, `ControlCommands`, `IdeLinkServer` (Claude Code's /ide),
  `AgentNotes`, `AgentModel`,
  `AgentsPanel`, `AgentPages`, `AgentWindow`, `ProcessMonitor` (libproc,
  `minicode procs`), `ProcessesPanel`.
- `Demo` (`make demos`, inert unless `MINICODE_DEMO`), `Bench`
  (`MINICODE_BENCH`).
- Elsewhere: `tools/minicode-cli.cpp`, `scripts/minicode-launcher`,
  `tools/makegif.m`, `tools/procmem.c`, `skills/minicode/SKILL.md`,
  `tests/fixtures/claude`, `tests/gui`, `tests/latex`, `tests/markdown`,
  `tests/keepsorted`.

### EditorController and its categories

`EditorController.mm` keeps init, dealloc, the window delegate,
`validateMenuItem:`, undo and redo, and the forwarding accessors of the active
tab. The rest is in `EditorController+<Concern>.mm`: Layout, StatusBar, Panels
(terminal dock, browser, keyboard between panes), Search, Tree, TreeDetails,
Tabs (tabs, closed tabs, saving), Persistence (sessions, backups, auto-save,
files changed outside), Rendering (what the editor's slot shows), Highlighting,
Settings, Format, EditorView (line numbers, brackets, zoom, comments, tasks,
editing hooks), Navigation, LanguageServers, Agents (inline agent, Request a
Change, control channel, agent work), Processes (the Processes panel's
hooks, its state in associated objects), Markdown and MarkdownRender. Rules, so it
stays warning-free:

- `EditorController+Private.h` holds the class extension (ivars in its ivar
  block, private properties), each category's private methods in an `@interface
  EditorController (Concern)`, and helpers several files share as `static
  inline` (so a file that does not call one is not warned). Only
  EditorController files import it. Shared non-window state is a global defined
  in one file (`gShowHidden` in FileTree.mm), never a static in a header.
- **Ivars (2026-11-06): the shared block is closed to one category's state.**
  It holds only variables two or more category files use; one used by a
  single category goes in that category's own class extension,
  `src/EditorController+<Concern>Ivars.h` (`@interface EditorController ()
  { ... }`, importing `+Private.h`), imported by that category's `.mm` and by
  `EditorController.mm`, whose `@implementation` must see a class extension
  for its ivars to exist (miss it and the link fails on `OBJC_IVAR_$_...`).
  Make the header when the category has none yet. Better still, a feature in
  files of its own keeps its state in its own object, reached through one
  property or ivar (`MCEditorFind`, `MCFolds`, `MCAgentSession`). The
  `structure:ivars` core test fails on a shared variable fewer than two
  categories use, an ivar header `EditorController.mm` does not import, or a
  category using another's ivars.
- Methods the rest of the app calls go in `EditorController.h` in an
  `@interface EditorController (Concern)` named for the implementing category:
  a method the class declares but a category implements draws warnings. For the
  same reason host protocols (MCChangeHost, MCAgentHost, MCAgentNotesHost,
  MCAgentWorkHost, MCNavigatorHost) are adopted by their categories, not the
  class extension.
- A KVO context and its observer share a file (the player's, in +Rendering).
- No new weight: a feature goes in its own files and classes, and
  EditorController gains only hooks.

## UI map (read this before UI work)

```
container (PanelHost, laid out by hand in layoutContainer)
├── blurView (NSVisualEffectView)                 window.blur
├── titlebarView + titleLabel                     with a custom title bar
├── NSSplitView (vertical)                        sidebar | right pane
│   ├── treeScroll → ClickOutline                 file tree, MCTreeCell rows,
│   │   a Worktrees node first; or MCGitPanel (Ctrl+Shift+G), the Agents
│   │   panel (Shift+Cmd+A) or MCProcessesPanel (View > Toggle Processes)
│   │   swapped in with replaceSubview:
│   └── rightArea (PanelHost, relayoutRightArea)
│       ├── changeBar (MCChangeBar)                Request a Change, top, over all
│       ├── tabStrip (MCTabStrip)                  top of the editor's side
│       ├── installBar (MCInstallBar)              under the strip
│       ├── emptyLabel                             no file open
│       ├── editorScroll → CodeTextView            over MCSourceStorage, with
│       │   MCLineGutter (vertical ruler), the MCEditorFind bar at top
│       │   right, agent notes and the MCAgentBar over the text
│       ├── or in the editor's slot: LatexView, image, PDFView, AVPlayerView,
│       │   diffScroll, CompareView, Quick Look, CSV table, SqliteView,
│       │   an EditorPage (Evidence, a worktree's changes)
│       ├── JSON outline                           right of the text
│       ├── browser (BrowserView)                  right of the editor
│       ├── problemsPanel (MCProblemsPanel)        above the terminal
│       ├── terminalPanel (MCTerminalPanel)        bottom, full width: strip,
│       │   +, ◫, TerminalViews side by side, MCTerminalFindBar
│       ├── browserDivider (DragBar, vertical)
│       └── termDivider (DragBar)                  kept topmost
├── status bar                                    Saved..., settings and keys
│                                                 errors, problem counts, server
│                                                 item, ↻ restart, hints key
├── hintsPanel                                    Shift+Cmd+H
└── MCPaletteOverlay                              while the palette is open
```

- Colors come from `Settings` (defaults are VS Code Dark+'s). Each view has an
  `applySettings`, run at startup and on `MCSettingsDidChangeNotification`,
  that recolors what is on screen (editor re-highlights, Markdown re-renders
  keeping scroll, terminal text marked `MCTerminalDefaultForeground` follows).
  A new setting goes in `Settings.cpp` (a row of `keyTable()`, `MC_KEY(key,
  Word | CommandLine | Path, isColor, body)`, which says how far its value
  runs on the line and applies it; only a family matched by prefix, such as
  `lsp.*` or a surface's `.background`, is code in `apply()`), its default,
  the commented `defaultFileText` (`settings:key-table` fails on a row it
  lacks) and the view's `applySettings`. Per-language blocks are not done. Still inline hex (`Hex`, `THex`, `SHex`): accents #4EA1F7,
  muted #9CA3AF, dividers #333333, the hints overlay, terminal panel messages,
  the Search window, the git panel, the developer amber #E8A33D, the ANSI
  palette in `TermColor::rgb()`; `linux/src/Palette.h` mirrors the defaults.
- The window always uses `NSWindowStyleMaskFullSizeContentView`;
  `layoutContainer` keeps the split below the title bar with
  `contentLayoutRect`, empty before the window is shown, so `titlebarHeight`
  falls back to frame math (or the split is 0 px tall). With a custom title bar
  (any `titlebar.*` key, or a non-opaque window) `titleLabel` mirrors
  `window.title` via KVO.
- **Translucency**: paint a translucent color once, as a plain layer color.
  `PanelScrollView` draws no scroll or clip background and sizes a layer-backed
  backdrop in `tile`. Traps: a clip view's translucent `backgroundColor`
  renders opaque; NSScrollView never resizes extra subviews (an autoresizing
  backdrop stays 0x0); the backdrop stops where the gutter starts, which paints
  its own. Property checks passed every time; only pixels caught these.
- **Manual frames**, not Auto Layout. Flags: `sidebarCollapsed`,
  `editorHidden`, `terminalVisible`, `browserVisible`, plus NSSplitView's
  collapse of `rightArea`. Position the sidebar divider only after the window
  is on screen and laid out, or it collapses to zero. Cmd+B fought the
  delegate's 160 px minimum; `sidebarCollapsed` lets it drop to 0. Dragging the
  divider far right collapses the right pane (`canCollapseSubview:`, a 5 px
  grab area either side, minimum 150). `relayoutRightArea` re-raises the drag
  bars (the terminal's last) only when out of order, never during a drag,
  counting the agent bar last. Bars take the cursor from a tracking area
  (cursor rects lose to text views) and clear their hover light in
  `viewDidHide`.
- **Editor, browser and terminal**: the top splits into `editorRect` and
  `browserRect` (`browserFraction`, `kMinSideWidth` 150 a side), and every view
  in the editor's slot gets `editorRect`. A bar dragged within 16 points of an
  edge hides that side; the smallest top is the tab strip plus 40 points
  (`kMinTopSlotHeight`). Shift+Cmd+E or the terminal bar at the top sets
  `editorHidden`; `collapseRightAreaIfEmpty`; opening a file, Cmd+1 and the
  preview toggle call `revealEditor`. Cmd+1 focuses what the tab shows
  (`focusActiveDocument`), never a text view hidden behind a viewer. The GTK
  port still puts the browser in the editor's place.
- **The page never takes the keyboard by itself.** WebKit makes the web view
  first responder whenever the page focuses a field, even hidden. The window is
  an `MCKeyboardWindow`, whose `makeFirstResponder:` asks `MCWebView
  mc_refusesKeyboard` before the current responder resigns (refusing in
  `becomeFirstResponder` left the window first responder). Allowed: a mouse
  down in the page, Tab from the toolbar, a page loaded from the toolbar while
  the keyboard is there; never while hidden. `navigateToString:` clears the
  toolbar allowance. Tab in the address bar goes into the page through the
  field's delegate while the field editor still has the keyboard.
- Window tabbing is off (`setAllowsAutomaticWindowTabbing:NO`, or Cmd+W closes
  a merged group); `window.releasedWhenClosed = NO` (or ARC and the legacy
  release double-free it on close).

## Conventions

- **Keys**: a feature minicode shares with VS Code takes VS Code's Mac key.
  minicode's own Shift+Command keys never move (Shift+Cmd+K Request a Change,
  Shift+Cmd+T Toggle Terminal, Shift+Cmd+D TODOs, Shift+Cmd+E, Shift+Cmd+B,
  Shift+Cmd+H, ...); only a Command-only key yields when VS Code binds it to
  something else (Cmd+N New File, Cmd+L Select Line, Cmd+I the inline agent).
  Dump every menu key equivalent before adding one; no key is bound twice (the
  `keymap` suite and `commands:*` check it).
- **A new shortcut**: a row in `src/Commands.cpp` (id in VS Code's spelling
  where VS Code has the command, else `minicode.*`; title, menu, default keys,
  where it applies, selector, tag); the menu item in `main.mm` (and a
  forwarding method on AppDelegate if needed); a hints row (`appendHintsRow:`
  with `[self hk:@"id"]`, which reads the registry); a README table row, sorted
  by action, a column per platform. The hints panel is an attributed string on
  tab stops; never align it with spaces (⌘ ⇧ ⌃ ⌫ ⏎ differ in width). Prefer
  menu items with key equivalents to the key monitor: they work in every pane
  and for shifted variants.
- No third-party dependencies, ever; new capabilities use system frameworks.
- README and user-facing prose: plain sentences, Oxford commas, no emoji, no
  LLM-ish phrasing; the name is "minicode", lowercase, in prose. No labels the
  user did not choose.
- Commits in the repo's style (the area, then why and what was checked), ending
  with the Co-Authored-By trailer.
- A finished user-visible Mac change is pushed, released and upgraded on the
  user's Mac. Ask before anything that puts a window on their screen.

## Settings, themes and Adjust Settings

- `AppSettings` loads `~/.config/minicode/settings.conf` (or
  `$MINICODE_SETTINGS`), watches it, and posts the change notification. The
  command values of `lsp.*`, `format.*` and `run.*` are whole command lines up
  to a ` #` comment outside quotes, unlike other keys.
- **Swatches**: in the settings file each color value is a swatch
  (`NSLinkAttributeName` `minicode-color`); a click opens NSColorPanel and
  `colorPicked:` rewrites the line with `Settings::setColor` (uncommenting it),
  saving after 150 ms. An opacity value is a `minicode-opacity` link opening
  `MCOpacityPopover` (`Settings::setOpacity`, keeping a `%` form).
  `linkTextAttributes` is only a pointing-hand cursor. Trap: an NSTextView with
  `usesFontPanel` sets the shared NSColorPanel to the color under the caret
  when the selection moves, and the panel forwarded that to `colorPicked:`, so
  every pick became the text color; the editor sets `usesFontPanel = NO`.
- **Adjust Settings…** ends every surface's context menu and opens the file at
  that surface's keys (`Settings::appearanceSurfaces()`: `tree`, `git`,
  `editor`, `syntax`, `markdown`, `terminal`, `browser`, `statusbar`,
  `titlebar`, `window`, placeholders `tabs` and `terminal-tabs`; own keys, then
  the `window.*` fallbacks). A new view adds it with
  `MCAddAdjustSettingsItem(menu, @"surface", self)` at the end of
  `menuForEvent:` (`willOpenMenu:withEvent:` for WKWebView) or `view.menu =
  MCAdjustSettingsMenu(...)`; `MCSurfaceTextField` does it for a field's
  editing menu (the field editor asks the field, never its delegate). The keys'
  lines are tinted with temporary attributes for about 3 s. A file older than
  some keys gets their commented defaults (`Settings::missingSurfaceLines`)
  appended under one "# Added by Adjust Settings" line, one undo step, saved at
  once only if it had no unsaved changes. The ruler is left out of the map on
  purpose (old files would get new lines).
- **Themes**: `theme = <id>` (`kThemes`: dark-modern, dark-plus, monokai,
  solarized-dark, tomorrow-night-blue, light-modern, light-plus, quiet-light,
  solarized-light, mapped by hand from VS Code's files) gives every color; any
  color the file sets wins. Unset is dark-plus, value for value the old look;
  `defaultFileText` keeps Dark+'s values. Opacity is separate. View > Theme
  (`MCThemeSubmenu`) writes the line through `Settings::withTheme` (CRLF kept)
  and, when the file sets colors of its own (`colorOverrides`), offers to
  comment them out. `Settings::isDark()` sets the window's NSAppearance;
  popovers that force DarkAqua and inline hex are unchanged.
  `selectedTextAttributes` is only a background, since after a switch AppKit
  drew selected text in its own color.

## Keys: the registry, keys.conf and the key monitor

- `src/Commands.cpp` has every command (about 160) in menu order: id, title,
  menu, default keys (`shift+cmd+p` notation), where it applies (`Any`,
  `Editor`, `Terminal`, `Tree`), selector and tag, flags (`Global`: the key
  monitor runs it before any view, the grid included; `Fixed`: a view's own
  keys, listed and protected but not changeable yet). `resolve()` applies
  `keys.conf` (`~/.config/minicode/keys.conf` or `$MINICODE_KEYS`): `key = id`
  adds, `key = -` takes the key from every command, `key = -id` from one; a key
  given to a command leaves any that had it where both apply; two lines giving
  one key to two commands in one place are a conflict and the later is refused
  with its line number.
- `Keymap.mm` reads and watches the file and puts the keys on the menu items
  found by action and tag (the first on the item, others on hidden items beside
  it, `allowsKeyEquivalentWhenHidden`). Errors join the settings errors in the
  status bar; the hints, the palette and minicode > Keyboard Shortcuts… (which
  writes the file, every command commented out, the first time) follow it.
- The key monitor asks `MCKeymapHandleKeyDown` first, which runs a command
  (validated as its menu item would be) when it is Global (Ctrl+` and
  Ctrl+Shift+`, the Menu key, Cmd+keypad 0, Option+] and [ during a tour); when
  the key has neither Command nor Control (Shift+Option+F and O, F2, F8, F9,
  Shift+F12) and the editor has the keyboard, since AppKit gives such a key to
  the text view as typing before the menu (in another field or the grid it
  types); when two commands share a key in different places; and for keys a
  menu cannot show (keypad digits, the Menu key, Ctrl+Shift with punctuation);
  and for a `Tree` command (renameFile on F2, moveFileToTrash on Cmd+Delete)
  while the tree has the keyboard. A `Tree` command's keys are never put on
  its menu item, since a menu key acts from every pane (Cmd+Delete once
  trashed the tree's selection from the editor); no view hard-codes them.
  The grid's prompt line edits (Cmd+Left/Right/Delete, Option+Left/Right) and
  the tree's Space and Esc (Quick Look) are `Fixed` rows: typing keys, or
  keys the view sends on to the shell. A plain Control key goes to the terminal grid while it has the keyboard
  (Ctrl+G for nano) unless Global. Plain typing skips the keymap at once. Cmd+.
  is the key bindings' `cancelOperation:` everywhere, so only an editable
  editor sends it to the menu. Validation decides: a command that cannot act
  lets the key go (Option+] types ‘ with no tour).
- **Trap (1.5.0 and 1.5.1)**: Shift+Option+F typed Ï, because the "verified"
  check had sent the event to the menu directly. Test such keys through
  `[window sendEvent:]` or `MCGuiSendKeyToApp`, never the menu alone.

## Tabs, sessions and the disk (Mac)

- **One text view, many documents.** `MCTab` holds a document's path, text,
  `savedText`, mode flags, `dirty`, its own `NSUndoManager` (from
  `undoManagerForTextView:`), its own `LatexView`, folds, viewer kind,
  `agentPreview`, `comparison` and where its view was. The controller's
  document properties are forwarding accessors to `_activeTab`
  (`MC_TAB_PROPERTY`), nil/NO with no tab. Shared viewers are let go by
  `stashActiveTab` (a playing sound stops) and rebuilt by `showActiveTab`. A
  session's tab is read when first shown (`loaded`). Diffs and commits share
  one tab.
- `EditorTabs` rules: a new tab right of the active one, the most recently used
  after closing the active one, wrap on next and previous, labels that add
  folders when names clash. `TabStrip` only draws and reports clicks. A tab's
  right click (`menuForTabAtIndex:`) has the close commands, copy paths,
  reveal, rename and trash; the File menu repeats them for the active tab so
  the palette finds them.
- **Dirty means "differs from disk"**: `noteEdited` compares with `savedText`
  (length first), so undoing back clears the dot. `savedText` is the text as
  read (CRLF survives); a UTF-8 BOM is kept apart in `MCTab bom` and written
  back.
- **One writer**: Cmd+S, `saveTab:` and auto-save all go through
  `writeText:toTab:interactive:` over `FolderSearch::writeFileAtomically`
  (a symlink stays a link, hard links and files owned by someone else are
  written in place, xattrs, ACL and creation date kept, F_FULLFSYNC), which
  refuses a tab without text of its own (`tabHasTextToWrite:`: SQLite, Quick
  Look, a large CSV) and sends didSave. `writeToFile:atomically:` replaced a
  link with a plain file. What a tab knows of its file is a stat with
  symlinks followed (`MCDiskStamp`: date, inode, size; `tab:changedOnDisk:`
  compares all three), taken before the read (`MCReadDiskText`, over
  `FolderSearch::readFileStable`), so a file still being written never reads
  as clean; `attributesOfItemAtPath:` did not follow links.
- **Keys**: Ctrl+Tab / Ctrl+Shift+Tab, Shift+Cmd+] / [, Shift+Cmd+1..9 and
  Ctrl+1..9 (macOS takes Shift+Cmd+3, 4, 5). Cmd+W (`closeFocused:`) closes the
  tab, or the terminal, browser or Problems list with the keyboard; a repeat
  (`isARepeat`) is ignored, so a held Cmd+W closes one thing. Shift+Cmd+W
  closes the window. Cmd+N New File (with no window, opens one), Option+Cmd+N
  New Window. Reopen Closed Tab (Option+Shift+Cmd+T): the newest of the
  window's last 50 (`_closedTabs`, with place, selection, scroll, mode),
  skipping files gone.
- **Unsaved tabs are never dropped**: a dirty tab asks Save / Don't Save /
  Cancel; window close, Open Folder and quit ask once for all. Trash closes
  only clean tabs of what went; `pathMovedFrom:to:` moves tabs.
- **The disk**: a buffer replaced from disk by choice (Reload) loses its undo
  steps (`forgetHistoryOf:`). Every save checks the stamp first (`mayWriteTab:`:
  changed outside asks Keep Mine / Reload / Cancel; gone asks before writing).
  Save All asks every question first (`diskDecisionForTab:`), then writes.
  `saveTab:` saves a background tab in place (a keep-sorted sort waits in
  `pendingSort`). Moved or deleted outside (FSEvents, `checkTabsOnDisk`, show
  and focus): a same-folder rename is followed by inode and date (`fileID`),
  else the tab is `gone` ("(deleted)") until the file returns, and is read in
  the same pass when it does. **A clean tab follows the disk**: from FSEvents
  (`followCleanTabInBackground:`) the file is read and diffed off the main
  thread, generation-counted, and a file over 8 MB is only noted and read on
  show or focus; on show or focus (`reloadCleanTab:`) on the main thread. The
  new text goes in as one undo step, "Reload from Disk", keeping caret, top
  line and the steps before it (`replaceSourceWithText:`, or for a tab not on
  show as text an undo action of its own, `registerReloadUndoForTab:`; an
  NSTextView registration made during an undo redid onto the wrong range).
  A read that fails moves no date.
- **Sessions**: `MCTabSessions` (NSUserDefaults, by resolved folder, 50
  folders) reopens a folder's tabs and its browser page, else a read-only empty
  editor. Off under `MINICODE_DEMO` and `MINICODE_BENCH`;
  `MINICODE_DEFAULTS_SUITE` points it at a scratch suite. A tab over 16 MB or
  not a plain file waits for a click. Windows on one folder share it: each save
  lists the saving window's files then the others'. Agent preview tabs are left
  out.
- AppDelegate's `current` maps a window an editor made (`ownsWindow:`, Find in
  Folder and TODOs) to that editor; items that edit the file on show are
  disabled while another window is key, so keys reach that window's field.
- GTK and Android have no tabs.

## Backups, hot exit and auto-save (Mac)

- Every dirty tab's text is on disk while edited, as VS Code's backups;
  quitting still asks (the user wants the question). `Backups.{h,cpp}` (format,
  `Store` over an `Fs`, `plan`, `foldersToReopen`, `expired`, `compare`),
  `BackupStore.mm` (folder, write queue, which process holds a backup),
  +Persistence (what and when). Store: `~/Library/Application
  Support/MiniCode/Backups` (NSHomeDirectory, so `CFFIXED_USER_HOME` moves it)
  or `$MINICODE_BACKUP_DIR`; never in the project. One `<uuid>.mcbackup` per
  tab whose header is its own index entry, so processes never race. Written to
  a temporary (0600), flushed with `fcntl(F_FULLFSYNC)` (plain `fsync` leaves
  data in the drive's cache; fsync where a volume refuses, and on Linux),
  renamed, and the folder synced too.
- `scheduleBackups` (from `noteEdited` and `updateTitle`): a clean tab's backup
  goes at once; a changed dirty buffer (`sourceText` compared by pointer with
  `backedUpText`) is written 1 s after typing stops, at most 5 s after the
  first unwritten change, the immutable NSString converted on the queue. Only
  text tabs with a path.
- Restore: `planForRoot:` takes the folder's backups no live process holds (pid
  and start time); `takeBackup:intoTab:` makes the tab dirty with the backup's
  base date and inode, and on first show (`showRestoredText:`) the backup is
  the buffer, the disk text `savedText`, undo empty, with a note ("Restored
  unsaved changes to x", and if the disk changed since).
  `reopenFoldersWithBackups` opens a window per folder with unheld backups. The
  newest of several wins; orphans over 30 days and temporaries over a day go
  (`cleanUpOnce`).
- Quit asks as before, then `backupsAppWillTerminate:` removes them. A restart
  from Request a Change calls `keepUnsavedTabsForRestart` (dirty buffers
  written synchronously and not asked about; a failed write falls back to the
  question). A crash, SIGKILL or power loss leaves them.
- **Auto-save**: `editor.auto-save` = off (default), `after-delay`
  (`editor.auto-save-delay`, 1000 ms) or `focus-change` (the editor loses the
  keyboard, the window resigns key, another tab shows). `scheduleAutoSave` sits
  beside `scheduleBackups`. A plain write: no formatter, no keep-sorted, never
  a dialog from a timer (`writeTabForAutoSave:` checks the disk by hand; a file
  changed outside is skipped and named, so Cmd+S asks). Skipped: gone and
  unloaded tabs, a tab whose Cmd+S waits for its formatter, a tab with an
  inline-agent edit awaiting Keep or Discard (`MCAgentSession editPending`),
  and the tab on show while an input method composes (`hasMarkedText`). The
  close and quit questions run `autoSaveNow` first, so auto-save's writes are
  not asked about. A restored backup opens in its viewer (a CSV's table).
  Trap: the focus-change save and the delay's debounce shared a selector and
  `updateTitle` cancelled the save after a tab switch; nothing may cancel
  `autoSaveNow`.

## The editor's text view (Mac)

- **The editor is TextKit 1.** AppKit gives any NSTextView subclass that
  overrides `drawRect:` a TextKit 1 layout manager, with no notification. Never
  ask a TextKit 2 view for its layout manager (it switches over).
- **Storage**: `MCSourceStorage` is an NSTextStorage over a plain
  NSMutableAttributedString that fixes attributes eagerly (only the edited
  paragraph) and skips the glyph-info fix unless glyph info is in the range.
  AppKit's storage re-fixed a 65,536-character block around the caret twice a
  keystroke and walked every run for glyph info: about 11 ms a keystroke in a
  highlighted 100,000-line file, now about 0.5 ms (opening such a file costs
  more, about 220 ms). EditorController builds the TextKit 1 stack itself and
  makes the view with `initWithFrame:textContainer:`. **Never
  `replaceTextStorage:` after the view exists**: CodeTextView, the carets and
  the folds subscribe to the storage at init and heard nothing on a new one.
- **Contiguous layout stays on**: everything that reads positions relies on
  exactness. Never call `numberOfGlyphs` on the editor; lay out only up to the
  character needed. `textDidChange:`'s copy into `sourceText` shares characters
  (about a microsecond) and the backups rely on a new object per change;
  measured, leave it.
- **Programmatic edits** go through `CodeTextView replaceRanges:withStrings:
  actionName:` (Lsp.h): `replaceSourceWithText:actionName:` (Format
  Document, the inline agent, a clean tab following the disk, a pending
  sort), several carets (`MCMultiCarets applyEdit:`) and Replace All; the
  workspace edit applier and completion's extra edits (Lsp.mm) have the same
  shape. One `shouldChangeTextInRanges`, each range its own storage edit,
  last first, one `didChangeText`, so layout runs once and the highlighter,
  folds, squiggles, gutter, Go Back and server follow each edit. Never wrap
  them in `beginEditing`/`endEditing`: the storage then reports one edit from
  first to last, folds between open, squiggles stretch over it, and the
  highlighter relexes all of it (a quote at two carets 99,800 lines apart
  took 4.5 s; 3 ms now). The undo step is the view's own (allowsUndo off
  around shouldChangeTextInRanges), putting the old text back as plain
  strings through the same method: NSTextView's inserts each as an
  attributed string, splitting the attribute runs one at a time, and
  undoing a letter typed at 20,000 selections ran for minutes. The
  highlighter keeps its pending ranges apart (`_pending`, distances from the
  end) and restyles each in its own attribute edit. Select All Occurrences,
  Cmd+D and Add Cursor stop at 10,000 carets (VS Code's limit); typing at
  10,000 in a 100,000-line file still costs about a second, most of it the
  flat string's memmove per edit. The caret and top
  character are mapped by counting non-whitespace characters within a hunk;
  hunks with only blank lines between are joined. Small edits (Cmd+/, F9) call
  `breakUndoCoalescing`, so each is its own undo step.
- `CodeTextView` hooks: `onFocusLost`, `onMouseHover`, `onPreviewDoubleClick`,
  `onFindAction`/`validateFindAction`, `drawBehindText`. The ruler
  (`editor.ruler`: a column, a list such as 80,120, or off, 100 by default;
  `editor.ruler-color`) is a 1 pt backing-aligned line at container origin +
  padding + column times the width of "0", drawn in
  `drawViewBackgroundInRect:`, none while read-only.
- **Line numbers** (`MCLineGutter`): a vertical NSRulerView; each logical line
  numbered on its first fragment, the carets' lines in the active color, a dot
  for errors and warnings, the fold lane (`kFoldLane`). Only fragments on
  screen are visited and numbers come from a `LineIndex` fed by storage edits
  (with the characters either side, since a line ends after '\n', U+2029 or
  a '\r' not before '\n', as in TextFind; the folds count lines the same way).
  Width by digit count (three at least); click, drag and Shift+click select
  lines; shown only while editable. Settings: `editor.line-numbers` (on, off,
  relative), `editor.gutter-background`, `editor.line-number-color`,
  `editor.line-number-active-color`. Traps: NSScrollView lays a ruler out as a
  content inset of a clip view that still spans the width, so `PanelScrollView
  tile` subtracts the insets; never change `ruleThickness` inside a storage
  edit (tile asked for text that was gone and threw), so it changes on the next
  run-loop turn.
- **Brackets**: the bracket by the caret (the one before first) and its partner
  get boxes in `editor.bracket-match`; strings and comments skipped by the
  highlighter's tokens within 20,000 units; nothing lexed unless a bracket
  touches the caret. Go to Bracket is Shift+Cmd+\.
- **Zoom** (EditorFont.mm): Cmd+=, Shift+Cmd+=, Cmd+-, Option+Cmd+0, Cmd+keypad
  0; Cmd+0 stays Focus File Tree. `MCEditorFont()` is `editor.font-size` (13)
  plus `MCEditorZoom` (user defaults, every window, never in the settings
  file); `MCZoomScale` is (13 + zoom) / 13 for the Markdown preview
  (re-rendered keeping its place) and the edit popovers; the terminal adds the
  zoom to `terminal.font-size` (`MCEditorZoomDidChangeNotification`).
- `_liveHighlight` must be off whenever the storage holds something that is not
  source (previews, messages); set it before every `setAttributedString:`.

## Highlighting

- `SyntaxHighlighter` lexes a line at a time from a `LexState`;
  `IncrementalHighlighter<Ch>` keeps line starts and end states and re-lexes
  from an edit until a line ends in its old state. States over 16 bits (a
  raw-string delimiter, a template stack) keep their text in a process-wide
  table (`stateTextId`), so equality stays two integer compares. Tokens start
  and end at ASCII characters, so UTF-8 and UTF-16 agree.
- Grammars worth knowing: Python f- and t-strings (fields lexed as code; a
  triple-quoted or continued f-string carries `0x100` in its quote; a field
  open at a line's end is not carried); `lexJsLine` for JS and TS (regex
  literals only where an expression can start, `kJsBeforeRegex`, never after a
  name, number, string, `) ] }` or `<`, closed on their line; template literals
  across lines through a stack); `lexLongString` (C++ raw strings, Rust raw
  strings, Go backticks, triple quotes in Java, Swift, Dart, Groovy, Kotlin and
  Scala, C# verbatim and raw); Rust lifetimes are not strings; `lexTexLine` (a
  blank line ends open math, so an unclosed `$` colors only its paragraph). Not
  done: Swift `#"..."#` and `\(...)`, Kotlin/Scala/Dart/C# templates, Groovy
  slashy strings, a block comment across lines inside a JS template field, a
  regex at line start continuing a division.
- Tests: `tests/LegacyHighlighter.h` is the frozen old lexer, an oracle under
  random edits; where it cannot apply (`newGrammar`, `mayHoldFString`,
  `mayHoldNewLiteral`) the edits compare against a full lex.
- The Mac (+Highlighting): `applyHighlighting` is the full pass (and
  `recolorEditor` on settings); `textStorage:willProcessEditing:` feeds edits
  to the highlighter and records lines; `textDidChange:` (or a zero-delay
  perform when there is no `didChangeText`) calls `flushHighlighting`. Up to
  64K characters, `restyleChangedIn:` sets attributes only on pieces whose run
  differs (each merge or split memmoves the run array). Traps: never change
  attributes inside `willProcessEditing` (it widens the edit and the caret
  jumps to its end); reset a range with one `setAttributes:`, never
  `addAttribute:` over many runs (3.4 s for 50k lines against 1 ms).
- Linux retags in idle slices over a UTF-8 mirror; Android's `Highlighter.kt`
  uses the core over a native mirror.

## Folding (Mac)

- `Fold::compute` finds regions as whole lines: brackets for the C-like
  languages and .bib (the closer's line stays in view when only closers precede
  it, so `} else {` reads as in VS Code; brackets in strings and comments do
  not count), indentation for Python, YAML and the rest (trailing blanks
  excluded; lines starting inside an earlier string or comment neither open nor
  close), headings, fences and display math in Markdown, sections and
  environments in LaTeX, block comments, docstrings, comment runs, and
  `#region`/`#endregion` in the usual spellings. `sanitize` nests any list (the
  server's ranges too). `Fold::Collapsed` keeps closed folds as UTF-16 spans,
  so they follow edits; an edit touching what one hides opens it.
- `MCFolds` (FoldView.mm) is the layout manager's delegate: hidden characters
  get null glyphs except the break ending the first line, which becomes a blank
  as wide as the placeholder the view draws. The text never changes, so the
  gutter, undo, the highlighter, copy (copies what is hidden) and carets see it
  whole; arrows step over null glyphs; squiggles under folds are not drawn.
  Backspace after or Delete before a placeholder opens the fold; Return after
  it makes a line below. A selection landing inside a fold opens it
  (`setSelectedRanges:`), which is how Find, Go to Line, definition, F8 and
  Select All Occurrences reveal their target. Folds are per tab and forgotten
  on a reload from disk.
- Regions recompute off the main thread 0.3 s after typing (generation
  counted); `textDocument/foldingRange` is asked at 0.6 s and wins for its
  text. Chevrons show while the pointer is over the gutter.
- Keys: VS Code's Option+Cmd+[ ] are minicode's terminal switching and its
  other fold keys are Cmd+K chords, which minicode has none of, so View >
  Folding is Control+Command plus the chord's second key ([ ] L 0 J / and 1 to
  7). Control+[ types ESC: test these as real key codes.
- Trap: one region at a time was quadratic (6 s and 18 s for Fold All and
  Unfold All of 50,000); they go in as one sorted merge and one invalidation
  (`collapseAll`).

## Several carets and line commands (Mac)

- NSTextView keeps one empty selection, so carets live in `MCMultiCarets`
  (UTF-16); the view's selection holds the non-empty ones or the first caret,
  and the view draws every caret after `drawRect:`. Any change not made by the
  carets (a click, undo, reload, tab switch, Find), Esc, or an unknown command
  (run at the first caret) ends them; scrolling passes. Edits go through
  `replaceRanges:withStrings:actionName:` as one undo step; the server
  sees no single typed character (no completion); the electric dedent is off
  while they apply (`multiCarets.applying`); typing never crosses a line break,
  so a column stays a column.
- Column: Cmd+Shift-, Option- or middle-button drag (`MCMultiCarets mouseDown:`
  and `otherMouseDown:`, first in CodeTextView's), each line's caret from
  `characterIndexForInsertionAtPoint:` at its first fragment; Cmd+Shift clicks
  pair up (the owner's binding), the anchor holding while carets and length
  are as the last press left them (`_anchorCarets`); Shift+Option click or
  drag is VS Code's box from the caret's selection start (or the live
  anchor) to the pointer; Option+click adds or removes a caret. VS Code's
  own Cmd+Shift click is a Shift click. **Trap (2026-11-06)**: the window
  server counts clicks by time alone (HIDClickSpace is (0,0)), so a second
  click within 0.5 s is clickCount 2 wherever it lands; the old `clickCount
  > 1` bail sent the owner's quick second Cmd+Shift click to NSTextView. A
  press counts as a double-click only on the character the last press hit
  (VS Code's `trySetCount` does the same). Offscreen, neither `[window
  sendEvent:]` nor CGEventPostToPid delivers a mouse-down to the view;
  `MultiCaretClickSuite` hit-tests and calls the view with CGEvent-made
  events carrying the real click counts.
- Cmd+D (word at an empty caret by `MultiCaret::wordAt`, then the next
  occurrence after the last added, wrapping, case-sensitive, whole-word only
  when begun at an empty caret), Shift+Cmd+L, Option+Cmd+Down/Up (a caret a
  visual line away at `_goalX`). Shift+Cmd+Up/Down and Shift+Option+Up/Down
  copy lines; Cmd+Up/Down and Option+Up/Down move lines (neighbouring carets as
  one block; nothing moves at an edge); Cmd+Home/End go to the file's ends.
  They are caught in CodeTextView by selector and arrow key code from the event
  `keyDown:` recorded (`_keyEvent`; `NSApp.currentEvent` is unset for a direct
  `keyDown:`), because Shift+Home and Shift+End send the same selectors;
  Option+arrows by key code in `keyDown:`. Not menu items, which would take
  keys from the find field and commit box. Cmd+Delete deletes to the line's
  start (the tree takes it as Move to Trash in its own `keyDown:`; as a menu
  key it trashed the tree's selection from the editor).
- Up and Down with many carets walk the layout manager a line at a time (goal x
  by binary search over glyph starts; `glyphIndexForPoint:` was most of the
  time); before, carets on lines not laid out got empty rects and merged.
  Drawing places only carets among the visible characters (placing all laid out
  the file: seconds a frame for 50,000).
- An opener (`( [ { " ' \``) typed while every caret has a selection wraps each
  and keeps it selected (`MultiCaret::surround`), never while an input method
  composes.
- Line commands: Cmd+L (`expandLineSelection`), Ctrl+Shift+K (`deleteLines`),
  Cmd+C and Cmd+X with nothing selected take whole lines (`lineCopy`) and mark
  the item `org.minicode.editor.line-copy`; pasting such an item with nothing
  selected goes above the caret's line (`pasteLinesAbove`). Only while
  editable; `copy:` and `cut:` validate with an empty selection there.

## Indentation and sorting (all three ports)

- `AutoIndent::scan` reads from the top to the caret: strings (Python prefixes
  and triple quotes, JS templates, Go raw strings; Rust lifetimes are not
  strings), comments (`#` only at a word start in shell and YAML; no `//` in
  CSS or JSON), and open brackets with their lines. Languages: Python, CLike,
  Yaml, Generic (brackets only), Plain (keep the indentation). `ScanCache`
  keeps the scanner's state at line starts about 4,096 characters apart (a
  state depends only on what precedes it) and `detect`'s answer; the Mac keeps
  one per text storage, fed from `NSTextStorageDidProcessEditing`, and every
  edit must be reported.
- Python Return, in order: in a string keep the indentation; in a bracket,
  `(|)` opens the pair, a moved closer takes the opener line's indentation,
  text after the opener aligns with it, else a hanging indent; otherwise a
  trailing backslash indents once, a block colon indents,
  return/pass/break/continue/raise dedent (only with nothing moved down), else
  the statement's first line's indentation. CLike: one unit after an opener,
  else the line's own or the bracket opener line's; " * " in `/*`.
- Electric dedent (`typed`): a closer alone on its line takes its opener's
  indentation (for `(` and `[` only when the opener ended its line); in Python
  the colon of else, elif, except, finally or case moves the line to the
  nearest shallower statement start of the right kind.
- Indent unit (`detect`): tabs if more lines start with one, else the commonest
  step of 4, 2, 8 or 3 after a block opener; defaults 4, 2 in YAML, tabs in Go
  and Makefiles; `.tsv` never soft tabs. No settings key: the file decides.
- `KeepSorted` is keep-sorted's Go code (v0.10) step for step, including the
  regex options through std::regex with a small RE2 translation (`\p`, `\Q` and
  the like leave a block alone with a message); a marker without its partner
  leaves the file alone; a block whose groups would split a nested block is
  left alone (keep-sorted scatters the markers). Fuzzed against the real tool;
  its golden files are `tests/keepsorted` (Apache 2.0). An outermost block
  sorted to nothing leaves one empty line, as keep-sorted does.
- F9 sorts a selection of two or more lines (stable, by text after leading
  spaces) or the innermost block; Shift+F9 every block;
  `editor.keep-sorted-on-save` (not a tab showing a preview). Each edit is one
  replacement and undo step (Mac: `breakUndoCoalescing`).
- Ports call in from their key paths: the Mac's `textView:doCommandBySelector:`
  through `MCIndentCommand`, Linux `onViewKey` unless completion is open,
  Android `CodeEditText` (keys, committed "\n" and "\t",
  `deleteSurroundingText`, the symbol row's Tab).

## Find and replace (Mac)

- `TextFind` matches a line at a time: plain text with FolderSearch's
  one-to-one folding, or std::regex (ECMAScript) over UTF-32 code points. Whole
  word is VS Code's edge check; replacements take `$1`..`$99`, `$&`, `$0`,
  `$$`, `\n`, `\t`, `\\` for a regex. Traps: libc++ lets `^` match mid-line
  with `match_prev_avail`, so lines are walked with `std::regex_iterator`;
  libstdc++ tests bracket ranges through the C locale's ctype, so `[а-я]`
  ignoring case missed Ж on CI's Linux job, and a case-insensitive regex now
  runs over the line folded to lower case, one code point for one.
- `MCEditorFind` replaced NSTextFinder (`usesFindBar = NO`): a box at the
  editor's top right with "3 of 17", the toggles, and a replace row
  (Option+Cmd+F). It searches as you type on a serial queue (cancelled between
  lines, applied `whenCurrent:`) and draws through `drawBehindText`. Cmd+G /
  Shift+Cmd+G, Return / Shift+Return; Option+Cmd+C, W, R; Return in the replace
  field replaces and moves on; Shift+Cmd+1 replaces only while the bar has the
  keyboard (Go to Tab 1 elsewhere); Option+Cmd+Return replaces all as one
  programmatic edit; Esc; Cmd+E. A preview finds but does not replace. Menu
  items still send `performTextFinderAction:` with NSTextFinderAction tags, so
  the terminal and diff view answer their own.
- Find in Folder uses `FolderSearch::search` (two characters, one for an
  untrimmed regex; dotfiles and `node_modules`/`build`/... skipped; 1 MB and
  UTF-8 only; 2000 matches; ANSI stripped; symlink loops and named pipes safe;
  generation-counted and cancelled per file). Replace in Folder
  (Option+Shift+Cmd+F) counts and asks; a file in a clean tab here is edited in
  the tab through the rename applier (`applyEditsToPaths:`); a file with
  unsaved text in any window is left alone and named; others go through
  `FolderSearch::writeFileAtomically` (mode kept, synced, renamed; symlinks
  followed; hard links written in place). A file under two names (a symlink,
  a hard link, a symlinked folder) is counted and replaced once, by device and
  inode, and one reached through a link to outside the folder is named in the
  question (`FolderReplaceFile::outside`). Replace Match replaces only while
  the row's line still reads as searched. `replaceOneAt` maps a result's byte
  column to a tab's UTF-16.

## Formatting (Mac)

- Shift+Option+F pipes the buffer through an external formatter.
  `format.<python|cpp|go|rust|shell>` (`{file}`, `off`) runs as written. Else
  the nearest folder's config (up to the open folder or a `.git`) decides:
  `[tool.pyink]`, `.style.yapf`, `[yapf]` in setup.cfg, `[tool.black]`,
  ruff.toml, .ruff.toml or any `[tool.ruff]` (lint settings count, but pyink,
  yapf or black in the same folder win), `.clang-format`, rustfmt.toml, an
  `.editorconfig` shell section; for .js .mjs .cjs .jsx .ts .mts .cts .tsx,
  before clang-format, Prettier (`.prettierrc*`, `prettier.config.*`,
  "prettier" in package.json, or Prettier as a dependency or in
  node_modules/.bin, since it needs no config file;
  `prettier --stdin-filepath {file}`) or Biome
  (biome.json[c]; `biome format --stdin-file-path={file}`), from
  `node_modules/.bin` first (`Formatter::nodeBinDirs`). That tool runs without
  style flags, and when it is missing the user is told to install it, since
  another would restyle the project. Without config, Google's defaults:
  `clang-format --style=file --fallback-style=Google`, pyink (`--line-length 80
  --pyink-use-majority-quotes`, Google's published 4-space guide) then yapf
  `--style=google`, gofmt, rustfmt with Cargo.toml's edition, `shfmt -i 2 -ci
  -bn`. No LSP formatting fallback (clangd and pylsp format in other styles).
- `MCFormatText`: `MCFindProgram` (never xcrun shims), venv bins first for
  Python; stdin to stdout in the file's folder, wide PATH, `PYTHONUTF8=1`, 10 s
  then SIGTERM and SIGKILL. A non-zero exit, empty output or non-UTF-8 leaves
  the buffer alone with stderr's first 6 lines in a popover. Paths are planned
  resolved; a result for changed text is dropped; a selection passes `--lines`
  (clang-format, yapf) or `--line-ranges` (pyink, black).
- `format.on-save`: Cmd+S (non-nil sender) formats then saves; minicode's own
  saves and Save All never format. A Cmd+S is never lost (`_formatSaveTab`:
  leaving the tab saves it unformatted).

## Install hints (Mac)

- One table, `Install::server` / `Install::formatter` (Python's first way is
  `uv tool install basedpyright`, which brings its own Node; Homebrew's node
  broke pyright here). `suggest` picks the first way whose installer
  `MCInstallerAvailable` finds, else adds `brew install uv` or a link;
  `$MINICODE_INSTALL_HAVE` fakes installers. `install:readme` ties it to the
  README.
- `MCInstallBar` under the strip when a wanted server is `NotInstalled`, once
  per language per launch (`MCInstallBarTakeOffer`), until ×, Don't Show Again
  (`MCInstallHintsMuted`), or the server appearing. `MCInstallView` (command,
  Copy, Run in Terminal, alternatives) is in the status item's popover, the
  servers card and Format Document's note. Run in Terminal only types
  (`typeCommand:`). Copy uses `MCInstallPasteboard()` (`$MINICODE_PASTEBOARD`).
  `lookAgainForMissingServer` runs on window key and
  `MCTerminalCommandDidFinishNotification`, so no restart is needed.

## The palette, symbols and Go Back (Mac)

- Shift+Cmd+P opens it with ">", Cmd+P empty (Quick Open), Ctrl+G with ":",
  Shift+Cmd+O with "@" ("@:" groups by kind), Cmd+T with "#"; the first
  character decides (`CommandPalette::parse`). Ctrl+G goes to the terminal grid
  while it has the keyboard.
- `MCPaletteOverlay` is a view over the window, not a window, so the editor
  window stays key and validation sees the menu bar's responder chain. It
  remembers the first responder (a field, not its field editor, with its
  selection) and puts the keyboard back on Esc, a click outside, or running
  something; the keyboard moving elsewhere closes it.
- Commands are read from `NSApp.mainMenu` on each open (submenus `update`d;
  enabled, visible, non-alternate items as "Menu > Item", with the first key in
  effect). Return sends the action with `[NSApp sendAction:to:from:]`, the item
  as sender, on the next run-loop turn. Context-menu-only commands are not
  found; the File and View menus repeat them. `CommandPalette::rank`: a
  subsequence aligned by a small DP favouring word starts and runs, in tiers,
  then recent (`MCCommandPaletteRecent`), score, menu order.
- Quick Open: `listFiles` breadth first on a background queue (Find in Folder's
  skips, symlinked folders once, 20,000 files, cancelled on close); the name
  matches before the path; recent tabs and `MCQuickOpenRecent` first. Go to
  Line takes "42", "42:7" or "42,7".
- Symbols: `textDocument/documentSymbol` from the main server (`Lsp::Symbol`, a
  tree flattened parent first; `symbolAt` selects the caret's), else
  `SymbolOutline` (Markdown headings, LaTeX sections, Python def and class, the
  C family's functions and types). Moving through the list shows each symbol
  without taking the keyboard; Esc puts selection and scroll back.
  `workspace/symbol` goes to every running server that offers it, older
  requests cancelled, answers merged.
- Go Back and Forward (Ctrl+-, Ctrl+Shift+-): `MCNavigator` over `NavHistory`
  (places within ten lines are one entry; a new jump drops what lay ahead; 50
  entries; entries follow renames and leave with deleted files). Recorded: tab
  switches (`tabWillSwitch`/`tabDidSwitch`), every LSP jump
  (`LspSession.aroundJump`), Go to Line, terminal links, search and TODO
  results, symbols, and a caret moving over ten lines without the text changing
  (length-changing selections ignored). In the grid terminal Ctrl+- belongs to
  the program.

## LSP (read before touching it)

The protocol is `LspClient.cpp`, tested against a scripted server; on the Mac
`Lsp.mm` (`LspSession`), `LspServer.mm`, `LspPopups.mm` and `CodeTextView.mm`
own processes and UI; the GTK port has `linux/src/Lsp.cpp` over the same core.
GTK trap: a popover parented to a `GtkTextView` must be unparented before the
view is disposed (on `unrealize`), or dispose loops forever.

- **Hooks are few**: `documentOpened:` (nil for messages, binaries and
  previews), `documentSaved`, `setRoot:`, `shutdown`, `openPaths` and
  `bufferText` (every tab's text), `tabsChanged`, and
  `textView:doCommandBySelector:` to `handleCommand:`. The session observes the
  storage itself; didChange is debounced 0.3 s, every request flushes first,
  and unchanged text is dropped.
- **Every tab's file stays open** on its server (`syncOpenDocuments`: on show,
  server start, before rename and references, on `tabsChanged`); only a running
  server takes background tabs, so a tab never starts one. With only the file
  on show open, rename missed unsaved uses elsewhere and the server read
  edited-but-unsaved files from disk.
- Requests during the handshake are queued; server-to-client requests are
  always answered (nulls for configuration, -32601 for unknown methods).
- **Lookup** (`MCFindProgram`): PATH plus Homebrew, Cargo, Go, ~/.local/bin,
  npm's prefixes, nvm and fnm (newest first), volta, pnpm, bun, pip --user,
  pyenv, MacPorts, /usr/bin and keg-only llvm, since a Finder-launched app has
  a short PATH. `/usr/bin/clangd` and `/usr/bin/git` are xcrun shims that pop
  the install dialog: never run them; `MCDeveloperTool` finds the real binary
  through /var/db/xcode_select_link. The child's PATH puts the server's own
  folder first (an nvm script's `#!/usr/bin/env node` then gets nvm's node;
  Homebrew's node was broken here).
- **Settings**: `lsp.enabled`, `lsp.<cpp|python|go|rust|typescript>` (a command
  line or `off`), `lsp.<lang>.also` (auto, off, a command),
  `python.interpreter`. Python tries basedpyright, pyright, pylsp,
  jedi-language-server; basedpyright gets `typeCheckingMode = standard` unless
  the project sets it. A change restarts the servers.
- **States** (`MCLspEntry` per language and second server) show in
  `MCLspStatusField` and the Language Servers popover. Exits restart after 1,
  2, 4 s; the fourth crash in a row (three minutes up clears it) stays Crashed.
  The popover lives in a static (`gServersPopover`), so it holds sessions
  weakly (a strong capture kept a closed window's session alive).
- **Processes** (`MCLspServer`): pipes, reads on a background queue, writes on
  a serial queue with raw `write()` and `F_SETNOSIGPIPE`; stderr drained (a
  full pipe stalls clangd); `$MINICODE_LSP_LOG` logs all traffic. Window close:
  shutdown, exit, stdin closed, SIGTERM at 2 s, SIGKILL at 3; quit:
  `MCLspTerminateAllServers` with 0.5 s; a crash closes stdin, which ends them.
  `LspSession dealloc` cancels performs scheduled on itself (it crashed 50 ms
  after a window closed).
- **The second server**: `Lsp::defaultAlsoCommand` runs `ruff server` for
  Python in a project configuring Ruff anywhere up to the root or `.git`
  (`Formatter::configFile`), when ruff is found (the project's .venv first). It
  is its own entry ("python.also"). `_servers` per file, main first; `_docs`
  keyed by `DocKey`. `Lsp::routeRequest` sends one-server requests (completion,
  hover, signature, definition, references, rename) to the main server, also
  while it starts, else one that offers it. Diagnostics per server in
  `Lsp::DiagnosticStore`, tagged "(Ruff F401)"; code actions merged
  (`Lsp::mergeCodeActions`, 3 s); Organize Imports (Shift+Option+O) prefers a
  tool's `source.organizeImports.<tool>` (`pickOrganizeImports`); basedpyright
  offers none. Traps: Ruff's diagnostic source is "Ruff", its server name
  "ruff"; editing ruff.toml needs a restart (no dynamic registration).
- **Python interpreter**: the folder's choice (`MCPythonInterpreters`),
  `python.interpreter`, the nearest .venv/venv/env, `$VIRTUAL_ENV`,
  `$CONDA_PREFIX`, else the server's own (/usr/bin/python3 skipped). Sent only
  when it differs: `python.pythonPath` by workspace/configuration for the
  pyrights, `pylsp.plugins.jedi.environment`, jedi's `initializationOptions` (a
  restart). **An environment inside the folder needs the user's yes**
  (2026-11-06, review 3): a server runs the interpreter it is given and is
  looked for in its bin, so `serverPythonInterpreterWithSource:` leaves the
  .venv out until the folder's answer (`MCPythonEnvironmentConsent` in
  `MCAppDefaults()`) is "allow"; the install bar asks first (Use It, Don't Use
  It, × for this launch). Run File, which the user starts, still uses it
  (`pythonInterpreterWithSource:`).
- **Completion** opens after `.`/`->`/`::` and on a word's second letter; an
  `isIncomplete` list is asked again. `Lsp::completionKeyAccepts`: Tab takes;
  Return only when asked for (Ctrl+Space, Option+Esc), after a trigger
  character, or once the arrows moved, else a new line; an item equal to the
  typed word is a new line; Shift+Tab never takes. `filterCompletions` ranks
  exact, same-case prefix, any-case prefix, subsequence (sortText within) and
  reselects the first row unless the arrows chose an item still listed. Docs
  show beside the list after `completionItem/resolve` (80 ms). Snippets off.
  **Accepting** inserts the word and `additionalTextEdits` (imports,
  `#include`) as one multi-range replacement (`Lsp::completionEdits`),
  resolving first when a server sends them only then, for at most 0.4 s on the
  main run loop (`kResolveBeforeAccept`; keys pressed meanwhile arrive after).
  Signature help (`(`, `,`) sits above the line. Both close on focus loss, key
  loss and tab switch. Trap: the list's `close` must `reloadData` after
  emptying its labels, or layout asks for a vanished row.
- **Diagnostics are drawn, not attributed**: squiggles after `super drawRect:`
  for the viewport, from marks (NSRanges that follow edits). Kept per open file
  with the text they were computed on; `diagnosticSpans` places them on the
  text as it is now, leaving out ones about text typed over. Publishes for a
  version older than one since sent are dropped (versions count on across a
  close and reopen); `Client::textFor` gives a publish's text. Tags: an
  Unnecessary one is faded (no squiggle for a hint), a Deprecated one struck
  through, a plain hint three dots. Positions convert through one
  `Lsp::PositionIndex` per text, never a scan per position. Workspace edits
  carry the server's document versions from when they were asked
  (`Client::versions`, `staleEditFile`) and are refused for a file that moved
  on. Files no tab has open are kept apart (`_otherDiags`) for
  Problems.
- **Problems** (Shift+Cmd+M, `MCProblemsPanel`, above the terminal):
  `problemRows` (files with errors first, then severity, line, column),
  `countProblems`, `problemCountsText`, `problemOneLine`, `stepProblem` (F8 and
  Shift+F8, wrapping, through several at one place, the message in the hover
  box). Hints are left out (pyright sends one per unused name).
  `onProblemsChanged` is debounced 50 ms; `problemsLabel` toggles it; Cmd+W and
  Esc work in it.
- **Hover**: 0.4 s on a non-blank character (`characterIndexUnderPoint:` checks
  the character's own box) shows `MCHoverPopup`, a non-key child panel,
  diagnostics first, Markdown asked for first (the pyrights sent plain text
  otherwise). Option+Cmd+I shows it at the caret.
- **Definition**: dropped unless generation, document and selection are as
  asked (`detachDocument` bumps them). Targets are respelled in the root's
  form. Trap: `stringByResolvingSymlinksInPath` turns /private/tmp into /tmp;
  use realpath for URIs. `initialize` gets the resolved folder (basedpyright
  found every reference twice across /tmp and /private/tmp) and `CanonicalURI`
  merges spellings.
- **Rename, references, quick fixes** (F2, Shift+F12, Cmd+.): prepareRename
  when offered ("This can't be renamed"); `MCRenameBox` is a key-capable child
  panel (an NSPopover never shows for a window not on screen); references and
  fixes share `MCPickerPopup`, which never takes the keyboard (keys through
  `handleCommand:`). Fixes send the diagnostics touching the caret, else the
  caret line's; an action without its edit is resolved, its edit applied, then
  its command run (edits the server sends meanwhile are undone under its
  title). Core: `parseWorkspaceEdit` (changes and documentChanges merged;
  overlaps and file operations refused whole), `offsetEdits`,
  `groupReferences`, `parseCodeActions`, `diagnosticsInRange` (as sent,
  `Diagnostic::raw`), `onApplyEdit`.
- **The workspace edit applier** (`applyWorkspaceEdit:actionName:failure:`,
  `applyEditsToPaths:`) changes every file or none: read and check each
  (editor, `bufferText`, or disk; positions checked against what the server was
  sent, `Client::textFor`; a dirty tab only if each range holds the disk's
  text), open each and check it shows as that text, then edit each as one
  multi-range replacement (one undo step per file). A refusal after edits puts
  them back with `undoNestedGroup` (a key press's own group left alone) and
  closes tabs it opened (`closeFile`). Nothing is saved. `_applyingEdit` keeps
  completion, signature help and the rename box out.

## Markdown

- **Parser** (rewritten 2026-09-29 to CommonMark and GitHub's rules, after the
  old one italicized `max_len` and `2 * 3 * 4`): `analyze()` reads lines into
  blocks and classifies each (`MdLine`); the inline pass reads a whole block,
  so emphasis spans lines, with the delimiter rules (underscores inside a word
  never emphasise; stars do, as on GitHub). Only a few HTML tags mean anything;
  others are dropped, their text kept. Between blocks comes exactly one `"\n"`
  run with `gap` set, however many blank lines. Brackets are matched once per
  block with a stack and nesting stops at 8, so bad input stays linear.
  `MarkdownEdit` asks `MarkdownParser::lines` rather than classifying itself,
  so the preview and a double-click agree. Tests: `md:*`,
  `tests/markdown/corpus.md`.
- **The Mac's page** (+MarkdownRender): a paragraph's style comes from its
  first run; quotes and code blocks are `NSTextBlock`s, one object carried
  across a block's paragraphs so its bar or box is drawn once; h1, h2 and rules
  are bordered text blocks; list text at 24 points a level with the marker
  right-aligned on a tab; a hard break is U+2028; gaps are 12 points. Fenced
  code naming a language (`MdRun::lang`,
  `MarkdownParser::extensionForLanguage`) is colored by `MarkdownCode.mm` after
  the page is built. Tables: runs carry table, row, column and alignment, and
  the Mac builds an `NSTextTable` (`appendMarkdownTable:`); a table holding
  typeset formulas gets `MCTableColumnWidths` (AppKit's table layout ignores
  attachment widths). Cells are padded by display width, never bytes.
- Every run carries its source `line` (`kMarkdownSourceLine`): Shift+Cmd+V
  opens the preview at the caret and the source where a scrolled preview is.
  Links (`openMarkdownLink:`): `#heading` by GitHub's anchor
  (`MarkdownParser::anchor`, as on Linux and Android), web addresses in the browser, paths relative to the
  file through `openTerminalLink:` (`#L12` works).
- **Editing from the preview**: a double-click (`onPreviewDoubleClick`) reads
  the line (and `kMarkdownTableCell`), `MarkdownEdit::blockAt` finds the block
  (heading text, list item with continuations, paragraph or quote, code between
  fences, a table cell with newlines and pipes escaped back, a `$$` block
  whole); `applyMarkdownSource:` splices, marks dirty, registers undo,
  re-renders at the same scroll, and drops an edit if the buffer changed
  meanwhile.
- **Pictures**: `MCMarkdownImage` attachments with `MCAnimatedImageCell` (sized
  to the line; a GIF's timer runs only while on screen). Traps: the TextKit 1
  editor never asks for a TextKit 2 view provider (1.4.1 shipped still GIFs);
  `attachmentCell` set in init reads back nil, so it is an override. **Web
  pictures** (`MCWebImageLoader`): https only (redirects too), one ephemeral
  NSURLSession (no cookies, credentials or cache; 15 s idle, 30 s total;
  `User-Agent: MiniCode/<version>`), `data:image/` decoded in place,
  `markdown.web-images = false` off. The render never waits: alt text stands in
  (`kMarkdownWebImage`), the address joins `_awaitedWebImages`, and the
  notification queues one re-render 0.1 s later through
  `renderMarkdownKeepingPlace` (the top character kept by subtracting what
  arrived above). Limits: 20 MB, 50 megapixels; an NSCache of bytes, not images
  (the animated cell sets frames on its own image); failures remembered 30 s;
  nothing on disk. NSImage draws SVG; an SVG's `<image href>` is never fetched.
- **Task lists** (all ports): `MdRun::task`; the Mac's box is an `MCTaskBox`
  with `NSLinkAttributeName` `minicode-task:<line>` (`clickedOnCell:` never
  fired), applied as a preview edit, never saved by itself (the user ruled out
  autosaving preview edits); a double-click's second click on a box is
  swallowed. Done text takes `markdown.done`. Toggle Task is Option+Cmd+L
  (Ctrl+L, leader L); TODOs Shift+Cmd+D (Ctrl+Shift+L, leader W). "[ ] item"
  without a dash is not a task (GitHub, Obsidian, VS Code agree), and the task
  key adds only the dash. The TODOs window is Find in Folder's in a mode; a tag
  counts after a comment opener or first on a line with a colon, and in
  Markdown only after `<!--`. Known: a numbered task shows its box but not its
  number.
- Empty Markdown and LaTeX files open in the source on every port.

## Math in the Markdown preview (Mac)

Typeset by tectonic; elsewhere (Linux, Android, no tectonic, `markdown.math =
false`) a formula shows as its TeX in the code style.

- **Parser**: read before other inline rules. Inline `$...$` as pandoc (opener
  not before a space; closer not after a space nor before a digit, so "$5 and
  $10" stays text); a backslash pair and a code span are skipped inside.
  `$$...$$` inline is display, `\(...\)` inline. Display blocks run from a line
  starting `$$` or `\[` to one ending `$$` or `\]` without a blank line,
  interrupting a paragraph; plus ```` ```math ````. `\[...\]` counts only as a
  block (inline it is an escaped bracket). Memos keep unclosed openers linear.
  `tests/markdown/math.md` has every case.
- **The batch** (`MathTex`): each formula in `\setbox\mcbox\hbox{$...$}` (never
  a macro argument, so TeX recovers from a stray `\par`), shipped by `\mcship`
  as a page of its box plus 1 pt; the log's `MC:BEGIN:k` and
  `MC:k:page:wd:ht:dp` give pages, boxes and errors; the output routine drops
  other pages. `check` refuses what could derail later formulas (unbalanced
  braces, a stray `$`, `\input`, `\end{document}`, `\write`); `prepare`
  rewrites MathJax-only forms (`\mathbb{1}` to dsfont's `\mathds`, align and
  friends inline, `\tag`, Unicode symbols); the preamble and macros are in
  MathTex.cpp. **Bump `MathTex::kVersion`** when any of it changes: it is in
  every cache key.
- **The run** (`MarkdownMath.mm`): requests gather 50 ms into one run on a
  serial queue; `-Z continue-on-errors --untrusted --print --keep-logs --reruns
  0` (`--untrusted` keeps shell escape off). A bad formula fails alone with
  TeX's complaint as its tooltip; a page must be the size the log gave; a run
  with no PDF fails the formula TeX was in and reruns the rest. A watchdog
  kills tectonic after 15 s without output once in the formulas (the last
  `MC:BEGIN` names the culprit), 60 s before (downloads). A run reaching no
  formula fails them for 60 s with the reason at the page's top (`firstError`
  reads tectonic's panic when the bundle is out of reach). Failures stay in
  memory.
- **Cache**: whole PDFs under `~/Library/Caches/MiniCode/math`
  (`$MINICODE_MATH_CACHE`) with a `.idx`; formulas of a run share fonts (a PDF
  each was far bigger). Key: SHA-256 of `kVersion`, display or inline, and the
  TeX (pages stay vector, so no size). Past 50 MB, down to 40 MB.
- **Drawing** (`MCMathAttachment`): vectors filled with the text color in
  source-in mode, so a formula takes the color around it; the frame stretched
  to the font's ascender and descender (an attachment-only line sat high) with
  margins drawn past it; a formula wider than a line scaled to the whole line's
  width, not what is left of it. Trap: an attachment glyph's location is the
  bottom of its frame, not the baseline.
- **Never blocking**: the cache's formulas show at once, the rest as TeX
  (`kMarkdownMath`), re-rendered on `MCMathTypesetNotification` keeping the
  place. While formulas are out the top line says "Typesetting N formulas…",
  and that a first run downloads LaTeX files when tectonic's cache is empty (a
  first run once sat 44 s with only TeX showing). Without tectonic, a muted
  line with a Download tectonic link (`MCDownloadTectonic`, shared with the
  LaTeX preview).
- Known gaps: copying a typeset formula copies an attachment character;
  `\color` inside is drawn in the text's color.

### Mermaid in the preview (Mac)

```mermaid blocks go to the user's mermaid-cli (`mmdc`, MCFindProgram, or
`$MINICODE_MMDC`), one run at a time off the main thread, each its own
process group with 90 s (a run starts a headless browser, about 2.5 s):
a transparent PNG at scale 2 in the preview's theme (dark or default),
kept in memory and `~/Library/Caches/MiniCode/mermaid` (`$MINICODE_MERMAID_CACHE`;
50 MB). Chosen over a renderer of our own (flowchart and sequence alone are
a layout engine) and over mermaid.js (third-party code). PNG, not PDF or
SVG: mmdc 12 makes PDFs fit by default and refuses the older `--pdfFit`,
draws PDF backgrounds white, and its SVG labels are HTML in foreignObject,
which NSImage leaves out. Until it arrives, and without mmdc or with
`markdown.mermaid = false`, the block is code (a failure's complaint, from
`MermaidDiagram::errorSummary`, its tooltip); the code carries
`MCMarkdownMermaidAttribute` so `renderMarkdownKeepingPlace` keeps the
place. Core: `MermaidDiagram.{h,cpp}` (`mermaid:*`); Mac: `MarkdownMermaid.mm`,
a hook in `renderMarkdown:`. `make guitest SUITES=mermaid` uses a stand-in
mmdc; `MC_GUI_REAL_MMDC=<an mmdc>` adds a real diagram and a picture.

## LaTeX preview (read before touching it)

- **tectonic**: `MINICODE_TECTONIC`, then `~/Library/Application
  Support/MiniCode/bin` (0.17.0 is there; its cache,
  `~/Library/Caches/TectonicProject.Tectonic`, has text fonts but no math
  fonts), then PATH and Homebrew; missing, the panel offers the pinned
  download.
- **The buffer is typeset from a hidden sibling** `.<name>.minicode.tex` in the
  document's folder, so relative paths resolve and the user's file is never
  written; deleted when tectonic exits; output in
  `NSTemporaryDirectory()/MiniCode-latex`.
- Compiles are debounced (0.6 s) and generation-counted; a run has 5 minutes,
  then SIGTERM and SIGKILL, with a log line (a looping document spun a core and
  queued every later edit). Scroll and zoom are restored by page index and
  point.
- **Unknown commands are looked through, not skipped**: `LatexDoc` skips only
  known machinery (`\label`, `\includegraphics`, `\setlength`, ...) and enters
  the first braced argument `looksLikeProse` accepts (not `{0.5em}`, `{l}`,
  `{sec:intro}`, `{GDM.png}` or URLs; yes `{2014 - 2019}`). A URL disqualifies
  only a group that is one unbroken token; a group holding commands is always
  entered. Letters spelled as commands (`\ss`, `\o`, `\LaTeX`, `\c{c}`,
  `\'{e}`) stay in the run (`kTextSymbols`, `kLetterAccents`) and `displayText`
  reads them; `\\` ends a run like `&`; `\url{...}` is a field.
- **A click that matches nothing refuses** (null rather than the nearest span
  whenever there was a real word); text TeX makes up is refused. **A single
  letter or digit is always refused** (page, section, list and footnote numbers
  fell through to the nearest span), accepted by the user at the cost of
  one-letter words, except a Han, kana or Hangul character whose surroundings
  agree with the source.
- **Click to source** (`MCLatexSpanAtPoint`, which the sweep calls): lines from
  `SyncTexIndex::textHitsAtPoint` (a box carries the line its paragraph closed
  on, glue and kerns each word's line: right of the click first, then left,
  then boxes; glue on a box's end edge goes left first); the word from
  `selectionForWordAtPoint:` plus 40 characters each side, rejoining
  hyphenation (`MCJoinHyphenation`); `LatexDoc::matchKey` on both sides
  (letters and digits, lowercase, ligatures split, accents dropped; three
  characters or fewer must be whole words, except in math); choice by context,
  a later candidate beating the nearest only by 4 characters (a tabular reads
  back by column). Spans cover all their lines; `\title`/`\author`/`\date`
  count at `\maketitle`; a word split by a font change matches a join with
  balanced braces; a footnote mark glued to a word is retried without digits.
- **Measure before changing the matcher**: `tests/latex/sweep.sh doc.tex [-v]`
  double-clicks every word of every page and reports found, refused, wrong and
  not placed (scratch `TECTONIC_CACHE_DIR`). The bar is 0 wrong; the user's
  resume is at 100% and `tests/latex/torture.tex` at 98.7% (the rest rightly
  refused). Trap: `characterBoundsAtIndex:` drifts from `page.string`'s indices
  down a page; a one-character `selectionForRange:`'s bounds do not.
- **Edits are byte splices** of the span's own LaTeX (nothing escaped or
  re-serialized), marked dirty, saved only by Cmd+S, undone through
  `LatexView`'s own snapshot stack (50) via `undo:`/`redo:`.
- **Export PDF** (Shift+Cmd+S): `pdfForPath:source:completion:` hands back
  tectonic's bytes (`_pdfData`, `_pdfSrc`), typesetting first if they are not
  the buffer's. Trap: a synthetic `NSEvent` for Shift+Cmd+S with "s" matched
  Save and wrote the file, with "S" nothing; `CGEventCreateKeyboardEvent`
  behaves like hardware.
- tectonic's bundle is older than Overleaf's: a missing package (`twemojis`)
  goes in the document's folder, and since tectonic discards docstrip's output,
  a script keeping a `.dtx`'s code lines made the `.sty`. Spacing differs from
  Overleaf by design (XeTeX; compare with Overleaf set to XeLaTeX).
- Headless: drive `openEditorForPage:point:`, `startAddItem:`, `commitEdit:`
  with points from `[PDFDocument findString:]`, on scratch `MINICODE_TECTONIC`
  and `TECTONIC_CACHE_DIR`.
- Linux (`Latex.cpp`, `PageWords.cpp`, `LatexClick.cpp`, poppler) differs on
  purpose (`linux/HANDOFF.md` item 7); `tests/latex/sweep-linux.sh` found the
  single-character problem.

## Viewers in the editor's slot (Mac)

- `openFileAtPath:` calls `resetViewMode` (mode flags cleared, text view
  read-only) and routes by kind before reading text; `refreshDisplay` always
  relays out (it once left an image on screen). Viewers save nothing
  (`setPlainMessage:@""`, `saveCurrentFile:` returns); `checkExternalChange`
  reloads keeping the place.
- `FileSniff::classify` (first 8 KB and name): a NUL byte means binary, never
  text; SQLite's header means a database; Office, iWork, OpenDocument, ePub,
  archives and fonts go to Quick Look, and so does a file that is not UTF-8;
  .csv, .tsv, .json and .ipynb get viewers. `MCFileViewers` per window; a tab's
  `viewerKind` is without text (Quick Look, big CSV, SQLite), over text (the
  CSV table, whose edits go through `setViewerText:actionName:reshow:` so
  dirty, undo, backups and saving work as for text) or beside text (the JSON
  outline, `besideTextShown`). Shift+Cmd+V flips them (`canTogglePreview`).
- **Images** (not SVG, which is source): `NSImageView`, scaled down never up,
  `animates`, size set to pixels (a 144 dpi screenshot drew at half size); one
  that fails to decode falls through to the text path's "Cannot display".
  **PDFs**: `PDFView`, reload keeping page, point and scale.
- **Video and audio** (`+isMediaPath:`): `AVPlayerView`, paused on the first
  frame; `stopMedia` (from `resetViewMode`, window close, dealloc) drops the
  player so sound stops; size (rotation applied) and length arrive by KVO;
  Shift+Cmd+Space plays from any pane (Cmd+Return is the commit box's). `.ts`
  and `.mts` are TypeScript; `asset.playable` is checked first; WebM, mkv, ogv,
  wmv and flv get "Cannot display".
- **Quick Look**: `MCQuickLookSlot` (`QLPreviewView`) in a tab;
  `MCQuickLookPanel` for Space in the tree, where `ClickOutline` is the panel's
  controller and keeps the keyboard. It follows arrows and clicks; Space closes
  it only for the tree that drives it (another window's tree, or one whose
  control ended, gets its selection shown); a space within a second of a
  typed letter is type-select's.
- **CSV and TSV** (`Csv`, `CsvTable.mm`): rows keep their bytes and endings,
  fields their bytes and quoting, so an unedited file round-trips and one cell
  edit changes one line; BOM and a missing final newline kept; a touched row
  of one empty field is written `""`; delimiter guessed (it must split most
  lines as it splits the first). Over 50 MB read-only, 100,000 rows from
  disk. **One table view serves every CSV tab**: a cell being typed into is
  committed in `stashActiveTab` and the toggle (`commitEditing`), and
  dropped before `showText:` replaces the document, or NSTableView commits
  it into the next file at the old row. Undo steps keep only the changed
  splice (`registerViewerUndoFrom:`); going back from the text to the table
  turns its NSTextView typing steps (target: the text storage) into one
  "Typing" step from `MCTab.textSessionStart`, since the table empties the
  text view they were recorded against.
- **JSON outline**: beside the text; a node selects its text with the keyboard
  kept in the outline, the caret selects its node; re-parsed 0.3 s after
  typing, the last good outline kept under an error line, dimmed and
  selecting nothing (`stale`: its offsets are off). `.jsonc`, tsconfig*.json,
  `.vscode/*.json` and the like allow comments (`JsonOutline::allowsComments`).
- **Notebooks** (`Notebook`): nbformat 4 (and 3's cells) to Markdown, rendered
  read-only (`showsRenderedMarkdown`); outputs as plain blocks, pictures as
  data: images (SVG too, base64'd), HTML as text, an output past 5,000 lines
  or 512 KB cut to its first and last 1,000 (`shortenOutput`). Nothing runs.
- **SQLite** (`SqliteView.mm`): `SQLITE_OPEN_READONLY` with a `mode=ro` URI,
  query_only and the defensive flag; a writing statement
  (`sqlite3_stmt_readonly`) refused before it runs, and an authorizer refuses
  BEGIN, SAVEPOINT, ATTACH and setting pragmas (a BEGIN held a lock other
  writers waited on); only the first statement; a progress handler stops a
  query at 5 s; one background queue, opening included; 1,000 rows. A WAL
  file with no `-wal` beside it opens with `immutable=1` (read-only cannot
  make the WAL files); a failure is shown in the tab. A reopen keeps the
  table or query on show. NULL is `NSNull` in the rows (`isNullAtRow:`).
- **Diffs and pages**: git diffs and commits in `diffScroll`; the side-by-side
  diff is `CompareView` (`showComparison:preview:`, `MCComparison`, panes
  scrolling together); an `EditorPage` (`showPage:`) is a read-only view in a
  tab, reused by key (Evidence, a worktree's changes).

## The terminal

`terminal.mode` (read once per terminal in `initWithDirectory:`): **grid**, the
default since 1.5.5, runs zsh's own line editor in a real terminal grid;
**line**, kept as a fallback, is a log with a line input and a grid swapped in
for full-screen programs. Both can be open at once.

- **The pty** (`MCPty` in Terminal.mm) owns the master and does every read,
  write and teardown on the terminal's serial I/O queue. The master is
  non-blocking; bytes it will not take wait in a buffer while a write source
  waits for room; `sendBytes:` only hands a copy over (a blocking write on the
  main thread froze the app on a paste of over about 1 KB into a busy program,
  and with flow control could deadlock). Teardown on the queue cancels both
  sources, resumes a suspended one (a source cancelled while suspended never
  finishes cancelling), drops unwritten bytes; then the main thread lets go of
  `_master` before it is closed, and the shell is reaped by polling `waitpid`,
  SIGKILL 2 s after the hang-up. The child is prepared before `fork` (only
  async-signal-safe calls after). Traps: paste only once the tty is raw; check
  reaping with `kill(pid, 0) == ESRCH`, not `waitpid`.
- **Output flow**: past 1 MB handed to the main thread the reader
  (`MCPtyReader`) suspends its source (the program blocks) until under 256 KB;
  the main thread parses in 16 KB pieces for about 12 ms a run-loop turn
  (`drainBacklog`, common modes; 36 ms when a turn ends mid-frame of
  synchronized output). A shell's last output is parsed before "[shell
  exited]".
- **Grid mode's shell**: `zsh -l -i`, `ZDOTDIR` at
  `NSTemporaryDirectory()/MiniCode-zsh-grid/.zshenv`, which hands ZDOTDIR back
  and at the first precmd (after .zshrc, so last in line) installs hooks that
  only print: precmd `133;D;status` (if preexec ran), OSC 7, `133;A`; a
  `zle-line-init` hook (`add-zle-hook-widget`) `133;B`; preexec
  `633;E;<command, control characters blanked>` then `133;C`. PS1, PS2, RPS1,
  PROMPT_SP and tty modes are untouched; HIST_IGNORE_SPACE keeps
  `setDirectory:`'s ` cd` out of history; `TERM=xterm-256color`.
  Every mark ends in `;mc=<nonce>` (`__minicode_mark`): 128 random bits per
  shell in `MINICODE_NONCE`, which the .zshenv reads and unsets, and
  `TerminalStream::setMarkNonce` drops marks without it (2026-11-06; any
  program could print them: a remote shell with iTerm2's integration made
  Run File type into ssh). OSC 7 percent-encodes `%`, `;` and control
  characters. B only in ZLE's `start` and `cont` contexts (not vared). The
  precmd puts our line-init hook back at every prompt (`-d` then add, since
  add-zle-hook-widget keeps a plugin's widget only when ours is not still on
  its list): a plugin taking zle-line-init over in its first precmd (as
  zsh-vi-mode does) stopped B for good.
- **exec**: preexec looks for `exec` as a command word (first, or after `; &&
  || & |`, `then`, `do`, `else`, `{`, `!`, and past `builtin`, `command`,
  `noglob`) and for that command exports ZDOTDIR as the integration folder (the
  user's in `MINICODE_USER_ZDOTDIR`) with `MINICODE_EXEC=1`, and the nonce
  when the program is a shell (a bash keeps it in its environment, so a zsh
  started from it marks again; `exec claude` gets none), so an exec'd zsh
  loads the integration and reports the exec finished (133;D;0); the next
  precmd restores both if the exec failed. Line mode also `unsetopt ZLE` there.
  Exec into another shell (bash, sh, dash, fish, ksh, tcsh, csh, nu, elvish,
  xonsh, pwsh, by `proc_name`, polled twice a second while a command runs) sets
  `_integrated` NO: not busy, title cleared, the finished notification posted,
  waiting input typed, and from then busy only while another program holds the
  foreground. A later zsh mark (`_integrationFile`) turns it back on. `exec
  claude` still counts as running.
- **Events** (`TerminalStream` reads every byte for them): 133;B sets
  `_zleReady`; `CommandStart` carries 633;E's text as `_runningCommand`
  (titles, `runningCommands`, close questions); OSC 0/2 `Title` names the tab
  only between 133;C and the next prompt (an empty title, the prompt or exit
  restore the rule; `minicode term --title` wins). `busy`, `atPrompt`,
  `gridMode` (a program has the alternate screen), `lineMode` and
  `MCTerminalCommandDidFinishNotification` work in both modes.
- **Typing for the user** (`typeLine:run:`): first a private key, `ESC [9999~`,
  bound in emacs, viins and vicmd to `zle push-input` (`__minicode_push`), so a
  half-typed line returns at the next prompt; `runCommand:` adds Return;
  `typeCommand:` pastes in brackets when ZLE asked, never pressing Return;
  outside brackets control characters are dropped. Both go through
  `canTypeNow`: `tcgetpgrp(master)` must be the shell, and `_zleReady` (B),
  not A, since bytes sent between precmd and ZLE start echo as `^[[9999~`;
  with no B, a prompt 0.3 s old whose tty is raw counts. Typing uses the
  prompt up (`_zleReady`, `_atPrompt` cleared), so a second command waits.
  What cannot go in yet is queued (`_pendingCommand`, `_pendingTyped`) and
  `flushPendingInput` tries again on every event and every 0.1 s. `busy` is
  also YES while another program holds the foreground. Private keys sent only while ZLE reads: `ESC[9998~`
  beginning-of-line, 9997 end-of-line, 9996 backward-kill-line, 9995
  clear-screen (Cmd+Left, Cmd+Right, Cmd+Delete, Cmd+K); elsewhere on the main
  screen Cmd+arrows and Cmd+Delete send Ctrl+A, Ctrl+E, Ctrl+U (zsh's emacs
  Ctrl+U kills the whole line and vi mode lacks Ctrl+A/E, hence the private
  keys); full-screen programs keep their keys.
- **Scrollback** (`TerminalScreen`): lines leaving the top on a line feed at
  the bottom, main screen, scroll region at row 0 (as xterm); RI, IL/DL, SU/SD
  never save; `ED 3` clears it, `ED 2` and RIS keep it. Stored packed
  (`HistLine`: text in one string, a style run per change, a per-cell table
  only for lines with anything but one byte of width 1 per cell, offset << 2 |
  width, offsets under 1 GiB; 2 to 6 MB per 10,000 lines where cells took 30
  to 59 MB). A cell keeps at most `kTermMaxCellBytes` (32) of character and
  marks, in both models: 4 MB of U+0301 made one cell every frame copied,
  and past 16 MiB on a line the old 24-bit offsets wrapped and find read out
  of bounds. `lineCell` returns by value;
  `historyBytes()`. `terminal.scrollback` (10000, up to 100000) sets it live; 0
  unless the host sets it, so Android has none.
- **Reflow**: a row's `wrap` byte (0 end, 1 wrapped, 2 wrapped with a wide
  character's blank) is kept in the scrollback. `resize(cols, rows, &keep)`
  reflows main screen and scrollback (`reflowMain`): wrapped rows rejoin and
  wrap again, wide characters move whole, the cursor and saved cursor stay on
  their characters, places in `keep` (the view's top line, a selection, a find
  match) follow their text, blank rows below the text go first, lines come back
  down when there is room, and `historyBase() - historySize()` is unchanged.
  Lines below the cursor that a shorter screen cannot hold go to `below_` and
  come back first when it grows (dropped once the main screen scrolls or is
  erased). At least 2 columns. The alternate screen is cut. Lines are moved, not copied (10,000 in about 1
  ms). Erasing ends wraps as xterm.js does; the wrap of zsh's PROMPT_SP after
  unfinished output (`promptSp_`, seen through the 133;A after it) is not soft.
  `textBetween`, `find` (`TermMatch` end line and column), links, words and
  `recentText` read across wraps. The view resizes through
  `resizeScreenToCols:rows:`.
- **Mouse and focus**: modes 9, 1000, 1002, 1003, encodings 1005, 1006, 1015,
  and focus (1004) are tracked, answered by DECRQM and encoded by `encodeMouse`
  and `encodeFocus`. The view's `reportsMouse:` gate: the program asked, the
  view is at offset 0, no Shift (selection) or Cmd (links). The prompt's return
  or the shell's exit turns off modes a program left on, so a click never types
  into the prompt.
- **Find, clear, synchronized output**: `TerminalScreen::find` (scrollback then
  screen, ASCII/Latin-1/Greek/Cyrillic folding, packed lines in place, 10,000
  cap); `MCTerminalFindBar` (Cmd+F with a grid focused; matches by line number;
  re-search 0.2 s after output; the bar takes Cmd+F and Cmd+G in
  `performKeyEquivalent:`, since its field editor would answer them). Cmd+K
  (`clearTerminal:`): at zsh's prompt `clearAll` and ZLE's clear-screen (a
  multi-line prompt comes back whole); while a program runs
  `clearToCursorLine`, nothing sent; line mode empties the log. Mode 2026: no
  drawing while a frame is open, a draw at its end or after 150 ms, the mode
  ignored after a frame that never ended; the decision is deferred with a
  zero-delay perform, because a redraw requested after one piece cannot be
  withdrawn once the layer has it (`setNeedsDisplay:NO` did not). DECRQM
  answers 1 set, 2 reset, 0 unknown.
- **The view** (`TerminalGridView`): scroll offset, selection and hovered link
  are line numbers from `historyBase()`, so they stay on their text while
  output arrives; typing, pasting and app-typed commands snap back; on the
  alternate screen the wheel sends arrows unless the program takes the mouse. A
  double click's word is anything but blanks, brackets, quotes and `|;,` (so
  paths and URLs). Typing goes through `interpretKeyEvents:`, so dead keys and
  input methods compose; marked text wraps onto the rows below
  (`drawMarkedTextAtRow:`), since dictation composes a whole sentence.
- **Keys**: Ctrl with a letter, symbol or Space is claimed in the grid's
  `performKeyEquivalent:` (the window asks views before the menu), so
  Ctrl+Space reaches zsh; Ctrl+Tab, Ctrl+Return and Ctrl+digits stay the app's.
  Ctrl with Shift or Option goes to the app only when the menu item with that
  key can act against the grid's responder chain (`MCMenuItemForKey`, with and
  without Shift's effect), else to the program (Ctrl+Shift+K while Delete Line
  cannot act; Ctrl+Shift+Tab with one tab as ESC [ Z). Option is Meta
  (`terminal.option-as-meta`); Option+Left/Right send ESC b / ESC f.
  Shift+Return is ESC CR (Claude Code's new line). Shift+F10 is the context
  menu, not the program's.
- **Font**: `terminal.font-size` (12) plus the zoom; the grid remeasures its
  cell, the input line and log re-font (keeping weight and slant), the panel
  relays out and the pty gets the rows and columns that fit.
- **Line mode**: `zsh -l -i +Z`, `ZDOTDIR` at
  `NSTemporaryDirectory()/MiniCode-zsh` (hooks emit 133 A/C/D and OSC 7, blank
  `PS1`, `PROMPT_SP` unset); echo is off while zsh reads, so the master's ECHO
  flag shows hidden input. The last log line is "live" (replaced from
  `_liveStart`), so panel-written lines go through `ensureNewline`
  (`breakLine()`) or the next update overwrites them. Each chunk feeds both
  `TerminalStream` and `TerminalScreen`; the grid shows while `altScreen()` or
  `_stickyGrid` (out of ICANON for two 0.1 s polls, which stops `less -F`
  flashing it). Both modes size the pty from the grid's cell metrics, so
  switching sends no SIGWINCH. `TerminalScreen` must stay copyable (a `Grid*`
  into itself once made copies draw the original). The log is capped
  (`trimLog`); the input line draws no background (a fixed #232323 ignored the
  theme and doubled alpha).
- **Several terminals** (`MCTerminalPanel`; `terminal` is a getter for the
  active one, so older code and demo KVC work): tabs on the editor's
  `MCTabStrip`, panes side by side; hidden tabs keep their frames and run; the
  active terminal follows the keyboard through KVO on `firstResponder` with a
  weak window reference removed in `viewWillMoveToWindow:` (a strong one kept
  closed windows and their shells alive). Ctrl+Shift+` new tab (keyCode 50:
  Shift makes it ~), Cmd+\ split (refused under 20 columns a pane,
  `kMinSplitColumns`), Option+Cmd+] and [, Ctrl+Shift+W. Every pane with room
  for two cells gets its true pty size.
- **Closing** (`TerminalView close`, `stopShell`, `_closed`): a terminal with
  `runningCommands` asks: a command since 133;C, a foreground process group
  other than the shell's (`tcgetpgrp`, `proc_name`), a `runCommand:` still
  waiting, and background jobs (processes with the pty as controlling tty,
  `KERN_PROC_TTY`, in other groups, named from `KERN_PROCARGS2`;
  `KERN_PROC_SESSION` answers nothing on macOS). `&!` jobs count too. Window
  close and quit ask once for all; idle shells close unasked.
- **Run File** (Shift+Cmd+Return): `Settings::runTemplate` filled by
  `RunFile::expand` (`shellQuote`, or `quoteInside` when the template quotes
  the placeholder); `{python}` is the LSP session's interpreter. It saves
  first, goes through `runCommand:` (so into history), never types into a busy
  terminal (a new tab instead), and leaves the keyboard alone.
- **Links** (Cmd+click; `TermLinks`): resolved against the shell's cwd (OSC 7),
  the window folder, its worktrees and the sessions' scratchpads
  (`AgentIndex::resolveTarget`): one match opens, several get a picker, none
  names the roots tried; paths resolved so /private/tmp matches a /tmp root.
  The log checks `characterIndexForPoint:` against the character's own rect (it
  answers past a line's end); a flagsChanged monitor catches Cmd pressed
  without moving. Not done: OSC 8 hyperlinks.
- Testing: a scratch `HOME` and `ZDOTDIR`, or commands land in the user's
  `~/.zsh_history`; an app-typed command snaps a scrolled view, so test "stays
  on its text" with output from a command already running.

## Git panel (all three ports)

Ctrl+Shift+G swaps the tree for `MCGitPanel` (`replaceSubview:with:`, same
frame). All three panels draw from the core's `src/GitModel.{h,cpp}`
(`GitUi`: rows, the selection after a refresh, every argument vector, the
summary line, failure text, the graph's build, key, colors and tooltips, and
diff and commit text as styled runs in characters, which `MCGitStyledText`
maps to UTF-16; tested in `tests/gitmodel_tests.inc`, `git-ui:*`). It was
the GTK port's until 2026-11-06, when the Mac's `GitPanel.mm` still had its
own copy of every rule. The Mac passes its own date format (`DateFormat`,
the user's locale). A rule or wording changes there, once. The GTK port
(`linux/src/GitPanel`) and Android (`GitPanel.kt`, `git_jni.cpp`, git in
Termux through one bash loop) follow the same rules and wording (`linux/HANDOFF.md` item 10; `android/README.md`, "Source
control"). Logging: `G_MESSAGES_DEBUG=minicode-git` on Linux, `adb shell
setprop log.tag.MiniCodeGit DEBUG` on Android.

- **What runs**: status, diff, add, restore --staged, commit -m, rm --cached
  --force before the first commit, and for the graph for-each-ref, log,
  rev-list, show, rev-parse. Never a shell string: `MCGitRunSync`
  `posix_spawn`s an argument vector in the top level. Paths after `--`; add,
  restore and rm get `--literal-pathspecs` (`sp*ecial.txt` never stages
  `spXecial.txt`). Status `--porcelain=v2 --branch -z --untracked-files=all`;
  diffs `-c core.quotePath=false diff --no-color --no-ext-diff --src-prefix=a/
  --dst-prefix=b/` (a user's noprefix or diff tool would change the output),
  `--cached -M` for a staged rename, `--no-index -- /dev/null path` for
  untracked (status 1 means differs). `MCFindProgram(@"git")`, never
  /usr/bin/git's shim.
- **Environment**: `GIT_OPTIONAL_LOCKS=0` (a plain status rewrites .git/index,
  FSEvents fire, the panel refreshes: a loop), `GIT_TERMINAL_PROMPT=0`,
  `GIT_EDITOR=true`, stdin /dev/null, the wide PATH; stdout and stderr drained
  at once on their own queues.
- **Time limits** (`Git::timeoutFor`, one table for the Mac and GTK): 30 s for
  reads, 2 minutes for add, restore and rm (filters, index.lock), 10 for commit
  (hooks, signing). git leads its own process group, which gets SIGTERM at the
  limit and SIGKILL 2 s later; the panel shows "git did not answer within 30
  seconds: <command on one line, 160 characters at most>" (`timeoutMessage`),
  or `heldOutputMessage` when git exited but something it started holds its
  pipes. git is reaped on its own thread; a timed-out for-each-ref skips the
  log. GTK: `runGitWithin` kills `-pid`, waiting with GSubprocess async calls
  on the git thread's own main context; that thread stays single on purpose (an
  add and its status must not pass).
- **Threads**: a serial queue per panel; results dropped if the root changed or
  a newer diff was asked. `refresh` coalesces 50 ms, only while in a window (on
  show, after actions, on window key, on the tree's FSEvents).
- **Errors**: git's stderr under Commit (6 lines, all in the tooltip); empty
  stderr uses stdout's last line (a commit with nothing staged explains itself
  there). Keys live in `MCGitTable keyDown:` and the message box
  (`MCCommitTextView`, Cmd+Return by keyCode 36/76); the diff takes the
  editor's slot (`showDiffNamed:`, `MCGitDiffText`), cut past 4 MB.
- **The graph**: `for-each-ref`; `-c log.showSignature=false log -z
  --topo-order --max-count=N HEAD @{upstream} --` (`--branches --remotes` for
  All branches; no `@{upstream}` when there is none, it is gone or HEAD is
  detached); `rev-list --left-right HEAD...@{upstream}` only when ahead or
  behind; a commit's `show ... -M --diff-merges=first-parent --stat --patch` (a
  merge's combined diff is usually empty). The status applies first, the graph
  after on the same queue, keyed on HEAD, branch, upstream, limit, checkbox and
  the for-each-ref output, so most refreshes run for-each-ref only. Show more
  adds 200. Lanes (`Git::layoutGraph`): a commit takes the leftmost lane
  waiting for it and ends the others waiting there; none waiting is a new tip
  at the right; the first parent continues the lane, others join a waiting lane
  or the first free one; free lanes close up, bending left in the same row. 12
  pt lanes, narrower past 40% of the panel.
- **Tool guard trap**: in a worktree-isolated agent session, git commands in
  compound or unusual forms are refused (even a filename containing "git"); use
  plain `cd x && git ...` lines or a script file.

## The file tree (Mac)

- `ClickOutline mouseDown:` runs the native tracking first and opens the file
  on release only if no drag began (`dragBegan`); NSOutlineView's selection
  notifications did not fire reliably. Drag and drop moves rows into folders
  (`validateDrop:` retargets onto a folder; into itself, a descendant or where
  it is refused), copies files from Finder in and rows out; a clash is reported
  after the drop (Finder waits on a modal during it). `pathMovedFrom:to:` moves
  tabs, the LaTeX path and a preview's relative pictures. F2 renames,
  Cmd+Delete trashes (the registry's `Tree` commands, run by the key monitor),
  Space previews with Quick Look.
- **The Worktrees node** (first, shown with or without hidden files, not again
  inside .claude): each worktree named for its agent with its ahead count and
  changes (git off the main thread); right click Compare with Main (each
  changed file side by side with main at the merge base, so main's later work
  never shows as the agent deleting it; `AgentView::compareBase`) and Open in
  New Window. A window on `/` or the home folder indexes no sessions
  (`AgentView::indexesFolder`): every transcript on the Mac is inside them.
- **Columns** (`MCTreeColumns` bitmask, `toggleTreeColumn:`): size, saved and
  created (`st_birthtimespec`) on their own keys, the rightmost dropped first
  on a narrow row; switching one on widens a narrow sidebar
  (`MCTreeCellWidthFor`, up to 40%). Trap: NSTableCellView moves its textField
  to the cell's width without a layout pass, so `MCTreeCell` places its fields
  again in `viewWillDraw`.
- **Folder totals** (`FolderSizes::measure`, `MCFolderSizer`, last asked
  first): every regular file below, hidden ones included (as Finder and du
  count), never through a symlink or into another device; subfolders cached by
  stamp (dev, inode, own mtime); FSEvents paths (`/private/tmp` mapped back to
  `/tmp` through realpath) invalidate a folder and its parents, and a store is
  refused if an invalidation came after the count began. 200,000 entries or 2
  s, then "≥ X".
- **Sort By**: name ascending with folders first is exactly the old order
  (`caseInsensitiveCompare:`, so nothing moves for anyone who never sorts); a
  new key starts in its own direction (dates newest, sizes largest). While a
  folder lacks a total, folders stay in name order, then the list re-sorts once
  (`folderTotalsArrived:`, through `reloadTreeKeepingState:`). Remembered per
  folder (`MCTreeSort`). `FileItem` keeps `children` in name order and `shown`
  in the window's.
- **The context menu by keyboard**: Shift+F10, a hidden Ctrl+Return (otherwise
  a text view's U+2028) and the PC Menu key (keyCode 0x6E).
  `MCShowContextMenuInWindow` builds a right mouse down just inside the
  selected row's bottom edge (so `clickedRow` is that row) or at the caret,
  asks `menuForEvent:` (restoring a text view's selection, which a right click
  moves), and pops it with `popUpContextMenu:withEvent:forView:` on the next
  run-loop turn.

## The inline agent, Request a Change and restarting (Mac)

- **The inline agent** (Cmd+I, `AgentPanel.mm`; hover is Option+Cmd+I). What
  has the keyboard decides: a text file in the editor or a Markdown preview (on
  the source) is an edit; an idle terminal a command; anything else (tree, git
  panel, browser, LaTeX preview, picture, busy terminal, another window) a
  task. Cmd+Return in the bar makes any request a task. `MCAgentContext`
  carries blocks, so the bar knows nothing of the views. `MCAgentRun` runs
  `/bin/zsh -l -c 'exec <agent.command> -p --tools Read,Grep,Glob
  --no-session-persistence --output-format text'` in the folder, the prompt on
  stdin (written off the main thread: a big buffer outgrows a pipe), five
  minutes at most, exit 127 as not installed. The prompt
  (`InlineAgent::editPrompt`) asks for SEARCH/REPLACE blocks or an answer;
  `applyEdits` matches each SEARCH ever more loosely about whitespace, nearest
  the cursor, all or nothing, so typing elsewhere meanwhile does no harm. The
  edit goes in through `replaceSourceWithText:`, tinted; Return keeps; Esc
  restores the text from before the bar's first edit if the buffer is as the
  agent left it. Commands are typed, never run. Tasks write the prompt to
  `$TMPDIR/MiniCode-agent/` and run `cd <root> && <agent.command> "$(cat
  file)"` in a new terminal tab. Traps: `NSTextView string` is the live
  storage, so "the text before" must be a copy; the bar's place uses line
  fragments up to the caret only (the first Cmd+I once laid out the whole file:
  1.7 s); `firstRectForCharacterRange:` gave nothing usable offscreen.
- **Request a Change** (Shift+Cmd+K, `ChangePanel.mm`, an `MCChangeSession` per
  window): `MCChangeBar` across the top of the right pane, laid out first
  (`layoutChangeBarIn:`), styled as developer mode (an amber stripe, the
  sidebar tinted 13% amber, a DEV pill, the source folder) so it is not taken
  for an ordinary field; its box wraps and grows to 10 lines
  (`heightForWidth:`). Return sends, Shift or Option+Return adds a line, Esc
  (`complete:`) closes. The prompt (`ChangeRequest::prompt`: the request, then
  asking for `make`, CLAUDE.md, and no commit, push or release) goes to
  `$TMPDIR/MiniCode-change/request-<pid>-<n>.txt`, and `cd <source> &&
  <develop.command> "$(cat <file>)"` is typed into a new terminal tab (or
  Claude's previous idle one), focused; the whole prompt on the line had
  flooded it, the tab title and zsh's history. Source folder
  (`MCChangeSourceFolder`): `develop.source`, else the checkout the bundle was
  built in, else `MCChangeCloneFolder()` (`~/Library/Application
  Support/MiniCode/source`), cloned on first use by one typed line (`(git clone
  --branch v<version> <repo> <dir> || git clone <repo> <dir>) && cd <dir> &&
  claude ...`). "Source" means a Makefile and `src/EditorController.mm`. A 2 s
  timer watches `<source>/MiniCode.app/Contents/_CodeSignature/CodeResources`
  (codesign is `make app`'s last step, and that file is its target, so a
  `make` with nothing to do leaves it alone) and offers Restart once it is
  newer and stable. Prompt files whose writer has quit, or a day old, are removed as
  the next is written (`MCRemoveStalePromptFiles`, the inline agent's too).
- **Restart** (`AppDelegate restartIntoApp:resumingIn:source:command:`,
  `MCRestarting`): the quit questions titled "Restart minicode?", leaving
  Claude's terminal out; unsaved tabs come back from backups;
  `applicationWillTerminate` writes `MCRestart` and `MCLaunchAfterExit` spawns
  a detached `/bin/sh` that waits for our pid and runs `open -n
  <source>/MiniCode.app`; the new process takes a record under two minutes old
  (`reopenAfterRestart:`) and runs `develop.command --continue` in the
  resuming window (never the record's command, and only in a folder that
  looks like the source). After a restart the user runs the repo's build until the next `brew
  upgrade`. Restart minicode (minicode menu, the status bar's ↻,
  `restartMinicode:`; no key, Cmd+R is Refresh) uses
  `NSBundle.mainBundle.bundlePath` (beeps when not a .app).
- **The browser comes back**: the session entry's `browser` key (address plus
  `WKWebView.interactionState`, about 2.6 KB with history and scroll, macOS
  12+, left out past 2 MB; visibility; fraction), restored in
  `restoreBrowserFromSession` for a new window, never on Open Folder;
  `applicationWillTerminate` saves every window's session. Cookies persist in
  the default data store.
- `mc-dev` (the user's `~/Code/dotfiles/shell/aliases.sh`) runs the repo's
  build; launching a build registers it with Launch Services, so Finder may
  offer two minicodes (`lsregister -u` it).

## The control channel, notes and tours (Mac)

- Each minicode listens on `$TMPDIR/MiniCode/<pid>.sock` (0600 in a 0700
  folder; another uid refused by `getpeereid`; removed on exit; at startup
  sockets of dead pids, and of live pids where nothing listens, are removed).
  Use `getenv("TMPDIR")`: `NSTemporaryDirectory()` ignores it; without it,
  `confstr(_CS_DARWIN_USER_TEMP_DIR)`. `MINICODE_CONTROL_DIR` overrides both
  sides (tests). One JSON object per line, checked against
  `ControlProtocol`'s table of commands and arguments (unknown or wrongly
  typed is an error answer, never a crash), handled on the main thread within
  100 ms (`ControlServer.mm`, `ControlCommands.mm`). A connection has one
  request at a time with the main thread (the next line is read once the
  answer is out), writes are non-blocking with a write source, and a peer
  that takes nothing for 10 s is dropped, so one client can neither queue
  unbounded main-thread work nor hold up the others.
- `tools/minicode-cli.cpp` builds `Contents/MacOS/minicode-cli`, which the
  launcher execs for subcommands: newest live socket (or `--pid`) in the
  shell's `$TMPDIR/MiniCode` and the user's own temporary folder's (a nix
  shell's TMPDIR differs from the app's), only in folders that are the
  user's, real and 0700, moving on to the next when one refuses the
  connection; starting minicode and waiting 10 s when none runs, except for
  reads (`state`, `selection`, `term-output`, `procs`), which exit 3 at once; a
  running minicode with no socket (older than the channel) gets "quit and
  open it again", nothing launched. Exit codes 0, 1 command error, 2 usage, 3
  no app.
- Commands: `open path[:line[:col]]` (`--to`, `--note`, `--from`, `--keep`),
  `window`, `browse` (into the pane's agent page; see below), `page`,
  `term` (`--cwd`, `--title`,
  `--run` typed and entered visibly), `diff` (`--base` never starting with
  "-"), `split`, `state` (`--pretty`), `selection`, `term-output`, `tour`, `procs`
  (`--json`; see "Processes and ports").
  None closes, replaces or reorders the user's tabs and terminals. `open`,
  `diff` and `tour` read and open only plain files (a FIFO blocks `open(2)`
  for good), `diff` up to 16 MB a side, reading and aligning off the main
  thread (`MCComparison prepare`, once).
- **The keyboard**: the editor is one text view for every tab, so putting the
  first responder back (`keepKeyboardDuring:`) is not enough: it then shows
  the agent's file, and the user's next keystroke lands there. While the
  window is key and the keyboard is in the editor's slot
  (`userIsInDocument`), `openTabForAgent:preview:quiet:` and
  `showComparison:preview:quiet:` add the tab without showing it, with a
  status note and `"background": true` (the CLI says so on stderr); a note
  waits with its tab (`MCNoteState.pending`) and is placed when it is shown.
  The user's own steps (Option+], a click) always show.
- **Notes** (`AgentNotes.mm`): a layout-manager highlight and a box over the
  text view, never in the file or its undo; upper third; closed by Escape, ×,
  or an edit touching its lines (undo does not bring it back); plain text up to
  500 characters (cleaned by the handlers too, bidi controls dropped); not
  persisted. They go in one agent preview tab per window (`agentPreview`,
  italic title), replaced by each such open and kept by an edit or a
  double-click on its title; comparisons have a preview tab of their own, so
  the skill's open-then-diff keeps the note. A note moves the caret only in
  the preview tab, and a clean tab the agent just rewrote takes the disk's
  text before its note is placed (`notesCatchUpWithDisk`), or the reload a
  moment later closed it. Tours step with Option+] and [ (keymap commands
  enabled only during a tour) and show "2 of 3".
- `skills/minicode/SKILL.md` (under 150 lines; `minicode install-skill` or
  `make install`): show work as
  notes, a diff and tests in a terminal; open code with a note when asked where
  something is; read `state` and `selection` for "this"; never rearrange the
  user's tabs, `--run` long or destructive commands, or write workspace scripts
  unasked; plain paths on exit 3.
- **Agents check pages** (spec feature 6, 2026-11-06; `PageCheck`,
  `BrowserPages.mm`, `ControlPageCommands.mm`): `browse` loads into a second
  web view in `BrowserView`, the agent page, with a Yours | Agent switcher
  and an x that closes it (the user's page as it was); an address typed in
  the bar or a link from elsewhere goes to the user's page. The pane shows
  the agent page unless the keyboard is in the pane (`"pane_busy"`).
  `browse` takes only http, https, file, about:blank, host:port and paths,
  never a search or another scheme. `page --url/--text/--console/
  --screenshot/--reload`, `--yours` for the user's page; text, console and
  screenshots only for local pages unless `control.page-any-origin`, checked
  again when the read comes back. Status from
  `decidePolicyForNavigationResponse` (which answers as WebKit does without
  it: allow what it can show). The agent page's configuration installs, at
  document start, a listener in an isolated `WKContentWorld` (uncaught
  errors, rejections, and the only message handler) and a page-world
  wrapper of console.* that relays each call as a DOM event: an isolated
  world cannot see the page's console object, so there is no other public
  way; relayed lines can only be console calls (`fromPage`). Traps: a
  snapshot paints only the visible area, so `--full` rasterizes
  `createPDFWithConfiguration:` (sRGB; WebKit's #FF0000 reads back as about
  (1, 0.15, 0) in both kinds of picture); `WKWebView.title` can still be
  empty at didFinish, so `--wait` reads `document.title`; a GUI check's
  command line is built when queued, so start a test server before
  queueing steps. `BrowserPagesSuite` serves pages from a server inside the
  test process (127.0.0.1 and ::1); 20 checks grow minicode plus the agent
  page's WebKit process by about 0 MB (the page process itself is 25-35 MB).
- Ground rules for this and the agent workspace: nothing runs because a folder
  or file was opened; Claude Code's data is read, never written, moved or
  deleted; nothing leaves the Mac.

## The Claude Code IDE link (Mac, agent.ide-link)

Claude Code's `/ide` answered per window, as VS Code's extension does. Off by
default; View > Claude Code IDE Link (`MCIdeLinkMenuItem`, its own target)
writes `agent.ide-link` through `Settings::withValue`. Core `IdeLink.{h,cpp}`
(`ide:*` tests): SHA-1, base64, the upgrade handshake, RFC 6455 frames, MCP
over JSON-RPC with a tool table, the lock file, the tools' JSON shapes. Mac
`IdeLinkServer.mm`: sockets on the main queue, the lock file, the tools.

- **The protocol, as read** from Claude Code 2.1.319 (the bun binary's
  embedded JS: search it for "Failed to read IDE lockfile", "ws-ide",
  "callIdeRpc", "openDiff") and the VS Code extension 2.1.318
  (`~/.vscode/extensions/anthropic.claude-code-*/extension.js`: "Fre(",
  "Wnt(", the tool handlers): `$CLAUDE_CONFIG_DIR/ide` or `~/.claude/ide`,
  `<port>.lock` 0600 holding pid, workspaceFolders, ideName, transport "ws",
  runningInWindows, authToken. Claude Code connects to `ws://127.0.0.1:port`
  with subprotocol "mcp" and `X-Claude-Code-Ide-Authorization`, sends
  `ide_connected {pid}`, listens for `selection_changed {text, filePath,
  fileUrl, selection}` (0-based lines, UTF-16 characters), calls `openDiff
  {old_file_path, new_file_path, new_file_contents, tab_name}` and expects
  `[FILE_SAVED, text]` or `[DIFF_REJECTED, tab]` (it writes the file itself),
  then `close_tab`; `getDiagnostics {uri?}` before and after edits (it
  compares the answer's uri with the one it sent, so answer under that
  spelling). It shows the model only getDiagnostics and executeCode. It uses
  openDiff only when its global `diffTool` is "auto". It auto-connects when
  CLAUDE_CODE_SSE_PORT is the port (minicode's terminals get it,
  `MCIdeLinkTerminalEnvironment`), else /ide lists lock files whose folders
  hold its cwd (unresolved, so the lock lists the folder as spelled and
  resolved); it skips the pid check outside VS Code and JetBrains terminals,
  and deletes lock files of dead pids itself.
- **Rules kept**: 127.0.0.1 only, a 48-hex-digit secret per window, Host must
  be loopback, nothing said before the token; writes nothing under ~/.claude
  but its own lock file (temp file and rename, removed on window close,
  setting off and atexit); removes at start only lock files with ideName
  minicode, another pid, that pid gone. openFile goes through
  `openTabForAgent:preview:quiet:` (beside the user while they type) and
  highlights startText..endText with an empty note; openDiff is a
  comparison tab (`showComparison:preview:NO quiet:YES`) with a decision bar
  (`MCComparison decisionPrompt/onDecision`, CompareView); closing the tab
  rejects (`closePageOf:` calls `tabClosed`); close_tab and closeAllDiffTabs
  close only comparisons the link offered. Not offered: saveDocument,
  executeCode. A client that goes leaves its diff saying so.
- Checked by `make guitest SUITES=idelink` (a WebSocket client on a thread;
  `MC_GUI_NODE=/opt/homebrew/bin/node` adds node's own client) and the
  `ide:*` groups. Not run against a real Claude Code session.

## The agent workspace (Mac)

- **Core**: `AgentTranscript` reads Claude Code's JSONL behind a `Format`
  interface (a second agent would be a second Format), skimming lines for type,
  timestamp, ids, cwd, gitBranch, version, text and each tool call's file_path,
  command and description; outputs are byte ranges read later by `readOutput`
  from two 64 KB windows. Bad lines are counted and skipped, unknown fields
  ignored, Claude Code's request copies left out; only user and assistant lines
  count. `Tail` reads whole lines from the last offset, holding back a line
  being written, and restarts on a replaced or shrunk file. `AgentIndex` finds
  sessions whose cwd is the folder or inside it, from the files' own cwd (the
  first decides), never from the lossy encoded folder names; a file whose first
  cwd is elsewhere is read for 64 KB once. Not thread-safe: copy rows out.
  `AgentEvidence`: every Bash call, test commands recognised anywhere in a
  chain past env, time, timeout and assignments (pytest forms, make test and
  check, ctest, npm/pnpm/yarn test, cargo test, go test), their summaries read
  from the tail, no verdicts. Paths: `~/.claude/projects`
  (`$MINICODE_CLAUDE_PROJECTS`), the `/private/tmp/claude-<uid>` scratchpads
  (`$MINICODE_CLAUDE_TMP`). Fixtures: `tests/fixtures/claude`.
- **Mac**: `MCAgentModel` keeps the index on one serial queue and git on
  another, copies rows to the main thread, follows FSEvents on transcripts and
  scratchpads, and empties (saying so) if the transcripts folder goes.
  `MCAgentWindow` is made lazily. The Agents panel (Shift+Cmd+A) swaps in like
  the git panel: sessions newest first, subagents nested, files in the folder,
  the worktree and outside (gone ones struck through). The Evidence view
  (`MCEvidencePage`, an `EditorPage`): the final message beside the last test
  command's result, then every command with the first and last 20 lines of
  output read lazily (Show all up to 1 MB); Open in Terminal types a command at
  the session's folder.
- **Waiting for you** (spec feature 7, 2026-11-06): a terminal's id
  (`TerminalView terminalID`, what `minicode state` reports) is given when it
  is made, not on first use, because its shell gets it as
  `MINICODE_TERMINAL_ID`, with `MINICODE_PID` (inherited ones are dropped).
  `minicode hooks --install` (run by the user only; backs up, merges, a second
  run changes nothing) adds `minicode notify --from-hook` to Claude Code's
  Notification, Stop, UserPromptSubmit and PostToolUse hooks; notify never
  starts minicode and with --from-hook prints nothing and exits 0 whatever
  happens. Core: `ClaudeHooks` (hook JSON, the settings merge) and
  `AttentionBoard` (marks, sessions); Mac: `MCTerminalAttention` draws it
  (the tab's dot and tooltip, the Dock badge, Ctrl+Cmd+W, Show Terminal in
  the Agents panel, the session's scratchpad and worktrees first for a
  clicked path). Typing (the grid's `onTyped`, the input line's edits) and
  closing reach it as Terminal.h notifications. `make guitest
  SUITES=attention`. Trap: once a Control+Command key reaches the main menu,
  AppKit redraws its own Emoji & Symbols and Start Dictation items in their
  Globe-key form beside hidden twins with the same key; a menu key clash
  check must skip them (ControlSuite does).

## Processes and ports (Mac)

- **Core** (`ProcessTable`, `procs:*`): a `Tracker` samples a `Source`
  (the table, then per process cwd, command line, footprint, CPU time,
  listening TCP ports) and keys processes by (pid, start time). Tracked:
  descendants of terminal shells (the shells themselves not), kept with
  their origin after re-parenting to launchd, and their children; the
  user's processes outside minicode's own tree (language servers, git and
  shells are in it) whose cwd is in an open window's folder ("not started
  here", dropped when the cwd leaves). The command line is read again when
  the short name changes (a fork seen before its exec). Exited ones stay a
  minute. `buildView` groups per window: its terminals, orphans (terminal
  gone; shown by window or by cwd), not started here. `planStop` signals the
  process group only when every member is the process or below it, so a
  server Claude started never takes Claude's group down; never minicode, its
  group, a shell, pid 1.
- **Mac**: `MCProcessMonitor` starts itself at launch (`+load` observes
  didFinishLaunching), samples on its own serial queue every 10 s, 2 s while
  a panel shows; at each look it asks AppDelegate's `controlControllers` for
  folders (`realpath`, not `stringByResolvingSymlinksInPath`, which turns
  /private/tmp into /tmp while the kernel reports /private/tmp) and
  terminals (`MCControlTerminalID`, `TerminalView shellPid`). CPU times from
  `proc_pid_rusage` are Mach ticks: convert with the timebase. Stop checks
  each pid's start time before SIGTERM and again before the SIGKILL 3 s
  later. `minicode procs` samples once more before answering, and never
  opens a window for `--folder`. Session (R7.4) is a field the terminals
  could fill; nothing sets it yet.
- **Checks**: the `processes` suite starts a server (python3 found with
  `MCFindProgram`, else `nc -lk`) in its own terminal, disowns it, closes
  the terminal, stops the orphan; a sleep for Stop's question; a server
  started by `sh` that exits at once (launchd its parent) for "not started
  here"; times five looks with at least 200 processes. Trap: in `sh -c 'cd
  d && prog &'` the `&` takes the whole list, so a subshell holds the
  pipes and is the parent; write `cd d || exit; prog &`.

## Opening from Finder, the Dock and the command line (Mac)

- **Info.plist** `CFBundleDocumentTypes`, all Alternate with no
  `NSDocumentClass`: text and source UTIs, Markdown
  (`net.daringfireball.markdown`, also imported), the exact image, video and
  audio UTIs shown (not `public.image` or `public.movie`), PDF,
  `public.folder`, Office, iWork, OpenDocument and SQLite, and by extension
  TeX, Go, Rust, TOML, conf, ini, tsx and `.ts` (macOS types it as MPEG-2
  video). No `public.data` (VS Code has none either). Where nothing else claims
  a type (.conf, .bib) minicode is the double-click app anyway.
- Open With, double-click and Dock drops arrive as odoc events at
  `application:openURLs:`: folders first (a window on it, else a new one), then
  each file to the window with the innermost root holding it, else the window
  in front (a new window only with none open); paths compared resolved and
  respelled from the window's root; `showFile:` opens hidden dotfiles too.
  `applicationShouldOpenUntitledFile:` returns NO.
- Launch Services delivers the open between will- and didFinishLaunching, so
  setup is in `applicationWillFinishLaunching:`, and didFinishLaunching opens
  no window of its own when one exists and argv is empty (a Finder launch has /
  as cwd). `main()` registers `NSTreatUnknownArgumentsAsOpen` NO, so argv goes
  through the argv code only.
- The launcher (`scripts/minicode-launcher`, `Contents/Resources/minicode`):
  paths go to a running app with `open -b com.minicode.editor`, else `open -a`
  its own bundle (VS Code's `code` rules); `-r` writes a request file to
  `$TMPDIR/MiniCode-open/*.minicode-open` (read by `MCTakeOpenRequest` only
  from the app's own temporary folder); `-n` runs the binary detached;
  "running" is `ps -axo comm=` ending in `/MiniCode` (pgrep found nothing from
  Claude Code's shell); `MINICODE_LAUNCHER_DRYRUN=1` prints the command.
- **Never `lsregister` or `open` a build in the repo or a worktree**: Finder
  then offers that copy too. Check claims with a renamed copy registered
  outside /private/tmp (Launch Services ignores apps there), asked with
  `LSCopyApplicationURLsForURL`, then unregistered; send odoc events offscreen
  through `NSAppleEventManager dispatchRawAppleEvent:`.

## Demo GIFs (`make demos`)

`scripts/demos.sh` builds `/private/tmp/minicode-demo` (a 5-commit copy of
`demo/` with fixed dates, scratch HOME and ZDOTDIR, `tools/demo-settings.conf`
with the terminal in line mode, a copy of the tectonic cache, a `.vimrc`), runs
the raw binary with `MINICODE_DEMO=<scene>` under `env -i`, kills only its own
pid after 2 min, and runs `build/makegif`. `MINICODE_DEMO=list` lists scenes;
`DEMO_KEEP_FRAMES=<dir>` keeps frames and the log.

- A scene is a function in `src/Demo.mm` of queued builder steps (`clickFile:`,
  `type:`, `key:caption:`, `waitFor:timeout:recorded:`, ...), points resolved
  lazily (`pointForText:` through `firstRectForCharacterRange:`). Input is real
  posted `NSEvent`s; the pointer and key captions are an overlay view. The
  user's own input is swallowed meanwhile (only events whose timestamp, in
  whole microseconds, the demo posted pass), since a stray key once dirtied a
  file and hung the scene on "Save changes?".
- Capture is `screencapture -x -o -l<wid>` every 100 ms; a popover grows the
  capture and the manifest's `child` rects let makegif crop; windows in front
  are captured apart and composited. makegif writes the GIF itself (ImageIO
  stores frames whole, with disposal 2 for alpha): one median-cut palette,
  merged frames, changed rectangles with transparency, LZW.
- `NSHomeDirectory()` ignores `$HOME`, so the project lives under /tmp. The
  display must be awake (`caffeinate -u -d -t 200 &`) or captures are black and
  `-l` fails like a permission error. The lsp scene saves before F12; never
  press Esc in the editor in a scene (`complete:`); keep `demo/paper/notes.tex`
  free of math (the cache has no math fonts); the LaTeX scene exits 3 without
  tectonic; `links` draws the hover underline itself.

## Memory benchmark (`make membench`)

`scripts/membench.sh` runs one workload on minicode and on VS Code + Chrome +
Preview and sums each side's physical footprint (`tools/procmem.c`,
`proc_pid_rusage`; 320 MB against 1,283 MB on 2026-09-21, in the README). A
process belongs to a side by parent or responsible pid (WebKit's helpers are
launchd's children); a shell-launched app makes the terminal responsible, so
minicode starts with `open -n -g --env`. `src/Bench.mm` resolves paths like the
tree; a tiny VS Code extension types the command; `MINICODE_BENCH` skips
activation. Check each side's breakdown before quoting a number (a first run
"measured" 55 MB with clangd and WebKit missing).

## Verification reality (important)

How much of the GUI you can check depends on the port and on whether the
session has a display. Check first.

- **Core**: `make test`, and `make test CXX=g++-16` for libstdc++.
- **The Mac, committed checks**: `make guitest [SUITES=...]` builds
  `build/guitest` (the whole app with main.mm's `main` renamed `minicode_main`,
  plus `tests/gui/*.mm`) and runs it offscreen; the macOS CI job runs it. The
  harness (`tests/gui/Harness.{h,mm}`) re-execs into a scratch world under
  `/tmp/mcgui-*` (HOME and CFFIXED_USER_HOME, ZDOTDIR with an empty .zshrc, a
  short TMPDIR for socket paths, settings with language servers off, a defaults
  suite, backups, a private pasteboard, git without global config), swizzles
  window ordering, activation (policy Prohibited), child windows, NSAlert
  (`MCGuiAnswerAlerts`), the pasteboard and `openURL:`, reports the test window
  as key and main, and plays suites (`MC_GUI_SUITE`, `MCGuiStep`, `MCGuiWait`,
  `MCGuiAsync`, `MCGuiPause`, `GCHECK`) from the run loop; it fails if any
  window reaches the screen. `MC_GUI_SNAPSHOTS=dir` saves pictures to look at,
  `MC_GUI_KEEP=1` keeps the scratch folder, `MC_GUI_SAMPLE=file` samples the
  keystroke suite (`MC_GUI_FULL_PERF=1` runs foldperf on 100,000 lines, not
  20,000). Suites: control, agents, backups, find_bar, folding, foldperf,
  format, git_panel, keymap, keystroke, lsp_fixes, palette, paste, previews,
  problems, processes, replace_in_folder, run_file, symbols, terminal_reflow, and in
  LooseEndsSuite.mm terminal_exec, terminal_keys, terminal_zoom,
  agent_autosave, multicaret_rows. **Language server checks use the scripted
  server** `build/fakelsp` (`tests/fakelsp/fakelsp.cpp`, built by `make
  guitest`): `MCGuiFakeLsp(name, script, program, words)` writes a JSON
  script of rules and returns the command line for `lsp.<language>`, and
  `MCGuiFakeLspReceived` reads what the client sent; the file's header
  describes the script. basedpyright, where installed, runs as well in
  symbols, never instead. A suite named `_x` runs only when named, as a
  second app that `MCGuiRunChild` starts in the same world (backups SIGKILLs
  one). Wait for a condition; never pause before a positive check. **New
  user-visible behaviour gets a committed check here.**
- **Harness traps**: key-monitor keys go through `[NSApp sendEvent:]` with the
  window's number (`MCGuiSendKeyToApp`); menu keys as CGEvent-made events
  through the window then the main menu (`MCGuiPerformMenuKey`, built from the
  key code; setting the unicode string made the menu refuse a matching key);
  tree keys through `[window sendEvent:]`; a shifted key matches the menu only
  as the NSEvent `eventWithCGEvent:` makes, never one rebuilt from characters;
  everything in one step is one undo group (wrap edits in `MCGuiUndoGroup`,
  close a group a step opens); give a suite its own window and close it
  (`MCGuiSetKeyWindow(nil)`); cd terminal suites into `MCGuiProject()`;
  `MCGuiPause` is timed from when it is reached; never browse non-file URLs.
  Language servers are off in the harness; a check that needs one points
  `MINICODE_LSP_LOG` at a scratch log and kills only the PIDs it started (the
  user's own minicode and other sessions run clangd too). Older traps: an arrow
  key event needs the function-key character (U+F701...) as `characters`, and
  an empty string makes `-[NSMenu performKeyEquivalent:]` throw; Shift+Cmd+H
  matches only with `charactersIgnoringModifiers` "h"; with `groupsByEvent`
  off, `saveCurrentFile:` outside a grouping throws once a sort registers undo;
  an offscreen window is never key, so a plain key through `keyDown:` does
  nothing (type with `insertText:`); an `NSEvent keyEventWithType:` posted
  through NSApp reaches the monitor but not an offscreen first responder;
  `NSApp.currentEvent` is not the event given to `performKeyEquivalent:`; a
  scroll restored right after `setAttributedString:` stops where layout ends
  unless a screen past it is laid out; an NSPopover never shows for a window
  not on screen; wait from a timer, not a main-queue block, or the app's
  dispatches never run; `[NSApp run]` calls `applicationWillFinishLaunching:`
  itself; NSZombieEnabled names a message sent to a freed object. Keep harness
  files in a scratchpad folder of your own name.
- **Ad hoc**: a temporary `getenv("MINICODE_*TEST")` block run headless with a
  poll-and-kill loop (no `timeout` on macOS), removed by exact text. Read state
  by KVC; drag NSSplitView by posting drag and up events, then calling
  `mouseDown:`; `hitTest:` measures grab areas.
- **Pixels**: `/usr/sbin/screencapture -x -o -l<windowNumber>` from inside the
  app, read with `NSBitmapImageRep colorAtX:y:` (y from the top, Retina scale);
  `renderInContext:` on a layer gives exact colors where `cacheDisplayInRect:`
  shifts them. The only way to verify translucency. `CGWindowListCreateImage`
  is gone in the macOS 15 SDK.
- When a check fails, print the actual value first: most failures were wrong
  checks. Never call GUI behaviour verified when it was only compiled; say what
  needs the user (the look, real keys and mouse, trackpad, sound). Never
  activate the app from a test; ask before anything that shows a window.
- **GTK from the Mac: Docker.** `linux/dev/gtk-dev.sh` builds the image
  (ubuntu:26.04, CI packages, Xvfb, xcompmgr, xdotool, ImageMagick) and a
  container with the repo mounted; `linux/dev/README.md` has the workflow.
  Build into `/build` inside; `Xvfb :99` plus `xcompmgr`; launch with
  `GDK_BACKEND=x11 GTK_A11Y=none` under `dbus-run-session`, a scratch HOME, and
  `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` (bubblewrap cannot start there).
  Drive with `xdotool`, capture with `import -window root` (dotted debris is
  xcompmgr's). `pkill -x minicode`, never `pkill -f` with a path (it kills the
  `docker exec`). The image is about 1.8 GB on a tight disk. **Docker trap**:
  `open -g -a Docker` raised an administrator password prompt and the daemon
  never started; never start Docker unattended.
- **GTK on the ThinkPad** (Ubuntu 26.04, GNOME on Wayland; check
  `$WAYLAND_DISPLAY`): `meson test -C build`; `setsid gtk-launch
  org.minicode.Editor &` then `pgrep -a minicode` (a real window: say so, leave
  none behind); `systemctl --user list-units --type=scope | grep minicode`
  (`app-gnome-org.minicode.Editor-*.scope` proves the dock association, a
  nested `vte-spawn-*.scope` the terminal); `journalctl --user --since "-15
  min"`. Real input: a private rootful Xwayland driven by XTest
  (`linux/HANDOFF.md`, "How to work on it"); prefer it to hooks, which passed
  three bugs the user then hit by hand.

## The Linux port

`linux/HANDOFF.md` (items 1 to 14) and `BUILD-LINUX.md` (build, and what is
unverified). It shares the highlighter, Markdown, tasks, settings, comments,
AutoIndent, KeepSorted, FolderSearch, TextFind, LatexDoc, SyncTex, Json,
LspClient, TermLinks, GitStatus, GitGraph and GitModel; VTE is its terminal. The core
lists in `linux/meson.build` and `linux/Makefile` must agree
(`linux:build-lists`; CI builds only with Meson). Its traps are in HANDOFF.md:
accelerators taking keys from VTE (`updateShellKeys`), the selection color
(`ThemeCss.cpp`), one click and autoselect in the tree, `settleOnCaret`,
`<Ctrl>greater`, `ScrollSettle.h`, links acted on from an idle, embedded
widgets off the view before a re-render, `Terminal::topRow`.

## The Android port

`android/README.md` is its handoff. The phone is a Unihertz Titan 2 (Android
16, 576 by 640 dp, hardware keyboard, Termux and F-Droid); wireless debugging
changes port on every reconnect, so ask for it (restart the adb server if
connect says "No route"); test files are in `/sdcard/mc-test`. Gradle 8.14.3
via the wrapper, JDK 21, SDK and NDK from `sdkmanager`. CI builds the debug
APK (no key needed); `linux:build-lists` checks the CMakeLists names every
core file its files include. Traps paid for:

- The core compiles with the NDK and its tests pass on the phone. `TermKey`
  ordinals in `Pty.kt` mirror `src/TerminalScreen.h` (Enter numbered as Escape
  looked like a dead key). The terminal uses `TerminalScreen` with scrollback
  off; its `resize` now reflows wrapped lines there too.
- No Ctrl, Esc or Tab on the phone; Alt is the symbol layer; Android claims Sym
  with some letters; a spare key (403 on the Titan, Menu elsewhere) is the
  leader and repeats in bursts, so presses split on release. `adb logcat -s
  MiniCodeKeys` shows keys. Injection cannot send Sym, keycodes above ~288 or
  `sendevent`; `adb shell input keycombination KEYCODE_CTRL_LEFT KEYCODE_U`
  does carry Ctrl. `adb shell input text` shuffles letters in fields.
- Letters reach a focused field through the input method
  (`CodeEditText.commitText`), not key events; Pastiera needs the
  visible-password variation; a letter with Ctrl has no `unicodeChar`; the
  terminal declares `TYPE_NULL` (composition re-sent words).
- Termux: bash on a pty from a helper built as `libminicode_pty.so`, sent over
  the token socket and run from Termux's `$TMPDIR`; servers and git there too
  (listen on 127.0.0.1 by name: `getLoopbackAddress()` is ::1; git through one
  bash loop with NUL-separated fields and `-c safe.directory`, never written to
  a config); only shared storage.
- One FileObserver per path; `wm size` recreated the activity until
  `smallestScreenSize|screenLayout` joined `configChanges` (the phone's
  override is 1076x1200, never `wm size reset`); `adb shell` in a `while read`
  loop eats its input (`-n`); no app can force the on-screen keyboard up beside
  a hardware one (`show_ime_with_hard_keyboard`, put back as it was); Back on
  Android 16 for API 36 only through `OnBackPressedDispatcher`; a focusable
  ViewGroup gets a grey veil (`defaultFocusHighlightEnabled = false`); a long
  diff needs a RecyclerView; preview text must not be selectable; View's
  constructor reports visibility before a Kotlin subclass's fields exist.
- A paused MediaPlayer moves by itself (offload restarts with a seek after 10
  s), so `PlayerPane` releases it 4 s after pausing; MediaController is a
  focusable window, so the pane draws its own bar. Web pictures re-render
  keeping the place by run index, with arrived bytes passed straight to the
  next render (an LruCache loop otherwise); release builds trust only system
  CAs, so test against public https addresses.

## Distribution

- `e-c-hansen/minicode` (public); each `vX.Y.Z` GitHub Release carries the zip
  and the signed APK.
- `e-c-hansen/homebrew-tap` holds only `Casks/minicode.rb`, written by
  `release.sh` from `packaging/minicode.rb` (the only copy anyone edits). The
  cask clears the quarantine flag in `postflight_steps` (Homebrew 7 deprecated
  `postflight`; `{{appdir}}` is the token); check edits with `brew style` in
  `/opt/homebrew/Library/Taps/e-c-hansen/homebrew-tap`. Homebrew is the only
  updater, so the cask must not declare `auto_updates` (a plain `brew upgrade`
  would skip it). Homebrew runs `lsregister` only when it reopens apps it quit;
  macOS registers a new bundle in /Applications itself.
- Not notarized (that needs the paid Apple Developer Program);
  `scripts/sign-and-notarize.sh` is ready.
- The APK is signed with `~/.config/minicode/release.keystore` (password in the
  Keychain, `minicode-android-keystore`); every update must carry that
  signature, so never regenerate it (the user keeps a backup). Installed by
  hand or through Obtainium.
- Ubuntu PPA `ppa:echansen/minicode` (a lost key can be replaced on Launchpad).
  Traps: macOS `tar` adds `._*` files `dpkg-source` rejects (`git archive`);
  `tar` fails with ENOSYS under Docker Desktop's amd64 emulation; gpg-agent
  cannot be forwarded into Docker Desktop, so signing is on the host.

## Current state (2026-11-05)

- **Released: 1.5.6** (themes, the inline agent, Restart minicode, the browser
  kept across restarts, the first release signed with "MiniCode Local
  Signing"); the user's Homebrew copy is on it. 1.5.4 to 1.5.6 went out without
  the PPA upload (Docker was not running): `scripts/ppa.sh 1.5.6` once Docker
  is up.
- **Merged since, not released** (`git log f3e6886..`, Mac unless noted): the
  second review's bug fixes (git time limits and the Makefile on Linux too),
  auto-save, the line commands and moved keys, Problems, Ruff beside
  basedpyright, find and replace, line numbers, brackets, zoom, folding,
  symbols and Go Back, the terminal's find, Cmd+K, synchronized output, reflow,
  mouse and titles, the control channel with notes and tours, the agent
  workspace, the new viewers, keys.conf, MCSourceStorage, the EditorController
  and Lsp splits, and `make guitest` in CI.
- **Needs the user's eyes**: all of the above on a real screen with real keys
  and mouse (the gutter and folds, the find bar, Problems, the Agents panel and
  Evidence, the viewers, notes and tours, the palette's symbols, trackpad
  momentum in the terminal); the grid terminal with their own .zshrc, prompt,
  autosuggestions and fzf, Claude Code's Shift+Return, vim with the mouse; real
  F2, F8, F9, Shift+F12 (fn on a laptop), the Control+Command folding keys,
  Shift+Option+O; a real crash or force quit with unsaved tabs; a real Request
  a Change end to end and its relaunch; Finder's Open With, double-click and
  Dock drops; `minicode -r` against the running app; the LaTeX popover's
  placement; Ctrl+Space with several input sources. **Linux**: none of today's
  work ran on Linux beyond CI; the ThinkPad still owes the folder picker after
  closing its window, F2 rename in the tree, renames across two windows under
  GNOME, the PDF placeholder at 2x and the git panel. **Android**: the leader
  key and real typing in the commit and edit boxes (Enter and Shift+Enter), the
  key rows against the curved corners, bash and vim on the real keyboard, the
  on-screen keyboard resizing the pane, real sound and keys in the player, and
  for web pictures a badge link, the scroll staying put and the toggle making
  no requests (then delete `/sdcard/mc-test/web-test`).
- The Reddit drafts are a Claude Doc, "MiniCode Reddit drafts"
  (https://claude.ai/code/artifact/06496ec8-70b7-443d-af5c-9c53f3996ffe).

## What's next

See `ROADMAP.md`.

