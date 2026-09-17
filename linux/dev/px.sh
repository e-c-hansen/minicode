#!/bin/bash
# px.sh image.png X Y [X Y ...]   print the color at each point
img=$1; shift
while [ $# -ge 2 ]; do
  echo "$1,$2 $(convert "$img" -format "%[pixel:p{$1,$2}]" info:)"
  shift 2
done
