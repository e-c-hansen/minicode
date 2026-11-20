# Demo GIFs, the memory benchmark, and the task benchmark

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## Demo GIFs (`make demos`)

**The GIFs come from CI, not from a hand** (2026-11-14,
`.github/workflows/demos.yml`): on a `v*` tag, or run by hand from the
Actions tab (a `scenes` input), the macOS job plays every scene on the
runner's own display and the Linux job every `_demo_` suite, and the last
job (`scripts/demos-propose.sh`) commits the GIFs that changed on a
`demos/<tag>-<run>` branch and opens a pull request against main (or prints
the link where Actions may not open one), then starts CI on that branch by
hand (`gh workflow run`): a push or pull request made with the workflow's
own token starts no workflow, so the pull request would never get "CI
passed", which main requires. release.sh does not wait for it.
The macOS runner (a VM, 1024x768 at 1x, display sleep off) lets
`screencapture -l` work, since its TCC database grants /bin/bash Screen
Recording; frames are 1x. Never run `make demos` on the owner's Mac
unasked: it puts windows on the screen. The GTK scenes are underscore suites
in `linux/tests/gui/DemoSuite.cpp`, recorded with ffmpeg's x11grab on the
harness's Xvfb and written to `docs/demos/linux/` (one palette a GIF).

`scripts/demos.sh` builds `/private/tmp/mutible-demo` (a 7-commit copy of
`demo/` with fixed dates and a local git identity, a fresh HOME, ZDOTDIR and
CFFIXED_USER_HOME per scene with a short zsh prompt and a `.vimrc`,
`tools/demo-settings.conf` (the grid terminal), two made-up Claude Code
sessions from `tools/demo-agents/make-transcripts.py`, a copy of the tectonic
cache), runs the raw binary with `MUTIBLE_DEMO=<scene>` under `env -i`,
kills only its own pid after 2 min, and runs `build/makegif`.
`MUTIBLE_DEMO=list` lists scenes; `DEMO_KEEP_FRAMES=<dir>` keeps frames and
the log. A scene ends with `exit(0)` once its frames are written, so a file
it left unsaved never holds it on the quit question.

