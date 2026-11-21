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
clicks, ScreenCaptureKit and Vision read the screen.

- **Running it**: `make taskbench-mac` drives the apps (it takes the screen;
  ask the owner first, and never run it unattended on the owner's Mac).
  `./scripts/taskbench-mac.sh --list` prints the task table and each app's
  path, `--show <task>` each app's steps, `--check` the Accessibility
  permission, the apps found and their versions, and the disk; none of the
  three drives anything. `APPS`, `TASKS`, `LOG_MB` (500), and `KEEP=1` (keep
  the scratch folder) narrow or keep a run; `TASKBENCH_DIR` moves the
  scratch folder (`/private/tmp/mutible-taskbench`; its name must hold
  "taskbench"), which two agents running it at once need (one emptied the
  other's on 2026-11-20); `KEEP_APP=1` leaves the last task's app running
  to look at (`axdump`, `screentext`, `run --pid`); `TASKBENCH_TRACE=DIR`
  writes each step's CPU samples and every frame Vision reads. The terminal
  it runs in needs Accessibility permission (`taskbench-mac check` says so
  when it is missing), and Screen Recording for results read from the
  screen (without it they read as unverified). Report:
  `build/taskbench-mac.txt` (a table per task), `build/taskbench-mac.csv`
  (a row per task and app), and `build/taskbench-mac-steps.csv` (every
  step's time, its resolution, and how its end was observed). Repetitions
  are left out (the owner, 2026-11-20).
- **The tasks** (the table's twelve; add a row and a `steps_<id>_<app>` per
  app). Ours: review the uncommitted changes and stage one hunk (`git diff
  --cached`), open a PDF, PNG, MP4, CSV, notebook, and SQLite file (each read
  as "shown", "opens as text", or "refused"), stop a server left running on a
  port (the port closes), open a localhost page in the app (the server logs
  the request), open a 500 MB log cold and find a line near its end, and open
  one file from Finder cold to a window with the keyboard in the text.
  Neutral ones (2026-11-20, "tasks we did not choose, where VS Code and Zed
  are expected to be strong"): go to a definition in another file (F12,
  the window's title), rename a function across three files (F2, checked
  on disk), replace a string in three files (checked on disk), open app.py's
  version from an older commit (its line read through AX or from the
  screen), switch to a light theme (the window's brightness, read from the
  screen in all three), and rename a variable used four times with a cursor
  on each (Shift+Cmd+L, checked on disk). The fixture (a git repo with four
  commits, changes in two files and a new one, the media, `site/index.html`,
  `report.py` and `orders.py` for the neutral tasks, a lone file) is made
  fresh for every task and app; the log only for its task, and deleted
  after.
- **Measures**: a step's wall time runs from the last event its input
  posted (the moment an AXPress is sent, which can block while the app
  handles it) to the moment its result happened, summed per task. For a
  condition read by polling (AX, a file, git, a port) that is when the poll
  found it, every 10 ms (never a fixed sleep), so a poll's length is its
  resolution (VS Code's AX tree takes longest). For a `quiet` condition it is
  when the app's CPU activity stopped, and for a screen read when the first
  frame showing the result was on screen (below), so the benchmark's own
  waiting is never in the wall time (2026-11-20: run 2 put the 150 ms quiet
  window and the OCR into every Zed step, so typing "report.pdf" scored 1,172
  ms). The steps CSV has each step's `res_ms` (its resolution) and
  `observed`: ax, file, git, port, cpu-quiet, or screen. Inputs are clicks,
  key chords, typed keystrokes (one a character), and Finder opens; CPU is
  user + system time of the app's process tree over the steps
  (`proc_pid_rusage`, with the time of children it reaped, and the last seen
  time of launchd-parented helpers that exit). Memory is the tree's physical
  footprint, sampled every 10 ms by a thread from the launch through the
  settle (once the tree is quiet for a second), so the peak is over all of
  it: run 2 sampled only during steps, and mutible's finder task showed a
  settled 265 MB over a peak of 100. The report asserts peak ≥ settled and
  fails (exit 5, "MEASURING FAULT") on a row where it is not. The tree is
  membench's: the app and every process whose parent or responsible pid is
  in it, so WebKit's and Electron's helpers and the language servers count. A
  task on the project launches the app first, timed apart as its launch
  (its settle back-dated as a step's is); a cold task's first input starts
  the app, so its launch is in the task.
- **Quiet and settled**: an app at rest is not always at zero (Zed redraws
  about every 12 ms while a picker is open), so quiet is judged against the
  CPU's own settled level: each 10 ms sample's CPU, against the median of
  the quiet window; a sample at least 1 ms over it is busy, except a lone
  one under 2 ms over (a caret's blink); a window is quiet when its median is
  under 20% of a core and nothing in it is busy. `quiet MS` reads the app's
  own process (its UI answering; a language server starting beside Zed held
  its go-to-line step 700 ms when the whole tree was read), `settle MS` and
  a `view`'s settle the whole tree (helpers render views). Zed's and VS
  Code's caret blink is off in their quiet settings (`cursor_blink`,
  `editor.cursorBlinking: solid`): Zed's blink costs about 5 ms a blink, as
  much as a real answer. mutible's AppKit caret blinks under the threshold.
- **Fairness**: each app takes its shortest documented path with its own
  default keys, cited beside its steps in the script; a task an app cannot
  do is "cannot" with the reason (VS Code and Zed have no process view,
  Zed no browser). Opening a file by name is Cmd+P in all three, though a
  tree click is shorter in each, because Zed's tree cannot be reached
  through AX. What an app needs once (VS Code's folder trust, its PDF,
  SQLite, and Python extensions, counted as their GUI path's inputs) is
  setup, counted apart and not timed; VS Code's extensions are a task's own
  (`$VSEXT/<task>`), so one task's extension does not run in another's.
  Settings that only keep an app quiet (no update checks, no telemetry, no
  caret blink) are not counted. The language server tasks: mutible and Zed
  both run the owner's installed basedpyright (`~/.local/bin`, which each
  found with nothing set up for it), VS Code Pylance from the
  Python extension. F12 and F2 are pressed once the cursor is on the name,
  as a person presses them, so a server still starting is in the step;
  waiting for the tree to settle first was dropped, since VS Code's stayed
  busy 30 to 90 s while Pylance indexed, which F12 does not wait for.
- **Reading an app with no AX tree** (2026-11-20): Zed is read from its
  pixels. A ScreenCaptureKit stream of the app's windows delivers a frame
  each time they change (30 a second at most), stamped with when it was
  shown; `screen TEXT`, `!screen TEXT`, and `luma>X` ask the newest frame,
  and once one holds, the frames since the last that did not are halved down
  to the first that does: the step ends at that frame's time. The stream is
  started, with a frame in, before the input of a step that reads the
  screen. Vision (the accurate level, else the fast one) reads the front
  window's part of a frame; a `view` whose window has no AX tree is
  classified from it ("refused (on screen: "Could not open file")", "opens
  as text (on screen: ...)", "shown (read from the screen)"), and `sclick
  TEXT` clicks where TEXT shows (Zed's File History list, mutible's commit
  graph). `taskbench-mac screentext PID` prints what Vision reads,
  `capturetest PID` how long the stream takes to start.
- **Isolation**: VS Code on `--user-data-dir` and `--extensions-dir`, Zed on
  `--user-data-dir` (1.29 has it) and a scratch HOME, mutible on a scratch
  HOME, CFFIXED_USER_HOME, TMPDIR and defaults suite as the GUI checks use;
  every app is started with `open -n`, so a copy the owner has open is never
  used, and only the pids it started are stopped (`taskbench-mac quit`
  checks the executable first; never `pkill`). VS Code also gets
  `--use-mock-keychain` and `--force-renderer-accessibility` (Traps) and
  `editor.accessibilitySupport: "on"` in its quiet settings: the mode it
  switches to by itself once an AX client reads it, without the
  notification asking; "off" hides the editor's text from AX, so a file
  opened as text could not be told from one shown. Zed is skipped while the
  owner's Zed runs, since a second Zed may hand its requests to it. Setup
  runs once per app on a template profile; tasks on the project run on a
  copy of it, cold tasks on a fresh profile with only the quiet settings.
- **The owner first**: the run waits until HIDIdleTime says the keyboard and
  mouse have been idle 15 s, and stops at once (exit 3, a partial report) on
  any input event it did not post: the guard file holds the time of the last
  event it accounts for, the owner's before the run and then each event the
  tool posts, and an event newer than that is someone at the Mac. Keys and
  clicks are posted only while the app being driven is in front (asked
  through AX each time), so nothing is ever typed into another app. Two
  runs at once stop each other this way (each sees the other's events).
- **Caveats**: AX reads only what an app exposes. Zed shows almost no AX
  tree beyond its window's title, so its results are read from the screen,
  its title, or from outside (git's index, the port, files on disk), and its
  palette steps end on its CPU (`quiet`); where none can tell, the result is
  "unverified" and marked * in the table (Zed's finder task: its text is
  read from the screen, the keyboard's place cannot be). mutible 1.6.9
  gave the keyboard back to the file tree when Quick Open closed, its
  commit graph's rows carried no AX text, and its Option+Shift+Cmd+F with
  nothing searched yet put the keyboard in the replacement field (found
  2026-11-20), so its multicursor path paid Cmd+1, its history click was
  read from the screen, and its replace path searched first with
  Shift+Cmd+F. All three were fixed the same day, and those steps take the
  direct path: Cmd+P ends with the keyboard in the text (AXTextArea
  focused), history presses the AXCell named by the commit's subject, and
  replace is Option+Shift+Cmd+F, the search, Tab, the replacement.
  mutible has no history of one file: its history task uses the branch's
  commit graph. Chromium's AX tree costs VS Code some CPU and memory, and
  every app pays for being queried. Chords are posted by US key positions.
  Every step was watched on screen on 2026-11-20 (one app and task at a
  time, `KEEP=1 KEEP_APP=1`, `axdump`, `screentext`); a step that differs
  from the app's docs says what was watched beside it in the script.
- **Working out a step**: `taskbench-mac run --pid PID` drives a copy a
  kept run left running (it must be the `--exe`'s), so a step can be tried
  without a relaunch; `axdump PID [DEPTH] [manual]` (`manual` sets
  `AXManualAccessibility` on an Electron app started without the flag and
  waits 3 s for its tree); `screentext PID` and `windows PID` for Zed;
  `ocr PNG` for a frame `TASKBENCH_TRACE` kept.

## Traps

- **An app in AppTranslocation** (2026-11-20): VS Code, unzipped into
  /Applications and still quarantined, runs from
  `/private/var/folders/.../AppTranslocation/<uuid>/d/Visual Studio
  Code.app`, so a pid matched against `/Applications/...` by its exact
  executable path was never found: no AX, no measures, and `quit` never ran,
  which left six scratch copies running. `ExeMatches` (and the script's
  `running`) accept a translocated copy of the same bundle. Never clear the
  owner's quarantine attribute to "fix" it.
- **VS Code under a scratch HOME blocks on the keychain**: its safeStorage
  asks for "Code Safe Storage" at startup; with no login keychain under the
  scratch HOME macOS shows "Keychain Not Found" (SecurityAgent, on the
  owner's screen, one per launch) and VS Code's main thread waits on it,
  ignoring SIGTERM. The first run's 90 s launch timeouts were this.
  `--use-mock-keychain` (Chromium's switch) avoids it. If such a dialog is
  ever left up, press only its Cancel; "Reset To Defaults" resets the
  owner's keychain.
- **Electron's AX tree**: set `AXManualAccessibility` on a starting app and
  the request is lost; set later, the tree takes seconds, and meanwhile
  the app lists its application element among its windows and as its own
  child (a cycle that filled every walk to 40,000 nodes) and fails AX calls
  with kAXErrorCannotComplete; VS Code 1.107 never reads the attribute back
  as true. Start Electron apps with `--force-renderer-accessibility`
  instead: the tree is there from the first window. `Walk` visits each
  element once and does not descend into an application or menu bar
  element.
- **Frontmost through AX fails while VS Code is busy**: the system-wide
  `AXFocusedApplication` asks the front app, and VS Code answered
  kAXErrorCannotComplete, so every key was refused as "would not come to
  the front" though VS Code was in front. `Frontmost` falls back to the
  owner of the frontmost window (CGWindowList).
- **VS Code's keyboard element is unreliable**: with Quick Open up it
  reported a static text of the page behind as focused. Check its fields
  by their description and value (`ax AXTextField~Search files by name`,
  `ax AXTextField=NAME`), not `focus`.
- **A click at a row that has just appeared** landed on the row that took
  its place as VS Code's Source Control view filled in (Commit, not
  app.py; it asked to commit everything). A click now waits for its
  target's frame to hold still (60 ms, 2 s at most), and that wait is in
  the step's wall time.
- **Webviews keep the keyboard in VS Code**: after the PDF viewer (pdf.js)
  Cmd+P does nothing (pdf.js takes it for print), and after the video
  preview Quick Open comes up with the keyboard still in the player; the
  steps use Go > Go to File… and a click in the field there, as a person
  would.
- **Zed asks "Unrecognized Project" (Restricted Mode)** for every folder
  and for a file opened cold, in a modal that AX cannot read and that
  takes every key. The first run's Zed failures (Cmd+Y staged nothing,
  Cmd+F found no bar) were this, not Zed. Setup answers it once (Return,
  "Trust and Continue", kept in the profile the tasks copy); a cold task
  answers it as a counted input.
- **Zed's Project Diff opens above its first hunk**: the first Cmd+Y (git:
  stage and next) only moves to the hunk; the second stages it.
- **VS Code's Simple Browser asks for `/?id=...&vscodeBrowserReqId=...`**,
  so the page condition greps `GET /[ ?]`, not `GET / `.
- **A magic that the viewer itself prints**: Zed's image viewer shows
  "PNG" in its size line, which read as "opens as text"; the PNG's magic is
  its IHDR chunk name.
- **A view's time runs until the app is quiet**: `view` waits for the
  window title, then for the tree to settle for 300 ms (or 3 s; quiet as
  Measures says), so work an app does after drawing counts. mutible's PDF at
  2,210 ms (1.6.9) was drawn in about 30 ms; the rest was PDFKit's
  Vision pass over the pages (viewers.md, PDFs). `sample PID` during the
  step says what keeps it busy.
- **Vision's accurate level read nothing** in a frame of Zed's image viewer
  (a large picture beside the tabs and the tree) while its fast level read
  every line: `Recognise` falls back to the fast level when the accurate
  one finds no text (the picture read as "unverified" until then).
- **A wait for the tree to settle is not a wait for the app**: Zed redraws
  every frame while a picker is open, its caret blink costs as much as an
  answer, and a language server starting beside it kept its whole tree busy
  for 700 ms after a go-to-line that showed at once. Quiet is relative to
  the settled level, UI steps read the app's own process, and the caret
  blink is off (Measures).
- **VS Code's editors show no text through AX until one has the keyboard**
  (the diff editor a Timeline click opens): `text` found nothing there and
  walking every text area for it took 32 s; its history step reads the
  screen.
- **AXPress blocks while the app handles it** (299 ms for VS Code's Replace
  button): a step's wall starts when the press is sent, not when the call
  returns, or the app's time would hide in the input's.
- **Two agents running taskbench at once** shared
  `/private/tmp/mutible-taskbench`, and one's `rm -rf` emptied the other's
  run; each also sees the other's posted events as the owner's. Give each
  its own `TASKBENCH_DIR`, and check `ps` for a running `taskbench-mac run`
  first.
