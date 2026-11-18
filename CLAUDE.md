# CLAUDE.md — working notes for minicode

minicode is a small native macOS code editor built from scratch, no Electron,
no third-party dependencies. It links only Apple system frameworks (Cocoa,
WebKit, CoreServices, Quartz for PDFKit and Quick Look, AVKit, AVFoundation,
CoreMedia, and Carbon for the terminal's secure keyboard entry) plus the C++ standard library, the system zlib and the system
libsqlite3. The LaTeX preview and the Markdown math run tectonic, which the
user installs or the app downloads on request, and Markdown's diagrams the
user's mermaid-cli or a pinned mermaid.js downloaded the same way; the language servers, formatters
and Claude Code it talks to are the user's own.

The shared core is `src/` (C++ only); the Mac app is under `mac/` (`mac/src`,
its GUI checks in `mac/tests/gui`), the GTK4 Linux app under `linux/` and the
Android app under `android/`, all three building the core verbatim. The
owner's rule (2026-11-14): shared logic looks generic, and anything specific
to one system is named for it, by its folder or its name. A new Mac-only
file goes in `mac/`; a Mac-only file outside those folders (a script, a
tool) carries `-mac` in its name. Until 2026-11-14 the Mac's files were in
`src/` beside the core. Their handoff notes are
`BUILD-LINUX.md`, `linux/HANDOFF.md`, `linux/README.md` and
`android/README.md`; this file covers the macOS app except where it says
otherwise. Almost everything added since 1.5.0 is Mac-only, with its logic in
the core so the ports can follow (`ROADMAP.md`).

This file is architecture and the traps already paid for. What a commit
checked, and its timings, is in its message (`git log`); each source file's
opening comment says what it does in more detail than the lines below.

**Area notes live in `docs/notes/`** (split out 2026-11-17, when this file
had grown to about 320,000 characters that every session and subagent
loaded).
This file holds only what an agent needs before touching any code; the
"Area notes" list below says which notes file to read before working in
an area. **A new rule or trap about one area goes in its notes file, not
here**: add it to that file's section, and to its "Traps" list when it is
a trap.

## Safety rules

- **Never start Docker unattended**: `open -g -a Docker` raised an
  administrator password prompt and the daemon never started. The GTK
  checks from the Mac and `scripts/ppa.sh` on the Mac need it; ask first.
- **Never write, move or delete anything under `~/.claude`**: Claude Code's
  data is read only. The one exception is the waiting dots' hooks, written
  only on the user's own two presses (`docs/notes/agents.md`); tests use a
  scratch `CLAUDE_CONFIG_DIR`.
- **Ask before anything that puts a window on the user's screen**:
  `make guitest` takes the keyboard, `make demos` records the screen,
  `make run` and the GTK app on the ThinkPad open windows. Never activate
  the app from a test.
- **Kill only processes you started**, by recorded pid: never `pkill -f`
  with a path, never the user's own minicode, clangd or another session's
  processes.
- **Never `lsregister` or `open` a build in the repo or a worktree**:
  Finder then offers that copy too (`docs/notes/opening.md`).
- Never regenerate the Android release keystore; never edit a version by
  hand (`scripts/release.sh` stamps it).

## Build, test, run, release

- `make` builds `MiniCode.app` (with `build/minicode-cli` copied into
  `Contents/MacOS`), signed with "MiniCode Local Signing" when the login
  keychain has it, else ad hoc (Apple Silicon needs a signature).
  Why the certificate, and Info.plist's usage descriptions:
  `docs/notes/build.md`.
- Built for macOS 11 and up: `-mmacosx-version-min` comes from Info.plist's
  LSMinimumSystemVersion, a newer API without an `@available` check is a
  compile error, and `scripts/check-minos.sh` (CI, release.sh) checks the
  binaries' minos and the cask's `depends_on`.
- `make test` builds and runs the core's tests (`tests/run_tests.cpp` with the
  `tests/*.inc` groups and the `*Tests.cpp` files) in `build/test-obj`, without
  -O2, and exits non-zero on failure. Groups are named `area:case`. Counts grow
  with every feature; do not quote them (8,490 at the end of 2026-11-05). Run
  it from the repo root: corpus and docs checks read files there and skip
  themselves when missing. clang++ on the Mac, make's default (g++) elsewhere,
  so CI runs it on Linux (the sanitizers job's g++ build; since 2026-11-17
  the GTK job runs no plain copy); `make test CXX=g++-16` reproduces CI's
  libstdc++ build on the Mac. Checks that read the docs: `commands:readme` (every default key
  is in `docs/REFERENCE.md`'s macOS column), `commands:linux-readme` (its
  Linux column against the Linux table, both ways), `install:readme` (every
  install command is in `docs/REFERENCE.md` in backticks), `linux:build-lists` (linux/Makefile and
  meson.build name the same core files, and Android's CMakeLists names every
  core file its own files include), `docs:gui-suites` (the GUI suite list
  under "Verification" names every suite and nothing else), and the
  `settings:*` count of keys in the default settings file.
