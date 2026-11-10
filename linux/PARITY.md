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
  `meson compile -C build`, `meson test -C build`, and the GUI suites,
  `meson test -C build --suite gui`, from `linux/`.
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
-Dbrowser=enabled -Dpdf=enabled`.

The GUI checks (item 0.1) are `meson test -C build --suite gui` from
`linux/`, about 8 seconds in November 2026 (3 before the `keys` suite); `./build/minicode-guitest
<suite>...` runs some with their output as it comes. They start their own
Xvfb and session bus, so several agents can run them at once, and they put
nothing on the owner's screen. `HANDOFF.md` ("How to work on it" and the GUI
traps) has the rest.

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

Choices item 0.3 made where D1 left room, each by the rule:
- A command the port does not have yet has no row, so its key is free: the
  palette's Ctrl+Shift+P, Select Line's Ctrl+L, Select All Occurrences'
  Ctrl+Shift+L (item 1.9), Replace's Ctrl+H, the inline agent's Ctrl+I, Go
  to Line's Ctrl+G (VS Code's on Linux, so Find Next is F3 alone), and
  Close Tab's Ctrl+W (item 1.1).
- New Folder, the Mac's Shift+Cmd+N, yields Ctrl+Shift+N to New Window and
  takes Ctrl+Alt+N (New File's old key), as on the Mac New Window took
  Option+Cmd+N. Close Window is Ctrl+Shift+W, VS Code's Linux key and the
  Mac's Shift+Cmd+W.
- A Mac key with Option and Command and no VS Code key on Linux is Ctrl+Alt
  with the same letter: Show Hover is Ctrl+Alt+I (VS Code's is a chord,
  Ctrl+K Ctrl+I), and the task key Ctrl+Alt+L. Alt+C, markdown-all-in-one's
  task key on every platform, stays as on the Mac, in Markdown files only.
- Open Folder stays Ctrl+O: VS Code's Linux Open Folder is a chord, and its
  Ctrl+O opens a file, which minicode has no command for.
- Toggle Terminal is Ctrl+Shift+T and Ctrl+` without the Mac's Cmd+J: it is
  Global, and Ctrl+J would be taken from the shell, which ends a line with it.
- Ctrl+0, Ctrl+1 and Ctrl+Tab are Global, as the port's pane keys always
  were, so they leave the terminal. Ctrl+Tab was the registry's
  workbench.action.nextEditor titled Previous File until item 1.1 made it
  Next Tab.
- In the terminal Ctrl+Shift+C and Ctrl+Shift+V stay VTE's copy and paste,
  so the preview's Ctrl+Shift+V does not apply there; Ctrl+Shift+G and
  Ctrl+Shift+Space still go to the shell, as before.
- The tree's F2 and Delete are VS Code's explorer keys, on the tree's own
  controller, never the menu; on Linux keys.conf lets a tree command have a
  key that types elsewhere.
