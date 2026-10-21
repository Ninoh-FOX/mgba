/* Copyright (c) 2026 Ninoh-FOX (based on the Dual Cable Link of gpSP plus)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Dual Cable Link (see dual.h and README-dual.md).
 *
 * Every console runs every GBA of the session: the own one (the core the
 * frontend shows) and copies of the other players' (hidden cores). The GBAs
 * of a console are joined by a cable in memory (the bus below), and only the
 * buttons cross the network.
 *
 *   host                                     clients
 *     HELLO (ROM, BIOS, settings) <-------->  HELLO
 *     WAITING (players so far)    --------->  (the games go on meanwhile)
 *     START (who is in each slot) --------->  every game stops
 *     PING/PONG (round trip to each client)
 *     GO (clock for the RTC)      --------->
 *     STATE of its own GBA        <-------->  STATE, to everybody
 *       (with the save memory, run length coded: after the restart it is
 *        mostly empty)
 *     READY (input delay)         --------->
 *     INPUT (frame, buttons, ...) <-------->  INPUT, every frame, to everybody
 *     HASH (every GBA, sometimes) <-------->  HASH: all the copies must agree
 *
 * The clients only talk to the host; the frontend passes on what a client
 * sends to everybody. The host waits for all the players before it starts,
 * so that the games restart once (games only look for the cable at boot),
 * keeping each player's save: each GBA is copied to every console. If
 * anything does not match (other ROM, BIOS, settings, number of players)
 * nobody is linked and each console plays alone.
 *
 * The buttons for frame N are sent at frame N - delay, and every console
 * runs frame N with the same buttons on every GBA: all the consoles compute
 * the same machines. A console that does not have someone's buttons yet
 * shows the last frame again and tries later. All the consoles also show
 * frame N at the same time: each one tells the others how far ahead of each
 * it thinks it is, and one that is ahead waits a frame now and then. */

#include "dual.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <mgba-util/common.h>

#include <mgba/core/cheats.h>
#include <mgba/core/core.h>
#include <mgba/core/serialize.h>
#include <mgba/core/version.h>
#include <mgba/gba/core.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/sio.h>
#include <mgba-util/crc32.h>
#include <mgba-util/memory.h>
#include <mgba-util/vfs.h>

#define NP DUAL_MAX_PLAYERS
#define DUAL_HIDDEN (NP - 1)

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifdef DUAL_DEBUG
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DBG(...)
#endif

/* ---- timing ------------------------------------------------------------- */

/* How far (cycles) the parent may run ahead of the slowest child. A child
 * never runs ahead of the parent, so whatever the parent does reaches the
 * children on time; what a child does (other than answering a transfer)
 * reaches the others this much later. Half a frame. */
#define DUAL_LOOKAHEAD 140448

/* What a GBA that finished its frame promises the others (cycles). */
#define DUAL_FRAME_SLACK 1232

/* Each GBA looks at the cable every this many cycles: what the others did
 * is applied then. (Its own transfers end at their exact time.) */
#define DUAL_SYNC_STEP 512

/* ---- protocol ----------------------------------------------------------- */

#define DUAL_HEADER        0x4d474431   /* MGD1 (mGBA, Dual) */
#define DUAL_VERSION       1

/* Packets: header, type | sender's slot << 8 (FF: none yet), words, data. */
#define DPK_HELLO          1   /* version, rom, bios, settings, players, host */
#define DPK_PING           2   /* time */
#define DPK_PONG           3   /* time (echoed) */
#define DPK_STATE          4   /* offset, total (coded) size, data */
#define DPK_READY          5   /* input delay, one way time of each slot (us) */
#define DPK_INPUT          6   /* frame, buttons, current frame, lead over
                                  each slot (us), times we waited for each */
#define DPK_HASH           7   /* frame, hash of each slot's GBA */
#define DPK_BYE            8
#define DPK_WAITING        9   /* players so far, players wanted */
#define DPK_START          10  /* players, client id of each slot */
#define DPK_GO             11  /* the states can go (RTC clock, ms: lo, hi) */
#define DPK_GIVEUP         12  /* nobody is linked (1: players differ) */

#define DUAL_NO_SLOT       0xFF
#define DUAL_CHUNK         (16 * 1024)
#define DUAL_STATE_MAX     (4 * 1024 * 1024)   /* Sanity limit of a coded state */
#define DUAL_RING          256         /* Frames of buttons kept */
#define DUAL_HASH_EVERY    120         /* Frames between agreement checks */
#define DUAL_HASH_SLOTS    4
#define DUAL_HELLO_US      6000000     /* No answer: the other core is not dual */
#define DUAL_HELLO_EVERY   250000      /* HELLO sent again until the host's comes */
#define DUAL_SETUP_US      30000000    /* Whole setup */
#define DUAL_PEER_US       15000000    /* Peer silent while playing: it is gone */
#define DUAL_STALL_US      12000       /* Longest wait for buttons in a frame */
#define DUAL_FRAME_US      16743
#define DUAL_KEY_RESET     0x80000000  /* With the buttons: every game restarts */
#define DUAL_PINGS         8           /* Round trips measured before the states */
#define DUAL_PING_US       1000000     /* ...for at most this long */
#define DUAL_PONG_US       200000      /* A ping with no answer: send another */
#define DUAL_SLICE_US      10000       /* Setup work per frame (it waits anyway) */
#define DUAL_SYNC_WIN      16          /* Packets a peer's position comes from */
#define DUAL_SYNC_US       (DUAL_FRAME_US * 5 / 8)   /* Ahead by more: wait */
#define DUAL_SYNC_GAP      60          /* Frames between two waits */
#define DUAL_DELAY_MORE    4           /* Most frames added to the input delay */
#define DUAL_DELAY_CALM_US 60000000    /* Peers happy this long: one frame less */
#define DUAL_DELAY_CALM_MAX 600000000  /* (twice as long each time that was wrong) */

#define DUAL_MSG_DIFFER  "Cable Link: games or settings differ, playing alone"
#define DUAL_MSG_PLAYERS "Cable Link: the consoles have a different number of players, playing alone"

enum { DS_OFF = 0, DS_HELLO, DS_WAIT, DS_PING, DS_TRANSFER, DS_READY, DS_RUN };

/* ---- one end of the cable (a SIO driver per GBA) ------------------------ */

#define DMODE_NORMAL   0
#define DMODE_MULTI    1
#define DMODE_UART     2
#define DMODE_GPIO     3
#define DMODE_JOYBUS   4

/* Wire state word (DEV_WIRES.v0) */
#define DW_SO          0x01   /* SO level this port drives */
#define DW_SC_DRIVE    0x02   /* GPIO: drives SC */
#define DW_SC          0x04
#define DW_SD_DRIVE    0x08   /* GPIO: drives SD */
#define DW_SD          0x10
#define DW_MODE_SH     8      /* bits 8..10: DMODE_* */

/* Transfer in progress on a port */
#define DX_NONE        0
#define DX_MP_PARENT   1
#define DX_MP_CHILD    2
#define DX_N_MASTER    3
#define DX_N_SLAVE     4

/* Events from one end of the cable to another. */
#define DEV_WIRES      1   /* v0: the sender's mode and pin levels */
#define DEV_MP_START   2   /* multiplayer transfer started (parent) */
#define DEV_N_START    3   /* normal mode transfer started (internal clock) */

typedef struct {
	u64 t;               /* When it happened (sender's session cycles) */
	u64 t_done;          /* *_START: when the transfer ends */
	u32 v0;              /* Data word */
	u32 seq;             /* *_START: transfer number */
	u8 kind;
	u8 a;                /* N_START: 8 or 32 bits */
	u8 from;             /* Slot of the sender */
} dual_event;

struct DualPort {
	struct GBASIODriver d;     /* (first: the driver is the port) */
	struct mTimingEvent syncEvent;
	struct mCore* core;
	struct GBA* gba;
	bool active;
	unsigned slot;
	unsigned n;                /* GBAs on the cable */
	u64 clock;                 /* Session cycles, at the last look */
	u32 lastTime;              /* mTimingCurrentTime then */
	u64 promised;              /* Nothing we do is stamped earlier than this */
	u64 pub;                   /* What we published last */

	u32 wires_sent;            /* Last wire word posted (~0: none) */
	u32 so_out;                /* Normal mode SO level (latched while busy) */
	u32 p_wires[NP];           /* The other ports, as last heard */
	u32 p_known;               /* (one bit per port) */

	u32 xfer;                  /* DX_* */
	u64 t_done;
	u32 seq;
	u32 bits;                  /* Normal mode: 8 or 32 */
	u32 word_rx;               /* Normal mode: what this port receives */
	u16 word_parent;           /* Multiplayer: the parent's word... */
	u16 word_own;              /* ...and ours (child) */
};

/* ---- state -------------------------------------------------------------- */

static struct {
	/* Frontend side */
	retro_environment_t environ_cb;
	void (*message)(const char* text, unsigned ms);
	void (*prepare)(void);
	retro_netpacket_send_t send_fn;
	retro_netpacket_poll_receive_t poll_fn;
	struct mCore* own;
	const void* rom;             /* The game, as the frontend gave it (or NULL) */
	size_t rom_size;
	char rom_path[1024];
	int rom_fd;                  /* Copy on write ROM for carts with GPIO */
	u32 cfg;                     /* Settings that change the machine */
	unsigned players;            /* GBAs on the cable in this session */
	unsigned cfg_players;        /* Core option (for the next session) */

	/* Hidden cores */
	struct {
		struct mCore* core;
		void* savedata;
		void* bios;
		bool th_ok;
		pthread_t th;
		sem_t start, done;
		volatile bool quit;
		unsigned slot;
	} hid[DUAL_HIDDEN];

	/* The ends of the cable, by slot */
	struct DualPort port[NP];

	/* What the own core had before the session */
	struct {
		bool saved;
		enum mRTCGenericType rtc_override;
		int64_t rtc_value;
		int idle_opt;
		bool skip_bios;
		bool opposing;
		void* rotation;
		void* lux;
		bool cheats[64];
		size_t ncheats;
	} own_saved;

	/* Session */
	int state;
	bool is_host;
	bool gave_up;                /* Nobody is linked for the rest of the session */
	unsigned my_id;              /* Our libretro client id (host: 0) */
	unsigned n;                  /* GBAs on the cable (once START is out) */
	unsigned my_slot;
	int slot_id[NP];             /* libretro client id of each slot's console */
	u64 t_state;                 /* When the current step started */
	u64 t_heard[NP];             /* Last packet from each slot */
	s64 rtc_epoch_ms;            /* RTC time at the start of the session */

	/* Host, before START: the clients that joined, in that order */
	struct {
		int id;
		bool hello;              /* Its HELLO came (and matched) */
		u64 t_join;
	} cand[NP - 1];
	unsigned ncand;
	u64 early_hello;             /* HELLOs before the frontend told us. Bit = id */

	/* Client, before START */
	bool hello_sent, hello_rcvd;
	u64 t_hello;
	bool go;

	/* Host: round trips to each client */
	u32 rtt[NP][DUAL_PINGS], rtt_n[NP];
	bool ping_out[NP];
	u64 t_ping[NP];

	u32 ow_us[NP];               /* One way time between each slot and the host */
	bool peer_ready;
	u32 delay;                   /* Input delay, frames (agreed at the start) */
	u32 send_delay;              /* Ours now: more after a peer waited */
	u32 stalls[NP];
	u32 stall_frame[NP];
	u32 peer_stalls[NP];
	u64 t_peer_stall;
	u64 t_lowered;
	u64 calm_us;
	bool reset_wanted;

	u8* own_blob;                /* Our GBA (savestate with the save memory) */
	u32 own_blob_len;
	u8* own_pack;                /* ...and the same run length coded */
	u32 own_pack_len;
	u8* peer_pack[NP];
	u32 peer_pack_len[NP], peer_rcvd[NP], peer_pack_cap[NP];
	bool loaded;                 /* The cable is plugged */