- `make test-asan` (ASan and UBSan, the `fuzz:*` groups, `MC_FUZZ_SCALE`) and
  `make test-tsan` (ThreadSanitizer, `tests/tsan.supp`): `docs/notes/build.md`.
- CI (`.github/workflows/ci.yml`, six jobs: macOS, GTK4, sanitizers, tsan,
  Android debug plus Kotlin checks, Android release; **Android is frozen**
  since 2026-11-17: its jobs run only when CI is started by hand, CI passed
  does not wait for them, and release.sh skips the APK;
  `docs/ci.md`) is the gate: no merge
  to main while a job on main is red, except the fix for that red. The
  macOS job also builds `make dist-zip RELEASE=1` and checks it as
  release.sh does (`nm`, the zip's contents, check-minos); the Android
  release job signs with a key made in the job (`keytool`, random
  password), never the owner's release key. Every job has a
  `timeout-minutes` (macOS 40, GTK 45, tsan 45, the rest 20; `ci:timeouts` checks
  a new job has one), so a hang never holds a runner for six hours.
  **Since 2026-11-14** (`docs/ci.md`, the failure history and the branch
  protection): a seventh job, **CI passed**, is green only when the six
  are, and main's branch protection requires it (administrators, so
  release.sh, are not held to it). Every change goes to a branch and a
  pull request and is merged only once CI passed is green on that exact
  commit, never with `--admin`; the one direct push is release.sh's
  version commit, from a commit whose CI already passed. Each job's
  last step, `scripts/ci-summary.sh`, puts one line per failed check
  (FAIL, FATAL, sanitizer reports, compiler errors) plus SLOW, NEAR and
  LEAK lines at the top of the run's page and on the PR; the runner
  images are named (`macos-26`, `ubuntu-26.04`), never `latest`; a newer
  push cancels a PR's older run.
  How CI is made quick (ccache, two harness processes, `GUI_ALONE`,
  `GUI_SECOND_HALF`): `docs/notes/build.md`. `scripts/ci-local.sh` runs on the Mac
  what CI runs, without Docker (`--quick` before a push: core tests,
  build, check-minos, the sweep; the full run adds g++, the sanitizers,
  `make guitest`, which takes the keyboard, and the release zip).
  **Retries** (2026-11-17, `docs/ci.md`): a GUI suite that fails in CI
  is run once more, alone, in a fresh process (`MC_GUI_RETRIES`,
  `tests/GuiRetry.h`); passing then, it is a `FLAKY [suite]` line and
  the run passes; three such suites a job at most. The core tests are
  never retried. Every FLAKY line must lead to a fix; the nightly soak
  (`.github/workflows/soak.yml`, seeded orders `MC_GUI_SHUFFLE`, no
  retries) keeps the issue "Soak: flaky checks".