- Settings… and Keyboard Shortcuts… are in the Edit menu and Quit in the File
  menu, Reveal in Finder is Open Containing Folder. On Linux keys.conf refuses
  a line with cmd (a Mac's file) and a command the port does not have.

Choices item 1.1 made, by the same rules:
- Reopen Closed Tab is Ctrl+Alt+Shift+T and Save All Ctrl+Alt+S, the Mac's
  Option+Shift+Cmd+T and Option+Cmd+S with Ctrl+Alt for Option+Cmd: VS
  Code's Ctrl+Shift+T is Toggle Terminal here, and its Save All a chord.
- Next and Previous Tab are VS Code's Ctrl+Page Down and Ctrl+Page Up, with
  the Mac's Ctrl+Tab and Ctrl+Shift+Tab too, all Global, so they leave the
  terminal. Go to Tab is VS Code's Alt+1 to Alt+8, and Alt+9 or Alt+0 the
  last.
- Close Tab is VS Code's Ctrl+W and Ctrl+F4, not Global: in the terminal
  the shell has them (Ctrl+W deletes a word). With the browser's keyboard it
  hides the browser, and with no tab open it closes the window, as the
  Mac's Cmd+W. Unlike the Mac's, a held Ctrl+W is not yet ignored after the
  first close.
- Close Other Tabs, Close Tabs to the Right, Close All Tabs, Keep Open, Copy
  Path of Active File, Open Containing Folder of Active File, and Reveal in
  File Tree have no key (VS Code's are Ctrl+K chords). Previous File is
  gone; its Ctrl+Tab steps through the tabs.
- Tabs are not reordered by dragging, as the Mac's editor strip is not.

Choices item 1.6 made, by the same rules:
- The find bar's keys are VS Code's on Linux: Ctrl+H the replace row, F3
  and Shift+F3, Ctrl+Shift+1 Replace, Ctrl+Alt+Enter Replace All, and Alt+C,
  Alt+W, Alt+R, Alt+P and Alt+L for the options, which work from the editor
  as well as the bar, as in VS Code. Alt+L is Find in Selection's, as in VS
  Code, since the task key it took on the Mac is Ctrl+Alt+L here. Alt+C
  stays the task key in a Markdown file's editor.
- While the terminal has the keyboard, Alt with a letter alone is the
  program's (Meta: readline's word moves and case changes), unless Global;
  before, every key with Alt stayed the app's. Alt with a digit (the tabs)
  and Ctrl+Alt keys are unchanged.
- Replace in Folder is Ctrl+Alt+Shift+F, the Mac's Option+Shift+Cmd+F with
  Ctrl+Alt for Option+Cmd, as Reopen Closed Tab's: VS Code's Ctrl+Shift+H
  is the shortcut hints' (minicode's own Shift key). Ctrl+Shift+1 on a row
  replaces its match alone, VS Code's key in its search view.
