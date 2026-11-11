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

Choices item 1.5 made, by the same rules:
- The palette is VS Code's Ctrl+Shift+P and F1, Quick Open its Ctrl+P, Go
  to Line its Ctrl+G, and Go to Symbol in File its Ctrl+Shift+O. None is
  Global: in the terminal Ctrl+P, Ctrl+G, and F1 are the program's
  (readline's previous line, Emacs's quit), as the Mac's grid keeps Ctrl+G,
  and Ctrl+Shift+P and Ctrl+Shift+O open the palette from there.
- Ctrl+T is VS Code's Go to Symbol in Workspace (item 2.12), a plain Ctrl
  key, so the program in the terminal keeps it (fzf's file widget).

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

Choices item 1.9 made, by the same rules:
- Ctrl+D, Ctrl+Shift+L, Ctrl+L, Ctrl+Shift+K, Alt+Up and Alt+Down are VS
  Code's Linux keys. Its Linux Copy Line keys, Ctrl+Shift+Alt+Up and Down,
  are GNOME's (move the window to the workspace above or below, beside
  Ctrl+Alt+Up and Down switching to it), so Copy Line is Shift+Alt+Up and
  Down, VS Code's key on Windows and the Mac's; that is VS Code's first
  Linux key for Add Cursor, which takes its second, Ctrl+Shift+Up and Down.
  The core's check of GNOME's keys now holds Ctrl+Shift+Alt+arrows too.
- All are editor commands, so in the terminal the shell keeps Ctrl+D and
  Ctrl+L. Move and Copy Line, which the Mac's registry keeps out of the
  menus as the text view's own, are in the Edit menu here (and so in the
  palette), at its end after Settings, as the moved items are.
- Ctrl+Home and Ctrl+End (with Shift) are rows of the Linux table, the
  registry's Fixed commands, with no action and no accelerator: GTK's own
  keys with one caret, Carets.cpp's with several, so they never leave an
  entry. Keys.cpp's table check passes a Fixed row without an action.
- Ctrl+Up and Ctrl+Down scroll a line (VS Code's scrollLineUp and
  scrollLineDown, which the registry has no row for), and Ctrl+Left,
  Ctrl+Right, Ctrl+Backspace and Ctrl+Delete move and delete by VS Code's
  words; with Ctrl+C, Ctrl+X, Ctrl+V, Escape and the find bar's Alt+Enter
  they act on the text view's own signals, and README's check counts them
  as the view's own keys.
- The mouse is VS Code's on Linux: Alt+click adds or removes a caret and
  Shift+Alt+click or drag makes a column from the caret; a middle-button
  drag makes a column and a middle click still pastes the primary
  selection. Alt+drag makes a fresh column, as the Mac's Option+drag (VS
  Code's adds a selection). The Mac's Cmd+Shift click pairs are not
  ported: Ctrl+click is Go to Definition.

Choices item 1.10 made, by the same rules:
- Fold and Unfold are VS Code's Linux Ctrl+Shift+[ and Ctrl+Shift+]. GTK
  matches them as the braces Shift makes on a US layout (`<Ctrl>braceleft`,
  through `linuxAccelerators`), so on a layout where Shift+[ is no brace
  they follow that layout's character.
- VS Code's other folding keys are Ctrl+K chords, which minicode has none
  of, so they are Ctrl+Alt with the chord's second key, as the Mac's are
  Control+Command with it: Fold Recursively Ctrl+Alt+[, Unfold Recursively
  Ctrl+Alt+], Fold All Ctrl+Alt+0, Unfold All Ctrl+Alt+J, Fold All Comments
  Ctrl+Alt+/, and Fold Level 1 to 7 Ctrl+Alt+1 to 7. GNOME takes none of
  them. Outside the editor Ctrl+Alt+0 is the terminals' zoom reset and a
  page's fit width (see the decision after 1.11's choices).
- Toggle Fold's would be Ctrl+Alt+L, the task key's, so it has no key; it is
  in View > Folding and the palette.
- All of them are editor commands, so while the terminal has the keyboard
  the program in it gets every one of these keys.

Choices item 1.11 made, by the same rules:
- The editor's zoom is VS Code's Linux Ctrl+= (Ctrl+Shift+=, Ctrl+plus,
  too) and Ctrl+-, and its reset VS Code's Ctrl+keypad 0 (Ctrl+0 is Focus
  File Tree; see the decision below for Ctrl+Alt+0). A page (a PDF or the
  LaTeX preview on show, or the browser with the keyboard) has the same
  keys for its own zoom, and Ctrl+Alt+0 for fit width (a web page: 100%),
  as the Mac's Page context; Ctrl+0 stays Focus File Tree there.
- The terminals' zoom has the same keys while the terminal has the
  keyboard, as on the Mac (and GNOME Terminal's Ctrl+= and Ctrl+-); they
  are in a new Terminal menu. A Terminal command keeps its plain Ctrl keys
  in the terminal (`linuxTerminalTakes`), since it applies nowhere else.
- Word wrap is VS Code's Alt+Z, an editor command, so Meta Z to the program
  in the terminal, as Option+Z is on the Mac.
- Go Back and Go Forward are VS Code's Ctrl+Alt+- and Ctrl+Shift+-. In the
  terminal Ctrl+Shift+- is the program's (readline's undo, Ctrl+_), as the
  Mac's grid keeps Ctrl+-; Ctrl+Alt+- still goes back from there.
- Maximize Pane has no VS Code key (its Toggle Maximize Editor Group is a
  chord), so the Mac's Ctrl+Cmd+M with Ctrl+Alt: Ctrl+Alt+M, which GNOME
  leaves alone, and which works from the terminal.

Choices item 2.9 made, by the same rules:
- View > Source Control's thirteen commands (Fetch, Pull, Push, Sync,
  Stage All, Unstage All, Discard Changes, Undo Discard, Switch Branch,
  Commit and Amend, Undo Last Commit, Stash, Pop Stash) have no keys, as in
  VS Code on Linux and on the Mac; the menu bar, the palette and the
  panel's right click have them.
- In the panel's lists a Ctrl click chooses another row, as the Mac's
  Command click; Shift click and Shift with the arrows as on the Mac. The
  Menu key and Shift+F10 open a row's menu, and the diff's.

Decided when items 1.10 and 1.11 met (the coordinator, 2026-11-10): both
gave the editor Ctrl+Alt+0, 1.10 to Fold All and 1.11 to the editor's zoom
reset (the Mac's Option+Cmd+0 with Ctrl+Alt). In the editor it is Fold
All's: it belongs to a family, Ctrl+Alt with the second key of VS Code's
Ctrl+K chords (Ctrl+Alt+1 to 7, Ctrl+Alt+J, Ctrl+Alt+/, Ctrl+Alt+[ and ]),
which D1's rule for chords gives, while the zoom reset has a VS Code Linux
key of its own, Ctrl+keypad 0, and keeps that, its menu item and the
palette. Ctrl+Alt+0 stays the terminals' zoom reset, the browser's page
zoom reset, and a PDF's or the LaTeX preview's fit width, places the
editor's commands never meet (`Commands::overlaps`; `Keys.cpp` binds a page
command only in a page and a terminal command only in the terminal).

Choices item 2.1 made, by the same rules:
- Rename Symbol, Find All References and Quick Fix are VS Code's Linux F2,
  Shift+F12 and Ctrl+.. They are editor commands, so in the terminal the
  program has all three; in the tree F2 stays Rename File's (the tree's own
  controller, the places do not overlap), and the keys suite now leaves a
  tree key out of the editor command's accelerators while the tree has the
  keyboard. Ctrl+. outranks GtkTextView's own emoji chooser key, as Ctrl+/
  outranks its Select All.

