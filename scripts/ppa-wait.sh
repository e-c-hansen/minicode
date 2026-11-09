#!/bin/bash
# Wait for Launchpad to build an upload to the PPA, and fail loudly unless it
# built.
#   scripts/ppa-wait.sh 1.5.15        waits for 1.5.15-1~ppa1~resolute1
#   scripts/ppa-wait.sh 1.5.15 2      the second upload of 1.5.15, ~ppa2
#
# ppa.sh runs this after every real upload (and release.sh through it), so a
# release only says the PPA is done once Launchpad has built the package; it
# also runs alone, to wait again after giving up or to look at an old upload.
# The 1.5.13 upload failed to build and nobody noticed for a release, since
# both scripts stopped at dput.
#
# It reads the PPA's public build records (no login, read only):
#   https://api.launchpad.net/1.0/~OWNER/+archive/ubuntu/NAME
#       ?ws.op=getBuildRecords&source_name=minicode
# and waits until every build of this exact version has ended. Exit status:
#   0  every build of it is "Successfully built"
#   1  a build ended any other way ("Failed to build", "Dependency wait",
#      "Chroot problem", "Failed to upload", "Cancelled build", ...); its
#      state and build log's address are printed
#   2  usage
#   3  gave up: no build record within MINICODE_PPA_ACCEPT_MINUTES of
#      starting (Launchpad may have rejected the upload; it mails the reason
#      to the signer), the build not over within MINICODE_PPA_WAIT_MINUTES,
#      or Launchpad unreadable ten times in a row
#
#   MINICODE_PPA_SERIES          as ppa.sh (default resolute)
#   MINICODE_PPA                 as ppa.sh (default ppa:echansen/minicode)
#   MINICODE_PPA_POLL_SECONDS    between looks (default 60)
#   MINICODE_PPA_ACCEPT_MINUTES  for the build record to appear (default 30)
#   MINICODE_PPA_WAIT_MINUTES    for the build to end (default 240; a build
#                                takes about 35 minutes, and the queue varies)
#   MINICODE_PPA_ANSWERS         for the tests (tests/ppa_wait_tests.inc): a
#                                folder of canned answers, 1.json for the
#                                first look, 2.json for the second, and so
#                                on, the last one repeating; N.fail stands
#                                for a look that could not read Launchpad.
#                                Time then passes by the poll interval per
#                                look without sleeping.
set -euo pipefail

VERSION="${1:-}"
REV="${2:-1}"
if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || ! [[ "$REV" =~ ^[1-9][0-9]*$ ]]; then
    echo "usage: scripts/ppa-wait.sh <version> [ppa revision]   e.g. 1.5.15" >&2
    exit 2
fi
SERIES="${MINICODE_PPA_SERIES:-resolute}"
PPA="${MINICODE_PPA:-ppa:echansen/minicode}"
if ! [[ "$PPA" =~ ^ppa:([^/]+)/([^/]+)$ ]]; then
    echo "error: MINICODE_PPA must look like ppa:owner/name, got '$PPA'" >&2; exit 2
fi
PPA_OWNER="${BASH_REMATCH[1]}"
PPA_NAME="${BASH_REMATCH[2]}"
# As ppa.sh numbers it (packaging/PPA.md, "Version numbers").
PKGVER="$VERSION-1~ppa$REV~${SERIES}1"
for n in MINICODE_PPA_POLL_SECONDS MINICODE_PPA_ACCEPT_MINUTES MINICODE_PPA_WAIT_MINUTES; do
    if [ -n "${!n:-}" ] && ! [[ "${!n}" =~ ^[0-9]+$ ]]; then
        echo "error: $n must be a whole number, got '${!n}'" >&2; exit 2
    fi
done

AGAIN="scripts/ppa-wait.sh $VERSION"
[ "$REV" = 1 ] || AGAIN="$AGAIN $REV"

exec python3 -I - "$PKGVER" "$PPA_OWNER" "$PPA_NAME" "$AGAIN" <<'PY'
import json, os, subprocess, sys, time

