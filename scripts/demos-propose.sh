#!/bin/bash
# Proposes the GIFs a Demos workflow run made (.github/workflows/demos.yml):
#
#   scripts/demos-propose.sh ARTIFACTS
#
# ARTIFACTS is where actions/download-artifact put the run's artifacts
# (demos-mac with docs/demos/*.gif, demos-linux with docs/demos/linux/*.gif).
# The checkout moves to origin/main, the GIFs go over the committed ones,
# and when any differ they are committed on a branch of their own. With
# PROPOSE=true the branch is pushed and a pull request against main opened
# (or, where Actions may not open one, a link printed to open it by hand),
# with a run of CI started on the branch by hand, since the workflow's own
# token starts no workflow and main requires "CI passed";
# otherwise the commit stays local. WHAT names the release or ref in the
# titles. Only GIFs are taken: the stills and recordings stay artifacts.
set -euo pipefail

artifacts="$1"
what="${WHAT:-manual}"

git fetch -q origin main
git checkout -q -B demos-work origin/main

mkdir -p docs/demos/linux
shopt -s nullglob
for f in "$artifacts"/demos-mac/*.gif; do cp "$f" docs/demos/; done
for f in "$artifacts"/demos-linux/*.gif; do cp "$f" docs/demos/linux/; done

git add docs/demos
if git diff --cached --quiet; then
    echo "The GIFs are as committed; nothing to propose."
    exit 0
fi
git diff --cached --stat

run="${GITHUB_SERVER_URL:-https://github.com}/${GITHUB_REPOSITORY:-e-c-hansen/minicode}/actions/runs/${GITHUB_RUN_ID:-0}"
git -c user.name="github-actions[bot]" \
    -c user.email="41898282+github-actions[bot]@users.noreply.github.com" \
    commit -q -m "Demos: record the GIFs again for $what" -m "Made by the Demos workflow, $run."

if [ "${PROPOSE:-}" != "true" ]; then
    echo "Not proposing (a push to a branch): the commit stays here."
    exit 0
fi

branch="demos/${what//\//-}-${GITHUB_RUN_ID:-0}"
git push -q origin "HEAD:refs/heads/$branch"
# A push or pull request made with the workflow's own token starts no
# workflow, so the pull request would never get "CI passed", which main
# requires. A run by hand is the exception GitHub allows, and its checks
# land on the branch's commit, so the pull request gets them.
if ! gh workflow run ci.yml --ref "$branch"; then
    echo "Could not start CI on $branch; run it from the Actions tab before merging."
fi
body="The demo GIFs recorded again by the Demos workflow for $what ($run).

Look at each before merging. A scene that failed kept its old GIF, and the run's log says which. The Linux GIFs are in docs/demos/linux."
if ! gh pr create --base main --head "$branch" \
        --title "Demos: record the GIFs again for $what" --body "$body"; then
    echo "Actions may not open pull requests in this repository. Open one from"
    echo "${GITHUB_SERVER_URL:-https://github.com}/${GITHUB_REPOSITORY:-e-c-hansen/minicode}/compare/main...$branch"
fi
