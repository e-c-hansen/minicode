#!/bin/bash
# mutible-desktop-setup.sh — turn a fresh minimal Ubuntu 26.04 into the
# mutible desktop: the machine boots straight into mutible, full screen, as
# the whole desktop, under cage (a Wayland kiosk compositor), with no
# desktop environment. linux/desktop/README.md says what it is and how to
# use it.
#
# Run it on the machine that is to become the desktop, with sudo:
#
#   sudo ./mutible-desktop-setup.sh [--user NAME] [--deb FILE] [--start]
#   sudo ./mutible-desktop-setup.sh --undo [--remove-packages] [--now]
#   ./mutible-desktop-setup.sh --dry-run ...    (prints, changes nothing)
#
# What it changes, every step printed as it goes:
#   1. adds the mutible PPA (ppa:echansen/minicode, the PPA's old name) as
#      /etc/apt/sources.list.d/mutible-desktop-ppa.sources, its signing key
#      inside the file, unless --deb is given or the PPA is there already;
#   2. installs what is missing of cage, mutible (from the PPA, or the .deb
#      given with --deb), NetworkManager, PipeWire with WirePlumber,
#      brightnessctl, polkitd and a font, noting every package that was not
#      installed before, so --undo can list them;
#   3. writes the session: /usr/local/lib/mutible-desktop/session (runs
#      `cage -s -- mutible --foreground` with MUTIBLE_DESKTOP=1),
#      /etc/pam.d/mutible-desktop, /etc/default/mutible-desktop (yours to
#      edit; kept when it differs) and /etc/systemd/system/
#      mutible-desktop.service, which logs NAME in on tty1 in place of its
#      getty and starts the session again whenever it ends, 2 s later (a
#      session that ends within 30 s of starting three times running, a
#      crash loop, leaves a text shell on tty1 instead);
#   4. disables getty@tty1.service (noting that it was enabled) and enables
#      mutible-desktop.service. Nothing starts until the next boot, or now
#      with --start (refused from tty1 itself, whose login it would end).
# Running it again changes only what differs. --undo removes those files,
# enables getty@tty1 again if it was, removes the PPA file it added and
# lists the packages it installed, with the command that removes them
# (--remove-packages runs it, apt asking first).
#
# Its record is /var/lib/mutible-desktop/state.
set -euo pipefail

STATE_DIR=/var/lib/mutible-desktop
STATE=$STATE_DIR/state
LIB=/usr/local/lib/mutible-desktop
SESSION=$LIB/session
PAM=/etc/pam.d/mutible-desktop
DEFAULTS=/etc/default/mutible-desktop
UNIT=/etc/systemd/system/mutible-desktop.service
UNIT_NAME=mutible-desktop.service
PPA_FILE=/etc/apt/sources.list.d/mutible-desktop-ppa.sources
PPA_URL=https://ppa.launchpadcontent.net/echansen/minicode/ubuntu
PPA_FINGERPRINT=4EC31F514321C71F5B47A5218919EF5B37EEFF40

# What the desktop needs besides cage and mutible. Recommends are left out
# (a lean machine); git and curl are mutible's own recommends worth having.
PACKAGES=(
    cage                      # the compositor: one app, full screen
    network-manager           # Wi-Fi: nmcli and nmtui in mutible's terminal
    wpasupplicant
    wireless-regdb
    polkitd                   # lets the seat's user suspend and power off
    libpam-systemd            # the login session on seat0 (logind)
    dbus-user-session         # the session bus mutible and PipeWire use
    pipewire                  # sound: wpctl
    pipewire-pulse
    pipewire-alsa
    wireplumber
    brightnessctl             # the screen's brightness
    fonts-dejavu-core         # a font for the editor and the terminal
    git
    curl
)

USER_NAME=""
DEB=""
UNDO=0
DRY=0
START=0
NOW=0
REMOVE_PACKAGES=0

