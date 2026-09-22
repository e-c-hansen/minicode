#!/bin/bash
# Measure how much memory MiniCode needs for a day-to-day workload, and how
# much the usual alternative needs for the same work: VS Code, plus Chrome for
# the web page and Preview for the typeset PDF.
#
#   scripts/membench.sh            (run through `make membench`)
#
#   SETTLE=60     seconds to let each setup settle before measuring
#   SAMPLES=5     measurements per setup, 5 seconds apart; the median is kept
#   BENCH_URL=... the page both browsers show
#
# The workload, the same on both sides:
#   - a copy of this repository open as the project
#   - tests/run_tests.cpp open in the editor, with clangd running on it
#   - a zsh terminal open
#   - demo/paper/notes.tex typeset by tectonic, and the PDF on screen
#   - one web page open
#
# MiniCode does all of it in one window: its own terminal, browser panel and
# LaTeX preview. The other side is a separate VS Code instance with a scratch
# profile and only the clangd extension, a Chrome window with a fresh profile,
# and Preview showing the PDF, which tectonic typesets from VS Code's terminal.
# Your own VS Code, Chrome and settings are never touched: every app runs on
# scratch data under /tmp, and only processes this script started are stopped.
#
# What counts: every process each setup owns, found two ways. Children (VS
# Code's renderers and extension host, clangd, the shell) by parent pid, and
# helpers that launchd starts on an app's behalf (WebKit's page renderers,
# Chrome's crash handler) by the pid macOS holds responsible for them. The
# number is the physical footprint, what Activity Monitor calls Memory.
#
# Windows come to the front while this runs, for a few minutes in all. Leave
# the keyboard and mouse alone meanwhile.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
PROCMEM="$REPO/build/procmem"
MINICODE="$REPO/MiniCode.app/Contents/MacOS/MiniCode"
VSCODE_APP="/Applications/Visual Studio Code.app"
CHROME_APP="/Applications/Google Chrome.app"
TECTONIC="$HOME/Library/Application Support/MiniCode/bin/tectonic"
TECTONIC_CACHE="$HOME/Library/Caches/TectonicProject.Tectonic"
SETTLE="${SETTLE:-60}"
SAMPLES="${SAMPLES:-5}"
URL="${BENCH_URL:-https://en.wikipedia.org/wiki/Source-code_editor}"
WORK=/private/tmp/minicode-bench
P="$WORK/project"

for need in "$PROCMEM" "$MINICODE" "$VSCODE_APP" "$CHROME_APP" "$TECTONIC"; do
    [ -e "$need" ] || { echo "membench: missing $need" >&2; exit 1; }
done
CLANGD="$(xcrun -f clangd 2>/dev/null || true)"
[ -x "$CLANGD" ] || { echo "membench: clangd not found (xcode-select --install)" >&2; exit 1; }

started=()   # pids this script launched; the only ones it will stop
cleanup() {
    for pid in "${started[@]:-}"; do [ -n "$pid" ] && kill "$pid" 2>/dev/null || true; done
    # Anything still running on this script's scratch profiles (VS Code and
    # Chrome helpers can outlive their parent for a moment).
    sleep 2
    pkill -f "$WORK/vscode/data" 2>/dev/null || true
    pkill -f "$WORK/chrome" 2>/dev/null || true
}
trap cleanup EXIT

rm -rf "$WORK"
mkdir -p "$P" "$WORK/home-mc" "$WORK/home-vs" "$WORK/results"
for d in src tests demo Makefile; do cp -R "$REPO/$d" "$P/"; done
rm -f "$P/demo/paper/notes.pdf"   # typeset fresh on both sides
printf -- '-std=c++17\n-Isrc\n' >"$P/compile_flags.txt"   # same clangd config for both
cp -R "$TECTONIC_CACHE" "$WORK/tectonic-cache"

# ---------------------------------------------------------------- measuring

