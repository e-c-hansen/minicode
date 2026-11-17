#!/bin/bash
# ci-together.sh LOGDIR NAME COMMAND [NAME COMMAND]... — runs the commands
# at once and waits for all of them (.github/workflows/ci.yml: the core
# tests beside two halves of the GUI checks).
#
# Each command runs under bash with its output, stdout and stderr together,
# in LOGDIR/NAME.log, where scripts/ci-summary.sh reads it as it reads every
# other step's log, and in the step's own output as it comes, each line
# marked [NAME], so a hang or a crash still shows where each one was. The
# exit status is 0 only when every command's is; a line at the end says how
# each one ended and how long it took.
set -u
dir="${1:?usage: ci-together.sh LOGDIR NAME COMMAND [NAME COMMAND]...}"
shift
[ $# -ge 2 ] && [ $(($# % 2)) -eq 0 ] || {
    echo "usage: ci-together.sh LOGDIR NAME COMMAND [NAME COMMAND]..." >&2
    exit 2
}
mkdir -p "$dir"

names=()
pids=()
while [ $# -ge 2 ]; do
    name="$1" command="$2"
    shift 2
    (
        set -o pipefail
        bash -c "$command" 2>&1 | tee "$dir/$name.log" | awk -v p="[$name] " '{ print p $0; fflush() }'
        status=$?
        date +%s >"$dir/.$name.ended"
        exit "$status"
    ) &
    names+=("$name")
    pids+=($!)
    echo "[$name] started: $command"
done

started=$(date +%s)
failed=0
report=()
for i in "${!pids[@]}"; do
    wait "${pids[$i]}"
    status=$?
    ended=$(cat "$dir/.${names[$i]}.ended" 2>/dev/null || date +%s)
    rm -f "$dir/.${names[$i]}.ended"
    [ "$status" -eq 0 ] || failed=1
    report+=("${names[$i]}: exit status $status after $((ended - started)) s")
done
echo
echo "Ran together:"
printf '  %s\n' "${report[@]}"
exit "$failed"
