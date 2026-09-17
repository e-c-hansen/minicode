#!/bin/bash
# Build the image if needed, start the container with the repo mounted, and
# build the port inside it. Run from anywhere:
#   linux/dev/gtk-dev.sh            build and run the tests
#   linux/dev/gtk-dev.sh shell      then open a shell in the container
#   linux/dev/gtk-dev.sh stop       remove the container
set -e
repo=$(cd "$(dirname "$0")/../.." && pwd)
name=minicode-gtk

if [ "$1" = stop ]; then docker rm -f $name >/dev/null; exit; fi

docker build -q -t $name "$repo/linux/dev" >/dev/null
if ! docker ps --format '{{.Names}}' | grep -qx $name; then
  docker rm -f $name >/dev/null 2>&1 || true
  docker run -d --name $name -v "$repo:/work" $name sleep infinity >/dev/null
fi
# Build outside the mounted repo, so linux/build stays the host's.
docker exec $name bash -c '
  [ -f /build/build.ninja ] || meson setup /build -Dterminal=enabled -Dbrowser=enabled >/dev/null
  meson compile -C /build && meson test -C /build --print-errorlogs'
[ "$1" = shell ] && exec docker exec -it $name bash
echo "Container $name is up. Try:"
echo "  docker exec $name /tools/launch.sh"
echo "  docker exec $name bash -c 'DISPLAY=:99 xdotool key ctrl+comma; DISPLAY=:99 import -window root /tmp/shot.png'"
echo "  docker cp $name:/tmp/shot.png ."