say() { printf '==> %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die() { printf 'mutible-desktop-setup: %s\n' "$*" >&2; exit 1; }

usage() {
    sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

# A command that changes the machine: printed, then run (or only printed
# with --dry-run).
run() {
    printf '    $ %s\n' "$*"
    [ "$DRY" = 1 ] || "$@"
}

state_get() {
    [ -r "$STATE" ] && sed -n "s/^$1=//p" "$STATE" | tail -n 1 || true
}
state_set() {
    [ "$DRY" = 1 ] && return 0
    mkdir -p "$STATE_DIR"
    touch "$STATE"
    grep -v "^$1=" "$STATE" > "$STATE.new" || true
    printf '%s=%s\n' "$1" "$2" >> "$STATE.new"
    mv "$STATE.new" "$STATE"
}

# put FILE MODE: the text on stdin becomes FILE, written only when it
# differs, saying which.
put() {
    local file=$1 mode=$2 tmp
    tmp=$(mktemp)
    cat > "$tmp"
    if [ -f "$file" ] && cmp -s "$tmp" "$file"; then
        note "unchanged: $file"
        rm -f "$tmp"
        return 0
    fi
    if [ -f "$file" ]; then note "updating: $file"; else note "writing: $file"; fi
    if [ "$DRY" = 1 ]; then
        sed 's/^/      | /' "$tmp"
        rm -f "$tmp"
        return 0
    fi
    mkdir -p "$(dirname "$file")"
    install -m "$mode" "$tmp" "$file"
    rm -f "$tmp"
}

installed() {
    [ "$(dpkg-query -W -f='${db:Status-Status}' "$1" 2>/dev/null || true)" = installed ]
}

while [ $# -gt 0 ]; do
    case "$1" in
        --user) USER_NAME=${2:?--user needs a name}; shift 2 ;;
        --user=*) USER_NAME=${1#--user=}; shift ;;
        --deb) DEB=${2:?--deb needs a file}; shift 2 ;;
        --deb=*) DEB=${1#--deb=}; shift ;;
        --undo) UNDO=1; shift ;;
        --dry-run|-n) DRY=1; shift ;;
        --start) START=1; shift ;;
        --now) NOW=1; shift ;;
        --remove-packages) REMOVE_PACKAGES=1; shift ;;
        -h|--help) usage 0 ;;
        *) printf 'unknown option: %s\n\n' "$1" >&2; usage 2 ;;
    esac
done

if [ "$DRY" = 1 ]; then
    say "dry run: nothing is changed, every change is printed"
elif [ "$(id -u)" != 0 ]; then
    die "run it with sudo (or --dry-run to see what it would do)"
fi

# Whether this script runs inside the desktop session itself (its terminal):
# stopping the service would end this very shell.
in_session() {
    grep -q "/$UNIT_NAME" /proc/self/cgroup 2>/dev/null
}
on_tty1() {
    local t
    t=$(ps -o tty= -p $$ 2>/dev/null | tr -d ' ' || true)
    [ "$t" = tty1 ]
}

# ---------------------------------------------------------------- undo
if [ "$UNDO" = 1 ]; then
    say "undoing the mutible desktop (record: $STATE)"
    [ -r "$STATE" ] || note "no record: removing the files it would have written"
    say "step 1: the service"
    if [ -f "$UNIT" ]; then
        run systemctl disable "$UNIT_NAME"
    else
        note "not installed: $UNIT"
    fi
    say "step 2: the session's files"
    for f in "$UNIT" "$PAM" "$SESSION"; do
        if [ -e "$f" ]; then run rm -f "$f"; else note "already gone: $f"; fi
    done
    [ -d "$LIB" ] && run rmdir --ignore-fail-on-non-empty "$LIB"
    if [ -f "$DEFAULTS" ]; then
        if [ "$(state_get defaults-sha256)" = "$(sha256sum "$DEFAULTS" | cut -d' ' -f1)" ]; then
            run rm -f "$DEFAULTS"
        else
            note "kept, since you changed it: $DEFAULTS"
        fi
    fi
    run systemctl daemon-reload
    say "step 3: tty1's text login"
    if [ "$(state_get getty-tty1)" = enabled ] || [ ! -r "$STATE" ]; then
        run systemctl enable getty@tty1.service
    else
        note "getty@tty1 was not enabled before; left as it is"
    fi
    say "step 4: the PPA"
    if [ "$(state_get ppa-added)" = yes ] && [ -f "$PPA_FILE" ]; then
        run rm -f "$PPA_FILE"
        note "run 'apt update' to forget its packages"
    else
        note "not added by this script; left as it is"
    fi
    say "step 5: the packages it installed"
    added=$(state_get packages-added)
    if [ -n "$added" ]; then
        # shellcheck disable=SC2086
        set -- $added
        note "$# packages were installed by the setup (with their dependencies):"
        printf '%s ' "$@" | fold -s -w 72 | sed 's/^/      /'
        printf '\n'
        note "to remove them: sudo apt remove --autoremove <the packages above>"
        if [ "$REMOVE_PACKAGES" = 1 ]; then
            run apt-get remove --autoremove "$@"
        else
            note "(--remove-packages runs that, apt asking first)"
        fi
    else
        note "none recorded"
    fi
    if [ "$DRY" = 0 ]; then rm -rf "$STATE_DIR"; fi
    say "step 6: the session that runs now"
    if systemctl is-active --quiet "$UNIT_NAME" 2>/dev/null; then
        if in_session && [ "$NOW" = 0 ]; then
            note "this shell runs inside the desktop session; it keeps running until you"
            note "reboot (or log out from mutible's File menu), and tty1 is a text login after that"
        elif [ "$NOW" = 1 ]; then
            run systemctl stop "$UNIT_NAME"
            run systemctl start getty@tty1.service
        else
            note "it keeps running until the next boot; --now stops it and starts tty1's login"
        fi
    else
        note "not running"
    fi
    say "done: the machine is as it was, but for the packages listed above"
    exit 0