	u32 frame;
	u32 sent_upto;
	u32 own_keys[DUAL_RING];
	u32 keys[NP][DUAL_RING];
	u32 frame_ok[NP][DUAL_RING];
	u32 my_hash[DUAL_HASH_SLOTS][1 + NP];
	u32 peer_hash[NP][DUAL_HASH_SLOTS][1 + NP];

	/* Time sync */
	s64 peer_pos[NP][DUAL_SYNC_WIN];
	u32 peer_pos_n[NP];
	s32 peer_lead[NP];
	u32 sync_next;
} ds = { .players = 2, .cfg_players = 2, .rom_fd = -1 };

static __thread bool in_hidden_thread;

bool dual_hidden_thread(void) {
	return in_hidden_thread;
}

static u64 time_usec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64) ts.tv_sec * 1000000ULL + (u64) (ts.tv_nsec / 1000);
}

static u32 netorder32(u32 v) {
	const u8* b = (const u8*) &v;
	return ((u32) b[0] << 24) | ((u32) b[1] << 16) | ((u32) b[2] << 8) | b[3];
}

static void net_send(unsigned to, const void* buf, size_t len) {
	if (ds.send_fn) {
		ds.send_fn(RETRO_NETPACKET_RELIABLE | RETRO_NETPACKET_FLUSH_HINT, buf, len, (uint16_t) to);
	}
}

static void net_poll(void) {
	if (ds.poll_fn) {
		ds.poll_fn();
	}
}

/* To one console (libretro client id) or RETRO_NETPACKET_BROADCAST. */
static void dual_send(unsigned to, u32 type, const u32* words, u32 nwords, const void* data, u32 len) {
	static u8 buf[8 + 16 * 4 + DUAL_CHUNK];
	u32* hdr = (u32*) buf;
	u32 i;
	const u32 slot = ds.n ? ds.my_slot : DUAL_NO_SLOT;
	hdr[0] = netorder32(DUAL_HEADER);
	hdr[1] = netorder32(type | (slot << 8));
	for (i = 0; i < nwords; i++) {
		hdr[2 + i] = netorder32(words[i]);
	}
	if (len) {
		memcpy(&buf[8 + nwords * 4], data, len);
	}
	net_send(to, buf, 8 + nwords * 4 + len);
}

static void dual_send_all(u32 type, const u32* words, u32 nwords, const void* data, u32 len) {
	dual_send(RETRO_NETPACKET_BROADCAST, type, words, nwords, data, len);
}

static void dual_msg(const char* text) {
	if (ds.message) {
		ds.message(text, 3000);
	}
}

/* The hidden core for each other slot, in slot order. */
static unsigned hidden_slot(unsigned k) {
	return k < ds.my_slot ? k : k + 1;
}

static struct mCore* slot_core(unsigned s) {
	if (s == ds.my_slot) {
		return ds.own;
	}
	return ds.hid[s < ds.my_slot ? s : s - 1].core;
}

/* ---- the bus: the cable between the GBAs of this console ---------------- */

#define DUAL_QSIZE   2048   /* Events kept per pair of GBAs (a full queue would lose them) */
#define DUAL_REPLIES 64     /* Transfers a child's answer is kept for */

typedef u64 bus_clock __attribute__((aligned(8)));

