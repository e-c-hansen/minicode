#!/bin/bash
# soak.sh mac|linux OUTDIR ROUNDS [SUITES] — the GUI checks of one app, run
# ROUNDS times (.github/workflows/soak.yml), to find the checks that fail
# now and then. Run from the repository's top folder, after the build
# (`make guitest-build` on the Mac, `meson compile -C build` in linux/).
#
# Each round plays the suites in an order of its own: a seed drawn for the
# round (MC_GUI_SHUFFLE, tests/GuiRetry.h), printed here and kept in
# OUTDIR/rounds.tsv, so `MC_GUI_SHUFFLE=<seed>` with the same suites plays
# that order again. Without SUITES a round runs as CI's job does
# (.github/workflows/ci.yml, whose lists are read from it here): on the Mac
# two halves at once (GUI_SECOND_HALF and the rest) then the suites with
# tight bounds alone (GUI_ALONE), on GTK the two halves. With SUITES, the
# names this app has run in one process (a name only the other app has is
# left out). The GTK rounds alternate MALLOC_PERTURB_ between 165 and 90
# (glibc fills freed and new blocks with them), which once exposed a read
# of memory never written. CI's room for a slow runner is given
# (MC_GUI_WAIT_SCALE, MC_TIMING_SCALE); retries are not (no
# MC_GUI_RETRIES): the point is to see every failure.
#
# Writes, in OUTDIR:
#   round-N/*.log, round-N/pictures   each process's output, failure pictures
#   rounds.tsv     app, round, seed, MALLOC_PERTURB_, exit status
#   failures.tsv   app, round, seed, suite, its first failure, per failed
#                  suite per round (scripts/soak-report.sh makes the table)
# It exits 0 whatever the checks did, once every round has run.
set -u
platform="${1:?usage: soak.sh mac|linux OUTDIR ROUNDS [SUITES]}"
out="${2:?usage: soak.sh mac|linux OUTDIR ROUNDS [SUITES]}"
rounds="${3:?usage: soak.sh mac|linux OUTDIR ROUNDS [SUITES]}"
wantedSuites="${4:-}"
top="$(pwd)"
case "$platform" in mac) app=macOS ;; linux) app=GTK4 ;; *) echo "soak.sh: mac or linux, not $platform" >&2; exit 2 ;; esac
case "$rounds" in '' | *[!0-9]*) echo "soak.sh: ROUNDS is a number, not $rounds" >&2; exit 2 ;; esac
# The job's time limit allows about ten.
[ "$rounds" -ge 1 ] || rounds=1
[ "$rounds" -le 10 ] || { echo "soak.sh: at most 10 rounds; running 10"; rounds=10; }
mkdir -p "$out"
out="$(cd "$out" && pwd)"
: >"$out/rounds.tsv"
: >"$out/failures.tsv"

# A list from ci.yml: the folded value of VAR (a `VAR: >-` block) in JOB.
ci_list() {
    awk -v job="  $1:" -v key="$2: >-" '
        $0 == job { injob = 1; next }
        injob && /^  [^ #]/ { injob = 0 }
        injob && !grab && index($0, key) { grab = 1; indent = index($0, key) - 1; next }
        grab { match($0, /^ */); if ($0 ~ /[^ ]/ && RLENGTH > indent) { printf "%s ", $0; next } exit }
    ' "$top/.github/workflows/ci.yml" | xargs
}

# The suites this app has (each MC_GUI_SUITE(name) in its suite files).
known_suites() {
    if [ "$platform" = mac ]; then
        grep -ho 'MC_GUI_SUITE([a-z0-9_]*)' "$top"/mac/tests/gui/*.mm
    else
        grep -ho 'MC_GUI_SUITE([a-z0-9_]*)' "$top"/linux/tests/gui/*.cpp
    fi | sed 's/MC_GUI_SUITE(\(.*\))/\1/' | sort -u
}

suites=""
if [ -n "$wantedSuites" ]; then
    known="$(known_suites)"
    for s in $wantedSuites; do
        if printf '%s\n' "$known" | grep -qx -- "$s"; then suites="$suites $s"; fi
    done
    suites="$(echo $suites)"
    if [ -z "$suites" ]; then
        echo "soak.sh: none of \"$wantedSuites\" is a suite of the $app app; no rounds"
        exit 0
    fi
fi

export MC_GUI_WAIT_SCALE=1.5 MC_TIMING_SCALE=1.5
unset MC_GUI_RETRIES MC_GUI_SUITES MC_GUI_SKIP_SUITES

# The suites that failed in a round's logs, each with its first failure:
# FAIL and LEAK lines, and a crash or a stuck step naming its suite.
round_failures() {   # round_failures DIR -> "suite<TAB>line" lines
    cat "$1"/*.log 2>/dev/null | sed -E 's/\x1b\[[0-9;]*[A-Za-z]//g' | awk '
        /^  (FAIL|LEAK) \[/ {
            s = $0; sub(/^  (FAIL|LEAK) \[/, "", s); name = s; sub(/\].*/, "", name)
            line = $0; sub(/^  /, "", line)
        }
        /^FATAL/ {
            name = ""; if (match($0, /\[[^]]*\]/)) name = substr($0, RSTART + 1, RLENGTH - 2)
            line = $0
        }
        /^  (FAIL|LEAK) \[/ || /^FATAL/ {
            if (name == "") name = "(outside a suite)"
            gsub(/\t/, " ", line)
            if (!(name in seen)) { seen[name] = 1; print name "\t" line }
        }'
}

