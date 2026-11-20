# Verification detail

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## Verification reality (important) (the rest)

(Moved from CLAUDE.md's "Verification reality (important)".)

- **What a suite leaves** (2026-11-14, both harnesses): the runner
  compares the world as each suite starts and ends (editor windows,
  sheets, terminals, settings.conf, keys.conf, the zooms, Show Hidden
  Files, the tree's columns, a key window left pinned, alert answers
  left unused; on GTK app windows, a window on top, terminal tabs, the
  two files, a pin) and prints `LEAK [suite]`, naming the suite that
  left it rather than the one that later fails (daily_work's pinned key
  window failed five git suites on CI). It fails that suite and puts
  back what it can (the pin, the answers). The two files are the
  harness's to put back between suites, and so are the terminals a suite
  opened and left (closed unasked, 2026-11-18, after control, which
  counts its terminals from none, failed in every soak round behind
  run_file, terminal_vt or daily_work): `LEFT [suite]`, a warning, so a
  suite needs no step to undo its settings or close its shells, and no
  suite may count on a terminal an earlier one left. A browser pane or agent page shown or hidden in any window is
  a LEFT line too (2026-11-17, after control's agent page narrowed
  markdown_side_sync's editor), until the suites that leave one close
  it; then it can be a LEAK. On GTK, Find in Folder's scope, files to
  include and exclude and options are a LEFT line too (2026-11-19), put
  back by `MCGuiResetFolderSearch`, which a suite that searched calls
  before it ends, and so is the project folder left a git repository
  (2026-11-19: agents' git init, worktree and branches), removed by
  `MCGuiRemoveProjectRepository` with the worktrees git lists inside it,
  which a suite that made it one calls before it ends. A browser pane
  left on show is a LEFT line on GTK too (2026-11-19, after browser_tabs
  left one and tab_strip's row was half as wide), hidden as View >
  Toggle Browser hides it, and so is an agent page left open
  (2026-11-20, after terminal_splits' `split --right browse` left one
  and browser_pages, which checks `page --text` with none, failed
  behind it), closed. The suites that left either (browser_tabs,
  browser_pages, browser_find, bookmark_save, bookmark_menu, bookmarks,
  terminal_splits) put them back as they end (`MCGuiHideBrowserPane`).
  `MC_GUI_LEAKS=note` prints and changes nothing; "_" suites
  are skipped. A failed check with `MC_GUI_SNAPSHOTS` set leaves a
  picture of the test window (the first ten); CI keeps them as an
  artifact, a folder per half.
- **What a suite needs, it sets up** (2026-11-19, GTK `preview_tabs`,
  which failed in all three rounds of that day's soak). Trap: in CI's
  order a suite runs after the same few suites every time, so it can
  pass for months on what they happen to leave and fail in every
  shuffled order. preview_tabs ran sixth in CI's order, with the
  project nearly empty, and found its tree rows by name among the rows
  on screen; after 28 to 74 other suites, the folders they had opened
  in the tree put p-a.txt far below the tree's last row, and its Find
  in Folder still had replace_in_folder's (or latin1_files') scope and
  files to include, so "needle" found nothing. A suite finds a tree row
  by its path, brought into view (`MCGuiBringTreeRow`, then
  `MCGuiTreeRow`), and sets every part of a shared panel it relies on
  (`MCGuiResetFolderSearch`, then its own scope) rather than counting on
  a new window's; the LEFT lines name only what the harness looks at.
  What a suite makes in the shared project it takes away: agents left
  the project a git repository, with a worktree under .claude, so in
  later suites every tree row was "Untracked (git)" with a tooltip and a
  Worktrees node sat at the tree's top; it now removes the repository,
  the worktree and its files, and waits until the tree has neither.
- **A tooltip takes the next click** (2026-11-19, GTK, soak #127). Trap:
  GTK shows a widget's tooltip just below a resting pointer, as an X
  window of its own over the app's, and XTest's pointer jumps where a
  hand's would pass through the row first and put the tooltip away.
  preview_tabs' click on p-a left p-a's "Untracked (git)" up over p-b,
  and its click on p-b landed on the tooltip: the tree saw no press, and
  every later click on a row went the same way. `MCGuiOnScreen` cannot
  see it (it asks GTK what its own window draws there). The harness
  hides a tooltip of the app's that X has over the point before any
  click, press, drag or wheel goes there (`uncover` in Input.cpp), with
  a note ("a tooltip (...) over the point to click, hidden first");
  anything else over the point is left for the check to see.
- **A long run on a loaded machine** (2026-11-19, GTK, soak #127, round
  2's order replayed beside three other agents' builds). Traps, each a
  wait that held only on a quiet machine:
  - GDK hands the window one X event a turn of the main loop, so a wait
    on the first effect of `MCGuiType("mine ")` (the tab dirty) can pass
    with "ne " still to come, and the next step's tab gets them
    (auto_save's other.txt read "one other.txt"). Wait for all of it.
  - A window's VTE has its 80 x 24 grid until GTK first lays the window
    out, and its shell can start before that (host_wrap read 80 x 24 as
    the view's grid). Wait for the grid its frame holds.
  - Work the main thread does itself (GTK laying out 470,000 lines) is
    bounded by its CPU time, not the clock: `MCGuiWaitCpu(what, cpu,
    wall)` fails after `cpu` seconds of the main thread's CPU, the clock's
    `wall` only ending a wait nothing moves. large_file's layout took 13
    to 44 s of CPU and 14 to 470 s of the clock. The editor's settling
    (`settleOnPlace`, `settleOnCaret`) has limits of the clock of its own
    (three minutes, a second without growth): with eight busy loops on
    one core it ended early and the place was lost, still open.
  - "The main thread idle" as its CPU standing still for half a second
    (`MCGuiWaitQuiet` over `MCGuiThreadCpuMs() / 20`) never comes while a
    text view with the keyboard blinks its caret: GTK 4 fades it, at 60
    frames a second, for 10 s after the last key (20 to 85 ms of CPU a
    second here), and a loaded machine makes each frame dearer
    (gutter_typing, after fifty suites). Wait for what the idle time was
    for (gutter_typing: the last line colored).
  - A tab a suite leaves open costs every later suite: word_wrap's
    300,001-line file took over a second of the main thread each time a
    later suite's settings were put back. Close what you open.
- **Two harnesses at once** (2026-11-17, CI's halves): each process makes
  its own world, so nothing a suite touches is shared. Mac: `/tmp/mcgui-*`
  by mkdtemp, home, TMPDIR (so control sockets; `MUTIBLE_CONTROL_DIR`
  is only set by suites, inside their scratch), the defaults suite and
  pasteboard named by pid, `CLAUDE_CONFIG_DIR`; the Python interpreter
  choice was the one app state in the standard defaults (the `guitest`
  domain, shared by every process of that name) and now goes through
  `MCAppDefaults()` like the rest. Ports: servers bind port 0 and keep
  the socket, or take one from `FreePort()` and hand it to `serve.py`;
  macOS gives TCP ephemeral ports out in turn
  (`net.inet.tcp.randomize_ports` 0), so a released port is not handed to
  the other process before the range wraps. Launch Services is never
  touched (openURL: swizzled, BROWSER's fallback a recorder); the recent
  folders the app notes go to the system's list for the `guitest`
  identity, which no check reads. GTK: Xvfb picks its own display
  (`-displayfd`), each run has its own dbus-run-session, scratch, XDG
  folders, TMPDIR and `GSETTINGS_BACKEND=memory`, and processes are
  found by walking this run's descendants or a token, never by name. A
  suite that fails only beside the other half is a sharing bug to fix
  here, never a retry.
- **Seeded orders and one retry** (2026-11-17, both harnesses,
  `tests/GuiRetry.h`): `MC_GUI_SHUFFLE=<seed>` plays the selected suites
  in an order drawn from the seed (splitmix64 and Fisher-Yates of our
  own, so libc++ and libstdc++ agree and a seed replays anywhere); the
  soak workflow prints a seed per round. `MC_GUI_RETRIES=<n>` (CI's 3):
  the run writes what failed to `MC_GUI_RESULTS` as it ends (per suite,
  with the first failure; failures in no suite counted apart), and the
  process that started it (the Mac's first stage, which now forks and
  waits instead of exec'ing the run; GTK's wrapper) runs each failed
  suite once more as a fresh run of the whole program, its world, and
  on GTK its display, new. A pass is a `FLAKY [suite]` line and a passing
  run. A suite that depends on what an earlier suite left passes its
  retry only if it sets that up itself: a suite must stand alone, which
  the seeded orders test too. `_x` children get an empty `MC_GUI_RESULTS`
  and `MC_GUI_SHUFFLE`, so they never write over their parent's record.
- **Environment variables in a suite** (2026-11-17): only `MCSetEnv(name,
  value or nil)`, never setenv or unsetenv, which raced with git and the
  other workers copying the environment for their children (the GTK
  harness corrupted its heap that way); `env:raw-access` in `make test`
  holds mac/tests/gui to it. See docs/notes/build.md, "The environment
  and starting programs".
- **A stand-in holds a stage until the suite has seen it, never for a
  time** (2026-11-19, agent_panel): a stage a stand-in program keeps up
  with `sleep` is a pause in disguise. On CI, at the start of the job,
  a GUI process's main thread was once kept away 7 to 9 s (the other
  half's first suite ran 8 s long at the same moment) while the stand-in
  agent's stream of a few seconds went by, so the waits never saw the
  1.2 s it held "Reading util.py…". The stand-in now waits at a gate
  for a file the suite writes once its wait has seen that stage
  (`GatedStream`, `Gate` in AgentPanelSuite.mm), and goes on by itself
  after 30 s so a failed suite is not held.
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
  `MUTIBLE_LSP_LOG` at a scratch log and kills only the PIDs it started (the
  user's own mutible and other sessions run clangd too). Older traps: an arrow
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
- **CI's macOS runner differs** (2026-11-06): its windows are at 1x, its
  scroll bars always take room (no trackpad), and a reverse DNS lookup of
  127.0.0.1 can stall. `MC_GUI_SCROLLBARS=always` gives the scroll bars here;
  compute pixel checks from `backingScaleFactor`, never from a picture's
  width; local servers in checks are `mac/tests/gui/serve.py` (http.server
  without the getfqdn it makes between bind and listen, which kept the port
  closed and the banner unprinted past the waits). An exception AppKit cannot
  recover from (one in `drawRect:`) prints `FATAL exception in [suite] (step:
  ...)` with its reason and stack before the crash. Use `MCMonoFont`, never
  `+[NSFont monospacedSystemFontOfSize:weight:]` directly: it returns nil now
  and then while other threads make fonts, and a nil in an attributes
  literal threw inside the git panel's row drawing.
- **Ad hoc**: a temporary `getenv("MUTIBLE_*TEST")` block run headless with a
  poll-and-kill loop (no `timeout` on macOS), removed by exact text. Read state
  by KVC; drag NSSplitView by posting drag and up events, then calling
  `mouseDown:`; `hitTest:` measures grab areas.
- **Pixels**: `/usr/sbin/screencapture -x -o -l<windowNumber>` from inside the
  app, read with `NSBitmapImageRep colorAtX:y:` (y from the top, Retina scale);
  `renderInContext:` on a layer gives exact colors where `cacheDisplayInRect:`
  shifts them. The only way to verify translucency. `CGWindowListCreateImage`
  is gone in the macOS 15 SDK.
- **GTK from the Mac: Docker.** `linux/dev/gtk-dev.sh` builds the image
  (ubuntu:26.04, CI packages, Xvfb, xcompmgr, xdotool, ImageMagick) and a
  container with the repo mounted; `linux/dev/README.md` has the workflow.
  Build into `/build` inside; `Xvfb :99` plus `xcompmgr`; launch with
  `GDK_BACKEND=x11 GTK_A11Y=none` under `dbus-run-session`, a scratch HOME, and
  `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` (bubblewrap cannot start there),
  and `--foreground` when a terminal runs it (else the command returns once
  the window is up, `dbus-run-session` stops its bus, and the app quits:
  `linux/HANDOFF.md` item 23).
  Drive with `xdotool`, capture with `import -window root` (dotted debris is
  xcompmgr's). `pkill -x mutible`, never `pkill -f` with a path (it kills the
  `docker exec`). The image is about 1.8 GB on a tight disk. **Docker trap**:
  `open -g -a Docker` raised an administrator password prompt and the daemon
  never started; never start Docker unattended.
- **GTK on the ThinkPad** (Ubuntu 26.04, GNOME on Wayland; check
  `$WAYLAND_DISPLAY`): `meson test -C build`; `setsid gtk-launch
  ai.ible.Mutible &` then `pgrep -a mutible` (a real window: say so, leave
  none behind); `systemctl --user list-units --type=scope | grep mutible`
  (`app-gnome-ai.ible.Mutible-*.scope` proves the dock association,
  `app-mutible-ai.ible.Mutible-*.scope` a start from a terminal, a
  nested `vte-spawn-*.scope` the terminal); `journalctl --user --since "-15
  min"`. Real input: a private rootful Xwayland driven by XTest
  (`linux/HANDOFF.md`, "How to work on it"); prefer it to hooks, which passed
  three bugs the user then hit by hand.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- Verification reality (important) (the rest): **What a suite needs, it sets up**: in CI's order a suite runs after the same few suites every time, so it can pass for months on what they happen to leave and fail in every shuffled order.
- Verification reality (important) (the rest): **A tooltip takes the next click**: GTK's tooltip is an X window below a resting pointer, and XTest's pointer jumps onto it; the harness hides one over the point before pointer input goes there.
- Verification reality (important) (the rest): **A long run on a loaded machine**: GDK hands the window one X event a turn of the main loop, so wait for all of a typed string; VTE is 80 x 24 until the window's first layout; bound the main thread's own work by its CPU (`MCGuiWaitCpu`); a caret blinking keeps the main thread from ever standing still; close the tabs a suite opens.
- Verification reality (important) (the rest): **A stand-in holds a stage until the suite has seen it, never for a time**: a stage a stand-in program keeps up with `sleep` is a pause in disguise.
- Verification reality (important) (the rest): **Harness traps**: key-monitor keys go through `[NSApp sendEvent:]` with the window's number (`MCGuiSendKeyToApp`); menu keys as CGEvent-made events through the window then the main menu (`MCGuiPerformMenuKey`, built from the key code; setting the unicode string made the menu refuse a matching key); tree keys through `[window ...
- Verification reality (important) (the rest): Older traps: an arrow key event needs the function-key character (U+F701...) as `characters`, and an empty string makes `-[NSMenu performKeyEquivalent:]` throw;
- Verification reality (important) (the rest): **Docker trap**: `open -g -a Docker` raised an administrator password prompt and the daemon never started; never start Docker unattended.
