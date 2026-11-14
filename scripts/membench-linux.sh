#!/bin/bash
# Measure how much memory minicode needs for a day-to-day workload on Linux,
# and how much the usual alternative needs for the same work: VS Code, plus
# Chrome for the web page and GNOME's Papers for the typeset PDF. The Linux
# twin of scripts/membench-mac.sh; the two workloads are the same.
#
#   scripts/membench-linux.sh      (run through `make membench-linux`)
#
#   SETTLE=60     seconds to let each setup settle before measuring
#   SAMPLES=5     measurements per setup, 5 seconds apart; the median is kept
#   BENCH_URL=... the page both browsers show
#   MEMBENCH_DISPLAY=xvfb (default) | desktop
#   BUILD=skip    use linux/build-bench as it is
#
# The workload, the same on both sides:
#   - a copy of this repository open as the project
#   - tests/run_tests.cpp open in the editor, with clangd running on it
#   - a bash terminal open
#   - demo/paper/notes.tex typeset by tectonic, and the PDF on screen
#   - one web page open
#
# minicode does all of it with its own editor, terminal, browser pane and
# LaTeX preview (a release build, linux/build-bench; the LaTeX preview is in
# a second window, as on the Mac). The other side is VS Code (the snap) with
# a scratch profile and only the clangd extension, a Chrome with a fresh
# profile, and Papers showing the PDF, which tectonic typesets from VS Code's
# terminal. Your own VS Code, Chrome and settings are never touched: every app
# runs on scratch data under /tmp/membench-linux, and only processes this
# script started are stopped.
#
# The display: by default a private Xvfb of its own, so no window appears on
# your screen, with each side under its own dbus-run-session and GTK, Chrome
# and Electron on X11. A virtual display has no GPU, so Chrome, Electron and
# WebKit draw in software, which changes their numbers somewhat.
# MEMBENCH_DISPLAY=desktop runs on the session's own display instead (windows
# appear for a few minutes; leave the keyboard and mouse alone).
#
# What counts: every process each setup owns, found two ways. Descendants of
# the launched root (found from /proc's parent pids), and processes that share
# a snap's systemd scope with one of them (a launcher that exits leaves its
# children to init, which the parent pid cannot see). The number is PSS, the
# Pss: line of /proc/<pid>/smaps_rollup: each shared page is split among the
# processes mapping it, so WebKitGTK's, Chrome's and GTK's shared libraries
# are counted once between them. The Mac's script sums the physical footprint.
# The script's own bash, the Xvfb server and the harness launcher are not
# counted on either side.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
PROCMEM="$REPO/build/procmem-linux"
MINICODE="$REPO/linux/build-bench/minicode"
CODE="/snap/bin/code"
CHROME="$(command -v google-chrome || true)"
PAPERS="$(command -v papers || true)"
CLANGD="$(command -v clangd || true)"
TECTONIC="${MINICODE_TECTONIC:-$HOME/.local/share/minicode/bin/tectonic}"
TECTONIC_CACHE="${TECTONIC_CACHE_DIR:-$HOME/.cache/tectonic}"
SETTLE="${SETTLE:-60}"
SAMPLES="${SAMPLES:-5}"
URL="${BENCH_URL:-https://en.wikipedia.org/wiki/Source-code_editor}"
MODE="${MEMBENCH_DISPLAY:-xvfb}"
WORK=/tmp/membench-linux
P="$WORK/project"
REALHOME="$HOME"

for need in "$CODE" "$CHROME" "$PAPERS" "$CLANGD" "$TECTONIC" "$TECTONIC_CACHE"; do
    [ -n "$need" ] && [ -e "$need" ] || { echo "membench: missing ${need:-a program}" >&2; exit 1; }
done
if [ "$MODE" = xvfb ]; then
    command -v Xvfb >/dev/null || { echo "membench: Xvfb not found" >&2; exit 1; }
fi

if [ "${BUILD:-}" != skip ]; then
    echo "== building linux/build-bench (release)"
    (cd "$REPO/linux" && { [ -d build-bench ] || meson setup build-bench --buildtype=release >/dev/null; } \
        && ninja -C build-bench >"$REPO/build/membench-linux-build.log" 2>&1) \
        || { echo "membench: build failed, see build/membench-linux-build.log" >&2; exit 1; }