static struct {
	bus_clock pub[NP];                    /* Published clocks (atomic) */
	unsigned n;
	dual_event q[NP][NP][DUAL_QSIZE];     /* Events FOR each slot, FROM each slot */
	u32 qhead[NP][NP], qcount[NP][NP];
	struct { u32 seq, word, valid; } rep[NP][DUAL_REPLIES];  /* Children's answers */
	pthread_mutex_t mtx;
	pthread_cond_t cond;
	int waiters;
	u64 notified[NP];
	volatile int abort;
} bus = { .mtx = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static void bus_reset(unsigned n) {
	pthread_mutex_lock(&bus.mtx);
	memset(bus.pub, 0, sizeof(bus.pub));
	memset(bus.qhead, 0, sizeof(bus.qhead));
	memset(bus.qcount, 0, sizeof(bus.qcount));
	memset(bus.rep, 0, sizeof(bus.rep));
	memset(bus.notified, 0, sizeof(bus.notified));
	bus.n = n;
	bus.abort = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	pthread_mutex_unlock(&bus.mtx);
}

/* Waking a sleeping thread up costs some tens of microseconds: a GBA wakes
 * the others up only every DUAL_NOTIFY_STEP cycles, or whenever it stops
 * itself (it waits, or its frame is over). This decides when the other
 * threads wake up, never what they compute. */
#define DUAL_NOTIFY_STEP 4096

static void bus_notify(unsigned slot, bool force) {
	if (__atomic_load_n(&bus.waiters, __ATOMIC_SEQ_CST)) {
		const u64 p = __atomic_load_n(&bus.pub[slot], __ATOMIC_SEQ_CST);
		if (force || p >= bus.notified[slot] + DUAL_NOTIFY_STEP) {
			bus.notified[slot] = p;
			pthread_mutex_lock(&bus.mtx);
			pthread_cond_broadcast(&bus.cond);
			pthread_mutex_unlock(&bus.mtx);
		}
	}
}

/* Whether a GBA at `clock` may go on. Nothing new from slot s is stamped
 * at or before pub[s]; what a child does is seen DUAL_LOOKAHEAD later, what
 * the parent does at once. So a GBA may go on when every event that could
 * be due by `clock` has been posted already. */
static bool bus_may_run(unsigned slot, u64 clock) {
	unsigned s;
	if (slot && __atomic_load_n(&bus.pub[0], __ATOMIC_SEQ_CST) < clock) {
		return false;
	}
	for (s = 1; s < bus.n; s++) {
		if (s != slot && __atomic_load_n(&bus.pub[s], __ATOMIC_SEQ_CST) + DUAL_LOOKAHEAD < clock) {
			return false;
		}
	}
	return true;
}

static bool bus_has_reply(unsigned from, u32 seq) {
	return __atomic_load_n(&bus.rep[from][seq % DUAL_REPLIES].valid, __ATOMIC_SEQ_CST) &&
	       __atomic_load_n(&bus.rep[from][seq % DUAL_REPLIES].seq, __ATOMIC_SEQ_CST) == seq;
}

/* Spin for a little while (the other GBAs run on the other CPU cores and
 * are usually about to get there), then sleep until woken up. */
#define BUS_WAIT(ready) \
	do { \
		unsigned spins; \
		for (spins = 0; !(ready) && !bus.abort; spins++) { \
			if (spins < 200) { \
				sched_yield(); \
			} else { \
				pthread_mutex_lock(&bus.mtx); \
				__atomic_add_fetch(&bus.waiters, 1, __ATOMIC_SEQ_CST); \
				while (!(ready) && !bus.abort) { \
					pthread_cond_wait(&bus.cond, &bus.mtx); \
				} \
				__atomic_sub_fetch(&bus.waiters, 1, __ATOMIC_SEQ_CST); \
				pthread_mutex_unlock(&bus.mtx); \
				break; \
			} \
		} \
	} while (0)

/* Publish how far we are (nothing new from us will be stamped at or before
 * `publish`), then wait until we may run at `clock` (0: never waits). */
static void bus_sync(unsigned slot, u64 publish, u64 clock) {
	bool ready;
	__atomic_store_n(&bus.pub[slot], publish, __ATOMIC_SEQ_CST);
	ready = !clock || bus_may_run(slot, clock);
	bus_notify(slot, !ready || !clock);
	if (!ready) {
		BUS_WAIT(bus_may_run(slot, clock));
	}
}

static void bus_post(unsigned from, unsigned to, const dual_event* ev) {
	DBG("[bus] slot %u posts kind %u to %u t %llu\n", from, ev->kind, to, (unsigned long long) ev->t);
	pthread_mutex_lock(&bus.mtx);
	if (bus.qcount[to][from] < DUAL_QSIZE) {
		dual_event* e = &bus.q[to][from][(bus.qhead[to][from] + bus.qcount[to][from]) % DUAL_QSIZE];
		*e = *ev;
		e->from = from;
		bus.qcount[to][from]++;
	}
	pthread_mutex_unlock(&bus.mtx);
}

/* Takes the event for `slot` due first, if it is due at `now`. (Ties go to
 * the lowest slot: the order never depends on the threads.) */
static bool bus_pop(unsigned slot, u64 now, dual_event* ev) {
	int best = -1;
	u64 best_due = 0;
	unsigned s;
	pthread_mutex_lock(&bus.mtx);
	for (s = 0; s < bus.n; s++) {
		if (s != slot && bus.qcount[slot][s]) {
			const dual_event* h = &bus.q[slot][s][bus.qhead[slot][s]];
			const u64 due = h->t + (s ? DUAL_LOOKAHEAD : 0);
			if (due <= now && (best < 0 || due < best_due)) {
				best = (int) s;
				best_due = due;
			}
		}
	}
	if (best >= 0) {
		*ev = bus.q[slot][best][bus.qhead[slot][best]];
		bus.qhead[slot][best] = (bus.qhead[slot][best] + 1) % DUAL_QSIZE;
		bus.qcount[slot][best]--;
	}
	pthread_mutex_unlock(&bus.mtx);
	return best >= 0;
}

static void bus_reply(unsigned from, u32 seq, u32 word, bool active) {
	pthread_mutex_lock(&bus.mtx);
	__atomic_store_n(&bus.rep[from][seq % DUAL_REPLIES].word, active ? word : 0xFFFF, __ATOMIC_SEQ_CST);
	__atomic_store_n(&bus.rep[from][seq % DUAL_REPLIES].seq, seq, __ATOMIC_SEQ_CST);
	__atomic_store_n(&bus.rep[from][seq % DUAL_REPLIES].valid, 1, __ATOMIC_SEQ_CST);
	pthread_cond_broadcast(&bus.cond);
	pthread_mutex_unlock(&bus.mtx);
}

static bool bus_wait_reply(unsigned slot, unsigned from, u32 seq, u32* word) {
	bus_notify(slot, true);
	BUS_WAIT(bus_has_reply(from, seq));
	if (!bus_has_reply(from, seq)) {
		return false;
	}
	*word = __atomic_load_n(&bus.rep[from][seq % DUAL_REPLIES].word, __ATOMIC_SEQ_CST);
	return true;
}

/* ---- a port: one end of the cable ----------------------------------------
 *
 * The cable is an AGB-005 Game Link Cable (GBATEK), for 2 to 4 GBAs:
 *  - The parent's SI is grounded, each child's SI is the SO of the one
 *    before it (slot - 1). SC and SD are common wires.
 *  - Multiplayer: only the parent starts. SIOMULTI0-3 read FFFF during the
 *    transfer and the word of each GBA at the end (FFFF where there is
 *    none), on every end.
 *  - Normal mode is for two: between the parent and the first child. The
 *    child gets the parent's bits, the parent always gets zero (its SI is
 *    grounded).
 *  - General purpose: the pins as wired above.
 *
 * TIME. Each port counts session cycles; every GBA starts the session at the
 * same time (0). Whatever a port does is stamped with its time (never before
 * what it published last), and the other ports apply it when their own
 * clock gets there, at their next look at the cable. */

static u64 port_now(struct DualPort* p) {
	return p->clock + (u32) (mTimingCurrentTime(&p->gba->timing) - p->lastTime);
}

static void port_tick(struct DualPort* p) {
	const u32 t = mTimingCurrentTime(&p->gba->timing);
	p->clock += (u32) (t - p->lastTime);
	p->lastTime = t;
}

/* Time stamp for something the game does now. */
static u64 port_io_time(struct DualPort* p) {
	return MAX(port_now(p), p->pub + 1);
}

static void port_publish(struct DualPort* p, u64 clock) {
	port_tick(p);
	p->pub = MAX(p->clock, p->promised);
	bus_sync(p->slot, p->pub, clock);
}

static u32 port_mode(struct DualPort* p) {
	switch (p->gba->sio.mode) {
	case GBA_SIO_NORMAL_8:
	case GBA_SIO_NORMAL_32:
		return DMODE_NORMAL;
	case GBA_SIO_MULTI:
		return DMODE_MULTI;
	case GBA_SIO_UART:
		return DMODE_UART;
	case GBA_SIO_GPIO:
		return DMODE_GPIO;
	case GBA_SIO_JOYBUS:
		return DMODE_JOYBUS;
	default:
		return DMODE_NORMAL;
	}
}

/* The other end of a normal mode cable: the parent and the first child. */
static int port_partner(struct DualPort* p) {
	return (p->slot < 2 && p->n >= 2) ? (int) (p->slot ^ 1) : -1;
}

/* What this port drives on the cable. */
static u32 port_wires(struct DualPort* p, u16 rcnt) {
	const u32 mode = port_mode(p);
	u32 w = mode << DW_MODE_SH;
	switch (mode) {
	case DMODE_NORMAL:
		w |= p->so_out ? DW_SO : 0;
		break;
	case DMODE_GPIO:
		/* An input pin is not driven: the other side sees the pull-up. */
		w |= (rcnt & 0x80) ? ((rcnt & 0x08) ? DW_SO : 0) : DW_SO;
		if (rcnt & 0x10) {
			w |= DW_SC_DRIVE | ((rcnt & 0x01) ? DW_SC : 0);
		}
		if (rcnt & 0x20) {
			w |= DW_SD_DRIVE | ((rcnt & 0x02) ? DW_SD : 0);
		}
		break;
	default:
		w |= DW_SO; /* Idle high */
		break;
	}
	return w;
}

static void port_post_all(struct DualPort* p, const dual_event* ev) {
	unsigned s;
	for (s = 0; s < p->n; s++) {
		if (s != p->slot) {
			bus_post(p->slot, s, ev);
		}
	}
}

static void port_post_wires(struct DualPort* p, u16 rcnt, u64 t) {
	const u32 w = port_wires(p, rcnt);
	if (w != p->wires_sent) {
		dual_event ev;
		memset(&ev, 0, sizeof(ev));
		ev.kind = DEV_WIRES;
		ev.t = t;
		ev.v0 = w;
		port_post_all(p, &ev);
		p->wires_sent = w;
	}
}

#define P_KNOWN(p, s) (((p)->p_known >> (s)) & 1)

/* Our SI: grounded on the parent end, the SO of the one before on a child. */
static u32 port_si(struct DualPort* p) {
	if (!p->slot) {
		return 0;
	}
	return P_KNOWN(p, p->slot - 1) ? ((p->p_wires[p->slot - 1] & DW_SO) != 0) : 1;
}

/* SD in multiplayer: high once every other GBA is in multiplayer mode. */
static bool port_all_multi(struct DualPort* p) {
	unsigned s;
	for (s = 0; s < p->n; s++) {
		if (s != p->slot && (!P_KNOWN(p, s) || ((p->p_wires[s] >> DW_MODE_SH) & 7) != DMODE_MULTI)) {
			return false;
		}
	}
	return p->n > 1;
}

/* A common wire (SC or SD) in general purpose mode: any GBA driving it low
 * pulls it low, otherwise the pull-up keeps it high. */
static u32 port_common_wire(struct DualPort* p, u32 drive, u32 level) {
	unsigned s;
	u32 v = 1;
	for (s = 0; s < p->n; s++) {
		if (s != p->slot && P_KNOWN(p, s) && (p->p_wires[s] & drive) && !(p->p_wires[s] & level)) {
			v = 0;
		}
	}
	return v;
}

/* The bits of SIOCNT the other ends drive. */
static u16 port_siocnt_inputs(struct DualPort* p, u16 siocnt) {
	const u32 si = port_si(p);
	switch (port_mode(p)) {
	case DMODE_NORMAL:
		siocnt = (siocnt & ~0x0004) | (si ? 0x0004 : 0);
		break;
	case DMODE_MULTI:
		siocnt = (siocnt & ~0x004C) | (si ? 0x0004 : 0) | (port_all_multi(p) ? 0x0008 : 0);
		break;
	default:
		break;
	}
	return siocnt;
}

/* The pins in RCNT (bits 0-3). Sets *irq on a general purpose SI falling
 * edge that asks for one. */
static u16 port_rcnt_inputs(struct DualPort* p, u16 rcnt, u16 old, bool* irq) {
	const u32 si = port_si(p);
	switch (port_mode(p)) {
	case DMODE_MULTI: {
		const bool busy = (p->gba->sio.siocnt & 0x0080) != 0;
		rcnt = (rcnt & ~0x000F) | (busy ? 0 : 0x1) | (port_all_multi(p) ? 0x2 : 0) | (si ? 0x4 : 0) | 0x8;
		break;
	}
	case DMODE_GPIO: {
		u32 v = rcnt & 0x0F;
		if (!(rcnt & 0x10)) { /* SC input */
			v = (v & ~1u) | port_common_wire(p, DW_SC_DRIVE, DW_SC);
		}
		if (!(rcnt & 0x20)) { /* SD input */
			v = (v & ~2u) | (port_common_wire(p, DW_SD_DRIVE, DW_SD) << 1);
		}
		if (!(rcnt & 0x40)) { /* SI input */
			v = (v & ~4u) | (si ? 4 : 0);
		}
		if (!(rcnt & 0x80)) { /* SO input: nothing drives it on this cable */
			v = (v & ~8u) | 8;
		}
		if ((old & 4) && !(v & 4) && (rcnt & 0x100) && irq) {
			*irq = true;
		}
		rcnt = (rcnt & ~0x000F) | v;
		break;
	}
	default:
		break;
	}
	return rcnt;
}

/* After the other ends changed something. */
static void port_refresh(struct DualPort* p) {
	struct GBASIO* sio = &p->gba->sio;
	bool irq = false;
	sio->siocnt = port_siocnt_inputs(p, sio->siocnt);
	sio->rcnt = port_rcnt_inputs(p, sio->rcnt, sio->rcnt, &irq);
	if (irq) {
		GBARaiseIRQ(p->gba, GBA_IRQ_SIO, 0);
	}
}

static void port_schedule_done(struct DualPort* p, u64 now) {
	struct GBASIO* sio = &p->gba->sio;
	const s64 until = (s64) p->t_done - (s64) now;
	mTimingDeschedule(&p->gba->timing, &sio->completeEvent);
	mTimingSchedule(&p->gba->timing, &sio->completeEvent, (int32_t) MAX(until, 0));
}

/* Something from another end is due now. */
static void port_apply(struct DualPort* p, const dual_event* ev, u64 now) {
	struct GBASIO* sio = &p->gba->sio;
	const u32 mode = port_mode(p);

	switch (ev->kind) {
	case DEV_WIRES:
		p->p_wires[ev->from] = ev->v0;
		p->p_known |= 1u << ev->from;
		port_refresh(p);
		break;

	case DEV_MP_START:
		/* Only the children get these. One takes part if it is in
		 * multiplayer mode and not busy; either way the parent gets an answer. */
		if (p->slot && mode == DMODE_MULTI && p->xfer == DX_NONE) {
			p->xfer = DX_MP_CHILD;
			p->seq = ev->seq;
			p->t_done = ev->t_done;
			p->word_parent = ev->v0;
			p->word_own = p->gba->memory.io[GBA_REG(SIOMLT_SEND)];
			p->gba->memory.io[GBA_REG(SIOMULTI0)] = 0xFFFF;
			p->gba->memory.io[GBA_REG(SIOMULTI1)] = 0xFFFF;
			p->gba->memory.io[GBA_REG(SIOMULTI2)] = 0xFFFF;
			p->gba->memory.io[GBA_REG(SIOMULTI3)] = 0xFFFF;
			sio->siocnt |= 0x0080; /* Busy */
			bus_reply(p->slot, ev->seq, p->word_own, true);
			port_refresh(p);
			port_schedule_done(p, now);
		} else {
			bus_reply(p->slot, ev->seq, 0xFFFF, false);
		}
		break;

	case DEV_N_START:
		/* The other end clocks a transfer: we get its bits if we wait for an
		 * external clock with the same word size. The parent always gets zero. */
		if (mode == DMODE_NORMAL && (sio->siocnt & 0x0081) == 0x0080 && p->xfer == DX_NONE &&
		    ((sio->siocnt & 0x1000) ? 32 : 8) == ev->a) {
			p->xfer = DX_N_SLAVE;
			p->bits = ev->a;
			p->word_rx = p->slot ? ev->v0 : 0;
			/* Our view of it is late by the lookahead when it comes from a child. */
			p->t_done = MAX(ev->t_done + (ev->from ? DUAL_LOOKAHEAD : 0), now);
			port_schedule_done(p, now);
		}
		break;
	}
}

/* Every DUAL_SYNC_STEP cycles: publish, wait for the others if need be,
 * and apply what is due. */
static void port_sync_event(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct DualPort* p = context;
	dual_event ev;
	UNUSED(cyclesLate);
	port_publish(p, p->clock + (u32) (mTimingCurrentTime(timing) - p->lastTime));
	while (bus_pop(p->slot, p->clock, &ev)) {
		port_apply(p, &ev, p->clock);
	}
	mTimingSchedule(timing, &p->syncEvent, DUAL_SYNC_STEP);
}

static void port_mp_start_parent(struct DualPort* p, u16 siocnt, u64 t) {
	dual_event ev;
	struct GBASIO* sio = &p->gba->sio;
	p->xfer = DX_MP_PARENT;
	p->t_done = t + GBASIOTransferCycles(GBA_SIO_MULTI, siocnt, p->n - 1);
	p->seq++;
	p->word_parent = p->gba->memory.io[GBA_REG(SIOMLT_SEND)];
	memset(&ev, 0, sizeof(ev));
	ev.kind = DEV_MP_START;
	ev.t = t;
	ev.t_done = p->t_done;
	ev.v0 = p->word_parent;
	ev.seq = p->seq;
	port_post_all(p, &ev);
	UNUSED(sio);
	port_schedule_done(p, port_now(p));
}

static void port_n_start_master(struct DualPort* p, u16 siocnt, u64 t) {
	dual_event ev;
	const bool is32 = p->gba->sio.mode == GBA_SIO_NORMAL_32;
	p->xfer = DX_N_MASTER;
	p->bits = is32 ? 32 : 8;
	p->t_done = t + GBASIOTransferCycles(p->gba->sio.mode, siocnt, 0);
	memset(&ev, 0, sizeof(ev));
	ev.kind = DEV_N_START;
	ev.t = t;
	ev.t_done = p->t_done;
	ev.v0 = is32 ? (p->gba->memory.io[GBA_REG(SIODATA32_LO)] | ((u32) p->gba->memory.io[GBA_REG(SIODATA32_HI)] << 16))
	             : (p->gba->memory.io[GBA_REG(SIODATA8)] & 0xFF);
	ev.a = p->bits;
	if (port_partner(p) >= 0) {
		bus_post(p->slot, (unsigned) port_partner(p), &ev);
	}
	port_schedule_done(p, port_now(p));
}

/* The transfer time is over: our word and the others'. */
static void port_finish(struct DualPort* p) {
	p->xfer = DX_NONE;
	port_refresh(p);
	port_post_wires(p, p->gba->sio.rcnt, port_io_time(p));
}

/* GBASIODriver */

static struct DualPort* port_of(struct GBASIODriver* d) {
	return (struct DualPort*) d;
}

static bool port_init(struct GBASIODriver* d) {
	UNUSED(d);
	return true;
}

static void port_deinit(struct GBASIODriver* d) {
	struct DualPort* p = port_of(d);
	if (p->gba) {
		mTimingDeschedule(&p->gba->timing, &p->syncEvent);
	}
	p->active = false;
}

static void port_reset(struct GBASIODriver* d) {
	/* The GBA restarts: the timing events are gone, dual_reset_all plugs the
	 * cable again (port_begin). */
	struct DualPort* p = port_of(d);
	p->xfer = DX_NONE;
	p->active = false; /* (its clock means nothing until port_begin) */
}

static uint32_t port_driver_id(const struct GBASIODriver* d) {
	UNUSED(d);
	return 0x6C617544; /* Dual */
}

static void port_set_mode(struct GBASIODriver* d, enum GBASIOMode mode) {
	struct DualPort* p = port_of(d);
	UNUSED(mode);
	if (!p->active) {
		return;
	}
	if (p->xfer != DX_NONE) {
		/* A transfer going on is lost (a child still gets its answer: it
		 * gave it when the transfer started) */
		mTimingDeschedule(&p->gba->timing, &p->gba->sio.completeEvent);
		p->gba->sio.siocnt &= ~0x0080;
		p->xfer = DX_NONE;
	}
	port_post_wires(p, p->gba->sio.rcnt, port_io_time(p));
}

static bool port_handles_mode(struct GBASIODriver* d, enum GBASIOMode mode) {
	UNUSED(d);
	UNUSED(mode);
	return true;
}

static int port_connected(struct GBASIODriver* d) {
	struct DualPort* p = port_of(d);
	return p->active && p->n > 1 ? (int) p->n - 1 : 0;
}

static int port_device_id(struct GBASIODriver* d) {
	return (int) port_of(d)->slot;
}

static uint16_t port_write_siocnt(struct GBASIODriver* d, uint16_t value) {
	struct DualPort* p = port_of(d);
	const u16 old = p->gba->sio.siocnt;
	u64 t;
	if (!p->active) {
		return value;
	}
	t = port_io_time(p);

	switch (port_mode(p)) {
	case DMODE_MULTI:
		/* Start is read only on the children; only the parent starts */
		if (p->slot || p->xfer != DX_NONE) {
			value = (value & ~0x0080) | (old & 0x0080);
		}
		value = port_siocnt_inputs(p, value);
		if (!p->slot && (value & 0x0080) && !(old & 0x0080) && p->xfer == DX_NONE) {
			port_mp_start_parent(p, value, t);
		}
		break;

	case DMODE_NORMAL:
		if (p->xfer != DX_NONE) {
			value |= 0x0080; /* Start stays set until done */
		}
		value = port_siocnt_inputs(p, value);
		if (!(value & 0x0080)) {
			p->so_out = (value & 0x0008) ? 1 : 0;
		}
		/* A transfer runs whenever start is set with the internal clock:
		 * games arm the port first and switch the clock source with start
		 * still set. */
		if ((value & 0x0081) == 0x0081 && p->xfer == DX_NONE && (old & 0x0081) != 0x0081) {
			port_n_start_master(p, value, t);
		}
		break;

	default:
		break; /* UART / GPIO / JOY BUS: just stored */
	}

	port_post_wires(p, p->gba->sio.rcnt, t);
	return value;
}

static uint16_t port_write_rcnt(struct GBASIODriver* d, uint16_t value) {
	struct DualPort* p = port_of(d);
	bool irq = false;
	u64 t;
	if (!p->active) {
		return value;
	}
	t = port_io_time(p);
	value = port_rcnt_inputs(p, value, p->gba->sio.rcnt, &irq);
	port_post_wires(p, value, t);
	if (irq) {
		GBARaiseIRQ(p->gba, GBA_IRQ_SIO, 0);
	}
	return value;
}

static bool port_start(struct GBASIODriver* d) {
	/* The transfers are started in port_write_siocnt (it sees the value
	 * written) and their end scheduled there. */
	UNUSED(d);
	return false;
}

static void port_finish_multi(struct GBASIODriver* d, uint16_t data[4]) {
	struct DualPort* p = port_of(d);
	unsigned s;
	if (!p->active || (p->xfer != DX_MP_PARENT && p->xfer != DX_MP_CHILD)) {
		memset(data, 0xFF, sizeof(uint16_t) * 4);
		return;
	}
	/* Each child answered this transfer when it got to its start (none is
	 * ever that far behind): wait for the answers, whenever they get here
	 * they are the same ones. */
	port_publish(p, 0);
	data[0] = p->word_parent;
	for (s = 1; s < 4; s++) {
		u32 w = 0xFFFF;
		if (s == p->slot) {
			w = p->word_own;
		} else if (s < p->n && !bus_wait_reply(p->slot, s, p->seq, &w)) {
			w = 0xFFFF;
		}
		data[s] = (u16) w;
	}
	port_finish(p);
}

static u32 port_normal_rx(struct DualPort* p) {
	if (p->xfer == DX_N_SLAVE) {
		return p->word_rx;
	}
	if (p->xfer != DX_N_MASTER) {
		return 0xFFFFFFFF;
	}
	if (!p->slot) {
		return 0; /* SI grounded */
	}
	return port_si(p) ? 0xFFFFFFFF : 0; /* The idle SO before us */
}

static uint8_t port_finish_normal8(struct GBASIODriver* d) {
	struct DualPort* p = port_of(d);
	const u32 rx = port_normal_rx(p);
	if (p->active) {
		p->so_out = (p->gba->sio.siocnt & 0x0008) ? 1 : 0; /* SO back to bit 3 */
		port_finish(p);
	}
	return rx & 0xFF;
}

static uint32_t port_finish_normal32(struct GBASIODriver* d) {
	struct DualPort* p = port_of(d);
	const u32 rx = port_normal_rx(p);
	if (p->active) {
		p->so_out = (p->gba->sio.siocnt & 0x0008) ? 1 : 0;
		port_finish(p);
	}
	return rx;
}

/* The cable is plugged (at the start of the session, or after a reset):
 * the clock starts at 0 on every GBA. */
static void port_begin(unsigned slot, struct mCore* core) {
	struct DualPort* p = &ds.port[slot];
	struct GBA* gba = core->board;
	memset(p, 0, sizeof(*p));
	p->d.init = port_init;
	p->d.deinit = port_deinit;
	p->d.reset = port_reset;
	p->d.driverId = port_driver_id;
	p->d.setMode = port_set_mode;
	p->d.handlesMode = port_handles_mode;
	p->d.connectedDevices = port_connected;
	p->d.deviceId = port_device_id;
	p->d.writeSIOCNT = port_write_siocnt;
	p->d.writeRCNT = port_write_rcnt;
	p->d.start = port_start;
	p->d.finishMultiplayer = port_finish_multi;
	p->d.finishNormal8 = port_finish_normal8;
	p->d.finishNormal32 = port_finish_normal32;
	p->syncEvent.context = p;
	p->syncEvent.callback = port_sync_event;
	p->syncEvent.name = "Dual Cable Link";
	p->syncEvent.priority = 0x90;
	p->core = core;
	p->gba = gba;
	p->slot = slot;
	p->n = bus.n;
	p->lastTime = mTimingCurrentTime(&gba->timing);
	p->wires_sent = ~0U;

	core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, &p->d);
	p->active = true;

	/* Anything that was going on is lost, the cable was just plugged in. */
	mTimingDeschedule(&gba->timing, &gba->sio.completeEvent);
	gba->sio.siocnt &= ~0x0080;
	p->so_out = (gba->sio.siocnt & 0x0008) ? 1 : 0;
	if (gba->sio.mode == GBA_SIO_MULTI) {
		gba->sio.siocnt = GBASIOMultiplayerSetSlave(gba->sio.siocnt, slot != 0);
		gba->sio.siocnt = GBASIOMultiplayerSetId(gba->sio.siocnt, slot);
	}
	port_refresh(p);
	port_post_wires(p, gba->sio.rcnt, 0);
	mTimingDeschedule(&gba->timing, &p->syncEvent);
	mTimingSchedule(&gba->timing, &p->syncEvent, DUAL_SYNC_STEP);
}

