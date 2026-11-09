# Bringing the Linux port up to the Mac app

Written 2026-11-09, at release 1.5.17. The GTK port has what the Mac had in
September 2026; almost everything the Mac gained from 1.5.0 on is missing
from it. This file is the plan for that work, and its list of items is the
one place that says what is done. An agent starting on the ThinkPad needs
only this file and what it points to.

## Picking up an item

1. Read this file, then `linux/HANDOFF.md` ("How to work on it" and "Traps
   already known"). Read the sections of `../CLAUDE.md` for the feature you
   are taking: it describes the Mac, and the Mac is the specification.
2. Take the first open item of the earliest unfinished stage. Items in a
   stage may run in parallel only where the stage says so.
3. Mark it `[~]` with your branch name, in the branch.
4. Read the Mac's files for the item and its checks in `tests/gui`. What the
   owner reported and what a trap cost are in those files' comments and in
   `git log` for them; keep those behaviours.
5. Build it, check it as "Done means" says, mark it `[x]` with one line on
   what is verified and what needs the owner's eyes, and merge.

An item that turns out to be wrong or much larger than written: change this
file in the same branch and say why, rather than working around it.

## Working rules

These hold for every agent, in addition to `../CLAUDE.md`'s conventions.

- **Main always works.** The owner can run out of quota at any moment. Merge
  only verified work. Work on a branch in its own worktree; an unmerged
  branch is an acceptable leftover, a broken main is not.
- **Before each merge**, on the branch with main merged in, and with no
  warnings at `warning_level=2`: `make test` from the repo root,
  `meson compile -C build` and `meson test -C build` from `linux/`, and the
  GUI suites once item 0.1 exists.
- **A new GUI check must fail without its fix.** Take the fix out, see the
  check fail, and put the fix back.
- **Push after each merge, one at a time.** Wait for CI to pass on a push
  before the next. No merge to main while a job is red, except the fix for
  it. The Mac's fixes land on the same main, so pull first; a change to
  `src/` also has to pass the Mac's job.
- **Do not change the Mac's behaviour.** Core changes keep the Mac as it is,
  or are a fix with a test. There is no clang on the ThinkPad; CI's macOS job
  is the check.
- **Say what was not verified.** Never call GUI behaviour verified when it
  was only compiled. Real keys, input methods, Wayland focus, and how things
  look need the owner; list them under the item.
- **Never put a window on the owner's desktop without asking.** Use the
  offscreen display of item 0.1. `HANDOFF.md`'s Xephyr and Xwayland routes
  open a window, so ask first.
- **Kill only processes you started, by PID.** The owner runs the packaged
  minicode and works on this machine; `pkill -x minicode` has cost them
  unsaved work.
- **Writes outside the repo.** Never write to `~/.claude`. Ask before writing
  the owner's configuration, such as `~/.config/minicode`; checks use a
  scratch home.
- **Ask the owner before** a release, a PPA upload, or anything needing sudo.
- **No new dependencies** beyond what the distribution ships. No debugger:
  the owner decided against one.
- **Logic goes in `../src` with tests**, and the GTK code only shows it. A
  feature goes in its own files; `main.cpp` and `Editor.cpp` gain only hooks.
- **Prose**: plain sentences, Oxford commas, no emoji, and "minicode" in
  lowercase.

## Environment

The ThinkPad is Ubuntu 26.04 with GNOME on Wayland, 8 cores, GTK 4.22, VTE
0.84, and WebKitGTK 2.52. The checkout is `~/minicode`. The owner must install
what the offscreen checks need:

    sudo apt install xvfb xdotool imagemagick xcompmgr libxtst-dev

Then, once, from `linux/`: `meson setup build -Dterminal=enabled
-Dbrowser=enabled -Dpdf=enabled -Dwarning_level=2`.

**Releases** go at the end of a stage and only when the owner asks, with
`scripts/release.sh --linux X.Y.Z` from the ThinkPad, after the owner has
unlocked the signing key at the machine (`echo test | gpg --clearsign -u
EEF9635EDB7F9335 >/dev/null`). It shares its version numbers with the Mac, so
use the next number after the latest tag. It waits about 35 minutes for
Launchpad's build.

## Decisions

Settled so that agents do not each choose differently. "Try first" means do
a small throwaway test before building on it, and write the result here.

**D1. Keys.** The registry (`src/Commands.cpp`) is shared and the Linux keys
are a table of their own, `src/CommandsLinux.cpp`: command id to default keys
in the same notation. The Mac's rows do not change. The rule follows the
Mac's:
- A feature minicode shares with VS Code takes VS Code's Linux key.
- minicode's own Shift+Cmd key is the same letter with Ctrl+Shift, and does
  not move.
- minicode's own Cmd-only key is the same letter with Ctrl, unless VS Code's
  Linux keys use it for something else; then minicode's yields.
- While the terminal has the keyboard, plain Ctrl keys are the program's
  unless the command is Global (`updateShellKeys`), so a terminal command
  takes Ctrl+Shift, as VS Code's do.
- Keep away from what GNOME takes: Super, Ctrl+Alt+arrows, Ctrl+Alt+T, and
  Ctrl+Alt+Delete.
- The port's older keys that break this move once, in item 0.3: the palette
  gets Ctrl+Shift+P and the preview Ctrl+Shift+V; Ctrl+L selects a line and
  the task key goes to Ctrl+Alt+L; TODOs goes to Ctrl+Shift+D;
  Ctrl+Shift+L is Select All Occurrences; Ctrl+N is New File and
  Ctrl+Shift+N New Window; Ctrl+H is Replace; Ctrl+I is the inline agent once
  it exists.

No key is bound twice where the places overlap (`Commands::overlaps`).

**D2. One `GtkTextView`, one `GtkTextBuffer` per tab.** Split `Editor` into
the view and slot, and a `Document` holding what is per file: the buffer, its
path, undo, the highlighter and its mirror, dirty state, and disk stamp. All
buffers share one `GtkTextTagTable`. Switching tabs is
`gtk_text_view_set_buffer`, with scroll and caret kept per document. The tab
rules are `EditorTabs`, as on the Mac.

**D3. The Markdown preview gets a buffer of its own.** Today it overwrites
the source buffer and keeps `source_` apart. With a page buffer per document,
the source keeps its undo, carets, and highlighter state, and the side
preview needs nothing new.

**D4. A `GtkTextView` subclass does the drawing.** Carets, bracket boxes,
inlay labels, fold placeholders, and the ruler go in `snapshot_layer`. Line
numbers, folds, and change bars are a widget set with
`gtk_text_view_set_gutter`. Both APIs are in the installed GTK.

**D5. Edits with several ranges go through one function**, the counterpart of
the Mac's `replaceRanges:withStrings:actionName:`: one user action, last range
first, each range its own buffer edit, so the highlighter, folds, and
language server follow each one. Carets, rename, formatting, and agent edits
all use it.

**D6. Nothing is put in the buffer that is not in the file.** A child anchor
adds a character, which breaks every offset, the mirror, undo, and what the
language server sees.

**D7. Several carets: try first.** Keep them in `MultiCaret` as `GtkTextMark`s
and draw them in `snapshot_layer`, with the real insertion mark on the first.
Measure typing at 1,000 carets before the rest of the item.

**D8. Folding: try first.** An `invisible` tag over the hidden lines, so the
text never changes and the gutter, undo, and copy see it whole. Check that
carets step over it and scrolling stays right, with 50,000 fold regions. If
the tag cannot do it, stop and ask the owner before building an alternative.

**D9. Inlay hints: try first.** The Mac leaves room with a kern. Try a
`letter-spacing` tag on the character before the hint, with the label drawn in
`snapshot_layer`. If Pango will not leave the room on one side, leave hints
out and say so here.

**D10. The terminal stays VTE**, and `TerminalScreen` stays out of the build.
What the Mac learns from its own parser comes from VTE's termprops
(`VTE_TERMPROP_SHELL_PRECMD`, `_PREEXEC`, `_POSTEXEC`, and `CURRENT_DIRECTORY_URI`
are in the installed headers). Try first: whether the zsh integration can set
them with the nonce's protection. Features that need the marks (Run File's
busy check, the control channel's `term`) wait on that result.

**D11. Where state lives.** Sessions and the window's other remembered
state are JSON (`Json`) under `$XDG_STATE_HOME/minicode` (default
`~/.local/state/minicode`); backups under `$XDG_DATA_HOME/minicode/Backups`.
`MINICODE_BACKUP_DIR` overrides as on the Mac, and every one of these moves
with the harness's scratch home.

**D12. The palette reads the registry and the port's action table**, not a
menu: the port has no menu bar. A command with no action on Linux does not
appear. Each item adds its actions to that table, so the hints panel and
`README.md`'s Linux column follow.

## Left out on purpose

- Window blur: GTK 4 has no API and Mutter no protocol for it.
- Quick Look, Finder and Launch Services opening, codesigning, secure keyboard
  entry, the demo GIFs, and `make membench`: Mac-only, or not worth a second
  copy.
- A terminal grid of our own (D10).
- Request a Change and restarting into a build, unless the owner asks.
- A debugger, a third editor group, and a split down, as on the Mac.

## The work

Sizes are estimates of new GTK code. `[ ]` open, `[~] branch` under way,
`[x]` done.

### Stage 0: foundations, about 2,000 lines, one agent at a time

- [ ] **0.1 A GUI harness.** `linux/tests/gui`, the counterpart of
  `tests/gui/Harness.{h,mm}`: the whole app with `main` renamed, plus suites
  played as steps from the GLib main loop. A wrapper starts its own Xvfb and
  sets `GDK_BACKEND=x11` and `GTK_A11Y=none` under `dbus-run-session`, with a
  scratch `HOME`, `XDG_*`, `ZDOTDIR`, settings, and git config. The harness
  refuses to run on any display it did not start. Real input is XTest, called
  in process. There is no window manager, so if windows do not become active
  the harness calls `XSetInputFocus` on the window's id. Steps, waits, and
  checks read like the Mac's (`step`, `wait`, `check`, timed by the CPU of the
  thread doing the work). *Done means:* one suite opens a file, types through
  XTest, presses Ctrl+S through real accelerator matching, and checks the
  file on disk, and the same suite fails with the save action disabled.