fi

# ---------------------------------------------------------------- set up
say "checking the machine"
. /etc/os-release
note "system: ${PRETTY_NAME:-unknown}"
[ "${ID:-}" = ubuntu ] || die "this is for Ubuntu (found ${ID:-nothing})"
[ "${VERSION_CODENAME:-}" = resolute ] ||
    note "warning: written for Ubuntu 26.04 (resolute); this is ${VERSION_CODENAME:-unknown}"
[ "$(ps -p 1 -o comm= 2>/dev/null)" = systemd ] || die "systemd is not PID 1 here"

if [ -z "$USER_NAME" ]; then
    USER_NAME=${SUDO_USER:-}
    [ -n "$USER_NAME" ] || USER_NAME=$(state_get user)
    [ -n "$USER_NAME" ] || [ "$(id -u)" = 0 ] || USER_NAME=$(id -un)   # a dry run
fi
[ -n "$USER_NAME" ] || die "say whose desktop it is: --user NAME"
[ "$USER_NAME" != root ] || die "the desktop's user cannot be root"
USER_UID=$(id -u "$USER_NAME" 2>/dev/null) || die "no user $USER_NAME"
[ "$USER_UID" -ge 1000 ] || die "$USER_NAME (uid $USER_UID) is a system account"
USER_HOME=$(getent passwd "$USER_NAME" | cut -d: -f6)
note "the desktop's user: $USER_NAME (uid $USER_UID, home $USER_HOME)"
if [ -n "$DEB" ]; then
    [ -f "$DEB" ] || die "no such file: $DEB"
    DEB=$(readlink -f "$DEB")
    note "mutible from: $DEB"
else
    note "mutible from: ppa:echansen/minicode"
fi
state_set user "$USER_NAME"

say "step 1: the mutible PPA"
if [ -n "$DEB" ]; then
    note "skipped: mutible comes from $DEB"
elif grep -rqs "echansen/minicode" /etc/apt/sources.list /etc/apt/sources.list.d/ &&
     [ ! -f "$PPA_FILE" ]; then
    note "already configured (not by this script); left as it is"