fi
mkdir -p "$REPO/build"
cc -O2 -Wall -Wextra "$REPO/tools/procmem-linux.c" -o "$PROCMEM"
[ -x "$MINICODE" ] || { echo "membench: missing $MINICODE" >&2; exit 1; }

started=()   # pids this script launched; the only ones it will stop
members=()   # every pid seen in a counted setup, for the final sweep
# Everything this script started carries its own XDG_RUNTIME_DIR; this finds
# what the process tree lost (a daemon the bus started, reparented to init
# after its parent died) without touching anything else of yours.
mine() {
    local p
    for p in $(ps -u "$(id -u)" -o pid=); do
        { tr "\0" "\n" <"/proc/$p/environ"; } 2>/dev/null | grep -qx "XDG_RUNTIME_DIR=$WORK/runtime" && echo "$p"
    done
    return 0
}
cleanup() {
    for pid in "${started[@]:-}"; do [ -n "$pid" ] && kill "$pid" 2>/dev/null || true; done
    sleep 2
    local left; left="$(mine)"
    for pid in "${members[@]:-}" $left; do [ -n "$pid" ] && kill "$pid" 2>/dev/null || true; done
    sleep 2
    for pid in $(mine); do kill -9 "$pid" 2>/dev/null || true; done
}
trap cleanup EXIT

rm -rf "$WORK"
mkdir -p "$P" "$WORK/home-mc" "$WORK/home-vs" "$WORK/home-vs/.config" "$WORK/results" "$WORK/runtime"
chmod 700 "$WORK/runtime"
for d in src tests demo Makefile; do cp -R "$REPO/$d" "$P/"; done
rm -f "$P/demo/paper/notes.pdf"   # typeset fresh on both sides
printf -- '-std=c++17\n-Isrc\n' >"$P/compile_flags.txt"   # same clangd config for both
cp -R "$TECTONIC_CACHE" "$WORK/tectonic-cache"
printf 'lsp.enabled = true\nlsp.cpp = %s\n' "$CLANGD" >"$WORK/home-mc/settings.conf"

# The display. Everything below runs under an env that sets it up.
DISPENV=()
if [ "$MODE" = xvfb ]; then
    Xvfb -displayfd 3 -screen 0 1920x1080x24 -nolisten tcp 3>"$WORK/xdisplay" >"$WORK/xvfb.log" 2>&1 &
    started+=("$!")
    for ((t = 0; t < 20; t++)); do [ -s "$WORK/xdisplay" ] && break; sleep 1; done
    [ -s "$WORK/xdisplay" ] || { echo "membench: Xvfb did not start" >&2; exit 1; }
    DISPENV=(DISPLAY=":$(head -n1 "$WORK/xdisplay")" GDK_BACKEND=x11 WAYLAND_DISPLAY=)
    CHROMEFLAGS=(--ozone-platform=x11)
    CODEFLAGS=(--ozone-platform=x11)
else
    DISPENV=()
    CHROMEFLAGS=()
    CODEFLAGS=()
fi
# A setup's environment: its own HOME and no session bus, config or runtime
# folder of yours (dbus-run-session makes the bus).
setupenv() {   # setupenv <home> <command...>
    local h="$1"; shift
    env -u DBUS_SESSION_BUS_ADDRESS -u XDG_SESSION_ID -u CLAUDECODE -u GNOME_TERMINAL_SERVICE \
        "${DISPENV[@]}" HOME="$h" XDG_CONFIG_HOME="$h/.config" XDG_CACHE_HOME="$h/.cache" \
        XDG_DATA_HOME="$h/.local/share" XDG_RUNTIME_DIR="$WORK/runtime" SHELL=/bin/bash \
        TECTONIC_CACHE_DIR="$WORK/tectonic-cache" "$@"
}

# ---------------------------------------------------------------- measuring