static void port_end(unsigned slot) {
	struct DualPort* p = &ds.port[slot];
	if (p->core) {
		p->core->setPeripheral(p->core, mPERIPH_GBA_LINK_PORT, NULL);
	}
	p->active = false;
	p->core = NULL;
	p->gba = NULL;
}

/* Our frame is over: we stop until the next one, and the other GBAs may
 * need to go a few cycles further than us to finish theirs. We promise that
 * nothing we do from now on is stamped before a scanline later, so that
 * they can get there without waiting for our next frame. */
static void port_frame_end(unsigned slot) {
	struct DualPort* p = &ds.port[slot];
	port_tick(p);
	p->promised = MAX(p->promised, p->clock + DUAL_FRAME_SLACK);
	port_publish(p, 0);
}

/* ---- hidden cores ---------------------------------------------------------- */

static void* b_thread_main(void* arg) {
	const unsigned k = (unsigned) (uintptr_t) arg;
	in_hidden_thread = true;
	while (1) {
		sem_wait(&ds.hid[k].start);
		if (ds.hid[k].quit) {
			break;
		}
		ds.hid[k].core->runFrame(ds.hid[k].core);
		port_frame_end(ds.hid[k].slot);
		mAudioBufferClear(ds.hid[k].core->getAudioBuffer(ds.hid[k].core));
		sem_post(&ds.hid[k].done);
	}
	return NULL;
}

/* A file with the game, that each hidden core maps copy on write: carts
 * with GPIO (RTC, rumble, ...) write the pins into the ROM. */
static int rom_fd_for_gpio(void) {
	if (ds.rom_fd >= 0) {
		return ds.rom_fd;
	}
#ifdef SYS_memfd_create
	if (ds.rom) {
		int fd = (int) syscall(SYS_memfd_create, "mgba-rom", 0);
		if (fd >= 0) {
			const u8* src = ds.rom;
			size_t left = ds.rom_size;
			while (left) {
				ssize_t w = write(fd, src, left);
				if (w <= 0) {
					close(fd);
					return -1;
				}
				src += w;
				left -= (size_t) w;
			}
			ds.rom_fd = fd;
		}
	}
#endif
	return ds.rom_fd;
}

static struct VFile* b_rom_vf(unsigned k) {
	struct GBA* gba = ds.own->board;
	const bool gpio = (gba->memory.hw.devices & HW_GPIO) != 0;
	UNUSED(k);
	if (!ds.rom) {
		/* Mapped copy on write from the file (MAP_PRIVATE) */
		return VFileOpen(ds.rom_path, O_RDONLY);
	}
	if (!gpio) {
		/* Nothing writes into the ROM: every GBA reads the frontend's copy */
		return VFileFromConstMemory(ds.rom, ds.rom_size);
	}
	if (rom_fd_for_gpio() >= 0) {
		int fd = dup(ds.rom_fd);
		if (fd >= 0) {
			struct VFile* vf = VFileFromFD(fd);
			if (vf) {
				return vf;
			}
			close(fd);
		}
	}
	/* A copy of its own (freed with the VFile) */
	return VFileMemChunk(ds.rom, ds.rom_size);
}

static void b_unload(unsigned k) {
	if (ds.hid[k].th_ok) {
		ds.hid[k].quit = true;
		sem_post(&ds.hid[k].start);
		pthread_join(ds.hid[k].th, NULL);
		sem_destroy(&ds.hid[k].start);
		sem_destroy(&ds.hid[k].done);
		ds.hid[k].th_ok = false;
	}
	if (ds.hid[k].core) {
		struct mCore* h = ds.hid[k].core;
		mCoreConfigDeinit(&h->config);
		h->deinit(h);
		ds.hid[k].core = NULL;
	}
	if (ds.hid[k].savedata) {
		mappedMemoryFree(ds.hid[k].savedata, GBA_SIZE_FLASH1M);
		ds.hid[k].savedata = NULL;
	}
	free(ds.hid[k].bios);
	ds.hid[k].bios = NULL;
}

