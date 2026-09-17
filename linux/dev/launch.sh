#!/bin/bash
# (Re)launch MiniCode on the virtual display with a scratch home and project.
#   BIN=/build/minicode   the binary (default)
#   WAIT=1.5              seconds to wait after the window appears
#   GTK_CSD=1             draw GTK's own title bar (there is no window manager)
# The log goes to /tmp/app.log.
source /tools/display.sh
pkill -x minicode 2>/dev/null; sleep 0.3   # -x: -f would match this script's caller
mkdir -p /t/home /t/proj
[ -e /t/proj/a.py ] || printf 'def f():\n    return hello\n' > /t/proj/a.py
[ -e /t/proj/b.md ] || printf '# Title\n\nbody\n' > /t/proj/b.md
# WebKit's bubblewrap sandbox cannot start inside a container and aborts the
# whole app, so it is disabled here. Never do this outside a test container.
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 GTK_A11Y=none GDK_BACKEND=x11
export HOME=/t/home XDG_CONFIG_HOME=/t/home/.config SHELL=/bin/bash
cd /t/proj
dbus-run-session -- "${BIN:-/build/minicode}" /t/proj >/tmp/app.log 2>&1 &
for i in $(seq 1 50); do xdotool search --name MiniCode >/dev/null 2>&1 && break; sleep 0.2; done
sleep "${WAIT:-1.5}"
