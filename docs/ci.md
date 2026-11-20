# CI: what runs, how a failure reads, and keeping main green

CI is `.github/workflows/ci.yml`: six jobs (macOS app, GTK4 port, the core
tests under ASan and UBSan, under ThreadSanitizer, the Android debug APK with
the Kotlin checks, and the Android release APK) and a seventh, **CI passed**,
which is green only when all six are. **Android is frozen** (owner,
2026-11-17): its two jobs run only when CI is started by hand (Actions >
CI > Run workflow), CI passed waits for the other four, and releases go
out without an APK (release.sh sets `MUTIBLE_NO_APK=1` unless it is
set to the empty string). The comment above the jobs in
ci.yml says how to unfreeze. CLAUDE.md ("Build, test, run,
release" and "Verification reality", with the rest in
`docs/notes/build.md` and `docs/notes/verification.md`) has what each job
checks.

## Which runs go on (2026-11-19)

ci.yml's `concurrency` decides. A newer push to a pull request's branch
cancels that branch's run still going, which nobody will read. **Every
push to main gets a run of its own** (its group is `ci-push-<sha>`), so
each commit on main has a verdict, and when main goes red after several
merges the first red commit names the merge that did it. Until
2026-11-19 main was one group, which keeps one run going and one
waiting: a third push cancelled the waiting run before any of its jobs
started, and 13 of the 52 commits merged that day got no verdict (round
10's engineering grade, N1). A run by hand (`workflow_dispatch`) is a
group of its own as well, so several can run on one commit. Runs on main
now queue for the runners beside the pull requests' runs; a burst of
merges costs a run each.

## What makes a run quick (2026-11-17)

Before, a run took about 21 minutes, set by the macOS job (core tests 4.4
minutes, build 1.9, GUI checks 10.3) and the GTK job (core tests 3.5, build
4.8, GUI checks 7.6). Three changes, none costing anything:

- **ccache, kept between runs with actions/cache**, in the macOS, GTK4,
  sanitizers and tsan jobs (`scripts/ci-ccache.sh`). Links named clang++,
  g++ or clang++-22 go first on PATH, so make runs the same command lines
  and `build/obj/flags.txt` is the same; Meson puts ccache in front of g++
  by itself. Each run starts from the newest cache for its job and
  compiler (its own branch's, else main's) and saves one trimmed to what
  it used. A stale object cannot pass: ccache keys every compile on the
  compiler's `--version` output (so a new Xcode behind the same
  /usr/bin/clang++ misses), the whole command line, and the contents of
  every file the compile read, SDK and system headers included, and no
  build folder is cached, so make's dates and Meson's rules decide what to
  build as on a fresh checkout. The cache key (OS, job, compiler, a hash of
  the build files) only chooses where to start. The Android jobs build
  through Gradle and are not on the critical path, so they have none.
- **The core tests beside the GUI checks** in the macOS job: one make
  builds the app, the tests and the GUI checks, then
  `scripts/ci-together.sh` runs `build/run_tests` and the two halves of
  the GUI checks at once, each into its own log (`core-tests`,
  `gui-checks-1`, `gui-checks-2` in ci-logs, which the summary reads), then
  the suites with tight CPU-time bounds (`GUI_ALONE`: keystroke, large_file,
  foldperf, terminal_draw, lsp_progress, processes) alone (`gui-checks-alone`,
  since beside other processes a thread's CPU time rises too: keystroke
  failed at 1.23 ms against 1 on 2026-11-17), and fails if any failed. The GTK job runs no plain core tests: the
  sanitizers job builds the same tests with the same g++ and libstdc++
  and runs every group, timing bounds included, and the macOS job runs
  them plainly with clang.
- **The GUI checks as two processes on one runner**, in both GUI jobs.
  The step's `GUI_SECOND_HALF` names the second half's suites and the
  first half runs every other suite (`MC_GUI_SKIP_SUITES`), so a new suite
  runs in the first unlisted; a name in the list that no suite has fails
  the run, so a rename cannot drop a suite from both. The halves were
  split by each suite's time on CI (from a run's "Suite x" timestamps):
  about 290 s each on the Mac, 240 s on GTK. On GTK they are two
  `meson test --no-rebuild --logbase gui-first-half|gui-second-half` runs.
  Each half's failure pictures go in a folder of their own in the
  artifact, and the summary's NEAR list is the closest ten across both.

