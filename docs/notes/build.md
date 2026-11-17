# Build and CI detail

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## Build, test, run, release (the detail)

(Moved from CLAUDE.md's "Build, test, run, release".)

- **Why the
  certificate** (2026-11-05): macOS keys what an app was allowed (a protected
  folder, the microphone a terminal program asks for) to its designated
  requirement, and an ad-hoc one is `cdhash H"..."`, the exact binary, so every
  build, every restart into one and every release asked again. With the
  certificate it is `identifier "com.minicode.editor" and certificate leaf =
  H"..."`, the same from build to build. It is self-signed and untrusted, which
  neither codesign nor TCC minds. `scripts/make-signing-identity.sh` makes it
  once (openssl, then `security import -T /usr/bin/codesign`, so codesign uses
  it with no prompt); the auto-mode classifier refused to run it for the user,
  who runs it themselves. Releases built on that Mac are signed with it too
  (1.5.6 was the first), so installs keep their answers across releases.
  Info.plist carries the usage descriptions (microphone, camera, Apple events,
  Desktop, Documents, Downloads, removable and network volumes): programs in
  the terminal count as minicode to macOS, and without
  `NSMicrophoneUsageDescription` Claude Code's voice input was refused without
  a prompt.
- Each source file is its own object in `build/obj` with a `-MMD -MP`
  dependency file; `build/obj/flags.txt` holds the compile command, so new
  flags rebuild everything. The Mac's GNU make 3.81 cannot tell a makefile
  whether `-j` was given, so the top-level make adds `-j$(JOBS)` itself (one
  job per core, never with `clean` among the goals); `make JOBS=1` builds
  serially.
- **compile_commands.json** (2026-11-16): `make` (and `make compdb` alone)
  writes it at the repo root for clangd, one entry per source file from
  the Makefile's own variables (`APPCMD` for `src/`, `mac/src`, the CLI
  and the sweep, plus `-Imac/tests/gui` for the GUI suites; `TESTCMD` for
  run_tests.cpp and fakelsp), rewritten only when its text changes,
  git-ignored with clangd's `.cache/`. Without it clangd guessed (no
  `-Isrc`, no `-fobjc-arc`): 21 false errors on mac/src/AgentModel.mm,
  none with it (`clangd --check=`, the command line tools' clangd, never
  /usr/bin's shim). The `*.inc` groups and `*Tests.cpp` have no entry;
  clangd takes them from run_tests.cpp. A new compile rule with new flags
  needs its line in `COMPDB_LINES`. Linux's Meson build writes its own in
  its build folder.
- `make test-asan` runs the same tests under ASan and UBSan in
  `build/test-asan` (UB stops the run); CI's `sanitizers` job runs it with
  g++ and leak detection. The `fuzz:*` groups (`tests/parser_fuzz.inc` for
  Json, the LSP framing and Reader, the IDE link, Markdown and settings;
  `tests/terminal_fuzz.inc`) are seeded and short; `MC_FUZZ_SCALE=40
  ./build/test-asan/run_tests` runs forty times the rounds. A parser change
  that breaks a fuzzer's property is a bug in the parser until shown
  otherwise; the fuzzers use only public interfaces.
- `make test-tsan` runs them under ThreadSanitizer in `build/test-tsan`
  (a report stops the run; timing bounds skipped, since instrumented code
  costs several times the CPU they allow); CI's `tsan` job runs it with
  `CXX=clang++-22` after `sysctl vm.mmap_rnd_bits=28`, never g++: the g++
  15.2 build on ubuntu-26.04 died about 90 s in with a SEGV in a
  shared library and a "nested bug" in TSan's handler, no report printed
  (2026-11-07). `MC_PRINT_GROUPS=1` names each group on stderr as it
  starts (the job sets it), and after a crash the job reruns the binary
  under gdb for a backtrace. A known false positive goes in
  `tests/tsan.supp` by name, with why; a real race is fixed. About 4.5
  minutes on an M3 with clang.
- **Speed** (2026-11-17, docs/ci.md's
  "What makes a run quick"): the macOS, GTK4, sanitizers and tsan jobs
  compile through ccache, restored and saved with actions/cache
  (`scripts/ci-ccache.sh`: links named clang++, g++ or clang++-22 in front
  of PATH, so make's command lines and `build/obj/flags.txt` are
  unchanged; Meson adds ccache itself; ccache's compiler check is the
  compiler's `--version`, and no build folder is cached, so make and
  Meson decide what to build as on a fresh checkout). The macOS job runs
  `build/run_tests` beside the GUI checks, and both GUI jobs run their
  checks as two harness processes at once (`scripts/ci-together.sh`, one
  log each: core-tests, gui-checks-1, gui-checks-2): the step's
  `GUI_SECOND_HALF` names the second half's suites, and the first half
  skips them and the macOS job's `GUI_ALONE` (the tight CPU-time bounds:
  keystroke, large_file, foldperf, terminal_draw, lsp_progress, processes),
  which run alone afterwards, log gui-checks-alone (`MC_GUI_SKIP_SUITES`,
  both harnesses), so a new suite runs
  in the first unlisted; a name no suite has fails the run. Rebalance
  the list from a run's "Suite x" timestamps when one half grows well
  past the other. The GTK job runs no plain `make test` (the sanitizers
  job's g++ build runs the same groups).

## The environment and starting programs (Mac, 2026-11-17)

Threads that start programs never read `environ`. libc's setenv grows the
array with realloc and rewrites values in place, and no lock covers a
reader walking it: on Linux a git worker copying it for a child while a
GUI suite's main thread set a variable corrupted the heap
(`realloc <- g_environ_setenv <- g_subprocess_launcher_setenv <-
runGitWithin`). The Mac had the same shape: `MCGitRun` on the panel's
queue, the formatter, language server, inline agent and tectonic tasks,
and mmdc's drawing queue copied `NSProcessInfo.environment` or walked
`environ`, while 59 `setenv`/`unsetenv` calls in the GUI suites ran on
the main thread. The app itself sets nothing after startup.

- The core's `ProcessEnv` (src/ProcessEnv.h) holds an immutable snapshot
  of the environment. `main()` takes it before any thread starts a
  program (`ProcessEnv::refresh()`); `ProcessEnv::set` changes a variable
  and takes a new one under the same lock, on the main thread only.
- On the Mac, `MCEnvironment()` (mac/src/Environment.h) is the snapshot
  as a dictionary (one object per snapshot), `MCEnv(name)` one value,
  `MCSetEnv(name, value or nil)` the only way to change one. A child's
  environment is the snapshot with changes: a dictionary copy for
  NSTask (every NSTask sets `environment`, or NSTask copies environ on
  the thread that launches it), a `ProcessEnv::Block` (an explicit envp)
  for posix_spawn and execve.
- The GUI checks change variables only through `MCSetEnv`.
- `make test` checks it: `env:snapshot`, `env:spawn-stress` (four
  threads spawn `/bin/sh` from the snapshot while the main thread sets
  and unsets 120 variables six times; a torn value or a child missing its
  variable fails, and a worker walking environ there is ASan's use after
  free), and `env:raw-access` (no `setenv`, `unsetenv`, `putenv`,
  `environ` or `NSProcessInfo.environment` in mac/src, and no raw
  setenv in mac/tests/gui; comments are not scanned).
- getenv on the main thread stays (ControlServer, the IDE link's folder,
  the session host's switches): only the main thread writes.

## Traps

None recorded under a "Trap" label; read the sections above.