# The processes owned by the given root pids in one snapshot, summed by what
# they are. Prints "category<TAB>bytes" lines.
own() {
    awk -F'\t' -v roots="$*" '
        BEGIN { n = split(roots, r, " "); for (i = 1; i <= n; i++) in_set[r[i]] = 1 }
        { pid[NR] = $1; ppid[NR] = $2; rpid[NR] = $3; fp[NR] = $4; name[NR] = $5 }
        END {
            do {   # grow the set until no process joins
                changed = 0
                for (i = 1; i <= NR; i++)
                    if (!(pid[i] in in_set) && (ppid[i] in in_set || rpid[i] in in_set)) {
                        in_set[pid[i]] = 1; changed = 1
                    }
            } while (changed)
            for (i = 1; i <= NR; i++) {
                if (!(pid[i] in in_set)) continue
                p = name[i]; c = "other"
                if (p ~ /clangd/) c = "clangd"
                else if (p ~ /\/(zsh|login|bash)$/) c = "shell"
                else if (p ~ /WebKit/) c = "WebKit (web page, PDF)"
                else if (p ~ /MiniCode/) c = "MiniCode"
                else if (p ~ /Visual Studio Code/) c = "VS Code"
                else if (p ~ /Google Chrome/) c = "Chrome"
                else if (p ~ /Preview/) c = "Preview"
                else { sub(/.*\//, "", p); c = "other: " p }
                sum[c] += fp[i]
            }
            for (c in sum) printf "%s\t%d\n", c, sum[c]
        }' "$SNAP"
}

# Take SAMPLES snapshots of the setup under the given roots, keep the one with
# the median total, and write it to results/<label>.
measure() {
    local label="$1"; shift
    local i totals=()
    for ((i = 1; i <= SAMPLES; i++)); do
        SNAP="$WORK/results/$label.snap$i"
        "$PROCMEM" >"$SNAP"
        own "$@" >"$WORK/results/$label.$i"
        totals+=("$(awk -F'\t' '{s += $2} END {print s}' "$WORK/results/$label.$i") $i")
        if [ "$i" -lt "$SAMPLES" ]; then sleep 5; fi
    done
    local median
    median="$(printf '%s\n' "${totals[@]}" | sort -n | awk -v m=$(( (SAMPLES + 1) / 2 )) 'NR == m {print $2}')"
    cp "$WORK/results/$label.$median" "$WORK/results/$label"
    SNAP="$WORK/results/$label.snap$median"
}

# Stop the setup: the roots, then anything they owned that is still alive.
stop() {
    local members
    members="$(awk -F'\t' -v roots="$*" '
        BEGIN { n = split(roots, r, " "); for (i = 1; i <= n; i++) s[r[i]] = 1 }
        { p[NR] = $1; pp[NR] = $2; rp[NR] = $3 }
        END { do { c = 0; for (i = 1; i <= NR; i++)
                if (!(p[i] in s) && (pp[i] in s || rp[i] in s)) { s[p[i]] = 1; c = 1 } } while (c)
              for (x in s) print x }' "$SNAP")"
    kill "$@" 2>/dev/null || true
    sleep 3
    for pid in $members; do kill "$pid" 2>/dev/null || true; done
}

wait_for() {   # wait_for <seconds> <description> <command...>
    local limit="$1" what="$2"; shift 2
    for ((t = 0; t < limit; t++)); do "$@" && return 0; sleep 1; done
    echo "membench: gave up waiting for $what" >&2
    exit 1
}

# ---------------------------------------------------------------- MiniCode

echo "== MiniCode: launching (settles for ${SETTLE}s)"
# Through LaunchServices, as Finder or the Dock would, and in the background
# (-g). That makes macOS hold MiniCode, not this shell, responsible for the
# WebKit helpers its browser starts, so they are counted as its own.
MCAPP="$REPO/MiniCode.app"
open -n -g -a "$MCAPP" --stdout "$WORK/minicode.log" --stderr "$WORK/minicode.log" \
    --env HOME="$WORK/home-mc" --env ZDOTDIR="$WORK/home-mc" \
    --env MINICODE_SETTINGS="$WORK/home-mc/settings.conf" \
    --env MINICODE_TECTONIC="$TECTONIC" --env TECTONIC_CACHE_DIR="$WORK/tectonic-cache" \
    --env MINICODE_BENCH=1 --env MINICODE_BENCH_CPP="$P/tests/run_tests.cpp" \
    --env MINICODE_BENCH_TEX="$P/demo/paper/notes.tex" --env MINICODE_BENCH_URL="$URL" \
    --args "$P"
sleep 2
mc="$(pgrep -n -f "$MCAPP/Contents/MacOS/MiniCode")"; started+=("$mc")
wait_for 60 "MiniCode's workload" grep -q "minicode bench: ready" "$WORK/minicode.log"
clangd_under() { SNAP="$WORK/probe"; "$PROCMEM" >"$SNAP"; own "$1" | grep -q '^clangd'; }
wait_for 90 "clangd under MiniCode" clangd_under "$mc"
sleep "$SETTLE"
echo "== MiniCode: measuring"
measure minicode "$mc"
stop "$mc"
if [ "${ONLY:-}" = minicode ]; then
    awk -F'\t' '{printf "  %-26s %6.0f MB\n", $1, $2 / 1048576}' "$WORK/results/minicode"
    exit 0
fi

# ---------------------------------------------------------------- VS Code + Chrome + Preview

VSDATA="$WORK/vscode/data"; VSEXT="$WORK/vscode/ext"
mkdir -p "$VSDATA/User" "$VSEXT"
echo "== VS Code: installing the clangd extension into a scratch profile"
"$VSCODE_APP/Contents/Resources/app/bin/code" --user-data-dir "$VSDATA" \
    --extensions-dir "$VSEXT" --install-extension llvm-vs-code-extensions.vscode-clangd \
    >"$WORK/vscode-install.log" 2>&1
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
# started: a zsh in the integrated terminal, where it types the tectonic
# command a person would. (A task set to run on folder open did the same,
# but VS Code skipped it on some launches.)
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
    const t = vscode.window.createTerminal({ name: 'zsh', shellPath: '/bin/zsh',
        shellArgs: ['-il'], env: { TECTONIC_CACHE_DIR: '$WORK/tectonic-cache' } });
    t.show();
    t.sendText("'$TECTONIC' -X compile demo/paper/notes.tex");
};
EOF

echo "== VS Code, Chrome, Preview: launching (settles for ${SETTLE}s)"
env -i PATH="/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin" \
    HOME="$WORK/home-vs" ZDOTDIR="$WORK/home-vs" USER="$USER" LOGNAME="$USER" \
    SHELL=/bin/zsh LANG=en_US.UTF-8 TMPDIR="${TMPDIR:-/tmp}" \
    "$VSCODE_APP/Contents/MacOS/Electron" --user-data-dir "$VSDATA" \
    --extensions-dir "$VSEXT" --extensionDevelopmentPath="$BENCHEXT" \
    --new-window --skip-welcome --skip-release-notes \
    --disable-workspace-trust "$P" "$P/tests/run_tests.cpp" \
    >"$WORK/vscode.log" 2>&1 &
vs=$!; started+=("$vs")
"$CHROME_APP/Contents/MacOS/Google Chrome" --user-data-dir="$WORK/chrome" \
    --no-first-run --no-default-browser-check --disable-sync --new-window "$URL" \
    >"$WORK/chrome.log" 2>&1 &
ch=$!; started+=("$ch")
wait_for 120 "tectonic in VS Code's terminal" test -s "$P/demo/paper/notes.pdf"
sleep 2
open -n -g -a Preview "$P/demo/paper/notes.pdf"
sleep 2
pv="$(pgrep -n -x Preview)"; started+=("$pv")
wait_for 90 "clangd under VS Code" clangd_under "$vs"
shell_under() { SNAP="$WORK/probe"; "$PROCMEM" >"$SNAP"; own "$1" | grep -q '^shell'; }
wait_for 30 "a shell in VS Code's terminal" shell_under "$vs"
sleep "$SETTLE"
echo "== VS Code, Chrome, Preview: measuring"
measure others "$vs" "$ch" "$pv"
stop "$vs" "$ch" "$pv"

# ---------------------------------------------------------------- report

mb() { awk '{printf "%.0f", $1 / 1048576}'; }
total() { awk -F'\t' '{s += $2} END {print s}' "$1" | mb; }
row() { sort -t$'\t' -k2 -nr "$1" | awk -F'\t' '{printf "  %-26s %6.0f MB\n", $1, $2 / 1048576}'; }
{
    echo "MiniCode membench, $(date '+%Y-%m-%d %H:%M')"
    echo "$(sw_vers -productName) $(sw_vers -productVersion), $(sysctl -n machdep.cpu.brand_string), $(( $(sysctl -n hw.memsize) / 1073741824 )) GB"
    echo "MiniCode $(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$REPO/MiniCode.app/Contents/Info.plist"), VS Code $(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$VSCODE_APP/Contents/Info.plist"), Chrome $(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$CHROME_APP/Contents/Info.plist")"
    echo "Settled ${SETTLE}s, median of ${SAMPLES} samples, physical footprint"
    echo
    echo "MiniCode: $(total "$WORK/results/minicode") MB"
    row "$WORK/results/minicode"
    echo
    echo "VS Code + Chrome + Preview: $(total "$WORK/results/others") MB"
    row "$WORK/results/others"
} | tee "$REPO/build/membench.txt"
echo
echo "Saved to build/membench.txt"