static bool b_load(unsigned k) {
	struct mCore* h;
	struct GBA* own_gba = ds.own->board;
	struct VFile* vf;
	struct mCoreOptions opts = {
		.useBios = ds.own->opts.useBios,
		.skipBios = true,
		.volume = 0x100,
	};
	const char* idle = mCoreConfigGetValue(&ds.own->config, "idleOptimization");
	const char* gbp = mCoreConfigGetValue(&ds.own->config, "gba.forceGbp");
	if (ds.hid[k].core) {
		return true;
	}

	h = GBACoreCreate();
	if (!h) {
		return false;
	}
	mCoreInitConfig(h, NULL);
	if (!h->init(h)) {
		mCoreConfigDeinit(&h->config);
		free(h);
		return false;
	}
	if (idle) {
		mCoreConfigSetDefaultValue(&h->config, "idleOptimization", idle);
	}
	if (gbp) {
		mCoreConfigSetDefaultValue(&h->config, "gba.forceGbp", gbp);
	}
	mCoreConfigLoadDefaults(&h->config, &opts);
	mCoreLoadConfig(h);
	ds.hid[k].core = h;

	vf = b_rom_vf(k);
	if (!vf || !h->loadROM(h, vf)) {
		if (vf) {
			vf->close(vf);
		}
		b_unload(k);
		return false;
	}

	/* The same BIOS as the own GBA (the built-in one, or the file it read) */
	if (own_gba->memory.fullBios) {
		ds.hid[k].bios = malloc(GBA_SIZE_BIOS);
		if (ds.hid[k].bios) {
			memcpy(ds.hid[k].bios, own_gba->memory.bios, GBA_SIZE_BIOS);
			h->loadBIOS(h, VFileFromMemory(ds.hid[k].bios, GBA_SIZE_BIOS), 0);
		}
	}

	ds.hid[k].savedata = anonymousMemoryMap(GBA_SIZE_FLASH1M);
	if (!ds.hid[k].savedata) {
		b_unload(k);
		return false;
	}
	memset(ds.hid[k].savedata, 0xFF, GBA_SIZE_FLASH1M);
	h->loadSave(h, VFileFromMemory(ds.hid[k].savedata, GBA_SIZE_FLASH1M));
	h->setAudioBufferSize(h, 1024);
	h->reset(h);

	if (!ds.hid[k].th_ok) {
		sem_init(&ds.hid[k].start, 0, 0);
		sem_init(&ds.hid[k].done, 0, 0);
		ds.hid[k].quit = false;
		ds.hid[k].th_ok = !pthread_create(&ds.hid[k].th, NULL, b_thread_main, (void*) (uintptr_t) k);
		if (!ds.hid[k].th_ok) {
			sem_destroy(&ds.hid[k].start);
			sem_destroy(&ds.hid[k].done);
			b_unload(k);
			return false;
		}
	}
	return true;
}

/* The hidden cores for `players` (only as many as the option asks for). */
static bool b_load_for(unsigned players) {
	unsigned k;
	for (k = 0; k + 1 < players; k++) {
		if (!b_load(k)) {
			return false;
		}
	}
	return true;
}

/* ---- agreement checks ------------------------------------------------- */

static u32 hash_mem(u32 h, const void* data, size_t len) {
	const u8* p = data;
	size_t i;
	for (i = 0; i + 4 <= len; i += 4) {
		u32 w;
		memcpy(&w, &p[i], 4);
		h = (h ^ w) * 16777619u;
		h ^= h >> 15;
	}
	for (; i < len; i++) {
		h = (h ^ p[i]) * 16777619u;
	}
	return h;
}

/* Everything the game can see. */
static u32 hash_slot(unsigned s) {
	struct mCore* c = slot_core(s);
	struct GBA* gba;
	struct ARMCore* cpu;
	u32 h = 2166136261u;
	u16 sio[2];
	if (!c) {
		return 0;
	}
	gba = c->board;
	cpu = c->cpu;
	h = hash_mem(h, gba->memory.wram, GBA_SIZE_EWRAM);
	h = hash_mem(h, gba->memory.iwram, GBA_SIZE_IWRAM);
	h = hash_mem(h, gba->video.vram, GBA_SIZE_VRAM);
	h = hash_mem(h, gba->video.oam.raw, GBA_SIZE_OAM);
	h = hash_mem(h, gba->video.palette, GBA_SIZE_PALETTE_RAM);
	h = hash_mem(h, gba->memory.io, sizeof(gba->memory.io));
	sio[0] = gba->sio.siocnt;
	sio[1] = gba->sio.rcnt;
	h = hash_mem(h, sio, sizeof(sio));
	h = hash_mem(h, cpu->gprs, sizeof(cpu->gprs));
	h = hash_mem(h, &cpu->cpsr.packed, sizeof(cpu->cpsr.packed));
	return h;
}

/* ---- run length coding ---------------------------------------------------
 * After the restart the states are mostly zeros (and save memory mostly FF).
 * A control byte c < 80 is followed by c + 1 bytes as they are; otherwise
 * the next byte is there (c & 7F) + 3 times. */

static u32 rle_pack(u8* dst, const u8* src, u32 len) {
	u32 i = 0, lit = 0, o = 0;
	while (i <= len) {
		u32 run = 0;
		if (i < len) {
			for (run = 1; i + run < len && run < 130 && src[i + run] == src[i]; run++);
		}
		if (i == len || run >= 3) {
			while (lit < i) {
				const u32 n = MIN(128, i - lit);
				dst[o++] = n - 1;
				memcpy(&dst[o], &src[lit], n);
				o += n;
				lit += n;
			}
			if (i == len) {
				break;
			}
			dst[o++] = 0x80 | (run - 3);
			dst[o++] = src[i];
			lit = i + run;
		}
		i += run;
	}
	return o;
}

/* Returns the unpacked length, or 0. */
static u32 rle_unpack(u8* dst, u32 cap, const u8* src, u32 slen) {
	u32 i = 0, o = 0;
	while (i < slen) {
		const u32 c = src[i++];
		if (c < 0x80) {
			if (i + c + 1 > slen || o + c + 1 > cap) {
				return 0;
			}
			memcpy(&dst[o], &src[i], c + 1);
			i += c + 1;
			o += c + 1;
		} else {
			if (i >= slen || o + (c & 0x7F) + 3 > cap) {
				return 0;
			}
			memset(&dst[o], src[i++], (c & 0x7F) + 3);
			o += (c & 0x7F) + 3;
		}
	}
	return o;
}

#define DUAL_BLOB_MAX (DUAL_STATE_MAX)

/* ---- the own GBA while linked -------------------------------------------- */

/* Everything that would make the own GBA another machine than its copies
 * is put aside for the session: the copies have no sensors, no cheats, a
 * clock that every console agrees on, no idle loop detection (it learns
 * from the history of each GBA) and skip the BIOS intro on a reset. */
static void own_link_begin(void) {
	struct mCore* c = ds.own;
	struct GBA* gba = c->board;
	struct mCheatDevice* dev = c->cheatDevice ? c->cheatDevice(c) : NULL;
	size_t i;
	if (ds.own_saved.saved) {
		return;
	}
	ds.own_saved.saved = true;
	ds.own_saved.rtc_override = c->rtc.override;
	ds.own_saved.rtc_value = c->rtc.value;
	ds.own_saved.idle_opt = gba->idleOptimization;
	ds.own_saved.skip_bios = c->opts.skipBios;
	ds.own_saved.opposing = gba->allowOpposingDirections;
	ds.own_saved.rotation = gba->rotationSource;
	ds.own_saved.lux = gba->luminanceSource;
	ds.own_saved.ncheats = 0;
	if (dev) {
		for (i = 0; i < mCheatSetsSize(&dev->cheats) && i < 64; i++) {
			struct mCheatSet* set = *mCheatSetsGetPointer(&dev->cheats, i);
			ds.own_saved.cheats[i] = set->enabled;
			set->enabled = false;
			mCheatRefresh(dev, set); /* (takes the ROM patches out) */
		}
		ds.own_saved.ncheats = i;
	}
	c->opts.skipBios = true;
	gba->idleLoop = GBA_IDLE_LOOP_NONE; /* (the reset puts the known one back) */
	gba->idleOptimization = IDLE_LOOP_REMOVE;
	gba->allowOpposingDirections = true; /* (each console filters its own keys) */
	c->setPeripheral(c, mPERIPH_ROTATION, NULL);
	c->setPeripheral(c, mPERIPH_GBA_LUMINANCE, NULL);
}

static void own_link_end(void) {
	struct mCore* c = ds.own;
	struct GBA* gba;
	struct mCheatDevice* dev;
	size_t i;
	if (!ds.own_saved.saved || !c) {
		return;
	}
	gba = c->board;
	ds.own_saved.saved = false;
	c->rtc.override = ds.own_saved.rtc_override;
	c->rtc.value = ds.own_saved.rtc_value;
	gba->idleOptimization = ds.own_saved.idle_opt;
	c->opts.skipBios = ds.own_saved.skip_bios;
	gba->allowOpposingDirections = ds.own_saved.opposing;
	c->setPeripheral(c, mPERIPH_ROTATION, ds.own_saved.rotation);
	c->setPeripheral(c, mPERIPH_GBA_LUMINANCE, ds.own_saved.lux);
	dev = c->cheatDevice ? c->cheatDevice(c) : NULL;
	if (dev) {
		for (i = 0; i < mCheatSetsSize(&dev->cheats) && i < ds.own_saved.ncheats; i++) {
			struct mCheatSet* set = *mCheatSetsGetPointer(&dev->cheats, i);
			set->enabled = ds.own_saved.cheats[i];
			mCheatRefresh(dev, set);
		}
	}
}

/* Settings of the hidden cores for the session (see own_link_begin). */
static void hidden_link_begin(struct mCore* h) {
	struct GBA* gba = h->board;
	gba->idleOptimization = IDLE_LOOP_REMOVE;
	gba->allowOpposingDirections = true;
	h->opts.skipBios = true;
}

/* ---- session ---------------------------------------------------------- */

static void dual_free_blobs(void) {
	unsigned s;
	free(ds.own_blob);
	free(ds.own_pack);
	ds.own_blob = ds.own_pack = NULL;
	for (s = 0; s < NP; s++) {
		free(ds.peer_pack[s]);
		ds.peer_pack[s] = NULL;
		ds.peer_pack_cap[s] = 0;
	}
}

/* Back to a single GBA: the player's one goes on, unplugged. */
static void dual_stop(const char* why) {
	unsigned s;
	if (ds.state == DS_OFF) {
		return;
	}
	if (why) {
		dual_msg(why);
	}
	ds.state = DS_OFF;
	dual_free_blobs();
	if (ds.loaded) {
		bus.abort = 1;
		for (s = 0; s < ds.n; s++) {
			port_end(s);
		}
		ds.loaded = false;
	}
	own_link_end();
}

/* Nobody is linked for the rest of this session. */
static void dual_give_up(const char* why) {
	if (ds.is_host) {
		u32 w[1] = { why && !strcmp(why, DUAL_MSG_PLAYERS) };
		dual_send_all(DPK_GIVEUP, w, 1, NULL, 0);
	} else if (ds.state == DS_HELLO || ds.state == DS_WAIT) {
		dual_send(0, DPK_BYE, NULL, 0, NULL, 0);
	}
	dual_stop(NULL);
	ds.state = DS_OFF;
	ds.gave_up = true;
	if (why) {
		dual_msg(why);
	}
}

static u32 dual_ow(unsigned a, unsigned b) {
	return ds.ow_us[a] + ds.ow_us[b];
}

static void dual_count_stall(unsigned s, u32 f) {
	if (ds.stall_frame[s] != f + 1) {
		ds.stall_frame[s] = f + 1;
		ds.stalls[s]++;
	}
}

/* Which game this is (mGBA works its CRC out when it loads it). */
static u32 dual_rom_id(void) {
	struct GBA* gba = ds.own->board;
	return gba->romCrc32 ^ (u32) (gba->pristineRomSize * 2654435761u);
}