else
    {
        printf '# The mutible PPA (it keeps its old name), added by mutible-desktop-setup.sh.\n'
        printf '# Signed by %s.\n' "$PPA_FINGERPRINT"
        printf 'Types: deb\nURIs: %s\nSuites: %s\nComponents: main\nSigned-By:\n' \
            "$PPA_URL" "${VERSION_CODENAME:-resolute}"
        sed -e 's/^$/./' -e 's/^/ /' <<'KEY'
-----BEGIN PGP PUBLIC KEY BLOCK-----

xsFNBGrGiMQBEACnfFHadyeoXfuQbc9Xg4iN8F1njqUro/ZuOfcPEdBt6AUiLRI8
VMaLfrvOVY82nSTCplG+fSrERAOsbXLRRj0bybTce8j+P8Cyr/Zj1hFWMaOOGzes
V/suD9LmKfotrY49W4IdbLVYntIH+9CPNqpgnxuQK9C/Xm/23AV0dNqr31Tbrin4
UewE0CYBrbdHfEFJQxkml02Cz/tTYs11uM7VnS4Tti02iLbHCzFxbcf7NEAkTFXq
BWFILXvHmpjhrW4rHgRy5jBPUsWfzu4BGahPIMU/jh9MfOM8G5oArGHRRs0P1qeN
00azI+l9roNx+VjxmC5PCAS2l/n/kgeM5/WzOfmRjrXjvFgyThWvVdhR5ggemT2U
1NfkiAtZyfWGZbfzHp2xy6TwIGg3ChDwvbf9DGf8gaR86KMLKTy1Zkr5NZRIerxq
vuGbsVAlqIq2c6T1V53OTtxaOElrfy+q/+w9RCDgxnrPzU+1SgAM83pMuc33LqqF
W5YBV22hBKUl1Keho3irQS5qVP+mRszRqAMPyU1FDcsCvJpiU6LtJHi4mmHoqZ/o
aHeaoVgcvvp9xVuX11dQjfuGnJ9Y147iaFvHD1xDhczrDDW/4wDT6fe3DcUFF6vW
YZ6v3UAebuO6UA2bpFEFWhT2oH21KyrK6IN8cwtmxDaRTp4FlTmITn4IpwARAQAB
zR1MYXVuY2hwYWQgUFBBIGZvciBFcmljIEhhbnNlbsLBjgQTAQoAOBYhBE7DH1FD
IccfW0elIYkZ71s37v9ABQJqxojEAhsDBQsJCAcCBhUKCQgLAgQWAgMBAh4BAheA
AAoJEIkZ71s37v9A1M4QAKdfOaG4h+GuOPBz3uxUxa0lK2nEY9My8mAg7JDLijzs
wCqZOALoOp5pCumuR4+veU5JgwIaQbtGyNzpIoAB4rZVQSUGaMNzO73jUr/lJWyP
VYdEql1eoVPaIXyA++zqyATm6ijP9qB6298Pj3BNh6XsIUBuliHMcPosqKLfBrF2
aeY6y9r5m2Dw062SCt583HzzkIVJVkQv0pqs6XHchCB2+7IJ8+HhJAopoQ8bM5Xa
KUPJRPeD4omwU59YFLK9Iex3K0ZKTORrH7Q22E6Fua7qBIGGKb4ieOhA7V95c5Jd
rz9Zbq6YJecVW55uQMja7hK+EP+tHM5s4FMWjCr+swMGtTUGKF0Z4ND7+xdKnnXs
3jIoPyjqSkqgMX2rzGNAKJ1lZMyl8gUzuBWRZGlRRtXNmEtogd3H2qJMrAW2Kx/s
fQHvB5MRrggtuZFKwoEhVdQaJAiksJ8woNh8umUxx+pbOEpbJtbiT8K0mjInJoq6
/S9y6Uk5wFrRqJkueOEVeQV+o35SagRl9v+2zCM//Wku7G42DYUPrjWEzhA9QHlo
nvYpDEVjJdaKyFL22WRFpUpZDV3BJ+d73ZdVMClkiveSTR7OcbbkdNYLznPNTWau
odFV43ZkbkKLuoUG3Sq7pVuzsyIqrcdDKhQ3lvT/pDBItRMVRBQL+jHd6wTQHtn5
=2wYx
-----END PGP PUBLIC KEY BLOCK-----
KEY
    } | put "$PPA_FILE" 0644
    [ -f "$PPA_FILE" ] && [ "$DRY" = 0 ] && state_set ppa-added yes
fi

say "step 2: packages"
before=$(dpkg-query -W -f='${db:Status-Status} ${Package}\n' 2>/dev/null | awk '$1=="installed"{print $2}' | sort)
want=()
for p in "${PACKAGES[@]}"; do installed "$p" || want+=("$p"); done
want_mutible=0
if [ -n "$DEB" ]; then
    # The same version from the same .deb again is nothing to do.
    [ "$(dpkg-query -W -f='${Version}' mutible 2>/dev/null || true)" = "$(dpkg-deb -f "$DEB" Version)" ] &&
        installed mutible || want_mutible=1
elif ! installed mutible; then
    want_mutible=1
