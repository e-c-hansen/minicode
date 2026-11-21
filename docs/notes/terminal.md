# The terminal

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

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
  reaping with `kill(pid, 0) == ESRCH`, not `waitpid`. `make guitest
  SUITES=terminal_pty` checks the teardown (sources finished, fd closed,
  shell reaped; a shell ignoring SIGHUP; 1 MB into a program not reading;
  closing during `yes`), reaching MCPty's state by KVC, no test hook.
- **The shell's end** (2026-11-19, round 9's A4; `src/TerminalExit.h`,
  `terminal:exit`): `exit`, or Ctrl+D at an empty prompt, closes the
  terminal (`terminal.close-on-exit`, on by default): `shellExited:status:`
  decides from the wait status (MCPty's reap passes it to `exited`, the
  host's TermExited carries it), and on Close tells `onShellExit` a turn
  later, which MCTerminalPanel answers with `removeTerminal:` (unasked;
  the last one empties the dock, `onEmpty`). The host has ended its
  terminal by then, and the dock's record follows `changed`. Off, it says
  "Process exited" (with the code when not 0) and waits for Return. A
  signal never closes it: "The shell ended (signal N). Press Return to
  start a new one." A shell that ends before its first prompt (no
  `_seenPrompt`, under 2 s) waits for Return too, so a broken startup file
  neither closes a new terminal nor loops. A command still waiting
  (`runCommand:` before the prompt) gets a new shell instead of a close.
  MCPty also watches the shell's pid (a `DISPATCH_PROC_EXIT` source): on
  its exit it reads what is left (1 MB at most) and shuts the pty down,
  so a job left holding the pty open (nohup) no longer keeps `exit` from
  closing anything, as the host's `exited` already did. Checked in
  `terminal_pty` (both CI passes): `exit` closes the last terminal and
  the dock goes, Ctrl+D closes one of two, the setting off says "Process
  exited (code 3)" and Return starts a shell, SIGKILL says signal 9 and
  Return starts a shell; `terminal_reflow`'s killed shell waits for Return.
  The GTK port does the same (2026-11-19, linux/HANDOFF.md item 117):
  `Terminal::shellExited(status, error)` asks the same core functions
  with the host's TermExited status (`prompted`: a prompt the host
  counted, a held shell attached, or 2 s since the spawn), and on Close
  calls `onShellExit` from an idle, which `TerminalDock::hook` answers
  with `closePane(t, false)`: a split's pane, the tab with its last pane,
  the dock with the last tab (`onEmpty`), as Close Tab. Waiting, a Return
  in VTE's commit starts a new shell (`startAgain`; nothing else typed
  goes anywhere), and so do `runLine` and `typeText`; a line runLine
  still waits to type gets a new shell instead of a close. The restart it
  did before (three quick deaths stopped the loop) is gone. Checked by
  the GTK harness's `terminal_exit` and `processes` (a `( job & )` under
  its terminal, then an orphan after `exit`). **Trap**: GTK suites that
  wanted a fresh shell typed `exit` and waited for the restart
  (`shell_integration`, `terminal_marks`); they now make a new terminal
  and close the others.
- **Output flow**: past 1 MB handed to the main thread the reader
  (`MCPtyReader`) suspends its source (the program blocks) until under 256 KB;
  the main thread parses in 16 KB pieces for about 12 ms a run-loop turn
  (`drainBacklog`, common modes; 36 ms when a turn ends mid-frame of
  synchronized output). A shell's last output is parsed before the note
  that it ended.
