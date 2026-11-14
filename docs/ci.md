# CI: what runs, how a failure reads, and keeping main green

CI is `.github/workflows/ci.yml`: six jobs (macOS app, GTK4 port, the core
tests under ASan and UBSan, under ThreadSanitizer, the Android debug APK with
the Kotlin checks, and the Android release APK) and a seventh, **CI passed**,
which is green only when all six are. CLAUDE.md ("Build, test, run,
release" and "Verification reality") has what each job checks.

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
(the `mac-gui-checks` and `gui-checks` artifacts).

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

- `search` (Mac), SearchSuite.mm "20,000 matches in 2 files": once "in 3
  files" (PR #8, run 43610029465), never before in the history read. Cause
  unknown; the check now names the files when it fails.

## Branch protection (for the owner to decide)

Today pushes land on main directly, and "no merge to main while a job on
main is red" is kept by hand. GitHub can keep it instead. The settings
below are a recommendation; nothing here has been changed in the
repository's settings.

**Recommended: a ruleset on `main`** (Settings > Rules > Rulesets > New
branch ruleset, target the default branch):

1. *Require status checks to pass*, with the one check **CI passed**
   (the `ci-passed` job). One check, so adding or renaming a job never
   needs the settings touched. Turn on *Require branches to be up to date
   before merging*: a pull request's run tests it merged with main as main
   was when the run started, so two that pass apart can still fail
   together once both land; up to date makes the run that passed test what
   lands.
2. *Require a pull request before merging*, with 0 approvals required (a
   solo project; the point is that CI runs on the branch first), and
   *Require conversation resolution* off.
3. *Block force pushes* and *Restrict deletions*.
4. Bypass list: empty. Every agent pushes with the owner's credentials, so
   a bypass for the owner is a bypass for every agent.

**What it changes:**

- Agents push a branch and open a pull request; whoever merges (the owner
  or the Mac session) merges only when **CI passed** is green. The
  coordinating sessions already work this way (PRs #4 to #7).
- `scripts/release.sh` pushes its version commit straight to main, which
  the ruleset would refuse. It would push the commit to a `release-X.Y.Z`
  branch, open a pull request, wait for CI (`gh pr checks --watch`), merge
  it (`gh pr merge --rebase`), then tag the merged commit and go on. That
  is a change to release.sh to make together with the ruleset, not before.
  The Linux-only release (`--linux`) on the ThinkPad needs the same.
- A run takes about 25 minutes (the GTK job), so a merge waits that long
  after the last push to its branch.

**A lighter first step**, if the release change should wait: the same
ruleset with only rule 1 (required status checks) and rule 3. Direct
pushes then still work, but only of commits that already passed CI (a
branch's run, or `gh workflow run CI --ref <branch>`), and release.sh
would push its commit to a branch, run CI there, then push the same commit
to main.

To apply the recommended ruleset from the command line (the owner, not an
agent):

```sh
gh api repos/e-c-hansen/minicode/rulesets --method POST --input - <<'EOF'
{
  "name": "main stays green",
  "target": "branch",
  "enforcement": "active",
  "conditions": {"ref_name": {"include": ["~DEFAULT_BRANCH"], "exclude": []}},
  "rules": [
    {"type": "required_status_checks",
     "parameters": {"strict_required_status_checks_policy": true,
                    "required_status_checks": [{"context": "CI passed"}]}},
    {"type": "pull_request",
     "parameters": {"required_approving_review_count": 0,
                    "dismiss_stale_reviews_on_push": false,
                    "require_code_owner_review": false,
                    "require_last_push_approval": false,
                    "required_review_thread_resolution": false}},
    {"type": "non_fast_forward"},
    {"type": "deletion"}
  ]
}
EOF
```

## Measuring a flake rate

`gh workflow run CI --ref <branch>` starts a run of a branch by hand;
`gh run rerun <id> --job <job id>` reruns one job. Five runs of the GUI jobs
on one commit, all green, is the bar for calling a fix to a flaky check
done.