static u32 dual_bios_id(void) {
	struct GBA* gba = ds.own->board;
	return doCrc32(gba->memory.bios, GBA_SIZE_BIOS) ^ (gba->memory.fullBios ? 0x80000000 : 0);
}

/* Everything else that makes this GBA a different machine. */
static u32 dual_machine_cfg(void) {
	struct GBA* gba = ds.own->board;
	const char* gbp = mCoreConfigGetValue(&ds.own->config, "gba.forceGbp");
	u32 cfg = 2166136261u;
	const u32 v[] = {
		(u32) gba->vbaBugCompat,
		(u32) (gbp && strcmp(gbp, "0") != 0),
		doCrc32(projectVersion, strlen(projectVersion)),
	};
	size_t i;
	for (i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		cfg = (cfg ^ v[i]) * 16777619u;
	}
	return cfg;
}

static void dual_send_hello(unsigned to) {
	u32 w[6];
	w[0] = DUAL_VERSION;
	w[1] = dual_rom_id();
	w[2] = dual_bios_id();
	w[3] = ds.cfg;
	w[4] = ds.players;
	w[5] = ds.is_host ? 1 : 0;
	dual_send(to, DPK_HELLO, w, 6, NULL, 0);
	ds.hello_sent = true;
	ds.t_hello = time_usec();
}

static bool dual_hello_matches(const u32* w) {
	return w[0] == DUAL_VERSION && w[1] == dual_rom_id() && w[2] == dual_bios_id() && w[3] == ds.cfg &&
	       w[4] == ds.players && w[5] == (ds.is_host ? 0u : 1u);
}

static void dual_send_ping(unsigned s) {
	u32 w[2];
	const u64 now = time_usec();
	w[0] = (u32) now;
	w[1] = (u32) (now >> 32);
	dual_send(ds.slot_id[s], DPK_PING, w, 2, NULL, 0);
	ds.ping_out[s] = true;
	ds.t_ping[s] = now;
}

static u32 dual_rtt_min(unsigned s) {
	u32 i, m = ~0U;
	for (i = 0; i < MIN(ds.rtt_n[s], DUAL_PINGS); i++) {
		m = MIN(m, ds.rtt[s][i]);
	}
	return ds.rtt_n[s] ? m : 0;
}

/* Our GBA starts over (with the session's settings) and the copy that is
 * sent to the others is made. */
static bool dual_capture(void) {
	struct VFile* vf;
	ssize_t size;
	struct mCore* c = ds.own;

	if (ds.prepare) {
		ds.prepare(); /* (the save memory, if the frontend did not give it yet) */
	}
	own_link_begin();
	c->rtc.override = RTC_FAKE_EPOCH;
	c->rtc.value = ds.rtc_epoch_ms;
	c->reset(c);

	vf = VFileMemChunk(NULL, 0);
	if (!vf) {
		return false;
	}
	if (!mCoreSaveStateNamed(c, vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC)) {
		vf->close(vf);
		return false;
	}
	size = vf->size(vf);
	if (size <= 0 || size > DUAL_BLOB_MAX) {
		vf->close(vf);
		return false;
	}
	ds.own_blob_len = (u32) size;
	ds.own_blob = malloc(ds.own_blob_len);
	ds.own_pack = malloc(ds.own_blob_len + ds.own_blob_len / 128 + 16);
	if (!ds.own_blob || !ds.own_pack) {
		vf->close(vf);
		return false;
	}
	vf->seek(vf, 0, SEEK_SET);
	vf->read(vf, ds.own_blob, ds.own_blob_len);
	vf->close(vf);
	ds.own_pack_len = rle_pack(ds.own_pack, ds.own_blob, ds.own_blob_len);
	return true;
}

static void dual_send_blob(void) {
	u32 off;
	for (off = 0; off < ds.own_pack_len; off += DUAL_CHUNK) {
		u32 w[2] = { off, ds.own_pack_len };
		u32 len = MIN(DUAL_CHUNK, ds.own_pack_len - off);
		dual_send_all(DPK_STATE, w, 2, &ds.own_pack[off], len);
	}
	free(ds.own_pack);
	ds.own_pack = NULL;
}

static bool dual_have_states(void) {
	unsigned s;
	for (s = 0; s < ds.n; s++) {
		if (s != ds.my_slot && (!ds.peer_pack_len[s] || ds.peer_rcvd[s] < ds.peer_pack_len[s])) {
			return false;
		}
	}
	return true;
}

