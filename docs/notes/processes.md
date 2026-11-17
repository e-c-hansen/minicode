# Processes and ports

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

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
- **The panel** (Shift+Cmd+J, minicode's own key since 2026-11-15): a
  process row shows a Stop button at its right while hovered (a tracking
  area) or selected (`outlineViewSelectionDidChange:` sets each row view's
  `selected`), which runs `stopRow:` with its questions; the memory figure
  moves left for it.
- **Mac**: `MCProcessMonitor` starts itself at launch (`+load` observes
  didFinishLaunching), samples on its own serial queue every 10 s, 2 s while
  a panel shows; at each look it asks AppDelegate's `controlControllers` for
  folders (`realpath`, not `stringByResolvingSymlinksInPath`, which turns
  /private/tmp into /tmp while the kernel reports /private/tmp) and
  terminals (`MCControlTerminalID`, `TerminalView shellPid`). CPU times from
  `proc_pid_rusage` are Mach ticks: convert with the timebase. Stop checks
  each pid's start time before SIGTERM and again before the SIGKILL 3 s
  later. `minicode procs` samples once more before answering, and never
  opens a window for `--folder`. Session (R7.4): each terminal's session is
  the attention board's (the last `minicode notify`, with its time); the
  Tracker takes the newest child of the shell started before that notify
  (Claude Code holds the foreground while its hooks run) as the session's
  process, or the shell itself after `exec claude`, and a process under it
  gets the session once, when first seen there, kept after (a `/clear`'s
  new session labels only what starts after it; a program run by hand
  after Claude Code quit is nobody's). In `procs --json` and the row's
  tooltip.
- **Checks**: the `processes` suite starts a server (python3 found with
  `MCFindProgram`, else `nc -lk`) in its own terminal, disowns it, closes
  the terminal, stops the orphan; a sleep for Stop's question; a server
  started by `sh` that exits at once (launchd its parent) for "not started
  here"; times five looks with at least 200 processes. Trap: in `sh -c 'cd
  d && prog &'` the `&` takes the whole list, so a subshell holds the
  pipes and is the parent; write `cd d || exit; prog &`.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- Processes and ports (Mac): Trap: in `sh -c 'cd d && prog &'` the `&` takes the whole list, so a subshell holds the pipes and is the parent; write `cd d || exit; prog &`.