- Select all matches (VS Code's Alt+Enter) waits for several carets (1.9).

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
all use it. It is `MultiEdit::apply` (`src/MultiEdit.h`), made in item 1.6.

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
Items 1.2 and 1.3 made it `sessions.json` (the core's `TabSessions`, one
object keyed by the folder's resolved path), and a backup's owner start is
the process's start in seconds since boot from `/proc`, not wall-clock time
as on the Mac, so a clock set forward or back never orphans a running
window's backups.

**D12. The registry builds the menu bar, and the palette reads the
registry.** Today `main.cpp` writes its `GMenu` and accelerators by hand. A
table in the port maps a command id to its GAction; a command with none on
Linux appears nowhere, and an action's enabled state decides whether the
palette offers it. Each item adds its rows to that table, so the menus, the
hints panel, the palette, and `README.md`'s Linux column cannot drift apart.

**D13. GUI checks.** An item's checks are suites in `linux/tests/gui`, one
file a suite and one line in `suites.txt` (merged by union, so parallel
branches do not clash). They use the harness's `MCGuiStep`, `MCGuiWait`,
`GCHECK`, and the rest, named as on the Mac so a Mac suite ports nearly line
by line. Where the input is what is checked, it is real input (`MCGuiKey`,
`MCGuiType`, `MCGuiClick`), not a call into the app. The app's state is read
through `tests/gui/AppSeam.h`: a suite that needs more adds a field there,
and nothing in `main.cpp` becomes global for a check. Timing bounds are CPU
time of the thread doing the work (`GCHECK_TIME`).

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

- [x] **0.1 A GUI harness.** `linux/tests/gui`, the counterpart of
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
  *Verified on the ThinkPad, 2026-11-09:* `save` passes and fails with
  `win.save` taken out of `main.cpp`; four runs at once pass, and nothing
  outlives a run, a hung step, a crash, or a SIGKILL to the wrapper. It puts
  nothing on the owner's screen, so it needs no one's eyes; CI runs it (0.2).
- [x] **0.2 CI.** The GTK job installs `xvfb` and `libxtst-dev` and runs the
  suites under its 20-minute limit. *Done means:* a deliberately failing check
  turns the job red on a branch. The checks also need `dbus-run-session`
  (Ubuntu's `dbus-daemon` package), which `--no-install-recommends` may
  leave out. Whether WebKit's sandbox (bubblewrap) starts on the runner is
  not known; the app makes a web view in every window.
  *Verified on GitHub, 2026-11-09:* the job installs `xvfb libxtst-dev
  dbus-daemon` and runs `meson test --suite gui -v`; a deliberately failed
  check turned it red with its FAIL line and a picture of the window kept as
  the `gui-checks` artifact (run 42709131059), and it is green without
  (42708024644, 42711062114), the whole job 5 to 7 minutes (4.5 before) and
  the checks 7 seconds of it. The sandbox starts: the runner restricts user namespaces as Ubuntu
  desktops do, but loads AppArmor's `bwrap-userns-restrict` profile, so no
  workaround is needed, and the `harness` suite now checks that the page
  answers from inside it. Its home page was a web search for "about:blank",
  online; it is now `about:blank` itself.
- [x] **0.3 The registry and keys (D1, D12).** Build `Commands` into the port,
  add `CommandsLinux.cpp` with core tests (each id exists, no key twice where
  places overlap, keys.conf applies), and drive the accelerators, the menu
  bar, and the hints from it. Move the keys D1 names and update the Linux column in `README.md`
  and `BUILD-LINUX.md`. *Done means:* a GUI suite presses every moved key
  through XTest, in the editor and with the terminal focused.
  *Verified on the ThinkPad, 2026-11-09:* `commands:linux-*` hold the table
  to the registry, D1 and README's Linux column; the `keys` suite presses
  every moved key and its old key in the editor, and with the terminal
  focused reads the bytes a raw `head` got for the plain Ctrl keys, then
  moves keys with a scratch keys.conf. It fails with the preview's row taken
  out of `CommandsLinux.cpp`, and with the terminal's rule broken. Needs the
  owner: the keys under GNOME on Wayland with IBus (Ctrl+Alt+L locks the
  screen on some desktops), and how the new menu bar reads.

### Stage 1: daily editing, 8,000 to 10,000 lines

1.1 goes first, alone. After it, items may run in parallel in worktrees, but
1.9 and 1.10 only after 1.4.

- [x] **1.1 Tabs (D2, D3).** `Document`, `EditorTabs`, a tab strip, the
  preview tab, closing and reopening, and Save All. Mac: `EditorTab`,
  `TabStrip`, `EditorController+Tabs`. Unsaved tabs are never dropped.
  *Verified in Xvfb, 2026-11-09:* `tabs:closed` holds the reopen list, and
  the `tabs`, `preview_tabs`, and `tab_viewers` suites press real keys and
  clicks: switching by the strip, Ctrl+Page Down and Up, Ctrl+Tab, and
  Alt+digits; each tab's text, caret, scroll, and undo; the dot gone once
  undone to the disk's text; Cancel, Don't Save, and Save on a dirty tab,
  and one question for a window's several; Reopen with its place, skipping
  a removed file; Save All; folder labels; the tab menu; preview tabs from
  the tree and Find in Folder; pictures, PDFs, LaTeX, and diffs in tabs; and
  the Markdown preview in a buffer of its own that leaves the source's undo
  alone. They fail with the old dirty rule and with typing not keeping a
  preview tab. A run by hand against clangd saw the squiggles follow the tab
  on show. Needs the owner: the strip's look, and Alt+digits and
  Ctrl+Alt+Shift+T under GNOME on Wayland.
- [x] **1.2 Sessions (D11).** A folder reopens its tabs. Mac: `MCTabSessions`.
  *Verified in Xvfb, 2026-11-09:* `tab-sessions:*` hold the file's format,
  the 50 folders and the merging of windows on one folder; in the `sessions`
  suite a second app opens a folder's files (a Markdown page, a named pipe,
  a 17 MB file, one deleted afterwards, the preview tab), moves the caret by
  keys and closes with Ctrl+Shift+W, and a third gets them back in order
  without the deleted one, the preview tab as one, the caret and scroll as
  they were, and only the tab on show read (never the pipe or the large
  file at launch); two windows on one folder share it, and one that closes
  drops out. It fails with the restore taken out of `newWindow`. Not done:
  the browser's page, which the Mac's session keeps too (after 1.13).
  Needs the owner: a logout under GNOME.
- [x] **1.3 Backups, hot exit, and auto-save (D11).** `Backups` over its POSIX
  `Fs`. Mac: `BackupStore`, `+Persistence`. Check with a real SIGKILL.
  *Verified in Xvfb, 2026-11-09:* in the `backups` suite a second app types
  into three tabs and is killed with a real `kill -9`; a third, started on
  another folder, opens a window on the first by itself and has every
  buffer back unsaved over the disk's text, nothing to undo, the note in the
  status bar, the question about the file changed meanwhile, and the
  deleted file's text; Ctrl+Q with Don't Save leaves no backup. The
  `auto_save` suite presses keys for after-delay, focus-change (Ctrl+0, a
  tab switch), Close Tab writing instead of asking, a file changed on disk
  left alone and named, one deleted not made again, a dead key composing,
  and off. They fail with backups never written, auto-save off, or any of
  its three guards taken out. Needs the
  owner: a real power loss, a crash or logout under GNOME, and focus-change
  with Wayland's focus and an IBus input method.
- [x] **1.4 Line numbers and the gutter (D4).** `LineIndex`, the
  current-line color, click and drag to select lines.
  *Verified in Xvfb, 2026-11-09:* `lineindex:utf8` holds the code-point
  index to a full one under 20,000 random edits; the `gutter`, `ruler`, and
  `gutter_typing` suites hold the numbers drawn to an index made afresh and
  to GTK's line positions after typing that adds and removes lines and a
  paste with a lone CR, U+2029, and U+2028, and check the active color
  following the caret, relative and off, the colors and `editor.opacity` as
  pixels, real clicks, drags up and down, and Shift+click selecting whole
  lines, no gutter in a Markdown preview, each tab's numbers and width, the
  dots from the squiggle tags, the change and fold lanes' APIs, the rulers
  at their columns' x as pixels, and typing in 100,000 lines (0.05 ms of
  gutter CPU a key; 8 to 9 ms a key for the main thread, with the gutter or
  without). They fail without the line-start rule, without the ruler pass,
  and with the index counted again per key. Needs the owner: how it looks
  (font, alignment, colors) and HiDPI. The window's divider takes presses
  in the gutter's first 14 px (`HANDOFF.md` item 16), for 1.13 to narrow
  before 2.10's change bars take clicks.
- [ ] **1.5 The palette (D12).** Commands, Quick Open, Go to Line, and
  file symbols without a server. `CommandPalette`, `SymbolOutline`. Mac:
  `PalettePanel`.
- [x] **1.6 Find and replace.** The find bar with case, word, regex, preserve
  case, and in selection; Replace in Folder. `TextFind`, `FolderSearch`.
  D5's function is `MultiEdit::apply` (HANDOFF.md item 18).
  *Verified in Xvfb, 2026-11-09:* `findrules:*` and `editspans:*` hold the
  core's rules (the seed, the count, the search in UTF-16 and code points,
  the changes, the colors against the Mac's table), and the `find_bar` and
  `replace_in_folder` suites press real keys: Ctrl+F seeded from a
  selection or the word at the caret, the options changing the count,
  a regex's groups in the replacement, Preserve Case, Find in Selection,
  Enter, F3 and their Shift forms wrapping, Replace and Replace All undone
  in one step, Escape from the bar and the editor; Replace All of 20,000
  matches in a 100,000-line file in about 170 ms of main-thread CPU and its
  undo in 1.6 s; the options' Alt keys going to a program in the terminal;
  Replace in Folder counting and asking, a clean tab changed in place, a
  tab with unsaved text left alone and named, a symlinked file replaced
  once, a stale row refused, and files to include and exclude. They fail
  without the primary selection fixes, without the selection taken away
  during MultiEdit's edits (a minute), without Document's undo hook, and
  with unsaved tabs not held back. Needs the owner: the bar's look in both
  themes, the keys under GNOME on Wayland with IBus (whether an input method
  takes the Alt letters), the primary selection on Wayland, AT-SPI's cost on
  a large Replace All, and Replace in Folder's window.
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
  Measured for R1 on main with tabs (2026-11-09, the main thread held by
  one `Editor::openFile`): a 15 MB C file of 300,000 lines, 0.6 s of CPU
  (0.2 s as .txt, so most of it `Document::startHighlighting`); a 5 MB
  file on one line, 1.5 to 3.0 s of CPU, and once 5.2 s long at load
  average 8, past GNOME's 5 s before "not responding". Before tabs these
  were 3.0 s and about 5 s of CPU.
- [ ] **1.13 The Mac's layout.** The owner's report, 2026-11-09: the browser
  should sit right of the editor with a drag bar between them, and the
  terminal below both, full width, as on the Mac; today the browser takes
  the editor's place. Closing the editor, the browser, and the terminal
  leaves an empty window, as on the Mac; today the port reopens the first
  one closed. Mac: "Editor, browser and terminal" in `../CLAUDE.md`'s UI
  map (`editorRect` and `browserRect`, `browserFraction`, 150 a side, a bar
  dragged within 16 points of an edge hides that side, the smallest top,
  `editorHidden`, `collapseRightAreaIfEmpty`, `revealEditor`), and
  `EditorController+Layout.mm`. After 1.1; it may run beside the others,
  but it owns the window's layout code in `main.cpp`.

*Done means, for each:* the feature's Mac GUI checks have Linux counterparts
in `linux/tests/gui`, pressing real keys.

### Stage 2: language and git, 6,000 to 8,000 lines

These may run in parallel. They need Stage 1's tabs (every open tab stays open
on its server) and D5. Item 1.1 left the session on the tab on show
(`LspSession::useBuffer`, re-pointed on every switch); keeping every tab open on
its server comes first here.

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
  `PreviewScroll`. Measured for R1 (2026-11-09): opening README.md in the
  preview holds the main thread for 0.3 to 0.6 s of CPU, the same without
  its pictures, while CLAUDE.md (twice the size, few tables) never holds it
  20 ms. The cost is GTK measuring the tables, each a `GtkGrid` of wrapping
  labels (`buildTable` in `Markdown.cpp`, about 540 cells in README). Its
  eight GIFs add about 130 ms of waiting on the image loader's process.
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

