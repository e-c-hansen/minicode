#!/bin/sh
# Typeset a .tex file the way the preview does, then double-click every word
# of the result through the real click path and report the hit rate.
#
#   tests/latex/sweep.sh path/to/doc.tex [-v]
#
# Needs tectonic: MINICODE_TECTONIC, else the copy the app downloads. Set
# TECTONIC_CACHE_DIR to a scratch copy of the cache to keep the run off the
# network and out of your real cache. Writes a hidden sibling of the document
# (removed afterwards), exactly as the app does, and nothing else beside it.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
tex=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
shift
tectonic=${MINICODE_TECTONIC:-"$HOME/Library/Application Support/MiniCode/bin/tectonic"}
out=${TMPDIR:-/tmp}/minicode-sweep
mkdir -p "$out" "$root/build"

# The harness links the app's own sources, minus main.mm.
srcs=$(ls "$root"/src/*.mm "$root"/src/*.cpp | grep -v '/main\.mm$')
clang++ -std=c++17 -fobjc-arc -Wall -Wextra -O1 -I"$root/src" \
    "$here/sweep.mm" $srcs \
    -framework Cocoa -framework WebKit -framework CoreServices -framework Quartz -lz \
    -o "$root/build/latex-sweep"

dir=$(dirname "$tex")
name=$(basename "$tex" .tex)
scratch="$dir/.$name.minicode.tex"
cp "$tex" "$scratch"
trap 'rm -f "$scratch"' EXIT
(cd "$dir" && "$tectonic" --synctex --chatter minimal --color never \
    --outdir "$out" "$scratch" >"$out/log.txt" 2>&1) || {
    cat "$out/log.txt"; exit 1; }
"$root/build/latex-sweep" "$tex" "$scratch" "$out/.$name.minicode.pdf" \
    "$out/.$name.minicode.synctex.gz" "$@"