- [ ] **0.2 CI.** The GTK job installs `xvfb` and `libxtst-dev` and runs the
  suites under its 20-minute limit. *Done means:* a deliberately failing check
  turns the job red on a branch.
- [ ] **0.3 The registry and keys (D1, D12).** Build `Commands` into the port,
  add `CommandsLinux.cpp` with core tests (each id exists, no key twice where
  places overlap, keys.conf applies), and drive the accelerators and hints
  from it. Move the keys D1 names and update the Linux column in `README.md`
  and `BUILD-LINUX.md`. *Done means:* a GUI suite presses every moved key
  through XTest, in the editor and with the terminal focused.

### Stage 1: daily editing, 8,000 to 10,000 lines

1.1 goes first, alone. After it, items may run in parallel in worktrees, but
1.9 and 1.10 only after 1.4.

- [ ] **1.1 Tabs (D2, D3).** `Document`, `EditorTabs`, a tab strip, the
  preview tab, closing and reopening, and Save All. Mac: `EditorTab`,
  `TabStrip`, `EditorController+Tabs`. Unsaved tabs are never dropped.
- [ ] **1.2 Sessions (D11).** A folder reopens its tabs. Mac: `MCTabSessions`.
- [ ] **1.3 Backups, hot exit, and auto-save (D11).** `Backups` over its POSIX
  `Fs`. Mac: `BackupStore`, `+Persistence`. Check with a real SIGKILL.
