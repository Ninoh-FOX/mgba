/* Copyright (c) 2026 Ninoh-FOX (based on the Dual Cable Link of gpSP plus)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Dual Cable Link: all the GBAs of a link session (2 to 4) run on every
 * console, joined by a link cable in memory, and only the buttons travel
 * over the network (libretro netpacket interface).
 *
 * The GBA the frontend shows is the "own" core (libretro.c). The other
 * players' GBAs are hidden mGBA cores of the same game, each on its own
 * thread. Each GBA has its own end of the cable (a GBASIODriver, dual.c).
 *
 * Slot 0 is the parent end of the cable (the netplay host's GBA), slots 1-3
 * the children (the clients' GBAs, in the order the host gave them). On each
 * console the own core is that console's slot and the hidden cores are the
 * other ones. Every console computes the same machines, so everything the
 * cable does depends on emulated time only, never on how fast the threads
 * happen to run. See README-dual.md. */

#ifndef MGBA_LIBRETRO_DUAL_H
#define MGBA_LIBRETRO_DUAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libretro.h"

struct mCore;

#define DUAL_MAX_PLAYERS 4

/* What retro_run does with a frame (dual_frame_begin). */
enum {
	DUAL_FRAME_WAIT = 0, /* Someone's buttons are not here yet: show the last frame again */
	DUAL_FRAME_RUN,      /* Run it (dual_frame_run) */
	DUAL_FRAME_HOLD      /* Ahead of the other consoles: a frame of silence */
};

/* Frontend side, called by libretro.c */
void dual_set_frontend(retro_environment_t env, void (*message)(const char* text, unsigned ms),
                       void (*prepare)(void));
void dual_game_loaded(struct mCore* core, const void* rom, size_t romSize, const char* path);
void dual_game_unloaded(void);
void dual_set_players(unsigned players);
void dual_set_net(retro_netpacket_send_t send, retro_netpacket_poll_receive_t poll);

void dual_session_start(unsigned client_id);
bool dual_peer_connected(unsigned client_id);
void dual_peer_disconnected(unsigned client_id);
void dual_session_stop(void);
bool dual_net_receive(const void* buf, size_t len, uint16_t client_id);

bool dual_setting_up(void);   /* true: do not run the game this frame */
bool dual_running(void);
bool dual_linked(void);       /* Setting up or running: no savestates, no cheats */
void dual_request_reset(void);

int dual_frame_begin(uint32_t local_keys);
void dual_frame_run(void);

bool dual_hidden_thread(void);
void dual_shutdown(void);

#endif
