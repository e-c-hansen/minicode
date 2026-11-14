#!/bin/bash
# ci-local.sh — run on this Mac what CI runs (.github/workflows/ci.yml), so a
# red run is seen before the push rather than in GitHub's email.
#
#   scripts/ci-local.sh            everything this Mac can run (about 25 minutes)
#   scripts/ci-local.sh --quick    the core tests, the build and its checks
#                                  (a few minutes): worth it before every push
#   --gui / --no-gui               the Mac GUI checks or not (see below)
#   --no-sanitizers                skip ASan/UBSan and TSan (most of the time)
#   --no-release                   skip the release zip
#
# THE GUI CHECKS PUT NO WINDOW ON SCREEN, BUT THEY TAKE THE KEYBOARD while
# they run (about 10 minutes): the app under test is a real app that the
# window server hands key events to. Run them when nobody is typing. With no
# terminal attached (an agent's shell) they are skipped unless --gui is
# given.
#
# Steps, in CI's order: the core tests with clang (the macOS job) and with
# g++ (libstdc++, as the Linux jobs build it, if g++-16 or g++-15 is
# installed); ASan/UBSan and TSan (the sanitizer jobs, with clang); the app;
# its deployment target; the LaTeX sweep; the GUI checks; the release zip and
# its checks; the GTK port's GTK-free tests (linux/Makefile). Not here: the
# GTK GUI checks and the Android builds, which need Docker or a JDK and run
# on CI only (never start Docker unattended on this Mac; CLAUDE.md).
#
# Each step's output is in build/ci-local/<step>.log; the end prints one line
# for each failed check (scripts/ci-summary.sh, as CI's job summaries do)
# and a table of the steps. Exit status 1 when any step failed.
set -u
cd "$(dirname "$0")/.." || exit 2

quick=0 gui=auto sanitizers=1 release=1
for a in "$@"; do
    case "$a" in
    --quick) quick=1 ;;
    --gui) gui=1 ;;
    --no-gui) gui=0 ;;
    --no-sanitizers) sanitizers=0 ;;
    --no-release) release=0 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "ci-local.sh: unknown option $a (see --help)" >&2; exit 2 ;;
    esac
done
if [ "$quick" = 1 ]; then
    sanitizers=0 release=0
    [ "$gui" = auto ] && gui=0
fi
if [ "$gui" = auto ]; then
    if [ -t 0 ] && [ -t 1 ]; then gui=1; else
        gui=0
        echo "No terminal attached: the GUI checks are skipped (--gui runs them; they take the keyboard)."
    fi
fi

logs=build/ci-local
rm -rf "$logs"
mkdir -p "$logs"
results=()
failed=0

# step NAME COMMAND...: runs it with its output in $logs/NAME.log and on screen.
step() {
    local name="$1"; shift
    local t0=$SECONDS
    printf '\n==> %s: %s\n' "$name" "$*"
    "$@" 2>&1 | tee "$logs/$name.log"
    local status=${PIPESTATUS[0]}
    local took=$((SECONDS - t0))
    if [ "$status" -eq 0 ]; then
        results+=("$(printf 'ok      %-22s %4ds' "$name" "$took")")
    else
        results+=("$(printf 'FAILED  %-22s %4ds  (exit %d, %s)' "$name" "$took" "$status" "$logs/$name.log")")
        failed=1
    fi
    return "$status"
}
skip() { results+=("$(printf 'skipped %-22s       %s' "$1" "$2")"); }

# --- the core tests: clang as the macOS job, then g++ as the Linux jobs.
step core-tests make test
gxx=""
for c in g++-16 g++-15; do command -v "$c" >/dev/null && { gxx="$c"; break; }; done
if [ "$quick" = 1 ]; then skip core-tests-gcc "--quick"
elif [ -n "$gxx" ]; then
    # Objects of their own, so neither compiler's build is thrown away by the
    # other's; the test binary's path is shared, so it is removed after.
    step core-tests-gcc make test CXX="$gxx" TESTDIR=build/test-gcc OBJDIR=build/obj-gcc
    rm -f build/run_tests
else skip core-tests-gcc "no g++-16 or g++-15 (brew install gcc)"
fi

if [ "$sanitizers" = 1 ]; then
    ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:strict_string_checks=1 \
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
        step core-tests-asan make test-asan
    MC_PRINT_GROUPS=1 step core-tests-tsan make test-tsan
else
    skip core-tests-asan "--no-sanitizers or --quick"
    skip core-tests-tsan "--no-sanitizers or --quick"
fi

# --- the macOS job: the app and what is checked of it.
if step build make app; then
    step check-minos scripts/check-minos.sh
    step latex-sweep make latex-sweep
    if [ "$gui" = 1 ]; then
        echo
        echo "The GUI checks take the keyboard for about 10 minutes (no window shows). Ctrl+C now to stop."
        sleep 5
        # The same room CI's job gives (keep in step with ci.yml).
        MC_GUI_WAIT_SCALE=1.5 MC_TIMING_SCALE=1.5 \
            step gui-checks make guitest
    else skip gui-checks "--no-gui, --quick, or no terminal"
    fi
    if [ "$release" = 1 ]; then
        release_checks() {
            make dist-zip RELEASE=1 || return 1
            if nm MiniCode.app/Contents/MacOS/MiniCode | grep -q 'MCDemoStart\|MCBenchStart'; then
                echo "error: the release build still has the demo scenes or the benchmark in it"
                return 1
            fi
            unzip -l MiniCode.zip | grep -q ' MiniCode.app/Contents/MacOS/MiniCode$' || return 1
            unzip -l MiniCode.zip | grep -q ' MiniCode.app/Contents/MacOS/minicode-cli$' || return 1
            scripts/check-minos.sh MiniCode.app
        }
        step release-zip release_checks
        # Put the development build back, so `make run` is what it was.
        make app >/dev/null 2>&1
    else skip release-zip "--no-release or --quick"
    fi
else
    for s in check-minos latex-sweep gui-checks release-zip; do skip "$s" "the build failed"; done
fi

# --- the GTK job's GTK-free tests (the Makefile fallback), with this Mac's g++.
if [ "$quick" = 0 ] && [ -n "$gxx" ]; then
    step linux-makefile-tests make -C linux test CXX="$gxx"
else skip linux-makefile-tests "--quick, or no g++"
fi
skip gtk-gui-checks "CI only (Docker on this Mac is never started unattended)"
skip android "CI only (no JDK 17 to 21 here)"

echo
scripts/ci-summary.sh "ci-local" "$logs"
echo "Steps:"
printf '  %s\n' "${results[@]}"
echo "Logs: $logs/"
exit "$failed"