# The processes owned by the given root pids in one snapshot ($SNAP), summed
# by what they are. Prints "category<TAB>bytes" lines; with OWNFILE set, also
# every counted process to that file.
own() {
    awk -F'\t' -v roots="$*" -v listing="${OWNFILE:-/dev/null}" '
        BEGIN { n = split(roots, r, " "); for (i = 1; i <= n; i++) in_set[r[i]] = 1 }
        { pid[NR] = $1; ppid[NR] = $2; scope[NR] = $3; fp[NR] = $4; prog[NR] = $5; cmd[NR] = $6 }
        END {
            do {   # grow the set until no process joins
                changed = 0
                for (i = 1; i <= NR; i++) {
                    if (pid[i] in in_set) {
                        if (scope[i] ~ /^snap\./) snapscope[scope[i]] = 1
                        continue
                    }
                    if (ppid[i] in in_set) { in_set[pid[i]] = 1; changed = 1 }
                }
                for (i = 1; i <= NR; i++)
                    if (!(pid[i] in in_set) && scope[i] in snapscope) {
                        in_set[pid[i]] = 1; viascope[pid[i]] = 1; changed = 1
                    }
            } while (changed)
            for (i = 1; i <= NR; i++) {
                if (!(pid[i] in in_set)) continue
                p = prog[i]; c = ""
                if (p ~ /clangd/) c = "clangd"
                else if (cmd[i] ~ /alt-launch\.sh/ && p ~ /bash$/) c = "harness"
                else if (p ~ /\/(bash|zsh|dash|sh)$/) c = "shell"
                else if (p ~ /WebKit/ || p ~ /\/webkit/) c = "WebKit (web page, PDF)"
                else if (p ~ /build-bench\/minicode$/) c = "minicode"
                else if (p ~ /\/snap\/code\// || p ~ /\/code$/) c = "VS Code"
                else if (p ~ /\/chrome/ || p ~ /google-chrome/) c = "Chrome"
                else if (p ~ /\/papers/) c = "Papers"
                else if (p ~ /dbus/) c = "dbus"
                else { sub(/.*\//, "", p); c = "other: " p }
                sum[c] += fp[i]
                printf "%s\t%s\t%d\t%s\t%s%s\n", pid[i], c, fp[i], prog[i], (pid[i] in viascope ? "[scope] " : ""), cmd[i] >> listing
            }
            for (c in sum) if (c != "harness") printf "%s\t%d\n", c, sum[c]
        }' "$SNAP"
}

# Take SAMPLES snapshots of the setup under the given roots, keep the one with
# the median total, and write it to results/<label> (the processes of it to
# results/<label>.procs).
measure() {
    local label="$1"; shift
    local i totals=()
    for ((i = 1; i <= SAMPLES; i++)); do
        SNAP="$WORK/results/$label.snap$i"
        "$PROCMEM" >"$SNAP"
        : >"$WORK/results/$label.procs$i"
        OWNFILE="$WORK/results/$label.procs$i" own "$@" >"$WORK/results/$label.$i"
        totals+=("$(awk -F'\t' '{s += $2} END {print s}' "$WORK/results/$label.$i") $i")
        if [ "$i" -lt "$SAMPLES" ]; then sleep 5; fi
    done
    local median
    median="$(printf '%s\n' "${totals[@]}" | sort -n | awk -v m=$(( (SAMPLES + 1) / 2 )) 'NR == m {print $2}')"
    cp "$WORK/results/$label.$median" "$WORK/results/$label"
    cp "$WORK/results/$label.procs$median" "$WORK/results/$label.procs"
    SNAP="$WORK/results/$label.snap$median"
}

# The pids of a setup (roots and everything they own) right now.
pids_of() {
    SNAP="$WORK/probe"; "$PROCMEM" >"$SNAP"
    OWNFILE="$WORK/probe.procs" ; : >"$WORK/probe.procs"
    OWNFILE="$WORK/probe.procs" own "$@" >/dev/null
    cut -f1 "$WORK/probe.procs"
}

# Stop the setup: its roots, then anything they owned that is still alive.
stop() {
    local pids
    pids="$(pids_of "$@")"
    members+=($pids)
    kill "$@" 2>/dev/null || true
    sleep 3
    for pid in $pids; do kill "$pid" 2>/dev/null || true; done
    sleep 2
    for pid in $pids; do kill -9 "$pid" 2>/dev/null || true; done
}

wait_for() {   # wait_for <seconds> <description> <command...>
    local limit="$1" what="$2"; shift 2
    for ((t = 0; t < limit; t++)); do "$@" && return 0; sleep 1; done
    echo "membench: gave up waiting for $what" >&2
    exit 1
}
has() {   # has <category> <root...>: the setup owns a process of that category
    local cat="$1"; shift
    SNAP="$WORK/probe"; "$PROCMEM" >"$SNAP"
    own "$@" | grep -q "^$cat"
}

# ---------------------------------------------------------------- minicode

if [ -z "${SKIP_MINICODE:-}" ]; then
echo "== minicode: launching (settles for ${SETTLE}s)"
setupenv "$WORK/home-mc" MINICODE_SETTINGS="$WORK/home-mc/settings.conf" \
    MINICODE_TECTONIC="$TECTONIC" MINICODE_FOREGROUND=1 \
    MINICODE_BENCH=1 MINICODE_BENCH_CPP="$P/tests/run_tests.cpp" \
    MINICODE_BENCH_TEX="$P/demo/paper/notes.tex" MINICODE_BENCH_URL="$URL" \
    dbus-run-session -- "$MINICODE" "$P" >"$WORK/minicode.log" 2>&1 &
mc=$!; started+=("$mc")
wait_for 90 "minicode's workload" grep -q "minicode bench: ready" "$WORK/minicode.log"
wait_for 90 "clangd under minicode" has clangd "$mc"
wait_for 30 "a shell under minicode" has shell "$mc"
# The preview keeps its PDF in the runtime folder, not beside the source.
pdf_made() { [ -n "$(find "$WORK/runtime" -name '*.pdf' -size +1k 2>/dev/null | head -n1)" ]; }
wait_for 60 "the LaTeX preview's PDF" pdf_made
sleep "$SETTLE"
echo "== minicode: measuring"
measure minicode "$mc"
stop "$mc"
if [ "${ONLY:-}" = minicode ]; then
    awk -F'\t' '{printf "  %-26s %6.0f MB\n", $1, $2 / 1048576}' "$WORK/results/minicode"
    exit 0
fi
fi
rm -f "$P/demo/paper/notes.pdf"   # the other side typesets it itself

# ---------------------------------------------------------------- VS Code + Chrome + Papers

VSDATA="$WORK/vscode/data"; VSEXT="$WORK/vscode/ext"
mkdir -p "$VSDATA/User" "$VSEXT"
echo "== VS Code: installing the clangd extension into a scratch profile"
setupenv "$WORK/home-vs" "$CODE" --user-data-dir "$VSDATA" \
    --extensions-dir "$VSEXT" --install-extension llvm-vs-code-extensions.vscode-clangd \
    >"$WORK/vscode-install.log" 2>&1 || { echo "membench: extension install failed:" >&2; tail -5 "$WORK/vscode-install.log" >&2; exit 1; }
cat >"$VSDATA/User/settings.json" <<EOF
{
    "security.workspace.trust.enabled": false,
    "telemetry.telemetryLevel": "off",
    "update.mode": "none",
    "extensions.autoUpdate": false,
    "extensions.ignoreRecommendations": true,
    "workbench.startupEditor": "none",
    "workbench.tips.enabled": false,
    "clangd.path": "$CLANGD",
    "clangd.checkUpdates": false
}
EOF
# The terminal is opened by a tiny local extension, as soon as VS Code has
# started: a bash in the integrated terminal, where it types the tectonic
# command a person would.
BENCHEXT="$WORK/vscode/bench-ext"
mkdir -p "$BENCHEXT"
cat >"$BENCHEXT/package.json" <<EOF
{ "name": "membench", "publisher": "minicode", "version": "0.0.1",
  "engines": { "vscode": "^1.80.0" }, "main": "extension.js",
  "activationEvents": ["onStartupFinished"] }
EOF
cat >"$BENCHEXT/extension.js" <<EOF
const vscode = require('vscode');
exports.activate = () => {
    const t = vscode.window.createTerminal({ name: 'bash', shellPath: '/bin/bash',
        shellArgs: ['-i'], env: { TECTONIC_CACHE_DIR: '$WORK/tectonic-cache' } });
    t.show();
    t.sendText("'$TECTONIC' -X compile demo/paper/notes.tex");
};
EOF
# One launcher under one dbus session: VS Code (--wait keeps its launcher, so
# the app stays a descendant), Chrome, and Papers once the PDF exists.
cat >"$WORK/alt-launch.sh" <<EOF
#!/bin/bash
cd "$P"
"$CODE" --wait --user-data-dir "$VSDATA" --extensions-dir "$VSEXT" \\
    --extensionDevelopmentPath="$BENCHEXT" ${CODEFLAGS[@]:-} --password-store=basic --new-window --skip-welcome \\
    --skip-release-notes --disable-workspace-trust "$P" "$P/tests/run_tests.cpp" \\
    >"$WORK/vscode.log" 2>&1 &
"$CHROME" --user-data-dir="$WORK/chrome" --no-first-run --no-default-browser-check \\
    --disable-sync --password-store=basic ${CHROMEFLAGS[@]:-} --new-window "$URL" \\
    >"$WORK/chrome.log" 2>&1 &
for ((t = 0; t < 180; t++)); do [ -s "$P/demo/paper/notes.pdf" ] && break; sleep 1; done
sleep 2
"$PAPERS" "$P/demo/paper/notes.pdf" >"$WORK/papers.log" 2>&1 &
wait
EOF
chmod +x "$WORK/alt-launch.sh"

echo "== VS Code, Chrome, Papers: launching (settles for ${SETTLE}s)"
setupenv "$WORK/home-vs" dbus-run-session -- "$WORK/alt-launch.sh" >"$WORK/alt.log" 2>&1 &
alt=$!; started+=("$alt")
wait_for 180 "tectonic in VS Code's terminal" test -s "$P/demo/paper/notes.pdf"
wait_for 90 "clangd under VS Code" has clangd "$alt"
wait_for 30 "a shell in VS Code's terminal" has shell "$alt"
wait_for 60 "Papers" has Papers "$alt"
wait_for 60 "Chrome" has Chrome "$alt"
sleep "$SETTLE"
echo "== VS Code, Chrome, Papers: measuring"
measure others "$alt"
stop "$alt"

# ---------------------------------------------------------------- report

mb() { awk '{printf "%.0f", $1 / 1048576}'; }
total() { awk -F'\t' '{s += $2} END {print s}' "$1" | mb; }
row() { sort -t$'\t' -k2 -nr "$1" | awk -F'\t' '{printf "  %-26s %6.0f MB\n", $1, $2 / 1048576}'; }
vers() { grep -A1 CFBundleShortVersionString "$REPO/Info.plist" | sed -n 's/.*<string>\(.*\)<\/string>.*/\1/p' | head -n1; }
{
    echo "minicode membench (Linux), $(date '+%Y-%m-%d %H:%M')"
    echo "$(. /etc/os-release; echo "$PRETTY_NAME"), kernel $(uname -r), $(awk -F': ' '/model name/ {print $2; exit}' /proc/cpuinfo), $(nproc) cores, $(awk '/MemTotal/ {printf "%.0f", $2 / 1048576}' /proc/meminfo) GB"
    echo "minicode $(vers) ($(git -C "$REPO" rev-parse --short HEAD), release build), VS Code $("$CODE" --version 2>/dev/null | head -n1) (snap), $("$CHROME" --version 2>/dev/null), $("$PAPERS" --version 2>/dev/null | head -n1), $(clangd --version | head -n1)"
    echo "Display: $([ "$MODE" = xvfb ] && echo 'private Xvfb, no GPU (software drawing)' || echo 'the session desktop')"
    echo "Settled ${SETTLE}s, median of ${SAMPLES} samples, PSS"
    echo
    echo "minicode: $(total "$WORK/results/minicode") MB"
    row "$WORK/results/minicode"
    echo
    echo "VS Code + Chrome + Papers: $(total "$WORK/results/others") MB"
    row "$WORK/results/others"
} | tee "$REPO/build/membench-linux.txt"
cp "$WORK/results/minicode.procs" "$REPO/build/membench-linux-minicode.procs"
cp "$WORK/results/others.procs" "$REPO/build/membench-linux-others.procs"
echo
echo "Saved to build/membench-linux.txt (processes: build/membench-linux-*.procs)"
