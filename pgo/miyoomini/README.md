# PGO profile for the Miyoo Mini build

`profile/` is the GCC profile (`.gcda`) recorded on a Miyoo Mini Plus at 1200 MHz:
three games (Mario Kart: Super Circuit, The Minish Cap, Final Fight One) and a
two-console Cable Link session.

`src/platform/libretro/build-miyoomini.sh` uses it automatically
(`-fprofile-use`); `PGO=0 build-miyoomini.sh` builds without it.

It only fits these sources at this path (/opt/codes/mgba): GCC checks the source
file names, and CMake passes absolute ones. After changing a source file, or with
the repository somewhere else, record it again (GCC ignores the profile of what
does not match and prints a `-Wcoverage-mismatch` warning).

Same output as without it (video and audio CRCs identical, Cable Link test with
4 consoles passes).

## Recording it again

1. `PGO=generate src/platform/libretro/build-miyoomini.sh` (use a fresh build dir)
2. `arm-linux-gcc -O2 -I src/platform/libretro -o lrbench pgo/miyoomini/lrbench.c -ldl` and `make -C tools/dualtest arm` (Cable Link harness)
3. Copy `mgba_plus_libretro.so`, `lrbench`, `train.sh` and `dualtest-arm` to one folder
   on the device (e.g. /tmp/pgo_tools) and run
   `train.sh /tmp/pgo_tools/mgba_plus_libretro.so mGBA 1800 /tmp/pgo_tools/dualtest-arm`
4. Unpack `/tmp/pgo.tar` into `pgo/miyoomini/profile/` (replacing it) and rebuild.

`lrbench core.so rom frames [core.opt]` is also a benchmark (fps and CRCs of the
video and audio). `LR_KEYS=1` presses START/A on a schedule, `LR_SYSDIR=dir` is the BIOS folder.
