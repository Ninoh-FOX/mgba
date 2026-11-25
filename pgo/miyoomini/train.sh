#!/bin/sh
# Records the PGO profile on the Miyoo Mini with the instrumented core
# (built with PGO=generate, it writes its counters under /tmp/pgo).
#   train.sh CORE.so "CONFIG DIR" FRAMES [CABLE_LINK_HARNESS]
# Runs three GBA games from /mnt/SDCARD/Roms/GBA with the BIOS and scripted
# START/A presses, then (with the harness from tools/dualtest built for ARM) a
# two-console Cable Link session so the hidden GBAs get a profile too.
# Leaves the profile in /tmp/pgo.tar. lrbench must be next to this script.
C=$1; CD=$2; FR=$3; DUAL=$4
HERE=$(cd "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH=/mnt/SDCARD/Koriki/lib:/lib:/config/lib:/customer/lib LR_KEYS=1 LR_SYSDIR=/mnt/SDCARD/BIOS
OPT="/mnt/SDCARD/RetroArch/.retroarch/config/$CD/$CD.opt"
SM=$(pidof simplemenu); [ -n "$SM" ] && kill -STOP $SM
rm -rf /tmp/pgo; mkdir -p /tmp/pgo
for g in "Mario Kart" "Minish Cap (Europe" "Final Fight"; do
  R=$(ls /mnt/SDCARD/Roms/GBA/*.gba | grep "$g" | head -1)
  printf "%s: " "$g"; $HERE/lrbench $C "$R" $FR "$OPT" 2>/dev/null | tail -1
done
if [ -n "$DUAL" ]; then
  R=$(ls /mnt/SDCARD/Roms/GBA/*.gba | grep "Mario Kart" | head -1)
  mkdir -p /tmp/pgo_dt; printf "Cable Link: "
  TMPDIR=/tmp/pgo_dt PLAYERS=2 $DUAL $C "$R" $FR 2>&1 | grep -E "^session|PASS|FAIL" | tr "\n" " "; echo
  rm -rf /tmp/pgo_dt
fi
[ -n "$SM" ] && kill -CONT $SM
cd /tmp/pgo && tar cf /tmp/pgo.tar . && echo "profile: $(find . -name '*.gcda' | wc -l) files in /tmp/pgo.tar"
