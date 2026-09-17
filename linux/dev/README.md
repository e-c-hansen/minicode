# Testing the GTK port in Docker

This builds and runs the Linux port from any machine with Docker, a Mac
included, and lets you drive the real app and look at it. It is how the
settings file, transparency, Ctrl+/ and the divider drags were checked.

    linux/dev/gtk-dev.sh          # build the image and the port, run the tests
    linux/dev/gtk-dev.sh shell    # the same, then a shell in the container
    linux/dev/gtk-dev.sh stop     # remove the container

The repository is mounted at `/work`, and the port builds into `/build` inside
the container, so the host's `linux/build` is left alone. Rerunning the script
rebuilds incrementally. The image is about 1.8 GB.

Inside the container:

- `/tools/launch.sh` starts (or restarts) MiniCode on a virtual 1280x800 X
  display with a compositor, using a scratch home in `/t/home` and a small
  project in `/t/proj`. Set `GTK_CSD=1` to see GTK's own title bar, since
  there is no window manager.
- Input: `DISPLAY=:99 xdotool key ctrl+shift+t`, `xdotool mousemove X Y click 1`,
  and drags as `mousedown 1`, a few `mousemove`s, then `mouseup 1`.
- Screenshots: `import -window root shot.png`, then `docker cp` it out.
  `/tools/px.sh shot.png X Y` prints pixel colors. The compositor's backdrop is
  gray 128, so a panel at opacity `a` over color `c` reads `a*c + (1-a)*128`.
- `/tmp/app.log` has the app's output.

Things to know:

- WebKit's sandbox cannot start inside a container, so `launch.sh` turns it
  off. Pages still load, but the TLS certificate store is empty, so HTTPS
  pages show a certificate error. The toolbar still works.
- Dotted glyph debris in a root screenshot is the compositor missing a redraw,
  not the app; it disappears after a resize and never shows in
  `import -window <id>`.
- This is X11 with a basic compositor. GNOME on Wayland is close but not the
  same, so the look is still worth a glance on the real desktop.