- [ ] **1.4 Line numbers and the gutter (D4).** `LineIndex`, the
  current-line color, click and drag to select lines.
- [ ] **1.5 The palette (D12).** Commands, Quick Open, Go to Line, and
  file symbols without a server. `CommandPalette`, `SymbolOutline`. Mac:
  `PalettePanel`.
- [ ] **1.6 Find and replace.** The find bar with case, word, regex, preserve
  case, and in selection; Replace in Folder. `TextFind`, `FolderSearch`.
- [ ] **1.7 Brackets.** `BracketMatch`, `BracketPairs`, and Go to Bracket.
- [ ] **1.8 Auto-closing pairs.** `AutoClose`, with VS Code's overtype and
  surround.
- [ ] **1.9 Line commands and several carets (D5, D7).** `MultiCaret`, Cmd+D's
  and Shift+Cmd+L's counterparts, column selection, move and copy lines, and
  VS Code's word separators.
- [ ] **1.10 Folding (D8).** `Folding`.
- [ ] **1.11 Getting around.** Editor zoom, word wrap toggle, Go Back and
  Forward (`NavHistory`), Maximize Pane.
- [ ] **1.12 Large-file mode.** `LargeFile.h`, at VS Code's thresholds.
  *Done means:* a 25 MB file opens and types within the Mac's bounds.

