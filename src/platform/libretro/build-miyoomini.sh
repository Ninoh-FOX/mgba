#!/bin/sh
# Builds the libretro core for the Miyoo Mini as mgba_plus_libretro.so (at
# the root of the repository), optimized and stripped.
#
#   src/platform/libretro/build-miyoomini.sh [build dir]
#
# The flags follow gpSP's "platform=miyoomini" (Cortex-A7, NEON, code kept
# small for the small caches). The toolchain's CMake file puts its -Os in
# CMAKE_C_FLAGS: passing our own CMAKE_C_FLAGS replaces it, so the -O level
# must be here (without one the core is built at -O0 and runs at half speed).
# -fvisibility=hidden: only the retro_* functions are exported, everything
# else is called and read directly (not through the PLT/GOT).
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
TC=/opt/miyoomini-toolchain
BUILD=${1:-$ROOT/build-miyoomini}

CFLAGS="-Ofast -marm -mcpu=cortex-a7 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
 -fdata-sections -ffunction-sections -fno-stack-protector -fno-ident -fomit-frame-pointer \
 -falign-functions=1 -falign-jumps=1 -falign-loops=1 -fno-unwind-tables \
 -fno-asynchronous-unwind-tables -fno-unroll-loops -fmerge-all-constants -fno-math-errno \
 -fvisibility=hidden"

mkdir -p "$BUILD"
cd "$BUILD"
cmake "$ROOT" -DCMAKE_TOOLCHAIN_FILE=$TC/share/buildroot/toolchainfile.cmake \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="$CFLAGS" \
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--gc-sections" \
  -DBUILD_LIBRETRO=ON -DSKIP_LIBRARY=ON -DBUILD_QT=OFF -DBUILD_SDL=OFF \
  -DBUILD_GL=OFF -DBUILD_GLES2=OFF -DBUILD_GLES3=OFF -DUSE_FFMPEG=OFF \
  -DUSE_ZLIB=OFF -DUSE_PNG=OFF -DUSE_LIBZIP=OFF -DUSE_MINIZIP=OFF \
  -DUSE_SQLITE3=OFF -DUSE_ELF=OFF -DUSE_LUA=OFF -DUSE_DISCORD_RPC=OFF \
  -DUSE_EDITLINE=OFF -DUSE_EPOXY=OFF -DUSE_LZMA=OFF -DUSE_JSON_C=OFF \
  -DENABLE_SCRIPTING=OFF -DBUILD_UPDATER=OFF >/dev/null

# The flags that reach the compiler must have the optimization level
if ! grep -q -- "-Ofast" CMakeFiles/mgba_libretro.dir/flags.make; then
  echo "error: the core would not be optimized" >&2
  exit 1
fi

make -j"$(nproc)" mgba_libretro
$TC/usr/bin/arm-linux-strip --strip-unneeded -o "$ROOT/mgba_plus_libretro.so" mgba_libretro.so
echo "$ROOT/mgba_plus_libretro.so"
