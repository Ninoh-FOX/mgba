# Cable Link over netplay (libretro core)

The mGBA libretro core can join 2 to 4 GBAs with a Game Link Cable over a
RetroArch netplay session. It works like the "Dual Cable Link" of gpSP:
**every console runs every GBA of the link**: the player's own GBA, and
hidden copies of the other players'. The GBAs of a console are joined by a
cable in memory, and only the buttons travel over the network. A game's link
traffic never waits for the WiFi, however many transfers it makes per frame
(Zelda: Four Swords, for one). The price
is a few frames of input delay (2-3 on a local network, chosen
automatically from the round trip times) and the CPU of the other GBAs (they
run on their own threads, so on the other CPU cores).

## Options

| option | values | |
|---|---|---|
| **Cable Link over Netplay** | ON (default) / OFF | Read when the game loads. With ON the core takes over RetroArch's netplay for GBA games (RetroArch then sends the core's packets instead of running its own rollback netplay). |
| **Cable Link Players** | 2 (default), 3, 4 | How many GBAs are on the cable. Every console must have the same value. Only that many GBAs are loaded. |

## What must match

Each console runs a copy of the others' GBAs, so they must be the very same
machine. If anything differs, every console says **"Cable Link: games or
settings differ, playing alone"** (or "... a different number of players
...") and each one plays alone:

- the game: the very same dump (same CRC),
- the BIOS: option **Use BIOS File if Found** and `system/gba_bios.bin`
  (copy the same file to every console, or turn the option off everywhere),
- **Game Boy Player Rumble**,
- the core: the same build on every console.

Everything else (colours, frameskip, audio filter, solar sensor level,
"allow opposing directions", idle loop removal, skip BIOS) may differ: while
linked the core puts the settings that would change the machine aside on
every GBA (no idle loop *detection*, the known idle loops are still removed;
a reset skips the BIOS intro; sensors read nothing) and gives back the
player's own when the link ends.

## How a session goes

1. Every console starts a netplay session (host and clients) as usual.
   Each client compares ROM, BIOS, settings, protocol version and number of
   players with the host.
2. Until all the players are there the games go on, and every console shows
   "waiting for players (2/4)".
3. With everybody there, the host measures the round trip to each client
   and picks the input delay.
4. Each GBA restarts once (games only look for the cable at boot), keeping
   its player's save memory, and its state goes to every other console. The
   host is player 1, the clients follow in the order they came. The RTC of
   every GBA starts from the host's clock.
5. From then on only the buttons are sent. Every 2 seconds the consoles
   compare all the GBAs; if they ever differ the link is cut with "the
   consoles went out of sync".
6. Every console shows the same frame at the same time: one that gets ahead
   holds a frame (a frame of silence) now and then. If the WiFi has a bad
   moment, a console sends its buttons a frame earlier from then on (up to
   4 frames more), and takes it off again after a calm minute.

A reset while linked goes with the buttons: every console restarts every
GBA in the same frame, the cable still plugged in. While linked, savestates
cannot be loaded and cheats are off (both would make the copies of a GBA
different). When a console leaves, each player's GBA carries on alone.

## The cable

`dual.c` has one end of the cable per GBA (a `GBASIODriver`): an AGB-005
cable for 2 to 4 GBAs (multiplayer mode for 2-4, normal mode between players
1 and 2, general purpose mode with the pins as wired). Each end counts
session cycles; what one GBA does is applied by the others when their clock
gets there. A child never runs ahead of the parent, and the parent at most
half a frame ahead of the slowest child, so the result never depends on how
the threads happen to run: every console computes the same machines.

| file | what |
|---|---|
| `dual.h` | the interface for `libretro.c` |
| `dual.c` | the cable, the hidden cores and their threads, the network protocol, the input lockstep |
| `libretro.c` | netpacket interface, `retro_run` of a linked frame, reset/savestate/cheat guards |

The hidden GBAs use the dummy renderer (they never draw) and read the ROM of
the player's GBA (no extra copy), except carts with GPIO (RTC, rumble,
...), which write their pins into the ROM: those get a copy-on-write mapping.

## Build (Miyoo Mini)

    src/platform/libretro/build-miyoomini.sh

gives `mgba_plus_libretro.so` at the root of the repository (optimized,
stripped, only the `retro_*` functions exported). The flags follow gpSP's
`platform=miyoomini`. Do not build it with a bare `cmake
-DCMAKE_C_FLAGS=...`: the toolchain's CMake file keeps its `-Os` in
`CMAKE_C_FLAGS`, so passing your own flags without an `-O` level gives a
core built at `-O0`, about half as fast (the script checks for that).
`dual.c` uses POSIX threads and semaphores (Linux, Android, ...): a Windows
build of the core would need a pthreads library.

## Test without consoles

`tools/dualtest` (adapted from gpSP's) runs 2 to 4 consoles with a
RetroArch-like frontend in one process, joined by a network in memory:

    tools/dualtest/run.sh tools/dualtest/linktest.gba 600 5 60
    PLAYERS=4 DATA=1 tools/dualtest/run.sh /path/to/game.gba 2400 5 60
    PLAYERS=2 tools/dualtest/run.sh arm tools/dualtest/linktest.gba 600 5 60

Arguments: ROM, frames, one-way latency (ms), frames each player plays
alone first. The first form uses `build-host/mgba_libretro.so`, `arm` the
Miyoo Mini build (`mgba_plus_libretro.so`, see above) under `qemu-arm`. `DATA=1` gives the ROM in memory (as
RetroArch does), `SESSIONS=n`, `RESET_AT=n`, `ROM2=...`, `PLAYERS1=n`,
`PACE=1`, `KEYS0..3`, `SHOTS=dir` work as described in `dualtest.c`.
`SOLO_ONLY=1` only runs the frames alone (to time any core). `linktest.gba` makes tens of multiplayer transfers per frame; the harness
checks that every GBA logged the same words. A core built with
`-DDUAL_DEBUG` and run with `DUAL_FAULT=1` gives a hidden GBA other
buttons once, to check that the agreement check catches it.
