#!/bin/bash
# rename-mutible.sh — the mechanical half of renaming minicode to mutible
# (1.6.0): renames tracked files whose path has the old name, and replaces
# the name in every tracked text file (and inside gzipped ones). Idempotent: run again, it changes
# nothing.
#
#   scripts/rename-mutible.sh           make the rename in the working tree
#   scripts/rename-mutible.sh --check   list what still says minicode (the
#                                       places left alone on purpose are not
#                                       listed); changes nothing
#
# The rename is three parts on the rename-mutible branch:
#   1. hand-written commits (src/Rename.h and what reads the old names, the
#      first launch's migration, the `minicode` alias, the casks and
#      release.sh, and this script),
#   2. one commit made by this script alone ("Rename: minicode to mutible,
#      by scripts/rename-mutible.sh"),
#   3. the commits written by hand on top of the script's output: the docs,
#      and the new icon.
# To redo it on a newer main: rebase the commits of part 1 onto it, dropping
# 2 and 3; run this script; commit everything it changed as part 2;
# cherry-pick the commits of part 3; then make, make test, and --check.
#
# What is replaced, in this order, on every line not marked to keep:
#   the GitHub repo and the tap   e-c-hansen/minicode, e-c-hansen/tap/minicode
#                                 and e-c-hansen/homebrew-tap -> ible-ai/...
#   the bundle id and other ids   com.minicode.editor -> ai.ible.mutible,
#                                 org.minicode.x and com.minicode.x ->
#                                 ai.ible.mutible.x
#   the name                      MiniCode -> Mutible, MINICODE -> MUTIBLE,
#                                 Minicode -> Mutible, minicode -> mutible
#
# What is left alone:
#   - files: linux/, debian/ and android/ (the Linux session renames the
#     first two; Android is frozen), .grades/, this script, src/Rename.h and
#     tests/rename_tests.inc (the old names on purpose), the old cask
#     (packaging/minicode.rb), and the PPA's scripts, notes and checks
#     (ppa.sh, ppa-wait.sh, packaging/PPA.md, packaging/ppa/,
#     tests/ppa_wait_tests.inc), and the Linux-only BUILD-LINUX.md,
#     scripts/membench-linux.sh, tests/latex/sweep-linux.sh and
#     docs/notes/android.md;
#   - lines with "rename-mutible: keep", and lines between
#     "rename-mutible: keep-begin" and "rename-mutible: keep-end" (the
#     compatibility code's old names);
#   - the names in PROTECT below: the signing certificate "MiniCode Local
#     Signing", the Android release key and its keystore, the APK's name,
#     the PPA and its variables, local checkout paths (Code/public/minicode),
#     and the Linux and Android ports' own names and paths until those
#     ports move.
#
# The few edits in linux/ that the core's rename forces (files the Linux
# build takes from the core's folders, the CLI's entry points, the command
# ids and LaTeX scratch names it shares with the core, and its checks of
# what the core now says) are made by linux_fixes below, and nothing else
# there.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

CHECK=""
[ "${1:-}" = "--check" ] && CHECK=1

EXCLUDE='^(\.grades/|linux/|debian/|android/|scripts/rename-mutible\.sh$|src/Rename\.h$|tests/rename_tests\.inc$|packaging/minicode\.rb$|scripts/ppa\.sh$|scripts/ppa-wait\.sh$|scripts/membench-linux\.sh$|packaging/PPA\.md$|packaging/ppa/|tests/ppa_wait_tests\.inc$|BUILD-LINUX\.md$|tests/latex/sweep-linux\.sh$|docs/notes/android\.md$)'

