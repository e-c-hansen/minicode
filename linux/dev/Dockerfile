# A Linux environment for building and exercising the GTK port from any machine
# with Docker, including a Mac. See linux/dev/README.md.
FROM ubuntu:26.04
ENV DEBIAN_FRONTEND=noninteractive
# The CI packages, plus a virtual X display with a compositor (so translucent
# panels really blend), synthetic input, screenshots, and a session bus.
RUN apt-get update && apt-get install -y --no-install-recommends \
      meson ninja-build build-essential pkg-config \
      libgtk-4-dev libvte-2.91-gtk4-dev libwebkitgtk-6.0-dev \
      xvfb xcompmgr x11-xserver-utils xdotool imagemagick \
      dbus-x11 fonts-dejavu-core adwaita-icon-theme zsh \
    && rm -rf /var/lib/apt/lists/*
COPY display.sh launch.sh px.sh /tools/
RUN chmod +x /tools/*.sh
WORKDIR /work/linux
