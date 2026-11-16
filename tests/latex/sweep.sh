#!/bin/sh
# Double-click every word of a typeset LaTeX document through the preview's
# real click path and report the hit rate.
#
#   tests/latex/sweep.sh path/to/doc.tex [-v]
#   tests/latex/sweep.sh --fixture [-v]
#
# The first typesets the document the way the preview does, which needs
# tectonic: MINICODE_TECTONIC, else the copy the app downloads. Set
# TECTONIC_CACHE_DIR to a scratch cache to keep the run out of your real
# one. It writes a hidden sibling of the document (removed afterwards), as
# the app does, and nothing else beside it.
#
# --fixture needs no tectonic: it clicks the committed typeset of
# tests/latex/torture.tex (tests/latex/fixtures/torture.pdf and
# torture.synctex.gz, written by tectonic 0.17.0) and fails on any wrong
# answer or a hit rate below the bound in sweep_bound.txt, the last run's
# (CI runs it; `make latex-sweep` is the same). After a change to the
# matcher that raises the rate, raise the bound with it. It then clicks the
# committed three-file fixture (fixtures/main.tex with its \input files,
# main.pdf and main.synctex, whose paths are "@DIR@/..." and are found
# beside main.tex) against sweep_bound_inputs.txt the same way.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)

# The harness links the app's own objects, minus main.mm.
make -C "$root" build/latex-sweep >/dev/null

if [ "$1" = "--fixture" ]; then
    shift
    fx="$here/fixtures"
    "$root/build/latex-sweep" "$here/torture.tex" "/fixture/.torture.minicode.tex" \
        "$fx/torture.pdf" "$fx/torture.synctex.gz" --bound "$(cat "$here/sweep_bound.txt")" "$@"
    # A document of three files (fixtures/main.tex reads chapter.tex and
    # appendix.tex): clicks on the \input files' text edit those files.
    exec "$root/build/latex-sweep" "$fx/main.tex" "@DIR@/.main.minicode.tex" \
        "$fx/main.pdf" "$fx/main.synctex" --bound "$(cat "$here/sweep_bound_inputs.txt")" "$@"
fi

tex=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
shift
tectonic=${MINICODE_TECTONIC:-"$HOME/Library/Application Support/MiniCode/bin/tectonic"}
out=${TMPDIR:-/tmp}/minicode-sweep
mkdir -p "$out"

dir=$(dirname "$tex")
name=$(basename "$tex" .tex)
scratch="$dir/.$name.minicode.tex"
cp "$tex" "$scratch"
trap 'rm -f "$scratch"' EXIT
(cd "$dir" && "$tectonic" --synctex --untrusted --chatter minimal --color never \
    --outdir "$out" "$scratch" >"$out/log.txt" 2>&1) || {
    cat "$out/log.txt"; exit 1; }
"$root/build/latex-sweep" "$tex" "$scratch" "$out/.$name.minicode.pdf" \
    "$out/.$name.minicode.synctex.gz" "$@"
