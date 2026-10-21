#!/bin/sh
# Runs the Cable Link test harness.
#   run.sh [arm] ROM FRAMES [latency_ms] [solo_frames]
# Without "arm": the core built for this machine (build-host). With "arm":
# the Miyoo Mini core (mgba_plus_libretro.so, from
# src/platform/libretro/build-miyoomini.sh) under qemu-arm.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$HERE/../..
if [ "$1" = "arm" ]; then
  shift
  make -s -C "$HERE" arm
  exec qemu-arm -L /opt/miyoomini-toolchain/arm-miyoomini-linux-gnueabihf/sysroot \
    "$HERE/dualtest-arm" "$ROOT/mgba_plus_libretro.so" "$@"
fi
make -s -C "$HERE" dualtest
exec "$HERE/dualtest" "$ROOT/build-host/mgba_libretro.so" "$@"