static bool load_blob(struct mCore* c, const u8* blob, u32 len) {
	struct VFile* vf = VFileFromConstMemory(blob, len);
	bool ok;
	if (!vf) {
		return false;
	}
	ok = mCoreLoadStateNamed(c, vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
	vf->close(vf);
	return ok;
}

/* Every GBA starts from the states that were exchanged: ours from the copy
 * we sent (so that every copy of it starts the same way), the others from
 * what we got. Then the cable is plugged. */
static bool dual_load_machines(void) {
	u8* blob;
	unsigned k, s;

	if (!load_blob(ds.own, ds.own_blob, ds.own_blob_len)) {
		return false;
	}
	blob = malloc(DUAL_BLOB_MAX);
	if (!blob) {
		return false;
	}
	for (k = 0; k + 1 < ds.n; k++) {
		const unsigned slot = hidden_slot(k);
		u32 len = rle_unpack(blob, DUAL_BLOB_MAX, ds.peer_pack[slot], ds.peer_pack_len[slot]);
		/* (A restart first: a hidden core kept from an earlier session would
		 * carry what a savestate leaves out, the own GBA does not.) */
		hidden_link_begin(ds.hid[k].core);
		ds.hid[k].core->reset(ds.hid[k].core);
		if (!len || !load_blob(ds.hid[k].core, blob, len)) {
			free(blob);
			return false;
		}
		ds.hid[k].slot = slot;
	}
	free(blob);

	bus_reset(ds.n);
	for (s = 0; s < ds.n; s++) {
		port_begin(s, slot_core(s));
	}
	dual_free_blobs();
	ds.loaded = true;
	return true;
}

/* A new session: forget everything about the last one. */
static void dual_session_reset(void) {
	const u64 now = time_usec();
	unsigned s;
	dual_free_blobs();
	ds.n = 0;
	ds.ncand = 0;
	ds.early_hello = 0;
	ds.gave_up = false;
	ds.hello_sent = ds.hello_rcvd = ds.go = ds.peer_ready = false;
	memset(ds.peer_pack_len, 0, sizeof(ds.peer_pack_len));
	memset(ds.peer_rcvd, 0, sizeof(ds.peer_rcvd));
	memset(ds.rtt_n, 0, sizeof(ds.rtt_n));
	memset(ds.ping_out, 0, sizeof(ds.ping_out));
	memset(ds.ow_us, 0, sizeof(ds.ow_us));
	memset(ds.keys, 0, sizeof(ds.keys));
	memset(ds.frame_ok, 0, sizeof(ds.frame_ok));
	memset(ds.my_hash, 0, sizeof(ds.my_hash));
	memset(ds.peer_hash, 0, sizeof(ds.peer_hash));
	memset(ds.peer_pos_n, 0, sizeof(ds.peer_pos_n));
	memset(ds.peer_lead, 0, sizeof(ds.peer_lead));
	for (s = 0; s < NP; s++) {
		ds.slot_id[s] = -1;
		ds.t_heard[s] = now;
	}
	ds.t_state = now;
}

static void dual_start_running(void) {
	u32 i;
	unsigned s;
	char msg[96];
	ds.state = DS_RUN;
	ds.frame = 0;
	/* Nobody presses anything during the first frames (the delay). */
	for (i = 0; i < ds.delay; i++) {
		ds.own_keys[i] = 0;
		for (s = 0; s < NP; s++) {
			ds.keys[s][i] = 0;
			ds.frame_ok[s][i] = i + 1;
		}
	}
	ds.sent_upto = ds.delay;
	ds.send_delay = ds.delay;
	memset(ds.stalls, 0, sizeof(ds.stalls));
	memset(ds.stall_frame, 0, sizeof(ds.stall_frame));
	memset(ds.peer_stalls, 0, sizeof(ds.peer_stalls));
	ds.t_peer_stall = time_usec();
	ds.t_lowered = 0;
	ds.calm_us = DUAL_DELAY_CALM_US;
	ds.sync_next = DUAL_SYNC_GAP;
	ds.reset_wanted = false;
	for (s = 0; s < NP; s++) {
		ds.t_heard[s] = time_usec();
	}
	if (ds.n == 2) {
		snprintf(msg, sizeof(msg), "Cable Link: both GBAs connected (%u frames of delay)", (unsigned) ds.delay);
	} else {
		snprintf(msg, sizeof(msg), "Cable Link: %u GBAs connected (%u frames of delay)", ds.n, (unsigned) ds.delay);
	}
	dual_msg(msg);
}

/* Input delay from the round trips: the buttons must get to every other
 * console before it needs them, and between two clients they go through
 * the host, which only passes them on when the frontend reads the network
 * (once a frame: up to a frame more). The time sync keeps the consoles
 * within DUAL_SYNC_US of each other; on top of that the network takes one
 * way, and a frame more covers it having a bad moment. */
static u32 dual_pick_delay(void) {
	u32 a = 0, b = ds.n > 2 ? DUAL_FRAME_US : 0, d;
	u32 ow_b = 0;
	unsigned s;
	for (s = 1; s < ds.n; s++) {
		const u32 ow = ds.ow_us[s];
		if (ow > a) {
			ow_b = a;
			a = ow;
		} else if (ow > ow_b) {
			ow_b = ow;
		}
	}
	d = (a + ow_b + b + DUAL_SYNC_US + DUAL_FRAME_US + DUAL_FRAME_US - 1) / DUAL_FRAME_US;
	return MAX(2, MIN(d, 30));
}

static void dual_waiting_msg(unsigned have) {
	char msg[96];
	snprintf(msg, sizeof(msg), "Cable Link: waiting for players (%u/%u)", have, ds.players);
	dual_msg(msg);
}

static void dual_cand_ready(void) {
	u32 wt[2] = { ds.ncand + 1, ds.players };
	if (ds.ncand + 1 < ds.players) {
		dual_send_all(DPK_WAITING, wt, 2, NULL, 0);
		dual_waiting_msg(ds.ncand + 1);
	}
}

/* Host: everybody is here. Each client gets its slot, in the order they came. */
static void dual_host_start(void) {
	u32 w[1 + NP];
	unsigned s;
	ds.n = ds.ncand + 1;
	ds.my_slot = 0;
	ds.slot_id[0] = 0;
	for (s = 1; s < ds.n; s++) {
		ds.slot_id[s] = ds.cand[s - 1].id;
	}
	w[0] = ds.n;
	for (s = 0; s < NP; s++) {
		w[1 + s] = (u32) ds.slot_id[s];
	}
	dual_send_all(DPK_START, w, 1 + NP, NULL, 0);
	ds.state = DS_PING;
	ds.t_state = time_usec();
	for (s = 1; s < ds.n; s++) {
		dual_send_ping(s);
	}
}

static void dual_begin_transfer(void) {
	if (!b_load_for(ds.n) || !dual_capture()) {
		dual_send_all(DPK_BYE, NULL, 0, NULL, 0);
		dual_stop("Cable Link: not enough memory for the other GBAs");
		return;
	}
	ds.state = DS_TRANSFER;
	dual_send_blob();
}

/* Setup steps, between frames. */
static void dual_setup_step(void) {
	const u64 now = time_usec();
	unsigned s;

	if (ds.state == DS_HELLO) {
		/* Client. The host may not be listening yet: say it again until it answers. */
		if (!ds.hello_sent || now - ds.t_hello > DUAL_HELLO_EVERY) {
			dual_send_hello(0);
		}
		if (now - ds.t_state > DUAL_HELLO_US) {
			dual_give_up("Cable Link: the host does not have this core");
		}
		return;
	}

	if (ds.state == DS_WAIT) {
		if (!ds.is_host) {
			return; /* Waiting for START (the game goes on) */
		}
		for (s = 0; s < ds.ncand; s++) {
			if (!ds.cand[s].hello && now - ds.cand[s].t_join > DUAL_HELLO_US) {
				dual_give_up("Cable Link: a console does not have this core, playing alone");
				return;
			}
		}
		if (ds.ncand + 1 == ds.players) {
			for (s = 0; s < ds.ncand && ds.cand[s].hello; s++);
			if (s == ds.ncand) {
				dual_host_start();
			}
		}
		return;
	}

	if (ds.state == DS_PING) {
		if (ds.is_host) {
			bool done = true;
			for (s = 1; s < ds.n; s++) {
				if (ds.rtt_n[s] < DUAL_PINGS) {
					done = false;
					if (!ds.ping_out[s] || now - ds.t_ping[s] > DUAL_PONG_US) {
						dual_send_ping(s);
					}
				}
			}
			if (done || now - ds.t_state > DUAL_PING_US) {
				struct timespec ts;
				u32 w[2];
				for (s = 1; s < ds.n; s++) {
					ds.ow_us[s] = dual_rtt_min(s) / 2;
				}
				ds.delay = ds.n > 1 ? dual_pick_delay() : 4;
				/* Every console's RTC starts from the host's clock */
				clock_gettime(CLOCK_REALTIME, &ts);
				ds.rtc_epoch_ms = (s64) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
				w[0] = (u32) ds.rtc_epoch_ms;
				w[1] = (u32) ((u64) ds.rtc_epoch_ms >> 32);
				dual_send_all(DPK_GO, w, 2, NULL, 0);
				dual_begin_transfer();
			}
		} else if (ds.go) {
			dual_begin_transfer();
		}
	}

	if (ds.state == DS_TRANSFER && dual_have_states()) {
		if (!dual_load_machines()) {
			dual_send_all(DPK_BYE, NULL, 0, NULL, 0);
			dual_stop("Cable Link: could not start the other GBAs");
			return;
		}
		if (ds.is_host) {
			u32 w[1 + NP];
			w[0] = ds.delay;
			for (s = 0; s < NP; s++) {
				w[1 + s] = ds.ow_us[s];
			}
			dual_send_all(DPK_READY, w, 1 + NP, NULL, 0);
		}
		ds.state = DS_READY;
	}

	if (ds.state == DS_READY && (ds.is_host || ds.peer_ready)) {
		dual_start_running();
	}

	if (ds.state >= DS_PING && ds.state < DS_RUN && now - ds.t_state > DUAL_SETUP_US) {
		dual_send_all(DPK_BYE, NULL, 0, NULL, 0);
		dual_stop("Cable Link: another console did not answer");
	}
}

/* ---- frontend interface (libretro.c) ---------------------------------- */

void dual_set_frontend(retro_environment_t env, void (*message)(const char* text, unsigned ms),
                       void (*prepare)(void)) {
	ds.environ_cb = env;
	ds.message = message;
	ds.prepare = prepare;
}

void dual_game_loaded(struct mCore* core, const void* rom, size_t romSize, const char* path) {
	ds.own = core;
	ds.rom = rom;
	ds.rom_size = romSize;
	ds.rom_path[0] = '\0';
	if (path) {
		strncpy(ds.rom_path, path, sizeof(ds.rom_path) - 1);
		ds.rom_path[sizeof(ds.rom_path) - 1] = '\0';
	}
	if (ds.rom_fd >= 0) {
		close(ds.rom_fd);
		ds.rom_fd = -1;
	}
}

void dual_set_players(unsigned players) {
	ds.cfg_players = MAX(2, MIN(players, NP));
	if (ds.state == DS_OFF && !ds.gave_up) {
		ds.players = ds.cfg_players;
	}
}

void dual_set_net(retro_netpacket_send_t send, retro_netpacket_poll_receive_t poll) {
	ds.send_fn = send;
	ds.poll_fn = poll;
}

/* A netplay session started. The host waits for clients. The slow part of
 * the setup (the other GBAs) is done now: the host does it while it waits,
 * a client while its HELLO is on the way. */
void dual_session_start(unsigned client_id) {
	unsigned k;
	if (!ds.own || ds.own->platform(ds.own) != mPLATFORM_GBA) {
		return;
	}
	dual_stop(NULL);
	dual_session_reset();
	ds.players = MAX(2, MIN(ds.cfg_players, NP));
	ds.is_host = (client_id == 0);
	ds.my_id = client_id;
	ds.cfg = dual_machine_cfg();
	if (ds.is_host) {
		ds.state = DS_WAIT;
	} else {
		ds.state = DS_HELLO;
		dual_send_hello(0);
	}
	/* Only the GBAs the option asks for: free the others, the memory of
	 * the console is small. */
	for (k = ds.players - 1; k < DUAL_HIDDEN; k++) {
		b_unload(k);
	}
	b_load_for(ds.players); /* (if it fails, the setup says so) */
	ds.t_state = time_usec();
}

bool dual_peer_connected(unsigned client_id) {
	const u64 bit = client_id < 64 ? 1ull << client_id : 0;
	if (!ds.is_host || ds.gave_up || ds.state == DS_OFF) {
		return true;
	}
	if (ds.state != DS_WAIT || ds.ncand + 1 >= ds.players) {
		return false;
	}
	ds.cand[ds.ncand].id = (int) client_id;
	ds.cand[ds.ncand].hello = (ds.early_hello & bit) != 0;
	ds.cand[ds.ncand].t_join = time_usec();
	ds.early_hello &= ~bit;
	ds.ncand++;
	if (ds.cand[ds.ncand - 1].hello) {
		dual_cand_ready();
	}
	return true;
}

void dual_peer_disconnected(unsigned client_id) {
	unsigned s;
	if (!ds.is_host || ds.state == DS_OFF) {
		return;
	}
	if (ds.state == DS_WAIT) {
		if (client_id < 64) {
			ds.early_hello &= ~(1ull << client_id);
		}
		for (s = 0; s < ds.ncand; s++) {
			if (ds.cand[s].id == (int) client_id) {
				u32 w[2];
				memmove(&ds.cand[s], &ds.cand[s + 1], (ds.ncand - s - 1) * sizeof(ds.cand[0]));
				ds.ncand--;
				w[0] = ds.ncand + 1;
				w[1] = ds.players;
				dual_send_all(DPK_WAITING, w, 2, NULL, 0);
				dual_waiting_msg(ds.ncand + 1);
				break;
			}
		}
		return;
	}
	for (s = 1; s < ds.n; s++) {
		if (ds.slot_id[s] == (int) client_id) {
			dual_send_all(DPK_BYE, NULL, 0, NULL, 0);
			dual_stop(ds.state == DS_RUN ? "Cable Link: a console left" : NULL);
			return;
		}
	}
}

void dual_session_stop(void) {
	dual_stop(NULL);
	ds.state = DS_OFF;
	ds.send_fn = NULL;
	ds.poll_fn = NULL;
}

/* Whether the cable is setting up (true: do not run the game now). Rather
 * than one step per frame, go on for most of the frame, as soon as the
 * others answer. (While the host waits for the players, the games go on.) */
bool dual_setting_up(void) {
	const u64 t0 = time_usec();
	if (ds.state == DS_WAIT || ds.state == DS_HELLO) {
		dual_setup_step();
		if (ds.state == DS_WAIT || ds.state == DS_HELLO || ds.state == DS_OFF) {
			return false;
		}
	}
	while (ds.state != DS_OFF && ds.state != DS_RUN) {
		dual_setup_step();
		if (ds.state == DS_OFF || ds.state == DS_RUN || time_usec() - t0 > DUAL_SLICE_US) {
			break;
		}
		net_poll();
		usleep(250);
	}
	return ds.state != DS_OFF && ds.state != DS_RUN;
}

bool dual_running(void) {
	return ds.state == DS_RUN;
}

bool dual_linked(void) {
	return ds.state >= DS_PING;
}

void dual_request_reset(void) {
	if (ds.state == DS_RUN) {
		ds.reset_wanted = true;
	}
}

static s32 dual_lead(unsigned s, u64 now) {
	s64 best;
	u32 i;
	if (!ds.peer_pos_n[s]) {
		return 0;
	}
	best = ds.peer_pos[s][0];
	for (i = 1; i < MIN(ds.peer_pos_n[s], DUAL_SYNC_WIN); i++) {
		best = MAX(best, ds.peer_pos[s][i]);
	}
	return (s32) ((s64) ds.frame * DUAL_FRAME_US - (s64) now - best);
}

/* Every GBA starts over, with the cable plugged again (as at the start). */
static void dual_reset_all(void) {
	unsigned s;
	for (s = 0; s < ds.n; s++) {
		struct mCore* c = slot_core(s);
		c->reset(c);
	}
	bus_reset(ds.n);
	for (s = 0; s < ds.n; s++) {
		port_begin(s, slot_core(s));
	}
}

int dual_frame_begin(u32 local_keys) {
	const u32 f = ds.frame;
	const u32 fi = f % DUAL_RING;
	const u64 t0 = time_usec();
	s32 lead[NP] = { 0 }, ahead = 0;
	bool full = true;
	unsigned s;

	/* Ahead of another console: stay a frame where we are. Each client
	 * keeps with the host, and the host with each client. */
	for (s = 0; s < ds.n; s++) {
		if (s != ds.my_slot) {
			lead[s] = dual_lead(s, t0);
			if (ds.my_slot && s) {
				continue;
			}
			ahead = MAX(ahead, (lead[s] - ds.peer_lead[s]) / 2);
			if (ds.peer_pos_n[s] < DUAL_SYNC_WIN) {
				full = false;
			}
		}
	}
	if (f >= ds.sync_next && full && ahead > DUAL_SYNC_US) {
		ds.sync_next = f + DUAL_SYNC_GAP;
		return DUAL_FRAME_HOLD;
	}

	/* Someone waited for our buttons: they go out earlier. Back down once
	 * all is calm. */
	if (ds.send_delay > ds.delay && t0 - ds.t_peer_stall > ds.calm_us) {
		ds.send_delay--;
		ds.t_peer_stall = ds.t_lowered = t0;
	}

	/* Ours for frame f + delay */
	while (ds.sent_upto <= f + ds.send_delay) {
		u32 w[3 + 2 * NP];
		w[0] = ds.sent_upto;
		w[1] = (local_keys & 0x3FF) | (ds.reset_wanted ? DUAL_KEY_RESET : 0);
		ds.reset_wanted = false;
		w[2] = f;
		for (s = 0; s < NP; s++) {
			w[3 + s] = (u32) lead[s];
			w[3 + NP + s] = ds.stalls[s];
		}
		ds.own_keys[ds.sent_upto % DUAL_RING] = w[1];
		dual_send_all(DPK_INPUT, w, 3 + 2 * NP, NULL, 0);
		ds.sent_upto++;
	}

	/* Everybody else's for frame f */
	while (1) {
		const u64 now = time_usec();
		bool missing = false;
		for (s = 0; s < ds.n; s++) {
			if (s != ds.my_slot && ds.frame_ok[s][fi] != f + 1) {
				missing = true;
			}
		}
		if (!missing) {
			break;
		}
		if (ds.state != DS_RUN) {
			return DUAL_FRAME_WAIT;
		}
		for (s = 0; s < ds.n; s++) {
			if (s != ds.my_slot && now - ds.t_heard[s] > DUAL_PEER_US) {
				dual_send_all(DPK_BYE, NULL, 0, NULL, 0);
				dual_stop("Cable Link: a console stopped answering");
				return DUAL_FRAME_WAIT;
			}
		}
		if (now - t0 > DUAL_STALL_US) {
			for (s = 0; s < ds.n; s++) {
				if (s != ds.my_slot && ds.frame_ok[s][fi] != f + 1) {
					dual_count_stall(s, f);
				}
			}
			return DUAL_FRAME_WAIT;
		}
		net_poll();
		usleep(250);
	}
	if (ds.state != DS_RUN) {
		return DUAL_FRAME_WAIT;
	}

	/* A player pressed reset: every console restarts every game in this
	 * very frame, with the cable plugged again. */
	{
		u32 any = ds.own_keys[fi];
		for (s = 0; s < ds.n; s++) {
			if (s != ds.my_slot) {
				any |= ds.keys[s][fi];
			}
		}
		if (any & DUAL_KEY_RESET) {
			dual_reset_all();
		}
	}

	for (s = 0; s < ds.n; s++) {
		struct mCore* c = slot_core(s);
		u32 k = s == ds.my_slot ? ds.own_keys[fi] : ds.keys[s][fi];
#ifdef DUAL_DEBUG
		/* Test of the agreement check: a hidden GBA gets other buttons once. */
		if (getenv("DUAL_FAULT") && f == 300 && s != ds.my_slot) {
			k ^= 1;
		}
#endif
		c->setKeys(c, k & 0x3FF);
	}
	return DUAL_FRAME_RUN;
}

static void dual_check_hash(u32 f) {
	const u32 i = (f / DUAL_HASH_EVERY) % DUAL_HASH_SLOTS;
	const u32* mine = ds.my_hash[i];
	unsigned s, g;
	if (mine[0] != f + 1) {
		return;
	}
	for (s = 0; s < ds.n; s++) {
		const u32* theirs = ds.peer_hash[s][i];
		if (s == ds.my_slot || theirs[0] != f + 1) {
			continue;
		}
		for (g = 0; g < ds.n; g++) {
			if (mine[1 + g] != theirs[1 + g]) {
				dual_send_all(DPK_BYE, NULL, 0, NULL, 0);
				dual_stop("Cable Link: the consoles went out of sync");
				return;
			}
		}
	}
}

/* A frame of every GBA: ours here, the others on their threads. */
void dual_frame_run(void) {
	unsigned k, s;
	const u32 f = ds.frame;
	for (k = 0; k + 1 < ds.n; k++) {
		sem_post(&ds.hid[k].start);
	}
	ds.own->runFrame(ds.own);
	port_frame_end(ds.my_slot);
	for (k = 0; k + 1 < ds.n; k++) {
		sem_wait(&ds.hid[k].done);
	}
	ds.frame++;

	if (f % DUAL_HASH_EVERY == 0) {
		u32 w[1 + NP], *mine = ds.my_hash[(f / DUAL_HASH_EVERY) % DUAL_HASH_SLOTS];
		w[0] = f;
		mine[0] = f + 1;
		for (s = 0; s < NP; s++) {
			w[1 + s] = s < ds.n ? hash_slot(s) : 0;
			mine[1 + s] = w[1 + s];
		}
		dual_send_all(DPK_HASH, w, 1 + NP, NULL, 0);
		dual_check_hash(f);
	}
}

/* Returns true if the packet was ours. */
bool dual_net_receive(const void* buf, size_t len, uint16_t client_id) {
	const u32* pkt = (const u32*) buf;
	const u64 now = time_usec();
	u32 type, from;
	unsigned s;
	if (len < 8 || netorder32(pkt[0]) != DUAL_HEADER) {
		return false;
	}
	if (ds.state == DS_OFF) {
		return true;
	}

	type = netorder32(pkt[1]) & 0xFF;
	from = (netorder32(pkt[1]) >> 8) & 0xFF;
	if (type == DPK_START) {
		from = DUAL_NO_SLOT;
	}
	/* Packets with a slot: only from a console of this session. */
	if (from != DUAL_NO_SLOT) {
		if (!ds.n || from >= ds.n || from == ds.my_slot || ds.slot_id[from] != (int) client_id) {
			return true;
		}
		ds.t_heard[from] = now;
	}

	switch (type) {
	case DPK_HELLO:
		if (len < 8 + 6 * 4) {
			break;
		} else {
			u32 w[6];
			for (s = 0; s < 6; s++) {
				w[s] = netorder32(pkt[2 + s]);
			}
			if (ds.is_host && ds.state == DS_WAIT) {
				for (s = 0; s < ds.ncand && ds.cand[s].id != (int) client_id; s++);
				if (!dual_hello_matches(w)) {
					dual_give_up(w[4] != ds.players ? DUAL_MSG_PLAYERS : DUAL_MSG_DIFFER);
					break;
				}
				/* Answer right away, even if the frontend has not told us
				 * about it yet: otherwise it would think we do not have this core. */
				dual_send_hello(client_id);
				if (s == ds.ncand) {
					if (client_id < 64) {
						ds.early_hello |= 1ull << client_id;
					}
					break;
				}
				if (!ds.cand[s].hello) {
					ds.cand[s].hello = true;
					dual_cand_ready();
				}
			} else if (!ds.is_host && ds.state == DS_HELLO && client_id == 0) {
				ds.hello_rcvd = true;
				if (!dual_hello_matches(w)) {
					dual_give_up(w[4] != ds.players ? DUAL_MSG_PLAYERS : DUAL_MSG_DIFFER);
				} else {
					ds.state = DS_WAIT;
				}
			}
		}
		break;

	case DPK_WAITING:
		if (len >= 16 && !ds.is_host && client_id == 0 && (ds.state == DS_WAIT || ds.state == DS_HELLO)) {
			const u32 have = netorder32(pkt[2]);
			if (have < ds.players) {
				dual_waiting_msg(have);
			}
		}
		break;

	case DPK_START:
		if (len >= 8 + (1 + NP) * 4 && !ds.is_host && client_id == 0 &&
		    (ds.state == DS_WAIT || ds.state == DS_HELLO)) {
			const u32 n = netorder32(pkt[2]);
			if (n < 2 || n > NP || n != ds.players) {
				dual_give_up(DUAL_MSG_PLAYERS);
				break;
			}
			ds.n = n;
			ds.my_slot = DUAL_NO_SLOT;
			for (s = 0; s < NP; s++) {
				ds.slot_id[s] = (int) netorder32(pkt[3 + s]);
				if (s < n && ds.slot_id[s] == (int) ds.my_id) {
					ds.my_slot = s;
				}
			}
			if (ds.my_slot == DUAL_NO_SLOT || !ds.my_slot) {
				ds.n = 0;
				dual_give_up(NULL);
				break;
			}
			for (s = 0; s < NP; s++) {
				ds.t_heard[s] = now;
			}
			ds.state = DS_PING;
			ds.t_state = now;
		}
		break;

	case DPK_PING:
		if (len >= 16) {
			u32 w[2] = { netorder32(pkt[2]), netorder32(pkt[3]) };
			dual_send(client_id, DPK_PONG, w, 2, NULL, 0);
		}
		break;

	case DPK_PONG:
		if (len >= 16 && ds.is_host && from != DUAL_NO_SLOT) {
			const u64 t = netorder32(pkt[2]) | ((u64) netorder32(pkt[3]) << 32);
			ds.rtt[from][ds.rtt_n[from] % DUAL_PINGS] = (u32) (now - t);
			ds.rtt_n[from]++;
			ds.ping_out[from] = false;
		}
		break;

	case DPK_GO:
		if (len >= 16 && !ds.is_host && from == 0) {
			ds.rtc_epoch_ms = (s64) (netorder32(pkt[2]) | ((u64) netorder32(pkt[3]) << 32));
			ds.go = true;
		}
		break;

	case DPK_STATE:
		if (len > 16 && from != DUAL_NO_SLOT && ds.state >= DS_PING && ds.state <= DS_TRANSFER) {
			const u32 off = netorder32(pkt[2]);
			const u32 total = netorder32(pkt[3]);
			const u32 dlen = (u32) len - 16;
			if (total > DUAL_STATE_MAX) {
				break;
			}
			if (!ds.peer_pack[from] || ds.peer_pack_cap[from] < total) {
				free(ds.peer_pack[from]);
				ds.peer_pack[from] = malloc(total);
				ds.peer_pack_cap[from] = ds.peer_pack[from] ? total : 0;
				ds.peer_rcvd[from] = 0;
			}
			/* (Checked so that no sum can wrap around.) */
			if (ds.peer_pack[from] && off <= total && dlen <= total - off) {
				memcpy(&ds.peer_pack[from][off], &pkt[4], dlen);
				ds.peer_pack_len[from] = total;
				ds.peer_rcvd[from] += dlen;
			}
		}
		break;

	case DPK_READY:
		if (len >= 8 + (1 + NP) * 4 && !ds.is_host && from == 0) {
			ds.delay = MAX(1, MIN(netorder32(pkt[2]), 30));
			for (s = 0; s < NP; s++) {
				ds.ow_us[s] = netorder32(pkt[3 + s]);
			}
			ds.peer_ready = true;
		}
		break;

	case DPK_INPUT:
		if (len >= 8 + (3 + 2 * NP) * 4 && from != DUAL_NO_SLOT && ds.state >= DS_TRANSFER) {
			const u32 f = netorder32(pkt[2]);
			const u32 their_stalls = netorder32(pkt[5 + NP + ds.my_slot]);
			ds.keys[from][f % DUAL_RING] = netorder32(pkt[3]);
			ds.frame_ok[from][f % DUAL_RING] = f + 1;
			/* It was starting this frame one way ago. */
			ds.peer_pos[from][ds.peer_pos_n[from]++ % DUAL_SYNC_WIN] =
			    (s64) netorder32(pkt[4]) * DUAL_FRAME_US + dual_ow(ds.my_slot, from) - (s64) now;
			ds.peer_lead[from] = (s32) netorder32(pkt[5 + ds.my_slot]);
			/* It waited for our buttons: they go out a frame earlier. */
			if (their_stalls != ds.peer_stalls[from] && ds.state == DS_RUN) {
				ds.peer_stalls[from] = their_stalls;
				ds.send_delay = MIN(ds.send_delay + 1, MIN(ds.delay + DUAL_DELAY_MORE, DUAL_RING / 2));
				if (ds.t_lowered && now - ds.t_lowered < ds.calm_us) {
					ds.calm_us = MIN(ds.calm_us * 2, DUAL_DELAY_CALM_MAX);
				}
				ds.t_peer_stall = now;
			}
		}
		break;

	case DPK_HASH:
		if (len >= 8 + (1 + NP) * 4 && from != DUAL_NO_SLOT && ds.state == DS_RUN) {
			const u32 f = netorder32(pkt[2]);
			u32* theirs = ds.peer_hash[from][(f / DUAL_HASH_EVERY) % DUAL_HASH_SLOTS];
			theirs[0] = f + 1;
			for (s = 0; s < NP; s++) {
				theirs[1 + s] = netorder32(pkt[3 + s]);
			}
			dual_check_hash(f);
		}
		break;

	case DPK_GIVEUP:
		if (!ds.is_host && client_id == 0) {
			dual_stop(NULL);
			ds.gave_up = true;
			dual_msg(len >= 12 && netorder32(pkt[2]) ? DUAL_MSG_PLAYERS : DUAL_MSG_DIFFER);
		}
		break;

	case DPK_BYE:
		if (ds.is_host && ds.state == DS_WAIT && from == DUAL_NO_SLOT) {
			/* A client that cannot take part: nobody is linked. */
			dual_give_up(DUAL_MSG_DIFFER);
			break;
		}
		if (from != DUAL_NO_SLOT || client_id == 0) {
			dual_stop(ds.state == DS_RUN ? "Cable Link: a console left" : NULL);
		}
		break;
	}
	return true;
}

void dual_game_unloaded(void) {
	unsigned k;
	dual_stop(NULL);
	bus.abort = 1;
	for (k = 0; k < DUAL_HIDDEN; k++) {
		b_unload(k);
	}
	if (ds.rom_fd >= 0) {
		close(ds.rom_fd);
		ds.rom_fd = -1;
	}
	ds.own = NULL;
	ds.rom = NULL;
}

void dual_shutdown(void) {
	dual_game_unloaded();
}
