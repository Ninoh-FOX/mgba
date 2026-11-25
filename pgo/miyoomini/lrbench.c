// lrbench: run a libretro core headless for N frames and report emulation speed.
// usage: lrbench core.so rom [frames] [opt-file]
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "libretro.h"
#include <signal.h>
#include <ucontext.h>
#include <sys/time.h>

/* LR_PROF=file: sample the PC 1000x/s (CPU time) and dump raw addresses */
static uintptr_t prof_pc[200000];
static volatile int prof_n;
static void prof_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si;
    if (prof_n < 200000) prof_pc[prof_n++] = ((ucontext_t *)uc)->uc_mcontext.arm_pc;
}

static char opts[64][2][128];
static int nopts;
static uint32_t crc;
static unsigned long long nsamples;
static uint32_t acrc;
static FILE *wav;
static unsigned target_rate;

static void load_opts(const char *f)
{
    FILE *fp = fopen(f, "r");
    char line[512];
    if (!fp) return;
    while (fgets(line, sizeof line, fp) && nopts < 64) {
        char k[128], v[128];
        if (sscanf(line, " %127[^ =] = \"%127[^\"]\"", k, v) == 2) {
            strcpy(opts[nopts][0], k); strcpy(opts[nopts][1], v); nopts++;
        }
    }
    fclose(fp);
}

static bool env(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = getenv("LR_SYSDIR") ? getenv("LR_SYSDIR") : "/tmp"; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        for (int i = 0; i < nopts; i++)
            if (!strcmp(v->key, opts[i][0])) { v->value = opts[i][1]; return true; }
        v->value = NULL; return false;
    }
    case (81 | 0x10000): /* GET_TARGET_SAMPLE_RATE */
        if (!target_rate) return false;
        *(unsigned *)data = target_rate; return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = false; return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 0; return true;
    default: return false;
    }
}

static void video(const void *d, unsigned w, unsigned h, size_t pitch)
{
    // cheap checksum of one line so results can be compared between builds
    if (d) { const uint32_t *p = (const uint32_t *)((const char *)d + pitch * (h / 2));
        for (unsigned i = 0; i < w; i++) crc = crc * 31 + p[i]; }
}
static void audio(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch(const int16_t *d, size_t f)
{ if (wav) fwrite(d, 4, f, wav); nsamples += f; for (size_t i = 0; i < f * 2; i++) acrc = acrc * 31 + (uint16_t)d[i]; return f; }
static void poll(void) {}
/* LR_KEYS=1: press START at frames 0-5 and A at 120-125 of every 240 (reaches menus/gameplay) */
static int keys_on, cur_frame;
static int16_t input(unsigned p, unsigned d, unsigned i, unsigned id)
{
    (void)i;
    if (!keys_on || p != 0 || d != RETRO_DEVICE_JOYPAD) return 0;
    int f = cur_frame % 240;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        return (f < 6 ? 1 << RETRO_DEVICE_ID_JOYPAD_START : 0) | (f >= 120 && f < 126 ? 1 << RETRO_DEVICE_ID_JOYPAD_A : 0);
    if (id == RETRO_DEVICE_ID_JOYPAD_START) return f < 6;
    if (id == RETRO_DEVICE_ID_JOYPAD_A) return f >= 120 && f < 126;
    return 0;
}

static double now(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

#define SYM(n) n = dlsym(h, #n); if (!n) { fprintf(stderr, "missing %s\n", #n); return 1; }
int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s core rom [frames] [optfile]\n", argv[0]); return 1; }
    int frames = argc > 3 ? atoi(argv[3]) : 3600;
    if (argc > 4) load_opts(argv[4]);
    if (getenv("LR_RATE")) target_rate = atoi(getenv("LR_RATE"));
    keys_on = getenv("LR_KEYS") != NULL;
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    void (*retro_set_environment)(retro_environment_t), (*retro_set_video_refresh)(retro_video_refresh_t),
         (*retro_set_audio_sample)(retro_audio_sample_t), (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t),
         (*retro_set_input_poll)(retro_input_poll_t), (*retro_set_input_state)(retro_input_state_t),
         (*retro_init)(void), (*retro_run)(void), (*retro_deinit)(void);
    bool (*retro_load_game)(const struct retro_game_info *);
    SYM(retro_set_environment) SYM(retro_set_video_refresh) SYM(retro_set_audio_sample)
    SYM(retro_set_audio_sample_batch) SYM(retro_set_input_poll) SYM(retro_set_input_state)
    SYM(retro_init) SYM(retro_run) SYM(retro_deinit) SYM(retro_load_game)
    retro_set_environment(env); retro_set_video_refresh(video); retro_set_audio_sample(audio);
    retro_set_audio_sample_batch(audio_batch); retro_set_input_poll(poll); retro_set_input_state(input);
    retro_init();
    FILE *fp = fopen(argv[2], "rb");
    if (!fp) { perror(argv[2]); return 1; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    void *buf = malloc(sz); fread(buf, 1, sz, fp); fclose(fp);
    struct retro_game_info gi = { argv[2], buf, (size_t)sz, NULL };
    if (!retro_load_game(&gi)) { fprintf(stderr, "load failed\n"); return 1; }
    for (int i = 0; i < 60; i++) { cur_frame = i; retro_run(); } // warm-up / boot
    const char *pf = getenv("LR_PROF");
    if (pf) {
        struct sigaction sa; memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = prof_handler; sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGPROF, &sa, NULL);
        struct itimerval it = { { 0, 1000 }, { 0, 1000 } };
        setitimer(ITIMER_PROF, &it, NULL);
    }
    double worst = 0, t0 = now(), tp = t0;
    nsamples = 0;
    if (getenv("LR_PCM")) wav = fopen(getenv("LR_PCM"), "wb");
    for (int i = 0; i < frames; i++) {
        cur_frame = 60 + i;
        retro_run();
        double t = now(); if (t - tp > worst) worst = t - tp; tp = t;
    }
    double dt = now() - t0;
    printf("fps=%.1f avg=%.2f ms worst=%.2f ms audio=%.0f Hz crc=%08x acrc=%08x\n",
           frames / dt, dt * 1000 / frames, worst * 1000, nsamples * 59.73 / frames, crc, acrc);
    if (wav) fclose(wav);
    if (pf) {
        struct itimerval it = { { 0, 0 }, { 0, 0 } };
        setitimer(ITIMER_PROF, &it, NULL);
        Dl_info di; uintptr_t base = 0;
        if (dladdr((void *)retro_run, &di)) base = (uintptr_t)di.dli_fbase;
        FILE *o = fopen(pf, "w");
        for (int i = 0; i < prof_n; i++)
            fprintf(o, "%lx\n", (unsigned long)(prof_pc[i] - base));
        fclose(o);
    }
    return 0;
}