*Done means, for each:* the feature's Mac GUI checks have Linux counterparts
in `linux/tests/gui`, pressing real keys.

### Stage 2: language and git, 6,000 to 8,000 lines

These may run in parallel. They need Stage 1's tabs (every open tab stays open
on its server) and D5.

- [ ] **2.1 Rename, references, and quick fixes.** The all-or-nothing
  workspace edit applier. Mac: `Lsp+WorkspaceEdit`, `Lsp+Actions`, `LspPicker`.
- [ ] **2.2 Problems**, the second server, watched files, and Organize
  Imports. Mac: `ProblemsPanel`, `LspWatcher`.
- [ ] **2.3 Server and tool consent.** A folder's own `.venv`, `node_modules`,
  and project configuration are never run unasked. Mac: "Servers that run the
  folder's code" and "needs the user's yes" in `CLAUDE.md`. This goes in with
  or before 2.4.
- [ ] **2.4 Format Document.** `Formatter`, the server fallback, and format on
  save. Mac: `FormatRunner`, `Lsp+Formatting`.
- [ ] **2.5 Completion's additional edits and snippets.** `Snippet`,
  Tab and Return as on the Mac.
- [ ] **2.6 Semantic tokens.** Tags, painted for the lines on screen only.
- [ ] **2.7 Inlay hints (D6, D9).** `InlayHints`.
- [ ] **2.8 Install hints.** `InstallHints`.
- [ ] **2.9 Git verbs.** Push, pull, autofetch, Stage All, Discard with its
  Trash copy and undo, hunks and lines, the branch picker, commit verbs.
  `GitModel` already has the plans.
- [ ] **2.10 Change bars and the tree's git colors.** `QuickDiff`,
  `GitStatus`.
- [ ] **2.11 Themes from a menu and Adjust Settings.**

### Stage 3: agents, 8,000 to 10,000 lines

3.1 and D10's test come first. The rest may run in parallel.

- [ ] **3.1 The control channel and `minicode` commands.** `ControlProtocol`,
  a socket under `$XDG_RUNTIME_DIR`, 0600 in a 0700 folder. Mac:
  `ControlServer`, `ControlCommands`, `tools/minicode-cli.cpp`.
- [ ] **3.2 The compare view.** `SideBySide`, word changes, Accept and Reject
  per hunk.
- [ ] **3.3 The inline agent.** `InlineAgent`, with pending edits. Never run
  the real Claude Code in a check: use `tests/fixtures/claude`.
- [ ] **3.4 Notes and tours.** Mac: `AgentNotes`.
- [ ] **3.5 The Agents panel and Evidence.** `AgentTranscript`, `AgentIndex`,
  `AgentEvidence`, `AgentView`, with Linux's scratchpad path. Claude Code's
  data is read, never written.
- [ ] **3.6 Waiting for you.** `ClaudeHooks`, `AttentionBoard`.
- [ ] **3.7 Agents checking pages.** `PageCheck`, on WebKitGTK.
- [ ] **3.8 The Processes panel.** `ProcessTable` over `/proc`; its `Source` is
  the seam.
- [ ] **3.9 The Claude Code IDE link.** `IdeLink`.

### Stage 4: documents and layout, 5,000 to 7,000 lines

These may run in parallel.

- [ ] **4.1 Viewers.** CSV, JSON outline, notebooks, and SQLite, with the
  same read-only rules. `FileSniff`, `Csv`, `JsonOutline`, `Notebook`.
- [ ] **4.2 The preview to the side and scroll sync.** `MarkdownPage`,
  `PreviewScroll`.
- [ ] **4.3 Math, Mermaid, and Export as HTML.** `MathTex`, `MermaidDiagram`,
  `MarkdownHtml`.
- [ ] **4.4 LaTeX problems, warnings, and forward search.** `LatexLog`,
  `LatexScratch`.
- [ ] **4.5 Browser tabs, find in the page, and page zoom.** `BrowserTabs`.
- [ ] **4.6 The split editor.** `EditorTabs::restoreGroups`. With D2 the second
  group can be a second view on the same buffer, which is simpler than the
  Mac's mirror.
- [ ] **4.7 Terminal tabs and splits**, their own zoom, find, command marks
  (D10), and Run File. `TerminalLayout`, `RunFile`.

## After each stage

Update `BUILD-LINUX.md` ("What is verified") and the list of what still needs
a person. Tell the owner what to try by hand, and ask whether to release.
