# What scripts/ppa.sh needs to make, check and upload MiniCode's Ubuntu source
# package. Only the source package is built here (Launchpad builds the
# binaries), so none of the GTK, VTE or WebKit build dependencies are needed.
FROM ubuntu:26.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      debhelper devscripts distro-info dput libdistro-info-perl gnupg lintian meson \
    && rm -rf /var/lib/apt/lists/*
