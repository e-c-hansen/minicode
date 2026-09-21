#!/bin/bash
# Record the README's demo GIFs: docs/demos/<scene>.gif.
#
#   scripts/demos.sh              every scene the app knows
#   scripts/demos.sh tour latex   just these
#
#   POSTERS=1           also write a still docs/demos/<scene>.png
#   DEMO_WIDTH=960      width of the GIFs, in pixels
#   DEMO_KEEP_FRAMES=d  copy the raw frames and the app's log into d
#
# Run through `make demos`, which builds the app and tools/makegif first.
#
# Each scene plays in a real MiniCode window (it appears on screen for about
# twenty seconds; leave the mouse and keyboard alone meanwhile). The app runs
# against scratch copies of everything it would read from your account: HOME,
# ZDOTDIR, the settings file (from tools/demo-settings.conf), and the tectonic
# cache. The folder it shows is a copy of demo/, made into a small git repo,
# under /tmp, so no path from your home directory can appear in a frame.
# Frames are written to the scratch folder and deleted at the end.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
APP="$REPO/MiniCode.app/Contents/MacOS/MiniCode"
MAKEGIF="$REPO/build/makegif"
OUT="$REPO/docs/demos"
WIDTH="${DEMO_WIDTH:-960}"

[ -x "$APP" ] || { echo "demos: build the app first (make)" >&2; exit 1; }
[ -x "$MAKEGIF" ] || { echo "demos: build tools/makegif first (make demos)" >&2; exit 1; }
# Short and outside $HOME: this path shows in the terminal scene's prompt.
WORK=/private/tmp/minicode-demo
rm -rf "$WORK"
mkdir -p "$WORK/home" "$OUT"
pid=
cleanup() {
    # Only ever the scene's own process: a MiniCode you have open is left alone.
    [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

TECTONIC="$HOME/Library/Application Support/MiniCode/bin/tectonic"
TECTONIC_CACHE="$HOME/Library/Caches/TectonicProject.Tectonic"

# A fresh copy of demo/ for each scene, since scenes type into files and save.
prepare_project() {
    rm -rf "$WORK/demo"
    cp -R "$REPO/demo" "$WORK/demo"
    (
        cd "$WORK/demo"
        export GIT_AUTHOR_NAME="MiniCode" GIT_AUTHOR_EMAIL="demo@example.com"
        export GIT_COMMITTER_NAME="MiniCode" GIT_COMMITTER_EMAIL="demo@example.com"
        export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
        n=0
        commit() {   # fixed dates, so the log reads the same every run
            n=$((n + 1))
            export GIT_AUTHOR_DATE="2026-09-0${n}T10:00:00" GIT_COMMITTER_DATE="2026-09-0${n}T10:00:00"
            git add -A "${@:2}" && git commit -q -m "$1"
        }
        git init -q -b main
        commit "Add a Python sample" hello.py
        commit "Add a C++ sample" sample.cpp
        commit "Write the demo README" README.md
        commit "Add field notes in LaTeX" paper
        commit "Add a small vector type for the LSP demo" vec
    )
    cp "$REPO/tools/demo-settings.conf" "$WORK/settings.conf"
    # For the vim scene: line numbers and color, nothing else.
    printf 'syntax on\nset number\nset laststatus=2\n' >"$WORK/home/.vimrc"
}

if [ $# -gt 0 ]; then
    SCENES=("$@")
else
    SCENES=()
    while IFS= read -r s; do SCENES+=("$s"); done < <(MINICODE_DEMO=list "$APP")
fi

status=0
for scene in "${SCENES[@]}"; do
    echo "== $scene"
    prepare_project
    frames="$WORK/frames-$scene"
    rm -rf "$frames"

    extra=()
    if [ -x "$TECTONIC" ]; then
        extra+=("MINICODE_TECTONIC=$TECTONIC")
        if [ -d "$TECTONIC_CACHE" ] && [ ! -d "$WORK/tectonic-cache" ]; then
            cp -R "$TECTONIC_CACHE" "$WORK/tectonic-cache"
        fi
        extra+=("TECTONIC_CACHE_DIR=$WORK/tectonic-cache")
    else
        extra+=("MINICODE_TECTONIC=/nonexistent")
    fi

    env -i \
        PATH="/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin" \
        HOME="$WORK/home" ZDOTDIR="$WORK/home" \
        USER="${USER:-demo}" LOGNAME="${LOGNAME:-demo}" SHELL=/bin/zsh \
        LANG="en_US.UTF-8" TMPDIR="${TMPDIR:-/tmp}" \
        GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1 \
        MINICODE_SETTINGS="$WORK/settings.conf" \
        MINICODE_DEMO="$scene" MINICODE_DEMO_FRAMES="$frames" \
        "${extra[@]}" \
        "$APP" "$WORK/demo" >"$WORK/$scene.log" 2>&1 &
    pid=$!

    # Poll and kill (macOS has no `timeout`): a scene takes well under a minute.
    for _ in $(seq 1 1200); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "$pid" 2>/dev/null; then
        echo "demos: $scene is still running after two minutes; stopping it" >&2
        kill "$pid" 2>/dev/null || true
        sleep 1
        kill -9 "$pid" 2>/dev/null || true
    fi
    rc=0
    wait "$pid" || rc=$?
    pid=

    if [ "$rc" -eq 3 ]; then
        echo "   skipped: $(grep -o 'skipping.*' "$WORK/$scene.log" | head -1)"
        continue
    fi
    if [ "$rc" -ne 0 ] || ! grep -q "^end " "$frames/manifest.txt" 2>/dev/null; then
        echo "demos: $scene failed (exit $rc)" >&2
        grep -i "minicode demo" "$WORK/$scene.log" >&2 || tail -5 "$WORK/$scene.log" >&2
        # A failed run's frames and log are what show where it stopped.
        if [ -n "${DEMO_KEEP_FRAMES:-}" ]; then
            cp -R "$frames" "$DEMO_KEEP_FRAMES/" 2>/dev/null || true
            cp "$WORK/$scene.log" "$DEMO_KEEP_FRAMES/" || true
        fi
        status=1
        continue
    fi
    poster=()
    [ -n "${POSTERS:-}" ] && poster=(--poster "$OUT/$scene.png")
    "$MAKEGIF" "$frames" "$OUT/$scene.gif" --width "$WIDTH" ${poster[@]+"${poster[@]}"}
    [ -n "${DEMO_KEEP_FRAMES:-}" ] && cp -R "$frames" "$DEMO_KEEP_FRAMES/" || true
    [ -n "${DEMO_KEEP_FRAMES:-}" ] && cp "$WORK/$scene.log" "$DEMO_KEEP_FRAMES/" || true
    rm -rf "$frames"
done

exit $status