for r in $(seq 1 "$rounds"); do
    seed=$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')
    dir="$out/round-$r"
    mkdir -p "$dir/pictures"
    export MC_GUI_SHUFFLE="$seed" PICTURES="$dir/pictures"
    perturb="-"
    echo
    if [ "$platform" = mac ]; then
        echo "=== Round $r of $rounds (macOS): MC_GUI_SHUFFLE=$seed"
        if [ -n "$suites" ]; then
            export SOAK_SUITES="$suites"
            scripts/ci-together.sh "$dir" gui-checks \
                'MC_GUI_SNAPSHOTS="$PICTURES/suites" ./build/guitest $SOAK_SUITES'
            status=$?
        else
            GUI_SECOND_HALF="$(ci_list macos GUI_SECOND_HALF)"
            GUI_ALONE="$(ci_list macos GUI_ALONE)"
            export GUI_SECOND_HALF GUI_ALONE
            together=0
            scripts/ci-together.sh "$dir" \
                gui-checks-1 'MC_GUI_SKIP_SUITES="$GUI_SECOND_HALF $GUI_ALONE" MC_GUI_SNAPSHOTS="$PICTURES/first-half" ./build/guitest' \
                gui-checks-2 'MC_GUI_SNAPSHOTS="$PICTURES/second-half" ./build/guitest $GUI_SECOND_HALF' || together=$?
            alone=0
            scripts/ci-together.sh "$dir" \
                gui-checks-alone 'MC_GUI_SNAPSHOTS="$PICTURES/alone" ./build/guitest $GUI_ALONE' || alone=$?
            status=$((together || alone))
        fi
    else
        # Odd rounds 165 (0xA5), even 90 (0x5A).
        perturb=$([ $((r % 2)) -eq 1 ] && echo 165 || echo 90)
        export MALLOC_PERTURB_="$perturb" MC_GUI_TOTAL_SECONDS=1080 ROUND="$r"
        echo "=== Round $r of $rounds (GTK4): MC_GUI_SHUFFLE=$seed MALLOC_PERTURB_=$perturb"
        if [ -n "$suites" ]; then
            export SOAK_SUITES="$suites"
            (cd linux && "$top/scripts/ci-together.sh" "$dir" gui-checks \
                'MC_GUI_SUITES="$SOAK_SUITES" MC_GUI_SNAPSHOTS="$PICTURES/suites" meson test -C build --suite gui --no-rebuild --logbase soak-$ROUND -v --print-errorlogs')
            status=$?
        else
            GUI_SECOND_HALF="$(ci_list linux GUI_SECOND_HALF)"
            export GUI_SECOND_HALF
            (cd linux && "$top/scripts/ci-together.sh" "$dir" \
                gui-checks-1 'MC_GUI_SKIP_SUITES="$GUI_SECOND_HALF" MC_GUI_SNAPSHOTS="$PICTURES/first-half" meson test -C build --suite gui --no-rebuild --logbase soak-$ROUND-first-half -v --print-errorlogs' \
                gui-checks-2 'MC_GUI_SUITES="$GUI_SECOND_HALF" MC_GUI_SNAPSHOTS="$PICTURES/second-half" meson test -C build --suite gui --no-rebuild --logbase soak-$ROUND-second-half -v --print-errorlogs')
            status=$?
        fi
    fi
    printf '%s\t%s\t%s\t%s\t%s\n' "$app" "$r" "$seed" "$perturb" "$status" >>"$out/rounds.tsv"
    failed="$(round_failures "$dir")"
    if [ "$status" -ne 0 ] && [ -z "$failed" ]; then
        failed="$(printf '(no suite named)\tthe round failed (exit status %s) without naming a suite; see its logs' "$status")"
    fi
    if [ -n "$failed" ]; then
        printf '%s\n' "$failed" | while IFS="$(printf '\t')" read -r suite line; do
            printf '%s\t%s\t%s\t%s\t%s\n' "$app" "$r" "$seed" "$suite" "$line" >>"$out/failures.tsv"
        done
        echo "=== Round $r failed in: $(printf '%s\n' "$failed" | cut -f1 | xargs)"
    else
        echo "=== Round $r: every suite passed"
    fi
done
exit 0
