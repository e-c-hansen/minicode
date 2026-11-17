#!/bin/bash
# soak-report.sh OUT.md DIR... — the soak workflow's table
# (.github/workflows/soak.yml): from each DIR's rounds.tsv and failures.tsv
# (scripts/soak.sh), one row per suite that failed in any round, with in
# how many of its app's rounds it failed, those rounds' seeds, and its
# first failure. The workflow puts it on the run's page and in the issue
# "Soak: flaky checks". Plain bash and awk, so it runs where the report
# job has no checkout (the soak jobs ship it with their results).
set -u
outFile="${1:?usage: soak-report.sh OUT.md DIR...}"
shift
rounds="$(cat "${@/%//rounds.tsv}" 2>/dev/null)"
failures="$(cat "${@/%//failures.tsv}" 2>/dev/null)"

run=""
if [ -n "${GITHUB_RUN_ID:-}" ]; then
    run="[run ${GITHUB_RUN_ID}](${GITHUB_SERVER_URL:-https://github.com}/${GITHUB_REPOSITORY:-}/actions/runs/${GITHUB_RUN_ID})"
fi

{
    echo "## Soak: flaky checks"
    echo
    echo "The nightly soak (\`.github/workflows/soak.yml\`) runs both apps' GUI suites several times, each round in a different seeded order, with no retries. This body is replaced on every run. Last: ${run:-a run outside Actions}, $(date -u '+%Y-%m-%d %H:%M UTC')${GITHUB_SHA:+, commit \`${GITHUB_SHA:0:9}\`}."
    echo
    for app in macOS GTK4; do
        line="$(printf '%s\n' "$rounds" | awk -F'\t' -v app="$app" '
            $1 == app { n++; seeds = seeds (n > 1 ? ", " : "") $3; if ($4 != "-") perturb = perturb (n > 1 ? ", " : "") $4 }
            END {
                if (!n) { print "no rounds ran (the job stopped before its first; see the run)"; exit }
                printf "%d round(s), seeds %s", n, seeds
                if (perturb != "") printf " (MALLOC_PERTURB_ %s)", perturb
                print ""
            }')"
        echo "- **$app**: $line"
    done
    echo
    if [ -z "$failures" ]; then
        echo "No suite failed in any round."
    else
        echo "| App | Suite | Failed in | Seeds of those rounds | First failure |"
        echo "|---|---|---|---|---|"
        # Rounds per app, then each suite's failed rounds, most first.
        printf '%s\n' "$rounds" | awk -F'\t' 'NF >= 3 { n[$1]++ } END { for (a in n) print "R\t" a "\t" n[a] }' |
            cat - <(printf '%s\n' "$failures") | awk -F'\t' '
                $1 == "R" { total[$2] = $3; next }
                NF >= 5 {
                    k = $1 "\t" $4
                    if (!(k in count)) { order[++m] = k; first[k] = $5 }
                    count[k]++
                    seeds[k] = seeds[k] (count[k] > 1 ? ", " : "") $3
                }
                END {
                    for (i = 1; i <= m; i++) {
                        k = order[i]; split(k, p, "\t")
                        line = first[k]; gsub(/\|/, "\\|", line); gsub(/`/, "'"'"'", line)
                        if (length(line) > 200) line = substr(line, 1, 200) "..."
                        printf "%d\t| %s | %s | %d of %d | %s | `%s` |\n", count[k], p[1], p[2], count[k], total[p[1]], seeds[k], line
                    }
                }' | sort -t "$(printf '\t')" -k1,1 -rn -s | cut -f2-
    fi
    echo
    echo "To play a round's order again: the same suites with \`MC_GUI_SHUFFLE=<seed>\` (on GTK also its \`MALLOC_PERTURB_\`); \`scripts/soak.sh\` runs a round as CI does. Every suite here needs a fix: a FLAKY line in CI is the same failure seen once (\`docs/ci.md\`, \"Retries\")."
} >"$outFile"
