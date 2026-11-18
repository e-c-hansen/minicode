#!/bin/sh
# The Linux twin of sweep.sh: typeset a .tex file the way the GTK preview
# does, then double-click every word of the result through the preview's own
# click path (linux/src/PageWords.cpp over poppler) and report the hit rate.
#
#   tests/latex/sweep-linux.sh path/to/doc.tex [-v]
#
# Needs the harness built (`meson compile -C linux/build`, with poppler) and
# tectonic: MUTIBLE_TECTONIC, else the copy the app downloads
# (~/.local/share/mutible/bin/tectonic), else PATH. Set TECTONIC_CACHE_DIR to
# a scratch copy of the cache to keep the run off the network and out of your
# real cache. Writes a hidden sibling of the document (removed afterwards),
# exactly as the app does, and nothing else beside it. Exits non-zero when any
# click opened the wrong text.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
tex=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
shift
sweep=${MUTIBLE_SWEEP_BIN:-"$root/linux/build/mutible-latex-sweep"}
[ -x "$sweep" ] || { echo "build the harness first: meson compile -C linux/build" >&2; exit 2; }

tectonic=${MUTIBLE_TECTONIC:-}
if [ -z "$tectonic" ]; then
    data=${XDG_DATA_HOME:-"$HOME/.local/share"}
    if [ -x "$data/mutible/bin/tectonic" ]; then tectonic="$data/mutible/bin/tectonic"
    else tectonic=$(command -v tectonic) || { echo "tectonic not found" >&2; exit 2; }
    fi
fi

out=$(mktemp -d "${TMPDIR:-/tmp}/mutible-sweep.XXXXXX")
dir=$(dirname "$tex")
name=$(basename "$tex" .tex)
scratch="$dir/.$name.mutible.tex"
cp "$tex" "$scratch"
trap 'rm -f "$scratch"; rm -rf "$out"' EXIT
(cd "$dir" && "$tectonic" --synctex --chatter minimal --color never \
    --outdir "$out" "$scratch" >"$out/log.txt" 2>&1) || {
    cat "$out/log.txt"; exit 1; }
"$sweep" "$tex" "$scratch" "$out/.$name.mutible.pdf" \
    "$out/.$name.mutible.synctex.gz" "$@"