What the two harness processes could share, and why they do not (`docs/notes/verification.md`,
"Two harnesses at once"): each makes its own scratch world (mkdtemp), with
its own home, TMPDIR (and so its control sockets), defaults suite and
pasteboard named by its pid, and on GTK its own Xvfb (the server picks the
display) and session bus. The Python interpreter choice was the one piece
of app state still in the standard defaults, shared by every process
called guitest; it now goes through the scratch suite like the rest.
Servers in the checks bind port 0, or take a free port and hand it to
`serve.py`; macOS hands out TCP ephemeral ports in turn, so a port one
process released is not given to the other before the range wraps. Launch
Services is never asked anything (openURL: is swizzled), and the recent
folders the app notes go to a system list no check reads.

A check that fails only when the halves run together is a sharing bug,
or a check that reads a place before it has settled (on a loaded runner it
settles later): fix the sharing or the wait, or, only when the product's
own timing is the cause, move the suite so it runs alone, and say so here.
The retry (below) may let such a run pass with a FLAKY line; the fix is
still owed. Found so far: markdown_side_sync failed on two of the
PR's four runs with the source steady at line 9,356 rather than about
10,000. The control suite had left the project's window with the browser
pane and its agent page showing (`browse`), which narrowed the source until
its lines wrapped. Control now closes what it opened, markdown_side_sync
works in a window of its own, and the harness says when a suite leaves a
browser pane changed (a `LEFT` line for now, see `docs/notes/verification.md`).

## Before pushing

`scripts/ci-local.sh` runs on the Mac what CI runs, without Docker:

- `scripts/ci-local.sh --quick` (a few minutes): the core tests, the app, its
  deployment target, and the LaTeX sweep. Worth it before every push.
- `scripts/ci-local.sh` (about 25 minutes): also the core tests with g++, ASan
  and UBSan, TSan, the Mac GUI checks, the release zip, and the GTK port's
  GTK-free tests. The GUI checks put no window on screen but take the
  keyboard while they run, about ten minutes; with no terminal attached
  they are skipped unless `--gui` is given.

The GTK GUI checks and the Android builds run on CI only.

## Reading a failure

Each job's last step, `scripts/ci-summary.sh`, writes one line for each failed
check at the top of the run's page (the job summary) and as an annotation on
the pull request: `FAIL [suite] file:line what (in: step)`, a crash
(`FATAL`), a sanitizer report, or the compiler's error when the build broke.
It also says which suites failed and how often ("Failed in: git_panel (19),
git_actions (18)"), so a cascade from one suite reads at once. A failed GUI
run keeps a picture of the test window at each of its first ten failures
in each half (the `mac-gui-checks` and `gui-checks` artifacts, a folder per
half).

Three more kinds of line warn before anything fails:

- `SLOW [suite]`: a CPU-time bound passed only within the room CI's runner is
  given (below). A bound that shows here repeatedly is being outgrown.
- `NEAR [suite]`, under "Timing checks closest to their bounds": the ten
  timing checks with the least margin in that run.
- `FLAKY [suite] <first failure>`: a GUI suite failed, then passed when
  run again alone in a fresh process (see "Retries" below). The run
  passed; the failure is still a bug to fix.
- `LEAK [suite]` (a failure) and `LEFT [suite]` (a warning): state a GUI
  suite left for the next. LEAK is what a later suite would trip over (a
  window, a sheet or question, a zoom, a key window pinned, alert answers
  unused); LEFT is a settings or keys file, or a terminal left open,
  which the harness puts back (closes) between suites anyway. Either names the suite
  that left it, not the one that would fail because of it.

## Why a check passes here and fails on CI

From November 6 to 14, 2026, 60 of 180 CI runs failed (55 of them on main),
and 12 more went green only on a rerun. About half were real bugs caught,
in the product or in a check; most of the rest were the causes below, and
what now guards against each:

- **CPU-time bounds on a slower, uneven machine** (most of the macOS job's
  reruns: keystroke, bracket_pairs, terminal_draw, markdown_side_sync,
  lsp_progress, lsp_sync, large_file, inlay_hints). CI's runner is a shared
  virtual Mac; a full terminal draw took 0.7 ms of CPU on one run and 9 ms
  on another. Every miss that passed on a rerun was under 1.45 times its
  bound; the real regressions caught were 1.8 times or more. So CI runs the
  GUI checks with `MC_TIMING_SCALE=1.5` (a time between the bound and 1.5
  times it passes with a SLOW line), and a check whose work varies with the
  machine compares with a baseline taken on the same thread in the same run
  (terminal_draw's frames against its full draws; large_file and keystroke
  already did).
- **State left by an earlier suite** (daily_work's pinned key window failed
  five git suites; on GTK a terminal's width after zoom, the editor's font,
  a row menu left open). The harnesses now compare the world before and
  after each suite (LEAK lines).
- **A toolkit's judgment by the clock racing a slow main thread** (GTK
  counting a double-click with a timer while the first click's work ran
  long: the bookmarks and editor_words failures). Fixed in the product where
  found; a suite should wait on the condition it checks, never on time.