# The replacement, as a perl program over lines; RENAME_MODE=check prints
# the lines that would still say minicode instead.
read -r -d '' PERL <<'EOF' || true
my @PROTECT = (
    qr/MiniCode Local Signing/,
    qr/Code[\/-]public[\/-]minicode/,                      # local checkouts
    qr/echansen\/minicode/, qr/ubuntu\/minicode/,           # the PPA
    qr/MINICODE_(?:NO_)?PPA\w*/,
    qr/MINICODE_KEYSTORE\w*/, qr/minicode-android-keystore/, qr/minicodeVersion/,
    qr/-a minicode -s/, qr/-alias minicode/, qr/CN=minicode/,
    qr/MiniCode-[^\s"'`)]*\.apk/,                           # Android's APK
    qr/\.config\/minicode\/(?:release\.keystore|ppa-key[\w.-]*)/,
    qr/(?:linux|debian|android)\/[\w.\/-]*minicode[\w.\/-]*/,
    qr/org\.minicode\.Editor/, qr/org\.minicode\.editor(?!\.line-copy)/,
    qr/org\/minicode\/editor/, qr/minicode_jni/,
    qr/minicode-(?:guitest|linux-tests|latex-sweep|gtk)\b/,
    qr/\.local\/(?:share|state)\/minicode/, qr/\.cache\/minicode/,
    qr/XDG_(?:STATE|DATA|CACHE)_HOME\/minicode/, qr/\/usr\/bin\/minicode/,
);
my $mode = $ENV{RENAME_MODE} // 'rename';
my $keep = 0;
while (my $line = <>) {
    if ($line =~ /rename-mutible: keep-begin/) { $keep = 1; print $line if $mode eq 'rename'; next; }
    if ($line =~ /rename-mutible: keep-end/)   { $keep = 0; print $line if $mode eq 'rename'; next; }
    if ($keep || $line =~ /rename-mutible: keep/) { print $line if $mode eq 'rename'; next; }
    # Renamed in linux/ too (linux_fixes), so said with the new name.
    $line =~ s{linux/resources/minicode-bash-integration\.bash}{linux/resources/mutible-bash-integration.bash}g;
    my @saved;
    for my $re (@PROTECT) {
        $line =~ s/($re)/push @saved, $1; "\x{1}" . $#saved . "\x{2}"/ge;
    }
    if ($mode eq 'check') {
        print "$ARGV:$.: $line" if $line =~ /minicode/i;
        next;
    }
    $line =~ s{e-c-hansen/homebrew-tap}{ible-ai/homebrew-tap}g;
    $line =~ s{e-c-hansen/tap/minicode}{ible-ai/tap/mutible}g;
    $line =~ s{e-c-hansen/minicode}{ible-ai/mutible}g;
    $line =~ s{com\.minicode\.editor}{ai.ible.mutible}g;
    $line =~ s{org\.minicode\.editor\.}{ai.ible.mutible.}g;
    $line =~ s{(?:com|org)\.minicode\.}{ai.ible.mutible.}g;
    $line =~ s/MiniCode/Mutible/g;
    $line =~ s/MINICODE/MUTIBLE/g;
    $line =~ s/Minicode/Mutible/g;
    $line =~ s/minicode/mutible/g;
    $line =~ s/\x{1}(\d+)\x{2}/$saved[$1]/g;
    print $line;
} continue {
    close ARGV if eof;   # $. counts each file's lines
}
EOF

# The tracked text files outside EXCLUDE that mention the name.
files_with_name() {
    git grep -I -l -i minicode -- . | grep -Ev "$EXCLUDE" || true
}

# The tracked .gz files outside EXCLUDE whose text mentions the name.
gz_with_name() {
    git ls-files -- '*.gz' | grep -Ev "$EXCLUDE" | while IFS= read -r f; do
        # grep -c reads to the end: grep -q would stop early, and gzip's
        # SIGPIPE would fail the pipe (pipefail) and hide the file.
        if gzip -dc "$f" 2>/dev/null | grep -aic minicode >/dev/null; then echo "$f"; fi
    done
}

if [ -n "$CHECK" ]; then
    files_with_name | while IFS= read -r f; do
        RENAME_MODE=check perl -e "$PERL" "$f"
    done
    gz_with_name | while IFS= read -r f; do
        gzip -dc "$f" | RENAME_MODE=check perl -e "$PERL" | sed "s|^-:|$f:|"
    done
    git ls-files | grep -Ev "$EXCLUDE" | grep -i minicode |
        grep -Ev 'Code-public-minicode' | sed 's/^/path: /' || true
    exit 0
fi

# 1. File names: each tracked path outside EXCLUDE with the name in it, by
#    the same rules as the text (so a protected name keeps its file).
git ls-files | grep -Ev "$EXCLUDE" | grep -i minicode | while IFS= read -r path; do
    new="$(printf '%s\n' "$path" | RENAME_MODE=rename perl -e "$PERL")"
    [ "$new" = "$path" ] && continue
    [ -e "$new" ] && { echo "rename-mutible: $new exists already; $path left" >&2; continue; }
    mkdir -p "$(dirname "$new")"
    git mv "$path" "$new"
done

# 2. Text: every tracked text file outside EXCLUDE.
files_with_name | while IFS= read -r f; do
    RENAME_MODE=rename perl -i -e "$PERL" "$f"
done

# 2b. Gzipped files (the LaTeX sweep's SyncTeX fixture names the preview's
#     scratch file; the terminal fixture has the generator's marker): the
#     same replacement inside, recompressed with no name
#     or time in the header, so a second run writes the same bytes.
gz_with_name | while IFS= read -r f; do
    gzip -dc "$f" | RENAME_MODE=rename perl -e "$PERL" | gzip -n -9 > "$f.rename-tmp"
    mv "$f.rename-tmp" "$f"
done

# 3. The edits in linux/ that the core's rename forces: the Linux build takes
#    the zsh integration, the CLI and the skill from the core's folders, its
#    bash integration's name is the core's (ShellIntegration.h), and it calls
#    the CLI's entry points by name. Nothing else in linux/ changes.
linux_fixes() {
    local old=linux/resources/minicode-bash-integration.bash
    local new=linux/resources/mutible-bash-integration.bash
    if [ -e "$old" ] && [ ! -e "$new" ]; then git mv "$old" "$new"; fi
    for f in linux/meson.build linux/Makefile; do
        [ -f "$f" ] || continue
        perl -pi -e '
            s/minicode-zsh-integration\.zsh/mutible-zsh-integration.zsh/g;
            s/minicode-bash-integration\.bash/mutible-bash-integration.bash/g;
            s{tools/minicode-cli\.cpp}{tools/mutible-cli.cpp}g;
            s{\x27tools\x27 / \x27minicode-cli\.cpp\x27}{\x27tools\x27 / \x27mutible-cli.cpp\x27}g;
            s{\x27skills\x27 / \x27minicode\x27 / \x27SKILL\.md\x27}{\x27skills\x27 / \x27mutible\x27 / \x27SKILL.md\x27}g;
        ' "$f"
    done
    [ -f linux/src/main.cpp ] &&
        perl -pi -e 's/\bminicodeCliMain\b/mutibleCliMain/g; s/\bminicodeInstallSkill\b/mutibleInstallSkill/g' \
            linux/src/main.cpp
    # Names the Linux app shares with the core, so they move with it: the
    # command ids of the core's tables, the LaTeX preview's scratch files
    # (LatexScratch.h), and, in its checks, what the core's CLI, IDE link
    # and hooks now say.
    git ls-files -- 'linux/src/*.cpp' 'linux/src/*.h' 'linux/tests/*.cpp' 'linux/tests/*.h' |
        xargs grep -l -E '"minicode\.|\.minicode\.tex' 2>/dev/null |
        xargs -r perl -pi -e 's/"minicode\./"mutible./g; s/\.minicode\.tex/.mutible.tex/g'
    local t=linux/tests/gui
    [ -f $t/ControlSuite.cpp ] && perl -pi -e \
        's{skills/minicode}{skills/mutible}g; s/Installed the minicode skill/Installed the mutible skill/g' \
        $t/ControlSuite.cpp
    [ -f $t/IdeLinkSuite.cpp ] && perl -pi -e \
        's/info\.ideName == "minicode"/info.ideName == "mutible"/g; s/asString\(\), "minicode"\)/asString(), "mutible")/g' \
        $t/IdeLinkSuite.cpp
    [ -f $t/AttentionSuite.cpp ] && perl -pi -e 's/minicode notify --from-hook/mutible notify --from-hook/g' \
        $t/AttentionSuite.cpp
    git ls-files -- "$t/*.cpp" | xargs grep -l -E 'until minicode quits|# minicode keyboard shortcuts' 2>/dev/null |
        xargs -r perl -pi -e 's/until minicode quits/until mutible quits/g; s/# minicode keyboard shortcuts/# mutible keyboard shortcuts/g'
    return 0
}
linux_fixes

echo "rename-mutible: done; scripts/rename-mutible.sh --check lists what still says minicode"