- `make guitest [SUITES="control agents"]`: the offscreen GUI checks in
  `mac/tests/gui` (see "Verification").
- `make run [DIR=~/path]`, `make icon` (from `tools/makeicon.m`), `make demos
  [SCENES="tour latex"] [POSTERS=1]`, `make membench-mac`, `make membench-linux`, `make install` (runs `minicode
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
- Linux-only releases (`scripts/release.sh --linux`), the PPA (`scripts/ppa.sh`,
  `scripts/ppa-wait.sh`, the keys) and distribution (the tap, the APK key,
  notarization): `docs/notes/release.md`.

## Layout of the code

The full map, file by file, is `docs/notes/code-map.md`; read it before
adding a file or looking for where something lives.

- `src/`: the core, pure C++17, no dependencies and no I/O unless noted,
  tested by `make test`; the Mac, GTK and Android apps all build it
  verbatim. **New logic goes here first, with tests; the GUI only displays
  it.** Its groups: documents and formats (`SyntaxHighlighter`,
  `MarkdownParser` and the other `Markdown*` files, `MathTex`,
  `MermaidDiagram`, `LatexDoc`, `LatexLog`, `SyncTex`, `Json`, `Csv`,
  `Notebook`, `IdeLink`), settings and commands (`Settings`, `Commands`),
  the document model and tabs (`Document`, `UndoHistory`, `SavePlan`,
  `TabSessions`, `EditorTabs`, `Backups`), editing (`LineDiff`,
  `AutoIndent`, `MultiCaret`, `AutoClose`, `Snippet`, `LineIndex`,
  `Folding`, `TextFind`, `Regex`, `Formatter`), language servers
  (`LspClient.h` over its nine files), the terminal (`TerminalStream`,
  `TerminalScreen`, `TermLinks`, `TerminalLayout`), search and git
  (`FolderSearch`, `GitStatus`, `GitGraph`, `GitModel`, `QuickDiff`,
  `ChangeReview`), and agents, palette and files (`CommandPalette`,
  `ControlProtocol`, the `Agent*` files, `ProcessTable`, `FileSniff`,
  `WebBookmark`, `PageCheck`).
- `mac/src/`: the Mac app in Objective-C++: `main.mm` (AppDelegate, the
  menus, the one key monitor), `EditorController` and its categories, the
  editor (`SourceStorage`, `CodeTextView`, `Gutter`, `FoldView`), `Lsp.mm`
  and its `Lsp+<Concern>` categories, the terminal (`Terminal`,
  `TerminalGridView`, `TerminalPanel`), the viewers, `Browser`,
  `GitPanel`, the agents and the control channel (`AgentPanel`,
  `ChangePanel`, `ControlServer`, `IdeLinkServer`), `Demo` and `Bench`.
- `mac/tests/gui`: the offscreen GUI suites; `tests/`: the core's tests,
  fixtures and corpora; `tools/` and `scripts/`: the CLI, the launcher,
  release, CI and benchmark scripts; `linux/` and `android/`: the ports;
  `docs/`: `REFERENCE.md` (for users), `ci.md`, and `notes/` (for agents).
- **EditorController's rules** (in full in `docs/notes/code-map.md`,
  checked by `structure:ivars`): `EditorController.mm` keeps init,
  dealloc, the window delegate, `validateMenuItem:`, undo and redo, and
  the active tab's forwarding accessors; everything else is in
  `EditorController+<Concern>.mm`. No new weight: a feature goes in its
  own files and classes, and EditorController gains only hooks. An ivar
  one category uses goes in that category's
  `EditorController+<Concern>Ivars.h` (imported by the category and by
  `EditorController.mm`); the shared block in `+Private.h` holds only
  ivars two or more categories use. Methods the rest of the app calls are
  declared in `EditorController.h` under `@interface EditorController
  (Concern)` named for the implementing category, and host protocols are
  adopted by their categories. LspSession is split the same way.

## Conventions

- **Keys**: a feature minicode shares with VS Code takes VS Code's Mac key.
  minicode's own Shift+Command keys never move (Shift+Cmd+K Request a Change,
  Shift+Cmd+T Toggle Terminal, Shift+Cmd+D TODOs, Shift+Cmd+E, Shift+Cmd+B,
  Shift+Cmd+H, ...); only a Command-only key yields when VS Code binds it to
  something else (Cmd+N New File, Cmd+L Select Line, Cmd+I the inline agent).
  The one exception, the owner's call (2026-11-16): Shift+Cmd+N, New Folder
  by Finder's convention, is VS Code's New Window (`When::NotTree`, keeping
  Option+Cmd+N) and New Folder only while the tree has the keyboard
  (`minicode.tree.newFolder`, a Mac-only `Tree` row sending `newFolder:`
  with tag 1; `explorer.newFolder` keeps the menu item, keyless on the Mac
  and Ctrl+Alt+N everywhere on Linux).
  Dump every menu key equivalent before adding one; no key is bound twice (the
  `keymap` suite and `commands:*` check it).
- **A new shortcut**: a row in `src/Commands.cpp` (id in VS Code's spelling
  where VS Code has the command, else `minicode.*`; title, menu, default keys,
  where it applies, selector, tag); the menu item in `main.mm` (and a
  forwarding method on AppDelegate if needed); a hints row (`appendHintsRow:`
  with `[self hk:@"id"]`, which reads the registry); a row in `docs/REFERENCE.md`'s
  table, sorted by action, a column per platform (the README's short table
  only for an essential key). The hints panel is an attributed string on
  tab stops; never align it with spaces (⌘ ⇧ ⌃ ⌫ ⏎ differ in width). Its
  stops come from the drawn widths of each column's keys and labels
  (`PageMeasure::layoutHints`): two columns when they fit the window, else
  one, never a row broken mid-row; it scales with the editor's zoom and
  scrolls when taller than the window (2026-11-08). **Its search box**
  (2026-11-16, `HintsSearchBox.{h,mm}`, core `HintsSearch.h`,
  `hints:search`): while the panel shows, the key monitor asks
  `hintsPanelTakesKeyDown:` before `MCKeymapHandleKeyDown`, and every key
  without Command or Control, not a function or Menu key, goes to the box
  (sent to its field editor's `keyDown:`, the keyboard taken at the first,
  given back on close; not while the palette is open): no command or view
  sees it, so Option+Z types. Esc clears, then closes; Return and Tab do
  nothing. A shortcut (key, label, state; a pair is two) matches when every
  word is found in it, keys also by name ("cmd", "shift+cmd+h"); matches
  are marked (`MCHintsMatchAttribute`) and the rest dimmed
  (`MCHintsDimmedAttribute`), never hidden, so the layout stays. Prefer
  menu items with key equivalents to the key monitor: they work in every pane
  and for shifted variants.
- No third-party dependencies, ever; new capabilities use system frameworks.
- The README is the front page (GIFs, highlights, getting started, the
  essential keys, links), kept short; the detail goes in `docs/REFERENCE.md`
  (every feature, settings, servers and formatters with the install table,
  the command line and agents, the ports, every shortcut, limitations), one
  file for every platform (split 2026-11-14, when the README was 23,000
  words).
- README and user-facing prose: plain sentences, Oxford commas, no emoji, no
  LLM-ish phrasing; the name is "minicode", lowercase, in prose and in every
  port's window title (2026-11-14; the bundle, the menu bar's app name, the
  launcher's label and folder names stay MiniCode). No labels the user did
  not choose.
- Commits in the repo's style (the area, then why and what was checked), ending
  with the Co-Authored-By trailer.
- A finished user-visible Mac change is pushed, released and upgraded on the
  user's Mac. Ask before anything that puts a window on their screen.
- **A new setting** goes in `Settings.cpp`'s `keyTable()`, its default, the
  commented `defaultFileText` (`settings:key-table` fails on a row it
  lacks), the view's `applySettings`, and `docs/REFERENCE.md`
  (`docs/notes/ui.md` for colors, `docs/notes/settings-and-keys.md` for
  the file and Adjust Settings).

## Area notes

Before working in an area, read its file in `docs/notes/`. Each keeps its
sections as they stood in this file, and ends with a "Traps" list of the
traps already paid for there.

- Build detail (signing, objects, compile_commands.json, the sanitizers,
  CI's speed): `docs/notes/build.md`.
- Releases for Linux, the PPA and distribution: `docs/notes/release.md`.
- The full map of the code and EditorController's categories:
  `docs/notes/code-map.md` (traps inside).
- Before UI work (the view tree, colors, translucency, manual frames, the
  editor, browser and terminal split, the page and the keyboard, window
  tabbing): `docs/notes/ui.md` (traps inside).
- Settings, themes, Adjust Settings, and keys (the registry, keys.conf, the
  key monitor): `docs/notes/settings-and-keys.md` (traps inside).
- Tabs, the preview tab, two editor groups, sessions, the disk, backups and
  auto-save: `docs/notes/sessions.md` (traps inside).
- The editor's text view (TextKit 1, the storage, layout, programmatic
  edits, line numbers, zoom, word wrap, large-file mode), folding, several
  carets and line commands, indentation and sorting, the palette and Go
  Back: `docs/notes/editor.md` (traps inside).
- The session's host on the Mac (editor.session-host: who owns a tab's
  text, the storage's edits recorded, the host's edits put back):
  `docs/notes/session-host.md` (traps inside).
- Highlighting (the lexers, markup, semantic tokens, bracket-pair colors):
  `docs/notes/highlighting.md` (traps inside).
- Find and replace, the regex engine and Find in Folder:
  `docs/notes/find.md` (traps inside).
- Format Document, formatter consent and install hints:
  `docs/notes/formatting.md`.
- Before touching language servers: `docs/notes/lsp.md` (traps inside).
- Markdown (the parser, the preview, the side preview, pictures, media,
  tasks, Export as HTML), math and Mermaid: `docs/notes/markdown.md`
  (traps inside).
- Before touching the LaTeX preview: `docs/notes/latex.md` (traps inside).
- Viewers in the editor's slot (images, PDFs, media, Quick Look, CSV, JSON,
  notebooks, SQLite, HTML pages, the compare view): `docs/notes/viewers.md`
  (traps inside).
- Before touching the terminal: `docs/notes/terminal.md` (traps inside).
- Git (the panel, the graph, sync, hunks, the review view, change bars):
  `docs/notes/git.md` (traps inside).
- The file tree: `docs/notes/tree.md` (traps inside).
- The inline agent, Request a Change, restarting, the Claude Code IDE link
  and the agent workspace: `docs/notes/agents.md` (traps inside).
- The browser pane (sessions, tabs, bookmarks, the address bar, find in
  the page): `docs/notes/browser.md` (traps inside).
- The control channel, `minicode-cli`, notes, tours and agents checking
  pages: `docs/notes/control.md` (traps inside).
- Processes and ports: `docs/notes/processes.md` (traps inside).
- Opening from Finder, the Dock and the command line:
  `docs/notes/opening.md`.
- Demo GIFs and the memory benchmark: `docs/notes/demos.md` (traps
  inside).
- The rest of the verification notes (harness traps, CI's runner, pixels,
  the GTK port by Docker or on the ThinkPad): `docs/notes/verification.md`
  (traps inside).
- The Linux port: `docs/notes/linux.md`, then `linux/PARITY.md` and
  `linux/HANDOFF.md`.
- The Android port: `docs/notes/android.md`, then `android/README.md`
  (traps inside).
- Current state (releases, the PPA, what needs the user's eyes):
  `docs/notes/current-state.md`.

## Verification reality (important)

How much of the GUI you can check depends on the port and on whether the
session has a display. Check first.

- **Core**: `make test`, `make test CXX=g++-16` for libstdc++, `make
  test-asan` (also with `CXX=g++-16`) for the sanitizers, and `make
  test-tsan` (clang; CI uses clang++-22 on Linux) for races.
- **The Mac, committed checks**: `make guitest [SUITES=...]` builds
  `build/guitest` (the whole app with main.mm's `main` renamed `minicode_main`,
  plus `mac/tests/gui/*.mm`) and runs it offscreen; the macOS CI job runs it. The
  harness (`mac/tests/gui/Harness.{h,mm}`) re-execs into a scratch world under
  `/tmp/mcgui-*` (HOME and CFFIXED_USER_HOME, ZDOTDIR with an empty .zshrc, a
  short TMPDIR for socket paths, settings with language servers off, a defaults
  suite, backups, a private pasteboard, git without global config), swizzles
  window ordering, activation (policy Prohibited), child windows, NSAlert
  (`MCGuiAnswerAlerts`), the pasteboard and `openURL:`, reports the test window
  as key and main, and plays suites (`MC_GUI_SUITE`, `MCGuiStep`, `MCGuiWait`,
  `MCGuiAsync`, `MCGuiPause`, `GCHECK`) from the run loop; it fails if any
  window reaches the screen. `MC_GUI_SNAPSHOTS=dir` saves pictures to look at,
  `MC_GUI_KEEP=1` keeps the scratch folder, `MC_GUI_SAMPLE=file` samples the
  keystroke suite and `MC_GUI_SAMPLE=<folder>` large_file's open and typing
  steps (`MC_GUI_FULL_PERF=1` runs foldperf on 100,000 lines, not
  20,000). The suites are listed in the next item. **Language server
  checks use the scripted server** `build/fakelsp` (`tests/fakelsp/fakelsp.cpp`, built by `make
  guitest`): `MCGuiFakeLsp(name, script, program, words)` writes a JSON
  script of rules and returns the command line for `lsp.<language>`, and
  `MCGuiFakeLspReceived` reads what the client sent; the file's header
  describes the script. basedpyright, where installed, runs as well in
  symbols, never instead. A suite named `_x` runs only when named, as a
  second app that `MCGuiRunChild` starts in the same world (backups SIGKILLs
  one). Wait for a condition; never pause before a positive check
  (`MCGuiPause` is for a negative check or where time is the condition, with
  a comment saying which; `MCGuiWaitQuiet` waits for a count to stop
  changing). **New user-visible behaviour gets a committed check here.**
- **The GUI suites**, by area (each an `MC_GUI_SUITE` in `mac/tests/gui`; the
  core's `docs:gui-suites` holds this list to them, both ways):
  editing: keystroke, multiedit, editor_keys, auto_close, bracket_pairs,
  folding, foldperf, multicaretclick, multicaret_rows, find_bar,
  large_file, save, paste, zoomfit, spell_check;
  files and windows: tree, tree_hidden, tree_focus, folder_picker, preview_tabs, split_editor, search,
  replace_in_folder, backups, window_restore, palette, symbols, keymap, config_writes,
  daily_work, structure, utf16, hints_panel, swatch_escape;
  language servers and formatting: language, lsp_fixes, lsp_sync,
  lsp_progress, lsp_watch, lsp_open_tabs, inlay_hints, semantic_tokens,
  snippets, completion_keys, problems, clangd_flags, servers_by_extension, server_consent, tools_consent,
  organize_on_save, format, format_consent, format_coverage, format_on_save;
  documents and viewers: markdown_preview, markdown_side,
  markdown_side_sync, markdown_width, markdown_page, markdown_math, mermaid, latex,
  previews, compare, html_page;
  terminal: terminal_draw, terminal_links, terminal_marks, terminal_paste,
  terminal_pty, terminal_reflow, terminal_splits, terminal_vt,
  terminal_exec, terminal_keys, terminal_zoom, terminal_host, run_file, test_gutter;
  git: git_panel, git_actions, git_hunks, git_verbs, git_review, worktree_review, git_conflicts, git_sync,
  git_gutter, line_history;
  agents and control: agents, agent_panel, agent_places, agent_task, agentcommands,
  agent_autosave, attention, control, browserpages, browser_tabs, browser_find, bookmarks,
  idelink, processes, session_web, session_browser;
  run only when named: _backups_writer, _backups_restore, _restore_writer,
  _restore_reader.
  A suite's file is usually its name in CamelCase; the exceptions are
  mermaid (IdeLinkSuite.mm), foldperf (FoldingSuite.mm), the format_*
  suites (FormatSuite.mm), git_actions, git_hunks, git_review,
  git_verbs, worktree_review and git_conflicts (GitPanelSuite.mm), markdown_side_sync (MarkdownSideSuite.mm), and
  LooseEndsSuite.mm (terminal_exec, terminal_keys, terminal_zoom,
  agent_autosave, multicaret_rows), and PageMeasureSuite.mm (hints_panel,
  markdown_width, swatch_escape).
- **Timing bounds hold under load** (2026-11-07): with several agents
  building, load 40 to 100 ran wall times three to ten times over and failed
  brackets:bounded, procs:speed, keystroke, large_file, processes and
  lsp_progress. Bound the CPU time of the thread doing the work: the core's
  `CHECK_TIME(cpuMs() - t0, bound)` (run_tests.cpp), the GUI's
  `GCHECK_TIME(MCGuiThreadCpuMs() - c0, bound, what)`; work on a queue
  measures its own thread (`MCProcessMonitor lastSampleCpuMilliseconds`); a
  main-thread stall is its CPU between two timer fires (lsp_progress), not
  the gap. Wall time stays only for the product's own wall-clock limits (the
  regex fallback's second). CPU time is not machine-proof either: on a
  loaded Mac a thread may run on an efficiency core (`taskpolicy -b`
  reproduces it), several times slower, so leave room for that or compare
  with a baseline taken on the same thread in the same step (large_file).
  `MC_SKIP_TIMING=1` passes every bound. **CI's room** (2026-11-14, both
  harnesses): CI runs the GUI checks with `MC_TIMING_SCALE=1.5` (a time
  between the bound and 1.5 times it passes with a `SLOW [suite]` line)
  and `MC_GUI_WAIT_SCALE=1.5` (every wait's timeout; a wait polls, so
  this only delays a failure). Every timing miss that passed on a rerun
  from November 6 to 14 was under 1.45 times its bound, every real
  regression 1.8 or more; CI's virtual Mac ran one terminal full draw at
  0.7 ms of CPU and, on another run, 9 ms. Set a bound for this Mac, not
  for CI, and where the work's cost follows the machine compare with a
  baseline from the same run (terminal_draw against its full draws). The
  end of a run lists the ten timing checks nearest their bounds (`NEAR`).
  A timing check goes through GCHECK_TIME, never a plain GCHECK, or
  neither the room nor MC_SKIP_TIMING applies.
- When a check fails, print the actual value first: most failures were wrong
  checks. Never call GUI behaviour verified when it was only compiled; say what
  needs the user (the look, real keys and mouse, trackpad, sound). Never
  activate the app from a test; ask before anything that shows a window.
- What a suite leaves (`LEAK`, `LEFT`), two harnesses at once, the harness
  traps, CI's macOS runner, ad hoc checks, pixels, and the GTK port from the
  Mac (Docker) or on the ThinkPad: `docs/notes/verification.md` (traps inside).

## What's next

See `ROADMAP.md`.