- **The integration is one script for both modes** (2026-11-08):
  `resources/mutible-zsh-integration.zsh`, in the bundle's Resources and
  `build/` (the GUI checks). Terminal.mm reads it once per process and
  writes a copy beside a `.zshenv` stub (`ShellIntegration::zshenvStub`,
  `src/ShellIntegration.h`: `__mutible_mode=grid|line`, then source the
  copy) in each mode's ZDOTDIR folder, so a running app never sources a
  newer bundle's script. The folders are the process's own
  (`ShellIntegration::folderName`: `Mutible-zsh-grid-<pid>-<start>` and
  `Mutible-zsh-<pid>-<start>` in `NSTemporaryDirectory()`; 2026-11-08,
  round 6's defect 6: one shared folder let two mutibles running at once
  source each other's copy); the first terminal of a launch removes those
  of processes gone, and quitting removes its own. The old shared folders
  are left to the older mutibles using them. The private ZLE keys live in
  ShellIntegration.h.
  `make test`'s `zsh:*` groups (`tests/zsh_integration_tests.inc`) run it in
  a real interactive zsh reading a pipe (precmd and preexec run there):
  `zsh -n`, the marks of both modes, the nonce, OSC 7's encoding, and the
  exec hook case by case; skipped without /bin/zsh (CI's Linux jobs).
- **Grid mode's shell**: `zsh -l -i`, `ZDOTDIR` at the process's
  `Mutible-zsh-grid-<pid>-<start>`, whose .zshenv hands ZDOTDIR back
  and at the first precmd (after .zshrc, so last in line) installs hooks that
  only print: precmd `133;D;status` (if preexec ran), OSC 7, `133;A`; a
  `zle-line-init` hook (`add-zle-hook-widget`) `133;B`; preexec
  `633;E;<command, control characters blanked>` then `133;C`. PS1, PS2, RPS1,
  PROMPT_SP and tty modes are untouched; HIST_IGNORE_SPACE keeps
  `setDirectory:`'s ` cd` out of history; `TERM=xterm-256color`.
  `terminal.activate-venv` (both modes, 2026-11-06): the panel asks the
  window for each new terminal's environment, and `MUTIBLE_VENV` (with
  `MUTIBLE_VENV_PROMPT`) makes the first precmd do what bin/activate does,
  VIRTUAL_ENV, the bin first on PATH (after .zshrc and path_helper), PYTHONHOME
  unset, the name before PS1, a deactivate; nothing of the environment's is
  sourced or run. The interpreter is Run File's choice from the terminal's
  folder, but the folder's own .venv only once allowed by the P5 consent
  (`terminalPythonInterpreterInDirectory:waiting:`), since a prompt that runs
  git would otherwise run a planted .venv/bin/git unasked.
  Every mark ends in `;mc=<nonce>` (`__mutible_mark`): 128 random bits per
  shell in `MUTIBLE_NONCE`, which the .zshenv reads and unsets, and
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
  `noglob`). The exec's program, and nothing else on the line, gets ZDOTDIR
  as the integration folder (the user's in `MUTIBLE_USER_ZDOTDIR`) with
  `MUTIBLE_EXEC=1` (`__mutible_exec_env`): for the line, preexec puts a
  function named `exec` (`__mutible_exec`) in front of the builtin, which
  exports them just before `builtin exec` and puts the shell's back if it
  returns; the next precmd removes it (2026-11-08, round 6's defect 2:
  exported from preexec, `make && exec zsh` gave make the nonce). Functions
  are looked up at run time, so this works for a line already parsed. A
  `builtin exec` skips functions and is set up from preexec only as the
  line's first command; a line with an exec of no program (`exec >log`)
  gets no function, since a function call's redirections end with it. The
  nonce goes only to zsh (2026-11-06: a bash, or `exec claude`, kept it in
  its environment for everything it ran, any of which could forge marks; so
  a zsh exec'd from that bash no longer marks), only one that reads its
  startup files (`__mutible_reads_rcs`: not `-f` or a cluster holding f,
  `--no-rcs`, `-o norcs`, `+o rcs`, unless a later option turns them back
  on; `exec zsh -f` kept it), and not one emulating sh or ksh, which reads
  .profile instead (`__mutible_takes_nonce`: `--emulate`, or a name, exec
  -a's or the program's, starting with s, b or k after an optional r, zsh's
  own rule; `exec -a sh zsh` kept it), so an exec'd zsh loads the
  integration and reports the exec finished (133;D;0). zsh 5.9 exits when
  an interactive exec fails. Line mode also `unsetopt ZLE` there.
  Exec into another shell (bash, sh, dash, fish, ksh, tcsh, csh, nu, elvish,
  xonsh, pwsh, by `proc_name`, polled twice a second while a command runs) sets
  `_integrated` NO: not busy, title cleared, the finished notification posted,
  waiting input typed, and from then busy only while another program holds the
  foreground. A later zsh mark (`_integrationFile`) with the nonce turns it
  back on. `exec claude` still counts as running.
- **Events** (`TerminalStream` reads every byte for them): 133;B sets
  `_zleReady`; `CommandStart` carries 633;E's text as `_runningCommand`
  (titles, `runningCommands`, close questions); OSC 0/2 `Title` names the tab
  only between 133;C and the next prompt (an empty title, the prompt or exit
  restore the rule; `mutible term --title` wins). `busy`, `atPrompt`,
  `gridMode` (a program has the alternate screen), `lineMode` and
  `MCTerminalCommandDidFinishNotification` work in both modes.
- **Typing for the user** (`typeLine:run:`): first a private key, `ESC [9999~`,
  bound in emacs, viins and vicmd to `zle push-input` (`__mutible_push`), so a
  half-typed line returns at the next prompt; `runCommand:` adds Return;
  `typeCommand:` pastes in brackets when ZLE asked, never pressing Return;
  outside brackets control characters are dropped. Both go through
  `canTypeNow`: `tcgetpgrp(master)` must be the shell, and `_zleReady` (B),
  not A, since bytes sent between precmd and ZLE start echo as `^[[9999~`;
  with no B, a prompt 0.3 s old counts; either way the tty must be raw
  (2026-11-16: a B a background job printed while cat ran was still set
  when cat ended, before the shell's prompt, and `read` reads a cooked tty
  too). Typing uses the prompt up (`_zleReady`, `_atPrompt` cleared), so a
  second command waits. What cannot go in yet is queued, commands in order
  (`_pendingCommands`, one typed per prompt; `pendingCommands`) and
  `_pendingTyped`, and `flushPendingInput` tries again on every event and
  every 0.1 s. **Trap** (2026-11-16): the queue was one line, and a second
  `runCommand:` while the first still waited (a prompt between its 133;A
  and B) replaced it: the first never ran (terminal_reflow's flake on CI,
  `read` missing from the screen). **Trap** (2026-11-18): Ctrl+C typed
  while ZLE reads left the old B believed until zsh's SIGINT was handled,
  and a command typed in that moment lost its `ESC [` to the dying line
  editor and ran as `[9999~read ...` (terminal_paste's flake on CI); the
  grid's input now clears `_zleReady` and restarts the 0.3 s clock on a
  Ctrl+C, so the command waits for the new prompt's B. In a test, Ctrl+D
  then Ctrl+C to end cat raced cat's exit: the C landed at precmd and cut
  the prompt mark off; send one of them. **Trap** (2026-11-20, zsh 5.9):
  a Ctrl+C that reaches zsh while ZLE is still reading and drawing a
  paste is lost: ZLE takes it while busy, then blocks in read() for
  the next key as if none had come (zsh's signal handling, apparently
  queued until just before that read), so no new prompt comes, and the next byte typed wakes ZLE only to abort the line
  and throw that input away (terminal_paste's flake on CI, where a drop
  and Ctrl+C went in the same turn: "a fresh prompt" timed out, then
  the next command's output never came; a pty script reproduced it in
  6 of 10 tries, and in none of 40 once the paste was on the screen
  first). A person sees the paste first; a test waits until zsh has
  drawn it (`PasteDrawn`). Over the session's host it should not
  happen, since the keys one turn sends reach the pty in one write and
  the C flushes the paste unread. `busy` is
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
  to 59 MB). A cell keeps at most `kTermMaxCellBytes` (64; a skin-toned
  family emoji is 41) of character and
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
- **Widths and clusters** (2026-11-06): `termCharWidth` is tables generated
  from Unicode 16 (Mn, Me, Cf zero but U+00AD; Hangul medial and final jamo
  and skin tones zero; East Asian W and F two) between markers in
  TerminalStream.cpp by `tools/gen-term-width.py`, run by hand on the UCD
  files (fetched into a scratch folder, not kept); never edit the ranges by
  hand. The hand-written table gave 1,256 BMP combining marks a column, so
  Hebrew, Arabic, Indic and Thai lines put the cursor where ZLE did not.
  `termJoinsCluster`: a pictograph after an emoji ending in ZWJ, and a
  flag's second regional indicator, join the cell before (both models);
  VS16 after a pictograph, a ZWJ join or a flag widen a narrow cell to two
  in place (not at the right edge, not in insert mode).
- **Mode 2027 and zsh's widths** (2026-11-08): clusters (above) except
  on the main screen from a 133;A to the next 133;C, where each code point
  takes `termCodepointWidth`, zsh's own count, so typing at the prompt
  agrees with ZLE (`TerminalScreen::clusterWidths`), unless the pty has
  ISIG off (`setRawTty`, from `tcgetattr` once per `drainBacklog`): ZLE
  keeps ISIG, raw mode (cfmakeraw, Go's term.MakeRaw) drops it, so that is
  a program a widget runs, such as fzf's Ctrl+R with --height, which counts
  clusters (round 6's defect 5; the suite runs such a picker). xterm.js,
  VS Code's terminal, counts code points everywhere with Unicode 11's
  widths and joins no clusters. macOS's /bin/zsh never
  calls libc's wcwidth (`nm -u /bin/zsh` has no `_wcwidth`): it is built
  with zsh's Unicode 9 table, `Src/wcwidth9.h`, so a regional indicator is
  2, U+2764 1 and U+093F 0. `tools/gen-zsh-width.py` writes the ranges
  from that header (fetched, not kept); code points zsh has no width for
  (non-printing, unassigned in Unicode 9: it shows them as <XXXX>) fall
  back to `termCharWidth`. DECSET/DECRST 2027 choose clusters or code
  points for good (RIS undoes it); DECRQM answers what is in effect. The
  `terminal_vt` suite checks the table against zsh's `${(m)#x}` and Ctrl+A
  at the prompt. Trap: the harness can run zsh; an agent's own shell may
  be refused it by the worktree guard.
- **VT conformance** (2026-11-08, `term:vt-conformance`): DA1 `?62;22c`,
  DA2 `>1;<version as MMmmpp>;0c`, XTVERSION `DCS >|mutible <version> ST`
  (the host's `setVersion`), OSC 4/10/11/12 `?` answered from
  `setThemeColors` (Terminal.mm `tellScreenTheme`, on applySettings),
  ended with BEL or ST as asked (`TermParser::oscEndedWithBel`); DECSCUSR
  and mode 12 (the grid draws bar and underline shapes and blinks at 0.6 s
  only with the keyboard; `terminal.cursor-style` when no program chose);
  SGR 8/28, 4:0-4:5 and 21, 58/59 (underline colors as ids in the
  screen's `TermColorTable`; ids no cell uses are freed by a scan of every
  cell and scrollback run, only when a new color misses a full table and
  the work since the last scan, in printed characters, SGR bytes and
  erased cells, is a quarter of what it read: before, every SGR with "58"
  in its text rescanned, 0.1 ms each at 9,000 lines), 53/55;
  the kitty keyboard protocol with a stack per screen (16 deep, a fresh
  alternate screen starts empty, the prompt's return clears both), flag 1
  only, encoded by `encodeKey`/`encodeChar(..., keyboardFlags)`.
- **Marks on the screen** (2026-11-06): `TerminalScreen` keeps OSC 133 A, C
  and D (nonce-checked, `setMarkNonce`, as the stream) where they came, as
  a line number from `historyBase` and a column in `marks_`, moved by
  `scrollUp`/`scrollDown` (regions, IL/DL), dropped by ED 2, ED 3, RIS,
  `clearAll`, `clearToCursorLine` and the scrollback limit, carried through
  `resize` as reflow points (reflowMain looks only at each line's own
  points: thousands of marks over 100,000 lines), and an A followed by
  PROMPT_SP's wrap moves down to the prompt. `commands()`, `commandOutput`,
  `previous/nextCommandLine`. Cmd+L (2026-11-14, the owner's ask) is
  Terminal.app's Clear to Previous Mark (`clearToPreviousMark`): the last
  command that ran, prompt and output, leaves the screen and scrollback,
  what follows comes up and the scrollback comes down over the cursor;
  its marks go by order, from its prompt mark to the current one, so an
  earlier command's end mark at the start of its first line stays
  (`screen:clear-to-mark`, and the terminal fuzzer). A registry row of
  its own (`When::Terminal`; Cmd+L stays Select Line in the editor), run
  by the key monitor. The grid: Cmd+Up and Cmd+Down to the previous
  or next command that ran (outlined, its prompt at the top), Shift to
  select whole lines from where the first press started; a red bar beside a
  failed command's prompt; Terminal > Copy Last Command Output. Fixed
  `Terminal` rows in the registry, so the editor keeps its own Cmd+Up/Down
  (the file's start and end); the keys reach the grid's `keyDown:` (no menu
  item has them).
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
  alternate screen the wheel sends arrows unless the program takes the mouse.
  The scroll bar is the view's own (2026-11-14: the owner could not hit
  the old 4-point marker, which took no presses): presses in the right 14
  points while there is scrollback drag its thumb, or center it on the
  track first, and never select or reach the program; it widens with a
  track under the pointer (`scrollBarThumb:track:`, terminal_reflow). A
  double click's word is anything but blanks, brackets, quotes and `|;,` (so
  paths and URLs). Typing goes through `interpretKeyEvents:`, so dead keys and
  input methods compose; marked text wraps onto the rows below
  (`drawMarkedTextAtRow:`), since dictation composes a whole sentence.
  **Drawing is proportional to change** (2026-11-07): `screenChanged`
  takes `TerminalScreen::takeDirty()` (rows touched, lines scrolled, marks
  moved, or all) and invalidates only touched rows whose cells differ from
  what the view last drew there (`_drawn`; Ink rewrites its whole region
  every frame), plus the cursor's old and new rows; scrolling, resizes, the
  marks, the colors or font, an outlined command or marked text draw it
  all. Anything drawn from state other than the cells (selection, find,
  links, the lock) must call `setNeedsDisplay:` itself. `drawRect:` reads
  cells by reference (`row`, `lineCells`), keeps a CGColor and font per
  style, and draws a style run as glyphs placed at their columns (Core Text
  only for runs needing fallback or shaping). Trap: a CGColor taken from a
  temporary NSColor can be freed before it is retained; hold the NSColor
  with `objc_precise_lifetime` until then. `make guitest
  SUITES=terminal_draw` replays `tests/fixtures/terminal` and bounds the
  frame time.
- **Paste and passwords** (2026-11-06): `encodePaste` shows every ESC
  inside the brackets as U+241B, as xterm.js does (stripping `ESC[201~`
  once let "ESC[20" + mark + "1~" close up into a new mark). The grid's
  `paste:` asks first per `TerminalScreen::checkPaste`
  (`terminal.multiline-paste-warning`, VS Code's auto/always/never; a lone
  CR counts as a break). One line asks too when, unbracketed, it holds a
  control character other than Tab (2026-11-08: "echo hi" + Ctrl+O ran
  unasked), with Paste and Cancel only; the preview shows controls as
  U+2400 symbols, and when the first is past the first three lines' 30
  characters a line saying where it is, with the 30 characters around it
  (round 6's defect 4: the question showed none). A final line break
  ends the last line and does not start another (2026-11-18, T2: three
  copied lines asked "Paste 4 lines" with a false "…"): `lines` and the
  preview leave that empty line out, "…" only for lines left out, and
  `breaks` says whether the text sends Return, so one line and its break
  (asked under "always") is "Paste 1 line" with Paste as One Line. The question is a
  sheet on the grid's window (2026-11-16; a free dialog sat mid-screen
  where the window had been), the paste sent from its handler;
  `askingPaste` while it is up, when another paste beeps. **Trap**
  (2026-11-21): a check that pastes into a command it has just run must
  wait for the screen's `bracketedPaste()` to go off, not only for
  `busy`, which is YES as soon as `runCommand:` types (or queues) the
  line, before zsh has read it and turned bracketed paste off as ZLE
  ends: the paste went in markers, `wc` got the closing `ESC[201~` as
  text, and the Ctrl+D after it landed mid-line, so it only flushed the
  line and wc never ended (paste's flake on CI). Wait first for the
  prompt with `zleReady` and bracketed paste on, since before ZLE starts
  it is off too and a merely queued command would pass the wait. Secure keyboard entry: TerminalView polls termios
  (after output, and four times a second while a command runs) and sets
  the grid's `hiddenInput` for ECHO off with ICANON on (ZLE and full-screen
  programs turn ICANON off); the grid calls Enable/DisableSecureEventInput
  in pairs (`_secureOn`) only while it has the keyboard. Carbon is linked
  for it.
- **Drops** (2026-11-18, the owner could not drop an image into Claude
  Code): the grid registers file URLs, PNG, TIFF, strings, URLs and file
  promises. `performDragOperation:` focuses the grid (the panel's
  firstResponder observer makes it the active split) and sends, in this
  order: file URLs as `TermDrop::paths` (core, header only, `term:drop`:
  Terminal.app's backslash quoting, `$'...'` for a name with a control
  character, a space after each path); else a picture's PNG or TIFF data
  written to `$TMPDIR/Mutible-drop/drop-<date>-<n>.png` (TMPDIR as
  ControlServer reads it, so the GUI checks write to their scratch;
  files a day old are deleted at each drop); else file promises
  received into a new folder there; else the text. Everything goes
  through `pasteString:`, Edit > Paste's path: bracketed when asked, the
  paste question, `sendTyped:`, so on the session host it is
  `TermInput` without flag 1 and counts as typing for the size rule.
  A screenshot thumbnail's file (`TemporaryItems/...screencaptureui...`)
  is copied first, since screencaptureui deletes it after the drag.
  Refused (no copy cursor) while the paste question is up. Checked in
  terminal_paste (a scratch pasteboard and a fake NSDraggingInfo; zsh
  reads the paths back) and needs a real drag from Finder into Claude
  Code by eye. A row from the file tree comes with the tree's within-app
  mask (Move, Copy and Generic since 2026-11-18; Move alone was refused
  here), which terminal_paste now drives from mouse events posted to the
  outline (`docs/notes/tree.md`). Trap: quote dropped paths with backslashes, never single
  quotes. Claude Code 2.1.327 reads a pasted path by trimming it,
  taking off one outer pair of quotes and then every backslash before a
  character, and splits several at a space before "/" (or a quote and
  "/"): single quotes around each path would keep `'it'\''s'` from
  reading back, which backslashes do not.
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
- **Font**: `terminal.font-size` (12) plus the terminals' own zoom
  (`MCTerminalZoom`, see "Zoom"; never the editor's); the grid remeasures its
  cell, the input line and log re-font (keeping weight and slant), the panel
  relays out and the pty gets the rows and columns that fit.
- **Line mode**: `zsh -l -i +Z`, `ZDOTDIR` at the process's
  `Mutible-zsh-<pid>-<start>` (hooks emit 133 A/C/D and OSC 7, blank
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
  `MCTabStrip`; within a tab, columns side by side, each a stack (one level,
  as VS Code's groups), sized by shares in `TerminalLayout` (core,
  `termlayout:*`): a split halves the active pane, or evens all when a half
  would be under 20 columns (5 rows stacked), else beeps; a closed pane's
  share goes to its neighbour. A `DragBar` sits on every boundary (made once,
  each knowing its place, so its block never changes mid-drag), above the
  terminals, which are added at the back. Ctrl+Cmd+arrows
  (`resizeTerminalPane:`, tag the direction, answered by the panel in the
  responder chain) move a boundary 4 cells; Up and Down for a pane alone in
  its column resize the dock (`onResizeDock`). Hidden tabs keep their frames
  and run; the active terminal follows the keyboard through KVO on
  `firstResponder` with a weak window reference removed in
  `viewWillMoveToWindow:` (a strong one kept closed windows and their shells
  alive). Ctrl+Shift+` new tab (keyCode 50: Shift makes it ~), Cmd+\ split,
  Split Terminal Down, Option+Cmd+] and [, Ctrl+Shift+W. Rename Terminal… sets
  `TerminalView userTitle`, which wins over OSC titles and `fixedTitle`; tabs
  reorder by a drag in the strip (`MCTabStrip onMove`, `EditorTabs::dropIndex`;
  the editor's strip sets none yet) or Move Terminal Left and Right. Every
  pane with room for two cells gets its true pty size. **Trap**: the panel's
  `closeTerminal:` and `renameTerminal:` share their selectors with the menu's
  actions, so with the keyboard in a terminal the panel received Ctrl+Shift+W
  as `closeTerminal:(NSMenuItem *)` and closed nothing (until 2026-11-08);
  both take a non-terminal argument as "the active one". `make guitest
  SUITES=terminal_splits`. **The host's record** (2026-11-18, with
  `editor.session-host` on or headless; docs/notes/session-host.md,
  "Terminal splits in the record"): the panel's `onChanged` says when
  something the record keeps changed (a tab added, closed, moved or shown,
  a split, a pane closed or given the keyboard, a keyboard resize, a bar's
  drag ended: `DragBar onDragEnd`, on mouse up), and `sessionDock:` gives
  the tabs as host ids with their `TerminalLayout::Layout` and active pane;
  while a bar is dragged its tab's shares are the ones from before the drag
  (`_dragTab`, `_dragLayout`), so the record moves once per drag.
  `restoreHostDock:fresh:` builds a window's held terminals as the record
  had them (`Shared::termDockFrom`), each pane made in `TermDock::order()`.
- **Closing** (`TerminalView close`, `stopShell`, `_closed`): a terminal with
  `runningCommands` asks (since 2026-11-15, as the user closes it, the
  tab's ×, its menu, Cmd+W, Ctrl+Shift+W: `closeTerminalAsking:` and
  `closeTabAsking:`, a sheet on the window, the close in its handler,
  `_askingClose` so a second press only beeps; Close, Cancel, and Show in
  Processes, which shows the Processes panel with the terminal's
  `foregroundJob` or its group's first process selected,
  `revealTerminal:job:`, and keeps the terminal; `closeTerminal:` and
  `closeTab:` still answer at once with runModal, for code and checks): a command since 133;C, a foreground process group
  other than the shell's (`tcgetpgrp`, `proc_name`), a `runCommand:` still
  waiting, and background jobs (processes with the pty as controlling tty,
  `KERN_PROC_TTY`, in other groups, named from `KERN_PROCARGS2`;
  `KERN_PROC_SESSION` answers nothing on macOS). `&!` jobs count too. Window
  close and quit ask once for all; idle shells close unasked.
- **Run File** (Shift+Cmd+Return): `Settings::runTemplate` filled by
  `RunFile::expand` (`shellQuote`, or `quoteInside` when the template quotes
  the placeholder); `{python}` is the LSP session's interpreter. It saves
  first, goes through `runCommand:` (so into history), never types into a busy
  terminal (a new tab instead), and leaves the keyboard alone
  (`runFileCommand:inDirectory:busyNote:` in +Panels).
- **Running one test** (2026-11-16, C5; `Runnables.{h,cpp}`, `runnables:*`,
  `TestGutter.{h,mm}`, `EditorController+Tests.mm`, `make guitest
  SUITES=test_gutter`): Zed's runnables and VS Code's Testing icons. The
  core finds tests without a server (pytest in test_*.py, *_test.py and
  tests.py, go test in *_test.go, cargo's #[test] and #[...::test] in .rs
  with their `mod`s, Jest and Vitest test( and it( with a quoted name in
  *.test.*, *.spec.* and __tests__, inside their describe( blocks), reading
  the text with comments and strings blanked by
  `SyntaxHighlighter::highlight`'s tokens, and builds the command:
  `test.<language>` (python, go, rust, js) or the built-in template, with
  Run File's placeholders and {project}, {path}, {tests}, {pattern},
  {package}, {target}, {runner} and {filter} (lists of words, an empty one
  dropped with its space). The command runs in {project} (go.mod,
  Cargo.toml, package.json or a Python project file up from the file):
  `runFileCommand:` puts `cd` first unless the terminal's OSC 7 folder is
  already it. `MCTestGutter` (one per window, made in showWindow) finds the
  tests off the main thread 0.3 s after typing and gives the gutter's test
  lane a mark per test; a click (or Run Test at Cursor, `testing.runAtCursor`,
  no key: VS Code's is a chord) runs it as Run File does, and the run is
  matched to the terminal's `MCTerminalCommandDidFinishNotification`, whose
  userInfo now carries 133;D's exit status and the command's line
  (`MCTerminalExitStatusKey`, `MCTerminalCommandKey`, read before the prompt
  mark clears it): 0 is a check, anything else a cross. Results are kept
  per file and test until an edit that is not a whole new text (a tab
  switch). The GTK port has it too (2026-11-18, `linux/HANDOFF.md`
  item 96: `linux/src/TestGutter.{h,cpp}`, the marks in each buffer's
  `BufferLines`, a run matched to its terminal, shell and the
  integration's count of commands started); Android has none yet.
- **Links** (Cmd+click; `TermLinks`): resolved against the shell's cwd (OSC 7),
  the window folder, its worktrees and the sessions' scratchpads
  (`AgentIndex::resolveTarget`): one match opens, several get a picker, none
  names the roots tried; paths resolved so /private/tmp matches a /tmp root.
  The log checks `characterIndexForPoint:` against the character's own rect (it
  answers past a line's end); a flagsChanged monitor catches Cmd pressed
  without moving. Without Cmd, a rest of 0.5 s on a link (a timer each
  `mouseMoved:` re-arms, so no lookup per move) shows "⌘-click to open"
  beside the pointer (`showRestHint`, both modes; with a hyperlink's
  address on a second line), never while a program takes the mouse or a
  selection is dragged; a move, a press, a key, a scroll, or leaving the
  view clears it. With Cmd held over a hyperlink its address shows there
  at once. **Trap** (round 9, 2026-11-19): until then the hint was the
  view's `toolTip`, and the checks read that property, so they passed
  while no user ever saw it: AppKit shows a tooltip only after a move
  inside the view once it is set, and here it was set after the pointer
  had rested and cleared by the next move. The hint is now `MCHoverHint`
  (HoverHint.h), a subview in the tooltip material that clicks go through;
  the checks read `linkHintText` and the subview's frame. The link
  hit-test is the core's (`TermLinks::find` and `at`, through `linkAt`).
  The hint's words are the core's `TermLinks::restHint` ("Ctrl+click to
  open" on GTK, `linux/HANDOFF.md` item 125, where the hint is a label in
  the terminal's overlay placed by `get-child-position`, and OSC 8 links
  come from VTE's `vte_terminal_check_hyperlink_at`). A URL goes to `openWebAddressFromTerminal:` (the user's
  page, pane shown quietly with no home page, keyboard kept; 0.0.0.0 and [::]
  become localhost, `TermLinks::browserAddress`).
- **OSC 8** (grid only; the line-mode log ignores them): `TerminalScreen`
  keeps a table of addresses (http, https, file; 2 KB; no spaces or controls)
  and puts the id in `TermStyle::link` of each printed cell, so links survive
  the scrollback and reflow; the open link is kept apart from SGR. 4096 ids at
  most; when full, ids no cell (screen, scrollback, `below_`) uses are freed,
  else a new link prints plain. TermStyle's flags and underline style are
  `uint16_t` bit-fields, and the link and underline color `uint16_t` ids,
  so it stays 16 bytes (a StyleRun per style change in the scrollback; the
  scrollback-memory check caught 24; `term:sgr-underline` checks the size). Cmd+click on a cell with an id opens its
  address (file:// only on this host and when the file exists, through
  `TermLinks::hyperlink`), the span is the cells with that id in the wrapped
  line, and the address is the view's tooltip while Cmd hovers.
- **BROWSER** (2026-11-06): every shell gets `BROWSER=<bundle>/Contents/
  Resources/mutible-browser.js` (`resources/`; build/'s copy for the GUI
  checks), a file that is sh and JavaScript at once: Claude Code (on a clicked
  link, with mouse reporting on) and Python's webbrowser exec it with the
  address; Vite and CRA run a BROWSER ending in .js with node, their own
  arguments first (with any other value Vite would run `open -a <value>`).
  Both hand the last argument to `mutible-cli open-url`, which sends pages
  that are not local (`ControlChannel::opensInPane`: sign-in pages refuse an
  embedded web view) and every page when `MUTIBLE_PID`'s mutible does not
  answer to `/usr/bin/open` (`MUTIBLE_OPEN_FALLBACK` in tests, a recorder),
  and never launches mutible. The `open-url` command loads the user's page in
  the window of `MUTIBLE_TERMINAL_ID`. The npm `open` package and `open`
  itself ignore BROWSER.
- Testing: a scratch `HOME` and `ZDOTDIR`, or commands land in the user's
  `~/.zsh_history`; an app-typed command snaps a scrolled view, so test "stays
  on its text" with output from a command already running.

## Terminals owned by the session's host (stage 5)

SESSION-DESIGN.md's stage 5, core half (2026-11-17; `HostPty.{h,cpp}`,
`SessionHost`, `SessionProtocol`, `SessionClient`, `TerminalScreen::replay`,
`tests/session_terminal_tests.inc`). The GTK port runs on it
(linux/HANDOFF.md item 90); the Mac's grid terminals do with
`editor.session-host` on (below, and docs/notes/session-host.md,
"Terminals on the host").

- **The pty** (`Shared::Pty`): `posix_openpt`, the slave opened and its
  modes (the kernel's plus IUTF8) and size set in the parent, then
  `fork`; the child resets signals, `setsid`, `TIOCSCTTY`, closes every
  other descriptor and execs, with argv, the environment and the program's
  path (looked up in the spec's PATH) prepared before the fork. MCPty's
  rules hold: the master non-blocking, unwritten bytes waiting in order for
  `flush` (the host polls for writing while `wantsWrite`), a hang-up that
  drops them, closes the master and sends SIGHUP, and `reap` polling
  `waitpid` with SIGKILL 2 s after the hang-up. `exited` notices a shell
  gone while a program it started still holds the pty open (VTE's
  child-exited does the same); the terminal ends with the shell then.
- **The host's thread does the I/O.** `SessionHost::loop` waits in `poll`
  on a pipe `post` writes to and every pty `Host::pollSet` names, takes
  every queued message first, then reads 64 KB per readable pty a turn, so
  a key never waits behind `yes`. `Host::tick` reaps hung-up shells (every
  50 ms while any wait) and looks at each pty's foreground group and modes
  every 200 ms (and after each read), so `TermShell::foreground` is at most
  that old.
- **Per terminal** the host keeps a headless `TerminalScreen` (the
  scrollback the renderer asked for, the integration's nonce) and an
  events-only `TerminalStream` with the same nonce; `applyTermEvent` turns
  the stream's events into a `TermShell` (known, busy, at a prompt, the
  line editor reading, counts of prompts, commands started and finished,
  the last status, the running command's text, the folder, the title),
  sent when it changes. `TermMarks` sends every marked command (prompt,
  output, end, status) as lines counted from the first that ever went into
  the scrollback, after the output that moved them; `TermOutput` carries
  the host's cursor line after its bytes, so a renderer maps a mark to its
  own emulator's rows through the cursor (VTE's rows are counted the same
  way). **The host's screen answers no queries**: `takeReplies` is thrown
  away, since the renderer's emulator answers DA, DSR and the rest through
  its input, and two answers would reach the program.
- **Credit**: output not yet acknowledged (`TermAck`) past `kTermCredit`
  (1 MB) takes the pty out of the poll set, so the program blocks, as the
  Mac's reader stops past 1 MB; a renderer acknowledges once its emulator
  has the bytes. A renderer that goes (`disconnect`) leaves its terminals
  running unthrottled, nobody hearing them, until stage 6 attaches again.
- **Replay** (`TerminalScreen::replay`, ~300 lines): bytes that rebuild the
  screen in a fresh terminal of the same size and scrollback limit, fed to
  a renderer that attaches (`TermAttach`, answered by `TermReplay`,
  `TermShell` and `TermMarks`). Every scrollback and screen line is written
  at the bottom with its SGR and OSC 8 link, soft wraps reproduced by the
  receiver's own autowrap (a wide character's wrap with a wide character,
  written over when the line no longer starts with one; a bottom row
  wrapping onto itself under a scroll region that leaves it out), the
  marks emitted where they were (`;mc=<nonce>`), then the alternate screen
  written whole, the cursors and saved cursors (a cursor waiting to wrap
  by writing its cell again), the region, tab stops, character sets,
  modes, keyboard flags, title, folder and open link. **Traps found by its
  fuzz** (`screen:replay`, 300 rounds; 24,000 clean while writing it): a
  prompt mark makes the next wrap zsh's PROMPT_SP, so each written A is
  followed by a carriage return; `promptActive_` (zsh's widths) follows
  marks the screen does not keep, so it is set at the end with a mark
  written on the alternate screen; a line entered by a wrap was erased in
  the colors the wrap was written in, so its blanks are erased again with
  the default ones (EL); a mark on a wide character's right half, at a
  wrapped line's start, or after the last column each need the cursor put
  there without writing. Left out: lines a shrink set aside (`below_`),
  and the wrap of a bottom row that a wrap continued onto (and of any
  wrapped bottom row on a screen of two rows), which only a scroll could
  make.

- **The GTK port on it** (linux/src/Terminal.cpp, item 90): VTE has no
  pty. `spawnShell` opens a `TermSpec` through `SessionLink::client()`;
  `termOutput` is fed to VTE (`vte_terminal_feed`) and acknowledged at low
  priority once the main loop is idle; VTE's "commit" signal, which it
  emits with no pty too (VTE issue 222), carries keys, pastes,
  `feed_child`, `paste_text` and VTE's answers to queries to the host's
  pty. The shell integration (bash's script and the zsh script without its
  old VTE branch) prints OSC 133, 633;E and 7 with the nonce, as on the
  Mac; D10's termprop is gone. Ctrl+Up and Ctrl+Down move between
  commands: a command's VTE row is its host line plus VTE's cursor row
  minus the host's cursor line (`rowOfLine`), both counting every line
  from the first. The waiting dot goes when a key's commit reaches the
  shell (a key counts for 200 ms, an input method's commit coming late),
  so Ctrl+Shift+C and Ctrl+Up leave it. **Trap** (2026-11-20): a new
  view's first shell must start at its grid, so `makeTerminal` waits for
  VTE's first layout (`spawnWhenLaidOut`: the first frame VTE has a
  width, a second at most). It spawned at once, at the 80x24 of a VTE not
  yet laid out, and was told its size a frame or more later; on CI's
  slower runs bash drew its first prompt (91 columns there, wrapped at 80)
  before that, and readline keeps where a prompt wraps and which of its
  invisible bytes (vte.sh's OSC 133 marks in PS1) fall on which row from
  when it drew it, so at 105 columns every redraw put the cursor 9 columns
  off and `term --run 'echo hi'` was echoed early and written over: the
  prompt, then `hi`, and no `echo hi` (shell_integration). Waiting for
  the pty's size before typing does not help: the prompt is already
  drawn. **Trap** (2026-11-21): the same miscount comes back whenever
  the width changes under a prompt that wraps at one width and fits at
  the other (a font change, the panel resized, the soak's agent_panel
  after another suite moved it), since its cause is the invisible bytes:
  so the bash integration takes vte.sh's two marks out of PS1 (their
  exact text, only with the nonce; ours print from PROMPT_COMMAND and PS0
  and are never in PS1). A user's own `\[...\]` bytes in PS1 still go
  wrong the same way; that is readline's.
- **The GTK port's cursor and Control keys** (2026-11-18,
  linux/HANDOFF.md item 110). The cursor is the terminal's text color
  and the character under the block the background's, both opaque
  (`Settings::terminalCursor` and `terminalCursorText`, as the Mac's grid
  draws it; no built-in theme has a cursor color), set in
  `Terminal::applyColors` after `vte_terminal_set_colors` on every
  settings change, with the selection (`theme::editorSelection`). **Trap**:
  `vte_terminal_set_colors` unsets the cursor, cursor text, highlight and
  bold colors; the port set the cursor once before it, and VTE, with only
  the cursor's text set, drew the block and the unfocused outline in the
  cell's background: no cursor showed on any theme until then. Keys: while
  the terminal has the keyboard, every plain Ctrl key but the Global ones
  (Ctrl+`, 0, 1, 2, I, Tab, Page Up and Down) is no accelerator
  (`Commands::linuxTerminalTakes`, Keys.cpp's `bound`), so readline's
  Ctrl+W, A, E, U, K, R, L, D, C, Z, Y, P, N, B and F reach the shell, as
  Control does on the Mac; the harness found them all reaching it before
  item 110 too. VS Code's terminal on Linux does the same apart from its
  `terminal.integrated.commandsToSkipShell` (Quick Open's Ctrl+P and
  Ctrl+E, its terminal find's Ctrl+F, chords such as Ctrl+K while
  `allowChords` is on), which mutible gives the shell. A Mac-style remapper
  (Toshy, Kinto) turns the physical Ctrl into Super in any window it does
  not list as a terminal, so on the owner's ThinkPad a physical Ctrl+W
  reaches mutible as Super+W: the fix is a keymap in the remapper for
  `ai.ible.Mutible`, or the setting below. Checked by `terminal_keys`
  and `terminal_draw` (the GTK harness).
- **Mac-style keys in the GTK terminal** (2026-11-19, linux/HANDOFF.md
  item 113): `terminal.mac-style-keys` (Linux only, off by default) is for
  a Mac-style remapper (Toshy, Kinto), which sends the key beside the
  space bar, the Mac's Cmd, as Ctrl and the physical Ctrl as Super in any
  window it does not list as a terminal. With it on, while the terminal
  has the keyboard, the keys do what they do in the Mac's Terminal:
  - **Ctrl is the Mac's Cmd.** `Commands::linuxTerminalTakes(c, k, true)`
    gives no key with Ctrl to the terminal but VTE's Ctrl+Shift+C and V,
    so Keys.cpp's `bound` (told by `Keys::setTerminalMacStyle`, from
    `onSettingsChanged` and at startup) binds every Ctrl key of a command
    that applies there: Ctrl+W, F, S, P, N, O, B, Q, R, T, comma,
    Ctrl+Shift+G and Ctrl+Shift+Space too. Close Tab with the terminal's
    keyboard closes that pane (`act_close_tab`, as the Mac's
    `closeFocusedTerminal`: a running program asks first, the last tab
    hides the dock), only with the setting on, so the File menu's Close
    Tab is unchanged with it off. What no accelerator took reaches the
    terminal's capture controller (`Terminal::macStyleKey`, core
    `Commands::linuxMacStyleKey`): Ctrl+C copies the selection (nothing
    without one), Ctrl+V pastes as Ctrl+Shift+V does
    (`Terminal::pasteClipboard`), Ctrl+A selects all, as the Mac's grid
    does for Cmd+C, V and A; any other Ctrl key that types a character
    (Shift or Alt with it too) is dropped, so it never reaches the
    program. Ctrl with a key that types nothing (Ctrl+Left, Ctrl+Backspace,
    Ctrl+F4 once unbound) goes on to VTE as before, and Ctrl+Up and
    Ctrl+Down still move between commands, as Cmd+Up and Down do.
  - **Super is the program's Ctrl.** Super with a character sends the byte
    VTE sends for Ctrl with it (`Commands::terminalControlByte`), through
    `vte_terminal_feed_child`, so it goes by VTE's commit to the host as a
    key does, counted as typing: every letter (Super+W ^W, Super+C ^C,
    Super+R ^R), with Shift the same (a terminal sends one byte for Ctrl
    and Ctrl+Shift with a letter), Super+[ Esc, Super+] ^], Super+\ ^\,
    Super+Shift+- ^_, Super+Space and Super+Shift+2 NUL, Super+Shift+6
    ^^, Super+/ ^_, Super+3 to 8 Esc, ^\, ^], ^^, ^_ and DEL, each what
    VTE itself sends for Ctrl with the key (terminal_keys reads VTE's
    bytes for all fourteen with the setting off). Super with Alt
    and a letter sends the same: Toshy's general keymap turns Super+A, E,
    B, F, N, P, K and D into Home, End, the arrows, Shift+End Backspace and
    Delete (so the physical Ctrl+A, E, B, F, N and P arrive as keys
    readline takes the same way, never as Super), and GNOME grabs Super+L
    to lock the screen, so Toshy's keymap for VS Code's terminal sends the
    physical Ctrl+K, D and L as Super+Alt+K, D and L, and a keymap for
    mutible's window can do the same. Super with Ctrl (the Mac's
    Ctrl+Cmd), or with a key that types nothing, goes on to VTE.
  - The hints panel's "In the terminal" part says so while it is on. The
    editor, the other panes and the hints search box (which never takes a
    key with Super) are the same with it on or off, and so is everything
    with it off (`terminal_keys`'s first half runs with it off,
    unchanged).
  Checked by `terminal_keys` (VTE's own bytes for Ctrl with readline's
  punctuation against `terminalControlByte`; with the setting on Super's
  bytes from a raw `head` and VTE's commit, the dropped Ctrl keys, Super+W
  at a prompt, Super+Alt+K, Super+C on `sleep 100`, Ctrl+A, C and V
  through the clipboard, Ctrl+W on a terminal tab and on an editor tab,
  the hints, and off again) and core `commands:linux-terminal`.
- **The Mac on it** (2026-11-17, with `editor.session-host` on, grid mode
  only): `TerminalView` keeps everything above (its own `TerminalStream`
  and `TerminalScreen` parse the same bytes, so the integration, nonce,
  marks, typing queue, titles and links are unchanged) and changes only
  where the bytes come from and go: `startShell` sends the same argv and
  environment as a `TermSpec`, and waits (`settle`) for the shell's pid;
  `MCHostTermListener` hands output to the backlog, acknowledged as each
  slice is parsed; `sendBytes` (keys, pastes, typed commands) is
  `terminalInput`, and the screen's replies go apart as answers
  (`sendReplies:`, flag 1); `updateWindowSize` is `resizeTerminal`, and
  the screen takes the pty's size from `TermShell` where it came in the
  output, the grid clipped or padded to the view (`placeGrid`;
  docs/notes/session-host.md, "Terminals on the host", **Size**).
  What read the master (`tcgetpgrp`, `tcgetattr` for ICANON, ECHO and
  ISIG) reads the host's `TermShell` through `ptyForeground`, `ptyRaw`,
  `ptyHidesInput` and `ptySignalsOff`. Line mode keeps MCPty.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- The terminal: Traps: paste only once the tty is raw; check reaping with `kill(pid, 0) == ESRCH`, not `waitpid`.
- The terminal: **Trap**: GTK suites that wanted a fresh shell typed `exit` and waited for the restart (`shell_integration`, `terminal_marks`); they now make a new terminal and close the others.
- The terminal: **Trap** (2026-11-16): the queue was one line, and a second `runCommand:` while the first still waited (a prompt between its 133;A and B) replaced it: the first never ran (terminal_reflow's flake on CI, `read` missing from the screen).
- The terminal: **Trap** (2026-11-18): Ctrl+C typed while ZLE reads left the old B believed until zsh's SIGINT was handled, and a command typed in that moment lost its `ESC [` to the dying line editor and ran as `[9999~read ...` (terminal_paste's flake on CI).
- The terminal: **Trap** (2026-11-20, zsh 5.9): a Ctrl+C that reaches zsh while ZLE is still reading and drawing a paste is lost: ZLE takes it while busy, then blocks in read() for the next key as if none had come, so no new prompt comes, and the next byte typed wakes ZLE only to abort the line and throw that input away (terminal_paste's flake on CI).
- The terminal: Trap: the harness can run zsh; an agent's own shell may be refused it by the worktree guard.
- The terminal: **Trap** (2026-11-21): a check that pastes into a command it has just run must wait for the screen's `bracketedPaste()` to go off, not only for `busy`, which is YES as soon as `runCommand:` types (or queues) the line, before zsh has read it and turned bracketed paste off as ZLE ends: the paste went in markers, `wc` got the closing `ESC[201~` as text, and the Ctrl+D after it landed mid-line, so it only flushed the line and wc never ended (paste's flake on CI).
- The terminal: Trap: quote dropped paths with backslashes, never single quotes.
- The terminal: **Trap** (round 9, 2026-11-19): until then the hint was the view's `toolTip`, and the checks read that property, so they passed while no user ever saw it.
- The terminal: Trap: a CGColor taken from a temporary NSColor can be freed before it is retained; hold the NSColor with `objc_precise_lifetime` until then.
- The terminal: **Trap**: the panel's `closeTerminal:` and `renameTerminal:` share their selectors with the menu's actions, so with the keyboard in a terminal the panel received Ctrl+Shift+W as `closeTerminal:(NSMenuItem *)` and closed nothing (until 2026-11-08); both take a non-terminal argument as "the active one".
- Terminals owned by the session's host (stage 5): **The host's screen answers no queries**: `takeReplies` is thrown away, since the renderer's emulator answers DA, DSR and the rest through its input, and two answers would reach the program.
- Terminals owned by the session's host (stage 5): **Traps found by its fuzz** (`screen:replay`): a prompt mark makes the next wrap zsh's PROMPT_SP, so each written A is followed by a carriage return.
- Terminals owned by the session's host (stage 5): **Trap** (2026-11-20): a new view's first shell must start at its grid, so `makeTerminal` waits for VTE's first layout (`spawnWhenLaidOut`: the first frame VTE has a width, a second at most).
- Terminals owned by the session's host (stage 5): **Trap** (2026-11-21): the same miscount comes back whenever the width changes under a prompt that wraps at one width and fits at the other (a font change, the panel resized, the soak's agent_panel after another suite moved it), since its cause is the invisible bytes: so the bash integration takes vte.sh's two marks out of PS1 (their exact text, only with the nonce; ours print from PROMPT_COMMAND and PS0 and are never in PS1).
- Terminals owned by the session's host (stage 5): **Trap**: `vte_terminal_set_colors` unsets the cursor, cursor text, highlight and bold colors; the port set the cursor once before it, and VTE, with only the cursor's text set, drew the block and the unfocused outline in the cell's background: no cursor showed on any theme until then.