## Reported by the owner

Problems the owner hit in the port that no item above covers. Each is
fixed on its own branch, with a GUI check that fails without the fix.

- [x] **R1. "minicode is not responding" while clicking around the file
  tree** (2026-11-09, the PPA's 1.4.14). The tree's code is the same on
  main. Find the stall, fix it, and bound it with a check.
  *Found and verified on the ThinkPad, 2026-11-09:* the tree patched a
  folder's rows once per `GFileMonitor` event, through a sort model costing
  0.25 ms a change in a big folder, with GIO handing bursts over in one
  dispatch. Deleting the 30,000 files of an open folder held the main loop
  for 117 s, making them again for 36 s, and opening that folder cost 7.6 s
  of main-thread CPU in pieces. Now an event only asks for a listing
  (`TreeRefresh::Pacer`), made on a worker thread and put in as
  `TreeRefresh::splices` 8 ms a turn. The `tree_stall` suite (real clicks,
  a churning folder, 10,000 files deleted and made in an open folder) bounds
  each phase's longest stall at 500 ms of CPU: it measures 30 to 60 ms, and
  without the fix 18.2 s and 5.6 s. The owner's own folders hold nothing
  that large (`~/minicode`'s biggest folder has 363 entries), so their
  freeze was not reproduced as it happened. `~/minicode` opened read only
  in the harness, with a build churning an open `build/test-obj`, stalled
  at most 43 ms, except README.md's preview: 0.8 s of CPU, 2.1 s long at
  load average 8, from its tables (item 4.2). Ask the owner which folder
  was open, whether hidden files were shown, and what the click opened.
  Measured on the way, not fixed here: on main a one-line 5 MB file holds
  the main loop 1.5 to 3 s of CPU, 5.2 s long once on a loaded machine
  (item 1.12). Needs the owner: the tree under GNOME on Wayland while a
  build runs in an open folder.
- Right-clicking a part of the window offers no Adjust Settings (the same
  day): that is item 2.11.

## After each stage

Update `BUILD-LINUX.md` ("What is verified") and the list of what still needs
a person. Tell the owner what to try by hand, and ask whether to release.