Choices item 2.10 made, by the same rules:
- Go to Next Change and Go to Previous Change are VS Code's Alt+F5 and
  Shift+Alt+F5. GNOME's own keys give Alt+F5 to Restore Window
  (org.gnome.desktop.wm.keybindings unmaximize, Ubuntu's default too; the
  owner's desktop has it turned off), but every VS Code platform has
  Alt+F5, so there is no other VS Code key to yield to, as Copy Line
  yielded to Shift+Alt+Up: the key is kept, and on such a desktop the
  Navigate menu, the palette or keys.conf runs the command. They are editor
  commands, so in the terminal the program has both. Revert Change and
  Stage Change have no key, as on the Mac and in VS Code, and are in View >
  Source Control.

Choices items 2.3 and 2.4 made, by the same rules:
- Format Document is VS Code's Linux Ctrl+Shift+I (the Mac's Shift+Option+F
  is VS Code's Mac key, and Shift+Alt+F its Windows key). It is an editor
  command, so in the terminal the key is the program's. GTK's inspector
  takes Ctrl+Shift+I only when it is turned on (`GTK_DEBUG=interactive`,
  or GTK's enable-inspector-keybinding setting), which it is not by
  default.
- Format Selection is Format Document with lines selected, as on the Mac:
  VS Code's Linux Format Selection is a chord (Ctrl+K Ctrl+F), which
  minicode has none of.
- Ask Again About This Folder's Tools has no key, as on the Mac, and is in
  the Edit menu with Settings, since the port has no minicode menu.

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

*Tried on the ThinkPad, 2026-11-10 (item 1.9's branch; a throwaway suite
putting a character at 1,000 carets through `MultiEdit::apply` in a
100,000-line Python file, main-thread CPU, load 4 to 8 unless said).*
**Reasonable, once the highlighter takes such an edit as what it is.** The
carets are character offsets, not marks: the core says where each goes
after an edit, and any edit or caret move not made by the carets ends them
(as on the Mac), so nothing has to follow edits. Marks cost about as much
(a mark each changed an edit at 1,000 carets by noise, moving 1,000 of
them took 2 to 6 ms, reading them 0.2 to 1 ms) and bought nothing.

What a character at 1,000 carets cost, the edit itself and then the main
thread until idle again (GTK laying out the changed lines, the lines off
screen retagged in idle time):
- Before: a column of 1,000 lines, 61 to 96 ms and 79 to 122 ms; carets
  one line in a hundred over the whole file, 115 to 143 ms and 1.0 to
  1.2 s. A user action of more than 64 edits made `Document` read the
  buffer back whole (55 ms for 3.5 MB) and take the change as one edit
  from its first range to its last: every line between re-lexed (110 ms
  for the spread carets) and retagged, the rest in idle time. A plain
  text file, no highlighter: 18 to 24 and 40 to 48 ms for the column, 23
  to 79 and 46 to 131 for the spread.
- After: 22 to 33 ms and 51 to 70 ms for the column, 22 to 31 ms and 129
  to 156 ms for the spread; the plain file 12 to 26 and 31 to 73. The
  changes of a `MultiEdit` of 8 ranges or more are taken in bytes at
  begin-user-action, before the buffer changes; at its end the new mirror
  is made from the old in one pass and the highlighter takes the edits
  together (`IncrementalHighlighter::editMany`, the core's `EditBatch.h`,
  `editbatch:*`), each re-lexing its own lines, the line table moved once
  (edit() of each in turn moved 50 million line starts for 1,000 carets);
  only the lines re-lexed are retagged, those on screen at once and the
  others in idle time.
- Through real keys (`multicaret_perf`, Ctrl+Shift+L on 1,000 places, five
  letters then a quote that opens a string on each line): 35 to 55 ms a key
  in the edit, 100 to 200 in all; the quote 30 ms in the edit (and as much
  again in the auto-closing rules), about 210 in all. Without the batch,
  195 ms a key in the edit and 650 in all, the quote 2.3 s.
Drawing costs nothing that grows with the carets: only those on screen are
placed (a few `gtk_text_view_get_cursor_locations` a frame).

Found on the way, not the carets': while the caret blinks GTK 4.22 draws the
text view every frame, for seconds after any key press (Shift too), about
1.7 ms a frame and 100 ms of CPU a second at 100,000 lines; a measure that
caught it put half a second on one key. `multicaret_perf` turns the blink
off for its measures.

**D8. Folding: try first.** An `invisible` tag over the hidden lines, so the
text never changes and the gutter, undo, and copy see it whole. Check that
carets step over it and scrolling stays right, with 50,000 fold regions. If
the tag cannot do it, stop and ask the owner before building an alternative.

*Tried on the ThinkPad, 2026-11-09 (item 1.10's branch; a throwaway GUI
suite in the real editor, real keys through XTest, CPU time of the main
thread, the machine at load 8 to 25).* The tag hides whole buffer lines,
from the start of a region's second line to the start of the line after
it, so GTK gives those lines no height. **With many small folds it does
the job; with many lines hidden on screen it does not, and that cannot be
fixed from outside GTK. Waiting on the owner.**

What works, on 100,000 lines of `def` and one body line (50,000 regions):
- Fold All is one `apply_tag` per outermost region: 180 to 300 ms for
  50,000, the first frame 0.3 s after the start. GTK then measures the
  lines again in idle slices, 1.7 to 2.3 s of CPU (opening the file costs
  5 to 7 s that way). Unfold All is one `remove_tag` over the text: 50 to
  100 ms, then 2.2 to 3.1 s of idle measuring.
- Right and Left step over a fold (end of the folded line to the start of
  the line after it, and back); Up and Down step over it; Page Down lands
  on a shown line, on screen (65 to 70 ms; 175 to 190 ms with 5,000 folds
  of 20 lines); 20 wheel clicks take 17 to 28 ms; Ctrl+End lands on
  screen (45 to 70 ms). The scroll range shrinks to the shown lines.
- The gutter numbers only the shown lines (1 3 5 7 ...), each right.
- The tag is not in the undo history: Ctrl+Z after Fold All takes back
  typing and the lines stay hidden.
- Text typed at the end of a folded line, or at the start of the line
  after a fold, is not tagged (GTK extends the tag at neither end).
- Typing among the folds: 12 ms a key against 8 without folds (10 keys);
  21 against 10 with 5,000 folds of 20 lines. The Mac's bound is 50.

What would need code of our own, and could have it:
- Copy leaves the hidden text out: GTK's clipboard writes the selection
  without invisible text ("def f0(x):\ndef f1(x):\n" for two lines across
  a fold). A copy handler would write the text with it. GTK's own copy is
  slow for a large selection anyway, folds or not (1.6 s of CPU for
  20,000 lines: it copies into a scratch buffer with every tag).
- Return at the end of a folded line puts the break before the hidden
  lines; the Mac's rule (a line below the fold) needs the key caught.
- The gutter walks every line between the first and last on screen, the
  hidden ones too (about 12 µs a hidden line a frame); it could jump over
  folds by their ends.

What fails: **every frame costs about 6 µs for each hidden line between the
first and last line on screen**, in GTK's own drawing. One redraw of the
text alone took 26 ms with 5,000 hidden lines on screen and 120 ms with
20,000; typing there cost 7.5 ms and 37 to 42 ms a key. With one fold of
100,000 lines on screen (with our gutter's walk on top) the first frame took
1.2 s, Page Down 2.4 s, 20 wheel clicks 1.1 s, Ctrl+End 1.8 s, and the main
thread spent 12 s of CPU before it was idle; selecting across it with
Shift+Down and pressing Ctrl+C held the main loop past the harness's
one-minute limit (GTK's copy alone, 8.6 s). Such a screen is ordinary: Fold
All in any long file wrapped in one block (a JSON object, a namespace, a
class, a Markdown document under one heading), or in a long file of a few
dozen big functions, whose folds all fit on one screen. The count of regions
does not matter; the lines hidden between the first and last shown line do.
The cause, read in GTK 4.22's source: `gtk_text_layout_snapshot` visits
every `GtkTextLine` from the first to the last in the clip and asks each for
its line display, and the display cache holds three screens of lines
(`SCREEN_HEIGHT / line height * 3` in `gtk_text_view_size_allocate`), so
every hidden line's display is made again every frame. Neither is public
(`gtk_text_layout_set_mru_size` is not exported), and a cap on one fold's
size would not bound it, since forty folds of 2,500 lines on one screen hide
100,000 lines between them.

The owner's choice, none of it built: accept the tag as it is (large folds
slow: 0.12 s a frame at 20,000 hidden lines on screen, and by the same rate
about 0.6 s at 100,000); or an alternative to the tag, which D8 says to ask
about first (for example a second buffer per tab holding only the shown
lines, which breaks D2's one buffer and every offset, or a GTK patch
upstream so the drawing skips lines of no height, which would take a release
to reach Ubuntu). The throwaway suite is not committed.

*Decided, 2026-11-10 (the owner left it to the coordinator):* the tag, with
a budget. The folds closed in a tab hide at most 5,000 lines between them,
where the try measured 26 ms a frame and 7.5 ms a key at worst; a fold that
would go past it stays open, with a note in the status bar saying why, and
Fold All, Fold Level N, and Fold Recursively close what fits (choosing
which by a rule written here when it is built) and say how many stayed
open. The copy handler, Return after a folded line, and the gutter's jump
over folds are part of the item. If GTK ever skips lines of no height when
it draws, the budget can grow.

*Built, 2026-11-10 (item 1.10), with this rule for what fits:* Fold All,
Fold Level N, Fold Recursively and Fold All Comments take the regions
outermost first and, at each depth, from the top of the file down. A region
whose lines would take the tab past 5,000 hidden lines stays open, and the
regions inside it get their turn at the next depth; a region inside a fold
already closed hides nothing more and closes too. So the top of a long file
folds as it would without the limit, and further down the big regions stay
open with their insides folded where they fit. The status bar says "Folded
5,000 regions; 15,000 stayed open, since this tab's folds may hide 5,000
lines at most". A single fold (Ctrl+Shift+[, a chevron) that would pass the
limit stays open with "Not folded: this tab's folds would hide 5,003 lines,
and 5,000 is the most they may hide, since GTK draws hidden lines too". The
rule is the core's `Fold::pickWithinBudget` (`fold:budget`). Measured in the
checks at the limit, the ThinkPad at load 10 to 30: a frame of the text with
5,000 hidden lines on screen 11 to 20 ms of CPU (0.3 with nothing folded),
typing among them 12 to 26 ms a key, Fold All of 20,000 regions 16 ms and
Unfold All 6 ms, then 0.5 to 0.7 s of GTK measuring lines again in idle
slices.

**D9. Inlay hints: try first.** The Mac leaves room with a kern. Try a
`letter-spacing` tag on the character before the hint, with the label drawn in
`snapshot_layer`. If Pango will not leave the room on one side, leave hints
out and say so here.

*Tried on the ThinkPad, 2026-11-11 (a standalone GTK 4 probe, GTK 4.22.4, Pango
1.57.0, on a private Xvfb, a monospace 14 px font; positions are
`gtk_text_view_get_iter_location` and ink columns read from a rendered
picture).* **Not workable: Pango splits the room, so hints stay out of the
Linux port.** A `letter-spacing` of L on one character puts L/2 before the
glyph and L/2 after it, never all after: with L = 20 on the `x` of
`foo(x, y)` the `x` is inked 10 px right of where it was (45 to 50, not 35
to 40) and the next character 20 px right. The room is also not where the
buffer says: `get_iter_location` for the tagged character still answers the
old left edge (34), which is now the start of the left half, so the glyph's
own box begins 10 px inside it. A color tag on the next character changes
nothing. Before a line break only the right half is trimmed (the last
character of `abc` moved 10 px and the line end 10 px). The property takes
0 and up (-20 and -10 are refused with a GLib critical), so the one thing
that could cancel the left half, a negative value on the character before
or the one after, does not exist; a positive value on a neighbour only adds
another hole beside it, and tagging the character before and the one after
makes the gap between them L with a hole of L/2 on each outer side, twice
the width for the room wanted. Nothing in `GtkTextTag` shapes a run or
makes room on one side (an anchor or paintable would put U+FFFC in the
buffer, against D6). The caret follows the logical box and not the
hint: at the offset after the tagged character it draws at 62 (right of the
room), at the offset of the character itself at 33 (left of the hole), and
a click scan put 34 to 47 on offset 4 leading, 48 to 62 on 4 trailing and
64 on 5, so a click on the left half of the room lands before the
character, which the Mac's rule forbids, and the hole before the character
stays visible. A hint at a paragraph's start is the one case that works: a
tag with `indent` of 20 on the paragraph's first character moved only the
first display line (x 20, wrapped lines 0, the next paragraph 0), where
`left-margin` moved every line; a hint at a wrapped line's start has no such
tag. Cost, 300 tags over 2,000 lines (main thread CPU): 2.2 ms to apply, 1.3
to remove, but the relayout that follows was 137 ms after applying and 192
after removing. Drawing the label in `snapshot_layer` itself is simple (a
rounded rectangle and a scaled `PangoLayout` at the iterator's rectangle in
the ABOVE_TEXT layer, saved as a PNG through `GtkWidgetPaintable`) and was
not the problem. Paragraph-start hints alone would be a patchy half of the
feature, so item 2.7 leaves inlay hints out until GTK or Pango can leave
room on one side.

**D10. The terminal stays VTE**, and `TerminalScreen` stays out of the build.
What the Mac learns from its own parser comes from VTE's termprops
(`VTE_TERMPROP_SHELL_PRECMD`, `_PREEXEC`, `_POSTEXEC`, and `CURRENT_DIRECTORY_URI`
are in the installed headers). Try first: whether the zsh integration can set
them with the nonce's protection. Features that need the marks (Run File's
busy check, the control channel's `term`) wait on that result.

*Tried on the ThinkPad, 2026-11-11 (a standalone GTK 4 and VTE probe, VTE
0.84.0, on a private Xvfb, bash printing the sequences a zsh hook would;
zsh is not installed on the ThinkPad, so the hooks themselves were not
run).* **Partly workable: a nonce-checked state mark works; per-event
reporting and marks at their rows do not.** VTE does
not parse OSC 133 or 633 at all: `133;A`, `B`, `C` and `D;n` changed no
termprop and fired no signal, so `vte.shell.precmd`, `.preexec` and
`.postexec` (`VTE_TERMPROP_SHELL_*`) can come only from the termprop OSC,
`OSC 666 ; <name>=<value> ST`. It needs ST (ESC `\`); with BEL nothing
happened. An app installs its own with `vte_install_termprop("vte.ext.<app>.<name>",
VTE_PROPERTY_STRING, flags)` before the first `VteTerminal` exists (a name
without the `vte.ext.` prefix is refused with a VTE-CRITICAL and -1; at
least four components), and a string value arrives whole, so `mc=<nonce>`
can ride in it and the app ignores any value without it with a string
compare. The signal is `termprops-changed` (property ids; read the value by
id). **It is not an event stream.** A plain termprop set to the value it
already has fires nothing (P then P: one signal); an ephemeral one fires
each time; but only the last value of a read's parse can be read: A, B, C
in one write gave C, three equal marks gave one, 200 in one write gave the
200th. Thirty precmd/preexec/postexec triples printed back to back as
separate small writes gave one signal; with 1 ms between writes 66 of 90
arrived. The signal comes after the chunk is parsed, so the cursor row read
in the handler is the row at the chunk's end (a mark printed at row 16
read 18, and an earlier mark in the same write was lost): marks cannot be
placed on screen by row this way. **Forgery:** the termprop OSC is open to
any program: `printf '\e]666;vte.shell.postexec=7\e\\'` set
`vte.shell.postexec` to 7 and fired the signal (a forged valueless `precmd`
and `vte.cwd=...` did nothing), so the built-in shell termprops can never be
trusted, and only a `vte.ext.<app>` string carrying the nonce is
checkable. A forged plain OSC 133 changes nothing, since VTE ignores it.
**The nonce is readable by the user's own programs, as on the Mac:**
after `unset MINICODE_NONCE` it is still in `/proc/<pid>/environ` for any
same-user process, as the Mac's shell's exec-time environment is through
`KERN_PROCARGS2` (`ps -E`). That is within the nonce's purpose, which is
to refuse marks that are only printed (a file shown with `cat`, a remote
shell, a program echoing what it read), not code already running as the
user; a 0600 file the .zshenv reads and the app deletes, or an inherited
descriptor closed before user code runs, would hide it better (not
tried). `CURRENT_DIRECTORY_URI`
(`vte.cwd`) works as is from OSC 7: `file://host/tmp/a%20b` came back as
that URI at once, and the termprop OSC cannot set it.

**Recipe, if it goes ahead:** the zsh hooks print `ESC ] 666 ;
vte.ext.minicode.mark=p=<n>;x=<n>;d=<n>;s=<status>;c=<command>;mc=<nonce>
ESC \`, where p, x and d count precmd, preexec and finished commands (a
lost mark shows as a jump, and the value is never equal twice); the app
installs the string termprop at startup, drops any value without its
`mc=`, and takes busy (x greater than d), the last status and the command
text from the newest value only. Kept: busy state, the cwd. Lost: each
command's own status when several finish inside one read, and marks at
rows. Run File's busy check and `term` can live with that; command marks
cannot.

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
  (font, alignment, colors) and HiDPI. The window's divider took presses
  in the gutter's first 14 px; 1.13 narrowed it to none over the editor
  (`HANDOFF.md` item 18), and the `gutter` suite's change bar click is now
  13 px from the edge, inside the old reach.
- [x] **1.5 The palette (D12).** Commands, Quick Open, Go to Line, and
  file symbols without a server. `CommandPalette`, `SymbolOutline`. Mac:
  `PalettePanel`.
  *Verified in Xvfb, 2026-11-09:* the `palette` and `symbols` suites press
  real keys and clicks. Ctrl+Shift+P and F1 list each command with an
  action once, "Menu > Title" with its key, leaving out disabled ones
  (Export PDF until a LaTeX file shows, the zoom keys); "tog term" and
  "hidden" rank first, Return runs them on the next turn (an app action
  too), and the command comes first next time, from
  `$XDG_STATE_HOME/minicode/palette.json`. Escape, a click outside (which
  goes no further), and running a row give the keyboard back, the find
  bar's selection included; Ctrl+0 closes it, leaving the keyboard in the
  tree. Ctrl+P lists the folder off the main thread by Find in Folder's
  rules and opens a file by Return or a click, tabs and earlier picks
  first; Ctrl+G goes to "120:6" and "42,7"; in the terminal Ctrl+P, Ctrl+G,
  and F1 reach the program and Ctrl+Shift+P opens the palette; a window
  closes cleanly while its palette lists. Ctrl+Shift+O lists the outline
  with the caret's symbol chosen, previews each while the field keeps the
  keyboard, and Escape puts selection and scroll back; "@:" groups by
  kind; a Markdown page scrolls to its headings. Each check failed without
  its fix (the enabled filter, the selection, the putting back, the worker
  thread). Needs the owner: the look, F1 and Ctrl+Shift+O under GNOME with
  IBus, and Quick Open on a large real folder. The server's symbols and Go
  to Symbol in Workspace came with item 2.12.
- [x] **1.6 Find and replace.** The find bar with case, word, regex, preserve
  case, and in selection; Replace in Folder. `TextFind`, `FolderSearch`.
  D5's function is `MultiEdit::apply` (HANDOFF.md item 20).
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
- [x] **1.7 Brackets.** `BracketMatch`, `BracketPairs`, and Go to Bracket.
  *Verified in Xvfb, 2026-11-09:* `bracketpairs:utf8` and
  `autoclose:utf8-context` hold the port's UTF-8 highlighter, in bytes and
  code points, to the Mac's UTF-16 results; the `brackets` suite moves the
  caret by real keys and checks the pair boxed (the bracket before the caret
  first), the boxes as drawn and as pixels of `editor.bracket-match`,
  strings and comments skipped, nothing read away from a bracket, no
  partner past 20,000 characters, plain text, no boxes in a Markdown page,
  and Ctrl+Shift+\ out and back and from inside a pair, its key the
  program's in the terminal; `bracket_pairs` checks the three depths and
  the unexpected closer as tags and pixels, lines painted only as they come
  on screen (wheel, Ctrl+End, a jump), a typed ( at the top repainting only
  the lines on screen and the rest once seen, Ctrl+Z, off by its key, none
  in plain text, an open of 30,000 lines (46 ms of CPU against 36 without
  colors; 81 against 64 under load) and typing in 100,000 lines (7 to 9 ms
  a key either way). They fail with the
  strings not skipped and with an edit's change of depth not followed.
  Needs the owner: the colors and boxes on a real screen and theme, HiDPI.
  Large-file mode (1.12) should turn the boxes off as the Mac does; the
  colors go with the highlighter.
- [x] **1.8 Auto-closing pairs.** `AutoClose`, with VS Code's overtype and
  surround.
  *Verified in Xvfb, 2026-11-09:* the `auto_close` suite types every key
  through XTest: print( closing with the caret inside, the typed ( going
  into the buffer last with the caret after it (what the language server
  reads), one Ctrl+Z taking the pair and Ctrl+Shift+Z putting it back, )
  typed over, Backspace taking the pair, a pair typed by hand or let go
  staying, the closers let go when the caret leaves or the pair spans lines,
  Python's quotes (a word's apostrophe, f', the docstring's third quote), a
  selection wrapped and still selected, Markdown closing brackets and not
  quotes and wrapping in * and _, nothing while a dead key composes, a quote
  an input method commits typed as it is, the setting off typing plainly
  and still wrapping, and a ( in 100,000 lines (2 to 7 ms a key, against 2
  plainly). It fails with the hook taken out, with the composing guard
  taken out, and with the caret placed over a selection (GTK then lost the
  selection a moment later). Needs the owner: IBus and other input methods,
  and keyboard layouts where brackets need AltGr. Several carets come with
  1.9 (`AutoCloseInput`'s calls take carets).
- [x] **1.9 Line commands and several carets (D5, D7).** `MultiCaret`, Cmd+D's
  and Shift+Cmd+L's counterparts, column selection, move and copy lines, and
  VS Code's word separators.
  *Verified in Xvfb, 2026-11-10:* `editbatch:*` hold the highlighter's
  batch (D7) to edit() of each change in turn and to a fresh lex, and
  `commands:linux-*` the keys (D1's choices above). The `multicaret`,
  `multicaret_select`, `line_commands`, `editor_words` and
  `multicaret_perf` suites press real keys and clicks: Alt+click adding and
  taking away carets, drawn (pixels) with their lines active in the
  gutter; typing, Backspace, Return keeping each line's indentation, and
  brackets closing, typed over and deleted in pairs at every caret, one
  Ctrl+Z taking a key back from all; Shift+Alt, Alt and middle-button
  drags making columns (their selections as pixels), a middle click still
  pasting the primary selection, Shift+Alt+click from the caret; Copy at
  several selections and Paste of as many lines as carets one at each;
  the arrows, Home, End, Ctrl+End and Ctrl+Shift+Home; the find bar's
  Alt+Enter; Ctrl+D (not "arrays" for "arr"), Ctrl+Shift+L, Ctrl+Shift+Up
  and Down, and the 10,000 limit with its note; Ctrl+L, Ctrl+Shift+K,
  Alt+Up and Down and Shift+Alt+Up and Down at the edges, an emoji moved
  whole; whole lines copied and cut and pasted above, other text pasted at
  the caret; double-clicks and their drags by VS Code's words (arr.size,
  $el, value_2), a triple click, Ctrl+Left and Right, Ctrl+Backspace and
  Delete, a Markdown page's double-click left alone, Ctrl+Up and Down
  scrolling; and five letters and a quote typed at 1,000 carets over
  100,000 lines (35 to 55 ms a key in the edit, 100 to 200 in all; the
  colors held to a fresh lex after). They fail without the batch (195 and
  650 ms a key), without the word selection, and before the keys existed.
  Needs the owner: the carets and their selections on a real screen and
  HiDPI, the keys under GNOME on Wayland with IBus (whether an input method
  takes Alt+click or Shift+Alt drags; text it composes goes in at the first
  caret only), a real middle button and touchpad, and how the Edit menu
  reads with Move and Copy Line at its end.
- [x] **1.10 Folding (D8).** `Folding`. D8's try (2026-11-09) found the
  `invisible` tag right for carets, scrolling, the gutter, and undo with
  50,000 folds, but every frame costs about 6 µs per hidden line on screen
  inside GTK; built on the tag with D8's budget of 5,000 hidden lines.
  *Verified in Xvfb, 2026-11-10:* `fold:budget` holds the limit's rule, and
  the `folding`, `fold_budget`, and `foldperf` suites press real keys and
  clicks: Ctrl+Shift+[ folding the innermost region and then the next one
  out, with the text and undo untouched; the gutter numbering only the lines
  on show, each at GTK's own line, and visiting none of the hidden ones (30
  lines walked for 30 rows with 5,000 hidden on screen; 5,030 without the
  jump); Right, Left, Up, and Down stepping over a fold; typing above a fold
  and at the end of its line moving it, and undo putting it back; Shift+Down
  and Ctrl+C across a fold copying what it hides; Go to Line and Find
  opening what hides their target; Ctrl+Alt+0, Ctrl+Alt+J, and Ctrl+Alt+2;
  a chevron's click folding and unfolding, a click on the numbers still
  selecting the line; a click on the placeholder opening it; Backspace at
  the start of the line after a fold and Delete at the end of a folded line
  opening it and deleting nothing; Return at the end of a folded line making
  a line below the fold, and its undo leaving it closed; an edit inside
  opening it; folds per tab, kept with the source while its Markdown page
  shows (a page of its own, D3), and forgotten on a reload from disk; a long
  line's own hidden part left alone by folding it; none in large-file mode;
  in the terminal no folding key bound; and the limit (Fold All closing what
  fits and saying how many stayed open, a fold past it left open with the
  note, then folding once room is made), a frame and a keystroke at the
  limit, and Fold All and Unfold All of 20,000 regions (50,000 with
  `MC_GUI_FULL_PERF=1`), each within a CPU bound (D8 has the timings). They
  fail without the gutter's jump, without Return's move below the fold, and
  without the limit. The language server's foldingRange came with item
  2.13. Needs the owner: how the placeholder and chevrons look in both
  themes, the keys on a real keyboard (Ctrl+Shift+[ on layouts where Shift+[
  is no brace), a touchpad's scrolling past folds, and how GTK's measuring
  of thousands of lines after Fold All feels on a slower machine.
- [x] **1.11 Getting around.** Editor zoom, word wrap toggle, Go Back and
  Forward (`NavHistory`), Maximize Pane.
  *Verified in Xvfb, 2026-11-10:* `zoom:*`, `navrecorder:*` (the Mac's
  MCNavigator as the core's `Nav::Recorder`), `rightarea:maximize`,
  `settings:font-size-set` and `commands:linux-getting-around` hold the
  rules, and four suites press real keys. `zoom`: Ctrl+=, Ctrl+Shift+=,
  Ctrl+- and Ctrl+keypad 0 (Ctrl+Alt+0 until the decision under D1 gave it
  to Fold All in the editor) change the editor's font a point at
  a time (never under 6 points) and the gutter's width with it, scale the
  Markdown page's sizes by (13 + zoom) / 13, keep the zoom in
  `zoom.json` and not the settings file, and a second app opens at it; with
  a PDF on show the keys zoom the PDF and Ctrl+0 still focuses the tree,
  with the browser's keyboard they zoom its page on Safari's steps; with the
  terminal's the same keys zoom only the terminals (VTE's font scale: fewer
  columns, which `tput cols` reads), while Ctrl+W, Ctrl+B, Ctrl+E, Alt+Z
  and Ctrl+Shift+- still reach the program as bytes. `word_wrap`: a long
  line wraps by default with nothing to scroll sideways, Alt+Z turns that
  over in every window for the session (nothing typed, the settings file
  untouched), a Markdown page still wraps, editor.word-wrap = off keeps it
  off and a setting changed since Alt+Z wins; a 30,000-character line is
  still shown a part at a time, on one row while the view wraps, End going
  on into its hidden rest and text typed at its start keeping it so; and a
  file of 300,001 lines, in large-file mode, does not wrap (wrapping is one
  of the features off there, as in VS Code: GTK took half as long again to
  lay it out wrapped).
  `go_back`: the ten-line rule (eight Downs no jump, Ctrl+End one), typing
  no jump, Back and Forward stopping at the ends, a tab switch by
  Ctrl+Page Up, Go to Line, a symbol from the palette (its previews and
  Escape no jump), the server's definition in another file (the scripted
  server), a Find in Folder hit, a new jump dropping what lay ahead, a file
  renamed in the tree followed, and one deleted from the disk passed over.
  `maximize`: Ctrl+Alt+M from the browser, the terminal, the editor and the
  tree, each alone over the right side with no bars and everything back as
  it was; Toggle Terminal showing the terminal rather than closing it, Focus
  Editor, a file opened from the tree, and Toggle Editor hiding the
  maximized editor each ending it. They fail without the editor's size
  rule, the Terminal commands' keys in the terminal, source wrapping, the
  long lines' no-wrap tag, the palette telling the history it is open, the
  jump around a Find in Folder result, and revealEditor ending Maximize
  Pane. Changed for the user: source now wraps
  by default (editor.word-wrap, as on the Mac), editor.font-size and
  terminal.font-size apply when the settings file sets them (unset, the
  desktop's size as before), and the menu bar has a Terminal menu. Not
  done: the browser's zoom kept per site (item 4.5), and the completion
  list, hover and find bar following the zoom. Needs the owner: the keys
  under GNOME on Wayland with IBus (Ctrl+Alt+M, Ctrl+Alt+-, Alt+Z), the
  keypad with Num Lock on (only off is pressed in the checks), how the
  zoomed text and gutter look and HiDPI, and Go Back with a real
  language server and terminal links.
- [x] **1.12 Large-file mode.** `LargeFile.h`, at VS Code's thresholds.
  *Done means:* a 25 MB file opens and types within the Mac's bounds.
  Measured for R1 on main with tabs (2026-11-09, the main thread held by
  one `Editor::openFile`): a 15 MB C file of 300,000 lines, 0.6 s of CPU
  (0.2 s as .txt, so most of it `Document::startHighlighting`); a 5 MB
  file on one line, 1.5 to 3.0 s of CPU, and once 5.2 s long at load
  average 8, past GNOME's 5 s before "not responding". Before tabs these
  were 3.0 s and about 5 s of CPU.
  *Verified in Xvfb, 2026-11-10:* `large-file:utf8` and
  `large-file:long-lines` hold the UTF-8 thresholds, the pieces a big file
  goes in by, and the part of a long line shown. The `large_file` suite
  opens a 25 MB Python file read in the background (0.33 to 0.37 s of
  main-thread CPU in all, never more than 21 ms at a time, against 0.6 s
  at once before): plain, kept from a scripted server, with the status
  bar's note; 200 characters at 0.07 to 0.18 ms each, and Return and Tab
  from the keyboard never more than 25 ms (0.45 to 0.6 s before); away and
  back in 8 to 60 ms, at its place; a backup; Reopen Closed Tab with its
  selection. A 300,001-line file is large too, and a real click on the
  note turns colors and the server on; the setting off opens it as any
  file; a 70 MB file asks Open Anyway or Cancel, and no server gets it past
  the sync limit. A 5 MB line opens and types without holding the main
  loop (33 and 10 ms at most; 6.7 s and 3 to 9 s a key before), shown
  10,000 characters at a time, End and the find bar reaching its hidden
  part, and Select All and Copy taking all of it. Each phase's longest
  stall is bounded at 250 ms of CPU, timed as R1's `tree_stall` does. It
  fails without the long lines' hiding (17 s), without reading in the
  background (0.58 and 1.9 s), without the server gate, without
  AutoIndent's slice (0.45 s), and without the copy of hidden text.
  `HANDOFF.md` item 22 has the rest. Needs the owner: how "Opening…", the
  note after a long line's part, and the status bar's note look; a real big
  log and minified file on the desktop; a middle-click paste of hidden text
  (GTK's, the visible part only); GTK spends 10 to 20 s laying a
  470,000-line file out after it comes on show, during which a jump far
  down jumps about before it settles.
- [x] **1.13 The Mac's layout.** The owner's report, 2026-11-09: the browser
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
  *Verified in Xvfb, 2026-11-09:* `rightarea:*` hold the core's rules
  (`../src/RightAreaLayout.h`, the Mac's `relayoutRightArea` and its two
  bars), and the `layout` suite, in a window of its own, presses the toggle
  keys and drags the bars for real: the browser right of the editor at the
  same top and the terminal below both across the whole width; the bar
  between them dragged (both follow, and the browser keeps its width when
  hidden and shown), and within 16 px of either edge (that side hides, and
  comes back when the same drag goes back out); the terminal's bar to the
  top (the editor and the browser hide) and back down; the editor, the
  browser, and the terminal hidden in all six orders staying hidden, the
  tree then having the window, and each coming back at its size; a click in
  the tree, the preview key, or Focus Editor showing a hidden editor; the
  sidebar's divider dragged to the right edge giving the tree the window,
  and a drag from 3 px inside the editor's left edge leaving it alone (it
  took presses 14 px into the editor); Ctrl+B hiding only the sidebar; the
  find bar on the editor's side. It fails with the old rule
  that brought the editor back when the terminal closed, and with the old
  one that hid the editor while the browser showed. Needs the owner: how the
  bars feel with a real mouse and touchpad under GNOME (each grabs 4 px past
  its line on either side, over the panes' edges), and how they look.

*Done means, for each:* the feature's Mac GUI checks have Linux counterparts
in `linux/tests/gui`, pressing real keys.

### Stage 2: language and git, 6,000 to 8,000 lines

These may run in parallel. They need Stage 1's tabs (every open tab stays open
on its server) and D5. Item 1.1 left the session on the tab on show
(`LspSession::useBuffer`, re-pointed on every switch); keeping every tab open on
its server comes first here (2.0).

- [x] **2.0 The client's shape, and every tab open on its server.**
  `linux/src/Lsp.cpp` split by concern as the Mac's `LspSession` is
  (`HANDOFF.md` item 28 has the shape, for 2.2, 2.5, 2.6, 2.7, 2.12 and 2.13
  to each add a file), and every tab's file kept open on its language's
  running server, as the Mac's `syncOpenDocuments`.
  *Verified in Xvfb, 2026-11-10:* the `lsp_tabs` suite, against the scripted
  server taking ranged changes, opens three Python tabs and a Go tab: each
  Python file opened once and none closed by leaving it; keys typed into two
  tabs reach the server as ranges with each tab's own text (an emoji
  included, which sends the whole text), and an edit to a tab in the
  background (as Replace in Folder makes) too; a server started by a
  settings change is given every Python tab with its unsaved text, while
  the Go tab in the background starts no server until it comes on show;
  Ctrl+W (Don't Save) closes the file on the server. It fails with only the
  tab on show synced (13 checks) and with a background tab starting its
  server (3). Needs the owner: real clangd, basedpyright, or rust-analyzer
  with several tabs (their memory and indexing with every tab open).
- [x] **2.1 Rename, references, and quick fixes.** The all-or-nothing
  workspace edit applier. Mac: `Lsp+WorkspaceEdit`, `Lsp+Actions`, `LspPicker`.
  *Verified in Xvfb, 2026-11-10:* `lsp:edits-hold`, `lsp:point-edits` and
  `lsp:quick-fix-range` hold the core's new rules, and the `lsp_actions`
  suite presses F2, Shift+F12, Ctrl+. and the lists' keys against the
  scripted server: references grouped by file with the one under the caret
  selected, Down past a heading, Enter into another file as a jump Go Back
  returns from, a click on a row, Escape; prepareRename's refusal ("This
  can't be renamed"); a rename typed into the box across the clean tab on
  show, a tab with unsaved text the server has, a tab the server was never
  sent whose change is elsewhere (checked against the disk), and a file with
  no tab, opened as a kept tab and left unsaved, one Ctrl+Z each; refused
  whole, nothing changed anywhere, when a file moved on (an older version),
  when a range no longer reads what the server saw, and when a file it
  opens shows as a preview (the tabs it opened closed again); a quick fix
  for the caret's line's diagnostic with an edit, one resolved first, and
  one whose command the server answers with `workspace/applyEdit`, answered
  applied; and the three keys unbound in the terminal, F2 the tree's
  Rename File there. It fails without the ranges' check and the tabs'
  closing (17 checks) and without the list's keys (Down, Enter). Keys (D1):
  VS Code's Linux F2, Shift+F12 and Ctrl+., editor commands. Needs the
  owner: rename, references and fixes with a real clangd, basedpyright or
  rust-analyzer; the list's and the box's look; the box's keyboard under
  GNOME on Wayland with IBus, and Ctrl+. there (an input method's emoji key
  in some setups).
- [x] **2.2 Problems**, the second server, watched files, and Organize
  Imports. Mac: `ProblemsPanel`, `LspWatcher`.
  *Verified in Xvfb, 2026-11-10* (`HANDOFF.md` item 32): `lsp:watch-roots`
  holds the core's new rules, and four suites against the scripted server
  press real keys and clicks. `problems`: Ctrl+Shift+M lists three files'
  problems (one no tab has) by file, errors first, hints left out, the
  status bar counting them; Down and Up skip a heading, Enter and a click
  open a problem, Escape and Ctrl+W, F8 and Shift+F8 wrapping with the
  message at the caret, a closed tab's problems dropped, the counts' click.
  `second_server`: a stand-in ruff on PATH runs as "ruff server" for a file
  below a ruff.toml only; both servers' diagnostics drawn, listed and
  tagged "(Ruff F401)"; hover to the main server only; Ctrl+. lists both
  servers' fixes, each naming its server, and runs Ruff's. `lsp_watch`:
  registered globs get created, changed and deleted files (in a folder
  made after watching began, and below a folder outside the project),
  never a .txt, .git or anything after unregistering; 20,000 changes
  matched off the main thread (0.4 ms of its CPU). `organize_imports`:
  Shift+Alt+O runs Ruff's source.organizeImports.ruff over the main
  server's bare one, one undo step; Ctrl+S organizes, then formats, then
  writes; off, or by auto-save, nothing is asked; a silent server holds a
  save 3 s. They fail without the tag (1 check), without watching new
  folders (3), and without organizing on save (5). Keys (D1): VS Code's
  Linux Ctrl+Shift+M, F8, Shift+F8 and Shift+Alt+O. Needs the owner: a
  real basedpyright and ruff (Ruff's diagnostics, fixes and imports, and
  basedpyright seeing a module written to disk), inotify's watch limit on
  a large tree with a .venv, and the list's look and its keys under GNOME
  on Wayland.
- [x] **2.3 Server and tool consent.** A folder's own `.venv`, `node_modules`,
  and project configuration are never run unasked. Mac: "Servers that run the
  folder's code" and "needs the user's yes" in `CLAUDE.md`. This goes in with
  or before 2.4.
  *Verified in Xvfb, 2026-11-10:* `folder-tools:*` hold the servers'
  question, its words and the answers file
  (`$XDG_STATE_HOME/minicode/folder-tools.json`), and `format-run:*` the
  formatters' side (below); the `tools_consent` suite clicks the bar under
  the tabs for real: with a `.venv` holding the only Python server (a
  script recording that it ran), unasked the bar asks, nothing from it runs
  and Ask Again is off; Use It runs the server from it and gives the
  servers its python; Ask Again from the palette (real keys) takes the
  answer back and the servers start again without it; Don't Use It keeps
  "deny"; × puts the question off through tab switches and an answer given
  meanwhile, until Ask Again. `server_consent` runs the scripted server as
  rust-analyzer: unanswered it starts with build scripts, proc macros and
  cargo check off (initializationOptions and its settings' answer) and the
  bar asks; Allow starts it with its defaults; with a rust-toolchain.toml
  and Don't Allow it does not start and the status bar says it waits, and
  taken back the bar names the file. They fail with the folder's answer
  taken out of the servers' interpreter and of `checkProjectCode`. Not
  done: Select Python Interpreter, the Language Servers popover, and the
  terminal's venv, which the port has not. Needs the owner: the bar's look
  and its buttons with a real mouse, and a real `.venv` with basedpyright,
  rust-analyzer, and the TypeScript server.
- [x] **2.4 Format Document.** `Formatter`, the server fallback, and format on
  save. Mac: `FormatRunner`, `Lsp+Formatting`.
  *Verified in Xvfb, 2026-11-10:* `format-run:*` hold the core's
  `FormatRun` (the Mac's MCFormatText decisions and words, which copy runs
  or why none, what an exit means, the question, the output as line runs in
  code points). Four suites press real keys with stand-in formatters on
  PATH. `format`: Ctrl+Shift+I on a 3,000-line file changes its 300 lines in
  one user action (one Ctrl+Z, Ctrl+Shift+Z again), the caret on its text
  and the top line kept, unsaved; Google's pyink found on PATH with its
  options and the file; three selected lines give `--line-ranges=3-5`; a
  failing formatter, one writing what is not UTF-8, and one past the 10 s
  limit (SIGTERM ignored, so SIGKILL; its process gone) leave the buffer
  alone with their note; a result for text typed into meanwhile is
  dropped. `format_on_save`: Ctrl+S formats then writes; Save All and
  auto-save write as typed; auto-save leaves a tab whose Ctrl+S waits; a
  tab left during its format is saved as it was. `format_consent`: the
  Mac's whole suite, with real clicks on the question (Allow, Don't Allow,
  Cancel; Deny formats with the copy outside the folder; Ctrl+S asks too; a
  prettier.config.js asks even for the user's own command, above the
  folder by its whole path; npx with the folder's Prettier). `format_coverage`:
  a README formatted by the project's Prettier, never by its server; the
  server (the scripted one) formats it when no Prettier is set up, and a
  `.notes` file with `lsp.notes` and `lsp.notes.extensions`, before Ctrl+S
  writes it too. They fail with the folder's answer not passed to the
  decision, with each line run its own user action, without the server
  fallback, and without auto-save's guard. Changed for the user: servers the settings name by
  extension (`lsp.<name>.extensions`) and an optional built-in one the
  settings give a command (`lsp.markdown = ...`) now run on Linux, as on
  the Mac, which the fallback needs. The server's edits go in through
  item 2.1's workspace edit applier, as the Mac's go through its own.
  editor.organize-imports-on-save waits for the port's code actions (item
  2.2). Needs the
  owner: the owner's real formatters (ruff, clang-format, prettier), Ctrl+Shift+I under
  GNOME on Wayland with IBus, and how the note by the caret looks.
- [x] **2.5 Completion's additional edits and snippets.** `Snippet`,
  Tab and Return as on the Mac.
  *Verified in Xvfb, 2026-11-10* (`HANDOFF.md` item 33): the
  `completion_keys`, `snippets`, `completion_edits` and `signature_help`
  suites, real keys against the scripted server, show the list opening by
  itself, Tab and Return taking an item whenever it shows (Shift+Return
  and Escape not), snippets expanded with stops that Tab and Shift+Tab walk
  and Escape ends, a mirror following typing, a choice's options in the
  list, an import going in with its word and out with one Ctrl+Z, a late
  import from resolve in the word's step or its own, and the signature
  hint on "(" and ","; they fail without the accept's own undo step and
  without the late import's replay. Needs the owner: real clangd and
  basedpyright, and the keys and look under GNOME on Wayland with IBus.
- [x] **2.6 Semantic tokens.** Tags, painted for the lines on screen only.
  Done (`HANDOFF.md` item 36): `SemanticColors`, `LspSessionSemantic.cpp`,
  the core's `lsp:semantic-*` groups and the `semantic_tokens` suite (class,
  function, parameter and variable colors, the string keeping its own, far
  lines unpainted until seen, edits ending the tokens). Needs the owner: the
  colors with a real server.
- [x] **2.7 Inlay hints (D6, D9).** `InlayHints`. Left out of the Linux
  port (the owner's call, 2026-11-11): Pango splits a `letter-spacing`
  tag's room on both sides of the character and GtkTextTag takes no
  negative value, so nothing leaves room on one side (D9 has the
  measurements). Look again when GTK or Pango can.
- [x] **2.8 Install hints.** `InstallHints`. Done: the core's Linux ways, `LspSession::missingServer` and `lookAgainForMissingServer`, `Terminal::typeText` and `onCommandFinished`, `InstallView`, the offer in `ConsentBar` (Install..., Copy, Run in Terminal, x, Don't Show Again), wired in main.cpp, and the `install_hints` GUI suite (HANDOFF item 37). Format Document's note offers the missing formatter's install command too.
- [x] **2.9 Git verbs.** Push, pull, autofetch, Stage All, Discard with its
  Trash copy and undo, hunks and lines, the branch picker, commit verbs.
  `GitModel` already has the plans.
  *Verified in Xvfb, 2026-11-10* (`HANDOFF.md` item 30): `git-ui:*` hold
  the new rules (`keepChosen`, `hunkMenu`, Linux's words in
  `networkFailureText`), and four suites over scratch repositories and a
  bare remote press real keys and clicks: `git_sync` (Fetch from the
  palette with the panel hidden, said in the status bar; the right click's
  four, Sync Changes, Publish Branch, Amend asking for a pushed commit, the
  picker tracking `origin/topic`, a push whose ssh wants a key failing in
  0.2 s with ssh's words, in a session apart from minicode's; autofetch's
  three minutes, the period, backoff after failures and recovery, no remote
  never fetched), `git_actions` (Stage All, Unstage All, Discard asking,
  restoring from the index not HEAD, untracked to the Trash, a deletion
  restored, Undo Discard, a round trip through the scratch world's real GIO
  Trash, Discard from the palette on the file on show, the row menu, the
  picker by a click and the palette, a bad name, git's refusal, a
  submodule, an `add -N` file, a conflict and a staged row refused, each
  with its reason, and Stage All asking with a conflict left), `git_hunks`
  (the links, Stage Hunk, Unstage Selected Lines from the right click,
  Discard Hunk and its undo, a stale diff refused, Shift+F10), and
  `git_verbs` (Shift with the arrows, Ctrl and Shift clicks, Space and the
  row menu on two rows, Discard of three, Commit by Ctrl+Return, Amend,
  Undo Last Commit, Stash, Pop Stash). They fail without the session of
  its own, the hunk links, the pick list, or the rows kept chosen. Needs
  the owner: push, pull and autofetch against their real remotes with ssh
  keys, an agent, a GUI askpass or a credential helper; the desktop's real
  Trash and GNOME Files' Restore From Trash; how the menus, links and
  button look; Ctrl and Shift clicks and Shift+F10 under GNOME on Wayland.
  The flakes of whole runs were the checks clicking before GTK had laid a
  widget or a new diff out (`HANDOFF.md` item 30's traps); every click
  now waits for its target, and `tabs palette git_sync git_actions
  git_hunks git_verbs` passed 6 of 6 runs (4 beside a dozen busy loops,
  load 11 to 13), `git_hunks` alone 10 of 10 so loaded, and the whole GUI
  run once (2026-11-10). `large_file` failed its 250 ms bound once (263.8
  ms), not this item's. Since the merge with 2.11, every menu of the panel
  ends with Adjust Settings… (`GitPanel::popupMenu`), the message box's
  text menu too, and a part with nothing of its own on the Mac (the change
  list off a file row, the graph, the panel outside a repository) shows it
  alone.
- [x] **2.10 Change bars and the tree's git colors.** `QuickDiff`,
  `GitStatus`. Done (`HANDOFF.md` item 34): the `git_gutter` and
  `tree_git_colors` suites, real keys and clicks over scratch repositories
  (bars for typed edits, moved at once, Alt+F5 and Shift+Alt+F5, the bar's
  popover with Revert and Stage, Stage refused when the index moved on,
  git outside with the panel hidden; the tree's modified, added,
  untracked and ignored colors, folders two deep, a commit outside, the
  theme's and the keys' colors), 3 of 3 runs each. Needs the owner: the
  popover's look and placement, the bars' colors on a real screen, Alt+F5
  under GNOME (its own keys take it unless turned off, as on this
  ThinkPad), and a file changed by another program in a folder the tree
  never listed (colored at the next look, not at once as on the Mac).
- [x] **2.11 Themes from a menu and Adjust Settings.** The owner's words
  (2026-11-09): "no 'change settings' when right clicking on some portion
  of the GUI. This should point to the relevant part of the minicode
  config." `HANDOFF.md` item 31 says how a new part adds its item.
  *Verified in Xvfb, 2026-11-10:* `settings:adjust-addition` and
  `settings:surface-key-lines` hold the core's text and lines to the
  Mac's, `theme:light` the stylesheet's light selection and scroll bars,
  and four suites press real keys and clicks. `adjust_settings`: a right
  click on the file tree, the editor, a Markdown page, a tab and the strip
  past the tabs, the status bar, the Source Control panel and its message
  box, the terminal, the browser's page (WebKit's own menu) and its
  address bar each opens a menu whose last item is Adjust Settings…
  for that part; clicking it opens the settings file with the keyboard
  in it, the first key's value selected and every key's line tinted,
  still at 1.5 s and gone by 3.3 s; an old file gets the missing keys'
  commented defaults under one "# Added by Adjust Settings" line, saved at
  once and taken back by one Ctrl+Z when the tab was clean, left unsaved
  when it was not (View > Adjust Settings > Terminal through the menu
  bar). `themes`: View > Theme by real clicks writes the line, its row is
  checked, and the editor's text, the gutter, the tree, the terminal, the
  status bar, the tab strip, the find bar and the Source Control panel
  read back as the theme's pixels, the keyword tag in its color, GTK's
  color scheme and the menu bar dark or light with it, and the keyboard
  back where it was; a color of the
  file's own asks (Keep My Colors, Use the Theme's Colors, Cancel each
  doing what it says), CRLF stays, a symlinked file stays a link with
  nothing left beside it, and a tab holding the file unsaved refuses with
  the reason in the status bar until undone. `swatches`: a swatch's click
  (on its last digit) opens the chooser at that color, not the caret
  line's, writing nothing; a color typed into its field rewrites the line
  and saves; an opacity value's click opens a slider at it, the arrow
  keys move it, the line is uncommented and saved. `config_writes`:
  Keyboard Shortcuts… and Adjust Settings… make keys.conf and
  settings.conf through dangling symlinks, which stay links. They fail
  without the item in the menus, without the color scheme and the theme
  action's state following the file, without the menu bar giving the
  keyboard back (GTK left it on the bar's item: `MenuBarFocus`), without
  the character-under-the-point fix for swatches and opacities, and with
  `g_file_set_contents` for the first writes. In two of three full runs
  at load 40 `large_file`'s "its place as it was" timed out (180 s); it
  passed alone twice and after every suite before it, at load 3, so it
  is noted as a flake under load, not chased. Not done: the palette does
  not list the themes or the parts (the Mac's reads its menu bar), and
  the title bar has no menu (the port draws none). Needs the owner: how
  each theme looks on the desktop, and whether GTK's menus and dialogs
  follow a light theme under the desktop's own `Yaru-dark` (only GTK's
  built-in theme was tried), right clicks with a real mouse and touchpad
  under GNOME, the WebKit page's menu there, and the color chooser and
  slider popovers' look and keyboard.
- [x] **2.12 Symbols from the servers.** Done (November 2026,
  `HANDOFF.md` item 35). Go to Symbol in File ("@", Ctrl+Shift+O) asks the
  file's server first (`textDocument/documentSymbol`, the outline standing
  in when none lists symbols, one refuses, or it exits), and Go to Symbol in
  Workspace is the palette's "#" with VS Code's Ctrl+T, asking every running
  server offering `workspace/symbol`, cancelling the question before and
  merging the answers (`LspSessionSymbols.cpp`). Suite `server_symbols`
  fails without the fix. Needs the owner: real clangd and basedpyright, and
  Ctrl+T in the terminal (the program keeps it there).
- [x] **2.13 Folding ranges from the servers.** Done (November 2026,
  `HANDOFF.md` item 35). `textDocument/foldingRange` is asked 0.1 s after a
  file comes on show and 0.6 s after typing (`LspSessionFolding.cpp`), and
  `DocumentFolds::setServerRanges` takes the sanitized answer over
  `Fold::compute`'s regions while the text is the one asked about. Suite
  `server_folding` fails without the fix. Needs the owner: real servers'
  folds in the gutter.

### Stage 3: agents, 8,000 to 10,000 lines

3.1 and D10's test come first. The rest may run in parallel.

- [~] **3.1 The control channel and `minicode` commands.** `ControlProtocol`,
  a socket under `$XDG_RUNTIME_DIR`, 0600 in a 0700 folder. Mac:
  `ControlServer`, `ControlCommands`, `tools/minicode-cli.cpp`.
  Part one (done, November 2026; `HANDOFF.md` item 38): the server, the
  client (the Mac's CLI, shared) and `open`, `window`, `state`, `selection`
  and `ping`, with the user's typing protected (`"background"`); the `control`
  GUI suite. Remains: `term` and `term-output` (with the terminal),
  `split`, `install-skill` and the skill; every one answers "not on Linux yet"
  now. `diff` is done with 3.2; `browse`, `page` and `open-url`/BROWSER with
  3.7.
- [~] **3.2 The compare view.** `SideBySide`, word changes, Accept and Reject
  per hunk. Part one done (HANDOFF item 39): `CompareView` and
  `ComparePanes`, a comparison tab through `Editor::showComparison` with
  syntax colors, changed rows and words marked, panes scrolling together, F7
  and Shift+F7. Part two done (HANDOFF item 39): a comparison with a
  decision (`CompareDecision`: a check and a cross per hunk in a gutter, the
  bar, `lastChoiceGrace` with Send Now, `acceptedText`) and `minicode diff`.
  Compare with Main is done (3.5, HANDOFF item 44);
  the IDE link's openDiff is done (3.9). Also the Mac's diff preview tab that each `diff` replaces (every
  call opens a tab of its own here), choice tints on the rows, and keeping a
  tab's place.
- [~] **3.3 The inline agent.** `InlineAgent`, with pending edits. Never run
  the real Claude Code in a check: use `tests/fixtures/claude`. Part one done,
  November 2026 (HANDOFF item 46): Ctrl+I, the bar at the caret, the streamed
  preview, the pending diff and tint, Keep and Esc, the `agent_panel` GUI
  suite. Remains (part two): commands typed for an idle terminal, tasks
  handed to Claude Code in a terminal tab, the Markdown preview's context,
  Ctrl+Enter.
- [x] **3.4 Notes and tours.** Mac: `AgentNotes`. Done, November 2026
  (HANDOFF item 41): `AgentNotes` over a text tag and an overlay box,
  `open --note`/`--from`, `tour` and its Alt+] and Alt+[ keys, the agent's
  preview tab, the `notes` GUI suite.
- [~] **3.5 The Agents panel and Evidence.** `AgentTranscript`, `AgentIndex`,
  `AgentEvidence`, `AgentView`, with Linux's scratchpad path. Claude Code's
  data is read, never written. Part one (November 2026, HANDOFF item 44):
  the model on a worker thread following the transcripts and scratchpads
  with GFileMonitor, and the panel in the sidebar (Ctrl+Shift+A), with the
  `agents` suite. Part two (HANDOFF item 44): the Evidence page in a tab
  (a click on a session), the Worktrees node first in the file tree with
  Compare with Main and Open in New Window. Remains: Show Terminal (a
  session waiting in a terminal), and the node appearing without a relisting
  of the top folder when `.claude/worktrees` is first created.
- [x] **3.6 Waiting for you.** `ClaudeHooks`, `AttentionBoard`. Done
  (November 2026, HANDOFF item 42): the terminal's id and the two
  variables, `notify --from-hook`, `hooks --print|--install`, a dot and
  tooltip over the terminal, the waiting count in the window titles, Next
  Waiting Terminal on Ctrl+Alt+W, typing and closing clearing the mark; the
  `attention` GUI suite. Remains: the session labels in the Processes panel.
- [x] **3.7 Agents checking pages.** `PageCheck`, on WebKitGTK. Done,
  November 2026 (HANDOFF item 45): the agent page beside the user's in the
  pane with its Yours | Agent switcher and x, `browse`, `page --url/--text/
  --console/--screenshot/--reload`, `open-url` and BROWSER; the
  `browser_pages` suite.
- [~] **3.8 The Processes panel.** `ProcessTable` over `/proc`; its `Source` is
  the seam. Part one (November 2026, HANDOFF item 40): `ProcFs::Source`, a
  sampler thread (`ProcessMonitor`), Stop's SIGTERM then SIGKILL, and the
  control channel's `procs`, with `procfs:*` and the `processes` suite.
  Part two (HANDOFF item 40): the panel in the sidebar (View > Toggle
  Processes), rows by group, Stop in the menu and on Delete, 2 s sampling
  only while it shows. Remains: the session labels (the board of item 3.6 is built; the wiring), "Open
  localhost:port" and "Reveal Origin" in the row's menu, and the owner's
  eyes on the look.
- [x] **3.9 The Claude Code IDE link.** `IdeLink`. Done, November 2026
  (HANDOFF item 43): `IdeLinkServer` on the GLib main loop, the lock file,
  `agent.ide-link` and View > Claude Code IDE Link, `CLAUDE_CODE_SSE_PORT`
  for the terminal, selection_changed, openFile, openDiff, close_tab,
  closeAllDiffTabs, getDiagnostics, the `idelink` GUI suite. Remains: a run
  against a real Claude Code (the owner's).

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
  (fixed by item 1.12: 33 ms at most). Needs the owner: the tree under GNOME on Wayland while a
  build runs in an open folder.
  *Follow-up, 2026-11-09:* after R1 about one GUI run in five failed at the
  save suite's first tree click. A listing adds a row whose widgets GTK maps
  at once and places at the next frame; the harness counted mapped as on
  show and clicked the unplaced label, which lands on the window's corner
  (the File menu). The harness now hands out only widgets with a size that
  GTK picks at their center (`MCGuiOnScreen`), so neither an unplaced row
  nor one scrolled just out of view. The tree also kept the pressed row
  and the last click's row by position, so a listing landing while the
  button was down opened the file put in under the pointer, or nothing; it
  now keeps row objects. The `tree_clicks` suite checks both and fails
  without either fix.
- [x] **R2. `minicode` from a terminal holds the terminal** (2026-11-10):
  "if I launch a minicode application instance from the terminal on mac,
  it doesn't block. If I close that terminal, the process is not bound and
  I'm not warned that closing the terminal window will also kill the
  minicode instance. On linux, it is bound and does block the terminal."
  Started from a terminal, `minicode` gives the terminal back at once and
  is not tied to it, as on the Mac and as VS Code's `code` on Linux;
  `--foreground` keeps the old way.
  *Done, 2026-11-10 (`HANDOFF.md` item 23):* `main` first calls
  `Detach::start` (`src/Detach.{h,cpp}`, the rules in
  `src/LaunchOptions.{h,cpp}`), which forks with a terminal on hand: the
  child takes a session of its own and runs the app, the parent returns
  once the child's first window is made, or with the child's status when
  it ends first (a second `minicode` handing its path over, no display).
  The child then puts `/dev/null` on its standard streams, keeping a file
  the user sent them to, and asks systemd for
  `app-minicode-org.minicode.Editor-<pid>.scope`. `-h`, `-v`,
  `--foreground`, and `--` are the port's options; the Mac's `-n` and `-r`
  are refused, since the port has neither behaviour. GNOME Terminal never
  stops a shell's scope (libvte 0.84 has no StopUnit), so only the
  hang-up's SIGHUP had closed minicode. The `detach` suite runs the real
  `build/minicode` on pseudo-terminals, on the checks' Xvfb and a session
  bus of its own (58 checks, about 5 s; 33 fail with the old `main`): it
  returns 0 in under half a second with its window up, in its own session
  with no controlling terminal and nothing of the terminal open, outlives
  the terminal's hang-up, hands a second `minicode notes.txt` over, keeps
  a log file it was sent to, returns 1 with GTK's message when there is no
  display, does not fork when started as the desktop starts it, and with
  `--foreground` holds the terminal and goes with it. Needs the owner:
  closing a real GNOME Terminal window with minicode started from it, that
  GNOME Terminal asks nothing first, and `systemctl --user list-units
  'app-minicode-*'` while it runs (the checks answered for systemd).
- Right-clicking a part of the window offers no Adjust Settings (the same
  day): that is item 2.11, done.
- [x] **R3. "It takes multiple seconds to open minicode on ubuntu and
  under 1 s to do the same on mac"** (2026-11-10, the PPA's 1.4.14 on
  `~/minicode`), on `worktree-agent-a4711ccfaf9d559fd`. Find where the time
  to a usable window goes, cut what need not happen before it as the Mac
  does, and bound it with a check.
  *Measured and fixed on the ThinkPad's Xvfb, 2026-11-10* (`HANDOFF.md`
  item 24): `MINICODE_STARTUP_LOG=1` prints each phase of the start with
  the time since main and the main thread's CPU. Every window made its
  hidden terminal and browser at once: VTE started the shell, and the
  WebKitWebView started WebKit's network and web processes in bwrap
  sandboxes the main thread waited for, with xdg-dbus-proxy, and loaded
  DuckDuckGo. The Mac makes both when first shown, and now so does the
  port; the language server for a session's tab starts after the first
  frame. Medians of six warm runs on `~/minicode`, interleaved, load 3 to
  6, milliseconds after main (wall / main-thread CPU), main then this
  branch: GTK's own start 100/93 and 96/93 (a bare GtkApplication window
  takes the same); the window built 164/142 and 115/110 (the browser was
  39/23 of it); realized and presented 217/182 and 166/150; first frame
  244/203 and 189/170. With a session of five tabs: first frame 299/248
  and 220/197, clangd started before it and 90 ms after it. The process
  ran 47 to 50 ms before main in both. The app and what it started used
  1.87 s of CPU in its first three seconds, and 0.26 s now. The PPA's
  1.4.14, timed from exec by an LD_PRELOAD probe, drew its first frame at
  268 ms with 1.91 s of CPU in all. So on Xvfb, with GTK drawing in
  software, no version takes seconds; the owner's seconds are on the
  desktop, where GTK and WebKit draw on the Intel GPU and WebKit's
  processes, the page and the shell's startup files compete with the
  window for four cores. The `startup` suite bounds the first frame at
  1500 ms of main-thread CPU (165 to 300 here) and 20 s, checks that no
  shell, WebKit process, bwrap or xdg-dbus-proxy runs below the app at
  its first frame and neither pane's part is made, that the server starts
  after it, and that both panes work once shown; without the fix it fails
  eight checks. Needs the owner: the same log on the desktop,
  `linux/dev/startup-times.sh` with the build of main and this branch
  (item 24 has the commands), for GTK's default renderer, `gl`, `vulkan`
  and `cairo`, to see what the GPU renderer's first frame and the desktop
  add; and whether the window now opens in well under a second there.

## After each stage

Update `BUILD-LINUX.md` ("What is verified") and the list of what still needs
a person (`BUILD-LINUX.md`, "What needs a person"). Tell the owner what to
try by hand, and ask whether to release.
