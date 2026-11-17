#!/bin/bash
# ci-ccache.sh — ccache for a CI job's compiles (.github/workflows/ci.yml).
#
#   ci-ccache.sh setup COMPILER [WRAP...]
#       Installs ccache when the runner has none, points it at a folder
#       under $RUNNER_TEMP (which actions/cache restores and saves), and
#       prints compiler=<hash of `COMPILER --version`> to $GITHUB_OUTPUT for
#       the cache's key. Each WRAP (clang++, g++, clang++-22) gets a link to
#       ccache under that name in a folder put first on PATH, so `make` runs
#       the same command line as before (build/obj/flags.txt is unchanged)
#       and ccache finds the real compiler further down PATH. Meson needs no
#       link: it puts ccache in front of the compiler itself when it finds
#       ccache on PATH at `meson setup`.
#   ci-ccache.sh finish JOBSTATUS
#       Prints the cache's numbers and, when the job succeeded, drops every
#       entry this job did not use, so the cache saved after it holds what
#       this commit compiles and no more. A failed or stopped job keeps
#       everything (a build that broke part way used only part of it).
#
# Why a cache can never make a stale build pass: ccache's key for each
# object is the compiler's own `--version` output (compiler_check below,
# so a new Xcode behind the same /usr/bin/clang++ shim, or a new g++
# package, misses), the whole command line, and the contents of the source
# and of every header the compile read, SDK and system headers included.
# No build folder is cached: every job starts with no objects, so make's
# dates and flags.txt, and Meson's own rebuild rules, decide as on a fresh
# checkout, and ccache only answers "this exact compile was done before".
# The actions/cache key (OS, job, compiler, a hash of the build files) only
# picks which earlier cache to start from.
set -euo pipefail

cmd="${1:?usage: ci-ccache.sh setup COMPILER [WRAP...] | finish JOBSTATUS}"
shift

case "$cmd" in
setup)
    compiler="${1:?usage: ci-ccache.sh setup COMPILER [WRAP...]}"
    shift
    if ! command -v ccache >/dev/null; then
        if [ "$(uname -s)" = Darwin ]; then
            HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1 HOMEBREW_NO_ENV_HINTS=1 \
                brew install --quiet ccache
        else
            sudo apt-get update -q
            sudo apt-get install -y -q --no-install-recommends ccache
        fi
    fi
    ccache="$(command -v ccache)"
    ccache --version | head -1
    if [ $# -gt 0 ]; then
        bin="$RUNNER_TEMP/ccache-bin"
        mkdir -p "$bin"
        for c in "$@"; do
            command -v "$c" >/dev/null || { echo "error: no $c on PATH to put ccache in front of" >&2; exit 1; }
            ln -sf "$ccache" "$bin/$c"
        done
        echo "$bin" >>"$GITHUB_PATH"
    fi
    {
        echo "CCACHE_DIR=$RUNNER_TEMP/ccache"
        echo "CCACHE_BASEDIR=$GITHUB_WORKSPACE"
        echo "CCACHE_COMPILERCHECK=%compiler% --version"
        # Preprocessor mode: direct mode keys on the headers a file used
        # last time, so a new header earlier on the include path (src/HtmlPage.h
        # shadowing mac/src/HtmlPage.h, 2026-11-17) reused a stale object.
        echo "CCACHE_NODIRECT=1"
        echo "CCACHE_MAXSIZE=1G"
        echo "CI_CCACHE_STARTED=$(date +%s)"
    } >>"$GITHUB_ENV"
    version="$("$compiler" --version 2>&1)"
    printf '%s\n' "$version" | head -2
    echo "compiler=$(printf '%s' "$version" | shasum -a 256 | cut -c1-16)" >>"$GITHUB_OUTPUT"
    ;;
finish)
    status="${1:-}"
    command -v ccache >/dev/null || exit 0
    ccache --show-stats || true
    if [ "$status" = success ] && [ -n "${CI_CCACHE_STARTED:-}" ]; then
        # A hit touches its entry, so anything older than this job is unused.
        age=$(($(date +%s) - CI_CCACHE_STARTED + 60))
        ccache --evict-older-than "${age}s" || true
        ccache --show-stats | grep -iE 'cache size|files' || true
    fi
    ;;
*)
    echo "usage: ci-ccache.sh setup COMPILER [WRAP...] | finish JOBSTATUS" >&2
    exit 2
    ;;
esac
