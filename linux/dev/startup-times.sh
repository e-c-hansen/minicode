#!/usr/bin/env bash
# startup-times.sh — opens minicode a few times with its startup log on
# (MINICODE_STARTUP_LOG=1, linux/src/StartupLog.h) and prints each run's
# phases, once for each GTK renderer: the numbers linux/PARITY.md R3 asks
# for from the desktop, where GTK draws on the GPU (an Xvfb has none).
#
#   linux/dev/startup-times.sh [-n runs] [-r "default gl vulkan cairo"] BINARY [FOLDER]
#
# Each run opens a window on FOLDER (the current folder by default), waits
# until the log says the start has settled, leaves it two seconds more, and
# ends the app by the PID this script started. Quit every minicode first: a
# second one hands its folder to the one already running and logs nothing.
# The whole logs stay in a folder under $TMPDIR, named at the end.
set -u
runs=3
renderers="default gl vulkan cairo"
while getopts n:r: opt; do
    case $opt in
        n) runs=$OPTARG ;;
        r) renderers=$OPTARG ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
bin=${1:?usage: startup-times.sh [-n runs] [-r renderers] BINARY [FOLDER]}
dir=${2:-$PWD}
out=$(mktemp -d "${TMPDIR:-/tmp}/minicode-startup-XXXXXX")

for r in $renderers; do
    for i in $(seq 1 "$runs"); do
        log="$out/$r-$i.log"
        # MINICODE_FOREGROUND keeps a build that detaches from the terminal
        # (R2) in this process, so its PID is the app's.
        if [ "$r" = default ]; then
            env -u GSK_RENDERER MINICODE_STARTUP_LOG=1 MINICODE_FOREGROUND=1 \
                "$bin" "$dir" >/dev/null 2>"$log" &
        else
            env GSK_RENDERER="$r" MINICODE_STARTUP_LOG=1 MINICODE_FOREGROUND=1 \
                "$bin" "$dir" >/dev/null 2>"$log" &
        fi
        pid=$!
        for _ in $(seq 1 600); do   # a minute at most
            grep -q 'settled' "$log" && break
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
        sleep 2
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        echo "== GSK_RENDERER=$r, run $i"
        grep -E 'main \(|gtk: started|window: session restored|window: realized|window: presented|frame 1:|first frame drawn|settled|terminal:|browser:|language server started' "$log" ||
            echo "   (no startup log: is another minicode running?)"
    done
done
echo "Logs: $out"
