# CI: what runs, how a failure reads, and keeping main green

CI is `.github/workflows/ci.yml`: six jobs (macOS app, GTK4 port, the core
tests under ASan and UBSan, under ThreadSanitizer, the Android debug APK with
the Kotlin checks, and the Android release APK) and a seventh, **CI passed**,
which is green only when all six are. CLAUDE.md ("Build, test, run,
release" and "Verification reality") has what each job checks.

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
  `gui-checks-1`, `gui-checks-2` in ci-logs, which the summary reads), and
  fails if any of them failed. The GTK job runs no plain core tests: the
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

What the two harness processes could share, and why they do not (CLAUDE.md,
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
Never retry it. Found so far: markdown_side_sync failed on two of the
PR's four runs with the source steady at line 9,356 rather than about
10,000. The control suite had left the project's window with the browser
pane and its agent page showing (`browse`), which narrowed the source until
its lines wrapped. Control now closes what it opened, markdown_side_sync
works in a window of its own, and the harness says when a suite leaves a
browser pane changed (a `LEFT` line for now, see CLAUDE.md).

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
- `LEAK [suite]` (a failure) and `LEFT [suite]` (a warning): state a GUI
  suite left for the next. LEAK is what a later suite would trip over (a
  window, a sheet or question, a zoom, a key window pinned, alert answers
  unused); LEFT is a settings or keys file, which the harness puts back
  between suites anyway, or a terminal left open. Either names the suite
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
  real bugs in the checks; CLAUDE.md lists them under "CI's macOS runner
  differs".

There are no blanket retries, and none should be added: a check that fails
once in ten runs is telling the truth about a race in the product or in the
check. A retry for one named check, if ever needed, must print `FLAKY` with
the check's name in the job summary and be listed under "Known flaky
checks" below with its cause.

The runner images are named (`macos-26`, `ubuntu-26.04`), not `latest`, so a
new image arrives as a change to ci.yml on a branch, where its failures are
seen before main's.

## Known flaky checks

None retried. Checks seen failing on CI and passing on a rerun, with no
fix yet, are listed here with the run that showed them, and removed when
fixed.

- (none open as of 2026-11-14)

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

A run took about 21 minutes until 2026-11-17 (see "What makes a run
quick" for now), and a merge waits that long after the last push to its
branch.

## Measuring a flake rate

`gh workflow run CI --ref <branch>` starts a run of a branch by hand;
`gh run rerun <id> --job <job id>` reruns one job. Five runs of the GUI jobs
on one commit, all green, is the bar for calling a fix to a flaky check
done.