- **Waits too short for the runner**: CI gives every wait half again its
  timeout (`MC_GUI_WAIT_SCALE=1.5`). A wait polls, so more room only delays
  a failure; it never hides one.
- **The runner's environment** (1x screens, scroll bars that take room, no
  reverse DNS for 127.0.0.1, a prompt wider than the terminal). These are
  real bugs in the checks; `docs/notes/verification.md` lists them under "CI's macOS runner
  differs".

## Retries (since 2026-11-17)

Until 2026-11-17 nothing was retried. The owner then approved exactly one
kind of retry, with a soak (below) to keep it honest:

- **One retry of a failed GUI suite, in either harness.** With
  `MC_GUI_RETRIES=3` (set in ci.yml's three GUI steps) the process that
  starts a harness run (the Mac's first stage in `mac/tests/gui/Harness.mm`,
  the GTK port's wrapper in `linux/tests/gui/Wrapper.cpp`) reads what
  failed (`MC_GUI_RESULTS`, written as the run ends) and runs each failed
  suite once more, alone, as a fresh run of the whole program: a new
  scratch world, home, defaults and pasteboard, and on GTK a new Xvfb and
  session bus, as the `_x` child suites run. Its output is in the log
  marked `    retry| `. A suite that passes then prints
  `FLAKY [suite] <its first failure>`, which `scripts/ci-summary.sh` puts at
  the top of the run's page (and as a warning on the pull request) in
  place of that suite's FAIL and LEAK lines, and the run goes on. One that
  fails again prints `RETRY [suite] failed again`, its FAIL lines stand,
  and the run fails as it would have. `tests/GuiRetry.h` is the shared
  code, tested by the core's `gui-retry:*` groups.
- **Nothing else is retried.** The core tests (and the sanitizer jobs)
  never are. Neither is a run that crashed (`FATAL`), hit its own time
  limit, or failed a check outside any suite (a suite name no suite has,
  the end's checks): the harness says "No suite retried" and why.
- **Timing checks too.** A CPU-time bound already has CI's 1.5 times room
  (`MC_TIMING_SCALE`, a SLOW line); a time past even that fails its suite,
  and that suite goes through the retry like any other. A timing check
  that shows up FLAKY is a bound the runner outgrows or a real regression
  that varies, and is treated like any other FLAKY line.
- **At most three a run.** A harness process with more than three failed
  suites retries none, and a job whose processes together needed more
  than three retries fails (`ci-summary.sh` adds a `FAIL [retries]` line):
  that many flakes means something is wrong with the runner or the
  product, not one check.
- **Every FLAKY line must lead to a fix.** A check that fails once in ten
  runs is telling the truth about a race in the product or in the check;
  the retry only keeps one such race from stopping every merge while it
  is found. Note the FLAKY line's suite and run in "Known flaky checks"
  below (or in the soak's issue) and fix it like a failure: the cause in
  the product or the check, never a longer wait. Five green runs of the
  GUI jobs on one commit with no FLAKY line for it is the bar for calling
  it fixed.

## The soak (`.github/workflows/soak.yml`, since 2026-11-17)

Every night (03:17 UTC), and by hand from the Actions tab (`gh workflow
run Soak -f rounds=2 [-f suites="tree git_panel"]`), the GUI suites of
both apps run several rounds (3 by default, 10 at most), never on a
pull request and never gating a merge. `scripts/soak.sh` runs each round
as CI does (the Mac's two halves at once then `GUI_ALONE`, GTK's two
halves, the lists read from ci.yml; with `suites`, those names in one
process), with no retries, each round in its own order: a seed drawn per
round and printed (`MC_GUI_SHUFFLE=<seed>`, which both harnesses take:
the suites they selected in an order drawn from the seed, the same on
every machine). `MC_GUI_SHUFFLE=<seed>` with the same suites plays a
round's order again. The GTK rounds alternate `MALLOC_PERTURB_` between
165 and 90 (glibc fills new and freed blocks with them; Meson's own
random value once exposed a read of memory never written).

The report job (the workflow's only write permission, `issues: write`)
makes one table, each suite that failed in any round with in how many of
its app's rounds, those rounds' seeds, and its first failure, puts it on
the run's page, and replaces the body of the open issue "Soak: flaky
checks" with it (opening one when none is open). The logs and failure
pictures are the run's `soak-mac` and `soak-linux` artifacts. A suite in
that table is owed a fix, as a FLAKY line is.

The runner images are named (`macos-26`, `ubuntu-26.04`), not `latest`, so a
new image arrives as a change to ci.yml on a branch, where its failures are
seen before main's.

## Known flaky checks

Checks seen failing on CI and passing on a rerun or a retry (a FLAKY
line), with no fix yet, are listed here with the run that showed them,
and removed when fixed. The soak's issue lists what the nightly rounds
find.

The first soak (2026-11-17, two rounds each, run 44116695149, issue #127)
found mostly suites that pass only in their usual place in the order:
on GTK `save`, `tabs` and `keys` failed in both shuffled rounds (waits on
the editor or its keyboard that time out), and `processes`, `mermaid`,
`latex_forward`, `agent_panel`, `attention`, `palette`, `harness` and
`tree_stall` in one; on the Mac `control` in both (`controlTerminals`
not empty where it expects none). Each is a suite that depends on what
an earlier one did or left, to be made to stand alone. The GTK ones were
(2026-11-18): rows off screen in a tree a full run leaves with a hundred
entries, and a watch/notes.txt taken for save's notes.txt
(`MCGuiBringTreeRow` and `MCGuiTreeRow` reveal a row and know it by
path; tree_stall scrolls to the top); the browser pane left shown
(`MCGuiHideBrowserPane`: the harness suite, and latex_forward, whose
page 2 could not reach the top of a narrower preview); keys reading
accelerators with the keyboard left in a page; palette's "big" matching
find_bar's find/big.py first; terminal_ids leaving the close question
off, so processes closed its busy terminal unasked (now a LEAK); and
browser_find's about:blank, which the GTK address bar took for a web
search (the app fixed). `MC_GUI_IN_ORDER=1` replays a round's order,
from its log, up to the suite that failed. agent_panel's and
attention's shell waits did not fail again in that round's replay.
A second pass (2026-11-18, seed 2076540248's order and 275908225's)
found: agents marking the terminal waiting and never clearing it, so
every later title began "(1 waiting)" and tabs read it (a mark left is
now a LEAK, and cleared); install_hints and agents typing a command at
the shell's prompt without Return and leaving it there, so find_bar's
"stty raw ..." joined "uv tool install ..." and never ran
(`MCGuiClearTerminalLine`; the harness now also sends every idle shell
Ctrl+U as a suite ends, and a program left running in a terminal is a
LEAK); the X focus left on a window a git suite closed, so tabs' click
reached an inactive window (the git suites give it back, and so does the
harness, with a LEFT line); control expecting the keyboard out of the
editor, where an open lands in the background; zoom's and browser_find's
browser pane as an earlier suite left it (shown, or with tabs before
theirs); layout's and palette's tree rows off screen; and, in the app,
the Evidence page's folder line, which did not shorten: with a long
project folder it made every page in the editor's slot wider than the
window, so Compare with Main's Apply to Main… and the format offer's ×
were out of reach (agents failed even alone in a checkout under
`.claude/worktrees`, whose path also gave a session in the project's
own worktree the outer one: AgentIndex now takes the innermost).

A third pass (2026-11-19, the soak of d1855c3, run 44518243311, seeds
3148957826, 364479112 and 1595826833 on the Mac, 791492888, 4059859019
and 3863698393 on GTK) found three suites that failed in two or three
shuffled rounds, each fixed in its suite and to be called fixed after
five soak rounds without it:

- Mac `replace_in_folder`, "the search Return ran", in all three rounds:
  the search suite, which ran before it in each of them (never in CI's
  order: the file is linked after), closes the Search window both
  suites use. Offscreen a
  closed window is never ordered in again, and `[NSWindow sendEvent:]`
  drops every key sent to it, so the Return never reached the replace
  field (a stale expectation it was not: Return there searches since
  #210, and the check waits for that search). The suite's `Press` now
  hands the key to the window's first responder, as the window does
  with a key without modifiers.
- GTK `keys`, "the terminal has it", in two: Ctrl+` toggles the
  terminal, and a terminal an earlier suite left shown was put away
  instead of given the keyboard. The suite hides it first.
- GTK `browser_tabs`, "the row wheeled back to its start", in two: the
  wheel was aimed at the first tab's title, which is off screen
  whenever the row is scrolled (an earlier suite left the pane
  narrower); it is aimed at the row now.
- GTK `preview_tabs`, in all three rounds (rounds 2 and 3 replayed
  with `MC_GUI_IN_ORDER=1` and the round's order failed as the soak
  did): CI's
  order runs it sixth, with the project nearly empty, and it found its
  tree rows by name among those on screen. After 28 and 63 other
  suites the folders they had opened in the tree held p-a.txt far
  below the last row ("the tree lists them", then a crash on the null
  tab its next step read); after 74, replace_in_folder had left Find in
  Folder on rif with files to include "three.txt", so "needle" found
  nothing ("both results"). The suite now brings each row into view and
  finds it by path, puts Find in Folder back and scopes it to its own
  folder; replace_in_folder, go_back and latin1_files put it back as
  they end, and the harness says `LEFT` for a Find in Folder left
  changed and puts it back. The other suites that found tree rows by
  name (adjust_settings, bookmarks, bookmark_workspace, go_back,
  maximize, tree_git_colors, the demo scenes) find them by path too,
  and adjust_settings right-clicks the active tab's title, which the
  strip keeps in view, not the first, which the tabs earlier suites
  left on the folder put off the strip's left end in its new window
  (go_back and maximize before it, in a local run).

- GTK `preview_tabs` again in rounds 1 and 3's orders, with the fixes
  above (2026-11-19, a second change): the second single click on a
  tree row (p-b) reached no handler of the tree's, nor did any click
  after it. agents, before it in both orders, ran `git init` in the
  shared project, made a worktree and a branch under .claude, and left
  them, so every row was "Untracked (git)", with a tooltip, and the
  tooltip that came up under the pointer after the click on p-a covered
  p-b; XTest's pointer jumped onto it. `agents preview_tabs` alone
  failed the same way before the change; with the harness hiding the
  tooltip and the repository left (`MC_GUI_LEAKS=note`) preview_tabs
  passed, its log noting `a tooltip ("Untracked (git)") over the point
  to click, hidden first`. agents now removes the repository, the
  worktree and its files and waits until the tree shows neither; the
  harness says `LEFT` for a project left a repository and removes it
  (`MCGuiRemoveProjectRepository`), and hides a tooltip of the app's
  over the point of any click (docs/notes/verification.md, "A tooltip
  takes the next click").
- GTK `tree_stall`, "a-tree-stall is on show", in a local full run with
  seed 791492888 (all suites in one process): its folder never came on
  show. Replayed up to it (`bookmarks format_offer host_splits
  latex_problems second_server git_review detach json_outline
  tree_stall`) it failed twice in three runs on a loaded machine (a
  build, or six busy loops, on the same four cores) and never unloaded;
  without any one of the eight suites before it, or after json_outline
  alone, it passed. A diagnostic build showed the folder in the tree's
  model, first at the top level, and never on show: the eight suites
  leave the project with more rows than the tree shows, and the row
  that came in at the top stayed above the pane, the rows on show kept
  where they were (GtkListView's anchor, `FileTree::keepRows`), after
  the suite's one scroll to the top. The suite now scrolls to the top on
  every look of its wait, noting when the tree had left it again. Under
  six busy loops on the same cores a later row went the same way
  (data.bin, the churning folder's rows coming and going below a-files'
  left the tree scrolled past them), so every other row it clicks is
  brought into view by its path first (`MCGuiBringTreeRow`).

A fourth pass (2026-11-19): round 2's order (seed 4059859019, the first
half's 74 suites) replayed with `MC_GUI_IN_ORDER=1` on a ThinkPad
loaded by three other agents failed five suites; in CI the same order
failed only `tab_strip`. Each was run alone, quiet and on one or two
cores beside busy loops, then in the order with a probe of the main
thread's CPU over three idle seconds before each suite:

- GTK `tab_strip`, "the row scrolls to its end while the tab is held
  there" (CI and the replay; state left): browser_tabs ended with the
  browser pane on show, so the row was 472 wide, not 851; the press on
  config.json's tab put its JSON outline beside the text, the right side
  was laid out again, and the row went to 713 under the held drag, the
  pointer at its old end no longer near the end (12 pixels scrolled; the
  failure pictures show the outline and a narrowed browser pane). Two
  local replays with the pane left on show kept the row 421 wide and
  passed: the outline's width there left the editor wide enough.
  browser_tabs hides the pane as it ends, tab_strip hides it first, the
  harness says `LEFT` for a browser pane left on show and hides it, and
  tab_strip checks the row kept its width under the drag.
- GTK `auto_save`, "the tab on show is written" (load): the wait for
  outside.txt's edit passed on its first key, and "ne " of "mine " came
  after other.txt was shown ("one other.txt"); the status line's path
  was that failure's, its note gone by then. It waits for the whole
  edit.
- GTK `host_wrap`, "VTE at the view's own grid again" (load): the view's
  grid was read as VTE's 80 x 24 before the new window's first layout
  (104 x 10 in CI). It waits for the grid VTE's frame holds.
- GTK `gutter_typing`, "the main thread idle" (load): a wait for the
  main thread's CPU to stand still for half a second, which a caret
  fading at 60 frames a second for 10 s after the last key never lets
  happen once frames cost enough. It waits for the last line colored,
  what the idle time was for.
- GTK `large_file`, "its place as it was" (load): GTK's layout of the
  25 MB file took 13 to 44 s of the main thread's CPU and 14 to 470 s
  of the clock (one core beside eight busy loops); the replay took 155 s
  for the first layout and outran the wait's 180 s (270 with the scale)
  on the second. Both waits are bounded by the main thread's CPU now
  (`MCGuiWaitCpu`: 90 s of it, the clock's 900 s only for a wait
  nothing moves). Still open: the editor's settling (`settleOnPlace`)
  has limits of the clock of its own, three minutes and a second
  without the height growing; on one core beside eight busy loops it
  ended early (the top line 17,296 of 299,985 after 900 s, 70 s of
  CPU), as it would have before.
- word_wrap left its 300,001-line file open, and each settings file a
  later suite put back cost over a second of the main thread (as
  tab_strip began); it closes its tabs now. browser_pages, browser_find
  and bookmark_save left the browser pane on show too (the new `LEFT`
  lines); they hide it as they end. browser_tabs, which counts the
  pane's tabs from none, failed after bookmark_save's; it closes any
  left first.
- GTK `latex_problems`, "one error, on line 3, in the document" (order;
  found checking the above, not in a soak yet): the preview sweeps a
  dead process's folders at a launch's first typeset only, and run after
  latex_forward and latex_inputs in one process its typeset was not the
  first: the crashed process's folders it had written were all still
  there. It asks for the sweep again (`LatexPreview::sweepAgainForTest`).
- `MCGuiWaitCpu` also fails once the main thread has spent under 0.3 s
  of CPU in a minute of the clock (scaled), its work over and the
  condition not come, so a large_file place lost for good fails in a
  minute, not at 900 s (22 minutes at CI's scale, half the GTK job's
  limit).

With the changes, the round's 74 suites replayed in order on four cores
beside four busy loops, twice, the five above passing both times
(large_file's layouts 18 to 27 s of CPU, 26 to 32 s of the clock):
3262 passed and 2 failed, then 3262 and 4. Both runs failed
`editor_keys`' "the caret at 0" (the selection left at 0 to 3), the
round's own still-open failure below; the second also failed
`preview_tabs`' "gone" after Ctrl+W, the keyboard in the window's
GtkPaned rather than the editor (seen once, not looked into).

GTK `tree_edits`, "the tree kept its place; b_dir/f050b.txt selected"
(2026-11-20, order; in about two runs of three after `maximize`,
`hints_panel`, `preview_tabs` or a tree suite): the app's fault. When
the idle that takes the closed Rename box off the list ran before the
window's next frame, GTK gave the keyboard, still in the box, to the
tree's first row, which scrolled the list to its top, and the renamed
row was then centered from there. The box gives the list the keyboard
as it closes (`docs/notes/tree.md`, the GTK traps).

Still open from that soak:

- Seen in one round each: Mac `organize_on_save` (an alert count off by
  one as its servers were turned off), GTK `tree_clicks`,
  `editor_keys` (again in the fourth pass's loaded replay), `agents`
  and `attention` (issue #127 has each one's first failure).

- GTK `agents`, "the same key closes the panel": in one local full run
  on the ThinkPad (2026-11-17, main at b0ce717 plus a test-only change),
  Ctrl+Shift+A did not close the Agents panel within 8 s, the keyboard
  in a GtkScrolledWindow of the editor's tab strip; the rest of the
  suite's Worktrees steps then timed out. Not seen on CI. The suite
  passed alone, after `browser_pages`, after `terminal_ids`, `idelink`
  and `browser_pages` (three times), and in a full rerun. Seen once more
  alone on 2026-11-18 (the same place, the keyboard in the tab strip),
  then not in thirteen runs. Not the tab strip: a throwaway suite gave it
  the keyboard and Ctrl+Shift+A closed the panel every time, as it did
  with no focus widget at all. The keyboard lands there because the
  panel's list is emptied (the transcripts folder removed) and GTK moves
  it on. Cause still unknown.

Found while checking this: `search` failed twice on PR #8 once the leak
check closed a window `previews` had left open. The suite had searched
the whole project instead of its own folder, and passed only because that
leftover window, on another folder, was the newest. Fixed in the suite.

## Branch protection (on since 2026-11-14)

The owner turned on classic branch protection for `main`:

- **Required status check: CI passed** (the `ci-passed` job), one check so
  adding or renaming a job never needs the settings touched. "Require
  branches to be up to date" is off.
- **No pull request or review is required**, and **administrators are not
  held to it** (`enforce_admins` off), so `scripts/release.sh` can still
  push its version commit to main; GitHub then prints "Required status
  check 'CI passed' is expected" as a bypass notice.
- Force pushes and deletions are refused.

**How the sessions work under it** (2026-11-14, after a direct push of an
unchecked commit turned main red; the owner: "why do we not know the
outcome before trying to ship it?"): every change goes to a branch and a
pull request, rebased onto the latest main before its last run, and is
merged only once **CI passed** is green on that exact commit, never with
`--admin`. The one direct push is release.sh's version commit, made only
from a commit whose CI already passed. A pull request with no run (one the
Demos workflow opened before `scripts/demos-propose.sh` started CI itself)
gets one by hand: `gh workflow run CI --ref <branch>`.

Since the rename (1.6.0) the repo is `ible-ai/mutible`; GitHub redirects
the old `e-c-hansen/minicode` URLs. Branch protection moves with the repo, <!-- rename-mutible: keep -->
but check it is still on after the move (`gh api
repos/ible-ai/mutible/branches/main/protection`).

A run took about 21 minutes until 2026-11-17 (see "What makes a run
quick" for now), and a merge waits that long after the last push to its
branch.

## Measuring a flake rate

`gh workflow run CI --ref <branch>` starts a run of a branch by hand;
`gh run rerun <id> --job <job id>` reruns one job. Five runs of the GUI jobs
on one commit, all green, is the bar for calling a fix to a flaky check
done.