- A scene is a function in `mac/src/Demo.mm` of queued builder steps (`clickFile:`,
  `type:`, `key:caption:`, `waitFor:timeout:recorded:`, ...), points resolved
  lazily (`pointForText:` through `firstRectForCharacterRange:`). Input is real
  posted `NSEvent`s; the pointer and key captions are an overlay view. Every
  scene says what each step is for with `label:` (2026-11-17, the owner's
  review: a first-time viewer could not tell): blue, right-aligned in the
  tab strip's row, kept until the next label; key captions (the keys
  pressed) stay at the bottom center. The user's own input is swallowed meanwhile (only events whose timestamp, in
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
  tectonic. Terminal scenes find text on the grid (`TermPoint`, from
  `TerminalScreen::lineText`); git's pager runs with LESS=FRX, which keeps the
  main screen, so wait on `busy`, not `gridMode`. Posted keys with modifiers
  go between flagsChanged events (`postFlags:`): without the release AppKit
  kept Shift as held. On the runner a posted Cmd+P opened the command palette
  rather than Go to File, and Shift+Cmd+A never reached the menu (the agents
  scene falls back to the menu's action); neither has been seen by hand.

## Memory benchmark (`make membench-mac`, `make membench-linux`)

Two scripts run one workload (a copy of the repo as the project,
tests/run_tests.cpp open with clangd, a shell, demo/paper/notes.tex typeset
by tectonic with the PDF on screen, one web page) on mutible and on the
usual alternative, and sum each side's memory; each system's files are
named for it (`-mac`, `-linux`).

- **Mac**: `scripts/membench-mac.sh` and `tools/procmem-mac.c` (built as
  `build/procmem-mac`): VS Code + Chrome + Preview against mutible, summing
  the physical footprint (`proc_pid_rusage`; 320 MB against 1,283 MB on
  2026-09-21, in the README). A process belongs to a side by parent or
  responsible pid (WebKit's helpers are launchd's children); a shell-launched
  app makes the terminal responsible, so mutible starts with `open -n -g
  --env`. `mac/src/Bench.mm` resolves paths like the tree; a tiny VS Code
  extension types the command; `MUTIBLE_BENCH` skips activation. Report:
  `build/membench-mac.txt`.
- **Linux**: `scripts/membench-linux.sh` and `tools/procmem-linux.c`
  (`build/procmem-linux`): the VS Code snap, Chrome and GNOME's Papers
  against a release build of the GTK port (`linux/build-bench`, made with
  `meson setup build-bench --buildtype=release`), summing PSS, the `Pss:`
  line of `/proc/<pid>/smaps_rollup`, which splits each shared page between
  the processes mapping it (WebKitGTK's and Chrome's libraries are counted
  once between them; RSS would count them per process). A side is the
  launched root and its descendants by `/proc`'s parent pids, plus any
  process sharing a `snap.*` systemd scope with one of them (a snap's
  launcher can leave its children to init; `code --wait` keeps them under
  it). The Mac needs the responsible pid because launchd starts helpers on an
  app's behalf; here a helper is a child, so the tree is enough. Everything
  runs in a private `Xvfb` with each side under its own `dbus-run-session`
  and GTK, Chrome and Electron on X11 (no window on the owner's screen);
  `MEMBENCH_DISPLAY=desktop` uses the session's display, run by the owner
  only. A virtual display has no GPU, so Chrome, Electron and WebKit draw in
  software and their numbers shift somewhat. `linux/src/Bench.cpp`
  (`MUTIBLE_BENCH`, inert otherwise) opens the C++ file, shows the terminal
  and the page, opens the LaTeX preview in a second window as the Mac's
  does, and prints `mutible bench: ready`. Language servers are turned on
  by the scratch `settings.conf` in `MUTIBLE_SETTINGS`. Report:
  `build/membench-linux.txt`, with each side's process list in
  `build/membench-linux-*.procs`. Scratch data is under
  `/tmp/membench-linux`; processes are stopped by recorded pid, never
  `pkill`. First complete run, 2026-11-14 (settled 60 s, 5 samples):
  957 MB against 2,081 MB (`linux/HANDOFF.md` item 67; in the README's
  Memory section beside the Mac's, each table compared only within itself,
  since PSS and the physical footprint count differently).

Check each side's breakdown before quoting a number (a first Mac run
"measured" 55 MB with clangd and WebKit missing).

## Task benchmark (`make taskbench-mac`)

The same tasks, driven by script in mutible, VS Code, and Zed, compared by
numbers rather than by an agent's walk (2026-11-20, the owner's words:
"grab the cpu usage, memory usage, number of clicks, and total walltime and
use them to compare each approach"). `scripts/taskbench-mac.sh` holds the
task table, each app's steps, the fixture, and the report;
`tools/taskbench-mac.m` (built as `build/taskbench-mac`) drives one app
through one task's steps and measures it. No third-party tools: the
Accessibility API finds elements and reads results, CGEvent posts keys and
clicks.

- **Running it**: `make taskbench-mac` drives the apps (it takes the screen;
  ask the owner first, and never run it unattended on the owner's Mac).
  `./scripts/taskbench-mac.sh --list` prints the task table and each app's
  path, `--show <task>` each app's steps, `--check` the Accessibility
  permission, the apps found and their versions, and the disk; none of the
  three drives anything. `APPS`, `TASKS`, `LOG_MB` (500), and `KEEP=1` (keep
  `/private/tmp/mutible-taskbench`) narrow or keep a run. The terminal it
  runs in needs Accessibility permission (`taskbench-mac check` says so when
  it is missing). Report: `build/taskbench-mac.txt` (a table per task),
  `build/taskbench-mac.csv` (a row per task and app), and
  `build/taskbench-mac-steps.csv` (every step's time).
- **The tasks** (the table's six; add a row and a `steps_<id>_<app>` per
  app): review the uncommitted changes and stage one hunk (`git diff
  --cached`), open a PDF, PNG, MP4, CSV, notebook, and SQLite file (each read
  as "shown", "opens as text", or "refused"), stop a server left running on a
  port (the port closes), open a localhost page in the app (the server logs
  the request), open a 500 MB log cold and find a line near its end, and open
  one file from Finder cold to a window with the keyboard in the text. The
  fixture (a git repo with changes in two files and a new one, the media,
  `site/index.html`, a lone file) is made fresh for every task and app; the
  log only for its task, and deleted after.
- **Measures**: wall time per step from the input sent to its condition
  true, polled every 10 ms (never a fixed sleep), summed per task (a poll
  that walks a large AX tree, as VS Code's is, takes longer, and the steps
  CSV's `poll_ms` gives each step's slowest poll: its resolution); inputs as
  clicks, key chords, typed keystrokes (one a character), and Finder opens;
  CPU as user + system time of the app's process tree over the steps
  (`proc_pid_rusage`, with the time of children it reaped, and the last
  seen time of launchd-parented helpers that exit); memory as the tree's
  physical footprint, the peak over the steps (100 ms samples) and settled
  (once the tree has used under 5% of a core for a second). The tree is
  membench's: the app and every process whose parent or responsible pid is
  in it, so WebKit's and Electron's helpers count. A task on the project
  launches the app first, timed apart as its launch; a cold task's first
  input starts the app, so its launch is in the task.
- **Fairness**: each app takes its shortest documented path with its own
  default keys, cited beside its steps in the script; a task an app cannot
  do is "cannot" with the reason (VS Code and Zed have no process view,
  Zed no browser). Opening a file by name is Cmd+P in all three, though a
  tree click is shorter in each, because Zed's tree cannot be reached
  through AX. What an app needs once (VS Code's folder trust, its PDF and
  SQLite extensions, counted as their GUI path's inputs) is setup, counted
  apart and not timed; settings that only keep an app quiet (no update
  checks, no telemetry) are not counted.
- **Isolation**: VS Code on `--user-data-dir` and `--extensions-dir`, Zed on
  `--user-data-dir` (1.29 has it) and a scratch HOME, mutible on a scratch
  HOME, CFFIXED_USER_HOME, TMPDIR and defaults suite as the GUI checks use;
  every app is started with `open -n`, so a copy the owner has open is never
  used, and only the pids it started are stopped (`taskbench-mac quit`
  checks the executable first; never `pkill`). Zed is skipped while the
  owner's Zed runs, since a second Zed may hand its requests to it. Setup
  runs once per app on a template profile; tasks on the project run on a
  copy of it, cold tasks on a fresh profile with only the quiet settings.
- **The owner first**: the run waits until HIDIdleTime says the keyboard and
  mouse have been idle 15 s, and stops at once (exit 3, a partial report) on
  any input event it did not post: the guard file holds the time of the last
  event it accounts for, the owner's before the run and then each event the
  tool posts, and an event newer than that is someone at the Mac. Keys and
  clicks are posted only while the app being driven is in front (asked
  through AX each time), so nothing is ever typed into another app.
- **Caveats**: AX reads only what an app exposes. Zed shows almost no AX
  tree, so its results are mostly read from its CPU settling (`quiet`,
  reported as "unverified" and marked * in the table) or from outside (git's
  index, the port). mutible's review page and processes rows were drawn
  without AX elements until 2026-11-20 (the review task staged from the
  editor, the server task picked the list's second row); now its review
  task presses "Stage change 1 in app.py" and its server task the Stop
  named "on port N", as VoiceOver would. Turning on Chromium's AX tree
  (`AXManualAccessibility`) costs VS Code some CPU and memory, and every app
  pays for being queried. AX cannot see mutible's palette rows (drawn) or
  Zed's list, so in every app the step after typing in a palette waits for
  the app to settle (150 ms of quiet), and that window is in each wall
  time. Chords are posted by US key positions. The
  conditions were written from each app's docs and source, not yet watched
  on screen: a first run with the owner present should check each step with
  `taskbench-mac axdump <pid>` where a condition times out, and adjust the
  table.

## Traps

None recorded under a "Trap" label; read the sections above.
