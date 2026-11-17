#!/bin/bash
# ci-summary.sh JOB LOGDIR — one line for each failed, slow, leaking or
# crashed check in the logs a CI job's steps wrote (*.log in LOGDIR), as
# GitHub Actions annotations and in the job's summary page
# ($GITHUB_STEP_SUMMARY), so a red run says what broke at the top of its
# page and on the pull request, without opening a log. Run outside Actions
# (scripts/ci-local.sh), it prints the same lines to stdout.
#
# What it reads, as the test programs print it:
#   "  FAIL [group] file:line  ..."   the core tests and both GUI harnesses
#   "FATAL ..."                       a GUI harness's crash or stuck step
#   "  LEAK [suite] ..."              state a GUI suite left for the next
#                                     (fails the suite)
#   "  LEFT [suite] ..."              the same, warned only: a settings or
#                                     keys file the harness put back, or a
#                                     terminal left open
#   "  SLOW [suite] ..."              a timing check over its bound, within
#                                     the room CI gives (a pass)
#   "  NEAR [suite] ..."              the timing checks closest to their bounds
#   "  FLAKY [suite] <first failure>" a suite that failed, then passed when
#                                     its harness ran it again alone in a
#                                     fresh process (tests/GuiRetry.h): its
#                                     first run's FAIL and LEAK lines are not
#                                     counted as failures, the FLAKY line is
#   "  RETRY [suite] ..."             a suite that failed its retry too (its
#                                     FAIL lines stand)
#   sanitizer reports, compiler errors, and make's and ninja's failures.
#
# A job may need at most MAX_FLAKY (3) retried suites in all, whatever
# processes they ran in: past that, more is wrong than one flaky check, so
# this fails the job with a line saying so (docs/ci.md, "Retries"). It
# exits 0 otherwise; the steps before it decide whether the job failed.
set -u
job="${1:?usage: ci-summary.sh JOB LOGDIR}"
dir="${2:?usage: ci-summary.sh JOB LOGDIR}"
out="${GITHUB_STEP_SUMMARY:-/dev/stdout}"
inActions="${GITHUB_ACTIONS:-}"

shopt -s nullglob
logs=("$dir"/*.log)
if [ ${#logs[@]} -eq 0 ]; then
    { echo "### $job"; echo; echo "No test logs were written (the job stopped before its tests)."; } >>"$out"
    exit 0
fi

# Strip a log line to its text: GitHub's timestamps and ANSI colors.
clean() { sed -E 's/^[0-9-]+T[0-9:.]+Z //; s/\x1b\[[0-9;]*[A-Za-z]//g'; }

grab() {   # grab REGEX: matching lines of every log, cleaned, in order, unique
    cat "${logs[@]}" | clean | grep -E "$1" | awk '!seen[$0]++'
}

fails="$(grab '^  FAIL \[|^FATAL|SUMMARY: (Address|Undefined|Thread|Leak)Sanitizer|^WARNING: ThreadSanitizer|runtime error: ')"
leaks="$(grab '^  LEAK \[')"
flaky="$(grab '^  FLAKY \[')"
retried="$(grab '^  RETRY \[|^No suite retried: ')"
# A suite that passed its retry: its first run's FAIL and LEAK lines are the
# FLAKY line's business now, not failures.
if [ -n "$flaky" ]; then
    flakySuites="$(printf '%s\n' "$flaky" | sed -nE 's/^  FLAKY \[([^]]*)\].*/\1/p' | sort -u | tr '\n' ' ')"
    notFlaky() {
        awk -v names="$flakySuites" 'BEGIN { n = split(names, a, " "); for (i = 1; i <= n; i++) skip[a[i]] = 1 }
            { s = $0; if (match(s, /^  (FAIL|LEAK) \[[^]]*\]/)) { name = substr(s, 1, RLENGTH); sub(/^  (FAIL|LEAK) \[/, "", name); sub(/\]$/, "", name); if (name in skip) next } print }'
    }
    fails="$(printf '%s\n' "$fails" | notFlaky)"
    leaks="$(printf '%s\n' "$leaks" | notFlaky)"
fi
MAX_FLAKY="${MAX_FLAKY:-3}"
nflaky=0
[ -n "$flaky" ] && nflaky=$(printf '%s\n' "$flaky" | wc -l | tr -d ' ')
if [ "$nflaky" -gt "$MAX_FLAKY" ]; then
    fails="$(printf '%s\n%s' "  FAIL [retries] $nflaky suites in this job passed only when retried, more than the $MAX_FLAKY a run may: more is wrong than one flaky check" "$fails" | sed '/^$/d')"
fi
left="$(grab '^  LEFT \[')"
slow="$(grab '^  SLOW \[')"
# Closest first, across every log: the GUI checks run as two processes, each
# listing its own ten.
near="$(grab '^  NEAR \[' | awk '{ p = $0; sub(/.*: /, "", p); sub(/%.*/, "", p); print p "\t" $0 }' |
        sort -t "$(printf '\t')" -k1,1 -rn | cut -f2- | head -10)"