fi
if [ ${#want[@]} -eq 0 ] && [ "$want_mutible" = 0 ]; then
    note "everything is installed already"
else
    [ ${#want[@]} -gt 0 ] && note "missing: ${want[*]}"
    export DEBIAN_FRONTEND=noninteractive
    run apt-get update
    if [ ${#want[@]} -gt 0 ]; then
        run apt-get install -y --no-install-recommends "${want[@]}"
    fi
    if [ -n "$DEB" ]; then
        run apt-get install -y --no-install-recommends "$DEB"
    elif [ "$want_mutible" = 1 ]; then
        if [ "$DRY" = 0 ] && ! apt-cache policy mutible | grep -q 'Candidate: [0-9]'; then
            die "the PPA has no mutible for ${VERSION_CODENAME:-this release} yet; try --deb FILE"
        fi
        run apt-get install -y --no-install-recommends mutible
    fi
fi
if [ "$DRY" = 0 ]; then
    after=$(dpkg-query -W -f='${db:Status-Status} ${Package}\n' 2>/dev/null | awk '$1=="installed"{print $2}' | sort)
    new=$(comm -13 <(printf '%s\n' "$before") <(printf '%s\n' "$after") | tr '\n' ' ')
    old=$(state_get packages-added)
    all=$(printf '%s %s' "$old" "$new" | tr ' ' '\n' | sed '/^$/d' | sort -u | tr '\n' ' ')
    state_set packages-added "${all% }"
    [ -n "$new" ] && note "newly installed: $(printf '%s' "$new" | wc -w) packages (recorded for --undo)"
fi
command -v cage >/dev/null || [ "$DRY" = 1 ] || die "cage did not install"
command -v mutible >/dev/null || [ "$DRY" = 1 ] || die "mutible did not install"

say "step 3: the session"
put "$SESSION" 0755 <<'EOF'
#!/bin/sh
# The mutible desktop's session (mutible-desktop-setup.sh): run by
# mutible-desktop.service on tty1, as the desktop's user, in a logind
# session on seat0. mutible is cage's only app; when it quits, cage and the
# session end and systemd starts them again.
export MUTIBLE_DESKTOP=1               # mutible's desktop mode (Suspend, Power Off, ...)
export XDG_SESSION_TYPE=wayland
export XDG_CURRENT_DESKTOP=mutible
export GDK_DEBUG=no-portals            # GTK's own file chooser: no portal runs here
# The keyboard layout the console uses (dpkg-reconfigure keyboard-configuration)
if [ -r /etc/default/keyboard ]; then
    . /etc/default/keyboard
    [ -n "${XKBLAYOUT:-}" ] && export XKB_DEFAULT_LAYOUT="$XKBLAYOUT"
    [ -n "${XKBVARIANT:-}" ] && export XKB_DEFAULT_VARIANT="$XKBVARIANT"
    [ -n "${XKBOPTIONS:-}" ] && export XKB_DEFAULT_OPTIONS="$XKBOPTIONS"
    [ -n "${XKBMODEL:-}" ] && export XKB_DEFAULT_MODEL="$XKBMODEL"
fi
cd "$HOME" || cd /
# Through the user's login shell, so ~/.profile's PATH (~/.local/bin, for
# language servers) reaches mutible. -s: Ctrl+Alt+F1 to F12 switch consoles.
start=$(cut -d. -f1 /proc/uptime)
"${SHELL:-/bin/sh}" -l -c 'exec cage -s -- mutible --foreground'
status=$?
ran=$(( $(cut -d. -f1 /proc/uptime) - start ))
# A session that ends by itself within 30 s of starting, three times running,
# is a crash at start: rather than start it again, tty1 gets a text shell
# (Ctrl+Alt+F2 is a login prompt whatever happens). Log Out exits 0.
quick=${RUNTIME_DIRECTORY:-/tmp}/quick-exits
if [ "$status" -eq 0 ] || [ "$ran" -ge 30 ]; then
    rm -f "$quick"
    exit "$status"
fi
n=$(( $(cat "$quick" 2>/dev/null || echo 0) + 1 ))
echo "$n" > "$quick"
[ "$n" -ge 3 ] || exit "$status"
rm -f "$quick"
exec </dev/tty >/dev/tty 2>&1
printf '\nmutible ended within 30 s of starting, %s times running (status %s).\n' "$n" "$status"
printf 'This is a text shell. journalctl -b -u mutible-desktop shows what it printed;\n'
printf 'type exit to start the desktop again.\n\n'
exec "${SHELL:-/bin/sh}" -l
EOF

put "$PAM" 0644 <<'EOF'
# PAM for mutible-desktop.service (mutible-desktop-setup.sh). systemd opens
# the session without asking for a password; the session lines register it
# with logind (pam_systemd, in common-session) on seat0, which gives the
# user the screen, input and sound, and polkit's "active local session".
@include common-auth
@include common-account
session   required  pam_loginuid.so
session   optional  pam_keyinit.so force revoke
session   required  pam_limits.so
session   required  pam_env.so readenv=1
session   required  pam_env.so readenv=1 envfile=/etc/default/locale
@include common-session
EOF

defaults_text='# Settings for the mutible desktop (mutible-desktop-setup.sh), read by
# mutible-desktop.service: one VARIABLE=value a line, for mutible and cage.
# Restart the session (File > Log Out…) after a change.
#
# The keyboard layout comes from /etc/default/keyboard; to set another:
#XKB_DEFAULT_LAYOUT=us
#XKB_DEFAULT_OPTIONS=ctrl:nocaps
# How long mutible takes to start, phase by phase, in the journal
# (journalctl -u mutible-desktop):
#MUTIBLE_STARTUP_LOG=1'
if [ -f "$DEFAULTS" ] && [ "$(state_get defaults-sha256)" != "$(sha256sum "$DEFAULTS" | cut -d' ' -f1)" ]; then
    note "kept, since you changed it: $DEFAULTS"
else
    printf '%s\n' "$defaults_text" | put "$DEFAULTS" 0644
    [ "$DRY" = 0 ] && state_set defaults-sha256 "$(sha256sum "$DEFAULTS" | cut -d' ' -f1)"
fi

put "$UNIT" 0644 <<EOF
# The mutible desktop (mutible-desktop-setup.sh): $USER_NAME logged in on
# tty1, running mutible under cage, in place of tty1's text login.
[Unit]
Description=mutible desktop on tty1
Documentation=https://github.com/ible-ai/mutible/blob/main/linux/desktop/README.md
After=systemd-user-sessions.service plymouth-quit-wait.service getty-pre.target
After=dbus.socket systemd-logind.service
Wants=dbus.socket systemd-logind.service
Conflicts=getty@tty1.service
After=getty@tty1.service
ConditionPathExists=/dev/tty0
# The session script stops a crash loop itself (a text shell on tty1 after
# three quick exits); this is the backstop if even that fails at once.
StartLimitIntervalSec=60
StartLimitBurst=10

[Service]
Type=simple
User=$USER_NAME
WorkingDirectory=~
PAMName=mutible-desktop
TTYPath=/dev/tty1
TTYReset=yes
TTYVHangup=yes
TTYVTDisallocate=yes
StandardInput=tty-fail
StandardOutput=journal
StandardError=journal
UtmpIdentifier=tty1
UtmpMode=user
Environment=XDG_SESSION_TYPE=wayland XDG_SEAT=seat0 XDG_VTNR=1
EnvironmentFile=-$DEFAULTS
ExecStart=$SESSION
Restart=always
RestartSec=2
RuntimeDirectory=mutible-desktop
RuntimeDirectoryPreserve=restart

[Install]
WantedBy=multi-user.target
EOF

say "step 4: tty1"
run systemctl daemon-reload
if [ -z "$(state_get getty-tty1)" ]; then
    if systemctl is-enabled --quiet getty@tty1.service 2>/dev/null; then
        state_set getty-tty1 enabled
    else
        state_set getty-tty1 disabled
    fi
fi
if systemctl is-enabled --quiet getty@tty1.service 2>/dev/null; then
    run systemctl disable getty@tty1.service
else
    note "getty@tty1 is not enabled"
fi
if systemctl is-enabled --quiet "$UNIT_NAME" 2>/dev/null; then
    note "enabled already: $UNIT_NAME"
else
    run systemctl enable "$UNIT_NAME"
fi

say "step 5: power and the lid (logind; nothing changed)"
lid=$(systemd-analyze cat-config systemd/logind.conf 2>/dev/null |
      sed -n 's/^HandleLidSwitch=//p' | tail -n 1)
[ -n "$lid" ] || lid="suspend (logind's default)"
note "closing the lid: $lid"
note "suspend and power off: allowed to $USER_NAME's session without a password while"
note "no one else is logged in (polkit's defaults); systemctl suspend works in mutible's terminal"

if [ "$START" = 1 ]; then
    say "starting it now"
    if on_tty1; then
        note "refused: this shell is tty1's login, which starting it would end; reboot instead"
    else
        run systemctl reset-failed "$UNIT_NAME" || true
        run systemctl start "$UNIT_NAME"
    fi
fi
say "done: reboot, and $USER_NAME's mutible comes up on tty1 (Ctrl+Alt+F2 is a text console)"
note "undo it with: sudo $0 --undo"