pkgver, owner, name, again = sys.argv[1:5]
poll = int(os.environ.get("MINICODE_PPA_POLL_SECONDS") or 60)
accept_limit = 60 * int(os.environ.get("MINICODE_PPA_ACCEPT_MINUTES") or 30)
wait_limit = 60 * int(os.environ.get("MINICODE_PPA_WAIT_MINUTES") or 240)
answers = os.environ.get("MINICODE_PPA_ANSWERS") or ""
page = f"https://launchpad.net/~{owner}/+archive/ubuntu/{name}/+packages"
url = (f"https://api.launchpad.net/1.0/~{owner}/+archive/ubuntu/{name}"
       "?ws.op=getBuildRecords&source_name=minicode&ws.size=50")

# Launchpad's BuildStatus titles for a build that has not ended. Every other
# state is an end, and only "Successfully built" a good one, so a state this
# list lacks fails loudly rather than being waited on for hours.
PENDING = {"Needs building", "Currently building", "Gathering build output",
           "Uploading build", "Cancelling build"}
BUILT = "Successfully built"

def say(text):
    print(text, flush=True)

looks = 0
clock = 0.0          # seconds since the start: real, or by look with answers
start = time.monotonic()

def fetch():
    """The build records' entries, or None when Launchpad could not be read."""
    global looks
    looks += 1
    if answers:
        files = sorted((f for f in os.listdir(answers) if f.split(".")[0].isdigit()),
                       key=lambda f: int(f.split(".")[0]))
        if not files:
            return None
        chosen = files[-1]
        for f in files:
            if int(f.split(".")[0]) == looks:
                chosen = f
        if chosen.endswith(".fail"):
            return None
        with open(os.path.join(answers, chosen), "rb") as f:
            body = f.read()
    else:
        r = subprocess.run(["curl", "-fsS", "--max-time", "60", url],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if r.returncode != 0:
            say("    could not read Launchpad: " +
                (r.stderr.decode("utf-8", "replace").strip() or f"curl exit {r.returncode}"))
            return None
        body = r.stdout
    try:
        entries = json.loads(body)["entries"]
        return [e for e in entries if isinstance(e, dict)]
    except (ValueError, KeyError, TypeError):
        say("    Launchpad's answer was not the build records")
        return None

def wait():
    global clock
    if answers:
        clock += poll
    else:
        time.sleep(poll)
        clock = time.monotonic() - start

def minutes(seconds):
    return f"{int(seconds // 60)} min"

say(f"==> Waiting for Launchpad to build minicode {pkgver}")
say(f"    {page}")
say(f"    (Ctrl+C stops the wait, not the build; {again} waits again)")
failures = 0
last = {}
while True:
    if answers and looks >= 100000:   # a test whose limits never come
        say("error: 100,000 canned looks without an end")
        sys.exit(3)
    entries = fetch()
    if entries is None:
        failures += 1
        if failures >= 10:
            say(f"error: could not read Launchpad ten times in a row; how the build of {pkgver}")
            say(f"       went is unknown. Look at {page}")
            sys.exit(3)
    else:
        failures = 0
        mine = [e for e in entries
                if e.get("source_package_name") == "minicode"
                and e.get("source_package_version") == pkgver]
        states = {e.get("arch_tag") or "?": e.get("buildstate") or "?" for e in mine}
        for arch, state in sorted(states.items()):
            if last.get(arch) != state:
                say(f"    {minutes(clock):>7}  {arch}: {state}")
        last = states
        if mine and all(s not in PENDING for s in states.values()):
            bad = [e for e in mine if e.get("buildstate") != BUILT]
            if not bad:
                say(f"==> Launchpad built {pkgver} ({', '.join(sorted(states))}).")
                sys.exit(0)
            for e in bad:
                say(f"error: Launchpad's {e.get('arch_tag')} build of {pkgver} ended as "
                    f"\"{e.get('buildstate')}\", not \"{BUILT}\"")
                if e.get("dependencies"):
                    say(f"       waiting for: {e['dependencies']}")
                for key in ("build_log_url", "web_link"):
                    if e.get(key):
                        say(f"       {e[key]}")
            sys.exit(1)
        if not mine and clock >= accept_limit:
            say(f"error: Launchpad has no build of {pkgver} after {minutes(clock)}. It may have")
            say("       rejected the upload (it mails the reason to the key's address);")
            say(f"       {page}")
            sys.exit(3)
        if mine and clock >= wait_limit:
            say(f"error: the build of {pkgver} has not ended after {minutes(clock)}; stopped waiting.")
            say(f"       Wait again with {again}, or look at {page}")
            sys.exit(3)
    wait()
PY