builds="$(grab '(: error: |^FAILED: |^make(\[[0-9]+\])?: \*\*\* |^ninja: build stopped|^error: )' | head -15)"
# Each log's own totals, named for the log (core-tests, gui-checks-1, ...).
totals="$(for l in "${logs[@]}"; do
              clean <"$l" | grep -E '^(GUI checks: )?[0-9]+ passed, [0-9]+ failed|^Also: ' |
                  sed "s|^|$(basename "$l" .log): |"
          done)"

count() { [ -z "$1" ] && echo 0 || printf '%s\n' "$1" | wc -l | tr -d ' '; }
nf=$(count "$fails"); nl=$(count "$leaks"); ns=$(count "$slow")
# Leaks fail their suite unless the job only prints them (MC_GUI_LEAKS=note).
leakKind=error
[ "${MC_GUI_LEAKS:-}" = note ] && leakKind=warning

# An annotation's text may not hold raw %, CR or LF.
annotate() {   # annotate error|warning LINE
    [ -n "$inActions" ] || return 0
    local text="${2:0:400}"
    text="${text//'%'/%25}"
    printf '::%s title=%s::%s\n' "$1" "$job" "$text"
}

{
    echo "### $job"
    echo
    if [ "$nf" -eq 0 ] && [ -z "$builds" ]; then
        echo "No failed checks."
    else
        echo "**$nf failed check(s)**"
    fi
    [ "$nl" -gt 0 ] && echo "**$nl leak(s) between GUI suites**"
    [ "$ns" -gt 0 ] && echo "$ns timing check(s) over their bound, passed within CI's room (SLOW)"
    [ "$nflaky" -gt 0 ] && echo "**$nflaky suite(s) failed, then passed when retried alone (FLAKY): each needs a fix** (docs/ci.md, \"Retries\"; the soak issue tracks them)"
    # Which suites or groups failed, and how often: a cascade reads at once.
    if [ "$nf" -gt 0 ]; then
        bySuite="$(printf '%s\n' "$fails" | sed -nE 's/^  FAIL \[([^]]*)\].*/\1/p' | sort | uniq -c |
                   sort -rn | awk '{printf "%s%s (%s)", (NR>1?", ":""), $2, $1}')"
        [ -n "$bySuite" ] && echo "Failed in: $bySuite"
    fi
    echo
    lines() {   # at most 60 lines of a list, as Markdown items
        [ -n "$1" ] || return 0
        printf '%s\n' "$1" | head -60 | while IFS= read -r l; do printf -- '- `%s`\n' "${l//\`/\'}"; done
        local n; n=$(count "$1")
        [ "$n" -gt 60 ] && echo "- and $((n - 60)) more in the log"
        return 0
    }
    lines "$fails"
    if [ "$nf" -eq 0 ] && [ -n "$builds" ]; then
        echo "Build or step errors:"
        lines "$builds"
    fi
    lines "$flaky"
    lines "$leaks"
    lines "$slow"
    if [ -n "$retried" ]; then
        echo
        echo "Retries that did not pass, or were not made:"
        lines "$retried"
    fi
    if [ -n "$left" ]; then
        echo
        echo "<details><summary>$(count "$left") thing(s) GUI suites left that the harness put back or only warns about (LEFT)</summary>"
        echo
        lines "$left"
        echo "</details>"
    fi
    if [ -n "$totals" ]; then
        echo
        echo "<details><summary>Totals</summary>"
        echo
        lines "$totals"
        echo "</details>"
    fi
    if [ -n "$near" ]; then
        echo
        echo "<details><summary>Timing checks closest to their bounds</summary>"
        echo
        lines "$near"
        echo "</details>"
    fi
    echo
} >>"$out"

# Annotations: GitHub shows ten of each kind per step, so failures first.
if [ -n "$fails" ]; then printf '%s\n' "$fails" | head -20 | while IFS= read -r l; do annotate error "$l"; done; fi
if [ "$nf" -eq 0 ] && [ -n "$builds" ]; then printf '%s\n' "$builds" | while IFS= read -r l; do annotate error "$l"; done; fi
if [ -n "$leaks" ]; then printf '%s\n' "$leaks" | while IFS= read -r l; do annotate "$leakKind" "$l"; done; fi
if [ -n "$slow" ]; then printf '%s\n' "$slow" | while IFS= read -r l; do annotate warning "$l"; done; fi
if [ -n "$flaky" ]; then printf '%s\n' "$flaky" | while IFS= read -r l; do annotate warning "$l"; done; fi
[ "$nflaky" -gt "$MAX_FLAKY" ] && exit 1
exit 0
