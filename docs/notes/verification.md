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
  harness's to put back between suites and an extra terminal is only
  noted: `LEFT [suite]`, a warning, so a suite needs no step to undo its
  settings. A browser pane or agent page shown or hidden in any window is
  a LEFT line too (2026-11-17, after control's agent page narrowed
  markdown_side_sync's editor), until the suites that leave one close
  it; then it can be a LEAK. `MC_GUI_LEAKS=note` prints and changes nothing; "_" suites
  are skipped. A failed check with `MC_GUI_SNAPSHOTS` set leaves a
  picture of the test window (the first ten); CI keeps them as an
  artifact, a folder per half.
- **Two harnesses at once** (2026-11-17, CI's halves): each process makes
  its own world, so nothing a suite touches is shared. Mac: `/tmp/mcgui-*`
  by mkdtemp, home, TMPDIR (so control sockets; `MINICODE_CONTROL_DIR`
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
  `MINICODE_LSP_LOG` at a scratch log and kills only the PIDs it started (the
  user's own minicode and other sessions run clangd too). Older traps: an arrow
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
- **Ad hoc**: a temporary `getenv("MINICODE_*TEST")` block run headless with a
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
  xcompmgr's). `pkill -x minicode`, never `pkill -f` with a path (it kills the
  `docker exec`). The image is about 1.8 GB on a tight disk. **Docker trap**:
  `open -g -a Docker` raised an administrator password prompt and the daemon
  never started; never start Docker unattended.
- **GTK on the ThinkPad** (Ubuntu 26.04, GNOME on Wayland; check
  `$WAYLAND_DISPLAY`): `meson test -C build`; `setsid gtk-launch
  org.minicode.Editor &` then `pgrep -a minicode` (a real window: say so, leave
  none behind); `systemctl --user list-units --type=scope | grep minicode`
  (`app-gnome-org.minicode.Editor-*.scope` proves the dock association,
  `app-minicode-org.minicode.Editor-*.scope` a start from a terminal, a
  nested `vte-spawn-*.scope` the terminal); `journalctl --user --since "-15
  min"`. Real input: a private rootful Xwayland driven by XTest
  (`linux/HANDOFF.md`, "How to work on it"); prefer it to hooks, which passed
  three bugs the user then hit by hand.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- Verification reality (important) (the rest): **Harness traps**: key-monitor keys go through `[NSApp sendEvent:]` with the window's number (`MCGuiSendKeyToApp`); menu keys as CGEvent-made events through the window then the main menu (`MCGuiPerformMenuKey`, built from the key code; setting the unicode string made the menu refuse a matching key); tree keys through `[window ...
- Verification reality (important) (the rest): Older traps: an arrow key event needs the function-key character (U+F701...) as `characters`, and an empty string makes `-[NSMenu performKeyEquivalent:]` throw;
- Verification reality (important) (the rest): **Docker trap**: `open -g -a Docker` raised an administrator password prompt and the daemon never started; never start Docker unattended.
