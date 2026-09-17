#!/bin/bash
# Start a virtual X display with a compositor, once. Its backdrop is gray 128,
# so a see-through panel's alpha can be worked out from the screenshot blend.
export DISPLAY=:99
if ! xset q >/dev/null 2>&1; then
  Xvfb :99 -screen 0 1280x800x24 +extension Composite >/tmp/xvfb.log 2>&1 &
  for i in $(seq 1 50); do xset q >/dev/null 2>&1 && break; sleep 0.1; done
  xcompmgr >/tmp/xcompmgr.log 2>&1 &
  sleep 0.5
fi
