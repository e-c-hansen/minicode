# Demo GIFs and the memory benchmark

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

`scripts/demos.sh` builds `/private/tmp/minicode-demo` (a 7-commit copy of
`demo/` with fixed dates and a local git identity, a fresh HOME, ZDOTDIR and
CFFIXED_USER_HOME per scene with a short zsh prompt and a `.vimrc`,
`tools/demo-settings.conf` (the grid terminal), two made-up Claude Code
sessions from `tools/demo-agents/make-transcripts.py`, a copy of the tectonic
cache), runs the raw binary with `MINICODE_DEMO=<scene>` under `env -i`,
kills only its own pid after 2 min, and runs `build/makegif`.
`MINICODE_DEMO=list` lists scenes; `DEMO_KEEP_FRAMES=<dir>` keeps frames and
the log. A scene ends with `exit(0)` once its frames are written, so a file
it left unsaved never holds it on the quit question.

- A scene is a function in `mac/src/Demo.mm` of queued builder steps (`clickFile:`,
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
by tectonic with the PDF on screen, one web page) on minicode and on the
usual alternative, and sum each side's memory; each system's files are
named for it (`-mac`, `-linux`).

- **Mac**: `scripts/membench-mac.sh` and `tools/procmem-mac.c` (built as
  `build/procmem-mac`): VS Code + Chrome + Preview against minicode, summing
  the physical footprint (`proc_pid_rusage`; 320 MB against 1,283 MB on
  2026-09-21, in the README). A process belongs to a side by parent or
  responsible pid (WebKit's helpers are launchd's children); a shell-launched
  app makes the terminal responsible, so minicode starts with `open -n -g
  --env`. `mac/src/Bench.mm` resolves paths like the tree; a tiny VS Code
  extension types the command; `MINICODE_BENCH` skips activation. Report:
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
  (`MINICODE_BENCH`, inert otherwise) opens the C++ file, shows the terminal
  and the page, opens the LaTeX preview in a second window as the Mac's
  does, and prints `minicode bench: ready`. Language servers are turned on
  by the scratch `settings.conf` in `MINICODE_SETTINGS`. Report:
  `build/membench-linux.txt`, with each side's process list in
  `build/membench-linux-*.procs`. Scratch data is under
  `/tmp/membench-linux`; processes are stopped by recorded pid, never
  `pkill`. First complete run, 2026-11-14 (settled 60 s, 5 samples):
  957 MB against 2,081 MB (`linux/HANDOFF.md` item 67; in the README's
  Memory section beside the Mac's, each table compared only within itself,
  since PSS and the physical footprint count differently).

Check each side's breakdown before quoting a number (a first Mac run
"measured" 55 MB with clangd and WebKit missing).

## Traps

None recorded under a "Trap" label; read the sections above.
